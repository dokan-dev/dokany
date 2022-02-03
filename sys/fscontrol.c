/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2017 - 2021 Google, Inc.
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
#include "util/fcb.h"
#include "util/mountmgr.h"
#include "util/irp_buffer_helper.h"
#include "util/str.h"

#include <wdmsec.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanOplockRequest)
#endif
#include <mountdev.h>

void DokanMaybeLogOplockRequest(__in PDOKAN_LOGGER Logger,
                                __in PDokanFCB Fcb,
                                __in ULONG FsControlCode,
                                __in ULONG OplockCount,
                                __in BOOLEAN AcquiredFcb,
                                __in BOOLEAN AcquiredVcb,
                                __in ULONG RequestedLevel,
                                __in ULONG Flags) {
  // These calls log to the fixed-size DokanOlockDebugInfo and not to a file, so
  // we enable them no matter what.
  OplockDebugRecordRequest(Fcb, FsControlCode, RequestedLevel);
  if (Flags & REQUEST_OPLOCK_INPUT_FLAG_ACK) {
    OplockDebugRecordFlag(Fcb, DOKAN_OPLOCK_DEBUG_GENERIC_ACKNOWLEDGEMENT);
  }
  // Don't log via the Event Log unless flagged on (which should never be the
  // case by default).
  if (!DokanOpLockDebugEnabled()) {
    return;
  }
  if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
    DokanLogInfo(Logger, L"Oplock request FSCTL_REQUEST_OPLOCK for file \"%wZ\";"
                 L" oplock count %d; acquired FCB %d; acquired VCB %d;"
                 L" level = %I32x; flags = %I32x",
                 &Fcb->FileName, OplockCount, AcquiredFcb, AcquiredVcb,
                 RequestedLevel, Flags);
    return;
  }
  DokanLogInfo(Logger, L"Oplock request %s for file \"%wZ\"; oplock count %d;"
               L" acquired FCB %d; acquired VCB %d",
               DokanGetIoctlStr(FsControlCode),
               &Fcb->FileName, OplockCount, AcquiredFcb, AcquiredVcb);
}

void DokanMaybeLogOplockResult(__in PDOKAN_LOGGER Logger,
                               __in PDokanFCB Fcb,
                               __in ULONG FsControlCode,
                               __in ULONG RequestedLevel,
                               __in ULONG Flags,
                               __in NTSTATUS Status) {
  if (!DokanOpLockDebugEnabled()) {
    return;
  }
  if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
    DokanLogInfo(Logger, L"Oplock result for FSCTL_REQUEST_OPLOCK for file \"%wZ\";"
                 L" level = %I32x; flags = %I32x; status = 0x%I32x",
                 &Fcb->FileName, RequestedLevel, Flags, Status);
    return;
  }
  DokanLogInfo(Logger, L"Oplock result for %s for file \"%wZ\"; status = 0x%I32x",
               DokanGetIoctlStr(FsControlCode), &Fcb->FileName,
               Status);
}

NTSTATUS DokanOplockRequest(__in PREQUEST_CONTEXT RequestContext) {
  NTSTATUS status = STATUS_SUCCESS;
  ULONG fsControlCode;
  PDokanDCB dcb;
  PDokanVCB vcb;
  PDokanFCB fcb = NULL;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;
  ULONG oplockCount = 0;

  BOOLEAN acquiredVcb = FALSE;
  BOOLEAN acquiredFcb = FALSE;

  PREQUEST_OPLOCK_INPUT_BUFFER inputBuffer = NULL;
  ULONG outputBufferLength;

  PAGED_CODE();

  //
  //  Save some references to make our life a little easier
  //
  fsControlCode =
      RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode;

  fileObject = RequestContext->IrpSp->FileObject;
  DOKAN_LOG_FINE_IRP(RequestContext, "FileObject=%p", fileObject);

  ccb = fileObject->FsContext2;
  if (ccb == NULL || ccb->Identifier.Type != CCB) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Invalid CCB or wrong type");
    return STATUS_INVALID_PARAMETER;
  }

  fcb = ccb->Fcb;
  if (fcb == NULL || fcb->Identifier.Type != FCB) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Invalid FCB or wrong type");
    return STATUS_INVALID_PARAMETER;
  }
  OplockDebugRecordMajorFunction(fcb, IRP_MJ_FILE_SYSTEM_CONTROL);
  vcb = fcb->Vcb;
  if (vcb == NULL || vcb->Identifier.Type != VCB) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Invalid Vcb or wrong type");
    return STATUS_INVALID_PARAMETER;
  }
  DOKAN_INIT_LOGGER(logger, vcb->DeviceObject->DriverObject, 0);

  dcb = vcb->Dcb;
  if (dcb == NULL || dcb->Identifier.Type != DCB) {
    return STATUS_INVALID_PARAMETER;
  }

  //
  //  Get the input & output buffer lengths and pointers.
  //
  if (fsControlCode == FSCTL_REQUEST_OPLOCK) {

    outputBufferLength =
        RequestContext->IrpSp->Parameters.FileSystemControl.OutputBufferLength;

    //
    //  Check for a minimum length on the input and ouput buffers.
    //
    GET_IRP_BUFFER_OR_RETURN(RequestContext->Irp, inputBuffer);
    // Use OutputBuffer only for buffer size check
    if (outputBufferLength < sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
  }

  //
  //  If the oplock request is on a directory it must be for a Read or
  //  Read-Handle oplock only.
  //
  if ((DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) &&
      ((fsControlCode != FSCTL_REQUEST_OPLOCK) ||
       !FsRtlOplockIsSharedRequest(RequestContext->Irp))) {

    DOKAN_LOG_FINE_IRP(RequestContext, "Only read oplock allowed for directories");
    return STATUS_INVALID_PARAMETER;
  }

  //
  //  Use a try finally to free the Fcb/Vcb
  //
  try {

    //
    //  We grab the Fcb exclusively for oplock requests, shared for oplock
    //  break acknowledgement.
    //
    if ((fsControlCode == FSCTL_REQUEST_OPLOCK_LEVEL_1) ||
        (fsControlCode == FSCTL_REQUEST_BATCH_OPLOCK) ||
        (fsControlCode == FSCTL_REQUEST_FILTER_OPLOCK) ||
        (fsControlCode == FSCTL_REQUEST_OPLOCK_LEVEL_2) ||
        ((fsControlCode == FSCTL_REQUEST_OPLOCK) &&
            FlagOn(inputBuffer->Flags, REQUEST_OPLOCK_INPUT_FLAG_REQUEST))
    ) {

      DokanVCBLockRO(fcb->Vcb);
      acquiredVcb = TRUE;
      DokanFCBLockRW(fcb);
      acquiredFcb = TRUE;

      if (!dcb->FileLockInUserMode) {

        if (FsRtlOplockIsSharedRequest(RequestContext->Irp)) {
          //
          //  Byte-range locks are only valid on files.
          //
          if (!DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {

            //
            //  Set OplockCount to nonzero if FsRtl denies access
            //  based on current byte-range lock state.
            //
            if (DokanFsRtlCheckLockForOplockRequest) // Win8+
              oplockCount = (ULONG)!DokanFsRtlCheckLockForOplockRequest(
                  &fcb->FileLock, &fcb->AdvancedFCBHeader.AllocationSize);
            else
              oplockCount = (ULONG)FsRtlAreThereCurrentOrInProgressFileLocks(
                  &fcb->FileLock);
          }
        } else {
          // Shouldn't be something like UncleanCount counter and not FileCount
          // here?
          oplockCount = fcb->FileCount;
        }
      }
    } else if ((fsControlCode == FSCTL_OPLOCK_BREAK_ACKNOWLEDGE) ||
               (fsControlCode == FSCTL_OPBATCH_ACK_CLOSE_PENDING) ||
               (fsControlCode == FSCTL_OPLOCK_BREAK_NOTIFY) ||
               (fsControlCode == FSCTL_OPLOCK_BREAK_ACK_NO_2) ||
               ((fsControlCode == FSCTL_REQUEST_OPLOCK) &&
                   FlagOn(inputBuffer->Flags, REQUEST_OPLOCK_INPUT_FLAG_ACK))
    ) {
      DokanFCBLockRO(fcb);
      acquiredFcb = TRUE;
    } else if (fsControlCode == FSCTL_REQUEST_OPLOCK) {
      //
      //  The caller didn't provide either REQUEST_OPLOCK_INPUT_FLAG_REQUEST or
      //  REQUEST_OPLOCK_INPUT_FLAG_ACK on the input buffer.
      //
      status = STATUS_INVALID_PARAMETER;
      __leave;
    } else {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    //
    //  Fail batch, filter, and handle oplock requests if the file is marked
    //  for delete.
    //
    if (((fsControlCode == FSCTL_REQUEST_FILTER_OPLOCK) ||
         (fsControlCode == FSCTL_REQUEST_BATCH_OPLOCK) ||
         ((fsControlCode == FSCTL_REQUEST_OPLOCK) &&
          FlagOn(inputBuffer->RequestedOplockLevel, OPLOCK_LEVEL_CACHE_HANDLE))
             ) &&
        DokanFCBFlagsIsSet(fcb, DOKAN_DELETE_ON_CLOSE)) {
      status = STATUS_DELETE_PENDING;
      __leave;
    }

    ULONG level = 0;
    ULONG flags = 0;
    if (fsControlCode == FSCTL_REQUEST_OPLOCK) {
      level = inputBuffer->RequestedOplockLevel;
      flags = inputBuffer->Flags;
    }
    DokanMaybeLogOplockRequest(&logger, fcb, fsControlCode, oplockCount,
                               acquiredFcb, acquiredVcb, level, flags);

    //
    //  Call the FsRtl routine to grant/acknowledge oplock.
    //
    status = FsRtlOplockFsctrl(DokanGetFcbOplock(fcb), RequestContext->Irp,
                               oplockCount);
    DokanMaybeLogOplockResult(&logger, fcb, fsControlCode, level, flags,
                              status);
    //
    //  Once we call FsRtlOplockFsctrl, we no longer own the IRP and we should
    //  not complete it.
    //
    RequestContext->DoNotComplete = TRUE;

  } finally {

    //
    //  Release all of our resources
    //
    if (acquiredFcb) {
      DokanFCBUnlock(fcb);
    }
    if (acquiredVcb) {
      DokanVCBUnlock(fcb->Vcb);
    }
  }

  return status;
}

NTSTATUS
DokanDiskUserFsRequest(__in PREQUEST_CONTEXT RequestContext);

NTSTATUS
DokanVolumeUserFsRequest(__in PREQUEST_CONTEXT RequestContext) {
  PFILE_OBJECT fileObject = NULL;
  PDokanCCB ccb = NULL;
  PDokanFCB fcb = NULL;
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject,
                    IRP_MJ_FILE_SYSTEM_CONTROL);

  switch (RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode) {
    case FSCTL_ACTIVATE_KEEPALIVE: {
      fileObject = RequestContext->IrpSp->FileObject;
      if (fileObject == NULL) {
        return DokanLogError(
            &logger, STATUS_INVALID_PARAMETER,
            L"Received FSCTL_ACTIVATE_KEEPALIVE with no FileObject.");
      }
      ccb = fileObject->FsContext2;
      if (ccb == NULL || ccb->Identifier.Type != CCB) {
        return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                             L"Received FSCTL_ACTIVATE_KEEPALIVE with no CCB.");
      }

      fcb = ccb->Fcb;
      if (fcb == NULL || fcb->Identifier.Type != FCB) {
        return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                             L"Received FSCTL_ACTIVATE_KEEPALIVE with no FCB.");
      }

      if (!fcb->IsKeepalive) {
        return DokanLogError(
            &logger, STATUS_INVALID_PARAMETER,
            L"Received FSCTL_ACTIVATE_KEEPALIVE for wrong file: \"%wZ\"",
            &fcb->FileName);
      }

      if (fcb->Vcb->IsKeepaliveActive && !ccb->IsKeepaliveActive) {
        return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                             L"Received FSCTL_ACTIVATE_KEEPALIVE when a "
                             L"different keepalive handle"
                             L" was already active.");
      }

      DokanLogInfo(&logger, L"Activating keepalive handle from process %lu.",
                   RequestContext->ProcessId);
      DokanFCBLockRW(fcb);
      ccb->IsKeepaliveActive = TRUE;
      fcb->Vcb->IsKeepaliveActive = TRUE;
      DokanFCBUnlock(fcb);
      return STATUS_SUCCESS;
    }

    case FSCTL_NOTIFY_PATH: {
      PDOKAN_NOTIFY_PATH_INTERMEDIATE pNotifyPath = NULL;
      GET_IRP_NOTIFY_PATH_INTERMEDIATE_OR_RETURN(RequestContext->Irp,
                                                 pNotifyPath);

      fileObject = RequestContext->IrpSp->FileObject;
      if (fileObject == NULL) {
        return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                             L"Received FSCTL_NOTIFY_PATH with no FileObject.");
      }
      ccb = fileObject->FsContext2;
      if (ccb == NULL || ccb->Identifier.Type != CCB) {
        return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                             L"Received FSCTL_NOTIFY_PATH with no CCB.");
      }
      fcb = ccb->Fcb;
      if (fcb == NULL || fcb->Identifier.Type != FCB) {
        return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                             L"Received FSCTL_NOTIFY_PATH with no FCB.");
      }
      UNICODE_STRING receivedBuffer;
      receivedBuffer.Length = pNotifyPath->Length;
      receivedBuffer.MaximumLength = pNotifyPath->Length;
      receivedBuffer.Buffer = pNotifyPath->Buffer;
      DOKAN_LOG_FINE_IRP(RequestContext,
                         "CompletionFilter: %lu, Action: %lu, "
                         "Length: %i, Path: \"%wZ\"",
                         pNotifyPath->CompletionFilter, pNotifyPath->Action,
                         receivedBuffer.Length, &receivedBuffer);
      DokanFCBLockRO(fcb);
      NTSTATUS status = DokanNotifyReportChange0(
          RequestContext, fcb, &receivedBuffer, pNotifyPath->CompletionFilter,
                                        pNotifyPath->Action);
      DokanFCBUnlock(fcb);
      if (status == STATUS_OBJECT_NAME_INVALID) {
        DokanCleanupAllChangeNotificationWaiters(fcb->Vcb);
      }
      return status;
    }

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
    case FSCTL_REQUEST_FILTER_OPLOCK:
    case FSCTL_REQUEST_OPLOCK:
      return DokanOplockRequest(RequestContext);

    case FSCTL_LOCK_VOLUME:
    case FSCTL_UNLOCK_VOLUME:
    case FSCTL_IS_VOLUME_MOUNTED:
      return STATUS_SUCCESS;

    case FSCTL_GET_REPARSE_POINT:
      return STATUS_NOT_A_REPARSE_POINT;
  }
  // TODO(someone): Find if there is a way to send FSCTL to Disk type for DokanRedirector
  if (RequestContext->Dcb && RequestContext->Dcb->VolumeDeviceType ==
                                 FILE_DEVICE_NETWORK_FILE_SYSTEM) {
    NTSTATUS status = DokanDiskUserFsRequest(RequestContext);
    if (status != STATUS_INVALID_DEVICE_REQUEST) {
      return status;
    }
  }
  DOKAN_LOG_FINE_IRP(
      RequestContext, "Unsupported FsControlCode %x",
      RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode);
  return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
DokanGlobalUserFsRequest(__in PREQUEST_CONTEXT RequestContext) {
  switch (RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode) {
    case FSCTL_EVENT_START:
      return DokanEventStart(RequestContext);

    case FSCTL_SET_DEBUG_MODE: {
      PULONG pDebug = NULL;
      GET_IRP_BUFFER_OR_RETURN(RequestContext->Irp, pDebug);
      g_Debug = *pDebug;
      DOKAN_LOG_FINE_IRP(RequestContext, "Set debug mode: %d", g_Debug);
      return STATUS_SUCCESS;
    };

    case FSCTL_EVENT_RELEASE:
      return DokanGlobalEventRelease(RequestContext);

    case FSCTL_EVENT_MOUNTPOINT_LIST:
      return DokanGetMountPointList(RequestContext);

    case FSCTL_GET_VERSION: {
      ULONG *version;
      if (!PREPARE_OUTPUT(RequestContext->Irp, version,
                          /*SetInformationOnFailure=*/FALSE)) {
        return STATUS_BUFFER_TOO_SMALL;
      }
      *version = (ULONG)DOKAN_DRIVER_VERSION;
      return STATUS_SUCCESS;
    };

    case FSCTL_MOUNTPOINT_CLEANUP:
      RemoveSessionDevices(RequestContext, GetCurrentSessionId(RequestContext));
      return STATUS_SUCCESS;
  }
  DOKAN_LOG_FINE_IRP(
      RequestContext, "Unsupported FsControlCode %x",
      RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode);
  return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS PullEvents(__in PREQUEST_CONTEXT RequestContext,
                    __in PIRP_LIST NotifyEvent) {
  PDRIVER_EVENT_CONTEXT workItem = NULL;
  PDRIVER_EVENT_CONTEXT alreadySeenWorkItem = NULL;
  PLIST_ENTRY workItemListEntry = NULL;
  KIRQL workQueueIrql;
  ULONG workItemBytes = 0;
  ULONG currentIoctlBufferBytesRemaining =
      RequestContext->IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
  PCHAR currentIoctlBuffer =
      (PCHAR)RequestContext->Irp->AssociatedIrp.SystemBuffer;

  ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
  KeAcquireSpinLock(&NotifyEvent->ListLock, &workQueueIrql);
  while (!IsListEmpty(&NotifyEvent->ListHead)) {
    workItemListEntry = RemoveHeadList(&NotifyEvent->ListHead);
    workItem =
        CONTAINING_RECORD(workItemListEntry, DRIVER_EVENT_CONTEXT, ListEntry);
    workItemBytes = workItem->EventContext.Length;
    // Buffer is not specified or short of length (this may mean we filled the
    // space in one of the DLL's buffers in batch mode). Put the IRP back in
    // the work queue; it will have to go in a different buffer.
    if (currentIoctlBufferBytesRemaining < workItemBytes) {
      InsertTailList(&NotifyEvent->ListHead, &workItem->ListEntry);
      if (alreadySeenWorkItem == workItem) {
        // We have reached the end of the list
        break;
      }
      if (!alreadySeenWorkItem) {
        alreadySeenWorkItem = workItem;
      }
      continue;
    }
    // Send the work item back in the response to the current IOCTL.
    RtlCopyMemory(currentIoctlBuffer, &workItem->EventContext, workItemBytes);
    currentIoctlBufferBytesRemaining -= workItemBytes;
    currentIoctlBuffer += workItemBytes;
    RequestContext->Irp->IoStatus.Information += workItemBytes;
    ExFreePool(workItem);
    if (!RequestContext->Dcb->AllowIpcBatching) {
      break;
    }
  }
  // If there is still pending items we need to reflag the queue for when we come back
  if (!IsListEmpty(&NotifyEvent->ListHead) &&
       !KeReadStateQueue(&RequestContext->Dcb->NotifyIrpEventQueue)) {
     KeInsertQueue(&RequestContext->Dcb->NotifyIrpEventQueue,
                   &RequestContext->Dcb->NotifyIrpEventQueueList);
  }
  KeReleaseSpinLock(&NotifyEvent->ListLock, workQueueIrql);
  RequestContext->Irp->IoStatus.Status = STATUS_SUCCESS;
  return RequestContext->Irp->IoStatus.Status;
}


NTSTATUS DokanProcessAndPullEvents(__in PREQUEST_CONTEXT RequestContext) {
  // 1 - Complete the optional event.
  // Main pull thread will not have events to complete when:
  // * The file system just started and it is the first pull.
  // * The event completed had no answer to send like Close().
  // Other threads from the pool will have an EVENT_INFORMATION
  // to complete that include the wait timeout for new events.
  NTSTATUS status = DokanCompleteIrp(RequestContext);
  if (status != STATUS_BUFFER_TOO_SMALL && status != STATUS_SUCCESS) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Failed to process IRP");
    return status;
  }

  // 2 - Ensure we have enough space to at least store an event before waiting.
  if (RequestContext->IrpSp->Parameters.DeviceIoControl.OutputBufferLength <
      sizeof(EVENT_CONTEXT)) {
    DOKAN_LOG_FINE_IRP(RequestContext, "No output buffer provided");
    return status;
  }
  // 3 - Flag the device as having workers starting to pull events.
  RequestContext->Vcb->HasEventWait = TRUE;

  PEVENT_INFORMATION eventInfo =
      (PEVENT_INFORMATION)(RequestContext->Irp->AssociatedIrp.SystemBuffer);
  ULONG waitTimeoutMs =
      status == STATUS_BUFFER_TOO_SMALL ? 0 : eventInfo->PullEventTimeoutMs;
  LARGE_INTEGER timeout;
  if (waitTimeoutMs) {
    DokanQuerySystemTime(&timeout);
    timeout.QuadPart += (LONGLONG)waitTimeoutMs * 10000; // Ms to 100 nano
  }

  // 4 - Wait for new event indefinitely if we are the main pull thread
  // or wait for the requested time.
  PLIST_ENTRY listEntry;
  KeRemoveQueueEx(&RequestContext->Dcb->NotifyIrpEventQueue, KernelMode, TRUE,
                  waitTimeoutMs ? &timeout : NULL, &listEntry, 1);
  if (listEntry != &RequestContext->Dcb->NotifyIrpEventQueueList) {
    // Here we got interrupted: Alert / Timeout.
    // In that case listEntry is an NTSTATUS (See KeRemoveQueue doc).

    // Were we awake due to the device being unmount ?
    if (IsUnmountPendingVcb(RequestContext->Vcb)) {
      return STATUS_NO_SUCH_DEVICE;
    }
    return STATUS_SUCCESS;
  }

  // 5 - Fill the provided buffer as much as we can with events.
  return PullEvents(RequestContext, &RequestContext->Dcb->NotifyEvent);
}

NTSTATUS
DokanDiskUserFsRequest(__in PREQUEST_CONTEXT RequestContext) {
  REQUEST_CONTEXT requestContext = *RequestContext;
  // TODO(adrienj): Fake the request target the Vcb until we migrate the
  // following function to expected being called to a Dcb.
  requestContext.Vcb = requestContext.Dcb->Vcb;
  switch (RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode) {
    case FSCTL_EVENT_PROCESS_N_PULL:
      return DokanProcessAndPullEvents(&requestContext);
    case FSCTL_EVENT_RELEASE:
      return DokanEventRelease(&requestContext, requestContext.Vcb->DeviceObject);
    case FSCTL_EVENT_WRITE:
      return DokanEventWrite(&requestContext);
    case FSCTL_GET_VOLUME_METRICS:
      return DokanGetVolumeMetrics(&requestContext);
    case FSCTL_RESET_TIMEOUT:
      return DokanResetPendingIrpTimeout(&requestContext);
    case FSCTL_GET_ACCESS_TOKEN:
      return DokanGetAccessToken(&requestContext);
  }
  DOKAN_LOG_FINE_IRP(
      RequestContext, "Unsupported FsControlCode %x",
      RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode);
  return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
DokanUserFsRequest(__in PREQUEST_CONTEXT RequestContext) {
  DOKAN_LOG_IOCTL(
      RequestContext,
      RequestContext->IrpSp->Parameters.FileSystemControl.FsControlCode,
      "FileObject=%p", RequestContext->IrpSp->FileObject);
  if (RequestContext->DokanGlobal) {
    return DokanGlobalUserFsRequest(RequestContext);
  } else if (RequestContext->Vcb) {
    return DokanVolumeUserFsRequest(RequestContext);
  } else if (RequestContext->Dcb) {
    return DokanDiskUserFsRequest(RequestContext);
  }
  return STATUS_INVALID_DEVICE_REQUEST;
}


// Returns TRUE if |dcb| type matches |DCB| and FALSE otherwise.
BOOLEAN MatchDokanDCBType(__in PREQUEST_CONTEXT RequestContext,
                          __in PDokanDCB Dcb,
                          __in PDOKAN_LOGGER Logger,
                          __in BOOLEAN LogFailures) {
  UNREFERENCED_PARAMETER(RequestContext);
  UNREFERENCED_PARAMETER(Logger);
  if (!Dcb) {
    if (LogFailures) {
      DOKAN_LOG_FINE_IRP(RequestContext, "There is no DCB.");
    }
    return FALSE;
  }
  if (GetIdentifierType(Dcb) != DCB) {
    if (LogFailures) {
      DOKAN_LOG_FINE_IRP(RequestContext, "The DCB type is actually %s expected %s.",
                    DokanGetIdTypeStr(Dcb), STR(DCB));
    }
    return FALSE;
  }
  return TRUE;
}

PCHAR CreateSetReparsePointRequest(PREQUEST_CONTEXT RequestContext,
                                   PUNICODE_STRING SymbolicLinkName,
                                   PULONG Length) {
  UNREFERENCED_PARAMETER(RequestContext);
  USHORT mountPointReparsePathLength =
      SymbolicLinkName->Length + sizeof(WCHAR) /* "\\" */;
  *Length =
      FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) +
      mountPointReparsePathLength + sizeof(WCHAR) + sizeof(WCHAR);
  PREPARSE_DATA_BUFFER reparseData = DokanAllocZero(*Length);
  if (!reparseData) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Failed to allocate reparseData buffer");
    *Length = 0;
    return NULL;
  }

  reparseData->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  reparseData->ReparseDataLength =
      (USHORT)(*Length) - REPARSE_DATA_BUFFER_HEADER_SIZE;
  reparseData->MountPointReparseBuffer.SubstituteNameOffset = 0;
  reparseData->MountPointReparseBuffer.SubstituteNameLength =
      mountPointReparsePathLength;
  reparseData->MountPointReparseBuffer.PrintNameOffset =
      reparseData->MountPointReparseBuffer.SubstituteNameLength + sizeof(WCHAR);
  reparseData->MountPointReparseBuffer.PrintNameLength = 0;
  // SET_REPARSE expect a path ending with a backslash
  // We add it manually to our PersistanteSymbolicLink: \??\Volume{GUID}
  RtlCopyMemory(reparseData->MountPointReparseBuffer.PathBuffer,
                SymbolicLinkName->Buffer, SymbolicLinkName->Length);
  reparseData->MountPointReparseBuffer
      .PathBuffer[mountPointReparsePathLength / sizeof(WCHAR) - 1] = L'\\';

  return (PCHAR)reparseData;
}

PCHAR CreateRemoveReparsePointRequest(PREQUEST_CONTEXT RequestContext,
                                      PULONG Length) {
  UNREFERENCED_PARAMETER(RequestContext);
  *Length = REPARSE_GUID_DATA_BUFFER_HEADER_SIZE;
  PREPARSE_DATA_BUFFER reparseData =
      DokanAllocZero(sizeof(REPARSE_DATA_BUFFER));
  if (!reparseData) {
    DOKAN_LOG_FINE_IRP(RequestContext, "Failed to allocate reparseGuidData buffer");
    *Length = 0;
    return NULL;
  }
  reparseData->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  return (PCHAR)reparseData;
}

NTSTATUS SendDirectoryFsctl(PREQUEST_CONTEXT RequestContext,
                            PUNICODE_STRING Path, ULONG Code, PCHAR Input,
                            ULONG Length) {
  UNREFERENCED_PARAMETER(RequestContext);
  HANDLE handle = 0;
  PUNICODE_STRING directoryStr = NULL;
  NTSTATUS status = STATUS_SUCCESS;
  PIRP topLevelIrp = NULL;
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject,
                    IRP_MJ_FILE_SYSTEM_CONTROL);

  __try {
    // Convert Dcb MountPoint \DosDevices\C:\foo to \??\C:\foo
    directoryStr = ChangePrefix(Path, &g_DosDevicesPrefix, TRUE /*HasPrefix*/,
                                &g_ObjectManagerPrefix);
    if (!directoryStr) {
      status = STATUS_INVALID_PARAMETER;
      DokanLogError(&logger, status, L"Failed to change prefix for \"%wZ\"\n",
                    Path);
      __leave;
    }

    if (RequestContext->IsTopLevelIrp) {
      topLevelIrp = IoGetTopLevelIrp();
      IoSetTopLevelIrp(NULL);
    }

    // Open the directory as \??\C:\foo
    IO_STATUS_BLOCK ioStatusBlock;
    OBJECT_ATTRIBUTES objectAttributes;
    InitializeObjectAttributes(&objectAttributes, directoryStr,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                               NULL);
    DOKAN_LOG_FINE_IRP(RequestContext, "Open directory \"%wZ\"", directoryStr);
    status = ZwOpenFile(&handle, FILE_WRITE_ATTRIBUTES, &objectAttributes,
                        &ioStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT);
    if (!NT_SUCCESS(status)) {
      DokanLogError(&logger, status,
          L"SendDirectoryFsctl - ZwOpenFile failed to open\"%wZ\"\n",
          directoryStr);
      __leave;
    }

    status = ZwFsControlFile(handle, NULL, NULL, NULL, &ioStatusBlock, Code,
                             Input, Length, NULL, 0);
    if (!NT_SUCCESS(status)) {
      DokanLogError(
          &logger, status,
          L"SendDirectoryFsctl - ZwFsControlFile Code %X on \"%wZ\" failed\n",
          Code, directoryStr);
      __leave;
    }
  } __finally {
    if (directoryStr) {
      DokanFreeUnicodeString(directoryStr);
    }
    if (handle) {
      ZwClose(handle);
    }
    if (topLevelIrp) {
      IoSetTopLevelIrp(topLevelIrp);
    }
  }

  DOKAN_LOG_FINE_IRP(RequestContext, "Success");
  return status;
}

// TODO(adrienj): Change DDbgPrint in this function to DokanLogInfo when we will
// better logging.
NTSTATUS DokanMountVolume(__in PREQUEST_CONTEXT RequestContext) {
  PDokanDCB dcb = NULL;
  PDokanVCB vcb = NULL;
  PVPB vpb = NULL;
  DOKAN_CONTROL dokanControl;
  PMOUNT_ENTRY mountEntry = NULL;
  PDEVICE_OBJECT volDeviceObject;
  PDRIVER_OBJECT driverObject = RequestContext->DeviceObject->DriverObject;
  NTSTATUS status = STATUS_UNRECOGNIZED_VOLUME;
  // Note: this can't live on DOKAN_GLOBAL because we can't reliably access that
  // in the case where we use this.
  static LONG hasMountedAnyDisk = 0;

  DOKAN_INIT_LOGGER(logger, driverObject, IRP_MJ_FILE_SYSTEM_CONTROL);
  DOKAN_LOG_FINE_IRP(RequestContext, "Mounting disk device.");

  PDEVICE_OBJECT deviceObject =
      RequestContext->IrpSp->Parameters.MountVolume.DeviceObject;
  dcb = deviceObject->DeviceExtension;
  PDEVICE_OBJECT lowerDeviceObject = NULL;
  while (!MatchDokanDCBType(RequestContext, dcb, &logger,
                            /*LogFailures=*/!hasMountedAnyDisk)) {
    PDEVICE_OBJECT parentDeviceObject =
        lowerDeviceObject ? lowerDeviceObject : deviceObject;
    lowerDeviceObject = IoGetLowerDeviceObject(parentDeviceObject);
    if (parentDeviceObject != deviceObject) {
      ObDereferenceObject(parentDeviceObject);
    }
    if (!lowerDeviceObject) {
      if (!hasMountedAnyDisk) {
        // We stop logging wrapped devices once we successfully mount any disk,
        // because otherwise these messages generate useless noise when the file
        // system gets random mount requests for non-dokan disks.
        DOKAN_LOG_FINE_IRP(
            RequestContext,
            "Not mounting because there is no matching DCB. This is"
            " expected, if a non-DriveFS device is being mounted. If"
            " this prevents DriveFS startup, it may mean the DriveFS"
            " device has its identity obscured by a filter driver.");
      }
      return STATUS_UNRECOGNIZED_VOLUME;
    }
    if (!hasMountedAnyDisk) {
      DOKAN_LOG_FINE_IRP(RequestContext,
                         "Processing the lower level DeviceObject, in case"
                         " this is a DriveFS device wrapped by a filter.");
    }
    dcb = lowerDeviceObject->DeviceExtension;
  }
  if (lowerDeviceObject) {
    ObDereferenceObject(lowerDeviceObject);
  }

  if (dcb->Global->DriverVersion != DOKAN_DRIVER_VERSION) {
    return DokanLogError(&logger, STATUS_UNRECOGNIZED_VOLUME,
                         L"The driver version of the disk does not match.");
  }

  if (IsDeletePending(dcb->DeviceObject)) {
    return DokanLogError(&logger, STATUS_DEVICE_REMOVED,
                         L"This is a remount try of the device.");
  }

  BOOLEAN isNetworkFileSystem =
      (dcb->VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);

  DokanLogInfo(&logger,
               L"Mounting volume using MountPoint \"%wZ\" device \"%wZ\"",
               dcb->MountPoint, dcb->DiskDeviceName);

  if (!isNetworkFileSystem) {
    status = IoCreateDevice(driverObject,               // DriverObject
                            sizeof(DokanVCB),           // DeviceExtensionSize
                            NULL,                       // DeviceName
                            dcb->VolumeDeviceType,      // DeviceType
                            dcb->DeviceCharacteristics, // DeviceCharacteristics
                            FALSE,                      // Not Exclusive
                            &volDeviceObject);          // DeviceObject
  } else {
    status = IoCreateDeviceSecure(
        driverObject,               // DriverObject
        sizeof(DokanVCB),           // DeviceExtensionSize
        dcb->DiskDeviceName,        // DeviceName
        dcb->VolumeDeviceType,      // DeviceType
        dcb->DeviceCharacteristics, // DeviceCharacteristics
        FALSE,                      // Not Exclusive
        &sddl,                      // Default SDDL String
        NULL,                       // Device Class GUID
        &volDeviceObject);          // DeviceObject
  }

  if (!NT_SUCCESS(status)) {
    return DokanLogError(&logger, status, L"IoCreateDevice failed.");
  }

  vcb = volDeviceObject->DeviceExtension;
  vcb->Identifier.Type = VCB;
  vcb->Identifier.Size = sizeof(DokanVCB);

  vcb->DeviceObject = volDeviceObject;
  vcb->Dcb = dcb;
  vcb->ResourceLogger.DriverObject = driverObject;
  vcb->ValidFcbMask = 0xffffffffffffffff;
  dcb->Vcb = vcb;

  if (vcb->Dcb->FcbGarbageCollectionIntervalMs != 0) {
    InitializeListHead(&vcb->FcbGarbageList);
    KeInitializeEvent(&vcb->FcbGarbageListNotEmpty, SynchronizationEvent,
                      FALSE);
    DokanStartFcbGarbageCollector(vcb);
  }

  RtlInitializeGenericTableAvl(&vcb->FcbTable, DokanCompareFcb,
                               DokanAllocateFcbAvl, DokanFreeFcbAvl, vcb);

  InitializeListHead(&vcb->DirNotifyList);
  FsRtlNotifyInitializeSync(&vcb->NotifySync);

  ExInitializeFastMutex(&vcb->AdvancedFCBHeaderMutex);

  FsRtlSetupAdvancedHeader(&vcb->VolumeFileHeader,
                           &vcb->AdvancedFCBHeaderMutex);

  vpb = RequestContext->IrpSp->Parameters.MountVolume.Vpb;
  DokanInitVpb(vpb, vcb->DeviceObject);

  //
  // Establish user-buffer access method.
  //
  SetLongFlag(volDeviceObject->Flags, DO_DIRECT_IO);
  ClearLongFlag(volDeviceObject->Flags, DO_DEVICE_INITIALIZING);
  SetLongFlag(vcb->Flags, VCB_MOUNTED);

  ObReferenceObject(volDeviceObject);

  DOKAN_LOG_FINE_IRP(RequestContext, "ExAcquireResourceExclusiveLite dcb resource");
  ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);

  // set the device on dokanControl
  RtlZeroMemory(&dokanControl, sizeof(DOKAN_CONTROL));
  RtlCopyMemory(dokanControl.DeviceName, dcb->DiskDeviceName->Buffer,
                dcb->DiskDeviceName->Length);
  if (dcb->UNCName->Buffer != NULL && dcb->UNCName->Length > 0) {
    RtlCopyMemory(dokanControl.UNCName, dcb->UNCName->Buffer,
                  dcb->UNCName->Length);
  }
  dokanControl.SessionId = dcb->SessionId;
  mountEntry = FindMountEntry(dcb->Global, &dokanControl, TRUE);
  if (mountEntry != NULL) {
    mountEntry->MountControl.VolumeDeviceObject = volDeviceObject;
    mountEntry->MountControl.MountOptions = dcb->MountOptions;
  } else {
    ExReleaseResourceLite(&dcb->Resource);
    return DokanLogError(&logger, STATUS_DEVICE_REMOVED,
                         L"MountEntry not found.");
  }

  ExReleaseResourceLite(&dcb->Resource);

  // Start check thread
  DokanStartCheckThread(dcb);

  BOOLEAN isDriveLetter = IsMountPointDriveLetter(dcb->MountPoint);
  // Create mount point for the volume
  if (dcb->UseMountManager) {
    BOOLEAN autoMountStateBackup = TRUE;
    if (!isDriveLetter) {
      ExAcquireResourceExclusiveLite(&dcb->Global->MountManagerLock, TRUE);
      // Query current AutoMount State to restore it afterward.
      DokanQueryAutoMount(&autoMountStateBackup);
      // In case of failure, we suppose it was Enabled.

      // MountManager suggest workflow do not accept a path longer than
      // a driver letter mount point so we cannot use it to suggest
      // our directory mount point. We disable Mount Manager AutoMount
      // for avoiding having a driver letter assign to our device
      // for the time we create our own mount point.
      if (autoMountStateBackup) {
        DokanSendAutoMount(FALSE);
      }
    }
    status = DokanSendVolumeArrivalNotification(dcb->DiskDeviceName);
    if (!NT_SUCCESS(status)) {
      DokanLogError(&logger, status,
                    L"DokanSendVolumeArrivalNotification failed.");
    }
    if (!isDriveLetter) {
      // Restore previous AutoMount state.
      if (autoMountStateBackup) {
        DokanSendAutoMount(TRUE);
      }
      ExReleaseResourceLite(&dcb->Global->MountManagerLock);
    }
  } else if (isDriveLetter) {
    DokanCreateMountPoint(dcb);
  }

  if (isNetworkFileSystem) {
    RunAsSystem(DokanRegisterUncProvider, dcb);
  }

  InterlockedOr(&hasMountedAnyDisk, 1);
  DokanLogInfo(&logger, L"Mounting successfully done.");
  DOKAN_LOG_FINE_IRP(RequestContext, "Mounting successfully done.");

  return STATUS_SUCCESS;
}

VOID DokanInitVpb(__in PVPB Vpb, __in PDEVICE_OBJECT VolumeDevice) {
  if (Vpb != NULL) {
    Vpb->DeviceObject = VolumeDevice;
    Vpb->VolumeLabelLength = (USHORT)wcslen(VOLUME_LABEL) * sizeof(WCHAR);
    RtlStringCchCopyW(Vpb->VolumeLabel,
                      sizeof(Vpb->VolumeLabel) / sizeof(WCHAR), VOLUME_LABEL);
    Vpb->SerialNumber = 0x19831116;
  }
}

NTSTATUS
DokanDispatchFileSystemControl(__in PREQUEST_CONTEXT RequestContext) {
  switch (RequestContext->IrpSp->MinorFunction) {
    case IRP_MN_MOUNT_VOLUME:
      return DokanMountVolume(RequestContext);
    case IRP_MN_USER_FS_REQUEST:
      return DokanUserFsRequest(RequestContext);
  }
  DOKAN_LOG_FINE_IRP(RequestContext, "Unsupported MinorFunction %x",
                     RequestContext->IrpSp->MinorFunction);
  return STATUS_INVALID_DEVICE_REQUEST;
}
