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

#include <initguid.h>
#include <ntddstor.h>
#include <wdmsec.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanDeleteDeviceObject)
#endif

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
}

VOID DokanInitIrpList(__in PIRP_LIST IrpList, __in BOOLEAN EventEnabled) {
  InitializeListHead(&IrpList->ListHead);
  KeInitializeSpinLock(&IrpList->ListLock);
  KeInitializeEvent(&IrpList->NotEmpty, NotificationEvent, FALSE);
  IrpList->EventEnabled = EventEnabled;
}

PDEVICE_ENTRY
InsertDeviceToDelete(PDOKAN_GLOBAL dokanGlobal, PDEVICE_OBJECT DiskDeviceObject,
                     PDEVICE_OBJECT VolumeDeviceObject, BOOLEAN lockGlobal,
                     ULONG SessionId, PUNICODE_STRING MountPoint) {
  PDEVICE_ENTRY deviceEntry;

  ASSERT(DiskDeviceObject != NULL);

  deviceEntry = DokanAllocZero(sizeof(DEVICE_ENTRY));
  if (deviceEntry == NULL) {
    DOKAN_LOG("Allocation failed");
    return NULL;
  }
  deviceEntry->DiskDeviceObject = DiskDeviceObject;
  deviceEntry->VolumeDeviceObject = VolumeDeviceObject;
  deviceEntry->SessionId = SessionId;

  if (SessionId != -1) {
    deviceEntry->MountPoint.Buffer = DokanAlloc(MountPoint->MaximumLength);
    if (deviceEntry->MountPoint.Buffer == NULL) {
      DOKAN_LOG("MountPoint allocation failed");
      ExFreePool(deviceEntry);
      return NULL;
    }
    deviceEntry->MountPoint.MaximumLength = MountPoint->MaximumLength;
    RtlUnicodeStringCopy(&deviceEntry->MountPoint, MountPoint);
  }

  InitializeListHead(&deviceEntry->ListEntry);

  if (lockGlobal) {
    ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);
  }

  InsertTailList(&dokanGlobal->DeviceDeleteList, &deviceEntry->ListEntry);

  if (lockGlobal) {
    ExReleaseResourceLite(&dokanGlobal->Resource);
  }
  return deviceEntry;
}

PDEVICE_ENTRY
InsertDcbToDelete(PDokanDCB Dcb, PDEVICE_OBJECT VolumeDeviceObject,
                  BOOLEAN lockGlobal) {
  if (IsFlagOn(Dcb->Flags, DCB_MOUNTPOINT_DELETED)) {
    DOKAN_LOG("MountPoint has been deleted so reset the sessionid");
    Dcb->SessionId = (ULONG)-1;
  }
  return InsertDeviceToDelete(Dcb->Global, Dcb->DeviceObject,
                              VolumeDeviceObject, lockGlobal, Dcb->SessionId,
                              Dcb->MountPoint);
}

PMOUNT_ENTRY
InsertMountEntry(PDOKAN_GLOBAL dokanGlobal, PDOKAN_CONTROL DokanControl,
                 BOOLEAN lockGlobal) {
  PMOUNT_ENTRY mountEntry;
  mountEntry = DokanAllocZero(sizeof(MOUNT_ENTRY));
  if (mountEntry == NULL) {
    DOKAN_LOG("Allocation failed");
    return NULL;
  }
  RtlCopyMemory(&mountEntry->MountControl, DokanControl, sizeof(DOKAN_CONTROL));

  InitializeListHead(&mountEntry->ListEntry);

  if (lockGlobal) {
    ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);
  }

  InsertTailList(&dokanGlobal->MountPointList, &mountEntry->ListEntry);

  if (lockGlobal) {
    ExReleaseResourceLite(&dokanGlobal->Resource);
  }
  return mountEntry;
}

PDEVICE_ENTRY FindDeviceForDeleteBySessionId(PDOKAN_GLOBAL dokanGlobal,
                                             ULONG sessionId) {
  PLIST_ENTRY listHead = &dokanGlobal->DeviceDeleteList;
  PLIST_ENTRY entry;
  PLIST_ENTRY nextEntry;

  if (sessionId == -1) {
    return NULL;
  }

  if (IsListEmpty(&dokanGlobal->DeviceDeleteList)) {
    return NULL;
  }

  for (entry = listHead->Flink, nextEntry = entry->Flink; entry != listHead;
       entry = nextEntry, nextEntry = entry->Flink) {
    PDEVICE_ENTRY deviceEntry =
        CONTAINING_RECORD(entry, DEVICE_ENTRY, ListEntry);
    if (deviceEntry) {
      if (deviceEntry->SessionId == sessionId) {
        DOKAN_LOG("Device to delete found for a specific session\n");
        return deviceEntry;
      }
    }
  }
  return NULL;
}

VOID DeleteDeviceDelayed(PDOKAN_GLOBAL dokanGlobal) {

  PDEVICE_ENTRY deviceEntry;
  LIST_ENTRY completeList;
  PLIST_ENTRY listHead = &dokanGlobal->DeviceDeleteList;
  PLIST_ENTRY entry;
  PLIST_ENTRY nextEntry;
  ULONG totalCount = 0;
  PDokanDCB dcb;
  NTSTATUS status;

  InitializeListHead(&completeList);

  if (!ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, FALSE)) {
    DOKAN_LOG("Not able to acquire dokanGlobal->Resource");
    return;
  }

  if (IsListEmpty(&dokanGlobal->DeviceDeleteList)) {
    ExReleaseResourceLite(&dokanGlobal->Resource);
    return;
  }

  for (entry = listHead->Flink, nextEntry = entry->Flink; entry != listHead;
       entry = nextEntry, nextEntry = entry->Flink, totalCount++) {

    deviceEntry = CONTAINING_RECORD(entry, DEVICE_ENTRY, ListEntry);
    if (deviceEntry) {
      DOKAN_LOG_("There is a device for delayed delete. Counter %lu",
                deviceEntry->Counter);

      BOOLEAN canDeleteDiskDevice = FALSE;
      dcb = NULL;
      if (deviceEntry->DiskDeviceObject) {
        canDeleteDiskDevice =
            deviceEntry->DiskDeviceObject->ReferenceCount == 0;
        dcb = deviceEntry->DiskDeviceObject->DeviceExtension;
        InterlockedIncrement((LONG *)&deviceEntry->Counter);
      }

      // ensure that each device has the same delay in minimum
      // This way we can "ensure" that pending requests are processed
      // the value here of 3 is just a fantasy value.
      if (deviceEntry->Counter > 3) {

        if (deviceEntry->VolumeDeviceObject) {
          DOKAN_LOG_("Counter reached the limit. ReferenceCount %lu",
                    deviceEntry->VolumeDeviceObject->ReferenceCount);
          if (deviceEntry->VolumeDeviceObject->ReferenceCount == 0) {

            if (canDeleteDiskDevice) {

              DOKAN_LOG_("Delete Symbolic Name: \"%wZ\"", dcb->SymbolicLinkName);
              status = IoDeleteSymbolicLink(dcb->SymbolicLinkName);
              if (!NT_SUCCESS(status)) {
                DOKAN_LOG_("Delete of Symbolic failed Name: %wZ %s",
                           dcb->SymbolicLinkName, DokanGetNTSTATUSStr(status));
              }

              FreeDcbNames(dcb);

              DOKAN_LOG_("Delete the volume device. ReferenceCount %lu",
                        deviceEntry->VolumeDeviceObject->ReferenceCount);
              IoDeleteDevice(deviceEntry->VolumeDeviceObject);
              deviceEntry->VolumeDeviceObject = NULL;
            } else {
              DOKAN_LOG_(
                  "Disk device has still some references. "
                        "ReferenceCount %lu",
                        deviceEntry->DiskDeviceObject->ReferenceCount);
            }
          } else {
            canDeleteDiskDevice = FALSE;
            DOKAN_LOG_(
                "Counter reached the limit. Can't delete the volume "
                      "device. ReferenceCount %lu",
                      deviceEntry->VolumeDeviceObject->ReferenceCount);
          }
        }

        if (deviceEntry->DiskDeviceObject && canDeleteDiskDevice) {
          DOKAN_LOG_("Delete the disk device. ReferenceCount %lu",
                    deviceEntry->DiskDeviceObject->ReferenceCount);
          IoDeleteDevice(deviceEntry->DiskDeviceObject);
          if (deviceEntry->DiskDeviceObject->Vpb) {
            DOKAN_LOG("Volume->DeviceObject set to NULL");
            deviceEntry->DiskDeviceObject->Vpb->DeviceObject = NULL;
          }
          deviceEntry->DiskDeviceObject = NULL;
        }
      }

      if (deviceEntry->VolumeDeviceObject == NULL &&
          deviceEntry->DiskDeviceObject == NULL) {
        if (deviceEntry->SessionId == -1) {
          RemoveEntryList(&deviceEntry->ListEntry);
          InitializeListHead(&deviceEntry->ListEntry);
        } else {
          DOKAN_LOG("Device is just there because of the sessionId");
        }
      }
    }
  }

  DOKAN_LOG_("Total devices to delete in the list %lu", totalCount);

  ExReleaseResourceLite(&dokanGlobal->Resource);
}

KSTART_ROUTINE DokanDeleteDeviceThread;
VOID DokanDeleteDeviceThread(__in PVOID pdokanGlobal)
/*++

Routine Description:

checks wheter pending IRP is timeout or not each DOKAN_CHECK_INTERVAL

--*/
{
  NTSTATUS status;
  KTIMER timer;
  PVOID pollevents[2];
  LARGE_INTEGER timeout = {0};
  BOOLEAN waitObj = TRUE;
  PDOKAN_GLOBAL dokanGlobal = pdokanGlobal;

  DOKAN_LOG("Start");

  KeInitializeTimerEx(&timer, SynchronizationTimer);

  pollevents[0] = (PVOID)&dokanGlobal->KillDeleteDeviceEvent;
  pollevents[1] = (PVOID)&timer;

  KeSetTimerEx(&timer, timeout, DOKAN_CHECK_INTERVAL, NULL);

  while (waitObj) {
    status = KeWaitForMultipleObjects(2, pollevents, WaitAny, Executive,
                                      KernelMode, FALSE, NULL, NULL);

    if (!NT_SUCCESS(status) || status == STATUS_WAIT_0) {
      DOKAN_LOG("Catched KillEvent");
      // KillEvent or something error is occurred
      waitObj = FALSE;
    } else {
      DeleteDeviceDelayed(dokanGlobal);
    }
  }

  KeCancelTimer(&timer);

  DOKAN_LOG("Stop");

  PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
DokanStartDeleteDeviceThread(__in PDOKAN_GLOBAL dokanGlobal)
/*++

Routine Description:

execute DokanDeviceDeleteDelayedThread

--*/
{
  NTSTATUS status;
  HANDLE thread;

  status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                (PKSTART_ROUTINE)DokanDeleteDeviceThread,
                                dokanGlobal);

  if (!NT_SUCCESS(status)) {
    DOKAN_LOG("Failed to start thread");
    return status;
  }

  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode,
                            (PVOID *)&dokanGlobal->DeviceDeleteThread, NULL);

  ZwClose(thread);

  return STATUS_SUCCESS;
}

VOID RemoveMountEntry(PDOKAN_GLOBAL dokanGlobal, PMOUNT_ENTRY MountEntry) {
  ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);

  RemoveEntryList(&MountEntry->ListEntry);
  InitializeListHead(&MountEntry->ListEntry);

  ExReleaseResourceLite(&dokanGlobal->Resource);
  ExFreePool(MountEntry);
}

BOOLEAN IsMounted(__in PDEVICE_OBJECT DeviceObject) {
  PDokanVCB vcb;
  PDokanDCB dcb;

  if (DeviceObject == NULL)
    return FALSE;

  dcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(dcb) == DCB) {
    vcb = dcb->Vcb;
  } else {
    vcb = DeviceObject->DeviceExtension;
  }

  if (vcb == NULL)
    return FALSE;

  if (GetIdentifierType(vcb) == VCB) {
    dcb = vcb->Dcb;
    if (IsFlagOn(vcb->Flags, VCB_MOUNTED)) {
      if (dcb->DeviceObject->Vpb) {
        return IsFlagOn(dcb->DeviceObject->Vpb->Flags, VPB_MOUNTED);
      }
      return TRUE;
    }
  }

  return FALSE;
}

BOOLEAN IsDeletePending(__in PDEVICE_OBJECT DeviceObject) {
  PDokanDCB dcb;

  if (DeviceObject == NULL) {
    DOKAN_LOG("DeviceObject is null");
    return TRUE;
  }

  if (DeviceObject->Vpb == NULL && DeviceObject->DeviceExtension == NULL &&
      DeviceObject->Characteristics == 0) {
    DOKAN_LOG("This device seems to be deleted");
    return TRUE;
  }

  dcb = DeviceObject->DeviceExtension;
  if (dcb == NULL)
    return TRUE;

  if (GetIdentifierType(dcb) == DCB) {

    if (IsFlagOn(dcb->Flags, DCB_DELETE_PENDING)) {
      DOKAN_LOG("Delete is pending for this device");
      return TRUE;
    }
  }

  if (DeviceObject->DeviceObjectExtension &&
      DeviceObject->DeviceObjectExtension->ExtensionFlags &
          (DOE_UNLOAD_PENDING | DOE_DELETE_PENDING | DOE_REMOVE_PENDING |
           DOE_REMOVE_PROCESSED)) {
    DOKAN_LOG("This device is deleted");
    return TRUE;
  }

  return FALSE;
}

BOOLEAN IsUnmountPending(__in PDEVICE_OBJECT DeviceObject) {

  PDokanVCB vcb;
  PDokanDCB dcb;

  if (DeviceObject == NULL)
    return FALSE;

  dcb = DeviceObject->DeviceExtension;

  if (dcb == NULL)
    return FALSE;

  if (GetIdentifierType(dcb) == DCB) {
    vcb = dcb->Vcb;
  } else {
    vcb = DeviceObject->DeviceExtension;
  }

  return IsDeletePending(DeviceObject) || IsUnmountPendingVcb(vcb);
}

BOOLEAN IsUnmountPendingVcb(__in PDokanVCB vcb) {

  if (vcb == NULL) {
    return FALSE;
  }

  if (GetIdentifierType(vcb) == VCB) {
    if (IsFlagOn(vcb->Flags, VCB_DISMOUNT_PENDING)) {
      DOKAN_LOG("Volume is dismounting");
      return TRUE;
    }
    return IsDeletePending(vcb->Dcb->DeviceObject);
  }

  return FALSE;
}

PMOUNT_ENTRY
FindMountEntry(__in PDOKAN_GLOBAL dokanGlobal, __in PDOKAN_CONTROL DokanControl,
               __in BOOLEAN lockGlobal) {
  PLIST_ENTRY listEntry;
  PMOUNT_ENTRY mountEntry = NULL;
  BOOLEAN useMountPoint = (DokanControl->MountPoint[0] != L'\0');
  BOOLEAN found = FALSE;
  BOOLEAN isSessionIdMatch = FALSE;
  DOKAN_INIT_LOGGER(logger, dokanGlobal->DeviceObject->DriverObject, 0);

  DokanLogInfo(&logger,
               L"Finding mount entry; lockGlobal = %d; mount point = %s.",
               lockGlobal, DokanControl->MountPoint);

  if (lockGlobal) {
    ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);
  }

  for (listEntry = dokanGlobal->MountPointList.Flink;
       listEntry != &dokanGlobal->MountPointList;
       listEntry = listEntry->Flink) {
    mountEntry = CONTAINING_RECORD(listEntry, MOUNT_ENTRY, ListEntry);
    if (useMountPoint) {
      isSessionIdMatch =
          DokanControl->SessionId == mountEntry->MountControl.SessionId;
      if ((wcscmp(DokanControl->MountPoint,
                  mountEntry->MountControl.MountPoint) == 0) &&
          (isSessionIdMatch ||
           mountEntry->MountControl.SessionId == (ULONG)-1)) {
        DokanLogInfo(&logger, L"Found entry with matching mount point.");
        found = TRUE;
        break;
      } else {
        DokanLogInfo(&logger,
                     L"Skipping entry with non-matching mount point: %s",
                     mountEntry->MountControl.MountPoint);
      }
    } else {
      if (wcscmp(DokanControl->DeviceName,
                 mountEntry->MountControl.DeviceName) == 0) {
        DokanLogInfo(&logger, L"Found entry with matching device name: %s",
                     mountEntry->MountControl.DeviceName);
        found = TRUE;
        break;
      }
    }
  }

  if (lockGlobal) {
    ExReleaseResourceLite(&dokanGlobal->Resource);
  }

  if (found) {
    DokanLogInfo(&logger, L"Mount entry found: %s -> %s",
                 mountEntry->MountControl.MountPoint,
                 mountEntry->MountControl.DeviceName);
    return mountEntry;
  } else {
    DokanLogInfo(&logger, L"No mount entry found.");
    return NULL;
  }
}

PMOUNT_ENTRY FindMountEntryByName(__in PDOKAN_GLOBAL DokanGlobal,
                                  __in PUNICODE_STRING DiskDeviceName,
                                  __in PUNICODE_STRING UNCName,
                                  __in BOOLEAN LockGlobal) {
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
  mountEntry = FindMountEntry(DokanGlobal, dokanControl, LockGlobal);
  ExFreePool(dokanControl);
  return mountEntry;
}

NTSTATUS
DokanGetMountPointList(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PLIST_ENTRY listEntry;
  PMOUNT_ENTRY mountEntry;
  PDOKAN_MOUNT_POINT_INFO dokanMountPointInfo;
  USHORT i = 0;

  __try {
    ExAcquireResourceExclusiveLite(&RequestContext->DokanGlobal->Resource,
                                   TRUE);
    dokanMountPointInfo = (PDOKAN_MOUNT_POINT_INFO)
                              RequestContext->Irp->AssociatedIrp.SystemBuffer;
    for (listEntry = RequestContext->DokanGlobal->MountPointList.Flink;
         listEntry != &RequestContext->DokanGlobal->MountPointList;
         listEntry = listEntry->Flink, ++i) {
      if (!ExtendOutputBufferBySize(RequestContext->Irp,
                                    sizeof(DOKAN_MOUNT_POINT_INFO),
                                    /*UpdateInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      mountEntry = CONTAINING_RECORD(listEntry, MOUNT_ENTRY, ListEntry);

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
    }

    status = STATUS_SUCCESS;
  } __finally {
    ExReleaseResourceLite(&RequestContext->DokanGlobal->Resource);
  }

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

  ObReferenceObject(deviceObject);

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

  dokanGlobal = deviceObject->DeviceExtension;
  dokanGlobal->DeviceObject = deviceObject;
  dokanGlobal->FsDiskDeviceObject = fsDiskDeviceObject;
  dokanGlobal->FsCdDeviceObject = fsCdDeviceObject;
  dokanGlobal->MountId = 0;
  dokanGlobal->DriverVersion = DOKAN_DRIVER_VERSION;

  InitializeListHead(&dokanGlobal->MountPointList);
  InitializeListHead(&dokanGlobal->DeviceDeleteList);
  ExInitializeResourceLite(&dokanGlobal->Resource);
  ExInitializeResourceLite(&dokanGlobal->MountManagerLock);

  dokanGlobal->Identifier.Type = DGL;
  dokanGlobal->Identifier.Size = sizeof(DOKAN_GLOBAL);

  KeInitializeEvent(&dokanGlobal->KillDeleteDeviceEvent, NotificationEvent,
                    FALSE);
  DokanStartDeleteDeviceThread(dokanGlobal);
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

KSTART_ROUTINE DokanDeregisterUncProvider;
VOID DokanDeregisterUncProvider(__in PVOID pDcb) {
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
          SendDirectoryFsctl(RequestContext, Dcb->MountPoint,
                             FSCTL_DELETE_REPARSE_POINT, removeReparseInput,
                             removeReparseInputlength);
          ExFreePool(removeReparseInput);
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
    // Create DeviceObject for the Disk Device
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
          (isNetworkFileSystem
              ? L"IoCreateDevice(FILE_DEVICE_UNKNOWN) failed."
              : L"IoCreateDeviceSecure(FILE_DEVICE_DISK) failed."));
      __leave;
    }

    if (VolumeSecurityDescriptor != NULL) {
      status = DokanSetVolumeSecurity(diskDeviceObject,
                                      VolumeSecurityDescriptor);

      if (!NT_SUCCESS(status)) {
        DokanLogError(&logger, status,
                      L"Failed to set volume security descriptor.");
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

    dcb->MountId = MountId;
    dcb->VolumeDeviceType = DeviceType;
    dcb->DeviceType = FILE_DEVICE_DISK;
    dcb->DeviceCharacteristics = DeviceCharacteristics;
    dcb->SessionId = SessionId;
    KeInitializeEvent(&dcb->KillEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&dcb->ForceTimeoutEvent, NotificationEvent, FALSE);
    IoInitializeRemoveLock(&dcb->RemoveLock, TAG, 1, 100);
    //
    // Establish user-buffer access method.
    //
    diskDeviceObject->Flags |= DO_DIRECT_IO;

    // initialize Event and Event queue
    DokanInitIrpList(&dcb->PendingIrp, /*EventEnabled=*/FALSE);
    DokanInitIrpList(&dcb->NotifyEvent, /*EventEnabled=*/TRUE);
    DokanInitIrpList(&dcb->PendingRetryIrp, /*EventEnabled=*/TRUE);
    RtlZeroMemory(&dcb->NotifyIrpEventQueueList, sizeof(LIST_ENTRY));
    InitializeListHead(&dcb->NotifyIrpEventQueueList);
    KeInitializeQueue(&dcb->NotifyIrpEventQueue, 0);

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
      ExDeleteResourceLite(&dcb->Resource);
      IoDeleteDevice(diskDeviceObject);
      FreeDcbNames(dcb);
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
      ExFreePool(diskDeviceNameBuf);
    if (symbolicLinkNameBuf)
      ExFreePool(symbolicLinkNameBuf);
    if (mountPointBuf)
      ExFreePool(mountPointBuf);
  }

  return status;
}

VOID DokanDeleteDeviceObject(__in_opt PREQUEST_CONTEXT RequestContext,
                             __in PDokanDCB Dcb) {
  PDokanVCB vcb;
  DOKAN_CONTROL dokanControl;
  PMOUNT_ENTRY mountEntry = NULL;
  DOKAN_INIT_LOGGER(logger, Dcb->DeviceObject->DriverObject, 0);

  PAGED_CODE();

  UNREFERENCED_PARAMETER(RequestContext);

  vcb = Dcb->Vcb;

  if (Dcb->SymbolicLinkName == NULL) {
    DokanLogInfo(&logger, L"Symbolic name already deleted.");
    return;
  }

  DokanLogInfo(&logger, L"Deleting device object.");
  RtlZeroMemory(&dokanControl, sizeof(DOKAN_CONTROL));
  RtlCopyMemory(dokanControl.DeviceName, Dcb->DiskDeviceName->Buffer,
                Dcb->DiskDeviceName->Length);
  dokanControl.SessionId = Dcb->SessionId;
  mountEntry = FindMountEntry(Dcb->Global, &dokanControl, TRUE);
  if (mountEntry != NULL) {
    if (mountEntry->MountControl.Type == FILE_DEVICE_NETWORK_FILE_SYSTEM) {
      // Run FsRtlDeregisterUncProvider in System thread.
      RunAsSystem(DokanDeregisterUncProvider, Dcb);
    }
    DokanLogInfo(&logger, L"Removing mount entry.");
    RemoveMountEntry(Dcb->Global, mountEntry);
  } else {
    DOKAN_LOG_FINE_IRP(RequestContext, "Cannot found associated mount entry.");
  }

  if (Dcb->MountedDeviceInterfaceName.Buffer != NULL) {
    DokanLogInfo(&logger,
                 L"Changing interface state to false for mounted device \"%wZ\"",
                 &Dcb->MountedDeviceInterfaceName);
    IoSetDeviceInterfaceState(&Dcb->MountedDeviceInterfaceName, FALSE);

    RtlFreeUnicodeString(&Dcb->MountedDeviceInterfaceName);
    RtlInitUnicodeString(&Dcb->MountedDeviceInterfaceName, NULL);
  }
  if (Dcb->DiskDeviceInterfaceName.Buffer != NULL) {
    DokanLogInfo(&logger,
                 L"Changing interface state to false for disk device \"%wZ\"",
                 &Dcb->DiskDeviceInterfaceName);
    IoSetDeviceInterfaceState(&Dcb->DiskDeviceInterfaceName, FALSE);

    RtlFreeUnicodeString(&Dcb->DiskDeviceInterfaceName);
    RtlInitUnicodeString(&Dcb->DiskDeviceInterfaceName, NULL);
  }

  PDEVICE_OBJECT volumeDeviceObject = NULL;

  if (vcb != NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext, "FCB allocated: %d", vcb->FcbAllocated);
    DOKAN_LOG_FINE_IRP(RequestContext, "FCB     freed: %d", vcb->FcbFreed);
    DOKAN_LOG_FINE_IRP(RequestContext, "CCB allocated: %d", vcb->CcbAllocated);
    DOKAN_LOG_FINE_IRP(RequestContext, "CCB     freed: %d", vcb->CcbFreed);

    CleanDokanLogEntry(vcb);

    DokanLogInfo(&logger, L"Deleting volume device object.");
    volumeDeviceObject = vcb->DeviceObject;
  }

  InsertDcbToDelete(Dcb, volumeDeviceObject, FALSE);

  DokanLogInfo(&logger, L"Finished deleting device.");
}
