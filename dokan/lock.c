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
#include "fileinfo.h"

VOID DispatchLock(PDOKAN_IO_EVENT IoEvent) {
  NTSTATUS status;

  CheckFileName(IoEvent->EventContext->Operation.Lock.FileName);

  CreateDispatchCommon(IoEvent, 0, /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  DbgPrint("###Lock file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
           IoEvent->DokanOpenInfo,
           IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId
                                            : -1,
           IoEvent);

  IoEvent->EventResult->Status = STATUS_NOT_IMPLEMENTED;

  switch (IoEvent->EventContext->MinorFunction) {
  case IRP_MN_LOCK:
    if (IoEvent->DokanInstance->DokanOperations->LockFile) {

      status = IoEvent->DokanInstance->DokanOperations->LockFile(
          IoEvent->EventContext->Operation.Lock.FileName,
          IoEvent->EventContext->Operation.Lock.ByteOffset.QuadPart,
          IoEvent->EventContext->Operation.Lock.Length.QuadPart,
          // EventContext->Operation.Lock.Key,
          &IoEvent->DokanFileInfo);

      if (status != STATUS_NOT_IMPLEMENTED) {
        IoEvent->EventResult->Status =
            status != STATUS_SUCCESS ? STATUS_LOCK_NOT_GRANTED : STATUS_SUCCESS;
      }
    }
    break;
  case IRP_MN_UNLOCK_ALL:
    break;
  case IRP_MN_UNLOCK_ALL_BY_KEY:
    break;
  case IRP_MN_UNLOCK_SINGLE:
    if (IoEvent->DokanInstance->DokanOperations->UnlockFile) {

      status = IoEvent->DokanInstance->DokanOperations->UnlockFile(
          IoEvent->EventContext->Operation.Lock.FileName,
          IoEvent->EventContext->Operation.Lock.ByteOffset.QuadPart,
          IoEvent->EventContext->Operation.Lock.Length.QuadPart,
          // EventContext->Operation.Lock.Key,
          &IoEvent->DokanFileInfo);

      if (status != STATUS_NOT_IMPLEMENTED) {
        IoEvent->EventResult->Status =
            STATUS_SUCCESS; // always succeeds so it cannot fail ?
      }
    }
    break;
  default:
    DbgPrint("unknown lock function %d\n",
             IoEvent->EventContext->MinorFunction);
  }

  EventCompletion(IoEvent);
}
