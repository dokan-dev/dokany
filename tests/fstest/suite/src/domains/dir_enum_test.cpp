// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fstest {
namespace {

core::Win32Result<core::UniqueHandle>
OpenDirForEnum(std::wstring_view dir_path) {
  return core::CreateWin32File(dir_path, FILE_LIST_DIRECTORY | SYNCHRONIZE,
                               core::kShareAll, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS);
}

core::Win32Result<core::UniqueHandle>
OpenRootForEnum(const core::NormalizedRoot &root) {
  std::wstring open_path = root.extended_root;
  if (root.kind == core::RootKind::kUncShare && !open_path.empty() &&
      open_path.back() != L'\\') {
    open_path.push_back(L'\\');
  }
  auto h = OpenDirForEnum(open_path);
  if (!h && root.kind == core::RootKind::kUncShare) {
    std::wstring fallback = root.win32_root;
    if (!fallback.empty() && fallback.back() != L'\\') {
      fallback.push_back(L'\\');
    }
    h = OpenDirForEnum(fallback);
  }
  return h;
}

::testing::AssertionResult CreateEmptyFile(std::wstring_view path) {
  auto res = helpers::CreateEmptyFileResult(path);
  if (!res) {
    return ::testing::AssertionFailure()
           << "Failed to create '" << core::WideToUtf8(path)
           << "': " << core::FormatWin32Error(res.error);
  }
  return ::testing::AssertionSuccess();
}

template <typename Callback>
::testing::AssertionResult ForEachWin32FindEntry(std::wstring_view query,
                                                 bool use_ex_basic,
                                                 Callback &&on_entry) {
  const std::wstring query_str(query);
  WIN32_FIND_DATAW data{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueFindHandle find(
      use_ex_basic
          ? ::FindFirstFileExW(query_str.c_str(), FindExInfoBasic, &data,
                               FindExSearchNameMatch, nullptr, 0)
          : ::FindFirstFileW(query_str.c_str(), &data));
  const DWORD first_err = ::GetLastError();
  if (!find) {
    return ::testing::AssertionFailure()
           << "FindFirstFile failed for '" << core::WideToUtf8(query)
           << "': " << core::FormatWin32Error(first_err);
  }
  do {
    on_entry(data);
  } while (::FindNextFileW(find.get(), &data) != FALSE);
  const DWORD end_err = ::GetLastError();
  if (end_err != static_cast<DWORD>(ERROR_NO_MORE_FILES)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_NO_MORE_FILES after FindNextFileW for '"
           << core::WideToUtf8(query) << "', got "
           << core::FormatWin32Error(end_err);
  }
  return ::testing::AssertionSuccess();
}

void VerifyVolumeRootEntries(const std::vector<std::wstring> &names,
                             std::wstring_view expected_temp_basename,
                             const char *label) {
  std::map<std::wstring, size_t> counts;
  bool found_temp_dir = false;
  for (const auto &entry : names) {
    EXPECT_NE(entry, L".") << "Volume root " << label
                           << " enum must not emit '.'";
    EXPECT_NE(entry, L"..")
        << "Volume root " << label << " enum must not emit '..'";
    const size_t seen = ++counts[entry];
    EXPECT_EQ(seen, 1u) << "Duplicate entry in volume root " << label
                        << " enum: " << core::WideToUtf8(entry);
    if (entry == expected_temp_basename) {
      found_temp_dir = true;
    }
  }
  EXPECT_TRUE(found_temp_dir)
      << "Own TempDir basename '" << core::WideToUtf8(expected_temp_basename)
      << "' missing from " << label << " volume root enumeration";
}

struct ExpectedFileIds {
  int64_t id64 = 0;
  nt::FileId128 id128{};
};

ExpectedFileIds QueryFileIds(std::wstring_view path, bool is_dir,
                             bool include_id128,
                             bool open_reparse_point = false) {
  ExpectedFileIds res{};
  DWORD flags = is_dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
  if (open_reparse_point) {
    flags |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  auto h = core::CreateWin32File(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 core::kShareAll, OPEN_EXISTING, flags);
  EXPECT_TRUE(h) << core::FormatWin32Error(h.error);
  if (!h) {
    return res;
  }
  auto internal = core::QueryInternalInfo(h.value->get());
  EXPECT_NT_SUCCESS(internal.status);
  if (internal) {
    res.id64 = internal.value->index_number.QuadPart;
  }
  if (include_id128) {
    const auto &ntdll = core::Ntdll();
    EXPECT_NE(ntdll.NtQueryInformationFile, nullptr);
    if (ntdll.NtQueryInformationFile != nullptr) {
      nt::IoStatusBlock iosb{};
      nt::FileIdInfo id_info{};
      const NTSTATUS st = ntdll.NtQueryInformationFile(
          h.value->get(), &iosb, &id_info, sizeof(id_info),
          nt::FileInfoClass::FileIdInformation);
      EXPECT_NT_SUCCESS(st);
      if (nt::NtSuccess(st)) {
        res.id128 = id_info.file_id;
      }
    }
  }
  return res;
}

template <typename EntryStruct> struct DirInfoTraits;

template <> struct DirInfoTraits<nt::FileDirectoryInfo> {
  static constexpr nt::FileInfoClass kInfoClass =
      nt::FileInfoClass::FileDirectoryInformation;
  static constexpr size_t kAlignment = 8;
};

template <> struct DirInfoTraits<nt::FileFullDirInfo> {
  static constexpr nt::FileInfoClass kInfoClass =
      nt::FileInfoClass::FileFullDirectoryInformation;
  static constexpr size_t kAlignment = 8;
};

template <> struct DirInfoTraits<nt::FileBothDirInfo> {
  static constexpr nt::FileInfoClass kInfoClass =
      nt::FileInfoClass::FileBothDirectoryInformation;
  static constexpr size_t kAlignment = 8;
};

template <> struct DirInfoTraits<nt::FileNamesInfo> {
  static constexpr nt::FileInfoClass kInfoClass =
      nt::FileInfoClass::FileNamesInformation;
  static constexpr size_t kAlignment = 4;
};

template <> struct DirInfoTraits<nt::FileIdBothDirInfo> {
  static constexpr nt::FileInfoClass kInfoClass =
      nt::FileInfoClass::FileIdBothDirectoryInformation;
  static constexpr size_t kAlignment = 8;
};

template <> struct DirInfoTraits<nt::FileIdFullDirInfo> {
  static constexpr nt::FileInfoClass kInfoClass =
      nt::FileInfoClass::FileIdFullDirectoryInformation;
  static constexpr size_t kAlignment = 8;
};

template <> struct DirInfoTraits<nt::FileIdExtdDirInfo> {
  static constexpr nt::FileInfoClass kInfoClass =
      nt::FileInfoClass::FileIdExtdDirectoryInformation;
  static constexpr size_t kAlignment = 8;
};

struct NtDirEntry {
  std::wstring name;
  int64_t end_of_file = 0;
  ULONG attributes = 0;
  ULONG ea_size = 0;
  int64_t file_id = 0;
  ULONG reparse_tag = 0;
  nt::FileId128 file_id_128{};
  bool has_metadata = false;
  bool has_ea_size = false;
  bool has_file_id_64 = false;
  bool has_file_id_128 = false;
};

// Walks a FILE_*_DIR_INFORMATION chain of `bytes_returned` bytes at the start
// of `buf`, invoking `on_entry` for each record. Every structural defect is a
// fatal failure (callers check HasFatalFailure()):
//   - `bytes_returned` larger than the buffer, or no room for one fixed header
//     (which also covers "zero entries returned"),
//   - a truncated trailing header,
//   - a NextEntryOffset that is misaligned, smaller than the fixed header, or
//     past the returned bytes,
//   - a FileNameLength that is odd or overruns its own record (bounded by
//     NextEntryOffset when non-zero, otherwise by the returned byte count).
template <typename EntryStruct, typename Callback>
void ParseDirBuffer(const std::vector<uint8_t> &buf, ULONG_PTR bytes_returned,
                    bool expect_single_entry, Callback &&on_entry,
                    size_t *entries_in_buffer_out = nullptr) {
  if (entries_in_buffer_out != nullptr) {
    *entries_in_buffer_out = 0;
  }
  ASSERT_LE(static_cast<size_t>(bytes_returned), buf.size())
      << "Driver reported iosb.information > buffer size";
  constexpr size_t kRequiredAlignment = DirInfoTraits<EntryStruct>::kAlignment;
  constexpr size_t kHeaderBytes = offsetof(EntryStruct, file_name);
  const size_t total_bytes = static_cast<size_t>(bytes_returned);
  size_t offset = 0;
  size_t count = 0;
  for (;;) {
    ASSERT_LE(offset, total_bytes);
    const size_t remaining = total_bytes - offset;
    ASSERT_GE(remaining, kHeaderBytes)
        << "Truncated directory entry header at offset " << offset << " ("
        << remaining << " bytes left of " << total_bytes
        << ", entries parsed: " << count << ")";
    const auto *entry =
        reinterpret_cast<const EntryStruct *>(buf.data() + offset);
    const size_t next_offset = static_cast<size_t>(entry->next_entry_offset);
    size_t entry_limit = remaining;
    if (next_offset != 0) {
      ASSERT_EQ(next_offset % kRequiredAlignment, size_t{0})
          << "NextEntryOffset " << next_offset << " is not aligned to "
          << kRequiredAlignment << " bytes";
      ASSERT_GE(next_offset, kHeaderBytes)
          << "NextEntryOffset " << next_offset
          << " is smaller than the fixed entry header (" << kHeaderBytes << ")";
      ASSERT_LE(next_offset, remaining)
          << "NextEntryOffset " << next_offset
          << " exceeds remaining buffer bytes (" << remaining << ")";
      entry_limit = next_offset;
    }
    const size_t name_bytes = static_cast<size_t>(entry->file_name_length);
    ASSERT_EQ(name_bytes % sizeof(wchar_t), size_t{0})
        << "Odd FileNameLength " << name_bytes;
    ASSERT_LE(name_bytes, entry_limit - kHeaderBytes)
        << "FileNameLength " << name_bytes << " overruns its entry (limit "
        << (entry_limit - kHeaderBytes) << " bytes)";
    if (expect_single_entry) {
      EXPECT_EQ(entry->next_entry_offset, 0u);
    }

    NtDirEntry parsed{};
    parsed.name.assign(entry->file_name, name_bytes / sizeof(wchar_t));
    if constexpr (!std::is_same_v<EntryStruct, nt::FileNamesInfo>) {
      parsed.end_of_file = entry->end_of_file.QuadPart;
      parsed.attributes = entry->file_attributes;
      parsed.has_metadata = true;
    }
    if constexpr (std::is_same_v<EntryStruct, nt::FileFullDirInfo> ||
                  std::is_same_v<EntryStruct, nt::FileBothDirInfo> ||
                  std::is_same_v<EntryStruct, nt::FileIdBothDirInfo> ||
                  std::is_same_v<EntryStruct, nt::FileIdFullDirInfo> ||
                  std::is_same_v<EntryStruct, nt::FileIdExtdDirInfo>) {
      parsed.ea_size = entry->ea_size;
      parsed.has_ea_size = true;
    }
    if constexpr (std::is_same_v<EntryStruct, nt::FileIdBothDirInfo> ||
                  std::is_same_v<EntryStruct, nt::FileIdFullDirInfo>) {
      parsed.file_id = entry->file_id.QuadPart;
      parsed.has_file_id_64 = true;
    }
    if constexpr (std::is_same_v<EntryStruct, nt::FileIdExtdDirInfo>) {
      parsed.reparse_tag = entry->reparse_point_tag;
      parsed.file_id_128 = entry->file_id;
      parsed.has_file_id_128 = true;
    }

    on_entry(std::move(parsed));
    ++count;
    if (entries_in_buffer_out != nullptr) {
      *entries_in_buffer_out = count;
    }

    if (next_offset == 0) {
      break;
    }
    offset += next_offset;
  }
}

template <typename EntryStruct, typename QueryFn, typename Callback>
NTSTATUS ForEachDirEntryWithQuery(size_t buffer_size, bool return_single_entry,
                                  QueryFn &&query_fn, Callback &&on_entry,
                                  size_t *call_count_out = nullptr) {
  std::vector<uint8_t> buf(buffer_size, 0);
  bool first = true;
  size_t calls = 0;
  for (;;) {
    nt::IoStatusBlock iosb{};
    const NTSTATUS st =
        query_fn(buf.data(), static_cast<ULONG>(buf.size()), first, &iosb);
    first = false;
    if (st == STATUS_NO_MORE_FILES) {
      if (call_count_out != nullptr) {
        *call_count_out = calls;
      }
      return STATUS_NO_MORE_FILES;
    }
    if (!nt::NtSuccess(st)) {
      if (call_count_out != nullptr) {
        *call_count_out = calls;
      }
      return st;
    }
    if (iosb.information == 0 || calls >= 100000) {
      ADD_FAILURE() << "Directory enumeration stalled or exceeded safety cap "
                       "(calls="
                    << calls << ", bytes=" << iosb.information << ")";
      if (call_count_out != nullptr) {
        *call_count_out = calls;
      }
      return STATUS_UNSUCCESSFUL;
    }
    ++calls;
    size_t count_this_call = 0;
    ParseDirBuffer<EntryStruct>(buf, iosb.information, return_single_entry,
                                on_entry, &count_this_call);
    if (::testing::Test::HasFatalFailure()) {
      if (call_count_out != nullptr) {
        *call_count_out = calls;
      }
      return st;
    }
    if (return_single_entry) {
      EXPECT_EQ(count_this_call, 1u);
    }
  }
}

template <typename EntryStruct, typename Callback>
NTSTATUS ForEachDirEntry(HANDLE dir_handle, size_t buffer_size,
                         bool return_single_entry,
                         std::wstring_view pattern_view, Callback &&on_entry,
                         size_t *call_count_out = nullptr) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryDirectoryFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }

  std::wstring pattern(pattern_view);
  nt::UnicodeString pattern_us{};
  nt::UnicodeString *pattern_ptr = nullptr;
  if (!pattern.empty()) {
    pattern_us = nt::MakeUnicodeString(pattern.data(), pattern.size());
    pattern_ptr = &pattern_us;
  }

  return ForEachDirEntryWithQuery<EntryStruct>(
      buffer_size, return_single_entry,
      [&](void *buf, ULONG len, bool first, nt::IoStatusBlock *iosb) {
        return ntdll.NtQueryDirectoryFile(
            dir_handle, nullptr, nullptr, nullptr, iosb, buf, len,
            DirInfoTraits<EntryStruct>::kInfoClass,
            return_single_entry ? TRUE : FALSE, first ? pattern_ptr : nullptr,
            first ? TRUE : FALSE);
      },
      std::forward<Callback>(on_entry), call_count_out);
}

template <typename EntryStruct>
NTSTATUS EnumerateDirClass(HANDLE dir_handle, size_t buffer_size,
                           bool return_single_entry,
                           std::wstring_view pattern_view,
                           std::vector<NtDirEntry> *out,
                           size_t *call_count_out = nullptr) {
  return ForEachDirEntry<EntryStruct>(
      dir_handle, buffer_size, return_single_entry, pattern_view,
      [out](NtDirEntry entry) { out->push_back(std::move(entry)); },
      call_count_out);
}

template <typename EntryStruct>
NTSTATUS EnumerateDirClassEx(HANDLE dir_handle, size_t buffer_size,
                             ULONG base_flags, bool restart_first,
                             std::vector<NtDirEntry> *out) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryDirectoryFileEx == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  const bool return_single = (base_flags & nt::kSlReturnSingleEntry) != 0;
  return ForEachDirEntryWithQuery<EntryStruct>(
      buffer_size, return_single,
      [&](void *buf, ULONG len, bool first, nt::IoStatusBlock *iosb) {
        const ULONG flags =
            base_flags | ((restart_first && first) ? nt::kSlRestartScan : 0UL);
        return ntdll.NtQueryDirectoryFileEx(
            dir_handle, nullptr, nullptr, nullptr, iosb, buf, len,
            DirInfoTraits<EntryStruct>::kInfoClass, flags, nullptr);
      },
      [out](NtDirEntry entry) { out->push_back(std::move(entry)); });
}

template <typename EntryStruct, typename CallFn>
NTSTATUS QuerySingleDirEntryWithCall(std::vector<uint8_t> *buf,
                                     std::wstring_view pattern_view,
                                     CallFn &&call_fn, NtDirEntry *out) {
  std::wstring pattern(pattern_view);
  nt::UnicodeString pattern_us{};
  nt::UnicodeString *pattern_ptr = nullptr;
  if (!pattern.empty()) {
    pattern_us = nt::MakeUnicodeString(pattern.data(), pattern.size());
    pattern_ptr = &pattern_us;
  }

  nt::IoStatusBlock iosb{};
  const NTSTATUS st =
      call_fn(buf->data(), static_cast<ULONG>(buf->size()), pattern_ptr, &iosb);
  if (!nt::NtSuccess(st)) {
    return st;
  }
  size_t count = 0;
  ParseDirBuffer<EntryStruct>(
      *buf, iosb.information, /*expect_single_entry=*/true,
      [out](NtDirEntry entry) {
        if (out != nullptr) {
          *out = std::move(entry);
        }
      },
      &count);
  if (::testing::Test::HasFatalFailure()) {
    return st;
  }
  EXPECT_EQ(count, 1u);
  return st;
}

template <typename EntryStruct>
NTSTATUS QuerySingleDirEntry(HANDLE dir_handle, std::vector<uint8_t> *buf,
                             bool restart_scan, std::wstring_view pattern_view,
                             NtDirEntry *out) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryDirectoryFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  return QuerySingleDirEntryWithCall<EntryStruct>(
      buf, pattern_view,
      [&](void *data, ULONG len, nt::UnicodeString *pat,
          nt::IoStatusBlock *iosb) {
        return ntdll.NtQueryDirectoryFile(
            dir_handle, nullptr, nullptr, nullptr, iosb, data, len,
            DirInfoTraits<EntryStruct>::kInfoClass,
            /*return_single_entry=*/TRUE, pat, restart_scan ? TRUE : FALSE);
      },
      out);
}

template <typename EntryStruct>
NTSTATUS QuerySingleDirEntryEx(HANDLE dir_handle, std::vector<uint8_t> *buf,
                               ULONG query_flags,
                               std::wstring_view pattern_view,
                               NtDirEntry *out) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryDirectoryFileEx == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  return QuerySingleDirEntryWithCall<EntryStruct>(
      buf, pattern_view,
      [&](void *data, ULONG len, nt::UnicodeString *pat,
          nt::IoStatusBlock *iosb) {
        return ntdll.NtQueryDirectoryFileEx(
            dir_handle, nullptr, nullptr, nullptr, iosb, data, len,
            DirInfoTraits<EntryStruct>::kInfoClass, query_flags, pat);
      },
      out);
}

std::vector<std::wstring> ExtractNames(const std::vector<NtDirEntry> &entries) {
  std::vector<std::wstring> names;
  names.reserve(entries.size());
  for (const auto &e : entries) {
    names.push_back(e.name);
  }
  return names;
}

const NtDirEntry *FindEntry(const std::vector<NtDirEntry> &entries,
                            std::wstring_view name) {
  for (const auto &e : entries) {
    if (e.name == name) {
      return &e;
    }
  }
  return nullptr;
}

template <typename EntryStruct>
::testing::AssertionResult
EnumerateDirPathExpectMultiset(std::wstring_view dir_path,
                               std::wstring_view pattern,
                               const std::vector<std::wstring> &expected,
                               std::vector<NtDirEntry> *entries_out = nullptr) {
  auto dh = OpenDirForEnum(dir_path);
  if (!dh) {
    return ::testing::AssertionFailure()
           << "Failed to open directory '" << core::WideToUtf8(dir_path)
           << "': " << core::FormatWin32Error(dh.error);
  }
  std::vector<NtDirEntry> entries;
  const NTSTATUS st = EnumerateDirClass<EntryStruct>(
      dh.value->get(), /*buffer_size=*/4096, /*return_single_entry=*/false,
      pattern, &entries);
  if (::testing::Test::HasFatalFailure()) {
    return ::testing::AssertionFailure()
           << "Fatal buffer parse failure during enumeration (pattern '"
           << core::WideToUtf8(pattern) << "')";
  }
  if (st != STATUS_NO_MORE_FILES) {
    return ::testing::AssertionFailure()
           << "Expected STATUS_NO_MORE_FILES for pattern '"
           << core::WideToUtf8(pattern) << "', got "
           << core::FormatNtStatus(st);
  }
  auto cmp = ::fstest::detail::CheckMultisetEq(
      "ExtractNames(entries)", "expected", ExtractNames(entries), expected);
  if (!cmp) {
    return cmp << "\nPattern: " << core::WideToUtf8(pattern);
  }
  if (entries_out != nullptr) {
    *entries_out = std::move(entries);
  }
  return ::testing::AssertionSuccess();
}

// Shared layout for the per-info-class NtQueryDirectoryFile tests: one regular
// file, one HIDDEN | ARCHIVE file, and one child directory.
struct InfoClassDir {
  std::wstring dir;
  std::wstring alpha_path;
  std::wstring beta_path;
  std::wstring child_dir;
};

constexpr size_t kInfoClassAlphaSize = 37;
constexpr size_t kInfoClassBetaSize = 123;

::testing::AssertionResult CreateInfoClassDir(std::wstring_view dir,
                                              InfoClassDir *out) {
  out->dir = std::wstring(dir);
  out->alpha_path = core::JoinPath(dir, L"alpha.txt");
  out->beta_path = core::JoinPath(dir, L"beta.dat");
  out->child_dir = core::JoinPath(dir, L"child_dir");

  ::SetLastError(ERROR_SUCCESS);
  if (::CreateDirectoryW(out->dir.c_str(), nullptr) == FALSE) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "CreateDirectoryW('" << core::WideToUtf8(out->dir)
           << "') failed: " << core::FormatWin32Error(err);
  }
  const std::string alpha_data(kInfoClassAlphaSize, 'A');
  if (auto wr = core::WriteFileContents(out->alpha_path, alpha_data); !wr) {
    return ::testing::AssertionFailure()
           << "Failed to write alpha.txt: " << core::FormatWin32Error(wr.error);
  }
  const std::string beta_data(kInfoClassBetaSize, 'B');
  if (auto wr = core::WriteFileContents(out->beta_path, beta_data,
                                        FILE_ATTRIBUTE_HIDDEN |
                                            FILE_ATTRIBUTE_ARCHIVE);
      !wr) {
    return ::testing::AssertionFailure()
           << "Failed to write beta.dat: " << core::FormatWin32Error(wr.error);
  }
  ::SetLastError(ERROR_SUCCESS);
  if (::CreateDirectoryW(out->child_dir.c_str(), nullptr) == FALSE) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "CreateDirectoryW('" << core::WideToUtf8(out->child_dir)
           << "') failed: " << core::FormatWin32Error(err);
  }
  return ::testing::AssertionSuccess();
}

// Enumerates `d.dir` with EntryStruct's information class and verifies the
// exact name multiset, record chaining/alignment (ParseDirBuffer), and - for
// classes that carry metadata - EndOfFile and FileAttributes. FileId fields
// are deliberately not checked here (see the dedicated FileId test).
template <typename EntryStruct>
void ExpectInfoClassListing(const InfoClassDir &d,
                            std::vector<NtDirEntry> *entries_out = nullptr) {
  const std::vector<std::wstring> expected = {L".", L"..", L"alpha.txt",
                                              L"beta.dat", L"child_dir"};
  std::vector<NtDirEntry> entries;
  ASSERT_TRUE(EnumerateDirPathExpectMultiset<EntryStruct>(d.dir, L"", expected,
                                                          &entries));
  if constexpr (!std::is_same_v<EntryStruct, nt::FileNamesInfo>) {
    const NtDirEntry *a = FindEntry(entries, L"alpha.txt");
    const NtDirEntry *b = FindEntry(entries, L"beta.dat");
    const NtDirEntry *c = FindEntry(entries, L"child_dir");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(a->end_of_file, static_cast<int64_t>(kInfoClassAlphaSize));
    EXPECT_EQ(b->end_of_file, static_cast<int64_t>(kInfoClassBetaSize));
    EXPECT_EQ(a->attributes & FILE_ATTRIBUTE_DIRECTORY, 0u);
    EXPECT_NE(MaskPolicyAttributes(b->attributes) & FILE_ATTRIBUTE_HIDDEN, 0u);
    EXPECT_NE(c->attributes & FILE_ATTRIBUTE_DIRECTORY, 0u);
  }
  if (entries_out != nullptr) {
    *entries_out = std::move(entries);
  }
}

// Fixture file names shared by the Win32 wildcard tests.
std::vector<std::wstring> WildcardFixtureFiles() {
  return {L"a", L"ab", L"abc.txt", L"axc.dat", L"a1c.txt", L"foo.bar.txt"};
}

::testing::AssertionResult CreateWildcardDir(std::wstring_view dir) {
  const std::wstring dir_str(dir);
  ::SetLastError(ERROR_SUCCESS);
  if (::CreateDirectoryW(dir_str.c_str(), nullptr) == FALSE) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "CreateDirectoryW('" << core::WideToUtf8(dir)
           << "') failed: " << core::FormatWin32Error(err);
  }
  for (const auto &f : WildcardFixtureFiles()) {
    if (auto created = CreateEmptyFile(core::JoinPath(dir, f)); !created) {
      return created;
    }
  }
  return ::testing::AssertionSuccess();
}

// Verifies FindFirstFileW/FindNextFileW(`dir`\`pattern`) yields exactly
// `expected` as a strict multiset.
::testing::AssertionResult
CheckWin32Pattern(std::wstring_view dir, std::wstring_view pattern,
                  const std::vector<std::wstring> &expected) {
  auto res = core::ListDirectoryWin32(dir, pattern);
  if (!res) {
    return ::testing::AssertionFailure()
           << "Pattern '" << core::WideToUtf8(pattern)
           << "': " << core::FormatWin32Error(res.error);
  }
  auto cmp = ::fstest::detail::CheckMultisetEq("*res.value", "expected",
                                               *res.value, expected);
  if (!cmp) {
    return cmp << "\nPattern: " << core::WideToUtf8(pattern);
  }
  return cmp;
}

// Creates `dir` containing `count` empty files named `<prefix>NN.txt` and
// returns the expected enumeration multiset including '.' and '..'.
::testing::AssertionResult
CreateNumberedFilesDir(std::wstring_view dir, std::wstring_view prefix,
                       int count, std::vector<std::wstring> *expected_out) {
  const std::wstring dir_str(dir);
  ::SetLastError(ERROR_SUCCESS);
  if (::CreateDirectoryW(dir_str.c_str(), nullptr) == FALSE) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "CreateDirectoryW('" << core::WideToUtf8(dir)
           << "') failed: " << core::FormatWin32Error(err);
  }
  expected_out->clear();
  expected_out->emplace_back(L".");
  expected_out->emplace_back(L"..");
  for (int i = 0; i < count; ++i) {
    std::wstring name(prefix);
    if (i < 10) {
      name.push_back(L'0');
    }
    name += std::to_wstring(i);
    name += L".txt";
    if (auto created = CreateEmptyFile(core::JoinPath(dir, name)); !created) {
      return created;
    }
    expected_out->push_back(std::move(name));
  }
  return ::testing::AssertionSuccess();
}

// Issues one NtQueryDirectoryFile on a FILE_FLAG_OVERLAPPED directory handle,
// passing `event` for completion, and returns the final status (the
// IO_STATUS_BLOCK status when the call pended). `*pended_out` reports whether
// the request returned STATUS_PENDING.
//
// `buf` and `iosb` are written by the kernel when the request completes, so on
// a wait timeout the request is cancelled and then drained without a bound
// before returning: a filesystem that never completes the IRP hangs here (and
// is reported by the outer runner timeout) instead of completing into freed
// memory.
NTSTATUS QueryDirectoryOverlapped(HANDLE dir_handle, HANDLE event,
                                  std::vector<uint8_t> *buf,
                                  nt::FileInfoClass info_class,
                                  bool restart_scan, nt::IoStatusBlock *iosb,
                                  bool *pended_out) {
  constexpr DWORD kCompletionTimeoutMs = 30000;
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryDirectoryFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  *pended_out = false;
  ::ResetEvent(event);
  NTSTATUS st = ntdll.NtQueryDirectoryFile(
      dir_handle, event, nullptr, nullptr, iosb, buf->data(),
      static_cast<ULONG>(buf->size()), info_class,
      /*return_single_entry=*/FALSE, nullptr, restart_scan ? TRUE : FALSE);
  if (st != STATUS_PENDING) {
    return st;
  }
  *pended_out = true;
  const DWORD wait = ::WaitForSingleObject(event, kCompletionTimeoutMs);
  if (wait != static_cast<DWORD>(WAIT_OBJECT_0)) {
    ::CancelIoEx(dir_handle, nullptr);
    ::WaitForSingleObject(event, INFINITE);
    ADD_FAILURE() << "Overlapped NtQueryDirectoryFile did not complete within "
                  << kCompletionTimeoutMs << " ms (wait result " << wait
                  << "); request cancelled";
    return STATUS_IO_TIMEOUT;
  }
  st = iosb->status;
  return st;
}

// FILE_INFO_BY_HANDLE_CLASS values for FileFullDirectoryInfo and
// FileFullDirectoryRestartInfo (Windows 8+). Spelled numerically so the suite
// still compiles when an embedder builds with _WIN32_WINNT < 0x0602.
constexpr FILE_INFO_BY_HANDLE_CLASS kByHandleFullDirectoryInfo =
    static_cast<FILE_INFO_BY_HANDLE_CLASS>(14);
constexpr FILE_INFO_BY_HANDLE_CLASS kByHandleFullDirectoryRestartInfo =
    static_cast<FILE_INFO_BY_HANDLE_CLASS>(15);

// Enumerates a directory handle through Win32 GetFileInformationByHandleEx,
// using `restart_class` for the first call and `continue_class` afterwards,
// until ERROR_NO_MORE_FILES. GetFileInformationByHandleEx does not report a
// byte count, so each call starts from a zeroed buffer and records are walked
// through their NextEntryOffset chain.
template <typename EntryStruct>
::testing::AssertionResult
EnumerateByHandleEx(HANDLE dir_handle, FILE_INFO_BY_HANDLE_CLASS restart_class,
                    FILE_INFO_BY_HANDLE_CLASS continue_class,
                    size_t buffer_size, std::vector<NtDirEntry> *out,
                    size_t *call_count_out = nullptr) {
  std::vector<uint8_t> buf(buffer_size, 0);
  bool first = true;
  for (size_t calls = 0; calls < 100000; ++calls) {
    std::fill(buf.begin(), buf.end(), uint8_t{0});
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok = ::GetFileInformationByHandleEx(
        dir_handle, first ? restart_class : continue_class, buf.data(),
        static_cast<DWORD>(buf.size()));
    const DWORD err = ::GetLastError();
    first = false;
    if (ok == FALSE) {
      if (err == static_cast<DWORD>(ERROR_NO_MORE_FILES)) {
        if (call_count_out != nullptr) {
          *call_count_out = calls;
        }
        return ::testing::AssertionSuccess();
      }
      return ::testing::AssertionFailure()
             << "GetFileInformationByHandleEx failed after " << calls
             << " successful calls: " << core::FormatWin32Error(err);
    }
    ParseDirBuffer<EntryStruct>(
        buf, buf.size(), /*expect_single_entry=*/false,
        [out](NtDirEntry entry) { out->push_back(std::move(entry)); });
    if (::testing::Test::HasFatalFailure()) {
      return ::testing::AssertionFailure()
             << "Fatal buffer parse failure on GetFileInformationByHandleEx "
                "call "
             << calls;
    }
  }
  return ::testing::AssertionFailure()
         << "GetFileInformationByHandleEx enumeration exceeded the safety cap";
}

// Verifies a full restart + continue enumeration through
// GetFileInformationByHandleEx (paginated by a small buffer), entry sizes, and
// that the restart class rewinds an exhausted handle.
template <typename EntryStruct>
void ExpectByHandleExEnumeration(std::wstring_view dir,
                                 FILE_INFO_BY_HANDLE_CLASS restart_class,
                                 FILE_INFO_BY_HANDLE_CLASS continue_class) {
  InfoClassDir d;
  ASSERT_TRUE(CreateInfoClassDir(dir, &d));
  std::vector<std::wstring> expected = {L".", L"..", L"alpha.txt", L"beta.dat",
                                        L"child_dir"};
  for (int i = 0; i < 12; ++i) {
    const std::wstring name = L"pad_" + std::to_wstring(i) + L".txt";
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(d.dir, name)));
    expected.push_back(name);
  }

  auto dh = OpenDirForEnum(d.dir);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  // 512 bytes hold only a few records, forcing several continuation calls.
  std::vector<NtDirEntry> entries;
  size_t calls = 0;
  ASSERT_TRUE(EnumerateByHandleEx<EntryStruct>(
      dh.value->get(), restart_class, continue_class,
      /*buffer_size=*/512, &entries, &calls));
  EXPECT_GE(calls, 2u);
  EXPECT_MULTISET_EQ(ExtractNames(entries), expected);

  const NtDirEntry *a = FindEntry(entries, L"alpha.txt");
  const NtDirEntry *b = FindEntry(entries, L"beta.dat");
  const NtDirEntry *c = FindEntry(entries, L"child_dir");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(a->end_of_file, static_cast<int64_t>(kInfoClassAlphaSize));
  EXPECT_EQ(b->end_of_file, static_cast<int64_t>(kInfoClassBetaSize));
  EXPECT_NE(c->attributes & FILE_ATTRIBUTE_DIRECTORY, 0u);

  // The exhausted handle keeps failing ERROR_NO_MORE_FILES on the continue
  // class, and the restart class rewinds it to a complete second pass.
  std::vector<uint8_t> probe(512, 0);
  EXPECT_WIN32_ERROR(::GetFileInformationByHandleEx(
                         dh.value->get(), continue_class, probe.data(),
                         static_cast<DWORD>(probe.size())),
                     ERROR_NO_MORE_FILES);

  std::vector<NtDirEntry> second_pass;
  ASSERT_TRUE(EnumerateByHandleEx<EntryStruct>(
      dh.value->get(), restart_class, continue_class,
      /*buffer_size=*/4096, &second_pass));
  EXPECT_MULTISET_EQ(ExtractNames(second_pass), expected);
}

} // namespace

// Spec: Win32 FindFirstFileW/FindNextFileW enumerates '.', '..', and all
// entries in a subdirectory as a strict multiset terminating in
// ERROR_NO_MORE_FILES.
TEST_F(DirEnumTest, FindFilesBasicStrictMultiset) {
  const std::wstring sub = TestPath(L"basic");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"foo")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"bar")));

  std::vector<std::wstring> names;
  ASSERT_TRUE(ForEachWin32FindEntry(core::JoinPath(sub, L"*"),
                                    /*use_ex_basic=*/false,
                                    [&](const WIN32_FIND_DATAW &data) {
                                      names.emplace_back(data.cFileName);
                                    }));

  const std::vector<std::wstring> expected = {L".", L"..", L"foo", L"bar"};
  EXPECT_MULTISET_EQ(names, expected);

  ASSERT_WIN32_TRUE(::DeleteFileW(core::JoinPath(sub, L"foo").c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(core::JoinPath(sub, L"bar").c_str()));
  ASSERT_WIN32_TRUE(::RemoveDirectoryW(sub.c_str()));
}

// Spec: [MS-FSA] 2.1.5.6.3 / Win32 FindFirstFileW — enumerating an empty
// non-root directory returns strictly '.' and '..' with
// FILE_ATTRIBUTE_DIRECTORY set.
TEST_F(DirEnumTest, EmptySubdirectoryHasDotAndDotDotOnly) {
  const std::wstring empty_dir = TestPath(L"empty_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(empty_dir.c_str(), nullptr));

  std::vector<std::wstring> names;
  ASSERT_TRUE(ForEachWin32FindEntry(
      core::JoinPath(empty_dir, L"*"), /*use_ex_basic=*/false,
      [&](const WIN32_FIND_DATAW &data) {
        names.emplace_back(data.cFileName);
        EXPECT_NE(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY, 0u)
            << "Expected FILE_ATTRIBUTE_DIRECTORY on '"
            << core::WideToUtf8(data.cFileName) << "'";
      }));

  const std::vector<std::wstring> expected = {L".", L".."};
  EXPECT_MULTISET_EQ(names, expected);
}

// Spec: [MS-FSA] 2.1.5.6.3 — volume root enumeration (Win32 and NT) omits '.'
// and '..' and yields each root entry exactly once.
TEST_F(DirEnumTest, MountRootOmitsDotAndDotDot) {
  REQUIRE_VOLUME_ROOT();

  auto win32_res = core::ListDirectoryWin32(VolumeRootPath(), L"*");
  ASSERT_TRUE(win32_res) << core::FormatWin32Error(win32_res.error);
  VerifyVolumeRootEntries(*win32_res.value, TempDir().Basename(), "Win32");

  auto root_h = OpenRootForEnum(Root());
  ASSERT_TRUE(root_h) << core::FormatWin32Error(root_h.error);

  std::vector<NtDirEntry> nt_entries;
  const NTSTATUS st = EnumerateDirClass<nt::FileBothDirInfo>(
      root_h.value->get(), /*buffer_size=*/8192, /*return_single_entry=*/false,
      L"", &nt_entries);
  ASSERT_NT_STATUS(st, STATUS_NO_MORE_FILES);
  VerifyVolumeRootEntries(ExtractNames(nt_entries), TempDir().Basename(), "NT");
}

// Spec: [MS-FSCC] 2.4.10 FileDirectoryInformation — NtQueryDirectoryFile
// returns the exact name multiset with 8-byte aligned records and correct
// EndOfFile / FileAttributes.
TEST_F(DirEnumTest, NtQueryDirectoryFileDirectoryInfoClass) {
  InfoClassDir d;
  ASSERT_TRUE(CreateInfoClassDir(TestPath(L"class_directory"), &d));
  ExpectInfoClassListing<nt::FileDirectoryInfo>(d);
}

// Spec: [MS-FSCC] 2.4.15 FileFullDirectoryInformation — NtQueryDirectoryFile
// returns the exact name multiset with 8-byte aligned records and correct
// EndOfFile / FileAttributes.
TEST_F(DirEnumTest, NtQueryDirectoryFileFullDirectoryInfoClass) {
  InfoClassDir d;
  ASSERT_TRUE(CreateInfoClassDir(TestPath(L"class_full"), &d));
  ExpectInfoClassListing<nt::FileFullDirInfo>(d);
}

// Spec: [MS-FSCC] 2.4.8 FileBothDirectoryInformation — NtQueryDirectoryFile
// returns the exact name multiset with 8-byte aligned records and correct
// EndOfFile / FileAttributes.
TEST_F(DirEnumTest, NtQueryDirectoryFileBothDirectoryInfoClass) {
  InfoClassDir d;
  ASSERT_TRUE(CreateInfoClassDir(TestPath(L"class_both"), &d));
  ExpectInfoClassListing<nt::FileBothDirInfo>(d);
}

// Spec: [MS-FSCC] 2.4.33 FileNamesInformation — NtQueryDirectoryFile returns
// the exact name multiset with 4-byte aligned name-only records.
TEST_F(DirEnumTest, NtQueryDirectoryFileNamesInfoClass) {
  InfoClassDir d;
  ASSERT_TRUE(CreateInfoClassDir(TestPath(L"class_names"), &d));
  ExpectInfoClassListing<nt::FileNamesInfo>(d);
}

// Spec: [MS-FSCC] 2.4.22 FileIdBothDirectoryInformation — NtQueryDirectoryFile
// returns the exact name multiset with 8-byte aligned records and correct
// EndOfFile / FileAttributes (FileId values are covered separately below).
TEST_F(DirEnumTest, NtQueryDirectoryFileIdBothDirectoryInfoClass) {
  InfoClassDir d;
  ASSERT_TRUE(CreateInfoClassDir(TestPath(L"class_id_both"), &d));
  ExpectInfoClassListing<nt::FileIdBothDirInfo>(d);
}

// Spec: [MS-FSCC] 2.4.22 FileIdBothDirectoryInformation — the 64-bit FileId of
// every entry equals the file's FileInternalInformation IndexNumber.
TEST_F(DirEnumTest, NtQueryDirectoryFileIdBothDirectoryInfoReportsFileIds) {
  InfoClassDir d;
  ASSERT_TRUE(CreateInfoClassDir(TestPath(L"class_id_both_ids"), &d));

  const int64_t alpha_id =
      QueryFileIds(d.alpha_path, /*is_dir=*/false, /*include_id128=*/false)
          .id64;
  const int64_t beta_id =
      QueryFileIds(d.beta_path, /*is_dir=*/false, /*include_id128=*/false).id64;
  const int64_t child_dir_id =
      QueryFileIds(d.child_dir, /*is_dir=*/true, /*include_id128=*/false).id64;
  ASSERT_NE(alpha_id, 0LL);
  ASSERT_NE(beta_id, 0LL);
  ASSERT_NE(child_dir_id, 0LL);

  std::vector<NtDirEntry> entries;
  const std::vector<std::wstring> expected = {L".", L"..", L"alpha.txt",
                                              L"beta.dat", L"child_dir"};
  ASSERT_TRUE(EnumerateDirPathExpectMultiset<nt::FileIdBothDirInfo>(
      d.dir, L"", expected, &entries));
  const NtDirEntry *a = FindEntry(entries, L"alpha.txt");
  const NtDirEntry *b = FindEntry(entries, L"beta.dat");
  const NtDirEntry *c = FindEntry(entries, L"child_dir");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(a->file_id, alpha_id);
  EXPECT_EQ(b->file_id, beta_id);
  EXPECT_EQ(c->file_id, child_dir_id);
}

// Spec: [MS-FSA] 2.1.5.6.3 — ReturnSingleEntry=TRUE returns exactly one entry
// per call with NextEntryOffset==0 until STATUS_NO_MORE_FILES.
TEST_F(DirEnumTest, NtQueryDirectoryReturnSingleEntry) {
  const std::wstring sub = TestPath(L"single_entry");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  std::vector<std::wstring> expected = {L".", L".."};
  for (int i = 0; i < 5; ++i) {
    const std::wstring name = L"f" + std::to_wstring(i) + L".txt";
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name)));
    expected.push_back(name);
  }

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  std::vector<NtDirEntry> actual;
  const NTSTATUS st = EnumerateDirClass<nt::FileBothDirInfo>(
      dh.value->get(), /*buffer_size=*/4096, /*return_single_entry=*/true, L"",
      &actual);
  ASSERT_NT_STATUS(st, STATUS_NO_MORE_FILES);
  EXPECT_MULTISET_EQ(ExtractNames(actual), expected);
}

// Spec: [MS-FSA] 2.1.5.6.3 — RestartScan=TRUE after a partial scan resets the
// directory cursor to the beginning with zero duplicates or omissions.
TEST_F(DirEnumTest, NtQueryDirectoryRestartScan) {
  const std::wstring sub = TestPath(L"restart_scan");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  std::vector<std::wstring> expected = {L".", L".."};
  for (int i = 0; i < 6; ++i) {
    const std::wstring name = L"r" + std::to_wstring(i) + L".txt";
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name)));
    expected.push_back(name);
  }

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  // Partially consume two entries.
  std::vector<uint8_t> buf(4096, 0);
  for (int step = 0; step < 2; ++step) {
    NtDirEntry unused_entry{};
    ASSERT_NT_SUCCESS(QuerySingleDirEntry<nt::FileBothDirInfo>(
        dh.value->get(), &buf, /*restart_scan=*/(step == 0), L"",
        &unused_entry));
  }

  // RestartScan = TRUE must reset cursor to the beginning and return the
  // complete multiset with zero duplicates.
  std::vector<NtDirEntry> restarted;
  const NTSTATUS st = EnumerateDirClass<nt::FileBothDirInfo>(
      dh.value->get(), /*buffer_size=*/4096, /*return_single_entry=*/false, L"",
      &restarted);
  ASSERT_NT_STATUS(st, STATUS_NO_MORE_FILES);
  EXPECT_MULTISET_EQ(ExtractNames(restarted), expected);
}

// Spec: [MS-FSA] 2.1.5.6.3 — undersized buffers fail with
// STATUS_INFO_LENGTH_MISMATCH / STATUS_BUFFER_OVERFLOW and a tight buffer
// paginates across all directory entries with zero duplicates or omissions.
TEST_F(DirEnumTest, NtQueryDirectorySmallBufferPagination) {
  const std::wstring sub = TestPath(L"small_buf");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  std::vector<std::wstring> expected = {L".", L".."};
  for (int i = 0; i < 20; ++i) {
    wchar_t name_buf[32];
    std::swprintf(name_buf, sizeof(name_buf) / sizeof(name_buf[0]),
                  L"item_%02d.txt", i);
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name_buf)));
    expected.emplace_back(name_buf);
  }

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryDirectoryFile, nullptr);

  // 1. Sub-header buffer (< sizeof(FileBothDirInfo)) fails
  // STATUS_INFO_LENGTH_MISMATCH.
  {
    auto dh = OpenDirForEnum(sub);
    ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);
    alignas(8) std::array<uint8_t, 16> tiny_buf{};
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryDirectoryFile(
        dh.value->get(), nullptr, nullptr, nullptr, &iosb, tiny_buf.data(),
        static_cast<ULONG>(tiny_buf.size()),
        nt::FileInfoClass::FileBothDirectoryInformation,
        /*return_single_entry=*/FALSE, nullptr, /*restart_scan=*/TRUE);
    EXPECT_NT_STATUS(st, STATUS_INFO_LENGTH_MISMATCH);
  }

  // 2. Buffer sized to sizeof(FileBothDirInfo) (96 B: 94-byte fixed header
  // plus one name character), too small for the full 116-byte entry for
  // "item_00.txt" -> STATUS_BUFFER_OVERFLOW. Partial-name contents and cursor
  // behavior are covered by
  // NtQueryDirectoryBufferOverflowReturnsPartialNameAndPreservesCursor.
  {
    auto dh = OpenDirForEnum(sub);
    ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);
    std::wstring pat = L"item_00.txt";
    nt::UnicodeString pat_us = nt::MakeUnicodeString(pat.data(), pat.size());
    std::vector<uint8_t> short_buf(sizeof(nt::FileBothDirInfo), 0);
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryDirectoryFile(
        dh.value->get(), nullptr, nullptr, nullptr, &iosb, short_buf.data(),
        static_cast<ULONG>(short_buf.size()),
        nt::FileInfoClass::FileBothDirectoryInformation,
        /*return_single_entry=*/TRUE, &pat_us, /*restart_scan=*/TRUE);
    EXPECT_NT_STATUS(st, STATUS_BUFFER_OVERFLOW);
  }

  // 3. Tight buffer sized to fit "." + ".." (194 B) and strictly 1 file entry
  //    per subsequent call (~116 B each, 2 entries = 236 B > 224 B) paginates
  //    across all 20 files with zero duplicates and zero omissions.
  {
    auto dh = OpenDirForEnum(sub);
    ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);
    constexpr size_t kTightBufferSize = 224;
    std::vector<NtDirEntry> entries;
    size_t call_count = 0;
    const NTSTATUS st = EnumerateDirClass<nt::FileBothDirInfo>(
        dh.value->get(), kTightBufferSize, /*return_single_entry=*/false, L"",
        &entries, &call_count);
    ASSERT_NT_STATUS(st, STATUS_NO_MORE_FILES);
    EXPECT_GE(call_count, 10u);
    EXPECT_MULTISET_EQ(ExtractNames(entries), expected);
  }
}

// Spec: [MS-FSA] 2.1.5.6.3 / FindFirstFileW & NtQueryDirectoryFile —
// multi-buffer Win32 and NT enumeration of large directories produces the
// exact expected entry multiset with no pagination skips or duplicates.
TEST_F(DirEnumTest, LargeDirectoryEntriesStrictMultiset) {
  const uint32_t count = std::max<uint32_t>(Config().large_dir_entry_count, 1u);
  const std::wstring sub = TestPath(L"large_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  std::vector<std::wstring> expected;
  expected.reserve(static_cast<size_t>(count) + 2);
  expected.emplace_back(L".");
  expected.emplace_back(L"..");

  for (uint32_t i = 0; i < count; ++i) {
    wchar_t name_buf[32];
    std::swprintf(name_buf, sizeof(name_buf) / sizeof(name_buf[0]),
                  L"entry_%04u.dat", static_cast<unsigned int>(i));
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name_buf)));
    expected.emplace_back(name_buf);
  }

  auto win32_res = core::ListDirectoryWin32(sub, L"*");
  ASSERT_TRUE(win32_res) << core::FormatWin32Error(win32_res.error);
  EXPECT_EQ(win32_res.value->size(), static_cast<size_t>(count) + 2);
  EXPECT_MULTISET_EQ(*win32_res.value, expected);

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);
  std::vector<NtDirEntry> nt_entries;
  const NTSTATUS st = EnumerateDirClass<nt::FileIdBothDirInfo>(
      dh.value->get(), /*buffer_size=*/4096, /*return_single_entry=*/false, L"",
      &nt_entries);
  ASSERT_NT_STATUS(st, STATUS_NO_MORE_FILES);
  EXPECT_EQ(nt_entries.size(), static_cast<size_t>(count) + 2);
  EXPECT_MULTISET_EQ(ExtractNames(nt_entries), expected);
}

// Spec: [MS-FSA] 2.1.4.4 / Win32 FindFirstFileW — '*' matches every entry
// including '.' and '..'.
TEST_F(DirEnumTest, Win32WildcardStarMatchesAllEntries) {
  const std::wstring sub = TestPath(L"wildcard_star");
  ASSERT_TRUE(CreateWildcardDir(sub));

  std::vector<std::wstring> all_expected = {L".", L".."};
  const std::vector<std::wstring> files = WildcardFixtureFiles();
  all_expected.insert(all_expected.end(), files.begin(), files.end());
  EXPECT_TRUE(CheckWin32Pattern(sub, L"*", all_expected));
}

// Spec: [MS-FSA] 2.1.4.4 / Win32 FindFirstFileW — '*.ext' matches every name
// whose final extension is 'ext', including names with embedded dots.
TEST_F(DirEnumTest, Win32WildcardStarWithExtension) {
  const std::wstring sub = TestPath(L"wildcard_star_ext");
  ASSERT_TRUE(CreateWildcardDir(sub));

  EXPECT_TRUE(CheckWin32Pattern(sub, L"*.txt",
                                {L"abc.txt", L"a1c.txt", L"foo.bar.txt"}));
}

// Spec: [MS-FSA] 2.1.4.4 / Win32 FindFirstFileW — '?' matches exactly one
// character in the middle of a name ('a?c.*' excludes 'a', 'ab').
TEST_F(DirEnumTest, Win32WildcardQuestionMarkMatchesSingleCharacter) {
  const std::wstring sub = TestPath(L"wildcard_qm");
  ASSERT_TRUE(CreateWildcardDir(sub));

  EXPECT_TRUE(
      CheckWin32Pattern(sub, L"a?c.*", {L"abc.txt", L"axc.dat", L"a1c.txt"}));
}

// Spec: [MS-FSA] 2.1.4.4 / Win32 FindFirstFileW — a pattern without wildcards
// matches only the entry with that exact name.
TEST_F(DirEnumTest, Win32WildcardLiteralNameMatchesSingleEntry) {
  const std::wstring sub = TestPath(L"wildcard_literal");
  ASSERT_TRUE(CreateWildcardDir(sub));

  EXPECT_TRUE(CheckWin32Pattern(sub, L"axc.dat", {L"axc.dat"}));
}

// Spec: Win32 FindFirstFileW — a pattern matching no entry fails with
// ERROR_FILE_NOT_FOUND.
TEST_F(DirEnumTest, Win32WildcardNoMatchFailsFileNotFound) {
  const std::wstring sub = TestPath(L"wildcard_nomatch");
  ASSERT_TRUE(CreateWildcardDir(sub));

  auto res = core::ListDirectoryWin32(sub, L"zzz_nomatch*.bin");
  EXPECT_FALSE(res);
  EXPECT_WIN32_ERROR_CODE(res.error, ERROR_FILE_NOT_FOUND);
}

// Spec: [MS-FSA] 2.1.5.6.3 — the first NtQueryDirectoryFile call with a
// pattern matching no entry fails with STATUS_NO_SUCH_FILE.
TEST_F(DirEnumTest, NtQueryDirectoryNoMatchFailsNoSuchFile) {
  const std::wstring sub = TestPath(L"nt_nomatch");
  ASSERT_TRUE(CreateWildcardDir(sub));

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);
  std::vector<NtDirEntry> entries;
  const NTSTATUS st = EnumerateDirClass<nt::FileBothDirInfo>(
      dh.value->get(), /*buffer_size=*/4096, /*return_single_entry=*/false,
      L"zzz_nomatch*.bin", &entries);
  EXPECT_NT_STATUS(st, STATUS_NO_SUCH_FILE);
  EXPECT_TRUE(entries.empty());
}

// Spec: [MS-FSA] 2.1.4.4 — NtQueryDirectoryFile honors legacy DOS wildcards
// DOS_STAR ('<'), DOS_QM ('>'), and DOS_DOT ('"').
TEST_F(DirEnumTest, NtQueryDirectoryDosWildcards) {
  const std::wstring sub = TestPath(L"dos_wildcards");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::vector<std::wstring> files = {
      L"a",    L"ab",     L"abc",     L"abcd",        L"a.txt",  L"foo",
      L"foo1", L"foobar", L"foo.txt", L"foo.bar.txt", L"footxt",
  };
  for (const auto &f : files) {
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, f)));
  }

  // DOS_STAR ('<'): matches 0+ chars up to the final '.' in the name.
  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(
      sub, L"foo<", {L"foo", L"foo1", L"foobar", L"footxt"}));
  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(
      sub, L"foo<.txt", {L"foo.txt", L"foo.bar.txt"}));

  // DOS_QM ('>'): matches 1 char, or 0 chars at end of name or before '.'.
  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(
      sub, L"a>>", {L"a", L"ab", L"abc"}));
  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(
      sub, L"a>>.txt", {L"a.txt"}));

  // DOS_DOT ('"'): matches literal '.' or 0 chars at end of name.
  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(sub, L"foo\"",
                                                                  {L"foo"}));
  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(
      sub, L"foo\"txt", {L"foo.txt"}));
}

// Spec: FindFirstFileExW(FindExInfoBasic) enumerates every entry exactly once.
// (That cAlternateFileName stays empty at this info level is decided by
// kernel32, which simply requests an information class without short names, so
// it is not asserted as a filesystem contract.)
TEST_F(DirEnumTest, FindFirstFileExBasicInfoLevel) {
  const std::wstring sub = TestPath(L"find_ex_basic");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(CreateEmptyFile(
      core::JoinPath(sub, L"long_filename_for_basic_info_level.txt")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"short.txt")));

  std::vector<std::wstring> names;
  ASSERT_TRUE(ForEachWin32FindEntry(core::JoinPath(sub, L"*"),
                                    /*use_ex_basic=*/true,
                                    [&](const WIN32_FIND_DATAW &data) {
                                      names.emplace_back(data.cFileName);
                                    }));

  const std::vector<std::wstring> expected = {
      L".", L"..", L"long_filename_for_basic_info_level.txt", L"short.txt"};
  EXPECT_MULTISET_EQ(names, expected);
}

// Spec: Directory enumeration metadata (sizes, attributes, 100ns timestamps)
// matches GetFileInformationByHandle for both files and subdirectories.
TEST_F(DirEnumTest, FindFilesMetadataMatchesGetFileInformation) {
  const std::wstring sub = TestPath(L"meta_match");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring file_path = core::JoinPath(sub, L"sample.dat");
  const std::wstring nested_dir = core::JoinPath(sub, L"nested_dir");
  const std::string payload(1234, 'M');
  ASSERT_TRUE(core::WriteFileContents(
      file_path, payload, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(nested_dir.c_str(), nullptr));

  // Set explicit 100ns timestamps on sample.dat (2020-01-01 + 100ns ticks).
  {
    auto fh =
        core::CreateWin32File(file_path, FILE_WRITE_ATTRIBUTES, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);
    const FILETIME ft_create = helpers::U64ToFileTime(132223104001234567ULL);
    const FILETIME ft_access = helpers::U64ToFileTime(132223140002345678ULL);
    const FILETIME ft_write = helpers::U64ToFileTime(132223176003456789ULL);
    ASSERT_WIN32_TRUE(
        ::SetFileTime(fh.value->get(), &ft_create, &ft_access, &ft_write));
  }

  auto file_bhfi = core::QueryInfoByPath(file_path, /*backup_semantics=*/false);
  ASSERT_TRUE(file_bhfi) << core::FormatWin32Error(file_bhfi.error);
  auto dir_bhfi = core::QueryInfoByPath(nested_dir, /*backup_semantics=*/true);
  ASSERT_TRUE(dir_bhfi) << core::FormatWin32Error(dir_bhfi.error);

  bool saw_file = false;
  bool saw_dir = false;
  ASSERT_TRUE(ForEachWin32FindEntry(
      core::JoinPath(sub, L"*"), /*use_ex_basic=*/false,
      [&](const WIN32_FIND_DATAW &data) {
        const std::wstring_view name(data.cFileName);
        if (name == L"sample.dat") {
          saw_file = true;
          EXPECT_EQ(data.nFileSizeHigh, file_bhfi.value->nFileSizeHigh);
          EXPECT_EQ(data.nFileSizeLow, file_bhfi.value->nFileSizeLow);
          EXPECT_EQ(data.nFileSizeLow, static_cast<DWORD>(payload.size()));
          EXPECT_EQ(MaskPolicyAttributes(data.dwFileAttributes),
                    MaskPolicyAttributes(file_bhfi.value->dwFileAttributes));
          EXPECT_EQ(core::FileTimeToU64(data.ftCreationTime),
                    core::FileTimeToU64(file_bhfi.value->ftCreationTime));
          EXPECT_EQ(core::FileTimeToU64(data.ftLastAccessTime),
                    core::FileTimeToU64(file_bhfi.value->ftLastAccessTime));
          EXPECT_EQ(core::FileTimeToU64(data.ftLastWriteTime),
                    core::FileTimeToU64(file_bhfi.value->ftLastWriteTime));
        } else if (name == L"nested_dir") {
          saw_dir = true;
          EXPECT_EQ(MaskPolicyAttributes(data.dwFileAttributes),
                    MaskPolicyAttributes(dir_bhfi.value->dwFileAttributes));
          EXPECT_NE(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY, 0u);
          EXPECT_EQ(core::FileTimeToU64(data.ftCreationTime),
                    core::FileTimeToU64(dir_bhfi.value->ftCreationTime));
          EXPECT_EQ(core::FileTimeToU64(data.ftLastWriteTime),
                    core::FileTimeToU64(dir_bhfi.value->ftLastWriteTime));
        }
      }));
  EXPECT_TRUE(saw_file);
  EXPECT_TRUE(saw_dir);
}

// Spec: [MS-FSA] 2.1.5.6.3 — enumerating a non-directory file fails with
// STATUS_INVALID_PARAMETER on NT handles and ERROR_DIRECTORY on FindFirstFileW.
TEST_F(DirEnumTest, EnumerateRegularFileFailsNotDirectory) {
  const std::wstring file_path = TestPath(L"regular.dat");
  ASSERT_TRUE(core::WriteFileContents(file_path, "not a directory"));

  auto fh = core::CreateWin32File(file_path, FILE_READ_DATA | SYNCHRONIZE,
                                  core::kShareAll, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  std::vector<NtDirEntry> unused_entries;
  const NTSTATUS st = EnumerateDirClass<nt::FileBothDirInfo>(
      fh.value->get(), /*buffer_size=*/1024, /*return_single_entry=*/false, L"",
      &unused_entries);
  EXPECT_NT_STATUS(st, STATUS_INVALID_PARAMETER);
  fh.value->reset();

  const std::wstring star_query = file_path + L"\\*";
  WIN32_FIND_DATAW data{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueFindHandle find(::FindFirstFileW(star_query.c_str(), &data));
  const DWORD err = ::GetLastError();
  EXPECT_FALSE(find);
  EXPECT_WIN32_ERROR_CODE(err, ERROR_DIRECTORY);
}

// Spec: [MS-FSA] 2.1.5.6.3 / NtQueryDirectoryFile — mid-scan file creations
// and deletions terminate cleanly with no duplicate names and never omit
// unmodified entries.
TEST_F(DirEnumTest, MutationDuringEnumerationNoCrashNoDuplicates) {
  const std::wstring sub = TestPath(L"mutation");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  std::vector<std::wstring> keep_files;
  for (int i = 0; i < 20; ++i) {
    wchar_t name_buf[32];
    std::swprintf(name_buf, sizeof(name_buf) / sizeof(name_buf[0]),
                  L"keep_%02d.txt", i);
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name_buf)));
    keep_files.emplace_back(name_buf);
  }
  for (int i = 0; i < 5; ++i) {
    const std::wstring name = L"del_" + std::to_wstring(i) + L".txt";
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name)));
  }

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  std::map<std::wstring, size_t> seen_counts;
  bool mutated = false;
  const NTSTATUS st = ForEachDirEntry<nt::FileBothDirInfo>(
      dh.value->get(), /*buffer_size=*/512, /*return_single_entry=*/true, L"",
      [&](NtDirEntry entry) {
        ++seen_counts[entry.name];
        if (!mutated && seen_counts.size() >= 5) {
          mutated = true;
          for (int i = 0; i < 5; ++i) {
            const std::wstring del_path =
                core::JoinPath(sub, L"del_" + std::to_wstring(i) + L".txt");
            EXPECT_WIN32_TRUE(::DeleteFileW(del_path.c_str()));
            const std::wstring add_path =
                core::JoinPath(sub, L"added_" + std::to_wstring(i) + L".txt");
            EXPECT_TRUE(CreateEmptyFile(add_path));
          }
        }
      });

  EXPECT_TRUE(mutated);
  EXPECT_NT_STATUS(st, STATUS_NO_MORE_FILES)
      << "Enumeration did not reach STATUS_NO_MORE_FILES cleanly";
  for (const auto &[name, count] : seen_counts) {
    EXPECT_EQ(count, 1u) << "Duplicate filename emitted during mutation: "
                         << core::WideToUtf8(name);
  }
  EXPECT_EQ(seen_counts[L"."], 1u);
  EXPECT_EQ(seen_counts[L".."], 1u);
  for (const auto &k : keep_files) {
    EXPECT_EQ(seen_counts[k], 1u)
        << "Unmodified entry missing after mid-scan mutation: "
        << core::WideToUtf8(k);
  }
}

// Spec: [MS-FSCC] 2.4.23/24 — FileIdFullDirectoryInformation (64-bit ID) and
// FileIdExtdDirectoryInformation (128-bit ID + ReparseTag) match file queries.
TEST_F(DirEnumTest, NtQueryDirectoryIdFullAndIdExtdInfoClasses) {
  const std::wstring sub = TestPath(L"id_full_extd");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::string data_a(19, 'X');
  const std::string data_b(53, 'Y');
  const std::wstring path_a = core::JoinPath(sub, L"file_a.dat");
  const std::wstring path_b = core::JoinPath(sub, L"file_b.dat");
  const std::wstring path_d = core::JoinPath(sub, L"nested");

  ASSERT_TRUE(core::WriteFileContents(path_a, data_a));
  ASSERT_TRUE(core::WriteFileContents(path_b, data_b));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(path_d.c_str(), nullptr));

  const ExpectedFileIds ids_a =
      QueryFileIds(path_a, /*is_dir=*/false, /*include_id128=*/true);
  const ExpectedFileIds ids_b =
      QueryFileIds(path_b, /*is_dir=*/false, /*include_id128=*/true);
  const ExpectedFileIds ids_d =
      QueryFileIds(path_d, /*is_dir=*/true, /*include_id128=*/true);
  ASSERT_NE(ids_a.id64, 0LL);
  ASSERT_NE(ids_b.id64, 0LL);
  ASSERT_NE(ids_d.id64, 0LL);
  ASSERT_FALSE(helpers::AllBytesEqual(ids_a.id128.identifier,
                                      sizeof(ids_a.id128.identifier), 0));
  ASSERT_FALSE(helpers::AllBytesEqual(ids_b.id128.identifier,
                                      sizeof(ids_b.id128.identifier), 0));
  ASSERT_FALSE(helpers::AllBytesEqual(ids_d.id128.identifier,
                                      sizeof(ids_d.id128.identifier), 0));
  const std::vector<std::wstring> expected = {L".", L"..", L"file_a.dat",
                                              L"file_b.dat", L"nested"};

  auto verify_id_class = [&](auto tag) {
    using EntryType = decltype(tag);
    std::vector<NtDirEntry> entries;
    ASSERT_TRUE(EnumerateDirPathExpectMultiset<EntryType>(sub, L"", expected,
                                                          &entries));

    const NtDirEntry *ea = FindEntry(entries, L"file_a.dat");
    const NtDirEntry *eb = FindEntry(entries, L"file_b.dat");
    const NtDirEntry *ed = FindEntry(entries, L"nested");
    ASSERT_NE(ea, nullptr);
    ASSERT_NE(eb, nullptr);
    ASSERT_NE(ed, nullptr);
    EXPECT_EQ(ea->end_of_file, static_cast<int64_t>(data_a.size()));
    EXPECT_EQ(eb->end_of_file, static_cast<int64_t>(data_b.size()));

    if constexpr (std::is_same_v<EntryType, nt::FileIdFullDirInfo>) {
      EXPECT_EQ(ea->file_id, ids_a.id64);
      EXPECT_EQ(eb->file_id, ids_b.id64);
      EXPECT_EQ(ed->file_id, ids_d.id64);
    } else {
      EXPECT_EQ(ea->reparse_tag, 0u);
      EXPECT_EQ(eb->reparse_tag, 0u);
      EXPECT_EQ(ed->reparse_tag, 0u);
      EXPECT_EQ(std::memcmp(ea->file_id_128.identifier, ids_a.id128.identifier,
                            sizeof(nt::FileId128)),
                0);
      EXPECT_EQ(std::memcmp(eb->file_id_128.identifier, ids_b.id128.identifier,
                            sizeof(nt::FileId128)),
                0);
      EXPECT_EQ(std::memcmp(ed->file_id_128.identifier, ids_d.id128.identifier,
                            sizeof(nt::FileId128)),
                0);
    }
  };

  // FileIdFullDirectoryInformation (class 38) and
  // FileIdExtdDirectoryInformation (class 60).
  ASSERT_NO_FATAL_FAILURE(verify_id_class(nt::FileIdFullDirInfo{}));
  ASSERT_NO_FATAL_FAILURE(verify_id_class(nt::FileIdExtdDirInfo{}));
}

// Spec: [MS-FSA] 2.1.5.6.3 — the search pattern bound on the first query is
// immutable on subsequent calls and preserved across RestartScan=TRUE.
TEST_F(DirEnumTest, NtQueryDirectoryPatternImmutabilityAndRestartScan) {
  const std::wstring sub = TestPath(L"pattern_immut");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"a1.txt")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"a2.txt")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"a3.txt")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"b1.dat")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"b2.dat")));

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  std::vector<uint8_t> buf(4096, 0);
  std::vector<std::wstring> pass1;

  // Call 1 binds "*.txt" onto the handle.
  {
    NtDirEntry entry{};
    ASSERT_NT_SUCCESS(QuerySingleDirEntry<nt::FileBothDirInfo>(
        dh.value->get(), &buf, /*restart_scan=*/true, L"*.txt", &entry));
    pass1.push_back(std::move(entry.name));
  }

  // Subsequent calls passing "*.dat" with RestartScan=FALSE must ignore "*.dat"
  // and keep enumerating "*.txt" per [MS-FSA] 2.1.5.6.3.
  bool pass1_done = false;
  for (int iter = 0; iter < 1000; ++iter) {
    NtDirEntry entry{};
    const NTSTATUS st = QuerySingleDirEntry<nt::FileBothDirInfo>(
        dh.value->get(), &buf, /*restart_scan=*/false, L"*.dat", &entry);
    if (st == STATUS_NO_MORE_FILES) {
      pass1_done = true;
      break;
    }
    ASSERT_NT_SUCCESS(st);
    pass1.push_back(std::move(entry.name));
  }
  EXPECT_TRUE(pass1_done);

  const std::vector<std::wstring> expected_txt = {L"a1.txt", L"a2.txt",
                                                  L"a3.txt"};
  EXPECT_MULTISET_EQ(pass1, expected_txt);

  // RestartScan=TRUE resets the cursor back to the start of the bound "*.txt"
  // query pattern.
  std::vector<NtDirEntry> pass2;
  ASSERT_NT_STATUS(EnumerateDirClass<nt::FileBothDirInfo>(
                       dh.value->get(), /*buffer_size=*/4096,
                       /*return_single_entry=*/true, L"", &pass2),
                   STATUS_NO_MORE_FILES);
  EXPECT_MULTISET_EQ(ExtractNames(pass2), expected_txt);
}

// Spec: [MS-FSA] 2.1.5.6.3 — separate directory handles maintain independent
// enumeration cursors when interleaved.
TEST_F(DirEnumTest, NtQueryDirectoryIndependentCursorsTwoHandles) {
  const std::wstring sub = TestPath(L"two_handles");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  std::vector<std::wstring> expected = {L".", L".."};
  for (int i = 0; i < 6; ++i) {
    const std::wstring name = L"c" + std::to_wstring(i) + L".txt";
    ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name)));
    expected.push_back(name);
  }

  auto h1 = OpenDirForEnum(sub);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);
  auto h2 = OpenDirForEnum(sub);
  ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);

  std::vector<uint8_t> buf(4096, 0);
  std::vector<std::wstring> out1;
  std::vector<std::wstring> out2;
  bool done1 = false;
  bool done2 = false;
  bool first1 = true;
  bool first2 = true;

  auto step_handle = [&](HANDLE h, bool *first_flag, bool *done_flag,
                         std::vector<std::wstring> *out) {
    if (*done_flag) {
      return;
    }
    NtDirEntry entry{};
    const NTSTATUS st = QuerySingleDirEntry<nt::FileBothDirInfo>(
        h, &buf, /*restart_scan=*/*first_flag, L"", &entry);
    *first_flag = false;
    if (st == STATUS_NO_MORE_FILES) {
      *done_flag = true;
      return;
    }
    if (!nt::NtSuccess(st)) {
      *done_flag = true;
      ADD_FAILURE() << "NtQueryDirectoryFile failed: "
                    << core::FormatNtStatus(st);
      return;
    }
    out->push_back(std::move(entry.name));
  };

  for (int iter = 0; (!done1 || !done2) && iter < 1000; ++iter) {
    step_handle(h1.value->get(), &first1, &done1, &out1);
    step_handle(h2.value->get(), &first2, &done2, &out2);
  }
  EXPECT_TRUE(done1);
  EXPECT_TRUE(done2);

  EXPECT_MULTISET_EQ(out1, expected);
  EXPECT_MULTISET_EQ(out2, expected);
}

// Spec: [MS-FSA] 2.1.4.4 — standard directory wildcard matching is
// case-insensitive while preserving on-disk filename casing.
TEST_F(DirEnumTest, NtQueryDirectoryCaseInsensitiveWildcards) {
  const std::wstring sub = TestPath(L"case_wildcards");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"alpha.txt")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"BETA.TxT")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"foo_bar.dat")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"FOO_baz.bin")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"other.log")));

  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(
      sub, L"*.TXT", {L"alpha.txt", L"BETA.TxT"}));
  EXPECT_TRUE(EnumerateDirPathExpectMultiset<nt::FileBothDirInfo>(
      sub, L"FoO*", {L"foo_bar.dat", L"FOO_baz.bin"}));
}

// Spec: WDK NtQueryDirectoryFileEx (Windows 10 1709+) — honors
// SL_RETURN_SINGLE_ENTRY, SL_RESTART_SCAN, and non-advancing
// SL_NO_CURSOR_UPDATE probes.
TEST_F(DirEnumTest, NtQueryDirectoryFileExQuerySingleEntryNoCursorUpdate) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryDirectoryFileEx, nullptr);

  const std::wstring sub = TestPath(L"query_dir_ex");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"alpha.txt")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"beta.txt")));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"gamma.txt")));
  const std::vector<std::wstring> expected = {L".", L"..", L"alpha.txt",
                                              L"beta.txt", L"gamma.txt"};

  // 1. Single-entry enumeration with SL_RETURN_SINGLE_ENTRY + SL_RESTART_SCAN.
  {
    auto dh = OpenDirForEnum(sub);
    ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

    std::vector<NtDirEntry> walked;
    ASSERT_NT_STATUS(EnumerateDirClassEx<nt::FileBothDirInfo>(
                         dh.value->get(), /*buffer_size=*/4096,
                         nt::kSlReturnSingleEntry,
                         /*restart_first=*/true, &walked),
                     STATUS_NO_MORE_FILES);
    EXPECT_MULTISET_EQ(ExtractNames(walked), expected);
  }

  // 2. Interleaved SL_NO_CURSOR_UPDATE (0x10) probe mid-scan leaves the
  // sequential enumeration cursor undisturbed, and SL_RESTART_SCAN resets it.
  {
    auto dh = OpenDirForEnum(sub);
    ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

    std::vector<uint8_t> buf(4096, 0);
    std::vector<NtDirEntry> seq_entries;

    // Advance cursor by 1 entry.
    {
      NtDirEntry first_entry{};
      ASSERT_NT_SUCCESS(QuerySingleDirEntryEx<nt::FileBothDirInfo>(
          dh.value->get(), &buf, nt::kSlRestartScan | nt::kSlReturnSingleEntry,
          L"", &first_entry));
      seq_entries.push_back(std::move(first_entry));
    }

    // Issue a single-entry query with SL_NO_CURSOR_UPDATE (0x10).
    {
      NtDirEntry probe{};
      const NTSTATUS st = QuerySingleDirEntryEx<nt::FileBothDirInfo>(
          dh.value->get(), &buf,
          nt::kSlReturnSingleEntry | nt::kSlNoCursorUpdate, L"gamma.txt",
          &probe);
      ASSERT_NT_STATUS(st, STATUS_SUCCESS);
      EXPECT_EQ(probe.name, L"gamma.txt");
    }

    // Finish sequential scan without SL_RESTART_SCAN: cursor must be intact.
    ASSERT_NT_STATUS(EnumerateDirClassEx<nt::FileBothDirInfo>(
                         dh.value->get(), /*buffer_size=*/4096,
                         nt::kSlReturnSingleEntry,
                         /*restart_first=*/false, &seq_entries),
                     STATUS_NO_MORE_FILES);
    EXPECT_MULTISET_EQ(ExtractNames(seq_entries), expected);

    // Verify SL_RESTART_SCAN resets the exhausted handle cleanly.
    std::vector<NtDirEntry> restarted;
    ASSERT_NT_STATUS(EnumerateDirClassEx<nt::FileBothDirInfo>(
                         dh.value->get(), /*buffer_size=*/4096,
                         /*base_flags=*/0UL,
                         /*restart_first=*/true, &restarted),
                     STATUS_NO_MORE_FILES);
    EXPECT_MULTISET_EQ(ExtractNames(restarted), expected);
  }
}

// Spec: [MS-FSCC] 2.4.23 FileIdExtdDirectoryInformation — NtQueryDirectoryFile
// reports IO_REPARSE_TAG_MOUNT_POINT, FILE_ATTRIBUTE_REPARSE_POINT, and exact
// 128-bit FileId for directory junctions alongside 0 for regular files.
TEST_F(DirEnumTest, FileIdExtdDirectoryInfoReportsReparseTag) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring sub = TestPath(L"extd_reparse_enum");
  const std::wstring target_dir = TestPath(L"extd_reparse_target");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));

  const std::wstring junc_path = core::JoinPath(sub, L"junc_entry");
  const std::wstring plain_path = core::JoinPath(sub, L"plain.dat");
  auto jres = core::CreateDirectoryJunction(junc_path, target_dir);
  ASSERT_TRUE(jres) << core::FormatWin32Error(jres.error);
  ASSERT_TRUE(core::WriteFileContents(plain_path, "plain-file"));

  const ExpectedFileIds junc_ids =
      QueryFileIds(junc_path, /*is_dir=*/true, /*include_id128=*/true,
                   /*open_reparse_point=*/true);
  ASSERT_NE(junc_ids.id64, 0LL);
  ASSERT_FALSE(helpers::AllBytesEqual(junc_ids.id128.identifier,
                                      sizeof(junc_ids.id128.identifier), 0));

  std::vector<NtDirEntry> entries;
  const std::vector<std::wstring> expected = {L".", L"..", L"junc_entry",
                                              L"plain.dat"};
  ASSERT_TRUE(EnumerateDirPathExpectMultiset<nt::FileIdExtdDirInfo>(
      sub, L"", expected, &entries));

  const NtDirEntry *ej = FindEntry(entries, L"junc_entry");
  const NtDirEntry *ep = FindEntry(entries, L"plain.dat");
  ASSERT_NE(ej, nullptr);
  ASSERT_NE(ep, nullptr);

  EXPECT_NE(ej->attributes & FILE_ATTRIBUTE_REPARSE_POINT, 0u);
  EXPECT_NE(ej->attributes & FILE_ATTRIBUTE_DIRECTORY, 0u);
  EXPECT_EQ(ej->reparse_tag, static_cast<ULONG>(IO_REPARSE_TAG_MOUNT_POINT));
  EXPECT_EQ(std::memcmp(ej->file_id_128.identifier, junc_ids.id128.identifier,
                        sizeof(nt::FileId128)),
            0);

  EXPECT_EQ(ep->attributes & FILE_ATTRIBUTE_REPARSE_POINT, 0u);
  EXPECT_EQ(ep->reparse_tag, 0u);

  EXPECT_WIN32_TRUE(::RemoveDirectoryW(junc_path.c_str()));
}

// Spec: [MS-FSCC] 2.4.15 FileFullDirectoryInformation — NtQueryDirectoryFile
// reports non-zero EaSize matching FileEaInformation for files with extended
// attributes and 0 for files without EAs.
TEST_F(DirEnumTest, FileFullDirectoryInfoReportsEaSize) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtSetEaFile, nullptr);
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring sub = TestPath(L"full_dir_ea");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring with_ea_path = core::JoinPath(sub, L"with_ea.dat");
  const std::wstring no_ea_path = core::JoinPath(sub, L"no_ea.dat");
  ASSERT_TRUE(CreateEmptyFile(no_ea_path));

  ULONG expected_ea_size = 0;
  {
    auto fh = core::CreateWin32File(with_ea_path, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

    std::vector<uint8_t> ea_buf = core::BuildEaBuffer({
        {0, "meta_a", {'h', 'e', 'l', 'l', 'o'}},
        {0, "meta_b", {0x01, 0x02, 0x03, 0x04}},
    });
    nt::IoStatusBlock iosb{};
    ASSERT_NT_SUCCESS(ntdll.NtSetEaFile(fh.value->get(), &iosb, ea_buf.data(),
                                        static_cast<ULONG>(ea_buf.size())));

    nt::FileEaInfo ea_info{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &ea_info, sizeof(ea_info),
        nt::FileInfoClass::FileEaInformation));
    ASSERT_GT(ea_info.ea_size, 0u);
    expected_ea_size = ea_info.ea_size;
  }

  std::vector<NtDirEntry> entries;
  const std::vector<std::wstring> expected = {L".", L"..", L"with_ea.dat",
                                              L"no_ea.dat"};
  ASSERT_TRUE(EnumerateDirPathExpectMultiset<nt::FileFullDirInfo>(
      sub, L"", expected, &entries));

  const NtDirEntry *ew = FindEntry(entries, L"with_ea.dat");
  const NtDirEntry *en = FindEntry(entries, L"no_ea.dat");
  ASSERT_NE(ew, nullptr);
  ASSERT_NE(en, nullptr);
  EXPECT_TRUE(ew->has_ea_size);
  EXPECT_EQ(ew->ea_size, expected_ea_size);
  EXPECT_EQ(en->ea_size, 0u);
}

// Spec: [MS-FSA] 2.1.4.4 / Win32 FindFirstFileW — wildcard queries match
// generated 8.3 short names while returning the canonical long filename.
TEST_F(DirEnumTest, FindFirstFileMatches8dot3ShortNameWildcard) {
  REQUIRE_SHORT_NAMES_ENABLED();

  const std::wstring sub = TestPath(L"short_wildcard");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring long_name = L"verylongfilename_alpha.txt";
  const std::wstring long_path = core::JoinPath(sub, long_name);
  ASSERT_TRUE(CreateEmptyFile(long_path));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"other.log")));

  WIN32_FIND_DATAW direct_fd{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueFindHandle direct_find(
      ::FindFirstFileW(long_path.c_str(), &direct_fd));
  const DWORD direct_find_err = ::GetLastError();
  ASSERT_TRUE(direct_find) << core::FormatWin32Error(direct_find_err);
  ASSERT_NE(direct_fd.cAlternateFileName[0], L'\0')
      << "Expected generated 8.3 short name for '"
      << core::WideToUtf8(long_name) << "'";
  const std::wstring short_name(direct_fd.cAlternateFileName);
  EXPECT_NE(short_name.find(L'~'), std::wstring::npos)
      << "Unexpected 8.3 short name: " << core::WideToUtf8(short_name);

  // Wildcard matching the generated 8.3 short name (*~1.TXT) must match and
  // return the canonical long filename in cFileName.
  std::vector<std::wstring> matched_long_names;
  ASSERT_TRUE(ForEachWin32FindEntry(
      core::JoinPath(sub, L"*~1.TXT"), /*use_ex_basic=*/false,
      [&](const WIN32_FIND_DATAW &data) {
        matched_long_names.emplace_back(data.cFileName);
        EXPECT_TRUE(
            core::EqualsIgnoreCase(data.cAlternateFileName, short_name));
      }));
  EXPECT_MULTISET_EQ(matched_long_names,
                     (std::vector<std::wstring>{long_name}));
}

// Spec: [MS-FSA] 2.1.5.6.3 / NtQueryDirectoryFile -- when the output buffer can
// hold the fixed header and a prefix of the first matching entry's filename but
// not the full entry, NTFS returns STATUS_BUFFER_OVERFLOW with the partial
// filename, the full untruncated FileNameLength, and IoStatus.Information equal
// to the bytes written, without advancing the enumeration cursor so the next
// call with an adequate buffer still yields the full entry.
TEST_F(DirEnumTest,
       NtQueryDirectoryBufferOverflowReturnsPartialNameAndPreservesCursor) {
  const std::wstring sub = TestPath(L"overflow_cursor");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  // Two same-length names sharing the prefix "ovf_" keep the test independent
  // of the filesystem's enumeration order.
  const std::wstring name_a = L"ovf_alpha_long.txt";
  const std::wstring name_b = L"ovf_bravo_long.txt";
  ASSERT_EQ(name_a.size(), name_b.size());
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name_a)));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, name_b)));
  ASSERT_TRUE(CreateEmptyFile(core::JoinPath(sub, L"unrelated.dat")));

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryDirectoryFile, nullptr);

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  constexpr size_t kHeaderBytes = offsetof(nt::FileBothDirInfo, file_name);
  constexpr size_t kPartialChars = 4; // "ovf_"
  constexpr size_t kShortBufferSize =
      kHeaderBytes + kPartialChars * sizeof(wchar_t);
  // Buffers smaller than the fixed structure fail STATUS_INFO_LENGTH_MISMATCH
  // instead (see NtQueryDirectorySmallBufferPagination).
  static_assert(kShortBufferSize >= sizeof(nt::FileBothDirInfo));
  const size_t full_name_bytes = name_a.size() * sizeof(wchar_t);

  // 1. Undersized single-entry query binds "ovf_*" and overflows.
  std::wstring pat = L"ovf_*";
  nt::UnicodeString pat_us = nt::MakeUnicodeString(pat.data(), pat.size());
  std::vector<uint8_t> short_buf(kShortBufferSize, 0);
  nt::IoStatusBlock iosb{};
  const NTSTATUS overflow_st = ntdll.NtQueryDirectoryFile(
      dh.value->get(), nullptr, nullptr, nullptr, &iosb, short_buf.data(),
      static_cast<ULONG>(short_buf.size()),
      nt::FileInfoClass::FileBothDirectoryInformation,
      /*return_single_entry=*/TRUE, &pat_us, /*restart_scan=*/TRUE);
  ASSERT_NT_STATUS(overflow_st, STATUS_BUFFER_OVERFLOW);
  EXPECT_EQ(static_cast<size_t>(iosb.information), kShortBufferSize);

  const auto *partial_entry =
      reinterpret_cast<const nt::FileBothDirInfo *>(short_buf.data());
  EXPECT_EQ(partial_entry->next_entry_offset, 0u);
  EXPECT_EQ(static_cast<size_t>(partial_entry->file_name_length),
            full_name_bytes)
      << "FileNameLength must report the full (untruncated) name length";
  const std::wstring partial_name(partial_entry->file_name, kPartialChars);
  EXPECT_EQ(partial_name, L"ovf_");

  // 2. Continuing without RestartScan with an adequate buffer returns both
  // matching entries: the overflowed entry was NOT lost from the cursor.
  std::vector<uint8_t> buf(4096, 0);
  std::vector<std::wstring> continued;
  bool continued_done = false;
  for (int iter = 0; iter < 16; ++iter) {
    NtDirEntry entry{};
    const NTSTATUS st = QuerySingleDirEntry<nt::FileBothDirInfo>(
        dh.value->get(), &buf, /*restart_scan=*/false, L"", &entry);
    if (st == STATUS_NO_MORE_FILES) {
      continued_done = true;
      break;
    }
    ASSERT_NT_SUCCESS(st);
    continued.push_back(std::move(entry.name));
  }
  EXPECT_TRUE(continued_done);
  EXPECT_MULTISET_EQ(continued, (std::vector<std::wstring>{name_a, name_b}));

  // 3. RestartScan=TRUE with an adequate buffer returns both entries intact.
  std::vector<NtDirEntry> restarted;
  ASSERT_NT_STATUS(EnumerateDirClass<nt::FileBothDirInfo>(
                       dh.value->get(), /*buffer_size=*/4096,
                       /*return_single_entry=*/false, L"", &restarted),
                   STATUS_NO_MORE_FILES);
  EXPECT_MULTISET_EQ(ExtractNames(restarted),
                     (std::vector<std::wstring>{name_a, name_b}));
}

// Spec: [MS-FSA] 2.1.5.6.3 / NtQueryDirectoryFile — on a directory handle
// opened for overlapped I/O, queries complete (inline or via STATUS_PENDING
// and the caller's event) with the same paginated results as synchronous
// handles and terminate with STATUS_NO_MORE_FILES.
TEST_F(DirEnumTest, NtQueryDirectoryOverlappedHandleCompletesViaEvent) {
  const std::wstring sub = TestPath(L"overlapped_enum");
  std::vector<std::wstring> expected;
  ASSERT_TRUE(CreateNumberedFilesDir(sub, L"ov_", 12, &expected));

  auto dh = core::CreateWin32File(
      sub, FILE_LIST_DIRECTORY, core::kShareAll, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle event(::CreateEventW(nullptr, /*bManualReset=*/TRUE,
                                          /*bInitialState=*/FALSE, nullptr));
  const DWORD event_err = ::GetLastError();
  ASSERT_TRUE(event.valid()) << core::FormatWin32Error(event_err);

  // 512 bytes hold only a few FileBothDirectoryInformation records, forcing
  // several overlapped continuation calls.
  std::vector<uint8_t> buf(512, 0);
  std::vector<NtDirEntry> entries;
  bool done = false;
  size_t successful_calls = 0;
  for (int iter = 0; iter < 1000; ++iter) {
    nt::IoStatusBlock iosb{};
    bool pended = false;
    const NTSTATUS st = QueryDirectoryOverlapped(
        dh.value->get(), event.get(), &buf,
        nt::FileInfoClass::FileBothDirectoryInformation,
        /*restart_scan=*/(iter == 0), &iosb, &pended);
    if (st == STATUS_NO_MORE_FILES) {
      done = true;
      break;
    }
    ASSERT_NT_STATUS(st, STATUS_SUCCESS);
    if (pended) {
      EXPECT_NT_STATUS(iosb.status, STATUS_SUCCESS);
    }
    ASSERT_GT(static_cast<size_t>(iosb.information), size_t{0});
    ++successful_calls;
    ParseDirBuffer<nt::FileBothDirInfo>(
        buf, iosb.information, /*expect_single_entry=*/false,
        [&entries](NtDirEntry entry) { entries.push_back(std::move(entry)); });
    ASSERT_FALSE(HasFatalFailure());
  }
  EXPECT_TRUE(done);
  EXPECT_GE(successful_calls, 2u);
  EXPECT_MULTISET_EQ(ExtractNames(entries), expected);
}

// Spec: Win32 GetFileInformationByHandleEx(FileIdBothDirectoryRestartInfo /
// FileIdBothDirectoryInfo) — paginates a directory to ERROR_NO_MORE_FILES with
// correct names and sizes, and the restart class rewinds the handle.
TEST_F(DirEnumTest, GetFileInformationByHandleExIdBothDirectoryInfo) {
  ExpectByHandleExEnumeration<nt::FileIdBothDirInfo>(
      TestPath(L"by_handle_id_both"), FileIdBothDirectoryRestartInfo,
      FileIdBothDirectoryInfo);
}

// Spec: Win32 GetFileInformationByHandleEx(FileFullDirectoryRestartInfo /
// FileFullDirectoryInfo, Windows 8+) — paginates a directory to
// ERROR_NO_MORE_FILES with correct names and sizes, and the restart class
// rewinds the handle.
TEST_F(DirEnumTest, GetFileInformationByHandleExFullDirectoryInfo) {
  ExpectByHandleExEnumeration<nt::FileFullDirInfo>(
      TestPath(L"by_handle_full"), kByHandleFullDirectoryRestartInfo,
      kByHandleFullDirectoryInfo);
}

// Spec: [MS-FSA] 2.1.5.6.3 — the enumeration cursor belongs to the open, not
// to the information class: switching FileInformationClass between calls
// without RestartScan continues the same scan with no duplicate or skipped
// entries.
TEST_F(DirEnumTest, NtQueryDirectoryInfoClassSwitchMidScanKeepsCursor) {
  const std::wstring sub = TestPath(L"class_switch");
  std::vector<std::wstring> expected;
  ASSERT_TRUE(CreateNumberedFilesDir(sub, L"sw_", 6, &expected));

  auto dh = OpenDirForEnum(sub);
  ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

  std::vector<uint8_t> buf(4096, 0);
  std::vector<std::wstring> seen;

  // Entries 1-2: FileBothDirectoryInformation.
  for (int step = 0; step < 2; ++step) {
    NtDirEntry entry{};
    ASSERT_NT_STATUS(
        QuerySingleDirEntry<nt::FileBothDirInfo>(
            dh.value->get(), &buf, /*restart_scan=*/(step == 0), L"", &entry),
        STATUS_SUCCESS);
    seen.push_back(std::move(entry.name));
  }
  // Entries 3-4: FileNamesInformation.
  for (int step = 0; step < 2; ++step) {
    NtDirEntry entry{};
    ASSERT_NT_STATUS(
        QuerySingleDirEntry<nt::FileNamesInfo>(
            dh.value->get(), &buf, /*restart_scan=*/false, L"", &entry),
        STATUS_SUCCESS);
    seen.push_back(std::move(entry.name));
  }
  // Remaining entries: FileIdFullDirectoryInformation.
  bool done = false;
  for (int iter = 0; iter < 64; ++iter) {
    NtDirEntry entry{};
    const NTSTATUS st = QuerySingleDirEntry<nt::FileIdFullDirInfo>(
        dh.value->get(), &buf, /*restart_scan=*/false, L"", &entry);
    if (st == STATUS_NO_MORE_FILES) {
      done = true;
      break;
    }
    ASSERT_NT_STATUS(st, STATUS_SUCCESS);
    seen.push_back(std::move(entry.name));
  }
  EXPECT_TRUE(done);
  EXPECT_MULTISET_EQ(seen, expected);
}

void ForceLink_DirEnum() {}

} // namespace fstest
