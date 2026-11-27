/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2025 Google, Inc.
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

HANDLE DokanOpenRequestorTokenForEvent(_In_ PDOKAN_IO_EVENT IoEvent) {
  if (IoEvent == NULL || IoEvent->EventContext == NULL ||
      IoEvent->DokanInstance == NULL) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }

  HANDLE device = IoEvent->DokanInstance->Device;
  if (device == NULL || device == INVALID_HANDLE_VALUE) {
    SetLastError(ERROR_INVALID_HANDLE);
    return INVALID_HANDLE_VALUE;
  }

  OVERLAPPED ov;
  ZeroMemory(&ov, sizeof(ov));
  ov.hEvent = CreateEventW(NULL, /*bManualReset=*/TRUE,
                           /*bInitialState=*/FALSE, NULL);
  if (ov.hEvent == NULL) {
    return INVALID_HANDLE_VALUE;
  }

  EVENT_INFORMATION tokenEvent;
  ZeroMemory(&tokenEvent, sizeof(tokenEvent));
  tokenEvent.SerialNumber = IoEvent->EventContext->SerialNumber;

  DWORD returnedLength = 0;
  HANDLE token = INVALID_HANDLE_VALUE;
  DWORD lastError = ERROR_SUCCESS;

  BOOL ioctlOk = DeviceIoControl(device, FSCTL_GET_ACCESS_TOKEN, &tokenEvent,
                                 sizeof(tokenEvent), &tokenEvent,
                                 sizeof(tokenEvent), &returnedLength, &ov);
  if (!ioctlOk) {
    lastError = GetLastError();
    if (lastError == ERROR_IO_PENDING) {
      if (GetOverlappedResult(device, &ov, &returnedLength, /*bWait=*/TRUE)) {
        ioctlOk = TRUE;
        lastError = ERROR_SUCCESS;
      } else {
        lastError = GetLastError();
      }
    }
  }

  if (ioctlOk) {
    if (tokenEvent.Operation.AccessToken.Handle != NULL &&
        tokenEvent.Operation.AccessToken.Handle != INVALID_HANDLE_VALUE) {
      token = tokenEvent.Operation.AccessToken.Handle;
    } else {
      lastError = ERROR_INVALID_HANDLE;
    }
  }

  CloseHandle(ov.hEvent);
  if (token == INVALID_HANDLE_VALUE) {
    SetLastError(lastError);
  }
  return token;
}

HANDLE DOKANAPI DokanOpenRequestorToken(PDOKAN_FILE_INFO FileInfo) {
  PDOKAN_IO_EVENT ioEvent;
  HANDLE handle;

  if (FileInfo == NULL || FileInfo->DokanContext == 0) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }

  ioEvent = (PDOKAN_IO_EVENT)(UINT_PTR)FileInfo->DokanContext;
  if (ioEvent->EventContext == NULL || ioEvent->DokanInstance == NULL) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }

  if (ioEvent->EventContext->MajorFunction != IRP_MJ_CREATE) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }

  handle = DokanOpenRequestorTokenForEvent(ioEvent);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    DbgPrintW(L"Failed to OpenRequestorToken for %04d\n",
              ioEvent->EventContext->SerialNumber);
    SetLastError(err);
  }
  return handle;
}