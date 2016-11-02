/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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
#include <ntstatus.h>
#include <stdio.h>
#include <assert.h>

#define DOKAN_STREAM_ENTRY_ALIGNMENT		8

NTSTATUS
DokanFillFileBasicInfo(PFILE_BASIC_INFORMATION BasicInfo,
                       PBY_HANDLE_FILE_INFORMATION FileInfo,
                       PULONG RemainingLength) {

  if (*RemainingLength < sizeof(FILE_BASIC_INFORMATION)) {

    return STATUS_BUFFER_OVERFLOW;
  }

  BasicInfo->CreationTime.LowPart = FileInfo->ftCreationTime.dwLowDateTime;
  BasicInfo->CreationTime.HighPart = FileInfo->ftCreationTime.dwHighDateTime;
  BasicInfo->LastAccessTime.LowPart = FileInfo->ftLastAccessTime.dwLowDateTime;
  BasicInfo->LastAccessTime.HighPart =
      FileInfo->ftLastAccessTime.dwHighDateTime;
  BasicInfo->LastWriteTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  BasicInfo->LastWriteTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  BasicInfo->ChangeTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  BasicInfo->ChangeTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  BasicInfo->FileAttributes = FileInfo->dwFileAttributes;

  *RemainingLength -= sizeof(FILE_BASIC_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileStandardInfo(PFILE_STANDARD_INFORMATION StandardInfo,
                          PBY_HANDLE_FILE_INFORMATION FileInfo,
                          PULONG RemainingLength,
                          PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_STANDARD_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  StandardInfo->AllocationSize.HighPart = FileInfo->nFileSizeHigh;
  StandardInfo->AllocationSize.LowPart = FileInfo->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&StandardInfo->AllocationSize,
                        DokanInstance->DokanOptions);
  StandardInfo->EndOfFile.HighPart = FileInfo->nFileSizeHigh;
  StandardInfo->EndOfFile.LowPart = FileInfo->nFileSizeLow;
  StandardInfo->NumberOfLinks = FileInfo->nNumberOfLinks;
  StandardInfo->DeletePending = FALSE;
  StandardInfo->Directory = FALSE;

  if (FileInfo->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    StandardInfo->Directory = TRUE;
  }

  *RemainingLength -= sizeof(FILE_STANDARD_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFilePositionInfo(PFILE_POSITION_INFORMATION PosInfo,
                          PBY_HANDLE_FILE_INFORMATION FileInfo,
                          PULONG RemainingLength) {

  UNREFERENCED_PARAMETER(FileInfo);

  if (*RemainingLength < sizeof(FILE_POSITION_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  // this field is filled by driver
  PosInfo->CurrentByteOffset.QuadPart = 0; // fileObject->CurrentByteOffset;

  *RemainingLength -= sizeof(FILE_POSITION_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillInternalInfo(PFILE_INTERNAL_INFORMATION InternalInfo,
                      PBY_HANDLE_FILE_INFORMATION FileInfo,
                      PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_INTERNAL_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  InternalInfo->IndexNumber.HighPart = FileInfo->nFileIndexHigh;
  InternalInfo->IndexNumber.LowPart = FileInfo->nFileIndexLow;

  *RemainingLength -= sizeof(FILE_INTERNAL_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileAllInfo(PFILE_ALL_INFORMATION AllInfo,
                     PBY_HANDLE_FILE_INFORMATION FileInfo,
                     PULONG RemainingLength, PEVENT_CONTEXT EventContext,
                     PDOKAN_INSTANCE DokanInstance) {
  ULONG allRemainingLength = *RemainingLength;

  if (*RemainingLength < sizeof(FILE_ALL_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  // FileBasicInformation
  DokanFillFileBasicInfo(&AllInfo->BasicInformation, FileInfo, RemainingLength);

  // FileStandardInformation
  DokanFillFileStandardInfo(&AllInfo->StandardInformation, FileInfo,
                            RemainingLength, DokanInstance);

  // FileInternalInformation
  DokanFillInternalInfo(&AllInfo->InternalInformation, FileInfo,
                        RemainingLength);

  AllInfo->EaInformation.EaSize = 0;

  // FilePositionInformation
  DokanFillFilePositionInfo(&AllInfo->PositionInformation, FileInfo,
                            RemainingLength);

  // there is not enough space to fill FileNameInformation
  if (allRemainingLength < sizeof(FILE_ALL_INFORMATION) +
                               EventContext->Operation.File.FileNameLength) {
    // fill out to the limit
    // FileNameInformation
    AllInfo->NameInformation.FileNameLength =
        EventContext->Operation.File.FileNameLength;
    AllInfo->NameInformation.FileName[0] =
        EventContext->Operation.File.FileName[0];

    allRemainingLength -= sizeof(FILE_ALL_INFORMATION);
    *RemainingLength = allRemainingLength;
    return STATUS_BUFFER_OVERFLOW;
  }

  // FileNameInformation
  AllInfo->NameInformation.FileNameLength =
      EventContext->Operation.File.FileNameLength;
  RtlCopyMemory(&(AllInfo->NameInformation.FileName[0]),
                EventContext->Operation.File.FileName,
                EventContext->Operation.File.FileNameLength);

  // the size except of FILE_NAME_INFORMATION
  allRemainingLength -=
      (sizeof(FILE_ALL_INFORMATION) - sizeof(FILE_NAME_INFORMATION));

  // the size of FILE_NAME_INFORMATION
  allRemainingLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]);
  allRemainingLength -= AllInfo->NameInformation.FileNameLength;

  *RemainingLength = allRemainingLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileNameInfo(PFILE_NAME_INFORMATION NameInfo,
                      PBY_HANDLE_FILE_INFORMATION FileInfo,
                      PULONG RemainingLength, PEVENT_CONTEXT EventContext) {

  UNREFERENCED_PARAMETER(FileInfo);

  if (*RemainingLength < sizeof(FILE_NAME_INFORMATION) +
                             EventContext->Operation.File.FileNameLength) {
    return STATUS_BUFFER_OVERFLOW;
  }

  NameInfo->FileNameLength = EventContext->Operation.File.FileNameLength;
  RtlCopyMemory(&(NameInfo->FileName[0]), EventContext->Operation.File.FileName,
                EventContext->Operation.File.FileNameLength);

  *RemainingLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]);
  *RemainingLength -= NameInfo->FileNameLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileAttributeTagInfo(PFILE_ATTRIBUTE_TAG_INFORMATION AttrTagInfo,
                              PBY_HANDLE_FILE_INFORMATION FileInfo,
                              PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  AttrTagInfo->FileAttributes = FileInfo->dwFileAttributes;
  AttrTagInfo->ReparseTag = 0;

  *RemainingLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillNetworkOpenInfo(PFILE_NETWORK_OPEN_INFORMATION NetInfo,
                         PBY_HANDLE_FILE_INFORMATION FileInfo,
                         PULONG RemainingLength,
                         PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  NetInfo->CreationTime.LowPart = FileInfo->ftCreationTime.dwLowDateTime;
  NetInfo->CreationTime.HighPart = FileInfo->ftCreationTime.dwHighDateTime;
  NetInfo->LastAccessTime.LowPart = FileInfo->ftLastAccessTime.dwLowDateTime;
  NetInfo->LastAccessTime.HighPart = FileInfo->ftLastAccessTime.dwHighDateTime;
  NetInfo->LastWriteTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  NetInfo->LastWriteTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  NetInfo->ChangeTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  NetInfo->ChangeTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  NetInfo->AllocationSize.HighPart = FileInfo->nFileSizeHigh;
  NetInfo->AllocationSize.LowPart = FileInfo->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&NetInfo->AllocationSize, DokanInstance->DokanOptions);
  NetInfo->EndOfFile.HighPart = FileInfo->nFileSizeHigh;
  NetInfo->EndOfFile.LowPart = FileInfo->nFileSizeLow;
  NetInfo->FileAttributes = FileInfo->dwFileAttributes;

  *RemainingLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillNetworkPhysicalNameInfo(
    PFILE_NETWORK_PHYSICAL_NAME_INFORMATION NetInfo,
    PBY_HANDLE_FILE_INFORMATION FileInfo, PULONG RemainingLength,
    PEVENT_CONTEXT EventContext) {
  if (*RemainingLength < sizeof(FILE_NETWORK_PHYSICAL_NAME_INFORMATION) +
                             EventContext->Operation.File.FileNameLength) {
    return STATUS_BUFFER_OVERFLOW;
  }

  UNREFERENCED_PARAMETER(FileInfo);

  NetInfo->FileNameLength = EventContext->Operation.File.FileNameLength;
  CopyMemory(NetInfo->FileName, EventContext->Operation.File.FileName,
             EventContext->Operation.File.FileNameLength);

  *RemainingLength -=
      FIELD_OFFSET(FILE_NETWORK_PHYSICAL_NAME_INFORMATION, FileName[0]);
  *RemainingLength -= NetInfo->FileNameLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillIdInfo(PFILE_ID_INFORMATION IdInfo,
                PBY_HANDLE_FILE_INFORMATION FileInfo, PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_ID_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  IdInfo->VolumeSerialNumber = FileInfo->dwVolumeSerialNumber;

  ZeroMemory(IdInfo->FileId.Identifier, sizeof(IdInfo->FileId.Identifier));

  ((DWORD *)(IdInfo->FileId.Identifier))[0] = FileInfo->nFileIndexLow;
  ((DWORD *)(IdInfo->FileId.Identifier))[1] = FileInfo->nFileIndexHigh;

  *RemainingLength -= sizeof(FILE_ID_INFORMATION);

  return STATUS_SUCCESS;
}

DOKAN_STREAM_FIND_RESULT WINAPI DokanFillFindStreamData(
	PDOKAN_FIND_STREAMS_EVENT EventInfo,
	PWIN32_FIND_STREAM_DATA FindStreamData) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	ULONG offset = (ULONG)ioEvent->EventResult->BufferLength;
	ULONG resultBufferSize = ioEvent->KernelInfo.EventContext.Operation.File.BufferLength;
	
	ULONG streamNameLength =
		(ULONG)wcslen(FindStreamData->cStreamName) * sizeof(WCHAR);

	// Must be aligned on a 8-byte boundary.
	ULONG entrySize = QuadAlign(sizeof(FILE_STREAM_INFORMATION) + streamNameLength);

	assert(entrySize % DOKAN_STREAM_ENTRY_ALIGNMENT == 0);

	PFILE_STREAM_INFORMATION streamInfo = (PFILE_STREAM_INFORMATION)&ioEvent->EventResult->Buffer[offset];

	if(offset + entrySize + streamInfo->NextEntryOffset > resultBufferSize) {

		return DOKAN_STREAM_BUFFER_FULL;
	}

	// If this isn't the first entry move to the next
	// memory location
	if(streamInfo->NextEntryOffset != 0) {

		offset += streamInfo->NextEntryOffset;
		streamInfo = (PFILE_STREAM_INFORMATION)&ioEvent->EventResult->Buffer[offset];
	}

	assert(streamInfo->NextEntryOffset == 0);

	// Fill the new entry
	streamInfo->StreamNameLength = streamNameLength;
	
	memcpy_s(streamInfo->StreamName, streamNameLength, FindStreamData->cStreamName, streamNameLength);

	streamInfo->StreamSize = FindStreamData->StreamSize;
	streamInfo->StreamAllocationSize = FindStreamData->StreamSize;
	streamInfo->NextEntryOffset = entrySize;
	
	ALIGN_ALLOCATION_SIZE(&streamInfo->StreamAllocationSize, ioEvent->DokanInstance->DokanOptions);

	ioEvent->EventResult->BufferLength = offset;

	return DOKAN_STREAM_BUFFER_CONTINUE;
}

void BeginDispatchQueryInformation(DOKAN_IO_EVENT *EventInfo) {

  DOKAN_GET_FILE_INFO_EVENT *getFileInfo = &EventInfo->EventInfo.GetFileInfo;
  DOKAN_FIND_STREAMS_EVENT *findStreams = &EventInfo->EventInfo.FindStreams;
  NTSTATUS status = STATUS_INVALID_PARAMETER;

  DbgPrint("###GetFileInfo file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
	  EventInfo);

  assert((void*)getFileInfo == (void*)EventInfo && (void*)findStreams == (void*)EventInfo);
  assert(EventInfo->ProcessingContext == NULL);

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.File.FileName);

  CreateDispatchCommon(EventInfo, EventInfo->KernelInfo.EventContext.Operation.File.BufferLength);

  if(EventInfo->KernelInfo.EventContext.Operation.File.FileInformationClass == FileStreamInformation) {

	  DbgPrint("FileStreamInformation\n");

	  // https://msdn.microsoft.com/en-us/library/windows/hardware/ff540364(v=vs.85).aspx
	  if(EventInfo->KernelInfo.EventContext.Operation.File.BufferLength < sizeof(FILE_STREAM_INFORMATION)) {

		  status = STATUS_BUFFER_TOO_SMALL;
	  }
	  else if(EventInfo->DokanInstance->DokanOperations->FindStreams) {

		  findStreams->DokanFileInfo = &EventInfo->DokanFileInfo;
		  findStreams->FileName = EventInfo->KernelInfo.EventContext.Operation.File.FileName;
		  findStreams->FillFindStreamData = DokanFillFindStreamData;

		  status = EventInfo->DokanInstance->DokanOperations->FindStreams(findStreams);
	  }
	  else {

		  status = STATUS_NOT_IMPLEMENTED;
	  }
  }
  else if(EventInfo->DokanInstance->DokanOperations->GetFileInformation) {

	  getFileInfo->DokanFileInfo = &EventInfo->DokanFileInfo;
	  getFileInfo->FileName = EventInfo->KernelInfo.EventContext.Operation.File.FileName;

    status = EventInfo->DokanInstance->DokanOperations->GetFileInformation(getFileInfo);
  }
  else {

    status = STATUS_NOT_IMPLEMENTED;
  }

  if(status != STATUS_PENDING) {

	  if(EventInfo->KernelInfo.EventContext.Operation.File.FileInformationClass == FileStreamInformation) {

		  DokanEndDispatchFindStreams(findStreams, status);
	  }
	  else {

		  DokanEndDispatchGetFileInformation(getFileInfo, status);
	  }
  }
}

void DOKANAPI DokanEndDispatchGetFileInformation(DOKAN_GET_FILE_INFO_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	ULONG remainingLength = ioEvent->KernelInfo.EventContext.Operation.File.BufferLength;

	DbgPrint("\tresult =  %lx\n", ResultStatus);

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchGetFileInformation() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}
	
	assert(ioEvent->EventResult->BufferLength == 0);

	if(ResultStatus == STATUS_SUCCESS) {

		switch(ioEvent->KernelInfo.EventContext.Operation.File.FileInformationClass) {
		case FileBasicInformation:
			DbgPrint("\tFileBasicInformation\n");
			ResultStatus =
				DokanFillFileBasicInfo((PFILE_BASIC_INFORMATION)ioEvent->EventResult->Buffer,
					&EventInfo->FileHandleInfo, &remainingLength);
			break;

		case FileIdInformation:
			DbgPrint("\tFileIdInformation\n");
			ResultStatus = DokanFillIdInfo((PFILE_ID_INFORMATION)ioEvent->EventResult->Buffer,
				&EventInfo->FileHandleInfo, &remainingLength);
			break;

		case FileInternalInformation:
			DbgPrint("\tFileInternalInformation\n");
			ResultStatus =
				DokanFillInternalInfo((PFILE_INTERNAL_INFORMATION)ioEvent->EventResult->Buffer,
					&EventInfo->FileHandleInfo, &remainingLength);
			break;

		case FileEaInformation:
			DbgPrint("\tFileEaInformation\n");
			// status = STATUS_NOT_IMPLEMENTED;
			ResultStatus = STATUS_SUCCESS;
			remainingLength -= sizeof(FILE_EA_INFORMATION);
			break;

		case FileStandardInformation:
			DbgPrint("\tFileStandardInformation\n");
			ResultStatus = DokanFillFileStandardInfo(
				(PFILE_STANDARD_INFORMATION)ioEvent->EventResult->Buffer, &EventInfo->FileHandleInfo,
				&remainingLength, ioEvent->DokanInstance);
			break;

		case FileAllInformation:
			DbgPrint("\tFileAllInformation\n");
			ResultStatus = DokanFillFileAllInfo((PFILE_ALL_INFORMATION)ioEvent->EventResult->Buffer,
				&EventInfo->FileHandleInfo, &remainingLength,
				&ioEvent->KernelInfo.EventContext, ioEvent->DokanInstance);
			break;

		case FileAlternateNameInformation:
			DbgPrint("\tFileAlternateNameInformation (not supported)\n");
			ResultStatus = STATUS_NOT_IMPLEMENTED;
			break;

		case FileAttributeTagInformation:
			DbgPrint("\tFileAttributeTagInformation\n");
			ResultStatus = DokanFillFileAttributeTagInfo(
				(PFILE_ATTRIBUTE_TAG_INFORMATION)ioEvent->EventResult->Buffer, &EventInfo->FileHandleInfo,
				&remainingLength);
			break;

		case FileCompressionInformation:
			DbgPrint("\tFileCompressionInformation (not supported)\n");

			if(remainingLength < sizeof(FILE_COMPRESSION_INFORMATION)) {

				ResultStatus = STATUS_BUFFER_OVERFLOW;
			}
			else {
				
				// https://msdn.microsoft.com/en-us/library/cc232096.aspx?f=255&MSPPError=-2147217396

				ResultStatus = STATUS_SUCCESS;

				// zero disables compression
				RtlZeroMemory(ioEvent->EventResult->Buffer, sizeof(FILE_COMPRESSION_INFORMATION));

				remainingLength -= sizeof(FILE_COMPRESSION_INFORMATION);
			}

			break;

		case FileNormalizedNameInformation:
			DbgPrint("\tFileNormalizedNameInformation\n");
		case FileNameInformation:
			// this case is not used because driver deal with
			DbgPrint("\tFileNameInformation\n");
			ResultStatus = DokanFillFileNameInfo((PFILE_NAME_INFORMATION)ioEvent->EventResult->Buffer,
				&EventInfo->FileHandleInfo, &remainingLength,
				&ioEvent->KernelInfo.EventContext);
			break;

		case FileNetworkOpenInformation:
			DbgPrint("\tFileNetworkOpenInformation\n");
			ResultStatus = DokanFillNetworkOpenInfo(
				(PFILE_NETWORK_OPEN_INFORMATION)ioEvent->EventResult->Buffer, &EventInfo->FileHandleInfo,
				&remainingLength, ioEvent->DokanInstance);
			break;

		case FilePositionInformation:
			// this case is not used because driver deal with
			DbgPrint("\tFilePositionInformation\n");
			ResultStatus = DokanFillFilePositionInfo(
				(PFILE_POSITION_INFORMATION)ioEvent->EventResult->Buffer, &EventInfo->FileHandleInfo,
				&remainingLength);
			break;
		case FileStreamInformation:
			DbgPrint("FileStreamInformation (internal error)\n");
			// shouldn't get here
			ResultStatus = STATUS_INTERNAL_ERROR;
			break;
		case FileNetworkPhysicalNameInformation:
			DbgPrint("FileNetworkPhysicalNameInformation\n");
			ResultStatus = DokanFillNetworkPhysicalNameInfo(
				(PFILE_NETWORK_PHYSICAL_NAME_INFORMATION)ioEvent->EventResult->Buffer,
				&EventInfo->FileHandleInfo, &remainingLength, &ioEvent->KernelInfo.EventContext);
			break;
		case FileStandardLinkInformation:
			DbgPrint("FileStandardLinkInformation (not supported)\n");
			// https://msdn.microsoft.com/en-us/library/dd414603.aspx?f=255&MSPPError=-2147217396
			ResultStatus = STATUS_NOT_SUPPORTED;
			break;
		default: {
			ResultStatus = STATUS_INVALID_PARAMETER;
			DbgPrint("  unknown type:%d\n",
				ioEvent->KernelInfo.EventContext.Operation.File.FileInformationClass);
		} break;
		}

		ioEvent->EventResult->BufferLength =
			ioEvent->KernelInfo.EventContext.Operation.File.BufferLength - remainingLength;
	}

	ioEvent->EventResult->Status = ResultStatus;

	DbgPrint("\tDispatchQueryInformation result =  %lx\n", ResultStatus);

	SendIoEventResult(ioEvent);
}

void DOKANAPI DokanEndDispatchFindStreams(DOKAN_FIND_STREAMS_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	ULONG resultBufferSize = IoEventResultBufferSize(ioEvent);
	PFILE_STREAM_INFORMATION streamInfo =
		(PFILE_STREAM_INFORMATION)&ioEvent->EventResult->Buffer[ioEvent->EventResult->BufferLength];

	DbgPrint("\tresult =  %lx\n", ResultStatus);

	// Entries must be 8 byte aligned
	assert(streamInfo->NextEntryOffset % DOKAN_STREAM_ENTRY_ALIGNMENT == 0);

	// Ensure that the last entry doesn't point to another entry.
	ioEvent->EventResult->BufferLength += streamInfo->NextEntryOffset;
	streamInfo->NextEntryOffset = 0;

	assert(ioEvent->EventResult->BufferLength <= resultBufferSize);

	if(ioEvent->EventResult->BufferLength > resultBufferSize) {

		ioEvent->EventResult->BufferLength = 0;
		ResultStatus = STATUS_BUFFER_OVERFLOW;
	}

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchFindStreams() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	ioEvent->EventResult->Status = ResultStatus;

	DbgPrint("\tDokanEndDispatchFindStreams result =  0x%x\n", ResultStatus);

	SendIoEventResult(ioEvent);
}