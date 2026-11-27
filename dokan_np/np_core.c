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

#include "np_core.h"

#include <npapi.h>

// Longest name accepted from callers, in WCHARs. Keeps every size computation
// far away from overflow.
#define NP_MAX_NAME_LENGTH 32767

#define NP_PROVIDER_NAME_LENGTH (ARRAYSIZE(DOKAN_NP_NAME) - 1)

// Kind of the items of an enumeration.
#define NP_ITEM_SERVER 1
#define NP_ITEM_MOUNT 2
#define NP_ITEM_CONNECTION 3

// Remote name supplied by a caller, validated by NpParseRemoteName.
typedef struct _NP_REMOTE_NAME {
  LPCWSTR Name;
  // WCHARs, trailing backslashes excluded.
  SIZE_T Length;
  // WCHARs in the "\\server" prefix.
  SIZE_T ServerLength;
} NP_REMOTE_NAME;

// Description of one NETRESOURCEW to write. NULL strings stay NULL.
typedef struct _NP_RESOURCE {
  DWORD Scope;
  DWORD Type;
  DWORD DisplayType;
  DWORD Usage;
  LPCWSTR LocalName;
  SIZE_T LocalNameLength;
  LPCWSTR RemoteName;
  SIZE_T RemoteNameLength;
  LPCWSTR Provider;
  SIZE_T ProviderLength;
} NP_RESOURCE;

// Enumeration handle: one snapshot and the precomputed, deduplicated list of
// entries to return.
typedef struct _NP_ENUM {
  DOKAN_NP_MOUNT_TABLE *Table;
  ULONG Kind;
  DWORD Scope;
  ULONG Next;
  ULONG ItemCount;
  ULONG Items[ANYSIZE_ARRAY];
} NP_ENUM;

static SIZE_T NpLength(LPCWSTR Text, SIZE_T Max) {
  SIZE_T length = 0;
  if (Text == NULL) {
    return 0;
  }
  while (length < Max && Text[length] != L'\0') {
    ++length;
  }
  return length;
}

static VOID NpCopyChars(LPWSTR Destination, LPCWSTR Source, SIZE_T Length) {
  // Forward copy: also correct when Destination <= Source overlap.
  for (SIZE_T i = 0; i < Length; ++i) {
    Destination[i] = Source[i];
  }
}

static BOOL NpEqualNoCase(LPCWSTR Left, SIZE_T LeftLength, LPCWSTR Right,
                          SIZE_T RightLength) {
  if (LeftLength != RightLength) {
    return FALSE;
  }
  if (LeftLength == 0) {
    return TRUE;
  }
  // Both lengths are bounded by NP_MAX_NAME_LENGTH.
  return CompareStringOrdinal(Left, (int)LeftLength, Right, (int)RightLength,
                              /*bIgnoreCase=*/TRUE) == CSTR_EQUAL;
}

static BOOL NpIsAsciiLetter(WCHAR Char) {
  return (Char >= L'a' && Char <= L'z') || (Char >= L'A' && Char <= L'Z');
}

static WCHAR NpToUpperAscii(WCHAR Char) {
  return (Char >= L'a' && Char <= L'z') ? (WCHAR)(Char - L'a' + L'A') : Char;
}

// Returns the uppercase drive letter of "X:" (or "X:\" when allowed), else 0.
static WCHAR NpParseDriveName(LPCWSTR Name, BOOL AllowTrailingBackslash) {
  SIZE_T length = NpLength(Name, 4);
  if ((length == 2 ||
       (length == 3 && AllowTrailingBackslash && Name[2] == L'\\')) &&
      NpIsAsciiLetter(Name[0]) && Name[1] == L':') {
    return NpToUpperAscii(Name[0]);
  }
  return 0;
}

// Accepts "\\server[\...]" with exactly two leading backslashes and strips
// trailing backslashes.
static BOOL NpParseRemoteName(LPCWSTR Name, NP_REMOTE_NAME *Remote) {
  SIZE_T length = NpLength(Name, NP_MAX_NAME_LENGTH + 1);
  if (length < 3 || length > NP_MAX_NAME_LENGTH || Name[0] != L'\\' ||
      Name[1] != L'\\' || Name[2] == L'\\') {
    return FALSE;
  }
  while (Name[length - 1] == L'\\') {
    --length;
  }
  SIZE_T serverLength = 2;
  while (serverLength < length && Name[serverLength] != L'\\') {
    ++serverLength;
  }
  Remote->Name = Name;
  Remote->Length = length;
  Remote->ServerLength = serverLength;
  return TRUE;
}

static BOOL NpIsProviderName(LPCWSTR Name) {
  return NpEqualNoCase(Name, NpLength(Name, NP_PROVIDER_NAME_LENGTH + 1),
                       DOKAN_NP_NAME, NP_PROVIDER_NAME_LENGTH);
}

// A NULL or empty provider means "any provider".
static BOOL NpIsOurProvider(LPCWSTR Provider) {
  return Provider == NULL || Provider[0] == L'\0' || NpIsProviderName(Provider);
}

static BOOL NpSameServer(const DOKAN_NP_MOUNT *Left,
                         const DOKAN_NP_MOUNT *Right) {
  return NpEqualNoCase(Left->RemoteName, Left->ServerEnd, Right->RemoteName,
                       Right->ServerEnd);
}

static BOOL NpSameRemoteName(const DOKAN_NP_MOUNT *Left,
                             const DOKAN_NP_MOUNT *Right) {
  return NpEqualNoCase(Left->RemoteName, Left->RemoteNameLength,
                       Right->RemoteName, Right->RemoteNameLength);
}

static const DOKAN_NP_MOUNT *NpFindDrive(const DOKAN_NP_MOUNT_TABLE *Table,
                                         WCHAR DriveLetter) {
  for (ULONG i = 0; i < Table->Count; ++i) {
    if (Table->Entries[i].DriveLetter == DriveLetter) {
      return &Table->Entries[i];
    }
  }
  return NULL;
}

static BOOL NpServerMatches(const DOKAN_NP_MOUNT *Mount,
                            const NP_REMOTE_NAME *Remote) {
  return NpEqualNoCase(Mount->RemoteName, Mount->ServerEnd, Remote->Name,
                       Remote->ServerLength);
}

static const DOKAN_NP_MOUNT *NpFindServer(const DOKAN_NP_MOUNT_TABLE *Table,
                                          const NP_REMOTE_NAME *Remote) {
  for (ULONG i = 0; i < Table->Count; ++i) {
    if (NpServerMatches(&Table->Entries[i], Remote)) {
      return &Table->Entries[i];
    }
  }
  return NULL;
}

// Longest mount whose RemoteName is Remote or a component-boundary prefix of
// it.
static const DOKAN_NP_MOUNT *NpFindMount(const DOKAN_NP_MOUNT_TABLE *Table,
                                         const NP_REMOTE_NAME *Remote) {
  const DOKAN_NP_MOUNT *best = NULL;
  for (ULONG i = 0; i < Table->Count; ++i) {
    const DOKAN_NP_MOUNT *mount = &Table->Entries[i];
    SIZE_T length = mount->RemoteNameLength;
    if (length > Remote->Length ||
        (length < Remote->Length && Remote->Name[length] != L'\\')) {
      continue;
    }
    if ((best == NULL || length > best->RemoteNameLength) &&
        NpEqualNoCase(mount->RemoteName, length, Remote->Name, length)) {
      best = mount;
    }
  }
  return best;
}

// First non-alias mount whose RemoteName is exactly Remote.
static const DOKAN_NP_MOUNT *
NpFindPrimaryMount(const DOKAN_NP_MOUNT_TABLE *Table,
                   const NP_REMOTE_NAME *Remote) {
  for (ULONG i = 0; i < Table->Count; ++i) {
    const DOKAN_NP_MOUNT *mount = &Table->Entries[i];
    if ((mount->Flags & DOKAN_NP_MOUNT_ALIAS) == 0 &&
        NpEqualNoCase(mount->RemoteName, mount->RemoteNameLength, Remote->Name,
                      Remote->Length)) {
      return mount;
    }
  }
  return NULL;
}

static DWORD NpQueryMounts(const DOKAN_NP_TRANSPORT *Transport,
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

static SIZE_T NpStringBytes(LPCWSTR Text, SIZE_T Length) {
  return Text != NULL ? (Length + 1) * sizeof(WCHAR) : 0;
}

static SIZE_T NpResourceStringBytes(const NP_RESOURCE *Resource) {
  return NpStringBytes(Resource->LocalName, Resource->LocalNameLength) +
         NpStringBytes(Resource->RemoteName, Resource->RemoteNameLength) +
         NpStringBytes(Resource->Provider, Resource->ProviderLength);
}

static LPWSTR NpPutString(LPWSTR *Cursor, LPCWSTR Text, SIZE_T Length) {
  if (Text == NULL) {
    return NULL;
  }
  LPWSTR destination = *Cursor;
  NpCopyChars(destination, Text, Length);
  destination[Length] = L'\0';
  *Cursor += Length + 1;
  return destination;
}

// Writes Resource into Out with its strings at Strings, which must have room
// for NpResourceStringBytes(Resource). Returns the end of the strings.
static LPWSTR NpFillResource(NETRESOURCEW *Out, const NP_RESOURCE *Resource,
                             LPWSTR Strings) {
  LPWSTR cursor = Strings;
  ZeroMemory(Out, sizeof(*Out));
  Out->dwScope = Resource->Scope;
  Out->dwType = Resource->Type;
  Out->dwDisplayType = Resource->DisplayType;
  Out->dwUsage = Resource->Usage;
  Out->lpLocalName =
      NpPutString(&cursor, Resource->LocalName, Resource->LocalNameLength);
  Out->lpRemoteName =
      NpPutString(&cursor, Resource->RemoteName, Resource->RemoteNameLength);
  Out->lpComment = NULL;
  Out->lpProvider =
      NpPutString(&cursor, Resource->Provider, Resource->ProviderLength);
  return cursor;
}

static VOID NpDescribeServer(const DOKAN_NP_MOUNT *Mount, DWORD Scope,
                             NP_RESOURCE *Resource) {
  ZeroMemory(Resource, sizeof(*Resource));
  Resource->Scope = Scope;
  Resource->Type = RESOURCETYPE_ANY;
  Resource->DisplayType = RESOURCEDISPLAYTYPE_SERVER;
  Resource->Usage = RESOURCEUSAGE_CONTAINER;
  Resource->RemoteName = Mount->RemoteName;
  Resource->RemoteNameLength = Mount->ServerEnd;
  Resource->Provider = DOKAN_NP_NAME;
  Resource->ProviderLength = NP_PROVIDER_NAME_LENGTH;
}

static VOID NpDescribeMount(const DOKAN_NP_MOUNT *Mount, DWORD Scope,
                            NP_RESOURCE *Resource) {
  ZeroMemory(Resource, sizeof(*Resource));
  Resource->Scope = Scope;
  Resource->Type = RESOURCETYPE_DISK;
  Resource->DisplayType = RESOURCEDISPLAYTYPE_SHARE;
  Resource->Usage = RESOURCEUSAGE_CONNECTABLE;
  Resource->RemoteName = Mount->RemoteName;
  Resource->RemoteNameLength = Mount->RemoteNameLength;
  Resource->Provider = DOKAN_NP_NAME;
  Resource->ProviderLength = NP_PROVIDER_NAME_LENGTH;
}

// LocalNameBuffer must outlive Resource.
static VOID NpDescribeConnection(const DOKAN_NP_MOUNT *Mount,
                                 WCHAR LocalNameBuffer[3],
                                 NP_RESOURCE *Resource) {
  NpDescribeMount(Mount, RESOURCE_CONNECTED, Resource);
  if (Mount->DriveLetter != 0) {
    LocalNameBuffer[0] = Mount->DriveLetter;
    LocalNameBuffer[1] = L':';
    LocalNameBuffer[2] = L'\0';
    Resource->LocalName = LocalNameBuffer;
    Resource->LocalNameLength = 2;
  }
}

// The top of the provider's browse hierarchy: no parent on the network.
static VOID NpDescribeNoParent(NP_RESOURCE *Resource) {
  ZeroMemory(Resource, sizeof(*Resource));
  Resource->Type = RESOURCETYPE_ANY;
  Resource->DisplayType = RESOURCEDISPLAYTYPE_NETWORK;
  Resource->Usage = RESOURCEUSAGE_CONTAINER;
  Resource->Provider = DOKAN_NP_NAME;
  Resource->ProviderLength = NP_PROVIDER_NAME_LENGTH;
}

// Writes one NETRESOURCEW followed by its strings and, when System is not
// NULL, the System string.
static DWORD NpWriteSingleResource(const NP_RESOURCE *Resource, LPCWSTR System,
                                   SIZE_T SystemLength, LPVOID Buffer,
                                   LPDWORD BufferSize, LPWSTR *SystemOut) {
  SIZE_T required = sizeof(NETRESOURCEW) + NpResourceStringBytes(Resource) +
                    NpStringBytes(System, SystemLength);
  if (Buffer == NULL || *BufferSize < required) {
    // Bounded by a few NP_MAX_NAME_LENGTH strings.
    *BufferSize = (DWORD)required;
    return WN_MORE_DATA;
  }
  NETRESOURCEW *out = (NETRESOURCEW *)Buffer;
  LPWSTR cursor = NpFillResource(out, Resource, (LPWSTR)(out + 1));
  LPWSTR system = NpPutString(&cursor, System, SystemLength);
  if (SystemOut != NULL) {
    *SystemOut = system;
  }
  return WN_SUCCESS;
}

static BOOL NpIsDiskType(DWORD Type) {
  return Type == RESOURCETYPE_ANY || Type == RESOURCETYPE_DISK;
}

DWORD DokanNpGetCaps(DWORD Index) {
  switch (Index) {
  case WNNC_SPEC_VERSION:
    return WNNC_SPEC_VERSION51;
  case WNNC_NET_TYPE:
    return WNNC_NET_RDR2SAMPLE;
  case WNNC_DRIVER_VERSION:
    return 1;
  case WNNC_USER:
    return 0;
  case WNNC_CONNECTION:
    return WNNC_CON_GETCONNECTIONS | WNNC_CON_CANCELCONNECTION |
           WNNC_CON_ADDCONNECTION | WNNC_CON_ADDCONNECTION3;
  case WNNC_DIALOG:
    return WNNC_DLG_GETRESOURCEPARENT | WNNC_DLG_GETRESOURCEINFORMATION |
           WNNC_DLG_FORMATNETWORKNAME;
  case WNNC_ADMIN:
    return 0;
  case WNNC_ENUMERATION:
    return WNNC_ENUM_GLOBAL | WNNC_ENUM_LOCAL | WNNC_ENUM_CONTEXT;
  case WNNC_START:
    return WNNC_WAIT_FOR_START;
  default:
    return 0;
  }
}

DWORD DokanNpAddConnection(const DOKAN_NP_TRANSPORT *Transport,
                           LPNETRESOURCEW NetResource, LPWSTR Password,
                           LPWSTR UserName) {
  return DokanNpAddConnection3(Transport, NULL, NetResource, Password, UserName,
                               0);
}

DWORD DokanNpAddConnection3(const DOKAN_NP_TRANSPORT *Transport, HWND Owner,
                            LPNETRESOURCEW NetResource, LPWSTR Password,
                            LPWSTR UserName, DWORD Flags) {
  // Connections only target shares that are already mounted: no credentials,
  // no user interface.
  UNREFERENCED_PARAMETER(Owner);
  UNREFERENCED_PARAMETER(Password);
  UNREFERENCED_PARAMETER(UserName);

  if (NetResource == NULL || NetResource->lpRemoteName == NULL) {
    return WN_BAD_NETNAME;
  }
  if (!NpIsOurProvider(NetResource->lpProvider)) {
    return WN_BAD_PROVIDER;
  }
  NP_REMOTE_NAME remote;
  if (!NpParseRemoteName(NetResource->lpRemoteName, &remote)) {
    return WN_BAD_NETNAME;
  }

  DOKAN_NP_MOUNT_TABLE *table = NULL;
  DWORD status = NpQueryMounts(Transport, &table);
  if (status != WN_SUCCESS) {
    return status;
  }

  const DOKAN_NP_MOUNT *target = NpFindPrimaryMount(table, &remote);
  LPCWSTR localName = NetResource->lpLocalName;
  WCHAR driveLetter = 0;
  if (target == NULL) {
    status = WN_BAD_NETNAME;
  } else if (!NpIsDiskType(NetResource->dwType)) {
    status = WN_BAD_DEV_TYPE;
  } else if (localName == NULL || localName[0] == L'\0') {
    // The share is already reachable through its UNC name.
    status = (Flags & CONNECT_REDIRECT) ? WN_NOT_SUPPORTED : WN_SUCCESS;
  } else if ((driveLetter = NpParseDriveName(localName,
                                             /*AllowTrailingBackslash=*/
                                             FALSE)) == 0) {
    status = WN_BAD_LOCALNAME;
  } else if (NpFindDrive(table, driveLetter) != NULL) {
    status = WN_ALREADY_CONNECTED;
  } else if (Transport->AddDriveAlias == NULL) {
    status = WN_NOT_SUPPORTED;
  } else {
    status = Transport->AddDriveAlias(Transport->Context, target, driveLetter);
  }

  DokanNpTableFree(table);
  return status;
}

DWORD DokanNpCancelConnection(const DOKAN_NP_TRANSPORT *Transport, LPWSTR Name,
                              BOOL Force) {
  if (Name == NULL || Name[0] == L'\0') {
    return WN_NOT_CONNECTED;
  }
  WCHAR driveLetter = NpParseDriveName(Name, /*AllowTrailingBackslash=*/TRUE);
  NP_REMOTE_NAME remote;
  ZeroMemory(&remote, sizeof(remote));
  if (driveLetter == 0 && !NpParseRemoteName(Name, &remote)) {
    return WN_NOT_CONNECTED;
  }

  DOKAN_NP_MOUNT_TABLE *table = NULL;
  DWORD status = NpQueryMounts(Transport, &table);
  if (status != WN_SUCCESS) {
    return status == WN_OUT_OF_MEMORY ? status : WN_NOT_CONNECTED;
  }

  const DOKAN_NP_MOUNT *mount = driveLetter != 0
                                    ? NpFindDrive(table, driveLetter)
                                    : NpFindPrimaryMount(table, &remote);
  if (mount == NULL) {
    status = WN_NOT_CONNECTED;
  } else if ((mount->Flags & DOKAN_NP_MOUNT_CANCELABLE) == 0) {
    status = WN_ACCESS_DENIED;
  } else if (Transport->CancelMount == NULL) {
    status = WN_NOT_SUPPORTED;
  } else {
    status = Transport->CancelMount(Transport->Context, mount, Force);
  }

  DokanNpTableFree(table);
  return status;
}

DWORD DokanNpGetConnection(const DOKAN_NP_TRANSPORT *Transport,
                           LPWSTR LocalName, LPWSTR RemoteName,
                           LPDWORD BufferSize) {
  if (BufferSize == NULL) {
    return WN_BAD_VALUE;
  }
  WCHAR driveLetter =
      NpParseDriveName(LocalName, /*AllowTrailingBackslash=*/FALSE);
  if (driveLetter == 0) {
    return WN_NOT_CONNECTED;
  }

  DOKAN_NP_MOUNT_TABLE *table = NULL;
  DWORD status = NpQueryMounts(Transport, &table);
  if (status != WN_SUCCESS) {
    return status == WN_OUT_OF_MEMORY ? status : WN_NOT_CONNECTED;
  }

  const DOKAN_NP_MOUNT *mount = NpFindDrive(table, driveLetter);
  if (mount == NULL) {
    status = WN_NOT_CONNECTED;
  } else {
    DWORD required = mount->RemoteNameLength + 1;
    if (RemoteName == NULL || *BufferSize < required) {
      *BufferSize = required;
      status = WN_MORE_DATA;
    } else {
      NpCopyChars(RemoteName, mount->RemoteName, required);
      status = WN_SUCCESS;
    }
  }

  DokanNpTableFree(table);
  return status;
}

DWORD DokanNpOpenEnum(const DOKAN_NP_TRANSPORT *Transport, DWORD Scope,
                      DWORD Type, DWORD Usage, LPNETRESOURCEW NetResource,
                      LPHANDLE Enum) {
  if (Enum == NULL) {
    return WN_BAD_VALUE;
  }
  *Enum = NULL;

  if (Usage == 0) {
    Usage = RESOURCEUSAGE_CONNECTABLE | RESOURCEUSAGE_CONTAINER;
  }
  BOOL wantDisk = Type == RESOURCETYPE_ANY || (Type & RESOURCETYPE_DISK) != 0;

  ULONG kind = 0;
  NP_REMOTE_NAME server;
  ZeroMemory(&server, sizeof(server));
  switch (Scope) {
  case RESOURCE_CONNECTED:
    kind = NP_ITEM_CONNECTION;
    break;
  case RESOURCE_CONTEXT:
    // The caller's network context has no container argument.
    if (NetResource != NULL) {
      return NpIsOurProvider(NetResource->lpProvider) ? WN_BAD_VALUE
                                                      : WN_BAD_PROVIDER;
    }
    kind = NP_ITEM_SERVER;
    break;
  case RESOURCE_GLOBALNET:
    if (NetResource != NULL && !NpIsOurProvider(NetResource->lpProvider)) {
      return WN_BAD_PROVIDER;
    }
    if (NetResource == NULL || NetResource->lpRemoteName == NULL ||
        NetResource->lpRemoteName[0] == L'\0' ||
        NpIsProviderName(NetResource->lpRemoteName)) {
      kind = NP_ITEM_SERVER;
    } else if (NpParseRemoteName(NetResource->lpRemoteName, &server)) {
      kind = NP_ITEM_MOUNT;
    } else {
      return WN_BAD_NETNAME;
    }
    break;
  default:
    return WN_NOT_SUPPORTED;
  }

  DOKAN_NP_MOUNT_TABLE *table = NULL;
  DWORD status = NpQueryMounts(Transport, &table);
  if (status != WN_SUCCESS) {
    return status;
  }

  if (kind == NP_ITEM_MOUNT) {
    if (server.ServerLength != server.Length) {
      // A mount (or a path below one) is not a container of the network.
      status = NpFindMount(table, &server) != NULL ? WN_NOT_CONTAINER
                                                   : WN_BAD_NETNAME;
    } else if (NpFindServer(table, &server) == NULL) {
      status = WN_BAD_NETNAME;
    }
    if (status != WN_SUCCESS) {
      DokanNpTableFree(table);
      return status;
    }
  }

  NP_ENUM *context = (NP_ENUM *)HeapAlloc(
      GetProcessHeap(), HEAP_ZERO_MEMORY,
      FIELD_OFFSET(NP_ENUM, Items) +
          (SIZE_T)(table->Count > 0 ? table->Count : 1) * sizeof(ULONG));
  if (context == NULL) {
    DokanNpTableFree(table);
    return WN_OUT_OF_MEMORY;
  }
  context->Table = table;
  context->Kind = kind;
  context->Scope = Scope;

  for (ULONG i = 0; i < table->Count; ++i) {
    const DOKAN_NP_MOUNT *mount = &table->Entries[i];
    BOOL include = wantDisk;
    if (kind == NP_ITEM_SERVER) {
      include = include && (Usage & RESOURCEUSAGE_CONTAINER) != 0;
    } else if (kind == NP_ITEM_MOUNT) {
      include = include && (Usage & RESOURCEUSAGE_CONNECTABLE) != 0 &&
                NpServerMatches(mount, &server);
    } else if (mount->DriveLetter == 0) {
      // Driveless connections are listed once per remote name, aliases never.
      include = include && (mount->Flags & DOKAN_NP_MOUNT_ALIAS) == 0;
    }
    for (ULONG j = 0; include && j < i; ++j) {
      const DOKAN_NP_MOUNT *previous = &table->Entries[j];
      if (kind == NP_ITEM_SERVER) {
        include = !NpSameServer(mount, previous);
      } else if (kind == NP_ITEM_MOUNT) {
        include = !NpSameRemoteName(mount, previous);
      } else if (mount->DriveLetter != 0) {
        include = previous->DriveLetter != mount->DriveLetter;
      } else {
        include = previous->DriveLetter != 0 ||
                  (previous->Flags & DOKAN_NP_MOUNT_ALIAS) != 0 ||
                  !NpSameRemoteName(mount, previous);
      }
    }
    if (include) {
      context->Items[context->ItemCount++] = i;
    }
  }

  *Enum = (HANDLE)context;
  return WN_SUCCESS;
}

DWORD DokanNpEnumResource(HANDLE Enum, LPDWORD Count, LPVOID Buffer,
                          LPDWORD BufferSize) {
  NP_ENUM *context = (NP_ENUM *)Enum;
  if (context == NULL) {
    return WN_BAD_HANDLE;
  }
  if (Count == NULL || BufferSize == NULL) {
    return WN_BAD_VALUE;
  }

  DWORD requested = *Count;
  *Count = 0;
  if (requested == 0) {
    return WN_SUCCESS;
  }
  if (context->Next >= context->ItemCount) {
    return WN_NO_MORE_ENTRIES;
  }

  // NETRESOURCEW entries are packed from the start of the buffer and their
  // strings from its WCHAR-aligned end, downwards.
  PBYTE base = (PBYTE)Buffer;
  ULONG_PTR start = (ULONG_PTR)base;
  ULONG_PTR tail = 0;
  if (base != NULL) {
    tail = (start + *BufferSize) & ~(ULONG_PTR)(sizeof(WCHAR) - 1);
  }
  SIZE_T front = 0;
  DWORD copied = 0;
  SIZE_T firstRequired = 0;

  while (copied < requested && context->Next < context->ItemCount) {
    const DOKAN_NP_MOUNT *mount =
        &context->Table->Entries[context->Items[context->Next]];
    NP_RESOURCE resource;
    WCHAR localName[3] = {0};
    if (context->Kind == NP_ITEM_SERVER) {
      NpDescribeServer(mount, context->Scope, &resource);
    } else if (context->Kind == NP_ITEM_MOUNT) {
      NpDescribeMount(mount, context->Scope, &resource);
    } else {
      NpDescribeConnection(mount, localName, &resource);
    }

    SIZE_T stringBytes = NpResourceStringBytes(&resource);
    SIZE_T needed = sizeof(NETRESOURCEW) + stringBytes;
    SIZE_T available = 0;
    if (base != NULL && tail >= start + front) {
      available = tail - (start + front);
    }
    if (needed > available) {
      firstRequired = needed;
      break;
    }

    tail -= stringBytes;
    NpFillResource((NETRESOURCEW *)(base + front), &resource, (LPWSTR)tail);
    front += sizeof(NETRESOURCEW);
    ++copied;
    ++context->Next;
  }

  if (copied == 0) {
    // Bounded by a few NP_MAX_NAME_LENGTH strings.
    *BufferSize = (DWORD)firstRequired;
    return WN_MORE_DATA;
  }
  *Count = copied;
  return WN_SUCCESS;
}

DWORD DokanNpCloseEnum(HANDLE Enum) {
  NP_ENUM *context = (NP_ENUM *)Enum;
  if (context == NULL) {
    return WN_BAD_HANDLE;
  }
  DokanNpTableFree(context->Table);
  HeapFree(GetProcessHeap(), 0, context);
  return WN_SUCCESS;
}

DWORD DokanNpGetUniversalName(const DOKAN_NP_TRANSPORT *Transport,
                              LPCWSTR LocalPath, DWORD InfoLevel, LPVOID Buffer,
                              LPDWORD BufferSize) {
  if (BufferSize == NULL) {
    return WN_BAD_VALUE;
  }
  if (InfoLevel != UNIVERSAL_NAME_INFO_LEVEL &&
      InfoLevel != REMOTE_NAME_INFO_LEVEL) {
    return WN_BAD_LEVEL;
  }
  if (LocalPath == NULL || !NpIsAsciiLetter(LocalPath[0]) ||
      LocalPath[1] != L':' ||
      (LocalPath[2] != L'\0' && LocalPath[2] != L'\\')) {
    return WN_BAD_LOCALNAME;
  }
  LPCWSTR tail = LocalPath + 2;
  SIZE_T tailLength = NpLength(tail, NP_MAX_NAME_LENGTH + 1);
  if (tailLength > NP_MAX_NAME_LENGTH) {
    return WN_BAD_LOCALNAME;
  }

  DOKAN_NP_MOUNT_TABLE *table = NULL;
  DWORD status = NpQueryMounts(Transport, &table);
  if (status != WN_SUCCESS) {
    return status == WN_OUT_OF_MEMORY ? status : WN_NOT_CONNECTED;
  }

  const DOKAN_NP_MOUNT *mount =
      NpFindDrive(table, NpToUpperAscii(LocalPath[0]));
  if (mount == NULL) {
    DokanNpTableFree(table);
    return WN_NOT_CONNECTED;
  }

  SIZE_T remoteLength = mount->RemoteNameLength;
  SIZE_T universalChars = remoteLength + tailLength + 1;
  SIZE_T required;
  if (InfoLevel == UNIVERSAL_NAME_INFO_LEVEL) {
    required = sizeof(UNIVERSAL_NAME_INFOW) + universalChars * sizeof(WCHAR);
  } else {
    required = sizeof(REMOTE_NAME_INFOW) +
               (universalChars + (remoteLength + 1) + (tailLength + 1)) *
                   sizeof(WCHAR);
  }

  if (Buffer == NULL || *BufferSize < required) {
    // Bounded by a few NP_MAX_NAME_LENGTH strings.
    *BufferSize = (DWORD)required;
    status = WN_MORE_DATA;
  } else if (InfoLevel == UNIVERSAL_NAME_INFO_LEVEL) {
    UNIVERSAL_NAME_INFOW *info = (UNIVERSAL_NAME_INFOW *)Buffer;
    LPWSTR universal = (LPWSTR)(info + 1);
    NpCopyChars(universal, mount->RemoteName, remoteLength);
    NpCopyChars(universal + remoteLength, tail, tailLength);
    universal[remoteLength + tailLength] = L'\0';
    info->lpUniversalName = universal;
    status = WN_SUCCESS;
  } else {
    REMOTE_NAME_INFOW *info = (REMOTE_NAME_INFOW *)Buffer;
    LPWSTR universal = (LPWSTR)(info + 1);
    NpCopyChars(universal, mount->RemoteName, remoteLength);
    NpCopyChars(universal + remoteLength, tail, tailLength);
    universal[remoteLength + tailLength] = L'\0';
    LPWSTR cursor = universal + universalChars;
    info->lpUniversalName = universal;
    info->lpConnectionName =
        NpPutString(&cursor, mount->RemoteName, remoteLength);
    info->lpRemainingPath = NpPutString(&cursor, tail, tailLength);
    status = WN_SUCCESS;
  }

  DokanNpTableFree(table);
  return status;
}

DWORD DokanNpGetResourceParent(const DOKAN_NP_TRANSPORT *Transport,
                               LPNETRESOURCEW NetResource, LPVOID Buffer,
                               LPDWORD BufferSize) {
  if (NetResource == NULL || BufferSize == NULL) {
    return WN_BAD_VALUE;
  }
  if (!NpIsOurProvider(NetResource->lpProvider)) {
    return WN_BAD_PROVIDER;
  }
  if (NetResource->lpRemoteName == NULL) {
    return WN_BAD_NETNAME;
  }

  NP_RESOURCE parent;
  ZeroMemory(&parent, sizeof(parent));
  if (NpIsProviderName(NetResource->lpRemoteName)) {
    NpDescribeNoParent(&parent);
    return NpWriteSingleResource(&parent, NULL, 0, Buffer, BufferSize, NULL);
  }

  NP_REMOTE_NAME remote;
  if (!NpParseRemoteName(NetResource->lpRemoteName, &remote)) {
    return WN_BAD_NETNAME;
  }

  DOKAN_NP_MOUNT_TABLE *table = NULL;
  DWORD status = NpQueryMounts(Transport, &table);
  if (status != WN_SUCCESS) {
    return status == WN_OUT_OF_MEMORY ? status : WN_BAD_NETNAME;
  }

  // The input may live in Buffer: decide everything before writing.
  status = WN_SUCCESS;
  if (remote.ServerLength == remote.Length) {
    if (NpFindServer(table, &remote) != NULL) {
      NpDescribeNoParent(&parent);
    } else {
      status = WN_BAD_NETNAME;
    }
  } else {
    const DOKAN_NP_MOUNT *mount = NpFindMount(table, &remote);
    if (mount == NULL) {
      status = WN_BAD_NETNAME;
    } else if (mount->RemoteNameLength == remote.Length) {
      NpDescribeServer(mount, 0, &parent);
    } else {
      NpDescribeMount(mount, 0, &parent);
    }
  }
  if (status == WN_SUCCESS) {
    status = NpWriteSingleResource(&parent, NULL, 0, Buffer, BufferSize, NULL);
  }

  DokanNpTableFree(table);
  return status;
}

DWORD DokanNpGetResourceInformation(const DOKAN_NP_TRANSPORT *Transport,
                                    LPNETRESOURCEW NetResource, LPVOID Buffer,
                                    LPDWORD BufferSize, LPWSTR *System) {
  if (System != NULL) {
    *System = NULL;
  }
  if (NetResource == NULL || NetResource->lpRemoteName == NULL ||
      BufferSize == NULL) {
    return WN_BAD_VALUE;
  }
  if (!NpIsOurProvider(NetResource->lpProvider)) {
    return WN_BAD_PROVIDER;
  }
  NP_REMOTE_NAME remote;
  if (!NpParseRemoteName(NetResource->lpRemoteName, &remote)) {
    return WN_BAD_NETNAME;
  }
  DWORD type = NetResource->dwType;

  DOKAN_NP_MOUNT_TABLE *table = NULL;
  DWORD status = NpQueryMounts(Transport, &table);
  if (status != WN_SUCCESS) {
    return status == WN_OUT_OF_MEMORY ? status : WN_BAD_NETNAME;
  }

  NP_RESOURCE resource;
  ZeroMemory(&resource, sizeof(resource));
  LPWSTR remainder = NULL;
  SIZE_T remainderLength = 0;
  if (remote.ServerLength == remote.Length) {
    const DOKAN_NP_MOUNT *mount = NpFindServer(table, &remote);
    if (mount == NULL) {
      status = WN_BAD_NETNAME;
    } else {
      NpDescribeServer(mount, 0, &resource);
    }
  } else {
    const DOKAN_NP_MOUNT *mount = NpFindMount(table, &remote);
    if (mount == NULL) {
      status = WN_BAD_NETNAME;
    } else {
      NpDescribeMount(mount, 0, &resource);
      remainderLength = remote.Length - mount->RemoteNameLength;
      if (remainderLength > 0) {
        // The input may live in Buffer: copy the remainder out first.
        remainder = (LPWSTR)HeapAlloc(GetProcessHeap(), 0,
                                      (remainderLength + 1) * sizeof(WCHAR));
        if (remainder == NULL) {
          status = WN_OUT_OF_MEMORY;
        } else {
          NpCopyChars(remainder, remote.Name + mount->RemoteNameLength,
                      remainderLength);
          remainder[remainderLength] = L'\0';
        }
      }
    }
  }
  if (status == WN_SUCCESS && !NpIsDiskType(type)) {
    status = WN_BAD_DEV_TYPE;
  }
  if (status == WN_SUCCESS) {
    status = NpWriteSingleResource(&resource, remainder, remainderLength,
                                   Buffer, BufferSize, System);
  }

  if (remainder != NULL) {
    HeapFree(GetProcessHeap(), 0, remainder);
  }
  DokanNpTableFree(table);
  return status;
}

DWORD DokanNpFormatNetworkName(LPWSTR RemoteName, LPWSTR FormattedName,
                               LPDWORD Length, DWORD Flags,
                               DWORD AveCharPerLine) {
  UNREFERENCED_PARAMETER(AveCharPerLine);
  if (RemoteName == NULL || Length == NULL) {
    return WN_BAD_VALUE;
  }
  SIZE_T length = NpLength(RemoteName, NP_MAX_NAME_LENGTH + 1);
  if (length > NP_MAX_NAME_LENGTH) {
    return WN_BAD_NETNAME;
  }

  LPCWSTR name = RemoteName;
  if (Flags & WNFMT_INENUM) {
    // Inside an enumeration only the last component is shown.
    SIZE_T end = length;
    while (end > 0 && RemoteName[end - 1] == L'\\') {
      --end;
    }
    SIZE_T begin = end;
    while (begin > 0 && RemoteName[begin - 1] != L'\\') {
      --begin;
    }
    if (begin < end) {
      name = RemoteName + begin;
      length = end - begin;
    }
  }

  DWORD required = (DWORD)length + 1;
  if (FormattedName == NULL || *Length < required) {
    *Length = required;
    return WN_MORE_DATA;
  }
  NpCopyChars(FormattedName, name, length);
  FormattedName[length] = L'\0';
  return WN_SUCCESS;
}
