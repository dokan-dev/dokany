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
#include "irp_buffer_helper.h"

#include <mountdev.h>
#include <mountmgr.h>
#include <ntddvol.h>
#include <storduid.h>

VOID PrintUnknownDeviceIoctlCode(__in ULONG IoctlCode) {
  PCHAR baseCodeStr = "unknown";
  ULONG baseCode = DEVICE_TYPE_FROM_CTL_CODE(IoctlCode);
  ULONG functionCode = (IoctlCode & (~0xffffc003)) >> 2;

  DDbgPrint("   Unknown Code 0x%x\n", IoctlCode);

  switch (baseCode) {
  case IOCTL_STORAGE_BASE:
    baseCodeStr = "IOCTL_STORAGE_BASE";
    break;
  case IOCTL_DISK_BASE:
    baseCodeStr = "IOCTL_DISK_BASE";
    break;
  case IOCTL_VOLUME_BASE:
    baseCodeStr = "IOCTL_VOLUME_BASE";
    break;
  case MOUNTDEVCONTROLTYPE:
    baseCodeStr = "MOUNTDEVCONTROLTYPE";
    break;
  case MOUNTMGRCONTROLTYPE:
    baseCodeStr = "MOUNTMGRCONTROLTYPE";
    break;
  default:
    break;
  }
  UNREFERENCED_PARAMETER(functionCode);
  DDbgPrint("   BaseCode: 0x%x(%s) FunctionCode 0x%x(%d)\n", baseCode,
            baseCodeStr, functionCode, functionCode);
}

NTSTATUS
GlobalDeviceControl(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;
  PDOKAN_GLOBAL dokanGlobal;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  DDbgPrint("   => DokanGlobalDeviceControl\n");
  irpSp = IoGetCurrentIrpStackLocation(Irp);
  dokanGlobal = DeviceObject->DeviceExtension;

  switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
  case IOCTL_EVENT_START:
    DDbgPrint("  IOCTL_EVENT_START\n");
    status = DokanEventStart(DeviceObject, Irp);
    break;

  case IOCTL_SET_DEBUG_MODE: {
    PULONG pDebug = NULL;
    GET_IRP_BUFFER_OR_BREAK(Irp, pDebug)
    g_Debug = *pDebug;
    status = STATUS_SUCCESS;
    DDbgPrint("  IOCTL_SET_DEBUG_MODE: %d\n", g_Debug);
  } break;

  case IOCTL_EVENT_RELEASE:
    DDbgPrint("  IOCTL_EVENT_RELEASE\n");
    status = DokanGlobalEventRelease(DeviceObject, Irp);
    break;

  case IOCTL_MOUNTPOINT_CLEANUP:
    RemoveSessionDevices(dokanGlobal, GetCurrentSessionId(Irp));
    status = STATUS_SUCCESS;
    break;

  case IOCTL_EVENT_MOUNTPOINT_LIST:
    if (GetIdentifierType(dokanGlobal) != DGL) {
      return STATUS_INVALID_PARAMETER;
    }
    status = DokanGetMountPointList(DeviceObject, Irp, dokanGlobal);
    break;

  case IOCTL_GET_VERSION: {
    ULONG* version;
    if (!PREPARE_OUTPUT(Irp, version, /*SetInformationOnFailure=*/FALSE)) {
      break;
    }
    *version = (ULONG) DOKAN_DRIVER_VERSION;
    status = STATUS_SUCCESS;
  } break;

  default:
    PrintUnknownDeviceIoctlCode(
        irpSp->Parameters.DeviceIoControl.IoControlCode);
    status = STATUS_INVALID_PARAMETER;
    break;
  }

  DDbgPrint("   <= DokanGlobalDeviceControl\n");
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
DiskDeviceControl(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;
  PDokanDCB dcb;
  PDokanVCB vcb;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  DOKAN_INIT_LOGGER(logger, DeviceObject->DriverObject, IRP_MJ_DEVICE_CONTROL);

  DDbgPrint("   => DokanDiskDeviceControl\n");
  irpSp = IoGetCurrentIrpStackLocation(Irp);
  dcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(dcb) != DCB) {
    PrintIdType(dcb);
    DDbgPrint("   Device is not dcb so go out here\n");
    return STATUS_INVALID_PARAMETER;
  }

  if (IsDeletePending(DeviceObject)) {
    DDbgPrint("   Device object is pending for delete valid anymore\n");
    return STATUS_DEVICE_REMOVED;
  }

  vcb = dcb->Vcb;
  if (IsUnmountPendingVcb(vcb)) {
    DDbgPrint("   Volume is unmounted so ignore dcb requests\n");
    return STATUS_NO_SUCH_DEVICE;
  }

  DDbgPrint("   DiskDeviceControl Device name %wZ \n", dcb->DiskDeviceName);

  switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
  case IOCTL_DISK_GET_DRIVE_GEOMETRY: {
    DDbgPrint("  IOCTL_DISK_GET_DRIVE_GEOMETRY\n");
    PDISK_GEOMETRY diskGeometry;
    if (!PREPARE_OUTPUT(Irp, diskGeometry,
                        /*SetInformationOnFailure=*/FALSE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }
    DokanPopulateDiskGeometry(diskGeometry);
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_DISK_GET_LENGTH_INFO: {
    DDbgPrint("  IOCTL_DISK_GET_LENGTH_INFO\n");
    PGET_LENGTH_INFORMATION getLengthInfo;
    if (!PREPARE_OUTPUT(Irp, getLengthInfo,
                        /*SetInformationOnFailure=*/FALSE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }

    getLengthInfo->Length.QuadPart = 1024 * 1024 * 500;
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_DISK_GET_DRIVE_LAYOUT: {
    DDbgPrint("  IOCTL_DISK_GET_DRIVE_LAYOUT\n");
    PDRIVE_LAYOUT_INFORMATION layout;
    if (!PREPARE_OUTPUT(Irp, layout,  /*SetInformationOnFailure=*/FALSE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }
    layout->PartitionCount = 1;
    layout->Signature = 1;
    DokanPopulatePartitionInfo(layout->PartitionEntry);
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_DISK_GET_DRIVE_LAYOUT_EX: {
    DDbgPrint("  IOCTL_DISK_GET_DRIVE_LAYOUT_EX\n");
    PDRIVE_LAYOUT_INFORMATION_EX layoutEx;
    if (!PREPARE_OUTPUT(Irp, layoutEx, /*SetInformationOnFailure=*/FALSE)) {
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
    DDbgPrint("  IOCTL_DISK_GET_PARTITION_INFO\n");
    PPARTITION_INFORMATION partitionInfo;
    if (!PREPARE_OUTPUT(Irp, partitionInfo,
                        /*SetInformationOnFailure=*/FALSE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }
    DokanPopulatePartitionInfo(partitionInfo);
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_DISK_GET_PARTITION_INFO_EX: {
    DDbgPrint("  IOCTL_DISK_GET_PARTITION_INFO_EX\n");
    PPARTITION_INFORMATION_EX partitionInfo;
    if (!PREPARE_OUTPUT(Irp, partitionInfo,
                        /*SetInformationOnFailure=*/FALSE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }
    DokanPopulatePartitionInfoEx(partitionInfo);
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_DISK_IS_WRITABLE:
    DDbgPrint("  IOCTL_DISK_IS_WRITABLE\n");
    status = IS_DEVICE_READ_ONLY(DeviceObject) ? STATUS_MEDIA_WRITE_PROTECTED
                                               : STATUS_SUCCESS;
    break;

  case IOCTL_DISK_MEDIA_REMOVAL:
    DDbgPrint("  IOCTL_DISK_MEDIA_REMOVAL\n");
    status = STATUS_SUCCESS;
    break;

  case IOCTL_STORAGE_MEDIA_REMOVAL:
    DDbgPrint("  IOCTL_STORAGE_MEDIA_REMOVAL\n");
    status = STATUS_SUCCESS;
    break;

  case IOCTL_DISK_SET_PARTITION_INFO:
    DDbgPrint("  IOCTL_DISK_SET_PARTITION_INFO\n");
    break;

  case IOCTL_DISK_VERIFY:
    DDbgPrint("  IOCTL_DISK_VERIFY\n");
    break;

  case IOCTL_STORAGE_GET_HOTPLUG_INFO: {
    DDbgPrint("  IOCTL_STORAGE_GET_HOTPLUG_INFO\n");
    PSTORAGE_HOTPLUG_INFO hotplugInfo;
    if (!PREPARE_OUTPUT(Irp, hotplugInfo, /*SetInformationOnFailure=*/FALSE)) {
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
    DDbgPrint("   IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
    PVOLUME_GET_GPT_ATTRIBUTES_INFORMATION gptAttrInfo;
    if (!PREPARE_OUTPUT(Irp, gptAttrInfo, /*SetInformationOnFailure=*/FALSE)) {
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
    DDbgPrint("  IOCTL_STORAGE_CHECK_VERIFY\n");
    status = STATUS_SUCCESS;
    break;

  case IOCTL_STORAGE_CHECK_VERIFY2:
    DDbgPrint("  IOCTL_STORAGE_CHECK_VERIFY2\n");
    status = STATUS_SUCCESS;
    break;

  case IOCTL_STORAGE_QUERY_PROPERTY:
    DDbgPrint("  IOCTL_STORAGE_QUERY_PROPERTY\n");

    PSTORAGE_PROPERTY_QUERY query = NULL;
    GET_IRP_BUFFER_OR_BREAK(Irp, query)

    if (query->QueryType == PropertyExistsQuery) {
      if (query->PropertyId == StorageDeviceUniqueIdProperty) {
        DDbgPrint("    PropertyExistsQuery StorageDeviceUniqueIdProperty\n");

        PSTORAGE_DEVICE_UNIQUE_IDENTIFIER storage = NULL;
        GET_IRP_BUFFER_OR_BREAK(Irp, storage)

        status = STATUS_SUCCESS;
      } else if (query->PropertyId == StorageDeviceWriteCacheProperty) {
        DDbgPrint("    PropertyExistsQuery StorageDeviceWriteCacheProperty\n");
        status = STATUS_NOT_IMPLEMENTED;
      } else {
        DDbgPrint("    PropertyExistsQuery Unknown %d\n", query->PropertyId);
        status = STATUS_NOT_IMPLEMENTED;
      }
    } else if (query->QueryType == PropertyStandardQuery) {
      if (query->PropertyId == StorageDeviceProperty) {
        DDbgPrint("    PropertyStandardQuery StorageDeviceProperty\n");

        PSTORAGE_DEVICE_DESCRIPTOR storage = NULL;
        GET_IRP_BUFFER_OR_BREAK(Irp, storage)

        status = STATUS_SUCCESS;
      } else if (query->PropertyId == StorageAdapterProperty) {
        DDbgPrint("    PropertyStandardQuery StorageAdapterProperty\n");
        status = STATUS_NOT_IMPLEMENTED;
      } else {
        DDbgPrint("    PropertyStandardQuery Unknown %d\n", query->PropertyId);
        status = STATUS_ACCESS_DENIED;
      }
    } else {
      DDbgPrint("    Unknown query type %d\n", query->QueryType);
      status = STATUS_ACCESS_DENIED;
    }
    break;

  case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME: {
    // Note: GetVolumeNameForVolumeMountPoint, which wraps this function, may
    // return an error even if this returns success, if it doesn't match the
    // Mount Manager's cached data.
    DDbgPrint("   IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");
    PMOUNTDEV_NAME mountdevName;
    if (!PREPARE_OUTPUT(Irp, mountdevName, /*SetInformationOnFailure=*/TRUE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }
    mountdevName->NameLength = dcb->DiskDeviceName->Length;
    if (AppendVarSizeOutputString(Irp, &mountdevName->Name, dcb->DiskDeviceName,
                                  /*UpdateInformationOnFailure=*/FALSE,
                                  /*FillSpaceWithPartialString=*/FALSE)) {
      status = STATUS_SUCCESS;
    } else {
      status = STATUS_BUFFER_OVERFLOW;
    }
  } break;

  case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID: {
    DDbgPrint("   IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
    PMOUNTDEV_UNIQUE_ID uniqueId;
    if (!PREPARE_OUTPUT(Irp, uniqueId, /*SetInformationOnFailure=*/TRUE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }

    uniqueId->UniqueIdLength = dcb->DiskDeviceName->Length;
    if (AppendVarSizeOutputString(Irp, &uniqueId->UniqueId, dcb->DiskDeviceName,
                                  /*UpdateInformationOnFailure=*/FALSE,
                                  /*FillSpaceWithPartialString=*/FALSE)) {
      status = STATUS_SUCCESS;
    } else {
      status = STATUS_BUFFER_OVERFLOW;
    }
  } break;

  case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME: {
    // Invoked when the mount manager is considering assigning a drive letter
    // to a newly mounted volume. This lets us make a non-binding request for a
    // certain drive letter before assignment happens.
    DDbgPrint("   IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME\n");

    PMOUNTDEV_SUGGESTED_LINK_NAME linkName;
    if (!PREPARE_OUTPUT(Irp, linkName, /*SetInformationOnFailure=*/TRUE)) {
      status = STATUS_BUFFER_TOO_SMALL;
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
      DokanLogInfo(
          &logger,
          L"Not suggesting link name due to non-drive-letter mount point: %wZ",
          dcb->MountPoint);
      status = STATUS_NOT_FOUND;
      break;
    }

    // Return the drive letter. Generally this is the one specified in the mount
    // request from user mode.
    linkName->UseOnlyIfThereAreNoOtherLinks = FALSE;
    linkName->NameLength = dcb->MountPoint->Length;
    if (!AppendVarSizeOutputString(Irp, &linkName->Name, dcb->MountPoint,
                                   /*UpdateInformationOnFailure=*/FALSE,
                                   /*FillSpaceWithPartialString=*/FALSE)) {
      DokanLogInfo(
          &logger,
          L"Could not fit the suggested name in the output buffer.");
      status = STATUS_BUFFER_OVERFLOW;
      break;
    }
    DokanLogInfo(&logger, L"Returning suggested name: %wZ", dcb->MountPoint);
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_MOUNTDEV_LINK_CREATED: {
    // Invoked when a mount point gets assigned by the mount manager. Usually it
    // is the one we asked for, but not always; therefore we have to update the
    // data structures that are thus far presuming it's the one we asked for.
    DDbgPrint("   IOCTL_MOUNTDEV_LINK_CREATED\n");

    PMOUNTDEV_NAME mountdevName = NULL;
    GET_IRP_MOUNTDEV_NAME_OR_BREAK(Irp, mountdevName)
    UNICODE_STRING mountdevNameString =
        DokanWrapUnicodeString(mountdevName->Name, mountdevName->NameLength);
    status = STATUS_SUCCESS;
    DokanLogInfo(&logger, L"Link created: %wZ", &mountdevNameString);
    if (mountdevName->NameLength == 0) {
      DokanLogInfo(&logger, L"Link created with empty name; ignoring.");
      break;
    }
    if (IsUnmountPending(DeviceObject)) {
      DokanLogInfo(&logger, L"Link created when unmount is pending; ignoring.");
      break;
    }
    if (!StartsWithDosDevicesPrefix(&mountdevNameString)) {
      DokanLogInfo(&logger, L"Link name is not under DosDevices; ignoring.");
      break;
    }
    if (dcb->MountPoint && RtlEqualUnicodeString(dcb->MountPoint,
                                                 &mountdevNameString,
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
    DDbgPrint("   IOCTL_MOUNTDEV_LINK_DELETED\n");

    PMOUNTDEV_NAME mountdevName = NULL;
    GET_IRP_MOUNTDEV_NAME_OR_BREAK(Irp, mountdevName)
    UNICODE_STRING mountdevNameString =
        DokanWrapUnicodeString(mountdevName->Name, mountdevName->NameLength);
    status = STATUS_SUCCESS;
    DokanLogInfo(&logger, L"Link deleted: %wZ", &mountdevNameString);
    if (!dcb->UseMountManager) {
      DokanLogInfo(
          &logger,
          L"Mount manager is disabled for this device or dokan initiated"
          L" deletion; ignoring.");
      break;
    }
    if (!dcb->MountPoint || dcb->MountPoint->Length == 0) {
      DokanLogInfo(
          &logger,
          L"Deleting the device even though it never had the mount point set.");
      status = DokanEventRelease(vcb->DeviceObject, Irp);
      break;
    }
    if (!RtlEqualUnicodeString(dcb->MountPoint, &mountdevNameString,
                               /*CaseInsensitive=*/FALSE)) {
      DokanLogInfo(
          &logger,
          L"Ignoring deletion because device has different mount point: %wZ",
          dcb->MountPoint);
      break;
    }
    status = DokanEventRelease(vcb->DeviceObject, Irp);
  } break;

  case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
    DDbgPrint("   IOCTL_MOUNTDEV_QUERY_STABLE_GUID\n");
    break;
  case IOCTL_VOLUME_ONLINE:
    DDbgPrint("   IOCTL_VOLUME_ONLINE\n");
    status = STATUS_SUCCESS;
    break;
  case IOCTL_VOLUME_OFFLINE:
    DDbgPrint("   IOCTL_VOLUME_OFFLINE\n");
    status = STATUS_SUCCESS;
    break;
  case IOCTL_VOLUME_READ_PLEX:
    DDbgPrint("   IOCTL_VOLUME_READ_PLEX\n");
    break;
  case IOCTL_VOLUME_PHYSICAL_TO_LOGICAL:
    DDbgPrint("   IOCTL_VOLUME_PHYSICAL_TO_LOGICAL\n");
    break;
  case IOCTL_VOLUME_IS_CLUSTERED:
    DDbgPrint("   IOCTL_VOLUME_IS_CLUSTERED\n");
    break;
  case IOCTL_VOLUME_PREPARE_FOR_CRITICAL_IO:
    DDbgPrint("   IOCTL_VOLUME_PREPARE_FOR_CRITICAL_IO\n");
    break;

  case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS: {
    DDbgPrint("   IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\n");
    PVOLUME_DISK_EXTENTS volume;
    if (!PREPARE_OUTPUT(Irp, volume, /*SetInformationOnFailure=*/FALSE)) {
      status = STATUS_INVALID_PARAMETER;
      break;
    }
    volume->NumberOfDiskExtents = 1;
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_STORAGE_EJECT_MEDIA: {
    DDbgPrint("   IOCTL_STORAGE_EJECT_MEDIA\n");
    DokanUnmount(dcb);
    status = STATUS_SUCCESS;
  } break;

  case IOCTL_REDIR_QUERY_PATH_EX:
  case IOCTL_REDIR_QUERY_PATH: {
    PQUERY_PATH_RESPONSE pathResp;
    BOOLEAN prefixOk = FALSE;

    if (dcb->UNCName != NULL && dcb->UNCName->Length > 0) {
      if (Irp->RequestorMode != KernelMode) {
        break;
      }

      WCHAR *lpPath = NULL;
      ULONG ulPath = 0;

      if (irpSp->Parameters.DeviceIoControl.IoControlCode ==
          IOCTL_REDIR_QUERY_PATH) {
        PQUERY_PATH_REQUEST pathReq;
        DDbgPrint("  IOCTL_REDIR_QUERY_PATH\n");

        GET_IRP_BUFFER_OR_BREAK(Irp, pathReq);

        DDbgPrint("   PathNameLength = %d\n", pathReq->PathNameLength);
        DDbgPrint("   SecurityContext = %p\n", pathReq->SecurityContext);
        DDbgPrint("   FilePathName = %.*ls\n",
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
        DDbgPrint("  IOCTL_REDIR_QUERY_PATH_EX\n");

        GET_IRP_BUFFER_OR_BREAK(Irp, pathReqEx);

        DDbgPrint("   pSecurityContext = %p\n", pathReqEx->pSecurityContext);
        DDbgPrint("   EaLength = %d\n", pathReqEx->EaLength);
        DDbgPrint("   pEaBuffer = %p\n", pathReqEx->pEaBuffer);
        DDbgPrint("   PathNameLength = %d\n", pathReqEx->PathName.Length);
        DDbgPrint("   FilePathName = %*ls\n",
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
      for (;
           i < ulPath && i * sizeof(WCHAR) < dcb->UNCName->Length && !prefixOk;
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

      for (; i < ulPath && i * sizeof(WCHAR) < dcb->UNCName->Length && prefixOk;
           ++i) {
        if (_wcsnicmp(&lpPath[i], &dcb->UNCName->Buffer[i], 1) != 0) {
          prefixOk = FALSE;
        }
      }

      if (!prefixOk) {
        status = STATUS_BAD_NETWORK_NAME;
        break;
      }

      if (!PREPARE_OUTPUT(Irp, pathResp, /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      pathResp->LengthAccepted = dcb->UNCName->Length;
      status = STATUS_SUCCESS;
    }
  } break;
  case IOCTL_STORAGE_GET_MEDIA_TYPES_EX: {
    DDbgPrint("  IOCTL_STORAGE_GET_MEDIA_TYPES_EX\n");

    PGET_MEDIA_TYPES mediaTypes = NULL;
    PDEVICE_MEDIA_INFO mediaInfo = NULL; //&mediaTypes->MediaInfo[0];

    // We always return only one media type, so in our case it's a fixed-size
    // struct.
    if (!PREPARE_OUTPUT(Irp, mediaTypes, /*SetInformationOnFailure=*/FALSE)) {
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
    DDbgPrint("  IOCTL_STORAGE_GET_DEVICE_NUMBER\n");
    PSTORAGE_DEVICE_NUMBER deviceNumber;
    if (!PREPARE_OUTPUT(Irp, deviceNumber, /*SetInformationOnFailure=*/TRUE)) {
      status = STATUS_BUFFER_TOO_SMALL;
      break;
    }

    deviceNumber->DeviceType = FILE_DEVICE_VIRTUAL_DISK;
    if (vcb) {
      deviceNumber->DeviceType = vcb->DeviceObject->DeviceType;
    }

    deviceNumber->DeviceNumber = 0; // Always one volume only per disk device
    deviceNumber->PartitionNumber = (ULONG)-1; // Not partitionable

    status = STATUS_SUCCESS;
  } break;

  default:
    PrintUnknownDeviceIoctlCode(
        irpSp->Parameters.DeviceIoControl.IoControlCode);
    status = STATUS_INVALID_DEVICE_REQUEST;
    break;
  }
  DDbgPrint("   <= DokanDiskDeviceControl\n");
  return status;
}

NTSTATUS
DiskDeviceControlWithLock(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {

  PDokanDCB dcb;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  dcb = DeviceObject->DeviceExtension;

  if (GetIdentifierType(dcb) != DCB) {
    PrintIdType(dcb);
    DDbgPrint("   Device is not dcb so go out here\n");
    return STATUS_INVALID_PARAMETER;
  }

  status = IoAcquireRemoveLock(&dcb->RemoveLock, Irp);
  if (!NT_SUCCESS(status)) {
    DDbgPrint("IoAcquireRemoveLock failed with %#x", status);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (IsDeletePending(DeviceObject)) {
    DDbgPrint("Device is deleted, so go out here \n");
    IoReleaseRemoveLock(&dcb->RemoveLock, Irp);
    return STATUS_NO_SUCH_DEVICE;
  }
  status = DiskDeviceControl(DeviceObject, Irp);

  IoReleaseRemoveLock(&dcb->RemoveLock, Irp);

  return status;
}

// Determines whether the given file object was obtained by opening the volume
// itself as opposed to a specific file.
BOOLEAN
IsVolumeOpen(__in PDokanVCB Vcb, __in PFILE_OBJECT FileObject) {
  return FileObject != NULL && FileObject->FsContext == &Vcb->VolumeFileHeader;
}

NTSTATUS DokanGetVolumeMetrics(__in PIRP Irp, __in PDokanVCB Vcb) {
  VOLUME_METRICS* outputBuffer;
  if (!PREPARE_OUTPUT(Irp, outputBuffer, /*SetInformationOnFailure=*/TRUE)) {
    return STATUS_BUFFER_TOO_SMALL;
  }
  DokanVCBLockRO(Vcb);
  *outputBuffer = Vcb->VolumeMetrics;
  DokanVCBUnlock(Vcb);
  return STATUS_SUCCESS;
}

NTSTATUS
DokanDispatchDeviceControl(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp)

/*++

Routine Description:

        This device control dispatcher handles IOCTLs.

Arguments:

        DeviceObject - Context for the activity.
        Irp          - The device control argument block.

Return Value:

        NTSTATUS

--*/

{
  PDokanVCB vcb;
  PDokanDCB dcb;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  ULONG controlCode = 0;
  // {DCA0E0A5-D2CA-4f0f-8416-A6414657A77A}
  // GUID dokanGUID = { 0xdca0e0a5, 0xd2ca, 0x4f0f, { 0x84, 0x16, 0xa6, 0x41,
  // 0x46, 0x57, 0xa7, 0x7a } };

  __try {
    Irp->IoStatus.Information = 0;

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    controlCode = irpSp->Parameters.DeviceIoControl.IoControlCode;

    if (controlCode != IOCTL_EVENT_WAIT && controlCode != IOCTL_EVENT_INFO &&
        controlCode != IOCTL_KEEPALIVE) {

      DDbgPrint("==> DokanDispatchIoControl\n");
      DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
      DDbgPrint("  IoControlCode: %lx\n", controlCode);
    }

    if (DeviceObject->DriverObject == NULL ||
        DeviceObject->ReferenceCount < 0) {
      status = STATUS_DEVICE_DOES_NOT_EXIST;
      __leave;
    }

    vcb = DeviceObject->DeviceExtension;
    PrintIdType(vcb);
    if (GetIdentifierType(vcb) == DGL) {
      status = GlobalDeviceControl(DeviceObject, Irp);
      __leave;
    } else if (GetIdentifierType(vcb) == DCB) {
      status = DiskDeviceControlWithLock(DeviceObject, Irp);
      __leave;
    } else if (GetIdentifierType(vcb) != VCB) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }
    dcb = vcb->Dcb;

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_EVENT_WAIT:
      DDbgPrint("  IOCTL_EVENT_WAIT\n");
      status = DokanRegisterPendingIrpForEvent(DeviceObject, Irp);
      break;

    case IOCTL_EVENT_INFO:
      // DDbgPrint("  IOCTL_EVENT_INFO\n");
      status = DokanCompleteIrp(DeviceObject, Irp);
      break;

    case IOCTL_EVENT_RELEASE:
      DDbgPrint("  IOCTL_EVENT_RELEASE\n");
      status = DokanEventRelease(DeviceObject, Irp);
      break;

    case IOCTL_EVENT_WRITE:
      DDbgPrint("  IOCTL_EVENT_WRITE\n");
      status = DokanEventWrite(DeviceObject, Irp);
      break;

    case IOCTL_GET_VOLUME_METRICS:
      status = DokanGetVolumeMetrics(Irp, vcb);
      break;

    case IOCTL_KEEPALIVE:
	  //Remove for Dokan 2.x.x
      DDbgPrint("  IOCTL_KEEPALIVE\n");
      if (IsFlagOn(vcb->Flags, VCB_MOUNTED)) {
        ExEnterCriticalRegionAndAcquireResourceExclusive(&dcb->Resource);
        DokanUpdateTimeout(&dcb->TickCount, DOKAN_KEEPALIVE_TIMEOUT_DEFAULT);
        ExReleaseResourceAndLeaveCriticalRegion(&dcb->Resource);
        status = STATUS_SUCCESS;
      } else {
        DDbgPrint(" device is not mounted\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
      }
      break;

    case IOCTL_RESET_TIMEOUT:
      status = DokanResetPendingIrpTimeout(DeviceObject, Irp);
      break;

    case IOCTL_GET_ACCESS_TOKEN:
      status = DokanGetAccessToken(DeviceObject, Irp);
      break;

    default: {
      ULONG baseCode = DEVICE_TYPE_FROM_CTL_CODE(
          irpSp->Parameters.DeviceIoControl.IoControlCode);
      status = STATUS_NOT_IMPLEMENTED;
      // In case of IOCTL_STORAGE_BASE or IOCTL_DISK_BASE OR
      // FILE_DEVICE_NETWORK_FILE_SYSTEM or MOUNTDEVCONTROLTYPE ioctl type, pass
      // to DiskDeviceControl to avoid code duplication
      // TODO: probably not the best way to pass down Irp...
      if (baseCode == IOCTL_STORAGE_BASE || baseCode == IOCTL_DISK_BASE ||
          baseCode == FILE_DEVICE_NETWORK_FILE_SYSTEM ||
          baseCode == MOUNTDEVCONTROLTYPE) {
        status = DiskDeviceControlWithLock(dcb->DeviceObject, Irp);
      }

      if (status == STATUS_NOT_IMPLEMENTED) {
        PrintUnknownDeviceIoctlCode(
            irpSp->Parameters.DeviceIoControl.IoControlCode);
      }

      // Device control functions are only supposed to work on a volume handle.
      // Some win32 functions, like GetVolumePathName, rely on these operations
      // failing for file/directory handles. On the other hand, dokan issues its
      // custom operations on non-volume handles, so we can't do this check at
      // the top.
      if (status == STATUS_NOT_IMPLEMENTED
          && !IsVolumeOpen(vcb, irpSp->FileObject)) {
        status = STATUS_INVALID_PARAMETER;
      }
    } break;
    } // switch IoControlCode

  } __finally {

    if (status != STATUS_PENDING) {
      if (IsDeletePending(DeviceObject)) {
        DDbgPrint("  DeviceObject is invalid, so prevent BSOD");
        status = STATUS_DEVICE_REMOVED;
      }
      DokanCompleteIrpRequest(Irp, status, Irp->IoStatus.Information);
    }

    if (controlCode != IOCTL_EVENT_WAIT && controlCode != IOCTL_EVENT_INFO &&
        controlCode != IOCTL_KEEPALIVE) {

      DokanPrintNTStatus(status);
      DDbgPrint("<== DokanDispatchIoControl\n");
    }
  }

  return status;
}
