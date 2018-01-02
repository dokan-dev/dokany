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

#include "dokani.h"
#include <sddl.h>
/*
 * DefaultGetFileSecurity build a sddl of the current process user
 * with authenticate user rights for context menu. (New Folder, ...)
 * TODO - Only calcul one time the user group sid
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
    DbgPrint("  GetTokenInformaiton failed: %d\n", GetLastError());
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
    DbgPrint("  GetTokenInformaiton failed: %d\n", GetLastError());
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

VOID DispatchQuerySecurity(HANDLE Handle, PEVENT_CONTEXT EventContext,
                           PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  DOKAN_FILE_INFO fileInfo;
  PDOKAN_OPEN_INFO openInfo;
  ULONG eventInfoLength;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  ULONG lengthNeeded = 0;

  eventInfoLength = sizeof(EVENT_INFORMATION) - 8 +
                    EventContext->Operation.Security.BufferLength;
  CheckFileName(EventContext->Operation.Security.FileName);

  eventInfo = DispatchCommon(EventContext, eventInfoLength, DokanInstance,
                             &fileInfo, &openInfo);

  DbgPrint("###GetFileSecurity %04d\n",
           openInfo != NULL ? openInfo->EventId : -1);

  if (DokanInstance->DokanOperations->GetFileSecurity) {
    status = DokanInstance->DokanOperations->GetFileSecurity(
        EventContext->Operation.Security.FileName,
        &EventContext->Operation.Security.SecurityInformation,
        &eventInfo->Buffer, EventContext->Operation.Security.BufferLength,
        &lengthNeeded, &fileInfo);
  }

  if (status == STATUS_NOT_IMPLEMENTED) {
    status = DefaultGetFileSecurity(
        EventContext->Operation.Security.FileName,
        &EventContext->Operation.Security.SecurityInformation,
        &eventInfo->Buffer, EventContext->Operation.Security.BufferLength,
        &lengthNeeded, &fileInfo);
  }

  eventInfo->Status = status;

  if (status != STATUS_SUCCESS && status != STATUS_BUFFER_OVERFLOW) {
    eventInfo->BufferLength = 0;
  } else {
    eventInfo->BufferLength = lengthNeeded;

    if (EventContext->Operation.Security.BufferLength < lengthNeeded) {
      // Filesystem Application should return STATUS_BUFFER_OVERFLOW in this
      // case.
      eventInfo->Status = STATUS_BUFFER_OVERFLOW;
    }
  }

  SendEventInformation(Handle, eventInfo, eventInfoLength, DokanInstance);
  free(eventInfo);
}

VOID DispatchSetSecurity(HANDLE Handle, PEVENT_CONTEXT EventContext,
                         PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  DOKAN_FILE_INFO fileInfo;
  PDOKAN_OPEN_INFO openInfo;
  ULONG eventInfoLength;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;
  PSECURITY_DESCRIPTOR securityDescriptor;

  eventInfoLength = sizeof(EVENT_INFORMATION);
  CheckFileName(EventContext->Operation.SetSecurity.FileName);

  eventInfo = DispatchCommon(EventContext, eventInfoLength, DokanInstance,
                             &fileInfo, &openInfo);

  DbgPrint("###SetSecurity %04d\n", openInfo != NULL ? openInfo->EventId : -1);

  securityDescriptor =
      (PCHAR)EventContext + EventContext->Operation.SetSecurity.BufferOffset;

  if (DokanInstance->DokanOperations->SetFileSecurity) {
    status = DokanInstance->DokanOperations->SetFileSecurity(
        EventContext->Operation.SetSecurity.FileName,
        &EventContext->Operation.SetSecurity.SecurityInformation,
        securityDescriptor, EventContext->Operation.SetSecurity.BufferLength,
        &fileInfo);
  }

  if (status != STATUS_SUCCESS) {
    eventInfo->Status = STATUS_INVALID_PARAMETER;
    eventInfo->BufferLength = 0;
  } else {
    eventInfo->Status = STATUS_SUCCESS;
    eventInfo->BufferLength = 0;
  }

  SendEventInformation(Handle, eventInfo, eventInfoLength, DokanInstance);
  free(eventInfo);
}
