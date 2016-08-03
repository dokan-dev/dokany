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

void BeginDispatchFlush(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_FLUSH_BUFFERS_EVENT *flushBuffers = &EventInfo->EventInfo.FlushBuffers;
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	DbgPrint("###Flush file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
		EventInfo->DokanOpenInfo,
		EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
		EventInfo);

	assert(EventInfo->DokanOpenInfo);
	assert((void*)flushBuffers == (void*)EventInfo);
	assert(EventInfo->ProcessingContext == NULL);

	CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Flush.FileName);

	CreateDispatchCommon(EventInfo, 0);

	if(EventInfo->DokanInstance->DokanOperations->FlushFileBuffers) {

		flushBuffers->DokanFileInfo = &EventInfo->DokanFileInfo;
		flushBuffers->FileName = EventInfo->KernelInfo.EventContext.Operation.Flush.FileName;

		status = EventInfo->DokanInstance->DokanOperations->FlushFileBuffers(flushBuffers);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchFlush(flushBuffers, status);
	}
}

void DOKANAPI DokanEndDispatchFlush(DOKAN_FLUSH_BUFFERS_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchFlush() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	ioEvent->EventResult->Status = ResultStatus;

	SendIoEventResult(ioEvent);
}