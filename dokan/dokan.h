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

#ifndef DOKAN_H_
#define DOKAN_H_

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include "fileinfo.h"
#include "public.h"

#define DOKAN_DRIVER_NAME L"dokan" DOKAN_MAJOR_API_VERSION L".sys"
#define DOKAN_NP_NAME L"Dokan" DOKAN_MAJOR_API_VERSION

#ifdef _EXPORTING
#define DOKANAPI /*__declspec(dllexport)*/                                     \
  __stdcall      // exports defined in dokan.def
#define DOKAN_API
#else
#define DOKANAPI __declspec(dllimport) __stdcall
#define DOKAN_API __declspec(dllimport)
#endif

#define DOKAN_CALLBACK __stdcall

#include "dokan_vector.h"

#ifdef __cplusplus
extern "C" {
#endif

// The current Dokan version (ver 1.0.0). Please set this constant on
// DokanOptions->Version.
#define DOKAN_VERSION 100
#define DOKAN_MINIMUM_COMPATIBLE_VERSION 100

#define DOKAN_MAX_INSTANCES 32 // Maximum number of dokan instances

#define DOKAN_OPTION_DEBUG					1			// ouput debug message
#define DOKAN_OPTION_STDERR					(1 << 1)    // ouput debug message to stderr
#define DOKAN_OPTION_ALT_STREAM				(1 << 2)    // use alternate stream
#define DOKAN_OPTION_WRITE_PROTECT			(1 << 3)	// mount drive as write-protected.
#define DOKAN_OPTION_NETWORK				(1 << 4)    // use network drive, you need to
														// install Dokan network provider.
#define DOKAN_OPTION_REMOVABLE				(1 << 5)	// use removable drive
#define DOKAN_OPTION_MOUNT_MANAGER			(1 << 6)	// use mount manager
#define DOKAN_OPTION_CURRENT_SESSION		(1 << 7)	// mount the drive on current session only
#define DOKAN_OPTION_FILELOCK_USER_MODE		(1 << 8)	// FileLock in User Mode
#define DOKAN_OPTION_ASYNC_IO				(1 << 9)	// use asynchronous IO
#define DOKAN_OPTION_FORCE_SINGLE_THREADED	(1 << 10)	// Dokan uses a single thread. If DOKAN_OPTION_ASYNC_IO is specified the thread waits until the async job is over before starting another.

typedef void *DOKAN_HANDLE, **PDOKAN_HANDLE;

typedef struct _DOKAN_OPTIONS {
  USHORT Version;        // Supported Dokan Version, ex. "530" (Dokan ver 0.5.3)
  USHORT ThreadCount;    // Unused
  ULONG Options;         // combination of DOKAN_OPTIONS_*
  ULONG64 GlobalContext; // FileSystem can store anything here
  LPCWSTR MountPoint; //  mount point "M:\" (drive letter) or "C:\mount\dokan"
                      //  (path in NTFS)
  LPCWSTR UNCName;    // UNC provider name
  ULONG Timeout;      // IrpTimeout in milliseconds
  ULONG AllocationUnitSize; // Device allocation size
  ULONG SectorSize;         // Device sector size
} DOKAN_OPTIONS, *PDOKAN_OPTIONS;

typedef struct _DOKAN_FILE_INFO {
  ULONG64						Context;						// FileSystem can store anything here
  PVOID							DokanContext;					// For internal use only
  PDOKAN_OPTIONS				DokanOptions;					// A pointer to DOKAN_OPTIONS
																// which was passed to DokanMain.
  BOOL							IsDirectory;					// requesting a directory file
  ULONG							ProcessId;						// process id for the thread that originally requested a
																// given I/O operation
  UCHAR							DeleteOnClose;					// Delete on when "cleanup" is called
  UCHAR							PagingIo;						// Read or write is paging IO.
  UCHAR							SynchronousIo;					// Read or write is synchronous IO.
  UCHAR							Nocache;
  UCHAR							WriteToEndOfFile;				//  If true, write to the current end of file instead
																//  of Offset parameter.

} DOKAN_FILE_INFO, *PDOKAN_FILE_INFO;

typedef struct _DOKAN_MOUNTED_INFO {
  PDOKAN_OPTIONS				DokanOptions;					// A pointer to DOKAN_OPTIONS
  PTP_POOL						ThreadPool;						// The thread pool associated with the Dokan context
} DOKAN_MOUNTED_INFO, *PDOKAN_MOUNTED_INFO;

typedef struct _DOKAN_UNMOUNTED_INFO {
	PDOKAN_OPTIONS				DokanOptions;					// A pointer to DOKAN_OPTIONS
} DOKAN_UNMOUNTED_INFO, *PDOKAN_UNMOUNTED_INFO;

typedef void* (WINAPI *PDokanMalloc)(size_t size, const char *fileName, int lineNumber);
typedef void (WINAPI *PDokanFree)(void *userData);
typedef void* (WINAPI *PDokanRealloc)(void *userData, size_t newSize, const char *fileName, int lineNumber);

typedef struct _DOKAN_MEMORY_CALLBACKS {
	PDokanMalloc	Malloc;
	PDokanFree		Free;
	PDokanRealloc	Realloc;
} DOKAN_MEMORY_CALLBACKS, *PDOKAN_MEMORY_CALLBACKS;

#define DOKAN_EXCEPTION_NOT_INITIALIZED			0x0f0ff0ff
#define DOKAN_EXCEPTION_INITIALIZATION_FAILED	0x0fbadbad
#define DOKAN_EXCEPTION_SHUTDOWN_FAILED			0x0fbadf00

// Forward declarations
struct _DOKAN_FIND_FILES_EVENT;
typedef struct _DOKAN_FIND_FILES_EVENT DOKAN_FIND_FILES_EVENT, *PDOKAN_FIND_FILES_EVENT;

struct _DOKAN_FIND_FILES_PATTERN_EVENT;
typedef struct _DOKAN_FIND_FILES_PATTERN_EVENT DOKAN_FIND_FILES_PATTERN_EVENT, *PDOKAN_FIND_FILES_PATTERN_EVENT;

struct _DOKAN_FIND_STREAMS_EVENT;
typedef struct _DOKAN_FIND_STREAMS_EVENT DOKAN_FIND_STREAMS_EVENT, *PDOKAN_FIND_STREAMS_EVENT;

// FillFindData
//   is used to add an entry in FindFiles
//   returns 1 if buffer is full, otherwise 0
//   (currently it never returns 1)
typedef int (WINAPI *PFillFindData)(PDOKAN_FIND_FILES_EVENT, PWIN32_FIND_DATAW);

// FillFindDataWithPattern
//   is used to add an entry in FindFilesWithPattern
//   returns 1 if buffer is full, otherwise 0
//   (currently it never returns 1)
typedef int (WINAPI *PFillFindDataWithPattern)(PDOKAN_FIND_FILES_PATTERN_EVENT, PWIN32_FIND_DATAW);

// FillFindStreamData
//   is used to add an entry in FindStreams
//   returns 1 if buffer is full, otherwise 0
typedef int (WINAPI *PFillFindStreamData)(PDOKAN_FIND_STREAMS_EVENT, PWIN32_FIND_STREAM_DATA);

typedef struct _DOKAN_CREATE_FILE_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	LPCWSTR							OriginalFileName;
	DOKAN_IO_SECURITY_CONTEXT		SecurityContext; // https://msdn.microsoft.com/en-us/library/windows/hardware/ff550613(v=vs.85).aspx

	ACCESS_MASK						DesiredAccess;
	ULONG							FileAttributes;
	ULONG							ShareAccess;
	ULONG							CreateDisposition;
	ULONG							CreateOptions;
} DOKAN_CREATE_FILE_EVENT, *PDOKAN_CREATE_FILE_EVENT;

typedef struct _DOKAN_CLEANUP_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
} DOKAN_CLEANUP_EVENT, *PDOKAN_CLEANUP_EVENT;

typedef DOKAN_CLEANUP_EVENT DOKAN_CLOSE_FILE_EVENT, *PDOKAN_CLOSE_FILE_EVENT;
typedef DOKAN_CLEANUP_EVENT DOKAN_FLUSH_BUFFERS_EVENT, *PDOKAN_FLUSH_BUFFERS_EVENT;
typedef DOKAN_CLEANUP_EVENT DOKAN_CAN_DELETE_FILE_EVENT, *PDOKAN_CAN_DELETE_FILE_EVENT;

typedef struct _DOKAN_READ_FILE_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	LPVOID							Buffer;
	LONGLONG						Offset;
	DWORD							NumberOfBytesToRead;
	DWORD							NumberOfBytesRead;
} DOKAN_READ_FILE_EVENT, *PDOKAN_READ_FILE_EVENT;

typedef struct _DOKAN_WRITE_FILE_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	LPCVOID							Buffer;
	LONGLONG						Offset;
	DWORD							NumberOfBytesToWrite;
	DWORD							NumberOfBytesWritten;
} DOKAN_WRITE_FILE_EVENT, *PDOKAN_WRITE_FILE_EVENT;

typedef struct _DOKAN_GET_FILE_INFO_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	BY_HANDLE_FILE_INFORMATION		FileHandleInfo;
} DOKAN_GET_FILE_INFO_EVENT, *PDOKAN_GET_FILE_INFO_EVENT;

typedef struct _DOKAN_FIND_FILES_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							PathName;
	PFillFindData					FillFindData;     // call this function with PWIN32_FIND_DATAW
} DOKAN_FIND_FILES_EVENT, *PDOKAN_FIND_FILES_EVENT;

typedef struct _DOKAN_FIND_FILES_PATTERN_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							PathName;
	LPCWSTR							SearchPattern;
	PFillFindDataWithPattern		FillFindData;     // call this function with PWIN32_FIND_DATAW
} DOKAN_FIND_FILES_PATTERN_EVENT, *PDOKAN_FIND_FILES_PATTERN_EVENT;

typedef struct _DOKAN_SET_FILE_BASIC_INFO_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	FILE_BASIC_INFORMATION			*Info;
} DOKAN_SET_FILE_BASIC_INFO_EVENT, *PDOKAN_SET_FILE_BASIC_INFO_EVENT;

typedef struct _DOKAN_MOVE_FILE_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	LPCWSTR							NewFileName;
	BOOL							ReplaceIfExists;
} DOKAN_MOVE_FILE_EVENT, *PDOKAN_MOVE_FILE_EVENT;

typedef struct _DOKAN_SET_EOF_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	LONGLONG						Length;
} DOKAN_SET_EOF_EVENT, *PDOKAN_SET_EOF_EVENT;

typedef DOKAN_SET_EOF_EVENT DOKAN_SET_ALLOCATION_SIZE_EVENT, *PDOKAN_SET_ALLOCATION_SIZE_EVENT;

typedef struct _DOKAN_LOCK_FILE_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	LONGLONG						ByteOffset;
	LONGLONG						Length;
	ULONG							Key;
} DOKAN_LOCK_FILE_EVENT, *PDOKAN_LOCK_FILE_EVENT;

typedef DOKAN_LOCK_FILE_EVENT DOKAN_UNLOCK_FILE_EVENT, *PDOKAN_UNLOCK_FILE_EVENT;

// see FILE_FS_FULL_SIZE_INFORMATION for more information
// https://msdn.microsoft.com/en-us/library/windows/hardware/ff540267(v=vs.85).aspx
typedef struct _DOKAN_GET_DISK_FREE_SPACE_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	ULONGLONG						FreeBytesAvailable;
	ULONGLONG						TotalNumberOfBytes;
	ULONGLONG						TotalNumberOfFreeBytes;
} DOKAN_GET_DISK_FREE_SPACE_EVENT, *PDOKAN_GET_DISK_FREE_SPACE_EVENT;

typedef struct _DOKAN_GET_VOLUME_INFO_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	PFILE_FS_VOLUME_INFORMATION		VolumeInfo;
	DWORD							MaxLabelLengthInChars;
} DOKAN_GET_VOLUME_INFO_EVENT, *PDOKAN_GET_VOLUME_INFO_EVENT;

typedef struct _DOKAN_GET_VOLUME_ATTRIBUTES_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	PFILE_FS_ATTRIBUTE_INFORMATION	Attributes;
	DWORD							MaxFileSystemNameLengthInChars;
} DOKAN_GET_VOLUME_ATTRIBUTES_EVENT, *PDOKAN_GET_VOLUME_ATTRIBUTES_EVENT;

typedef struct _DOKAN_GET_FILE_SECURITY_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPWSTR							FileName;
	PSECURITY_INFORMATION			SecurityInformation; // A pointer to SECURITY_INFORMATION value being requested
	PSECURITY_DESCRIPTOR			SecurityDescriptor; // A pointer to SECURITY_DESCRIPTOR buffer to be filled
	ULONG							SecurityDescriptorSize; // length of Security descriptor buffer
	ULONG							LengthNeeded;
} DOKAN_GET_FILE_SECURITY_EVENT, *PDOKAN_GET_FILE_SECURITY_EVENT;

typedef struct _DOKAN_SET_FILE_SECURITY_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPWSTR							FileName;
	PSECURITY_INFORMATION			SecurityInformation;
	PSECURITY_DESCRIPTOR			SecurityDescriptor; // A pointer to SECURITY_DESCRIPTOR buffer to be filled
	ULONG							SecurityDescriptorSize; // length of Security descriptor buffer
} DOKAN_SET_FILE_SECURITY_EVENT, *PDOKAN_SET_FILE_SECURITY_EVENT;

typedef struct _DOKAN_FIND_STREAMS_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPWSTR							FileName;
	PFillFindStreamData				FillFindStreamData; // call this function with PWIN32_FIND_STREAM_DATA
} DOKAN_FIND_STREAMS_EVENT, *PDOKAN_FIND_STREAMS_EVENT;

typedef struct _DOKAN_OPERATIONS {

  // When an error occurs, return NTSTATUS
  // (https://support.microsoft.com/en-us/kb/113996)

  // CreateFile
  //   In case OPEN_ALWAYS & CREATE_ALWAYS are opening successfully a already
  //   existing file,
  //   you have to SetLastError(ERROR_ALREADY_EXISTS)
  //   If file is a directory, CreateFile is also called.
  //   In this case, CreateFile should return STATUS_SUCCESS when that directory
  //   can be opened.
  //   You should set TRUE on DokanFileInfo->IsDirectory when file is a
  //   directory.
  //   See ZwCreateFile()
  //   https://msdn.microsoft.com/en-us/library/windows/hardware/ff566424(v=vs.85).aspx
  //   for more information about the parameters of this callback.
NTSTATUS(DOKAN_CALLBACK *ZwCreateFile)(_In_ DOKAN_CREATE_FILE_EVENT *EventInfo);
  
  // When FileInfo->DeleteOnClose is true, you must delete the file in Cleanup.
  // Refer to comment at DeleteFile definition below in this file for
  // explanation.
  void(DOKAN_CALLBACK *Cleanup)(_In_ DOKAN_CLEANUP_EVENT *EventInfo);

  void(DOKAN_CALLBACK *CloseFile)(_In_ DOKAN_CLOSE_FILE_EVENT *EventInfo);

  // ReadFile and WriteFile can be called from multiple threads in
  // the same time with the same DOKAN_FILE_INFO.Context if a OVERLAPPED is
  // requested.
  NTSTATUS(DOKAN_CALLBACK *ReadFile)(_In_ DOKAN_READ_FILE_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *WriteFile)(_In_ DOKAN_WRITE_FILE_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *FlushFileBuffers)(_In_ DOKAN_FLUSH_BUFFERS_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *GetFileInformation)(_In_ DOKAN_GET_FILE_INFO_EVENT *EventInfo);

  // FindFilesWithPattern is checking first. If it is not implemented or
  // returns STATUS_NOT_IMPLEMENTED, then FindFiles is called, if implemented.
  NTSTATUS(DOKAN_CALLBACK *FindFiles)(_In_ DOKAN_FIND_FILES_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *FindFilesWithPattern)(_In_ DOKAN_FIND_FILES_PATTERN_EVENT *EventInfo);

  // SetFileAttributes and SetFileTime are called only if both of them
  // are implemented.
  NTSTATUS(DOKAN_CALLBACK *SetFileBasicInformation)(_In_ DOKAN_SET_FILE_BASIC_INFO_EVENT *EventInfo);

  // You should not delete the file on DeleteFile, but
  // instead
  // you must only check whether you can delete the file or not,
  // and return STATUS_SUCCESS (when you can delete it) or appropriate error
  // codes such as
  // STATUS_ACCESS_DENIED, STATUS_OBJECT_PATH_NOT_FOUND,
  // STATUS_OBJECT_NAME_NOT_FOUND.
  // When you return STATUS_SUCCESS, you get a Cleanup call afterwards with
  // FileInfo->DeleteOnClose set to TRUE and only then you have to actually
  // delete the file being closed.
  NTSTATUS(DOKAN_CALLBACK *CanDeleteFile)(_In_ DOKAN_CAN_DELETE_FILE_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *MoveFileW)(_In_ DOKAN_MOVE_FILE_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *SetEndOfFile)(_In_ DOKAN_SET_EOF_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *SetAllocationSize)(_In_ DOKAN_SET_ALLOCATION_SIZE_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *LockFile)(_In_ DOKAN_LOCK_FILE_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *UnlockFile)(_In_ DOKAN_UNLOCK_FILE_EVENT *EventInfo);

  // Neither GetVolumeFreeSpace nor GetVolumeInformation
  // save the DokanFileContext->Context.
  // Before these methods are called, CreateFile may not be called.
  // (ditto CloseFile and Cleanup)

  // see Win32 API GetDiskFreeSpaceEx
  NTSTATUS(DOKAN_CALLBACK *GetVolumeFreeSpace)(_In_ DOKAN_GET_DISK_FREE_SPACE_EVENT *EventInfo);

  // Note:
  // FILE_READ_ONLY_VOLUME is automatically added to the
  // FileSystemFlags if DOKAN_OPTION_WRITE_PROTECT was
  // specified in DOKAN_OPTIONS when the volume was mounted.

  // see Win32 API GetVolumeInformation
  NTSTATUS(DOKAN_CALLBACK *GetVolumeInformationW)(_In_ DOKAN_GET_VOLUME_INFO_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *GetVolumeAttributes)(_In_ DOKAN_GET_VOLUME_ATTRIBUTES_EVENT *EventInfo);

  void(DOKAN_CALLBACK *Mounted)(DOKAN_MOUNTED_INFO *EventInfo);

  void(DOKAN_CALLBACK *Unmounted)(DOKAN_UNMOUNTED_INFO *EventInfo);

  // Suported since 0.6.0. You must specify the version at
  // DOKAN_OPTIONS.Version.
  NTSTATUS(DOKAN_CALLBACK *GetFileSecurityW)(_In_ DOKAN_GET_FILE_SECURITY_EVENT *EventInfo);

  NTSTATUS(DOKAN_CALLBACK *SetFileSecurityW)(_In_ DOKAN_SET_FILE_SECURITY_EVENT *EventInfo);

  // Supported since 0.8.0. You must specify the version at
  // DOKAN_OPTIONS.Version.
  NTSTATUS(DOKAN_CALLBACK *FindStreams)(_In_ DOKAN_FIND_STREAMS_EVENT *EventInfo);

} DOKAN_OPERATIONS, *PDOKAN_OPERATIONS;

typedef struct _DOKAN_CONTROL {
  ULONG Type;
  WCHAR MountPoint[MAX_PATH];
  WCHAR UNCName[64];
  WCHAR DeviceName[64];
  PVOID DeviceObject;
} DOKAN_CONTROL, *PDOKAN_CONTROL;

/* DokanMain returns error codes */
#define DOKAN_SUCCESS 0
#define DOKAN_ERROR -1                /* General Error */
#define DOKAN_DRIVE_LETTER_ERROR -2   /* Bad Drive letter */
#define DOKAN_DRIVER_INSTALL_ERROR -3 /* Can't install driver */
#define DOKAN_START_ERROR -4          /* Driver something wrong */
#define DOKAN_MOUNT_ERROR -5 /* Can't assign a drive letter or mount point */
#define DOKAN_MOUNT_POINT_ERROR -6 /* Mount point is invalid */
#define DOKAN_VERSION_ERROR -7     /* Requested an incompatible version */

#define DOKAN_SUCCEEDED(x) ((x) == DOKAN_SUCCESS)
#define DOKAN_FAILED(x) ((x) != DOKAN_SUCCESS)

int DOKANAPI DokanMain(PDOKAN_OPTIONS DokanOptions,
                       PDOKAN_OPERATIONS DokanOperations);

int DOKANAPI DokanCreateFileSystem(
	_In_ PDOKAN_OPTIONS DokanOptions,
	_In_ PDOKAN_OPERATIONS DokanOperations,
	_Out_ DOKAN_HANDLE *DokanInstance);

BOOL DOKANAPI DokanIsFileSystemRunning(_In_ DOKAN_HANDLE DokanInstance);

// See WaitForSingleObject() for a description of return values
DWORD DOKANAPI DokanWaitForFileSystemClosed(
	DOKAN_HANDLE DokanInstance,
	DWORD dwMilliseconds);

void DOKANAPI DokanCloseHandle(DOKAN_HANDLE DokanInstance);

BOOL DOKANAPI DokanUnmount(WCHAR DriveLetter);

BOOL DOKANAPI DokanRemoveMountPoint(LPCWSTR MountPoint);

// DokanIsNameInExpression
//   checks whether Name can match Expression
//   Expression can contain wildcard characters (? and *)
BOOL DOKANAPI DokanIsNameInExpression(LPCWSTR Expression, // matching pattern
                                      LPCWSTR Name,       // file name
                                      BOOL IgnoreCase);

ULONG DOKANAPI DokanVersion();

ULONG DOKANAPI DokanDriverVersion();

// DokanResetTimeout
//   extends the time out of the current IO operation in driver.
BOOL DOKANAPI DokanResetTimeout(ULONG Timeout, // timeout in millisecond
                                PDOKAN_FILE_INFO DokanFileInfo);

// Get the handle to Access Token
// This method needs be called in CreateFile callback.
// The caller must call CloseHandle for the returned handle.
HANDLE DOKANAPI DokanOpenRequestorToken(PDOKAN_CREATE_FILE_EVENT DokanFileInfo);

BOOL DOKANAPI DokanGetMountPointList(PDOKAN_CONTROL list, ULONG length,
                                     BOOL uncOnly, PULONG nbRead);

VOID DOKANAPI DokanMapKernelToUserCreateFileFlags(
    ULONG FileAttributes, ULONG CreateOptions, ULONG CreateDisposition,
    DWORD *outFileAttributesAndFlags, DWORD *outCreationDisposition);

// Translate Win32 Error code to the NtStatus code's corresponding
NTSTATUS DOKANAPI DokanNtStatusFromWin32(DWORD Error);

// Async IO completion functions
void DOKANAPI DokanEndDispatchCreate(DOKAN_CREATE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchRead(DOKAN_READ_FILE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchWrite(DOKAN_WRITE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchFindFiles(DOKAN_FIND_FILES_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchFindFilesWithPattern(DOKAN_FIND_FILES_PATTERN_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchGetFileInformation(DOKAN_GET_FILE_INFO_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchFindStreams(DOKAN_FIND_STREAMS_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchGetVolumeInfo(DOKAN_GET_VOLUME_INFO_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchGetVolumeFreeSpace(DOKAN_GET_DISK_FREE_SPACE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchGetVolumeAttributes(DOKAN_GET_VOLUME_ATTRIBUTES_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchLockFile(DOKAN_LOCK_FILE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchUnlockFile(DOKAN_LOCK_FILE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchSetAllocationSize(DOKAN_SET_ALLOCATION_SIZE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchSetFileBasicInformation(DOKAN_SET_FILE_BASIC_INFO_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchCanDeleteFile(DOKAN_CAN_DELETE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchSetEndOfFile(DOKAN_SET_EOF_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchMoveFile(DOKAN_MOVE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchFlush(DOKAN_FLUSH_BUFFERS_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchGetFileSecurity(DOKAN_GET_FILE_SECURITY_EVENT *EventInfo, NTSTATUS ResultStatus);
void DOKANAPI DokanEndDispatchSetFileSecurity(DOKAN_SET_FILE_SECURITY_EVENT *EventInfo, NTSTATUS ResultStatus);

// Threading helpers
DOKAN_API PTP_POOL DOKAN_CALLBACK DokanGetThreadPool();

// Init/shutdown
void DOKANAPI DokanInit(DOKAN_MEMORY_CALLBACKS *memoryCallbacks);
void DOKANAPI DokanShutdown();

#ifdef __cplusplus
}
#endif

#endif // DOKAN_H_
