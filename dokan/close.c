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

void DispatchClose(DOKAN_IO_EVENT *EventInfo) {

  PDOKAN_INSTANCE dokan = EventInfo->DokanInstance;
  DOKAN_CLOSE_FILE_EVENT *closeFileEvent = &EventInfo->EventInfo.CloseFile;

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Close.FileName);

  CreateDispatchCommon(EventInfo, 0);

  EventInfo->EventResult->Status = STATUS_SUCCESS; // return success at any case
  
  DbgPrint("###Close file handle = 0x%p, eventID = %04d\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1);

  if (dokan->DokanOperations->CloseFile) {

	  closeFileEvent->DokanFileInfo = &EventInfo->DokanFileInfo;
	  closeFileEvent->FileName = EventInfo->KernelInfo.EventContext.Operation.Close.FileName;

	  dokan->DokanOperations->CloseFile(closeFileEvent);
  }

  if(EventInfo->DokanOpenInfo) {

	  PushFileOpenInfo(EventInfo->DokanOpenInfo);
	  EventInfo->DokanOpenInfo = NULL;
  }

  // do not send it to the driver
}
