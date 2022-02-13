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

VOID DispatchCleanup(PDOKAN_IO_EVENT IoEvent) {
  CheckFileName(IoEvent->EventContext->Operation.Cleanup.FileName);

  CreateDispatchCommon(IoEvent, 0, /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  IoEvent->EventResult->Status = STATUS_SUCCESS; // return success at any case

  DbgPrint("###Cleanup file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
           IoEvent->DokanOpenInfo,
           IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId
                                          : -1,
           IoEvent);

  if (IoEvent->DokanInstance->DokanOperations->Cleanup) {
    // ignore return value
    IoEvent->DokanInstance->DokanOperations->Cleanup(
        IoEvent->EventContext->Operation.Cleanup.FileName,
        &IoEvent->DokanFileInfo);
  }

  EventCompletion(IoEvent);
}
