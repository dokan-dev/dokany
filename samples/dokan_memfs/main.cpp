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

void show_usage() {
  // clang-format off
  spdlog::error("memfs.exe - Dokan Memory filesystem that can be mounted as a local or network drive.\n"
                "  /l MountPoint (ex. /l m)\t\t\t Mount point. Can be M:\\ (drive letter) or empty NTFS folder C:\\mount\\dokan .\n"
                "  /m (use removable drive)\t\t\t Show device as removable media.\n"
                "  /c (mount for current session only)\t\t Device only visible for current user session.\n"
                "  /n (use network drive)\t\t\t Show device as network device.\n"
                "  /u (UNC provider name ex. \\localhost\\myfs)\t UNC name used for network volume.\n"
                "  /t ThreadCount (ex. /t 5)\t\t\t Number of threads to be used internally by Dokan library.\n\t\t\t\t\t\t More threads will handle more event at the same time.\n"
                "  /d (enable debug output)\t\t\t Enable debug output to an attached debugger.\n"
                "  /i (Timeout in Milliseconds ex. /i 30000)\t Timeout until a running operation is aborted and the device is unmounted.\n"
                "  /x (network unmount)\t\t\t Allows unmounting network drive from file explorer\n"
                "Examples:\n"
                "\tmemfs.exe \t\t\t# Mount as a local filesystem into a drive of letter M:\\.\n"
                "\tmemfs.exe /l P:\t\t\t# Mount as a local filesystem into a drive of letter P:\\.\n"
                "\tmemfs.exe /l C:\\mount\\dokan\t# Mount into NTFS folder C:\\mount\\dokan.\n"
                "\tmemfs.exe /l M: /n /u \\myfs\\myfs1\t# Mount into a network drive M:\\. with UNC \\\\myfs\\myfs1\n\n"
                "Unmount the drive with CTRL + C in the console or alternatively via \"dokanctl /u MountPoint\".\n");
  // clang-format on
}

int __cdecl wmain(ULONG argc, PWCHAR argv[]) {
  try {
    auto dokan_memfs = std::make_shared<memfs::memfs>();
    // Parse arguments
    for (ULONG i = 1; i < argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"/h") {
        show_usage();
        return 0;
      } else if (arg == L"/n") {
        dokan_memfs->network_drive = true;
      } else if (arg == L"/m") {
        dokan_memfs->removable_drive = true;
      } else if (arg == L"/c") {
        dokan_memfs->current_session = true;
      } else if (arg == L"/d") {
        dokan_memfs->debug_log = true;
      } else if (arg == L"/x") {
        dokan_memfs->enable_network_unmount = true;
      } else {
        if (i + 1 >= argc) {
          show_usage();
          return 1;
        }
        std::wstring extra_arg = argv[++i];
        if (arg == L"/i") {
          dokan_memfs->timeout = std::stoul(extra_arg);
        } else if (arg == L"/l") {
          wcscpy_s(dokan_memfs->mount_point,
                   sizeof(dokan_memfs->mount_point) / sizeof(WCHAR),
                   extra_arg.c_str());
        } else if (arg == L"/u") {
          wcscpy_s(dokan_memfs->unc_name,
                   sizeof(dokan_memfs->unc_name) / sizeof(WCHAR),
                   extra_arg.c_str());
        } else if (arg == L"/t") {
          dokan_memfs->thread_number = std::stoi(extra_arg);
        }
      }
    }
    // Start the memory filesystem
    dokan_memfs->run();
  } catch (const std::exception& ex) {
    spdlog::error("dokan_memfs failure: {}", ex.what());
    return 1;
  }
  return 0;
}