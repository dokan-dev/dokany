/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2021 Google, Inc.
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

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanCommonLockControl)
#endif

NTSTATUS
DokanCommonLockControl(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_SUCCESS;
  PDokanFCB fcb;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;

  PAGED_CODE();

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  ccb = fileObject->FsContext2;
  if (ccb == NULL || ccb->Identifier.Type != CCB) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Invalid CCB or wrong type");
    return STATUS_INVALID_PARAMETER;
  }

  fcb = ccb->Fcb;
  if (fcb == NULL || fcb->Identifier.Type != FCB) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Invalid FCB or wrong type");
    return STATUS_INVALID_PARAMETER;
  }

  //
  //  If the file is not a user file open then we reject the request
  //  as an invalid parameter
  //
  if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Not a user file open");
    return STATUS_INVALID_PARAMETER;
  }

  //
  //  We check whether we can proceed
  //  based on the state of the file oplocks.
  //

  if (!DokanFsRtlAreThereWaitingFileLocks ||
      // For >= NTDDI_WIN8 we check the file state
      // Fcb's AllocationSize is constant after creation.
      ((IRP_MN_LOCK == RequestContext->IrpSp->MinorFunction) &&
       ((ULONGLONG)
            RequestContext->IrpSp->Parameters.LockControl.ByteOffset.QuadPart <
        (ULONGLONG)fcb->AdvancedFCBHeader.AllocationSize.QuadPart)) ||
      ((IRP_MN_LOCK != RequestContext->IrpSp->MinorFunction) &&
       DokanFsRtlAreThereWaitingFileLocks(&fcb->FileLock))) {

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

    // Dokan DokanOplockComplete sends the operation to user mode, which isn't
    // what we want to do
    // so now wait for the oplock to be broken (pass in NULL for the callback)
    // This may block and enter wait state.
    status = DokanCheckOplock(fcb, RequestContext->Irp, NULL /* EventContext */,
                              NULL /*DokanOplockComplete*/, NULL);
  }

  //  If we were waiting for the callback, then STATUS_PENDING would be ok too
  if (status == STATUS_SUCCESS) {
    //
    //  Now call the FsRtl routine to do the actual processing of the
    //  Lock request
    //
    status = FsRtlProcessFileLock(&fcb->FileLock, RequestContext->Irp, NULL);

    // FsRtlProcessFileLock relinquish control of the input IRP
    RequestContext->DoNotComplete = TRUE;
  }

  return status;
}

NTSTATUS
DokanDispatchLock(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  PEVENT_CONTEXT eventContext = NULL;
  ULONG eventLength;

  __try {
    fileObject = RequestContext->IrpSp->FileObject;
    DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

    if (fileObject == NULL || !RequestContext->Vcb ||
        !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);
    DokanFCBLockRW(fcb);

    OplockDebugRecordMajorFunction(fcb, IRP_MJ_LOCK_CONTROL);
    if (!RequestContext->Dcb->FileLockInUserMode) {
      status = DokanCommonLockControl(RequestContext);
      __leave;
    }

    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
    eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    DOKAN_LOG_FINE_IRP(RequestContext, "Get Context %X", (ULONG)ccb->UserContext);

    // copy file name to be locked
    eventContext->Operation.Lock.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Lock.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    // parameters of Lock
    eventContext->Operation.Lock.ByteOffset =
        RequestContext->IrpSp->Parameters.LockControl.ByteOffset;
    if (RequestContext->IrpSp->Parameters.LockControl.Length != NULL) {
      eventContext->Operation.Lock.Length.QuadPart =
          RequestContext->IrpSp->Parameters.LockControl.Length->QuadPart;
    } else {
      DOKAN_LOG_FINE_IRP(RequestContext, "LockControl.Length = NULL");
    }
    eventContext->Operation.Lock.Key =
        RequestContext->IrpSp->Parameters.LockControl.Key;

    // register this IRP to waiting IRP list and make it pending status
    status = DokanRegisterPendingIrp(RequestContext, eventContext);
  } __finally {
    if (fcb) {
      DokanFCBUnlock(fcb);
    }
  }

  return status;
}

VOID DokanCompleteLock(__in PREQUEST_CONTEXT RequestContext,
                       __in PEVENT_INFORMATION EventInfo) {
  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
}
