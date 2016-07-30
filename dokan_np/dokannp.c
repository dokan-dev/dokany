/*
Dokan : user-mode file system library for Windows

Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

http://dokan-dev.github.io

------------
Some work on Network Provider can be originated from VirtualBox Windows Guest Shared Folders project
http://www.virtualbox.org/svn/vbox/trunk/src/VBox/Additions/WINNT/SharedFolders/np/vboxmrxnp.cpp
------------

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

#include "../dokan/dokan.h"
#include <malloc.h>
#include <npapi.h>
#include <stdio.h>
#include <strsafe.h>
#include <windows.h>
#include <winnetwk.h>

static VOID DokanDbgPrintW(LPCWSTR format, ...) {
  const WCHAR *outputString;
  WCHAR *buffer;
  size_t length;
  va_list argp;

  va_start(argp, format);
  length = _vscwprintf(format, argp) + 1;
  buffer = _malloca(length * sizeof(WCHAR));
  if (buffer) {
    StringCchVPrintfW(buffer, length, format, argp);
    outputString = buffer;
  } else {
    outputString = format;
  }
  OutputDebugStringW(outputString);
  _freea(buffer);
  va_end(argp);
}

#define DbgPrintW(format, ...) DokanDbgPrintW(format, __VA_ARGS__)

DWORD APIENTRY NPGetCaps(DWORD Index) {
  DWORD rc = 0;
  DbgPrintW(L"NPGetCaps %d\n", Index);

  switch (Index) {
  case WNNC_SPEC_VERSION:
    DbgPrintW(L"  WNNC_SPEC_VERSION\n");
    rc = WNNC_SPEC_VERSION51;
    break;

  case WNNC_NET_TYPE:
    DbgPrintW(L"  WNNC_NET_TYPE\n");
    rc = WNNC_NET_RDR2SAMPLE;
    break;

  case WNNC_DRIVER_VERSION:
    DbgPrintW(L"  WNNC_DRIVER_VERSION\n");
    rc = 1;
    break;

  case WNNC_CONNECTION:
    DbgPrintW(L"  WNC_CONNECTION\n");
    rc = WNNC_CON_GETCONNECTIONS | WNNC_CON_CANCELCONNECTION |
         WNNC_CON_ADDCONNECTION | WNNC_CON_ADDCONNECTION3;
    break;

  case WNNC_ENUMERATION:
    DbgPrintW(L"  WNNC_ENUMERATION\n");
    rc = WNNC_ENUM_LOCAL | WNNC_ENUM_GLOBAL | WNNC_ENUM_SHAREABLE;
    break;

  case WNNC_START:
    DbgPrintW(L"  WNNC_START\n");
    rc = WNNC_WAIT_FOR_START;
    break;

  case WNNC_USER:
    DbgPrintW(L"  WNNC_USER\n");
    rc = 0;
    break;
  case WNNC_DIALOG:
    DbgPrintW(L"  WNNC_DIALOG\n");
    rc = WNNC_DLG_GETRESOURCEPARENT | WNNC_DLG_GETRESOURCEINFORMATION;
    break;
  case WNNC_ADMIN:
    DbgPrintW(L"  WNNC_ADMIN\n");
    rc = 0;
    break;
  default:
    DbgPrintW(L"  default\n");
    rc = 0;
    break;
  }

  return rc;
}

static const WCHAR *parseServerName(const WCHAR *lpRemoteName,
                                    const WCHAR **lpServerName,
                                    ULONG *ulServerName) {
  int cLeadingBackslashes = 0;
  while (*lpRemoteName == L'\\') {
    lpRemoteName++;
    cLeadingBackslashes++;
  }

  DbgPrintW(L"parseServerName: cLeadingBackslashes %d\n", cLeadingBackslashes);

  if (cLeadingBackslashes == 0 || cLeadingBackslashes == 2) {
    if (lpServerName != NULL) {
      *lpServerName = lpRemoteName;
    }
    if (ulServerName != NULL) {
      *ulServerName = 0;
    }

    while (*lpRemoteName && *lpRemoteName != L'\\') {
      lpRemoteName++;
      if (ulServerName != NULL) {
        (*ulServerName)++;
      }
    }

    return lpRemoteName;
  }

  return NULL;
}

DWORD APIENTRY NPLogonNotify(__in PLUID LogonId, __in PCWSTR AuthentInfoType,
                             __in PVOID AuthentInfo,
                             __in PCWSTR PreviousAuthentInfoType,
                             __in PVOID PreviousAuthentInfo,
                             __in PWSTR StationName, __in PVOID StationHandle,
                             __out PWSTR *LogonScript) {
  UNREFERENCED_PARAMETER(LogonId);
  UNREFERENCED_PARAMETER(AuthentInfoType);
  UNREFERENCED_PARAMETER(AuthentInfo);
  UNREFERENCED_PARAMETER(PreviousAuthentInfoType);
  UNREFERENCED_PARAMETER(PreviousAuthentInfo);
  UNREFERENCED_PARAMETER(StationName);
  UNREFERENCED_PARAMETER(StationHandle);

  DbgPrintW(L"NPLogonNotify\n");
  *LogonScript = NULL;
  return WN_SUCCESS;
}

DWORD APIENTRY NPPasswordChangeNotify(
    __in LPCWSTR AuthentInfoType, __in LPVOID AuthentInfo,
    __in LPCWSTR PreviousAuthentInfoType, __in LPVOID RreviousAuthentInfo,
    __in LPWSTR StationName, __in PVOID StationHandle, __in DWORD ChangeInfo) {
  UNREFERENCED_PARAMETER(AuthentInfoType);
  UNREFERENCED_PARAMETER(AuthentInfo);
  UNREFERENCED_PARAMETER(PreviousAuthentInfoType);
  UNREFERENCED_PARAMETER(RreviousAuthentInfo);
  UNREFERENCED_PARAMETER(StationName);
  UNREFERENCED_PARAMETER(StationHandle);
  UNREFERENCED_PARAMETER(ChangeInfo);

  DbgPrintW(L"NPPasswordChangeNotify\n");
  SetLastError(WN_NOT_SUPPORTED);
  return WN_NOT_SUPPORTED;
}

DWORD APIENTRY NPAddConnection(__in LPNETRESOURCE NetResource,
                               __in LPWSTR Password, __in LPWSTR UserName) {
  DbgPrintW(L"NPAddConnection\n");
  return NPAddConnection3(NULL, NetResource, Password, UserName, 0);
}

DWORD APIENTRY NPAddConnection3(__in HWND WndOwner,
                                __in LPNETRESOURCE NetResource,
                                __in LPWSTR Password, __in LPWSTR UserName,
                                __in DWORD Flags) {
  DWORD status;
  WCHAR temp[MAX_PATH + 1];
  WCHAR local[3];

  UNREFERENCED_PARAMETER(WndOwner);
  UNREFERENCED_PARAMETER(Password);
  UNREFERENCED_PARAMETER(UserName);
  UNREFERENCED_PARAMETER(Flags);

  DbgPrintW(L"NPAddConnection3\n");
  DbgPrintW(L"  LocalName: %s\n", NetResource->lpLocalName);
  DbgPrintW(L"  RemoteName: %s\n", NetResource->lpRemoteName);

  ZeroMemory(local, sizeof(local));

  if (lstrlen(NetResource->lpLocalName) > 1 &&
      NetResource->lpLocalName[1] == L':') {
    local[0] = (WCHAR)toupper(NetResource->lpLocalName[0]);
    local[1] = L':';
    local[2] = L'\0';
  }

  if (QueryDosDevice(local, temp, MAX_PATH / 2)) {
    DbgPrintW(L"  WN_ALREADY_CONNECTED\n");
    status = WN_ALREADY_CONNECTED;
  } else {
    DbgPrintW(L"  WN_BAD_NETNAME\n");
    status = WN_BAD_NETNAME;
  }

  return status;
}

DWORD APIENTRY NPCancelConnection(__in LPWSTR Name, __in BOOL Force) {
  DbgPrintW(L"NpCancelConnection %s %d\n", Name, Force);
  return WN_SUCCESS;
}

DWORD APIENTRY NPGetConnection(__in LPWSTR LocalName, __out LPWSTR RemoteName,
                               __inout LPDWORD BufferSize) {
  DbgPrintW(L"NpGetConnection %s, %d\n", LocalName, *BufferSize);

  ULONG nbRead = 0;
  WCHAR dosDevice[] = L"\\DosDevices\\C:";
  DOKAN_CONTROL dokanControl[DOKAN_MAX_INSTANCES];
  if (!DokanGetMountPointList(dokanControl, DOKAN_MAX_INSTANCES, FALSE,
                              &nbRead)) {
    DbgPrintW(L"NpGetConnection DokanGetMountPointList failed\n");
    return WN_NOT_CONNECTED;
  }
  dosDevice[12] = LocalName[0];

  for (unsigned int i = 0; i < nbRead; ++i) {
    if (wcscmp(dokanControl[i].MountPoint, dosDevice) == 0) {
      if (wcscmp(dokanControl[i].UNCName, L"") == 0) {
        // No UNC, always return success
		if (*BufferSize != 0) {
			RemoteName[0] = L'\0';
			*BufferSize = sizeof(WCHAR);
		}
        return WN_SUCCESS;
      }

      DWORD len = (lstrlenW(dokanControl[i].UNCName) + 1) * sizeof(WCHAR);
      if (len > *BufferSize) {
        *BufferSize = len;
        return WN_MORE_DATA;
      }
      CopyMemory(RemoteName, dokanControl[i].UNCName, len);
      *BufferSize = len;
      return WN_SUCCESS;
    }
  }

  return WN_NOT_CONNECTED;
}

/* Enumerate shared folders as hierarchy:
* SERVER(container)
* +--------------------+
* |                     \
* Folder1(connectable)  FolderN(connectable)
*/
typedef struct _NPENUMCTX {
  ULONG index; /* Index of last entry returned. */
  DWORD dwScope;
  DWORD dwOriginalScope;
  DWORD dwType;
  DWORD dwUsage;
  BOOL fRoot;
} NPENUMCTX;

DWORD APIENTRY NPOpenEnum(__in DWORD Scope, __in DWORD Type, __in DWORD Usage,
                          __in LPNETRESOURCE NetResource, __in LPHANDLE Enum) {
  DWORD dwStatus;

  DbgPrintW(L"NPOpenEnum: dwScope 0x%08X, dwType 0x%08X, dwUsage 0x%08X, "
            L"lpNetResource %p\n",
            Scope, Type, Usage, NetResource);

  if (Usage == 0) {
    /* The bitmask may be zero to match all of the flags. */
    Usage = RESOURCEUSAGE_CONNECTABLE | RESOURCEUSAGE_CONTAINER;
  }

  *Enum = NULL;

  /* Allocate the context structure. */
  NPENUMCTX *pCtx = (NPENUMCTX *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           sizeof(NPENUMCTX));

  if (pCtx == NULL) {
    dwStatus = WN_OUT_OF_MEMORY;
  } else {
    if (NetResource && NetResource->lpRemoteName) {
      DbgPrintW(L"NPOpenEnum: lpRemoteName %ls\n", NetResource->lpRemoteName);
    }

    switch (Scope) {
    case 6: /* Advertised as WNNC_ENUM_SHAREABLE. This returns C$ system shares.
                    * NpEnumResource will return NO_MORE_ENTRIES.
                    */
    {
      if (NetResource == NULL || NetResource->lpRemoteName == NULL) {
        /* If it is NULL or if the lpRemoteName field of the NETRESOURCE is
        * NULL,
        * the provider should enumerate the top level of its network.
        * But system shares can't be on top level.
        */
        dwStatus = WN_NOT_CONTAINER;
        break;
      }

      const WCHAR *lpAfterName =
          parseServerName(NetResource->lpRemoteName, NULL, NULL);
      if (lpAfterName == NULL || (*lpAfterName != L'\\' && *lpAfterName != 0)) {
        dwStatus = WN_NOT_CONTAINER;
        break;
      }

      /* Valid server name. */
      pCtx->index = 0;
      pCtx->dwScope = 6;
      pCtx->dwOriginalScope = Scope;
      pCtx->dwType = Type;
      pCtx->dwUsage = Usage;

      dwStatus = WN_SUCCESS;
      break;
    }
    case RESOURCE_GLOBALNET: /* All resources on the network. */
    {
      if (NetResource == NULL || NetResource->lpRemoteName == NULL) {
        /* If it is NULL or if the lpRemoteName field of the NETRESOURCE is
        * NULL,
        * the provider should enumerate the top level of its network.
        */
        pCtx->fRoot = TRUE;
      } else {
        /* Enumerate lpNetResource->lpRemoteName container, which can be only
         * the Dokan container. */
        const WCHAR *lpAfterName =
            parseServerName(NetResource->lpRemoteName, NULL, NULL);
        if (lpAfterName == NULL ||
            (*lpAfterName != L'\\' && *lpAfterName != 0)) {
          dwStatus = WN_NOT_CONTAINER;
          break;
        }

        /* Valid server name. */
        pCtx->fRoot = FALSE;
      }

      pCtx->index = 0;
      pCtx->dwScope = RESOURCE_GLOBALNET;
      pCtx->dwOriginalScope = Scope;
      pCtx->dwType = Type;
      pCtx->dwUsage = Usage;

      dwStatus = WN_SUCCESS;
      break;
    }

    case RESOURCE_CONNECTED: /* All currently connected resources. */
    case RESOURCE_CONTEXT:   /* The interpretation of this is left to the
                                provider. Treat this as RESOURCE_GLOBALNET. */
    {
      pCtx->index = 0;
      pCtx->dwScope = RESOURCE_CONNECTED;
      pCtx->dwOriginalScope = Scope;
      pCtx->dwType = Type;
      pCtx->dwUsage = Usage;
      pCtx->fRoot = FALSE; /* Actually ignored for RESOURCE_CONNECTED. */

      dwStatus = WN_SUCCESS;
      break;
    }

    default:
      DbgPrintW(L"NPOpenEnum: unsupported scope 0x%lx\n", Scope);
      dwStatus = WN_NOT_SUPPORTED;
      break;
    }
  }

  if (dwStatus != WN_SUCCESS) {
    DbgPrintW(L"NPOpenEnum: Returned error 0x%08X\n", dwStatus);
    if (pCtx) {
      HeapFree(GetProcessHeap(), 0, pCtx);
    }
  } else {
    DbgPrintW(L"NPOpenEnum: pCtx %p\n", pCtx);
    *Enum = pCtx;
  }

  return dwStatus;
}

DWORD APIENTRY NPCloseEnum(__in HANDLE Enum) {
  NPENUMCTX *pCtx = (NPENUMCTX *)Enum;

  DbgPrintW(L"NPCloseEnum: hEnum %p\n", Enum);

  if (pCtx) {
    HeapFree(GetProcessHeap(), 0, pCtx);
  }

  DbgPrintW(L"NPCloseEnum: returns\n");
  return WN_SUCCESS;
}

DWORD APIENTRY NPGetResourceParent(__in LPNETRESOURCE NetResource,
                                   __in LPVOID Buffer,
                                   __in LPDWORD BufferSize) {
  DbgPrintW(
      L"NPGetResourceParent: lpNetResource %p, lpBuffer %p, lpBufferSize %p\n",
      NetResource, Buffer, BufferSize);

  /* Construct a new NETRESOURCE which is syntactically a parent of
  * lpNetResource,
  * then call NPGetResourceInformation to actually fill the buffer.
  */
  if (!NetResource || !NetResource->lpRemoteName || !BufferSize) {
    return WN_BAD_NETNAME;
  }

  const WCHAR *lpAfterName =
      parseServerName(NetResource->lpRemoteName, NULL, NULL);
  if (lpAfterName == NULL || (*lpAfterName != L'\\' && *lpAfterName != 0)) {
    DbgPrintW(L"NPGetResourceParent: WN_BAD_NETNAME\n");
    return WN_BAD_NETNAME;
  }

  DWORD RemoteNameLength = lstrlen(NetResource->lpRemoteName);

  DWORD cbEntry = sizeof(NETRESOURCE);
  cbEntry += (RemoteNameLength + 1) * sizeof(WCHAR);

  NETRESOURCE *pParent =
      (NETRESOURCE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbEntry);

  if (!pParent) {
    return WN_OUT_OF_MEMORY;
  }

  pParent->lpRemoteName = (WCHAR *)((PBYTE)pParent + sizeof(NETRESOURCE));
  lstrcpy(pParent->lpRemoteName, NetResource->lpRemoteName);

  /* Remove last path component of the pParent->lpRemoteName. */
  WCHAR *pLastSlash = pParent->lpRemoteName + RemoteNameLength;
  if (*pLastSlash == L'\\') {
    /* \\server\share\path\, skip last slash immediately. */
    pLastSlash--;
  }

  while (pLastSlash != pParent->lpRemoteName) {
    if (*pLastSlash == L'\\') {
      break;
    }

    pLastSlash--;
  }

  DWORD dwStatus = WN_SUCCESS;

  if (pLastSlash == pParent->lpRemoteName ||
      pLastSlash == pParent->lpRemoteName + 1) {
    /* It is a leading backslash. Construct "no parent" NETRESOURCE. */
    NETRESOURCE *pNetResource = (NETRESOURCE *)Buffer;

    cbEntry = sizeof(NETRESOURCE);
    cbEntry += sizeof(DOKAN_NP_NAME); /* remote name */
    cbEntry += sizeof(DOKAN_NP_NAME); /* provider name */

    if (cbEntry > *BufferSize) {
      DbgPrintW(L"NPGetResourceParent: WN_MORE_DATA 0x%x\n", cbEntry);
      *BufferSize = cbEntry;
      dwStatus = WN_MORE_DATA;
    } else {
      memset(pNetResource, 0, sizeof(*pNetResource));

      pNetResource->dwType = RESOURCETYPE_ANY;
      pNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_NETWORK;
      pNetResource->dwUsage = RESOURCEUSAGE_CONTAINER;

      WCHAR *pStrings = (WCHAR *)((PBYTE)Buffer + *BufferSize);
      pStrings = (PWCHAR)((PBYTE)pStrings - (cbEntry - sizeof(NETRESOURCE)));

      pNetResource->lpRemoteName = pStrings;
      CopyMemory(pStrings, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));
      pStrings += sizeof(DOKAN_NP_NAME) / sizeof(WCHAR);

      pNetResource->lpProvider = pStrings;
      CopyMemory(pStrings, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));
      pStrings += sizeof(DOKAN_NP_NAME) / sizeof(WCHAR);

      DbgPrintW(L"NPGetResourceParent: no parent, strings %p/%p\n", pStrings,
                (PBYTE)Buffer + *BufferSize);
    }
  } else {
    /* Make the parent remote name and get its information. */
    *pLastSlash = 0;

    LPWSTR lpSystem = NULL;
    dwStatus = NPGetResourceInformation(pParent, Buffer, BufferSize, &lpSystem);
  }

  if (pParent) {
    HeapFree(GetProcessHeap(), 0, pParent);
  }

  return dwStatus;
}

DWORD APIENTRY NPEnumResource(__in HANDLE Enum, __in LPDWORD Count,
                              __in LPVOID Buffer, __in LPDWORD BufferSize) {
  DWORD dwStatus = WN_SUCCESS;
  NPENUMCTX *pCtx = (NPENUMCTX *)Enum;
  ULONG cbEntry = 0;

  DbgPrintW(
      L"NPEnumResource: hEnum %p, lpcCount %p, lpBuffer %p, lpBufferSize %p.\n",
      Enum, Count, Buffer, BufferSize);

  if (pCtx == NULL) {
    DbgPrintW(L"NPEnumResource: WN_BAD_HANDLE\n");
    return WN_BAD_HANDLE;
  }

  if (Count == NULL || Buffer == NULL) {
    DbgPrintW(L"NPEnumResource: WN_BAD_VALUE\n");
    return WN_BAD_VALUE;
  }

  DbgPrintW(
      L"NPEnumResource: *lpcCount 0x%x, *lpBufferSize 0x%x, pCtx->index %d\n",
      *Count, *BufferSize, pCtx->index);

  LPNETRESOURCE pNetResource = (LPNETRESOURCE)Buffer;
  ULONG cbRemaining = *BufferSize;
  ULONG cEntriesCopied = 0;
  PWCHAR pStrings = (PWCHAR)((PBYTE)Buffer + *BufferSize);
  PWCHAR pDst;

  ULONG nbRead = 0;
  DOKAN_CONTROL dokanControl[DOKAN_MAX_INSTANCES];
  if (!DokanGetMountPointList(dokanControl, DOKAN_MAX_INSTANCES, TRUE,
                              &nbRead)) {
    DbgPrintW(L"NPEnumResource DokanGetMountPointList failed\n");
    return WN_NO_MORE_ENTRIES;
  }

  while (cEntriesCopied < *Count && pCtx->index < nbRead) {
    if (wcscmp(dokanControl[pCtx->index].UNCName, L"") == 0) {
      DbgPrintW(L"NPEnumResource: end reached at index %d\n", pCtx->index);
      break;
    }

    if (pCtx->dwScope == RESOURCE_CONNECTED) {
      DbgPrintW(L"NPEnumResource: RESOURCE_CONNECTED\n");

      if (lstrlenW(dokanControl[pCtx->index].MountPoint) >
          12 /* \DosDevices\C: */) {
        /* How many bytes is needed for the current NETRESOURCE data. */
        ULONG cbRemoteName =
            (lstrlenW(dokanControl[pCtx->index].UNCName) + 1) * sizeof(WCHAR);
        cbEntry = sizeof(NETRESOURCE);
        cbEntry += 3 * sizeof(WCHAR);            /* C:\0*/
        cbEntry += sizeof(WCHAR) + cbRemoteName; /* Leading \. */
        cbEntry += sizeof(DOKAN_NP_NAME);

        if (cbEntry > cbRemaining) {
          break;
        }

        cbRemaining -= cbEntry;

        memset(pNetResource, 0, sizeof(*pNetResource));

        pNetResource->dwScope = RESOURCE_CONNECTED;
        pNetResource->dwType = RESOURCETYPE_DISK;
        pNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_SHARE;
        pNetResource->dwUsage = RESOURCEUSAGE_CONNECTABLE;

        /* Reserve the space in the string area. */
        pStrings = (PWCHAR)((PBYTE)pStrings - (cbEntry - sizeof(NETRESOURCE)));
        pDst = pStrings;

        pNetResource->lpLocalName = pDst;
        *pDst++ = dokanControl[pCtx->index].MountPoint[12];
        *pDst++ = L':';
        *pDst++ = L'\0';

        pNetResource->lpRemoteName = pDst;
        *pDst++ = L'\\';
        CopyMemory(pDst, dokanControl[pCtx->index].UNCName, cbRemoteName);
        pDst += cbRemoteName / sizeof(WCHAR);

        pNetResource->lpComment = NULL;

        pNetResource->lpProvider = pDst;
        CopyMemory(pDst, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));

        DbgPrintW(L"NPEnumResource: lpRemoteName: %ls\n",
                  pNetResource->lpRemoteName);

        cEntriesCopied++;
        pNetResource++;
      }

      pCtx->index++;
    } else if (pCtx->dwScope == RESOURCE_GLOBALNET) {
      DbgPrintW(L"NPEnumResource: RESOURCE_GLOBALNET: root %d\n", pCtx->fRoot);

      if (pCtx->fRoot) {
        WCHAR *lpServerName = NULL;
        ULONG ulServerName = 0;
        parseServerName(&dokanControl[pCtx->index].UNCName[1], &lpServerName,
                        &ulServerName);

        /* Return server.
        * Determine the space needed for this entry.
        */
        cbEntry = sizeof(NETRESOURCE);
        cbEntry +=
            (2 + ulServerName) * sizeof(WCHAR); /* \\ + the server name */
        cbEntry += sizeof(DOKAN_NP_NAME);

        if (cbEntry > cbRemaining) {
          break;
        }

        cbRemaining -= cbEntry;

        memset(pNetResource, 0, sizeof(*pNetResource));

        pNetResource->dwScope = RESOURCE_GLOBALNET;
        pNetResource->dwType = RESOURCETYPE_ANY;
        pNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_SERVER;
        pNetResource->dwUsage = RESOURCEUSAGE_CONTAINER;

        pStrings = (PWCHAR)((PBYTE)pStrings - (cbEntry - sizeof(NETRESOURCE)));
        pDst = pStrings;

        pNetResource->lpLocalName = NULL;

        pNetResource->lpRemoteName = pDst;
        *pDst++ = L'\\';
        *pDst++ = L'\\';
        CopyMemory(pDst, lpServerName, ulServerName * sizeof(WCHAR));
        pDst += ulServerName;

        pNetResource->lpComment = NULL;

        pNetResource->lpProvider = pDst;
        CopyMemory(pDst, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));

        cEntriesCopied++;

        pCtx->index++;
      } else {
        /* How many bytes is needed for the current NETRESOURCE data. */
        ULONG cbRemoteName =
            (lstrlenW(dokanControl[pCtx->index].UNCName) + 1) * sizeof(WCHAR);
        cbEntry = sizeof(NETRESOURCE);
        /* Remote name: \\ + server + \ + name. */
        cbEntry += 1 * sizeof(WCHAR) + cbRemoteName;
        cbEntry += sizeof(DOKAN_NP_NAME);

        if (cbEntry > cbRemaining) {
          break;
        }

        cbRemaining -= cbEntry;

        memset(pNetResource, 0, sizeof(*pNetResource));

        pNetResource->dwScope = pCtx->dwOriginalScope;
        pNetResource->dwType = RESOURCETYPE_DISK;
        pNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_SHARE;
        pNetResource->dwUsage = RESOURCEUSAGE_CONNECTABLE;

        pStrings = (PWCHAR)((PBYTE)pStrings - (cbEntry - sizeof(NETRESOURCE)));
        pDst = pStrings;

        pNetResource->lpLocalName = NULL;

        pNetResource->lpRemoteName = pDst;
        *pDst++ = L'\\';
        CopyMemory(pDst, dokanControl[pCtx->index].UNCName, cbRemoteName);
        pDst += cbRemoteName / sizeof(WCHAR);

        pNetResource->lpComment = NULL;

        pNetResource->lpProvider = pDst;
        CopyMemory(pDst, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));

        DbgPrintW(L"NPEnumResource: lpRemoteName: %ls\n",
                  pNetResource->lpRemoteName);

        cEntriesCopied++;
        pNetResource++;

        pCtx->index++;
      }
    } else if (pCtx->dwScope == 6) {
      DbgPrintW(L"NPEnumResource: dwScope 6\n");
      dwStatus = WN_NO_MORE_ENTRIES;
    } else {
      DbgPrintW(L"NPEnumResource: invalid dwScope 0x%x\n", pCtx->dwScope);
      return WN_BAD_HANDLE;
    }
  }

  *Count = cEntriesCopied;

  if (cEntriesCopied == 0 && dwStatus == WN_SUCCESS) {
    if (pCtx->index >= nbRead ||
        wcscmp(dokanControl[pCtx->index].UNCName, L"") == 0) {
      dwStatus = WN_NO_MORE_ENTRIES;
    } else {
      DbgPrintW(L"NPEnumResource: More Data Needed - %d\n", cbEntry);
      *BufferSize = cbEntry;
      dwStatus = WN_MORE_DATA;
    }
  }

  DbgPrintW(L"NPEnumResource: Entries returned %d, dwStatus 0x%08X\n",
            cEntriesCopied, dwStatus);
  return dwStatus;
}

DWORD APIENTRY NPGetResourceInformation(__in LPNETRESOURCE NetResource,
                                        __out LPVOID Buffer,
                                        __out LPDWORD BufferSize,
                                        __out LPWSTR *System) {
  DbgPrintW(L"NPGetResourceInformation: NetResource %p, Buffer %p, BufferSize "
            L"%p, System %p\n",
            NetResource, Buffer, BufferSize, System);

  if (NetResource == NULL || NetResource->lpRemoteName == NULL ||
      BufferSize == NULL) {
    DbgPrintW(L"NPGetResourceInformation: WN_BAD_VALUE\n");
    return WN_BAD_VALUE;
  }

  DbgPrintW(L"NPGetResourceInformation: lpRemoteName %ls, *BufferSize 0x%x\n",
            NetResource->lpRemoteName, *BufferSize);

  WCHAR *lpServerName = NULL;
  ULONG ulServerName = 0;
  const WCHAR *lpAfterName =
      parseServerName(NetResource->lpRemoteName, &lpServerName, &ulServerName);
  if (lpServerName == NULL || lpAfterName == NULL ||
      (*lpAfterName != L'\\' && *lpAfterName != 0)) {
    DbgPrintW(L"NPGetResourceInformation: WN_BAD_NETNAME\n");
    return WN_BAD_NETNAME;
  }
  DbgPrintW(L"NPGetResourceInformation: lpServerName %ls - %lu\n", lpServerName,
            ulServerName);
  DbgPrintW(L"NPGetResourceInformation: lpAfterName %ls\n", lpAfterName);

  if (NetResource->dwType != 0 && NetResource->dwType != RESOURCETYPE_DISK) {
    /* The caller passed in a nonzero dwType that does not match
    * the actual type of the network resource.
    */
    return WN_BAD_DEV_TYPE;
  }

  /*
  * If the input remote resource name was "\\server\share\dir1\dir2",
  * then the output NETRESOURCE contains information about the resource
  * "\\server\share".
  * The lpRemoteName, lpProvider, dwType, dwDisplayType, and dwUsage fields are
  * returned
  * containing values, all other fields being set to NULL.
  */
  DWORD cbEntry;
  WCHAR *pStrings = (WCHAR *)((PBYTE)Buffer + *BufferSize);
  NETRESOURCE *pNetResource = (NETRESOURCE *)Buffer;

  /* Check what kind of the resource is that by parsing path components.
  * lpAfterName points to first WCHAR after a valid server name.
  */
  if (lpAfterName[0] == 0 || lpAfterName[1] == 0) {
    DbgPrintW(L"NPGetResourceInformation: type 1\n");
    /* "\\DOKAN" or "\\DOKAN\" */
    cbEntry = sizeof(NETRESOURCE);
    cbEntry += (2 + ulServerName + 1) * sizeof(WCHAR); /* \\ + server name */
    cbEntry += sizeof(DOKAN_NP_NAME);                  /* provider name */

    if (cbEntry > *BufferSize) {
      DbgPrintW(L"NPGetResourceInformation: WN_MORE_DATA 0x%x\n", cbEntry);
      *BufferSize = cbEntry;
      return WN_MORE_DATA;
    }

    memset(pNetResource, 0, sizeof(*pNetResource));

    pNetResource->dwType = RESOURCETYPE_ANY;
    pNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_SERVER;
    pNetResource->dwUsage = RESOURCEUSAGE_CONTAINER;

    pStrings = (PWCHAR)((PBYTE)pStrings - (cbEntry - sizeof(NETRESOURCE)));

    pNetResource->lpRemoteName = pStrings;
    *pStrings++ = L'\\';
    *pStrings++ = L'\\';
    CopyMemory(pStrings, lpServerName, ulServerName * sizeof(WCHAR));
    pStrings += ulServerName;
    *pStrings++ = L'\0';

    pNetResource->lpProvider = pStrings;
    CopyMemory(pStrings, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));
    pStrings += sizeof(DOKAN_NP_NAME) / sizeof(WCHAR);

    DbgPrintW(L"NPGetResourceInformation: lpRemoteName: %ls, strings %p/%p\n",
              pNetResource->lpRemoteName, pStrings,
              (PBYTE)Buffer + *BufferSize);

    if (System) {
      *System = NULL;
    }

    return WN_SUCCESS;
  }

  /* *lpAfterName == L'\\', could be share or share + path.
  * Check if there are more path components after the share name.
  */
  const WCHAR *lp = lpAfterName + 1;
  while (*lp && *lp != L'\\') {
    lp++;
  }

  if (*lp == 0) {
    DbgPrintW(L"NPGetResourceInformation: type 2\n");
    /* It is a share only: \\dokan\share */
    cbEntry = sizeof(NETRESOURCE);
    cbEntry += (2 + ulServerName + 1) *
               sizeof(WCHAR); /* \\ + server name with trailing nul */
    cbEntry += (DWORD)((lp - lpAfterName) *
                       sizeof(WCHAR)); /* The share name with leading \\ */
    cbEntry += sizeof(DOKAN_NP_NAME);  /* provider name */

    if (cbEntry > *BufferSize) {
      DbgPrintW(L"NPGetResourceInformation: WN_MORE_DATA 0x%x\n", cbEntry);
      *BufferSize = cbEntry;
      return WN_MORE_DATA;
    }

    memset(pNetResource, 0, sizeof(*pNetResource));

    pNetResource->dwType = RESOURCETYPE_DISK;
    pNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_SHARE;
    pNetResource->dwUsage = RESOURCEUSAGE_CONNECTABLE;

    pStrings = (PWCHAR)((PBYTE)pStrings - (cbEntry - sizeof(NETRESOURCE)));

    pNetResource->lpRemoteName = pStrings;
    *pStrings++ = L'\\';
    *pStrings++ = L'\\';
    CopyMemory(pStrings, lpServerName, ulServerName * sizeof(WCHAR));
    pStrings += ulServerName;
    CopyMemory(pStrings, lpAfterName, (lp - lpAfterName + 1) * sizeof(WCHAR));
    pStrings += lp - lpAfterName + 1;

    pNetResource->lpProvider = pStrings;
    CopyMemory(pStrings, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));
    pStrings += sizeof(DOKAN_NP_NAME) / sizeof(WCHAR);

    DbgPrintW(L"NPGetResourceInformation: lpRemoteName: %ls, strings %p/%p\n",
              pNetResource->lpRemoteName, pStrings,
              (PBYTE)Buffer + *BufferSize);

    if (System) {
      *System = NULL;
    }

    return WN_SUCCESS;
  }

  /* \\dokan\share\path */
  cbEntry = sizeof(NETRESOURCE);
  cbEntry += (2 + ulServerName + 1) *
             sizeof(WCHAR); /* \\ + server name with trailing nul */
  cbEntry += (DWORD)((lp - lpAfterName) *
                     sizeof(WCHAR)); /* The share name with leading \\ */
  cbEntry += sizeof(DOKAN_NP_NAME);  /* provider name */
  cbEntry += (lstrlen(lp) + 1) * sizeof(WCHAR); /* path string for lplpSystem */

  if (cbEntry > *BufferSize) {
    DbgPrintW(L"NPGetResourceInformation: WN_MORE_DATA 0x%x\n", cbEntry);
    *BufferSize = cbEntry;
    return WN_MORE_DATA;
  }

  memset(pNetResource, 0, sizeof(*pNetResource));

  pNetResource->dwType = RESOURCETYPE_DISK;
  pNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_SHARE;
  pNetResource->dwUsage = RESOURCEUSAGE_CONNECTABLE;

  pStrings = (PWCHAR)((PBYTE)pStrings - (cbEntry - sizeof(NETRESOURCE)));

  /* The server + share. */
  pNetResource->lpRemoteName = pStrings;
  *pStrings++ = L'\\';
  *pStrings++ = L'\\';
  CopyMemory(pStrings, lpServerName, ulServerName * sizeof(WCHAR));
  pStrings += ulServerName;
  CopyMemory(pStrings, lpAfterName, (lp - lpAfterName) * sizeof(WCHAR));
  pStrings += lp - lpAfterName;
  *pStrings++ = 0;

  pNetResource->lpProvider = pStrings;
  CopyMemory(pStrings, DOKAN_NP_NAME, sizeof(DOKAN_NP_NAME));
  pStrings += sizeof(DOKAN_NP_NAME) / sizeof(WCHAR);

  if (System) {
    *System = pStrings;
  }

  lstrcpy(pStrings, lp);
  pStrings += lstrlen(lp) + 1;

  DbgPrintW(L"NPGetResourceInformation: lpRemoteName: %ls, strings %p/%p\n",
            pNetResource->lpRemoteName, pStrings, (PBYTE)Buffer + *BufferSize);
  DbgPrintW(L"NPGetResourceInformation: *System: %ls\n", (System != NULL) ? *System : L"NULL");

  return WN_SUCCESS;
}

DWORD APIENTRY NPGetUniversalName(__in LPCWSTR LocalPath, __in DWORD InfoLevel,
                                  __in LPVOID Buffer, __in LPDWORD BufferSize) {

  DWORD dwStatus;

  DWORD BufferRequired = 0;
  DWORD RemoteNameLength = 0;
  DWORD RemainingPathLength = 0;

  WCHAR LocalDrive[3];

  const WCHAR *lpRemainingPath;
  WCHAR *lpString;

  DbgPrintW(
      L"NPGetUniversalName LocalPath = %s, InfoLevel = %d, *BufferSize = %d\n",
      LocalPath, InfoLevel, *BufferSize);

  /* Check is input parameter is OK. */
  if (InfoLevel != UNIVERSAL_NAME_INFO_LEVEL &&
      InfoLevel != REMOTE_NAME_INFO_LEVEL) {
    DbgPrintW(L"NPGetUniversalName: Bad dwInfoLevel InfoLevel: %d\n",
              InfoLevel);
    return WN_BAD_LEVEL;
  }

  /* The 'LocalPath' is "X:\something". Extract the "X:" to pass to
   * NPGetConnection. */
  if (LocalPath == NULL || LocalPath[0] == 0 || LocalPath[1] != L':') {
    DbgPrintW(L"NPGetUniversalName: Bad LocalPath.\n");
    return WN_BAD_LOCALNAME;
  }

  LocalDrive[0] = LocalPath[0];
  LocalDrive[1] = LocalPath[1];
  LocalDrive[2] = 0;

  /* Length of the original path without the driver letter, including trailing
   * NULL. */
  lpRemainingPath = &LocalPath[2];
  RemainingPathLength = (DWORD)((wcslen(lpRemainingPath) + 1) * sizeof(WCHAR));

  /* Build the required structure in place of the supplied buffer. */
  if (InfoLevel == UNIVERSAL_NAME_INFO_LEVEL) {
    LPUNIVERSAL_NAME_INFOW pUniversalNameInfo = (LPUNIVERSAL_NAME_INFOW)Buffer;

    BufferRequired = sizeof(UNIVERSAL_NAME_INFOW);

    if (*BufferSize >= BufferRequired) {
      /* Enough place for the structure. */
      pUniversalNameInfo->lpUniversalName =
          (PWCHAR)((PBYTE)Buffer + sizeof(UNIVERSAL_NAME_INFOW));

      /* At least so many bytes are available for obtaining the remote name. */
      RemoteNameLength = *BufferSize - BufferRequired;
    } else {
      RemoteNameLength = 0;
    }

    /* Put the remote name directly to the buffer if possible and get the name
     * length. */
    dwStatus = NPGetConnection(
        LocalDrive,
        RemoteNameLength ? pUniversalNameInfo->lpUniversalName : NULL,
        &RemoteNameLength);

    if (dwStatus != WN_SUCCESS && dwStatus != WN_MORE_DATA) {
      if (dwStatus != WN_NOT_CONNECTED) {
        DbgPrintW(L"NPGetUniversalName: NPGetConnection returned error 0x%lx\n",
                  dwStatus);
      }
      return dwStatus;
    }

    if (RemoteNameLength < sizeof(WCHAR)) {
      DbgPrintW(L"NPGetUniversalName: Remote name is empty.\n");
      return WN_NO_NETWORK;
    }

    /* Adjust for actual remote name length. */
    BufferRequired += RemoteNameLength;

    /* And for required place for remaining path. */
    BufferRequired += RemainingPathLength;

    if (*BufferSize < BufferRequired) {
      DbgPrintW(L"NPGetUniversalName: WN_MORE_DATA BufferRequired: %d\n",
                BufferRequired);
      *BufferSize = BufferRequired;
      return WN_MORE_DATA;
    }

    /* Enough memory in the buffer. Add '\' and remaining path to the remote
     * name. */
    lpString =
        &pUniversalNameInfo->lpUniversalName[RemoteNameLength / sizeof(WCHAR)];
    lpString--; /* Trailing NULL */

    CopyMemory(lpString, lpRemainingPath, RemainingPathLength);
  } else {
    LPREMOTE_NAME_INFOW pRemoteNameInfo = (LPREMOTE_NAME_INFOW)Buffer;
    WCHAR *lpDelimiter;

    BufferRequired = sizeof(REMOTE_NAME_INFOW);

    if (*BufferSize >= BufferRequired) {
      /* Enough place for the structure. */
      pRemoteNameInfo->lpUniversalName =
          (PWCHAR)((PBYTE)Buffer + sizeof(REMOTE_NAME_INFOW));
      pRemoteNameInfo->lpConnectionName = NULL;
      pRemoteNameInfo->lpRemainingPath = NULL;

      /* At least so many bytes are available for obtaining the remote name. */
      RemoteNameLength = *BufferSize - BufferRequired;
    } else {
      RemoteNameLength = 0;
    }

    /* Put the remote name directly to the buffer if possible and get the name
     * length. */
    dwStatus = NPGetConnection(
        LocalDrive, RemoteNameLength ? pRemoteNameInfo->lpUniversalName : NULL,
        &RemoteNameLength);

    if (dwStatus != WN_SUCCESS && dwStatus != WN_MORE_DATA) {
      if (dwStatus != WN_NOT_CONNECTED) {
        DbgPrintW(L"NPGetUniversalName: NPGetConnection returned error 0x%lx\n",
                  dwStatus);
      }
      return dwStatus;
    }

    if (RemoteNameLength < sizeof(WCHAR)) {
      DbgPrintW(L"NPGetUniversalName: Remote name is empty.\n");
      return WN_NO_NETWORK;
    }

    /* Adjust for actual remote name length as a part of the universal name. */
    BufferRequired += RemoteNameLength;

    /* And for required place for remaining path as a part of the universal
     * name. */
    BufferRequired += RemainingPathLength;

    /* lpConnectionName, which is the remote name. */
    BufferRequired += RemoteNameLength;

    /* lpRemainingPath. */
    BufferRequired += RemainingPathLength;

    if (*BufferSize < BufferRequired) {
      DbgPrintW(L"NPGetUniversalName: WN_MORE_DATA BufferRequired: %d\n",
                BufferRequired);
      *BufferSize = BufferRequired;
      return WN_MORE_DATA;
    }

    /* Enough memory in the buffer. Add \ and remaining path to the remote name.
     */
    lpString =
        &pRemoteNameInfo->lpUniversalName[RemoteNameLength / sizeof(WCHAR)];
    lpString--; /* Trailing NULL */

    lpDelimiter = lpString; /* Delimiter between the remote name and the
                             * remaining path.
                                                    * May be 0 if the remaining
                             * path is empty.
                                                    */

    CopyMemory(lpString, lpRemainingPath, RemainingPathLength);
    lpString += RemainingPathLength / sizeof(WCHAR);

    *lpDelimiter = 0; /* Keep NULL terminated remote name. */

    pRemoteNameInfo->lpConnectionName = lpString;
    CopyMemory(lpString, pRemoteNameInfo->lpUniversalName, RemoteNameLength);
    lpString += RemoteNameLength / sizeof(WCHAR);

    pRemoteNameInfo->lpRemainingPath = lpString;
    CopyMemory(lpString, lpRemainingPath, RemainingPathLength);

    /* If remaining path was not empty, restore the delimiter in the universal
     * name. */
    if (RemainingPathLength > sizeof(WCHAR)) {
      *lpDelimiter = L'\\';
    }
  }

  return WN_SUCCESS;
}
