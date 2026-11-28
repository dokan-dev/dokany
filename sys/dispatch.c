/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2017 - 2025 Google, Inc.
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

NTSTATUS DokanBuildRequest(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp) {
  BOOLEAN isTopLevelIrp = FALSE;
  NTSTATUS status = STATUS_UNSUCCESSFUL;

  __try {
    __try {
      FsRtlEnterFileSystem();

      if (!IoGetTopLevelIrp()) {
        isTopLevelIrp = TRUE;
        IoSetTopLevelIrp(Irp);
      }

      status = DokanDispatchRequest(DeviceObject, Irp, isTopLevelIrp);

    } __except (DokanExceptionFilter(Irp, GetExceptionInformation())) {
      status = DokanExceptionHandler(DeviceObject, Irp, GetExceptionCode());
    }
  } __finally {
    if (isTopLevelIrp) {
      IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();
  }

  return status;
}

VOID DokanCompleteRequestAndCheckDismount(__in PREQUEST_CONTEXT RequestContext,
                                          __in UCHAR MajorFunction,
                                          __in NTSTATUS Status,
                                          __in BOOLEAN AllowSyncDismount) {
  // Pin volDeviceObject while Vcb->OpenFileCount (for close/create unwind) or
  // the caller's IRP keeps the volume alive. For top-level PASSIVE_LEVEL
  // IRP_MJ_CLOSE and failed IRP_MJ_CREATE, run DokanCheckForDismount BEFORE
  // completing the IRP (matching FastFat) so IopDeleteFile / IopParseDevice
  // still holds its deterministic +1 VPB reference (ResidualVpbRefs = 1).
  PDEVICE_OBJECT dismountVolumeDevice = NULL;
  BOOLEAN canSyncDismount = FALSE;
  ULONG residualVpbRefs = 0;
  BOOLEAN decrementOpenFileCount = RequestContext->DecrementOpenFileCount;

  if (RequestContext->Vcb != NULL &&
      (decrementOpenFileCount ||
       (MajorFunction == IRP_MJ_CREATE && !NT_SUCCESS(Status)))) {
    dismountVolumeDevice = RequestContext->Vcb->DeviceObject;
    ObReferenceObject(dismountVolumeDevice);
    LONG cond =
        InterlockedCompareExchange(&RequestContext->Vcb->Condition, 0, 0);
    if (AllowSyncDismount && !RequestContext->DoNotComplete &&
        Status != STATUS_PENDING && KeGetCurrentIrql() == PASSIVE_LEVEL &&
        RequestContext->IsTopLevelIrp && cond != VcbDeleted &&
        ((MajorFunction == IRP_MJ_CLOSE && decrementOpenFileCount) ||
         (MajorFunction == IRP_MJ_CREATE && !NT_SUCCESS(Status)))) {
      // Only inspect Vcb->Resource when OpenFileCount >= 1 guarantees
      // Vcb->Resource cannot be concurrently deleted; on an early-rejected
      // create (decrementOpenFileCount == FALSE), this thread never acquired
      // Vcb->Resource. ExIsResourceAcquiredSharedLite returns non-zero for
      // both shared and exclusive ownership by the current thread.
      BOOLEAN vcbResourceHeld =
          decrementOpenFileCount &&
          ExIsResourceAcquiredSharedLite(&RequestContext->Vcb->Resource) != 0;
      if (!vcbResourceHeld) {
        canSyncDismount = TRUE;
        if (MajorFunction == IRP_MJ_CLOSE) {
          PFILE_OBJECT fileObject = RequestContext->IrpSp->FileObject;
          // ntoskrnl!IopDeleteFile decrements FileObject->Vpb->ReferenceCount
          // only after IRP_MJ_CLOSE completes (FastFat ResidualReferenceCount).
          if (fileObject != NULL && RequestContext->Vcb->Vpb != NULL &&
              fileObject->Vpb == RequestContext->Vcb->Vpb) {
            residualVpbRefs = 1;
          }
        } else {
          // ntoskrnl!IopParseDevice holds +1 on Vcb->Vpb across IRP_MJ_CREATE
          // even when an early rejection fails before FileObject->Vpb is set.
          if (RequestContext->Vcb->Vpb != NULL) {
            residualVpbRefs = 1;
          }
        }
      }
    }
  }

  if (canSyncDismount) {
    // Log before decrementing OpenFileCount / tearing down Vcb and Dcb.
    DOKAN_LOG_FINE_IRP(RequestContext, "End - %s Information=%llx",
                       DokanGetNTSTATUSStr(Status),
                       RequestContext->Irp->IoStatus.Information);
    PDokanVCB vcbToDismount = (PDokanVCB)dismountVolumeDevice->DeviceExtension;
    LONG remainingOpens =
        decrementOpenFileCount
            ? InterlockedDecrement(&vcbToDismount->OpenFileCount)
            : InterlockedCompareExchange(&vcbToDismount->OpenFileCount, 0, 0);
    BOOLEAN needsDeferredRetry = FALSE;
    if (remainingOpens == 0 &&
        InterlockedCompareExchange(&vcbToDismount->Condition, 0, 0) ==
            VcbDismounted) {
      if (!DokanCheckForDismount(vcbToDismount, residualVpbRefs) &&
          InterlockedCompareExchange(&vcbToDismount->Condition, 0, 0) ==
              VcbDismounted) {
        needsDeferredRetry = TRUE;
      }
    }
    DokanCompleteIrpRequest(RequestContext->Irp, Status);
    if (needsDeferredRetry) {
      DokanQueueDismountWorkItem(vcbToDismount);
    }
    ObDereferenceObject(dismountVolumeDevice);
    return;
  }

  DOKAN_LOG_END_MJ(RequestContext, Status);

  if (dismountVolumeDevice != NULL) {
    PDokanVCB vcbToDismount = (PDokanVCB)dismountVolumeDevice->DeviceExtension;
    LONG remainingOpens =
        decrementOpenFileCount
            ? InterlockedDecrement(&vcbToDismount->OpenFileCount)
            : InterlockedCompareExchange(&vcbToDismount->OpenFileCount, 0, 0);
    if (remainingOpens == 0 &&
        InterlockedCompareExchange(&vcbToDismount->Condition, 0, 0) ==
            VcbDismounted) {
      DokanQueueDismountWorkItem(vcbToDismount);
    }
    ObDereferenceObject(dismountVolumeDevice);
  }
}

VOID DokanCancelCreateIrp(__in PREQUEST_CONTEXT RequestContext,
                          __in NTSTATUS Status) {
  BOOLEAN isTopLevelIrp = FALSE;
  EVENT_INFORMATION eventInfo;

  __try {
    __try {
      FsRtlEnterFileSystem();

      if (!IoGetTopLevelIrp()) {
        isTopLevelIrp = TRUE;
        IoSetTopLevelIrp(RequestContext->Irp);
      }

      RtlZeroMemory(&eventInfo, sizeof(EVENT_INFORMATION));
      eventInfo.Status = Status;
      DokanCompleteCreate(RequestContext, &eventInfo);

      RequestContext->IsTopLevelIrp = isTopLevelIrp;
      DokanCompleteRequestAndCheckDismount(RequestContext, IRP_MJ_CREATE,
                                           RequestContext->Irp->IoStatus.Status,
                                           /*AllowSyncDismount=*/TRUE);

    } __except (
        DokanExceptionFilter(RequestContext->Irp, GetExceptionInformation())) {
      DokanExceptionHandler(RequestContext->DeviceObject, RequestContext->Irp,
                            GetExceptionCode());
    }
  } __finally {
    if (isTopLevelIrp) {
      IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();
  }
}

NTSTATUS DokanBuildRequestContext(_In_ PDEVICE_OBJECT DeviceObject,
                                  _In_ PIRP Irp, BOOLEAN IsTopLevelIrp,
                                  _Out_ PREQUEST_CONTEXT RequestContext) {
  RtlZeroMemory(RequestContext, sizeof(REQUEST_CONTEXT));
  RequestContext->DeviceObject = DeviceObject;
  RequestContext->Irp = Irp;
  RequestContext->Irp->IoStatus.Information = 0;
  RequestContext->IrpSp = IoGetCurrentIrpStackLocation(Irp);
  if (!(RequestContext->IrpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
        RequestContext->IrpSp->MinorFunction == IRP_MN_MOUNT_VOLUME) &&
      RequestContext->DeviceObject->DeviceExtension) {
    switch (GetIdentifierType(RequestContext->DeviceObject->DeviceExtension)) {
      case DGL:
        RequestContext->DokanGlobal = DeviceObject->DeviceExtension;
        break;
      case DCB:
        RequestContext->Dcb = DeviceObject->DeviceExtension;
        break;
      case VCB:
        RequestContext->Vcb = DeviceObject->DeviceExtension;
        if (InterlockedCompareExchange(&RequestContext->Vcb->Condition, 0, 0) !=
            VcbDeleted) {
          RequestContext->Dcb = RequestContext->Vcb->Dcb;
        }
        break;
      default:
        DOKAN_LOG_("Invalid device type received for IRP=%p", Irp);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
  }
  RequestContext->DoNotLogActivity =
      DOKAN_DENIED_LOG_EVENT(RequestContext->IrpSp);
  RequestContext->ProcessId = IoGetRequestorProcessId(RequestContext->Irp);
  RequestContext->IsTopLevelIrp = IsTopLevelIrp;
  return STATUS_SUCCESS;
}

NTSTATUS
DokanDispatchRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp,
                     BOOLEAN IsTopLevelIrp) {
  NTSTATUS status = STATUS_DRIVER_INTERNAL_ERROR;
  REQUEST_CONTEXT requestContext;

  NTSTATUS buildRequestStatus = DokanBuildRequestContext(
      DeviceObject, Irp, IsTopLevelIrp, &requestContext);
  if (!NT_SUCCESS(buildRequestStatus)) {
    return buildRequestStatus;
  }

  // Capture MajorFunction before dispatching because handlers such as
  // FsRtlNotifyFullChangeDirectory, FsRtlProcessFileLock, FsRtlOplockFsctrl,
  // or async pending IRP registration may complete and free the IRP before
  // __finally runs.
  const UCHAR majorFunction = requestContext.IrpSp->MajorFunction;

  DOKAN_LOG_BEGIN_MJ((&requestContext));

  __try {
    if (requestContext.Vcb != NULL &&
        InterlockedCompareExchange(&requestContext.Vcb->Condition, 0, 0) ==
            VcbDeleted) {
      status = (majorFunction == IRP_MJ_CLOSE) ? STATUS_SUCCESS
                                               : STATUS_VOLUME_DISMOUNTED;
      __leave;
    }

    if (requestContext.IrpSp->MajorFunction != IRP_MJ_FILE_SYSTEM_CONTROL &&
        requestContext.IrpSp->MajorFunction != IRP_MJ_SHUTDOWN &&
        requestContext.IrpSp->MajorFunction != IRP_MJ_CLEANUP &&
        requestContext.IrpSp->MajorFunction != IRP_MJ_CLOSE) {
      // Do not short-circuit a DOKAN_RETRY_CREATE IRP before DokanDispatchCreate
      // can unwind its already-allocated CCB, FCB, and OpenFileCount.
      BOOLEAN isRetryCreate =
          requestContext.IrpSp->MajorFunction == IRP_MJ_CREATE &&
          requestContext.IrpSp->FileObject != NULL &&
          requestContext.IrpSp->FileObject->FsContext2 != NULL;
      // While DokanDeleteMountPoint runs during VcbDismountInProgress
      // (Dcb->Condition is still DcbActive), MountManager opens the disk
      // device (IRP_MJ_CREATE) and sends teardown IOCTLs (IRP_MJ_DEVICE_CONTROL,
      // filtered to QUERY_DEVICE_NAME / QUERY_UNIQUE_ID / LINK_DELETED in
      // DiskDeviceControl). Allow those to reach the DCB handlers while still
      // rejecting all VCB I/O and all requests once Dcb->Condition != DcbActive.
      BOOLEAN isActiveDcbTeardownHandshake =
          requestContext.Vcb == NULL && requestContext.Dcb != NULL &&
          !IsDeletePending(DeviceObject) &&
          (majorFunction == IRP_MJ_CREATE ||
           majorFunction == IRP_MJ_DEVICE_CONTROL);
      if (!isRetryCreate && !isActiveDcbTeardownHandshake &&
          IsUnmountPending(DeviceObject)) {
        DOKAN_LOG_FINE_IRP(
            (&requestContext),
            "Volume is not mounted so return STATUS_NO_SUCH_DEVICE");
        status = STATUS_NO_SUCH_DEVICE;
        __leave;
      }
    }

    // If volume is write protected and this request
    // would modify it then return write protected status.
    if (IS_DEVICE_READ_ONLY(DeviceObject)) {
      if ((requestContext.IrpSp->MajorFunction == IRP_MJ_WRITE) ||
          (requestContext.IrpSp->MajorFunction == IRP_MJ_SET_INFORMATION) ||
          (requestContext.IrpSp->MajorFunction == IRP_MJ_SET_EA) ||
          (requestContext.IrpSp->MajorFunction == IRP_MJ_FLUSH_BUFFERS) ||
          (requestContext.IrpSp->MajorFunction == IRP_MJ_SET_SECURITY) ||
          (requestContext.IrpSp->MajorFunction ==
           IRP_MJ_SET_VOLUME_INFORMATION) ||
          (requestContext.IrpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
           requestContext.IrpSp->MinorFunction == IRP_MN_USER_FS_REQUEST &&
           requestContext.IrpSp->Parameters.FileSystemControl.FsControlCode ==
               FSCTL_MARK_VOLUME_DIRTY)) {
        DOKAN_LOG_FINE_IRP((&requestContext), "Media is write protected");
        status = STATUS_MEDIA_WRITE_PROTECTED;
        __leave;
      }
    }

    switch (requestContext.IrpSp->MajorFunction) {
      case IRP_MJ_CREATE:
        status = DokanDispatchCreate(&requestContext);
        break;
      case IRP_MJ_CLOSE:
        status = DokanDispatchClose(&requestContext);
        break;
      case IRP_MJ_READ:
        status = DokanDispatchRead(&requestContext);
        break;
      case IRP_MJ_WRITE:
        status = DokanDispatchWrite(&requestContext);
        break;
      case IRP_MJ_FLUSH_BUFFERS:
        status = DokanDispatchFlush(&requestContext);
        break;
      case IRP_MJ_QUERY_INFORMATION:
        status = DokanDispatchQueryInformation(&requestContext);
        break;
      case IRP_MJ_SET_INFORMATION:
        status = DokanDispatchSetInformation(&requestContext);
        break;
      case IRP_MJ_QUERY_VOLUME_INFORMATION:
        status = DokanDispatchQueryVolumeInformation(&requestContext);
        break;
      case IRP_MJ_SET_VOLUME_INFORMATION:
        status = DokanDispatchSetVolumeInformation(&requestContext);
        break;
      case IRP_MJ_DIRECTORY_CONTROL:
        status = DokanDispatchDirectoryControl(&requestContext);
        break;
      case IRP_MJ_FILE_SYSTEM_CONTROL:
        status = DokanDispatchFileSystemControl(&requestContext);
        break;
      case IRP_MJ_DEVICE_CONTROL:
        status = DokanDispatchDeviceControl(&requestContext);
        break;
      case IRP_MJ_LOCK_CONTROL:
        status = DokanDispatchLock(&requestContext);
        break;
      case IRP_MJ_CLEANUP:
        status = DokanDispatchCleanup(&requestContext);
        break;
      case IRP_MJ_SHUTDOWN:
        status = DokanDispatchShutdown(&requestContext);
        break;
      case IRP_MJ_QUERY_SECURITY:
        status = DokanDispatchQuerySecurity(&requestContext);
        break;
      case IRP_MJ_SET_SECURITY:
        status = DokanDispatchSetSecurity(&requestContext);
        break;
    }
  } __finally {
    DokanCompleteRequestAndCheckDismount(&requestContext, majorFunction, status,
                                         /*AllowSyncDismount=*/TRUE);
  }
  return status;
}
