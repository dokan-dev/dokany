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


#include "dokani.h"
#include "fileinfo.h"


int DokanGetDiskFreeSpace(
	PULONGLONG			FreeBytesAvailable,
	PULONGLONG			TotalNumberOfBytes,
	PULONGLONG			TotalNumberOfFreeBytes,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	*FreeBytesAvailable = 512*1024*1024;
	*TotalNumberOfBytes = 1024*1024*1024;
	*TotalNumberOfFreeBytes = 512*1024*1024;
	
	return 0;
}


int DokanGetVolumeInformation(
	LPWSTR		VolumeNameBuffer,
	DWORD		VolumeNameSize,
	LPDWORD		VolumeSerialNumber,
	LPDWORD		MaximumComponentLength,
	LPDWORD		FileSystemFlags,
	LPWSTR		FileSystemNameBuffer,
	DWORD		FileSystemNameSize,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	wcscpy_s(VolumeNameBuffer, VolumeNameSize / sizeof(WCHAR), L"DOKAN");
	*VolumeSerialNumber = 0x19831116;
	*MaximumComponentLength = 256;
	*FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | 
						FILE_CASE_PRESERVED_NAMES | 
						FILE_SUPPORTS_REMOTE_STORAGE |
						FILE_UNICODE_ON_DISK;

	wcscpy_s(FileSystemNameBuffer, FileSystemNameSize / sizeof(WCHAR), L"Dokan");

	return 0;
}



ULONG
DokanFsVolumeInformation(
	PEVENT_INFORMATION	EventInfo,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_FILE_INFO	FileInfo,
	PDOKAN_OPERATIONS	DokanOperations)
{
	WCHAR	volumeName[MAX_PATH];
	DWORD	volumeSerial;
	DWORD	maxComLength;
	DWORD	fsFlags;
	WCHAR	fsName[MAX_PATH];
	ULONG	remainingLength;
	ULONG	bytesToCopy;

	int		status = -1;

	PFILE_FS_VOLUME_INFORMATION volumeInfo = 
		(PFILE_FS_VOLUME_INFORMATION)EventInfo->Buffer;

	
	if (!DokanOperations->GetVolumeInformation) {
		//return STATUS_NOT_IMPLEMENTED;
		DokanOperations->GetVolumeInformation = DokanGetVolumeInformation;
	}

	remainingLength = EventContext->Volume.BufferLength;

	if (remainingLength < sizeof(FILE_FS_VOLUME_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}


	RtlZeroMemory(volumeName, sizeof(volumeName));
	RtlZeroMemory(fsName, sizeof(fsName));

	status = DokanOperations->GetVolumeInformation(
				volumeName,							// VolumeNameBuffer
				sizeof(volumeName) / sizeof(WCHAR), // VolumeNameSize
				&volumeSerial,						// VolumeSerialNumber
				&maxComLength,						// MaximumComponentLength
				&fsFlags,							// FileSystemFlags
				fsName,								// FileSystemNameBuffer
				sizeof(fsName)  / sizeof(WCHAR),	// FileSystemNameSize
				FileInfo);

	if (status < 0) {
		return STATUS_INVALID_PARAMETER;
	}


	volumeInfo->VolumeCreationTime.QuadPart = 0;
	volumeInfo->VolumeSerialNumber = volumeSerial;
	volumeInfo->SupportsObjects = FALSE;
	
	remainingLength -= FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel[0]);
	
	bytesToCopy = wcslen(volumeName) * sizeof(WCHAR);
	if (remainingLength < bytesToCopy) {
		bytesToCopy = remainingLength;
	}

	volumeInfo->VolumeLabelLength = bytesToCopy;
	RtlCopyMemory(volumeInfo->VolumeLabel, volumeName, bytesToCopy);
	remainingLength -= bytesToCopy;

	EventInfo->BufferLength = EventContext->Volume.BufferLength - remainingLength;
	
	return STATUS_SUCCESS;
}


ULONG
DokanFsSizeInformation(
	PEVENT_INFORMATION	EventInfo,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_FILE_INFO	FileInfo,
	PDOKAN_OPERATIONS	DokanOperations)
{
	ULONGLONG	freeBytesAvailable = 0;
	ULONGLONG	totalBytes = 0;
	ULONGLONG	freeBytes = 0;
	
	int			status = -1;

	PFILE_FS_SIZE_INFORMATION sizeInfo = (PFILE_FS_SIZE_INFORMATION)EventInfo->Buffer;

	
	if (!DokanOperations->GetDiskFreeSpace) {
		//return STATUS_NOT_IMPLEMENTED;
		DokanOperations->GetDiskFreeSpace = DokanGetDiskFreeSpace;
	}

	if (EventContext->Volume.BufferLength < sizeof(FILE_FS_SIZE_INFORMATION) ) {
		return STATUS_BUFFER_OVERFLOW;
	}

	status = DokanOperations->GetDiskFreeSpace(
		&freeBytesAvailable, // FreeBytesAvailable
		&totalBytes, // TotalNumberOfBytes
		&freeBytes, // TotalNumberOfFreeBytes
		FileInfo);

	if (status < 0) {
		return STATUS_INVALID_PARAMETER;
	}

	sizeInfo->TotalAllocationUnits.QuadPart		= totalBytes / DOKAN_ALLOCATION_UNIT_SIZE;
	sizeInfo->AvailableAllocationUnits.QuadPart	= freeBytesAvailable / DOKAN_ALLOCATION_UNIT_SIZE;
	sizeInfo->SectorsPerAllocationUnit			= DOKAN_ALLOCATION_UNIT_SIZE / DOKAN_SECTOR_SIZE;
	sizeInfo->BytesPerSector					= DOKAN_SECTOR_SIZE;

	EventInfo->BufferLength = sizeof(FILE_FS_SIZE_INFORMATION);

	return STATUS_SUCCESS;
}


ULONG
DokanFsAttributeInformation(
	PEVENT_INFORMATION	EventInfo,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_FILE_INFO	FileInfo,
	PDOKAN_OPERATIONS	DokanOperations)
{
	WCHAR	volumeName[MAX_PATH];
	DWORD	volumeSerial;
	DWORD	maxComLength;
	DWORD	fsFlags;
	WCHAR	fsName[MAX_PATH];
	ULONG	remainingLength;
	ULONG	bytesToCopy;

	int		status = -1;

	PFILE_FS_ATTRIBUTE_INFORMATION attrInfo = 
		(PFILE_FS_ATTRIBUTE_INFORMATION)EventInfo->Buffer;
	
	if (!DokanOperations->GetVolumeInformation) {
		DokanOperations->GetVolumeInformation = DokanGetVolumeInformation;
		//return STATUS_NOT_IMPLEMENTED;
	}

	remainingLength = EventContext->Volume.BufferLength;

	if (remainingLength < sizeof(FILE_FS_ATTRIBUTE_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}


	RtlZeroMemory(volumeName, sizeof(volumeName));
	RtlZeroMemory(fsName, sizeof(fsName));

	status = DokanOperations->GetVolumeInformation(
				volumeName,			// VolumeNameBuffer
				sizeof(volumeName),	// VolumeNameSize
				&volumeSerial,		// VolumeSerialNumber
				&maxComLength,		// MaximumComponentLength
				&fsFlags,			// FileSystemFlags
				fsName,				// FileSystemNameBuffer
				sizeof(fsName),		// FileSystemNameSize
				FileInfo);

	if (status < 0) {
		return STATUS_INVALID_PARAMETER;
	}


	attrInfo->FileSystemAttributes = fsFlags;
	attrInfo->MaximumComponentNameLength = maxComLength;

	remainingLength -= FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName[0]);
	
	bytesToCopy = wcslen(fsName) * sizeof(WCHAR);
	if (remainingLength < bytesToCopy) {
		bytesToCopy = remainingLength;
	}

	attrInfo->FileSystemNameLength = bytesToCopy;
	RtlCopyMemory(attrInfo->FileSystemName, fsName, bytesToCopy);
	remainingLength -= bytesToCopy;

	EventInfo->BufferLength = EventContext->Volume.BufferLength - remainingLength;
	
	return STATUS_SUCCESS;
}


ULONG
DokanFsFullSizeInformation(
	PEVENT_INFORMATION	EventInfo,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_FILE_INFO	FileInfo,
	PDOKAN_OPERATIONS	DokanOperations)
{
	ULONGLONG	freeBytesAvailable = 0;
	ULONGLONG	totalBytes = 0;
	ULONGLONG	freeBytes = 0;
	
	int			status = -1;

	PFILE_FS_FULL_SIZE_INFORMATION sizeInfo = (PFILE_FS_FULL_SIZE_INFORMATION)EventInfo->Buffer;

	
	if (!DokanOperations->GetDiskFreeSpace) {
		DokanOperations->GetDiskFreeSpace = DokanGetDiskFreeSpace;
		//return STATUS_NOT_IMPLEMENTED;
	}

	if (EventContext->Volume.BufferLength < sizeof(FILE_FS_FULL_SIZE_INFORMATION) ) {
		return STATUS_BUFFER_OVERFLOW;
	}

	status = DokanOperations->GetDiskFreeSpace(
		&freeBytesAvailable, // FreeBytesAvailable
		&totalBytes, // TotalNumberOfBytes
		&freeBytes, // TotalNumberOfFreeBytes
		FileInfo);

	if (status < 0) {
		return STATUS_INVALID_PARAMETER;
	}

	sizeInfo->TotalAllocationUnits.QuadPart		= totalBytes / DOKAN_ALLOCATION_UNIT_SIZE;
	sizeInfo->ActualAvailableAllocationUnits.QuadPart = freeBytes / DOKAN_ALLOCATION_UNIT_SIZE;
	sizeInfo->CallerAvailableAllocationUnits.QuadPart = freeBytesAvailable / DOKAN_ALLOCATION_UNIT_SIZE;
	sizeInfo->SectorsPerAllocationUnit			= DOKAN_ALLOCATION_UNIT_SIZE / DOKAN_SECTOR_SIZE;
	sizeInfo->BytesPerSector					= DOKAN_SECTOR_SIZE;

	EventInfo->BufferLength = sizeof(FILE_FS_FULL_SIZE_INFORMATION);

	return STATUS_SUCCESS;
}


VOID
DispatchQueryVolumeInformation(
	HANDLE				Handle,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_INSTANCE		DokanInstance)
{
	PEVENT_INFORMATION		eventInfo;
	DOKAN_FILE_INFO			fileInfo;
	PDOKAN_OPEN_INFO		openInfo;
	int						status = -1;
	ULONG					sizeOfEventInfo = sizeof(EVENT_INFORMATION)
								- 8 + EventContext->Volume.BufferLength;

	eventInfo = (PEVENT_INFORMATION)malloc(sizeOfEventInfo);

	RtlZeroMemory(eventInfo, sizeOfEventInfo);
	RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));

	// There is no Context because file is not opened
	// so DispatchCommon is not used here
	openInfo = (PDOKAN_OPEN_INFO)EventContext->Context;
	
	eventInfo->BufferLength = 0;
	eventInfo->SerialNumber = EventContext->SerialNumber;

	fileInfo.ProcessId = EventContext->ProcessId;
	fileInfo.DokanOptions = DokanInstance->DokanOptions;

	eventInfo->Status = STATUS_NOT_IMPLEMENTED;
	eventInfo->BufferLength = 0;

	DbgPrint("###QueryVolumeInfo %04d\n", openInfo ? openInfo->EventId : -1);

	switch (EventContext->Volume.FsInformationClass) {
	case FileFsVolumeInformation:
		eventInfo->Status = DokanFsVolumeInformation(
								eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
		break;
	case FileFsSizeInformation:
		eventInfo->Status = DokanFsSizeInformation(
								eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
		break;
	case FileFsAttributeInformation:
		eventInfo->Status = DokanFsAttributeInformation(
								eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
		break;
	case FileFsFullSizeInformation:
		eventInfo->Status = DokanFsFullSizeInformation(
								eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
		break;
	default:
		DbgPrint("error unknown volume info %d\n", EventContext->Volume.FsInformationClass);
	}

	SendEventInformation(Handle, eventInfo, sizeOfEventInfo, NULL);
	free(eventInfo);
	return;
}
