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

#include "dokan.h"
#include <wdmsec.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanOplockRequest)
#endif

const WCHAR* DokanGetOplockControlCodeName(ULONG FsControlCode) {
  switch (FsControlCode) {
    case FSCTL_REQUEST_OPLOCK:
      return L"FSCTL_REQUEST_OPLOCK";
    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
      return L"FSCTL_REQUEST_OPLOCK_LEVEL_1";
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
      return L"FSCTL_REQUEST_OPLOCK_LEVEL_2";
    case FSCTL_REQUEST_BATCH_OPLOCK:
      return L"FSCTL_REQUEST_BATCH_OPLOCK";
    case FSCTL_REQUEST_FILTER_OPLOCK:
      return L"FSCTL_REQUEST_FILTER_OPLOCK";
    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
      return L"FSCTL_OPLOCK_BREAK_ACKNOWLEDGE";
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
      return L"FSCTL_OPBATCH_ACK_CLOSE_PENDING";
    case FSCTL_OPLOCK_BREAK_NOTIFY:
      return L"FSCTL_OPLOCK_BREAK_NOTIFY";
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
      return L"FSCTL_OPLOCK_BREAK_ACK_NO_2";
    default:
      return L"<unknown>";
  }
}

void DokanMaybeLogOplockRequest(__in PDOKAN_LOGGER Logger,
                                __in PDokanFCB Fcb,
                                __in ULONG FsControlCode,
                                __in ULONG OplockCount,
                                __in BOOLEAN AcquiredFcb,
                                __in BOOLEAN AcquiredVcb,
                                __in ULONG RequestedLevel,
                                __in ULONG Flags) {
  // These calls log to the fixed-size DokanOlockDebugInfo and not to a file, so
  // we enable them no matter what.
  OplockDebugRecordRequest(Fcb, FsControlCode, RequestedLevel);
  if (Flags & REQUEST_OPLOCK_INPUT_FLAG_ACK) {
    OplockDebugRecordFlag(Fcb, DOKAN_OPLOCK_DEBUG_GENERIC_ACKNOWLEDGEMENT);
  }
  // Don't log via the Event Log unless flagged on (which should never be the
  // case by default).
  if (!DokanOpLockDebugEnabled()) {
    return;
  }
  if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
    DokanLogInfo(Logger, L"Oplock request FSCTL_REQUEST_OPLOCK for file %wZ;"
                 L" oplock count %d; acquired FCB %d; acquired VCB %d;"
                 L" level = %I32x; flags = %I32x",
                 &Fcb->FileName, OplockCount, AcquiredFcb, AcquiredVcb,
                 RequestedLevel, Flags);
    return;
  }
  DokanLogInfo(Logger, L"Oplock request %s for file %wZ; oplock count %d;"
               L" acquired FCB %d; acquired VCB %d",
               DokanGetOplockControlCodeName(FsControlCode),
               &Fcb->FileName, OplockCount, AcquiredFcb, AcquiredVcb);
}

void DokanMaybeLogOplockResult(__in PDOKAN_LOGGER Logger,
                               __in PDokanFCB Fcb,
                               __in ULONG FsControlCode,
                               __in ULONG RequestedLevel,
                               __in ULONG Flags,
                               __in NTSTATUS Status) {
  if (!DokanOpLockDebugEnabled()) {
    return;
  }
  if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
    DokanLogInfo(Logger, L"Oplock result for FSCTL_REQUEST_OPLOCK for file %wZ;"
                 L" level = %I32x; flags = %I32x; status = 0x%I32x",
                 &Fcb->FileName, RequestedLevel, Flags, Status);
    return;
  }
  DokanLogInfo(Logger, L"Oplock result for %s for file %wZ; status = 0x%I32x",
               DokanGetOplockControlCodeName(FsControlCode), &Fcb->FileName,
               Status);
}

NTSTATUS DokanOplockRequest(__in PIRP *pIrp) {
  NTSTATUS Status = STATUS_SUCCESS;
  ULONG FsControlCode;
  PDokanDCB Dcb;
  PDokanVCB Vcb;
  PDokanFCB Fcb = NULL;
  PDokanCCB Ccb;
  PFILE_OBJECT fileObject;
  PIRP Irp = *pIrp;
  ULONG OplockCount = 0;

  PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

  BOOLEAN AcquiredVcb = FALSE;
  BOOLEAN AcquiredFcb = FALSE;

  PREQUEST_OPLOCK_INPUT_BUFFER InputBuffer = NULL;
  ULONG InputBufferLength;
  ULONG OutputBufferLength;

  PAGED_CODE();

  //
  //  Save some references to make our life a little easier
  //
  FsControlCode = IrpSp->Parameters.FileSystemControl.FsControlCode;

  fileObject = IrpSp->FileObject;
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
  OplockDebugRecordMajorFunction(Fcb, IRP_MJ_FILE_SYSTEM_CONTROL);
  Vcb = Fcb->Vcb;
  if (Vcb == NULL || Vcb->Identifier.Type != VCB) {
    DDbgPrint("    DokanOplockRequest STATUS_INVALID_PARAMETER\n");
    return STATUS_INVALID_PARAMETER;
  }
  DOKAN_INIT_LOGGER(logger, Vcb->DeviceObject->DriverObject, 0);

  Dcb = Vcb->Dcb;
  if (Dcb == NULL || Dcb->Identifier.Type != DCB) {
    return STATUS_INVALID_PARAMETER;
  }

  if (Dcb->OplocksDisabled) {
    return STATUS_NOT_SUPPORTED;
  }

  //
  //  Get the input & output buffer lengths and pointers.
  //
  if (FsControlCode == FSCTL_REQUEST_OPLOCK) {

    InputBufferLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    InputBuffer = (PREQUEST_OPLOCK_INPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;

    OutputBufferLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;

    //
    //  Check for a minimum length on the input and ouput buffers.
    //
    if ((InputBufferLength < sizeof(REQUEST_OPLOCK_INPUT_BUFFER)) ||
        (OutputBufferLength < sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER))) {
      DDbgPrint("    DokanOplockRequest STATUS_BUFFER_TOO_SMALL\n");
      return STATUS_BUFFER_TOO_SMALL;
    }
  }

  //
  //  If the oplock request is on a directory it must be for a Read or
  //  Read-Handle
  //  oplock only.
  //
  if ((DokanFCBFlagsIsSet(Fcb, DOKAN_FILE_DIRECTORY)) &&
      ((FsControlCode != FSCTL_REQUEST_OPLOCK) ||
       !FsRtlOplockIsSharedRequest(Irp))) {

    DDbgPrint("    DokanOplockRequest STATUS_INVALID_PARAMETER\n");
    return STATUS_INVALID_PARAMETER;
  }

  //
  //  Use a try finally to free the Fcb/Vcb
  //
  try {

    //
    //  We grab the Fcb exclusively for oplock requests, shared for oplock
    //  break acknowledgement.
    //
    if ((FsControlCode == FSCTL_REQUEST_OPLOCK_LEVEL_1) ||
        (FsControlCode == FSCTL_REQUEST_BATCH_OPLOCK) ||
        (FsControlCode == FSCTL_REQUEST_FILTER_OPLOCK) ||
        (FsControlCode == FSCTL_REQUEST_OPLOCK_LEVEL_2) ||
        ((FsControlCode == FSCTL_REQUEST_OPLOCK) &&
            FlagOn(InputBuffer->Flags, REQUEST_OPLOCK_INPUT_FLAG_REQUEST))
    ) {

      DokanVCBLockRO(Fcb->Vcb);
      AcquiredVcb = TRUE;
      DokanFCBLockRW(Fcb);
      AcquiredFcb = TRUE;

      if (!Dcb->FileLockInUserMode) {

        if (FsRtlOplockIsSharedRequest(Irp)) {
          //
          //  Byte-range locks are only valid on files.
          //
          if (!DokanFCBFlagsIsSet(Fcb, DOKAN_FILE_DIRECTORY)) {

            //
            //  Set OplockCount to nonzero if FsRtl denies access
            //  based on current byte-range lock state.
            //
            if (DokanFsRtlCheckLockForOplockRequest) // Win8+
              OplockCount = (ULONG)!DokanFsRtlCheckLockForOplockRequest(
                  &Fcb->FileLock, &Fcb->AdvancedFCBHeader.AllocationSize);
            else
              OplockCount = (ULONG)FsRtlAreThereCurrentOrInProgressFileLocks(
                  &Fcb->FileLock);
          }
        } else {
          // Shouldn't be something like UncleanCount counter and not FileCount
          // here?
          OplockCount = Fcb->FileCount;
        }
      }
    } else if ((FsControlCode == FSCTL_OPLOCK_BREAK_ACKNOWLEDGE) ||
               (FsControlCode == FSCTL_OPBATCH_ACK_CLOSE_PENDING) ||
               (FsControlCode == FSCTL_OPLOCK_BREAK_NOTIFY) ||
               (FsControlCode == FSCTL_OPLOCK_BREAK_ACK_NO_2) ||
               ((FsControlCode == FSCTL_REQUEST_OPLOCK) &&
                   FlagOn(InputBuffer->Flags, REQUEST_OPLOCK_INPUT_FLAG_ACK))
    ) {
      DokanFCBLockRO(Fcb);
      AcquiredFcb = TRUE;
    } else if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
      //
      //  The caller didn't provide either REQUEST_OPLOCK_INPUT_FLAG_REQUEST or
      //  REQUEST_OPLOCK_INPUT_FLAG_ACK on the input buffer.
      //
      DDbgPrint("    DokanOplockRequest STATUS_INVALID_PARAMETER\n");
      Status = STATUS_INVALID_PARAMETER;
      __leave;
    } else {
      DDbgPrint("    DokanOplockRequest STATUS_INVALID_PARAMETER\n");
      Status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    //
    //  Fail batch, filter, and handle oplock requests if the file is marked
    //  for delete.
    //
    if (((FsControlCode == FSCTL_REQUEST_FILTER_OPLOCK) ||
         (FsControlCode == FSCTL_REQUEST_BATCH_OPLOCK) ||
         ((FsControlCode == FSCTL_REQUEST_OPLOCK) &&
          FlagOn(InputBuffer->RequestedOplockLevel, OPLOCK_LEVEL_CACHE_HANDLE))
             ) &&
        DokanFCBFlagsIsSet(Fcb, DOKAN_DELETE_ON_CLOSE)) {

      DDbgPrint("    DokanOplockRequest STATUS_DELETE_PENDING\n");
      Status = STATUS_DELETE_PENDING;
      __leave;
    }

    ULONG level = 0;
    ULONG flags = 0;
    if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
      level = InputBuffer->RequestedOplockLevel;
      flags = InputBuffer->Flags;
    }
    DokanMaybeLogOplockRequest(&logger, Fcb, FsControlCode, OplockCount,
                               AcquiredFcb, AcquiredVcb, level, flags);

    //
    //  Call the FsRtl routine to grant/acknowledge oplock.
    //
    Status = FsRtlOplockFsctrl(DokanGetFcbOplock(Fcb), Irp, OplockCount);
    DokanMaybeLogOplockResult(&logger, Fcb, FsControlCode, level, flags,
                              Status);
    //
    //  Once we call FsRtlOplockFsctrl, we no longer own the IRP and we should
    //  not complete it.
    //
    *pIrp = NULL;

  } finally {

    //
    //  Release all of our resources
    //
    if (AcquiredFcb) {
      DokanFCBUnlock(Fcb);
    }
    if (AcquiredVcb) {
      DokanVCBUnlock(Fcb->Vcb);
    }

    DDbgPrint("    DokanOplockRequest return 0x%x\n", Status);
  }

  return Status;
}

NTSTATUS
DokanUserFsRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP *pIrp) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  PIO_STACK_LOCATION irpSp;
  PFILE_OBJECT fileObject = NULL;
  PDokanCCB ccb = NULL;
  PDokanFCB fcb = NULL;
  DOKAN_INIT_LOGGER(logger, DeviceObject->DriverObject,
                    IRP_MJ_FILE_SYSTEM_CONTROL);

  irpSp = IoGetCurrentIrpStackLocation(*pIrp);

  switch (irpSp->Parameters.FileSystemControl.FsControlCode) {
  case FSCTL_ACTIVATE_KEEPALIVE:
    fileObject = irpSp->FileObject;
    if (fileObject == NULL) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE with no FileObject.");
    }
    ccb = fileObject->FsContext2;
    if (ccb == NULL || ccb->Identifier.Type != CCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE with no CCB.");
    }

    fcb = ccb->Fcb;
    if (fcb == NULL || fcb->Identifier.Type != FCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE with no FCB.");
    }

    if (!fcb->IsKeepalive) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE for wrong file: %wZ",
          &fcb->FileName);
    }

    if (fcb->Vcb->IsKeepaliveActive && !ccb->IsKeepaliveActive) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE when a different keepalive handle"
          L" was already active.");
    }

    DokanLogInfo(&logger, L"Activating keepalive handle from process %lu.",
                 IoGetRequestorProcessId(*pIrp));
    DokanFCBLockRW(fcb);
    ccb->IsKeepaliveActive = TRUE;
    fcb->Vcb->IsKeepaliveActive = TRUE;
    DokanFCBUnlock(fcb);
    status = STATUS_SUCCESS;
    break;

  case FSCTL_NOTIFY_PATH: {
    PDOKAN_NOTIFY_PATH_INTERMEDIATE pNotifyPath;
    irpSp = IoGetCurrentIrpStackLocation(*pIrp);
    if (irpSp->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(DOKAN_NOTIFY_PATH_INTERMEDIATE)) {
      DDbgPrint(
          "Input buffer is too small (< DOKAN_NOTIFY_PATH_INTERMEDIATE)\n");
      return STATUS_BUFFER_TOO_SMALL;
    }
    pNotifyPath =
        (PDOKAN_NOTIFY_PATH_INTERMEDIATE)(*pIrp)->AssociatedIrp.SystemBuffer;
    if (irpSp->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(DOKAN_NOTIFY_PATH_INTERMEDIATE) + pNotifyPath->Length -
            sizeof(WCHAR)) {
      DDbgPrint("Input buffer is too small\n");
      return STATUS_BUFFER_TOO_SMALL;
    }
    fileObject = irpSp->FileObject;
    if (fileObject == NULL) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_NOTIFY_PATH with no FileObject.");
    }
    ccb = fileObject->FsContext2;
    if (ccb == NULL || ccb->Identifier.Type != CCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_NOTIFY_PATH with no CCB.");
    }
    fcb = ccb->Fcb;
    if (fcb == NULL || fcb->Identifier.Type != FCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_NOTIFY_PATH with no FCB.");
    }
    UNICODE_STRING receivedBuffer;
    receivedBuffer.Length = pNotifyPath->Length;
    receivedBuffer.MaximumLength = pNotifyPath->Length;
    receivedBuffer.Buffer = pNotifyPath->Buffer;
    DDbgPrint(
        "Received FSCTL_NOTIFY_PATH, CompletionFilter: %lu, Action: %lu, "
        "Length: %i, Path: %wZ", pNotifyPath->CompletionFilter,
        pNotifyPath->Action, receivedBuffer.Length, &receivedBuffer);
    DokanFCBLockRO(fcb);
    DokanNotifyReportChange0(fcb, &receivedBuffer,
                             pNotifyPath->CompletionFilter,
                             pNotifyPath->Action);
    DokanFCBUnlock(fcb);
    status = STATUS_SUCCESS;
  } break;
  case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    DDbgPrint("    FSCTL_REQUEST_OPLOCK_LEVEL_1\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    DDbgPrint("    FSCTL_REQUEST_OPLOCK_LEVEL_2\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_BATCH_OPLOCK:
    DDbgPrint("    FSCTL_REQUEST_BATCH_OPLOCK\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    DDbgPrint("    FSCTL_OPLOCK_BREAK_ACKNOWLEDGE\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    DDbgPrint("    FSCTL_OPBATCH_ACK_CLOSE_PENDING\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPLOCK_BREAK_NOTIFY:
    DDbgPrint("    FSCTL_OPLOCK_BREAK_NOTIFY\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPLOCK_BREAK_ACK_NO_2:
    DDbgPrint("    FSCTL_OPLOCK_BREAK_ACK_NO_2\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_FILTER_OPLOCK:
    DDbgPrint("    FSCTL_REQUEST_FILTER_OPLOCK\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_OPLOCK:
    DDbgPrint("    FSCTL_REQUEST_OPLOCK\n");
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_LOCK_VOLUME:
    DDbgPrint("    FSCTL_LOCK_VOLUME\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_UNLOCK_VOLUME:
    DDbgPrint("    FSCTL_UNLOCK_VOLUME\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_DISMOUNT_VOLUME:
    DDbgPrint("    FSCTL_DISMOUNT_VOLUME\n");
    break;

  case FSCTL_IS_VOLUME_MOUNTED:
    DDbgPrint("    FSCTL_IS_VOLUME_MOUNTED\n");
    status = STATUS_SUCCESS;
    break;

  case FSCTL_IS_PATHNAME_VALID:
    DDbgPrint("    FSCTL_IS_PATHNAME_VALID\n");
    break;

  case FSCTL_MARK_VOLUME_DIRTY:
    DDbgPrint("    FSCTL_MARK_VOLUME_DIRTY\n");
    break;

  case FSCTL_QUERY_RETRIEVAL_POINTERS:
    DDbgPrint("    FSCTL_QUERY_RETRIEVAL_POINTERS\n");
    break;

  case FSCTL_GET_COMPRESSION:
    DDbgPrint("    FSCTL_GET_COMPRESSION\n");
    break;

  case FSCTL_SET_COMPRESSION:
    DDbgPrint("    FSCTL_SET_COMPRESSION\n");
    break;

  case FSCTL_MARK_AS_SYSTEM_HIVE:
    DDbgPrint("    FSCTL_MARK_AS_SYSTEM_HIVE\n");
    break;

  case FSCTL_INVALIDATE_VOLUMES:
    DDbgPrint("    FSCTL_INVALIDATE_VOLUMES\n");
    break;

  case FSCTL_QUERY_FAT_BPB:
    DDbgPrint("    FSCTL_QUERY_FAT_BPB\n");
    break;

  case FSCTL_FILESYSTEM_GET_STATISTICS:
    DDbgPrint("    FSCTL_FILESYSTEM_GET_STATISTICS\n");
    break;

  case FSCTL_GET_NTFS_VOLUME_DATA:
    DDbgPrint("    FSCTL_GET_NTFS_VOLUME_DATA\n");
    break;

  case FSCTL_GET_NTFS_FILE_RECORD:
    DDbgPrint("    FSCTL_GET_NTFS_FILE_RECORD\n");
    break;

  case FSCTL_GET_VOLUME_BITMAP:
    DDbgPrint("    FSCTL_GET_VOLUME_BITMAP\n");
    break;

  case FSCTL_GET_RETRIEVAL_POINTERS:
    DDbgPrint("    FSCTL_GET_RETRIEVAL_POINTERS\n");
    break;

  case FSCTL_MOVE_FILE:
    DDbgPrint("    FSCTL_MOVE_FILE\n");
    break;

  case FSCTL_IS_VOLUME_DIRTY:
    DDbgPrint("    FSCTL_IS_VOLUME_DIRTY\n");
    break;

  case FSCTL_ALLOW_EXTENDED_DASD_IO:
    DDbgPrint("    FSCTL_ALLOW_EXTENDED_DASD_IO\n");
    break;

  case FSCTL_FIND_FILES_BY_SID:
    DDbgPrint("    FSCTL_FIND_FILES_BY_SID\n");
    break;

  case FSCTL_SET_OBJECT_ID:
    DDbgPrint("    FSCTL_SET_OBJECT_ID\n");
    break;

  case FSCTL_GET_OBJECT_ID:
    DDbgPrint("    FSCTL_GET_OBJECT_ID\n");
    break;

  case FSCTL_DELETE_OBJECT_ID:
    DDbgPrint("    FSCTL_DELETE_OBJECT_ID\n");
    break;

  case FSCTL_SET_REPARSE_POINT:
    DDbgPrint("    FSCTL_SET_REPARSE_POINT\n");
    break;

  case FSCTL_GET_REPARSE_POINT:
    DDbgPrint("    FSCTL_GET_REPARSE_POINT\n");
    status = STATUS_NOT_A_REPARSE_POINT;
    break;

  case FSCTL_DELETE_REPARSE_POINT:
    DDbgPrint("    FSCTL_DELETE_REPARSE_POINT\n");
    break;

  case FSCTL_ENUM_USN_DATA:
    DDbgPrint("    FSCTL_ENUM_USN_DATA\n");
    break;

  case FSCTL_SECURITY_ID_CHECK:
    DDbgPrint("    FSCTL_SECURITY_ID_CHECK\n");
    break;

  case FSCTL_READ_USN_JOURNAL:
    DDbgPrint("    FSCTL_READ_USN_JOURNAL\n");
    break;

  case FSCTL_SET_OBJECT_ID_EXTENDED:
    DDbgPrint("    FSCTL_SET_OBJECT_ID_EXTENDED\n");
    break;

  case FSCTL_CREATE_OR_GET_OBJECT_ID:
    DDbgPrint("    FSCTL_CREATE_OR_GET_OBJECT_ID\n");
    break;

  case FSCTL_SET_SPARSE:
    DDbgPrint("    FSCTL_SET_SPARSE\n");
    break;

  case FSCTL_SET_ZERO_DATA:
    DDbgPrint("    FSCTL_SET_ZERO_DATA\n");
    break;

  case FSCTL_QUERY_ALLOCATED_RANGES:
    DDbgPrint("    FSCTL_QUERY_ALLOCATED_RANGES\n");
    break;

  case FSCTL_SET_ENCRYPTION:
    DDbgPrint("    FSCTL_SET_ENCRYPTION\n");
    break;

  case FSCTL_ENCRYPTION_FSCTL_IO:
    DDbgPrint("    FSCTL_ENCRYPTION_FSCTL_IO\n");
    break;

  case FSCTL_WRITE_RAW_ENCRYPTED:
    DDbgPrint("    FSCTL_WRITE_RAW_ENCRYPTED\n");
    break;

  case FSCTL_READ_RAW_ENCRYPTED:
    DDbgPrint("    FSCTL_READ_RAW_ENCRYPTED\n");
    break;

  case FSCTL_CREATE_USN_JOURNAL:
    DDbgPrint("    FSCTL_CREATE_USN_JOURNAL\n");
    break;

  case FSCTL_READ_FILE_USN_DATA:
    DDbgPrint("    FSCTL_READ_FILE_USN_DATA\n");
    break;

  case FSCTL_WRITE_USN_CLOSE_RECORD:
    DDbgPrint("    FSCTL_WRITE_USN_CLOSE_RECORD\n");
    break;

  case FSCTL_EXTEND_VOLUME:
    DDbgPrint("    FSCTL_EXTEND_VOLUME\n");
    break;

  case FSCTL_QUERY_USN_JOURNAL:
    DDbgPrint("    FSCTL_QUERY_USN_JOURNAL\n");
    break;

  case FSCTL_DELETE_USN_JOURNAL:
    DDbgPrint("    FSCTL_DELETE_USN_JOURNAL\n");
    break;

  case FSCTL_MARK_HANDLE:
    DDbgPrint("    FSCTL_MARK_HANDLE\n");
    break;

  case FSCTL_SIS_COPYFILE:
    DDbgPrint("    FSCTL_SIS_COPYFILE\n");
    break;

  case FSCTL_SIS_LINK_FILES:
    DDbgPrint("    FSCTL_SIS_LINK_FILES\n");
    break;

  case FSCTL_RECALL_FILE:
    DDbgPrint("    FSCTL_RECALL_FILE\n");
    break;

  case FSCTL_SET_ZERO_ON_DEALLOCATION:
    DDbgPrint("    FSCTL_SET_ZERO_ON_DEALLOCATION\n");
    break;

  case FSCTL_CSC_INTERNAL:
    DDbgPrint("    FSCTL_CSC_INTERNAL\n");
    break;

  case FSCTL_QUERY_ON_DISK_VOLUME_INFO:
    DDbgPrint("    FSCTL_QUERY_ON_DISK_VOLUME_INFO\n");
    break;

  default:
    DDbgPrint("    Unknown FSCTL %d\n",
              (irpSp->Parameters.FileSystemControl.FsControlCode >> 2) & 0xFFF);
    status = STATUS_INVALID_DEVICE_REQUEST;
  }

  return status;
}

NTSTATUS DokanMountVolume(__in PDEVICE_OBJECT DiskDevice, __in PIRP Irp) {
  PDokanDCB dcb = NULL;
  PDokanVCB vcb = NULL;
  PVPB vpb = NULL;
  DOKAN_CONTROL dokanControl;
  PMOUNT_ENTRY mountEntry = NULL;
  PIO_STACK_LOCATION irpSp;
  PDEVICE_OBJECT volDeviceObject;
  PDRIVER_OBJECT DriverObject = DiskDevice->DriverObject;
  NTSTATUS status = STATUS_UNRECOGNIZED_VOLUME;

  DOKAN_INIT_LOGGER(logger, DriverObject, IRP_MJ_FILE_SYSTEM_CONTROL);
  DokanLogInfo(&logger, L"Mounting disk device.");

  irpSp = IoGetCurrentIrpStackLocation(Irp);
  dcb = irpSp->Parameters.MountVolume.DeviceObject->DeviceExtension;
  if (!dcb) {
    DDbgPrint("   Not DokanDiskDevice (no device extension)\n");
    return status;
  }
  PrintIdType(dcb);
  if (GetIdentifierType(dcb) != DCB) {
    DDbgPrint("   Not DokanDiskDevice\n");
    return status;
  }

  if (IsDeletePending(dcb->DeviceObject)) {
    return DokanLogError(&logger, STATUS_DEVICE_REMOVED,
                         L"This is a remount try of the device.");
  }

  BOOLEAN isNetworkFileSystem =
      (dcb->VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);

  DokanLogInfo(&logger, L"Mounting volume using MountPoint %wZ device %wZ",
               dcb->MountPoint, dcb->DiskDeviceName);

  if (!isNetworkFileSystem) {
    status = IoCreateDevice(DriverObject,               // DriverObject
                            sizeof(DokanVCB),           // DeviceExtensionSize
                            NULL,                       // DeviceName
                            dcb->VolumeDeviceType,      // DeviceType
                            dcb->DeviceCharacteristics, // DeviceCharacteristics
                            FALSE,                      // Not Exclusive
                            &volDeviceObject);          // DeviceObject
  } else {
    status = IoCreateDeviceSecure(
        DriverObject,               // DriverObject
        sizeof(DokanVCB),           // DeviceExtensionSize
        dcb->DiskDeviceName,        // DeviceName
        dcb->VolumeDeviceType,      // DeviceType
        dcb->DeviceCharacteristics, // DeviceCharacteristics
        FALSE,                      // Not Exclusive
        &sddl,                      // Default SDDL String
        NULL,                       // Device Class GUID
        &volDeviceObject);          // DeviceObject
  }

  if (!NT_SUCCESS(status)) {
    return DokanLogError(&logger, status, L"IoCreateDevice failed.");
  }

  vcb = volDeviceObject->DeviceExtension;
  vcb->Identifier.Type = VCB;
  vcb->Identifier.Size = sizeof(DokanVCB);

  vcb->DeviceObject = volDeviceObject;
  vcb->Dcb = dcb;
  vcb->ResourceLogger.DriverObject = DriverObject;
  dcb->Vcb = vcb;

  InitializeListHead(&vcb->NextFCB);

  InitializeListHead(&vcb->DirNotifyList);
  FsRtlNotifyInitializeSync(&vcb->NotifySync);

  ExInitializeFastMutex(&vcb->AdvancedFCBHeaderMutex);

  FsRtlSetupAdvancedHeader(&vcb->VolumeFileHeader,
                           &vcb->AdvancedFCBHeaderMutex);

  vpb = irpSp->Parameters.MountVolume.Vpb;
  DokanInitVpb(vpb, vcb->DeviceObject);

  //
  // Establish user-buffer access method.
  //
  SetLongFlag(volDeviceObject->Flags, DO_DIRECT_IO);
  ClearLongFlag(volDeviceObject->Flags, DO_DEVICE_INITIALIZING);
  SetLongFlag(vcb->Flags, VCB_MOUNTED);

  ObReferenceObject(volDeviceObject);

  DDbgPrint("  ExAcquireResourceExclusiveLite dcb resource \n")
  ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);

  // set the device on dokanControl
  RtlZeroMemory(&dokanControl, sizeof(dokanControl));
  RtlCopyMemory(dokanControl.DeviceName, dcb->DiskDeviceName->Buffer,
                dcb->DiskDeviceName->Length);
  if (dcb->UNCName->Buffer != NULL && dcb->UNCName->Length > 0) {
    RtlCopyMemory(dokanControl.UNCName, dcb->UNCName->Buffer,
                  dcb->UNCName->Length);
  }
  dokanControl.SessionId = dcb->SessionId;
  mountEntry = FindMountEntry(dcb->Global, &dokanControl, TRUE);
  if (mountEntry != NULL) {
    mountEntry->MountControl.DeviceObject = volDeviceObject;
  } else {
    ExReleaseResourceLite(&dcb->Resource);
    return DokanLogError(&logger, STATUS_DEVICE_REMOVED,
                         L"MountEntry not found.");
  }

  ExReleaseResourceLite(&dcb->Resource);

  // Start check thread
  ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);
  DokanUpdateTimeout(&dcb->TickCount, DOKAN_KEEPALIVE_TIMEOUT_DEFAULT * 3);
  ExReleaseResourceLite(&dcb->Resource);
  DokanStartCheckThread(dcb);

  // Create mount point for the volume
  if (dcb->UseMountManager) {
    status = DokanSendVolumeArrivalNotification(dcb->DiskDeviceName);
    if (!NT_SUCCESS(status)) {
      DokanLogError(&logger, status,
                    L"DokanSendVolumeArrivalNotification failed.");
    }
  }
  DokanCreateMountPoint(dcb);

  if (isNetworkFileSystem) {
    DokanRegisterUncProviderSystem(dcb);
  }

  DokanLogInfo(&logger, L"Mounting successfully done.");

  return STATUS_SUCCESS;
}

VOID DokanInitVpb(__in PVPB Vpb, __in PDEVICE_OBJECT VolumeDevice) {
  if (Vpb != NULL) {
    Vpb->DeviceObject = VolumeDevice;
    Vpb->VolumeLabelLength = (USHORT)wcslen(VOLUME_LABEL) * sizeof(WCHAR);
    RtlStringCchCopyW(Vpb->VolumeLabel,
                      sizeof(Vpb->VolumeLabel) / sizeof(WCHAR), VOLUME_LABEL);
    Vpb->SerialNumber = 0x19831116;
  }
}

NTSTATUS
DokanDispatchFileSystemControl(__in PDEVICE_OBJECT DeviceObject,
                               __in PIRP Irp) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  PIO_STACK_LOCATION irpSp;

  __try {
    DDbgPrint("==> DokanFileSystemControl\n");
    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (irpSp->MinorFunction) {
    case IRP_MN_KERNEL_CALL:
      DDbgPrint("     IRP_MN_KERNEL_CALL\n");
      break;

    case IRP_MN_LOAD_FILE_SYSTEM:
      DDbgPrint("     IRP_MN_LOAD_FILE_SYSTEM\n");
      break;

    case IRP_MN_MOUNT_VOLUME: {
      DDbgPrint("     IRP_MN_MOUNT_VOLUME\n");
      status = DokanMountVolume(DeviceObject, Irp);
    } break;

    case IRP_MN_USER_FS_REQUEST:
      DDbgPrint("     IRP_MN_USER_FS_REQUEST\n");
      status = DokanUserFsRequest(DeviceObject, &Irp);
      break;

    case IRP_MN_VERIFY_VOLUME:
      DDbgPrint("     IRP_MN_VERIFY_VOLUME\n");
      break;

    default:
      DDbgPrint("  unknown %d\n", irpSp->MinorFunction);
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
    }

  } __finally {
    DokanCompleteIrpRequest(Irp, status, 0);
    DDbgPrint("<== DokanFileSystemControl\n");
  }

  return status;
}
