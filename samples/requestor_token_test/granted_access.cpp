// SPDX-License-Identifier: MIT
// winternl.h and Dokany's fileinfo.h declare overlapping native types.
#include <windows.h>
#include <winternl.h>

bool HasGrantedAccess(HANDLE token, ACCESS_MASK expected) {
  PUBLIC_OBJECT_BASIC_INFORMATION info = {};
  return NtQueryObject(token, ObjectBasicInformation, &info, sizeof(info), NULL) >= 0 &&
         info.GrantedAccess == expected;
}
