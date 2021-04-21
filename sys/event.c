/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2017 - 2021 Google, Inc.
  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

  http://dokan-dev.github.io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "dokan.h"
#include "util/irp_buffer_helper.h"
#include "util/mountmgr.h"
#include "util/str.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanOplockComplete)
#endif

VOID DokanCreateIrpCancelRoutine(_Inout_ PDEVICE_OBJECT DeviceObject,
                                 _Inout_ _IRQL_uses_cancel_ PIRP Irp) {
  // Cancellation of a Create is handled like a timeout. The whole effect of
  // this routine is just to set the IRP entry's tick count so as to trigger a
  // timeout, and then force the timeout thread to wake up. This simplifies the
  // complex cleanup that needs to be done and can't be done on the unknown
  // context where the cancel routine runs. For other types of IRPs, the cancel
  // routine actually does cancelation/cleanup.
  IoReleaseCancelSpinLock(Irp->CancelIrql);
  PIRP_ENTRY irpEntry =
      Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY];
  if (irpEntry != NULL) {
    Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = NULL;
    InterlockedAnd64(&irpEntry->TickCount.QuadPart, 0);
    irpEntry->AsyncStatus = STATUS_CANCELLED;
    irpEntry->RequestContext.ForcedCanceled = TRUE;
    PDokanVCB vcb = DeviceObject->DeviceExtension;
    PDokanDCB dcb = vcb->Dcb;
    KeSetEvent(&dcb->ForceTimeoutEvent, 0, FALSE);
  }
}

// Only used for non-Create IRPs.
VOID DokanIrpCancelRoutine(_Inout_ PDEVICE_OBJECT DeviceObject,
                           _Inout_ _IRQL_uses_cancel_ PIRP Irp) {
  KIRQL oldIrql;
  PIRP_ENTRY irpEntry;
  ULONG serialNumber = 0;
  REQUEST_CONTEXT requestContext;
  NTSTATUS status;

  // Release the cancel spinlock
  IoReleaseCancelSpinLock(Irp->CancelIrql);

  status = DokanBuildRequestContext(DeviceObject, Irp, &requestContext);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("Failed to build request context for IRP=%p Status=%s", Irp,
               DokanGetNTSTATUSStr(status));
    return;
  }

  irpEntry = Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY];

  if (irpEntry != NULL) {
    PKSPIN_LOCK lock = &irpEntry->IrpList->ListLock;

    // Acquire the queue spinlock
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    KeAcquireSpinLock(lock, &oldIrql);

    DOKAN_LOG_FINE_IRP((&requestContext), "Start cancel");

    serialNumber = irpEntry->SerialNumber;

    RemoveEntryList(&irpEntry->ListEntry);
    InitializeListHead(&irpEntry->ListEntry);

    // If Write is canceld before completion and buffer that saves writing
    // content is not freed, free it here
    if (requestContext.IrpSp->MajorFunction == IRP_MJ_WRITE) {
      PVOID eventContext =
          Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT];
      if (eventContext != NULL) {
        DokanFreeEventContext(eventContext);
      }
      Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = NULL;
    }

    if (IsListEmpty(&irpEntry->IrpList->ListHead)) {
      KeClearEvent(&irpEntry->IrpList->NotEmpty);
    }

    irpEntry->RequestContext.Irp = NULL;

    if (irpEntry->CancelRoutineFreeMemory == FALSE) {
      InitializeListHead(&irpEntry->ListEntry);
    } else {
      DokanFreeIrpEntry(irpEntry);
      irpEntry = NULL;
    }

    Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;

    KeReleaseSpinLock(lock, oldIrql);
    DOKAN_LOG_FINE_IRP((&requestContext), "Canceled");
  }

  Irp->IoStatus.Information = 0;
  DokanCompleteIrpRequest(Irp, STATUS_CANCELLED);
}

VOID DokanOplockComplete(IN PVOID Context, IN PIRP Irp)
/*++
Routine Description:
This routine is called by the oplock package when an oplock break has
completed, allowing an Irp to resume execution.  If the status in
the Irp is STATUS_SUCCESS, then we queue the Irp to the Fsp queue.
Otherwise we complete the Irp with the status in the Irp.
Arguments:
Context - Pointer to the EventContext to be queued to the Fsp
Irp - I/O Request Packet.
Return Value:
None.
--*/
{
  PAGED_CODE();

  if (Irp == NULL) {
    DOKAN_LOG("NULL Irp received");
    return;
  }
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

  REQUEST_CONTEXT requestContext;
  NTSTATUS status =
      DokanBuildRequestContext(irpSp->DeviceObject, Irp, &requestContext);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("Failed to build request context for IRP=%p Status=%s", Irp,
               DokanGetNTSTATUSStr(status));
    return;
  }

  DOKAN_LOG_FINE_IRP((&requestContext), "Oplock break completed %s",
                     DokanGetNTSTATUSStr(Irp->IoStatus.Status));

  //
  //  Check on the return value in the Irp.
  //
  if (Irp->IoStatus.Status == STATUS_SUCCESS) {
    DokanRegisterPendingIrp(&requestContext, (PEVENT_CONTEXT)Context);
  } else {
    Irp->IoStatus.Information = 0;
    DokanCompleteIrpRequest(Irp, Irp->IoStatus.Status);
  }
}

// Routine for kernel oplock functions to invoke if they are going to wait for
// e.g. an oplock break acknowledgement and return STATUS_PENDING. This enables
// us to mark the IRP pending without there being a race condition where it
// gets acted on by the acknowledging thread before we do this.
// do the right thing here with conservative impact.
VOID DokanPrePostIrp(IN PVOID Context, IN PIRP Irp)
{
  UNREFERENCED_PARAMETER(Context);
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
  REQUEST_CONTEXT requestContext;
  DokanBuildRequestContext(irpSp->DeviceObject, Irp, &requestContext);
  DOKAN_LOG_FINE_IRP((&requestContext), "Mark Irp pending");
  IoMarkIrpPending(Irp);
}

NTSTATUS
RegisterPendingIrpMain(__in PREQUEST_CONTEXT RequestContext,
                       __in_opt PEVENT_CONTEXT EventContext,
                       __in PIRP_LIST IrpList, __in ULONG CheckMount,
                       __in NTSTATUS CurrentStatus) {
  PIRP_ENTRY irpEntry;
  KIRQL oldIrql;

  if (RequestContext->Vcb && CheckMount &&
      IsUnmountPendingVcb(RequestContext->Vcb)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Device is not mounted");
    return STATUS_NO_SUCH_DEVICE;
  }

  // Allocate a record and save all the event context.
  irpEntry = DokanAllocateIrpEntry();

  if (NULL == irpEntry) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Can't allocate IRP_ENTRY");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlZeroMemory(irpEntry, sizeof(IRP_ENTRY));

  InitializeListHead(&irpEntry->ListEntry);

  irpEntry->SerialNumber = 0;
  irpEntry->RequestContext = *RequestContext;
  irpEntry->IrpList = IrpList;
  irpEntry->AsyncStatus = CurrentStatus;

  // Update the irp timeout for the entry
  if (RequestContext->Vcb) {
    ExAcquireResourceExclusiveLite(&RequestContext->Dcb->Resource, TRUE);
    DokanUpdateTimeout(&irpEntry->TickCount, RequestContext->Dcb->IrpTimeout);
    ExReleaseResourceLite(&RequestContext->Dcb->Resource);
  } else {
    DokanUpdateTimeout(&irpEntry->TickCount, DOKAN_IRP_PENDING_TIMEOUT);
  }

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&IrpList->ListLock, &oldIrql);

  // Second unmount check with list lock acquired to ensure the device is
  // not being unmount. Once DokanEventRelease has set the unmount-pending flag
  // and canceled all IRPs, any IRP added to those list will never be completed
  // and canceled.
  if (RequestContext->Vcb && CheckMount &&
      IsUnmountPendingVcb(RequestContext->Vcb)) {
    KeReleaseSpinLock(&IrpList->ListLock, oldIrql);
    DokanFreeIrpEntry(irpEntry);
    return STATUS_NO_SUCH_DEVICE;
  }

  if (EventContext) {
    EventContext->SerialNumber =
        InterlockedIncrement((LONG*)&RequestContext->Dcb->SerialNumber);
    irpEntry->SerialNumber = EventContext->SerialNumber;
  }
  if (RequestContext->IrpSp->MajorFunction == IRP_MJ_CREATE) {
    IoSetCancelRoutine(RequestContext->Irp, DokanCreateIrpCancelRoutine);
  } else {
    IoSetCancelRoutine(RequestContext->Irp, DokanIrpCancelRoutine);
  }

  if (RequestContext->Irp->Cancel) {
    if (IoSetCancelRoutine(RequestContext->Irp, NULL) != NULL) {
      KeReleaseSpinLock(&IrpList->ListLock, oldIrql);
      DokanFreeIrpEntry(irpEntry);
      return STATUS_CANCELLED;
    }
  }

  IoMarkIrpPending(RequestContext->Irp);

  InsertTailList(&IrpList->ListHead, &irpEntry->ListEntry);

  irpEntry->CancelRoutineFreeMemory = FALSE;

  // save the pointer in order to be accessed by cancel routine
  RequestContext->Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] =
      irpEntry;

  KeSetEvent(&IrpList->NotEmpty, IO_NO_INCREMENT, FALSE);

  KeReleaseSpinLock(&IrpList->ListLock, oldIrql);

  return STATUS_PENDING;
}

NTSTATUS
DokanRegisterPendingIrp(__in PREQUEST_CONTEXT RequestContext,
                        __in PEVENT_CONTEXT EventContext) {
  NTSTATUS status;
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject, 0);

  DOKAN_LOG_FINE_IRP(RequestContext, "Register the IRP pending");

  if (!RequestContext->Vcb) {
    DOKAN_LOG_FINE_IRP(RequestContext, "IdentifierType is not VCB");
    return STATUS_INVALID_PARAMETER;
  }

  // We check if we will have the space to sent the event before registering it
  // to the pending IRPs. Write is an exception as it has a special workflow for
  // large buffer that will request userland to allocate a specific buffer size
  // that match it.
  if (RequestContext->IrpSp->MajorFunction != IRP_MJ_WRITE &&
      EventContext->Length > EVENT_CONTEXT_MAX_SIZE) {
    InterlockedIncrement64((LONG64*)&RequestContext->Vcb->VolumeMetrics
                               .LargeIRPRegistrationCanceled);
    status = DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                           L"Received a too large buffer to handle for Major "
                           L"IRP %xh, canceling it.",
                           RequestContext->IrpSp->MajorFunction);
  } else {
    status = RegisterPendingIrpMain(RequestContext, EventContext,
                                    &RequestContext->Dcb->PendingIrp, TRUE,
                                    /*CurrentStatus=*/STATUS_SUCCESS);
  }

  if (status == STATUS_PENDING) {
    DokanEventNotification(&RequestContext->Dcb->NotifyEvent, EventContext);
  } else {
    DokanFreeEventContext(EventContext);
  }

  DOKAN_LOG_FINE_IRP(RequestContext, "Pending Registration: %s", DokanGetNTSTATUSStr(status));
  return status;
}

VOID DokanRegisterPendingRetryIrp(__in PREQUEST_CONTEXT RequestContext) {
  if (!RequestContext->Vcb) {
    return;
  }
  if (RequestContext->IrpSp->MajorFunction == IRP_MJ_CREATE) {
    // You can't just re-dispatch a create, since the part before the pending
    // retry has side-effects. DokanDispatchCreate uses this flag to identify
    // retries.
    PDokanCCB ccb = RequestContext->IrpSp->FileObject->FsContext2;
    ASSERT(ccb != NULL);
    DokanCCBFlagsSetBit(ccb, DOKAN_RETRY_CREATE);
    OplockDebugRecordFlag(ccb->Fcb, DOKAN_OPLOCK_DEBUG_CREATE_RETRY_QUEUED);
  }
  RegisterPendingIrpMain(RequestContext, /*EventContext=*/NULL,
                         &RequestContext->Dcb->PendingRetryIrp,
                         /*CheckMount=*/TRUE,
                         /*CurrentStatus=*/STATUS_SUCCESS);
}

VOID DokanRegisterAsyncCreateFailure(__in PREQUEST_CONTEXT RequestContext,
                                     __in NTSTATUS Status) {
  if (!RequestContext->Vcb) {
    return;
  }
  RegisterPendingIrpMain(RequestContext, /*EventContext=*/NULL,
                         &RequestContext->Dcb->PendingIrp,
                         /*CheckMount=*/TRUE, Status);
  KeSetEvent(&RequestContext->Dcb->ForceTimeoutEvent, 0, FALSE);
}

NTSTATUS
DokanRegisterPendingIrpForEvent(__in PREQUEST_CONTEXT RequestContext) {
  // TODO(adrienj): Remove the check when moving to FSCTL only.
  if (RequestContext->Vcb == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  RequestContext->Vcb->HasEventWait = TRUE;

  return RegisterPendingIrpMain(RequestContext,
                                NULL,  // EventContext
                                &RequestContext->Dcb->PendingEvent,
                                TRUE,
                                /*CurrentStatus=*/STATUS_SUCCESS);
}

void DokanDispatchCompletion(__in PDEVICE_OBJECT DeviceObject,
                             __in PIRP_ENTRY irpEntry,
                             __in PEVENT_INFORMATION eventInfo) {
  DOKAN_INIT_LOGGER(logger, DeviceObject->DriverObject, 0);

  if (irpEntry->RequestContext.Irp == NULL) {
    // this IRP is already canceled
    ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
    return;
  }

  if (IoSetCancelRoutine(irpEntry->RequestContext.Irp, NULL) == NULL) {
    // Cancel routine will run as soon as we release the lock
    InitializeListHead(&irpEntry->ListEntry);
    irpEntry->CancelRoutineFreeMemory = TRUE;
    return;
  }

  // IrpEntry is saved here for CancelRoutine
  // Clear it to prevent to be completed by CancelRoutine twice
  irpEntry->RequestContext.Irp->Tail.Overlay
      .DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;

  DOKAN_LOG_BEGIN_MJ((&irpEntry->RequestContext));

  if (eventInfo->Status == STATUS_PENDING) {
    DokanLogError(&logger, /*Status=*/0,
                  L"DLL returned STATUS_PENDING for IRP of type %d. "
                  L"It should never return STATUS_PENDING for any IRP.",
                  irpEntry->RequestContext.IrpSp->MajorFunction);
  }

  ASSERT(!irpEntry->RequestContext.DoNotComplete);

  switch (irpEntry->RequestContext.IrpSp->MajorFunction) {
  case IRP_MJ_DIRECTORY_CONTROL:
    DokanCompleteDirectoryControl(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_READ:
    DokanCompleteRead(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_WRITE:
    DokanCompleteWrite(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_QUERY_INFORMATION:
    DokanCompleteQueryInformation(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_QUERY_VOLUME_INFORMATION:
    DokanCompleteQueryVolumeInformation(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_CREATE:
    DokanCompleteCreate(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_CLEANUP:
    DokanCompleteCleanup(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_LOCK_CONTROL:
    DokanCompleteLock(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_SET_INFORMATION:
    DokanCompleteSetInformation(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_FLUSH_BUFFERS:
    DokanCompleteFlush(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_QUERY_SECURITY:
    DokanCompleteQuerySecurity(&irpEntry->RequestContext, eventInfo);
    break;
  case IRP_MJ_SET_SECURITY:
    DokanCompleteSetSecurity(&irpEntry->RequestContext, eventInfo);
    break;
  }

  DOKAN_LOG_END_MJ((&irpEntry->RequestContext),
                   irpEntry->RequestContext.Irp->IoStatus.Status);
}

ULONG
GetEventInfoSize(__in ULONG MajorFunction, __in PEVENT_INFORMATION EventInfo) {
  if (MajorFunction == IRP_MJ_WRITE) {
    // For writes only, the reply is a fixed size and the BufferLength inside it
    // is the "bytes written" value as opposed to the reply size.
    return sizeof(EVENT_INFORMATION);
  }
  if (EventInfo->Status == STATUS_BUFFER_OVERFLOW) {
    // For buffer overflow replies, the BufferLength is the needed length and
    // not the used length. The caller needs to take precautions in case the
    // used length is a value not specified in the struct.
    return sizeof(EVENT_INFORMATION);
  }
  return max(sizeof(EVENT_INFORMATION),
             sizeof(EVENT_INFORMATION) - sizeof(EventInfo->Buffer)
                 + EventInfo->BufferLength);
}

// When user-mode file system application returns EventInformation,
// search corresponding pending IRP and complete it
NTSTATUS
DokanCompleteIrp(__in PREQUEST_CONTEXT RequestContext) {
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject, 0);
  KIRQL oldIrql;
  NTSTATUS result = STATUS_SUCCESS;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry;
  LIST_ENTRY completeList;
  ULONG offset = 0;
  ULONG eventInfoSize = 0;
  ULONG lastSerialNumber = 0;
  PEVENT_INFORMATION eventInfo = NULL;
  BOOLEAN badUsageByCaller = FALSE;
  ULONG bufferLength = 0;
  PCHAR buffer = NULL;

  bufferLength =
      RequestContext->IrpSp->Parameters.DeviceIoControl.InputBufferLength;
  // Dokan 1.x.x Library can send buffer under EVENT_INFO struct size:
  // - IRP_MJ_QUERY_SECURITY sending STATUS_BUFFER_OVERFLOW
  // - IRP_MJ_READ with negative read size
  // The behavior was fixed since but adding the next line would break
  // backward compatiblity.
  // TODO 2.x.x - use GET_IRP_BUFFER_OR_RETURN(Irp, eventInfo);
  /*if (bufferLength < sizeof(EVENT_INFORMATION)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Wrong input buffer length");
    return STATUS_BUFFER_TOO_SMALL;
  }*/

  buffer = (PCHAR)RequestContext->Irp->AssociatedIrp.SystemBuffer;
  ASSERT(buffer != NULL);

  // TODO(adrienj): Remove the check when moving to FSCTL only.
  if (RequestContext->Vcb == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  if (IsUnmountPendingVcb(RequestContext->Vcb)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Volume is not mounted");
    return STATUS_NO_SUCH_DEVICE;
  }

  InitializeListHead(&completeList);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, &oldIrql);

  // search corresponding IRP through pending IRP list
  listHead = &RequestContext->Dcb->PendingIrp.ListHead;
  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {
    nextEntry = thisEntry->Flink;
    irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);
    eventInfo = (PEVENT_INFORMATION)(buffer + offset);
    if (eventInfo->SerialNumber < lastSerialNumber) {
      // This would be a coding error in the DLL.
      result = DokanLogError(&logger,
                             STATUS_INVALID_PARAMETER,
                             L"Reply batch not sorted by serial number.");
      badUsageByCaller = TRUE;
      break;
    }
    lastSerialNumber = eventInfo->SerialNumber;
    if (irpEntry->SerialNumber != eventInfo->SerialNumber) {
      continue;
    }
    RemoveEntryList(thisEntry);
    InsertTailList(&completeList, thisEntry);
    // We break until 2.x.x - See function head comment
__pragma(warning(push))
__pragma(warning(disable : 4127))
    if (1 == 1)
        break;
__pragma(warning(pop))
    offset += GetEventInfoSize(irpEntry->RequestContext.IrpSp->MajorFunction,
                               eventInfo);
    // Everything through offset - 1 must be readable by the completion function
    // that receives the EVENT_INFORMATION object.
    if (offset > bufferLength) {
      result = DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Full EVENT_INFORMATION size too large for passed-in buffer.");
      badUsageByCaller = TRUE;
      break;
    }
    // Batching is currently not allowed for buffer overflow replies, and the
    // checks below don't work for them. Essentially, if user mode populated a
    // partial object, it has no way of indicating the real size of that
    // EVENT_INFORMATION object, other than the size passed to DeviceIoControl
    // externally to the buffer.
    if (eventInfo->Status == STATUS_BUFFER_OVERFLOW) {
      break;
    }
    // Don't loop if batching is not enabled; there should only be one reply at
    // a time in that case.
    if (!RequestContext->Dcb->AllowIpcBatching) {
      if (offset < bufferLength) {
        result = DokanLogError(
            &logger,
            STATUS_INVALID_PARAMETER,
            L"Unexpected batch reply with batching flagged off.");
        badUsageByCaller = TRUE;
      }
      break;
    }
    // Don't loop if this is the last reply in the batch.
    if (offset == bufferLength) {
      break;
    }
    // Don't loop if the next thing in the batch is a fragment of an
    // EVENT_INFORMATION object.
    if (offset + sizeof(EVENT_INFORMATION) > bufferLength) {
      DokanLogInfo(&logger, L"Wrong input buffer length.");
      badUsageByCaller = TRUE;
      break;
    }
  }
  KeReleaseSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, oldIrql);
  offset = 0;
  eventInfo = NULL;
  if (IsListEmpty(&completeList)) {
    DokanLogInfo(&logger, L"Warning: no matching IRPs found for reply.");
  }
  while (!IsListEmpty(&completeList)) {
    listHead = RemoveHeadList(&completeList);
    if (IsUnmountPendingVcb(RequestContext->Vcb)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Volume is not mounted second check");
      return STATUS_NO_SUCH_DEVICE;
    }
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    if (offset >= bufferLength) {
      DokanLogInfo(&logger, L"Unexpected end of event info list.");
      irpEntry->RequestContext.Irp->IoStatus.Information = 0;
      DokanCompleteIrpRequest(irpEntry->RequestContext.Irp, STATUS_CANCELLED);
    } else {
      eventInfo = (PEVENT_INFORMATION)(buffer + offset);
      eventInfoSize = GetEventInfoSize(
          irpEntry->RequestContext.IrpSp->MajorFunction,
                                       eventInfo);
      DokanDispatchCompletion(RequestContext->DeviceObject, irpEntry, eventInfo);
      offset += eventInfoSize;
    }
    DokanFreeIrpEntry(irpEntry);
    irpEntry = NULL;
  }
  if (badUsageByCaller) {
    // This flag should only be set if there is a coding error in the DLL.
    DokanLogInfo(
        &logger,
        L"Unmounting to avoid hanging requests due to incorrect usage.");
    DokanUnmount(RequestContext, RequestContext->Dcb);
  }
  return result;
}

VOID RemoveSessionDevices(__in PREQUEST_CONTEXT RequestContext,
                          __in ULONG sessionId) {
  if (sessionId == -1) {
    return;
  }

  PDEVICE_ENTRY foundEntry;

  BOOLEAN isDone = FALSE;
  do {
    foundEntry =
        FindDeviceForDeleteBySessionId(RequestContext->DokanGlobal, sessionId);
    if (foundEntry != NULL) {
      DeleteMountPointSymbolicLink(&foundEntry->MountPoint);
      foundEntry->SessionId = (ULONG)-1;
      foundEntry->MountPoint.Buffer = NULL;
    } else {
      isDone = TRUE;
    }
  } while (!isDone);

}

// start event dispatching
NTSTATUS
DokanEventStart(__in PREQUEST_CONTEXT RequestContext) {
  ULONG outBufferLen;
  PEVENT_START eventStart = NULL;
  PEVENT_DRIVER_INFO driverInfo = NULL;
  PDokanDCB dcb = NULL;
  NTSTATUS status;
  DEVICE_TYPE deviceType;
  ULONG deviceCharacteristics = 0;
  WCHAR *baseGuidString;
  GUID baseGuid = DOKAN_BASE_GUID;
  UNICODE_STRING unicodeGuid;
  ULONG deviceNamePos;
  BOOLEAN useMountManager = FALSE;
  BOOLEAN mountGlobally = TRUE;
  BOOLEAN fileLockUserMode = FALSE;
  BOOLEAN fcbGcEnabled = FALSE;
  ULONG sessionId = (ULONG)-1;
  BOOLEAN startFailure = FALSE;
  BOOLEAN isMountPointDriveLetter = FALSE;

  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject, 0);

  DokanLogInfo(&logger, L"Entered event start.");
  DOKAN_LOG_FINE_IRP(RequestContext, "Event start");

  // We just use eventStart variable for his type size calculation here
  GET_IRP_BUFFER(RequestContext->Irp, eventStart);

  outBufferLen =
      RequestContext->IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
  if (outBufferLen != sizeof(EVENT_DRIVER_INFO) || !eventStart) {
    return DokanLogError(
        &logger, STATUS_INSUFFICIENT_RESOURCES,
        L"Buffer IN/OUT received do not match the expected size.");
  }

  eventStart = DokanAlloc(sizeof(EVENT_START));
  baseGuidString = DokanAllocZero(64 * sizeof(WCHAR));
  if (eventStart == NULL || baseGuidString == NULL) {
    if (eventStart)
      ExFreePool(eventStart);
    if (baseGuidString)
      ExFreePool(baseGuidString);
    return DokanLogError(&logger, STATUS_INSUFFICIENT_RESOURCES,
                         L"Failed to allocate buffers in event start.");
  }

  RtlCopyMemory(eventStart, RequestContext->Irp->AssociatedIrp.SystemBuffer,
                sizeof(EVENT_START));
  if (eventStart->UserVersion != DOKAN_DRIVER_VERSION) {
    DokanLogInfo(&logger, L"Driver version check in event start failed.");
    startFailure = TRUE;
  }

  if (DokanSearchStringChar(eventStart->MountPoint,
                        sizeof(eventStart->MountPoint), '\0') == -1 ||
      DokanSearchStringChar(eventStart->UNCName,
                        sizeof(eventStart->UNCName), '\0') == -1) {
    DokanLogInfo(&logger, L"MountPoint / UNCName provided are not null "
                          L"terminated in event start.");
    startFailure = TRUE;
  }

  driverInfo = RequestContext->Irp->AssociatedIrp.SystemBuffer;
  if (startFailure) {
    driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;
    driverInfo->Status = DOKAN_START_FAILED;
    RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
    RequestContext->Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return STATUS_SUCCESS;
  }

  switch (eventStart->DeviceType) {
  case DOKAN_DISK_FILE_SYSTEM:
    deviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
    break;
  case DOKAN_NETWORK_FILE_SYSTEM:
    deviceType = FILE_DEVICE_NETWORK_FILE_SYSTEM;
    deviceCharacteristics |= FILE_REMOTE_DEVICE;
    break;
  default:
    DOKAN_LOG_FINE_IRP(RequestContext, "Unknown device type: %d", eventStart->DeviceType);
    deviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
  }

  if (eventStart->Flags & DOKAN_EVENT_REMOVABLE) {
    DOKAN_LOG_FINE_IRP(RequestContext, "DeviceCharacteristics |= FILE_REMOVABLE_MEDIA");
    deviceCharacteristics |= FILE_REMOVABLE_MEDIA;
  }

  if (eventStart->Flags & DOKAN_EVENT_WRITE_PROTECT) {
    DOKAN_LOG_FINE_IRP(RequestContext, "DeviceCharacteristics |= FILE_READ_ONLY_DEVICE");
    deviceCharacteristics |= FILE_READ_ONLY_DEVICE;
  }

  if (eventStart->Flags & DOKAN_EVENT_MOUNT_MANAGER) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Using Mount Manager");
    useMountManager = TRUE;
  }

  if (eventStart->Flags & DOKAN_EVENT_CURRENT_SESSION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Mounting on current session only");
    mountGlobally = FALSE;
    sessionId = GetCurrentSessionId(RequestContext);
  }

  if (eventStart->Flags & DOKAN_EVENT_FILELOCK_USER_MODE) {
    DOKAN_LOG_FINE_IRP(RequestContext, "FileLock in User Mode");
    fileLockUserMode = TRUE;
  }

  if (eventStart->Flags & DOKAN_EVENT_ENABLE_FCB_GC) {
    DOKAN_LOG_FINE_IRP(RequestContext, "FCB GC enabled\n");
    fcbGcEnabled = TRUE;
  }

  if (eventStart->Flags & DOKAN_EVENT_CASE_SENSITIVE) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Case sensitive enabled\n");
  }

  if (eventStart->Flags & DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Network unmount enabled\n");
  }

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&RequestContext->DokanGlobal->Resource, TRUE);

  DOKAN_CONTROL dokanControl;
  RtlZeroMemory(&dokanControl, sizeof(DOKAN_CONTROL));
  RtlStringCchCopyW(dokanControl.MountPoint, MAXIMUM_FILENAME_LENGTH,
                    L"\\DosDevices\\");
  if (wcslen(eventStart->MountPoint) == 1) {
    dokanControl.MountPoint[12] = towupper(eventStart->MountPoint[0]);
    dokanControl.MountPoint[13] = L':';
    dokanControl.MountPoint[14] = L'\0';
  } else {
    RtlStringCchCatW(dokanControl.MountPoint, MAXIMUM_FILENAME_LENGTH,
                     eventStart->MountPoint);
  }
  dokanControl.SessionId = sessionId;
  dokanControl.MountOptions = eventStart->Flags;

  DOKAN_LOG_FINE_IRP(RequestContext, "Checking for MountPoint %ls", dokanControl.MountPoint);
  PMOUNT_ENTRY foundEntry =
      FindMountEntry(RequestContext->DokanGlobal, &dokanControl, FALSE);
  if (foundEntry != NULL) {
    DokanLogInfo(
        &logger,
        L"Mount point exists; DOKAN_EVENT_RESOLVE_MOUNT_CONFLICTS not set: %s",
        dokanControl.MountPoint);
    driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;
    driverInfo->Status = DOKAN_START_FAILED;
    RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
    RequestContext->Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
    ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
    KeLeaveCriticalRegion();
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return STATUS_SUCCESS;
  }

  baseGuid.Data2 =
      (USHORT)(RequestContext->DokanGlobal->MountId & 0xFFFF) ^ baseGuid.Data2;
  baseGuid.Data3 =
      (USHORT)(RequestContext->DokanGlobal->MountId >> 16) ^ baseGuid.Data3;

  status = RtlStringFromGUID(&baseGuid, &unicodeGuid);
  if (!NT_SUCCESS(status)) {
    ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
    KeLeaveCriticalRegion();
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return DokanLogError(&logger, status, L"Failed to convert GUID to string.");
  }

  RtlStringCchCopyW(baseGuidString, 64, unicodeGuid.Buffer);
  RtlFreeUnicodeString(&unicodeGuid);

  InterlockedIncrement((LONG*)&RequestContext->DokanGlobal->MountId);

  status = DokanCreateDiskDevice(
      RequestContext->DeviceObject->DriverObject,
      RequestContext->DokanGlobal->MountId, eventStart->MountPoint,
      eventStart->UNCName, sessionId, baseGuidString,
      RequestContext->DokanGlobal, deviceType, deviceCharacteristics,
      mountGlobally, useMountManager, &dcb);

  if (!NT_SUCCESS(status)) {
    ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
    KeLeaveCriticalRegion();
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return DokanLogError(&logger, status, L"Disk device creation failed.");
  }

  isMountPointDriveLetter = IsMountPointDriveLetter(dcb->MountPoint);

  dcb->FcbGarbageCollectionIntervalMs = fcbGcEnabled ? 2000 : 0;
  dcb->MountOptions = eventStart->Flags;
  dcb->DispatchDriverLogs =
      (eventStart->Flags & DOKAN_EVENT_DISPATCH_DRIVER_LOGS) != 0;

  if (dcb->DispatchDriverLogs) {
    IncrementVcbLogCacheCount();
  }

  dcb->FileLockInUserMode = fileLockUserMode;
  driverInfo->DeviceNumber = RequestContext->DokanGlobal->MountId;
  driverInfo->MountId = RequestContext->DokanGlobal->MountId;
  driverInfo->Status = DOKAN_MOUNTED;
  driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;

  // SymbolicName is
  // \\DosDevices\\Global\\Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
  // Finds the last '\' and copy into DeviceName.
  // DeviceName is \Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
  deviceNamePos = dcb->SymbolicLinkName->Length / sizeof(WCHAR) - 1;
  deviceNamePos = DokanSearchWcharinUnicodeStringWithUlong(
      dcb->SymbolicLinkName, L'\\', deviceNamePos, 0);

  RtlStringCchCopyW(driverInfo->DeviceName,
                    sizeof(driverInfo->DeviceName) / sizeof(WCHAR),
                    &(dcb->SymbolicLinkName->Buffer[deviceNamePos]));

  // Set the irp timeout in milliseconds
  // If the IrpTimeout is 0, we assume that the value was not changed
  dcb->IrpTimeout = DOKAN_IRP_PENDING_TIMEOUT;
  if (eventStart->IrpTimeout > 0) {
    if (eventStart->IrpTimeout > DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX) {
      eventStart->IrpTimeout = DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX;
    }

    if (eventStart->IrpTimeout < DOKAN_IRP_PENDING_TIMEOUT) {
      eventStart->IrpTimeout = DOKAN_IRP_PENDING_TIMEOUT;
    }
    dcb->IrpTimeout = eventStart->IrpTimeout;
  }

  DokanLogInfo(&logger, L"Event start using mount ID: %d; device name: %s.",
               dcb->MountId, driverInfo->DeviceName);

  dcb->UseAltStream = 0;
  if (eventStart->Flags & DOKAN_EVENT_ALTERNATIVE_STREAM_ON) {
    DOKAN_LOG_FINE_IRP(RequestContext, "ALT_STREAM_ON");
    dcb->UseAltStream = 1;
  }

  DokanStartEventNotificationThread(dcb);

  ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
  KeLeaveCriticalRegion();

  IoVerifyVolume(dcb->DeviceObject, FALSE);

  if (useMountManager) {
    if (!isMountPointDriveLetter && dcb->PersistentSymbolicLinkName) {
      // Set our existing directory path as reparse point.
      // It needs to be done outside IoVerifyVolume/DokanMountVolume as the
      // MountManager will also call IoVerifyVolume on the device which will
      // lead on a deadlock while trying to acquire the MountManager database.
      ULONG setReparseInputlength = 0;
      PCHAR setReparseInput = CreateSetReparsePointRequest(
          RequestContext, dcb->PersistentSymbolicLinkName,
          &setReparseInputlength);
      if (setReparseInput) {
        status = SendDirectoryFsctl(RequestContext, dcb->MountPoint,
                                    FSCTL_SET_REPARSE_POINT, setReparseInput,
                                    setReparseInputlength);
        ExFreePool(setReparseInput);
        if (NT_SUCCESS(status)) {
          // Inform MountManager of the new mount point.
          NotifyDirectoryMountPointCreated(dcb);
        } else {
          DokanLogError(&logger, status,
                        L"Failed to set reparse point on MountPoint \"%wZ\"",
                        dcb->MountPoint);
        }
      }
    }
  }

  RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
  RequestContext->Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);

  ExFreePool(eventStart);
  ExFreePool(baseGuidString);

  DokanLogInfo(&logger, L"Finished event start successfully");

  return RequestContext->Irp->IoStatus.Status;
}

// user assinged bigger buffer that is enough to return WriteEventContext
NTSTATUS
DokanEventWrite(__in PREQUEST_CONTEXT RequestContext) {
  KIRQL oldIrql;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry;
  PEVENT_INFORMATION eventInfo = NULL;
  PIRP writeIrp;

  GET_IRP_BUFFER_OR_RETURN(RequestContext->Irp, eventInfo);

  DOKAN_LOG_FINE_IRP(RequestContext, "EventInfo #%X", eventInfo->SerialNumber);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, &oldIrql);

  // search corresponding write IRP through pending IRP list
  listHead = &RequestContext->Dcb->PendingIrp.ListHead;

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {

    PIO_STACK_LOCATION writeIrpSp, eventIrpSp;
    PEVENT_CONTEXT eventContext;
    ULONG info = 0;
    NTSTATUS status;

    nextEntry = thisEntry->Flink;
    irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

    // check whehter this is corresponding IRP
    if (irpEntry->SerialNumber != eventInfo->SerialNumber) {
      continue;
    }

    // do NOT free irpEntry here
    writeIrp = irpEntry->RequestContext.Irp;
    if (writeIrp == NULL) {
      // this IRP has already been canceled
      ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
      DokanFreeIrpEntry(irpEntry);
      continue;
    }

    if (IoSetCancelRoutine(writeIrp, DokanIrpCancelRoutine) == NULL) {
      // if (IoSetCancelRoutine(writeIrp, NULL) != NULL) {
      // Cancel routine will run as soon as we release the lock
      InitializeListHead(&irpEntry->ListEntry);
      irpEntry->CancelRoutineFreeMemory = TRUE;
      continue;
    }

    writeIrpSp = irpEntry->RequestContext.IrpSp;
    eventIrpSp = IoGetCurrentIrpStackLocation(RequestContext->Irp);

    ASSERT(writeIrpSp != NULL);
    ASSERT(eventIrpSp != NULL);

    eventContext =
        (PEVENT_CONTEXT)
            writeIrp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT];
    ASSERT(eventContext != NULL);

    // short of buffer length
    if (eventIrpSp->Parameters.DeviceIoControl.OutputBufferLength <
        eventContext->Length) {
      DOKAN_LOG_FINE_IRP(RequestContext, "EventWrite: Buffer too small");
      status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
      PVOID buffer;
      if (RequestContext->Irp->MdlAddress)
        buffer =
            MmGetSystemAddressForMdlNormalSafe(RequestContext->Irp->MdlAddress);
      else
        buffer = RequestContext->Irp->AssociatedIrp.SystemBuffer;

      ASSERT(buffer != NULL);
      // The large event context that was not added to the list will not have a
      // serial number specified. It should use the same serial number as the
      // initial context that was sent to user mode for the IRP.
      eventContext->SerialNumber = irpEntry->SerialNumber;
      RtlCopyMemory(buffer, eventContext, eventContext->Length);

      info = eventContext->Length;
      status = STATUS_SUCCESS;
    }

    DokanFreeEventContext(eventContext);
    writeIrp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;

    KeReleaseSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, oldIrql);

    RequestContext->Irp->IoStatus.Status = status;
    RequestContext->Irp->IoStatus.Information = info;

    // this IRP will be completed by caller function
    return RequestContext->Irp->IoStatus.Status;
  }

  KeReleaseSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, oldIrql);

  // if the corresponding IRP not found, the user should already
  // canceled the operation and the IRP already destroyed.
  DOKAN_LOG_FINE_IRP(
      RequestContext,
      "EventWrite : Cannot found corresponding IRP. User should "
      "already canceled the operation. Return STATUS_CANCELLED.");

  return STATUS_CANCELLED;
}
