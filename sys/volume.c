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
DokanDispatchQueryVolumeInformation(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PVOID buffer;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  ULONG requiredLength;

  if (!RequestContext->Vcb) {
    return STATUS_INVALID_PARAMETER;
  }

  buffer = RequestContext->Irp->AssociatedIrp.SystemBuffer;
  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(
      RequestContext, "FileObject=%p FsInfoClass=%s", fileObject,
      DokanGetFsInformationClassStr(
          RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass));

  if (fileObject == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  ULONGLONG freeBytesAvailable = 512 * 1024 * 1024;
  ULONGLONG totalBytes = 1024 * 1024 * 1024;

  // TODO(drivefs-team): The block below returns hard coded defaults to
  // filter drivers that make requests during mount. We should either
  // eliminate the need for this or pass real defaults in the mount options.
  switch (RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass) {
    case FileFsVolumeInformation: {
      if (RequestContext->Vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(RequestContext, "Still no threads for processing available");
      PFILE_FS_VOLUME_INFORMATION FsVolInfo;

      if (RequestContext->IrpSp->Parameters.QueryVolume.Length <
          sizeof(FILE_FS_VOLUME_INFORMATION)) {
        return STATUS_BUFFER_OVERFLOW;
      }

      FsVolInfo = (PFILE_FS_VOLUME_INFORMATION)buffer;
      FsVolInfo->VolumeCreationTime.QuadPart = 0;
      FsVolInfo->VolumeSerialNumber = 0x19831116;

      FsVolInfo->VolumeLabelLength =
          (USHORT)wcslen(VOLUME_LABEL) * sizeof(WCHAR);
      /* We don't support ObjectId */
      FsVolInfo->SupportsObjects = FALSE;

      requiredLength = sizeof(FILE_FS_VOLUME_INFORMATION) +
                       FsVolInfo->VolumeLabelLength - sizeof(WCHAR);

      if (RequestContext->IrpSp->Parameters.QueryVolume.Length < requiredLength) {
        RequestContext->Irp->IoStatus.Information =
            sizeof(FILE_FS_VOLUME_INFORMATION);
        return STATUS_BUFFER_OVERFLOW;
      }

      RtlCopyMemory(FsVolInfo->VolumeLabel, VOLUME_LABEL,
                    FsVolInfo->VolumeLabelLength);

      RequestContext->Irp->IoStatus.Information = requiredLength;
      return STATUS_SUCCESS;
    }

    case FileFsSizeInformation: {
      if (RequestContext->Vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(RequestContext, "Still no threads for processing available");
      PFILE_FS_SIZE_INFORMATION sizeInfo;

      if (!PREPARE_OUTPUT(RequestContext->Irp, sizeInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        return STATUS_BUFFER_OVERFLOW;
      }

      sizeInfo->TotalAllocationUnits.QuadPart =
          totalBytes / DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
      sizeInfo->AvailableAllocationUnits.QuadPart =
          freeBytesAvailable / DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
      sizeInfo->SectorsPerAllocationUnit =
          DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE / DOKAN_DEFAULT_SECTOR_SIZE;
      sizeInfo->BytesPerSector = DOKAN_DEFAULT_SECTOR_SIZE;
      return STATUS_SUCCESS;
    }

    case FileFsDeviceInformation: {
      PFILE_FS_DEVICE_INFORMATION device;
      device = (PFILE_FS_DEVICE_INFORMATION)
                   RequestContext->Irp->AssociatedIrp.SystemBuffer;
      if (RequestContext->IrpSp->Parameters.QueryVolume.Length <
          sizeof(FILE_FS_DEVICE_INFORMATION)) {
        RequestContext->Irp->IoStatus.Information =
            sizeof(FILE_FS_DEVICE_INFORMATION);
        return STATUS_BUFFER_TOO_SMALL;
      }
      device->DeviceType = RequestContext->Dcb->DeviceType;
      device->Characteristics = RequestContext->Dcb->DeviceCharacteristics;
      RequestContext->Irp->IoStatus.Information =
          sizeof(FILE_FS_DEVICE_INFORMATION);
      return STATUS_SUCCESS;
    }

    case FileFsAttributeInformation: {
      if (RequestContext->Vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(RequestContext, "Still no threads for processing available");
      PFILE_FS_ATTRIBUTE_INFORMATION FsAttrInfo;

      if (RequestContext->IrpSp->Parameters.QueryVolume.Length <
          sizeof(FILE_FS_ATTRIBUTE_INFORMATION)) {
        return STATUS_BUFFER_OVERFLOW;
      }

      FsAttrInfo = (PFILE_FS_ATTRIBUTE_INFORMATION)
                       RequestContext->Irp->AssociatedIrp.SystemBuffer;
      FsAttrInfo->FileSystemAttributes = FILE_SUPPORTS_HARD_LINKS |
                                         FILE_CASE_SENSITIVE_SEARCH |
                                         FILE_CASE_PRESERVED_NAMES;

      FsAttrInfo->MaximumComponentNameLength = 256;
      FsAttrInfo->FileSystemNameLength = 8;

      requiredLength = sizeof(FILE_FS_ATTRIBUTE_INFORMATION) +
                       FsAttrInfo->FileSystemNameLength - sizeof(WCHAR);

      if (RequestContext->IrpSp->Parameters.QueryVolume.Length < requiredLength) {
        RequestContext->Irp->IoStatus.Information =
            sizeof(FILE_FS_ATTRIBUTE_INFORMATION);
        return STATUS_BUFFER_OVERFLOW;
      }

      RtlCopyMemory(FsAttrInfo->FileSystemName, L"NTFS",
                    FsAttrInfo->FileSystemNameLength);
      RequestContext->Irp->IoStatus.Information = requiredLength;
      return STATUS_SUCCESS;
    }

    case FileFsFullSizeInformation: {
      if (RequestContext->Vcb->HasEventWait) {
        break;
      }

      DOKAN_LOG_FINE_IRP(RequestContext, "Still no threads for processing available");
      PFILE_FS_FULL_SIZE_INFORMATION sizeInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, sizeInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        return STATUS_BUFFER_OVERFLOW;
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
      return STATUS_SUCCESS;
    }
  }

  if (RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass ==
          FileFsVolumeInformation ||
      RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass ==
          FileFsSizeInformation ||
      RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass ==
          FileFsAttributeInformation ||
      RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass ==
          FileFsFullSizeInformation) {
    ULONG eventLength = sizeof(EVENT_CONTEXT);
    PEVENT_CONTEXT eventContext;

    ccb = fileObject->FsContext2;
    if (ccb && !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
      return STATUS_INVALID_PARAMETER;
    }

    // this memory must be freed in this {}
    eventContext = AllocateEventContext(RequestContext, eventLength, NULL);

    if (eventContext == NULL) {
      return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ccb) {
      eventContext->Context = ccb->UserContext;
      eventContext->FileFlags = DokanCCBFlagsGet(ccb);
    }

    eventContext->Operation.Volume.FsInformationClass =
        RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass;

    // the length which can be returned to user-mode
    eventContext->Operation.Volume.BufferLength =
        RequestContext->IrpSp->Parameters.QueryVolume.Length;

    return DokanRegisterPendingIrp(RequestContext, eventContext);
  }

  return status;
}

VOID DokanCompleteQueryVolumeInformation(__in PREQUEST_CONTEXT RequestContext,
                                         __in PEVENT_INFORMATION EventInfo) {
  ULONG bufferLen = 0;
  PVOID buffer = NULL;

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p",
                     RequestContext->IrpSp->FileObject);

  // buffer which is used to copy VolumeInfo
  buffer = RequestContext->Irp->AssociatedIrp.SystemBuffer;

  // available buffer size to inform
  bufferLen = RequestContext->IrpSp->Parameters.QueryVolume.Length;

  // if buffer is invalid or short of length
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {
    RequestContext->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    return;
  }

  // If this is an attribute request and the volume
  // is write protected, ensure read-only flag is present
  if (RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass ==
          FileFsAttributeInformation &&
      IS_DEVICE_READ_ONLY(RequestContext->IrpSp->DeviceObject)) {
    DOKAN_LOG_FINE_IRP(RequestContext,
                       "Adding FILE_READ_ONLY_VOLUME flag to attributes");
    PFILE_FS_ATTRIBUTE_INFORMATION attrInfo =
        (PFILE_FS_ATTRIBUTE_INFORMATION)EventInfo->Buffer;
    attrInfo->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
  }

  // copy the information from user-mode to specified buffer
  ASSERT(buffer != NULL);

  if (RequestContext->IrpSp->Parameters.QueryVolume.FsInformationClass ==
          FileFsVolumeInformation &&
      RequestContext->Dcb->VolumeLabel != NULL) {
    PFILE_FS_VOLUME_INFORMATION volumeInfo =
        (PFILE_FS_VOLUME_INFORMATION)EventInfo->Buffer;

    ULONG remainingLength = bufferLen;
    remainingLength -= FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel[0]);
    ULONG bytesToCopy =
        (ULONG)wcslen(RequestContext->Dcb->VolumeLabel) * sizeof(WCHAR);
    if (remainingLength < bytesToCopy) {
      bytesToCopy = remainingLength;
    }

    volumeInfo->VolumeLabelLength = bytesToCopy;
    RtlCopyMemory(volumeInfo->VolumeLabel, RequestContext->Dcb->VolumeLabel,
                  bytesToCopy);
    remainingLength -= bytesToCopy;

    EventInfo->BufferLength = bufferLen - remainingLength;
  }

  RtlZeroMemory(buffer, bufferLen);
  RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

  // the written length
  RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;
  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
}

NTSTATUS
DokanDispatchSetVolumeInformation(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PVOID buffer;
  FS_INFORMATION_CLASS fsInformationClass;

  if (!RequestContext->Vcb) {
    return STATUS_INVALID_PARAMETER;
  }

  buffer = RequestContext->Irp->AssociatedIrp.SystemBuffer;
  fsInformationClass =
      RequestContext->IrpSp->Parameters.SetVolume.FsInformationClass;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p FsInfoClass=%s",
                     RequestContext->IrpSp->FileObject,
                     DokanGetFsInformationClassStr(fsInformationClass));
  switch (fsInformationClass) {
    case FileFsLabelInformation: {
      if (sizeof(FILE_FS_LABEL_INFORMATION) >
          RequestContext->IrpSp->Parameters.SetVolume.Length) {
        return STATUS_INVALID_PARAMETER;
      }

      PFILE_FS_LABEL_INFORMATION Info = (PFILE_FS_LABEL_INFORMATION)buffer;
      ExAcquireResourceExclusiveLite(&RequestContext->Dcb->Resource, TRUE);
      if (RequestContext->Dcb->VolumeLabel != NULL)
        ExFreePool(RequestContext->Dcb->VolumeLabel);
      RequestContext->Dcb->VolumeLabel =
          DokanAlloc(Info->VolumeLabelLength + sizeof(WCHAR));
      if (RequestContext->Dcb->VolumeLabel == NULL) {
        DOKAN_LOG_FINE_IRP(RequestContext, "Can't allocate VolumeLabel");
        return STATUS_INSUFFICIENT_RESOURCES;
      }

      RtlCopyMemory(RequestContext->Dcb->VolumeLabel, Info->VolumeLabel,
                    Info->VolumeLabelLength);
      RequestContext->Dcb
          ->VolumeLabel[Info->VolumeLabelLength / sizeof(WCHAR)] = '\0';
      ExReleaseResourceLite(&RequestContext->Dcb->Resource);
      DOKAN_LOG_FINE_IRP(RequestContext, "Volume label changed to %ws",
                         RequestContext->Dcb->VolumeLabel);

      return STATUS_SUCCESS;
    }
    default:
      DOKAN_LOG_FINE_IRP(RequestContext, "Unsupported FsInfoClass %x",
                         fsInformationClass);
  }

  return status;
}
