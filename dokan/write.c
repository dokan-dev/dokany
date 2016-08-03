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

NTSTATUS SendWriteRequest(DOKAN_IO_EVENT *EventInfo) {
  
  DOKAN_IO_EVENT *writeEvent;
  DOKAN_OVERLAPPED *overlapped;
  DWORD lastError;

  DbgPrint("Dokan Information: Requesting write buffer of size %u.\n", EventInfo->KernelInfo.EventContext.Operation.Write.RequestLength);

  assert(EventInfo->EventResult);
  assert(EventInfo->EventResultSize > 0);

  overlapped = PopOverlapped();

  if(!overlapped) {

	  DbgPrint("Failed to allocate overlapped.\n");

	  return STATUS_INTERNAL_ERROR;
  }

  writeEvent = (DOKAN_IO_EVENT*)DokanMalloc(DOKAN_IO_EVENT_ALLOC_SIZE(EventInfo->KernelInfo.EventContext.Operation.Write.RequestLength));

  if(!writeEvent) {

	  DbgPrint("Dokan Error: Failed to allocate memory for write operation.\n");
	  
	  PushOverlapped(overlapped);

	  return STATUS_NO_MEMORY;
  }

  RtlZeroMemory(writeEvent, EventInfo->KernelInfo.EventContext.Operation.Write.RequestLength);
  writeEvent->DokanInstance = EventInfo->DokanInstance;

  overlapped->InputPayload = EventInfo;
  overlapped->OutputPayload = writeEvent;
  overlapped->PayloadType = DOKAN_OVERLAPPED_TYPE_IOEVENT_WRITE_SIZE;
  overlapped->Flags = EventInfo->Flags;

  StartThreadpoolIo(EventInfo->DokanInstance->ThreadInfo.IoCompletion);

  if(!DeviceIoControl(EventInfo->DokanInstance->Device,					// Handle to device
	  IOCTL_EVENT_WRITE,												// IO Control code
	  EventInfo->EventResult,											// Input Buffer to driver.
	  EventInfo->EventResultSize,										// Length of input buffer in bytes.
	  writeEvent->KernelInfo.EventContextBuffer,						// Output Buffer from driver.
	  EventInfo->KernelInfo.EventContext.Operation.Write.RequestLength,	// Length of output buffer in bytes.
	  NULL,																// Bytes placed in buffer.
	  (LPOVERLAPPED)overlapped											// asynchronous call
  )) {

	  lastError = GetLastError();

	  if(lastError != ERROR_IO_PENDING) {

		  DbgPrint("Dokan Error: Dokan device ioctl failed for wait with code %d.\n", lastError);

		  CancelThreadpoolIo(EventInfo->DokanInstance->ThreadInfo.IoCompletion);

		  PushOverlapped(overlapped);

		  DokanFree(writeEvent);

		  return DokanNtStatusFromWin32(lastError);
	  }
  }

  return STATUS_SUCCESS;
}

void BeginDispatchWrite(DOKAN_IO_EVENT *EventInfo) {

  DOKAN_WRITE_FILE_EVENT *writeFileEvent = &EventInfo->EventInfo.WriteFile;
  PDOKAN_INSTANCE dokan = EventInfo->DokanInstance;
  NTSTATUS status;

  assert(EventInfo->DokanOpenInfo);
  assert((void*)writeFileEvent == (void*)EventInfo);
  assert(EventInfo->ProcessingContext == NULL);

  CreateDispatchCommon(EventInfo, 0);

  // Since driver requested bigger memory,
  // allocate enough memory and send it to driver
  if (EventInfo->KernelInfo.EventContext.Operation.Write.RequestLength > 0) {

	  if((status = SendWriteRequest(EventInfo)) != STATUS_SUCCESS) {

		  DokanEndDispatchWrite(writeFileEvent, status);
	  }

	  return;
  }

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
	result->BufferLength = EventInfo->NumberOfBytesWritten;
	result->Operation.Write.CurrentByteOffset.QuadPart = EventInfo->Offset + EventInfo->NumberOfBytesWritten;

	SendIoEventResult(ioEvent);
}