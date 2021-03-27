/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.

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

#include "../dokan.h"
#include "str.h"

const UNICODE_STRING g_DosDevicesPrefix =
    RTL_CONSTANT_STRING(L"\\DosDevices\\");
const UNICODE_STRING g_VolumeGuidPrefix =
    RTL_CONSTANT_STRING(L"\\??\\Volume{");
const UNICODE_STRING g_ObjectManagerPrefix =
    RTL_CONSTANT_STRING(L"\\??\\");

PUNICODE_STRING
DokanAllocateUnicodeString(__in PCWSTR String) {
  PUNICODE_STRING unicode;
  PWSTR buffer;
  ULONG length;
  unicode = DokanAlloc(sizeof(UNICODE_STRING));
  if (unicode == NULL) {
    return NULL;
  }

  length = (ULONG)(wcslen(String) + 1) * sizeof(WCHAR);
  buffer = DokanAlloc(length);
  if (buffer == NULL) {
    ExFreePool(unicode);
    return NULL;
  }
  RtlCopyMemory(buffer, String, length);
  NTSTATUS result = RtlUnicodeStringInitEx(unicode, buffer, 0);
  if (!NT_SUCCESS(result)) {
    DOKAN_LOG("Invalid string size received.");
    ExFreePool(buffer);
    ExFreePool(unicode);
    return NULL;
  }
  return unicode;
}

VOID DokanFreeUnicodeString(PUNICODE_STRING UnicodeString) {
  if (UnicodeString != NULL) {
    ExFreePool(UnicodeString->Buffer);
    ExFreePool(UnicodeString);
  }
}

PUNICODE_STRING DokanAllocDuplicateString(__in const PUNICODE_STRING Src) {
  PUNICODE_STRING result = DokanAllocZero(sizeof(UNICODE_STRING));
  if (!result) {
    return NULL;
  }
  if (!DokanDuplicateUnicodeString(result, Src)) {
    ExFreePool(result);
    return NULL;
  }
  return result;
}

BOOLEAN DokanDuplicateUnicodeString(__out PUNICODE_STRING Dest,
                                    __in const PUNICODE_STRING Src) {
  if (Dest->Buffer) {
    ExFreePool(Dest->Buffer);
  }
  Dest->Buffer = DokanAlloc(Src->MaximumLength);
  if (!Dest->Buffer) {
    Dest->Length = 0;
    Dest->MaximumLength = 0;
    return FALSE;
  }
  Dest->MaximumLength = Src->MaximumLength;
  Dest->Length = Src->Length;
  RtlCopyMemory(Dest->Buffer, Src->Buffer, Dest->MaximumLength);
  return TRUE;
}

BOOLEAN StartsWith(__in const UNICODE_STRING* Str,
                   __in const UNICODE_STRING* Prefix) {
  if (Prefix == NULL || Prefix->Length == 0) {
    return TRUE;
  }

  if (Str == NULL || Prefix->Length > Str->Length) {
    return FALSE;
  }

  LPCWSTR prefixToUse, stringToCompareTo;
  prefixToUse = Prefix->Buffer;
  stringToCompareTo = Str->Buffer;

  while (*prefixToUse) {
    if (*prefixToUse++ != *stringToCompareTo++)
      return FALSE;
  }

  return TRUE;
}

BOOLEAN StartsWithDosDevicesPrefix(__in const UNICODE_STRING* Str) {
  return StartsWith(Str, &g_DosDevicesPrefix);
}

BOOLEAN StartsWithVolumeGuidPrefix(__in const UNICODE_STRING* Str) {
  return StartsWith(Str, &g_VolumeGuidPrefix);
}

PUNICODE_STRING ChangePrefix(const UNICODE_STRING* Str,
                             const UNICODE_STRING* Prefix, BOOLEAN HasPrefix,
                             const UNICODE_STRING* NewPrefix) {
  PUNICODE_STRING newStr = NULL;
  BOOLEAN startWithPrefix = FALSE;
  USHORT prefixLength = 0;

  startWithPrefix = StartsWith(Str, Prefix);
  if (!startWithPrefix && HasPrefix) {
    DOKAN_LOG_("\"%wZ\" do not start with Prefix \"%wZ\"", Str, Prefix);
    return NULL;
  }

  USHORT length = Str->Length + NewPrefix->Length;
  if (startWithPrefix) {
    prefixLength = Prefix->Length;
    length -= Prefix->Length;
  }
  newStr = DokanAllocZero(sizeof(UNICODE_STRING));
  if (!newStr) {
    DOKAN_LOG("Failed to allocate unicode_string");
    return NULL;
  }
  newStr->Length = 0;
  newStr->MaximumLength = length;
  newStr->Buffer = DokanAllocZero(length);
  if (!newStr->Buffer) {
    DOKAN_LOG("Failed to allocate unicode_string buffer");
    ExFreePool(newStr);
    return NULL;
  }

  RtlUnicodeStringCopy(newStr, NewPrefix);
  UNICODE_STRING strAfterPrefix = DokanWrapUnicodeString(
      (PWCHAR)((PCHAR)Str->Buffer + prefixLength), Str->Length - prefixLength);
  RtlUnicodeStringCat(newStr, &strAfterPrefix);
  return newStr;
}

BOOLEAN IsMountPointDriveLetter(__in const UNICODE_STRING* MountPoint) {
  size_t colonIndex = g_DosDevicesPrefix.Length / sizeof(WCHAR) + 1;
  size_t driveLetterLength = g_DosDevicesPrefix.Length + 2 * sizeof(WCHAR);
  BOOLEAN nonTerminatedDriveLetterLength =
      MountPoint->Length == driveLetterLength;
  BOOLEAN nullTerminatedDriveLetterLength =
      MountPoint->Length == driveLetterLength + sizeof(WCHAR)
      && MountPoint->Buffer[colonIndex + 1] == L'\0';
  // Note: the size range is for an optional null char.
  return StartsWithDosDevicesPrefix(MountPoint)
      && (nonTerminatedDriveLetterLength || nullTerminatedDriveLetterLength)
      && MountPoint->Buffer[colonIndex] == L':';
}

ULONG DokanSearchWcharinUnicodeStringWithUlong(
    __in PUNICODE_STRING inputPUnicodeString, __in WCHAR targetWchar,
    __in ULONG offsetPosition, __in int isIgnoreTargetWchar) {
  ASSERT(inputPUnicodeString != NULL);

  if (offsetPosition > inputPUnicodeString->MaximumLength) {
    // trying to prevent BSOD for invalid input parameter
    offsetPosition = inputPUnicodeString->Length;
    // if inputPUnicodeString->Length == 0, the while loop will be skiped
    // directly. So, the return value will be 0.
  }

  // 0 > 0 will return false and end the loop
  while (offsetPosition > 0) {
    offsetPosition--;

    if (inputPUnicodeString->Buffer[offsetPosition] == targetWchar) {
      if (isIgnoreTargetWchar == 1) {
        offsetPosition++;  // the next is the beginning of filename
      }
      break;
    }
  }
  return offsetPosition;
}

LONG DokanSearchUnicodeStringChar(__in PUNICODE_STRING UnicodeString,
                                  __in WCHAR Char) {
  return DokanSearchStringChar(UnicodeString->Buffer, UnicodeString->Length,
                               Char);
}

LONG DokanSearchStringChar(__in PWCHAR String, __in ULONG Length,
                           __in WCHAR Char) {
  for (ULONG i = 0; i < Length / sizeof(WCHAR); ++i) {
    if (String[i] == Char) {
      return i;
    }
  }
  return -1;
}