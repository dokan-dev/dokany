/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2017 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

  //
  //  Initialize the Io status field in the Irp.
  //

  Irp->IoStatus.Status = STATUS_PENDING;
  Irp->IoStatus.Information = 0;

  if (irpSp->MajorFunction != IRP_MJ_FILE_SYSTEM_CONTROL &&
      irpSp->MajorFunction != IRP_MJ_SHUTDOWN &&
      irpSp->MajorFunction != IRP_MJ_CLEANUP &&
      irpSp->MajorFunction != IRP_MJ_CLOSE &&
      irpSp->MajorFunction != IRP_MJ_PNP) {
    if (IsUnmountPending(DeviceObject)) {
      DDbgPrint("  Volume is not mounted so return STATUS_NO_SUCH_DEVICE\n");
      DokanCompleteIrpRequest(Irp, STATUS_NO_SUCH_DEVICE, 0);
      return Irp->IoStatus.Status;
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
      return Irp->IoStatus.Status;
    }
  }

  switch (irpSp->MajorFunction) {

  case IRP_MJ_CREATE:
    DokanDispatchCreate(DeviceObject, Irp);
    break;

  case IRP_MJ_CLOSE:
    DokanDispatchClose(DeviceObject, Irp);
    break;

  case IRP_MJ_READ:
    DokanDispatchRead(DeviceObject, Irp);
    break;

  case IRP_MJ_WRITE:
    DokanDispatchWrite(DeviceObject, Irp);
    break;

  case IRP_MJ_FLUSH_BUFFERS:
    DokanDispatchFlush(DeviceObject, Irp);
    break;

  case IRP_MJ_QUERY_INFORMATION:
    DokanDispatchQueryInformation(DeviceObject, Irp);
    break;

  case IRP_MJ_SET_INFORMATION:
    DokanDispatchSetInformation(DeviceObject, Irp);
    break;

  case IRP_MJ_QUERY_VOLUME_INFORMATION:
    DokanDispatchQueryVolumeInformation(DeviceObject, Irp);
    break;

  case IRP_MJ_SET_VOLUME_INFORMATION:
    DokanDispatchSetVolumeInformation(DeviceObject, Irp);
    break;

  case IRP_MJ_DIRECTORY_CONTROL:
    DokanDispatchDirectoryControl(DeviceObject, Irp);
    break;
  case IRP_MJ_FILE_SYSTEM_CONTROL:
    DokanDispatchFileSystemControl(DeviceObject, Irp);
    break;

  case IRP_MJ_DEVICE_CONTROL:
    DokanDispatchDeviceControl(DeviceObject, Irp);
    break;

  case IRP_MJ_LOCK_CONTROL:
    DokanDispatchLock(DeviceObject, Irp);
    break;

  case IRP_MJ_CLEANUP:
    DokanDispatchCleanup(DeviceObject, Irp);
    break;

  case IRP_MJ_SHUTDOWN:
    DokanDispatchShutdown(DeviceObject, Irp);
    break;

  case IRP_MJ_QUERY_SECURITY:
    DokanDispatchQuerySecurity(DeviceObject, Irp);
    break;

  case IRP_MJ_SET_SECURITY:
    DokanDispatchSetSecurity(DeviceObject, Irp);
    break;

#if (_WIN32_WINNT >= 0x0500)
  case IRP_MJ_PNP:
    DokanDispatchPnp(DeviceObject, Irp);
    break;

#endif //(_WIN32_WINNT >= 0x0500)
  default:
    DDbgPrint("DokanDispatchRequest: Unexpected major function: %xh\n",
              irpSp->MajorFunction);

    DokanCompleteIrpRequest(Irp, STATUS_DRIVER_INTERNAL_ERROR, 0);
  }

  return Irp->IoStatus.Status;
}