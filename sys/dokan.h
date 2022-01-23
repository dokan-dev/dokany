/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2017 - 2021 Google, Inc.
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

/*++


--*/

#ifndef DOKAN_H_
#define DOKAN_H_

#define POOL_NX_OPTIN 1
#include <ntifs.h>
#include <ntdddisk.h>
#include <ntstrsafe.h>

#include "public.h"
#include "util/log.h"

//
// DEFINES
//

extern LOOKASIDE_LIST_EX g_DokanCCBLookasideList;
extern LOOKASIDE_LIST_EX g_DokanFCBLookasideList;
extern LOOKASIDE_LIST_EX g_DokanEResourceLookasideList;
extern BOOLEAN g_FixFileNameForReparseMountPoint;

// GetSystemRoutineAddress Function Pointer
typedef NTKERNELAPI BOOLEAN DokanPtr_FsRtlCheckLockForOplockRequest(
    _In_ PFILE_LOCK FileLock, _In_ PLARGE_INTEGER AllocationSize);
typedef NTKERNELAPI BOOLEAN
DokanPtr_FsRtlAreThereWaitingFileLocks(_In_ PFILE_LOCK FileLock);

// extern GetSystemRoutineAddress
extern DokanPtr_FsRtlCheckLockForOplockRequest
    *DokanFsRtlCheckLockForOplockRequest;
extern DokanPtr_FsRtlAreThereWaitingFileLocks
    *DokanFsRtlAreThereWaitingFileLocks;

#define DOKAN_DISK_DEVICE_NAME L"\\Device\\Volume"
#define DOKAN_SYMBOLIC_LINK_NAME L"\\DosDevices\\Global\\Volume"

#ifndef DOKAN_DEVICE_PREFIX_NAME
#define DOKAN_DEVICE_PREFIX_NAME L"Dokan"
#endif
#ifndef DOKAN_GLOBAL_DEVICE_NAME
#define DOKAN_GLOBAL_DEVICE_NAME \
  L"\\Device\\" DOKAN_DEVICE_PREFIX_NAME L"_" DOKAN_MAJOR_API_VERSION
#endif
#ifndef DOKAN_GLOBAL_SYMBOLIC_LINK_NAME
#define DOKAN_GLOBAL_SYMBOLIC_LINK_NAME                                        \
  L"\\DosDevices\\Global\\" \
    DOKAN_DEVICE_PREFIX_NAME L"_" DOKAN_MAJOR_API_VERSION
#endif
#ifndef DOKAN_GLOBAL_FS_DISK_DEVICE_NAME
#define DOKAN_GLOBAL_FS_DISK_DEVICE_NAME                                       \
  L"\\Device\\" DOKAN_DEVICE_PREFIX_NAME L"Fs" DOKAN_MAJOR_API_VERSION
#endif
#ifndef DOKAN_GLOBAL_FS_CD_DEVICE_NAME
#define DOKAN_GLOBAL_FS_CD_DEVICE_NAME                                         \
  L"\\Device\\" DOKAN_DEVICE_PREFIX_NAME L"CdFs" DOKAN_MAJOR_API_VERSION
#endif
#ifndef DOKAN_NET_DEVICE_NAME
#define DOKAN_NET_DEVICE_NAME                                                  \
  L"\\Device\\" DOKAN_DEVICE_PREFIX_NAME L"Redirector" DOKAN_MAJOR_API_VERSION
#endif
#ifndef DOKAN_NET_SYMBOLIC_LINK_NAME
#define DOKAN_NET_SYMBOLIC_LINK_NAME                                           \
  L"\\DosDevices\\Global\\" \
    DOKAN_DEVICE_PREFIX_NAME L"Redirector" DOKAN_MAJOR_API_VERSION
#endif

#ifndef VOLUME_LABEL
#define VOLUME_LABEL L"DOKAN"
#endif

// {D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
#ifndef DOKAN_BASE_GUID
#define DOKAN_BASE_GUID                                                        \
  {                                                                            \
    0xd6cc17c5, 0x1734, 0x4085, {                                              \
      0xbc, 0xe7, 0x96, 0x4f, 0x1e, 0x9f, 0x5d, 0xe9                           \
    }                                                                          \
  }
#endif

#define TAG (ULONG)'AKOD'

#define DOKAN_MDL_ALLOCATED 0x1

#define DokanAlloc(size) ExAllocatePoolWithTag(NonPagedPool, size, TAG)

inline PVOID DokanAllocZero(SIZE_T size) {
  PVOID buffer = DokanAlloc(size);
  if (buffer) { RtlZeroMemory(buffer, size); }
  return buffer;
}

extern ULONG DokanMdlSafePriority;
#define MmGetSystemAddressForMdlNormalSafe(mdl)                                \
  MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | DokanMdlSafePriority)

#define DRIVER_CONTEXT_EVENT 2
#define DRIVER_CONTEXT_IRP_ENTRY 3

#define DOKAN_IRP_PENDING_TIMEOUT (1000 * 15)               // in millisecond
#define DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX (1000 * 60 * 5) // in millisecond
#define DOKAN_CHECK_INTERVAL (1000 * 5)                     // in millisecond

extern NPAGED_LOOKASIDE_LIST DokanIrpEntryLookasideList;
#define DokanAllocateIrpEntry()                                                \
  ExAllocateFromNPagedLookasideList(&DokanIrpEntryLookasideList)
#define DokanFreeIrpEntry(IrpEntry)                                            \
  ExFreeToNPagedLookasideList(&DokanIrpEntryLookasideList, IrpEntry)

//
//  Undocumented definition of ExtensionFlags
//

#ifndef DOE_UNLOAD_PENDING
#define DOE_UNLOAD_PENDING 0x00000001
#define DOE_DELETE_PENDING 0x00000002
#define DOE_REMOVE_PENDING 0x00000004
#define DOE_REMOVE_PROCESSED 0x00000008
#define DOE_START_PENDING 0x00000010
#endif

//
// FSD_IDENTIFIER_TYPE
//
// Identifiers used to mark the structures
//
typedef enum _FSD_IDENTIFIER_TYPE {
  DGL = ':DGL',       // Dokan Global
  DCB = ':DDC',       // Disk Control Block
  VCB = ':VCB',       // Volume Control Block
  FCB = ':FCB',       // File Control Block
  CCB = ':CCB',       // Context Control Block
  FREED_FCB = ':FFC', // FCB that has been freed
} FSD_IDENTIFIER_TYPE;

//
// FSD_IDENTIFIER
//
// Header put in the beginning of every structure
//
typedef struct _FSD_IDENTIFIER {
  FSD_IDENTIFIER_TYPE Type;
  ULONG Size;
} FSD_IDENTIFIER, *PFSD_IDENTIFIER;

#define GetIdentifierType(Obj) (((PFSD_IDENTIFIER)Obj)->Type)

//
// DATA
//

typedef struct _IRP_LIST {
  LIST_ENTRY ListHead;
  BOOLEAN EventEnabled;
  KEVENT NotEmpty;
  KSPIN_LOCK ListLock;
} IRP_LIST, *PIRP_LIST;

typedef struct _DOKAN_GLOBAL {
  FSD_IDENTIFIER Identifier;
  ERESOURCE Resource;
  PDEVICE_OBJECT DeviceObject;
  PDEVICE_OBJECT FsDiskDeviceObject;
  PDEVICE_OBJECT FsCdDeviceObject;
  ULONG MountId;

  PKTHREAD DeviceDeleteThread;

  LIST_ENTRY MountPointList;
  LIST_ENTRY DeviceDeleteList;
  KEVENT KillDeleteDeviceEvent;

  ULONG DriverVersion;
  
  // We try to avoid having race condition when switching the AutoMount flag of
  // the MountManager. Yes, this only guarantee for dokan mount and it is still
  // possible another process conflict with it but that the best we can do.
  ERESOURCE MountManagerLock;

} DOKAN_GLOBAL, *PDOKAN_GLOBAL;

// make sure Identifier is the top of struct
typedef struct _DokanDiskControlBlock {

  FSD_IDENTIFIER Identifier;

  ERESOURCE Resource;

  PDOKAN_GLOBAL Global;
  PDRIVER_OBJECT DriverObject;
  PDEVICE_OBJECT DeviceObject;

  PVOID Vcb;

  // Pending IRPs
  IRP_LIST PendingIrp;
  // Pending IRPs waiting to be dispatched to userland
  IRP_LIST NotifyEvent;
  LIST_ENTRY NotifyIrpEventQueueList;
  KQUEUE NotifyIrpEventQueue;
  // IRPs that need to be retried in kernel mode, e.g. due to oplock breaks
  // asynchronously requested on an earlier try. These are IRPs that have never
  // yet been dispatched to user mode. The IRPs are supposed to be added here at
  // the time they become ready to retry.
  IRP_LIST PendingRetryIrp;

  PUNICODE_STRING DiskDeviceName;
  PUNICODE_STRING SymbolicLinkName;
  // MountManager assigned a persistent symbolic link name
  // during IOCTL_MOUNTDEV_LINK_CREATED
  PUNICODE_STRING PersistentSymbolicLinkName;
  PUNICODE_STRING MountPoint;
  PUNICODE_STRING UNCName;
  LPWSTR VolumeLabel;

  DEVICE_TYPE DeviceType;
  DEVICE_TYPE VolumeDeviceType;
  ULONG DeviceCharacteristics;
  HANDLE MupHandle;
  UNICODE_STRING MountedDeviceInterfaceName;
  UNICODE_STRING DiskDeviceInterfaceName;

  // When timeout is occuerd, KillEvent is triggered.
  KEVENT KillEvent;
  KEVENT ForceTimeoutEvent;
  KEVENT ReleaseEvent;

  // the thread to deal with timeout
  PKTHREAD TimeoutThread;
  PKTHREAD EventNotificationThread;

  // When UseAltStream is 1, use Alternate stream
  USHORT UseAltStream;
  USHORT UseMountManager;
  USHORT MountGlobally;
  USHORT FileLockInUserMode;

  // to make a unique id for pending IRP
  ULONG SerialNumber;

  ULONG MountId;
  ULONG Flags;

  CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
  CACHE_MANAGER_CALLBACKS CacheManagerNoOpCallbacks;

  ULONG IrpTimeout;
  ULONG SessionId;
  IO_REMOVE_LOCK RemoveLock;
  
  // If true, we know the requested mount point is occupied by a dokan drive we
  // can't remove, so force the mount manager to auto-assign a different drive
  // letter.
  BOOLEAN ForceDriveLetterAutoAssignment;
  // Whether the mount manager has notified us of the actual assigned mount
  // point yet.
  BOOLEAN MountPointDetermined;

  // Whether to dispatch the driver logs to userland.
  BOOLEAN DispatchDriverLogs;
  // Allow I/O requests to be conveyed to user mode in batches, rather than
  // strictly one for each DeviceIoControl that the DLL issues to fetch a
  // request.
  BOOLEAN AllowIpcBatching;

  // How often to garbage-collect FCBs. If this is 0, we use the historical
  // default behavior of freeing them on the spot and in the current context
  // when the FileCount reaches 0. If this is nonzero, then a background thread
  // frees a list of FileCount == 0 FCBs at this interval, but requires them to
  // have had FileCount == 0 for one whole interval before deleting them. The
  // advantage of the GC approach is that it prevents filter drivers from
  // exponentially slowing down procedures like zip file extraction due to
  // repeatedly rebuilding state that they attach to the FCB header.
  ULONG FcbGarbageCollectionIntervalMs;

  // Contains mount options from user space. See DOKAN_EVENT_* in public.h
  // for possible values.
  ULONG MountOptions;

} DokanDCB, *PDokanDCB;

#define MAX_PATH 260

typedef struct _DOKAN_CONTROL {
  // File System Type
  ULONG Type;
  // Mount point. Can be "M:\" (drive letter) or "C:\mount\dokan" (path in NTFS)
  WCHAR MountPoint[MAX_PATH];
  // UNC name used for network volume
  WCHAR UNCName[64];
  // Disk Device Name
  WCHAR DeviceName[64];
  // Always set on MOUNT_ENTRY
  PDokanDCB Dcb;
  // Always set on MOUNT_ENTRY
  PDEVICE_OBJECT DiskDeviceObject;
  // NULL until fully mounted
  PDEVICE_OBJECT VolumeDeviceObject;
  // Session ID of calling process
  ULONG SessionId;
  // Contains information about the flags on the mount
  ULONG MountOptions;
} DOKAN_CONTROL, *PDOKAN_CONTROL;

typedef struct _MOUNT_ENTRY {
  LIST_ENTRY ListEntry;
  DOKAN_CONTROL MountControl;
} MOUNT_ENTRY, *PMOUNT_ENTRY;

#define IS_DEVICE_READ_ONLY(DeviceObject)                                      \
  (DeviceObject->Characteristics & FILE_READ_ONLY_DEVICE)

// Information that can be attached to an object whose use is governed by an
// ERESOURCE, to help diagnose locking problems.
typedef struct _DokanResourceDebugInfo {
  // A description of the call site in the code where the resource was
  // exclusively acquired. If it is not exclusively acquired currently, this is
  // NULL.
  const char* ExclusiveLockSite;

  // The thread in which the lock was exclusively acquired. If it is not
  // exclusively acquired currently, this is NULL.
  PKTHREAD ExclusiveOwnerThread;

  // The number of active acquisitions held by ExclusiveOwnerThread. A value
  // greater than 1 means it has been recursively acquired.
  ULONG ExclusiveLockCount;
} DokanResourceDebugInfo, *PDokanResourceDebugInfo;

typedef struct _DokanVolumeControlBlock {

  FSD_IDENTIFIER Identifier;

  FSRTL_ADVANCED_FCB_HEADER VolumeFileHeader;
  SECTION_OBJECT_POINTERS SectionObjectPointers;
  FAST_MUTEX AdvancedFCBHeaderMutex;

  ERESOURCE Resource;
  PDEVICE_OBJECT DeviceObject;
  PDokanDCB Dcb;

  // Avl Table storing the DokanFCB instances.
  RTL_AVL_TABLE FcbTable;
  // Lookaside list for the FCB Avl table node containing the Avl header and a
  // pointer to our DokanFCB that is allocated by the global fcb lookaside list.
  LOOKASIDE_LIST_EX FCBAvlNodeLookasideList;
  // Whether the lookaside list was initialized.
  BOOLEAN FCBAvlNodeLookasideListInit;

  // NotifySync is used by notify directory change
  PNOTIFY_SYNC NotifySync;
  LIST_ENTRY DirNotifyList;

  LONG FcbAllocated;
  LONG FcbFreed;
  LONG CcbAllocated;
  LONG CcbFreed;
  ULONG Flags;
  BOOLEAN HasEventWait;
  DokanResourceDebugInfo ResourceDebugInfo;
  DOKAN_LOGGER ResourceLogger;

  // A mask that all Fcbs created for this volume match. We update this when we
  // deal each one out. In practice, they all tend to have the same first 40
  // bits on x64.
  LONG64 ValidFcbMask;

  // Whether keep-alive has been activated on this volume.
  BOOLEAN IsKeepaliveActive;

  PKTHREAD FcbGarbageCollectorThread;
  LIST_ENTRY FcbGarbageList;
  KEVENT FcbGarbageListNotEmpty;

  VOLUME_METRICS VolumeMetrics;
} DokanVCB, *PDokanVCB;

// Flags for volume
#define VCB_MOUNTED 0x00000004
#define VCB_DISMOUNT_PENDING 0x00000008

// Flags for device
#define DCB_DELETE_PENDING 0x00000001
#define DCB_MOUNTPOINT_DELETED 0x00000004

// Captures information about how oplocks have been used with an FCB for the
// whole lifetime of the FCB (and therefore the lifetime of the associated
// OPLOCK struct).
typedef struct _DokanOplockDebugInfo {
  // All of the access masks for all IRP_MJ_CREATEs that have ever targeted this
  // FCB, whether or not they were successful.
  ULONG AccessMask;

  // All of the share access flags from all IRP_MJ_CREATEs that have ever
  // targeted this FCB, whether or not they were successful.
  ULONG ShareAccessMask;

  // All of the IRP major functions that have ever been used with this FCB. If
  // a function has been used then 1 << MajorFunction is 1 in here.
  ULONG MajorFunctionMask;

  // All of the RequestedOplockLevel bits that have ever been used with this FCB
  // ORed together.
  ULONG OplockLevelMask;

  // All of the oplock-related FSCTL minor functions values that have ever been
  // used with this FCB. The functions have bits assigned by
  // GetOplockControlDebugInfoBit.
  ULONG OplockFsctlMask;

  // Pointers to all EPROCESS objects that have ever made oplock-related
  // requests (via an FSCTL or IRP_MJ_CREATE) on this FCB, ORed together.
  ULONG64 OplockProcessMask;

  // The number of FILE_OPEN_REQUIRING_OPLOCK opens that have occurred.
  LONG AtomicRequestCount;

  // The number of IRP_MJ_FILE_SYSTEM_CONTROL requests related to oplocks that
  // have occurred.
  LONG OplockFsctlCount;

  // DOKAN_OPLOCK_DEBUG_* values indicating things that have ever happened in
  // the lifetime of the FCB.
  ULONG Flags;
} DokanOplockDebugInfo, *PDokanOplockDebugInfo;

// Flags indicating oplock-related events that have occurred on an FCB.
#define DOKAN_OPLOCK_DEBUG_ATOMIC_BACKOUT 1
#define DOKAN_OPLOCK_DEBUG_COMPLETE_IF_OPLOCKED 2
#define DOKAN_OPLOCK_DEBUG_EXPLICIT_BREAK_IN_CREATE 4
#define DOKAN_OPLOCK_DEBUG_GENERIC_ACKNOWLEDGEMENT 8
#define DOKAN_OPLOCK_DEBUG_CANCELED_CREATE 16
#define DOKAN_OPLOCK_DEBUG_TIMED_OUT_CREATE 32
#define DOKAN_OPLOCK_DEBUG_CREATE_RETRY_QUEUED 64
#define DOKAN_OPLOCK_DEBUG_CREATE_RETRIED 128

typedef struct _DokanFileControlBlock {
  // Locking: Identifier is read-only, no locks needed.
  FSD_IDENTIFIER Identifier;

  // Locking: FIXME
  FSRTL_ADVANCED_FCB_HEADER AdvancedFCBHeader;
  // Locking: FIXME
  SECTION_OBJECT_POINTERS SectionObjectPointers;

  // Locking: FIXME
  FAST_MUTEX AdvancedFCBHeaderMutex;

  // Locking: Lock for paging io.
  ERESOURCE PagingIoResource;

  // Locking: Vcb pointer is read-only, no locks needed.
  PDokanVCB Vcb;
  // Locking: DokanFCBLock{RO,RW} and usually vcb lock
  LIST_ENTRY NextFCB;
  // Locking: DokanFCBLock{RO,RW}
  LIST_ENTRY NextCCB;

  // Locking: Atomics - not behind an accessor.
  LONG FileCount;

  // Locking: Use atomic flag operations - DokanFCBFlags*
  ULONG Flags;
  // Locking: Functions are ok to call concurrently.
  SHARE_ACCESS ShareAccess;

  // Locking: DokanFCBLock{RO,RW} - e.g. renames change this field.
  // Modifications must lock the VCB followed by the FCB. Reads may
  // lock either one.
  UNICODE_STRING FileName;

  // Locking: FsRtl routines should be enough after initialization.
  FILE_LOCK FileLock;

  //
  //  The following field is used by the oplock module
  //  to maintain current oplock information for < NTDDI_WIN8.
  //  OPLOCK is part of FSRTL_ADVANCED_FCB_HEADER since NTDDI_WIN8.
  //
  // Locking: DokanFCBLock{RO,RW}
  OPLOCK Oplock;

  // uint32 ReferenceCount;
  // uint32 OpenHandleCount;
  DokanResourceDebugInfo ResourceDebugInfo;
  DokanResourceDebugInfo PagingIoResourceDebugInfo;

  // A keep-alive FCB is a special FCB whose last cleanup triggers automatic
  // unmounting. This is meant to unmount the file system when the owning
  // process abruptly terminates, replacing dokan's original ping/timeout-based
  // mechanism. The owning process must open the special keepalive file name,
  // then issue a FSCTL_ACTIVATE_KEEPALIVE DeviceIoControl to that file handle
  // (which sets the IsKeepaliveActive flag), and hold the handle open until
  // after normal unmounting. The DeviceIoControl step ensures that if a filter
  // turns the CreateFile into a CreateFile + CloseHandle + CreateFile sequence,
  // the hidden CloseHandle call doesn't trigger unmounting.

  // TRUE if this FCB points to the keep-alive file name. This allows keepalive
  // activation to happen on a particular CCB, which sets IsKeepaliveActive on
  // that CCB and triggers unmount-on-cleanup behavior.
  BOOLEAN IsKeepalive;

  // If true, never dispatch requests to user mode for this handle. This is set
  // for keepalive and notification handles that are typically held by the
  // user mode process to which the file system sends requests. That process
  // must avoid having automatic IRP_MJ_CLEANUPs from its termination handle
  // sweep get dispatched back to itself, since its normal threads will all be
  // gone at that point.
  BOOLEAN BlockUserModeDispatch;

  // Info that is useful for troubleshooting oplock problems in a debugger.
  DokanOplockDebugInfo OplockDebugInfo;

  // Used only when FCB garbage collection is enabled. This is the entry for
  // this FCB in the VCB's list of FCBs that are ready for garbage collection.
  // If it is in this list, then it is also in the usable FCB list. This is
  // guarded by the VCB lock.
  LIST_ENTRY NextGarbageCollectableFcb;

  // Used only when FCB garbage collection is enabled, to ensure that the FCB
  // has been scheduled for deletion long enough to actually delete it. This is
  // owned by the VCB and guarded by the VCB lock.
  BOOLEAN GarbageCollectionGracePeriodPassed;

  // The Fcb was removed from the Avl table and is waiting to be deleted when
  // all existing handles are being closed. This can happen when a file is
  // renamed with the destination having an open handle. NTFS denies this action
  // but due to a Dokan bug, this is actually possible. Until it is fixed, we
  // reproduce the behavior prior to the Avl table.
  BOOLEAN ReplacedByRename;
} DokanFCB, *PDokanFCB;

#define DokanResourceLockRO(resource)                                          \
  {                                                                            \
    KeEnterCriticalRegion();                                                   \
    ExAcquireResourceSharedLite(resource, TRUE);                               \
  }

#define DokanResourceLockRW(resource)                                          \
  ExEnterCriticalRegionAndAcquireResourceExclusive(resource)

#define DokanResourceUnlock(resource)                                          \
  ExReleaseResourceAndLeaveCriticalRegion(resource)

VOID DokanResourceLockWithDebugInfo(__in BOOLEAN Writable,
                                    __in PERESOURCE Resource,
                                    __in PDokanResourceDebugInfo DebugInfo,
                                    __in PDOKAN_LOGGER Logger,
                                    __in const char *Site,
                                    __in const UNICODE_STRING *ResourceName,
                                    __in const void *ResourcePointer);

VOID DokanResourceUnlockWithDebugInfo(__in PERESOURCE Resource,
                                      __in PDokanResourceDebugInfo DebugInfo);

#define DokanStringizeInternal(x) #x
#define DokanStringize(x) DokanStringizeInternal(x)
#define DokanCallSiteID __FUNCTION__ ":" DokanStringize(__LINE__)

#define DokanOpLockDebugEnabled()                                              \
  (g_Debug & DOKAN_DEBUG_OPLOCKS)

#define DokanLockDebugEnabled()                                                \
  (g_Debug & DOKAN_DEBUG_LOCK)

#define DokanFCBLockRW(fcb)                                      \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                       \
        DokanResourceLockWithDebugInfo(                          \
            TRUE,                                                \
            (fcb)->AdvancedFCBHeader.Resource,                   \
            &(fcb)->ResourceDebugInfo,                           \
            &(fcb)->Vcb->ResourceLogger,                         \
            DokanCallSiteID,                                     \
            &(fcb)->FileName,                                    \
            (fcb));                                              \
      } else {                                                   \
        DokanResourceLockRW((fcb)->AdvancedFCBHeader.Resource);  \
      }                                                          \
    } while (0)

#define DokanFCBLockRO(fcb)                                      \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                       \
        DokanResourceLockWithDebugInfo(                          \
            FALSE,                                               \
            (fcb)->AdvancedFCBHeader.Resource,                   \
            &(fcb)->ResourceDebugInfo,                           \
            &(fcb)->Vcb->ResourceLogger,                         \
            DokanCallSiteID,                                     \
            &(fcb)->FileName,                                    \
            (fcb));                                              \
      } else {                                                   \
        DokanResourceLockRO((fcb)->AdvancedFCBHeader.Resource);  \
      }                                                          \
    } while (0)

#define DokanFCBUnlock(fcb)                                      \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                       \
        DokanResourceUnlockWithDebugInfo(                        \
            (fcb)->AdvancedFCBHeader.Resource,                   \
            &(fcb)->ResourceDebugInfo);                          \
      } else {                                                   \
        DokanResourceUnlock((fcb)->AdvancedFCBHeader.Resource);  \
      }                                                          \
    } while (0)

#define DokanPagingIoLockRW(fcb)                                 \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                       \
        DokanResourceLockWithDebugInfo(                          \
            TRUE,                                                \
            &(fcb)->PagingIoResource,                            \
            &(fcb)->PagingIoResourceDebugInfo,                   \
            &(fcb)->Vcb->ResourceLogger,                         \
            DokanCallSiteID,                                     \
            &(fcb)->FileName,                                    \
            (fcb));                                              \
      } else {                                                   \
        DokanResourceLockRW(&(fcb)->PagingIoResource);           \
      }                                                          \
    } while (0)

#define DokanPagingIoLockRO(fcb)                                 \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                       \
        DokanResourceLockWithDebugInfo(                          \
            FALSE,                                               \
            &(fcb)->PagingIoResource,                            \
            &(fcb)->PagingIoResourceDebugInfo,                   \
            &(fcb)->Vcb->ResourceLogger,                         \
            DokanCallSiteID,                                     \
            &(fcb)->FileName,                                    \
            (fcb));                                              \
      } else {                                                   \
        DokanResourceLockRO(&(fcb)->PagingIoResource);           \
      }                                                          \
    } while (0)

#define DokanPagingIoUnlock(fcb)                                 \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                       \
        DokanResourceUnlockWithDebugInfo(                        \
            &(fcb)->PagingIoResource,                            \
            &(fcb)->PagingIoResourceDebugInfo);                  \
      } else {                                                   \
        DokanResourceUnlock(&(fcb)->PagingIoResource);           \
      }                                                          \
    } while (0)

// Locks the given VCB for read-write and returns TRUE, if it is not already
// locked at all by another thread; otherwise returns FALSE.
BOOLEAN DokanVCBTryLockRW(PDokanVCB vcb);

#define DokanVCBLockRW(vcb)                                      \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                        \
        DokanResourceLockWithDebugInfo(                          \
            TRUE,                                                \
            &(vcb)->Resource,                                    \
            &(vcb)->ResourceDebugInfo,                           \
            &(vcb)->ResourceLogger,                              \
            DokanCallSiteID,                                     \
            (vcb)->Dcb->MountPoint,                              \
            (vcb));                                              \
      } else {                                                   \
        DokanResourceLockRW(&(vcb)->Resource);                   \
      }                                                          \
    } while (0)

#define DokanVCBLockRO(vcb)                                      \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                        \
        DokanResourceLockWithDebugInfo(                          \
            FALSE,                                               \
            &(vcb)->Resource,                                    \
            &(vcb)->ResourceDebugInfo,                           \
            &(vcb)->ResourceLogger,                              \
            DokanCallSiteID,                                     \
            (vcb)->Dcb->MountPoint,                              \
            (vcb));                                              \
      } else {                                                   \
        DokanResourceLockRO(&(vcb)->Resource);                   \
      }                                                          \
    } while (0)

#define DokanVCBUnlock(vcb)                                      \
    do {                                                         \
      if (DokanLockDebugEnabled()) {                        \
        DokanResourceUnlockWithDebugInfo(                        \
            &(vcb)->Resource,                                    \
            &(vcb)->ResourceDebugInfo);                          \
      } else {                                                   \
        DokanResourceUnlock(&(vcb)->Resource);                   \
      }                                                          \
    } while (0)

typedef struct _DokanContextControlBlock {
  // Locking: Read only field. No locking needed.
  FSD_IDENTIFIER Identifier;
  // Locking: Read only field. No locking needed.
  PDokanFCB Fcb;
  // Locking: Modified with the *FCB* lock held.
  LIST_ENTRY NextCCB;

  ULONG64 Context;
  ULONG64 UserContext;

  PWCHAR SearchPattern;
  ULONG SearchPatternLength;

  // Locking: Use atomic flag operations - DokanCCBFlags*
  ULONG Flags;

  // Locking: Read only field. No locking needed.
  ULONG MountId;

  // Whether keep-alive has been activated on this FCB.
  BOOLEAN IsKeepaliveActive;

  // Whether this CCB has a pending IRP_MJ_CREATE with
  // FILE_OPEN_REQUIRING_OPLOCK, which has gotten past the request for the
  // oplock in DokanDispatchCreate. If so, the request needs to be backed out if
  // the IRP_MJ_CREATE does not succeed. The flag is cleared when the oplock
  // request is backed out.
  BOOLEAN AtomicOplockRequestPending;

  // The process that created the CCB, for debugging purposes.
  HANDLE ProcessId;
} DokanCCB, *PDokanCCB;

//
//  The following macro is used to retrieve the oplock structure within
//  the Fcb. This structure was moved to the advanced Fcb header
//  in Win8.
//
#if (NTDDI_VERSION >= NTDDI_WIN8)
#define DokanGetFcbOplock(F) &(F)->AdvancedFCBHeader.Oplock
#else
#define DokanGetFcbOplock(F) &(F)->Oplock
#endif

// Calls FsRtlCheckOplock if oplocks are enabled for the volume associated with
// the given FCB. Otherwise, returns STATUS_SUCCESS.
NTSTATUS DokanCheckOplock(
    __in PDokanFCB Fcb,
    __in PIRP Irp,
    __in_opt PVOID Context,
    __in_opt POPLOCK_WAIT_COMPLETE_ROUTINE CompletionRoutine,
    __in_opt POPLOCK_FS_PREPOST_IRP PostIrpRoutine);

typedef struct _REQUEST_CONTEXT {
  PDEVICE_OBJECT DeviceObject;
  ULONG ProcessId;

  // Following variables are set depending the type of DeviceObject Type.
  // Note: For Vcb, Dcb will also be set to Vcb->Dcb.
  PDOKAN_GLOBAL DokanGlobal;
  PDokanDCB Dcb;
  PDokanVCB Vcb;

  PIRP Irp;
  PIO_STACK_LOCATION IrpSp;
  ULONG Flags;

  // The IRP was already recompleted by a FsRtl function or we lost ownership.
  BOOLEAN DoNotComplete;

  // Nullify any logging activity for this request.
  BOOLEAN DoNotLogActivity;

  // The IRP is forced timeout for being properly canceled.
  // See DokanCreateIrpCancelRoutine.
  BOOLEAN ForcedCanceled;

  // Whether if we are the top-level IRP.
  BOOLEAN IsTopLevelIrp;
} REQUEST_CONTEXT, *PREQUEST_CONTEXT;

// IRP list which has pending status
// this structure is also used to store event notification IRP
typedef struct _IRP_ENTRY {
  LIST_ENTRY ListEntry;
  ULONG SerialNumber;
  REQUEST_CONTEXT RequestContext;
  BOOLEAN CancelRoutineFreeMemory;
  NTSTATUS AsyncStatus;
  LARGE_INTEGER TickCount;
  PIRP_LIST IrpList;
} IRP_ENTRY, *PIRP_ENTRY;

typedef struct _DEVICE_ENTRY {
  LIST_ENTRY ListEntry;
  PDEVICE_OBJECT DiskDeviceObject;
  PDEVICE_OBJECT VolumeDeviceObject;
  ULONG SessionId;
  ULONG Counter;
  UNICODE_STRING MountPoint;
} DEVICE_ENTRY, *PDEVICE_ENTRY;

typedef struct _DRIVER_EVENT_CONTEXT {
  LIST_ENTRY ListEntry;
  EVENT_CONTEXT EventContext;
} DRIVER_EVENT_CONTEXT, *PDRIVER_EVENT_CONTEXT;

// WARN: Undocumented Microsoft struct.
// Extra create parameters (ECPs) for Symbolic link.
// Used to get correct casing name when reparse point is used.
#pragma warning(disable : 4214)
struct SYMLINK_ECP_CONTEXT {
  USHORT UnparsedNameLength;
  union {
    USHORT Flags;
    struct {
      USHORT MountPoint : 1;
    } MountPoint;
  } FlagsMountPoint;
  USHORT DeviceNameLength;
  USHORT Zero;
  struct SYMLINK_ECP_CONTEXT *Reparsed;
  UNICODE_STRING Name;
};
#pragma warning(default : 4214)

DRIVER_INITIALIZE DriverEntry;

// Redefine wdm DRIVER_DISPATCH with APC_LEVEL max level
_Function_class_(DRIVER_DISPATCH)
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_same_
typedef NTSTATUS DOKAN_DISPATCH(_In_ struct _DEVICE_OBJECT *DeviceObject,
                   _Inout_ struct _IRP *Irp);
// clang-format off
_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_READ)
_Dispatch_type_(IRP_MJ_WRITE)
_Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
_Dispatch_type_(IRP_MJ_CLEANUP)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
_Dispatch_type_(IRP_MJ_FILE_SYSTEM_CONTROL)
_Dispatch_type_(IRP_MJ_DIRECTORY_CONTROL)
_Dispatch_type_(IRP_MJ_QUERY_INFORMATION)
_Dispatch_type_(IRP_MJ_SET_INFORMATION)
_Dispatch_type_(IRP_MJ_QUERY_VOLUME_INFORMATION)
_Dispatch_type_(IRP_MJ_SET_VOLUME_INFORMATION)
_Dispatch_type_(IRP_MJ_SHUTDOWN)
_Dispatch_type_(IRP_MJ_LOCK_CONTROL)
_Dispatch_type_(IRP_MJ_QUERY_SECURITY)
_Dispatch_type_(IRP_MJ_SET_SECURITY)
DOKAN_DISPATCH DokanBuildRequest;
// clang-format on

// Build a REQUEST_CONTEXT that holds the Irp, IrpSp and other device
// information that will be passed during all the context of the request. This
// avoids having us getting incorrect value (like IrpSp) after the IRP gets
// canceled or we lose the ownership.
NTSTATUS DokanBuildRequestContext(_In_ PDEVICE_OBJECT DeviceObject,
                                  _In_ PIRP Irp, BOOLEAN IsTopLevelIrp,
                                  _Outptr_ PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchClose(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchCreate(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchRead(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchWrite(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchFlush(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchQueryInformation(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchSetInformation(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchQueryVolumeInformation(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchSetVolumeInformation(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchDirectoryControl(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchFileSystemControl(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchDeviceControl(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchLock(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchCleanup(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchShutdown(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchQuerySecurity(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchSetSecurity(__in PREQUEST_CONTEXT RequestContext);

DRIVER_UNLOAD DokanUnload;

DRIVER_CANCEL DokanEventCancelRoutine;

DRIVER_CANCEL DokanIrpCancelRoutine;

DRIVER_CANCEL DokanCreateIrpCancelRoutine;

VOID DokanOplockComplete(IN PVOID Context, IN PIRP Irp);

VOID DokanPrePostIrp(IN PVOID Context, IN PIRP Irp);

NTSTATUS
DokanCompleteIrp(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS DokanResetPendingIrpTimeout(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanGetAccessToken(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanCheckShareAccess(_In_ PREQUEST_CONTEXT RequestContext,
                      _In_ PFILE_OBJECT FileObject, _In_ PDokanFCB FcbOrDcb,
                      _In_ ACCESS_MASK DesiredAccess, _In_ ULONG ShareAccess);

NTSTATUS
DokanGetMountPointList(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp,
                     BOOLEAN IsTopLevelIrp);

NTSTATUS
DokanEventRelease(__in_opt PREQUEST_CONTEXT RequestContext,
                  __in PDEVICE_OBJECT DeviceObject);

NTSTATUS
DokanGlobalEventRelease(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanExceptionFilter(__in PIRP Irp, __in PEXCEPTION_POINTERS ExceptionPointer);

NTSTATUS
DokanExceptionHandler(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp,
                      __in NTSTATUS ExceptionCode);

NTSTATUS
DokanEventStart(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanEventWrite(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS DokanGetVolumeMetrics(__in PREQUEST_CONTEXT RequestContext);

PEVENT_CONTEXT
AllocateEventContextRaw(__in ULONG EventContextLength);

PEVENT_CONTEXT
AllocateEventContext(__in PREQUEST_CONTEXT RequestContext,
                     __in ULONG EventContextLength, __in_opt PDokanCCB Ccb);

VOID DokanFreeEventContext(__in PEVENT_CONTEXT EventContext);

NTSTATUS
DokanRegisterPendingIrp(__in PREQUEST_CONTEXT RequestContext,
                        __in PEVENT_CONTEXT EventContext);

VOID DokanRegisterPendingRetryIrp(__in PREQUEST_CONTEXT RequestContext);

VOID DokanRegisterAsyncCreateFailure(__in PREQUEST_CONTEXT RequestContext,
                                     __in NTSTATUS Status);

VOID DokanEventNotification(__in PREQUEST_CONTEXT RequestContext,
                            __in PIRP_LIST NotifyEvent,
                            __in PEVENT_CONTEXT EventContext);

VOID DokanCompleteDirectoryControl(__in PREQUEST_CONTEXT RequestContext,
                                   __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteRead(__in PREQUEST_CONTEXT RequestContext,
                       __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteWrite(__in PREQUEST_CONTEXT RequestContext,
                        __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteQueryInformation(__in PREQUEST_CONTEXT RequestContext,
                                   __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteSetInformation(__in PREQUEST_CONTEXT RequestContext,
                                 __in PEVENT_INFORMATION EventInfo);

// Invokes DokanCompleteCreate safely to time out an IRP_MJ_CREATE from a thread
// that is not already in the context of a file system request.
VOID DokanCancelCreateIrp(__in PREQUEST_CONTEXT RequestContext,
                          __in NTSTATUS Status);

VOID DokanCompleteCreate(__in PREQUEST_CONTEXT RequestContext,
                         __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteCleanup(__in PREQUEST_CONTEXT RequestContext,
                          __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteLock(__in PREQUEST_CONTEXT RequestContext,
                       __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteQueryVolumeInformation(__in PREQUEST_CONTEXT RequestContext,
                                         __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteFlush(__in PREQUEST_CONTEXT RequestContext,
                        __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteQuerySecurity(__in PREQUEST_CONTEXT RequestContext,
                                __in PEVENT_INFORMATION EventInfo);

VOID DokanCompleteSetSecurity(__in PREQUEST_CONTEXT RequestContext,
                              __in PEVENT_INFORMATION EventInfo);

VOID DokanNoOpRelease(__in PVOID Fcb);

BOOLEAN
DokanNoOpAcquire(__in PVOID Fcb, __in BOOLEAN Wait);

NTSTATUS
DokanCreateGlobalDiskDevice(__in PDRIVER_OBJECT DriverObject,
                            __out PDOKAN_GLOBAL *DokanGlobal);

NTSTATUS
DokanCreateDiskDevice(__in PDRIVER_OBJECT DriverObject, __in ULONG MountId,
                      __in PWCHAR MountPoint, __in PWCHAR UNCName,
                      __in_opt PSECURITY_DESCRIPTOR VolumeSecurityDescriptor,
                      __in ULONG sessionID, __in PWCHAR BaseGuid,
                      __in PDOKAN_GLOBAL DokanGlobal,
                      __in DEVICE_TYPE DeviceType,
                      __in ULONG DeviceCharacteristics,
                      __in BOOLEAN MountGlobally, __in BOOLEAN UseMountManager,
                      __out PDOKAN_CONTROL DokanControl);

VOID DokanInitVpb(__in PVPB Vpb, __in PDEVICE_OBJECT VolumeDevice);
VOID DokanDeleteDeviceObject(__in_opt PREQUEST_CONTEXT RequestContext,
                             __in PDokanDCB Dcb);
VOID DokanDeleteMountPoint(__in_opt PREQUEST_CONTEXT RequestContext,
                           __in PDokanDCB Dcb);

// Create FSCTL_SET_REPARSE_POINT payload request aim to be sent with
// SendDirectoryFsctl.
PCHAR CreateSetReparsePointRequest(__in PREQUEST_CONTEXT RequestContext,
                                   __in PUNICODE_STRING SymbolicLinkName,
                                   __out PULONG Length);

// Create FSCTL_DELETE_REPARSE_POINT payload request aim to be sent with
// SendDirectoryFsctl.
PCHAR CreateRemoveReparsePointRequest(__in_opt PREQUEST_CONTEXT RequestContext,
                                      __out PULONG Length);

// Open a Directory path and send a FsControl with the input buffer.
NTSTATUS SendDirectoryFsctl(__in_opt PREQUEST_CONTEXT RequestContext,
                            __in PUNICODE_STRING Path, __in ULONG Code,
                            __in PCHAR Input, __in ULONG Length);

// Run function as System user and wait that it returns.
VOID RunAsSystem(_In_ PKSTART_ROUTINE StartRoutine, PVOID StartContext);

NTSTATUS DokanOplockRequest(__in PREQUEST_CONTEXT RequestContext);
NTSTATUS DokanCommonLockControl(__in PREQUEST_CONTEXT RequestContext);

// Register the UNCName to system multiple UNC provider.
VOID DokanRegisterUncProvider(__in PVOID pDcb);

// Complete the IRP. This can happen at the end of the dispatch routines if
// dispatch to userland is not needed or only happen after we received the
// answer from userland.
VOID DokanCompleteIrpRequest(__in PIRP Irp, __in NTSTATUS Status);

// DokanNotifyReportChange* returns
// - STATUS_OBJECT_NAME_INVALID if there appears to be an invalid name stored
//   in the NotifyList.
// - STATUS_INVALID_PARAMETER if the string passed in triggers an access
//   violation.
// - STATUS_SUCCESS otherwise.
NTSTATUS DokanNotifyReportChange0(__in PREQUEST_CONTEXT RequestContext,
                                  __in PDokanFCB Fcb,
                                  __in PUNICODE_STRING FileName,
                                  __in ULONG FilterMatch,
                                  __in ULONG Action);

NTSTATUS DokanNotifyReportChange(__in PREQUEST_CONTEXT RequestContext,
                                 __in PDokanFCB Fcb,
                                 __in ULONG FilterMatch,
                                 __in ULONG Action);

// Ends all pending waits for directory change notifications.
VOID DokanCleanupAllChangeNotificationWaiters(__in PDokanVCB Vcb);

// Backs out an atomic oplock request that was made in DokanDispatchCreate. This
// should be called if the IRP for which the request was made is about to fail.
VOID DokanMaybeBackOutAtomicOplockRequest(__in PDokanCCB Ccb, __in PIRP Irp);

PDokanCCB DokanAllocateCCB(__in PREQUEST_CONTEXT RequestContext, __in PDokanFCB Fcb);

NTSTATUS
DokanFreeCCB(__in PREQUEST_CONTEXT RequestContext, __in PDokanCCB Ccb);

NTSTATUS
DokanStartCheckThread(__in PDokanDCB Dcb);

VOID DokanStopCheckThread(__in PDokanDCB Dcb);

BOOLEAN
DokanCheckCCB(__in PREQUEST_CONTEXT RequestContext, __in_opt PDokanCCB Ccb);

NTSTATUS
DokanStartEventNotificationThread(__in PDokanDCB Dcb);

VOID DokanStopEventNotificationThread(__in PDokanDCB Dcb);

VOID DokanUpdateTimeout(__out PLARGE_INTEGER KickCount, __in ULONG Timeout);

VOID DokanUnmount(__in_opt PREQUEST_CONTEXT RequestContext, __in PDokanDCB Dcb);

BOOLEAN IsUnmountPending(__in PDEVICE_OBJECT DeviceObject);

BOOLEAN IsMounted(__in PDEVICE_OBJECT DeviceObject);

BOOLEAN IsDeletePending(__in PDEVICE_OBJECT DeviceObject);

BOOLEAN IsUnmountPendingVcb(__in PDokanVCB vcb);

PMOUNT_ENTRY InsertMountEntry(PDOKAN_GLOBAL dokanGlobal,
                              PDOKAN_CONTROL DokanControl,
                              BOOLEAN lockGlobal);

PMOUNT_ENTRY FindMountEntry(__in PDOKAN_GLOBAL dokanGlobal,
                            __in PDOKAN_CONTROL DokanControl,
                            __in BOOLEAN lockGlobal);

PMOUNT_ENTRY FindMountEntryByName(__in PDOKAN_GLOBAL DokanGlobal,
                                  __in PUNICODE_STRING DiskDeviceName,
                                  __in PUNICODE_STRING UNCName,
                                  __in BOOLEAN LockGlobal);

NTSTATUS DokanAllocateMdl(__in PREQUEST_CONTEXT RequestContext,
                          __in ULONG Length);

VOID DokanFreeMdl(__in PIRP Irp);

ULONG PointerAlignSize(ULONG sizeInBytes);

VOID DokanCreateMountPoint(__in PDokanDCB Dcb);

VOID FlushFcb(__in PREQUEST_CONTEXT RequestContext, __in PDokanFCB fcb,
              __in_opt PFILE_OBJECT fileObject);

BOOLEAN
DokanLookasideCreate(__in LOOKASIDE_LIST_EX *pCache, __in size_t cbElement);

PDEVICE_ENTRY
FindDeviceForDeleteBySessionId(PDOKAN_GLOBAL dokanGlobal, ULONG sessionId);

BOOLEAN DeleteMountPointSymbolicLink(__in PUNICODE_STRING MountPoint);

ULONG GetCurrentSessionId(__in PREQUEST_CONTEXT RequestContext);

VOID RemoveSessionDevices(__in PREQUEST_CONTEXT RequestContext,
                          __in ULONG sessionId);

static UNICODE_STRING sddl = RTL_CONSTANT_STRING(
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGX;;;RC)");

#define SetLongFlag(_F, _SF) DokanSetFlag(&(_F), (ULONG)(_SF))
#define ClearLongFlag(_F, _SF) DokanClearFlag(&(_F), (ULONG)(_SF))

__inline VOID DokanSetFlag(PULONG Flags, ULONG FlagBit) {
  ULONG _ret = InterlockedOr((PLONG)Flags, FlagBit);
  UNREFERENCED_PARAMETER(_ret);
  ASSERT(*Flags == (_ret | FlagBit));
}

__inline VOID DokanClearFlag(PULONG Flags, ULONG FlagBit) {
  ULONG _ret = InterlockedAnd((PLONG)Flags, ~FlagBit);
  UNREFERENCED_PARAMETER(_ret);
  ASSERT(*Flags == (_ret & (~FlagBit)));
}

#define IsFlagOn(a, b) ((BOOLEAN)(FlagOn(a, b) == b))

#define DokanFCBFlagsGet(fcb) ((fcb)->Flags)
#define DokanFCBFlagsIsSet(fcb, bit) (((fcb)->Flags) & (bit))
#define DokanFCBFlagsSetBit(fcb, bit) SetLongFlag((fcb)->Flags, (bit))
#define DokanFCBFlagsClearBit(fcb, bit) ClearLongFlag((fcb)->Flags, (bit))

#define DokanCCBFlagsGet DokanFCBFlagsGet
#define DokanCCBFlagsIsSet DokanFCBFlagsIsSet
#define DokanCCBFlagsSetBit DokanFCBFlagsSetBit
#define DokanCCBFlagsClearBit DokanFCBFlagsClearBit

// Logs the occurrence of the given type of IRP in the oplock debug info of the
// FCB.
void OplockDebugRecordMajorFunction(__in PDokanFCB Fcb,
                                    __in UCHAR MajorFunction);

// Logs the occurrence of an event type indicated by the DOKAN_OPLOCK_DEBUG_*
// constants. The Flag value should be one constant.
void OplockDebugRecordFlag(__in PDokanFCB Fcb, __in ULONG Flag);

// Logs the process doing an oplock-related operation in the oplock debug info
// of the FCB. This is not needed for explicit oplock requests. Use it e.g. for
// an IRP_MJ_CREATE that requests an atomic oplock.
void OplockDebugRecordProcess(__in PDokanFCB Fcb);

// Records debug info about an oplock-related IRP_MJ_FILE_SYSTEM_CONTROL in the
// oplock debug info of the FCB. The OplockLevel is a valid combination of
// Windows OPLOCK_LEVEL_CACHE_* flags indicating the kind of oplock requested,
// if applicable.
void OplockDebugRecordRequest(__in PDokanFCB Fcb,
                              __in ULONG FsControlMinorFunction,
                              __in ULONG OplockLevel);

// Logs the occurrence of an IRP_MJ_CREATE request with the specified access
// flags in the oplock debug info of the FCB. Further information, like the
// presence of an atomic oplock request, should be logged separately.
void OplockDebugRecordCreateRequest(__in PDokanFCB Fcb,
                                    __in ACCESS_MASK AccessMask,
                                    __in ULONG ShareAccess);

// Logs an atomic oplock request within a create operation.
void OplockDebugRecordAtomicRequest(__in PDokanFCB Fcb);

#endif // DOKAN_H_
