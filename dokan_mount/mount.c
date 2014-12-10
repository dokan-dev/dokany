/*

Copyright (c) 2007, 2008 Hiroki Asakawa asakaw@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include "mount.h"

typedef struct _REPARSE_DATA_BUFFER {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR  DataBuffer[1];
        } GenericReparseBuffer;
    } DUMMYUNIONNAME;
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

#define REPARSE_DATA_BUFFER_HEADER_SIZE   FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)

BOOL
CreateMountPoint(
	LPCWSTR	MountPoint,
	LPCWSTR	DeviceName)
{
	HANDLE handle;
	PREPARSE_DATA_BUFFER reparseData;
	USHORT	bufferLength;
	USHORT	targetLength;
	BOOL	result;
	ULONG	resultLength;
	WCHAR	targetDeviceName[MAX_PATH] =  L"\\??";

	wcscat_s(targetDeviceName, MAX_PATH, DeviceName);
	wcscat_s(targetDeviceName, MAX_PATH, L"\\");

	handle = CreateFile(
		MountPoint, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		DbgPrintW(L"CreateFile failed: %s (%d)\n", MountPoint, GetLastError());
		return FALSE;
	}

	targetLength = wcslen(targetDeviceName) * sizeof(WCHAR);
	bufferLength = FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) +
		targetLength + sizeof(WCHAR) + sizeof(WCHAR);

	reparseData = malloc(bufferLength);

	ZeroMemory(reparseData, bufferLength);

	reparseData->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	reparseData->ReparseDataLength = bufferLength - REPARSE_DATA_BUFFER_HEADER_SIZE;

	reparseData->MountPointReparseBuffer.SubstituteNameOffset = 0;
	reparseData->MountPointReparseBuffer.SubstituteNameLength = targetLength;
	reparseData->MountPointReparseBuffer.PrintNameOffset = targetLength + sizeof(WCHAR);
	reparseData->MountPointReparseBuffer.PrintNameLength = 0;

	RtlCopyMemory(reparseData->MountPointReparseBuffer.PathBuffer, targetDeviceName, targetLength);

	result = DeviceIoControl(
				handle,
				FSCTL_SET_REPARSE_POINT,
				reparseData,
				bufferLength,
				NULL,
				0,
				&resultLength,
				NULL);
	
	CloseHandle(handle);
	free(reparseData);

	if (result) {
		DbgPrintW(L"CreateMountPoint %s -> %s success\n",
			MountPoint, targetDeviceName);
	} else {
		DbgPrintW(L"CreateMountPoint %s -> %s failed: %d\n",
			MountPoint, targetDeviceName, GetLastError());
	}
	return result;
}

BOOL
DeleteMountPoint(
	LPCWSTR	MountPoint)
{
	HANDLE	handle;
	BOOL	result;
	ULONG	resultLength;
	REPARSE_GUID_DATA_BUFFER	reparseData = { 0 };

	handle = CreateFile(
		MountPoint, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		DbgPrintW(L"CreateFile failed: %s (%d)\n", MountPoint, GetLastError());
		return FALSE;
	}

	reparseData.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;

	result = DeviceIoControl(
				handle,
				FSCTL_DELETE_REPARSE_POINT,
				&reparseData,
				REPARSE_GUID_DATA_BUFFER_HEADER_SIZE,
				NULL,
				0,
				&resultLength,
				NULL);
	
	CloseHandle(handle);

	if (result) {
		DbgPrintW(L"DeleteMountPoint %s success\n", MountPoint);
	} else {
		DbgPrintW(L"DeleteMountPoint %s failed: %d\n", MountPoint, GetLastError());
	}
	return result;
}

BOOL
CreateDriveLetter(
	WCHAR		DriveLetter,
	LPCWSTR	DeviceName)
{
	WCHAR   dosDevice[] = L"\\\\.\\C:";
	WCHAR   driveName[] = L"C:";
	WCHAR	rawDeviceName[MAX_PATH] = L"\\Device";
	HANDLE  device;

	dosDevice[4] = DriveLetter;
	driveName[0] = DriveLetter;
	wcscat_s(rawDeviceName, MAX_PATH, DeviceName);

	DbgPrintW(L"DriveLetter: %c, DeviceName %s\n", DriveLetter, rawDeviceName);

	device = CreateFile(
		dosDevice,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING,
		NULL
		);

    if (device != INVALID_HANDLE_VALUE) {
		DbgPrintW(L"DokanControl Mount failed: %c: is alredy used\n", DriveLetter);
		CloseHandle(device);
        return FALSE;
    }

    if (!DefineDosDevice(DDD_RAW_TARGET_PATH, driveName, rawDeviceName)) {
		DbgPrintW(L"DokanControl DefineDosDevice failed: %d\n", GetLastError());
        return FALSE;
    }

	device = CreateFile(
        dosDevice,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING,
        NULL
        );

    if (device == INVALID_HANDLE_VALUE) {
		DbgPrintW(L"DokanControl Mount %c failed:%d\n", DriveLetter, GetLastError());
        DefineDosDevice(DDD_REMOVE_DEFINITION, dosDevice, NULL);
        return FALSE;
    }

	CloseHandle(device);
	return TRUE;
}

BOOL
DokanControlMount(
	LPCWSTR	MountPoint,
	LPCWSTR	DeviceName)
{
	ULONG length = wcslen(MountPoint);

	if (length == 1 ||
		(length == 2 && MountPoint[1] == L':') ||
		(length == 3 && MountPoint[1] == L':' && MountPoint[2] == L'\\')) {
		return CreateDriveLetter(MountPoint[0], DeviceName);
	} else if (length > 3) {
		return CreateMountPoint(MountPoint, DeviceName);
	}
	return FALSE; 
}

BOOL
DokanControlUnmount(
	LPCWSTR	MountPoint)
{
    
	ULONG	length = wcslen(MountPoint);

	if (length == 1 ||
		(length == 2 && MountPoint[1] == L':') ||
		(length == 3 && MountPoint[1] == L':' && MountPoint[2] == L'\\')) {

		WCHAR   drive[] = L"C:";	
	    drive[0] = MountPoint[0];

		if (!DefineDosDevice(DDD_REMOVE_DEFINITION, drive, NULL)) {
			DbgPrintW(L"DriveLetter %c\n", MountPoint[0]);
			DbgPrintW(L"DokanControl DefineDosDevice failed\n");
			return FALSE;
		} else {
			DbgPrintW(L"DokanControl DD_REMOVE_DEFINITION success\n");
			return TRUE;
		}

	} else if (length > 3 ) {
		return DeleteMountPoint(MountPoint);
	}

	return FALSE;
}
