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
#include "fileinfo.h"

HANDLE DOKANAPI DokanOpenRequestorToken(PDOKAN_FILE_INFO FileInfo) {
  
  PDOKAN_IO_EVENT ioEvent = (PDOKAN_IO_EVENT)FileInfo->DokanContext;
  BOOL status;
  ULONG returnedLength;
  PEVENT_INFORMATION eventInfo;
  HANDLE handle = INVALID_HANDLE_VALUE;
  ULONG eventInfoSize;
  WCHAR rawDeviceName[MAX_PATH];

  if (ioEvent->DokanOpenInfo == NULL
	  || ioEvent->DokanInstance == NULL
	  || ioEvent->DokanFileInfo.DokanContext != (PVOID)ioEvent) {

    SetLastError(ERROR_INVALID_PARAMETER);

    return INVALID_HANDLE_VALUE;
  }

  if (ioEvent->KernelInfo.EventContext.MajorFunction != IRP_MJ_CREATE
	  && ioEvent->KernelInfo.EventContext.MajorFunction != IRP_MJ_SET_SECURITY) {

	SetLastError(ERROR_INVALID_PARAMETER);

    return INVALID_HANDLE_VALUE;
  }

  eventInfoSize = sizeof(EVENT_INFORMATION);
  eventInfo = (PEVENT_INFORMATION)DokanMalloc(eventInfoSize);

  if (eventInfo == NULL) {

	SetLastError(ERROR_OUTOFMEMORY);

    return INVALID_HANDLE_VALUE;
  }

  RtlZeroMemory(eventInfo, eventInfoSize);

  eventInfo->SerialNumber = ioEvent->KernelInfo.EventContext.SerialNumber;

  status = SendToDevice(
      GetRawDeviceName(ioEvent->DokanInstance->DeviceName, rawDeviceName, MAX_PATH),
      IOCTL_GET_ACCESS_TOKEN, eventInfo, eventInfoSize, eventInfo,
      eventInfoSize, &returnedLength);

  if (status) {

    handle = eventInfo->Operation.AccessToken.Handle;
  }
  else {

    DbgPrintW(L"IOCTL_GET_ACCESS_TOKEN failed\n");
  }

  DokanFree(eventInfo);

  return handle;
}