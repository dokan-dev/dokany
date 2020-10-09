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

#ifndef MEMFS_H_
#define MEMFS_H_

#include <dokan/dokan.h>
#include <dokan/fileinfo.h>

#include "filenodes.h"
#include "memfs_operations.h"

#include <WinBase.h>
#include <iostream>

namespace memfs {
class memfs {
 public:
  memfs() = default;
  // Start the memory filesystem
  void run();
  // Unmount the device when destructor is called
  virtual ~memfs();

  // FileSystem mount options
  WCHAR mount_point[MAX_PATH] = L"M:\\";
  WCHAR unc_name[MAX_PATH] = L"";
  USHORT thread_number = 5;
  bool network_drive = false;
  bool removable_drive = false;
  bool current_session = false;
  bool debug_log = false;
  bool enable_network_unmount = false;
  ULONG timeout = 0;

  // FileSystem context runtime
  std::unique_ptr<fs_filenodes> fs_filenodes;
};
}  // namespace memfs

#endif  // MEMFS_H_