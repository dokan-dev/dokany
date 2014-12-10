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

/*++


--*/

#ifndef _DOKAN_H_
#define _DOKAN_H_


#include <ntifs.h>
#include <ntdddisk.h>
#include <ntstrsafe.h>

#include "public.h"

//
// DEFINES
//

#define DOKAN_DEBUG_DEFAULT 0

extern ULONG g_Debug;

#define DOKAN_GLOBAL_DEVICE_NAME			L"\\Device\\Dokan"
#define DOKAN_GLOBAL_SYMBOLIC_LINK_NAME		L"\\DosDevices\\Global\\Dokan"

#define DOKAN_FS_DEVICE_NAME		L"\\Device\\Dokan"
#define DOKAN_DISK_DEVICE_NAME		L"\\Device\\Volume"
#define DOKAN_SYMBOLIC_LINK_NAME    L"\\DosDevices\\Global\\Volume"

#define DOKAN_NET_DEVICE_NAME			L"\\Device\\DokanRedirector"
#define DOKAN_NET_SYMBOLIC_LINK_NAME    L"\\DosDevices\\Global\\DokanRedirector"

#define VOLUME_LABEL			L"DOKAN"
								// {D6CC17C5-1734-4085-BCE7-964F1E9F5DE9}
#define DOKAN_BASE_GUID			{0xd6cc17c5, 0x1734, 0x4085, {0xbc, 0xe7, 0x96, 0x4f, 0x1e, 0x9f, 0x5d, 0xe9}}

#define TAG (ULONG)'AKOD'

#define DOKAN_MDL_ALLOCATED		0x1


#ifdef ExAllocatePool
#undef ExAllocatePool
#endif
#define ExAllocatePool(size)	ExAllocatePoolWithTag(NonPagedPool, size, TAG)

#define DRIVER_CONTEXT_EVENT		2
#define DRIVER_CONTEXT_IRP_ENTRY	3

#define DOKAN_IRP_PENDING_TIMEOUT	(1000 * 15) // in millisecond
#define DOKAN_IRP_PENDING_TIMEOUT_RESET_MAX (1000 * 60 * 5) // in millisecond
#define DOKAN_CHECK_INTERVAL		(1000 * 5) // in millisecond

#define DOKAN_KEEPALIVE_TIMEOUT		(1000 * 15) // in millisecond

#if _WIN32_WINNT > 0x501
	#define DDbgPrint(...) \
	if (g_Debug) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "[DokanFS] " __VA_ARGS__ )); }
#else
	#define DDbgPrint(...) \
		if (g_Debug) { DbgPrint("[DokanFS] " __VA_ARGS__); }
#endif

#if _WIN32_WINNT < 0x0501
	extern PFN_FSRTLTEARDOWNPERSTREAMCONTEXTS DokanFsRtlTeardownPerStreamContexts;
#endif

extern UNICODE_STRING	FcbFileNameNull;
#define DokanPrintFileName(FileObject) \
	DDbgPrint("  FileName: %wZ FCB.FileName: %wZ\n", \
		&FileObject->FileName, \
		FileObject->FsContext2 ? \
			(((PDokanCCB)FileObject->FsContext2)->Fcb ? \
				&((PDokanCCB)FileObject->FsContext2)->Fcb->FileName : &FcbFileNameNull) : \
			&FcbFileNameNull)


	
extern NPAGED_LOOKASIDE_LIST	DokanIrpEntryLookasideList;
#define DokanAllocateIrpEntry()		ExAllocateFromNPagedLookasideList(&DokanIrpEntryLookasideList)
#define DokanFreeIrpEntry(IrpEntry)	ExFreeToNPagedLookasideList(&DokanIrpEntryLookasideList, IrpEntry)

	
//
// FSD_IDENTIFIER_TYPE
//
// Identifiers used to mark the structures
//
typedef enum _FSD_IDENTIFIER_TYPE {
	DGL = ':DGL', // Dokan Global
    DCB = ':DCB', // Disk Control Block
    VCB = ':VCB', // Volume Control Block
    FCB = ':FCB', // File Control Block
    CCB = ':CCB', // Context Control Block
} FSD_IDENTIFIER_TYPE;

//
// FSD_IDENTIFIER
//
// Header put in the beginning of every structure
//
typedef struct _FSD_IDENTIFIER {
    FSD_IDENTIFIER_TYPE     Type;
    ULONG                   Size;
} FSD_IDENTIFIER, *PFSD_IDENTIFIER;


#define GetIdentifierType(Obj) (((PFSD_IDENTIFIER)Obj)->Type)


//
// DATA
//


typedef struct _IRP_LIST {
	LIST_ENTRY		ListHead;
	KEVENT			NotEmpty;
	KSPIN_LOCK		ListLock;
} IRP_LIST, *PIRP_LIST;


typedef struct _DOKAN_GLOBAL {
	FSD_IDENTIFIER	Identifier;
	ERESOURCE		Resource;
	PDEVICE_OBJECT	DeviceObject;
	ULONG			MountId;
	// the list of waiting IRP for mount service
	IRP_LIST		PendingService;
	IRP_LIST		NotifyService;

} DOKAN_GLOBAL, *PDOKAN_GLOBAL;


// make sure Identifier is the top of struct
typedef struct _DokanDiskControlBlock {

	FSD_IDENTIFIER			Identifier;

	ERESOURCE				Resource;

	PDOKAN_GLOBAL			Global;
	PDRIVER_OBJECT			DriverObject;
	PDEVICE_OBJECT			DeviceObject;
	
	PVOID					Vcb;

	// the list of waiting Event
	IRP_LIST				PendingIrp;
	IRP_LIST				PendingEvent;
	IRP_LIST				NotifyEvent;

	PUNICODE_STRING			DiskDeviceName;
	PUNICODE_STRING			FileSystemDeviceName;
	PUNICODE_STRING			SymbolicLinkName;

	DEVICE_TYPE				DeviceType;
	ULONG					DeviceCharacteristics;
	HANDLE					MupHandle;
	UNICODE_STRING			MountedDeviceInterfaceName;
	UNICODE_STRING			DiskDeviceInterfaceName;

	// When timeout is occuerd, KillEvent is triggered.
	KEVENT					KillEvent;

	KEVENT					ReleaseEvent;

	// the thread to deal with timeout
	PKTHREAD				TimeoutThread;
	PKTHREAD				EventNotificationThread;

	// When UseAltStream is 1, use Alternate stream
	USHORT					UseAltStream;
	USHORT					UseKeepAlive;
	USHORT					Mounted;

	// to make a unique id for pending IRP
	ULONG					SerialNumber;

	ULONG					MountId;

	LARGE_INTEGER			TickCount;

	CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    CACHE_MANAGER_CALLBACKS CacheManagerNoOpCallbacks;
} DokanDCB, *PDokanDCB;


typedef struct _DokanVolumeControlBlock {

	FSD_IDENTIFIER				Identifier;

	FSRTL_ADVANCED_FCB_HEADER	VolumeFileHeader;
	SECTION_OBJECT_POINTERS		SectionObjectPointers;
	FAST_MUTEX					AdvancedFCBHeaderMutex;

	ERESOURCE					Resource;
	PDEVICE_OBJECT				DeviceObject;
	PDokanDCB					Dcb;
	LIST_ENTRY					NextFCB;

	// NotifySync is used by notify directory change
    PNOTIFY_SYNC				NotifySync;
    LIST_ENTRY					DirNotifyList;

	ULONG						FcbAllocated;
	ULONG						FcbFreed;
	ULONG						CcbAllocated;
	ULONG						CcbFreed;

} DokanVCB, *PDokanVCB;


typedef struct _DokanFileControlBlock
{
	FSD_IDENTIFIER				Identifier;

	FSRTL_ADVANCED_FCB_HEADER	AdvancedFCBHeader;
	SECTION_OBJECT_POINTERS		SectionObjectPointers;
	
	FAST_MUTEX				AdvancedFCBHeaderMutex;

	ERESOURCE				MainResource;
	ERESOURCE				PagingIoResource;
	
	PDokanVCB				Vcb;
	LIST_ENTRY				NextFCB;
	ERESOURCE				Resource;
	LIST_ENTRY				NextCCB;

	ULONG					FileCount;

	ULONG					Flags;

	UNICODE_STRING			FileName;

	//uint32 ReferenceCount;
	//uint32 OpenHandleCount;
} DokanFCB, *PDokanFCB;



typedef struct _DokanContextControlBlock
{
	FSD_IDENTIFIER		Identifier;
	ERESOURCE			Resource;
	PDokanFCB			Fcb;
	LIST_ENTRY			NextCCB;
	ULONG64				Context;
	ULONG64				UserContext;
	
	PWCHAR				SearchPattern;
	ULONG				SearchPatternLength;

	ULONG				Flags;

	int					FileCount;
	ULONG				MountId;
} DokanCCB, *PDokanCCB;


// IRP list which has pending status
// this structure is also used to store event notification IRP
typedef struct _IRP_ENTRY {
	LIST_ENTRY			ListEntry;
	ULONG				SerialNumber;
	PIRP				Irp;
	PIO_STACK_LOCATION	IrpSp;
	PFILE_OBJECT		FileObject;
	BOOLEAN				CancelRoutineFreeMemory;
	ULONG				Flags;
	LARGE_INTEGER		TickCount;
	PIRP_LIST			IrpList;
} IRP_ENTRY, *PIRP_ENTRY;


typedef struct _DRIVER_EVENT_CONTEXT {
	LIST_ENTRY		ListEntry;
	PKEVENT			Completed;
	EVENT_CONTEXT	EventContext;
} DRIVER_EVENT_CONTEXT, *PDRIVER_EVENT_CONTEXT;


DRIVER_INITIALIZE DriverEntry;

__drv_dispatchType(IRP_MJ_CREATE)	DRIVER_DISPATCH DokanDispatchCreate;
__drv_dispatchType(IRP_MJ_CLOSE)	DRIVER_DISPATCH DokanDispatchClose;
__drv_dispatchType(IRP_MJ_READ)		DRIVER_DISPATCH DokanDispatchRead;
__drv_dispatchType(IRP_MJ_WRITE)	DRIVER_DISPATCH DokanDispatchWrite;
__drv_dispatchType(IRP_MJ_FLUSH_BUFFERS)	DRIVER_DISPATCH DokanDispatchFlush;
__drv_dispatchType(IRP_MJ_CLEANUP)			DRIVER_DISPATCH DokanDispatchCleanup;
__drv_dispatchType(IRP_MJ_DEVICE_CONTROL)		DRIVER_DISPATCH DokanDispatchDeviceControl;
__drv_dispatchType(IRP_MJ_FILE_SYSTEM_CONTROL)	DRIVER_DISPATCH DokanDispatchFileSystemControl;
__drv_dispatchType(IRP_MJ_DIRECTORY_CONTROL)	DRIVER_DISPATCH DokanDispatchDirectoryControl;
__drv_dispatchType(IRP_MJ_QUERY_INFORMATION)	DRIVER_DISPATCH DokanDispatchQueryInformation;
__drv_dispatchType(IRP_MJ_SET_INFORMATION)		DRIVER_DISPATCH DokanDispatchSetInformation;
__drv_dispatchType(IRP_MJ_QUERY_VOLUME_INFORMATION)	DRIVER_DISPATCH DokanDispatchQueryVolumeInformation;
__drv_dispatchType(IRP_MJ_SET_VOLUME_INFORMATION)	DRIVER_DISPATCH DokanDispatchSetVolumeInformation;
__drv_dispatchType(IRP_MJ_SHUTDOWN)		DRIVER_DISPATCH DokanDispatchShutdown;
__drv_dispatchType(IRP_MJ_PNP)			DRIVER_DISPATCH DokanDispatchPnp;
__drv_dispatchType(IRP_MJ_LOCK_CONTROL)	DRIVER_DISPATCH DokanDispatchLock;
__drv_dispatchType(IRP_MJ_QUERY_SECURITY)	DRIVER_DISPATCH DokanDispatchQuerySecurity;
__drv_dispatchType(IRP_MJ_SET_SECURITY)		DRIVER_DISPATCH DokanDispatchSetSecurity;

DRIVER_UNLOAD DokanUnload;



DRIVER_CANCEL DokanEventCancelRoutine;

DRIVER_CANCEL DokanIrpCancelRoutine;

DRIVER_DISPATCH DokanRegisterPendingIrpForEvent;

DRIVER_DISPATCH DokanRegisterPendingIrpForService;

DRIVER_DISPATCH DokanCompleteIrp;

DRIVER_DISPATCH DokanResetPendingIrpTimeout;

DRIVER_DISPATCH DokanGetAccessToken;

NTSTATUS
DokanEventRelease(
	__in PDEVICE_OBJECT DeviceObject);


DRIVER_DISPATCH DokanEventStart;

DRIVER_DISPATCH DokanEventWrite;


PEVENT_CONTEXT
AllocateEventContextRaw(
	__in ULONG	EventContextLength
	);

PEVENT_CONTEXT
AllocateEventContext(
	__in PDokanDCB	Dcb,
	__in PIRP				Irp,
	__in ULONG				EventContextLength,
	__in PDokanCCB			Ccb);

VOID
DokanFreeEventContext(
	__in PEVENT_CONTEXT	EventContext);


NTSTATUS
DokanRegisterPendingIrp(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP			Irp,
	__in PEVENT_CONTEXT	EventContext,
	__in ULONG			Flags);


VOID
DokanEventNotification(
	__in PIRP_LIST		NotifyEvent,
	__in PEVENT_CONTEXT	EventContext);


NTSTATUS
DokanUnmountNotification(
	__in PDokanDCB	Dcb,
	__in PEVENT_CONTEXT		EventContext);


VOID
DokanCompleteDirectoryControl(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);

VOID
DokanCompleteRead(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);

VOID
DokanCompleteWrite(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);


VOID
DokanCompleteQueryInformation(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);


VOID
DokanCompleteSetInformation(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION EventInfo);

VOID
DokanCompleteCreate(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);


VOID
DokanCompleteCleanup(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);


VOID
DokanCompleteLock(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);

VOID
DokanCompleteQueryVolumeInformation(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);

VOID
DokanCompleteFlush(
	__in PIRP_ENTRY			IrpEntry,
	__in PEVENT_INFORMATION	EventInfo);

VOID
DokanCompleteQuerySecurity(
	__in PIRP_ENTRY		IrpEntry,
	__in PEVENT_INFORMATION EventInfo);

VOID
DokanCompleteSetSecurity(
	__in PIRP_ENTRY		IrpEntry,
	__in PEVENT_INFORMATION EventInfo);

VOID
DokanNoOpRelease (
    __in PVOID Fcb);

BOOLEAN
DokanNoOpAcquire(
    __in PVOID Fcb,
    __in BOOLEAN Wait);

NTSTATUS
DokanCreateGlobalDiskDevice(
	__in PDRIVER_OBJECT DriverObject,
	__out PDOKAN_GLOBAL* DokanGlobal);

NTSTATUS
DokanCreateDiskDevice(
	__in PDRIVER_OBJECT DriverObject,
	__in ULONG			MountId,
	__in PWCHAR			BaseGuid,
	__in PDOKAN_GLOBAL	DokanGlobal,
	__in DEVICE_TYPE	DeviceType,
	__in ULONG			DeviceCharacteristics,
	__out PDokanDCB* Dcb);


VOID
DokanDeleteDeviceObject(
	__in PDokanDCB Dcb);

VOID
DokanPrintNTStatus(
	NTSTATUS	Status);


VOID
DokanNotifyReportChange0(
	__in PDokanFCB				Fcb,
	__in PUNICODE_STRING		FileName,
	__in ULONG					FilterMatch,
	__in ULONG					Action);

VOID
DokanNotifyReportChange(
	__in PDokanFCB	Fcb,
	__in ULONG		FilterMatch,
	__in ULONG		Action);


PDokanFCB
DokanAllocateFCB(
	__in PDokanVCB Vcb);


NTSTATUS
DokanFreeFCB(
  __in PDokanFCB Fcb);


PDokanCCB
DokanAllocateCCB(
	__in PDokanDCB Dcb,
	__in PDokanFCB	Fcb);


NTSTATUS
DokanFreeCCB(
  __in PDokanCCB Ccb);

NTSTATUS
DokanStartCheckThread(
	__in PDokanDCB	Dcb);

VOID
DokanStopCheckThread(
	__in PDokanDCB	Dcb);


BOOLEAN
DokanCheckCCB(
	__in PDokanDCB	Dcb,
	__in PDokanCCB	Ccb);

VOID
DokanInitIrpList(
	 __in PIRP_LIST		IrpList);

NTSTATUS
DokanStartEventNotificationThread(
	__in PDokanDCB	Dcb);

VOID
DokanStopEventNotificationThread(
	__in PDokanDCB	Dcb);


VOID
DokanUpdateTimeout(
	__out PLARGE_INTEGER KickCount,
	__in ULONG Timeout);

VOID
DokanUnmount(
	__in PDokanDCB Dcb);

VOID
PrintIdType(
	__in VOID* Id);

NTSTATUS
DokanAllocateMdl(
	__in PIRP	Irp,
	__in ULONG	Length);

VOID
DokanFreeMdl(
	__in PIRP	Irp);


#endif // _DOKAN_H_

