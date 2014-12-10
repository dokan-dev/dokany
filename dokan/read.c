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
DispatchRead(
	HANDLE				Handle,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_INSTANCE		DokanInstance)
{
	PEVENT_INFORMATION		eventInfo;
	PDOKAN_OPEN_INFO		openInfo;
	ULONG					readLength = 0;
	int						status;
	DOKAN_FILE_INFO			fileInfo;
	ULONG					sizeOfEventInfo;
	
	sizeOfEventInfo = sizeof(EVENT_INFORMATION) - 8 + EventContext->Read.BufferLength;

	CheckFileName(EventContext->Read.FileName);

	eventInfo = DispatchCommon(
		EventContext, sizeOfEventInfo, DokanInstance, &fileInfo, &openInfo);

	DbgPrint("###Read %04d\n", openInfo != NULL ? openInfo->EventId : -1);

	if (DokanInstance->DokanOperations->ReadFile) {
		status = DokanInstance->DokanOperations->ReadFile(
						EventContext->Read.FileName,
						eventInfo->Buffer,
						EventContext->Read.BufferLength,
						&readLength,
						EventContext->Read.ByteOffset.QuadPart,
						&fileInfo);
	} else {
		status = -1;
	}

	openInfo->UserContext = fileInfo.Context;
	eventInfo->BufferLength = 0;

	if (status < 0) {
		eventInfo->Status = STATUS_INVALID_PARAMETER;
	} else if(readLength == 0) {
		eventInfo->Status = STATUS_END_OF_FILE;
	} else {
		eventInfo->Status = STATUS_SUCCESS;
		eventInfo->BufferLength = readLength;
		eventInfo->Read.CurrentByteOffset.QuadPart =
			EventContext->Read.ByteOffset.QuadPart + readLength;
	}

	SendEventInformation(Handle, eventInfo, sizeOfEventInfo, DokanInstance);
	free(eventInfo);
	return;
}
