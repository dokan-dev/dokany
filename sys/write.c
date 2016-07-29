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
DokanDispatchWrite(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;
  PFILE_OBJECT fileObject;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;
  PDokanCCB ccb;
  PDokanFCB fcb;
  PDokanVCB vcb;
  PVOID buffer;
  BOOLEAN writeToEoF = FALSE;
  BOOLEAN isPagingIo = FALSE;
  BOOLEAN isNonCached = FALSE;
  BOOLEAN isSynchronousIo = FALSE;

  __try {

    DDbgPrint("==> DokanWrite\n");

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;

    //
    //  If this is a zero length write then return SUCCESS immediately.
    //

    if (irpSp->Parameters.Write.Length == 0) {
      DDbgPrint("  Parameters.Write.Length == 0\n");
      Irp->IoStatus.Information = 0;
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
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
    DokanPrintFileName(fileObject);

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    if (fcb->Flags & DOKAN_FILE_DIRECTORY) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    if (Irp->MdlAddress) {
      DDbgPrint("  use MdlAddress\n");
      buffer = MmGetSystemAddressForMdlNormalSafe(Irp->MdlAddress);
    } else {
      DDbgPrint("  use UserBuffer\n");
      buffer = Irp->UserBuffer;
    }

    if (buffer == NULL) {
      DDbgPrint("  buffer == NULL\n");
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    if (irpSp->Parameters.Write.ByteOffset.LowPart ==
            FILE_WRITE_TO_END_OF_FILE &&
        irpSp->Parameters.Write.ByteOffset.HighPart == -1) {
      writeToEoF = TRUE;
    }

    if (Irp->Flags & IRP_PAGING_IO) {
      isPagingIo = TRUE;
    }

    if (Irp->Flags & IRP_NOCACHE) {
      isNonCached = TRUE;
    }

    if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
      isSynchronousIo = TRUE;
    }

    if (!isPagingIo && (fileObject->SectionObjectPointer != NULL) &&
        (fileObject->SectionObjectPointer->DataSectionObject != NULL)) {
      ExAcquireResourceExclusiveLite(&fcb->PagingIoResource, TRUE);
      CcFlushCache(&fcb->SectionObjectPointers,
                   writeToEoF ? NULL : &irpSp->Parameters.Write.ByteOffset,
                   irpSp->Parameters.Write.Length, NULL);
      CcPurgeCacheSection(&fcb->SectionObjectPointers,
                          writeToEoF ? NULL
                                     : &irpSp->Parameters.Write.ByteOffset,
                          irpSp->Parameters.Write.Length, FALSE);
      ExReleaseResourceLite(&fcb->PagingIoResource);
    }

    // Cannot write at end of the file when using paging IO
    if (writeToEoF && isPagingIo) {
      DDbgPrint("  writeToEoF & isPagingIo\n");
      Irp->IoStatus.Information = 0;
      status = STATUS_SUCCESS;
      __leave;
    }

    // the length of EventContext is sum of length to write and length of file
    // name
    eventLength = sizeof(EVENT_CONTEXT) + irpSp->Parameters.Write.Length +
                  fcb->FileName.Length;

    eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

    // no more memory!
    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    // DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);

    // When the length is bigger than usual event notitfication buffer,
    // saves pointer in DiverContext to copy EventContext after allocating
    // more bigger memory.
    Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = eventContext;

    if (isPagingIo) {
      DDbgPrint("  Paging IO\n");
      eventContext->FileFlags |= DOKAN_PAGING_IO;
    }
    if (isSynchronousIo) {
      DDbgPrint("  Synchronous IO\n");
      eventContext->FileFlags |= DOKAN_SYNCHRONOUS_IO;
    }
    if (isNonCached) {
      DDbgPrint("  Nocache\n");
      eventContext->FileFlags |= DOKAN_NOCACHE;
    }

    // offset of file to write
    eventContext->Operation.Write.ByteOffset =
        irpSp->Parameters.Write.ByteOffset;

    if (writeToEoF) {
      eventContext->FileFlags |= DOKAN_WRITE_TO_END_OF_FILE;
      DDbgPrint("  WriteOffset = end of file\n");
    }

    if (isSynchronousIo &&
        ((irpSp->Parameters.Write.ByteOffset.LowPart ==
          FILE_USE_FILE_POINTER_POSITION) &&
         (irpSp->Parameters.Write.ByteOffset.HighPart == -1))) {
      // NOTE:
      // http://msdn.microsoft.com/en-us/library/ms795960.aspx
      // Do not check IrpSp->Parameters.Write.ByteOffset.QuadPart == 0
      // Probably the document is wrong.
      eventContext->Operation.Write.ByteOffset.QuadPart =
          fileObject->CurrentByteOffset.QuadPart;
    }

    // the size of buffer to write
    eventContext->Operation.Write.BufferLength = irpSp->Parameters.Write.Length;

    // the offset from the begining of structure
    // the contents to write will be copyed to this offset
    eventContext->Operation.Write.BufferOffset =
        FIELD_OFFSET(EVENT_CONTEXT, Operation.Write.FileName[0]) +
        fcb->FileName.Length + sizeof(WCHAR); // adds last null char

    // copies the content to write to EventContext
    RtlCopyMemory((PCHAR)eventContext +
                      eventContext->Operation.Write.BufferOffset,
                  buffer, irpSp->Parameters.Write.Length);

    // copies file name
    eventContext->Operation.Write.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Write.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    // When eventlength is less than event notification buffer,
    // returns it to user-mode using pending event.
    if (eventLength <= EVENT_CONTEXT_MAX_SIZE) {

      DDbgPrint("   Offset %d:%d, Length %d\n",
                irpSp->Parameters.Write.ByteOffset.HighPart,
                irpSp->Parameters.Write.ByteOffset.LowPart,
                irpSp->Parameters.Write.Length);

      // EventContext is no longer needed, clear it
      Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;

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
      }

      // register this IRP to IRP waiting list and make it pending status
      status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);

      // Resuests bigger memory
      // eventContext will be freed later using
      // Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT]
    } else {
      // the length at lest file name can be stored
      ULONG requestContextLength = max(
          sizeof(EVENT_CONTEXT), eventContext->Operation.Write.BufferOffset);
      PEVENT_CONTEXT requestContext =
          AllocateEventContext(vcb->Dcb, Irp, requestContextLength, ccb);

      // no more memory!
      if (requestContext == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;
        DokanFreeEventContext(eventContext);
        __leave;
      }

      DDbgPrint("   Offset %d:%d, Length %d (request)\n",
                irpSp->Parameters.Write.ByteOffset.HighPart,
                irpSp->Parameters.Write.ByteOffset.LowPart,
                irpSp->Parameters.Write.Length);

      // copies from begining of EventContext to the end of file name
      RtlCopyMemory(requestContext, eventContext,
                    eventContext->Operation.Write.BufferOffset);
      // puts actual size of RequestContext
      requestContext->Length = requestContextLength;
      // requsts enough size to copy EventContext
      requestContext->Operation.Write.RequestLength = eventLength;

      //
      //  We now check whether we can proceed based on the state of
      //  the file oplocks.
      //
      if (!FlagOn(Irp->Flags, IRP_PAGING_IO)) {
        status = FsRtlCheckOplock(DokanGetFcbOplock(fcb), Irp, requestContext,
                                  DokanOplockComplete, DokanPrePostIrp);

        //
        //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
        //  to service an oplock break and we need to leave now.
        //
        if (status != STATUS_SUCCESS) {
          if (status == STATUS_PENDING) {
            DDbgPrint("   FsRtlCheckOplock returned STATUS_PENDING\n");
          } else {
            DokanFreeEventContext(requestContext);
            Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;
            DokanFreeEventContext(eventContext);
          }
          __leave;
        }
      }

      // regiters this IRP to IRP wainting list and make it pending status
      status = DokanRegisterPendingIrp(DeviceObject, Irp, requestContext, 0);
    }

  } __finally {

    DokanCompleteIrpRequest(Irp, status, 0);

    DDbgPrint("<== DokanWrite\n");
  }

  return status;
}

VOID DokanCompleteWrite(__in PIRP_ENTRY IrpEntry,
                        __in PEVENT_INFORMATION EventInfo) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_SUCCESS;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;

  fileObject = IrpEntry->FileObject;
  ASSERT(fileObject != NULL);

  DDbgPrint("==> DokanCompleteWrite %wZ\n", &fileObject->FileName);

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  ccb->UserContext = EventInfo->Context;
  // DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

  status = EventInfo->Status;

  irp->IoStatus.Status = status;
  irp->IoStatus.Information = EventInfo->BufferLength;

  if (NT_SUCCESS(status) && EventInfo->BufferLength != 0 &&
      fileObject->Flags & FO_SYNCHRONOUS_IO && !(irp->Flags & IRP_PAGING_IO)) {
    // update current byte offset only when synchronous IO and not paging IO
    fileObject->CurrentByteOffset.QuadPart =
        EventInfo->Operation.Write.CurrentByteOffset.QuadPart;
    DDbgPrint("  Updated CurrentByteOffset %I64d\n",
              fileObject->CurrentByteOffset.QuadPart);
  }

  DokanCompleteIrpRequest(irp, irp->IoStatus.Status, irp->IoStatus.Information);

  DDbgPrint("<== DokanCompleteWrite\n");
}
