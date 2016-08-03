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
DokanDispatchRead(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp)

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
  PIO_STACK_LOCATION irpSp;
  PFILE_OBJECT fileObject;
  ULONG bufferLength;
  LARGE_INTEGER byteOffset;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  ULONG readLength = 0;
  PDokanCCB ccb;
  PDokanFCB fcb;
  PDokanVCB vcb;
  PVOID currentAddress = NULL;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;
  BOOLEAN isPagingIo = FALSE;
  BOOLEAN isSynchronousIo = FALSE;
  BOOLEAN noCache = FALSE;

  __try {

    DDbgPrint("==> DokanRead\n");

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;

    //
    //  If this is a zero length read then return SUCCESS immediately.
    //
    if (irpSp->Parameters.Read.Length == 0) {
      DDbgPrint("  Parameters.Read.Length == 0 \n");
      Irp->IoStatus.Information = 0;
      status = STATUS_SUCCESS;
      __leave;
    }

    if (irpSp->MinorFunction == IRP_MN_COMPLETE) {
      Irp->MdlAddress = NULL;
      status = STATUS_SUCCESS;
      __leave;
    }

    if (fileObject == NULL && Irp->MdlAddress != NULL) {
      DDbgPrint("  Reads by File System Recognizers\n");

      currentAddress = MmGetSystemAddressForMdlNormalSafe(Irp->MdlAddress);
      if (currentAddress == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }

      // here we could return the bootsector. If we don't have one
      // the requested read lenght must be returned as requested
      readLength = irpSp->Parameters.Read.Length;
      status = STATUS_SUCCESS;
      __leave;
    }

    if (fileObject == NULL) {
      DDbgPrint("  fileObject == NULL\n");
      status = STATUS_INVALID_DEVICE_REQUEST;
      __leave;
    }

    vcb = DeviceObject->DeviceExtension;
    if (GetIdentifierType(vcb) != VCB ||
        !DokanCheckCCB(vcb->Dcb, fileObject->FsContext2)) {
      status = STATUS_INVALID_DEVICE_REQUEST;
      __leave;
    }

    bufferLength = irpSp->Parameters.Read.Length;
    if (irpSp->Parameters.Read.ByteOffset.LowPart ==
            FILE_USE_FILE_POINTER_POSITION &&
        irpSp->Parameters.Read.ByteOffset.HighPart == -1) {

      // irpSp->Parameters.Read.ByteOffset == NULL don't need check?

      DDbgPrint("use FileObject ByteOffset\n");

      byteOffset = fileObject->CurrentByteOffset;

    } else {
      byteOffset = irpSp->Parameters.Read.ByteOffset;
    }

    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
    DokanPrintFileName(fileObject);
    DDbgPrint("  ByteCount:%lu ByteOffset:%I64d\n", bufferLength,
              byteOffset.QuadPart);

    if (bufferLength == 0) {
      status = STATUS_SUCCESS;
      readLength = 0;
      __leave;
    }

    // make a MDL for UserBuffer that can be used later on another thread
    // context
    if (Irp->MdlAddress == NULL) {
      status = DokanAllocateMdl(Irp, irpSp->Parameters.Read.Length);
      if (!NT_SUCCESS(status)) {
        __leave;
      }
    }

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    if (fcb->Flags & DOKAN_FILE_DIRECTORY) {
      DDbgPrint("   DOKAN_FILE_DIRECTORY %p\n", fcb);
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    if (Irp->Flags & IRP_PAGING_IO) {
      isPagingIo = TRUE;
    }
    if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
      isSynchronousIo = TRUE;
    }

    if (Irp->Flags & IRP_NOCACHE) {
      noCache = TRUE;
    }

    if (!isPagingIo && (fileObject->SectionObjectPointer != NULL) &&
        (fileObject->SectionObjectPointer->DataSectionObject != NULL)) {
      ExAcquireResourceExclusiveLite(&fcb->PagingIoResource, TRUE);
      CcFlushCache(&fcb->SectionObjectPointers,
                   &irpSp->Parameters.Read.ByteOffset,
                   irpSp->Parameters.Read.Length, NULL);
      ExReleaseResourceLite(&fcb->PagingIoResource);
    }

    // length of EventContext is sum of file name length and itself
    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;

    eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);
    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    // DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);

    if (isPagingIo) {
      DDbgPrint("  Paging IO\n");
      eventContext->FileFlags |= DOKAN_PAGING_IO;
    }
    if (isSynchronousIo) {
      DDbgPrint("  Synchronous IO\n");
      eventContext->FileFlags |= DOKAN_SYNCHRONOUS_IO;
    }

    if (noCache) {
      DDbgPrint("  Nocache\n");
      eventContext->FileFlags |= DOKAN_NOCACHE;
    }

    // offset of file to read
    eventContext->Operation.Read.ByteOffset = byteOffset;

    // buffer size for read
    // user-mode file system application can return this size
    eventContext->Operation.Read.BufferLength = irpSp->Parameters.Read.Length;

    // copy the accessed file name
    eventContext->Operation.Read.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Read.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    //
    //  We now check whether we can proceed based on the state of
    //  the file oplocks.
    //
    if (!FlagOn(Irp->Flags, IRP_PAGING_IO)) {
      status = FsRtlCheckOplock(DokanGetFcbOplock(fcb), Irp, eventContext,
                                DokanOplockComplete, DokanPrePostIrp);

      //
      //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
      //  to service an oplock break and we need to leave now.
      //
      if (status != STATUS_SUCCESS) {
        if (status == STATUS_PENDING) {
          DDbgPrint("   FsRtlCheckOplock returned STATUS_PENDING\n");
        } else {
          DokanFreeEventContext(eventContext);
        }
        __leave;
      }

      //
      // We have to check for read access according to the current
      // state of the file locks, and set FileSize from the Fcb.
      //
      if (!FsRtlCheckLockForReadAccess(&fcb->FileLock, Irp)) {
        status = STATUS_FILE_LOCK_CONFLICT;
        __leave;
      }
    }

    // register this IRP to pending IPR list and make it pending status
    status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);
  } __finally {

    DokanCompleteIrpRequest(Irp, status, readLength);

    DDbgPrint("<== DokanRead\n");
  }

  return status;
}

VOID DokanCompleteRead(__in PIRP_ENTRY IrpEntry,
                       __in PEVENT_INFORMATION EventInfo) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_SUCCESS;
  ULONG readLength = 0;
  ULONG bufferLen = 0;
  PVOID buffer = NULL;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;

  fileObject = IrpEntry->FileObject;
  ASSERT(fileObject != NULL);

  DDbgPrint("==> DokanCompleteRead %wZ\n", &fileObject->FileName);

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  ccb->UserContext = EventInfo->Context;
  // DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

  // buffer which is used to copy Read info
  if (irp->MdlAddress) {
    // DDbgPrint("   use MDL Address\n");
    buffer = MmGetSystemAddressForMdlNormalSafe(irp->MdlAddress);
  } else {
    // DDbgPrint("   use UserBuffer\n");
    buffer = irp->UserBuffer;
  }

  // available buffer size
  bufferLen = irpSp->Parameters.Read.Length;

  DDbgPrint("  bufferLen %d, Event.BufferLen %d\n", bufferLen,
            EventInfo->BufferLength);

  // buffer is not specified or short of length
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {

    readLength = 0;
    status = STATUS_INSUFFICIENT_RESOURCES;

  } else {
    RtlZeroMemory(buffer, bufferLen);
    RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

    // read length which is actually read
    readLength = EventInfo->BufferLength;
    status = EventInfo->Status;

    if (NT_SUCCESS(status) && EventInfo->BufferLength > 0 &&
        (fileObject->Flags & FO_SYNCHRONOUS_IO) &&
        !(irp->Flags & IRP_PAGING_IO)) {
      // update current byte offset only when synchronous IO and not pagind IO
      fileObject->CurrentByteOffset.QuadPart =
          EventInfo->Operation.Read.CurrentByteOffset.QuadPart;
      DDbgPrint("  Updated CurrentByteOffset %I64d\n",
                fileObject->CurrentByteOffset.QuadPart);
    }
  }

  if (NT_SUCCESS(status)) {
    DDbgPrint("  STATUS_SUCCESS\n");
  } else if (status == STATUS_INSUFFICIENT_RESOURCES) {
    DDbgPrint("  STATUS_INSUFFICIENT_RESOURCES\n");
  } else if (status == STATUS_END_OF_FILE) {
    DDbgPrint("  STATUS_END_OF_FILE\n");
  } else {
    DDbgPrint("  status = 0x%X\n", status);
  }

  DokanCompleteIrpRequest(irp, status, readLength);

  DDbgPrint("<== DokanCompleteRead\n");
}
