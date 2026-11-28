/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2025 Google, Inc.
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

#define DOKAN_VALID_FILE_DISPOSITION_EX_FLAGS                                  \
  (FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS |                \
   FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK | FILE_DISPOSITION_ON_CLOSE |    \
   FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE)

NTSTATUS
DokanSetAllocationInformation(PEVENT_CONTEXT EventContext,
                              PDOKAN_FILE_INFO FileInfo,
                              PDOKAN_OPERATIONS DokanOperations) {
  PFILE_ALLOCATION_INFORMATION allocInfo = (PFILE_ALLOCATION_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  if (FileInfo->IsDirectory) {
    return STATUS_INVALID_PARAMETER;
  }

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
  PFILE_BASIC_INFORMATION basicInfo = (PFILE_BASIC_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  if (basicInfo->CreationTime.QuadPart < -2 ||
      basicInfo->LastAccessTime.QuadPart < -2 ||
      basicInfo->LastWriteTime.QuadPart < -2 ||
      basicInfo->ChangeTime.QuadPart < -2) {
    return STATUS_INVALID_PARAMETER;
  }

  if (FileInfo->IsDirectory &&
      (basicInfo->FileAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0) {
    return STATUS_INVALID_PARAMETER;
  }

  if (!DokanOperations->SetFileBasicInformation) {
    return STATUS_NOT_IMPLEMENTED;
  }

  return DokanOperations->SetFileBasicInformation(
      EventContext->Operation.SetFile.FileName, basicInfo, FileInfo);
}

NTSTATUS
DokanSetDispositionInformation(PEVENT_CONTEXT EventContext,
                               PDOKAN_FILE_INFO FileInfo,
                               PDOKAN_OPERATIONS DokanOperations) {
  ULONG dispositionFlags = 0;
  BOOLEAN DeleteFileFlag = FALSE;
  NTSTATUS result;

  switch (EventContext->Operation.SetFile.FileInformationClass) {
  case FileDispositionInformation: {
    PFILE_DISPOSITION_INFORMATION dispositionInfo =
        (PFILE_DISPOSITION_INFORMATION)(
            (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
    dispositionFlags = dispositionInfo->DeleteFile
                           ? FILE_DISPOSITION_DELETE
                           : FILE_DISPOSITION_DO_NOT_DELETE;
  } break;
  case FileDispositionInformationEx: {
    PFILE_DISPOSITION_INFORMATION_EX dispositionexInfo =
        (PFILE_DISPOSITION_INFORMATION_EX)(
            (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
    dispositionFlags = dispositionexInfo->Flags;
    if ((dispositionFlags & ~(ULONG)DOKAN_VALID_FILE_DISPOSITION_EX_FLAGS) !=
        0) {
      return STATUS_INVALID_PARAMETER;
    }
  } break;
  default:
    return STATUS_INVALID_PARAMETER;
  }

  DeleteFileFlag = (dispositionFlags & FILE_DISPOSITION_DELETE) ? TRUE : FALSE;

  if (!DokanOperations->SetFileDisposition) {
    return STATUS_NOT_IMPLEMENTED;
  }

  if (DokanOperations->GetFileInformation && DeleteFileFlag &&
      (dispositionFlags & FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE) == 0) {
    DOKAN_FILE_INFORMATION dokanFileInfo;
    ZeroMemory(&dokanFileInfo, sizeof(DOKAN_FILE_INFORMATION));
    result = DokanOperations->GetFileInformation(
        EventContext->Operation.SetFile.FileName, &dokanFileInfo, FileInfo);

    if (result == STATUS_SUCCESS &&
        (dokanFileInfo.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
      return STATUS_CANNOT_DELETE;
    }
  }

  FileInfo->DeletePending = DeleteFileFlag;
  result = DokanOperations->SetFileDisposition(
      EventContext->Operation.SetFile.FileName, dispositionFlags, FileInfo);
  // Double set to ensure FS callback did not mutate DeletePending.
  FileInfo->DeletePending = DeleteFileFlag;
  return result;
}

NTSTATUS
DokanSetEndOfFileInformation(PEVENT_CONTEXT EventContext,
                             PDOKAN_FILE_INFO FileInfo,
                             PDOKAN_OPERATIONS DokanOperations) {
  PFILE_END_OF_FILE_INFORMATION endInfo = (PFILE_END_OF_FILE_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  if (FileInfo->IsDirectory) {
    return STATUS_INVALID_PARAMETER;
  }

  if (!DokanOperations->SetEndOfFile) {
    return STATUS_NOT_IMPLEMENTED;
  }

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

  if (wcscmp(EventContext->Operation.SetFile.FileName, L"\\") == 0) {
    return STATUS_ACCESS_DENIED;
  }

  if (!DokanOperations->MoveFile) {
    return STATUS_NOT_IMPLEMENTED;
  }

  newFileName = (PWCHAR)malloc(renameInfo->FileNameLength + sizeof(WCHAR));
  if (newFileName == NULL)
    return STATUS_INSUFFICIENT_RESOURCES;
  RtlCopyMemory(newFileName, renameInfo->FileName, renameInfo->FileNameLength);
  newFileName[renameInfo->FileNameLength / sizeof(WCHAR)] = L'\0';

  ULONG renameFlags = 0;
  if (EventContext->Operation.SetFile.FileInformationClass ==
      FileRenameInformationEx) {
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN10_RS1)
    renameFlags = renameInfo->Flags;
#else
    renameFlags = (ULONG)renameInfo->ReplaceIfExists;
#endif
  } else {
    renameFlags =
        renameInfo->ReplaceIfExists ? FILE_RENAME_REPLACE_IF_EXISTS : 0;
  }

  status = DokanOperations->MoveFile(EventContext->Operation.SetFile.FileName,
                                     newFileName, renameFlags, FileInfo);
  free(newFileName);
  return status;
}

NTSTATUS
DokanSetValidDataLengthInformation(PDOKAN_IO_EVENT IoEvent) {
  PEVENT_CONTEXT eventContext = IoEvent->EventContext;
  PDOKAN_FILE_INFO fileInfo = &IoEvent->DokanFileInfo;
  PDOKAN_OPERATIONS dokanOperations = IoEvent->DokanInstance->DokanOperations;
  PFILE_VALID_DATA_LENGTH_INFORMATION validInfo =
      (PFILE_VALID_DATA_LENGTH_INFORMATION)((PCHAR)eventContext +
                                            eventContext->Operation.SetFile
                                                .BufferOffset);

  if (fileInfo->IsDirectory || validInfo->ValidDataLength.QuadPart < 0) {
    return STATUS_INVALID_PARAMETER;
  }

  if (!dokanOperations->SetValidDataLength) {
    return STATUS_NOT_IMPLEMENTED;
  }

  if (!IoEvent->DokanOpenInfo ||
      !IoEvent->DokanOpenInfo->HasManageVolumePrivilege) {
    return STATUS_PRIVILEGE_NOT_HELD;
  }

  return dokanOperations->SetValidDataLength(
      eventContext->Operation.SetFile.FileName,
      validInfo->ValidDataLength.QuadPart, fileInfo);
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
    status = DokanSetValidDataLengthInformation(IoEvent);
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
      IoEvent->EventResult->Operation.Delete.DeletePending =
          IoEvent->DokanFileInfo.DeletePending;
      DbgPrint("  dispositionInfo->DeletePending = %d\n",
               IoEvent->DokanFileInfo.DeletePending);
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
