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

#include "dokan.h"

NTSTATUS
DokanGetAccessToken(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {

  KIRQL oldIrql = 0;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry = NULL;
  PDokanVCB vcb;
  PEVENT_INFORMATION eventInfo;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PIO_STACK_LOCATION irpSp = NULL;
  BOOLEAN hasLock = FALSE;
  ULONG outBufferLen;
  ULONG inBufferLen;
  PSECURITY_SUBJECT_CONTEXT subjectContext = NULL;
  SECURITY_SUBJECT_CONTEXT tmpSubjectContext;
  SECURITY_QUALITY_OF_SERVICE securityQualityOfService;
  SECURITY_CLIENT_CONTEXT securityClientContext;

  DDbgPrint("==> DokanGetAccessToken\n");
  vcb = DeviceObject->DeviceExtension;

  __try {
    eventInfo = (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
    ASSERT(eventInfo != NULL);

    if (Irp->RequestorMode != UserMode) {
      DDbgPrint("  needs to be called from user-mode\n");
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    if (GetIdentifierType(vcb) != VCB) {
      DDbgPrint("  GetIdentifierType != VCB\n");
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    outBufferLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    inBufferLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;

    if (outBufferLen != sizeof(EVENT_INFORMATION) ||
        inBufferLen != sizeof(EVENT_INFORMATION)) {

      DDbgPrint("  wrong input or output buffer length\n");
      status = STATUS_INVALID_PARAMETER;

      __leave;
    }

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    KeAcquireSpinLock(&vcb->Dcb->PendingIrp.ListLock, &oldIrql);
    hasLock = TRUE;

    // search corresponding IRP through pending IRP list
    listHead = &vcb->Dcb->PendingIrp.ListHead;

    for (thisEntry = listHead->Flink; thisEntry != listHead;
         thisEntry = nextEntry) {

      nextEntry = thisEntry->Flink;

      irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

      if (irpEntry->SerialNumber != eventInfo->SerialNumber) {

        continue;
      }

      // this irp must be IRP_MJ_CREATE or IRP_MJ_SET_SECURITY
	  
	  if(irpEntry->IrpSp->MajorFunction == IRP_MJ_CREATE) {

		  if(irpEntry->IrpSp->Parameters.Create.SecurityContext) {

			  subjectContext = &irpEntry->IrpSp->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext;
		  }
	  }
	  else if(irpEntry->IrpSp->MajorFunction == IRP_MJ_SET_SECURITY) {

		  //subjectContext = &irpEntry->ContextInfo.Security.SecuritySubjectContext;
		  //SeCaptureSubjectContext(&tmpSubjectContext);
		  subjectContext = &tmpSubjectContext;
	  }
      
      break;
    }

    KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);
    hasLock = FALSE;

    if (!irpEntry || !subjectContext) {

      DDbgPrint("  can't find pending Irp: %d\n", eventInfo->SerialNumber);
      __leave;
    }

	if(irpEntry && irpEntry->IrpSp->MajorFunction != IRP_MJ_CREATE) {

		SeCaptureSubjectContext(subjectContext);
	}

	securityQualityOfService.Length = sizeof(securityQualityOfService);
	securityQualityOfService.ImpersonationLevel = SecurityIdentification;
	securityQualityOfService.ContextTrackingMode = SECURITY_STATIC_TRACKING;
	securityQualityOfService.EffectiveOnly = FALSE;

	SeLockSubjectContext(subjectContext);

	status = SeCreateClientSecurityFromSubjectContext(subjectContext, &securityQualityOfService, FALSE, &securityClientContext);

	SeUnlockSubjectContext(subjectContext);

	if(irpEntry && irpEntry->IrpSp->MajorFunction != IRP_MJ_CREATE) {

		SeReleaseSubjectContext(subjectContext);
	}

	if(!NT_SUCCESS(status)) {

		DDbgPrint("  SeCreateClientSecurityFromSubjectContext failed: 0x%x\n", status);
		__leave;
	}

	ASSERT(TokenImpersonation == SeTokenType(securityClientContext.ClientToken));

    // NOTE: Accessing *SeTokenObjectType while acquiring a spin lock causes
    // BSOD on Windows XP.
    status = ObOpenObjectByPointer(
		securityClientContext.ClientToken,
		0,
		NULL,
		TOKEN_READ | TOKEN_DUPLICATE,
		*SeTokenObjectType,
		UserMode,
		&eventInfo->Operation.AccessToken.Handle);

	SeDeleteClientSecurity(&securityClientContext);

    if (!NT_SUCCESS(status)) {

      DDbgPrint("  ObOpenObjectByPointer failed: 0x%x\n", status);
      __leave;
    }

    Irp->IoStatus.Information = sizeof(EVENT_INFORMATION);
    status = STATUS_SUCCESS;

  } __finally {
    if (hasLock) {
      KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);
    }
  }
  DDbgPrint("<== DokanGetAccessToken\n");
  return status;
}