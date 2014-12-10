/*I
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

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


VOID
DokanIrpCancelRoutine(
    __in PDEVICE_OBJECT   DeviceObject,
    __in PIRP             Irp
    )
{
    KIRQL               oldIrql;
    PIRP_ENTRY			irpEntry;
	ULONG				serialNumber;
	PIO_STACK_LOCATION	irpSp;

    DDbgPrint("==> DokanIrpCancelRoutine\n");

    // Release the cancel spinlock
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    irpEntry = Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY];
    
	if (irpEntry != NULL) {
		PKSPIN_LOCK	lock = &irpEntry->IrpList->ListLock;

		// Acquire the queue spinlock
		ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
		KeAcquireSpinLock(lock, &oldIrql);

		irpSp = IoGetCurrentIrpStackLocation(Irp);
		ASSERT(irpSp != NULL);

		serialNumber = irpEntry->SerialNumber;

		RemoveEntryList(&irpEntry->ListEntry);

		// If Write is canceld before completion and buffer that saves writing
		// content is not freed, free it here
		if (irpSp->MajorFunction == IRP_MJ_WRITE) {
			PVOID eventContext = Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT];
			if (eventContext != NULL) {
				DokanFreeEventContext(eventContext);
			}
			Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = NULL;
		}


		if (IsListEmpty(&irpEntry->IrpList->ListHead)) {
			//DDbgPrint("    list is empty ClearEvent\n");
			KeClearEvent(&irpEntry->IrpList->NotEmpty);
		}

		irpEntry->Irp = NULL;

		if (irpEntry->CancelRoutineFreeMemory == FALSE) {
			InitializeListHead(&irpEntry->ListEntry);
		} else {
			DokanFreeIrpEntry(irpEntry);
			irpEntry = NULL;
		}

		Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL; 

		KeReleaseSpinLock(lock, oldIrql);
	}

	DDbgPrint("   canceled IRP #%X\n", serialNumber);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

	DDbgPrint("<== DokanIrpCancelRoutine\n");
    return;

}


NTSTATUS
RegisterPendingIrpMain(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP			Irp,
	__in ULONG			SerialNumber,
	__in PIRP_LIST		IrpList,
	__in ULONG			Flags,
	__in ULONG			CheckMount
    )
{
 	PIRP_ENTRY			irpEntry;
    PIO_STACK_LOCATION	irpSp;
    KIRQL				oldIrql;
 
	//DDbgPrint("==> DokanRegisterPendingIrpMain\n");

	if (GetIdentifierType(DeviceObject->DeviceExtension) == VCB) {
		PDokanVCB vcb = DeviceObject->DeviceExtension;
		if (CheckMount && !vcb->Dcb->Mounted) {
			DDbgPrint(" device is not mounted\n");
			return STATUS_INSUFFICIENT_RESOURCES;
		}
	}

    irpSp = IoGetCurrentIrpStackLocation(Irp);
 
    // Allocate a record and save all the event context.
    irpEntry = DokanAllocateIrpEntry();

    if (NULL == irpEntry) {
		DDbgPrint("  can't allocate IRP_ENTRY\n");
        return  STATUS_INSUFFICIENT_RESOURCES;
    }

	RtlZeroMemory(irpEntry, sizeof(IRP_ENTRY));

    InitializeListHead(&irpEntry->ListEntry);

	irpEntry->SerialNumber		= SerialNumber;
    irpEntry->FileObject		= irpSp->FileObject;
    irpEntry->Irp				= Irp;
	irpEntry->IrpSp				= irpSp;
	irpEntry->IrpList			= IrpList;
	irpEntry->Flags				= Flags;

	DokanUpdateTimeout(&irpEntry->TickCount, DOKAN_IRP_PENDING_TIMEOUT);

	//DDbgPrint("  Lock IrpList.ListLock\n");
	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    KeAcquireSpinLock(&IrpList->ListLock, &oldIrql);

    IoSetCancelRoutine(Irp, DokanIrpCancelRoutine);

    if (Irp->Cancel) {
        if (IoSetCancelRoutine(Irp, NULL) != NULL) {
			//DDbgPrint("  Release IrpList.ListLock %d\n", __LINE__);
            KeReleaseSpinLock(&IrpList->ListLock, oldIrql);

            DokanFreeIrpEntry(irpEntry);

            return STATUS_CANCELLED;
        }
	}

    IoMarkIrpPending(Irp);

    InsertTailList(&IrpList->ListHead, &irpEntry->ListEntry);

    irpEntry->CancelRoutineFreeMemory = FALSE;

	// save the pointer in order to be accessed by cancel routine
	Irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] =  irpEntry;


	KeSetEvent(&IrpList->NotEmpty, IO_NO_INCREMENT, FALSE);

	//DDbgPrint("  Release IrpList.ListLock\n");
    KeReleaseSpinLock(&IrpList->ListLock, oldIrql);

	//DDbgPrint("<== DokanRegisterPendingIrpMain\n");
    return STATUS_PENDING;;
}


NTSTATUS
DokanRegisterPendingIrp(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP			Irp,
	__in PEVENT_CONTEXT	EventContext,
	__in ULONG			Flags
    )
{
	PDokanVCB vcb = DeviceObject->DeviceExtension;
	NTSTATUS status;

	if (GetIdentifierType(vcb) != VCB) {
		DbgPrint("  Type != VCB\n");
		return STATUS_INVALID_PARAMETER;
	}

	status = RegisterPendingIrpMain(
		DeviceObject,
		Irp,
		EventContext->SerialNumber,
		&vcb->Dcb->PendingIrp,
		Flags,
		TRUE);

	if (status == STATUS_PENDING) {
		DokanEventNotification(&vcb->Dcb->NotifyEvent, EventContext);
	} else {
		DokanFreeEventContext(EventContext);
	}
	return status;
}



NTSTATUS
DokanRegisterPendingIrpForEvent(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP			Irp
    )
{
	PDokanVCB vcb = DeviceObject->DeviceExtension;

	if (GetIdentifierType(vcb) != VCB) {
		DbgPrint("  Type != VCB\n");
		return STATUS_INVALID_PARAMETER;
	}

	//DDbgPrint("DokanRegisterPendingIrpForEvent\n");

	return RegisterPendingIrpMain(
		DeviceObject,
		Irp,
		0, // SerialNumber
		&vcb->Dcb->PendingEvent,
		0, // Flags
		TRUE);
}



NTSTATUS
DokanRegisterPendingIrpForService(
	__in PDEVICE_OBJECT	DeviceObject,
	__in PIRP			Irp
	)
{
	PDOKAN_GLOBAL dokanGlobal;
	DDbgPrint("DokanRegisterPendingIrpForService\n");

	dokanGlobal = DeviceObject->DeviceExtension;
	if (GetIdentifierType(dokanGlobal) != DGL) {
		return STATUS_INVALID_PARAMETER;
	}

	return RegisterPendingIrpMain(
		DeviceObject,
		Irp,
		0, // SerialNumber
		&dokanGlobal->PendingService,
		0, // Flags
		FALSE);
}


// When user-mode file system application returns EventInformation,
// search corresponding pending IRP and complete it
NTSTATUS
DokanCompleteIrp(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP Irp
	)
{
	KIRQL				oldIrql;
    PLIST_ENTRY			thisEntry, nextEntry, listHead;
	PIRP_ENTRY			irpEntry;
	PDokanVCB			vcb;
	PEVENT_INFORMATION	eventInfo;

	eventInfo		= (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
	ASSERT(eventInfo != NULL);
	
	//DDbgPrint("==> DokanCompleteIrp [EventInfo #%X]\n", eventInfo->SerialNumber);

	vcb = DeviceObject->DeviceExtension;
	if (GetIdentifierType(vcb) != VCB) {
		return STATUS_INVALID_PARAMETER;
	}

	//DDbgPrint("      Lock IrpList.ListLock\n");
	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	KeAcquireSpinLock(&vcb->Dcb->PendingIrp.ListLock, &oldIrql);

	// search corresponding IRP through pending IRP list
	listHead = &vcb->Dcb->PendingIrp.ListHead;

    for (thisEntry = listHead->Flink; thisEntry != listHead; thisEntry = nextEntry) {

		PIRP				irp;
		PIO_STACK_LOCATION	irpSp;

        nextEntry = thisEntry->Flink;

        irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

		// check whether this is corresponding IRP

        //DDbgPrint("SerialNumber irpEntry %X eventInfo %X\n", irpEntry->SerialNumber, eventInfo->SerialNumber);

		// this irpEntry must be freed in this if statement
		if (irpEntry->SerialNumber != eventInfo->SerialNumber)  {
			continue;
		}

		RemoveEntryList(thisEntry);

		irp = irpEntry->Irp;
	
		if (irp == NULL) {
			// this IRP is already canceled
			ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
			DokanFreeIrpEntry(irpEntry);
			irpEntry = NULL;
			break;
		}

		if (IoSetCancelRoutine(irp, NULL) == NULL) {
			// Cancel routine will run as soon as we release the lock
			InitializeListHead(&irpEntry->ListEntry);
			irpEntry->CancelRoutineFreeMemory = TRUE;
			break;
		}

		// IRP is not canceled yet
		irpSp = irpEntry->IrpSp;	
		
		ASSERT(irpSp != NULL);
					
		// IrpEntry is saved here for CancelRoutine
		// Clear it to prevent to be completed by CancelRoutine twice
		irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;
		KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

		switch (irpSp->MajorFunction) {
		case IRP_MJ_DIRECTORY_CONTROL:
			DokanCompleteDirectoryControl(irpEntry, eventInfo);
			break;
		case IRP_MJ_READ:
			DokanCompleteRead(irpEntry, eventInfo);
			break;
		case IRP_MJ_WRITE:
			DokanCompleteWrite(irpEntry, eventInfo);
			break;
		case IRP_MJ_QUERY_INFORMATION:
			DokanCompleteQueryInformation(irpEntry, eventInfo);
			break;
		case IRP_MJ_QUERY_VOLUME_INFORMATION:
			DokanCompleteQueryVolumeInformation(irpEntry, eventInfo);
			break;
		case IRP_MJ_CREATE:
			DokanCompleteCreate(irpEntry, eventInfo);
			break;
		case IRP_MJ_CLEANUP:
			DokanCompleteCleanup(irpEntry, eventInfo);
			break;
		case IRP_MJ_LOCK_CONTROL:
			DokanCompleteLock(irpEntry, eventInfo);
			break;
		case IRP_MJ_SET_INFORMATION:
			DokanCompleteSetInformation(irpEntry, eventInfo);
			break;
		case IRP_MJ_FLUSH_BUFFERS:
			DokanCompleteFlush(irpEntry, eventInfo);
			break;
		case IRP_MJ_QUERY_SECURITY:
			DokanCompleteQuerySecurity(irpEntry, eventInfo);
			break;
		case IRP_MJ_SET_SECURITY:
			DokanCompleteSetSecurity(irpEntry, eventInfo);
			break;
		default:
			DDbgPrint("Unknown IRP %d\n", irpSp->MajorFunction);
			// TODO: in this case, should complete this IRP
			break;
		}		

		DokanFreeIrpEntry(irpEntry);
		irpEntry = NULL;

		return STATUS_SUCCESS;
	}

	KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

    //DDbgPrint("<== AACompleteIrp [EventInfo #%X]\n", eventInfo->SerialNumber);

	// TODO: should return error
    return STATUS_SUCCESS;
}

 
// start event dispatching
NTSTATUS
DokanEventStart(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP Irp
   )
{
	ULONG				outBufferLen;
	ULONG				inBufferLen;
	PVOID				buffer;
	PIO_STACK_LOCATION	irpSp;
	EVENT_START			eventStart;
	PEVENT_DRIVER_INFO	driverInfo;
	PDOKAN_GLOBAL		dokanGlobal;
	PDokanDCB			dcb;
	NTSTATUS			status;
	DEVICE_TYPE			deviceType;
	ULONG				deviceCharacteristics;
	WCHAR				baseGuidString[64];
	GUID				baseGuid = DOKAN_BASE_GUID;
	UNICODE_STRING		unicodeGuid;
	ULONG				deviceNamePos;
	

	DDbgPrint("==> DokanEventStart\n");

	dokanGlobal = DeviceObject->DeviceExtension;
	if (GetIdentifierType(dokanGlobal) != DGL) {
		return STATUS_INVALID_PARAMETER;
	}

	irpSp = IoGetCurrentIrpStackLocation(Irp);

	outBufferLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
	inBufferLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;

	if (outBufferLen != sizeof(EVENT_DRIVER_INFO) ||
		inBufferLen != sizeof(EVENT_START)) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlCopyMemory(&eventStart, Irp->AssociatedIrp.SystemBuffer, sizeof(EVENT_START));
	driverInfo = Irp->AssociatedIrp.SystemBuffer;

	if (eventStart.UserVersion != DOKAN_DRIVER_VERSION) {
		driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;
		driverInfo->Status = DOKAN_START_FAILED;
		Irp->IoStatus.Status = STATUS_SUCCESS;
		Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);
		return STATUS_SUCCESS;
	}

	deviceCharacteristics = FILE_DEVICE_IS_MOUNTED;

	switch (eventStart.DeviceType) {
	case DOKAN_DISK_FILE_SYSTEM:
		deviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
		break;
	case DOKAN_NETWORK_FILE_SYSTEM:
		deviceType = FILE_DEVICE_NETWORK_FILE_SYSTEM;
		deviceCharacteristics |= FILE_REMOTE_DEVICE;
		break;
	default:
		DDbgPrint("  Unknown device type: %d\n", eventStart.DeviceType);
		deviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
	}

	if (eventStart.Flags & DOKAN_EVENT_REMOVABLE) {
		DDbgPrint("  DeviceCharacteristics |= FILE_REMOVABLE_MEDIA\n");
		deviceCharacteristics |= FILE_REMOVABLE_MEDIA;
	}

	baseGuid.Data2 = (USHORT)(dokanGlobal->MountId & 0xFFFF) ^ baseGuid.Data2;
	baseGuid.Data3 = (USHORT)(dokanGlobal->MountId >> 16) ^ baseGuid.Data3;

	status = RtlStringFromGUID(&baseGuid, &unicodeGuid);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	RtlZeroMemory(baseGuidString, sizeof(baseGuidString));
	RtlStringCchCopyW(baseGuidString, sizeof(baseGuidString) / sizeof(WCHAR), unicodeGuid.Buffer);
	RtlFreeUnicodeString(&unicodeGuid);

	InterlockedIncrement(&dokanGlobal->MountId);

	KeEnterCriticalRegion();
	ExAcquireResourceExclusiveLite(&dokanGlobal->Resource, TRUE);

	status = DokanCreateDiskDevice(
				DeviceObject->DriverObject,
				dokanGlobal->MountId,
				baseGuidString,
				dokanGlobal,
				deviceType,
				deviceCharacteristics,
				&dcb);

	if (!NT_SUCCESS(status)) {
		ExReleaseResourceLite(&dokanGlobal->Resource);
		KeLeaveCriticalRegion();
		return status;
	}

	DDbgPrint("  MountId:%d\n", dcb->MountId);
	driverInfo->DeviceNumber = dokanGlobal->MountId;
	driverInfo->MountId = dokanGlobal->MountId;
	driverInfo->Status = DOKAN_MOUNTED;
	driverInfo->DriverVersion = DOKAN_DRIVER_VERSION;

	// SymbolicName is \\DosDevices\\Global\\Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
	// Finds the last '\' and copy into DeviceName.
	// DeviceName is \Volume{D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
	deviceNamePos = dcb->SymbolicLinkName->Length / sizeof(WCHAR) - 1;
	for (; dcb->SymbolicLinkName->Buffer[deviceNamePos] != L'\\'; --deviceNamePos)
		;
	RtlStringCchCopyW(driverInfo->DeviceName,
			sizeof(driverInfo->DeviceName) / sizeof(WCHAR),
			&(dcb->SymbolicLinkName->Buffer[deviceNamePos]));

	DDbgPrint("  DeviceName:%ws\n", driverInfo->DeviceName);
	DokanUpdateTimeout(&dcb->TickCount, DOKAN_KEEPALIVE_TIMEOUT);

	dcb->UseAltStream = 0;
	if (eventStart.Flags & DOKAN_EVENT_ALTERNATIVE_STREAM_ON) {
		DDbgPrint("  ALT_STREAM_ON\n");
		dcb->UseAltStream = 1;
	}
	dcb->UseKeepAlive = 0;
	if (eventStart.Flags & DOKAN_EVENT_KEEP_ALIVE_ON) {
		DDbgPrint("  KEEP_ALIVE_ON\n");
		dcb->UseKeepAlive = 1;
	}
	dcb->Mounted = 1;

	DokanStartEventNotificationThread(dcb);
	DokanStartCheckThread(dcb);

	ExReleaseResourceLite(&dokanGlobal->Resource);
	KeLeaveCriticalRegion();

	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(EVENT_DRIVER_INFO);

	DDbgPrint("<== DokanEventStart\n");

	return Irp->IoStatus.Status;
}




// user assinged bigger buffer that is enough to return WriteEventContext
NTSTATUS
DokanEventWrite(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP Irp
	)
{
	KIRQL				oldIrql;
    PLIST_ENTRY			thisEntry, nextEntry, listHead;
	PIRP_ENTRY			irpEntry;
    PDokanVCB			vcb;
	PEVENT_INFORMATION	eventInfo;
	PIRP				writeIrp;

	eventInfo		= (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
	ASSERT(eventInfo != NULL);
	
	DDbgPrint("==> DokanEventWrite [EventInfo #%X]\n", eventInfo->SerialNumber);

	vcb = DeviceObject->DeviceExtension;

	if (GetIdentifierType(vcb) != VCB) {
		return STATUS_INVALID_PARAMETER;
	}

	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	KeAcquireSpinLock(&vcb->Dcb->PendingIrp.ListLock, &oldIrql);

	// search corresponding write IRP through pending IRP list
	listHead = &vcb->Dcb->PendingIrp.ListHead;

    for (thisEntry = listHead->Flink; thisEntry != listHead; thisEntry = nextEntry) {

		PIO_STACK_LOCATION writeIrpSp, eventIrpSp;
		PEVENT_CONTEXT	eventContext;
		ULONG			info = 0;
		NTSTATUS		status;

		nextEntry = thisEntry->Flink;
        irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

		// check whehter this is corresponding IRP

        //DDbgPrint("SerialNumber irpEntry %X eventInfo %X\n", irpEntry->SerialNumber, eventInfo->SerialNumber);

		if (irpEntry->SerialNumber != eventInfo->SerialNumber)  {
			continue;
		}

		// do NOT free irpEntry here
		writeIrp = irpEntry->Irp;
		if (writeIrp == NULL) {
			// this IRP has already been canceled
			ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
			DokanFreeIrpEntry(irpEntry);
			continue;
		}

		if (IoSetCancelRoutine(writeIrp, DokanIrpCancelRoutine) == NULL) {
		//if (IoSetCancelRoutine(writeIrp, NULL) != NULL) {
			// Cancel routine will run as soon as we release the lock
			InitializeListHead(&irpEntry->ListEntry);
			irpEntry->CancelRoutineFreeMemory = TRUE;
			continue;
		}

		writeIrpSp = irpEntry->IrpSp;
		eventIrpSp = IoGetCurrentIrpStackLocation(Irp);
			
		ASSERT(writeIrpSp != NULL);
		ASSERT(eventIrpSp != NULL);

		eventContext = (PEVENT_CONTEXT)writeIrp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT];
		ASSERT(eventContext != NULL);
				
		// short of buffer length
		if (eventIrpSp->Parameters.DeviceIoControl.OutputBufferLength
			< eventContext->Length) {		
			DDbgPrint("  EventWrite: STATUS_INSUFFICIENT_RESOURCE\n");
			status =  STATUS_INSUFFICIENT_RESOURCES;
		} else {
			PVOID buffer;
			//DDbgPrint("  EventWrite CopyMemory\n");
			//DDbgPrint("  EventLength %d, BufLength %d\n", eventContext->Length,
			//			eventIrpSp->Parameters.DeviceIoControl.OutputBufferLength);
			if (Irp->MdlAddress)
				buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
			else
				buffer = Irp->AssociatedIrp.SystemBuffer;
					
			ASSERT(buffer != NULL);
			RtlCopyMemory(buffer, eventContext, eventContext->Length);
						
			info = eventContext->Length;
			status = STATUS_SUCCESS;
		}

		DokanFreeEventContext(eventContext);
		writeIrp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_EVENT] = 0;

		KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

		Irp->IoStatus.Status = status;
		Irp->IoStatus.Information = info;

		// this IRP will be completed by caller function
		return Irp->IoStatus.Status;
	}

	KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);

   return STATUS_SUCCESS;
}

