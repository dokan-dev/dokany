/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2025 Google, Inc.
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
#include "np_registry.h"
#include <dbt.h>
#include <shlobj.h>
#include <stdio.h>
#include <strsafe.h>

#pragma warning(push)
#pragma warning(disable : 4201)
/**
 * \struct REPARSE_DATA_BUFFER
 * \brief Contains reparse point data for a Microsoft reparse point.
 *
 * Used to create a dokan mount point in CreateMountPoint function.
 */
typedef struct _REPARSE_DATA_BUFFER {
  /**
  * Reparse point tag. Must be a Microsoft reparse point tag.
  */
  ULONG ReparseTag;
  /**
  * Size, in bytes, of the reparse data in the DataBuffer member.
  */
  USHORT ReparseDataLength;
  /**
  * Length, in bytes, of the unparsed portion of the file name pointed
  * to by the FileName member of the associated file object.
  */
  USHORT Reserved;
  union {
    struct {
      /** Offset, in bytes, of the substitute name string in the PathBuffer array. */
      USHORT SubstituteNameOffset;
      /** Length, in bytes, of the substitute name string. */
      USHORT SubstituteNameLength;
      /** Offset, in bytes, of the print name string in the PathBuffer array. */
      USHORT PrintNameOffset;
      /** Length, in bytes, of the print name string. */
      USHORT PrintNameLength;
      /** Used to indicate if the given symbolic link is an absolute or relative symbolic link. */
      ULONG Flags;
      /** First character of the path string. This is followed in memory by the remainder of the string. */
      WCHAR PathBuffer[1];
    } SymbolicLinkReparseBuffer;
    struct {
      /** Offset, in bytes, of the substitute name string in the PathBuffer array. */
      USHORT SubstituteNameOffset;
      /** Length, in bytes, of the substitute name string. */
      USHORT SubstituteNameLength;
      /** Offset, in bytes, of the print name string in the PathBuffer array. */
      USHORT PrintNameOffset;
      /** Length, in bytes, of the print name string. */
      USHORT PrintNameLength;
      /** First character of the path string. */
      WCHAR PathBuffer[1];
    } MountPointReparseBuffer;
    struct {
      /** Microsoft-defined data for the reparse point. */
      UCHAR DataBuffer[1];
    } GenericReparseBuffer;
  } DUMMYUNIONNAME;
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;
#pragma warning(pop)

#define REPARSE_DATA_BUFFER_HEADER_SIZE                                        \
  FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)

static BOOL DokanServiceExists(LPCWSTR ServiceName) {
  SC_HANDLE controlHandle;
  SC_HANDLE serviceHandle;

  controlHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);

  if (controlHandle == NULL) {
    DbgPrint("DokanServiceExists: Failed to open Service Control Manager. error "
             "= %d\n",
             GetLastError());
    return FALSE;
  }

  serviceHandle =
      OpenService(controlHandle, ServiceName,
                  SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);

  if (serviceHandle == NULL) {
    DokanDbgPrintW(
        L"DokanServiceExists: Failed to open Service (%s). error = %d\n",
        ServiceName, GetLastError());
    CloseServiceHandle(controlHandle);
    return FALSE;
  }

  CloseServiceHandle(serviceHandle);
  CloseServiceHandle(controlHandle);

  return TRUE;
}

static BOOL DokanServiceControl(LPCWSTR ServiceName, ULONG Type) {
  SC_HANDLE controlHandle;
  SC_HANDLE serviceHandle;
  SERVICE_STATUS ss;
  BOOL result = TRUE;

  controlHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);

  if (controlHandle == NULL) {
    DokanDbgPrint("DokanServiceControl: Failed to open Service Control "
                  "Manager. error = %d\n",
                  GetLastError());
    return FALSE;
  }

  serviceHandle =
      OpenService(controlHandle, ServiceName,
                  SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);

  if (serviceHandle == NULL) {
    DokanDbgPrintW(
        L"DokanServiceControl: Failed to open Service (%s). error = %d\n",
        ServiceName, GetLastError());
    CloseServiceHandle(controlHandle);
    return FALSE;
  }

  if (QueryServiceStatus(serviceHandle, &ss) != 0) {
    if (Type == DOKAN_SERVICE_DELETE) {
      if (DeleteService(serviceHandle)) {
        DokanDbgPrintW(L"DokanServiceControl: Service (%s) deleted\n",
                       ServiceName);
        result = TRUE;
      } else {
        DokanDbgPrintW(
            L"DokanServiceControl: Failed to delete service (%s). error = %d\n",
            ServiceName, GetLastError());
        result = FALSE;
      }
    } else if (ss.dwCurrentState == SERVICE_STOPPED &&
               Type == DOKAN_SERVICE_START) {
      if (StartService(serviceHandle, 0, NULL)) {
        DokanDbgPrintW(L"DokanServiceControl: Service (%s) started\n",
                       ServiceName);
        result = TRUE;
      } else {
        DokanDbgPrintW(
            L"DokanServiceControl: Failed to start service (%s). error = %d\n",
            ServiceName, GetLastError());
        result = FALSE;
      }
    } else if (ss.dwCurrentState == SERVICE_RUNNING &&
               Type == DOKAN_SERVICE_STOP) {
      if (ControlService(serviceHandle, SERVICE_CONTROL_STOP, &ss)) {
        DokanDbgPrintW(L"DokanServiceControl: Service (%s) stopped\n",
                       ServiceName);
        result = TRUE;
      } else {
        DokanDbgPrintW(
            L"DokanServiceControl: Failed to stop service (%s). error = %d\n",
            ServiceName, GetLastError());
        result = FALSE;
      }
    }
  } else {
    DokanDbgPrintW(
        L"DokanServiceControl: QueryServiceStatus Failed (%s). error = %d\n",
        ServiceName, GetLastError());
    result = FALSE;
  }

  CloseServiceHandle(serviceHandle);
  CloseServiceHandle(controlHandle);

  Sleep(100);
  return result;
}

#define DOKAN_EVENT_LOG_SYSTEM_SERVICE_KEY                                            \
  L"System\\CurrentControlSet\\Services\\EventLog\\System"
#define DOKAN_SYS_PATH                                                         \
  L"%SystemRoot%\\System32\\drivers\\dokan" DOKAN_MAJOR_API_VERSION L".sys"

VOID DokanDriverEventLogInstall() {
  HKEY key;
  DWORD position;
  DWORD type = 0x7;

  if (RegCreateKeyEx(
          HKEY_LOCAL_MACHINE,
          DOKAN_EVENT_LOG_SYSTEM_SERVICE_KEY L"\\dokan" DOKAN_MAJOR_API_VERSION,
          0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key,
          &position) != ERROR_SUCCESS) {
    return;
  }

  RegSetValueEx(key, L"EventMessageFile", 0, REG_SZ, (BYTE *)DOKAN_SYS_PATH,
                (DWORD)(wcslen(DOKAN_SYS_PATH) + 1) * sizeof(WCHAR));

  RegSetValueEx(key, L"TypesSupported", 0, REG_DWORD, (BYTE *)&type,
                sizeof(type));

  RegCloseKey(key);
  return;
}

VOID DokanDriverEventLogUninstall() {
  HKEY key;
  if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, DOKAN_EVENT_LOG_SYSTEM_SERVICE_KEY, 0,
                    KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) {
    return;
  }
  RegDeleteKey(key, L"dokan" DOKAN_MAJOR_API_VERSION);
  RegCloseKey(key);
}

BOOL DOKANAPI DokanServiceInstall(LPCWSTR ServiceName, DWORD ServiceType,
                                  LPCWSTR ServiceFullPath) {
  SC_HANDLE controlHandle;
  SC_HANDLE serviceHandle;

  controlHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
  if (controlHandle == NULL) {
    DokanDbgPrint("DokanServiceInstall: Failed to open Service Control "
                  "Manager. error = %d\n",
                  GetLastError());
    return FALSE;
  }

  serviceHandle =
      CreateService(controlHandle, ServiceName, ServiceName, 0, ServiceType,
                    SERVICE_AUTO_START, SERVICE_ERROR_IGNORE, ServiceFullPath,
                    NULL, NULL, NULL, NULL, NULL);

  if (serviceHandle == NULL) {
    BOOL error = GetLastError();
    if (error == ERROR_SERVICE_EXISTS) {
      DokanDbgPrintW(
          L"DokanServiceInstall: Service (%s) is already installed\n",
          ServiceName);
    } else {
      DokanDbgPrintW(
          L"DokanServiceInstall: Failed to install service (%s). error = %d\n",
          ServiceName, error);
    }
    CloseServiceHandle(controlHandle);
    return FALSE;
  }

  CloseServiceHandle(serviceHandle);
  CloseServiceHandle(controlHandle);

  DokanDbgPrintW(L"DokanServiceInstall: Service (%s) installed\n", ServiceName);

  if (!DokanServiceControl(ServiceName, DOKAN_SERVICE_START)) {
    DokanDbgPrintW(L"DokanServiceInstall: Service (%s) start failed\n",
                   ServiceName);
    return FALSE;
  }
  DokanDbgPrintW(L"DokanServiceInstall: Service (%s) started\n", ServiceName);

  DokanDriverEventLogInstall();
  return TRUE;
}

BOOL DOKANAPI DokanServiceDelete(LPCWSTR ServiceName) {
  if (!DokanServiceExists(ServiceName)) {
    return TRUE;
  }
  DokanDriverEventLogUninstall();
  DokanServiceControl(ServiceName, DOKAN_SERVICE_STOP);
  if (!DokanServiceControl(ServiceName, DOKAN_SERVICE_DELETE)) {
    return FALSE;
  }
  return TRUE;
}

BOOL DOKANAPI DokanUnmount(WCHAR DriveLetter) {
  WCHAR mountPoint[MAX_PATH] = L"M:";
  mountPoint[0] = DriveLetter;
  return DokanRemoveMountPoint(mountPoint);
}

BOOL DOKANAPI DokanNetworkProviderInstall() {
  DWORD error = DokanNetworkProviderInstallAt(HKEY_LOCAL_MACHINE);
  SetLastError(error);
  return error == ERROR_SUCCESS;
}

BOOL DOKANAPI DokanNetworkProviderUninstall() {
  DWORD error = DokanNetworkProviderUninstallAt(HKEY_LOCAL_MACHINE);
  SetLastError(error);
  return error == ERROR_SUCCESS;
}

static VOID FormatSystemErrorMessage(DWORD ErrorCode, LPWSTR Buffer,
                                     DWORD BufferChars) {
  if (Buffer == NULL || BufferChars == 0) {
    return;
  }
  Buffer[0] = L'\0';
  DWORD len =
      FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     NULL, ErrorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     Buffer, BufferChars, NULL);
  if (len == 0) {
    (void)StringCchCopyW(Buffer, BufferChars, L"Unknown error");
    return;
  }
  while (len > 0 && (Buffer[len - 1] == L'\r' || Buffer[len - 1] == L'\n' ||
                     Buffer[len - 1] == L' ')) {
    Buffer[--len] = L'\0';
  }
}

static LPCWSTR GetMountPointErrorHint(DWORD ErrorCode) {
  switch (ErrorCode) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    return L" (mount point directory does not exist)";
  case ERROR_ACCESS_DENIED:
    return L" (insufficient permissions for mount point directory)";
  case ERROR_DIR_NOT_EMPTY:
    return L" (mount point directory must be empty)";
  case ERROR_INVALID_FUNCTION:
  case ERROR_NOT_SUPPORTED:
  case ERROR_DIRECTORY:
    return L" (mount point must be an existing empty directory on an NTFS "
           L"volume)";
  case ERROR_SHARING_VIOLATION:
    return L" (mount point directory is in use)";
  default:
    return L"";
  }
}

static VOID LogMountPointError(LPCWSTR Operation, LPCWSTR MountPoint,
                               LPCWSTR TargetDeviceName, DWORD ErrorCode) {
  WCHAR errorMsg[256];
  FormatSystemErrorMessage(ErrorCode, errorMsg, ARRAYSIZE(errorMsg));
  LPCWSTR hint = GetMountPointErrorHint(ErrorCode);
  if (TargetDeviceName != NULL) {
    DokanDbgPrintW(L"Dokan Error: %s %s -> %s failed: (%lu) %s%s\n", Operation,
                   MountPoint, TargetDeviceName, ErrorCode, errorMsg, hint);
  } else {
    DokanDbgPrintW(L"Dokan Error: %s %s failed: (%lu) %s%s\n", Operation,
                   MountPoint, ErrorCode, errorMsg, hint);
  }
}

BOOL CreateMountPoint(LPCWSTR MountPoint, LPCWSTR DeviceName) {
  HANDLE handle;
  PREPARSE_DATA_BUFFER reparseData;
  USHORT bufferLength;
  USHORT targetLength;
  BOOL result;
  ULONG resultLength;
  WCHAR targetDeviceName[MAX_PATH];

  ZeroMemory(targetDeviceName, sizeof(targetDeviceName));
  wcscat_s(targetDeviceName, MAX_PATH, L"\\??");
  wcscat_s(targetDeviceName, MAX_PATH, DeviceName);
  wcscat_s(targetDeviceName, MAX_PATH, L"\\");

  handle = CreateFile(MountPoint, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                      NULL);

  if (handle == INVALID_HANDLE_VALUE) {
    DWORD lastError = GetLastError();
    LogMountPointError(L"Use mount point", MountPoint, NULL, lastError);
    return FALSE;
  }

  targetLength = (USHORT)wcslen(targetDeviceName) * sizeof(WCHAR);
  bufferLength =
      FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) +
      targetLength + sizeof(WCHAR) + sizeof(WCHAR);

  reparseData = (PREPARSE_DATA_BUFFER)malloc(bufferLength);
  if (reparseData == NULL) {
    CloseHandle(handle);
    return FALSE;
  }

  ZeroMemory(reparseData, bufferLength);

  reparseData->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  reparseData->ReparseDataLength =
      bufferLength - REPARSE_DATA_BUFFER_HEADER_SIZE;

  reparseData->MountPointReparseBuffer.SubstituteNameOffset = 0;
  reparseData->MountPointReparseBuffer.SubstituteNameLength = targetLength;
  reparseData->MountPointReparseBuffer.PrintNameOffset =
      targetLength + sizeof(WCHAR);
  reparseData->MountPointReparseBuffer.PrintNameLength = 0;

  RtlCopyMemory(reparseData->MountPointReparseBuffer.PathBuffer,
                targetDeviceName, targetLength);

  result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, reparseData,
                           bufferLength, NULL, 0, &resultLength, NULL);
  DWORD lastError = result ? ERROR_SUCCESS : GetLastError();

  CloseHandle(handle);
  free(reparseData);

  if (result) {
    DbgPrintW(L"CreateMountPoint %s -> %s success\n", MountPoint,
              targetDeviceName);
  } else {
    LogMountPointError(L"CreateMountPoint", MountPoint, targetDeviceName,
                       lastError);
  }
  return result;
}

BOOL DeleteMountPoint(LPCWSTR MountPoint) {
  HANDLE handle;
  BOOL result;
  ULONG resultLength;
  REPARSE_GUID_DATA_BUFFER reparseData = {0};

  handle = CreateFile(MountPoint, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                      NULL);

  if (handle == INVALID_HANDLE_VALUE) {
    DWORD lastError = GetLastError();
    WCHAR errorMsg[256];
    FormatSystemErrorMessage(lastError, errorMsg, ARRAYSIZE(errorMsg));
    DbgPrintW(L"DeleteMountPoint CreateFile %s failed: (%lu) %s\n", MountPoint,
              lastError, errorMsg);
    return FALSE;
  }

  reparseData.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;

  result = DeviceIoControl(handle, FSCTL_DELETE_REPARSE_POINT, &reparseData,
                           REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, NULL, 0,
                           &resultLength, NULL);
  DWORD lastError = result ? ERROR_SUCCESS : GetLastError();

  CloseHandle(handle);

  if (result) {
    DbgPrintW(L"DeleteMountPoint %s success\n", MountPoint);
  } else {
    if (lastError == ERROR_NOT_A_REPARSE_POINT) {
      // Not a failure for us as this happen when mount manager
      // is used and reparse point is removed from kernel.
      return TRUE;
    }
    WCHAR errorMsg[256];
    FormatSystemErrorMessage(lastError, errorMsg, ARRAYSIZE(errorMsg));
    DbgPrintW(L"DeleteMountPoint %s failed: (%lu) %s\n", MountPoint, lastError,
              errorMsg);
  }
  return result;
}

BOOL EnableTokenPrivilege(LPCTSTR lpszSystemName, BOOL bEnable) {
  HANDLE hToken = NULL;
  if (OpenProcessToken(GetCurrentProcess(),
                       TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
    TOKEN_PRIVILEGES tp = {0};
    if (LookupPrivilegeValue(NULL, lpszSystemName, &tp.Privileges[0].Luid)) {
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Attributes = (bEnable ? SE_PRIVILEGE_ENABLED : 0);

      if (AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES),
                                (PTOKEN_PRIVILEGES)NULL, NULL)) {
        CloseHandle(hToken);
        return GetLastError() == ERROR_SUCCESS;
      }
    }
  }

  if (hToken) {
    CloseHandle(hToken);
  }
  return FALSE;
}

/**
 * \struct DOKAN_BROADCAST_CONTEXT
 * \brief Parameters passed to DokanBroadcastCallback for async volume
 *        arrival and removal broadcasts.
 */
typedef struct _DOKAN_BROADCAST_CONTEXT {
  /** Uppercase ASCII drive letter ('A'-'Z') for the volume. */
  WCHAR DriveLetter;
  /** Whether the mount is a network volume (sets DBTF_NET). */
  BOOL IsNetwork;
  /** BroadcastSystemMessage recipient bitmask. */
  DWORD Recipients;
  /** TRUE on unmount (DBT_DEVICEREMOVECOMPLETE), FALSE on mount. */
  BOOL IsRemoved;
} DOKAN_BROADCAST_CONTEXT, *PDOKAN_BROADCAST_CONTEXT;

VOID CALLBACK DokanBroadcastCallback(PTP_CALLBACK_INSTANCE Instance,
                                     PVOID Parameter) {
  // Release the library reference taken in DokanBroadcastLink once this
  // callback has returned, so the library cannot be unloaded while the
  // broadcast is still running.
  HMODULE self = NULL;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)DokanBroadcastCallback, &self)) {
    FreeLibraryWhenCallbackReturns(Instance, self);
  }

  PDOKAN_BROADCAST_CONTEXT context = (PDOKAN_BROADCAST_CONTEXT)Parameter;
  if (context == NULL) {
    return;
  }

  WCHAR upperLetter = ToUpperAsciiDriveLetter(context->DriveLetter);
  BOOL isNetwork = context->IsNetwork;
  DWORD recipients = context->Recipients;
  BOOL isRemoved = context->IsRemoved;
  free(context);

  if (!IsValidDriveLetter(upperLetter)) {
    return;
  }

  DWORD device_event = isRemoved ? DBT_DEVICEREMOVECOMPLETE : DBT_DEVICEARRIVAL;
  DEV_BROADCAST_VOLUME params;
  ZeroMemory(&params, sizeof(params));
  params.dbcv_size = sizeof(params);
  params.dbcv_devicetype = DBT_DEVTYP_VOLUME;
  params.dbcv_reserved = 0;
  params.dbcv_unitmask = (1UL << (ULONG)(upperLetter - L'A'));
  params.dbcv_flags = isNetwork ? DBTF_NET : 0;

  if (BroadcastSystemMessage(BSF_NOHANG | BSF_FORCEIFHUNG, &recipients,
                             WM_DEVICECHANGE, device_event,
                             (LPARAM)&params) <= 0) {
    DbgPrint("DokanBroadcastLink: BroadcastSystemMessage failed - %d\n",
             GetLastError());
  }
}

VOID DokanBroadcastLink(PDOKAN_INSTANCE DokanInstance, BOOL bRemoved) {
  DWORD recipients = BSM_APPLICATIONS;
  WCHAR drive[4] = L"C:\\";
  LONG wEventId;
  WCHAR cLetter = DokanInstance->MountPoint[0];

  if (!IsValidDriveLetter(cLetter)) {
    DbgPrint("DokanBroadcastLink: invalid parameter\n");
    return;
  }
  WCHAR upperLetter = ToUpperAsciiDriveLetter(cLetter);

  // Unsafe to call Advapi32.dll during DLL_PROCESS_DETACH
  if (EnableTokenPrivilege(SE_TCB_NAME, TRUE)) {
    recipients |= BSM_ALLDESKTOPS;
  }

  // Async call BroadcastSystemMessage to avoid possible hangs from receiver
  // apps. The broadcast must not run on the instance callback environment (or
  // on any environment whose cleanup group is waited for in
  // DeleteDokanInstance / CleanupPool): BroadcastSystemMessage with
  // BSF_FORCEIFHUNG waits about 5 seconds for each top-level window that is
  // not processing messages, so a single hung window delayed DokanCloseHandle
  // by 5 or 10 seconds. It is submitted to the default pool instead, and the
  // library is pinned with a module reference that the callback releases when
  // it returns.
  PDOKAN_BROADCAST_CONTEXT context =
      (PDOKAN_BROADCAST_CONTEXT)malloc(sizeof(DOKAN_BROADCAST_CONTEXT));
  if (context == NULL) {
    DbgPrintW(L"Dokan Error: Failed to allocate broadcast context.\n");
  } else {
    context->DriveLetter = upperLetter;
    context->IsNetwork =
        DokanInstance->DokanOptions != NULL &&
        (DokanInstance->DokanOptions->Options & DOKAN_OPTION_NETWORK) != 0;
    context->Recipients = recipients;
    context->IsRemoved = bRemoved;

    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCWSTR)DokanBroadcastCallback, &self)) {
      DWORD lastError = GetLastError();
      DbgPrintW(L"Dokan Error: GetModuleHandleEx() has returned error code "
                L"%u.\n",
                lastError);
      free(context);
    } else if (!TrySubmitThreadpoolCallback(DokanBroadcastCallback, context,
                                            NULL)) {
      DWORD lastError = GetLastError();
      DbgPrintW(L"Dokan Error: TrySubmitThreadpoolCallback() has returned "
                L"error code %u.\n",
                lastError);
      FreeLibrary(self);
      free(context);
    }
  }

  drive[0] = upperLetter;
  wEventId = bRemoved ? SHCNE_DRIVEREMOVED : SHCNE_DRIVEADD;
  SHChangeNotify(wEventId, SHCNF_PATH, drive, NULL);
}

BOOL DokanMount(PDOKAN_INSTANCE DokanInstance, PDOKAN_OPTIONS DokanOptions) {
  UNREFERENCED_PARAMETER(DokanOptions);
  if (DokanInstance->MountPoint != NULL) {
    if (!IsMountPointDriveLetter(DokanInstance->MountPoint)) {
      if (DokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) {
        return TRUE; // Kernel has already created the reparse point.
      }
      // Should it not also be moved to kernel ?
      return CreateMountPoint(DokanInstance->MountPoint,
                              DokanInstance->DeviceName);
    } else {
      // Notify applications / explorer
      DokanBroadcastLink(DokanInstance, FALSE);
    }
  }
  return TRUE;
}

void GenerateUnmountPoint(LPCWSTR MountPoint, WCHAR *Result,
                          size_t ResultMaxChars) {
  size_t length = wcslen(MountPoint);
  if (IsMountPointDriveLetter(MountPoint)) {
    wcscpy_s(Result, ResultMaxChars, L"C:");
    Result[0] = MountPoint[0];
  } else {
    wcscpy_s(Result, ResultMaxChars, MountPoint);
    if (length > 0 && Result[length - 1] == L'\\') {
      Result[length - 1] = L'\0';
    }
  }
}

BOOL DOKANAPI DokanRemoveMountPoint(LPCWSTR MountPoint) {
  if (MountPoint != NULL) {
    size_t length = wcslen(MountPoint);
    if (length > 0) {
      WCHAR unmountPoint[MAX_PATH];
      GenerateUnmountPoint(MountPoint, unmountPoint, ARRAYSIZE(unmountPoint));
      return SendGlobalReleaseIRP(unmountPoint) ? TRUE : FALSE;
    }
  }
  return FALSE;
}

VOID DokanNotifyUnmounted(PDOKAN_INSTANCE DokanInstance) {
  WCHAR unmountPoint[MAX_PATH];
  GenerateUnmountPoint(DokanInstance->MountPoint, unmountPoint,
                       ARRAYSIZE(unmountPoint));
  if (!IsMountPointDriveLetter(DokanInstance->MountPoint)) {
    size_t length = wcslen(unmountPoint);
    if (!(DokanInstance->DokanOptions->Options & DOKAN_OPTION_MOUNT_MANAGER) &&
        length + 1 < MAX_PATH) {
      unmountPoint[length] = L'\\';
      unmountPoint[length + 1] = L'\0';
      // Required to remove reparse point (could also be done through
      // FSCTL_DELETE_REPARSE_POINT with DeleteMountPoint function)
      DeleteVolumeMountPoint(unmountPoint);
    }
  } else {
    // Notify applications / explorer
    DokanBroadcastLink(DokanInstance, TRUE);
  }

  if (DokanInstance->DokanOperations->Unmounted) {
    DOKAN_FILE_INFO fileInfo;
    RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));
    fileInfo.DokanOptions = DokanInstance->DokanOptions;
    // Ignore return value
    DokanInstance->DokanOperations->Unmounted(&fileInfo);
  }
}
