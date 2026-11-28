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

#define DOKAN_NP_SERVICE_NAME L"dokan" DOKAN_MAJOR_API_VERSION
#define DOKAN_NP_PROVIDER_KEY                                                  \
  L"System\\CurrentControlSet\\Services"                                       \
  L"\\" DOKAN_NP_SERVICE_NAME L"\\NetworkProvider"
#define DOKAN_NP_DEVICE_NAME                                                   \
  L"\\Device\\DokanRedirector" DOKAN_MAJOR_API_VERSION
#define DOKAN_NP_PATH                                                          \
  L"%SystemRoot%\\System32\\dokannp" DOKAN_MAJOR_API_VERSION L".dll"
#define DOKAN_NP_ORDER_KEY                                                     \
  L"System\\CurrentControlSet\\Control\\NetworkProvider\\Order"
#define DOKAN_NP_HWORDER_KEY                                                   \
  L"System\\CurrentControlSet\\Control\\NetworkProvider\\HwOrder"
#define DOKAN_NP_ORDER_VALUE L"ProviderOrder"

// Largest ProviderOrder value accepted, in bytes and WCHARs.
#define DOKAN_NP_MAX_ORDER_BYTES (1024 * 1024)
#define DOKAN_NP_MAX_ORDER_CHARS (DOKAN_NP_MAX_ORDER_BYTES / sizeof(WCHAR))

static SIZE_T NpRegBoundedLength(LPCWSTR Text, SIZE_T Max) {
  SIZE_T length = 0;
  if (Text != NULL) {
    while (length < Max && Text[length] != L'\0') {
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

static BOOL NpRegTrimmedEquals(LPCWSTR Token, SIZE_T Length, LPCWSTR Provider,
                               SIZE_T ProviderLength) {
  return Length > 0 && Length == ProviderLength &&
         CompareStringOrdinal(Token, (int)Length, Provider, (int)ProviderLength,
                              /*bIgnoreCase=*/TRUE) == CSTR_EQUAL;
}

static BOOL NpRegTokenEquals(LPCWSTR Token, SIZE_T Length, LPCWSTR Provider,
                             SIZE_T ProviderLength) {
  NpRegTrim(&Token, &Length);
  return NpRegTrimmedEquals(Token, Length, Provider, ProviderLength);
}

// A provider name is a single non-blank token.
static BOOL NpRegIsValidProvider(LPCWSTR Provider, SIZE_T *Length) {
  SIZE_T length = NpRegBoundedLength(Provider, DOKAN_NP_MAX_ORDER_CHARS + 1);
  if (length == 0 || length > DOKAN_NP_MAX_ORDER_CHARS) {
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
  SIZE_T orderLength = NpRegBoundedLength(Order, DOKAN_NP_MAX_ORDER_CHARS + 1);
  if (orderLength > DOKAN_NP_MAX_ORDER_CHARS) {
    return ERROR_INVALID_DATA;
  }

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
  SIZE_T orderLength = NpRegBoundedLength(Order, DOKAN_NP_MAX_ORDER_CHARS + 1);
  if (orderLength > DOKAN_NP_MAX_ORDER_CHARS) {
    return ERROR_INVALID_DATA;
  }
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
      LPCWSTR trimmed = Order + start;
      SIZE_T trimmedLength = i - start;
      NpRegTrim(&trimmed, &trimmedLength);
      if (NpRegTrimmedEquals(trimmed, trimmedLength, Provider,
                             providerLength)) {
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

static DWORD NpRegSetString(HKEY Key, LPCWSTR Name, DWORD Type, LPCWSTR Value) {
  SIZE_T length = NpRegBoundedLength(Value, DOKAN_NP_MAX_ORDER_CHARS + 1);
  if (length > DOKAN_NP_MAX_ORDER_CHARS) {
    return ERROR_INVALID_DATA;
  }
  return RegSetValueExW(Key, Name, 0, Type, (const BYTE *)Value,
                        (DWORD)((length + 1) * sizeof(WCHAR)));
}

typedef struct _NP_ORDER_PLAN {
  HKEY Key;
  LPWSTR NewOrder;
  DWORD Type;
} NP_ORDER_PLAN;

static VOID NpOrderPlanClose(NP_ORDER_PLAN *Plan) {
  DokanProviderOrderFree(Plan->NewOrder);
  Plan->NewOrder = NULL;
  if (Plan->Key != NULL) {
    RegCloseKey(Plan->Key);
    Plan->Key = NULL;
  }
}

static DWORD NpOrderPlanPrepare(HKEY Root, LPCWSTR SubKey, BOOL Add,
                                BOOL KeyRequired, NP_ORDER_PLAN *Plan) {
  ZeroMemory(Plan, sizeof(*Plan));
  Plan->Type = REG_SZ;
  DWORD error = RegOpenKeyExW(Root, SubKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                              &Plan->Key);
  if (error == ERROR_FILE_NOT_FOUND && !KeyRequired) {
    return ERROR_SUCCESS;
  }
  if (error != ERROR_SUCCESS) {
    return error;
  }

  LPWSTR order = NULL;
  error = NpRegReadOrder(Plan->Key, &order, &Plan->Type);
  if (error == ERROR_FILE_NOT_FOUND) {
    // A missing list is an empty list.
    error = ERROR_SUCCESS;
  }
  if (error == ERROR_SUCCESS) {
    error =
        Add ? DokanProviderOrderAdd(order, DOKAN_NP_NAME, &Plan->NewOrder)
            : DokanProviderOrderRemove(order, DOKAN_NP_NAME, &Plan->NewOrder);
  }
  DokanProviderOrderFree(order);
  if (error != ERROR_SUCCESS) {
    NpOrderPlanClose(Plan);
  }
  return error;
}

static DWORD NpOrderPlanCommit(const NP_ORDER_PLAN *Plan) {
  if (Plan->Key == NULL || Plan->NewOrder == NULL) {
    return ERROR_SUCCESS;
  }
  return NpRegSetString(Plan->Key, DOKAN_NP_ORDER_VALUE, Plan->Type,
                        Plan->NewOrder);
}

DWORD DokanNetworkProviderInstallAt(HKEY Root) {
  if (Root == NULL) {
    return ERROR_INVALID_HANDLE;
  }

  // Validate both Order and HwOrder before mutating any key so a malformed
  // value in either list leaves the registry untouched.
  NP_ORDER_PLAN orderPlan;
  NP_ORDER_PLAN hwOrderPlan;
  ZeroMemory(&hwOrderPlan, sizeof(hwOrderPlan));
  DWORD error = NpOrderPlanPrepare(Root, DOKAN_NP_ORDER_KEY, /*Add=*/TRUE,
                                   /*KeyRequired=*/TRUE, &orderPlan);
  if (error == ERROR_SUCCESS) {
    error = NpOrderPlanPrepare(Root, DOKAN_NP_HWORDER_KEY, /*Add=*/TRUE,
                               /*KeyRequired=*/FALSE, &hwOrderPlan);
  }
  if (error != ERROR_SUCCESS) {
    NpOrderPlanClose(&hwOrderPlan);
    NpOrderPlanClose(&orderPlan);
    return error;
  }

  HKEY key = NULL;
  error =
      RegCreateKeyExW(Root, DOKAN_NP_PROVIDER_KEY, 0, NULL,
                      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
  if (error == ERROR_SUCCESS) {
    DWORD providerClass = WN_NETWORK_CLASS;
    error = NpRegSetString(key, L"DeviceName", REG_SZ, DOKAN_NP_DEVICE_NAME);
    if (error == ERROR_SUCCESS) {
      error = NpRegSetString(key, L"Name", REG_SZ, DOKAN_NP_NAME);
    }
    if (error == ERROR_SUCCESS) {
      error =
          NpRegSetString(key, L"ProviderPath", REG_EXPAND_SZ, DOKAN_NP_PATH);
    }
    if (error == ERROR_SUCCESS) {
      error =
          RegSetValueExW(key, L"Class", 0, REG_DWORD,
                         (const BYTE *)&providerClass, sizeof(providerClass));
    }
    RegCloseKey(key);
  }

  // The provider key exists before MPR can see the name in the order lists.
  if (error == ERROR_SUCCESS) {
    error = NpOrderPlanCommit(&orderPlan);
  }
  if (error == ERROR_SUCCESS) {
    error = NpOrderPlanCommit(&hwOrderPlan);
  }

  NpOrderPlanClose(&hwOrderPlan);
  NpOrderPlanClose(&orderPlan);
  return error;
}

DWORD DokanNetworkProviderUninstallAt(HKEY Root) {
  if (Root == NULL) {
    return ERROR_INVALID_HANDLE;
  }

  // The name leaves the primary Order list before the provider key goes away.
  // A corrupt optional HwOrder value is still reported, without blocking
  // cleanup of the primary Order list and NetworkProvider key.
  NP_ORDER_PLAN orderPlan;
  DWORD error = NpOrderPlanPrepare(Root, DOKAN_NP_ORDER_KEY, /*Add=*/FALSE,
                                   /*KeyRequired=*/FALSE, &orderPlan);
  if (error != ERROR_SUCCESS) {
    return error;
  }

  NP_ORDER_PLAN hwOrderPlan;
  DWORD hwError = NpOrderPlanPrepare(Root, DOKAN_NP_HWORDER_KEY, /*Add=*/FALSE,
                                     /*KeyRequired=*/FALSE, &hwOrderPlan);

  error = NpOrderPlanCommit(&orderPlan);
  if (error == ERROR_SUCCESS && hwError == ERROR_SUCCESS) {
    hwError = NpOrderPlanCommit(&hwOrderPlan);
  }
  if (error == ERROR_SUCCESS) {
    DWORD deleteError = RegDeleteKeyW(Root, DOKAN_NP_PROVIDER_KEY);
    if (deleteError != ERROR_SUCCESS && deleteError != ERROR_FILE_NOT_FOUND) {
      error = deleteError;
    }
  }

  NpOrderPlanClose(&hwOrderPlan);
  NpOrderPlanClose(&orderPlan);
  return error != ERROR_SUCCESS ? error : hwError;
}
