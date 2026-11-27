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

#include "dokan.h"

#include <winnetwk.h>
#include <npapi.h>

#include "np_registry.h"

#define DOKAN_NP_PROVIDER_KEY                                                  \
  L"System\\CurrentControlSet\\Services\\doka"                                 \
  L"n" DOKAN_MAJOR_API_VERSION L"\\NetworkProvider"
#define DOKAN_NP_DEVICE_NAME                                                   \
  L"\\Device\\DokanRedirector" DOKAN_MAJOR_API_VERSION
#define DOKAN_NP_PATH                                                          \
  L"%SystemRoot%\\System32\\dokannp" DOKAN_MAJOR_API_VERSION L".dll"
#define DOKAN_NP_ORDER_KEY                                                     \
  L"System\\CurrentControlSet\\Control\\NetworkProvider\\Order"
#define DOKAN_NP_HWORDER_KEY                                                   \
  L"System\\CurrentControlSet\\Control\\NetworkProvider\\HwOrder"
#define DOKAN_NP_ORDER_VALUE L"ProviderOrder"

// Largest ProviderOrder value accepted, in bytes.
#define DOKAN_NP_MAX_ORDER_BYTES (1024 * 1024)

static SIZE_T NpRegLength(LPCWSTR Text) {
  SIZE_T length = 0;
  if (Text != NULL) {
    while (Text[length] != L'\0') {
      ++length;
    }
  }
  return length;
}

static BOOL NpRegIsBlank(WCHAR Char) { return Char == L' ' || Char == L'\t'; }

static VOID NpRegTrim(LPCWSTR *Token, SIZE_T *Length) {
  while (*Length > 0 && NpRegIsBlank((*Token)[0])) {
    ++*Token;
    --*Length;
  }
  while (*Length > 0 && NpRegIsBlank((*Token)[*Length - 1])) {
    --*Length;
  }
}

static BOOL NpRegTokenEquals(LPCWSTR Token, SIZE_T Length, LPCWSTR Provider,
                             SIZE_T ProviderLength) {
  NpRegTrim(&Token, &Length);
  return Length > 0 && Length == ProviderLength &&
         CompareStringOrdinal(Token, (int)Length, Provider, (int)ProviderLength,
                              /*bIgnoreCase=*/TRUE) == CSTR_EQUAL;
}

// A provider name is a single non-blank token.
static BOOL NpRegIsValidProvider(LPCWSTR Provider, SIZE_T *Length) {
  SIZE_T length = NpRegLength(Provider);
  if (length == 0 || length > DOKAN_NP_MAX_ORDER_BYTES / sizeof(WCHAR)) {
    return FALSE;
  }
  for (SIZE_T i = 0; i < length; ++i) {
    if (Provider[i] == L',' || NpRegIsBlank(Provider[i])) {
      return FALSE;
    }
  }
  *Length = length;
  return TRUE;
}

static LPWSTR NpRegAllocString(SIZE_T Length) {
  return (LPWSTR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                           (Length + 1) * sizeof(WCHAR));
}

static VOID NpRegAppend(LPWSTR Destination, SIZE_T *Position, LPCWSTR Source,
                        SIZE_T Length) {
  for (SIZE_T i = 0; i < Length; ++i) {
    Destination[*Position + i] = Source[i];
  }
  *Position += Length;
}

VOID DokanProviderOrderFree(LPWSTR Order) {
  if (Order != NULL) {
    HeapFree(GetProcessHeap(), 0, Order);
  }
}

DWORD DokanProviderOrderAdd(LPCWSTR Order, LPCWSTR Provider, LPWSTR *NewOrder) {
  SIZE_T providerLength = 0;
  if (NewOrder == NULL) {
    return ERROR_INVALID_PARAMETER;
  }
  *NewOrder = NULL;
  if (!NpRegIsValidProvider(Provider, &providerLength)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (Order == NULL) {
    Order = L"";
  }
  SIZE_T orderLength = NpRegLength(Order);

  SIZE_T start = 0;
  for (SIZE_T i = 0; i <= orderLength; ++i) {
    if (i == orderLength || Order[i] == L',') {
      if (NpRegTokenEquals(Order + start, i - start, Provider,
                           providerLength)) {
        return ERROR_SUCCESS;
      }
      start = i + 1;
    }
  }

  // Drop trailing separators and blanks so that no empty token is created.
  SIZE_T keep = orderLength;
  while (keep > 0 &&
         (Order[keep - 1] == L',' || NpRegIsBlank(Order[keep - 1]))) {
    --keep;
  }
  LPWSTR result = NpRegAllocString(keep + 1 + providerLength);
  if (result == NULL) {
    return ERROR_NOT_ENOUGH_MEMORY;
  }
  SIZE_T position = 0;
  NpRegAppend(result, &position, Order, keep);
  if (keep > 0) {
    NpRegAppend(result, &position, L",", 1);
  }
  NpRegAppend(result, &position, Provider, providerLength);
  result[position] = L'\0';
  *NewOrder = result;
  return ERROR_SUCCESS;
}

DWORD DokanProviderOrderRemove(LPCWSTR Order, LPCWSTR Provider,
                               LPWSTR *NewOrder) {
  SIZE_T providerLength = 0;
  if (NewOrder == NULL) {
    return ERROR_INVALID_PARAMETER;
  }
  *NewOrder = NULL;
  if (!NpRegIsValidProvider(Provider, &providerLength)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (Order == NULL) {
    return ERROR_SUCCESS;
  }
  SIZE_T orderLength = NpRegLength(Order);
  LPWSTR result = NpRegAllocString(orderLength);
  if (result == NULL) {
    return ERROR_NOT_ENOUGH_MEMORY;
  }

  // Keep every other non-empty token, without its surrounding blanks.
  BOOL found = FALSE;
  SIZE_T position = 0;
  SIZE_T start = 0;
  for (SIZE_T i = 0; i <= orderLength; ++i) {
    if (i == orderLength || Order[i] == L',') {
      LPCWSTR token = Order + start;
      SIZE_T tokenLength = i - start;
      LPCWSTR trimmed = token;
      SIZE_T trimmedLength = tokenLength;
      NpRegTrim(&trimmed, &trimmedLength);
      if (NpRegTokenEquals(token, tokenLength, Provider, providerLength)) {
        found = TRUE;
      } else if (trimmedLength > 0) {
        if (position > 0) {
          NpRegAppend(result, &position, L",", 1);
        }
        NpRegAppend(result, &position, trimmed, trimmedLength);
      }
      start = i + 1;
    }
  }

  if (!found) {
    DokanProviderOrderFree(result);
    return ERROR_SUCCESS;
  }
  result[position] = L'\0';
  *NewOrder = result;
  return ERROR_SUCCESS;
}

// Reads the ProviderOrder value as a NUL-terminated string.
static DWORD NpRegReadOrder(HKEY Key, LPWSTR *Value, DWORD *Type) {
  *Value = NULL;
  *Type = REG_SZ;
  // Retry if the value grows between the size query and the read.
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD type = 0;
    DWORD size = 0;
    DWORD error =
        RegQueryValueExW(Key, DOKAN_NP_ORDER_VALUE, NULL, &type, NULL, &size);
    if (error != ERROR_SUCCESS) {
      return error;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
      return ERROR_INVALID_DATATYPE;
    }
    if (size % sizeof(WCHAR) != 0 || size > DOKAN_NP_MAX_ORDER_BYTES) {
      return ERROR_INVALID_DATA;
    }

    // One extra WCHAR guarantees termination of unterminated data.
    LPWSTR buffer = NpRegAllocString(size / sizeof(WCHAR));
    if (buffer == NULL) {
      return ERROR_NOT_ENOUGH_MEMORY;
    }
    DWORD read = size;
    error = RegQueryValueExW(Key, DOKAN_NP_ORDER_VALUE, NULL, &type,
                             (LPBYTE)buffer, &read);
    if (error == ERROR_SUCCESS && type != REG_SZ && type != REG_EXPAND_SZ) {
      error = ERROR_INVALID_DATATYPE;
    }
    if (error == ERROR_SUCCESS && (read % sizeof(WCHAR) != 0 || read > size)) {
      error = ERROR_INVALID_DATA;
    }
    if (error != ERROR_SUCCESS) {
      DokanProviderOrderFree(buffer);
      if (error == ERROR_MORE_DATA) {
        continue;
      }
      return error;
    }
    buffer[read / sizeof(WCHAR)] = L'\0';
    *Value = buffer;
    *Type = type;
    return ERROR_SUCCESS;
  }
  return ERROR_MORE_DATA;
}

static DWORD NpRegUpdateOrder(HKEY Root, LPCWSTR SubKey, BOOL Add,
                              BOOL KeyRequired) {
  HKEY key = NULL;
  DWORD error =
      RegOpenKeyExW(Root, SubKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
  if (error == ERROR_FILE_NOT_FOUND && !KeyRequired) {
    return ERROR_SUCCESS;
  }
  if (error != ERROR_SUCCESS) {
    return error;
  }

  LPWSTR order = NULL;
  LPWSTR newOrder = NULL;
  DWORD type = REG_SZ;
  error = NpRegReadOrder(key, &order, &type);
  if (error == ERROR_FILE_NOT_FOUND) {
    // A missing list is an empty list.
    error = ERROR_SUCCESS;
  }
  if (error == ERROR_SUCCESS) {
    error = Add ? DokanProviderOrderAdd(order, DOKAN_NP_NAME, &newOrder)
                : DokanProviderOrderRemove(order, DOKAN_NP_NAME, &newOrder);
  }
  if (error == ERROR_SUCCESS && newOrder != NULL) {
    error = RegSetValueExW(
        key, DOKAN_NP_ORDER_VALUE, 0, type, (const BYTE *)newOrder,
        (DWORD)((NpRegLength(newOrder) + 1) * sizeof(WCHAR)));
  }

  DokanProviderOrderFree(newOrder);
  DokanProviderOrderFree(order);
  RegCloseKey(key);
  return error;
}

static DWORD NpRegSetString(HKEY Key, LPCWSTR Name, DWORD Type, LPCWSTR Value) {
  return RegSetValueExW(Key, Name, 0, Type, (const BYTE *)Value,
                        (DWORD)((NpRegLength(Value) + 1) * sizeof(WCHAR)));
}

DWORD DokanNetworkProviderInstallAt(HKEY Root) {
  if (Root == NULL) {
    return ERROR_INVALID_HANDLE;
  }

  HKEY key = NULL;
  DWORD error =
      RegCreateKeyExW(Root, DOKAN_NP_PROVIDER_KEY, 0, NULL,
                      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
  if (error != ERROR_SUCCESS) {
    return error;
  }
  DWORD providerClass = WN_NETWORK_CLASS;
  error = NpRegSetString(key, L"DeviceName", REG_SZ, DOKAN_NP_DEVICE_NAME);
  if (error == ERROR_SUCCESS) {
    error = NpRegSetString(key, L"Name", REG_SZ, DOKAN_NP_NAME);
  }
  if (error == ERROR_SUCCESS) {
    error = NpRegSetString(key, L"ProviderPath", REG_EXPAND_SZ, DOKAN_NP_PATH);
  }
  if (error == ERROR_SUCCESS) {
    error = RegSetValueExW(key, L"Class", 0, REG_DWORD,
                           (const BYTE *)&providerClass, sizeof(providerClass));
  }
  RegCloseKey(key);

  // The provider key exists before MPR can see the name in the order lists.
  if (error == ERROR_SUCCESS) {
    error = NpRegUpdateOrder(Root, DOKAN_NP_ORDER_KEY, /*Add=*/TRUE,
                             /*KeyRequired=*/TRUE);
  }
  if (error == ERROR_SUCCESS) {
    error = NpRegUpdateOrder(Root, DOKAN_NP_HWORDER_KEY, /*Add=*/TRUE,
                             /*KeyRequired=*/FALSE);
  }
  return error;
}

DWORD DokanNetworkProviderUninstallAt(HKEY Root) {
  if (Root == NULL) {
    return ERROR_INVALID_HANDLE;
  }

  // The name leaves the order lists before the provider key goes away.
  DWORD error = NpRegUpdateOrder(Root, DOKAN_NP_ORDER_KEY, /*Add=*/FALSE,
                                 /*KeyRequired=*/FALSE);
  if (error == ERROR_SUCCESS) {
    error = NpRegUpdateOrder(Root, DOKAN_NP_HWORDER_KEY, /*Add=*/FALSE,
                             /*KeyRequired=*/FALSE);
  }
  if (error == ERROR_SUCCESS) {
    error = RegDeleteKeyW(Root, DOKAN_NP_PROVIDER_KEY);
    if (error == ERROR_FILE_NOT_FOUND) {
      error = ERROR_SUCCESS;
    }
  }
  return error;
}
