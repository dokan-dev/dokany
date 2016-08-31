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

#include "dokan.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, DokanUnload)
#endif

ULONG g_Debug = DOKAN_DEBUG_DEFAULT;
LOOKASIDE_LIST_EX g_DokanCCBLookasideList;
LOOKASIDE_LIST_EX g_DokanFCBLookasideList;

#if _WIN32_WINNT < 0x0501
PFN_FSRTLTEARDOWNPERSTREAMCONTEXTS DokanFsRtlTeardownPerStreamContexts;
#endif

NPAGED_LOOKASIDE_LIST DokanIrpEntryLookasideList;
UNICODE_STRING FcbFileNameNull;

FAST_IO_CHECK_IF_POSSIBLE DokanFastIoCheckIfPossible;

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
  PFSRTL_ADVANCED_FCB_HEADER header;

  header = FileObject->FsContext;
  if (header && header->Resource) {
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(header->Resource, TRUE);
    KeLeaveCriticalRegion();
  }

  DDbgPrint("DokanAcquireForCreateSection\n");
}

FAST_IO_RELEASE_FILE DokanReleaseForCreateSection;
VOID DokanReleaseForCreateSection(__in PFILE_OBJECT FileObject) {
  PFSRTL_ADVANCED_FCB_HEADER header;

  header = FileObject->FsContext;
  if (header && header->Resource) {
    KeEnterCriticalRegion();
    ExReleaseResourceLite(header->Resource);
    KeLeaveCriticalRegion();
  }

  DDbgPrint("DokanReleaseForCreateSection\n");
}

NTSTATUS
DokanFilterCallbackAcquireForCreateSection(__in PFS_FILTER_CALLBACK_DATA
                                               CallbackData,
                                           __out PVOID *CompletionContext) {
  PFSRTL_ADVANCED_FCB_HEADER header;

  UNREFERENCED_PARAMETER(CompletionContext);

  DDbgPrint("DokanFilterCallbackAcquireForCreateSection\n");

  header = CallbackData->FileObject->FsContext;

  if (header && header->Resource) {
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(header->Resource, TRUE);
    KeLeaveCriticalRegion();
  }

  if (CallbackData->Parameters.AcquireForSectionSynchronization.SyncType !=
      SyncTypeCreateSection) {
    return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
  } else {
    return STATUS_FILE_LOCKED_WITH_WRITERS;
  }
}

BOOLEAN
DokanLookasideCreate(LOOKASIDE_LIST_EX *pCache, size_t cbElement) {
#if _WIN32_WINNT > 0x601
  NTSTATUS Status = ExInitializeLookasideListEx(
      pCache, NULL, NULL, NonPagedPoolNx, 0, cbElement, TAG, 0);
#else
  NTSTATUS Status = ExInitializeLookasideListEx(
      pCache, NULL, NULL, NonPagedPool, 0, cbElement, TAG, 0);
#endif

  if (!NT_SUCCESS(Status)) {
    DDbgPrint("ExInitializeLookasideListEx failed, Status (0x%x)", Status);
    return FALSE;
  }

  return TRUE;
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
  PFAST_IO_DISPATCH fastIoDispatch;
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

  fastIoDispatch = ExAllocatePool(sizeof(FAST_IO_DISPATCH));
  if (!fastIoDispatch) {
    IoDeleteDevice(dokanGlobal->FsDiskDeviceObject);
    IoDeleteDevice(dokanGlobal->FsCdDeviceObject);
    IoDeleteDevice(dokanGlobal->DeviceObject);
    DDbgPrint("  ExAllocatePool failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlZeroMemory(fastIoDispatch, sizeof(FAST_IO_DISPATCH));

  fastIoDispatch->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
  fastIoDispatch->FastIoCheckIfPossible = DokanFastIoCheckIfPossible;
  // fastIoDispatch->FastIoRead = DokanFastIoRead;
  fastIoDispatch->FastIoRead = FsRtlCopyRead;
  fastIoDispatch->FastIoWrite = FsRtlCopyWrite;
  fastIoDispatch->AcquireFileForNtCreateSection = DokanAcquireForCreateSection;
  fastIoDispatch->ReleaseFileForNtCreateSection = DokanReleaseForCreateSection;
  fastIoDispatch->MdlRead = FsRtlMdlReadDev;
  fastIoDispatch->MdlReadComplete = FsRtlMdlReadCompleteDev;
  fastIoDispatch->PrepareMdlWrite = FsRtlPrepareMdlWriteDev;
  fastIoDispatch->MdlWriteComplete = FsRtlMdlWriteCompleteDev;

  DriverObject->FastIoDispatch = fastIoDispatch;
#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
  ExInitializeNPagedLookasideList(&DokanIrpEntryLookasideList, NULL, NULL,
                                  POOL_NX_ALLOCATION, sizeof(IRP_ENTRY), TAG,
                                  0);
#else
  ExInitializeNPagedLookasideList(&DokanIrpEntryLookasideList, NULL, NULL, 0,
                                  sizeof(IRP_ENTRY), TAG, 0);
#endif

#if _WIN32_WINNT < 0x0501
  RtlInitUnicodeString(&functionName, L"FsRtlTeardownPerStreamContexts");
  DokanFsRtlTeardownPerStreamContexts =
      MmGetSystemRoutineAddress(&functionName);
#endif

  RtlZeroMemory(&filterCallbacks, sizeof(FS_FILTER_CALLBACKS));

  // only be used by filter driver?
  filterCallbacks.SizeOfFsFilterCallbacks = sizeof(FS_FILTER_CALLBACKS);
  filterCallbacks.PreAcquireForSectionSynchronization =
      DokanFilterCallbackAcquireForCreateSection;

  status =
      FsRtlRegisterFileSystemFilterCallbacks(DriverObject, &filterCallbacks);

  if (!NT_SUCCESS(status)) {
    IoDeleteDevice(dokanGlobal->FsDiskDeviceObject);
    IoDeleteDevice(dokanGlobal->FsCdDeviceObject);
    IoDeleteDevice(dokanGlobal->DeviceObject);
    DDbgPrint("  FsRtlRegisterFileSystemFilterCallbacks returned 0x%x\n",
              status);
    return status;
  }

  if (!DokanLookasideCreate(&g_DokanCCBLookasideList, sizeof(DokanCCB))) {
    IoDeleteDevice(dokanGlobal->FsDiskDeviceObject);
    IoDeleteDevice(dokanGlobal->FsCdDeviceObject);
    IoDeleteDevice(dokanGlobal->DeviceObject);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (!DokanLookasideCreate(&g_DokanFCBLookasideList, sizeof(DokanFCB))) {
    IoDeleteDevice(dokanGlobal->FsDiskDeviceObject);
    IoDeleteDevice(dokanGlobal->FsCdDeviceObject);
    IoDeleteDevice(dokanGlobal->DeviceObject);
    ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

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
  WCHAR symbolicLinkBuf[] = DOKAN_GLOBAL_SYMBOLIC_LINK_NAME;
  UNICODE_STRING symbolicLinkName;
  PDOKAN_GLOBAL dokanGlobal;

  // PAGED_CODE();
  DDbgPrint("==> DokanUnload\n");

  dokanGlobal = deviceObject->DeviceExtension;
  if (GetIdentifierType(dokanGlobal) == DGL) {
    DDbgPrint("  Delete Global DeviceObject\n");

    KeSetEvent(&dokanGlobal->KillDeleteDeviceEvent, 0, FALSE);
    RtlInitUnicodeString(&symbolicLinkName, symbolicLinkBuf);
    IoDeleteSymbolicLink(&symbolicLinkName);

    IoUnregisterFileSystem(dokanGlobal->FsDiskDeviceObject);
    IoUnregisterFileSystem(dokanGlobal->FsCdDeviceObject);

    IoDeleteDevice(dokanGlobal->FsDiskDeviceObject);
    IoDeleteDevice(dokanGlobal->FsCdDeviceObject);
    IoDeleteDevice(deviceObject);
  }

  ExDeleteNPagedLookasideList(&DokanIrpEntryLookasideList);

  ExDeleteLookasideListEx(&g_DokanCCBLookasideList);
  ExDeleteLookasideListEx(&g_DokanFCBLookasideList);

  DDbgPrint("<== DokanUnload\n");
  return;
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
  return;
}

#define PrintStatus(val, flag)                                                 \
  if (val == flag)                                                             \
  DDbgPrint("  status = " #flag "\n")

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

VOID DokanNotifyReportChange0(__in PDokanFCB Fcb, __in PUNICODE_STRING FileName,
                              __in ULONG FilterMatch, __in ULONG Action) {
  USHORT nameOffset;

  DDbgPrint("==> DokanNotifyReportChange %wZ\n", FileName);

  ASSERT(Fcb != NULL);
  ASSERT(FileName != NULL);

  // search the last "\"
  nameOffset = (USHORT)(FileName->Length / sizeof(WCHAR) - 1);
  for (; FileName->Buffer[nameOffset] != L'\\'; --nameOffset)
    ;
  nameOffset++; // the next is the begining of filename

  nameOffset *= sizeof(WCHAR); // Offset is in bytes

  FsRtlNotifyFullReportChange(Fcb->Vcb->NotifySync, &Fcb->Vcb->DirNotifyList,
                              (PSTRING)FileName, nameOffset,
                              NULL, // StreamName
                              NULL, // NormalizedParentName
                              FilterMatch, Action,
                              NULL); // TargetContext

  DDbgPrint("<== DokanNotifyReportChange\n");
}

// DokanNotifyReportChange should be called with the Fcb at least share-locked.
// due to the ro access to the FileName field.
VOID DokanNotifyReportChange(__in PDokanFCB Fcb, __in ULONG FilterMatch,
                             __in ULONG Action) {
  ASSERT(Fcb != NULL);
  DokanNotifyReportChange0(Fcb, &Fcb->FileName, FilterMatch, Action);
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

  if (Ccb == NULL || Ccb == 0) {
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