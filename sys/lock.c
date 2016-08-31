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

NTSTATUS
DokanCommonLockControl(__in PIRP Irp) {
  NTSTATUS Status = STATUS_SUCCESS;

  PDokanFCB Fcb;
  PDokanCCB Ccb;
  PFILE_OBJECT fileObject;

  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

  DDbgPrint("==> DokanCommonLockControl\n");

  PAGED_CODE();

  fileObject = irpSp->FileObject;
  DokanPrintFileName(fileObject);

  Ccb = fileObject->FsContext2;
  if (Ccb == NULL || Ccb->Identifier.Type != CCB) {
    DDbgPrint("    DokanOplockRequest STATUS_INVALID_PARAMETER\n");
    return STATUS_INVALID_PARAMETER;
  }

  Fcb = Ccb->Fcb;
  if (Fcb == NULL || Fcb->Identifier.Type != FCB) {
    DDbgPrint("    DokanOplockRequest STATUS_INVALID_PARAMETER\n");
    return STATUS_INVALID_PARAMETER;
  }
  DokanFCBLockRW(Fcb);

  //
  //  If the file is not a user file open then we reject the request
  //  as an invalid parameter
  //
  if (FlagOn(Fcb->Flags, DOKAN_FILE_DIRECTORY)) {
    DDbgPrint("  DokanCommonLockControl -> STATUS_INVALID_PARAMETER\n", 0);
    DokanFCBUnlock(Fcb);
    return STATUS_INVALID_PARAMETER;
  }

  try {

//
//  We check whether we can proceed
//  based on the state of the file oplocks.
//
#if (NTDDI_VERSION >= NTDDI_WIN8)

    if (((IRP_MN_LOCK == irpSp->MinorFunction) &&
         ((ULONGLONG)irpSp->Parameters.LockControl.ByteOffset.QuadPart <
          (ULONGLONG)Fcb->AdvancedFCBHeader.AllocationSize.QuadPart)) ||
        ((IRP_MN_LOCK != irpSp->MinorFunction) &&
         FsRtlAreThereWaitingFileLocks(&Fcb->FileLock))) {

//
//  Check whether we can proceed based on the state of file oplocks if doing
//  an operation that interferes with oplocks. Those operations are:
//
//      1. Lock a range within the file's AllocationSize.
//      2. Unlock a range when there are waiting locks on the file. This one
//         is not guaranteed to interfere with oplocks, but it could, as
//         unlocking this range might cause a waiting lock to be granted
//         within AllocationSize!
//
#endif
      // Dokan DokanOplockComplete sends the operation to user mode, which isn't
      // what we want to do
      // so now wait for the oplock to be broken (pass in NULL for the callback)
      Status =
          FsRtlCheckOplock(DokanGetFcbOplock(Fcb), Irp, NULL /* EventContext */,
                           NULL /*DokanOplockComplete*/, NULL);

#if (NTDDI_VERSION >= NTDDI_WIN8)
    }
#endif
    //  If we were waiting for the callback, then STATUS_PENDING would be ok too
    if (Status != STATUS_SUCCESS) {
      __leave;
    }

    //
    //  Now call the FsRtl routine to do the actual processing of the
    //  Lock request
    //
    Status = FsRtlProcessFileLock(&Fcb->FileLock, Irp, NULL);
  } finally {
    DokanFCBUnlock(Fcb);
  }

  DDbgPrint("<== DokanCommonLockControl\n");

  return Status;
}

NTSTATUS
DokanDispatchLock(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  PDokanVCB vcb;
  PDokanDCB dcb;
  PEVENT_CONTEXT eventContext = NULL;
  ULONG eventLength;
  BOOLEAN completeIrp = TRUE;

  __try {
    DDbgPrint("==> DokanLock\n");

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;

    if (fileObject == NULL) {
      DDbgPrint("  fileObject == NULL\n");
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    vcb = DeviceObject->DeviceExtension;
    if (GetIdentifierType(vcb) != VCB ||
        !DokanCheckCCB(vcb->Dcb, fileObject->FsContext2)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }
    dcb = vcb->Dcb;

    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
    DokanPrintFileName(fileObject);

    switch (irpSp->MinorFunction) {
    case IRP_MN_LOCK:
      DDbgPrint("  IRP_MN_LOCK\n");
      break;
    case IRP_MN_UNLOCK_ALL:
      DDbgPrint("  IRP_MN_UNLOCK_ALL\n");
      break;
    case IRP_MN_UNLOCK_ALL_BY_KEY:
      DDbgPrint("  IRP_MN_UNLOCK_ALL_BY_KEY\n");
      break;
    case IRP_MN_UNLOCK_SINGLE:
      DDbgPrint("  IRP_MN_UNLOCK_SINGLE\n");
      break;
    default:
      DDbgPrint("  unknown function : %d\n", irpSp->MinorFunction);
      break;
    }

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);
    DokanFCBLockRW(fcb);

    if (dcb->FileLockInUserMode) {

      eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
      eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

      if (eventContext == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }

      eventContext->Context = ccb->UserContext;
      DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);

      // copy file name to be locked
      eventContext->Operation.Lock.FileNameLength = fcb->FileName.Length;
      RtlCopyMemory(eventContext->Operation.Lock.FileName, fcb->FileName.Buffer,
                    fcb->FileName.Length);

      // parameters of Lock
      eventContext->Operation.Lock.ByteOffset =
          irpSp->Parameters.LockControl.ByteOffset;
      if (irpSp->Parameters.LockControl.Length != NULL) {
        eventContext->Operation.Lock.Length.QuadPart =
            irpSp->Parameters.LockControl.Length->QuadPart;
      } else {
        DDbgPrint("  LockControl.Length = NULL\n");
      }
      eventContext->Operation.Lock.Key = irpSp->Parameters.LockControl.Key;

      // register this IRP to waiting IRP list and make it pending status
      status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);
    } else {
      status = DokanCommonLockControl(Irp);
      completeIrp = FALSE;
    }

  } __finally {
    if(fcb)
      DokanFCBUnlock(fcb);

    if (completeIrp) {
      DokanCompleteIrpRequest(Irp, status, 0);
    }

    DDbgPrint("<== DokanLock\n");
  }

  return status;
}

VOID DokanCompleteLock(__in PIRP_ENTRY IrpEntry,
                       __in PEVENT_INFORMATION EventInfo) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;

  DDbgPrint("==> DokanCompleteLock\n");

  DokanCompleteIrpRequest(irp, EventInfo->Status, 0);

  DDbgPrint("<== DokanCompleteLock\n");
}