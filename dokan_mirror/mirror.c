/*

Copyright (c) 2007, 2008 Hiroki Asakawa info@dokan-dev.net

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

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winbase.h>
#include <ntstatus.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include "../dokan/dokan.h"
#include "../dokan/fileinfo.h"

BOOL g_UseStdErr;
BOOL g_DebugMode;

static void DbgPrint(LPCWSTR format, ...)
{
	if (g_DebugMode) {
		const WCHAR *outputString;
		WCHAR *buffer;
		size_t length;
		va_list argp;

		va_start(argp, format);
		length = _vscwprintf(format, argp) + 1;
		buffer = _malloca(length*sizeof(WCHAR));
		if (buffer) {
			vswprintf_s(buffer, length, format, argp);
			outputString = buffer;
		} else {
			outputString = format;
		}
		if (g_UseStdErr)
			fputws(outputString, stderr);
		else
			OutputDebugStringW(outputString);
		_freea(buffer);
		va_end(argp);
	}
}

static WCHAR RootDirectory[MAX_PATH] = L"C:";
static WCHAR MountPoint[MAX_PATH] = L"M:\\";

static void
GetFilePath(
	PWCHAR	filePath,
	ULONG	numberOfElements,
	LPCWSTR FileName)
{
	wcsncpy_s(filePath, numberOfElements, RootDirectory, wcslen(RootDirectory));
	wcsncat_s(filePath, numberOfElements, FileName, wcslen(FileName));
}


static void
PrintUserName(PDOKAN_FILE_INFO	DokanFileInfo)
{
	HANDLE	handle;
	UCHAR buffer[1024];
	DWORD returnLength;
	WCHAR accountName[256];
	WCHAR domainName[256];
	DWORD accountLength = sizeof(accountName) / sizeof(WCHAR);
	DWORD domainLength = sizeof(domainName) / sizeof(WCHAR);
	PTOKEN_USER tokenUser;
	SID_NAME_USE snu;

	handle = DokanOpenRequestorToken(DokanFileInfo);
	if (handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"  DokanOpenRequestorToken failed\n");
		return;
	}

	if (!GetTokenInformation(handle, TokenUser, buffer, sizeof(buffer), &returnLength)) {
		DbgPrint(L"  GetTokenInformaiton failed: %d\n", GetLastError());
		CloseHandle(handle);
		return;
	}

	CloseHandle(handle);

	tokenUser = (PTOKEN_USER)buffer;
	if (!LookupAccountSid(NULL, tokenUser->User.Sid, accountName,
			&accountLength, domainName, &domainLength, &snu)) {
		DbgPrint(L"  LookupAccountSid failed: %d\n", GetLastError());
		return;
	}

	DbgPrint(L"  AccountName: %s, DomainName: %s\n", accountName, domainName);
}

NTSTATUS ToNtStatus(DWORD dwError)
{
	switch (dwError)
	{
	case ERROR_FILE_NOT_FOUND:
		return STATUS_OBJECT_NAME_NOT_FOUND;
	case ERROR_PATH_NOT_FOUND:
		return STATUS_OBJECT_PATH_NOT_FOUND;
	case ERROR_INVALID_PARAMETER:
		return STATUS_INVALID_PARAMETER;
	default:
		return STATUS_ACCESS_DENIED;
	}
}

#define MirrorCheckFlag(val, flag) if (val&flag) { DbgPrint(L"\t" L#flag L"\n"); }

static NTSTATUS DOKAN_CALLBACK
MirrorCreateFile(
	LPCWSTR					FileName,
	DWORD					AccessMode,
	DWORD					ShareMode,
	DWORD					CreationDisposition,
	DWORD					FlagsAndAttributes,
	PDOKAN_FILE_INFO		DokanFileInfo)
{
	WCHAR filePath[MAX_PATH];
	HANDLE handle;
	DWORD fileAttr;
	NTSTATUS status = STATUS_SUCCESS;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"CreateFile : %s\n", filePath);

	PrintUserName(DokanFileInfo);

	if (CreationDisposition == CREATE_NEW)
		DbgPrint(L"\tCREATE_NEW\n");
	if (CreationDisposition == OPEN_ALWAYS)
		DbgPrint(L"\tOPEN_ALWAYS\n");
	if (CreationDisposition == CREATE_ALWAYS)
		DbgPrint(L"\tCREATE_ALWAYS\n");
	if (CreationDisposition == OPEN_EXISTING)
		DbgPrint(L"\tOPEN_EXISTING\n");
	if (CreationDisposition == TRUNCATE_EXISTING)
		DbgPrint(L"\tTRUNCATE_EXISTING\n");

	/*
	if (ShareMode == 0 && AccessMode & FILE_WRITE_DATA)
		ShareMode = FILE_SHARE_WRITE;
	else if (ShareMode == 0)
		ShareMode = FILE_SHARE_READ;
	*/

	DbgPrint(L"\tShareMode = 0x%x\n", ShareMode);

	MirrorCheckFlag(ShareMode, FILE_SHARE_READ);
	MirrorCheckFlag(ShareMode, FILE_SHARE_WRITE);
	MirrorCheckFlag(ShareMode, FILE_SHARE_DELETE);

	DbgPrint(L"\tAccessMode = 0x%x\n", AccessMode);

	MirrorCheckFlag(AccessMode, GENERIC_READ);
	MirrorCheckFlag(AccessMode, GENERIC_WRITE);
	MirrorCheckFlag(AccessMode, GENERIC_EXECUTE);
	
	MirrorCheckFlag(AccessMode, DELETE);
	MirrorCheckFlag(AccessMode, FILE_READ_DATA);
	MirrorCheckFlag(AccessMode, FILE_READ_ATTRIBUTES);
	MirrorCheckFlag(AccessMode, FILE_READ_EA);
	MirrorCheckFlag(AccessMode, READ_CONTROL);
	MirrorCheckFlag(AccessMode, FILE_WRITE_DATA);
	MirrorCheckFlag(AccessMode, FILE_WRITE_ATTRIBUTES);
	MirrorCheckFlag(AccessMode, FILE_WRITE_EA);
	MirrorCheckFlag(AccessMode, FILE_APPEND_DATA);
	MirrorCheckFlag(AccessMode, WRITE_DAC);
	MirrorCheckFlag(AccessMode, WRITE_OWNER);
	MirrorCheckFlag(AccessMode, SYNCHRONIZE);
	MirrorCheckFlag(AccessMode, FILE_EXECUTE);
	MirrorCheckFlag(AccessMode, STANDARD_RIGHTS_READ);
	MirrorCheckFlag(AccessMode, STANDARD_RIGHTS_WRITE);
	MirrorCheckFlag(AccessMode, STANDARD_RIGHTS_EXECUTE);
	
	// When filePath is a directory, needs to change the flag so that the file can be opened.
	fileAttr = GetFileAttributes(filePath);
	if (fileAttr != INVALID_FILE_ATTRIBUTES && fileAttr & FILE_ATTRIBUTE_DIRECTORY) {
		FlagsAndAttributes |= FILE_FLAG_BACKUP_SEMANTICS;
		//AccessMode = 0;
	}
	DbgPrint(L"\tFlagsAndAttributes = 0x%x\n", FlagsAndAttributes);

	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_ARCHIVE);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_ENCRYPTED);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_HIDDEN);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_NORMAL);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_OFFLINE);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_READONLY);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_SYSTEM);
	MirrorCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_TEMPORARY);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_WRITE_THROUGH);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_OVERLAPPED);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_NO_BUFFERING);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_RANDOM_ACCESS);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_SEQUENTIAL_SCAN);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_DELETE_ON_CLOSE);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_BACKUP_SEMANTICS);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_POSIX_SEMANTICS);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_OPEN_REPARSE_POINT);
	MirrorCheckFlag(FlagsAndAttributes, FILE_FLAG_OPEN_NO_RECALL);
	MirrorCheckFlag(FlagsAndAttributes, SECURITY_ANONYMOUS);
	MirrorCheckFlag(FlagsAndAttributes, SECURITY_IDENTIFICATION);
	MirrorCheckFlag(FlagsAndAttributes, SECURITY_IMPERSONATION);
	MirrorCheckFlag(FlagsAndAttributes, SECURITY_DELEGATION);
	MirrorCheckFlag(FlagsAndAttributes, SECURITY_CONTEXT_TRACKING);
	MirrorCheckFlag(FlagsAndAttributes, SECURITY_EFFECTIVE_ONLY);
	MirrorCheckFlag(FlagsAndAttributes, SECURITY_SQOS_PRESENT);

	handle = CreateFile(
		filePath,
		AccessMode,//GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
		ShareMode,
		NULL, // security attribute
		CreationDisposition,
		FlagsAndAttributes,// |FILE_FLAG_NO_BUFFERING,
		NULL); // template file handle

	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);

		switch (error) {
		case ERROR_FILE_NOT_FOUND:
			status = STATUS_OBJECT_NAME_NOT_FOUND;
			break;
		case ERROR_PATH_NOT_FOUND:
			status = STATUS_OBJECT_PATH_NOT_FOUND;
			break;
		case ERROR_ACCESS_DENIED:
			status = STATUS_ACCESS_DENIED;
			break;
		case ERROR_SHARING_VIOLATION:
			status = STATUS_SHARING_VIOLATION;
			break;
		case ERROR_INVALID_NAME:
			status = STATUS_OBJECT_NAME_NOT_FOUND;
			break;
		case ERROR_FILE_EXISTS:
		case ERROR_ALREADY_EXISTS:
			status = STATUS_OBJECT_NAME_COLLISION;
			break;
		case ERROR_PRIVILEGE_NOT_HELD:
			status = STATUS_PRIVILEGE_NOT_HELD;
			break;
		case ERROR_NOT_READY:
			status = STATUS_DEVICE_NOT_READY;
			break;
		default:
			status = STATUS_INVALID_PARAMETER;
			DbgPrint(L"Create got unknown error code %d\n", error);
		}
	} else
		DokanFileInfo->Context = (ULONG64)handle; // save the file handle in Context

	DbgPrint(L"\n");
	return status;
}


static NTSTATUS DOKAN_CALLBACK
MirrorCreateDirectory(
	LPCWSTR					FileName,
	PDOKAN_FILE_INFO		DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

	WCHAR filePath[MAX_PATH];
	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"CreateDirectory : %s\n", filePath);
	if (!CreateDirectory(filePath, NULL)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return ToNtStatus(error);
	}
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorOpenDirectory(
	LPCWSTR					FileName,
	PDOKAN_FILE_INFO		DokanFileInfo)
{
	WCHAR filePath[MAX_PATH];
	HANDLE handle;
	DWORD attr;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"OpenDirectory : %s\n", filePath);

	attr = GetFileAttributes(filePath);
	if (attr == INVALID_FILE_ATTRIBUTES) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return ToNtStatus(error);
	}
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		return STATUS_NOT_IMPLEMENTED;
	}

	handle = CreateFile(
		filePath,
		0,
		FILE_SHARE_READ|FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return ToNtStatus(error);
	}

	DbgPrint(L"\n");

	DokanFileInfo->Context = (ULONG64)handle;

	return STATUS_SUCCESS;
}


static void DOKAN_CALLBACK
MirrorCloseFile(
	LPCWSTR					FileName,
	PDOKAN_FILE_INFO		DokanFileInfo)
{
	WCHAR filePath[MAX_PATH];
	GetFilePath(filePath, MAX_PATH, FileName);

	if (DokanFileInfo->Context) {
		DbgPrint(L"CloseFile: %s\n", filePath);
		DbgPrint(L"\terror : not cleanuped file\n\n");
		CloseHandle((HANDLE)DokanFileInfo->Context);
		DokanFileInfo->Context = 0;
	} else {
		DbgPrint(L"Close: %s\n\n", filePath);
	}
}


static void DOKAN_CALLBACK
MirrorCleanup(
	LPCWSTR					FileName,
	PDOKAN_FILE_INFO		DokanFileInfo)
{
	WCHAR filePath[MAX_PATH];
	GetFilePath(filePath, MAX_PATH, FileName);

	if (DokanFileInfo->Context) {
		DbgPrint(L"Cleanup: %s\n\n", filePath);
		CloseHandle((HANDLE)DokanFileInfo->Context);
		DokanFileInfo->Context = 0;

		if (DokanFileInfo->DeleteOnClose) {
			DbgPrint(L"\tDeleteOnClose\n");
			if (DokanFileInfo->IsDirectory) {
				DbgPrint(L"  DeleteDirectory ");
				if (!RemoveDirectory(filePath)) {
					DbgPrint(L"error code = %d\n\n", GetLastError());
				} else {
					DbgPrint(L"success\n\n");
				}
			} else {
				DbgPrint(L"  DeleteFile ");
				if (DeleteFile(filePath) == 0) {
					DbgPrint(L" error code = %d\n\n", GetLastError());
				} else {
					DbgPrint(L"success\n\n");
				}
			}
		}

	} else {
		DbgPrint(L"Cleanup: %s\n\tinvalid handle\n\n", filePath);
	}
}


static NTSTATUS DOKAN_CALLBACK
MirrorReadFile(
	LPCWSTR				FileName,
	LPVOID				Buffer,
	DWORD				BufferLength,
	LPDWORD				ReadLength,
	LONGLONG			Offset,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR	filePath[MAX_PATH];
	HANDLE	handle = (HANDLE)DokanFileInfo->Context;
	ULONG	offset = (ULONG)Offset;
	BOOL	opened = FALSE;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"ReadFile : %s\n", filePath);

	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle, cleanuped?\n");
		handle = CreateFile(
			filePath,
			GENERIC_READ,
			FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);
		if (handle == INVALID_HANDLE_VALUE) {
			DWORD error = GetLastError();
			DbgPrint(L"\tCreateFile error : %d\n\n", error);
			return ToNtStatus(error);
		}
		opened = TRUE;
	}
	
    LARGE_INTEGER distanceToMove;
    distanceToMove.QuadPart = Offset;
    if (!SetFilePointerEx(handle, distanceToMove, NULL, FILE_BEGIN)) {
		DWORD error = GetLastError();
		DbgPrint(L"\tseek error, offset = %d\n\n", offset);
		if (opened)
			CloseHandle(handle);
		return ToNtStatus(error);
	}

		
	if (!ReadFile(handle, Buffer, BufferLength, ReadLength,NULL)) {
		DWORD error = GetLastError();
		DbgPrint(L"\tread error = %u, buffer length = %d, read length = %d\n\n",
			error, BufferLength, *ReadLength);
		if (opened)
			CloseHandle(handle);
		return ToNtStatus(error);

	} else {
		DbgPrint(L"\tread %d, offset %d\n\n", *ReadLength, offset);
	}

	if (opened)
		CloseHandle(handle);

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorWriteFile(
	LPCWSTR		FileName,
	LPCVOID		Buffer,
	DWORD		NumberOfBytesToWrite,
	LPDWORD		NumberOfBytesWritten,
	LONGLONG			Offset,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR	filePath[MAX_PATH];
	HANDLE	handle = (HANDLE)DokanFileInfo->Context;
	ULONG	offset = (ULONG)Offset;
	BOOL	opened = FALSE;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"WriteFile : %s, offset %I64d, length %d\n", filePath, Offset, NumberOfBytesToWrite);

	// reopen the file
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle, cleanuped?\n");
		handle = CreateFile(
			filePath,
			GENERIC_WRITE,
			FILE_SHARE_WRITE,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);
		if (handle == INVALID_HANDLE_VALUE) {
			DWORD error = GetLastError();
			DbgPrint(L"\tCreateFile error : %d\n\n", error);
			return ToNtStatus(error);
		}
		opened = TRUE;
	}

    LARGE_INTEGER distanceToMove;
    distanceToMove.QuadPart = Offset;

	if (DokanFileInfo->WriteToEndOfFile) {
        LARGE_INTEGER z;
        z.QuadPart = 0;
		if (!SetFilePointerEx(handle, z, NULL, FILE_END)) {
			DWORD error = GetLastError();
			DbgPrint(L"\tseek error, offset = EOF, error = %d\n", error);
			return ToNtStatus(error);
		}
    }
    else if (!SetFilePointerEx(handle, distanceToMove, NULL, FILE_BEGIN)) {
		DWORD error = GetLastError();
		DbgPrint(L"\tseek error, offset = %d, error = %d\n", offset, error);
		return ToNtStatus(error);
	}

		
	if (!WriteFile(handle, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten, NULL)) {
		DWORD error = GetLastError();
		DbgPrint(L"\twrite error = %u, buffer length = %d, write length = %d\n",
			error, NumberOfBytesToWrite, *NumberOfBytesWritten);
		return ToNtStatus(error);

	} else {
		DbgPrint(L"\twrite %d, offset %d\n\n", *NumberOfBytesWritten, offset);
	}

	// close the file when it is reopened
	if (opened)
		CloseHandle(handle);

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorFlushFileBuffers(
	LPCWSTR		FileName,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR	filePath[MAX_PATH];
	HANDLE	handle = (HANDLE)DokanFileInfo->Context;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"FlushFileBuffers : %s\n", filePath);

	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_SUCCESS;
	}

	if (FlushFileBuffers(handle)) {
		return STATUS_SUCCESS;
	} else {
		DWORD error = GetLastError();
		DbgPrint(L"\tflush error code = %d\n", error);
		return ToNtStatus(error);
	}

}


static NTSTATUS DOKAN_CALLBACK
MirrorGetFileInformation(
	LPCWSTR							FileName,
	LPBY_HANDLE_FILE_INFORMATION	HandleFileInformation,
	PDOKAN_FILE_INFO				DokanFileInfo)
{
	WCHAR	filePath[MAX_PATH];
	HANDLE	handle = (HANDLE)DokanFileInfo->Context;
	BOOL	opened = FALSE;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"GetFileInfo : %s\n", filePath);

	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");

		// If CreateDirectory returned FILE_ALREADY_EXISTS and 
		// it is called with FILE_OPEN_IF, that handle must be opened.
		handle = CreateFile(filePath, 0, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS, NULL);
		if (handle == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			DbgPrint(L"GetFileInfo failed(%d)\n", error);
			return ToNtStatus(error);
		}
		opened = TRUE;
	}

	if (!GetFileInformationByHandle(handle,HandleFileInformation)) {
		DbgPrint(L"\terror code = %d\n", GetLastError());

		// FileName is a root directory
		// in this case, FindFirstFile can't get directory information
		if (wcslen(FileName) == 1) {
			DbgPrint(L"  root dir\n");
			HandleFileInformation->dwFileAttributes = GetFileAttributes(filePath);

		} else {
			WIN32_FIND_DATAW find;
			ZeroMemory(&find, sizeof(WIN32_FIND_DATAW));
			handle = FindFirstFile(filePath, &find);
			if (handle == INVALID_HANDLE_VALUE) {
				DWORD error = GetLastError();
				DbgPrint(L"\tFindFirstFile error code = %d\n\n", error);
				return ToNtStatus(error);
			}
			HandleFileInformation->dwFileAttributes = find.dwFileAttributes;
			HandleFileInformation->ftCreationTime = find.ftCreationTime;
			HandleFileInformation->ftLastAccessTime = find.ftLastAccessTime;
			HandleFileInformation->ftLastWriteTime = find.ftLastWriteTime;
			HandleFileInformation->nFileSizeHigh = find.nFileSizeHigh;
			HandleFileInformation->nFileSizeLow = find.nFileSizeLow;
			DbgPrint(L"\tFindFiles OK, file size = %d\n", find.nFileSizeLow);
			FindClose(handle);
		}
	} else {
		DbgPrint(L"\tGetFileInformationByHandle success, file size = %d\n",
			HandleFileInformation->nFileSizeLow);
	}

	DbgPrint(L"\n");

	if (opened) {
		CloseHandle(handle);
	}

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorFindFiles(
	LPCWSTR				FileName,
	PFillFindData		FillFindData, // function pointer
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR				filePath[MAX_PATH];
	size_t				fileLen;
	HANDLE				hFind;
	WIN32_FIND_DATAW	findData;
	DWORD				error;
	int					count = 0;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"FindFiles :%s\n", filePath);

	fileLen = wcslen(filePath);
	if (filePath[fileLen-1] != L'\\') {
		filePath[fileLen++] = L'\\';
	}
	filePath[fileLen] = L'*';
	filePath[fileLen+1] = L'\0';

	hFind = FindFirstFile(filePath, &findData);

	if (hFind == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		DbgPrint(L"\tinvalid file handle. Error is %u\n\n", error);
		return ToNtStatus(error);
	}

	FillFindData(&findData, DokanFileInfo);
	count++;

	while (FindNextFile(hFind, &findData) != 0) {
 		FillFindData(&findData, DokanFileInfo);
		count++;
	}
	
	error = GetLastError();
	FindClose(hFind);

	if (error != ERROR_NO_MORE_FILES) {
		DbgPrint(L"\tFindNextFile error. Error is %u\n\n", error);
		return ToNtStatus(error);
	}

	DbgPrint(L"\tFindFiles return %d entries in %s\n\n", count, filePath);

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorDeleteFile(
	LPCWSTR				FileName,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

	WCHAR	filePath[MAX_PATH];
	//HANDLE	handle = (HANDLE)DokanFileInfo->Context;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"DeleteFile %s\n", filePath);

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorDeleteDirectory(
	LPCWSTR				FileName,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

	WCHAR	filePath[MAX_PATH];
	//HANDLE	handle = (HANDLE)DokanFileInfo->Context;
	HANDLE	hFind;
	WIN32_FIND_DATAW	findData;
	size_t	fileLen;

	ZeroMemory(filePath, sizeof(filePath));
	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"DeleteDirectory %s\n", filePath);

	fileLen = wcslen(filePath);
	if (filePath[fileLen-1] != L'\\') {
		filePath[fileLen++] = L'\\';
	}
	filePath[fileLen] = L'*';
	filePath[fileLen+1] = L'\0';

	hFind = FindFirstFile(filePath, &findData);

	if (hFind == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);
		return ToNtStatus(error);
	}

	while (hFind != INVALID_HANDLE_VALUE) {
		if (wcscmp(findData.cFileName, L"..") != 0 &&
			wcscmp(findData.cFileName, L".") != 0) {
			FindClose(hFind);
			DbgPrint(L"\tDirectory is not empty: %s\n", findData.cFileName);
			return STATUS_DIRECTORY_NOT_EMPTY;
		}
		if (!FindNextFile(hFind, &findData)) {
			break;
		}
	}
	DWORD error = GetLastError();
	FindClose(hFind);

	if (error != ERROR_NO_MORE_FILES) {
		DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);
		return ToNtStatus(error);
	}

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorMoveFile(
	LPCWSTR				FileName, // existing file name
	LPCWSTR				NewFileName,
	BOOL				ReplaceIfExisting,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR			filePath[MAX_PATH];
	WCHAR			newFilePath[MAX_PATH];
	BOOL			status;

	GetFilePath(filePath, MAX_PATH, FileName);
	GetFilePath(newFilePath, MAX_PATH, NewFileName);

	DbgPrint(L"MoveFile %s -> %s\n\n", filePath, newFilePath);

	if (DokanFileInfo->Context) {
		// should close? or rename at closing?
		CloseHandle((HANDLE)DokanFileInfo->Context);
		DokanFileInfo->Context = 0;
	}

	if (ReplaceIfExisting)
		status = MoveFileEx(filePath, newFilePath, MOVEFILE_REPLACE_EXISTING);
	else
		status = MoveFile(filePath, newFilePath);

	if (status == FALSE) {
		DWORD error = GetLastError();
		DbgPrint(L"\tMoveFile failed status = %d, code = %d\n", status, error);
		return ToNtStatus(error);
	} else {
		return STATUS_SUCCESS;
	}
}


static NTSTATUS DOKAN_CALLBACK
MirrorLockFile(
	LPCWSTR				FileName,
	LONGLONG			ByteOffset,
	LONGLONG			Length,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR	filePath[MAX_PATH];
	HANDLE	handle;
	LARGE_INTEGER offset;
	LARGE_INTEGER length;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"LockFile %s\n", filePath);

	handle = (HANDLE)DokanFileInfo->Context;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_INVALID_HANDLE;
	}

	length.QuadPart = Length;
	offset.QuadPart = ByteOffset;

	if (!LockFile(handle, offset.HighPart, offset.LowPart, length.HighPart, length.LowPart)) {
		DWORD error = GetLastError();
		DbgPrint(L"\tfailed(%d)\n", error);
		return ToNtStatus(error);
	}

	DbgPrint(L"\tsuccess\n\n");
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorSetEndOfFile(
	LPCWSTR				FileName,
	LONGLONG			ByteOffset,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR			filePath[MAX_PATH];
	HANDLE			handle;
	LARGE_INTEGER	offset;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"SetEndOfFile %s, %I64d\n", filePath, ByteOffset);

	handle = (HANDLE)DokanFileInfo->Context;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_INVALID_HANDLE;
	}

	offset.QuadPart = ByteOffset;
	if (!SetFilePointerEx(handle, offset, NULL, FILE_BEGIN)) {
		DWORD error = GetLastError();
		DbgPrint(L"\tSetFilePointer error: %d, offset = %I64d\n\n",
			error, ByteOffset);
		return ToNtStatus(error);
	}

	if (!SetEndOfFile(handle)) {
		DWORD error = GetLastError();
		DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);
		return ToNtStatus(error);
	}

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorSetAllocationSize(
	LPCWSTR				FileName,
	LONGLONG			AllocSize,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR			filePath[MAX_PATH];
	HANDLE			handle;
	LARGE_INTEGER	fileSize;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"SetAllocationSize %s, %I64d\n", filePath, AllocSize);

	handle = (HANDLE)DokanFileInfo->Context;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_INVALID_HANDLE;
	}

	if (GetFileSizeEx(handle, &fileSize)) {
		if (AllocSize < fileSize.QuadPart) {
			fileSize.QuadPart = AllocSize;
			if (!SetFilePointerEx(handle, fileSize, NULL, FILE_BEGIN)) {
				DWORD error = GetLastError();
				DbgPrint(L"\tSetAllocationSize: SetFilePointer eror: %d, "
					L"offset = %I64d\n\n", error, AllocSize);
				return ToNtStatus(error);
			}
			if (!SetEndOfFile(handle)) {
				DWORD error = GetLastError();
				DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);
				return ToNtStatus(error);
			}
		}
	} else {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return ToNtStatus(error);
	}
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorSetFileAttributes(
	LPCWSTR				FileName,
	DWORD				FileAttributes,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

	WCHAR	filePath[MAX_PATH];
	
	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"SetFileAttributes %s\n", filePath);

	if (!SetFileAttributes(filePath, FileAttributes)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return ToNtStatus(error);
	}

	DbgPrint(L"\n");
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorSetFileTime(
	LPCWSTR				FileName,
	CONST FILETIME*		CreationTime,
	CONST FILETIME*		LastAccessTime,
	CONST FILETIME*		LastWriteTime,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR	filePath[MAX_PATH];
	HANDLE	handle;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"SetFileTime %s\n", filePath);

	handle = (HANDLE)DokanFileInfo->Context;

	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_INVALID_HANDLE;
	}

	if (!SetFileTime(handle, CreationTime, LastAccessTime, LastWriteTime)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return ToNtStatus(error);
	}

	DbgPrint(L"\n");
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorUnlockFile(
	LPCWSTR				FileName,
	LONGLONG			ByteOffset,
	LONGLONG			Length,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	WCHAR	filePath[MAX_PATH];
	HANDLE	handle;
	LARGE_INTEGER	length;
	LARGE_INTEGER	offset;

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"UnlockFile %s\n", filePath);

	handle = (HANDLE)DokanFileInfo->Context;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_INVALID_HANDLE;
	}

	length.QuadPart = Length;
	offset.QuadPart = ByteOffset;

	if (!UnlockFile(handle, offset.HighPart, offset.LowPart, length.HighPart, length.LowPart)) {
		DWORD error = GetLastError();
		DbgPrint(L"\terror code = %d\n\n", error);
		return ToNtStatus(error);
	}

	DbgPrint(L"\tsuccess\n\n");
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorGetFileSecurity(
	LPCWSTR					FileName,
	PSECURITY_INFORMATION	SecurityInformation,
	PSECURITY_DESCRIPTOR	SecurityDescriptor,
	ULONG				BufferLength,
	PULONG				LengthNeeded,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	HANDLE	handle;
	WCHAR	filePath[MAX_PATH];

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"GetFileSecurity %s\n", filePath);

	handle = (HANDLE)DokanFileInfo->Context;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_INVALID_HANDLE;
	}

	if (!GetUserObjectSecurity(handle, SecurityInformation, SecurityDescriptor,
			BufferLength, LengthNeeded)) {
		int error = GetLastError();
		if (error == ERROR_INSUFFICIENT_BUFFER) {
			DbgPrint(L"  GetUserObjectSecurity failed: ERROR_INSUFFICIENT_BUFFER\n");
			return STATUS_BUFFER_OVERFLOW;
		} else {
			DbgPrint(L"  GetUserObjectSecurity failed: %d\n", error);
			return ToNtStatus(error);
		}
	}
	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorSetFileSecurity(
	LPCWSTR					FileName,
	PSECURITY_INFORMATION	SecurityInformation,
	PSECURITY_DESCRIPTOR	SecurityDescriptor,
	ULONG				SecurityDescriptorLength,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
	HANDLE	handle;
	WCHAR	filePath[MAX_PATH];

	UNREFERENCED_PARAMETER(SecurityDescriptorLength);

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"SetFileSecurity %s\n", filePath);

	handle = (HANDLE)DokanFileInfo->Context;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_INVALID_HANDLE;
	}

	if (!SetUserObjectSecurity(handle, SecurityInformation, SecurityDescriptor)) {
		int error = GetLastError();
		DbgPrint(L"  SetUserObjectSecurity failed: %d\n", error);
		return ToNtStatus(error);
	}
	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorGetVolumeInformation(
	LPWSTR		VolumeNameBuffer,
	DWORD		VolumeNameSize,
	LPDWORD		VolumeSerialNumber,
	LPDWORD		MaximumComponentLength,
	LPDWORD		FileSystemFlags,
	LPWSTR		FileSystemNameBuffer,
	DWORD		FileSystemNameSize,
	PDOKAN_FILE_INFO	DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

	wcscpy_s(VolumeNameBuffer, VolumeNameSize, L"DOKAN");
	*VolumeSerialNumber = 0x19831116;
	*MaximumComponentLength = 256;
	*FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | 
						FILE_CASE_PRESERVED_NAMES | 
						FILE_SUPPORTS_REMOTE_STORAGE |
						FILE_UNICODE_ON_DISK |
						FILE_PERSISTENT_ACLS;

	wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"Dokan");

	return STATUS_SUCCESS;
}


static NTSTATUS DOKAN_CALLBACK
MirrorUnmount(
	PDOKAN_FILE_INFO	DokanFileInfo)
{
    UNREFERENCED_PARAMETER(DokanFileInfo);

	DbgPrint(L"Unmount\n");
	return STATUS_SUCCESS;
}

/**
 * Avoid #include <winternl.h> which as conflict with FILE_INFORMATION_CLASS definition.
 * This only for MirrorEnumerateNamedStreams. Link with ntdll.lib still required.
 * 
 * Not needed if you're not using NtQueryInformationFile!
 *
 * BEGIN
 */
typedef struct _IO_STATUS_BLOCK {
	union {
		NTSTATUS Status;
		PVOID Pointer;
	} DUMMYUNIONNAME;

	ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

NTSYSCALLAPI NTSTATUS NTAPI 	NtQueryInformationFile(_In_ HANDLE FileHandle, _Out_ PIO_STATUS_BLOCK IoStatusBlock, _Out_writes_bytes_(Length) PVOID FileInformation, _In_ ULONG Length, _In_ FILE_INFORMATION_CLASS FileInformationClass);
/**
 * END
 */

static NTSTATUS DOKAN_CALLBACK
MirrorEnumerateNamedStreams(
	LPCWSTR					FileName,
	PVOID*					EnumContext,
	LPWSTR					StreamName,
	PLONGLONG				StreamSize,
	PDOKAN_FILE_INFO		DokanFileInfo)
{
	HANDLE	handle;
	WCHAR	filePath[MAX_PATH];

	GetFilePath(filePath, MAX_PATH, FileName);

	DbgPrint(L"EnumerateNamedStreams %s\n", filePath);

	handle = (HANDLE)DokanFileInfo->Context;
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		DbgPrint(L"\tinvalid handle\n\n");
		return STATUS_NOT_IMPLEMENTED;
	}

	// As we are requested one by one, it would be better to use FindFirstStream / FindNextStream instead of requesting all streams each time
	// But this doesn't really matter on mirror sample
	BYTE InfoBlock[64 * 1024];
	PFILE_STREAM_INFORMATION pStreamInfo = (PFILE_STREAM_INFORMATION)InfoBlock;
	IO_STATUS_BLOCK ioStatus;
	ZeroMemory(InfoBlock, sizeof(InfoBlock));

	NTSTATUS status = NtQueryInformationFile(handle, &ioStatus, InfoBlock, sizeof(InfoBlock), FileStreamInformation);
	if (status != STATUS_SUCCESS) {
		DbgPrint(L"\tNtQueryInformationFile failed with %d.\n", status);
		return STATUS_NOT_IMPLEMENTED;
	}

	if (pStreamInfo->StreamNameLength == 0) {
		DbgPrint(L"\tNo stream found.\n");
		return STATUS_NOT_IMPLEMENTED;
	}

	UINT index = (UINT)*EnumContext;
	DbgPrint(L"\tStream #%d requested.\n", index);
	
	for (UINT i = 0; i != index; ++i) {
		if (pStreamInfo->NextEntryOffset == 0) {
			DbgPrint(L"\tNo more stream.\n");
			return STATUS_NOT_IMPLEMENTED;
		}
		pStreamInfo = (PFILE_STREAM_INFORMATION) ((LPBYTE)pStreamInfo + pStreamInfo->NextEntryOffset);   // Next stream record
	}

	wcscpy_s(StreamName, SHRT_MAX + 1, pStreamInfo->StreamName);
	*StreamSize = pStreamInfo->StreamSize.QuadPart;

	DbgPrint(L"\t Stream %ws\n", pStreamInfo->StreamName);

	// Remember next stream entry index
	*EnumContext = (PVOID)++index;

	return STATUS_SUCCESS;
}


int __cdecl
wmain(ULONG argc, PWCHAR argv[])
{
	int status;
	ULONG command;
	PDOKAN_OPERATIONS dokanOperations =
			(PDOKAN_OPERATIONS)malloc(sizeof(DOKAN_OPERATIONS));
	if (dokanOperations == NULL) {
		return EXIT_FAILURE;
	}
	PDOKAN_OPTIONS dokanOptions =
			(PDOKAN_OPTIONS)malloc(sizeof(DOKAN_OPTIONS));
	if (dokanOptions == NULL) {
		free(dokanOperations);
		return EXIT_FAILURE;
	}

	if (argc < 5) {
		fprintf(stderr, "mirror.exe\n"
			"  /r RootDirectory (ex. /r c:\\test)\n"
			"  /l DriveLetter (ex. /l m)\n"
			"  /t ThreadCount (ex. /t 5)\n"
			"  /d (enable debug output)\n"
			"  /s (use stderr for output)\n"
			"  /n (use network drive)\n"
			"  /m (use removable drive)\n"
            "  /i (Timeout in Milliseconds ex. /i 30000)\n");
		return EXIT_FAILURE;
	}

	g_DebugMode = FALSE;
	g_UseStdErr = FALSE;

	ZeroMemory(dokanOptions, sizeof(DOKAN_OPTIONS));
	dokanOptions->Version = DOKAN_VERSION;
	dokanOptions->ThreadCount = 0; // use default

	for (command = 1; command < argc; command++) {
		switch (towlower(argv[command][1])) {
		case L'r':
			command++;
			wcscpy_s(RootDirectory, sizeof(RootDirectory)/sizeof(WCHAR), argv[command]);
			DbgPrint(L"RootDirectory: %ls\n", RootDirectory);
			break;
		case L'l':
			command++;
			wcscpy_s(MountPoint, sizeof(MountPoint)/sizeof(WCHAR), argv[command]);
			dokanOptions->MountPoint = MountPoint;
			break;
		case L't':
			command++;
			dokanOptions->ThreadCount = (USHORT)_wtoi(argv[command]);
			break;
		case L'd':
			g_DebugMode = TRUE;
			break;
		case L's':
			g_UseStdErr = TRUE;
			break;
		case L'n':
			dokanOptions->Options |= DOKAN_OPTION_NETWORK;
			break;
		case L'm':
			dokanOptions->Options |= DOKAN_OPTION_REMOVABLE;
			break;
        case L'i':
            command++;
            dokanOptions->Timeout = (ULONG)_wtol(argv[command]);
            break;
		default:
			fwprintf(stderr, L"unknown command: %s\n", argv[command]);
			return EXIT_FAILURE;
		}
	}

	if (g_DebugMode) {
		dokanOptions->Options |= DOKAN_OPTION_DEBUG;
	}
	if (g_UseStdErr) {
		dokanOptions->Options |= DOKAN_OPTION_STDERR;
	}

	dokanOptions->Options |= DOKAN_OPTION_ALT_STREAM;

	ZeroMemory(dokanOperations, sizeof(DOKAN_OPERATIONS));
	dokanOperations->CreateFile = MirrorCreateFile;
	dokanOperations->OpenDirectory = MirrorOpenDirectory;
	dokanOperations->CreateDirectory = MirrorCreateDirectory;
	dokanOperations->Cleanup = MirrorCleanup;
	dokanOperations->CloseFile = MirrorCloseFile;
	dokanOperations->ReadFile = MirrorReadFile;
	dokanOperations->WriteFile = MirrorWriteFile;
	dokanOperations->FlushFileBuffers = MirrorFlushFileBuffers;
	dokanOperations->GetFileInformation = MirrorGetFileInformation;
	dokanOperations->FindFiles = MirrorFindFiles;
	dokanOperations->FindFilesWithPattern = NULL;
	dokanOperations->SetFileAttributes = MirrorSetFileAttributes;
	dokanOperations->SetFileTime = MirrorSetFileTime;
	dokanOperations->DeleteFile = MirrorDeleteFile;
	dokanOperations->DeleteDirectory = MirrorDeleteDirectory;
	dokanOperations->MoveFile = MirrorMoveFile;
	dokanOperations->SetEndOfFile = MirrorSetEndOfFile;
	dokanOperations->SetAllocationSize = MirrorSetAllocationSize;	
	dokanOperations->LockFile = MirrorLockFile;
	dokanOperations->UnlockFile = MirrorUnlockFile;
	dokanOperations->GetFileSecurity = MirrorGetFileSecurity;
	dokanOperations->SetFileSecurity = MirrorSetFileSecurity;
	dokanOperations->GetDiskFreeSpace = NULL;
	dokanOperations->GetVolumeInformation = MirrorGetVolumeInformation;
	dokanOperations->Unmount = MirrorUnmount;
	dokanOperations->EnumerateNamedStreams = MirrorEnumerateNamedStreams;

	status = DokanMain(dokanOptions, dokanOperations);
	switch (status) {
	case DOKAN_SUCCESS:
		fprintf(stderr, "Success\n");
		break;
	case DOKAN_ERROR:
		fprintf(stderr, "Error\n");
		break;
	case DOKAN_DRIVE_LETTER_ERROR:
		fprintf(stderr, "Bad Drive letter\n");
		break;
	case DOKAN_DRIVER_INSTALL_ERROR:
		fprintf(stderr, "Can't install driver\n");
		break;
	case DOKAN_START_ERROR:
		fprintf(stderr, "Driver something wrong\n");
		break;
	case DOKAN_MOUNT_ERROR:
		fprintf(stderr, "Can't assign a drive letter\n");
		break;
	case DOKAN_MOUNT_POINT_ERROR:
		fprintf(stderr, "Mount point error\n");
		break;
	default:
		fprintf(stderr, "Unknown error: %d\n", status);
		break;
	}

	free(dokanOptions);
	free(dokanOperations);
	return EXIT_SUCCESS;
}

