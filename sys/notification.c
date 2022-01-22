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
IRP_MJ_READ:
DokanDispatchRead
  DokanRegisterPendingIrp
    # add IRP_MJ_READ to PendingIrp list
    RegisterPendingIrpMain(PendingIrp)
    # put MJ_READ event into NotifyEvent
    DokanEventNotification(NotifyEvent, EventContext)

FSCTL_EVENT_PROCESS_N_PULL:
  DokanProcessAndPullEvents
    # Pull the previously registered event
    PullEvents(NotifyEvent)

FSCTL_EVENT_PROCESS_N_PULL:
  DokanProcessAndPullEvents
    # Complete the IRP process by userland
    DokanCompleteIrp
    # Pull the new registered event
    PullEvents(NotifyEvent)
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

VOID DokanEventNotification(__in PREQUEST_CONTEXT RequestContext,
                            __in PIRP_LIST NotifyEvent,
                            __in PEVENT_CONTEXT EventContext) {
  PDRIVER_EVENT_CONTEXT driverEventContext =
      CONTAINING_RECORD(EventContext, DRIVER_EVENT_CONTEXT, EventContext);

  InitializeListHead(&driverEventContext->ListEntry);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

  KIRQL oldIrql;
  KeAcquireSpinLock(&NotifyEvent->ListLock, &oldIrql);
  InsertTailList(&NotifyEvent->ListHead, &driverEventContext->ListEntry);
  if (!KeReadStateQueue(&RequestContext->Dcb->NotifyIrpEventQueue)) {
    KeInsertQueue(&RequestContext->Dcb->NotifyIrpEventQueue,
                  &RequestContext->Dcb->NotifyIrpEventQueueList);
  }
  KeReleaseSpinLock(&NotifyEvent->ListLock, oldIrql);
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

KSTART_ROUTINE NotificationThread;
VOID NotificationThread(__in PVOID pDcb) {
  PKEVENT events[2];
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
  events[1] = &Dcb->PendingRetryIrp.NotEmpty;
  do {
    status = KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode,
                                      FALSE, NULL, waitBlock);
    if (status == STATUS_WAIT_1) {
      RetryIrps(&Dcb->PendingRetryIrp);
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
  ReleasePendingIrp(&dcb->PendingRetryIrp);
  ReleaseNotifyEvent(&dcb->NotifyEvent);
  DokanStopCheckThread(dcb);
  DokanStopEventNotificationThread(dcb);
  KeRundownQueue(&dcb->NotifyIrpEventQueue);

  // Note that the garbage collector thread also gets signalled to stop by
  // DokanStopEventNotificationThread. TODO(drivefs-team): maybe seperate out
  // the signal to stop.
  DokanStopFcbGarbageCollectorThread(vcb);
  ClearLongFlag(vcb->Flags, VCB_MOUNTED);

  if (vcb->FCBAvlNodeLookasideListInit) {
    ExDeleteLookasideListEx(&vcb->FCBAvlNodeLookasideList);
  }
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
    DOKAN_LOG_FINE_IRP(RequestContext, "IoGetRequestorSessionId failed %s",
                       DokanGetNTSTATUSStr(status));
    return (ULONG)-1;
  }
  DOKAN_LOG_FINE_IRP(RequestContext, "Session number: %lu", sessionNumber);
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
