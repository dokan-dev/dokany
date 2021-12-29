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
DokanQueryDirectory(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanNotifyChangeDirectory(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanDispatchDirectoryControl(__in PREQUEST_CONTEXT RequestContext) {
  PFILE_OBJECT fileObject;

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  if (fileObject == NULL || !RequestContext->Vcb ||
      !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
    return STATUS_INVALID_PARAMETER;
  }

  switch (RequestContext->IrpSp->MinorFunction) {
    case IRP_MN_QUERY_DIRECTORY:
      return DokanQueryDirectory(RequestContext);
    case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
      return DokanNotifyChangeDirectory(RequestContext);
  }
  DOKAN_LOG_FINE_IRP(RequestContext, "Unsupported MinorFunction %x",
                     RequestContext->IrpSp->MinorFunction);
  return STATUS_INVALID_PARAMETER;
}

NTSTATUS
DokanQueryDirectory(__in PREQUEST_CONTEXT RequestContext) {
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  PDokanFCB fcb;
  NTSTATUS status;
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;
  ULONG index;
  BOOLEAN initial;

  fileObject = RequestContext->IrpSp->FileObject;

  if (!RequestContext->Vcb) {
    return STATUS_INVALID_PARAMETER;
  }

  ccb = fileObject->FsContext2;
  if (ccb == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  ASSERT(ccb != NULL);

  if (RequestContext->IrpSp->Flags & SL_INDEX_SPECIFIED) {
    DOKAN_LOG_FINE_IRP(
        RequestContext, "Index specified %d",
        RequestContext->IrpSp->Parameters.QueryDirectory.FileIndex);
  }
  if (RequestContext->IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Return single entry");
  }
  if (RequestContext->IrpSp->Flags & SL_RESTART_SCAN) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Restart scan");
  }
  if (RequestContext->IrpSp->Parameters.QueryDirectory.FileName) {
    DOKAN_LOG_FINE_IRP(
        RequestContext, "Pattern=\"%wZ\"",
        RequestContext->IrpSp->Parameters.QueryDirectory.FileName);
  }

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p FileInformationClass=%s",
                     fileObject,
                     DokanGetFileInformationClassStr(
                         RequestContext->IrpSp->Parameters.QueryDirectory
                             .FileInformationClass));

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  OplockDebugRecordMajorFunction(fcb, IRP_MJ_DIRECTORY_CONTROL);

  // make a MDL for UserBuffer that can be used later on another thread context
  if (RequestContext->Irp->MdlAddress == NULL) {
    status = DokanAllocateMdl(
        RequestContext,
        RequestContext->IrpSp->Parameters.QueryDirectory.Length);
    if (!NT_SUCCESS(status)) {
      return status;
    }
    RequestContext->Flags = DOKAN_MDL_ALLOCATED;
  }

  DokanFCBLockRO(fcb);

  // size of EVENT_CONTEXT is sum of its length and file name length
  eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;

  initial = (BOOLEAN)(ccb->SearchPattern == NULL &&
                      !(DokanCCBFlagsIsSet(ccb, DOKAN_DIR_MATCH_ALL)));

  // this is an initial query
  if (initial) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Initial query");
    // and search pattern is provided
    if (RequestContext->IrpSp->Parameters.QueryDirectory.FileName) {
      // free current search pattern stored in CCB
      if (ccb->SearchPattern) ExFreePool(ccb->SearchPattern);

      // the size of search pattern
      ccb->SearchPatternLength =
          RequestContext->IrpSp->Parameters.QueryDirectory.FileName->Length;
      ccb->SearchPattern =
          DokanAllocZero(ccb->SearchPatternLength + sizeof(WCHAR));

      if (ccb->SearchPattern == NULL) {
        DokanFCBUnlock(fcb);
        return STATUS_INSUFFICIENT_RESOURCES;
      }

      // copy provided search pattern to CCB
      RtlCopyMemory(
          ccb->SearchPattern,
          RequestContext->IrpSp->Parameters.QueryDirectory.FileName->Buffer,
          ccb->SearchPatternLength);

    } else {
      DokanCCBFlagsSetBit(ccb, DOKAN_DIR_MATCH_ALL);
    }
  }

  // if search pattern is provided, add the length of it to store pattern
  if (ccb->SearchPattern) {
    eventLength += ccb->SearchPatternLength;
  }

  eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

  if (eventContext == NULL) {
    DokanFCBUnlock(fcb);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  eventContext->Context = ccb->UserContext;

  // index which specified index-1 th directory entry has been returned
  // this time, 'index'th entry should be returned
  index = 0;

  if (RequestContext->IrpSp->Flags & SL_INDEX_SPECIFIED) {
    index = RequestContext->IrpSp->Parameters.QueryDirectory.FileIndex;
    DOKAN_LOG_FINE_IRP(RequestContext, "Using FileIndex %d", index);

  } else if (FlagOn(RequestContext->IrpSp->Flags, SL_RESTART_SCAN)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "SL_RESTART_SCAN");
    index = 0;

  } else {
    index = (ULONG)ccb->Context;
    DOKAN_LOG_FINE_IRP(RequestContext, "ccb->Context %d", index);
  }

  eventContext->Operation.Directory.FileInformationClass =
      RequestContext->IrpSp->Parameters.QueryDirectory.FileInformationClass;
  eventContext->Operation.Directory.BufferLength =
      RequestContext->IrpSp->Parameters.QueryDirectory
          .Length;  // length of buffer
  eventContext->Operation.Directory.FileIndex =
      index;  // directory index which should be returned this time

  // copying file name(directory name)
  eventContext->Operation.Directory.DirectoryNameLength = fcb->FileName.Length;
  RtlCopyMemory(eventContext->Operation.Directory.DirectoryName,
                fcb->FileName.Buffer, fcb->FileName.Length);
  DokanFCBUnlock(fcb);

  // if search pattern is specified, copy it to EventContext
  if (ccb->SearchPatternLength && ccb->SearchPattern) {
    PVOID searchBuffer;

    eventContext->Operation.Directory.SearchPatternLength =
        ccb->SearchPatternLength;
    eventContext->Operation.Directory.SearchPatternOffset =
        eventContext->Operation.Directory.DirectoryNameLength;

    searchBuffer = (PVOID)(
        (SIZE_T)&eventContext->Operation.Directory.SearchPatternBase[0] +
        (SIZE_T)eventContext->Operation.Directory.SearchPatternOffset);

    RtlCopyMemory(searchBuffer, ccb->SearchPattern, ccb->SearchPatternLength);

    DOKAN_LOG_FINE_IRP(RequestContext, "ccb->SearchPattern %ws",
                       ccb->SearchPattern);
  }

  status = DokanRegisterPendingIrp(RequestContext, eventContext);

  return status;
}

NTSTATUS
DokanNotifyChangeDirectory(__in PREQUEST_CONTEXT RequestContext) {
  PDokanCCB ccb;
  PDokanFCB fcb;
  PFILE_OBJECT fileObject;

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  if (!RequestContext->Vcb) {
    return STATUS_INVALID_PARAMETER;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  if (!DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
    return STATUS_INVALID_PARAMETER;
  }

  DokanFCBLockRO(fcb);
  // These flags are only set for debugging purposes.
  DokanFCBFlagsSetBit(fcb, DOKAN_EVER_USED_IN_NOTIFY_LIST);
  DokanCCBFlagsSetBit(ccb, DOKAN_EVER_USED_IN_NOTIFY_LIST);
  // Note that this stores a pointer to the FileName embedded in the FCB in the
  // opaque DirNotifyList (the string is not copied).
  FsRtlNotifyFullChangeDirectory(
      RequestContext->Vcb->NotifySync, &RequestContext->Vcb->DirNotifyList, ccb,
      (PSTRING)&fcb->FileName,
      (RequestContext->IrpSp->Flags & SL_WATCH_TREE) ? TRUE : FALSE, FALSE,
      RequestContext->IrpSp->Parameters.NotifyDirectory.CompletionFilter,
      RequestContext->Irp, NULL, NULL);
  DokanFCBUnlock(fcb);

  // FsRtlNotifyFullChangeDirectory has now completed the IRP and it is unsafe
  // to access or complete the IRP.
  RequestContext->DoNotComplete = TRUE;

  return STATUS_PENDING;
}

VOID DokanCompleteDirectoryControl(__in PREQUEST_CONTEXT RequestContext,
                                   __in PEVENT_INFORMATION EventInfo) {
  ULONG bufferLen = 0;
  PVOID buffer = NULL;

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p",
                     RequestContext->IrpSp->FileObject);

  // buffer pointer which points DirecotryInfo
  if (RequestContext->Irp->MdlAddress) {
    buffer =
        MmGetSystemAddressForMdlNormalSafe(RequestContext->Irp->MdlAddress);
  } else {
    buffer = RequestContext->Irp->UserBuffer;
  }
  // usable buffer size
  bufferLen = RequestContext->IrpSp->Parameters.QueryDirectory.Length;

  // buffer is not specified or short of length
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {
    RequestContext->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

  } else {

    PDokanCCB ccb = RequestContext->IrpSp->FileObject->FsContext2;
    // ULONG     orgLen = irpSp->Parameters.QueryDirectory.Length;

    //
    // set the information received from user mode
    //
    ASSERT(buffer != NULL);
    RtlZeroMemory(buffer, bufferLen);

    RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

    DOKAN_LOG_FINE_IRP(RequestContext, "EventInfo->Directory.Index = %lu",
                  EventInfo->Operation.Directory.Index);
    DOKAN_LOG_FINE_IRP(RequestContext, "EventInfo->BufferLength = % lu",
                  EventInfo->BufferLength);
    DOKAN_LOG_FINE_IRP(RequestContext, "EventInfo->Status = % x(% lu)", EventInfo->Status,
                  EventInfo->Status);

    // update index which specified n-th directory entry is returned
    // this should be locked before writing?
    ccb->Context = EventInfo->Operation.Directory.Index;

    ccb->UserContext = EventInfo->Context;

    // written bytes
    // irpSp->Parameters.QueryDirectory.Length = EventInfo->BufferLength;

    RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;
    RequestContext->Irp->IoStatus.Status = EventInfo->Status;
  }

  if (RequestContext->Flags & DOKAN_MDL_ALLOCATED) {
    DokanFreeMdl(RequestContext->Irp);
    RequestContext->Flags &= ~DOKAN_MDL_ALLOCATED;
  }
}
