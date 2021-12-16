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

#ifndef FILENODES_H_
#define FILENODES_H_

#include "filenode.h"

#include <memory>
#include <mutex>

#include <optional>
#include <iostream>
#include <set>
#include <unordered_map>

namespace memfs {
// Memfs filenode storage
// There is only one instance of fs_filenodes per dokan mount
// as fs_filenodes describre the whole filesystem hierarchy context.
class fs_filenodes {
 public:
  fs_filenodes();

  // Add a new filenode to the filesystem hierarchy.
  // The file will directly be visible on the filesystem.
  // An already processed GetStreamNames can optional be provided
  NTSTATUS
  add(const std::shared_ptr<filenode> &filenode,
      std::optional<std::pair<std::wstring, std::wstring>> stream_names);

  // Return the filenode linked to the filename if present.
  std::shared_ptr<filenode> find(const std::wstring& filename);

  // Return all filenode of the directory scope give in param.
  std::set<std::shared_ptr<filenode>> list_folder(const std::wstring& filename);

  // Remove filenode from the filesystem hierarchy.
  // If the filenode has alternated streams attached, they will also be removed.
  // If the filenode is a directory not empty, all sub filenode will be removed
  // recursively.
  void remove(const std::wstring& filename);
  void remove(const std::shared_ptr<filenode>& filenode);

  // Move the current filenode position to the new one in the filesystem
  // hierarchy.
  NTSTATUS move(const std::wstring& old_filename,
                const std::wstring& new_filename, BOOL replace_if_existing);

  // Help - return a pair containing for example for \foo:bar
  // first: filename: foo
  // second: alternated stream name: bar
  // If the filename do not contain an alternated stream, second is empty.
  static std::pair<std::wstring, std::wstring> get_stream_names(
      std::wstring real_filename);

 private:
  // Global FS FileIndex count.
  // Note: Alternated stream and main stream share the same FileIndex.
  std::atomic<LONGLONG> _fs_fileindex_count = 1;

  // Mutex need to be aquired when using fileNodes / directoryPaths.
  std::recursive_mutex _filesnodes_mutex;
  // Global map of filename / filenode for all the filesystem.
  std::unordered_map<std::wstring, std::shared_ptr<filenode>> _filenodes;
  // Directory map of directoryname / sub filenodes in the scope.
  // A directory \foo with 2 files bar and coco will have one entry:
  // first: foo - second: set filenode { bar, coco }
  std::unordered_map<std::wstring, std::set<std::shared_ptr<filenode>>>
      _directoryPaths;
};
}  // namespace memfs

#endif // FILENODES_H_