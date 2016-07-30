/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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
