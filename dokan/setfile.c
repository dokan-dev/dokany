/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2018 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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
  NTSTATUS status;

  if (DokanOperations->SetAllocationSize) {
    status = DokanOperations->SetAllocationSize(
        EventContext->Operation.SetFile.FileName,
        allocInfo->AllocationSize.QuadPart, FileInfo);
  } else {
    status = STATUS_NOT_IMPLEMENTED;
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
  PFILE_DISPOSITION_INFORMATION dispositionInfo =
      (PFILE_DISPOSITION_INFORMATION)(
          (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);

  if (!DokanOperations->DeleteFile || !DokanOperations->DeleteDirectory)
    return STATUS_NOT_IMPLEMENTED;

  if (dispositionInfo->DeleteFile == FileInfo->DeleteOnClose) {
    return STATUS_SUCCESS;
  }

  if (DokanOperations->GetFileInformation) {
    BY_HANDLE_FILE_INFORMATION byHandleFileInfo;
    ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));
    NTSTATUS result = DokanOperations->GetFileInformation(
        EventContext->Operation.SetFile.FileName, &byHandleFileInfo, FileInfo);

    if (result == STATUS_SUCCESS &&
        (byHandleFileInfo.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0)
      return STATUS_CANNOT_DELETE;
  }

  FileInfo->DeleteOnClose = (dispositionInfo->DeleteFile) ? TRUE : FALSE;

  if (FileInfo->IsDirectory) {
    return DokanOperations->DeleteDirectory(
        EventContext->Operation.SetFile.FileName, FileInfo);
  } else {
    return DokanOperations->DeleteFile(EventContext->Operation.SetFile.FileName,
                                       FileInfo);
  }
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
DokanSetLinkInformation(PEVENT_CONTEXT EventContext, PDOKAN_FILE_INFO FileInfo,
                        PDOKAN_OPERATIONS DokanOperations) {
  UNREFERENCED_PARAMETER(EventContext);
  UNREFERENCED_PARAMETER(FileInfo);
  UNREFERENCED_PARAMETER(DokanOperations);
  // PDOKAN_LINK_INFORMATION linkInfo =
  // (PDOKAN_LINK_INFORMATION)((PCHAR)EventContext +
  // EventContext->Operation.SetFile.BufferOffset);

  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
DokanSetRenameInformation(PEVENT_CONTEXT EventContext,
                          PDOKAN_FILE_INFO FileInfo,
                          PDOKAN_OPERATIONS DokanOperations) {
  PDOKAN_RENAME_INFORMATION renameInfo = (PDOKAN_RENAME_INFORMATION)(
      (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  WCHAR *newName = NULL;

  if (!DokanOperations->MoveFile)
    return STATUS_NOT_IMPLEMENTED;

  if (renameInfo->FileName[0] != L'\\') {
    ULONG pos;
    for (pos = EventContext->Operation.SetFile.FileNameLength / sizeof(WCHAR);
         pos != 0; --pos) {
      if (EventContext->Operation.SetFile.FileName[pos] == '\\')
        break;
    }
    newName = (WCHAR *)malloc((pos + 1) * sizeof(WCHAR) +
                              renameInfo->FileNameLength + sizeof(WCHAR));
    if (newName == NULL)
      return STATUS_INSUFFICIENT_RESOURCES;
    ZeroMemory(newName, (pos + 1) * sizeof(WCHAR) + renameInfo->FileNameLength +
                            sizeof(WCHAR));
    RtlCopyMemory(newName, EventContext->Operation.SetFile.FileName,
                  (pos + 1) * sizeof(WCHAR));
    RtlCopyMemory((PCHAR)newName + (pos + 1) * sizeof(WCHAR),
                  renameInfo->FileName, renameInfo->FileNameLength);
  } else {
    newName = (WCHAR *)malloc(renameInfo->FileNameLength + sizeof(WCHAR));
    if (newName == NULL)
      return STATUS_INSUFFICIENT_RESOURCES;
    ZeroMemory(newName, renameInfo->FileNameLength + sizeof(WCHAR));
    RtlCopyMemory(newName, renameInfo->FileName, renameInfo->FileNameLength);
  }

  status =
      DokanOperations->MoveFile(EventContext->Operation.SetFile.FileName,
                                newName, renameInfo->ReplaceIfExists, FileInfo);
  free(newName);
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

VOID DispatchSetInformation(HANDLE Handle, PEVENT_CONTEXT EventContext,
                            PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  PDOKAN_OPEN_INFO openInfo;
  DOKAN_FILE_INFO fileInfo;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  ULONG sizeOfEventInfo = sizeof(EVENT_INFORMATION);

  if (EventContext->Operation.SetFile.FileInformationClass == FileRenameInformation
	  || EventContext->Operation.SetFile.FileInformationClass == FileRenameInformationEx) {
    PDOKAN_RENAME_INFORMATION renameInfo = (PDOKAN_RENAME_INFORMATION)(
        (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
    sizeOfEventInfo += renameInfo->FileNameLength;
  }

  CheckFileName(EventContext->Operation.SetFile.FileName);

  eventInfo = DispatchCommon(EventContext, sizeOfEventInfo, DokanInstance,
                             &fileInfo, &openInfo);

  DbgPrint("###SetFileInfo %04d  %d\n",
           openInfo != NULL ? openInfo->EventId : -1,
           EventContext->Operation.SetFile.FileInformationClass);

  switch (EventContext->Operation.SetFile.FileInformationClass) {
  case FileAllocationInformation:
    status = DokanSetAllocationInformation(EventContext, &fileInfo,
                                           DokanInstance->DokanOperations);
    break;

  case FileBasicInformation:
    status = DokanSetBasicInformation(EventContext, &fileInfo,
                                      DokanInstance->DokanOperations);
    break;

  case FileDispositionInformation:
    status = DokanSetDispositionInformation(EventContext, &fileInfo,
                                            DokanInstance->DokanOperations);
    break;

  case FileEndOfFileInformation:
    status = DokanSetEndOfFileInformation(EventContext, &fileInfo,
                                          DokanInstance->DokanOperations);
    break;

  case FileLinkInformation:
    status = DokanSetLinkInformation(EventContext, &fileInfo,
                                     DokanInstance->DokanOperations);
    break;

  case FilePositionInformation:
    // this case is dealed with by driver
    status = STATUS_NOT_IMPLEMENTED;
    break;

  case FileRenameInformation:
  case FileRenameInformationEx:
    status = DokanSetRenameInformation(EventContext, &fileInfo,
                                       DokanInstance->DokanOperations);
    break;

  case FileValidDataLengthInformation:
    status = DokanSetValidDataLengthInformation(EventContext, &fileInfo,
                                                DokanInstance->DokanOperations);
    break;
  default:
    DbgPrint("  unknown FileInformationClass %d\n",
      EventContext->Operation.SetFile.FileInformationClass);
    break;
  }

  if (openInfo != NULL)
    openInfo->UserContext = fileInfo.Context;
  eventInfo->BufferLength = 0;
  eventInfo->Status = status;

  if (EventContext->Operation.SetFile.FileInformationClass ==
      FileDispositionInformation) {
    if (status == STATUS_SUCCESS) {
      PFILE_DISPOSITION_INFORMATION dispositionInfo =
          (PFILE_DISPOSITION_INFORMATION)(
              (PCHAR)EventContext +
              EventContext->Operation.SetFile.BufferOffset);
      eventInfo->Operation.Delete.DeleteOnClose =
          dispositionInfo->DeleteFile ? TRUE : FALSE;
      DbgPrint("  dispositionInfo->DeleteFile = %d\n",
               dispositionInfo->DeleteFile);
    }

  } else {
    // notice new file name to driver
    if (status == STATUS_SUCCESS &&
        EventContext->Operation.SetFile.FileInformationClass == FileRenameInformation
		|| EventContext->Operation.SetFile.FileInformationClass == FileRenameInformationEx) {
      PDOKAN_RENAME_INFORMATION renameInfo = (PDOKAN_RENAME_INFORMATION)(
          (PCHAR)EventContext + EventContext->Operation.SetFile.BufferOffset);
      eventInfo->BufferLength = renameInfo->FileNameLength;
      CopyMemory(eventInfo->Buffer, renameInfo->FileName,
                 renameInfo->FileNameLength);
    }
  }

  DbgPrint("\tDispatchSetInformation result =  %lx\n", status);

  SendEventInformation(Handle, eventInfo, sizeOfEventInfo, DokanInstance);
  free(eventInfo);
}
