/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

  http://dokan-dev.github.io

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

#define MIRROR_DEBUG_MEMORY 1
#define MIRROR_IS_DEBUGGING_MEMORY (_DEBUG && MIRROR_DEBUG_MEMORY)

#if MIRROR_IS_DEBUGGING_MEMORY
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include "../../dokan/dokan.h"
#include "../../dokan/fileinfo.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <winbase.h>
#include <assert.h>

#define USE_ASYNC_IO 1

typedef struct _MIRROR_FILE_HANDLE {

	HANDLE		FileHandle;

#if USE_ASYNC_IO
	PTP_IO		IoCompletion;
#endif

} MIRROR_FILE_HANDLE;

#define MIRROR_HANDLE_ASSERT(mirrorHandle) assert((mirrorHandle) && (mirrorHandle)->FileHandle && (mirrorHandle)->FileHandle != INVALID_HANDLE_VALUE)

#if USE_ASYNC_IO

TP_CALLBACK_ENVIRON g_ThreadPoolEnvironment;
PTP_CLEANUP_GROUP g_ThreadPoolCleanupGroup = NULL;
PTP_POOL g_ThreadPool = NULL;
CRITICAL_SECTION g_ThreadPoolCS;

DOKAN_VECTOR g_OverlappedPool;
CRITICAL_SECTION g_OverlappedPoolCS;

typedef enum _MIRROR_IOTYPE {
	MIRROR_IOTYPE_UNKNOWN = 0,
	MIRROR_IOTYPE_READ,
	MIRROR_IOTYPE_WRITE
} MIRROR_IO_OP_TYPE;

typedef struct _MIRROR_OVERLAPPED {
	OVERLAPPED			InternalOverlapped;
	MIRROR_FILE_HANDLE	*FileHandle;
	void				*Context;
	MIRROR_IO_OP_TYPE	IoType;
} MIRROR_OVERLAPPED;

void CALLBACK MirrorIoCallback(
	_Inout_     PTP_CALLBACK_INSTANCE Instance,
	_Inout_opt_ PVOID                 Context,
	_Inout_opt_ PVOID                 Overlapped,
	_In_        ULONG                 IoResult,
	_In_        ULONG_PTR             NumberOfBytesTransferred,
	_Inout_     PTP_IO                Io
);

void CleanupPendingAsyncIO();

#endif

DOKAN_VECTOR g_FileHandlePool;
CRITICAL_SECTION g_FileHandlePoolCS;

BOOL g_UseStdErr;
BOOL g_DebugMode;
volatile LONG g_IsUnmounted = 0;

static void DbgPrint(LPCWSTR format, ...) {

  if (g_DebugMode) {

    const WCHAR *outputString;
    WCHAR *buffer = NULL;
    size_t length;
    va_list argp;

    va_start(argp, format);

    length = _vscwprintf(format, argp) + 1;

    buffer = _malloca(length * sizeof(WCHAR));

    if (buffer) {

      vswprintf_s(buffer, length, format, argp);
      outputString = buffer;
    }
	else {

      outputString = format;
    }

	OutputDebugStringW(outputString);

	if(g_UseStdErr) {

		fputws(outputString, stderr);
		fflush(stderr);
	}
      

	if(buffer) {

		_freea(buffer);
	}

    va_end(argp);
  }
}

/////////////////// MIRROR_FILE_HANDLE ///////////////////

void FreeMirrorFileHandle(MIRROR_FILE_HANDLE *FileHandle) {

	if(FileHandle) {

#if USE_ASYNC_IO
		if(FileHandle->IoCompletion) {

			EnterCriticalSection(&g_ThreadPoolCS);
			{
				if(g_ThreadPoolCleanupGroup) {

					CloseThreadpoolIo(FileHandle->IoCompletion);
				}
			}
			LeaveCriticalSection(&g_ThreadPoolCS);

			FileHandle->IoCompletion = NULL;
		}
#endif

		free(FileHandle);
	}
}

void PushMirrorFileHandle(MIRROR_FILE_HANDLE *FileHandle) {

	assert(FileHandle);

#if USE_ASYNC_IO
	if(FileHandle->IoCompletion) {

		EnterCriticalSection(&g_ThreadPoolCS);
		{
			if(g_ThreadPoolCleanupGroup) {

				CloseThreadpoolIo(FileHandle->IoCompletion);
			}
		}
		LeaveCriticalSection(&g_ThreadPoolCS);

		FileHandle->IoCompletion = NULL;
	}
#endif

	EnterCriticalSection(&g_FileHandlePoolCS);
	{
		DokanVector_PushBack(&g_FileHandlePool, &FileHandle);
	}
	LeaveCriticalSection(&g_FileHandlePoolCS);
}

MIRROR_FILE_HANDLE* PopMirrorFileHandle(HANDLE ActualFileHandle) {

	if(ActualFileHandle == NULL
		|| ActualFileHandle == INVALID_HANDLE_VALUE
		|| InterlockedAdd(&g_IsUnmounted, 0) != FALSE) {

		return NULL;
	}

	MIRROR_FILE_HANDLE *mirrorHandle = NULL;

	EnterCriticalSection(&g_FileHandlePoolCS);
	{
		if(DokanVector_GetCount(&g_FileHandlePool) > 0)
		{
			mirrorHandle = *(MIRROR_FILE_HANDLE**)DokanVector_GetLastItem(&g_FileHandlePool);
			DokanVector_PopBack(&g_FileHandlePool);
		}
	}
	LeaveCriticalSection(&g_FileHandlePoolCS);

	if(!mirrorHandle) {

		mirrorHandle = (MIRROR_FILE_HANDLE*)malloc(sizeof(MIRROR_FILE_HANDLE));
	}

	if(mirrorHandle) {

		RtlZeroMemory(mirrorHandle, sizeof(MIRROR_FILE_HANDLE));
		mirrorHandle->FileHandle = ActualFileHandle;

#if USE_ASYNC_IO

		EnterCriticalSection(&g_ThreadPoolCS);
		{
			if(g_ThreadPoolCleanupGroup) {

				mirrorHandle->IoCompletion = CreateThreadpoolIo(ActualFileHandle, MirrorIoCallback, mirrorHandle, &g_ThreadPoolEnvironment);
			}
		}
		LeaveCriticalSection(&g_ThreadPoolCS);

		if(!mirrorHandle->IoCompletion) {

			PushMirrorFileHandle(mirrorHandle);
			mirrorHandle = NULL;
		}
#endif
	}

	return mirrorHandle;
}

/////////////////// MIRROR_OVERLAPPED ///////////////////

#if USE_ASYNC_IO

void FreeMirrorOverlapped(MIRROR_OVERLAPPED *Overlapped) {

	if(Overlapped) {

		free(Overlapped);
	}
}

void PushMirrorOverlapped(MIRROR_OVERLAPPED *Overlapped) {

	assert(Overlapped);

	EnterCriticalSection(&g_OverlappedPoolCS);
	{
		DokanVector_PushBack(&g_OverlappedPool, &Overlapped);
	}
	LeaveCriticalSection(&g_OverlappedPoolCS);
}

MIRROR_OVERLAPPED* PopMirrorOverlapped() {

	MIRROR_OVERLAPPED *overlapped = NULL;

	EnterCriticalSection(&g_OverlappedPoolCS);
	{
		if(DokanVector_GetCount(&g_OverlappedPool) > 0)
		{
			overlapped = *(MIRROR_OVERLAPPED**)DokanVector_GetLastItem(&g_OverlappedPool);
			DokanVector_PopBack(&g_OverlappedPool);
		}
	}
	LeaveCriticalSection(&g_OverlappedPoolCS);

	if(!overlapped) {

		overlapped = (MIRROR_OVERLAPPED*)malloc(sizeof(MIRROR_OVERLAPPED));
	}

	if(overlapped) {

		RtlZeroMemory(overlapped, sizeof(MIRROR_OVERLAPPED));
	}

	return overlapped;
}

#endif

/////////////////// Push/Pop pattern finished ///////////////////

static WCHAR RootDirectory[MAX_PATH] = L"C:";
static WCHAR MountPoint[MAX_PATH] = L"M:\\";
static WCHAR UNCName[MAX_PATH] = L"";

static void GetFilePath(PWCHAR filePath, ULONG numberOfElements,
                        LPCWSTR FileName) {
  wcsncpy_s(filePath, numberOfElements, RootDirectory, wcslen(RootDirectory));
  size_t unclen = wcslen(UNCName);
  if (unclen > 0 && _wcsnicmp(FileName, UNCName, unclen) == 0) {
    if (_wcsnicmp(FileName + unclen, L".", 1) != 0) {
      wcsncat_s(filePath, numberOfElements, FileName + unclen,
                wcslen(FileName) - unclen);
    }
  } else {
    wcsncat_s(filePath, numberOfElements, FileName, wcslen(FileName));
  }
}

static void PrintUserName(DOKAN_CREATE_FILE_EVENT *EventInfo) {
  HANDLE handle;
  UCHAR buffer[1024];
  DWORD returnLength;
  WCHAR accountName[256];
  WCHAR domainName[256];
  DWORD accountLength = sizeof(accountName) / sizeof(WCHAR);
  DWORD domainLength = sizeof(domainName) / sizeof(WCHAR);
  PTOKEN_USER tokenUser;
  SID_NAME_USE snu;

  handle = DokanOpenRequestorToken(EventInfo);
  if (handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"  DokanOpenRequestorToken failed\n");
    return;
  }

  if (!GetTokenInformation(handle, TokenUser, buffer, sizeof(buffer),
                           &returnLength)) {
    DbgPrint(L"  GetTokenInformaiton failed: %d\n", GetLastError());
    CloseHandle(handle);
    return;
  }

  CloseHandle(handle);

  tokenUser = (PTOKEN_USER)buffer;
  if (!LookupAccountSid(NULL, tokenUser->User.Sid, accountName, &accountLength,
                        domainName, &domainLength, &snu)) {
    DbgPrint(L"  LookupAccountSid failed: %d\n", GetLastError());
    return;
  }

  DbgPrint(L"  AccountName: %s, DomainName: %s\n", accountName, domainName);
}

static BOOL AddSeSecurityNamePrivilege() {
  HANDLE token = 0;
  DbgPrint(
      L"## Attempting to add SE_SECURITY_NAME privilege to process token ##\n");
  DWORD err;
  LUID luid;
  if (!LookupPrivilegeValue(0, SE_SECURITY_NAME, &luid)) {
    err = GetLastError();
    if (err != ERROR_SUCCESS) {
      DbgPrint(L"  failed: Unable to lookup privilege value. error = %u\n",
               err);
      return FALSE;
    }
  }

  LUID_AND_ATTRIBUTES attr;
  attr.Attributes = SE_PRIVILEGE_ENABLED;
  attr.Luid = luid;

  TOKEN_PRIVILEGES priv;
  priv.PrivilegeCount = 1;
  priv.Privileges[0] = attr;

  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    err = GetLastError();
    if (err != ERROR_SUCCESS) {
      DbgPrint(L"  failed: Unable obtain process token. error = %u\n", err);
      return FALSE;
    }
  }

  TOKEN_PRIVILEGES oldPriv;
  DWORD retSize;
  AdjustTokenPrivileges(token, FALSE, &priv, sizeof(TOKEN_PRIVILEGES), &oldPriv,
                        &retSize);
  err = GetLastError();
  if (err != ERROR_SUCCESS) {
    DbgPrint(L"  failed: Unable to adjust token privileges: %u\n", err);
    CloseHandle(token);
    return FALSE;
  }

  BOOL privAlreadyPresent = FALSE;
  for (unsigned int i = 0; i < oldPriv.PrivilegeCount; i++) {
    if (oldPriv.Privileges[i].Luid.HighPart == luid.HighPart &&
        oldPriv.Privileges[i].Luid.LowPart == luid.LowPart) {
      privAlreadyPresent = TRUE;
      break;
    }
  }
  DbgPrint(privAlreadyPresent ? L"  success: privilege already present\n"
                              : L"  success: privilege added\n");
  if (token)
    CloseHandle(token);
  return TRUE;
}

#define MirrorCheckFlag(val, flag)                                             \
  if (val & flag) {                                                            \
    DbgPrint(L"\t" L#flag L"\n");                                              \
  }

static NTSTATUS DOKAN_CALLBACK
MirrorCreateFile(DOKAN_CREATE_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  HANDLE handle;
  DWORD fileAttr;
  NTSTATUS status = STATUS_SUCCESS;
  DWORD creationDisposition;
  DWORD fileAttributesAndFlags;
  DWORD error = 0;
  SECURITY_ATTRIBUTES securityAttrib;

  securityAttrib.nLength = sizeof(securityAttrib);
  securityAttrib.lpSecurityDescriptor =
	  EventInfo->SecurityContext.AccessState.SecurityDescriptor;
  securityAttrib.bInheritHandle = FALSE;

  
  DokanMapKernelToUserCreateFileFlags(
	  EventInfo->FileAttributes,
	  EventInfo->CreateOptions,
	  EventInfo->CreateDisposition,
	  &fileAttributesAndFlags,
      &creationDisposition);

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"CreateFile : %s\n", filePath);

  PrintUserName(EventInfo);

  /*
  if (ShareMode == 0 && AccessMode & FILE_WRITE_DATA)
          ShareMode = FILE_SHARE_WRITE;
  else if (ShareMode == 0)
          ShareMode = FILE_SHARE_READ;
  */

  DbgPrint(L"\tShareMode = 0x%x\n", EventInfo->ShareAccess);

  MirrorCheckFlag(EventInfo->ShareAccess, FILE_SHARE_READ);
  MirrorCheckFlag(EventInfo->ShareAccess, FILE_SHARE_WRITE);
  MirrorCheckFlag(EventInfo->ShareAccess, FILE_SHARE_DELETE);

  DbgPrint(L"\tAccessMode = 0x%x\n", EventInfo->DesiredAccess);

  MirrorCheckFlag(EventInfo->DesiredAccess, GENERIC_READ);
  MirrorCheckFlag(EventInfo->DesiredAccess, GENERIC_WRITE);
  MirrorCheckFlag(EventInfo->DesiredAccess, GENERIC_EXECUTE);

  MirrorCheckFlag(EventInfo->DesiredAccess, DELETE);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_READ_DATA);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_READ_ATTRIBUTES);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_READ_EA);
  MirrorCheckFlag(EventInfo->DesiredAccess, READ_CONTROL);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_WRITE_DATA);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_WRITE_ATTRIBUTES);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_WRITE_EA);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_APPEND_DATA);
  MirrorCheckFlag(EventInfo->DesiredAccess, WRITE_DAC);
  MirrorCheckFlag(EventInfo->DesiredAccess, WRITE_OWNER);
  MirrorCheckFlag(EventInfo->DesiredAccess, SYNCHRONIZE);
  MirrorCheckFlag(EventInfo->DesiredAccess, FILE_EXECUTE);
  MirrorCheckFlag(EventInfo->DesiredAccess, STANDARD_RIGHTS_READ);
  MirrorCheckFlag(EventInfo->DesiredAccess, STANDARD_RIGHTS_WRITE);
  MirrorCheckFlag(EventInfo->DesiredAccess, STANDARD_RIGHTS_EXECUTE);

  // When filePath is a directory, needs to change the flag so that the file can
  // be opened.
  fileAttr = GetFileAttributes(filePath);

  if (fileAttr != INVALID_FILE_ATTRIBUTES
	  && (fileAttr & FILE_ATTRIBUTE_DIRECTORY)
	  && !(EventInfo->CreateOptions & FILE_NON_DIRECTORY_FILE)) {

	  EventInfo->DokanFileInfo->IsDirectory = TRUE;
    
	  if (EventInfo->DesiredAccess & DELETE) {
          // Needed by FindFirstFile to see if directory is empty or not
		  EventInfo->ShareAccess |= FILE_SHARE_READ;
    }
  }

  DbgPrint(L"\tFlagsAndAttributes = 0x%x\n", fileAttributesAndFlags);

  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_ARCHIVE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_ENCRYPTED);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_HIDDEN);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_NORMAL);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_OFFLINE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_READONLY);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_SYSTEM);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_ATTRIBUTE_TEMPORARY);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_WRITE_THROUGH);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_OVERLAPPED);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_NO_BUFFERING);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_RANDOM_ACCESS);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_SEQUENTIAL_SCAN);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_DELETE_ON_CLOSE);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_BACKUP_SEMANTICS);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_POSIX_SEMANTICS);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_OPEN_REPARSE_POINT);
  MirrorCheckFlag(fileAttributesAndFlags, FILE_FLAG_OPEN_NO_RECALL);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_ANONYMOUS);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_IDENTIFICATION);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_IMPERSONATION);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_DELEGATION);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_CONTEXT_TRACKING);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_EFFECTIVE_ONLY);
  MirrorCheckFlag(fileAttributesAndFlags, SECURITY_SQOS_PRESENT);

#if USE_ASYNC_IO
  fileAttributesAndFlags |= FILE_FLAG_OVERLAPPED;
#endif

  if (creationDisposition == CREATE_NEW) {
    DbgPrint(L"\tCREATE_NEW\n");
  } else if (creationDisposition == OPEN_ALWAYS) {
    DbgPrint(L"\tOPEN_ALWAYS\n");
  } else if (creationDisposition == CREATE_ALWAYS) {
    DbgPrint(L"\tCREATE_ALWAYS\n");
  } else if (creationDisposition == OPEN_EXISTING) {
    DbgPrint(L"\tOPEN_EXISTING\n");
  } else if (creationDisposition == TRUNCATE_EXISTING) {
    DbgPrint(L"\tTRUNCATE_EXISTING\n");
  } else {
    DbgPrint(L"\tUNKNOWN creationDisposition!\n");
  }

  if (EventInfo->DokanFileInfo->IsDirectory) {

    // It is a create directory request
    if (creationDisposition == CREATE_NEW) {

      if (!CreateDirectory(filePath, &securityAttrib)) {

        error = GetLastError();
        DbgPrint(L"\terror code = %d\n\n", error);
        status = DokanNtStatusFromWin32(error);
      }
    }
	else if (creationDisposition == OPEN_ALWAYS) {

      if (!CreateDirectory(filePath, &securityAttrib)) {

        error = GetLastError();

        if (error != ERROR_ALREADY_EXISTS) {

          DbgPrint(L"\terror code = %d\n\n", error);
          status = DokanNtStatusFromWin32(error);
        }
      }
    }

    if (status == STATUS_SUCCESS) {

      // FILE_FLAG_BACKUP_SEMANTICS is required for opening directory handles

      handle = CreateFile(
          filePath, EventInfo->DesiredAccess, EventInfo->ShareAccess, &securityAttrib, OPEN_EXISTING,
          fileAttributesAndFlags | FILE_FLAG_BACKUP_SEMANTICS, NULL);

      if (handle == INVALID_HANDLE_VALUE) {

        error = GetLastError();
        DbgPrint(L"\terror code = %d\n\n", error);

        status = DokanNtStatusFromWin32(error);
      }
	  else {

		  MIRROR_FILE_HANDLE *mirrorHandle = PopMirrorFileHandle(handle);

		  if(!mirrorHandle) {

			  DbgPrint(L"\tFailed to create MIRROR_FILE_HANDLE\n");

			  SetLastError(ERROR_INTERNAL_ERROR);

			  CloseHandle(handle);

			  return STATUS_INTERNAL_ERROR;
		  }

		  // save the file handle in Context
		  EventInfo->DokanFileInfo->Context = (ULONG64)mirrorHandle;
      }
    }
  }
  else {

    // It is a create file request

	  if(fileAttr != INVALID_FILE_ATTRIBUTES &&
		  (fileAttr & FILE_ATTRIBUTE_DIRECTORY) &&
		  EventInfo->CreateDisposition == FILE_CREATE) {

		  return STATUS_OBJECT_NAME_COLLISION; // File already exist because
											   // GetFileAttributes found it
	  }

    handle =
        CreateFile(filePath,
                   EventInfo->DesiredAccess, // GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
                   EventInfo->ShareAccess,
                   &securityAttrib, // security attribute
                   creationDisposition,
                   fileAttributesAndFlags, // |FILE_FLAG_NO_BUFFERING,
                   NULL);                  // template file handle

    if (handle == INVALID_HANDLE_VALUE) {

      error = GetLastError();
      DbgPrint(L"\terror code = %d\n\n", error);

      status = DokanNtStatusFromWin32(error);
    }
	else {

		MIRROR_FILE_HANDLE *mirrorHandle = PopMirrorFileHandle(handle);

		if(!mirrorHandle) {

			DbgPrint(L"\tFailed to create MIRROR_FILE_HANDLE\n");

			SetLastError(ERROR_INTERNAL_ERROR);

			CloseHandle(handle);
			
			return STATUS_INTERNAL_ERROR;
		}

		// save the file handle in Context
		EventInfo->DokanFileInfo->Context = (ULONG64)mirrorHandle;

      if (creationDisposition == OPEN_ALWAYS ||
          creationDisposition == CREATE_ALWAYS) {

        error = GetLastError();

        if (error == ERROR_ALREADY_EXISTS) {

          DbgPrint(L"\tOpen an already existing file\n");
          SetLastError(ERROR_ALREADY_EXISTS); // Inform the driver that we have
                                              // open a already existing file
          return STATUS_SUCCESS;
        }
      }
    }
  }

  DbgPrint(L"\n");

  return status;
}

#pragma warning(push)
#pragma warning(disable : 4305)

static void DOKAN_CALLBACK MirrorCloseFile(DOKAN_CLOSE_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"CloseFile: %s\n", filePath);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  if (mirrorHandle) {

    CloseHandle(mirrorHandle->FileHandle);
	
	mirrorHandle->FileHandle = NULL;
	EventInfo->DokanFileInfo->Context = 0;

	PushMirrorFileHandle(mirrorHandle);

	if(EventInfo->DokanFileInfo->DeleteOnClose) {

		// Should already be deleted by CloseHandle
		// if open with FILE_FLAG_DELETE_ON_CLOSE
		DbgPrint(L"\tDeleteOnClose\n");

		if(EventInfo->DokanFileInfo->IsDirectory) {

			DbgPrint(L"  DeleteDirectory ");

			if(!RemoveDirectory(filePath)) {

				DbgPrint(L"error code = %d\n\n", GetLastError());
			}
			else {
				DbgPrint(L"success\n\n");
			}
		}
		else {

			DbgPrint(L"  DeleteFile ");

			if(DeleteFile(filePath) == 0) {

				DbgPrint(L" error code = %d\n\n", GetLastError());
			}
			else {

				DbgPrint(L"success\n\n");
			}
		}
	}
  }
}

static void DOKAN_CALLBACK MirrorCleanup(DOKAN_CLEANUP_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"Cleanup: %s\n\n", filePath);

  /* This gets called BEFORE MirrorCloseFile(). Per the documentation:
   *
   * Receipt of the IRP_MJ_CLEANUP request indicates that the handle reference count on a file object has reached zero.
   * (In other words, all handles to the file object have been closed.)
   * Often it is sent when a user-mode application has called the Microsoft Win32 CloseHandle function
   * (or when a kernel-mode driver has called ZwClose) on the last outstanding handle to a file object.
   *
   * It is important to note that when all handles to a file object have been closed, this does not necessarily
   * mean that the file object is no longer being used. System components, such as the Cache Manager and the Memory Manager,
   * might hold outstanding references to the file object. These components can still read to or write from a file, even
   * after an IRP_MJ_CLEANUP request is received.
   */
}

static NTSTATUS DOKAN_CALLBACK MirrorReadFile(DOKAN_READ_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"ReadFile : %s\n", filePath);

  if(!mirrorHandle) {

	  return STATUS_FILE_CLOSED;
  }

#if USE_ASYNC_IO

  MIRROR_OVERLAPPED *overlapped = PopMirrorOverlapped();

  if(!overlapped) {

	  return STATUS_MEMORY_NOT_ALLOCATED;
  }

  overlapped->InternalOverlapped.Offset = (DWORD)(EventInfo->Offset & 0xffffffff);
  overlapped->InternalOverlapped.OffsetHigh = (DWORD)((EventInfo->Offset >> 32) & 0xffffffff);
  overlapped->FileHandle = mirrorHandle;
  overlapped->Context = EventInfo;
  overlapped->IoType = MIRROR_IOTYPE_READ;

  StartThreadpoolIo(mirrorHandle->IoCompletion);

  if(!ReadFile(mirrorHandle->FileHandle,
	  EventInfo->Buffer,
	  EventInfo->NumberOfBytesToRead,
	  &EventInfo->NumberOfBytesRead,
	  (LPOVERLAPPED)overlapped)) {

	  int lastError = GetLastError();

	  if(lastError != ERROR_IO_PENDING) {

		  CancelThreadpoolIo(mirrorHandle->IoCompletion);

		  DbgPrint(L"\tread error = %u, buffer length = %d, read length = %d\n\n",
			  lastError, EventInfo->NumberOfBytesToRead, EventInfo->NumberOfBytesRead);

		  return DokanNtStatusFromWin32(lastError);
	  }
  }

  return STATUS_PENDING;

#else
  LARGE_INTEGER distanceToMove;

  distanceToMove.QuadPart = EventInfo->Offset;

  if (!SetFilePointerEx(mirrorHandle->FileHandle, distanceToMove, NULL, FILE_BEGIN)) {

    DWORD error = GetLastError();

    DbgPrint(L"\tseek error, offset = %d\n\n", EventInfo->Offset);

    return DokanNtStatusFromWin32(error);
  }

  if (!ReadFile(mirrorHandle->FileHandle,
	  EventInfo->Buffer,
	  EventInfo->NumberOfBytesToRead,
	  &EventInfo->NumberOfBytesRead,
	  NULL)) {

    DWORD error = GetLastError();

    DbgPrint(L"\tread error = %u, buffer length = %d, read length = %d\n\n",
             error, EventInfo->NumberOfBytesToRead, EventInfo->NumberOfBytesRead);

    return DokanNtStatusFromWin32(error);
  }
  else {

    DbgPrint(L"\tByte to read: %d, Byte read %d, offset %d\n\n",
		EventInfo->NumberOfBytesToRead,
		EventInfo->NumberOfBytesRead,
		EventInfo->Offset);
  }

  return STATUS_SUCCESS;
#endif
}

static NTSTATUS DOKAN_CALLBACK MirrorWriteFile(DOKAN_WRITE_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"WriteFile : %s, offset %I64d, length %d\n", filePath, EventInfo->Offset,
	  EventInfo->NumberOfBytesToWrite);

  if(!mirrorHandle) {

	  return STATUS_FILE_CLOSED;
  }

  UINT64 fileSize = 0;
  DWORD fileSizeLow = 0;
  DWORD fileSizeHigh = 0;

  fileSizeLow = GetFileSize(mirrorHandle->FileHandle, &fileSizeHigh);

  if (fileSizeLow == INVALID_FILE_SIZE) {

    DWORD error = GetLastError();

    DbgPrint(L"\tcan not get a file size error = %d\n", error);

    return DokanNtStatusFromWin32(error);
  }

  fileSize = ((UINT64)fileSizeHigh << 32) | fileSizeLow;

#if USE_ASYNC_IO

  if(EventInfo->DokanFileInfo->PagingIo) {

	  if((UINT64)EventInfo->Offset >= fileSize) {

		  EventInfo->NumberOfBytesWritten = 0;

		  return STATUS_SUCCESS;
	  }

	  if(((UINT64)EventInfo->Offset + EventInfo->NumberOfBytesToWrite) > fileSize) {

		  UINT64 bytes = fileSize - EventInfo->Offset;

		  if(bytes >> 32) {

			  EventInfo->NumberOfBytesToWrite = (DWORD)(bytes & 0xFFFFFFFFUL);
		  }
		  else {

			  EventInfo->NumberOfBytesToWrite = (DWORD)bytes;
		  }
	  }
  }

  MIRROR_OVERLAPPED *overlapped = PopMirrorOverlapped();

  if(!overlapped) {

	  return STATUS_MEMORY_NOT_ALLOCATED;
  }

  overlapped->InternalOverlapped.Offset = (DWORD)(EventInfo->Offset & 0xffffffff);
  overlapped->InternalOverlapped.OffsetHigh = (DWORD)((EventInfo->Offset >> 32) & 0xffffffff);
  overlapped->FileHandle = mirrorHandle;
  overlapped->Context = EventInfo;
  overlapped->IoType = MIRROR_IOTYPE_WRITE;

  StartThreadpoolIo(mirrorHandle->IoCompletion);

  if(!WriteFile(mirrorHandle->FileHandle,
	  EventInfo->Buffer,
	  EventInfo->NumberOfBytesToWrite,
	  &EventInfo->NumberOfBytesWritten,
	  (LPOVERLAPPED)overlapped)) {

	  int lastError = GetLastError();

	  if(lastError != ERROR_IO_PENDING) {

		  CancelThreadpoolIo(mirrorHandle->IoCompletion);

		  DbgPrint(L"\twrite error = %u, buffer length = %d, write length = %d\n",
			  lastError, EventInfo->NumberOfBytesToWrite, EventInfo->NumberOfBytesWritten);

		  return DokanNtStatusFromWin32(lastError);
	  }
  }

  return STATUS_PENDING;

#else
  LARGE_INTEGER distanceToMove;

  if (EventInfo->DokanFileInfo->WriteToEndOfFile) {

    LARGE_INTEGER z;
    z.QuadPart = 0;

    if (!SetFilePointerEx(mirrorHandle->FileHandle, z, NULL, FILE_END)) {

      DWORD error = GetLastError();

      DbgPrint(L"\tseek error, offset = EOF, error = %d\n", error);

      return DokanNtStatusFromWin32(error);
    }
  }
  else {
    // Paging IO cannot write after allocate file size.
    if (EventInfo->DokanFileInfo->PagingIo) {

      if ((UINT64)EventInfo->Offset >= fileSize) {

		  EventInfo->NumberOfBytesWritten = 0;
		  
		  return STATUS_SUCCESS;
      }

      if (((UINT64)EventInfo->Offset + EventInfo->NumberOfBytesToWrite) > fileSize) {

        UINT64 bytes = fileSize - EventInfo->Offset;

        if (bytes >> 32) {

			EventInfo->NumberOfBytesToWrite = (DWORD)(bytes & 0xFFFFFFFFUL);
        }
		else {

			EventInfo->NumberOfBytesToWrite = (DWORD)bytes;
        }
      }
    }

    if ((UINT64)EventInfo->Offset > fileSize) {
      // In the mirror sample helperZeroFileData is not necessary. NTFS will
      // zero a hole.
      // But if user's file system is different from NTFS( or other Windows's
      // file systems ) then  users will have to zero the hole themselves.
    }

    distanceToMove.QuadPart = EventInfo->Offset;

    if (!SetFilePointerEx(mirrorHandle->FileHandle, distanceToMove, NULL, FILE_BEGIN)) {

      DWORD error = GetLastError();

      DbgPrint(L"\tseek error, offset = %I64d, error = %d\n", EventInfo->Offset, error);

      return DokanNtStatusFromWin32(error);
    }
  }

  if (!WriteFile(mirrorHandle->FileHandle,
	  EventInfo->Buffer,
	  EventInfo->NumberOfBytesToWrite,
	  &EventInfo->NumberOfBytesWritten,
	  NULL)) {

    DWORD error = GetLastError();

    DbgPrint(L"\twrite error = %u, buffer length = %d, write length = %d\n",
             error, EventInfo->NumberOfBytesToWrite, EventInfo->NumberOfBytesWritten);

    return DokanNtStatusFromWin32(error);
  }
  else {

    DbgPrint(L"\twrite %d, offset %I64d\n\n", EventInfo->NumberOfBytesWritten, EventInfo->Offset);
  }

  return STATUS_SUCCESS;
#endif
}

static NTSTATUS DOKAN_CALLBACK
MirrorFlushFileBuffers(DOKAN_FLUSH_BUFFERS_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"FlushFileBuffers : %s\n", filePath);

  if(!mirrorHandle) {

	  return STATUS_FILE_CLOSED;
  }

  if (FlushFileBuffers(mirrorHandle->FileHandle)) {

    return STATUS_SUCCESS;
  }
  else {

    DWORD error = GetLastError();
    
	DbgPrint(L"\tflush error code = %d\n", error);

    return DokanNtStatusFromWin32(error);
  }
}

static NTSTATUS DOKAN_CALLBACK MirrorGetFileInformation(DOKAN_GET_FILE_INFO_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"GetFileInfo : %s\n", filePath);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  if (!GetFileInformationByHandle(mirrorHandle->FileHandle, &EventInfo->FileHandleInfo)) {

    DbgPrint(L"\terror code = %d\n", GetLastError());

    // FileName is a root directory
    // in this case, FindFirstFile can't get directory information
    if (wcslen(EventInfo->FileName) == 1) {

      DbgPrint(L"  root dir\n");
	  EventInfo->FileHandleInfo.dwFileAttributes = GetFileAttributes(filePath);

    }
	else {

      WIN32_FIND_DATAW find;
      ZeroMemory(&find, sizeof(WIN32_FIND_DATAW));

      HANDLE findHandle = FindFirstFile(filePath, &find);

      if (findHandle == INVALID_HANDLE_VALUE) {

        DWORD error = GetLastError();

        DbgPrint(L"\tFindFirstFile error code = %d\n\n", error);

        return DokanNtStatusFromWin32(error);
      }

      EventInfo->FileHandleInfo.dwFileAttributes = find.dwFileAttributes;
      EventInfo->FileHandleInfo.ftCreationTime = find.ftCreationTime;
      EventInfo->FileHandleInfo.ftLastAccessTime = find.ftLastAccessTime;
      EventInfo->FileHandleInfo.ftLastWriteTime = find.ftLastWriteTime;
      EventInfo->FileHandleInfo.nFileSizeHigh = find.nFileSizeHigh;
      EventInfo->FileHandleInfo.nFileSizeLow = find.nFileSizeLow;

      DbgPrint(L"\tFindFiles OK, file size = %d\n", find.nFileSizeLow);

      FindClose(findHandle);
    }
  }
  else {

    DbgPrint(L"\tGetFileInformationByHandle success, file size = %d\n",
		EventInfo->FileHandleInfo.nFileSizeLow);
  }

  DbgPrint(L"\n");

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorFindFiles(DOKAN_FIND_FILES_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  size_t fileLen;
  HANDLE hFind;
  WIN32_FIND_DATAW findData;
  DWORD error;
  int count = 0;

  GetFilePath(filePath, MAX_PATH, EventInfo->PathName);

  DbgPrint(L"FindFiles :%s\n", filePath);

  fileLen = wcslen(filePath);
  if (filePath[fileLen - 1] != L'\\') {
    filePath[fileLen++] = L'\\';
  }
  filePath[fileLen] = L'*';
  filePath[fileLen + 1] = L'\0';

  hFind = FindFirstFile(filePath, &findData);

  if (hFind == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    DbgPrint(L"\tinvalid file handle. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  // Root folder does not have . and .. folder - we remove them
  BOOLEAN rootFolder = (wcscmp(EventInfo->PathName, L"\\") == 0);

  do {
	  if(!rootFolder || (wcscmp(findData.cFileName, L".") != 0 &&
		  wcscmp(findData.cFileName, L"..") != 0))
	  {
		  EventInfo->FillFindData(EventInfo, &findData);
	  }

    count++;

  } while (FindNextFile(hFind, &findData) != 0);

  error = GetLastError();
  FindClose(hFind);

  if (error != ERROR_NO_MORE_FILES) {
    DbgPrint(L"\tFindNextFile error. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tFindFiles return %d entries in %s\n\n", count, filePath);

  return STATUS_SUCCESS;
}

NTSTATUS
MirrorCanDeleteDirectory(LPWSTR filePath) {

	HANDLE hFind;
	WIN32_FIND_DATAW findData;
	size_t fileLen;

	DbgPrint(L"CanDeleteDirectory %s\n", filePath);

	fileLen = wcslen(filePath);

	if(filePath[fileLen - 1] != L'\\') {

		filePath[fileLen++] = L'\\';
	}

	filePath[fileLen] = L'*';
	filePath[fileLen + 1] = L'\0';

	hFind = FindFirstFile(filePath, &findData);

	if(hFind == INVALID_HANDLE_VALUE) {

		DWORD error = GetLastError();
		DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);

		return DokanNtStatusFromWin32(error);
	}

	do {

		if(wcscmp(findData.cFileName, L"..") != 0 &&
			wcscmp(findData.cFileName, L".") != 0) {

			FindClose(hFind);
			DbgPrint(L"\tDirectory is not empty: %s\n", findData.cFileName);

			return STATUS_DIRECTORY_NOT_EMPTY;
		}

	} while(FindNextFile(hFind, &findData) != 0);

	DWORD error = GetLastError();

	if(error != ERROR_NO_MORE_FILES) {

		DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);
		return DokanNtStatusFromWin32(error);
	}

	FindClose(hFind);

	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorCanDeleteFile(DOKAN_CAN_DELETE_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);
  DbgPrint(L"CanDeleteFile %s\n", filePath);

  if(EventInfo->DokanFileInfo->IsDirectory) {

	  return MirrorCanDeleteDirectory(filePath);
  }

  DWORD dwAttrib = GetFileAttributes(filePath);

  if(dwAttrib == INVALID_FILE_ATTRIBUTES)
  {
	  return DokanNtStatusFromWin32(GetLastError());
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorMoveFile(DOKAN_MOVE_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  WCHAR newFilePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;
  DWORD bufferSize;
  BOOL result;
  size_t newFilePathLen;

  PFILE_RENAME_INFO renameInfo = NULL;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);
  GetFilePath(newFilePath, MAX_PATH, EventInfo->NewFileName);

  DbgPrint(L"MoveFile %s -> %s\n\n", filePath, newFilePath);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  newFilePathLen = wcslen(newFilePath);

  // the PFILE_RENAME_INFO struct has space for one WCHAR for the name at
  // the end, so that
  // accounts for the null terminator

  bufferSize = (DWORD)(sizeof(FILE_RENAME_INFO) +
                       newFilePathLen * sizeof(newFilePath[0]));

  renameInfo = (PFILE_RENAME_INFO)malloc(bufferSize);

  if (!renameInfo) {

    return STATUS_BUFFER_OVERFLOW;
  }

  ZeroMemory(renameInfo, bufferSize);

  renameInfo->ReplaceIfExists =
	  EventInfo->ReplaceIfExists
          ? TRUE
          : FALSE; // some warning about converting BOOL to BOOLEAN

  renameInfo->RootDirectory = NULL; // hope it is never needed, shouldn't be

  renameInfo->FileNameLength =
      (DWORD)newFilePathLen *
      sizeof(newFilePath[0]); // they want length in bytes

  wcscpy_s(renameInfo->FileName, newFilePathLen + 1, newFilePath);

  result = SetFileInformationByHandle(mirrorHandle->FileHandle, FileRenameInfo, renameInfo, bufferSize);

  free(renameInfo);

  if (result) {

    return STATUS_SUCCESS;
  }
  else {

    DWORD error = GetLastError();
    
	DbgPrint(L"\tMoveFile error = %u\n", error);

    return DokanNtStatusFromWin32(error);
  }
}

static NTSTATUS DOKAN_CALLBACK MirrorLockFile(DOKAN_LOCK_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;
  LARGE_INTEGER offset;
  LARGE_INTEGER length;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"LockFile %s\n", filePath);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  length.QuadPart = EventInfo->Length;
  offset.QuadPart = EventInfo->ByteOffset;

  if (!LockFile(mirrorHandle->FileHandle, offset.LowPart, offset.HighPart, length.LowPart, length.HighPart)) {

    DWORD error = GetLastError();

    DbgPrint(L"\terror code = %d\n\n", error);

    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tsuccess\n\n");

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetEndOfFile(DOKAN_SET_EOF_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;
  LARGE_INTEGER offset;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"SetEndOfFile %s, %I64d\n", filePath, EventInfo->Length);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  offset.QuadPart = EventInfo->Length;

  if (!SetFilePointerEx(mirrorHandle->FileHandle, offset, NULL, FILE_BEGIN)) {

    DWORD error = GetLastError();

    DbgPrint(L"\tSetFilePointer error: %d, offset = %I64d\n\n", error,
		EventInfo->Length);

    return DokanNtStatusFromWin32(error);
  }

  if (!SetEndOfFile(mirrorHandle->FileHandle)) {

    DWORD error = GetLastError();

    DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);

    return DokanNtStatusFromWin32(error);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetAllocationSize(DOKAN_SET_ALLOCATION_SIZE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;
  LARGE_INTEGER fileSize;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"SetAllocationSize %s, %I64d\n", filePath, EventInfo->Length);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  if (GetFileSizeEx(mirrorHandle->FileHandle, &fileSize)) {
    
	  if (EventInfo->Length < fileSize.QuadPart) {

      fileSize.QuadPart = EventInfo->Length;

      if (!SetFilePointerEx(mirrorHandle->FileHandle, fileSize, NULL, FILE_BEGIN)) {
        
		DWORD error = GetLastError();

        DbgPrint(L"\tSetAllocationSize: SetFilePointer eror: %d, "
                 L"offset = %I64d\n\n",
                 error, EventInfo->Length);

        return DokanNtStatusFromWin32(error);
      }

      if (!SetEndOfFile(mirrorHandle->FileHandle)) {
        
		DWORD error = GetLastError();

        DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);

        return DokanNtStatusFromWin32(error);
      }
    }
  }
  else {

    DWORD error = GetLastError();

    DbgPrint(L"\terror code = %d\n\n", error);

    return DokanNtStatusFromWin32(error);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetFileBasicInformation(DOKAN_SET_FILE_BASIC_INFO_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"SetFileBasicInformation %s\n", filePath);

  MIRROR_HANDLE_ASSERT(mirrorHandle);
  assert(sizeof(FILE_BASIC_INFORMATION) == sizeof(FILE_BASIC_INFO));

  // Due to potential byte alignment issues we need to make a copy of the input data
  FILE_BASIC_INFO basicInfo;
  memcpy_s(&basicInfo, sizeof(basicInfo), EventInfo->Info, sizeof(FILE_BASIC_INFORMATION));

  if (!SetFileInformationByHandle(mirrorHandle->FileHandle,
	  FileBasicInfo,
	  &basicInfo,
	  (DWORD)sizeof(basicInfo))) {

    DWORD error = GetLastError();

    DbgPrint(L"\terror code = %d\n\n", error);

    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\n");

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorUnlockFile(DOKAN_UNLOCK_FILE_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;
  LARGE_INTEGER length;
  LARGE_INTEGER offset;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"UnlockFile %s\n", filePath);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  length.QuadPart = EventInfo->Length;
  offset.QuadPart = EventInfo->ByteOffset;

  if (!UnlockFile(mirrorHandle->FileHandle, offset.LowPart, offset.HighPart, length.LowPart, length.HighPart)) {

    DWORD error = GetLastError();

    DbgPrint(L"\terror code = %d\n\n", error);

    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tsuccess\n\n");

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorGetFileSecurity(DOKAN_GET_FILE_SECURITY_EVENT *EventInfo) {

	WCHAR filePath[MAX_PATH];
	MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;

	GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

	DbgPrint(L"GetFileSecurity %s\n", filePath);

	MIRROR_HANDLE_ASSERT(mirrorHandle);

	MirrorCheckFlag(*EventInfo->SecurityInformation, FILE_SHARE_READ);
	MirrorCheckFlag(*EventInfo->SecurityInformation, OWNER_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, GROUP_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, DACL_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, SACL_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, LABEL_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, ATTRIBUTE_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, SCOPE_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation,
		PROCESS_TRUST_LABEL_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, BACKUP_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, PROTECTED_DACL_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, PROTECTED_SACL_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, UNPROTECTED_DACL_SECURITY_INFORMATION);
	MirrorCheckFlag(*EventInfo->SecurityInformation, UNPROTECTED_SACL_SECURITY_INFORMATION);

	DbgPrint(L"  Opening new handle with READ_CONTROL access\n");

	HANDLE handle = CreateFile(
		filePath,
		READ_CONTROL | (((*EventInfo->SecurityInformation & SACL_SECURITY_INFORMATION) ||
		(*EventInfo->SecurityInformation & BACKUP_SECURITY_INFORMATION))
			? ACCESS_SYSTEM_SECURITY
			: 0),
		FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE,
		NULL, // security attribute
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, // |FILE_FLAG_NO_BUFFERING,
		NULL);

	if(!handle || handle == INVALID_HANDLE_VALUE) {

		DbgPrint(L"\tinvalid handle\n\n");

		int error = GetLastError();

		return DokanNtStatusFromWin32(error);
	}

	if(!GetUserObjectSecurity(handle, EventInfo->SecurityInformation, EventInfo->SecurityDescriptor,
		EventInfo->SecurityDescriptorSize, &EventInfo->LengthNeeded)) {

		int error = GetLastError();

		if(error == ERROR_INSUFFICIENT_BUFFER) {
				
			DbgPrint(L"  GetUserObjectSecurity error: ERROR_INSUFFICIENT_BUFFER\n");
				
			CloseHandle(handle);
				
			return STATUS_BUFFER_OVERFLOW;
		}
		else {
				
			DbgPrint(L"  GetUserObjectSecurity error: %d\n", error);
				
			CloseHandle(handle);
				
			return DokanNtStatusFromWin32(error);
		}
	}

	CloseHandle(handle);

	return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetFileSecurity(DOKAN_SET_FILE_SECURITY_EVENT *EventInfo) {

  MIRROR_FILE_HANDLE *mirrorHandle = (MIRROR_FILE_HANDLE*)EventInfo->DokanFileInfo->Context;
  WCHAR filePath[MAX_PATH];

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"SetFileSecurity %s\n", filePath);

  MIRROR_HANDLE_ASSERT(mirrorHandle);

  if (!SetUserObjectSecurity(mirrorHandle->FileHandle, EventInfo->SecurityInformation, EventInfo->SecurityDescriptor)) {

    int error = GetLastError();

    DbgPrint(L"  SetUserObjectSecurity error: %d\n", error);

    return DokanNtStatusFromWin32(error);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorGetVolumeInformation(DOKAN_GET_VOLUME_INFO_EVENT *EventInfo) {

  LPCWSTR volumeName = L"DOKAN";
  size_t volumeNameLengthInBytes = wcslen(volumeName) * sizeof(WCHAR);
  size_t maxVolumeNameLengthInBytes = EventInfo->MaxLabelLengthInChars * sizeof(WCHAR);
  size_t bytesToWrite = min(maxVolumeNameLengthInBytes, volumeNameLengthInBytes);

  memcpy_s(
	  EventInfo->VolumeInfo->VolumeLabel,
	  maxVolumeNameLengthInBytes,
	  volumeName,
	  bytesToWrite);


  EventInfo->VolumeInfo->VolumeSerialNumber = 0x19831116;
  EventInfo->VolumeInfo->VolumeLabelLength = (ULONG)(bytesToWrite / sizeof(WCHAR));
  EventInfo->VolumeInfo->SupportsObjects = FALSE;

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorGetVolumeAttributes(DOKAN_GET_VOLUME_ATTRIBUTES_EVENT *EventInfo) {

	LPCWSTR fileSystemName = L"NTFS";
	size_t fileSystemNameLengthInBytes = wcslen(fileSystemName) * sizeof(WCHAR);
	size_t maxFileSystemNameLengthInBytes = EventInfo->MaxFileSystemNameLengthInChars * sizeof(WCHAR);
	size_t bytesToWrite = min(maxFileSystemNameLengthInBytes, fileSystemNameLengthInBytes);
	
	EventInfo->Attributes->FileSystemAttributes =
		FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
		FILE_SUPPORTS_REMOTE_STORAGE | FILE_UNICODE_ON_DISK |
		FILE_PERSISTENT_ACLS;

	EventInfo->Attributes->MaximumComponentNameLength = 256;

	// File system name could be anything up to 10 characters.
	// But Windows check few feature availability based on file system name.
	// For this, it is recommended to set NTFS or FAT here.
	memcpy_s(
		EventInfo->Attributes->FileSystemName,
		maxFileSystemNameLengthInBytes,
		fileSystemName,
		bytesToWrite);

	EventInfo->Attributes->FileSystemNameLength = (ULONG)(bytesToWrite / sizeof(WCHAR));

	return STATUS_SUCCESS;
}


//Uncomment for personalize disk space
static NTSTATUS DOKAN_CALLBACK MirrorDokanGetDiskFreeSpace(DOKAN_GET_DISK_FREE_SPACE_EVENT *EventInfo) {

	if(!GetDiskFreeSpaceExW(
		RootDirectory,
		(ULARGE_INTEGER*)&EventInfo->FreeBytesAvailable,
		(ULARGE_INTEGER*)&EventInfo->TotalNumberOfBytes,
		(ULARGE_INTEGER*)&EventInfo->TotalNumberOfFreeBytes))
	{
		int error = GetLastError();
		DbgPrint(L"  GetDiskFreeSpaceExW error: %d\n", error);

		return DokanNtStatusFromWin32(error);
	}
	
	return STATUS_SUCCESS;
}


/**
 * Avoid #include <winternl.h> which as conflict with FILE_INFORMATION_CLASS
 * definition.
 * This only for MirrorFindStreams. Link with ntdll.lib still required.
 *
 * Not needed if you're not using NtQueryInformationFile!
 *
 * BEGIN
 */
#pragma warning(push)
#pragma warning(disable : 4201)
typedef struct _IO_STATUS_BLOCK {
  union {
    NTSTATUS Status;
    PVOID Pointer;
  } DUMMYUNIONNAME;

  ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
#pragma warning(pop)

NTSYSCALLAPI NTSTATUS NTAPI NtQueryInformationFile(
    _In_ HANDLE FileHandle, _Out_ PIO_STATUS_BLOCK IoStatusBlock,
    _Out_writes_bytes_(Length) PVOID FileInformation, _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS FileInformationClass);
/**
 * END
 */

NTSTATUS DOKAN_CALLBACK
MirrorFindStreams(DOKAN_FIND_STREAMS_EVENT *EventInfo) {

  WCHAR filePath[MAX_PATH];
  HANDLE hFind;
  WIN32_FIND_STREAM_DATA findData;
  DWORD error;
  int count = 0;

  GetFilePath(filePath, MAX_PATH, EventInfo->FileName);

  DbgPrint(L"FindStreams :%s\n", filePath);

  hFind = FindFirstStreamW(filePath, FindStreamInfoStandard, &findData, 0);

  if (hFind == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    DbgPrint(L"\tinvalid file handle. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  EventInfo->FillFindStreamData(EventInfo, &findData);
  count++;

  while (FindNextStreamW(hFind, &findData) != 0) {
	  EventInfo->FillFindStreamData(EventInfo, &findData);
    count++;
  }

  error = GetLastError();
  FindClose(hFind);

  if (error != ERROR_HANDLE_EOF) {
    DbgPrint(L"\tFindNextStreamW error. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tFindStreams return %d entries in %s\n\n", count, filePath);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorMounted(DOKAN_MOUNTED_INFO *EventInfo) {
  UNREFERENCED_PARAMETER(EventInfo);

  DbgPrint(L"Mounted\n");

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorUnmounted(DOKAN_UNMOUNTED_INFO *EventInfo) {
  UNREFERENCED_PARAMETER(EventInfo);

  DbgPrint(L"Unmounted\n");

  InterlockedExchange(&g_IsUnmounted, TRUE);

#if USE_ASYNC_IO
  CleanupPendingAsyncIO();
#endif

  return STATUS_SUCCESS;
}

#pragma warning(pop)

#if USE_ASYNC_IO

BOOL InitializeAsyncIO() {

	g_ThreadPool = DokanGetThreadPool();

	if(!g_ThreadPool) {

		return FALSE;
	}

	g_ThreadPoolCleanupGroup = CreateThreadpoolCleanupGroup();

	if(!g_ThreadPoolCleanupGroup) {

		return FALSE;
	}

	InitializeThreadpoolEnvironment(&g_ThreadPoolEnvironment);

	SetThreadpoolCallbackPool(&g_ThreadPoolEnvironment, g_ThreadPool);
	SetThreadpoolCallbackCleanupGroup(&g_ThreadPoolEnvironment, g_ThreadPoolCleanupGroup, NULL);

	InitializeCriticalSection(&g_OverlappedPoolCS);
	DokanVector_StackAlloc(&g_OverlappedPool, sizeof(MIRROR_OVERLAPPED*));

	InitializeCriticalSection(&g_ThreadPoolCS);

	return TRUE;
}

void CleanupPendingAsyncIO() {

	EnterCriticalSection(&g_ThreadPoolCS);
	{
		if(g_ThreadPoolCleanupGroup) {

			CloseThreadpoolCleanupGroupMembers(g_ThreadPoolCleanupGroup, FALSE, NULL);
			CloseThreadpoolCleanupGroup(g_ThreadPoolCleanupGroup);
			g_ThreadPoolCleanupGroup = NULL;

			DestroyThreadpoolEnvironment(&g_ThreadPoolEnvironment);
		}
	}
	LeaveCriticalSection(&g_ThreadPoolCS);
}

void CleanupAsyncIO() {

	CleanupPendingAsyncIO();

	EnterCriticalSection(&g_OverlappedPoolCS);
	{
		for(size_t i = 0; i < DokanVector_GetCount(&g_OverlappedPool); ++i) {

			FreeMirrorOverlapped(*(MIRROR_OVERLAPPED**)DokanVector_GetItem(&g_OverlappedPool, i));
		}

		DokanVector_Free(&g_OverlappedPool);
	}
	LeaveCriticalSection(&g_OverlappedPoolCS);

	DeleteCriticalSection(&g_OverlappedPoolCS);
	DeleteCriticalSection(&g_ThreadPoolCS);
}

void CALLBACK MirrorIoCallback(
	_Inout_     PTP_CALLBACK_INSTANCE Instance,
	_Inout_opt_ PVOID                 Context,
	_Inout_opt_ PVOID                 Overlapped,
	_In_        ULONG                 IoResult,
	_In_        ULONG_PTR             NumberOfBytesTransferred,
	_Inout_     PTP_IO                Io
) {

	UNREFERENCED_PARAMETER(Instance);
	UNREFERENCED_PARAMETER(Context);
	UNREFERENCED_PARAMETER(Io);

	MIRROR_OVERLAPPED *overlapped = (MIRROR_OVERLAPPED*)Overlapped;
	DOKAN_READ_FILE_EVENT *readFile = NULL;
	DOKAN_WRITE_FILE_EVENT *writeFile = NULL;

	switch(overlapped->IoType) {

	case MIRROR_IOTYPE_READ:

		readFile = (DOKAN_READ_FILE_EVENT*)overlapped->Context;

		assert(readFile);

		readFile->NumberOfBytesRead = (DWORD)NumberOfBytesTransferred;
		
		DokanEndDispatchRead(readFile, DokanNtStatusFromWin32(IoResult));

		break;

	case MIRROR_IOTYPE_WRITE:

		writeFile = (DOKAN_WRITE_FILE_EVENT*)overlapped->Context;

		assert(writeFile);

		writeFile->NumberOfBytesWritten = (DWORD)NumberOfBytesTransferred;

		DokanEndDispatchWrite(writeFile, DokanNtStatusFromWin32(IoResult));

		break;

	default:
		DbgPrint(L"Unrecognized async IO operation: %d\n", overlapped->IoType);
		break;
	}

	PushMirrorOverlapped(overlapped);
}

#endif

#if MIRROR_IS_DEBUGGING_MEMORY

void* WINAPI MirrorMalloc(size_t size, const char *fileName, int lineNumber) {

	return _malloc_dbg(size, _NORMAL_BLOCK, fileName, lineNumber);
}

void WINAPI MirrorFree(void *userData) {

	_free_dbg(userData, _NORMAL_BLOCK);
}

void* WINAPI MirrorRealloc(void *userData, size_t newSize, const char *fileName, int lineNumber) {

	return _realloc_dbg(userData, newSize, _NORMAL_BLOCK, fileName, lineNumber);
}

#endif

BOOL WINAPI CtrlHandler(DWORD dwCtrlType) {
  switch (dwCtrlType) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    DokanRemoveMountPoint(MountPoint);
    return TRUE;
  default:
    return FALSE;
  }
}

int __cdecl wmain(ULONG argc, PWCHAR argv[]) {

  int status;
  ULONG command;
  DOKAN_OPTIONS dokanOptions;
  DOKAN_OPERATIONS dokanOperations;

#if MIRROR_IS_DEBUGGING_MEMORY
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  if (argc < 3) {
    fprintf(stderr, "mirror.exe\n"
                    "  /r RootDirectory (ex. /r c:\\test)\n"
                    "  /l DriveLetter (ex. /l m)\n"
                    "  /t ThreadCount (ex. /t 5)\n"
                    "  /d (enable debug output)\n"
                    "  /s (use stderr for output)\n"
                    "  /n (use network drive)\n"
                    "  /m (use removable drive)\n"
                    "  /w (write-protect drive)\n"
                    "  /o (use mount manager)\n"
                    "  /c (mount for current session only)\n"
                    "  /u UNC provider name\n"
                    "  /a Allocation unit size (ex. /a 512)\n"
                    "  /k Sector size (ex. /k 512)\n"
                    "  /i (Timeout in Milliseconds ex. /i 30000)\n");
    
    return EXIT_FAILURE;
  }

  g_DebugMode = FALSE;
  g_UseStdErr = FALSE;

  ZeroMemory(&dokanOptions, sizeof(DOKAN_OPTIONS));
  dokanOptions.Version = DOKAN_VERSION;
  dokanOptions.ThreadCount = 0; // use default

  for (command = 1; command < argc; command++) {
    switch (towlower(argv[command][1])) {
    case L'r':
      command++;
      wcscpy_s(RootDirectory, sizeof(RootDirectory) / sizeof(WCHAR),
               argv[command]);
      DbgPrint(L"RootDirectory: %ls\n", RootDirectory);
      break;
    case L'l':
      command++;
      wcscpy_s(MountPoint, sizeof(MountPoint) / sizeof(WCHAR), argv[command]);
      dokanOptions.MountPoint = MountPoint;
      break;
    case L't':
      command++;
      dokanOptions.ThreadCount = (USHORT)_wtoi(argv[command]);
      break;
    case L'd':
      g_DebugMode = TRUE;
      break;
    case L's':
      g_UseStdErr = TRUE;
      break;
    case L'n':
      dokanOptions.Options |= DOKAN_OPTION_NETWORK;
      break;
    case L'm':
      dokanOptions.Options |= DOKAN_OPTION_REMOVABLE;
      break;
    case L'w':
      dokanOptions.Options |= DOKAN_OPTION_WRITE_PROTECT;
      break;
    case L'o':
      dokanOptions.Options |= DOKAN_OPTION_MOUNT_MANAGER;
      break;
    case L'c':
      dokanOptions.Options |= DOKAN_OPTION_CURRENT_SESSION;
      break;
    case L'u':
      command++;
      wcscpy_s(UNCName, sizeof(UNCName) / sizeof(WCHAR), argv[command]);
      dokanOptions.UNCName = UNCName;
      DbgPrint(L"UNC Name: %ls\n", UNCName);
      break;
    case L'i':
      command++;
      dokanOptions.Timeout = (ULONG)_wtol(argv[command]);
      break;
    case L'a':
      command++;
      dokanOptions.AllocationUnitSize = (ULONG)_wtol(argv[command]);
      break;
    case L'k':
      command++;
      dokanOptions.SectorSize = (ULONG)_wtol(argv[command]);
      break;
    default:
      fwprintf(stderr, L"unknown command: %s\n", argv[command]);
      return EXIT_FAILURE;
    }
  }

  if (wcscmp(UNCName, L"") != 0 &&
      !(dokanOptions.Options & DOKAN_OPTION_NETWORK)) {
    fwprintf(
        stderr,
        L"  Warning: UNC provider name should be set on network drive only.\n");
  }

  if (dokanOptions.Options & DOKAN_OPTION_NETWORK &&
      dokanOptions.Options & DOKAN_OPTION_MOUNT_MANAGER) {

    fwprintf(stderr, L"Mount manager cannot be used on network drive.\n");

    return -1;
  }

  if (!(dokanOptions.Options & DOKAN_OPTION_MOUNT_MANAGER) &&
      wcscmp(MountPoint, L"") == 0) {

    fwprintf(stderr, L"Mount Point required.\n");
    return -1;
  }

  if ((dokanOptions.Options & DOKAN_OPTION_MOUNT_MANAGER) &&
      (dokanOptions.Options & DOKAN_OPTION_CURRENT_SESSION)) {

    fwprintf(stderr,
             L"Mount Manager always mount the drive for all user sessions.\n");
    return -1;
  }

  if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {

    fwprintf(stderr, L"Control Handler is not set.\n");
  }

  // Add security name privilege. Required here to handle GetFileSecurity
  // properly.
  if (!AddSeSecurityNamePrivilege()) {

    fwprintf(stderr, L"Failed to add security privilege to process\n");
    fwprintf(stderr,
             L"\t=> GetFileSecurity/SetFileSecurity may not work properly\n");
    fwprintf(stderr, L"\t=> Please restart mirror sample with administrator "
                     L"rights to fix it\n");
  }

  if (g_DebugMode) {
    dokanOptions.Options |= DOKAN_OPTION_DEBUG;
  }
  if (g_UseStdErr) {
    dokanOptions.Options |= DOKAN_OPTION_STDERR;
  }

  dokanOptions.Options |= DOKAN_OPTION_ALT_STREAM;

  ZeroMemory(&dokanOperations, sizeof(DOKAN_OPERATIONS));
  dokanOperations.ZwCreateFile = MirrorCreateFile;
  dokanOperations.Cleanup = MirrorCleanup;
  dokanOperations.CloseFile = MirrorCloseFile;
  dokanOperations.ReadFile = MirrorReadFile;
  dokanOperations.WriteFile = MirrorWriteFile;
  dokanOperations.FlushFileBuffers = MirrorFlushFileBuffers;
  dokanOperations.GetFileInformation = MirrorGetFileInformation;
  dokanOperations.FindFiles = MirrorFindFiles;
  dokanOperations.FindFilesWithPattern = NULL;
  dokanOperations.SetFileBasicInformation = MirrorSetFileBasicInformation;
  dokanOperations.CanDeleteFile = MirrorCanDeleteFile;
  dokanOperations.MoveFileW = MirrorMoveFile;
  dokanOperations.SetEndOfFile = MirrorSetEndOfFile;
  dokanOperations.SetAllocationSize = MirrorSetAllocationSize;
  dokanOperations.LockFile = MirrorLockFile;
  dokanOperations.UnlockFile = MirrorUnlockFile;
  dokanOperations.GetVolumeFreeSpace = MirrorDokanGetDiskFreeSpace;
  dokanOperations.GetVolumeInformationW = MirrorGetVolumeInformation;
  dokanOperations.GetVolumeAttributes = MirrorGetVolumeAttributes;
  dokanOperations.Mounted = MirrorMounted;
  dokanOperations.Unmounted = MirrorUnmounted;
  dokanOperations.GetFileSecurityW = MirrorGetFileSecurity;
  dokanOperations.SetFileSecurityW = MirrorSetFileSecurity;
  dokanOperations.FindStreams = MirrorFindStreams;

#if MIRROR_IS_DEBUGGING_MEMORY

  DOKAN_MEMORY_CALLBACKS memoryCallbacks;

  memoryCallbacks.Malloc = MirrorMalloc;
  memoryCallbacks.Free = MirrorFree;
  memoryCallbacks.Realloc = MirrorRealloc;

  DokanInit(&memoryCallbacks);

#else

  DokanInit(NULL);

#endif

#if USE_ASYNC_IO
  if(!InitializeAsyncIO()) {

	  fprintf(stderr, "Failed to initialize async IO.\n");
	  return -2;
  }
#endif

  InitializeCriticalSection(&g_FileHandlePoolCS);
  DokanVector_StackAlloc(&g_FileHandlePool, sizeof(MIRROR_FILE_HANDLE*));

  status = DokanMain(&dokanOptions, &dokanOperations);

  EnterCriticalSection(&g_FileHandlePoolCS);
  {
	  for(size_t i = 0; i < DokanVector_GetCount(&g_FileHandlePool); ++i) {

		  FreeMirrorFileHandle(*(MIRROR_FILE_HANDLE**)DokanVector_GetItem(&g_FileHandlePool, i));
	  }

	  DokanVector_Free(&g_FileHandlePool);
  }
  LeaveCriticalSection(&g_FileHandlePoolCS);

  DeleteCriticalSection(&g_FileHandlePoolCS);

#if USE_ASYNC_IO
  CleanupAsyncIO();
#endif

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
  case DOKAN_VERSION_ERROR:
    fprintf(stderr, "Version error\n");
    break;
  default:
    fprintf(stderr, "Unknown error: %d\n", status);
    break;
  }

  DokanShutdown();

  return EXIT_SUCCESS;
}