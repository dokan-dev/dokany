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
#include "util/fcb.h"
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
  DokanFree(driverEventContext);
}

VOID DokanFreeWriteEventContext(__in PIRP Irp) {
  PVOID eventContext = Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT];
  if (eventContext != NULL) {
    DokanFreeEventContext((PEVENT_CONTEXT)eventContext);
    Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = NULL;
  }
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
  if (InterlockedCompareExchange(&RequestContext->Dcb->Condition, 0, 0) !=
          DcbActive ||
      (RequestContext->Vcb != NULL &&
       IsUnmountPendingVcb(RequestContext->Vcb))) {
    KeReleaseSpinLock(&NotifyEvent->ListLock, oldIrql);
    DokanFree(driverEventContext);
    return;
  }
  InsertTailList(&NotifyEvent->ListHead, &driverEventContext->ListEntry);
  KeSetEvent(&NotifyEvent->NotEmpty, IO_NO_INCREMENT, FALSE);
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

  InitializeListHead(Dest);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&Source->ListLock, &oldIrql);

  while (!IsListEmpty(&Source->ListHead)) {
    listHead = Source->ListHead.Flink;
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    if (DokanPopIrpEntryLocked(irpEntry)) {
      InsertTailList(Dest, &irpEntry->ListEntry);
    }
  }

  KeClearEvent(&Source->NotEmpty);
  KeReleaseSpinLock(&Source->ListLock, oldIrql);
}

VOID DokanCancelPendingIrp(__in PREQUEST_CONTEXT RequestContext,
                           __in NTSTATUS Status) {
  if (RequestContext->IrpSp->MajorFunction == IRP_MJ_CREATE &&
      RequestContext->IrpSp->FileObject != NULL &&
      RequestContext->IrpSp->FileObject->FsContext2 != NULL) {
    DokanCancelCreateIrp(RequestContext, Status);
  } else {
    if (RequestContext->IrpSp->MajorFunction == IRP_MJ_CLEANUP) {
      DokanExecuteCleanup(RequestContext);
      // Windows FSDs must never fail IRP_MJ_CLEANUP once local handle state
      // has been torn down (see DokanDispatchCleanup).
      Status = STATUS_SUCCESS;
    } else if (RequestContext->IrpSp->MajorFunction == IRP_MJ_WRITE) {
      DokanFreeWriteEventContext(RequestContext->Irp);
    }
    RequestContext->Irp->IoStatus.Information = 0;
    DokanCompleteIrpRequest(RequestContext->Irp, Status);
  }
}

VOID ReleasePendingIrp(__in PIRP_LIST PendingIrp) {
  PLIST_ENTRY listHead;
  LIST_ENTRY completeList;
  PIRP_ENTRY irpEntry;
  REQUEST_CONTEXT requestContext;

  MoveIrpList(PendingIrp, &completeList);
  while (!IsListEmpty(&completeList)) {
    listHead = RemoveHeadList(&completeList);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    requestContext = irpEntry->RequestContext;
    DokanFreeIrpEntry(irpEntry);
    DOKAN_LOG_("[%s][%s] FileObject=%p",
               DokanGetMajorFunctionStr(requestContext.IrpSp->MajorFunction),
               DokanGetMinorFunctionStr(requestContext.IrpSp->MajorFunction,
                                        requestContext.IrpSp->MinorFunction),
               requestContext.IrpSp->FileObject);
    DokanCancelPendingIrp(&requestContext, STATUS_CANCELLED);
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
    DokanFree(driverEventContext);
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

  DokanFree(waitBlock);
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
  DokanStopSystemThread(&Dcb->ReleaseEvent, &Dcb->EventNotificationThread);
}

VOID DokanCleanupAllChangeNotificationWaiters(__in PDokanVCB Vcb) {
  DokanVCBLockRW(Vcb);
  DOKAN_INIT_LOGGER(logger, Vcb->Dcb->DeviceObject->DriverObject, 0);
  DokanLogInfo(&logger, L"Cleaning up all change notification waiters.");
  FsRtlNotifyCleanupAll(Vcb->NotifySync, &Vcb->DirNotifyList);
  DokanVCBUnlock(Vcb);
}

VOID DokanStopFcbGarbageCollectorThread(__in PDokanVCB Vcb) {
  PKTHREAD gcThread = Vcb->FcbGarbageCollectorThread;
  if (gcThread != NULL) {
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    KeWaitForSingleObject(gcThread, Executive, KernelMode, FALSE, NULL);
    DokanVCBLockRW(Vcb);
    DokanForceFcbGarbageCollection(Vcb);
    Vcb->FcbGarbageCollectorThread = NULL;
    DokanVCBUnlock(Vcb);
    ObDereferenceObject(gcThread);
  }
}

static IO_WORKITEM_ROUTINE DokanEventReleaseWorkItemRoutine;
static VOID DokanEventReleaseWorkItemRoutine(_In_ PDEVICE_OBJECT DeviceObject,
                                             _In_opt_ PVOID Context) {
  PIO_WORKITEM workItem = (PIO_WORKITEM)Context;
  PDokanVCB vcb = (PDokanVCB)DeviceObject->DeviceExtension;
  PDOKAN_GLOBAL dokanGlobal = vcb->Global;
  if (workItem != NULL) {
    IoFreeWorkItem(workItem);
  }
  FsRtlEnterFileSystem();
  DokanEventRelease(NULL, DeviceObject);
  FsRtlExitFileSystem();
  ObDereferenceObject(DeviceObject);
  ExReleaseRundownProtection(&dokanGlobal->TeardownRundown);
}

VOID DokanQueueEventRelease(__in PDEVICE_OBJECT VolumeDeviceObject) {
  if (VolumeDeviceObject == NULL ||
      VolumeDeviceObject->DeviceExtension == NULL ||
      GetIdentifierType(VolumeDeviceObject->DeviceExtension) != VCB) {
    return;
  }
  PDokanVCB vcb = (PDokanVCB)VolumeDeviceObject->DeviceExtension;
  PDOKAN_GLOBAL dokanGlobal = vcb->Global;
  if (!ExAcquireRundownProtection(&dokanGlobal->TeardownRundown)) {
    return;
  }
  ObReferenceObject(VolumeDeviceObject);
  PIO_WORKITEM workItem = IoAllocateWorkItem(VolumeDeviceObject);
  if (workItem == NULL) {
    DOKAN_LOG("Failed to allocate event release work item");
    ObDereferenceObject(VolumeDeviceObject);
    ExReleaseRundownProtection(&dokanGlobal->TeardownRundown);
    return;
  }
  IoQueueWorkItem(workItem, DokanEventReleaseWorkItemRoutine, DelayedWorkQueue,
                  workItem);
}

NTSTATUS DokanEventRelease(__in_opt PREQUEST_CONTEXT RequestContext,
                           __in PDEVICE_OBJECT DeviceObject) {
  PDokanDCB dcb;
  PDokanVCB vcb;
  PDEVICE_OBJECT diskDeviceObject;
  NTSTATUS status = STATUS_SUCCESS;
  DOKAN_INIT_LOGGER(
      logger, DeviceObject == NULL ? NULL : DeviceObject->DriverObject, 0);

  if (DeviceObject == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  DokanLogInfo(&logger, L"Entered event release.");

  vcb = DeviceObject->DeviceExtension;
  if (vcb == NULL || GetIdentifierType(vcb) != VCB) {
    return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                         L"VCB being released has wrong identifier type.");
  }

  // Atomically claim dismount ownership (VcbGood -> VcbDismountStarting) while
  // keeping IsUnmountPending FALSE during DokanDeleteMountPoint so
  // MountManager's re-entrant IOCTL_MOUNTDEV_* requests succeed.
  if (InterlockedCompareExchange(&vcb->Condition, VcbDismountStarting,
                                 VcbGood) != VcbGood) {
    DokanLogInfo(&logger, L"Event release is already running for this volume.");
    return STATUS_SUCCESS;
  }

  dcb = vcb->Dcb;
  diskDeviceObject = dcb->DeviceObject;

  // Pin both device objects for the duration of DokanEventRelease so
  // synchronous or concurrent DokanDeleteVcb / DokanDeleteDcb cannot free
  // their extensions before we finish.
  ObReferenceObject(DeviceObject);
  ObReferenceObject(diskDeviceObject);

  status = IoAcquireRemoveLock(&dcb->RemoveLock, vcb);
  if (!NT_SUCCESS(status)) {
    DokanLogError(&logger, status, L"IoAcquireRemoveLock failed in release.");
    ObDereferenceObject(diskDeviceObject);
    ObDereferenceObject(DeviceObject);
    return STATUS_DEVICE_REMOVED;
  }

  // 1. Delete the mount point with zero Dokan ERESOURCEs held. RemoveLock is
  //    still active and Condition == VcbDismountStarting so MountManager's
  //    re-entrant IOCTL_MOUNTDEV_* requests complete normally
  //    (IOCTL_MOUNTDEV_LINK_DELETED is ignored because UseMountManager is
  //    cleared, or defers via work item).
  DokanDeleteMountPoint(RequestContext, dcb);

  // 2. Now reject all new I/O: mark VCB dismount-in-progress, DCB
  //    delete-pending, and flag VPB with VPB_REMOVE_PENDING (while keeping
  //    Vpb->DeviceObject non-NULL until OpenFileCount == 0 &&
  //    Vpb->ReferenceCount == 0 so late CLEANUP/CLOSE route to volDeviceObject).
  InterlockedExchange(&vcb->Condition, VcbDismountInProgress);
  InterlockedExchange(&dcb->Condition, DcbDeletePending);
  if (vcb->Vpb != NULL) {
    KIRQL vpbIrql;
    IoAcquireVpbSpinLock(&vpbIrql);
    ClearFlag(vcb->Vpb->Flags, VPB_MOUNTED);
    SetFlag(vcb->Vpb->Flags, VPB_REMOVE_PENDING);
    IoReleaseVpbSpinLock(vpbIrql);
  }

  DokanLogInfo(&logger, L"Starting unmount for device \"%wZ\"",
               dcb->DiskDeviceName);

  // 3. Permanently signal ReleaseEvent to wake all waiting/late-entering pull
  //    threads, drain pending IRPs and events, and stop worker threads.
  KeSetEvent(&dcb->ReleaseEvent, IO_NO_INCREMENT, FALSE);
  ReleasePendingIrp(&dcb->PendingIrp);
  ReleasePendingIrp(&dcb->PendingRetryIrp);
  ReleaseNotifyEvent(&dcb->NotifyEvent);
  DokanStopCheckThread(dcb);
  DokanStopEventNotificationThread(dcb);
  DokanStopFcbGarbageCollectorThread(vcb);
  DokanCleanupAllChangeNotificationWaiters(vcb);

  // 4. Drain all active DiskDeviceControl / DokanDiskUserFsRequest /
  //    DokanEventStart callers; future IoAcquireRemoveLock calls fail fast.
  IoReleaseRemoveLockAndWait(&dcb->RemoveLock, vcb);

  // 5. Deregister MUP, remove MountEntry, disable PnP device interfaces, and
  //    delete the \DosDevices\Global\Volume{...} symlink.
  if (dcb->MupHandle != 0) {
    RunAsSystem(DokanDeregisterUncProvider, dcb);
  }
  if (dcb->DiskDeviceName != NULL) {
    DOKAN_CONTROL dokanControl;
    RtlZeroMemory(&dokanControl, sizeof(DOKAN_CONTROL));
    RtlCopyMemory(dokanControl.DeviceName, dcb->DiskDeviceName->Buffer,
                  dcb->DiskDeviceName->Length);
    dokanControl.SessionId = dcb->SessionId;
    DokanLogInfo(&logger, L"Removing mount entry.");
    RemoveMountEntry(dcb->Global, &dokanControl);
  }
  if (dcb->MountedDeviceInterfaceName.Buffer != NULL) {
    DokanLogInfo(
        &logger,
        L"Changing interface state to false for mounted device \"%wZ\"",
        &dcb->MountedDeviceInterfaceName);
    (VOID) IoSetDeviceInterfaceState(&dcb->MountedDeviceInterfaceName, FALSE);
    RtlFreeUnicodeString(&dcb->MountedDeviceInterfaceName);
    RtlInitUnicodeString(&dcb->MountedDeviceInterfaceName, NULL);
  }
  if (dcb->DiskDeviceInterfaceName.Buffer != NULL) {
    DokanLogInfo(&logger,
                 L"Changing interface state to false for disk device \"%wZ\"",
                 &dcb->DiskDeviceInterfaceName);
    (VOID) IoSetDeviceInterfaceState(&dcb->DiskDeviceInterfaceName, FALSE);
    RtlFreeUnicodeString(&dcb->DiskDeviceInterfaceName);
    RtlInitUnicodeString(&dcb->DiskDeviceInterfaceName, NULL);
  }
  if (dcb->SymbolicLinkName != NULL) {
    NTSTATUS linkStatus = IoDeleteSymbolicLink(dcb->SymbolicLinkName);
    if (!NT_SUCCESS(linkStatus) && linkStatus != STATUS_OBJECT_NAME_NOT_FOUND) {
      DOKAN_LOG_("Delete of SymbolicLink failed \"%wZ\" %s",
                 dcb->SymbolicLinkName, DokanGetNTSTATUSStr(linkStatus));
    }
  }

  // 6. Evict standby SEC_IMAGE ControlAreas and clean cached sections with
  //    zero Dokan locks held so their internal FILE_OBJECTs close synchronously.
  DokanPurgeVolumeCachedSections(vcb);

  // 7. Transition to VcbDismounted and tear down VCB/DCB synchronously if no
  //    open files or VPB references remain (otherwise final IRP_MJ_CLOSE or
  //    the non-blocking dismount work item / backoff timer completes teardown).
  InterlockedExchange(&vcb->Condition, VcbDismounted);
  DokanCheckForDismount(vcb, /*ResidualVpbRefs=*/0);

  DokanLogInfo(&logger, L"Finished event release.");

  ObDereferenceObject(diskDeviceObject);
  ObDereferenceObject(DeviceObject);
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
  mountEntry = FindMountEntry(RequestContext->DokanGlobal, &dokanControl,
                 /*ExclusiveLock=*/FALSE);
  if (mountEntry == NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Cannot found device associated to mount point %ws",
                  dokanControl.MountPoint);
    return STATUS_BUFFER_TOO_SMALL;
  }

  PDEVICE_OBJECT volumeDeviceObject =
      mountEntry->MountControl.VolumeDeviceObject;
  PDEVICE_OBJECT diskDeviceObject = mountEntry->MountControl.DiskDeviceObject;
  if (volumeDeviceObject != NULL) {
    ObReferenceObject(volumeDeviceObject);
  }
  if (diskDeviceObject != NULL) {
    ObReferenceObject(diskDeviceObject);
  }
  ExReleaseResourceLite(&mountEntry->Resource);

  NTSTATUS status;
  if (volumeDeviceObject == NULL || IsDeletePending(volumeDeviceObject)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Device is deleted");
    status = STATUS_DEVICE_REMOVED;
  } else if (!IsMounted(volumeDeviceObject)) {
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "Device is still not mounted, so an unmount not possible at this "
        "point");
    status = STATUS_DEVICE_BUSY;
  } else {
    status = DokanEventRelease(RequestContext, volumeDeviceObject);
  }

  if (volumeDeviceObject != NULL) {
    ObDereferenceObject(volumeDeviceObject);
  }
  if (diskDeviceObject != NULL) {
    ObDereferenceObject(diskDeviceObject);
  }
  return status;
}
