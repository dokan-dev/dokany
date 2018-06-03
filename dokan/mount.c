/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2018 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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
#include <Dbt.h>
#include <ShlObj.h>
#include <stdio.h>

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

static BOOL DokanServiceCheck(LPCWSTR ServiceName) {
  SC_HANDLE controlHandle;
  SC_HANDLE serviceHandle;

  controlHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);

  if (controlHandle == NULL) {
    DbgPrint("DokanServiceCheck: Failed to open Service Control Manager. error "
             "= %d\n",
             GetLastError());
    return FALSE;
  }

  serviceHandle =
      OpenService(controlHandle, ServiceName,
                  SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);

  if (serviceHandle == NULL) {
    DokanDbgPrintW(
        L"DokanServiceCheck: Failed to open Service (%s). error = %d\n",
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

  if (DokanServiceControl(ServiceName, DOKAN_SERVICE_START)) {
    DokanDbgPrintW(L"DokanServiceInstall: Service (%s) started\n", ServiceName);
    return TRUE;
  } else {
    DokanDbgPrintW(L"DokanServiceInstall: Service (%s) start failed\n",
                   ServiceName);
    return FALSE;
  }
}

BOOL DOKANAPI DokanServiceDelete(LPCWSTR ServiceName) {
  if (DokanServiceCheck(ServiceName)) {
    DokanServiceControl(ServiceName, DOKAN_SERVICE_STOP);
    if (DokanServiceControl(ServiceName, DOKAN_SERVICE_DELETE)) {
      return TRUE;
    } else {
      return FALSE;
    }
  }
  return TRUE;
}

BOOL DOKANAPI DokanUnmount(WCHAR DriveLetter) {
  WCHAR mountPoint[] = L"M:";
  mountPoint[0] = DriveLetter;
  return DokanRemoveMountPoint(mountPoint);
}

#define DOKAN_NP_SERVICE_KEY                                                   \
  L"System\\CurrentControlSet\\Services\\dokan" DOKAN_MAJOR_API_VERSION
#define DOKAN_NP_DEVICE_NAME                                                   \
  L"\\Device\\DokanRedirector" DOKAN_MAJOR_API_VERSION
#define DOKAN_NP_NAME L"Dokan" DOKAN_MAJOR_API_VERSION
#define DOKAN_NP_PATH L"System32\\dokannp" DOKAN_MAJOR_API_VERSION L".dll"
#define DOKAN_BINARY_NAME L"dokannp" DOKAN_MAJOR_API_VERSION L".dll"
#define DOKAN_NP_ORDER_KEY                                                     \
  L"System\\CurrentControlSet\\Control\\NetworkProvider\\Order"

BOOL DOKANAPI DokanNetworkProviderInstall() {
  HKEY key;
  DWORD position;
  DWORD type;
  WCHAR commanp[64];
  WCHAR buffer[1024];
  DWORD buffer_size = sizeof(buffer);

  ZeroMemory(&buffer, sizeof(buffer));
  ZeroMemory(commanp, sizeof(commanp));

  RegCreateKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_SERVICE_KEY L"\\NetworkProvider",
                 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key,
                 &position);

  RegSetValueEx(key, L"DeviceName", 0, REG_SZ, (BYTE *)DOKAN_NP_DEVICE_NAME,
                (DWORD)(wcslen(DOKAN_NP_DEVICE_NAME) + 1) * sizeof(WCHAR));

  RegSetValueEx(key, L"Name", 0, REG_SZ, (BYTE *)DOKAN_NP_NAME,
                (DWORD)(wcslen(DOKAN_NP_NAME) + 1) * sizeof(WCHAR));

  RegSetValueEx(key, L"ProviderPath", 0, REG_SZ, (BYTE *)DOKAN_NP_PATH,
                (DWORD)(wcslen(DOKAN_NP_PATH) + 1) * sizeof(WCHAR));

  RegCloseKey(key);

  RegOpenKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_ORDER_KEY, 0, KEY_ALL_ACCESS, &key);

  RegQueryValueEx(key, L"ProviderOrder", 0, &type, (BYTE *)&buffer,
                  &buffer_size);

  wcscat_s(commanp, sizeof(commanp) / sizeof(WCHAR), L",");
  wcscat_s(commanp, sizeof(commanp) / sizeof(WCHAR), DOKAN_NP_NAME);

  if (wcsstr(buffer, commanp) == NULL) {
    wcscat_s(buffer, sizeof(buffer) / sizeof(WCHAR), commanp);
    RegSetValueEx(key, L"ProviderOrder", 0, REG_SZ, (BYTE *)&buffer,
                  (DWORD)(wcslen(buffer) + 1) * sizeof(WCHAR));
  }

  RegCloseKey(key);
  return TRUE;
}

BOOL DOKANAPI DokanNetworkProviderUninstall() {
  HKEY key;
  DWORD type;
  WCHAR commanp[64];
  WCHAR buffer[1024];
  WCHAR buffer2[1024];

  DWORD buffer_size = sizeof(buffer);
  ZeroMemory(&buffer, sizeof(buffer));
  ZeroMemory(&buffer2, sizeof(buffer));
  ZeroMemory(commanp, sizeof(commanp));

  RegOpenKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_SERVICE_KEY, 0, KEY_ALL_ACCESS,
               &key);
  RegDeleteKey(key, L"NetworkProvider");

  RegCloseKey(key);

  RegOpenKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_ORDER_KEY, 0, KEY_ALL_ACCESS, &key);

  RegQueryValueEx(key, L"ProviderOrder", 0, &type, (BYTE *)&buffer,
                  &buffer_size);

  wcscat_s(commanp, sizeof(commanp) / sizeof(WCHAR), L",");
  wcscat_s(commanp, sizeof(commanp) / sizeof(WCHAR), DOKAN_NP_NAME);

  WCHAR *dokan_pos = wcsstr(buffer, commanp);
  if (dokan_pos == NULL)
    return TRUE;
  wcsncpy_s(buffer2, sizeof(buffer2) / sizeof(WCHAR), buffer,
            dokan_pos - buffer);
  wcscat_s(buffer2, sizeof(buffer2) / sizeof(WCHAR),
           dokan_pos + wcslen(commanp));
  RegSetValueEx(key, L"ProviderOrder", 0, REG_SZ, (BYTE *)&buffer2,
                (DWORD)(wcslen(buffer2) + 1) * sizeof(WCHAR));

  RegCloseKey(key);

  return TRUE;
}

BOOL CreateMountPoint(LPCWSTR MountPoint, LPCWSTR DeviceName) {
  HANDLE handle;
  PREPARSE_DATA_BUFFER reparseData;
  USHORT bufferLength;
  USHORT targetLength;
  BOOL result;
  ULONG resultLength;
  WCHAR targetDeviceName[MAX_PATH];
  WCHAR errorMsg[256];

  ZeroMemory(targetDeviceName, sizeof(targetDeviceName));
  wcscat_s(targetDeviceName, MAX_PATH, L"\\??");
  wcscat_s(targetDeviceName, MAX_PATH, DeviceName);
  wcscat_s(targetDeviceName, MAX_PATH, L"\\");

  handle = CreateFile(MountPoint, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                      NULL);

  if (handle == INVALID_HANDLE_VALUE) {
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorMsg, 256,
                  NULL);
    DbgPrintW(L"Use %s as mount point failed: (%d) %s", MountPoint,
              GetLastError(), errorMsg);
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

  CloseHandle(handle);
  free(reparseData);

  if (result) {
    DbgPrintW(L"CreateMountPoint %s -> %s success\n", MountPoint,
              targetDeviceName);
  } else {
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorMsg, 256,
                  NULL);
    DbgPrintW(L"CreateMountPoint %s -> %s failed: (%d) %s", MountPoint,
              targetDeviceName, GetLastError(), errorMsg);
  }
  return result;
}

BOOL DeleteMountPoint(LPCWSTR MountPoint) {
  HANDLE handle;
  BOOL result;
  ULONG resultLength;
  REPARSE_GUID_DATA_BUFFER reparseData = {0};
  WCHAR errorMsg[256];

  handle = CreateFile(MountPoint, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                      NULL);

  if (handle == INVALID_HANDLE_VALUE) {
    DbgPrintW(L"CreateFile failed: %s (%d)\n", MountPoint, GetLastError());
    return FALSE;
  }

  reparseData.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;

  result = DeviceIoControl(handle, FSCTL_DELETE_REPARSE_POINT, &reparseData,
                           REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, NULL, 0,
                           &resultLength, NULL);

  CloseHandle(handle);

  if (result) {
    DbgPrintW(L"DeleteMountPoint %s success\n", MountPoint);
  } else {
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorMsg, 256,
                  NULL);
    DbgPrintW(L"DeleteMountPoint %s failed: (%d) %s", MountPoint,
              GetLastError(), errorMsg);
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

void DokanBroadcastLink(WCHAR cLetter, BOOL bRemoved, BOOL safe) {
  DWORD receipients;
  DWORD device_event;
  DEV_BROADCAST_VOLUME params;
  WCHAR drive[4] = L"C:\\";
  LONG wEventId;

  if (!isalpha(cLetter)) {
    DbgPrint("DokanBroadcastLink: invalid parameter\n");
    return;
  }

  receipients = BSM_APPLICATIONS;
  // Unsafe to call Advapi32.dll during DLL_PROCESS_DETACH
  if (safe && EnableTokenPrivilege(SE_TCB_NAME, TRUE)) {
    receipients |= BSM_ALLDESKTOPS;
  }

  device_event = bRemoved ? DBT_DEVICEREMOVECOMPLETE : DBT_DEVICEARRIVAL;

  ZeroMemory(&params, sizeof(params));
  params.dbcv_size = sizeof(params);
  params.dbcv_devicetype = DBT_DEVTYP_VOLUME;
  params.dbcv_reserved = 0;
  params.dbcv_unitmask = (1 << (toupper(cLetter) - 'A'));
  params.dbcv_flags = 0;

  if (BroadcastSystemMessage(
          BSF_NOHANG | BSF_FORCEIFHUNG | BSF_NOTIMEOUTIFNOTHUNG, &receipients,
          WM_DEVICECHANGE, device_event, (LPARAM)&params) <= 0) {

    DbgPrint("DokanBroadcastLink: BroadcastSystemMessage failed - %d\n",
             GetLastError());
  }

  // Unsafe to call ole32.dll during DLL_PROCESS_DETACH
  if (safe) {
    drive[0] = towupper(cLetter);
    wEventId = bRemoved ? SHCNE_DRIVEREMOVED : SHCNE_DRIVEADD;
    SHChangeNotify(wEventId, SHCNF_PATH, drive, NULL);
  }
}

BOOL DokanMount(LPCWSTR MountPoint, LPCWSTR DeviceName,
                PDOKAN_OPTIONS DokanOptions) {
  UNREFERENCED_PARAMETER(DokanOptions);
  if (MountPoint != NULL) {
    if (!IsMountPointDriveLetter(MountPoint)) {
      // Unfortunately mount manager is not working as excepted and don't
      // support mount folder on associated IOCTL, which breaks dokan (ghost
      // drive, ...)
      // In that case we cannot use mount manager ; doesn't this should be done
      // kernel-mode too?
      return CreateMountPoint(MountPoint, DeviceName);
    } else {
      // Notify applications / explorer
      DokanBroadcastLink(MountPoint[0], FALSE, TRUE);
    }
  }
  return TRUE;
}

BOOL DOKANAPI DokanRemoveMountPointEx(LPCWSTR MountPoint, BOOL Safe) {
  if (MountPoint != NULL) {
    size_t length = wcslen(MountPoint);
    if (length > 0) {
      WCHAR mountPoint[MAX_PATH];

      if (IsMountPointDriveLetter(MountPoint)) {
        wcscpy_s(mountPoint, sizeof(mountPoint) / sizeof(WCHAR), L"C:");
        mountPoint[0] = MountPoint[0];
      } else {
        wcscpy_s(mountPoint, sizeof(mountPoint) / sizeof(WCHAR), MountPoint);
        if (mountPoint[length - 1] == L'\\') {
          mountPoint[length - 1] = L'\0';
        }
      }

      if (SendGlobalReleaseIRP(mountPoint)) {
        if (!IsMountPointDriveLetter(MountPoint)) {
          length = wcslen(mountPoint);
          if (length + 1 < MAX_PATH) {
            mountPoint[length] = L'\\';
            mountPoint[length + 1] = L'\0';
            return DeleteMountPoint(mountPoint);
          }
        } else {
          // Notify applications / explorer
          DokanBroadcastLink(MountPoint[0], TRUE, Safe);
          return TRUE;
        }
      }
    }
  }
  return FALSE;
}

BOOL DOKANAPI DokanRemoveMountPoint(LPCWSTR MountPoint) {
  return DokanRemoveMountPointEx(MountPoint, TRUE);
}