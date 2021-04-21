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

/*

IOCTL_EVENT_START:
DokanStartEventNotificationThread
  NotificationThread
        # PendingEvent has pending IPRs (IOCTL_EVENT_WAIT)
    # NotifyEvent has IO events (ex.IRP_MJ_READ)
    # notify NotifyEvent using PendingEvent in this loop
        NotificationLoop(&Dcb->PendingEvent, &Dcb->NotifyEvent);

IOCTL_EVENT_RELEASE:
DokanStopEventNotificationThread

IRP_MJ_READ:
DokanDispatchRead
  DokanRegisterPendingIrp
    # add IRP_MJ_READ to PendingIrp list
    DokanRegisterPendingIrpMain(PendingIrp)
        # put MJ_READ event into NotifyEvent
    DokanEventNotification(NotifyEvent, EventContext)

IOCTL_EVENT_WAIT:
  DokanRegisterPendingIrpForEvent
    # add this irp to PendingEvent list
    DokanRegisterPendingIrpMain(PendingEvent)

IOCTL_EVENT_INFO:
  DokanCompleteIrp
    DokanCompleteRead

*/

#include "dokan.h"
#include "util/irp_buffer_helper.h"

VOID SetCommonEventContext(__in PREQUEST_CONTEXT RequestContext,
                           __in PEVENT_CONTEXT EventContext,
                           __in_opt PDokanCCB Ccb) {
  EventContext->MountId = RequestContext->Dcb->MountId;
  EventContext->MajorFunction = RequestContext->IrpSp->MajorFunction;
  EventContext->MinorFunction = RequestContext->IrpSp->MinorFunction;
  EventContext->Flags = RequestContext->IrpSp->Flags;

  if (Ccb) {
    EventContext->FileFlags = DokanCCBFlagsGet(Ccb);
  }

  EventContext->ProcessId = RequestContext->ProcessId;
}

PEVENT_CONTEXT
AllocateEventContextRaw(__in ULONG EventContextLength) {
  ULONG driverContextLength;
  PDRIVER_EVENT_CONTEXT driverEventContext;
  PEVENT_CONTEXT eventContext;

  if (EventContextLength < sizeof(EVENT_CONTEXT) ||
      EventContextLength > MAXULONG - sizeof(DRIVER_EVENT_CONTEXT)) {
    DOKAN_LOG("Invalid EventContextLength requested.");
    return NULL;
  }

  driverContextLength =
      EventContextLength - sizeof(EVENT_CONTEXT) + sizeof(DRIVER_EVENT_CONTEXT);
  driverEventContext = DokanAllocZero(driverContextLength);
  if (driverEventContext == NULL) {
    return NULL;
  }

  InitializeListHead(&driverEventContext->ListEntry);

  eventContext = &driverEventContext->EventContext;
  eventContext->Length = EventContextLength;

  return eventContext;
}

PEVENT_CONTEXT
AllocateEventContext(__in PREQUEST_CONTEXT RequestContext,
                     __in ULONG EventContextLength, __in_opt PDokanCCB Ccb) {
  PEVENT_CONTEXT eventContext;
  eventContext = AllocateEventContextRaw(EventContextLength);
  if (eventContext == NULL) {
    return NULL;
  }
  SetCommonEventContext(RequestContext, eventContext, Ccb);
  return eventContext;
}

VOID DokanFreeEventContext(__in PEVENT_CONTEXT EventContext) {
  PDRIVER_EVENT_CONTEXT driverEventContext =
      CONTAINING_RECORD(EventContext, DRIVER_EVENT_CONTEXT, EventContext);
  ExFreePool(driverEventContext);
}

VOID DokanEventNotification(__in PIRP_LIST NotifyEvent,
                            __in PEVENT_CONTEXT EventContext) {
  PDRIVER_EVENT_CONTEXT driverEventContext =
      CONTAINING_RECORD(EventContext, DRIVER_EVENT_CONTEXT, EventContext);

  InitializeListHead(&driverEventContext->ListEntry);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

  ExInterlockedInsertTailList(&NotifyEvent->ListHead,
                              &driverEventContext->ListEntry,
                              &NotifyEvent->ListLock);

  KeSetEvent(&NotifyEvent->NotEmpty, IO_NO_INCREMENT, FALSE);
}

// Moves the contents of the given Source list to Dest, discarding IRPs that
// have been canceled while waiting in the list. The IRPs that end up in Dest
// should then be acted on in some way that leads to their completion. The
// Source list is still usable and is empty after this function returns.
VOID MoveIrpList(__in PIRP_LIST Source, __out LIST_ENTRY* Dest) {
  PLIST_ENTRY listHead;
  PIRP_ENTRY irpEntry;
  KIRQL oldIrql;
  PIRP irp;

  InitializeListHead(Dest);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&Source->ListLock, &oldIrql);

  while (!IsListEmpty(&Source->ListHead)) {
    listHead = RemoveHeadList(&Source->ListHead);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    irp = irpEntry->RequestContext.Irp;
    if (irp == NULL) {
      // this IRP has already been canceled
      ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
      DokanFreeIrpEntry(irpEntry);
      continue;
    }

    if (IoSetCancelRoutine(irp, NULL) == NULL) {
      // Cancel routine will run as soon as we release the lock
      InitializeListHead(&irpEntry->ListEntry);
      irpEntry->CancelRoutineFreeMemory = TRUE;
      continue;
    }
    InsertTailList(Dest, &irpEntry->ListEntry);
  }

  KeClearEvent(&Source->NotEmpty);
  KeReleaseSpinLock(&Source->ListLock, oldIrql);
}

VOID ReleasePendingIrp(__in PIRP_LIST PendingIrp) {
  PLIST_ENTRY listHead;
  LIST_ENTRY completeList;
  PIRP_ENTRY irpEntry;
  PIRP irp;

  MoveIrpList(PendingIrp, &completeList);
  while (!IsListEmpty(&completeList)) {
    listHead = RemoveHeadList(&completeList);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    irp = irpEntry->RequestContext.Irp;
    DokanFreeIrpEntry(irpEntry);
    irp->IoStatus.Information = 0;
    DokanCompleteIrpRequest(irp, STATUS_CANCELLED);
  }
}

VOID ReleaseNotifyEvent(__in PIRP_LIST NotifyEvent) {
  PDRIVER_EVENT_CONTEXT driverEventContext;
  PLIST_ENTRY listHead;
  KIRQL oldIrql;

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&NotifyEvent->ListLock, &oldIrql);

  while (!IsListEmpty(&NotifyEvent->ListHead)) {
    listHead = RemoveHeadList(&NotifyEvent->ListHead);
    driverEventContext =
        CONTAINING_RECORD(listHead, DRIVER_EVENT_CONTEXT, ListEntry);
    ExFreePool(driverEventContext);
  }

  KeClearEvent(&NotifyEvent->NotEmpty);
  KeReleaseSpinLock(&NotifyEvent->ListLock, oldIrql);
}

VOID RetryIrps(__in PIRP_LIST PendingRetryIrp) {
  PLIST_ENTRY listHead;
  LIST_ENTRY retryList;
  PIRP_ENTRY irpEntry;
  PIRP irp;
  PDEVICE_OBJECT deviceObject = NULL;

  MoveIrpList(PendingRetryIrp, &retryList);
  while (!IsListEmpty(&retryList)) {
    listHead = RemoveHeadList(&retryList);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    irp = irpEntry->RequestContext.Irp;
    deviceObject = irpEntry->RequestContext.DeviceObject;
    DokanFreeIrpEntry(irpEntry);
    DokanBuildRequest(deviceObject, irp);
  }
}

// Called whenever we detect that we are ready to send some I/O traffic to the
// user mode DLL/service. The user mode component fetches work from the kernel
// by doing DeviceIoControl(IOCTL_EVENT_WAIT, buffer, size) invocations in one
// or more loops, depending on how it is configured. Each pending
// DeviceIoControl becomes an IRP in PendingIoctls.
//
// When the driver receives an incoming I/O request (e.g. from an app) that it
// can't process without involving the user mode code, that becomes an element
// in WorkQueue. Under heavy load, WorkQueue may reach a size of 2 to 10 or more
// in between each IOCTL from the DLL/service, but traditionally each IOCTL only
// pulls one request out of the queue, which is inefficient. If AllowBatching is
// TRUE then each pending IOCTL can get its buffer packed with concatenated
// WorkQueue items.
//
// This function consumes the actual lists as well, to the extent that it is
// able to send out work items, so it is expected that the WorkQueue items are
// also in another list where they can later be looked up at completion time.
VOID NotificationLoop(__in PIRP_LIST PendingIoctls, __in PIRP_LIST WorkQueue,
                      __in BOOLEAN AllowBatching) {
  PDRIVER_EVENT_CONTEXT workItem = NULL;
  PLIST_ENTRY workItemListEntry = NULL;
  PLIST_ENTRY currentIoctlListEntry = NULL;
  PIRP_ENTRY currentIoctlIrpEntry = NULL;
  LIST_ENTRY completedIoctls;
  KIRQL pendingIoctlsIrql;
  KIRQL workQueueIrql;
  PIRP currentIoctl = NULL;
  ULONG workItemBytes = 0;
  ULONG currentIoctlBufferBytesRemaining = 0;
  PCHAR currentIoctlBuffer = NULL;

  InitializeListHead(&completedIoctls);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&PendingIoctls->ListLock, &pendingIoctlsIrql);
  KeAcquireSpinLock(&WorkQueue->ListLock, &workQueueIrql);
  while (!IsListEmpty(&WorkQueue->ListHead)) {
    if (!AllowBatching || currentIoctl == NULL) {
      if (IsListEmpty(&PendingIoctls->ListHead)) {
        break;
      }
      currentIoctlListEntry = RemoveHeadList(&PendingIoctls->ListHead);
      currentIoctlIrpEntry = CONTAINING_RECORD(currentIoctlListEntry, IRP_ENTRY,
                                               ListEntry);
      currentIoctl = currentIoctlIrpEntry->RequestContext.Irp;
      InsertTailList(&completedIoctls, &currentIoctlIrpEntry->ListEntry);
      // The buffer we are sending back to user mode for this IOCTL_EVENT_WAIT.
      currentIoctlBuffer = (PCHAR)currentIoctl->AssociatedIrp.SystemBuffer;
      currentIoctlBufferBytesRemaining = currentIoctlIrpEntry->RequestContext.IrpSp->Parameters
              .DeviceIoControl.OutputBufferLength;

      // Ensure this IRP is not cancelled.
      if (currentIoctl == NULL) {
        ASSERT(currentIoctlIrpEntry->CancelRoutineFreeMemory == FALSE);
        DokanFreeIrpEntry(currentIoctlIrpEntry);
        continue;
      }
      if (IoSetCancelRoutine(currentIoctl, NULL) == NULL) {
        // Cancel routine will run as soon as we release the lock
        InitializeListHead(&currentIoctlIrpEntry->ListEntry);
        currentIoctlIrpEntry->CancelRoutineFreeMemory = TRUE;
        currentIoctl = NULL;
        continue;
      }
      // The serial number gets re-purposed as the amount of the DLL's buffer
      // that has been filled, for historical reasons. We increment this while
      // filling the buffer below, unless nothing fits in it; then we send
      // an error to the DLL.
      currentIoctlIrpEntry->SerialNumber = 0;
    }

    workItemListEntry = RemoveHeadList(&WorkQueue->ListHead);
    workItem = CONTAINING_RECORD(workItemListEntry, DRIVER_EVENT_CONTEXT,
                                 ListEntry);
    workItemBytes = workItem->EventContext.Length;
    // Buffer is not specified or short of length (this may mean we filled the
    // space in one of the DLL's buffers in batch mode). Put the IRP back in
    // the work queue; it will have to go in a different buffer.
    if (currentIoctlBuffer == NULL
        || currentIoctlBufferBytesRemaining < workItemBytes) {
      InsertTailList(&WorkQueue->ListHead, &workItem->ListEntry);
      currentIoctl = NULL;
      continue;
    }
    // Send the work item back in the response to the current IOCTL.
    RtlCopyMemory(currentIoctlBuffer, &workItem->EventContext, workItemBytes);
    currentIoctlBufferBytesRemaining -= workItemBytes;
    currentIoctlBuffer += workItemBytes;
    currentIoctlIrpEntry->SerialNumber += workItemBytes;
    if (workItem->Completed) {
      KeSetEvent(workItem->Completed, IO_NO_INCREMENT, FALSE);
    }
    ExFreePool(workItem);
  }

  KeClearEvent(&WorkQueue->NotEmpty);
  KeClearEvent(&PendingIoctls->NotEmpty);
  KeReleaseSpinLock(&WorkQueue->ListLock, workQueueIrql);
  KeReleaseSpinLock(&PendingIoctls->ListLock, pendingIoctlsIrql);

  // Go through the motions of making the appropriate DeviceIoControl requests
  // from the DLL/service actually finish.
  while (!IsListEmpty(&completedIoctls)) {
    currentIoctlListEntry = RemoveHeadList(&completedIoctls);
    currentIoctlIrpEntry = CONTAINING_RECORD(currentIoctlListEntry, IRP_ENTRY,
                                             ListEntry);
    currentIoctl = currentIoctlIrpEntry->RequestContext.Irp;
    if (currentIoctlIrpEntry->SerialNumber == 0) {
      currentIoctl->IoStatus.Information = 0;
      currentIoctl->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
      // This is not the serial number but the aomunt of data written to the
      // DLL's return buffer.
      currentIoctl->IoStatus.Information = currentIoctlIrpEntry->SerialNumber;
      currentIoctl->IoStatus.Status = STATUS_SUCCESS;
    }
    DokanFreeIrpEntry(currentIoctlIrpEntry);
    DokanCompleteIrpRequest(currentIoctl, currentIoctl->IoStatus.Status);
  }
}

KSTART_ROUTINE NotificationThread;
VOID NotificationThread(__in PVOID pDcb) {
  PKEVENT events[4];
  PKWAIT_BLOCK waitBlock;
  NTSTATUS status;
  PDokanDCB Dcb = pDcb;

  DOKAN_LOG("Start");

  waitBlock = DokanAlloc(sizeof(KWAIT_BLOCK) * 6);
  if (waitBlock == NULL) {
    DOKAN_LOG("Can't allocate WAIT_BLOCK");
    return;
  }
  events[0] = &Dcb->ReleaseEvent;
  events[1] = &Dcb->NotifyEvent.NotEmpty;
  events[2] = &Dcb->PendingEvent.NotEmpty;
  events[3] = &Dcb->PendingRetryIrp.NotEmpty;
  do {
    status = KeWaitForMultipleObjects(4, events, WaitAny, Executive, KernelMode,
                                      FALSE, NULL, waitBlock);

    if (status != STATUS_WAIT_0) {
      if (status == STATUS_WAIT_1 || status == STATUS_WAIT_2) {
        NotificationLoop(&Dcb->PendingEvent, &Dcb->NotifyEvent,
                         Dcb->AllowIpcBatching);
      } else {
        RetryIrps(&Dcb->PendingRetryIrp);
      }
    }
  } while (status != STATUS_WAIT_0);

  ExFreePool(waitBlock);
  DOKAN_LOG("Stop");
}

NTSTATUS
DokanStartEventNotificationThread(__in PDokanDCB Dcb) {
  NTSTATUS status;
  HANDLE thread;

  KeResetEvent(&Dcb->ReleaseEvent);

  status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                (PKSTART_ROUTINE)NotificationThread, Dcb);

  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("Failed to create Thread %s", DokanGetNTSTATUSStr(status));
    return status;
  }

  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode,
                            (PVOID *)&Dcb->EventNotificationThread, NULL);

  ZwClose(thread);

  return STATUS_SUCCESS;
}

VOID DokanStopEventNotificationThread(__in PDokanDCB Dcb) {
  DOKAN_LOG("Stopping Thread");
  if (KeSetEvent(&Dcb->ReleaseEvent, 0, FALSE) > 0 &&
      Dcb->EventNotificationThread) {
    DOKAN_LOG("Waiting for thread to terminate");
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    if (Dcb->EventNotificationThread) {
      KeWaitForSingleObject(Dcb->EventNotificationThread, Executive, KernelMode,
                            FALSE, NULL);
      DOKAN_LOG("Thread successfully terminated");
      ObDereferenceObject(Dcb->EventNotificationThread);
      Dcb->EventNotificationThread = NULL;
    }
  }
}

VOID DokanCleanupAllChangeNotificationWaiters(__in PDokanVCB Vcb) {
  DokanVCBLockRW(Vcb);
  DOKAN_INIT_LOGGER(logger, Vcb->Dcb->DeviceObject->DriverObject, 0);
  DokanLogInfo(&logger, L"Cleaning up all change notification waiters.");
  FsRtlNotifyCleanupAll(Vcb->NotifySync, &Vcb->DirNotifyList);
  DokanVCBUnlock(Vcb);
}

VOID DokanStopFcbGarbageCollectorThread(__in PDokanVCB Vcb) {
  if (Vcb->FcbGarbageCollectorThread != NULL) {
    KeWaitForSingleObject(Vcb->FcbGarbageCollectorThread, Executive, KernelMode,
                          FALSE, NULL);
    ObDereferenceObject(Vcb->FcbGarbageCollectorThread);
    Vcb->FcbGarbageCollectorThread = NULL;
  }
}

NTSTATUS DokanEventRelease(__in_opt PREQUEST_CONTEXT RequestContext,
                           __in PDEVICE_OBJECT DeviceObject) {
  PDokanDCB dcb;
  PDokanVCB vcb;
  NTSTATUS status = STATUS_SUCCESS;
  DOKAN_INIT_LOGGER(logger,
                    DeviceObject == NULL ? NULL
                                         : DeviceObject->DriverObject,
                    0);

  if (DeviceObject == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  DokanLogInfo(&logger, L"Entered event release.");

  vcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(vcb) != VCB) {
    return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                         L"VCB being released has wrong identifier type.");
  }
  dcb = vcb->Dcb;

  if (IsDeletePending(dcb->DeviceObject)) {
    DokanLogInfo(&logger, L"Event release is already running for this device.");
    return STATUS_SUCCESS;
  }

  if (IsUnmountPendingVcb(vcb)) {
    DokanLogInfo(&logger, L"Event release is already running for this volume.");
    return STATUS_SUCCESS;
  }

  status = IoAcquireRemoveLock(&dcb->RemoveLock, RequestContext);
  if (!NT_SUCCESS(status)) {
    DokanLogError(&logger, status, L"IoAcquireRemoveLock failed in release.");
    return STATUS_DEVICE_REMOVED;
  }

  // as first delete the mountpoint
  // in case of MountManager some request because of delete
  // must be handled properly
  DokanDeleteMountPoint(RequestContext, dcb);

  // then mark the device for unmount pending
  SetLongFlag(vcb->Flags, VCB_DISMOUNT_PENDING);
  SetLongFlag(dcb->Flags, DCB_DELETE_PENDING);

  DokanLogInfo(&logger, L"Starting unmount for device \"%wZ\"",
                        dcb->DiskDeviceName);

  ReleasePendingIrp(&dcb->PendingIrp);
  ReleasePendingIrp(&dcb->PendingEvent);
  ReleasePendingIrp(&dcb->PendingRetryIrp);
  DokanStopCheckThread(dcb);
  DokanStopEventNotificationThread(dcb);

  // Note that the garbage collector thread also gets signalled to stop by
  // DokanStopEventNotificationThread. TODO(drivefs-team): maybe seperate out
  // the signal to stop.
  DokanStopFcbGarbageCollectorThread(vcb);
  ClearLongFlag(vcb->Flags, VCB_MOUNTED);

  DokanCleanupAllChangeNotificationWaiters(vcb);
  IoReleaseRemoveLockAndWait(&dcb->RemoveLock, RequestContext);

  DokanDeleteDeviceObject(RequestContext, dcb);

  DokanLogInfo(&logger, L"Finished event release.");

  return status;
}

ULONG GetCurrentSessionId(__in PREQUEST_CONTEXT RequestContext) {
  ULONG sessionNumber;
  NTSTATUS status;

  status = IoGetRequestorSessionId(RequestContext->Irp, &sessionNumber);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Failed %s",
                       DokanGetNTSTATUSStr(status));
    return (ULONG)-1;
  }
  DOKAN_LOG_FINE_IRP(RequestContext, "%lu", sessionNumber);
  return sessionNumber;
}

NTSTATUS DokanGlobalEventRelease(__in PREQUEST_CONTEXT RequestContext) {
  PDOKAN_UNICODE_STRING_INTERMEDIATE szMountPoint;
  DOKAN_CONTROL dokanControl;
  PMOUNT_ENTRY mountEntry;

  GET_IRP_UNICODE_STRING_INTERMEDIATE_OR_RETURN(RequestContext->Irp,
                                                szMountPoint);

  RtlZeroMemory(&dokanControl, sizeof(DOKAN_CONTROL));
  RtlStringCchCopyW(dokanControl.MountPoint, MAXIMUM_FILENAME_LENGTH,
                    L"\\DosDevices\\");
  if ((szMountPoint->Length / sizeof(WCHAR)) < 4) {
    dokanControl.MountPoint[12] = towupper(szMountPoint->Buffer[0]);
    dokanControl.MountPoint[13] = L':';
    dokanControl.MountPoint[14] = L'\0';
  } else {
    if (szMountPoint->Length >
        sizeof(dokanControl.MountPoint) - 12 * sizeof(WCHAR)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Mount point buffer has invalid size");
      return STATUS_BUFFER_OVERFLOW;
    }
    RtlCopyMemory(&dokanControl.MountPoint[12], szMountPoint->Buffer,
                  szMountPoint->Length);
  }

  dokanControl.SessionId = GetCurrentSessionId(RequestContext);
  mountEntry = FindMountEntry(RequestContext->DokanGlobal, &dokanControl, TRUE);
  if (mountEntry == NULL) {
    dokanControl.SessionId = (ULONG)-1;
    DOKAN_LOG_FINE_IRP(RequestContext, "Cannot found device associated to mount point %ws",
                  dokanControl.MountPoint);
    return STATUS_BUFFER_TOO_SMALL;
  }

  if (IsDeletePending(mountEntry->MountControl.VolumeDeviceObject)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Device is deleted");
    return STATUS_DEVICE_REMOVED;
  }

  if (!IsMounted(mountEntry->MountControl.VolumeDeviceObject)) {
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "Device is still not mounted, so an unmount not possible at this "
        "point");
    return STATUS_DEVICE_BUSY;
  }

  return DokanEventRelease(RequestContext,
                           mountEntry->MountControl.VolumeDeviceObject);
}
