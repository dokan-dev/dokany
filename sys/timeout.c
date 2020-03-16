/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2017 Google, Inc.
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

VOID DokanUnmount(__in PDokanDCB Dcb) {
  ULONG eventLength;
  PEVENT_CONTEXT eventContext;
  PDRIVER_EVENT_CONTEXT driverEventContext;
  PKEVENT completedEvent;
  LARGE_INTEGER timeout;
  PDokanVCB vcb = Dcb->Vcb;
  ULONG deviceNamePos;

  DDbgPrint("==> DokanUnmount\n");

  eventLength = sizeof(EVENT_CONTEXT);
  eventContext = AllocateEventContextRaw(eventLength);

  if (eventContext == NULL) {
    ; // STATUS_INSUFFICIENT_RESOURCES;
    DDbgPrint(" Not able to allocate eventContext.\n");
    if (vcb) {
      DokanEventRelease(vcb->DeviceObject, NULL);
    }
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
  deviceNamePos = DokanSearchWcharinUnicodeStringWithUlong(
      Dcb->SymbolicLinkName, L'\\', deviceNamePos, 0);

  RtlStringCchCopyW(eventContext->Operation.Unmount.DeviceName,
                    sizeof(eventContext->Operation.Unmount.DeviceName) /
                        sizeof(WCHAR),
                    &(Dcb->SymbolicLinkName->Buffer[deviceNamePos]));

  DDbgPrint("  Send Unmount to Service : %ws\n",
            eventContext->Operation.Unmount.DeviceName);

  DokanEventNotification(&Dcb->Global->NotifyService, eventContext);

  if (completedEvent) {
    timeout.QuadPart = -1 * 10 * 1000 * 10; // 10 sec
    KeWaitForSingleObject(completedEvent, Executive, KernelMode, FALSE,
                          &timeout);
  }

  if (vcb) {
    DokanEventRelease(vcb->DeviceObject, NULL);
  }

  if (completedEvent) {
    ExFreePool(completedEvent);
  }

  DDbgPrint("<== DokanUnmount\n");
}

VOID DokanCheckKeepAlive(__in PDokanDCB Dcb) {
  LARGE_INTEGER tickCount;
  PDokanVCB vcb;

  // DDbgPrint("==> DokanCheckKeepAlive\n");

  KeEnterCriticalRegion();
  KeQueryTickCount(&tickCount);
  ExAcquireResourceSharedLite(&Dcb->Resource, TRUE);

  if (Dcb->TickCount.QuadPart < tickCount.QuadPart) {

    vcb = Dcb->Vcb;

    ExReleaseResourceLite(&Dcb->Resource);

    DDbgPrint("  Timeout reached so perform an umount\n");

    if (IsUnmountPendingVcb(vcb)) {
      DDbgPrint("  Volume is not mounted\n");
      KeLeaveCriticalRegion();
      return;
    }
    DokanUnmount(Dcb);

  } else {
    ExReleaseResourceLite(&Dcb->Resource);
  }

  KeLeaveCriticalRegion();
  // DDbgPrint("<== DokanCheckKeepAlive\n");
}

NTSTATUS
ReleaseTimeoutPendingIrp(__in PDokanDCB Dcb) {
  KIRQL oldIrql;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry;
  LARGE_INTEGER tickCount;
  LIST_ENTRY completeList;
  PIRP irp;
  BOOLEAN shouldUnmount = FALSE;
  PDokanVCB vcb = Dcb->Vcb;
  DOKAN_INIT_LOGGER(logger, Dcb->DeviceObject->DriverObject, 0);

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

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {

    nextEntry = thisEntry->Flink;

    irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

    // If an async operation (like an oplock break or CancelIoEx call from user
    // mode) has set the AsyncStatus to a failure status, then we clean up that
    // IRP as if it had timed out but use the status. The normal way an IRP gets
    // timed out is by its TickCount being too long ago. Continuing here means
    // the IRP is not eligible for cleanup in either way.
    if (irpEntry->AsyncStatus == STATUS_SUCCESS &&
        tickCount.QuadPart < irpEntry->TickCount.QuadPart) {
      continue;
    }

    RemoveEntryList(thisEntry);

    DDbgPrint(" timeout Irp #%X\n", irpEntry->SerialNumber);

    irp = irpEntry->Irp;

    // Create IRPs are special in that this routine is always their place of
    // effective cancellation. So we only care about races with the cancel
    // routine for other IRPs (which can be effectively canceled in either
    // place).
    if (irpEntry->IrpSp->MajorFunction != IRP_MJ_CREATE) {
      if (irp == NULL) {
        // Already canceled previously.
        ASSERT(irpEntry->CancelRoutineFreeMemory == FALSE);
        DokanFreeIrpEntry(irpEntry);
        continue;
      }
      if (IoSetCancelRoutine(irp, NULL) == NULL) {
        // Cancel routine is already destined to run.
        InitializeListHead(&irpEntry->ListEntry);
        irpEntry->CancelRoutineFreeMemory = TRUE;
        continue;
      }
    }

    // Prevent possible future runs of the cancel routine from doing anything.
    irp->Tail.Overlay.DriverContext[DRIVER_CONTEXT_IRP_ENTRY] = NULL;

    InsertTailList(&completeList, &irpEntry->ListEntry);
  }

  if (IsListEmpty(&Dcb->PendingIrp.ListHead)) {
    KeClearEvent(&Dcb->PendingIrp.NotEmpty);
  }
  KeReleaseSpinLock(&Dcb->PendingIrp.ListLock, oldIrql);

  shouldUnmount = !vcb->IsKeepaliveActive && !IsListEmpty(&completeList);
  while (!IsListEmpty(&completeList)) {
    listHead = RemoveHeadList(&completeList);
    irpEntry = CONTAINING_RECORD(listHead, IRP_ENTRY, ListEntry);
    irp = irpEntry->Irp;
    PIO_STACK_LOCATION irpSp = irpEntry->IrpSp;
    if (irpSp->MajorFunction == IRP_MJ_CREATE) {
      BOOLEAN canceled = (irpEntry->TickCount.QuadPart == 0);
      PFILE_OBJECT fileObject = irpEntry->FileObject;
      if (fileObject != NULL) {
        PDokanCCB ccb = fileObject->FsContext2;
        if (ccb != NULL) {
          PDokanFCB fcb = ccb->Fcb;
          OplockDebugRecordFlag(
              fcb, canceled ? DOKAN_OPLOCK_DEBUG_CANCELED_CREATE
                            : DOKAN_OPLOCK_DEBUG_TIMED_OUT_CREATE);
        }
      }
      DokanCancelCreateIrp(
          Dcb->DeviceObject, irpEntry,
          canceled ? STATUS_CANCELLED : STATUS_INSUFFICIENT_RESOURCES);
    } else {
      DokanCompleteIrpRequest(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    DokanFreeIrpEntry(irpEntry);
  }

  DDbgPrint("<== ReleaseTimeoutPendingIRP\n");

  if (shouldUnmount) {
    // This avoids a race condition where the app terminates before activating
    // the keepalive handle. In that case, we unmount the file system as soon
    // as some specific operation gets timed out, which avoids repeated delays
    // in Explorer.
    DokanLogInfo(
        &logger,
        L"Unmounting due to operation timeout before keepalive handle was"
        L" activated.");
    DokanUnmount(Dcb);
  }
  return STATUS_SUCCESS;
}

NTSTATUS
DokanResetPendingIrpTimeout(__in PDEVICE_OBJECT DeviceObject,
                            _Inout_ PIRP Irp) {
  KIRQL oldIrql;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry;
  PDokanVCB vcb;
  PEVENT_INFORMATION eventInfo;
  ULONG timeout; // in milisecond

  DDbgPrint("==> ResetPendingIrpTimeout\n");

  eventInfo = (PEVENT_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
  ASSERT(eventInfo != NULL);

  timeout = eventInfo->Operation.ResetTimeout.Timeout;
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

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {

    nextEntry = thisEntry->Flink;

    irpEntry = CONTAINING_RECORD(thisEntry, IRP_ENTRY, ListEntry);

    if (irpEntry->SerialNumber != eventInfo->SerialNumber) {
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
VOID DokanTimeoutThread(PVOID pDcb)
/*++

Routine Description:

        checks wheter pending IRP is timeout or not each DOKAN_CHECK_INTERVAL

--*/
{
  NTSTATUS status;
  KTIMER timer;
  PVOID pollevents[3];
  LARGE_INTEGER timeout = {0};
  BOOLEAN waitObj = TRUE;
  LARGE_INTEGER LastTime = {0};
  LARGE_INTEGER CurrentTime = {0};
  PDokanVCB vcb;
  PDokanDCB Dcb = pDcb;
  DOKAN_INIT_LOGGER(logger, Dcb->DeviceObject->DriverObject, 0);

  DDbgPrint("==> DokanTimeoutThread\n");

  KeInitializeTimerEx(&timer, SynchronizationTimer);

  pollevents[0] = (PVOID)&Dcb->KillEvent;
  pollevents[1] = (PVOID)&Dcb->ForceTimeoutEvent;
  pollevents[2] = (PVOID)&timer;

  vcb = Dcb->Vcb;

  KeSetTimerEx(&timer, timeout, DOKAN_CHECK_INTERVAL, NULL);

  KeQuerySystemTime(&LastTime);

  while (waitObj) {
    status = KeWaitForMultipleObjects(3, pollevents, WaitAny, Executive,
                                      KernelMode, FALSE, NULL, NULL);

    if (!NT_SUCCESS(status) || status == STATUS_WAIT_0) {
      DDbgPrint("  DokanTimeoutThread catched KillEvent\n");
      // KillEvent or something error is occurred
      waitObj = FALSE;
    } else {
      KeClearEvent(&Dcb->ForceTimeoutEvent);
      // in this case the timer was executed and we are checking if the timer
      // occurred regulary using the period DOKAN_CHECK_INTERVAL. If not, this
      // means the system was in sleep mode. If in this case the timer is
      // faster awaken than the incoming IOCTL_KEEPALIVE
      // the MountPoint would be removed by mistake (DokanCheckKeepAlive).
      KeQuerySystemTime(&CurrentTime);
      if ((CurrentTime.QuadPart - LastTime.QuadPart) >
          ((DOKAN_CHECK_INTERVAL + 2000) * 10000)) {
        DokanLogInfo(&logger, L"Wake from sleep detected.");
      } else {
        ReleaseTimeoutPendingIrp(Dcb);
        if (!vcb->IsKeepaliveActive)
          DokanCheckKeepAlive(Dcb); //Remove for Dokan 2.x.x
      }
      KeQuerySystemTime(&LastTime);
    }
  }

  KeCancelTimer(&timer);

  DDbgPrint("<== DokanTimeoutThread\n");

  PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
DokanStartCheckThread(__in PDokanDCB Dcb)
/*++

Routine Description:

        execute DokanTimeoutThread

--*/
{
  NTSTATUS status;
  HANDLE thread;

  DDbgPrint("==> DokanStartCheckThread\n");

  status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                (PKSTART_ROUTINE)DokanTimeoutThread, Dcb);

  if (!NT_SUCCESS(status)) {
    return status;
  }

  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode,
                            (PVOID *)&Dcb->TimeoutThread, NULL);

  ZwClose(thread);

  DDbgPrint("<== DokanStartCheckThread\n");

  return STATUS_SUCCESS;
}

VOID DokanStopCheckThread(__in PDokanDCB Dcb)
/*++

Routine Description:

        exits DokanTimeoutThread

--*/
{
  DDbgPrint("==> DokanStopCheckThread\n");

  if (KeSetEvent(&Dcb->KillEvent, 0, FALSE) > 0 && Dcb->TimeoutThread) {
    DDbgPrint("Waiting for Timeout thread to terminate.\n");
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    KeWaitForSingleObject(Dcb->TimeoutThread, Executive, KernelMode, FALSE,
                          NULL);
    DDbgPrint("Timeout thread successfully terminated.\n");
    ObDereferenceObject(Dcb->TimeoutThread);
    Dcb->TimeoutThread = NULL;
  }

  DDbgPrint("<== DokanStopCheckThread\n");
}

VOID DokanUpdateTimeout(__out PLARGE_INTEGER TickCount, __in ULONG Timeout) {
  KeQueryTickCount(TickCount);
  TickCount->QuadPart += Timeout * 1000 * 10 / KeQueryTimeIncrement();
}
