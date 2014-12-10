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


VOID
DispatchCreate(
	HANDLE				Handle,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_INSTANCE		DokanInstance)
{
	static eventId = 0;
	ULONG					length	  = sizeof(EVENT_INFORMATION);
	PEVENT_INFORMATION		eventInfo = (PEVENT_INFORMATION)malloc(length);
	int						status;
	DOKAN_FILE_INFO			fileInfo;
	DWORD					disposition;
	PDOKAN_OPEN_INFO		openInfo;
	BOOL					directoryRequested = FALSE;
	DWORD					options;

	CheckFileName(EventContext->Create.FileName);

	RtlZeroMemory(eventInfo, length);
	RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));

	eventInfo->BufferLength = 0;
	eventInfo->SerialNumber = EventContext->SerialNumber;

	fileInfo.ProcessId = EventContext->ProcessId;
	fileInfo.DokanOptions = DokanInstance->DokanOptions;

	// DOKAN_OPEN_INFO is structure for a opened file
	// this will be freed by Close
	openInfo = malloc(sizeof(DOKAN_OPEN_INFO));
	ZeroMemory(openInfo, sizeof(DOKAN_OPEN_INFO));
	openInfo->OpenCount = 2;
	openInfo->EventContext = EventContext;
	openInfo->DokanInstance = DokanInstance;
	fileInfo.DokanContext = (ULONG64)openInfo;

	// pass it to driver and when the same handle is used get it back
	eventInfo->Context = (ULONG64)openInfo;

	// The high 8 bits of this parameter correspond to the Disposition parameter
	disposition = (EventContext->Create.CreateOptions >> 24) & 0x000000ff;

	status = -1; // in case being not dispatched
	
	// The low 24 bits of this member correspond to the CreateOptions parameter
	options = EventContext->Create.CreateOptions & FILE_VALID_OPTION_FLAGS;
	//DbgPrint("Create.CreateOptions 0x%x\n", options);

	// to open directory
	// even if this flag is not specifed, 
	// there is a case to open a directory
	if (options & FILE_DIRECTORY_FILE) {
		//DbgPrint("FILE_DIRECTORY_FILE\n");
		directoryRequested = TRUE;
	}

	// to open no directory file
	// event if this flag is not specified,
	// there is a case to open non directory file
	if (options & FILE_NON_DIRECTORY_FILE) {
		//DbgPrint("FILE_NON_DIRECTORY_FILE\n");
	}

	if (options & FILE_DELETE_ON_CLOSE) {
		EventContext->Create.FileAttributes |= FILE_FLAG_DELETE_ON_CLOSE;
	}

	DbgPrint("###Create %04d\n", eventId);
	//DbgPrint("### OpenInfo %X\n", openInfo);
	openInfo->EventId = eventId++;

	// make a directory or open
	if (directoryRequested) {
		fileInfo.IsDirectory = TRUE;

		if (disposition == FILE_CREATE || disposition == FILE_OPEN_IF) {
			if (DokanInstance->DokanOperations->CreateDirectory) {
				status = DokanInstance->DokanOperations->CreateDirectory(
							EventContext->Create.FileName, &fileInfo);
			}
		} else if(disposition == FILE_OPEN) {
			if (DokanInstance->DokanOperations->OpenDirectory) {
				status = DokanInstance->DokanOperations->OpenDirectory(
							EventContext->Create.FileName, &fileInfo);
			}
		} else {
			DbgPrint("### Create other disposition : %d\n", disposition);
		}
	
	// open a file
	} else {
		DWORD creationDisposition = OPEN_EXISTING;
		fileInfo.IsDirectory = FALSE;
		DbgPrint("   CreateDisposition %X\n", disposition);
		switch(disposition) {
			case FILE_CREATE:
				creationDisposition = CREATE_NEW;
				break;
			case FILE_OPEN:
				creationDisposition = OPEN_EXISTING;
				break;
			case FILE_OPEN_IF:
				creationDisposition = OPEN_ALWAYS;
				break;
			case FILE_OVERWRITE:
				creationDisposition = TRUNCATE_EXISTING;
				break;
			case FILE_OVERWRITE_IF:
				creationDisposition = CREATE_ALWAYS;
				break;
			default:
				// TODO: should support FILE_SUPERSEDE ?
				DbgPrint("### Create other disposition : %d\n", disposition);
				break;
		}
		
		if(DokanInstance->DokanOperations->CreateFile) {
			status = DokanInstance->DokanOperations->CreateFile(
									EventContext->Create.FileName,
									EventContext->Create.DesiredAccess,
									EventContext->Create.ShareAccess,
									creationDisposition,
									EventContext->Create.FileAttributes,
									&fileInfo);
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


	if (status < 0) {

		int error = status * -1;
		
		DbgPrint("CreateFile status = %d\n", status);
		if (EventContext->Flags & SL_OPEN_TARGET_DIRECTORY) {
			DbgPrint("SL_OPEN_TARGET_DIRECTORY spcefied\n");
		}
		eventInfo->Create.Information = FILE_DOES_NOT_EXIST;

		switch(error) {
		case ERROR_FILE_NOT_FOUND:
			if (EventContext->Flags & SL_OPEN_TARGET_DIRECTORY)
				eventInfo->Status = STATUS_SUCCESS;
			else
				eventInfo->Status = STATUS_OBJECT_NAME_NOT_FOUND;
			break;
		case ERROR_PATH_NOT_FOUND:
			//if (EventContext->Flags & SL_OPEN_TARGET_DIRECTORY)
			//	eventInfo->Status = STATUS_SUCCESS;
			//else
			eventInfo->Status = STATUS_OBJECT_PATH_NOT_FOUND;
			break;
		case ERROR_ACCESS_DENIED:
			eventInfo->Status = STATUS_ACCESS_DENIED;
			break;
		case ERROR_SHARING_VIOLATION:
			eventInfo->Status = STATUS_SHARING_VIOLATION;
			break;
		case ERROR_INVALID_NAME:
			eventInfo->Status = STATUS_OBJECT_NAME_NOT_FOUND;
			break;
		case ERROR_FILE_EXISTS:
		case ERROR_ALREADY_EXISTS:		
			eventInfo->Status = STATUS_OBJECT_NAME_COLLISION;
			eventInfo->Create.Information = FILE_EXISTS;
			break;
		case ERROR_PRIVILEGE_NOT_HELD:
			eventInfo->Status = STATUS_PRIVILEGE_NOT_HELD;
			break;
		case ERROR_NOT_READY:
			eventInfo->Status = STATUS_DEVICE_NOT_READY;
			break;
		default:
			eventInfo->Status = STATUS_INVALID_PARAMETER;
			DbgPrint("Create got unknown error code %d\n", error);
		}


		if (eventInfo->Status != STATUS_SUCCESS) {
			// Needs to free openInfo because Close is never called.
			free(openInfo);
			eventInfo->Context = 0;
		}

	} else {
		
		//DbgPrint("status = %d\n", status);

		eventInfo->Status = STATUS_SUCCESS;
		eventInfo->Create.Information = FILE_OPENED;

		if (disposition == FILE_CREATE ||
			disposition == FILE_OPEN_IF ||
			disposition == FILE_OVERWRITE_IF) {

			if (status != ERROR_ALREADY_EXISTS) {
				eventInfo->Create.Information = FILE_CREATED;
			}
		}

		if ((disposition == FILE_OVERWRITE_IF || disposition == FILE_OVERWRITE) &&
			eventInfo->Create.Information != FILE_CREATED) {
			
			eventInfo->Create.Information = FILE_OVERWRITTEN;
		}

		if (fileInfo.IsDirectory)
			eventInfo->Create.Flags |= DOKAN_FILE_DIRECTORY;
	}
	
	SendEventInformation(Handle, eventInfo, length, DokanInstance);
	free(eventInfo);
	return;
}
