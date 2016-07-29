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

NTSTATUS
DokanBuildRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  BOOLEAN AtIrqlPassiveLevel = FALSE;
  BOOLEAN IsTopLevelIrp = FALSE;
  NTSTATUS Status = STATUS_UNSUCCESSFUL;

  __try {

    __try {

      AtIrqlPassiveLevel = (KeGetCurrentIrql() == PASSIVE_LEVEL);

      if (AtIrqlPassiveLevel) {
        FsRtlEnterFileSystem();
      }

      if (!IoGetTopLevelIrp()) {
        IsTopLevelIrp = TRUE;
        IoSetTopLevelIrp(Irp);
      }

      Status = DokanDispatchRequest(DeviceObject, Irp);

    } __except (DokanExceptionFilter(Irp, GetExceptionInformation())) {

      Status = DokanExceptionHandler(DeviceObject, Irp, GetExceptionCode());
    }

  } __finally {

    if (IsTopLevelIrp) {
      IoSetTopLevelIrp(NULL);
    }

    if (AtIrqlPassiveLevel) {
      FsRtlExitFileSystem();
    }
  }

  return Status;
}

NTSTATUS
DokanDispatchRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  if (irpSp->MajorFunction != IRP_MJ_FILE_SYSTEM_CONTROL &&
      irpSp->MajorFunction != IRP_MJ_SHUTDOWN &&
      irpSp->MajorFunction != IRP_MJ_CLEANUP &&
      irpSp->MajorFunction != IRP_MJ_CLOSE &&
      irpSp->MajorFunction != IRP_MJ_PNP) {
    if (IsUnmountPending(DeviceObject)) {
      DDbgPrint("  Volume is not mounted so return STATUS_NO_SUCH_DEVICE\n");
      NTSTATUS status = STATUS_NO_SUCH_DEVICE;
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

      DDbgPrint("    Media is write protected\n");
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
    return DokanDispatchShutdown(DeviceObject, Irp);

  case IRP_MJ_QUERY_SECURITY:
    return DokanDispatchQuerySecurity(DeviceObject, Irp);

  case IRP_MJ_SET_SECURITY:
    return DokanDispatchSetSecurity(DeviceObject, Irp);

#if (_WIN32_WINNT >= 0x0500)
  case IRP_MJ_PNP:
    return DokanDispatchPnp(DeviceObject, Irp);
#endif //(_WIN32_WINNT >= 0x0500)
  default:
    DDbgPrint("DokanDispatchRequest: Unexpected major function: %xh\n",
              irpSp->MajorFunction);

    DokanCompleteIrpRequest(Irp, STATUS_DRIVER_INTERNAL_ERROR, 0);

    return STATUS_DRIVER_INTERNAL_ERROR;
  }
}