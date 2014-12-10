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

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "dokani.h"
#include "fileinfo.h"


ULONG
DokanFillFileBasicInfo(
	PFILE_BASIC_INFORMATION		BasicInfo,
	PBY_HANDLE_FILE_INFORMATION FileInfo,
	PULONG						RemainingLength)
{
	if (*RemainingLength < sizeof(FILE_BASIC_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}

	BasicInfo->CreationTime.LowPart   = FileInfo->ftCreationTime.dwLowDateTime;
	BasicInfo->CreationTime.HighPart  = FileInfo->ftCreationTime.dwHighDateTime;
	BasicInfo->LastAccessTime.LowPart = FileInfo->ftLastAccessTime.dwLowDateTime;
	BasicInfo->LastAccessTime.HighPart= FileInfo->ftLastAccessTime.dwHighDateTime;
	BasicInfo->LastWriteTime.LowPart  = FileInfo->ftLastWriteTime.dwLowDateTime;
	BasicInfo->LastWriteTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
	BasicInfo->ChangeTime.LowPart     = FileInfo->ftLastWriteTime.dwLowDateTime;
	BasicInfo->ChangeTime.HighPart    = FileInfo->ftLastWriteTime.dwHighDateTime;
	BasicInfo->FileAttributes         = FileInfo->dwFileAttributes;

	*RemainingLength -= sizeof(FILE_BASIC_INFORMATION);
	
	return STATUS_SUCCESS;
}


ULONG
DokanFillFileStandardInfo(
	PFILE_STANDARD_INFORMATION	StandardInfo,
	PBY_HANDLE_FILE_INFORMATION	FileInfo,
	PULONG						RemainingLength)
{
	if (*RemainingLength < sizeof(FILE_STANDARD_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}

	StandardInfo->AllocationSize.HighPart = FileInfo->nFileSizeHigh;
	StandardInfo->AllocationSize.LowPart  = FileInfo->nFileSizeLow;
	StandardInfo->EndOfFile.HighPart      = FileInfo->nFileSizeHigh;
	StandardInfo->EndOfFile.LowPart       = FileInfo->nFileSizeLow;
	StandardInfo->NumberOfLinks           = FileInfo->nNumberOfLinks;
	StandardInfo->DeletePending           = FALSE;
	StandardInfo->Directory               = FALSE;

	if (FileInfo->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		StandardInfo->Directory = TRUE;
	}

	*RemainingLength -= sizeof(FILE_STANDARD_INFORMATION);
	
	return STATUS_SUCCESS;
}


ULONG
DokanFillFilePositionInfo(
	PFILE_POSITION_INFORMATION	PosInfo,
	PBY_HANDLE_FILE_INFORMATION	FileInfo,
	PULONG						RemainingLength)
{

	if (*RemainingLength < sizeof(FILE_POSITION_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}

	// this field is filled by driver
	PosInfo->CurrentByteOffset.QuadPart = 0;//fileObject->CurrentByteOffset;
				
	*RemainingLength -= sizeof(FILE_POSITION_INFORMATION);
	
	return STATUS_SUCCESS;
}


ULONG
DokanFillFileAllInfo(
	PFILE_ALL_INFORMATION		AllInfo,
	PBY_HANDLE_FILE_INFORMATION	FileInfo,
	PULONG						RemainingLength,
	PEVENT_CONTEXT				EventContext)
{
	ULONG	allRemainingLength = *RemainingLength;

	if (*RemainingLength < sizeof(FILE_ALL_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}
	
	// FileBasicInformation
	DokanFillFileBasicInfo(&AllInfo->BasicInformation, FileInfo, RemainingLength);
	
	// FileStandardInformation
	DokanFillFileStandardInfo(&AllInfo->StandardInformation, FileInfo, RemainingLength);
	
	// FilePositionInformation
	DokanFillFilePositionInfo(&AllInfo->PositionInformation, FileInfo, RemainingLength);

	// there is not enough space to fill FileNameInformation
	if (allRemainingLength < sizeof(FILE_ALL_INFORMATION) + EventContext->File.FileNameLength) {
		// fill out to the limit
		// FileNameInformation
		AllInfo->NameInformation.FileNameLength = EventContext->File.FileNameLength;
		AllInfo->NameInformation.FileName[0] = EventContext->File.FileName[0];
					
		allRemainingLength -= sizeof(FILE_ALL_INFORMATION);
		*RemainingLength = allRemainingLength;
		return STATUS_BUFFER_OVERFLOW;
	}

	// FileNameInformation
	AllInfo->NameInformation.FileNameLength = EventContext->File.FileNameLength;
	RtlCopyMemory(&(AllInfo->NameInformation.FileName[0]),
					EventContext->File.FileName, EventContext->File.FileNameLength);

	// the size except of FILE_NAME_INFORMATION
	allRemainingLength -= (sizeof(FILE_ALL_INFORMATION) - sizeof(FILE_NAME_INFORMATION));

	// the size of FILE_NAME_INFORMATION
	allRemainingLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]);
	allRemainingLength -= AllInfo->NameInformation.FileNameLength;
	
	*RemainingLength = allRemainingLength;
	
	return STATUS_SUCCESS;
}


ULONG
DokanFillFileNameInfo(
	PFILE_NAME_INFORMATION		NameInfo,
	PBY_HANDLE_FILE_INFORMATION	FileInfo,
	PULONG						RemainingLength,
	PEVENT_CONTEXT				EventContext)
{
	if (*RemainingLength < sizeof(FILE_NAME_INFORMATION) 
		+ EventContext->File.FileNameLength) {
		return STATUS_BUFFER_OVERFLOW;
	}

	NameInfo->FileNameLength = EventContext->File.FileNameLength;
	RtlCopyMemory(&(NameInfo->FileName[0]),
			EventContext->File.FileName, EventContext->File.FileNameLength);

	*RemainingLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]);
	*RemainingLength -= NameInfo->FileNameLength;

	return STATUS_SUCCESS;
}


ULONG
DokanFillFileAttributeTagInfo(
	PFILE_ATTRIBUTE_TAG_INFORMATION		AttrTagInfo,
	PBY_HANDLE_FILE_INFORMATION			FileInfo,
	PULONG								RemainingLength)
{
	if (*RemainingLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}

	AttrTagInfo->FileAttributes = FileInfo->dwFileAttributes;
	AttrTagInfo->ReparseTag = 0;

	*RemainingLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);

	return STATUS_SUCCESS;
}


ULONG
DokanFillNetworkOpenInfo(
	PFILE_NETWORK_OPEN_INFORMATION	NetInfo,
	PBY_HANDLE_FILE_INFORMATION		FileInfo,
	PULONG							RemainingLength)
{
	if (*RemainingLength < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}

	NetInfo->CreationTime.LowPart	= FileInfo->ftCreationTime.dwLowDateTime;
	NetInfo->CreationTime.HighPart	= FileInfo->ftCreationTime.dwHighDateTime;
	NetInfo->LastAccessTime.LowPart	= FileInfo->ftLastAccessTime.dwLowDateTime;
	NetInfo->LastAccessTime.HighPart= FileInfo->ftLastAccessTime.dwHighDateTime;
	NetInfo->LastWriteTime.LowPart	= FileInfo->ftLastWriteTime.dwLowDateTime;
	NetInfo->LastWriteTime.HighPart	= FileInfo->ftLastWriteTime.dwHighDateTime;
	NetInfo->ChangeTime.LowPart		= FileInfo->ftLastWriteTime.dwLowDateTime;
	NetInfo->ChangeTime.HighPart	= FileInfo->ftLastWriteTime.dwHighDateTime;
	NetInfo->AllocationSize.HighPart= FileInfo->nFileSizeHigh;
	NetInfo->AllocationSize.LowPart	= FileInfo->nFileSizeLow;
	NetInfo->EndOfFile.HighPart		= FileInfo->nFileSizeHigh;
	NetInfo->EndOfFile.LowPart		= FileInfo->nFileSizeLow;
	NetInfo->FileAttributes			= FileInfo->dwFileAttributes;

	*RemainingLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);

	return STATUS_SUCCESS;
}


ULONG
DokanFillInternalInfo(
	PFILE_INTERNAL_INFORMATION	InternalInfo,
	PBY_HANDLE_FILE_INFORMATION	FileInfo,
	PULONG						RemainingLength)
{
	if (*RemainingLength < sizeof(FILE_INTERNAL_INFORMATION)) {
		return STATUS_BUFFER_OVERFLOW;
	}

	InternalInfo->IndexNumber.HighPart = FileInfo->nFileIndexHigh;
	InternalInfo->IndexNumber.LowPart = FileInfo->nFileIndexLow;

	*RemainingLength -= sizeof(FILE_INTERNAL_INFORMATION);

	return STATUS_SUCCESS;
}


VOID
DispatchQueryInformation(
	HANDLE				Handle,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_INSTANCE		DokanInstance)
{
	PEVENT_INFORMATION			eventInfo;
	DOKAN_FILE_INFO				fileInfo;
	BY_HANDLE_FILE_INFORMATION	byHandleFileInfo;
	ULONG				remainingLength;
	ULONG				status;
	int					result;
	PDOKAN_OPEN_INFO	openInfo;
	ULONG				sizeOfEventInfo;

	sizeOfEventInfo = sizeof(EVENT_INFORMATION) - 8 + EventContext->File.BufferLength;

	CheckFileName(EventContext->File.FileName);

	ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));

	eventInfo = DispatchCommon(
		EventContext, sizeOfEventInfo, DokanInstance, &fileInfo, &openInfo);
	
	eventInfo->BufferLength = EventContext->File.BufferLength;

	DbgPrint("###GetFileInfo %04d\n", openInfo != NULL ? openInfo->EventId : -1);

	if (DokanInstance->DokanOperations->GetFileInformation) {
		result = DokanInstance->DokanOperations->GetFileInformation(
										EventContext->File.FileName,
										&byHandleFileInfo,
										&fileInfo);
	} else {
		result = -1;
	}

	remainingLength = eventInfo->BufferLength;

	if (result < 0) {
		eventInfo->Status = STATUS_INVALID_PARAMETER;
		eventInfo->BufferLength = 0;
	
	} else {

		switch(EventContext->File.FileInformationClass) {
		case FileBasicInformation:
			//DbgPrint("FileBasicInformation\n");
			status = DokanFillFileBasicInfo((PVOID)eventInfo->Buffer,
										&byHandleFileInfo, &remainingLength);
			break;

		case FileInternalInformation:
			status = DokanFillInternalInfo((PVOID)eventInfo->Buffer,
											&byHandleFileInfo, &remainingLength);
			break;

		case FileEaInformation:
			//DbgPrint("FileEaInformation or FileInternalInformation\n");
			//status = STATUS_NOT_IMPLEMENTED;
			status = STATUS_SUCCESS;
			remainingLength -= sizeof(FILE_EA_INFORMATION);
			break;

		case FileStandardInformation:
			//DbgPrint("FileStandardInformation\n");
			status = DokanFillFileStandardInfo((PVOID)eventInfo->Buffer,
										&byHandleFileInfo, &remainingLength);
			break;

		case FileAllInformation:
			//DbgPrint("FileAllInformation\n");
			status = DokanFillFileAllInfo((PVOID)eventInfo->Buffer,
										&byHandleFileInfo, &remainingLength, EventContext);
			break;

		case FileAlternateNameInformation:
			status = STATUS_NOT_IMPLEMENTED;
			break;

		case FileAttributeTagInformation:
			status = DokanFillFileAttributeTagInfo((PVOID)eventInfo->Buffer,
										&byHandleFileInfo, &remainingLength);
			break;

		case FileCompressionInformation:
			//DbgPrint("FileAlternateNameInformation or...\n");
			status = STATUS_NOT_IMPLEMENTED;
			break;

		case FileNameInformation:
			// this case is not used because driver deal with
			//DbgPrint("FileNameInformation\n");
			status = DokanFillFileNameInfo((PVOID)eventInfo->Buffer,
								&byHandleFileInfo, &remainingLength, EventContext);
			break;

		case FileNetworkOpenInformation:
			//DbgPrint("FileNetworkOpenInformation\n");
			status = DokanFillNetworkOpenInfo((PVOID)eventInfo->Buffer,
								&byHandleFileInfo, &remainingLength);
			break;

		case FilePositionInformation:
			// this case is not used because driver deal with
			//DbgPrint("FilePositionInformation\n");
			status = DokanFillFilePositionInfo((PVOID)eventInfo->Buffer,
								&byHandleFileInfo, &remainingLength);

			break;
		case FileStreamInformation:
			//DbgPrint("FileStreamInformation\n");
			status = STATUS_NOT_IMPLEMENTED;
			break;
        default:
			{
				DbgPrint("  unknown type:%d\n", EventContext->File.FileInformationClass);
			}
            break;
		}
	
		eventInfo->Status = status;
		eventInfo->BufferLength = EventContext->File.BufferLength - remainingLength;
	}

	// information for FileSystem
	openInfo->UserContext = fileInfo.Context;

	SendEventInformation(Handle, eventInfo, sizeOfEventInfo, DokanInstance);
	free(eventInfo);
	return;

}
