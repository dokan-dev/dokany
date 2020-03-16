/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2017 - 2018 Google, Inc.
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
        NotificationLoop(&Dcb->PendingEvent,
                                              &Dcb->NotifyEvent);

    # PendingService has service events (ex. Unmount notification)
        # NotifyService has pending IRPs (IOCTL_SERVICE_WAIT)
    NotificationLoop(Dcb->Global->PendingService,
                          &Dcb->Global->NotifyService);

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

VOID SetCommonEventContext(__in PDokanDCB Dcb, __in PEVENT_CONTEXT EventContext,
                           __in PIRP Irp, __in_opt PDokanCCB Ccb) {
  PIO_STACK_LOCATION irpSp;

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  EventContext->MountId = Dcb->MountId;
  EventContext->MajorFunction = irpSp->MajorFunction;
  EventContext->MinorFunction = irpSp->MinorFunction;
  EventContext->Flags = irpSp->Flags;

  if (Ccb) {
    EventContext->FileFlags = DokanCCBFlagsGet(Ccb);
  }

  EventContext->ProcessId = IoGetRequestorProcessId(Irp);
}

PEVENT_CONTEXT
AllocateEventContextRaw(__in ULONG EventContextLength) {
  ULONG driverContextLength;
  PDRIVER_EVENT_CONTEXT driverEventContext;
  PEVENT_CONTEXT eventContext;

  driverContextLength =
      EventContextLength - sizeof(EVENT_CONTEXT) + sizeof(DRIVER_EVENT_CONTEXT);
  driverEventContext = ExAllocatePool(driverContextLength);

  if (driverEventContext == NULL) {
    return NULL;
  }

  RtlZeroMemory(driverEventContext, driverContextLength);
  InitializeListHead(&driverEventContext->ListEntry);

  eventContext = &driverEventContext->EventContext;
  eventContext->Length = EventContextLength;

  return eventContext;
}

PEVENT_CONTEXT
AllocateEventContext(__in PDokanDCB Dcb, __in PIRP Irp,
                     __in ULONG EventContextLength, __in_opt PDokanCCB Ccb) {
  PEVENT_CONTEXT eventContext;
  eventContext = AllocateEventContextRaw(EventContextLength);
  if (eventContext == NULL) {
    return NULL;
  }
  SetCommonEventContext(Dcb, eventContext, Irp, Ccb);
  eventContext->SerialNumber = InterlockedIncrement((LONG *)&Dcb->SerialNumber);

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

  // DDbgPrint("DokanEventNotification\n");

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
    irp = irpEntry->Irp;
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
    irp = irpEntry->Irp;
    DokanFreeIrpEntry(irpEntry);
    DokanCompleteIrpRequest(irp, STATUS_CANCELLED, 0);
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
    irp = irpEntry->Irp;
    deviceObject = irpEntry->IrpSp->DeviceObject;
    DokanFreeIrpEntry(irpEntry);
    DokanBuildRequest(deviceObject, irp);
  }
}

VOID NotificationLoop(__in PIRP_LIST PendingIrp, __in PIRP_LIST NotifyEvent) {
  PDRIVER_EVENT_CONTEXT driverEventContext;
  PLIST_ENTRY listHead;
  PIRP_ENTRY irpEntry;
  LIST_ENTRY completeList;
  KIRQL irpIrql;
  KIRQL notifyIrql;
  PIRP irp;
  ULONG eventLen;
  ULONG bufferLen;
  PVOID buffer;

  DDbgPrint("=> NotificationLoop\n");

  InitializeListHead(&completeList);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  DDbgPrint("Try acquire SpinLock...\n");
  KeAcquireSpinLock(&PendingIrp->ListLock, &irpIrql);
  DDbgPrint("SpinLock irp Acquired\n");
  KeAcquireSpinLock(&NotifyEvent->ListLock, &notifyIrql);
  DDbgPrint("SpinLock notify Acquired\n");

  while (!IsListEmpty(&PendingIrp->ListHead) &&
         !IsListEmpty(&NotifyEvent->ListHead)) {

    listHead = RemoveHeadList(&NotifyEvent->ListHead);

    driverEventContext =
        CONTAINING_RECORD(listHead, DRIVER_EVENT_CONTEXT, ListEntry);

    listHead = RemoveHeadList(&PendingIrp->ListHead);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);

    eventLen = driverEventContext->EventContext.Length;

    // ensure this eventIrp is not cancelled
    irp = irpEntry->Irp;

    if (irp == NULL) {
      // this IRP has already been canceled
      DDbgPrint("Irp canceled\n");
      ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
      DokanFreeIrpEntry(irpEntry);
      // push back
      InsertTailList(&NotifyEvent->ListHead, &driverEventContext->ListEntry);
      continue;
    }

    if (IoSetCancelRoutine(irp, NULL) == NULL) {
      DDbgPrint("IoSetCancelRoutine return NULL\n");
      // Cancel routine will run as soon as we release the lock
      InitializeListHead(&irpEntry->ListEntry);
      irpEntry->CancelRoutineFreeMemory = TRUE;
      // push back
      InsertTailList(&NotifyEvent->ListHead, &driverEventContext->ListEntry);
      continue;
    }

    // available size that is used for event notification
    bufferLen = irpEntry->IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    // buffer that is used to inform Event
    buffer = irp->AssociatedIrp.SystemBuffer;

    // buffer is not specified or short of length
    if (bufferLen == 0 || buffer == NULL || bufferLen < eventLen) {
      DDbgPrint("EventNotice : STATUS_INSUFFICIENT_RESOURCES\n");
      DDbgPrint("  bufferLen: %d, eventLen: %d\n", bufferLen, eventLen);
      // push back
      InsertTailList(&NotifyEvent->ListHead, &driverEventContext->ListEntry);
      // marks as STATUS_INSUFFICIENT_RESOURCES
      irpEntry->SerialNumber = 0;
    } else {
      // let's copy EVENT_CONTEXT
      RtlCopyMemory(buffer, &driverEventContext->EventContext, eventLen);
      // save event length
      irpEntry->SerialNumber = eventLen;

      if (driverEventContext->Completed) {
        KeSetEvent(driverEventContext->Completed, IO_NO_INCREMENT, FALSE);
      }
      ExFreePool(driverEventContext);
    }
    InsertTailList(&completeList, &irpEntry->ListEntry);
  }

  DDbgPrint("Clear Events...\n");
  KeClearEvent(&NotifyEvent->NotEmpty);
  DDbgPrint("Notify event cleared\n");
  KeClearEvent(&PendingIrp->NotEmpty);
  DDbgPrint("Pending event cleared\n");

  DDbgPrint("Release SpinLock...\n");
  KeReleaseSpinLock(&NotifyEvent->ListLock, notifyIrql);
  DDbgPrint("SpinLock notify Released\n");
  KeReleaseSpinLock(&PendingIrp->ListLock, irpIrql);
  DDbgPrint("SpinLock irp Released\n");

  while (!IsListEmpty(&completeList)) {
    listHead = RemoveHeadList(&completeList);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    irp = irpEntry->Irp;
    if (irpEntry->SerialNumber == 0) {
      irp->IoStatus.Information = 0;
      irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
      irp->IoStatus.Information = irpEntry->SerialNumber;
      irp->IoStatus.Status = STATUS_SUCCESS;
    }
    DokanFreeIrpEntry(irpEntry);
    DokanCompleteIrpRequest(irp, irp->IoStatus.Status,
                            irp->IoStatus.Information);
  }

  DDbgPrint("<= NotificationLoop\n");
}

KSTART_ROUTINE NotificationThread;
VOID NotificationThread(__in PVOID pDcb) {
  PKEVENT events[6];
  PKWAIT_BLOCK waitBlock;
  NTSTATUS status;
  PDokanDCB Dcb = pDcb;

  DDbgPrint("==> NotificationThread\n");

  waitBlock = ExAllocatePool(sizeof(KWAIT_BLOCK) * 6);
  if (waitBlock == NULL) {
    DDbgPrint("  Can't allocate WAIT_BLOCK\n");
    return;
  }
  events[0] = &Dcb->ReleaseEvent;
  events[1] = &Dcb->NotifyEvent.NotEmpty;
  events[2] = &Dcb->PendingEvent.NotEmpty;
  events[3] = &Dcb->Global->PendingService.NotEmpty;
  events[4] = &Dcb->Global->NotifyService.NotEmpty;
  events[5] = &Dcb->PendingRetryIrp.NotEmpty;
  do {
    status = KeWaitForMultipleObjects(6, events, WaitAny, Executive, KernelMode,
                                      FALSE, NULL, waitBlock);

    if (status != STATUS_WAIT_0) {
      if (status == STATUS_WAIT_1 || status == STATUS_WAIT_2) {
        NotificationLoop(&Dcb->PendingEvent, &Dcb->NotifyEvent);
      } else if (status == STATUS_WAIT_0 + 3 || status == STATUS_WAIT_0 + 4) {
        NotificationLoop(&Dcb->Global->PendingService,
                         &Dcb->Global->NotifyService);
      } else {
        RetryIrps(&Dcb->PendingRetryIrp);
      }
    }
  } while (status != STATUS_WAIT_0);

  ExFreePool(waitBlock);
  DDbgPrint("<== NotificationThread\n");
}

NTSTATUS
DokanStartEventNotificationThread(__in PDokanDCB Dcb) {
  NTSTATUS status;
  HANDLE thread;

  DDbgPrint("==> DokanStartEventNotificationThread\n");

  KeResetEvent(&Dcb->ReleaseEvent);

  status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                (PKSTART_ROUTINE)NotificationThread, Dcb);

  if (!NT_SUCCESS(status)) {
    return status;
  }

  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode,
                            (PVOID *)&Dcb->EventNotificationThread, NULL);

  ZwClose(thread);

  DDbgPrint("<== DokanStartEventNotificationThread\n");

  return STATUS_SUCCESS;
}

VOID DokanStopEventNotificationThread(__in PDokanDCB Dcb) {
  DDbgPrint("==> DokanStopEventNotificationThread\n");

  if (KeSetEvent(&Dcb->ReleaseEvent, 0, FALSE) > 0 &&
      Dcb->EventNotificationThread) {
    DDbgPrint("Waiting for Notify thread to terminate.\n");
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    if (Dcb->EventNotificationThread) {
      KeWaitForSingleObject(Dcb->EventNotificationThread, Executive, KernelMode,
                            FALSE, NULL);
      DDbgPrint("Notify thread successfully terminated.\n");
      ObDereferenceObject(Dcb->EventNotificationThread);
      Dcb->EventNotificationThread = NULL;
    }
  }
  DDbgPrint("<== DokanStopEventNotificationThread\n");
}

NTSTATUS DokanEventRelease(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PDokanDCB dcb;
  PDokanVCB vcb;
  PDokanFCB fcb;
  PDokanCCB ccb;
  PLIST_ENTRY fcbEntry, fcbNext, fcbHead;
  PLIST_ENTRY ccbEntry, ccbNext, ccbHead;
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

  status = IoAcquireRemoveLock(&dcb->RemoveLock, Irp);
  if (!NT_SUCCESS(status)) {
    DokanLogError(&logger, status, L"IoAcquireRemoveLock failed in release.");
    return STATUS_DEVICE_REMOVED;
  }

  // as first delete the mountpoint
  // in case of MountManager some request because of delete
  // must be handled properly
  DokanDeleteMountPoint(dcb);

  // then mark the device for unmount pending
  SetLongFlag(vcb->Flags, VCB_DISMOUNT_PENDING);
  SetLongFlag(dcb->Flags, DCB_DELETE_PENDING);

  DokanLogInfo(&logger, L"Starting unmount for device %wZ",
               dcb->DiskDeviceName);

  ReleasePendingIrp(&dcb->PendingIrp);
  ReleasePendingIrp(&dcb->PendingEvent);
  ReleasePendingIrp(&dcb->PendingRetryIrp);
  DokanStopCheckThread(dcb);
  DokanStopEventNotificationThread(dcb);

  ClearLongFlag(vcb->Flags, VCB_MOUNTED);

  // search CCB list to complete not completed Directory Notification

  DokanVCBLockRW(vcb);

  fcbHead = &vcb->NextFCB;

  for (fcbEntry = fcbHead->Flink; fcbEntry != fcbHead; fcbEntry = fcbNext) {

    fcbNext = fcbEntry->Flink;
    fcb = CONTAINING_RECORD(fcbEntry, DokanFCB, NextFCB);
    DokanFCBLockRW(fcb);

    ccbHead = &fcb->NextCCB;

    for (ccbEntry = ccbHead->Flink; ccbEntry != ccbHead; ccbEntry = ccbNext) {
      ccbNext = ccbEntry->Flink;
      ccb = CONTAINING_RECORD(ccbEntry, DokanCCB, NextCCB);

      DDbgPrint("  NotifyCleanup ccb:%p, context:%X, filename:%wZ\n", ccb,
                (ULONG)ccb->UserContext, &fcb->FileName);
      FsRtlNotifyCleanup(vcb->NotifySync, &vcb->DirNotifyList, ccb);
    }
    DokanFCBUnlock(fcb);
  }

  DokanVCBUnlock(vcb);

  IoReleaseRemoveLockAndWait(&dcb->RemoveLock, Irp);

  DokanDeleteDeviceObject(dcb);

  DokanLogInfo(&logger, L"Finished event release.");

  return status;
}

ULONG GetCurrentSessionId(__in PIRP Irp) {
  ULONG sessionNumber;
  NTSTATUS status;

  status = IoGetRequestorSessionId(Irp, &sessionNumber);
  if (!NT_SUCCESS(status)) {
    DDbgPrint("   IoGetRequestorSessionId failed\n");
    return (ULONG)-1;
  }
  DDbgPrint("   GetCurrentSessionId %lu\n", sessionNumber);
  return sessionNumber;
}

NTSTATUS DokanGlobalEventRelease(__in PDEVICE_OBJECT DeviceObject,
                                 __in PIRP Irp) {
  PDOKAN_GLOBAL dokanGlobal;
  PIO_STACK_LOCATION irpSp;
  PDOKAN_UNICODE_STRING_INTERMEDIATE szMountPoint;
  DOKAN_CONTROL dokanControl;
  PMOUNT_ENTRY mountEntry;

  dokanGlobal = DeviceObject->DeviceExtension;
  if (GetIdentifierType(dokanGlobal) != DGL) {
    return STATUS_INVALID_PARAMETER;
  }

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  if (irpSp->Parameters.DeviceIoControl.InputBufferLength <
      sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE)) {
    DDbgPrint(
        "Input buffer is too small (< DOKAN_UNICODE_STRING_INTERMEDIATE)\n");
    return STATUS_BUFFER_TOO_SMALL;
  }
  szMountPoint =
      (PDOKAN_UNICODE_STRING_INTERMEDIATE)Irp->AssociatedIrp.SystemBuffer;
  if (irpSp->Parameters.DeviceIoControl.InputBufferLength <
      sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) + szMountPoint->MaximumLength) {
    DDbgPrint("Input buffer is too small\n");
    return STATUS_BUFFER_TOO_SMALL;
  }

  RtlZeroMemory(&dokanControl, sizeof(dokanControl));
  RtlStringCchCopyW(dokanControl.MountPoint, MAXIMUM_FILENAME_LENGTH,
                    L"\\DosDevices\\");
  if ((szMountPoint->Length / sizeof(WCHAR)) < 4) {
    dokanControl.MountPoint[12] = towupper(szMountPoint->Buffer[0]);
    dokanControl.MountPoint[13] = L':';
    dokanControl.MountPoint[14] = L'\0';
  } else {
    if ((szMountPoint->Length / sizeof(WCHAR) + 12) > MAX_PATH) {
      DDbgPrint("Montpoint Buffer has an invalid size\n");
      return STATUS_BUFFER_OVERFLOW;
	}
    RtlCopyMemory(&dokanControl.MountPoint[12], szMountPoint->Buffer,
                  szMountPoint->Length);
  }

  dokanControl.SessionId = GetCurrentSessionId(Irp);
  mountEntry = FindMountEntry(dokanGlobal, &dokanControl, TRUE);
  if (mountEntry == NULL) {
    dokanControl.SessionId = (ULONG)-1;
    DDbgPrint("Cannot found device associated to mount point %ws\n",
              dokanControl.MountPoint);
    return STATUS_BUFFER_TOO_SMALL;
  }

  if (IsDeletePending(mountEntry->MountControl.DeviceObject)) {
    DDbgPrint("Device is deleted\n") return STATUS_DEVICE_REMOVED;
  }

  if (!IsMounted(mountEntry->MountControl.DeviceObject)) {
    DDbgPrint("Device is still not mounted, so an unmount not possible at this "
              "point\n") return STATUS_DEVICE_BUSY;
  }

  return DokanEventRelease(mountEntry->MountControl.DeviceObject, Irp);
}
