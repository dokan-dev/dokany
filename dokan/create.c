/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2025 Google, Inc.
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

#include "dokani.h"
#include "dokan_pool.h"

#include <assert.h>

VOID SetIOSecurityContext(PEVENT_CONTEXT EventContext,
                          PDOKAN_IO_SECURITY_CONTEXT ioSecurityContext) {
  PDOKAN_UNICODE_STRING_INTERMEDIATE intermediateObjName = NULL;
  PDOKAN_UNICODE_STRING_INTERMEDIATE intermediateObjType = NULL;

  ioSecurityContext->AccessState.SecurityEvaluated =
      EventContext->Operation.Create.SecurityContext.AccessState
          .SecurityEvaluated;
  ioSecurityContext->AccessState.GenerateAudit =
      EventContext->Operation.Create.SecurityContext.AccessState.GenerateAudit;
  ioSecurityContext->AccessState.GenerateOnClose =
      EventContext->Operation.Create.SecurityContext.AccessState
          .GenerateOnClose;
  ioSecurityContext->AccessState.AuditPrivileges =
      EventContext->Operation.Create.SecurityContext.AccessState
          .AuditPrivileges;
  ioSecurityContext->AccessState.Flags =
      EventContext->Operation.Create.SecurityContext.AccessState.Flags;
  ioSecurityContext->AccessState.RemainingDesiredAccess =
      EventContext->Operation.Create.SecurityContext.AccessState
          .RemainingDesiredAccess;
  ioSecurityContext->AccessState.PreviouslyGrantedAccess =
      EventContext->Operation.Create.SecurityContext.AccessState
          .PreviouslyGrantedAccess;
  ioSecurityContext->AccessState.OriginalDesiredAccess =
      EventContext->Operation.Create.SecurityContext.AccessState
          .OriginalDesiredAccess;

  if (EventContext->Operation.Create.SecurityContext.AccessState
          .SecurityDescriptorOffset > 0) {
    ioSecurityContext->AccessState.SecurityDescriptor = (PSECURITY_DESCRIPTOR)(
        (char *)&EventContext->Operation.Create.SecurityContext.AccessState +
        EventContext->Operation.Create.SecurityContext.AccessState
            .SecurityDescriptorOffset);
  } else {
    ioSecurityContext->AccessState.SecurityDescriptor = NULL;
  }

  intermediateObjName = (PDOKAN_UNICODE_STRING_INTERMEDIATE)(
      (char *)&EventContext->Operation.Create.SecurityContext.AccessState +
      EventContext->Operation.Create.SecurityContext.AccessState
          .UnicodeStringObjectNameOffset);
  intermediateObjType = (PDOKAN_UNICODE_STRING_INTERMEDIATE)(
      (char *)&EventContext->Operation.Create.SecurityContext.AccessState +
      EventContext->Operation.Create.SecurityContext.AccessState
          .UnicodeStringObjectTypeOffset);

  ioSecurityContext->AccessState.ObjectName.Length =
      intermediateObjName->Length;
  ioSecurityContext->AccessState.ObjectName.MaximumLength =
      intermediateObjName->MaximumLength;
  ioSecurityContext->AccessState.ObjectName.Buffer =
      &intermediateObjName->Buffer[0];

  ioSecurityContext->AccessState.ObjectType.Length =
      intermediateObjType->Length;
  ioSecurityContext->AccessState.ObjectType.MaximumLength =
      intermediateObjType->MaximumLength;
  ioSecurityContext->AccessState.ObjectType.Buffer =
      &intermediateObjType->Buffer[0];

  ioSecurityContext->DesiredAccess =
      EventContext->Operation.Create.SecurityContext.DesiredAccess;
}

BOOL CreateSuccesStatusCheck(NTSTATUS status, ULONG disposition) {
  if (NT_SUCCESS(status))
    return TRUE;

  // In case OPEN_ALWAYS & CREATE_ALWAYS are successfully opening an
  // existing file, STATUS_OBJECT_NAME_COLLISION is returned instead of STATUS_SUCCESS.
  if (status == STATUS_OBJECT_NAME_COLLISION &&
    (disposition == FILE_OPEN_IF || disposition == FILE_SUPERSEDE ||
      disposition == FILE_OVERWRITE_IF))
    return TRUE;

  return FALSE;
}

// Reject literal "." and ".." path components per [MS-FSA 2.1.5.1]. Win32
// canonicalizes "." and ".." before calling the kernel, so literal segments
// only reach the filesystem via verbatim "\\?\" or native NtCreateFile paths.
// Scanning handles both '\' and '/' separators and stops at the first ':'
// because anything after ':' is an alternate data stream suffix (where "."
// and ".." are valid stream names rather than directory traversal components).
static BOOL HasLiteralDotOrDotDotSegment(LPCWSTR FileName) {
  LPCWSTR p = FileName;
  while (*p != L'\0' && *p != L':') {
    while (*p == L'\\' || *p == L'/') {
      ++p;
    }
    if (*p == L'\0' || *p == L':') {
      break;
    }
    LPCWSTR segStart = p;
    while (*p != L'\0' && *p != L'\\' && *p != L'/' && *p != L':') {
      ++p;
    }
    size_t segLen = (size_t)(p - segStart);
    if ((segLen == 1 && segStart[0] == L'.') ||
        (segLen == 2 && segStart[0] == L'.' && segStart[1] == L'.')) {
      return TRUE;
    }
  }
  return FALSE;
}

VOID DispatchCreate(PDOKAN_IO_EVENT IoEvent) {
  static volatile LONG globalEventId = 0;
  ULONG currentEventId = InterlockedIncrement(&globalEventId);
  NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
  ULONG disposition;
  DWORD options;
  DOKAN_IO_SECURITY_CONTEXT ioSecurityContext;
  WCHAR *fileName;
  BOOL childExisted = TRUE;
  BOOL openTargetDirectory =
      (IoEvent->EventContext->Flags & SL_OPEN_TARGET_DIRECTORY) != 0;
  WCHAR *origFileName = NULL;
  DWORD origOptions;

  fileName = (WCHAR *)((PCHAR)&IoEvent->EventContext->Operation.Create +
                       IoEvent->EventContext->Operation.Create.FileNameOffset);

  CheckFileName(fileName);

  CreateDispatchCommon(IoEvent, 0, /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  assert(IoEvent->DokanOpenInfo == NULL);

  if (HasLiteralDotOrDotDotSegment(fileName)) {
    status = STATUS_OBJECT_NAME_INVALID;
    goto fail;
  }

  if (openTargetDirectory && wcscmp(fileName, L"\\") == 0) {
    // Per [MS-FSA 2.1.5.1.2], the volume root has no parent directory.
    status = STATUS_INVALID_PARAMETER;
    goto fail;
  }

  IoEvent->DokanOpenInfo = PopFileOpenInfo();
  if (!IoEvent->DokanOpenInfo) {
    status = STATUS_INSUFFICIENT_RESOURCES;
    goto fail;
  }
  IoEvent->DokanOpenInfo->OpenCount = 1;
  IoEvent->DokanOpenInfo->EventContext = IoEvent->EventContext;
  IoEvent->DokanOpenInfo->DokanInstance = IoEvent->DokanInstance;
  IoEvent->DokanOpenInfo->EventId = currentEventId;

  // Pass it to the driver so we can retrieve it on the next call of the same context.
  IoEvent->EventResult->Context = (ULONG64)IoEvent->DokanOpenInfo;

  // The high 8 bits of this parameter correspond to the Disposition parameter
  disposition = (IoEvent->EventContext->Operation.Create.CreateOptions >> 24) &
                0x000000ff;
  // The low 24 bits of this member correspond to the CreateOptions parameter
  options = IoEvent->EventContext->Operation.Create.CreateOptions &
            FILE_VALID_OPTION_FLAGS;

  origOptions = options;

  if (openTargetDirectory) {
    // NOTE: SL_OPEN_TARGET_DIRECTORY means open the parent directory of the
    // specified file without creating, truncating, or deleting the child or
    // parent ([MS-FSA 2.1.5.1.2] / WDK IRP_MJ_CREATE). We pull out the parent
    // directory name and switch the flags to open it as a directory.
    // https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/irp-mj-create
    origFileName = _wcsdup(fileName);

    options |= FILE_DIRECTORY_FILE;
    options &= ~(DWORD)(FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE);

    DbgPrint("SL_OPEN_TARGET_DIRECTORY specified\n");

    // Strip the last section of the file path.
    WCHAR *lastP = NULL;
    for (WCHAR *p = fileName; *p; p++) {
      if ((*p == L'\\' || *p == L'/') && p[1]) {
        lastP = p;
      }
    }

    if (lastP) {
      *lastP = L'\0';
    }

    if (!fileName[0]) {
      fileName[0] = L'\\';
      fileName[1] = L'\0';
    }
  } else if (options & FILE_DIRECTORY_FILE) {
    // To open directory (even if this flag is not specified, a directory can
    // still be opened).
    IoEvent->DokanFileInfo.IsDirectory = TRUE;
  }

  DbgPrint("###Create file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
           IoEvent->DokanOpenInfo, currentEventId, IoEvent);

  if (openTargetDirectory && origFileName == NULL) {
    status = STATUS_INSUFFICIENT_RESOURCES;
  } else if (IoEvent->DokanInstance->DokanOperations->ZwCreateFile) {
    SetIOSecurityContext(IoEvent->EventContext, &ioSecurityContext);

    if ((origOptions & FILE_NON_DIRECTORY_FILE) &&
        (origOptions & FILE_DIRECTORY_FILE)) {
      status = STATUS_INVALID_PARAMETER;
    } else {
      if (openTargetDirectory) {
        if (IoEvent->DokanInstance->DokanOperations->Cleanup &&
            IoEvent->DokanInstance->DokanOperations->CloseFile) {
          // Non-destructive child existence probe per [MS-FSA 2.1.5.1.2].
          ACCESS_MASK origDesiredAccess = ioSecurityContext.DesiredAccess;
          DWORD probeOptions = (origOptions & FILE_OPEN_REPARSE_POINT);

          ioSecurityContext.DesiredAccess = FILE_READ_ATTRIBUTES;
          IoEvent->DokanFileInfo.IsDirectory = FALSE;
          status = IoEvent->DokanInstance->DokanOperations->ZwCreateFile(
              origFileName, &ioSecurityContext, FILE_READ_ATTRIBUTES,
              /*FileAttributes=*/0,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
              probeOptions, &IoEvent->DokanFileInfo);
          ioSecurityContext.DesiredAccess = origDesiredAccess;

          if (CreateSuccesStatusCheck(status, FILE_OPEN)) {
            IoEvent->DokanInstance->DokanOperations->Cleanup(
                origFileName, &IoEvent->DokanFileInfo);
            IoEvent->DokanInstance->DokanOperations->CloseFile(
                origFileName, &IoEvent->DokanFileInfo);
          } else if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
            DbgPrint("SL_OPEN_TARGET_DIRECTORY file not found\n");
            childExisted = FALSE;
          }

          IoEvent->DokanFileInfo.Context = 0;
          IoEvent->DokanFileInfo.DeletePending = FALSE;
        }
        IoEvent->DokanFileInfo.IsDirectory = TRUE;
      }

      status = IoEvent->DokanInstance->DokanOperations->ZwCreateFile(
          fileName, &ioSecurityContext, ioSecurityContext.DesiredAccess,
          openTargetDirectory
              ? 0
              : IoEvent->EventContext->Operation.Create.FileAttributes,
          IoEvent->EventContext->Operation.Create.ShareAccess,
          openTargetDirectory ? FILE_OPEN : disposition, options,
          &IoEvent->DokanFileInfo);

      if (openTargetDirectory) {
        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
          status = STATUS_OBJECT_PATH_NOT_FOUND;
        } else if (status == STATUS_NOT_A_DIRECTORY) {
          status = STATUS_INVALID_PARAMETER;
        }
      }
    }
  } else {
    status = STATUS_NOT_IMPLEMENTED;
  }

  // save the information about this access in DOKAN_OPEN_INFO
  IoEvent->DokanOpenInfo->IsDirectory = IoEvent->DokanFileInfo.IsDirectory;
  IoEvent->DokanOpenInfo->UserContext = IoEvent->DokanFileInfo.Context;

  if (!CreateSuccesStatusCheck(status,
                               openTargetDirectory ? FILE_OPEN : disposition)) {
    if (openTargetDirectory) {
      DbgPrint("SL_OPEN_TARGET_DIRECTORY specified\n");
    }
    goto fail;
  }

  IoEvent->EventResult->Status = STATUS_SUCCESS;
  if (openTargetDirectory) {
    IoEvent->EventResult->Operation.Create.Information =
        childExisted ? FILE_EXISTS : FILE_DOES_NOT_EXIST;
  } else {
    IoEvent->EventResult->Operation.Create.Information = FILE_OPENED;

    if (disposition == FILE_CREATE || disposition == FILE_OPEN_IF ||
        disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE) {
      IoEvent->EventResult->Operation.Create.Information = FILE_CREATED;

      if (status == STATUS_OBJECT_NAME_COLLISION) {
        if (disposition == FILE_OPEN_IF) {
          IoEvent->EventResult->Operation.Create.Information = FILE_OPENED;
        } else if (disposition == FILE_OVERWRITE_IF) {
          IoEvent->EventResult->Operation.Create.Information = FILE_OVERWRITTEN;
        } else if (disposition == FILE_SUPERSEDE) {
          IoEvent->EventResult->Operation.Create.Information = FILE_SUPERSEDED;
        }
      }
    }

    if (disposition == FILE_OVERWRITE) {
      IoEvent->EventResult->Operation.Create.Information = FILE_OVERWRITTEN;
    }
  }

  if (IoEvent->DokanFileInfo.IsDirectory) {
    IoEvent->EventResult->Operation.Create.Flags |= DOKAN_FILE_DIRECTORY;
  }
  goto cleanup;

fail:
  IoEvent->EventResult->Operation.Create.Information =
      (status == STATUS_OBJECT_NAME_COLLISION) ? FILE_EXISTS
                                               : FILE_DOES_NOT_EXIST;
  IoEvent->EventResult->Status = status;
  if (IoEvent->DokanOpenInfo) {
    PushFileOpenInfo(IoEvent->DokanOpenInfo);
    IoEvent->DokanOpenInfo = NULL;
  }

cleanup:
  if (origFileName) {
    free(origFileName);
  }

  if (!NT_SUCCESS(IoEvent->EventResult->Status)) {
    IoEvent->EventResult->Context = 0;
  }

  DbgPrint("Dokan Information: DokanEndDispatchCreate() status = %lx, file "
           "handle = 0x%p, eventID = %04d, result = 0x%x\n",
           IoEvent->EventResult->Status, IoEvent->DokanOpenInfo,
           IoEvent->DokanOpenInfo ? IoEvent->DokanOpenInfo->EventId : -1,
           IoEvent->EventResult->Operation.Create.Information);
}
