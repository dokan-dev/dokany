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
#define DDbgPrint(...)                                                        \
  if (g_Debug) {                                                              \
    KdPrintEx(                                                                \
        (DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "[DokanFS] " __VA_ARGS__)); \
  }

// Print NTSTATUS hex value and the define string.
VOID DokanPrintNTStatus(NTSTATUS Status);

// Return the NTSTATUS define string name.
PWCHAR DokanGetNTSTATUSStr(NTSTATUS Status);
#else
// Nullify debug print on release.
#define DDbgPrint(...)
#define DokanPrintNTStatus(s)
#endif

extern UNICODE_STRING FcbFileNameNull;
#define DokanPrintFileName(FileObject)                                       \
  DDbgPrint("  FileName: %wZ FCB.FileName: %wZ\n", &FileObject->FileName,    \
            FileObject->FsContext2                                           \
                ? (((PDokanCCB)FileObject->FsContext2)->Fcb                  \
                       ? &((PDokanCCB)FileObject->FsContext2)->Fcb->FileName \
                       : &FcbFileNameNull)                                   \
                : &FcbFileNameNull)


// Print VCB or DCB Identifier Type
VOID PrintIdType(__in VOID *Id);

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