/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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
#include <initguid.h>
#include <mountmgr.h>
#include <ntddstor.h>
#include <wdmsec.h>

NTSTATUS IsMountPointDriveLetter(__in PUNICODE_STRING mountPoint) {
  if (mountPoint != NULL) {
    // Check if mount point match \DosDevices\C:
    USHORT length = mountPoint->Length / sizeof(WCHAR);
    if (length > 12 && length <= 15) {
      return STATUS_SUCCESS;
    }
  }

  return STATUS_INVALID_PARAMETER;
}

NTSTATUS
DokanSendIoContlToMountManager(__in ULONG IoControlCode, __in PVOID InputBuffer,
                               __in ULONG Length, __out PVOID OutputBuffer,
                               __in ULONG OutputLength) {
  NTSTATUS status;
  UNICODE_STRING mountManagerName;
  PFILE_OBJECT mountFileObject;
  PDEVICE_OBJECT mountDeviceObject;
  PIRP irp;
  KEVENT driverEvent;
  IO_STATUS_BLOCK iosb;

  DDbgPrint("=> DokanSendIoContlToMountManager\n");

  RtlInitUnicodeString(&mountManagerName, MOUNTMGR_DEVICE_NAME);

  status = IoGetDeviceObjectPointer(&mountManagerName, FILE_READ_ATTRIBUTES,
                                    &mountFileObject, &mountDeviceObject);

  if (!NT_SUCCESS(status)) {
    DDbgPrint("  IoGetDeviceObjectPointer failed: 0x%x\n", status);
    return status;
  }

  KeInitializeEvent(&driverEvent, NotificationEvent, FALSE);

  irp = IoBuildDeviceIoControlRequest(IoControlCode, mountDeviceObject,
                                      InputBuffer, Length, OutputBuffer,
                                      OutputLength, FALSE, &driverEvent, &iosb);

  if (irp == NULL) {
    DDbgPrint("  IoBuildDeviceIoControlRequest failed\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  status = IoCallDriver(mountDeviceObject, irp);

  if (status == STATUS_PENDING) {
    KeWaitForSingleObject(&driverEvent, Executive, KernelMode, FALSE, NULL);
  }
  status = iosb.Status;

  ObDereferenceObject(mountFileObject);
  // Don't dereference mountDeviceObject, mountFileObject is enough

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoCallDriver success\n");
  } else {
    DDbgPrint("  IoCallDriver failed: 0x%x\n", status);
  }

  DDbgPrint("<= DokanSendIoContlToMountManager\n");

  return status;
}

NTSTATUS
DokanSendVolumeArrivalNotification(PUNICODE_STRING DeviceName) {
  NTSTATUS status;
  PMOUNTMGR_TARGET_NAME targetName;
  ULONG length;

  DDbgPrint("=> DokanSendVolumeArrivalNotification\n");

  length = sizeof(MOUNTMGR_TARGET_NAME) + DeviceName->Length - 1;
  targetName = ExAllocatePool(length);

  if (targetName == NULL) {
    DDbgPrint("  can't allocate MOUNTMGR_TARGET_NAME\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlZeroMemory(targetName, length);

  targetName->DeviceNameLength = DeviceName->Length;
  RtlCopyMemory(targetName->DeviceName, DeviceName->Buffer, DeviceName->Length);

  status = DokanSendIoContlToMountManager(
      IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION, targetName, length, NULL, 0);

  if (NT_SUCCESS(status)) {
    DDbgPrint(
        "  IoCallDriver IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION success\n");
  } else {
    DDbgPrint("  IoCallDriver IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION "
              "failed: 0x%x\n",
              status);
  }

  ExFreePool(targetName);

  DDbgPrint("<= DokanSendVolumeArrivalNotification\n");

  return status;
}

NTSTATUS
DokanSendVolumeDeletePoints(__in PUNICODE_STRING MountPoint,
                            __in PUNICODE_STRING DeviceName) {
  NTSTATUS status;
  PMOUNTMGR_MOUNT_POINT point;
  PMOUNTMGR_MOUNT_POINTS deletedPoints;
  ULONG length;
  ULONG olength;

  DDbgPrint("=> DokanSendVolumeDeletePoints\n");

  length = sizeof(MOUNTMGR_MOUNT_POINT) + MountPoint->Length;
  if (DeviceName != NULL) {
    length += DeviceName->Length;
  }
  point = ExAllocatePool(length);

  if (point == NULL) {
    DDbgPrint("  can't allocate MOUNTMGR_CREATE_POINT_INPUT\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  olength = sizeof(MOUNTMGR_MOUNT_POINTS) + 1024;
  deletedPoints = ExAllocatePool(olength);
  if (deletedPoints == NULL) {
    DDbgPrint("  can't allocate PMOUNTMGR_MOUNT_POINTS\n");
    ExFreePool(point);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlZeroMemory(point, length);
  RtlZeroMemory(deletedPoints, olength);

  DDbgPrint("  MountPoint: %wZ\n", MountPoint);
  point->SymbolicLinkNameOffset = sizeof(MOUNTMGR_MOUNT_POINT);
  point->SymbolicLinkNameLength = MountPoint->Length;
  RtlCopyMemory((PCHAR)point + point->SymbolicLinkNameOffset,
                MountPoint->Buffer, MountPoint->Length);
  if (DeviceName != NULL) {
    DDbgPrint("  DeviceName: %wZ\n", DeviceName);
    point->DeviceNameOffset =
        point->SymbolicLinkNameOffset + point->SymbolicLinkNameLength;
    point->DeviceNameLength = DeviceName->Length;
    RtlCopyMemory((PCHAR)point + point->DeviceNameOffset, DeviceName->Buffer,
                  DeviceName->Length);
  }

  status = DokanSendIoContlToMountManager(IOCTL_MOUNTMGR_DELETE_POINTS, point,
                                          length, deletedPoints, olength);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoCallDriver success, %d mount points deleted.\n",
              deletedPoints->NumberOfMountPoints);
  } else {
    DDbgPrint("  IoCallDriver failed: 0x%x\n", status);
  }

  ExFreePool(point);
  ExFreePool(deletedPoints);

  DDbgPrint("<= DokanSendVolumeDeletePoints\n");

  return status;
}

NTSTATUS
DokanSendVolumeCreatePoint(__in PUNICODE_STRING DeviceName,
                           __in PUNICODE_STRING MountPoint) {
  NTSTATUS status;
  PMOUNTMGR_CREATE_POINT_INPUT point;
  ULONG length;

  DDbgPrint("=> DokanSendVolumeCreatePoint\n");

  length = sizeof(MOUNTMGR_CREATE_POINT_INPUT) + MountPoint->Length +
           DeviceName->Length;
  point = ExAllocatePool(length);

  if (point == NULL) {
    DDbgPrint("  can't allocate MOUNTMGR_CREATE_POINT_INPUT\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlZeroMemory(point, length);

  DDbgPrint("  DeviceName: %wZ\n", DeviceName);
  point->DeviceNameOffset = sizeof(MOUNTMGR_CREATE_POINT_INPUT);
  point->DeviceNameLength = DeviceName->Length;
  RtlCopyMemory((PCHAR)point + point->DeviceNameOffset, DeviceName->Buffer,
                DeviceName->Length);

  DDbgPrint("  MountPoint: %wZ\n", MountPoint);
  point->SymbolicLinkNameOffset =
      point->DeviceNameOffset + point->DeviceNameLength;
  point->SymbolicLinkNameLength = MountPoint->Length;
  RtlCopyMemory((PCHAR)point + point->SymbolicLinkNameOffset,
                MountPoint->Buffer, MountPoint->Length);

  status = DokanSendIoContlToMountManager(IOCTL_MOUNTMGR_CREATE_POINT, point,
                                          length, NULL, 0);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoCallDriver success\n");
  } else {
    DDbgPrint("  IoCallDriver failed: 0x%x\n", status);
  }

  ExFreePool(point);

  DDbgPrint("<= DokanSendVolumeCreatePoint\n");

  return status;
}

NTSTATUS
DokanRegisterMountedDeviceInterface(__in PDEVICE_OBJECT DeviceObject,
                                    __in PDokanDCB Dcb) {
  NTSTATUS status;
  UNICODE_STRING interfaceName;
  DDbgPrint("=> DokanRegisterMountedDeviceInterface\n");

  status = IoRegisterDeviceInterface(
      DeviceObject, &MOUNTDEV_MOUNTED_DEVICE_GUID, NULL, &interfaceName);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  InterfaceName:%wZ\n", &interfaceName);

    Dcb->MountedDeviceInterfaceName = interfaceName;
    status = IoSetDeviceInterfaceState(&interfaceName, TRUE);

    if (!NT_SUCCESS(status)) {
      DDbgPrint("  IoSetDeviceInterfaceState failed: 0x%x\n", status);
      RtlFreeUnicodeString(&interfaceName);
    }
  } else {
    DDbgPrint("  IoRegisterDeviceInterface failed: 0x%x\n", status);
  }

  if (!NT_SUCCESS(status)) {
    RtlInitUnicodeString(&(Dcb->MountedDeviceInterfaceName), NULL);
  }
  DDbgPrint("<= DokanRegisterMountedDeviceInterface\n");
  return status;
}

NTSTATUS
DokanRegisterDeviceInterface(__in PDRIVER_OBJECT DriverObject,
                             __in PDEVICE_OBJECT DeviceObject,
                             __in PDokanDCB Dcb) {
  PDEVICE_OBJECT pnpDeviceObject = NULL;
  NTSTATUS status;
  DDbgPrint("=> DokanRegisterDeviceInterface\n");

  status = IoReportDetectedDevice(DriverObject, InterfaceTypeUndefined, 0, 0,
                                  NULL, NULL, FALSE, &pnpDeviceObject);
  pnpDeviceObject->DeviceExtension = Dcb;
  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoReportDetectedDevice success\n");
  } else {
    DDbgPrint("  IoReportDetectedDevice failed: 0x%x\n", status);
    return status;
  }

  if (IoAttachDeviceToDeviceStack(pnpDeviceObject, DeviceObject) != NULL) {
    DDbgPrint("  IoAttachDeviceToDeviceStack success\n");
  } else {
    DDbgPrint("  IoAttachDeviceToDeviceStack failed\n");
  }

  status = IoRegisterDeviceInterface(pnpDeviceObject, &GUID_DEVINTERFACE_DISK,
                                     NULL, &Dcb->DiskDeviceInterfaceName);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoRegisterDeviceInterface success: %wZ\n",
              &Dcb->DiskDeviceInterfaceName);
  } else {
    RtlInitUnicodeString(&Dcb->DiskDeviceInterfaceName, NULL);
    DDbgPrint("  IoRegisterDeviceInterface failed: 0x%x\n", status);
    return status;
  }

  status = IoSetDeviceInterfaceState(&Dcb->DiskDeviceInterfaceName, TRUE);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoSetDeviceInterfaceState success\n");
  } else {
    DDbgPrint("  IoSetDeviceInterfaceState failed: 0x%x\n", status);
    return status;
  }

  status =
      IoRegisterDeviceInterface(pnpDeviceObject, &MOUNTDEV_MOUNTED_DEVICE_GUID,
                                NULL, &Dcb->MountedDeviceInterfaceName);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoRegisterDeviceInterface success: %wZ\n",
              &Dcb->MountedDeviceInterfaceName);
  } else {
    DDbgPrint("  IoRegisterDeviceInterface failed: 0x%x\n", status);
    return status;
  }

  status = IoSetDeviceInterfaceState(&Dcb->MountedDeviceInterfaceName, TRUE);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoSetDeviceInterfaceState success\n");
  } else {
    RtlInitUnicodeString(&Dcb->MountedDeviceInterfaceName, NULL);
    DDbgPrint("  IoSetDeviceInterfaceState failed: 0x%x\n", status);
    return status;
  }

  DDbgPrint("<= DokanRegisterDeviceInterface\n");
  return status;
}

VOID DokanInitIrpList(__in PIRP_LIST IrpList) {
  InitializeListHead(&IrpList->ListHead);
  KeInitializeSpinLock(&IrpList->ListLock);
  KeInitializeEvent(&IrpList->NotEmpty, NotificationEvent, FALSE);
}

PMOUNT_ENTRY
InsertMountEntry(PDOKAN_GLOBAL dokanGlobal, PDOKAN_CONTROL DokanControl) {
  PMOUNT_ENTRY mountEntry;
  mountEntry = ExAllocatePool(sizeof(MOUNT_ENTRY));
  if (mountEntry == NULL) {
    DDbgPrint("  InsertMountEntry allocation failed\n");
    return NULL;
  }
  RtlZeroMemory(mountEntry, sizeof(MOUNT_ENTRY));
  RtlCopyMemory(&mountEntry->MountControl, DokanControl, sizeof(DOKAN_CONTROL));

  InitializeListHead(&mountEntry->ListEntry);

  ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);
  InsertTailList(&dokanGlobal->MountPointList, &mountEntry->ListEntry);
  ExReleaseResourceLite(&dokanGlobal->Resource);

  return mountEntry;
}

VOID RemoveMountEntry(PDOKAN_GLOBAL dokanGlobal, PMOUNT_ENTRY MountEntry) {
  ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);
  RemoveEntryList(&MountEntry->ListEntry);
  ExReleaseResourceLite(&dokanGlobal->Resource);

  ExFreePool(MountEntry);
}

PMOUNT_ENTRY
FindMountEntry(__in PDOKAN_GLOBAL dokanGlobal,
               __out PDOKAN_CONTROL DokanControl) {
  PLIST_ENTRY listEntry;
  PMOUNT_ENTRY mountEntry = NULL;
  BOOLEAN useMountPoint = (DokanControl->MountPoint[0] != L'\0');
  BOOLEAN found = FALSE;

  ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);

  for (listEntry = dokanGlobal->MountPointList.Flink;
       listEntry != &dokanGlobal->MountPointList;
       listEntry = listEntry->Flink) {
    mountEntry = CONTAINING_RECORD(listEntry, MOUNT_ENTRY, ListEntry);
    if (useMountPoint) {
      if (wcscmp(DokanControl->MountPoint,
                 mountEntry->MountControl.MountPoint) == 0) {
        found = TRUE;
        break;
      }
    } else {
      if (wcscmp(DokanControl->DeviceName,
                 mountEntry->MountControl.DeviceName) == 0) {
        found = TRUE;
        break;
      }
    }
  }

  ExReleaseResourceLite(&dokanGlobal->Resource);

  if (found) {
    DDbgPrint("FindMountEntry %ws -> %ws\n",
              mountEntry->MountControl.MountPoint,
              mountEntry->MountControl.DeviceName);
    return mountEntry;
  } else {
    return NULL;
  }
}

NTSTATUS
DokanGetMountPointList(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp,
                       __in PDOKAN_GLOBAL dokanGlobal) {
  UNREFERENCED_PARAMETER(DeviceObject);
  PIO_STACK_LOCATION irpSp = NULL;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PLIST_ENTRY listEntry;
  PMOUNT_ENTRY mountEntry;
  PDOKAN_CONTROL dokanControl;
  int i = 0;

  DDbgPrint("==> DokanGetMountPointList\n");
  irpSp = IoGetCurrentIrpStackLocation(Irp);

  try {
    dokanControl = (PDOKAN_CONTROL)Irp->AssociatedIrp.SystemBuffer;
    for (listEntry = dokanGlobal->MountPointList.Flink;
         listEntry != &dokanGlobal->MountPointList;
         listEntry = listEntry->Flink, ++i) {
      Irp->IoStatus.Information = sizeof(DOKAN_CONTROL) * (i + 1);
      if (irpSp->Parameters.DeviceIoControl.OutputBufferLength <
          (sizeof(DOKAN_CONTROL) * (i + 1))) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      mountEntry = CONTAINING_RECORD(listEntry, MOUNT_ENTRY, ListEntry);
      RtlCopyMemory(&dokanControl[i], &mountEntry->MountControl,
                    sizeof(DOKAN_CONTROL));
    }

    status = STATUS_SUCCESS;
  } finally {
  }

  DDbgPrint("<== DokanGetMountPointList\n");
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
    DDbgPrint("  IoCreateDevice returned 0x%x\n", status);
    return status;
  }
  DDbgPrint("DokanGlobalDevice: %wZ created\n", &deviceName);

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
    DDbgPrint("  IoCreateDevice Disk FileSystem failed: 0x%x\n", status);
    IoDeleteDevice(deviceObject);
    return status;
  }
  DDbgPrint("DokanDiskFileSystemDevice: %wZ created\n", &fsDiskDeviceName);

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
    DDbgPrint("  IoCreateDevice Cd FileSystem failed: 0x%x\n", status);
    IoDeleteDevice(fsDiskDeviceObject);
    IoDeleteDevice(deviceObject);
    return status;
  }
  DDbgPrint("DokanCdFileSystemDevice: %wZ created\n", &fsCdDeviceName);

  ObReferenceObject(deviceObject);

  status = IoCreateSymbolicLink(&symbolicLinkName, &deviceName);
  if (!NT_SUCCESS(status)) {
    DDbgPrint("  IoCreateSymbolicLink returned 0x%x\n", status);
    IoDeleteDevice(deviceObject);
    return status;
  }
  DDbgPrint("SymbolicLink: %wZ -> %wZ created\n", &deviceName,
            &symbolicLinkName);
  dokanGlobal = deviceObject->DeviceExtension;
  dokanGlobal->DeviceObject = deviceObject;
  dokanGlobal->FsDiskDeviceObject = fsDiskDeviceObject;
  dokanGlobal->FsCdDeviceObject = fsCdDeviceObject;

  RtlZeroMemory(dokanGlobal, sizeof(DOKAN_GLOBAL));
  DokanInitIrpList(&dokanGlobal->PendingService);
  DokanInitIrpList(&dokanGlobal->NotifyService);
  InitializeListHead(&dokanGlobal->MountPointList);

  dokanGlobal->Identifier.Type = DGL;
  dokanGlobal->Identifier.Size = sizeof(DOKAN_GLOBAL);

  //
  // Establish user-buffer access method.
  //
  fsDiskDeviceObject->Flags |= DO_DIRECT_IO;
  fsDiskDeviceObject->Flags |= DO_LOW_PRIORITY_FILESYSTEM;
  fsCdDeviceObject->Flags |= DO_DIRECT_IO;
  fsCdDeviceObject->Flags |= DO_LOW_PRIORITY_FILESYSTEM;

  fsDiskDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
  fsCdDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

  // Register file systems
  IoRegisterFileSystem(fsDiskDeviceObject);
  IoRegisterFileSystem(fsCdDeviceObject);

  ObReferenceObject(fsDiskDeviceObject);
  ObReferenceObject(fsCdDeviceObject);

  *DokanGlobal = dokanGlobal;
  return STATUS_SUCCESS;
}

PUNICODE_STRING
DokanAllocateUnicodeString(__in PCWSTR String) {
  PUNICODE_STRING unicode;
  PWSTR buffer;
  ULONG length;
  unicode = ExAllocatePool(sizeof(UNICODE_STRING));
  if (unicode == NULL) {
    return NULL;
  }

  length = (ULONG)(wcslen(String) + 1) * sizeof(WCHAR);
  buffer = ExAllocatePool(length);
  if (buffer == NULL) {
    ExFreePool(unicode);
    return NULL;
  }
  RtlCopyMemory(buffer, String, length);
  RtlInitUnicodeString(unicode, buffer);
  return unicode;
}

VOID FreeUnicodeString(PUNICODE_STRING UnicodeString) {
  if (UnicodeString != NULL) {
    ExFreePool(UnicodeString->Buffer);
    ExFreePool(UnicodeString);
  }
}

static VOID FreeDcbNames(__in PDokanDCB Dcb) {
  if (Dcb->MountPoint != NULL) {
    FreeUnicodeString(Dcb->MountPoint);
    Dcb->MountPoint = NULL;
  }
  if (Dcb->SymbolicLinkName != NULL) {
    FreeUnicodeString(Dcb->SymbolicLinkName);
    Dcb->SymbolicLinkName = NULL;
  }
  if (Dcb->DiskDeviceName != NULL) {
    FreeUnicodeString(Dcb->DiskDeviceName);
    Dcb->DiskDeviceName = NULL;
  }
  if (Dcb->UNCName != NULL) {
    FreeUnicodeString(Dcb->UNCName);
    Dcb->UNCName = NULL;
  }
}

KSTART_ROUTINE DokanRegisterUncProvider;
VOID DokanRegisterUncProvider(__in PDokanDCB Dcb) {
  NTSTATUS status;

  if (Dcb->UNCName != NULL && Dcb->UNCName->Length > 0) {
    status =
        FsRtlRegisterUncProvider(&(Dcb->MupHandle), Dcb->DiskDeviceName, FALSE);
    if (NT_SUCCESS(status)) {
      DDbgPrint("  FsRtlRegisterUncProvider success\n");
    } else {
      DDbgPrint("  FsRtlRegisterUncProvider failed: 0x%x\n", status);
      Dcb->MupHandle = 0;
    }
  }
  PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DokanRegisterUncProviderSystem(PDokanDCB dcb) {
  // Run FsRtlRegisterUncProvider in System thread.
  HANDLE handle;
  PKTHREAD thread;
  OBJECT_ATTRIBUTES objectAttribs;
  NTSTATUS status;

  InitializeObjectAttributes(&objectAttribs, NULL, OBJ_KERNEL_HANDLE, NULL,
                             NULL);
  status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &objectAttribs,
                                NULL, NULL,
                                (PKSTART_ROUTINE)DokanRegisterUncProvider, dcb);
  if (!NT_SUCCESS(status)) {
    DDbgPrint("PsCreateSystemThread failed: 0x%X\n", status);
  } else {
    ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, NULL, KernelMode,
                              &thread, NULL);
    ZwClose(handle);
    KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(thread);
  }
  return status;
}

KSTART_ROUTINE DokanDeregisterUncProvider;
VOID DokanDeregisterUncProvider(__in PDokanDCB Dcb) {
  if (Dcb->MupHandle) {
    FsRtlDeregisterUncProvider(Dcb->MupHandle);
    Dcb->MupHandle = 0;
  }
  PsTerminateSystemThread(STATUS_SUCCESS);
}

KSTART_ROUTINE DokanCreateMountPointSysProc;
VOID DokanCreateMountPointSysProc(__in PDokanDCB Dcb) {
  NTSTATUS status;

  DDbgPrint("=> DokanCreateMountPointSysProc\n");

  status = IoCreateSymbolicLink(Dcb->MountPoint, Dcb->DiskDeviceName);
  if (!NT_SUCCESS(status)) {
    DDbgPrint("IoCreateSymbolicLink for mount point %wZ failed: 0x%X\n",
              Dcb->MountPoint, status);
  }

  DDbgPrint("<= DokanCreateMountPointSysProc\n");
}

VOID DokanCreateMountPoint(__in PDokanDCB Dcb) {
  NTSTATUS status;

  if (Dcb->MountPoint != NULL && Dcb->MountPoint->Length > 0) {
    if (Dcb->UseMountManager) {
      DokanSendVolumeCreatePoint(Dcb->DiskDeviceName, Dcb->MountPoint);
    } else {
      if (Dcb->MountGlobally) {
        // Run DokanCreateMountPointProc in system thread.
        HANDLE handle;
        PKTHREAD thread;
        OBJECT_ATTRIBUTES objectAttribs;

        InitializeObjectAttributes(&objectAttribs, NULL, OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        status = PsCreateSystemThread(
            &handle, THREAD_ALL_ACCESS, &objectAttribs, NULL, NULL,
            (PKSTART_ROUTINE)DokanCreateMountPointSysProc, Dcb);
        if (!NT_SUCCESS(status)) {
          DDbgPrint("DokanCreateMountPoint PsCreateSystemThread failed: 0x%X\n",
                    status);
        } else {
          ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, NULL, KernelMode,
                                    &thread, NULL);
          ZwClose(handle);
          KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
          ObDereferenceObject(thread);
        }
      } else {
        DokanCreateMountPointSysProc(Dcb);
      }
    }
  }
}

KSTART_ROUTINE DokanDeleteMountPointSysProc;
VOID DokanDeleteMountPointSysProc(__in PDokanDCB Dcb) {
  DDbgPrint("=> DokanDeleteMountPointSysProc\n");
  if (Dcb->MountPoint != NULL && Dcb->MountPoint->Length > 0) {
    DDbgPrint("  Delete Mount Point Symbolic Name: %wZ\n", Dcb->MountPoint);
    IoDeleteSymbolicLink(Dcb->MountPoint);
  }
  DDbgPrint("<= DokanDeleteMountPointSysProc\n");
}

VOID DokanDeleteMountPoint(__in PDokanDCB Dcb) {
  NTSTATUS status;

  if (Dcb->MountPoint != NULL && Dcb->MountPoint->Length > 0) {
    if (Dcb->UseMountManager) {
      Dcb->UseMountManager = FALSE; // To avoid recursive call
      DokanSendVolumeDeletePoints(Dcb->MountPoint, Dcb->DiskDeviceName);
    } else {
      if (Dcb->MountGlobally) {
        // Run DokanDeleteMountPointProc in System thread.
        HANDLE handle;
        PKTHREAD thread;
        OBJECT_ATTRIBUTES objectAttribs;

        InitializeObjectAttributes(&objectAttribs, NULL, OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        status = PsCreateSystemThread(
            &handle, THREAD_ALL_ACCESS, &objectAttribs, NULL, NULL,
            (PKSTART_ROUTINE)DokanDeleteMountPointSysProc, Dcb);
        if (!NT_SUCCESS(status)) {
          DDbgPrint("DokanDeleteMountPoint PsCreateSystemThread failed: 0x%X\n",
                    status);
        } else {
          ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, NULL, KernelMode,
                                    &thread, NULL);
          ZwClose(handle);
          KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
          ObDereferenceObject(thread);
        }
      } else {
        DokanDeleteMountPointSysProc(Dcb);
      }
    }
  }
}

//#define DOKAN_NET_PROVIDER

NTSTATUS
DokanCreateDiskDevice(__in PDRIVER_OBJECT DriverObject, __in ULONG MountId,
                      __in PWCHAR MountPoint, __in PWCHAR UNCName,
                      __in PWCHAR BaseGuid, __in PDOKAN_GLOBAL DokanGlobal,
                      __in DEVICE_TYPE DeviceType,
                      __in ULONG DeviceCharacteristics,
                      __in BOOLEAN MountGlobally, __in BOOLEAN UseMountManager,
                      __out PDokanDCB *Dcb) {
  WCHAR diskDeviceNameBuf[MAXIMUM_FILENAME_LENGTH];
  WCHAR symbolicLinkNameBuf[MAXIMUM_FILENAME_LENGTH];
  WCHAR mountPointBuf[MAXIMUM_FILENAME_LENGTH];
  PDEVICE_OBJECT diskDeviceObject;
  PDokanDCB dcb;
  UNICODE_STRING diskDeviceName;
  NTSTATUS status;
  BOOLEAN isNetworkFileSystem = (DeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);
  DOKAN_CONTROL dokanControl;

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
    DDbgPrint("  %s failed: 0x%x\n",
              isNetworkFileSystem ? "IoCreateDevice(FILE_DEVICE_UNKNOWN)"
                                  : "IoCreateDeviceSecure(FILE_DEVICE_DISK)",
              status);
    return status;
  }

  //
  // Initialize the device extension.
  //
  dcb = diskDeviceObject->DeviceExtension;
  *Dcb = dcb;
  dcb->DeviceObject = diskDeviceObject;
  dcb->Global = DokanGlobal;

  dcb->Identifier.Type = DCB;
  dcb->Identifier.Size = sizeof(DokanDCB);

  dcb->MountId = MountId;
  dcb->VolumeDeviceType = DeviceType;
  dcb->DeviceType = FILE_DEVICE_DISK;
  dcb->DeviceCharacteristics = DeviceCharacteristics;
  KeInitializeEvent(&dcb->KillEvent, NotificationEvent, FALSE);

  //
  // Establish user-buffer access method.
  //
  diskDeviceObject->Flags |= DO_DIRECT_IO;

  // initialize Event and Event queue
  DokanInitIrpList(&dcb->PendingIrp);
  DokanInitIrpList(&dcb->PendingEvent);
  DokanInitIrpList(&dcb->NotifyEvent);

  KeInitializeEvent(&dcb->ReleaseEvent, NotificationEvent, FALSE);

  // "0" means not mounted
  dcb->Mounted = 0;

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
      if (isNetworkFileSystem) {
        dcb->UseMountManager = FALSE;
      }
    } else {
      dcb->UseMountManager = FALSE;
      RtlStringCchCatW(mountPointBuf, MAXIMUM_FILENAME_LENGTH, MountPoint);
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
    DDbgPrint("  Failed to allocate memory for device naming");
    FreeDcbNames(dcb);
    ExDeleteResourceLite(&dcb->Resource);
    IoDeleteDevice(diskDeviceObject);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  DDbgPrint("DiskDeviceName: %wZ - SymbolicLinkName: %wZ - MountPoint: %wZ\n",
            dcb->DiskDeviceName, dcb->SymbolicLinkName, dcb->MountPoint);

  DDbgPrint("  IoCreateDevice DeviceType: %d\n", DeviceType);

  //
  // Create a symbolic link for userapp to interact with the driver.
  //
  status = IoCreateSymbolicLink(dcb->SymbolicLinkName, dcb->DiskDeviceName);

  if (!NT_SUCCESS(status)) {
    ExDeleteResourceLite(&dcb->Resource);
    IoDeleteDevice(diskDeviceObject);
    FreeDcbNames(dcb);
    DDbgPrint("  IoCreateSymbolicLink returned 0x%x\n", status);
    return status;
  }
  DDbgPrint("SymbolicLink: %wZ -> %wZ created\n", dcb->SymbolicLinkName,
            dcb->DiskDeviceName);

  // Mark devices as initialized
  diskDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

  ObReferenceObject(diskDeviceObject);

  // DokanRegisterDeviceInterface(DriverObject, dcb->DeviceObject, dcb);
  // DokanRegisterMountedDeviceInterface(dcb->DeviceObject, dcb);

  // Save to the global mounted list
  RtlZeroMemory(&dokanControl, sizeof(dokanControl));
  RtlStringCchCopyW(dokanControl.DeviceName,
                    sizeof(dokanControl.DeviceName) / sizeof(WCHAR),
                    diskDeviceNameBuf);
  RtlStringCchCopyW(dokanControl.MountPoint,
                    sizeof(dokanControl.MountPoint) / sizeof(WCHAR),
                    mountPointBuf);
  if (UNCName != NULL) {
    RtlStringCchCopyW(dokanControl.UNCName,
                      sizeof(dokanControl.UNCName) / sizeof(WCHAR), UNCName);
  }
  dokanControl.Type = DeviceType;

  InsertMountEntry(DokanGlobal, &dokanControl);

  IoVerifyVolume(dcb->DeviceObject, FALSE);

  return STATUS_SUCCESS;
}

VOID DokanDeleteDeviceObject(__in PDokanDCB Dcb) {
  PDokanVCB vcb;
  DOKAN_CONTROL dokanControl;
  PMOUNT_ENTRY mountEntry = NULL;

  ASSERT(GetIdentifierType(Dcb) == DCB);
  vcb = Dcb->Vcb;

  if (Dcb->SymbolicLinkName == NULL) {
    DDbgPrint("  Symbolic Name already deleted, so go out here\n");
    return;
  }

  RtlZeroMemory(&dokanControl, sizeof(dokanControl));
  RtlCopyMemory(dokanControl.DeviceName, Dcb->DiskDeviceName->Buffer,
                Dcb->DiskDeviceName->Length);
  mountEntry = FindMountEntry(Dcb->Global, &dokanControl);
  if (mountEntry != NULL) {
    if (mountEntry->MountControl.Type == FILE_DEVICE_NETWORK_FILE_SYSTEM) {
      // Run FsRtlDeregisterUncProvider in System thread.
      HANDLE handle;
      PKTHREAD thread;
      OBJECT_ATTRIBUTES objectAttribs;
      NTSTATUS status;

      InitializeObjectAttributes(&objectAttribs, NULL, OBJ_KERNEL_HANDLE, NULL,
                                 NULL);
      status = PsCreateSystemThread(
          &handle, THREAD_ALL_ACCESS, &objectAttribs, NULL, NULL,
          (PKSTART_ROUTINE)DokanDeregisterUncProvider, Dcb);
      if (!NT_SUCCESS(status)) {
        DDbgPrint("PsCreateSystemThread failed: 0x%X\n", status);
      } else {
        ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, NULL, KernelMode,
                                  &thread, NULL);
        ZwClose(handle);
        KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(thread);
      }
    }
    RemoveMountEntry(Dcb->Global, mountEntry);
  } else {
    DDbgPrint("  Cannot found associated mount entry.\n");
  }

  DDbgPrint("  Delete Symbolic Name: %wZ\n", Dcb->SymbolicLinkName);
  IoDeleteSymbolicLink(Dcb->SymbolicLinkName);

  if (Dcb->MountedDeviceInterfaceName.Buffer != NULL) {
    IoSetDeviceInterfaceState(&Dcb->MountedDeviceInterfaceName, FALSE);

    RtlFreeUnicodeString(&Dcb->MountedDeviceInterfaceName);
    RtlInitUnicodeString(&Dcb->MountedDeviceInterfaceName, NULL);
  }
  if (Dcb->DiskDeviceInterfaceName.Buffer != NULL) {
    IoSetDeviceInterfaceState(&Dcb->DiskDeviceInterfaceName, FALSE);

    RtlFreeUnicodeString(&Dcb->DiskDeviceInterfaceName);
    RtlInitUnicodeString(&Dcb->DiskDeviceInterfaceName, NULL);
  }

  FreeDcbNames(Dcb);

  if (Dcb->DeviceObject->Vpb) {
    Dcb->DeviceObject->Vpb->DeviceObject = NULL;
    Dcb->DeviceObject->Vpb->RealDevice = NULL;
    Dcb->DeviceObject->Vpb->Flags = 0;
  }

  if (vcb != NULL) {
    DDbgPrint("  FCB allocated: %d\n", vcb->FcbAllocated);
    DDbgPrint("  FCB     freed: %d\n", vcb->FcbFreed);
    DDbgPrint("  CCB allocated: %d\n", vcb->CcbAllocated);
    DDbgPrint("  CCB     freed: %d\n", vcb->CcbFreed);

    // delete volDeviceObject
    DDbgPrint("  Delete Volume DeviceObject\n");
    IoDeleteDevice(vcb->DeviceObject);
  }

  // delete diskDeviceObject
  DDbgPrint("  Delete Disk DeviceObject\n");
  IoDeleteDevice(Dcb->DeviceObject);
}
