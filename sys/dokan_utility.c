/*
Dokan : user-mode file system library for Windows

Copyright (C) 2015 - 2018 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

/***
  inputPUnicodeString, the input PUNICODE_STRING to search
  targetWchar, the target WCHAR you want to search in the UNICODE_STRING
  offsetPosition, the starting point to search
  isIgnoreTargetWchar, boolean value, determine you want to truncate(ignore) the UNICODE_STRING with the targetWchar or not.

  Example input : \\DosDevices\\Global\\Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9} and targetWchar = L'\\'
  Set isIgnoreTargetWchar = 0, you are trying to get the offset in order to get something like : \Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
  Set isIgnoreTargetWchar = 1, you are trying to get the offset in order to get something like : Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}

*/
ULONG DokanSearchWcharinUnicodeStringWithUlong(
    __in PUNICODE_STRING inputPUnicodeString, __in WCHAR targetWchar,
    __in ULONG offsetPosition, __in int isIgnoreTargetWchar) {

  ASSERT(inputPUnicodeString != NULL);

  if (offsetPosition > inputPUnicodeString->MaximumLength) {
    // trying to prevent BSOD for invalid input parameter
    offsetPosition = inputPUnicodeString->Length;
    // if inputPUnicodeString->Length == 0, the while loop will be skiped directly. So, the return value will be 0.
  }

  // 0 > 0 will return false and end the loop
  while (offsetPosition > 0) {
    offsetPosition--;

    if (inputPUnicodeString->Buffer[offsetPosition] == targetWchar) {
      if (isIgnoreTargetWchar == 1) {
        offsetPosition++; // the next is the beginning of filename
      }
      break;
    }
  }
  return offsetPosition;
}