/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include "dokani.h"
#include "fileinfo.h"

NTSTATUS
DokanSetAllocationInformation(PEVENT_CONTEXT EventContext,
                              PDOKAN_FILE_INFO FileInfo,
                              PDOKAN_OPERATIONS DokanOperations) {
  PFILE_ALLOCATION_INFORMATION allocInfo = (PFILE_ALLOCATION_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  // A file's allocation size and end-of-file position are independent of each
  // other,
  // with the following exception: The end-of-file position must always be less
  // than
  // or equal to the allocation size. If the allocation size is set to a value
  // that
  // is less than the end-of-file position, the end-of-file position is
  // automatically
  // adjusted to match the allocation size.
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  if (DokanOperations->SetAllocationSize) {
    status = DokanOperations->SetAllocationSize(
        EventContext->Operation.SetFile.FileName,
        allocInfo->AllocationSize.QuadPart, FileInfo);
  }

  return status;
}

NTSTATUS
DokanSetBasicInformation(PEVENT_CONTEXT EventContext, PDOKAN_FILE_INFO FileInfo,
                         PDOKAN_OPERATIONS DokanOperations) {
  FILETIME creation, lastAccess, lastWrite;
  NTSTATUS status;

  PFILE_BASIC_INFORMATION basicInfo = (PFILE_BASIC_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  if (!DokanOperations->SetFileAttributes)
    return STATUS_NOT_IMPLEMENTED;

  if (!DokanOperations->SetFileTime)
    return STATUS_NOT_IMPLEMENTED;

  status = DokanOperations->SetFileAttributes(
      EventContext->Operation.SetFile.FileName, basicInfo->FileAttributes,
      FileInfo);

  if (status != STATUS_SUCCESS)
    return status;

  creation.dwLowDateTime = basicInfo->CreationTime.LowPart;
  creation.dwHighDateTime = basicInfo->CreationTime.HighPart;
  lastAccess.dwLowDateTime = basicInfo->LastAccessTime.LowPart;
  lastAccess.dwHighDateTime = basicInfo->LastAccessTime.HighPart;
  lastWrite.dwLowDateTime = basicInfo->LastWriteTime.LowPart;
  lastWrite.dwHighDateTime = basicInfo->LastWriteTime.HighPart;

  return DokanOperations->SetFileTime(EventContext->Operation.SetFile.FileName,
                                      &creation, &lastAccess, &lastWrite,
                                      FileInfo);
}

NTSTATUS
DokanSetDispositionInformation(PEVENT_CONTEXT EventContext,
                               PDOKAN_FILE_INFO FileInfo,
                               PDOKAN_OPERATIONS DokanOperations) {

  BOOLEAN DeleteFileFlag = FALSE;
  NTSTATUS result;

  switch (EventContext->Operation.SetFile.FileInformationClass) {
  case FileDispositionInformation: {
    PFILE_DISPOSITION_INFORMATION dispositionInfo =
        (PFILE_DISPOSITION_INFORMATION)(
            (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
    DeleteFileFlag = dispositionInfo->DeleteFile;
  } break;
  case FileDispositionInformationEx: {
    PFILE_DISPOSITION_INFORMATION_EX dispositionexInfo =
        (PFILE_DISPOSITION_INFORMATION_EX)(
            (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

    DeleteFileFlag = (dispositionexInfo->Flags & FILE_DISPOSITION_DELETE) != 0;
  } break;
  default:
    return STATUS_INVALID_PARAMETER;
  }

  if (!DokanOperations->DeleteFile || !DokanOperations->DeleteDirectory)
    return STATUS_NOT_IMPLEMENTED;

  if (DeleteFileFlag == FileInfo->DeleteOnClose) {
    return STATUS_SUCCESS;
  }

  if (DokanOperations->GetFileInformation && DeleteFileFlag) {
    BY_HANDLE_FILE_INFORMATION byHandleFileInfo;
    ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));
    result = DokanOperations->GetFileInformation(
        EventContext->Operation.SetFile.FileName, &byHandleFileInfo, FileInfo);

    if (result == STATUS_SUCCESS &&
        (byHandleFileInfo.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0)
      return STATUS_CANNOT_DELETE;
  }

  FileInfo->DeleteOnClose = DeleteFileFlag;

  if (FileInfo->IsDirectory) {
    result = DokanOperations->DeleteDirectory(
        EventContext->Operation.SetFile.FileName, FileInfo);
  } else {
    result = DokanOperations->DeleteFile(EventContext->Operation.SetFile.FileName,
                                       FileInfo);
  }
  //Double set for later be sure FS user did not changed it
  FileInfo->DeleteOnClose = DeleteFileFlag;
  return result;
}

NTSTATUS
DokanSetEndOfFileInformation(PEVENT_CONTEXT EventContext,
                             PDOKAN_FILE_INFO FileInfo,
                             PDOKAN_OPERATIONS DokanOperations) {
  PFILE_END_OF_FILE_INFORMATION endInfo = (PFILE_END_OF_FILE_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  if (!DokanOperations->SetEndOfFile)
    return STATUS_NOT_IMPLEMENTED;

  return DokanOperations->SetEndOfFile(EventContext->Operation.SetFile.FileName,
                                       endInfo->EndOfFile.QuadPart, FileInfo);
}

NTSTATUS
DokanSetRenameInformation(PEVENT_CONTEXT EventContext,
                          PDOKAN_FILE_INFO FileInfo,
                          PDOKAN_OPERATIONS DokanOperations) {
  PDOKAN_RENAME_INFORMATION renameInfo = (PDOKAN_RENAME_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  PWCHAR newFileName = NULL;

  if (!DokanOperations->MoveFile)
    return STATUS_NOT_IMPLEMENTED;

  newFileName = (PWCHAR)malloc(renameInfo->FileNameLength + sizeof(WCHAR));
  if (newFileName == NULL)
    return STATUS_INSUFFICIENT_RESOURCES;
  RtlCopyMemory(newFileName, renameInfo->FileName, renameInfo->FileNameLength);
  newFileName[renameInfo->FileNameLength / sizeof(WCHAR)] = L'\0';

  status = DokanOperations->MoveFile(EventContext->Operation.SetFile.FileName,
                                     newFileName, renameInfo->ReplaceIfExists,
                                     FileInfo);
  free(newFileName);
  return status;
}

NTSTATUS
DokanSetValidDataLengthInformation(PEVENT_CONTEXT EventContext,
                                   PDOKAN_FILE_INFO FileInfo,
                                   PDOKAN_OPERATIONS DokanOperations) {
  PFILE_VALID_DATA_LENGTH_INFORMATION validInfo =
      (PFILE_VALID_DATA_LENGTH_INFORMATION)(
          (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  if (!DokanOperations->SetEndOfFile)
    return STATUS_NOT_IMPLEMENTED;

  return DokanOperations->SetEndOfFile(EventContext->Operation.SetFile.FileName,
                                       validInfo->ValidDataLength.QuadPart,
                                       FileInfo);
}

VOID DispatchSetInformation(PDOKAN_IO_EVENT IoEvent) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  ULONG fileInformationClass =
      IoEvent->EventContext->Operation.SetFile.FileInformationClass;

  if (fileInformationClass == FileRenameInformation ||
      fileInformationClass == FileRenameInformationEx) {
    PDOKAN_RENAME_INFORMATION renameInfo =
        (PDOKAN_RENAME_INFORMATION)((PCHAR)IoEvent->EventContext +
                                    IoEvent->EventContext->Operation.SetFile
                                        .BufferOffset);
    CreateDispatchCommon(IoEvent, renameInfo->FileNameLength,
                         /*UseExtraMemoryPool=*/FALSE,
                         /*ClearNonPoolBuffer=*/TRUE);
  } else {
    CreateDispatchCommon(IoEvent, 0, /*UseExtraMemoryPool=*/FALSE,
                         /*ClearNonPoolBuffer=*/TRUE);
  }

  CheckFileName(IoEvent->EventContext->Operation.SetFile.FileName);

  DbgPrint(
      "###SetFileInfo file handle = 0x%p, eventID = %04d, FileInformationClass "
      "= %d, event Info = 0x%p\n",
      IoEvent->DokanOpenInfo,
      IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId : -1,
      fileInformationClass, IoEvent);

  switch (fileInformationClass) {
  case FileAllocationInformation:
    status =
        DokanSetAllocationInformation(IoEvent->EventContext, &IoEvent->DokanFileInfo,
                                      IoEvent->DokanInstance->DokanOperations);
    break;

  case FileBasicInformation:
    status =
        DokanSetBasicInformation(IoEvent->EventContext, &IoEvent->DokanFileInfo,
                                      IoEvent->DokanInstance->DokanOperations);
    break;

  case FileDispositionInformation:
  case FileDispositionInformationEx:
    status = DokanSetDispositionInformation(
        IoEvent->EventContext, &IoEvent->DokanFileInfo,
                                       IoEvent->DokanInstance->DokanOperations);
    break;

  case FileEndOfFileInformation:
    status = DokanSetEndOfFileInformation(
        IoEvent->EventContext, &IoEvent->DokanFileInfo,
                                     IoEvent->DokanInstance->DokanOperations);
    break;

  case FilePositionInformation:
    // this case is dealt with by the driver
    status = STATUS_NOT_IMPLEMENTED;
    break;

  case FileRenameInformation:
  case FileRenameInformationEx:
    status = DokanSetRenameInformation(IoEvent->EventContext,
                                       &IoEvent->DokanFileInfo,
                                       IoEvent->DokanInstance->DokanOperations);
    break;

  case FileValidDataLengthInformation:
    status = DokanSetValidDataLengthInformation(
        IoEvent->EventContext, &IoEvent->DokanFileInfo,
        IoEvent->DokanInstance->DokanOperations);
    break;
  default:
    DbgPrint("  unknown FileInformationClass %d\n", fileInformationClass);
    break;
  }

  IoEvent->EventResult->BufferLength = 0;
  IoEvent->EventResult->Status = status;

  if (status == STATUS_SUCCESS) {
    if (fileInformationClass == FileDispositionInformation ||
        fileInformationClass == FileDispositionInformationEx) {
      IoEvent->EventResult->Operation.Delete.DeleteOnClose =
          IoEvent->DokanFileInfo.DeleteOnClose;
      DbgPrint("  dispositionInfo->DeleteFile = %d\n", IoEvent->DokanFileInfo.DeleteOnClose);
    } else if (fileInformationClass == FileRenameInformation ||
               fileInformationClass == FileRenameInformationEx) {
      PDOKAN_RENAME_INFORMATION renameInfo =
          (PDOKAN_RENAME_INFORMATION)((PCHAR)IoEvent->EventContext +
                                      IoEvent->EventContext->Operation.SetFile
                                          .BufferOffset);
      IoEvent->EventResult->BufferLength = renameInfo->FileNameLength;
      CopyMemory(IoEvent->EventResult->Buffer, renameInfo->FileName,
                 renameInfo->FileNameLength);
    }
  }

  DbgPrint("\tDispatchSetInformation result =  %lx\n", status);

  EventCompletion(IoEvent);
}
