/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

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

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <conio.h>
#include <tchar.h>
#include <process.h>
#include <locale.h>
#include <ntstatus.h>
#include "fileinfo.h"
#include "dokani.h"
#include "list.h"

#define DokanMapKernelBit(dest, src, userBit, kernelBit) if(((src) & (kernelBit)) == (kernelBit)) (dest) |= (userBit)

// DokanOptions->DebugMode is ON?
BOOL	g_DebugMode = TRUE;

// DokanOptions->UseStdErr is ON?
BOOL	g_UseStdErr = FALSE;


CRITICAL_SECTION	g_InstanceCriticalSection;
LIST_ENTRY			g_InstanceList;	

VOID DOKANAPI
DokanUseStdErr(BOOL Status)
{
	g_UseStdErr = Status;
}

VOID DOKANAPI
DokanDebugMode(BOOL Status)
{
	g_DebugMode = Status;
}

PDOKAN_INSTANCE 
NewDokanInstance()
{
	PDOKAN_INSTANCE instance = (PDOKAN_INSTANCE)malloc(sizeof(DOKAN_INSTANCE));
    if (instance == NULL)
        return NULL;

	ZeroMemory(instance, sizeof(DOKAN_INSTANCE));

#if _MSC_VER < 1300
	InitializeCriticalSection(&instance->CriticalSection);
#else
	InitializeCriticalSectionAndSpinCount(
		&instance->CriticalSection, 0x80000400);
#endif

	InitializeListHead(&instance->ListEntry);

	EnterCriticalSection(&g_InstanceCriticalSection);
	InsertTailList(&g_InstanceList, &instance->ListEntry);
	LeaveCriticalSection(&g_InstanceCriticalSection);

	return instance;
}

VOID 
DeleteDokanInstance(PDOKAN_INSTANCE Instance)
{
	DeleteCriticalSection(&Instance->CriticalSection);

	EnterCriticalSection(&g_InstanceCriticalSection);
	RemoveEntryList(&Instance->ListEntry);
	LeaveCriticalSection(&g_InstanceCriticalSection);

	free(Instance);
}

BOOL
IsValidDriveLetter(WCHAR DriveLetter)
{
	return (L'b' <= DriveLetter && DriveLetter <= L'z') ||
		(L'B' <= DriveLetter && DriveLetter <= L'Z');
}

int
CheckMountPoint(LPCWSTR	MountPoint)
{
	size_t	length = wcslen(MountPoint);

	if ((length == 1) ||
		(length == 2 && MountPoint[1] == L':') ||
		(length == 3 && MountPoint[1] == L':' && MountPoint[2] == L'\\')) {
		WCHAR driveLetter = MountPoint[0];
		
		if (IsValidDriveLetter(driveLetter)) {
			return DOKAN_SUCCESS;
		} else {
			DokanDbgPrintW(L"Dokan Error: bad drive letter %s\n", MountPoint);
			return DOKAN_DRIVE_LETTER_ERROR;
		}
	} else if (length > 3) {
		HANDLE handle = CreateFile(
						MountPoint, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
						FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
		if (handle == INVALID_HANDLE_VALUE) {
			DokanDbgPrintW(L"Dokan Error: bad mount point %s\n", MountPoint);
			return DOKAN_MOUNT_POINT_ERROR;
		}
		CloseHandle(handle);
		return DOKAN_SUCCESS;
	}
	return DOKAN_MOUNT_POINT_ERROR;
}

int DOKANAPI
DokanMain(PDOKAN_OPTIONS DokanOptions, PDOKAN_OPERATIONS DokanOperations)
{
	ULONG	threadNum = 0;
	ULONG	i;
	HANDLE	device;
	HANDLE	threadIds[DOKAN_MAX_THREAD];
	BOOL	useMountPoint = FALSE;
	PDOKAN_INSTANCE instance;

	g_DebugMode = DokanOptions->Options & DOKAN_OPTION_DEBUG;
	g_UseStdErr = DokanOptions->Options & DOKAN_OPTION_STDERR;

	if (g_DebugMode) {
		DbgPrintW(L"Dokan: debug mode on\n");
	}

	if (g_UseStdErr) {
		DbgPrintW(L"Dokan: use stderr\n");
		g_DebugMode = TRUE;
	}

	if (DokanOptions->ThreadCount == 0) {
		DokanOptions->ThreadCount = 5;

	} else if ((DOKAN_MAX_THREAD - 1) < DokanOptions->ThreadCount) {
		// DOKAN_MAX_THREAD includes DokanKeepAlive thread, so 
		// available thread is DOKAN_MAX_THREAD -1
		DokanDbgPrintW(L"Dokan Error: too many thread count %d\n",
			DokanOptions->ThreadCount);
		DokanOptions->ThreadCount = DOKAN_MAX_THREAD - 1;
	}

	if (DOKAN_MOUNT_POINT_SUPPORTED_VERSION <= DokanOptions->Version &&
		DokanOptions->MountPoint) {
		int error = CheckMountPoint(DokanOptions->MountPoint);
		if (error != DOKAN_SUCCESS) {
			return error;
		}
		useMountPoint = TRUE;
	} else if (!IsValidDriveLetter((WCHAR)DokanOptions->Version)) {
		// Older versions use the first 2 bytes of DokanOptions struct as DriveLetter.
		DokanDbgPrintW(L"Dokan Error: bad drive letter %wc\n", (WCHAR)DokanOptions->Version);
		return DOKAN_DRIVE_LETTER_ERROR;
	}

	device = CreateFile(
					DOKAN_GLOBAL_DEVICE_NAME,			// lpFileName
					GENERIC_READ|GENERIC_WRITE,			// dwDesiredAccess
					FILE_SHARE_READ|FILE_SHARE_WRITE,	// dwShareMode
					NULL,								// lpSecurityAttributes
					OPEN_EXISTING,						// dwCreationDistribution
					0,									// dwFlagsAndAttributes
					NULL								// hTemplateFile
                    );

	if (device == INVALID_HANDLE_VALUE){
		DokanDbgPrintW(L"Dokan Error: CreatFile Failed %s: %d\n", 
			DOKAN_GLOBAL_DEVICE_NAME, GetLastError());
		return DOKAN_DRIVER_INSTALL_ERROR;
	}

	DbgPrint("device opened\n");

	instance = NewDokanInstance();
	instance->DokanOptions = DokanOptions;
	instance->DokanOperations = DokanOperations;
	if (useMountPoint) {
		wcscpy_s(instance->MountPoint, sizeof(instance->MountPoint) / sizeof(WCHAR),
				DokanOptions->MountPoint);
	} else {
		// Older versions use the first 2 bytes of DokanOptions struct as DriveLetter.
		instance->MountPoint[0] = (WCHAR)DokanOptions->Version;
		instance->MountPoint[1] = L':';
		instance->MountPoint[2] = L'\\';
	}

	if (!DokanStart(instance)) {
		CloseHandle(device);
		return DOKAN_START_ERROR;
	}

	if (!DokanMount(instance->MountPoint, instance->DeviceName)) {
		SendReleaseIRP(instance->DeviceName);
		DokanDbgPrint("Dokan Error: DefineDosDevice Failed\n");
		CloseHandle(device);
		return DOKAN_MOUNT_ERROR;
	}

	DbgPrintW(L"mounted: %s -> %s\n", instance->MountPoint, instance->DeviceName);

	//Start Keep Alive thread
	threadIds[threadNum++] = (HANDLE)_beginthreadex(
		NULL, // Security Attributes
		0, //stack size
		DokanKeepAlive,
		(PVOID)instance, // param
		0, // create flag
		NULL);

	for (i = 0; i < DokanOptions->ThreadCount; ++i) {
		threadIds[threadNum++] = (HANDLE)_beginthreadex(
			NULL, // Security Attributes
			0, //stack size
			DokanLoop,
			(PVOID)instance, // param
			0, // create flag
			NULL);
	}


	// wait for thread terminations
	WaitForMultipleObjects(threadNum, threadIds, TRUE, INFINITE);

	for (i = 0; i < threadNum; ++i) {
		CloseHandle(threadIds[i]);
	}

    CloseHandle(device);

	Sleep(1000);
	
	DbgPrint("\nunload\n");

	DeleteDokanInstance(instance);

    return DOKAN_SUCCESS;
}

LPWSTR
GetRawDeviceName(LPCWSTR DeviceName, LPWSTR DestinationBuffer, rsize_t DestinationBufferSizeInElements)
{
	if(DeviceName && DestinationBuffer && DestinationBufferSizeInElements > 0) {
		wcscpy_s(DestinationBuffer, DestinationBufferSizeInElements, L"\\\\.");
		wcscat_s(DestinationBuffer, DestinationBufferSizeInElements, DeviceName);
	}

	return DestinationBuffer;
}

void
ALIGN_ALLOCATION_SIZE(PLARGE_INTEGER size)
{
	long long r = size->QuadPart % DOKAN_ALLOCATION_UNIT_SIZE;
	size->QuadPart = (size->QuadPart + (r > 0 ? DOKAN_ALLOCATION_UNIT_SIZE - r : 0));
}

UINT WINAPI
DokanLoop(
   PDOKAN_INSTANCE DokanInstance
	)
{
	HANDLE	device;
	char	buffer[EVENT_CONTEXT_MAX_SIZE];
	BOOL	status;
	ULONG	returnedLength;
	DWORD	result = 0;
    DWORD   lastError = 0;
	WCHAR	rawDeviceName[MAX_PATH];

	RtlZeroMemory(buffer, sizeof(buffer));

	device = CreateFile(
				GetRawDeviceName(DokanInstance->DeviceName, rawDeviceName, MAX_PATH), // lpFileName
				GENERIC_READ | GENERIC_WRITE,       // dwDesiredAccess
				FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
				NULL,                               // lpSecurityAttributes
				OPEN_EXISTING,                      // dwCreationDistribution
				0,                                  // dwFlagsAndAttributes
				NULL                                // hTemplateFile
			);

	if (device == INVALID_HANDLE_VALUE) {
		DbgPrint("Dokan Error: CreateFile failed %ws: %d\n",
			GetRawDeviceName(DokanInstance->DeviceName, rawDeviceName, MAX_PATH), GetLastError());
		result = (DWORD)-1;
		_endthreadex(result);
		return result;
	}

	status = TRUE;
	while (status) {

		status = DeviceIoControl(
					device,				// Handle to device
					IOCTL_EVENT_WAIT,	// IO Control code
					NULL,				// Input Buffer to driver.
					0,					// Length of input buffer in bytes.
					buffer,             // Output Buffer from driver.
					sizeof(buffer),		// Length of output buffer in bytes.
					&returnedLength,	// Bytes placed in buffer.
					NULL                // synchronous call
					);

		if (!status) {
            lastError = GetLastError();
			DbgPrint("Ioctl failed for wait with code %d.\n", lastError);
            if (lastError == ERROR_NO_SYSTEM_RESOURCES) {
                DbgPrint("Processing will continue\n");
                status = TRUE;
                Sleep(200);
                continue;
            }
            DbgPrint("Thread will be terminated\n");
			break;
		}

		//printf("#%d got notification %d\n", (ULONG)Param, count++);

		if(returnedLength > 0) {
			PEVENT_CONTEXT context = (PEVENT_CONTEXT)buffer;
			if (context->MountId != DokanInstance->MountId) {
				DbgPrint("Dokan Error: Invalid MountId (expected:%d, acctual:%d)\n",
						DokanInstance->MountId, context->MountId);
				continue;
			}

			switch (context->MajorFunction) {
			case IRP_MJ_CREATE:
				DispatchCreate(device, context, DokanInstance);
				break;
			case IRP_MJ_CLEANUP:
				DispatchCleanup(device, context, DokanInstance);
				break;
			case IRP_MJ_CLOSE:
				DispatchClose(device, context, DokanInstance);
				break;
			case IRP_MJ_DIRECTORY_CONTROL:
				DispatchDirectoryInformation(device, context, DokanInstance);
				break;
			case IRP_MJ_READ:
				DispatchRead(device, context, DokanInstance);
				break;
			case IRP_MJ_WRITE:
				DispatchWrite(device, context, DokanInstance);
				break;
			case IRP_MJ_QUERY_INFORMATION:
				DispatchQueryInformation(device, context, DokanInstance);
				break;
			case IRP_MJ_QUERY_VOLUME_INFORMATION:
				DispatchQueryVolumeInformation(device ,context, DokanInstance);
				break;
			case IRP_MJ_LOCK_CONTROL:
				DispatchLock(device, context, DokanInstance);
				break;
			case IRP_MJ_SET_INFORMATION:
				DispatchSetInformation(device, context, DokanInstance);
				break;
			case IRP_MJ_FLUSH_BUFFERS:
				DispatchFlush(device, context, DokanInstance);
				break;
			case IRP_MJ_QUERY_SECURITY:
				DispatchQuerySecurity(device, context, DokanInstance);
				break;
			case IRP_MJ_SET_SECURITY:
				DispatchSetSecurity(device, context, DokanInstance);
				break;
			case IRP_MJ_SHUTDOWN:
				// this case is used before unmount not shutdown
				DispatchUnmount(device, context, DokanInstance);
				break;
			default:
				break;
			}

		} else {
			DbgPrint("ReturnedLength %d\n", returnedLength);
		}
	}

	CloseHandle(device);
    _endthreadex(result);

	return result;
}



VOID
SendEventInformation(
	HANDLE				Handle,
	PEVENT_INFORMATION	EventInfo,
	ULONG				EventLength,
	PDOKAN_INSTANCE		DokanInstance)
{
	BOOL	status;
	ULONG	returnedLength;

	//DbgPrint("###EventInfo->Context %X\n", EventInfo->Context);
	if (DokanInstance != NULL) {
		ReleaseDokanOpenInfo(EventInfo, DokanInstance);
	}

	// send event info to driver
	status = DeviceIoControl(
					Handle,				// Handle to device
					IOCTL_EVENT_INFO,	// IO Control code
					EventInfo,			// Input Buffer to driver.
					EventLength,		// Length of input buffer in bytes.
					NULL,				// Output Buffer from driver.
					0,					// Length of output buffer in bytes.
					&returnedLength,	// Bytes placed in buffer.
					NULL				// synchronous call
					);

	if (!status) {
		DWORD errorCode = GetLastError();
		DbgPrint("Dokan Error: Ioctl failed with code %d\n", errorCode );
	}
}


VOID
CheckFileName(
	LPWSTR	FileName)
{
	size_t len = wcslen(FileName);
	// if the beginning of file name is "\\",
	// replace it with "\"
	if (len >= 2 && FileName[0] == L'\\' && FileName[1] == L'\\') {
		int i;
		for (i = 0; FileName[i+1] != L'\0'; ++i) {
			FileName[i] = FileName[i+1];
		}
		FileName[i] = L'\0';
	}
	
	// Remove "\" in front of Directory
	len = wcslen(FileName);
	if (len > 2 && FileName[len - 1] == L'\\')
		FileName[len - 1] = '\0';
}



PEVENT_INFORMATION
DispatchCommon(
	PEVENT_CONTEXT		EventContext,
	ULONG				SizeOfEventInfo,
	PDOKAN_INSTANCE		DokanInstance,
	PDOKAN_FILE_INFO	DokanFileInfo,
	PDOKAN_OPEN_INFO*	DokanOpenInfo)
{
	PEVENT_INFORMATION	eventInfo = (PEVENT_INFORMATION)malloc(SizeOfEventInfo);

	if (eventInfo == NULL) {
		return NULL;
	}
	RtlZeroMemory(eventInfo, SizeOfEventInfo);
	RtlZeroMemory(DokanFileInfo, sizeof(DOKAN_FILE_INFO));

	eventInfo->BufferLength = 0;
	eventInfo->SerialNumber = EventContext->SerialNumber;

	DokanFileInfo->ProcessId	= EventContext->ProcessId;
	DokanFileInfo->DokanOptions = DokanInstance->DokanOptions;
	if (EventContext->FileFlags & DOKAN_DELETE_ON_CLOSE) {
		DokanFileInfo->DeleteOnClose = 1;
	}
	if (EventContext->FileFlags & DOKAN_PAGING_IO) {
		DokanFileInfo->PagingIo = 1;
	}
	if (EventContext->FileFlags & DOKAN_WRITE_TO_END_OF_FILE) {
		DokanFileInfo->WriteToEndOfFile = 1;
	}
	if (EventContext->FileFlags & DOKAN_SYNCHRONOUS_IO) {
		DokanFileInfo->SynchronousIo = 1;
	}
	if (EventContext->FileFlags & DOKAN_NOCACHE) {
		DokanFileInfo->Nocache = 1;
	}

	*DokanOpenInfo = GetDokanOpenInfo(EventContext, DokanInstance);
	if (*DokanOpenInfo == NULL) {
		DbgPrint("error openInfo is NULL\n");
		return eventInfo;
	}

	DokanFileInfo->Context		= (ULONG64)(*DokanOpenInfo)->UserContext;
	DokanFileInfo->IsDirectory	= (UCHAR)(*DokanOpenInfo)->IsDirectory;
	DokanFileInfo->DokanContext = (ULONG64)(*DokanOpenInfo);

	eventInfo->Context = (ULONG64)(*DokanOpenInfo);

	return eventInfo;
}


PDOKAN_OPEN_INFO
GetDokanOpenInfo(
	PEVENT_CONTEXT		EventContext,
	PDOKAN_INSTANCE		DokanInstance)
{
	PDOKAN_OPEN_INFO openInfo;
	EnterCriticalSection(&DokanInstance->CriticalSection);

    openInfo = (PDOKAN_OPEN_INFO)(UINT_PTR)EventContext->Context;
	if (openInfo != NULL) {
		openInfo->OpenCount++;
		openInfo->EventContext = EventContext;
		openInfo->DokanInstance = DokanInstance;
	}
	LeaveCriticalSection(&DokanInstance->CriticalSection);
	return openInfo;
}


VOID
ReleaseDokanOpenInfo(
	PEVENT_INFORMATION	EventInformation,
	PDOKAN_INSTANCE		DokanInstance)
{
	PDOKAN_OPEN_INFO openInfo;
	EnterCriticalSection(&DokanInstance->CriticalSection);

    openInfo = (PDOKAN_OPEN_INFO)(UINT_PTR)EventInformation->Context;
	if (openInfo != NULL) {
		openInfo->OpenCount--;
		if (openInfo->OpenCount < 1) {
			if (openInfo->DirListHead != NULL) {
				ClearFindData(openInfo->DirListHead);
				free(openInfo->DirListHead);
				openInfo->DirListHead = NULL;
			}
			if (openInfo->StreamListHead != NULL) {
				ClearFindStreamData(openInfo->StreamListHead);
				free(openInfo->StreamListHead);
				openInfo->StreamListHead = NULL;
			}
			free(openInfo);
			EventInformation->Context = 0;
		}
	}
	LeaveCriticalSection(&DokanInstance->CriticalSection);
}


VOID
DispatchUnmount(
	HANDLE				Handle,
	PEVENT_CONTEXT		EventContext,
	PDOKAN_INSTANCE		DokanInstance)
{
    UNREFERENCED_PARAMETER(Handle);

	DOKAN_FILE_INFO			fileInfo;
	static int count = 0;

	// Unmount is called only once
	EnterCriticalSection(&DokanInstance->CriticalSection); 
	
	if (count > 0) {
		LeaveCriticalSection(&DokanInstance->CriticalSection);
		return;
	}
	count++;

	RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));

	fileInfo.ProcessId = EventContext->ProcessId;

	if (DokanInstance->DokanOperations->Unmount) {
		// ignore return value
		DokanInstance->DokanOperations->Unmount(&fileInfo);
	}

	LeaveCriticalSection(&DokanInstance->CriticalSection);

	// do not notice anything to the driver
	return;
}


// ask driver to release all pending IRP to prepare for Unmount.
BOOL
SendReleaseIRP(
	LPCWSTR	DeviceName)
{
	ULONG	returnedLength;
	WCHAR	rawDeviceName[MAX_PATH];

	DbgPrint("send release\n");

	if (!SendToDevice(
				GetRawDeviceName(DeviceName, rawDeviceName, MAX_PATH),
				IOCTL_EVENT_RELEASE,
				NULL,
				0,
				NULL,
				0,
				&returnedLength) ) {
		
		DbgPrint("Failed to unmount device:%ws\n", DeviceName);
		return FALSE;
	}

	return TRUE;
}


BOOL
DokanStart(PDOKAN_INSTANCE Instance)
{
	EVENT_START			eventStart;
	EVENT_DRIVER_INFO	driverInfo;
	ULONG		returnedLength = 0;

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
	if (Instance->DokanOptions->Options & DOKAN_OPTION_READONLY) {
		eventStart.Flags |= DOKAN_EVENT_READONLY;
	}
    
    eventStart.IrpTimeout = Instance->DokanOptions->Timeout;
    

	SendToDevice(
		DOKAN_GLOBAL_DEVICE_NAME,
		IOCTL_EVENT_START,
		&eventStart,
		sizeof(EVENT_START),
		&driverInfo,
		sizeof(EVENT_DRIVER_INFO),
		&returnedLength);

	if (driverInfo.Status == DOKAN_START_FAILED) {
		if (driverInfo.DriverVersion != eventStart.UserVersion) {
			DokanDbgPrint(
				"Dokan Error: driver version mismatch, driver %X, dll %X\n",
				driverInfo.DriverVersion, eventStart.UserVersion);
		} else {
			DokanDbgPrint("Dokan Error: driver start error\n");
		}
		return FALSE;
	} else if (driverInfo.Status == DOKAN_MOUNTED) {
		Instance->MountId = driverInfo.MountId;
		Instance->DeviceNumber = driverInfo.DeviceNumber;
		wcscpy_s(Instance->DeviceName,
				sizeof(Instance->DeviceName) / sizeof(WCHAR),
				driverInfo.DeviceName);
		return TRUE;
	}
	return FALSE;
}


BOOL DOKANAPI
DokanSetDebugMode(
	ULONG	Mode)
{
	ULONG returnedLength;
	return SendToDevice(
		DOKAN_GLOBAL_DEVICE_NAME,
		IOCTL_SET_DEBUG_MODE,
		&Mode,
		sizeof(ULONG),
		NULL,
		0,
		&returnedLength);
}


BOOL
SendToDevice(
	LPCWSTR	DeviceName,
	DWORD	IoControlCode,
	PVOID	InputBuffer,
	ULONG	InputLength,
	PVOID	OutputBuffer,
	ULONG	OutputLength,
	PULONG	ReturnedLength)
{
	HANDLE	device;
	BOOL	status;

	device = CreateFile(
				DeviceName,							// lpFileName
				GENERIC_READ | GENERIC_WRITE,       // dwDesiredAccess
                FILE_SHARE_READ | FILE_SHARE_WRITE, // dwShareMode
                NULL,                               // lpSecurityAttributes
                OPEN_EXISTING,                      // dwCreationDistribution
                0,                                  // dwFlagsAndAttributes
                NULL                                // hTemplateFile
			);

    if (device == INVALID_HANDLE_VALUE) {
		DWORD dwErrorCode = GetLastError();
		DbgPrint("Dokan Error: Failed to open %ws with code %d\n",
			DeviceName, dwErrorCode);
        return FALSE;
    }

	status = DeviceIoControl(
				device,                 // Handle to device
				IoControlCode,			// IO Control code
				InputBuffer,		    // Input Buffer to driver.
				InputLength,			// Length of input buffer in bytes.
				OutputBuffer,           // Output Buffer from driver.
				OutputLength,			// Length of output buffer in bytes.
				ReturnedLength,		    // Bytes placed in buffer.
				NULL                    // synchronous call
			);

	CloseHandle(device);

	if (!status) {
		DbgPrint("DokanError: Ioctl failed with code %d\n", GetLastError());
		return FALSE;
	}

	return TRUE;
}


BOOL WINAPI DllMain(
	HINSTANCE	Instance,
	DWORD		Reason,
	LPVOID		Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);
    UNREFERENCED_PARAMETER(Instance);

	switch(Reason) {
		case DLL_PROCESS_ATTACH:
			{
#if _MSC_VER < 1300
				InitializeCriticalSection(&g_InstanceCriticalSection);
#else
				InitializeCriticalSectionAndSpinCount(
					&g_InstanceCriticalSection, 0x80000400);
#endif
				
				InitializeListHead(&g_InstanceList);
			}
			break;			
		case DLL_PROCESS_DETACH:
			{
				EnterCriticalSection(&g_InstanceCriticalSection);

				while(!IsListEmpty(&g_InstanceList)) {
					PLIST_ENTRY entry = RemoveHeadList(&g_InstanceList);
					PDOKAN_INSTANCE instance =
						CONTAINING_RECORD(entry, DOKAN_INSTANCE, ListEntry);
					
					DokanRemoveMountPoint(instance->MountPoint);
					free(instance);
				}

				LeaveCriticalSection(&g_InstanceCriticalSection);
				DeleteCriticalSection(&g_InstanceCriticalSection);
			}
			break;
	}
	return TRUE;
}

void DOKANAPI
DokanMapKernelToUserCreateFileFlags(
	ULONG FileAttributes,
	ULONG CreateOptions,
	ULONG CreateDisposition,
	DWORD *outFileAttributesAndFlags,
	DWORD *outCreationDisposition)
{
	if(outFileAttributesAndFlags) {

		*outFileAttributesAndFlags = FileAttributes;

		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_WRITE_THROUGH, FILE_WRITE_THROUGH);
		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_SEQUENTIAL_SCAN, FILE_SEQUENTIAL_ONLY);
		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_RANDOM_ACCESS, FILE_RANDOM_ACCESS);
		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_NO_BUFFERING, FILE_NO_INTERMEDIATE_BUFFERING);
		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_OPEN_REPARSE_POINT, FILE_OPEN_REPARSE_POINT);
		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_DELETE_ON_CLOSE, FILE_DELETE_ON_CLOSE);
		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_BACKUP_SEMANTICS, FILE_OPEN_FOR_BACKUP_INTENT);

#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
		DokanMapKernelBit(*outFileAttributesAndFlags, CreateOptions, FILE_FLAG_SESSION_AWARE, FILE_SESSION_AWARE);
#endif
	}

	if(outCreationDisposition) {

		switch(CreateDisposition) {
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
			// The documentation isn't clear on the difference between replacing a file and truncating it.
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
