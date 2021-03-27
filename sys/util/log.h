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

extern ULONG g_Debug;

#ifdef _DEBUG

// Stringify variable name
#define STR(x) #x

// Main print function which should not be used directly.
#define DDbgPrint(...) \
  KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, __VA_ARGS__));

// Debug print header that should always be used by the print function.
#define DOKAN_LOG_HEADER "[dokan1][" __FUNCTION__ "][%d]"

// Internal formating log function.
#define DOKAN_LOG_INTERNAL(Format, ...) \
  DDbgPrint(DOKAN_LOG_HEADER Format "\n", KeGetCurrentIrql(), __VA_ARGS__)

// Default log function.
#define DOKAN_LOG(Format) \
  DDbgPrint(DOKAN_LOG_HEADER ": " Format "\n", KeGetCurrentIrql())

// Default log function with variable params.
#define DOKAN_LOG_(Format, ...) DOKAN_LOG_INTERNAL(": " Format, __VA_ARGS__)

// Logging function that should be used in an Irp context.
#define DOKAN_LOG_FINE_IRP(Irp, Format, ...) \
  DOKAN_LOG_INTERNAL("[%p]: " Format, Irp, __VA_ARGS__)

// Log the Irp FSCTL or IOCTL Control code.
#define DOKAN_LOG_IOCTL(Irp, ControlCode, format, ...)                        \
  DOKAN_LOG_INTERNAL("[%p][%s]: " format, Irp, DokanGetIoctlStr(ControlCode), \
                     __VA_ARGS__)

// Log the whole Irp informations.
#define DOKAN_LOG_MJ_IRP(Irp, IrpSp, Format, ...)                           \
  DOKAN_LOG_INTERNAL(                                                       \
      "[%p][%s][%s][%s][%lu]: " Format, Irp,                                \
      DokanGetIdTypeStr(IrpSp->DeviceObject->DeviceExtension),              \
      DokanGetMajorFunctionStr(IrpSp->MajorFunction),                       \
      DokanGetMinorFunctionStr(IrpSp->MajorFunction, IrpSp->MinorFunction), \
      IoGetRequestorProcessId(Irp), __VA_ARGS__)

// Log the Irp at dispatch time.
#define DOKAN_LOG_BEGIN_MJ(Irp) \
  DOKAN_LOG_MJ_IRP(Irp, IoGetCurrentIrpStackLocation(Irp), "Begin")

// Log the Irp on exit of the dispatch.
#define DOKAN_LOG_END_MJ(Irp, Status, Information)                            \
  {                                                                           \
    if (Irp) {                                                                \
      DOKAN_LOG_FINE_IRP(Irp, "End - %s Information=%llx",                    \
                         DokanGetNTSTATUSStr(Status), (ULONG_PTR)Information) \
    } else {                                                                  \
      DOKAN_LOG_FINE_IRP(Irp, "End - Irp not completed %s",                   \
                         DokanGetNTSTATUSStr(Status))                         \
    }                                                                         \
  }

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
PCHAR DokanGetCreateInformationStr(ULONG Information);

#else
// Nullify debug print on release.
#define DDbgPrint(...)
#define DOKAN_LOG_(f, ...)
#define DOKAN_LOG(f)
#define DOKAN_LOG_FINE_IRP(i, f, ...)
#define DOKAN_LOG_IOCTL(i, c, f, ...)
#define DOKAN_LOG_BEGIN_MJ(i)
#define DOKAN_LOG_END_MJ(i, s, info)
#define DokanGetIdTypeStr(i)
#define DokanGetNTSTATUSStr(s)
#define DokanGetMajorFunctionStr(major)
#define DokanGetMinorFunctionStr(major, minor)
#define DokanGetFileInformationClassStr(f)
#define DokanGetFsInformationClassStr(f)
#define DokanGetCreateInformationStr(i)
#endif

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