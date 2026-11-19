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

#include "dokan.h"
#include "util/irp_buffer_helper.h"

// Capture the opener before publishing the create. Static tracking snapshots
// its credentials here. With dynamic tracking the security manager takes a
// reference to the opener's effective token.
VOID DokanPrepareRequestorToken(__inout PIRP_ENTRY IrpEntry) {
  PIO_SECURITY_CONTEXT security =
      IrpEntry->RequestContext.IrpSp->Parameters.Create.SecurityContext;
  SECURITY_CLIENT_CONTEXT client;
  SECURITY_QUALITY_OF_SERVICE qos = {sizeof(qos), SecurityImpersonation,
                                     SECURITY_DYNAMIC_TRACKING, TRUE};

  if (KeGetCurrentIrql() != PASSIVE_LEVEL || security == NULL ||
      security->AccessState == NULL) {
    return;
  }
  // Use a supplied per-open QoS only to limit the local server. The I/O
  // manager owns this field and the pending entry keeps no pointer to it.
  if (security->SecurityQos != NULL) {
    SECURITY_IMPERSONATION_LEVEL level =
        security->SecurityQos->ImpersonationLevel;
    if (security->SecurityQos->Length != sizeof(qos) ||
        level < SecurityIdentification || level > SecurityDelegation) {
      return;
    }
    qos.ContextTrackingMode = security->SecurityQos->ContextTrackingMode;
    if (level == SecurityIdentification) {
      qos.ImpersonationLevel = SecurityIdentification;
    }
  }

  // Let the security manager check the opener's effective subject, including
  // an opening thread that was already impersonating. An ineligible create
  // still goes to the file system, but cannot supply a requestor token.
  if (NT_SUCCESS(SeCreateClientSecurityFromSubjectContext(
          &security->AccessState->SubjectSecurityContext, &qos, FALSE, &client))) {
    IrpEntry->RequestorToken = client.ClientToken;
    ObReferenceObject(IrpEntry->RequestorToken);
    IrpEntry->RequestorLevel = qos.ImpersonationLevel;
    SeDeleteClientSecurity(&client);
  }
}

static BOOLEAN MountCanImpersonate(PEPROCESS MountProcess) {
  LUID privilege = RtlConvertLongToLuid(SE_IMPERSONATE_PRIVILEGE);
  SECURITY_SUBJECT_CONTEXT subject;
  PRIVILEGE_SET required = {0};
  BOOLEAN granted;

  if (!SeSinglePrivilegeCheck(privilege, UserMode)) {
    return FALSE;
  }
  // An impersonating worker must not lend a client's privileges to the host.
  SeCaptureSubjectContextEx(NULL, MountProcess, &subject);
  required.PrivilegeCount = 1;
  required.Control = PRIVILEGE_SET_ALL_NECESSARY;
  required.Privilege[0].Luid = privilege;
  granted = SePrivilegeCheck(&required, &subject, UserMode);
  SeReleaseSubjectContext(&subject);
  return granted;
}

NTSTATUS
DokanGetAccessToken(__in PREQUEST_CONTEXT RequestContext) {
  PIRP irp = RequestContext->Irp;
  PDokanDCB dcb = RequestContext->Dcb;
  PEVENT_INFORMATION eventInfo;
  PLIST_ENTRY head = &dcb->PendingIrp.ListHead;
  PLIST_ENTRY entry;
  KIRQL oldIrql;
  PACCESS_TOKEN token = NULL;
  SECURITY_QUALITY_OF_SERVICE qos = {sizeof(qos), SecurityIdentification,
                                     SECURITY_STATIC_TRACKING, TRUE};
  OBJECT_ATTRIBUTES attributes;
  ACCESS_MASK access = TOKEN_QUERY;
  HANDLE source, handle;
  NTSTATUS status = STATUS_INVALID_PARAMETER;

  if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
    return STATUS_INVALID_DEVICE_STATE;
  }
  if (irp->RequestorMode != UserMode ||
      dcb->MountProcess != PsGetCurrentProcess() ||
      dcb->MountProcess != IoGetRequestorProcess(irp)) {
    return STATUS_ACCESS_DENIED;
  }
  GET_IRP_BUFFER_OR_RETURN(irp, eventInfo);
  if (RequestContext->IrpSp->Parameters.FileSystemControl.OutputBufferLength !=
      sizeof(*eventInfo)) {
    return STATUS_INVALID_PARAMETER;
  }
  eventInfo->Operation.AccessToken.Handle = NULL;

  KeAcquireSpinLock(&dcb->PendingIrp.ListLock, &oldIrql);
  for (entry = head->Flink; entry != head; entry = entry->Flink) {
    PIRP_ENTRY pending = CONTAINING_RECORD(entry, IRP_ENTRY, ListEntry);
    if (pending->SerialNumber != eventInfo->SerialNumber) {
      continue;
    }
    if (pending->RequestContext.Irp != NULL &&
        pending->RequestContext.IrpSp->MajorFunction == IRP_MJ_CREATE &&
        !pending->RequestContext.Irp->Cancel &&
        !pending->RequestContext.ForcedCanceled) {
      token = pending->RequestorToken;
      if (token != NULL) {
        ObReferenceObject(token);
        qos.ImpersonationLevel = pending->RequestorLevel;
      } else {
        status = STATUS_BAD_IMPERSONATION_LEVEL;
      }
    }
    break;
  }
  KeReleaseSpinLock(&dcb->PendingIrp.ListLock, oldIrql);
  if (token == NULL) {
    return status;
  }

  status = ObOpenObjectByPointer(token, OBJ_KERNEL_HANDLE, NULL, TOKEN_DUPLICATE,
                                 *SeTokenObjectType, KernelMode, &source);
  ObDereferenceObject(token);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  if (qos.ImpersonationLevel == SecurityImpersonation &&
      MountCanImpersonate(dcb->MountProcess)) {
    access |= TOKEN_IMPERSONATE;
  } else {
    qos.ImpersonationLevel = SecurityIdentification;
  }
  // Only this effective-only duplicate is placed in the mount process's
  // handle table. The original token handle stays private to the kernel.
  InitializeObjectAttributes(&attributes, NULL, OBJ_FORCE_ACCESS_CHECK, NULL,
                             NULL);
  attributes.SecurityQualityOfService = &qos;
  status = ZwDuplicateToken(source, access, &attributes, qos.EffectiveOnly,
                            TokenImpersonation, &handle);
  ZwClose(source);
  if (NT_SUCCESS(status)) {
    eventInfo->Operation.AccessToken.Handle = handle;
    irp->IoStatus.Information = sizeof(*eventInfo);
  }
  return status;
}
