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

// Shared internal constants and CRT-free helpers for dokannp3.dll.
// Kept inline with no CRT calls so dokannp3.dll imports only KERNEL32.dll.

#ifndef DOKAN_NP_UTIL_H_
#define DOKAN_NP_UTIL_H_

#ifndef _WINDOWS_
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#endif

// Upper bounds keeping every buffer-size computation far away from overflow.
#define DOKAN_NP_MAX_NAME_LENGTH 32767
#define DOKAN_NP_MAX_ENTRIES 4096

static __inline SIZE_T NpBoundedLength(LPCWSTR Text, SIZE_T Max) {
  SIZE_T length = 0;
  if (Text == NULL) {
    return 0;
  }
  while (length < Max && Text[length] != L'\0') {
    ++length;
  }
  return length;
}

static __inline VOID NpCopyChars(LPWSTR Destination, LPCWSTR Source,
                                 SIZE_T Length) {
  // Forward copy: also correct when Destination <= Source overlap.
  for (SIZE_T i = 0; i < Length; ++i) {
    Destination[i] = Source[i];
  }
}

static __inline BOOL NpIsAsciiLetter(WCHAR Char) {
  return (Char >= L'a' && Char <= L'z') || (Char >= L'A' && Char <= L'Z');
}

static __inline WCHAR NpToUpperAscii(WCHAR Char) {
  return (Char >= L'a' && Char <= L'z') ? (WCHAR)(Char - L'a' + L'A') : Char;
}

#endif // DOKAN_NP_UTIL_H_
