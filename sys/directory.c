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
DokanQueryDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp);

NTSTATUS
DokanNotifyChangeDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP *pIrp);

NTSTATUS
DokanDispatchDirectoryControl(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  PFILE_OBJECT fileObject;
  PIO_STACK_LOCATION irpSp;
  PDokanVCB vcb;

  __try {
    DOKAN_LOG_BEGIN_MJ(Irp);
    irpSp = IoGetCurrentIrpStackLocation(Irp);
    fileObject = irpSp->FileObject;
    DOKAN_LOG_FINE_IRP(Irp, "FileObject=%p", irpSp->FileObject);

    if (fileObject == NULL) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    vcb = DeviceObject->DeviceExtension;
    if (GetIdentifierType(vcb) != VCB ||
        !DokanCheckCCB(Irp, vcb->Dcb, fileObject->FsContext2)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    if (irpSp->MinorFunction == IRP_MN_QUERY_DIRECTORY) {
      status = DokanQueryDirectory(DeviceObject, Irp);

    } else if (irpSp->MinorFunction == IRP_MN_NOTIFY_CHANGE_DIRECTORY) {
      status = DokanNotifyChangeDirectory(DeviceObject, &Irp);
    } else {
      DOKAN_LOG_FINE_IRP(Irp, "Invalid minor function");
      status = STATUS_INVALID_PARAMETER;
    }

  } __finally {
    DOKAN_LOG_END_MJ(Irp, status, 0);
    DokanCompleteIrpRequest(Irp, status, 0);
  }

  return status;
}

NTSTATUS
DokanQueryDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PFILE_OBJECT fileObject;
  PIO_STACK_LOCATION irpSp;
  PDokanVCB vcb;
  PDokanCCB ccb;
  PDokanFCB fcb;
  NTSTATUS status;
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;
  ULONG index;
  BOOLEAN initial;
  ULONG flags = 0;

  irpSp = IoGetCurrentIrpStackLocation(Irp);
  fileObject = irpSp->FileObject;

  vcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(vcb) != VCB) {
    return STATUS_INVALID_PARAMETER;
  }

  ccb = fileObject->FsContext2;
  if (ccb == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  ASSERT(ccb != NULL);

  if (irpSp->Flags & SL_INDEX_SPECIFIED) {
    DOKAN_LOG_FINE_IRP(Irp, "Index specified %d",
              irpSp->Parameters.QueryDirectory.FileIndex);
  }
  if (irpSp->Flags & SL_RETURN_SINGLE_ENTRY) {
    DOKAN_LOG_FINE_IRP(Irp, "Return single entry");
  }
  if (irpSp->Flags & SL_RESTART_SCAN) {
    DOKAN_LOG_FINE_IRP(Irp, "Restart scan");
  }
  if (irpSp->Parameters.QueryDirectory.FileName) {
    DOKAN_LOG_FINE_IRP(Irp, "Pattern=\"%wZ\"",
                  irpSp->Parameters.QueryDirectory.FileName);
  }

  DOKAN_LOG_FINE_IRP(
      Irp, "FileObject=%p FileInformationClass=%s", fileObject,
      DokanGetFileInformationClassStr(
          irpSp->Parameters.QueryDirectory.FileInformationClass));

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  OplockDebugRecordMajorFunction(fcb, IRP_MJ_DIRECTORY_CONTROL);

  // make a MDL for UserBuffer that can be used later on another thread context
  if (Irp->MdlAddress == NULL) {
    status = DokanAllocateMdl(Irp, irpSp->Parameters.QueryDirectory.Length);
    if (!NT_SUCCESS(status)) {
      return status;
    }
    flags = DOKAN_MDL_ALLOCATED;
  }

  DokanFCBLockRO(fcb);

  // size of EVENT_CONTEXT is sum of its length and file name length
  eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;

  initial = (BOOLEAN)(ccb->SearchPattern == NULL &&
                      !(DokanCCBFlagsIsSet(ccb, DOKAN_DIR_MATCH_ALL)));

  // this is an initial query
  if (initial) {
    DOKAN_LOG_FINE_IRP(Irp, "Initial query");
    // and search pattern is provided
    if (irpSp->Parameters.QueryDirectory.FileName) {
      // free current search pattern stored in CCB
      if (ccb->SearchPattern)
        ExFreePool(ccb->SearchPattern);

      // the size of search pattern
      ccb->SearchPatternLength =
          irpSp->Parameters.QueryDirectory.FileName->Length;
      ccb->SearchPattern =
          DokanAllocZero(ccb->SearchPatternLength + sizeof(WCHAR));

      if (ccb->SearchPattern == NULL) {
        DokanFCBUnlock(fcb);
        return STATUS_INSUFFICIENT_RESOURCES;
      }

      // copy provided search pattern to CCB
      RtlCopyMemory(ccb->SearchPattern,
                    irpSp->Parameters.QueryDirectory.FileName->Buffer,
                    ccb->SearchPatternLength);

    } else {
      DokanCCBFlagsSetBit(ccb, DOKAN_DIR_MATCH_ALL);
    }
  }

  // if search pattern is provided, add the length of it to store pattern
  if (ccb->SearchPattern) {
    eventLength += ccb->SearchPatternLength;
  }

  eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

  if (eventContext == NULL) {
    DokanFCBUnlock(fcb);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  eventContext->Context = ccb->UserContext;
  // DDbgPrint("   get Context %X\n", (ULONG)ccb->UserContext);

  // index which specified index-1 th directory entry has been returned
  // this time, 'index'th entry should be returned
  index = 0;

  if (irpSp->Flags & SL_INDEX_SPECIFIED) {
    index = irpSp->Parameters.QueryDirectory.FileIndex;
    DOKAN_LOG_FINE_IRP(Irp, "Using FileIndex %d", index);

  } else if (FlagOn(irpSp->Flags, SL_RESTART_SCAN)) {
    DOKAN_LOG_FINE_IRP(Irp, "SL_RESTART_SCAN");
    index = 0;

  } else {
    index = (ULONG)ccb->Context;
    DOKAN_LOG_FINE_IRP(Irp, "ccb->Context %d", index);
  }

  eventContext->Operation.Directory.FileInformationClass =
      irpSp->Parameters.QueryDirectory.FileInformationClass;
  eventContext->Operation.Directory.BufferLength =
      irpSp->Parameters.QueryDirectory.Length; // length of buffer
  eventContext->Operation.Directory.FileIndex =
      index; // directory index which should be returned this time

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

    DOKAN_LOG_FINE_IRP(Irp, "ccb->SearchPattern %ws", ccb->SearchPattern);
  }

  status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, flags);

  return status;
}

NTSTATUS
DokanNotifyChangeDirectory(__in PDEVICE_OBJECT DeviceObject, __in PIRP *pIrp) {
  PDokanCCB ccb;
  PDokanFCB fcb;
  PFILE_OBJECT fileObject;
  PIO_STACK_LOCATION irpSp;
  PDokanVCB vcb;

  irpSp = IoGetCurrentIrpStackLocation(*pIrp);
  fileObject = irpSp->FileObject;
  DOKAN_LOG_FINE_IRP(*pIrp, "FileObject=%p", fileObject);

  vcb = DeviceObject->DeviceExtension;
  if (GetIdentifierType(vcb) != VCB) {
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
  FsRtlNotifyFullChangeDirectory(
      vcb->NotifySync, &vcb->DirNotifyList, ccb, (PSTRING)&fcb->FileName,
      (irpSp->Flags & SL_WATCH_TREE) ? TRUE : FALSE, FALSE,
      irpSp->Parameters.NotifyDirectory.CompletionFilter, *pIrp, NULL, NULL);
  DokanFCBUnlock(fcb);

  // FsRtlNotifyFullChangeDirectory has now completed the IRP and it is unsafe
  // to access or complete the IRP.
  *pIrp = NULL;

  return STATUS_PENDING;
}

VOID DokanCompleteDirectoryControl(__in PIRP_ENTRY IrpEntry,
                                   __in PEVENT_INFORMATION EventInfo) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_SUCCESS;
  ULONG info = 0;
  ULONG bufferLen = 0;
  PVOID buffer = NULL;

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;

  DOKAN_LOG_BEGIN_MJ(irp);
  DOKAN_LOG_FINE_IRP(irp, "FileObject=%p", irpSp->FileObject);

  // buffer pointer which points DirecotryInfo
  if (irp->MdlAddress) {
    // DDbgPrint("   use MDL Address\n");
    buffer = MmGetSystemAddressForMdlNormalSafe(irp->MdlAddress);
  } else {
    // DDbgPrint("   use UserBuffer\n");
    buffer = irp->UserBuffer;
  }
  // usable buffer size
  bufferLen = irpSp->Parameters.QueryDirectory.Length;

  // DDbgPrint("  !!Returning DirectoryInfo!!\n");

  // buffer is not specified or short of length
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {
    info = 0;
    status = STATUS_INSUFFICIENT_RESOURCES;

  } else {

    PDokanCCB ccb = IrpEntry->FileObject->FsContext2;
    // ULONG     orgLen = irpSp->Parameters.QueryDirectory.Length;

    //
    // set the information received from user mode
    //
    ASSERT(buffer != NULL);
    RtlZeroMemory(buffer, bufferLen);

    // DDbgPrint("   copy DirectoryInfo\n");
    RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

    DOKAN_LOG_FINE_IRP(irp, "EventInfo->Directory.Index = %lu",
                  EventInfo->Operation.Directory.Index);
    DOKAN_LOG_FINE_IRP(irp, "EventInfo->BufferLength = % lu",
                  EventInfo->BufferLength);
    DOKAN_LOG_FINE_IRP(irp, "EventInfo->Status = % x(% lu)", EventInfo->Status,
                  EventInfo->Status);

    // update index which specified n-th directory entry is returned
    // this should be locked before writing?
    ccb->Context = EventInfo->Operation.Directory.Index;

    ccb->UserContext = EventInfo->Context;
    // DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

    // written bytes
    // irpSp->Parameters.QueryDirectory.Length = EventInfo->BufferLength;

    status = EventInfo->Status;

    info = EventInfo->BufferLength;
  }

  if (IrpEntry->Flags & DOKAN_MDL_ALLOCATED) {
    DokanFreeMdl(irp);
    IrpEntry->Flags &= ~DOKAN_MDL_ALLOCATED;
  }

  DOKAN_LOG_END_MJ(irp, status, info);
  DokanCompleteIrpRequest(irp, status, info);
}
