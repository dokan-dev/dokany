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

#include "../dokan.h"
#include "log.h"

#include "../dokanfs_msg.h"

ULONG g_Debug = DOKAN_DEBUG_DEFAULT;
UNICODE_STRING FcbFileNameNull;

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
    message = DokanAlloc(sizeof(WCHAR) * messageCapacity);
    if (message == NULL) {
      DDbgPrint("Failed to allocate message of capacity %d\n", messageCapacity);
      __leave;
    }

    status = RtlStringCchVPrintfW(message, messageCapacity, Format, Args);
    if (status == STATUS_BUFFER_OVERFLOW) {
      // In this case we want to at least log what we can fit.
      DDbgPrint("Log message was larger than DOKAN_LOG_MAX_CHAR_COUNT."
                " Format: %S\n", Format);
    } else if (status != STATUS_SUCCESS) {
      DDbgPrint("Failed to generate log message with format: %S; status: %x\n",
                Format, status);
      __leave;
    }

    status = RtlStringCchLengthW(message, messageCapacity, &messageCharCount);
    if (status != STATUS_SUCCESS) {
      DDbgPrint("Failed to determine message length, status: %x\n", status);
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
        DDbgPrint("Failed to allocate packet of size %d\n", packetSize);
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

#ifdef _DEBUG
PWCHAR DokanGetNTSTATUSStr(NTSTATUS Status) {
  switch (Status) {
#include "ntstatus_log.inc"
    default:
      return L"Unknown";
  }
}

VOID DokanPrintNTStatus(NTSTATUS Status) {
  DDbgPrint("  status = 0x%x %ls\n", Status, DokanGetNTSTATUSStr(Status));
}
#endif

VOID PrintIdType(__in VOID *Id) {
  if (Id == NULL) {
    DDbgPrint("    IdType = NULL\n");
    return;
  }
  switch (GetIdentifierType(Id)) {
    case DGL:
      DDbgPrint("    IdType = DGL\n");
      break;
    case DCB:
      DDbgPrint("   IdType = DCB\n");
      break;
    case VCB:
      DDbgPrint("   IdType = VCB\n");
      break;
    case FCB:
      DDbgPrint("   IdType = FCB\n");
      break;
    case CCB:
      DDbgPrint("   IdType = CCB\n");
      break;
    default:
      DDbgPrint("   IdType = Unknown\n");
      break;
  }
}