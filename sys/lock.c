/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

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
DokanDispatchLock(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)
{
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status = STATUS_INVALID_PARAMETER;
	PFILE_OBJECT		fileObject;
	PDokanCCB			ccb;
	PDokanFCB			fcb;
	PDokanVCB			vcb;
	PEVENT_CONTEXT		eventContext;
	ULONG				eventLength;

	PAGED_CODE();

	__try {
		FsRtlEnterFileSystem();

		DDbgPrint("==> DokanLock\n");
	
		irpSp = IoGetCurrentIrpStackLocation(Irp);
		fileObject = irpSp->FileObject;

		if (fileObject == NULL) {
			DDbgPrint("  fileObject == NULL\n");
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		vcb = DeviceObject->DeviceExtension;
		if (GetIdentifierType(vcb) != VCB ||
			!DokanCheckCCB(vcb->Dcb, fileObject->FsContext2)) {
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
		DokanPrintFileName(fileObject);

		switch(irpSp->MinorFunction) {
		case IRP_MN_LOCK:
			DDbgPrint("  IRP_MN_LOCK\n");
			break;
		case IRP_MN_UNLOCK_ALL:
			DDbgPrint("  IRP_MN_UNLOCK_ALL\n");
			break;
		case IRP_MN_UNLOCK_ALL_BY_KEY:
			DDbgPrint("  IRP_MN_UNLOCK_ALL_BY_KEY\n");
			break;
		case IRP_MN_UNLOCK_SINGLE:
			DDbgPrint("  IRP_MN_UNLOCK_SINGLE\n");
			break;
		default:
			DDbgPrint("  unknown function : %d\n", irpSp->MinorFunction);
			break;
		}

		ccb = fileObject->FsContext2;
		ASSERT(ccb != NULL);

		fcb = ccb->Fcb;
		ASSERT(fcb != NULL);

		eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
		eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

		if (eventContext == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}

		eventContext->Context = ccb->UserContext;
		DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);

		// copy file name to be locked
		eventContext->Lock.FileNameLength = fcb->FileName.Length;
		RtlCopyMemory(eventContext->Lock.FileName, fcb->FileName.Buffer, fcb->FileName.Length);

		// parameters of Lock
		eventContext->Lock.ByteOffset = irpSp->Parameters.LockControl.ByteOffset;
		if (irpSp->Parameters.LockControl.Length != NULL) {
			eventContext->Lock.Length.QuadPart = irpSp->Parameters.LockControl.Length->QuadPart;
		} else {
			DDbgPrint("  LockControl.Length = NULL\n");
		}
		eventContext->Lock.Key = irpSp->Parameters.LockControl.Key;

		// register this IRP to waiting IRP list and make it pending status
		status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);

	} __finally {

		if (status != STATUS_PENDING) {
			//
			// complete the Irp
			//
			Irp->IoStatus.Status = status;
			Irp->IoStatus.Information = 0;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			DokanPrintNTStatus(status);
		}

		DDbgPrint("<== DokanLock\n");
		FsRtlExitFileSystem();
	}

	return status;
}


VOID
DokanCompleteLock(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo
	)
{
	PIRP				irp;
	PIO_STACK_LOCATION	irpSp;
	PDokanCCB			ccb;
	PFILE_OBJECT		fileObject;
	NTSTATUS			status;

	irp   = IrpEntry->Irp;
	irpSp = IrpEntry->IrpSp;	

	//FsRtlEnterFileSystem();

	DDbgPrint("==> DokanCompleteLock\n");

	fileObject = irpSp->FileObject;
	ccb = fileObject->FsContext2;
	ASSERT(ccb != NULL);

	ccb->UserContext = EventInfo->Context;
	// DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

	status = EventInfo->Status;
	irp->IoStatus.Status = status;
	irp->IoStatus.Information = 0;
	IoCompleteRequest(irp, IO_NO_INCREMENT);

	DokanPrintNTStatus(status);

	DDbgPrint("<== DokanCompleteLock\n");

	//FsRtlExitFileSystem();
}
