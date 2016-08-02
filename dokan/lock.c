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

void EndDispatchLockCommon(DOKAN_IO_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result = ioEvent->EventResult;

	assert(result->BufferLength == 0);

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: EndDispatchLockCommon() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	result->Status = ResultStatus;

	SendIoEventResult(ioEvent);
}

void BeginDispatchLock(DOKAN_IO_EVENT *EventInfo) {

  DOKAN_LOCK_FILE_EVENT *lockFileEvent = &EventInfo->EventInfo.LockFile;
  DOKAN_UNLOCK_FILE_EVENT *unlockFileEvent = &EventInfo->EventInfo.UnlockFile;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  DbgPrint("###Lock file handle = 0x%p, eventID = %04d\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1);

  assert(EventInfo->DokanOpenInfo);
  assert(EventInfo->ProcessingContext == NULL);

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Lock.FileName);

  CreateDispatchCommon(EventInfo, 0);

  switch (EventInfo->KernelInfo.EventContext.MinorFunction) {

  case IRP_MN_LOCK:

	  assert((void*)lockFileEvent == (void*)EventInfo);
	  
	  if (EventInfo->DokanInstance->DokanOperations->LockFile) {
		  
		  lockFileEvent->DokanFileInfo = &EventInfo->DokanFileInfo;
		  lockFileEvent->FileName = EventInfo->KernelInfo.EventContext.Operation.Lock.FileName;
		  lockFileEvent->ByteOffset = EventInfo->KernelInfo.EventContext.Operation.Lock.ByteOffset.QuadPart;
		  lockFileEvent->Length = EventInfo->KernelInfo.EventContext.Operation.Lock.Length.QuadPart;
		  lockFileEvent->Key = EventInfo->KernelInfo.EventContext.Operation.Lock.Key;

		  status = EventInfo->DokanInstance->DokanOperations->LockFile(lockFileEvent);
    }

    break;
  case IRP_MN_UNLOCK_ALL:
    break;
  case IRP_MN_UNLOCK_ALL_BY_KEY:
    break;
  case IRP_MN_UNLOCK_SINGLE:

    if (EventInfo->DokanInstance->DokanOperations->UnlockFile) {

		assert((void*)unlockFileEvent == (void*)EventInfo);

		unlockFileEvent->DokanFileInfo = &EventInfo->DokanFileInfo;
		unlockFileEvent->FileName = EventInfo->KernelInfo.EventContext.Operation.Lock.FileName;
		unlockFileEvent->ByteOffset = EventInfo->KernelInfo.EventContext.Operation.Lock.ByteOffset.QuadPart;
		unlockFileEvent->Length = EventInfo->KernelInfo.EventContext.Operation.Lock.Length.QuadPart;
		unlockFileEvent->Key = EventInfo->KernelInfo.EventContext.Operation.Lock.Key;

      status = EventInfo->DokanInstance->DokanOperations->UnlockFile(unlockFileEvent);
    }

    break;
  default:
    DbgPrint("unkown lock function %d\n", EventInfo->KernelInfo.EventContext.MinorFunction);
	break;
  }

  if(status != STATUS_PENDING) {

	  EndDispatchLockCommon(EventInfo, status);
  }
}

void DOKANAPI DokanEndDispatchLockFile(DOKAN_LOCK_FILE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	EndDispatchLockCommon((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}

void DOKANAPI DokanEndDispatchUnlockFile(DOKAN_LOCK_FILE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	EndDispatchLockCommon((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}