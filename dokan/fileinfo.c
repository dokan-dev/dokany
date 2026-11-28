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

#include "dokani.h"
#include "fileinfo.h"

#include <ntstatus.h>
#include <stdio.h>
#include <assert.h>
#include <stddef.h>

C_ASSERT(sizeof(FILE_STANDARD_LINK_INFORMATION) == 12);
C_ASSERT(sizeof(FILE_STAT_INFORMATION) == 72);
C_ASSERT(sizeof(FILE_STAT_LX_INFORMATION) == 96);
C_ASSERT(sizeof(DOKAN_FILE_INFORMATION) == 80);
C_ASSERT(sizeof(DOKAN_FIND_DATA) == 592);

LARGE_INTEGER DokanResolveChangeTime(LARGE_INTEGER ChangeTime,
                                     LARGE_INTEGER LastWriteTime) {
  return ChangeTime.QuadPart != 0 ? ChangeTime : LastWriteTime;
}

// Positive AllocationSize is reported verbatim, DOKAN_ALLOCATION_SIZE_ZERO
// reports literal 0, and 0 (or any other negative value) auto-aligns EndOfFile
// up to AllocationUnitSize, clamping at MAXLONGLONG to avoid signed overflow.
LARGE_INTEGER DokanResolveAllocationSize(LARGE_INTEGER AllocationSize,
                                         LARGE_INTEGER EndOfFile,
                                         PDOKAN_OPTIONS DokanOptions) {
  if (AllocationSize.QuadPart > 0) {
    return AllocationSize;
  }
  if (AllocationSize.QuadPart == DOKAN_ALLOCATION_SIZE_ZERO) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return zero;
  }
  LARGE_INTEGER resolved = EndOfFile;
  if (resolved.QuadPart < 0) {
    resolved.QuadPart = 0;
  }
  LONGLONG allocUnit = (LONGLONG)DokanOptions->AllocationUnitSize;
  if (allocUnit > 0) {
    LONGLONG rem = resolved.QuadPart % allocUnit;
    if (rem > 0) {
      LONGLONG pad = allocUnit - rem;
      if (resolved.QuadPart > MAXLONGLONG - pad) {
        resolved.QuadPart = MAXLONGLONG;
      } else {
        resolved.QuadPart += pad;
      }
    }
  }
  return resolved;
}

ACCESS_MASK DokanResolveEffectiveAccess(ACCESS_MASK EffectiveAccess) {
  return EffectiveAccess != 0 ? EffectiveAccess : FILE_ALL_ACCESS;
}

VOID DokanFillFileId128(ULONG64 FileIndex, FILE_ID_128 *FileId) {
  ZeroMemory(FileId->Identifier, sizeof(FileId->Identifier));
  for (size_t i = 0; i < sizeof(FileIndex); ++i) {
    FileId->Identifier[i] = (BYTE)((FileIndex >> (i * 8)) & 0xFF);
  }
}

NTSTATUS
DokanFillFileBasicInfo(PFILE_BASIC_INFORMATION BasicInfo,
                       const DOKAN_FILE_INFORMATION *FileInfo,
                       PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_BASIC_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  BasicInfo->CreationTime = FileInfo->CreationTime;
  BasicInfo->LastAccessTime = FileInfo->LastAccessTime;
  BasicInfo->LastWriteTime = FileInfo->LastWriteTime;
  BasicInfo->ChangeTime =
      DokanResolveChangeTime(FileInfo->ChangeTime, FileInfo->LastWriteTime);
  BasicInfo->FileAttributes = FileInfo->FileAttributes;

  *RemainingLength -= sizeof(FILE_BASIC_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileStandardInfo(PFILE_STANDARD_INFORMATION StandardInfo,
                          const DOKAN_FILE_INFORMATION *FileInfo,
                          PULONG RemainingLength,
                          PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_STANDARD_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  StandardInfo->AllocationSize =
      DokanResolveAllocationSize(FileInfo->AllocationSize, FileInfo->EndOfFile,
                                 DokanInstance->DokanOptions);
  StandardInfo->EndOfFile = FileInfo->EndOfFile;
  StandardInfo->NumberOfLinks = FileInfo->NumberOfLinks;
  // Overwritten from FCB state by dokan3.sys (sys/fileinfo.c).
  StandardInfo->DeletePending = FALSE;
  StandardInfo->Directory =
      (FileInfo->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? TRUE : FALSE;

  *RemainingLength -= sizeof(FILE_STANDARD_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFilePositionInfo(PFILE_POSITION_INFORMATION PosInfo,
                          const DOKAN_FILE_INFORMATION *FileInfo,
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
                      const DOKAN_FILE_INFORMATION *FileInfo,
                      PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_INTERNAL_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  InternalInfo->IndexNumber.QuadPart = (LONGLONG)FileInfo->FileIndex;

  *RemainingLength -= sizeof(FILE_INTERNAL_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileAllInfo(PFILE_ALL_INFORMATION AllInfo,
                     const DOKAN_FILE_INFORMATION *FileInfo,
                     PULONG RemainingLength, PDOKAN_INSTANCE DokanInstance) {
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

  // AllInfo->NameInformation.FileName is populated by the Kernel

  // the size except of FILE_NAME_INFORMATION
  allRemainingLength -=
      FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName);

  *RemainingLength = allRemainingLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileNameInfo(PFILE_NAME_INFORMATION NameInfo,
                      const DOKAN_FILE_INFORMATION *FileInfo,
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
                              const DOKAN_FILE_INFORMATION *FileInfo,
                              PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  AttrTagInfo->FileAttributes = FileInfo->FileAttributes;
  AttrTagInfo->ReparseTag = FileInfo->ReparseTag;

  *RemainingLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillNetworkOpenInfo(PFILE_NETWORK_OPEN_INFORMATION NetInfo,
                         const DOKAN_FILE_INFORMATION *FileInfo,
                         PULONG RemainingLength,
                         PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  NetInfo->CreationTime = FileInfo->CreationTime;
  NetInfo->LastAccessTime = FileInfo->LastAccessTime;
  NetInfo->LastWriteTime = FileInfo->LastWriteTime;
  NetInfo->ChangeTime =
      DokanResolveChangeTime(FileInfo->ChangeTime, FileInfo->LastWriteTime);
  NetInfo->AllocationSize =
      DokanResolveAllocationSize(FileInfo->AllocationSize, FileInfo->EndOfFile,
                                 DokanInstance->DokanOptions);
  NetInfo->EndOfFile = FileInfo->EndOfFile;
  NetInfo->FileAttributes = FileInfo->FileAttributes;

  *RemainingLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillIdInfo(PFILE_ID_INFORMATION IdInfo,
                const DOKAN_FILE_INFORMATION *FileInfo,
                PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_ID_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  IdInfo->VolumeSerialNumber = FileInfo->VolumeSerialNumber;
  DokanFillFileId128(FileInfo->FileIndex, &IdInfo->FileId);

  *RemainingLength -= sizeof(FILE_ID_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFillFileStandardLinkInfo(PFILE_STANDARD_LINK_INFORMATION LinkInfo,
                              const DOKAN_FILE_INFORMATION *FileInfo,
                              PULONG RemainingLength) {
  if (*RemainingLength < sizeof(FILE_STANDARD_LINK_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  RtlZeroMemory(LinkInfo, sizeof(FILE_STANDARD_LINK_INFORMATION));
  LinkInfo->NumberOfAccessibleLinks = FileInfo->NumberOfLinks;
  LinkInfo->TotalNumberOfLinks = FileInfo->NumberOfLinks;
  // A future dokan3.sys update will populate DeletePending from
  // DokanFCBIsPendingDeletion(fcb) in sys/fileinfo.c alongside
  // FileStandardInformation and FileAllInformation.
  LinkInfo->DeletePending = FALSE;
  LinkInfo->Directory =
      (FileInfo->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? TRUE : FALSE;

  *RemainingLength -= sizeof(FILE_STANDARD_LINK_INFORMATION);

  return STATUS_SUCCESS;
}

#define DOKAN_FILL_STAT_COMMON(Dest, FileInfo, DokanOptions)                   \
  do {                                                                         \
    (Dest)->FileId.QuadPart = (LONGLONG)(FileInfo)->FileIndex;                 \
    (Dest)->CreationTime = (FileInfo)->CreationTime;                           \
    (Dest)->LastAccessTime = (FileInfo)->LastAccessTime;                       \
    (Dest)->LastWriteTime = (FileInfo)->LastWriteTime;                         \
    (Dest)->ChangeTime = DokanResolveChangeTime((FileInfo)->ChangeTime,        \
                                                (FileInfo)->LastWriteTime);    \
    (Dest)->AllocationSize = DokanResolveAllocationSize(                       \
        (FileInfo)->AllocationSize, (FileInfo)->EndOfFile, (DokanOptions));    \
    (Dest)->EndOfFile = (FileInfo)->EndOfFile;                                 \
    (Dest)->FileAttributes = (FileInfo)->FileAttributes;                       \
    (Dest)->ReparseTag = (FileInfo)->ReparseTag;                               \
    (Dest)->NumberOfLinks = (FileInfo)->NumberOfLinks;                         \
    (Dest)->EffectiveAccess =                                                  \
        DokanResolveEffectiveAccess((FileInfo)->EffectiveAccess);              \
  } while (0)

NTSTATUS
DokanFillFileStatInfo(PFILE_STAT_INFORMATION StatInfo,
                      const DOKAN_FILE_INFORMATION *FileInfo,
                      PULONG RemainingLength, PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_STAT_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  DOKAN_FILL_STAT_COMMON(StatInfo, FileInfo, DokanInstance->DokanOptions);

  *RemainingLength -= sizeof(FILE_STAT_INFORMATION);

  return STATUS_SUCCESS;
}

C_ASSERT(FIELD_OFFSET(FILE_STAT_LX_INFORMATION, LxFlags) ==
         sizeof(FILE_STAT_INFORMATION));

NTSTATUS
DokanFillFileStatLxInfo(PFILE_STAT_LX_INFORMATION StatLxInfo,
                        const DOKAN_FILE_INFORMATION *FileInfo,
                        PULONG RemainingLength, PDOKAN_INSTANCE DokanInstance) {
  if (*RemainingLength < sizeof(FILE_STAT_LX_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  RtlZeroMemory(StatLxInfo, sizeof(FILE_STAT_LX_INFORMATION));
  DOKAN_FILL_STAT_COMMON(StatLxInfo, FileInfo, DokanInstance->DokanOptions);

  *RemainingLength -= sizeof(FILE_STAT_LX_INFORMATION);

  return STATUS_SUCCESS;
}

#undef DOKAN_FILL_STAT_COMMON

/**
 * \struct DOKAN_FIND_STREAM_CONTEXT
 * \brief Context for filling FILE_STREAM_INFORMATION entries during a
 * FindStreams dispatch.
 */
typedef struct _DOKAN_FIND_STREAM_CONTEXT {
  /** Active IO event receiving the stream information buffer. */
  PDOKAN_IO_EVENT IoEvent;
  /** Maximum byte capacity of the result buffer for this query. */
  ULONG ResultBufferSize;
  /** Byte offset of the most recently written stream entry. */
  ULONG LastEntryOffset;
  /** Unaligned byte size of the most recently written stream entry. */
  ULONG LastUnalignedSize;
  /** Whether at least one complete stream entry has been written. */
  BOOL HasEntries;
  /** Whether an entry failed to fit in the result buffer. */
  BOOL Overflowed;
} DOKAN_FIND_STREAM_CONTEXT, *PDOKAN_FIND_STREAM_CONTEXT;

static ULONG GetQueryResultBufferSize(PDOKAN_IO_EVENT IoEvent) {
  ULONG resultBufferSize = IoEvent->EventContext->Operation.File.BufferLength;
  ULONG allocBufferSize = (ULONG)IOEVENT_RESULT_BUFFER_SIZE(IoEvent);
  return resultBufferSize < allocBufferSize ? resultBufferSize
                                            : allocBufferSize;
}

static BOOL WINAPI DokanFillFindStreamData(
    _In_ PWIN32_FIND_STREAM_DATA FindStreamData, _In_ PVOID FindStreamContext) {
  PDOKAN_FIND_STREAM_CONTEXT context =
      (PDOKAN_FIND_STREAM_CONTEXT)FindStreamContext;
  if (context->Overflowed) {
    return FALSE;
  }

  PDOKAN_IO_EVENT ioEvent = context->IoEvent;
  ULONG resultBufferSize = context->ResultBufferSize;
  ULONG headerBytes = (ULONG)FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName);
  ULONG streamNameLength =
      (ULONG)wcslen(FindStreamData->cStreamName) * sizeof(WCHAR);
  ULONG unalignedSize = headerBytes + streamNameLength;

  if (unalignedSize < headerBytes) {
    context->Overflowed = TRUE;
    return FALSE;
  }

  PCHAR baseBuffer = (PCHAR)ioEvent->EventResult->Buffer;
  ULONG entryOffset = 0;

  if (context->HasEntries) {
    ULONG prevAlignedSize = (ULONG)QuadAlign(context->LastUnalignedSize);
    if (prevAlignedSize < context->LastUnalignedSize ||
        context->LastEntryOffset > resultBufferSize ||
        prevAlignedSize > resultBufferSize - context->LastEntryOffset) {
      context->Overflowed = TRUE;
      return FALSE;
    }
    entryOffset = context->LastEntryOffset + prevAlignedSize;
    if (unalignedSize > resultBufferSize - entryOffset) {
      context->Overflowed = TRUE;
      return FALSE;
    }
    if (prevAlignedSize > context->LastUnalignedSize) {
      RtlZeroMemory(baseBuffer + context->LastEntryOffset +
                        context->LastUnalignedSize,
                    prevAlignedSize - context->LastUnalignedSize);
    }
    ((PFILE_STREAM_INFORMATION)(baseBuffer + context->LastEntryOffset))
        ->NextEntryOffset = prevAlignedSize;
  } else if (unalignedSize > resultBufferSize) {
    context->Overflowed = TRUE;
    return FALSE;
  }

  PFILE_STREAM_INFORMATION streamInfo =
      (PFILE_STREAM_INFORMATION)(baseBuffer + entryOffset);
  streamInfo->NextEntryOffset = 0;
  streamInfo->StreamNameLength = streamNameLength;
  if (streamNameLength > 0) {
    RtlCopyMemory(streamInfo->StreamName, FindStreamData->cStreamName,
                  streamNameLength);
  }
  streamInfo->StreamSize = FindStreamData->StreamSize;
  streamInfo->StreamAllocationSize = FindStreamData->StreamSize;
  ALIGN_ALLOCATION_SIZE(&streamInfo->StreamAllocationSize,
                        ioEvent->DokanInstance->DokanOptions);

  context->LastEntryOffset = entryOffset;
  context->LastUnalignedSize = unalignedSize;
  context->HasEntries = TRUE;
  return TRUE;
}

static VOID
DokanEndDispatchGetFileInformation(PDOKAN_IO_EVENT IoEvent,
                                   const DOKAN_FILE_INFORMATION *DokanFileInfo,
                                   NTSTATUS Status) {
  ULONG initialLength = GetQueryResultBufferSize(IoEvent);
  ULONG remainingLength = initialLength;

  DbgPrint("\tresult =  %lx\n", Status);

  if (Status != STATUS_SUCCESS) {
    IoEvent->EventResult->Status = Status;
    IoEvent->EventResult->BufferLength = 0;
  } else {
    ULONG fileInformationClass =
        IoEvent->EventContext->Operation.File.FileInformationClass;
    switch (fileInformationClass) {
    case FileBasicInformation:
      DbgPrint("\tFileBasicInformation\n");
      Status = DokanFillFileBasicInfo(
          (PFILE_BASIC_INFORMATION)IoEvent->EventResult->Buffer, DokanFileInfo,
          &remainingLength);
      break;

    case FileIdInformation:
      DbgPrint("\tFileIdInformation\n");
      Status =
          DokanFillIdInfo((PFILE_ID_INFORMATION)IoEvent->EventResult->Buffer,
                          DokanFileInfo, &remainingLength);
      break;

    case FileInternalInformation:
      DbgPrint("\tFileInternalInformation\n");
      Status = DokanFillInternalInfo(
          (PFILE_INTERNAL_INFORMATION)IoEvent->EventResult->Buffer,
          DokanFileInfo, &remainingLength);
      break;

    case FileEaInformation:
      DbgPrint("\tFileEaInformation\n");
      if (remainingLength < sizeof(FILE_EA_INFORMATION)) {
        Status = STATUS_BUFFER_OVERFLOW;
      } else {
        ((PFILE_EA_INFORMATION)IoEvent->EventResult->Buffer)->EaSize = 0;
        Status = STATUS_SUCCESS;
        remainingLength -= sizeof(FILE_EA_INFORMATION);
      }
      break;

    case FileStandardInformation:
      DbgPrint("\tFileStandardInformation\n");
      Status = DokanFillFileStandardInfo(
          (PFILE_STANDARD_INFORMATION)IoEvent->EventResult->Buffer,
          DokanFileInfo, &remainingLength, IoEvent->DokanInstance);
      break;

    case FileStandardLinkInformation:
      DbgPrint("\tFileStandardLinkInformation\n");
      Status = DokanFillFileStandardLinkInfo(
          (PFILE_STANDARD_LINK_INFORMATION)IoEvent->EventResult->Buffer,
          DokanFileInfo, &remainingLength);
      break;

    case FileAllInformation:
      DbgPrint("\tFileAllInformation\n");
      Status = DokanFillFileAllInfo(
          (PFILE_ALL_INFORMATION)IoEvent->EventResult->Buffer, DokanFileInfo,
          &remainingLength, IoEvent->DokanInstance);
      break;

    case FileAlternateNameInformation:
      DbgPrint("\tFileAlternateNameInformation\n");
      Status = STATUS_NOT_IMPLEMENTED;
      break;

    case FileAttributeTagInformation:
      DbgPrint("\tFileAttributeTagInformation\n");
      Status = DokanFillFileAttributeTagInfo(
          (PFILE_ATTRIBUTE_TAG_INFORMATION)IoEvent->EventResult->Buffer,
          DokanFileInfo, &remainingLength);
      break;

    case FileCompressionInformation:
      DbgPrint("\tFileCompressionInformation\n");
      Status = STATUS_NOT_IMPLEMENTED;
      break;

    case FileNormalizedNameInformation:
    case FileNameInformation:
      // this case is not used because driver deal with
      if (fileInformationClass == FileNormalizedNameInformation) {
        DbgPrint("\tFileNormalizedNameInformation\n");
      } else {
        DbgPrint("\tFileNameInformation\n");
      }
      Status = DokanFillFileNameInfo(
          (PFILE_NAME_INFORMATION)IoEvent->EventResult->Buffer, DokanFileInfo,
          &remainingLength, IoEvent->EventContext);
      break;

    case FileNetworkOpenInformation:
      DbgPrint("\tFileNetworkOpenInformation\n");
      Status = DokanFillNetworkOpenInfo(
          (PFILE_NETWORK_OPEN_INFORMATION)IoEvent->EventResult->Buffer,
          DokanFileInfo, &remainingLength, IoEvent->DokanInstance);
      break;

    case FilePositionInformation:
      // this case is not used because driver deal with
      DbgPrint("\tFilePositionInformation\n");
      Status = DokanFillFilePositionInfo(
          (PFILE_POSITION_INFORMATION)IoEvent->EventResult->Buffer,
          DokanFileInfo, &remainingLength);
      break;

    case FileStatInformation:
      DbgPrint("\tFileStatInformation\n");
      Status = DokanFillFileStatInfo(
          (PFILE_STAT_INFORMATION)IoEvent->EventResult->Buffer, DokanFileInfo,
          &remainingLength, IoEvent->DokanInstance);
      break;

    case FileStatLxInformation:
      DbgPrint("\tFileStatLxInformation\n");
      Status = DokanFillFileStatLxInfo(
          (PFILE_STAT_LX_INFORMATION)IoEvent->EventResult->Buffer,
          DokanFileInfo, &remainingLength, IoEvent->DokanInstance);
      break;

    case FileStreamInformation:
      DbgPrint("FileStreamInformation (internal error)\n");
      // shouldn't get here
      Status = STATUS_INTERNAL_ERROR;
      break;
    default: {
      Status = STATUS_INVALID_PARAMETER;
      DbgPrint("  unknown type:%d\n", fileInformationClass);
    } break;
    }

    IoEvent->EventResult->Status = Status;
    IoEvent->EventResult->BufferLength = initialLength - remainingLength;
  }

  DbgPrint("\tDispatchQueryInformation result =  %lx\n", Status);
  EventCompletion(IoEvent);
}

static VOID DokanEndDispatchFindStreams(PDOKAN_FIND_STREAM_CONTEXT Context,
                                        NTSTATUS Status) {
  PDOKAN_IO_EVENT ioEvent = Context->IoEvent;

  DbgPrint("\tresult =  %lx\n", Status);

  // STATUS_PENDING should not be passed to this function
  if (Status == STATUS_PENDING) {
    DbgPrint("Dokan Error: DokanEndDispatchFindStreams() failed because "
             "STATUS_PENDING was supplied for ResultStatus.\n");
    Status = STATUS_INTERNAL_ERROR;
  }

  if (Status == STATUS_SUCCESS && Context->Overflowed) {
    Status = STATUS_BUFFER_OVERFLOW;
  }

  if ((Status == STATUS_SUCCESS || Status == STATUS_BUFFER_OVERFLOW) &&
      Context->HasEntries) {
    assert(Context->LastEntryOffset <= Context->ResultBufferSize &&
           Context->LastUnalignedSize <=
               Context->ResultBufferSize - Context->LastEntryOffset);
    ioEvent->EventResult->BufferLength =
        Context->LastEntryOffset + Context->LastUnalignedSize;
  } else {
    ioEvent->EventResult->BufferLength = 0;
  }

  ioEvent->EventResult->Status = Status;
  DbgPrint("\tDokanEndDispatchFindStreams result =  0x%x\n", Status);
  EventCompletion(ioEvent);
}

VOID DispatchQueryInformation(PDOKAN_IO_EVENT IoEvent) {
  DOKAN_FILE_INFORMATION dokanFileInfo;
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
    // https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_stream_information
    ULONG resultBufferSize = GetQueryResultBufferSize(IoEvent);
    if (resultBufferSize <
        (ULONG)FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName)) {
      status = STATUS_BUFFER_TOO_SMALL;
    } else if (IoEvent->DokanInstance->DokanOperations->FindStreams) {
      DOKAN_FIND_STREAM_CONTEXT findStreamContext;
      findStreamContext.IoEvent = IoEvent;
      findStreamContext.ResultBufferSize = resultBufferSize;
      findStreamContext.LastEntryOffset = 0;
      findStreamContext.LastUnalignedSize = 0;
      findStreamContext.HasEntries = FALSE;
      findStreamContext.Overflowed = FALSE;

      status = IoEvent->DokanInstance->DokanOperations->FindStreams(
          IoEvent->EventContext->Operation.File.FileName,
          DokanFillFindStreamData, &findStreamContext, &IoEvent->DokanFileInfo);
      DokanEndDispatchFindStreams(&findStreamContext, status);
      return;
    } else {
      status = STATUS_NOT_IMPLEMENTED;
    }
  } else if (IoEvent->DokanInstance->DokanOperations->GetFileInformation) {
    ZeroMemory(&dokanFileInfo, sizeof(DOKAN_FILE_INFORMATION));
    status = IoEvent->DokanInstance->DokanOperations->GetFileInformation(
        IoEvent->EventContext->Operation.File.FileName, &dokanFileInfo,
        &IoEvent->DokanFileInfo);
    DokanEndDispatchGetFileInformation(IoEvent, &dokanFileInfo, status);
    return;
  } else {
    status = STATUS_NOT_IMPLEMENTED;
  }

  IoEvent->EventResult->BufferLength = 0;
  IoEvent->EventResult->Status = status;
  EventCompletion(IoEvent);
}
