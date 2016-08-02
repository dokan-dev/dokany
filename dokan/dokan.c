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

#include <conio.h>
#include <locale.h>
#include <ntstatus.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <winioctl.h>
#include <assert.h>

// 1024 * ~32k = 32mb when the pool is full
// We should probably optimize our usage of fixed size 32k buffers for
// EVENT_CONTEXT but that will be for another PR.
#define DOKAN_IO_EVENT_POOL_SIZE 1024

#define DOKAN_OVERLAPPED_POOL_SIZE 1024

#define DOKAN_DIRECTORY_LIST_POOL_SIZE 128

#define DokanMapKernelBit(dest, src, userBit, kernelBit)                       \
  if (((src) & (kernelBit)) == (kernelBit))                                    \
  (dest) |= (userBit)

// DokanOptions->DebugMode is ON?
BOOL					g_DebugMode = TRUE;

// DokanOptions->UseStdErr is ON?
BOOL					g_UseStdErr = FALSE;

// Dokan DLL critical section
CRITICAL_SECTION		g_InstanceCriticalSection;

// Global linked list of mounted Dokan instances
LIST_ENTRY				g_InstanceList;

// Global thread pool
PTP_POOL				g_ThreadPool = NULL;

// Global vector of event buffers
DOKAN_VECTOR			*g_EventBufferPool = NULL;
CRITICAL_SECTION		g_EventBufferCriticalSection;

DOKAN_VECTOR			*g_OverlappedPool = NULL;
CRITICAL_SECTION		g_OverlappedCriticalSection;

DOKAN_VECTOR			*g_EventResultPool = NULL;
CRITICAL_SECTION		g_EventResultCriticalSection;

DOKAN_VECTOR			*g_FileInfoPool = NULL;
CRITICAL_SECTION		g_FileInfoCriticalSection;

DOKAN_VECTOR			*g_DirectoryListPool = NULL;
CRITICAL_SECTION		g_DirectoryListCriticalSection;


// TODO NEXT:
// * Double check all DOKAN_VECTOR code.
// * Add functions for pushing and popping global event buffers
// * Finish DokanMain() by initiating the IO checks for the device
// * Finish DokanLoop() which needs to queue the next IO check for the device
// * Look at CancelThreadpoolIo (https://msdn.microsoft.com/en-us/library/windows/desktop/ms681983(v=vs.85).aspx)
//   for cancelling pending IO operations
// * Need global event handler for cancelling pending IO operations
// * Need global event handler for cleanup of Dokan object
// * All shutdown/cleanup code needs to be reviewed. We need a way to guarantee a Dokan instance is done doing stuff before deallocating it.
//   This probably needs to be done using event HANDLE's that get signalled when the object is clean


VOID DOKANAPI DokanUseStdErr(BOOL Status) { g_UseStdErr = Status; }

VOID DOKANAPI DokanDebugMode(BOOL Status) { g_DebugMode = Status; }

int InitializeThreadPool(HMODULE hModule) {

	UNREFERENCED_PARAMETER(hModule);

	EnterCriticalSection(&g_InstanceCriticalSection);
	{
		if(g_ThreadPool) {

			DokanDbgPrint("Dokan Error: Thread pool has already been created.\n");
			LeaveCriticalSection(&g_InstanceCriticalSection);
			return DOKAN_DRIVER_INSTALL_ERROR;
		}

		// It seems this is only needed if LoadLibrary() and FreeLibrary() are used and it should be called by the exe
		// SetThreadpoolCallbackLibrary(&g_ThreadPoolCallbackEnvironment, hModule);

		g_ThreadPool = CreateThreadpool(NULL);

		if(!g_ThreadPool) {

			DokanDbgPrint("Dokan Error: Failed to create thread pool.\n");
			LeaveCriticalSection(&g_InstanceCriticalSection);
			return DOKAN_DRIVER_INSTALL_ERROR;
		}
	}
	LeaveCriticalSection(&g_InstanceCriticalSection);

	return DOKAN_SUCCESS;
}

void CleanupThreadpool() {

	EnterCriticalSection(&g_InstanceCriticalSection);
	{
		// TODO: Iterate all instances and deallocate their cleanup groups

		if(g_ThreadPool) {

			CloseThreadpool(g_ThreadPool);
			g_ThreadPool = NULL;
		}
	}
	LeaveCriticalSection(&g_InstanceCriticalSection);
}

/////////////////// DOKAN_IO_EVENT ///////////////////

DOKAN_IO_EVENT* PopIoEventBuffer() {

	DOKAN_IO_EVENT *ioEvent = NULL;

	EnterCriticalSection(&g_EventBufferCriticalSection);
	{
		if(DokanVector_GetCount(g_EventBufferPool) > 0)
		{
			ioEvent = *(DOKAN_IO_EVENT**)DokanVector_GetLastItem(g_EventBufferPool);
			DokanVector_PopBack(g_EventBufferPool);
		}
	}
	LeaveCriticalSection(&g_EventBufferCriticalSection);

	if(!ioEvent) {

		ioEvent = (DOKAN_IO_EVENT*)malloc(sizeof(DOKAN_IO_EVENT));
	}

	if(ioEvent) {

		RtlZeroMemory(ioEvent, sizeof(DOKAN_IO_EVENT));
		ioEvent->Flags = DOKAN_IO_EVENT_FLAGS_POOLED;
	}

	return ioEvent;
}

void FreeIOEventBuffer(DOKAN_IO_EVENT *IOEvent) {

	if(IOEvent) {
		
		free(IOEvent);
	}
}

void PushIoEventBuffer(DOKAN_IO_EVENT *IOEvent) {

	assert(IOEvent);

	if((IOEvent->Flags & DOKAN_IO_EVENT_FLAGS_POOLED) != DOKAN_IO_EVENT_FLAGS_POOLED) {

		FreeIOEventBuffer(IOEvent);
		return;
	}

	EnterCriticalSection(&g_EventBufferCriticalSection);
	{
		if(DokanVector_GetCount(g_EventBufferPool) < DOKAN_IO_EVENT_POOL_SIZE) {

			DokanVector_PushBack(g_EventBufferPool, &IOEvent);
			IOEvent = NULL;
		}
	}
	LeaveCriticalSection(&g_EventBufferCriticalSection);

	if(IOEvent) {

		FreeIOEventBuffer(IOEvent);
	}
}

/////////////////// DOKAN_OVERLAPPED ///////////////////

void ResetOverlapped(DOKAN_OVERLAPPED *overlapped) {

	//HANDLE tempHandle;

	if(overlapped) {

		//tempHandle = overlapped->InternalOverlapped.hEvent;

		RtlZeroMemory(overlapped, sizeof(DOKAN_OVERLAPPED));

		/*overlapped->InternalOverlapped.hEvent = tempHandle;

		if(tempHandle) {

			ResetEvent(tempHandle);
		}*/
	}
}

DOKAN_OVERLAPPED* PopOverlapped() {

	DOKAN_OVERLAPPED *overlapped = NULL;

	EnterCriticalSection(&g_OverlappedCriticalSection);
	{
		if(DokanVector_GetCount(g_OverlappedPool) > 0)
		{
			overlapped = *(DOKAN_OVERLAPPED**)DokanVector_GetLastItem(g_OverlappedPool);
			DokanVector_PopBack(g_OverlappedPool);
		}
	}
	LeaveCriticalSection(&g_OverlappedCriticalSection);

	if(!overlapped) {

		overlapped = (DOKAN_OVERLAPPED*)malloc(sizeof(DOKAN_OVERLAPPED));
		/*overlapped->InternalOverlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

		if(!overlapped->InternalOverlapped.hEvent) {

			DbgPrint("Dokan Warning: Failed to create DOKAN_OVERLAPPED event handle.\n");
		}*/
	}

	if(overlapped) {

		ResetOverlapped(overlapped);
	}

	return overlapped;
}

void FreeOverlapped(DOKAN_OVERLAPPED *Overlapped) {

	if(Overlapped) {

		if(Overlapped->InternalOverlapped.hEvent) {

			CloseHandle(Overlapped->InternalOverlapped.hEvent);
		}
		
		free(Overlapped);
	}
}


void PushOverlapped(DOKAN_OVERLAPPED *Overlapped) {
	
	assert(Overlapped);

	EnterCriticalSection(&g_OverlappedCriticalSection);
	{
		if(DokanVector_GetCount(g_OverlappedPool) < DOKAN_OVERLAPPED_POOL_SIZE) {

			DokanVector_PushBack(g_OverlappedPool, &Overlapped);
			Overlapped = NULL;
		}
	}
	LeaveCriticalSection(&g_OverlappedCriticalSection);

	if(Overlapped) {

		FreeOverlapped(Overlapped);
	}
}

/////////////////// EVENT_INFORMATION ///////////////////

EVENT_INFORMATION* PopEventResult() {

	EVENT_INFORMATION *eventResult = NULL;

	EnterCriticalSection(&g_EventResultCriticalSection);
	{
		if(DokanVector_GetCount(g_EventResultPool) > 0) {

			eventResult = *(EVENT_INFORMATION**)DokanVector_GetLastItem(g_EventResultPool);
			DokanVector_PopBack(g_EventResultPool);
		}
	}
	LeaveCriticalSection(&g_EventResultCriticalSection);

	if(!eventResult) {

		eventResult = (EVENT_INFORMATION*)malloc(DOKAN_EVENT_INFO_DEFAULT_SIZE);
	}

	if(eventResult) {

		RtlZeroMemory(eventResult, DOKAN_EVENT_INFO_DEFAULT_SIZE);
	}

	return eventResult;
}

void FreeEventResult(EVENT_INFORMATION *EventResult) {

	if(EventResult) {

		free(EventResult);
	}
}


void PushEventResult(EVENT_INFORMATION *EventResult) {

	assert(EventResult);

	EnterCriticalSection(&g_EventResultCriticalSection);
	{
		if(DokanVector_GetCount(g_EventResultPool) < DOKAN_OVERLAPPED_POOL_SIZE) {

			DokanVector_PushBack(g_EventResultPool, &EventResult);
			EventResult = NULL;
		}
	}
	LeaveCriticalSection(&g_EventResultCriticalSection);

	if(EventResult) {

		FreeEventResult(EventResult);
	}
}

/////////////////// DOKAN_OPEN_INFO ///////////////////

DOKAN_OPEN_INFO* PopFileOpenInfo() {

	DOKAN_OPEN_INFO *fileInfo = NULL;

	EnterCriticalSection(&g_FileInfoCriticalSection);
	{
		if(DokanVector_GetCount(g_FileInfoPool) > 0)
		{
			fileInfo = *(DOKAN_OPEN_INFO**)DokanVector_GetLastItem(g_FileInfoPool);
			DokanVector_PopBack(g_FileInfoPool);
		}
	}
	LeaveCriticalSection(&g_FileInfoCriticalSection);

	if(!fileInfo) {

		fileInfo = (DOKAN_OPEN_INFO*)malloc(sizeof(DOKAN_OPEN_INFO));

		RtlZeroMemory(fileInfo, sizeof(DOKAN_OPEN_INFO));

		InitializeCriticalSection(&fileInfo->CriticalSection);
	}

	if(fileInfo) {

		fileInfo->DokanInstance = NULL;
		fileInfo->DirList = NULL;
		InterlockedExchange64(&fileInfo->UserContext, 0);
		fileInfo->EventId = 0;
		fileInfo->IsDirectory = FALSE;
	}

	return fileInfo;
}

void CleanupFileOpenInfo(DOKAN_OPEN_INFO *FileInfo) {

	assert(FileInfo);

	DOKAN_VECTOR *dirList = NULL;

	EnterCriticalSection(&FileInfo->CriticalSection);
	{
		if(FileInfo->DirListSearchPattern) {

			free(FileInfo->DirListSearchPattern);
			FileInfo->DirListSearchPattern = NULL;
		}

		if(FileInfo->DirList) {

			dirList = FileInfo->DirList;
			FileInfo->DirList = NULL;
		}
	}
	LeaveCriticalSection(&FileInfo->CriticalSection);

	if(dirList) {

		PushDirectoryList(dirList);
	}
}

void FreeFileOpenInfo(DOKAN_OPEN_INFO *FileInfo) {

	if(FileInfo) {

		CleanupFileOpenInfo(FileInfo);

		DeleteCriticalSection(&FileInfo->CriticalSection);
		free(FileInfo);
	}
}

void PushFileOpenInfo(DOKAN_OPEN_INFO *FileInfo) {

	assert(FileInfo);

	CleanupFileOpenInfo(FileInfo);

	EnterCriticalSection(&g_FileInfoCriticalSection);
	{
		if(DokanVector_GetCount(g_FileInfoPool) < DOKAN_OVERLAPPED_POOL_SIZE) {

			DokanVector_PushBack(g_FileInfoPool, &FileInfo);
			FileInfo = NULL;
		}
	}
	LeaveCriticalSection(&g_FileInfoCriticalSection);

	if(FileInfo) {

		FreeFileOpenInfo(FileInfo);
	}
}

/////////////////// Directory list ///////////////////

DOKAN_VECTOR* PopDirectoryList() {

	DOKAN_VECTOR *directoryList = NULL;

	EnterCriticalSection(&g_DirectoryListCriticalSection);
	{
		if(DokanVector_GetCount(g_DirectoryListPool) > 0)
		{
			directoryList = *(DOKAN_VECTOR**)DokanVector_GetLastItem(g_DirectoryListPool);
			DokanVector_PopBack(g_DirectoryListPool);
		}
	}
	LeaveCriticalSection(&g_DirectoryListCriticalSection);

	if(!directoryList) {

		directoryList = DokanVector_Alloc(sizeof(WIN32_FIND_DATAW));
	}

	if(directoryList) {

		DokanVector_Clear(directoryList);
	}

	return directoryList;
}

void PushDirectoryList(DOKAN_VECTOR *DirectoryList) {

	assert(DirectoryList);
	assert(DokanVector_GetItemSize(DirectoryList) == sizeof(WIN32_FIND_DATAW));

	EnterCriticalSection(&g_DirectoryListCriticalSection);
	{
		if(DokanVector_GetCount(g_DirectoryListPool) < DOKAN_DIRECTORY_LIST_POOL_SIZE) {

			DokanVector_PushBack(g_DirectoryListPool, &DirectoryList);
			DirectoryList = NULL;
		}
	}
	LeaveCriticalSection(&g_DirectoryListCriticalSection);

	if(DirectoryList) {

		DokanVector_Free(DirectoryList);
	}
}

/////////////////// Push/Pop pattern finished ///////////////////

PDOKAN_INSTANCE
NewDokanInstance() {
  PDOKAN_INSTANCE instance = (PDOKAN_INSTANCE)malloc(sizeof(DOKAN_INSTANCE));

  if(instance == NULL) {
	  return NULL;
  }

  RtlZeroMemory(instance, sizeof(DOKAN_INSTANCE));

  instance->GlobalDevice = INVALID_HANDLE_VALUE;
  instance->Device = INVALID_HANDLE_VALUE;

#if _MSC_VER < 1300
  InitializeCriticalSection(&instance->CriticalSection);
#else
  InitializeCriticalSectionAndSpinCount(&instance->CriticalSection, 0x80000400);
#endif

  InitializeListHead(&instance->ListEntry);
  
  instance->DeviceClosedWaitHandle = CreateEvent(NULL, TRUE, FALSE, NULL);

  if(!instance->DeviceClosedWaitHandle) {

	  DokanDbgPrint("Dokan Error: Cannot create Dokan instance because the device closed wait handle could not be created.\n");
	  
	  DeleteCriticalSection(&instance->CriticalSection);

	  free(instance);
	  
	  return NULL;
  }

  EnterCriticalSection(&g_InstanceCriticalSection);
  {
	  if(!g_ThreadPool) {

		  DokanDbgPrint("Dokan Error: Cannot create Dokan instance because the thread pool hasn't been created.\n");
		  LeaveCriticalSection(&g_InstanceCriticalSection);

		  DeleteCriticalSection(&instance->CriticalSection);

		  CloseHandle(instance->DeviceClosedWaitHandle);

		  free(instance);

		  return NULL;
	  }

	  instance->ThreadInfo.ThreadPool = g_ThreadPool;
	  instance->ThreadInfo.CleanupGroup = CreateThreadpoolCleanupGroup();

	  if(!instance->ThreadInfo.CleanupGroup) {

		  DokanDbgPrint("Dokan Error: Failed to create thread pool cleanup group.\n");

		  LeaveCriticalSection(&g_InstanceCriticalSection);

		  DeleteCriticalSection(&instance->CriticalSection);

		  CloseHandle(instance->DeviceClosedWaitHandle);

		  free(instance);

		  return NULL;
	  }

	  InitializeThreadpoolEnvironment(&instance->ThreadInfo.CallbackEnvironment);

	  SetThreadpoolCallbackPool(&instance->ThreadInfo.CallbackEnvironment, g_ThreadPool);

	  SetThreadpoolCallbackCleanupGroup(&instance->ThreadInfo.CallbackEnvironment, instance->ThreadInfo.CleanupGroup, NULL);

	  InsertTailList(&g_InstanceList, &instance->ListEntry);
  }
  LeaveCriticalSection(&g_InstanceCriticalSection);

  return instance;
}

void DeleteDokanInstance(PDOKAN_INSTANCE Instance) {

  SetEvent(Instance->DeviceClosedWaitHandle);

  if(Instance->ThreadInfo.KeepAliveTimer) {
	  
	  // cancel timer
	  SetThreadpoolTimer(Instance->ThreadInfo.KeepAliveTimer, NULL, 0, 0);
  }

  if(Instance->ThreadInfo.CleanupGroup) {

	  CloseThreadpoolCleanupGroupMembers(Instance->ThreadInfo.CleanupGroup, FALSE, Instance);
	  CloseThreadpoolCleanupGroup(Instance->ThreadInfo.CleanupGroup);
	  Instance->ThreadInfo.CleanupGroup = NULL;

	  DestroyThreadpoolEnvironment(&Instance->ThreadInfo.CallbackEnvironment);

	  // Members freed by CloseThreadpoolCleanupGroupMembers():

	  Instance->ThreadInfo.KeepAliveTimer = NULL;
	  Instance->ThreadInfo.IoCompletion = NULL;
  }

  if(Instance->Device && Instance->Device != INVALID_HANDLE_VALUE) {

	  CloseHandle(Instance->Device);
  }

  if(Instance->GlobalDevice && Instance->GlobalDevice != INVALID_HANDLE_VALUE) {
	  
	  CloseHandle(Instance->GlobalDevice);
  }

  DeleteCriticalSection(&Instance->CriticalSection);

  EnterCriticalSection(&g_InstanceCriticalSection);
  {
	  RemoveEntryList(&Instance->ListEntry);
  }
  LeaveCriticalSection(&g_InstanceCriticalSection);

  CloseHandle(Instance->DeviceClosedWaitHandle);

  free(Instance);
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
  return (L'b' <= DriveLetter && DriveLetter <= L'z') ||
         (L'B' <= DriveLetter && DriveLetter <= L'Z');
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
    DbgPrintW(L"CheckDriveLetterAvailability failed, QueryDosDevice detected "
              L"drive \"%c\"\n",
              DriveLetter);
    return FALSE;
  }

  DWORD drives = GetLogicalDrives();
  result = (drives >> (driveLetter - L'A') & 0x00000001);
  if (result > 0) {
    DbgPrintW(L"CheckDriveLetterAvailability failed, GetLogicalDrives detected "
              L"drive \"%c\"\n",
              DriveLetter);
    return FALSE;
  }

  return TRUE;
}

void CheckAllocationUnitSectorSize(PDOKAN_OPTIONS DokanOptions) {
  ULONG allocationUnitSize = DokanOptions->AllocationUnitSize;
  ULONG sectorSize = DokanOptions->SectorSize;

  if ((allocationUnitSize < 512 || allocationUnitSize > 65536 ||
       (allocationUnitSize & (allocationUnitSize - 1)) != 0) // Is power of tow
      || (sectorSize < 512 || sectorSize > 65536 ||
          (sectorSize & (sectorSize - 1)))) { // Is power of tow
    // Reset to default if values does not fit windows FAT/NTFS value
    // https://support.microsoft.com/en-us/kb/140365
    DokanOptions->SectorSize = DOKAN_DEFAULT_SECTOR_SIZE;
    DokanOptions->AllocationUnitSize = DOKAN_DEFAULT_ALLOCATION_UNIT_SIZE;
  }

  DbgPrintW(L"AllocationUnitSize: %d SectorSize: %d\n",
            DokanOptions->AllocationUnitSize, DokanOptions->SectorSize);
}

BOOL StartDeviceIO(PDOKAN_INSTANCE Dokan, DOKAN_OVERLAPPED *Overlapped) {

	DOKAN_IO_EVENT *ioEvent = PopIoEventBuffer();
	DWORD lastError = 0;

	if(!ioEvent) {
		
		DokanDbgPrint("Dokan Error: Failed to allocate IO event buffer.\n");
		
		return FALSE;
	}

	assert(ioEvent->EventResult == NULL && ioEvent->EventResultSize == 0);

	ioEvent->DokanInstance = Dokan;

	if(!Overlapped) {

		Overlapped = PopOverlapped();

		if(!Overlapped) {

			DokanDbgPrint("Dokan Error: Failed to allocate overlapped info.\n");

			PushIoEventBuffer(ioEvent);

			return FALSE;
		}
	}

	Overlapped->OutputPayload = ioEvent;
	Overlapped->PayloadType = DOKAN_OVERLAPPED_TYPE_IOEVENT;

	StartThreadpoolIo(Dokan->ThreadInfo.IoCompletion);

	if(!DeviceIoControl(
		Dokan->Device,							// Handle to device
		IOCTL_EVENT_WAIT,						// IO Control code
		NULL,									// Input Buffer to driver.
		0,										// Length of input buffer in bytes.
		ioEvent->KernelInfo.EventContextBuffer,	// Output Buffer from driver.
		EVENT_CONTEXT_MAX_SIZE,					// Length of output buffer in bytes.
		NULL,									// Bytes placed in buffer.
		(OVERLAPPED*)Overlapped					// asynchronous call
	)) {

		lastError = GetLastError();

		if(lastError != ERROR_IO_PENDING) {

			DbgPrint("Dokan Error: Dokan device ioctl failed for wait with code %d.\n", lastError);

			CancelThreadpoolIo(Dokan->ThreadInfo.IoCompletion);

			PushIoEventBuffer(ioEvent);
			PushOverlapped(Overlapped);

			return FALSE;
		}
	}

	return TRUE;
}

BOOL DOKANAPI DokanIsFileSystemRunning(_In_ DOKAN_HANDLE DokanInstance) {

	DOKAN_INSTANCE *instance = (DOKAN_INSTANCE*)DokanInstance;

	if(!instance) {

		return FALSE;
	}

	return WaitForSingleObject(instance->DeviceClosedWaitHandle, 0) == WAIT_TIMEOUT ? TRUE : FALSE;
}

DWORD DOKANAPI DokanWaitForFileSystemClosed(
	DOKAN_HANDLE DokanInstance,
	DWORD dwMilliseconds) {

	DOKAN_INSTANCE *instance = (DOKAN_INSTANCE*)DokanInstance;

	if(!instance) {

		return FALSE;
	}

	return WaitForSingleObject(instance->DeviceClosedWaitHandle, dwMilliseconds);
}

void DOKANAPI DokanCloseHandle(DOKAN_HANDLE DokanInstance) {

	DOKAN_INSTANCE *instance = (DOKAN_INSTANCE*)DokanInstance;

	if(!instance) {

		return;
	}

	// make sure the driver is unmounted
	DokanRemoveMountPoint(instance->MountPoint);

	DokanWaitForFileSystemClosed((DOKAN_HANDLE)instance, INFINITE);

	DeleteDokanInstance(instance);
}

int DOKANAPI DokanMain(PDOKAN_OPTIONS DokanOptions,
	PDOKAN_OPERATIONS DokanOperations) {

	DOKAN_INSTANCE *instance = NULL;
	int returnCode;

	returnCode = DokanCreateFileSystem(DokanOptions, DokanOperations, (DOKAN_HANDLE*)&instance);

	if(DOKAN_FAILED(returnCode)) {

		return returnCode;
	}

	DokanWaitForFileSystemClosed((DOKAN_HANDLE)instance, INFINITE);

	DeleteDokanInstance(instance);

	return returnCode;
}

int DOKANAPI DokanCreateFileSystem(
	_In_ PDOKAN_OPTIONS DokanOptions,
	_In_ PDOKAN_OPERATIONS DokanOperations,
	_Out_ DOKAN_HANDLE *DokanInstance) {

  PDOKAN_INSTANCE instance;
  ULARGE_INTEGER timerDueTime;
  WCHAR rawDeviceName[MAX_PATH];

  g_DebugMode = DokanOptions->Options & DOKAN_OPTION_DEBUG;
  g_UseStdErr = DokanOptions->Options & DOKAN_OPTION_STDERR;

  if (g_DebugMode) {
    DbgPrintW(L"Dokan: debug mode on\n");
  }

  if (g_UseStdErr) {
    DbgPrintW(L"Dokan: use stderr\n");
    g_DebugMode = TRUE;
  }

  if ((DokanOptions->Options & DOKAN_OPTION_NETWORK)
	  && !IsMountPointDriveLetter(DokanOptions->MountPoint)) {

    DokanOptions->Options &= ~DOKAN_OPTION_NETWORK;

    DbgPrintW(L"Dokan: Mount point folder is specified with network device "
              L"option. Disable network device.\n");
  }

  if (DokanOptions->Version < DOKAN_MINIMUM_COMPATIBLE_VERSION) {
    DokanDbgPrintW(
        L"Dokan Error: Incompatible version (%d), minimum is (%d) \n",
        DokanOptions->Version, DOKAN_MINIMUM_COMPATIBLE_VERSION);
    return DOKAN_VERSION_ERROR;
  }

  CheckAllocationUnitSectorSize(DokanOptions);

  if (DokanOptions->ThreadCount != 0) {
	  DbgPrintW(L"Dokan Warning: DOKAN_OPTIONS::ThreadCount is no longer used.\n");
  }

  instance = NewDokanInstance();

  if(!instance) {
	  return DOKAN_DRIVER_INSTALL_ERROR;
  }

  instance->DokanOptions = DokanOptions;
  instance->DokanOperations = DokanOperations;

  instance->GlobalDevice = CreateFile(DOKAN_GLOBAL_DEVICE_NAME, // lpFileName
                      GENERIC_READ | GENERIC_WRITE,				// dwDesiredAccess
                      FILE_SHARE_READ | FILE_SHARE_WRITE,		// dwShareMode
                      NULL,										// lpSecurityAttributes
                      OPEN_EXISTING,							// dwCreationDistribution
                      0,										// dwFlagsAndAttributes
                      NULL										// hTemplateFile
                      );

  if(instance->GlobalDevice == INVALID_HANDLE_VALUE) {

	  DWORD lastError = GetLastError();

	  DokanDbgPrintW(L"Dokan Error: CreatFile failed to open %s: %d\n",
		  DOKAN_GLOBAL_DEVICE_NAME, lastError);

	  DeleteDokanInstance(instance);

	  return DOKAN_DRIVER_INSTALL_ERROR;
  }

  DbgPrint("Global device opened\n");

  if (DokanOptions->MountPoint != NULL) {
    
	  wcscpy_s(instance->MountPoint, sizeof(instance->MountPoint) / sizeof(WCHAR), DokanOptions->MountPoint);

    if (IsMountPointDriveLetter(instance->MountPoint)) {
      if (!CheckDriveLetterAvailability(instance->MountPoint[0])) {

        DokanDbgPrint("Dokan Error: CheckDriveLetterAvailability Failed\n");

		DeleteDokanInstance(instance);

        return DOKAN_MOUNT_ERROR;
      }
    }
  }

  if (DokanOptions->UNCName != NULL) {

    wcscpy_s(instance->UNCName, sizeof(instance->UNCName) / sizeof(WCHAR), DokanOptions->UNCName);
  }

  if (!DokanStart(instance)) {
    
	  DeleteDokanInstance(instance);

    return DOKAN_START_ERROR;
  }

  GetRawDeviceName(instance->DeviceName, rawDeviceName, MAX_PATH);

  instance->Device = CreateFile(rawDeviceName,							// lpFileName
								  GENERIC_READ | GENERIC_WRITE,			// dwDesiredAccess
								  FILE_SHARE_READ | FILE_SHARE_WRITE,	// dwShareMode
								  NULL,									// lpSecurityAttributes
								  OPEN_EXISTING,						// dwCreationDistribution
								  FILE_FLAG_OVERLAPPED,					// dwFlagsAndAttributes
								  NULL									// hTemplateFile
								  );

  if(instance->Device == INVALID_HANDLE_VALUE) {

	  DWORD lastError = GetLastError();

	  DokanDbgPrintW(L"Dokan Error: CreatFile failed to open %s: %d\n",
		  rawDeviceName, lastError);

	  DeleteDokanInstance(instance);

	  return DOKAN_DRIVER_INSTALL_ERROR;
  }

  instance->ThreadInfo.IoCompletion = CreateThreadpoolIo(instance->Device, DokanLoop, instance, &instance->ThreadInfo.CallbackEnvironment);

  if(!instance->ThreadInfo.IoCompletion) {

	  DokanDbgPrintW(L"Dokan Error: Failed to allocate IO completion port.\n");

	  DeleteDokanInstance(instance);

	  return DOKAN_DRIVER_INSTALL_ERROR;
  }

  instance->ThreadInfo.KeepAliveTimer = CreateThreadpoolTimer(DokanKeepAlive, (PVOID)instance->Device, &instance->ThreadInfo.CallbackEnvironment);

  if(!instance->ThreadInfo.KeepAliveTimer) {

	  SendReleaseIRP(instance->DeviceName);

	  DokanDbgPrint("Dokan Error: Failed to create keep alive timer.\n");

	  DeleteDokanInstance(instance);

	  return DOKAN_START_ERROR;
  }

  // convert milliseconds into 100 nanosecond units and make it negative for relative time
  timerDueTime.QuadPart = (ULONGLONG)(-(DOKAN_KEEPALIVE_TIME * (1000000 / 100)));

  // Start Keep Alive thread
  SetThreadpoolTimer(instance->ThreadInfo.KeepAliveTimer, (FILETIME*)&timerDueTime.QuadPart, DOKAN_KEEPALIVE_TIME, 0);

  if(!StartDeviceIO(instance, NULL)) {

	  DokanDbgPrint("Dokan Error: Failed to  start device IO.\n");
	  DeleteDokanInstance(instance);

	  return DOKAN_START_ERROR;
  }
  else {

	  DbgPrint("Dokan Information: Started device IO.\n");
  }

  if (!DokanMount(instance->MountPoint, instance->DeviceName, DokanOptions)) {
	  
	  SendReleaseIRP(instance->DeviceName);
	  
	  DokanDbgPrint("Dokan Error: DokanMount Failed\n");
	  
	  DeleteDokanInstance(instance);

	  return DOKAN_MOUNT_ERROR;
  }

  // Here we should have been mounter by mountmanager thanks to
  // IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME
  DbgPrintW(L"Dokan Information: mounted: %s -> %s\n", instance->MountPoint, instance->DeviceName);

  if (DokanOperations->Mounted) {
	  
	  DOKAN_MOUNTED_INFO mountedInfo;
	  
	  mountedInfo.DokanOptions = DokanOptions;
	  mountedInfo.ThreadPool = instance->ThreadInfo.ThreadPool;
	  
	  DokanOperations->Mounted(&mountedInfo);
  }

  if(DokanInstance) {
	  
	  *DokanInstance = instance;
  }

  /*CloseHandle(device);

  if (DokanOperations->Unmounted) {
    DOKAN_FILE_INFO fileInfo;
    RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));
    fileInfo.DokanOptions = DokanOptions;
    // ignore return value
    DokanOperations->Unmounted(&fileInfo);
  }

  DbgPrint("\nunload\n");*/

  return DOKAN_SUCCESS;
}

LPWSTR
GetRawDeviceName(LPCWSTR DeviceName, LPWSTR DestinationBuffer,
                 rsize_t DestinationBufferSizeInElements) {
  if (DeviceName && DestinationBuffer && DestinationBufferSizeInElements > 0) {
    wcscpy_s(DestinationBuffer, DestinationBufferSizeInElements, L"\\\\.");
    wcscat_s(DestinationBuffer, DestinationBufferSizeInElements, DeviceName);
  }

  return DestinationBuffer;
}

void ALIGN_ALLOCATION_SIZE(PLARGE_INTEGER size, PDOKAN_OPTIONS DokanOptions) {
  long long r = size->QuadPart % DokanOptions->AllocationUnitSize;
  size->QuadPart =
      (size->QuadPart + (r > 0 ? DokanOptions->AllocationUnitSize - r : 0));
}

void SetupIOEventForProcessing(DOKAN_IO_EVENT *EventInfo) {

	EventInfo->DokanOpenInfo = (PDOKAN_OPEN_INFO)(UINT_PTR)EventInfo->KernelInfo.EventContext.Context;
	EventInfo->DokanFileInfo.DokanContext = EventInfo;
	EventInfo->DokanFileInfo.ProcessId = EventInfo->KernelInfo.EventContext.ProcessId;
	EventInfo->DokanFileInfo.DokanOptions = EventInfo->DokanInstance->DokanOptions;

	if(EventInfo->DokanOpenInfo) {

		EventInfo->DokanFileInfo.Context = InterlockedAdd64(&EventInfo->DokanOpenInfo->UserContext, 0);
		EventInfo->DokanFileInfo.IsDirectory = EventInfo->DokanOpenInfo->IsDirectory;

		if(EventInfo->KernelInfo.EventContext.FileFlags & DOKAN_DELETE_ON_CLOSE) {

			EventInfo->DokanFileInfo.DeleteOnClose = 1;
		}

		if(EventInfo->KernelInfo.EventContext.FileFlags & DOKAN_PAGING_IO) {

			EventInfo->DokanFileInfo.PagingIo = 1;
		}

		if(EventInfo->KernelInfo.EventContext.FileFlags & DOKAN_WRITE_TO_END_OF_FILE) {

			EventInfo->DokanFileInfo.WriteToEndOfFile = 1;
		}

		if(EventInfo->KernelInfo.EventContext.FileFlags & DOKAN_SYNCHRONOUS_IO) {

			EventInfo->DokanFileInfo.SynchronousIo = 1;
		}

		if(EventInfo->KernelInfo.EventContext.FileFlags & DOKAN_NOCACHE) {

			EventInfo->DokanFileInfo.Nocache = 1;
		}
	}

	assert(EventInfo->EventResult == NULL);
}

void OnDeviceIoCtlFailed(PDOKAN_INSTANCE Dokan, ULONG IoResult) {

	// disable keep alive timer
	SetThreadpoolTimer(Dokan->ThreadInfo.KeepAliveTimer, NULL, 0, 0);

	DokanDbgPrintW(L"Dokan Warning: Closing IO processing for dokan instance %s with error code 0x%x and unmounting volume.\n", Dokan->DeviceName, IoResult);

	DokanNotifyUnmounted(Dokan);

	// set the device to a closed state
	SetEvent(Dokan->DeviceClosedWaitHandle);
}

void ProcessIOEvent(
	PDOKAN_INSTANCE Dokan,
	DOKAN_OVERLAPPED *Overlapped,
	ULONG IoResult,
	ULONG_PTR NumberOfBytesTransferred) {

	DOKAN_IO_EVENT *currentIoEvent = (DOKAN_IO_EVENT*)Overlapped->OutputPayload;
	currentIoEvent->EventSize = (ULONG)NumberOfBytesTransferred;

	assert(currentIoEvent->EventResult == NULL && currentIoEvent->EventResultSize == 0);

	if(IoResult != NO_ERROR) {

		PushIoEventBuffer(currentIoEvent);
		PushOverlapped(Overlapped);

		OnDeviceIoCtlFailed(Dokan, IoResult);

		return;
	}

	// reuse Overlapped and queue up another async IO operation
	ResetOverlapped(Overlapped);

	BOOL restartDeviceIOSucceeded = StartDeviceIO(Dokan, Overlapped);
	DWORD lastError = ERROR_SUCCESS;
	
	if(!restartDeviceIOSucceeded) {
		
		lastError = GetLastError();
		SetLastError(ERROR_SUCCESS);
	}

	// begin processing IO event
	SetupIOEventForProcessing(currentIoEvent);

	if(NumberOfBytesTransferred > 0) {

		//MajorFunction
		switch(currentIoEvent->KernelInfo.EventContext.MajorFunction) {
		
		case IRP_MJ_CREATE:
			BeginDispatchCreate(currentIoEvent);
			break;
		case IRP_MJ_CLEANUP:
			DispatchCleanup(currentIoEvent);
			break;
		case IRP_MJ_CLOSE:
			DispatchClose(currentIoEvent);
			break;
		case IRP_MJ_DIRECTORY_CONTROL:
			BeginDispatchDirectoryInformation(currentIoEvent);
			break;
		case IRP_MJ_READ:
			BeginDispatchRead(currentIoEvent);
			break;
		case IRP_MJ_WRITE:
			BeginDispatchWrite(currentIoEvent);
			break;
		case IRP_MJ_QUERY_INFORMATION:
			BeginDispatchQueryInformation(currentIoEvent);
			break;
		case IRP_MJ_QUERY_VOLUME_INFORMATION:
			BeginDispatchQueryVolumeInformation(currentIoEvent);
			break;
		case IRP_MJ_LOCK_CONTROL:
			BeginDispatchLock(currentIoEvent);
			break;
		case IRP_MJ_SET_INFORMATION:
			BeginDispatchSetInformation(currentIoEvent);
			break;
		case IRP_MJ_FLUSH_BUFFERS:
			BeginDispatchFlush(currentIoEvent);
			break;
		case IRP_MJ_QUERY_SECURITY:
			BeginDispatchQuerySecurity(currentIoEvent);
			break;
		case IRP_MJ_SET_SECURITY:
			BeginDispatchSetSecurity(currentIoEvent);
			break;
		default:
			DokanDbgPrintW(L"Dokan Warning: Unsupported IRP 0x%x.\n", currentIoEvent->KernelInfo.EventContext.MajorFunction);
			PushIoEventBuffer(currentIoEvent);
			break;
		}
	}
	else {

		PushIoEventBuffer(currentIoEvent);
		DbgPrint("ReturnedLength %d\n", NumberOfBytesTransferred);
	}

	if(!restartDeviceIOSucceeded) {

		// NOTE: This MUST be handled at the end of this method. OnDeviceIoCtlFailed() will unmount the volume
		// at which point the user-mode driver needs to wait on all outstanding IO operations in its Unmount()
		// callback before returning control to this handler. If OnDeviceIoCtlFailed() is called above there will
		// be 1 pending IO operation which could potentially queue an async operation. If everything gets cleaned up
		// before that operation completes then bad things could happen.

		OnDeviceIoCtlFailed(Dokan, lastError);
	}
}

void ProcessWriteSizeEvent(
	DOKAN_OVERLAPPED *Overlapped,
	ULONG IoResult,
	ULONG_PTR NumberOfBytesTransferred) {

	DOKAN_IO_EVENT *inputIoEvent = (DOKAN_IO_EVENT*)Overlapped->InputPayload;
	DOKAN_IO_EVENT *outputIoEvent = (DOKAN_IO_EVENT*)Overlapped->OutputPayload;

	assert(inputIoEvent && outputIoEvent);
	assert(inputIoEvent->EventResult);
	assert(inputIoEvent->EventInfo.WriteFile.NumberOfBytesWritten == 0);
	assert(outputIoEvent->DokanInstance);

	PushOverlapped(Overlapped);

	if(IoResult != NO_ERROR) {

		// This will push the input buffer so we don't need to manually do that
		DokanEndDispatchWrite(&inputIoEvent->EventInfo.WriteFile, STATUS_INTERNAL_ERROR);

		PushIoEventBuffer(outputIoEvent);

		return;
	}

	FreeIoEventResult(inputIoEvent->EventResult, Overlapped->Flags);
	PushIoEventBuffer(inputIoEvent);

	outputIoEvent->EventSize = (ULONG)NumberOfBytesTransferred;

	SetupIOEventForProcessing(outputIoEvent);

	BeginDispatchWrite(outputIoEvent);
}

// Process the result of SendEventInformation()
void ProcessKernelResultEvent(
	DOKAN_OVERLAPPED *Overlapped) {

	PEVENT_INFORMATION eventResult = (PEVENT_INFORMATION)Overlapped->InputPayload;

	assert(eventResult);

	FreeIoEventResult(eventResult, Overlapped->Flags);

	PushOverlapped(Overlapped);
}

VOID CALLBACK DokanLoop(
	_Inout_     PTP_CALLBACK_INSTANCE Instance,
	_Inout_opt_ PVOID                 Context,
	_Inout_opt_ PVOID                 Overlapped,
	_In_        ULONG                 IoResult,
	_In_        ULONG_PTR             NumberOfBytesTransferred,
	_Inout_     PTP_IO                Io
) {
  
  UNREFERENCED_PARAMETER(Instance);
  UNREFERENCED_PARAMETER(Io);

  PDOKAN_INSTANCE dokan = (PDOKAN_INSTANCE)Context;
  DOKAN_OVERLAPPED *overlapped = (DOKAN_OVERLAPPED*)Overlapped;

  assert(dokan);

  switch(overlapped->PayloadType) {
	  
  case DOKAN_OVERLAPPED_TYPE_IOEVENT:
	  ProcessIOEvent(dokan, overlapped, IoResult, NumberOfBytesTransferred);
	  break;
  case DOKAN_OVERLAPPED_TYPE_IOEVENT_WRITE_SIZE:
	  ProcessWriteSizeEvent(overlapped, IoResult, NumberOfBytesTransferred);
	  break;
  case DOKAN_OVERLAPPED_TYPE_IOEVENT_RESULT:
	  ProcessKernelResultEvent(overlapped);
	  break;
  default:
	  DokanDbgPrintW(L"Unrecognized overlapped type of %d for dokan instance %s. The payload is probably being leaked.\n", overlapped->PayloadType, dokan->DeviceName);
	  PushOverlapped(overlapped);
	  break;
  }
}

BOOL SendIoEventResult(DOKAN_IO_EVENT *EventInfo) {

	assert(EventInfo->EventResult);

	if(EventInfo->DokanOpenInfo) {

		InterlockedExchange64(&EventInfo->DokanOpenInfo->UserContext, EventInfo->DokanFileInfo.Context);
	}

	BOOL result = SendEventInformation(EventInfo->EventResult, EventInfo->DokanInstance, EventInfo->Flags);

	if(!result) {

		FreeIoEventResult(EventInfo->EventResult, EventInfo->Flags);
	}

	PushIoEventBuffer(EventInfo);

	return result;
}

BOOL SendEventInformation(PEVENT_INFORMATION EventInfo,
	PDOKAN_INSTANCE DokanInstance, DOKAN_IO_EVENT_FLAGS EventFlags) {
  
  DOKAN_OVERLAPPED *overlapped = NULL;
  DWORD lastError = 0;
  DWORD eventSize = max(sizeof(EVENT_INFORMATION), DOKAN_EVENT_INFO_ALLOC_SIZE(EventInfo->BufferLength));

  DbgPrint("Dokan Information: SendEventInformation() with NTSTATUS result 0x%x and context 0x%lx\n",
	  EventInfo->Status, EventInfo->Context);

  overlapped = PopOverlapped();

  if(!overlapped) {

	  DbgPrint("Dokan Error: Failed to allocate overlapped info.\n");

	  return FALSE;
  }

  overlapped->InputPayload = EventInfo;
  overlapped->PayloadType = DOKAN_OVERLAPPED_TYPE_IOEVENT_RESULT;
  overlapped->Flags = EventFlags;

  StartThreadpoolIo(DokanInstance->ThreadInfo.IoCompletion);

  if(!DeviceIoControl(
	  DokanInstance->Device,		// Handle to device
	  IOCTL_EVENT_INFO,				// IO Control code
	  EventInfo,					// Input Buffer to driver.
	  eventSize,					// Length of input buffer in bytes.
	  NULL,							// Output Buffer from driver.
	  0,							// Length of output buffer in bytes.
	  NULL,							// Bytes placed in buffer.
	  (OVERLAPPED*)overlapped       // asynchronous call
  )) {

	  lastError = GetLastError();

	  if(lastError != ERROR_IO_PENDING) {

		  DbgPrint("Dokan Error: Dokan device result ioctl failed for wait with code %d.\n", lastError);

		  CancelThreadpoolIo(DokanInstance->ThreadInfo.IoCompletion);

		  PushOverlapped(overlapped);

		  return FALSE;
	  }
  }

  return TRUE;
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

void CreateDispatchCommon(DOKAN_IO_EVENT *EventInfo, ULONG SizeOfEventInfo) {

  assert(EventInfo != NULL);
  assert(EventInfo->EventResult == NULL && EventInfo->EventResultSize == 0);

  if(SizeOfEventInfo <= DOKAN_EVENT_INFO_DEFAULT_BUFFER_SIZE) {

	  EventInfo->EventResult = PopEventResult();
	  EventInfo->EventResultSize = DOKAN_EVENT_INFO_DEFAULT_SIZE;
	  EventInfo->Flags |= DOKAN_IO_EVENT_FLAGS_POOLED_RESULT;
  }
  else {

	  EventInfo->EventResultSize = DOKAN_EVENT_INFO_ALLOC_SIZE(SizeOfEventInfo);
	  EventInfo->EventResult = (PEVENT_INFORMATION)malloc(EventInfo->EventResultSize);
	  EventInfo->Flags &= ~DOKAN_IO_EVENT_FLAGS_POOLED_RESULT;

	  RtlZeroMemory(EventInfo->EventResult, EventInfo->EventResultSize);
  }

  assert(EventInfo->EventResult && EventInfo->EventResultSize >= DOKAN_EVENT_INFO_ALLOC_SIZE(SizeOfEventInfo));

  EventInfo->EventResult->SerialNumber = EventInfo->KernelInfo.EventContext.SerialNumber;
  EventInfo->EventResult->Context = EventInfo->KernelInfo.EventContext.Context;
}

void FreeIoEventResult(PEVENT_INFORMATION EventResult, DOKAN_IO_EVENT_FLAGS Flags) {

	if(EventResult) {
		
		if(Flags & DOKAN_IO_EVENT_FLAGS_POOLED_RESULT) {

			PushEventResult(EventResult);
		}
		else {

			free(EventResult);
		}
	}
}

// ask driver to release all pending IRP to prepare for Unmount.
BOOL SendReleaseIRP(LPCWSTR DeviceName) {
  ULONG returnedLength;
  WCHAR rawDeviceName[MAX_PATH];

  DbgPrint("send release to %ws\n", DeviceName);

  if (!SendToDevice(GetRawDeviceName(DeviceName, rawDeviceName, MAX_PATH),
                    IOCTL_EVENT_RELEASE, NULL, 0, NULL, 0, &returnedLength)) {

    DbgPrint("Failed to unmount device:%ws\n", DeviceName);
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

        DbgPrint("send global release for %ws\n", MountPoint);

        if (!SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, IOCTL_EVENT_RELEASE,
                          szMountPoint, inputLength, NULL, 0,
                          &returnedLength)) {

          DbgPrint("Failed to unmount: %ws\n", MountPoint);
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

BOOL DokanStart(PDOKAN_INSTANCE Instance) {
  EVENT_START eventStart;
  EVENT_DRIVER_INFO driverInfo;
  ULONG returnedLength = 0;

  ZeroMemory(&eventStart, sizeof(EVENT_START));
  ZeroMemory(&driverInfo, sizeof(EVENT_DRIVER_INFO));

  eventStart.UserVersion = DOKAN_DRIVER_VERSION;
  if (Instance->DokanOptions->Options & DOKAN_OPTION_ALT_STREAM) {
    eventStart.Flags |= DOKAN_EVENT_ALTERNATIVE_STREAM_ON;
  }
  if (Instance->DokanOptions->Options & DOKAN_OPTION_NETWORK) {
    eventStart.DeviceType = DOKAN_NETWORK_FILE_SYSTEM;
  }
  if (Instance->DokanOptions->Options & DOKAN_OPTION_REMOVABLE) {
    eventStart.Flags |= DOKAN_EVENT_REMOVABLE;
  }
  if (Instance->DokanOptions->Options & DOKAN_OPTION_WRITE_PROTECT) {
    eventStart.Flags |= DOKAN_EVENT_WRITE_PROTECT;
  }
  if (Instance->DokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) {
    eventStart.Flags |= DOKAN_EVENT_MOUNT_MANAGER;
  }
  if (Instance->DokanOptions->Options & DOKAN_OPTION_CURRENT_SESSION) {
    eventStart.Flags |= DOKAN_EVENT_CURRENT_SESSION;
  }
  if (Instance->DokanOptions->Options & DOKAN_OPTION_FILELOCK_USER_MODE) {
    eventStart.Flags |= DOKAN_EVENT_FILELOCK_USER_MODE;
  }

  memcpy_s(eventStart.MountPoint, sizeof(eventStart.MountPoint),
           Instance->MountPoint, sizeof(Instance->MountPoint));
  memcpy_s(eventStart.UNCName, sizeof(eventStart.UNCName), Instance->UNCName,
           sizeof(Instance->UNCName));

  eventStart.IrpTimeout = Instance->DokanOptions->Timeout;

  SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, IOCTL_EVENT_START, &eventStart,
               sizeof(EVENT_START), &driverInfo, sizeof(EVENT_DRIVER_INFO),
               &returnedLength);

  if (driverInfo.Status == DOKAN_START_FAILED) {
    if (driverInfo.DriverVersion != eventStart.UserVersion) {
      DokanDbgPrint("Dokan Error: driver version mismatch, driver %X, dll %X\n",
                    driverInfo.DriverVersion, eventStart.UserVersion);
    } else {
      DokanDbgPrint("Dokan Error: driver start error\n");
    }
    return FALSE;
  } else if (driverInfo.Status == DOKAN_MOUNTED) {
    Instance->MountId = driverInfo.MountId;
    Instance->DeviceNumber = driverInfo.DeviceNumber;
    wcscpy_s(Instance->DeviceName, sizeof(Instance->DeviceName) / sizeof(WCHAR),
             driverInfo.DeviceName);
    return TRUE;
  }
  return FALSE;
}

BOOL DOKANAPI DokanSetDebugMode(ULONG Mode) {
  ULONG returnedLength;
  return SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, IOCTL_SET_DEBUG_MODE, &Mode,
                      sizeof(ULONG), NULL, 0, &returnedLength);
}

BOOL SendToDevice(LPCWSTR DeviceName, DWORD IoControlCode, PVOID InputBuffer,
                  ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                  PULONG ReturnedLength) {
  HANDLE device;
  BOOL status;

  device = CreateFile(DeviceName,                         // lpFileName
                      GENERIC_READ | GENERIC_WRITE,       // dwDesiredAccess
                      FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
                      NULL,          // lpSecurityAttributes
                      OPEN_EXISTING, // dwCreationDistribution
                      0,             // dwFlagsAndAttributes
                      NULL           // hTemplateFile
                      );

  if (device == INVALID_HANDLE_VALUE) {

    DWORD dwErrorCode = GetLastError();
    DbgPrint("Dokan Error: Failed to open %ws with code %d\n", DeviceName,
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
	  
	  DWORD dwErrorCode = GetLastError();

    DbgPrint("DokanError: Ioctl failed with code %d\n", dwErrorCode);
    return FALSE;
  }

  return TRUE;
}

BOOL DOKANAPI DokanGetMountPointList(PDOKAN_CONTROL list, ULONG length,
                                     BOOL uncOnly, PULONG nbRead) {
  ULONG returnedLength = 0;

  DOKAN_CONTROL dokanControl[DOKAN_MAX_INSTANCES];
  ZeroMemory(dokanControl, sizeof(dokanControl));
  *nbRead = 0;

  if (SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, IOCTL_EVENT_MOUNTPOINT_LIST, NULL,
                   0, dokanControl, sizeof(dokanControl), &returnedLength)) {
    for (int i = 0; i < DOKAN_MAX_INSTANCES; ++i) {
      if (wcscmp(dokanControl[i].DeviceName, L"") == 0) {
        break;
      }
      if (!uncOnly || wcscmp(dokanControl[i].UNCName, L"") != 0) {
        if (length < ((*nbRead) + 1))
          return TRUE;

        CopyMemory(&list[*nbRead], &dokanControl[i], sizeof(DOKAN_CONTROL));
        (*nbRead)++;
      }
    }

    return TRUE;
  }

  return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  UNREFERENCED_PARAMETER(Reserved);
  UNREFERENCED_PARAMETER(Instance);

  switch (Reason) {
  case DLL_PROCESS_ATTACH: {
#if _MSC_VER < 1300
    InitializeCriticalSection(&g_InstanceCriticalSection);
	InitializeCriticalSection(&g_EventBufferCriticalSection);
	InitializeCriticalSection(&g_OverlappedCriticalSection);
	InitializeCriticalSection(&g_EventResultCriticalSection);
	InitializeCriticalSection(&g_FileInfoCriticalSection);
	InitializeCriticalSection(&g_DirectoryListCriticalSection);
#else
    InitializeCriticalSectionAndSpinCount(&g_InstanceCriticalSection, 0x80000400);
	InitializeCriticalSectionAndSpinCount(&g_EventBufferCriticalSection, 0x80000400);
	InitializeCriticalSectionAndSpinCount(&g_OverlappedCriticalSection, 0x80000400);
	InitializeCriticalSectionAndSpinCount(&g_EventResultCriticalSection, 0x80000400);
	InitializeCriticalSectionAndSpinCount(&g_FileInfoCriticalSection, 0x80000400);
	InitializeCriticalSectionAndSpinCount(&g_DirectoryListCriticalSection, 0x80000400);
#endif

    InitializeListHead(&g_InstanceList);
	InitializeThreadPool(Instance);

	g_EventBufferPool = DokanVector_AllocWithCapacity(sizeof(void*), DOKAN_IO_EVENT_POOL_SIZE);
	g_OverlappedPool = DokanVector_AllocWithCapacity(sizeof(void*), DOKAN_OVERLAPPED_POOL_SIZE);
	g_EventResultPool = DokanVector_AllocWithCapacity(sizeof(void*), DOKAN_OVERLAPPED_POOL_SIZE);
	g_FileInfoPool = DokanVector_AllocWithCapacity(sizeof(void*), DOKAN_OVERLAPPED_POOL_SIZE);
	g_DirectoryListPool = DokanVector_AllocWithCapacity(sizeof(void*), DOKAN_DIRECTORY_LIST_POOL_SIZE);

  } break;
  case DLL_PROCESS_DETACH: {

    EnterCriticalSection(&g_InstanceCriticalSection);
	{
		while(!IsListEmpty(&g_InstanceList)) {

			PLIST_ENTRY entry = RemoveHeadList(&g_InstanceList);

			PDOKAN_INSTANCE instance =
				CONTAINING_RECORD(entry, DOKAN_INSTANCE, ListEntry);

			DokanCloseHandle((DOKAN_HANDLE)instance);
		}
	}
    LeaveCriticalSection(&g_InstanceCriticalSection);

	CleanupThreadpool();

	//////////////////// IO event buffer object pool ////////////////////
	{
		EnterCriticalSection(&g_EventBufferCriticalSection);
		{
			for(size_t i = 0; i < DokanVector_GetCount(g_EventBufferPool); ++i) {

				FreeIOEventBuffer(*(DOKAN_IO_EVENT**)DokanVector_GetItem(g_EventBufferPool, i));
			}

			DokanVector_Free(g_EventBufferPool);
			g_EventBufferPool = NULL;
		}
		LeaveCriticalSection(&g_EventBufferCriticalSection);

		DeleteCriticalSection(&g_EventBufferCriticalSection);
	}

	//////////////////// Overlapped object pool ////////////////////
	{
		EnterCriticalSection(&g_OverlappedCriticalSection);
		{
			for(size_t i = 0; i < DokanVector_GetCount(g_OverlappedPool); ++i) {

				FreeOverlapped(*(DOKAN_OVERLAPPED**)DokanVector_GetItem(g_OverlappedPool, i));
			}

			DokanVector_Free(g_OverlappedPool);
			g_OverlappedPool = NULL;
		}
		LeaveCriticalSection(&g_OverlappedCriticalSection);

		DeleteCriticalSection(&g_OverlappedCriticalSection);
	}

	//////////////////// Event result object pool ////////////////////
	{
		EnterCriticalSection(&g_EventResultCriticalSection);
		{
			for(size_t i = 0; i < DokanVector_GetCount(g_EventResultPool); ++i) {

				FreeEventResult(*(EVENT_INFORMATION**)DokanVector_GetItem(g_EventResultPool, i));
			}

			DokanVector_Free(g_EventResultPool);
			g_EventResultPool = NULL;
		}
		LeaveCriticalSection(&g_EventResultCriticalSection);

		DeleteCriticalSection(&g_EventResultCriticalSection);
	}

	//////////////////// File info object pool ////////////////////
	{
		EnterCriticalSection(&g_FileInfoCriticalSection);
		{
			for(size_t i = 0; i < DokanVector_GetCount(g_FileInfoPool); ++i) {

				FreeFileOpenInfo(*(DOKAN_OPEN_INFO**)DokanVector_GetItem(g_FileInfoPool, i));
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
			for(size_t i = 0; i < DokanVector_GetCount(g_DirectoryListPool); ++i) {

				DokanVector_Free(*(DOKAN_VECTOR**)DokanVector_GetItem(g_DirectoryListPool, i));
			}

			DokanVector_Free(g_DirectoryListPool);
			g_DirectoryListPool = NULL;
		}
		LeaveCriticalSection(&g_DirectoryListCriticalSection);

		DeleteCriticalSection(&g_DirectoryListCriticalSection);
	}

	//////////////////// Object pool cleanup finished ////////////////////

    DeleteCriticalSection(&g_InstanceCriticalSection);

  } break;
  }
  return TRUE;
}

VOID DOKANAPI DokanMapKernelToUserCreateFileFlags(
    ULONG FileAttributes, ULONG CreateOptions, ULONG CreateDisposition,
    DWORD *outFileAttributesAndFlags, DWORD *outCreationDisposition) {
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
}

DOKAN_API PTP_POOL DOKAN_CALLBACK DokanGetThreadPool() {

	PTP_POOL threadPool;

	EnterCriticalSection(&g_InstanceCriticalSection);

	threadPool = g_ThreadPool;

	LeaveCriticalSection(&g_InstanceCriticalSection);

	return threadPool;
}