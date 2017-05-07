/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2017 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

void DispatchCleanup(DOKAN_IO_EVENT *EventInfo) {

  PDOKAN_INSTANCE dokan = EventInfo->DokanInstance;
  DOKAN_CLEANUP_EVENT *cleanupFileEvent = &EventInfo->EventInfo.Cleanup;

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Cleanup.FileName);

  CreateDispatchCommon(EventInfo, 0);

  EventInfo->EventResult->Status = STATUS_SUCCESS; // return success at any case

  DbgPrint("###Cleanup file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
	  EventInfo);

  if (dokan->DokanOperations->Cleanup) {
    
	  cleanupFileEvent->DokanFileInfo = &EventInfo->DokanFileInfo;
	  cleanupFileEvent->FileName = EventInfo->KernelInfo.EventContext.Operation.Close.FileName;

	  dokan->DokanOperations->Cleanup(cleanupFileEvent);
  }

  SendIoEventResult(EventInfo);
}
