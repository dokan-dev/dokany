/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.
  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

VOID DispatchFlush(PDOKAN_IO_EVENT IoEvent) {
  NTSTATUS status;

  CheckFileName(IoEvent->EventContext->Operation.Flush.FileName);

  CreateDispatchCommon(IoEvent, 0, /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  DbgPrint("###Flush file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
           IoEvent->DokanOpenInfo,
           IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId
                                          : -1,
           IoEvent);

  if (IoEvent->DokanInstance->DokanOperations->FlushFileBuffers) {
    status = IoEvent->DokanInstance->DokanOperations->FlushFileBuffers(
        IoEvent->EventContext->Operation.Flush.FileName, &IoEvent->DokanFileInfo);
  } else {
    status = STATUS_NOT_IMPLEMENTED;
  }

  if (status == STATUS_NOT_IMPLEMENTED) {
    IoEvent->EventResult->Status = STATUS_SUCCESS;
  } else {
    IoEvent->EventResult->Status =
        status != STATUS_SUCCESS ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
  }

  EventCompletion(IoEvent);
}
