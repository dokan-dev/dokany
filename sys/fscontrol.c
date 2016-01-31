/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

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
#include <wdmsec.h>

NTSTATUS
DokanUserFsRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  PIO_STACK_LOCATION irpSp;

  UNREFERENCED_PARAMETER(DeviceObject);

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  switch (irpSp->Parameters.FileSystemControl.FsControlCode) {

  case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    DDbgPrint("    FSCTL_REQUEST_OPLOCK_LEVEL_1\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    DDbgPrint("    FSCTL_REQUEST_OPLOCK_LEVEL_2\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_REQUEST_BATCH_OPLOCK:
    DDbgPrint("    FSCTL_REQUEST_BATCH_OPLOCK\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    DDbgPrint("    FSCTL_OPLOCK_BREAK_ACKNOWLEDGE\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    DDbgPrint("    FSCTL_OPBATCH_ACK_CLOSE_PENDING\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_OPLOCK_BREAK_NOTIFY:
    DDbgPrint("    FSCTL_OPLOCK_BREAK_NOTIFY\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_OPLOCK_BREAK_ACK_NO_2:
    DDbgPrint("    FSCTL_OPLOCK_BREAK_ACK_NO_2\n");
    break;

  case FSCTL_REQUEST_FILTER_OPLOCK:
    DDbgPrint("    FSCTL_REQUEST_FILTER_OPLOCK\n");
    break;

#if (NTDDI_VERSION >= NTDDI_WIN7)
  case FSCTL_REQUEST_OPLOCK:
    DDbgPrint("    FSCTL_REQUEST_OPLOCK\n");
    PREQUEST_OPLOCK_INPUT_BUFFER InputBuffer = NULL;
    ULONG InputBufferLength;
    ULONG OutputBufferLength;
    PDokanFCB fcb;
    PDokanCCB ccb;
    PFILE_OBJECT fileObject;

    InputBufferLength = irpSp->Parameters.FileSystemControl.InputBufferLength;
    InputBuffer = (PREQUEST_OPLOCK_INPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    OutputBufferLength = irpSp->Parameters.FileSystemControl.OutputBufferLength;

    if ((InputBufferLength < sizeof(REQUEST_OPLOCK_INPUT_BUFFER)) ||
        (OutputBufferLength < sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER))) {
      DDbgPrint("    STATUS_BUFFER_TOO_SMALL\n");
      return STATUS_BUFFER_TOO_SMALL;
    }

    fileObject = irpSp->FileObject;
    DokanPrintFileName(fileObject);

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    status = FsRtlOplockFsctrl(
        &(fcb->Oplock), Irp,
        0); // Fake OpenCount - TODO: Manage shared/locked files
    break;
#endif

  case FSCTL_LOCK_VOLUME:
    DDbgPrint("    FSCTL_LOCK_VOLUME\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_UNLOCK_VOLUME:
    DDbgPrint("    FSCTL_UNLOCK_VOLUME\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_DISMOUNT_VOLUME:
    DDbgPrint("    FSCTL_DISMOUNT_VOLUME\n");
    break;

  case FSCTL_IS_VOLUME_MOUNTED:
    DDbgPrint("    FSCTL_IS_VOLUME_MOUNTED\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_IS_PATHNAME_VALID:
    DDbgPrint("    FSCTL_IS_PATHNAME_VALID\n");
    break;

  case FSCTL_MARK_VOLUME_DIRTY:
    DDbgPrint("    FSCTL_MARK_VOLUME_DIRTY\n");
    break;

  case FSCTL_QUERY_RETRIEVAL_POINTERS:
    DDbgPrint("    FSCTL_QUERY_RETRIEVAL_POINTERS\n");
    break;

  case FSCTL_GET_COMPRESSION:
    DDbgPrint("    FSCTL_GET_COMPRESSION\n");
    break;

  case FSCTL_SET_COMPRESSION:
    DDbgPrint("    FSCTL_SET_COMPRESSION\n");
    break;

  case FSCTL_MARK_AS_SYSTEM_HIVE:
    DDbgPrint("    FSCTL_MARK_AS_SYSTEM_HIVE\n");
    break;

  case FSCTL_INVALIDATE_VOLUMES:
    DDbgPrint("    FSCTL_INVALIDATE_VOLUMES\n");
    break;

  case FSCTL_QUERY_FAT_BPB:
    DDbgPrint("    FSCTL_QUERY_FAT_BPB\n");
    break;

  case FSCTL_FILESYSTEM_GET_STATISTICS:
    DDbgPrint("    FSCTL_FILESYSTEM_GET_STATISTICS\n");
    break;

  case FSCTL_GET_NTFS_VOLUME_DATA:
    DDbgPrint("    FSCTL_GET_NTFS_VOLUME_DATA\n");
    break;

  case FSCTL_GET_NTFS_FILE_RECORD:
    DDbgPrint("    FSCTL_GET_NTFS_FILE_RECORD\n");
    break;

  case FSCTL_GET_VOLUME_BITMAP:
    DDbgPrint("    FSCTL_GET_VOLUME_BITMAP\n");
    break;

  case FSCTL_GET_RETRIEVAL_POINTERS:
    DDbgPrint("    FSCTL_GET_RETRIEVAL_POINTERS\n");
    break;

  case FSCTL_MOVE_FILE:
    DDbgPrint("    FSCTL_MOVE_FILE\n");
    break;

  case FSCTL_IS_VOLUME_DIRTY:
    DDbgPrint("    FSCTL_IS_VOLUME_DIRTY\n");
    break;

  case FSCTL_ALLOW_EXTENDED_DASD_IO:
    DDbgPrint("    FSCTL_ALLOW_EXTENDED_DASD_IO\n");
    break;

  case FSCTL_FIND_FILES_BY_SID:
    DDbgPrint("    FSCTL_FIND_FILES_BY_SID\n");
    break;

  case FSCTL_SET_OBJECT_ID:
    DDbgPrint("    FSCTL_SET_OBJECT_ID\n");
    break;

  case FSCTL_GET_OBJECT_ID:
    DDbgPrint("    FSCTL_GET_OBJECT_ID\n");
    break;

  case FSCTL_DELETE_OBJECT_ID:
    DDbgPrint("    FSCTL_DELETE_OBJECT_ID\n");
    break;

  case FSCTL_SET_REPARSE_POINT:
    DDbgPrint("    FSCTL_SET_REPARSE_POINT\n");
    break;

  case FSCTL_GET_REPARSE_POINT:
    DDbgPrint("    FSCTL_GET_REPARSE_POINT\n");
    status = STATUS_NOT_A_REPARSE_POINT;
    break;

  case FSCTL_DELETE_REPARSE_POINT:
    DDbgPrint("    FSCTL_DELETE_REPARSE_POINT\n");
    break;

  case FSCTL_ENUM_USN_DATA:
    DDbgPrint("    FSCTL_ENUM_USN_DATA\n");
    break;

  case FSCTL_SECURITY_ID_CHECK:
    DDbgPrint("    FSCTL_SECURITY_ID_CHECK\n");
    break;

  case FSCTL_READ_USN_JOURNAL:
    DDbgPrint("    FSCTL_READ_USN_JOURNAL\n");
    break;

  case FSCTL_SET_OBJECT_ID_EXTENDED:
    DDbgPrint("    FSCTL_SET_OBJECT_ID_EXTENDED\n");
    break;

  case FSCTL_CREATE_OR_GET_OBJECT_ID:
    DDbgPrint("    FSCTL_CREATE_OR_GET_OBJECT_ID\n");
    break;

  case FSCTL_SET_SPARSE:
    DDbgPrint("    FSCTL_SET_SPARSE\n");
    break;

  case FSCTL_SET_ZERO_DATA:
    DDbgPrint("    FSCTL_SET_ZERO_DATA\n");
    break;

  case FSCTL_QUERY_ALLOCATED_RANGES:
    DDbgPrint("    FSCTL_QUERY_ALLOCATED_RANGES\n");
    break;

  case FSCTL_SET_ENCRYPTION:
    DDbgPrint("    FSCTL_SET_ENCRYPTION\n");
    break;

  case FSCTL_ENCRYPTION_FSCTL_IO:
    DDbgPrint("    FSCTL_ENCRYPTION_FSCTL_IO\n");
    break;

  case FSCTL_WRITE_RAW_ENCRYPTED:
    DDbgPrint("    FSCTL_WRITE_RAW_ENCRYPTED\n");
    break;

  case FSCTL_READ_RAW_ENCRYPTED:
    DDbgPrint("    FSCTL_READ_RAW_ENCRYPTED\n");
    break;

  case FSCTL_CREATE_USN_JOURNAL:
    DDbgPrint("    FSCTL_CREATE_USN_JOURNAL\n");
    break;

  case FSCTL_READ_FILE_USN_DATA:
    DDbgPrint("    FSCTL_READ_FILE_USN_DATA\n");
    break;

  case FSCTL_WRITE_USN_CLOSE_RECORD:
    DDbgPrint("    FSCTL_WRITE_USN_CLOSE_RECORD\n");
    break;

  case FSCTL_EXTEND_VOLUME:
    DDbgPrint("    FSCTL_EXTEND_VOLUME\n");
    break;

  case FSCTL_QUERY_USN_JOURNAL:
    DDbgPrint("    FSCTL_QUERY_USN_JOURNAL\n");
    break;

  case FSCTL_DELETE_USN_JOURNAL:
    DDbgPrint("    FSCTL_DELETE_USN_JOURNAL\n");
    break;

  case FSCTL_MARK_HANDLE:
    DDbgPrint("    FSCTL_MARK_HANDLE\n");
    break;

  case FSCTL_SIS_COPYFILE:
    DDbgPrint("    FSCTL_SIS_COPYFILE\n");
    break;

  case FSCTL_SIS_LINK_FILES:
    DDbgPrint("    FSCTL_SIS_LINK_FILES\n");
    break;

  case FSCTL_RECALL_FILE:
    DDbgPrint("    FSCTL_RECALL_FILE\n");
    break;

  case FSCTL_SET_ZERO_ON_DEALLOCATION:
    DDbgPrint("    FSCTL_SET_ZERO_ON_DEALLOCATION\n");
    break;

  case FSCTL_CSC_INTERNAL:
    DDbgPrint("    FSCTL_CSC_INTERNAL\n");
    break;
  case FSCTL_QUERY_ON_DISK_VOLUME_INFO:
	  DDbgPrint("    FSCTL_QUERY_ON_DISK_VOLUME_INFO\n");
	  break;
  default:
    DDbgPrint("    Unknown FSCTL %d\n",
              (irpSp->Parameters.FileSystemControl.FsControlCode >> 2) & 0xFFF);
    status = STATUS_INVALID_DEVICE_REQUEST;
  }

  return status;
}

NTSTATUS NotifyAllApps(__in PDEVICE_OBJECT VolumeDeviceObject) {
	PFILE_OBJECT VolumeFileObject = NULL;
	NTSTATUS status = STATUS_SUCCESS;
	try {
		VolumeFileObject = IoCreateStreamFileObjectLite(NULL, VolumeDeviceObject);
	} except(EXCEPTION_EXECUTE_HANDLER) {
		status = GetExceptionCode();
		DDbgPrint("   IoCreateStreamFileObjectLite failed with status code 0x%x\n", status);
	}

	if (NT_SUCCESS(status)) {
		// notify all application about volume mount
		status = FsRtlNotifyVolumeEvent(VolumeFileObject, FSRTL_VOLUME_MOUNT);
		ObDereferenceObject(VolumeFileObject);
		if (NT_SUCCESS(status)) {
			DDbgPrint("   FsRtlNotifyVolumeEvent has been successfully sent\n");
		}
		else {
			DDbgPrint("   FsRtlNotifyVolumeEvent failed with status code 0x%x\n", status);
		}
	}

	return status;
}

NTSTATUS DokanMountVolume(__in PDEVICE_OBJECT DiskDevice, __in PIRP Irp) {
	PDokanDCB dcb = NULL;
	PDokanVCB vcb = NULL;
	PVPB vpb = NULL;
	DOKAN_CONTROL dokanControl;
	PMOUNT_ENTRY mountEntry = NULL;
	PIO_STACK_LOCATION irpSp;
	PDEVICE_OBJECT volDeviceObject;
	PDRIVER_OBJECT DriverObject = DiskDevice->DriverObject;
	NTSTATUS status = STATUS_UNRECOGNIZED_VOLUME;
	
	irpSp = IoGetCurrentIrpStackLocation(Irp);
	dcb = irpSp->Parameters.MountVolume.DeviceObject->DeviceExtension;
	if (!dcb) {
		DDbgPrint("   Not DokanDiskDevice (no device extension)\n");
		return status;
	}
	PrintIdType(dcb);
	if (GetIdentifierType(dcb) != DCB) {
		DDbgPrint("   Not DokanDiskDevice\n");
		return status;
	}
	BOOLEAN isNetworkFileSystem = (dcb->VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);

	if (!isNetworkFileSystem) {
		status = IoCreateDevice(DriverObject,          // DriverObject
			sizeof(DokanVCB),      // DeviceExtensionSize
			NULL,                  // DeviceName
			dcb->VolumeDeviceType,            // DeviceType
			dcb->DeviceCharacteristics, // DeviceCharacteristics
			FALSE,                 // Not Exclusive
			&volDeviceObject);     // DeviceObject
	}
	else {
		status =
			IoCreateDeviceSecure(DriverObject,          // DriverObject
				sizeof(DokanVCB),      // DeviceExtensionSize
				dcb->DiskDeviceName,       // DeviceName
				dcb->VolumeDeviceType,            // DeviceType
				dcb->DeviceCharacteristics, // DeviceCharacteristics
				FALSE,                 // Not Exclusive
				&sddl,                 // Default SDDL String
				NULL,                  // Device Class GUID
				&volDeviceObject);     // DeviceObject
	}

	if (!NT_SUCCESS(status)) {
		DDbgPrint("  IoCreateDevice failed: 0x%x\n", status);
		return status;
	}

	vcb = volDeviceObject->DeviceExtension;
	vcb->Identifier.Type = VCB;
	vcb->Identifier.Size = sizeof(DokanVCB);

	vcb->DeviceObject = volDeviceObject;
	vcb->Dcb = dcb;
	dcb->Vcb = vcb;

	InitializeListHead(&vcb->NextFCB);

	InitializeListHead(&vcb->DirNotifyList);
	FsRtlNotifyInitializeSync(&vcb->NotifySync);

	ExInitializeFastMutex(&vcb->AdvancedFCBHeaderMutex);

#if _WIN32_WINNT >= 0x0501
	FsRtlSetupAdvancedHeader(&vcb->VolumeFileHeader,
		&vcb->AdvancedFCBHeaderMutex);
#else
	if (DokanFsRtlTeardownPerStreamContexts) {
		FsRtlSetupAdvancedHeader(&vcb->VolumeFileHeader,
			&vcb->AdvancedFCBHeaderMutex);
	}
#endif

	vpb = irpSp->Parameters.MountVolume.Vpb;
	DokanInitVpb(vpb, vcb->DeviceObject);

	//
	// Establish user-buffer access method.
	//
	volDeviceObject->Flags |= DO_DIRECT_IO;

	volDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	dcb->Mounted = 1;
	ObReferenceObject(volDeviceObject);

	// set the device on dokanControl
	RtlZeroMemory(&dokanControl, sizeof(dokanControl));
	RtlCopyMemory(dokanControl.DeviceName, dcb->DiskDeviceName->Buffer,	dcb->DiskDeviceName->Length);
	mountEntry = FindMountEntry(dcb->Global, &dokanControl);
	if (mountEntry != NULL) {
		mountEntry->MountControl.DeviceObject = volDeviceObject;
	}
	else {
		DDbgPrint("MountEntry not found. This way the dokanControl does not have the DeviceObject")
	}

	// Start check thread
	DokanStartCheckThread(dcb);
	ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);
	DokanUpdateTimeout(&dcb->TickCount, DOKAN_KEEPALIVE_TIMEOUT * 3);
	ExReleaseResourceLite(&dcb->Resource);

	// Create mount point for the volume
	if (dcb->UseMountManager) {
		status = DokanSendVolumeArrivalNotification(dcb->DiskDeviceName);
		if (!NT_SUCCESS(status)) {
			DDbgPrint("  DokanSendVolumeArrivalNotification failed: 0x%x\n", status);
		}
	}
	DokanCreateMountPoint(dcb);

	return STATUS_SUCCESS;
}

VOID DokanInitVpb(__in PVPB Vpb, __in PDEVICE_OBJECT VolumeDevice) {
  if (Vpb != NULL) {
    Vpb->DeviceObject = VolumeDevice;
    Vpb->VolumeLabelLength = (USHORT)wcslen(VOLUME_LABEL) * sizeof(WCHAR);
    RtlStringCchCopyW(Vpb->VolumeLabel,
                      sizeof(Vpb->VolumeLabel) / sizeof(WCHAR), VOLUME_LABEL);
    Vpb->SerialNumber = 0x19831116;
  }
}

NTSTATUS
DokanDispatchFileSystemControl(__in PDEVICE_OBJECT DeviceObject,
                               __in PIRP Irp) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PIO_STACK_LOCATION irpSp;

  __try {
    DDbgPrint("==> DokanFileSystemControl\n");
    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (irpSp->MinorFunction) {
    case IRP_MN_KERNEL_CALL:
      DDbgPrint("	 IRP_MN_KERNEL_CALL\n");
      break;

    case IRP_MN_LOAD_FILE_SYSTEM:
      DDbgPrint("	 IRP_MN_LOAD_FILE_SYSTEM\n");
      break;

    case IRP_MN_MOUNT_VOLUME: {
	  DDbgPrint("	 IRP_MN_MOUNT_VOLUME\n");
	  status = DokanMountVolume(DeviceObject, Irp);
    } break;

    case IRP_MN_USER_FS_REQUEST:
      DDbgPrint("	 IRP_MN_USER_FS_REQUEST\n");
      status = DokanUserFsRequest(DeviceObject, Irp);
      break;

    case IRP_MN_VERIFY_VOLUME:
      DDbgPrint("	 IRP_MN_VERIFY_VOLUME\n");
      break;

    default:
      DDbgPrint("  unknown %d\n", irpSp->MinorFunction);
      status = STATUS_INVALID_PARAMETER;
      break;
    }

  } __finally {

    DokanCompleteIrpRequest(Irp, status, 0);

    DDbgPrint("<== DokanFileSystemControl\n");
  }

  return status;
}
