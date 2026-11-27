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
#include "util/fcb.h"
#include "util/irp_buffer_helper.h"
#include "util/mountmgr.h"
#include "util/str.h"

#include <initguid.h>
#include <ntddstor.h>
#include <wdmsec.h>

static VOID FreeDcbNames(__in PDokanDCB Dcb) {
  if (Dcb->MountPoint != NULL) {
    DokanFreeUnicodeString(Dcb->MountPoint);
    Dcb->MountPoint = NULL;
  }
  if (Dcb->SymbolicLinkName != NULL) {
    DokanFreeUnicodeString(Dcb->SymbolicLinkName);
    Dcb->SymbolicLinkName = NULL;
  }
  if (Dcb->DiskDeviceName != NULL) {
    DokanFreeUnicodeString(Dcb->DiskDeviceName);
    Dcb->DiskDeviceName = NULL;
  }
  if (Dcb->UNCName != NULL) {
    DokanFreeUnicodeString(Dcb->UNCName);
    Dcb->UNCName = NULL;
  }
  if (Dcb->PersistentSymbolicLinkName != NULL) {
    DokanFreeUnicodeString(Dcb->PersistentSymbolicLinkName);
    Dcb->PersistentSymbolicLinkName = NULL;
  }
  if (Dcb->VolumeLabel != NULL) {
    DokanFree(Dcb->VolumeLabel);
    Dcb->VolumeLabel = NULL;
  }
}

VOID DokanInitIrpList(__in PIRP_LIST IrpList, __in BOOLEAN EventEnabled,
                      __in EVENT_TYPE EventType) {
  InitializeListHead(&IrpList->ListHead);
  KeInitializeSpinLock(&IrpList->ListLock);
  KeInitializeEvent(&IrpList->NotEmpty, EventType, FALSE);
  IrpList->EventEnabled = EventEnabled;
}

VOID DokanRecordSessionMountPoint(__in PDOKAN_GLOBAL DokanGlobal,
                                  __in ULONG SessionId,
                                  __in_opt PUNICODE_STRING MountPoint) {
  if (SessionId == (ULONG)-1 || MountPoint == NULL || MountPoint->Length == 0 ||
      MountPoint->Buffer == NULL) {
    return;
  }

  PSESSION_MOUNT_POINT_ENTRY entry =
      DokanAllocZero(sizeof(SESSION_MOUNT_POINT_ENTRY));
  if (entry == NULL) {
    DOKAN_LOG("Failed to allocate SESSION_MOUNT_POINT_ENTRY");
    return;
  }

  entry->MountPoint.Buffer = DokanAlloc(MountPoint->MaximumLength);
  if (entry->MountPoint.Buffer == NULL) {
    DOKAN_LOG("Failed to allocate session MountPoint buffer");
    DokanFree(entry);
    return;
  }

  entry->SessionId = SessionId;
  entry->MountPoint.MaximumLength = MountPoint->MaximumLength;
  RtlUnicodeStringCopy(&entry->MountPoint, MountPoint);
  InitializeListHead(&entry->ListEntry);

  ExAcquireFastMutex(&DokanGlobal->SessionMountPointLock);
  InsertTailList(&DokanGlobal->SessionMountPointList, &entry->ListEntry);
  ExReleaseFastMutex(&DokanGlobal->SessionMountPointLock);
}

BOOLEAN
InsertMountEntry(PDOKAN_GLOBAL DokanGlobal, PDOKAN_CONTROL DokanControl) {
  PMOUNT_ENTRY mountEntry = DokanAllocZero(sizeof(MOUNT_ENTRY));
  if (mountEntry == NULL) {
    DOKAN_LOG("Allocation failed");
    return FALSE;
  }
  ExInitializeResourceLite(&mountEntry->Resource);
  InitializeListHead(&mountEntry->ListEntry);
  RtlCopyMemory(&mountEntry->MountControl, DokanControl, sizeof(DOKAN_CONTROL));
  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&DokanGlobal->MountPointListLock, TRUE);
  InsertTailList(&DokanGlobal->MountPointList, &mountEntry->ListEntry);
  ExReleaseResourceLite(&DokanGlobal->MountPointListLock);
  KeLeaveCriticalRegion();
  return TRUE;
}

VOID DokanPurgeVolumeCachedSections(__in PDokanVCB Vcb) {
  ULONG fcbCount = 0;
  PDokanFCB *fcbSnapshot = NULL;
  ULONG snapshotCount = 0;
  PIRP prevTopLevelIrp = IoGetTopLevelIrp();

  // Mark non-top-level so any recursive IRP_MJ_CLOSE triggered by
  // MmFlushImageSection / CcPurgeCacheSection defers dismount cleanly while
  // DokanEventRelease finishes and calls DokanCheckForDismount afterward.
  if (prevTopLevelIrp == NULL) {
    IoSetTopLevelIrp((PIRP)FSRTL_FSP_TOP_LEVEL_IRP);
  }

  DokanVCBLockRO(Vcb);
  fcbCount = RtlNumberGenericTableElementsAvl(&Vcb->FcbTable);
  for (PLIST_ENTRY entry = Vcb->ReplacedFcbList.Flink;
       entry != &Vcb->ReplacedFcbList; entry = entry->Flink) {
    ++fcbCount;
  }
  DokanVCBUnlock(Vcb);

  if (fcbCount > 0) {
    fcbSnapshot = DokanAlloc(fcbCount * sizeof(PDokanFCB));
    if (fcbSnapshot != NULL) {
      DokanVCBLockRO(Vcb);
      PVOID restartKey = NULL;
      for (PDokanFCB *elem =
               (PDokanFCB *)RtlEnumerateGenericTableWithoutSplayingAvl(
                   &Vcb->FcbTable, &restartKey);
           elem != NULL && snapshotCount < fcbCount;
           elem = (PDokanFCB *)RtlEnumerateGenericTableWithoutSplayingAvl(
               &Vcb->FcbTable, &restartKey)) {
        PDokanFCB fcb = *elem;
        if (fcb != NULL &&
            (fcb->SectionObjectPointers.ImageSectionObject != NULL ||
             fcb->SectionObjectPointers.DataSectionObject != NULL)) {
          InterlockedIncrement(&fcb->OpenCount);
          fcbSnapshot[snapshotCount++] = fcb;
        }
      }
      for (PLIST_ENTRY entry = Vcb->ReplacedFcbList.Flink;
           entry != &Vcb->ReplacedFcbList && snapshotCount < fcbCount;
           entry = entry->Flink) {
        PDokanFCB fcb = CONTAINING_RECORD(entry, DokanFCB, NextReplacedFcb);
        if (fcb->SectionObjectPointers.ImageSectionObject != NULL ||
            fcb->SectionObjectPointers.DataSectionObject != NULL) {
          InterlockedIncrement(&fcb->OpenCount);
          fcbSnapshot[snapshotCount++] = fcb;
        }
      }
      DokanVCBUnlock(Vcb);

      for (ULONG i = 0; i < snapshotCount; ++i) {
        PDokanFCB fcb = fcbSnapshot[i];
        if (fcb->SectionObjectPointers.ImageSectionObject != NULL) {
          MmFlushImageSection(&fcb->SectionObjectPointers, MmFlushForWrite);
          MmFlushImageSection(&fcb->SectionObjectPointers, MmFlushForDelete);
        }
        if (fcb->SectionObjectPointers.DataSectionObject != NULL) {
          // Intentionally omit CcFlushCache: userland is already torn down
          // during forced dismount, so flushing dirty pages would fail or
          // stall. Acquire Fcb->MainResource exclusively across the purge.
          DokanFCBLockRW(fcb);
          CcPurgeCacheSection(&fcb->SectionObjectPointers, NULL, 0, FALSE);
          DokanFCBUnlock(fcb);
        }
        if (fcb->SectionObjectPointers.ImageSectionObject != NULL ||
            fcb->SectionObjectPointers.DataSectionObject != NULL) {
          // Close standby DataSectionObject / ImageSectionObject ControlAreas
          // directly (CcPurgeCacheSection is a no-op once SharedCacheMap is
          // uninitialized) and mark any still-mapped ControlAreas for delayed
          // close on final unmap.
          MmForceSectionClosed(&fcb->SectionObjectPointers,
                               /*DelayClose=*/TRUE);
        }
        DokanFreeFCB(Vcb, fcb);
      }
      DokanFree(fcbSnapshot);
    }
  }

  if (prevTopLevelIrp == NULL) {
    IoSetTopLevelIrp(NULL);
  }
}

VOID DokanDeleteDcb(__in PDokanDCB Dcb) {
  if (InterlockedCompareExchange(&Dcb->Condition, DcbDeleted,
                                 DcbDeletePending) != DcbDeletePending &&
      InterlockedCompareExchange(&Dcb->Condition, DcbDeleted, DcbActive) !=
          DcbActive) {
    return;
  }

  PDEVICE_OBJECT diskDeviceObject = Dcb->DeviceObject;
  DOKAN_INIT_LOGGER(logger, diskDeviceObject->DriverObject, 0);

  if (Dcb->DispatchDriverLogs) {
    Dcb->DispatchDriverLogs = FALSE;
    DecrementVcbLogCacheCount();
  }

  if (Dcb->SymbolicLinkName != NULL) {
    NTSTATUS status = IoDeleteSymbolicLink(Dcb->SymbolicLinkName);
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND) {
      DOKAN_LOG_("Delete of SymbolicLink failed \"%wZ\" %s",
                 Dcb->SymbolicLinkName, DokanGetNTSTATUSStr(status));
    }
  }

  FreeDcbNames(Dcb);

  // Delete Dcb->Resource strictly before IoDeleteDevice / ObDereferenceObject
  // frees the DCB extension.
  ExDeleteResourceLite(&Dcb->Resource);

  DokanLogInfo(&logger, L"Deleting disk device object.");
  // Pair with ObReferenceObject(diskDeviceObject) in DokanCreateDiskDevice
  // before IoDeleteDevice releases the IoCreateDevice reference.
  ObDereferenceObject(diskDeviceObject);
  IoDeleteDevice(diskDeviceObject);
}

VOID DokanDeleteVcb(__in PDokanVCB Vcb) {
  PDEVICE_OBJECT volDeviceObject = Vcb->DeviceObject;
  PDokanDCB dcb = Vcb->Dcb;
  PDOKAN_GLOBAL dokanGlobal = Vcb->Global;
  PDEVICE_OBJECT targetDeviceObject = Vcb->TargetDeviceObject;
  DOKAN_INIT_LOGGER(logger, volDeviceObject->DriverObject, 0);

  ASSERT(Vcb->OpenFileCount == 0);
  ASSERT(RtlIsGenericTableEmptyAvl(&Vcb->FcbTable));
  ASSERT(IsListEmpty(&Vcb->ReplacedFcbList));
  ASSERT(Vcb->FcbGarbageCollectorThread == NULL);

  // Cancel any armed dismount backoff timer before tearing down Vcb. Because
  // DismountTimer is only armed under VcbListLock while Condition ==
  // VcbDismounted and Condition is already VcbDeleted here, no new timer can be
  // armed concurrently. If the timer already fired, its DPC/work item holds
  // its own volDeviceObject and TeardownRundown references and will exit on
  // observing VcbDeleted.
  if (KeCancelTimer(&Vcb->DismountTimer)) {
    PIO_WORKITEM workItem = Vcb->DismountWorkItem;
    Vcb->DismountWorkItem = NULL;
    if (workItem != NULL) {
      IoFreeWorkItem(workItem);
    }
    InterlockedExchange(&Vcb->DismountWorkItemQueued, 0);
    ObDereferenceObject(volDeviceObject);
    ExReleaseRundownProtection(&dokanGlobal->TeardownRundown);
  }

  DOKAN_LOG_("Deleting VCB %p (FCB alloc=%ld free=%ld, CCB alloc=%ld free=%ld)",
             Vcb, Vcb->FcbAllocated, Vcb->FcbFreed, Vcb->CcbAllocated,
             Vcb->CcbFreed);

  CleanDokanLogEntry(Vcb);

  // Detach VCB from DCB (RemoveLock is already drained).
  dcb->Vcb = NULL;

  // Tear down per-stream contexts on VolumeFileHeader and NotifySync ('FSfm').
  FsRtlTeardownPerStreamContexts(&Vcb->VolumeFileHeader);
  if (Vcb->NotifySync != NULL) {
    FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
    Vcb->NotifySync = NULL;
  }

  // Delete Vcb->Resource strictly before IoDeleteDevice / ObDereferenceObject.
  ExDeleteResourceLite(&Vcb->Resource);

  DokanLogInfo(&logger, L"Deleting volume device object.");
  // Pair with ObReferenceObject(volDeviceObject) in DokanMountVolume before
  // IoDeleteDevice releases the IoCreateDevice reference.
  ObDereferenceObject(volDeviceObject);
  IoDeleteDevice(volDeviceObject);

  // VCB has detached and RemoveLock is drained; tear down DCB now.
  DokanDeleteDcb(dcb);

  // Release the TargetDeviceObject reference transferred by
  // ntoskrnl!IopMountVolume on STATUS_SUCCESS (matching FastFat).
  if (targetDeviceObject != NULL) {
    ObDereferenceObject(targetDeviceObject);
  }
}

static BOOLEAN DokanCheckForDismountInternal(__in PDokanVCB Vcb,
                                             __in ULONG ResidualVpbRefs,
                                             __in BOOLEAN QueueRetryOnVpbRef,
                                             __in BOOLEAN RearmTimer,
                                             __out_opt PBOOLEAN TimerArmed) {
  PDOKAN_GLOBAL dokanGlobal = Vcb->Global;
  BOOLEAN deleteVcb = FALSE;
  BOOLEAN needsRetryWorkItem = FALSE;

  if (TimerArmed != NULL) {
    *TimerArmed = FALSE;
  }

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&dokanGlobal->VcbListLock, TRUE);

  if (InterlockedCompareExchange(&Vcb->Condition, 0, 0) != VcbDismounted) {
    ExReleaseResourceLite(&dokanGlobal->VcbListLock);
    KeLeaveCriticalRegion();
    return FALSE;
  }

  DokanVCBLockRW(Vcb);

  LONG openFiles = InterlockedCompareExchange(&Vcb->OpenFileCount, 0, 0);
  if (openFiles == 0) {
    PVPB vpb = Vcb->Vpb;
    if (vpb != NULL) {
      KIRQL vpbIrql;
      IoAcquireVpbSpinLock(&vpbIrql);
      if (vpb->ReferenceCount <= ResidualVpbRefs) {
        ClearFlag(vpb->Flags, VPB_MOUNTED);
        SetFlag(vpb->Flags, VPB_REMOVE_PENDING);
        vpb->DeviceObject = NULL;
        deleteVcb = TRUE;
      } else {
        needsRetryWorkItem = TRUE;
      }
      IoReleaseVpbSpinLock(vpbIrql);
    } else {
      deleteVcb = TRUE;
    }
  }

  if (deleteVcb) {
    InterlockedExchange(&Vcb->Condition, VcbDeleted);
    if (Vcb->VcbLinks.Flink != NULL && !IsListEmpty(&Vcb->VcbLinks)) {
      RemoveEntryList(&Vcb->VcbLinks);
      InitializeListHead(&Vcb->VcbLinks);
    }
  } else if (RearmTimer && needsRetryWorkItem &&
             KeReadStateEvent(&dokanGlobal->UnloadEvent) == 0) {
    // Arm the non-blocking exponential backoff timer while holding VcbListLock
    // so KeSetTimer is strictly ordered with VcbDeleted -> DokanDeleteVcb ->
    // KeCancelTimer.
    static const ULONG kBackoffMs[] = {10, 20, 50, 100, 250, 500};
    ULONG idx = Vcb->DismountRetryCount;
    if (idx >= RTL_NUMBER_OF(kBackoffMs)) {
      idx = RTL_NUMBER_OF(kBackoffMs) - 1;
    } else {
      ++Vcb->DismountRetryCount;
    }
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -(LONGLONG)kBackoffMs[idx] * 10000LL;
    KeSetTimer(&Vcb->DismountTimer, dueTime, &Vcb->DismountDpc);
    if (TimerArmed != NULL) {
      *TimerArmed = TRUE;
    }
  }

  DokanVCBUnlock(Vcb);
  ExReleaseResourceLite(&dokanGlobal->VcbListLock);
  KeLeaveCriticalRegion();

  if (deleteVcb) {
    DokanDeleteVcb(Vcb);
    return TRUE;
  }

  if (needsRetryWorkItem && QueueRetryOnVpbRef) {
    DokanQueueDismountWorkItem(Vcb);
  }
  return FALSE;
}

BOOLEAN DokanCheckForDismount(__in PDokanVCB Vcb, __in ULONG ResidualVpbRefs) {
  return DokanCheckForDismountInternal(Vcb, ResidualVpbRefs,
                                       /*QueueRetryOnVpbRef=*/TRUE,
                                       /*RearmTimer=*/FALSE,
                                       /*TimerArmed=*/NULL);
}

static IO_WORKITEM_ROUTINE DokanDismountWorkItemRoutine;
static VOID DokanDismountWorkItemRoutine(_In_ PDEVICE_OBJECT DeviceObject,
                                         _In_opt_ PVOID Context) {
  PIO_WORKITEM workItem = (PIO_WORKITEM)Context;
  PDokanVCB vcb = (PDokanVCB)DeviceObject->DeviceExtension;
  PDOKAN_GLOBAL dokanGlobal = vcb->Global;
  BOOLEAN timerArmed = FALSE;

  FsRtlEnterFileSystem();

  (VOID) DokanCheckForDismountInternal(vcb, /*ResidualVpbRefs=*/0,
                                       /*QueueRetryOnVpbRef=*/FALSE,
                                       /*RearmTimer=*/TRUE, &timerArmed);

  if (timerArmed) {
    // Ownership of (DismountWorkItemQueued=1, workItem, +1 DeviceObject,
    // +1 TeardownRundown) transferred to DismountTimer / DismountDpc.
    FsRtlExitFileSystem();
    return;
  }

  vcb->DismountWorkItem = NULL;
  if (workItem != NULL) {
    IoFreeWorkItem(workItem);
  }
  InterlockedExchange(&vcb->DismountWorkItemQueued, 0);
  if (InterlockedCompareExchange(&vcb->Condition, 0, 0) == VcbDismounted &&
      InterlockedCompareExchange(&vcb->OpenFileCount, 0, 0) == 0 &&
      KeReadStateEvent(&dokanGlobal->UnloadEvent) == 0) {
    DokanCheckForDismount(vcb, /*ResidualVpbRefs=*/0);
  }

  FsRtlExitFileSystem();
  ObDereferenceObject(DeviceObject);
  ExReleaseRundownProtection(&dokanGlobal->TeardownRundown);
}

VOID DokanDismountTimerDpc(_In_ struct _KDPC *Dpc,
                           _In_opt_ PVOID DeferredContext,
                           _In_opt_ PVOID SystemArgument1,
                           _In_opt_ PVOID SystemArgument2) {
  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument1);
  UNREFERENCED_PARAMETER(SystemArgument2);
  PDokanVCB vcb = (PDokanVCB)DeferredContext;
  if (vcb == NULL || vcb->DismountWorkItem == NULL) {
    return;
  }
  // Reuse the pre-allocated workItem so DISPATCH_LEVEL retry never allocates
  // pool or dereferences DeviceObject directly.
  IoQueueWorkItem(vcb->DismountWorkItem, DokanDismountWorkItemRoutine,
                  DelayedWorkQueue, vcb->DismountWorkItem);
}

VOID DokanQueueDismountWorkItem(__in PDokanVCB Vcb) {
  if (InterlockedCompareExchange(&Vcb->Condition, 0, 0) != VcbDismounted) {
    return;
  }
  if (InterlockedCompareExchange(&Vcb->DismountWorkItemQueued, 1, 0) != 0) {
    return;
  }
  PDOKAN_GLOBAL dokanGlobal = Vcb->Global;
  if (!ExAcquireRundownProtection(&dokanGlobal->TeardownRundown)) {
    InterlockedExchange(&Vcb->DismountWorkItemQueued, 0);
    return;
  }
  PDEVICE_OBJECT volDeviceObject = Vcb->DeviceObject;
  ObReferenceObject(volDeviceObject);
  PIO_WORKITEM workItem = IoAllocateWorkItem(volDeviceObject);
  if (workItem == NULL) {
    InterlockedExchange(&Vcb->DismountWorkItemQueued, 0);
    ObDereferenceObject(volDeviceObject);
    ExReleaseRundownProtection(&dokanGlobal->TeardownRundown);
    return;
  }
  Vcb->DismountWorkItem = workItem;
  Vcb->DismountRetryCount = 0;
  IoQueueWorkItem(workItem, DokanDismountWorkItemRoutine, DelayedWorkQueue,
                  workItem);
}

VOID RemoveMountEntry(__in PDOKAN_GLOBAL DokanGlobal,
                      __in PDOKAN_CONTROL DokanControl) {
  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&DokanGlobal->MountPointListLock, TRUE);

  // We need to refetch the entry to get the correct locking order
  // but we call it with the mount entry list lock acquired so we can remove
  // and release its memory without having another thread waiting
  // on it in a concurrent FindMountEntry call.
  PMOUNT_ENTRY mountEntry =
      FindMountEntry(DokanGlobal, DokanControl, /*ExclusiveLock=*/TRUE);
  if (!mountEntry) {
    // Already removed
    ExReleaseResourceLite(&DokanGlobal->MountPointListLock);
    KeLeaveCriticalRegion();
    return;
  }
  RemoveEntryList(&mountEntry->ListEntry);
  InitializeListHead(&mountEntry->ListEntry);
  ExReleaseResourceLite(&mountEntry->Resource);
  ExDeleteResourceLite(&mountEntry->Resource);
  DokanFree(mountEntry);

  ExReleaseResourceLite(&DokanGlobal->MountPointListLock);
  KeLeaveCriticalRegion();
}

BOOLEAN IsMounted(__in PDEVICE_OBJECT DeviceObject) {
  PDokanVCB vcb;
  PDokanDCB dcb;

  if (DeviceObject == NULL || DeviceObject->DeviceExtension == NULL) {
    return FALSE;
  }

  dcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(dcb) == DCB) {
    if (InterlockedCompareExchange(&dcb->Condition, 0, 0) != DcbActive) {
      return FALSE;
    }
    vcb = dcb->Vcb;
  } else {
    vcb = DeviceObject->DeviceExtension;
  }

  if (vcb == NULL || GetIdentifierType(vcb) != VCB) {
    return FALSE;
  }

  return InterlockedCompareExchange(&vcb->Condition, 0, 0) == VcbGood;
}

BOOLEAN IsDeletePending(__in PDEVICE_OBJECT DeviceObject) {
  PVOID extension;

  if (DeviceObject == NULL) {
    DOKAN_LOG("DeviceObject is null");
    return TRUE;
  }

  extension = DeviceObject->DeviceExtension;
  if (extension == NULL) {
    return TRUE;
  }

  if (GetIdentifierType(extension) == DCB) {
    PDokanDCB dcb = (PDokanDCB)extension;
    if (InterlockedCompareExchange(&dcb->Condition, 0, 0) != DcbActive) {
      DOKAN_LOG("Delete is pending for this DCB");
      return TRUE;
    }
    return FALSE;
  }

  if (GetIdentifierType(extension) == VCB) {
    PDokanVCB vcb = (PDokanVCB)extension;
    LONG cond = InterlockedCompareExchange(&vcb->Condition, 0, 0);
    if (cond == VcbDismountStarting || cond == VcbDismountInProgress ||
        cond == VcbDismounted || cond == VcbDeleted) {
      DOKAN_LOG("Delete is pending for this VCB");
      return TRUE;
    }
    return FALSE;
  }

  return FALSE;
}

BOOLEAN IsUnmountPending(__in PDEVICE_OBJECT DeviceObject) {
  PVOID extension;

  if (DeviceObject == NULL) {
    return FALSE;
  }

  extension = DeviceObject->DeviceExtension;
  if (extension == NULL) {
    return FALSE;
  }

  if (GetIdentifierType(extension) == DCB) {
    PDokanDCB dcb = (PDokanDCB)extension;
    if (InterlockedCompareExchange(&dcb->Condition, 0, 0) != DcbActive) {
      return TRUE;
    }
    return IsUnmountPendingVcb((PDokanVCB)dcb->Vcb);
  }

  if (GetIdentifierType(extension) == VCB) {
    return IsUnmountPendingVcb((PDokanVCB)extension);
  }

  return FALSE;
}

BOOLEAN IsUnmountPendingVcb(__in PDokanVCB vcb) {
  if (vcb == NULL) {
    return FALSE;
  }

  if (GetIdentifierType(vcb) == VCB) {
    LONG cond = InterlockedCompareExchange(&vcb->Condition, 0, 0);
    if (cond == VcbDismountInProgress || cond == VcbDismounted ||
        cond == VcbDeleted) {
      DOKAN_LOG("Volume is dismounting");
      return TRUE;
    }
    if (cond == VcbNotMounted) {
      return FALSE;
    }
    return InterlockedCompareExchange(&vcb->Dcb->Condition, 0, 0) != DcbActive;
  }

  return FALSE;
}

_Ret_maybenull_
_When_(ExclusiveLock, _Acquires_exclusive_lock_(return->Resource))
_When_(!ExclusiveLock, _Acquires_shared_lock_(return->Resource))
PMOUNT_ENTRY FindMountEntry(__in PDOKAN_GLOBAL DokanGlobal,
                            __in PDOKAN_CONTROL DokanControl,
                            __in BOOLEAN ExclusiveLock) {
  PMOUNT_ENTRY mountEntry = NULL;
  PLIST_ENTRY listEntry;
  PMOUNT_ENTRY mountEntryLookup = NULL;
  PDOKAN_CONTROL dokanControlLookup = NULL;
  BOOLEAN useMountPoint = (DokanControl->MountPoint[0] != L'\0');
  BOOLEAN isSessionIdMatch = FALSE;
  BOOLEAN lockMountEntryList =
      !ExIsResourceAcquiredExclusiveLite(&DokanGlobal->MountPointListLock);
  DOKAN_INIT_LOGGER(logger, DokanGlobal->DeviceObject->DriverObject, 0);

  // Callers acquire MountEntry->Resource returned by this function and must
  // already have normal kernel APCs disabled (FsRtlEnterFileSystem or
  // KeEnterCriticalRegion).
  ASSERT(KeAreApcsDisabled());

  DokanLogInfo(&logger, L"Finding mount entry; mount point = %s.",
               DokanControl->MountPoint);

  if (lockMountEntryList) {
    ExAcquireResourceExclusiveLite(&DokanGlobal->MountPointListLock, TRUE);
  }
  for (listEntry = DokanGlobal->MountPointList.Flink;
       listEntry != &DokanGlobal->MountPointList;
       listEntry = listEntry->Flink) {
    mountEntryLookup = CONTAINING_RECORD(listEntry, MOUNT_ENTRY, ListEntry);
    ExAcquireResourceSharedLite(&mountEntryLookup->Resource, TRUE);
    dokanControlLookup = &mountEntryLookup->MountControl;
    if (useMountPoint) {
      isSessionIdMatch =
          (DokanControl->SessionId == dokanControlLookup->SessionId) ||
          (dokanControlLookup->SessionId == (ULONG)-1);
      if (isSessionIdMatch && (wcscmp(DokanControl->MountPoint,
                                      dokanControlLookup->MountPoint) == 0)) {
        DokanLogInfo(&logger, L"Found entry with matching mount point.");
        mountEntry = mountEntryLookup;
        break;
      }
      DokanLogInfo(&logger, L"Skipping entry with non-matching mount point: %s",
                   dokanControlLookup->MountPoint);
    } else if (wcscmp(DokanControl->DeviceName,
                      dokanControlLookup->DeviceName) == 0) {
      DokanLogInfo(&logger, L"Found entry with matching device name: %s",
                   dokanControlLookup->DeviceName);
      mountEntry = mountEntryLookup;
      break;
    }
    ExReleaseResourceLite(&mountEntryLookup->Resource);
  }

  if (mountEntry) {
    if (ExclusiveLock) {
      // Convert owned Shared lock to Exclusive
      ExReleaseResourceLite(&mountEntry->Resource);
      ExAcquireResourceExclusiveLite(&mountEntry->Resource, TRUE);
    }
    DokanLogInfo(&logger, L"Mount entry found: %s -> %s",
                 mountEntry->MountControl.MountPoint,
                 mountEntry->MountControl.DeviceName);
  } else {
    DokanLogInfo(&logger, L"No mount entry found.");
  }

  if (lockMountEntryList) {
    ExReleaseResourceLite(&DokanGlobal->MountPointListLock);
  }
  return mountEntry;
}

_Ret_maybenull_
_When_(ExclusiveLock, _Acquires_exclusive_lock_(return->Resource))
_When_(!ExclusiveLock, _Acquires_shared_lock_(return->Resource))
PMOUNT_ENTRY FindMountEntryByName(__in PDOKAN_GLOBAL DokanGlobal,
                                  __in PUNICODE_STRING DiskDeviceName,
                                  __in PUNICODE_STRING UNCName,
                                  __in BOOLEAN ExclusiveLock) {
  PMOUNT_ENTRY mountEntry = NULL;
  if (DiskDeviceName == NULL) {
    return NULL;
  }
  PDOKAN_CONTROL dokanControl = DokanAllocZero(sizeof(DOKAN_CONTROL));
  if (dokanControl == NULL) {
    return NULL;
  }
  RtlCopyMemory(dokanControl->DeviceName, DiskDeviceName->Buffer,
                DiskDeviceName->Length);
  if (UNCName->Buffer != NULL && UNCName->Length > 0) {
    RtlCopyMemory(dokanControl->UNCName, UNCName->Buffer, UNCName->Length);
  }
  mountEntry = FindMountEntry(DokanGlobal, dokanControl, ExclusiveLock);
  DokanFree(dokanControl);
  return mountEntry;
}

NTSTATUS
DokanGetMountPointList(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_SUCCESS;
  PLIST_ENTRY listEntry;
  PMOUNT_ENTRY mountEntry;
  PDOKAN_MOUNT_POINT_INFO dokanMountPointInfo;
  USHORT i = 0;

  BOOLEAN isAdmin =
      SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_IMPERSONATE_PRIVILEGE),
                             RequestContext->Irp->RequestorMode);

  ULONG sessionId;
  if (!NT_SUCCESS(IoGetRequestorSessionId(RequestContext->Irp, &sessionId))) {
    sessionId = 0;
  }

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(
      &RequestContext->DokanGlobal->MountPointListLock, TRUE);

  dokanMountPointInfo =
      (PDOKAN_MOUNT_POINT_INFO)RequestContext->Irp->AssociatedIrp.SystemBuffer;

  for (listEntry = RequestContext->DokanGlobal->MountPointList.Flink;
       listEntry != &RequestContext->DokanGlobal->MountPointList;
       listEntry = listEntry->Flink, ++i) {

    mountEntry = CONTAINING_RECORD(listEntry, MOUNT_ENTRY, ListEntry);
    ExAcquireResourceSharedLite(&mountEntry->Resource, TRUE);

    __try {
      // For non-admins, skip drives that are not in same session as requestor
      if (!isAdmin &&
          mountEntry->MountControl.SessionId != -1 &&
          mountEntry->MountControl.SessionId != sessionId) {
        i--;
        continue;
      }

      if (!ExtendOutputBufferBySize(RequestContext->Irp,
                                    sizeof(DOKAN_MOUNT_POINT_INFO),
                                    /*UpdateInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_OVERFLOW;
        break;
      }

      dokanMountPointInfo[i].Type = mountEntry->MountControl.Type;
      dokanMountPointInfo[i].SessionId = mountEntry->MountControl.SessionId;
      dokanMountPointInfo[i].MountOptions =
          mountEntry->MountControl.MountOptions;
      RtlCopyMemory(&dokanMountPointInfo[i].MountPoint,
                    &mountEntry->MountControl.MountPoint,
                    sizeof(mountEntry->MountControl.MountPoint));
      RtlCopyMemory(&dokanMountPointInfo[i].UNCName,
                    &mountEntry->MountControl.UNCName,
                    sizeof(mountEntry->MountControl.UNCName));
      RtlCopyMemory(&dokanMountPointInfo[i].DeviceName,
                    &mountEntry->MountControl.DeviceName,
                    sizeof(mountEntry->MountControl.DeviceName));
    } __finally {
      ExReleaseResourceLite(&mountEntry->Resource);
    }
  }

  ExReleaseResourceLite(&RequestContext->DokanGlobal->MountPointListLock);
  KeLeaveCriticalRegion();

  return status;
}

NTSTATUS
DokanCreateGlobalDiskDevice(__in PDRIVER_OBJECT DriverObject,
                            __out PDOKAN_GLOBAL *DokanGlobal) {

  NTSTATUS status;
  UNICODE_STRING deviceName;
  UNICODE_STRING symbolicLinkName;
  UNICODE_STRING fsDiskDeviceName;
  UNICODE_STRING fsCdDeviceName;
  PDEVICE_OBJECT deviceObject;
  PDEVICE_OBJECT fsDiskDeviceObject;
  PDEVICE_OBJECT fsCdDeviceObject;
  PDOKAN_GLOBAL dokanGlobal;

  RtlInitUnicodeString(&deviceName, DOKAN_GLOBAL_DEVICE_NAME);
  RtlInitUnicodeString(&symbolicLinkName, DOKAN_GLOBAL_SYMBOLIC_LINK_NAME);
  RtlInitUnicodeString(&fsDiskDeviceName, DOKAN_GLOBAL_FS_DISK_DEVICE_NAME);
  RtlInitUnicodeString(&fsCdDeviceName, DOKAN_GLOBAL_FS_CD_DEVICE_NAME);

  status = IoCreateDeviceSecure(DriverObject,         // DriverObject
                                sizeof(DOKAN_GLOBAL), // DeviceExtensionSize
                                &deviceName,          // DeviceName
                                FILE_DEVICE_UNKNOWN,  // DeviceType
                                0,                    // DeviceCharacteristics
                                FALSE,                // Not Exclusive
                                &sddl,                // Default SDDL String
                                NULL,                 // Device Class GUID
                                &deviceObject);       // DeviceObject

  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("IoCreateDevice returned 0x%x %s", status,
              DokanGetNTSTATUSStr(status));
    return status;
  }
  DOKAN_LOG_("\"%wZ\" created", &deviceName);

  // Create supported file system device types and register them

  status = IoCreateDeviceSecure(DriverObject,      // DriverObject
                                0,                 // DeviceExtensionSize
                                &fsDiskDeviceName, // DeviceName
                                FILE_DEVICE_DISK_FILE_SYSTEM, // DeviceType
                                0,                    // DeviceCharacteristics
                                FALSE,                // Not Exclusive
                                &sddl,                // Default SDDL String
                                NULL,                 // Device Class GUID
                                &fsDiskDeviceObject); // DeviceObject

  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("IoCreateDevice Disk FileSystem failed: 0x%x %s", status,
              DokanGetNTSTATUSStr(status));
    IoDeleteDevice(deviceObject);
    return status;
  }
  DOKAN_LOG_("DokanDiskFileSystemDevice: \"%wZ\" created", &fsDiskDeviceName);

  status = IoCreateDeviceSecure(DriverObject,    // DriverObject
                                0,               // DeviceExtensionSize
                                &fsCdDeviceName, // DeviceName
                                FILE_DEVICE_CD_ROM_FILE_SYSTEM, // DeviceType
                                0,                  // DeviceCharacteristics
                                FALSE,              // Not Exclusive
                                &sddl,              // Default SDDL String
                                NULL,               // Device Class GUID
                                &fsCdDeviceObject); // DeviceObject

  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("IoCreateDevice Cd FileSystem failed: 0x%x %s", status,
              DokanGetNTSTATUSStr(status));
    IoDeleteDevice(fsDiskDeviceObject);
    IoDeleteDevice(deviceObject);
    return status;
  }
  DOKAN_LOG_("DokanCdFileSystemDevice: \"%wZ\" created", &fsCdDeviceName);

  status = IoCreateSymbolicLink(&symbolicLinkName, &deviceName);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("IoCreateSymbolicLink returned 0x%x %s", status,
              DokanGetNTSTATUSStr(status));
    IoDeleteDevice(fsDiskDeviceObject);
    IoDeleteDevice(fsCdDeviceObject);
    IoDeleteDevice(deviceObject);
    return status;
  }
  DOKAN_LOG_("SymbolicLink: \"%wZ\" -> \"%wZ\" created", &deviceName,
            &symbolicLinkName);

  ObReferenceObject(deviceObject);

  dokanGlobal = deviceObject->DeviceExtension;
  dokanGlobal->DeviceObject = deviceObject;
  dokanGlobal->FsDiskDeviceObject = fsDiskDeviceObject;
  dokanGlobal->FsCdDeviceObject = fsCdDeviceObject;
  dokanGlobal->MountId = 0;
  dokanGlobal->DriverVersion = DOKAN_DRIVER_VERSION;

  InitializeListHead(&dokanGlobal->VcbList);
  InitializeListHead(&dokanGlobal->MountPointList);
  InitializeListHead(&dokanGlobal->SessionMountPointList);
  ExInitializeResourceLite(&dokanGlobal->Resource);
  ExInitializeResourceLite(&dokanGlobal->VcbListLock);
  ExInitializeResourceLite(&dokanGlobal->MountPointListLock);
  ExInitializeResourceLite(&dokanGlobal->MountManagerLock);
  ExInitializeFastMutex(&dokanGlobal->SessionMountPointLock);
  KeInitializeEvent(&dokanGlobal->UnloadEvent, NotificationEvent, FALSE);
  ExInitializeRundownProtection(&dokanGlobal->TeardownRundown);

  dokanGlobal->Identifier.Type = DGL;
  dokanGlobal->Identifier.Size = sizeof(DOKAN_GLOBAL);
  //
  // Request direct I/O user-buffer access method.
  //
  SetFlag(fsDiskDeviceObject->Flags, DO_DIRECT_IO);
  SetFlag(fsCdDeviceObject->Flags, DO_DIRECT_IO);
  //
  // Inserted FS next-to-last position in the queue
  // during IoRegisterFileSystem.
  //
  SetFlag(fsDiskDeviceObject->Flags, DO_LOW_PRIORITY_FILESYSTEM);
  SetFlag(fsCdDeviceObject->Flags, DO_LOW_PRIORITY_FILESYSTEM);

  //
  // The initialization is complete.
  //
  ClearFlag(fsDiskDeviceObject->Flags, DO_DEVICE_INITIALIZING);
  ClearFlag(fsCdDeviceObject->Flags, DO_DEVICE_INITIALIZING);

  // Register file systems
  IoRegisterFileSystem(fsDiskDeviceObject);
  IoRegisterFileSystem(fsCdDeviceObject);

  ObReferenceObject(fsDiskDeviceObject);
  ObReferenceObject(fsCdDeviceObject);

  *DokanGlobal = dokanGlobal;
  return STATUS_SUCCESS;
}

KSTART_ROUTINE DokanRegisterUncProvider;
VOID DokanRegisterUncProvider(__in PVOID pDcb) {
  NTSTATUS status;
  PDokanDCB Dcb = pDcb;

  if (Dcb->UNCName != NULL && Dcb->UNCName->Length > 0) {
    status =
        FsRtlRegisterUncProvider(&(Dcb->MupHandle), Dcb->DiskDeviceName, FALSE);
    DOKAN_LOG_("FsRtlRegisterUncProvider %s", DokanGetNTSTATUSStr(status));
    if (!NT_SUCCESS(status)) {
      Dcb->MupHandle = 0;
    }
  }
  PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID DokanDeregisterUncProvider(_In_ PVOID pDcb) {
  PDokanDCB Dcb = pDcb;
  if (Dcb->MupHandle) {
    FsRtlDeregisterUncProvider(Dcb->MupHandle);
    Dcb->MupHandle = 0;
  }
  PsTerminateSystemThread(STATUS_SUCCESS);
}

KSTART_ROUTINE DokanCreateMountPointSysProc;
VOID DokanCreateMountPointSysProc(__in PVOID pDcb) {
  NTSTATUS status;
  PDokanDCB Dcb = pDcb;

  if (IsUnmountPendingVcb(Dcb->Vcb)) {
    DOKAN_LOG("Device was in meantime deleted");
    return;
  }
  status = IoCreateSymbolicLink(Dcb->MountPoint, Dcb->DiskDeviceName);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("IoCreateSymbolicLink for mount point \"%wZ\" failed: 0x%X %s",
              Dcb->MountPoint, status, DokanGetNTSTATUSStr(status));
  }
}

// This function is deprecated and should not be used if MountManager & ResolveMountConflicts
// semantics are flagged on. The volume arrival notification should be
// sufficient and more correct in that case.
VOID DokanCreateMountPoint(__in PDokanDCB Dcb) {
  DOKAN_INIT_LOGGER(logger, Dcb->DriverObject, IRP_MJ_FILE_SYSTEM_CONTROL);

  if (Dcb->MountPoint != NULL && Dcb->MountPoint->Length > 0) {
    if (Dcb->UseMountManager) {
      DokanSendVolumeCreatePoint(Dcb->DriverObject, Dcb->DiskDeviceName,
                                 Dcb->MountPoint);
    } else {
      DokanLogInfo(&logger, L"Not using Mount Manager.");
      if (Dcb->MountGlobally) {
        // Run DokanCreateMountPointProc in system thread.
        RunAsSystem(DokanCreateMountPointSysProc, Dcb);
      } else {
        DokanCreateMountPointSysProc(Dcb);
      }
    }
  } else {
    DokanLogInfo(&logger, L"Mount point string is empty.");
  }
}

BOOLEAN DeleteMountPointSymbolicLink(__in PUNICODE_STRING MountPoint) {
  if (MountPoint == NULL || MountPoint->Length <= 0) {
    DOKAN_LOG("Mount Point is null");
    return FALSE;
  }

  DOKAN_LOG_("Delete Mount Point Symbolic Name: %wZ", MountPoint);
  NTSTATUS status = IoDeleteSymbolicLink(MountPoint);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("Delete Mount Point failed Symbolic Name: %wZ %s", MountPoint,
               DokanGetNTSTATUSStr(status));
    return FALSE;
  }
  return TRUE;
}

KSTART_ROUTINE DokanDeleteMountPointSysProc;
VOID DokanDeleteMountPointSysProc(__in PVOID pDcb) {
  PDokanDCB Dcb = pDcb;
  if (DeleteMountPointSymbolicLink(Dcb->MountPoint)) {
  	DOKAN_LOG_("Delete Mount Point Symbolic Name: \"%wZ\"", Dcb->MountPoint);
    SetLongFlag(Dcb->Flags, DCB_MOUNTPOINT_DELETED);
  }
}

VOID DokanDeleteMountPoint(__in_opt PREQUEST_CONTEXT RequestContext,
                           __in PDokanDCB Dcb) {
  DOKAN_INIT_LOGGER(logger, Dcb->DeviceObject->DriverObject, 0);
  if (Dcb->MountPoint != NULL && Dcb->MountPoint->Length > 0) {
    if (Dcb->UseMountManager) {
      Dcb->UseMountManager = FALSE;  // To avoid recursive call
      if (IsMountPointDriveLetter(Dcb->MountPoint)) {
        DokanLogInfo(&logger,
                     L"Issuing a clean mount manager delete for device \"%wZ\"",
                     Dcb->DiskDeviceName);
        // This is the correct way to do it. It makes the mount manager forget
        // the volume rather than leaving a do-not-assign record.
        DokanSendVolumeDeletePoints(NULL, Dcb->DiskDeviceName);
      } else if (Dcb->PersistentSymbolicLinkName) {
        // Remove the actual reparse point on our directory mount point.
        ULONG removeReparseInputlength = 0;
        PCHAR removeReparseInput = CreateRemoveReparsePointRequest(
            RequestContext, &removeReparseInputlength);
        if (removeReparseInput) {
          SendDirectoryFsctl(RequestContext, Dcb->DriverObject, Dcb->MountPoint,
                             FSCTL_DELETE_REPARSE_POINT, removeReparseInput,
                             removeReparseInputlength);
          DokanFree(removeReparseInput);
        }
        // Inform MountManager we are removing the reparse point.
        NotifyDirectoryMountPointDeleted(Dcb);
        // Remove the device from MountManager DB that should no longer have a
        // mount point attached.
        DokanSendVolumeDeletePoints(NULL, Dcb->DiskDeviceName);
      }
    } else {
      if (Dcb->MountGlobally) {
        // Run DokanDeleteMountPointProc in System thread.
        RunAsSystem(DokanDeleteMountPointSysProc, Dcb);
      } else {
        DOKAN_LOG("Device mounted for current session only so run "
                  "DokanDeleteMountPointProc without System thread.");
        DokanDeleteMountPointSysProc(Dcb);
        if (!IsFlagOn(Dcb->Flags, DCB_MOUNTPOINT_DELETED) &&
            Dcb->SessionId != (ULONG)-1) {
          // Could not delete session-local symlink from current session/System
          // context; record it for FSCTL_MOUNTPOINT_CLEANUP from that session.
          DokanRecordSessionMountPoint(Dcb->Global, Dcb->SessionId,
                                       Dcb->MountPoint);
        }
      }
    }
  }
}

//#define DOKAN_NET_PROVIDER

NTSTATUS
DokanSetVolumeSecurity(__in PDEVICE_OBJECT DeviceObject,
                       __in PSECURITY_DESCRIPTOR SecurityDescriptor) {
  NTSTATUS status = STATUS_SUCCESS;
  HANDLE deviceHandle = 0;

  __try {
    status = ObOpenObjectByPointer(DeviceObject, OBJ_KERNEL_HANDLE, NULL,
                                   WRITE_DAC, 0, KernelMode, &deviceHandle);
    if (!NT_SUCCESS(status)) {
      DOKAN_LOG_("Failed to open volume handle: 0x%x %s", status,
                DokanGetNTSTATUSStr(status));
      __leave;
    }

    status = ZwSetSecurityObject(deviceHandle, DACL_SECURITY_INFORMATION,
                                 SecurityDescriptor);
    if (!NT_SUCCESS(status)) {
      DOKAN_LOG_("ZeSetSecurityObject failed: 0x%x %s", status,
                DokanGetNTSTATUSStr(status));
      __leave;
    }
  }
  __finally {
    if (deviceHandle != 0) {
      ZwClose(deviceHandle);
    }
  }

  return status;
}

NTSTATUS
DokanCreateDiskDevice(__in PDRIVER_OBJECT DriverObject, __in ULONG MountId,
                      __in PWCHAR MountPoint, __in PWCHAR UNCName,
                      __in_opt PSECURITY_DESCRIPTOR VolumeSecurityDescriptor,
                      __in ULONG SessionId, __in PWCHAR BaseGuid,
                      __in PDOKAN_GLOBAL DokanGlobal,
                      __in DEVICE_TYPE DeviceType,
                      __in ULONG DeviceCharacteristics,
                      __in BOOLEAN MountGlobally, __in BOOLEAN UseMountManager,
                      __out PDOKAN_CONTROL DokanControl) {
  WCHAR *diskDeviceNameBuf = NULL;
  WCHAR *symbolicLinkNameBuf = NULL;
  WCHAR *mountPointBuf = NULL;
  PDEVICE_OBJECT diskDeviceObject = NULL;
  PDokanDCB dcb = NULL;
  UNICODE_STRING diskDeviceName;
  BOOLEAN isNetworkFileSystem = FALSE;
  NTSTATUS status = STATUS_SUCCESS;
  DOKAN_INIT_LOGGER(logger, DriverObject, 0);

  isNetworkFileSystem = (DeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);
  __try {
    DokanLogInfo(&logger,
                 L"Creating disk device; mount point = %s; mount ID = %ul",
                 MountPoint, MountId);
    diskDeviceNameBuf = DokanAllocZero(MAXIMUM_FILENAME_LENGTH * sizeof(WCHAR));
    symbolicLinkNameBuf =
        DokanAllocZero(MAXIMUM_FILENAME_LENGTH * sizeof(WCHAR));
    mountPointBuf = DokanAllocZero(MAXIMUM_FILENAME_LENGTH * sizeof(WCHAR));
    if (diskDeviceNameBuf == NULL || symbolicLinkNameBuf == NULL ||
        mountPointBuf == NULL) {
      status = DokanLogError(&logger, STATUS_INSUFFICIENT_RESOURCES,
          L"Could not allocate buffers while creating disk device.");
      __leave;
    }

    // make DeviceName and SymboliLink
    if (isNetworkFileSystem) {
#ifdef DOKAN_NET_PROVIDER
      RtlStringCchCopyW(diskDeviceNameBuf, MAXIMUM_FILENAME_LENGTH,
                        DOKAN_NET_DEVICE_NAME);
      RtlStringCchCopyW(symbolicLinkNameBuf, MAXIMUM_FILENAME_LENGTH,
                        DOKAN_NET_SYMBOLIC_LINK_NAME);
#else
      RtlStringCchCopyW(diskDeviceNameBuf, MAXIMUM_FILENAME_LENGTH,
                        DOKAN_NET_DEVICE_NAME);
      RtlStringCchCatW(diskDeviceNameBuf, MAXIMUM_FILENAME_LENGTH, BaseGuid);
      RtlStringCchCopyW(symbolicLinkNameBuf, MAXIMUM_FILENAME_LENGTH,
                        DOKAN_NET_SYMBOLIC_LINK_NAME);
      RtlStringCchCatW(symbolicLinkNameBuf, MAXIMUM_FILENAME_LENGTH, BaseGuid);
#endif
    } else {
      RtlStringCchCopyW(diskDeviceNameBuf, MAXIMUM_FILENAME_LENGTH,
                        DOKAN_DISK_DEVICE_NAME);
      RtlStringCchCatW(diskDeviceNameBuf, MAXIMUM_FILENAME_LENGTH, BaseGuid);
      RtlStringCchCopyW(symbolicLinkNameBuf, MAXIMUM_FILENAME_LENGTH,
                        DOKAN_SYMBOLIC_LINK_NAME);
      RtlStringCchCatW(symbolicLinkNameBuf, MAXIMUM_FILENAME_LENGTH, BaseGuid);
    }

    RtlInitUnicodeString(&diskDeviceName, diskDeviceNameBuf);

    //
    // Create DeviceObject for the Disk Device.
    // Network redirector mounts MUST leave the backing FILE_DEVICE_DISK
    // unnamed (NULL, via IoCreateDevice because IoCreateDeviceSecure rejects
    // NULL DeviceName without FILE_AUTOGENERATED_DEVICE_NAME): naming it causes
    // FltMgr!FltpFsControlMountVolume -> IoVolumeDeviceToGuid -> MountMgr ->
    // WdFilter!MpInstanceSetup -> FltCreateFileEx2 to re-enter IopMountVolume
    // on the same disk device while DeviceLock is held, self-deadlocking the
    // mount thread.
    //
    if (!isNetworkFileSystem) {
      status =
          IoCreateDeviceSecure(DriverObject,          // DriverObject
                               sizeof(DokanDCB),      // DeviceExtensionSize
                               &diskDeviceName,       // DeviceName
                               FILE_DEVICE_DISK,      // DeviceType
                               DeviceCharacteristics, // DeviceCharacteristics
                               FALSE,                 // Not Exclusive
                               &sddl,                 // Default SDDL String
                               NULL,                  // Device Class GUID
                               &diskDeviceObject);    // DeviceObject
    } else {
      status = IoCreateDevice(DriverObject,          // DriverObject
                              sizeof(DokanDCB),      // DeviceExtensionSize
                              NULL,                  // DeviceName
                              FILE_DEVICE_DISK,      // DeviceType
                              DeviceCharacteristics, // DeviceCharacteristics
                              FALSE,                 // Not Exclusive
                              &diskDeviceObject);    // DeviceObject
    }

    if (!NT_SUCCESS(status)) {
      DokanLogError(&logger, status,
                    isNetworkFileSystem
                        ? L"IoCreateDevice(FILE_DEVICE_DISK) failed."
                        : L"IoCreateDeviceSecure(FILE_DEVICE_DISK) failed.");
      __leave;
    }

    if (VolumeSecurityDescriptor != NULL) {
      status = DokanSetVolumeSecurity(diskDeviceObject,
                                      VolumeSecurityDescriptor);

      if (!NT_SUCCESS(status)) {
        DokanLogError(&logger, status,
                      L"Failed to set volume security descriptor.");
        IoDeleteDevice(diskDeviceObject);
        __leave;
      }

      DokanLogInfo(&logger, L"Set volume security descriptor successfully.");
    }

    //
    // Initialize the device extension.
    //
    dcb = diskDeviceObject->DeviceExtension;
    dcb->DeviceObject = diskDeviceObject;
    dcb->DriverObject = DriverObject;
    dcb->Global = DokanGlobal;

    dcb->Identifier.Type = DCB;
    dcb->Identifier.Size = sizeof(DokanDCB);
    dcb->Condition = DcbActive;

    dcb->MountId = MountId;
    dcb->VolumeDeviceType = DeviceType;
    dcb->DeviceType = FILE_DEVICE_DISK;
    dcb->DeviceCharacteristics = DeviceCharacteristics;
    dcb->SessionId = SessionId;
    KeInitializeEvent(&dcb->KillEvent, NotificationEvent, FALSE);
    // MaxLockedMinutes = 0: FSCTL_EVENT_PROCESS_N_PULL holds RemoveLock while
    // waiting indefinitely for events on an idle volume.
    IoInitializeRemoveLock(&dcb->RemoveLock, TAG, 0, 0);
    //
    // Establish user-buffer access method.
    //
    diskDeviceObject->Flags |= DO_DIRECT_IO;

    // initialize Event and Event queues
    DokanInitIrpList(&dcb->PendingIrp, /*EventEnabled=*/FALSE,
                     NotificationEvent);
    DokanInitIrpList(&dcb->NotifyEvent, /*EventEnabled=*/TRUE,
                     SynchronizationEvent);
    DokanInitIrpList(&dcb->PendingRetryIrp, /*EventEnabled=*/TRUE,
                     NotificationEvent);

    KeInitializeEvent(&dcb->ReleaseEvent, NotificationEvent, FALSE);
    ExInitializeResourceLite(&dcb->Resource);

    dcb->CacheManagerNoOpCallbacks.AcquireForLazyWrite = &DokanNoOpAcquire;
    dcb->CacheManagerNoOpCallbacks.ReleaseFromLazyWrite = &DokanNoOpRelease;
    dcb->CacheManagerNoOpCallbacks.AcquireForReadAhead = &DokanNoOpAcquire;
    dcb->CacheManagerNoOpCallbacks.ReleaseFromReadAhead = &DokanNoOpRelease;

    dcb->MountGlobally = MountGlobally;
    dcb->UseMountManager = UseMountManager;
    if (wcscmp(MountPoint, L"") != 0) {
      RtlStringCchCopyW(mountPointBuf, MAXIMUM_FILENAME_LENGTH,
                        L"\\DosDevices\\");
      if (wcslen(MountPoint) < 4) {
        mountPointBuf[12] = towupper(MountPoint[0]);
        mountPointBuf[13] = L':';
        mountPointBuf[14] = L'\0';
      } else {
        RtlStringCchCatW(mountPointBuf, MAXIMUM_FILENAME_LENGTH, MountPoint);
      }
      if (isNetworkFileSystem) {
        dcb->UseMountManager = FALSE;
      }
    } else {
      RtlStringCchCopyW(mountPointBuf, MAXIMUM_FILENAME_LENGTH, L"");
    }

    dcb->DiskDeviceName = DokanAllocateUnicodeString(diskDeviceNameBuf);
    dcb->SymbolicLinkName = DokanAllocateUnicodeString(symbolicLinkNameBuf);
    dcb->MountPoint = DokanAllocateUnicodeString(mountPointBuf);
    if (UNCName != NULL) {
      dcb->UNCName = DokanAllocateUnicodeString(UNCName);
    }

    if (dcb->DiskDeviceName == NULL || dcb->SymbolicLinkName == NULL ||
        dcb->MountPoint == NULL || (dcb->UNCName == NULL && UNCName != NULL)) {
      DOKAN_LOG("Failed to allocate memory for device naming");
      FreeDcbNames(dcb);
      ExDeleteResourceLite(&dcb->Resource);
      IoDeleteDevice(diskDeviceObject);
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }
    DokanLogInfo(&logger,
                 L"disk device name: \"%wZ\"; symbolic link name: \"%wZ\"; "
                 L"mount point: \"%wZ\"; type: %d",
                 dcb->DiskDeviceName, dcb->SymbolicLinkName, dcb->MountPoint,
                 DeviceType);

    //
    // Create a symbolic link for userapp to interact with the driver.
    //
    status = IoCreateSymbolicLink(dcb->SymbolicLinkName, dcb->DiskDeviceName);

    if (!NT_SUCCESS(status)) {
      FreeDcbNames(dcb);
      ExDeleteResourceLite(&dcb->Resource);
      IoDeleteDevice(diskDeviceObject);
      DokanLogError(&logger, status, L"IoCreateSymbolicLink failed.");
      __leave;
    }
    DokanLogInfo(&logger, L"SymbolicLink: \"%wZ\" -> \"%wZ\" created",
                 dcb->SymbolicLinkName, dcb->DiskDeviceName);

    // Mark devices as initialized
    diskDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    ObReferenceObject(diskDeviceObject);

    // Prepare the DOKAN_CONTROL struct that the caller will add to the mount
    // list.
    RtlZeroMemory(DokanControl, sizeof(DOKAN_CONTROL));
    RtlStringCchCopyW(DokanControl->DeviceName,
                      sizeof(DokanControl->DeviceName) / sizeof(WCHAR),
                      diskDeviceNameBuf);
    RtlStringCchCopyW(DokanControl->MountPoint,
                      sizeof(DokanControl->MountPoint) / sizeof(WCHAR),
                      mountPointBuf);
    if (UNCName != NULL) {
      RtlStringCchCopyW(DokanControl->UNCName,
                        sizeof(DokanControl->UNCName) / sizeof(WCHAR), UNCName);
    }
    DokanControl->Dcb = dcb;
    DokanControl->DiskDeviceObject = diskDeviceObject;
    DokanControl->Type = DeviceType;
    DokanControl->SessionId = dcb->SessionId;
  } __finally {
    if (diskDeviceNameBuf)
      DokanFree(diskDeviceNameBuf);
    if (symbolicLinkNameBuf)
      DokanFree(symbolicLinkNameBuf);
    if (mountPointBuf)
      DokanFree(mountPointBuf);
  }

  return status;
}
