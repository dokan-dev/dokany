/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2010 Hiroki Asakawa info@dokan-dev.net

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
DispatchQuerySecurity(
	HANDLE			Handle,
	PEVENT_CONTEXT	EventContext,
	PDOKAN_INSTANCE	DokanInstance)
{
	PEVENT_INFORMATION	eventInfo;
	DOKAN_FILE_INFO		fileInfo;
	PDOKAN_OPEN_INFO	openInfo;
	ULONG	eventInfoLength;
	ULONG	securityDescLength;
	int		status = -ERROR_CALL_NOT_IMPLEMENTED;
	ULONG	lengthNeeded = 0;

	eventInfoLength = sizeof(EVENT_INFORMATION) - 8 + EventContext->Security.BufferLength;
	CheckFileName(EventContext->Security.FileName);

	eventInfo = DispatchCommon(EventContext, eventInfoLength, DokanInstance, &fileInfo, &openInfo);
	

	if (DOKAN_SECURITY_SUPPORTED_VERSION <= DokanInstance->DokanOptions->Version &&
		DokanInstance->DokanOperations->GetFileSecurity) {
		status = DokanInstance->DokanOperations->GetFileSecurity(
					EventContext->Security.FileName,
					&EventContext->Security.SecurityInformation,
					&eventInfo->Buffer,
					EventContext->Security.BufferLength,
					&lengthNeeded,
					&fileInfo);
	}

	if (status < 0) {
		int error = status * -1;
		if (error == ERROR_INSUFFICIENT_BUFFER && lengthNeeded > 0) {
			eventInfo->Status = STATUS_BUFFER_OVERFLOW;
			eventInfo->BufferLength = lengthNeeded;
		} else if (error == ERROR_ACCESS_DENIED) {
			eventInfo->Status = STATUS_ACCESS_DENIED;
			eventInfo->BufferLength = 0;
		} else if (error == ERROR_CALL_NOT_IMPLEMENTED) {
			eventInfo->Status = STATUS_NOT_IMPLEMENTED;
			eventInfo->BufferLength = 0;
		} else {
			eventInfo->Status = STATUS_INVALID_PARAMETER;
			eventInfo->BufferLength = 0;
		}
	} else {
		if (EventContext->Security.BufferLength < lengthNeeded) {
			// Filesystem Application should return -ERROR_BUFFER_OVERFLOW in this case.
			eventInfo->BufferLength = lengthNeeded;
			eventInfo->Status = STATUS_BUFFER_OVERFLOW;
		} else {
			eventInfo->BufferLength = lengthNeeded;
			eventInfo->Status = STATUS_SUCCESS;
		}
	}

	SendEventInformation(Handle, eventInfo, eventInfoLength, DokanInstance);
	free(eventInfo);
}


VOID
DispatchSetSecurity(
	HANDLE			Handle,
	PEVENT_CONTEXT	EventContext,
	PDOKAN_INSTANCE	DokanInstance)
{
	PEVENT_INFORMATION	eventInfo;
	DOKAN_FILE_INFO		fileInfo;
	PDOKAN_OPEN_INFO	openInfo;
	ULONG	eventInfoLength;
	int		status = -1;
	PSECURITY_DESCRIPTOR	securityDescriptor;
	
	eventInfoLength = sizeof(EVENT_INFORMATION);
	CheckFileName(EventContext->SetSecurity.FileName);

	eventInfo = DispatchCommon(EventContext, eventInfoLength, DokanInstance, &fileInfo, &openInfo);
	
	securityDescriptor = (PCHAR)EventContext + EventContext->SetSecurity.BufferOffset;

	if (DOKAN_SECURITY_SUPPORTED_VERSION <= DokanInstance->DokanOptions->Version &&
		DokanInstance->DokanOperations->SetFileSecurity) {
		status = DokanInstance->DokanOperations->SetFileSecurity(
					EventContext->SetSecurity.FileName,
					&EventContext->SetSecurity.SecurityInformation,
					securityDescriptor,
					EventContext->SetSecurity.BufferLength,
					&fileInfo);
	}

	if (status < 0) {
		eventInfo->Status = STATUS_INVALID_PARAMETER;
		eventInfo->BufferLength = 0;
	} else {
		eventInfo->Status = STATUS_SUCCESS;
		eventInfo->BufferLength = 0;
	}

	SendEventInformation(Handle, eventInfo, eventInfoLength, DokanInstance);
	free(eventInfo);
}


