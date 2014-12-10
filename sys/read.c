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
DokanDispatchRead(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)

/*++

Routine Description:

	This device control dispatcher handles read IRPs.

Arguments:

	DeviceObject - Context for the activity.
	Irp 		 - The device control argument block.

Return Value:

	NTSTATUS

--*/
{
	PIO_STACK_LOCATION	irpSp;
	PFILE_OBJECT		fileObject;
	ULONG				bufferLength;
	LARGE_INTEGER		byteOffset;
	PVOID				buffer;
	NTSTATUS			status = STATUS_INVALID_PARAMETER;
	ULONG				readLength = 0;
	PDokanCCB			ccb;
	PDokanFCB			fcb;
	PDokanVCB			vcb;
	PEVENT_CONTEXT		eventContext;
	ULONG				eventLength;

	PAGED_CODE();

	__try {

		FsRtlEnterFileSystem();

		DDbgPrint("==> DokanRead\n");

		irpSp		= IoGetCurrentIrpStackLocation(Irp);
		fileObject	= irpSp->FileObject;

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

	    bufferLength = irpSp->Parameters.Read.Length;
		if (irpSp->Parameters.Read.ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION &&
			irpSp->Parameters.Read.ByteOffset.HighPart == -1) {

			// irpSp->Parameters.Read.ByteOffset == NULL don't need check?
	
			DDbgPrint("use FileObject ByteOffset\n");
			
			byteOffset = fileObject->CurrentByteOffset;
		
		} else {
			byteOffset	 = irpSp->Parameters.Read.ByteOffset;
		}

		DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
		DokanPrintFileName(fileObject);
		DDbgPrint("  ByteCount:%d ByteOffset:%d\n", bufferLength, byteOffset);

		if (bufferLength == 0) {
			status = STATUS_SUCCESS;
			readLength = 0;
			__leave;
		}

		// make a MDL for UserBuffer that can be used later on another thread context
		if (Irp->MdlAddress == NULL) {
			status = DokanAllocateMdl(Irp,  irpSp->Parameters.Read.Length);
			if (!NT_SUCCESS(status)) {
				__leave;
			}
		}

		ccb	= fileObject->FsContext2;
		ASSERT(ccb != NULL);

		fcb	= ccb->Fcb;
		ASSERT(fcb != NULL);

		if (fcb->Flags & DOKAN_FILE_DIRECTORY) {
			DDbgPrint("   DOKAN_FILE_DIRECTORY %p\n", fcb);
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		// length of EventContext is sum of file name length and itself
		eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;

		eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);
		if (eventContext == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}

		eventContext->Context = ccb->UserContext;
		//DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);

		if (Irp->Flags & IRP_PAGING_IO) {
			DDbgPrint("  Paging IO\n");
			eventContext->FileFlags |= DOKAN_PAGING_IO;
		}
		if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
			DDbgPrint("  Synchronous IO\n");
			eventContext->FileFlags |= DOKAN_SYNCHRONOUS_IO;
		}

		if (Irp->Flags & IRP_NOCACHE) {
			DDbgPrint("  Nocache\n");
			eventContext->FileFlags |= DOKAN_NOCACHE;
		}

		// offset of file to read
		eventContext->Read.ByteOffset = byteOffset;

		// buffer size for read
		// user-mode file system application can return this size
		eventContext->Read.BufferLength = irpSp->Parameters.Read.Length;

		// copy the accessed file name
		eventContext->Read.FileNameLength = fcb->FileName.Length;
		RtlCopyMemory(eventContext->Read.FileName, fcb->FileName.Buffer, fcb->FileName.Length);


		// register this IRP to pending IPR list and make it pending status
		status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);		
	} __finally {

		// if IRP status is not pending, must complete current IRP
		if (status != STATUS_PENDING) {
			Irp->IoStatus.Status = status;
			Irp->IoStatus.Information = readLength;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			DokanPrintNTStatus(status);
		} else {
			DDbgPrint("  STATUS_PENDING\n");
		}

		DDbgPrint("<== DokanRead\n");
		
		FsRtlExitFileSystem();

	}

	return status;
}



VOID
DokanCompleteRead(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo
	)
{
	PIRP				irp;
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status     = STATUS_SUCCESS;
	ULONG				readLength = 0;
	ULONG				bufferLen  = 0;
	PVOID				buffer	   = NULL;
	PDokanCCB			ccb;
	PFILE_OBJECT		fileObject;

	fileObject = IrpEntry->FileObject;
	ASSERT(fileObject != NULL);

	DDbgPrint("==> DokanCompleteRead %wZ\n", &fileObject->FileName);

	irp   = IrpEntry->Irp;
	irpSp = IrpEntry->IrpSp;	

	ccb = fileObject->FsContext2;
	ASSERT(ccb != NULL);

	ccb->UserContext = EventInfo->Context;
	// DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

	// buffer which is used to copy Read info
	if (irp->MdlAddress) {
		//DDbgPrint("   use MDL Address\n");
		buffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
	} else {
		//DDbgPrint("   use UserBuffer\n");
		buffer	= irp->UserBuffer;
	}

	// available buffer size
	bufferLen = irpSp->Parameters.Read.Length;

	DDbgPrint("  bufferLen %d, Event.BufferLen %d\n", bufferLen, EventInfo->BufferLength);

	// buffer is not specified or short of length
	if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {
	
		readLength  = 0;
		status		= STATUS_INSUFFICIENT_RESOURCES;
		
	} else {
		RtlZeroMemory(buffer, bufferLen);
		RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

		// read length which is actually read
		readLength = EventInfo->BufferLength;
		status = EventInfo->Status;

		if (NT_SUCCESS(status) &&
			 EventInfo->BufferLength > 0 &&
			 (fileObject->Flags & FO_SYNCHRONOUS_IO) &&
			!(irp->Flags & IRP_PAGING_IO)) {
			// update current byte offset only when synchronous IO and not pagind IO
			fileObject->CurrentByteOffset.QuadPart =
				EventInfo->Read.CurrentByteOffset.QuadPart;
			DDbgPrint("  Updated CurrentByteOffset %I64d\n",
				fileObject->CurrentByteOffset.QuadPart); 
		}
	}

	if (status == STATUS_SUCCESS) {
		DDbgPrint("  STATUS_SUCCESS\n");
	} else if (status == STATUS_INSUFFICIENT_RESOURCES) {
		DDbgPrint("  STATUS_INSUFFICIENT_RESOURCES\n");
	} else if (status == STATUS_END_OF_FILE) {
		DDbgPrint("  STATUS_END_OF_FILE\n");
	} else {
		DDbgPrint("  status = 0x%X\n", status);
	}

	irp->IoStatus.Status = status;
	irp->IoStatus.Information = readLength;
	IoCompleteRequest(irp, IO_NO_INCREMENT);

	DDbgPrint("<== DokanCompleteRead\n");
}


