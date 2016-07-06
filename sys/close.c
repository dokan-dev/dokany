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
DokanDispatchClose(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp)

/*++

Routine Description:

        This device control dispatcher handles create & close IRPs.

Arguments:

        DeviceObject - Context for the activity.
        Irp 		 - The device control argument block.

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

  __try {

    DDbgPrint("==> DokanClose\n");

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;

    if (fileObject == NULL) {
      DDbgPrint("  fileObject is NULL\n");
      status = STATUS_SUCCESS;
      __leave;
    }

    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
    DokanPrintFileName(fileObject);

    vcb = DeviceObject->DeviceExtension;
    if (vcb == NULL) {
      DDbgPrint("  No device extension\n");
      status = STATUS_SUCCESS;
      __leave;
    }

    if (GetIdentifierType(vcb) != VCB ||
        !DokanCheckCCB(vcb->Dcb, fileObject->FsContext2)) {

      if (fileObject->FsContext2) {
        ccb = fileObject->FsContext2;
        ASSERT(ccb != NULL);

        fcb = ccb->Fcb;
        ASSERT(fcb != NULL);

        DDbgPrint("   Free CCB:%p\n", ccb);
        DokanFreeCCB(ccb);

        DokanFreeFCB(fcb);

        fileObject->FsContext2 = NULL;
      }

      status = STATUS_SUCCESS;
      __leave;
    }

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
    eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

    if (eventContext == NULL) {
      // status = STATUS_INSUFFICIENT_RESOURCES;
      DDbgPrint("   eventContext == NULL\n");
      DDbgPrint("   Free CCB:%p\n", ccb);
      DokanFreeCCB(ccb);
      DokanFreeFCB(fcb);
      status = STATUS_SUCCESS;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    DDbgPrint("   UserContext:%X\n", (ULONG)ccb->UserContext);

    // copy the file name to be closed
    eventContext->Operation.Close.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Close.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    DDbgPrint("   Free CCB:%p\n", ccb);
    DokanFreeCCB(ccb);

    DokanFreeFCB(fcb);

    // Close can not be pending status
    // don't register this IRP
    // status = DokanRegisterPendingIrp(DeviceObject, Irp,
    // eventContext->SerialNumber, 0);

    // inform it to user-mode
    DokanEventNotification(&vcb->Dcb->NotifyEvent, eventContext);

    status = STATUS_SUCCESS;

  } __finally {

    DokanCompleteIrpRequest(Irp, status, 0);

    DDbgPrint("<== DokanClose\n");
  }

  return status;
}
