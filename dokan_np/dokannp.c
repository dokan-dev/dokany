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

// Network provider entry points (npapi.h) exported by dokannp3.dll. This DLL
// is loaded into Explorer and into every process using the WNet API, so it
// keeps no state, does no work in DllMain, imports only KERNEL32.dll and emits
// no debug output in release builds.

#include "np_core.h"
#include "np_transport_live.h"

#include <npapi.h>

#if defined(_DEBUG)
#include <strsafe.h>

static VOID DokanNpTrace(LPCWSTR Function, DWORD Status) {
  WCHAR message[128];
  if (SUCCEEDED(StringCchPrintfW(message, ARRAYSIZE(message),
                                 L"dokannp: %s -> %lu\n", Function, Status))) {
    OutputDebugStringW(message);
  }
}

#define DOKAN_NP_TRACE(Function, Status) DokanNpTrace(Function, Status)
#else
#define DOKAN_NP_TRACE(Function, Status) ((void)0)
#endif

DWORD APIENTRY NPGetCaps(DWORD Index) { return DokanNpGetCaps(Index); }

DWORD APIENTRY NPAddConnection(LPNETRESOURCE NetResource, LPWSTR Password,
                               LPWSTR UserName) {
  DWORD status = DokanNpAddConnection(&g_DokanNpLiveTransport, NetResource,
                                      Password, UserName);
  DOKAN_NP_TRACE(L"NPAddConnection", status);
  return status;
}

DWORD APIENTRY NPAddConnection3(HWND Owner, LPNETRESOURCE NetResource,
                                LPWSTR Password, LPWSTR UserName, DWORD Flags) {
  DWORD status = DokanNpAddConnection3(&g_DokanNpLiveTransport, Owner,
                                       NetResource, Password, UserName, Flags);
  DOKAN_NP_TRACE(L"NPAddConnection3", status);
  return status;
}

DWORD APIENTRY NPCancelConnection(LPWSTR Name, BOOL Force) {
  DWORD status = DokanNpCancelConnection(&g_DokanNpLiveTransport, Name, Force);
  DOKAN_NP_TRACE(L"NPCancelConnection", status);
  return status;
}

DWORD APIENTRY NPGetConnection(LPWSTR LocalName, LPWSTR RemoteName,
                               LPDWORD BufferSize) {
  DWORD status = DokanNpGetConnection(&g_DokanNpLiveTransport, LocalName,
                                      RemoteName, BufferSize);
  DOKAN_NP_TRACE(L"NPGetConnection", status);
  return status;
}

DWORD APIENTRY NPOpenEnum(DWORD Scope, DWORD Type, DWORD Usage,
                          LPNETRESOURCE NetResource, LPHANDLE Enum) {
  DWORD status = DokanNpOpenEnum(&g_DokanNpLiveTransport, Scope, Type, Usage,
                                 NetResource, Enum);
  DOKAN_NP_TRACE(L"NPOpenEnum", status);
  return status;
}

DWORD APIENTRY NPEnumResource(HANDLE Enum, LPDWORD Count, LPVOID Buffer,
                              LPDWORD BufferSize) {
  DWORD status = DokanNpEnumResource(Enum, Count, Buffer, BufferSize);
  DOKAN_NP_TRACE(L"NPEnumResource", status);
  return status;
}

DWORD APIENTRY NPCloseEnum(HANDLE Enum) {
  DWORD status = DokanNpCloseEnum(Enum);
  DOKAN_NP_TRACE(L"NPCloseEnum", status);
  return status;
}

DWORD APIENTRY NPGetUniversalName(LPCWSTR LocalPath, DWORD InfoLevel,
                                  LPVOID Buffer, LPDWORD BufferSize) {
  DWORD status = DokanNpGetUniversalName(&g_DokanNpLiveTransport, LocalPath,
                                         InfoLevel, Buffer, BufferSize);
  DOKAN_NP_TRACE(L"NPGetUniversalName", status);
  return status;
}

DWORD APIENTRY NPGetResourceParent(LPNETRESOURCE NetResource, LPVOID Buffer,
                                   LPDWORD BufferSize) {
  DWORD status = DokanNpGetResourceParent(&g_DokanNpLiveTransport, NetResource,
                                          Buffer, BufferSize);
  DOKAN_NP_TRACE(L"NPGetResourceParent", status);
  return status;
}

DWORD APIENTRY NPGetResourceInformation(LPNETRESOURCE NetResource,
                                        LPVOID Buffer, LPDWORD BufferSize,
                                        LPWSTR *System) {
  DWORD status = DokanNpGetResourceInformation(
      &g_DokanNpLiveTransport, NetResource, Buffer, BufferSize, System);
  DOKAN_NP_TRACE(L"NPGetResourceInformation", status);
  return status;
}

DWORD APIENTRY NPFormatNetworkName(LPWSTR RemoteName, LPWSTR FormattedName,
                                   LPDWORD Length, DWORD Flags,
                                   DWORD AveCharPerLine) {
  DWORD status = DokanNpFormatNetworkName(RemoteName, FormattedName, Length,
                                          Flags, AveCharPerLine);
  DOKAN_NP_TRACE(L"NPFormatNetworkName", status);
  return status;
}
