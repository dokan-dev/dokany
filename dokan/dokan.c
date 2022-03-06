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

#include <conio.h>
#include <process.h>
#include <stdlib.h>
#include <tchar.h>
#include <strsafe.h>
#include <assert.h>

#define DokanMapKernelBit(dest, src, userBit, kernelBit)                       \
  if (((src) & (kernelBit)) == (kernelBit))                                    \
  (dest) |= (userBit)

// DokanOptions->DebugMode is ON?
BOOL g_DebugMode = TRUE;

// DokanOptions->UseStdErr is ON?
BOOL g_UseStdErr = FALSE;

// Dokan DLL critical section
CRITICAL_SECTION g_InstanceCriticalSection;

// Global linked list of mounted Dokan instances
LIST_ENTRY g_InstanceList;

volatile LONG g_DokanInitialized = 0;

VOID DOKANAPI DokanUseStdErr(BOOL Status) { g_UseStdErr = Status; }

VOID DOKANAPI DokanDebugMode(BOOL Status) { g_DebugMode = Status; }

VOID DispatchDriverLogs(PDOKAN_IO_EVENT IoEvent) {
  UNREFERENCED_PARAMETER(IoEvent);

  PDOKAN_LOG_MESSAGE log_message =
      (PDOKAN_LOG_MESSAGE)((PCHAR)IoEvent->EventContext +
                           sizeof(EVENT_CONTEXT));
  if (log_message->MessageLength) {
    ULONG paquet_size = FIELD_OFFSET(DOKAN_LOG_MESSAGE, Message[0]) +
                        log_message->MessageLength;
    if (((PCHAR)log_message + paquet_size) <=
        ((PCHAR)IoEvent->EventContext + IoEvent->EventContext->Length)) {
      DbgPrint("DriverLog: %.*s\n", log_message->MessageLength,
               log_message->Message);
    } else {
      DbgPrint("Invalid driver log message received.\n");
    }
  }
}

PDOKAN_INSTANCE
NewDokanInstance() {
  PDOKAN_INSTANCE dokanInstance =
      (PDOKAN_INSTANCE)malloc(sizeof(DOKAN_INSTANCE));
  if (dokanInstance == NULL)
    return NULL;

  ZeroMemory(dokanInstance, sizeof(DOKAN_INSTANCE));

  dokanInstance->GlobalDevice = INVALID_HANDLE_VALUE;
  dokanInstance->Device = INVALID_HANDLE_VALUE;
  dokanInstance->NotifyHandle = INVALID_HANDLE_VALUE;
  dokanInstance->KeepaliveHandle = INVALID_HANDLE_VALUE;

  (void)InitializeCriticalSectionAndSpinCount(&dokanInstance->CriticalSection,
                                              0x80000400);

  InitializeListHead(&dokanInstance->ListEntry);

  dokanInstance->DeviceClosedWaitHandle = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!dokanInstance->DeviceClosedWaitHandle) {
    DokanDbgPrint("Dokan Error: Cannot create Dokan instance because the "
                  "device closed wait handle could not be created.\n");
    DeleteCriticalSection(&dokanInstance->CriticalSection);
    free(dokanInstance);
    return NULL;
  }

  EnterCriticalSection(&g_InstanceCriticalSection);
  {
    PTP_POOL threadPool = GetThreadPool();
    if (!threadPool) {
      DokanDbgPrint("Dokan Error: Cannot create Dokan instance because the "
                    "thread pool hasn't been created.\n");
      LeaveCriticalSection(&g_InstanceCriticalSection);
      DeleteCriticalSection(&dokanInstance->CriticalSection);
      CloseHandle(dokanInstance->DeviceClosedWaitHandle);
      free(dokanInstance);
      return NULL;
    }

    dokanInstance->ThreadInfo.ThreadPool = threadPool;
    dokanInstance->ThreadInfo.CleanupGroup = CreateThreadpoolCleanupGroup();
    if (!dokanInstance->ThreadInfo.CleanupGroup) {
      DokanDbgPrint(
          "Dokan Error: Failed to create thread pool cleanup group.\n");
      LeaveCriticalSection(&g_InstanceCriticalSection);
      DeleteCriticalSection(&dokanInstance->CriticalSection);
      CloseHandle(dokanInstance->DeviceClosedWaitHandle);
      free(dokanInstance);
      return NULL;
    }
    InitializeThreadpoolEnvironment(
        &dokanInstance->ThreadInfo.CallbackEnvironment);
    SetThreadpoolCallbackPool(&dokanInstance->ThreadInfo.CallbackEnvironment,
                              threadPool);
    SetThreadpoolCallbackCleanupGroup(
        &dokanInstance->ThreadInfo.CallbackEnvironment,
        dokanInstance->ThreadInfo.CleanupGroup, NULL);
    InsertTailList(&g_InstanceList, &dokanInstance->ListEntry);
  }
  LeaveCriticalSection(&g_InstanceCriticalSection);
  return dokanInstance;
}

VOID DeleteDokanInstance(PDOKAN_INSTANCE DokanInstance) {
  SetEvent(DokanInstance->DeviceClosedWaitHandle);
  if (DokanInstance->ThreadInfo.CleanupGroup) {
    CloseThreadpoolCleanupGroupMembers(DokanInstance->ThreadInfo.CleanupGroup,
                                       FALSE, DokanInstance);
    CloseThreadpoolCleanupGroup(DokanInstance->ThreadInfo.CleanupGroup);
    DokanInstance->ThreadInfo.CleanupGroup = NULL;
    DestroyThreadpoolEnvironment(
        &DokanInstance->ThreadInfo.CallbackEnvironment);
  }
  if (DokanInstance->NotifyHandle &&
      DokanInstance->NotifyHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(DokanInstance->NotifyHandle);
  }
  if (DokanInstance->KeepaliveHandle &&
      DokanInstance->KeepaliveHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(DokanInstance->KeepaliveHandle);
  }
  if (DokanInstance->Device && DokanInstance->Device != INVALID_HANDLE_VALUE) {
    CloseHandle(DokanInstance->Device);
  }
  if (DokanInstance->GlobalDevice &&
      DokanInstance->GlobalDevice != INVALID_HANDLE_VALUE) {
    CloseHandle(DokanInstance->GlobalDevice);
  }
  DeleteCriticalSection(&DokanInstance->CriticalSection);
  EnterCriticalSection(&g_InstanceCriticalSection);
  { RemoveEntryList(&DokanInstance->ListEntry); }
  LeaveCriticalSection(&g_InstanceCriticalSection);
  CloseHandle(DokanInstance->DeviceClosedWaitHandle);
  free(DokanInstance);
}

BOOL IsMountPointDriveLetter(LPCWSTR mountPoint) {
  size_t mountPointLength;

  if (!mountPoint || *mountPoint == 0) {
    return FALSE;
  }

  mountPointLength = wcslen(mountPoint);

  if (mountPointLength == 1 ||
      (mountPointLength == 2 && mountPoint[1] == L':') ||
      (mountPointLength == 3 && mountPoint[1] == L':' &&
       mountPoint[2] == L'\\')) {

    return TRUE;
  }

  return FALSE;
}

BOOL IsValidDriveLetter(WCHAR DriveLetter) {
  return (L'a' <= DriveLetter && DriveLetter <= L'z') ||
         (L'A' <= DriveLetter && DriveLetter <= L'Z');
}

BOOL CheckDriveLetterAvailability(WCHAR DriveLetter) {
  DWORD result = 0;
  WCHAR buffer[MAX_PATH];
  WCHAR dosDevice[] = L"\\\\.\\C:";
  WCHAR driveName[] = L"C:";
  WCHAR driveLetter = towupper(DriveLetter);
  HANDLE device = NULL;
  dosDevice[4] = driveLetter;
  driveName[0] = driveLetter;

  DokanMountPointsCleanUp();

  if (!IsValidDriveLetter(driveLetter)) {
    DbgPrintW(L"CheckDriveLetterAvailability failed, bad drive letter %c\n",
              DriveLetter);
    return FALSE;
  }

  device = CreateFile(dosDevice, GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                      FILE_FLAG_NO_BUFFERING, NULL);

  if (device != INVALID_HANDLE_VALUE) {
    DbgPrintW(L"CheckDriveLetterAvailability failed, %c: is already used\n",
              DriveLetter);
    CloseHandle(device);
    return FALSE;
  }

  ZeroMemory(buffer, MAX_PATH * sizeof(WCHAR));
  result = QueryDosDevice(driveName, buffer, MAX_PATH);
  if (result > 0) {
    DbgPrintW(L"CheckDriveLetterAvailability failed, QueryDosDevice - Drive "
              L"letter \"%c\" is already used.\n",
              DriveLetter);
    return FALSE;
  }

  DWORD drives = GetLogicalDrives();
  result = (drives >> (driveLetter - L'A') & 0x00000001);
  if (result > 0) {
    DbgPrintW(L"CheckDriveLetterAvailability failed, GetLogicalDrives - Drive "
              L"letter \"%c\" is already used.\n",
              DriveLetter);
    return FALSE;
  }

  return TRUE;
}

VOID CheckAllocationUnitSectorSize(PDOKAN_OPTIONS DokanOptions) {
  ULONG allocationUnitSize = DokanOptions->AllocationUnitSize;
  ULONG sectorSize = DokanOptions->SectorSize;

  if ((allocationUnitSize < 512 || allocationUnitSize > 65536 ||
       (allocationUnitSize & (allocationUnitSize - 1)) != 0) // Is power of two
      || (sectorSize < 512 || sectorSize > 65536 ||
          (sectorSize & (sectorSize - 1)))) { // Is power of two
    // Reset to default if values does not fit windows FAT/NTFS value
    // https://support.microsoft.com/en-us/kb/140365
    DokanOptions->SectorSize = DOKAN_DEFAULT_SECTOR_SIZE;
    DokanOptions->AllocationUnitSize = DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
  }

  DbgPrintW(L"AllocationUnitSize: %d SectorSize: %d\n",
            DokanOptions->AllocationUnitSize, DokanOptions->SectorSize);
}

VOID SetupIOEventForProcessing(PDOKAN_IO_EVENT IoEvent) {
  IoEvent->DokanOpenInfo =
      (PDOKAN_OPEN_INFO)(UINT_PTR)IoEvent->EventContext->Context;
  IoEvent->DokanFileInfo.DokanContext = (ULONG64)IoEvent;
  IoEvent->DokanFileInfo.ProcessId = IoEvent->EventContext->ProcessId;
  IoEvent->DokanFileInfo.DokanOptions = IoEvent->DokanInstance->DokanOptions;

  if (IoEvent->DokanOpenInfo) {
    EnterCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
    IoEvent->DokanOpenInfo->OpenCount++;
    IoEvent->DokanFileInfo.Context = IoEvent->DokanOpenInfo->UserContext;
    LeaveCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
    IoEvent->DokanFileInfo.IsDirectory =
        (UCHAR)IoEvent->DokanOpenInfo->IsDirectory;

    if (IoEvent->EventContext->FileFlags & DOKAN_DELETE_ON_CLOSE) {
      IoEvent->DokanFileInfo.DeleteOnClose = 1;
    }
    if (IoEvent->EventContext->FileFlags & DOKAN_PAGING_IO) {
      IoEvent->DokanFileInfo.PagingIo = 1;
    }
    if (IoEvent->EventContext->FileFlags & DOKAN_WRITE_TO_END_OF_FILE) {
      IoEvent->DokanFileInfo.WriteToEndOfFile = 1;
    }
    if (IoEvent->EventContext->FileFlags & DOKAN_SYNCHRONOUS_IO) {
      IoEvent->DokanFileInfo.SynchronousIo = 1;
    }
    if (IoEvent->EventContext->FileFlags & DOKAN_NOCACHE) {
      IoEvent->DokanFileInfo.Nocache = 1;
    }
  }
  assert(IoEvent->EventResult == NULL);
}

VOID DispatchEvent(PDOKAN_IO_EVENT ioEvent) {
  SetupIOEventForProcessing(ioEvent);
  switch (ioEvent->EventContext->MajorFunction) {
  case IRP_MJ_CREATE:
    DispatchCreate(ioEvent);
    break;
  case IRP_MJ_CLEANUP:
    DispatchCleanup(ioEvent);
    break;
  case IRP_MJ_CLOSE:
    DispatchClose(ioEvent);
    break;
  case IRP_MJ_DIRECTORY_CONTROL:
    DispatchDirectoryInformation(ioEvent);
    break;
  case IRP_MJ_READ:
    DispatchRead(ioEvent);
    break;
  case IRP_MJ_WRITE:
    DispatchWrite(ioEvent);
    break;
  case IRP_MJ_QUERY_INFORMATION:
    DispatchQueryInformation(ioEvent);
    break;
  case IRP_MJ_QUERY_VOLUME_INFORMATION:
    DispatchQueryVolumeInformation(ioEvent);
    break;
  case IRP_MJ_LOCK_CONTROL:
    DispatchLock(ioEvent);
    break;
  case IRP_MJ_SET_INFORMATION:
    DispatchSetInformation(ioEvent);
    break;
  case IRP_MJ_FLUSH_BUFFERS:
    DispatchFlush(ioEvent);
    break;
  case IRP_MJ_QUERY_SECURITY:
    DispatchQuerySecurity(ioEvent);
    break;
  case IRP_MJ_SET_SECURITY:
    DispatchSetSecurity(ioEvent);
    break;
  case DOKAN_IRP_LOG_MESSAGE:
    DispatchDriverLogs(ioEvent);
    break;
  default:
    DokanDbgPrintW(L"Dokan Warning: Unsupported IRP 0x%x, event Info = 0x%p.\n",
                   ioEvent->EventContext->MajorFunction, ioEvent->EventContext);
    PushIoEventBuffer(ioEvent);
    break;
  }
}

VOID OnDeviceIoCtlFailed(PDOKAN_INSTANCE DokanInstance, DWORD Result) {
  if (!DokanInstance->FileSystemStopped) {
    DokanDbgPrintW(L"Dokan Fatal: Closing IO processing for dokan instance %s "
                   L"with error code 0x%x and unmounting volume.\n",
                   DokanInstance->DeviceName, Result);
  }

  if (InterlockedAdd(&DokanInstance->UnmountedCalled, 1) == 1) {
    DokanNotifyUnmounted(DokanInstance);
  }

  // set the device to a closed state
  SetEvent(DokanInstance->DeviceClosedWaitHandle);
}

// Don't know what went wrong
// Life will never be the same again
// End it all
VOID HandleProcessIoFatalError(PDOKAN_INSTANCE DokanInstance,
                               PDOKAN_IO_BATCH IoBatch, DWORD Result) {
  PushIoBatchBuffer(IoBatch);
  OnDeviceIoCtlFailed(DokanInstance, Result);
}

VOID FreeIoEventResult(PEVENT_INFORMATION EventResult, ULONG EventResultSize,
                       BOOL PoolAllocated) {
  if (EventResult) {
    if (PoolAllocated) {
      if (EventResultSize <= DOKAN_EVENT_INFO_DEFAULT_SIZE) {
        PushEventResult(EventResult);
      } else if (EventResultSize <= DOKAN_EVENT_INFO_16K_SIZE) {
        Push16KEventResult(EventResult);
      } else if (EventResultSize <= DOKAN_EVENT_INFO_32K_SIZE) {
        Push32KEventResult(EventResult);
      } else if (EventResultSize <= DOKAN_EVENT_INFO_64K_SIZE) {
        Push64KEventResult(EventResult);
      } else if (EventResultSize <= DOKAN_EVENT_INFO_128K_SIZE) {
        Push128KEventResult(EventResult);
      } else {
        assert(FALSE);
      }
    } else {
      FreeEventResult(EventResult);
    }
  }
}

VOID QueueIoEvent(PDOKAN_IO_EVENT IoEvent, PTP_WORK_CALLBACK Callback) {
  PTP_WORK work = CreateThreadpoolWork(
      Callback, IoEvent,
      &IoEvent->DokanInstance->ThreadInfo.CallbackEnvironment);
  if (!work) {
    DWORD lastError = GetLastError();
    DbgPrintW(L"Dokan Error: CreateThreadpoolWork() has returned error "
              L"code %u.\n",
              lastError);
    OnDeviceIoCtlFailed(IoEvent->DokanInstance, lastError);
    return;
  }
  SubmitThreadpoolWork(work);
}

DWORD
GetEventInfoSize(__in ULONG MajorFunction, __in PEVENT_INFORMATION EventInfo) {
  if (MajorFunction == IRP_MJ_WRITE) {
    // For writes only, the reply is a fixed size and the BufferLength inside it
    // is the "bytes written" value as opposed to the reply size.
    return sizeof(EVENT_INFORMATION);
  }
  return (DWORD)max((ULONG)sizeof(EVENT_INFORMATION),
                    FIELD_OFFSET(EVENT_INFORMATION, Buffer[0]) +
                        EventInfo->BufferLength);
}

DWORD SendAndPullEventInformation(PDOKAN_IO_EVENT IoEvent,
                                  PDOKAN_IO_BATCH IoBatch,
                                  BOOL ReleaseBatchBuffers) {
  DWORD lastError = 0;
  PCHAR inputBuffer = NULL;
  DWORD eventInfoSize = 0;
  ULONG eventResultSize = 0;
  PEVENT_INFORMATION eventInfo = NULL;
  BOOL eventInfoPollAllocated = FALSE;

  if (IoEvent && IoEvent->EventResult) {
    eventInfo = IoEvent->EventResult;
    eventResultSize = IoEvent->EventResultSize;
    eventInfoPollAllocated = IoEvent->PoolAllocated;
    inputBuffer = (PCHAR)eventInfo;
    eventInfoSize =
        GetEventInfoSize(IoEvent->EventContext->MajorFunction, eventInfo);
    eventInfo->PullEventTimeoutMs =
        IoBatch->MainPullThread ? /*infinite*/ 0 : DOKAN_PULL_EVENT_TIMEOUT_MS;
    if (ReleaseBatchBuffers) {
      PushIoBatchBuffer(IoEvent->IoBatch);
      PushIoEventBuffer(IoEvent);
    }
    DbgPrint(
        "Dokan Information: SendAndPullEventInformation() with NTSTATUS 0x%x, "
        "context 0x%lx, and result object 0x%p with size %d\n",
        eventInfo->Status, eventInfo->Context, eventInfo, eventInfoSize);
  } else {
    // Main pull thread is allowed to pull events without having event results to send
    assert(IoBatch->MainPullThread);
  }

  if (!DeviceIoControl(
          IoBatch->DokanInstance->Device, // Handle to device
          FSCTL_EVENT_PROCESS_N_PULL,     // IO Control code
          inputBuffer,                    // Input Buffer to driver.
          eventInfoSize,                  // Length of input buffer in bytes.
          &IoBatch->EventContext[0],      // Output Buffer from driver.
          BATCH_EVENT_CONTEXT_SIZE,       // Length of output buffer in bytes.
          &IoBatch->NumberOfBytesTransferred, // Bytes placed in buffer.
          NULL                                // asynchronous call
          )) {
    lastError = GetLastError();
    if (eventInfo) {
      FreeIoEventResult(eventInfo, eventResultSize, eventInfoPollAllocated);
    }
    if (!IoBatch->DokanInstance->FileSystemStopped) {
      DokanDbgPrintW(
          L"Dokan Error: Dokan device result ioctl failed for wait with "
          L"code %d.\n",
          lastError);
    }
    return lastError;
  }
  if (eventInfo) {
    FreeIoEventResult(eventInfo, eventResultSize, eventInfoPollAllocated);
  }
  return 0;
}

VOID CALLBACK DispatchBatchIoCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Parameter,
                               PTP_WORK Work) {
  UNREFERENCED_PARAMETER(Instance);
  UNREFERENCED_PARAMETER(Work);

  PDOKAN_IO_EVENT ioEvent = (PDOKAN_IO_EVENT)Parameter;
  assert(ioEvent);
  PDOKAN_INSTANCE dokanInstance = ioEvent->DokanInstance;
  PDOKAN_IO_BATCH ioBatch = NULL;
  BOOL mainPullThread = ioEvent->EventContext == NULL;

  while (TRUE) {
    // 6 - Process events coming from:
    // - Last event not dispatched to the pool (see bottom of this fct).
    // - New pool thread that just started with a dispatched event.
    // Note: Main pull thread does not have an EventContext when started.
    if (ioEvent && ioEvent->EventContext) {
      DispatchEvent(ioEvent);
      if (!ioEvent->EventResult) {
        // Some events like Close() do not have event results.
        // Release the resource and terminate here unless we are the main pulling thread.
        PushIoBatchBuffer(ioEvent->IoBatch);
        PushIoEventBuffer(ioEvent);
        if (mainPullThread) {
          ioEvent = NULL;
          continue;
        }
        return;
      }
    }

    ioBatch = PopIoBatchBuffer();
    ioBatch->MainPullThread = mainPullThread;
    ioBatch->DokanInstance = dokanInstance;

    // 1 - Send event result and pull new events.
    DWORD error = SendAndPullEventInformation(ioEvent, ioBatch, /*ReleaseBatchBuffers=*/TRUE);
    if (error) {
      HandleProcessIoFatalError(dokanInstance, ioBatch, error);
      return;
    }

    // 2 - Terminate thread as nothing needs to be proceed unless we are the mainPullThread.
    if (!ioBatch->NumberOfBytesTransferred) {
      PushIoBatchBuffer(ioBatch);
      if (mainPullThread) {
        ioEvent = NULL;
        continue;
      }
      return;
    }

    PEVENT_CONTEXT context = ioBatch->EventContext;
    ULONG_PTR currentNumberOfBytesTransferred =
        ioBatch->NumberOfBytesTransferred;
    while (currentNumberOfBytesTransferred) {
      ++ioBatch->EventContextBatchCount;
      currentNumberOfBytesTransferred -= context->Length;
      context = (PEVENT_CONTEXT)((PCHAR)(context) + context->Length);
    }
    // 3 - Dispatch Events
    context = ioBatch->EventContext;
    LONG eventContextBatchCount = ioBatch->EventContextBatchCount;
    while (eventContextBatchCount) {
      ioEvent = PopIoEventBuffer();
      if (!ioEvent) {
        DbgPrintW(L"Dokan Error: IoEvent allocation failed.\n");
        OnDeviceIoCtlFailed(ioBatch->DokanInstance, ERROR_OUTOFMEMORY);
        return;
      }
      ioEvent->DokanInstance = ioBatch->DokanInstance;
      ioEvent->EventContext = context;
      ioEvent->IoBatch = ioBatch;
      --eventContextBatchCount;
      // It is unsafe to access the context from here after Queuing the event.
      context = (PEVENT_CONTEXT)((PCHAR)(context) + context->Length);
      // 4 - All batched events are dispatched to the thread pool except the last event that is executed on the current thread.
      // Note: Single thread mode has batching disabled and therefore only has one event which is executed on the main thread.
      if (eventContextBatchCount) {
        QueueIoEvent(ioEvent, DispatchBatchIoCallback);
      }
    }
  }
}

VOID CALLBACK DispatchDedicatedIoCallback(PTP_CALLBACK_INSTANCE Instance,
                                          PVOID Parameter, PTP_WORK Work) {
  UNREFERENCED_PARAMETER(Instance);
  UNREFERENCED_PARAMETER(Work);

  PDOKAN_IO_EVENT ioEvent = (PDOKAN_IO_EVENT)Parameter;
  assert(ioEvent);
  PDOKAN_IO_BATCH ioBatch = PopIoBatchBuffer();
  ioBatch->MainPullThread = TRUE;
  ioBatch->DokanInstance = ioEvent->DokanInstance;
  ioEvent->EventContext = ioBatch->EventContext;
  ioEvent->IoBatch = ioBatch;

  while (TRUE) {
    // 1 - Send possible event result and pull new events.
    DWORD error =
        SendAndPullEventInformation(ioEvent, ioBatch, /*ReleaseBatchBuffers=*/FALSE);
    if (error) {
      PushIoEventBuffer(ioEvent);
      HandleProcessIoFatalError(ioBatch->DokanInstance, ioBatch, error);
      return;
    }
    RtlZeroMemory(ioEvent, sizeof(DOKAN_IO_EVENT));
    ioEvent->DokanInstance = ioBatch->DokanInstance;
    ioEvent->EventContext = ioBatch->EventContext;
    ioEvent->IoBatch = ioBatch;
    // 2 - Restart pulling as there is nothing to process.
    if (!ioBatch->NumberOfBytesTransferred) {
      continue;
    }
    // 3 - Process event
    DispatchEvent(ioEvent);
  }
}

BOOL DOKANAPI DokanIsFileSystemRunning(_In_ DOKAN_HANDLE DokanInstance) {
  DOKAN_INSTANCE *instance = (DOKAN_INSTANCE *)DokanInstance;
  if (!instance) {
    return FALSE;
  }
  return WaitForSingleObject(instance->DeviceClosedWaitHandle, 0) ==
                 WAIT_TIMEOUT
             ? TRUE
             : FALSE;
}

DWORD DOKANAPI DokanWaitForFileSystemClosed(_In_ DOKAN_HANDLE DokanInstance,
                                            _In_ DWORD dwMilliseconds) {
  DOKAN_INSTANCE *instance = (DOKAN_INSTANCE *)DokanInstance;
  if (!instance) {
    return FALSE;
  }
  return WaitForSingleObject(instance->DeviceClosedWaitHandle, dwMilliseconds);
}

VOID DOKANAPI DokanCloseHandle(_In_ DOKAN_HANDLE DokanInstance) {
  DOKAN_INSTANCE *instance = (DOKAN_INSTANCE *)DokanInstance;
  if (!instance) {
    return;
  }
  // make sure the driver is unmounted
  instance->FileSystemStopped = TRUE;
  DokanRemoveMountPoint(instance->MountPoint);
  DokanWaitForFileSystemClosed((DOKAN_HANDLE)instance, INFINITE);
  EnterCriticalSection(&g_InstanceCriticalSection);
  DeleteDokanInstance(instance);
  LeaveCriticalSection(&g_InstanceCriticalSection);
}

int DOKANAPI DokanMain(PDOKAN_OPTIONS DokanOptions,
                       PDOKAN_OPERATIONS DokanOperations) {
  DOKAN_INSTANCE *instance = NULL;
  int returnCode;
  returnCode = DokanCreateFileSystem(DokanOptions, DokanOperations,
                                     (DOKAN_HANDLE *)&instance);
  if (returnCode != DOKAN_SUCCESS) {
    return returnCode;
  }
  DokanWaitForFileSystemClosed((DOKAN_HANDLE)instance, INFINITE);
  DeleteDokanInstance(instance);
  return returnCode;
}

int DOKANAPI DokanCreateFileSystem(_In_ PDOKAN_OPTIONS DokanOptions,
                                   _In_ PDOKAN_OPERATIONS DokanOperations,
                                   _Out_ DOKAN_HANDLE *DokanInstance) {
  PDOKAN_INSTANCE dokanInstance;
  WCHAR rawDeviceName[MAX_PATH];

  if (DokanInstance) {
    *DokanInstance = NULL;
  }

  if (InterlockedAdd(&g_DokanInitialized, 0) <= 0) {
    RaiseException(DOKAN_EXCEPTION_NOT_INITIALIZED, 0, 0, NULL);
  }

  g_DebugMode = DokanOptions->Options & DOKAN_OPTION_DEBUG;
  g_UseStdErr = DokanOptions->Options & DOKAN_OPTION_STDERR;

  if (g_DebugMode) {
    DbgPrintW(L"Dokan: debug mode on\n");
  }

  if (g_UseStdErr) {
    DbgPrintW(L"Dokan: use stderr\n");
    g_DebugMode = TRUE;
  }

  if ((DokanOptions->Options & DOKAN_OPTION_NETWORK) &&
      !IsMountPointDriveLetter(DokanOptions->MountPoint)) {
    DokanOptions->Options &= ~DOKAN_OPTION_NETWORK;
    DbgPrintW(L"Dokan: Mount point folder is specified with network device "
              L"option. Disable network device.\n");
  }

  if ((DokanOptions->Options & DOKAN_OPTION_NETWORK) &&
      DokanOptions->UNCName == NULL) {
    DbgPrintW(L"Dokan: Network filesystem is enabled without UNC name.\n");
    return DOKAN_MOUNT_POINT_ERROR;
  }

  if (DokanOptions->Version < DOKAN_MINIMUM_COMPATIBLE_VERSION) {
    DokanDbgPrintW(
        L"Dokan Error: Incompatible version (%d), minimum is (%d) \n",
        DokanOptions->Version, DOKAN_MINIMUM_COMPATIBLE_VERSION);
    return DOKAN_VERSION_ERROR;
  }

  if (DokanOptions->SingleThread) {
    DbgPrintW(L"Dokan Info: Single thread mode enabled.\n");
  }

  CheckAllocationUnitSectorSize(DokanOptions);
  dokanInstance = NewDokanInstance();
  if (!dokanInstance) {
    return DOKAN_DRIVER_INSTALL_ERROR;
  }

  dokanInstance->DokanOptions = DokanOptions;
  dokanInstance->DokanOperations = DokanOperations;
  dokanInstance->GlobalDevice =
      CreateFile(DOKAN_GLOBAL_DEVICE_NAME,           // lpFileName
                 0,                                  // dwDesiredAccess
                 FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
                 NULL,                               // lpSecurityAttributes
                 OPEN_EXISTING,                      // dwCreationDistribution
                 0,                                  // dwFlagsAndAttributes
                 NULL                                // hTemplateFile
      );
  if (dokanInstance->GlobalDevice == INVALID_HANDLE_VALUE) {
    DWORD lastError = GetLastError();
    DokanDbgPrintW(L"Dokan Error: CreatFile failed to open %s: %d\n",
                   DOKAN_GLOBAL_DEVICE_NAME, lastError);
    DeleteDokanInstance(dokanInstance);
    return DOKAN_DRIVER_INSTALL_ERROR;
  }

  DbgPrint("Global device opened\n");
  if (DokanOptions->MountPoint != NULL) {
    wcscpy_s(dokanInstance->MountPoint,
             sizeof(dokanInstance->MountPoint) / sizeof(WCHAR),
             DokanOptions->MountPoint);
    // When mount manager is enabled we will try to release the busy letter if we own it or get one assigned but otherwise we just fail here.
    if (!(DokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) &&
        IsMountPointDriveLetter(dokanInstance->MountPoint) &&
        !CheckDriveLetterAvailability(dokanInstance->MountPoint[0])) {
      DokanDbgPrint("Dokan Error: CheckDriveLetterAvailability Failed\n");
      DeleteDokanInstance(dokanInstance);
      return DOKAN_MOUNT_ERROR;
    }
  }

  if (DokanOptions->UNCName != NULL) {
    wcscpy_s(dokanInstance->UNCName, sizeof(dokanInstance->UNCName) / sizeof(WCHAR),
             DokanOptions->UNCName);
  }

  int result = DokanStart(dokanInstance);
  if (result != DOKAN_SUCCESS) {
    DeleteDokanInstance(dokanInstance);
    return result;
  }

  GetRawDeviceName(dokanInstance->DeviceName, rawDeviceName, MAX_PATH);
  dokanInstance->Device =
      CreateFile(rawDeviceName,                      // lpFileName
                 0,                                  // dwDesiredAccess
                 FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
                 NULL,                               // lpSecurityAttributes
                 OPEN_EXISTING,                      // dwCreationDistribution
                 FILE_FLAG_OVERLAPPED,               // dwFlagsAndAttributes
                 NULL                                // hTemplateFile
      );
  if (dokanInstance->Device == INVALID_HANDLE_VALUE) {
    DWORD lastError = GetLastError();
    DokanDbgPrintW(L"Dokan Error: CreatFile failed to open %s: %d\n",
                   rawDeviceName, lastError);
    DeleteDokanInstance(dokanInstance);
    return DOKAN_DRIVER_INSTALL_ERROR;
  }

  DWORD_PTR processAffinityMask;
  DWORD_PTR systemAffinityMask;
  DWORD mainPullThreadCount = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinityMask,
                             &systemAffinityMask)) {
    while (processAffinityMask) {
      mainPullThreadCount += 1;
      processAffinityMask >>= 1;
    }
  } else {
    DbgPrintW(L"Dokan Error: GetProcessAffinityMask failed with Error %d\n",
              GetLastError());
  }
  if (DokanOptions->SingleThread) {
    mainPullThreadCount = 1; // Really not recommanded
    DokanOptions->Options &= ~DOKAN_OPTION_ALLOW_IPC_BATCHING;
  } else if (mainPullThreadCount < DOKAN_MAIN_PULL_THREAD_COUNT_MIN) {
    mainPullThreadCount = DOKAN_MAIN_PULL_THREAD_COUNT_MIN;
  } else if (mainPullThreadCount > DOKAN_MAIN_PULL_THREAD_COUNT_MAX) {
    // Thread pool will allocate more threads when pulling batched events
    DokanOptions->Options |= DOKAN_OPTION_ALLOW_IPC_BATCHING;
    mainPullThreadCount = DOKAN_MAIN_PULL_THREAD_COUNT_MAX;
  }
  DbgPrintW(L"Dokan: Using %d main pull threads\n", mainPullThreadCount);
  for (DWORD x = 0; x < mainPullThreadCount; ++x) {
    PDOKAN_IO_EVENT ioEvent = PopIoEventBuffer();
    if (!ioEvent) {
      DokanDbgPrintW(L"Dokan Error: IoEvent allocation failed.");
      DeleteDokanInstance(dokanInstance);
      return DOKAN_MOUNT_ERROR;
    }
    ioEvent->DokanInstance = dokanInstance;
    QueueIoEvent(ioEvent, DokanOptions->Options & DOKAN_OPTION_ALLOW_IPC_BATCHING
                              ? DispatchBatchIoCallback
                              : DispatchDedicatedIoCallback);
  }

  if (!DokanMount(dokanInstance->MountPoint, dokanInstance->DeviceName, DokanOptions)) {
    SendReleaseIRP(dokanInstance->DeviceName);
    DokanDbgPrint("Dokan Error: DokanMount Failed\n");
    DeleteDokanInstance(dokanInstance);
    return DOKAN_MOUNT_ERROR;
  }

  wchar_t keepalive_path[128];
  StringCbPrintfW(keepalive_path, sizeof(keepalive_path), L"\\\\?%s%s",
                  dokanInstance->DeviceName, DOKAN_KEEPALIVE_FILE_NAME);
  dokanInstance->KeepaliveHandle =
      CreateFile(keepalive_path, 0, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (dokanInstance->KeepaliveHandle == INVALID_HANDLE_VALUE) {
    // We don't consider this a fatal error because the keepalive handle is only
    // needed for abnormal termination cases anyway.
    DbgPrintW(L"Failed to open keepalive file: %s error %d\n", keepalive_path,
              GetLastError());
  } else {
    DWORD keepalive_bytes_returned = 0;
    if (!DeviceIoControl(dokanInstance->KeepaliveHandle, FSCTL_ACTIVATE_KEEPALIVE,
                         NULL, 0, NULL, 0, &keepalive_bytes_returned, NULL))
      DbgPrintW(L"Failed to activate keepalive handle.\n");
  }

  wchar_t notify_path[128];
  StringCbPrintfW(notify_path, sizeof(notify_path), L"\\\\?%s%s",
                  dokanInstance->DeviceName, DOKAN_NOTIFICATION_FILE_NAME);
  dokanInstance->NotifyHandle = CreateFile(
      notify_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (dokanInstance->NotifyHandle == INVALID_HANDLE_VALUE) {
    DbgPrintW(L"Failed to open notify handle: %s\n", notify_path);
  }

  // Here we should have been mounter by mountmanager thanks to
  // IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME
  DbgPrintW(L"Dokan Information: mounted: %s -> %s\n", dokanInstance->MountPoint,
            dokanInstance->DeviceName);

  if (DokanOperations->Mounted) {
    DOKAN_FILE_INFO fileInfo;
    RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));
    fileInfo.DokanOptions = DokanOptions;
    // Ignore return value
    DokanOperations->Mounted(dokanInstance->MountPoint, &fileInfo);
  }

  if (DokanInstance) {
    *DokanInstance = dokanInstance;
  }
  return DOKAN_SUCCESS;
}

VOID GetRawDeviceName(LPCWSTR DeviceName, LPWSTR DestinationBuffer,
                      rsize_t DestinationBufferSizeInElements) {
  if (DeviceName && DestinationBuffer && DestinationBufferSizeInElements > 0) {
    wcscpy_s(DestinationBuffer, DestinationBufferSizeInElements, L"\\\\.");
    wcscat_s(DestinationBuffer, DestinationBufferSizeInElements, DeviceName);
  }
}

VOID ALIGN_ALLOCATION_SIZE(PLARGE_INTEGER size, PDOKAN_OPTIONS DokanOptions) {
  long long r = size->QuadPart % DokanOptions->AllocationUnitSize;
  size->QuadPart =
      (size->QuadPart + (r > 0 ? DokanOptions->AllocationUnitSize - r : 0));
}

VOID EventCompletion(PDOKAN_IO_EVENT IoEvent) {
  assert(IoEvent->EventResult);
  ReleaseDokanOpenInfo(IoEvent);
}

VOID CheckFileName(LPWSTR FileName) {
  size_t len = wcslen(FileName);
  // if the beginning of file name is "\\",
  // replace it with "\"
  if (len >= 2 && FileName[0] == L'\\' && FileName[1] == L'\\') {
    int i;
    for (i = 0; FileName[i + 1] != L'\0'; ++i) {
      FileName[i] = FileName[i + 1];
    }
    FileName[i] = L'\0';
  }

  // Remove "\" in front of Directory
  len = wcslen(FileName);
  if (len > 2 && FileName[len - 1] == L'\\')
    FileName[len - 1] = '\0';
}

ULONG DispatchGetEventInformationLength(ULONG bufferSize) {
  // EVENT_INFORMATION has a buffer of size 8 already
  // we remote it to the struct size and add the requested buffer size
  // but we need at least to have enough space to set EVENT_INFORMATION
  return max((ULONG)sizeof(EVENT_INFORMATION),
             FIELD_OFFSET(EVENT_INFORMATION, Buffer[0]) + bufferSize);
}

VOID CreateDispatchCommon(PDOKAN_IO_EVENT IoEvent, ULONG SizeOfEventInfo, BOOL UseExtraMemoryPool, BOOL ClearNonPoolBuffer) {
  assert(IoEvent != NULL);
  assert(IoEvent->EventResult == NULL && IoEvent->EventResultSize == 0);

  if (SizeOfEventInfo <= DOKAN_EVENT_INFO_DEFAULT_BUFFER_SIZE) {
    IoEvent->EventResult = PopEventResult();
    IoEvent->EventResultSize = DOKAN_EVENT_INFO_DEFAULT_SIZE;
    IoEvent->PoolAllocated = TRUE;
  } else {
    if (UseExtraMemoryPool) {
      if (SizeOfEventInfo <= (16 * 1024)) {
        IoEvent->EventResult = Pop16KEventResult();
        IoEvent->EventResultSize = DOKAN_EVENT_INFO_16K_SIZE;
        IoEvent->PoolAllocated = TRUE;
      } else if (SizeOfEventInfo <= (32 * 1024)) {
        IoEvent->EventResult = Pop32KEventResult();
        IoEvent->EventResultSize = DOKAN_EVENT_INFO_32K_SIZE;
        IoEvent->PoolAllocated = TRUE;
      } else if (SizeOfEventInfo <= (64 * 1024)) {
        IoEvent->EventResult = Pop64KEventResult();
        IoEvent->EventResultSize = DOKAN_EVENT_INFO_64K_SIZE;
        IoEvent->PoolAllocated = TRUE;
      } else if (SizeOfEventInfo <= (128 * 1024)) {
        IoEvent->EventResult = Pop128KEventResult();
        IoEvent->EventResultSize = DOKAN_EVENT_INFO_128K_SIZE;
        IoEvent->PoolAllocated = TRUE;
      }
    }
    if (IoEvent->EventResult == NULL) {
      IoEvent->EventResultSize =
          DispatchGetEventInformationLength(SizeOfEventInfo);
      IoEvent->EventResult =
          (PEVENT_INFORMATION)malloc(IoEvent->EventResultSize);
      if (!IoEvent->EventResult) {
        return;
      }
      ZeroMemory(IoEvent->EventResult,
                 ClearNonPoolBuffer
                     ? IoEvent->EventResultSize
                     : FIELD_OFFSET(EVENT_INFORMATION, Buffer[0]));
    }
  }
  assert(IoEvent->EventResult &&
         IoEvent->EventResultSize >=
             DispatchGetEventInformationLength(SizeOfEventInfo));

  IoEvent->EventResult->SerialNumber = IoEvent->EventContext->SerialNumber;
  IoEvent->EventResult->Context = IoEvent->EventContext->Context;
}

VOID ReleaseDokanOpenInfo(PDOKAN_IO_EVENT IoEvent) {
  if (!IoEvent->DokanOpenInfo) {
    return;
  }
  EnterCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
  IoEvent->DokanOpenInfo->UserContext = IoEvent->DokanFileInfo.Context;
  IoEvent->DokanOpenInfo->OpenCount--;
  if (IoEvent->EventContext->MajorFunction == IRP_MJ_CLOSE) {
    IoEvent->DokanOpenInfo->CloseFileName =
        _wcsdup(IoEvent->EventContext->Operation.Close.FileName);
    IoEvent->DokanOpenInfo->CloseUserContext = IoEvent->DokanFileInfo.Context;
    IoEvent->DokanOpenInfo->OpenCount--;
  }
  if (IoEvent->DokanOpenInfo->OpenCount > 0) {
    // We are still waiting for the Close event or there is another event running. We delay the Close event.
    LeaveCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
    return;
  }

  // Process close event as OpenCount is now 0
  LPWSTR fileNameForClose = NULL;
  if (IoEvent->DokanOpenInfo->CloseFileName) {
    fileNameForClose = IoEvent->DokanOpenInfo->CloseFileName;
    IoEvent->DokanOpenInfo->CloseFileName = NULL;
  }
  IoEvent->DokanFileInfo.Context = IoEvent->DokanOpenInfo->CloseUserContext;
  LeaveCriticalSection(&IoEvent->DokanOpenInfo->CriticalSection);
  PushFileOpenInfo(IoEvent->DokanOpenInfo);
  IoEvent->DokanOpenInfo = NULL;
  if (IoEvent->EventResult) {
    // Reset the Kernel UserContext if we can. Close events do not have one.
    IoEvent->EventResult->Context = 0;
  }
  if (fileNameForClose) {
    if (IoEvent->DokanInstance->DokanOperations->CloseFile) {
      IoEvent->DokanInstance->DokanOperations->CloseFile(
          fileNameForClose, &IoEvent->DokanFileInfo);
    }
    free(fileNameForClose);
  }
}

// ask driver to release all pending IRP to prepare for Unmount.
BOOL SendReleaseIRP(LPCWSTR DeviceName) {
  ULONG returnedLength;
  WCHAR rawDeviceName[MAX_PATH];

  DbgPrintW(L"send release to %s\n", DeviceName);

  GetRawDeviceName(DeviceName, rawDeviceName, MAX_PATH);
  if (!SendToDevice(rawDeviceName, FSCTL_EVENT_RELEASE, NULL, 0, NULL, 0,
                    &returnedLength)) {

    DbgPrintW(L"Failed to unmount device:%s\n", DeviceName);
    return FALSE;
  }

  return TRUE;
}

BOOL SendGlobalReleaseIRP(LPCWSTR MountPoint) {
  if (MountPoint != NULL) {
    size_t length = wcslen(MountPoint);
    if (length > 0) {
      ULONG returnedLength;
      ULONG inputLength = sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) +
                          (MAX_PATH * sizeof(WCHAR));
      PDOKAN_UNICODE_STRING_INTERMEDIATE szMountPoint = malloc(inputLength);

      if (szMountPoint != NULL) {
        ZeroMemory(szMountPoint, inputLength);
        szMountPoint->MaximumLength = MAX_PATH * sizeof(WCHAR);
        szMountPoint->Length = (USHORT)(length * sizeof(WCHAR));
        CopyMemory(szMountPoint->Buffer, MountPoint, szMountPoint->Length);

        DbgPrintW(L"Send global Release for %s\n", MountPoint);

        if (!SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, FSCTL_EVENT_RELEASE,
                          szMountPoint, inputLength, NULL, 0,
                          &returnedLength)) {

          DbgPrintW(L"Failed to unmount: %s\n", MountPoint);
          free(szMountPoint);
          return FALSE;
        }

        free(szMountPoint);
        return TRUE;
      }
    }
  }

  return FALSE;
}

int DokanStart(_In_ PDOKAN_INSTANCE DokanInstance) {
  EVENT_START eventStart;
  EVENT_DRIVER_INFO driverInfo;
  ULONG returnedLength = 0;
  BOOL mountManager = FALSE;
  BOOL driverLetter = IsMountPointDriveLetter(DokanInstance->MountPoint);

  ZeroMemory(&eventStart, sizeof(EVENT_START));
  ZeroMemory(&driverInfo, sizeof(EVENT_DRIVER_INFO));

  eventStart.UserVersion = DOKAN_DRIVER_VERSION;
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_ALT_STREAM) {
    eventStart.Flags |= DOKAN_EVENT_ALTERNATIVE_STREAM_ON;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_NETWORK) {
    eventStart.DeviceType = DOKAN_NETWORK_FILE_SYSTEM;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_REMOVABLE) {
    eventStart.Flags |= DOKAN_EVENT_REMOVABLE;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_WRITE_PROTECT) {
    eventStart.Flags |= DOKAN_EVENT_WRITE_PROTECT;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) {
    eventStart.Flags |= DOKAN_EVENT_MOUNT_MANAGER;
    mountManager = TRUE;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_CURRENT_SESSION) {
    eventStart.Flags |= DOKAN_EVENT_CURRENT_SESSION;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_FILELOCK_USER_MODE) {
    eventStart.Flags |= DOKAN_EVENT_FILELOCK_USER_MODE;
  }
  if (DokanInstance->DokanOptions->Options &
      DOKAN_OPTION_ENABLE_UNMOUNT_NETWORK_DRIVE) {
    eventStart.Flags |= DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_CASE_SENSITIVE) {
    eventStart.Flags |= DOKAN_EVENT_CASE_SENSITIVE;
  }
  if (DokanInstance->DokanOptions->Options & DOKAN_OPTION_DISPATCH_DRIVER_LOGS) {
    eventStart.Flags |= DOKAN_EVENT_DISPATCH_DRIVER_LOGS;
  }
  if (driverLetter && mountManager &&
      !CheckDriveLetterAvailability(DokanInstance->MountPoint[0])) {
    eventStart.Flags |= DOKAN_EVENT_DRIVE_LETTER_IN_USE;
  }

  if (DokanInstance->DokanOptions->VolumeSecurityDescriptorLength != 0) {
    if (DokanInstance->DokanOptions->VolumeSecurityDescriptorLength >
        VOLUME_SECURITY_DESCRIPTOR_MAX_SIZE) {
      DokanDbgPrint(
          "Dokan Error: Invalid volume security descriptor length "
          "provided %ld\n",
          DokanInstance->DokanOptions->VolumeSecurityDescriptorLength);
      return DOKAN_START_ERROR;
    }
    eventStart.VolumeSecurityDescriptorLength =
        DokanInstance->DokanOptions->VolumeSecurityDescriptorLength;
    memcpy_s(eventStart.VolumeSecurityDescriptor,
             sizeof(eventStart.VolumeSecurityDescriptor),
             DokanInstance->DokanOptions->VolumeSecurityDescriptor,
             sizeof(DokanInstance->DokanOptions->VolumeSecurityDescriptor));
  }

  memcpy_s(eventStart.MountPoint, sizeof(eventStart.MountPoint),
           DokanInstance->MountPoint, sizeof(DokanInstance->MountPoint));
  memcpy_s(eventStart.UNCName, sizeof(eventStart.UNCName), DokanInstance->UNCName,
           sizeof(DokanInstance->UNCName));

  eventStart.IrpTimeout = DokanInstance->DokanOptions->Timeout;
  eventStart.FcbGarbageCollectionIntervalMs = 2000;

  SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, FSCTL_EVENT_START, &eventStart,
               sizeof(EVENT_START), &driverInfo, sizeof(EVENT_DRIVER_INFO),
               &returnedLength);

  if (driverInfo.Status == DOKAN_START_FAILED) {
    if (driverInfo.DriverVersion != eventStart.UserVersion) {
      DokanDbgPrint("Dokan Error: driver version mismatch, driver %X, dll %X\n",
                    driverInfo.DriverVersion, eventStart.UserVersion);
      return DOKAN_VERSION_ERROR;
    } else if (driverInfo.Flags == DOKAN_DRIVER_INFO_NO_MOUNT_POINT_ASSIGNED) {
      DokanDbgPrint("Dokan Error: Driver failed to set mount point %s\n",
                    eventStart.MountPoint);
      return DOKAN_MOUNT_ERROR;
    }
    DokanDbgPrint("Dokan Error: driver start error\n");    
    return DOKAN_START_ERROR;
  } else if (driverInfo.Status == DOKAN_MOUNTED) {
    DokanInstance->MountId = driverInfo.MountId;
    DokanInstance->DeviceNumber = driverInfo.DeviceNumber;
    wcscpy_s(DokanInstance->DeviceName, sizeof(DokanInstance->DeviceName) / sizeof(WCHAR),
             driverInfo.DeviceName);
    if (driverLetter && mountManager) {
      DokanInstance->MountPoint[0] = driverInfo.ActualDriveLetter;
    }
    return DOKAN_SUCCESS;
  }
  return DOKAN_START_ERROR;
}

BOOL DOKANAPI DokanSetDebugMode(ULONG Mode) {
  ULONG returnedLength;
  return SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, FSCTL_SET_DEBUG_MODE, &Mode,
                      sizeof(ULONG), NULL, 0, &returnedLength);
}

BOOL DOKANAPI DokanMountPointsCleanUp() {
  ULONG returnedLength;
  return SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, FSCTL_MOUNTPOINT_CLEANUP, NULL,
                      0, NULL, 0, &returnedLength);
}

BOOL SendToDevice(LPCWSTR DeviceName, DWORD IoControlCode, PVOID InputBuffer,
                  ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                  PULONG ReturnedLength) {
  HANDLE device;
  BOOL status;

  device = CreateFile(DeviceName,                         // lpFileName
                      0,                                  // dwDesiredAccess
                      FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
                      NULL,          // lpSecurityAttributes
                      OPEN_EXISTING, // dwCreationDistribution
                      0,             // dwFlagsAndAttributes
                      NULL           // hTemplateFile
  );

  if (device == INVALID_HANDLE_VALUE) {
    DWORD dwErrorCode = GetLastError();
    DbgPrintW(L"Dokan Error: Failed to open %ws with code %d\n", DeviceName,
              dwErrorCode);
    return FALSE;
  }

  status = DeviceIoControl(device,         // Handle to device
                           IoControlCode,  // IO Control code
                           InputBuffer,    // Input Buffer to driver.
                           InputLength,    // Length of input buffer in bytes.
                           OutputBuffer,   // Output Buffer from driver.
                           OutputLength,   // Length of output buffer in bytes.
                           ReturnedLength, // Bytes placed in buffer.
                           NULL            // synchronous call
  );

  CloseHandle(device);

  if (!status) {
    DbgPrint("DokanError: Ioctl 0x%x failed with code %d on Device %ws\n",
             IoControlCode, GetLastError(), DeviceName);
    return FALSE;
  }

  return TRUE;
}

PDOKAN_MOUNT_POINT_INFO DOKANAPI DokanGetMountPointList(BOOL uncOnly,
                                                        PULONG nbRead) {
  ULONG returnedLength = 0;
  PDOKAN_MOUNT_POINT_INFO dokanMountPointInfo = NULL;
  PDOKAN_MOUNT_POINT_INFO results = NULL;
  ULONG bufferLength = 32 * sizeof(*dokanMountPointInfo);
  BOOL success;

  *nbRead = 0;

  do {
    if (dokanMountPointInfo != NULL)
      free(dokanMountPointInfo);
    dokanMountPointInfo = malloc(bufferLength);
    if (dokanMountPointInfo == NULL)
      return NULL;
    ZeroMemory(dokanMountPointInfo, bufferLength);

    success = SendToDevice(DOKAN_GLOBAL_DEVICE_NAME,
                           FSCTL_EVENT_MOUNTPOINT_LIST, NULL, 0,
                           dokanMountPointInfo, bufferLength, &returnedLength);

    if (!success && GetLastError() != ERROR_MORE_DATA) {
      free(dokanMountPointInfo);
      return NULL;
    }
    bufferLength *= 2;
  } while (!success);

  if (returnedLength == 0) {
    free(dokanMountPointInfo);
    return NULL;
  }

  *nbRead = returnedLength / sizeof(DOKAN_MOUNT_POINT_INFO);
  results = malloc(returnedLength);
  if (results != NULL) {
    ZeroMemory(results, returnedLength);
    for (ULONG i = 0; i < *nbRead; ++i) {
      if (!uncOnly || wcscmp(dokanMountPointInfo[i].UNCName, L"") != 0)
        CopyMemory(&results[i], &dokanMountPointInfo[i],
                   sizeof(DOKAN_MOUNT_POINT_INFO));
    }
  }
  free(dokanMountPointInfo);
  return results;
}

VOID DOKANAPI DokanReleaseMountPointList(PDOKAN_MOUNT_POINT_INFO list) {
  free(list);
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  UNREFERENCED_PARAMETER(Reserved);
  UNREFERENCED_PARAMETER(Instance);

  switch (Reason) {
  case DLL_PROCESS_ATTACH: {

  } break;
  case DLL_PROCESS_DETACH: {

  } break;
  default:
    break;
  }
  return TRUE;
}

VOID DOKANAPI DokanMapKernelToUserCreateFileFlags(
    ACCESS_MASK DesiredAccess, ULONG FileAttributes, ULONG CreateOptions,
    ULONG CreateDisposition, ACCESS_MASK *outDesiredAccess,
    DWORD *outFileAttributesAndFlags, DWORD *outCreationDisposition) {
  BOOL genericRead = FALSE, genericWrite = FALSE, genericExecute = FALSE,
       genericAll = FALSE;

  if (outFileAttributesAndFlags) {

    *outFileAttributesAndFlags = FileAttributes;

    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_WRITE_THROUGH, FILE_WRITE_THROUGH);
    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_SEQUENTIAL_SCAN, FILE_SEQUENTIAL_ONLY);
    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_RANDOM_ACCESS, FILE_RANDOM_ACCESS);
    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_NO_BUFFERING, FILE_NO_INTERMEDIATE_BUFFERING);
    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_OPEN_REPARSE_POINT, FILE_OPEN_REPARSE_POINT);
    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_DELETE_ON_CLOSE, FILE_DELETE_ON_CLOSE);
    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_BACKUP_SEMANTICS, FILE_OPEN_FOR_BACKUP_INTENT);

#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions,
                      FILE_FLAG_SESSION_AWARE, FILE_SESSION_AWARE);
#endif
  }

  if (outCreationDisposition) {

    switch (CreateDisposition) {
    case FILE_CREATE:
      *outCreationDisposition = CREATE_NEW;
      break;
    case FILE_OPEN:
      *outCreationDisposition = OPEN_EXISTING;
      break;
    case FILE_OPEN_IF:
      *outCreationDisposition = OPEN_ALWAYS;
      break;
    case FILE_OVERWRITE:
      *outCreationDisposition = TRUNCATE_EXISTING;
      break;
    case FILE_SUPERSEDE:
    // The documentation isn't clear on the difference between replacing a file
    // and truncating it.
    // For now we just map it to create/truncate
    case FILE_OVERWRITE_IF:
      *outCreationDisposition = CREATE_ALWAYS;
      break;
    default:
      *outCreationDisposition = 0;
      break;
    }
  }

  if (outDesiredAccess) {

    *outDesiredAccess = DesiredAccess;

    if ((*outDesiredAccess & FILE_GENERIC_READ) == FILE_GENERIC_READ) {
      *outDesiredAccess |= GENERIC_READ;
      genericRead = TRUE;
    }
    if ((*outDesiredAccess & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE) {
      *outDesiredAccess |= GENERIC_WRITE;
      genericWrite = TRUE;
    }
    if ((*outDesiredAccess & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE) {
      *outDesiredAccess |= GENERIC_EXECUTE;
      genericExecute = TRUE;
    }
    if ((*outDesiredAccess & FILE_ALL_ACCESS) == FILE_ALL_ACCESS) {
      *outDesiredAccess |= GENERIC_ALL;
      genericAll = TRUE;
    }

    if (genericRead)
      *outDesiredAccess &= ~FILE_GENERIC_READ;
    if (genericWrite)
      *outDesiredAccess &= ~FILE_GENERIC_WRITE;
    if (genericExecute)
      *outDesiredAccess &= ~FILE_GENERIC_EXECUTE;
    if (genericAll)
      *outDesiredAccess &= ~FILE_ALL_ACCESS;
  }
}

VOID DOKANAPI DokanInit() {
  // ensure 64-bit alignment
  assert(FIELD_OFFSET(EVENT_INFORMATION, Buffer) % 8 == 0);

  // this is not as safe as a critical section so to some degree we rely on
  // the user to do the right thing
  LONG initRefCount = InterlockedIncrement(&g_DokanInitialized);
  if (initRefCount <= 0) {
    RaiseException(DOKAN_EXCEPTION_INITIALIZATION_FAILED,
                   EXCEPTION_NONCONTINUABLE, 0, NULL);
    return;
  }
  if (initRefCount > 1) {
    return;
  }

  (void)InitializeCriticalSectionAndSpinCount(&g_InstanceCriticalSection,
                                              0x80000400);

  InitializeListHead(&g_InstanceList);
  EnterCriticalSection(&g_InstanceCriticalSection);
  { InitializePool(); }
  LeaveCriticalSection(&g_InstanceCriticalSection);
}

VOID DOKANAPI DokanShutdown() {
  LONG initRefCount = InterlockedDecrement(&g_DokanInitialized);
  if (initRefCount < 0) {
    RaiseException(DOKAN_EXCEPTION_SHUTDOWN_FAILED, EXCEPTION_NONCONTINUABLE, 0,
                   NULL);
    return;
  }
  if (initRefCount > 0) {
    return;
  }

  EnterCriticalSection(&g_InstanceCriticalSection);
  {
    while (!IsListEmpty(&g_InstanceList)) {
      PLIST_ENTRY entry = RemoveHeadList(&g_InstanceList);
      PDOKAN_INSTANCE dokanInstance =
          CONTAINING_RECORD(entry, DOKAN_INSTANCE, ListEntry);
      DokanCloseHandle((DOKAN_HANDLE)dokanInstance);
    }
    CleanupPool();
  }
  LeaveCriticalSection(&g_InstanceCriticalSection);
  DeleteCriticalSection(&g_InstanceCriticalSection);
}

BOOL DOKANAPI DokanNotifyPath(_In_ DOKAN_HANDLE DokanInstance,
                              _In_ LPCWSTR FilePath,
                              _In_ ULONG CompletionFilter, _In_ ULONG Action) {
  DOKAN_INSTANCE *instance = (DOKAN_INSTANCE *)DokanInstance;
  if (!instance) {
    return FALSE;
  }
  if (FilePath == NULL || !instance || !instance->NotifyHandle) {
    return FALSE;
  }
  size_t length = wcslen(FilePath);
  const size_t prefixSize = 2; // size of mount letter plus ":"
  if (length <= prefixSize) {
    return FALSE;
  }
  // remove the mount letter and colon from length, for example: "G:"
  length -= prefixSize;
  ULONG returnedLength;
  ULONG inputLength = (ULONG)(sizeof(DOKAN_NOTIFY_PATH_INTERMEDIATE) +
                              (length * sizeof(WCHAR)));
  PDOKAN_NOTIFY_PATH_INTERMEDIATE pNotifyPath = malloc(inputLength);
  if (pNotifyPath == NULL) {
    DbgPrint("Failed to allocate NotifyPath\n");
    return FALSE;
  }
  ZeroMemory(pNotifyPath, inputLength);
  pNotifyPath->CompletionFilter = CompletionFilter;
  pNotifyPath->Action = Action;
  pNotifyPath->Length = (USHORT)(length * sizeof(WCHAR));
  CopyMemory(pNotifyPath->Buffer, FilePath + prefixSize, pNotifyPath->Length);
  if (!DeviceIoControl(instance->NotifyHandle, FSCTL_NOTIFY_PATH, pNotifyPath,
                       inputLength, NULL, 0, &returnedLength, NULL)) {
    DbgPrint("Failed to send notify path command:%ws\n", FilePath);
    free(pNotifyPath);
    return FALSE;
  }
  free(pNotifyPath);
  return TRUE;
}

BOOL DOKANAPI DokanNotifyCreate(_In_ DOKAN_HANDLE DokanInstance,
                                _In_ LPCWSTR FilePath, _In_ BOOL IsDirectory) {
  return DokanNotifyPath(DokanInstance, FilePath,
                         IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME
                                     : FILE_NOTIFY_CHANGE_FILE_NAME,
                         FILE_ACTION_ADDED);
}

BOOL DOKANAPI DokanNotifyDelete(_In_ DOKAN_HANDLE DokanInstance,
                                _In_ LPCWSTR FilePath, _In_ BOOL IsDirectory) {
  return DokanNotifyPath(DokanInstance, FilePath,
                         IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME
                                     : FILE_NOTIFY_CHANGE_FILE_NAME,
                         FILE_ACTION_REMOVED);
}

BOOL DOKANAPI DokanNotifyUpdate(_In_ DOKAN_HANDLE DokanInstance,
                                _In_ LPCWSTR FilePath) {
  return DokanNotifyPath(DokanInstance, FilePath, FILE_NOTIFY_CHANGE_ATTRIBUTES,
                         FILE_ACTION_MODIFIED);
}

BOOL DOKANAPI DokanNotifyXAttrUpdate(_In_ DOKAN_HANDLE DokanInstance,
                                     _In_ LPCWSTR FilePath) {
  return DokanNotifyPath(DokanInstance, FilePath, FILE_NOTIFY_CHANGE_ATTRIBUTES,
                         FILE_ACTION_MODIFIED);
}

BOOL DOKANAPI DokanNotifyRename(_In_ DOKAN_HANDLE DokanInstance,
                                _In_ LPCWSTR OldPath, _In_ LPCWSTR NewPath,
                                _In_ BOOL IsDirectory,
                                _In_ BOOL IsInSameDirectory) {
  BOOL success = DokanNotifyPath(
      DokanInstance, OldPath,
      IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
      IsInSameDirectory ? FILE_ACTION_RENAMED_OLD_NAME : FILE_ACTION_REMOVED);
  success &= DokanNotifyPath(
      DokanInstance, NewPath,
      IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
      IsInSameDirectory ? FILE_ACTION_RENAMED_NEW_NAME : FILE_ACTION_ADDED);
  return success;
}
