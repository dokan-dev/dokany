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
#include "util/fcb.h"
#include "util/irp_buffer_helper.h"
#include "util/str.h"

NTSTATUS FillNameInformation(__in PREQUEST_CONTEXT RequestContext,
                             __in PDokanFCB Fcb,
                             __in PFILE_NAME_INFORMATION NameInfo) {
  PUNICODE_STRING fileName = &Fcb->FileName;
  PCHAR dest = (PCHAR)&NameInfo->FileName;
  NameInfo->FileNameLength = fileName->Length;

  BOOLEAN isNetworkDevice = (RequestContext->Dcb->VolumeDeviceType ==
                             FILE_DEVICE_NETWORK_FILE_SYSTEM);
  if (isNetworkDevice) {
    PUNICODE_STRING devicePath = RequestContext->Dcb->UNCName->Length
                                     ? RequestContext->Dcb->UNCName
                                     : RequestContext->Dcb->DiskDeviceName;
    NameInfo->FileNameLength += devicePath->Length;

    if (!AppendVarSizeOutputString(RequestContext->Irp, dest, devicePath,
                                   /*UpdateInformationOnFailure=*/FALSE,
                                   /*FillSpaceWithPartialString=*/TRUE)) {
      return STATUS_BUFFER_OVERFLOW;
    }
    dest += devicePath->Length;
  }

  if (!AppendVarSizeOutputString(RequestContext->Irp, dest, fileName,
                                 /*UpdateInformationOnFailure=*/FALSE,
                                 /*FillSpaceWithPartialString=*/TRUE)) {
    return STATUS_BUFFER_OVERFLOW;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
DokanDispatchQueryInformation(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject;
  FILE_INFORMATION_CLASS infoClass;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;
  BOOLEAN fcbLocked = FALSE;

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
    switch (infoClass) {
    case FileAllInformation: {
      PFILE_ALL_INFORMATION allInfo;
      if (!PrepareOutputHelper(
              RequestContext->Irp, &allInfo,
              FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName),
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        __leave;
      }
    } break;
    case FileNormalizedNameInformation:
    case FileNameInformation: {
      PFILE_NAME_INFORMATION nameInfo;
      if (!PrepareOutputHelper(RequestContext->Irp, &nameInfo,
                               FIELD_OFFSET(FILE_NAME_INFORMATION, FileName),
                          /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_TOO_SMALL;
        __leave;
      }

      if (!fcbLocked) {
        DokanFCBLockRO(fcb);
        fcbLocked = TRUE;
      }
      status = FillNameInformation(RequestContext, fcb, nameInfo);
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
      if (!PrepareOutputHelper(
              RequestContext->Irp, &netInfo,
              FIELD_OFFSET(FILE_NETWORK_PHYSICAL_NAME_INFORMATION, FileName),
              /*SetInformationOnFailure=*/FALSE)) {
        status = STATUS_BUFFER_OVERFLOW;
        __leave;
      }

      if (!fcbLocked) {
        DokanFCBLockRO(fcb);
        fcbLocked = TRUE;
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

    if (!fcbLocked) {
      DokanFCBLockRO(fcb);
      fcbLocked = TRUE;
    }

    // If the request is not handled by the switch case we send it to userland.
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
    if (fcbLocked)
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

        DokanFCBLockRO(ccb->Fcb);
        RequestContext->Irp->IoStatus.Status = FillNameInformation(
            RequestContext, ccb->Fcb, &allInfo->NameInformation);
        DokanFCBUnlock(ccb->Fcb);

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
                         "AllocationSize: %llu, EndOfFile: %llu",
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

VOID FlushIfDescendant(__in PREQUEST_CONTEXT RequestContext,
                       __in PDokanFCB FcbRelatedTo, __in PDokanFCB Fcb) {
  if (DokanFCBFlagsIsSet(Fcb, DOKAN_FILE_DIRECTORY)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "FCB=%p is directory so skip it.", Fcb);
    return;
  }

  DOKAN_LOG_FINE_IRP(RequestContext, "Check \"%wZ\" if is related to \"%wZ\"",
                     &Fcb->FileName, &FcbRelatedTo->FileName);

  if (!StartsWith(&Fcb->FileName, &FcbRelatedTo->FileName)) {
    return;
  }
  DOKAN_LOG_FINE_IRP(RequestContext, "Flush \"%wZ\" if it is possible.",
                     &Fcb->FileName);
  FlushFcb(RequestContext, Fcb, NULL);
}

VOID FlushAllCachedFcb(__in PREQUEST_CONTEXT RequestContext,
                       __in PDokanFCB FcbRelatedTo,
                       __in_opt PFILE_OBJECT FileObject) {
  if (FcbRelatedTo == NULL) {
    return;
  }

  if (!DokanFCBFlagsIsSet(FcbRelatedTo, DOKAN_FILE_DIRECTORY)) {
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "FlushAllCachedFcb file passed in. Flush only: FCB=%p FileObject=%p.",
        FcbRelatedTo, FileObject);
    FlushFcb(RequestContext, FcbRelatedTo, FileObject);
    return;
  }

  DokanVCBLockRW(FcbRelatedTo->Vcb);

  for (PDokanFCB *fcbInTable = (PDokanFCB *)RtlEnumerateGenericTableAvl(
           &RequestContext->Vcb->FcbTable, /*Restart=*/TRUE);
       fcbInTable != NULL;
       fcbInTable = (PDokanFCB *)RtlEnumerateGenericTableAvl(
           &RequestContext->Vcb->FcbTable, /*Restart=*/FALSE)) {
    FlushIfDescendant(RequestContext, FcbRelatedTo, *fcbInTable);
  }

  DokanVCBUnlock(FcbRelatedTo->Vcb);

  DOKAN_LOG_FINE_IRP(RequestContext, "Finished");
}

// Need to be called with RootDirectoryFcb locked RO.
// If EventContext is NULL, the function will do a Dry run where it will only
// calculate the renamed FileName size and return it.
ULONG PopulateRenameEventInformations(__in PREQUEST_CONTEXT RequestContext,
                                      __in PDokanFCB Fcb,
                                      __in_opt PEVENT_CONTEXT EventContext,
                                      __in_opt PDokanFCB RootDirectoryFcb) {
  ULONG fileNameLength = 0;
  PFILE_OBJECT targetFileObject =
      RequestContext->IrpSp->Parameters.SetFile.FileObject;

  // We need to hanle FileRenameInformation separetly because the structure
  // of FILE_RENAME_INFORMATION has HANDLE type field, which size is
  // different in 32 bit and 64 bit environment. This cases problems when
  // driver is 64 bit and user mode library is 32 bit.
  PFILE_RENAME_INFORMATION renameInfo =
      (PFILE_RENAME_INFORMATION)RequestContext->Irp->AssociatedIrp.SystemBuffer;
  PDOKAN_RENAME_INFORMATION renameContext = NULL;

  if (EventContext) {
    renameContext = (PDOKAN_RENAME_INFORMATION)(
        (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
    renameContext->ReplaceIfExists = renameInfo->ReplaceIfExists;
  }

  // It is valid to provide a FileName ending with '\'. Clean them up.
  ULONG renameInfoFileNameLength = renameInfo->FileNameLength;
  while (renameInfoFileNameLength > sizeof(WCHAR) &&
         renameInfo->FileName[renameInfoFileNameLength / sizeof(WCHAR) - 1] ==
             L'\\')
    renameInfoFileNameLength -= sizeof(WCHAR);

  if (targetFileObject == NULL) {
    // Simple rename in the same directory: RenameInfo only contains the
    // filename. We need to build the full path from the Fcb.
    ULONG fcbFileNamePos = Fcb->FileName.Length / sizeof(WCHAR) - 1;
    BOOLEAN isAlternateRename = renameInfo->FileNameLength > sizeof(WCHAR) &&
                                renameInfo->FileName[0] == L':';
    WCHAR targetWchar = isAlternateRename ? L':' : L'\\';
    BOOLEAN isIgnoreTargetWchar = !isAlternateRename;
    fcbFileNamePos = DokanSearchWcharinUnicodeStringWithUlong(
        &Fcb->FileName, targetWchar, fcbFileNamePos, isIgnoreTargetWchar);
    if (isAlternateRename && fcbFileNamePos == 0) {
      // Here FCB doesn't contain the $DATA stream name but it is actually
      // renamed so we just need to append the stream.
      fcbFileNamePos = Fcb->FileName.Length / sizeof(WCHAR);
    }
    fileNameLength = fcbFileNamePos * sizeof(WCHAR) + renameInfoFileNameLength;
    if (renameContext) {
      RtlCopyMemory(renameContext->FileName, Fcb->FileName.Buffer,
                    fcbFileNamePos * sizeof(WCHAR));
      RtlCopyMemory(renameContext->FileName + fcbFileNamePos,
                    renameInfo->FileName, renameInfoFileNameLength);
    }
  } else if (renameInfo->RootDirectory == NULL || !RootDirectoryFcb) {
    // Fully qualified rename: TargetFileObject hold the full path
    // destination
    fileNameLength = targetFileObject->FileName.Length;
    if (renameContext) {
      RtlCopyMemory(renameContext->FileName, targetFileObject->FileName.Buffer,
                    targetFileObject->FileName.Length);
    }
  } else {
    // Relative rename: RenameInfo only contains the filename. We need to
    // build the full path with the RootDirectory. Using
    // TargetFileObject->RelatedFileObject is possible but not enough in some
    // cases.
    // Note: FASTFAT does not support this type of rename but NTFS does.
    PUNICODE_STRING rootFileName = &RootDirectoryFcb->FileName;
    BOOLEAN addSeparator =
        rootFileName->Buffer[rootFileName->Length / sizeof(WCHAR) - 1] != L'\\';
    fileNameLength = rootFileName->Length + (addSeparator ? sizeof(WCHAR) : 0);
    if (renameContext) {
      RtlCopyMemory(renameContext->FileName, rootFileName->Buffer,
                    rootFileName->Length);
      if (addSeparator) {
        renameContext->FileName[rootFileName->Length / sizeof(WCHAR)] = L'\\';
      }
      RtlCopyMemory((PCHAR)renameContext->FileName + fileNameLength,
                    renameInfo->FileName, renameInfoFileNameLength);
    }
    fileNameLength += renameInfoFileNameLength;
  }

  if (renameContext) {
    renameContext->FileNameLength = fileNameLength;
    DOKAN_LOG_FINE_IRP(
        RequestContext, "Rename: \"%wZ\" => \"%ls\", Fcb=%p FileCount = %u",
        Fcb->FileName, renameContext->FileName, Fcb, (ULONG)Fcb->FileCount);
  }
  return fileNameLength;
}

NTSTATUS
DokanDispatchSetInformation(__in PREQUEST_CONTEXT RequestContext) {

  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PVOID buffer;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  ULONG eventLength;
  BOOLEAN isRename = FALSE;
  PFILE_OBJECT targetFileObject = NULL;
  PDokanFCB rootDirectoryFcb = NULL;
  BOOLEAN rootDirectoryFcbLocked = FALSE;
  PFILE_OBJECT rootDirObject = NULL;
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
        isRename = TRUE;
      /* Flush any opened files before doing a rename
       * of the parent directory or the specific file
       */
      targetFileObject = RequestContext->IrpSp->Parameters.SetFile.FileObject;
      if (targetFileObject != NULL) {
          DOKAN_LOG_FINE_IRP(RequestContext,
                             "FileObject Specified so perform flush %p \"%wZ\"",
                             targetFileObject, &(targetFileObject->FileName));
        PDokanCCB targetCcb = (PDokanCCB)targetFileObject->FsContext2;
        ASSERT(targetCcb != NULL);
        PDokanFCB targetFcb = (PDokanFCB)targetCcb->Fcb;
        FlushAllCachedFcb(RequestContext, targetFcb, targetFileObject);
      }
      FlushAllCachedFcb(RequestContext, fcb, fileObject);
      break;
    default:
      DOKAN_LOG_FINE_IRP(
          RequestContext, "Unsupported FileInfoClass %x",
          RequestContext->IrpSp->Parameters.SetFile.FileInformationClass);
    }

    DokanFCBLockRW(fcb);
    fcbLocked = TRUE;

    // When this IRP is not handled in switch case, calculate the size of
    // EVENT_CONTEXT: FileName length + FileInformation buffer size
    eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
    if (RequestContext->IrpSp->Parameters.SetFile.Length >
        MAXULONG - eventLength) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Invalid SetFile Length received");
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }
    eventLength += RequestContext->IrpSp->Parameters.SetFile.Length;

    if (isRename) {
      PFILE_RENAME_INFORMATION renameInfo =
          (PFILE_RENAME_INFORMATION)
              RequestContext->Irp->AssociatedIrp.SystemBuffer;
      if (renameInfo->RootDirectory != NULL) {
        // Relative rename
        status = ObReferenceObjectByHandle(
            renameInfo->RootDirectory, STANDARD_RIGHTS_READ, *IoFileObjectType,
            KernelMode, (PVOID *)&rootDirObject, NULL);
        if (!NT_SUCCESS(status)) {
          DOKAN_LOG_FINE_IRP(RequestContext,
                             "Failed to get RootDirectory object - %s",
                             DokanGetNTSTATUSStr(status));
          __leave;
        }
        // TODO(adrienj): Create a helper to get FCB directly from FsContext2
        PDokanCCB rootDirectoryCcb = (PDokanCCB)rootDirObject->FsContext2;
        if (DokanCheckCCB(RequestContext, rootDirectoryCcb)) {
          rootDirectoryFcb = (PDokanFCB)rootDirectoryCcb->Fcb;
          ASSERT(rootDirectoryFcb != NULL);
          DokanFCBLockRO(rootDirectoryFcb);
          rootDirectoryFcbLocked = TRUE;
          DOKAN_LOG_FINE_IRP(RequestContext, "RootDirectory FCB %p \"%wZ\"",
                             rootDirectoryFcb, rootDirectoryFcb->FileName);
          // NTFS does not seem to support relative stream rename. In our case
          // we do our best but having a FileName without the base is clearly
          // invalid.
          if (renameInfo->FileNameLength >= sizeof(WCHAR) &&
              renameInfo->FileName[0] == L':') {
            status = STATUS_INVALID_PARAMETER;
            __leave;
          }
        }
      }
      // This is a dry run just to get the needed size to AllocateEventContext
      ULONG renameFileNameLength = PopulateRenameEventInformations(
          RequestContext, fcb, NULL, rootDirectoryFcb);
      if (renameFileNameLength > MAXULONG - eventLength) {
        DOKAN_LOG_FINE_IRP(RequestContext,
                           "Invalid destination FileName length %lu",
                           renameFileNameLength);
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }
      eventLength += renameFileNameLength;
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

    if (isRename) {
      PopulateRenameEventInformations(RequestContext, fcb, eventContext,
                                      rootDirectoryFcb);
    } else {
      // copy FileInformation
      RtlCopyMemory(
          (PCHAR)eventContext + eventContext->Operation.SetFile.BufferOffset,
          RequestContext->Irp->AssociatedIrp.SystemBuffer,
          RequestContext->IrpSp->Parameters.SetFile.Length);
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
    if (rootDirectoryFcbLocked) {
      DokanFCBUnlock(rootDirectoryFcb);
      ObDereferenceObject(rootDirObject);
    }
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

    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);

    infoClass = RequestContext->IrpSp->Parameters.SetFile.FileInformationClass;
    DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p infoClass=%s",
                       RequestContext->IrpSp->FileObject,
                       DokanGetFileInformationClassStr(infoClass));

    ccb->UserContext = EventInfo->Context;

    if (!NT_SUCCESS(RequestContext->Irp->IoStatus.Status)) {
      __leave;
    }

    // Note that we do not acquire the resource for paging file
    // operations in order to avoid deadlock with Mm
    if (!(RequestContext->Irp->Flags & IRP_PAGING_IO)) {
      // If we are going to change the FileName on the FCB, then we want the VCB
      // locked so that we don't race with the loop in create.c that searches
      // currently open FCBs for a matching name. However, we need to lock that
      // before the FCB so that the lock order is consistent everywhere.
      if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status) &&
          infoClass == FileRenameInformation) {
        DokanVCBLockRW(RequestContext->Vcb);
        vcbLocked = TRUE;
      }
      DokanFCBLockRW(fcb);
      fcbLocked = TRUE;
    }

    switch (infoClass) {
    case FileDispositionInformation:
    case FileDispositionInformationEx: {
      if (EventInfo->Operation.Delete.DeleteOnClose) {
        if (!MmFlushImageSection(&fcb->SectionObjectPointers,
                                 MmFlushForDelete)) {
          DOKAN_LOG_FINE_IRP(RequestContext, "Cannot delete user mapped image");
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
        DOKAN_LOG_FINE_IRP(RequestContext, "FileObject->DeletePending = FALSE");
        RequestContext->IrpSp->FileObject->DeletePending = FALSE;
      }
      break;
    }
    case FileRenameInformation:
    case FileRenameInformationEx: {
      // Process rename
      oldFileName =
          DokanWrapUnicodeString(fcb->FileName.Buffer, fcb->FileName.Length);
      // Copy new file name
      PVOID buffer = DokanAllocZero(EventInfo->BufferLength + sizeof(WCHAR));
      if (buffer == NULL) {
        RequestContext->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }
      RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);
      DokanRenameFcb(RequestContext, fcb, buffer,
                     (USHORT)EventInfo->BufferLength);
      DOKAN_LOG_FINE_IRP(RequestContext, "Fcb=%p renamed \"%wZ\"", fcb,
                         &fcb->FileName);
      break;
    }
    }

    switch (infoClass) {
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
    case FileRenameInformation:
    case FileRenameInformationEx: {
      if (IsInSameDirectory(&oldFileName, &fcb->FileName)) {
        DokanNotifyReportChange0(RequestContext, fcb, &oldFileName,
                                 DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)
                                     ? FILE_NOTIFY_CHANGE_DIR_NAME
                                     : FILE_NOTIFY_CHANGE_FILE_NAME,
                                 FILE_ACTION_RENAMED_OLD_NAME);
        DokanNotifyReportChange(RequestContext, fcb,
                                DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)
                                    ? FILE_NOTIFY_CHANGE_DIR_NAME
                                    : FILE_NOTIFY_CHANGE_FILE_NAME,
                                FILE_ACTION_RENAMED_NEW_NAME);
      } else {
        DokanNotifyReportChange0(RequestContext, fcb, &oldFileName,
                                 DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)
                                     ? FILE_NOTIFY_CHANGE_DIR_NAME
                                     : FILE_NOTIFY_CHANGE_FILE_NAME,
                                 FILE_ACTION_REMOVED);
        DokanNotifyReportChange(RequestContext, fcb,
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

  } __finally {
    if (fcbLocked) {
      DokanFCBUnlock(fcb);
    }
    if (vcbLocked) {
      DokanVCBUnlock(RequestContext->Vcb);
    }
  }
}