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
    PWIN32_FIND_DATAW FindData, PDOKAN_INSTANCE DokanInstance,
    PLARGE_INTEGER CreationTime, PLARGE_INTEGER LastAccessTime,
    PLARGE_INTEGER LastWriteTime, PLARGE_INTEGER ChangeTime,
    PLARGE_INTEGER EndOfFile, PLARGE_INTEGER AllocationSize) {
  EndOfFile->HighPart = FindData->nFileSizeHigh;
  EndOfFile->LowPart = FindData->nFileSizeLow;
  AllocationSize->HighPart = FindData->nFileSizeHigh;
  AllocationSize->LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(AllocationSize, DokanInstance->DokanOptions);

  CreationTime->HighPart = FindData->ftCreationTime.dwHighDateTime;
  CreationTime->LowPart = FindData->ftCreationTime.dwLowDateTime;
  LastAccessTime->HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  LastAccessTime->LowPart = FindData->ftLastAccessTime.dwLowDateTime;
  LastWriteTime->HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  LastWriteTime->LowPart = FindData->ftLastWriteTime.dwLowDateTime;
  ChangeTime->HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  ChangeTime->LowPart = FindData->ftLastWriteTime.dwLowDateTime;
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
// the directory entry. NextEntryOffset and optional per-class fields not
// carried by WIN32_FIND_DATAW (EaSize, ShortNameLength, ShortName,
// ReparsePointTag, and FileId) remain zero.
static VOID DokanWriteDirectoryEntry(FILE_INFORMATION_CLASS DirectoryInfo,
                                     PVOID Buffer, PWIN32_FIND_DATAW FindData,
                                     ULONG Index, ULONG NameBytes,
                                     ULONG CopyNameBytes,
                                     PDOKAN_INSTANCE DokanInstance) {
  ULONG headerBytes = GetDirectoryInfoHeaderSize(DirectoryInfo);
  if (headerBytes == 0) {
    return;
  }
  RtlZeroMemory(Buffer, headerBytes + CopyNameBytes);

#define DOKAN_FILL_DIR_CASE(InfoClass, StructType)                             \
  case InfoClass: {                                                            \
    StructType *entry = (StructType *)Buffer;                                  \
    entry->FileIndex = Index;                                                  \
    entry->FileAttributes = FindData->dwFileAttributes;                        \
    entry->FileNameLength = NameBytes;                                         \
    DokanFillDirTimesAndSizes(FindData, DokanInstance, &entry->CreationTime,   \
                              &entry->LastAccessTime, &entry->LastWriteTime,   \
                              &entry->ChangeTime, &entry->EndOfFile,           \
                              &entry->AllocationSize);                         \
    if (CopyNameBytes > 0) {                                                   \
      RtlCopyMemory(entry->FileName, FindData->cFileName, CopyNameBytes);      \
    }                                                                          \
    break;                                                                     \
  }

  switch (DirectoryInfo) {
    DOKAN_FILL_DIR_CASE(FileDirectoryInformation, FILE_DIRECTORY_INFORMATION)
    DOKAN_FILL_DIR_CASE(FileFullDirectoryInformation, FILE_FULL_DIR_INFORMATION)
    DOKAN_FILL_DIR_CASE(FileIdFullDirectoryInformation,
                        FILE_ID_FULL_DIR_INFORMATION)
    DOKAN_FILL_DIR_CASE(FileBothDirectoryInformation, FILE_BOTH_DIR_INFORMATION)
    DOKAN_FILL_DIR_CASE(FileIdBothDirectoryInformation,
                        FILE_ID_BOTH_DIR_INFORMATION)
    DOKAN_FILL_DIR_CASE(FileIdExtdDirectoryInformation, FILE_ID_EXTD_DIR_INFO)
    DOKAN_FILL_DIR_CASE(FileIdExtdBothDirectoryInformation,
                        FILE_ID_EXTD_BOTH_DIR_INFORMATION)
  case FileNamesInformation: {
    PFILE_NAMES_INFORMATION entry = (PFILE_NAMES_INFORMATION)Buffer;
    entry->FileIndex = Index;
    entry->FileNameLength = NameBytes;
    if (CopyNameBytes > 0) {
      RtlCopyMemory(entry->FileName, FindData->cFileName, CopyNameBytes);
    }
    break;
  }
  default:
    break;
  }

#undef DOKAN_FILL_DIR_CASE
}

int WINAPI DokanFillFileData(PWIN32_FIND_DATAW FindData,
                             PDOKAN_FILE_INFO FileInfo) {
  assert(FileInfo->ProcessingContext);
  PDOKAN_VECTOR dirList = (PDOKAN_VECTOR)FileInfo->ProcessingContext;
  DokanVector_PushBack(dirList, FindData);
  return 0;
}

// Write entries from the already-filtered DirList starting at FileIndex into
// EventResult->Buffer. On STATUS_SUCCESS, *NextIndex receives the next 0-based
// FileIndex cursor; on error or overflow, *NextIndex stays at FileIndex.
static NTSTATUS MatchFiles(PDOKAN_IO_EVENT IoEvent, PDOKAN_VECTOR DirList,
                           PULONG NextIndex) {
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

  PCHAR baseBuffer = (PCHAR)IoEvent->EventResult->Buffer;
  size_t count = DokanVector_GetCount(DirList);
  ULONG usedBytes = 0;
  ULONG lastEntryOffset = 0;
  ULONG lastUnalignedSize = 0;
  ULONG entryCount = 0;
  ULONG nextIndex = startIndex;

  for (size_t i = startIndex; i < count; ++i) {
    PWIN32_FIND_DATAW findData =
        (PWIN32_FIND_DATAW)DokanVector_GetItem(DirList, i);
    ULONG nameBytes = (ULONG)wcslen(findData->cFileName) * sizeof(WCHAR);
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
                                 (ULONG)(i + 1), nameBytes, avail,
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
                             (ULONG)(i + 1), nameBytes, nameBytes,
                             IoEvent->DokanInstance);

    lastEntryOffset = entryOffset;
    lastUnalignedSize = unalignedSize;
    usedBytes = entryOffset + unalignedSize;
    entryCount++;
    nextIndex = (ULONG)(i + 1);

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

static VOID AddMissingCurrentAndParentFolder(PDOKAN_IO_EVENT IoEvent,
                                             BOOL FilterPattern) {
  BOOLEAN currentFolder = FALSE;
  BOOLEAN parentFolder = FALSE;
  WIN32_FIND_DATAW findData;
  FILETIME systime;
  PDOKAN_VECTOR dirList =
      (PDOKAN_VECTOR)IoEvent->DokanFileInfo.ProcessingContext;

  assert(dirList);

  if (wcscmp(IoEvent->EventContext->Operation.Directory.DirectoryName, L"\\") ==
      0) {
    return;
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
      return;
    }
  }

  for (size_t i = 0;
       (!currentFolder || !parentFolder) && i < DokanVector_GetCount(dirList);
       ++i) {
    PWIN32_FIND_DATAW find = (PWIN32_FIND_DATAW)DokanVector_GetItem(dirList, i);
    if (wcscmp(find->cFileName, L".") == 0) {
      currentFolder = TRUE;
    }

    if (wcscmp(find->cFileName, L"..") == 0) {
      parentFolder = TRUE;
    }
  }

  BOOL addCurrent = matchCurrent && !currentFolder;
  BOOL addParent = matchParent && !parentFolder;
  if (addCurrent || addParent) {
    WIN32_FIND_DATAW missingItems[2];
    ULONG missingCount = 0;

    ZeroMemory(&findData, sizeof(WIN32_FIND_DATAW));
    findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    // Folders times should ideally be the real current and parent folder times.
    GetSystemTimeAsFileTime(&systime);
    findData.ftCreationTime = systime;
    findData.ftLastAccessTime = systime;
    findData.ftLastWriteTime = systime;
    if (addCurrent) {
      findData.cFileName[0] = L'.';
      findData.cFileName[1] = L'\0';
      missingItems[missingCount++] = findData;
    }
    if (addParent) {
      findData.cFileName[0] = L'.';
      findData.cFileName[1] = L'.';
      findData.cFileName[2] = L'\0';
      missingItems[missingCount++] = findData;
    }
    if (missingCount > 0) {
      DokanVector_PushFrontArray(dirList, missingItems, missingCount);
    }
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
    PWIN32_FIND_DATAW entry =
        (PWIN32_FIND_DATAW)DokanVector_GetItem(DirList, readIdx);
    if (DokanIsNameInExpression(pattern, entry->cFileName, !caseSensitive)) {
      if (writeIdx != readIdx) {
        PWIN32_FIND_DATAW dst =
            (PWIN32_FIND_DATAW)DokanVector_GetItem(DirList, writeIdx);
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
                                      PDOKAN_VECTOR dirList) {
  // If this function is called then so far everything should be good
  assert(EventInfo->EventResult->Status == STATUS_SUCCESS);
  // Write the file info to the output buffer
  ULONG nextIndex = EventInfo->EventContext->Operation.Directory.FileIndex;
  NTSTATUS status = MatchFiles(EventInfo, dirList, &nextIndex);
  DbgPrint("WriteDirectoryResults() status = %lx, new directory index = %lu.\n",
           status, nextIndex);
  EventInfo->EventResult->Status = status;
  EventInfo->EventResult->Operation.Directory.Index = nextIndex;
  return status;
}

static VOID EndFindFilesCommon(PDOKAN_IO_EVENT IoEvent,
                               PDOKAN_OPEN_INFO OpenInfo,
                               BOOL PatternFilterNeeded, NTSTATUS Status) {
  PDOKAN_VECTOR dirList =
      (PDOKAN_VECTOR)IoEvent->DokanFileInfo.ProcessingContext;
  PDOKAN_VECTOR oldDirList = NULL;

  assert(IoEvent->EventResult->BufferLength == 0);
  assert(IoEvent->DokanFileInfo.ProcessingContext);

  // STATUS_PENDING should not be passed to this function
  if (Status == STATUS_PENDING) {
    DbgPrint("Dokan Error: EndFindFilesCommon() failed because STATUS_PENDING "
             "was supplied for ResultStatus.\n");
    Status = STATUS_INTERNAL_ERROR;
  }

  if (Status == STATUS_SUCCESS) {
    PWCHAR pattern = GetDirectorySearchPattern(IoEvent);
    // When FilterDirectoryList runs next, defer wildcard matching of '.'/'..'
    // to it so they are evaluated once rather than twice.
    AddMissingCurrentAndParentFolder(IoEvent, !PatternFilterNeeded);
    if (PatternFilterNeeded) {
      FilterDirectoryList(IoEvent, dirList);
    }
    Status = WriteDirectoryResults(IoEvent, dirList);
    EnterCriticalSection(&OpenInfo->CriticalSection);
    {
      if (OpenInfo->DirList != dirList) {
        oldDirList = OpenInfo->DirList;
        OpenInfo->DirList = dirList;
      } else {
        // They should never point to the same object
        DbgPrint("Dokan Warning: EndFindFilesCommon() "
                 "EventInfo->DokanOpenInfo->DirList == dirList\n");
      }
      if (OpenInfo->DirListSearchPattern) {
        free(OpenInfo->DirListSearchPattern);
        OpenInfo->DirListSearchPattern = NULL;
      }
      if (pattern != NULL) {
        OpenInfo->DirListSearchPattern = _wcsdup(pattern);
      }
    }
    LeaveCriticalSection(&OpenInfo->CriticalSection);
    if (oldDirList) {
      PushDirectoryList(oldDirList);
    }
  } else {
    PushDirectoryList(dirList);
  }
  IoEvent->DokanFileInfo.ProcessingContext = NULL;
  IoEvent->EventResult->Status = Status;
  EventCompletion(IoEvent);
}

VOID DispatchDirectoryInformation(PDOKAN_IO_EVENT IoEvent) {
  PWCHAR searchPattern = NULL;
  NTSTATUS status = STATUS_SUCCESS;
  ULONG fileInfoClass =
      IoEvent->EventContext->Operation.Directory.FileInformationClass;
  BOOL forceScan = FALSE;
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

  IoEvent->EventResult->Operation.Directory.Index =
      IoEvent->EventContext->Operation.Directory.FileIndex;

  searchPattern = GetDirectorySearchPattern(IoEvent);

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
    if (!forceScan) {
      status = WriteDirectoryResults(IoEvent, openInfo->DirList);
    } else {
      unimplementedFindFilesWithPattern =
          openInfo->UnimplementedFindFilesWithPattern;
    }
  }
  LeaveCriticalSection(&openInfo->CriticalSection);

  if (!forceScan) {
    IoEvent->EventResult->Status = status;
    EventCompletion(IoEvent);
    if (allocatedOpenInfo) {
      PushFileOpenInfo(openInfo);
    }
    return;
  }

  IoEvent->DokanFileInfo.ProcessingContext = PopDirectoryList();
  if (!IoEvent->DokanFileInfo.ProcessingContext) {
    DbgPrint(
        "Dokan Error: Failed to allocate memory for a new directory list.\n");
    IoEvent->EventResult->Status = STATUS_NO_MEMORY;
    EventCompletion(IoEvent);
    if (allocatedOpenInfo) {
      PushFileOpenInfo(openInfo);
    }
    return;
  }

  status = STATUS_NOT_IMPLEMENTED;
  BOOL patternFilterNeeded = FALSE;

  // Reminder: FindFilesWithPattern may not be implemented by returning
  // STATUS_NOT_IMPLEMENTED.
  if (!unimplementedFindFilesWithPattern &&
      IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern) {
    status = IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern(
        IoEvent->EventContext->Operation.Directory.DirectoryName,
        searchPattern ? searchPattern : L"*", DokanFillFileData,
        &IoEvent->DokanFileInfo);
    if (status == STATUS_NOT_IMPLEMENTED) {
      EnterCriticalSection(&openInfo->CriticalSection);
      openInfo->UnimplementedFindFilesWithPattern = TRUE;
      LeaveCriticalSection(&openInfo->CriticalSection);
    }
  }

  // And if not, try with FindFiles.
  if (status == STATUS_NOT_IMPLEMENTED &&
      IoEvent->DokanInstance->DokanOperations->FindFiles) {
    patternFilterNeeded = TRUE;
    status = IoEvent->DokanInstance->DokanOperations->FindFiles(
        IoEvent->EventContext->Operation.Directory.DirectoryName,
        DokanFillFileData, &IoEvent->DokanFileInfo);
  }

  if (status != STATUS_NOT_IMPLEMENTED) {
    EndFindFilesCommon(IoEvent, openInfo, patternFilterNeeded, status);
  } else {
    // Neither FindFilesWithPattern nor FindFiles are implemented.
    PushDirectoryList((PDOKAN_VECTOR)IoEvent->DokanFileInfo.ProcessingContext);
    IoEvent->DokanFileInfo.ProcessingContext = NULL;
    IoEvent->EventResult->Status = STATUS_NOT_IMPLEMENTED;
    EventCompletion(IoEvent);
  }

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
BOOL DOKANAPI DokanIsNameInExpression(LPCWSTR Expression, // matching pattern
                                      LPCWSTR Name,       // file name
                                      BOOL IgnoreCase) {
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
