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
#include "dokan_pool.h"

#include <assert.h>


DWORD SendWriteRequest(PDOKAN_IO_EVENT IoEvent, ULONG WriteEventContextLength) {
  DWORD WrittenLength = 0;
  DOKAN_IO_BATCH *writeIoBatch =
      malloc((SIZE_T)FIELD_OFFSET(DOKAN_IO_BATCH, EventContext) +
             WriteEventContextLength);
  if (!writeIoBatch) {
    DokanDbgPrintW(L"Dokan Error: Failed to allocate IO event buffer.\n");
    return ERROR_NO_SYSTEM_RESOURCES;
  }
  ZeroMemory(writeIoBatch, (SIZE_T)FIELD_OFFSET(DOKAN_IO_BATCH, EventContext) +
                               WriteEventContextLength);
  writeIoBatch->DokanInstance = IoEvent->DokanInstance;
  PushIoBatchBuffer(IoEvent->IoBatch);
  IoEvent->IoBatch = writeIoBatch;
  IoEvent->EventContext = IoEvent->IoBatch->EventContext;

  if (!DeviceIoControl(
      IoEvent->DokanInstance->Device, // Handle to device
      FSCTL_EVENT_WRITE,              // IO Control code
      IoEvent->EventResult,           // Input Buffer to driver.
      IoEvent->EventResultSize,       // Length of input buffer in bytes.
      &writeIoBatch->EventContext[0], // Output Buffer from driver.
      WriteEventContextLength,        // Length of output buffer in bytes.
      &WrittenLength,                 // Bytes placed in buffer.
      NULL                            // asynchronous call
          )) {
    return GetLastError();
  }
  return 0;
}

VOID DispatchWrite(PDOKAN_IO_EVENT IoEvent) {
  ULONG writtenLength = 0;
  NTSTATUS status;

  CreateDispatchCommon(IoEvent, 0);

  CheckFileName(IoEvent->EventContext->Operation.Write.FileName);

  DbgPrint(
      "###WriteFile file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
      IoEvent->DokanOpenInfo,
      IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId : -1,
      IoEvent);

  // Since driver requested bigger memory,
  // allocate enough memory and send it to driver
  if (IoEvent->EventContext->Operation.Write.RequestLength > 0) {
    DWORD error = SendWriteRequest(IoEvent, IoEvent->EventContext->Operation.Write.RequestLength);
    if (error != ERROR_SUCCESS) {
      if (error == ERROR_OPERATION_ABORTED) {
        IoEvent->EventResult->Status = STATUS_CANCELLED;
        DbgPrint(
            "WriteFile Error : User should already canceled the operation. "
            "Return STATUS_CANCELLED. \n");
      } else {
        IoEvent->EventResult->Status = DokanNtStatusFromWin32(error);
        DbgPrint("Unknown SendWriteRequest Error : LastError from "
                 "SendWriteRequest = %lu. \nUnknown SendWriteRequest error : "
                 "EventContext had been destoryed. Status = %X. \n",
                 error, IoEvent->EventResult->Status);
      }
      EventCompletion(IoEvent);
      return;
    }
  }

  // for the case SendWriteRequest success
  if (IoEvent->DokanInstance->DokanOperations->WriteFile) {
    status = IoEvent->DokanInstance->DokanOperations->WriteFile(
        IoEvent->EventContext->Operation.Write.FileName,
        (PCHAR)IoEvent->EventContext +
            IoEvent->EventContext->Operation.Write.BufferOffset,
        IoEvent->EventContext->Operation.Write.BufferLength, &writtenLength,
        IoEvent->EventContext->Operation.Write.ByteOffset.QuadPart, &IoEvent->DokanFileInfo);
  } else {
    status = STATUS_NOT_IMPLEMENTED;
  }

  IoEvent->EventResult->Status = status;
  IoEvent->EventResult->BufferLength = 0;

  if (status == STATUS_SUCCESS) {
    IoEvent->EventResult->BufferLength = writtenLength;
    IoEvent->EventResult->Operation.Write.CurrentByteOffset.QuadPart =
        IoEvent->EventContext->Operation.Write.ByteOffset.QuadPart +
        writtenLength;
  }

  EventCompletion(IoEvent);
}
