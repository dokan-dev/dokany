/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

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
#pragma alloc_text (PAGE, DokanDispatchCreate)
#endif


// We must NOT call without VCB lcok
PDokanFCB
DokanAllocateFCB(
	__in PDokanVCB Vcb
	)
{
	PDokanFCB fcb = ExAllocatePool(sizeof(DokanFCB));

	if (fcb == NULL) {
		return NULL;
	}

	ASSERT(fcb != NULL);
	ASSERT(Vcb != NULL);

	RtlZeroMemory(fcb, sizeof(DokanFCB));

	fcb->Identifier.Type = FCB;
	fcb->Identifier.Size = sizeof(DokanFCB);

	fcb->Vcb = Vcb;

	ExInitializeResourceLite(&fcb->MainResource);
	ExInitializeResourceLite(&fcb->PagingIoResource);

	ExInitializeFastMutex(&fcb->AdvancedFCBHeaderMutex);

#if _WIN32_WINNT >= 0x0501
	FsRtlSetupAdvancedHeader(&fcb->AdvancedFCBHeader, &fcb->AdvancedFCBHeaderMutex);
#else
	if (DokanFsRtlTeardownPerStreamContexts) {
		FsRtlSetupAdvancedHeader(&fcb->AdvancedFCBHeader, &fcb->AdvancedFCBHeaderMutex);
	}
#endif


	fcb->AdvancedFCBHeader.ValidDataLength.LowPart = 0xffffffff;
	fcb->AdvancedFCBHeader.ValidDataLength.HighPart = 0x7fffffff;

	fcb->AdvancedFCBHeader.Resource = &fcb->MainResource;
	fcb->AdvancedFCBHeader.PagingIoResource = &fcb->PagingIoResource;

	fcb->AdvancedFCBHeader.AllocationSize.QuadPart = 4096;
	fcb->AdvancedFCBHeader.FileSize.QuadPart = 4096;

	fcb->AdvancedFCBHeader.IsFastIoPossible = FastIoIsNotPossible;

	ExInitializeResourceLite(&fcb->Resource);

	InitializeListHead(&fcb->NextCCB);
	InsertTailList(&Vcb->NextFCB, &fcb->NextFCB);

	InterlockedIncrement(&Vcb->FcbAllocated);

	return fcb;
}


PDokanFCB
DokanGetFCB(
	__in PDokanVCB	Vcb,
	__in PWCHAR		FileName,
	__in ULONG		FileNameLength)
{
	PLIST_ENTRY		thisEntry, nextEntry, listHead;
	PDokanFCB		fcb = NULL;
	ULONG			pos;

	KeEnterCriticalRegion();
	ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

	// search the FCB which is already allocated
	// (being used now)
	listHead = &Vcb->NextFCB;

    for (thisEntry = listHead->Flink;
			thisEntry != listHead;
			thisEntry = nextEntry) {

		nextEntry = thisEntry->Flink;

        fcb = CONTAINING_RECORD(thisEntry, DokanFCB, NextFCB);

		if (fcb->FileName.Length == FileNameLength) {
			// FileNameLength in bytes
			for (pos = 0; pos < FileNameLength/sizeof(WCHAR); ++pos) {
				if (fcb->FileName.Buffer[pos] != FileName[pos])
					break;
			}
			// we have the FCB which is already allocated and used
			if (pos == FileNameLength/sizeof(WCHAR))
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

	ExReleaseResourceLite(&Vcb->Resource);
	KeLeaveCriticalRegion();

	InterlockedIncrement(&fcb->FileCount);
	return fcb;
}



NTSTATUS
DokanFreeFCB(
  __in PDokanFCB Fcb
  )
{
	PDokanVCB vcb;

	ASSERT(Fcb != NULL);

	vcb = Fcb->Vcb;

	KeEnterCriticalRegion();
	ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE);
	ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

	Fcb->FileCount--;

	if (Fcb->FileCount == 0) {

		RemoveEntryList(&Fcb->NextFCB);

		DDbgPrint("  Free FCB:%X\n", Fcb);
		ExFreePool(Fcb->FileName.Buffer);

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
		ExFreePool(Fcb);

	} else {
		ExReleaseResourceLite(&Fcb->Resource);
	}

	ExReleaseResourceLite(&vcb->Resource);
	KeLeaveCriticalRegion();

	return STATUS_SUCCESS;
}



PDokanCCB
DokanAllocateCCB(
	__in PDokanDCB	Dcb,
	__in PDokanFCB	Fcb
	)
{
	PDokanCCB ccb = ExAllocatePool(sizeof(DokanCCB));

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
DokanFreeCCB(
  __in PDokanCCB ccb
  )
{
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

	ExFreePool(ccb);
	InterlockedIncrement(&fcb->Vcb->CcbFreed);

	return STATUS_SUCCESS;
}


LONG
DokanUnicodeStringChar(
	__in PUNICODE_STRING UnicodeString,
	__in WCHAR	Char)
{
	ULONG i = 0;
	for (; i < UnicodeString->Length/sizeof(WCHAR); ++i) {
		if (UnicodeString->Buffer[i] == Char) {
			return i;
		}
	}
	return -1;
}


VOID
SetFileObjectForVCB(
	__in PFILE_OBJECT	FileObject,
	__in PDokanVCB		Vcb)
{
	FileObject->SectionObjectPointer = &Vcb->SectionObjectPointers;
	FileObject->FsContext = &Vcb->VolumeFileHeader;
}



NTSTATUS
DokanDispatchCreate(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)

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
	PDokanVCB			vcb;
	PDokanDCB			dcb;
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status = STATUS_INVALID_PARAMETER;
	PFILE_OBJECT		fileObject;
	ULONG				info = 0;
	PEPROCESS			process;
	PUNICODE_STRING		processImageName;
	PEVENT_CONTEXT		eventContext;
	PFILE_OBJECT		relatedFileObject;
	ULONG				fileNameLength = 0;
	ULONG				eventLength;
	PDokanFCB			fcb;
	PDokanCCB			ccb;
	PWCHAR				fileName;
	BOOLEAN				needBackSlashAfterRelatedFile = FALSE;
	HANDLE				accessTokenHandle;

	PAGED_CODE();

	__try {
		FsRtlEnterFileSystem();

		DDbgPrint("==> DokanCreate\n");

		irpSp = IoGetCurrentIrpStackLocation(Irp);
		fileObject = irpSp->FileObject;
		relatedFileObject = fileObject->RelatedFileObject;

		if (fileObject == NULL) {
			DDbgPrint("  fileObject == NULL\n");
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
		DDbgPrint("  FileName:%wZ\n", &fileObject->FileName);

		vcb = DeviceObject->DeviceExtension;
		PrintIdType(vcb);
		if (GetIdentifierType(vcb) != VCB) {
			status = STATUS_SUCCESS;
			__leave;
		}
		dcb = vcb->Dcb;

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

		if (relatedFileObject != NULL) {
			fileObject->Vpb = relatedFileObject->Vpb;
		} else {
			fileObject->Vpb = dcb->DeviceObject->Vpb;
		}

		if ((relatedFileObject == NULL || relatedFileObject->FileName.Length == 0) &&
			fileObject->FileName.Length == 0) {

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
		   fileObject->FileName.Buffer[fileObject->FileName.Length/sizeof(WCHAR)-1] == L'\\') {
			fileObject->FileName.Length -= sizeof(WCHAR);
	   }

   		fileNameLength = fileObject->FileName.Length;
		if (relatedFileObject) {
			fileNameLength += relatedFileObject->FileName.Length;

			if (fileObject->FileName.Length > 0 &&
				fileObject->FileName.Buffer[0] == '\\') {
				DDbgPrint("  when RelatedFileObject is specified, the file name should be relative path\n");
				status = STATUS_OBJECT_NAME_INVALID;
				__leave;
			}
			if (relatedFileObject->FileName.Length > 0 &&
				relatedFileObject->FileName.Buffer[relatedFileObject->FileName.Length/sizeof(WCHAR)-1] != '\\') {
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
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}

		RtlZeroMemory(fileName, fileNameLength + sizeof(WCHAR));

		if (relatedFileObject != NULL) {
			DDbgPrint("  RelatedFileName:%wZ\n", &relatedFileObject->FileName);

			// copy the file name of related file object
			RtlCopyMemory(fileName,
							relatedFileObject->FileName.Buffer,
							relatedFileObject->FileName.Length);

			if (needBackSlashAfterRelatedFile) {
				((PWCHAR)fileName)[relatedFileObject->FileName.Length/sizeof(WCHAR)] = '\\';
			}
			// copy the file name of fileObject
			RtlCopyMemory((PCHAR)fileName +
							relatedFileObject->FileName.Length +
							(needBackSlashAfterRelatedFile? sizeof(WCHAR) : 0),
							fileObject->FileName.Buffer,
							fileObject->FileName.Length);

		} else {
			// if related file object is not specifed, copy the file name of file object
			RtlCopyMemory(fileName,
							fileObject->FileName.Buffer,
							fileObject->FileName.Length);
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
			DokanFreeFCB(fcb); // FileName is freed here
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}

		fileObject->FsContext = &fcb->AdvancedFCBHeader;
		fileObject->FsContext2 = ccb;
		fileObject->PrivateCacheMap = NULL;
		fileObject->SectionObjectPointer = &fcb->SectionObjectPointers;
		//fileObject->Flags |= FILE_NO_INTERMEDIATE_BUFFERING;

		eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
		eventContext = AllocateEventContext(vcb->Dcb, Irp, eventLength, ccb);
				
		if (eventContext == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}

		eventContext->Context = 0;
		eventContext->FileFlags |= fcb->Flags;

		// copy the file name
		eventContext->Create.FileNameLength = fcb->FileName.Length;
		RtlCopyMemory(eventContext->Create.FileName, fcb->FileName.Buffer, fcb->FileName.Length);

		eventContext->Create.FileAttributes = irpSp->Parameters.Create.FileAttributes;
		eventContext->Create.CreateOptions  = irpSp->Parameters.Create.Options;
		eventContext->Create.DesiredAccess  = irpSp->Parameters.Create.SecurityContext->DesiredAccess;
		eventContext->Create.ShareAccess    = irpSp->Parameters.Create.ShareAccess;

		// register this IRP to waiting IPR list
		status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext, 0);

	} __finally {

		if (status != STATUS_PENDING) {
			Irp->IoStatus.Status = status;
			Irp->IoStatus.Information = info;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			DokanPrintNTStatus(status);
		}

		DDbgPrint("<== DokanCreate\n");
		FsRtlExitFileSystem();
	}

	return status;
}


VOID
DokanCompleteCreate(
	 __in PIRP_ENTRY			IrpEntry,
	 __in PEVENT_INFORMATION	EventInfo
	 )
{
	PIRP				irp;
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status;
	ULONG				info;
	PDokanCCB			ccb;
	PDokanFCB			fcb;

	irp   = IrpEntry->Irp;
	irpSp = IrpEntry->IrpSp;	

	DDbgPrint("==> DokanCompleteCreate\n");

	ccb	= IrpEntry->FileObject->FsContext2;
	ASSERT(ccb != NULL);
	
	fcb = ccb->Fcb;
	ASSERT(fcb != NULL);

	DDbgPrint("  FileName:%wZ\n", &fcb->FileName);

	ccb->UserContext = EventInfo->Context;
	//DDbgPrint("   set Context %X\n", (ULONG)ccb->UserContext);

	status = EventInfo->Status;

	info = EventInfo->Create.Information;

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
	default:
		DDbgPrint("  info = %d\n", info);
		break;
	}

	ExAcquireResourceExclusiveLite(&fcb->Resource, TRUE);
	if (NT_SUCCESS(status) &&
		(irpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE ||
		EventInfo->Create.Flags & DOKAN_FILE_DIRECTORY)) {
		if (irpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE) {
			DDbgPrint("  FILE_DIRECTORY_FILE %p\n", fcb);
		} else {
			DDbgPrint("  DOKAN_FILE_DIRECTORY %p\n", fcb);
		}
		fcb->Flags |= DOKAN_FILE_DIRECTORY;
	}
	ExReleaseResourceLite(&fcb->Resource);

	ExAcquireResourceExclusiveLite(&ccb->Resource, TRUE);
	if (NT_SUCCESS(status)) {
		ccb->Flags |= DOKAN_FILE_OPENED;
	}
	ExReleaseResourceLite(&ccb->Resource);

	if (NT_SUCCESS(status)) {
		if (info == FILE_CREATED) {
			if (fcb->Flags & DOKAN_FILE_DIRECTORY) {
				DokanNotifyReportChange(fcb, FILE_NOTIFY_CHANGE_DIR_NAME, FILE_ACTION_ADDED);
			} else {
				DokanNotifyReportChange(fcb, FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED);
			}
		}
	} else {
		DDbgPrint("   IRP_MJ_CREATE failed. Free CCB:%X\n", ccb);
		DokanFreeCCB(ccb);
		DokanFreeFCB(fcb);
	}
	
	irp->IoStatus.Status = status;
	irp->IoStatus.Information = info;
	IoCompleteRequest(irp, IO_NO_INCREMENT);

	DokanPrintNTStatus(status);
	DDbgPrint("<== DokanCompleteCreate\n");
}

