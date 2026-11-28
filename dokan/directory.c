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
#include "list.h"
#include "dokan_pool.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DOKAN_MATCH_STACK_LEN 260

C_ASSERT(FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, NextEntryOffset) == 0);
C_ASSERT(FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, NextEntryOffset) == 0);
C_ASSERT(FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, NextEntryOffset) == 0);
C_ASSERT(FIELD_OFFSET(FILE_NAMES_INFORMATION, NextEntryOffset) == 0);
C_ASSERT(FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, NextEntryOffset) == 0);
C_ASSERT(FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, NextEntryOffset) == 0);
C_ASSERT(FIELD_OFFSET(FILE_ID_EXTD_DIR_INFO, NextEntryOffset) == 0);
C_ASSERT(FIELD_OFFSET(FILE_ID_EXTD_BOTH_DIR_INFORMATION, NextEntryOffset) == 0);

static VOID SetDirEntryNextOffset(PVOID Entry, ULONG Offset) {
  RtlCopyMemory(Entry, &Offset, sizeof(Offset));
}

static VOID DokanFillDirTimesAndSizes(
    const DOKAN_FIND_DATA *FindData, PDOKAN_INSTANCE DokanInstance,
    PLARGE_INTEGER CreationTime, PLARGE_INTEGER LastAccessTime,
    PLARGE_INTEGER LastWriteTime, PLARGE_INTEGER ChangeTime,
    PLARGE_INTEGER EndOfFile, PLARGE_INTEGER AllocationSize) {
  *EndOfFile = FindData->EndOfFile;
  *AllocationSize =
      DokanResolveAllocationSize(FindData->AllocationSize, FindData->EndOfFile,
                                 DokanInstance->DokanOptions);
  *CreationTime = FindData->CreationTime;
  *LastAccessTime = FindData->LastAccessTime;
  *LastWriteTime = FindData->LastWriteTime;
  *ChangeTime =
      DokanResolveChangeTime(FindData->ChangeTime, FindData->LastWriteTime);
}

static PWCHAR GetDirectorySearchPattern(PDOKAN_IO_EVENT IoEvent) {
  if (IoEvent->EventContext->Operation.Directory.SearchPatternLength == 0) {
    return NULL;
  }
  return (PWCHAR)((SIZE_T)&IoEvent->EventContext->Operation.Directory
                      .SearchPatternBase[0] +
                  (SIZE_T)IoEvent->EventContext->Operation.Directory
                      .SearchPatternOffset);
}

static ULONG GetDirectoryInfoHeaderSize(FILE_INFORMATION_CLASS DirectoryInfo) {
  switch (DirectoryInfo) {
  case FileDirectoryInformation:
    return (ULONG)FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
  case FileFullDirectoryInformation:
    return (ULONG)FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
  case FileIdFullDirectoryInformation:
    return (ULONG)FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName);
  case FileNamesInformation:
    return (ULONG)FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
  case FileBothDirectoryInformation:
    return (ULONG)FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
  case FileIdBothDirectoryInformation:
    return (ULONG)FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName);
  case FileIdExtdDirectoryInformation:
    return (ULONG)FIELD_OFFSET(FILE_ID_EXTD_DIR_INFO, FileName);
  case FileIdExtdBothDirectoryInformation:
    return (ULONG)FIELD_OFFSET(FILE_ID_EXTD_BOTH_DIR_INFORMATION, FileName);
  default:
    return 0;
  }
}

// Zero-initialize Buffer for (headerBytes + CopyNameBytes) bytes and populate
// the directory entry. NextEntryOffset and per-class fields not carried by
// DOKAN_FIND_DATA (EaSize, ShortNameLength, ShortName) remain zero.
static VOID DokanWriteDirectoryEntry(FILE_INFORMATION_CLASS DirectoryInfo,
                                     PVOID Buffer,
                                     const DOKAN_FIND_DATA *FindData,
                                     ULONG Index, ULONG NameBytes,
                                     ULONG CopyNameBytes,
                                     PDOKAN_INSTANCE DokanInstance) {
  ULONG headerBytes = GetDirectoryInfoHeaderSize(DirectoryInfo);
  if (headerBytes == 0) {
    return;
  }
  RtlZeroMemory(Buffer, headerBytes + CopyNameBytes);

#define DOKAN_FILL_DIR_COMMON(entry)                                           \
  do {                                                                         \
    (entry)->FileIndex = Index;                                                \
    (entry)->FileAttributes = FindData->FileAttributes;                        \
    (entry)->FileNameLength = NameBytes;                                       \
    DokanFillDirTimesAndSizes(FindData, DokanInstance, &(entry)->CreationTime, \
                              &(entry)->LastAccessTime,                        \
                              &(entry)->LastWriteTime, &(entry)->ChangeTime,   \
                              &(entry)->EndOfFile, &(entry)->AllocationSize);  \
    if (CopyNameBytes > 0) {                                                   \
      RtlCopyMemory((entry)->FileName, FindData->FileName, CopyNameBytes);     \
    }                                                                          \
  } while (0)

  switch (DirectoryInfo) {
  case FileDirectoryInformation: {
    PFILE_DIRECTORY_INFORMATION entry = (PFILE_DIRECTORY_INFORMATION)Buffer;
    DOKAN_FILL_DIR_COMMON(entry);
    break;
  }
  case FileFullDirectoryInformation: {
    PFILE_FULL_DIR_INFORMATION entry = (PFILE_FULL_DIR_INFORMATION)Buffer;
    DOKAN_FILL_DIR_COMMON(entry);
    break;
  }
  case FileIdFullDirectoryInformation: {
    PFILE_ID_FULL_DIR_INFORMATION entry = (PFILE_ID_FULL_DIR_INFORMATION)Buffer;
    DOKAN_FILL_DIR_COMMON(entry);
    entry->FileId.QuadPart = (LONGLONG)FindData->FileIndex;
    break;
  }
  case FileBothDirectoryInformation: {
    PFILE_BOTH_DIR_INFORMATION entry = (PFILE_BOTH_DIR_INFORMATION)Buffer;
    DOKAN_FILL_DIR_COMMON(entry);
    break;
  }
  case FileIdBothDirectoryInformation: {
    PFILE_ID_BOTH_DIR_INFORMATION entry = (PFILE_ID_BOTH_DIR_INFORMATION)Buffer;
    DOKAN_FILL_DIR_COMMON(entry);
    entry->FileId.QuadPart = (LONGLONG)FindData->FileIndex;
    break;
  }
  case FileIdExtdDirectoryInformation: {
    PFILE_ID_EXTD_DIR_INFO entry = (PFILE_ID_EXTD_DIR_INFO)Buffer;
    DOKAN_FILL_DIR_COMMON(entry);
    entry->ReparsePointTag = FindData->ReparseTag;
    DokanFillFileId128(FindData->FileIndex, &entry->FileId);
    break;
  }
  case FileIdExtdBothDirectoryInformation: {
    PFILE_ID_EXTD_BOTH_DIR_INFORMATION entry =
        (PFILE_ID_EXTD_BOTH_DIR_INFORMATION)Buffer;
    DOKAN_FILL_DIR_COMMON(entry);
    entry->ReparsePointTag = FindData->ReparseTag;
    DokanFillFileId128(FindData->FileIndex, &entry->FileId);
    break;
  }
  case FileNamesInformation: {
    PFILE_NAMES_INFORMATION entry = (PFILE_NAMES_INFORMATION)Buffer;
    entry->FileIndex = Index;
    entry->FileNameLength = NameBytes;
    if (CopyNameBytes > 0) {
      RtlCopyMemory(entry->FileName, FindData->FileName, CopyNameBytes);
    }
    break;
  }
  default:
    break;
  }

#undef DOKAN_FILL_DIR_COMMON
}

/**
 * \struct DOKAN_FILL_FIND_CONTEXT
 * \brief Per-dispatch fill context passed via DOKAN_FILE_INFO.ProcessingContext
 * so DokanFillFileData can latch allocation failure even when a filesystem
 * ignores its non-zero return value.
 */
typedef struct _DOKAN_FILL_FIND_CONTEXT {
  /** Destination vector collecting DOKAN_FIND_DATA entries for the chunk. */
  PDOKAN_VECTOR DirList;
  /** Latched TRUE if DokanVector_PushBack fails to allocate memory. */
  BOOL AllocationFailed;
} DOKAN_FILL_FIND_CONTEXT, *PDOKAN_FILL_FIND_CONTEXT;

int WINAPI DokanFillFileData(_In_ const DOKAN_FIND_DATA *FindData,
                             _Inout_ PDOKAN_FILE_INFO FileInfo) {
  assert(FileInfo->ProcessingContext);
  PDOKAN_FILL_FIND_CONTEXT fillContext =
      (PDOKAN_FILL_FIND_CONTEXT)FileInfo->ProcessingContext;
  if (fillContext->AllocationFailed ||
      !DokanVector_PushBack(fillContext->DirList, FindData)) {
    fillContext->AllocationFailed = TRUE;
    return 1;
  }
  return 0;
}

// Write entries from the already-filtered DirList (holding logical 0-based
// indices [BaseIndex, BaseIndex + count)) starting at FileIndex into
// EventResult->Buffer. On STATUS_SUCCESS, *NextIndex receives the next 0-based
// FileIndex cursor; on error or overflow, *NextIndex stays at FileIndex.
static NTSTATUS MatchFiles(PDOKAN_IO_EVENT IoEvent, PDOKAN_VECTOR DirList,
                           ULONG BaseIndex, PULONG NextIndex) {
  FILE_INFORMATION_CLASS fileInfoClass =
      IoEvent->EventContext->Operation.Directory.FileInformationClass;
  ULONG headerBytes = GetDirectoryInfoHeaderSize(fileInfoClass);
  ULONG totalBufferLength =
      IoEvent->EventContext->Operation.Directory.BufferLength;
  ULONG allocBufferLength = (ULONG)IOEVENT_RESULT_BUFFER_SIZE(IoEvent);
  if (totalBufferLength > allocBufferLength) {
    totalBufferLength = allocBufferLength;
  }

  ULONG startIndex = IoEvent->EventContext->Operation.Directory.FileIndex;
  *NextIndex = startIndex;

  if (headerBytes == 0 || totalBufferLength < headerBytes) {
    IoEvent->EventResult->BufferLength = 0;
    return STATUS_INFO_LENGTH_MISMATCH;
  }

  if (startIndex < BaseIndex) {
    IoEvent->EventResult->BufferLength = 0;
    return STATUS_INTERNAL_ERROR;
  }

  PCHAR baseBuffer = (PCHAR)IoEvent->EventResult->Buffer;
  size_t count = DokanVector_GetCount(DirList);
  size_t localStart = (size_t)(startIndex - BaseIndex);
  ULONG usedBytes = 0;
  ULONG lastEntryOffset = 0;
  ULONG lastUnalignedSize = 0;
  ULONG entryCount = 0;
  ULONG nextIndex = startIndex;

  for (size_t i = localStart; i < count; ++i) {
    if (i >= (size_t)(ULONG_MAX - BaseIndex)) {
      if (entryCount == 0) {
        IoEvent->EventResult->BufferLength = 0;
        return STATUS_INTEGER_OVERFLOW;
      }
      break;
    }
    ULONG logicalIndex = BaseIndex + (ULONG)i;
    const DOKAN_FIND_DATA *findData =
        (const DOKAN_FIND_DATA *)DokanVector_GetItem(DirList, i);
    size_t nameChars = wcsnlen(findData->FileName, MAX_PATH);
    if (nameChars >= MAX_PATH) {
      IoEvent->EventResult->BufferLength = 0;
      return STATUS_INTERNAL_ERROR;
    }
    ULONG nameBytes = (ULONG)nameChars * sizeof(WCHAR);
    ULONG unalignedSize = headerBytes + nameBytes;
    if (unalignedSize < headerBytes) {
      break;
    }

    ULONG entryOffset = 0;
    if (entryCount == 0) {
      if (totalBufferLength < unalignedSize) {
        ULONG avail =
            (totalBufferLength - headerBytes) & ~(ULONG)(sizeof(WCHAR) - 1);
        ULONG writtenSize = headerBytes + avail;
        DokanWriteDirectoryEntry(fileInfoClass, baseBuffer, findData,
                                 logicalIndex + 1, nameBytes, avail,
                                 IoEvent->DokanInstance);
        IoEvent->EventResult->BufferLength = writtenSize;
        return STATUS_BUFFER_OVERFLOW;
      }
    } else {
      ULONG prevAlignedSize = (ULONG)QuadAlign(lastUnalignedSize);
      if (prevAlignedSize < lastUnalignedSize ||
          prevAlignedSize > totalBufferLength - lastEntryOffset) {
        DbgPrint("  no memory\n");
        break;
      }
      entryOffset = lastEntryOffset + prevAlignedSize;
      if (unalignedSize > totalBufferLength - entryOffset) {
        DbgPrint("  no memory\n");
        break;
      }
      if (prevAlignedSize > lastUnalignedSize) {
        RtlZeroMemory(baseBuffer + lastEntryOffset + lastUnalignedSize,
                      prevAlignedSize - lastUnalignedSize);
      }
      SetDirEntryNextOffset(baseBuffer + lastEntryOffset, prevAlignedSize);
    }

    PCHAR entryBuffer = baseBuffer + entryOffset;
    DokanWriteDirectoryEntry(fileInfoClass, entryBuffer, findData,
                             logicalIndex + 1, nameBytes, nameBytes,
                             IoEvent->DokanInstance);

    lastEntryOffset = entryOffset;
    lastUnalignedSize = unalignedSize;
    usedBytes = entryOffset + unalignedSize;
    entryCount++;
    nextIndex = logicalIndex + 1;

    if (IoEvent->EventContext->Flags & SL_RETURN_SINGLE_ENTRY) {
      DbgPrint("  =>return single entry\n");
      break;
    }
    DbgPrint("  =>return\n");
  }

  IoEvent->EventResult->BufferLength = usedBytes;
  if (entryCount == 0) {
    return startIndex == 0 ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
  }
  *NextIndex = nextIndex;
  return STATUS_SUCCESS;
}

static BOOL AddMissingCurrentAndParentFolder(PDOKAN_IO_EVENT IoEvent,
                                             PDOKAN_VECTOR DirList,
                                             BOOL FilterPattern) {
  BOOLEAN currentFolder = FALSE;
  BOOLEAN parentFolder = FALSE;
  DOKAN_FIND_DATA findData;
  FILETIME systime;

  assert(DirList);

  if (wcscmp(IoEvent->EventContext->Operation.Directory.DirectoryName, L"\\") ==
      0) {
    return TRUE;
  }

  BOOL matchCurrent = TRUE;
  BOOL matchParent = TRUE;
  if (FilterPattern) {
    PWCHAR pattern = GetDirectorySearchPattern(IoEvent);
    BOOL caseSensitive = (IoEvent->DokanInstance->DokanOptions->Options &
                          DOKAN_OPTION_CASE_SENSITIVE) != 0;
    matchCurrent = (!pattern || wcscmp(pattern, L"*") == 0 ||
                    DokanIsNameInExpression(pattern, L".", !caseSensitive));
    matchParent = (!pattern || wcscmp(pattern, L"*") == 0 ||
                   DokanIsNameInExpression(pattern, L"..", !caseSensitive));
    if (!matchCurrent && !matchParent) {
      return TRUE;
    }
  }

  for (size_t i = 0;
       (!currentFolder || !parentFolder) && i < DokanVector_GetCount(DirList);
       ++i) {
    const DOKAN_FIND_DATA *find =
        (const DOKAN_FIND_DATA *)DokanVector_GetItem(DirList, i);
    if (find->FileName[0] == L'.' && find->FileName[1] == L'\0') {
      currentFolder = TRUE;
    }

    if (find->FileName[0] == L'.' && find->FileName[1] == L'.' &&
        find->FileName[2] == L'\0') {
      parentFolder = TRUE;
    }
  }

  BOOL addCurrent = matchCurrent && !currentFolder;
  BOOL addParent = matchParent && !parentFolder;
  if (addCurrent || addParent) {
    DOKAN_FIND_DATA missingItems[2];
    ULONG missingCount = 0;
    LARGE_INTEGER sysLi;

    ZeroMemory(&findData, sizeof(DOKAN_FIND_DATA));
    findData.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    // Folders times should ideally be the real current and parent folder times.
    GetSystemTimeAsFileTime(&systime);
    sysLi.LowPart = systime.dwLowDateTime;
    sysLi.HighPart = (LONG)systime.dwHighDateTime;
    findData.CreationTime = sysLi;
    findData.LastAccessTime = sysLi;
    findData.LastWriteTime = sysLi;
    findData.ChangeTime = sysLi;
    if (addCurrent) {
      findData.FileName[0] = L'.';
      findData.FileName[1] = L'\0';
      missingItems[missingCount++] = findData;
    }
    if (addParent) {
      findData.FileName[0] = L'.';
      findData.FileName[1] = L'.';
      findData.FileName[2] = L'\0';
      missingItems[missingCount++] = findData;
    }
    if (missingCount > 0) {
      return DokanVector_PushFrontArray(DirList, missingItems, missingCount);
    }
  }
  return TRUE;
}

static BOOL IsDotOrDotDot(LPCWSTR Name) {
  return Name[0] == L'.' &&
         (Name[1] == L'\0' || (Name[1] == L'.' && Name[2] == L'\0'));
}

// Drop '.' and '..' emitted during continuation chunks so streaming filesystems
// cannot duplicate synthetic '.' / '..' entries injected on the initial chunk.
static VOID StripDotAndDotDotEntries(PDOKAN_VECTOR DirList) {
  size_t count = DokanVector_GetCount(DirList);
  size_t writeIdx = 0;
  for (size_t readIdx = 0; readIdx < count; ++readIdx) {
    PDOKAN_FIND_DATA entry =
        (PDOKAN_FIND_DATA)DokanVector_GetItem(DirList, readIdx);
    if (!IsDotOrDotDot(entry->FileName)) {
      if (writeIdx != readIdx) {
        PDOKAN_FIND_DATA dst =
            (PDOKAN_FIND_DATA)DokanVector_GetItem(DirList, writeIdx);
        *dst = *entry;
      }
      ++writeIdx;
    }
  }
  if (writeIdx < count) {
    DokanVector_PopBackArray(DirList, count - writeIdx);
  }
}

static VOID FilterDirectoryList(PDOKAN_IO_EVENT IoEvent,
                                PDOKAN_VECTOR DirList) {
  PWCHAR pattern = GetDirectorySearchPattern(IoEvent);
  if (!pattern || wcscmp(pattern, L"*") == 0) {
    return;
  }

  BOOL caseSensitive = (IoEvent->DokanInstance->DokanOptions->Options &
                        DOKAN_OPTION_CASE_SENSITIVE) != 0;
  size_t count = DokanVector_GetCount(DirList);
  size_t writeIdx = 0;
  for (size_t readIdx = 0; readIdx < count; ++readIdx) {
    PDOKAN_FIND_DATA entry =
        (PDOKAN_FIND_DATA)DokanVector_GetItem(DirList, readIdx);
    // Keep unterminated names so MatchFiles rejects them with
    // STATUS_INTERNAL_ERROR rather than reading out of bounds here.
    if (wcsnlen(entry->FileName, MAX_PATH) >= MAX_PATH ||
        DokanIsNameInExpression(pattern, entry->FileName, !caseSensitive)) {
      if (writeIdx != readIdx) {
        PDOKAN_FIND_DATA dst =
            (PDOKAN_FIND_DATA)DokanVector_GetItem(DirList, writeIdx);
        *dst = *entry;
      }
      ++writeIdx;
    }
  }
  if (writeIdx < count) {
    DokanVector_PopBackArray(DirList, count - writeIdx);
  }
}

static NTSTATUS WriteDirectoryResults(PDOKAN_IO_EVENT EventInfo,
                                      PDOKAN_VECTOR dirList, ULONG BaseIndex) {
  // If this function is called then so far everything should be good
  assert(EventInfo->EventResult->Status == STATUS_SUCCESS);
  // Write the file info to the output buffer
  ULONG nextIndex = EventInfo->EventContext->Operation.Directory.FileIndex;
  NTSTATUS status = MatchFiles(EventInfo, dirList, BaseIndex, &nextIndex);
  DbgPrint("WriteDirectoryResults() status = %lx, new directory index = %lu.\n",
           status, nextIndex);
  EventInfo->EventResult->Status = status;
  EventInfo->EventResult->Operation.Directory.Index = nextIndex;
  return status;
}

static NTSTATUS InvokeFindFilesChunk(PDOKAN_IO_EVENT IoEvent,
                                     PDOKAN_OPEN_INFO OpenInfo,
                                     LPCWSTR SearchPattern, ULONG FindFlags,
                                     PBOOLEAN UnimplementedFindFilesWithPattern,
                                     PBOOL PatternFilterNeeded) {
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  *PatternFilterNeeded = FALSE;

  // Reminder: FindFilesWithPattern may not be implemented by returning
  // STATUS_NOT_IMPLEMENTED.
  if (!*UnimplementedFindFilesWithPattern &&
      IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern) {
    status = IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern(
        IoEvent->EventContext->Operation.Directory.DirectoryName,
        SearchPattern ? SearchPattern : L"*", DokanFillFileData, FindFlags,
        &IoEvent->DokanFileInfo);
    if (status == STATUS_NOT_IMPLEMENTED) {
      *UnimplementedFindFilesWithPattern = TRUE;
      EnterCriticalSection(&OpenInfo->CriticalSection);
      OpenInfo->UnimplementedFindFilesWithPattern = TRUE;
      LeaveCriticalSection(&OpenInfo->CriticalSection);
    }
  }

  // And if not, try with FindFiles.
  if (status == STATUS_NOT_IMPLEMENTED &&
      IoEvent->DokanInstance->DokanOperations->FindFiles) {
    *PatternFilterNeeded = TRUE;
    ULONG findFilesFlags = FindFlags;
    if (SearchPattern != NULL && wcscmp(SearchPattern, L"*") != 0) {
      findFilesFlags &= ~(ULONG)DOKAN_FIND_SINGLE_ENTRY;
    }
    status = IoEvent->DokanInstance->DokanOperations->FindFiles(
        IoEvent->EventContext->Operation.Directory.DirectoryName,
        DokanFillFileData, findFilesFlags, &IoEvent->DokanFileInfo);
  }

  return status;
}

// Executed with openInfo->DirScanLock held (lock order: DirScanLock ->
// CriticalSection). Never holds CriticalSection across filesystem callbacks.
static NTSTATUS DispatchDirectoryInformationLocked(PDOKAN_IO_EVENT IoEvent,
                                                   PDOKAN_OPEN_INFO openInfo,
                                                   PWCHAR searchPattern) {
  ULONG startIndex = IoEvent->EventContext->Operation.Directory.FileIndex;
  NTSTATUS status = STATUS_SUCCESS;
  BOOL forceScan = FALSE;
  BOOL needMoreChunks = FALSE;
  PDOKAN_VECTOR dirList = NULL;
  ULONG baseIndex = 0;
  BOOLEAN unimplementedFindFilesWithPattern = FALSE;

  EnterCriticalSection(&openInfo->CriticalSection);
  {
    if (openInfo->DirList == NULL) {
      forceScan = TRUE;
    } else if (searchPattern && openInfo->DirListSearchPattern) {
      forceScan = wcscmp(searchPattern, openInfo->DirListSearchPattern) != 0
                      ? TRUE
                      : FALSE;
    } else if (searchPattern) {
      forceScan = wcscmp(searchPattern, L"*") != 0 ? TRUE : FALSE;
    } else if (openInfo->DirListSearchPattern) {
      forceScan =
          wcscmp(openInfo->DirListSearchPattern, L"*") != 0 ? TRUE : FALSE;
    }
    // In FastFat SL_INDEX_SPECIFIED overrides SL_RESTART_SCAN
    forceScan =
        (forceScan || (!(IoEvent->EventContext->Flags & SL_INDEX_SPECIFIED) &&
                       (IoEvent->EventContext->Flags & SL_RESTART_SCAN)))
            ? TRUE
            : FALSE;
    if (!forceScan && startIndex < openInfo->BaseIndex) {
      // Requested entry precedes the current chunk window; restart the scan.
      forceScan = TRUE;
    }

    if (!forceScan) {
      size_t cachedCount = DokanVector_GetCount(openInfo->DirList);
      if (openInfo->MoreItems &&
          (uint64_t)openInfo->BaseIndex + (uint64_t)cachedCount <=
              (uint64_t)startIndex) {
        if (cachedCount > (size_t)(ULONG_MAX - openInfo->BaseIndex)) {
          PDOKAN_VECTOR overflowList = openInfo->DirList;
          openInfo->DirList = NULL;
          openInfo->MoreItems = FALSE;
          openInfo->BaseIndex = 0;
          LeaveCriticalSection(&openInfo->CriticalSection);
          PushDirectoryList(overflowList);
          IoEvent->EventResult->BufferLength = 0;
          return STATUS_INTEGER_OVERFLOW;
        }
        // Pull the next chunk(s) from the filesystem outside CriticalSection.
        needMoreChunks = TRUE;
        dirList = openInfo->DirList;
        openInfo->DirList = NULL;
        baseIndex = openInfo->BaseIndex + (ULONG)cachedCount;
        if (cachedCount > 0) {
          DokanVector_Clear(dirList);
        }
        unimplementedFindFilesWithPattern =
            openInfo->UnimplementedFindFilesWithPattern;
      } else {
        status = WriteDirectoryResults(IoEvent, openInfo->DirList,
                                       openInfo->BaseIndex);
      }
    } else {
      unimplementedFindFilesWithPattern =
          openInfo->UnimplementedFindFilesWithPattern;
    }
  }
  LeaveCriticalSection(&openInfo->CriticalSection);

  if (!forceScan && !needMoreChunks) {
    return status;
  }

  if (dirList == NULL) {
    dirList = PopDirectoryList();
    if (!dirList) {
      DbgPrint(
          "Dokan Error: Failed to allocate memory for a new directory list.\n");
      PDOKAN_VECTOR oldDirList = NULL;
      EnterCriticalSection(&openInfo->CriticalSection);
      oldDirList = openInfo->DirList;
      openInfo->DirList = NULL;
      openInfo->MoreItems = FALSE;
      openInfo->BaseIndex = 0;
      LeaveCriticalSection(&openInfo->CriticalSection);
      if (oldDirList) {
        PushDirectoryList(oldDirList);
      }
      IoEvent->EventResult->BufferLength = 0;
      return STATUS_NO_MEMORY;
    }
  }

  DOKAN_FILL_FIND_CONTEXT fillContext;
  fillContext.DirList = dirList;
  fillContext.AllocationFailed = FALSE;
  IoEvent->DokanFileInfo.ProcessingContext = &fillContext;

  ULONG findFlags = 0;
  if (IoEvent->EventContext->Flags & SL_RETURN_SINGLE_ENTRY) {
    findFlags |= DOKAN_FIND_SINGLE_ENTRY;
  }
  if (forceScan) {
    findFlags |= DOKAN_FIND_RESTART_SCAN;
  }

  BOOL isInitialChunk = forceScan;
  BOOLEAN moreItems = FALSE;
  NTSTATUS fsStatus = STATUS_NOT_IMPLEMENTED;

  for (;;) {
    BOOL patternFilterNeeded = FALSE;
    fsStatus = InvokeFindFilesChunk(IoEvent, openInfo, searchPattern, findFlags,
                                    &unimplementedFindFilesWithPattern,
                                    &patternFilterNeeded);

    if (fsStatus == STATUS_PENDING) {
      DbgPrint("Dokan Error: DispatchDirectoryInformation() failed because "
               "STATUS_PENDING was supplied for ResultStatus.\n");
      fsStatus = STATUS_INTERNAL_ERROR;
      break;
    }

    if (fsStatus != STATUS_SUCCESS && fsStatus != STATUS_MORE_ENTRIES) {
      break;
    }

    if (fillContext.AllocationFailed) {
      fsStatus = STATUS_NO_MEMORY;
      break;
    }

    moreItems = (fsStatus == STATUS_MORE_ENTRIES) ? TRUE : FALSE;

    if (isInitialChunk) {
      // When FilterDirectoryList runs next, defer wildcard matching of '.'/'..'
      // to it so they are evaluated once rather than twice.
      if (!AddMissingCurrentAndParentFolder(IoEvent, dirList,
                                            !patternFilterNeeded)) {
        fsStatus = STATUS_NO_MEMORY;
        break;
      }
      isInitialChunk = FALSE;
    } else {
      StripDotAndDotDotEntries(dirList);
    }
    if (patternFilterNeeded) {
      FilterDirectoryList(IoEvent, dirList);
    }

    size_t chunkCount = DokanVector_GetCount(dirList);
    if (!moreItems ||
        (uint64_t)baseIndex + (uint64_t)chunkCount > (uint64_t)startIndex) {
      break;
    }

    // Filtered chunk produced no entries at or past startIndex while the
    // filesystem still has more chunks; advance logical BaseIndex and continue.
    if (chunkCount <= (size_t)(ULONG_MAX - baseIndex)) {
      baseIndex += (ULONG)chunkCount;
    } else {
      fsStatus = STATUS_INTEGER_OVERFLOW;
      break;
    }
    DokanVector_Clear(dirList);
    findFlags &= ~(ULONG)DOKAN_FIND_RESTART_SCAN;
  }

  if (fsStatus == STATUS_SUCCESS || fsStatus == STATUS_MORE_ENTRIES) {
    status = WriteDirectoryResults(IoEvent, dirList, baseIndex);

    PDOKAN_VECTOR oldDirList = NULL;
    EnterCriticalSection(&openInfo->CriticalSection);
    {
      oldDirList = openInfo->DirList;
      openInfo->DirList = dirList;
      openInfo->BaseIndex = baseIndex;
      openInfo->MoreItems = moreItems;
      if (forceScan) {
        if (openInfo->DirListSearchPattern) {
          free(openInfo->DirListSearchPattern);
          openInfo->DirListSearchPattern = NULL;
        }
        if (searchPattern != NULL) {
          openInfo->DirListSearchPattern = _wcsdup(searchPattern);
        }
      }
    }
    LeaveCriticalSection(&openInfo->CriticalSection);
    if (oldDirList) {
      PushDirectoryList(oldDirList);
    }
  } else {
    PDOKAN_VECTOR oldDirList = NULL;
    EnterCriticalSection(&openInfo->CriticalSection);
    oldDirList = openInfo->DirList;
    openInfo->DirList = NULL;
    openInfo->MoreItems = FALSE;
    openInfo->BaseIndex = 0;
    LeaveCriticalSection(&openInfo->CriticalSection);
    if (oldDirList) {
      PushDirectoryList(oldDirList);
    }
    PushDirectoryList(dirList);
    status = fsStatus;
  }

  IoEvent->DokanFileInfo.ProcessingContext = NULL;
  return status;
}

VOID DispatchDirectoryInformation(PDOKAN_IO_EVENT IoEvent) {
  ULONG fileInfoClass =
      IoEvent->EventContext->Operation.Directory.FileInformationClass;
  ULONG startIndex = IoEvent->EventContext->Operation.Directory.FileIndex;
  PDOKAN_OPEN_INFO openInfo = IoEvent->DokanOpenInfo;
  BOOLEAN allocatedOpenInfo = FALSE;

  DbgPrint(
      "###FindFiles file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
      IoEvent->DokanOpenInfo,
      IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId : -1,
      IoEvent);

  CheckFileName(IoEvent->EventContext->Operation.Directory.DirectoryName);

  CreateDispatchCommon(IoEvent,
                       IoEvent->EventContext->Operation.Directory.BufferLength,
                       /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  if (IoEvent->DokanOpenInfo != NULL && !IoEvent->DokanFileInfo.IsDirectory) {
    DbgPrint("Dokan Information: Directory enumeration on non-directory\n");
    IoEvent->EventResult->BufferLength = 0;
    IoEvent->EventResult->Status = STATUS_INVALID_PARAMETER;
    EventCompletion(IoEvent);
    return;
  }

  // check whether this is a handled FileInfoClass
  if (GetDirectoryInfoHeaderSize(fileInfoClass) == 0) {
    DbgPrint("Dokan Information: Unsupported file information class %d\n",
             fileInfoClass);
    // send directory info to driver
    IoEvent->EventResult->BufferLength = 0;
    IoEvent->EventResult->Status = STATUS_INVALID_PARAMETER;
    EventCompletion(IoEvent);
    return;
  }

  IoEvent->EventResult->Operation.Directory.Index = startIndex;

  PWCHAR searchPattern = GetDirectorySearchPattern(IoEvent);

  if (!openInfo) {
    openInfo = PopFileOpenInfo();
    if (!openInfo) {
      IoEvent->EventResult->BufferLength = 0;
      IoEvent->EventResult->Status = STATUS_NO_MEMORY;
      EventCompletion(IoEvent);
      return;
    }
    allocatedOpenInfo = TRUE;
  }

  EnterCriticalSection(&openInfo->DirScanLock);
  NTSTATUS status =
      DispatchDirectoryInformationLocked(IoEvent, openInfo, searchPattern);
  LeaveCriticalSection(&openInfo->DirScanLock);

  IoEvent->EventResult->Status = status;
  EventCompletion(IoEvent);

  if (allocatedOpenInfo) {
    PushFileOpenInfo(openInfo);
  }
}

typedef WCHAR(NTAPI *PFN_RTL_UPCASE_UNICODE_CHAR)(WCHAR SourceCharacter);

static WCHAR NTAPI DokanFallbackUpcaseUnicodeChar(WCHAR SourceCharacter) {
  WCHAR out = SourceCharacter;
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, &SourceCharacter, 1,
                    &out, 1, NULL, NULL, 0) == 0) {
    return SourceCharacter;
  }
  return out;
}

// Resolve RtlUpcaseUnicodeChar dynamically from ntdll.dll (since dokan3.dll
// does not link ntdll.lib) so case-insensitive matching uses the OS Unicode
// case-folding table per [MS-FSA 2.1.4.4], with an invariant-locale
// LCMapStringEx fallback if ntdll export lookup fails.
static PFN_RTL_UPCASE_UNICODE_CHAR DokanGetUpcaseUnicodeCharFn(VOID) {
  static INIT_ONCE initOnce = INIT_ONCE_STATIC_INIT;
  static PFN_RTL_UPCASE_UNICODE_CHAR upcaseFn = DokanFallbackUpcaseUnicodeChar;

  BOOL pending = FALSE;
  if (InitOnceBeginInitialize(&initOnce, 0, &pending, NULL) && pending) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != NULL) {
      FARPROC proc = GetProcAddress(ntdll, "RtlUpcaseUnicodeChar");
      if (proc != NULL) {
        memcpy(&upcaseFn, &proc, sizeof(upcaseFn));
      }
    }
    InitOnceComplete(&initOnce, 0, NULL);
  }
  return upcaseFn;
}

// Match Name against Expression per [MS-FSA 2.1.4.4] ("Algorithm for
// Determining if a File Name Is in an Expression").
//
// Supported wildcards:
//   '*'          Matches zero or more characters (including '.').
//   '?'          Matches any single character (including '.').
//   DOS_STAR '<' Matches zero or more characters without consuming the final
//                '.' in Name.
//   DOS_QM   '>' Matches one non-'.' character, or zero characters when
//                positioned at '.' or end-of-name.
//   DOS_DOT  '"' Matches a literal '.', or zero characters at end-of-name.
//
// When IgnoreCase is TRUE, characters are case-folded via dynamically resolved
// RtlUpcaseUnicodeChar (with an invariant-locale LCMapStringEx fallback) to
// match the OS Unicode case-folding table.
//
// Wildcard expressions use an iterative 2-row DP table (O(exprLen * nameLen)
// time, O(nameLen) space) to avoid exponential backtracking on repeated
// wildcards. Returns FALSE if heap allocation fails for names longer than
// DOKAN_MATCH_STACK_LEN.
BOOL DOKANAPI
DokanIsNameInExpression(_In_opt_ LPCWSTR Expression, // matching pattern
                        _In_opt_ LPCWSTR Name,       // file name
                        _In_ BOOL IgnoreCase) {
  if (Expression == NULL || Name == NULL) {
    return FALSE;
  }

  size_t exprLen = wcslen(Expression);
  size_t nameLen = wcslen(Name);

  if (exprLen == 0) {
    return nameLen == 0;
  }
  if (exprLen == 1 && Expression[0] == L'*') {
    return TRUE;
  }

  BOOL hasWildcard = FALSE;
  BOOL hasDosStar = FALSE;
  for (size_t i = 0; i < exprLen; ++i) {
    WCHAR ec = Expression[i];
    if (ec == L'*' || ec == L'?' || ec == DOS_QM || ec == DOS_DOT) {
      hasWildcard = TRUE;
    } else if (ec == DOS_STAR) {
      hasWildcard = TRUE;
      hasDosStar = TRUE;
    }
  }

  PFN_RTL_UPCASE_UNICODE_CHAR upcaseFn =
      IgnoreCase ? DokanGetUpcaseUnicodeCharFn() : NULL;

  if (!hasWildcard) {
    if (exprLen != nameLen) {
      return FALSE;
    }
    if (!IgnoreCase) {
      return wmemcmp(Expression, Name, nameLen) == 0 ? TRUE : FALSE;
    }
    for (size_t i = 0; i < nameLen; ++i) {
      if (upcaseFn(Expression[i]) != upcaseFn(Name[i])) {
        return FALSE;
      }
    }
    return TRUE;
  }

  // DOS_STAR ('<') cannot consume the last '.' in Name; only scan when needed.
  size_t lastDot = SIZE_MAX;
  if (hasDosStar) {
    for (size_t j = 0; j < nameLen; ++j) {
      if (Name[j] == L'.') {
        lastDot = j;
      }
    }
  }

  size_t rowSize = nameLen + 1;
  UCHAR stackBuf[2 * (DOKAN_MATCH_STACK_LEN + 1)];
  UCHAR *heapBuf = NULL;
  UCHAR *prev;
  UCHAR *curr;

  if (nameLen <= DOKAN_MATCH_STACK_LEN) {
    prev = stackBuf;
    curr = stackBuf + rowSize;
  } else {
    if (rowSize > (SIZE_MAX / 2)) {
      return FALSE;
    }
    heapBuf = (UCHAR *)malloc(2 * rowSize);
    if (heapBuf == NULL) {
      return FALSE;
    }
    prev = heapBuf;
    curr = heapBuf + rowSize;
  }

  BOOL matched = FALSE;
  WCHAR upNameStack[DOKAN_MATCH_STACK_LEN];
  WCHAR *upNameHeap = NULL;
  const WCHAR *matchName = Name;
  if (IgnoreCase && nameLen > 0) {
    WCHAR *upName;
    if (nameLen <= DOKAN_MATCH_STACK_LEN) {
      upName = upNameStack;
    } else {
      if (nameLen > (SIZE_MAX / sizeof(WCHAR))) {
        goto cleanup;
      }
      upNameHeap = (WCHAR *)malloc(nameLen * sizeof(WCHAR));
      if (upNameHeap == NULL) {
        goto cleanup;
      }
      upName = upNameHeap;
    }
    for (size_t j = 0; j < nameLen; ++j) {
      upName[j] = upcaseFn(Name[j]);
    }
    matchName = upName;
  }

  // Invariant: prev[j] == 1 iff Expression[0..i-1] matches Name[0..j-1].
  memset(prev, 0, rowSize);
  prev[0] = 1;

  for (size_t i = 0; i < exprLen; ++i) {
    WCHAR ec = Expression[i];
    memset(curr, 0, rowSize);
    BOOL anyReachable = FALSE;

    if (ec == L'*') {
      // '*': matches zero or more characters (including '.').
      for (size_t j = 0; j <= nameLen; ++j) {
        if (prev[j] || (j > 0 && curr[j - 1])) {
          curr[j] = 1;
          anyReachable = TRUE;
        }
      }
    } else if (ec == DOS_STAR) {
      // '<' (DOS_STAR): matches zero or more characters without consuming the
      // last '.' in Name (stops advancing at j == lastDot).
      for (size_t j = 0; j <= nameLen; ++j) {
        if (prev[j]) {
          curr[j] = 1;
        }
        if (curr[j]) {
          anyReachable = TRUE;
          if (j < nameLen && j != lastDot) {
            curr[j + 1] = 1;
          }
        }
      }
    } else if (ec == DOS_QM) {
      // '>' (DOS_QM): matches one non-'.' character, or zero characters at '.'
      // or end-of-name.
      for (size_t j = 0; j <= nameLen; ++j) {
        if (!prev[j]) {
          continue;
        }
        if (j == nameLen || Name[j] == L'.') {
          curr[j] = 1;
        } else {
          curr[j + 1] = 1;
        }
        anyReachable = TRUE;
      }
    } else if (ec == DOS_DOT) {
      // '"' (DOS_DOT): matches a literal '.', or zero characters at
      // end-of-name.
      for (size_t j = 0; j <= nameLen; ++j) {
        if (!prev[j]) {
          continue;
        }
        if (j == nameLen) {
          curr[j] = 1;
          anyReachable = TRUE;
        } else if (Name[j] == L'.') {
          curr[j + 1] = 1;
          anyReachable = TRUE;
        }
      }
    } else if (ec == L'?') {
      // '?': matches any single character (including '.').
      for (size_t j = 0; j < nameLen; ++j) {
        if (prev[j]) {
          curr[j + 1] = 1;
          anyReachable = TRUE;
        }
      }
    } else {
      // Literal character match (case-folded when IgnoreCase is TRUE).
      WCHAR target = IgnoreCase ? upcaseFn(ec) : ec;
      for (size_t j = 0; j < nameLen; ++j) {
        if (!prev[j]) {
          continue;
        }
        if (matchName[j] == target) {
          curr[j + 1] = 1;
          anyReachable = TRUE;
        }
      }
    }

    UCHAR *tmp = prev;
    prev = curr;
    curr = tmp;

    if (!anyReachable) {
      goto cleanup;
    }
  }

  matched = prev[nameLen] != 0;

cleanup:
  if (upNameHeap != NULL) {
    free(upNameHeap);
  }
  if (heapBuf != NULL) {
    free(heapBuf);
  }
  return matched;
}
