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
#include <stdio.h>
#include <stdlib.h>
#include <winbase.h>
#include <stdbool.h>

#include "vhd.h"
#include "mirror_dev.h"

#define VHD_SECTOR_SIZE 512


#define is_power_of_2(value)\
  (value && (value & (value - 1)) == 0)

const UINT64 VHD_COOKIE = 0x78697463656e6f63;
const UINT32 VHD_VERSION = 0x10000;
const UINT64 VHD_MAX_DISK_SIZE = 2040ULL * 1024 * 1024 * 1024;
const UINT32 VHD_TYPE_FIXED = 2;
const UINT64 VHD_INVALID_OFFSET = 0xFFFFFFFFFFFFFFFFULL;

struct VHD_FOOTER
{
  UINT64 Cookie;
  UINT32 Features;
  UINT32 FileFormatVersion;
  UINT64 DataOffset;
  UINT32 TimeStamp;
  UINT32 CreatorApplication;
  UINT32 CreatorVersion;
  UINT32 CreatorHostOS;
  UINT64 OriginalSize;
  UINT64 CurrentSize;
  UINT32 DiskGeometry;
  UINT32 DiskType;
  UINT32 Checksum;
  GUID   UniqueId;
  UINT8  SavedState;
  UINT8  Reserved[427];
};



static UINT8 *VHDToBigEndianByteBuffer(UINT8 *buffer, UINT64 value, UINT8 len_bytes)
{
  int i = 0;
  for (i = 0; i < len_bytes; i++) {
    *buffer = (UINT8) (value >> (64-((8 - (len_bytes-i-1)) * 8)));
    buffer++;
  }
  return buffer;
}

static void VHDFooterToBuffer(BYTE* buffer, const struct VHD_FOOTER *footer)
{
  BYTE *bufferOffs = buffer;
  memset(bufferOffs, 0, VHD_FOOTER_SIZE);
  memcpy(bufferOffs, &footer->Cookie, sizeof(footer->Cookie));
  bufferOffs += sizeof(footer->Cookie);
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->Features, sizeof(footer->Features));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->FileFormatVersion, sizeof(footer->FileFormatVersion));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->DataOffset, sizeof(footer->DataOffset));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->TimeStamp, sizeof(footer->TimeStamp));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->CreatorApplication, sizeof(footer->CreatorApplication));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->CreatorVersion, sizeof(footer->CreatorVersion));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->CreatorHostOS, sizeof(footer->CreatorHostOS));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->OriginalSize, sizeof(footer->OriginalSize));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->CurrentSize, sizeof(footer->CurrentSize));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->DiskGeometry, sizeof(footer->DiskGeometry));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->DiskType, sizeof(footer->DiskType));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->Checksum, sizeof(footer->Checksum));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->UniqueId.Data1, sizeof(footer->UniqueId.Data1));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->UniqueId.Data2, sizeof(footer->UniqueId.Data2));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->UniqueId.Data3, sizeof(footer->UniqueId.Data3));
  memcpy(bufferOffs, footer->UniqueId.Data4, sizeof(footer->UniqueId.Data4));
  bufferOffs += sizeof(footer->UniqueId.Data4);
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->UniqueId.Data3, sizeof(footer->UniqueId.Data3));
  bufferOffs = VHDToBigEndianByteBuffer(bufferOffs, footer->SavedState, sizeof(footer->SavedState));
}


UINT32 VHDChecksumUpdate(struct VHD_FOOTER* footer)
{
  footer->Checksum = 0;
  struct VHD_FOOTER_BUFFER driveFooter;
  VHDFooterToBuffer(&driveFooter.Byte[0], footer);
  UINT32 checksum = 0;
  for (UINT32 counter = 0; counter < sizeof(driveFooter.Byte); counter++)
  {
    checksum += driveFooter.Byte[counter];
  }
  return footer->Checksum = (~checksum);
}

UINT32 CHSCalculate(UINT64 disk_size)
{
  UINT64 totalSectors = disk_size / VHD_SECTOR_SIZE;
  UINT32 cylinderTimesHeads;
  UINT16 cylinders;
  UINT8  heads;
  UINT8  sectorsPerTrack;
  if (totalSectors > 65535 * 16 * 255)
  {
    totalSectors = 65535 * 16 * 255;
  }
  if (totalSectors >= 65535 * 16 * 63)
  {
    sectorsPerTrack = 255;
    heads = 16;
    cylinderTimesHeads = (UINT32)(totalSectors / sectorsPerTrack);
  }
  else
  {
    sectorsPerTrack = 17;
    cylinderTimesHeads = (UINT32)(totalSectors / sectorsPerTrack);
    heads = (UINT8)((cylinderTimesHeads + 1023) / 1024);
    if (heads < 4)
    {
      heads = 4;
    }
    if (cylinderTimesHeads >= (heads * 1024U) || heads > 16)
    {
      sectorsPerTrack = 31;
      heads = 16;
      cylinderTimesHeads = (UINT32)(totalSectors / sectorsPerTrack);
    }
    if (cylinderTimesHeads >= (heads * 1024U))
    {
      sectorsPerTrack = 63;
      heads = 16;
      cylinderTimesHeads = (UINT32)(totalSectors / sectorsPerTrack);
    }
  }
  cylinders = (UINT16)(cylinderTimesHeads / heads);
  return (cylinders << 16 | heads << 8 | sectorsPerTrack);
}

bool VHDFillFooter(struct VHD_FOOTER *footer, UINT64 disk_size)
{
  bool success = true;
  memset(footer, 0, sizeof(struct VHD_FOOTER));
  footer->Cookie = VHD_COOKIE;
  footer->Features = 2;
  footer->FileFormatVersion = VHD_VERSION;
  footer->DataOffset = VHD_INVALID_OFFSET;
  footer->OriginalSize = disk_size;
  footer->CurrentSize = disk_size;
  footer->DiskGeometry = CHSCalculate(disk_size);
  footer->DiskType = VHD_TYPE_FIXED;
  if (CoCreateGuid(&footer->UniqueId) != S_OK) {
    DbgPrint(L"Failed to create Guid");
    success = false;
  }
  VHDChecksumUpdate(footer);
  return success;
}


bool VHDFillFixedHeaderSector(struct VHD_FOOTER_BUFFER *buffer, UINT32 block_size, UINT64 disk_size)
{
  struct VHD_FOOTER vhd_footer;
  bool success = false;

  if (block_size == 0 || !is_power_of_2(block_size)) {
    DbgPrint(L"Invalid block size %d\n", block_size);
  }
  else if (VHDFillFooter(&vhd_footer, disk_size)) {
    VHDFooterToBuffer(&buffer->Byte[0], &vhd_footer);
    success = true;
  }

  return success;
}