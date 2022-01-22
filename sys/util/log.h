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

#ifndef LOG_H_
#define LOG_H_

#include <ntifs.h>

// Dokan debug log options
#define DOKAN_DEBUG_NONE 0
#define DOKAN_DEBUG_DEFAULT 1
#define DOKAN_DEBUG_LOCK 2
#define DOKAN_DEBUG_OPLOCKS 4

// Whether DbgPrint is enabled or not.
extern ULONG g_Debug;

typedef struct _DOKAN_LOG_ENTRY {
  LIST_ENTRY ListEntry;
  PVOID Vcb;
  DOKAN_LOG_MESSAGE Log;
} DOKAN_LOG_ENTRY, *PDOKAN_LOG_ENTRY;

typedef struct _DOKAN_LOG_CACHE {
  LONG NumberOfCachedEntries;
  ERESOURCE Resource;
  LIST_ENTRY Log;
} DOKAN_LOG_CACHE, *PDOKAN_LOG_CACHE;

// Global cache for driver global and specific volume logs.
extern DOKAN_LOG_CACHE g_DokanLogEntryList;

// Push log into the global log entry cache list.
VOID PushDokanLogEntry(_In_opt_ PVOID RequestContext,
                       _In_ PCSTR Format, ...);

// Remove all logs attached to a specific volume from the global cache.
VOID CleanDokanLogEntry(_In_ PVOID Vcb);

// Whether the IRP log should be cached.
// Used as an early check to unnecessary avoid computing the arguments when it
// is not needed.
BOOLEAN IsLogCacheEnabled(_In_opt_ PVOID RequestContext);

// Increment the active Volume having the cache log enabled and active the
// global caching if not already.
VOID IncrementVcbLogCacheCount();

// Stringify variable name
#define STR(x) #x

// Main print function which should not be used directly.
#define DDbgPrint(Format, ...)                        \
  KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, \
             "[dokan2]" Format "\n", __VA_ARGS__));

#if (NTDDI_VERSION >= NTDDI_WIN8)
#define DokanQuerySystemTime KeQuerySystemTimePrecise
#else
#define DokanQuerySystemTime KeQuerySystemTime
#endif

#define DOKAN_PUSH_LOG(RequestContext, Format, ...)                          \
  {                                                                          \
    LARGE_INTEGER SysTime;                                                   \
    LARGE_INTEGER LocalTime;                                                 \
    TIME_FIELDS TimeFields;                                                  \
    DokanQuerySystemTime(&SysTime);                                          \
    ExSystemTimeToLocalTime(&SysTime, &LocalTime);                           \
    RtlTimeToTimeFields(&LocalTime, &TimeFields);                            \
    PushDokanLogEntry(RequestContext, "[%02d:%02d:%02d.%03d]" Format,        \
                      TimeFields.Hour, TimeFields.Minute, TimeFields.Second, \
                      TimeFields.Milliseconds, __VA_ARGS__);                 \
  }

// Log and push to the cache the log message
#define DOKAN_CACHED_LOG(RequestContext, Format, ...)                  \
  {                                                                    \
    KIRQL Kirql = KeGetCurrentIrql();                                  \
    if (g_Debug) {                                                     \
      DDbgPrint("[%d]" Format, Kirql, __VA_ARGS__)                     \
    }                                                                  \
    if (Kirql == PASSIVE_LEVEL && IsLogCacheEnabled(RequestContext)) { \
      DOKAN_PUSH_LOG(RequestContext, Format, __VA_ARGS__)              \
    }                                                                  \
  }

// Log without caching the message. Must be only used for logging during the log
// caching workflow.
#define DOKAN_NO_CACHE_LOG(Format)                                       \
  {                                                                      \
    if (g_Debug) {                                                       \
      DDbgPrint("[%d]" DOKAN_LOG_HEADER ": " Format, KeGetCurrentIrql()) \
    }                                                                    \
  }

// Debug print header that should always be used by the print function.
#define DOKAN_LOG_HEADER "[" __FUNCTION__ "]"

// Internal formating log function.
#define DOKAN_LOG_INTERNAL(RequestContext, Format, ...)                    \
  if (!RequestContext || !RequestContext->DoNotLogActivity) {              \
    DOKAN_CACHED_LOG(RequestContext, DOKAN_LOG_HEADER Format, __VA_ARGS__) \
  }

// Default log function.
#define DOKAN_LOG(Format)                                           \
  do {                                                              \
    PREQUEST_CONTEXT dRequestContext = NULL;                        \
    DOKAN_CACHED_LOG(dRequestContext, DOKAN_LOG_HEADER ": " Format) \
  } while (0)

// Default log function with variable params.
#define DOKAN_LOG_(Format, ...)                                   \
  do {                                                            \
    PREQUEST_CONTEXT dRequestContext = NULL;                      \
    DOKAN_LOG_INTERNAL(dRequestContext, ": " Format, __VA_ARGS__) \
  } while (0)

// Logging function that should be used in an Irp context.
#define DOKAN_LOG_FINE_IRP(RequestContext, Format, ...) \
  DOKAN_LOG_INTERNAL(RequestContext, "[%p]: " Format,   \
                     (RequestContext ? RequestContext->Irp : 0), __VA_ARGS__)

// Logging function that should be used in an VCB context.
#define DOKAN_LOG_VCB(dVcb, Format, ...)                               \
  do {                                                                 \
    REQUEST_CONTEXT vcbRequestContext;                                 \
    RtlZeroMemory(&vcbRequestContext, sizeof(REQUEST_CONTEXT));        \
    vcbRequestContext.DoNotLogActivity = FALSE;                        \
    vcbRequestContext.Vcb = (dVcb);                                    \
    vcbRequestContext.Dcb = (dVcb)->Dcb;                               \
    DOKAN_LOG_INTERNAL((&vcbRequestContext), ": " Format, __VA_ARGS__) \
  } while (0)

// Only allow logging events that are not pulling events to avoid unnecessary log
// flood
#define DOKAN_DENIED_LOG_EVENT(IrpSp)                                          \
  (IrpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&                       \
   IrpSp->MinorFunction == IRP_MN_USER_FS_REQUEST &&                           \
   IrpSp->Parameters.FileSystemControl.FsControlCode ==                        \
       FSCTL_EVENT_PROCESS_N_PULL)

// Log the Irp FSCTL or IOCTL Control code.
#define DOKAN_LOG_IOCTL(RequestContext, ControlCode, format, ...)              \
  do {                                                                         \
    if (!RequestContext->DoNotLogActivity) {                                   \
      DOKAN_LOG_INTERNAL(RequestContext, "[%p][%s]: " format,                  \
                         RequestContext->Irp, DokanGetIoctlStr(ControlCode),   \
                         __VA_ARGS__)                                          \
    }                                                                          \
  } while (0)

// Log the whole Irp informations.
#define DOKAN_LOG_MJ_IRP(RequestContext, Format, ...)                          \
  DOKAN_LOG_INTERNAL(                                                          \
      RequestContext, "[%p][%s][%s][%s]: " Format, RequestContext->Irp,        \
      DokanGetIdTypeStr(RequestContext->DeviceObject->DeviceExtension),        \
      DokanGetMajorFunctionStr(RequestContext->IrpSp->MajorFunction),          \
      DokanGetMinorFunctionStr(RequestContext->IrpSp->MajorFunction,           \
                               RequestContext->IrpSp->MinorFunction),          \
      __VA_ARGS__)

// Log the Irp at dispatch time.
#define DOKAN_LOG_BEGIN_MJ(RequestContext)                                     \
  DOKAN_LOG_MJ_IRP(RequestContext, "Begin ProcessId=%lu",                      \
                   RequestContext->ProcessId)

// Log the Irp on exit of the dispatch.
#define DOKAN_LOG_END_MJ(RequestContext, Status)                               \
  do {                                                                         \
    if (RequestContext->DoNotComplete) {                                       \
      DOKAN_LOG_FINE_IRP(RequestContext, "End - Irp not completed %s",         \
                         DokanGetNTSTATUSStr(Status));                         \
    } else if (Status == STATUS_PENDING) {                                     \
      DOKAN_LOG_FINE_IRP(RequestContext, "End - Irp is marked pending");       \
    } else {                                                                   \
      DOKAN_LOG_FINE_IRP(RequestContext, "End - %s Information=%llx",          \
                         DokanGetNTSTATUSStr(Status),                          \
                         RequestContext->Irp->IoStatus.Information);           \
      DokanCompleteIrpRequest(RequestContext->Irp, Status);                    \
    }                                                                          \
  } while (0)

// Return the NTSTATUS define string name.
PCHAR DokanGetNTSTATUSStr(NTSTATUS Status);
// Return Identifier Type string name.
PCHAR DokanGetIdTypeStr(__in VOID *Id);
// Return IRP Major function string name.
PCHAR DokanGetMajorFunctionStr(UCHAR MajorFunction);
// Return IRP Minor function string name.
PCHAR DokanGetMinorFunctionStr(UCHAR MajorFunction, UCHAR MinorFunction);
// Return File Information class string name.
PCHAR DokanGetFileInformationClassStr(
    FILE_INFORMATION_CLASS FileInformationClass);
// Return Fs Information class string name.
PCHAR DokanGetFsInformationClassStr(FS_INFORMATION_CLASS FsInformationClass);
// Return NtCreateFile Information string name.
PCHAR DokanGetCreateInformationStr(ULONG_PTR Information);

// Return IOCTL string name.
PCHAR DokanGetIoctlStr(ULONG ControlCode);

typedef struct _DOKAN_LOGGER {
  PDRIVER_OBJECT DriverObject;
  UCHAR MajorFunctionCode;

} DOKAN_LOGGER, *PDOKAN_LOGGER;

#define DOKAN_INIT_LOGGER(logger, driverObject, majorFunctionCode) \
  DOKAN_LOGGER logger;                                             \
  logger.DriverObject = driverObject;                              \
  logger.MajorFunctionCode = majorFunctionCode;

// Logs an error to the Windows event log, even in production, with the given
// status, and returns the status passed in.
NTSTATUS DokanLogError(__in PDOKAN_LOGGER Logger, __in NTSTATUS Status,
                       __in LPCTSTR Format, ...);

// Logs an informational message to the Windows event log, even in production.
VOID DokanLogInfo(__in PDOKAN_LOGGER Logger, __in LPCTSTR Format, ...);

// A compact stack trace that can be easily logged.
typedef struct _DokanBackTrace {
  // The full address of a point-of-reference instruction near where the logging
  // occurs. One should be able to find this instruction in the disassembly of
  // the driver by seeing the log message content aside from this value. This
  // value then tells you the absolute address of that instruction at runtime.
  ULONG64 Address;

  // Three return addresses truncated to their lowest 20 bits. The lowest 20
  // bits of this value is the most distant return address, the next 20 bits are
  // the next frame up, etc. To find each of the 3 instructions referenced here,
  // one replaces the lowest 20 bits of Ip.
  ULONG64 ReturnAddresses;
} DokanBackTrace, *PDokanBackTrace;

// Captures a trace where Address is the full address of the call site
// instruction after the DokanCaptureBackTrace call, and ReturnAddresses
// indicates the 3 return addresses below that.
VOID DokanCaptureBackTrace(__out PDokanBackTrace Trace);

#endif  // LOG_H_