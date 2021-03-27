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
#include "util/fcb.h"

NTSTATUS
DokanDispatchClose(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp)

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
  PDokanVCB vcb;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;
  PDokanFCB fcb;
  DOKAN_INIT_LOGGER(logger, DeviceObject->DriverObject, IRP_MJ_CLOSE);

  __try {

    DOKAN_LOG_BEGIN_MJ(Irp);

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;
    DOKAN_LOG_FINE_IRP(Irp, "FileObject=%p", irpSp->FileObject);

    if (fileObject == NULL) {
      status = STATUS_SUCCESS;
      __leave;
    }

    vcb = DeviceObject->DeviceExtension;
    if (vcb == NULL) {
      status = STATUS_SUCCESS;
      __leave;
    }

    if (GetIdentifierType(vcb) != VCB ||
        !DokanCheckCCB(Irp, vcb->Dcb, fileObject->FsContext2)) {

      if (fileObject->FsContext2) {
        ccb = fileObject->FsContext2;
        ASSERT(ccb != NULL);

        fcb = ccb->Fcb;
        ASSERT(fcb != NULL);

        DokanFCBLockRW(fcb);
        DokanFreeCCB(Irp, ccb);
        DokanFCBUnlock(fcb);

        DokanFreeFCB(vcb, fcb);

        fileObject->FsContext2 = NULL;
      }

      status = STATUS_SUCCESS;
      __leave;
    }

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    OplockDebugRecordMajorFunction(fcb, IRP_MJ_CLOSE);
    DokanFCBLockRW(fcb);
    if (fcb->BlockUserModeDispatch) {
      DokanLogInfo(&logger, L"Closed file with user mode dispatch blocked: \"%wZ\"",
                   &fcb->FileName);
      DokanFreeCCB(Irp, ccb);
      DokanFCBUnlock(fcb);
      DokanFreeFCB(vcb, fcb);
      status = STATUS_SUCCESS;
      __leave;
    }

    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
    eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

    if (eventContext == NULL) {
      // status = STATUS_INSUFFICIENT_RESOURCES;
      DOKAN_LOG_FINE_IRP(Irp, "EventContext == NULL");
      DokanFreeCCB(Irp, ccb);
      DokanFCBUnlock(fcb);
      DokanFreeFCB(vcb, fcb);
      status = STATUS_SUCCESS;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    DOKAN_LOG_FINE_IRP(Irp, "UserContext:%X", (ULONG)ccb->UserContext);

    // copy the file name to be closed
    eventContext->Operation.Close.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Close.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    DokanFreeCCB(Irp, ccb);
    DokanFCBUnlock(fcb);
    DokanFreeFCB(vcb, fcb);

    // Close can not be pending status
    // don't register this IRP
    // status = DokanRegisterPendingIrp(DeviceObject, Irp,
    // eventContext->SerialNumber, 0);

    // inform it to user-mode
    DokanEventNotification(&vcb->Dcb->NotifyEvent, eventContext);

    status = STATUS_SUCCESS;

  } __finally {
    DOKAN_LOG_END_MJ(Irp, status, 0);
    DokanCompleteIrpRequest(Irp, status, 0);
  }

  return status;
}
