/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.

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

#include "mountmgr.h"

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
    DDbgPrint("  IoGetDeviceObjectPointer failed: 0x%x %ls\n", status,
              DokanGetNTSTATUSStr(status));
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
    DDbgPrint("  IoCallDriver pending\n");
    KeWaitForSingleObject(&driverEvent, Executive, KernelMode, FALSE, NULL);
  }
  status = iosb.Status;

  ObDereferenceObject(mountFileObject);
  // Don't dereference mountDeviceObject, mountFileObject is enough

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoCallDriver success\n");
  } else {
    DDbgPrint("  IoCallDriver failed: 0x%x %ls\n", status,
              DokanGetNTSTATUSStr(status));
  }

  DDbgPrint("<= DokanSendIoContlToMountManager\n");

  return status;
}

NTSTATUS DokanSendVolumeMountPoint(__in PDokanDCB Dcb, BOOLEAN Create) {
  NTSTATUS status;
  PMOUNTMGR_VOLUME_MOUNT_POINT volumMountPoint;
  ULONG length;

  DDbgPrint("=> DokanSendVolumeMountPoint\n");

  length = sizeof(MOUNTMGR_VOLUME_MOUNT_POINT) + Dcb->MountPoint->Length +
           Dcb->PersistentSymbolicLinkName->Length;
  volumMountPoint = DokanAllocZero(length);

  if (volumMountPoint == NULL) {
    DDbgPrint("  can't allocate MOUNTMGR_VOLUME_MOUNT_POINT\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  // IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_*
  // SourceVolumeName: \DosDevices\C:\foo
  // TargetVolumeName (PersistanteSymbolicLinkName):
  // \??\Volume{71c74232-886a-11ea-870b-5cf37092b67e}
  volumMountPoint->SourceVolumeNameOffset = sizeof(MOUNTMGR_VOLUME_MOUNT_POINT);
  volumMountPoint->SourceVolumeNameLength = Dcb->MountPoint->Length;
  RtlCopyMemory(
      (PCHAR)volumMountPoint + volumMountPoint->SourceVolumeNameOffset,
      Dcb->MountPoint->Buffer, volumMountPoint->SourceVolumeNameLength);
  volumMountPoint->TargetVolumeNameOffset =
      volumMountPoint->SourceVolumeNameOffset +
      volumMountPoint->SourceVolumeNameLength;
  volumMountPoint->TargetVolumeNameLength =
      Dcb->PersistentSymbolicLinkName->Length;
  RtlCopyMemory(
      (PCHAR)volumMountPoint + volumMountPoint->TargetVolumeNameOffset,
      Dcb->PersistentSymbolicLinkName->Buffer,
      volumMountPoint->TargetVolumeNameLength);

  status = DokanSendIoContlToMountManager(
      Create ? IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED
             : IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_DELETED,
      volumMountPoint, length, NULL, 0);

  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoCallDriver IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT success\n");
  } else {
    DDbgPrint(
        "  IoCallDriver IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT "
        "failed: 0x%x %ls\n",
        status, DokanGetNTSTATUSStr(status));
  }

  ExFreePool(volumMountPoint);

  DDbgPrint("<= DokanSendVolumeMountPoint\n");

  return status;
}

VOID NotifyDirectoryMountPointCreated(__in PDokanDCB Dcb) {
  DDbgPrint("==> NotifyDirectoryMountPointCreated\n");
  DokanSendVolumeMountPoint(Dcb, /*Create*/ TRUE);
  DDbgPrint("<== NotifyDirectoryMountPointCreated\n");
}

VOID NotifyDirectoryMountPointDeleted(__in PDokanDCB Dcb) {
  DDbgPrint("==> NotifyDirectoryMountPointDeleted\n");
  DokanSendVolumeMountPoint(Dcb, /*Create*/ FALSE);
  DDbgPrint("<== NotifyDirectoryMountPointDeleted\n");
}

NTSTATUS
DokanSendVolumeArrivalNotification(PUNICODE_STRING DeviceName) {
  NTSTATUS status;
  PMOUNTMGR_TARGET_NAME targetName;
  ULONG length;

  DDbgPrint("=> DokanSendVolumeArrivalNotification\n");

  length = sizeof(MOUNTMGR_TARGET_NAME) + DeviceName->Length - 1;
  targetName = DokanAllocZero(length);

  if (targetName == NULL) {
    DDbgPrint("  can't allocate MOUNTMGR_TARGET_NAME\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  targetName->DeviceNameLength = DeviceName->Length;
  RtlCopyMemory(targetName->DeviceName, DeviceName->Buffer, DeviceName->Length);

  status = DokanSendIoContlToMountManager(
      IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION, targetName, length, NULL, 0);

  if (NT_SUCCESS(status)) {
    DDbgPrint(
        "  IoCallDriver IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION success\n");
  } else {
    DDbgPrint(
        "  IoCallDriver IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION "
        "failed: 0x%x %ls\n",
        status, DokanGetNTSTATUSStr(status));
  }

  ExFreePool(targetName);

  DDbgPrint("<= DokanSendVolumeArrivalNotification\n");

  return status;
}

// Note that invoking this with a drive letter MountPoint and no DeviceName has
// the weird side-effect of inserting a mount manager database record telling it
// to never in the future auto-assign a drive letter to the thing currently
// using that drive letter.
NTSTATUS
DokanSendVolumeDeletePoints(__in PUNICODE_STRING MountPoint,
                            __in PUNICODE_STRING DeviceName) {
  NTSTATUS status;
  PMOUNTMGR_MOUNT_POINT point;
  PMOUNTMGR_MOUNT_POINTS deletedPoints;
  ULONG length;
  ULONG olength;

  DDbgPrint("=> DokanSendVolumeDeletePoints\n");
  length = sizeof(MOUNTMGR_MOUNT_POINT);
  if (MountPoint != NULL) {
    length += MountPoint->Length;
  }
  if (DeviceName != NULL) {
    length += DeviceName->Length;
  }
  point = DokanAllocZero(length);
  if (point == NULL) {
    DDbgPrint("  can't allocate MOUNTMGR_CREATE_POINT_INPUT\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  olength = sizeof(MOUNTMGR_MOUNT_POINTS) + 1024;
  deletedPoints = DokanAllocZero(olength);
  if (deletedPoints == NULL) {
    DDbgPrint("  can't allocate PMOUNTMGR_MOUNT_POINTS\n");
    ExFreePool(point);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  ULONG nextStringOffset = sizeof(MOUNTMGR_MOUNT_POINT);
  if (MountPoint != NULL) {
    DDbgPrint("  MountPoint: %wZ\n", MountPoint);
    point->SymbolicLinkNameOffset = nextStringOffset;
    point->SymbolicLinkNameLength = MountPoint->Length;
    nextStringOffset =
        point->SymbolicLinkNameOffset + point->SymbolicLinkNameLength;
    RtlCopyMemory((PCHAR)point + point->SymbolicLinkNameOffset,
                  MountPoint->Buffer, MountPoint->Length);
  }
  if (DeviceName != NULL) {
    DDbgPrint("  DeviceName: %wZ\n", DeviceName);
    point->DeviceNameOffset = nextStringOffset;
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
    DDbgPrint("  IoCallDriver failed: 0x%x %ls\n", status,
              DokanGetNTSTATUSStr(status));
  }

  ExFreePool(point);
  ExFreePool(deletedPoints);

  DDbgPrint("<= DokanSendVolumeDeletePoints\n");

  return status;
}

NTSTATUS
DokanSendVolumeCreatePoint(__in PDRIVER_OBJECT DriverObject,
                           __in PUNICODE_STRING DeviceName,
                           __in PUNICODE_STRING MountPoint) {
  NTSTATUS status;
  PMOUNTMGR_CREATE_POINT_INPUT point;
  ULONG length;
  DOKAN_INIT_LOGGER(logger, DriverObject, IRP_MJ_FILE_SYSTEM_CONTROL);
  DokanLogInfo(&logger, L"Creating mount point: %wZ for device name: %wZ.",
               MountPoint, DeviceName);

  length = sizeof(MOUNTMGR_CREATE_POINT_INPUT) + MountPoint->Length +
           DeviceName->Length;
  point = DokanAllocZero(length);
  if (point == NULL) {
    return DokanLogError(&logger, STATUS_INSUFFICIENT_RESOURCES,
                         L"Failed to allocate MOUNTMGR_CREATE_POINT_INPUT.");
  }

  point->DeviceNameOffset = sizeof(MOUNTMGR_CREATE_POINT_INPUT);
  point->DeviceNameLength = DeviceName->Length;
  RtlCopyMemory((PCHAR)point + point->DeviceNameOffset, DeviceName->Buffer,
                DeviceName->Length);

  point->SymbolicLinkNameOffset =
      point->DeviceNameOffset + point->DeviceNameLength;
  point->SymbolicLinkNameLength = MountPoint->Length;
  RtlCopyMemory((PCHAR)point + point->SymbolicLinkNameOffset,
                MountPoint->Buffer, MountPoint->Length);

  status = DokanSendIoContlToMountManager(IOCTL_MOUNTMGR_CREATE_POINT, point,
                                          length, NULL, 0);

  if (NT_SUCCESS(status)) {
    DokanLogInfo(&logger, L"IOCTL_MOUNTMGR_CREATE_POINT succeeded.");
  } else {
    DokanLogError(&logger, status, L"IOCTL_MOUNTMGR_CREATE_POINT failed.");
  }

  ExFreePool(point);

  return status;
}

NTSTATUS DokanQueryAutoMount(PBOOLEAN State) {
  NTSTATUS status;
  MOUNTMGR_QUERY_AUTO_MOUNT queryAutoMount;

  DDbgPrint("=> DokanQueryAutoMount\n");

  status =
      DokanSendIoContlToMountManager(IOCTL_MOUNTMGR_QUERY_AUTO_MOUNT, NULL, 0,
                                     &queryAutoMount, sizeof(queryAutoMount));
  if (NT_SUCCESS(status)) {
    DDbgPrint(
        "  IoCallDriver IOCTL_MOUNTMGR_QUERY_AUTO_MOUNT success. CurrentState: "
        "%d\n",
        queryAutoMount.CurrentState);
    *State = queryAutoMount.CurrentState;
  } else {
    DDbgPrint(
        "  IoCallDriver IOCTL_MOUNTMGR_QUERY_AUTO_MOUNT "
        "failed: 0x%x %ls\n",
        status, DokanGetNTSTATUSStr(status));
  }

  DDbgPrint("<= DokanQueryAutoMount\n");
  return status;
}

NTSTATUS DokanSendAutoMount(BOOLEAN State) {
  NTSTATUS status;
  MOUNTMGR_SET_AUTO_MOUNT setAutoMount;

  DDbgPrint("=> DokanSendAutoMount\n");

  setAutoMount.NewState = State;
  DDbgPrint("DokanSendAutoMount State %d\n", setAutoMount.NewState);
  status = DokanSendIoContlToMountManager(IOCTL_MOUNTMGR_SET_AUTO_MOUNT,
                                          &setAutoMount, sizeof(setAutoMount),
                                          NULL, 0);
  if (NT_SUCCESS(status)) {
    DDbgPrint("  IoCallDriver IOCTL_MOUNTMGR_SET_AUTO_MOUNT success\n");
  } else {
    DDbgPrint(
        "  IoCallDriver IOCTL_MOUNTMGR_SET_AUTO_MOUNT "
        "failed: 0x%x %ls\n",
        status, DokanGetNTSTATUSStr(status));
  }

  DDbgPrint("<= DokanSendAutoMount\n");
  return status;
}