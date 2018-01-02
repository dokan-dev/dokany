/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2018 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

VOID DispatchCleanup(HANDLE Handle, PEVENT_CONTEXT EventContext,
                     PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  DOKAN_FILE_INFO fileInfo;
  PDOKAN_OPEN_INFO openInfo;
  ULONG sizeOfEventInfo = sizeof(EVENT_INFORMATION);

  CheckFileName(EventContext->Operation.Cleanup.FileName);

  eventInfo = DispatchCommon(EventContext, sizeOfEventInfo, DokanInstance,
                             &fileInfo, &openInfo);

  eventInfo->Status = STATUS_SUCCESS; // return success at any case

  DbgPrint("###Cleanup %04d\n", openInfo != NULL ? openInfo->EventId : -1);

  if (DokanInstance->DokanOperations->Cleanup) {
    // ignore return value
    DokanInstance->DokanOperations->Cleanup(
        EventContext->Operation.Cleanup.FileName, &fileInfo);
  }

  if (openInfo != NULL)
    openInfo->UserContext = fileInfo.Context;

  SendEventInformation(Handle, eventInfo, sizeOfEventInfo, DokanInstance);

  free(eventInfo);
}
