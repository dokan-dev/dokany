/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2023 Google, Inc.

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
BOOLEAN DokanInitializeFcb(__in PREQUEST_CONTEXT RequestContext,
                           __in PDokanFCB Fcb) {
  Fcb->AdvancedFCBHeader.Resource =
      ExAllocateFromLookasideListEx(&g_DokanEResourceLookasideList);
  if (Fcb->AdvancedFCBHeader.Resource == NULL) {
    DOKAN_LOG_FINE_IRP(RequestContext,
                       "Failed to allocate FCB ERESOURCE for %p", Fcb);
    return FALSE;
  }

  Fcb->Identifier.Type = FCB;
  Fcb->Identifier.Size = sizeof(DokanFCB);

  Fcb->Vcb = RequestContext->Vcb;

  ExInitializeResourceLite(&Fcb->PagingIoResource);
  ExInitializeResourceLite(Fcb->AdvancedFCBHeader.Resource);

  ExInitializeFastMutex(&Fcb->AdvancedFCBHeaderMutex);

  FsRtlSetupAdvancedHeader(&Fcb->AdvancedFCBHeader,
                           &Fcb->AdvancedFCBHeaderMutex);

  // ValidDataLength not supported - initialize to 0x7fffffff / 0xffffffff
  // If fcb->Header.IsFastIoPossible was set the Cache manager would send
  // us a SetFilelnformation IRP to update this value
  Fcb->AdvancedFCBHeader.ValidDataLength.QuadPart = MAXLONGLONG;

  Fcb->AdvancedFCBHeader.PagingIoResource = &Fcb->PagingIoResource;

  Fcb->AdvancedFCBHeader.AllocationSize.QuadPart = 4096;
  Fcb->AdvancedFCBHeader.FileSize.QuadPart = 4096;

  Fcb->AdvancedFCBHeader.IsFastIoPossible = FastIoIsNotPossible;
  FsRtlInitializeOplock(DokanGetFcbOplock(Fcb));

  InitializeListHead(&Fcb->NextCCB);

  InterlockedIncrement(&RequestContext->Vcb->FcbAllocated);
  InterlockedAnd64(&RequestContext->Vcb->ValidFcbMask, (LONG64)Fcb);
  ++RequestContext->Vcb->VolumeMetrics.FcbAllocations;

  if (RtlEqualUnicodeString(&Fcb->FileName, &g_KeepAliveFileName, FALSE)) {
    Fcb->IsKeepalive = TRUE;
    Fcb->BlockUserModeDispatch = TRUE;
  }
  if (RtlEqualUnicodeString(&Fcb->FileName, &g_NotificationFileName, FALSE)) {
    Fcb->BlockUserModeDispatch = TRUE;
  }
  return TRUE;
}

PDokanFCB GetOrCreateUninitializedFcb(__in PREQUEST_CONTEXT RequestContext,
                                      __in PUNICODE_STRING FileName,
                                      __in PBOOLEAN NewElement) {
  PDokanFCB fcb = NULL;

  fcb = ExAllocateFromLookasideListEx(&g_DokanFCBLookasideList);
  // Try again if garbage collection frees up space. This is a no-op when
  // garbage collection is disabled.
  if (fcb == NULL && DokanForceFcbGarbageCollection(RequestContext->Vcb)) {
    fcb = ExAllocateFromLookasideListEx(&g_DokanFCBLookasideList);
  }
  if (fcb == NULL) {
    return NULL;
  }
  RtlZeroMemory(fcb, sizeof(DokanFCB));
  fcb->FileName = *FileName;

  PDokanFCB *fcbInTable = (PDokanFCB *)RtlInsertElementGenericTableAvl(
      &RequestContext->Vcb->FcbTable, &fcb, sizeof(PDokanFCB), NewElement);
  if (!fcbInTable) {
    ExFreeToLookasideListEx(&g_DokanFCBLookasideList, fcb);
    return NULL;
  }
  if (!(*NewElement)) {
    ExFreeToLookasideListEx(&g_DokanFCBLookasideList, fcb);
  }
  return *fcbInTable;
}

PDokanFCB DokanGetFCB(__in PREQUEST_CONTEXT RequestContext,
                      __in PWCHAR FileName, __in ULONG FileNameLength,
                      __out BOOLEAN* IsAlreadyOpen) {
  UNICODE_STRING fn = DokanWrapUnicodeString(FileName, FileNameLength);

  UNREFERENCED_PARAMETER(RequestContext);

  DokanVCBLockRW(RequestContext->Vcb);

  BOOLEAN newElement = FALSE;
  PDokanFCB fcb = GetOrCreateUninitializedFcb(RequestContext, &fn, &newElement);
  if (!fcb) {
    ExFreePool(FileName);
    DOKAN_LOG_FINE_IRP(RequestContext, "Failed to find or allocate FCB for %wZ",
                       &fn);
    DokanVCBUnlock(RequestContext->Vcb);
    return NULL;
  }

  if (newElement) {
    DOKAN_LOG_FINE_IRP(RequestContext, "New FCB %p allocated for %wZ", fcb,
                       &fcb->FileName);
    if (!DokanInitializeFcb(RequestContext, fcb)) {
      BOOLEAN removed =
          RtlDeleteElementGenericTableAvl(&RequestContext->Vcb->FcbTable, &fcb);
      ASSERT(removed);
      UNREFERENCED_PARAMETER(removed);
      DOKAN_LOG_FINE_IRP(RequestContext, "Failed to init FCB %p for %wZ", fcb,
                         &fcb->FileName);
      ExFreePool(FileName);
      ExFreeToLookasideListEx(&g_DokanFCBLookasideList, fcb);
      DokanVCBUnlock(RequestContext->Vcb);
      return NULL;
    }
  } else {
    DOKAN_LOG_FINE_IRP(RequestContext, "Found existing FCB %p for %wZ", fcb,
                       &fcb->FileName);
    DokanCancelFcbGarbageCollection(fcb, &fn);
  }

  LONG openCount = InterlockedIncrement(&fcb->FileCount);
  *IsAlreadyOpen = openCount > 1;
  DokanVCBUnlock(RequestContext->Vcb);
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

  // We need to be able to remove a FileObject that is not the final one,
  // without the VCB lock. This is because CcPurgeCacheSection may trigger a
  // close of one FileObject from within a cleanup of another (with the same
  // FCB). In that case, there is already a FCB lock held below us on the stack,
  // making it unsafe to acquire a VCB lock.
  if (InterlockedDecrement(&Fcb->FileCount) != 0) {
    return STATUS_SUCCESS;
  }

  DokanVCBLockRW(Vcb);
  DokanFCBLockRW(Fcb);

  // Note that the FileCount could theoretically be nonzero if incremented by
  // another thread after the early return and before the locking of the VCB.
  // The code that increments it does so with the VCB locked, so at this point
  // we are sure.
  if (Fcb->FileCount == 0 && !DokanScheduleFcbForGarbageCollection(Vcb, Fcb)) {
    // We get here when garbage collection is disabled.
    DokanDeleteFcb(Vcb, Fcb, /*RemoveFromTable=*/!Fcb->ReplacedByRename);
  } else {
    DokanFCBUnlock(Fcb);
  }

  DokanVCBUnlock(Vcb);
  return STATUS_SUCCESS;
}

VOID DokanDeleteFcb(__in PDokanVCB Vcb, __in PDokanFCB Fcb,
                    __in BOOLEAN DeleteFromTable) {
  ++Vcb->VolumeMetrics.FcbDeletions;

  if (DeleteFromTable) {
    BOOLEAN removed = RtlDeleteElementGenericTableAvl(&Vcb->FcbTable, &Fcb);
    ASSERT(removed);
    UNREFERENCED_PARAMETER(removed);
  }
  InitializeListHead(&Fcb->NextCCB);

  DOKAN_LOG_("Free FCB %p", Fcb);

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
  if (Fcb->ReplacedByRename) {
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
    DokanFCBFlagsClearBit(Fcb, DOKAN_FILE_CHANGE_LAST_WRITE);
  } else {
    // It's not actually scheduled for GC, so this function is a no-op aside
    // from its obligation to clean up the string.
    ExFreePool(NewFileName->Buffer);
  }
  NewFileName->Buffer = NULL;
  NewFileName->Length = 0;
}

VOID GarbageCollectFCB(__in PDokanVCB Vcb, __in PDokanFCB Fcb,
                       __in BOOLEAN RemoveFromTable) {
  RemoveEntryList(&Fcb->NextGarbageCollectableFcb);
  DokanFCBLockRW(Fcb);
  DokanDeleteFcb(Vcb, Fcb, RemoveFromTable);
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
      GarbageCollectFCB(Vcb, nextFcb, /*RemoveFromTable=*/TRUE);
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

RTL_GENERIC_COMPARE_RESULTS DokanCompareFcb(__in struct _RTL_AVL_TABLE *Table,
                                            __in PVOID FirstStruct,
                                            __in PVOID SecondStruct) {
  PDokanVCB vcb = (PDokanVCB)Table->TableContext;
  PDokanFCB firstFcb = *(PDokanFCB *)FirstStruct;
  PDokanFCB secondFcb = *(PDokanFCB *)SecondStruct;
  LONG result = RtlCompareUnicodeString(
      &firstFcb->FileName, &secondFcb->FileName,
      !(vcb->Dcb->MountOptions & DOKAN_EVENT_CASE_SENSITIVE));
  DOKAN_LOG_VCB(vcb, "First: %p %wZ Second: %p %wZ - Result: %ld", firstFcb,
                &firstFcb->FileName, secondFcb, &secondFcb->FileName, result);
  if (result < 0) {
    return GenericLessThan;
  } else if (result > 0) {
    return GenericGreaterThan;
  }
  return GenericEqual;
}

PVOID DokanAllocateFcbAvl(__in struct _RTL_AVL_TABLE *Table,
                          __in CLONG ByteSize) {
  PDokanVCB vcb = (PDokanVCB)Table->TableContext;
  if (!vcb->FCBAvlNodeLookasideListInit) {
    if (!DokanLookasideCreate(&vcb->FCBAvlNodeLookasideList, ByteSize)) {
      DOKAN_LOG_VCB(vcb,
                    "DokanLookasideCreate VCB FCBAvlNodeLookasideList failed.");
      return NULL;
    }
    vcb->FCBAvlNodeLookasideListInit = TRUE;
  }
  return ExAllocateFromLookasideListEx(&vcb->FCBAvlNodeLookasideList);
}

VOID DokanFreeFcbAvl(__in struct _RTL_AVL_TABLE *Table, __in PVOID Buffer) {
  PDokanVCB vcb = (PDokanVCB)Table->TableContext;
  ExFreeToLookasideListEx(&vcb->FCBAvlNodeLookasideList, Buffer);
}

VOID DokanRenameFcb(__in PREQUEST_CONTEXT RequestContext, __in PDokanFCB Fcb,
                    __in PWCH FileName, __in USHORT FileNameLength) {
  BOOLEAN removed =
      RtlDeleteElementGenericTableAvl(&RequestContext->Vcb->FcbTable, &Fcb);
  ASSERT(removed);
  UNREFERENCED_PARAMETER(removed);

  Fcb->FileName = DokanWrapUnicodeString(FileName, FileNameLength);

  BOOLEAN newElement = FALSE;
  PDokanFCB *fcbInTable = (PDokanFCB *)RtlInsertElementGenericTableAvl(
      &RequestContext->Vcb->FcbTable, &Fcb, sizeof(PDokanFCB), &newElement);
  ASSERT(fcbInTable);
  if (newElement) {
    return;
  }

  PDokanFCB conflictingFcb = *fcbInTable;
  ASSERT(!conflictingFcb->ReplacedByRename);
  // An Fcb with the same name already exists in the table and needs to be
  // removed to allow the new Fcb to take over.
  if (conflictingFcb->NextGarbageCollectableFcb.Flink) {
    // The Fcb is pending GC. Force it's deletion now.
    GarbageCollectFCB(RequestContext->Vcb, conflictingFcb,
                      /*RemoveFromTable=*/FALSE);
  } else {
    // This cannot happen on NTFS. See Fcb::PendingDeletion doc.
    conflictingFcb->ReplacedByRename = TRUE;
  }

  // Reinsert the Fcb with the updated name
  *fcbInTable = Fcb;
}