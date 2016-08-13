/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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
  StandardInfo->DeletePending = FALSE;
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
                     PULONG RemainingLength, PEVENT_CONTEXT EventContext,
                     PDOKAN_INSTANCE DokanInstance) {
  ULONG allRemainingLength = *RemainingLength;

  if (*RemainingLength < sizeof(FILE_ALL_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  // FileBasicInformation
  DokanFillFileBasicInfo(&AllInfo->BasicInformation, FileInfo, RemainingLength);

  // FileStandardInformation
  DokanFillFileStandardInfo(&AllInfo->StandardInformation, FileInfo,
                            RemainingLength, DokanInstance);

  // FileInternalInformation
  DokanFillInternalInfo(&AllInfo->InternalInformation, FileInfo,
                        RemainingLength);

  AllInfo->EaInformation.EaSize = 0;

  // FilePositionInformation
  DokanFillFilePositionInfo(&AllInfo->PositionInformation, FileInfo,
                            RemainingLength);

  // there is not enough space to fill FileNameInformation
  if (allRemainingLength < sizeof(FILE_ALL_INFORMATION) +
                               EventContext->Operation.File.FileNameLength) {
    // fill out to the limit
    // FileNameInformation
    AllInfo->NameInformation.FileNameLength =
        EventContext->Operation.File.FileNameLength;
    AllInfo->NameInformation.FileName[0] =
        EventContext->Operation.File.FileName[0];

    allRemainingLength -= sizeof(FILE_ALL_INFORMATION);
    *RemainingLength = allRemainingLength;
    return STATUS_BUFFER_OVERFLOW;
  }

  // FileNameInformation
  AllInfo->NameInformation.FileNameLength =
      EventContext->Operation.File.FileNameLength;
  RtlCopyMemory(&(AllInfo->NameInformation.FileName[0]),
                EventContext->Operation.File.FileName,
                EventContext->Operation.File.FileNameLength);

  // the size except of FILE_NAME_INFORMATION
  allRemainingLength -=
      (sizeof(FILE_ALL_INFORMATION) - sizeof(FILE_NAME_INFORMATION));

  // the size of FILE_NAME_INFORMATION
  allRemainingLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]);
  allRemainingLength -= AllInfo->NameInformation.FileNameLength;

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
DokanFillNetworkPhysicalNameInfo(
    PFILE_NETWORK_PHYSICAL_NAME_INFORMATION NetInfo,
    PBY_HANDLE_FILE_INFORMATION FileInfo, PULONG RemainingLength,
    PEVENT_CONTEXT EventContext) {
  if (*RemainingLength < sizeof(FILE_NETWORK_PHYSICAL_NAME_INFORMATION) +
                             EventContext->Operation.File.FileNameLength) {
    return STATUS_BUFFER_OVERFLOW;
  }

  UNREFERENCED_PARAMETER(FileInfo);

  NetInfo->FileNameLength = EventContext->Operation.File.FileNameLength;
  CopyMemory(NetInfo->FileName, EventContext->Operation.File.FileName,
             EventContext->Operation.File.FileNameLength);

  *RemainingLength -=
      FIELD_OFFSET(FILE_NETWORK_PHYSICAL_NAME_INFORMATION, FileName[0]);
  *RemainingLength -= NetInfo->FileNameLength;

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

int WINAPI DokanFillFindStreamData(PWIN32_FIND_STREAM_DATA FindStreamData,
                                   PDOKAN_FILE_INFO FileInfo) {
  PLIST_ENTRY listHead =
      ((PDOKAN_OPEN_INFO)(UINT_PTR)FileInfo->DokanContext)->StreamListHead;
  PDOKAN_FIND_STREAM_DATA findStreamData;

  findStreamData =
      (PDOKAN_FIND_STREAM_DATA)malloc(sizeof(DOKAN_FIND_STREAM_DATA));
  if (findStreamData == NULL) {
    return 0;
  }
  ZeroMemory(findStreamData, sizeof(DOKAN_FIND_STREAM_DATA));
  InitializeListHead(&findStreamData->ListEntry);

  findStreamData->FindStreamData = *FindStreamData;

  InsertTailList(listHead, &findStreamData->ListEntry);
  return 0;
}

VOID ClearFindStreamData(PLIST_ENTRY ListHead) {
  // free all list entries
  while (!IsListEmpty(ListHead)) {
    PLIST_ENTRY entry = RemoveHeadList(ListHead);
    PDOKAN_FIND_STREAM_DATA find =
        CONTAINING_RECORD(entry, DOKAN_FIND_STREAM_DATA, ListEntry);
    free(find);
  }
}

NTSTATUS
DokanFindStreams(PFILE_STREAM_INFORMATION StreamInfo, PDOKAN_FILE_INFO FileInfo,
                 PEVENT_CONTEXT EventContext, PDOKAN_INSTANCE DokanInstance,
                 PULONG RemainingLength) {
  PDOKAN_OPEN_INFO openInfo =
      (PDOKAN_OPEN_INFO)(UINT_PTR)FileInfo->DokanContext;
  NTSTATUS status = STATUS_SUCCESS;

  if (!DokanInstance->DokanOperations->FindStreams) {
    return STATUS_NOT_IMPLEMENTED;
  }

  if (openInfo->StreamListHead == NULL) {
    openInfo->StreamListHead = malloc(sizeof(LIST_ENTRY));
    if (openInfo->StreamListHead != NULL) {
      InitializeListHead(openInfo->StreamListHead);
    } else {
      status = STATUS_NO_MEMORY;
    }
  }

  if (status == STATUS_SUCCESS && IsListEmpty(openInfo->StreamListHead)) {
    status = DokanInstance->DokanOperations->FindStreams(
        EventContext->Operation.File.FileName, DokanFillFindStreamData,
        FileInfo);
  }

  if (status == STATUS_SUCCESS) {
    PLIST_ENTRY listHead, entry;
    ULONG entrySize;

    listHead = openInfo->StreamListHead;
    entrySize = 0;

    for (entry = listHead->Flink; entry != listHead; entry = entry->Flink) {
      PDOKAN_FIND_STREAM_DATA find =
          CONTAINING_RECORD(entry, DOKAN_FIND_STREAM_DATA, ListEntry);

      ULONG nextEntryOffset = entrySize;

      ULONG streamNameLength =
          (ULONG)wcslen(find->FindStreamData.cStreamName) * sizeof(WCHAR);
      entrySize = sizeof(FILE_STREAM_INFORMATION) + streamNameLength;
      // Must be align on a 8-byte boundary.
      entrySize = QuadAlign(entrySize);
      if (*RemainingLength < entrySize) {
        status = STATUS_BUFFER_OVERFLOW;
        break;
      }

      // Not the first entry, set the offset before filling the new entry
      if (nextEntryOffset > 0) {
        StreamInfo->NextEntryOffset = nextEntryOffset;
        StreamInfo = (PFILE_STREAM_INFORMATION)((LPBYTE)StreamInfo +
                                                StreamInfo->NextEntryOffset);
      }

      // Fill the new entry
      StreamInfo->StreamNameLength = streamNameLength;
      memcpy(StreamInfo->StreamName, find->FindStreamData.cStreamName,
             streamNameLength);
      StreamInfo->StreamSize = find->FindStreamData.StreamSize;
      StreamInfo->StreamAllocationSize = find->FindStreamData.StreamSize;
      StreamInfo->NextEntryOffset = 0;
      ALIGN_ALLOCATION_SIZE(&StreamInfo->StreamAllocationSize,
                            DokanInstance->DokanOptions);

      *RemainingLength -= entrySize;
    }

    if (status != STATUS_BUFFER_OVERFLOW) {
      ClearFindStreamData(openInfo->StreamListHead);
    }

  } else {
    ClearFindStreamData(openInfo->StreamListHead);
  }

  return status;
}

VOID DispatchQueryInformation(HANDLE Handle, PEVENT_CONTEXT EventContext,
                              PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  DOKAN_FILE_INFO fileInfo;
  BY_HANDLE_FILE_INFORMATION byHandleFileInfo;
  ULONG remainingLength;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PDOKAN_OPEN_INFO openInfo;
  ULONG sizeOfEventInfo;

  sizeOfEventInfo =
      sizeof(EVENT_INFORMATION) - 8 + EventContext->Operation.File.BufferLength;

  CheckFileName(EventContext->Operation.File.FileName);

  ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));

  eventInfo = DispatchCommon(EventContext, sizeOfEventInfo, DokanInstance,
                             &fileInfo, &openInfo);

  eventInfo->BufferLength = EventContext->Operation.File.BufferLength;

  DbgPrint("###GetFileInfo %04d\n", openInfo != NULL ? openInfo->EventId : -1);

  if (DokanInstance->DokanOperations->GetFileInformation) {
    status = DokanInstance->DokanOperations->GetFileInformation(
        EventContext->Operation.File.FileName, &byHandleFileInfo, &fileInfo);
  }

  remainingLength = eventInfo->BufferLength;

  DbgPrint("\tresult =  %lx\n", status);

  if (status != STATUS_SUCCESS) {
    eventInfo->Status = STATUS_INVALID_PARAMETER;
    eventInfo->BufferLength = 0;
  } else {

    switch (EventContext->Operation.File.FileInformationClass) {
    case FileBasicInformation:
      DbgPrint("\tFileBasicInformation\n");
      status =
          DokanFillFileBasicInfo((PFILE_BASIC_INFORMATION)eventInfo->Buffer,
                                 &byHandleFileInfo, &remainingLength);
      break;

    case FileIdInformation:
      DbgPrint("\tFileIdInformation\n");
      status = DokanFillIdInfo((PFILE_ID_INFORMATION)eventInfo->Buffer,
                               &byHandleFileInfo, &remainingLength);
      break;

    case FileInternalInformation:
      DbgPrint("\tFileInternalInformation\n");
      status =
          DokanFillInternalInfo((PFILE_INTERNAL_INFORMATION)eventInfo->Buffer,
                                &byHandleFileInfo, &remainingLength);
      break;

    case FileEaInformation:
      DbgPrint("\tFileEaInformation\n");
      // status = STATUS_NOT_IMPLEMENTED;
      status = STATUS_SUCCESS;
      remainingLength -= sizeof(FILE_EA_INFORMATION);
      break;

    case FileStandardInformation:
      DbgPrint("\tFileStandardInformation\n");
      status = DokanFillFileStandardInfo(
          (PFILE_STANDARD_INFORMATION)eventInfo->Buffer, &byHandleFileInfo,
          &remainingLength, DokanInstance);
      break;

    case FileAllInformation:
      DbgPrint("\tFileAllInformation\n");
      status = DokanFillFileAllInfo((PFILE_ALL_INFORMATION)eventInfo->Buffer,
                                    &byHandleFileInfo, &remainingLength,
                                    EventContext, DokanInstance);
      break;

    case FileAlternateNameInformation:
      DbgPrint("\tFileAlternateNameInformation\n");
      status = STATUS_NOT_IMPLEMENTED;
      break;

    case FileAttributeTagInformation:
      DbgPrint("\tFileAttributeTagInformation\n");
      status = DokanFillFileAttributeTagInfo(
          (PFILE_ATTRIBUTE_TAG_INFORMATION)eventInfo->Buffer, &byHandleFileInfo,
          &remainingLength);
      break;

    case FileCompressionInformation:
      DbgPrint("\tFileCompressionInformation\n");
      status = STATUS_NOT_IMPLEMENTED;
      break;

    case FileNormalizedNameInformation:
      DbgPrint("\tFileNormalizedNameInformation\n");
    case FileNameInformation:
      // this case is not used because driver deal with
      DbgPrint("\tFileNameInformation\n");
      status = DokanFillFileNameInfo((PFILE_NAME_INFORMATION)eventInfo->Buffer,
                                     &byHandleFileInfo, &remainingLength,
                                     EventContext);
      break;

    case FileNetworkOpenInformation:
      DbgPrint("\tFileNetworkOpenInformation\n");
      status = DokanFillNetworkOpenInfo(
          (PFILE_NETWORK_OPEN_INFORMATION)eventInfo->Buffer, &byHandleFileInfo,
          &remainingLength, DokanInstance);
      break;

    case FilePositionInformation:
      // this case is not used because driver deal with
      DbgPrint("\tFilePositionInformation\n");
      status = DokanFillFilePositionInfo(
          (PFILE_POSITION_INFORMATION)eventInfo->Buffer, &byHandleFileInfo,
          &remainingLength);
      break;
    case FileStreamInformation:
      DbgPrint("FileStreamInformation\n");
      status = DokanFindStreams((PFILE_STREAM_INFORMATION)eventInfo->Buffer,
                                &fileInfo, EventContext, DokanInstance,
                                &remainingLength);
      break;
    case FileNetworkPhysicalNameInformation:
      DbgPrint("FileNetworkPhysicalNameInformation\n");
      status = DokanFillNetworkPhysicalNameInfo(
          (PFILE_NETWORK_PHYSICAL_NAME_INFORMATION)eventInfo->Buffer,
          &byHandleFileInfo, &remainingLength, EventContext);
      break;
    default: {
      status = STATUS_INVALID_PARAMETER;
      DbgPrint("  unknown type:%d\n",
               EventContext->Operation.File.FileInformationClass);
    } break;
    }

    eventInfo->Status = status;
    eventInfo->BufferLength =
        EventContext->Operation.File.BufferLength - remainingLength;
  }

  DbgPrint("\tDispatchQueryInformation result =  %lx\n", status);

  // information for FileSystem
  if (openInfo != NULL)
    openInfo->UserContext = fileInfo.Context;

  SendEventInformation(Handle, eventInfo, sizeOfEventInfo, DokanInstance);
  free(eventInfo);
  return;
}
