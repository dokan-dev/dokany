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
DokanDispatchRead(__in PREQUEST_CONTEXT RequestContext)

/*++

Routine Description:

        This device control dispatcher handles read IRPs.

Arguments:

        DeviceObject - Context for the activity.
        Irp          - The device control argument block.

Return Value:

        NTSTATUS

--*/
{
  PFILE_OBJECT fileObject;
  ULONG bufferLength;
  LARGE_INTEGER byteOffset;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  PVOID currentAddress = NULL;
  PEVENT_CONTEXT eventContext;
  ULONG eventLength;
  BOOLEAN fcbLocked = FALSE;
  BOOLEAN isPagingIo = FALSE;
  BOOLEAN isSynchronousIo = FALSE;
  BOOLEAN noCache = FALSE;

  __try {
    fileObject = RequestContext->IrpSp->FileObject;
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "FileObject=%p MdlAddress=%p UserBuffer=%p Length=%ld ByteOffset=%I64u",
        fileObject, RequestContext->Irp->MdlAddress,
        RequestContext->Irp->UserBuffer,
        RequestContext->IrpSp->Parameters.Read.Length,
        RequestContext->IrpSp->Parameters.Read.ByteOffset.QuadPart);

    //
    //  If this is a zero length read then return SUCCESS immediately.
    //
    if (RequestContext->IrpSp->Parameters.Read.Length == 0) {
      status = STATUS_SUCCESS;
      __leave;
    }

    if (RequestContext->IrpSp->MinorFunction == IRP_MN_COMPLETE) {
      RequestContext->Irp->MdlAddress = NULL;
      status = STATUS_SUCCESS;
      __leave;
    }

    if (fileObject == NULL && RequestContext->Irp->MdlAddress != NULL) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Reads by File System Recognizers");

      currentAddress =
          MmGetSystemAddressForMdlNormalSafe(RequestContext->Irp->MdlAddress);
      if (currentAddress == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }

      // here we could return the bootsector. If we don't have one
      // the requested read length must be returned as requested
      RequestContext->Irp->IoStatus.Information =
          RequestContext->IrpSp->Parameters.Read.Length;
      status = STATUS_SUCCESS;
      __leave;
    }

    if (fileObject == NULL || !RequestContext->Vcb ||
        !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
      status = STATUS_INVALID_DEVICE_REQUEST;
      __leave;
    }

    bufferLength = RequestContext->IrpSp->Parameters.Read.Length;
    if (RequestContext->IrpSp->Parameters.Read.ByteOffset.LowPart ==
            FILE_USE_FILE_POINTER_POSITION &&
        RequestContext->IrpSp->Parameters.Read.ByteOffset.HighPart == -1) {
      // irpSp->Parameters.Read.ByteOffset == NULL don't need check?
      byteOffset = fileObject->CurrentByteOffset;
    } else {
      byteOffset = RequestContext->IrpSp->Parameters.Read.ByteOffset;
    }

    if (bufferLength == 0) {
      status = STATUS_SUCCESS;
      __leave;
    }

    // make a MDL for UserBuffer that can be used later on another thread
    // context
    if (RequestContext->Irp->MdlAddress == NULL) {
      status = DokanAllocateMdl(RequestContext,
                                RequestContext->IrpSp->Parameters.Read.Length);
      if (!NT_SUCCESS(status)) {
        __leave;
      }
    }

    ccb = fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    OplockDebugRecordMajorFunction(fcb, IRP_MJ_READ);
    if (fcb->BlockUserModeDispatch) {
      status = STATUS_SUCCESS;
      __leave;
    }

    if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "DOKAN_FILE_DIRECTORY FCB=%p", fcb);
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    isPagingIo = (RequestContext->Irp->Flags & IRP_PAGING_IO);

    if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
      isSynchronousIo = TRUE;
    }

    if (RequestContext->Irp->Flags & IRP_NOCACHE) {
      noCache = TRUE;
    }

    if (!isPagingIo && (fileObject->SectionObjectPointer != NULL) &&
        (fileObject->SectionObjectPointer->DataSectionObject != NULL)) {
      CcFlushCache(&fcb->SectionObjectPointers,
                   &RequestContext->IrpSp->Parameters.Read.ByteOffset,
                   RequestContext->IrpSp->Parameters.Read.Length, NULL);
    }

    DokanFCBLockRO(fcb);
    fcbLocked = TRUE;
    // length of EventContext is sum of file name length and itself
    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
    eventContext = AllocateEventContext(RequestContext, eventLength, ccb);
    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;

    if (isPagingIo) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Paging IO");
      eventContext->FileFlags |= DOKAN_PAGING_IO;
    }
    if (isSynchronousIo) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Synchronous IO");
      eventContext->FileFlags |= DOKAN_SYNCHRONOUS_IO;
    }

    if (noCache) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Nocache");
      eventContext->FileFlags |= DOKAN_NOCACHE;
    }

    // offset of file to read
    eventContext->Operation.Read.ByteOffset = byteOffset;

    // buffer size for read
    // user-mode file system application can return this size
    eventContext->Operation.Read.BufferLength =
        RequestContext->IrpSp->Parameters.Read.Length;

    // copy the accessed file name
    eventContext->Operation.Read.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.Read.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    //
    //  We now check whether we can proceed based on the state of
    //  the file oplocks.
    //
    if (!FlagOn(RequestContext->Irp->Flags, IRP_PAGING_IO)) {
      // FsRtlCheckOpLock is called with non-NULL completion routine - not blocking.
      status = DokanCheckOplock(fcb, RequestContext->Irp, eventContext,
                                DokanOplockComplete, DokanPrePostIrp);

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

      //
      // We have to check for read access according to the current
      // state of the file locks, and set FileSize from the Fcb.
      //
      // FsRtlCheckLockForReadAccess does not block.
      if (!FsRtlCheckLockForReadAccess(&fcb->FileLock, RequestContext->Irp)) {
        status = STATUS_FILE_LOCK_CONFLICT;
        __leave;
      }
    }

    // register this IRP to pending IPR list and make it pending status
    status = DokanRegisterPendingIrp(RequestContext, eventContext);
  } __finally {
    if (fcbLocked)
      DokanFCBUnlock(fcb);
  }

  return status;
}

VOID DokanCompleteRead(__in PREQUEST_CONTEXT RequestContext,
                       __in PEVENT_INFORMATION EventInfo) {
  ULONG bufferLen = 0;
  PVOID buffer = NULL;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;

  fileObject = RequestContext->IrpSp->FileObject;
  ASSERT(fileObject != NULL);

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  ccb->UserContext = EventInfo->Context;

  // buffer which is used to copy Read info
  if (RequestContext->Irp->MdlAddress) {
    buffer =
        MmGetSystemAddressForMdlNormalSafe(RequestContext->Irp->MdlAddress);
  } else {
    buffer = RequestContext->Irp->UserBuffer;
  }

  // available buffer size
  bufferLen = RequestContext->IrpSp->Parameters.Read.Length;

  DOKAN_LOG_FINE_IRP(RequestContext, "BufferLen %lu, Event.BufferLen %lu", bufferLen,
                EventInfo->BufferLength);

  // buffer is not specified or short of length
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {

    RequestContext->Irp->IoStatus.Information = 0;
    RequestContext->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

  } else {
    RtlZeroMemory(buffer, bufferLen);
    RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

    // read length which is actually read
    RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;
    RequestContext->Irp->IoStatus.Status = EventInfo->Status;

    if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status) &&
        EventInfo->BufferLength > 0 &&
        (fileObject->Flags & FO_SYNCHRONOUS_IO) &&
        !(RequestContext->Irp->Flags & IRP_PAGING_IO)) {
      // update current byte offset only when synchronous IO and not pagind IO
      fileObject->CurrentByteOffset.QuadPart =
          EventInfo->Operation.Read.CurrentByteOffset.QuadPart;
      DOKAN_LOG_FINE_IRP(RequestContext, "Updated CurrentByteOffset %I64u",
                fileObject->CurrentByteOffset.QuadPart);
    }
  }

  if (RequestContext->Flags & DOKAN_MDL_ALLOCATED) {
    DokanFreeMdl(RequestContext->Irp);
    RequestContext->Flags &= ~DOKAN_MDL_ALLOCATED;
  }
}
