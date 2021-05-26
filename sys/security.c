/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2021 Google, Inc.
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

NTSTATUS
DokanDispatchQuerySecurity(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject;
  ULONG bufferLength;
  PSECURITY_INFORMATION securityInfo;
  PDokanFCB fcb = NULL;
  PDokanCCB ccb;
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  if (fileObject == NULL || !RequestContext->Vcb ||
      !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
    return STATUS_INVALID_PARAMETER;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  if (fcb == NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext, "FCB == NULL");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  bufferLength = RequestContext->IrpSp->Parameters.QuerySecurity.Length;
  securityInfo =
      &RequestContext->IrpSp->Parameters.QuerySecurity.SecurityInformation;

  if (*securityInfo & OWNER_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "OWNER_SECURITY_INFORMATION");
  }
  if (*securityInfo & GROUP_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "GROUP_SECURITY_INFORMATION");
  }
  if (*securityInfo & DACL_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "DACL_SECURITY_INFORMATION");
  }
  if (*securityInfo & SACL_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "SACL_SECURITY_INFORMATION");
  }
  if (*securityInfo & LABEL_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "LABEL_SECURITY_INFORMATION");
  }

  DokanFCBLockRO(fcb);
  eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
  eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

  if (eventContext == NULL) {
  	DokanFCBUnlock(fcb);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (RequestContext->Irp->UserBuffer != NULL && bufferLength > 0) {
    // make a MDL for UserBuffer that can be used later on another thread
    // context
    if (RequestContext->Irp->MdlAddress == NULL) {
      status = DokanAllocateMdl(RequestContext, bufferLength);
      if (!NT_SUCCESS(status)) {
        DokanFreeEventContext(eventContext);
        DokanFCBUnlock(fcb);
        return status;
      }
      RequestContext->Flags = DOKAN_MDL_ALLOCATED;
    }
  }

  eventContext->Context = ccb->UserContext;
  eventContext->Operation.Security.SecurityInformation = *securityInfo;
  eventContext->Operation.Security.BufferLength = bufferLength;

  eventContext->Operation.Security.FileNameLength = fcb->FileName.Length;
  RtlCopyMemory(eventContext->Operation.Security.FileName,
                fcb->FileName.Buffer, fcb->FileName.Length);
  DokanFCBUnlock(fcb);
  return DokanRegisterPendingIrp(RequestContext, eventContext);
}

VOID DokanCompleteQuerySecurity(__in PREQUEST_CONTEXT RequestContext,
                                __in PEVENT_INFORMATION EventInfo) {
  PVOID buffer = NULL;
  ULONG bufferLength;
  PFILE_OBJECT fileObject;
  PDokanCCB ccb;

  fileObject = RequestContext->IrpSp->FileObject;
  ASSERT(fileObject != NULL);

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  if (RequestContext->Irp->MdlAddress) {
    buffer =
        MmGetSystemAddressForMdlNormalSafe(RequestContext->Irp->MdlAddress);
  }

  bufferLength = RequestContext->IrpSp->Parameters.QuerySecurity.Length;

  if (EventInfo->Status == STATUS_SUCCESS &&
      EventInfo->BufferLength <= bufferLength && buffer != NULL) {
    if (!RtlValidRelativeSecurityDescriptor(
            EventInfo->Buffer, EventInfo->BufferLength,
            RequestContext->IrpSp->Parameters.QuerySecurity
                .SecurityInformation)) {
      // No valid security descriptor to return.
      DOKAN_LOG_FINE_IRP(RequestContext, "Security Descriptor is not valid.");
      RequestContext->Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
    } else {
      RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);
      RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;
      RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
    }
  } else if (EventInfo->Status == STATUS_BUFFER_OVERFLOW ||
             (EventInfo->Status == STATUS_SUCCESS &&
              bufferLength < EventInfo->BufferLength)) {
    RequestContext->Irp->IoStatus.Information = EventInfo->BufferLength;
    RequestContext->Irp->IoStatus.Status = STATUS_BUFFER_OVERFLOW;

  } else {
    RequestContext->Irp->IoStatus.Status = EventInfo->Status;
  }

  if (RequestContext->Flags & DOKAN_MDL_ALLOCATED) {
    DokanFreeMdl(RequestContext->Irp);
    RequestContext->Flags &= ~DOKAN_MDL_ALLOCATED;
  }

  ccb = fileObject->FsContext2;
  if (ccb != NULL) {
    ccb->UserContext = EventInfo->Context;
  } else {
    DOKAN_LOG_FINE_IRP(RequestContext, "Ccb == NULL");
  }
}

NTSTATUS
DokanDispatchSetSecurity(__in PREQUEST_CONTEXT RequestContext) {
  PDokanCCB ccb;
  PDokanFCB fcb = NULL;
  PFILE_OBJECT fileObject;
  PSECURITY_INFORMATION securityInfo;
  PSECURITY_DESCRIPTOR securityDescriptor;
  ULONG securityDescLength;
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  if (fileObject == NULL || !RequestContext->Vcb ||
      !DokanCheckCCB(RequestContext, fileObject->FsContext2)) {
    return STATUS_INVALID_PARAMETER;
  }

  ccb = fileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  if (fcb == NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext, "FCB == NULL");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  securityInfo =
      &RequestContext->IrpSp->Parameters.SetSecurity.SecurityInformation;

  if (*securityInfo & OWNER_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "OWNER_SECURITY_INFORMATION");
  }
  if (*securityInfo & GROUP_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "GROUP_SECURITY_INFORMATION");
  }
  if (*securityInfo & DACL_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "DACL_SECURITY_INFORMATION");
  }
  if (*securityInfo & SACL_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "SACL_SECURITY_INFORMATION");
  }
  if (*securityInfo & LABEL_SECURITY_INFORMATION) {
    DOKAN_LOG_FINE_IRP(RequestContext, "LABEL_SECURITY_INFORMATION");
  }

  securityDescriptor =
      RequestContext->IrpSp->Parameters.SetSecurity.SecurityDescriptor;

  // Assumes the parameter is self relative SD.
  securityDescLength = RtlLengthSecurityDescriptor(securityDescriptor);

  // PSECURITY_DESCRIPTOR has to be aligned to a 4-byte boundary for use with
  // win32 functions. So we add 3 bytes here, to make sure we have extra room to
  // align BufferOffset.
  eventLength =
      sizeof(EVENT_CONTEXT) + securityDescLength + fcb->FileName.Length + 3;

  if (EVENT_CONTEXT_MAX_SIZE < eventLength) {
    // TODO: Handle this case like DispatchWrite.
    DOKAN_LOG_FINE_IRP(RequestContext, "SecurityDescriptor is too big: %d (limit %d)",
                       eventLength, EVENT_CONTEXT_MAX_SIZE);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  eventContext = AllocateEventContext(RequestContext, eventLength, ccb);

  if (eventContext == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  eventContext->Context = ccb->UserContext;
  eventContext->Operation.SetSecurity.SecurityInformation = *securityInfo;
  eventContext->Operation.SetSecurity.BufferLength = securityDescLength;

  // Align BufferOffset by adding 3, then zeroing the last 2 bits.
  eventContext->Operation.SetSecurity.BufferOffset =
      (FIELD_OFFSET(EVENT_CONTEXT, Operation.SetSecurity.FileName[0]) +
       fcb->FileName.Length + sizeof(WCHAR) + 3) &
      ~0x03;
  RtlCopyMemory(
      (PCHAR)eventContext + eventContext->Operation.SetSecurity.BufferOffset,
      securityDescriptor, securityDescLength);
  eventContext->Operation.SetSecurity.FileNameLength = fcb->FileName.Length;
  RtlCopyMemory(eventContext->Operation.SetSecurity.FileName,
                fcb->FileName.Buffer, fcb->FileName.Length);

  return DokanRegisterPendingIrp(RequestContext, eventContext);
}

VOID DokanCompleteSetSecurity(__in PREQUEST_CONTEXT RequestContext,
                              __in PEVENT_INFORMATION EventInfo) {
  PFILE_OBJECT fileObject;
  PDokanCCB ccb = NULL;
  PDokanFCB fcb = NULL;

  fileObject = RequestContext->IrpSp->FileObject;
  ASSERT(fileObject != NULL);

  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  ccb = fileObject->FsContext2;
  if (ccb != NULL) {
    ccb->UserContext = EventInfo->Context;
    fcb = ccb->Fcb;
    ASSERT(fcb != NULL);
  } else {
    DOKAN_LOG_FINE_IRP(RequestContext, "Ccb == NULL");
  }

  if (fcb && NT_SUCCESS(EventInfo->Status)) {
    DokanFCBLockRO(fcb);
    DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_SECURITY,
                            FILE_ACTION_MODIFIED);
    DokanFCBUnlock(fcb);
  }

  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
}
