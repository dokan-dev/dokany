/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2019 Adrien J. <liryna.stark@gmail.com>
  Copyright (C) 2020 Google, Inc.

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

#include "memfs.h"

#include <spdlog/spdlog.h>

namespace memfs {
void memfs::run() {
  fs_filenodes = std::make_unique<::memfs::fs_filenodes>();

  DOKAN_OPTIONS dokan_options;
  ZeroMemory(&dokan_options, sizeof(DOKAN_OPTIONS));
  dokan_options.Version = DOKAN_VERSION;
  dokan_options.Options = DOKAN_OPTION_ALT_STREAM | DOKAN_OPTION_CASE_SENSITIVE;
  dokan_options.MountPoint = mount_point;
  if (debug_log) {
    dokan_options.Options |= DOKAN_OPTION_STDERR | DOKAN_OPTION_DEBUG;
  } else {
    spdlog::set_level(spdlog::level::err);
  }
  // Mount type
  if (network_drive) {
    dokan_options.Options |= DOKAN_OPTION_NETWORK;
    if (unc_name[0]) {
      dokan_options.UNCName = unc_name;
    }
    if (enable_network_unmount) {
      dokan_options.Options |= DOKAN_OPTION_ENABLE_UNMOUNT_NETWORK_DRIVE;
    }
  } else if (removable_drive) {
    dokan_options.Options |= DOKAN_OPTION_REMOVABLE;
  } else {
    dokan_options.Options |= DOKAN_OPTION_MOUNT_MANAGER;
  }
  dokan_options.ThreadCount = thread_number;
  dokan_options.Timeout = timeout;
  dokan_options.GlobalContext = reinterpret_cast<ULONG64>(fs_filenodes.get());

  NTSTATUS status = DokanMain(&dokan_options, &memfs_operations);
  switch (status) {
    case DOKAN_SUCCESS:
      break;
    case DOKAN_ERROR:
      throw std::runtime_error("Error");
    case DOKAN_DRIVE_LETTER_ERROR:
      throw std::runtime_error("Bad Drive letter");
    case DOKAN_DRIVER_INSTALL_ERROR:
      throw std::runtime_error("Can't install driver");
    case DOKAN_START_ERROR:
      throw std::runtime_error("Driver something wrong");
    case DOKAN_MOUNT_ERROR:
      throw std::runtime_error("Can't assign a drive letter");
    case DOKAN_MOUNT_POINT_ERROR:
      throw std::runtime_error("Mount point error");
    case DOKAN_VERSION_ERROR:
      throw std::runtime_error("Version error");
    default:
      spdlog::error(L"DokanMain failed with {}", status);
      throw std::runtime_error("Unknown error"); // add error status
  }
}

memfs::~memfs() { DokanRemoveMountPoint(mount_point); }
}  // namespace memfs
