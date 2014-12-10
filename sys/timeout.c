/*
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
DokanUnmount(
	__in PDokanDCB Dcb
	)
{
	ULONG					eventLength;
	PEVENT_CONTEXT			eventContext;
	PDRIVER_EVENT_CONTEXT	driverEventContext;	
	PKEVENT					completedEvent;
	LARGE_INTEGER			timeout;
	PDokanVCB				vcb = Dcb->Vcb;
	ULONG					deviceNamePos;

	eventLength = sizeof(EVENT_CONTEXT);
	eventContext = AllocateEventContextRaw(eventLength);
				
	if (eventContext == NULL) {
		;//STATUS_INSUFFICIENT_RESOURCES;
		DokanEventRelease(vcb->DeviceObject);
		return;
	}

	driverEventContext =
		CONTAINING_RECORD(eventContext, DRIVER_EVENT_CONTEXT, EventContext);
	completedEvent = ExAllocatePool(sizeof(KEVENT));
	if (completedEvent) {
		KeInitializeEvent(completedEvent, NotificationEvent, FALSE);
		driverEventContext->Completed = completedEvent;
	}

	deviceNamePos = Dcb->SymbolicLinkName->Length / sizeof(WCHAR) - 1;
	for (; Dcb->SymbolicLinkName->Buffer[deviceNamePos] != L'\\'; --deviceNamePos)
		;
	RtlStringCchCopyW(eventContext->Unmount.DeviceName,
			sizeof(eventContext->Unmount.DeviceName) / sizeof(WCHAR),
			&(Dcb->SymbolicLinkName->Buffer[deviceNamePos]));

	DDbgPrint("  Send Unmount to Service : %ws\n", eventContext->Unmount.DeviceName);

	DokanEventNotification(&Dcb->Global->NotifyService, eventContext);

	if (completedEvent) {
		timeout.QuadPart = -1 * 10 * 1000 * 10; // 10 sec
		KeWaitForSingleObject(completedEvent, Executive, KernelMode, FALSE, &timeout);
	}

	DokanEventRelease(vcb->DeviceObject);

	if (completedEvent) {
		ExFreePool(completedEvent);
	}
}


VOID
DokanCheckKeepAlive(
	__in PDokanDCB	Dcb
	)
{
	LARGE_INTEGER		tickCount;
	ULONG				eventLength;
	PEVENT_CONTEXT		eventContext;
	ULONG				mounted;
	PDokanVCB			vcb = Dcb->Vcb;

	//DDbgPrint("==> DokanCheckKeepAlive\n");

	KeEnterCriticalRegion();
	KeQueryTickCount(&tickCount);
	ExAcquireResourceSharedLite(&Dcb->Resource, TRUE);

	if (Dcb->TickCount.QuadPart < tickCount.QuadPart) {

		mounted = Dcb->Mounted;

		ExReleaseResourceLite(&Dcb->Resource);

		DDbgPrint("  Timeout, force to umount\n");

		if (!mounted) {
			// not mounted
			KeLeaveCriticalRegion();
			return;
		}
		DokanUnmount(Dcb);

	} else {
		ExReleaseResourceLite(&Dcb->Resource);
	}

	KeLeaveCriticalRegion();
	//DDbgPrint("<== DokanCheckKeepAlive\n");
}



NTSTATUS
ReleaseTimeoutPendingIrp(
   __in PDokanDCB	Dcb
   )
{
	KIRQL				oldIrql;
    PLIST_ENTRY			thisEntry, nextEntry, listHead;
	PIRP_ENTRY			irpEntry;
	LARGE_INTEGER		tickCount;
	LIST_ENTRY			completeList;
	PIRP				irp;

	DDbgPrint("==> ReleaseTimeoutPendingIRP\n");
	InitializeListHead(&completeList);

	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	KeAcquireSpinLock(&Dcb->PendingIrp.ListLock, &oldIrql);

	// when IRP queue is empty, there is nothing to do
	if (IsListEmpty(&Dcb->PendingIrp.ListHead)) {
		KeReleaseSpinLock(&Dcb->PendingIrp.ListLock, oldIrql);
		DDbgPrint("  IrpQueue is Empty\n");
		return STATUS_SUCCESS;
	}

	KeQueryTickCount(&tickCount);

	// search timeout IRP through pending IRP list
	listHead = &Dcb->PendingIrp.ListHead;

    for (thisEntry = listHead->Flink;
		thisEntry != listHead;
		thisEntry = nextEntry) {

        nextEntry = thisEntry->Flink;

        irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

		// this IRP is NOT timeout yet
		if (tickCount.QuadPart < irpEntry->TickCount.QuadPart) {
			break;
		}

		RemoveEntryList(thisEntry);

		DDbgPrint(" timeout Irp #%X\n", irpEntry->SerialNumber);

		irp = irpEntry->Irp;

		if (irp == NULL) {
			// this IRP has already been canceled
			ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
			DokanFreeIrpEntry(irpEntry);
			continue;
		}

		// this IRP is not canceled yet
		if (IoSetCancelRoutine(irp, NULL) == NULL) {
			// Cancel routine will run as soon as we release the lock
			InitializeListHead(&irpEntry->ListEntry);
			irpEntry->CancelRoutineFreeMemory = TRUE;
			continue;
		}
		// IrpEntry is saved here for CancelRoutine
		// Clear it to prevent to be completed by CancelRoutine twice
		irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;
		InsertTailList(&completeList, &irpEntry->ListEntry);
	}

	if (IsListEmpty(&Dcb->PendingIrp.ListHead)) {
		KeClearEvent(&Dcb->PendingIrp.NotEmpty);
	}
	KeReleaseSpinLock(&Dcb->PendingIrp.ListLock, oldIrql);
	
	while (!IsListEmpty(&completeList)) {
		listHead = RemoveHeadList(&completeList);
		irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
		irp = irpEntry->Irp;
		irp->IoStatus.Information = 0;
		irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		DokanFreeIrpEntry(irpEntry);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}

	DDbgPrint("<== ReleaseTimeoutPendingIRP\n");
	return STATUS_SUCCESS;
}


NTSTATUS
DokanResetPendingIrpTimeout(
   __in PDEVICE_OBJECT	DeviceObject,
   __in PIRP			Irp
   )
{
	KIRQL				oldIrql;
    PLIST_ENTRY			thisEntry, nextEntry, listHead;
	PIRP_ENTRY			irpEntry;
	PDokanVCB			vcb;
	PEVENT_INFORMATION	eventInfo;
	ULONG				timeout; // in milisecond


	DDbgPrint("==> ResetPendingIrpTimeout\n");

	eventInfo		= (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
	ASSERT(eventInfo != NULL);

	timeout = eventInfo->ResetTimeout.Timeout;
	if (DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX < timeout) {
		timeout = DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX;
	}
	
	vcb = DeviceObject->DeviceExtension;
	if (GetIdentifierType(vcb) != VCB) {
		return STATUS_INVALID_PARAMETER;
	}
	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	KeAcquireSpinLock(&vcb->Dcb->PendingIrp.ListLock, &oldIrql);

	// search corresponding IRP through pending IRP list
	listHead = &vcb->Dcb->PendingIrp.ListHead;

    for (thisEntry = listHead->Flink; thisEntry != listHead; thisEntry = nextEntry) {

		PIRP				irp;
		PIO_STACK_LOCATION	irpSp;

        nextEntry = thisEntry->Flink;

        irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

		if (irpEntry->SerialNumber != eventInfo->SerialNumber)  {
			continue;
		}

		DokanUpdateTimeout(&irpEntry->TickCount, timeout);
		break;
	}
	KeReleaseSpinLock(&vcb->Dcb->PendingIrp.ListLock, oldIrql);
	DDbgPrint("<== ResetPendingIrpTimeout\n");
	return STATUS_SUCCESS;
}


KSTART_ROUTINE DokanTimeoutThread;
VOID
DokanTimeoutThread(
	PDokanDCB	Dcb)
/*++

Routine Description:

	checks wheter pending IRP is timeout or not each DOKAN_CHECK_INTERVAL

--*/
{
	NTSTATUS		status;
	KTIMER			timer;
	PVOID			pollevents[2];
	LARGE_INTEGER	timeout = {0};

	DDbgPrint("==> DokanTimeoutThread\n");

	KeInitializeTimerEx(&timer, SynchronizationTimer);
	
	pollevents[0] = (PVOID)&Dcb->KillEvent;
	pollevents[1] = (PVOID)&timer;

	KeSetTimerEx(&timer, timeout, DOKAN_CHECK_INTERVAL, NULL);
	
	while (TRUE) {
		status = KeWaitForMultipleObjects(2, pollevents, WaitAny,
			Executive, KernelMode, FALSE, NULL, NULL);
		
		if (!NT_SUCCESS(status) || status ==  STATUS_WAIT_0) {
			DDbgPrint("  DokanTimeoutThread catched KillEvent\n");
			// KillEvent or something error is occured
			break;
		}

		ReleaseTimeoutPendingIrp(Dcb);
		if (Dcb->UseKeepAlive)
			DokanCheckKeepAlive(Dcb);
	}

	KeCancelTimer(&timer);

	DDbgPrint("<== DokanTimeoutThread\n");

	PsTerminateSystemThread(STATUS_SUCCESS);
}


NTSTATUS
DokanStartCheckThread(
	__in PDokanDCB	Dcb)
/*++

Routine Description:

	execute DokanTimeoutThread

--*/
{
	NTSTATUS status;
	HANDLE	thread;

	DDbgPrint("==> DokanStartCheckThread\n");

	status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS,
		NULL, NULL, NULL, (PKSTART_ROUTINE)DokanTimeoutThread, Dcb);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL,
		KernelMode, (PVOID*)&Dcb->TimeoutThread, NULL);

	ZwClose(thread);

	DDbgPrint("<== DokanStartCheckThread\n");

	return STATUS_SUCCESS;
}


VOID
DokanStopCheckThread(
	__in PDokanDCB	Dcb)
/*++

Routine Description:

	exits DokanTimeoutThread

--*/
{
	DDbgPrint("==> DokanStopCheckThread\n");
	
	KeSetEvent(&Dcb->KillEvent, 0, FALSE);

	if (Dcb->TimeoutThread) {
		KeWaitForSingleObject(Dcb->TimeoutThread, Executive,
			KernelMode, FALSE, NULL);
		ObDereferenceObject(Dcb->TimeoutThread);
		Dcb->TimeoutThread = NULL;
	}
	
	DDbgPrint("<== DokanStopCheckThread\n");
}


NTSTATUS
DokanInformServiceAboutUnmount(
   __in PDEVICE_OBJECT	DeviceObject,
   __in PIRP			Irp)
{

	return STATUS_SUCCESS;
}


VOID
DokanUpdateTimeout(
	__out PLARGE_INTEGER TickCount,
	__in ULONG	Timeout
	)
{
	KeQueryTickCount(TickCount);
	TickCount->QuadPart += Timeout * 1000 * 10 / KeQueryTimeIncrement();
}
