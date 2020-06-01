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

#ifndef STR_H_
#define STR_H_

#include <ntifs.h>

// Global string resources
extern const UNICODE_STRING g_DosDevicesPrefix;
extern const UNICODE_STRING g_VolumeGuidPrefix;
extern const UNICODE_STRING g_ObjectManagerPrefix;

// Duplicates the given null-terminated string into a new UNICODE_STRING.
PUNICODE_STRING DokanAllocateUnicodeString(__in PCWSTR String);

// Free UNICODE_STRING allocated with DokanAllocateUnicodeString.
VOID DokanFreeUnicodeString(__in PUNICODE_STRING UnicodeString);

// Allocates a new UNICODE_STRING and then deep copies the given one into it.
PUNICODE_STRING DokanAllocDuplicateString(__in const PUNICODE_STRING Src);

// Performs a deep copy of a UNICODE_STRING, where the actual destination
// UNICODE_STRING struct already exists. If the destination struct has a
// buffer already allocated, this function deletes it.
BOOLEAN DokanDuplicateUnicodeString(__out PUNICODE_STRING Dest,
                                    __in const PUNICODE_STRING Src);

// Wraps the given raw string as a UNICODE_STRING with no copying.
inline UNICODE_STRING DokanWrapUnicodeString(__in WCHAR* Buffer,
                                             __in USHORT Length) {
  UNICODE_STRING result;
  result.Buffer = Buffer;
  result.Length = Length;
  result.MaximumLength = Length;
  return result;
}

// Search WCHAR character position in a string.
//
// inputPUnicodeString, the input PUNICODE_STRING to search
// targetWchar, the target WCHAR you want to search in the UNICODE_STRING
// offsetPosition, the starting point to search
// isIgnoreTargetWchar, boolean value, determine you want to truncate(ignore)
// the UNICODE_STRING with the targetWchar or not.
//
// Example input :
// \\DosDevices\\Global\\Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9} and
// targetWchar = L'\\'
// Set isIgnoreTargetWchar = 0, you are trying to get the offset in order to
// get something like : \Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
// Set isIgnoreTargetWchar = 1, you are trying to get the offset in order to
// get something like : Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
ULONG DokanSearchWcharinUnicodeStringWithUlong(
    __in PUNICODE_STRING inputPUnicodeString, __in WCHAR targetWchar,
    __in ULONG offsetPosition, __in int isIgnoreTargetWchar);

// Return Char position in Unicode String if exist.
LONG DokanSearchUnicodeStringChar(__in PUNICODE_STRING UnicodeString,
                                  __in WCHAR Char);

// Return Char position in String if exist.
LONG DokanSearchStringChar(__in PWCHAR String, __in ULONG Length,
                           __in WCHAR Char);

// Check if the string start with the Prefix.
BOOLEAN StartsWith(__in const UNICODE_STRING* Str,
                   __in const UNICODE_STRING* Prefix);

// Returns TRUE if the given string starts with "\DosDevices\".
BOOLEAN StartsWithDosDevicesPrefix(__in const UNICODE_STRING* Str);

// Returns TRUE if the given string starts with "\??\Volume{".
BOOLEAN StartsWithVolumeGuidPrefix(__in const UNICODE_STRING* Str);

// Returns TRUE if the given string is in the form "\DosDevices\C:" with any
// drive letter.
BOOLEAN IsMountPointDriveLetter(__in const UNICODE_STRING* MountPoint);

// Replace the Prefix if present by the NewPrefix in the UNICODE_STRING Str.
// With HasPrefix enabled, the function will fail if Prefix is not present.
PUNICODE_STRING ChangePrefix(const UNICODE_STRING* Str,
                             const UNICODE_STRING* Prefix, BOOLEAN HasPrefix,
                             const UNICODE_STRING* NewPrefix);

#endif  // STR_H_