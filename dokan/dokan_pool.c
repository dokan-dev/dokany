/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2016 Keith Newton
  Copyright (C) 2021 Google, Inc.

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

#include "dokan_pool.h"
#include "dokan_vector.h"

#include <assert.h>
#include <threadpoolapiset.h>

#define DOKAN_IO_BATCH_POOL_SIZE 1024
#define DOKAN_IO_EVENT_POOL_SIZE 1024
#define DOKAN_IO_EXTRA_EVENT_POOL_SIZE 128
#define DOKAN_DIRECTORY_LIST_POOL_SIZE 128

// Global thread pool
PTP_POOL g_ThreadPool = NULL;

// Global vector of event buffers
PDOKAN_VECTOR g_IoBatchBufferPool = NULL;
CRITICAL_SECTION g_IoBatchBufferCriticalSection;

PDOKAN_VECTOR g_IoEventBufferPool = NULL;
CRITICAL_SECTION g_IoEventBufferCriticalSection;

PDOKAN_VECTOR g_EventResultPool = NULL;
CRITICAL_SECTION g_EventResultCriticalSection;

PDOKAN_VECTOR g_16KEventResultPool = NULL;
CRITICAL_SECTION g_16KEventResultCriticalSection;

PDOKAN_VECTOR g_32KEventResultPool = NULL;
CRITICAL_SECTION g_32KEventResultCriticalSection;

PDOKAN_VECTOR g_64KEventResultPool = NULL;
CRITICAL_SECTION g_64KEventResultCriticalSection;

PDOKAN_VECTOR g_128KEventResultPool = NULL;
CRITICAL_SECTION g_128KEventResultCriticalSection;

PDOKAN_VECTOR g_FileInfoPool = NULL;
CRITICAL_SECTION g_FileInfoCriticalSection;

PDOKAN_VECTOR g_DirectoryListPool = NULL;
CRITICAL_SECTION g_DirectoryListCriticalSection;

PTP_POOL GetThreadPool() { return g_ThreadPool; }

VOID FreeIoEventBuffer(PDOKAN_IO_EVENT IoEvent) {
  if (IoEvent) {
    free(IoEvent);
  }
}

int InitializePool() {
  (void)InitializeCriticalSectionAndSpinCount(&g_IoBatchBufferCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_IoEventBufferCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_EventResultCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_16KEventResultCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_32KEventResultCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_64KEventResultCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_128KEventResultCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_FileInfoCriticalSection,
                                              0x80000400);
  (void)InitializeCriticalSectionAndSpinCount(&g_DirectoryListCriticalSection,
                                              0x80000400);

  if (g_ThreadPool) {
    DokanDbgPrint("Dokan Error: Thread pool has already been created.\n");
    return DOKAN_DRIVER_INSTALL_ERROR;
  }

  // It seems this is only needed if LoadLibrary() and FreeLibrary() are used and it should be called by the exe
  // SetThreadpoolCallbackLibrary(&g_ThreadPoolCallbackEnvironment, hModule);
  g_ThreadPool = CreateThreadpool(NULL);
  if (!g_ThreadPool) {
    DokanDbgPrint("Dokan Error: Failed to create thread pool.\n");
    return DOKAN_DRIVER_INSTALL_ERROR;
  }

  g_IoBatchBufferPool =
      DokanVector_AllocWithCapacity(sizeof(PVOID), DOKAN_IO_BATCH_POOL_SIZE);
  g_IoEventBufferPool =
      DokanVector_AllocWithCapacity(sizeof(PVOID), DOKAN_IO_EVENT_POOL_SIZE);
  g_EventResultPool =
      DokanVector_AllocWithCapacity(sizeof(PVOID), DOKAN_IO_EVENT_POOL_SIZE);
  g_16KEventResultPool = DokanVector_AllocWithCapacity(
      sizeof(PVOID), DOKAN_IO_EXTRA_EVENT_POOL_SIZE);
  g_32KEventResultPool = DokanVector_AllocWithCapacity(
      sizeof(PVOID), DOKAN_IO_EXTRA_EVENT_POOL_SIZE);
  g_64KEventResultPool = DokanVector_AllocWithCapacity(
      sizeof(PVOID), DOKAN_IO_EXTRA_EVENT_POOL_SIZE);
  g_128KEventResultPool = DokanVector_AllocWithCapacity(
      sizeof(PVOID), DOKAN_IO_EXTRA_EVENT_POOL_SIZE);
  g_FileInfoPool =
      DokanVector_AllocWithCapacity(sizeof(PVOID), DOKAN_IO_EVENT_POOL_SIZE);
  g_DirectoryListPool = DokanVector_AllocWithCapacity(
      sizeof(PVOID), DOKAN_DIRECTORY_LIST_POOL_SIZE);
  return DOKAN_SUCCESS;
}

VOID CleanupPool() {
  if (g_ThreadPool) {
    CloseThreadpool(g_ThreadPool);
    g_ThreadPool = NULL;
  }
  //////////////////// IO batch buffer object pool ////////////////////
  {
    EnterCriticalSection(&g_IoBatchBufferCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_IoBatchBufferPool); ++i) {
        FreeIoBatchBuffer(
            *(PDOKAN_IO_BATCH *)DokanVector_GetItem(g_IoBatchBufferPool, i));
      }
      DokanVector_Free(g_IoBatchBufferPool);
      g_IoBatchBufferPool = NULL;
    }
    LeaveCriticalSection(&g_IoBatchBufferCriticalSection);
    DeleteCriticalSection(&g_IoBatchBufferCriticalSection);
  }

  //////////////////// IO event buffer object pool ////////////////////
  {
    EnterCriticalSection(&g_IoEventBufferCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_IoEventBufferPool); ++i) {
        FreeIoEventBuffer(
            *(PDOKAN_IO_EVENT *)DokanVector_GetItem(g_IoEventBufferPool, i));
      }
      DokanVector_Free(g_IoEventBufferPool);
      g_IoEventBufferPool = NULL;
    }
    LeaveCriticalSection(&g_IoEventBufferCriticalSection);
    DeleteCriticalSection(&g_IoEventBufferCriticalSection);
  }

  //////////////////// Event result object pool ////////////////////
  {
    EnterCriticalSection(&g_EventResultCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_EventResultPool); ++i) {
        FreeEventResult(
            *(PEVENT_INFORMATION *)DokanVector_GetItem(g_EventResultPool, i));
      }
      DokanVector_Free(g_EventResultPool);
      g_EventResultPool = NULL;
    }
    LeaveCriticalSection(&g_EventResultCriticalSection);
    DeleteCriticalSection(&g_EventResultCriticalSection);
  }

  {
    EnterCriticalSection(&g_16KEventResultCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_16KEventResultPool); ++i) {
        FreeEventResult(*(PEVENT_INFORMATION *)DokanVector_GetItem(
            g_16KEventResultPool, i));
      }
      DokanVector_Free(g_16KEventResultPool);
      g_16KEventResultPool = NULL;
    }
    LeaveCriticalSection(&g_16KEventResultCriticalSection);
    DeleteCriticalSection(&g_16KEventResultCriticalSection);
  }

  {
    EnterCriticalSection(&g_32KEventResultCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_32KEventResultPool); ++i) {
        FreeEventResult(*(PEVENT_INFORMATION *)DokanVector_GetItem(
            g_32KEventResultPool, i));
      }
      DokanVector_Free(g_32KEventResultPool);
      g_32KEventResultPool = NULL;
    }
    LeaveCriticalSection(&g_32KEventResultCriticalSection);
    DeleteCriticalSection(&g_32KEventResultCriticalSection);
  }

  {
    EnterCriticalSection(&g_64KEventResultCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_64KEventResultPool); ++i) {
        FreeEventResult(*(PEVENT_INFORMATION *)DokanVector_GetItem(
            g_64KEventResultPool, i));
      }
      DokanVector_Free(g_64KEventResultPool);
      g_64KEventResultPool = NULL;
    }
    LeaveCriticalSection(&g_64KEventResultCriticalSection);
    DeleteCriticalSection(&g_64KEventResultCriticalSection);
  }

  {
    EnterCriticalSection(&g_128KEventResultCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_128KEventResultPool); ++i) {
        FreeEventResult(*(PEVENT_INFORMATION *)DokanVector_GetItem(
            g_128KEventResultPool, i));
      }
      DokanVector_Free(g_128KEventResultPool);
      g_128KEventResultPool = NULL;
    }
    LeaveCriticalSection(&g_128KEventResultCriticalSection);
    DeleteCriticalSection(&g_128KEventResultCriticalSection);
  }

  //////////////////// File info object pool ////////////////////
  {
    EnterCriticalSection(&g_FileInfoCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_FileInfoPool); ++i) {
        FreeFileOpenInfo(
            *(PDOKAN_OPEN_INFO *)DokanVector_GetItem(g_FileInfoPool, i));
      }
      DokanVector_Free(g_FileInfoPool);
      g_FileInfoPool = NULL;
    }
    LeaveCriticalSection(&g_FileInfoCriticalSection);
    DeleteCriticalSection(&g_FileInfoCriticalSection);
  }

  //////////////////// Directory list pool ////////////////////
  {
    EnterCriticalSection(&g_DirectoryListCriticalSection);
    {
      for (size_t i = 0; i < DokanVector_GetCount(g_DirectoryListPool); ++i) {
        DokanVector_Free(
            *(PDOKAN_VECTOR *)DokanVector_GetItem(g_DirectoryListPool, i));
      }
      DokanVector_Free(g_DirectoryListPool);
      g_DirectoryListPool = NULL;
    }
    LeaveCriticalSection(&g_DirectoryListCriticalSection);
    DeleteCriticalSection(&g_DirectoryListCriticalSection);
  }

  //////////////////// Object pool cleanup finished ////////////////////
}

/////////////////// DOKAN_IO_BATCH ///////////////////
PDOKAN_IO_BATCH PopIoBatchBuffer() {
  PDOKAN_IO_BATCH ioBatch = NULL;
  EnterCriticalSection(&g_IoBatchBufferCriticalSection);
  {
    if (DokanVector_GetCount(g_IoBatchBufferPool) > 0) {
      ioBatch =
          *(PDOKAN_IO_BATCH *)DokanVector_GetLastItem(g_IoBatchBufferPool);
      DokanVector_PopBack(g_IoBatchBufferPool);
    }
  }
  LeaveCriticalSection(&g_IoBatchBufferCriticalSection);
  if (!ioBatch) {
    ioBatch = (PDOKAN_IO_BATCH)malloc(DOKAN_IO_BATCH_SIZE);
  }
  if (ioBatch) {
    RtlZeroMemory(ioBatch, FIELD_OFFSET(DOKAN_IO_BATCH, EventContext));
    ioBatch->PoolAllocated = TRUE;
  }
  return ioBatch;
}

VOID FreeIoBatchBuffer(PDOKAN_IO_BATCH IoBatch) {
  if (IoBatch) {
    free(IoBatch);
  }
}

VOID PushIoBatchBuffer(PDOKAN_IO_BATCH IoBatch) {
  assert(IoBatch);
  LONG currentEventContextBatchCount =
      InterlockedDecrement(&IoBatch->EventContextBatchCount);
  if (currentEventContextBatchCount > 0) {
    return;
  }
  if (!IoBatch->PoolAllocated) {
    FreeIoBatchBuffer(IoBatch);
    return;
  }
  EnterCriticalSection(&g_IoBatchBufferCriticalSection);
  {
    if (DokanVector_GetCount(g_IoBatchBufferPool) < DOKAN_IO_BATCH_POOL_SIZE) {
      DokanVector_PushBack(g_IoBatchBufferPool, &IoBatch);
      IoBatch = NULL;
    }
  }
  LeaveCriticalSection(&g_IoBatchBufferCriticalSection);
  if (IoBatch) {
    FreeIoBatchBuffer(IoBatch);
  }
}

/////////////////// DOKAN_IO_EVENT ///////////////////
PDOKAN_IO_EVENT PopIoEventBuffer() {
  PDOKAN_IO_EVENT ioEvent = NULL;
  EnterCriticalSection(&g_IoEventBufferCriticalSection);
  {
    if (DokanVector_GetCount(g_IoEventBufferPool) > 0) {
      ioEvent =
          *(PDOKAN_IO_EVENT *)DokanVector_GetLastItem(g_IoEventBufferPool);
      DokanVector_PopBack(g_IoEventBufferPool);
    }
  }
  LeaveCriticalSection(&g_IoEventBufferCriticalSection);
  if (!ioEvent) {
    ioEvent = (PDOKAN_IO_EVENT)malloc(sizeof(DOKAN_IO_EVENT));
  }
  if (ioEvent) {
    RtlZeroMemory(ioEvent, sizeof(DOKAN_IO_EVENT));
  }
  return ioEvent;
}

VOID PushIoEventBuffer(PDOKAN_IO_EVENT IoEvent) {
  assert(IoEvent);
  EnterCriticalSection(&g_IoEventBufferCriticalSection);
  {
    if (DokanVector_GetCount(g_IoEventBufferPool) < DOKAN_IO_EVENT_POOL_SIZE) {
      DokanVector_PushBack(g_IoEventBufferPool, &IoEvent);
      IoEvent = NULL;
    }
  }
  LeaveCriticalSection(&g_IoEventBufferCriticalSection);
  if (IoEvent) {
    FreeIoEventBuffer(IoEvent);
  }
}

/////////////////// EVENT_INFORMATION ///////////////////
PEVENT_INFORMATION PopEventResult() {
  PEVENT_INFORMATION eventResult = NULL;
  EnterCriticalSection(&g_EventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_EventResultPool) > 0) {
      eventResult =
          *(PEVENT_INFORMATION *)DokanVector_GetLastItem(g_EventResultPool);
      DokanVector_PopBack(g_EventResultPool);
    }
  }
  LeaveCriticalSection(&g_EventResultCriticalSection);
  if (!eventResult) {
    eventResult = (PEVENT_INFORMATION)malloc(DOKAN_EVENT_INFO_DEFAULT_SIZE);
  }
  if (eventResult) {
    RtlZeroMemory(eventResult, DOKAN_EVENT_INFO_DEFAULT_SIZE);
  }
  return eventResult;
}

VOID FreeEventResult(PEVENT_INFORMATION EventResult) {
  if (EventResult) {
    free(EventResult);
  }
}

VOID PushEventResult(PEVENT_INFORMATION EventResult) {
  assert(EventResult);
  EnterCriticalSection(&g_EventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_EventResultPool) < DOKAN_IO_EVENT_POOL_SIZE) {
      DokanVector_PushBack(g_EventResultPool, &EventResult);
      EventResult = NULL;
    }
  }
  LeaveCriticalSection(&g_EventResultCriticalSection);
  if (EventResult) {
    FreeEventResult(EventResult);
  }
}

/////////////////// EVENT_INFORMATION 16K ///////////////////
PEVENT_INFORMATION Pop16KEventResult() {
  PEVENT_INFORMATION eventResult = NULL;
  EnterCriticalSection(&g_16KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_16KEventResultPool) > 0) {
      eventResult =
          *(PEVENT_INFORMATION *)DokanVector_GetLastItem(g_16KEventResultPool);
      DokanVector_PopBack(g_16KEventResultPool);
    }
  }
  LeaveCriticalSection(&g_16KEventResultCriticalSection);
  if (!eventResult) {
    eventResult = (PEVENT_INFORMATION)malloc(DOKAN_EVENT_INFO_16K_SIZE);
  }
  if (eventResult) {
    RtlZeroMemory(eventResult, FIELD_OFFSET(EVENT_INFORMATION, Buffer));
  }
  return eventResult;
}

VOID Push16KEventResult(PEVENT_INFORMATION EventResult) {
  assert(EventResult);
  EnterCriticalSection(&g_16KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_16KEventResultPool) <
        DOKAN_IO_EXTRA_EVENT_POOL_SIZE) {
      DokanVector_PushBack(g_16KEventResultPool, &EventResult);
      EventResult = NULL;
    }
  }
  LeaveCriticalSection(&g_16KEventResultCriticalSection);
  if (EventResult) {
    FreeEventResult(EventResult);
  }
}

/////////////////// EVENT_INFORMATION 32K ///////////////////
PEVENT_INFORMATION Pop32KEventResult() {
  PEVENT_INFORMATION eventResult = NULL;
  EnterCriticalSection(&g_32KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_32KEventResultPool) > 0) {
      eventResult =
          *(PEVENT_INFORMATION *)DokanVector_GetLastItem(g_32KEventResultPool);
      DokanVector_PopBack(g_32KEventResultPool);
    }
  }
  LeaveCriticalSection(&g_32KEventResultCriticalSection);
  if (!eventResult) {
    eventResult = (PEVENT_INFORMATION)malloc(DOKAN_EVENT_INFO_32K_SIZE);
  }
  if (eventResult) {
    RtlZeroMemory(eventResult, FIELD_OFFSET(EVENT_INFORMATION, Buffer));
  }
  return eventResult;
}

VOID Push32KEventResult(PEVENT_INFORMATION EventResult) {
  assert(EventResult);
  EnterCriticalSection(&g_32KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_32KEventResultPool) <
        DOKAN_IO_EXTRA_EVENT_POOL_SIZE) {
      DokanVector_PushBack(g_32KEventResultPool, &EventResult);
      EventResult = NULL;
    }
  }
  LeaveCriticalSection(&g_32KEventResultCriticalSection);
  if (EventResult) {
    FreeEventResult(EventResult);
  }
}

/////////////////// EVENT_INFORMATION 64K ///////////////////
PEVENT_INFORMATION Pop64KEventResult() {
  PEVENT_INFORMATION eventResult = NULL;
  EnterCriticalSection(&g_64KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_64KEventResultPool) > 0) {
      eventResult =
          *(PEVENT_INFORMATION *)DokanVector_GetLastItem(g_64KEventResultPool);
      DokanVector_PopBack(g_64KEventResultPool);
    }
  }
  LeaveCriticalSection(&g_64KEventResultCriticalSection);
  if (!eventResult) {
    eventResult = (PEVENT_INFORMATION)malloc(DOKAN_EVENT_INFO_64K_SIZE);
  }
  if (eventResult) {
    RtlZeroMemory(eventResult, FIELD_OFFSET(EVENT_INFORMATION, Buffer));
  }
  return eventResult;
}

VOID Push64KEventResult(PEVENT_INFORMATION EventResult) {
  assert(EventResult);
  EnterCriticalSection(&g_64KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_64KEventResultPool) <
        DOKAN_IO_EXTRA_EVENT_POOL_SIZE) {
      DokanVector_PushBack(g_64KEventResultPool, &EventResult);
      EventResult = NULL;
    }
  }
  LeaveCriticalSection(&g_64KEventResultCriticalSection);
  if (EventResult) {
    FreeEventResult(EventResult);
  }
}

/////////////////// EVENT_INFORMATION 128K ///////////////////
PEVENT_INFORMATION Pop128KEventResult() {
  PEVENT_INFORMATION eventResult = NULL;
  EnterCriticalSection(&g_128KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_128KEventResultPool) > 0) {
      eventResult =
          *(PEVENT_INFORMATION *)DokanVector_GetLastItem(g_128KEventResultPool);
      DokanVector_PopBack(g_128KEventResultPool);
    }
  }
  LeaveCriticalSection(&g_128KEventResultCriticalSection);
  if (!eventResult) {
    eventResult = (PEVENT_INFORMATION)malloc(DOKAN_EVENT_INFO_128K_SIZE);
  }
  if (eventResult) {
    RtlZeroMemory(eventResult, FIELD_OFFSET(EVENT_INFORMATION, Buffer));
  }
  return eventResult;
}

VOID Push128KEventResult(PEVENT_INFORMATION EventResult) {
  assert(EventResult);
  EnterCriticalSection(&g_128KEventResultCriticalSection);
  {
    if (DokanVector_GetCount(g_128KEventResultPool) <
        DOKAN_IO_EXTRA_EVENT_POOL_SIZE) {
      DokanVector_PushBack(g_128KEventResultPool, &EventResult);
      EventResult = NULL;
    }
  }
  LeaveCriticalSection(&g_128KEventResultCriticalSection);
  if (EventResult) {
    FreeEventResult(EventResult);
  }
}

/////////////////// DOKAN_OPEN_INFO ///////////////////
PDOKAN_OPEN_INFO PopFileOpenInfo() {
  PDOKAN_OPEN_INFO fileInfo = NULL;
  EnterCriticalSection(&g_FileInfoCriticalSection);
  {
    if (DokanVector_GetCount(g_FileInfoPool) > 0) {
      fileInfo = *(PDOKAN_OPEN_INFO *)DokanVector_GetLastItem(g_FileInfoPool);
      DokanVector_PopBack(g_FileInfoPool);
    }
  }
  LeaveCriticalSection(&g_FileInfoCriticalSection);
  if (!fileInfo) {
    fileInfo = (PDOKAN_OPEN_INFO)malloc(sizeof(DOKAN_OPEN_INFO));
    if (!fileInfo) {
      DokanDbgPrint("Dokan Error: Failed to allocate DOKAN_OPEN_INFO.\n");
      return NULL;
    }
    RtlZeroMemory(fileInfo, sizeof(DOKAN_OPEN_INFO));
    InitializeCriticalSection(&fileInfo->CriticalSection);
  }
  if (fileInfo) {
    fileInfo->DokanInstance = NULL;
    fileInfo->DirList = NULL;
    fileInfo->DirListSearchPattern= NULL;
    fileInfo->UserContext = 0;
    fileInfo->EventId = 0;
    fileInfo->IsDirectory = FALSE;
    fileInfo->OpenCount = 0;
    fileInfo->CloseFileName = NULL;
    fileInfo->CloseUserContext = 0;
    fileInfo->EventContext = NULL;
  }
  return fileInfo;
}

VOID CleanupFileOpenInfo(PDOKAN_OPEN_INFO FileInfo) {
  assert(FileInfo);
  PDOKAN_VECTOR dirList = NULL;
  EnterCriticalSection(&FileInfo->CriticalSection);
  {
    if (FileInfo->DirListSearchPattern) {
      free(FileInfo->DirListSearchPattern);
      FileInfo->DirListSearchPattern = NULL;
    }

    if (FileInfo->DirList) {
      dirList = FileInfo->DirList;
      FileInfo->DirList = NULL;
    }
  }
  LeaveCriticalSection(&FileInfo->CriticalSection);
  if (dirList) {
    PushDirectoryList(dirList);
  }
}

VOID FreeFileOpenInfo(PDOKAN_OPEN_INFO FileInfo) {
  if (FileInfo) {
    CleanupFileOpenInfo(FileInfo);
    DeleteCriticalSection(&FileInfo->CriticalSection);
    free(FileInfo);
  }
}

VOID PushFileOpenInfo(PDOKAN_OPEN_INFO FileInfo) {
  assert(FileInfo);
  CleanupFileOpenInfo(FileInfo);
  EnterCriticalSection(&g_FileInfoCriticalSection);
  {
    if (DokanVector_GetCount(g_FileInfoPool) < DOKAN_IO_EVENT_POOL_SIZE) {
      DokanVector_PushBack(g_FileInfoPool, &FileInfo);
      FileInfo = NULL;
    }
  }
  LeaveCriticalSection(&g_FileInfoCriticalSection);
  if (FileInfo) {
    FreeFileOpenInfo(FileInfo);
  }
}

/////////////////// Directory list ///////////////////
PDOKAN_VECTOR PopDirectoryList() {
  PDOKAN_VECTOR directoryList = NULL;
  EnterCriticalSection(&g_DirectoryListCriticalSection);
  {
    if (DokanVector_GetCount(g_DirectoryListPool) > 0) {
      directoryList =
          *(PDOKAN_VECTOR *)DokanVector_GetLastItem(g_DirectoryListPool);
      DokanVector_PopBack(g_DirectoryListPool);
    }
  }
  LeaveCriticalSection(&g_DirectoryListCriticalSection);
  if (!directoryList) {
    directoryList = DokanVector_Alloc(sizeof(WIN32_FIND_DATAW));
  }
  if (directoryList) {
    DokanVector_Clear(directoryList);
  }
  return directoryList;
}

VOID PushDirectoryList(PDOKAN_VECTOR DirectoryList) {
  assert(DirectoryList);
  assert(DokanVector_GetItemSize(DirectoryList) == sizeof(WIN32_FIND_DATAW));
  EnterCriticalSection(&g_DirectoryListCriticalSection);
  {
    if (DokanVector_GetCount(g_DirectoryListPool) <
        DOKAN_DIRECTORY_LIST_POOL_SIZE) {
      DokanVector_PushBack(g_DirectoryListPool, &DirectoryList);
      DirectoryList = NULL;
    }
  }
  LeaveCriticalSection(&g_DirectoryListCriticalSection);
  if (DirectoryList) {
    DokanVector_Free(DirectoryList);
  }
}

/////////////////// Push/Pop pattern finished ///////////////////