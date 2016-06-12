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

#include "dokani.h"
#include "fileinfo.h"

NTSTATUS DOKAN_CALLBACK DokanGetDiskFreeSpace(PULONGLONG FreeBytesAvailable,
                                              PULONGLONG TotalNumberOfBytes,
                                              PULONGLONG TotalNumberOfFreeBytes,
                                              PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  *FreeBytesAvailable = 512 * 1024 * 1024;
  *TotalNumberOfBytes = 1024 * 1024 * 1024;
  *TotalNumberOfFreeBytes = 512 * 1024 * 1024;

  return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK DokanGetVolumeInformation(
    LPWSTR VolumeNameBuffer, DWORD VolumeNameSize, LPDWORD VolumeSerialNumber,
    LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
    LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize,
    PDOKAN_FILE_INFO DokanFileInfo) {
  UNREFERENCED_PARAMETER(DokanFileInfo);

  wcscpy_s(VolumeNameBuffer, VolumeNameSize, L"DOKAN");
  *VolumeSerialNumber = 0x19831116;
  *MaximumComponentLength = 256;
  *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
                     FILE_SUPPORTS_REMOTE_STORAGE | FILE_UNICODE_ON_DISK;

  wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"NTFS");

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFsVolumeInformation(PEVENT_INFORMATION EventInfo,
                         PEVENT_CONTEXT EventContext, PDOKAN_FILE_INFO FileInfo,
                         PDOKAN_OPERATIONS DokanOperations) {
  WCHAR volumeName[MAX_PATH];
  DWORD volumeSerial = 0;
  DWORD maxComLength = 0;
  DWORD fsFlags = 0;
  WCHAR fsName[MAX_PATH];
  ULONG remainingLength;
  ULONG bytesToCopy;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  PFILE_FS_VOLUME_INFORMATION volumeInfo =
      (PFILE_FS_VOLUME_INFORMATION)EventInfo->Buffer;

  remainingLength = EventContext->Operation.Volume.BufferLength;

  if (remainingLength < sizeof(FILE_FS_VOLUME_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  RtlZeroMemory(volumeName, sizeof(volumeName));
  RtlZeroMemory(fsName, sizeof(fsName));

  if (DokanOperations->GetVolumeInformation) {
    status = DokanOperations->GetVolumeInformation(
        volumeName,                         // VolumeNameBuffer
        sizeof(volumeName) / sizeof(WCHAR), // VolumeNameSize
        &volumeSerial,                      // VolumeSerialNumber
        &maxComLength,                      // MaximumComponentLength
        &fsFlags,                           // FileSystemFlags
        fsName,                             // FileSystemNameBuffer
        sizeof(fsName) / sizeof(WCHAR),     // FileSystemNameSize
        FileInfo);
  }

  if (status == STATUS_NOT_IMPLEMENTED) {
    status = DokanGetVolumeInformation(
        volumeName,                         // VolumeNameBuffer
        sizeof(volumeName) / sizeof(WCHAR), // VolumeNameSize
        &volumeSerial,                      // VolumeSerialNumber
        &maxComLength,                      // MaximumComponentLength
        &fsFlags,                           // FileSystemFlags
        fsName,                             // FileSystemNameBuffer
        sizeof(fsName) / sizeof(WCHAR),     // FileSystemNameSize
        FileInfo);
  }

  if (status != STATUS_SUCCESS) {
    return status;
  }

  volumeInfo->VolumeCreationTime.QuadPart = 0;
  volumeInfo->VolumeSerialNumber = volumeSerial;
  volumeInfo->SupportsObjects = FALSE;

  remainingLength -= FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel[0]);

  bytesToCopy = (ULONG)wcslen(volumeName) * sizeof(WCHAR);
  if (remainingLength < bytesToCopy) {
    bytesToCopy = remainingLength;
  }

  volumeInfo->VolumeLabelLength = bytesToCopy;
  RtlCopyMemory(volumeInfo->VolumeLabel, volumeName, bytesToCopy);
  remainingLength -= bytesToCopy;

  EventInfo->BufferLength =
      EventContext->Operation.Volume.BufferLength - remainingLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFsSizeInformation(PEVENT_INFORMATION EventInfo,
                       PEVENT_CONTEXT EventContext, PDOKAN_FILE_INFO FileInfo,
                       PDOKAN_OPERATIONS DokanOperations) {
  ULONGLONG freeBytesAvailable = 0;
  ULONGLONG totalBytes = 0;
  ULONGLONG freeBytes = 0;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  ULONG allocationUnitSize = FileInfo->DokanOptions->AllocationUnitSize;
  ULONG sectorSize = FileInfo->DokanOptions->SectorSize;

  PFILE_FS_SIZE_INFORMATION sizeInfo =
      (PFILE_FS_SIZE_INFORMATION)EventInfo->Buffer;

  if (EventContext->Operation.Volume.BufferLength <
      sizeof(FILE_FS_SIZE_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  if (DokanOperations->GetDiskFreeSpace) {
    status = DokanOperations->GetDiskFreeSpace(
        &freeBytesAvailable, // FreeBytesAvailable
        &totalBytes,         // TotalNumberOfBytes
        &freeBytes,          // TotalNumberOfFreeBytes
        FileInfo);
  }

  if (status == STATUS_NOT_IMPLEMENTED) {
    status = DokanGetDiskFreeSpace(&freeBytesAvailable, // FreeBytesAvailable
                                   &totalBytes,         // TotalNumberOfBytes
                                   &freeBytes, // TotalNumberOfFreeBytes
                                   FileInfo);
  }

  if (status != STATUS_SUCCESS) {
    return status;
  }

  sizeInfo->TotalAllocationUnits.QuadPart =
      totalBytes / allocationUnitSize;
  sizeInfo->AvailableAllocationUnits.QuadPart =
      freeBytesAvailable / allocationUnitSize;
  sizeInfo->SectorsPerAllocationUnit =
	  allocationUnitSize / sectorSize;
  sizeInfo->BytesPerSector = sectorSize;

  EventInfo->BufferLength = sizeof(FILE_FS_SIZE_INFORMATION);

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFsAttributeInformation(PEVENT_INFORMATION EventInfo,
                            PEVENT_CONTEXT EventContext,
                            PDOKAN_FILE_INFO FileInfo,
                            PDOKAN_OPERATIONS DokanOperations) {
  WCHAR volumeName[MAX_PATH];
  DWORD volumeSerial;
  DWORD maxComLength = 0;
  DWORD fsFlags = 0;
  WCHAR fsName[MAX_PATH];
  ULONG remainingLength;
  ULONG bytesToCopy;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  PFILE_FS_ATTRIBUTE_INFORMATION attrInfo =
      (PFILE_FS_ATTRIBUTE_INFORMATION)EventInfo->Buffer;

  remainingLength = EventContext->Operation.Volume.BufferLength;

  if (remainingLength < sizeof(FILE_FS_ATTRIBUTE_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  RtlZeroMemory(volumeName, sizeof(volumeName));
  RtlZeroMemory(fsName, sizeof(fsName));

  if (DokanOperations->GetVolumeInformation) {
    status = DokanOperations->GetVolumeInformation(
        volumeName,                         // VolumeNameBuffer
        sizeof(volumeName) / sizeof(WCHAR), // VolumeNameSize
        &volumeSerial,                      // VolumeSerialNumber
        &maxComLength,                      // MaximumComponentLength
        &fsFlags,                           // FileSystemFlags
        fsName,                             // FileSystemNameBuffer
        sizeof(fsName) / sizeof(WCHAR),     // FileSystemNameSize
        FileInfo);
  }

  if (status == STATUS_NOT_IMPLEMENTED) {
    status = DokanGetVolumeInformation(
        volumeName,                         // VolumeNameBuffer
        sizeof(volumeName) / sizeof(WCHAR), // VolumeNameSize
        &volumeSerial,                      // VolumeSerialNumber
        &maxComLength,                      // MaximumComponentLength
        &fsFlags,                           // FileSystemFlags
        fsName,                             // FileSystemNameBuffer
        sizeof(fsName) / sizeof(WCHAR),     // FileSystemNameSize
        FileInfo);
  }

  if (status != STATUS_SUCCESS) {
    return status;
  }

  attrInfo->FileSystemAttributes = fsFlags;
  attrInfo->MaximumComponentNameLength = maxComLength;

  remainingLength -=
      FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName[0]);

  bytesToCopy = (ULONG)wcslen(fsName) * sizeof(WCHAR);
  if (remainingLength < bytesToCopy) {
    bytesToCopy = remainingLength;
  }

  attrInfo->FileSystemNameLength = bytesToCopy;
  RtlCopyMemory(attrInfo->FileSystemName, fsName, bytesToCopy);
  remainingLength -= bytesToCopy;

  EventInfo->BufferLength =
      EventContext->Operation.Volume.BufferLength - remainingLength;

  return STATUS_SUCCESS;
}

NTSTATUS
DokanFsFullSizeInformation(PEVENT_INFORMATION EventInfo,
                           PEVENT_CONTEXT EventContext,
                           PDOKAN_FILE_INFO FileInfo,
                           PDOKAN_OPERATIONS DokanOperations) {
  ULONGLONG freeBytesAvailable = 0;
  ULONGLONG totalBytes = 0;
  ULONGLONG freeBytes = 0;
  NTSTATUS status = STATUS_NOT_IMPLEMENTED;

  ULONG allocationUnitSize = FileInfo->DokanOptions->AllocationUnitSize;
  ULONG sectorSize = FileInfo->DokanOptions->SectorSize;

  PFILE_FS_FULL_SIZE_INFORMATION sizeInfo =
      (PFILE_FS_FULL_SIZE_INFORMATION)EventInfo->Buffer;

  if (EventContext->Operation.Volume.BufferLength <
      sizeof(FILE_FS_FULL_SIZE_INFORMATION)) {
    return STATUS_BUFFER_OVERFLOW;
  }

  if (DokanOperations->GetDiskFreeSpace) {
    status = DokanOperations->GetDiskFreeSpace(
        &freeBytesAvailable, // FreeBytesAvailable
        &totalBytes,         // TotalNumberOfBytes
        &freeBytes,          // TotalNumberOfFreeBytes
        FileInfo);
  }

  if (status == STATUS_NOT_IMPLEMENTED) {
    status = DokanGetDiskFreeSpace(&freeBytesAvailable, // FreeBytesAvailable
                                   &totalBytes,         // TotalNumberOfBytes
                                   &freeBytes, // TotalNumberOfFreeBytes
                                   FileInfo);
  }

  if (status != STATUS_SUCCESS) {
    return status;
  }

  sizeInfo->TotalAllocationUnits.QuadPart =
      totalBytes / allocationUnitSize;
  sizeInfo->ActualAvailableAllocationUnits.QuadPart =
      freeBytes / allocationUnitSize;
  sizeInfo->CallerAvailableAllocationUnits.QuadPart =
      freeBytesAvailable / allocationUnitSize;
  sizeInfo->SectorsPerAllocationUnit =
	  allocationUnitSize / sectorSize;
  sizeInfo->BytesPerSector = sectorSize;

  EventInfo->BufferLength = sizeof(FILE_FS_FULL_SIZE_INFORMATION);

  return STATUS_SUCCESS;
}

VOID DispatchQueryVolumeInformation(HANDLE Handle, PEVENT_CONTEXT EventContext,
                                    PDOKAN_INSTANCE DokanInstance) {
  PEVENT_INFORMATION eventInfo;
  DOKAN_FILE_INFO fileInfo;
  PDOKAN_OPEN_INFO openInfo;
  ULONG sizeOfEventInfo = sizeof(EVENT_INFORMATION) - 8 +
                          EventContext->Operation.Volume.BufferLength;

  eventInfo = (PEVENT_INFORMATION)malloc(sizeOfEventInfo);
  if (eventInfo == NULL) {
    return;
  }

  RtlZeroMemory(eventInfo, sizeOfEventInfo);
  RtlZeroMemory(&fileInfo, sizeof(DOKAN_FILE_INFO));

  // There is no Context because file is not opened
  // so DispatchCommon is not used here
  openInfo = (PDOKAN_OPEN_INFO)(INT_PTR)EventContext->Context;

  eventInfo->BufferLength = 0;
  eventInfo->SerialNumber = EventContext->SerialNumber;

  fileInfo.ProcessId = EventContext->ProcessId;
  fileInfo.DokanOptions = DokanInstance->DokanOptions;

  eventInfo->Status = STATUS_NOT_IMPLEMENTED;
  eventInfo->BufferLength = 0;

  DbgPrint("###QueryVolumeInfo %04d\n",
           openInfo != NULL ? openInfo->EventId : -1);

  switch (EventContext->Operation.Volume.FsInformationClass) {
  case FileFsVolumeInformation:
    eventInfo->Status = DokanFsVolumeInformation(
        eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
    break;
  case FileFsSizeInformation:
    eventInfo->Status = DokanFsSizeInformation(
        eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
    break;
  case FileFsAttributeInformation:
    eventInfo->Status = DokanFsAttributeInformation(
        eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
    break;
  case FileFsFullSizeInformation:
    eventInfo->Status = DokanFsFullSizeInformation(
        eventInfo, EventContext, &fileInfo, DokanInstance->DokanOperations);
    break;
  default:
    DbgPrint("error unknown volume info %d\n",
             EventContext->Operation.Volume.FsInformationClass);
  }

  SendEventInformation(Handle, eventInfo, sizeOfEventInfo, NULL);
  free(eventInfo);
  return;
}
