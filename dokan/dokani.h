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

#ifndef DOKANI_H_
#define DOKANI_H_

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <stdio.h>
#include <stdlib.h>

#include "dokan.h"
#include "dokanc.h"
#include "list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _DOKAN_INSTANCE {
  // to ensure that unmount dispatch is called at once
  CRITICAL_SECTION CriticalSection;

  // store CurrentDeviceName
  // (when there are many mounts, each mount uses different DeviceName)
  WCHAR DeviceName[64];
  WCHAR MountPoint[MAX_PATH];
  WCHAR UNCName[64];

  ULONG DeviceNumber;
  ULONG MountId;

  PDOKAN_OPTIONS DokanOptions;
  PDOKAN_OPERATIONS DokanOperations;

  LIST_ENTRY ListEntry;
} DOKAN_INSTANCE, *PDOKAN_INSTANCE;

typedef struct _DOKAN_OPEN_INFO {
  BOOL IsDirectory;
  ULONG OpenCount;
  PEVENT_CONTEXT EventContext;
  PDOKAN_INSTANCE DokanInstance;
  ULONG64 UserContext;
  ULONG EventId;
  PLIST_ENTRY DirListHead;
  PLIST_ENTRY StreamListHead;
} DOKAN_OPEN_INFO, *PDOKAN_OPEN_INFO;

BOOL DokanStart(PDOKAN_INSTANCE Instance);

BOOL SendToDevice(LPCWSTR DeviceName, DWORD IoControlCode, PVOID InputBuffer,
                  ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                  PULONG ReturnedLength);

LPWSTR
GetRawDeviceName(LPCWSTR DeviceName, LPWSTR DestinationBuffer,
                 rsize_t DestinationBufferSizeInElements);

void ALIGN_ALLOCATION_SIZE(PLARGE_INTEGER size, PDOKAN_OPTIONS DokanOptions);

UINT __stdcall DokanLoop(PVOID Param);

BOOL DokanMount(LPCWSTR MountPoint, LPCWSTR DeviceName,
                PDOKAN_OPTIONS DokanOptions);

BOOL IsMountPointDriveLetter(LPCWSTR mountPoint);

VOID SendEventInformation(HANDLE Handle, PEVENT_INFORMATION EventInfo,
                          ULONG EventLength, PDOKAN_INSTANCE DokanInstance);

PEVENT_INFORMATION
DispatchCommon(PEVENT_CONTEXT EventContext, ULONG SizeOfEventInfo,
               PDOKAN_INSTANCE DokanInstance, PDOKAN_FILE_INFO DokanFileInfo,
               PDOKAN_OPEN_INFO *DokanOpenInfo);

VOID DispatchDirectoryInformation(HANDLE Handle, PEVENT_CONTEXT EventContext,
                                  PDOKAN_INSTANCE DokanInstance);

VOID DispatchQueryInformation(HANDLE Handle, PEVENT_CONTEXT EventContext,
                              PDOKAN_INSTANCE DokanInstance);

VOID DispatchQueryVolumeInformation(HANDLE Handle, PEVENT_CONTEXT EventContext,
                                    PDOKAN_INSTANCE DokanInstance);

VOID DispatchSetInformation(HANDLE Handle, PEVENT_CONTEXT EventContext,
                            PDOKAN_INSTANCE DokanInstance);

VOID DispatchRead(HANDLE Handle, PEVENT_CONTEXT EventContext,
                  PDOKAN_INSTANCE DokanInstance);

VOID DispatchWrite(HANDLE Handle, PEVENT_CONTEXT EventContext,
                   PDOKAN_INSTANCE DokanInstance);

VOID DispatchCreate(HANDLE Handle, PEVENT_CONTEXT EventContext,
                    PDOKAN_INSTANCE DokanInstance);

VOID DispatchClose(HANDLE Handle, PEVENT_CONTEXT EventContext,
                   PDOKAN_INSTANCE DokanInstance);

VOID DispatchCleanup(HANDLE Handle, PEVENT_CONTEXT EventContext,
                     PDOKAN_INSTANCE DokanInstance);

VOID DispatchFlush(HANDLE Handle, PEVENT_CONTEXT EventContext,
                   PDOKAN_INSTANCE DokanInstance);

VOID DispatchLock(HANDLE Handle, PEVENT_CONTEXT EventContext,
                  PDOKAN_INSTANCE DokanInstance);

VOID DispatchQuerySecurity(HANDLE Handle, PEVENT_CONTEXT EventContext,
                           PDOKAN_INSTANCE DokanInstance);

VOID DispatchSetSecurity(HANDLE Handle, PEVENT_CONTEXT EventContext,
                         PDOKAN_INSTANCE DokanInstance);

BOOLEAN
InstallDriver(SC_HANDLE SchSCManager, LPCWSTR DriverName, LPCWSTR ServiceExe);

BOOLEAN
RemoveDriver(SC_HANDLE SchSCManager, LPCWSTR DriverName);

BOOLEAN
StartDriver(SC_HANDLE SchSCManager, LPCWSTR DriverName);

BOOLEAN
StopDriver(SC_HANDLE SchSCManager, LPCWSTR DriverName);

BOOLEAN
ManageDriver(LPCWSTR DriverName, LPCWSTR ServiceName, USHORT Function);

BOOL SendReleaseIRP(LPCWSTR DeviceName);

BOOL SendGlobalReleaseIRP(LPCWSTR MountPoint);

VOID CheckFileName(LPWSTR FileName);

VOID ClearFindData(PLIST_ENTRY ListHead);

VOID ClearFindStreamData(PLIST_ENTRY ListHead);

UINT WINAPI DokanKeepAlive(PVOID Param);

PDOKAN_OPEN_INFO
GetDokanOpenInfo(PEVENT_CONTEXT EventInfomation, PDOKAN_INSTANCE DokanInstance);

VOID ReleaseDokanOpenInfo(PEVENT_INFORMATION EventInfomation,
                          PDOKAN_INSTANCE DokanInstance);

#ifdef __cplusplus
}
#endif

#endif // DOKANI_H_
