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

#include "np_transport_live.h"

#include <winioctl.h>
#include <winnetwk.h>

#include "np_util.h"

// Same device as DOKAN_GLOBAL_DEVICE_NAME in dokan/dokanc.h.
#define DOKAN_NP_GLOBAL_DEVICE_NAME L"\\\\.\\Dokan_" DOKAN_MAJOR_API_VERSION

// Prefix of every MountPoint reported by the driver.
#define DOKAN_NP_DOS_DEVICES_PREFIX L"\\DosDevices\\"
#define DOKAN_NP_DOS_DEVICES_PREFIX_LENGTH                                     \
  (ARRAYSIZE(DOKAN_NP_DOS_DEVICES_PREFIX) - 1)

// Initial number of entries requested from the driver (grows up to
// DOKAN_NP_MAX_ENTRIES on ERROR_MORE_DATA).
#define DOKAN_NP_INITIAL_LIST_ENTRIES 32

// SessionId value reported by the driver for global (all-session) mounts.
#define DOKAN_NP_GLOBAL_SESSION 0xFFFFFFFFUL

// Session id matching no session, used when the caller's session is unknown:
// only global mounts stay visible.
#define DOKAN_NP_NO_SESSION 0xFFFFFFFEUL

static BOOL NpHasDosDevicesPrefix(const WCHAR *Text, SIZE_T Length) {
  return Length >= DOKAN_NP_DOS_DEVICES_PREFIX_LENGTH &&
         CompareStringOrdinal(Text, (int)DOKAN_NP_DOS_DEVICES_PREFIX_LENGTH,
                              DOKAN_NP_DOS_DEVICES_PREFIX,
                              (int)DOKAN_NP_DOS_DEVICES_PREFIX_LENGTH,
                              /*bIgnoreCase=*/TRUE) == CSTR_EQUAL;
}

DWORD DokanNpTableFromMountPointInfo(const DOKAN_MOUNT_POINT_INFO *Entries,
                                     ULONG EntryCount, ULONG CurrentSessionId,
                                     DOKAN_NP_MOUNT_TABLE **Table) {
  if (Table == NULL) {
    return ERROR_INVALID_PARAMETER;
  }
  *Table = NULL;
  if (Entries == NULL && EntryCount != 0) {
    return ERROR_INVALID_PARAMETER;
  }

  DOKAN_NP_TABLE_BUILDER *builder = NULL;
  DWORD error = DokanNpTableBuilderCreate(&builder);
  if (error != ERROR_SUCCESS) {
    return error;
  }

  for (ULONG i = 0; i < EntryCount; ++i) {
    const DOKAN_MOUNT_POINT_INFO *entry = &Entries[i];
    // For elevated/admin callers FSCTL_EVENT_MOUNTPOINT_LIST returns entries
    // from all sessions; keep only mounts visible in the caller's session.
    if (entry->SessionId != DOKAN_NP_GLOBAL_SESSION &&
        entry->SessionId != CurrentSessionId) {
      continue;
    }
    // Folder mounts may clear DOKAN_OPTION_NETWORK while retaining UNCName,
    // yielding a disk device entry that is not a network redirector mount.
    if (entry->Type != FILE_DEVICE_NETWORK_FILE_SYSTEM) {
      continue;
    }
    // The driver does not guarantee NUL termination of the fixed arrays.
    SIZE_T uncNameLength =
        NpBoundedLength(entry->UNCName, ARRAYSIZE(entry->UNCName));
    if (uncNameLength == 0) {
      continue;
    }

    // The transport key is the mount point without its "\DosDevices\" prefix,
    // the form FSCTL_EVENT_RELEASE expects ("X:" or "C:\mount\point").
    const WCHAR *key = entry->MountPoint;
    SIZE_T keyLength =
        NpBoundedLength(entry->MountPoint, ARRAYSIZE(entry->MountPoint));
    WCHAR driveLetter = 0;
    if (NpHasDosDevicesPrefix(key, keyLength)) {
      key += DOKAN_NP_DOS_DEVICES_PREFIX_LENGTH;
      keyLength -= DOKAN_NP_DOS_DEVICES_PREFIX_LENGTH;
      if (keyLength == 2 && key[1] == L':' && NpIsAsciiLetter(key[0])) {
        driveLetter = key[0];
      }
    }
    ULONG flags = (entry->MountOptions & DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT)
                      ? DOKAN_NP_MOUNT_CANCELABLE
                      : 0;

    error = DokanNpTableBuilderAdd(builder, entry->UNCName, uncNameLength,
                                   driveLetter, flags, key, keyLength);
    if (error == ERROR_INVALID_NAME) {
      continue;
    }
    if (error != ERROR_SUCCESS) {
      DokanNpTableBuilderAbort(builder);
      return error;
    }
  }

  return DokanNpTableBuilderFinish(builder, Table);
}

static DWORD NpEmptyTable(DOKAN_NP_MOUNT_TABLE **Table) {
  DOKAN_NP_TABLE_BUILDER *builder = NULL;
  if (DokanNpTableBuilderCreate(&builder) != ERROR_SUCCESS ||
      DokanNpTableBuilderFinish(builder, Table) != ERROR_SUCCESS) {
    return WN_OUT_OF_MEMORY;
  }
  return WN_SUCCESS;
}

static HANDLE NpOpenControlDevice(void) {
  // All the control codes used here are FILE_ANY_ACCESS.
  return CreateFileW(DOKAN_NP_GLOBAL_DEVICE_NAME, 0,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                     NULL);
}

static DWORD NpLiveQueryMounts(PVOID Context, DOKAN_NP_MOUNT_TABLE **Table) {
  UNREFERENCED_PARAMETER(Context);
  *Table = NULL;

  DWORD sessionId = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
    sessionId = DOKAN_NP_NO_SESSION;
  }

  HANDLE device = NpOpenControlDevice();
  if (device == INVALID_HANDLE_VALUE) {
    // Driver not installed or not running: nothing is mounted.
    return NpEmptyTable(Table);
  }

  DOKAN_MOUNT_POINT_INFO *entries = NULL;
  ULONG capacity = DOKAN_NP_INITIAL_LIST_ENTRIES;
  DWORD returned = 0;
  for (;;) {
    DWORD bytes = (DWORD)(capacity * sizeof(DOKAN_MOUNT_POINT_INFO));
    entries = (DOKAN_MOUNT_POINT_INFO *)HeapAlloc(GetProcessHeap(),
                                                  HEAP_ZERO_MEMORY, bytes);
    if (entries == NULL) {
      CloseHandle(device);
      return WN_OUT_OF_MEMORY;
    }
    returned = 0;
    if (DeviceIoControl(device, FSCTL_EVENT_MOUNTPOINT_LIST, NULL, 0, entries,
                        bytes, &returned, NULL)) {
      break;
    }
    DWORD error = GetLastError();
    HeapFree(GetProcessHeap(), 0, entries);
    entries = NULL;
    if (error != ERROR_MORE_DATA || capacity >= DOKAN_NP_MAX_ENTRIES) {
      CloseHandle(device);
      return NpEmptyTable(Table);
    }
    capacity *= 2;
  }
  CloseHandle(device);

  ULONG count = (ULONG)(returned / sizeof(DOKAN_MOUNT_POINT_INFO));
  if (count > capacity) {
    count = capacity;
  }
  DWORD error =
      DokanNpTableFromMountPointInfo(entries, count, sessionId, Table);
  HeapFree(GetProcessHeap(), 0, entries);
  if (error == ERROR_NOT_ENOUGH_MEMORY) {
    return WN_OUT_OF_MEMORY;
  }
  return error == ERROR_SUCCESS ? WN_SUCCESS : NpEmptyTable(Table);
}

static DWORD NpLiveCancelMount(PVOID Context, const DOKAN_NP_MOUNT *Mount,
                               BOOL Force) {
  // The driver release request has no graceful mode.
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Force);

  if (Mount == NULL || Mount->TransportKey == NULL) {
    return WN_NOT_CONNECTED;
  }
  SIZE_T keyLength = NpBoundedLength(Mount->TransportKey, MAX_PATH);
  if (keyLength == 0 || keyLength >= MAX_PATH) {
    return WN_NOT_CONNECTED;
  }

  // Same request layout as the library's SendGlobalReleaseIRP.
  DWORD inputLength = (DWORD)(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) +
                              MAX_PATH * sizeof(WCHAR));
  DOKAN_UNICODE_STRING_INTERMEDIATE *request =
      (DOKAN_UNICODE_STRING_INTERMEDIATE *)HeapAlloc(
          GetProcessHeap(), HEAP_ZERO_MEMORY, inputLength);
  if (request == NULL) {
    return WN_OUT_OF_MEMORY;
  }
  request->MaximumLength = (USHORT)(MAX_PATH * sizeof(WCHAR));
  request->Length = (USHORT)(keyLength * sizeof(WCHAR));
  CopyMemory(request->Buffer, Mount->TransportKey, keyLength * sizeof(WCHAR));

  DWORD status = WN_NOT_CONNECTED;
  HANDLE device = NpOpenControlDevice();
  if (device != INVALID_HANDLE_VALUE) {
    DWORD returned = 0;
    if (DeviceIoControl(device, FSCTL_EVENT_RELEASE, request, inputLength, NULL,
                        0, &returned, NULL)) {
      status = WN_SUCCESS;
    } else {
      switch (GetLastError()) {
      case ERROR_ACCESS_DENIED:
        status = WN_ACCESS_DENIED;
        break;
      case ERROR_OPEN_FILES:
        status = WN_OPEN_FILES;
        break;
      case ERROR_BUSY:
        status = WN_FUNCTION_BUSY;
        break;
      default:
        // Includes the "no such mount point" reply of the driver.
        status = WN_NOT_CONNECTED;
        break;
      }
    }
    CloseHandle(device);
  }

  HeapFree(GetProcessHeap(), 0, request);
  return status;
}

const DOKAN_NP_TRANSPORT g_DokanNpLiveTransport = {
    .QueryMounts = NpLiveQueryMounts,
    .CancelMount = NpLiveCancelMount,
    .AddDriveAlias = NULL,
    .Context = NULL,
};
