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

/*

IOCTL_EVENT_START:
DokanStartEventNotificationThread
  NotificationThread
	# PendingEvent has pending IPRs (IOCTL_EVENT_WAIT)
    # NotifyEvent has IO events (ex.IRP_MJ_READ)
    # notify NotifyEvent using PendingEvent in this loop
	NotificationLoop(&Dcb->PendingEvent,
					      &Dcb->NotifyEvent);

    # PendingService has service events (ex. Unmount notification)
	# NotifyService has pending IRPs (IOCTL_SERVICE_WAIT)
    NotificationLoop(Dcb->Global->PendingService,
                          &Dcb->Global->NotifyService);

IOCTL_EVENT_RELEASE:
DokanStopEventNotificationThread

IRP_MJ_READ:
DokanDispatchRead
  DokanRegisterPendingIrp
    # add IRP_MJ_READ to PendingIrp list
    DokanRegisterPendingIrpMain(PendingIrp)
	# put MJ_READ event into NotifyEvent
    DokanEventNotification(NotifyEvent, EventContext)

IOCTL_EVENT_WAIT:
  DokanRegisterPendingIrpForEvent
    # add this irp to PendingEvent list
    DokanRegisterPendingIrpMain(PendingEvent)

IOCTL_EVENT_INFO:
  DokanCompleteIrp
    DokanCompleteRead

*/


#include "dokan.h"

VOID
SetCommonEventContext(
	__in PDokanDCB		Dcb,
	__in PEVENT_CONTEXT	EventContext,
	__in PIRP			Irp,
	__in PDokanCCB		Ccb)
{
	PIO_STACK_LOCATION  irpSp;

	irpSp = IoGetCurrentIrpStackLocation(Irp);

	EventContext->MountId		= Dcb->MountId;
	EventContext->MajorFunction = irpSp->MajorFunction;
	EventContext->MinorFunction = irpSp->MinorFunction;
	EventContext->Flags			= irpSp->Flags;
	
	if (Ccb) {
		EventContext->FileFlags		= Ccb->Flags;
	}

	EventContext->ProcessId = IoGetRequestorProcessId(Irp);
}


PEVENT_CONTEXT
AllocateEventContextRaw(
	__in ULONG	EventContextLength
	)
{
	ULONG driverContextLength;
	PDRIVER_EVENT_CONTEXT driverEventContext;
	PEVENT_CONTEXT eventContext;

	driverContextLength = EventContextLength - sizeof(EVENT_CONTEXT) + sizeof(DRIVER_EVENT_CONTEXT);
	driverEventContext = ExAllocatePool(driverContextLength);

	if (driverEventContext == NULL) {
		return NULL;
	}

	RtlZeroMemory(driverEventContext, driverContextLength);
	InitializeListHead(&driverEventContext->ListEntry);

	eventContext = &driverEventContext->EventContext;
	eventContext->Length = EventContextLength;

	return eventContext;
}


PEVENT_CONTEXT
AllocateEventContext(
	__in PDokanDCB	Dcb,
	__in PIRP		Irp,
	__in ULONG		EventContextLength,
	__in PDokanCCB	Ccb
	)
{
	PEVENT_CONTEXT eventContext;
	eventContext = AllocateEventContextRaw(EventContextLength);
	if (eventContext == NULL) {
		return NULL;
	}
	SetCommonEventContext(Dcb, eventContext, Irp, Ccb);
	eventContext->SerialNumber = InterlockedIncrement(&Dcb->SerialNumber);

	return eventContext;
}


VOID
DokanFreeEventContext(
	__in PEVENT_CONTEXT	EventContext
	)
{
	PDRIVER_EVENT_CONTEXT driverEventContext =
		CONTAINING_RECORD(EventContext, DRIVER_EVENT_CONTEXT, EventContext);
	ExFreePool(driverEventContext);
}


VOID
DokanEventNotification(
	__in PIRP_LIST		NotifyEvent,
	__in PEVENT_CONTEXT	EventContext
	)
{
	PDRIVER_EVENT_CONTEXT driverEventContext =
		CONTAINING_RECORD(EventContext, DRIVER_EVENT_CONTEXT, EventContext);

	InitializeListHead(&driverEventContext->ListEntry);

	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

	//DDbgPrint("DokanEventNotification\n");

	ExInterlockedInsertTailList(
		&NotifyEvent->ListHead,
		&driverEventContext->ListEntry,
		&NotifyEvent->ListLock);

	KeSetEvent(&NotifyEvent->NotEmpty, IO_NO_INCREMENT, FALSE);
}


VOID
ReleasePendingIrp(
	__in PIRP_LIST	PendingIrp
	)
{
	PLIST_ENTRY	listHead;
	LIST_ENTRY	completeList;
	PIRP_ENTRY	irpEntry;
	KIRQL	oldIrql;
	PIRP	irp;
	
	InitializeListHead(&completeList);

	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	KeAcquireSpinLock(&PendingIrp->ListLock, &oldIrql);

	while (!IsListEmpty(&PendingIrp->ListHead)) {
		listHead = RemoveHeadList(&PendingIrp->ListHead);
		irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
		irp = irpEntry->Irp;
		if (irp == NULL) {
			// this IRP has already been canceled
			ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
			DokanFreeIrpEntry(irpEntry);
			continue;
		}

		if (IoSetCancelRoutine(irp, NULL) == NULL) {
			// Cancel routine will run as soon as we release the lock
			InitializeListHead(&irpEntry->ListEntry);
			irpEntry->CancelRoutineFreeMemory = TRUE;
			continue;
		}
		InsertTailList(&completeList, &irpEntry->ListEntry);
	}

	KeClearEvent(&PendingIrp->NotEmpty);
	KeReleaseSpinLock(&PendingIrp->ListLock, oldIrql);

	while (!IsListEmpty(&completeList)) {
		listHead = RemoveHeadList(&completeList);
		irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
		irp = irpEntry->Irp;
		irp->IoStatus.Information = 0;
		irp->IoStatus.Status = STATUS_SUCCESS;
		DokanFreeIrpEntry(irpEntry);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
}


VOID
ReleaseNotifyEvent(
	__in PIRP_LIST	NotifyEvent
	)
{
	PDRIVER_EVENT_CONTEXT	driverEventContext;
	PLIST_ENTRY	listHead;
	KIRQL oldIrql;

	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	KeAcquireSpinLock(&NotifyEvent->ListLock, &oldIrql);

	while(!IsListEmpty(&NotifyEvent->ListHead)) {
		listHead = RemoveHeadList(&NotifyEvent->ListHead);
		driverEventContext = CONTAINING_RECORD(
			listHead, DRIVER_EVENT_CONTEXT, ListEntry);
		ExFreePool(driverEventContext);
	}

	KeClearEvent(&NotifyEvent->NotEmpty);
	KeReleaseSpinLock(&NotifyEvent->ListLock, oldIrql);
}


VOID
NotificationLoop(
	__in PIRP_LIST	PendingIrp,
	__in PIRP_LIST	NotifyEvent
	)
{
	PDRIVER_EVENT_CONTEXT	driverEventContext;
	PLIST_ENTRY	listHead;
	PIRP_ENTRY	irpEntry;
	LIST_ENTRY	completeList;
	NTSTATUS	status;
	KIRQL	irpIrql;
	KIRQL	notifyIrql;
	PIRP	irp;
	ULONG	eventLen;
	ULONG	bufferLen;
	PVOID	buffer;

	//DDbgPrint("=> NotificationLoop\n");

	InitializeListHead(&completeList);

	ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	KeAcquireSpinLock(&PendingIrp->ListLock, &irpIrql);
	KeAcquireSpinLock(&NotifyEvent->ListLock, &notifyIrql);
		
	while (!IsListEmpty(&PendingIrp->ListHead) &&
		!IsListEmpty(&NotifyEvent->ListHead)) {
			
		listHead = RemoveHeadList(&NotifyEvent->ListHead);

		driverEventContext = CONTAINING_RECORD(
			listHead, DRIVER_EVENT_CONTEXT, ListEntry);

		listHead = RemoveHeadList(&PendingIrp->ListHead);
		irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);

		eventLen = driverEventContext->EventContext.Length;

		// ensure this eventIrp is not cancelled
		irp = irpEntry->Irp;

		if (irp == NULL) {
			// this IRP has already been canceled
			ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
			DokanFreeIrpEntry(irpEntry);
			// push back
			InsertTailList(&NotifyEvent->ListHead,
							&driverEventContext->ListEntry);
			continue;
		}

		if (IoSetCancelRoutine(irp, NULL) == NULL) {
			// Cancel routine will run as soon as we release the lock
			InitializeListHead(&irpEntry->ListEntry);
			irpEntry->CancelRoutineFreeMemory = TRUE;
			// push back
			InsertTailList(&NotifyEvent->ListHead,
							&driverEventContext->ListEntry);
			continue;
		}

		// available size that is used for event notification
		bufferLen =
			irpEntry->IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
		// buffer that is used to inform Event
		buffer	= irp->AssociatedIrp.SystemBuffer;

		// buffer is not specified or short of length
		if (bufferLen == 0 || buffer == NULL || bufferLen < eventLen) {
			DDbgPrint("EventNotice : STATUS_INSUFFICIENT_RESOURCES\n");
			DDbgPrint("  bufferLen: %d, eventLen: %d\n", bufferLen, eventLen);
			// push back
			InsertTailList(&NotifyEvent->ListHead,
							&driverEventContext->ListEntry);
			// marks as STATUS_INSUFFICIENT_RESOURCES
			irpEntry->SerialNumber = 0;
		} else {
			// let's copy EVENT_CONTEXT
			RtlCopyMemory(buffer, &driverEventContext->EventContext, eventLen);
			// save event length
			irpEntry->SerialNumber = eventLen;

			if (driverEventContext->Completed) {
				KeSetEvent(driverEventContext->Completed, IO_NO_INCREMENT, FALSE);
			}
			ExFreePool(driverEventContext);
		}
		InsertTailList(&completeList, &irpEntry->ListEntry);
	}

	KeClearEvent(&NotifyEvent->NotEmpty);
	KeClearEvent(&PendingIrp->NotEmpty);

	KeReleaseSpinLock(&NotifyEvent->ListLock, notifyIrql);
	KeReleaseSpinLock(&PendingIrp->ListLock, irpIrql);

	while (!IsListEmpty(&completeList)) {
		listHead = RemoveHeadList(&completeList);
		irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
		irp = irpEntry->Irp;
		if (irpEntry->SerialNumber == 0) {
			irp->IoStatus.Information = 0;
			irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		} else {
			irp->IoStatus.Information = irpEntry->SerialNumber;
			irp->IoStatus.Status = STATUS_SUCCESS;
		}
		DokanFreeIrpEntry(irpEntry);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}

	//DDbgPrint("<= NotificationLoop\n");
}


KSTART_ROUTINE NotificationThread;
VOID
NotificationThread(
	__in PDokanDCB	Dcb
	)
{
	PKEVENT events[5];
	PKWAIT_BLOCK waitBlock;
	NTSTATUS status;

	DDbgPrint("==> NotificationThread\n");

	waitBlock = ExAllocatePool(sizeof(KWAIT_BLOCK) * 5);
	if (waitBlock == NULL) {
		DDbgPrint("  Can't allocate WAIT_BLOCK\n");
		return;
	}
	events[0] = &Dcb->ReleaseEvent;
	events[1] = &Dcb->NotifyEvent.NotEmpty;
	events[2] = &Dcb->PendingEvent.NotEmpty;
	events[3] = &Dcb->Global->PendingService.NotEmpty;
	events[4] = &Dcb->Global->NotifyService.NotEmpty;

	while (1) {
		status = KeWaitForMultipleObjects(
			5, events, WaitAny, Executive, KernelMode, FALSE, NULL, waitBlock);

		if (status == STATUS_WAIT_0) {
			;
			break;

		} else if (status == STATUS_WAIT_1 || status == STATUS_WAIT_2) {

			NotificationLoop(
					&Dcb->PendingEvent,
					&Dcb->NotifyEvent);

		} else {
			NotificationLoop(
				&Dcb->Global->PendingService,
				&Dcb->Global->NotifyService);
		}
	}

	ExFreePool(waitBlock);
	DDbgPrint("<== NotificationThread\n");
}



NTSTATUS
DokanStartEventNotificationThread(
	__in PDokanDCB	Dcb)
{
	NTSTATUS status;
	HANDLE	thread;

	DDbgPrint("==> DokanStartEventNotificationThread\n");

	KeResetEvent(&Dcb->ReleaseEvent);

	status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS,
		NULL, NULL, NULL,
		(PKSTART_ROUTINE)NotificationThread,
		Dcb);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL,
		KernelMode, (PVOID*)&Dcb->EventNotificationThread, NULL);

	ZwClose(thread);

	DDbgPrint("<== DokanStartEventNotificationThread\n");

	return STATUS_SUCCESS;
}


VOID
DokanStopEventNotificationThread(
	__in PDokanDCB	Dcb)
{
	DDbgPrint("==> DokanStopEventNotificationThread\n");
	
	KeSetEvent(&Dcb->ReleaseEvent, 0, FALSE);

	if (Dcb->EventNotificationThread) {
		KeWaitForSingleObject(
			Dcb->EventNotificationThread, Executive,
			KernelMode, FALSE, NULL);
		ObDereferenceObject(Dcb->EventNotificationThread);
		Dcb->EventNotificationThread = NULL;
	}
	
	DDbgPrint("<== DokanStopEventNotificationThread\n");
}


NTSTATUS
DokanEventRelease(
	__in PDEVICE_OBJECT DeviceObject)
{
	PDokanDCB	dcb;
	PDokanVCB	vcb;
	PDokanFCB	fcb;
	PDokanCCB	ccb;
	PLIST_ENTRY	fcbEntry, fcbNext, fcbHead;
	PLIST_ENTRY	ccbEntry, ccbNext, ccbHead;
	NTSTATUS	status = STATUS_SUCCESS;

	vcb = DeviceObject->DeviceExtension;
	if (GetIdentifierType(vcb) != VCB) {
		return STATUS_INVALID_PARAMETER;
	}
	dcb = vcb->Dcb;

	//ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);
	dcb->Mounted = 0;
	//ExReleaseResourceLite(&dcb->Resource);

	// search CCB list to complete not completed Directory Notification 

	KeEnterCriticalRegion();
	ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE);

	fcbHead = &vcb->NextFCB;

    for (fcbEntry = fcbHead->Flink; fcbEntry != fcbHead; fcbEntry = fcbNext) {

		fcbNext = fcbEntry->Flink;
		fcb = CONTAINING_RECORD(fcbEntry, DokanFCB, NextFCB);

		ExAcquireResourceExclusiveLite(&fcb->Resource, TRUE);

		ccbHead = &fcb->NextCCB;

		for (ccbEntry = ccbHead->Flink; ccbEntry != ccbHead; ccbEntry = ccbNext) {
			ccbNext = ccbEntry->Flink;
			ccb = CONTAINING_RECORD(ccbEntry, DokanCCB, NextCCB);

			DDbgPrint("  NotifyCleanup ccb:%X, context:%X, filename:%wZ\n",
					ccb, (ULONG)ccb->UserContext, &fcb->FileName);
			FsRtlNotifyCleanup(vcb->NotifySync, &vcb->DirNotifyList, ccb);
		}
		ExReleaseResourceLite(&fcb->Resource);
	}

	ExReleaseResourceLite(&vcb->Resource);
	KeLeaveCriticalRegion();

	ReleasePendingIrp(&dcb->PendingIrp);
	ReleasePendingIrp(&dcb->PendingEvent);
	DokanStopCheckThread(dcb);
	DokanStopEventNotificationThread(dcb);

	DokanDeleteDeviceObject(dcb);

	return status;
}