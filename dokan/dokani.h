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

typedef struct _DOKAN_INSTANCE_THREADINFO {
	PTP_POOL			ThreadPool;
	PTP_CLEANUP_GROUP	CleanupGroup;
	PTP_TIMER			KeepAliveTimer;
	PTP_IO				IoCompletion;
	TP_CALLBACK_ENVIRON CallbackEnvironment;
} DOKAN_INSTANCE_THREADINFO;

typedef struct _DOKAN_INSTANCE {
  // to ensure that unmount dispatch is called at once
  CRITICAL_SECTION		CriticalSection;

  // store CurrentDeviceName
  // (when there are many mounts, each mount uses different DeviceName)
  WCHAR							DeviceName[64];
  WCHAR							MountPoint[MAX_PATH];
  WCHAR							UNCName[64];

  ULONG							DeviceNumber;
  ULONG							MountId;

  PDOKAN_OPTIONS				DokanOptions;
  PDOKAN_OPERATIONS				DokanOperations;

  LIST_ENTRY					ListEntry;
  
  HANDLE						GlobalDevice;
  HANDLE						Device;
  HANDLE						DeviceClosedWaitHandle;
  DOKAN_INSTANCE_THREADINFO		ThreadInfo;

} DOKAN_INSTANCE, *PDOKAN_INSTANCE;

typedef struct _DOKAN_OPEN_INFO {
  CRITICAL_SECTION		CriticalSection;
  PDOKAN_INSTANCE		DokanInstance;
  DOKAN_VECTOR			*DirList;
  PWCHAR				DirListSearchPattern;
  volatile LONG64		UserContext;
  ULONG					EventId;
  BOOL					IsDirectory;
} DOKAN_OPEN_INFO, *PDOKAN_OPEN_INFO;

typedef enum _DOKAN_OVERLAPPED_TYPE {

	// The overlapped operation contains a DOKAN_IO_EVENT as its payload
	DOKAN_OVERLAPPED_TYPE_IOEVENT = 0,

	// The overlapped operation payload contains a result being passed back to the
	// kernel driver. Results are represented as an EVENT_INFORMATION struct.
	DOKAN_OVERLAPPED_TYPE_IOEVENT_RESULT,

	// The overlapped operation contains both an input and an output because
	// the input IO event wasn't big enough to handle the write operation
	DOKAN_OVERLAPPED_TYPE_IOEVENT_WRITE_SIZE,

} DOKAN_OVERLAPPED_TYPE;

typedef enum _DOKAN_IO_EVENT_FLAGS {
	
	// There are no flags set
	DOKAN_IO_EVENT_FLAGS_NONE = 0,

	// The DOKAN_IO_EVENT object associated with the IO event
	// was allocated from a global pool and should be returned to
	// that pool instead of free'd
	DOKAN_IO_EVENT_FLAGS_POOLED = 1,

	// The EVENT_INFORMATION object associated with the IO event
	// was allocated from a global pool and should be returned to
	// that pool instead of free'd
	DOKAN_IO_EVENT_FLAGS_POOLED_RESULT = (1 << 1),

} DOKAN_IO_EVENT_FLAGS;

// See DeviceIoControl() for how InputPayload and OutputLoad are used as it's not entirely intuitive
// https://msdn.microsoft.com/en-us/library/windows/desktop/aa363216%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396
typedef struct _DOKAN_OVERLAPPED {
	OVERLAPPED				InternalOverlapped;
	void					*InputPayload;
	void					*OutputPayload;
	DOKAN_OVERLAPPED_TYPE	PayloadType;
	DOKAN_IO_EVENT_FLAGS	Flags;
} DOKAN_OVERLAPPED;

typedef struct _DOKAN_IO_EVENT {
	union {
		DOKAN_CREATE_FILE_EVENT				ZwCreateFile;
		DOKAN_CLEANUP_EVENT					Cleanup;
		DOKAN_CLOSE_FILE_EVENT				CloseFile;
		DOKAN_READ_FILE_EVENT				ReadFile;
		DOKAN_WRITE_FILE_EVENT				WriteFile;
		DOKAN_FLUSH_BUFFERS_EVENT			FlushBuffers;
		DOKAN_GET_FILE_INFO_EVENT			GetFileInfo;
		DOKAN_FIND_FILES_EVENT				FindFiles;
		DOKAN_FIND_FILES_PATTERN_EVENT		FindFilesWithPattern;
		DOKAN_SET_FILE_BASIC_INFO_EVENT		SetFileBasicInformation;
		DOKAN_CAN_DELETE_FILE_EVENT			CanDeleteFile;
		DOKAN_MOVE_FILE_EVENT				MoveFileW;
		DOKAN_SET_EOF_EVENT					SetEOF;
		DOKAN_SET_ALLOCATION_SIZE_EVENT		SetAllocationSize;
		DOKAN_LOCK_FILE_EVENT				LockFile;
		DOKAN_UNLOCK_FILE_EVENT				UnlockFile;
		DOKAN_GET_DISK_FREE_SPACE_EVENT		GetVolumeFreeSpace;
		DOKAN_GET_VOLUME_INFO_EVENT			GetVolumeInfo;
		DOKAN_GET_VOLUME_ATTRIBUTES_EVENT	GetVolumeAttributes;
		DOKAN_GET_FILE_SECURITY_EVENT		GetFileSecurityW;
		DOKAN_SET_FILE_SECURITY_EVENT		SetFileSecurityW;
		DOKAN_FIND_STREAMS_EVENT			FindStreams;
	} EventInfo;

	// If the processing for the event requires extra data to be associated with it
	// then a pointer to that data can be placed here
	void								*ProcessingContext;

	PDOKAN_INSTANCE						DokanInstance;
	DOKAN_OPEN_INFO						*DokanOpenInfo;
	PEVENT_INFORMATION					EventResult;
	ULONG								EventResultSize;
	ULONG								EventSize;
	DOKAN_IO_EVENT_FLAGS				Flags;
	DOKAN_FILE_INFO						DokanFileInfo;

	union {
		BYTE							EventContextBuffer[EVENT_CONTEXT_MAX_SIZE];
		EVENT_CONTEXT					EventContext;
	} KernelInfo;
} DOKAN_IO_EVENT, *PDOKAN_IO_EVENT;

#define IoEventResultBufferSize(ioEvent) ((ioEvent)->EventResultSize >= offsetof(EVENT_INFORMATION, Buffer) ? (ioEvent)->EventResultSize - offsetof(EVENT_INFORMATION, Buffer) : 0)
#define DOKAN_IO_EVENT_ALLOC_SIZE(bufSize) (offsetof(DOKAN_IO_EVENT, KernelInfo) + (bufSize))

BOOL DokanStart(PDOKAN_INSTANCE Instance);

BOOL SendToDevice(LPCWSTR DeviceName, DWORD IoControlCode, PVOID InputBuffer,
                  ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                  PULONG ReturnedLength);

LPWSTR
GetRawDeviceName(LPCWSTR DeviceName, LPWSTR DestinationBuffer,
                 rsize_t DestinationBufferSizeInElements);

void ALIGN_ALLOCATION_SIZE(PLARGE_INTEGER size, PDOKAN_OPTIONS DokanOptions);

VOID CALLBACK DokanLoop(
	_Inout_     PTP_CALLBACK_INSTANCE Instance,
	_Inout_opt_ PVOID                 Context,
	_Inout_opt_ PVOID                 Overlapped,
	_In_        ULONG                 IoResult,
	_In_        ULONG_PTR             NumberOfBytesTransferred,
	_Inout_     PTP_IO                Io
);

BOOL DokanMount(LPCWSTR MountPoint, LPCWSTR DeviceName,
                PDOKAN_OPTIONS DokanOptions);

BOOL IsMountPointDriveLetter(LPCWSTR mountPoint);

BOOL SendEventInformation(PEVENT_INFORMATION EventInfo,
	PDOKAN_INSTANCE DokanInstance, DOKAN_IO_EVENT_FLAGS EventFlags);

void CreateDispatchCommon(DOKAN_IO_EVENT *EventInfo, ULONG SizeOfEventInfo);

void FreeIoEventResult(PEVENT_INFORMATION EventResult, DOKAN_IO_EVENT_FLAGS Flags);

void BeginDispatchDirectoryInformation(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchQueryInformation(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchQueryVolumeInformation(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchSetInformation(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchRead(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchWrite(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchCreate(DOKAN_IO_EVENT *EventInfo);

void DispatchClose(DOKAN_IO_EVENT *EventInfo);

void DispatchCleanup(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchFlush(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchLock(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchQuerySecurity(DOKAN_IO_EVENT *EventInfo);

void BeginDispatchSetSecurity(DOKAN_IO_EVENT *EventInfo);

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

void NTAPI DokanKeepAlive(
	_Inout_     PTP_CALLBACK_INSTANCE Instance,
	_Inout_opt_ PVOID                 Context,
	_Inout_     PTP_TIMER             Timer
);

BOOL SendIoEventResult(DOKAN_IO_EVENT *EventInfo);

DOKAN_IO_EVENT* PopIoEventBuffer();
void PushIoEventBuffer(DOKAN_IO_EVENT *IOEvent);

DOKAN_OVERLAPPED* PopOverlapped();
void PushOverlapped(DOKAN_OVERLAPPED *overlapped);
void ResetOverlapped(DOKAN_OVERLAPPED *overlapped);

EVENT_INFORMATION* PopEventResult();
void PushEventResult(EVENT_INFORMATION *EventResult);

DOKAN_OPEN_INFO* PopFileOpenInfo();
void PushFileOpenInfo(DOKAN_OPEN_INFO *FileInfo);

DOKAN_VECTOR* PopDirectoryList();
void PushDirectoryList(DOKAN_VECTOR *DirectoryList);

void DokanNotifyUnmounted(DOKAN_INSTANCE *Instance);

void* DokanMallocImpl(size_t size, const char *fileName, int lineNumber);
void DokanFreeImpl(void *userData);
void* DokanReallocImpl(void *userData, size_t newSize, const char *fileName, int lineNumber);
LPWSTR DokanDupWImpl(LPCWSTR str, const char *fileName, int lineNumber);

#define DokanMalloc(size) DokanMallocImpl((size), __FILE__, __LINE__)
#define DokanFree(userData) DokanFreeImpl(userData)
#define DokanRealloc(userData, newSize) DokanReallocImpl((userData), (newSize), __FILE__, __LINE__)
#define DokanDupW(str) DokanDupWImpl((str), __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif // DOKANI_H_
