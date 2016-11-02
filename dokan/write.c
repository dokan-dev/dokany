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
#include <assert.h>

void BeginDispatchWrite(DOKAN_IO_EVENT *EventInfo) {

  DOKAN_WRITE_FILE_EVENT *writeFileEvent = &EventInfo->EventInfo.WriteFile;
  PDOKAN_INSTANCE dokan = EventInfo->DokanInstance;
  NTSTATUS status;

  assert(EventInfo->DokanOpenInfo);
  assert((void*)writeFileEvent == (void*)EventInfo);
  assert(EventInfo->ProcessingContext == NULL);

  CreateDispatchCommon(EventInfo, 0);

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Write.FileName);

  DbgPrint("###WriteFile file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
	  EventInfo);

  if (dokan->DokanOperations->WriteFile) {

	writeFileEvent->DokanFileInfo = &EventInfo->DokanFileInfo;
	writeFileEvent->FileName = EventInfo->KernelInfo.EventContext.Operation.Write.FileName;
	writeFileEvent->Buffer = ((PCHAR)&EventInfo->KernelInfo.EventContext) + EventInfo->KernelInfo.EventContext.Operation.Write.BufferOffset;
	writeFileEvent->Offset = EventInfo->KernelInfo.EventContext.Operation.Write.ByteOffset.QuadPart;
	writeFileEvent->NumberOfBytesToWrite = EventInfo->KernelInfo.EventContext.Operation.Write.BufferLength;

	assert(writeFileEvent->NumberOfBytesWritten == 0);

    status = dokan->DokanOperations->WriteFile(writeFileEvent);
  }
  else {
    
	status = STATUS_NOT_IMPLEMENTED;
  }

  if(status != STATUS_PENDING) {

	  DokanEndDispatchWrite(writeFileEvent, status);
  }
}

void DOKANAPI DokanEndDispatchWrite(DOKAN_WRITE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result;

	result = ioEvent->EventResult;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchWrite() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	result->Status = ResultStatus;
	result->BufferLength = 0;
	result->Operation.Write.CurrentByteOffset.QuadPart = EventInfo->Offset + EventInfo->NumberOfBytesWritten;
	result->Operation.Write.BytesWritten = EventInfo->NumberOfBytesWritten;

	SendIoEventResult(ioEvent);
}