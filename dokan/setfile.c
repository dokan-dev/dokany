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

#include <stdio.h>
#include <stdlib.h>
#include "dokani.h"
#include "fileinfo.h"
#include <assert.h>

void EndGenericSetOperation(DOKAN_IO_EVENT *EventInfo, NTSTATUS ResultStatus) {

	// STATUS_PENDING should not be passed to this function
	if(ResultStatus == STATUS_PENDING) {

		DbgPrint("Dokan Error: EndGenericSetOperation() failed because STATUS_PENDING was supplied for ResultStatus.\n");
		ResultStatus = STATUS_INTERNAL_ERROR;
	}

	EventInfo->EventResult->Status = ResultStatus;

	DbgPrint("\tDispatchSetInformation result =  %lx\n", ResultStatus);

	SendIoEventResult(EventInfo);
}

void DokanSetAllocationInformation(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_SET_ALLOCATION_SIZE_EVENT *allocEvent = &EventInfo->EventInfo.SetAllocationSize;
	PFILE_ALLOCATION_INFORMATION allocInfo = (PFILE_ALLOCATION_INFORMATION)(
		(PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

	assert((void*)allocEvent == (void*)EventInfo);

	// A file's allocation size and end-of-file position are independent of each
	// other,
	// with the following exception: The end-of-file position must always be less
	// than
	// or equal to the allocation size. If the allocation size is set to a value
	// that
	// is less than the end-of-file position, the end-of-file position is
	// automatically
	// adjusted to match the allocation size.

	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	if(EventInfo->DokanInstance->DokanOperations->SetAllocationSize) {

		allocEvent->DokanFileInfo = &EventInfo->DokanFileInfo;
		allocEvent->FileName = EventInfo->KernelInfo.EventContext.Operation.SetFile.FileName;
		allocEvent->Length = allocInfo->AllocationSize.QuadPart;

		status = EventInfo->DokanInstance->DokanOperations->SetAllocationSize(allocEvent);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchSetAllocationSize(allocEvent, status);
	}
}

void DOKANAPI DokanEndDispatchSetAllocationSize(DOKAN_SET_ALLOCATION_SIZE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	EndGenericSetOperation((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}

void DokanSetBasicInformation(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_SET_FILE_BASIC_INFO_EVENT *setBasicInfo = &EventInfo->EventInfo.SetFileBasicInformation;
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	assert((void*)setBasicInfo == (void*)EventInfo);

	if(EventInfo->DokanInstance->DokanOperations->SetFileBasicInformation) {

		setBasicInfo->DokanFileInfo = &EventInfo->DokanFileInfo;
		setBasicInfo->FileName = EventInfo->KernelInfo.EventContext.Operation.SetFile.FileName;
		setBasicInfo->Info = (PFILE_BASIC_INFORMATION)(
			(PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

		status = EventInfo->DokanInstance->DokanOperations->SetFileBasicInformation(setBasicInfo);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchSetFileBasicInformation(setBasicInfo, status);
	}
}

void DOKANAPI DokanEndDispatchSetFileBasicInformation(DOKAN_SET_FILE_BASIC_INFO_EVENT *EventInfo, NTSTATUS ResultStatus) {

	EndGenericSetOperation((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}

void DokanSetDispositionInformation(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_CAN_DELETE_FILE_EVENT *canDeleteFile = &EventInfo->EventInfo.CanDeleteFile;
	PFILE_DISPOSITION_INFORMATION dispositionInfo =
      (PFILE_DISPOSITION_INFORMATION)(
          (PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	assert((void*)canDeleteFile == (void*)EventInfo);

	if(!dispositionInfo->QueryDeleteFile) {
		
		DokanEndDispatchCanDeleteFile(canDeleteFile, STATUS_SUCCESS);
		return;
	}

	if(EventInfo->DokanInstance->DokanOperations->CanDeleteFile) {

		canDeleteFile->DokanFileInfo = &EventInfo->DokanFileInfo;
		canDeleteFile->FileName = EventInfo->KernelInfo.EventContext.Operation.SetFile.FileName;

		status = EventInfo->DokanInstance->DokanOperations->CanDeleteFile(canDeleteFile);
	}
  
	if(status != STATUS_PENDING) {

		DokanEndDispatchCanDeleteFile(canDeleteFile, status);
	}
}

void DOKANAPI DokanEndDispatchCanDeleteFile(DOKAN_CAN_DELETE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PFILE_DISPOSITION_INFORMATION dispositionInfo =
		(PFILE_DISPOSITION_INFORMATION)(
		(PCHAR)&ioEvent->KernelInfo.EventContext + ioEvent->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

	if(ResultStatus == STATUS_SUCCESS) {

		ioEvent->EventResult->Operation.Delete.DeleteOnClose = dispositionInfo->QueryDeleteFile ? TRUE : FALSE;
	}

	EndGenericSetOperation((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}

void DokanSetEndOfFileInformation(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_SET_EOF_EVENT *setEOF = &EventInfo->EventInfo.SetEOF;
	PFILE_END_OF_FILE_INFORMATION endInfo = (PFILE_END_OF_FILE_INFORMATION)(
      (PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetFile.BufferOffset);
	
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;
	
	assert((void*)setEOF == (void*)EventInfo);
	
	if(EventInfo->DokanInstance->DokanOperations->SetEndOfFile) {

		setEOF->DokanFileInfo = &EventInfo->DokanFileInfo;
		setEOF->FileName = EventInfo->KernelInfo.EventContext.Operation.SetFile.FileName;
		setEOF->Length = endInfo->EndOfFile.QuadPart;

		status = EventInfo->DokanInstance->DokanOperations->SetEndOfFile(setEOF);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchSetEndOfFile(setEOF, status);
	}
}

void DOKANAPI DokanEndDispatchSetEndOfFile(DOKAN_SET_EOF_EVENT *EventInfo, NTSTATUS ResultStatus) {

	EndGenericSetOperation((DOKAN_IO_EVENT*)EventInfo, ResultStatus);
}

void DokanSetRenameInformation(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_MOVE_FILE_EVENT *moveFile = &EventInfo->EventInfo.MoveFileW;
	PDOKAN_RENAME_INFORMATION renameInfo = (PDOKAN_RENAME_INFORMATION)(
      (PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	assert((void*)moveFile == (void*)EventInfo);
	
	if(EventInfo->DokanInstance->DokanOperations->MoveFileW) {
		
		moveFile->DokanFileInfo = &EventInfo->DokanFileInfo;
		moveFile->FileName = EventInfo->KernelInfo.EventContext.Operation.SetFile.FileName;
		moveFile->NewFileName = renameInfo->FileName;
		moveFile->ReplaceIfExists = renameInfo->ReplaceIfExists;

		status = EventInfo->DokanInstance->DokanOperations->MoveFileW(moveFile);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchMoveFile(moveFile, status);
	}
}

void DOKANAPI DokanEndDispatchMoveFile(DOKAN_MOVE_FILE_EVENT *EventInfo, NTSTATUS ResultStatus) {

	DOKAN_IO_EVENT *ioEvent = (DOKAN_IO_EVENT*)EventInfo;
	PDOKAN_RENAME_INFORMATION renameInfo = (PDOKAN_RENAME_INFORMATION)(
		(PCHAR)&ioEvent->KernelInfo.EventContext + ioEvent->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

	if(ResultStatus == STATUS_SUCCESS) {

		ioEvent->EventResult->BufferLength = renameInfo->FileNameLength;

		CopyMemory(ioEvent->EventResult->Buffer, renameInfo->FileName, renameInfo->FileNameLength);
	}

	EndGenericSetOperation(ioEvent, ResultStatus);
}

void DokanSetValidDataLengthInformation(DOKAN_IO_EVENT *EventInfo) {

	DOKAN_SET_EOF_EVENT *setEOF = &EventInfo->EventInfo.SetEOF;
	PFILE_VALID_DATA_LENGTH_INFORMATION validInfo =
		(PFILE_VALID_DATA_LENGTH_INFORMATION)(
		(PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

	NTSTATUS status = STATUS_NOT_IMPLEMENTED;

	assert((void*)setEOF == (void*)EventInfo);

	if(EventInfo->DokanInstance->DokanOperations->SetEndOfFile) {

		setEOF->DokanFileInfo = &EventInfo->DokanFileInfo;
		setEOF->FileName = EventInfo->KernelInfo.EventContext.Operation.SetFile.FileName;
		setEOF->Length = validInfo->ValidDataLength.QuadPart;

		status = EventInfo->DokanInstance->DokanOperations->SetEndOfFile(setEOF);
	}

	if(status != STATUS_PENDING) {

		DokanEndDispatchSetEndOfFile(setEOF, status);
	}
}

void BeginDispatchSetInformation(DOKAN_IO_EVENT *EventInfo) {

  ULONG sizeOfEventInfo = 0;

  DbgPrint("###SetFileInfo file handle = 0x%p, eventID = %04d, FileInformationClass = %d, event Info = 0x%p\n",
	  EventInfo->DokanOpenInfo,
	  EventInfo->DokanOpenInfo != NULL ? EventInfo->DokanOpenInfo->EventId : -1,
	  EventInfo->KernelInfo.EventContext.Operation.SetFile.FileInformationClass,
	  EventInfo);

  assert(EventInfo->ProcessingContext == NULL);
  assert(EventInfo->DokanOpenInfo);

  if (EventInfo->KernelInfo.EventContext.Operation.SetFile.FileInformationClass == FileRenameInformation) {

    PDOKAN_RENAME_INFORMATION renameInfo = (PDOKAN_RENAME_INFORMATION)(
        (PCHAR)&EventInfo->KernelInfo.EventContext + EventInfo->KernelInfo.EventContext.Operation.SetFile.BufferOffset);

    sizeOfEventInfo += renameInfo->FileNameLength;
  }

  CheckFileName(EventInfo->KernelInfo.EventContext.Operation.SetFile.FileName);

  CreateDispatchCommon(EventInfo, sizeOfEventInfo);

  switch (EventInfo->KernelInfo.EventContext.Operation.SetFile.FileInformationClass) {

  case FileAllocationInformation:
    DokanSetAllocationInformation(EventInfo);
    break;

  case FileBasicInformation:
    DokanSetBasicInformation(EventInfo);
    break;

  case FileDispositionInformation:
    DokanSetDispositionInformation(EventInfo);
    break;

  case FileEndOfFileInformation:
    DokanSetEndOfFileInformation(EventInfo);
    break;

  case FileLinkInformation:
	EndGenericSetOperation(EventInfo, STATUS_NOT_IMPLEMENTED);
    break;

  case FilePositionInformation:
    // this case is dealed with by driver
	EndGenericSetOperation(EventInfo, STATUS_NOT_IMPLEMENTED);
    break;

  case FileRenameInformation:
    DokanSetRenameInformation(EventInfo);
    break;

  case FileValidDataLengthInformation:
    DokanSetValidDataLengthInformation(EventInfo);
    break;

  default:
	  DbgPrint("Dokan Warning: Unrecognized EventInfo->KernelInfo.EventContext.Operation.SetFile.FileInformationClass: 0x%x\n",
		  EventInfo->KernelInfo.EventContext.Operation.SetFile.FileInformationClass);

	  EndGenericSetOperation(EventInfo, STATUS_NOT_IMPLEMENTED);

	  break;
  }
}
