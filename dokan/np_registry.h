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

// Network provider registration (Services\dokan<N>\NetworkProvider and the
// Control\NetworkProvider ProviderOrder lists). Internal to dokan<N>.dll and
// its unit tests.

#ifndef DOKAN_NP_REGISTRY_H_
#define DOKAN_NP_REGISTRY_H_

#ifndef _WINDOWS_
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Appends Provider to the comma separated Order (NULL means empty) unless it
// is already one of its tokens (compared case-insensitively, ignoring
// surrounding blanks). On success *NewOrder is NULL when Order needs no change,
// else a new string to release with DokanProviderOrderFree.
DWORD DokanProviderOrderAdd(LPCWSTR Order, LPCWSTR Provider, LPWSTR *NewOrder);

// Removes every token equal to Provider from Order. Other non-empty tokens are
// kept, without their surrounding blanks. *NewOrder follows the
// DokanProviderOrderAdd convention.
DWORD DokanProviderOrderRemove(LPCWSTR Order, LPCWSTR Provider,
                               LPWSTR *NewOrder);

// Accepts NULL.
VOID DokanProviderOrderFree(LPWSTR Order);

// Registers or unregisters the network provider below Root, which stands for
// HKEY_LOCAL_MACHINE (paths start with "System\CurrentControlSet\").
// Returns a Win32 error code.
DWORD DokanNetworkProviderInstallAt(HKEY Root);
DWORD DokanNetworkProviderUninstallAt(HKEY Root);

#ifdef __cplusplus
}
#endif

#endif // DOKAN_NP_REGISTRY_H_
