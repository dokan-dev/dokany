/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.
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

#include "dokani.h"
#include <sddl.h>
/*
 * DefaultGetFileSecurity build a sddl of the current process user
 * with authenticate user rights for context menu. (New Folder, ...)
 * TODO - Only calculate the user's group sid once
 */
NTSTATUS DefaultGetFileSecurity(LPCWSTR FileName,
                                PSECURITY_INFORMATION SecurityInformation,
                                PSECURITY_DESCRIPTOR SecurityDescriptor,
                                ULONG BufferLength, PULONG LengthNeeded,
                                PDOKAN_FILE_INFO DokanFileInfo) {
  WCHAR buffer[1024];
  WCHAR finalBuffer[2048];
  PTOKEN_USER userToken = NULL;
  PTOKEN_GROUPS groupsToken = NULL;
  HANDLE tokenHandle;
  LPTSTR userSidString = NULL, groupSidString = NULL;

  UNREFERENCED_PARAMETER(FileName);

  if (OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &tokenHandle) ==
      FALSE) {
    DbgPrint("  OpenProcessToken failed: %d\n", GetLastError());
    return STATUS_NOT_IMPLEMENTED;
  }

  DWORD returnLength;
  if (!GetTokenInformation(tokenHandle, TokenUser, buffer, sizeof(buffer),
                           &returnLength)) {
    DbgPrint("  GetTokenInformation failed: %d\n", GetLastError());
    CloseHandle(tokenHandle);
    return STATUS_NOT_IMPLEMENTED;
  }

  userToken = (PTOKEN_USER)buffer;
  if (!ConvertSidToStringSid(userToken->User.Sid, &userSidString)) {
    DbgPrint("  ConvertSidToStringSid failed: %d\n", GetLastError());
    CloseHandle(tokenHandle);
    return STATUS_NOT_IMPLEMENTED;
  }

  if (!GetTokenInformation(tokenHandle, TokenGroups, buffer, sizeof(buffer),
                           &returnLength)) {
    DbgPrint("  GetTokenInformation failed: %d\n", GetLastError());
    CloseHandle(tokenHandle);
    return STATUS_NOT_IMPLEMENTED;
  }

  groupsToken = (PTOKEN_GROUPS)buffer;
  if (groupsToken->GroupCount > 0) {
    if (!ConvertSidToStringSid(groupsToken->Groups[0].Sid, &groupSidString)) {
      DbgPrint("  ConvertSidToStringSid failed: %d\n", GetLastError());
      CloseHandle(tokenHandle);
      return STATUS_NOT_IMPLEMENTED;
    }
    swprintf_s(buffer, 1024, L"O:%lsG:%ls", userSidString, groupSidString);
  } else
    swprintf_s(buffer, 1024, L"O:%ls", userSidString);

  LocalFree(userSidString);
  LocalFree(groupSidString);
  CloseHandle(tokenHandle);

  // Authenticated users rights
  if (DokanFileInfo->IsDirectory) {
    swprintf_s(finalBuffer, 2048, L"%lsD:PAI(A;OICI;FA;;;AU)", buffer);
  } else {
    swprintf_s(finalBuffer, 2048, L"%lsD:AI(A;ID;FA;;;AU)", buffer);
  }

  PSECURITY_DESCRIPTOR SecurityDescriptorTmp = NULL;
  ULONG Size = 0;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
          finalBuffer, SDDL_REVISION_1, &SecurityDescriptorTmp, &Size)) {
    return STATUS_NOT_IMPLEMENTED;
  }

  LPTSTR pStringBuffer = NULL;
  if (!ConvertSecurityDescriptorToStringSecurityDescriptor(
          SecurityDescriptorTmp, SDDL_REVISION_1, *SecurityInformation,
          &pStringBuffer, NULL)) {
    return STATUS_NOT_IMPLEMENTED;
  }

  LocalFree(SecurityDescriptorTmp);
  SecurityDescriptorTmp = NULL;
  Size = 0;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
          pStringBuffer, SDDL_REVISION_1, &SecurityDescriptorTmp, &Size)) {
    return STATUS_NOT_IMPLEMENTED;
  }

  if (Size > BufferLength) {
    *LengthNeeded = Size;
    return STATUS_BUFFER_OVERFLOW;
  }

  memcpy(SecurityDescriptor, SecurityDescriptorTmp, Size);
  *LengthNeeded = Size;

  LocalFree(pStringBuffer);
  LocalFree(SecurityDescriptorTmp);

  return STATUS_SUCCESS;
}

VOID DispatchQuerySecurity(PDOKAN_IO_EVENT IoEvent) {
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  ULONG lengthNeeded = 0;

  CheckFileName(IoEvent->EventContext->Operation.Security.FileName);

  CreateDispatchCommon(IoEvent,
                       IoEvent->EventContext->Operation.Security.BufferLength,
                       /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  DbgPrint("###GetFileSecurity file handle = 0x%p, eventID = %04d, event Info "
           "= 0x%p\n",
           IoEvent->DokanOpenInfo,
           IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId
                                          : -1,
           IoEvent);

  if (IoEvent->DokanInstance->DokanOperations->GetFileSecurity) {
    status = IoEvent->DokanInstance->DokanOperations->GetFileSecurity(
        IoEvent->EventContext->Operation.Security.FileName,
        &IoEvent->EventContext->Operation.Security.SecurityInformation,
        &IoEvent->EventResult->Buffer,
        IoEvent->EventContext->Operation.Security.BufferLength, &lengthNeeded,
        &IoEvent->DokanFileInfo);
  }

  if (status == STATUS_NOT_IMPLEMENTED) {
    status = DefaultGetFileSecurity(
        IoEvent->EventContext->Operation.Security.FileName,
        &IoEvent->EventContext->Operation.Security.SecurityInformation,
        &IoEvent->EventResult->Buffer,
        IoEvent->EventContext->Operation.Security.BufferLength, &lengthNeeded,
        &IoEvent->DokanFileInfo);
  }

  IoEvent->EventResult->Status = status;

  if (status != STATUS_SUCCESS && status != STATUS_BUFFER_OVERFLOW) {
    IoEvent->EventResult->BufferLength = 0;
  } else {
    IoEvent->EventResult->BufferLength = lengthNeeded;

    if (IoEvent->EventContext->Operation.Security.BufferLength < lengthNeeded) {
      // Filesystem Application should return STATUS_BUFFER_OVERFLOW in this
      // case.
      IoEvent->EventResult->Status = STATUS_BUFFER_OVERFLOW;
    }
  }

  EventCompletion(IoEvent);
}

VOID DispatchSetSecurity(PDOKAN_IO_EVENT IoEvent) {
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  PSECURITY_DESCRIPTOR securityDescriptor;

  CheckFileName(IoEvent->EventContext->Operation.SetSecurity.FileName);

  CreateDispatchCommon(IoEvent, 0, /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  DbgPrint(
      "###SetSecurity file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
      IoEvent->DokanOpenInfo,
      IoEvent->DokanOpenInfo != NULL ? IoEvent->DokanOpenInfo->EventId : -1,
      IoEvent);

  securityDescriptor =
      (PCHAR)IoEvent->EventContext +
      IoEvent->EventContext->Operation.SetSecurity.BufferOffset;

  if (IoEvent->DokanInstance->DokanOperations->SetFileSecurity) {
    status = IoEvent->DokanInstance->DokanOperations->SetFileSecurity(
        IoEvent->EventContext->Operation.SetSecurity.FileName,
        &IoEvent->EventContext->Operation.SetSecurity.SecurityInformation,
        securityDescriptor,
        IoEvent->EventContext->Operation.SetSecurity.BufferLength, &IoEvent->DokanFileInfo);
  }

  if (status != STATUS_SUCCESS) {
    IoEvent->EventResult->Status = STATUS_INVALID_PARAMETER;
    IoEvent->EventResult->BufferLength = 0;
  } else {
    IoEvent->EventResult->Status = STATUS_SUCCESS;
    IoEvent->EventResult->BufferLength = 0;
  }

  EventCompletion(IoEvent);
}
