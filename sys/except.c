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
DokanExceptionFilter(__in PIRP Irp, __in PEXCEPTION_POINTERS ExceptionPointer) {
  UNREFERENCED_PARAMETER(Irp);

  NTSTATUS Status = EXCEPTION_CONTINUE_SEARCH;
  NTSTATUS ExceptionCode;
  PEXCEPTION_RECORD ExceptRecord;

  ExceptRecord = ExceptionPointer->ExceptionRecord;
  ExceptionCode = ExceptRecord->ExceptionCode;

  DbgPrint("-------------------------------------------------------------\n");
  DbgPrint("Exception happends in Dokan (code %xh):\n", ExceptionCode);
  DbgPrint(".exr %p;.cxr %p;\n", ExceptionPointer->ExceptionRecord,
           ExceptionPointer->ContextRecord);
  DbgPrint("-------------------------------------------------------------\n");

  DbgBreakPoint();

  if (FsRtlIsNtstatusExpected(ExceptionCode)) {
    //
    // If the exception is expected execute our handler
    //

    DDbgPrint("DokanExceptionFilter: Catching exception %xh\n", ExceptionCode);

    Status = EXCEPTION_EXECUTE_HANDLER;

  } else {

    //
    // Continue search for an higher level exception handler
    //

    DDbgPrint("DokanExceptionFilter: Passing on exception %#x\n",
              ExceptionCode);

    Status = EXCEPTION_CONTINUE_SEARCH;
  }

  return Status;
}

NTSTATUS
DokanExceptionHandler(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp,
                      __in NTSTATUS ExceptionCode) {
  NTSTATUS Status;

  Status = ExceptionCode;

  if (Irp) {

    PDokanVCB Vcb = NULL;
    PIO_STACK_LOCATION IrpSp;

    IrpSp = IoGetCurrentIrpStackLocation(Irp);
    Vcb = (PDokanVCB)DeviceObject->DeviceExtension;

    if (NULL == Vcb) {
      Status = STATUS_INVALID_PARAMETER;
    } else if (Vcb->Identifier.Type != VCB) {
      Status = STATUS_INVALID_PARAMETER;
    } else if (IsUnmountPendingVcb(Vcb)) {
      Status = STATUS_NO_SUCH_DEVICE;
    }

    if (Status == STATUS_PENDING) {
      goto errorout;
    }

    DokanCompleteIrpRequest(Irp, Status, 0);
  }

  else {

    Status = STATUS_INVALID_PARAMETER;
  }

errorout:

  return Status;
}
