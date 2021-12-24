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
#include "util/irp_buffer_helper.h"
#include "util/str.h"

VOID DokanUnmount(__in_opt PREQUEST_CONTEXT RequestContext, __in PDokanDCB Dcb) {
  PDokanVCB vcb = Dcb->Vcb;

  DOKAN_LOG("Start");
  if (vcb) {
    DokanEventRelease(RequestContext, vcb->DeviceObject);
  }
  DOKAN_LOG("End");
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

  DOKAN_LOG("Start");
  InitializeListHead(&completeList);

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&Dcb->PendingIrp.ListLock, &oldIrql);

  // when IRP queue is empty, there is nothing to do
  if (IsListEmpty(&Dcb->PendingIrp.ListHead)) {
    KeReleaseSpinLock(&Dcb->PendingIrp.ListLock, oldIrql);
    DOKAN_LOG("IrpQueue is Empty");
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

    DOKAN_LOG_("Timeout Irp %p", irpEntry->SerialNumber);

    irp = irpEntry->RequestContext.Irp;

    // Create IRPs (ForcedCanceled) are special in that this routine is always
    // their place of effective cancellation. So we only care about races with
    // the cancel routine for other IRPs (which can be effectively canceled in
    // either place).
    if (!irpEntry->RequestContext.ForcedCanceled) {
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
    } else {
      // Cleanup ForcedCanceled IRP of the attached CancelRoutine before
      // Completion.
      IoSetCancelRoutine(irp, NULL);
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
    irp = irpEntry->RequestContext.Irp;
    PIO_STACK_LOCATION irpSp = irpEntry->RequestContext.IrpSp;
    if (irpSp->MajorFunction == IRP_MJ_CREATE) {
      BOOLEAN canceled = (irpEntry->TickCount.QuadPart == 0);
      PFILE_OBJECT fileObject = irpEntry->RequestContext.IrpSp->FileObject;
      if (fileObject != NULL) {
        PDokanCCB ccb = fileObject->FsContext2;
        if (ccb != NULL) {
          PDokanFCB fcb = ccb->Fcb;
          OplockDebugRecordFlag(
              fcb, canceled ? DOKAN_OPLOCK_DEBUG_CANCELED_CREATE
                            : DOKAN_OPLOCK_DEBUG_TIMED_OUT_CREATE);
        }
      }
      DokanCancelCreateIrp(&irpEntry->RequestContext,
          canceled ? STATUS_CANCELLED : STATUS_INSUFFICIENT_RESOURCES);
    } else {
      irp->IoStatus.Information = 0;
      DokanCompleteIrpRequest(irp, STATUS_INSUFFICIENT_RESOURCES);
    }
    DokanFreeIrpEntry(irpEntry);
  }

  if (shouldUnmount) {
    // This avoids a race condition where the app terminates before activating
    // the keepalive handle. In that case, we unmount the file system as soon
    // as some specific operation gets timed out, which avoids repeated delays
    // in Explorer.
    DokanLogInfo(
        &logger,
        L"Unmounting due to operation timeout before keepalive handle was"
        L" activated.");
    DokanUnmount(NULL, Dcb);
  }

  DOKAN_LOG("End");

  return STATUS_SUCCESS;
}

NTSTATUS
DokanResetPendingIrpTimeout(__in PREQUEST_CONTEXT RequestContext) {
  KIRQL oldIrql;
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PIRP_ENTRY irpEntry;
  PEVENT_INFORMATION eventInfo = NULL;
  ULONG timeout; // in milisecond

  GET_IRP_BUFFER_OR_RETURN(RequestContext->Irp, eventInfo);

  timeout = eventInfo->Operation.ResetTimeout.Timeout;
  if (DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX < timeout) {
    timeout = DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX;
  }

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, &oldIrql);

  // search corresponding IRP through pending IRP list
  listHead = &RequestContext->Dcb->PendingIrp.ListHead;

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
  KeReleaseSpinLock(&RequestContext->Dcb->PendingIrp.ListLock, oldIrql);
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
  PDokanDCB Dcb = pDcb;
  DOKAN_INIT_LOGGER(logger, Dcb->DeviceObject->DriverObject, 0);

  DOKAN_LOG("Start");

  KeInitializeTimerEx(&timer, SynchronizationTimer);

  pollevents[0] = (PVOID)&Dcb->KillEvent;
  pollevents[1] = (PVOID)&Dcb->ForceTimeoutEvent;
  pollevents[2] = (PVOID)&timer;

  KeSetTimerEx(&timer, timeout, DOKAN_CHECK_INTERVAL, NULL);

  KeQuerySystemTime(&LastTime);

  while (waitObj) {
    status = KeWaitForMultipleObjects(3, pollevents, WaitAny, Executive,
                                      KernelMode, FALSE, NULL, NULL);

    if (!NT_SUCCESS(status) || status == STATUS_WAIT_0) {
      DOKAN_LOG("DokanTimeoutThread catched KillEvent");
      // KillEvent or something error is occurred
      waitObj = FALSE;
    } else {
      KeClearEvent(&Dcb->ForceTimeoutEvent);
      // In this case the timer was executed and we are checking if the timer
      // occurred regulary using the period DOKAN_CHECK_INTERVAL. If not, this
      // means the system was in sleep mode.
      KeQuerySystemTime(&CurrentTime);
      if ((CurrentTime.QuadPart - LastTime.QuadPart) >
          ((DOKAN_CHECK_INTERVAL + 2000) * 10000)) {
        DokanLogInfo(&logger, L"Wake from sleep detected.");
      } else {
        ReleaseTimeoutPendingIrp(Dcb);
      }
      KeQuerySystemTime(&LastTime);
    }
  }

  KeCancelTimer(&timer);

  DOKAN_LOG("Stop");

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

  status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                (PKSTART_ROUTINE)DokanTimeoutThread, Dcb);

  if (!NT_SUCCESS(status)) {
    DOKAN_LOG("Failed to create Thread");
    return status;
  }

  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode,
                            (PVOID *)&Dcb->TimeoutThread, NULL);

  ZwClose(thread);

  return STATUS_SUCCESS;
}

VOID DokanStopCheckThread(__in PDokanDCB Dcb)
/*++

Routine Description:

        exits DokanTimeoutThread

--*/
{
  DOKAN_LOG("Stopping Thread");
  if (KeSetEvent(&Dcb->KillEvent, 0, FALSE) > 0 && Dcb->TimeoutThread) {
    DOKAN_LOG("Waiting for thread to terminate");
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    KeWaitForSingleObject(Dcb->TimeoutThread, Executive, KernelMode, FALSE,
                          NULL);
    DOKAN_LOG("Thread successfully terminated");
    ObDereferenceObject(Dcb->TimeoutThread);
    Dcb->TimeoutThread = NULL;
  }
}

VOID DokanUpdateTimeout(__out PLARGE_INTEGER TickCount, __in ULONG Timeout) {
  KeQueryTickCount(TickCount);
  TickCount->QuadPart += Timeout * 1000 * 10 / KeQueryTimeIncrement();
}
