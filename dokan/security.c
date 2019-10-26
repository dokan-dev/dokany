/*
  Dokan : user-mode file system library for Windows

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
#include <assert.h>

/*
 * DefaultGetFileSecurity build a sddl of the current process user
 * with authenticate user rights for context menu. (New Folder, ...)
 * TODO - Only calcul one time the user group sid
 */
NTSTATUS DefaultGetFileSecurity(DOKAN_GET_FILE_SECURITY_EVENT *getFileSecurity) {
  WCHAR buffer[1024];
  WCHAR finalBuffer[2048];
  PTOKEN_USER userToken = NULL;
  PTOKEN_GROUPS groupsToken = NULL;
  HANDLE tokenHandle;
  LPTSTR userSidString = NULL, groupSidString = NULL;

  UNREFERENCED_PARAMETER(getFileSecurity->FileName);

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
  if (getFileSecurity->DokanFileInfo->IsDirectory) {
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
          SecurityDescriptorTmp, SDDL_REVISION_1, getFileSecurity->SecurityInformation,
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

  if (Size > getFileSecurity->SecurityDescriptorSize) {
    getFileSecurity->LengthNeeded = Size;
    return STATUS_BUFFER_OVERFLOW;
  }

  memcpy(getFileSecurity->SecurityDescriptor, SecurityDescriptorTmp, Size);
  getFileSecurity->LengthNeeded = Size;

  LocalFree(pStringBuffer);
  LocalFree(SecurityDescriptorTmp);

  return STATUS_SUCCESS;
}

void BeginDispatchQuerySecurity(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_GET_FILE_SECURITY_EVENT *getFileSecurity = &EventInfo->EventInfo.GetFileSecurityW;
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	DbgPrint("###GetFileSecurity file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
		EventInfo->DokanOpenInfo,
		EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
		EventInfo);

	assert(EventInfo->DokanOpenInfo);
	assert((void*)getFileSecurity == (void*)EventInfo);
	assert(EventInfo->ProcessingContext == NULL);

	CheckFileName(EventInfo->KernelInfo.EventContext.Operation.Security.FileName);

	CreateDispatchCommon(EventInfo, EventInfo->KernelInfo.EventContext.Operation.Security.BufferLength);

	if(EventInfo->DokanInstance->DokanOperations->GetFileSecurityW) {

		getFileSecurity->DokanFileInfo = &EventInfo->DokanFileInfo;
		getFileSecurity->FileName = EventInfo->KernelInfo.EventContext.Operation.Security.FileName;
		getFileSecurity->SecurityDescriptor = (PSECURITY_DESCRIPTOR)&EventInfo->EventResult->Buffer[0];
		getFileSecurity->SecurityInformation = EventInfo->KernelInfo.EventContext.Operation.Security.SecurityInformation;
		getFileSecurity->SecurityDescriptorSize = IoEventResultBufferSize(EventInfo);

		assert(getFileSecurity->LengthNeeded == 0);

		status = EventInfo->DokanInstance->DokanOperations->GetFileSecurityW(getFileSecurity);

		if (status == STATUS_NOT_IMPLEMENTED) {
			status = DefaultGetFileSecurity(getFileSecurity);
		}
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchGetFileSecurity(getFileSecurity, status);
	}
}

void DOKANAPI DokanEndDispatchGetFileSecurity(DOKAN_GET_FILE_SECURITY_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result = ioEvent->EventResult;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchGetFileSecurity() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	assert(result->BufferLength == 0);

	if(ResultStatus == STATUS_SUCCESS) {

		if(EventInfo->LengthNeeded > IoEventResultBufferSize(ioEvent)) {

			ResultStatus = STATUS_BUFFER_OVERFLOW;
		}
		else {

			result->BufferLength = EventInfo->LengthNeeded;
		}
	}

	result->Status = ResultStatus;

	SendIoEventResult(ioEvent);
}

void BeginDispatchSetSecurity(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_SET_FILE_SECURITY_EVENT *setFileSecurity = &EventInfo->EventInfo.SetFileSecurityW;
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	DbgPrint("###SetSecurity file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
		EventInfo->DokanOpenInfo,
		EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
		EventInfo);

	assert(EventInfo->DokanOpenInfo);
	assert((void*)setFileSecurity == (void*)EventInfo);
	assert(EventInfo->ProcessingContext == NULL);

	CheckFileName(EventInfo->KernelInfo.EventContext.Operation.SetSecurity.FileName);

	CreateDispatchCommon(EventInfo, 0);

	if(EventInfo->DokanInstance->DokanOperations->SetFileSecurityW) {

		setFileSecurity->DokanFileInfo = &EventInfo->DokanFileInfo;
		setFileSecurity->FileName = EventInfo->KernelInfo.EventContext.Operation.SetSecurity.FileName;
		setFileSecurity->SecurityDescriptor = (PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetSecurity.BufferOffset;
		setFileSecurity->SecurityInformation = EventInfo->KernelInfo.EventContext.Operation.SetSecurity.SecurityInformation;
		setFileSecurity->SecurityDescriptorSize = EventInfo->KernelInfo.EventContext.Operation.SetSecurity.BufferLength;

		status = EventInfo->DokanInstance->DokanOperations->SetFileSecurityW(setFileSecurity);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchSetFileSecurity(setFileSecurity, status);
	}
}

void DOKANAPI DokanEndDispatchSetFileSecurity(DOKAN_SET_FILE_SECURITY_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PEVENT_INFORMATION result = ioEvent->EventResult;

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchSetFileSecurity() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	assert(result->BufferLength == 0);

	result->Status = ResultStatus;

	SendIoEventResult(ioEvent);
}