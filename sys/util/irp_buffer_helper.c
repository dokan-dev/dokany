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
#include "irp_buffer_helper.h"

ULONG GetProvidedInputSize(_In_ PIRP Irp) {
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
  switch (irpSp->MajorFunction) {
    case IRP_MJ_DEVICE_CONTROL:
      return irpSp->Parameters.DeviceIoControl.InputBufferLength;
    case IRP_MJ_FILE_SYSTEM_CONTROL:
      return irpSp->Parameters.FileSystemControl.InputBufferLength;
    default:
      return 0;
  }
}

ULONG GetProvidedOutputSize(_In_ PIRP Irp) {
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
  switch (irpSp->MajorFunction) {
    case IRP_MJ_DEVICE_CONTROL:
      return irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    case IRP_MJ_DIRECTORY_CONTROL:
      return irpSp->Parameters.QueryDirectory.Length;
    case IRP_MJ_FILE_SYSTEM_CONTROL:
      return irpSp->Parameters.FileSystemControl.OutputBufferLength;
    case IRP_MJ_QUERY_INFORMATION:
      return irpSp->Parameters.QueryFile.Length;
    case IRP_MJ_QUERY_SECURITY:
      return irpSp->Parameters.QuerySecurity.Length;
    case IRP_MJ_QUERY_VOLUME_INFORMATION:
      return irpSp->Parameters.QueryVolume.Length;
    default:
      return 0;
  }
}

PVOID GetInputBuffer(_In_ PIRP Irp) {
  static const ULONG methodMask =
      METHOD_BUFFERED | METHOD_IN_DIRECT | METHOD_NEITHER;
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
  PCHAR buffer = Irp->AssociatedIrp.SystemBuffer;

  // Figure out if we should be using something other than SystemBuffer.
  if (irpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL
      && (irpSp->Parameters.DeviceIoControl.IoControlCode & methodMask)
        == METHOD_NEITHER) {
    buffer = irpSp->Parameters.DeviceIoControl.Type3InputBuffer;
  }
  if (irpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL
      && (irpSp->Parameters.FileSystemControl.FsControlCode & methodMask)
          == METHOD_NEITHER) {
    buffer = irpSp->Parameters.FileSystemControl.Type3InputBuffer;
  }

  // If using a Type3InputBuffer, we need to probe it.
  if (Irp->RequestorMode != KernelMode && buffer != NULL
      && buffer != Irp->AssociatedIrp.SystemBuffer) {
    __try {
      ProbeForRead(buffer, GetProvidedInputSize(Irp), sizeof(char));
    } __except (DokanExceptionFilter(Irp, GetExceptionInformation())) {
      buffer = NULL;
    }
  }
  return buffer;
}

PVOID GetOutputBuffer(_In_ PIRP Irp) {
  static const ULONG methodMask =
      METHOD_BUFFERED | METHOD_OUT_DIRECT | METHOD_NEITHER;
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
  PCHAR buffer = Irp->AssociatedIrp.SystemBuffer;

  // Figure out if we should be using something other than SystemBuffer.
  if (irpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL
      && (irpSp->Parameters.DeviceIoControl.IoControlCode & methodMask)
           == METHOD_NEITHER) {
    buffer = Irp->UserBuffer;
  }
  if (irpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL
      && (irpSp->Parameters.FileSystemControl.FsControlCode & methodMask)
           == METHOD_NEITHER) {
    buffer = Irp->UserBuffer;
  }
  if (irpSp->MajorFunction == IRP_MJ_QUERY_SECURITY) {
    buffer = Irp->UserBuffer;
  }
  if (irpSp->MajorFunction == IRP_MJ_DIRECTORY_CONTROL) {
    if (Irp->MdlAddress) {
      buffer = MmGetSystemAddressForMdlNormalSafe(Irp->MdlAddress);
    } else {
      buffer = Irp->UserBuffer;
    }
  }

  // If using UserBuffer, we need to probe it, or they could pass us a kernel
  // memory address and have us overwrite it.
  if (Irp->RequestorMode != KernelMode && buffer == Irp->UserBuffer) {
    __try {
      ProbeForWrite(buffer, GetProvidedOutputSize(Irp), sizeof(char));
    } __except (DokanExceptionFilter(Irp, GetExceptionInformation())) {
      buffer = NULL;
    }
  }
  return buffer;
}

PVOID PrepareOutputWithSize(_Inout_ PIRP Irp, _In_ ULONG Size,
                            _In_ BOOLEAN SetInformationOnFailure) {
  PCHAR buffer = GetOutputBuffer(Irp);
  ULONG providedSize = GetProvidedOutputSize(Irp);
  if (buffer == NULL) {
    return NULL;
  }
  if (providedSize < Size) {
    Irp->IoStatus.Information = SetInformationOnFailure ? Size : 0;
    return NULL;
  }
  RtlZeroMemory(buffer, Size);
  Irp->IoStatus.Information = Size;
  return buffer;
}

BOOLEAN ExtendOutputBufferBySize(_Inout_ PIRP Irp, _In_ ULONG AdditionalSize,
                                 _In_ BOOLEAN UpdateInformationOnFailure) {
  PCHAR buffer = GetOutputBuffer(Irp);
  ULONG providedSize = GetProvidedOutputSize(Irp);
  ULONG_PTR usedSize = Irp->IoStatus.Information;
  if (buffer == NULL) {
    // This is really misuse of the function.
    return FALSE;
  }
  if (providedSize < usedSize + AdditionalSize) {
    if (UpdateInformationOnFailure) {
      Irp->IoStatus.Information += AdditionalSize;
    }
    return FALSE;
  }
  RtlZeroMemory(buffer + usedSize, AdditionalSize);
  Irp->IoStatus.Information += AdditionalSize;
  return TRUE;
}

BOOLEAN AppendVarSizeOutputString(_Inout_ PIRP Irp, _Inout_ PVOID Dest,
                                  _In_ const UNICODE_STRING* Str,
                                  _In_ BOOLEAN UpdateInformationOnFailure,
                                  _In_ BOOLEAN FillSpaceWithPartialString) {
  PCHAR buffer = GetOutputBuffer(Irp);
  ULONG_PTR allocatedSize = Irp->IoStatus.Information;
  if ((PCHAR)Dest < buffer || (PCHAR)Dest > buffer + allocatedSize) {
    // This is misuse of the function.
    return FALSE;
  }
  if (Str->Length == 0) {
    return TRUE;
  }
  ULONG_PTR destOffset = (PCHAR)Dest - buffer;
  ULONG_PTR remainingSize = allocatedSize - destOffset;
  ULONG_PTR copySize = Str->Length;
  if (remainingSize < copySize) {
    ULONG additionalSize = (ULONG)(copySize - remainingSize);
    if (!ExtendOutputBufferBySize(Irp, additionalSize,
                                  UpdateInformationOnFailure)) {
      if (FillSpaceWithPartialString) {
        ULONG providedSize = GetProvidedOutputSize(Irp);
        // Only copy whole characters, like NTFS.
        copySize = (providedSize - destOffset) & ~(ULONG_PTR)1;
        Irp->IoStatus.Information = copySize + destOffset;
      } else {
        return FALSE;
      }
    }
  }
  RtlCopyMemory(Dest, Str->Buffer, copySize);
  return copySize == Str->Length;
}
