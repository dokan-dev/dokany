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

DWORD DokanNpGetCaps(DWORD Index);

DWORD DokanNpAddConnection(const DOKAN_NP_TRANSPORT *Transport,
                           LPNETRESOURCEW NetResource, LPWSTR Password,
                           LPWSTR UserName);

DWORD DokanNpAddConnection3(const DOKAN_NP_TRANSPORT *Transport, HWND Owner,
                            LPNETRESOURCEW NetResource, LPWSTR Password,
                            LPWSTR UserName, DWORD Flags);

DWORD DokanNpCancelConnection(const DOKAN_NP_TRANSPORT *Transport, LPWSTR Name,
                              BOOL Force);

// BufferSize is a count of WCHARs.
DWORD DokanNpGetConnection(const DOKAN_NP_TRANSPORT *Transport,
                           LPWSTR LocalName, LPWSTR RemoteName,
                           LPDWORD BufferSize);

DWORD DokanNpOpenEnum(const DOKAN_NP_TRANSPORT *Transport, DWORD Scope,
                      DWORD Type, DWORD Usage, LPNETRESOURCEW NetResource,
                      LPHANDLE Enum);

// BufferSize is a count of bytes.
DWORD DokanNpEnumResource(HANDLE Enum, LPDWORD Count, LPVOID Buffer,
                          LPDWORD BufferSize);

DWORD DokanNpCloseEnum(HANDLE Enum);

// BufferSize is a count of bytes.
DWORD DokanNpGetUniversalName(const DOKAN_NP_TRANSPORT *Transport,
                              LPCWSTR LocalPath, DWORD InfoLevel, LPVOID Buffer,
                              LPDWORD BufferSize);

// BufferSize is a count of bytes.
DWORD DokanNpGetResourceParent(const DOKAN_NP_TRANSPORT *Transport,
                               LPNETRESOURCEW NetResource, LPVOID Buffer,
                               LPDWORD BufferSize);

// BufferSize is a count of bytes. On success *System is NULL or points inside
// Buffer.
DWORD DokanNpGetResourceInformation(const DOKAN_NP_TRANSPORT *Transport,
                                    LPNETRESOURCEW NetResource, LPVOID Buffer,
                                    LPDWORD BufferSize, LPWSTR *System);

// Length is a count of WCHARs.
DWORD DokanNpFormatNetworkName(LPWSTR RemoteName, LPWSTR FormattedName,
                               LPDWORD Length, DWORD Flags,
                               DWORD AveCharPerLine);

#ifdef __cplusplus
}
#endif

#endif // DOKAN_NP_CORE_H_
