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

VOID DispatchClose(HANDLE Handle, PEVENT_CONTEXT EventContext,
                   PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  DOKAN_FILE_INFO fileInfo;
  PDOKAN_OPEN_INFO openInfo;
  ULONG sizeOfEventInfo = DispatchGetEventInformationLength(0);

  UNREFERENCED_PARAMETER(Handle);

  CheckFileName(EventContext->Operation.Close.FileName);

  eventInfo = DispatchCommon(EventContext, sizeOfEventInfo, DokanInstance,
                             &fileInfo, &openInfo);

  eventInfo->Status = STATUS_SUCCESS; // return success at any case

  DbgPrint("###Close %04d\n", openInfo != NULL ? openInfo->EventId : -1);
  
  // Driver has simply notifying us of the Close request which he has
  // already completed at this stage. Driver is not expecting us
  // to reply from this so there is no need to send an EVENT_INFORMATION.

  if (openInfo != NULL) {
    EnterCriticalSection(&DokanInstance->CriticalSection);
    openInfo->FileName = _wcsdup(EventContext->Operation.Close.FileName);
    openInfo->OpenCount--;
    LeaveCriticalSection(&DokanInstance->CriticalSection);
  }
  ReleaseDokanOpenInfo(eventInfo, &fileInfo, DokanInstance);
  free(eventInfo);
}
