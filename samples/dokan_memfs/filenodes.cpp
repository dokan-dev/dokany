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

#include "filenodes.h"

#include <sddl.h>
#include <spdlog/spdlog.h>

namespace memfs {
fs_filenodes::fs_filenodes() {
  WCHAR buffer[1024];
  WCHAR final_buffer[2048];
  PTOKEN_USER user_token = NULL;
  PTOKEN_GROUPS groups_token = NULL;
  HANDLE token_handle;
  LPTSTR user_sid_str = NULL;
  LPTSTR group_sid_str = NULL;

  // Build default root filenode SecurityDescriptor
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &token_handle) ==
      FALSE) {
    throw std::runtime_error("Failed init root resources");
  }
  DWORD return_length;
  if (!GetTokenInformation(token_handle, TokenUser, buffer, sizeof(buffer),
                           &return_length)) {
    CloseHandle(token_handle);
    throw std::runtime_error("Failed init root resources");
  }
  user_token = (PTOKEN_USER)buffer;
  if (!ConvertSidToStringSid(user_token->User.Sid, &user_sid_str)) {
    CloseHandle(token_handle);
    throw std::runtime_error("Failed init root resources");
  }
  if (!GetTokenInformation(token_handle, TokenGroups, buffer, sizeof(buffer),
                           &return_length)) {
    CloseHandle(token_handle);
    throw std::runtime_error("Failed init root resources");
  }
  groups_token = (PTOKEN_GROUPS)buffer;
  if (groups_token->GroupCount > 0) {
    if (!ConvertSidToStringSid(groups_token->Groups[0].Sid, &group_sid_str)) {
      CloseHandle(token_handle);
      throw std::runtime_error("Failed init root resources");
    }
    swprintf_s(buffer, 1024, L"O:%lsG:%ls", user_sid_str, group_sid_str);
  } else {
    swprintf_s(buffer, 1024, L"O:%ls", user_sid_str);
  }
  LocalFree(user_sid_str);
  LocalFree(group_sid_str);
  CloseHandle(token_handle);
  swprintf_s(final_buffer, 2048, L"%lsD:PAI(A;OICI;FA;;;AU)", buffer);
  PSECURITY_DESCRIPTOR security_descriptor = NULL;
  ULONG size = 0;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
          final_buffer, SDDL_REVISION_1, &security_descriptor, &size))
    throw std::runtime_error("Failed init root resources");
  auto fileNode = std::make_shared<filenode>(L"\\", true,
                                             FILE_ATTRIBUTE_DIRECTORY, nullptr);
  fileNode->security.SetDescriptor(security_descriptor);
  LocalFree(security_descriptor);

  _filenodes[L"\\"] = fileNode;
  _directoryPaths.emplace(L"\\", std::set<std::shared_ptr<filenode>>());
}

NTSTATUS fs_filenodes::add(const std::shared_ptr<filenode> &f,
                  std::optional<std::pair<std::wstring, std::wstring>> stream_names) {
  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);

  if (f->fileindex == 0)  // previous init
    f->fileindex = _fs_fileindex_count++;
  const auto filename = f->get_filename();
  const auto parent_path = memfs_helper::GetParentPath(filename);

  // Does target folder exist
  if (!_directoryPaths.count(parent_path)) {
    spdlog::warn(L"Add: No directory: {} exist FilePath: {}", parent_path,
                 filename);
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }

  if (!stream_names.has_value())
    stream_names = memfs_helper::GetStreamNames(filename);
  if (!stream_names.value().second.empty()) {
    auto &stream_names_value = stream_names.value();
    spdlog::info(
        L"Add file: {} is an alternate stream {} and has {} as main stream",
        filename, stream_names_value.second, stream_names_value.first);
    auto main_stream_name =
        memfs_helper::GetFileNameStreamLess(filename, stream_names_value);
    auto main_f = find(main_stream_name);
    if (!main_f)
      return STATUS_OBJECT_PATH_NOT_FOUND;
    main_f->add_stream(f);
    f->main_stream = main_f;
    f->fileindex = main_f->fileindex;
  }

  // If we have a folder, we add it to our directoryPaths
  if (f->is_directory && !_directoryPaths.count(filename))
    _directoryPaths.emplace(filename, std::set<std::shared_ptr<filenode>>());

  // Add our file to the fileNodes and directoryPaths
  auto previous_f = _filenodes[filename];
  _filenodes[filename] = f;
  _directoryPaths[parent_path].insert(f);
  if (previous_f)
    _directoryPaths[parent_path].erase(previous_f);

  spdlog::info(L"Add file: {} in folder: {}", filename, parent_path);
  return STATUS_SUCCESS;
}

std::shared_ptr<filenode> fs_filenodes::find(const std::wstring& filename) {
  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);
  auto fileNode = _filenodes.find(filename);
  return (fileNode != _filenodes.end()) ? fileNode->second : nullptr;
}

std::set<std::shared_ptr<filenode>> fs_filenodes::list_folder(
    const std::wstring& fileName) {
  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);

  auto it = _directoryPaths.find(fileName);
  return (it != _directoryPaths.end()) ? it->second
                                       : std::set<std::shared_ptr<filenode>>();
}

void fs_filenodes::remove(const std::wstring& filename) {
  return remove(find(filename));
}

void fs_filenodes::remove(const std::shared_ptr<filenode>& f) {
  if (!f) return;

  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);
  auto fileName = f->get_filename();
  spdlog::info(L"Remove: {}", fileName);

  // Remove node from fileNodes and directoryPaths
  _filenodes.erase(fileName);
  _directoryPaths[memfs_helper::GetParentPath(fileName)].erase(f);

  // if it was a directory we need to remove it from directoryPaths
  if (f->is_directory) {
    // but first we need to remove the directory content by looking recursively
    // into it
    auto files = list_folder(fileName);
    for (const auto& file : files) remove(file);

    _directoryPaths.erase(fileName);
  }

  // Cleanup streams
  if (f->main_stream) {
    // Is an alternate stream
    f->main_stream->remove_stream(f);
  } else {
    // Is a main stream
    // Remove possible alternate stream
    for (const auto& [stream_name, node] : f->get_streams())
      remove(stream_name);
  }
}

NTSTATUS fs_filenodes::move(const std::wstring& old_filename,
                            const std::wstring& new_filename,
                            BOOL replace_if_existing) {
  auto f = find(old_filename);
  auto new_f = find(new_filename);

  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  // Cannot move to an existing destination without replace flag
  if (!replace_if_existing && new_f) return STATUS_OBJECT_NAME_COLLISION;

  // Cannot replace read only destination
  if (new_f && new_f->attributes & FILE_ATTRIBUTE_READONLY)
    return STATUS_ACCESS_DENIED;

  // If destination exist - Cannot move directory or replace a directory
  if (new_f && (f->is_directory || new_f->is_directory))
    return STATUS_ACCESS_DENIED;

  auto newParent_path = memfs_helper::GetParentPath(new_filename);

  std::lock_guard<std::recursive_mutex> lock(_filesnodes_mutex);
  if (!_directoryPaths.count(newParent_path)) {
    spdlog::warn(L"Move: No directory: {} exist FilePath: {}", newParent_path,
                 new_filename);
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }

  // Remove destination
  remove(new_f);

  // Update current node with new data
  const auto fileName = f->get_filename();
  auto oldParentPath = memfs_helper::GetParentPath(fileName);
  f->set_filename(new_filename);

  // Move fileNode
  // 1 - by removing current not with oldName as key
  add(f, {});

  // 2 - If fileNode is a Dir we move content to destination
  if (f->is_directory) {
    // recurse remove sub folders/files
    auto files = list_folder(old_filename);
    for (const auto& file : files) {
      const auto sub_fileName = file->get_filename();
      auto newSubFileName = std::filesystem::path(new_filename)
                                .append(memfs_helper::GetFileName(sub_fileName))
                                .wstring();
      auto n = move(sub_fileName, newSubFileName, replace_if_existing);
      if (n != STATUS_SUCCESS) {
        spdlog::warn(
            L"Move: Subfolder file move {} to {} replaceIfExisting {} failed: "
            L"{}",
            sub_fileName, newSubFileName, replace_if_existing, n);
        return n;  // That's bad...we have not done a full move
      }
    }

    // remove folder from directories
    _directoryPaths.erase(old_filename);
  }

  // 3 - Remove fileNode link with oldFilename
  _filenodes.erase(old_filename);
  if (oldParentPath != newParent_path)  // Same folder destination
    _directoryPaths[oldParentPath].erase(f);

  spdlog::info(L"Move file: {} to folder: {}", old_filename, new_filename);
  return STATUS_SUCCESS;
}
}  // namespace memfs
