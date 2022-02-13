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

#include "filenode.h"

#include <spdlog/spdlog.h>

namespace memfs {
filenode::filenode(const std::wstring& filename, bool is_directory,
                   DWORD file_attr,
                   const PDOKAN_IO_SECURITY_CONTEXT security_context)
    : is_directory(is_directory), attributes(file_attr), _fileName(filename) {
  // No lock need, FileNode is still not in a directory
  times.reset();

  if (security_context && security_context->AccessState.SecurityDescriptor) {
    spdlog::info(L"{} : Attach SecurityDescriptor", filename);
    security.SetDescriptor(security_context->AccessState.SecurityDescriptor);
  }
}

DWORD filenode::read(LPVOID buffer, DWORD bufferlength, LONGLONG offset) {
  std::shared_lock lock(_data_mutex);
  if (static_cast<size_t>(offset + bufferlength) > _data.size())
    bufferlength = (_data.size() > static_cast<size_t>(offset))
                       ? static_cast<DWORD>(_data.size() - offset)
                       : 0;
  if (bufferlength)
    memcpy(buffer, &_data[static_cast<size_t>(offset)], bufferlength);
  spdlog::info(L"Read {} : BufferLength {} Offset {}", get_filename(),
               bufferlength, offset);
  return bufferlength;
}

DWORD filenode::write(LPCVOID buffer, DWORD number_of_bytes_to_write,
                      LONGLONG offset) {
  if (!number_of_bytes_to_write) return 0;

  std::unique_lock lock(_data_mutex);
  if (static_cast<size_t>(offset + number_of_bytes_to_write) > _data.size())
    _data.resize(static_cast<size_t>(offset + number_of_bytes_to_write));

  spdlog::info(L"Write {} : NumberOfBytesToWrite {} Offset {}", get_filename(),
               number_of_bytes_to_write, offset);
  memcpy(&_data[static_cast<size_t>(offset)], buffer, number_of_bytes_to_write);
  return number_of_bytes_to_write;
}

const LONGLONG filenode::get_filesize() {
  std::shared_lock lock(_data_mutex);
  return static_cast<LONGLONG>(_data.size());
}

void filenode::set_endoffile(const LONGLONG& byte_offset) {
  std::unique_lock lock(_data_mutex);
  _data.resize(static_cast<size_t>(byte_offset));
}

const std::wstring filenode::get_filename() {
  std::shared_lock lock(_fileName_mutex);
  return _fileName;
}

void filenode::set_filename(const std::wstring& f) {
  std::unique_lock lock(_fileName_mutex);
  _fileName = f;
}

void filenode::add_stream(const std::shared_ptr<filenode>& stream) {
  std::unique_lock lock(_data_mutex);
  _streams[stream->get_filename()] = stream;
}

void filenode::remove_stream(const std::shared_ptr<filenode>& stream) {
  std::unique_lock lock(_data_mutex);
  _streams.erase(stream->get_filename());
}

std::unordered_map<std::wstring, std::shared_ptr<filenode> >
filenode::get_streams() {
  std::shared_lock lock(_data_mutex);
  return _streams;
}
}  // namespace memfs