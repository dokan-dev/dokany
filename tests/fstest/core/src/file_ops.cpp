// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/file_ops.h"

#include "fstest/core/ntdll.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace fstest::core {
namespace {

template <typename T>
NtResult<T> QueryFixedFileInfo(HANDLE h, nt::FileInfoClass info_class) {
  NtResult<T> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  nt::IoStatusBlock iosb{};
  T info{};
  result.status =
      ntdll.NtQueryInformationFile(h, &iosb, &info, sizeof(info), info_class);
  result.information = iosb.information;
  if (nt::NtSuccess(result.status)) {
    result.value = info;
  }
  return result;
}

template <typename T>
NtResult<void> SetFixedFileInfo(HANDLE h, nt::FileInfoClass info_class,
                                const T &info) {
  NtResult<void> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtSetInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  nt::IoStatusBlock iosb{};
  T copy = info;
  result.status =
      ntdll.NtSetInformationFile(h, &iosb, &copy, sizeof(copy), info_class);
  result.information = iosb.information;
  return result;
}

NtResult<void> SetRenameOrLinkImpl(HANDLE h, nt::FileInfoClass info_class,
                                   std::wstring_view target_name,
                                   HANDLE root_dir, ULONG flags,
                                   bool is_ex_flags) {
  NtResult<void> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtSetInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }

  constexpr size_t kHeaderOffset = offsetof(nt::FileRenameInfo, file_name);
  constexpr size_t kMaxTotalBytes =
      static_cast<size_t>((std::numeric_limits<ULONG>::max)());
  if (target_name.size() > (kMaxTotalBytes - kHeaderOffset) / sizeof(wchar_t)) {
    result.status = STATUS_INVALID_PARAMETER;
    return result;
  }
  const size_t name_bytes = target_name.size() * sizeof(wchar_t);
  const size_t total_size =
      (std::max)(sizeof(nt::FileRenameInfo), kHeaderOffset + name_bytes);
  std::vector<uint8_t> buf(total_size, 0);
  auto *info = reinterpret_cast<nt::FileRenameInfo *>(buf.data());
  if (is_ex_flags) {
    info->flags = flags;
  } else {
    info->replace_if_exists = (flags != 0) ? TRUE : FALSE;
  }
  info->root_directory = root_dir;
  info->file_name_length = static_cast<ULONG>(name_bytes);
  if (name_bytes > 0) {
    std::memcpy(info->file_name, target_name.data(), name_bytes);
  }

  nt::IoStatusBlock iosb{};
  result.status = ntdll.NtSetInformationFile(
      h, &iosb, buf.data(), static_cast<ULONG>(buf.size()), info_class);
  result.information = iosb.information;
  return result;
}

Win32Result<void> DeviceIoControlNoOutput(HANDLE h, DWORD ioctl_code,
                                          void *in_buf, DWORD in_size) {
  Win32Result<void> result{};
  DWORD bytes_returned = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::DeviceIoControl(h, ioctl_code, in_buf, in_size, nullptr, 0,
                        &bytes_returned, nullptr) == FALSE) {
    result.error = ::GetLastError();
    if (result.error == ERROR_SUCCESS) {
      result.error = ERROR_GEN_FAILURE;
    }
    return result;
  }
  result.error = ERROR_SUCCESS;
  return result;
}

} // namespace

NtResult<UniqueHandle>
NtCreateFilePath(std::wstring_view nt_path, ACCESS_MASK desired_access,
                 ULONG file_attributes, ULONG share_access,
                 ULONG create_disposition, ULONG create_options,
                 HANDLE root_directory, void *ea_buffer, ULONG ea_length,
                 PLARGE_INTEGER allocation_size, ULONG obj_attributes,
                 void *security_descriptor) {
  NtResult<UniqueHandle> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtCreateFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }

  if (nt_path.size() > nt::kMaxUnicodeStringChars) {
    result.status = STATUS_NAME_TOO_LONG;
    return result;
  }

  std::wstring path_buf(nt_path);
  nt::UnicodeString us =
      nt::MakeUnicodeString(path_buf.data(), path_buf.size());
  nt::ObjectAttributes oa = nt::MakeObjectAttributes(
      &us, obj_attributes, root_directory, security_descriptor);
  nt::IoStatusBlock iosb{};
  HANDLE raw_handle = nullptr;

  result.status = ntdll.NtCreateFile(
      &raw_handle, desired_access, &oa, &iosb, allocation_size, file_attributes,
      share_access, create_disposition, create_options, ea_buffer, ea_length);
  result.information = iosb.information;
  if (nt::NtSuccess(result.status)) {
    result.value.emplace(raw_handle);
  }
  return result;
}

NtResult<UniqueHandle> NtOpenFilePath(std::wstring_view nt_path,
                                      ACCESS_MASK desired_access,
                                      ULONG share_access, ULONG open_options,
                                      HANDLE root_directory,
                                      ULONG obj_attributes) {
  NtResult<UniqueHandle> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtOpenFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }

  if (nt_path.size() > nt::kMaxUnicodeStringChars) {
    result.status = STATUS_NAME_TOO_LONG;
    return result;
  }

  std::wstring path_buf(nt_path);
  nt::UnicodeString us =
      nt::MakeUnicodeString(path_buf.data(), path_buf.size());
  nt::ObjectAttributes oa =
      nt::MakeObjectAttributes(&us, obj_attributes, root_directory);
  nt::IoStatusBlock iosb{};
  HANDLE raw_handle = nullptr;

  result.status = ntdll.NtOpenFile(&raw_handle, desired_access, &oa, &iosb,
                                   share_access, open_options);
  result.information = iosb.information;
  if (nt::NtSuccess(result.status)) {
    result.value.emplace(raw_handle);
  }
  return result;
}

NtResult<UniqueHandle>
OpenRelative(HANDLE dir_handle, std::wstring_view rel_path,
             ACCESS_MASK desired_access, ULONG share_access,
             ULONG create_disposition, ULONG create_options,
             ULONG file_attributes) {
  return NtCreateFilePath(rel_path, desired_access, file_attributes,
                          share_access, create_disposition, create_options,
                          dir_handle);
}

Win32OpenResult CreateWin32FileEx(std::wstring_view path, DWORD desired_access,
                                  DWORD share_mode, DWORD creation_disposition,
                                  DWORD flags_and_attributes,
                                  LPSECURITY_ATTRIBUTES security_attributes) {
  Win32OpenResult result{};
  std::wstring path_str(path);
  ::SetLastError(ERROR_SUCCESS);
  HANDLE h = ::CreateFileW(path_str.c_str(), desired_access, share_mode,
                           security_attributes, creation_disposition,
                           flags_and_attributes, nullptr);
  result.last_error = ::GetLastError();
  if (h == INVALID_HANDLE_VALUE) {
    if (result.last_error == ERROR_SUCCESS) {
      result.last_error = ERROR_GEN_FAILURE;
    }
    return result;
  }
  result.handle.reset(h);
  return result;
}

Win32Result<UniqueHandle>
CreateWin32File(std::wstring_view path, DWORD desired_access, DWORD share_mode,
                DWORD creation_disposition, DWORD flags_and_attributes,
                LPSECURITY_ATTRIBUTES security_attributes) {
  Win32Result<UniqueHandle> result{};
  Win32OpenResult opened =
      CreateWin32FileEx(path, desired_access, share_mode, creation_disposition,
                        flags_and_attributes, security_attributes);
  if (!opened.handle) {
    result.error = opened.last_error;
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(opened.handle);
  return result;
}

Win32Result<void> WriteAllBytes(HANDLE h, const void *data, size_t size) {
  Win32Result<void> result{};
  const auto *ptr = static_cast<const uint8_t *>(data);
  size_t remaining = size;
  while (remaining > 0) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(remaining, 16u * 1024u * 1024u));
    DWORD written = 0;
    if (::WriteFile(h, ptr, chunk, &written, nullptr) == FALSE) {
      result.error = ::GetLastError();
      return result;
    }
    if (written == 0) {
      result.error = ERROR_WRITE_FAULT;
      return result;
    }
    ptr += written;
    remaining -= written;
  }
  result.error = ERROR_SUCCESS;
  return result;
}

Win32Result<void> WriteFileContents(std::wstring_view path,
                                    std::string_view data,
                                    DWORD flags_and_attributes) {
  auto opened = CreateWin32File(path, GENERIC_WRITE, kShareAll, CREATE_ALWAYS,
                                flags_and_attributes);
  if (!opened) {
    return Win32Result<void>{opened.error};
  }
  return WriteAllBytes(opened.value->get(), data.data(), data.size());
}

Win32Result<std::string> ReadAllBytes(HANDLE h) {
  Win32Result<std::string> result{};
  std::string out;
  char chunk[16384];
  for (;;) {
    DWORD bytes_read = 0;
    if (::ReadFile(h, chunk, sizeof(chunk), &bytes_read, nullptr) == FALSE) {
      result.error = ::GetLastError();
      return result;
    }
    if (bytes_read == 0) {
      break;
    }
    out.append(chunk, bytes_read);
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(out);
  return result;
}

Win32Result<std::string> ReadFileContents(std::wstring_view path) {
  auto opened = CreateWin32File(path, GENERIC_READ, kShareAll, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL);
  if (!opened) {
    Win32Result<std::string> res{};
    res.error = opened.error;
    return res;
  }
  return ReadAllBytes(opened.value->get());
}

NtResult<nt::FileBasicInfo> QueryBasicInfo(HANDLE h) {
  return QueryFixedFileInfo<nt::FileBasicInfo>(
      h, nt::FileInfoClass::FileBasicInformation);
}

NtResult<void> SetBasicInfo(HANDLE h, const nt::FileBasicInfo &info) {
  return SetFixedFileInfo(h, nt::FileInfoClass::FileBasicInformation, info);
}

NtResult<nt::FileStandardInfo> QueryStandardInfo(HANDLE h) {
  return QueryFixedFileInfo<nt::FileStandardInfo>(
      h, nt::FileInfoClass::FileStandardInformation);
}

NtResult<nt::FileInternalInfo> QueryInternalInfo(HANDLE h) {
  return QueryFixedFileInfo<nt::FileInternalInfo>(
      h, nt::FileInfoClass::FileInternalInformation);
}

NtResult<nt::FileStandardLinkInfo> QueryStandardLinkInfo(HANDLE h) {
  return QueryFixedFileInfo<nt::FileStandardLinkInfo>(
      h, nt::FileInfoClass::FileStandardLinkInformation);
}

NtResult<std::vector<std::pair<int64_t, std::wstring>>>
QueryHardLinkEntries(HANDLE h) {
  NtResult<std::vector<std::pair<int64_t, std::wstring>>> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }

  std::vector<uint8_t> buf(4096, 0);
  for (int attempt = 0; attempt < 10; ++attempt) {
    nt::IoStatusBlock iosb{};
    result.status = ntdll.NtQueryInformationFile(
        h, &iosb, buf.data(), static_cast<ULONG>(buf.size()),
        nt::FileInfoClass::FileHardLinkInformation);
    result.information = iosb.information;
    if (result.status == STATUS_BUFFER_OVERFLOW ||
        result.status == STATUS_BUFFER_TOO_SMALL) {
      size_t next_size = buf.size() * 2;
      if (buf.size() >= sizeof(nt::FileLinksInfo)) {
        const auto *links =
            reinterpret_cast<const nt::FileLinksInfo *>(buf.data());
        constexpr size_t kMaxHardLinkBuf = 64u * 1024u * 1024u;
        if (links->bytes_needed > buf.size() &&
            links->bytes_needed <= kMaxHardLinkBuf) {
          next_size = std::max<size_t>(next_size, links->bytes_needed);
        }
      }
      buf.assign(next_size, 0);
      continue;
    }
    if (!nt::NtSuccess(result.status)) {
      return result;
    }

    const size_t valid_bytes =
        std::min<size_t>(static_cast<size_t>(iosb.information), buf.size());
    std::vector<std::pair<int64_t, std::wstring>> entries =
        ParseHardLinkBuffer(std::span<const uint8_t>(buf.data(), valid_bytes));
    result.value = std::move(entries);
    return result;
  }
  return result;
}

std::vector<std::pair<int64_t, std::wstring>>
ParseHardLinkBuffer(std::span<const uint8_t> returned) {
  std::vector<std::pair<int64_t, std::wstring>> entries;
  constexpr size_t kLinksHeaderSize = offsetof(nt::FileLinksInfo, entry);
  constexpr size_t kEntryHeaderSize =
      offsetof(nt::FileLinkEntryInfo, file_name);
  const size_t valid_bytes = returned.size();
  if (valid_bytes < kLinksHeaderSize) {
    return entries;
  }
  ULONG entries_returned = 0;
  std::memcpy(&entries_returned,
              returned.data() + offsetof(nt::FileLinksInfo, entries_returned),
              sizeof(entries_returned));
  size_t offset = kLinksHeaderSize;
  for (ULONG i = 0; i < entries_returned; ++i) {
    if (offset > valid_bytes || valid_bytes - offset < kEntryHeaderSize) {
      break;
    }
    // memcpy field loads: corrupt NextEntryOffset values may misalign entries.
    const uint8_t *base = returned.data() + offset;
    ULONG next_entry_offset = 0;
    LONGLONG parent_file_id = 0;
    ULONG name_chars_field = 0;
    std::memcpy(&next_entry_offset,
                base + offsetof(nt::FileLinkEntryInfo, next_entry_offset),
                sizeof(next_entry_offset));
    std::memcpy(&parent_file_id,
                base + offsetof(nt::FileLinkEntryInfo, parent_file_id),
                sizeof(parent_file_id));
    std::memcpy(&name_chars_field,
                base + offsetof(nt::FileLinkEntryInfo, file_name_length),
                sizeof(name_chars_field));
    // FILE_LINK_ENTRY_INFORMATION.FileNameLength is in WCHARs, not bytes.
    const size_t max_chars =
        (valid_bytes - offset - kEntryHeaderSize) / sizeof(wchar_t);
    const size_t name_chars = std::min<size_t>(name_chars_field, max_chars);
    std::wstring name(name_chars, L'\0');
    if (name_chars > 0) {
      std::memcpy(name.data(), base + kEntryHeaderSize,
                  name_chars * sizeof(wchar_t));
    }
    entries.emplace_back(static_cast<int64_t>(parent_file_id), std::move(name));
    if (next_entry_offset == 0 || next_entry_offset > valid_bytes - offset) {
      break;
    }
    offset += next_entry_offset;
  }
  return entries;
}

std::optional<std::wstring>
ParseFileNameInfoBuffer(std::span<const uint8_t> returned) {
  constexpr size_t kHeaderSize = offsetof(nt::FileNameInfo, file_name);
  if (returned.size() < kHeaderSize) {
    return std::nullopt;
  }
  ULONG name_bytes_field = 0;
  std::memcpy(&name_bytes_field,
              returned.data() + offsetof(nt::FileNameInfo, file_name_length),
              sizeof(name_bytes_field));
  const size_t byte_len =
      std::min<size_t>(name_bytes_field, returned.size() - kHeaderSize) &
      ~static_cast<size_t>(1);
  std::wstring name(byte_len / sizeof(wchar_t), L'\0');
  if (byte_len > 0) {
    std::memcpy(name.data(), returned.data() + kHeaderSize, byte_len);
  }
  return name;
}

NtResult<std::wstring> QueryFileNameInfo(HANDLE h, bool normalized) {
  NtResult<std::wstring> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  const nt::FileInfoClass info_class =
      normalized ? nt::FileInfoClass::FileNormalizedNameInformation
                 : nt::FileInfoClass::FileNameInformation;
  constexpr size_t kMaxNameBuf = 1024u * 1024u;
  std::vector<uint8_t> buf(1024, 0);
  for (int attempt = 0; attempt < 6; ++attempt) {
    nt::IoStatusBlock iosb{};
    result.status = ntdll.NtQueryInformationFile(
        h, &iosb, buf.data(), static_cast<ULONG>(buf.size()), info_class);
    result.information = iosb.information;
    if (result.status == STATUS_BUFFER_OVERFLOW ||
        result.status == STATUS_BUFFER_TOO_SMALL) {
      size_t needed = buf.size() * 2;
      if (result.status == STATUS_BUFFER_OVERFLOW &&
          iosb.information >= offsetof(nt::FileNameInfo, file_name)) {
        const auto *info =
            reinterpret_cast<const nt::FileNameInfo *>(buf.data());
        constexpr size_t kOverhead =
            offsetof(nt::FileNameInfo, file_name) + sizeof(wchar_t) * 2;
        if (info->file_name_length > kMaxNameBuf - kOverhead) {
          return result;
        }
        needed = std::max<size_t>(
            needed, kOverhead + static_cast<size_t>(info->file_name_length));
      }
      if (needed > kMaxNameBuf) {
        if (buf.size() >= kMaxNameBuf) {
          return result;
        }
        needed = kMaxNameBuf;
      }
      buf.assign(needed, 0);
      continue;
    }
    if (nt::NtSuccess(result.status)) {
      const size_t valid_bytes =
          std::min<size_t>(static_cast<size_t>(iosb.information), buf.size());
      std::optional<std::wstring> name = ParseFileNameInfoBuffer(
          std::span<const uint8_t>(buf.data(), valid_bytes));
      if (!name.has_value()) {
        result.status = STATUS_INFO_LENGTH_MISMATCH;
        return result;
      }
      result.value = std::move(name);
    }
    return result;
  }
  return result;
}

NtResult<void> SetDisposition(HANDLE h, bool delete_file) {
  nt::FileDispositionInfo info{
      static_cast<BOOLEAN>(delete_file ? TRUE : FALSE)};
  return SetFixedFileInfo(h, nt::FileInfoClass::FileDispositionInformation,
                          info);
}

NtResult<void> SetDispositionEx(HANDLE h, ULONG flags) {
  nt::FileDispositionInfoEx info{flags};
  return SetFixedFileInfo(h, nt::FileInfoClass::FileDispositionInformationEx,
                          info);
}

NtResult<void> SetRenameInfo(HANDLE h, std::wstring_view target_name,
                             HANDLE root_dir, bool replace_if_exists) {
  return SetRenameOrLinkImpl(h, nt::FileInfoClass::FileRenameInformation,
                             target_name, root_dir,
                             replace_if_exists ? 1UL : 0UL,
                             /*is_ex_flags=*/false);
}

NtResult<void> SetRenameInfoEx(HANDLE h, std::wstring_view target_name,
                               HANDLE root_dir, ULONG flags) {
  return SetRenameOrLinkImpl(h, nt::FileInfoClass::FileRenameInformationEx,
                             target_name, root_dir, flags,
                             /*is_ex_flags=*/true);
}

NtResult<void> SetLinkInfo(HANDLE h, std::wstring_view target_name,
                           HANDLE root_dir, bool replace_if_exists) {
  return SetRenameOrLinkImpl(h, nt::FileInfoClass::FileLinkInformation,
                             target_name, root_dir,
                             replace_if_exists ? 1UL : 0UL,
                             /*is_ex_flags=*/false);
}

NtResult<void> SetLinkInfoEx(HANDLE h, std::wstring_view target_name,
                             HANDLE root_dir, ULONG flags) {
  return SetRenameOrLinkImpl(h, nt::FileInfoClass::FileLinkInformationEx,
                             target_name, root_dir, flags,
                             /*is_ex_flags=*/true);
}

NtResult<void> SetEndOfFileNt(HANDLE h, int64_t end_of_file) {
  nt::FileEndOfFileInfo info{};
  info.end_of_file.QuadPart = end_of_file;
  return SetFixedFileInfo(h, nt::FileInfoClass::FileEndOfFileInformation, info);
}

NtResult<void> SetAllocationSizeNt(HANDLE h, int64_t allocation_size) {
  nt::FileAllocationInfo info{};
  info.allocation_size.QuadPart = allocation_size;
  return SetFixedFileInfo(h, nt::FileInfoClass::FileAllocationInformation,
                          info);
}

Win32Result<void> SetSparseFile(HANDLE h, bool sparse) {
  FILE_SET_SPARSE_BUFFER buf{};
  buf.SetSparse = sparse ? TRUE : FALSE;
  return DeviceIoControlNoOutput(h, FSCTL_SET_SPARSE, &buf,
                                 static_cast<DWORD>(sizeof(buf)));
}

Win32Result<void> SetZeroData(HANDLE h, int64_t file_offset,
                              int64_t beyond_final_zero) {
  FILE_ZERO_DATA_INFORMATION zdi{};
  zdi.FileOffset.QuadPart = file_offset;
  zdi.BeyondFinalZero.QuadPart = beyond_final_zero;
  return DeviceIoControlNoOutput(h, FSCTL_SET_ZERO_DATA, &zdi,
                                 static_cast<DWORD>(sizeof(zdi)));
}

Win32Result<std::vector<FILE_ALLOCATED_RANGE_BUFFER>>
QueryAllocatedRanges(HANDLE h, int64_t offset, int64_t length) {
  Win32Result<std::vector<FILE_ALLOCATED_RANGE_BUFFER>> result{};
  std::vector<FILE_ALLOCATED_RANGE_BUFFER> ranges;
  int64_t cursor = offset;
  int64_t remaining = length;

  constexpr int kMaxPages = 1024;
  for (int page_index = 0; page_index < kMaxPages; ++page_index) {
    FILE_ALLOCATED_RANGE_BUFFER query{};
    query.FileOffset.QuadPart = cursor;
    query.Length.QuadPart = remaining;

    FILE_ALLOCATED_RANGE_BUFFER page[64]{};
    DWORD bytes_returned = 0;
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok = ::DeviceIoControl(
        h, FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query), page,
        static_cast<DWORD>(sizeof(page)), &bytes_returned, nullptr);
    const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
    if (ok == FALSE && err != ERROR_MORE_DATA) {
      result.error = (err != ERROR_SUCCESS) ? err : ERROR_GEN_FAILURE;
      return result;
    }
    if (bytes_returned % sizeof(FILE_ALLOCATED_RANGE_BUFFER) != 0) {
      result.error = ERROR_INVALID_DATA;
      return result;
    }
    const size_t count = bytes_returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
    for (size_t i = 0; i < count; ++i) {
      ranges.push_back(page[i]);
    }
    if (ok != FALSE) {
      result.error = ERROR_SUCCESS;
      result.value = std::move(ranges);
      return result;
    }
    if (count == 0) {
      result.error = ERROR_MORE_DATA;
      return result;
    }

    const FILE_ALLOCATED_RANGE_BUFFER &last = page[count - 1];
    if (last.FileOffset.QuadPart < 0 || last.Length.QuadPart <= 0 ||
        last.Length.QuadPart >
            (std::numeric_limits<int64_t>::max)() - last.FileOffset.QuadPart) {
      result.error = ERROR_INVALID_DATA;
      return result;
    }
    const int64_t next_cursor = last.FileOffset.QuadPart + last.Length.QuadPart;
    if (next_cursor <= cursor) {
      result.error = ERROR_MORE_DATA;
      return result;
    }
    const int64_t advanced = next_cursor - cursor;
    if (advanced >= remaining) {
      result.error = ERROR_SUCCESS;
      result.value = std::move(ranges);
      return result;
    }
    cursor = next_cursor;
    remaining -= advanced;
  }

  result.error = ERROR_MORE_DATA;
  return result;
}

Win32Result<void> SetCompressionState(HANDLE h, USHORT format) {
  USHORT req_format = format;
  return DeviceIoControlNoOutput(h, FSCTL_SET_COMPRESSION, &req_format,
                                 static_cast<DWORD>(sizeof(req_format)));
}

Win32Result<USHORT> GetCompressionState(HANDLE h) {
  Win32Result<USHORT> result{};
  USHORT format = COMPRESSION_FORMAT_NONE;
  DWORD bytes_returned = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::DeviceIoControl(h, FSCTL_GET_COMPRESSION, nullptr, 0, &format,
                        static_cast<DWORD>(sizeof(format)), &bytes_returned,
                        nullptr) == FALSE) {
    result.error = ::GetLastError();
    if (result.error == ERROR_SUCCESS) {
      result.error = ERROR_GEN_FAILURE;
    }
    return result;
  }
  if (bytes_returned != sizeof(format)) {
    result.error = ERROR_INVALID_DATA;
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = format;
  return result;
}

Win32Result<BY_HANDLE_FILE_INFORMATION> GetByHandleInfo(HANDLE h) {
  Win32Result<BY_HANDLE_FILE_INFORMATION> result{};
  BY_HANDLE_FILE_INFORMATION info{};
  if (::GetFileInformationByHandle(h, &info) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = info;
  return result;
}

Win32Result<uint64_t> GetFileIndex(HANDLE h) {
  auto info = GetByHandleInfo(h);
  Win32Result<uint64_t> result{};
  result.error = info.error;
  if (info) {
    result.value = FileIndexOf(*info.value);
  }
  return result;
}

Win32Result<DWORD> QueryWin32Attributes(std::wstring_view path) {
  Win32Result<DWORD> result{};
  std::wstring path_str(path);
  ::SetLastError(ERROR_SUCCESS);
  const DWORD attrs = ::GetFileAttributesW(path_str.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    result.error = ::GetLastError();
    if (result.error == ERROR_SUCCESS) {
      result.error = ERROR_GEN_FAILURE;
    }
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = attrs;
  return result;
}

Win32Result<BY_HANDLE_FILE_INFORMATION> QueryInfoByPath(std::wstring_view path,
                                                        bool backup_semantics) {
  const DWORD flags =
      backup_semantics ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
  auto opened = CreateWin32File(path, FILE_READ_ATTRIBUTES, kShareAll,
                                OPEN_EXISTING, flags);
  if (!opened) {
    Win32Result<BY_HANDLE_FILE_INFORMATION> result{};
    result.error = opened.error;
    return result;
  }
  return GetByHandleInfo(opened.value->get());
}

Win32Result<uint64_t> QueryPathFileIndex(std::wstring_view path,
                                         bool backup_semantics) {
  auto info = QueryInfoByPath(path, backup_semantics);
  Win32Result<uint64_t> result{};
  result.error = info.error;
  if (info) {
    result.value = FileIndexOf(*info.value);
  }
  return result;
}

Win32Result<std::wstring> QueryFinalPath(HANDLE handle, DWORD flags) {
  Win32Result<std::wstring> result{};
  std::wstring buf(MAX_PATH + 1, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    ::SetLastError(ERROR_SUCCESS);
    const DWORD written = ::GetFinalPathNameByHandleW(
        handle, buf.data(), static_cast<DWORD>(buf.size()), flags);
    if (written == 0) {
      result.error = ::GetLastError();
      if (result.error == ERROR_SUCCESS) {
        result.error = ERROR_GEN_FAILURE;
      }
      return result;
    }
    if (written < buf.size()) {
      buf.resize(written);
      result.error = ERROR_SUCCESS;
      result.value = std::move(buf);
      return result;
    }
    // Buffer was too small: written is the required length including NUL.
    buf.assign(static_cast<size_t>(written) + 1, L'\0');
  }
  result.error = ERROR_INSUFFICIENT_BUFFER;
  return result;
}

FILETIME MakeUtcFileTime(WORD year, WORD month, WORD day, WORD hour,
                         WORD minute, WORD second) noexcept {
  SYSTEMTIME st{};
  st.wYear = year;
  st.wMonth = month;
  st.wDay = day;
  st.wHour = hour;
  st.wMinute = minute;
  st.wSecond = second;
  st.wMilliseconds = 0;
  FILETIME ft{};
  if (::SystemTimeToFileTime(&st, &ft) == FALSE) {
    return FILETIME{};
  }
  return ft;
}

int64_t MakeUtcFileTimeInt64(WORD year, WORD month, WORD day, WORD hour,
                             WORD minute, WORD second) noexcept {
  return FileTimeToInt64(
      MakeUtcFileTime(year, month, day, hour, minute, second));
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  if (a.empty()) {
    return true;
  }
  return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()),
                                /*bIgnoreCase=*/TRUE) == CSTR_EQUAL;
}

bool EndsWithIgnoreCase(std::wstring_view text,
                        std::wstring_view suffix) noexcept {
  if (text.size() < suffix.size()) {
    return false;
  }
  return EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

bool EndsWithExact(std::wstring_view text, std::wstring_view suffix) noexcept {
  if (text.size() < suffix.size()) {
    return false;
  }
  return text.substr(text.size() - suffix.size()) == suffix;
}

} // namespace fstest::core
