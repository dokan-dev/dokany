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

// Network provider protocol logic (npapi.h contracts) on top of the mount
// table seam. Every function mirrors the NP* entry point of the same name,
// with the transport passed explicitly so that it can be tested without the
// Dokan driver.

#ifndef DOKAN_NP_CORE_H_
#define DOKAN_NP_CORE_H_

#include "../dokan/dokan.h"

#include <winnetwk.h>

#include "np_mount_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DOKAN_NP_NAME
#define DOKAN_NP_NAME L"Dokan" DOKAN_MAJOR_API_VERSION
#endif

// All size parameters follow the npapi.h no-shrink contract: *BufferSize /
// *Length is updated only when returning WN_MORE_DATA and left untouched on
// WN_SUCCESS.

DWORD DokanNpGetCaps(DWORD Index);

DWORD DokanNpAddConnection(const DOKAN_NP_TRANSPORT *Transport,
                           LPNETRESOURCEW NetResource, LPWSTR Password,
                           LPWSTR UserName);

// Only targets already-mounted Dokan shares (deviceless validation or drive
// aliasing via Transport->AddDriveAlias); never spawns processes or prompts.
DWORD DokanNpAddConnection3(const DOKAN_NP_TRANSPORT *Transport, HWND Owner,
                            LPNETRESOURCEW NetResource, LPWSTR Password,
                            LPWSTR UserName, DWORD Flags);

// Accepts either a drive ("X:" / "X:\") or a primary UNC name ("\\srv\share")
// and forwards cancelable entries to Transport->CancelMount.
DWORD DokanNpCancelConnection(const DOKAN_NP_TRANSPORT *Transport, LPWSTR Name,
                              BOOL Force);

// BufferSize is a count of WCHARs (including the terminating NUL on
// WN_MORE_DATA).
DWORD DokanNpGetConnection(const DOKAN_NP_TRANSPORT *Transport,
                           LPWSTR LocalName, LPWSTR RemoteName,
                           LPDWORD BufferSize);

// Captures one mount-table snapshot for the lifetime of *Enum. Usage is
// honored only for RESOURCE_GLOBALNET per npapi.h.
DWORD DokanNpOpenEnum(const DOKAN_NP_TRANSPORT *Transport, DWORD Scope,
                      DWORD Type, DWORD Usage, LPNETRESOURCEW NetResource,
                      LPHANDLE Enum);

// BufferSize is a count of bytes.
DWORD DokanNpEnumResource(HANDLE Enum, LPDWORD Count, LPVOID Buffer,
                          LPDWORD BufferSize);

DWORD DokanNpCloseEnum(HANDLE Enum);

// BufferSize is a count of bytes. Supports UNIVERSAL_NAME_INFO_LEVEL and
// REMOTE_NAME_INFO_LEVEL.
DWORD DokanNpGetUniversalName(const DOKAN_NP_TRANSPORT *Transport,
                              LPCWSTR LocalPath, DWORD InfoLevel, LPVOID Buffer,
                              LPDWORD BufferSize);

// BufferSize is a count of bytes. NetResource and its strings may reside inside
// Buffer (buffer aliasing is safe).
DWORD DokanNpGetResourceParent(const DOKAN_NP_TRANSPORT *Transport,
                               LPNETRESOURCEW NetResource, LPVOID Buffer,
                               LPDWORD BufferSize);

// BufferSize is a count of bytes. NetResource and its strings may reside inside
// Buffer (buffer aliasing is safe). On success *System is NULL or points inside
// Buffer.
DWORD DokanNpGetResourceInformation(const DOKAN_NP_TRANSPORT *Transport,
                                    LPNETRESOURCEW NetResource, LPVOID Buffer,
                                    LPDWORD BufferSize, LPWSTR *System);

// Length is a count of WCHARs (including the terminating NUL on WN_MORE_DATA).
DWORD DokanNpFormatNetworkName(LPWSTR RemoteName, LPWSTR FormattedName,
                               LPDWORD Length, DWORD Flags,
                               DWORD AveCharPerLine);

#ifdef __cplusplus
}
#endif

#endif // DOKAN_NP_CORE_H_
