/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.

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

#include "fcb.h"
#include "str.h"

const UNICODE_STRING g_KeepAliveFileName =
    RTL_CONSTANT_STRING(DOKAN_KEEPALIVE_FILE_NAME);

const UNICODE_STRING g_NotificationFileName =
    RTL_CONSTANT_STRING(DOKAN_NOTIFICATION_FILE_NAME);

// We must NOT call without VCB lock
PDokanFCB DokanAllocateFCB(__in PDokanVCB Vcb, __in PWCHAR FileName,
                           __in ULONG FileNameLength) {
  PDokanFCB fcb = ExAllocateFromLookasideListEx(&g_DokanFCBLookasideList);

  // Try again if garbage collection frees up space. This is a no-op when
  // garbage collection is disabled.
  if (fcb == NULL && DokanForceFcbGarbageCollection(Vcb)) {
    fcb = ExAllocateFromLookasideListEx(&g_DokanFCBLookasideList);
  }

  if (fcb == NULL) {
    return NULL;
  }

  ASSERT(Vcb != NULL);

  RtlZeroMemory(fcb, sizeof(DokanFCB));

  fcb->AdvancedFCBHeader.Resource =
      ExAllocateFromLookasideListEx(&g_DokanEResourceLookasideList);
  if (fcb->AdvancedFCBHeader.Resource == NULL) {
    ExFreeToLookasideListEx(&g_DokanFCBLookasideList, fcb);
    return NULL;
  }

  fcb->Identifier.Type = FCB;
  fcb->Identifier.Size = sizeof(DokanFCB);

  fcb->Vcb = Vcb;

  ExInitializeResourceLite(&fcb->PagingIoResource);
  ExInitializeResourceLite(fcb->AdvancedFCBHeader.Resource);

  ExInitializeFastMutex(&fcb->AdvancedFCBHeaderMutex);

  FsRtlSetupAdvancedHeader(&fcb->AdvancedFCBHeader,
                           &fcb->AdvancedFCBHeaderMutex);

  // ValidDataLength not supported - initialize to 0x7fffffff / 0xffffffff
  // If fcb->Header.IsFastIoPossible was set the Cache manager would send
  // us a SetFilelnformation IRP to update this value
  fcb->AdvancedFCBHeader.ValidDataLength.QuadPart = MAXLONGLONG;

  fcb->AdvancedFCBHeader.PagingIoResource = &fcb->PagingIoResource;

  fcb->AdvancedFCBHeader.AllocationSize.QuadPart = 4096;
  fcb->AdvancedFCBHeader.FileSize.QuadPart = 4096;

  fcb->AdvancedFCBHeader.IsFastIoPossible = FastIoIsNotPossible;
  FsRtlInitializeOplock(DokanGetFcbOplock(fcb));

  fcb->FileName.Buffer = FileName;
  fcb->FileName.Length = (USHORT)FileNameLength;
  fcb->FileName.MaximumLength = (USHORT)FileNameLength;

  InitializeListHead(&fcb->NextCCB);
  InsertTailList(&Vcb->NextFCB, &fcb->NextFCB);

  InterlockedIncrement(&Vcb->FcbAllocated);
  InterlockedAnd64(&Vcb->ValidFcbMask, (LONG64)fcb);
  ++Vcb->VolumeMetrics.FcbAllocations;
  return fcb;
}

PDokanFCB DokanGetFCB(__in PDokanVCB Vcb, __in PWCHAR FileName,
                      __in ULONG FileNameLength, BOOLEAN CaseInSensitive) {
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PDokanFCB fcb = NULL;
  UNICODE_STRING fn = DokanWrapUnicodeString(FileName, FileNameLength);

  DokanVCBLockRW(Vcb);

  // search the FCB which is already allocated
  // (being used now)
  listHead = &Vcb->NextFCB;

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {

    nextEntry = thisEntry->Flink;

    fcb = CONTAINING_RECORD(thisEntry, DokanFCB, NextFCB);
    DDbgPrint("  DokanGetFCB has entry FileName: %wZ FileCount: %lu. Looking "
              "for %ls CaseInSensitive %d\n",
              &fcb->FileName, fcb->FileCount, FileName, CaseInSensitive);
    if (fcb->FileName.Length == FileNameLength  // FileNameLength in bytes
        && RtlEqualUnicodeString(&fn, &fcb->FileName, CaseInSensitive)) {
      // we have the FCB which is already allocated and used
      DDbgPrint("  Found existing FCB for %ls\n", FileName);
      break;
    }

    fcb = NULL;
  }

  // we don't have FCB
  if (fcb == NULL) {
    DDbgPrint("  Allocate FCB for %ls\n", FileName);

    fcb = DokanAllocateFCB(Vcb, FileName, FileNameLength);

    // no memory?
    if (fcb == NULL) {
      DDbgPrint("    Was not able to get FCB for FileName %ls\n", FileName);
      ExFreePool(FileName);
      DokanVCBUnlock(Vcb);
      return NULL;
    }

    ASSERT(fcb != NULL);
    if (RtlEqualUnicodeString(&fcb->FileName, &g_KeepAliveFileName, FALSE)) {
      fcb->IsKeepalive = TRUE;
      fcb->BlockUserModeDispatch = TRUE;
    }
    if (RtlEqualUnicodeString(&fcb->FileName, &g_NotificationFileName, FALSE)) {
      fcb->BlockUserModeDispatch = TRUE;
    }

    // we already have FCB
  } else {
    DokanCancelFcbGarbageCollection(fcb, &fn);
  }

  InterlockedIncrement(&fcb->FileCount);
  DokanVCBUnlock(Vcb);
  return fcb;
}

NTSTATUS
DokanFreeFCB(__in PDokanVCB Vcb, __in PDokanFCB Fcb) {
  DOKAN_INIT_LOGGER(logger, Vcb->DeviceObject->DriverObject, 0);
  DokanBackTrace trace = {0};
  ASSERT(Vcb != NULL);
  ASSERT(Fcb != NULL);

  // First try to make sure the FCB is good. We have had some BSODs trying to
  // access fields in an invalid FCB before adding these checks.

  if (GetIdentifierType(Vcb) != VCB) {
    DokanCaptureBackTrace(&trace);
    return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
        L"Freeing an FCB with an invalid VCB at %I64x:%I64x,"
        L" identifier type: %x",
        trace.Address, trace.ReturnAddresses, GetIdentifierType(Vcb));
  }

  // This check should identify wildly bogus FCB addresses like 12345.
  LONG64 validFcbMask = Vcb->ValidFcbMask;
  if ((validFcbMask & (LONG64)Fcb) != validFcbMask) {
    DokanCaptureBackTrace(&trace);
    return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
        L"Freeing invalid FCB at %I64x:%I64x: %I64x, which does not match mask:"
        L" %I64x",
        trace.Address, trace.ReturnAddresses, Fcb, validFcbMask);
  }

  // Hopefully if it passes the above check we can at least dereference it,
  // although that's not necessarily true. If we can read 4 bytes at the
  // address, we can determine if it's an invalid or already freed FCB.
  if (GetIdentifierType(Fcb) != FCB) {
    DokanCaptureBackTrace(&trace);
    return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
        L"Freeing FCB that has wrong identifier type at %I64x:%I64x: %x",
        trace.Address, trace.ReturnAddresses, GetIdentifierType(Fcb));
  }

  ASSERT(Fcb->Vcb == Vcb);

  DokanVCBLockRW(Vcb);
  DokanFCBLockRW(Fcb);

  if (InterlockedDecrement(&Fcb->FileCount) == 0 &&
      !DokanScheduleFcbForGarbageCollection(Vcb, Fcb)) {
    // We get here when garbage collection is disabled.
    DokanDeleteFcb(Vcb, Fcb);
  } else {
    DokanFCBUnlock(Fcb);
  }

  DokanVCBUnlock(Vcb);
  return STATUS_SUCCESS;
}

VOID DokanDeleteFcb(__in PDokanVCB Vcb, __in PDokanFCB Fcb) {
  ++Vcb->VolumeMetrics.FcbDeletions;
  RemoveEntryList(&Fcb->NextFCB);
  InitializeListHead(&Fcb->NextCCB);

  DDbgPrint("  Free FCB:%p\n", Fcb);

  ExFreePool(Fcb->FileName.Buffer);
  Fcb->FileName.Buffer = NULL;
  Fcb->FileName.Length = 0;
  Fcb->FileName.MaximumLength = 0;

  FsRtlUninitializeOplock(DokanGetFcbOplock(Fcb));

  FsRtlTeardownPerStreamContexts(&Fcb->AdvancedFCBHeader);

  Fcb->Identifier.Type = FREED_FCB;
  DokanFCBUnlock(Fcb);
  ExDeleteResourceLite(Fcb->AdvancedFCBHeader.Resource);
  ExFreeToLookasideListEx(&g_DokanEResourceLookasideList,
                          Fcb->AdvancedFCBHeader.Resource);
  ExDeleteResourceLite(&Fcb->PagingIoResource);

  InterlockedIncrement(&Vcb->FcbFreed);
  ExFreeToLookasideListEx(&g_DokanFCBLookasideList, Fcb);
}

BOOLEAN DokanScheduleFcbForGarbageCollection(__in PDokanVCB Vcb,
                                             __in PDokanFCB Fcb) {
  DOKAN_INIT_LOGGER(logger, Vcb->Dcb->DeviceObject->DriverObject, 0);
  if (Vcb->FcbGarbageCollectorThread == NULL) {
    return FALSE;
  }
  if (Fcb->NextGarbageCollectableFcb.Flink != NULL) {
    // This is probably not intentional but theoretically OK.
    DokanLogInfo(&logger,
                 L"Warning: scheduled an FCB for garbage collection when it is"
                 L" already scheduled.");
    return TRUE;
  }
  Fcb->GarbageCollectionGracePeriodPassed = FALSE;
  InsertTailList(&Vcb->FcbGarbageList, &Fcb->NextGarbageCollectableFcb);
  KeSetEvent(&Vcb->FcbGarbageListNotEmpty, IO_NO_INCREMENT, FALSE);
  return TRUE;
}

VOID DokanCancelFcbGarbageCollection(__in PDokanFCB Fcb,
                                     _Inout_ PUNICODE_STRING NewFileName) {
  if (Fcb->NextGarbageCollectableFcb.Flink != NULL) {
    ++Fcb->Vcb->VolumeMetrics.FcbGarbageCollectionCancellations;
    // Update the case of the file name and clear flags. Note that there cannot
    // be concurrent use of an FCB in the GC list while we are in this function.
    ASSERT(Fcb->FileName.Length == NewFileName->Length);
    ExFreePool(Fcb->FileName.Buffer);
    Fcb->FileName = *NewFileName;
    RemoveEntryList(&Fcb->NextGarbageCollectableFcb);
    Fcb->NextGarbageCollectableFcb.Flink = NULL;
    Fcb->GarbageCollectionGracePeriodPassed = FALSE;
    DokanFCBFlagsClearBit(Fcb, DOKAN_DELETE_ON_CLOSE);
    DokanFCBFlagsClearBit(Fcb, DOKAN_FILE_DIRECTORY);
  } else {
    // It's not actually scheduled for GC, so this function is a no-op aside
    // from its obligation to clean up the string.
    ExFreePool(NewFileName->Buffer);
  }
  NewFileName->Buffer = NULL;
  NewFileName->Length = 0;
}

// Called with the VCB locked. Immediately deletes the FCBs that are ready to
// delete. Returns how many are skipped due to having been scheduled too
// recently. If Force is TRUE then all the scheduled ones are deleted, and the
// return value is 0.
ULONG DeleteFcbGarbageAndGetRemainingCount(__in PDokanVCB Vcb,
                                           __in BOOLEAN Force) {
  ULONG remainingCount = 0;
  PLIST_ENTRY thisEntry = NULL;
  PLIST_ENTRY nextEntry = NULL;
  PDokanFCB nextFcb = NULL;
  for (thisEntry = Vcb->FcbGarbageList.Flink; thisEntry != &Vcb->FcbGarbageList;
       thisEntry = nextEntry) {
    nextEntry = thisEntry->Flink;
    nextFcb = CONTAINING_RECORD(thisEntry, DokanFCB, NextGarbageCollectableFcb);
    // We want it to have been scheduled for at least one timer interval so
    // that there is a guaranteed window of possible reuse, which achieves the
    // performance gains we are aiming for with GC.
    if (Force || nextFcb->GarbageCollectionGracePeriodPassed) {
      RemoveEntryList(thisEntry);
      DokanFCBLockRW(nextFcb);
      DokanDeleteFcb(Vcb, nextFcb);
    } else {
      nextFcb->GarbageCollectionGracePeriodPassed = TRUE;
      ++remainingCount;
    }
  }
  ASSERT(!Force || remainingCount == 0);
  // When an FCB gets deleted by a GC cycle already in progress at the time of
  // its scheduling, there's no point in triggering a follow-up cycle for that
  // one.
  if (remainingCount == 0) {
    KeClearEvent(&Vcb->FcbGarbageListNotEmpty);
  }
  return remainingCount;
}

BOOLEAN DokanForceFcbGarbageCollection(__in PDokanVCB Vcb) {
  if (Vcb->FcbGarbageCollectorThread == NULL ||
      IsListEmpty(&Vcb->FcbGarbageList)) {
    return FALSE;
  }
  ++Vcb->VolumeMetrics.ForcedFcbGarbageCollectionPasses;
  DeleteFcbGarbageAndGetRemainingCount(Vcb, /*Force=*/TRUE);
  return TRUE;
}

// Called when there are no pending garbage FCBs and we may need to wait
// indefinitely for one to appear.
NTSTATUS WaitForNewFcbGarbage(__in PDokanVCB Vcb) {
  PVOID events[2];
  events[0] = &Vcb->Dcb->ReleaseEvent;
  events[1] = &Vcb->FcbGarbageListNotEmpty;
  NTSTATUS status = KeWaitForMultipleObjects(2, events, WaitAny, Executive,
                                             KernelMode, FALSE, NULL, NULL);
  return status == STATUS_WAIT_1 ? STATUS_SUCCESS : STATUS_CANCELLED;
}

// Called when there are some pending garbage FCBs. This function keeps an eye
// on them until they expire and then deletes them, returning when there are no
// more pending ones.
NTSTATUS AgeAndDeleteFcbGarbage(__in PDokanVCB Vcb, __in PKTIMER Timer) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  ULONG pendingCount = 0;
  PVOID events[2];
  BOOLEAN waited = FALSE;
  events[0] = &Vcb->Dcb->ReleaseEvent;
  events[1] = Timer;
  ++Vcb->VolumeMetrics.NormalFcbGarbageCollectionCycles;
  for (;;) {
    // Get rid of any garbage that is ready to delete.
    DokanVCBLockRW(Vcb);
    ++Vcb->VolumeMetrics.NormalFcbGarbageCollectionPasses;
    pendingCount = DeleteFcbGarbageAndGetRemainingCount(Vcb, /*Force=*/FALSE);
    DokanVCBUnlock(Vcb);
    // If we have cleared out all the garbage, return so the garbage collector
    // will do an indefinite wait for new garbage. But we wait at least once on
    // the GC interval timer to avoid having multiple no-op cycles in one
    // interval.
    if (pendingCount == 0 && waited) {
      status = STATUS_SUCCESS;
      break;
    }
    // If there are any entries that haven't aged long enough, age them using
    // the timer until they are ready.
    status = KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode,
                                      FALSE, NULL, NULL);
    waited = TRUE;
    if (status != STATUS_WAIT_1) {
      status = STATUS_CANCELLED;
      break;
    }
  }
  return status;
}

// The thread function for the dedicated FCB garbage collection thread.
VOID FcbGarbageCollectorThread(__in PVOID pVcb) {
  KTIMER timer;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  LARGE_INTEGER timeout = {0};
  PDokanVCB Vcb = pVcb;
  DOKAN_INIT_LOGGER(logger, Vcb->Dcb->DeviceObject->DriverObject, 0);
  KeInitializeTimerEx(&timer, SynchronizationTimer);
  KeSetTimerEx(&timer, timeout, Vcb->Dcb->FcbGarbageCollectionIntervalMs, NULL);
  DokanLogInfo(&logger, L"Starting FCB garbage collector with %lu ms interval.",
               Vcb->Dcb->FcbGarbageCollectionIntervalMs);
  for (;;) {
    status = WaitForNewFcbGarbage(Vcb);
    if (status != STATUS_SUCCESS) {
      break;
    }
    status = AgeAndDeleteFcbGarbage(Vcb, &timer);
    if (status != STATUS_SUCCESS) {
      break;
    }
  }
  DokanLogInfo(&logger, L"Stopping FCB garbage collector.");
  KeCancelTimer(&timer);
}

void DokanStartFcbGarbageCollector(PDokanVCB Vcb) {
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  HANDLE thread = NULL;
  Vcb->FcbGarbageCollectorThread = NULL;
  if (Vcb->Dcb->FcbGarbageCollectionIntervalMs == 0) {
    return;
  }
  status =
      PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                           (PKSTART_ROUTINE)FcbGarbageCollectorThread, Vcb);
  if (!NT_SUCCESS(status)) {
    // Note: we will revert to shared_ptr-style deletion if the thread is NULL.
    return;
  }
  ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode,
                            (PVOID *)&Vcb->FcbGarbageCollectorThread, NULL);

  ZwClose(thread);
}