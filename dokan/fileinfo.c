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

#include "dokani.h"
#include "fileinfo.h"

#include <ntstatus.h>
#include <stdio.h>
#include <assert.h>

#define DOKAN_STREAM_ENTRY_ALIGNMENT 8

NTSTATUS
DokanFillFileBasicInfo(PFILE_BASIC_INFORMATION BasicInfo,
                       PBY_HANDLE_FILE_INFORMATION FileInfo,
                       PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_BASIC_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  BasicInfo->CreationTime.LowPart = FileInfo->ftCreationTime.dwLowDateTime;
  BasicInfo->CreationTime.HighPart = FileInfo->ftCreationTime.dwHighDateTime;
  BasicInfo->LastAccessTime.LowPart = FileInfo->ftLastAccessTime.dwLowDateTime;
  BasicInfo->LastAccessTime.HighPart =
      FileInfo->ftLastAccessTime.dwHighDateTime;
  BasicInfo->LastWriteTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  BasicInfo->LastWriteTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  BasicInfo->ChangeTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  BasicInfo->ChangeTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  BasicInfo->FileAttributes = FileInfo->dwFileAttributes;

  *RemainingLength -= sizeof(FILE_BASIC_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileStandardInfo(PFILE_STANDARD_INFORMATION StandardInfo,
                          PBY_HANDLE_FILE_INFORMATION FileInfo,
                          PULONG RemainingLength,
                          PDOKAN_FILE_INFO DokanFileInfo,
                          PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_STANDARD_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  StandardInfo->AllocationSize.HighPart = FileInfo->nFileSizeHigh;
  StandardInfo->AllocationSize.LowPart = FileInfo->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&StandardInfo->AllocationSize,
                        DokanInstance->DokanOptions);
  StandardInfo->EndOfFile.HighPart = FileInfo->nFileSizeHigh;
  StandardInfo->EndOfFile.LowPart = FileInfo->nFileSizeLow;
  StandardInfo->NumberOfLinks = FileInfo->nNumberOfLinks;
  StandardInfo->DeletePending = DokanFileInfo->DeleteOnClose;
  StandardInfo->Directory = FALSE;

  if (FileInfo->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    StandardInfo->Directory = TRUE;
  }

  *RemainingLength -= sizeof(FILE_STANDARD_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFilePositionInfo(PFILE_POSITION_INFORMATION PosInfo,
                          PBY_HANDLE_FILE_INFORMATION FileInfo,
                          PULONG RemainingLength) {

  UNREFERENCED_PARAMETER(FileInfo);

  if (*RemainingLength < sizeof(FILE_POSITION_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  // this field is filled by driver
  PosInfo->CurrentByteOffset.QuadPart = 0; // fileObject->CurrentByteOffset;

  *RemainingLength -= sizeof(FILE_POSITION_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillInternalInfo(PFILE_INTERNAL_INFORMATION InternalInfo,
                      PBY_HANDLE_FILE_INFORMATION FileInfo,
                      PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_INTERNAL_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  InternalInfo->IndexNumber.HighPart = FileInfo->nFileIndexHigh;
  InternalInfo->IndexNumber.LowPart = FileInfo->nFileIndexLow;

  *RemainingLength -= sizeof(FILE_INTERNAL_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileAllInfo(PFILE_ALL_INFORMATION AllInfo,
                     PBY_HANDLE_FILE_INFORMATION FileInfo,
                     PULONG RemainingLength,
                     PDOKAN_FILE_INFO DokanFileInfo,
                     PDOKAN_INSTANCE DokanInstance) {
  ULONG allRemainingLength = *RemainingLength;

  if (*RemainingLength < sizeof(FILE_ALL_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  // FileBasicInformation
  DokanFillFileBasicInfo(&AllInfo->BasicInformation, FileInfo, RemainingLength);

  // FileStandardInformation
  DokanFillFileStandardInfo(&AllInfo->StandardInformation, FileInfo,
                            RemainingLength, DokanFileInfo, DokanInstance);

  // FileInternalInformation
  DokanFillInternalInfo(&AllInfo->InternalInformation, FileInfo,
                        RemainingLength);

  AllInfo->EaInformation.EaSize = 0;

  // FilePositionInformation
  DokanFillFilePositionInfo(&AllInfo->PositionInformation, FileInfo,
                            RemainingLength);

  // AllInfo->NameInformation.FileName is populated by the Kernel

  // the size except of FILE_NAME_INFORMATION
  allRemainingLength -=
      FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName);

  *RemainingLength = allRemainingLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileNameInfo(PFILE_NAME_INFORMATION NameInfo,
                      PBY_HANDLE_FILE_INFORMATION FileInfo,
                      PULONG RemainingLength, PEVENT_CONTEXT EventContext) {

  UNREFERENCED_PARAMETER(FileInfo);

  if (*RemainingLength < sizeof(FILE_NAME_INFORMATION) +
                             EventContext->Operation.File.FileNameLength) {
    return STATUS_BUFFER_OVERFLOW;
  }

  NameInfo->FileNameLength = EventContext->Operation.File.FileNameLength;
  RtlCopyMemory(&(NameInfo->FileName[0]), EventContext->Operation.File.FileName,
                EventContext->Operation.File.FileNameLength);

  *RemainingLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]);
  *RemainingLength -= NameInfo->FileNameLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileAttributeTagInfo(PFILE_ATTRIBUTE_TAG_INFORMATION AttrTagInfo,
                              PBY_HANDLE_FILE_INFORMATION FileInfo,
                              PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  AttrTagInfo->FileAttributes = FileInfo->dwFileAttributes;
  AttrTagInfo->ReparseTag = 0;

  *RemainingLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillNetworkOpenInfo(PFILE_NETWORK_OPEN_INFORMATION NetInfo,
                         PBY_HANDLE_FILE_INFORMATION FileInfo,
                         PULONG RemainingLength,
                         PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  NetInfo->CreationTime.LowPart = FileInfo->ftCreationTime.dwLowDateTime;
  NetInfo->CreationTime.HighPart = FileInfo->ftCreationTime.dwHighDateTime;
  NetInfo->LastAccessTime.LowPart = FileInfo->ftLastAccessTime.dwLowDateTime;
  NetInfo->LastAccessTime.HighPart = FileInfo->ftLastAccessTime.dwHighDateTime;
  NetInfo->LastWriteTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  NetInfo->LastWriteTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  NetInfo->ChangeTime.LowPart = FileInfo->ftLastWriteTime.dwLowDateTime;
  NetInfo->ChangeTime.HighPart = FileInfo->ftLastWriteTime.dwHighDateTime;
  NetInfo->AllocationSize.HighPart = FileInfo->nFileSizeHigh;
  NetInfo->AllocationSize.LowPart = FileInfo->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&NetInfo->AllocationSize, DokanInstance->DokanOptions);
  NetInfo->EndOfFile.HighPart = FileInfo->nFileSizeHigh;
  NetInfo->EndOfFile.LowPart = FileInfo->nFileSizeLow;
  NetInfo->FileAttributes = FileInfo->dwFileAttributes;

  *RemainingLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillIdInfo(PFILE_ID_INFORMATION IdInfo,
                PBY_HANDLE_FILE_INFORMATION FileInfo, PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_ID_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  IdInfo->VolumeSerialNumber = FileInfo->dwVolumeSerialNumber;

  ZeroMemory(IdInfo->FileId.Identifier, sizeof(IdInfo->FileId.Identifier));

  ((DWORD *)(IdInfo->FileId.Identifier))[0] = FileInfo->nFileIndexLow;
  ((DWORD *)(IdInfo->FileId.Identifier))[1] = FileInfo->nFileIndexHigh;

  *RemainingLength -= sizeof(FILE_ID_INFORMATION);

  return STATUS_SUCCESS;
}

/**
 * \struct DOKAN_FIND_STREAM_DATA
 * \brief Dokan find stream list
 *
 * Used by FindStreams
 */
typedef struct _DOKAN_FIND_STREAM_DATA {
  /**
  * Stream data information link
  */
  WIN32_FIND_STREAM_DATA FindStreamData;
  /**
  * Current list entry informations
  */
  LIST_ENTRY ListEntry;
} DOKAN_FIND_STREAM_DATA, *PDOKAN_FIND_STREAM_DATA;

BOOL WINAPI DokanFillFindStreamData(PWIN32_FIND_STREAM_DATA FindStreamData,
                                    PVOID FindStreamContext) {

  PDOKAN_IO_EVENT ioEvent = (PDOKAN_IO_EVENT)FindStreamContext;
  ULONG offset = (ULONG)ioEvent->EventResult->BufferLength;
  ULONG resultBufferSize =
      ioEvent->EventContext->Operation.File.BufferLength;

  ULONG streamNameLength =
      (ULONG)wcslen(FindStreamData->cStreamName) * sizeof(WCHAR);

  // Must be aligned on a 8-byte boundary.
  ULONG entrySize =
      QuadAlign(sizeof(FILE_STREAM_INFORMATION) + streamNameLength);
  assert(entrySize % DOKAN_STREAM_ENTRY_ALIGNMENT == 0);

  PFILE_STREAM_INFORMATION streamInfo =
      (PFILE_STREAM_INFORMATION)&ioEvent->EventResult->Buffer[offset];
  if (offset + entrySize + streamInfo->NextEntryOffset > resultBufferSize) {
    return FALSE;
  }

  // If this isn't the first entry move to the next
  // memory location
  if (streamInfo->NextEntryOffset != 0) {
    offset += streamInfo->NextEntryOffset;
    streamInfo =
        (PFILE_STREAM_INFORMATION)&ioEvent->EventResult->Buffer[offset];
  }

  assert(streamInfo->NextEntryOffset == 0);

  // Fill the new entry
  streamInfo->StreamNameLength = streamNameLength;
  memcpy_s(streamInfo->StreamName, streamNameLength,
           FindStreamData->cStreamName, streamNameLength);
  streamInfo->StreamSize = FindStreamData->StreamSize;
  streamInfo->StreamAllocationSize = FindStreamData->StreamSize;
  streamInfo->NextEntryOffset = entrySize;
  ALIGN_ALLOCATION_SIZE(&streamInfo->StreamAllocationSize,
                        ioEvent->DokanInstance->DokanOptions);
  ioEvent->EventResult->BufferLength = offset;
  return TRUE;
}

VOID DOKANAPI DokanEndDispatchGetFileInformation(
    PDOKAN_IO_EVENT IoEvent, PBY_HANDLE_FILE_INFORMATION ByHandleFileInfo,
    NTSTATUS Status) {
  ULONG remainingLength = IoEvent->EventContext->Operation.File.BufferLength;

  DbgPrint("\tresult =  %lx\n", Status);

  if (Status != STATUS_SUCCESS) {
    IoEvent->EventResult->Status = STATUS_INVALID_PARAMETER;
    IoEvent->EventResult->BufferLength = 0;
  } else {

    switch (IoEvent->EventContext->Operation.File.FileInformationClass) {
    case FileBasicInformation:
      DbgPrint("\tFileBasicInformation\n");
      Status = DokanFillFileBasicInfo(
          (PFILE_BASIC_INFORMATION)IoEvent->EventResult->Buffer,
          ByHandleFileInfo, &remainingLength);
      break;

    case FileIdInformation:
      DbgPrint("\tFileIdInformation\n");
      Status =
          DokanFillIdInfo((PFILE_ID_INFORMATION)IoEvent->EventResult->Buffer,
                          ByHandleFileInfo, &remainingLength);
      break;

    case FileInternalInformation:
      DbgPrint("\tFileInternalInformation\n");
      Status = DokanFillInternalInfo(
          (PFILE_INTERNAL_INFORMATION)IoEvent->EventResult->Buffer,
          ByHandleFileInfo, &remainingLength);
      break;

    case FileEaInformation:
      DbgPrint("\tFileEaInformation\n");
      // status = STATUS_NOT_IMPLEMENTED;
      Status = STATUS_SUCCESS;
      remainingLength -= sizeof(FILE_EA_INFORMATION);
      break;

    case FileStandardInformation:
      DbgPrint("\tFileStandardInformation\n");
      Status = DokanFillFileStandardInfo(
          (PFILE_STANDARD_INFORMATION)IoEvent->EventResult->Buffer,
          ByHandleFileInfo, &remainingLength, &IoEvent->DokanFileInfo,
          IoEvent->DokanInstance);
      break;

    case FileAllInformation:
      DbgPrint("\tFileAllInformation\n");
      Status = DokanFillFileAllInfo(
          (PFILE_ALL_INFORMATION)IoEvent->EventResult->Buffer, ByHandleFileInfo,
          &remainingLength, &IoEvent->DokanFileInfo, IoEvent->DokanInstance);
      break;

    case FileAlternateNameInformation:
      DbgPrint("\tFileAlternateNameInformation\n");
      Status = STATUS_NOT_IMPLEMENTED;
      break;

    case FileAttributeTagInformation:
      DbgPrint("\tFileAttributeTagInformation\n");
      Status = DokanFillFileAttributeTagInfo(
          (PFILE_ATTRIBUTE_TAG_INFORMATION)IoEvent->EventResult->Buffer,
          ByHandleFileInfo, &remainingLength);
      break;

    case FileCompressionInformation:
      DbgPrint("\tFileCompressionInformation\n");
      Status = STATUS_NOT_IMPLEMENTED;
      break;

    case FileNormalizedNameInformation:
      DbgPrint("\tFileNormalizedNameInformation\n");
    case FileNameInformation:
      // this case is not used because driver deal with
      DbgPrint("\tFileNameInformation\n");
      Status = DokanFillFileNameInfo(
          (PFILE_NAME_INFORMATION)IoEvent->EventResult->Buffer,
          ByHandleFileInfo, &remainingLength, IoEvent->EventContext);
      break;

    case FileNetworkOpenInformation:
      DbgPrint("\tFileNetworkOpenInformation\n");
      Status = DokanFillNetworkOpenInfo(
          (PFILE_NETWORK_OPEN_INFORMATION)IoEvent->EventResult->Buffer,
          ByHandleFileInfo, &remainingLength, IoEvent->DokanInstance);
      break;

    case FilePositionInformation:
      // this case is not used because driver deal with
      DbgPrint("\tFilePositionInformation\n");
      Status = DokanFillFilePositionInfo(
          (PFILE_POSITION_INFORMATION)IoEvent->EventResult->Buffer,
          ByHandleFileInfo, &remainingLength);
      break;
    case FileStreamInformation:
      DbgPrint("FileStreamInformation (internal error)\n");
      // shouldn't get here
      Status = STATUS_INTERNAL_ERROR;
      break;
    default: {
      Status = STATUS_INVALID_PARAMETER;
      DbgPrint("  unknown type:%d\n",
               IoEvent->EventContext->Operation.File.FileInformationClass);
    } break;
    }

    IoEvent->EventResult->Status = Status;
    IoEvent->EventResult->BufferLength =
        IoEvent->EventContext->Operation.File.BufferLength - remainingLength;
  }

  DbgPrint("\tDispatchQueryInformation result =  %lx\n", Status);
  EventCompletion(IoEvent);
}

VOID DOKANAPI DokanEndDispatchFindStreams(PDOKAN_IO_EVENT IoEvent,
                                          NTSTATUS Status) {
  ULONG resultBufferSize = IOEVENT_RESULT_BUFFER_SIZE(IoEvent);
  PFILE_STREAM_INFORMATION streamInfo =
      (PFILE_STREAM_INFORMATION)&IoEvent->EventResult
          ->Buffer[IoEvent->EventResult->BufferLength];

  DbgPrint("\tresult =  %lx\n", Status);

  // Entries must be 8 byte aligned
  assert(streamInfo->NextEntryOffset % DOKAN_STREAM_ENTRY_ALIGNMENT == 0);

  // Ensure that the last entry doesn't point to another entry.
  IoEvent->EventResult->BufferLength += streamInfo->NextEntryOffset;
  streamInfo->NextEntryOffset = 0;

  assert(IoEvent->EventResult->BufferLength <= resultBufferSize);

  if (IoEvent->EventResult->BufferLength > resultBufferSize) {
    IoEvent->EventResult->BufferLength = 0;
    Status = STATUS_BUFFER_OVERFLOW;
  }

  // STATUS_PENDING should not be passed to this function
  if (Status == STATUS_PENDING) {
    DbgPrint("Dokan Error: DokanEndDispatchFindStreams() failed because "
             "STATUS_PENDING was supplied for ResultStatus.\n");
    Status = STATUS_INTERNAL_ERROR;
  }

  IoEvent->EventResult->Status = Status;
  DbgPrint("\tDokanEndDispatchFindStreams result =  0x%x\n", Status);
  EventCompletion(IoEvent);
}

VOID DispatchQueryInformation(PDOKAN_IO_EVENT IoEvent) {
  BY_HANDLE_FILE_INFORMATION byHandleFileInfo;
  NTSTATUS status = STATUS_INVALID_PARAMETER;

  DbgPrint(
      "###GetFileInfo file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
      IoEvent->DokanOpenInfo,
      IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId : -1,
      IoEvent);

  CheckFileName(IoEvent->EventContext->Operation.File.FileName);

  CreateDispatchCommon(IoEvent,
                       IoEvent->EventContext->Operation.File.BufferLength,
                       /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  if (IoEvent->EventContext->Operation.File.FileInformationClass ==
      FileStreamInformation) {
    DbgPrint("FileStreamInformation\n");
    // https://msdn.microsoft.com/en-us/library/windows/hardware/ff540364(v=vs.85).aspx
    if (IoEvent->EventContext->Operation.File.BufferLength <
        sizeof(FILE_STREAM_INFORMATION)) {

      status = STATUS_BUFFER_TOO_SMALL;
    } else if (IoEvent->DokanInstance->DokanOperations->FindStreams) {

      status = IoEvent->DokanInstance->DokanOperations->FindStreams(
          IoEvent->EventContext->Operation.File.FileName,
          DokanFillFindStreamData, IoEvent, &IoEvent->DokanFileInfo);
      DokanEndDispatchFindStreams(IoEvent, status);
    } else {
      status = STATUS_NOT_IMPLEMENTED;
    }
  } else if (IoEvent->DokanInstance->DokanOperations->GetFileInformation) {

    ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));
    status = IoEvent->DokanInstance->DokanOperations->GetFileInformation(
        IoEvent->EventContext->Operation.File.FileName, &byHandleFileInfo,
        &IoEvent->DokanFileInfo);
    DokanEndDispatchGetFileInformation(IoEvent, &byHandleFileInfo, status);
  } else {

    status = STATUS_NOT_IMPLEMENTED;
  }
}