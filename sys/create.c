/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanDispatchCreate)
#endif

// We must NOT call without VCB lock
PDokanFCB DokanAllocateFCB(__in PDokanVCB Vcb) {
  PDokanFCB fcb = ExAllocateFromLookasideListEx(&g_DokanFCBLookasideList);

  if (fcb == NULL) {
    return NULL;
  }

  ASSERT(Vcb != NULL);

  RtlZeroMemory(fcb, sizeof(DokanFCB));

  fcb->Identifier.Type = FCB;
  fcb->Identifier.Size = sizeof(DokanFCB);

  fcb->Vcb = Vcb;

  ExInitializeResourceLite(&fcb->MainResource);
  ExInitializeResourceLite(&fcb->PagingIoResource);

  ExInitializeFastMutex(&fcb->AdvancedFCBHeaderMutex);

#if _WIN32_WINNT >= 0x0501
  FsRtlSetupAdvancedHeader(&fcb->AdvancedFCBHeader,
                           &fcb->AdvancedFCBHeaderMutex);
#else
  if (DokanFsRtlTeardownPerStreamContexts) {
    FsRtlSetupAdvancedHeader(&fcb->AdvancedFCBHeader,
                             &fcb->AdvancedFCBHeaderMutex);
  }
#endif

  fcb->AdvancedFCBHeader.ValidDataLength.LowPart = 0xffffffff;
  fcb->AdvancedFCBHeader.ValidDataLength.HighPart = 0x7fffffff;

  fcb->AdvancedFCBHeader.Resource = &fcb->MainResource;
  fcb->AdvancedFCBHeader.PagingIoResource = &fcb->PagingIoResource;

  fcb->AdvancedFCBHeader.AllocationSize.QuadPart = 4096;
  fcb->AdvancedFCBHeader.FileSize.QuadPart = 4096;

  fcb->AdvancedFCBHeader.IsFastIoPossible = FastIoIsNotPossible;
  FsRtlInitializeOplock(DokanGetFcbOplock(fcb));

  ExInitializeResourceLite(&fcb->Resource);

  InitializeListHead(&fcb->NextCCB);
  InsertTailList(&Vcb->NextFCB, &fcb->NextFCB);

  InterlockedIncrement(&Vcb->FcbAllocated);

  return fcb;
}

PDokanFCB DokanGetFCB(__in PDokanVCB Vcb, __in PWCHAR FileName,
                      __in ULONG FileNameLength) {
  PLIST_ENTRY thisEntry, nextEntry, listHead;
  PDokanFCB fcb = NULL;
  ULONG pos;

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

  // search the FCB which is already allocated
  // (being used now)
  listHead = &Vcb->NextFCB;

  for (thisEntry = listHead->Flink; thisEntry != listHead;
       thisEntry = nextEntry) {

    nextEntry = thisEntry->Flink;

    fcb = CONTAINING_RECORD(thisEntry, DokanFCB, NextFCB);

    if (fcb->FileName.Length == FileNameLength) {
      // FileNameLength in bytes
      for (pos = 0; pos < FileNameLength / sizeof(WCHAR); ++pos) {
        if (fcb->FileName.Buffer[pos] != FileName[pos])
          break;
      }
      // we have the FCB which is already allocated and used
      if (pos == FileNameLength / sizeof(WCHAR))
        break;
    }

    fcb = NULL;
  }

  // we don't have FCB
  if (fcb == NULL) {
    DDbgPrint("  Allocate FCB\n");

    fcb = DokanAllocateFCB(Vcb);

    // no memory?
    if (fcb == NULL) {
      DDbgPrint("    Was not able to get FCB for FileName %ls\n", FileName);
      ExFreePool(FileName);
      ExReleaseResourceLite(&Vcb->Resource);
      KeLeaveCriticalRegion();
      return NULL;
    }

    ASSERT(fcb != NULL);

    fcb->FileName.Buffer = FileName;
    fcb->FileName.Length = (USHORT)FileNameLength;
    fcb->FileName.MaximumLength = (USHORT)FileNameLength;

    // we already have FCB
  } else {
    // FileName (argument) is never used and must be freed
    ExFreePool(FileName);
  }

  InterlockedIncrement(&fcb->FileCount);

  ExReleaseResourceLite(&Vcb->Resource);
  KeLeaveCriticalRegion();

  return fcb;
}

NTSTATUS
DokanFreeFCB(__in PDokanFCB Fcb) {
  PDokanVCB vcb;

  ASSERT(Fcb != NULL);

  vcb = Fcb->Vcb;

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE);
  ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

  InterlockedDecrement(&Fcb->FileCount);

  if (Fcb->FileCount == 0) {

    RemoveEntryList(&Fcb->NextFCB);

    DDbgPrint("  Free FCB:%p\n", Fcb);
    ExFreePool(Fcb->FileName.Buffer);

    FsRtlUninitializeOplock(DokanGetFcbOplock(Fcb));

#if _WIN32_WINNT >= 0x0501
    FsRtlTeardownPerStreamContexts(&Fcb->AdvancedFCBHeader);
#else
    if (DokanFsRtlTeardownPerStreamContexts) {
      DokanFsRtlTeardownPerStreamContexts(&Fcb->AdvancedFCBHeader);
    }
#endif
    ExReleaseResourceLite(&Fcb->Resource);

    ExDeleteResourceLite(&Fcb->Resource);
    ExDeleteResourceLite(&Fcb->MainResource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);

    InterlockedIncrement(&vcb->FcbFreed);
    ExFreeToLookasideListEx(&g_DokanFCBLookasideList, Fcb);

  } else {
    ExReleaseResourceLite(&Fcb->Resource);
  }

  ExReleaseResourceLite(&vcb->Resource);
  KeLeaveCriticalRegion();

  return STATUS_SUCCESS;
}

PDokanCCB DokanAllocateCCB(__in PDokanDCB Dcb, __in PDokanFCB Fcb) {
  PDokanCCB ccb = ExAllocateFromLookasideListEx(&g_DokanCCBLookasideList);

  if (ccb == NULL)
    return NULL;

  ASSERT(ccb != NULL);
  ASSERT(Fcb != NULL);

  RtlZeroMemory(ccb, sizeof(DokanCCB));

  ccb->Identifier.Type = CCB;
  ccb->Identifier.Size = sizeof(DokanCCB);

  ccb->Fcb = Fcb;

  ExInitializeResourceLite(&ccb->Resource);

  InitializeListHead(&ccb->NextCCB);

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

  InsertTailList(&Fcb->NextCCB, &ccb->NextCCB);

  ExReleaseResourceLite(&Fcb->Resource);
  KeLeaveCriticalRegion();

  ccb->MountId = Dcb->MountId;

  InterlockedIncrement(&Fcb->Vcb->CcbAllocated);
  return ccb;
}

NTSTATUS
DokanFreeCCB(__in PDokanCCB ccb) {
  PDokanFCB fcb;

  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&fcb->Resource, TRUE);

  RemoveEntryList(&ccb->NextCCB);

  ExReleaseResourceLite(&fcb->Resource);
  KeLeaveCriticalRegion();

  ExDeleteResourceLite(&ccb->Resource);

  if (ccb->SearchPattern) {
    ExFreePool(ccb->SearchPattern);
  }

  ExFreeToLookasideListEx(&g_DokanCCBLookasideList, ccb);
  InterlockedIncrement(&fcb->Vcb->CcbFreed);

  return STATUS_SUCCESS;
}

LONG DokanUnicodeStringChar(__in PUNICODE_STRING UnicodeString,
                            __in WCHAR Char) {
  ULONG i = 0;
  for (; i < UnicodeString->Length / sizeof(WCHAR); ++i) {
    if (UnicodeString->Buffer[i] == Char) {
      return i;
    }
  }
  return -1;
}

VOID SetFileObjectForVCB(__in PFILE_OBJECT FileObject, __in PDokanVCB Vcb) {
  FileObject->SectionObjectPointer = &Vcb->SectionObjectPointers;
  FileObject->FsContext = &Vcb->VolumeFileHeader;
}

NTSTATUS
DokanCheckShareAccess(_In_ PFILE_OBJECT FileObject, _In_ PDokanFCB FcbOrDcb,
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
  PAGED_CODE();

#if (NTDDI_VERSION >= NTDDI_VISTA)
  //
  //  Do an extra test for writeable user sections if the user did not allow
  //  write sharing - this is neccessary since a section may exist with no
  //  handles
  //  open to the file its based against.
  //
  if ((FcbOrDcb->Identifier.Type == FCB) &&
      !FlagOn(ShareAccess, FILE_SHARE_WRITE) &&
      FlagOn(DesiredAccess, FILE_EXECUTE | FILE_READ_DATA | FILE_WRITE_DATA |
                                FILE_APPEND_DATA | DELETE | MAXIMUM_ALLOWED) &&
      MmDoesFileHaveUserWritableReferences(&FcbOrDcb->SectionObjectPointers)) {

    DDbgPrint("  DokanCheckShareAccess FCB has no write shared access\n");
    return STATUS_SHARING_VIOLATION;
  }
#endif

  //
  //  Check if the Fcb has the proper share access.
  //
  return IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject,
                            &FcbOrDcb->ShareAccess, TRUE);
}

NTSTATUS
DokanDispatchCreate(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp)

/*++

Routine Description:

                This device control dispatcher handles create & close IRPs.

Arguments:

                DeviceObject - Context for the activity.
                Irp 		 - The device control argument block.

Return Value:

                NTSTATUS

--*/
{
  PDokanVCB vcb = NULL;
  PDokanDCB dcb = NULL;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_INVALID_PARAMETER;
  PFILE_OBJECT fileObject = NULL;
  ULONG info = 0;
  PEVENT_CONTEXT eventContext;
  PFILE_OBJECT relatedFileObject;
  ULONG fileNameLength = 0;
  ULONG eventLength;
  PDokanFCB fcb = NULL;
  PDokanCCB ccb = NULL;
  PWCHAR fileName = NULL;
  BOOLEAN needBackSlashAfterRelatedFile = FALSE;
  ULONG securityDescriptorSize = 0;
  ULONG alignedEventContextSize = 0;
  ULONG alignedObjectNameSize =
      PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE));
  ULONG alignedObjectTypeNameSize =
      PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE));
  PDOKAN_UNICODE_STRING_INTERMEDIATE intermediateUnicodeStr = NULL;
  PUNICODE_STRING relatedFileName = NULL;
  PSECURITY_DESCRIPTOR newFileSecurityDescriptor = NULL;
  BOOLEAN OpenRequiringOplock = FALSE;
  //BOOLEAN UnwindShareAccess = FALSE;

  PAGED_CODE();

  __try {
    DDbgPrint("==> DokanCreate\n");

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    if (irpSp->FileObject == NULL) {
      DDbgPrint("  irpSp->FileObject == NULL\n");
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    fileObject = irpSp->FileObject;
    relatedFileObject = fileObject->RelatedFileObject;

    DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
    DDbgPrint("  FileName:%wZ\n", &fileObject->FileName);

    vcb = DeviceObject->DeviceExtension;
    if (vcb == NULL) {
      DDbgPrint("  No device extension\n");
      status = STATUS_SUCCESS;
      __leave;
    }

    PrintIdType(vcb);

    if (GetIdentifierType(vcb) == DCB) {
      dcb = DeviceObject->DeviceExtension;
      if (!dcb->Mounted) {
        DDbgPrint("  IdentifierType is dcb which is not mounted\n");
        status = STATUS_VOLUME_DISMOUNTED;
        __leave;
      }
    }

    if (GetIdentifierType(vcb) != VCB) {
      DDbgPrint("  IdentifierType is not vcb\n");
      status = STATUS_SUCCESS;
      __leave;
    }
    dcb = vcb->Dcb;

    BOOLEAN isNetworkFileSystem =
        (dcb->VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);

    if (!isNetworkFileSystem) {
      if (relatedFileObject != NULL) {
        fileObject->Vpb = relatedFileObject->Vpb;
      } else {
        fileObject->Vpb = dcb->DeviceObject->Vpb;
      }
    }

    if (!vcb->HasEventWait) {
      DDbgPrint("  Here we only go in if some antivirus software tries to "
                "create files before startup is finished.\n");
      status = STATUS_SUCCESS;
      __leave;
    }

    DDbgPrint("  IrpSp->Flags = %d\n", irpSp->Flags);
    if (irpSp->Flags & SL_CASE_SENSITIVE) {
      DDbgPrint("  IrpSp->Flags SL_CASE_SENSITIVE\n");
    }
    if (irpSp->Flags & SL_FORCE_ACCESS_CHECK) {
      DDbgPrint("  IrpSp->Flags SL_FORCE_ACCESS_CHECK\n");
    }
    if (irpSp->Flags & SL_OPEN_PAGING_FILE) {
      DDbgPrint("  IrpSp->Flags SL_OPEN_PAGING_FILE\n");
    }
    if (irpSp->Flags & SL_OPEN_TARGET_DIRECTORY) {
      DDbgPrint("  IrpSp->Flags SL_OPEN_TARGET_DIRECTORY\n");
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
    if (relatedFileObject != NULL) {
      // Using relatedFileObject->FileName is not safe here, use cached filename
      // from context.
      if (relatedFileObject->FsContext2) {
        PDokanCCB relatedCcb = (PDokanCCB)relatedFileObject->FsContext2;
        if (relatedCcb->Fcb) {
          if (relatedCcb->Fcb->FileName.Length > 0 &&
              relatedCcb->Fcb->FileName.Buffer != NULL) {
            relatedFileName = &relatedCcb->Fcb->FileName;
          }
        }
      }
    }

    if (relatedFileName == NULL && fileObject->FileName.Length == 0) {

      DDbgPrint("   request for FS device\n");

      if (irpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE) {
        status = STATUS_NOT_A_DIRECTORY;
      } else {
        SetFileObjectForVCB(fileObject, vcb);
        info = FILE_OPENED;
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
        DDbgPrint("  when RelatedFileObject is specified, the file name should "
                  "be relative path\n");
        status = STATUS_OBJECT_NAME_INVALID;
        __leave;
      }
      if (relatedFileName->Length > 0 &&
          relatedFileName->Buffer[relatedFileName->Length / sizeof(WCHAR) -
                                  1] != '\\') {
        needBackSlashAfterRelatedFile = TRUE;
        fileNameLength += sizeof(WCHAR);
      }
    }

    // don't open file like stream
    if (!dcb->UseAltStream &&
        DokanUnicodeStringChar(&fileObject->FileName, L':') != -1) {
      DDbgPrint("    alternate stream\n");
      status = STATUS_INVALID_PARAMETER;
      info = 0;
      __leave;
    }

    // this memory is freed by DokanGetFCB if needed
    // "+ sizeof(WCHAR)" is for the last NULL character
    fileName = ExAllocatePool(fileNameLength + sizeof(WCHAR));
    if (fileName == NULL) {
      DDbgPrint("    Can't allocatePool for fileName\n");
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    RtlZeroMemory(fileName, fileNameLength + sizeof(WCHAR));

    if (relatedFileName != NULL) {
      DDbgPrint("  RelatedFileName:%wZ\n", relatedFileName);

      // copy the file name of related file object
      RtlCopyMemory(fileName, relatedFileName->Buffer, relatedFileName->Length);

      if (needBackSlashAfterRelatedFile) {
        ((PWCHAR)fileName)[relatedFileName->Length / sizeof(WCHAR)] = '\\';
      }
      // copy the file name of fileObject
      RtlCopyMemory((PCHAR)fileName + relatedFileName->Length +
                        (needBackSlashAfterRelatedFile ? sizeof(WCHAR) : 0),
                    fileObject->FileName.Buffer, fileObject->FileName.Length);

    } else {
      // if related file object is not specifed, copy the file name of file
      // object
      RtlCopyMemory(fileName, fileObject->FileName.Buffer,
                    fileObject->FileName.Length);
    }

    // Fail if device is read-only and request involves a write operation
    DWORD disposition = 0;
    if (IS_DEVICE_READ_ONLY(DeviceObject)) {
      disposition = (irpSp->Parameters.Create.Options >> 24) & 0x000000ff;

      if ((disposition == FILE_SUPERSEDE) || (disposition == FILE_CREATE) ||
          (disposition == FILE_OVERWRITE) ||
          (disposition == FILE_OVERWRITE_IF) ||
          (irpSp->Parameters.Create.Options & FILE_DELETE_ON_CLOSE)) {

        DDbgPrint("    Media is write protected\n");
        status = STATUS_MEDIA_WRITE_PROTECTED;
        ExFreePool(fileName);
        __leave;
      }
    }

    fcb = DokanGetFCB(vcb, fileName, fileNameLength);
    if (fcb == NULL) {
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    if (irpSp->Flags & SL_OPEN_PAGING_FILE) {
      fcb->AdvancedFCBHeader.Flags2 |= FSRTL_FLAG2_IS_PAGING_FILE;
      fcb->AdvancedFCBHeader.Flags2 &= ~FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS;
    }

    ccb = DokanAllocateCCB(dcb, fcb);
    if (ccb == NULL) {
      DDbgPrint("    Was not able to allocate CCB\n");
      DokanFreeFCB(fcb); // FileName is freed here
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    // remember FILE_DELETE_ON_CLOSE so than the file can be deleted in close
    // for windows 8
    if (irpSp->Parameters.Create.Options & FILE_DELETE_ON_CLOSE) {
      fcb->Flags |= DOKAN_DELETE_ON_CLOSE;
      DDbgPrint(
          "  FILE_DELETE_ON_CLOSE is set so remember for delete in cleanup\n");
    } else {
      fcb->Flags &= ~DOKAN_DELETE_ON_CLOSE;
    }

    fileObject->FsContext = &fcb->AdvancedFCBHeader;
    fileObject->FsContext2 = ccb;
    fileObject->PrivateCacheMap = NULL;
    fileObject->SectionObjectPointer = &fcb->SectionObjectPointers;
    // fileObject->Flags |= FILE_NO_INTERMEDIATE_BUFFERING;

    alignedEventContextSize = PointerAlignSize(sizeof(EVENT_CONTEXT));

    if (irpSp->Parameters.Create.SecurityContext->AccessState) {

      if (irpSp->Parameters.Create.SecurityContext->AccessState
              ->SecurityDescriptor) {
        // (CreateOptions & FILE_DIRECTORY_FILE) == FILE_DIRECTORY_FILE
        if (SeAssignSecurity(
                NULL, // we don't keep track of parents, this will have to be
                      // handled in user mode
                irpSp->Parameters.Create.SecurityContext->AccessState
                    ->SecurityDescriptor,
                &newFileSecurityDescriptor,
                (irpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE) ||
                    (irpSp->Flags & SL_OPEN_TARGET_DIRECTORY),
                &irpSp->Parameters.Create.SecurityContext->AccessState
                     ->SubjectSecurityContext,
                IoGetFileObjectGenericMapping(), PagedPool) == STATUS_SUCCESS) {

          securityDescriptorSize = PointerAlignSize(
              RtlLengthSecurityDescriptor(newFileSecurityDescriptor));
        } else {
          newFileSecurityDescriptor = NULL;
        }
      }

      if (irpSp->Parameters.Create.SecurityContext->AccessState->ObjectName
              .Length > 0) {
        // add 1 WCHAR for NULL
        alignedObjectNameSize =
            PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) +
                             irpSp->Parameters.Create.SecurityContext
                                 ->AccessState->ObjectName.Length +
                             sizeof(WCHAR));
      }
      // else alignedObjectNameSize =
      // PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE)) SEE
      // DECLARATION

      if (irpSp->Parameters.Create.SecurityContext->AccessState->ObjectTypeName
              .Length > 0) {
        // add 1 WCHAR for NULL
        alignedObjectTypeNameSize =
            PointerAlignSize(sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) +
                             irpSp->Parameters.Create.SecurityContext
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
    eventLength += fcb->FileName.Length + sizeof(WCHAR); // add WCHAR for NULL

    eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);

    if (eventContext == NULL) {
      DDbgPrint("    Was not able to allocate eventContext\n");
      status = STATUS_INSUFFICIENT_RESOURCES;
      __leave;
    }

    RtlZeroMemory((char *)eventContext + alignedEventContextSize,
                  eventLength - alignedEventContextSize);

    if (irpSp->Parameters.Create.SecurityContext->AccessState) {
      // Copy security context
      eventContext->Operation.Create.SecurityContext.AccessState
          .SecurityEvaluated = irpSp->Parameters.Create.SecurityContext
                                   ->AccessState->SecurityEvaluated;
      eventContext->Operation.Create.SecurityContext.AccessState.GenerateAudit =
          irpSp->Parameters.Create.SecurityContext->AccessState->GenerateAudit;
      eventContext->Operation.Create.SecurityContext.AccessState
          .GenerateOnClose = irpSp->Parameters.Create.SecurityContext
                                 ->AccessState->GenerateOnClose;
      eventContext->Operation.Create.SecurityContext.AccessState
          .AuditPrivileges = irpSp->Parameters.Create.SecurityContext
                                 ->AccessState->AuditPrivileges;
      eventContext->Operation.Create.SecurityContext.AccessState.Flags =
          irpSp->Parameters.Create.SecurityContext->AccessState->Flags;
      eventContext->Operation.Create.SecurityContext.AccessState
          .RemainingDesiredAccess = irpSp->Parameters.Create.SecurityContext
                                        ->AccessState->RemainingDesiredAccess;
      eventContext->Operation.Create.SecurityContext.AccessState
          .PreviouslyGrantedAccess = irpSp->Parameters.Create.SecurityContext
                                         ->AccessState->PreviouslyGrantedAccess;
      eventContext->Operation.Create.SecurityContext.AccessState
          .OriginalDesiredAccess = irpSp->Parameters.Create.SecurityContext
                                       ->AccessState->OriginalDesiredAccess;

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

    // Other SecurityContext attributes
    eventContext->Operation.Create.SecurityContext.DesiredAccess =
        irpSp->Parameters.Create.SecurityContext->DesiredAccess;

    // Other Create attributes
    eventContext->Operation.Create.FileAttributes =
        irpSp->Parameters.Create.FileAttributes;
    eventContext->Operation.Create.CreateOptions =
        irpSp->Parameters.Create.Options;
    if (IS_DEVICE_READ_ONLY(DeviceObject) && // do not reorder eval as
        disposition == FILE_OPEN_IF) { // disposition only init if read-only
      // Substitute FILE_OPEN for FILE_OPEN_IF
      // We check on return from userland in DokanCompleteCreate below
      // and if file didn't exist, return write-protected status
      eventContext->Operation.Create.CreateOptions &=
          ((FILE_OPEN << 24) | 0x00FFFFFF);
    }
    eventContext->Operation.Create.ShareAccess =
        irpSp->Parameters.Create.ShareAccess;
    eventContext->Operation.Create.FileNameLength = fcb->FileName.Length;
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

    if (irpSp->Parameters.Create.SecurityContext->AccessState) {
      // Object name
      intermediateUnicodeStr = (PDOKAN_UNICODE_STRING_INTERMEDIATE)(
          (char *)&eventContext->Operation.Create.SecurityContext.AccessState +
          eventContext->Operation.Create.SecurityContext.AccessState
              .UnicodeStringObjectNameOffset);
      intermediateUnicodeStr->Length = irpSp->Parameters.Create.SecurityContext
                                           ->AccessState->ObjectName.Length;
      intermediateUnicodeStr->MaximumLength = (USHORT)alignedObjectNameSize;

      if (irpSp->Parameters.Create.SecurityContext->AccessState->ObjectName
              .Length > 0) {

        RtlCopyMemory(intermediateUnicodeStr->Buffer,
                      irpSp->Parameters.Create.SecurityContext->AccessState
                          ->ObjectName.Buffer,
                      irpSp->Parameters.Create.SecurityContext->AccessState
                          ->ObjectName.Length);

        *(WCHAR *)((char *)intermediateUnicodeStr->Buffer +
                   intermediateUnicodeStr->Length) = 0;
      } else {
        intermediateUnicodeStr->Buffer[0] = 0;
      }

      // Object type name
      intermediateUnicodeStr = (PDOKAN_UNICODE_STRING_INTERMEDIATE)(
          (char *)intermediateUnicodeStr + alignedObjectNameSize);
      intermediateUnicodeStr->Length = irpSp->Parameters.Create.SecurityContext
                                           ->AccessState->ObjectTypeName.Length;
      intermediateUnicodeStr->MaximumLength = (USHORT)alignedObjectTypeNameSize;

      if (irpSp->Parameters.Create.SecurityContext->AccessState->ObjectTypeName
              .Length > 0) {

        RtlCopyMemory(intermediateUnicodeStr->Buffer,
                      irpSp->Parameters.Create.SecurityContext->AccessState
                          ->ObjectTypeName.Buffer,
                      irpSp->Parameters.Create.SecurityContext->AccessState
                          ->ObjectTypeName.Length);

        *(WCHAR *)((char *)intermediateUnicodeStr->Buffer +
                   intermediateUnicodeStr->Length) = 0;
      } else {
        intermediateUnicodeStr->Buffer[0] = 0;
      }
    }

    // other context info
    eventContext->Context = 0;
    eventContext->FileFlags |= fcb->Flags;

    // copy the file name
    RtlCopyMemory(((char *)&eventContext->Operation.Create +
                   eventContext->Operation.Create.FileNameOffset),
                  fcb->FileName.Buffer, fcb->FileName.Length);
    *(PWCHAR)((char *)&eventContext->Operation.Create +
              eventContext->Operation.Create.FileNameOffset +
              fcb->FileName.Length) = 0;

//
// Oplock
//

#if (NTDDI_VERSION >= NTDDI_WIN7)
    OpenRequiringOplock = BooleanFlagOn(irpSp->Parameters.Create.Options,
                                        FILE_OPEN_REQUIRING_OPLOCK);
#else
    OpenRequiringOplock = FALSE;
#endif

    /*
    // Share access support
    // WARNING: this implementation is incomplete and can result to unexcepted
STATUS_SHARING_VIOLATION results

    if (fcb->FileCount > 1) {
            //
            //  Check if the Fcb has the proper share access.  This routine will
also
            //  check for writable user sections if the user did not allow write
sharing.
            //
            if (!NT_SUCCESS(status = DokanCheckShareAccess(fileObject,
                    fcb,
                    eventContext->Operation.Create.SecurityContext.DesiredAccess,
                    eventContext->Operation.Create.ShareAccess))) {

                    DDbgPrint("   DokanCheckShareAccess failed with 0x%x\n",
status);

#if (NTDDI_VERSION >= NTDDI_WIN7)

                    NTSTATUS OplockBreakStatus = STATUS_SUCCESS;

                    //
                    //  If we got a sharing violation try to break outstanding
handle
                    //  oplocks and retry the sharing check.  If the caller
specified
                    //  FILE_COMPLETE_IF_OPLOCKED we don't bother breaking the
oplock;
                    //  we just return the sharing violation.
                    //
                    if ((status == STATUS_SHARING_VIOLATION) &&
                            !FlagOn(irpSp->Parameters.Create.Options,
FILE_COMPLETE_IF_OPLOCKED)) {

                            OplockBreakStatus =
FsRtlOplockBreakH(DokanGetFcbOplock(fcb),
                                    Irp,
                                    0,
                                    eventContext,
                                    DokanOplockComplete,
                                    DokanPrePostIrp);

                            //
                            //  If FsRtlOplockBreakH returned STATUS_PENDING,
then the IRP
                            //  has been posted and we need to stop working.
                            //
                            if (OplockBreakStatus == STATUS_PENDING) {
                                    DDbgPrint("   FsRtlOplockBreakH returned
STATUS_PENDING\n");
                                    status = STATUS_PENDING;
                                    __leave;
                                    //
                                    //  If FsRtlOplockBreakH returned an error
we want to return that now.
                                    //
                            }
                            else if (!NT_SUCCESS(OplockBreakStatus)) {
                                    DDbgPrint("   FsRtlOplockBreakH returned
0x%x\n", OplockBreakStatus);
                                    status = OplockBreakStatus;
                                    __leave;

                                    //
                                    //  Otherwise FsRtlOplockBreakH returned
STATUS_SUCCESS, indicating
                                    //  that there is no oplock to be broken.
The sharing violation is
                                    //  returned in that case.
                                    //
                            }
                            else {
                                    NT_ASSERT(OplockBreakStatus ==
STATUS_SUCCESS);
                                    __leave;
                            }

                            //
                            //  The initial sharing check failed with something
other than sharing
                            //  violation (which should never happen, but let's
be future-proof),
                            //  or we *did* get a sharing violation and the
caller specified
                            //  FILE_COMPLETE_IF_OPLOCKED.  Either way this
create is over.
                            //

                    }
                    else {
                            __leave;
                    }

#else
                    return status;
#endif
            }
    } else {
            IoSetShareAccess(eventContext->Operation.Create.SecurityContext.DesiredAccess,
                    eventContext->Operation.Create.ShareAccess,
                    fileObject,
                    &fcb->ShareAccess);
    }

    UnwindShareAccess = TRUE;*/

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
      status = FsRtlCheckOplock(DokanGetFcbOplock(fcb), Irp, eventContext,
                                DokanOplockComplete, DokanPrePostIrp);

      //
      //  if FsRtlCheckOplock returns STATUS_PENDING the IRP has been posted
      //  to service an oplock break and we need to leave now.
      //
      if (status == STATUS_PENDING) {
        DDbgPrint("   FsRtlCheckOplock returned STATUS_PENDING\n");
        __leave;
      }
    }

    if (OpenRequiringOplock) {
      DDbgPrint("   OpenRequiringOplock\n");
      //
      //  If the caller wants atomic create-with-oplock semantics, tell
      //  the oplock package.
      if ((status == STATUS_SUCCESS)) {
        status = FsRtlOplockFsctrl(DokanGetFcbOplock(fcb), Irp, fcb->FileCount);
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
        DDbgPrint("   FsRtlOplockFsctrl failed with 0x%x\n", status);
        __leave;
      }
    }

    // register this IRP to waiting IPR list
    status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);

  } __finally {

#if (NTDDI_VERSION >= NTDDI_WIN7)
    //
    //  If we're not getting out with success, and if the caller wanted
    //  atomic create-with-oplock semantics make sure we back out any
    //  oplock that may have been granted.
    //
    if (AbnormalTermination() || !NT_SUCCESS(status)) {
      if (OpenRequiringOplock && (status != STATUS_CANNOT_BREAK_OPLOCK) &&
          (fcb != NULL)) {
        FsRtlCheckOplockEx(DokanGetFcbOplock(fcb), Irp,
                           OPLOCK_FLAG_BACK_OUT_ATOMIC_OPLOCK, NULL, NULL,
                           NULL);
      }

      /*
	  if (UnwindShareAccess) {
        IoRemoveShareAccess(fileObject, &fcb->ShareAccess);
      }
	  */
    }
#endif

    DokanCompleteIrpRequest(Irp, status, info);

    DDbgPrint("<== DokanCreate\n");
  }

  return status;
}

VOID DokanCompleteCreate(__in PIRP_ENTRY IrpEntry,
                         __in PEVENT_INFORMATION EventInfo) {
  PIRP irp;
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status;
  ULONG info;
  PDokanCCB ccb;
  PDokanFCB fcb;

  irp = IrpEntry->Irp;
  irpSp = IrpEntry->IrpSp;

  DDbgPrint("==> DokanCompleteCreate\n");

  ccb = IrpEntry->FileObject->FsContext2;
  ASSERT(ccb != NULL);

  fcb = ccb->Fcb;
  ASSERT(fcb != NULL);

  DDbgPrint("  FileName:%wZ\n", &fcb->FileName);

  ccb->UserContext = EventInfo->Context;
  // DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

  status = EventInfo->Status;

  info = EventInfo->Operation.Create.Information;

  switch (info) {
  case FILE_OPENED:
    DDbgPrint("  FILE_OPENED\n");
    break;
  case FILE_CREATED:
    DDbgPrint("  FILE_CREATED\n");
    break;
  case FILE_OVERWRITTEN:
    DDbgPrint("  FILE_OVERWRITTEN\n");
    break;
  case FILE_DOES_NOT_EXIST:
    DDbgPrint("  FILE_DOES_NOT_EXIST\n");
    break;
  case FILE_EXISTS:
    DDbgPrint("  FILE_EXISTS\n");
    break;
  case FILE_SUPERSEDED:
    DDbgPrint("  FILE_SUPERSEDED\n");
    break;
  default:
    DDbgPrint("  info = %d\n", info);
    break;
  }

  // If volume is write-protected, we subbed FILE_OPEN for FILE_OPEN_IF
  // before call to userland in DokanDispatchCreate.
  // In this case, a not found error should return write protected status.
  if ((info == FILE_DOES_NOT_EXIST) &&
      (IS_DEVICE_READ_ONLY(irpSp->DeviceObject))) {

    DWORD disposition = (irpSp->Parameters.Create.Options >> 24) & 0x000000ff;
    if (disposition == FILE_OPEN_IF) {
      DDbgPrint("  Media is write protected\n");
      status = STATUS_MEDIA_WRITE_PROTECTED;
    }
  }

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&fcb->Resource, TRUE);
  if (NT_SUCCESS(status) &&
      (irpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE ||
       EventInfo->Operation.Create.Flags & DOKAN_FILE_DIRECTORY)) {
    if (irpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE) {
      DDbgPrint("  FILE_DIRECTORY_FILE %p\n", fcb);
    } else {
      DDbgPrint("  DOKAN_FILE_DIRECTORY %p\n", fcb);
    }
    fcb->Flags |= DOKAN_FILE_DIRECTORY;
  }
  ExReleaseResourceLite(&fcb->Resource);
  KeLeaveCriticalRegion();

  KeEnterCriticalRegion();
  ExAcquireResourceExclusiveLite(&ccb->Resource, TRUE);
  if (NT_SUCCESS(status)) {
    ccb->Flags |= DOKAN_FILE_OPENED;
  }
  ExReleaseResourceLite(&ccb->Resource);
  KeLeaveCriticalRegion();

  if (NT_SUCCESS(status)) {
    if (info == FILE_CREATED) {
      if (fcb->Flags & DOKAN_FILE_DIRECTORY) {
        DokanNotifyReportChange(fcb, FILE_NOTIFY_CHANGE_DIR_NAME,
                                FILE_ACTION_ADDED);
      } else {
        DokanNotifyReportChange(fcb, FILE_NOTIFY_CHANGE_FILE_NAME,
                                FILE_ACTION_ADDED);
      }
    }
  } else {
    DDbgPrint("   IRP_MJ_CREATE failed. Free CCB:%p\n", ccb);
    DokanFreeCCB(ccb);
    IoRemoveShareAccess(irpSp->FileObject, &fcb->ShareAccess);
    DokanFreeFCB(fcb);
  }

  DokanCompleteIrpRequest(irp, status, info);

  DDbgPrint("<== DokanCompleteCreate\n");
}
