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

#include <ntstatus.h>
#include "dokani.h"


VOID
DispatchCreate(
	HANDLE				Handle, // This handle is not for a file. It is for Dokan Device Driver(which is doing EVENT_WAIT).
	PEVENT_CONTEXT		EventContext,
	PDOKAN_INSTANCE		DokanInstance)
{
	static int								eventId = 0;
	ULONG									length	  = sizeof(EVENT_INFORMATION);
	PEVENT_INFORMATION						eventInfo = (PEVENT_INFORMATION)malloc(length);
	NTSTATUS								status = STATUS_INSUFFICIENT_RESOURCES;
	DOKAN_FILE_INFO							fileInfo;
	ULONG									disposition;
	PDOKAN_OPEN_INFO						openInfo = NULL;
	BOOL									directoryRequested = FALSE;
	DWORD									options;
	DOKAN_IO_SECURITY_CONTEXT				ioSecurityContext;
	WCHAR									*fileName;
	PDOKAN_UNICODE_STRING_INTERMEDIATE		intermediateObjName = NULL;
	PDOKAN_UNICODE_STRING_INTERMEDIATE		intermediateObjType = NULL;

	if (eventInfo == NULL) {
		return;
	}

	fileName = (WCHAR*)((char*)&EventContext->Operation.Create + EventContext->Operation.Create.FileNameOffset);

	CheckFileName(fileName);

	RtlZeroMemory(eventInfo, length);
	RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));

	eventInfo->BufferLength = 0;
	eventInfo->SerialNumber = EventContext->SerialNumber;

	fileInfo.ProcessId = EventContext->ProcessId;
	fileInfo.DokanOptions = DokanInstance->DokanOptions;

	// DOKAN_OPEN_INFO is structure for a opened file
	// this will be freed by Close
	openInfo = malloc(sizeof(DOKAN_OPEN_INFO));
	if (openInfo == NULL) {
		eventInfo->Status = STATUS_INSUFFICIENT_RESOURCES;
		SendEventInformation(Handle, eventInfo, length, NULL);
		free(eventInfo);
		return;
	}
	ZeroMemory(openInfo, sizeof(DOKAN_OPEN_INFO));
	openInfo->OpenCount = 2;
	openInfo->EventContext = EventContext;
	openInfo->DokanInstance = DokanInstance;
	fileInfo.DokanContext = (ULONG64)openInfo;

	// pass it to driver and when the same handle is used get it back
	eventInfo->Context = (ULONG64)openInfo;

	// The high 8 bits of this parameter correspond to the Disposition parameter
	disposition = (EventContext->Operation.Create.CreateOptions >> 24) & 0x000000ff;
	
	// The low 24 bits of this member correspond to the CreateOptions parameter
	options = EventContext->Operation.Create.CreateOptions & FILE_VALID_OPTION_FLAGS;
	//DbgPrint("Create.CreateOptions 0x%x\n", options);

	// to open directory
	// even if this flag is not specifed, 
	// there is a case to open a directory
	if (options & FILE_DIRECTORY_FILE) {
		//DbgPrint("FILE_DIRECTORY_FILE\n");
		directoryRequested = TRUE;
	}
	else {
	    if (EventContext->Flags & SL_OPEN_TARGET_DIRECTORY) {
			DbgPrint("SL_OPEN_TARGET_DIRECTORY specified\n");
			// strip the last section of the file path
			WCHAR* lastP = NULL;
			for (WCHAR* p = fileName; *p; p++) {
				if ((*p == L'\\' || *p == L'/') && p[1])
				lastP = p;
			}
			if (lastP) {
				directoryRequested = TRUE;
				*lastP = 0;
			}
	    }

	}

	// to open no directory file
	// event if this flag is not specified,
	// there is a case to open non directory file
	if (options & FILE_NON_DIRECTORY_FILE) {
		//DbgPrint("FILE_NON_DIRECTORY_FILE\n");
	}

	DbgPrint("###Create %04d\n", eventId);
	//DbgPrint("### OpenInfo %X\n", openInfo);
	openInfo->EventId = eventId++;

	// make a directory or open
	if (directoryRequested) {
		fileInfo.IsDirectory = TRUE;

		if (disposition == FILE_CREATE || disposition == FILE_OPEN_IF) {
			if (DokanInstance->DokanOperations->CreateDirectory) {
				status = DokanInstance->DokanOperations->CreateDirectory(fileName, &fileInfo);
			}
		} else if(disposition == FILE_OPEN) {
			if (DokanInstance->DokanOperations->OpenDirectory) {
				status = DokanInstance->DokanOperations->OpenDirectory(fileName, &fileInfo);
			}
		} else {
			DbgPrint("### Create other disposition : %d\n", disposition);
		}
	
	// open a file
	}
	else {
		if(DokanInstance->DokanOperations->CreateFile) {
			
			ioSecurityContext.AccessState.Flags = EventContext->Operation.Create.SecurityContext.AccessState.Flags;
			ioSecurityContext.AccessState.RemainingDesiredAccess = EventContext->Operation.Create.SecurityContext.AccessState.RemainingDesiredAccess;
			ioSecurityContext.AccessState.PreviouslyGrantedAccess = EventContext->Operation.Create.SecurityContext.AccessState.PreviouslyGrantedAccess;
			ioSecurityContext.AccessState.OriginalDesiredAccess = EventContext->Operation.Create.SecurityContext.AccessState.OriginalDesiredAccess;

			if(EventContext->Operation.Create.SecurityContext.AccessState.SecurityDescriptorOffset > 0) {
				ioSecurityContext.AccessState.SecurityDescriptor = (PSECURITY_DESCRIPTOR)((char*)&EventContext->Operation.Create.SecurityContext.AccessState + EventContext->Operation.Create.SecurityContext.AccessState.SecurityDescriptorOffset);
			}
			else {
				ioSecurityContext.AccessState.SecurityDescriptor = NULL;
			}

			intermediateObjName = (PDOKAN_UNICODE_STRING_INTERMEDIATE)((char*)&EventContext->Operation.Create.SecurityContext.AccessState + EventContext->Operation.Create.SecurityContext.AccessState.UnicodeStringObjectNameOffset);
			intermediateObjType = (PDOKAN_UNICODE_STRING_INTERMEDIATE)((char*)&EventContext->Operation.Create.SecurityContext.AccessState + EventContext->Operation.Create.SecurityContext.AccessState.UnicodeStringObjectTypeOffset);

			ioSecurityContext.AccessState.ObjectName.Length = intermediateObjName->Length;
			ioSecurityContext.AccessState.ObjectName.MaximumLength = intermediateObjName->MaximumLength;
			ioSecurityContext.AccessState.ObjectName.Buffer = &intermediateObjName->Buffer[0];

			ioSecurityContext.AccessState.ObjectType.Length = intermediateObjType->Length;
			ioSecurityContext.AccessState.ObjectType.MaximumLength = intermediateObjType->MaximumLength;
			ioSecurityContext.AccessState.ObjectType.Buffer = &intermediateObjType->Buffer[0];

			ioSecurityContext.DesiredAccess = EventContext->Operation.Create.SecurityContext.DesiredAccess;

			status = DokanInstance->DokanOperations->CreateFile(
				fileName,
				&ioSecurityContext,
				ioSecurityContext.DesiredAccess,
				EventContext->Operation.Create.FileAttributes,
				EventContext->Operation.Create.ShareAccess,
				disposition,
				options,
				&fileInfo);
		}
		else {
			status = STATUS_NOT_IMPLEMENTED;
		}
	}

	// save the information about this access in DOKAN_OPEN_INFO
	openInfo->IsDirectory = fileInfo.IsDirectory;
	openInfo->UserContext = fileInfo.Context;

	// FILE_CREATED
	// FILE_DOES_NOT_EXIST
	// FILE_EXISTS
	// FILE_OPENED
	// FILE_OVERWRITTEN
	// FILE_SUPERSEDED


    DbgPrint("CreateFile status = %lu\n", status);
	if (status != STATUS_SUCCESS) {
		if (EventContext->Flags & SL_OPEN_TARGET_DIRECTORY)
		{
			DbgPrint("SL_OPEN_TARGET_DIRECTORY spcefied\n");
		}
		eventInfo->Operation.Create.Information = FILE_DOES_NOT_EXIST;
		eventInfo->Status = status;

		if (status == STATUS_OBJECT_NAME_NOT_FOUND && EventContext->Flags & SL_OPEN_TARGET_DIRECTORY)
		{
			DbgPrint("This case should be returned as SUCCESS\n");
			eventInfo->Status = STATUS_SUCCESS;
		}

		if (status == STATUS_OBJECT_NAME_COLLISION)
		{
			eventInfo->Operation.Create.Information = FILE_EXISTS;

			if (disposition == FILE_OPEN_IF ||
				disposition == FILE_OVERWRITE_IF) {
				eventInfo->Status = STATUS_SUCCESS;
				if (disposition == FILE_OPEN_IF) {
					eventInfo->Operation.Create.Information = FILE_OPENED;
				} else {
					eventInfo->Operation.Create.Information = FILE_OVERWRITTEN;
				}
			}
		}
	} else {
		
		//DbgPrint("status = %d\n", status);

		eventInfo->Status = STATUS_SUCCESS;
		eventInfo->Operation.Create.Information = FILE_OPENED;

		if (disposition == FILE_CREATE ||
			disposition == FILE_OPEN_IF ||
			disposition == FILE_OVERWRITE_IF) {
			eventInfo->Operation.Create.Information = FILE_CREATED;
		}

		if (fileInfo.IsDirectory)
			eventInfo->Operation.Create.Flags |= DOKAN_FILE_DIRECTORY;
	}
	
	SendEventInformation(Handle, eventInfo, length, DokanInstance);
	free(eventInfo);
	return;
}
