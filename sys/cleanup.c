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

NTSTATUS
DokanDispatchCleanup(__in PREQUEST_CONTEXT RequestContext)

/*++

Routine Description:

        This device control dispatcher handles Cleanup IRP.

Arguments:

        DeviceObject - Context for the activity.
        Irp          - The device control argument block.

Return Value:

        NTSTATUS

--*/
{
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb = NULL;
  PDokanFCB fcb = NULL;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject,
                    IRP_MJ_CLEANUP);

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  // Cleanup must be success in any case
  if (fileObject == NULL || RequestContext->Vcb == NULL ||
      !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
    return STATUS_SUCCESS;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  OplockDebugRecordMajorFunction(fcb, IRP_MJ_CLEANUP);
  if (fcb->IsKeepalive) {
    DokanFCBLockRW(fcb);
    BOOLEAN shouldUnmount = ccb->IsKeepaliveActive;
    if (shouldUnmount) {
      // Here we intentionally let the VCB-level flag stay set, because
      // there's no sense in having an opportunity for an "operation timeout
      // unmount" in this case.
      ccb->IsKeepaliveActive = FALSE;
    }
    DokanFCBUnlock(fcb);
    if (shouldUnmount) {
      if (IsUnmountPendingVcb(RequestContext->Vcb)) {
        DokanLogInfo(&logger,
                     L"Ignoring keepalive close because unmount is already in"
                     L" progress.");
      } else {
        DokanLogInfo(&logger, L"Unmounting due to keepalive close.");
        DokanUnmount(RequestContext, RequestContext->Dcb);
      }
    }
  }
  if (fcb->BlockUserModeDispatch) {
    return STATUS_SUCCESS;
  }

  FlushFcb(RequestContext, fcb, fileObject);

  DokanFCBLockRW(fcb);


  eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
  eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

  if (eventContext == NULL) {
    DokanFCBUnlock(fcb);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  fileObject->Flags |= FO_CLEANUP_COMPLETE;

  eventContext->Context = ccb->UserContext;
  eventContext->FileFlags |= DokanCCBFlagsGet(ccb);

  // copy the filename to EventContext from ccb
  eventContext->Operation.Cleanup.FileNameLength = fcb->FileName.Length;
  RtlCopyMemory(eventContext->Operation.Cleanup.FileName,
                fcb->FileName.Buffer, fcb->FileName.Length);

  // FsRtlCheckOpLock is called with non-NULL completion routine - not blocking.
  status = DokanCheckOplock(fcb, RequestContext->Irp, eventContext,
                            DokanOplockComplete,
                            DokanPrePostIrp);
  DokanFCBUnlock(fcb);

  //
  //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
  //  to service an oplock break and we need to leave now.
  //
  if (status != STATUS_SUCCESS) {
    if (status == STATUS_PENDING) {
      DOKAN_LOG_FINE_IRP(RequestContext, "FsRtlCheckOplock returned STATUS_PENDING");
    } else {
      DokanFreeEventContext(eventContext);
    }
    return status;
  }

  // register this IRP to pending IRP list
  return DokanRegisterPendingIrp(RequestContext, eventContext);
}

VOID DokanCompleteCleanup(__in PREQUEST_CONTEXT RequestContext,
                          __in PEVENT_INFORMATION EventInfo) {
  PDokanCCB ccb;
  PDokanFCB fcb;
  PFILE_OBJECT fileObject;

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p",
                     RequestContext->IrpSp->FileObject);

  fileObject = RequestContext->IrpSp->FileObject;
  ASSERT(fileObject != NULL);

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);
  ccb->UserContext = EventInfo->Context;

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  DokanFCBLockRW(fcb);

  IoRemoveShareAccess(RequestContext->IrpSp->FileObject, &fcb->ShareAccess);

  if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_CHANGE_LAST_WRITE)) {
    DokanNotifyReportChange(RequestContext, fcb,
                            FILE_NOTIFY_CHANGE_LAST_WRITE,
                            FILE_ACTION_MODIFIED);
  }

  if (DokanFCBFlagsIsSet(fcb, DOKAN_DELETE_ON_CLOSE)) {
    if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
      DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_DIR_NAME,
                              FILE_ACTION_REMOVED);
    } else {
      DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_FILE_NAME,
                              FILE_ACTION_REMOVED);
    }
  }
  DokanFCBUnlock(fcb);
  //
  //  Unlock all outstanding file locks.
  //
  (VOID) FsRtlFastUnlockAll(&fcb->FileLock, fileObject,
                            IoGetRequestorProcess(RequestContext->Irp), NULL);

  if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
    FsRtlNotifyCleanup(RequestContext->Vcb->NotifySync,
                       &RequestContext->Vcb->DirNotifyList, ccb);
  }

  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
}
