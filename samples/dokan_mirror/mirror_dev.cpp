/*
Dokan : user-mode file system library for Windows

Copyright (C) 2015 - 2017 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

http://dokan-dev.github.io

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#include "../../dokan/dokan.h"
#include "../../dokan/fileinfo.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <winbase.h>
#include "mirror_dev.h"
#include <stdbool.h>
#include "../../makevhdx/VHD_VHDX_Base.h"

static wchar_t *g_MirrorDevDokanPathPrefix = L"\\MIRROR";
static size_t g_MirrorDevDokanPathPrefixLength = 8;
static wchar_t *g_MirrorDevDokanVhdPostfix = L".VHD";
static size_t g_MirrorDevDokanVhdPostfixLength = 4;
static wchar_t *g_MirrorDevDokanVhdxPostfix = L".VHDX";
static size_t g_MirrorDevDokanVhdxPostfixLength = 5;


/**
* Use a fixed offset with the disk - to experiment with exporting a partition only.
* I've found Windows partitions typically start at offset 0x100000
*/
#define FIXED_OFFSET 0


struct MirrorDevData
{
private:
  /**
  * The device file type, either raw disk image or vhd
  */
  enum MirrorDevFileType type;
public:
  /**
  * The size of the underlying physical device
  * to mirror in blocks
  */
  LONGLONG            devSizeInBlocks;
  /**
  * The handle to the physical device being mirrored
  */
  HANDLE mirrorDevPhysDevHandle;
  /**
  * The mutex to hold when seeking/reading on the mirror device
  */
  HANDLE mirrorDevAccessMutex;

  std::vector<BYTE> header;
  std::vector<BYTE> footer;

  wchar_t mirrorDevDokanPathBuffer[8 + 5 + 1];

  wchar_t *GetDokanMirrorDevFilePath()
  {
    if (mirrorDevDokanPathBuffer[0] == 0) {
      memset(&mirrorDevDokanPathBuffer[0], 0, sizeof(mirrorDevDokanPathBuffer));
      wcsncpy_s(&mirrorDevDokanPathBuffer[0], sizeof(mirrorDevDokanPathBuffer) / sizeof(WCHAR), &g_MirrorDevDokanPathPrefix[0], g_MirrorDevDokanPathPrefixLength);
      if (type == MirrorDevTypeVHD) {
        wcsncat_s(&mirrorDevDokanPathBuffer[0], sizeof(mirrorDevDokanPathBuffer) / sizeof(WCHAR), &g_MirrorDevDokanVhdPostfix[0], g_MirrorDevDokanVhdPostfixLength);
      }
      else if (type == MirrorDevTypeVHDX) {
        wcsncat_s(&mirrorDevDokanPathBuffer[0], sizeof(mirrorDevDokanPathBuffer) / sizeof(WCHAR), &g_MirrorDevDokanVhdxPostfix[0], g_MirrorDevDokanVhdxPostfixLength);
      }
    }
    return &mirrorDevDokanPathBuffer[0];
  }
  
  size_t GetDokanMirrorDevFilePathLength()
  {
    return wcsnlen_s(GetDokanMirrorDevFilePath(), sizeof(mirrorDevDokanPathBuffer) / sizeof(WCHAR));
  }

  size_t GetHeaderSizeBlocks() {
    return header.size() / MirrorDevBlockSize();
  }

  size_t GetFooterSizeBlocks() {
    return footer.size() / MirrorDevBlockSize();
  }
  MirrorDevData() :
    type(MirrorDevTypeRawFile),
    devSizeInBlocks(0),
    mirrorDevPhysDevHandle(NULL)
  {
    memset(&mirrorDevDokanPathBuffer, 0, sizeof(mirrorDevDokanPathBuffer));
    GetDokanMirrorDevFilePath();
    mirrorDevAccessMutex=CreateMutex(NULL, FALSE, NULL);
  }

  ~MirrorDevData()
  {
    CloseHandle(mirrorDevAccessMutex);
  }

  void SetType(enum MirrorDevFileType newType)
  {
    type = newType;
    mirrorDevDokanPathBuffer[0] = 0;
    GetDokanMirrorDevFilePath();
  }

#define MAX_BLOCK_SIZE 512
  DWORD MirrorDevBlockSize()
  {
    /**
    * TODO: actually look this up from device geometry
    */
    return MAX_BLOCK_SIZE;
  }


};

struct MirrorDevData g_MirrorDevData;


HANDLE GetMirrorDevHandle()
{
  return g_MirrorDevData.mirrorDevPhysDevHandle;
}

struct MirrorDevData &GetMirrorDevData()
{
  return g_MirrorDevData;
}

bool MirrorDevFileNameMatchesExpected(LPCWSTR FileName)
{
  size_t fileNameLength = wcslen(FileName);
  if (fileNameLength != GetMirrorDevData().GetDokanMirrorDevFilePathLength() ||
    (wcsncmp(FileName,
      GetMirrorDevData().GetDokanMirrorDevFilePath(),
      GetMirrorDevData().GetDokanMirrorDevFilePathLength()) != 0))
  {
    return false;
  }
  return true;
}


static NTSTATUS DOKAN_CALLBACK
MirrorDevCreateFile(LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext,
	ACCESS_MASK DesiredAccess, ULONG FileAttributes,
	ULONG ShareAccess, ULONG CreateDisposition,
	ULONG CreateOptions, PDOKAN_FILE_INFO DokanFileInfo) {
  DbgPrint(L"DevCreateFile : %s\n", FileName);
  DWORD creationDisposition;
  DWORD fileAttributesAndFlags;
  ACCESS_MASK genericDesiredAccess;

  DbgPrint(L"MirrorDevCreateFile %s Share Mode 0x%x\n", FileName, ShareAccess);
  /**
  * TODO: figure out how to use this
  */
  SecurityContext = SecurityContext;

  DokanMapKernelToUserCreateFileFlags(
    DesiredAccess, FileAttributes, CreateOptions, CreateDisposition,
    &genericDesiredAccess, &fileAttributesAndFlags, &creationDisposition);

  if (FileName == NULL)
  {
    DbgPrint(
      L"%s: invalid path.\n",
      FileName);
    return -ERROR_BAD_ARGUMENTS;
  }
  if ((genericDesiredAccess & GENERIC_WRITE) != 0)
  {
    return(-ERROR_WRITE_PROTECT);
  }
  /* Ignore the share_mode
  */
  if (creationDisposition == CREATE_NEW)
  {
    return(-ERROR_FILE_EXISTS);
  }
  else if (creationDisposition == CREATE_ALWAYS)
  {
    return(-ERROR_ALREADY_EXISTS);
  }
  else if (creationDisposition == OPEN_ALWAYS)
  {
    return(-ERROR_FILE_NOT_FOUND);
  }
  else if (creationDisposition == TRUNCATE_EXISTING)
  {
    return(-ERROR_FILE_NOT_FOUND);
  }
  else if (creationDisposition != OPEN_EXISTING)
  {
    DbgPrint(
      L"invalid creation disposition 0x%x.\n",
      creationDisposition);
    return(-ERROR_BAD_ARGUMENTS);
  }
  if (DokanFileInfo == NULL)
  {
    DbgPrint(L"Null file info\n");
    return(-ERROR_BAD_ARGUMENTS);
  }
  size_t fileNameLength = wcslen(FileName);
  if (fileNameLength == 1)
  {
    if (FileName[0] != (wchar_t) '\\')
    {
      return -ERROR_FILE_NOT_FOUND;
    }
  }
  else
  {
    if( !MirrorDevFileNameMatchesExpected(FileName) )
    {
      DbgPrint(L"Invalid file name %s\n", FileName);
      return -ERROR_FILE_NOT_FOUND;
    }
  }
  return NO_ERROR;

}

static void DOKAN_CALLBACK MirrorDevCloseFile(LPCWSTR FileName,
  PDOKAN_FILE_INFO DokanFileInfo) {
  if (DokanFileInfo->Context) {
    DbgPrint(L"CloseFile: %s\n", FileName);
    DbgPrint(L"\terror : not cleanuped file\n\n");
    CloseHandle((HANDLE)DokanFileInfo->Context);
    DokanFileInfo->Context = 0;
  }
  else {
    DbgPrint(L"Close: %s\n\n", FileName);
  }
}



bool GetMirrorDevAccessMutex(struct MirrorDevData *data)
{
  DWORD dwWaitResult;
  dwWaitResult = WaitForSingleObject(
    data->mirrorDevAccessMutex,    // handle to mutex
    INFINITE);  // no time-out interval

  return dwWaitResult == WAIT_OBJECT_0;
}

void ReleaseMirrorDevAccessMutex(struct MirrorDevData *data)
{
  ReleaseMutex(data->mirrorDevAccessMutex);
}
/**
* Read @param LengthInBlocks blocks at @param BlockOffset blocks offset of the mirror device into @param Buffer.
* Also handles read of vhd footer when attempts to read maxBlockOffset + 1 are issued.
* @return true on success, false on failure.
*/
bool MirrorDevReadAlignedBlocks(LONGLONG BlockOffset, LPVOID Buffer, DWORD LengthInBlocks)
{
  DWORD bytesReadThisCall;
  bool success = false;
  if ( LengthInBlocks > 0 && BlockOffset >= 0 )
  {
    DbgPrint(L"Read %llu blocks at block offset %llu (0x%llx)\n", (unsigned long long)LengthInBlocks, (unsigned long long)BlockOffset, (unsigned long long)BlockOffset);
    DWORD FullReadLength = LengthInBlocks * GetMirrorDevData().MirrorDevBlockSize();
    size_t ReadLengthRemaining = FullReadLength;
    LONGLONG DeviceStartBlockOffset = BlockOffset;
    BYTE *BufferWriteLocation = (BYTE*)Buffer;
    if (GetMirrorDevData().GetHeaderSizeBlocks() != 0 &&
      BlockOffset >= (LONGLONG)GetMirrorDevData().GetHeaderSizeBlocks()) {
      DeviceStartBlockOffset = BlockOffset - GetMirrorDevData().GetHeaderSizeBlocks();
      DbgPrint(L"Shift device block offset to %llu (0x%llx) to account for %llu header blocks\n", 
        (unsigned long long)DeviceStartBlockOffset, 
        (unsigned long long)DeviceStartBlockOffset,(unsigned long long)GetMirrorDevData().GetHeaderSizeBlocks());
    }
    else if(GetMirrorDevData().GetHeaderSizeBlocks() != 0) {
      size_t HeaderReadLenBlocks = min(LengthInBlocks, GetMirrorDevData().GetHeaderSizeBlocks());
      size_t HeaderByteOffset = BlockOffset * GetMirrorDevData().MirrorDevBlockSize();
      memcpy(&((UINT8*)Buffer)[0], &GetMirrorDevData().header.data()[HeaderByteOffset],HeaderReadLenBlocks*GetMirrorDevData().MirrorDevBlockSize());
      BlockOffset += HeaderReadLenBlocks;
      DeviceStartBlockOffset = 0;
      ReadLengthRemaining -= HeaderReadLenBlocks * GetMirrorDevData().MirrorDevBlockSize();
      if (ReadLengthRemaining == 0) {
        success = true;
      }
      DbgPrint(L"Copied %llu header blocks into the buffer starting at byte offset %llu, %llu remaining bytes to be read starting at device block 0\n", 
              (unsigned long long) HeaderReadLenBlocks, HeaderByteOffset, (unsigned long long)ReadLengthRemaining);
      BufferWriteLocation += HeaderReadLenBlocks * GetMirrorDevData().MirrorDevBlockSize();
    }
    if (ReadLengthRemaining != 0) {
      LONGLONG DeviceLastBlock = DeviceStartBlockOffset + (ReadLengthRemaining/GetMirrorDevData().MirrorDevBlockSize())-1;
      if (DeviceLastBlock >= GetMirrorDevData().devSizeInBlocks &&
          GetMirrorDevData().GetFooterSizeBlocks() > 0) {
        LONGLONG BufferOffsetToVhdFooter = (GetMirrorDevData().devSizeInBlocks - DeviceStartBlockOffset)*GetMirrorDevData().MirrorDevBlockSize();
        size_t FooterReadLenBytes = min(GetMirrorDevData().footer.size(), ReadLengthRemaining);
        /**
        * This is a read of the VHD footer, so copy it into the end of the buffer
        */
        memcpy(&((UINT8*)Buffer)[BufferOffsetToVhdFooter], GetMirrorDevData().footer.data(), FooterReadLenBytes);
        ReadLengthRemaining -= FooterReadLenBytes;
        DbgPrint(L"Write VHD footer to buffer in last %llu bytes starting at offset %llu\n", (unsigned long long)FooterReadLenBytes, (unsigned long long)BufferOffsetToVhdFooter);
        if (ReadLengthRemaining == 0) {
          success = true;
        }
      }
      if (ReadLengthRemaining > 0) {
        LARGE_INTEGER liOffset;
        liOffset.QuadPart = DeviceStartBlockOffset * GetMirrorDevData().MirrorDevBlockSize();
        if (GetMirrorDevAccessMutex(&g_MirrorDevData))
        {
          success = SetFilePointerEx(GetMirrorDevHandle(), liOffset, NULL, FILE_BEGIN);
          if (!success) {
            DbgPrint(L"Seek to offset %llu failed with error %d\n", liOffset.QuadPart, GetLastError());
          }
          else {
            success = ReadFile(GetMirrorDevHandle(), BufferWriteLocation, (DWORD) ReadLengthRemaining, &bytesReadThisCall, NULL);
            if (!success) {
              DbgPrint(L"ReadFile failed with error %d reading %llu bytes at offset %llu bytes\n", GetLastError(), (unsigned long long)ReadLengthRemaining, liOffset.QuadPart);
            }
          }
          ReleaseMirrorDevAccessMutex(&g_MirrorDevData);
        }
      }
    }
  }
  return success;
}
bool MirrorDevReadAligned(LONGLONG BlockOffset, DWORD FirstBlockOffset, LPVOID Buffer, DWORD BufferLength, LPDWORD BytesReadReturn )
{
  const DWORD blockSize = GetMirrorDevData().MirrorDevBlockSize();
  UCHAR tempBuffer[MAX_BLOCK_SIZE];
  PUCHAR currentBufferOffset = (PUCHAR)Buffer;
  DWORD remainingBytes = BufferLength;
  bool success = true;
  if ( FirstBlockOffset != 0 || remainingBytes <= blockSize )
  {
    DWORD firstBlockUsedBytes = min(BufferLength,blockSize- FirstBlockOffset);
    DbgPrint(L"Reading one block for copy of %d first block bytes\n", firstBlockUsedBytes);
    success = MirrorDevReadAlignedBlocks(BlockOffset,tempBuffer, 1);
    if( !success )
    {
      DbgPrint(L"MirrorDevReadAlignedBlocks failed reading %d bytes for first block offset %d\n", GetLastError(),blockSize,FirstBlockOffset);
    }
    else
    {
      DbgPrint(L"Read %lu aligned bytes, copied %d into buffer starting at offset %d\n", blockSize, firstBlockUsedBytes, FirstBlockOffset);
      memcpy(currentBufferOffset, &tempBuffer[FirstBlockOffset], firstBlockUsedBytes);
      currentBufferOffset += firstBlockUsedBytes;
      remainingBytes -= firstBlockUsedBytes;
      BlockOffset++;
      if (remainingBytes != 0 && remainingBytes < blockSize)
      {
        success = MirrorDevReadAlignedBlocks(BlockOffset, tempBuffer, 1);
        if (success)
        {
          DbgPrint(L"Read %lu aligned bytes, copied %d into buffer\n", blockSize, remainingBytes);
          memcpy(currentBufferOffset, &tempBuffer[0], remainingBytes);
          remainingBytes = 0;
        }
      }
    }
  }
  if (success)
  {
    if (remainingBytes >= blockSize)
    {
      DWORD alignedBlocksRead = remainingBytes/blockSize;
      DWORD alignedBytesRead = blockSize*alignedBlocksRead;
      success = MirrorDevReadAlignedBlocks(BlockOffset,currentBufferOffset, alignedBlocksRead);
      if (!success)
      {
        DbgPrint(L"MirrorDevReadAlignedBlocks failed reading aligned payload %lu bytes\n", alignedBytesRead);
      }
      else
      {
        DbgPrint(L"Read %lu aligned blocks into buffer\n", alignedBlocksRead);
      }
      currentBufferOffset += alignedBytesRead;
      remainingBytes -= alignedBytesRead;
      BlockOffset += (alignedBytesRead / blockSize);
      if (success && remainingBytes != 0)
      {
        success = MirrorDevReadAlignedBlocks(BlockOffset, tempBuffer, 1);
        if (!success)
        {
          DbgPrint(L"ReadFile failed with error %d reading remainder payload %d bytes\n", GetLastError(), blockSize);
        }
        else
        {
          memcpy(currentBufferOffset, &tempBuffer[0], remainingBytes);
          DbgPrint(L"Read %lu bytes,  copied %u into buffer \n", blockSize, remainingBytes);
        }
      }
    }
  }
  if (success)
  {
    *BytesReadReturn = BufferLength;
  }
  return success;
}

int GetMirrorBackingDevSize(LARGE_INTEGER *size)
{
  DISK_GEOMETRY_EX geo;
  DWORD returned;
  if (!DeviceIoControl(
    GetMirrorDevHandle(),
    IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
    NULL,
    0,
    &geo,
    sizeof(DISK_GEOMETRY_EX),
    &returned,
    NULL))
  {
    DbgPrint(L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX returned %d\n", GetLastError());
    return -ERROR_SEEK_ON_DEVICE;
  }
  else
  {
    geo.DiskSize.QuadPart -= FIXED_OFFSET;
    *size = geo.DiskSize;
    DbgPrint(L"Mirror backing disk size %llu (0x%llx) bytes\n", size->QuadPart, size->QuadPart );
  }

  return NO_ERROR;
}

int GetMirrorDevSize(LARGE_INTEGER *size)
{
  int rc = NO_ERROR;
  size->QuadPart = GetMirrorDevData().devSizeInBlocks * GetMirrorDevData().MirrorDevBlockSize();
  size->QuadPart += GetMirrorDevData().footer.size();
  size->QuadPart += GetMirrorDevData().header.size();
  DbgPrint(L"Mirror dev size %llu (0x%llx) bytes", size->QuadPart, size->QuadPart);
  return rc;
}

static NTSTATUS DOKAN_CALLBACK MirrorDevReadFile(LPCWSTR FileName, LPVOID Buffer,
  DWORD BufferLength,
  LPDWORD ReadLength,
  LONGLONG Offset,
  PDOKAN_FILE_INFO DokanFileInfo) {
  NTSTATUS rc =-ERROR_GENERIC_COMMAND_FAILED;
  Offset += FIXED_OFFSET;
  if (Offset < 0 ) {
    LARGE_INTEGER devSize;
    GetMirrorDevSize(&devSize);
    DbgPrint(L"MirrorDevReadFile recieved negative offset 0x%llx\n", Offset);
    Offset = devSize.QuadPart + Offset;
  }
  DbgPrint(L"MirrorDevReadFile %s length %lu offset %llu (0x%llx)\n", FileName, BufferLength, Offset,Offset);
  DokanFileInfo = DokanFileInfo;
  if (!MirrorDevFileNameMatchesExpected(FileName))
  {
    DbgPrint(L"Invalid file name %s\n", FileName);
    rc=-ERROR_FILE_NOT_FOUND;
  }
  else
  {
    DWORD offsetRemainder;
    DWORD blockSize = GetMirrorDevData().MirrorDevBlockSize();
    offsetRemainder = Offset % blockSize;

    if (MirrorDevReadAligned(Offset/ blockSize,offsetRemainder,Buffer, BufferLength, ReadLength))
    {
      DbgPrint(L"Read %lu bytes from device, first first 4 bytes 0x%02x%02x%02x%02x\n\n",
        *ReadLength, ((UCHAR *)Buffer)[0], ((UCHAR *)Buffer)[1], ((UCHAR *)Buffer)[2], ((UCHAR *)Buffer)[3]);
      rc = NO_ERROR;
    }
    else
    {
      DbgPrint(L"ReadFile failed with error %d\n", GetLastError());
      rc = -ERROR_READ_FAULT;
    }
  }
  return rc;
}




static NTSTATUS DOKAN_CALLBACK MirrorDevGetFileInformation(
  LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation,
  PDOKAN_FILE_INFO DokanFileInfo) {
  DbgPrint(L"MirrorDevGetFileInformation %s\n", FileName);
  size_t fileNameLength = wcslen(FileName);
  DokanFileInfo = DokanFileInfo;
  if (fileNameLength == 1)
  {
    if (FileName[0] != (wchar_t) '\\')
    {
      return -ERROR_FILE_NOT_FOUND;
    }
    HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    /* TODO set timestamps
    HandleFileInformation->ftCreationTime   = { 0, 0 };
    HandleFileInformation->ftLastAccessTime = { 0, 0 };
    HandleFileInformation->ftLastWriteTime  = { 0, 0 };
    */
  }
  else
  {
    if (!MirrorDevFileNameMatchesExpected(FileName))
    {
      DbgPrint(L"Invalid file name %s\n", FileName);
      return -ERROR_FILE_NOT_FOUND;
    }
    LARGE_INTEGER size;
    int rc = GetMirrorDevSize(&size);
    if( rc != 0 )
    {
      return rc;
    }
    HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    /* TODO set timestamps
    HandleFileInformation->ftCreationTime   = { 0, 0 };
    HandleFileInformation->ftLastAccessTime = { 0, 0 };
    HandleFileInformation->ftLastWriteTime  = { 0, 0 };
    */
    HandleFileInformation->nFileSizeHigh = size.HighPart;
    HandleFileInformation->nFileSizeLow = size.LowPart;
    DbgPrint(L"Mirror file size low part 0x%x high part 0x%x quad part %llu\n", size.LowPart, size.HighPart, size.QuadPart);
  }
  return NO_ERROR;
}

static NTSTATUS DOKAN_CALLBACK
MirrorDevFindFiles(LPCWSTR FileName,
  PFillFindData FillFindData, // function pointer
  PDOKAN_FILE_INFO DokanFileInfo) {
  WIN32_FIND_DATAW findData;
  DbgPrint(L"MirrorDevFindFiles %s\n",FileName);
  memset(&findData, 0, sizeof(WIN32_FIND_DATAW));
  size_t fileNameLength = wcslen(FileName);
  if ( (fileNameLength != 1) || 
     (FileName[0] != (wchar_t) '\\') )
  {
    DbgPrint(L"Invalid Filename for FindFiles %s\n", FileName);
    return -ERROR_FILE_NOT_FOUND;
  }
  wcsncpy_s(findData.cFileName, sizeof(findData.cFileName)/sizeof(WCHAR), L".", 1);
  wcsncpy_s(findData.cAlternateFileName, sizeof(findData.cAlternateFileName) / sizeof(WCHAR),  L".", 1);
  findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
  /* TODO set timestamps
  findData.ftCreationTime   = { 0, 0 };
  findData.ftLastAccessTime = { 0, 0 };
  findData.ftLastWriteTime  = { 0, 0 };
  */
  int rc= FillFindData(
    &findData,
    DokanFileInfo);
  if( rc != 0)
  {
    DbgPrint(L"FillFindData returned %d on . entry\n", rc);
    return -ERROR_GEN_FAILURE;
  }
  memset(&findData, 0, sizeof(WIN32_FIND_DATAW));
  wcsncpy_s(findData.cFileName, sizeof(findData.cFileName) / sizeof(WCHAR), L"..", 1);
  wcsncpy_s(findData.cAlternateFileName, sizeof(findData.cAlternateFileName) / sizeof(WCHAR), L"..", 1);
  findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
  /* TODO set timestamps
  findData.ftCreationTime   = { 0, 0 };
  findData.ftLastAccessTime = { 0, 0 };
  findData.ftLastWriteTime  = { 0, 0 };
  */
  rc = FillFindData(
    &findData,
    DokanFileInfo);
  if (rc != 0)
  {
    DbgPrint(L"FillFindData returned %d on .. entry\n", rc);
    return -ERROR_GEN_FAILURE;
  }
  memset(&findData, 0, sizeof(WIN32_FIND_DATAW));
  wcsncpy_s(findData.cFileName, sizeof(findData.cFileName)/sizeof(WCHAR), &(GetMirrorDevData().GetDokanMirrorDevFilePath()[1]), GetMirrorDevData().GetDokanMirrorDevFilePathLength() - 1);
  wcsncpy_s(findData.cAlternateFileName, sizeof(findData.cAlternateFileName) / sizeof(WCHAR), &(GetMirrorDevData().GetDokanMirrorDevFilePath()[1]), GetMirrorDevData().GetDokanMirrorDevFilePathLength() - 1);
  findData.dwFileAttributes = FILE_ATTRIBUTE_READONLY;

  LARGE_INTEGER size;
  rc = GetMirrorDevSize(&size);
  if (rc != 0)
  {
    return rc;
  }
  findData.nFileSizeLow = size.LowPart;
  findData.nFileSizeHigh = size.HighPart;
  rc = FillFindData(
    &findData,
    DokanFileInfo);
  if (rc != 0)
  {
    DbgPrint(L"FillFindData returned %d on mirror dev entry\n", rc);
    return -ERROR_GEN_FAILURE;
  }
  return NO_ERROR;

}

static NTSTATUS DOKAN_CALLBACK MirrorDevGetVolumeInformation(
  LPWSTR VolumeNameBuffer, DWORD VolumeNameSize, LPDWORD VolumeSerialNumber,
  LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
  LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize,
  PDOKAN_FILE_INFO DokanFileInfo) {
  DokanFileInfo = DokanFileInfo;
  DbgPrint(L"MirrorDevGetVolumeInformation\n");
  wcscpy_s(VolumeNameBuffer, VolumeNameSize, &(GetMirrorDevData().GetDokanMirrorDevFilePath()[1]));
  if (VolumeSerialNumber)
    *VolumeSerialNumber = 0x19831116;
  if (MaximumComponentLength)
    *MaximumComponentLength = 255;
  if (FileSystemFlags)
    *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
    FILE_UNICODE_ON_DISK |
    FILE_READ_ONLY_VOLUME;
  wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"Dokan");
  return NO_ERROR;
}
void MirrorDevFillOptions(PDOKAN_OPTIONS dokanOptions)
{
  DbgPrint(L"Forcing write protect since physical device implementation does not currently support reads\n");
  dokanOptions->Options |= DOKAN_OPTION_WRITE_PROTECT;
}
void MirrorDevFillOperations(PDOKAN_OPERATIONS dokanOperations)
{
  dokanOperations->ZwCreateFile = MirrorDevCreateFile;
  dokanOperations->CloseFile = MirrorDevCloseFile;
  dokanOperations->ReadFile = MirrorDevReadFile;
  dokanOperations->GetFileInformation = MirrorDevGetFileInformation;
  dokanOperations->FindFiles = MirrorDevFindFiles;
  dokanOperations->GetVolumeInformationW = MirrorDevGetVolumeInformation;
}

int MirrorDevInit(enum MirrorDevFileType type,
  LPWSTR PhysicalDrive, PDOKAN_OPTIONS Options, PDOKAN_OPERATIONS Operations)
{
  struct MirrorDevData &devData = g_MirrorDevData;
  int rc;
  devData.SetType(type);
  MirrorDevFillOptions(Options);
  MirrorDevFillOperations(Operations);
  devData.mirrorDevPhysDevHandle = CreateFile(PhysicalDrive,
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL, OPEN_EXISTING, 0, 0);

  if (devData.mirrorDevPhysDevHandle == INVALID_HANDLE_VALUE) {
    DbgPrint(L"Failed to open drive %s\n", PhysicalDrive);
    rc = -1;
  }
  else {
    LARGE_INTEGER size;
    rc = GetMirrorBackingDevSize(&size);
    if (rc == 0) {
      devData.devSizeInBlocks = size.QuadPart / devData.MirrorDevBlockSize();
      struct VHD_VHDX_IF *virt = NULL;

      if (type == MirrorDevTypeVHD) {
        virt = new VHD(512); // TODO: what should required alignment be for VHD?
      }
      else if (type == MirrorDevTypeVHDX) {
        virt = new VHDX();
      }
      if (virt != NULL) {
        FILE_END_OF_FILE_INFO eof_info;
        DbgPrint(L"Construct header for type %d\n", type);
        virt->ConstructHeader(size.QuadPart, 0, devData.MirrorDevBlockSize(), true, eof_info);
        DbgPrint(L"Write footer to buffer\n");
        virt->WriteFooterToBuffer(devData.footer);
        DbgPrint(L"Footer length %d bytes\n", devData.footer.size());
        DbgPrint(L"Write header to buffer\n");
        virt->WriteHeaderToBuffer(devData.header);
        DbgPrint(L"Header length %d bytes first 4 bytes 0x%02x%02x%02x%02x\n",
          devData.header.size(),
          devData.header.size() >= 4 ? devData.header[0] : 0,
          devData.header.size() >= 4 ? devData.header[1] : 0,
          devData.header.size() >= 4 ? devData.header[2] : 0,
          devData.header.size() >= 4 ? devData.header[3] : 0
          );


        delete virt;
        virt = NULL;
      }
    }
  }


  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

void MirrorDevTeardown()
{
  if (GetMirrorDevHandle() != INVALID_HANDLE_VALUE)
  {
    CloseHandle(GetMirrorDevHandle());
  }
}

/**
* Rename this wmain to test read operations in isolation.
* TODO: Should replace this with an actual test case
*/
#if MIRROR_DEV_MAIN
int __cdecl wmain2(ULONG argc, PWCHAR argv[]) {
  g_DebugMode = TRUE;
  g_UseStdErr = TRUE;
  g_MirrorDevHandle = CreateFile(L"\\\\.\\PhysicalDrive1",
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL, OPEN_EXISTING, 0, 0);

  if (g_MirrorDevHandle == INVALID_HANDLE_VALUE)
  {
    DbgPrint(L"CreateFile failed with error %d\n", GetLastError());
  }
  LONGLONG Offset = 528;
  DWORD offsetRemainder=0;
  DWORD ReadLengthDword;
  UCHAR Buffer[512];
  DWORD BufferLength = 512;
  DWORD ReadLength = BufferLength;

  if (MirrorDevSeekAligned(Offset, &offsetRemainder))
  {
    DbgPrint(L"Seek completed successfully to offset %u with remainder %u bytes\n", Offset, offsetRemainder);
    if (MirrorDevReadAligned(offsetRemainder, Buffer, ReadLength, &ReadLengthDword))
    {
      DbgPrint(L"Read %lu bytes from device, first first 4 bytes 0x%02x%02x%02x%02x\n\n",
        ReadLengthDword, ((UCHAR *)Buffer)[0], ((UCHAR *)Buffer)[1], ((UCHAR *)Buffer)[2], ((UCHAR *)Buffer)[3]);
    }
    else
    {
      DbgPrint(L"ReadFile failed with error %d\n", GetLastError());
    }
  }
  else
  {
    DbgPrint(L"Seek failed with error %d\n", GetLastError());
  }
  return 0;

}
#endif