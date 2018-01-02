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

#ifndef DOKANI_H_
#define DOKANI_H_

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <stdio.h>

#include "dokan.h"
#include "dokanc.h"
#include "list.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \struct DOKAN_INSTANCE
 * \brief Dokan mount instance informations
 *
 * This struct is build from the information provided by the user at DokanMain call.
 * \see DokanMain
 * \see DOKAN_OPTIONS
 * \see DOKAN_OPERATIONS
 */
typedef struct _DOKAN_INSTANCE {
  /** to ensure that unmount dispatch is called at once */
  CRITICAL_SECTION CriticalSection;

  /**
  * Current DeviceName.
  * When there are many mounts, each mount uses different DeviceName.
  */
  WCHAR DeviceName[64];
  /** Mount point. Can be "M:\" (drive letter) or "C:\mount\dokan" (path in NTFS) */
  WCHAR MountPoint[MAX_PATH];
  /** UNC name used for network volume */
  WCHAR UNCName[64];

  /** Device number */
  ULONG DeviceNumber;
  /** Mount ID */
  ULONG MountId;

  /** DOKAN_OPTIONS linked to the mount */
  PDOKAN_OPTIONS DokanOptions;
  /** DOKAN_OPERATIONS linked to the mount */
  PDOKAN_OPERATIONS DokanOperations;

  /** Current list entry informations */
  LIST_ENTRY ListEntry;
} DOKAN_INSTANCE, *PDOKAN_INSTANCE;

/**
 * \struct DOKAN_OPEN_INFO
 * \brief Dokan open file informations
 *
 * This is created in CreateFile and will be freed in CloseFile.
 */
typedef struct _DOKAN_OPEN_INFO {
  /** DOKAN_OPTIONS linked to the mount */
  BOOL IsDirectory;
  /** Open count on the file */
  ULONG OpenCount;
  /** Event context */
  PEVENT_CONTEXT EventContext;
  /** Dokan instance linked to the open */
  PDOKAN_INSTANCE DokanInstance;
  /** User Context see DOKAN_FILE_INFO.Context */
  ULONG64 UserContext;
  /** Event Id */
  ULONG EventId;
  /** Directories list. Used by FindFiles */
  PLIST_ENTRY DirListHead;
  /** File streams list. Used by FindStreams */
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
