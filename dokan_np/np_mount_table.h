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

// Mount table seam between the network provider protocol logic (np_core.c)
// and the transport that talks to the Dokan driver (np_transport_live.c).
// Internal to dokannp3.dll and its unit tests: this is not an exported ABI.

#ifndef DOKAN_NP_MOUNT_TABLE_H_
#define DOKAN_NP_MOUNT_TABLE_H_

#ifndef _WINDOWS_
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#endif

#include <winnetwk.h>

#ifdef __cplusplus
extern "C" {
#endif

// DOKAN_NP_MOUNT.Flags
// NPCancelConnection may disconnect this entry (unmount or alias removal).
#define DOKAN_NP_MOUNT_CANCELABLE 0x00000001UL
// Entry is a drive letter alias of another entry with the same RemoteName.
// Lookups keyed by remote name (cancel / add by UNC) skip alias entries.
#define DOKAN_NP_MOUNT_ALIAS 0x00000002UL

// One network-visible Dokan mount, normalized. Immutable; all pointers point
// into the owning DOKAN_NP_MOUNT_TABLE allocation.
typedef struct _DOKAN_NP_MOUNT {
  // Canonical "\\server\share[\more]": exactly two leading backslashes, no
  // empty component, no trailing backslash, no embedded NUL, NUL-terminated.
  // The full RemoteName is the network resource identity, including when it
  // has more than two components.
  LPCWSTR RemoteName;
  // WCHARs in RemoteName, excluding the terminating NUL.
  ULONG RemoteNameLength;
  // WCHARs in the "\\server" prefix of RemoteName.
  ULONG ServerEnd;
  // L'A'..L'Z', or 0 when the mount has no drive letter (folder or UNC only).
  WCHAR DriveLetter;
  // DOKAN_NP_MOUNT_* flags.
  ULONG Flags;
  // Opaque to the protocol logic; handed back to
  // DOKAN_NP_TRANSPORT.CancelMount. Never NULL, may be empty.
  LPCWSTR TransportKey;
} DOKAN_NP_MOUNT, *PDOKAN_NP_MOUNT;

// Single HeapAlloc(GetProcessHeap()) block holding the header, the entries and
// the string pool. Contains only the entries visible to the calling process.
// Entry order is the transport's order. For lookups keyed by drive letter the
// first entry with a given DriveLetter wins.
typedef struct _DOKAN_NP_MOUNT_TABLE {
  ULONG Count;
  DOKAN_NP_MOUNT Entries[ANYSIZE_ARRAY];
} DOKAN_NP_MOUNT_TABLE, *PDOKAN_NP_MOUNT_TABLE;

// Builder: the only way to create a table, so that every transport produces
// identically normalized entries. Builder functions return Win32 ERROR_* codes.
typedef struct _DOKAN_NP_TABLE_BUILDER DOKAN_NP_TABLE_BUILDER;

// Returns ERROR_SUCCESS, ERROR_INVALID_PARAMETER or ERROR_NOT_ENOUGH_MEMORY.
DWORD DokanNpTableBuilderCreate(DOKAN_NP_TABLE_BUILDER **Builder);

// Adds one mount. UncName is "\server\share[\more]" or "\\server\share[\more]"
// (UncNameLength WCHARs, not necessarily NUL-terminated). DriveLetter may be
// 'A'..'Z', 'a'..'z', or 0 (folder or driveless UNC mount). TransportKey may be
// NULL only when TransportKeyLength is 0.
// Returns ERROR_SUCCESS when added, ERROR_INVALID_NAME when the entry is
// rejected (not a valid network-visible mount; the caller continues with the
// next entry), ERROR_NOT_ENOUGH_MEMORY or ERROR_INVALID_PARAMETER otherwise.
DWORD DokanNpTableBuilderAdd(DOKAN_NP_TABLE_BUILDER *Builder, LPCWSTR UncName,
                             SIZE_T UncNameLength, WCHAR DriveLetter,
                             ULONG Flags, LPCWSTR TransportKey,
                             SIZE_T TransportKeyLength);

// Always consumes Builder. On success *Table is non-NULL (Count may be 0) and
// must be released with DokanNpTableFree.
DWORD DokanNpTableBuilderFinish(DOKAN_NP_TABLE_BUILDER *Builder,
                                DOKAN_NP_MOUNT_TABLE **Table);

// Releases a builder that will not be finished. Accepts NULL.
VOID DokanNpTableBuilderAbort(DOKAN_NP_TABLE_BUILDER *Builder);

// Releases a table produced by DokanNpTableBuilderFinish. Accepts NULL.
VOID DokanNpTableFree(DOKAN_NP_MOUNT_TABLE *Table);

// Transport callbacks (return WN_* status codes). All callbacks must be
// callable concurrently from any thread.

// Returns a snapshot of the mounts visible to the calling process. Visibility
// policy (session/SID/cross-session filtering) lives in the transport, never in
// np_core.c. Returns WN_SUCCESS (with a possibly empty table) or
// WN_OUT_OF_MEMORY. A missing or stopped driver yields an empty table.
typedef DWORD (*DOKAN_NP_QUERY_MOUNTS_FN)(PVOID Context,
                                          DOKAN_NP_MOUNT_TABLE **Table);

// Disconnects an entry of a table produced by this transport. Unmount versus
// alias removal is transport policy keyed by Mount->TransportKey. Force is
// forwarded from NPCancelConnection (transports without a graceful release mode
// may ignore it). Returns WN_SUCCESS, WN_NOT_CONNECTED, WN_ACCESS_DENIED,
// WN_FUNCTION_BUSY or WN_OPEN_FILES.
typedef DWORD (*DOKAN_NP_CANCEL_MOUNT_FN)(PVOID Context,
                                          const DOKAN_NP_MOUNT *Mount,
                                          BOOL Force);

// Optional (NULL when unsupported): maps DriveLetter ('A'..'Z') onto the
// already-mounted primary Target.
typedef DWORD (*DOKAN_NP_ADD_DRIVE_ALIAS_FN)(PVOID Context,
                                             const DOKAN_NP_MOUNT *Target,
                                             WCHAR DriveLetter);

// Transport: the only boundary between the protocol logic and the driver.
typedef struct _DOKAN_NP_TRANSPORT {
  DOKAN_NP_QUERY_MOUNTS_FN QueryMounts;
  DOKAN_NP_CANCEL_MOUNT_FN CancelMount;
  DOKAN_NP_ADD_DRIVE_ALIAS_FN AddDriveAlias;
  PVOID Context;
} DOKAN_NP_TRANSPORT, *PDOKAN_NP_TRANSPORT;

static __inline DWORD
DokanNpTransportQueryMounts(const DOKAN_NP_TRANSPORT *Transport,
                            DOKAN_NP_MOUNT_TABLE **Table) {
  *Table = NULL;
  if (Transport == NULL || Transport->QueryMounts == NULL) {
    return WN_NO_NETWORK;
  }
  DWORD status = Transport->QueryMounts(Transport->Context, Table);
  if (status != WN_SUCCESS) {
    DokanNpTableFree(*Table);
    *Table = NULL;
    return status;
  }
  return *Table != NULL ? WN_SUCCESS : WN_OUT_OF_MEMORY;
}

static __inline DWORD
DokanNpTransportCancelMount(const DOKAN_NP_TRANSPORT *Transport,
                            const DOKAN_NP_MOUNT *Mount, BOOL Force) {
  if (Transport == NULL || Transport->CancelMount == NULL) {
    return WN_NOT_SUPPORTED;
  }
  return Transport->CancelMount(Transport->Context, Mount, Force);
}

static __inline DWORD
DokanNpTransportAddDriveAlias(const DOKAN_NP_TRANSPORT *Transport,
                              const DOKAN_NP_MOUNT *Target, WCHAR DriveLetter) {
  if (Transport == NULL || Transport->AddDriveAlias == NULL) {
    return WN_NOT_SUPPORTED;
  }
  return Transport->AddDriveAlias(Transport->Context, Target, DriveLetter);
}

#ifdef __cplusplus
}
#endif

#endif // DOKAN_NP_MOUNT_TABLE_H_
