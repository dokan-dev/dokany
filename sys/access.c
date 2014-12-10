/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2010 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

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
DokanGetAccessToken(
   __in PDEVICE_OBJECT	DeviceObject,
   __in PIRP			Irp
   )
{
	KIRQL				oldIrql;
    PLIST_ENTRY			thisEntry, nextEntry, listHead;
	PIRP_ENTRY			irpEntry;
	PDokanVCB			vcb;
	PEVENT_INFORMATION	eventInfo;
	PACCESS_TOKEN		accessToken;
	NTSTATUS			status = STATUS_INVALID_PARAMETER;
	HANDLE				handle;
	PIO_STACK_LOCATION	irpSp = NULL;
	BOOLEAN				hasLock = FALSE;
	ULONG				outBufferLen;
	ULONG				inBufferLen;
	PACCESS_STATE		accessState;

	DDbgPrint("==> DokanGetAccessToken\n");

	__try {
		eventInfo		= (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
		ASSERT(eventInfo != NULL);

		if (Irp->RequestorMode != UserMode) {
			DDbgPrint("  needs to be called from user-mode\n");
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		vcb = DeviceObject->DeviceExtension;
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

		for (thisEntry = listHead->Flink; thisEntry != listHead; thisEntry = nextEntry) {

			nextEntry = thisEntry->Flink;

			irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

			if (irpEntry->SerialNumber != eventInfo->SerialNumber)  {
				continue;
			}

			// this irp must be IRP_MJ_CREATE
			if (irpEntry->IrpSp->Parameters.Create.SecurityContext) {
				accessState = irpEntry->IrpSp->Parameters.Create.SecurityContext->AccessState;
			}
			break;
		}
		KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);
		hasLock = FALSE;

		if (accessState == NULL) {
			DDbgPrint("  can't find pending Irp: %d\n", eventInfo->SerialNumber);
			__leave;
		}

		accessToken = SeQuerySubjectContextToken(&accessState->SubjectSecurityContext);
		if (accessToken == NULL) {
			DDbgPrint("  accessToken == NULL\n");
			__leave;
		}
		// NOTE: Accessing *SeTokenObjectType while acquring sping lock causes
		// BSOD on Windows XP.
		status = ObOpenObjectByPointer(accessToken, 0, NULL, GENERIC_ALL,
			*SeTokenObjectType, KernelMode, &handle);
		if (!NT_SUCCESS(status)) {
			DDbgPrint("  ObOpenObjectByPointer failed: 0x%x\n", status);
			__leave;
		}

		eventInfo->AccessToken.Handle = handle;
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