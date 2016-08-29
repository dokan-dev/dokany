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

#include "dokani.h"
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

void BeginDispatchCreate(DOKAN_IO_EVENT *EventInfo) {

  static volatile LONG globalEventId = 0;

  PDOKAN_INSTANCE dokan = EventInfo->DokanInstance;
  DOKAN_CREATE_FILE_EVENT *createFileEvent = &EventInfo->EventInfo.ZwCreateFile;
  NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
  ULONG currentEventId = InterlockedIncrement(&globalEventId);
  WCHAR *fileName = (WCHAR *)((char *)&EventInfo->KernelInfo.EventContext.Operation.Create +
	  EventInfo->KernelInfo.EventContext.Operation.Create.FileNameOffset);

  assert((void*)createFileEvent == (void*)EventInfo);

  DbgPrint("###Create file handle = 0x%p, eventID = %04d, event Info = 0x%p\n",
	  EventInfo->DokanOpenInfo,
	  currentEventId,
	  EventInfo);

  // The low 24 bits of this member correspond to the CreateOptions parameter
  createFileEvent->CreateOptions = EventInfo->KernelInfo.EventContext.Operation.Create.CreateOptions & FILE_VALID_OPTION_FLAGS;
  // DbgPrint("Create.CreateOptions 0x%x\n", options);

  // The high 8 bits of this parameter correspond to the Disposition parameter
  createFileEvent->CreateDisposition = (EventInfo->KernelInfo.EventContext.Operation.Create.CreateOptions >> 24) & 0x000000ff;
  createFileEvent->FileAttributes = EventInfo->KernelInfo.EventContext.Operation.Create.FileAttributes;
  createFileEvent->ShareAccess = EventInfo->KernelInfo.EventContext.Operation.Create.ShareAccess;

  if((createFileEvent->CreateOptions & FILE_NON_DIRECTORY_FILE) && (createFileEvent->CreateOptions & FILE_DIRECTORY_FILE)) {
	  
	  DokanEndDispatchCreate(createFileEvent, STATUS_INVALID_PARAMETER);

	  return;
  }

  createFileEvent->FileName = fileName;

  CheckFileName(fileName);

  assert(createFileEvent->OriginalFileName == NULL);

  createFileEvent->OriginalFileName = DokanDupW(createFileEvent->FileName);

  if (EventInfo->KernelInfo.EventContext.Flags & SL_OPEN_TARGET_DIRECTORY) {
    // NOTE: SL_OPEN_TARGET_DIRECTORY means open the parent directory of the
    // specified file
    // We pull out the parent directory name and then switch the flags to make
    // it look like it was
    // a regular request to open a directory.
    // https://msdn.microsoft.com/en-us/library/windows/hardware/ff548630(v=vs.85).aspx

	createFileEvent->CreateOptions |= FILE_DIRECTORY_FILE;
	createFileEvent->CreateOptions &= ~FILE_NON_DIRECTORY_FILE;

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
		((WCHAR*)fileName)[0] = L'\\';
		((WCHAR*)fileName)[1] = 0;
    }
  }

  assert(EventInfo->DokanOpenInfo == NULL);

  EventInfo->DokanOpenInfo = PopFileOpenInfo();
  EventInfo->DokanOpenInfo->DokanInstance = dokan;
  EventInfo->DokanOpenInfo->EventId = currentEventId;

  createFileEvent->DokanFileInfo = &EventInfo->DokanFileInfo;

  assert(EventInfo->DokanFileInfo.IsDirectory == FALSE && EventInfo->DokanOpenInfo->IsDirectory == FALSE);

  // Even if this flag is not specifed there can be reasons to open a directory
  // so this flag is ultimately up to the user mode driver.
  if(createFileEvent->CreateOptions & FILE_DIRECTORY_FILE) {

	  EventInfo->DokanOpenInfo->IsDirectory = TRUE;
	  EventInfo->DokanFileInfo.IsDirectory = TRUE;
  }

  if (dokan->DokanOperations->ZwCreateFile) {

    SetIOSecurityContext(&EventInfo->KernelInfo.EventContext, &createFileEvent->SecurityContext);

	createFileEvent->DesiredAccess = createFileEvent->SecurityContext.DesiredAccess;

    // Call SetLastError() to reset the error code to a known state
    // so we can check whether or not the user-mode driver set
    // ERROR_ALREADY_EXISTS
    SetLastError(ERROR_SUCCESS);

      // This should call SetLastError(ERROR_ALREADY_EXISTS) when appropriate
	status = dokan->DokanOperations->ZwCreateFile(createFileEvent);

  }
  else {

    status = STATUS_NOT_IMPLEMENTED;
  }

  if(status != STATUS_PENDING) {

	  DokanEndDispatchCreate(createFileEvent, status);
  }
}

BOOL CreateSuccesStatusCheck(NTSTATUS status, ULONG disposition) {

	return status == STATUS_SUCCESS
		|| (status == STATUS_OBJECT_NAME_COLLISION
			&& (disposition == FILE_OPEN_IF
				|| disposition == FILE_SUPERSEDE
				|| disposition == FILE_OVERWRITE_IF));
}

void DOKANAPI DokanEndDispatchCreate(DOKAN_CREATE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	DOKAN_CLEANUP_EVENT cleanupEvent;
	DOKAN_CLOSE_FILE_EVENT closeFileEvent;

	assert(ioEvent->DokanInstance);

	CreateDispatchCommon(ioEvent, 0);

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: DokanEndDispatchCreate() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	ioEvent->EventResult->Status = ResultStatus;

	if(ioEvent->EventInfo.ZwCreateFile.OriginalFileName) {

		DokanFree((void*)ioEvent->EventInfo.ZwCreateFile.OriginalFileName);
		ioEvent->EventInfo.ZwCreateFile.OriginalFileName = NULL;
	}

	DbgPrint("Dokan Information: DokanEndDispatchCreate() status = %lx, file handle = 0x%p, eventID = %04d\n",
		ResultStatus, ioEvent->DokanOpenInfo, ioEvent->DokanOpenInfo ? ioEvent->DokanOpenInfo->EventId : -1);

	// FILE_CREATED
	// FILE_DOES_NOT_EXIST
	// FILE_EXISTS
	// FILE_OPENED
	// FILE_OVERWRITTEN
	// FILE_SUPERSEDED

	if(!CreateSuccesStatusCheck(ResultStatus, EventInfo->CreateDisposition)) {

		if(ioEvent->KernelInfo.EventContext.Flags & SL_OPEN_TARGET_DIRECTORY) {

			DbgPrint("SL_OPEN_TARGET_DIRECTORY specified\n");
		}

		if(ioEvent->DokanOpenInfo) {

			if(ioEvent->DokanFileInfo.Context) {
				
				if(ioEvent->DokanInstance->DokanOperations->Cleanup) {

					cleanupEvent.DokanFileInfo = &ioEvent->DokanFileInfo;
					cleanupEvent.FileName = EventInfo->FileName;

					ioEvent->DokanInstance->DokanOperations->Cleanup(&cleanupEvent);
				}

				if(ioEvent->DokanInstance->DokanOperations->CloseFile) {

					closeFileEvent.DokanFileInfo = &ioEvent->DokanFileInfo;
					closeFileEvent.FileName = EventInfo->FileName;

					ioEvent->DokanInstance->DokanOperations->CloseFile(&closeFileEvent);
				}
			}

			PushFileOpenInfo(ioEvent->DokanOpenInfo);
			ioEvent->DokanOpenInfo = NULL;
		}

		ioEvent->EventResult->Operation.Create.Information = FILE_DOES_NOT_EXIST;
		ioEvent->EventResult->Status = ResultStatus;

		if(ResultStatus == STATUS_OBJECT_NAME_COLLISION) {

			ioEvent->EventResult->Operation.Create.Information = FILE_EXISTS;
		}
	}
	else {

		assert(ioEvent->DokanOpenInfo);

		// Pass the file handle to the driver so we can use the same handle for
		// future IO operations.
		ioEvent->DokanOpenInfo->IsDirectory = ioEvent->DokanFileInfo.IsDirectory;

		ioEvent->EventResult->Context = (ULONG64)ioEvent->DokanOpenInfo;
		ioEvent->EventResult->Operation.Create.Information = FILE_OPENED;

		if(EventInfo->CreateDisposition == FILE_CREATE
			|| EventInfo->CreateDisposition == FILE_OPEN_IF
			|| EventInfo->CreateDisposition == FILE_OVERWRITE_IF) {

			ioEvent->EventResult->Operation.Create.Information = FILE_CREATED;

			if(ResultStatus == STATUS_OBJECT_NAME_COLLISION) {

				if(EventInfo->CreateDisposition == FILE_OPEN_IF) {

					ioEvent->EventResult->Operation.Create.Information = FILE_OPENED;
				}
				else if(EventInfo->CreateDisposition == FILE_OVERWRITE_IF
					|| EventInfo->CreateDisposition == FILE_SUPERSEDE) {

					ioEvent->EventResult->Operation.Create.Information = FILE_OVERWRITTEN;
				}
			}
		}

		if(ioEvent->DokanFileInfo.IsDirectory) {

			ioEvent->EventResult->Operation.Create.Flags |= DOKAN_FILE_DIRECTORY;
		}
	}

	SendIoEventResult(ioEvent);
}