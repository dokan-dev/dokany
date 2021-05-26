/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2021 Google, Inc.
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
DokanDispatchFlush(__in PREQUEST_CONTEXT RequestContext) {
  PFILE_OBJECT fileObject;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PDokanFCB fcb = NULL;
  PDokanCCB ccb;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;

  __try {
    fileObject = RequestContext->IrpSp->FileObject;
    DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

    if (fileObject == NULL || !RequestContext->Vcb ||
        !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
      status = STATUS_SUCCESS;
      __leave;
    }

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);
    OplockDebugRecordMajorFunction(fcb, IRP_MJ_FLUSH_BUFFERS);
    DokanFCBLockRO(fcb);

    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
    eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    DOKAN_LOG_FINE_IRP(RequestContext, "Get Context %X", (ULONG)ccb->UserContext);

    // copy file name to be flushed
    eventContext->Operation.Flush.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Flush.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    CcUninitializeCacheMap(fileObject, NULL, NULL);

    // FsRtlCheckOpLock is called with non-NULL completion routine - not blocking.
    status = DokanCheckOplock(fcb, RequestContext->Irp, eventContext,
                              DokanOplockComplete,
                              DokanPrePostIrp);

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
      __leave;
    }

    // register this IRP to waiting IRP list and make it pending status
    status = DokanRegisterPendingIrp(RequestContext, eventContext);

  } __finally {
    if (fcb)
      DokanFCBUnlock(fcb);
  }

  return status;
}

VOID DokanCompleteFlush(__in PREQUEST_CONTEXT RequestContext,
                        __in PEVENT_INFORMATION EventInfo) {
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;

  fileObject = RequestContext->IrpSp->FileObject;

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  ccb->UserContext = EventInfo->Context;
  DOKAN_LOG_FINE_IRP(RequestContext, "Set Context %X", (ULONG)ccb->UserContext);

  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
}
