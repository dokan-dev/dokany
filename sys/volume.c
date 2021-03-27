/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2021 Google, Inc.
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

NTSTATUS
DokanDispatchQueryVolumeInformation(__in PDEVICE_OBJECT DeviceObject,
                                    __in PIRP Irp) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PIO_STACK_LOCATION irpSp;
  PVOID buffer;
  PFILE_OBJECT fileObject;
  PDokanVCB vcb;
  PDokanDCB dcb;
  PDokanCCB ccb;
  ULONG info = 0;
  ULONG RequiredLength;

  __try {

    DOKAN_LOG_BEGIN_MJ(Irp);

    vcb = DeviceObject->DeviceExtension;
    if (GetIdentifierType(vcb) != VCB) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    dcb = vcb->Dcb;

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    buffer = Irp->AssociatedIrp.SystemBuffer;

    fileObject = irpSp->FileObject;
    DOKAN_LOG_FINE_IRP(Irp, "FileObject=%p FsInfoClass=%s", fileObject,
                  DokanGetFsInformationClassStr(
                      irpSp->Parameters.QueryVolume.FsInformationClass));

    if (fileObject == NULL) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    ULONGLONG freeBytesAvailable = 512 * 1024 * 1024;
    ULONGLONG totalBytes = 1024 * 1024 * 1024;

    // TODO(drivefs-team): The block below returns hard coded defaults to
    // filter drivers that make requests during mount. We should either
    // eliminate the need for this or pass real defaults in the mount options.
    switch (irpSp->Parameters.QueryVolume.FsInformationClass) {
    case FileFsVolumeInformation:
      if (vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(Irp, "Still no threads for processing available");
      PFILE_FS_VOLUME_INFORMATION FsVolInfo;

      if (irpSp->Parameters.QueryVolume.Length <
          sizeof(FILE_FS_VOLUME_INFORMATION)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      FsVolInfo = (PFILE_FS_VOLUME_INFORMATION)buffer;
      FsVolInfo->VolumeCreationTime.QuadPart = 0;
      FsVolInfo->VolumeSerialNumber = 0x19831116;

      FsVolInfo->VolumeLabelLength =
          (USHORT)wcslen(VOLUME_LABEL) * sizeof(WCHAR);
      /* We don't support ObjectId */
      FsVolInfo->SupportsObjects = FALSE;

      RequiredLength = sizeof(FILE_FS_VOLUME_INFORMATION) +
                       FsVolInfo->VolumeLabelLength - sizeof(WCHAR);

      if (irpSp->Parameters.QueryVolume.Length < RequiredLength) {
        Irp->IoStatus.Information = sizeof(FILE_FS_VOLUME_INFORMATION);
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      RtlCopyMemory(FsVolInfo->VolumeLabel, VOLUME_LABEL,
                    FsVolInfo->VolumeLabelLength);

      Irp->IoStatus.Information = RequiredLength;
      status = STATUS_SUCCESS;
      __leave;
      break;

    case FileFsSizeInformation: {
      if (vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(Irp, "Still no threads for processing available");
      PFILE_FS_SIZE_INFORMATION sizeInfo;

      if (!PREPARE_OUTPUT(Irp, sizeInfo, /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      sizeInfo->TotalAllocationUnits.QuadPart =
          totalBytes / DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
      sizeInfo->AvailableAllocationUnits.QuadPart =
          freeBytesAvailable / DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
      sizeInfo->SectorsPerAllocationUnit =
          DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE / DOKAN_DEFAULT_SECTOR_SIZE;
      sizeInfo->BytesPerSector = DOKAN_DEFAULT_SECTOR_SIZE;

      status = STATUS_SUCCESS;
      __leave;
    }

    case FileFsDeviceInformation: {
      PFILE_FS_DEVICE_INFORMATION device;
      device = (PFILE_FS_DEVICE_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
      if (irpSp->Parameters.QueryVolume.Length <
          sizeof(FILE_FS_DEVICE_INFORMATION)) {
        status = STATUS_BUFFER_TOO_SMALL;
        info = sizeof(FILE_FS_DEVICE_INFORMATION);
        __leave;
      }
      device->DeviceType = dcb->DeviceType;
      device->Characteristics = dcb->DeviceCharacteristics;
      status = STATUS_SUCCESS;
      info = sizeof(FILE_FS_DEVICE_INFORMATION);
      __leave;
    } break;

    case FileFsAttributeInformation:
      if (vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(Irp, "Still no threads for processing available");
      PFILE_FS_ATTRIBUTE_INFORMATION FsAttrInfo;

      if (irpSp->Parameters.QueryVolume.Length <
          sizeof(FILE_FS_ATTRIBUTE_INFORMATION)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      FsAttrInfo =
          (PFILE_FS_ATTRIBUTE_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
      FsAttrInfo->FileSystemAttributes = FILE_SUPPORTS_HARD_LINKS |
                                         FILE_CASE_SENSITIVE_SEARCH |
                                         FILE_CASE_PRESERVED_NAMES;

      FsAttrInfo->MaximumComponentNameLength = 256;
      FsAttrInfo->FileSystemNameLength = 8;

      RequiredLength = sizeof(FILE_FS_ATTRIBUTE_INFORMATION) +
                       FsAttrInfo->FileSystemNameLength - sizeof(WCHAR);

      if (irpSp->Parameters.QueryVolume.Length < RequiredLength) {
        Irp->IoStatus.Information = sizeof(FILE_FS_ATTRIBUTE_INFORMATION);
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      RtlCopyMemory(FsAttrInfo->FileSystemName, L"NTFS",
                    FsAttrInfo->FileSystemNameLength);
      Irp->IoStatus.Information = RequiredLength;
      status = STATUS_SUCCESS;
      __leave;
      break;

    case FileFsFullSizeInformation: {
      if (vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(Irp, "Still no threads for processing available");
      PFILE_FS_FULL_SIZE_INFORMATION sizeInfo;
      if (!PREPARE_OUTPUT(Irp, sizeInfo, /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      sizeInfo->TotalAllocationUnits.QuadPart =
          totalBytes / DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
      sizeInfo->CallerAvailableAllocationUnits.QuadPart =
          freeBytesAvailable / DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
      sizeInfo->ActualAvailableAllocationUnits =
          sizeInfo->CallerAvailableAllocationUnits;
      sizeInfo->SectorsPerAllocationUnit =
          DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE / DOKAN_DEFAULT_SECTOR_SIZE;
      sizeInfo->BytesPerSector = DOKAN_DEFAULT_SECTOR_SIZE;
      status = STATUS_SUCCESS;
      __leave;
    }

    default:
      break;
    }

    if (irpSp->Parameters.QueryVolume.FsInformationClass ==
            FileFsVolumeInformation ||
        irpSp->Parameters.QueryVolume.FsInformationClass ==
            FileFsSizeInformation ||
        irpSp->Parameters.QueryVolume.FsInformationClass ==
            FileFsAttributeInformation ||
        irpSp->Parameters.QueryVolume.FsInformationClass ==
            FileFsFullSizeInformation) {

      ULONG eventLength = sizeof(EVENT_CONTEXT);
      PEVENT_CONTEXT eventContext;

      ccb = fileObject->FsContext2;
      if (ccb && !DokanCheckCCB(Irp, vcb->Dcb, fileObject->FsContext2)) {
        status = STATUS_INVALID_PARAMETER;
        __leave;
      }

      // this memory must be freed in this {}
      eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, NULL);

      if (eventContext == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }

      if (ccb) {
        eventContext->Context = ccb->UserContext;
        eventContext->FileFlags = DokanCCBFlagsGet(ccb);
        // DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);
      }

      eventContext->Operation.Volume.FsInformationClass =
          irpSp->Parameters.QueryVolume.FsInformationClass;

      // the length which can be returned to user-mode
      eventContext->Operation.Volume.BufferLength =
          irpSp->Parameters.QueryVolume.Length;

      status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);
    }

  } __finally {
    DOKAN_LOG_END_MJ(Irp, status, info);
    DokanCompleteIrpRequest(Irp, status, info);
  }

  return status;
}

VOID DokanCompleteQueryVolumeInformation(__in PIRP_ENTRY IrpEntry,
                                         __in PEVENT_INFORMATION EventInfo,
                                         __in PDEVICE_OBJECT DeviceObject) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_SUCCESS;
  ULONG info = 0;
  ULONG bufferLen = 0;
  PVOID buffer = NULL;
  PDokanDCB dcb;
  PDokanVCB vcb;

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;

  DOKAN_LOG_BEGIN_MJ(irp);
  DOKAN_LOG_FINE_IRP(irp, "FileObject=%p", irpSp->FileObject);

  vcb = DeviceObject->DeviceExtension;
  dcb = vcb->Dcb;

  // buffer which is used to copy VolumeInfo
  buffer = irp->AssociatedIrp.SystemBuffer;

  // available buffer size to inform
  bufferLen = irpSp->Parameters.QueryVolume.Length;

  // if buffer is invalid or short of length
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {

    info = 0;
    status = STATUS_INSUFFICIENT_RESOURCES;

  } else {
    // If this is an attribute request and the volume
    // is write protected, ensure read-only flag is present
    if (irpSp->Parameters.QueryVolume.FsInformationClass ==
            FileFsAttributeInformation &&
        IS_DEVICE_READ_ONLY(IrpEntry->IrpSp->DeviceObject)) {

      DOKAN_LOG_FINE_IRP(irp, "Adding FILE_READ_ONLY_VOLUME flag to attributes");
      PFILE_FS_ATTRIBUTE_INFORMATION attrInfo =
          (PFILE_FS_ATTRIBUTE_INFORMATION)EventInfo->Buffer;
      attrInfo->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
    }

    // copy the information from user-mode to specified buffer
    ASSERT(buffer != NULL);

    if (irpSp->Parameters.QueryVolume.FsInformationClass ==
            FileFsVolumeInformation &&
        dcb->VolumeLabel != NULL) {

      PFILE_FS_VOLUME_INFORMATION volumeInfo =
          (PFILE_FS_VOLUME_INFORMATION)EventInfo->Buffer;

      ULONG remainingLength = bufferLen;
      remainingLength -=
          FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel[0]);
      ULONG bytesToCopy = (ULONG)wcslen(dcb->VolumeLabel) * sizeof(WCHAR);
      if (remainingLength < bytesToCopy) {
        bytesToCopy = remainingLength;
      }

      volumeInfo->VolumeLabelLength = bytesToCopy;
      RtlCopyMemory(volumeInfo->VolumeLabel, dcb->VolumeLabel, bytesToCopy);
      remainingLength -= bytesToCopy;

      EventInfo->BufferLength = bufferLen - remainingLength;
    }

    RtlZeroMemory(buffer, bufferLen);
    RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

    // the written length
    info = EventInfo->BufferLength;

    status = EventInfo->Status;
  }

  DOKAN_LOG_END_MJ(irp, status, info);
  DokanCompleteIrpRequest(irp, status, info);
}

NTSTATUS
DokanDispatchSetVolumeInformation(__in PDEVICE_OBJECT DeviceObject,
                                  __in PIRP Irp) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PDokanVCB vcb;
  PDokanDCB dcb;
  PIO_STACK_LOCATION irpSp;
  PVOID buffer;
  FS_INFORMATION_CLASS FsInformationClass;

  __try
  {
    DOKAN_LOG_BEGIN_MJ(Irp);

    vcb = DeviceObject->DeviceExtension;
    if (GetIdentifierType(vcb) != VCB) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    dcb = vcb->Dcb;

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    buffer = Irp->AssociatedIrp.SystemBuffer;
    FsInformationClass = irpSp->Parameters.SetVolume.FsInformationClass;
    DOKAN_LOG_FINE_IRP(Irp, "FileObject=%p FsInfoClass=%s", irpSp->FileObject,
                  DokanGetFsInformationClassStr(FsInformationClass));
    switch (FsInformationClass) {
    case FileFsLabelInformation:
      if (sizeof(FILE_FS_LABEL_INFORMATION) >
          irpSp->Parameters.SetVolume.Length) {
        status = STATUS_INVALID_PARAMETER;
        __leave;
      }

      PFILE_FS_LABEL_INFORMATION Info = (PFILE_FS_LABEL_INFORMATION)buffer;
      ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);
      if (dcb->VolumeLabel != NULL)
        ExFreePool(dcb->VolumeLabel);
      dcb->VolumeLabel =
          DokanAlloc(Info->VolumeLabelLength + sizeof(WCHAR));
      if (dcb->VolumeLabel == NULL) {
        DOKAN_LOG_FINE_IRP(Irp, "Can't allocate VolumeLabel");
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }

      RtlCopyMemory(dcb->VolumeLabel, Info->VolumeLabel,
                    Info->VolumeLabelLength);
      dcb->VolumeLabel[Info->VolumeLabelLength / sizeof(WCHAR)] = '\0';
      ExReleaseResourceLite(&dcb->Resource);
      DOKAN_LOG_FINE_IRP(Irp, "Volume label changed to %ws", dcb->VolumeLabel);

      status = STATUS_SUCCESS;
      break;
    default:
      DOKAN_LOG_FINE_IRP(Irp, "Unsupported FsInfoClass %x", FsInformationClass);
    }

  } __finally {
    DOKAN_LOG_END_MJ(Irp, status, 0);
    DokanCompleteIrpRequest(Irp, status, 0);
  }


  return status;
}
