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

#include "dokan.h"
#include "util/str.h"

#include <mountmgr.h>

static VOID InitMultiVersionResources();

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(INIT, InitMultiVersionResources)
#pragma alloc_text(PAGE, DokanUnload)
#endif

LOOKASIDE_LIST_EX g_DokanCCBLookasideList;
LOOKASIDE_LIST_EX g_DokanFCBLookasideList;
LOOKASIDE_LIST_EX g_DokanEResourceLookasideList;
BOOLEAN g_FixFileNameForReparseMountPoint;

NPAGED_LOOKASIDE_LIST DokanIrpEntryLookasideList;
ULONG DokanMdlSafePriority = 0;

FAST_IO_DISPATCH FastIoDispatch;
FAST_IO_CHECK_IF_POSSIBLE DokanFastIoCheckIfPossible;

DokanPtr_FsRtlCheckLockForOplockRequest *DokanFsRtlCheckLockForOplockRequest = NULL;
DokanPtr_FsRtlAreThereWaitingFileLocks *DokanFsRtlAreThereWaitingFileLocks = NULL;

BOOLEAN
DokanFastIoCheckIfPossible(__in PFILE_OBJECT FileObject,
                           __in PLARGE_INTEGER FileOffset, __in ULONG Length,
                           __in BOOLEAN Wait, __in ULONG LockKey,
                           __in BOOLEAN CheckForReadOperation,
                           __out PIO_STATUS_BLOCK IoStatus,
                           __in PDEVICE_OBJECT DeviceObject) {
  UNREFERENCED_PARAMETER(FileObject);
  UNREFERENCED_PARAMETER(FileOffset);
  UNREFERENCED_PARAMETER(Length);
  UNREFERENCED_PARAMETER(Wait);
  UNREFERENCED_PARAMETER(LockKey);
  UNREFERENCED_PARAMETER(CheckForReadOperation);
  UNREFERENCED_PARAMETER(IoStatus);
  UNREFERENCED_PARAMETER(DeviceObject);

  DOKAN_LOG_("FileObject=%p", FileObject);
  return FALSE;
}

BOOLEAN
DokanFastIoRead(__in PFILE_OBJECT FileObject, __in PLARGE_INTEGER FileOffset,
                __in ULONG Length, __in BOOLEAN Wait, __in ULONG LockKey,
                __in PVOID Buffer, __out PIO_STATUS_BLOCK IoStatus,
                __in PDEVICE_OBJECT DeviceObject) {
  UNREFERENCED_PARAMETER(FileObject);
  UNREFERENCED_PARAMETER(FileOffset);
  UNREFERENCED_PARAMETER(Length);
  UNREFERENCED_PARAMETER(Wait);
  UNREFERENCED_PARAMETER(LockKey);
  UNREFERENCED_PARAMETER(Buffer);
  UNREFERENCED_PARAMETER(IoStatus);
  UNREFERENCED_PARAMETER(DeviceObject);

  DOKAN_LOG_("FileObject=%p", FileObject);
  return FALSE;
}

FAST_IO_ACQUIRE_FILE DokanAcquireForCreateSection;
VOID DokanAcquireForCreateSection(__in PFILE_OBJECT FileObject) {
  PDokanCCB ccb = (PDokanCCB)FileObject->FsContext2;
  if (ccb != NULL) {
    DokanFCBLockRW(ccb->Fcb);
    KeLeaveCriticalRegion();
  }
  DOKAN_LOG_("FileObject=%p CCB=%p", FileObject, ccb);
}

FAST_IO_RELEASE_FILE DokanReleaseForCreateSection;
VOID DokanReleaseForCreateSection(__in PFILE_OBJECT FileObject) {
  PDokanCCB ccb = (PDokanCCB)FileObject->FsContext2;
  if (ccb != NULL) {
    KeEnterCriticalRegion();
    DokanFCBUnlock(ccb->Fcb);
  }

  DOKAN_LOG_("FileObject=%p CCB=%p", FileObject, ccb);
}

FAST_IO_ACQUIRE_FOR_CCFLUSH DokanAcquireForCcFlush;
NTSTATUS DokanAcquireForCcFlush(__in PFILE_OBJECT FileObject,
                                __in PDEVICE_OBJECT DeviceObject) {
  // This does the same locking that the FsRtlAcquireFileForCcFlushEx call
  // within CcFlushCache would be doing, if we did not bother implementing
  // this function. The only point of implementing it is because our specific
  // incantation for acquiring the same locks will get this instrumented when
  // enable lock debugging is on.
  UNREFERENCED_PARAMETER(DeviceObject);
  PDokanCCB ccb = (PDokanCCB)FileObject->FsContext2;
  if (ccb != NULL) {
    DokanPagingIoLockRO(ccb->Fcb);
    KeLeaveCriticalRegion();
  }
  return STATUS_SUCCESS;
}

FAST_IO_RELEASE_FOR_CCFLUSH DokanReleaseForCcFlush;
NTSTATUS DokanReleaseForCcFlush(__in PFILE_OBJECT FileObject,
                                __in PDEVICE_OBJECT DeviceObject) {
  // See the comment in DokanAcquireForCcFlush.
  UNREFERENCED_PARAMETER(DeviceObject);
  PDokanCCB ccb = (PDokanCCB)FileObject->FsContext2;
  if (ccb != NULL) {
    // The unlock calls below expect to be in their own critical region index.
    KeEnterCriticalRegion();
    DokanPagingIoUnlock(ccb->Fcb);
  }
  return STATUS_SUCCESS;
}

NTSTATUS
DokanFilterCallbackAcquireForCreateSection(__in PFS_FILTER_CALLBACK_DATA
                                               CallbackData,
                                           __out PVOID *CompletionContext) {
  PDokanCCB ccb;

  UNREFERENCED_PARAMETER(CompletionContext);


  ccb = CallbackData->FileObject->FsContext2;
  DOKAN_LOG_("CCB=%p", ccb);
  if (ccb != NULL) {
    DokanFCBLockRW(ccb->Fcb);
    KeLeaveCriticalRegion();
  }

  if (ccb == NULL ||
      CallbackData->Parameters.AcquireForSectionSynchronization.SyncType !=
          SyncTypeCreateSection) {
    return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
  } else if (ccb->Fcb->ShareAccess.Writers == 0) {
    return STATUS_FILE_LOCKED_WITH_ONLY_READERS;
  } else {
    return STATUS_FILE_LOCKED_WITH_WRITERS;
  }
}

BOOLEAN
DokanLookasideCreate(__in LOOKASIDE_LIST_EX *pCache, __in size_t cbElement) {
  NTSTATUS Status = ExInitializeLookasideListEx(
      pCache, NULL, NULL, NonPagedPool, 0, cbElement, TAG, 0);

  if (!NT_SUCCESS(Status)) {
    DOKAN_LOG_("ExInitializeLookasideListEx failed, Status (0x%x) %s", Status,
               DokanGetNTSTATUSStr(Status));
    return FALSE;
  }

  return TRUE;
}

VOID CleanupGlobalDiskDevice(PDOKAN_GLOBAL dokanGlobal) {
  WCHAR symbolicLinkBuf[] = DOKAN_GLOBAL_SYMBOLIC_LINK_NAME;
  UNICODE_STRING symbolicLinkName;

  KeSetEvent(&dokanGlobal->KillDeleteDeviceEvent, 0, FALSE);

  RtlInitUnicodeString(&symbolicLinkName, symbolicLinkBuf);
  IoDeleteSymbolicLink(&symbolicLinkName);

  IoUnregisterFileSystem(dokanGlobal->FsDiskDeviceObject);
  IoUnregisterFileSystem(dokanGlobal->FsCdDeviceObject);

  IoDeleteDevice(dokanGlobal->FsDiskDeviceObject);
  IoDeleteDevice(dokanGlobal->FsCdDeviceObject);
  IoDeleteDevice(dokanGlobal->DeviceObject);
  ExDeleteResourceLite(&dokanGlobal->Resource);
  ExDeleteResourceLite(&dokanGlobal->MountManagerLock);
}

VOID InitMultiVersionResources() {
  // Enable No-Execute Nonpaged Pool - POOL_NX_OPTIN
  ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

  if (RtlIsNtDdiVersionAvailable(NTDDI_WIN8)) {

    DokanMdlSafePriority = MdlMappingNoExecute;

    UNICODE_STRING SystemRoutineName;
    RtlInitUnicodeString(&SystemRoutineName, L"FsRtlCheckLockForOplockRequest");
    DokanFsRtlCheckLockForOplockRequest =
        (DokanPtr_FsRtlCheckLockForOplockRequest *)MmGetSystemRoutineAddress(
            &SystemRoutineName);
    RtlInitUnicodeString(&SystemRoutineName, L"FsRtlAreThereWaitingFileLocks");
    DokanFsRtlAreThereWaitingFileLocks =
        (DokanPtr_FsRtlAreThereWaitingFileLocks *)MmGetSystemRoutineAddress(
            &SystemRoutineName);
  }
}

NTSTATUS
DriverEntry(__in PDRIVER_OBJECT DriverObject, __in PUNICODE_STRING RegistryPath)

/*++

Routine Description:

        This routine gets called by the system to initialize the driver.

Arguments:

        DriverObject    - the system supplied driver object.
        RegistryPath    - the system supplied registry path for this driver.

Return Value:

        NTSTATUS

--*/

{
  NTSTATUS status;
  FS_FILTER_CALLBACKS filterCallbacks;
  PDOKAN_GLOBAL dokanGlobal = NULL;

  UNREFERENCED_PARAMETER(RegistryPath);

  // Initialize log entry list first before any log.
  InitializeListHead(&g_DokanLogEntryList.Log);
  ExInitializeResourceLite(&g_DokanLogEntryList.Resource);

#ifdef DEBUG_
  DOKAN_LOG_("ver.%x, %s %s", DOKAN_DRIVER_VERSION, __DATE__,
            __TIME__);
#endif

  status = DokanCreateGlobalDiskDevice(DriverObject, &dokanGlobal);

  if (NT_ERROR(status)) {
    return status;
  }
  //
  // Set up dispatch entry points for the driver.
  //
  DriverObject->DriverUnload = DokanUnload;

  DriverObject->MajorFunction[IRP_MJ_CREATE] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_CLOSE] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_CLEANUP] = DokanBuildRequest;

  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = DokanBuildRequest;

  DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] = DokanBuildRequest;

  DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] =
      DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] =
      DokanBuildRequest;

  DriverObject->MajorFunction[IRP_MJ_READ] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_WRITE] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = DokanBuildRequest;

  DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = DokanBuildRequest;

  DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL] = DokanBuildRequest;

  DriverObject->MajorFunction[IRP_MJ_QUERY_SECURITY] = DokanBuildRequest;
  DriverObject->MajorFunction[IRP_MJ_SET_SECURITY] = DokanBuildRequest;

  RtlZeroMemory(&FastIoDispatch, sizeof(FAST_IO_DISPATCH));

  FastIoDispatch.SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
  FastIoDispatch.FastIoCheckIfPossible = DokanFastIoCheckIfPossible;
  // FastIoDispatch.FastIoRead = DokanFastIoRead;
  FastIoDispatch.FastIoRead = FsRtlCopyRead;
  FastIoDispatch.FastIoWrite = FsRtlCopyWrite;
  FastIoDispatch.AcquireFileForNtCreateSection = DokanAcquireForCreateSection;
  FastIoDispatch.ReleaseFileForNtCreateSection = DokanReleaseForCreateSection;
  FastIoDispatch.AcquireForCcFlush = DokanAcquireForCcFlush;
  FastIoDispatch.ReleaseForCcFlush = DokanReleaseForCcFlush;
  FastIoDispatch.MdlRead = FsRtlMdlReadDev;
  FastIoDispatch.MdlReadComplete = FsRtlMdlReadCompleteDev;
  FastIoDispatch.PrepareMdlWrite = FsRtlPrepareMdlWriteDev;
  FastIoDispatch.MdlWriteComplete = FsRtlMdlWriteCompleteDev;

  DriverObject->FastIoDispatch = &FastIoDispatch;

  InitMultiVersionResources();

  ExInitializeNPagedLookasideList(&DokanIrpEntryLookasideList, NULL, NULL, 0,
                                  sizeof(IRP_ENTRY), TAG, 0);

  RtlZeroMemory(&filterCallbacks, sizeof(FS_FILTER_CALLBACKS));

  // only be used by filter driver?
  filterCallbacks.SizeOfFsFilterCallbacks = sizeof(FS_FILTER_CALLBACKS);
  filterCallbacks.PreAcquireForSectionSynchronization =
      DokanFilterCallbackAcquireForCreateSection;

  status =
      FsRtlRegisterFileSystemFilterCallbacks(DriverObject, &filterCallbacks);

  if (!NT_SUCCESS(status)) {
    CleanupGlobalDiskDevice(dokanGlobal);
    DOKAN_LOG_("  FsRtlRegisterFileSystemFilterCallbacks returned 0x%x %s",
              status, DokanGetNTSTATUSStr(status));
    return status;
  }

  if (!DokanLookasideCreate(&g_DokanCCBLookasideList, sizeof(DokanCCB))) {
    DOKAN_LOG("DokanLookasideCreate g_DokanCCBLookasideList failed");
    CleanupGlobalDiskDevice(dokanGlobal);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (!DokanLookasideCreate(&g_DokanFCBLookasideList, sizeof(DokanFCB))) {
    DOKAN_LOG("DokanLookasideCreate g_DokanFCBLookasideList failed");
    CleanupGlobalDiskDevice(dokanGlobal);
    ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (!DokanLookasideCreate(&g_DokanEResourceLookasideList,
                            sizeof(ERESOURCE))) {
    DOKAN_LOG("DokanLookasideCreate g_DokanEResourceLookasideList failed");
    CleanupGlobalDiskDevice(dokanGlobal);
    ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
    ExDeleteLookasideListEx(&g_DokanFCBLookasideList);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  // Detect if we are running on a older version than NTDDI_WIN10_RS4
  // needing to fix FileName during Reparse MountPoint.
  g_FixFileNameForReparseMountPoint =
      !RtlIsNtDdiVersionAvailable(0x0A000005);

  DOKAN_LOG_("%s FixFileNameForReparseMountPoint=%d",
             DokanGetNTSTATUSStr(status), g_FixFileNameForReparseMountPoint);

  return (status);
}

VOID DokanUnload(__in PDRIVER_OBJECT DriverObject)
/*++

Routine Description:

        This routine gets called to remove the driver from the system.

Arguments:

        DriverObject    - the system supplied driver object.

Return Value:

        NTSTATUS

--*/

{

  PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
  PDOKAN_GLOBAL dokanGlobal;

  PAGED_CODE();

  dokanGlobal = deviceObject->DeviceExtension;
  if (GetIdentifierType(dokanGlobal) == DGL) {
    DOKAN_LOG("Delete Global DeviceObject");
    CleanupGlobalDiskDevice(dokanGlobal);
  }

  ExDeleteNPagedLookasideList(&DokanIrpEntryLookasideList);

  ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
  ExDeleteLookasideListEx(&g_DokanFCBLookasideList);
  ExDeleteLookasideListEx(&g_DokanEResourceLookasideList);

  DOKAN_LOG("All resources released");
}

NTSTATUS
DokanDispatchShutdown(__in PREQUEST_CONTEXT RequestContext) {
  // PAGED_CODE();

  UNREFERENCED_PARAMETER(RequestContext);

  // A driver does not receive an IRP_MJ_SHUTDOWN request for a device
  // object unless it registers to do so with either
  // IoRegisterShutdownNotification or
  // IoRegisterLastChanceShutdownNotification. We do not call those
  // functions and therefore should not get the IRP

  return STATUS_SUCCESS;
}

BOOLEAN
DokanNoOpAcquire(__in PVOID Fcb, __in BOOLEAN Wait) {
  UNREFERENCED_PARAMETER(Fcb);
  UNREFERENCED_PARAMETER(Wait);

  ASSERT(IoGetTopLevelIrp() == NULL);

  IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

  DOKAN_LOG("ToplevelIrp changed to FSRTL_CACHE_TOP_LEVEL_IRP");

  return TRUE;
}

VOID DokanNoOpRelease(__in PVOID Fcb) {
  ASSERT(IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

  IoSetTopLevelIrp(NULL);

  UNREFERENCED_PARAMETER(Fcb);

  DOKAN_LOG("ToplevelIrp restored to NULL");
}

NTSTATUS DokanCheckOplock(
    __in PDokanFCB Fcb,
    __in PIRP Irp,
    __in_opt PVOID Context,
    __in_opt POPLOCK_WAIT_COMPLETE_ROUTINE CompletionRoutine,
    __in_opt POPLOCK_FS_PREPOST_IRP PostIrpRoutine) {
  return FsRtlCheckOplock(DokanGetFcbOplock(Fcb), Irp, Context,
                          CompletionRoutine, PostIrpRoutine);
}

VOID DokanCompleteIrpRequest(__in PIRP Irp, __in NTSTATUS Status) {
  if (Status != STATUS_PENDING) {
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
  }
}

NTSTATUS DokanNotifyReportChange0(__in PREQUEST_CONTEXT RequestContext,
                                  __in PDokanFCB Fcb,
                                  __in PUNICODE_STRING FileName,
                                  __in ULONG FilterMatch, __in ULONG Action) {
  USHORT nameOffset;

  ASSERT(Fcb != NULL);
  ASSERT(FileName != NULL);

  // Alternate streams are supposed to use a different set of action
  // and filter values, but we do not expect the caller to be aware of this.
  if (DokanSearchUnicodeStringChar(FileName, L':') != -1) {  // FileStream

    // Convert file action to stream action
    switch (Action) {
      case FILE_ACTION_ADDED:
        Action = FILE_ACTION_ADDED_STREAM;
        break;
      case FILE_ACTION_REMOVED:
        Action = FILE_ACTION_REMOVED_STREAM;
        break;
      case FILE_ACTION_MODIFIED:
        Action = FILE_ACTION_MODIFIED_STREAM;
        break;
      default:
        break;
    }

    // Convert file flag to stream flag
    if (FlagOn(FilterMatch,
               FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME))
      SetFlag(FilterMatch, FILE_NOTIFY_CHANGE_STREAM_NAME);
    if (FlagOn(FilterMatch, FILE_NOTIFY_CHANGE_SIZE))
      SetFlag(FilterMatch, FILE_NOTIFY_CHANGE_STREAM_SIZE);
    if (FlagOn(FilterMatch, FILE_NOTIFY_CHANGE_LAST_WRITE))
      SetFlag(FilterMatch, FILE_NOTIFY_CHANGE_STREAM_WRITE);

    // Cleanup file flag converted
    ClearFlag(FilterMatch, ~(FILE_NOTIFY_CHANGE_STREAM_NAME |
                             FILE_NOTIFY_CHANGE_STREAM_SIZE |
                             FILE_NOTIFY_CHANGE_STREAM_WRITE));
  }

  nameOffset = (USHORT)(FileName->Length / sizeof(WCHAR) - 1);

  // search the last "\" and then calculate the Offset in bytes
  nameOffset = (USHORT)(DokanSearchWcharinUnicodeStringWithUlong(
      FileName, L'\\', (ULONG)nameOffset, 1));
  nameOffset *= sizeof(WCHAR);  // Offset is in bytes

  __try {
    FsRtlNotifyFullReportChange(Fcb->Vcb->NotifySync, &Fcb->Vcb->DirNotifyList,
                                (PSTRING)FileName, nameOffset,
                                NULL,  // StreamName
                                NULL,  // NormalizedParentName
                                FilterMatch, Action,
                                NULL);  // TargetContext
  } __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    DOKAN_INIT_LOGGER(logger, Fcb->Vcb->Dcb->DriverObject, 0);
    try {
      // This case is attested in the wild but very rare. We don't know why it
      // happens.
      return DokanLogError(
          &logger, STATUS_OBJECT_NAME_INVALID,
          L"Access violation in file change notification for \"%wZ\".",
          FileName);
    } __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
      // This is not a case we think ever happens, but we may as well not
      // crash if it does.
      return DokanLogError(
          &logger, STATUS_INVALID_PARAMETER,
          L"Access violation on the file name passed in a notification.");
    }
  }
  DOKAN_LOG_FINE_IRP(RequestContext,
                     "FCB=%p NameOffset=%x FilterMatch=%x Action=%x Success",
                     Fcb, nameOffset, FilterMatch, Action);
  return STATUS_SUCCESS;
}

// DokanNotifyReportChange should be called with the Fcb at least share-locked.
// due to the ro access to the FileName field.
NTSTATUS DokanNotifyReportChange(__in PREQUEST_CONTEXT RequestContext,
                                 __in PDokanFCB Fcb,
                                 __in ULONG FilterMatch,
                                 __in ULONG Action) {
  ASSERT(Fcb != NULL);
  return DokanNotifyReportChange0(RequestContext, Fcb, &Fcb->FileName,
                                  FilterMatch,
                                  Action);
}

BOOLEAN
DokanCheckCCB(__in PREQUEST_CONTEXT RequestContext,
              __in_opt PDokanCCB Ccb) {
  UNREFERENCED_PARAMETER(RequestContext);

  ASSERT(RequestContext->Dcb != NULL);
  if (!RequestContext->Dcb) {
    return FALSE;
  }

  if (Ccb == NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Ccb is NULL");
    return FALSE;
  }

  if (Ccb->MountId != RequestContext->Dcb->MountId) {
    DOKAN_LOG_FINE_IRP(RequestContext, "MountId is different: Ccb=%d Dcb=%d",
                       Ccb->MountId, RequestContext->Dcb->MountId);
    return FALSE;
  }

  if (!RequestContext->Vcb || IsUnmountPendingVcb(RequestContext->Vcb)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Not mounted");
    return FALSE;
  }

  return TRUE;
}

NTSTATUS
DokanAllocateMdl(__in PREQUEST_CONTEXT RequestContext, __in ULONG Length) {
  if (RequestContext->Irp->MdlAddress == NULL) {
    RequestContext->Irp->MdlAddress =
        IoAllocateMdl(RequestContext->Irp->UserBuffer, Length, FALSE, FALSE,
                      RequestContext->Irp);

    if (RequestContext->Irp->MdlAddress == NULL) {
      DOKAN_LOG_FINE_IRP(RequestContext, "IoAllocateMdl returned NULL");
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    __try {
      MmProbeAndLockPages(RequestContext->Irp->MdlAddress,
                          RequestContext->Irp->RequestorMode, IoWriteAccess);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
      DOKAN_LOG_FINE_IRP(RequestContext, "MmProveAndLockPages error");
      IoFreeMdl(RequestContext->Irp->MdlAddress);
      RequestContext->Irp->MdlAddress = NULL;
      return STATUS_INSUFFICIENT_RESOURCES;
    }
  }
  return STATUS_SUCCESS;
}

VOID DokanFreeMdl(__in PIRP Irp) {
  if (Irp->MdlAddress != NULL) {
    MmUnlockPages(Irp->MdlAddress);
    IoFreeMdl(Irp->MdlAddress);
    Irp->MdlAddress = NULL;
  }
}

ULONG
PointerAlignSize(ULONG sizeInBytes) {
  // power of 2 cheat to avoid using %
  ULONG remainder = sizeInBytes & (sizeof(void *) - 1);

  if (remainder > 0) {
    return sizeInBytes + (sizeof(void *) - remainder);
  }

  return sizeInBytes;
}

#define DOKAN_RESOURCE_LOCK_DEBUG_INTERVAL_MSEC 10
#define DOKAN_RESOURCE_LOCK_WARNING_MSEC 1000000 // 1 sec

static const UNICODE_STRING noName = RTL_CONSTANT_STRING(L"<no name>");

VOID DokanLockWarn(__in const ERESOURCE *Resource,
                   __in const DokanResourceDebugInfo *DebugInfo,
                   __in PDOKAN_LOGGER Logger,
                   __in const char *Site,
                   __in const UNICODE_STRING *ObjectName,
                   __in const void *ObjectPointer) {
  if (ObjectName == NULL || ObjectName->Length == 0) {
    ObjectName = &noName;
  }

  if (DebugInfo->ExclusiveOwnerThread != NULL) {
    DokanLogInfo(
        Logger,
        L"Stuck trying to lock \"%wZ\" (%I64x with ERESOURCE %I64x)"
            L" in thread %I64x at %S."
            L" Current exclusive owner is thread %I64x"
            L" with outermost lock at %S.",
        ObjectName,
        ObjectPointer,
        Resource,
        KeGetCurrentThread(),
        Site,
        DebugInfo->ExclusiveOwnerThread,
        DebugInfo->ExclusiveLockSite);
    // This is like DDbgPrint but gets written "unconditionally" as long as you
    // have the Debug Print Filter set up in the registry. Normal DDbgPrint
    // calls are utterly stripped from release builds. We know that
    // DokanLockWarn doesn't get invoked unless DriveFS is in lock debug mode,
    // so this is OK.
    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_TRACE_LEVEL,
        "Stuck trying to lock \"%wZ\" (%I64x with ERESOURCE %I64x)"
            " in thread %I64x at %s."
            " Current exclusive owner is thread %I64x"
            " with outermost lock at %s.\n",
        ObjectName,
        ObjectPointer,
        Resource,
        KeGetCurrentThread(),
        Site,
        DebugInfo->ExclusiveOwnerThread,
        DebugInfo->ExclusiveLockSite);
  } else {
    DokanLogInfo(
        Logger,
        L"Stuck trying to lock \"%wZ\" (%I64x with ERESOURCE %I64x)"
            L" in thread %I64x at %S."
            L" This resource has an unknown shared lock.",
        ObjectName,
        ObjectPointer,
        Resource,
        KeGetCurrentThread(),
        Site);
    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_TRACE_LEVEL,
        "Stuck trying to lock \"%wZ\" (%I64x with ERESOURCE %I64x)"
            " in thread %I64x at %s."
            " This resource has an unknown shared lock.\n",
        ObjectName,
        ObjectPointer,
        Resource,
        KeGetCurrentThread(),
        Site);
  }
}

VOID DokanLockNotifyResolved(__in const ERESOURCE *Resource,
                             __in PDOKAN_LOGGER Logger) {
  DokanLogInfo(Logger,
      L"Blocking on ERESOURCE %I64x has resolved on thread %I64x",
      Resource,
      KeGetCurrentThread());
  DbgPrintEx(
      DPFLTR_IHVDRIVER_ID,
      DPFLTR_TRACE_LEVEL,
      "Blocking on ERESOURCE %I64x has resolved on thread %I64x\n",
      Resource,
      KeGetCurrentThread());
}

VOID DokanResourceLockWithDebugInfo(__in BOOLEAN Writable,
                                    __in PERESOURCE Resource,
                                    __in PDokanResourceDebugInfo DebugInfo,
                                    __in PDOKAN_LOGGER Logger,
                                    __in const char *Site,
                                    __in const UNICODE_STRING *ObjectName,
                                    __in const void *ObjectPointer) {
  // The wait is in 100ns units. Negative means "from now" as opposed to an
  // absolute wake up time.
  LARGE_INTEGER wait = RtlConvertLongToLargeInteger(
      -DOKAN_RESOURCE_LOCK_DEBUG_INTERVAL_MSEC * 10);
  LARGE_INTEGER lastWarnTime = {0};
  LARGE_INTEGER systemTime = {0};
  BOOLEAN warned = FALSE;
  BOOLEAN result = FALSE;
  for (;;) {
    KeEnterCriticalRegion();
    if (Writable) {
      result = ExAcquireResourceExclusiveLite(Resource, FALSE);
    } else {
      result = ExAcquireResourceSharedLite(Resource, FALSE);
    }
    if (result) {
      break;
    }
    KeLeaveCriticalRegion();
    KeQuerySystemTime(&systemTime);
    if (lastWarnTime.QuadPart == 0) {
      lastWarnTime = systemTime;
    } else if ((systemTime.QuadPart - lastWarnTime.QuadPart) / 10
               >= DOKAN_RESOURCE_LOCK_WARNING_MSEC) {
      DokanLockWarn(Resource, DebugInfo, Logger, Site, ObjectName,
                    ObjectPointer);
      warned = TRUE;
      lastWarnTime = systemTime;
    }
    KeDelayExecutionThread(KernelMode, TRUE, &wait);
  }

  if (ExIsResourceAcquiredExclusiveLite(Resource)) {
    if (DebugInfo->ExclusiveLockCount == 0) {
      DebugInfo->ExclusiveLockSite = Site;
      DebugInfo->ExclusiveOwnerThread = KeGetCurrentThread();
    }
    // Note that we may need this increment even for a non-writable request,
    // since any recursive acquire of an exclusive lock is exclusive.
    ++DebugInfo->ExclusiveLockCount;
  }
  if (warned) {
    DokanLockNotifyResolved(Resource, Logger);
  }
}

VOID DokanResourceUnlockWithDebugInfo(
    __in PERESOURCE Resource,
    __in PDokanResourceDebugInfo DebugInfo) {
  if (ExIsResourceAcquiredExclusiveLite(Resource)) {
    if (--DebugInfo->ExclusiveLockCount == 0) {
      DebugInfo->ExclusiveLockSite = NULL;
      DebugInfo->ExclusiveOwnerThread = NULL;
    }
  }
  ExReleaseResourceLite(Resource);
  KeLeaveCriticalRegion();
}

BOOLEAN DokanVCBTryLockRW(PDokanVCB Vcb) {
  KeEnterCriticalRegion();
  BOOLEAN result = ExAcquireResourceExclusiveLite(&(Vcb)->Resource, FALSE);
  if (!result) {
    KeLeaveCriticalRegion();
  }
  return result;
}

ULONG GetOplockControlDebugInfoBit(ULONG FsControlCode) {
  switch (FsControlCode) {
  case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    return 1;
  case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    return 2;
  case FSCTL_REQUEST_BATCH_OPLOCK:
    return 4;
  case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    return 8;
  case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    return 16;
  case FSCTL_OPLOCK_BREAK_NOTIFY:
    return 32;
  case FSCTL_OPLOCK_BREAK_ACK_NO_2:
    return 64;
  case FSCTL_REQUEST_FILTER_OPLOCK:
    return 128;
  case FSCTL_REQUEST_OPLOCK:
    return 256;
  default:
    return 65536;
  }
}

void OplockDebugRecordMajorFunction(__in PDokanFCB Fcb, UCHAR MajorFunction) {
  InterlockedOr((PLONG)&Fcb->OplockDebugInfo.MajorFunctionMask,
                (1 << MajorFunction));
}

void OplockDebugRecordFlag(__in PDokanFCB Fcb, ULONG Flag) {
  InterlockedOr((PLONG)&Fcb->OplockDebugInfo.Flags, Flag);
}

void OplockDebugRecordProcess(__in PDokanFCB Fcb) {
  InterlockedOr64((PLONG64)&Fcb->OplockDebugInfo.OplockProcessMask,
                  (LONG64)PsGetCurrentProcess());
}

void OplockDebugRecordRequest(__in PDokanFCB Fcb,
                              __in ULONG FsControlMinorFunction,
                              __in ULONG OplockLevel) {
  OplockDebugRecordMajorFunction(Fcb, IRP_MJ_FILE_SYSTEM_CONTROL);
  InterlockedOr((PLONG)&Fcb->OplockDebugInfo.OplockFsctlMask,
                GetOplockControlDebugInfoBit(FsControlMinorFunction));
  InterlockedOr((PLONG)&Fcb->OplockDebugInfo.OplockLevelMask, OplockLevel);
  InterlockedIncrement(&Fcb->OplockDebugInfo.OplockFsctlCount);
  OplockDebugRecordProcess(Fcb);
}

void OplockDebugRecordCreateRequest(__in PDokanFCB Fcb,
                                    __in ACCESS_MASK AccessMask,
                                    __in ULONG ShareAccess) {
  OplockDebugRecordMajorFunction(Fcb, IRP_MJ_CREATE);
  InterlockedOr((PLONG)&Fcb->OplockDebugInfo.AccessMask, (LONG)AccessMask);
  InterlockedOr((PLONG)&Fcb->OplockDebugInfo.ShareAccessMask,
                (LONG)ShareAccess);
}

void OplockDebugRecordAtomicRequest(PDokanFCB Fcb) {
  OplockDebugRecordProcess(Fcb);
  InterlockedIncrement(&Fcb->OplockDebugInfo.AtomicRequestCount);
}

VOID RunAsSystem(_In_ PKSTART_ROUTINE StartRoutine, PVOID StartContext) {
  HANDLE handle;
  PKTHREAD thread;
  OBJECT_ATTRIBUTES objectAttribs;

  InitializeObjectAttributes(&objectAttribs, NULL, OBJ_KERNEL_HANDLE, NULL,
                             NULL);
  NTSTATUS status =
      PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &objectAttribs, NULL,
                           NULL, StartRoutine, StartContext);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("PsCreateSystemThread failed: 0x%X %s", status,
              DokanGetNTSTATUSStr(status));
  } else {
    ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, NULL, KernelMode,
                              &thread, NULL);
    ZwClose(handle);
    KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(thread);
  }
}
