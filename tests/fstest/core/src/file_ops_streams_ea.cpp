// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/file_ops.h"

#include "fstest/core/ntdll.h"
#include "fstest/core/path.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace fstest::core {

Win32Result<std::vector<std::wstring>>
ListDirectoryWin32(std::wstring_view dir_path, std::wstring_view pattern) {
  Win32Result<std::vector<std::wstring>> result{};
  const std::wstring query = JoinPath(dir_path, pattern);
  WIN32_FIND_DATAW data{};
  UniqueFindHandle find(::FindFirstFileW(query.c_str(), &data));
  if (!find) {
    result.error = ::GetLastError();
    return result;
  }
  std::vector<std::wstring> entries;
  do {
    entries.emplace_back(data.cFileName);
  } while (::FindNextFileW(find.get(), &data) != FALSE);

  const DWORD err = ::GetLastError();
  if (err != ERROR_NO_MORE_FILES) {
    result.error = err;
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(entries);
  return result;
}

Win32Result<std::vector<StreamEntry>> ListStreamsWin32(std::wstring_view path) {
  Win32Result<std::vector<StreamEntry>> result{};
  std::wstring path_str(path);
  WIN32_FIND_STREAM_DATA data{};
  UniqueFindHandle find(
      ::FindFirstStreamW(path_str.c_str(), FindStreamInfoStandard, &data, 0));
  if (!find) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_HANDLE_EOF) {
      result.error = ERROR_SUCCESS;
      result.value.emplace();
      return result;
    }
    result.error = err;
    return result;
  }
  std::vector<StreamEntry> entries;
  do {
    StreamEntry entry{};
    entry.name = data.cStreamName;
    entry.size = data.StreamSize.QuadPart;
    entries.push_back(std::move(entry));
  } while (::FindNextStreamW(find.get(), &data) != FALSE);

  const DWORD err = ::GetLastError();
  if (err != ERROR_HANDLE_EOF) {
    result.error = err;
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(entries);
  return result;
}

NtResult<std::vector<StreamEntry>> ListStreamsNt(HANDLE h) {
  NtResult<std::vector<StreamEntry>> result{};
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
        nt::FileInfoClass::FileStreamInformation);
    result.information = iosb.information;
    if (result.status == STATUS_BUFFER_OVERFLOW ||
        result.status == STATUS_BUFFER_TOO_SMALL) {
      buf.assign(buf.size() * 2, 0);
      continue;
    }
    if (!nt::NtSuccess(result.status)) {
      return result;
    }

    const size_t valid_bytes =
        std::min<size_t>(static_cast<size_t>(iosb.information), buf.size());
    std::vector<StreamEntry> entries = ParseStreamInfoBuffer(
        std::span<const uint8_t>(buf.data(), valid_bytes));
    result.value = std::move(entries);
    return result;
  }
  return result;
}

std::vector<StreamEntry>
ParseStreamInfoBuffer(std::span<const uint8_t> returned) {
  std::vector<StreamEntry> entries;
  constexpr size_t kHeaderSize = offsetof(nt::FileStreamInfo, stream_name);
  const size_t valid_bytes = returned.size();
  size_t offset = 0;
  while (offset < valid_bytes && valid_bytes - offset >= kHeaderSize) {
    // memcpy field loads: corrupt NextEntryOffset values may misalign entries.
    const uint8_t *base = returned.data() + offset;
    ULONG next_entry_offset = 0;
    ULONG name_length = 0;
    LARGE_INTEGER stream_size{};
    LARGE_INTEGER allocation_size{};
    std::memcpy(&next_entry_offset,
                base + offsetof(nt::FileStreamInfo, next_entry_offset),
                sizeof(next_entry_offset));
    std::memcpy(&name_length,
                base + offsetof(nt::FileStreamInfo, stream_name_length),
                sizeof(name_length));
    std::memcpy(&stream_size, base + offsetof(nt::FileStreamInfo, stream_size),
                sizeof(stream_size));
    std::memcpy(&allocation_size,
                base + offsetof(nt::FileStreamInfo, stream_allocation_size),
                sizeof(allocation_size));
    const size_t max_name_bytes = valid_bytes - offset - kHeaderSize;
    const size_t name_bytes =
        std::min<size_t>(name_length, max_name_bytes) & ~static_cast<size_t>(1);
    StreamEntry entry{};
    entry.name.assign(name_bytes / sizeof(wchar_t), L'\0');
    if (name_bytes > 0) {
      std::memcpy(entry.name.data(), base + kHeaderSize, name_bytes);
    }
    entry.size = stream_size.QuadPart;
    entry.allocation_size = allocation_size.QuadPart;
    entries.push_back(std::move(entry));
    if (next_entry_offset == 0 || next_entry_offset > valid_bytes - offset) {
      break;
    }
    offset += next_entry_offset;
  }
  return entries;
}

std::vector<uint8_t> BuildEaBuffer(const std::vector<EaEntry> &entries,
                                   std::string *error_out) {
  constexpr size_t kMaxEaNameBytes = 255;    // EaNameLength is a UCHAR.
  constexpr size_t kMaxEaValueBytes = 65535; // EaValueLength is a USHORT.
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &e = entries[i];
    if (e.name.size() > kMaxEaNameBytes || e.value.size() > kMaxEaValueBytes) {
      if (error_out != nullptr) {
        *error_out = "EA entry " + std::to_string(i) +
                     " exceeds limits (name " + std::to_string(e.name.size()) +
                     "/" + std::to_string(kMaxEaNameBytes) + " bytes, value " +
                     std::to_string(e.value.size()) + "/" +
                     std::to_string(kMaxEaValueBytes) + " bytes).";
      }
      return {};
    }
  }

  std::vector<uint8_t> out;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &e = entries[i];
    const size_t name_len = e.name.size();
    const size_t val_len = e.value.size();
    const size_t raw_size =
        offsetof(nt::FileFullEaInfo, ea_name) + name_len + 1 + val_len;
    const bool is_last = (i + 1 == entries.size());
    const size_t padded_size = is_last ? raw_size : ((raw_size + 3u) & ~3u);

    const size_t base = out.size();
    out.resize(base + padded_size, 0);
    auto *hdr = reinterpret_cast<nt::FileFullEaInfo *>(out.data() + base);
    hdr->next_entry_offset = is_last ? 0UL : static_cast<ULONG>(padded_size);
    hdr->flags = e.flags;
    hdr->ea_name_length = static_cast<UCHAR>(name_len);
    hdr->ea_value_length = static_cast<USHORT>(val_len);
    std::memcpy(hdr->ea_name, e.name.data(), name_len);
    hdr->ea_name[name_len] = '\0';
    if (val_len > 0) {
      std::memcpy(hdr->ea_name + name_len + 1, e.value.data(), val_len);
    }
  }
  return out;
}

std::vector<EaEntry> ParseEaBuffer(const uint8_t *data, size_t size) {
  std::vector<EaEntry> entries;
  if (data == nullptr || size == 0) {
    return entries;
  }
  constexpr size_t kHeaderSize = offsetof(nt::FileFullEaInfo, ea_name);
  size_t offset = 0;
  while (offset < size && size - offset > kHeaderSize) {
    const uint8_t *base = data + offset;
    ULONG next_entry_offset = 0;
    UCHAR flags = 0;
    UCHAR ea_name_length = 0;
    USHORT ea_value_length = 0;
    std::memcpy(&next_entry_offset,
                base + offsetof(nt::FileFullEaInfo, next_entry_offset),
                sizeof(next_entry_offset));
    std::memcpy(&flags, base + offsetof(nt::FileFullEaInfo, flags),
                sizeof(flags));
    std::memcpy(&ea_name_length,
                base + offsetof(nt::FileFullEaInfo, ea_name_length),
                sizeof(ea_name_length));
    std::memcpy(&ea_value_length,
                base + offsetof(nt::FileFullEaInfo, ea_value_length),
                sizeof(ea_value_length));

    const size_t payload_needed = static_cast<size_t>(ea_name_length) + 1u +
                                  static_cast<size_t>(ea_value_length);
    if (payload_needed > size - offset - kHeaderSize) {
      break;
    }
    const size_t name_offset = offset + kHeaderSize;
    const size_t val_offset = name_offset + ea_name_length + 1u;
    EaEntry entry{};
    entry.flags = flags;
    entry.name.assign(reinterpret_cast<const char *>(data + name_offset),
                      ea_name_length);
    entry.value.assign(data + val_offset, data + val_offset + ea_value_length);
    entries.push_back(std::move(entry));
    if (next_entry_offset == 0) {
      break;
    }
    if ((next_entry_offset % 4u) != 0 ||
        next_entry_offset < kHeaderSize + payload_needed ||
        next_entry_offset > size - offset) {
      break;
    }
    offset += next_entry_offset;
  }
  return entries;
}

} // namespace fstest::core
