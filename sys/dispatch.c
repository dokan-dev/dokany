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
  // Keep Vcb->OpenFileCount elevated across DOKAN_LOG_END_MJ so Vcb,
  // Vcb->Resource, and Dcb cannot be torn down concurrently before the IRP
  // finishes completing and logging. Pin volDeviceObject before
  // DOKAN_LOG_END_MJ, then decrement OpenFileCount and run teardown strictly
  // after completion.
  PDEVICE_OBJECT dismountVolumeDevice = NULL;
  BOOLEAN deferDismount = TRUE;
  ULONG residualVpbRefs = 0;
  BOOLEAN decrementOpenFileCount = RequestContext->DecrementOpenFileCount;

  if (RequestContext->Vcb != NULL &&
      (decrementOpenFileCount ||
       (MajorFunction == IRP_MJ_CREATE && !NT_SUCCESS(Status)))) {
    dismountVolumeDevice = RequestContext->Vcb->DeviceObject;
    ObReferenceObject(dismountVolumeDevice);
    // Matching FastFat (close.c): only run synchronous CheckForDismount on
    // top-level IRP_MJ_CLOSE with no Vcb->Resource held. Because
    // OpenFileCount is still >= 1 here, Vcb->Resource is guaranteed live, and
    // because DokanDispatchClose owns the IRP synchronously until
    // DOKAN_LOG_END_MJ completes it, IrpSp->FileObject is safe to inspect here.
    // Recursive closes (e.g. under Fcb->MainResource or MM/CC/filter locks) and
    // failed IRP_MJ_CREATEs (where IopParseDevice decrements
    // Vpb->ReferenceCount after IRP completion unwinds) defer to the
    // non-blocking dismount work item / backoff timer.
    if (AllowSyncDismount && !RequestContext->DoNotComplete &&
        MajorFunction == IRP_MJ_CLOSE && decrementOpenFileCount &&
        RequestContext->IsTopLevelIrp &&
        ExIsResourceAcquiredSharedLite(&RequestContext->Vcb->Resource) == 0 &&
        !ExIsResourceAcquiredExclusiveLite(&RequestContext->Vcb->Resource)) {
      deferDismount = FALSE;
      PFILE_OBJECT fileObject = RequestContext->IrpSp->FileObject;
      // ntoskrnl!IopDeleteFile decrements FileObject->Vpb->ReferenceCount only
      // after IRP_MJ_CLOSE returns (matching FastFat's ResidualReferenceCount).
      if (fileObject != NULL && RequestContext->Vcb->Vpb != NULL &&
          fileObject->Vpb == RequestContext->Vcb->Vpb) {
        residualVpbRefs = 1;
      }
    }
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
      if (deferDismount) {
        DokanQueueDismountWorkItem(vcbToDismount);
      } else {
        DokanCheckForDismount(vcbToDismount, residualVpbRefs);
      }
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

      DokanCompleteRequestAndCheckDismount(RequestContext, IRP_MJ_CREATE,
                                           RequestContext->Irp->IoStatus.Status,
                                           /*AllowSyncDismount=*/FALSE);

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
                                  _Outptr_ PREQUEST_CONTEXT RequestContext) {
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
        RequestContext->Dcb = RequestContext->Vcb->Dcb;
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
      if (!isRetryCreate && IsUnmountPending(DeviceObject)) {
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
