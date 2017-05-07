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

#include "dokani.h"
#include "fileinfo.h"
#include <assert.h>

NTSTATUS DOKAN_CALLBACK DokanGetDiskFreeSpace(DOKAN_GET_DISK_FREE_SPACE_EVENT *EventInfo) {

	EventInfo->FreeBytesAvailable = 0;
	EventInfo->TotalNumberOfBytes = 1024 * 1024 * 1024;
	EventInfo->TotalNumberOfFreeBytes = 0;
	
	return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK DokanGetVolumeInformation(DOKAN_GET_VOLUME_INFO_EVENT *EventInfo) {

  wcscpy_s(EventInfo->VolumeInfo->VolumeLabel, EventInfo->MaxLabelLengthInChars, L"DOKAN");
  EventInfo->VolumeInfo->VolumeLabelLength = (ULONG)(wcslen(EventInfo->VolumeInfo->VolumeLabel) * sizeof(WCHAR));

  EventInfo->VolumeInfo->VolumeSerialNumber = 0x19831116;

  return STATUS_SUCCESS;
}

void DokanFsVolumeInformation(DOKAN_IO_EVENT *EventInfo) {

  DOKAN_GET_VOLUME_INFO_EVENT *getVolumeInfo = &EventInfo->EventInfo.GetVolumeInfo;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  assert((void*)getVolumeInfo == (void*)EventInfo);

  if (EventInfo->KernelInfo.EventContext.Operation.Volume.BufferLength < sizeof(FILE_FS_VOLUME_INFORMATION)) {
    
	  DokanEndDispatchGetVolumeInfo(getVolumeInfo, STATUS_BUFFER_OVERFLOW);

	  return;
  }

  getVolumeInfo->DokanFileInfo = &EventInfo->DokanFileInfo;
  getVolumeInfo->VolumeInfo = (PFILE_FS_VOLUME_INFORMATION)&EventInfo->EventResult->Buffer[0];
  getVolumeInfo->MaxLabelLengthInChars =
	  (EventInfo->KernelInfo.EventContext.Operation.Volume.BufferLength - offsetof(FILE_FS_VOLUME_INFORMATION, VolumeLabel)) / sizeof(WCHAR);

  if (EventInfo->DokanInstance->DokanOperations->GetVolumeInformationW) {

    status = EventInfo->DokanInstance->DokanOperations->GetVolumeInformationW(getVolumeInfo);
  }

  if(status != STATUS_PENDING) {

	  DokanEndDispatchGetVolumeInfo(getVolumeInfo, status);
  }
}

void DOKANAPI DokanEndDispatchGetVolumeInfo(DOKAN_GET_VOLUME_INFO_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result = ioEvent->EventResult;
	PFILE_FS_VOLUME_INFORMATION volumeInfo = (PFILE_FS_VOLUME_INFORMATION)result->Buffer;

	assert(result->BufferLength == 0);

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchGetVolumeInfo() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	if(ResultStatus == STATUS_NOT_IMPLEMENTED) {

		ResultStatus = DokanGetVolumeInformation(EventInfo);
	}

	if(ResultStatus == STATUS_SUCCESS) {

		if(volumeInfo->VolumeLabelLength > EventInfo->MaxLabelLengthInChars) {

			DbgPrint("Dokan Error: DokanEndDispatchGetVolumeInfo() failed because VolumeLabelLength is greater than MaxLabelLengthInChars.\n");
			ResultStatus = STATUS_BUFFER_OVERFLOW;
		}
		else {
			
			// convert from chars to bytes
			volumeInfo->VolumeLabelLength *= sizeof(WCHAR);
			volumeInfo->SupportsObjects = FALSE;
			
			result->BufferLength = offsetof(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + volumeInfo->VolumeLabelLength;
		}
	}

	result->Status = ResultStatus;

	SendIoEventResult(ioEvent);
}

void DokanFsSizeInformation(DOKAN_IO_EVENT *EventInfo) {

  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  DOKAN_GET_DISK_FREE_SPACE_EVENT *getFreeSpace = &EventInfo->EventInfo.GetVolumeFreeSpace;

  assert((void*)getFreeSpace == (void*)EventInfo);

  if (EventInfo->KernelInfo.EventContext.Operation.Volume.BufferLength < sizeof(FILE_FS_SIZE_INFORMATION)) {

	  DokanEndDispatchGetVolumeFreeSpace(getFreeSpace, STATUS_BUFFER_OVERFLOW);
	  return;
  }

  if (EventInfo->DokanInstance->DokanOperations->GetVolumeFreeSpace) {

	  getFreeSpace->DokanFileInfo = &EventInfo->DokanFileInfo;

	  status = EventInfo->DokanInstance->DokanOperations->GetVolumeFreeSpace(getFreeSpace);
  }

  if (status != STATUS_PENDING) {

	  DokanEndDispatchGetVolumeFreeSpace(getFreeSpace, status);
  }
}

void DOKANAPI DokanEndDispatchGetVolumeFreeSpace(DOKAN_GET_DISK_FREE_SPACE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result = ioEvent->EventResult;
	ULONG allocationUnitSize = ioEvent->DokanInstance->DokanOptions->AllocationUnitSize;
	ULONG sectorSize = ioEvent->DokanInstance->DokanOptions->SectorSize;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchGetVolumeInfo() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	if(ResultStatus == STATUS_NOT_IMPLEMENTED) {

		ResultStatus = DokanGetDiskFreeSpace(EventInfo);
	}

	if(ResultStatus == STATUS_SUCCESS) {

		if(ioEvent->KernelInfo.EventContext.Operation.Volume.FsInformationClass == FileFsSizeInformation) {

			PFILE_FS_SIZE_INFORMATION freeSpaceInfo = (PFILE_FS_SIZE_INFORMATION)result->Buffer;

			freeSpaceInfo->TotalAllocationUnits.QuadPart =
				EventInfo->TotalNumberOfBytes / allocationUnitSize;

			freeSpaceInfo->AvailableAllocationUnits.QuadPart =
				EventInfo->FreeBytesAvailable / allocationUnitSize;

			freeSpaceInfo->SectorsPerAllocationUnit =
				allocationUnitSize / sectorSize;

			freeSpaceInfo->BytesPerSector = sectorSize;

			result->BufferLength = sizeof(FILE_FS_SIZE_INFORMATION);
		}
		else if(ioEvent->KernelInfo.EventContext.Operation.Volume.FsInformationClass == FileFsFullSizeInformation) {

			PFILE_FS_FULL_SIZE_INFORMATION freeSpaceInfo = (PFILE_FS_FULL_SIZE_INFORMATION)result->Buffer;

			freeSpaceInfo->TotalAllocationUnits.QuadPart =
				EventInfo->TotalNumberOfBytes / allocationUnitSize;

			freeSpaceInfo->ActualAvailableAllocationUnits.QuadPart =
				EventInfo->TotalNumberOfFreeBytes / allocationUnitSize;

			freeSpaceInfo->CallerAvailableAllocationUnits.QuadPart =
				EventInfo->FreeBytesAvailable / allocationUnitSize;

			freeSpaceInfo->SectorsPerAllocationUnit =
				allocationUnitSize / sectorSize;

			freeSpaceInfo->BytesPerSector = sectorSize;

			result->BufferLength = sizeof(FILE_FS_FULL_SIZE_INFORMATION);
		}
		else {

			DbgPrint("Dokan Error: DokanEndDispatchGetVolumeFreeSpace() received an invalid FsInformationClass: 0x%x\n",
				ioEvent->KernelInfo.EventContext.Operation.Volume.FsInformationClass);

			ResultStatus = STATUS_INTERNAL_ERROR;
		}
	}

	result->Status = ResultStatus;

	SendIoEventResult(ioEvent);
}

void DokanFsAttributeInformation(DOKAN_IO_EVENT *EventInfo) {

  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  DOKAN_GET_VOLUME_ATTRIBUTES_EVENT *getAttributes = &EventInfo->EventInfo.GetVolumeAttributes;

  if (EventInfo->KernelInfo.EventContext.Operation.Volume.BufferLength < sizeof(FILE_FS_ATTRIBUTE_INFORMATION)) {

	  DokanEndDispatchGetVolumeAttributes(getAttributes, STATUS_BUFFER_OVERFLOW);
	  return;
  }

  if (EventInfo->DokanInstance->DokanOperations->GetVolumeAttributes) {

	  getAttributes->DokanFileInfo = &EventInfo->DokanFileInfo;
	  getAttributes->Attributes = (PFILE_FS_ATTRIBUTE_INFORMATION)&EventInfo->EventResult->Buffer[0];
	  getAttributes->MaxFileSystemNameLengthInChars =
		  (EventInfo->KernelInfo.EventContext.Operation.Volume.BufferLength - offsetof(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName)) / sizeof(WCHAR);

	  status = EventInfo->DokanInstance->DokanOperations->GetVolumeAttributes(getAttributes);
  }

  if (status != STATUS_PENDING) {
    
	  DokanEndDispatchGetVolumeAttributes(getAttributes, status);
  }
}

void DOKANAPI DokanEndDispatchGetVolumeAttributes(DOKAN_GET_VOLUME_ATTRIBUTES_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result = ioEvent->EventResult;
	PFILE_FS_ATTRIBUTE_INFORMATION freeSpaceInfo = (PFILE_FS_ATTRIBUTE_INFORMATION)result->Buffer;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchGetVolumeAttributes() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	if(ResultStatus == STATUS_NOT_IMPLEMENTED) {

		freeSpaceInfo->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
												FILE_SUPPORTS_REMOTE_STORAGE | FILE_UNICODE_ON_DISK;

		wcscpy_s(&freeSpaceInfo->FileSystemName[0], EventInfo->MaxFileSystemNameLengthInChars, L"DokanFS");

		freeSpaceInfo->FileSystemNameLength = (ULONG)wcslen(&freeSpaceInfo->FileSystemName[0]);
		freeSpaceInfo->MaximumComponentNameLength = 256;

		ResultStatus = STATUS_SUCCESS;
	}

	if(ResultStatus == STATUS_SUCCESS) {

		if(freeSpaceInfo->FileSystemNameLength > EventInfo->MaxFileSystemNameLengthInChars) {

			DbgPrint("Dokan Error: DokanEndDispatchGetVolumeAttributes() failed because FileSystemNameLength is greater than MaxFileSystemNameLengthInChars.\n");

			ResultStatus = STATUS_BUFFER_OVERFLOW;
		}
		else {

			freeSpaceInfo->FileSystemNameLength *= sizeof(WCHAR);
			result->BufferLength = offsetof(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + freeSpaceInfo->FileSystemNameLength;
		}
	}

	result->Status = ResultStatus;

	SendIoEventResult(ioEvent);
}

void DokanFsFullSizeInformation(DOKAN_IO_EVENT *EventInfo) {

	NTSTATUS status = STATUS_NOT_IMPLEMENTED;
	DOKAN_GET_DISK_FREE_SPACE_EVENT *getFreeSpace = &EventInfo->EventInfo.GetVolumeFreeSpace;

	assert((void*)getFreeSpace == (void*)EventInfo);

	if(EventInfo->KernelInfo.EventContext.Operation.Volume.BufferLength < sizeof(FILE_FS_FULL_SIZE_INFORMATION)) {

		DokanEndDispatchGetVolumeFreeSpace(getFreeSpace, STATUS_BUFFER_OVERFLOW);
		return;
	}

	if(EventInfo->DokanInstance->DokanOperations->GetVolumeFreeSpace) {

		getFreeSpace->DokanFileInfo = &EventInfo->DokanFileInfo;

		status = EventInfo->DokanInstance->DokanOperations->GetVolumeFreeSpace(getFreeSpace);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchGetVolumeFreeSpace(getFreeSpace, status);
	}
}

void BeginDispatchQueryVolumeInformation(DOKAN_IO_EVENT *EventInfo) {

  CreateDispatchCommon(EventInfo, EventInfo->KernelInfo.EventContext.Operation.Volume.BufferLength);

  DbgPrint("###QueryVolumeInfo file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
	  EventInfo);

  switch (EventInfo->KernelInfo.EventContext.Operation.Volume.FsInformationClass) {
  case FileFsVolumeInformation:
    DokanFsVolumeInformation(EventInfo);
    break;
  case FileFsSizeInformation:
    DokanFsSizeInformation(EventInfo);
    break;
  case FileFsAttributeInformation:
    DokanFsAttributeInformation(EventInfo);
    break;
  case FileFsFullSizeInformation:
    DokanFsFullSizeInformation(EventInfo);
    break;
  default:
    
	  DbgPrint("Dokan Error: Unknown volume info FsInformationClass 0x%x\n",
             EventInfo->KernelInfo.EventContext.Operation.Volume.FsInformationClass);

	  EventInfo->EventResult->Status = STATUS_NOT_IMPLEMENTED;

	  SendIoEventResult(EventInfo);

	  break;
  }
}
