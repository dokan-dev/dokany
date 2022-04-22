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
#include "util/str.h"

#include <mountdev.h>
#include <mountmgr.h>
#include <ntddvol.h>
#include <storduid.h>

NTSTATUS
GlobalDeviceControl(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    DOKAN_LOG_FINE_IRP(
        RequestContext, "Unsupported IoControlCode %x",
        RequestContext->IrpSp->Parameters.DeviceIoControl.IoControlCode);
  return status;
}

VOID DokanPopulateDiskGeometry(__out PDISK_GEOMETRY DiskGeometry) {
  DiskGeometry->Cylinders.QuadPart =
      DOKAN_DEFAULT_DISK_SIZE / DOKAN_DEFAULT_SECTOR_SIZE / 32 / 2;
  DiskGeometry->MediaType = FixedMedia;
  DiskGeometry->TracksPerCylinder = 2;
  DiskGeometry->SectorsPerTrack = 32;
  DiskGeometry->BytesPerSector = DOKAN_DEFAULT_SECTOR_SIZE;
}

VOID DokanPopulatePartitionInfo(__out PPARTITION_INFORMATION Info) {
  Info->RewritePartition = FALSE;
  Info->RecognizedPartition = FALSE;
  Info->PartitionType = PARTITION_ENTRY_UNUSED;
  Info->BootIndicator = FALSE;
  Info->HiddenSectors = 0;
  Info->StartingOffset.QuadPart = 0;
  // Partition size is disk size here.
  Info->PartitionLength.QuadPart = DOKAN_DEFAULT_DISK_SIZE;
  Info->PartitionNumber = 0;
}

VOID DokanPopulatePartitionInfoEx(__out PPARTITION_INFORMATION_EX Info) {
  Info->PartitionStyle = PARTITION_STYLE_MBR;
  Info->RewritePartition = FALSE;
  Info->Mbr.RecognizedPartition = FALSE;
  Info->Mbr.PartitionType = PARTITION_ENTRY_UNUSED;
  Info->Mbr.BootIndicator = FALSE;
  Info->Mbr.HiddenSectors = 0;
  Info->StartingOffset.QuadPart = 0;
  // Partition size is disk size here.
  Info->PartitionLength.QuadPart = DOKAN_DEFAULT_DISK_SIZE;
  Info->PartitionNumber = 0;
}

NTSTATUS
DiskDeviceControl(__in PREQUEST_CONTEXT RequestContext,
                  __in PDEVICE_OBJECT DeviceObject) {
  PDokanDCB dcb;
  PDokanVCB vcb;
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject,
                    IRP_MJ_DEVICE_CONTROL);

  dcb = DeviceObject->DeviceExtension;

  if (GetIdentifierType(dcb) != DCB) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Device is not dcb so go out here");
    return STATUS_INVALID_PARAMETER;
  }

  if (IsDeletePending(DeviceObject)) {
    DOKAN_LOG_FINE_IRP(RequestContext,
                       "Device object is pending for delete valid anymore");
    return STATUS_DEVICE_REMOVED;
  }

  vcb = dcb->Vcb;
  if (IsUnmountPendingVcb(vcb)) {
    DOKAN_LOG_FINE_IRP(RequestContext,
                       "Volume is unmounted so ignore dcb requests");
    return STATUS_NO_SUCH_DEVICE;
  }

  DOKAN_LOG_FINE_IRP(RequestContext, "Device name \"%wZ\"",
                     dcb->DiskDeviceName);

  switch (RequestContext->IrpSp->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY: {
      PDISK_GEOMETRY diskGeometry;
      if (!PREPARE_OUTPUT(RequestContext->Irp, diskGeometry,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      DokanPopulateDiskGeometry(diskGeometry);
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_DISK_GET_LENGTH_INFO: {
      PGET_LENGTH_INFORMATION getLengthInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, getLengthInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      getLengthInfo->Length.QuadPart = 1024 * 1024 * 500;
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_DISK_GET_DRIVE_LAYOUT: {
      PDRIVE_LAYOUT_INFORMATION layout;
      if (!PREPARE_OUTPUT(RequestContext->Irp, layout,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      layout->PartitionCount = 1;
      layout->Signature = 1;
      DokanPopulatePartitionInfo(layout->PartitionEntry);
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_DISK_GET_DRIVE_LAYOUT_EX: {
      PDRIVE_LAYOUT_INFORMATION_EX layoutEx;
      if (!PREPARE_OUTPUT(RequestContext->Irp, layoutEx,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      layoutEx->PartitionStyle = PARTITION_STYLE_MBR;
      layoutEx->PartitionCount = 1;
      layoutEx->Mbr.Signature = 1;
      DokanPopulatePartitionInfoEx(layoutEx->PartitionEntry);
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_DISK_GET_PARTITION_INFO: {
      PPARTITION_INFORMATION partitionInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, partitionInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      DokanPopulatePartitionInfo(partitionInfo);
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_DISK_GET_PARTITION_INFO_EX: {
      PPARTITION_INFORMATION_EX partitionInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, partitionInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      DokanPopulatePartitionInfoEx(partitionInfo);
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_DISK_IS_WRITABLE:
      status = IS_DEVICE_READ_ONLY(DeviceObject) ? STATUS_MEDIA_WRITE_PROTECTED
                                                 : STATUS_SUCCESS;
      break;

    case IOCTL_DISK_MEDIA_REMOVAL:
      status = STATUS_SUCCESS;
      break;

    case IOCTL_STORAGE_MEDIA_REMOVAL:
      status = STATUS_SUCCESS;
      break;

    case IOCTL_STORAGE_GET_HOTPLUG_INFO: {
      PSTORAGE_HOTPLUG_INFO hotplugInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, hotplugInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      hotplugInfo->Size = sizeof(STORAGE_HOTPLUG_INFO);
      hotplugInfo->MediaRemovable = 1;
      hotplugInfo->MediaHotplug = 1;
      hotplugInfo->DeviceHotplug = 1;
      hotplugInfo->WriteCacheEnableOverride = 0;
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_VOLUME_GET_GPT_ATTRIBUTES: {
      PVOLUME_GET_GPT_ATTRIBUTES_INFORMATION gptAttrInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, gptAttrInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      // Set GPT read-only flag if device is not writable
      if (IS_DEVICE_READ_ONLY(DeviceObject)) {
        gptAttrInfo->GptAttributes = GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY;
      }
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_DISK_CHECK_VERIFY:
      status = STATUS_SUCCESS;
      break;

    case IOCTL_STORAGE_CHECK_VERIFY2:
      status = STATUS_SUCCESS;
      break;

    case IOCTL_STORAGE_QUERY_PROPERTY: {
      PSTORAGE_PROPERTY_QUERY query = NULL;
      GET_IRP_BUFFER_OR_BREAK(RequestContext->Irp, query);

      if (query->QueryType == PropertyExistsQuery) {
        if (query->PropertyId == StorageDeviceUniqueIdProperty) {
          DOKAN_LOG_FINE_IRP(
              RequestContext,
              "PropertyExistsQuery StorageDeviceUniqueIdProperty");

          PSTORAGE_DEVICE_UNIQUE_IDENTIFIER storage = NULL;
          GET_IRP_BUFFER_OR_BREAK(RequestContext->Irp, storage);

          status = STATUS_SUCCESS;
        } else if (query->PropertyId == StorageDeviceWriteCacheProperty) {
          DOKAN_LOG_FINE_IRP(
              RequestContext,
              "PropertyExistsQuery StorageDeviceWriteCacheProperty");
          status = STATUS_NOT_SUPPORTED;
        } else {
          DOKAN_LOG_FINE_IRP(RequestContext, "PropertyExistsQuery Unknown %d",
                             query->PropertyId);
          status = STATUS_NOT_SUPPORTED;
        }
      } else if (query->QueryType == PropertyStandardQuery) {
        if (query->PropertyId == StorageDeviceProperty) {
          DOKAN_LOG_FINE_IRP(RequestContext,
                             "PropertyStandardQuery StorageDeviceProperty");

          PSTORAGE_DEVICE_DESCRIPTOR storage = NULL;
          GET_IRP_BUFFER_OR_BREAK(RequestContext->Irp, storage);

          status = STATUS_SUCCESS;
        } else if (query->PropertyId == StorageAdapterProperty) {
          DOKAN_LOG_FINE_IRP(RequestContext,
                             "PropertyStandardQuery StorageAdapterProperty");
          status = STATUS_NOT_SUPPORTED;
        } else {
          DOKAN_LOG_FINE_IRP(RequestContext, "PropertyStandardQuery Unknown %d",
                             query->PropertyId);
          status = STATUS_ACCESS_DENIED;
        }
      } else {
        DOKAN_LOG_FINE_IRP(RequestContext, "Unknown query type %d",
                           query->QueryType);
        status = STATUS_ACCESS_DENIED;
      }
      break;
    }

    case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME: {
      // Note: GetVolumeNameForVolumeMountPoint, which wraps this function, may
      // return an error even if this returns success, if it doesn't match the
      // Mount Manager's cached data.
      PMOUNTDEV_NAME mountdevName;
      if (!PrepareOutputHelper(RequestContext->Irp, &mountdevName,
                               FIELD_OFFSET(MOUNTDEV_NAME, Name),
                          /*SetInformationOnFailure=*/TRUE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      mountdevName->NameLength = dcb->DiskDeviceName->Length;
      if (AppendVarSizeOutputString(RequestContext->Irp, &mountdevName->Name,
                                    dcb->DiskDeviceName,
                                    /*UpdateInformationOnFailure=*/FALSE,
                                    /*FillSpaceWithPartialString=*/FALSE)) {
        status = STATUS_SUCCESS;
      } else {
        status = STATUS_BUFFER_OVERFLOW;
      }
    } break;

    case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID: {
      PMOUNTDEV_UNIQUE_ID uniqueId;
      if (!PrepareOutputHelper(RequestContext->Irp, &uniqueId,
                               FIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId),
                          /*SetInformationOnFailure=*/TRUE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      uniqueId->UniqueIdLength = dcb->DiskDeviceName->Length;
      if (AppendVarSizeOutputString(RequestContext->Irp, &uniqueId->UniqueId,
                                    dcb->DiskDeviceName,
                                    /*UpdateInformationOnFailure=*/FALSE,
                                    /*FillSpaceWithPartialString=*/FALSE)) {
        status = STATUS_SUCCESS;
      } else {
        status = STATUS_BUFFER_OVERFLOW;
      }
    } break;

    case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME: {
      // Invoked when the mount manager is considering assigning a drive letter
      // to a newly mounted volume. This lets us make a non-binding request for
      // a certain drive letter before assignment happens.
      DokanLogInfo(&logger,
                   L"Mount manager is querying for desired drive letter."
                   L" ForceDriveLetterAutoAssignment = %d",
                   dcb->ForceDriveLetterAutoAssignment);

      PMOUNTDEV_SUGGESTED_LINK_NAME linkName;
      if (!PrepareOutputHelper(RequestContext->Irp, &linkName,
                               FIELD_OFFSET(MOUNTDEV_SUGGESTED_LINK_NAME, Name),
                          /*SetInformationOnFailure=*/TRUE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      if (dcb->ForceDriveLetterAutoAssignment) {
        DokanLogInfo(&logger,
                     L"Not suggesting a link name because auto-assignment was "
                     L"requested.");
        status = STATUS_NOT_FOUND;
        break;
      }
      if (dcb->MountPoint == NULL || dcb->MountPoint->Length == 0) {
        DokanLogInfo(
            &logger,
            L"Not suggesting link name because DCB has no mount point set.");
        status = STATUS_NOT_FOUND;
        break;
      }
      if (!IsMountPointDriveLetter(dcb->MountPoint)) {
        DokanLogInfo(&logger,
                     L"Not suggesting link name due to non-drive-letter mount "
                     L"point: \"%wZ\"",
                     dcb->MountPoint);
        status = STATUS_NOT_FOUND;
        break;
      }

      // Return the drive letter. Generally this is the one specified in the
      // mount request from user mode.
      linkName->UseOnlyIfThereAreNoOtherLinks = TRUE;
      linkName->NameLength = dcb->MountPoint->Length;
      if (!AppendVarSizeOutputString(RequestContext->Irp, &linkName->Name,
                                     dcb->MountPoint,
                                     /*UpdateInformationOnFailure=*/FALSE,
                                     /*FillSpaceWithPartialString=*/FALSE)) {
        DokanLogInfo(&logger,
                     L"Could not fit the suggested name in the output buffer.");
        status = STATUS_BUFFER_OVERFLOW;
        break;
      }
      DokanLogInfo(&logger, L"Returning suggested name: \"%wZ\"",
                   dcb->MountPoint);
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_MOUNTDEV_LINK_CREATED: {
      // Invoked when a mount point gets assigned by the mount manager. Usually
      // it is the one we asked for, but not always; therefore we have to update
      // the data structures that are thus far presuming it's the one we asked
      // for.

      PMOUNTDEV_NAME mountdevName = NULL;
      GET_IRP_MOUNTDEV_NAME_OR_BREAK(RequestContext->Irp, mountdevName);
      UNICODE_STRING mountdevNameString =
          DokanWrapUnicodeString(mountdevName->Name, mountdevName->NameLength);
      status = STATUS_SUCCESS;
      DokanLogInfo(&logger, L"Link created: \"%wZ\"", &mountdevNameString);
      if (mountdevName->NameLength == 0) {
        DokanLogInfo(&logger, L"Link created with empty name; ignoring.");
        break;
      }
      if (IsUnmountPending(DeviceObject)) {
        DokanLogInfo(&logger,
                     L"Link created when unmount is pending; ignoring.");
        break;
      }
      if (!dcb->PersistentSymbolicLinkName &&
          StartsWithVolumeGuidPrefix(&mountdevNameString)) {
        dcb->PersistentSymbolicLinkName =
            DokanAllocDuplicateString(&mountdevNameString);
        break;
      }
      if (!StartsWithDosDevicesPrefix(&mountdevNameString)) {
        DokanLogInfo(&logger, L"Link name is not under DosDevices; ignoring.");
        break;
      }
      if (dcb->MountPoint &&
          RtlEqualUnicodeString(dcb->MountPoint, &mountdevNameString,
                                /*CaseInsensitive=*/FALSE)) {
        dcb->MountPointDetermined = TRUE;
        DokanLogInfo(&logger, L"Link name matches the current one.");
        break;
      }

      // Update the mount point on the DCB for the volume.
      if (dcb->MountPoint) {
        ExFreePool(dcb->MountPoint);
      }
      dcb->MountPoint = DokanAllocDuplicateString(&mountdevNameString);
      if (dcb->MountPoint == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        break;
      }
      dcb->MountPointDetermined = TRUE;

      // Update the mount point in dokan's global list, so that other dokan
      // functions (e.g. for unmounting) can look up the drive by mount point
      // later.
      PMOUNT_ENTRY mountEntry = FindMountEntryByName(
          dcb->Global, dcb->DiskDeviceName, dcb->UNCName, /*LockGlobal=*/TRUE);
      if (mountEntry != NULL) {
        RtlStringCchCopyUnicodeString(mountEntry->MountControl.MountPoint,
                                      MAXIMUM_FILENAME_LENGTH, dcb->MountPoint);
      } else {
        DokanLogInfo(&logger, L"Cannot find associated MountEntry to update.");
      }
    } break;

    case IOCTL_MOUNTDEV_LINK_DELETED: {
      // This is normally triggered by dokan-initiated unmounting, in which case
      // it is an uninteresting acknowledgement from the mount manager. That is
      // the case even for most edge cases like termination of the mounting
      // process. However, it can be triggered due to external deletion of the
      // mount point, in which case we trigger the actual unmounting from here.

      PMOUNTDEV_NAME mountdevName = NULL;
      GET_IRP_MOUNTDEV_NAME_OR_BREAK(RequestContext->Irp, mountdevName);
      UNICODE_STRING mountdevNameString =
          DokanWrapUnicodeString(mountdevName->Name, mountdevName->NameLength);
      status = STATUS_SUCCESS;
      DokanLogInfo(&logger, L"Link deleted: \"%wZ\"", &mountdevNameString);
      if (!dcb->UseMountManager) {
        DokanLogInfo(
            &logger,
            L"Mount manager is disabled for this device or dokan initiated"
            L" deletion; ignoring.");
        break;
      }
      if (!dcb->MountPoint || dcb->MountPoint->Length == 0) {
        DokanLogInfo(&logger,
                     L"Deleting the device even though it never had the mount "
                     L"point set.");
        status = DokanEventRelease(RequestContext, vcb->DeviceObject);
        break;
      }
      if (!RtlEqualUnicodeString(dcb->MountPoint, &mountdevNameString,
                                 /*CaseInsensitive=*/FALSE)) {
        DokanLogInfo(&logger,
                     L"Ignoring deletion because device has different mount "
                     L"point: \"%wZ\"",
                     dcb->MountPoint);
        break;
      }
      status = DokanEventRelease(RequestContext, vcb->DeviceObject);
    } break;

    case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS: {
      PVOLUME_DISK_EXTENTS volume;
      if (!PREPARE_OUTPUT(RequestContext->Irp, volume,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_INVALID_PARAMETER;
        break;
      }
      volume->NumberOfDiskExtents = 1;
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_REDIR_QUERY_PATH_EX:
    case IOCTL_REDIR_QUERY_PATH: {
      PQUERY_PATH_RESPONSE pathResp;
      BOOLEAN prefixOk = FALSE;

      if (dcb->UNCName != NULL && dcb->UNCName->Length > 0) {
        if (RequestContext->Irp->RequestorMode != KernelMode) {
          break;
        }

        WCHAR* lpPath = NULL;
        ULONG ulPath = 0;

        if (RequestContext->IrpSp->Parameters.DeviceIoControl.IoControlCode ==
            IOCTL_REDIR_QUERY_PATH) {
          PQUERY_PATH_REQUEST pathReq;

          GET_IRP_BUFFER_OR_BREAK(RequestContext->Irp, pathReq);

          DOKAN_LOG_FINE_IRP(RequestContext, "PathNameLength = %d",
                             pathReq->PathNameLength);
          DOKAN_LOG_FINE_IRP(RequestContext, "SecurityContext = %p",
                             pathReq->SecurityContext);
          DOKAN_LOG_FINE_IRP(
              RequestContext, "FilePathName = %.*ls",
              (unsigned int)(pathReq->PathNameLength / sizeof(WCHAR)),
              pathReq->FilePathName);

          lpPath = pathReq->FilePathName;
          ulPath = pathReq->PathNameLength / sizeof(WCHAR);

          if (pathReq->PathNameLength >= dcb->UNCName->Length / sizeof(WCHAR)) {
            prefixOk = (_wcsnicmp(pathReq->FilePathName, dcb->UNCName->Buffer,
                                  dcb->UNCName->Length / sizeof(WCHAR)) == 0);
          }
        } else {
          PQUERY_PATH_REQUEST_EX pathReqEx;

          GET_IRP_BUFFER_OR_BREAK(RequestContext->Irp, pathReqEx);

          DOKAN_LOG_FINE_IRP(RequestContext, "pSecurityContext = %p",
                             pathReqEx->pSecurityContext);
          DOKAN_LOG_FINE_IRP(RequestContext, "EaLength = %d",
                             pathReqEx->EaLength);
          DOKAN_LOG_FINE_IRP(RequestContext, "pEaBuffer = %p",
                             pathReqEx->pEaBuffer);
          DOKAN_LOG_FINE_IRP(RequestContext, "PathNameLength = %d",
                             pathReqEx->PathName.Length);
          DOKAN_LOG_FINE_IRP(
              RequestContext, "FilePathName = %*ls",
              (unsigned int)(pathReqEx->PathName.Length / sizeof(WCHAR)),
              pathReqEx->PathName.Buffer);

          lpPath = pathReqEx->PathName.Buffer;
          ulPath = pathReqEx->PathName.Length / sizeof(WCHAR);

          if (pathReqEx->PathName.Length >= dcb->UNCName->Length) {
            prefixOk =
                (_wcsnicmp(pathReqEx->PathName.Buffer, dcb->UNCName->Buffer,
                           dcb->UNCName->Length / sizeof(WCHAR)) == 0);
          }
        }

        unsigned int i = 1;
        for (; i < ulPath && i * sizeof(WCHAR) < dcb->UNCName->Length &&
               !prefixOk;
             ++i) {
          if (_wcsnicmp(&lpPath[i], &dcb->UNCName->Buffer[i], 1) != 0) {
            break;
          }

          if ((i + 1) * sizeof(WCHAR) < dcb->UNCName->Length) {
            prefixOk = (dcb->UNCName->Buffer[i + 1] == L'\\');
          }
        }

        if (!prefixOk) {
          status = STATUS_BAD_NETWORK_PATH;
          break;
        }

        for (;
             i < ulPath && i * sizeof(WCHAR) < dcb->UNCName->Length && prefixOk;
             ++i) {
          if (_wcsnicmp(&lpPath[i], &dcb->UNCName->Buffer[i], 1) != 0) {
            prefixOk = FALSE;
          }
        }

        if (!prefixOk) {
          status = STATUS_BAD_NETWORK_NAME;
          break;
        }

        if (!PREPARE_OUTPUT(RequestContext->Irp, pathResp,
                            /*SetInformationOnFailure=*/FALSE)) {
          status = STATUS_BUFFER_TOO_SMALL;
          break;
        }

        pathResp->LengthAccepted = dcb->UNCName->Length;
        status = STATUS_SUCCESS;
      }
    } break;

    case IOCTL_STORAGE_GET_MEDIA_TYPES_EX: {
      PGET_MEDIA_TYPES mediaTypes = NULL;
      PDEVICE_MEDIA_INFO mediaInfo = NULL;  //&mediaTypes->MediaInfo[0];

      // We always return only one media type, so in our case it's a fixed-size
      // struct.
      if (!PREPARE_OUTPUT(RequestContext->Irp, mediaTypes,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      mediaInfo = &mediaTypes->MediaInfo[0];
      mediaTypes->DeviceType = FILE_DEVICE_VIRTUAL_DISK;
      mediaTypes->MediaInfoCount = 1;

      PDISK_GEOMETRY diskGeometry = DokanAllocZero(sizeof(DISK_GEOMETRY));
      if (diskGeometry == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        break;
      }
      DokanPopulateDiskGeometry(diskGeometry);
      mediaInfo->DeviceSpecific.DiskInfo.MediaType = diskGeometry->MediaType;
      mediaInfo->DeviceSpecific.DiskInfo.NumberMediaSides = 1;
      mediaInfo->DeviceSpecific.DiskInfo.MediaCharacteristics =
          (MEDIA_CURRENTLY_MOUNTED | MEDIA_READ_WRITE);
      mediaInfo->DeviceSpecific.DiskInfo.Cylinders.QuadPart =
          diskGeometry->Cylinders.QuadPart;
      mediaInfo->DeviceSpecific.DiskInfo.TracksPerCylinder =
          diskGeometry->TracksPerCylinder;
      mediaInfo->DeviceSpecific.DiskInfo.SectorsPerTrack =
          diskGeometry->SectorsPerTrack;
      mediaInfo->DeviceSpecific.DiskInfo.BytesPerSector =
          diskGeometry->BytesPerSector;
      ExFreePool(diskGeometry);
      status = STATUS_SUCCESS;
    } break;

    case IOCTL_STORAGE_GET_DEVICE_NUMBER: {
      PSTORAGE_DEVICE_NUMBER deviceNumber;
      if (!PREPARE_OUTPUT(RequestContext->Irp, deviceNumber,
                          /*SetInformationOnFailure=*/TRUE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      deviceNumber->DeviceType = FILE_DEVICE_VIRTUAL_DISK;
      if (vcb) {
        deviceNumber->DeviceType = vcb->DeviceObject->DeviceType;
      }

      deviceNumber->DeviceNumber = 0;  // Always one volume only per disk device
      deviceNumber->PartitionNumber = (ULONG)-1;  // Not partitionable

      status = STATUS_SUCCESS;
    } break;

    default:
      DOKAN_LOG_FINE_IRP(
          RequestContext, "Unsupported IoControlCode %x",
          RequestContext->IrpSp->Parameters.DeviceIoControl.IoControlCode);
      break;
  }

  return status;
}

// It is intentional to not use the Dcb from the RequestContext as this function
// is also used when passing down a DeviceObject in VolumeDeviceControl
NTSTATUS
DiskDeviceControlWithLock(__in PREQUEST_CONTEXT RequestContext,
                          __in PDEVICE_OBJECT DeviceObject) {
  PDokanDCB dcb;
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  dcb = DeviceObject->DeviceExtension;

  if (dcb == NULL || GetIdentifierType(dcb) != DCB) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Device is not dcb so go out here");
    return STATUS_INVALID_PARAMETER;
  }

  status = IoAcquireRemoveLock(&dcb->RemoveLock, RequestContext->Irp);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "IoAcquireRemoveLock failed with %#x %s",
                       status, DokanGetNTSTATUSStr(status));
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (IsDeletePending(DeviceObject)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Device is deleted, so go out here");
    IoReleaseRemoveLock(&dcb->RemoveLock, RequestContext->Irp);
    return STATUS_NO_SUCH_DEVICE;
  }
  status = DiskDeviceControl(RequestContext, DeviceObject);

  IoReleaseRemoveLock(&dcb->RemoveLock, RequestContext->Irp);

  return status;
}

// Determines whether the given file object was obtained by opening the volume
// itself as opposed to a specific file.
BOOLEAN
IsVolumeOpen(__in PDokanVCB Vcb, __in PFILE_OBJECT FileObject) {
  return FileObject != NULL && FileObject->FsContext == &Vcb->VolumeFileHeader;
}

NTSTATUS DokanGetVolumeMetrics(__in PREQUEST_CONTEXT RequestContext) {
  VOLUME_METRICS* outputBuffer;
  if (!PREPARE_OUTPUT(RequestContext->Irp, outputBuffer,
                      /*SetInformationOnFailure=*/TRUE)) {
    return STATUS_BUFFER_TOO_SMALL;
  }
  DokanVCBLockRO(RequestContext->Vcb);
  *outputBuffer = RequestContext->Vcb->VolumeMetrics;
  DokanVCBUnlock(RequestContext->Vcb);
  return STATUS_SUCCESS;
}

NTSTATUS
VolumeDeviceControl(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  ULONG baseCode = DEVICE_TYPE_FROM_CTL_CODE(
      RequestContext->IrpSp->Parameters.DeviceIoControl.IoControlCode);
  switch (baseCode) {
    case IOCTL_STORAGE_BASE:
    case IOCTL_DISK_BASE:
    case FILE_DEVICE_NETWORK_FILE_SYSTEM:
      status = DiskDeviceControlWithLock(RequestContext,
                                         RequestContext->Dcb->DeviceObject);
      break;

    case MOUNTDEVCONTROLTYPE:
      // Device control functions are only supposed to work on a volume handle.
      // Some win32 functions, like GetVolumePathName, rely on these operations
      // failing for file/directory handles. On the other hand, dokan issues its
      // custom operations on non-volume handles, so we can't do this check at
      // the top.
      if (!IsVolumeOpen(RequestContext->Vcb,
                       RequestContext->IrpSp->FileObject)) {
        status = STATUS_INVALID_PARAMETER;
        break;
      }
      status = DiskDeviceControlWithLock(RequestContext,
                                         RequestContext->Dcb->DeviceObject);
      break;

    default:
      DOKAN_LOG_FINE_IRP(
          RequestContext, "Unsupported IoControlCode %x",
          RequestContext->IrpSp->Parameters.DeviceIoControl.IoControlCode);
      break;
  }

  return status;
}

NTSTATUS
DokanDispatchDeviceControl(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  DOKAN_LOG_IOCTL(
      RequestContext,
      RequestContext->IrpSp->Parameters.DeviceIoControl.IoControlCode,
      "DeviceControl");

  if (RequestContext->DeviceObject->DriverObject == NULL ||
      RequestContext->DeviceObject->ReferenceCount < 0) {
    return STATUS_DEVICE_DOES_NOT_EXIST;
  }

  if (RequestContext->DokanGlobal) {
    status = GlobalDeviceControl(RequestContext);
  } else if (RequestContext->Vcb) {
    status = VolumeDeviceControl(RequestContext);
  } else if (RequestContext->Dcb) {
    status =
        DiskDeviceControlWithLock(RequestContext, RequestContext->DeviceObject);
  }

  if (status != STATUS_PENDING && status != STATUS_INVALID_DEVICE_REQUEST) {
    if (IsDeletePending(RequestContext->DeviceObject)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "DeviceObject is not longer valid.");
      status = STATUS_DEVICE_REMOVED;
      RequestContext->Irp->IoStatus.Information = 0;
    }
  }

  return status;
}