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

#include <assert.h>

void BeginDispatchRead(DOKAN_IO_EVENT *EventInfo) {

  PDOKAN_INSTANCE dokan = EventInfo->DokanInstance;
  DOKAN_READ_FILE_EVENT *readFileEvent = &EventInfo->EventInfo.ReadFile;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  assert(EventInfo->DokanOpenInfo);
  assert((void*)readFileEvent == (void*)EventInfo);
  assert(EventInfo->ProcessingContext == NULL);

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Read.FileName);

  DbgPrint("###Read file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
	  EventInfo);

  CreateDispatchCommon(EventInfo, EventInfo->KernelInfo.EventContext.Operation.Read.BufferLength);

  if (dokan->DokanOperations->ReadFile) {

	readFileEvent->DokanFileInfo = &EventInfo->DokanFileInfo;
	readFileEvent->FileName = EventInfo->KernelInfo.EventContext.Operation.Read.FileName;
	readFileEvent->Buffer = EventInfo->EventResult->Buffer;
	readFileEvent->Offset = EventInfo->KernelInfo.EventContext.Operation.Read.ByteOffset.QuadPart;
	
	assert(readFileEvent->NumberOfBytesRead == 0);
	
	readFileEvent->NumberOfBytesToRead = EventInfo->KernelInfo.EventContext.Operation.Read.BufferLength;

    status = dokan->DokanOperations->ReadFile(readFileEvent);
  }
  else {

    status = STATUS_NOT_IMPLEMENTED;
  }

  if(status != STATUS_PENDING) {

	  DokanEndDispatchRead(readFileEvent, status);
  }
}

void DOKANAPI DokanEndDispatchRead(DOKAN_READ_FILE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result = ioEvent->EventResult;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchRead() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	result->Status = ResultStatus;
	result->BufferLength = EventInfo->NumberOfBytesRead;
	result->Operation.Read.CurrentByteOffset.QuadPart = EventInfo->Offset + EventInfo->NumberOfBytesRead;

	if(ResultStatus == STATUS_SUCCESS && EventInfo->NumberOfBytesRead == 0) {

		result->Status = STATUS_END_OF_FILE;
	}

	SendIoEventResult(ioEvent);
}