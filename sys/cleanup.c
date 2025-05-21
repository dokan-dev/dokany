/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2017 - 2025 Google, Inc.
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

VOID DokanExecuteCleanup(__in PREQUEST_CONTEXT RequestContext) {
  PFILE_OBJECT fileObject;
  PDokanCCB ccb = NULL;
  PDokanFCB fcb = NULL;

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  // Cleanup must be success in any case
  if (!fileObject || !RequestContext->Vcb || !RequestContext->Dcb ||
      !fileObject->FsContext2) {
    return;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  if (!IsCcbAndDcbSameMount(RequestContext, ccb, RequestContext->Dcb)) {
    return;
  }

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  DokanFCBLockRW(fcb);

  InterlockedDecrement(&fcb->UncleanCount);

  if (!RequestContext->RemovedShareAccessBeforeCheckOplock)
    IoRemoveShareAccess(RequestContext->IrpSp->FileObject, &fcb->ShareAccess);

  DokanFCBUnlock(fcb);
  //
  //  Unlock all outstanding file locks.
  //
  (VOID) FsRtlFastUnlockAll(&fcb->FileLock, fileObject,
                            IoGetRequestorProcess(RequestContext->Irp), NULL);
}

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
  if (!fileObject || !RequestContext->Vcb || !RequestContext->Dcb ||
      !fileObject->FsContext2) {
    return STATUS_SUCCESS;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  if (!IsCcbAndDcbSameMount(RequestContext, ccb, RequestContext->Dcb)) {
    return STATUS_SUCCESS;
  }

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  DokanFCBLockRO(fcb);
  if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_CHANGE_LAST_WRITE)) {
    DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_LAST_WRITE,
                            FILE_ACTION_MODIFIED);
  }
  // DeleteOnClose is set during CreateFile but is only executed by the last
  // handle on the object is closed. We transfer the marker to the Fcb and will
  // see below if there is any outstanding open handle that will delay the
  // deletion execution.
  if (DokanCCBFlagsIsSet(ccb, DOKAN_DELETE_ON_CLOSE)) {
    // `DOKAN_FCB_STATE_DELETE_PENDING` will prevent the Fcb to be open. Note:
    // Since `UncleanCount` is set on `DokanCompleteCreate` but this flag is
    // checked during `DokanDispatchCreate`, there is still a possible race
    // condition where an inflight create will succeed.
    DokanFCBFlagsSetBit(fcb, DOKAN_FCB_STATE_DELETE_PENDING);
    DokanCCBFlagsClearBit(ccb, DOKAN_DELETE_ON_CLOSE);
    DOKAN_LOG_FINE_IRP(RequestContext,
                       "Transfer DeleteOnClose from Ccb=%p to Fcb=%p", ccb,
                       fcb);
  }
  // The Fcb is marked for deletion due to DeleteOnClose or through
  // FileDisposition, the last handle will execute the deletion.
  BOOLEAN deletePending =
      fcb->UncleanCount == 1 && DokanFCBIsPendingDeletion(fcb);
  BOOLEAN isDirectory = DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY);
  if (deletePending) {
    DokanNotifyReportChange(RequestContext, fcb,
                            isDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME
                                        : FILE_NOTIFY_CHANGE_FILE_NAME,
                            FILE_ACTION_REMOVED);
  }
  if (isDirectory) {
    FsRtlNotifyCleanup(RequestContext->Vcb->NotifySync,
                       &RequestContext->Vcb->DirNotifyList, ccb);
  }
  DokanFCBUnlock(fcb);

  BOOLEAN isUnmountPending = IsUnmountPendingVcb(RequestContext->Vcb);

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
      if (isUnmountPending) {
        DokanLogInfo(&logger,
                     L"Ignoring keepalive close because unmount is already in"
                     L" progress.");
      } else {
        DokanLogInfo(&logger, L"Unmounting due to keepalive close.");
        DokanUnmount(RequestContext, RequestContext->Dcb);
      }
    }
  }
  if (isUnmountPending || fcb->BlockUserModeDispatch) {
    // Request will not reach Userland and therefore `DokanCompleteCleanup`
    // will not run, releasing the resources now.
    DokanExecuteCleanup(RequestContext);
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
  eventContext->FileFlags |=
      DokanCCBFlagsGet(ccb) | (deletePending ? DOKAN_DELETE_ON_CLOSE : 0);

  // copy the filename to EventContext from ccb
  eventContext->Operation.Cleanup.FileNameLength = fcb->FileName.Length;
  RtlCopyMemory(eventContext->Operation.Cleanup.FileName,
                fcb->FileName.Buffer, fcb->FileName.Length);

  // normally, share access should be removed before calling DokanCheckOplock, see fastfat.
  // calling DokanCheckOplock without removing share access cause the pending create irp which break the oplock resume and return with STATUS_SHARING_VIOLATION.
  IoRemoveShareAccess(RequestContext->IrpSp->FileObject, &fcb->ShareAccess);

  // FsRtlCheckOpLock is called with non-NULL completion routine - not blocking.
  status = DokanCheckOplock(fcb, RequestContext->Irp, eventContext,
                            DokanOplockComplete, DokanPrePostIrp);

  // indicate that DokanExecuteCleanup should not remove share access again. Otherwise the IoCheckShareAccess may return STATUS_SUCCESS in resuming create while the File system application (mirror.exe) will return STATUS_SHARING_VIOLATION.
  RequestContext->RemovedShareAccessBeforeCheckOplock = TRUE;

  DokanFCBUnlock(fcb);

  //
  //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
  //  to service an oplock break and we need to leave now.
  //
  if (status != STATUS_SUCCESS) {
    if (status == STATUS_PENDING) {
      DOKAN_LOG_FINE_IRP(RequestContext,
                         "FsRtlCheckOplock returned STATUS_PENDING");
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
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p",
                     RequestContext->IrpSp->FileObject);

  DokanExecuteCleanup(RequestContext);

  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
}
