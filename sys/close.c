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
#include "util/fcb.h"

// Unlinks and frees Ccb while caller holds Fcb->MainResource exclusive,
// unlocks Fcb, releases the FCB reference, clears FileObject contexts, and
// requests deferred OpenFileCount decrement.
static VOID DokanCloseFileObject(__in PREQUEST_CONTEXT RequestContext,
                                 __in PFILE_OBJECT FileObject,
                                 __in PDokanFCB Fcb, __in PDokanCCB Ccb) {
  DokanFreeCcbLocked(RequestContext, Ccb);
  DokanFCBUnlock(Fcb);
  DokanFreeFCB(RequestContext->Vcb, Fcb);
  FileObject->FsContext = NULL;
  FileObject->FsContext2 = NULL;
  RequestContext->DecrementOpenFileCount = TRUE;
}

NTSTATUS
DokanDispatchClose(__in PREQUEST_CONTEXT RequestContext)

/*++

Routine Description:

        This device control dispatcher handles create & close IRPs.

Arguments:

        DeviceObject - Context for the activity.
        Irp          - The device control argument block.

Return Value:

        NTSTATUS

--*/
{
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;
  PDokanFCB fcb;
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject,
                    IRP_MJ_CLOSE);

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  if (fileObject == NULL || RequestContext->Vcb == NULL) {
    return STATUS_SUCCESS;
  }

  if (!fileObject->FsContext2) {
    // Direct volume open (VolumeFileHeader) or pre-HasEventWait open
    // (PreStartFileHeader).
    if (fileObject->FsContext == &RequestContext->Vcb->VolumeFileHeader ||
        fileObject->FsContext == &RequestContext->Vcb->PreStartFileHeader) {
      fileObject->FsContext = NULL;
      RequestContext->DecrementOpenFileCount = TRUE;
    }
    return STATUS_SUCCESS;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  if (!DokanCheckCCB(RequestContext, ccb)) {
    DokanFCBLockRW(fcb);
    DokanCloseFileObject(RequestContext, fileObject, fcb, ccb);
    return STATUS_SUCCESS;
  }

  OplockDebugRecordMajorFunction(fcb, IRP_MJ_CLOSE);
  DokanFCBLockRW(fcb);
  if (fcb->BlockUserModeDispatch) {
    DokanLogInfo(&logger,
                 L"Closed file with user mode dispatch blocked: \"%wZ\"",
                 &fcb->FileName);
    DokanCloseFileObject(RequestContext, fileObject, fcb, ccb);
    return STATUS_SUCCESS;
  }

  eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
  eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

  if (eventContext == NULL) {
    // status = STATUS_INSUFFICIENT_RESOURCES;
    DOKAN_LOG_FINE_IRP(RequestContext, "EventContext == NULL");
    DokanCloseFileObject(RequestContext, fileObject, fcb, ccb);
    return STATUS_SUCCESS;
  }

  eventContext->Context = ccb->UserContext;
  DOKAN_LOG_FINE_IRP(RequestContext, "UserContext:%X", (ULONG)ccb->UserContext);

  // copy the file name to be closed
  eventContext->Operation.Close.FileNameLength = fcb->FileName.Length;
  RtlCopyMemory(eventContext->Operation.Close.FileName, fcb->FileName.Buffer,
                fcb->FileName.Length);

  DokanCloseFileObject(RequestContext, fileObject, fcb, ccb);

  // Close can not be pending status don't register this IRP

  // inform it to user-mode
  DokanEventNotification(RequestContext, &RequestContext->Dcb->NotifyEvent, eventContext);

  return STATUS_SUCCESS;
}
