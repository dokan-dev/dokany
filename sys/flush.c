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
DokanDispatchFlush(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;
  PFILE_OBJECT fileObject;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PDokanFCB fcb = NULL;
  PDokanCCB ccb;
  PDokanVCB vcb;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;

  __try {
    DOKAN_LOG_BEGIN_MJ(Irp);
    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;
    DOKAN_LOG_FINE_IRP(Irp, "FileObject=%p", fileObject);

    if (fileObject == NULL) {
      status = STATUS_SUCCESS;
      __leave;
    }

    vcb = DeviceObject->DeviceExtension;
    if (GetIdentifierType(vcb) != VCB ||
        !DokanCheckCCB(Irp, vcb->Dcb, fileObject->FsContext2)) {
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
    eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    DOKAN_LOG_FINE_IRP(Irp, "Get Context %X", (ULONG)ccb->UserContext);

    // copy file name to be flushed
    eventContext->Operation.Flush.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Flush.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    CcUninitializeCacheMap(fileObject, NULL, NULL);

    // FsRtlCheckOpLock is called with non-NULL completion routine - not blocking.
    status = DokanCheckOplock(fcb, Irp, eventContext, DokanOplockComplete,
                              DokanPrePostIrp);

    //
    //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
    //  to service an oplock break and we need to leave now.
    //
    if (status != STATUS_SUCCESS) {
      if (status == STATUS_PENDING) {
        DOKAN_LOG_FINE_IRP(Irp, "FsRtlCheckOplock returned STATUS_PENDING");
      } else {
        DokanFreeEventContext(eventContext);
      }
      __leave;
    }

    // register this IRP to waiting IRP list and make it pending status
    status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);

  } __finally {
    if (fcb)
      DokanFCBUnlock(fcb);
    DOKAN_LOG_END_MJ(Irp, status, 0);
    DokanCompleteIrpRequest(Irp, status, 0);
  }

  return status;
}

VOID DokanCompleteFlush(__in PIRP_ENTRY IrpEntry,
                        __in PEVENT_INFORMATION EventInfo) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;
  fileObject = irpSp->FileObject;

  DOKAN_LOG_BEGIN_MJ(irp);
  DOKAN_LOG_FINE_IRP(irp, "FileObject=%p", fileObject);

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  ccb->UserContext = EventInfo->Context;
  DOKAN_LOG_FINE_IRP(irp, "Set Context %X", (ULONG)ccb->UserContext);

  DOKAN_LOG_END_MJ(irp, EventInfo->Status, 0);
  DokanCompleteIrpRequest(irp, EventInfo->Status, 0);
}
