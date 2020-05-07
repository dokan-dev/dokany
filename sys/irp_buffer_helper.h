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

#ifndef STRUCT_HELPER_H_
#define STRUCT_HELPER_H_

#include <ntifs.h>

#include "public.h"

ULONG GetProvidedInputSize(_In_ PIRP Irp);
PVOID GetInputBuffer(_In_ PIRP Irp);

// The goal of these defines are to simplify IRP DeviceIOControl Buffer usage
// and safety size check

// Main Get DeviceIOControl Buffer from IRP.
// Those should not be used directly, but instead see GET_IRP_BUFFER.

#define GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer, SizeCompare, Exit, Status,    \
                                  InformationSize)                           \
  {                                                                          \
    ULONG irpBufferLen = GetProvidedInputSize(Irp);                          \
    (Buffer) = GetInputBuffer(Irp);                                          \
    ASSERT((Buffer) != NULL);                                                \
    if (!(Buffer)) {                                                         \
      Exit((Irp), (Status), (InformationSize));                              \
    } else if (SizeCompare((Buffer), irpBufferLen)) {                        \
      DDbgPrint("  Invalid Input Buffer length\n");                          \
      (Buffer) = NULL;                                                       \
      Exit((Irp), (Status), (InformationSize));                              \
    }                                                                        \
  }

#define GET_IRP_GENERIC_BUFFER(Irp, Buffer, CompareSize) \
  GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer, CompareSize, DOKAN_EXIT_NONE, 0, 0)

// GET_IRP_XXX helpers

// Generic type calcul size
#define GENERIC_SIZE_COMPARE(Buffer, BufferLen) \
  (sizeof(*(Buffer)) > (BufferLen))

// Specific type calcul size
#define MOUNTDEV_NAME_SIZE_COMPARE(MountDevName, BufferLen) \
  (GENERIC_SIZE_COMPARE(MountDevName, BufferLen) ||         \
   (ULONG)(FIELD_OFFSET(MOUNTDEV_NAME, Name[0]) +           \
           (MountDevName)->NameLength) > (BufferLen))

#define DOKAN_NOTIFY_PATH_INTERMEDIATE_SIZE_COMPARE(DokanNotifyPath, \
                                                    BufferLen)       \
  (GENERIC_SIZE_COMPARE(DokanNotifyPath, BufferLen) ||               \
   (ULONG)(FIELD_OFFSET(DOKAN_NOTIFY_PATH_INTERMEDIATE, Buffer[0]) + \
           (DokanNotifyPath)->Length) > (BufferLen))

#define DOKAN_UNICODE_STRING_INTERMEDIATE_SIZE_COMPARE(DokanUnicodeString, \
                                                       BufferLen)          \
  (GENERIC_SIZE_COMPARE(DokanUnicodeString, BufferLen) ||                  \
   (ULONG)(FIELD_OFFSET(DOKAN_UNICODE_STRING_INTERMEDIATE, Buffer[0]) +    \
           (DokanUnicodeString)->Length) > (BufferLen) ||                  \
   (ULONG)(FIELD_OFFSET(DOKAN_UNICODE_STRING_INTERMEDIATE, Buffer[0]) +    \
           (DokanUnicodeString)->MaximumLength) > (BufferLen) ||           \
   (DokanUnicodeString)->Length > (DokanUnicodeString)->MaximumLength)

// Exit types in size check failure
#define DOKAN_EXIT_NONE(Irp, Status, InformationSize)

#define DOKAN_EXIT_LEAVE(Irp, Status, InformationSize) \
  (Irp)->IoStatus.Information = InformationSize;       \
  status = Status;                                     \
  __leave;

#define DOKAN_EXIT_BREAK(Irp, Status, InformationSize) \
  (Irp)->IoStatus.Information = InformationSize;       \
  status = Status;                                     \
  break;

#define DOKAN_EXIT_RETURN(Irp, Status, InformationSize) return Status;

// Generic Get DeviceIOControl Buffer from IRP
#define GET_IRP_BUFFER(Irp, Buffer) \
  GET_IRP_GENERIC_BUFFER(Irp, Buffer, GENERIC_SIZE_COMPARE)

#define GET_IRP_BUFFER_OR_LEAVE(Irp, Buffer)                   \
  GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer, GENERIC_SIZE_COMPARE, \
                            DOKAN_EXIT_LEAVE, STATUS_BUFFER_TOO_SMALL, 0)

#define GET_IRP_BUFFER_OR_BREAK(Irp, Buffer)                   \
  GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer, GENERIC_SIZE_COMPARE, \
                            DOKAN_EXIT_BREAK, STATUS_BUFFER_TOO_SMALL, 0)

#define GET_IRP_BUFFER_OR_RETURN(Irp, Buffer)                  \
  GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer, GENERIC_SIZE_COMPARE, \
                            DOKAN_EXIT_RETURN, STATUS_BUFFER_TOO_SMALL, 0)

// Variations of Get DeviceIOControl for input buffer with a size check.
// DOKAN_NOTIFY_PATH_INTERMEDIATE
#define GET_IRP_NOTIFY_PATH_INTERMEDIATE_OR_RETURN(Irp, Buffer)          \
  GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer,                                 \
                            DOKAN_NOTIFY_PATH_INTERMEDIATE_SIZE_COMPARE, \
                            DOKAN_EXIT_RETURN, STATUS_BUFFER_TOO_SMALL, 0)

// DOKAN_UNICODE_STRING_INTERMEDIATE
#define GET_IRP_UNICODE_STRING_INTERMEDIATE_OR_RETURN(Irp, Buffer)          \
  GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer,                                    \
                            DOKAN_UNICODE_STRING_INTERMEDIATE_SIZE_COMPARE, \
                            DOKAN_EXIT_RETURN, STATUS_BUFFER_TOO_SMALL, 0)

// MOUNTDEV_NAME
#define GET_IRP_MOUNTDEV_NAME_OR_BREAK(Irp, Buffer)                  \
  GET_IRP_GENERIC_BUFFER_EX(Irp, Buffer, MOUNTDEV_NAME_SIZE_COMPARE, \
                            DOKAN_EXIT_BREAK, STATUS_BUFFER_TOO_SMALL, 0)

// Checks the expectation that the output buffer provided with the given IRP is
// at least the given size in bytes.
//
// If the buffer is large enough, this function sets the IRP's Information
// value to the passed-in Size value, sets that many bytes in the buffer to 0,
// and returns the buffer.
//
// If the buffer is too small, this function optionally sets the IRP's
// Information value to Size if SetInformationOnFailure is TRUE (some use cases
// call for the Information to be the actual reserved size, and some call for it
// to be the requested size, which are only different values when it fails).
PVOID PrepareOutputWithSize(_Inout_ PIRP Irp, _In_ ULONG Size,
                            _In_ BOOLEAN SetInformationOnFailure);

// Helper that supports the PREPARE_OUTPUT macro without causing an
// assignment-in-conditional at call sites.
inline BOOLEAN PrepareOutputHelper(_Inout_ PIRP Irp,
                                   _Out_ VOID** Buffer,
                                   _In_ ULONG Size,
                                   _In_ BOOLEAN SetInformationOnFailure) {
  *Buffer = PrepareOutputWithSize(Irp, Size, SetInformationOnFailure);
  return *Buffer != NULL;
}

// Helper to make PrepareOutputWithSize calls simpler by using the local scope's
// knowledge of the actual Buffer type.
#define PREPARE_OUTPUT(Irp, Buffer, SetInformationOnFailure)                   \
   PrepareOutputHelper((Irp), &(Buffer), sizeof(*Buffer),                      \
                       (SetInformationOnFailure))

// Checks if the output buffer for the given IRP is large enough to fit the
// given additional number of bytes, beyond the initial reservation already done
// by PrepareOutputWithSize or similar.
//
// If the buffer is large enough, this function adds AdditionalSize to the IRP's
// Information value, sets the additional bytes in the buffer to 0, and returns
// TRUE.
//
// If the buffer is too small, this function's only effect is to optionally
// increase the IRP's Information value, if UpdateInformationOnFailure is TRUE.
BOOLEAN ExtendOutputBySize(_Inout_ PIRP Irp, _In_ ULONG AdditionalSize,
                           _In_ BOOLEAN UpdateInformationOnFailure);

// Given an IRP that is returning a struct ending with a variable-sized string,
// i.e.
//
// struct {
//   ... fixed-size stuff ...
//   WCHAR Dest[1];
// } StructType;
//
// This function populates Dest using the content from Str, leaving the rest of
// the struct intact. It does not automatically null-terminate Dest if Str is
// not null-terminated. This function must only be used once for a given Dest
// address.
//
// As a prerequisite, the IRP's output buffer must have been prepared for the
// fixed size portion of the struct, via a call like
// PrepareOutputWithSize(Irp, sizeof(StructType), ...);
//
// This function extends the output buffer if necessary, as if by
// ExtendOutputBySize, returning FALSE if it fails. If extending the buffer
// succeeds, this function populates Dest from Str->Buffer.
//
// The FillSpaceWithPartialString flag enables the less frequently desired
// behavior of copying the max possible amount of the string when the output
// buffer can't fit the whole thing. A partial copy still counts as "failure"
// for the return value and the updating of the Information value.
BOOLEAN AppendVarSizeOutputString(_Inout_ PIRP Irp, _Inout_ PVOID Dest,
                                  _In_ const UNICODE_STRING* Str,
                                  _In_ BOOLEAN UpdateInformationOnFailure,
                                  _In_ BOOLEAN FillSpaceWithPartialString);

#endif
