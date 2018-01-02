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
#include "fileinfo.h"

VOID DispatchLock(HANDLE Handle, PEVENT_CONTEXT EventContext,
                  PDOKAN_INSTANCE DokanInstance) {
  DOKAN_FILE_INFO fileInfo;
  PEVENT_INFORMATION eventInfo;
  ULONG sizeOfEventInfo = sizeof(EVENT_INFORMATION);
  PDOKAN_OPEN_INFO openInfo;
  NTSTATUS status;

  CheckFileName(EventContext->Operation.Lock.FileName);

  eventInfo = DispatchCommon(EventContext, sizeOfEventInfo, DokanInstance,
                             &fileInfo, &openInfo);

  DbgPrint("###Lock %04d\n", openInfo != NULL ? openInfo->EventId : -1);

  eventInfo->Status = STATUS_NOT_IMPLEMENTED;

  switch (EventContext->MinorFunction) {
  case IRP_MN_LOCK:
    if (DokanInstance->DokanOperations->LockFile) {

      status = DokanInstance->DokanOperations->LockFile(
          EventContext->Operation.Lock.FileName,
          EventContext->Operation.Lock.ByteOffset.QuadPart,
          EventContext->Operation.Lock.Length.QuadPart,
          // EventContext->Operation.Lock.Key,
          &fileInfo);

      if (status != STATUS_NOT_IMPLEMENTED) {
        eventInfo->Status =
            status != STATUS_SUCCESS ? STATUS_LOCK_NOT_GRANTED : STATUS_SUCCESS;
      }
    }
    break;
  case IRP_MN_UNLOCK_ALL:
    break;
  case IRP_MN_UNLOCK_ALL_BY_KEY:
    break;
  case IRP_MN_UNLOCK_SINGLE:
    if (DokanInstance->DokanOperations->UnlockFile) {

      status = DokanInstance->DokanOperations->UnlockFile(
          EventContext->Operation.Lock.FileName,
          EventContext->Operation.Lock.ByteOffset.QuadPart,
          EventContext->Operation.Lock.Length.QuadPart,
          // EventContext->Operation.Lock.Key,
          &fileInfo);

      if (status != STATUS_NOT_IMPLEMENTED) {
        eventInfo->Status =
            STATUS_SUCCESS; // always succeeds so it cannot fail ?
      }
    }
    break;
  default:
    DbgPrint("unkown lock function %d\n", EventContext->MinorFunction);
  }

  if (openInfo != NULL)
    openInfo->UserContext = fileInfo.Context;

  SendEventInformation(Handle, eventInfo, sizeOfEventInfo, DokanInstance);

  free(eventInfo);
}
