// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/file_ops.h"

#include "fstest/core/os_info.h"
#include "fstest/core/path.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace fstest::core {

Win32Result<void> CreateSymlink(std::wstring_view link_path,
                                std::wstring_view target_path,
                                bool is_directory, uint32_t os_build) {
  Win32Result<void> result{};
  DWORD flags =
      is_directory ? nt::kSymbolicLinkFlagDirectory : nt::kSymbolicLinkFlagFile;
  if (os_build >= kBuildWin10_1703) {
    flags |= nt::kSymbolicLinkFlagAllowUnprivilegedCreate;
  }
  std::wstring link_str(link_path);
  std::wstring target_str(target_path);
  if (::CreateSymbolicLinkW(link_str.c_str(), target_str.c_str(), flags) == 0) {
    result.error = ::GetLastError();
    if (result.error == ERROR_SUCCESS) {
      result.error = ERROR_GEN_FAILURE;
    }
    return result;
  }
  result.error = ERROR_SUCCESS;
  return result;
}

Win32Result<void> CreateDirectoryJunction(std::wstring_view junction_dir,
                                          std::wstring_view target_dir) {
  Win32Result<void> result{};
  const std::wstring print_name = NormalizeWin32Path(target_dir);
  const std::wstring substitute_name =
      VerbatimToNtPath(ToExtendedWin32Path(print_name));

  constexpr size_t kMaxChars =
      MAXIMUM_REPARSE_DATA_BUFFER_SIZE / sizeof(wchar_t);
  if (substitute_name.size() > kMaxChars || print_name.size() > kMaxChars) {
    result.error = ERROR_FILENAME_EXCED_RANGE;
    return result;
  }

  const size_t sub_bytes_sz = substitute_name.size() * sizeof(wchar_t);
  const size_t print_bytes_sz = print_name.size() * sizeof(wchar_t);
  // Mount point header (8 bytes) + sub + NUL + print + NUL.
  const size_t data_len_sz =
      8u + sub_bytes_sz + sizeof(wchar_t) + print_bytes_sz + sizeof(wchar_t);
  const size_t total_size = nt::kReparseDataBufferHeaderSize + data_len_sz;
  if (total_size > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
    result.error = ERROR_FILENAME_EXCED_RANGE;
    return result;
  }

  std::wstring junc_str(junction_dir);
  bool created_dir = false;
  if (::CreateDirectoryW(junc_str.c_str(), nullptr) != FALSE) {
    created_dir = true;
  } else {
    const DWORD err = ::GetLastError();
    if (err != ERROR_ALREADY_EXISTS) {
      result.error = err;
      return result;
    }
  }

  const USHORT sub_bytes = static_cast<USHORT>(sub_bytes_sz);
  const USHORT print_bytes = static_cast<USHORT>(print_bytes_sz);
  const USHORT data_len = static_cast<USHORT>(data_len_sz);

  std::vector<uint8_t> buf(total_size, 0);
  auto *rdb = reinterpret_cast<nt::ReparseDataBuffer *>(buf.data());
  rdb->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
  rdb->reparse_data_length = data_len;
  rdb->reserved = 0;
  rdb->mount_point.substitute_name_offset = 0;
  rdb->mount_point.substitute_name_length = sub_bytes;
  rdb->mount_point.print_name_offset =
      static_cast<USHORT>(sub_bytes + sizeof(wchar_t));
  rdb->mount_point.print_name_length = print_bytes;

  auto *dest = reinterpret_cast<uint8_t *>(rdb->mount_point.path_buffer);
  std::memcpy(dest, substitute_name.data(), sub_bytes);
  std::memcpy(dest + sub_bytes + sizeof(wchar_t), print_name.data(),
              print_bytes);

  auto opened = CreateWin32File(
      junc_str, GENERIC_WRITE, kShareAll, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
  if (!opened) {
    result.error = opened.error;
    if (created_dir) {
      ::RemoveDirectoryW(junc_str.c_str());
    }
    return result;
  }

  DWORD bytes_returned = 0;
  if (::DeviceIoControl(opened.value->get(), FSCTL_SET_REPARSE_POINT,
                        buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0,
                        &bytes_returned, nullptr) == FALSE) {
    result.error = ::GetLastError();
    opened.value->reset();
    if (created_dir) {
      ::RemoveDirectoryW(junc_str.c_str());
    }
    return result;
  }
  result.error = ERROR_SUCCESS;
  return result;
}

Win32Result<std::vector<uint8_t>> GetReparsePointData(HANDLE h) {
  Win32Result<std::vector<uint8_t>> result{};
  std::vector<uint8_t> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE, 0);
  DWORD bytes_returned = 0;
  if (::DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(),
                        static_cast<DWORD>(buf.size()), &bytes_returned,
                        nullptr) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  buf.resize(bytes_returned);
  result.error = ERROR_SUCCESS;
  result.value = std::move(buf);
  return result;
}

Win32Result<void> DeleteReparsePoint(HANDLE h, ULONG reparse_tag,
                                     const GUID *reparse_guid) {
  Win32Result<void> result{};
  DWORD bytes_returned = 0;
  if (reparse_guid != nullptr) {
    nt::ReparseGuidDataBuffer header{};
    header.reparse_tag = reparse_tag;
    header.reparse_data_length = 0;
    header.reserved = 0;
    header.reparse_guid = *reparse_guid;
    if (::DeviceIoControl(
            h, FSCTL_DELETE_REPARSE_POINT, &header,
            static_cast<DWORD>(nt::kReparseGuidDataBufferHeaderSize), nullptr,
            0, &bytes_returned, nullptr) == FALSE) {
      result.error = ::GetLastError();
      return result;
    }
  } else {
    nt::ReparseDataBuffer header{};
    header.reparse_tag = reparse_tag;
    header.reparse_data_length = 0;
    header.reserved = 0;
    if (::DeviceIoControl(h, FSCTL_DELETE_REPARSE_POINT, &header,
                          static_cast<DWORD>(nt::kReparseDataBufferHeaderSize),
                          nullptr, 0, &bytes_returned, nullptr) == FALSE) {
      result.error = ::GetLastError();
      return result;
    }
  }
  result.error = ERROR_SUCCESS;
  return result;
}

} // namespace fstest::core
