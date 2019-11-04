/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2017 Google, Inc.
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

/** Do not include NTSTATUS. Fix  duplicate preprocessor definitions */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include "fileinfo.h"
#include "public.h"

#ifdef _EXPORTING
/** Export dokan API see also dokan.def for export */
#define DOKANAPI __stdcall
#define DOKAN_API
#else
/** Import dokan API */
#define DOKANAPI __declspec(dllimport) __stdcall
#define DOKAN_API __declspec(dllimport)
#endif

/** Change calling convention to standard call */
#define DOKAN_CALLBACK __stdcall

#include "dokan_vector.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @file */

/**
 * \defgroup Dokan Dokan
 * \brief Dokan Library const and methods
 */
/** @{ */

/** The current Dokan version (ver 1.2.0). \ref DOKAN_OPTIONS.Version */
#define DOKAN_VERSION 130
/** Minimum Dokan version (ver 1.1.0) accepted. */
#define DOKAN_MINIMUM_COMPATIBLE_VERSION 110
/** Driver file name including the DOKAN_MAJOR_API_VERSION */
#define DOKAN_DRIVER_NAME L"dokan" DOKAN_MAJOR_API_VERSION L".sys"
/** Network provider name including the DOKAN_MAJOR_API_VERSION */
#define DOKAN_NP_NAME L"Dokan" DOKAN_MAJOR_API_VERSION

/** @} */

/**
 * \defgroup DOKAN_OPTION DOKAN_OPTION
 * \brief All DOKAN_OPTION flags used in DOKAN_OPTIONS.Options
 * \see DOKAN_FILE_INFO
 */
/** @{ */

/** Enable ouput debug message */
#define DOKAN_OPTION_DEBUG					1

/** Enable ouput debug message to stderr */
#define DOKAN_OPTION_STDERR					(1 << 1)

/**
 * Enable the use of alternate stream paths in the form
 * <file-name>:<stream-name>. If this is not specified then the driver will
 * fail any attempt to access a path with a colon.
 */
#define DOKAN_OPTION_ALT_STREAM				(1 << 2)

/** Enable mount drive as write-protected */
#define DOKAN_OPTION_WRITE_PROTECT			(1 << 3)

/** Use network drive - Dokan network provider needs to be installed */
#define DOKAN_OPTION_NETWORK				(1 << 4)

/** Use removable drive */
#define DOKAN_OPTION_REMOVABLE				(1 << 5)	// use removable drive

/** Use mount manager */
#define DOKAN_OPTION_MOUNT_MANAGER			(1 << 6)	// use mount manager

/** Mount the drive on current session only */
#define DOKAN_OPTION_CURRENT_SESSION		(1 << 7)	// mount the drive on current session only

/** Enable Lockfile/Unlockfile operations. Otherwise Dokan will take care of it */
#define DOKAN_OPTION_FILELOCK_USER_MODE		(1 << 8)	// FileLock in User Mode

/**
 * Whether DokanNotifyXXX functions should be enabled, which requires this
 * library to maintain a special handle while the file system is mounted.
 * Without this flag, the functions always return FALSE if invoked.
 */
#define DOKAN_OPTION_ENABLE_NOTIFICATION_API (1 << 9)

/**
 * Whether to disable any oplock support on the volume.
 * Regular range locks are enabled regardless.
 */
#define DOKAN_OPTION_DISABLE_OPLOCKS (1 << 10)
/**
 * Whether to satisfy a single-entry, name-only directory search without
 * dispatching to the FindFiles callback, if there is an open file from which
 * the driver can just copy the normalized name. These searches are frequently
 * done inside of CreateFile calls on Windows 7.
 */
#define DOKAN_OPTION_OPTIMIZE_SINGLE_NAME_SEARCH (1 << 11)

/** Dokan uses a single thread. */
#define DOKAN_OPTION_FORCE_SINGLE_THREADED (1 << 12)

/** @} */

typedef void *DOKAN_HANDLE, **PDOKAN_HANDLE;

/**
 * \struct DOKAN_OPTIONS
 * \brief Dokan mount options used to describe Dokan device behavior.
 * \see DokanMain
 */
typedef struct _DOKAN_OPTIONS {
  /** Version of the Dokan features requested without dots (version "123" is equal to Dokan version 1.2.3). */
  USHORT Version;
  /** Number of threads to be used by Dokan library internally. More threads will handle more events at the same time. */
  USHORT ThreadCount;
  /** Features enabled for the mount. See \ref DOKAN_OPTION. */
  ULONG Options;
  /** FileSystem can store anything here. */
  ULONG64 GlobalContext;
  /** Mount point. It can be a driver letter like "M:\" or a folder path "C:\mount\dokan" on a NTFS partition. */
  LPCWSTR MountPoint;
  /**
   * UNC Name for the Network Redirector
   * \see <a href="https://msdn.microsoft.com/en-us/library/windows/hardware/ff556761(v=vs.85).aspx">Support for UNC Naming</a>
   */
  LPCWSTR UNCName;
  /**
   * Max timeout in milliseconds of each request before Dokan gives up to wait events to complete.
   * A timeout request is a sign that the userland implementation is no longer able to properly manage requests in time.
   * The driver will therefore unmount the device when a timeout trigger in order to keep the system stable.
   */
  ULONG Timeout;
  /** Allocation Unit Size of the volume. This will affect the file size. */
  ULONG AllocationUnitSize;
  /** Sector Size of the volume. This will affect the file size. */
  ULONG SectorSize;
} DOKAN_OPTIONS, *PDOKAN_OPTIONS;

/**
 * \struct DOKAN_FILE_INFO
 * \brief Dokan file information on the current operation.
 */
typedef struct _DOKAN_FILE_INFO {
  /**
   * Context that can be used to carry information between operations.
   * The context can carry whatever type like \c HANDLE, struct, int,
   * internal reference that will help the implementation understand the request context of the event.
   */
  ULONG64						Context;

  /** Reserved. Used internally by Dokan library. Never modify. */
  PVOID							DokanContext;

  /** A pointer to DOKAN_OPTIONS which was passed to DokanMain. */
  PDOKAN_OPTIONS				        DokanOptions;

  /**
   * Requesting a directory file.
   * Must be set in \ref DOKAN_OPERATIONS.ZwCreateFile if the file appears to be a folder.
   */
  BOOL							IsDirectory;

  /**
   * Process ID for the thread that originally requested a given I/O operation.
   */
  ULONG							ProcessId;

  /** Flag if the file has to be deleted during DOKAN_OPERATIONS. Cleanup event. */
  UCHAR							DeleteOnClose;

  /** Read or write is paging IO. */
  UCHAR							PagingIo;

  /** Read or write is synchronous IO. */
  UCHAR							SynchronousIo;

  /** Read or write directly from data source without cache */
  UCHAR							Nocache;

  /**  If \c TRUE, write to the current end of file instead of using the Offset parameter. */
  UCHAR							WriteToEndOfFile;

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

typedef void (WINAPI *PDokanDbgPrint)(LPCSTR logString);
typedef void (WINAPI *PDokanDbgPrintW)(LPCWSTR logString);

typedef struct _DOKAN_MEMORY_CALLBACKS {
	PDokanMalloc	Malloc;
	PDokanFree		Free;
	PDokanRealloc	Realloc;
} DOKAN_MEMORY_CALLBACKS, *PDOKAN_MEMORY_CALLBACKS;

typedef struct _DOKAN_LOG_CALLBACKS
{
	PDokanDbgPrint DbgPrint;
	PDokanDbgPrintW DbgPrintW;
} DOKAN_LOG_CALLBACKS, *PDOKAN_LOG_CALLBACKS;

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

/**
 * \brief FillFindData Used to add an entry in FindFiles operation
 * \return 1 if buffer is full, otherwise 0 (currently it never returns 1)
 */
typedef int (WINAPI *PFillFindData)(PDOKAN_FIND_FILES_EVENT, PWIN32_FIND_DATAW);

// FillFindDataWithPattern
//   is used to add an entry in FindFilesWithPattern
//   returns 1 if buffer is full, otherwise 0
//   (currently it never returns 1)
typedef int (WINAPI *PFillFindDataWithPattern)(PDOKAN_FIND_FILES_PATTERN_EVENT, PWIN32_FIND_DATAW);

typedef enum _DOKAN_STREAM_FIND_RESULT {

	DOKAN_STREAM_BUFFER_CONTINUE = 0,
	DOKAN_STREAM_BUFFER_FULL = 1
} DOKAN_STREAM_FIND_RESULT, *PDOKAN_STREAM_FIND_RESULT;

/**
 * \brief FillFindStreamData Used to add an entry in FindStreams
 * \return 1 if buffer is full, otherwise 0 (currently it never returns 1)
 */
typedef DOKAN_STREAM_FIND_RESULT (WINAPI *PFillFindStreamData)(PDOKAN_FIND_STREAMS_EVENT, PWIN32_FIND_STREAM_DATA);

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
	LPCWSTR							FileName;
	PSECURITY_DESCRIPTOR			SecurityDescriptor; // A pointer to SECURITY_DESCRIPTOR buffer to be filled
	SECURITY_INFORMATION			SecurityInformation; // The types of security information being requested
	ULONG							SecurityDescriptorSize; // length of Security descriptor buffer
	ULONG							LengthNeeded;
} DOKAN_GET_FILE_SECURITY_EVENT, *PDOKAN_GET_FILE_SECURITY_EVENT;

typedef struct _DOKAN_SET_FILE_SECURITY_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	PSECURITY_DESCRIPTOR			SecurityDescriptor; // A pointer to SECURITY_DESCRIPTOR buffer to be filled
	SECURITY_INFORMATION			SecurityInformation;
	ULONG							SecurityDescriptorSize; // length of Security descriptor buffer
} DOKAN_SET_FILE_SECURITY_EVENT, *PDOKAN_SET_FILE_SECURITY_EVENT;

typedef struct _DOKAN_FIND_STREAMS_EVENT {
	PDOKAN_FILE_INFO				DokanFileInfo;
	LPCWSTR							FileName;
	PFillFindStreamData				FillFindStreamData; // call this function with PWIN32_FIND_STREAM_DATA
} DOKAN_FIND_STREAMS_EVENT, *PDOKAN_FIND_STREAMS_EVENT;

// clang-format off

/**
 * \struct DOKAN_OPERATIONS
 * \brief Dokan API callbacks interface
 *
 * DOKAN_OPERATIONS is a struct of callbacks that describe all Dokan API operations
 * that will be called when Windows access to the filesystem.
 *
 * If an error occurs, return NTSTATUS (https://support.microsoft.com/en-us/kb/113996).
 * Win32 Error can be converted to \c NTSTATUS with \ref DokanNtStatusFromWin32
 *
 * All callbacks can be set to \c NULL or return \c STATUS_NOT_IMPLEMENTED
 * if supporting one of them is not desired. Be aware that returning such values to important callbacks
 * such as DOKAN_OPERATIONS.ZwCreateFile / DOKAN_OPERATIONS.ReadFile / ... would make the filesystem not work or become unstable.
 */
typedef struct _DOKAN_OPERATIONS {


  /**
  * \brief CreateFile Dokan API callback
  *
  * CreateFile is called each time a request is made on a file system object.
  *
  * In case \c OPEN_ALWAYS & \c CREATE_ALWAYS are successfully opening an
  * existing file, \c STATUS_OBJECT_NAME_COLLISION should be returned instead of \c STATUS_SUCCESS .
  * This will inform Dokan that the file has been opened and not created during the request.
  *
  * If the file is a directory, CreateFile is also called.
  * In this case, CreateFile should return \c STATUS_SUCCESS when that directory
  * can be opened and DOKAN_FILE_INFO.IsDirectory has to be set to \c TRUE.
  * On the other hand, if DOKAN_FILE_INFO.IsDirectory is set to \c TRUE
  * but the path targets a file, \c STATUS_NOT_A_DIRECTORY must be returned.
  *
  * DOKAN_FILE_INFO.Context can be used to store Data (like \c HANDLE)
  * that can be retrieved in all other requests related to the Context.
  * To avoid memory leak, Context needs to be released in DOKAN_OPERATIONS.Cleanup.
  *
  * \param EventInfo Information about the event.
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param SecurityContext SecurityContext, see https://msdn.microsoft.com/en-us/library/windows/hardware/ff550613(v=vs.85).aspx
  * \param DesiredAccess Specifies an <a href="https://msdn.microsoft.com/en-us/library/windows/hardware/ff540466(v=vs.85).aspx">ACCESS_MASK</a> value that determines the requested access to the object.
  * \param FileAttributes Specifies one or more FILE_ATTRIBUTE_XXX flags, which represent the file attributes to set if a file is created or overwritten.
  * \param ShareAccess Type of share access, which is specified as zero or any combination of FILE_SHARE_* flags.
  * \param CreateDisposition Specifies the action to perform if the file does or does not exist.
  * \param CreateOptions Specifies the options to apply when the driver creates or opens the file.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see <a href="https://msdn.microsoft.com/en-us/library/windows/hardware/ff566424(v=vs.85).aspx">See ZwCreateFile for more information about the parameters of this callback (MSDN).</a>
  * \see DokanMapKernelToUserCreateFileFlags
  */
NTSTATUS(DOKAN_CALLBACK *ZwCreateFile)(_In_ DOKAN_CREATE_FILE_EVENT *EventInfo);
  
  /**
  * \brief Cleanup Dokan API callback
  *
  * Cleanup request before \ref CloseFile is called.
  *
  * When DOKAN_FILE_INFO.DeleteOnClose is \c TRUE, the file in Cleanup must be deleted.
  * See DeleteFile documentation for explanation.
  *
  * \param EventInfo Information about the event.
  * \see DeleteFile
  * \see DeleteDirectory
  */
  void(DOKAN_CALLBACK *Cleanup)(_In_ DOKAN_CLEANUP_EVENT *EventInfo);

  /**
  * \brief CloseFile Dokan API callback
  *
  * Clean remaining Context
  *
  * CloseFile is called at the end of the life of the context.
  * Anything remaining in \ref DOKAN_FILE_INFO.Context must be cleared before returning.
  *
  * \param EventInfo Information about the event.
  */
  void(DOKAN_CALLBACK *CloseFile)(_In_ DOKAN_CLOSE_FILE_EVENT *EventInfo);

  /**
  * \brief ReadFile Dokan API callback
  *
  * ReadFile callback on the file previously opened in DOKAN_OPERATIONS.ZwCreateFile.
  * It can be called by different threads at the same time, so the read/context has to be thread safe.
  *
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param Buffer Read buffer that has to be filled with the read result.
  * \param BufferLength Buffer length and read size to continue with.
  * \param ReadLength Total data size that has been read.
  * \param Offset Offset from where the read has to be continued.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see WriteFile
  */
  NTSTATUS(DOKAN_CALLBACK *ReadFile)(_In_ DOKAN_READ_FILE_EVENT *EventInfo);

  /**
  * \brief WriteFile Dokan API callback
  *
  * WriteFile callback on the file previously opened in DOKAN_OPERATIONS.ZwCreateFile
  * It can be called by different threads at the same time, sp the write/context has to be thread safe.
  *
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param Buffer Data that has to be written.
  * \param NumberOfBytesToWrite Buffer length and write size to continue with.
  * \param NumberOfBytesWritten Total number of bytes that have been written.
  * \param Offset Offset from where the write has to be continued.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see ReadFile
  */
  NTSTATUS(DOKAN_CALLBACK *WriteFile)(_In_ DOKAN_WRITE_FILE_EVENT *EventInfo);

  /**
  * \brief FlushFileBuffers Dokan API callback
  *
  * Clears buffers for this context and causes any buffered data to be written to the file.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *FlushFileBuffers)(_In_ DOKAN_FLUSH_BUFFERS_EVENT *EventInfo);

  /**
  * \brief GetFileInformation Dokan API callback
  *
  * Get specific information on a file.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *GetFileInformation)(_In_ DOKAN_GET_FILE_INFO_EVENT *EventInfo);

  /**
  * \brief GetFileInformation Dokan API callback
  *
  * List all files in the requested path
  * \ref DOKAN_OPERATIONS.FindFilesWithPattern is checked first. If it is not implemented or
  * returns \c STATUS_NOT_IMPLEMENTED, then FindFiles is called, if implemented.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *FindFiles)(_In_ DOKAN_FIND_FILES_EVENT *EventInfo);

  /**
  * \brief FindFilesWithPattern Dokan API callback
  *
  * Same as \ref DOKAN_OPERATIONS.FindFiles but with a search pattern.\n
  * The search pattern is a Windows MS-DOS-style expression. See \ref DokanIsNameInExpression .
  *
  * \param PathName Path requested by the Kernel on the FileSystem.
  * \param SearchPattern Search pattern.
  * \param FillFindData Callback that has to be called with PWIN32_FIND_DATAW that contains file information.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see FindFiles
  * \see DokanIsNameInExpression
  */
  NTSTATUS(DOKAN_CALLBACK *FindFilesWithPattern)(_In_ DOKAN_FIND_FILES_PATTERN_EVENT *EventInfo);

  /**
  * \brief SetFileBasicInformation Dokan API callback
  *
  * Set basic information for a file.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see FindFiles
  */
  NTSTATUS(DOKAN_CALLBACK *SetFileBasicInformation)(_In_ DOKAN_SET_FILE_BASIC_INFO_EVENT *EventInfo);

  /**
  * \brief DeleteFile Dokan API callback
  *
  * Check if it is possible to delete a file.
  *
  * DeleteFile will also be called with DOKAN_FILE_INFO.DeleteOnClose set to \c FALSE
  * to notify the driver when the file is no longer requested to be deleted.
  *
  * The file in DeleteFile should not be deleted, but instead the file
  * must be checked as to whether or not it can be deleted,
  * and \c STATUS_SUCCESS should be returned (when it can be deleted) or
  * appropriate error codes, such as \c STATUS_ACCESS_DENIED or
  * \c STATUS_OBJECT_NAME_NOT_FOUND, should be returned.
  *
  * When \c STATUS_SUCCESS is returned, a Cleanup call is received afterwards with
  * DOKAN_FILE_INFO.DeleteOnClose set to \c TRUE. Only then must the closing file
  * be deleted.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see Cleanup
  */
  NTSTATUS(DOKAN_CALLBACK *CanDeleteFile)(_In_ DOKAN_CAN_DELETE_FILE_EVENT *EventInfo);

  /**
  * \brief MoveFile Dokan API callback
  *
  * Move a file or directory to a new destination
  *
  * \param FileName Path for the file to be moved.
  * \param NewFileName Path for the new location of the file.
  * \param ReplaceIfExisting If destination already exists, can it be replaced?
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *MoveFileW)(_In_ DOKAN_MOVE_FILE_EVENT *EventInfo);

  /**
  * \brief SetEndOfFile Dokan API callback
  *
  * SetEndOfFile is used to truncate or extend a file (physical file size).
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *SetEndOfFile)(_In_ DOKAN_SET_EOF_EVENT *EventInfo);

  /**
  * \brief SetAllocationSize Dokan API callback
  *
  * SetAllocationSize is used to truncate or extend a file.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *SetAllocationSize)(_In_ DOKAN_SET_ALLOCATION_SIZE_EVENT *EventInfo);

  /**
  * \brief LockFile Dokan API callback
  *
  * Lock file at a specific offset and data length.
  * This is only used if \ref DOKAN_OPTION_FILELOCK_USER_MODE is enabled.
  *
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param ByteOffset Offset from where the lock has to be continued.
  * \param Length Data length to lock.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see UnlockFile
  */
  NTSTATUS(DOKAN_CALLBACK *LockFile)(_In_ DOKAN_LOCK_FILE_EVENT *EventInfo);

  /**
  * \brief UnlockFile Dokan API callback
  *
  * Unlock file at a specific offset and data length.
  * This is only used if \ref DOKAN_OPTION_FILELOCK_USER_MODE is enabled.
  *
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param ByteOffset Offset from where the lock has to be continued.
  * \param Length Data length to lock.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see LockFile
  */
  NTSTATUS(DOKAN_CALLBACK *UnlockFile)(_In_ DOKAN_UNLOCK_FILE_EVENT *EventInfo);

  /**
  * \brief GetVolumeFreeSpace Dokan API callback
  *
  * Retrieves information about the amount of space that is available on a disk volume.
  * It consits of the total amount of space, the total amount of free space, and
  * the total amount of free space available to the user that is associated with the calling thread.
  *
  * Neither GetDiskFreeSpace nor \ref GetVolumeInformation
  * save the  DOKAN_FILE_INFO.Context.
  * Before these methods are called, \ref ZwCreateFile may not be called.
  * (ditto \ref CloseFile and \ref Cleanup)
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or \c NTSTATUS appropriate to the request result.
  * \see <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa364937(v=vs.85).aspx"> GetDiskFreeSpaceEx function (MSDN)</a>
  * \see GetVolumeInformation
  */
  NTSTATUS(DOKAN_CALLBACK *GetVolumeFreeSpace)(_In_ DOKAN_GET_DISK_FREE_SPACE_EVENT *EventInfo);


  /**
  * \brief GetVolumeInformationW Dokan API callback
  *
  * Retrieves information about the file system and volume associated with the specified root directory.
  *
  * Neither GetVolumeInformation nor GetDiskFreeSpace
  * save the \ref DOKAN_FILE_INFO#Context.
  * Before these methods are called, \ref ZwCreateFile may not be called.
  * (ditto \ref CloseFile and \ref Cleanup)
  *
  * FileSystemName could be anything up to 10 characters.
  * But Windows check few feature availability based on file system name.
  * For this, it is recommended to set NTFS or FAT here.
  *
  * \c FILE_READ_ONLY_VOLUME is automatically added to the
  * FileSystemFlags if \ref DOKAN_OPTION_WRITE_PROTECT was
  * specified in DOKAN_OPTIONS when the volume was mounted.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa364993(v=vs.85).aspx"> GetVolumeInformation function (MSDN)</a>
  * \see GetDiskFreeSpace
  */
  NTSTATUS(DOKAN_CALLBACK *GetVolumeInformationW)(_In_ DOKAN_GET_VOLUME_INFO_EVENT *EventInfo);

  /**
  * \brief GetVolumeAttributes Dokan API callback
  *
  * Called when the file system needs to get the attributes for a volume.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *GetVolumeAttributes)(_In_ DOKAN_GET_VOLUME_ATTRIBUTES_EVENT *EventInfo);

  /**
  * \brief Mounted Dokan API callback
  *
  * Called when Dokan successfully mounts the volume.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see Unmounted
  */
  void(DOKAN_CALLBACK *Mounted)(DOKAN_MOUNTED_INFO *EventInfo);

  /**
  * \brief Unmounted Dokan API callback
  *
  * Called when Dokan is unmounting the volume.
  *
  * \param EventInfo Information about the event.
  * \return \c STATUS_SUCCESS on success or \c NTSTATUS appropriate to the request result.
  * \see Mounted
  */
  void(DOKAN_CALLBACK *Unmounted)(DOKAN_UNMOUNTED_INFO *EventInfo);

  /**
  * \brief GetFileSecurity Dokan API callback
  *
  * Get specified information about the security of a file or directory.
  *
  * Return \c STATUS_NOT_IMPLEMENTED to let dokan library build a sddl of the current process user with authenticate user rights for context menu.
  * Return \c STATUS_BUFFER_OVERFLOW if buffer size is too small.
  *
  * \since Supported since version 0.6.0. The version must be specified in \ref DOKAN_OPTIONS.Version.
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param SecurityInformation A SECURITY_INFORMATION value that identifies the security information being requested.
  * \param SecurityDescriptor A pointer to a buffer that receives a copy of the security descriptor of the requested file.
  * \param BufferLength Specifies the size, in bytes, of the buffer.
  * \param LengthNeeded A pointer to the variable that receives the number of bytes necessary to store the complete security descriptor.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see SetFileSecurityW
  * \see <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa446639(v=vs.85).aspx">GetFileSecurity function (MSDN)</a>
  */
  NTSTATUS(DOKAN_CALLBACK *GetFileSecurityW)(_In_ DOKAN_GET_FILE_SECURITY_EVENT *EventInfo);

  /**
  * \brief SetFileSecurity Dokan API callback
  *
  * Sets the security of a file or directory object.
  *
  * \since Supported since version 0.6.0. The version must be specified in \ref DOKAN_OPTIONS.Version.
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param SecurityInformation Structure that identifies the contents of the security descriptor pointed by \a SecurityDescriptor param.
  * \param SecurityDescriptor A pointer to a SECURITY_DESCRIPTOR structure.
  * \param BufferLength Specifies the size, in bytes, of the buffer.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  * \see GetFileSecurityW
  * \see <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa379577(v=vs.85).aspx">SetFileSecurity function (MSDN)</a>
  */
  NTSTATUS(DOKAN_CALLBACK *SetFileSecurityW)(_In_ DOKAN_SET_FILE_SECURITY_EVENT *EventInfo);

  /**
  * \brief FindStreams Dokan API callback
  *
  * Retrieve all NTFS Streams informations on the file.
  * This is only called if \ref DOKAN_OPTION_ALT_STREAM is enabled.
  *
  * \since Supported since version 0.8.0. The version must be specified in \ref DOKAN_OPTIONS.Version.
  * \param FileName File path requested by the Kernel on the FileSystem.
  * \param FillFindStreamData Callback that has to be called with PWIN32_FIND_STREAM_DATA that contain stream information.
  * \param DokanFileInfo Information about the file or directory.
  * \return \c STATUS_SUCCESS on success or NTSTATUS appropriate to the request result.
  */
  NTSTATUS(DOKAN_CALLBACK *FindStreams)(_In_ DOKAN_FIND_STREAMS_EVENT *EventInfo);

} DOKAN_OPERATIONS, *PDOKAN_OPERATIONS;

// clang-format on

/**
 * \defgroup DokanMainResult DokanMainResult
 * \brief \ref DokanMain returns error codes
 */
/** @{ */

/** Dokan mount succeed. */
#define DOKAN_SUCCESS 0
/** Dokan mount error. */
#define DOKAN_ERROR -1
/** Dokan mount failed - Bad drive letter. */
#define DOKAN_DRIVE_LETTER_ERROR -2
/** Dokan mount failed - Can't install driver.  */
#define DOKAN_DRIVER_INSTALL_ERROR -3
/** Dokan mount failed - Driver answer that something is wrong. */
#define DOKAN_START_ERROR -4
/**
 * Dokan mount failed.
 * Can't assign a drive letter or mount point.
 * Probably already used by another volume.
 */
#define DOKAN_MOUNT_ERROR -5
/**
 * Dokan mount failed.
 * Mount point is invalid.
 */
#define DOKAN_MOUNT_POINT_ERROR -6
/**
 * Dokan mount failed.
 * Requested an incompatible version.
 */
#define DOKAN_VERSION_ERROR -7

/** @} */

#define DOKAN_SUCCEEDED(x) ((x) == DOKAN_SUCCESS)
#define DOKAN_FAILED(x) ((x) != DOKAN_SUCCESS)

/**
 * \defgroup Dokan Dokan
 */
/** @{ */

/**
 * \brief Mount a new Dokan Volume.
 *
 * This function block until the device is unmounted.
 * If the mount fails, it will directly return a \ref DokanMainResult error.
 *
 * \param DokanOptions a \ref DOKAN_OPTIONS that describe the mount.
 * \param DokanOperations Instance of \ref DOKAN_OPERATIONS that will be called for each request made by the kernel.
 * \return \ref DokanMainResult status.
 */
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

/**
 * \brief Unmount a Dokan device from a driver letter.
 *
 * \param DriveLetter Dokan driver letter to unmount.
 * \return \c TRUE if device was unmounted or \c FALSE in case of failure or device not found.
 */
BOOL DOKANAPI DokanUnmount(WCHAR DriveLetter);

/**
 * \brief Unmount a Dokan device from a mount point
 *
 * \param MountPoint Mount point to unmount ("Z", "Z:", "Z:\", "Z:\MyMountPoint").
 * \return \c TRUE if device was unmounted or \c FALSE in case of failure or device not found.
 */
BOOL DOKANAPI DokanRemoveMountPoint(LPCWSTR MountPoint);

/**
<<<<<<< HEAD
=======
 * \brief Unmount a Dokan device from a mount point
 *
 * Same as \ref DokanRemoveMountPoint
 * If Safe is \c TRUE, it will broadcast to all desktops and Shells
 * Safe should not be used during DLL_PROCESS_DETACH
 *
 * \see DokanRemoveMountPoint
 *
 * \param MountPoint Mount point to unmount ("Z", "Z:", "Z:\", "Z:\MyMountPoint").
 * \param Safe Process is not in DLL_PROCESS_DETACH state.
 * \return \c TRUE if device was unmounted or \c FALSE in case of failure or device not found.
 */
BOOL DOKANAPI DokanRemoveMountPointEx(LPCWSTR MountPoint, BOOL Safe);

/**
>>>>>>> master
 * \brief Checks whether Name matches Expression
 *
 * Behave like \c FsRtlIsNameInExpression routine from <a href="https://msdn.microsoft.com/en-us/library/ff546850(v=VS.85).aspx">Microsoft</a>\n
 * \c * (asterisk) Matches zero or more characters.\n
 * <tt>?</tt> (question mark) Matches a single character.\n
 * \c DOS_DOT (\c " quotation mark) Matches either a period or zero characters beyond the name string.\n
 * \c DOS_QM (\c > greater than) Matches any single character or, upon encountering a period or end
 *        of name string, advances the expression to the end of the set of
 *        contiguous DOS_QMs.\n
 * \c DOS_STAR (\c < less than) Matches zero or more characters until encountering and matching
 *          the final \c . in the name.
 *
 * \param Expression Expression can contain any of the above characters.
 * \param Name Name to check
 * \param IgnoreCase Case sensitive or not
 * \return result if name matches the expression
 */
BOOL DOKANAPI DokanIsNameInExpression(LPCWSTR Expression, LPCWSTR Name,
                                      BOOL IgnoreCase);

/**
 * \brief Get the version of Dokan.
 * The returned ULONG is the version number without the dots.
 * \return The version of Dokan
 */
ULONG DOKANAPI DokanVersion();

/**
 * \brief Get the version of the Dokan driver.
 * The returned ULONG is the version number without the dots.
 * \return The version of Dokan driver.
 */
ULONG DOKANAPI DokanDriverVersion();

/**
 * \brief Extends the timeout of the current IO operation in driver.
 *
 * \param Timeout Extended time in milliseconds requested.
 * \param DokanFileInfo \ref DOKAN_FILE_INFO of the operation to extend.
 * \return If the operation was successful.
 */
BOOL DOKANAPI DokanResetTimeout(ULONG Timeout, PDOKAN_FILE_INFO DokanFileInfo);

/**
 * \brief Get the handle to Access Token.
 *
 * This method needs be called in <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx">CreateFile</a> callback.
 * The caller must call <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/ms724211(v=vs.85).aspx">CloseHandle</a>
 * for the returned handle.
 *
 * \param DokanFileInfo \ref DOKAN_FILE_INFO of the operation to extend.
 * \return A handle to the account token for the user on whose behalf the code is running.
 */
HANDLE DOKANAPI DokanOpenRequestorToken(PDOKAN_FILE_INFO DokanFileInfo);

/**
 * \brief Get active Dokan mount points.
 *
 * Returned array need to be released by calling \ref DokanReleaseMountPointList
 *
 * \param uncOnly Get only instances that have UNC Name.
 * \param nbRead Number of instances successfully retrieved.
 * \return Allocate array of \ref DOKAN_CONTROL.
 */
PDOKAN_CONTROL DOKANAPI DokanGetMountPointList(BOOL uncOnly, PULONG nbRead);

/**
 * \brief Release Mount point list resources from \ref DokanGetMountPointList.
 *
 * \param Allocate array of \ref DOKAN_CONTROL from \ref DokanGetMountPointList.
 */
VOID DOKANAPI DokanReleaseMountPointList(PDOKAN_CONTROL list);

/**
 * \brief Convert \ref DOKAN_OPERATIONS.ZwCreateFile parameters to <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx">CreateFile</a> parameters.
 *
 * Dokan Kernel forward the DesiredAccess directly from the IRP_MJ_CREATE.
 * This DesiredAccess has been converted from generic rights (user CreateFile request) to standard rights and will be converted back here.
 * https://msdn.microsoft.com/windows/hardware/drivers/ifs/access-mask
 *
 * \param DesiredAccess DesiredAccess from \ref DOKAN_OPERATIONS.ZwCreateFile.
 * \param FileAttributes FileAttributes from \ref DOKAN_OPERATIONS.ZwCreateFile.
 * \param CreateOptions CreateOptions from \ref DOKAN_OPERATIONS.ZwCreateFile.
 * \param CreateDisposition CreateDisposition from \ref DOKAN_OPERATIONS.ZwCreateFile.
 * \param outDesiredAccess New <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx">CreateFile</a> dwDesiredAccess.
 * \param outFileAttributesAndFlags New <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx">CreateFile</a> dwFlagsAndAttributes.
 * \param outCreationDisposition New <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx">CreateFile</a> dwCreationDisposition.
 * \see <a href="https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx">CreateFile function (MSDN)</a>
 */
void DOKANAPI DokanMapKernelToUserCreateFileFlags(
				DOKAN_CREATE_FILE_EVENT *EventInfo,
				ACCESS_MASK* outDesiredAccess,
				DWORD *outFileAttributesAndFlags,
				DWORD *outCreationDisposition);

/**
 * \defgroup DokanNotify Dokan Notify
 * \brief Dokan User FS file-change notification
 *
 * The application implementing the user file system can notify
 * the Dokan kernel driver of external file- and directory-changes.
 *
 * For example, the mirror application can notify the driver about
 * changes made in the mirrored directory so that those changes will
 * be automatically reflected in the implemented mirror file system.
 *
 * This requires the FilePath passed to the respective DokanNotify*-functions
 * to include the absolute path of the changed file including the drive-letter
 * and the path to the mount point, e.g. "C:\Dokan\ChangedFile.txt".
 *
 * These functions SHOULD NOT be called from within the implemented
 * file system and thus be independent of any Dokan file system operation.
 * @{
 */

/**
 * \brief Notify dokan that a file or a directory has been created.
 *
 * \param FilePath Absolute path to the file or directory, including the mount-point of the file system.
 * \param IsDirectory Indicates if the path is a directory.
 * \return \c TRUE if notification succeeded.
 */
BOOL DOKANAPI DokanNotifyCreate(LPCWSTR FilePath, BOOL IsDirectory);

/**
 * \brief Notify dokan that a file or a directory has been deleted.
 *
 * \param FilePath Absolute path to the file or directory, including the mount-point of the file system.
 * \param IsDirectory Indicates if the path was a directory.
 * \return \c TRUE if notification succeeded.
 */
BOOL DOKANAPI DokanNotifyDelete(LPCWSTR FilePath, BOOL IsDirectory);

/**
 * \brief Notify dokan that file or directory attributes have changed.
 *
 * \param FilePath Absolute path to the file or directory, including the mount-point of the file system.
 * \return \c TRUE if notification succeeded.
 */
BOOL DOKANAPI DokanNotifyUpdate(LPCWSTR FilePath);

/**
 * \brief Notify dokan that file or directory extended attributes have changed.
 *
 * \param FilePath Absolute path to the file or directory, including the mount-point of the file system.
 * \return \c TRUE if notification succeeded.
 */
BOOL DOKANAPI DokanNotifyXAttrUpdate(LPCWSTR FilePath);

/**
 * \brief Notify dokan that a file or a directory has been renamed. This method
 *  supports in-place rename for file/directory within the same parent.
 *
 * \param OldPath Old, absolute path to the file or directory, including the mount-point of the file system.
 * \param NewPath New, absolute path to the file or directory, including the mount-point of the file system.
 * \param IsDirectory Indicates if the path is a directory.
 * \param IsInSameFolder Indicates if the file or directory have the same parent directory.
 * \return \c TRUE if notification succeeded.
 */
BOOL DOKANAPI DokanNotifyRename(LPCWSTR OldPath, LPCWSTR NewPath,
                                BOOL IsDirectory, BOOL IsInSameDirectory);

/**@}*/

/**
 * \brief Convert WIN32 error to NTSTATUS
 *
 * https://support.microsoft.com/en-us/kb/113996
 *
 * \param Error Win32 Error to convert
 * \return NTSTATUS associate to the ERROR.
 */
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
void DOKANAPI DokanInit(DOKAN_MEMORY_CALLBACKS* memoryCallbacks, DOKAN_LOG_CALLBACKS* logCallbacks);
void DOKANAPI DokanShutdown();
/** @} */

#ifdef __cplusplus
}
#endif

#endif // DOKAN_H_
