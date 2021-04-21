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

NTSTATUS
DokanDispatchWrite(__in PREQUEST_CONTEXT RequestContext) {
  PFILE_OBJECT fileObject;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  PVOID buffer;
  BOOLEAN writeToEoF = FALSE;
  BOOLEAN isPagingIo = FALSE;
  BOOLEAN isNonCached = FALSE;
  BOOLEAN isSynchronousIo = FALSE;
  BOOLEAN fcbLocked = FALSE;

  __try {
    fileObject = RequestContext->IrpSp->FileObject;
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "FileObject=%p MdlAddress=%p UserBuffer=%p Length=%ld ByteOffset=%I64u",
        fileObject, RequestContext->Irp->MdlAddress,
        RequestContext->Irp->UserBuffer,
        RequestContext->IrpSp->Parameters.Write.Length,
        RequestContext->IrpSp->Parameters.Write.ByteOffset.QuadPart);
    //
    //  If this is a zero length write then return SUCCESS immediately.
    //

    if (RequestContext->IrpSp->Parameters.Write.Length == 0) {
      status = STATUS_SUCCESS;
      __leave;
    }

    if (fileObject == NULL || !RequestContext->Vcb ||
        !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject,
                      IRP_MJ_WRITE);

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    OplockDebugRecordMajorFunction(fcb, IRP_MJ_WRITE);
    if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    if (RequestContext->Irp->MdlAddress) {
      buffer =
          MmGetSystemAddressForMdlNormalSafe(RequestContext->Irp->MdlAddress);
    } else {
      buffer = RequestContext->Irp->UserBuffer;
    }

    if (buffer == NULL) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    if (RequestContext->IrpSp->Parameters.Write.ByteOffset.LowPart ==
            FILE_WRITE_TO_END_OF_FILE &&
        RequestContext->IrpSp->Parameters.Write.ByteOffset.HighPart == -1) {
      writeToEoF = TRUE;
    }

    isPagingIo = (RequestContext->Irp->Flags & IRP_PAGING_IO);

    if (RequestContext->Irp->Flags & IRP_NOCACHE) {
      isNonCached = TRUE;
    }

    if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
      isSynchronousIo = TRUE;
    }

    if (!isPagingIo && (fileObject->SectionObjectPointer != NULL) &&
        (fileObject->SectionObjectPointer->DataSectionObject != NULL)) {

      CcFlushCache(&fcb->SectionObjectPointers,
                   writeToEoF
                       ? NULL
                       : &RequestContext->IrpSp->Parameters.Write.ByteOffset,
                   RequestContext->IrpSp->Parameters.Write.Length, NULL);

      DokanPagingIoLockRW(fcb);
      DokanPagingIoUnlock(fcb);

      CcPurgeCacheSection(
          &fcb->SectionObjectPointers,
          writeToEoF ? NULL
                     : &RequestContext->IrpSp->Parameters.Write.ByteOffset,
          RequestContext->IrpSp->Parameters.Write.Length, FALSE);
    }

    // Cannot write at end of the file when using paging IO
    if (writeToEoF && isPagingIo) {
      DOKAN_LOG_FINE_IRP(RequestContext, "WriteToEoF & isPagingIo");
      status = STATUS_SUCCESS;
      __leave;
    }

    // the length of EventContext is sum of length to write and length of file
    // name
    DokanFCBLockRO(fcb);
    fcbLocked = TRUE;

    LARGE_INTEGER safeEventLength;
    safeEventLength.QuadPart =
        sizeof(EVENT_CONTEXT) + RequestContext->IrpSp->Parameters.Write.Length +
                  fcb->FileName.Length;
    if (safeEventLength.HighPart != 0 ||
        safeEventLength.QuadPart <
            sizeof(EVENT_CONTEXT) + fcb->FileName.Length) {
      DokanLogError(&logger,
                    STATUS_INVALID_PARAMETER,
                    L"Write with unsupported total size: %I64u",
                    safeEventLength.QuadPart);
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }
    eventLength = safeEventLength.LowPart;
    eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

    // no more memory!
    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;

    // When the length is bigger than usual event notitfication buffer,
    // saves pointer in DiverContext to copy EventContext after allocating
    // more bigger memory.
    RequestContext->Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] =
        eventContext;

    if (isPagingIo) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Paging IO");
      eventContext->FileFlags |= DOKAN_PAGING_IO;
    }
    if (isSynchronousIo) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Synchronous IO");
      eventContext->FileFlags |= DOKAN_SYNCHRONOUS_IO;
    }
    if (isNonCached) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Nocache");
      eventContext->FileFlags |= DOKAN_NOCACHE;
    }

    // offset of file to write
    eventContext->Operation.Write.ByteOffset =
        RequestContext->IrpSp->Parameters.Write.ByteOffset;

    if (writeToEoF) {
      eventContext->FileFlags |= DOKAN_WRITE_TO_END_OF_FILE;
      DOKAN_LOG_FINE_IRP(RequestContext, "WriteOffset = end of file");
    }

    if (isSynchronousIo &&
        ((RequestContext->IrpSp->Parameters.Write.ByteOffset.LowPart ==
          FILE_USE_FILE_POINTER_POSITION) &&
         (RequestContext->IrpSp->Parameters.Write.ByteOffset.HighPart == -1))) {
      // NOTE:
      // http://msdn.microsoft.com/en-us/library/ms795960.aspx
      // Do not check IrpSp->Parameters.Write.ByteOffset.QuadPart == 0
      // Probably the document is wrong.
      eventContext->Operation.Write.ByteOffset.QuadPart =
          fileObject->CurrentByteOffset.QuadPart;
    }

    // the size of buffer to write
    eventContext->Operation.Write.BufferLength =
        RequestContext->IrpSp->Parameters.Write.Length;

    // the offset from the beginning of structure
    // the contents to write will be copyed to this offset
    eventContext->Operation.Write.BufferOffset =
        FIELD_OFFSET(EVENT_CONTEXT, Operation.Write.FileName[0]) +
        fcb->FileName.Length + sizeof(WCHAR); // adds last null char

    // copies the content to write to EventContext
    RtlCopyMemory((PCHAR)eventContext +
                      eventContext->Operation.Write.BufferOffset,
        buffer, RequestContext->IrpSp->Parameters.Write.Length);

    // copies file name
    eventContext->Operation.Write.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Write.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    // When eventlength is less than event notification buffer,
    // returns it to user-mode using pending event.
    if (eventLength <= EVENT_CONTEXT_MAX_SIZE) {

      // EventContext is no longer needed, clear it
      RequestContext->Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;

      //
      //  We now check whether we can proceed based on the state of
      //  the file oplocks.
      //
      // FsRtlCheckOpLock is called with non-NULL completion routine - not blocking.
      if (!FlagOn(RequestContext->Irp->Flags, IRP_PAGING_IO)) {
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
      }

      // register this IRP to IRP waiting list and make it pending status
      status = DokanRegisterPendingIrp(RequestContext, eventContext);

      // Resuests bigger memory
      // eventContext will be freed later using
      // Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT]
    } else {
      // the length at lest file name can be stored
      ULONG eventWriteContextLength = max(
          sizeof(EVENT_CONTEXT), eventContext->Operation.Write.BufferOffset);
      PEVENT_CONTEXT eventWriteContext =
          AllocateEventContext(RequestContext, eventWriteContextLength, ccb);

      // no more memory!
      if (eventWriteContext == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        RequestContext->Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;
        DokanFreeEventContext(eventContext);
        __leave;
      }

      // copies from beginning of EventContext to the end of file name
      RtlCopyMemory(eventWriteContext, eventContext,
                    eventContext->Operation.Write.BufferOffset);
      // puts actual size of RequestContext
      eventWriteContext->Length = eventWriteContextLength;
      // requsts enough size to copy EventContext
      eventWriteContext->Operation.Write.RequestLength = eventLength;

      //
      //  We now check whether we can proceed based on the state of
      //  the file oplocks.
      //
      // FsRtlCheckOpLock is called with non-NULL completion routine - not
      // blocking.
      if (!FlagOn(RequestContext->Irp->Flags, IRP_PAGING_IO)) {
        status = FsRtlCheckOplock(DokanGetFcbOplock(fcb), RequestContext->Irp,
                                  eventWriteContext,
                                  DokanOplockComplete, DokanPrePostIrp);

        //
        //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
        //  to service an oplock break and we need to leave now.
        //
        if (status != STATUS_SUCCESS) {
          if (status == STATUS_PENDING) {
            DOKAN_LOG_FINE_IRP(RequestContext,
                               "FsRtlCheckOplock returned STATUS_PENDING");
          } else {
            DokanFreeEventContext(eventWriteContext);
            RequestContext->Irp->Tail.Overlay
                .DriverContext[DRIVER_CONTEXT_EVENT] = 0;
            DokanFreeEventContext(eventContext);
          }
          __leave;
        }
      }

      // regiters this IRP to IRP wainting list and make it pending status
      status = DokanRegisterPendingIrp(RequestContext, eventWriteContext);
    }

  } __finally {
    if (fcbLocked)
      DokanFCBUnlock(fcb);
  }

  return status;
}

VOID DokanCompleteWrite(__in PREQUEST_CONTEXT RequestContext,
                        __in PEVENT_INFORMATION EventInfo) {
  PDokanFCB fcb;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;
  BOOLEAN isPagingIo = FALSE;

  fileObject = RequestContext->IrpSp->FileObject;
  ASSERT(fileObject != NULL);

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  isPagingIo = (RequestContext->Irp->Flags & IRP_PAGING_IO);

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  ccb->UserContext = EventInfo->Context;

  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
  RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;

  if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status)) {

    //Check if file size changed
    if (fcb->AdvancedFCBHeader.FileSize.QuadPart <
        EventInfo->Operation.Write.CurrentByteOffset.QuadPart) {

      if (!isPagingIo) {
        DokanFCBLockRO(fcb);
      }

      DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_SIZE,
                              FILE_ACTION_MODIFIED);

      if (!isPagingIo) {
        DokanFCBUnlock(fcb);
      }

      //Update size with new offset
      InterlockedExchange64(
          &fcb->AdvancedFCBHeader.FileSize.QuadPart,
          EventInfo->Operation.Write.CurrentByteOffset.QuadPart);
    }

    DokanFCBFlagsSetBit(fcb, DOKAN_FILE_CHANGE_LAST_WRITE);

    if (!isPagingIo) {
      SetFlag(fileObject->Flags, FO_FILE_MODIFIED);
    }

    if (EventInfo->BufferLength != 0 && fileObject->Flags & FO_SYNCHRONOUS_IO &&
        !isPagingIo) {
      // update current byte offset only when synchronous IO and not paging IO
      fileObject->CurrentByteOffset.QuadPart =
          EventInfo->Operation.Write.CurrentByteOffset.QuadPart;
      DOKAN_LOG_FINE_IRP(RequestContext, "Updated CurrentByteOffset %I64u",
                         fileObject->CurrentByteOffset.QuadPart);
    }
  }
}
