/*
  Dokan : user-mode file system library for Windows

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

#ifndef DOKANI_H_
#define DOKANI_H_

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <stdio.h>

#include "dokan.h"
#include "dokanc.h"
#include "list.h"
#include "dokan_vector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _DOKAN_INSTANCE_THREADINFO {
  PTP_POOL ThreadPool;
  PTP_CLEANUP_GROUP CleanupGroup;
  TP_CALLBACK_ENVIRON CallbackEnvironment;
} DOKAN_INSTANCE_THREADINFO;

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

  HANDLE GlobalDevice;
  HANDLE Device;
  HANDLE DeviceClosedWaitHandle;
  DOKAN_INSTANCE_THREADINFO ThreadInfo;
  HANDLE NotifyHandle;
  HANDLE KeepaliveHandle;

} DOKAN_INSTANCE, *PDOKAN_INSTANCE;

/**
 * \struct DOKAN_OPEN_INFO
 * \brief Dokan open file informations
 *
 * This is created in CreateFile and will be freed in CloseFile.
 */
typedef struct _DOKAN_OPEN_INFO {
  CRITICAL_SECTION CriticalSection;
  /** Dokan instance linked to the open */
  PDOKAN_INSTANCE DokanInstance;
  PDOKAN_VECTOR DirList;
  PWCHAR DirListSearchPattern;
  /** User Context see DOKAN_FILE_INFO.Context */
  volatile LONG64 UserContext;
  /** Event Id */
  ULONG EventId;
  /** DOKAN_OPTIONS linked to the mount */
  BOOL IsDirectory;
  /** Open count on the file */
  ULONG OpenCount;
  /** Used when dispatching the close once the OpenCount drops to 0 **/
  LPWSTR FileName;
  /** Event context */
  PEVENT_CONTEXT EventContext;
} DOKAN_OPEN_INFO, *PDOKAN_OPEN_INFO;

typedef struct _DOKAN_IO_BATCH {
  PDOKAN_INSTANCE DokanInstance;
  DWORD NumberOfBytesTransferred;
  BOOL MainPullThread;
  BOOL PoolAllocated;
  LONG EventContextBatchCount;
  EVENT_CONTEXT EventContext[1];
} DOKAN_IO_BATCH, *PDOKAN_IO_BATCH;

typedef struct _DOKAN_IO_EVENT {
  PDOKAN_INSTANCE DokanInstance;
  PDOKAN_OPEN_INFO DokanOpenInfo;
  PEVENT_INFORMATION EventResult;
  ULONG EventResultSize;
  BOOL PoolAllocated;
  DOKAN_FILE_INFO DokanFileInfo;
  PEVENT_CONTEXT EventContext;
  PDOKAN_IO_BATCH IoBatch;
} DOKAN_IO_EVENT, *PDOKAN_IO_EVENT;

#define IOEVENT_RESULT_BUFFER_SIZE(ioEvent)                                    \
  ((ioEvent)->EventResultSize >= offsetof(EVENT_INFORMATION, Buffer)           \
       ? (ioEvent)->EventResultSize - offsetof(EVENT_INFORMATION, Buffer)      \
       : 0)


BOOL DokanStart(_In_ PDOKAN_INSTANCE DokanInstance);

BOOL SendToDevice(LPCWSTR DeviceName, DWORD IoControlCode, PVOID InputBuffer,
                  ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                  PULONG ReturnedLength);

VOID
GetRawDeviceName(LPCWSTR DeviceName, LPWSTR DestinationBuffer,
                 rsize_t DestinationBufferSizeInElements);

VOID ALIGN_ALLOCATION_SIZE(PLARGE_INTEGER size, PDOKAN_OPTIONS DokanOptions);

BOOL DokanMount(LPCWSTR MountPoint, LPCWSTR DeviceName,
                PDOKAN_OPTIONS DokanOptions);

BOOL IsMountPointDriveLetter(LPCWSTR mountPoint);

VOID EventCompletion(PDOKAN_IO_EVENT EventInfo);

VOID CreateDispatchCommon(PDOKAN_IO_EVENT IoEvent, ULONG SizeOfEventInfo);

VOID DispatchDirectoryInformation(PDOKAN_IO_EVENT IoEvent);

VOID DispatchQueryInformation(PDOKAN_IO_EVENT IoEvent);

VOID DispatchQueryVolumeInformation(PDOKAN_IO_EVENT IoEvent);

VOID DispatchSetInformation(PDOKAN_IO_EVENT IoEvent);

VOID DispatchRead(PDOKAN_IO_EVENT IoEvent);

VOID DispatchWrite(PDOKAN_IO_EVENT IoEvent);

VOID DispatchCreate(PDOKAN_IO_EVENT IoEvent);

VOID DispatchClose(PDOKAN_IO_EVENT IoEvent);

VOID DispatchCleanup(PDOKAN_IO_EVENT IoEvent);

VOID DispatchFlush(PDOKAN_IO_EVENT IoEvent);

VOID DispatchLock(PDOKAN_IO_EVENT IoEvent);

VOID DispatchQuerySecurity(PDOKAN_IO_EVENT IoEvent);

VOID DispatchSetSecurity(PDOKAN_IO_EVENT IoEvent);

BOOL SendReleaseIRP(LPCWSTR DeviceName);

BOOL SendGlobalReleaseIRP(LPCWSTR MountPoint);

VOID CheckFileName(LPWSTR FileName);

VOID ReleaseDokanOpenInfo(PDOKAN_IO_EVENT IoEvent);

VOID DokanNotifyUnmounted(PDOKAN_INSTANCE DokanInstance);

#ifdef __cplusplus
}
#endif

#endif // DOKANI_H_
