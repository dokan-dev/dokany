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
#include "list.h"

#include <assert.h>

#if _MSC_VER < 1300 // VC6
typedef ULONG ULONG_PTR;
#endif

typedef struct _DOKAN_FIND_DATA {
  WIN32_FIND_DATAW FindData;
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
  case FileNamesInformation:
    thisEntrySize += sizeof(FILE_NAMES_INFORMATION);
    break;
  case FileBothDirectoryInformation:
    thisEntrySize += sizeof(FILE_BOTH_DIR_INFORMATION);
    break;
  case FileIdBothDirectoryInformation:
    thisEntrySize += sizeof(FILE_ID_BOTH_DIR_INFORMATION);
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
  case FileNamesInformation:
    DokanFillNamesInfo(Buffer, FindData, Index);
    break;
  case FileBothDirectoryInformation:
    DokanFillBothDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  case FileIdBothDirectoryInformation:
    DokanFillIdBothDirInfo(Buffer, FindData, Index, DokanInstance);
    break;
  default:
    break;
  }

  *LengthRemaining -= thisEntrySize;

  return thisEntrySize;
}

int DokanFillFileDataEx(DOKAN_IO_EVENT *EventInfo, PWIN32_FIND_DATAW FindData) {
  
	assert(EventInfo->ProcessingContext);

	DOKAN_VECTOR *dirList = (DOKAN_VECTOR*)EventInfo->ProcessingContext;

	DokanVector_PushBack(dirList, FindData);

	return 0;
}

int WINAPI DokanFillFileData(PDOKAN_FIND_FILES_EVENT EventInfo, PWIN32_FIND_DATAW FindData) {

	return DokanFillFileDataEx((DOKAN_IO_EVENT*)EventInfo, FindData);
}

int WINAPI DokanFillFileDataWithPattern(PDOKAN_FIND_FILES_PATTERN_EVENT EventInfo, PWIN32_FIND_DATAW FindData) {

	return DokanFillFileDataEx((DOKAN_IO_EVENT*)EventInfo, FindData);
}

// add entry which matches the pattern specifed in EventContext
// to the buffer specifed in EventInfo
//
LONG MatchFiles(DOKAN_IO_EVENT *EventInfo, DOKAN_VECTOR *dirList) {

	ULONG lengthRemaining = EventInfo->KernelInfo.EventContext.Operation.Directory.BufferLength;
	PVOID currentBuffer = EventInfo->EventResult->Buffer;
	PVOID lastBuffer = currentBuffer;
	ULONG index = 0;
	BOOL patternCheck = FALSE;
	PWCHAR pattern = NULL;

	if(EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternLength > 0) {

		pattern = (PWCHAR)(
					(SIZE_T)&EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternBase[0] +
					(SIZE_T)EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternOffset);
	}

	if(pattern && wcscmp(pattern, L"*") != 0
		&& !EventInfo->DokanInstance->DokanOperations->FindFilesWithPattern) {

		patternCheck = TRUE;
	}

	for(size_t i = 0; i < DokanVector_GetCount(dirList); ++i) {

		PDOKAN_FIND_DATA find = (PDOKAN_FIND_DATA)DokanVector_GetItem(dirList, i);

		DbgPrintW(L"FileMatch? : %s (%s,%d,%d)\n", find->FindData.cFileName,
			(pattern ? pattern : L"null"),
			EventInfo->KernelInfo.EventContext.Operation.Directory.FileIndex, index);

		// pattern is not specified or pattern match is ignore cases
		if(!patternCheck ||
			DokanIsNameInExpression(pattern, find->FindData.cFileName, TRUE)) {

			if(EventInfo->KernelInfo.EventContext.Operation.Directory.FileIndex <= index) {

				// index+1 is very important, should use next entry index

				ULONG entrySize = DokanFillDirectoryInformation(
					EventInfo->KernelInfo.EventContext.Operation.Directory.FileInformationClass,
					currentBuffer, &lengthRemaining, &find->FindData, index + 1,
					EventInfo->DokanInstance);

				// buffer is full
				if(entrySize == 0) {

					break;
				}

				// pointer of the current last entry
				lastBuffer = currentBuffer;

				// end if needs to return single entry
				if(EventInfo->KernelInfo.EventContext.Flags & SL_RETURN_SINGLE_ENTRY) {

					DbgPrint("  =>return single entry\n");
					index++;
					break;
				}

				DbgPrint("  =>return\n");

				// the offset of next entry
				((PFILE_BOTH_DIR_INFORMATION)currentBuffer)->NextEntryOffset = entrySize;

				// next buffer position
				currentBuffer = (PCHAR)currentBuffer + entrySize;
			}

			index++;
		}
	}

	// Since next of the last entry doesn't exist, clear next offset
	((PFILE_BOTH_DIR_INFORMATION)lastBuffer)->NextEntryOffset = 0;

	// acctualy used length of buffer
	EventInfo->EventResult->BufferLength =
		EventInfo->KernelInfo.EventContext.Operation.Directory.BufferLength - lengthRemaining;

	// NO_MORE_FILES
	if(index <= EventInfo->KernelInfo.EventContext.Operation.Directory.FileIndex) {

		return -1;
	}

	return index;
}

void AddMissingCurrentAndParentFolder(DOKAN_IO_EVENT *EventInfo) {

  PWCHAR pattern = NULL;
  BOOLEAN currentFolder = FALSE, parentFolder = FALSE;
  WIN32_FIND_DATAW findData;
  FILETIME systime;
  DOKAN_VECTOR *dirList = (DOKAN_VECTOR*)EventInfo->ProcessingContext;

  assert(dirList);

  if (EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternLength != 0) {

    pattern = (PWCHAR)(
        (SIZE_T)&EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternBase[0] +
        (SIZE_T)EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternOffset);
  }

  if(wcscmp(EventInfo->KernelInfo.EventContext.Operation.Directory.DirectoryName, L"\\") == 0
	  || (pattern != NULL && wcscmp(pattern, L"*") != 0)) {

	  return;
  }

  for(size_t i = 0; (!currentFolder || !parentFolder) && i < DokanVector_GetCount(dirList); ++i) {

    PDOKAN_FIND_DATA find = (PDOKAN_FIND_DATA)DokanVector_GetItem(dirList, i);

	if(wcscmp(find->FindData.cFileName, L".") == 0) {
		
		currentFolder = TRUE;
	}
      
	if(wcscmp(find->FindData.cFileName, L"..") == 0) {

		parentFolder = TRUE;
	}
  }

  if(!currentFolder || !parentFolder) {

	  GetSystemTimeAsFileTime(&systime);

	  ZeroMemory(&findData, sizeof(WIN32_FIND_DATAW));

	  findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
	  findData.ftCreationTime = systime;
	  findData.ftLastAccessTime = systime;
	  findData.ftLastWriteTime = systime;

	  // Folders times should be the real current and parent folder times...
	  if(!parentFolder) {

		  findData.cFileName[0] = '.';
		  findData.cFileName[1] = '.';
		  // NULL written during ZeroMemory()

		  DokanVector_PushBack(dirList, &findData);
	  }

	  if(!currentFolder) {

		  findData.cFileName[0] = '.';
		  findData.cFileName[1] = '\0';
		  
		  DokanVector_PushBack(dirList, &findData);
	  }
  }
}

NTSTATUS WriteDirectoryResults(DOKAN_IO_EVENT *EventInfo, DOKAN_VECTOR *dirList) {

	// If this function is called then so far everything should be good
	assert(EventInfo->EventResult->Status == STATUS_SUCCESS);

	// Write the file info to the output buffer
	int index = MatchFiles(EventInfo, dirList);

	DbgPrint("WriteDirectoryResults() New directory index is %d.\n", index);

	// there is no matched file
	if(index < 0) {

		if(EventInfo->KernelInfo.EventContext.Operation.Directory.FileIndex == 0) {

			DbgPrint("  STATUS_NO_SUCH_FILE\n");
			EventInfo->EventResult->Status = STATUS_NO_SUCH_FILE;
		}
		else {

			DbgPrint("  STATUS_NO_MORE_FILES\n");
			EventInfo->EventResult->Status = STATUS_NO_MORE_FILES;
		}

		EventInfo->EventResult->Operation.Directory.Index =
			EventInfo->KernelInfo.EventContext.Operation.Directory.FileIndex;
	}
	else {

		DbgPrint("index to %d\n", index);
		EventInfo->EventResult->Operation.Directory.Index = index;
	}

	return EventInfo->EventResult->Status;
}

void EndDirectoryDispatch(DOKAN_IO_EVENT *EventInfo, NTSTATUS ResultStatus) {

	PEVENT_INFORMATION result = EventInfo->EventResult;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: EndDirectoryDispatch() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	result->Status = ResultStatus;

	SendIoEventResult(EventInfo);
}

void EndFindFilesCommon(DOKAN_IO_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_VECTOR *dirList = (DOKAN_VECTOR*)EventInfo->ProcessingContext;
	DOKAN_VECTOR *oldDirList = NULL;

	assert(EventInfo->EventResult->BufferLength == 0);
	assert(EventInfo->ProcessingContext);

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: EndFindFilesCommon() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	if(ResultStatus == STATUS_SUCCESS) {

		AddMissingCurrentAndParentFolder(EventInfo);
		
		ResultStatus = WriteDirectoryResults(EventInfo, dirList);

		EnterCriticalSection(&EventInfo->DokanOpenInfo->CriticalSection);
		{
			if(EventInfo->DokanOpenInfo->DirList != dirList) {

				oldDirList = EventInfo->DokanOpenInfo->DirList;
				EventInfo->DokanOpenInfo->DirList = dirList;
			}
			else {

				// They should never point to the same object
				DbgPrint("Dokan Warning: EndFindFilesCommon() EventInfo->DokanOpenInfo->DirList == dirList\n");
			}

			if(EventInfo->DokanOpenInfo->DirListSearchPattern) {

				free(EventInfo->DokanOpenInfo->DirListSearchPattern);
				EventInfo->DokanOpenInfo->DirListSearchPattern = NULL;
			}

			if(EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternLength > 0) {

				EventInfo->DokanOpenInfo->DirListSearchPattern =
					_wcsdup((PWCHAR)(
					(SIZE_T)&EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternBase[0] +
						(SIZE_T)EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternOffset));
			}
		}
		LeaveCriticalSection(&EventInfo->DokanOpenInfo->CriticalSection);

		if(oldDirList) {

			PushDirectoryList(oldDirList);
		}
	}
	else {

		PushDirectoryList(dirList);
	}

	EventInfo->ProcessingContext = NULL;

	EndDirectoryDispatch(EventInfo, ResultStatus);
}

void BeginDispatchDirectoryInformation(DOKAN_IO_EVENT *EventInfo) {

  PDOKAN_INSTANCE dokan = EventInfo->DokanInstance;
  PWCHAR searchPattern = NULL;
  NTSTATUS status = STATUS_SUCCESS;
  ULONG fileInfoClass = EventInfo->KernelInfo.EventContext.Operation.Directory.FileInformationClass;
  BOOL forceScan = FALSE;

  DbgPrint("###FindFiles file handle = 0x%p, eventID = %04d\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1);

  assert(EventInfo->ProcessingContext == NULL);

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Directory.DirectoryName);

  // check whether this is handled FileInfoClass
  if (fileInfoClass != FileDirectoryInformation &&
      fileInfoClass != FileFullDirectoryInformation &&
      fileInfoClass != FileNamesInformation &&
      fileInfoClass != FileIdBothDirectoryInformation &&
      fileInfoClass != FileBothDirectoryInformation) {

    DbgPrint("Dokan Information: Unsupported file information class %d\n", fileInfoClass);

    // send directory info to driver
	EndDirectoryDispatch(EventInfo, STATUS_NOT_IMPLEMENTED);

    return;
  }

  CreateDispatchCommon(EventInfo, EventInfo->KernelInfo.EventContext.Operation.Directory.BufferLength);

  EventInfo->EventResult->Operation.Directory.Index =
	  EventInfo->KernelInfo.EventContext.Operation.Directory.FileIndex;

  if(EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternLength > 0) {

	  searchPattern = (PWCHAR)(
		  (SIZE_T)&EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternBase[0] +
		  (SIZE_T)EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternOffset);
  }

  EnterCriticalSection(&EventInfo->DokanOpenInfo->CriticalSection);
  {
	  if(EventInfo->DokanOpenInfo->DirList == NULL) {

		  forceScan = TRUE;
	  }
	  else if(searchPattern && EventInfo->DokanOpenInfo->DirListSearchPattern) {

		  forceScan = wcscmp(searchPattern, EventInfo->DokanOpenInfo->DirListSearchPattern) != 0 ? TRUE : FALSE;
	  }
	  else if(searchPattern) {

		  forceScan = wcscmp(searchPattern, L"*") != 0 ? TRUE : FALSE;
	  }
	  else if(EventInfo->DokanOpenInfo->DirListSearchPattern) {

		  forceScan = wcscmp(EventInfo->DokanOpenInfo->DirListSearchPattern, L"*") != 0 ? TRUE : FALSE;
	  }

	  // In FastFat SL_INDEX_SPECIFIED overrides SL_RESTART_SCAN
	  forceScan = (forceScan
		  || (!(EventInfo->KernelInfo.EventContext.Flags & SL_INDEX_SPECIFIED)
			  && (EventInfo->KernelInfo.EventContext.Flags & SL_RESTART_SCAN))) ? TRUE : FALSE;

	  if(!forceScan) {

		  status = WriteDirectoryResults(EventInfo, EventInfo->DokanOpenInfo->DirList);
	  }
  }
  LeaveCriticalSection(&EventInfo->DokanOpenInfo->CriticalSection);

  if(!forceScan) {

	  EndDirectoryDispatch(EventInfo, status);
  }
  else {

	  EventInfo->ProcessingContext = PopDirectoryList();

	  if(!EventInfo->ProcessingContext) {

		  DbgPrint("Dokan Error: Failed to allocate memory for a new directory list.\n");

		  EndDirectoryDispatch(EventInfo, STATUS_NO_MEMORY);

		  return;
	  }

	  if((EventInfo->KernelInfo.EventContext.Operation.Directory.SearchPatternLength == 0
		  && dokan->DokanOperations->FindFiles)
		  || !dokan->DokanOperations->FindFilesWithPattern) {

		  DOKAN_FIND_FILES_EVENT *findFiles = &EventInfo->EventInfo.FindFiles;

		  assert((void*)findFiles == (void*)EventInfo);
		  
		  findFiles->DokanFileInfo = &EventInfo->DokanFileInfo;
		  findFiles->PathName = EventInfo->KernelInfo.EventContext.Operation.Directory.DirectoryName;
		  findFiles->FillFindData = DokanFillFileData;

		  status = dokan->DokanOperations->FindFiles(findFiles);

		  if(status != STATUS_PENDING) {

			  DokanEndDispatchFindFiles(findFiles, status);
		  }
	  }
	  else if(dokan->DokanOperations->FindFilesWithPattern) {

		  DOKAN_FIND_FILES_PATTERN_EVENT *findFilesPattern = &EventInfo->EventInfo.FindFilesWithPattern;

		  findFilesPattern->DokanFileInfo = &EventInfo->DokanFileInfo;
		  findFilesPattern->PathName = EventInfo->KernelInfo.EventContext.Operation.Directory.DirectoryName;
		  findFilesPattern->SearchPattern = searchPattern ? searchPattern : L"*";
		  findFilesPattern->FillFindData = DokanFillFileDataWithPattern;

		  status = dokan->DokanOperations->FindFilesWithPattern(findFilesPattern);

		  if(status != STATUS_PENDING) {

			  DokanEndDispatchFindFilesWithPattern(findFilesPattern, status);
		  }
	  }
	  else {

		  EndDirectoryDispatch(EventInfo, STATUS_NOT_IMPLEMENTED);
	  }
  }
}

#define DOS_STAR (L'<')
#define DOS_QM (L'>')
#define DOS_DOT (L'"')

// check whether Name matches Expression
// Expression can contain "?"(any one character) and "*" (any string)
// when IgnoreCase is TRUE, do case insenstive matching
//
// http://msdn.microsoft.com/en-us/library/ff546850(v=VS.85).aspx
// * (asterisk) Matches zero or more characters.
// ? (question mark) Matches a single character.
// DOS_DOT Matches either a period or zero characters beyond the name string.
// DOS_QM Matches any single character or, upon encountering a period or end
//        of name string, advances the expression to the end of the set of
//        contiguous DOS_QMs.
// DOS_STAR Matches zero or more characters until encountering and matching
//          the final . in the name.
BOOL DOKANAPI DokanIsNameInExpression(LPCWSTR Expression, // matching pattern
                                      LPCWSTR Name,       // file name
                                      BOOL IgnoreCase) {
  ULONG ei = 0;
  ULONG ni = 0;

  if((!Expression || !Expression[0]) && (!Name || !Name[0])) {

	  return TRUE;
  }

  if(!Expression || !Name || !Expression[0] || !Name[0]) {

	  return FALSE;
  }

  while (Expression[ei] && Name[ni]) {

    if (Expression[ei] == L'*') {

      ei++;

	  if(Expression[ei] == '\0') {

		  return TRUE;
	  }

      while (Name[ni] != '\0') {

		  if(DokanIsNameInExpression(&Expression[ei], &Name[ni], IgnoreCase)) {

			  return TRUE;
		  }

        ni++;
      }
    }
	else if (Expression[ei] == DOS_STAR) {

      ULONG p = ni;
      ULONG lastDot = 0;
      ei++;

      while (Name[p] != '\0') {

		  if(Name[p] == L'.') {

			  lastDot = p;
		  }

        p++;
      }

      BOOL endReached = FALSE;

      while (!endReached) {

        endReached = (Name[ni] == '\0' || ni == lastDot);

        if (!endReached) {

			if(DokanIsNameInExpression(&Expression[ei], &Name[ni], IgnoreCase)) {
				
				return TRUE;
			}

          ni++;
        }
      }
    }
	else if (Expression[ei] == DOS_QM) {

      ei++;

      if (Name[ni] != L'.') {

        ni++;
      }
	  else {

        ULONG p = ni + 1;

        while (Name[p] != '\0') {

			if(Name[p] == L'.') {

				break;
			}

          p++;
        }

		if(Name[p] == L'.') {

			ni++;
		}
      }
    }
	else if (Expression[ei] == DOS_DOT) {

      ei++;

	  if(Name[ni] == L'.') {

		  ni++;
	  }
    }
	else {

      if (Expression[ei] == L'?'
		  || (IgnoreCase && towupper(Expression[ei]) == towupper(Name[ni]))
		  || (!IgnoreCase && Expression[ei] == Name[ni])) {

        ei++;
        ni++;
      }
	  else {

        return FALSE;
      }
    }
  }

  return !Expression[ei] && !Name[ni] ? TRUE : FALSE;
}

void DOKANAPI DokanEndDispatchFindFiles(DOKAN_FIND_FILES_EVENT *EventInfo, NTSTATUS ResultStatus) {

	EndFindFilesCommon((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}

void DOKANAPI DokanEndDispatchFindFilesWithPattern(DOKAN_FIND_FILES_PATTERN_EVENT *EventInfo, NTSTATUS ResultStatus) {

	EndFindFilesCommon((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}