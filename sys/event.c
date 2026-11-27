/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2017 - 2025 Google, Inc.
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

static __inline PIO_WORKITEM
DokanGetIrpEntryWorkItem(__in PIRP_ENTRY IrpEntry) {
  return (PIO_WORKITEM)((PUCHAR)IrpEntry + DOKAN_IRP_ENTRY_WORKITEM_OFFSET);
}

static IO_WORKITEM_ROUTINE DokanCancelIrpWorkItemRoutine;
static VOID DokanCancelIrpWorkItemRoutine(_In_ PDEVICE_OBJECT DeviceObject,
                                          _In_opt_ PVOID Context) {
  UNREFERENCED_PARAMETER(DeviceObject);
  if (Context == NULL) {
    return;
  }
  PIRP_ENTRY irpEntry = (PIRP_ENTRY)Context;
  REQUEST_CONTEXT requestContext = irpEntry->RequestContext;
  NTSTATUS cancelStatus = irpEntry->AsyncStatus;

  IoUninitializeWorkItem(DokanGetIrpEntryWorkItem(irpEntry));
  DokanFreeIrpEntry(irpEntry);

  FsRtlEnterFileSystem();
  DokanCancelPendingIrp(&requestContext, cancelStatus);
  FsRtlExitFileSystem();
}

static VOID DokanQueueCancelIrpWorkItem(_In_ PDEVICE_OBJECT DeviceObject,
                                        _In_ PIRP_ENTRY IrpEntry,
                                        _In_ NTSTATUS Status) {
  PIO_WORKITEM workItem = DokanGetIrpEntryWorkItem(IrpEntry);
  IrpEntry->AsyncStatus = Status;
  IoInitializeWorkItem(DeviceObject, workItem);
  IoQueueWorkItem(workItem, DokanCancelIrpWorkItemRoutine, DelayedWorkQueue,
                  IrpEntry);
}

BOOLEAN DokanPopIrpEntryLocked(__in PIRP_ENTRY IrpEntry) {
  PIRP irp = IrpEntry->RequestContext.Irp;
  RemoveEntryList(&IrpEntry->ListEntry);
  InitializeListHead(&IrpEntry->ListEntry);
  if (irp == NULL) {
    DokanFreeIrpEntry(IrpEntry);
    return FALSE;
  }
  if (IoSetCancelRoutine(irp, NULL) == NULL) {
    // Cancel routine has already been invoked and will unlink (idempotently),
    // complete the IRP, and free IrpEntry once it acquires ListLock.
    return FALSE;
  }
  irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;
  return TRUE;
}

VOID DokanCreateIrpCancelRoutine(_Inout_ PDEVICE_OBJECT DeviceObject,
                                 _Inout_ _IRQL_uses_cancel_ PIRP Irp) {
  // Serialize with PendingIrp/PendingRetryIrp pop routines via IrpList->ListLock
  // so ownership of both Irp and irpEntry is unambiguous, then defer
  // DokanCancelCreateIrp to a PASSIVE_LEVEL system worker item.
  KIRQL oldIrql;
  PIRP_ENTRY irpEntry;

  IoReleaseCancelSpinLock(Irp->CancelIrql);

  irpEntry = Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY];
  if (irpEntry == NULL) {
    Irp->IoStatus.Information = 0;
    DokanCompleteIrpRequest(Irp, STATUS_CANCELLED);
    return;
  }

  PKSPIN_LOCK lock = &irpEntry->IrpList->ListLock;
  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(lock, &oldIrql);

  RemoveEntryList(&irpEntry->ListEntry);
  InitializeListHead(&irpEntry->ListEntry);
  if (IsListEmpty(&irpEntry->IrpList->ListHead)) {
    KeClearEvent(&irpEntry->IrpList->NotEmpty);
  }

  Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;
  Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = NULL;

  KeReleaseSpinLock(lock, oldIrql);

  PFILE_OBJECT fileObject = irpEntry->RequestContext.IrpSp->FileObject;
  if (fileObject != NULL && fileObject->FsContext2 != NULL) {
    PDokanCCB ccb = (PDokanCCB)fileObject->FsContext2;
    if (GetIdentifierType(ccb) == CCB && ccb->Fcb != NULL) {
      OplockDebugRecordFlag(ccb->Fcb, DOKAN_OPLOCK_DEBUG_CANCELED_CREATE);
    }
  }

  DokanQueueCancelIrpWorkItem(DeviceObject, irpEntry, STATUS_CANCELLED);
}

// Only used for non-Create IRPs.
VOID DokanIrpCancelRoutine(_Inout_ PDEVICE_OBJECT DeviceObject,
                           _Inout_ _IRQL_uses_cancel_ PIRP Irp) {
  KIRQL oldIrql;
  PIRP_ENTRY irpEntry;
  REQUEST_CONTEXT requestContext;
  NTSTATUS status;
  BOOLEAN needExecuteCleanup = FALSE;

  // Release the cancel spinlock
  IoReleaseCancelSpinLock(Irp->CancelIrql);

  status = DokanBuildRequestContext(DeviceObject, Irp, /*IsTopLevelIrp=*/FALSE,
                                    &requestContext);
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

    RemoveEntryList(&irpEntry->ListEntry);
    InitializeListHead(&irpEntry->ListEntry);

    DOKAN_LOG_("Cancel [%s][%s] FileObject=%p",
               DokanGetMajorFunctionStr(requestContext.IrpSp->MajorFunction),
               DokanGetMinorFunctionStr(requestContext.IrpSp->MajorFunction,
                                        requestContext.IrpSp->MinorFunction),
               requestContext.IrpSp->FileObject);

    // If Write is canceled before completion and buffer that saves writing
    // content is not freed, free it here
    if (requestContext.IrpSp->MajorFunction == IRP_MJ_WRITE) {
      DokanFreeWriteEventContext(Irp);
    } else if (requestContext.IrpSp->MajorFunction == IRP_MJ_CLEANUP) {
      // Cannot call DokanExecuteCleanup here because it requires PASSIVE_LEVEL
      // (it acquires ERESOURCE locks). Mark for execution after spinlock release.
      needExecuteCleanup = TRUE;
    }

    if (IsListEmpty(&irpEntry->IrpList->ListHead)) {
      KeClearEvent(&irpEntry->IrpList->NotEmpty);
    }

    Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;

    KeReleaseSpinLock(lock, oldIrql);
    DOKAN_LOG_FINE_IRP((&requestContext), "Canceled");

    if (needExecuteCleanup && KeGetCurrentIrql() > PASSIVE_LEVEL) {
      irpEntry->RequestContext.IsTopLevelIrp = FALSE;
      DokanQueueCancelIrpWorkItem(DeviceObject, irpEntry, STATUS_SUCCESS);
      return;
    }

    DokanFreeIrpEntry(irpEntry);

    // Execute cleanup at PASSIVE_LEVEL after releasing spinlock
    if (needExecuteCleanup) {
      DokanExecuteCleanup(&requestContext);
    }
  }

  Irp->IoStatus.Information = 0;
  DokanCompleteIrpRequest(Irp, needExecuteCleanup ? STATUS_SUCCESS
                                                  : STATUS_CANCELLED);
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
    if (Context != NULL) {
      DokanFreeEventContext((PEVENT_CONTEXT)Context);
    }
    DOKAN_LOG("NULL Irp received");
    return;
  }
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

  REQUEST_CONTEXT requestContext;
  NTSTATUS status = DokanBuildRequestContext(
      irpSp->DeviceObject, Irp, /*IsTopLevelIrp=*/FALSE, &requestContext);
  if (!NT_SUCCESS(status)) {
    if (Context != NULL) {
      DokanFreeEventContext((PEVENT_CONTEXT)Context);
    }
    DOKAN_LOG_("Failed to build request context for IRP=%p Status=%s", Irp,
               DokanGetNTSTATUSStr(status));
    return;
  }

  DOKAN_LOG_FINE_IRP((&requestContext),
                     "Oplock break completed FileObject=%p %s",
                     requestContext.IrpSp->FileObject,
                     DokanGetNTSTATUSStr(Irp->IoStatus.Status));

  //
  //  Check on the return value in the Irp.
  //
  if (Irp->IoStatus.Status == STATUS_SUCCESS) {
    status = DokanRegisterPendingIrp(&requestContext, (PEVENT_CONTEXT)Context);
    if (status != STATUS_PENDING) {
      if (requestContext.IrpSp->MajorFunction == IRP_MJ_WRITE) {
        DokanFreeWriteEventContext(Irp);
      }
      Irp->IoStatus.Information = 0;
      DokanCompleteIrpRequest(Irp, status);
    }
  } else {
    if (Context != NULL) {
      DokanFreeEventContext((PEVENT_CONTEXT)Context);
    }
    if (requestContext.IrpSp->MajorFunction == IRP_MJ_WRITE) {
      DokanFreeWriteEventContext(Irp);
    }
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
  DokanBuildRequestContext(irpSp->DeviceObject, Irp, /*IsTopLevelIrp=*/FALSE,
                           &requestContext);
  DOKAN_LOG_FINE_IRP((&requestContext), "Mark Irp pending");
  IoMarkIrpPending(Irp);
}

NTSTATUS
RegisterPendingIrpMain(__in PREQUEST_CONTEXT RequestContext,
                       __in_opt PEVENT_CONTEXT EventContext,
                       __in PIRP_LIST IrpList, __in ULONG CheckMount) {
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
  irpEntry->AsyncStatus = STATUS_SUCCESS;

  // Update the irp timeout for the entry
  if (RequestContext->Vcb) {
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&RequestContext->Dcb->Resource, TRUE);
    DokanUpdateTimeout(&irpEntry->TickCount, RequestContext->Dcb->IrpTimeout);
    ExReleaseResourceLite(&RequestContext->Dcb->Resource);
    KeLeaveCriticalRegion();
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

  // WDM Best Practice: Initialize IRP state and link structures BEFORE setting the
  // cancel routine, to ensure that if the cancel routine fires immediately, it has
  // valid pointers to work with (e.g., irpEntry).
  IoMarkIrpPending(RequestContext->Irp);
  RequestContext->Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] =
      irpEntry;

  if (RequestContext->IrpSp->MajorFunction == IRP_MJ_CREATE) {
    IoSetCancelRoutine(RequestContext->Irp, DokanCreateIrpCancelRoutine);
  } else {
    IoSetCancelRoutine(RequestContext->Irp, DokanIrpCancelRoutine);
  }

  // If the IRP was canceled just before or during our setup, try to back out.
  if (RequestContext->Irp->Cancel) {
    if (IoSetCancelRoutine(RequestContext->Irp, NULL) != NULL) {
      // We successfully unregistered the cancel routine before the OS fired it.
      // We can safely abort the pending registration and return immediately.
      RequestContext->Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;
      KeReleaseSpinLock(&IrpList->ListLock, oldIrql);
      DokanFreeIrpEntry(irpEntry);
      return STATUS_CANCELLED;
    }
    // If IoSetCancelRoutine returned NULL here, the OS has already taken the
    // cancel routine. We MUST continue and insert the entry into the list.
    // The cancel routine (which might be blocking on IrpList->ListLock right now)
    // will safely remove it and complete the IRP once we release the lock.
  }

  InsertTailList(&IrpList->ListHead, &irpEntry->ListEntry);

  if (IrpList->EventEnabled) {
    KeSetEvent(&IrpList->NotEmpty, IO_NO_INCREMENT, FALSE);
  }

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
                                    &RequestContext->Dcb->PendingIrp,
                                    /*CheckMount=*/TRUE);
  }

  if (status == STATUS_PENDING) {
    DokanEventNotification(RequestContext, &RequestContext->Dcb->NotifyEvent,
                           EventContext);
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
  NTSTATUS status = RegisterPendingIrpMain(
      RequestContext, /*EventContext=*/NULL,
      &RequestContext->Dcb->PendingRetryIrp, /*CheckMount=*/TRUE);
  if (status != STATUS_PENDING) {
    // The IRP was already marked pending by DokanPrePostIrp during the oplock
    // break, so we must complete/unwind it if registration failed (e.g. due to
    // concurrent unmount or cancellation).
    PIRP_ENTRY irpEntry = DokanAllocateIrpEntry();
    if (irpEntry != NULL) {
      RtlZeroMemory(irpEntry, sizeof(IRP_ENTRY));
      irpEntry->RequestContext = *RequestContext;
      DokanQueueCancelIrpWorkItem(RequestContext->DeviceObject, irpEntry,
                                  status);
    } else if (KeGetCurrentIrql() <= APC_LEVEL) {
      DokanCancelPendingIrp(RequestContext, status);
    } else {
      RequestContext->Irp->IoStatus.Information = 0;
      DokanCompleteIrpRequest(RequestContext->Irp, status);
    }
  }
}

VOID DokanRegisterAsyncCreateFailure(__in PREQUEST_CONTEXT RequestContext,
                                     __in NTSTATUS Status) {
  if (!RequestContext->Vcb) {
    return;
  }
  PIRP_ENTRY irpEntry = DokanAllocateIrpEntry();
  if (irpEntry != NULL) {
    RtlZeroMemory(irpEntry, sizeof(IRP_ENTRY));
    irpEntry->RequestContext = *RequestContext;
    DokanQueueCancelIrpWorkItem(RequestContext->DeviceObject, irpEntry, Status);
    return;
  }
  if (KeGetCurrentIrql() <= APC_LEVEL) {
    DokanCancelCreateIrp(RequestContext, Status);
  } else {
    RequestContext->Irp->IoStatus.Information = 0;
    DokanCompleteIrpRequest(RequestContext->Irp, Status);
  }
}

void DokanDispatchCompletion(__in PDEVICE_OBJECT DeviceObject,
                             __in PIRP_ENTRY irpEntry,
                             __in PEVENT_INFORMATION eventInfo) {
  DOKAN_INIT_LOGGER(logger, DeviceObject->DriverObject, 0);
  DOKAN_LOG_BEGIN_MJ((&irpEntry->RequestContext));

  if (eventInfo->Status == STATUS_PENDING) {
    DokanLogError(&logger, /*Status=*/0,
                  L"DLL returned STATUS_PENDING for IRP of type %d. "
                  L"It should never return STATUS_PENDING for any IRP.",
                  irpEntry->RequestContext.IrpSp->MajorFunction);
  }

  ASSERT(!irpEntry->RequestContext.DoNotComplete);

  const UCHAR majorFunction = irpEntry->RequestContext.IrpSp->MajorFunction;
  switch (majorFunction) {
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

  DokanCompleteRequestAndCheckDismount(
      &irpEntry->RequestContext, majorFunction,
      irpEntry->RequestContext.Irp->IoStatus.Status,
      /*AllowSyncDismount=*/FALSE);
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
  return max((ULONG)sizeof(EVENT_INFORMATION),
             FIELD_OFFSET(EVENT_INFORMATION, Buffer[0]) +
                 (ULONG)EventInfo->BufferLength);
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

  if (IsUnmountPendingVcb(RequestContext->Vcb)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Volume is not mounted");
    return STATUS_NO_SUCH_DEVICE;
  }

  bufferLength =
      RequestContext->IrpSp->Parameters.DeviceIoControl.InputBufferLength;
  if (bufferLength < sizeof(EVENT_INFORMATION)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Wrong input buffer length");
    return STATUS_BUFFER_TOO_SMALL;
  }

  buffer = (PCHAR)RequestContext->Irp->AssociatedIrp.SystemBuffer;
  ASSERT(buffer != NULL);

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
    UCHAR majorFunction = irpEntry->RequestContext.IrpSp->MajorFunction;
    ULONG entryOffset = offset;
    offset += GetEventInfoSize(majorFunction, eventInfo);
    if (DokanPopIrpEntryLocked(irpEntry)) {
      irpEntry->EventInfoOffset = entryOffset;
      InsertTailList(&completeList, thisEntry);
    }
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

  eventInfo = NULL;
  if (IsListEmpty(&completeList)) {
    DokanLogInfo(&logger, L"Warning: no matching IRPs found for reply.");
  }
  while (!IsListEmpty(&completeList)) {
    listHead = RemoveHeadList(&completeList);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    if (IsUnmountPendingVcb(RequestContext->Vcb)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Volume is not mounted second check");
      // These IRPs were already removed from PendingIrp with their cancel
      // routines cleared; ReleasePendingIrp cannot see them, so unwind and
      // complete every entry in completeList before returning.
      DokanCancelPendingIrp(&irpEntry->RequestContext, STATUS_CANCELLED);
      result = STATUS_NO_SUCH_DEVICE;
    } else if (irpEntry->EventInfoOffset + sizeof(EVENT_INFORMATION) >
               bufferLength) {
      DokanLogInfo(&logger, L"Unexpected end of event info list.");
      DokanCancelPendingIrp(&irpEntry->RequestContext, STATUS_CANCELLED);
    } else {
      eventInfo = (PEVENT_INFORMATION)(buffer + irpEntry->EventInfoOffset);
      eventInfoSize = GetEventInfoSize(
          irpEntry->RequestContext.IrpSp->MajorFunction, eventInfo);
      if (eventInfoSize > bufferLength - irpEntry->EventInfoOffset) {
        DokanLogInfo(&logger, L"Truncated EVENT_INFORMATION in reply batch.");
        DokanCancelPendingIrp(&irpEntry->RequestContext, STATUS_CANCELLED);
      } else {
        DokanDispatchCompletion(RequestContext->DeviceObject, irpEntry,
                                eventInfo);
      }
    }
    DokanFreeIrpEntry(irpEntry);
    irpEntry = NULL;
  }
  if (badUsageByCaller && result != STATUS_NO_SUCH_DEVICE) {
    // This flag should only be set if there is a coding error in the DLL.
    // Defer via work item because DokanDiskUserFsRequest currently holds
    // Dcb->RemoveLock (synchronous DokanUnmount would self-deadlock).
    DokanLogInfo(
        &logger,
        L"Unmounting to avoid hanging requests due to incorrect usage.");
    if (RequestContext->Vcb != NULL) {
      DokanQueueEventRelease(RequestContext->Vcb->DeviceObject);
    }
  }
  return result;
}

// Gets the binary owner information from the security descriptor of the device.
// Returns either the information or NULL if it cannot be read. Logs the reason
// for any failure. If the result is not NULL then the caller must free it via
// DokanFree.
char* GetDeviceOwner(__in PDOKAN_LOGGER Logger,
                     __in const WCHAR* DeviceNameForLog,
                     __in PDEVICE_OBJECT DeviceObject,
                     _Inout_ PULONG OwnerSize) {
  NTSTATUS status = STATUS_SUCCESS;
  HANDLE handle = 0;
  char* result = NULL;
  __try {
    status = ObOpenObjectByPointer(DeviceObject, OBJ_KERNEL_HANDLE, NULL,
                                   READ_CONTROL, 0, KernelMode, &handle);
    if (!NT_SUCCESS(status)) {
      DokanLogInfo(Logger, L"Failed to open device: %s, status: 0x%x",
                   DeviceNameForLog, status);
      __leave;
    }
    status = ZwQuerySecurityObject(handle, OWNER_SECURITY_INFORMATION, NULL, 0,
                                   OwnerSize);
    if (status != STATUS_BUFFER_TOO_SMALL) {
      DokanLogInfo(Logger,
                   L"Failed to query for owner length of device: %s,"
                   L" status: 0x%x",
                   DeviceNameForLog, status);
      __leave;
    }
    result = DokanAlloc(*OwnerSize);
    status = ZwQuerySecurityObject(handle, OWNER_SECURITY_INFORMATION, result,
                                   *OwnerSize, OwnerSize);
    if (!NT_SUCCESS(status)) {
      DokanLogInfo(Logger, L"Failed to query for owner of device: %s,"
                   L" status: 0x%x",
                   DeviceNameForLog, status);
      DokanFree(result);
      result = NULL;
      __leave;
    }
  } __finally {
    if (handle != 0) {
      ZwClose(handle);
    }
  }
  return result;
}

// Determines whether the given two devices have the same owner specified by
// their security descriptors. Calling the devices "new" and "existing" is
// useful for the logging done within this function, but otherwise irrelevant.
// Any failure or negative result is logged.
BOOLEAN HasSameOwner(__in PDOKAN_LOGGER Logger,
                     __in PDEVICE_OBJECT NewDevice,
                     __in PDEVICE_OBJECT OldDevice) {
  BOOLEAN result = FALSE;
  char* newOwner = NULL;
  char* oldOwner = NULL;
  ULONG oldOwnerSize = 0;
  ULONG newOwnerSize = 0;
  __try {
    newOwner = GetDeviceOwner(Logger, L"new device", NewDevice, &newOwnerSize);
    if (newOwner == NULL) {
      __leave;
    }
    oldOwner = GetDeviceOwner(Logger, L"old device", OldDevice, &oldOwnerSize);
    if (oldOwner == NULL) {
      __leave;
    }
    if (oldOwnerSize != newOwnerSize ||
        RtlCompareMemory(newOwner, oldOwner, oldOwnerSize) != oldOwnerSize) {
      DokanLogInfo(Logger, L"Retrieved device owners and they do not match.");
      __leave;
    }
    result = TRUE;
  } __finally {
    if (newOwner != NULL) {
      DokanFree(newOwner);
    }
    if (oldOwner != NULL) {
      DokanFree(oldOwner);
    }
  }
  return result;
}

// Unmounts the drive indicated by OldDevice / OldVolumeDevice in order for the
// caller to replace it with the one indicated by NewControl. Caller MUST NOT
// hold DokanGlobal->Resource or mountEntry->Resource and MUST have pinned
// OldDevice (and OldVolumeDevice if non-NULL).
BOOLEAN MaybeUnmountOldDrive(__in PREQUEST_CONTEXT RequestContext,
                             __in PDOKAN_LOGGER Logger,
                             __in PDOKAN_GLOBAL DokanGlobal,
                             __in PDEVICE_OBJECT OldDevice,
                             __in_opt PDEVICE_OBJECT OldVolumeDevice,
                             __in PDOKAN_CONTROL NewControl) {
  DokanLogInfo(Logger,
      L"Mount point exists and"
      L" DOKAN_EVENT_REPLACE_DOKAN_DRIVE_IF_EXISTS is set: %s",
      NewControl->MountPoint);
  if (!HasSameOwner(Logger, NewControl->DiskDeviceObject, OldDevice)) {
    DokanLogInfo(Logger,
                 L"Not replacing existing drive with different owner.");
    return FALSE;
  }
  DokanLogInfo(Logger, L"Unmounting the existing drive.");
  if (OldVolumeDevice != NULL) {
    DokanEventRelease(RequestContext, OldVolumeDevice);
  }
  PMOUNT_ENTRY entryAfterUnmount =
      FindMountEntry(DokanGlobal, NewControl, /*ExclusiveLock=*/FALSE);
  if (entryAfterUnmount != NULL) {
    DokanLogInfo(
        Logger,
        L"Warning: old mount entry was not removed by unmount attempt.");
    ExReleaseResourceLite(&entryAfterUnmount->Resource);
    return FALSE;
  }
  DokanLogInfo(Logger, L"The existing mount entry is now gone.");
  return TRUE;
}

VOID RemoveSessionDevices(__in PREQUEST_CONTEXT RequestContext,
                          __in ULONG sessionId) {
  if (sessionId == (ULONG)-1) {
    return;
  }

  LIST_ENTRY toDeleteList;
  InitializeListHead(&toDeleteList);

  ExAcquireFastMutex(&RequestContext->DokanGlobal->SessionMountPointLock);
  PLIST_ENTRY listHead = &RequestContext->DokanGlobal->SessionMountPointList;
  for (PLIST_ENTRY entry = listHead->Flink, nextEntry = entry->Flink;
       entry != listHead; entry = nextEntry, nextEntry = entry->Flink) {
    PSESSION_MOUNT_POINT_ENTRY sessionEntry =
        CONTAINING_RECORD(entry, SESSION_MOUNT_POINT_ENTRY, ListEntry);
    if (sessionEntry->SessionId == sessionId) {
      RemoveEntryList(&sessionEntry->ListEntry);
      InsertTailList(&toDeleteList, &sessionEntry->ListEntry);
    }
  }
  ExReleaseFastMutex(&RequestContext->DokanGlobal->SessionMountPointLock);

  while (!IsListEmpty(&toDeleteList)) {
    PLIST_ENTRY entry = RemoveHeadList(&toDeleteList);
    PSESSION_MOUNT_POINT_ENTRY sessionEntry =
        CONTAINING_RECORD(entry, SESSION_MOUNT_POINT_ENTRY, ListEntry);
    DeleteMountPointSymbolicLink(&sessionEntry->MountPoint);
    if (sessionEntry->MountPoint.Buffer != NULL) {
      DokanFree(sessionEntry->MountPoint.Buffer);
    }
    DokanFree(sessionEntry);
  }
}

// 40 KB is strictly above the default 24 KB x64/ARM64 (12 KB x86) kernel stack
// so KeExpandKernelStackAndCalloutEx always switches to a fresh expanded stack
// segment, yet safely below MAXIMUM_EXPANSION_SIZE (58 KB) on every Windows
// version from Server 2016 through Server 2025.
#define DOKAN_MOUNT_TRIGGER_STACK_SIZE (40 * 1024)

typedef struct _DOKAN_TRIGGER_MOUNT_PARAMS {
  PDokanDCB Dcb;
  BOOLEAN IsNetworkFileSystem;
} DOKAN_TRIGGER_MOUNT_PARAMS, *PDOKAN_TRIGGER_MOUNT_PARAMS;

// Triggers IopMountVolume on a newly created disk device without calling
// IoVerifyVolume on an unmounted disk (ntoskrnl!IoVerifyVolume allocates a
// replacement VPB via IopCreateVpb when VPB_MOUNTED is clear, leaking the
// initial VPB created by IoCreateDevice). For disk mounts, opening the named
// FILE_DEVICE_DISK object with FILE_READ_DATA routes through IopParseDevice ->
// IopMountVolume directly on the initial VPB. Network mounts intentionally keep
// the backing FILE_DEVICE_DISK unnamed (naming it triggers a FltMgr ->
// MountMgr -> WdFilter -> IopMountVolume self-deadlock on DeviceLock), so
// network mounts fall through to IoVerifyVolume.
static EXPAND_STACK_CALLOUT DokanTriggerVolumeMountCallout;
static VOID NTAPI DokanTriggerVolumeMountCallout(_In_opt_ PVOID Parameter) {
  if (Parameter == NULL) {
    return;
  }
  PDOKAN_TRIGGER_MOUNT_PARAMS params = (PDOKAN_TRIGGER_MOUNT_PARAMS)Parameter;
  PDokanDCB dcb = params->Dcb;

  if (!params->IsNetworkFileSystem && dcb->DiskDeviceName != NULL &&
      dcb->DiskDeviceName->Length > 0) {
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    HANDLE volumeHandle = NULL;
    PIRP topLevelIrp = IoGetTopLevelIrp();

    __try {
      if (topLevelIrp != NULL) {
        IoSetTopLevelIrp(NULL);
      }

      IO_STATUS_BLOCK ioStatusBlock;
      OBJECT_ATTRIBUTES objectAttributes;
      InitializeObjectAttributes(&objectAttributes, dcb->DiskDeviceName,
                                 OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                                 NULL);
      status =
          ZwCreateFile(&volumeHandle, FILE_READ_DATA | SYNCHRONIZE,
                       &objectAttributes, &ioStatusBlock, NULL, 0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    } __finally {
      if (volumeHandle != NULL) {
        ZwClose(volumeHandle);
      }
      if (topLevelIrp != NULL) {
        IoSetTopLevelIrp(topLevelIrp);
      }
    }

    if (NT_SUCCESS(status) && dcb->Vcb != NULL) {
      return;
    }
    DOKAN_LOG_("ZwCreateFile volume mount trigger did not mount "
               "(status=0x%x %s, Vcb=%p); falling back to IoVerifyVolume",
               status, DokanGetNTSTATUSStr(status), dcb->Vcb);
  }

  if (!IsDeletePending(dcb->DeviceObject)) {
    IoVerifyVolume(dcb->DeviceObject, FALSE);
  }
}

static VOID DokanTriggerVolumeMount(__in PDokanDCB Dcb,
                                    __in BOOLEAN IsNetworkFileSystem) {
  DOKAN_TRIGGER_MOUNT_PARAMS params;
  params.Dcb = Dcb;
  params.IsNetworkFileSystem = IsNetworkFileSystem;

  // ZwCreateFile -> IopMountVolume -> FltMgr -> WdFilter -> MountMgr ->
  // DokanDispatchCreate under Driver Verifier can consume ~20 KB of stack.
  // Expand onto a fresh kernel stack segment in the caller's thread/session
  // context (preserving session-local \DosDevices\ symlinks for /c mounts).
  NTSTATUS expandStatus = KeExpandKernelStackAndCalloutEx(
      DokanTriggerVolumeMountCallout, &params, DOKAN_MOUNT_TRIGGER_STACK_SIZE,
      /*Wait=*/TRUE, /*Context=*/NULL);
  if (!NT_SUCCESS(expandStatus)) {
    DOKAN_LOG_("KeExpandKernelStackAndCalloutEx failed (0x%x %s); running "
               "volume mount trigger on current stack",
               expandStatus, DokanGetNTSTATUSStr(expandStatus));
    DokanTriggerVolumeMountCallout(&params);
  }
}

// start event dispatching
NTSTATUS
DokanEventStart(__in PREQUEST_CONTEXT RequestContext) {
  ULONG outBufferLen;
  PEVENT_START eventStart = NULL;
  PEVENT_DRIVER_INFO driverInfo = NULL;
  PDOKAN_CONTROL dokanControl = NULL;
  PDokanDCB dcb = NULL;
  NTSTATUS status = STATUS_SUCCESS;
  DEVICE_TYPE deviceType;
  ULONG deviceCharacteristics = 0;
  WCHAR *baseGuidString = NULL;
  GUID baseGuid = DOKAN_BASE_GUID;
  UNICODE_STRING unicodeGuid;
  ULONG deviceNamePos;
  BOOLEAN useMountManager = FALSE;
  BOOLEAN mountGlobally = TRUE;
  BOOLEAN fileLockUserMode = FALSE;
  ULONG sessionId = (ULONG)-1;
  PSECURITY_DESCRIPTOR volumeSecurityDescriptor = NULL;
  BOOLEAN startFailure = FALSE;
  BOOLEAN isMountPointDriveLetter = FALSE;
  BOOLEAN globalResourceLocked = FALSE;
  PMOUNT_ENTRY foundPrevEntry = NULL;
  PDEVICE_OBJECT diskDeviceObject = NULL;
  PDEVICE_OBJECT volumeDeviceObject = NULL;

  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject, 0);

  DokanLogInfo(&logger, L"Entered event start.");
  DOKAN_LOG_FINE_IRP(RequestContext, "Event start");

  // Validate the input buffer size using a separate pointer before allocating
  // our own EVENT_START copy inside __try.
  PEVENT_START inputEventStart = NULL;
  GET_IRP_BUFFER(RequestContext->Irp, inputEventStart);

  outBufferLen =
      RequestContext->IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
  if (outBufferLen != sizeof(EVENT_DRIVER_INFO) || !inputEventStart) {
    return DokanLogError(
        &logger, STATUS_INSUFFICIENT_RESOURCES,
        L"Buffer IN/OUT received do not match the expected size.");
  }

  __try {
    eventStart = DokanAlloc(sizeof(EVENT_START));
    baseGuidString = DokanAllocZero(64 * sizeof(WCHAR));
    dokanControl = DokanAllocZero(sizeof(DOKAN_CONTROL));
    if (eventStart == NULL || baseGuidString == NULL || dokanControl == NULL) {
      status = DokanLogError(&logger, STATUS_INSUFFICIENT_RESOURCES,
                             L"Failed to allocate buffers in event start.");
      __leave;
    }

    RtlCopyMemory(eventStart, RequestContext->Irp->AssociatedIrp.SystemBuffer,
                  sizeof(EVENT_START));
    if (eventStart->UserVersion != DOKAN_DRIVER_VERSION) {
      DokanLogInfo(&logger, L"Driver version check in event start failed.");
      startFailure = TRUE;
    }

    if (DokanSearchStringChar(eventStart->MountPoint,
                              sizeof(eventStart->MountPoint), '\0') == -1 ||
        DokanSearchStringChar(eventStart->UNCName, sizeof(eventStart->UNCName),
                              '\0') == -1) {
      DokanLogInfo(&logger, L"MountPoint / UNCName provided are not null "
                            L"terminated in event start.");
      startFailure = TRUE;
    }

    if (eventStart->DeviceType == DOKAN_NETWORK_FILE_SYSTEM &&
        DokanSearchStringChar(eventStart->UNCName, sizeof(eventStart->UNCName),
                              '\0') == -1) {
      DokanLogInfo(&logger, L"Network filesystem is enabled without UNCName.");
      startFailure = TRUE;
    }

    driverInfo = RequestContext->Irp->AssociatedIrp.SystemBuffer;
    driverInfo->Flags = 0;
    if (startFailure) {
      driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;
      driverInfo->Status = DOKAN_START_FAILED;
      RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
      RequestContext->Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
      status = STATUS_SUCCESS;
      __leave;
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
      DOKAN_LOG_FINE_IRP(RequestContext, "Unknown device type: %d",
                         eventStart->DeviceType);
      deviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
    }

    if (eventStart->Flags & DOKAN_EVENT_REMOVABLE) {
      DOKAN_LOG_FINE_IRP(RequestContext,
                         "DeviceCharacteristics |= FILE_REMOVABLE_MEDIA");
      deviceCharacteristics |= FILE_REMOVABLE_MEDIA;
    }

    if (eventStart->Flags & DOKAN_EVENT_WRITE_PROTECT) {
      DOKAN_LOG_FINE_IRP(RequestContext,
                         "DeviceCharacteristics |= FILE_READ_ONLY_DEVICE");
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

    if (eventStart->Flags & DOKAN_EVENT_CASE_SENSITIVE) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Case sensitive enabled");
    }

    if (eventStart->Flags & DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Network unmount enabled");
    }

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&RequestContext->DokanGlobal->Resource,
                                   TRUE);
    globalResourceLocked = TRUE;

    RtlStringCchCopyW(dokanControl->MountPoint, MAXIMUM_FILENAME_LENGTH,
                      L"\\DosDevices\\");
    if (wcslen(eventStart->MountPoint) == 1) {
      dokanControl->MountPoint[12] = towupper(eventStart->MountPoint[0]);
      dokanControl->MountPoint[13] = L':';
      dokanControl->MountPoint[14] = L'\0';
    } else {
      RtlStringCchCatW(dokanControl->MountPoint, MAXIMUM_FILENAME_LENGTH,
                       eventStart->MountPoint);
    }
    dokanControl->SessionId = sessionId;
    dokanControl->MountOptions = eventStart->Flags;

    DOKAN_LOG_FINE_IRP(RequestContext, "Checking for MountPoint %ls",
                       dokanControl->MountPoint);
    foundPrevEntry = FindMountEntry(RequestContext->DokanGlobal, dokanControl,
                                    /*ExclusiveLock=*/FALSE);
    if (foundPrevEntry != NULL && !useMountManager) {
      // Legacy behavior: fail on existing mount entry with the same mount
      // point. Note: there are edge cases where this entry (which is internal
      // to dokan) may be left around despite the drive being technically
      // unmounted. In such a case, the code outside this driver can't know
      // that. Therefore, it's advisable to set the flag and avoid this branch.
      driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;
      driverInfo->Status = DOKAN_START_FAILED;
      RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
      RequestContext->Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
      status = STATUS_SUCCESS;
      __leave;
    }

    baseGuid.Data2 = (USHORT)(RequestContext->DokanGlobal->MountId & 0xFFFF) ^
                     baseGuid.Data2;
    baseGuid.Data3 =
        (USHORT)(RequestContext->DokanGlobal->MountId >> 16) ^ baseGuid.Data3;

    status = RtlStringFromGUID(&baseGuid, &unicodeGuid);
    if (!NT_SUCCESS(status)) {
      status =
          DokanLogError(&logger, status, L"Failed to convert GUID to string.");
      __leave;
    }

    RtlStringCchCopyW(baseGuidString, 64, unicodeGuid.Buffer);
    RtlFreeUnicodeString(&unicodeGuid);

    InterlockedIncrement((LONG *)&RequestContext->DokanGlobal->MountId);

    if (eventStart->VolumeSecurityDescriptorLength != 0) {
      DOKAN_LOG_FINE_IRP(RequestContext,
                         "Using volume security descriptor of length %d",
                         eventStart->VolumeSecurityDescriptorLength);
      deviceCharacteristics |= FILE_DEVICE_SECURE_OPEN;
      volumeSecurityDescriptor = eventStart->VolumeSecurityDescriptor;
    }

    status = DokanCreateDiskDevice(
        RequestContext->DeviceObject->DriverObject,
        RequestContext->DokanGlobal->MountId, eventStart->MountPoint,
        eventStart->UNCName, volumeSecurityDescriptor, sessionId,
        baseGuidString, RequestContext->DokanGlobal, deviceType,
        deviceCharacteristics, mountGlobally, useMountManager, dokanControl);

    if (!NT_SUCCESS(status)) {
      status = DokanLogError(&logger, status, L"Disk device creation failed.");
      __leave;
    }

    dcb = dokanControl->Dcb;
    diskDeviceObject = dcb->DeviceObject;
    // Pin diskDeviceObject across DokanEventStart so an external unmount cannot
    // free the DEVICE_OBJECT before we finish.
    ObReferenceObject(diskDeviceObject);

    dcb->MountOptions = eventStart->Flags;
    dcb->DispatchDriverLogs =
        (eventStart->Flags & DOKAN_EVENT_DISPATCH_DRIVER_LOGS) != 0;
    dcb->AllowIpcBatching =
        (eventStart->Flags & DOKAN_EVENT_ALLOW_IPC_BATCHING) != 0;
    isMountPointDriveLetter = IsMountPointDriveLetter(dcb->MountPoint);

    if (dcb->DispatchDriverLogs) {
      IncrementVcbLogCacheCount();
    }

    // This has 2 effects that differ from legacy behavior: (1) try to get rid
    // of the occupied drive, if it's a dokan drive owned by the same user; (2)
    // have the mount manager avoid using the occupied drive, if it's not one
    // we're willing to get rid of. Release DokanGlobal->Resource BEFORE
    // invoking MaybeUnmountOldDrive (which issues MountManager/MM/CC calls) to
    // avoid AB-BA lock cycles.
    if (foundPrevEntry != NULL) {
      PDEVICE_OBJECT oldDevice = foundPrevEntry->MountControl.DiskDeviceObject;
      PDEVICE_OBJECT oldVolumeDevice =
          foundPrevEntry->MountControl.VolumeDeviceObject;
      if (oldDevice != NULL) {
        ObReferenceObject(oldDevice);
      }
      if (oldVolumeDevice != NULL) {
        ObReferenceObject(oldVolumeDevice);
      }
      ExReleaseResourceLite(&foundPrevEntry->Resource);
      foundPrevEntry = NULL;
      ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
      KeLeaveCriticalRegion();
      globalResourceLocked = FALSE;

      BOOLEAN oldUnmounted = FALSE;
      if (oldDevice != NULL) {
        oldUnmounted = MaybeUnmountOldDrive(
            RequestContext, &logger, RequestContext->DokanGlobal, oldDevice,
            oldVolumeDevice, dokanControl);
      }
      if (oldVolumeDevice != NULL) {
        ObDereferenceObject(oldVolumeDevice);
      }
      if (oldDevice != NULL) {
        ObDereferenceObject(oldDevice);
      }

      KeEnterCriticalRegion();
      ExAcquireResourceExclusiveLite(&RequestContext->DokanGlobal->Resource,
                                     TRUE);
      globalResourceLocked = TRUE;

      if (oldUnmounted) {
        // Re-check in case another concurrent mount claimed this mount point
        // while DokanGlobal->Resource was released.
        PMOUNT_ENTRY racedEntry =
            FindMountEntry(RequestContext->DokanGlobal, dokanControl,
                           /*ExclusiveLock=*/FALSE);
        if (racedEntry != NULL) {
          ExReleaseResourceLite(&racedEntry->Resource);
          driverInfo->Flags |= DOKAN_DRIVER_INFO_OLD_DRIVE_LEFT_MOUNTED;
          dcb->ForceDriveLetterAutoAssignment = TRUE;
        } else {
          driverInfo->Flags |= DOKAN_DRIVER_INFO_OLD_DRIVE_UNMOUNTED;
        }
      } else {
        driverInfo->Flags |= DOKAN_DRIVER_INFO_OLD_DRIVE_LEFT_MOUNTED;
        dcb->ForceDriveLetterAutoAssignment = TRUE;
      }
    } else if (eventStart->Flags & DOKAN_EVENT_DRIVE_LETTER_IN_USE) {
      // The drive letter is perceived as being in use in user mode, and this
      // driver doesn't own it. In this case we explicitly ask not to use it.
      // Although we ask the mount manager to avoid clobbering existing links
      // in device.c, that only works for persistent ones and not e.g. if you
      // do something like "subst g: c:\temp".
      DokanLogInfo(
          &logger,
          L"Forcing auto-assignment because the drive letter is in use by"
          L" another driver.");
      dcb->ForceDriveLetterAutoAssignment = TRUE;
    }
    if (!isMountPointDriveLetter) {
      dcb->ForceDriveLetterAutoAssignment = FALSE;
    }
    if (dcb->ForceDriveLetterAutoAssignment) {
      driverInfo->Flags |= DOKAN_DRIVER_INFO_AUTO_ASSIGN_REQUESTED;
    }
    if (!InsertMountEntry(RequestContext->DokanGlobal, dokanControl)) {
      ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
      KeLeaveCriticalRegion();
      globalResourceLocked = FALSE;
      if (NT_SUCCESS(
              IoAcquireRemoveLock(&dcb->RemoveLock, RequestContext->Irp))) {
        IoReleaseRemoveLockAndWait(&dcb->RemoveLock, RequestContext->Irp);
      }
      DokanDeleteDcb(dcb);
      status = DokanLogError(&logger, STATUS_INSUFFICIENT_RESOURCES,
                             L"Failed to allocate new mount entry.");
      __leave;
    }
    DokanLogInfo(&logger, L"Inserted new mount entry.");

    dcb->FileLockInUserMode = fileLockUserMode;
    driverInfo->DeviceNumber = dcb->MountId;
    driverInfo->MountId = dcb->MountId;
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
    dcb->FcbGarbageCollectionIntervalMs =
        eventStart->FcbGarbageCollectionIntervalMs;
    // Sanitize the garbage collection parameter.
    if (dcb->FcbGarbageCollectionIntervalMs > 0) {
      if (dcb->FcbGarbageCollectionIntervalMs <
          MIN_FCB_GARBAGE_COLLECTION_INTERVAL) {
        DokanLogInfo(&logger,
                     L"Not using FCB garbage collection because the"
                     L" specified interval of %lu is too low to be useful.",
                     dcb->FcbGarbageCollectionIntervalMs);
        dcb->FcbGarbageCollectionIntervalMs = 0;
      }
    }

    DokanLogInfo(&logger, L"Event start using mount ID: %d; device name: %s.",
                 dcb->MountId, driverInfo->DeviceName);

    dcb->UseAltStream = 0;
    if (eventStart->Flags & DOKAN_EVENT_ALTERNATIVE_STREAM_ON) {
      DOKAN_LOG_FINE_IRP(RequestContext, "ALT_STREAM_ON");
      dcb->UseAltStream = 1;
    }

    status = DokanStartEventNotificationThread(dcb);
    if (!NT_SUCCESS(status)) {
      ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
      KeLeaveCriticalRegion();
      globalResourceLocked = FALSE;
      RemoveMountEntry(RequestContext->DokanGlobal, dokanControl);
      if (NT_SUCCESS(
              IoAcquireRemoveLock(&dcb->RemoveLock, RequestContext->Irp))) {
        IoReleaseRemoveLockAndWait(&dcb->RemoveLock, RequestContext->Irp);
      }
      DokanDeleteDcb(dcb);
      status = DokanLogError(&logger, status,
                             L"Failed to start event notification thread.");
      __leave;
    }

    ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
    KeLeaveCriticalRegion();
    globalResourceLocked = FALSE;

    DokanTriggerVolumeMount(dcb, deviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);

    PMOUNT_ENTRY mountEntry =
        FindMountEntry(RequestContext->DokanGlobal, dokanControl,
                       /*ExclusiveLock=*/FALSE);
    if (!mountEntry) {
      // The device was removed right after being mounted and before we finish.
      status = DokanLogError(
          &logger, STATUS_INSUFFICIENT_RESOURCES,
          L"Unable to find mount entry after insert and "
          L"DokanTriggerVolumeMount. The device must have been removed.");
      __leave;
    }
    volumeDeviceObject = mountEntry->MountControl.VolumeDeviceObject;
    if (volumeDeviceObject != NULL) {
      ObReferenceObject(volumeDeviceObject);
    }
    // Release mountEntry->Resource BEFORE calling MountManager/NTFS IOCTLs
    // (re-entrant IOCTL_MOUNTDEV_LINK_CREATED acquires MountPointListLock ->
    // mountEntry->Resource exclusively). Guard DCB string reads with
    // dcb->RemoveLock (zero ERESOURCEs held).
    ExReleaseResourceLite(&mountEntry->Resource);
    mountEntry = NULL;

    if (volumeDeviceObject == NULL) {
      // DokanMountVolume did not mount this disk; clean up the orphaned
      // notification thread, MountEntry, and DCB instead of reporting success.
      DokanLogError(&logger, STATUS_UNSUCCESSFUL,
                    L"Volume mount failed during DokanEventStart; cleaning up "
                    L"unmounted disk device.");
      KeSetEvent(&dcb->ReleaseEvent, IO_NO_INCREMENT, FALSE);
      DokanStopEventNotificationThread(dcb);
      RemoveMountEntry(RequestContext->DokanGlobal, dokanControl);
      if (NT_SUCCESS(
              IoAcquireRemoveLock(&dcb->RemoveLock, RequestContext->Irp))) {
        IoReleaseRemoveLockAndWait(&dcb->RemoveLock, RequestContext->Irp);
      }
      DokanDeleteDcb(dcb);
      driverInfo->Status = DOKAN_START_FAILED;
      driverInfo->DeviceNumber = 0;
      driverInfo->MountId = 0;
      RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
      RequestContext->Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
      status = STATUS_SUCCESS;
      __leave;
    }

    if (useMountManager) {
      NTSTATUS lockStatus =
          IoAcquireRemoveLock(&dcb->RemoveLock, RequestContext->Irp);
      if (!NT_SUCCESS(lockStatus)) {
        status = DokanLogError(&logger, STATUS_INSUFFICIENT_RESOURCES,
                               L"Device was removed before mount point setup.");
        __leave;
      }

      if (isMountPointDriveLetter) {
        if (!dcb->MountPointDetermined) {
          // Getting into this block is considered very rare, and we are not
          // even sure how to achieve it naturally. It can be triggered
          // artificially by adding an applicable deleted volume record under
          // HKLM\System\MountedDevices.
          DokanLogError(&logger, 0,
                        L"Warning: mount point creation is being forced.");
          driverInfo->Flags |= DOKAN_DRIVER_INFO_MOUNT_FORCED;
          DokanCreateMountPoint(dcb);
          if (!dcb->MountPointDetermined) {
            DokanLogError(&logger, 0,
                          L"Mount point was still not assigned after forcing.");
            driverInfo->Status = DOKAN_START_FAILED;
            driverInfo->Flags |= DOKAN_DRIVER_INFO_NO_MOUNT_POINT_ASSIGNED;
          }
        }
      } else if (!isMountPointDriveLetter && dcb->PersistentSymbolicLinkName) {
        // Set our existing directory path as reparse point.
        // It needs to be done outside DokanTriggerVolumeMount/DokanMountVolume
        // as the MountManager will also call IoVerifyVolume on the device which
        // will lead on a deadlock while trying to acquire the MountManager
        // database.
        ULONG setReparseInputlength = 0;
        PCHAR setReparseInput = CreateSetReparsePointRequest(
            RequestContext, dcb->PersistentSymbolicLinkName,
            &setReparseInputlength);
        if (setReparseInput) {
          NTSTATUS fsctlStatus = SendDirectoryFsctl(
              RequestContext, RequestContext->DeviceObject->DriverObject,
              dcb->MountPoint, FSCTL_SET_REPARSE_POINT, setReparseInput,
              setReparseInputlength);
          DokanFree(setReparseInput);
          if (NT_SUCCESS(fsctlStatus)) {
            // Inform MountManager of the new mount point.
            NotifyDirectoryMountPointCreated(dcb);
          } else {
            DokanLogError(&logger, fsctlStatus,
                          L"Failed to set reparse point on MountPoint \"%wZ\"",
                          dcb->MountPoint);
            driverInfo->Status = DOKAN_START_FAILED;
            driverInfo->Flags |= DOKAN_DRIVER_INFO_NO_MOUNT_POINT_ASSIGNED |
                                 DOKAN_DRIVER_INFO_SET_REPARSE_POINT_FAILED;
          }
        }
      }

      IoReleaseRemoveLock(&dcb->RemoveLock, RequestContext->Irp);
    }

    if (useMountManager && isMountPointDriveLetter) {
      // Re-fetch MountEntry to read the final assigned drive letter updated by
      // IOCTL_MOUNTDEV_LINK_CREATED.
      mountEntry = FindMountEntry(RequestContext->DokanGlobal, dokanControl,
                                  /*ExclusiveLock=*/FALSE);
      if (mountEntry != NULL) {
        if (RtlCompareMemory(mountEntry->MountControl.MountPoint,
                             L"\\DosDevices\\", 24) == 24) {
          driverInfo->ActualDriveLetter =
              mountEntry->MountControl.MountPoint[12];
          DokanLogInfo(&logger, L"Returning actual mount point %c",
                       driverInfo->ActualDriveLetter);
        } else {
          DokanLogInfo(
              &logger,
              L"Warning: actual mount point %s does not have expected prefix.",
              mountEntry->MountControl.MountPoint);
        }
        ExReleaseResourceLite(&mountEntry->Resource);
      }
    }

    if (driverInfo->Flags & DOKAN_DRIVER_INFO_NO_MOUNT_POINT_ASSIGNED) {
      DokanEventRelease(RequestContext, volumeDeviceObject);
      driverInfo->DeviceNumber = 0;
      driverInfo->MountId = 0;
    }

    RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
    RequestContext->Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
    status = STATUS_SUCCESS;

    DokanLogInfo(&logger,
                 L"Finished event start with status %d and flags: %I32x",
                 driverInfo->Status, driverInfo->Flags);
    DOKAN_LOG_FINE_IRP(RequestContext,
                       "Finished event start with status %d and flags: %I32x",
                       driverInfo->Status, driverInfo->Flags);
  } __finally {
    if (foundPrevEntry != NULL) {
      ExReleaseResourceLite(&foundPrevEntry->Resource);
    }
    if (globalResourceLocked) {
      ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
      KeLeaveCriticalRegion();
    }
    if (eventStart != NULL) {
      DokanFree(eventStart);
    }
    if (baseGuidString != NULL) {
      DokanFree(baseGuidString);
    }
    if (dokanControl != NULL) {
      DokanFree(dokanControl);
    }
    if (volumeDeviceObject != NULL) {
      ObDereferenceObject(volumeDeviceObject);
    }
    if (diskDeviceObject != NULL) {
      ObDereferenceObject(diskDeviceObject);
    }
  }

  return status;
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

    writeIrp = irpEntry->RequestContext.Irp;
    if (writeIrp == NULL) {
      // this IRP has already been canceled
      RemoveEntryList(&irpEntry->ListEntry);
      DokanFreeIrpEntry(irpEntry);
      continue;
    }

    if (IoSetCancelRoutine(writeIrp, NULL) == NULL) {
      // Cancel routine will run as soon as we release the lock
      RemoveEntryList(&irpEntry->ListEntry);
      InitializeListHead(&irpEntry->ListEntry);
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

    BOOLEAN writeCanceled = FALSE;
    IoSetCancelRoutine(writeIrp, DokanIrpCancelRoutine);
    if (writeIrp->Cancel && IoSetCancelRoutine(writeIrp, NULL) != NULL) {
      RemoveEntryList(&irpEntry->ListEntry);
      InitializeListHead(&irpEntry->ListEntry);
      writeIrp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;
      writeCanceled = TRUE;
    }

    KeReleaseSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, oldIrql);

    if (writeCanceled) {
      DokanFreeIrpEntry(irpEntry);
      writeIrp->IoStatus.Information = 0;
      DokanCompleteIrpRequest(writeIrp, STATUS_CANCELLED);
    }

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
