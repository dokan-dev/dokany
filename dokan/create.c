/*
  Dokan : user-mode file system library for Windows

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

VOID DispatchCreate(PDOKAN_IO_EVENT IoEvent) {
  static volatile LONG globalEventId = 0;
  ULONG currentEventId = InterlockedIncrement(&globalEventId);
  NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
  ULONG disposition;
  DWORD options;
  DOKAN_IO_SECURITY_CONTEXT ioSecurityContext;
  WCHAR *fileName;
  BOOL childExisted = TRUE;
  WCHAR *origFileName = NULL;
  DWORD origOptions;

  fileName = (WCHAR *)((PCHAR)&IoEvent->EventContext->Operation.Create +
                       IoEvent->EventContext->Operation.Create.FileNameOffset);

  CheckFileName(fileName);

  CreateDispatchCommon(IoEvent, 0, /*UseExtraMemoryPool=*/FALSE,
                       /*ClearNonPoolBuffer=*/TRUE);

  assert(IoEvent->DokanOpenInfo == NULL);

  IoEvent->DokanOpenInfo = PopFileOpenInfo();
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

  // to open directory
  // even if this flag is not specified,
  // there is a case to open a directory
  if (options & FILE_DIRECTORY_FILE) {
    // DbgPrint("FILE_DIRECTORY_FILE\n");
    IoEvent->DokanFileInfo.IsDirectory = TRUE;
  } else if (IoEvent->EventContext->Flags & SL_OPEN_TARGET_DIRECTORY) {
    // NOTE: SL_OPEN_TARGET_DIRECTORY means open the parent directory of the
    // specified file
    // We pull out the parent directory name and then switch the flags to make
    // it look like it was
    // a regular request to open a directory.
    // https://msdn.microsoft.com/en-us/library/windows/hardware/ff548630(v=vs.85).aspx

    origFileName = _wcsdup(fileName);

    options |= FILE_DIRECTORY_FILE;
    options &= ~FILE_NON_DIRECTORY_FILE;

    DbgPrint("SL_OPEN_TARGET_DIRECTORY specified\n");

    // strip the last section of the file path
    WCHAR *lastP = NULL;

    for (WCHAR *p = fileName; *p; p++) {
      if ((*p == L'\\' || *p == L'/') && p[1])
        lastP = p;
    }

    if (lastP) {
      *lastP = 0;
    }

    if (!fileName[0]) {
      fileName[0] = '\\';
      fileName[1] = 0;
    }
  }

  DbgPrint("###Create file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
           IoEvent->DokanOpenInfo, currentEventId, IoEvent);

  if (IoEvent->DokanInstance->DokanOperations->ZwCreateFile) {

    SetIOSecurityContext(IoEvent->EventContext, &ioSecurityContext);

    if ((IoEvent->EventContext->Flags & SL_OPEN_TARGET_DIRECTORY) &&
        IoEvent->DokanInstance->DokanOperations->Cleanup &&
        IoEvent->DokanInstance->DokanOperations->CloseFile) {

      if (options & FILE_NON_DIRECTORY_FILE && options & FILE_DIRECTORY_FILE)
        status = STATUS_INVALID_PARAMETER;
      else
        status = IoEvent->DokanInstance->DokanOperations->ZwCreateFile(
            origFileName, &ioSecurityContext, ioSecurityContext.DesiredAccess,
            IoEvent->EventContext->Operation.Create.FileAttributes,
            IoEvent->EventContext->Operation.Create.ShareAccess, disposition,
            origOptions, &IoEvent->DokanFileInfo);

      if (CreateSuccesStatusCheck(status, disposition)) {
        IoEvent->DokanInstance->DokanOperations->Cleanup(origFileName, &IoEvent->DokanFileInfo);
        IoEvent->DokanInstance->DokanOperations->CloseFile(
            origFileName, &IoEvent->DokanFileInfo);
      } else if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        DbgPrint("SL_OPEN_TARGET_DIRECTORY file not found\n");
        childExisted = FALSE;
      }

      IoEvent->DokanFileInfo.IsDirectory = TRUE;
    }

    if (options & FILE_NON_DIRECTORY_FILE && options & FILE_DIRECTORY_FILE)
      status = STATUS_INVALID_PARAMETER;
    else
      status = IoEvent->DokanInstance->DokanOperations->ZwCreateFile(
          fileName, &ioSecurityContext, ioSecurityContext.DesiredAccess,
          IoEvent->EventContext->Operation.Create.FileAttributes,
          IoEvent->EventContext->Operation.Create.ShareAccess, disposition,
          options, &IoEvent->DokanFileInfo);

    if (CreateSuccesStatusCheck(status, disposition)
      && !childExisted) {
      IoEvent->EventResult->Operation.Create.Information = FILE_DOES_NOT_EXIST;
    }
  } else {
    status = STATUS_NOT_IMPLEMENTED;
  }

  // save the information about this access in DOKAN_OPEN_INFO
  IoEvent->DokanOpenInfo->IsDirectory = IoEvent->DokanFileInfo.IsDirectory;
  IoEvent->DokanOpenInfo->UserContext = IoEvent->DokanFileInfo.Context;

  if (!CreateSuccesStatusCheck(status, disposition)) {
    if (IoEvent->EventContext->Flags & SL_OPEN_TARGET_DIRECTORY) {
      DbgPrint("SL_OPEN_TARGET_DIRECTORY specified\n");
    }
    IoEvent->EventResult->Operation.Create.Information = FILE_DOES_NOT_EXIST;
    IoEvent->EventResult->Status = status;

    if (status == STATUS_OBJECT_NAME_COLLISION) {
      IoEvent->EventResult->Operation.Create.Information = FILE_EXISTS;
    }

    if (STATUS_ACCESS_DENIED == status &&
        IoEvent->DokanInstance->DokanOperations->ZwCreateFile &&
        (IoEvent->EventContext->Operation.Create.SecurityContext.DesiredAccess &
         DELETE)) {
      DbgPrint("Delete failed, ask parent folder if we have the right\n");
      // strip the last section of the file path
      WCHAR *lastP = NULL;
      for (WCHAR *p = fileName; *p; p++) {
        if ((*p == L'\\' || *p == L'/') && p[1])
          lastP = p;
      }
      if (lastP) {
        *lastP = 0;
      }

      SetIOSecurityContext(IoEvent->EventContext, &ioSecurityContext);
      ACCESS_MASK newDesiredAccess =
          (MAXIMUM_ALLOWED & ioSecurityContext.DesiredAccess)
              ? (FILE_DELETE_CHILD | FILE_LIST_DIRECTORY)
              : (((DELETE & ioSecurityContext.DesiredAccess) ? FILE_DELETE_CHILD
                                                             : 0) |
                 ((FILE_READ_ATTRIBUTES & ioSecurityContext.DesiredAccess)
                      ? FILE_LIST_DIRECTORY
                      : 0));

      options |= FILE_OPEN_FOR_BACKUP_INTENT; //Enable open directory
      options &= ~FILE_NON_DIRECTORY_FILE;    //Remove non dir flag

      status = IoEvent->DokanInstance->DokanOperations->ZwCreateFile(
          fileName, &ioSecurityContext, newDesiredAccess,
          IoEvent->EventContext->Operation.Create.FileAttributes,
          IoEvent->EventContext->Operation.Create.ShareAccess, disposition,
          options, &IoEvent->DokanFileInfo);

      if (status == STATUS_SUCCESS) {
        DbgPrint("Parent give us the right to delete\n");
        IoEvent->EventResult->Status = STATUS_SUCCESS;
        IoEvent->EventResult->Operation.Create.Information = FILE_OPENED;
      } else {
        DbgPrint("Parent CreateFile failed status = %lx\n", status);
        PushFileOpenInfo(IoEvent->DokanOpenInfo);
        IoEvent->DokanOpenInfo = NULL;
      }
    } else {
      PushFileOpenInfo(IoEvent->DokanOpenInfo);
      IoEvent->DokanOpenInfo = NULL;
    }

  } else {

    IoEvent->EventResult->Status = STATUS_SUCCESS;
    IoEvent->EventResult->Operation.Create.Information = FILE_OPENED;

    if (disposition == FILE_CREATE || disposition == FILE_OPEN_IF ||
        disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE) {
      IoEvent->EventResult->Operation.Create.Information = FILE_CREATED;

      if (status == STATUS_OBJECT_NAME_COLLISION) {
        if (disposition == FILE_OPEN_IF) {
          IoEvent->EventResult->Operation.Create.Information = FILE_OPENED;
        } else if (disposition == FILE_OVERWRITE_IF) {
          IoEvent->EventResult->Operation.Create.Information =
              FILE_OVERWRITTEN;
        } else if (disposition == FILE_SUPERSEDE) {
          IoEvent->EventResult->Operation.Create.Information = FILE_SUPERSEDED;
        }
      }
    }

    if (disposition == FILE_OVERWRITE)
      IoEvent->EventResult->Operation.Create.Information = FILE_OVERWRITTEN;

    if (IoEvent->DokanFileInfo.IsDirectory)
      IoEvent->EventResult->Operation.Create.Flags |= DOKAN_FILE_DIRECTORY;
  }

  if (origFileName)
    free(origFileName);

  if (!NT_SUCCESS(IoEvent->EventResult->Status)) {
    IoEvent->EventResult->Context = 0;
  }

  DbgPrint("Dokan Information: DokanEndDispatchCreate() status = %lx, file "
           "handle = 0x%p, eventID = %04d, result = 0x%x\n",
           IoEvent->EventResult->Status, IoEvent->DokanOpenInfo,
           IoEvent->DokanOpenInfo ? IoEvent->DokanOpenInfo->EventId : -1,
           IoEvent->EventResult->Operation.Create.Information);
}
