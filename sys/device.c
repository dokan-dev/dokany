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

#include <mountdev.h>
#include <mountmgr.h>
#include <ntddvol.h>


VOID
PrintUnknownDeviceIoctlCode(
	__in ULONG	IoctlCode
	)
{
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
	}
	DDbgPrint("   BaseCode: 0x%x(%s) FunctionCode 0x%x(%d)\n",
		baseCode, baseCodeStr, functionCode, functionCode);
}


NTSTATUS
GlobalDeviceControl(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)
{
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status = STATUS_NOT_IMPLEMENTED;
	
	DDbgPrint("   => DokanGlobalDeviceControl\n");
	irpSp = IoGetCurrentIrpStackLocation(Irp);

	switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_EVENT_START:
		DDbgPrint("  IOCTL_EVENT_START\n");
		status = DokanEventStart(DeviceObject, Irp);
		break;
	case IOCTL_SERVICE_WAIT:
		status = DokanRegisterPendingIrpForService(DeviceObject, Irp);
		break;
	case IOCTL_SET_DEBUG_MODE:
		{
			if (irpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG)) {
				g_Debug = *(ULONG*)Irp->AssociatedIrp.SystemBuffer;
				status = STATUS_SUCCESS;
			}
			DDbgPrint("  IOCTL_SET_DEBUG_MODE: %d\n", g_Debug);
		}
		break;
	case IOCTL_TEST:
		if (irpSp->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG)) {
			*(ULONG*)Irp->AssociatedIrp.SystemBuffer = DOKAN_DRIVER_VERSION;
			Irp->IoStatus.Information = sizeof(ULONG);
			status = STATUS_SUCCESS;
			break;
		}
	default:
		PrintUnknownDeviceIoctlCode(irpSp->Parameters.DeviceIoControl.IoControlCode);
		status = STATUS_INVALID_PARAMETER;
		break;
	}

	DDbgPrint("   <= DokanGlobalDeviceControl\n");
	return status;
}


NTSTATUS
DiskDeviceControl(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)
{
	PIO_STACK_LOCATION	irpSp;
	PDokanDCB			dcb;
	NTSTATUS			status = STATUS_NOT_IMPLEMENTED;
	ULONG				outputLength = 0;
	
	DDbgPrint("   => DokanDiskDeviceControl\n");
	irpSp = IoGetCurrentIrpStackLocation(Irp);
	dcb = DeviceObject->DeviceExtension;
	outputLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;

	switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_DISK_GET_DRIVE_GEOMETRY:
		{
			PDISK_GEOMETRY	diskGeometry;
			ULONG		    length;

			DDbgPrint("  IOCTL_DISK_GET_DRIVE_GEOMETRY\n");
			if (outputLength < sizeof(DISK_GEOMETRY)) {
				status = STATUS_BUFFER_TOO_SMALL;
				Irp->IoStatus.Information = 0;
				break;
			}

			diskGeometry = (PDISK_GEOMETRY)Irp->AssociatedIrp.SystemBuffer;
			ASSERT(diskGeometry != NULL);

			length = 1024*1024*1024;
			diskGeometry->Cylinders.QuadPart = length / DOKAN_SECTOR_SIZE / 32 / 2;
			diskGeometry->MediaType = FixedMedia;
			diskGeometry->TracksPerCylinder = 2;
			diskGeometry->SectorsPerTrack = 32;
			diskGeometry->BytesPerSector = DOKAN_SECTOR_SIZE;

			status = STATUS_SUCCESS;
			Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
		}
		break;

	case IOCTL_DISK_GET_LENGTH_INFO:
		{
			PGET_LENGTH_INFORMATION getLengthInfo;

			DDbgPrint("  IOCTL_DISK_GET_LENGTH_INFO\n");
            
			if (outputLength < sizeof(GET_LENGTH_INFORMATION)) {
				status = STATUS_BUFFER_TOO_SMALL;
				Irp->IoStatus.Information = 0;
				break;
			}

			getLengthInfo = (PGET_LENGTH_INFORMATION) Irp->AssociatedIrp.SystemBuffer;
			ASSERT(getLengthInfo != NULL);

			getLengthInfo->Length.QuadPart = 1024*1024*500;
			status = STATUS_SUCCESS;
			Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);
		}
		break;

	case IOCTL_DISK_GET_PARTITION_INFO:
		DDbgPrint("  IOCTL_DISK_GET_PARTITION_INFO\n");
		break;

	case IOCTL_DISK_GET_PARTITION_INFO_EX:
		DDbgPrint("  IOCTL_DISK_GET_PARTITION_INFO_EX\n");
		break;

	case IOCTL_DISK_IS_WRITABLE:
		DDbgPrint("  IOCTL_DISK_IS_WRITABLE\n");
		status = STATUS_SUCCESS;
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

	case IOCTL_STORAGE_GET_HOTPLUG_INFO:
		{
			PSTORAGE_HOTPLUG_INFO hotplugInfo;
			DDbgPrint("  IOCTL_STORAGE_GET_HOTPLUG_INFO\n");
			if (outputLength < sizeof(STORAGE_HOTPLUG_INFO)) {
				status = STATUS_BUFFER_TOO_SMALL;
				Irp->IoStatus.Information = 0;
				break;
			}
			hotplugInfo = Irp->AssociatedIrp.SystemBuffer;
			hotplugInfo->Size =  sizeof(STORAGE_HOTPLUG_INFO);
			hotplugInfo->MediaRemovable = 1;
			hotplugInfo->MediaHotplug = 1;
			hotplugInfo->DeviceHotplug = 1;
			hotplugInfo->WriteCacheEnableOverride = 0;
			status = STATUS_SUCCESS;
			Irp->IoStatus.Information = sizeof(STORAGE_HOTPLUG_INFO);
		}
		break;
	case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
		{
			DDbgPrint("   IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
			status = STATUS_SUCCESS;
		}
		break;
	case IOCTL_DISK_CHECK_VERIFY:
		DDbgPrint("  IOCTL_DISK_CHECK_VERIFY\n");
		status = STATUS_SUCCESS;
		break;

	case IOCTL_STORAGE_CHECK_VERIFY:
		DDbgPrint("  IOCTL_STORAGE_CHECK_VERIFY\n");
		status = STATUS_SUCCESS;
		break;

	case IOCTL_STORAGE_CHECK_VERIFY2:
		DDbgPrint("  IOCTL_STORAGE_CHECK_VERIFY2\n");
		status = STATUS_SUCCESS;
		break;

	case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
		{
			PMOUNTDEV_NAME	mountdevName;
			ULONG			bufferLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
			PUNICODE_STRING	deviceName =  dcb->DiskDeviceName;
			
			DDbgPrint("   IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");

			if (bufferLength < sizeof(MOUNTDEV_NAME)) {
				status = STATUS_BUFFER_TOO_SMALL;
				Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
				break;
			}

			if (!dcb->Mounted) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			mountdevName = (PMOUNTDEV_NAME)Irp->AssociatedIrp.SystemBuffer;
			ASSERT(mountdevName != NULL);
			/* NOTE: When Windows API GetVolumeNameForVolumeMountPoint is called, this IO control is called.
			   Even if status = STATUS_SUCCESS, GetVolumeNameForVolumeMountPoint returns error.
			   Something is wrong..
			*/
			mountdevName->NameLength = deviceName->Length;

			if (sizeof(USHORT) + mountdevName->NameLength < bufferLength) {
				RtlCopyMemory((PCHAR)mountdevName->Name,
								deviceName->Buffer,
								mountdevName->NameLength);
				Irp->IoStatus.Information = sizeof(USHORT) + mountdevName->NameLength;
				status = STATUS_SUCCESS;
				DDbgPrint("  DeviceName %wZ\n", deviceName);
			} else {
				Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
				status = STATUS_BUFFER_OVERFLOW;
			}
		}
		break;
	case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:	
		{
			PMOUNTDEV_UNIQUE_ID uniqueId;
			ULONG				bufferLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;

			DDbgPrint("   IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
			if (bufferLength < sizeof(MOUNTDEV_UNIQUE_ID)) {
				status = STATUS_BUFFER_TOO_SMALL;
				Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
				break;
			}

			uniqueId = (PMOUNTDEV_UNIQUE_ID)Irp->AssociatedIrp.SystemBuffer;
			ASSERT(uniqueId != NULL);

			uniqueId->UniqueIdLength = dcb->SymbolicLinkName->Length;

			if (sizeof(USHORT) + uniqueId->UniqueIdLength < bufferLength) {
				RtlCopyMemory((PCHAR)uniqueId->UniqueId,  
								dcb->SymbolicLinkName->Buffer,
								uniqueId->UniqueIdLength);
				Irp->IoStatus.Information = FIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId[0]) +
											uniqueId->UniqueIdLength;
				status = STATUS_SUCCESS;
				DDbgPrint("  UniqueName %ws\n", uniqueId->UniqueId);
				break;
			} else {
				Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
				status = STATUS_BUFFER_OVERFLOW;
			}
		}
		break;
	case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
		DDbgPrint("   IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME\n");
		break;
	case IOCTL_MOUNTDEV_LINK_CREATED:
		{
			PMOUNTDEV_NAME	mountdevName = Irp->AssociatedIrp.SystemBuffer;
			DDbgPrint("   IOCTL_MOUNTDEV_LINK_CREATED\n");
			DDbgPrint("     Name: %ws\n", mountdevName->Name); 
			status = STATUS_SUCCESS;
		}
		break;
	case IOCTL_MOUNTDEV_LINK_DELETED:
		DDbgPrint("   IOCTL_MOUNTDEV_LINK_DELETED\n");
		status = STATUS_SUCCESS;
		break;
	//case IOCTL_MOUNTDEV_UNIQUE_ID_CHANGE_NOTIFY:
	//	DDbgPrint("   IOCTL_MOUNTDEV_UNIQUE_ID_CHANGE_NOTIFY\n");
	//	break;
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
	case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS:
		{
			PVOLUME_DISK_EXTENTS	volume;
			ULONG	bufferLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;

			DDbgPrint("   IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS\n");
			if (bufferLength < sizeof(VOLUME_DISK_EXTENTS)) {
				status =  STATUS_INVALID_PARAMETER;
				Irp->IoStatus.Information = 0;
				break;
			}
			volume = Irp->AssociatedIrp.SystemBuffer;
			RtlZeroMemory(volume, sizeof(VOLUME_DISK_EXTENTS));
			volume->NumberOfDiskExtents = 1;
			Irp->IoStatus.Information = sizeof(VOLUME_DISK_EXTENTS);
			status = STATUS_SUCCESS;
		}
		break;
	case IOCTL_STORAGE_EJECT_MEDIA:
		{
			DDbgPrint("   IOCTL_STORAGE_EJECT_MEDIA\n");
			DokanUnmount(dcb);				
			status = STATUS_SUCCESS;
		}
		break;
	case IOCTL_REDIR_QUERY_PATH:
		{
			DDbgPrint("  IOCTL_REDIR_QUERY_PATH\n");
		}
		break;


	default:
		PrintUnknownDeviceIoctlCode(irpSp->Parameters.DeviceIoControl.IoControlCode);
		status = STATUS_NOT_IMPLEMENTED;
		break;
	}
	DDbgPrint("   <= DokanDiskDeviceControl\n");
	return status;
}


NTSTATUS
DokanDispatchDeviceControl(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)

/*++

Routine Description:

	This device control dispatcher handles IOCTLs.

Arguments:

	DeviceObject - Context for the activity.
	Irp 		 - The device control argument block.

Return Value:

	NTSTATUS

--*/

{
	PDokanVCB			vcb;
	PDokanDCB			dcb;
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status = STATUS_NOT_IMPLEMENTED;
	ULONG				controlCode;
	// {DCA0E0A5-D2CA-4f0f-8416-A6414657A77A}
	GUID dokanGUID = 
		{ 0xdca0e0a5, 0xd2ca, 0x4f0f, { 0x84, 0x16, 0xa6, 0x41, 0x46, 0x57, 0xa7, 0x7a } };


	__try {
		FsRtlEnterFileSystem();

		Irp->IoStatus.Information = 0;

		irpSp = IoGetCurrentIrpStackLocation(Irp);

		controlCode = irpSp->Parameters.DeviceIoControl.IoControlCode;
	
		if (controlCode != IOCTL_EVENT_WAIT &&
			controlCode != IOCTL_EVENT_INFO &&
			controlCode != IOCTL_KEEPALIVE) {

			DDbgPrint("==> DokanDispatchIoControl\n");
			DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
		}

		vcb = DeviceObject->DeviceExtension;
		if (GetIdentifierType(vcb) == DGL) {
			status = GlobalDeviceControl(DeviceObject, Irp);
			__leave;
		} else if (GetIdentifierType(vcb) == DCB) {
			status = DiskDeviceControl(DeviceObject, Irp);
			__leave;
		} else if (GetIdentifierType(vcb) != VCB) {
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}
		dcb = vcb->Dcb;

		switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
		case IOCTL_EVENT_WAIT:
			//DDbgPrint("  IOCTL_EVENT_WAIT\n");
			status = DokanRegisterPendingIrpForEvent(DeviceObject, Irp);
			break;

		case IOCTL_EVENT_INFO:
			//DDbgPrint("  IOCTL_EVENT_INFO\n");
			status = DokanCompleteIrp(DeviceObject, Irp);
			break;

		case IOCTL_EVENT_RELEASE:
			DDbgPrint("  IOCTL_EVENT_RELEASE\n");
			status = DokanEventRelease(DeviceObject);
			break;

		case IOCTL_EVENT_WRITE:
			DDbgPrint("  IOCTL_EVENT_WRITE\n");
			status = DokanEventWrite(DeviceObject, Irp);
			break;

		case IOCTL_KEEPALIVE:
			if (dcb->Mounted) {
				ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);
				DokanUpdateTimeout(&dcb->TickCount, DOKAN_KEEPALIVE_TIMEOUT);
				ExReleaseResourceLite(&dcb->Resource);
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

		default:
			{
				PrintUnknownDeviceIoctlCode(irpSp->Parameters.DeviceIoControl.IoControlCode);
				status = STATUS_NOT_IMPLEMENTED;
			}
			break;
		} // switch IoControlCode
	
	} __finally {

		if (status != STATUS_PENDING) {
			//
			// complete the Irp
			//
			Irp->IoStatus.Status = status;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
		}

		if (controlCode != IOCTL_EVENT_WAIT &&
			controlCode != IOCTL_EVENT_INFO &&
			controlCode != IOCTL_KEEPALIVE) {

			DokanPrintNTStatus(status);
			DDbgPrint("<== DokanDispatchIoControl\n");
		}

		FsRtlExitFileSystem();
	}

	return status;
}
