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
#include "list.h"
#include "dokan_pool.h"

#include <assert.h>

/**
* \struct DOKAN_FIND_DATA
* \brief Dokan find file list
*
* Used by FindFiles
*/
typedef struct _DOKAN_FIND_DATA {
  /**
  * File data information link
  */
  WIN32_FIND_DATAW FindData;
  /**
  * Current list entry informations
  */
  LIST_ENTRY ListEntry;
} DOKAN_FIND_DATA, *PDOKAN_FIND_DATA;

VOID DokanFillDirInfo(PFILE_DIRECTORY_INFORMATION Buffer,
                      PWIN32_FIND_DATAW FindData, ULONG Index,
                      PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileAttributes = FindData->dwFileAttributes;
  Buffer->FileNameLength = nameBytes;

  Buffer->EndOfFile.HighPart = FindData->nFileSizeHigh;
  Buffer->EndOfFile.LowPart = FindData->nFileSizeLow;
  Buffer->AllocationSize.HighPart = FindData->nFileSizeHigh;
  Buffer->AllocationSize.LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&Buffer->AllocationSize, DokanInstance->DokanOptions);

  Buffer->CreationTime.HighPart = FindData->ftCreationTime.dwHighDateTime;
  Buffer->CreationTime.LowPart = FindData->ftCreationTime.dwLowDateTime;

  Buffer->LastAccessTime.HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  Buffer->LastAccessTime.LowPart = FindData->ftLastAccessTime.dwLowDateTime;

  Buffer->LastWriteTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->LastWriteTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->ChangeTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->ChangeTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

VOID DokanFillFullDirInfo(PFILE_FULL_DIR_INFORMATION Buffer,
                          PWIN32_FIND_DATAW FindData, ULONG Index,
                          PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileAttributes = FindData->dwFileAttributes;
  Buffer->FileNameLength = nameBytes;

  Buffer->EndOfFile.HighPart = FindData->nFileSizeHigh;
  Buffer->EndOfFile.LowPart = FindData->nFileSizeLow;
  Buffer->AllocationSize.HighPart = FindData->nFileSizeHigh;
  Buffer->AllocationSize.LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&Buffer->AllocationSize, DokanInstance->DokanOptions);

  Buffer->CreationTime.HighPart = FindData->ftCreationTime.dwHighDateTime;
  Buffer->CreationTime.LowPart = FindData->ftCreationTime.dwLowDateTime;

  Buffer->LastAccessTime.HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  Buffer->LastAccessTime.LowPart = FindData->ftLastAccessTime.dwLowDateTime;

  Buffer->LastWriteTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->LastWriteTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->ChangeTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->ChangeTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->EaSize = 0;

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

VOID DokanFillIdFullDirInfo(PFILE_ID_FULL_DIR_INFORMATION Buffer,
                            PWIN32_FIND_DATAW FindData, ULONG Index,
                            PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileAttributes = FindData->dwFileAttributes;
  Buffer->FileNameLength = nameBytes;

  Buffer->EndOfFile.HighPart = FindData->nFileSizeHigh;
  Buffer->EndOfFile.LowPart = FindData->nFileSizeLow;
  Buffer->AllocationSize.HighPart = FindData->nFileSizeHigh;
  Buffer->AllocationSize.LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&Buffer->AllocationSize, DokanInstance->DokanOptions);

  Buffer->CreationTime.HighPart = FindData->ftCreationTime.dwHighDateTime;
  Buffer->CreationTime.LowPart = FindData->ftCreationTime.dwLowDateTime;

  Buffer->LastAccessTime.HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  Buffer->LastAccessTime.LowPart = FindData->ftLastAccessTime.dwLowDateTime;

  Buffer->LastWriteTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->LastWriteTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->ChangeTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->ChangeTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->EaSize = 0;
  Buffer->FileId.QuadPart = 0;

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

VOID DokanFillIdBothDirInfo(PFILE_ID_BOTH_DIR_INFORMATION Buffer,
                            PWIN32_FIND_DATAW FindData, ULONG Index,
                            PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileAttributes = FindData->dwFileAttributes;
  Buffer->FileNameLength = nameBytes;
  Buffer->ShortNameLength = 0;

  Buffer->EndOfFile.HighPart = FindData->nFileSizeHigh;
  Buffer->EndOfFile.LowPart = FindData->nFileSizeLow;
  Buffer->AllocationSize.HighPart = FindData->nFileSizeHigh;
  Buffer->AllocationSize.LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&Buffer->AllocationSize, DokanInstance->DokanOptions);

  Buffer->CreationTime.HighPart = FindData->ftCreationTime.dwHighDateTime;
  Buffer->CreationTime.LowPart = FindData->ftCreationTime.dwLowDateTime;

  Buffer->LastAccessTime.HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  Buffer->LastAccessTime.LowPart = FindData->ftLastAccessTime.dwLowDateTime;

  Buffer->LastWriteTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->LastWriteTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->ChangeTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->ChangeTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->EaSize = 0;
  Buffer->FileId.QuadPart = 0;

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

VOID DokanFillIdExtdDirInfo(PFILE_ID_EXTD_DIR_INFO Buffer,
                            PWIN32_FIND_DATAW FindData, ULONG Index,
                            PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileAttributes = FindData->dwFileAttributes;
  Buffer->FileNameLength = nameBytes;

  Buffer->EndOfFile.HighPart = FindData->nFileSizeHigh;
  Buffer->EndOfFile.LowPart = FindData->nFileSizeLow;
  Buffer->AllocationSize.HighPart = FindData->nFileSizeHigh;
  Buffer->AllocationSize.LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&Buffer->AllocationSize, DokanInstance->DokanOptions);

  Buffer->CreationTime.HighPart = FindData->ftCreationTime.dwHighDateTime;
  Buffer->CreationTime.LowPart = FindData->ftCreationTime.dwLowDateTime;

  Buffer->LastAccessTime.HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  Buffer->LastAccessTime.LowPart = FindData->ftLastAccessTime.dwLowDateTime;

  Buffer->LastWriteTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->LastWriteTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->ChangeTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->ChangeTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->EaSize = 0;
  Buffer->ReparsePointTag = 0;
  RtlFillMemory(&Buffer->FileId.Identifier, sizeof Buffer->FileId.Identifier, 0);

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

VOID DokanFillIdExtdBothDirInfo(PFILE_ID_EXTD_BOTH_DIR_INFORMATION Buffer,
                            PWIN32_FIND_DATAW FindData, ULONG Index,
                            PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileAttributes = FindData->dwFileAttributes;
  Buffer->FileNameLength = nameBytes;
  Buffer->ShortNameLength = 0;

  Buffer->EndOfFile.HighPart = FindData->nFileSizeHigh;
  Buffer->EndOfFile.LowPart = FindData->nFileSizeLow;
  Buffer->AllocationSize.HighPart = FindData->nFileSizeHigh;
  Buffer->AllocationSize.LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&Buffer->AllocationSize, DokanInstance->DokanOptions);

  Buffer->CreationTime.HighPart = FindData->ftCreationTime.dwHighDateTime;
  Buffer->CreationTime.LowPart = FindData->ftCreationTime.dwLowDateTime;

  Buffer->LastAccessTime.HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  Buffer->LastAccessTime.LowPart = FindData->ftLastAccessTime.dwLowDateTime;

  Buffer->LastWriteTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->LastWriteTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->ChangeTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->ChangeTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->EaSize = 0;
  Buffer->ReparsePointTag = 0;
  RtlFillMemory(&Buffer->FileId.Identifier, sizeof Buffer->FileId.Identifier, 0);

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

VOID DokanFillBothDirInfo(PFILE_BOTH_DIR_INFORMATION Buffer,
                          PWIN32_FIND_DATAW FindData, ULONG Index,
                          PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileAttributes = FindData->dwFileAttributes;
  Buffer->FileNameLength = nameBytes;
  Buffer->ShortNameLength = 0;

  Buffer->EndOfFile.HighPart = FindData->nFileSizeHigh;
  Buffer->EndOfFile.LowPart = FindData->nFileSizeLow;
  Buffer->AllocationSize.HighPart = FindData->nFileSizeHigh;
  Buffer->AllocationSize.LowPart = FindData->nFileSizeLow;
  ALIGN_ALLOCATION_SIZE(&Buffer->AllocationSize, DokanInstance->DokanOptions);

  Buffer->CreationTime.HighPart = FindData->ftCreationTime.dwHighDateTime;
  Buffer->CreationTime.LowPart = FindData->ftCreationTime.dwLowDateTime;

  Buffer->LastAccessTime.HighPart = FindData->ftLastAccessTime.dwHighDateTime;
  Buffer->LastAccessTime.LowPart = FindData->ftLastAccessTime.dwLowDateTime;

  Buffer->LastWriteTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->LastWriteTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->ChangeTime.HighPart = FindData->ftLastWriteTime.dwHighDateTime;
  Buffer->ChangeTime.LowPart = FindData->ftLastWriteTime.dwLowDateTime;

  Buffer->EaSize = 0;

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

VOID DokanFillNamesInfo(PFILE_NAMES_INFORMATION Buffer,
                        PWIN32_FIND_DATAW FindData, ULONG Index) {
  ULONG nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  Buffer->FileIndex = Index;
  Buffer->FileNameLength = nameBytes;

  RtlCopyMemory(Buffer->FileName, FindData->cFileName, nameBytes);
}

ULONG
DokanFillDirectoryInformation(FILE_INFORMATION_CLASS DirectoryInfo,
                              PVOID Buffer, PULONG LengthRemaining,
                              PWIN32_FIND_DATAW FindData, ULONG Index,
                              PDOKAN_INSTANCE DokanInstance) {
  ULONG nameBytes;
  ULONG thisEntrySize;

  nameBytes = (ULONG)wcslen(FindData->cFileName) * sizeof(WCHAR);

  thisEntrySize = nameBytes;

  switch (DirectoryInfo) {
  case FileDirectoryInformation:
    thisEntrySize += sizeof(FILE_DIRECTORY_INFORMATION);
    break;
  case FileFullDirectoryInformation:
    thisEntrySize += sizeof(FILE_FULL_DIR_INFORMATION);
    break;
  case FileIdFullDirectoryInformation:
    thisEntrySize += sizeof(FILE_ID_FULL_DIR_INFORMATION);
    break;
  case FileNamesInformation:
    thisEntrySize += sizeof(FILE_NAMES_INFORMATION);
    break;
  case FileBothDirectoryInformation:
    thisEntrySize += sizeof(FILE_BOTH_DIR_INFORMATION);
    break;
  case FileIdBothDirectoryInformation:
    thisEntrySize += sizeof(FILE_ID_BOTH_DIR_INFORMATION);
    break;
  case FileIdExtdDirectoryInformation:
    thisEntrySize += sizeof(FILE_ID_EXTD_DIR_INFO);
    break;
  case FileIdExtdBothDirectoryInformation:
    thisEntrySize += sizeof(FILE_ID_EXTD_BOTH_DIR_INFORMATION);
    break;
  default:
    break;
  }

  // Must be align on a 8-byte boundary.
  thisEntrySize = QuadAlign(thisEntrySize);

  // no more memory, don't fill any more
  if (*LengthRemaining < thisEntrySize) {
    DbgPrint("  no memory\n");
    return 0;
  }

  RtlZeroMemory(Buffer, thisEntrySize);

  switch (DirectoryInfo) {
  case FileDirectoryInformation:
    DokanFillDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  case FileFullDirectoryInformation:
    DokanFillFullDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  case FileIdFullDirectoryInformation:
    DokanFillIdFullDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  case FileNamesInformation:
    DokanFillNamesInfo(Buffer, FindData, Index);
    break;
  case FileBothDirectoryInformation:
    DokanFillBothDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  case FileIdBothDirectoryInformation:
    DokanFillIdBothDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  case FileIdExtdDirectoryInformation:
    DokanFillIdExtdDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  case FileIdExtdBothDirectoryInformation:
    DokanFillIdExtdBothDirInfo(Buffer, FindData, Index, DokanInstance);
    break;    
  default:
    break;
  }

  *LengthRemaining -= thisEntrySize;

  return thisEntrySize;
}

int WINAPI DokanFillFileData(PWIN32_FIND_DATAW FindData,
                             PDOKAN_FILE_INFO FileInfo) {
  assert(FileInfo->ProcessingContext);
  PDOKAN_VECTOR dirList = (PDOKAN_VECTOR )FileInfo->ProcessingContext;
  DokanVector_PushBack(dirList, FindData);
  return 0;
}

// add entry which matches the pattern specifed in EventContext
// to the buffer specifed in EventInfo
//
LONG MatchFiles(PDOKAN_IO_EVENT IoEvent, PDOKAN_VECTOR DirList) {
  ULONG lengthRemaining =
      IoEvent->EventContext->Operation.Directory.BufferLength;
  PVOID currentBuffer = IoEvent->EventResult->Buffer;
  PVOID lastBuffer = currentBuffer;
  ULONG index = 0;
  BOOL patternCheck = FALSE;
  PWCHAR pattern = NULL;
  BOOL bufferOverFlow = FALSE;

  if (IoEvent->EventContext->Operation.Directory.SearchPatternLength > 0) {
    pattern = (PWCHAR)((SIZE_T)&IoEvent->EventContext->Operation.Directory
                           .SearchPatternBase[0] +
                       (SIZE_T)IoEvent->EventContext->Operation.Directory
                           .SearchPatternOffset);
  }

  if (pattern && wcscmp(pattern, L"*") != 0 &&
      !IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern) {
    patternCheck = TRUE;
  }

  for (size_t i = 0; i < DokanVector_GetCount(DirList); ++i) {
    PDOKAN_FIND_DATA find = (PDOKAN_FIND_DATA)DokanVector_GetItem(DirList, i);
    DbgPrintW(L"FileMatch? : %s (%s,%d,%d)\n", find->FindData.cFileName,
              (pattern ? pattern : L"null"),
              IoEvent->EventContext->Operation.Directory.FileIndex, index);

    // pattern is not specified or pattern match is ignore cases
    if (!patternCheck ||
        DokanIsNameInExpression(pattern, find->FindData.cFileName, TRUE)) {
      if (IoEvent->EventContext->Operation.Directory.FileIndex <= index) {
        // index+1 is very important, should use next entry index
        ULONG entrySize = DokanFillDirectoryInformation(
            IoEvent->EventContext->Operation.Directory.FileInformationClass,
            currentBuffer, &lengthRemaining, &find->FindData, index + 1,
            IoEvent->DokanInstance);
        // buffer is full
        if (entrySize == 0) {
          bufferOverFlow = TRUE;
          break;
        }
        // pointer of the current last entry
        lastBuffer = currentBuffer;
        // end if needs to return single entry
        if (IoEvent->EventContext->Flags & SL_RETURN_SINGLE_ENTRY) {

          DbgPrint("  =>return single entry\n");
          index++;
          break;
        }
        DbgPrint("  =>return\n");
        // the offset of next entry
        ((PFILE_BOTH_DIR_INFORMATION)currentBuffer)->NextEntryOffset =
            entrySize;
        // next buffer position
        currentBuffer = (PCHAR)currentBuffer + entrySize;
      }
      index++;
    }
  }

  // Since next of the last entry doesn't exist, clear next offset
  ((PFILE_BOTH_DIR_INFORMATION)lastBuffer)->NextEntryOffset = 0;
  // acctualy used length of buffer
  IoEvent->EventResult->BufferLength =
      IoEvent->EventContext->Operation.Directory.BufferLength -
      lengthRemaining;
  if (index <= IoEvent->EventContext->Operation.Directory.FileIndex) {
    if (bufferOverFlow)
      return -2; // BUFFER_OVERFLOW
    return -1;   // NO_MORE_FILES
  }
  return index;
}

VOID AddMissingCurrentAndParentFolder(PDOKAN_IO_EVENT IoEvent) {
  PWCHAR pattern = NULL;
  BOOLEAN currentFolder = FALSE, parentFolder = FALSE;
  WIN32_FIND_DATAW findData;
  FILETIME systime;
  PDOKAN_VECTOR dirList = (PDOKAN_VECTOR)IoEvent->DokanFileInfo.ProcessingContext;

  assert(dirList);
  if (IoEvent->EventContext->Operation.Directory.SearchPatternLength != 0) {
    pattern = (PWCHAR)((SIZE_T)&IoEvent->EventContext->Operation.Directory
                           .SearchPatternBase[0] +
                       (SIZE_T)IoEvent->EventContext->Operation.Directory
                           .SearchPatternOffset);
  }

  if (wcscmp(IoEvent->EventContext->Operation.Directory.DirectoryName,
             L"\\") == 0 ||
      (pattern != NULL && wcscmp(pattern, L"*") != 0)) {
    return;
  }

  for (size_t i = 0;
       (!currentFolder || !parentFolder) && i < DokanVector_GetCount(dirList);
       ++i) {
    PDOKAN_FIND_DATA find = (PDOKAN_FIND_DATA)DokanVector_GetItem(dirList, i);
    if (wcscmp(find->FindData.cFileName, L".") == 0) {
      currentFolder = TRUE;
    }

    if (wcscmp(find->FindData.cFileName, L"..") == 0) {
      parentFolder = TRUE;
    }
  }

  if (!currentFolder || !parentFolder) {
    GetSystemTimeAsFileTime(&systime);
    ZeroMemory(&findData, sizeof(WIN32_FIND_DATAW));
    findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    findData.ftCreationTime = systime;
    findData.ftLastAccessTime = systime;
    findData.ftLastWriteTime = systime;
    // Folders times should be the real current and parent folder times...
    // TODO: Should it be PushFront ?
    if (!parentFolder) {
      findData.cFileName[0] = '.';
      findData.cFileName[1] = '.';
      // NULL written during ZeroMemory()
      DokanVector_PushBack(dirList, &findData);
    }
    if (!currentFolder) {
      findData.cFileName[0] = '.';
      findData.cFileName[1] = '\0';
      DokanVector_PushBack(dirList, &findData);
    }
  }
}

NTSTATUS WriteDirectoryResults(PDOKAN_IO_EVENT EventInfo,
                               PDOKAN_VECTOR dirList) {
  // If this function is called then so far everything should be good
  assert(EventInfo->EventResult->Status == STATUS_SUCCESS);
  // Write the file info to the output buffer
  int index = MatchFiles(EventInfo, dirList);
  DbgPrint("WriteDirectoryResults() New directory index is %d.\n", index);
  // there is no matched file
  if (index < 0) {
    if (index == -1) {
      if (EventInfo->EventContext->Operation.Directory.FileIndex == 0) {
        DbgPrint("  STATUS_NO_SUCH_FILE\n");
        EventInfo->EventResult->Status = STATUS_NO_SUCH_FILE;
      } else {

        DbgPrint("  STATUS_NO_MORE_FILES\n");
        EventInfo->EventResult->Status = STATUS_NO_MORE_FILES;
      }
    } else if (index == -2) {
      DbgPrint("  STATUS_BUFFER_OVERFLOW\n");
      EventInfo->EventResult->Status = STATUS_BUFFER_OVERFLOW;
    }
    EventInfo->EventResult->Operation.Directory.Index =
        EventInfo->EventContext->Operation.Directory.FileIndex;
  } else {
    DbgPrint("index to %d\n", index);
    EventInfo->EventResult->Operation.Directory.Index = index;
  }
  return EventInfo->EventResult->Status;
}

VOID EndFindFilesCommon(PDOKAN_IO_EVENT IoEvent, NTSTATUS Status) {
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
    AddMissingCurrentAndParentFolder(IoEvent);
    Status = WriteDirectoryResults(IoEvent, dirList);
    EnterCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
    {
      if (IoEvent->DokanOpenInfo->DirList != dirList) {
        oldDirList = IoEvent->DokanOpenInfo->DirList;
        IoEvent->DokanOpenInfo->DirList = dirList;
      } else {
        // They should never point to the same object
        DbgPrint("Dokan Warning: EndFindFilesCommon() "
                 "EventInfo->DokanOpenInfo->DirList == dirList\n");
      }
      if (IoEvent->DokanOpenInfo->DirListSearchPattern) {

        free(IoEvent->DokanOpenInfo->DirListSearchPattern);
        IoEvent->DokanOpenInfo->DirListSearchPattern = NULL;
      }
      if (IoEvent->EventContext->Operation.Directory
              .SearchPatternLength > 0) {
        IoEvent->DokanOpenInfo->DirListSearchPattern =
            _wcsdup((PWCHAR)((SIZE_T)&IoEvent->EventContext->Operation.Directory
                                 .SearchPatternBase[0] +
                             (SIZE_T)IoEvent->EventContext->Operation.Directory
                                 .SearchPatternOffset));
      }
    }
    LeaveCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
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

   DbgPrint(
      "###FindFiles file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
      IoEvent->DokanOpenInfo,
      IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId : -1,
      IoEvent);

  CheckFileName(IoEvent->EventContext->Operation.Directory.DirectoryName);

  // check whether this is handled FileInfoClass
  if (fileInfoClass != FileDirectoryInformation &&
      fileInfoClass != FileFullDirectoryInformation &&
      fileInfoClass != FileBothDirectoryInformation &&
      fileInfoClass != FileNamesInformation &&
      fileInfoClass != FileIdBothDirectoryInformation &&
      fileInfoClass != FileIdFullDirectoryInformation &&
      fileInfoClass != FileIdExtdDirectoryInformation &&
      fileInfoClass != FileIdExtdBothDirectoryInformation) {
    DbgPrint("Dokan Information: Unsupported file information class %d\n",
             fileInfoClass);
    // send directory info to driver
    IoEvent->EventResult->BufferLength = 0;
    IoEvent->EventResult->Status = STATUS_INVALID_PARAMETER;
    EventCompletion(IoEvent);
    return;
  }

  CreateDispatchCommon(IoEvent,
                       IoEvent->EventContext->Operation.Directory.BufferLength,
                       /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  IoEvent->EventResult->Operation.Directory.Index =
      IoEvent->EventContext->Operation.Directory.FileIndex;

  if (IoEvent->EventContext->Operation.Directory.SearchPatternLength > 0) {
    searchPattern = (PWCHAR)((SIZE_T)&IoEvent->EventContext->Operation.Directory
                                 .SearchPatternBase[0] +
                             (SIZE_T)IoEvent->EventContext->Operation.Directory
                                 .SearchPatternOffset);
  }

  EnterCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
  {
    if (IoEvent->DokanOpenInfo->DirList == NULL) {
      forceScan = TRUE;
    } else if (searchPattern && IoEvent->DokanOpenInfo->DirListSearchPattern) {
      forceScan = wcscmp(searchPattern,
                         IoEvent->DokanOpenInfo->DirListSearchPattern) != 0
                      ? TRUE
                      : FALSE;
    } else if (searchPattern) {
      forceScan = wcscmp(searchPattern, L"*") != 0 ? TRUE : FALSE;
    } else if (IoEvent->DokanOpenInfo->DirListSearchPattern) {
      forceScan =
          wcscmp(IoEvent->DokanOpenInfo->DirListSearchPattern, L"*") != 0
              ? TRUE
              : FALSE;
    }
    // In FastFat SL_INDEX_SPECIFIED overrides SL_RESTART_SCAN
    forceScan =
        (forceScan || (!(IoEvent->EventContext->Flags & SL_INDEX_SPECIFIED) &&
                       (IoEvent->EventContext->Flags & SL_RESTART_SCAN)))
            ? TRUE
            : FALSE;
    if (!forceScan) {
      status = WriteDirectoryResults(IoEvent, IoEvent->DokanOpenInfo->DirList);
    }
  }
  LeaveCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);

  if (!forceScan) {
    IoEvent->EventResult->Status = status;
    EventCompletion(IoEvent);
    return;
  }

  IoEvent->DokanFileInfo.ProcessingContext = PopDirectoryList();
  if (!IoEvent->DokanFileInfo.ProcessingContext) {
    DbgPrint(
        "Dokan Error: Failed to allocate memory for a new directory list.\n");
    IoEvent->EventResult->Status = STATUS_NO_MEMORY;
    EventCompletion(IoEvent);
    return;
  }

  if ((!searchPattern ||
       !IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern) &&
      IoEvent->DokanInstance->DokanOperations->FindFiles) {
    status = IoEvent->DokanInstance->DokanOperations->FindFiles(
        IoEvent->EventContext->Operation.Directory.DirectoryName,
        DokanFillFileData, &IoEvent->DokanFileInfo);
    EndFindFilesCommon(IoEvent, status);

  } else if (IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern) {
    status = IoEvent->DokanInstance->DokanOperations->FindFilesWithPattern(
        IoEvent->EventContext->Operation.Directory.DirectoryName,
        searchPattern ? searchPattern : L"*", DokanFillFileData,
        &IoEvent->DokanFileInfo);
    EndFindFilesCommon(IoEvent, status);
  } else {
    IoEvent->EventResult->Status = STATUS_NOT_IMPLEMENTED;
    EventCompletion(IoEvent);
  }
}

#define DOS_STAR (L'<')
#define DOS_QM (L'>')
#define DOS_DOT (L'"')

BOOL DOKANAPI DokanIsNameInExpression(LPCWSTR Expression, // matching pattern
                                      LPCWSTR Name,       // file name
                                      BOOL IgnoreCase) {
  ULONG ei = 0;
  ULONG ni = 0;

  while (Expression[ei] != '\0') {

    if (Expression[ei] == L'*') {
      ei++;
      if (Expression[ei] == '\0')
        return TRUE;

      while (Name[ni] != '\0') {
        if (DokanIsNameInExpression(&Expression[ei], &Name[ni], IgnoreCase))
          return TRUE;
        ni++;
      }

    } else if (Expression[ei] == DOS_STAR) {

      ULONG p = ni;
      ULONG lastDot = 0;
      ei++;

      while (Name[p] != '\0') {
        if (Name[p] == L'.')
          lastDot = p;
        p++;
      }

      BOOL endReached = FALSE;
      while (!endReached) {

        endReached = (Name[ni] == '\0' || ni == lastDot);

        if (!endReached) {
          if (DokanIsNameInExpression(&Expression[ei], &Name[ni], IgnoreCase))
            return TRUE;

          ni++;
        }
      }

    } else if (Expression[ei] == DOS_QM) {

      ei++;
      if (Name[ni] != L'.') {
        ni++;
      } else {

        ULONG p = ni + 1;
        while (Name[p] != '\0') {
          if (Name[p] == L'.')
            break;
          p++;
        }

        if (Name[p] == L'.')
          ni++;
      }

    } else if (Expression[ei] == DOS_DOT) {
      ei++;

      if (Name[ni] == L'.')
        ni++;

    } else {
      if (Expression[ei] == L'?') {
        ei++;
        ni++;
      } else if (IgnoreCase && towupper(Expression[ei]) == towupper(Name[ni])) {
        ei++;
        ni++;
      } else if (!IgnoreCase && Expression[ei] == Name[ni]) {
        ei++;
        ni++;
      } else {
        return FALSE;
      }
    }
  }

  if (ei == wcslen(Expression) && ni == wcslen(Name))
    return TRUE;

  return FALSE;
}
