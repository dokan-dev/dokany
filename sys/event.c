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

#include "dokan.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanOplockComplete)
#endif

VOID DokanIrpCancelRoutine(_Inout_ PDEVICE_OBJECT DeviceObject,
                           _Inout_ _IRQL_uses_cancel_ PIRP Irp) {
  KIRQL oldIrql;
  PIRP_ENTRY irpEntry;
  ULONG serialNumber = 0;
  PIO_STACK_LOCATION irpSp;

  UNREFERENCED_PARAMETER(DeviceObject);

  DDbgPrint("==> DokanIrpCancelRoutine\n");

  // Release the cancel spinlock
  IoReleaseCancelSpinLock(Irp->CancelIrql);

  irpEntry = Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY];

  if (irpEntry != NULL) {
    PKSPIN_LOCK lock = &irpEntry->IrpList->ListLock;

    // Acquire the queue spinlock
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    KeAcquireSpinLock(lock, &oldIrql);

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    ASSERT(irpSp != NULL);

    serialNumber = irpEntry->SerialNumber;

    RemoveEntryList(&irpEntry->ListEntry);
    InitializeListHead(&irpEntry->ListEntry);

    // If Write is canceld before completion and buffer that saves writing
    // content is not freed, free it here
    if (irpSp->MajorFunction == IRP_MJ_WRITE) {
      PVOID eventContext =
          Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT];
      if (eventContext != NULL) {
        DokanFreeEventContext(eventContext);
      }
      Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = NULL;
    }

    if (IsListEmpty(&irpEntry->IrpList->ListHead)) {
      // DDbgPrint("    list is empty ClearEvent\n");
      KeClearEvent(&irpEntry->IrpList->NotEmpty);
    }

    irpEntry->Irp = NULL;

    if (irpEntry->CancelRoutineFreeMemory == FALSE) {
      InitializeListHead(&irpEntry->ListEntry);
    } else {
      DokanFreeIrpEntry(irpEntry);
      irpEntry = NULL;
    }

    Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;

    KeReleaseSpinLock(lock, oldIrql);
  }

  DDbgPrint("   canceled IRP #%X\n", serialNumber);
  DokanCompleteIrpRequest(Irp, STATUS_CANCELLED, 0);

  DDbgPrint("<== DokanIrpCancelRoutine\n");
}

VOID DokanOplockComplete(IN PVOID Context, IN PIRP Irp)
/*++
Routine Description:
This routine is called by the oplock package when an oplock break has
completed, allowing an Irp to resume execution.  If the status in
the Irp is STATUS_SUCCESS, then we queue the Irp to the Fsp queue.
Otherwise we complete the Irp with the status in the Irp.
Arguments:
Context - Pointer to the EventContext to be queued to the Fsp
Irp - I/O Request Packet.
Return Value:
None.
--*/
{
  PIO_STACK_LOCATION irpSp;

  DDbgPrint("==> DokanOplockComplete\n");
  PAGED_CODE();

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  //
  //  Check on the return value in the Irp.
  //
  if (Irp->IoStatus.Status == STATUS_SUCCESS) {
    DokanRegisterPendingIrp(irpSp->DeviceObject, Irp, (PEVENT_CONTEXT)Context,
                            0);
  } else {
    DokanCompleteIrpRequest(Irp, Irp->IoStatus.Status, 0);
  }

  DDbgPrint("<== DokanOplockComplete\n");
}

VOID DokanPrePostIrp(IN PVOID Context, IN PIRP Irp)
/*++
Routine Description:
This routine performs any necessary work before STATUS_PENDING is
returned with the Fsd thread.  This routine is called within the
filesystem and by the oplock package.
Arguments:
Context - Pointer to the EventContext to be queued to the Fsp
Irp - I/O Request Packet.
Return Value:
None.
--*/
{
  DDbgPrint("==> DokanPrePostIrp\n");

  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Irp);

  DDbgPrint("<== DokanPrePostIrp\n");
}

NTSTATUS
RegisterPendingIrpMain(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp,
                       __in ULONG SerialNumber, __in PIRP_LIST IrpList,
                       __in ULONG Flags, __in ULONG CheckMount) {
  PIRP_ENTRY irpEntry;
  PIO_STACK_LOCATION irpSp;
  KIRQL oldIrql;
  PDokanVCB vcb = NULL;

  DDbgPrint("==> DokanRegisterPendingIrpMain\n");

  if (GetIdentifierType(DeviceObject->DeviceExtension) == VCB) {
    vcb = DeviceObject->DeviceExtension;
    if (CheckMount && IsUnmountPendingVcb(vcb)) {
      DDbgPrint(" device is not mounted\n");
      return STATUS_NO_SUCH_DEVICE;
    }
  }

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  // Allocate a record and save all the event context.
  irpEntry = DokanAllocateIrpEntry();

  if (NULL == irpEntry) {
    DDbgPrint("  can't allocate IRP_ENTRY\n");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlZeroMemory(irpEntry, sizeof(IRP_ENTRY));

  InitializeListHead(&irpEntry->ListEntry);

  irpEntry->SerialNumber = SerialNumber;
  irpEntry->FileObject = irpSp->FileObject;
  irpEntry->Irp = Irp;
  irpEntry->IrpSp = irpSp;
  irpEntry->IrpList = IrpList;
  irpEntry->Flags = Flags;

  // Update the irp timeout for the entry
  if (vcb) {
    ExAcquireResourceExclusiveLite(&vcb->Dcb->Resource, TRUE);
    DokanUpdateTimeout(&irpEntry->TickCount, vcb->Dcb->IrpTimeout);
    ExReleaseResourceLite(&vcb->Dcb->Resource);
  } else {
    DokanUpdateTimeout(&irpEntry->TickCount, DOKAN_IRP_PENDING_TIMEOUT);
  }

  // DDbgPrint("  Lock IrpList.ListLock\n");
  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&IrpList->ListLock, &oldIrql);

  IoSetCancelRoutine(Irp, DokanIrpCancelRoutine);

  if (Irp->Cancel) {
    if (IoSetCancelRoutine(Irp, NULL) != NULL) {
      // DDbgPrint("  Release IrpList.ListLock %d\n", __LINE__);
      KeReleaseSpinLock(&IrpList->ListLock, oldIrql);

      DokanFreeIrpEntry(irpEntry);

      return STATUS_CANCELLED;
    }
  }

  IoMarkIrpPending(Irp);

  InsertTailList(&IrpList->ListHead, &irpEntry->ListEntry);

  irpEntry->CancelRoutineFreeMemory = FALSE;

  // save the pointer in order to be accessed by cancel routine
  Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = irpEntry;

  KeSetEvent(&IrpList->NotEmpty, IO_NO_INCREMENT, FALSE);

  // DDbgPrint("  Release IrpList.ListLock\n");
  KeReleaseSpinLock(&IrpList->ListLock, oldIrql);

  DDbgPrint("<== DokanRegisterPendingIrpMain\n");
  return STATUS_PENDING;
}

NTSTATUS
DokanRegisterPendingIrp(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp,
                        __in PEVENT_CONTEXT EventContext, __in ULONG Flags) {
  PDokanVCB vcb = DeviceObject->DeviceExtension;
  NTSTATUS status;

  DDbgPrint("==> DokanRegisterPendingIrp\n");

  if (GetIdentifierType(vcb) != VCB) {
    DDbgPrint("  IdentifierType is not VCB\n");
    return STATUS_INVALID_PARAMETER;
  }

  status = RegisterPendingIrpMain(DeviceObject, Irp, EventContext->SerialNumber,
                                  &vcb->Dcb->PendingIrp, Flags, TRUE);

  if (status == STATUS_PENDING) {
    DokanEventNotification(&vcb->Dcb->NotifyEvent, EventContext);
  } else {
    DokanFreeEventContext(EventContext);
  }

  DDbgPrint("<== DokanRegisterPendingIrp\n");
  return status;
}

NTSTATUS
DokanRegisterPendingIrpForEvent(__in PDEVICE_OBJECT DeviceObject,
                                _Inout_ PIRP Irp) {
  PDokanVCB vcb = DeviceObject->DeviceExtension;

  if (GetIdentifierType(vcb) != VCB) {
    DDbgPrint("  IdentifierType is not VCB\n");
    return STATUS_INVALID_PARAMETER;
  }

  if (IsUnmountPendingVcb(vcb)) {
    DDbgPrint("  Volume is dismounted\n");
    return STATUS_NO_SUCH_DEVICE;
  }

  // DDbgPrint("DokanRegisterPendingIrpForEvent\n");
  vcb->HasEventWait = TRUE;

  return RegisterPendingIrpMain(DeviceObject, Irp,
                                0, // SerialNumber
                                &vcb->Dcb->PendingEvent,
                                0, // Flags
                                TRUE);
}

NTSTATUS
DokanRegisterPendingIrpForService(__in PDEVICE_OBJECT DeviceObject,
                                  _Inout_ PIRP Irp) {
  PDOKAN_GLOBAL dokanGlobal;
  DDbgPrint("DokanRegisterPendingIrpForService\n");

  dokanGlobal = DeviceObject->DeviceExtension;
  if (GetIdentifierType(dokanGlobal) != DGL) {
    return STATUS_INVALID_PARAMETER;
  }

  return RegisterPendingIrpMain(DeviceObject, Irp,
                                0, // SerialNumber
                                &dokanGlobal->PendingService,
                                0, // Flags
                                FALSE);
}

NTSTATUS
DokanCompleteDispatch(__in PIRP_ENTRY IrpEntry,
                      __in PEVENT_INFORMATION EventInfo,
                      __in PDEVICE_OBJECT DeviceObject, __in BOOLEAN Wait) {
  NTSTATUS ret;

  switch (IrpEntry->IrpSp->MajorFunction) {
  case IRP_MJ_DIRECTORY_CONTROL:
    ret = DokanCompleteDirectoryControl(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_READ:
    ret = DokanCompleteRead(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_WRITE:
    ret = DokanCompleteWrite(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_QUERY_INFORMATION:
    ret = DokanCompleteQueryInformation(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_QUERY_VOLUME_INFORMATION:
    ret = DokanCompleteQueryVolumeInformation(IrpEntry, EventInfo, DeviceObject,
                                              Wait);
    break;
  case IRP_MJ_CREATE:
    ret = DokanCompleteCreate(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_CLEANUP:
    ret = DokanCompleteCleanup(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_LOCK_CONTROL:
    ret = DokanCompleteLock(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_SET_INFORMATION:
    ret = DokanCompleteSetInformation(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_FLUSH_BUFFERS:
    ret = DokanCompleteFlush(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_QUERY_SECURITY:
    ret = DokanCompleteQuerySecurity(IrpEntry, EventInfo, Wait);
    break;
  case IRP_MJ_SET_SECURITY:
    ret = DokanCompleteSetSecurity(IrpEntry, EventInfo, Wait);
    break;
  default:
    DDbgPrint("Unknown IRP %d\n", IrpEntry->IrpSp->MajorFunction);
    ret = STATUS_SUCCESS;
    // TODO: in this case, should complete this IRP
    break;
  }

  return ret;
}

typedef struct _WORK_CONTEXT {
  PIRP_ENTRY IrpEntry;
  PEVENT_INFORMATION EventInfo;
  PDEVICE_OBJECT DeviceObject;
} WORK_CONTEXT, *PWORK_CONTEXT;

VOID DokanDispathWork(PVOID Context) {
  PWORK_CONTEXT ctx = (PWORK_CONTEXT)Context;

  DokanCompleteDispatch(ctx->IrpEntry, ctx->EventInfo, ctx->DeviceObject, TRUE);

  DokanFreeIrpEntry(ctx->IrpEntry);
  ExFreePool(ctx->EventInfo);
  ExFreePool(ctx);
}

PWORK_CONTEXT
AllocateWorkContext(__in PIRP_ENTRY IrpEntry, __in PEVENT_INFORMATION EventInfo,
                    __in ULONG SizeOfEventInfo,
                    __in PDEVICE_OBJECT DeviceObject) {
  PWORK_CONTEXT context;

  context = ExAllocatePool(sizeof(WORK_CONTEXT));
  if (context == NULL) {
    return NULL;
  }

  context->EventInfo = ExAllocatePool(SizeOfEventInfo);
  if (context->EventInfo == NULL) {
    ExFreePool(context);
    return NULL;
  }

  context->IrpEntry = IrpEntry;
  context->DeviceObject = DeviceObject;
  RtlCopyMemory(context->EventInfo, EventInfo, SizeOfEventInfo);

  return context;
}

VOID DokanAddToWorkque(__in PIRP_ENTRY IrpEntry,
                       __in PEVENT_INFORMATION EventInfo,
                       __in ULONG SizeOfEventInfo,
                       __in PDEVICE_OBJECT DeviceObject) {
  PWORK_CONTEXT context;

  context =
      AllocateWorkContext(IrpEntry, EventInfo, SizeOfEventInfo, DeviceObject);
  if (context == NULL) {
    DokanCompleteDispatch(IrpEntry, EventInfo, DeviceObject, TRUE);
    return;
  }

  ExInitializeWorkItem(&IrpEntry->WorkQueueItem, DokanDispathWork, context);
  ExQueueWorkItem(&IrpEntry->WorkQueueItem, CriticalWorkQueue);
}
// When user-mode file system application returns EventInformation,
// search corresponding pending IRP and complete it
NTSTATUS
DokanCompleteIrp(__in PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp) {
  KIRQL oldIrql;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry;
  PDokanVCB vcb;
  PEVENT_INFORMATION eventInfo;
  ULONG sizeOfEventInfo;

  sizeOfEventInfo = IoGetCurrentIrpStackLocation(Irp)
                        ->Parameters.DeviceIoControl.InputBufferLength;
  eventInfo = (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
  ASSERT(eventInfo != NULL);

  // DDbgPrint("==> DokanCompleteIrp [EventInfo #%X]\n",
  // eventInfo->SerialNumber);

  vcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(vcb) != VCB) {
    return STATUS_INVALID_PARAMETER;
  }

  if (IsUnmountPendingVcb(vcb)) {
    DDbgPrint("      Volume is not mounted\n");
    return STATUS_NO_SUCH_DEVICE;
  }

  // DDbgPrint("      Lock IrpList.ListLock\n");
  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&vcb->Dcb->PendingIrp.ListLock, &oldIrql);

  // search corresponding IRP through pending IRP list
  listHead = &vcb->Dcb->PendingIrp.ListHead;

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {

    PIRP irp;
    PIO_STACK_LOCATION irpSp;
    INT ret = STATUS_SUCCESS;

    nextEntry = thisEntry->Flink;

    irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

    // check whether this is corresponding IRP

    // DDbgPrint("SerialNumber irpEntry %X eventInfo %X\n",
    // irpEntry->SerialNumber, eventInfo->SerialNumber);

    // this irpEntry must be freed in this if statement
    if (irpEntry->SerialNumber != eventInfo->SerialNumber) {
      continue;
    }

    RemoveEntryList(thisEntry);

    irp = irpEntry->Irp;

    if (irp == NULL) {
      // this IRP is already canceled
      ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
      DokanFreeIrpEntry(irpEntry);
      irpEntry = NULL;
      break;
    }

    if (IoSetCancelRoutine(irp, NULL) == NULL) {
      // Cancel routine will run as soon as we release the lock
      InitializeListHead(&irpEntry->ListEntry);
      irpEntry->CancelRoutineFreeMemory = TRUE;
      break;
    }

    // IRP is not canceled yet
    irpSp = irpEntry->IrpSp;

    ASSERT(irpSp != NULL);

    // IrpEntry is saved here for CancelRoutine
    // Clear it to prevent to be completed by CancelRoutine twice
    irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;
    KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

    if (IsUnmountPendingVcb(vcb)) {
      DDbgPrint("      Volume is not mounted second check\n");
      return STATUS_NO_SUCH_DEVICE;
    }

    if (eventInfo->Status == STATUS_PENDING) {
      DDbgPrint(
          "      !!WARNING!! Do not return STATUS_PENDING DokanCompleteIrp!");
    }

    ret = DokanCompleteDispatch(irpEntry, eventInfo, DeviceObject, FALSE);

    if (STATUS_SUCCESS == ret) {
      DokanFreeIrpEntry(irpEntry);
      irpEntry = NULL;
    } else {
      DokanAddToWorkque(irpEntry, eventInfo, sizeOfEventInfo, DeviceObject);
    }
    return STATUS_SUCCESS;
  }

  KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

  // DDbgPrint("<== AACompleteIrp [EventInfo #%X]\n", eventInfo->SerialNumber);

  // TODO: should return error
  return STATUS_SUCCESS;
}

VOID RemoveSessionDevices(__in PDOKAN_GLOBAL dokanGlobal,
                          __in ULONG sessionId) {
  DDbgPrint("==> RemoveSessionDevices");

  if (sessionId == -1) {
    return;
  }

  PDEVICE_ENTRY foundEntry;

  BOOLEAN isDone = FALSE;
  do {
    foundEntry = FindDeviceForDeleteBySessionId(dokanGlobal, sessionId);
    if (foundEntry != NULL) {
      DeleteMountPointSymbolicLink(&foundEntry->MountPoint);
      foundEntry->SessionId = (ULONG)-1;
      foundEntry->MountPoint.Buffer = NULL;
    } else {
      isDone = TRUE;
    }
  } while (!isDone);

  DDbgPrint("<== RemoveSessionDevices");
}

// start event dispatching
NTSTATUS
DokanEventStart(__in PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp) {
  ULONG outBufferLen;
  ULONG inBufferLen;
  PIO_STACK_LOCATION irpSp = NULL;
  PEVENT_START eventStart = NULL;
  PEVENT_DRIVER_INFO driverInfo = NULL;
  PDOKAN_GLOBAL dokanGlobal = NULL;
  PDokanDCB dcb = NULL;
  NTSTATUS status;
  DEVICE_TYPE deviceType;
  ULONG deviceCharacteristics = 0;
  WCHAR *baseGuidString;
  GUID baseGuid = DOKAN_BASE_GUID;
  UNICODE_STRING unicodeGuid;
  ULONG deviceNamePos;
  BOOLEAN useMountManager = FALSE;
  BOOLEAN mountGlobally = TRUE;
  BOOLEAN fileLockUserMode = FALSE;
  ULONG sessionId = (ULONG)-1;

  DDbgPrint("==> DokanEventStart\n");

  dokanGlobal = DeviceObject->DeviceExtension;
  if (GetIdentifierType(dokanGlobal) != DGL) {
    return STATUS_INVALID_PARAMETER;
  }

  irpSp = IoGetCurrentIrpStackLocation(Irp);

  outBufferLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
  inBufferLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;

  eventStart = ExAllocatePool(sizeof(EVENT_START));
  baseGuidString = ExAllocatePool(64 * sizeof(WCHAR));

  if (outBufferLen != sizeof(EVENT_DRIVER_INFO) ||
      inBufferLen != sizeof(EVENT_START) || eventStart == NULL ||
      baseGuidString == NULL) {
    if (eventStart)
      ExFreePool(eventStart);
    if (baseGuidString)
      ExFreePool(baseGuidString);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  RtlCopyMemory(eventStart, Irp->AssociatedIrp.SystemBuffer,
                sizeof(EVENT_START));
  driverInfo = Irp->AssociatedIrp.SystemBuffer;

  if (eventStart->UserVersion != DOKAN_DRIVER_VERSION) {
    driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;
    driverInfo->Status = DOKAN_START_FAILED;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return STATUS_SUCCESS;
  }

  switch (eventStart->DeviceType) {
  case DOKAN_DISK_FILE_SYSTEM:
    deviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
    break;
  case DOKAN_NETWORK_FILE_SYSTEM:
    deviceType = FILE_DEVICE_NETWORK_FILE_SYSTEM;
    deviceCharacteristics |= FILE_REMOTE_DEVICE;
    break;
  default:
    DDbgPrint("  Unknown device type: %d\n", eventStart->DeviceType);
    deviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
  }

  if (eventStart->Flags & DOKAN_EVENT_REMOVABLE) {
    DDbgPrint("  DeviceCharacteristics |= FILE_REMOVABLE_MEDIA\n");
    deviceCharacteristics |= FILE_REMOVABLE_MEDIA;
  }

  if (eventStart->Flags & DOKAN_EVENT_WRITE_PROTECT) {
    DDbgPrint("  DeviceCharacteristics |= FILE_READ_ONLY_DEVICE\n");
    deviceCharacteristics |= FILE_READ_ONLY_DEVICE;
  }

  if (eventStart->Flags & DOKAN_EVENT_MOUNT_MANAGER) {
    DDbgPrint("  Using Mount Manager\n");
    useMountManager = TRUE;
  }

  if (eventStart->Flags & DOKAN_EVENT_CURRENT_SESSION) {
    DDbgPrint("  Mounting on current session only\n");
    mountGlobally = FALSE;
    sessionId = GetCurrentSessionId(Irp);
  }

  if (eventStart->Flags & DOKAN_EVENT_FILELOCK_USER_MODE) {
    DDbgPrint("  FileLock in User Mode\n");
    fileLockUserMode = TRUE;
  }

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);

  DOKAN_CONTROL dokanControl;
  RtlZeroMemory(&dokanControl, sizeof(dokanControl));
  RtlStringCchCopyW(dokanControl.MountPoint, MAXIMUM_FILENAME_LENGTH,
                    L"\\DosDevices\\");
  if (wcslen(eventStart->MountPoint) == 1) {
    dokanControl.MountPoint[12] = towupper(eventStart->MountPoint[0]);
    dokanControl.MountPoint[13] = L':';
    dokanControl.MountPoint[14] = L'\0';
  } else {
    RtlStringCchCatW(dokanControl.MountPoint, MAXIMUM_FILENAME_LENGTH,
                     eventStart->MountPoint);
  }
  dokanControl.SessionId = sessionId;

  DDbgPrint("  Checking for MountPoint %ls \n", dokanControl.MountPoint);
  PMOUNT_ENTRY foundEntry = FindMountEntry(dokanGlobal, &dokanControl, FALSE);
  if (foundEntry != NULL) {
    DDbgPrint("  MountPoint exists already %ls \n", dokanControl.MountPoint);
    driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;
    driverInfo->Status = DOKAN_START_FAILED;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
    ExReleaseResourceLite(&dokanGlobal->Resource);
    KeLeaveCriticalRegion();
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return STATUS_SUCCESS;
  }

  baseGuid.Data2 = (USHORT)(dokanGlobal->MountId & 0xFFFF) ^ baseGuid.Data2;
  baseGuid.Data3 = (USHORT)(dokanGlobal->MountId >> 16) ^ baseGuid.Data3;

  status = RtlStringFromGUID(&baseGuid, &unicodeGuid);
  if (!NT_SUCCESS(status)) {
    ExReleaseResourceLite(&dokanGlobal->Resource);
    KeLeaveCriticalRegion();
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return status;
  }
  RtlZeroMemory(baseGuidString, 64 * sizeof(WCHAR));
  RtlStringCchCopyW(baseGuidString, 64, unicodeGuid.Buffer);
  RtlFreeUnicodeString(&unicodeGuid);

  InterlockedIncrement((LONG *)&dokanGlobal->MountId);

  status = DokanCreateDiskDevice(
      DeviceObject->DriverObject, dokanGlobal->MountId, eventStart->MountPoint,
      eventStart->UNCName, sessionId, baseGuidString, dokanGlobal, deviceType,
      deviceCharacteristics, mountGlobally, useMountManager, &dcb);

  if (!NT_SUCCESS(status)) {
    ExReleaseResourceLite(&dokanGlobal->Resource);
    KeLeaveCriticalRegion();
    ExFreePool(eventStart);
    ExFreePool(baseGuidString);
    return status;
  }

  dcb->FileLockInUserMode = fileLockUserMode;

  DDbgPrint("  MountId:%d\n", dcb->MountId);
  driverInfo->DeviceNumber = dokanGlobal->MountId;
  driverInfo->MountId = dokanGlobal->MountId;
  driverInfo->Status = DOKAN_MOUNTED;
  driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;

  // SymbolicName is
  // \\DosDevices\\Global\\Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
  // Finds the last '\' and copy into DeviceName.
  // DeviceName is \Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
  deviceNamePos = dcb->SymbolicLinkName->Length / sizeof(WCHAR) - 1;
  deviceNamePos = DokanSearchWcharinUnicodeStringWithUlong(
      dcb->SymbolicLinkName, L'\\', deviceNamePos, 0);

  RtlStringCchCopyW(driverInfo->DeviceName,
                    sizeof(driverInfo->DeviceName) / sizeof(WCHAR),
                    &(dcb->SymbolicLinkName->Buffer[deviceNamePos]));

  // Set the irp timeout in milliseconds
  // If the IrpTimeout is 0, we assume that the value was not changed
  dcb->IrpTimeout = DOKAN_IRP_PENDING_TIMEOUT;
  if (eventStart->IrpTimeout > 0) {
    if (eventStart->IrpTimeout > DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX) {
      eventStart->IrpTimeout = DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX;
    }

    if (eventStart->IrpTimeout < DOKAN_IRP_PENDING_TIMEOUT) {
      eventStart->IrpTimeout = DOKAN_IRP_PENDING_TIMEOUT;
    }
    dcb->IrpTimeout = eventStart->IrpTimeout;
  }

  DDbgPrint("  DeviceName:%ws\n", driverInfo->DeviceName);

  dcb->UseAltStream = 0;
  if (eventStart->Flags & DOKAN_EVENT_ALTERNATIVE_STREAM_ON) {
    DDbgPrint("  ALT_STREAM_ON\n");
    dcb->UseAltStream = 1;
  }

  DokanStartEventNotificationThread(dcb);

  ExReleaseResourceLite(&dokanGlobal->Resource);
  KeLeaveCriticalRegion();

  IoVerifyVolume(dcb->DeviceObject, FALSE);

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);

  ExFreePool(eventStart);
  ExFreePool(baseGuidString);

  DDbgPrint("<== DokanEventStart\n");

  return Irp->IoStatus.Status;
}

// user assinged bigger buffer that is enough to return WriteEventContext
NTSTATUS
DokanEventWrite(__in PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp) {
  KIRQL oldIrql;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry;
  PDokanVCB vcb;
  PEVENT_INFORMATION eventInfo;
  PIRP writeIrp;

  eventInfo = (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
  ASSERT(eventInfo != NULL);

  DDbgPrint("==> DokanEventWrite [EventInfo #%X]\n", eventInfo->SerialNumber);

  vcb = DeviceObject->DeviceExtension;

  if (GetIdentifierType(vcb) != VCB) {
    return STATUS_INVALID_PARAMETER;
  }

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&vcb->Dcb->PendingIrp.ListLock, &oldIrql);

  // search corresponding write IRP through pending IRP list
  listHead = &vcb->Dcb->PendingIrp.ListHead;

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {

    PIO_STACK_LOCATION writeIrpSp, eventIrpSp;
    PEVENT_CONTEXT eventContext;
    ULONG info = 0;
    NTSTATUS status;

    nextEntry = thisEntry->Flink;
    irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

    // check whehter this is corresponding IRP

    // DDbgPrint("SerialNumber irpEntry %X eventInfo %X\n",
    // irpEntry->SerialNumber, eventInfo->SerialNumber);

    if (irpEntry->SerialNumber != eventInfo->SerialNumber) {
      continue;
    }

    // do NOT free irpEntry here
    writeIrp = irpEntry->Irp;
    if (writeIrp == NULL) {
      // this IRP has already been canceled
      ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
      DokanFreeIrpEntry(irpEntry);
      continue;
    }

    if (IoSetCancelRoutine(writeIrp, DokanIrpCancelRoutine) == NULL) {
      // if (IoSetCancelRoutine(writeIrp, NULL) != NULL) {
      // Cancel routine will run as soon as we release the lock
      InitializeListHead(&irpEntry->ListEntry);
      irpEntry->CancelRoutineFreeMemory = TRUE;
      continue;
    }

    writeIrpSp = irpEntry->IrpSp;
    eventIrpSp = IoGetCurrentIrpStackLocation(Irp);

    ASSERT(writeIrpSp != NULL);
    ASSERT(eventIrpSp != NULL);

    eventContext =
        (PEVENT_CONTEXT)
            writeIrp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT];
    ASSERT(eventContext != NULL);

    // short of buffer length
    if (eventIrpSp->Parameters.DeviceIoControl.OutputBufferLength <
        eventContext->Length) {
      DDbgPrint("  EventWrite: STATUS_INSUFFICIENT_RESOURCE\n");
      status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
      PVOID buffer;
      // DDbgPrint("  EventWrite CopyMemory\n");
      // DDbgPrint("  EventLength %d, BufLength %d\n", eventContext->Length,
      //            eventIrpSp->Parameters.DeviceIoControl.OutputBufferLength);
      if (Irp->MdlAddress)
        buffer = MmGetSystemAddressForMdlNormalSafe(Irp->MdlAddress);
      else
        buffer = Irp->AssociatedIrp.SystemBuffer;

      ASSERT(buffer != NULL);
      RtlCopyMemory(buffer, eventContext, eventContext->Length);

      info = eventContext->Length;
      status = STATUS_SUCCESS;
    }

    DokanFreeEventContext(eventContext);
    writeIrp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;

    KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;

    // this IRP will be completed by caller function
    return Irp->IoStatus.Status;
  }

  KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

  // if the corresponding IRP not found, the user should already canceled the operation and the IRP already destroyed.
  DDbgPrint("  EventWrite : Cannot found corresponding IRP. User should "
            "already canceled the operation. Return STATUS_CANCELLED.");

  return STATUS_CANCELLED;
}
