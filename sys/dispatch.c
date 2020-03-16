/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2017 Google, Inc.
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

#pragma warning(disable : 4214)
struct SYMLINK_ECP_CONTEXT {
  USHORT UnparsedNameLength;
  union {
    USHORT Flags;
    struct {
      USHORT MountPoint : 1;
    } MountPoint;
  } FlagsMountPoint;
  USHORT DeviceNameLength;
  USHORT Zero;
  struct SYMLINK_ECP_CONTEXT *Reparsed;
  UNICODE_STRING Name;
};
#pragma warning(default : 4214)

/*
 * Revert file name when reparse point is used
 * We get real name from the IRP_MJ_CREATE extra information in ECPs
 * This behavior is fixed in >= win10 1803
 */
void RevertFileName(PIRP Irp) {
  RTL_OSVERSIONINFOW VersionInformation = {0};
  VersionInformation.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOW);
  RtlGetVersion(&VersionInformation);
  if (VersionInformation.dwMajorVersion > 10 ||
      (VersionInformation.dwMajorVersion == 10 &&
       VersionInformation.dwMinorVersion > 0) ||
      (VersionInformation.dwMajorVersion == 10 &&
       VersionInformation.dwMinorVersion == 0 &&
       VersionInformation.dwBuildNumber >= 17134))
    return;

  PECP_LIST EcpList;
  struct SYMLINK_ECP_CONTEXT *EcpContext;
  //IopSymlinkECPGuid "73d5118a-88ba-439f-92f4-46d38952d250";
  GUID IopSymlinkECPGuid;
  IopSymlinkECPGuid.Data1 = 0x73d5118a;
  IopSymlinkECPGuid.Data2 = 0x88ba;
  IopSymlinkECPGuid.Data3 = 0x439f;
  IopSymlinkECPGuid.Data4[0] = 0x92;
  IopSymlinkECPGuid.Data4[1] = 0xf4;
  IopSymlinkECPGuid.Data4[2] = 0x46;
  IopSymlinkECPGuid.Data4[3] = 0xd3;
  IopSymlinkECPGuid.Data4[4] = 0x89;
  IopSymlinkECPGuid.Data4[5] = 0x52;
  IopSymlinkECPGuid.Data4[6] = 0xd2;
  IopSymlinkECPGuid.Data4[7] = 0x50;

  if (0 <= FsRtlGetEcpListFromIrp(Irp, &EcpList) && EcpList &&
      0 <= FsRtlFindExtraCreateParameter(EcpList, &IopSymlinkECPGuid,
                                         (void **)&EcpContext, 0) &&
      !FsRtlIsEcpFromUserMode(EcpContext) &&
      EcpContext->FlagsMountPoint.MountPoint.MountPoint) {
    USHORT UnparsedNameLength = EcpContext->UnparsedNameLength;
    if (UnparsedNameLength != 0) {
      PUNICODE_STRING FileName =
          &IoGetCurrentIrpStackLocation(Irp)->FileObject->FileName;
      USHORT FileNameLength = FileName->Length;
      USHORT NameLength = EcpContext->Name.Length;
      if (UnparsedNameLength <= NameLength &&
          UnparsedNameLength <= FileNameLength) {
        UNICODE_STRING us1;
        us1.Length = UnparsedNameLength;
        us1.MaximumLength = UnparsedNameLength;
        us1.Buffer = (PWSTR)RtlOffsetToPointer(
            FileName->Buffer, FileNameLength - UnparsedNameLength);
        UNICODE_STRING us2;
        us2.Length = UnparsedNameLength;
        us2.MaximumLength = UnparsedNameLength;
        us2.Buffer = (PWSTR)RtlOffsetToPointer(EcpContext->Name.Buffer,
                                               NameLength - UnparsedNameLength);
        if (RtlEqualUnicodeString(&us1, &us2, TRUE)) {
          memcpy(us1.Buffer, us2.Buffer, UnparsedNameLength);
        }
      }
    }
  }
}

NTSTATUS
DokanBuildRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  BOOLEAN AtIrqlPassiveLevel = FALSE;
  BOOLEAN IsTopLevelIrp = FALSE;
  NTSTATUS Status = STATUS_UNSUCCESSFUL;

  RevertFileName(Irp);

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

VOID
DokanCancelCreateIrp(__in PDEVICE_OBJECT DeviceObject,
                     __in PIRP_ENTRY IrpEntry,
                     __in NTSTATUS Status) {
  BOOLEAN AtIrqlPassiveLevel = FALSE;
  BOOLEAN IsTopLevelIrp = FALSE;
  PIRP Irp = IrpEntry->Irp;
  PEVENT_INFORMATION eventInfo = NULL;

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

      eventInfo = ExAllocatePool(sizeof(EVENT_INFORMATION));
      RtlZeroMemory(eventInfo, sizeof(EVENT_INFORMATION));
      eventInfo->Status = Status;
      DokanCompleteCreate(IrpEntry, eventInfo);

    } __except (DokanExceptionFilter(Irp, GetExceptionInformation())) {

      DokanExceptionHandler(DeviceObject, Irp, GetExceptionCode());
    }

  } __finally {

    if (eventInfo != NULL) {
      ExFreePool(eventInfo);
    }

    if (IsTopLevelIrp) {
      IoSetTopLevelIrp(NULL);
    }

    if (AtIrqlPassiveLevel) {
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
    // A driver does not receive an IRP_MJ_SHUTDOWN request for a device object
    // unless it registers to do so with either IoRegisterShutdownNotification
    // or IoRegisterLastChanceShutdownNotification.
    // We do not call those functions and therefore should not get the IRP
    return DokanDispatchShutdown(DeviceObject, Irp);

  case IRP_MJ_QUERY_SECURITY:
    return DokanDispatchQuerySecurity(DeviceObject, Irp);

  case IRP_MJ_SET_SECURITY:
    return DokanDispatchSetSecurity(DeviceObject, Irp);

  case IRP_MJ_PNP:
    return DokanDispatchPnp(DeviceObject, Irp);

  default:
    DDbgPrint("DokanDispatchRequest: Unexpected major function: %xh\n",
              irpSp->MajorFunction);

    DokanCompleteIrpRequest(Irp, STATUS_DRIVER_INTERNAL_ERROR, 0);

    return STATUS_DRIVER_INTERNAL_ERROR;
  }
}
