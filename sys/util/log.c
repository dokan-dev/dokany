/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.

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

// Tmp fix for unresolve external symbol vswprintf build error on last WDK.
#define _NO_CRT_STDIO_INLINE

#include "../dokan.h"
#include "log.h"

#include <mountdev.h>
#include <ntdddisk.h>
#include <ntddvol.h>
#include <ntddstor.h>
#include <ntifs.h>
#include "../dokanfs_msg.h"

ULONG g_Debug = DOKAN_DEBUG_DEFAULT;
// Has any mount since startup has requested to cache driver logs. It is only at
// this moment that we will start to record global logs.
BOOLEAN g_DokanDriverLogCacheEnabled = FALSE;
// Number of current Dokan Volume having Drive log caching enabled.
LONG g_DokanVcbDriverLogCacheCount = 0;
DOKAN_LOG_CACHE g_DokanLogEntryList;

VOID PopDokanLogEntry(_In_opt_ PVOID RequestContext, _In_ PDokanVCB Vcb);
#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, PushDokanLogEntry)
#pragma alloc_text(PAGE, PopDokanLogEntry)
#pragma alloc_text(PAGE, CleanDokanLogEntry)
#endif

#define DOKAN_LOG_MAX_ENTRY_CACHED 1024

#define DOKAN_LOG_MAX_CHAR_COUNT 2048
#define DOKAN_LOG_MAX_PACKET_BYTES \
  (ERROR_LOG_MAXIMUM_SIZE - sizeof(IO_ERROR_LOG_PACKET))
#define DOKAN_LOG_MAX_PACKET_NONNULL_CHARS \
  (DOKAN_LOG_MAX_PACKET_BYTES / sizeof(WCHAR) - 1)

static VOID DokanPrintToSysLog(__in PDRIVER_OBJECT DriverObject,
                               __in UCHAR MajorFunctionCode,
                               __in NTSTATUS MessageId,
                               __in NTSTATUS Status,
                               __in LPCTSTR Format,
                               __in va_list Args) {
  NTSTATUS status = STATUS_SUCCESS;
  PIO_ERROR_LOG_PACKET packet = NULL;
  WCHAR *message = NULL;
  size_t messageCapacity = DOKAN_LOG_MAX_CHAR_COUNT;
  size_t messageCharCount = 0;
  size_t messageCharsWritten = 0;
  size_t packetCount = 0;
  size_t i = 0;
  UCHAR packetCharCount = 0;
  UCHAR packetSize = 0;

  __try {
  	if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
      DOKAN_LOG_("Event viewer logging called at a too high IRQL\n");
      __leave;
    }

    message = DokanAlloc(sizeof(WCHAR) * messageCapacity);
    if (message == NULL) {
      DOKAN_LOG_("Failed to allocate message of capacity %d",
                 messageCapacity);
      __leave;
    }

    status = RtlStringCchVPrintfW(message, messageCapacity, Format, Args);
    if (status == STATUS_BUFFER_OVERFLOW) {
      // In this case we want to at least log what we can fit.
      DOKAN_LOG_(
          "Log message was larger than DOKAN_LOG_MAX_CHAR_COUNT."
                " Format: %S", Format);
    } else if (status != STATUS_SUCCESS) {
      DOKAN_LOG_("Failed to generate log message with format: %S; status: %x",
                Format, status);
      __leave;
    }

    status = RtlStringCchLengthW(message, messageCapacity, &messageCharCount);
    if (status != STATUS_SUCCESS) {
      DOKAN_LOG_("Failed to determine message length, status: %x", status);
      __leave;
    }

    packetCount = messageCharCount / DOKAN_LOG_MAX_PACKET_NONNULL_CHARS;
    if (messageCharCount % DOKAN_LOG_MAX_PACKET_NONNULL_CHARS != 0) {
      ++packetCount;
    }

    for (i = 0; i < packetCount; i++) {
      packetCharCount = (UCHAR)min(messageCharCount - messageCharsWritten,
                                   DOKAN_LOG_MAX_PACKET_NONNULL_CHARS);
      packetSize =
          sizeof(IO_ERROR_LOG_PACKET) + sizeof(WCHAR) * (packetCharCount + 1);
      packet = IoAllocateErrorLogEntry(DriverObject, packetSize);
      if (packet == NULL) {
        DOKAN_LOG_("Failed to allocate packet of size %d", packetSize);
        __leave;
      }
      RtlZeroMemory(packet, packetSize);
      packet->MajorFunctionCode = MajorFunctionCode;
      packet->NumberOfStrings = 1;
      packet->StringOffset =
          (USHORT)((char *)&packet->DumpData[0] - (char *)packet);
      packet->ErrorCode = MessageId;
      packet->FinalStatus = Status;
      RtlCopyMemory(&packet->DumpData[0], message + messageCharsWritten,
                    sizeof(WCHAR) * packetCharCount);
      IoWriteErrorLogEntry(packet);  // Destroys packet.
      packet = NULL;
      messageCharsWritten += packetCharCount;
    }
  } __finally {
    if (message != NULL) {
      ExFreePool(message);
    }
  }
}

NTSTATUS DokanLogError(__in PDOKAN_LOGGER Logger,
                       __in NTSTATUS Status,
                       __in LPCTSTR Format,
                       ...) {
  va_list args;
  va_start(args, Format);
  DokanPrintToSysLog(Logger->DriverObject, Logger->MajorFunctionCode,
                     DOKANFS_INFO_MSG, Status, Format, args);
  va_end(args);
  return Status;
}

VOID DokanLogInfo(__in PDOKAN_LOGGER Logger, __in LPCTSTR Format, ...) {
  va_list args;
  va_start(args, Format);
  DokanPrintToSysLog(Logger->DriverObject, Logger->MajorFunctionCode,
                     DOKANFS_INFO_MSG, STATUS_SUCCESS, Format, args);
  va_end(args);
}

VOID DokanCaptureBackTrace(__out PDokanBackTrace Trace) {
  PVOID rawTrace[4];
  USHORT count = RtlCaptureStackBackTrace(1, 4, rawTrace, NULL);
  Trace->Address = (ULONG64)((count > 0) ? rawTrace[0] : 0);
  Trace->ReturnAddresses =
        (((count > 1) ? ((ULONG64)rawTrace[1] & 0xfffff) : 0) << 40)
      | (((count > 2) ? ((ULONG64)rawTrace[2] & 0xfffff) : 0) << 20)
      |  ((count > 3) ? ((ULONG64)rawTrace[3] & 0xfffff) : 0);
}

#define CASE_STR(x) \
  case x:           \
    return #x;

PCHAR DokanGetNTSTATUSStr(NTSTATUS Status) {
  switch (Status) {
#include "ntstatus_log.inc"
  }
  return "Unknown";
}

PCHAR DokanGetMajorFunctionStr(UCHAR MajorFunction) {
  // List imported from wdm.h
  switch (MajorFunction) {
    CASE_STR(IRP_MJ_CREATE)
    CASE_STR(IRP_MJ_CREATE_NAMED_PIPE)
    CASE_STR(IRP_MJ_CLOSE)
    CASE_STR(IRP_MJ_READ)
    CASE_STR(IRP_MJ_WRITE)
    CASE_STR(IRP_MJ_QUERY_INFORMATION)
    CASE_STR(IRP_MJ_SET_INFORMATION)
    CASE_STR(IRP_MJ_QUERY_EA)
    CASE_STR(IRP_MJ_SET_EA)
    CASE_STR(IRP_MJ_FLUSH_BUFFERS)
    CASE_STR(IRP_MJ_QUERY_VOLUME_INFORMATION)
    CASE_STR(IRP_MJ_SET_VOLUME_INFORMATION)
    CASE_STR(IRP_MJ_DIRECTORY_CONTROL)
    CASE_STR(IRP_MJ_FILE_SYSTEM_CONTROL)
    CASE_STR(IRP_MJ_DEVICE_CONTROL)
    CASE_STR(IRP_MJ_INTERNAL_DEVICE_CONTROL)
    CASE_STR(IRP_MJ_SHUTDOWN)
    CASE_STR(IRP_MJ_LOCK_CONTROL)
    CASE_STR(IRP_MJ_CLEANUP)
    CASE_STR(IRP_MJ_CREATE_MAILSLOT)
    CASE_STR(IRP_MJ_QUERY_SECURITY)
    CASE_STR(IRP_MJ_SET_SECURITY)
    CASE_STR(IRP_MJ_POWER)
    CASE_STR(IRP_MJ_SYSTEM_CONTROL)
    CASE_STR(IRP_MJ_DEVICE_CHANGE)
    CASE_STR(IRP_MJ_QUERY_QUOTA)
    CASE_STR(IRP_MJ_SET_QUOTA)
    CASE_STR(IRP_MJ_PNP)
  }
  return "MJ_UKNOWN";
}

PCHAR DokanGetMinorFunctionStr(UCHAR MajorFunction, UCHAR MinorFunction) {
  // List imported from ntddk.h & wdm.h
  switch (MajorFunction) {
    case IRP_MJ_SYSTEM_CONTROL: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_QUERY_ALL_DATA)
        CASE_STR(IRP_MN_QUERY_SINGLE_INSTANCE)
        CASE_STR(IRP_MN_CHANGE_SINGLE_INSTANCE)
        CASE_STR(IRP_MN_CHANGE_SINGLE_ITEM)
        CASE_STR(IRP_MN_ENABLE_EVENTS)
        CASE_STR(IRP_MN_DISABLE_EVENTS)
        CASE_STR(IRP_MN_ENABLE_COLLECTION)
        CASE_STR(IRP_MN_DISABLE_COLLECTION)
        CASE_STR(IRP_MN_REGINFO)
        CASE_STR(IRP_MN_EXECUTE_METHOD)
        CASE_STR(IRP_MN_REGINFO_EX)
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_POWER: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_WAIT_WAKE)
        CASE_STR(IRP_MN_POWER_SEQUENCE)
        CASE_STR(IRP_MN_SET_POWER)
        CASE_STR(IRP_MN_QUERY_POWER)
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_FLUSH_BUFFERS: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_FLUSH_AND_PURGE)
#if (NTDDI_VERSION >= NTDDI_WIN8)
        CASE_STR(IRP_MN_FLUSH_DATA_ONLY)
        CASE_STR(IRP_MN_FLUSH_NO_SYNC)
#endif
#if (NTDDI_VERSION >= NTDDI_WIN10_RS1)
        CASE_STR(IRP_MN_FLUSH_DATA_SYNC_ONLY)
#endif
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_DIRECTORY_CONTROL: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_QUERY_DIRECTORY)
        CASE_STR(IRP_MN_NOTIFY_CHANGE_DIRECTORY)
        CASE_STR(IRP_MN_NOTIFY_CHANGE_DIRECTORY_EX)
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_FILE_SYSTEM_CONTROL: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_USER_FS_REQUEST)
        CASE_STR(IRP_MN_MOUNT_VOLUME)
        CASE_STR(IRP_MN_VERIFY_VOLUME)
        CASE_STR(IRP_MN_LOAD_FILE_SYSTEM)
        CASE_STR(IRP_MN_KERNEL_CALL)
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_LOCK_CONTROL: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_LOCK)
        CASE_STR(IRP_MN_UNLOCK_ALL)
        CASE_STR(IRP_MN_UNLOCK_ALL_BY_KEY)
        CASE_STR(IRP_MN_UNLOCK_SINGLE)
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_PNP: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_START_DEVICE)
        CASE_STR(IRP_MN_QUERY_REMOVE_DEVICE)
        CASE_STR(IRP_MN_REMOVE_DEVICE)
        CASE_STR(IRP_MN_CANCEL_REMOVE_DEVICE)
        CASE_STR(IRP_MN_STOP_DEVICE)
        CASE_STR(IRP_MN_QUERY_STOP_DEVICE)
        CASE_STR(IRP_MN_CANCEL_STOP_DEVICE)
        CASE_STR(IRP_MN_QUERY_DEVICE_RELATIONS)
        CASE_STR(IRP_MN_QUERY_INTERFACE)
        CASE_STR(IRP_MN_QUERY_CAPABILITIES)
        CASE_STR(IRP_MN_QUERY_RESOURCES)
        CASE_STR(IRP_MN_QUERY_RESOURCE_REQUIREMENTS)
        CASE_STR(IRP_MN_QUERY_DEVICE_TEXT)
        CASE_STR(IRP_MN_FILTER_RESOURCE_REQUIREMENTS)
        CASE_STR(IRP_MN_READ_CONFIG)
        CASE_STR(IRP_MN_WRITE_CONFIG)
        CASE_STR(IRP_MN_EJECT)
        CASE_STR(IRP_MN_SET_LOCK)
        CASE_STR(IRP_MN_QUERY_ID)
        CASE_STR(IRP_MN_QUERY_PNP_DEVICE_STATE)
        CASE_STR(IRP_MN_QUERY_BUS_INFORMATION)
        CASE_STR(IRP_MN_DEVICE_USAGE_NOTIFICATION)
        CASE_STR(IRP_MN_SURPRISE_REMOVAL)
        CASE_STR(IRP_MN_DEVICE_ENUMERATED)
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_READ:
    case IRP_MJ_WRITE: {
      switch (MinorFunction) {
        CASE_STR(IRP_MN_COMPLETE)
        CASE_STR(IRP_MN_COMPLETE_MDL)
        CASE_STR(IRP_MN_COMPLETE_MDL_DPC)
        CASE_STR(IRP_MN_COMPRESSED)
        CASE_STR(IRP_MN_DPC)
        CASE_STR(IRP_MN_MDL)
        CASE_STR(IRP_MN_MDL_DPC)
        CASE_STR(IRP_MN_NORMAL)
        default:
          return "MN_UNKNOWN";
      }
    }
    case IRP_MJ_DEVICE_CONTROL: {
      switch (MinorFunction) {
        default:
          return "MN_UNKNOWN";
      }
    }
  }
  return "";
}

PCHAR DokanGetFileInformationClassStr(
    FILE_INFORMATION_CLASS FileInformationClass) {
    // List imported from wdm.h
  switch (FileInformationClass) {
    CASE_STR(FileDirectoryInformation)
    CASE_STR(FileFullDirectoryInformation)
    CASE_STR(FileBothDirectoryInformation)
    CASE_STR(FileBasicInformation)
    CASE_STR(FileStandardInformation)
    CASE_STR(FileInternalInformation)
    CASE_STR(FileEaInformation)
    CASE_STR(FileAccessInformation)
    CASE_STR(FileNameInformation)
    CASE_STR(FileRenameInformation)
    CASE_STR(FileLinkInformation)
    CASE_STR(FileNamesInformation)
    CASE_STR(FileDispositionInformation)
    CASE_STR(FilePositionInformation)
    CASE_STR(FileFullEaInformation)
    CASE_STR(FileModeInformation)
    CASE_STR(FileAlignmentInformation)
    CASE_STR(FileAllInformation)
    CASE_STR(FileAllocationInformation)
    CASE_STR(FileEndOfFileInformation)
    CASE_STR(FileAlternateNameInformation)
    CASE_STR(FileStreamInformation)
    CASE_STR(FilePipeInformation)
    CASE_STR(FilePipeLocalInformation)
    CASE_STR(FilePipeRemoteInformation)
    CASE_STR(FileMailslotQueryInformation)
    CASE_STR(FileMailslotSetInformation)
    CASE_STR(FileCompressionInformation)
    CASE_STR(FileObjectIdInformation)
    CASE_STR(FileCompletionInformation)
    CASE_STR(FileMoveClusterInformation)
    CASE_STR(FileQuotaInformation)
    CASE_STR(FileReparsePointInformation)
    CASE_STR(FileNetworkOpenInformation)
    CASE_STR(FileAttributeTagInformation)
    CASE_STR(FileTrackingInformation)
    CASE_STR(FileIdBothDirectoryInformation)
    CASE_STR(FileIdFullDirectoryInformation)
    CASE_STR(FileValidDataLengthInformation)
    CASE_STR(FileShortNameInformation)
    CASE_STR(FileIoCompletionNotificationInformation)
    CASE_STR(FileIoStatusBlockRangeInformation)
    CASE_STR(FileIoPriorityHintInformation)
    CASE_STR(FileSfioReserveInformation)
    CASE_STR(FileSfioVolumeInformation)
    CASE_STR(FileHardLinkInformation)
    CASE_STR(FileProcessIdsUsingFileInformation)
    CASE_STR(FileNormalizedNameInformation)
    CASE_STR(FileNetworkPhysicalNameInformation)
    CASE_STR(FileIdGlobalTxDirectoryInformation)
    CASE_STR(FileIsRemoteDeviceInformation)
    CASE_STR(FileUnusedInformation)
    CASE_STR(FileNumaNodeInformation)
    CASE_STR(FileStandardLinkInformation)
    CASE_STR(FileRemoteProtocolInformation)
    CASE_STR(FileRenameInformationBypassAccessCheck)
    CASE_STR(FileLinkInformationBypassAccessCheck)
    CASE_STR(FileVolumeNameInformation)
    CASE_STR(FileIdInformation)
    CASE_STR(FileIdExtdDirectoryInformation)
    CASE_STR(FileReplaceCompletionInformation)
    CASE_STR(FileHardLinkFullIdInformation)
    CASE_STR(FileIdExtdBothDirectoryInformation)
    CASE_STR(FileDispositionInformationEx)
    CASE_STR(FileRenameInformationEx)
    CASE_STR(FileRenameInformationExBypassAccessCheck)
    CASE_STR(FileDesiredStorageClassInformation)
    CASE_STR(FileStatInformation)
    CASE_STR(FileMemoryPartitionInformation)
    CASE_STR(FileStatLxInformation)
    CASE_STR(FileCaseSensitiveInformation)
    CASE_STR(FileLinkInformationEx)
    CASE_STR(FileLinkInformationExBypassAccessCheck)
    CASE_STR(FileStorageReserveIdInformation)
    CASE_STR(FileCaseSensitiveInformationForceAccessCheck)
  }
  return "Unknown";
}

PCHAR DokanGetFsInformationClassStr(FS_INFORMATION_CLASS FsInformationClass) {
  // List imported from wdm.h
  switch (FsInformationClass) {
    CASE_STR(FileFsVolumeInformation)
    CASE_STR(FileFsLabelInformation)
    CASE_STR(FileFsSizeInformation)
    CASE_STR(FileFsDeviceInformation)
    CASE_STR(FileFsAttributeInformation)
    CASE_STR(FileFsControlInformation)
    CASE_STR(FileFsFullSizeInformation)
    CASE_STR(FileFsObjectIdInformation)
    CASE_STR(FileFsDriverPathInformation)
    CASE_STR(FileFsVolumeFlagsInformation)
    CASE_STR(FileFsSectorSizeInformation)
    CASE_STR(FileFsDataCopyInformation)
    CASE_STR(FileFsMetadataSizeInformation)
    CASE_STR(FileFsFullSizeInformationEx)
  }
  return "Unknown";
}

PCHAR DokanGetIdTypeStr(__in VOID *Id) {
  if (Id == NULL) {
    return "NULL";
  }
  switch (GetIdentifierType(Id)) {
    CASE_STR(DGL)
    CASE_STR(DCB)
    CASE_STR(VCB)
    CASE_STR(FCB)
    CASE_STR(CCB)
    CASE_STR(FREED_FCB)
  }
  return "Unknown";
}

PCHAR DokanGetCreateInformationStr(ULONG_PTR Information) {
  // List imported from wdm.h
  switch (Information) {
    CASE_STR(FILE_SUPERSEDED)
    CASE_STR(FILE_OPENED)
    CASE_STR(FILE_CREATED)
    CASE_STR(FILE_OVERWRITTEN)
    CASE_STR(FILE_EXISTS)
    CASE_STR(FILE_DOES_NOT_EXIST)
  }
  return "Unknown";
}

PCHAR DokanGetIoctlStr(ULONG ControlCode) {
  switch (ControlCode) {
    CASE_STR(FSCTL_GET_VERSION)
    CASE_STR(FSCTL_SET_DEBUG_MODE)
    CASE_STR(FSCTL_EVENT_RELEASE)
    CASE_STR(FSCTL_EVENT_START)
    CASE_STR(FSCTL_EVENT_WRITE)
    CASE_STR(FSCTL_RESET_TIMEOUT)
    CASE_STR(FSCTL_GET_ACCESS_TOKEN)
    CASE_STR(FSCTL_EVENT_MOUNTPOINT_LIST)
    CASE_STR(FSCTL_ACTIVATE_KEEPALIVE)
    CASE_STR(FSCTL_NOTIFY_PATH)
    CASE_STR(FSCTL_GET_VOLUME_METRICS)
    CASE_STR(FSCTL_MOUNTPOINT_CLEANUP)
    CASE_STR(FSCTL_EVENT_PROCESS_N_PULL)
#include "ioctl.inc"
  }
  return "Unknown";
}

VOID PushDokanLogEntry(_In_opt_ PVOID RequestContext, _In_ PCSTR Format, ...) {
  PREQUEST_CONTEXT requestContext = RequestContext;
  PDOKAN_LOG_ENTRY logEntry;

  PAGED_CODE();

  // Is that a global log or a Vcb log that has driver log disptached
  // enabled ?
  if (requestContext && requestContext->Vcb && requestContext->Vcb->Dcb &&
      !requestContext->Vcb->Dcb->DispatchDriverLogs) {
    return;
  }

  __try {
    DokanResourceLockRW(&(g_DokanLogEntryList.Resource));
    if (g_DokanLogEntryList.NumberOfCachedEntries >=
        DOKAN_LOG_MAX_ENTRY_CACHED) {
      ASSERT(!IsListEmpty(&g_DokanLogEntryList.Log));
      PLIST_ENTRY listEntry = g_DokanLogEntryList.Log.Flink;
      RemoveEntryList(listEntry);
      logEntry = CONTAINING_RECORD(listEntry, DOKAN_LOG_ENTRY, ListEntry);
      ExFreePool(logEntry);
      --g_DokanLogEntryList.NumberOfCachedEntries;
    }

    logEntry =
        DokanAllocZero(sizeof(DOKAN_LOG_ENTRY) + DOKAN_LOG_MAX_CHAR_COUNT);
    if (!logEntry) {
      DOKAN_NO_CACHE_LOG("Failed to allocate DOKAN_LOG_ENTRY");
      __leave;
    }
    InitializeListHead(&logEntry->ListEntry);
    if (requestContext) {
      logEntry->Vcb = requestContext->Vcb;
    }

    PSTR ppszDestEnd = NULL;
    va_list args;
    va_start(args, Format);
    NTSTATUS status =
        RtlStringCchVPrintfExA(logEntry->Log.Message, DOKAN_LOG_MAX_CHAR_COUNT,
                               &ppszDestEnd, NULL, 0, Format, args);
    logEntry->Log.MessageLength = (ULONG)(ppszDestEnd - logEntry->Log.Message);
    va_end(args);
    if (status != STATUS_SUCCESS && status != STATUS_BUFFER_OVERFLOW) {
      ExFreePool(logEntry);
      __leave;
    }

    ++g_DokanLogEntryList.NumberOfCachedEntries;
    InsertTailList(&g_DokanLogEntryList.Log, &logEntry->ListEntry);

    if (requestContext && requestContext->Vcb && requestContext->Vcb->Dcb) {
      PopDokanLogEntry(requestContext, requestContext->Vcb);
    }
  } __finally {
    DokanResourceUnlock(&(g_DokanLogEntryList.Resource));
  }
}

// Dispatch global and specific Vcb log messages from global
// log entry cache list to userland.
// Need to be called with g_DokanLogEntryList Lock RW.
VOID PopDokanLogEntry(_In_opt_ PVOID RequestContext, _In_ PDokanVCB Vcb) {
  PLIST_ENTRY listEntry;
  PLIST_ENTRY nextListEntry;
  PDOKAN_LOG_ENTRY logEntry;
  PDOKAN_LOG_MESSAGE dokanLogString;
  ULONG messageFullSize;

  PAGED_CODE();

  listEntry = g_DokanLogEntryList.Log.Flink;
  while (listEntry != &(g_DokanLogEntryList.Log)) {
    logEntry = CONTAINING_RECORD(listEntry, DOKAN_LOG_ENTRY, ListEntry);

    // Only Pop global and the Vcb logs.
    if (logEntry->Vcb != NULL && logEntry->Vcb != Vcb) {
      listEntry = listEntry->Flink;
      continue;
    }

    messageFullSize = logEntry->Log.MessageLength +
                      FIELD_OFFSET(DOKAN_LOG_MESSAGE, Message[0]);
    PEVENT_CONTEXT eventContext =
        AllocateEventContextRaw(sizeof(EVENT_CONTEXT) + messageFullSize);
    if (!eventContext) {
      return;
    }
    eventContext->MountId = Vcb->Dcb->MountId;
    eventContext->MajorFunction = DOKAN_IRP_LOG_MESSAGE;
    dokanLogString = (PDOKAN_LOG_MESSAGE)((PCHAR)(PCHAR)eventContext +
                                          sizeof(EVENT_CONTEXT));
    RtlCopyMemory(dokanLogString, &logEntry->Log, messageFullSize);
    if (RequestContext) {
      DokanEventNotification(RequestContext, &Vcb->Dcb->NotifyEvent,
                             eventContext);
    }

    nextListEntry = listEntry->Flink;
    RemoveEntryList(listEntry);
    ExFreePool(logEntry);
    --g_DokanLogEntryList.NumberOfCachedEntries;
    listEntry = nextListEntry;
  }
}

// Is it a global log (NULL IRP) and we have the global cache enabled or it is a
// volume log (Valid IRP) with an active volume having the cache log enabled.
BOOLEAN IsLogCacheEnabled(_In_opt_ PVOID RequestContext) {
  return (!RequestContext && g_DokanDriverLogCacheEnabled) ||
         (RequestContext && g_DokanVcbDriverLogCacheCount);
}

VOID IncrementVcbLogCacheCount() {
  InterlockedIncrement(&g_DokanVcbDriverLogCacheCount);
  if (!g_DokanDriverLogCacheEnabled) {
    g_DokanDriverLogCacheEnabled = TRUE;
  }
}

VOID CleanDokanLogEntry(_In_ PVOID Vcb) {
  PLIST_ENTRY listEntry;
  PLIST_ENTRY nextListEntry;
  PDOKAN_LOG_ENTRY logEntry;
  PDokanVCB vcb = Vcb;

  PAGED_CODE();

  if (!vcb->Dcb->DispatchDriverLogs) {
    return;
  }

  InterlockedDecrement(&g_DokanVcbDriverLogCacheCount);

  __try {
    DokanResourceLockRW(&(g_DokanLogEntryList.Resource));
    listEntry = g_DokanLogEntryList.Log.Flink;
    while (listEntry != &(g_DokanLogEntryList.Log)) {
      logEntry = CONTAINING_RECORD(listEntry, DOKAN_LOG_ENTRY, ListEntry);

      if (logEntry->Vcb != Vcb) {
        listEntry = listEntry->Flink;
        continue;
      }

      nextListEntry = listEntry->Flink;
      RemoveEntryList(listEntry);
      ExFreePool(logEntry);
      --g_DokanLogEntryList.NumberOfCachedEntries;
      listEntry = nextListEntry;
    }
  } __finally {
    DokanResourceUnlock(&(g_DokanLogEntryList.Resource));
  }
}
