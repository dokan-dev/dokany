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

VOID DispatchRead(PDOKAN_IO_EVENT IoEvent) {
  ULONG readLength = 0;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  CheckFileName(IoEvent->EventContext->Operation.Read.FileName);

  CreateDispatchCommon(IoEvent,
                       IoEvent->EventContext->Operation.Read.BufferLength,
                       /*UseExtraMemoryPool=*/TRUE,
                       /*ClearNonPoolBuffer=*/FALSE);

  DbgPrint("###Read file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
           IoEvent->DokanOpenInfo,
           IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId
                                          : -1,
           IoEvent);

  if (IoEvent->DokanInstance->DokanOperations->ReadFile) {
    status = IoEvent->DokanInstance->DokanOperations->ReadFile(
        IoEvent->EventContext->Operation.Read.FileName,
        IoEvent->EventResult->Buffer,
        IoEvent->EventContext->Operation.Read.BufferLength, &readLength,
        IoEvent->EventContext->Operation.Read.ByteOffset.QuadPart,
        &IoEvent->DokanFileInfo);
  }

  IoEvent->EventResult->BufferLength = 0;
  IoEvent->EventResult->Status = status;

  if (status == STATUS_SUCCESS) {
    if (readLength == 0) {
      IoEvent->EventResult->Status = STATUS_END_OF_FILE;
    } else {
      IoEvent->EventResult->BufferLength = readLength;
      IoEvent->EventResult->Operation.Read.CurrentByteOffset.QuadPart =
          IoEvent->EventContext->Operation.Read.ByteOffset.QuadPart +
          readLength;
    }
  }

  EventCompletion(IoEvent);
}
