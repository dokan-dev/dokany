/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2023 Google, Inc.
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
#include "util/str.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanDispatchCreate)
#pragma alloc_text(PAGE, DokanCheckShareAccess)
#endif

static const UNICODE_STRING systemVolumeInformationFileName =
    RTL_CONSTANT_STRING(L"\\System Volume Information");

// DokanAllocateCCB must be called with exlusive Fcb lock held.
PDokanCCB DokanAllocateCCB(__in PREQUEST_CONTEXT RequestContext, __in PDokanFCB Fcb) {
  PDokanCCB ccb = ExAllocateFromLookasideListEx(&g_DokanCCBLookasideList);

  if (ccb == NULL)
    return NULL;

  ASSERT(ccb != NULL);
  ASSERT(Fcb != NULL);

  RtlZeroMemory(ccb, sizeof(DokanCCB));

  ccb->Identifier.Type = CCB;
  ccb->Identifier.Size = sizeof(DokanCCB);

  ccb->Fcb = Fcb;
  DOKAN_LOG_FINE_IRP(RequestContext, "Allocated CCB=%p", ccb);

  InitializeListHead(&ccb->NextCCB);

  InsertTailList(&Fcb->NextCCB, &ccb->NextCCB);

  ccb->MountId = RequestContext->Dcb->MountId;
  ccb->ProcessId = PsGetCurrentProcessId();

  InterlockedIncrement(&Fcb->Vcb->CcbAllocated);
  return ccb;
}

VOID
DokanMaybeBackOutAtomicOplockRequest(__in PDokanCCB Ccb, __in PIRP Irp) {
  if (Ccb->AtomicOplockRequestPending) {
    FsRtlCheckOplockEx(DokanGetFcbOplock(Ccb->Fcb), Irp,
                       OPLOCK_FLAG_BACK_OUT_ATOMIC_OPLOCK, NULL, NULL,
                       NULL);
    Ccb->AtomicOplockRequestPending = FALSE;
    OplockDebugRecordFlag(Ccb->Fcb, DOKAN_OPLOCK_DEBUG_ATOMIC_BACKOUT);
  }
}

NTSTATUS
DokanFreeCCB(__in PREQUEST_CONTEXT RequestContext, __in PDokanCCB ccb) {
  PDokanFCB fcb;

  UNREFERENCED_PARAMETER(RequestContext);

  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  if (!fcb) {
    return STATUS_SUCCESS;
  }

  DokanFCBLockRW(fcb);

  DOKAN_LOG_FINE_IRP(RequestContext, "Free CCB=%p", ccb);

  if (IsListEmpty(&ccb->NextCCB)) {
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "WARNING. &ccb->NextCCB is empty. This should never happen, "
        "so check the behavior. Would produce BSOD ");
    DokanFCBUnlock(fcb);
    return STATUS_SUCCESS;
  } else {
    RemoveEntryList(&ccb->NextCCB);
    InitializeListHead(&ccb->NextCCB);
  }

  DokanFCBUnlock(fcb);

  if (ccb->SearchPattern) {
    ExFreePool(ccb->SearchPattern);
  }

  ExFreeToLookasideListEx(&g_DokanCCBLookasideList, ccb);
  InterlockedIncrement(&fcb->Vcb->CcbFreed);

  return STATUS_SUCCESS;
}

// Creates a buffer from DokanAlloc() containing
// the parent dir of file/dir pointed to by fileName.
// the buffer IS null terminated
// in *parentDirLength returns length in bytes of string (not counting null
// terminator)
// fileName MUST be null terminated
// if last char of fileName is \, then it is ignored but a slash
// is appened to the returned path
//
//  e.g. \foo\bar.txt becomes \foo
//       \foo\bar\ bcomes \foo\
//
// if there is no parent, then it return STATUS_ACCESS_DENIED
// if DokanAlloc() fails, then it returns STATUS_INSUFFICIENT_RESOURCES
// otherwise returns STATUS_SUCCESS

NTSTATUS DokanGetParentDir(__in const WCHAR *fileName, __out WCHAR **parentDir,
                           __out ULONG *parentDirLength) {
  // first check if there is a parent

  LONG len = (LONG)wcslen(fileName);

  LONG i;

  BOOLEAN trailingSlash;

  *parentDir = NULL;
  *parentDirLength = 0;

  if (len < 1) {
    return STATUS_INVALID_PARAMETER;
  }

  if (!wcscmp(fileName, L"\\"))
    return STATUS_ACCESS_DENIED;

  trailingSlash = fileName[len - 1] == '\\';

  *parentDir = (WCHAR *)DokanAllocZero((len + 1) * sizeof(WCHAR));

  if (!*parentDir)
    return STATUS_INSUFFICIENT_RESOURCES;

  RtlStringCchCopyW(*parentDir, len, fileName);

  for (i = len - 1; i >= 0; i--) {
    if ((*parentDir)[i] == '\\') {
      if (i == len - 1 && trailingSlash) {
        continue;
      }
      (*parentDir)[i] = 0;
      break;
    }
  }

  if (i <= 0) {
    i = 1;
    (*parentDir)[0] = '\\';
    (*parentDir)[1] = 0;
  }

  *parentDirLength = i * sizeof(WCHAR);
  if (trailingSlash && i > 1) {
    (*parentDir)[i] = '\\';
    (*parentDir)[i + 1] = 0;
    *parentDirLength += sizeof(WCHAR);
  }

  return STATUS_SUCCESS;
}

/*
 * When the mount point is a reparse point (not a drive letter) on <= Win10
 * 1803, file names in FILE_OBJECTs initially have an 'unparsed' portion that is
 * all uppercase. This function changes them to the right case by getting it
 * from an undocumented extra create parameter. See:
 * https://community.osr.com/discussion/287522
 */
void FixFileNameForReparseMountPoint(__in const UNICODE_STRING *MountPoint,
                                     __in PIRP Irp,
                                     __in PIO_STACK_LOCATION IrpSp) {

  if (!g_FixFileNameForReparseMountPoint) {
    return;
  }

  // Only Revert when reparse point is used
  if (IsMountPointDriveLetter(MountPoint)) {
    return;
  }

  PECP_LIST ecpList;
  struct SYMLINK_ECP_CONTEXT *ecpContext;
  // IopSymlinkECPGuid "73d5118a-88ba-439f-92f4-46d38952d250";
  static const GUID iopSymlinkECPGuid = {
      0x73d5118a,
      0x88ba,
      0x439f,
      {0x92, 0xf4, 0x46, 0xd3, 0x89, 0x52, 0xd2, 0x50}};

  if (!NT_SUCCESS(FsRtlGetEcpListFromIrp(Irp, &ecpList)) || !ecpList) {
    return;
  }
  if (!NT_SUCCESS(FsRtlFindExtraCreateParameter(ecpList, &iopSymlinkECPGuid,
                                                (void **)&ecpContext, 0))) {
    return;
  }
  if (FsRtlIsEcpFromUserMode(ecpContext) ||
      !ecpContext->FlagsMountPoint.MountPoint.MountPoint) {
    return;
  }
  USHORT unparsedNameLength = ecpContext->UnparsedNameLength;
  if (unparsedNameLength == 0) {
    return;
  }

  PUNICODE_STRING FileName = &IrpSp->FileObject->FileName;
  USHORT fileNameLength = FileName->Length;
  USHORT ecpNameLength = ecpContext->Name.Length;
  if (unparsedNameLength > ecpNameLength ||
      unparsedNameLength > fileNameLength) {
    return;
  }

  PWSTR unparsedNameInFileObject = (PWSTR)RtlOffsetToPointer(
      FileName->Buffer, fileNameLength - unparsedNameLength);
  UNICODE_STRING unparsedNameInFileObjectUS =
      DokanWrapUnicodeString(unparsedNameInFileObject, unparsedNameLength);

  PWSTR unparsedNameInEcp = (PWSTR)RtlOffsetToPointer(
      ecpContext->Name.Buffer, ecpNameLength - unparsedNameLength);
  UNICODE_STRING unparsedNameInEcpUS =
      DokanWrapUnicodeString(unparsedNameInEcp, unparsedNameLength);

  if (RtlEqualUnicodeString(&unparsedNameInFileObjectUS, &unparsedNameInEcpUS,
                            /*CaseInSensitive=*/TRUE)) {
    RtlCopyMemory(unparsedNameInFileObject, unparsedNameInEcp,
                  unparsedNameLength);
  }
}

VOID SetFileObjectForVCB(__in PFILE_OBJECT FileObject, __in PDokanVCB Vcb) {
  FileObject->SectionObjectPointer = &Vcb->SectionObjectPointers;
  FileObject->FsContext = &Vcb->VolumeFileHeader;
}

BOOL IsDokanProcessFiles(UNICODE_STRING FileName) {
  return (FileName.Length > 0 &&
          (RtlEqualUnicodeString(&FileName, &g_KeepAliveFileName, FALSE) ||
           RtlEqualUnicodeString(&FileName, &g_NotificationFileName, FALSE)));
}

NTSTATUS
DokanCheckShareAccess(_In_ PREQUEST_CONTEXT RequestContext,
                      _In_ PFILE_OBJECT FileObject, _In_ PDokanFCB FcbOrDcb,
                      _In_ ACCESS_MASK DesiredAccess, _In_ ULONG ShareAccess)

/*++
Routine Description:
This routine checks conditions that may result in a sharing violation.
Arguments:
FileObject - Pointer to the file object of the current open request.
FcbOrDcb - Supplies a pointer to the Fcb/Dcb.
DesiredAccess - Desired access of current open request.
ShareAccess - Shared access requested by current open request.
Return Value:
If the accessor has access to the file, STATUS_SUCCESS is returned.
Otherwise, STATUS_SHARING_VIOLATION is returned.

--*/

{
  NTSTATUS status;
  PAGED_CODE();

  UNREFERENCED_PARAMETER(RequestContext);

  // Cannot open a file with delete pending without share delete
  if ((FcbOrDcb->Identifier.Type == FCB) &&
      !FlagOn(ShareAccess, FILE_SHARE_DELETE) &&
      DokanFCBFlagsIsSet(FcbOrDcb, DOKAN_DELETE_ON_CLOSE))
    return STATUS_DELETE_PENDING;

  //
  //  Do an extra test for writeable user sections if the user did not allow
  //  write sharing - this is necessary since a section may exist with no
  //  handles
  //  open to the file its based against.
  //
  if ((FcbOrDcb->Identifier.Type == FCB) &&
      !FlagOn(ShareAccess, FILE_SHARE_WRITE) &&
      FlagOn(DesiredAccess, FILE_EXECUTE | FILE_READ_DATA | FILE_WRITE_DATA |
                                FILE_APPEND_DATA | DELETE | MAXIMUM_ALLOWED) &&
      MmDoesFileHaveUserWritableReferences(&FcbOrDcb->SectionObjectPointers)) {

    DOKAN_LOG_FINE_IRP(RequestContext, "FCB has no write shared access");
    return STATUS_SHARING_VIOLATION;
  }

  //
  //  Check if the Fcb has the proper share access.
  //
  //  Pass FALSE for update.  We will update it later.
  status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject,
                              &FcbOrDcb->ShareAccess, FALSE);

  return status;
}

// Oplock break completion routine used for async oplock breaks that are
// triggered in DokanDispatchCreate. This either queues the IRP_MJ_CREATE to get
// re-dispatched or queues it to get failed asynchronously by calling
// DokanCompleteCreate in a safe context.
VOID DokanRetryCreateAfterOplockBreak(__in PVOID Context, __in PIRP Irp) {
  REQUEST_CONTEXT requestContext;
  NTSTATUS status =
      DokanBuildRequestContext((PDEVICE_OBJECT)Context, Irp,
                               /*IsTopLevelIrp=*/FALSE, &requestContext);
  if (!NT_SUCCESS(status)) {
    DOKAN_LOG_("Failed to build request context for IRP=%p Status=%s", Irp,
               DokanGetNTSTATUSStr(status));
    return;
  }
  if (NT_SUCCESS(Irp->IoStatus.Status)) {
    DokanRegisterPendingRetryIrp(&requestContext);
  } else {
    DokanRegisterAsyncCreateFailure(&requestContext, Irp->IoStatus.Status);
  }
}

NTSTATUS
DokanDispatchCreate(__in PREQUEST_CONTEXT RequestContext)

/*++

Routine Description:

                This device control dispatcher handles create & close IRPs.

Arguments:

                DeviceObject - Context for the activity.
                Irp          - The device control argument block.

Return Value:

                NTSTATUS

--*/
{
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject = NULL;
  PEVENT_CONTEXT eventContext = NULL;
  PFILE_OBJECT relatedFileObject;
  ULONG fileNameLength = 0;
  ULONG eventLength;
  PDokanFCB fcb = NULL;
  PDokanFCB relatedFcb = NULL;
  PDokanCCB ccb = NULL;
  PWCHAR fileName = NULL;
  PWCHAR parentDir = NULL; // for SL_OPEN_TARGET_DIRECTORY
  ULONG parentDirLength = 0;
  BOOLEAN needBackSlashAfterRelatedFile = FALSE;
  BOOLEAN alternateDataStreamOfRootDir = FALSE;
  ULONG securityDescriptorSize = 0;
  ULONG alignedEventContextSize = 0;
  ULONG alignedObjectNameSize =
      PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE));
  ULONG alignedObjectTypeNameSize =
      PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE));
  PDOKAN_UNICODE_STRING_INTERMEDIATE intermediateUnicodeStr = NULL;
  PUNICODE_STRING relatedFileName = NULL;
  PSECURITY_DESCRIPTOR newFileSecurityDescriptor = NULL;
  BOOLEAN openRequiringOplock = FALSE;
  BOOLEAN unwindShareAccess = FALSE;
  BOOLEAN eventContextConsumed = FALSE;
  DWORD disposition = 0;
  BOOLEAN fcbLocked = FALSE;
  DOKAN_INIT_LOGGER(logger, RequestContext->DeviceObject->DriverObject,
                    IRP_MJ_CREATE);

  PAGED_CODE();

  __try {
    fileObject = RequestContext->IrpSp->FileObject;
    if (fileObject == NULL) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    relatedFileObject = fileObject->RelatedFileObject;

    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "FileObject=%p RelatedFileObject=%p FileName=\"%wZ\" Flags=%x "
        "DesiredAccess=%lx Options=%lx "
        "FileAttributes=%x ShareAccess=%x",
        fileObject, relatedFileObject, &fileObject->FileName,
        RequestContext->IrpSp->Flags,
        RequestContext->IrpSp->Parameters.Create.SecurityContext->DesiredAccess,
        RequestContext->IrpSp->Parameters.Create.Options,
        RequestContext->IrpSp->Parameters.Create.FileAttributes,
        RequestContext->IrpSp->Parameters.Create.ShareAccess);

    disposition =
        (RequestContext->IrpSp->Parameters.Create.Options >> 24) & 0x000000ff;

    if (RequestContext->Vcb == NULL) {
      status = STATUS_SUCCESS;
      __leave;
    }

    if (IsUnmountPendingVcb(RequestContext->Vcb)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "IdentifierType is VCB which is not mounted");
      status = STATUS_NO_SUCH_DEVICE;
      __leave;
    }

    FixFileNameForReparseMountPoint(RequestContext->Dcb->MountPoint,
                                    RequestContext->Irp, RequestContext->IrpSp);

    BOOLEAN isNetworkFileSystem = (RequestContext->Dcb->VolumeDeviceType ==
                                   FILE_DEVICE_NETWORK_FILE_SYSTEM);

    if (!isNetworkFileSystem) {
      if (relatedFileObject != NULL) {
        fileObject->Vpb = relatedFileObject->Vpb;
      } else {
        fileObject->Vpb = RequestContext->Dcb->DeviceObject->Vpb;
      }
    }

    if (!RequestContext->Vcb->HasEventWait) {
      if (IsDokanProcessFiles(fileObject->FileName)) {
        DOKAN_LOG_FINE_IRP(RequestContext, "Dokan Process file called before startup finished");
      } else {
        // We want to always dispatch non-root opens so we don't have to
        // special-case anything here, but it needs a lot of testing.
        DOKAN_LOG_FINE_IRP(
            RequestContext,
            "Here we only go in if some antivirus software tries to "
            "create files before startup is finished.");
        if (fileObject->FileName.Length > 0) {
          DOKAN_LOG_FINE_IRP(
              RequestContext,
              "Verify if the system tries to access System Volume");
          if (StartsWith(&fileObject->FileName,
                         &systemVolumeInformationFileName)) {
            DOKAN_LOG_FINE_IRP(
                RequestContext,
                "It's an access to System Volume, so don't return "
                "SUCCESS. We don't have one.");
            status = STATUS_NO_SUCH_FILE;
            __leave;
          }
        }
        DokanLogInfo(&logger,
                     L"Handle created before IOCTL_EVENT_WAIT for file \"%wZ\"",
                     &fileObject->FileName);
        status = STATUS_SUCCESS;
        __leave;
      }
    }

    if (RequestContext->IrpSp->Flags & SL_CASE_SENSITIVE) {
      DOKAN_LOG_FINE_IRP(RequestContext, "IrpSp->Flags SL_CASE_SENSITIVE");
    }
    if (RequestContext->IrpSp->Flags & SL_FORCE_ACCESS_CHECK) {
      DOKAN_LOG_FINE_IRP(RequestContext, "IrpSp->Flags SL_FORCE_ACCESS_CHECK");
    }
    if (RequestContext->IrpSp->Flags & SL_OPEN_PAGING_FILE) {
      DOKAN_LOG_FINE_IRP(RequestContext, "IrpSp->Flags SL_OPEN_PAGING_FILE");
    }
    if (RequestContext->IrpSp->Flags & SL_OPEN_TARGET_DIRECTORY) {
      DOKAN_LOG_FINE_IRP(RequestContext, "IrpSp->Flags SL_OPEN_TARGET_DIRECTORY");
    }

    if ((fileObject->FileName.Length > sizeof(WCHAR)) &&
        (fileObject->FileName.Buffer[1] == L'\\') &&
        (fileObject->FileName.Buffer[0] == L'\\')) {

      fileObject->FileName.Length -= sizeof(WCHAR);

      RtlMoveMemory(&fileObject->FileName.Buffer[0],
                    &fileObject->FileName.Buffer[1],
                    fileObject->FileName.Length);
    }

    // Get RelatedFileObject filename.
    if (relatedFileObject != NULL && relatedFileObject->FsContext2) {
      // Using relatedFileObject->FileName is not safe here, use cached filename
      // from context.
      PDokanCCB relatedCcb = (PDokanCCB)relatedFileObject->FsContext2;
      if (relatedCcb->Fcb) {
        relatedFcb = relatedCcb->Fcb;
        DokanFCBLockRO(relatedFcb);
        if (relatedFcb->FileName.Length > 0 &&
            relatedFcb->FileName.Buffer != NULL) {
          relatedFileName = DokanAlloc(sizeof(UNICODE_STRING));
          if (relatedFileName == NULL) {
            DOKAN_LOG_FINE_IRP(RequestContext, "Can't allocatePool for relatedFileName");
            status = STATUS_INSUFFICIENT_RESOURCES;
            DokanFCBUnlock(relatedFcb);
            __leave;
          }
          relatedFileName->Buffer =
              DokanAlloc(relatedFcb->FileName.MaximumLength);
          if (relatedFileName->Buffer == NULL) {
            DOKAN_LOG_FINE_IRP(RequestContext, "Can't allocatePool for relatedFileName buffer");
            ExFreePool(relatedFileName);
            relatedFileName = NULL;
            status = STATUS_INSUFFICIENT_RESOURCES;
            DokanFCBUnlock(relatedFcb);
            __leave;
          }
          relatedFileName->MaximumLength = relatedFcb->FileName.MaximumLength;
          relatedFileName->Length = relatedFcb->FileName.Length;
          RtlUnicodeStringCopy(relatedFileName, &relatedFcb->FileName);
        }
        DokanFCBUnlock(relatedFcb);
      }
    }

    if (relatedFileName == NULL && fileObject->FileName.Length == 0) {

      DOKAN_LOG_FINE_IRP(RequestContext, "Request for FS device");

      if (RequestContext->IrpSp->Parameters.Create.Options &
          FILE_DIRECTORY_FILE) {
        status = STATUS_NOT_A_DIRECTORY;
      } else {
        SetFileObjectForVCB(fileObject, RequestContext->Vcb);
        RequestContext->Irp->IoStatus.Information = FILE_OPENED;
        status = STATUS_SUCCESS;
      }
      __leave;
    }

    if (fileObject->FileName.Length > sizeof(WCHAR) &&
        fileObject->FileName
                .Buffer[fileObject->FileName.Length / sizeof(WCHAR) - 1] ==
            L'\\') {
      fileObject->FileName.Length -= sizeof(WCHAR);
    }

    fileNameLength = fileObject->FileName.Length;
    if (relatedFileName != NULL) {
      fileNameLength += relatedFileName->Length;

      if (fileObject->FileName.Length > 0 &&
          fileObject->FileName.Buffer[0] == '\\') {
        DOKAN_LOG_FINE_IRP(
            RequestContext,
            "When RelatedFileObject is specified, the file name should "
            "be relative path");
        status = STATUS_INVALID_PARAMETER;
        __leave;
      }
      if (relatedFileName->Length > 0 && fileObject->FileName.Length > 0 &&
          relatedFileName->Buffer[relatedFileName->Length / sizeof(WCHAR) -
                                  1] != '\\' && fileObject->FileName.Buffer[0] != ':') {
        needBackSlashAfterRelatedFile = TRUE;
        fileNameLength += sizeof(WCHAR);
      }
      // for if we're trying to open a file that's actually an alternate data
      // stream of the root dircetory as in "\:foo"
      // in this case we won't prepend relatedFileName to the file name
      if (relatedFileName->Length / sizeof(WCHAR) == 1 &&
          fileObject->FileName.Length > 0 &&
          relatedFileName->Buffer[0] == '\\' &&
          fileObject->FileName.Buffer[0] == ':') {
        alternateDataStreamOfRootDir = TRUE;
      }
    }

    // don't open file like stream
    if (!RequestContext->Dcb->UseAltStream &&
        DokanSearchUnicodeStringChar(&fileObject->FileName, L':') != -1) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Alternate stream");
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    // this memory is freed by DokanGetFCB if needed
    // "+ sizeof(WCHAR)" is for the last NULL character
    fileName = DokanAllocZero(fileNameLength + sizeof(WCHAR));
    if (fileName == NULL) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Can't allocatePool for fileName");
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    if (relatedFileName != NULL && !alternateDataStreamOfRootDir) {
      DOKAN_LOG_FINE_IRP(RequestContext, "RelatedFileName=\"%wZ\"", relatedFileName);

      // copy the file name of related file object
      RtlCopyMemory(fileName, relatedFileName->Buffer, relatedFileName->Length);

      if (needBackSlashAfterRelatedFile) {
        ((PWCHAR)fileName)[relatedFileName->Length / sizeof(WCHAR)] = '\\';
      }
      // copy the file name of fileObject
      RtlCopyMemory((PCHAR)fileName + relatedFileName->Length +
                        (needBackSlashAfterRelatedFile ? sizeof(WCHAR) : 0),
                    fileObject->FileName.Buffer, fileObject->FileName.Length);
      DOKAN_LOG_FINE_IRP(RequestContext, "Absolute FileName=\"%ls\"", fileName);
    } else {
      // if related file object is not specified, copy the file name of file
      // object
      RtlCopyMemory(fileName, fileObject->FileName.Buffer,
                    fileObject->FileName.Length);
    }

    // Remove possible UNCName prefix
    if (RequestContext->Dcb->UNCName != NULL) {
      UNICODE_STRING fileNameUS =
          DokanWrapUnicodeString(fileName, fileNameLength);
      if (RtlPrefixUnicodeString(RequestContext->Dcb->UNCName, &fileNameUS,
                                 /*CaseInSensitive=*/TRUE)) {
        fileNameLength -= RequestContext->Dcb->UNCName->Length;
        if (fileNameLength == 0) {
          fileName[0] = '\\';
          fileName[1] = '\0';
          fileNameLength = sizeof(WCHAR);
        } else {
          RtlMoveMemory(fileName,
                        (PCHAR)fileName + RequestContext->Dcb->UNCName->Length,
                        fileNameLength);
          fileName[fileNameLength / sizeof(WCHAR)] = '\0';
        }
      }
    }

    // Fail if device is read-only and request involves a write operation
    if (IS_DEVICE_READ_ONLY(RequestContext->DeviceObject) &&
        ((disposition == FILE_SUPERSEDE) || (disposition == FILE_CREATE) ||
         (disposition == FILE_OVERWRITE) ||
         (disposition == FILE_OVERWRITE_IF) ||
         (RequestContext->IrpSp->Parameters.Create.Options &
          FILE_DELETE_ON_CLOSE))) {

      DOKAN_LOG_FINE_IRP(RequestContext, "Media is write protected");
      status = STATUS_MEDIA_WRITE_PROTECTED;
      ExFreePool(fileName);
      __leave;
    }

    BOOLEAN allocateCcb = TRUE;
    if (fileObject->FsContext2 != NULL) {
      // Check if we are retrying a create we started before.
      ccb = fileObject->FsContext2;
      if (GetIdentifierType(ccb) == CCB &&
          (DokanCCBFlagsIsSet(ccb, DOKAN_RETRY_CREATE))) {
        DokanCCBFlagsClearBit(ccb, DOKAN_RETRY_CREATE);
        fcb = ccb->Fcb;
        OplockDebugRecordFlag(fcb, DOKAN_OPLOCK_DEBUG_CREATE_RETRIED);
        allocateCcb = FALSE;
        ExFreePool(fileName);
        fileName = NULL;
      }
    }
    if (allocateCcb) {
      BOOLEAN isAlreadyOpen = FALSE;
      // Allocate an FCB or find one in the open list.
      if (RequestContext->IrpSp->Flags & SL_OPEN_TARGET_DIRECTORY) {
        status = DokanGetParentDir(fileName, &parentDir, &parentDirLength);
        if (status != STATUS_SUCCESS) {
          ExFreePool(fileName);
          fileName = NULL;
          __leave;
        }
        fcb = DokanGetFCB(RequestContext, parentDir, parentDirLength,
                          &isAlreadyOpen);
      } else {
        fcb = DokanGetFCB(RequestContext, fileName, fileNameLength,
                          &isAlreadyOpen);
      }
      if (fcb == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        __leave;
      }
      if (fcb->BlockUserModeDispatch) {
        DokanLogInfo(&logger,
                     L"Opened file with user mode dispatch blocked: \"%wZ\"",
                     &fileObject->FileName);
      }
      DOKAN_LOG_FINE_IRP(RequestContext, "Use FCB=%p", fcb);

      // Cannot create a file already open
      if (isAlreadyOpen && disposition == FILE_CREATE) {
        status = STATUS_OBJECT_NAME_COLLISION;
        __leave;
      }
    }

    // Cannot create a directory temporary
    if (FlagOn(RequestContext->IrpSp->Parameters.Create.Options,
               FILE_DIRECTORY_FILE) &&
        FlagOn(RequestContext->IrpSp->Parameters.Create.FileAttributes,
               FILE_ATTRIBUTE_TEMPORARY) &&
        (FILE_CREATE == disposition || FILE_OPEN_IF == disposition)) {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    fcbLocked = TRUE;
    DokanFCBLockRW(fcb);

    if (RequestContext->IrpSp->Flags & SL_OPEN_PAGING_FILE) {
      // Paging file is not supported
      // We would have otherwise set FSRTL_FLAG2_IS_PAGING_FILE
      // and clear FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS on
      // fcb->AdvancedFCBHeader.Flags2
      status = STATUS_ACCESS_DENIED;
      __leave;
    }

    if (allocateCcb) {
      ccb = DokanAllocateCCB(RequestContext, fcb);
    }

    if (ccb == NULL) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Was not able to allocate CCB");
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    if (RequestContext->IrpSp->Parameters.Create.Options &
        FILE_OPEN_FOR_BACKUP_INTENT) {
      DOKAN_LOG_FINE_IRP(RequestContext, "FILE_OPEN_FOR_BACKUP_INTENT");
    }

    fileObject->FsContext = &fcb->AdvancedFCBHeader;
    fileObject->FsContext2 = ccb;
    fileObject->PrivateCacheMap = NULL;
    fileObject->SectionObjectPointer = &fcb->SectionObjectPointers;
    if (fcb->IsKeepalive) {
      DokanLogInfo(&logger, L"Opened keepalive file from process %d.",
                   RequestContext->ProcessId);
    }
    if (fcb->BlockUserModeDispatch) {
      RequestContext->Irp->IoStatus.Information = FILE_OPENED;
      status = STATUS_SUCCESS;
      __leave;
    }
    // fileObject->Flags |= FILE_NO_INTERMEDIATE_BUFFERING;

    alignedEventContextSize = PointerAlignSize(sizeof(EVENT_CONTEXT));

    if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState) {

      if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->SecurityDescriptor) {
        // (CreateOptions & FILE_DIRECTORY_FILE) == FILE_DIRECTORY_FILE
        if (SeAssignSecurity(
                NULL,  // we don't keep track of parents, this will have to be
                       // handled in user mode
                RequestContext->IrpSp->Parameters.Create.SecurityContext
                    ->AccessState->SecurityDescriptor,
                &newFileSecurityDescriptor,
                (RequestContext->IrpSp->Parameters.Create.Options &
                 FILE_DIRECTORY_FILE) ||
                    (RequestContext->IrpSp->Flags & SL_OPEN_TARGET_DIRECTORY),
                &RequestContext->IrpSp->Parameters.Create.SecurityContext
                     ->AccessState->SubjectSecurityContext,
                IoGetFileObjectGenericMapping(), PagedPool) == STATUS_SUCCESS) {
          securityDescriptorSize = PointerAlignSize(
              RtlLengthSecurityDescriptor(newFileSecurityDescriptor));
        } else {
          newFileSecurityDescriptor = NULL;
        }
      }

      if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->ObjectName
              .Length > 0) {
        // add 1 WCHAR for NULL
        alignedObjectNameSize = PointerAlignSize(
            sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) +
            RequestContext->IrpSp->Parameters.Create.SecurityContext
                ->AccessState->ObjectName.Length +
            sizeof(WCHAR));
      }
      // else alignedObjectNameSize =
      // PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE)) SEE
      // DECLARATION

      if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->ObjectTypeName
              .Length > 0) {
        // add 1 WCHAR for NULL
        alignedObjectTypeNameSize =
            PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) +
            RequestContext->IrpSp->Parameters.Create.SecurityContext
                                 ->AccessState->ObjectTypeName.Length +
                             sizeof(WCHAR));
      }
      // else alignedObjectTypeNameSize =
      // PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE)) SEE
      // DECLARATION
    }

    eventLength = alignedEventContextSize + securityDescriptorSize;
    eventLength += alignedObjectNameSize;
    eventLength += alignedObjectTypeNameSize;
    eventLength += (parentDir ? fileNameLength : fcb->FileName.Length) +
                   sizeof(WCHAR); // add WCHAR for NULL

    eventContext =
        AllocateEventContext(RequestContext, eventLength, ccb);

    if (eventContext == NULL) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Was not able to allocate eventContext");
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    RtlZeroMemory((char *)eventContext + alignedEventContextSize,
                  eventLength - alignedEventContextSize);

    if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState) {
      // Copy security context
      eventContext->Operation.Create.SecurityContext.AccessState
          .SecurityEvaluated =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->SecurityEvaluated;
      eventContext->Operation.Create.SecurityContext.AccessState.GenerateAudit =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->GenerateAudit;
      eventContext->Operation.Create.SecurityContext.AccessState
          .GenerateOnClose = RequestContext->IrpSp->Parameters.Create
                                 .SecurityContext->AccessState->GenerateOnClose;
      eventContext->Operation.Create.SecurityContext.AccessState
          .AuditPrivileges = RequestContext->IrpSp->Parameters.Create
                                 .SecurityContext->AccessState->AuditPrivileges;
      eventContext->Operation.Create.SecurityContext.AccessState.Flags =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->Flags;
      eventContext->Operation.Create.SecurityContext.AccessState
          .RemainingDesiredAccess =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->RemainingDesiredAccess;
      eventContext->Operation.Create.SecurityContext.AccessState
          .PreviouslyGrantedAccess =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->PreviouslyGrantedAccess;
      eventContext->Operation.Create.SecurityContext.AccessState
          .OriginalDesiredAccess =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->OriginalDesiredAccess;

      // NOTE: AccessState offsets are relative to the start address of
      // AccessState

      if (securityDescriptorSize > 0) {
        eventContext->Operation.Create.SecurityContext.AccessState
            .SecurityDescriptorOffset =
            (ULONG)(((char *)eventContext + alignedEventContextSize) -
                    (char *)&eventContext->Operation.Create.SecurityContext
                        .AccessState);
      }

      eventContext->Operation.Create.SecurityContext.AccessState
          .UnicodeStringObjectNameOffset = (ULONG)(
          ((char *)eventContext + alignedEventContextSize +
           securityDescriptorSize) -
          (char *)&eventContext->Operation.Create.SecurityContext.AccessState);
      eventContext->Operation.Create.SecurityContext.AccessState
          .UnicodeStringObjectTypeOffset =
          eventContext->Operation.Create.SecurityContext.AccessState
              .UnicodeStringObjectNameOffset +
          alignedObjectNameSize;
    }

    OplockDebugRecordCreateRequest(
        fcb,
        RequestContext->IrpSp->Parameters.Create.SecurityContext->DesiredAccess,
        RequestContext->IrpSp->Parameters.Create.ShareAccess);

    // Other SecurityContext attributes
    eventContext->Operation.Create.SecurityContext.DesiredAccess =
        RequestContext->IrpSp->Parameters.Create.SecurityContext->DesiredAccess;

    // Other Create attributes
    eventContext->Operation.Create.FileAttributes =
        RequestContext->IrpSp->Parameters.Create.FileAttributes;
    eventContext->Operation.Create.CreateOptions =
        RequestContext->IrpSp->Parameters.Create.Options;
    if (IS_DEVICE_READ_ONLY(
            RequestContext->DeviceObject) &&  // do not reorder eval as
        disposition == FILE_OPEN_IF) {
      // Substitute FILE_OPEN for FILE_OPEN_IF
      // We check on return from userland in DokanCompleteCreate below
      // and if file didn't exist, return write-protected status
      eventContext->Operation.Create.CreateOptions &=
          ((FILE_OPEN << 24) | 0x00FFFFFF);
    }
    eventContext->Operation.Create.ShareAccess =
        RequestContext->IrpSp->Parameters.Create.ShareAccess;
    eventContext->Operation.Create.FileNameLength =
        parentDir ? fileNameLength : fcb->FileName.Length;
    eventContext->Operation.Create.FileNameOffset =
        (ULONG)(((char *)eventContext + alignedEventContextSize +
                 securityDescriptorSize + alignedObjectNameSize +
                 alignedObjectTypeNameSize) -
                (char *)&eventContext->Operation.Create);

    if (newFileSecurityDescriptor != NULL) {
      // Copy security descriptor
      RtlCopyMemory((char *)eventContext + alignedEventContextSize,
                    newFileSecurityDescriptor,
                    RtlLengthSecurityDescriptor(newFileSecurityDescriptor));
      SeDeassignSecurity(&newFileSecurityDescriptor);
      newFileSecurityDescriptor = NULL;
    }

    if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState) {
      // Object name
      intermediateUnicodeStr = (PDOKAN_UNICODE_STRING_INTERMEDIATE)(
          (char *)&eventContext->Operation.Create.SecurityContext.AccessState +
          eventContext->Operation.Create.SecurityContext.AccessState
              .UnicodeStringObjectNameOffset);
      intermediateUnicodeStr->Length =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->ObjectName.Length;
      intermediateUnicodeStr->MaximumLength = (USHORT)alignedObjectNameSize;

      if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->ObjectName.Length > 0) {
        RtlCopyMemory(intermediateUnicodeStr->Buffer,
                      RequestContext->IrpSp->Parameters.Create.SecurityContext
                          ->AccessState->ObjectName.Buffer,
                      RequestContext->IrpSp->Parameters.Create.SecurityContext
                          ->AccessState->ObjectName.Length);

        *(WCHAR *)((char *)intermediateUnicodeStr->Buffer +
                   intermediateUnicodeStr->Length) = 0;
      } else {
        intermediateUnicodeStr->Buffer[0] = 0;
      }

      // Object type name
      intermediateUnicodeStr = (PDOKAN_UNICODE_STRING_INTERMEDIATE)(
          (char *)intermediateUnicodeStr + alignedObjectNameSize);
      intermediateUnicodeStr->Length =
          RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->ObjectTypeName.Length;
      intermediateUnicodeStr->MaximumLength = (USHORT)alignedObjectTypeNameSize;

      if (RequestContext->IrpSp->Parameters.Create.SecurityContext->AccessState
              ->ObjectTypeName.Length > 0) {
        RtlCopyMemory(intermediateUnicodeStr->Buffer,
                      RequestContext->IrpSp->Parameters.Create.SecurityContext
                          ->AccessState->ObjectTypeName.Buffer,
                      RequestContext->IrpSp->Parameters.Create.SecurityContext
                          ->AccessState->ObjectTypeName.Length);

        *(WCHAR *)((char *)intermediateUnicodeStr->Buffer +
                   intermediateUnicodeStr->Length) = 0;
      } else {
        intermediateUnicodeStr->Buffer[0] = 0;
      }
    }

    // other context info
    eventContext->Context = 0;
    eventContext->FileFlags |= DokanFCBFlagsGet(fcb);

    // copy the file name

    RtlCopyMemory(((char *)&eventContext->Operation.Create +
                   eventContext->Operation.Create.FileNameOffset),
                  parentDir ? fileName : fcb->FileName.Buffer,
                  parentDir ? fileNameLength : fcb->FileName.Length);
    *(PWCHAR)((char *)&eventContext->Operation.Create +
              eventContext->Operation.Create.FileNameOffset +
              (parentDir ? fileNameLength : fcb->FileName.Length)) = 0;

    // The FCB lock used to be released here, but that creates a race condition
    // with oplock allocation, which is done lazily during calls like
    // FsRtlOplockFsctrl. The OPLOCK struct is really just an opaque pointer to
    // a NONOPAQUE_OPLOCK that is lazily allocated, and the OPLOCK is changed to
    // point to that without any hidden locking. Once it exists, changes to the
    // oplock state are automatically guarded by a mutex inside the
    // NONOPAQUE_OPLOCK.

    //
    // Oplock
    //

    openRequiringOplock =
        BooleanFlagOn(RequestContext->IrpSp->Parameters.Create.Options,
                                        FILE_OPEN_REQUIRING_OPLOCK);
    if (FlagOn(RequestContext->IrpSp->Parameters.Create.Options,
               FILE_COMPLETE_IF_OPLOCKED)) {
      OplockDebugRecordFlag(fcb, DOKAN_OPLOCK_DEBUG_COMPLETE_IF_OPLOCKED);
    }

    // Share access support

    if (fcb->FileCount > 1) {

      //
      //  Check if the Fcb has the proper share access.  This routine will
      //  also
      //  check for writable user sections if the user did not allow write
      //  sharing.
      //

      // DokanCheckShareAccess() will update the share access in the fcb if
      // the operation is allowed to go forward

      status = DokanCheckShareAccess(
          RequestContext, fileObject, fcb,
          eventContext->Operation.Create.SecurityContext.DesiredAccess,
          eventContext->Operation.Create.ShareAccess);

      if (!NT_SUCCESS(status)) {

        DOKAN_LOG_FINE_IRP(RequestContext, "DokanCheckShareAccess failed with 0x%x %s",
                         status, DokanGetNTSTATUSStr(status));

        NTSTATUS OplockBreakStatus = STATUS_SUCCESS;

        //
        //  If we got a sharing violation try to break outstanding
        //  handle
        //  oplocks and retry the sharing check.  If the caller
        //  specified
        //  FILE_COMPLETE_IF_OPLOCKED we don't bother breaking the
        //  oplock;
        //  we just return the sharing violation.
        //
        if ((status == STATUS_SHARING_VIOLATION) &&
            !FlagOn(RequestContext->IrpSp->Parameters.Create.Options,
                    FILE_COMPLETE_IF_OPLOCKED)) {

          POPLOCK oplock = DokanGetFcbOplock(fcb);
          // This may enter a wait state!

          OplockDebugRecordFlag(fcb,
                                DOKAN_OPLOCK_DEBUG_EXPLICIT_BREAK_IN_CREATE);
          OplockDebugRecordProcess(fcb);

          OplockBreakStatus = FsRtlOplockBreakH(
              oplock, RequestContext->Irp, 0, RequestContext->DeviceObject,
              DokanRetryCreateAfterOplockBreak,
              DokanPrePostIrp);

          //
          //  If FsRtlOplockBreakH returned STATUS_PENDING,
          //  then the IRP
          //  has been posted and we need to stop working.
          //
          if (OplockBreakStatus == STATUS_PENDING) {
            DOKAN_LOG_FINE_IRP(RequestContext, "FsRtlOplockBreakH returned STATUS_PENDING");
            status = STATUS_PENDING;
            __leave;
            //
            //  If FsRtlOplockBreakH returned an error
            //  we want to return that now.
            //
          } else if (!NT_SUCCESS(OplockBreakStatus)) {
            DOKAN_LOG_FINE_IRP(RequestContext, "FsRtlOplockBreakH returned 0x%x %s",
                          OplockBreakStatus,
                          DokanGetNTSTATUSStr(OplockBreakStatus));
            status = OplockBreakStatus;
            __leave;

            //
            //  Otherwise FsRtlOplockBreakH returned
            //  STATUS_SUCCESS, indicating
            //  that there is no oplock to be broken.
            //  The sharing violation is
            //  returned in that case.
            //
            //  We actually now pass null for the callback to
            //  FsRtlOplockBreakH so it will block until
            //  the oplock break is sent to holder of the oplock.
            //  This doesn't necessarily mean that the
            //  resourc was freed (file was closed) yet,
            //  but we check share access again in case it was
            //  to see if we can proceed normally or if we
            //  still have to reutrn a sharing violation.
          } else {
            status = DokanCheckShareAccess(
                RequestContext, fileObject, fcb,
                eventContext->Operation.Create.SecurityContext.DesiredAccess,
                eventContext->Operation.Create.ShareAccess);
            DOKAN_LOG_FINE_IRP(RequestContext, "Checked share access again, status = 0x%08x",
                      status);
            NT_ASSERT(OplockBreakStatus == STATUS_SUCCESS);
            if (status != STATUS_SUCCESS)
              __leave;
          }

          //
          //  The initial sharing check failed with something
          //  other than sharing
          //  violation (which should never happen, but let's
          //  be future-proof),
          //  or we *did* get a sharing violation and the
          //  caller specified
          //  FILE_COMPLETE_IF_OPLOCKED.  Either way this
          //  create is over.
          //
          // We can't really handle FILE_COMPLETE_IF_OPLOCKED correctly because
          // we don't have a way of creating a useable file handle
          // without actually creating the file in user mode, which
          // won't work unless the oplock gets broken before the
          // user mode create happens.
          // It is believed that FILE_COMPLETE_IF_OPLOCKED is extremely
          // rare and may never happened during normal operation.
        } else {

          if (status == STATUS_SHARING_VIOLATION &&
              FlagOn(RequestContext->IrpSp->Parameters.Create.Options,
                     FILE_COMPLETE_IF_OPLOCKED)) {
            DOKAN_LOG_FINE_IRP(
                RequestContext,
                "Failing a create with FILE_COMPLETE_IF_OPLOCKED because "
                "of sharing violation");
          }

          DOKAN_LOG_FINE_IRP(RequestContext,
                             "Create: sharing/oplock failed, status = 0x%08x",
                             status);
          __leave;
        }
      }
      IoUpdateShareAccess(fileObject, &fcb->ShareAccess);
    } else {
      IoSetShareAccess(
          eventContext->Operation.Create.SecurityContext.DesiredAccess,
          eventContext->Operation.Create.ShareAccess, fileObject,
          &fcb->ShareAccess);
    }

    unwindShareAccess = TRUE;

    //  Now check that we can continue based on the oplock state of the
    //  file.  If there are no open handles yet in addition to this new one
    //  we don't need to do this check; oplocks can only exist when there are
    //  handles.
    //
    //  It is important that we modified the DesiredAccess in place so
    //  that the Oplock check proceeds against any added access we had
    //  to give the caller.
    //
    if (fcb->FileCount > 1) {
      status = FsRtlCheckOplock(DokanGetFcbOplock(fcb), RequestContext->Irp,
                           RequestContext->DeviceObject,
                           DokanRetryCreateAfterOplockBreak, DokanPrePostIrp);

      //
      //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
      //  to service an oplock break and we need to leave now.
      //
      if (status == STATUS_PENDING) {
        DOKAN_LOG_FINE_IRP(RequestContext,
                           "FsRtlCheckOplock returned STATUS_PENDING, fcb = "
                           "%p, fileCount = %lu",
                           fcb, fcb->FileCount);
        __leave;
      }
    }

    //
    //  Let's make sure that if the caller provided an oplock key that it
    //  gets stored in the file object.
    //
    // OPLOCK_FLAG_OPLOCK_KEY_CHECK_ONLY means that no blocking.

    status =
        FsRtlCheckOplockEx(DokanGetFcbOplock(fcb), RequestContext->Irp,
                           OPLOCK_FLAG_OPLOCK_KEY_CHECK_ONLY, NULL, NULL, NULL);

    if (!NT_SUCCESS(status)) {
      DOKAN_LOG_FINE_IRP(RequestContext, "FsRtlCheckOplockEx return status = 0x%08x",
                       status);
      __leave;
    }

    if (openRequiringOplock) {
      DOKAN_LOG_FINE_IRP(RequestContext, "OpenRequiringOplock");
      OplockDebugRecordAtomicRequest(fcb);

      //
      //  If the caller wants atomic create-with-oplock semantics, tell
      //  the oplock package.
      if ((status == STATUS_SUCCESS)) {
        status = FsRtlOplockFsctrl(DokanGetFcbOplock(fcb), RequestContext->Irp,
                                   fcb->FileCount);
      }

      //
      //  If we've encountered a failure we need to leave.  FsRtlCheckOplock
      //  will have returned STATUS_OPLOCK_BREAK_IN_PROGRESS if it initiated
      //  and oplock break and the caller specified FILE_COMPLETE_IF_OPLOCKED
      //  on the create call.  That's an NT_SUCCESS code, so we need to keep
      //  going.
      //
      if ((status != STATUS_SUCCESS) &&
          (status != STATUS_OPLOCK_BREAK_IN_PROGRESS)) {
        DOKAN_LOG_FINE_IRP(RequestContext,
                      "FsRtlOplockFsctrl failed with 0x%x %s, fcb = %p, "
                      "fileCount = %lu",
                      status, DokanGetNTSTATUSStr(status), fcb, fcb->FileCount);

        __leave;
      } else if (status == STATUS_OPLOCK_BREAK_IN_PROGRESS) {
        DOKAN_LOG_FINE_IRP(RequestContext, "STATUS_OPLOCK_BREAK_IN_PROGRESS");
      }
      // if we fail after this point, the oplock will need to be backed out
      // if the oplock was granted (status == STATUS_SUCCESS)
      if (status == STATUS_SUCCESS) {
        ccb->AtomicOplockRequestPending = TRUE;
      }
    }

    // register this IRP to waiting IPR list
    status = DokanRegisterPendingIrp(RequestContext, eventContext);

    eventContextConsumed = TRUE;
  } __finally {
    // Getting here by __leave isn't always a failure,
    // so we shouldn't necessarily clean up only because
    // AbnormalTermination() returns true

    //
    //  If we're not getting out with success, and if the caller wanted
    //  atomic create-with-oplock semantics make sure we back out any
    //  oplock that may have been granted.
    //
    //  Also unwind any share access that was added to the fcb

    if (!NT_SUCCESS(status)) {
      if (ccb != NULL) {
        DokanMaybeBackOutAtomicOplockRequest(ccb, RequestContext->Irp);
      }
      if (unwindShareAccess) {
        IoRemoveShareAccess(fileObject, &fcb->ShareAccess);
      }
    }

    if (fcbLocked)
      DokanFCBUnlock(fcb);

    if (relatedFileName) {
      ExFreePool(relatedFileName->Buffer);
      ExFreePool(relatedFileName);
    }

    if (!NT_SUCCESS(status)) {

      // DokanRegisterPendingIrp consumes event context

      if (!eventContextConsumed && eventContext) {
        DokanFreeEventContext(eventContext);
      }
      if (ccb) {
        DokanFreeCCB(RequestContext, ccb);
      }
      if (fcb) {
        DokanFreeFCB(RequestContext->Vcb, fcb);
      }

      // Since we have just un-referenced the CCB and FCB, don't leave the
      // contexts on the FILE_OBJECT pointing to them, or they might be misused
      // later. The pgpfsfd filter driver has been seen to do that when saving
      // attachments from Outlook.
      fileObject->FsContext = NULL;
      fileObject->FsContext2 = NULL;
    }

    // If it's SL_OPEN_TARGET_DIRECTORY then
    // the FCB takes ownership of parentDir instead of fileName
    if (parentDir && fileName) {
      ExFreePool(fileName);
    }
  }

  return status;
}

VOID DokanCompleteCreate(__in PREQUEST_CONTEXT RequestContext,
                         __in PEVENT_INFORMATION EventInfo) {
  PDokanCCB ccb = NULL;
  PDokanFCB fcb = NULL;


  ccb = RequestContext->IrpSp->FileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  DokanFCBLockRW(fcb);

  ccb->UserContext = EventInfo->Context;
  RequestContext->Irp->IoStatus.Status = EventInfo->Status;
  RequestContext->Irp->IoStatus.Information =
      EventInfo->Operation.Create.Information;
  DOKAN_LOG_FINE_IRP(
      RequestContext, "FileObject=%p CreateInformation=%s",
      RequestContext->IrpSp->FileObject,
      DokanGetCreateInformationStr(RequestContext->Irp->IoStatus.Information));

  // If volume is write-protected, we subbed FILE_OPEN for FILE_OPEN_IF
  // before call to userland in DokanDispatchCreate.
  // In this case, a not found error should return write protected status.
  if ((RequestContext->Irp->IoStatus.Information == FILE_DOES_NOT_EXIST) &&
      (IS_DEVICE_READ_ONLY(RequestContext->IrpSp->DeviceObject))) {

    DWORD disposition =
        (RequestContext->IrpSp->Parameters.Create.Options >> 24) & 0x000000ff;
    if (disposition == FILE_OPEN_IF) {
      DOKAN_LOG_FINE_IRP(RequestContext, "Media is write protected");
      RequestContext->Irp->IoStatus.Status = STATUS_MEDIA_WRITE_PROTECTED;
    }
  }

  if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status) &&
      (RequestContext->IrpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE ||
       EventInfo->Operation.Create.Flags & DOKAN_FILE_DIRECTORY)) {
    if (RequestContext->IrpSp->Parameters.Create.Options &
        FILE_DIRECTORY_FILE) {
      DOKAN_LOG_FINE_IRP(RequestContext, "FILE_DIRECTORY_FILE %p", fcb);
    } else {
      DOKAN_LOG_FINE_IRP(RequestContext, "DOKAN_FILE_DIRECTORY %p", fcb);
    }
    DokanFCBFlagsSetBit(fcb, DOKAN_FILE_DIRECTORY);
  }

  if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status)) {
    DokanCCBFlagsSetBit(ccb, DOKAN_FILE_OPENED);
  }

  // On Windows 8 and above, you can mark the file
  // for delete-on-close at create time, which is acted on during cleanup.
  if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status) &&
      RequestContext->IrpSp->Parameters.Create.Options & FILE_DELETE_ON_CLOSE) {
    DokanFCBFlagsSetBit(fcb, DOKAN_DELETE_ON_CLOSE);
    DokanCCBFlagsSetBit(ccb, DOKAN_DELETE_ON_CLOSE);
    DOKAN_LOG_FINE_IRP(
        RequestContext,
        "FILE_DELETE_ON_CLOSE is set so remember for delete in cleanup");
  }

  if (NT_SUCCESS(RequestContext->Irp->IoStatus.Status)) {
    if (RequestContext->Irp->IoStatus.Information == FILE_CREATED) {
      if (DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {
        DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_DIR_NAME,
                                FILE_ACTION_ADDED);
      } else {
        DokanNotifyReportChange(RequestContext, fcb, FILE_NOTIFY_CHANGE_FILE_NAME,
                                FILE_ACTION_ADDED);
      }
    }
    ccb->AtomicOplockRequestPending = FALSE;
  } else {
    DokanMaybeBackOutAtomicOplockRequest(ccb, RequestContext->Irp);
    DokanFreeCCB(RequestContext, ccb);
    IoRemoveShareAccess(RequestContext->IrpSp->FileObject, &fcb->ShareAccess);
    DokanFCBUnlock(fcb);
    DokanFreeFCB(RequestContext->Vcb, fcb);
    fcb = NULL;
    RequestContext->IrpSp->FileObject->FsContext2 = NULL;
  }

  if (fcb)
    DokanFCBUnlock(fcb);
}
