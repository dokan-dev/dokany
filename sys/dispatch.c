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

NTSTATUS
DokanBuildRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  BOOLEAN atIrqlPassiveLevel = FALSE;
  BOOLEAN isTopLevelIrp = FALSE;
  NTSTATUS status = STATUS_UNSUCCESSFUL;

  __try {

    __try {

      atIrqlPassiveLevel = (KeGetCurrentIrql() == PASSIVE_LEVEL);

      if (atIrqlPassiveLevel) {
        FsRtlEnterFileSystem();
      }

      if (!IoGetTopLevelIrp()) {
        isTopLevelIrp = TRUE;
        IoSetTopLevelIrp(Irp);
      }

      status = DokanDispatchRequest(DeviceObject, Irp);

    } __except (DokanExceptionFilter(Irp, GetExceptionInformation())) {

      status = DokanExceptionHandler(DeviceObject, Irp, GetExceptionCode());
    }

  } __finally {

    if (isTopLevelIrp) {
      IoSetTopLevelIrp(NULL);
    }

    if (atIrqlPassiveLevel) {
      FsRtlExitFileSystem();
    }
  }

  return status;
}

VOID
DokanCancelCreateIrp(__in PDEVICE_OBJECT DeviceObject,
                     __in PIRP_ENTRY IrpEntry,
                     __in NTSTATUS Status) {
  BOOLEAN atIrqlPassiveLevel = FALSE;
  BOOLEAN isTopLevelIrp = FALSE;
  PIRP irp = IrpEntry->Irp;
  PEVENT_INFORMATION eventInfo = NULL;

  __try {

    __try {

      atIrqlPassiveLevel = (KeGetCurrentIrql() == PASSIVE_LEVEL);

      if (atIrqlPassiveLevel) {
        FsRtlEnterFileSystem();
      }

      if (!IoGetTopLevelIrp()) {
        isTopLevelIrp = TRUE;
        IoSetTopLevelIrp(irp);
      }

      eventInfo = DokanAllocZero(sizeof(EVENT_INFORMATION));
      eventInfo->Status = Status;
      DokanCompleteCreate(IrpEntry, eventInfo);

    } __except (DokanExceptionFilter(irp, GetExceptionInformation())) {

      DokanExceptionHandler(DeviceObject, irp, GetExceptionCode());
    }

  } __finally {

    if (eventInfo != NULL) {
      ExFreePool(eventInfo);
    }

    if (isTopLevelIrp) {
      IoSetTopLevelIrp(NULL);
    }

    if (atIrqlPassiveLevel) {
      FsRtlExitFileSystem();
    }
  }
}

NTSTATUS
DokanDispatchRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  if (irpSp->MajorFunction != IRP_MJ_FILE_SYSTEM_CONTROL &&
      irpSp->MajorFunction != IRP_MJ_SHUTDOWN &&
      irpSp->MajorFunction != IRP_MJ_CLEANUP &&
      irpSp->MajorFunction != IRP_MJ_CLOSE) {
    if (IsUnmountPending(DeviceObject)) {
      DOKAN_LOG_BEGIN_MJ(Irp);
      DOKAN_LOG_FINE_IRP(Irp, "Volume is not mounted so return STATUS_NO_SUCH_DEVICE");
      NTSTATUS status = STATUS_NO_SUCH_DEVICE;
      DOKAN_LOG_END_MJ(Irp, status, 0);
      DokanCompleteIrpRequest(Irp, status, 0);
      return status;
    }
  }

  // If volume is write protected and this request
  // would modify it then return write protected status.
  if (IS_DEVICE_READ_ONLY(DeviceObject)) {
    if ((irpSp->MajorFunction == IRP_MJ_WRITE) ||
        (irpSp->MajorFunction == IRP_MJ_SET_INFORMATION) ||
        (irpSp->MajorFunction == IRP_MJ_SET_EA) ||
        (irpSp->MajorFunction == IRP_MJ_FLUSH_BUFFERS) ||
        (irpSp->MajorFunction == IRP_MJ_SET_SECURITY) ||
        (irpSp->MajorFunction == IRP_MJ_SET_VOLUME_INFORMATION) ||
        (irpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
         irpSp->MinorFunction == IRP_MN_USER_FS_REQUEST &&
         irpSp->Parameters.FileSystemControl.FsControlCode ==
             FSCTL_MARK_VOLUME_DIRTY)) {

      DOKAN_LOG_BEGIN_MJ(Irp);
      DOKAN_LOG_FINE_IRP(Irp, "Media is write protected");
      DOKAN_LOG_END_MJ(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
      DokanCompleteIrpRequest(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
      return STATUS_MEDIA_WRITE_PROTECTED;
    }
  }

  switch (irpSp->MajorFunction) {

  case IRP_MJ_CREATE:
    return DokanDispatchCreate(DeviceObject, Irp);

  case IRP_MJ_CLOSE:
    return DokanDispatchClose(DeviceObject, Irp);

  case IRP_MJ_READ:
    return DokanDispatchRead(DeviceObject, Irp);

  case IRP_MJ_WRITE:
    return DokanDispatchWrite(DeviceObject, Irp);

  case IRP_MJ_FLUSH_BUFFERS:
    return DokanDispatchFlush(DeviceObject, Irp);

  case IRP_MJ_QUERY_INFORMATION:
    return DokanDispatchQueryInformation(DeviceObject, Irp);

  case IRP_MJ_SET_INFORMATION:
    return DokanDispatchSetInformation(DeviceObject, Irp);

  case IRP_MJ_QUERY_VOLUME_INFORMATION:
    return DokanDispatchQueryVolumeInformation(DeviceObject, Irp);

  case IRP_MJ_SET_VOLUME_INFORMATION:
    return DokanDispatchSetVolumeInformation(DeviceObject, Irp);

  case IRP_MJ_DIRECTORY_CONTROL:
    return DokanDispatchDirectoryControl(DeviceObject, Irp);

  case IRP_MJ_FILE_SYSTEM_CONTROL:
    return DokanDispatchFileSystemControl(DeviceObject, Irp);

  case IRP_MJ_DEVICE_CONTROL:
    return DokanDispatchDeviceControl(DeviceObject, Irp);

  case IRP_MJ_LOCK_CONTROL:
    return DokanDispatchLock(DeviceObject, Irp);

  case IRP_MJ_CLEANUP:
    return DokanDispatchCleanup(DeviceObject, Irp);

  case IRP_MJ_SHUTDOWN:
    // A driver does not receive an IRP_MJ_SHUTDOWN request for a device object
    // unless it registers to do so with either IoRegisterShutdownNotification
    // or IoRegisterLastChanceShutdownNotification.
    // We do not call those functions and therefore should not get the IRP
    return DokanDispatchShutdown(DeviceObject, Irp);

  case IRP_MJ_QUERY_SECURITY:
    return DokanDispatchQuerySecurity(DeviceObject, Irp);

  case IRP_MJ_SET_SECURITY:
    return DokanDispatchSetSecurity(DeviceObject, Irp);

  default:
    DOKAN_LOG_BEGIN_MJ(Irp);
    DOKAN_LOG_END_MJ(Irp, STATUS_DRIVER_INTERNAL_ERROR, 0);
    DokanCompleteIrpRequest(Irp, STATUS_DRIVER_INTERNAL_ERROR, 0);

    return STATUS_DRIVER_INTERNAL_ERROR;
  }
}
