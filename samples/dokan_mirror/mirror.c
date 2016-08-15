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

#include "../../dokan/dokan.h"
#include "../../dokan/fileinfo.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <winbase.h>

BOOL g_UseStdErr;
BOOL g_DebugMode;

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
    } else {
      outputString = format;
    }
    if (g_UseStdErr)
      fputws(outputString, stderr);
    else
      OutputDebugStringW(outputString);
    if (buffer)
      _freea(buffer);
    va_end(argp);
    if (g_UseStdErr)
      fflush(stderr);
  }
}

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

static void PrintUserName(PDOKAN_FILE_INFO DokanFileInfo) {
  HANDLE handle;
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
MirrorCreateFile(LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext,
                 ACCESS_MASK DesiredAccess, ULONG FileAttributes,
                 ULONG ShareAccess, ULONG CreateDisposition,
                 ULONG CreateOptions, PDOKAN_FILE_INFO DokanFileInfo) {
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
      SecurityContext->AccessState.SecurityDescriptor;
  securityAttrib.bInheritHandle = FALSE;

  DokanMapKernelToUserCreateFileFlags(
      FileAttributes, CreateOptions, CreateDisposition, &fileAttributesAndFlags,
      &creationDisposition);

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"CreateFile : %s\n", filePath);

  PrintUserName(DokanFileInfo);

  /*
  if (ShareMode == 0 && AccessMode & FILE_WRITE_DATA)
          ShareMode = FILE_SHARE_WRITE;
  else if (ShareMode == 0)
          ShareMode = FILE_SHARE_READ;
  */

  DbgPrint(L"\tShareMode = 0x%x\n", ShareAccess);

  MirrorCheckFlag(ShareAccess, FILE_SHARE_READ);
  MirrorCheckFlag(ShareAccess, FILE_SHARE_WRITE);
  MirrorCheckFlag(ShareAccess, FILE_SHARE_DELETE);

  DbgPrint(L"\tAccessMode = 0x%x\n", DesiredAccess);

  MirrorCheckFlag(DesiredAccess, GENERIC_READ);
  MirrorCheckFlag(DesiredAccess, GENERIC_WRITE);
  MirrorCheckFlag(DesiredAccess, GENERIC_EXECUTE);

  MirrorCheckFlag(DesiredAccess, DELETE);
  MirrorCheckFlag(DesiredAccess, FILE_READ_DATA);
  MirrorCheckFlag(DesiredAccess, FILE_READ_ATTRIBUTES);
  MirrorCheckFlag(DesiredAccess, FILE_READ_EA);
  MirrorCheckFlag(DesiredAccess, READ_CONTROL);
  MirrorCheckFlag(DesiredAccess, FILE_WRITE_DATA);
  MirrorCheckFlag(DesiredAccess, FILE_WRITE_ATTRIBUTES);
  MirrorCheckFlag(DesiredAccess, FILE_WRITE_EA);
  MirrorCheckFlag(DesiredAccess, FILE_APPEND_DATA);
  MirrorCheckFlag(DesiredAccess, WRITE_DAC);
  MirrorCheckFlag(DesiredAccess, WRITE_OWNER);
  MirrorCheckFlag(DesiredAccess, SYNCHRONIZE);
  MirrorCheckFlag(DesiredAccess, FILE_EXECUTE);
  MirrorCheckFlag(DesiredAccess, STANDARD_RIGHTS_READ);
  MirrorCheckFlag(DesiredAccess, STANDARD_RIGHTS_WRITE);
  MirrorCheckFlag(DesiredAccess, STANDARD_RIGHTS_EXECUTE);

  // When filePath is a directory, needs to change the flag so that the file can
  // be opened.
  fileAttr = GetFileAttributes(filePath);

  if (fileAttr != INVALID_FILE_ATTRIBUTES &&
      (fileAttr & FILE_ATTRIBUTE_DIRECTORY) &&
      !(CreateOptions & FILE_NON_DIRECTORY_FILE)) {
    DokanFileInfo->IsDirectory = TRUE;
    if (DesiredAccess & DELETE) {
      // Needed by FindFirstFile to see if directory is empty or not
      ShareAccess |= FILE_SHARE_READ;
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

  if (DokanFileInfo->IsDirectory) {
    // It is a create directory request
    if (creationDisposition == CREATE_NEW) {
      if (!CreateDirectory(filePath, &securityAttrib)) {
        error = GetLastError();
        DbgPrint(L"\terror code = %d\n\n", error);
        status = DokanNtStatusFromWin32(error);
      }
    } else if (creationDisposition == OPEN_ALWAYS) {

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
          filePath, DesiredAccess, ShareAccess, &securityAttrib, OPEN_EXISTING,
          fileAttributesAndFlags | FILE_FLAG_BACKUP_SEMANTICS, NULL);

      if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        DbgPrint(L"\terror code = %d\n\n", error);

        status = DokanNtStatusFromWin32(error);
      } else {
        DokanFileInfo->Context =
            (ULONG64)handle; // save the file handle in Context
      }
    }
  } else {
    // It is a create file request

    if (fileAttr != INVALID_FILE_ATTRIBUTES &&
        (fileAttr & FILE_ATTRIBUTE_DIRECTORY) &&
        CreateDisposition == FILE_CREATE)
      return STATUS_OBJECT_NAME_COLLISION; // File already exist because
                                           // GetFileAttributes found it
    handle =
        CreateFile(filePath,
                   DesiredAccess, // GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
                   ShareAccess,
                   &securityAttrib, // security attribute
                   creationDisposition,
                   fileAttributesAndFlags, // |FILE_FLAG_NO_BUFFERING,
                   NULL);                  // template file handle

    if (handle == INVALID_HANDLE_VALUE) {
      error = GetLastError();
      DbgPrint(L"\terror code = %d\n\n", error);

      status = DokanNtStatusFromWin32(error);
    } else {
      DokanFileInfo->Context =
          (ULONG64)handle; // save the file handle in Context

      if (creationDisposition == OPEN_ALWAYS ||
          creationDisposition == CREATE_ALWAYS) {
        error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) {
          DbgPrint(L"\tOpen an already existing file\n");
          // Open succeed but we need to inform the driver
          // that the file open and not created by returning STATUS_OBJECT_NAME_COLLISION
          return STATUS_OBJECT_NAME_COLLISION;
        }
      }
    }
  }

  DbgPrint(L"\n");
  return status;
}

#pragma warning(push)
#pragma warning(disable : 4305)

static void DOKAN_CALLBACK MirrorCloseFile(LPCWSTR FileName,
                                           PDOKAN_FILE_INFO DokanFileInfo) {
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

static void DOKAN_CALLBACK MirrorCleanup(LPCWSTR FileName,
                                         PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  GetFilePath(filePath, MAX_PATH, FileName);

  if (DokanFileInfo->Context) {
    DbgPrint(L"Cleanup: %s\n\n", filePath);
    CloseHandle((HANDLE)(DokanFileInfo->Context));
    DokanFileInfo->Context = 0;
  } else {
    DbgPrint(L"Cleanup: %s\n\tinvalid handle\n\n", filePath);
  }

  if (DokanFileInfo->DeleteOnClose) {
    // Should already be deleted by CloseHandle
    // if open with FILE_FLAG_DELETE_ON_CLOSE
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
}

static NTSTATUS DOKAN_CALLBACK MirrorReadFile(LPCWSTR FileName, LPVOID Buffer,
                                              DWORD BufferLength,
                                              LPDWORD ReadLength,
                                              LONGLONG Offset,
                                              PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle = (HANDLE)DokanFileInfo->Context;
  ULONG offset = (ULONG)Offset;
  BOOL opened = FALSE;

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"ReadFile : %s\n", filePath);

  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle, cleanuped?\n");
    handle = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      DbgPrint(L"\tCreateFile error : %d\n\n", error);
      return DokanNtStatusFromWin32(error);
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
    return DokanNtStatusFromWin32(error);
  }

  if (!ReadFile(handle, Buffer, BufferLength, ReadLength, NULL)) {
    DWORD error = GetLastError();
    DbgPrint(L"\tread error = %u, buffer length = %d, read length = %d\n\n",
             error, BufferLength, *ReadLength);
    if (opened)
      CloseHandle(handle);
    return DokanNtStatusFromWin32(error);

  } else {
    DbgPrint(L"\tByte to read: %d, Byte read %d, offset %d\n\n", BufferLength,
             *ReadLength, offset);
  }

  if (opened)
    CloseHandle(handle);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorWriteFile(LPCWSTR FileName, LPCVOID Buffer,
                                               DWORD NumberOfBytesToWrite,
                                               LPDWORD NumberOfBytesWritten,
                                               LONGLONG Offset,
                                               PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle = (HANDLE)DokanFileInfo->Context;
  BOOL opened = FALSE;

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"WriteFile : %s, offset %I64d, length %d\n", filePath, Offset,
           NumberOfBytesToWrite);

  // reopen the file
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle, cleanuped?\n");
    handle = CreateFile(filePath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      DbgPrint(L"\tCreateFile error : %d\n\n", error);
      return DokanNtStatusFromWin32(error);
    }
    opened = TRUE;
  }

  UINT64 fileSize = 0;
  DWORD fileSizeLow = 0;
  DWORD fileSizeHigh = 0;
  fileSizeLow = GetFileSize(handle, &fileSizeHigh);
  if (fileSizeLow == INVALID_FILE_SIZE) {
    DWORD error = GetLastError();
    DbgPrint(L"\tcan not get a file size error = %d\n", error);
    if (opened)
      CloseHandle(handle);
    return DokanNtStatusFromWin32(error);
  }

  fileSize = ((UINT64)fileSizeHigh << 32) | fileSizeLow;

  LARGE_INTEGER distanceToMove;
  if (DokanFileInfo->WriteToEndOfFile) {
    LARGE_INTEGER z;
    z.QuadPart = 0;
    if (!SetFilePointerEx(handle, z, NULL, FILE_END)) {
      DWORD error = GetLastError();
      DbgPrint(L"\tseek error, offset = EOF, error = %d\n", error);
      if (opened)
        CloseHandle(handle);
      return DokanNtStatusFromWin32(error);
    }
  } else {
    // Paging IO cannot write after allocate file size.
    if (DokanFileInfo->PagingIo) {
      if ((UINT64)Offset >= fileSize) {
        *NumberOfBytesWritten = 0;
        if (opened)
          CloseHandle(handle);
        return STATUS_SUCCESS;
      }

      if (((UINT64)Offset + NumberOfBytesToWrite) > fileSize) {
        UINT64 bytes = fileSize - Offset;
        if (bytes >> 32) {
          NumberOfBytesToWrite = (DWORD)(bytes & 0xFFFFFFFFUL);
        } else {
          NumberOfBytesToWrite = (DWORD)bytes;
        }
      }
    }

    if ((UINT64)Offset > fileSize) {
      // In the mirror sample helperZeroFileData is not necessary. NTFS will
      // zero a hole.
      // But if user's file system is different from NTFS( or other Windows's
      // file systems ) then  users will have to zero the hole themselves.
    }

    distanceToMove.QuadPart = Offset;
    if (!SetFilePointerEx(handle, distanceToMove, NULL, FILE_BEGIN)) {
      DWORD error = GetLastError();
      DbgPrint(L"\tseek error, offset = %I64d, error = %d\n", Offset, error);
      if (opened)
        CloseHandle(handle);
      return DokanNtStatusFromWin32(error);
    }
  }

  if (!WriteFile(handle, Buffer, NumberOfBytesToWrite, NumberOfBytesWritten,
                 NULL)) {
    DWORD error = GetLastError();
    DbgPrint(L"\twrite error = %u, buffer length = %d, write length = %d\n",
             error, NumberOfBytesToWrite, *NumberOfBytesWritten);
    if (opened)
      CloseHandle(handle);
    return DokanNtStatusFromWin32(error);

  } else {
    DbgPrint(L"\twrite %d, offset %I64d\n\n", *NumberOfBytesWritten, Offset);
  }

  // close the file when it is reopened
  if (opened)
    CloseHandle(handle);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorFlushFileBuffers(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle = (HANDLE)DokanFileInfo->Context;

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
    return DokanNtStatusFromWin32(error);
  }
}

static NTSTATUS DOKAN_CALLBACK MirrorGetFileInformation(
    LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation,
    PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle = (HANDLE)DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"GetFileInfo : %s\n", filePath);

  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_PARAMETER;
  }

  if (!GetFileInformationByHandle(handle, HandleFileInformation)) {
    DbgPrint(L"\terror code = %d\n", GetLastError());

    // FileName is a root directory
    // in this case, FindFirstFile can't get directory information
    if (wcslen(FileName) == 1) {
      DbgPrint(L"  root dir\n");
      HandleFileInformation->dwFileAttributes = GetFileAttributes(filePath);

    } else {
      WIN32_FIND_DATAW find;
      ZeroMemory(&find, sizeof(WIN32_FIND_DATAW));
      HANDLE findHandle = FindFirstFile(filePath, &find);
      if (findHandle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        DbgPrint(L"\tFindFirstFile error code = %d\n\n", error);
        return DokanNtStatusFromWin32(error);
      }
      HandleFileInformation->dwFileAttributes = find.dwFileAttributes;
      HandleFileInformation->ftCreationTime = find.ftCreationTime;
      HandleFileInformation->ftLastAccessTime = find.ftLastAccessTime;
      HandleFileInformation->ftLastWriteTime = find.ftLastWriteTime;
      HandleFileInformation->nFileSizeHigh = find.nFileSizeHigh;
      HandleFileInformation->nFileSizeLow = find.nFileSizeLow;
      DbgPrint(L"\tFindFiles OK, file size = %d\n", find.nFileSizeLow);
      FindClose(findHandle);
    }
  } else {
    DbgPrint(L"\tGetFileInformationByHandle success, file size = %d\n",
             HandleFileInformation->nFileSizeLow);
  }

  DbgPrint(L"\n");

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorFindFiles(LPCWSTR FileName,
                PFillFindData FillFindData, // function pointer
                PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  size_t fileLen;
  HANDLE hFind;
  WIN32_FIND_DATAW findData;
  DWORD error;
  int count = 0;

  GetFilePath(filePath, MAX_PATH, FileName);

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
  BOOLEAN rootFolder = (wcscmp(FileName, L"\\") == 0);
  do {
    if (!rootFolder || (wcscmp(findData.cFileName, L".") != 0 &&
                        wcscmp(findData.cFileName, L"..") != 0))
      FillFindData(&findData, DokanFileInfo);
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

static NTSTATUS DOKAN_CALLBACK
MirrorDeleteFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  WCHAR filePath[MAX_PATH];
  // HANDLE	handle = (HANDLE)DokanFileInfo->Context;

  GetFilePath(filePath, MAX_PATH, FileName);
  DbgPrint(L"DeleteFile %s\n", filePath);

  DWORD dwAttrib = GetFileAttributes(filePath);

  if (dwAttrib != INVALID_FILE_ATTRIBUTES &&
      (dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
    return STATUS_ACCESS_DENIED;

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorDeleteDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  WCHAR filePath[MAX_PATH];
  // HANDLE	handle = (HANDLE)DokanFileInfo->Context;
  HANDLE hFind;
  WIN32_FIND_DATAW findData;
  size_t fileLen;

  ZeroMemory(filePath, sizeof(filePath));
  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"DeleteDirectory %s\n", filePath);

  fileLen = wcslen(filePath);
  if (filePath[fileLen - 1] != L'\\') {
    filePath[fileLen++] = L'\\';
  }
  filePath[fileLen] = L'*';
  filePath[fileLen + 1] = L'\0';

  hFind = FindFirstFile(filePath, &findData);

  if (hFind == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  do {
    if (wcscmp(findData.cFileName, L"..") != 0 &&
        wcscmp(findData.cFileName, L".") != 0) {
      FindClose(hFind);
      DbgPrint(L"\tDirectory is not empty: %s\n", findData.cFileName);
      return STATUS_DIRECTORY_NOT_EMPTY;
    }
  } while (FindNextFile(hFind, &findData) != 0);

  DWORD error = GetLastError();

  if (error != ERROR_NO_MORE_FILES) {
    DbgPrint(L"\tDeleteDirectory error code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  FindClose(hFind);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorMoveFile(LPCWSTR FileName, // existing file name
               LPCWSTR NewFileName, BOOL ReplaceIfExisting,
               PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  WCHAR newFilePath[MAX_PATH];
  HANDLE handle;
  DWORD bufferSize;
  BOOL result;
  size_t newFilePathLen;

  PFILE_RENAME_INFO renameInfo = NULL;

  GetFilePath(filePath, MAX_PATH, FileName);
  GetFilePath(newFilePath, MAX_PATH, NewFileName);

  DbgPrint(L"MoveFile %s -> %s\n\n", filePath, newFilePath);
  handle = (HANDLE)DokanFileInfo->Context;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

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
      ReplaceIfExisting
          ? TRUE
          : FALSE; // some warning about converting BOOL to BOOLEAN
  renameInfo->RootDirectory = NULL; // hope it is never needed, shouldn't be
  renameInfo->FileNameLength =
      (DWORD)newFilePathLen *
      sizeof(newFilePath[0]); // they want length in bytes

  wcscpy_s(renameInfo->FileName, newFilePathLen + 1, newFilePath);

  result = SetFileInformationByHandle(handle, FileRenameInfo, renameInfo,
                                      bufferSize);

  free(renameInfo);

  if (result) {
    return STATUS_SUCCESS;
  } else {
    DWORD error = GetLastError();
    DbgPrint(L"\tMoveFile error = %u\n", error);
    return DokanNtStatusFromWin32(error);
  }
}

static NTSTATUS DOKAN_CALLBACK MirrorLockFile(LPCWSTR FileName,
                                              LONGLONG ByteOffset,
                                              LONGLONG Length,
                                              PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle;
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

  if (!LockFile(handle, offset.LowPart, offset.HighPart, length.LowPart,
                length.HighPart)) {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tsuccess\n\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetEndOfFile(
    LPCWSTR FileName, LONGLONG ByteOffset, PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle;
  LARGE_INTEGER offset;

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
    DbgPrint(L"\tSetFilePointer error: %d, offset = %I64d\n\n", error,
             ByteOffset);
    return DokanNtStatusFromWin32(error);
  }

  if (!SetEndOfFile(handle)) {
    DWORD error = GetLastError();
    DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetAllocationSize(
    LPCWSTR FileName, LONGLONG AllocSize, PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle;
  LARGE_INTEGER fileSize;

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
                 L"offset = %I64d\n\n",
                 error, AllocSize);
        return DokanNtStatusFromWin32(error);
      }
      if (!SetEndOfFile(handle)) {
        DWORD error = GetLastError();
        DbgPrint(L"\tSetEndOfFile error code = %d\n\n", error);
        return DokanNtStatusFromWin32(error);
      }
    }
  } else {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetFileAttributes(
    LPCWSTR FileName, DWORD FileAttributes, PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  WCHAR filePath[MAX_PATH];

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"SetFileAttributes %s\n", filePath);

  if (!SetFileAttributes(filePath, FileAttributes)) {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorSetFileTime(LPCWSTR FileName, CONST FILETIME *CreationTime,
                  CONST FILETIME *LastAccessTime, CONST FILETIME *LastWriteTime,
                  PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle;

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
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
MirrorUnlockFile(LPCWSTR FileName, LONGLONG ByteOffset, LONGLONG Length,
                 PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE handle;
  LARGE_INTEGER length;
  LARGE_INTEGER offset;

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"UnlockFile %s\n", filePath);

  handle = (HANDLE)DokanFileInfo->Context;
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    return STATUS_INVALID_HANDLE;
  }

  length.QuadPart = Length;
  offset.QuadPart = ByteOffset;

  if (!UnlockFile(handle, offset.LowPart, offset.HighPart, length.LowPart,
                  length.HighPart)) {
    DWORD error = GetLastError();
    DbgPrint(L"\terror code = %d\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  DbgPrint(L"\tsuccess\n\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorGetFileSecurity(
    LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG BufferLength,
    PULONG LengthNeeded, PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];

  UNREFERENCED_PARAMETER(DokanFileInfo);

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"GetFileSecurity %s\n", filePath);

  MirrorCheckFlag(*SecurityInformation, FILE_SHARE_READ);
  MirrorCheckFlag(*SecurityInformation, OWNER_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, GROUP_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, DACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, SACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, LABEL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, ATTRIBUTE_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, SCOPE_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation,
                  PROCESS_TRUST_LABEL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, BACKUP_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, PROTECTED_DACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, PROTECTED_SACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, UNPROTECTED_DACL_SECURITY_INFORMATION);
  MirrorCheckFlag(*SecurityInformation, UNPROTECTED_SACL_SECURITY_INFORMATION);

  DbgPrint(L"  Opening new handle with READ_CONTROL access\n");
  HANDLE handle = CreateFile(
      filePath,
      READ_CONTROL | (((*SecurityInformation & SACL_SECURITY_INFORMATION) ||
                       (*SecurityInformation & BACKUP_SECURITY_INFORMATION))
                          ? ACCESS_SYSTEM_SECURITY
                          : 0),
      FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE,
      NULL, // security attribute
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, // |FILE_FLAG_NO_BUFFERING,
      NULL);

  if (!handle || handle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"\tinvalid handle\n\n");
    int error = GetLastError();
    return DokanNtStatusFromWin32(error);
  }

  if (!GetUserObjectSecurity(handle, SecurityInformation, SecurityDescriptor,
                             BufferLength, LengthNeeded)) {
    int error = GetLastError();
    if (error == ERROR_INSUFFICIENT_BUFFER) {
      DbgPrint(L"  GetUserObjectSecurity error: ERROR_INSUFFICIENT_BUFFER\n");
      CloseHandle(handle);
      return STATUS_BUFFER_OVERFLOW;
    } else {
      DbgPrint(L"  GetUserObjectSecurity error: %d\n", error);
      CloseHandle(handle);
      return DokanNtStatusFromWin32(error);
    }
  }
  CloseHandle(handle);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorSetFileSecurity(
    LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG SecurityDescriptorLength,
    PDOKAN_FILE_INFO DokanFileInfo) {
  HANDLE handle;
  WCHAR filePath[MAX_PATH];

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
    DbgPrint(L"  SetUserObjectSecurity error: %d\n", error);
    return DokanNtStatusFromWin32(error);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorGetVolumeInformation(
    LPWSTR VolumeNameBuffer, DWORD VolumeNameSize, LPDWORD VolumeSerialNumber,
    LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
    LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize,
    PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  wcscpy_s(VolumeNameBuffer, VolumeNameSize, L"DOKAN");
  *VolumeSerialNumber = 0x19831116;
  *MaximumComponentLength = 256;
  *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
                     FILE_SUPPORTS_REMOTE_STORAGE | FILE_UNICODE_ON_DISK |
                     FILE_PERSISTENT_ACLS;

  // File system name could be anything up to 10 characters.
  // But Windows check few feature availability based on file system name.
  // For this, it is recommended to set NTFS or FAT here.
  wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"NTFS");

  return STATUS_SUCCESS;
}

/*
//Uncomment for personalize disk space
static NTSTATUS DOKAN_CALLBACK MirrorDokanGetDiskFreeSpace(
    PULONGLONG FreeBytesAvailable, PULONGLONG TotalNumberOfBytes,
    PULONGLONG TotalNumberOfFreeBytes, PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  *FreeBytesAvailable = (ULONGLONG)(512 * 1024 * 1024);
  *TotalNumberOfBytes = 9223372036854775807;
  *TotalNumberOfFreeBytes = 9223372036854775807;

  return STATUS_SUCCESS;
}
*/

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
MirrorFindStreams(LPCWSTR FileName, PFillFindStreamData FillFindStreamData,
                  PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR filePath[MAX_PATH];
  HANDLE hFind;
  WIN32_FIND_STREAM_DATA findData;
  DWORD error;
  int count = 0;

  GetFilePath(filePath, MAX_PATH, FileName);

  DbgPrint(L"FindStreams :%s\n", filePath);

  hFind = FindFirstStreamW(filePath, FindStreamInfoStandard, &findData, 0);

  if (hFind == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    DbgPrint(L"\tinvalid file handle. Error is %u\n\n", error);
    return DokanNtStatusFromWin32(error);
  }

  FillFindStreamData(&findData, DokanFileInfo);
  count++;

  while (FindNextStreamW(hFind, &findData) != 0) {
    FillFindStreamData(&findData, DokanFileInfo);
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

static NTSTATUS DOKAN_CALLBACK MirrorMounted(PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  DbgPrint(L"Mounted\n");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MirrorUnmounted(PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  DbgPrint(L"Unmounted\n");
  return STATUS_SUCCESS;
}

#pragma warning(pop)

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
  PDOKAN_OPERATIONS dokanOperations =
      (PDOKAN_OPERATIONS)malloc(sizeof(DOKAN_OPERATIONS));
  if (dokanOperations == NULL) {
    return EXIT_FAILURE;
  }
  PDOKAN_OPTIONS dokanOptions = (PDOKAN_OPTIONS)malloc(sizeof(DOKAN_OPTIONS));
  if (dokanOptions == NULL) {
    free(dokanOperations);
    return EXIT_FAILURE;
  }

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
    free(dokanOperations);
    free(dokanOptions);
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
      wcscpy_s(RootDirectory, sizeof(RootDirectory) / sizeof(WCHAR),
               argv[command]);
      DbgPrint(L"RootDirectory: %ls\n", RootDirectory);
      break;
    case L'l':
      command++;
      wcscpy_s(MountPoint, sizeof(MountPoint) / sizeof(WCHAR), argv[command]);
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
    case L'w':
      dokanOptions->Options |= DOKAN_OPTION_WRITE_PROTECT;
      break;
    case L'o':
      dokanOptions->Options |= DOKAN_OPTION_MOUNT_MANAGER;
      break;
    case L'c':
      dokanOptions->Options |= DOKAN_OPTION_CURRENT_SESSION;
      break;
    case L'u':
      command++;
      wcscpy_s(UNCName, sizeof(UNCName) / sizeof(WCHAR), argv[command]);
      dokanOptions->UNCName = UNCName;
      DbgPrint(L"UNC Name: %ls\n", UNCName);
      break;
    case L'i':
      command++;
      dokanOptions->Timeout = (ULONG)_wtol(argv[command]);
      break;
    case L'a':
      command++;
      dokanOptions->AllocationUnitSize = (ULONG)_wtol(argv[command]);
      break;
    case L'k':
      command++;
      dokanOptions->SectorSize = (ULONG)_wtol(argv[command]);
      break;
    default:
      fwprintf(stderr, L"unknown command: %s\n", argv[command]);
      free(dokanOperations);
      free(dokanOptions);
      return EXIT_FAILURE;
    }
  }

  if (wcscmp(UNCName, L"") != 0 &&
      !(dokanOptions->Options & DOKAN_OPTION_NETWORK)) {
    fwprintf(
        stderr,
        L"  Warning: UNC provider name should be set on network drive only.\n");
  }

  if (dokanOptions->Options & DOKAN_OPTION_NETWORK &&
      dokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) {
    fwprintf(stderr, L"Mount manager cannot be used on network drive.\n");
    free(dokanOperations);
    free(dokanOptions);
    return EXIT_FAILURE;
  }

  if (!(dokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) &&
      wcscmp(MountPoint, L"") == 0) {
    fwprintf(stderr, L"Mount Point required.\n");
    free(dokanOperations);
    free(dokanOptions);
    return EXIT_FAILURE;
  }

  if ((dokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) &&
      (dokanOptions->Options & DOKAN_OPTION_CURRENT_SESSION)) {
    fwprintf(stderr,
             L"Mount Manager always mount the drive for all user sessions.\n");
    free(dokanOperations);
    free(dokanOptions);
    return EXIT_FAILURE;
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
    dokanOptions->Options |= DOKAN_OPTION_DEBUG;
  }
  if (g_UseStdErr) {
    dokanOptions->Options |= DOKAN_OPTION_STDERR;
  }

  dokanOptions->Options |= DOKAN_OPTION_ALT_STREAM;

  ZeroMemory(dokanOperations, sizeof(DOKAN_OPERATIONS));
  dokanOperations->ZwCreateFile = MirrorCreateFile;
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
  dokanOperations->GetDiskFreeSpace = NULL; // MirrorDokanGetDiskFreeSpace;
  dokanOperations->GetVolumeInformation = MirrorGetVolumeInformation;
  dokanOperations->Unmounted = MirrorUnmounted;
  dokanOperations->FindStreams = MirrorFindStreams;
  dokanOperations->Mounted = MirrorMounted;

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
  case DOKAN_VERSION_ERROR:
    fprintf(stderr, "Version error\n");
    break;
  default:
    fprintf(stderr, "Unknown error: %d\n", status);
    break;
  }

  free(dokanOptions);
  free(dokanOperations);
  return EXIT_SUCCESS;
}
