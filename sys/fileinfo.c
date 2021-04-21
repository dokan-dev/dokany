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
#include "util/irp_buffer_helper.h"
#include "util/str.h"

NTSTATUS
DokanDispatchQueryInformation(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject;
  FILE_INFORMATION_CLASS infoClass;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;

  // PAGED_CODE();

  __try {
    fileObject = RequestContext->IrpSp->FileObject;
    infoClass =
        RequestContext->IrpSp->Parameters.QueryFile.FileInformationClass;

    DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p InfoClass=%s ", fileObject,
                  DokanGetFileInformationClassStr(infoClass));

    if (fileObject == NULL || !RequestContext->Vcb ||
        !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    ccb = (PDokanCCB)fileObject->FsContext2;
    ASSERT(ccb != NULL);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    OplockDebugRecordMajorFunction(fcb, IRP_MJ_QUERY_INFORMATION);
    DokanFCBLockRO(fcb);
    switch (infoClass) {
    case FileNormalizedNameInformation:
    case FileNameInformation: {
      PFILE_NAME_INFORMATION nameInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, nameInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        __leave;
      }

      PUNICODE_STRING fileName = &fcb->FileName;
      PCHAR dest = (PCHAR)&nameInfo->FileName;
      nameInfo->FileNameLength = fileName->Length;

      BOOLEAN isNetworkDevice = (RequestContext->Dcb->VolumeDeviceType ==
                                 FILE_DEVICE_NETWORK_FILE_SYSTEM);
      if (isNetworkDevice) {
        PUNICODE_STRING devicePath = RequestContext->Dcb->UNCName->Length
                                         ? RequestContext->Dcb->UNCName
                                         : RequestContext->Dcb->DiskDeviceName;
        nameInfo->FileNameLength += devicePath->Length;

        if (!AppendVarSizeOutputString(RequestContext->Irp, dest, devicePath,
                                       /*UpdateInformationOnFailure=*/FALSE,
                                       /*FillSpaceWithPartialString=*/TRUE)) {
          status = STATUS_BUFFER_OVERFLOW;
          __leave;
        }
        dest += devicePath->Length;
      }

      if (!AppendVarSizeOutputString(RequestContext->Irp, dest, fileName,
                                     /*UpdateInformationOnFailure=*/FALSE,
                                     /*FillSpaceWithPartialString=*/TRUE)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }
      status = STATUS_SUCCESS;
      __leave;
    } break;
    case FilePositionInformation: {
      PFILE_POSITION_INFORMATION posInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, posInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_INFO_LENGTH_MISMATCH;
        __leave;
      }

      if (fileObject->CurrentByteOffset.QuadPart < 0) {
        status = STATUS_INVALID_PARAMETER;
        __leave;
      }

      // set the current file offset
      posInfo->CurrentByteOffset = fileObject->CurrentByteOffset;
      status = STATUS_SUCCESS;
      __leave;
    } break;
    case FileStreamInformation:
      if (!RequestContext->Dcb->UseAltStream) {
        DOKAN_LOG_FINE_IRP(RequestContext, "Alternate stream disabled");
        status = STATUS_INVALID_PARAMETER;
        __leave;
      }
      break;
    case FileNetworkPhysicalNameInformation: {
      // This info class is generally not worth passing to the DLL. It will be
      // filled in with info that is accessible to the driver.

      PFILE_NETWORK_PHYSICAL_NAME_INFORMATION netInfo;
      if (!PREPARE_OUTPUT(RequestContext->Irp, netInfo,
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      if (!AppendVarSizeOutputString(RequestContext->Irp, &netInfo->FileName,
                                     &fcb->FileName,
                                     /*UpdateInformationOnFailure=*/FALSE,
                                     /*FillSpaceWithPartialString=*/FALSE)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }
      status = STATUS_SUCCESS;
      __leave;
    }
    default:
      DOKAN_LOG_FINE_IRP(RequestContext, "Unsupported FileInfoClass %x", infoClass);
    }

    if (fcb != NULL && fcb->BlockUserModeDispatch) {
      status = STATUS_SUCCESS;
      __leave;
    }

    // if it is not treadted in swich case

    // calculate the length of EVENT_CONTEXT
    // sum of it's size and file name length
    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;

    eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;
    eventContext->Operation.File.FileInformationClass = infoClass;

    // bytes length which is able to be returned
    eventContext->Operation.File.BufferLength =
        RequestContext->IrpSp->Parameters.QueryFile.Length;

    // copy file name to EventContext from FCB
    eventContext->Operation.File.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.File.FileName, fcb->FileName.Buffer,
                  fcb->FileName.Length);

    // register this IRP to pending IRP list
    status = DokanRegisterPendingIrp(RequestContext, eventContext);

  } __finally {
    if (fcb)
      DokanFCBUnlock(fcb);
  }

  return status;
}

VOID DokanCompleteQueryInformation(__in PREQUEST_CONTEXT RequestContext,
                                   __in PEVENT_INFORMATION EventInfo) {
  ULONG bufferLen = 0;
  PVOID buffer = NULL;
  PDokanCCB ccb;

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p",
                     RequestContext->IrpSp->FileObject);

  ccb = RequestContext->IrpSp->FileObject->FsContext2;

  ASSERT(ccb != NULL);

  ccb->UserContext = EventInfo->Context;

  // where we shold copy FileInfo to
  buffer = RequestContext->Irp->AssociatedIrp.SystemBuffer;

  // available buffer size
  bufferLen = RequestContext->IrpSp->Parameters.QueryFile.Length;

  // buffer is not specified or short of size
  if (bufferLen == 0 || buffer == NULL || bufferLen < EventInfo->BufferLength) {
    RequestContext->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

  } else {

    //
    // we write FileInfo from user mode
    //
    ASSERT(buffer != NULL);

    RtlZeroMemory(buffer, bufferLen);
    RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);

    // written bytes
    RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;
    RequestContext->Irp->IoStatus.Status = EventInfo->Status;

    //Update file size to FCB
    if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status) &&
        (RequestContext->IrpSp->Parameters.QueryFile.FileInformationClass ==
             FileAllInformation ||
         RequestContext->IrpSp->Parameters.QueryFile.FileInformationClass ==
             FileStandardInformation ||
         RequestContext->IrpSp->Parameters.QueryFile.FileInformationClass ==
             FileNetworkOpenInformation)) {

      FSRTL_ADVANCED_FCB_HEADER *header =
          RequestContext->IrpSp->FileObject->FsContext;
      LONGLONG allocationSize = 0;
      LONGLONG fileSize = 0;

      ASSERT(header != NULL);

      if (RequestContext->IrpSp->Parameters.QueryFile.FileInformationClass ==
          FileAllInformation) {

        PFILE_ALL_INFORMATION allInfo = (PFILE_ALL_INFORMATION)buffer;
        allocationSize = allInfo->StandardInformation.AllocationSize.QuadPart;
        fileSize = allInfo->StandardInformation.EndOfFile.QuadPart;

        allInfo->PositionInformation.CurrentByteOffset =
            RequestContext->IrpSp->FileObject->CurrentByteOffset;

      } else if (RequestContext->IrpSp->Parameters.QueryFile
                     .FileInformationClass ==
                 FileStandardInformation) {

        PFILE_STANDARD_INFORMATION standardInfo =
            (PFILE_STANDARD_INFORMATION)buffer;
        allocationSize = standardInfo->AllocationSize.QuadPart;
        fileSize = standardInfo->EndOfFile.QuadPart;

      } else if (RequestContext->IrpSp->Parameters.QueryFile
                     .FileInformationClass ==
                 FileNetworkOpenInformation) {

        PFILE_NETWORK_OPEN_INFORMATION networkInfo =
            (PFILE_NETWORK_OPEN_INFORMATION)buffer;
        allocationSize = networkInfo->AllocationSize.QuadPart;
        fileSize = networkInfo->EndOfFile.QuadPart;
      }

      InterlockedExchange64(&header->AllocationSize.QuadPart, allocationSize);
      InterlockedExchange64(&header->FileSize.QuadPart, fileSize);

      DOKAN_LOG_FINE_IRP(RequestContext,
                         "AllocationSize: %llu, EndOfFile: %llu\n",
                         allocationSize, fileSize);
    }
  }
}

VOID FlushFcb(__in PREQUEST_CONTEXT RequestContext, __in PDokanFCB Fcb,
              __in_opt PFILE_OBJECT FileObject) {
  UNREFERENCED_PARAMETER(RequestContext);

  if (Fcb == NULL) {
    return;
  }

  if (Fcb->SectionObjectPointers.ImageSectionObject != NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext, "MmFlushImageSection FCB=%p FileCount=%lu.", Fcb,
                  Fcb->FileCount);
    MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite);
    DOKAN_LOG_FINE_IRP(RequestContext, "MmFlushImageSection done FCB=%p FileCount=%lu.", Fcb,
                  Fcb->FileCount);
  }

  if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext, "CcFlushCache FCB=%p FileCount=%lu.", Fcb,
                  Fcb->FileCount);

    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, NULL);

    DokanPagingIoLockRW(Fcb);
    DokanPagingIoUnlock(Fcb);

    CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
    if (FileObject != NULL) {
      DOKAN_LOG_FINE_IRP(RequestContext, "CcUninitializeCacheMap FileObject=%p", FileObject);
      CcUninitializeCacheMap(FileObject, NULL, NULL);
    }
    DOKAN_LOG_FINE_IRP(RequestContext, "CcFlushCache done FCB=%p FileCount=%lu.", Fcb,
                  Fcb->FileCount);
  }
}

VOID FlushAllCachedFcb(__in PREQUEST_CONTEXT RequestContext,
                       __in PDokanFCB fcbRelatedTo,
                       __in_opt PFILE_OBJECT fileObject) {
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PDokanFCB fcb = NULL;

  if (fcbRelatedTo == NULL) {
    return;
  }

  if (!DokanFCBFlagsIsSet(fcbRelatedTo, DOKAN_FILE_DIRECTORY)) {
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "FlushAllCachedFcb file passed in. Flush only: FCB=%p FileObject=%p.",
        fcbRelatedTo, fileObject);
    FlushFcb(RequestContext, fcbRelatedTo, fileObject);
    return;
  }

  DokanVCBLockRW(fcbRelatedTo->Vcb);

  listHead = &fcbRelatedTo->Vcb->NextFCB;

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {
    nextEntry = thisEntry->Flink;

    fcb = CONTAINING_RECORD(thisEntry, DokanFCB, NextFCB);

    if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "FCB=%p is directory so skip it.",
                    fcb);
      continue;
    }

    DOKAN_LOG_FINE_IRP(RequestContext, "Check \"%wZ\" if is related to \"%wZ\"", &fcb->FileName,
                  &fcbRelatedTo->FileName);

    if (StartsWith(&fcb->FileName, &fcbRelatedTo->FileName)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Flush \"%wZ\" if it is possible.",
                    &fcb->FileName);
      FlushFcb(RequestContext, fcb, NULL);
    }

    fcb = NULL;
  }

  DokanVCBUnlock(fcbRelatedTo->Vcb);

  DOKAN_LOG_FINE_IRP(RequestContext, "Finished");
}

NTSTATUS
DokanDispatchSetInformation(__in PREQUEST_CONTEXT RequestContext) {

  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PVOID buffer;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  ULONG eventLength;
  PFILE_OBJECT targetFileObject;
  PEVENT_CONTEXT eventContext;
  BOOLEAN isPagingIo = FALSE;
  BOOLEAN fcbLocked = FALSE;

  __try {
    fileObject = RequestContext->IrpSp->FileObject;
    DOKAN_LOG_FINE_IRP(
        RequestContext, "FileObject=%p InfoClass=%s", fileObject,
        DokanGetFileInformationClassStr(
            RequestContext->IrpSp->Parameters.SetFile.FileInformationClass));

    if (fileObject == NULL || !RequestContext->Vcb ||
        !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    ccb = (PDokanCCB)fileObject->FsContext2;
    ASSERT(ccb != NULL);

    buffer = RequestContext->Irp->AssociatedIrp.SystemBuffer;

    isPagingIo = (RequestContext->Irp->Flags & IRP_PAGING_IO);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);
    OplockDebugRecordMajorFunction(fcb, IRP_MJ_SET_INFORMATION);
    switch (RequestContext->IrpSp->Parameters.SetFile.FileInformationClass) {
    case FileAllocationInformation: {
      if ((fileObject->SectionObjectPointer != NULL) &&
          (fileObject->SectionObjectPointer->DataSectionObject != NULL)) {

        LARGE_INTEGER AllocationSize =
            ((PFILE_ALLOCATION_INFORMATION)buffer)->AllocationSize;
        if (AllocationSize.QuadPart <
                fcb->AdvancedFCBHeader.AllocationSize.QuadPart &&
            !MmCanFileBeTruncated(fileObject->SectionObjectPointer,
                                  &AllocationSize)) {
          status = STATUS_USER_MAPPED_FILE;
          __leave;
        }
        DOKAN_LOG_FINE_IRP(
            RequestContext, "AllocationSize %lld",
            ((PFILE_ALLOCATION_INFORMATION)buffer)->AllocationSize.QuadPart);
      }
    } break;
    case FileEndOfFileInformation: {
      if ((fileObject->SectionObjectPointer != NULL) &&
          (fileObject->SectionObjectPointer->DataSectionObject != NULL)) {

        PFILE_END_OF_FILE_INFORMATION pInfoEoF =
            (PFILE_END_OF_FILE_INFORMATION)buffer;

        if (pInfoEoF->EndOfFile.QuadPart <
                fcb->AdvancedFCBHeader.FileSize.QuadPart &&
            !MmCanFileBeTruncated(fileObject->SectionObjectPointer,
                                  &pInfoEoF->EndOfFile)) {
          status = STATUS_USER_MAPPED_FILE;
          __leave;
        }

        if (!isPagingIo) {

          CcFlushCache(&fcb->SectionObjectPointers, NULL, 0, NULL);

          DokanPagingIoLockRW(fcb);
          DokanPagingIoUnlock(fcb);

          CcPurgeCacheSection(&fcb->SectionObjectPointers, NULL, 0, FALSE);
        }
      }
      DOKAN_LOG_FINE_IRP(
          RequestContext, "EndOfFile %lld",
          ((PFILE_END_OF_FILE_INFORMATION)buffer)->EndOfFile.QuadPart);
    } break;
    case FilePositionInformation: {
      PFILE_POSITION_INFORMATION posInfo;

      posInfo = (PFILE_POSITION_INFORMATION)
                    RequestContext->Irp->AssociatedIrp.SystemBuffer;
      ASSERT(posInfo != NULL);

      DOKAN_LOG_FINE_IRP(RequestContext, "FilePositionInformation %lld",
                posInfo->CurrentByteOffset.QuadPart);
      fileObject->CurrentByteOffset = posInfo->CurrentByteOffset;

      status = STATUS_SUCCESS;

      __leave;
    } break;
    case FileRenameInformation:
    case FileRenameInformationEx:
      /* Flush any opened files before doing a rename
       * of the parent directory or the specific file
       */
      targetFileObject = RequestContext->IrpSp->Parameters.SetFile.FileObject;
      if (targetFileObject) {
        DOKAN_LOG_FINE_IRP(RequestContext, "TargetFileObject specified so perform flush");
        PDokanCCB targetCcb = (PDokanCCB)targetFileObject->FsContext2;
        ASSERT(targetCcb != NULL);
        FlushAllCachedFcb(RequestContext, targetCcb->Fcb, targetFileObject);
      }
      FlushAllCachedFcb(RequestContext, fcb, fileObject);
      break;
    default:
      DOKAN_LOG_FINE_IRP(
          RequestContext, "Unsupported FileInfoClass %x",
          RequestContext->IrpSp->Parameters.SetFile.FileInformationClass);
    }

    //
    // when this IRP is not handled in swich case
    //

    // calcurate the size of EVENT_CONTEXT
    // it is sum of file name length and size of FileInformation
    DokanFCBLockRW(fcb);
    fcbLocked = TRUE;

    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
    if (RequestContext->IrpSp->Parameters.SetFile.Length >
        MAXULONG - eventLength) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Invalid SetFile Length received");
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }
    eventLength += RequestContext->IrpSp->Parameters.SetFile.Length;

    targetFileObject = RequestContext->IrpSp->Parameters.SetFile.FileObject;
    if (targetFileObject) {
      DOKAN_LOG_FINE_IRP(RequestContext, "FileObject Specified %p \"%wZ\"", targetFileObject,
                    &(targetFileObject->FileName));
      if (targetFileObject->FileName.Length > MAXULONG - eventLength) {
        DOKAN_LOG_FINE_IRP(RequestContext, "Invalid FileObject FileName Length received");
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }
      eventLength += targetFileObject->FileName.Length;
    }

    eventContext = AllocateEventContext(RequestContext, eventLength, ccb);
    if (eventContext == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    eventContext->Context = ccb->UserContext;

    eventContext->Operation.SetFile.FileInformationClass =
        RequestContext->IrpSp->Parameters.SetFile.FileInformationClass;

    // the size of FileInformation
    eventContext->Operation.SetFile.BufferLength =
        RequestContext->IrpSp->Parameters.SetFile.Length;

    // the offset from begining of structure to fill FileInfo
    eventContext->Operation.SetFile.BufferOffset =
        FIELD_OFFSET(EVENT_CONTEXT, Operation.SetFile.FileName[0]) +
        fcb->FileName.Length + sizeof(WCHAR); // the last null char

    BOOLEAN isRenameOrLink =
        RequestContext->IrpSp->Parameters.SetFile.FileInformationClass ==
            FileRenameInformation ||
        RequestContext->IrpSp->Parameters.SetFile.FileInformationClass ==
            FileLinkInformation;

    if (!isRenameOrLink) {
      // copy FileInformation
      RtlCopyMemory(
          (PCHAR)eventContext + eventContext->Operation.SetFile.BufferOffset,
          RequestContext->Irp->AssociatedIrp.SystemBuffer,
          RequestContext->IrpSp->Parameters.SetFile.Length);
    }

    if (isRenameOrLink) {
      // We need to hanle FileRenameInformation separetly because the structure
      // of FILE_RENAME_INFORMATION
      // has HANDLE type field, which size is different in 32 bit and 64 bit
      // environment.
      // This cases problems when driver is 64 bit and user mode library is 32
      // bit.
      PFILE_RENAME_INFORMATION renameInfo =
          (PFILE_RENAME_INFORMATION)
              RequestContext->Irp->AssociatedIrp.SystemBuffer;
      PDOKAN_RENAME_INFORMATION renameContext = (PDOKAN_RENAME_INFORMATION)(
          (PCHAR)eventContext + eventContext->Operation.SetFile.BufferOffset);

      // This code assumes FILE_RENAME_INFORMATION and FILE_LINK_INFORMATION
      // have
      // the same typse and fields.
      ASSERT(sizeof(FILE_RENAME_INFORMATION) == sizeof(FILE_LINK_INFORMATION));

      renameContext->ReplaceIfExists = renameInfo->ReplaceIfExists;
      renameContext->FileNameLength = renameInfo->FileNameLength;
      RtlCopyMemory(renameContext->FileName, renameInfo->FileName,
                    renameInfo->FileNameLength);

      if (targetFileObject != NULL) {
        // if Parameters.SetFile.FileObject is specified, replace
        // FILE_RENAME_INFO's file name by
        // FileObject's file name. The buffer size is already adjusted.

        DOKAN_LOG_FINE_IRP(RequestContext, "RenameContext->FileNameLength %d",
                      renameContext->FileNameLength);
        DOKAN_LOG_FINE_IRP(RequestContext, "RenameContext->FileName %ws",
                      renameContext->FileName);
        RtlZeroMemory(renameContext->FileName, renameContext->FileNameLength);

        PFILE_OBJECT parentFileObject = targetFileObject->RelatedFileObject;
        if (parentFileObject != NULL) {
          RtlCopyMemory(renameContext->FileName,
                        parentFileObject->FileName.Buffer,
                        parentFileObject->FileName.Length);

          RtlStringCchCatW(renameContext->FileName, NTSTRSAFE_MAX_CCH, L"\\");
          RtlStringCchCatW(renameContext->FileName, NTSTRSAFE_MAX_CCH,
                           targetFileObject->FileName.Buffer);
          renameContext->FileNameLength = targetFileObject->FileName.Length +
                                          parentFileObject->FileName.Length +
                                          sizeof(WCHAR);
        } else {
          RtlCopyMemory(renameContext->FileName,
                        targetFileObject->FileName.Buffer,
                        targetFileObject->FileName.Length);
          renameContext->FileNameLength = targetFileObject->FileName.Length;
        }
      }

      if (RequestContext->IrpSp->Parameters.SetFile.FileInformationClass ==
          FileRenameInformation) {
        DOKAN_LOG_FINE_IRP(RequestContext, "Rename: \"%wZ\" => \"%ls\", Fcb=%p FileCount = %u",
                      fcb->FileName, renameContext->FileName, fcb,
                      (ULONG)fcb->FileCount);
      }
    }

    // copy the file name
    eventContext->Operation.SetFile.FileNameLength = fcb->FileName.Length;
    RtlCopyMemory(eventContext->Operation.SetFile.FileName,
                  fcb->FileName.Buffer, fcb->FileName.Length);

    // FsRtlCheckOpLock is called with non-NULL completion routine - not blocking.
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

    // register this IRP to waiting IRP list and make it pending status
    status = DokanRegisterPendingIrp(RequestContext, eventContext);

  } __finally {
    if (fcbLocked)
      DokanFCBUnlock(fcb);
  }

  return status;
}

// Returns the last index, |i|, so that [0, i] represents the range of the path
// to the parent directory. For example, if |fileName| is |C:\temp\text.txt|,
// returns 7 (the index of |\| right before |text.txt|).
//
// Returns -1 if no '\\' is found,
LONG GetParentDirectoryEndingIndex(PUNICODE_STRING fileName) {
  if (fileName->Length == 0) {
    return -1;
  }
  // If the path ends with L'\\' (in which case, this is a directory, that last
  // '\\' character can be ignored.)
  USHORT lastIndex = fileName->Length / sizeof(WCHAR) - 1;
  if (fileName->Buffer[lastIndex] == L'\\') {
    lastIndex--;
  }
  for (LONG index = lastIndex; index >= 0; index--) {
    if (fileName->Buffer[index] == L'\\') {
      return index;
    }
  }
  // There is no '\\' found.
  return -1;
}

// Returns |TRUE| if |fileName1| and |fileName2| represent paths to two
// files/folders that are in the same directory.
BOOLEAN IsInSameDirectory(PUNICODE_STRING fileName1,
                          PUNICODE_STRING fileName2) {
  LONG parentEndingIndex = GetParentDirectoryEndingIndex(fileName1);
  if (parentEndingIndex != GetParentDirectoryEndingIndex(fileName2)) {
    return FALSE;
  }
  for (LONG i = 0; i < parentEndingIndex; i++) {
    // TODO(ttdinhtrong): This code assumes case sensitive, which is not always
    // true. As of now we do not know if the user is in case sensitive or case
    // insensitive mode.
    if (fileName1->Buffer[i] != fileName2->Buffer[i]) {
      return FALSE;
    }
  }
  return TRUE;
}

VOID DokanCompleteSetInformation(__in PREQUEST_CONTEXT RequestContext,
                                 __in PEVENT_INFORMATION EventInfo) {
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  UNICODE_STRING oldFileName;
  BOOLEAN fcbLocked = FALSE;
  BOOLEAN vcbLocked = FALSE;
  FILE_INFORMATION_CLASS infoClass;

  __try {
    RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;
    RequestContext->Irp->IoStatus.Status = EventInfo->Status;

    ccb = RequestContext->IrpSp->FileObject->FsContext2;
    ASSERT(ccb != NULL);

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&ccb->Resource, TRUE);

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    infoClass = RequestContext->IrpSp->Parameters.SetFile.FileInformationClass;
    DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p infoClass=%s",
                       RequestContext->IrpSp->FileObject,
                       DokanGetFileInformationClassStr(infoClass));

    // Note that we do not acquire the resource for paging file
    // operations in order to avoid deadlock with Mm
    if (!(RequestContext->Irp->Flags & IRP_PAGING_IO)) {
      // If we are going to change the FileName on the FCB, then we want the VCB
      // locked so that we don't race with the loop in create.c that searches
      // currently open FCBs for a matching name. However, we need to lock that
      // before the FCB so that the lock order is consistent everywhere.
      if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status) &&
          infoClass == FileRenameInformation) {
        DokanVCBLockRW(fcb->Vcb);
        vcbLocked = TRUE;
      }
      DokanFCBLockRW(fcb);
      fcbLocked = TRUE;
    }

    ccb->UserContext = EventInfo->Context;

    RtlZeroMemory(&oldFileName, sizeof(UNICODE_STRING));

    if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status)) {
      if (infoClass == FileDispositionInformation ||
          infoClass == FileDispositionInformationEx) {
        if (EventInfo->Operation.Delete.DeleteOnClose) {

          if (!MmFlushImageSection(&fcb->SectionObjectPointers,
                                   MmFlushForDelete)) {
            DOKAN_LOG_FINE_IRP(RequestContext,
                               "Cannot delete user mapped image");
            RequestContext->Irp->IoStatus.Status = STATUS_CANNOT_DELETE;
          } else {
            DokanCCBFlagsSetBit(ccb, DOKAN_DELETE_ON_CLOSE);
            DokanFCBFlagsSetBit(fcb, DOKAN_DELETE_ON_CLOSE);
            DOKAN_LOG_FINE_IRP(RequestContext,
                               "FileObject->DeletePending = TRUE");
            RequestContext->IrpSp->FileObject->DeletePending = TRUE;
          }

        } else {
          DokanCCBFlagsClearBit(ccb, DOKAN_DELETE_ON_CLOSE);
          DokanFCBFlagsClearBit(fcb, DOKAN_DELETE_ON_CLOSE);
          DOKAN_LOG_FINE_IRP(RequestContext,
                             "FileObject->DeletePending = FALSE");
          RequestContext->IrpSp->FileObject->DeletePending = FALSE;
        }
      }

      // if rename is executed, reassign the file name
      if (infoClass == FileRenameInformation ||
          infoClass == FileRenameInformationEx) {
        PVOID buffer = NULL;

        // this is used to inform rename in the bellow switch case
        oldFileName.Buffer = fcb->FileName.Buffer;
        oldFileName.Length = (USHORT)fcb->FileName.Length;
        oldFileName.MaximumLength = (USHORT)fcb->FileName.Length;

        // copy new file name
        buffer = DokanAllocZero(EventInfo->BufferLength + sizeof(WCHAR));
        if (buffer == NULL) {
          RequestContext->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
          ExReleaseResourceLite(&ccb->Resource);
          KeLeaveCriticalRegion();
          __leave;
        }

        fcb->FileName.Buffer = buffer;
        ASSERT(fcb->FileName.Buffer != NULL);

        RtlCopyMemory(fcb->FileName.Buffer, EventInfo->Buffer,
                      EventInfo->BufferLength);

        fcb->FileName.Length = (USHORT)EventInfo->BufferLength;
        fcb->FileName.MaximumLength = (USHORT)EventInfo->BufferLength;
        DOKAN_LOG_FINE_IRP(RequestContext, "Fcb=%p renamed \"%wZ\"", fcb,
                           &fcb->FileName);
      }
    }

    ExReleaseResourceLite(&ccb->Resource);
    KeLeaveCriticalRegion();

    if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status)) {
      switch (RequestContext->IrpSp->Parameters.SetFile.FileInformationClass) {
        case FileAllocationInformation:
          DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_SIZE,
                                  FILE_ACTION_MODIFIED);
          break;
        case FileBasicInformation:
          DokanNotifyReportChange(
              RequestContext, fcb,
              FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_LAST_WRITE |
                  FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_CREATION,
              FILE_ACTION_MODIFIED);
          break;
        case FileDispositionInformation:
        case FileDispositionInformationEx:
         if (RequestContext->IrpSp->FileObject->DeletePending) {
            if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
              DokanNotifyReportChange(RequestContext, fcb,
                                      FILE_NOTIFY_CHANGE_DIR_NAME,
                                      FILE_ACTION_REMOVED);
            } else {
              DokanNotifyReportChange(RequestContext, fcb,
                                      FILE_NOTIFY_CHANGE_FILE_NAME,
                                      FILE_ACTION_REMOVED);
            }
          }
          break;
        case FileEndOfFileInformation:
          DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_SIZE,
                                  FILE_ACTION_MODIFIED);
          break;
        case FileLinkInformation:
          // TODO: should check whether this is a directory
          // TODO: should notify new link name
          // DokanNotifyReportChange(vcb, ccb, FILE_NOTIFY_CHANGE_FILE_NAME,
          // FILE_ACTION_ADDED);
          break;
        case FileRenameInformationEx:
        case FileRenameInformation: {
          if (IsInSameDirectory(&oldFileName, &fcb->FileName)) {
            DokanNotifyReportChange0(
                RequestContext, fcb, &oldFileName,
                DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)
                    ? FILE_NOTIFY_CHANGE_DIR_NAME
                    : FILE_NOTIFY_CHANGE_FILE_NAME,
                FILE_ACTION_RENAMED_OLD_NAME);
            DokanNotifyReportChange(
                RequestContext, fcb,
                DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)
                    ? FILE_NOTIFY_CHANGE_DIR_NAME
                    : FILE_NOTIFY_CHANGE_FILE_NAME,
                FILE_ACTION_RENAMED_NEW_NAME);
          } else {
            DokanNotifyReportChange0(
                RequestContext, fcb, &oldFileName,
                DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)
                    ? FILE_NOTIFY_CHANGE_DIR_NAME
                    : FILE_NOTIFY_CHANGE_FILE_NAME,
                FILE_ACTION_REMOVED);
            DokanNotifyReportChange(
                RequestContext, fcb,
                DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)
                    ? FILE_NOTIFY_CHANGE_DIR_NAME
                    : FILE_NOTIFY_CHANGE_FILE_NAME,
                FILE_ACTION_ADDED);
          }
          // free old file name
          ExFreePool(oldFileName.Buffer);
        } break;
        case FileValidDataLengthInformation:
          DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_SIZE,
                                  FILE_ACTION_MODIFIED);
          break;
      }
    }

  } __finally {
    if (fcbLocked) {
      DokanFCBUnlock(fcb);
    }
    if (vcbLocked) {
      DokanVCBUnlock(fcb->Vcb);
    }
  }
}
