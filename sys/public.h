/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2017 - 2018 Google, Inc.
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

#ifndef PUBLIC_H_
#define PUBLIC_H_

#ifndef DOKAN_MAJOR_API_VERSION
#define DOKAN_MAJOR_API_VERSION L"1"
#include <minwindef.h>
#endif

#define DOKAN_DRIVER_VERSION 0x0000190

#define EVENT_CONTEXT_MAX_SIZE (1024 * 32)

#define IOCTL_GET_VERSION                                                      \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_SET_DEBUG_MODE                                                   \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_EVENT_WAIT                                                       \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_EVENT_INFO                                                       \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_EVENT_RELEASE                                                    \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_EVENT_START                                                      \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_EVENT_WRITE                                                      \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

#define IOCTL_KEEPALIVE                                                        \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_SERVICE_WAIT                                                     \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_RESET_TIMEOUT                                                    \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_GET_ACCESS_TOKEN                                                 \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_EVENT_MOUNTPOINT_LIST                                            \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80D, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_MOUNTPOINT_CLEANUP                                               \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80E, METHOD_BUFFERED, FILE_ANY_ACCESS)

// DeviceIoControl code to send to a keepalive handle to activate it (see the
// documentation for the keepalive flags in the DokanFCB struct).
#define FSCTL_ACTIVATE_KEEPALIVE                                               \
  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x80F, METHOD_BUFFERED, FILE_ANY_ACCESS)

// DeviceIoControl code to send path notification request.
#define FSCTL_NOTIFY_PATH                                                      \
  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

// DeviceIoControl code to retrieve the VOLUME_METRICS struct for the targeted
// volume.
#define IOCTL_GET_VOLUME_METRICS                                               \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define DRIVER_FUNC_INSTALL 0x01
#define DRIVER_FUNC_REMOVE 0x02

#define DOKAN_MOUNTED 1
#define DOKAN_USED 2
#define DOKAN_START_FAILED 3

#define DOKAN_DEVICE_MAX 10

#define DOKAN_DEFAULT_SECTOR_SIZE 512
#define DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE 512
#define DOKAN_DEFAULT_DISK_SIZE 1024 * 1024 * 1024

// used in CCB->Flags and FCB->Flags
#define DOKAN_FILE_DIRECTORY 1
#define DOKAN_FILE_DELETED 2
#define DOKAN_FILE_OPENED 4
#define DOKAN_DIR_MATCH_ALL 8
#define DOKAN_DELETE_ON_CLOSE 16
#define DOKAN_PAGING_IO 32
#define DOKAN_SYNCHRONOUS_IO 64
#define DOKAN_WRITE_TO_END_OF_FILE 128
#define DOKAN_NOCACHE 256
#define DOKAN_RETRY_CREATE 512
#define DOKAN_FILE_CHANGE_LAST_WRITE 1024

// used in DOKAN_START->DeviceType
#define DOKAN_DISK_FILE_SYSTEM 0
#define DOKAN_NETWORK_FILE_SYSTEM 1

// Special files that are tagged for specfic FS purpose when their FCB is init.
// Note: This file names can no longer be used by userland FS correctly.
#define DOKAN_KEEPALIVE_FILE_NAME L"\\__drive_fs_keepalive"
#define DOKAN_NOTIFICATION_FILE_NAME L"\\drive_fs_notification"

// The minimum FCB garbage collection interval, below which the parameter is
// ignored (instantaneous deletion with an interval of 0 is more efficient than
// using the machinery with a tight interval).
#define MIN_FCB_GARBAGE_COLLECTION_INTERVAL 500

/*
 * This structure is used for copying UNICODE_STRING from the kernel mode driver
 * into the user mode driver.
 * https://msdn.microsoft.com/en-us/library/windows/hardware/ff564879(v=vs.85).aspx
 */
typedef struct _DOKAN_UNICODE_STRING_INTERMEDIATE {
  USHORT Length;
  USHORT MaximumLength;
  WCHAR Buffer[1];
} DOKAN_UNICODE_STRING_INTERMEDIATE, *PDOKAN_UNICODE_STRING_INTERMEDIATE;

/*
 * This structure is used for sending notify path information from the user mode
 * driver to the kernel mode driver. See below links for parameter details for
 * CompletionFilter and Action, and FsRtlNotifyFullReportChange call.
 * https://msdn.microsoft.com/en-us/library/windows/hardware/ff547026(v=vs.85).aspx
 * https://msdn.microsoft.com/en-us/library/windows/hardware/ff547041(v=vs.85).aspx
 */
typedef struct _DOKAN_NOTIFY_PATH_INTERMEDIATE {
  ULONG CompletionFilter;
  ULONG Action;
  USHORT Length;
  WCHAR Buffer[1];
} DOKAN_NOTIFY_PATH_INTERMEDIATE, *PDOKAN_NOTIFY_PATH_INTERMEDIATE;

/*
 * This structure is used for copying ACCESS_STATE from the kernel mode driver
 * into the user mode driver.
 * https://msdn.microsoft.com/en-us/library/windows/hardware/ff538840(v=vs.85).aspx
*/
typedef struct _DOKAN_ACCESS_STATE_INTERMEDIATE {
  BOOLEAN SecurityEvaluated;
  BOOLEAN GenerateAudit;
  BOOLEAN GenerateOnClose;
  BOOLEAN AuditPrivileges;
  ULONG Flags;
  ACCESS_MASK RemainingDesiredAccess;
  ACCESS_MASK PreviouslyGrantedAccess;
  ACCESS_MASK OriginalDesiredAccess;

  // Offset from the beginning of this structure to a SECURITY_DESCRIPTOR
  // if 0 that means there is no security descriptor
  ULONG SecurityDescriptorOffset;

  // Offset from the beginning of this structure to a
  // DOKAN_UNICODE_STRING_INTERMEDIATE
  ULONG UnicodeStringObjectNameOffset;

  // Offset from the beginning of this structure to a
  // DOKAN_UNICODE_STRING_INTERMEDIATE
  ULONG UnicodeStringObjectTypeOffset;
} DOKAN_ACCESS_STATE_INTERMEDIATE, *PDOKAN_ACCESS_STATE_INTERMEDIATE;

typedef struct _DOKAN_ACCESS_STATE {
  BOOLEAN SecurityEvaluated;
  BOOLEAN GenerateAudit;
  BOOLEAN GenerateOnClose;
  BOOLEAN AuditPrivileges;
  ULONG Flags;
  ACCESS_MASK RemainingDesiredAccess;
  ACCESS_MASK PreviouslyGrantedAccess;
  ACCESS_MASK OriginalDesiredAccess;
  PSECURITY_DESCRIPTOR SecurityDescriptor;
  UNICODE_STRING ObjectName;
  UNICODE_STRING ObjectType;
} DOKAN_ACCESS_STATE, *PDOKAN_ACCESS_STATE;

/*
 * This structure is used for copying IO_SECURITY_CONTEXT from the kernel mode
 * driver into the user mode driver.
 * https://msdn.microsoft.com/en-us/library/windows/hardware/ff550613(v=vs.85).aspx
 */
typedef struct _DOKAN_IO_SECURITY_CONTEXT_INTERMEDIATE {
  DOKAN_ACCESS_STATE_INTERMEDIATE AccessState;
  ACCESS_MASK DesiredAccess;
} DOKAN_IO_SECURITY_CONTEXT_INTERMEDIATE,
    *PDOKAN_IO_SECURITY_CONTEXT_INTERMEDIATE;

typedef struct _DOKAN_IO_SECURITY_CONTEXT {
  DOKAN_ACCESS_STATE AccessState;
  ACCESS_MASK DesiredAccess;
} DOKAN_IO_SECURITY_CONTEXT, *PDOKAN_IO_SECURITY_CONTEXT;

typedef struct _CREATE_CONTEXT {
  DOKAN_IO_SECURITY_CONTEXT_INTERMEDIATE SecurityContext;
  ULONG FileAttributes;
  ULONG CreateOptions;
  ULONG ShareAccess;
  ULONG FileNameLength;

  // Offset from the beginning of this structure to the string
  ULONG FileNameOffset;
} CREATE_CONTEXT, *PCREATE_CONTEXT;

typedef struct _CLEANUP_CONTEXT {
  ULONG FileNameLength;
  WCHAR FileName[1];

} CLEANUP_CONTEXT, *PCLEANUP_CONTEXT;

typedef struct _CLOSE_CONTEXT {
  ULONG FileNameLength;
  WCHAR FileName[1];

} CLOSE_CONTEXT, *PCLOSE_CONTEXT;

typedef struct _DIRECTORY_CONTEXT {
  ULONG FileInformationClass;
  ULONG FileIndex;
  ULONG BufferLength;
  ULONG DirectoryNameLength;
  ULONG SearchPatternLength;
  ULONG SearchPatternOffset;
  WCHAR DirectoryName[1];
  WCHAR SearchPatternBase[1];

} DIRECTORY_CONTEXT, *PDIRECTORY_CONTEXT;

typedef struct _READ_CONTEXT {
  LARGE_INTEGER ByteOffset;
  ULONG BufferLength;
  ULONG FileNameLength;
  WCHAR FileName[1];
} READ_CONTEXT, *PREAD_CONTEXT;

typedef struct _WRITE_CONTEXT {
  LARGE_INTEGER ByteOffset;
  ULONG BufferLength;
  ULONG BufferOffset;
  ULONG RequestLength;
  ULONG FileNameLength;
  WCHAR FileName[2];
  // "2" means to keep last null of contents to write
} WRITE_CONTEXT, *PWRITE_CONTEXT;

typedef struct _FILEINFO_CONTEXT {
  ULONG FileInformationClass;
  ULONG BufferLength;
  ULONG FileNameLength;
  WCHAR FileName[1];
} FILEINFO_CONTEXT, *PFILEINFO_CONTEXT;

typedef struct _SETFILE_CONTEXT {
  ULONG FileInformationClass;
  ULONG BufferLength;
  ULONG BufferOffset;
  ULONG FileNameLength;
  WCHAR FileName[1];
} SETFILE_CONTEXT, *PSETFILE_CONTEXT;

typedef struct _VOLUME_CONTEXT {
  ULONG FsInformationClass;
  ULONG BufferLength;
} VOLUME_CONTEXT, *PVOLUME_CONTEXT;

typedef struct _LOCK_CONTEXT {
  LARGE_INTEGER ByteOffset;
  LARGE_INTEGER Length;
  ULONG Key;
  ULONG FileNameLength;
  WCHAR FileName[1];
} LOCK_CONTEXT, *PLOCK_CONTEXT;

typedef struct _FLUSH_CONTEXT {
  ULONG FileNameLength;
  WCHAR FileName[1];
} FLUSH_CONTEXT, *PFLUSH_CONTEXT;

typedef struct _UNMOUNT_CONTEXT {
  WCHAR DeviceName[64];
  ULONG Option;
} UNMOUNT_CONTEXT, *PUNMOUNT_CONTEXT;

typedef struct _SECURITY_CONTEXT {
  SECURITY_INFORMATION SecurityInformation;
  ULONG BufferLength;
  ULONG FileNameLength;
  WCHAR FileName[1];
} SECURITY_CONTEXT, *PSECURITY_CONTEXT;

typedef struct _SET_SECURITY_CONTEXT {
  SECURITY_INFORMATION SecurityInformation;
  ULONG BufferLength;
  ULONG BufferOffset;
  ULONG FileNameLength;
  WCHAR FileName[1];
} SET_SECURITY_CONTEXT, *PSET_SECURITY_CONTEXT;

typedef struct _EVENT_CONTEXT {
  ULONG Length;
  ULONG MountId;
  ULONG SerialNumber;
  ULONG ProcessId;
  UCHAR MajorFunction;
  UCHAR MinorFunction;
  ULONG Flags;
  ULONG FileFlags;
  ULONG64 Context;
  union {
    DIRECTORY_CONTEXT Directory;
    READ_CONTEXT Read;
    WRITE_CONTEXT Write;
    FILEINFO_CONTEXT File;
    CREATE_CONTEXT Create;
    CLOSE_CONTEXT Close;
    SETFILE_CONTEXT SetFile;
    CLEANUP_CONTEXT Cleanup;
    LOCK_CONTEXT Lock;
    VOLUME_CONTEXT Volume;
    FLUSH_CONTEXT Flush;
    UNMOUNT_CONTEXT Unmount;
    SECURITY_CONTEXT Security;
    SET_SECURITY_CONTEXT SetSecurity;
  } Operation;
} EVENT_CONTEXT, *PEVENT_CONTEXT;

// The output from IOCTL_GET_VOLUME_METRICS.
typedef struct _VOLUME_METRICS {
  ULONG64 NormalFcbGarbageCollectionCycles;
  // A "cycle" can consist of multiple "passes".
  ULONG64 NormalFcbGarbageCollectionPasses;
  ULONG64 ForcedFcbGarbageCollectionPasses;
  ULONG64 FcbAllocations;
  ULONG64 FcbDeletions;
  // A "cancellation" is when a single FCB's garbage collection gets canceled.
  ULONG64 FcbGarbageCollectionCancellations;
} VOLUME_METRICS, *PVOLUME_METRICS;

#define WRITE_MAX_SIZE                                                         \
  (EVENT_CONTEXT_MAX_SIZE - sizeof(EVENT_CONTEXT) - 256 * sizeof(WCHAR))

typedef struct _EVENT_INFORMATION {
  ULONG SerialNumber;
  NTSTATUS Status;
  ULONG Flags;
  union {
    struct {
      ULONG Index;
    } Directory;
    struct {
      ULONG Flags;
      ULONG Information;
    } Create;
    struct {
      LARGE_INTEGER CurrentByteOffset;
    } Read;
    struct {
      LARGE_INTEGER CurrentByteOffset;
    } Write;
    struct {
      UCHAR DeleteOnClose;
    } Delete;
    struct {
      ULONG Timeout;
    } ResetTimeout;
    struct {
      HANDLE Handle;
    } AccessToken;
  } Operation;
  ULONG64 Context;
  ULONG BufferLength;
  UCHAR Buffer[8];

} EVENT_INFORMATION, *PEVENT_INFORMATION;

// Dokan mount options
#define DOKAN_EVENT_ALTERNATIVE_STREAM_ON                           1
#define DOKAN_EVENT_WRITE_PROTECT                                   (1 << 1)
#define DOKAN_EVENT_REMOVABLE                                       (1 << 2)
#define DOKAN_EVENT_MOUNT_MANAGER                                   (1 << 3)
#define DOKAN_EVENT_CURRENT_SESSION                                 (1 << 4)
#define DOKAN_EVENT_FILELOCK_USER_MODE                              (1 << 5)
#define DOKAN_EVENT_DISABLE_OPLOCKS                                 (1 << 6)
#define DOKAN_EVENT_ENABLE_FCB_GC                                   (1 << 7)

typedef struct _EVENT_DRIVER_INFO {
  ULONG DriverVersion;
  ULONG Status;
  ULONG DeviceNumber;
  ULONG MountId;
  WCHAR DeviceName[64];
} EVENT_DRIVER_INFO, *PEVENT_DRIVER_INFO;

typedef struct _EVENT_START {
  ULONG UserVersion;
  ULONG DeviceType;
  ULONG Flags;
  WCHAR MountPoint[260];
  WCHAR UNCName[64];
  ULONG IrpTimeout;
} EVENT_START, *PEVENT_START;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201)
#endif
typedef struct _DOKAN_RENAME_INFORMATION {
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN10_RS1)
	union {
		BOOLEAN ReplaceIfExists;  // FileRenameInformation
		ULONG Flags;              // FileRenameInformationEx
	} DUMMYUNIONNAME;
#else
	BOOLEAN ReplaceIfExists;
#endif
  ULONG FileNameLength;
  WCHAR FileName[1];
} DOKAN_RENAME_INFORMATION, *PDOKAN_RENAME_INFORMATION;
#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef struct _DOKAN_LINK_INFORMATION {
  BOOLEAN ReplaceIfExists;
  ULONG FileNameLength;
  WCHAR FileName[1];
} DOKAN_LINK_INFORMATION, *PDOKAN_LINK_INFORMATION;

/**
* \struct DOKAN_CONTROL
* \brief Dokan Control
*/
typedef struct _DOKAN_CONTROL {
  /** File System Type */
  ULONG Type;
  /** Mount point. Can be "M:\" (drive letter) or "C:\mount\dokan" (path in NTFS) */
  WCHAR MountPoint[MAX_PATH];
  /** UNC name used for network volume */
  WCHAR UNCName[64];
  /** Disk Device Name */
  WCHAR DeviceName[64];
#ifdef _MSC_VER
  /**
  * Volume Device Object. The value is always 0
  * and should be removed from the public DOKAN_CONTROL.
  * MinGW also do not support PVOID64 so we convert it to ULONG64 see #902.
  */
  PVOID64 VolumeDeviceObject;
#else
  ULONG64 VolumeDeviceObject;
#endif
  /** Session ID of calling process */
  ULONG SessionId;
} DOKAN_CONTROL, *PDOKAN_CONTROL;

#endif // PUBLIC_H_
