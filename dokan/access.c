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

HANDLE DOKANAPI DokanOpenRequestorToken(PDOKAN_FILE_INFO FileInfo) {
  EVENT_INFORMATION eventInfo = {0};
  ULONG returnedLength;
  PDOKAN_IO_EVENT ioEvent;
  WCHAR rawDeviceName[MAX_PATH];

  if (FileInfo == NULL || FileInfo->DokanContext == 0) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }
  ioEvent = (PDOKAN_IO_EVENT)(UINT_PTR)FileInfo->DokanContext;
  if (ioEvent->EventContext == NULL || ioEvent->DokanInstance == NULL ||
      ioEvent->EventContext->MajorFunction != IRP_MJ_CREATE) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }

  eventInfo.SerialNumber = ioEvent->EventContext->SerialNumber;
  GetRawDeviceName(ioEvent->DokanInstance->DeviceName, rawDeviceName, MAX_PATH);
  if (!SendToDevice(rawDeviceName, FSCTL_GET_ACCESS_TOKEN, &eventInfo,
                    sizeof(eventInfo), &eventInfo, sizeof(eventInfo),
                    &returnedLength)) {
    return INVALID_HANDLE_VALUE;
  }
  return eventInfo.Operation.AccessToken.Handle;
}
