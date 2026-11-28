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

#include "np_mount_table.h"

#include "np_util.h"

#define DOKAN_NP_KNOWN_FLAGS (DOKAN_NP_MOUNT_CANCELABLE | DOKAN_NP_MOUNT_ALIAS)

typedef struct _DOKAN_NP_PENDING_MOUNT {
  // "\\server\share[\more]", NUL-terminated, heap allocated.
  LPWSTR RemoteName;
  ULONG RemoteNameLength;
  ULONG ServerEnd;
  WCHAR DriveLetter;
  ULONG Flags;
  // NUL-terminated, heap allocated.
  LPWSTR TransportKey;
  ULONG TransportKeyLength;
} DOKAN_NP_PENDING_MOUNT;

struct _DOKAN_NP_TABLE_BUILDER {
  DOKAN_NP_PENDING_MOUNT *Entries;
  ULONG Count;
  ULONG Capacity;
};

static PVOID NpAlloc(SIZE_T Size) {
  return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

static VOID NpFree(PVOID Memory) {
  if (Memory != NULL) {
    HeapFree(GetProcessHeap(), 0, Memory);
  }
}

static BOOL NpContainsNul(LPCWSTR Text, SIZE_T Length) {
  for (SIZE_T i = 0; i < Length; ++i) {
    if (Text[i] == L'\0') {
      return TRUE;
    }
  }
  return FALSE;
}

DWORD DokanNpTableBuilderCreate(DOKAN_NP_TABLE_BUILDER **Builder) {
  if (Builder == NULL) {
    return ERROR_INVALID_PARAMETER;
  }
  *Builder = (DOKAN_NP_TABLE_BUILDER *)NpAlloc(sizeof(DOKAN_NP_TABLE_BUILDER));
  return *Builder != NULL ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
}

VOID DokanNpTableBuilderAbort(DOKAN_NP_TABLE_BUILDER *Builder) {
  if (Builder == NULL) {
    return;
  }
  for (ULONG i = 0; i < Builder->Count; ++i) {
    NpFree(Builder->Entries[i].RemoteName);
    NpFree(Builder->Entries[i].TransportKey);
  }
  NpFree(Builder->Entries);
  NpFree(Builder);
}

// Validates UncName and computes the body of the canonical remote name (the
// part after the two leading backslashes). Returns FALSE when the name is not
// a valid "\server\share[\more]" or "\\server\share[\more]".
static BOOL NpNormalizeUncName(LPCWSTR UncName, SIZE_T UncNameLength,
                               LPCWSTR *Body, SIZE_T *BodyLength,
                               SIZE_T *ServerLength) {
  if (UncName == NULL || UncNameLength == 0 ||
      UncNameLength > DOKAN_NP_MAX_NAME_LENGTH ||
      NpContainsNul(UncName, UncNameLength)) {
    return FALSE;
  }

  SIZE_T leading = 0;
  while (leading < UncNameLength && UncName[leading] == L'\\') {
    ++leading;
  }
  if (leading != 1 && leading != 2) {
    return FALSE;
  }

  LPCWSTR body = UncName + leading;
  SIZE_T bodyLength = UncNameLength - leading;
  while (bodyLength > 0 && body[bodyLength - 1] == L'\\') {
    --bodyLength;
  }

  // Require a non-empty server, at least one more component, and no empty
  // component in between.
  SIZE_T components = 0;
  SIZE_T componentLength = 0;
  SIZE_T serverLength = 0;
  for (SIZE_T i = 0; i <= bodyLength; ++i) {
    if (i == bodyLength || body[i] == L'\\') {
      if (componentLength == 0) {
        return FALSE;
      }
      if (components == 0) {
        serverLength = componentLength;
      }
      ++components;
      componentLength = 0;
    } else {
      ++componentLength;
    }
  }
  if (components < 2) {
    return FALSE;
  }

  *Body = body;
  *BodyLength = bodyLength;
  *ServerLength = serverLength;
  return TRUE;
}

static DWORD NpBuilderGrow(DOKAN_NP_TABLE_BUILDER *Builder) {
  ULONG capacity = Builder->Capacity == 0 ? 8 : Builder->Capacity * 2;
  DOKAN_NP_PENDING_MOUNT *entries = (DOKAN_NP_PENDING_MOUNT *)NpAlloc(
      (SIZE_T)capacity * sizeof(DOKAN_NP_PENDING_MOUNT));
  if (entries == NULL) {
    return ERROR_NOT_ENOUGH_MEMORY;
  }
  for (ULONG i = 0; i < Builder->Count; ++i) {
    entries[i] = Builder->Entries[i];
  }
  NpFree(Builder->Entries);
  Builder->Entries = entries;
  Builder->Capacity = capacity;
  return ERROR_SUCCESS;
}

DWORD DokanNpTableBuilderAdd(DOKAN_NP_TABLE_BUILDER *Builder, LPCWSTR UncName,
                             SIZE_T UncNameLength, WCHAR DriveLetter,
                             ULONG Flags, LPCWSTR TransportKey,
                             SIZE_T TransportKeyLength) {
  if (Builder == NULL) {
    return ERROR_INVALID_PARAMETER;
  }

  LPCWSTR body = NULL;
  SIZE_T bodyLength = 0;
  SIZE_T serverLength = 0;
  if (!NpNormalizeUncName(UncName, UncNameLength, &body, &bodyLength,
                          &serverLength)) {
    return ERROR_INVALID_NAME;
  }

  DriveLetter = NpToUpperAscii(DriveLetter);
  if (DriveLetter != 0 && !NpIsAsciiLetter(DriveLetter)) {
    return ERROR_INVALID_NAME;
  }
  if ((Flags & ~DOKAN_NP_KNOWN_FLAGS) != 0) {
    return ERROR_INVALID_NAME;
  }
  if ((TransportKey == NULL && TransportKeyLength != 0) ||
      TransportKeyLength > DOKAN_NP_MAX_NAME_LENGTH ||
      (TransportKey != NULL &&
       NpContainsNul(TransportKey, TransportKeyLength))) {
    return ERROR_INVALID_NAME;
  }
  if (Builder->Count >= DOKAN_NP_MAX_ENTRIES) {
    return ERROR_INVALID_NAME;
  }

  if (Builder->Count == Builder->Capacity) {
    DWORD growError = NpBuilderGrow(Builder);
    if (growError != ERROR_SUCCESS) {
      return growError;
    }
  }

  // Both lengths are bounded by DOKAN_NP_MAX_NAME_LENGTH.
  SIZE_T remoteNameLength = 2 + bodyLength;
  LPWSTR remoteName = (LPWSTR)NpAlloc((remoteNameLength + 1) * sizeof(WCHAR));
  LPWSTR transportKey =
      (LPWSTR)NpAlloc((TransportKeyLength + 1) * sizeof(WCHAR));
  if (remoteName == NULL || transportKey == NULL) {
    NpFree(remoteName);
    NpFree(transportKey);
    return ERROR_NOT_ENOUGH_MEMORY;
  }
  remoteName[0] = L'\\';
  remoteName[1] = L'\\';
  NpCopyChars(remoteName + 2, body, bodyLength);
  remoteName[remoteNameLength] = L'\0';
  if (TransportKeyLength > 0) {
    NpCopyChars(transportKey, TransportKey, TransportKeyLength);
  }
  transportKey[TransportKeyLength] = L'\0';

  DOKAN_NP_PENDING_MOUNT *entry = &Builder->Entries[Builder->Count];
  entry->RemoteName = remoteName;
  entry->RemoteNameLength = (ULONG)remoteNameLength;
  entry->ServerEnd = (ULONG)(2 + serverLength);
  entry->DriveLetter = DriveLetter;
  entry->Flags = Flags;
  entry->TransportKey = transportKey;
  entry->TransportKeyLength = (ULONG)TransportKeyLength;
  ++Builder->Count;
  return ERROR_SUCCESS;
}

DWORD DokanNpTableBuilderFinish(DOKAN_NP_TABLE_BUILDER *Builder,
                                DOKAN_NP_MOUNT_TABLE **Table) {
  if (Table == NULL) {
    DokanNpTableBuilderAbort(Builder);
    return ERROR_INVALID_PARAMETER;
  }
  *Table = NULL;
  if (Builder == NULL) {
    return ERROR_INVALID_PARAMETER;
  }

  // Count <= DOKAN_NP_MAX_ENTRIES and every string is at most
  // DOKAN_NP_MAX_NAME_LENGTH + 2 WCHARs, so none of this can overflow.
  SIZE_T headerSize = FIELD_OFFSET(DOKAN_NP_MOUNT_TABLE, Entries) +
                      (SIZE_T)Builder->Count * sizeof(DOKAN_NP_MOUNT);
  if (headerSize < sizeof(DOKAN_NP_MOUNT_TABLE)) {
    headerSize = sizeof(DOKAN_NP_MOUNT_TABLE);
  }
  SIZE_T stringChars = 0;
  for (ULONG i = 0; i < Builder->Count; ++i) {
    stringChars += (SIZE_T)Builder->Entries[i].RemoteNameLength + 1;
    stringChars += (SIZE_T)Builder->Entries[i].TransportKeyLength + 1;
  }

  DOKAN_NP_MOUNT_TABLE *table =
      (DOKAN_NP_MOUNT_TABLE *)NpAlloc(headerSize + stringChars * sizeof(WCHAR));
  if (table == NULL) {
    DokanNpTableBuilderAbort(Builder);
    return ERROR_NOT_ENOUGH_MEMORY;
  }

  LPWSTR pool = (LPWSTR)((PBYTE)table + headerSize);
  for (ULONG i = 0; i < Builder->Count; ++i) {
    const DOKAN_NP_PENDING_MOUNT *pending = &Builder->Entries[i];
    DOKAN_NP_MOUNT *mount = &table->Entries[i];

    NpCopyChars(pool, pending->RemoteName,
                (SIZE_T)pending->RemoteNameLength + 1);
    mount->RemoteName = pool;
    pool += pending->RemoteNameLength + 1;

    NpCopyChars(pool, pending->TransportKey,
                (SIZE_T)pending->TransportKeyLength + 1);
    mount->TransportKey = pool;
    pool += pending->TransportKeyLength + 1;

    mount->RemoteNameLength = pending->RemoteNameLength;
    mount->ServerEnd = pending->ServerEnd;
    mount->DriveLetter = pending->DriveLetter;
    mount->Flags = pending->Flags;
  }
  table->Count = Builder->Count;

  DokanNpTableBuilderAbort(Builder);
  *Table = table;
  return ERROR_SUCCESS;
}

VOID DokanNpTableFree(DOKAN_NP_MOUNT_TABLE *Table) { NpFree(Table); }
