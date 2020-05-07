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

#include "dokan.h"

#include "dokanfs_msg.h"

static VOID InitMultiVersionResources();

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(INIT, InitMultiVersionResources)
#pragma alloc_text(PAGE, DokanUnload)
#endif

ULONG g_Debug = DOKAN_DEBUG_NONE;
LOOKASIDE_LIST_EX g_DokanCCBLookasideList;
LOOKASIDE_LIST_EX g_DokanFCBLookasideList;
LOOKASIDE_LIST_EX g_DokanEResourceLookasideList;
BOOLEAN g_FixFileNameForReparseMountPoint;

NPAGED_LOOKASIDE_LIST DokanIrpEntryLookasideList;
ULONG DokanMdlSafePriority = 0;
UNICODE_STRING FcbFileNameNull;

static const UNICODE_STRING dosDevicesPrefix =
    RTL_CONSTANT_STRING(L"\\DosDevices\\");

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

  DDbgPrint("DokanFastIoCheckIfPossible\n");
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

  DDbgPrint("DokanFastIoRead\n");
  return FALSE;
}

FAST_IO_ACQUIRE_FILE DokanAcquireForCreateSection;
VOID DokanAcquireForCreateSection(__in PFILE_OBJECT FileObject) {
  PDokanCCB ccb = (PDokanCCB)FileObject->FsContext2;
  if (ccb != NULL) {
    DokanFCBLockRW(ccb->Fcb);
    KeLeaveCriticalRegion();
  }
  DDbgPrint("DokanAcquireForCreateSection\n");
}

FAST_IO_RELEASE_FILE DokanReleaseForCreateSection;
VOID DokanReleaseForCreateSection(__in PFILE_OBJECT FileObject) {
  PDokanCCB ccb = (PDokanCCB)FileObject->FsContext2;
  if (ccb != NULL) {
    KeEnterCriticalRegion();
    DokanFCBUnlock(ccb->Fcb);
  }

  DDbgPrint("DokanReleaseForCreateSection\n");
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

  DDbgPrint("DokanFilterCallbackAcquireForCreateSection\n");

  ccb = CallbackData->FileObject->FsContext2;

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
DokanLookasideCreate(LOOKASIDE_LIST_EX *pCache, size_t cbElement) {
  NTSTATUS Status = ExInitializeLookasideListEx(
      pCache, NULL, NULL, NonPagedPool, 0, cbElement, TAG, 0);

  if (!NT_SUCCESS(Status)) {
    DDbgPrint("ExInitializeLookasideListEx failed, Status (0x%x)", Status);
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

  DDbgPrint("==> DriverEntry ver.%x, %s %s\n", DOKAN_DRIVER_VERSION, __DATE__,
            __TIME__);

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
  DriverObject->MajorFunction[IRP_MJ_PNP] = DokanBuildRequest;

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
    DDbgPrint("  FsRtlRegisterFileSystemFilterCallbacks returned 0x%x\n",
              status);
    return status;
  }

  if (!DokanLookasideCreate(&g_DokanCCBLookasideList, sizeof(DokanCCB))) {
    DDbgPrint("  DokanLookasideCreate g_DokanCCBLookasideList  failed");
    CleanupGlobalDiskDevice(dokanGlobal);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (!DokanLookasideCreate(&g_DokanFCBLookasideList, sizeof(DokanFCB))) {
    DDbgPrint("  DokanLookasideCreate g_DokanFCBLookasideList  failed");
    CleanupGlobalDiskDevice(dokanGlobal);
    ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (!DokanLookasideCreate(&g_DokanEResourceLookasideList,
                            sizeof(ERESOURCE))) {
    DDbgPrint("  DokanLookasideCreate g_DokanEResourceLookasideList  failed");
    CleanupGlobalDiskDevice(dokanGlobal);
    ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
    ExDeleteLookasideListEx(&g_DokanFCBLookasideList);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  // Detect if we are running on a older version than NTDDI_WIN10_RS4
  // needing to fix FileName during Reparse MountPoint.
  g_FixFileNameForReparseMountPoint =
      !RtlIsNtDdiVersionAvailable(0x0A000005);

  DDbgPrint("<== DriverEntry\n");

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

  DDbgPrint("==> DokanUnload\n");

  PAGED_CODE();

  dokanGlobal = deviceObject->DeviceExtension;
  if (GetIdentifierType(dokanGlobal) == DGL) {
    DDbgPrint("  Delete Global DeviceObject\n");
    CleanupGlobalDiskDevice(dokanGlobal);
  }

  ExDeleteNPagedLookasideList(&DokanIrpEntryLookasideList);

  ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
  ExDeleteLookasideListEx(&g_DokanFCBLookasideList);
  ExDeleteLookasideListEx(&g_DokanEResourceLookasideList);

  DDbgPrint("<== DokanUnload\n");
}

NTSTATUS
DokanDispatchShutdown(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  // PAGED_CODE();
  DDbgPrint("==> DokanShutdown\n");

  DokanCompleteIrpRequest(Irp, STATUS_SUCCESS, 0);

  DDbgPrint("<== DokanShutdown\n");
  return STATUS_SUCCESS;
}

BOOLEAN
DokanNoOpAcquire(__in PVOID Fcb, __in BOOLEAN Wait) {
  UNREFERENCED_PARAMETER(Fcb);
  UNREFERENCED_PARAMETER(Wait);

  DDbgPrint("==> DokanNoOpAcquire\n");

  ASSERT(IoGetTopLevelIrp() == NULL);

  IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

  DDbgPrint("<== DokanNoOpAcquire\n");

  return TRUE;
}

VOID DokanNoOpRelease(__in PVOID Fcb) {
  DDbgPrint("==> DokanNoOpRelease\n");
  ASSERT(IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

  IoSetTopLevelIrp(NULL);

  UNREFERENCED_PARAMETER(Fcb);

  DDbgPrint("<== DokanNoOpRelease\n");
}

NTSTATUS DokanCheckOplock(
    __in PDokanFCB Fcb,
    __in PIRP Irp,
    __in_opt PVOID Context,
    __in_opt POPLOCK_WAIT_COMPLETE_ROUTINE CompletionRoutine,
    __in_opt POPLOCK_FS_PREPOST_IRP PostIrpRoutine) {
  ASSERT(Fcb->Vcb != NULL);
  ASSERT(Fcb->Vcb->Dcb != NULL);
  if (Fcb->Vcb != NULL && Fcb->Vcb->Dcb != NULL &&
      !Fcb->Vcb->Dcb->OplocksDisabled) {
    return FsRtlCheckOplock(DokanGetFcbOplock(Fcb), Irp, Context,
                            CompletionRoutine, PostIrpRoutine);
  }
  return STATUS_SUCCESS;
}

#define PrintStatus(val, flag)                                                 \
  if (val == flag)                                                             \
  DDbgPrint("  status = " #flag "\n")

#define DOKAN_LOG_MAX_CHAR_COUNT 2048
#define DOKAN_LOG_MAX_PACKET_BYTES \
    (ERROR_LOG_MAXIMUM_SIZE - sizeof(IO_ERROR_LOG_PACKET))
#define DOKAN_LOG_MAX_PACKET_NONNULL_CHARS \
    (DOKAN_LOG_MAX_PACKET_BYTES / sizeof(WCHAR) - 1)

VOID DokanPrintToSysLog(__in PDRIVER_OBJECT DriverObject,
                        __in UCHAR MajorFunctionCode,
                        __in NTSTATUS MessageId,
                        __in NTSTATUS Status,
                        __in LPCTSTR Format,
                        __in va_list Args) {
  NTSTATUS status = STATUS_SUCCESS;
  PIO_ERROR_LOG_PACKET packet = NULL;
  WCHAR *message = NULL;
  size_t messageCapacity = DOKAN_LOG_MAX_CHAR_COUNT;
  size_t messageCharCount = 0;
  size_t messageCharsWritten = 0;
  size_t packetCount = 0;
  size_t i = 0;
  UCHAR packetCharCount = 0;
  UCHAR packetSize = 0;

  __try {
    message = DokanAlloc(sizeof(WCHAR) * messageCapacity);
    if (message == NULL) {
      DDbgPrint("Failed to allocate message of capacity %d\n", messageCapacity);
      __leave;
    }

    status = RtlStringCchVPrintfW(message, messageCapacity, Format, Args);
    if (status == STATUS_BUFFER_OVERFLOW) {
      // In this case we want to at least log what we can fit.
      DDbgPrint("Log message was larger than DOKAN_LOG_MAX_CHAR_COUNT."
                " Format: %S\n", Format);
    } else if (status != STATUS_SUCCESS) {
      DDbgPrint("Failed to generate log message with format: %S; status: %x\n",
                Format, status);
      __leave;
    }

    status = RtlStringCchLengthW(message, messageCapacity, &messageCharCount);
    if (status != STATUS_SUCCESS) {
      DDbgPrint("Failed to determine message length, status: %x\n", status);
      __leave;
    }

    packetCount = messageCharCount / DOKAN_LOG_MAX_PACKET_NONNULL_CHARS;
    if (messageCharCount % DOKAN_LOG_MAX_PACKET_NONNULL_CHARS != 0) {
      ++packetCount;
    }

    for (i = 0; i < packetCount; i++) {
      packetCharCount = (UCHAR)min(messageCharCount - messageCharsWritten,
                                   DOKAN_LOG_MAX_PACKET_NONNULL_CHARS);
      packetSize =
          sizeof(IO_ERROR_LOG_PACKET) + sizeof(WCHAR) * (packetCharCount + 1);
      packet = IoAllocateErrorLogEntry(DriverObject, packetSize);
      if (packet == NULL) {
        DDbgPrint("Failed to allocate packet of size %d\n", packetSize);
        __leave;
      }
      RtlZeroMemory(packet, packetSize);
      packet->MajorFunctionCode = MajorFunctionCode;
      packet->NumberOfStrings = 1;
      packet->StringOffset =
          (USHORT)((char *)&packet->DumpData[0] - (char *)packet);
      packet->ErrorCode = MessageId;
      packet->FinalStatus = Status;
      RtlCopyMemory(&packet->DumpData[0], message + messageCharsWritten,
                    sizeof(WCHAR) * packetCharCount);
      IoWriteErrorLogEntry(packet); // Destroys packet.
      packet = NULL;
      messageCharsWritten += packetCharCount;
    }
  } __finally {
    if (message != NULL) {
      ExFreePool(message);
    }
  }
}

NTSTATUS DokanLogError(__in PDOKAN_LOGGER Logger,
                       __in NTSTATUS Status,
                       __in LPCTSTR Format,
                       ...) {
  if (g_Debug & DOKAN_DEBUG_DEFAULT) {
    va_list args;
    va_start(args, Format);
    DokanPrintToSysLog(Logger->DriverObject, Logger->MajorFunctionCode,
                       DOKANFS_ERROR_MSG, Status, Format, args);
    va_end(args);
  }
  return Status;
}

VOID DokanLogInfo(__in PDOKAN_LOGGER Logger, __in LPCTSTR Format, ...) {
  if (g_Debug & DOKAN_DEBUG_DEFAULT) {
    va_list args;
    va_start(args, Format);
    DokanPrintToSysLog(Logger->DriverObject, Logger->MajorFunctionCode,
                       DOKANFS_INFO_MSG, STATUS_SUCCESS, Format, args);
    va_end(args);
  }
}

VOID DokanCaptureBackTrace(__out PDokanBackTrace Trace) {
  PVOID rawTrace[4];
  USHORT count = RtlCaptureStackBackTrace(1, 4, rawTrace, NULL);
  Trace->Address = (ULONG64)((count > 0) ? rawTrace[0] : 0);
  Trace->ReturnAddresses =
        (((count > 1) ? ((ULONG64)rawTrace[1] & 0xfffff) : 0) << 40)
      | (((count > 2) ? ((ULONG64)rawTrace[2] & 0xfffff) : 0) << 20)
      |  ((count > 3) ? ((ULONG64)rawTrace[3] & 0xfffff) : 0);
}

VOID DokanPrintNTStatus(NTSTATUS Status) {
  DDbgPrint("  status = 0x%x\n", Status);

  PrintStatus(Status, STATUS_SUCCESS);
  PrintStatus(Status, STATUS_PENDING);
  PrintStatus(Status, STATUS_NO_MORE_FILES);
  PrintStatus(Status, STATUS_END_OF_FILE);
  PrintStatus(Status, STATUS_NO_SUCH_FILE);
  PrintStatus(Status, STATUS_NOT_IMPLEMENTED);
  PrintStatus(Status, STATUS_BUFFER_OVERFLOW);
  PrintStatus(Status, STATUS_FILE_IS_A_DIRECTORY);
  PrintStatus(Status, STATUS_SHARING_VIOLATION);
  PrintStatus(Status, STATUS_OBJECT_NAME_INVALID);
  PrintStatus(Status, STATUS_OBJECT_NAME_NOT_FOUND);
  PrintStatus(Status, STATUS_OBJECT_NAME_COLLISION);
  PrintStatus(Status, STATUS_OBJECT_PATH_INVALID);
  PrintStatus(Status, STATUS_OBJECT_PATH_NOT_FOUND);
  PrintStatus(Status, STATUS_OBJECT_PATH_SYNTAX_BAD);
  PrintStatus(Status, STATUS_ACCESS_DENIED);
  PrintStatus(Status, STATUS_ACCESS_VIOLATION);
  PrintStatus(Status, STATUS_INVALID_PARAMETER);
  PrintStatus(Status, STATUS_INVALID_USER_BUFFER);
  PrintStatus(Status, STATUS_INVALID_HANDLE);
  PrintStatus(Status, STATUS_INSUFFICIENT_RESOURCES);
  PrintStatus(Status, STATUS_DEVICE_DOES_NOT_EXIST);
  PrintStatus(Status, STATUS_INVALID_DEVICE_REQUEST);
  PrintStatus(Status, STATUS_VOLUME_DISMOUNTED);
  PrintStatus(Status, STATUS_NO_SUCH_DEVICE);
}

VOID DokanCompleteIrpRequest(__in PIRP Irp, __in NTSTATUS Status,
                             __in ULONG_PTR Info) {
  if (Irp == NULL) {
    DDbgPrint("  Irp is NULL, so no complete required\n");
    return;
  }
  if (Status == -1) {
    DDbgPrint("  Status is -1 which is not valid NTSTATUS\n");
    Status = STATUS_INVALID_PARAMETER;
  }
  if (Status != STATUS_PENDING) {
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
  }
  DokanPrintNTStatus(Status);
}

NTSTATUS DokanNotifyReportChange0(__in PDokanFCB Fcb,
                                  __in PUNICODE_STRING FileName,
                                  __in ULONG FilterMatch,
                                  __in ULONG Action) {
  USHORT nameOffset;

  DDbgPrint("==> DokanNotifyReportChange %wZ\n", FileName);

  ASSERT(Fcb != NULL);
  ASSERT(FileName != NULL);

  // Alternate streams are supposed to use a different set of action
  // and filter values, but we do not expect the caller to be aware of this.
  if (DokanUnicodeStringChar(FileName, L':') != -1) { //FileStream

    //Convert file action to stream action
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
  nameOffset *= sizeof(WCHAR); // Offset is in bytes

  __try {
    FsRtlNotifyFullReportChange(Fcb->Vcb->NotifySync, &Fcb->Vcb->DirNotifyList,
                                (PSTRING)FileName, nameOffset,
                                NULL, // StreamName
                                NULL, // NormalizedParentName
                                FilterMatch, Action,
                                NULL); // TargetContext
  } __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION
              ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    DOKAN_INIT_LOGGER(logger, Fcb->Vcb->Dcb->DriverObject, 0);
    try {
      // This case is attested in the wild but very rare. We don't know why it
      // happens.
      return DokanLogError(
          &logger, STATUS_OBJECT_NAME_INVALID,
          L"Access violation in file change notification for %wZ.", FileName);
    } __except(GetExceptionCode() == STATUS_ACCESS_VIOLATION
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
       // This is not a case we think ever happens, but we may as well not
       // crash if it does.
       return DokanLogError(
            &logger, STATUS_INVALID_PARAMETER,
            L"Access violation on the file name passed in a notification.");
    }
  }
  DDbgPrint("<== DokanNotifyReportChange\n");
  return STATUS_SUCCESS;
}

// DokanNotifyReportChange should be called with the Fcb at least share-locked.
// due to the ro access to the FileName field.
NTSTATUS DokanNotifyReportChange(__in PDokanFCB Fcb, __in ULONG FilterMatch,
                                 __in ULONG Action) {
  ASSERT(Fcb != NULL);
  return DokanNotifyReportChange0(Fcb, &Fcb->FileName, FilterMatch, Action);
}

VOID PrintIdType(__in VOID *Id) {
  if (Id == NULL) {
    DDbgPrint("    IdType = NULL\n");
    return;
  }
  switch (GetIdentifierType(Id)) {
  case DGL:
    DDbgPrint("    IdType = DGL\n");
    break;
  case DCB:
    DDbgPrint("   IdType = DCB\n");
    break;
  case VCB:
    DDbgPrint("   IdType = VCB\n");
    break;
  case FCB:
    DDbgPrint("   IdType = FCB\n");
    break;
  case CCB:
    DDbgPrint("   IdType = CCB\n");
    break;
  default:
    DDbgPrint("   IdType = Unknown\n");
    break;
  }
}

BOOLEAN
DokanCheckCCB(__in PDokanDCB Dcb, __in_opt PDokanCCB Ccb) {
  PDokanVCB vcb;
  ASSERT(Dcb != NULL);
  if (GetIdentifierType(Dcb) != DCB) {
    PrintIdType(Dcb);
    return FALSE;
  }

  if (Ccb == NULL) {
    PrintIdType(Dcb);
    DDbgPrint("   ccb is NULL\n");
    return FALSE;
  }

  if (Ccb->MountId != Dcb->MountId) {
    DDbgPrint("   MountId is different\n");
    return FALSE;
  }

  vcb = Dcb->Vcb;
  if (!vcb || IsUnmountPendingVcb(vcb)) {
    DDbgPrint("  Not mounted\n");
    return FALSE;
  }

  return TRUE;
}

NTSTATUS
DokanAllocateMdl(__in PIRP Irp, __in ULONG Length) {
  if (Irp->MdlAddress == NULL) {
    Irp->MdlAddress = IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp);

    if (Irp->MdlAddress == NULL) {
      DDbgPrint("    IoAllocateMdl returned NULL\n");
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    __try {
      MmProbeAndLockPages(Irp->MdlAddress, Irp->RequestorMode, IoWriteAccess);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
      DDbgPrint("    MmProveAndLockPages error\n");
      IoFreeMdl(Irp->MdlAddress);
      Irp->MdlAddress = NULL;
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
        L"Stuck trying to lock %wZ (%I64x with ERESOURCE %I64x)"
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
        "Stuck trying to lock %wZ (%I64x with ERESOURCE %I64x)"
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
        L"Stuck trying to lock %wZ (%I64x with ERESOURCE %I64x)"
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
        "Stuck trying to lock %wZ (%I64x with ERESOURCE %I64x)"
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
      "Blocking on ERESOURCE %I64x has resolved on thread %I64x",
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

BOOLEAN DokanScheduleFcbForGarbageCollection(__in PDokanVCB Vcb,
                                             __in PDokanFCB Fcb) {
  DOKAN_INIT_LOGGER(logger, Vcb->Dcb->DeviceObject->DriverObject, 0);
  if (Vcb->FcbGarbageCollectorThread == NULL) {
    return FALSE;
  }
  if (Fcb->NextGarbageCollectableFcb.Flink != NULL) {
    // This is probably not intentional but theoretically OK.
    DokanLogInfo(&logger,
                 L"Warning: scheduled an FCB for garbage collection when it is"
                 L" already scheduled.");
    return TRUE;
  }
  Fcb->GarbageCollectionGracePeriodPassed = FALSE;
  InsertTailList(&Vcb->FcbGarbageList, &Fcb->NextGarbageCollectableFcb);
  KeSetEvent(&Vcb->FcbGarbageListNotEmpty, IO_NO_INCREMENT, FALSE);
  return TRUE;
}

VOID DokanCancelFcbGarbageCollection(__in PDokanFCB Fcb) {
  if (Fcb->NextGarbageCollectableFcb.Flink != NULL) {
    ++Fcb->Vcb->VolumeMetrics.FcbGarbageCollectionCancellations;
    RemoveEntryList(&Fcb->NextGarbageCollectableFcb);
    Fcb->NextGarbageCollectableFcb.Flink = NULL;
    Fcb->GarbageCollectionGracePeriodPassed = FALSE;
    DokanFCBFlagsClearBit(Fcb, DOKAN_DELETE_ON_CLOSE);
    DokanFCBFlagsClearBit(Fcb, DOKAN_FILE_DIRECTORY);
  }
}

// Called with the VCB locked. Immediately deletes the FCBs that are ready to
// delete. Returns how many are skipped due to having been scheduled too
// recently. If Force is TRUE then all the scheduled ones are deleted, and the
// return value is 0.
ULONG DeleteFcbGarbageAndGetRemainingCount(__in PDokanVCB Vcb,
                                           __in BOOLEAN Force) {
  ULONG remainingCount = 0;
  PLIST_ENTRY thisEntry = NULL;
  PLIST_ENTRY nextEntry = NULL;
  PDokanFCB nextFcb = NULL;
  for (thisEntry = Vcb->FcbGarbageList.Flink;
       thisEntry != &Vcb->FcbGarbageList; thisEntry = nextEntry) {
    nextEntry = thisEntry->Flink;
    nextFcb = CONTAINING_RECORD(thisEntry, DokanFCB,
                                NextGarbageCollectableFcb);
    // We want it to have been scheduled for at least one timer interval so
    // that there is a guaranteed window of possible reuse, which achieves the
    // performance gains we are aiming for with GC.
    if (Force || nextFcb->GarbageCollectionGracePeriodPassed) {
      RemoveEntryList(thisEntry);
      DokanFCBLockRW(nextFcb);
      DokanDeleteFcb(Vcb, nextFcb);
    } else {
      nextFcb->GarbageCollectionGracePeriodPassed = TRUE;
      ++remainingCount;
    }
  }
  ASSERT(!Force || remainingCount == 0);
  // When an FCB gets deleted by a GC cycle already in progress at the time of
  // its scheduling, there's no point in triggering a follow-up cycle for that
  // one.
  if (remainingCount == 0) {
    KeClearEvent(&Vcb->FcbGarbageListNotEmpty);
  }
  return remainingCount;
}

BOOLEAN DokanForceFcbGarbageCollection(__in PDokanVCB Vcb) {
  if (Vcb->FcbGarbageCollectorThread == NULL
      || IsListEmpty(&Vcb->FcbGarbageList)) {
    return FALSE;
  }
  ++Vcb->VolumeMetrics.ForcedFcbGarbageCollectionPasses;
  DeleteFcbGarbageAndGetRemainingCount(Vcb, /*Force=*/TRUE);
  return TRUE;
}

// Called when there are no pending garbage FCBs and we may need to wait
// indefinitely for one to appear.
NTSTATUS WaitForNewFcbGarbage(__in PDokanVCB Vcb) {
  PVOID events[2];
  events[0] = &Vcb->Dcb->ReleaseEvent;
  events[1] = &Vcb->FcbGarbageListNotEmpty;
  NTSTATUS status = KeWaitForMultipleObjects(2, events, WaitAny, Executive,
                                             KernelMode, FALSE, NULL, NULL);
  return status == STATUS_WAIT_1 ? STATUS_SUCCESS : STATUS_CANCELLED;
}

// Called when there are some pending garbage FCBs. This function keeps an eye
// on them until they expire and then deletes them, returning when there are no
// more pending ones.
NTSTATUS AgeAndDeleteFcbGarbage(__in PDokanVCB Vcb, __in PKTIMER Timer) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  ULONG pendingCount = 0;
  PVOID events[2];
  BOOLEAN waited = FALSE;
  events[0] = &Vcb->Dcb->ReleaseEvent;
  events[1] = Timer;
  ++Vcb->VolumeMetrics.NormalFcbGarbageCollectionCycles;
  for (;;) {
    // Get rid of any garbage that is ready to delete.
    DokanVCBLockRW(Vcb);
    ++Vcb->VolumeMetrics.NormalFcbGarbageCollectionPasses;
    pendingCount = DeleteFcbGarbageAndGetRemainingCount(Vcb, /*Force=*/FALSE);
    DokanVCBUnlock(Vcb);
    // If we have cleared out all the garbage, return so the garbage collector
    // will do an indefinite wait for new garbage. But we wait at least once on
    // the GC interval timer to avoid having multiple no-op cycles in one
    // interval.
    if (pendingCount == 0 && waited) {
      status = STATUS_SUCCESS;
      break;
    }
    // If there are any entries that haven't aged long enough, age them using
    // the timer until they are ready.
    status = KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode,
                                      FALSE, NULL, NULL);
    waited = TRUE;
    if (status != STATUS_WAIT_1) {
      status = STATUS_CANCELLED;
      break;
    }
  }
  return status;
}

// The thread function for the dedicated FCB garbage collection thread.
VOID FcbGarbageCollectorThread(__in PVOID pVcb) {
  KTIMER timer;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  LARGE_INTEGER timeout = {0};
  PDokanVCB Vcb = pVcb;
  DOKAN_INIT_LOGGER(logger, Vcb->Dcb->DeviceObject->DriverObject, 0);
  KeInitializeTimerEx(&timer, SynchronizationTimer);
  KeSetTimerEx(&timer, timeout, Vcb->Dcb->FcbGarbageCollectionIntervalMs, NULL);
  DokanLogInfo(&logger, L"Starting FCB garbage collector with %lu ms interval.",
               Vcb->Dcb->FcbGarbageCollectionIntervalMs);
  for (;;) {
    status = WaitForNewFcbGarbage(Vcb);
    if (status != STATUS_SUCCESS) {
      break;
    }
    status = AgeAndDeleteFcbGarbage(Vcb, &timer);
    if (status != STATUS_SUCCESS) {
      break;
    }
  }
  DokanLogInfo(&logger, L"Stopping FCB garbage collector.");
  KeCancelTimer(&timer);
}

void DokanStartFcbGarbageCollector(PDokanVCB Vcb) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  HANDLE thread = NULL;
  Vcb->FcbGarbageCollectorThread = NULL;
  if (Vcb->Dcb->FcbGarbageCollectionIntervalMs == 0) {
    return;
  }
  status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                (PKSTART_ROUTINE)FcbGarbageCollectorThread,
                                Vcb);
  if (!NT_SUCCESS(status)) {
    // Note: we will revert to shared_ptr-style deletion if the thread is NULL.
    return;
  }
  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode,
                            (PVOID *)&Vcb->FcbGarbageCollectorThread, NULL);

  ZwClose(thread);
}

PUNICODE_STRING DokanAllocDuplicateString(__in const UNICODE_STRING* Src) {
  PUNICODE_STRING result = DokanAllocZero(sizeof(UNICODE_STRING));
  if (!result) {
    return NULL;
  }
  if (!DokanDuplicateUnicodeString(result, Src)) {
    ExFreePool(result);
    return NULL;
  }
  return result;
}

BOOLEAN DokanDuplicateUnicodeString(__out UNICODE_STRING* Dest,
                                    __in const UNICODE_STRING* Src) {
  if (Dest->Buffer) {
    ExFreePool(Dest->Buffer);
  }
  Dest->Buffer = DokanAlloc(Src->MaximumLength);
  if (!Dest->Buffer) {
    Dest->Length = 0;
    Dest->MaximumLength = 0;
    return FALSE;
  }
  Dest->MaximumLength = Src->MaximumLength;
  Dest->Length = Src->Length;
  RtlCopyMemory(Dest->Buffer, Src->Buffer, Dest->MaximumLength);
  return TRUE;
}

BOOLEAN StartsWith(__in const UNICODE_STRING* Str,
                   __in const UNICODE_STRING* Prefix) {
  if (Prefix == NULL || Prefix->Length == 0) {
    return TRUE;
  }

  if (Str == NULL || Prefix->Length > Str->Length) {
    return FALSE;
  }

  LPCWSTR prefixToUse, stringToCompareTo;
  prefixToUse = Prefix->Buffer;
  stringToCompareTo = Str->Buffer;

  while (*prefixToUse) {
    if (*prefixToUse++ != *stringToCompareTo++)
      return FALSE;
  }

  return TRUE;
}

BOOLEAN StartsWithDosDevicesPrefix(__in const UNICODE_STRING* Str) {
  return StartsWith(Str, &dosDevicesPrefix);
}

BOOLEAN IsMountPointDriveLetter(__in const UNICODE_STRING* MountPoint) {
  size_t colonIndex = dosDevicesPrefix.Length / sizeof(WCHAR) + 1;
  size_t driveLetterLength = dosDevicesPrefix.Length + 2 * sizeof(WCHAR);
  BOOLEAN nonTerminatedDriveLetterLength =
      MountPoint->Length == driveLetterLength;
  BOOLEAN nullTerminatedDriveLetterLength =
      MountPoint->Length == driveLetterLength + sizeof(WCHAR)
      && MountPoint->Buffer[colonIndex + 1] == L'\0';
  // Note: the size range is for an optional null char.
  return StartsWithDosDevicesPrefix(MountPoint)
      && (nonTerminatedDriveLetterLength || nullTerminatedDriveLetterLength)
      && MountPoint->Buffer[colonIndex] == L':';
}
