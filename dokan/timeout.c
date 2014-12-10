/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2010 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

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

#include <process.h>
#include "dokani.h"

BOOL DOKANAPI
DokanResetTimeout(ULONG Timeout, PDOKAN_FILE_INFO FileInfo)
{
	BOOL	status;
	ULONG	returnedLength;
	PDOKAN_INSTANCE		instance;
	PDOKAN_OPEN_INFO	openInfo;
	PEVENT_CONTEXT		eventContext;
	PEVENT_INFORMATION	eventInfo;
	ULONG	eventInfoSize = sizeof(EVENT_INFORMATION);

	openInfo = (PDOKAN_OPEN_INFO)FileInfo->DokanContext;
	if (openInfo == NULL) {
		return FALSE;
	}

	eventContext = openInfo->EventContext;
	if (eventContext == NULL) {
		return FALSE;
	}

	instance = openInfo->DokanInstance;
	if (instance == NULL) {
		return FALSE;
	}

	eventInfo = (PEVENT_INFORMATION)malloc(eventInfoSize);
	RtlZeroMemory(eventInfo, eventInfoSize);

	eventInfo->SerialNumber = eventContext->SerialNumber;
	eventInfo->ResetTimeout.Timeout = Timeout;

	status = SendToDevice(
				GetRawDeviceName(instance->DeviceName),
				IOCTL_RESET_TIMEOUT,
				eventInfo,
				eventInfoSize,
				NULL,
				0,
				&returnedLength);
	free(eventInfo);
	return status;
}


DWORD WINAPI
DokanKeepAlive(
	PDOKAN_INSTANCE DokanInstance)
{
	HANDLE	device;
	ULONG	ReturnedLength;
	ULONG	returnedLength;
	BOOL	status;

	device = CreateFile(
				GetRawDeviceName(DokanInstance->DeviceName),
				GENERIC_READ | GENERIC_WRITE,       // dwDesiredAccess
                FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
                NULL,                               // lpSecurityAttributes
                OPEN_EXISTING,                      // dwCreationDistribution
                0,                                  // dwFlagsAndAttributes
                NULL                                // hTemplateFile
			);

    while(device != INVALID_HANDLE_VALUE) {

		status = DeviceIoControl(
					device,                 // Handle to device
					IOCTL_KEEPALIVE,			// IO Control code
					NULL,		    // Input Buffer to driver.
					0,			// Length of input buffer in bytes.
					NULL,           // Output Buffer from driver.
					0,			// Length of output buffer in bytes.
					&ReturnedLength,		    // Bytes placed in buffer.
					NULL                    // synchronous call
				);
		if (!status) {
			break;
		}
		Sleep(DOKAN_KEEPALIVE_TIME);
	}

	CloseHandle(device);

	_endthreadex(0);
	return 0;
}
