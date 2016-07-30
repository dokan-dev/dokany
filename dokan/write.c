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

VOID SendWriteRequest(HANDLE Handle, PEVENT_INFORMATION EventInfo,
                      ULONG EventLength, PVOID Buffer, ULONG BufferLength) {
  BOOL status;
  ULONG returnedLength;

  DbgPrint("SendWriteRequest\n");

  status = DeviceIoControl(Handle,            // Handle to device
                           IOCTL_EVENT_WRITE, // IO Control code
                           EventInfo,         // Input Buffer to driver.
                           EventLength,     // Length of input buffer in bytes.
                           Buffer,          // Output Buffer from driver.
                           BufferLength,    // Length of output buffer in bytes.
                           &returnedLength, // Bytes placed in buffer.
                           NULL             // synchronous call
                           );

  if (!status) {
    DWORD errorCode = GetLastError();
    DbgPrint("Ioctl failed with code %d\n", errorCode);
  }

  DbgPrint("SendWriteRequest got %d bytes\n", returnedLength);
}

VOID DispatchWrite(HANDLE Handle, PEVENT_CONTEXT EventContext,
                   PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  PDOKAN_OPEN_INFO openInfo;
  ULONG writtenLength = 0;
  NTSTATUS status;
  DOKAN_FILE_INFO fileInfo;
  BOOL bufferAllocated = FALSE;
  ULONG sizeOfEventInfo = sizeof(EVENT_INFORMATION);

  eventInfo = DispatchCommon(EventContext, sizeOfEventInfo, DokanInstance,
                             &fileInfo, &openInfo);

  // Since driver requested bigger memory,
  // allocate enough memory and send it to driver
  if (EventContext->Operation.Write.RequestLength > 0) {
    ULONG contextLength = EventContext->Operation.Write.RequestLength;
    PEVENT_CONTEXT contextBuf = (PEVENT_CONTEXT)malloc(contextLength);
    if (contextBuf == NULL) {
      free(eventInfo);
      return;
    }
    SendWriteRequest(Handle, eventInfo, sizeOfEventInfo, contextBuf,
                     contextLength);
    EventContext = contextBuf;
    bufferAllocated = TRUE;
  }

  CheckFileName(EventContext->Operation.Write.FileName);

  DbgPrint("###WriteFile %04d\n", openInfo != NULL ? openInfo->EventId : -1);

  if (DokanInstance->DokanOperations->WriteFile) {
    status = DokanInstance->DokanOperations->WriteFile(
        EventContext->Operation.Write.FileName,
        (PCHAR)EventContext + EventContext->Operation.Write.BufferOffset,
        EventContext->Operation.Write.BufferLength, &writtenLength,
        EventContext->Operation.Write.ByteOffset.QuadPart, &fileInfo);
  } else {
    status = STATUS_NOT_IMPLEMENTED;
  }

  if (openInfo != NULL)
    openInfo->UserContext = fileInfo.Context;
  eventInfo->BufferLength = 0;

  if (status == STATUS_SUCCESS) {
    eventInfo->Status = status;
    eventInfo->BufferLength = writtenLength;
    eventInfo->Operation.Write.CurrentByteOffset.QuadPart =
        EventContext->Operation.Write.ByteOffset.QuadPart + writtenLength;
  } else {
    eventInfo->Status = STATUS_INVALID_PARAMETER;
  }

  SendEventInformation(Handle, eventInfo, sizeOfEventInfo, DokanInstance);
  free(eventInfo);

  if (bufferAllocated)
    free(EventContext);

  return;
}
