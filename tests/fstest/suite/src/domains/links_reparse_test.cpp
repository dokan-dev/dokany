// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <cstdint>
#include <cstring>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {
namespace {

core::Win32Result<std::vector<std::wstring>>
EnumerateHardLinks(std::wstring_view file_path) {
  core::Win32Result<std::vector<std::wstring>> result{};
  std::wstring path_str(file_path);
  std::vector<wchar_t> buf(1024, L'\0');
  DWORD len = static_cast<DWORD>(buf.size());

  core::UniqueFindHandle find(
      ::FindFirstFileNameW(path_str.c_str(), 0, &len, buf.data()));
  if (!find) {
    DWORD err = ::GetLastError();
    if (err == ERROR_MORE_DATA && len > buf.size()) {
      buf.resize(static_cast<size_t>(len) + 1, L'\0');
      len = static_cast<DWORD>(buf.size());
      find.reset(::FindFirstFileNameW(path_str.c_str(), 0, &len, buf.data()));
      if (!find) {
        result.error = ::GetLastError();
        return result;
      }
    } else {
      result.error = err;
      return result;
    }
  }

  std::vector<std::wstring> links;
  links.emplace_back(buf.data());

  for (;;) {
    len = static_cast<DWORD>(buf.size());
    if (::FindNextFileNameW(find.get(), &len, buf.data()) != FALSE) {
      links.emplace_back(buf.data());
      continue;
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_MORE_DATA && len > buf.size()) {
      buf.resize(static_cast<size_t>(len) + 1, L'\0');
      continue;
    }
    if (err != ERROR_HANDLE_EOF) {
      result.error = err;
      return result;
    }
    break;
  }

  result.error = ERROR_SUCCESS;
  result.value = std::move(links);
  return result;
}

std::wstring ToLowerAscii(std::wstring_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    out.push_back(c);
  }
  return out;
}

std::vector<std::wstring>
ExtractTempRelativePaths(const std::vector<std::wstring> &raw_links,
                         std::wstring_view temp_basename) {
  const std::wstring needle =
      ToLowerAscii(L"\\" + std::wstring(temp_basename) + L"\\");
  std::vector<std::wstring> out;
  out.reserve(raw_links.size());
  for (const auto &raw : raw_links) {
    const std::wstring lower = ToLowerAscii(raw);
    const size_t pos = lower.find(needle);
    if (pos != std::wstring::npos) {
      out.push_back(raw.substr(pos + needle.size()));
    } else {
      out.push_back(raw);
    }
  }
  return out;
}

::testing::AssertionResult CheckLinkFileIndexAndCount(std::wstring_view path,
                                                      uint64_t expected_index,
                                                      DWORD expected_links) {
  auto info = core::QueryInfoByPath(path);
  if (!info) {
    return ::testing::AssertionFailure()
           << "QueryInfoByPath('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(info.error);
  }
  const uint64_t actual_index = core::FileIndexOf(*info.value);
  if (actual_index != expected_index) {
    return ::testing::AssertionFailure()
           << "FileIndex mismatch for '" << core::WideToUtf8(path)
           << "': actual=" << actual_index << ", expected=" << expected_index;
  }
  if (info.value->nNumberOfLinks != expected_links) {
    return ::testing::AssertionFailure()
           << "NumberOfLinks mismatch for '" << core::WideToUtf8(path)
           << "': actual=" << info.value->nNumberOfLinks
           << ", expected=" << expected_links;
  }
  return ::testing::AssertionSuccess();
}

core::Win32Result<core::UniqueHandle>
OpenReparsePointHandle(std::wstring_view path, DWORD desired_access,
                       bool is_directory) {
  const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                      (is_directory ? FILE_FLAG_BACKUP_SEMANTICS : DWORD{0});
  return core::CreateWin32File(path, desired_access, core::kShareAll,
                               OPEN_EXISTING, flags);
}

::testing::AssertionResult CheckReparsePointTag(std::wstring_view path,
                                                bool is_directory,
                                                ULONG expected_tag,
                                                uint64_t *out_index = nullptr) {
  auto h = OpenReparsePointHandle(path, FILE_READ_ATTRIBUTES, is_directory);
  if (!h) {
    return ::testing::AssertionFailure()
           << "OpenReparsePointHandle('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(h.error);
  }
  auto info = core::GetByHandleInfo(h.value->get());
  if (!info) {
    return ::testing::AssertionFailure()
           << "GetByHandleInfo('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(info.error);
  }
  if ((info.value->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
    return ::testing::AssertionFailure()
           << "Expected FILE_ATTRIBUTE_REPARSE_POINT on '"
           << core::WideToUtf8(path) << "'";
  }
  if (out_index != nullptr) {
    *out_index = core::FileIndexOf(*info.value);
  }
  auto rp_data = core::GetReparsePointData(h.value->get());
  if (!rp_data) {
    return ::testing::AssertionFailure()
           << "GetReparsePointData('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(rp_data.error);
  }
  if (rp_data.value->size() < nt::kReparseDataBufferHeaderSize) {
    return ::testing::AssertionFailure()
           << "Reparse data too short for '" << core::WideToUtf8(path)
           << "': " << rp_data.value->size() << " bytes";
  }
  const auto *rdb =
      reinterpret_cast<const nt::ReparseDataBuffer *>(rp_data.value->data());
  if (rdb->reparse_tag != expected_tag) {
    return ::testing::AssertionFailure()
           << "Reparse tag mismatch for '" << core::WideToUtf8(path)
           << "': actual=0x" << std::hex << rdb->reparse_tag << ", expected=0x"
           << expected_tag;
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckFinalPathEndsWith(HANDLE h,
                                                  std::wstring_view suffix) {
  auto final_path =
      core::QueryFinalPath(h, VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
  if (!final_path) {
    return ::testing::AssertionFailure()
           << "QueryFinalPath failed: "
           << core::FormatWin32Error(final_path.error);
  }
  if (!core::EndsWithIgnoreCase(*final_path.value, suffix)) {
    return ::testing::AssertionFailure()
           << "Final path '" << core::WideToUtf8(*final_path.value)
           << "' did not end with '" << core::WideToUtf8(suffix) << "'";
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult
CheckPathFinalPathEndsWith(std::wstring_view path, std::wstring_view suffix) {
  auto h = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  if (!h) {
    return ::testing::AssertionFailure()
           << "CreateWin32File('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(h.error);
  }
  return CheckFinalPathEndsWith(h.value->get(), suffix);
}

core::Win32Result<void> SetCustomReparsePoint(HANDLE h, ULONG tag,
                                              const GUID &guid,
                                              const uint8_t *payload,
                                              size_t payload_size) {
  core::Win32Result<void> result{};
  std::vector<uint8_t> in_buf(
      nt::kReparseGuidDataBufferHeaderSize + payload_size, 0);
  auto *in_rdb = reinterpret_cast<nt::ReparseGuidDataBuffer *>(in_buf.data());
  in_rdb->reparse_tag = tag;
  in_rdb->reparse_data_length = static_cast<USHORT>(payload_size);
  in_rdb->reserved = 0;
  in_rdb->reparse_guid = guid;
  if (payload_size > 0) {
    std::memcpy(in_rdb->data_buffer, payload, payload_size);
  }
  DWORD bytes_returned = 0;
  if (::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, in_buf.data(),
                        static_cast<DWORD>(in_buf.size()), nullptr, 0,
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

::testing::AssertionResult CheckCustomReparsePoint(HANDLE h, ULONG expected_tag,
                                                   const GUID &expected_guid,
                                                   const uint8_t *expected_data,
                                                   size_t expected_size) {
  auto out_data = core::GetReparsePointData(h);
  if (!out_data) {
    return ::testing::AssertionFailure()
           << "GetReparsePointData failed: "
           << core::FormatWin32Error(out_data.error);
  }
  const size_t expected_total =
      nt::kReparseGuidDataBufferHeaderSize + expected_size;
  if (out_data.value->size() != expected_total) {
    return ::testing::AssertionFailure()
           << "Reparse buffer size mismatch: actual=" << out_data.value->size()
           << ", expected=" << expected_total;
  }
  const auto *out_rdb = reinterpret_cast<const nt::ReparseGuidDataBuffer *>(
      out_data.value->data());
  if (out_rdb->reparse_tag != expected_tag) {
    return ::testing::AssertionFailure()
           << "Custom reparse tag mismatch: actual=0x" << std::hex
           << out_rdb->reparse_tag << ", expected=0x" << expected_tag;
  }
  if (out_rdb->reparse_data_length != static_cast<USHORT>(expected_size)) {
    return ::testing::AssertionFailure()
           << "Custom reparse data_length mismatch: actual="
           << out_rdb->reparse_data_length << ", expected=" << expected_size;
  }
  if (std::memcmp(&out_rdb->reparse_guid, &expected_guid, sizeof(GUID)) != 0) {
    return ::testing::AssertionFailure() << "Custom reparse GUID mismatch";
  }
  if (expected_size > 0 &&
      std::memcmp(out_rdb->data_buffer, expected_data, expected_size) != 0) {
    return ::testing::AssertionFailure() << "Custom reparse payload mismatch";
  }
  return ::testing::AssertionSuccess();
}

struct FindReparseEntryInfo {
  DWORD attributes = 0;
  DWORD reserved0 = 0;
};

core::Win32Result<std::map<std::wstring, FindReparseEntryInfo>>
CollectFindEntryInfos(std::wstring_view dir_path) {
  core::Win32Result<std::map<std::wstring, FindReparseEntryInfo>> result{};
  const std::wstring pattern = core::JoinPath(dir_path, L"*");
  WIN32_FIND_DATAW fd{};
  core::UniqueFindHandle find(::FindFirstFileW(pattern.c_str(), &fd));
  if (!find.valid()) {
    result.error = ::GetLastError();
    return result;
  }
  std::map<std::wstring, FindReparseEntryInfo> entries;
  do {
    entries.emplace(std::wstring(fd.cFileName),
                    FindReparseEntryInfo{fd.dwFileAttributes, fd.dwReserved0});
  } while (::FindNextFileW(find.get(), &fd) != FALSE);

  const DWORD err = ::GetLastError();
  if (err != ERROR_NO_MORE_FILES) {
    result.error = err;
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(entries);
  return result;
}

::testing::AssertionResult CheckHardLinkEntries(
    HANDLE h,
    const std::vector<std::pair<uint64_t, std::wstring>> &expected_entries) {
  auto entries = core::QueryHardLinkEntries(h);
  if (entries.status != STATUS_SUCCESS || !entries.value.has_value()) {
    return ::testing::AssertionFailure()
           << "QueryHardLinkEntries failed: "
           << core::FormatNtStatus(entries.status);
  }
  if (entries.value->size() != expected_entries.size()) {
    return ::testing::AssertionFailure()
           << "HardLinkEntries count mismatch: actual=" << entries.value->size()
           << ", expected=" << expected_entries.size();
  }
  std::map<std::wstring, uint64_t> expected_parents;
  std::vector<std::wstring> expected_names;
  expected_names.reserve(expected_entries.size());
  for (const auto &[parent_id, name] : expected_entries) {
    expected_parents.emplace(name, parent_id);
    expected_names.push_back(name);
  }
  std::vector<std::wstring> actual_names;
  actual_names.reserve(entries.value->size());
  for (const auto &[parent_id, name] : *entries.value) {
    actual_names.push_back(name);
    const auto it = expected_parents.find(name);
    if (it != expected_parents.end() &&
        static_cast<uint64_t>(parent_id) != it->second) {
      return ::testing::AssertionFailure()
             << "Parent FileIndex mismatch for link '" << core::WideToUtf8(name)
             << "': actual=" << static_cast<uint64_t>(parent_id)
             << ", expected=" << it->second;
    }
  }
  return ::fstest::detail::CheckMultisetEq("actual_names", "expected_names",
                                           actual_names, expected_names);
}

// NTFS allows at most 1024 names (the original plus 1023 additional hard
// links) per file.
constexpr DWORD kMaxLinksPerFile = 1024;

// Valid non-Microsoft application reparse tags: bit 31 (Microsoft) = 0,
// reserved bits 30 and 27..16 = 0, non-zero low 16 bits.
constexpr ULONG kAppTag1 = 0x00000A5AUL;
constexpr ULONG kAppTag2 = 0x00000A5BUL;

constexpr GUID kAppGuid1 = {0x4e746673UL,
                            0x4673U,
                            0x5465U,
                            {0x73, 0x74, 0x52, 0x65, 0x70, 0x61, 0x72, 0x01}};
constexpr GUID kAppGuid2 = {0x4e746673UL,
                            0x4673U,
                            0x5465U,
                            {0x73, 0x74, 0x52, 0x65, 0x70, 0x61, 0x72, 0x02}};

constexpr uint8_t kAppPayload1[12] = {0x01, 0x12, 0x23, 0x34, 0x45, 0x56,
                                      0x67, 0x78, 0x89, 0x9A, 0xAB, 0xBC};
constexpr uint8_t kAppPayload2[8] = {0xDE, 0xAD, 0xBE, 0xEF,
                                     0xCA, 0xFE, 0xBA, 0xBE};

// Creates a new empty file at `path`, opens it with
// FILE_FLAG_OPEN_REPARSE_POINT for GENERIC_READ | GENERIC_WRITE, and stores
// the handle in `*out`.
::testing::AssertionResult CreateFileForReparse(std::wstring_view path,
                                                core::UniqueHandle *out) {
  auto created = helpers::CreateEmptyFileResult(path);
  if (!created) {
    return ::testing::AssertionFailure()
           << "CreateEmptyFileResult('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(created.error);
  }
  auto h = OpenReparsePointHandle(path, GENERIC_READ | GENERIC_WRITE,
                                  /*is_directory=*/false);
  if (!h) {
    return ::testing::AssertionFailure()
           << "OpenReparsePointHandle('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(h.error);
  }
  *out = std::move(*h.value);
  return ::testing::AssertionSuccess();
}

// Creates a new file at `path` carrying the custom (kAppTag1, kAppGuid1,
// kAppPayload1) reparse point and stores its open-reparse-point handle in
// `*out`.
::testing::AssertionResult
CreateFileWithAppReparsePoint(std::wstring_view path, core::UniqueHandle *out) {
  auto created = CreateFileForReparse(path, out);
  if (!created) {
    return created;
  }
  auto set = SetCustomReparsePoint(out->get(), kAppTag1, kAppGuid1,
                                   kAppPayload1, sizeof(kAppPayload1));
  if (!set) {
    return ::testing::AssertionFailure()
           << "FSCTL_SET_REPARSE_POINT on '" << core::WideToUtf8(path)
           << "' failed: " << core::FormatWin32Error(set.error);
  }
  return ::testing::AssertionSuccess();
}

// Verifies that `h` carries no reparse point: FILE_ATTRIBUTE_REPARSE_POINT is
// clear and FSCTL_GET_REPARSE_POINT fails with ERROR_NOT_A_REPARSE_POINT.
::testing::AssertionResult CheckNoReparsePoint(HANDLE h) {
  auto info = core::GetByHandleInfo(h);
  if (!info) {
    return ::testing::AssertionFailure()
           << "GetByHandleInfo failed: " << core::FormatWin32Error(info.error);
  }
  if ((info.value->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return ::testing::AssertionFailure()
           << "FILE_ATTRIBUTE_REPARSE_POINT is unexpectedly set";
  }
  auto data = core::GetReparsePointData(h);
  if (data) {
    return ::testing::AssertionFailure()
           << "FSCTL_GET_REPARSE_POINT unexpectedly succeeded ("
           << data.value->size() << " bytes)";
  }
  if (data.error != static_cast<DWORD>(ERROR_NOT_A_REPARSE_POINT)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_NOT_A_REPARSE_POINT from "
              "FSCTL_GET_REPARSE_POINT, got "
           << core::FormatWin32Error(data.error);
  }
  return ::testing::AssertionSuccess();
}

} // namespace

// ---------------------------------------------------------------------------
// Domain 10: Links & Reparse Points
// ---------------------------------------------------------------------------

// Spec: [MS-FSA] 2.1.5.15.7 & Win32 Hard Links — hard links share a single
// FileIndex and data stream while incrementing NumberOfLinks across links.
TEST_F(LinksReparseTest, HardLinkCreateSharedDataAndFileIndex) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring link_a = TestPath(L"hl_a.dat");
  const std::wstring link_b = TestPath(L"hl_b.dat");
  const std::wstring link_c = TestPath(L"hl_c.dat");

  ASSERT_TRUE(core::WriteFileContents(link_a, "initial-hl-data"));
  auto info_a = core::QueryInfoByPath(link_a);
  ASSERT_TRUE(info_a) << core::FormatWin32Error(info_a.error);
  const uint64_t expected_index = core::FileIndexOf(*info_a.value);
  EXPECT_EQ(info_a.value->nNumberOfLinks, DWORD{1});

  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_a, expected_index, DWORD{2}));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_b, expected_index, DWORD{2}));

  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_c.c_str(), link_a.c_str(), nullptr));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_a, expected_index, DWORD{3}));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_b, expected_index, DWORD{3}));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_c, expected_index, DWORD{3}));

  // Writing via link B is immediately visible through links A and C.
  {
    auto h_b = core::CreateWin32File(link_b, GENERIC_WRITE, core::kShareAll,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h_b) << core::FormatWin32Error(h_b.error);
    const std::string updated = "written-through-link-b";
    ASSERT_TRUE(
        core::WriteAllBytes(h_b.value->get(), updated.data(), updated.size()));
    ASSERT_WIN32_TRUE(::SetEndOfFile(h_b.value->get()));
  }

  EXPECT_TRUE(helpers::CheckFileContents(link_a, "written-through-link-b"));
  EXPECT_TRUE(helpers::CheckFileContents(link_c, "written-through-link-b"));
}

// Spec: [MS-FSA] 2.1.5.15.7 & 2.1.5.15.3 — hard links share attributes and
// timestamps; unlinking one decrements NumberOfLinks while others survive.
TEST_F(LinksReparseTest, HardLinkSharedMetadataAndUnlink) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring link_a = TestPath(L"hl_meta_a.dat");
  const std::wstring link_b = TestPath(L"hl_meta_b.dat");
  ASSERT_TRUE(core::WriteFileContents(link_a, "shared-meta-payload"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));

  auto info_a = core::QueryInfoByPath(link_a);
  ASSERT_TRUE(info_a) << core::FormatWin32Error(info_a.error);
  const uint64_t expected_index = core::FileIndexOf(*info_a.value);

  // Attribute changes on A immediately reflect on B.
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(link_a.c_str(), FILE_ATTRIBUTE_HIDDEN));
  const DWORD attrs_b = ::GetFileAttributesW(link_b.c_str());
  ASSERT_NE(attrs_b, INVALID_FILE_ATTRIBUTES);
  EXPECT_NE(attrs_b & FILE_ATTRIBUTE_HIDDEN, DWORD{0});

  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(link_b.c_str(), FILE_ATTRIBUTE_NORMAL));
  const DWORD attrs_a = ::GetFileAttributesW(link_a.c_str());
  ASSERT_NE(attrs_a, INVALID_FILE_ATTRIBUTES);
  EXPECT_EQ(attrs_a & FILE_ATTRIBUTE_HIDDEN, DWORD{0});

  // Timestamp changes on A immediately reflect on B.
  FILETIME custom_write{};
  custom_write.dwHighDateTime = 0x01D80000UL;
  custom_write.dwLowDateTime = 0x12345678UL;
  {
    auto h_a =
        core::CreateWin32File(link_a, FILE_WRITE_ATTRIBUTES, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h_a) << core::FormatWin32Error(h_a.error);
    ASSERT_WIN32_TRUE(
        ::SetFileTime(h_a.value->get(), nullptr, nullptr, &custom_write));
  }
  auto info_b = core::QueryInfoByPath(link_b);
  ASSERT_TRUE(info_b) << core::FormatWin32Error(info_b.error);
  EXPECT_EQ(core::FileTimeToU64(info_b.value->ftLastWriteTime),
            core::FileTimeToU64(custom_write));

  // Unlinking A decrements NumberOfLinks to 1 while B retains data & FileIndex.
  ASSERT_WIN32_TRUE(::DeleteFileW(link_a.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(link_a));

  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_b, expected_index, DWORD{1}));
  EXPECT_TRUE(helpers::CheckFileContents(link_b, "shared-meta-payload"));
}

// Spec: [MS-FSA] 2.1.5.15.7 FileLinkInformation — creating a hard link to
// a directory fails with ERROR_ACCESS_DENIED without creating an entry.
TEST_F(LinksReparseTest, HardLinkOnDirectoryFailsAccessDenied) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring dir = TestPath(L"hl_target_dir");
  const std::wstring link = TestPath(L"hl_dir_link");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  EXPECT_WIN32_ERROR(::CreateHardLinkW(link.c_str(), dir.c_str(), nullptr),
                     ERROR_ACCESS_DENIED);
  EXPECT_TRUE(helpers::CheckFileNotFound(link));
}

// Spec: Win32 FindFirstFileNameW / FindNextFileNameW — hard-link
// enumeration returns all volume-relative link paths matching NumberOfLinks.
TEST_F(LinksReparseTest, HardLinkFileNameEnumeration) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring sub = TestPath(L"sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring link_a = TestPath(L"enum_a.dat");
  const std::wstring link_b = TestPath(L"enum_b.dat");
  const std::wstring link_c = sub + L"\\enum_c.dat";

  ASSERT_TRUE(core::WriteFileContents(link_a, "enum-links"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_c.c_str(), link_a.c_str(), nullptr));

  auto h = core::CreateWin32File(link_a, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 core::kShareAll, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  auto std_info = core::QueryStandardInfo(h.value->get());
  ASSERT_NT_SUCCESS(std_info.status);
  ASSERT_TRUE(std_info.value.has_value());
  EXPECT_EQ(std_info.value->number_of_links, ULONG{3});

  auto links = EnumerateHardLinks(link_a);
  ASSERT_TRUE(links) << core::FormatWin32Error(links.error);
  EXPECT_EQ(links.value->size(),
            static_cast<size_t>(std_info.value->number_of_links));

  const std::vector<std::wstring> rel_links =
      ExtractTempRelativePaths(*links.value, TempDir().Basename());
  EXPECT_MULTISET_EQ(rel_links,
                     (std::vector<std::wstring>{L"enum_a.dat", L"enum_b.dat",
                                                L"sub\\enum_c.dat"}));
}

// Spec: [MS-FSCC] 2.1.2.4 & Win32 Symbolic Links — normal opens follow file
// symlinks while FILE_FLAG_OPEN_REPARSE_POINT opens the reparse point.
TEST_F(LinksReparseTest, FileSymlinkFollowAndOpenReparsePoint) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target = TestPath(L"sym_file_target.dat");
  const std::wstring link = TestPath(L"sym_file_link.dat");
  ASSERT_TRUE(core::WriteFileContents(target, "sym-target-bytes"));

  auto target_info = core::QueryInfoByPath(target);
  ASSERT_TRUE(target_info) << core::FormatWin32Error(target_info.error);
  const uint64_t target_index = core::FileIndexOf(*target_info.value);

  auto created =
      core::CreateSymlink(link, target, /*is_directory=*/false, Os().build);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  // Normal open follows symlink to target.
  {
    auto followed = core::CreateWin32File(link, GENERIC_READ, core::kShareAll,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(followed) << core::FormatWin32Error(followed.error);
    auto followed_info = core::GetByHandleInfo(followed.value->get());
    ASSERT_TRUE(followed_info) << core::FormatWin32Error(followed_info.error);
    EXPECT_EQ(core::FileIndexOf(*followed_info.value), target_index);
    auto data = core::ReadAllBytes(followed.value->get());
    ASSERT_TRUE(data) << core::FormatWin32Error(data.error);
    EXPECT_EQ(*data.value, "sym-target-bytes");
  }

  // Opening with FILE_FLAG_OPEN_REPARSE_POINT opens the link itself.
  uint64_t rp_index = 0;
  EXPECT_TRUE(CheckReparsePointTag(link, /*is_directory=*/false,
                                   IO_REPARSE_TAG_SYMLINK, &rp_index));
  EXPECT_NE(rp_index, target_index);

  // Deleting symlink leaves target intact.
  ASSERT_WIN32_TRUE(::DeleteFileW(link.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(link));
  EXPECT_TRUE(helpers::CheckFileContents(target, "sym-target-bytes"));
}

// Spec: [MS-FSCC] 2.1.2.4 & Win32 Symbolic Links — directory symlinks
// traverse to target children and RemoveDirectoryW removes only the link.
TEST_F(LinksReparseTest, DirectorySymlinkFollowAndRemove) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target_dir = TestPath(L"sym_target_dir");
  const std::wstring child = target_dir + L"\\child.dat";
  const std::wstring link_dir = TestPath(L"sym_link_dir");

  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(child, "inside-dir-symlink"));

  auto created = core::CreateSymlink(link_dir, target_dir,
                                     /*is_directory=*/true, Os().build);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  const DWORD link_attrs = ::GetFileAttributesW(link_dir.c_str());
  ASSERT_NE(link_attrs, INVALID_FILE_ATTRIBUTES);
  EXPECT_NE(link_attrs & FILE_ATTRIBUTE_DIRECTORY, DWORD{0});
  EXPECT_NE(link_attrs & FILE_ATTRIBUTE_REPARSE_POINT, DWORD{0});

  EXPECT_TRUE(helpers::CheckFileContents(link_dir + L"\\child.dat",
                                         "inside-dir-symlink"));

  // RemoveDirectoryW removes the symlink only, leaving target dir and children.
  ASSERT_WIN32_TRUE(::RemoveDirectoryW(link_dir.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(link_dir));
  EXPECT_TRUE(helpers::CheckFileContents(child, "inside-dir-symlink"));
}

// Spec: [MS-FSCC] 2.1.2.4 Symbolic Link — relative symlinks record
// SYMLINK_FLAG_RELATIVE and resolve paths relative to the link directory.
TEST_F(LinksReparseTest, RelativeSymlinkResolution) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target = TestPath(L"rel_target.dat");
  const std::wstring sub = TestPath(L"rel_sub");
  const std::wstring link = sub + L"\\rel_link.dat";

  ASSERT_TRUE(core::WriteFileContents(target, "relative-symlink-payload"));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  auto created = core::CreateSymlink(link, L"..\\rel_target.dat",
                                     /*is_directory=*/false, Os().build);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  // Verify the reparse buffer records kSymlinkFlagRelative.
  {
    auto rp_h = OpenReparsePointHandle(link, FILE_READ_ATTRIBUTES,
                                       /*is_directory=*/false);
    ASSERT_TRUE(rp_h) << core::FormatWin32Error(rp_h.error);
    auto rp_data = core::GetReparsePointData(rp_h.value->get());
    ASSERT_TRUE(rp_data) << core::FormatWin32Error(rp_data.error);
    using SymlinkReparse = decltype(nt::ReparseDataBuffer::symbolic_link);
    ASSERT_GE(rp_data.value->size(), nt::kReparseDataBufferHeaderSize +
                                         offsetof(SymlinkReparse, path_buffer));
    const auto *rdb =
        reinterpret_cast<const nt::ReparseDataBuffer *>(rp_data.value->data());
    EXPECT_EQ(rdb->reparse_tag, ULONG{IO_REPARSE_TAG_SYMLINK});
    EXPECT_NE(rdb->symbolic_link.flags & nt::kSymlinkFlagRelative, ULONG{0});
  }

  EXPECT_TRUE(helpers::CheckFileContents(link, "relative-symlink-payload"));
}

// Spec: [MS-FSCC] 2.1.2.4 & Win32 Reparse Points — broken symlinks fail
// normal opens while FILE_FLAG_OPEN_REPARSE_POINT and DeleteFileW succeed.
TEST_F(LinksReparseTest, BrokenSymlinkOpenFailsAndOpenReparsePointSucceeds) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target = TestPath(L"broken_target.dat");
  const std::wstring link = TestPath(L"broken_link.dat");
  ASSERT_TRUE(core::WriteFileContents(target, "soon-deleted"));

  auto created =
      core::CreateSymlink(link, target, /*is_directory=*/false, Os().build);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  ASSERT_WIN32_TRUE(::DeleteFileW(target.c_str()));

  // Normal open fails ERROR_FILE_NOT_FOUND.
  auto normal = core::CreateWin32File(link, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(normal);
  EXPECT_WIN32_ERROR_CODE(normal.error, ERROR_FILE_NOT_FOUND);

  // FILE_FLAG_OPEN_REPARSE_POINT and FSCTL_GET_REPARSE_POINT still succeed.
  EXPECT_TRUE(CheckReparsePointTag(link, /*is_directory=*/false,
                                   IO_REPARSE_TAG_SYMLINK));

  // DeleteFileW removes the broken symlink cleanly.
  ASSERT_WIN32_TRUE(::DeleteFileW(link.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(link));
}

// Spec: [MS-FSCC] 2.1.2.5 Mount Point — directory junctions store
// IO_REPARSE_TAG_MOUNT_POINT, traverse to target, and remove independently.
TEST_F(LinksReparseTest, DirectoryJunctionCreateFollowAndRemove) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring target_dir = TestPath(L"junc_target");
  const std::wstring child = target_dir + L"\\child.dat";
  const std::wstring junction_dir = TestPath(L"junc_mount");

  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(child, "junction-payload"));

  auto created = core::CreateDirectoryJunction(junction_dir, target_dir);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  EXPECT_TRUE(CheckReparsePointTag(junction_dir, /*is_directory=*/true,
                                   IO_REPARSE_TAG_MOUNT_POINT));
  EXPECT_TRUE(helpers::CheckFileContents(junction_dir + L"\\child.dat",
                                         "junction-payload"));

  ASSERT_WIN32_TRUE(::RemoveDirectoryW(junction_dir.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(junction_dir));
  EXPECT_TRUE(helpers::CheckFileContents(child, "junction-payload"));
}

// Spec: [MS-FSA] 2.1.5.10 & [MS-FSCC] 2.3 — FSCTL_GET_REPARSE_POINT reads
// mount-point payloads and FSCTL_DELETE_REPARSE_POINT clears reparse state.
TEST_F(LinksReparseTest, GetAndDeleteReparsePointFsctl) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring target_dir = TestPath(L"fsctl_rp_target");
  const std::wstring junc_dir = TestPath(L"fsctl_rp_junc");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));

  auto created = core::CreateDirectoryJunction(junc_dir, target_dir);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  const DWORD attrs_before = ::GetFileAttributesW(junc_dir.c_str());
  ASSERT_NE(attrs_before, INVALID_FILE_ATTRIBUTES);
  EXPECT_NE(attrs_before & FILE_ATTRIBUTE_REPARSE_POINT, DWORD{0});

  auto h = OpenReparsePointHandle(junc_dir, GENERIC_READ | GENERIC_WRITE,
                                  /*is_directory=*/true);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

  // FSCTL_GET_REPARSE_POINT returns matching tag and substitute name.
  auto rp_data = core::GetReparsePointData(h.value->get());
  ASSERT_TRUE(rp_data) << core::FormatWin32Error(rp_data.error);
  using MountPointReparse = decltype(nt::ReparseDataBuffer::mount_point);
  const size_t path_buf_off = nt::kReparseDataBufferHeaderSize +
                              offsetof(MountPointReparse, path_buffer);
  ASSERT_GE(rp_data.value->size(), path_buf_off);
  const auto *rdb =
      reinterpret_cast<const nt::ReparseDataBuffer *>(rp_data.value->data());
  ASSERT_EQ(rdb->reparse_tag, ULONG{IO_REPARSE_TAG_MOUNT_POINT});
  const size_t sub_start =
      path_buf_off + rdb->mount_point.substitute_name_offset;
  const size_t sub_len = rdb->mount_point.substitute_name_length;
  ASSERT_EQ(sub_start % sizeof(wchar_t), size_t{0});
  ASSERT_EQ(sub_len % sizeof(wchar_t), size_t{0});
  ASSERT_LE(sub_start + sub_len, rp_data.value->size());
  std::wstring sub_name(sub_len / sizeof(wchar_t), L'\0');
  if (!sub_name.empty()) {
    std::memcpy(sub_name.data(), rp_data.value->data() + sub_start, sub_len);
  }
  const std::wstring expected_sub = core::VerbatimToNtPath(
      core::ToExtendedWin32Path(core::NormalizeWin32Path(target_dir)));
  EXPECT_EQ(sub_name, expected_sub);

  // FSCTL_DELETE_REPARSE_POINT clears FILE_ATTRIBUTE_REPARSE_POINT, leaving a
  // normal empty directory.
  auto del_res =
      core::DeleteReparsePoint(h.value->get(), IO_REPARSE_TAG_MOUNT_POINT);
  ASSERT_TRUE(del_res) << core::FormatWin32Error(del_res.error);

  auto info_after = core::GetByHandleInfo(h.value->get());
  ASSERT_TRUE(info_after) << core::FormatWin32Error(info_after.error);
  EXPECT_EQ(info_after.value->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT,
            DWORD{0});
  EXPECT_NE(info_after.value->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY,
            DWORD{0});

  auto after_get = core::GetReparsePointData(h.value->get());
  EXPECT_FALSE(after_get);
  EXPECT_WIN32_ERROR_CODE(after_get.error, ERROR_NOT_A_REPARSE_POINT);

  h.value->reset();
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(junc_dir.c_str()));
}

// Spec: Win32 FindFirstFileW / WIN32_FIND_DATAW — directory enumeration
// reports FILE_ATTRIBUTE_REPARSE_POINT and IO_REPARSE_TAG_MOUNT_POINT in
// dwReserved0 for directory junctions while plain directories omit the flag.
TEST_F(LinksReparseTest, FindFilesReportsReparseTagInReserved0) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring target_dir = TestPath(L"find_rp_target_dir");
  const std::wstring junc_dir = TestPath(L"find_rp_junc");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));
  auto junc_res = core::CreateDirectoryJunction(junc_dir, target_dir);
  ASSERT_TRUE(junc_res) << core::FormatWin32Error(junc_res.error);

  auto entries = CollectFindEntryInfos(TempDir().Path());
  ASSERT_TRUE(entries) << core::FormatWin32Error(entries.error);

  const auto junc_it = entries.value->find(L"find_rp_junc");
  ASSERT_NE(junc_it, entries.value->end());
  EXPECT_NE(junc_it->second.attributes & FILE_ATTRIBUTE_REPARSE_POINT,
            DWORD{0});
  EXPECT_EQ(junc_it->second.reserved0, DWORD{IO_REPARSE_TAG_MOUNT_POINT});

  const auto dir_it = entries.value->find(L"find_rp_target_dir");
  ASSERT_NE(dir_it, entries.value->end());
  EXPECT_EQ(dir_it->second.attributes & FILE_ATTRIBUTE_REPARSE_POINT, DWORD{0});
}

// Spec: Win32 FindFirstFileW / WIN32_FIND_DATAW — directory enumeration
// reports FILE_ATTRIBUTE_REPARSE_POINT and IO_REPARSE_TAG_SYMLINK in
// dwReserved0 for both file and directory symbolic links.
TEST_F(LinksReparseTest, FindFilesReportsSymlinkReparseTagInReserved0) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target_file = TestPath(L"find_sym_target.dat");
  const std::wstring target_dir = TestPath(L"find_sym_target_dir");
  const std::wstring sym_file = TestPath(L"find_sym_file.dat");
  const std::wstring sym_dir = TestPath(L"find_sym_dir");
  ASSERT_TRUE(core::WriteFileContents(target_file, "sym"));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));

  auto sym_file_res = core::CreateSymlink(sym_file, target_file,
                                          /*is_directory=*/false, Os().build);
  ASSERT_TRUE(sym_file_res) << core::FormatWin32Error(sym_file_res.error);
  auto sym_dir_res = core::CreateSymlink(sym_dir, target_dir,
                                         /*is_directory=*/true, Os().build);
  ASSERT_TRUE(sym_dir_res) << core::FormatWin32Error(sym_dir_res.error);

  auto entries = CollectFindEntryInfos(TempDir().Path());
  ASSERT_TRUE(entries) << core::FormatWin32Error(entries.error);

  const auto file_it = entries.value->find(L"find_sym_file.dat");
  ASSERT_NE(file_it, entries.value->end());
  EXPECT_NE(file_it->second.attributes & FILE_ATTRIBUTE_REPARSE_POINT,
            DWORD{0});
  EXPECT_EQ(file_it->second.attributes & FILE_ATTRIBUTE_DIRECTORY, DWORD{0});
  EXPECT_EQ(file_it->second.reserved0, DWORD{IO_REPARSE_TAG_SYMLINK});

  const auto dir_it = entries.value->find(L"find_sym_dir");
  ASSERT_NE(dir_it, entries.value->end());
  EXPECT_NE(dir_it->second.attributes & FILE_ATTRIBUTE_REPARSE_POINT, DWORD{0});
  EXPECT_NE(dir_it->second.attributes & FILE_ATTRIBUTE_DIRECTORY, DWORD{0});
  EXPECT_EQ(dir_it->second.reserved0, DWORD{IO_REPARSE_TAG_SYMLINK});

  const auto plain_it = entries.value->find(L"find_sym_target.dat");
  ASSERT_NE(plain_it, entries.value->end());
  EXPECT_EQ(plain_it->second.attributes & FILE_ATTRIBUTE_REPARSE_POINT,
            DWORD{0});
}

// Spec: [MS-FSCC] 2.1.2.3 REPARSE_GUID_DATA_BUFFER — non-Microsoft custom
// GUID reparse tags round-trip byte-for-byte and fail unhandled opens.
TEST_F(LinksReparseTest, CustomApplicationReparseTagRoundTrip) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  // Valid non-Microsoft application reparse tag: bit 31 (Microsoft) = 0,
  // reserved bits 30 and 27..16 = 0, non-zero low 16 bits.
  constexpr ULONG kCustomTag = 0x00000A5AUL;
  EXPECT_FALSE(IsReparseTagMicrosoft(kCustomTag) != 0);

  constexpr GUID kCustomGuid = {
      0x4e746673UL,
      0x4673U,
      0x5465U,
      {0x73, 0x74, 0x52, 0x65, 0x70, 0x61, 0x72, 0x73}};
  constexpr uint8_t kPayload[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65,
                                    0x76, 0x87, 0x98, 0xA9, 0xBA, 0xCB,
                                    0xDC, 0xED, 0xFE, 0x0F};

  const std::wstring path = TestPath(L"custom_reparse.dat");
  ASSERT_TRUE(helpers::CreateEmptyFileResult(path));

  auto h = OpenReparsePointHandle(path, GENERIC_READ | GENERIC_WRITE,
                                  /*is_directory=*/false);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

  auto set_res = SetCustomReparsePoint(h.value->get(), kCustomTag, kCustomGuid,
                                       kPayload, sizeof(kPayload));
  ASSERT_TRUE(set_res) << core::FormatWin32Error(set_res.error);

  auto info = core::GetByHandleInfo(h.value->get());
  ASSERT_TRUE(info) << core::FormatWin32Error(info.error);
  EXPECT_NE(info.value->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT,
            DWORD{0});

  // Read back and verify tag, GUID, and payload byte-for-byte.
  EXPECT_TRUE(CheckCustomReparsePoint(h.value->get(), kCustomTag, kCustomGuid,
                                      kPayload, sizeof(kPayload)));

  // Opening without FILE_FLAG_OPEN_REPARSE_POINT fails because no filter
  // handles the custom tag.
  auto unhandled = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(unhandled);
  EXPECT_WIN32_ERROR_CODE(unhandled.error, ERROR_CANT_ACCESS_FILE);

  // Delete the custom reparse point and verify normal open works again.
  auto del = core::DeleteReparsePoint(h.value->get(), kCustomTag, &kCustomGuid);
  ASSERT_TRUE(del) << core::FormatWin32Error(del.error);

  auto info_after = core::GetByHandleInfo(h.value->get());
  ASSERT_TRUE(info_after) << core::FormatWin32Error(info_after.error);
  EXPECT_EQ(info_after.value->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT,
            DWORD{0});
  h.value->reset();

  auto normal = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_TRUE(normal) << core::FormatWin32Error(normal.error);
}

// Spec: [MS-FSA] 2.1.5.15.7 & Win32 CreateHardLinkW — creating a hard link to
// a FILE_ATTRIBUTE_READONLY target succeeds and preserves shared attributes.
TEST_F(LinksReparseTest, HardLinkOnReadOnlyFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring link_a = TestPath(L"ro_hl_a.dat");
  const std::wstring link_b = TestPath(L"ro_hl_b.dat");
  ASSERT_TRUE(core::WriteFileContents(link_a, "readonly-hardlink"));
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(link_a.c_str(), FILE_ATTRIBUTE_READONLY));

  // CreateHardLinkW succeeds even when the target file is READONLY.
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));
  auto info_a = core::QueryInfoByPath(link_a);
  auto info_b = core::QueryInfoByPath(link_b);
  ASSERT_TRUE(info_a) << core::FormatWin32Error(info_a.error);
  ASSERT_TRUE(info_b) << core::FormatWin32Error(info_b.error);
  EXPECT_EQ(core::FileIndexOf(*info_a.value), core::FileIndexOf(*info_b.value));
  EXPECT_EQ(info_b.value->nNumberOfLinks, DWORD{2});
  EXPECT_NE(info_b.value->dwFileAttributes & FILE_ATTRIBUTE_READONLY, DWORD{0});

  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(link_a.c_str(), FILE_ATTRIBUTE_NORMAL));
}

// Spec: [MS-FSA] 2.1.5.15.7 & Win32 File Streams — all hard links to the same
// file share alternate data streams for reads, writes, and stream deletion.
TEST_F(LinksReparseTest, HardLinkSharesAlternateDataStreams) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring link_a = TestPath(L"ads_hl_a.dat");
  const std::wstring link_b = TestPath(L"ads_hl_b.dat");
  ASSERT_TRUE(core::WriteFileContents(link_a, "ads-hardlink-base"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));

  ASSERT_TRUE(core::WriteFileContents(link_a + L":ads", "shared-ads-v1"));
  EXPECT_TRUE(helpers::CheckFileContents(link_b + L":ads", "shared-ads-v1"));

  ASSERT_TRUE(core::WriteFileContents(link_b + L":ads", "shared-ads-v2"));
  EXPECT_TRUE(helpers::CheckFileContents(link_a + L":ads", "shared-ads-v2"));

  ASSERT_WIN32_TRUE(::DeleteFileW((link_a + L":ads").c_str()));
  auto gone_b = core::ReadFileContents(link_b + L":ads");
  EXPECT_FALSE(gone_b);
  EXPECT_WIN32_ERROR_CODE(gone_b.error, ERROR_FILE_NOT_FOUND);
}

// Spec: [MS-FSA] 2.1.5.15.7 & Win32 CreateHardLinkW — creating a hard link
// over an existing file fails ERROR_ALREADY_EXISTS and leaves it untouched.
TEST_F(LinksReparseTest, HardLinkOverExistingFileFailsAlreadyExists) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring src = TestPath(L"hl_src.dat");
  const std::wstring dst = TestPath(L"hl_existing_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "source-bytes"));
  ASSERT_TRUE(core::WriteFileContents(dst, "existing-dst-bytes"));

  auto src_info = core::QueryInfoByPath(src);
  auto dst_info = core::QueryInfoByPath(dst);
  ASSERT_TRUE(src_info) << core::FormatWin32Error(src_info.error);
  ASSERT_TRUE(dst_info) << core::FormatWin32Error(dst_info.error);
  const uint64_t src_index = core::FileIndexOf(*src_info.value);
  const uint64_t dst_index = core::FileIndexOf(*dst_info.value);
  EXPECT_NE(src_index, dst_index);

  EXPECT_WIN32_ERROR(::CreateHardLinkW(dst.c_str(), src.c_str(), nullptr),
                     ERROR_ALREADY_EXISTS);

  // Neither file was relinked: both keep their own FileIndex, a link count of
  // one, and their original contents.
  EXPECT_TRUE(CheckLinkFileIndexAndCount(src, src_index, DWORD{1}));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(dst, dst_index, DWORD{1}));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "existing-dst-bytes"));
}

// Spec: WDK FileLinkInformationEx — on Windows 10 1903+ a link request with
// FILE_LINK_REPLACE_IF_EXISTS | FILE_LINK_POSIX_SEMANTICS atomically replaces
// an existing target even while another handle holds that target open.
TEST_F(LinksReparseTest, LinkInfoExPosixReplacesOpenTarget) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1903);

  const std::wstring src = TestPath(L"hl_src.dat");
  const std::wstring dst = TestPath(L"hl_existing_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "source-bytes"));
  ASSERT_TRUE(core::WriteFileContents(dst, "existing-dst-bytes"));

  auto src_info = core::QueryInfoByPath(src);
  ASSERT_TRUE(src_info) << core::FormatWin32Error(src_info.error);
  const uint64_t src_index = core::FileIndexOf(*src_info.value);

  auto h_old_dst = core::CreateWin32File(dst, GENERIC_READ, core::kShareAll,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_old_dst) << core::FormatWin32Error(h_old_dst.error);

  auto dir_h = core::CreateWin32File(
      TempDir().Path(), FILE_ADD_FILE | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  auto src_h = core::CreateWin32File(src, FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                                     core::kShareAll, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(src_h) << core::FormatWin32Error(src_h.error);

  auto link_ex = core::SetLinkInfoEx(
      src_h.value->get(), L"hl_existing_dst.dat", dir_h.value->get(),
      nt::kFileLinkReplaceIfExists | nt::kFileLinkPosixSemantics);
  ASSERT_NT_SUCCESS(link_ex.status);

  // While h_old_dst stays open, dst now resolves to src (same FileIndex).
  EXPECT_TRUE(CheckLinkFileIndexAndCount(dst, src_index, DWORD{2}));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "source-bytes"));

  // Pre-existing handle h_old_dst continues reading the replaced file.
  auto old_data = core::ReadAllBytes(h_old_dst.value->get());
  ASSERT_TRUE(old_data) << core::FormatWin32Error(old_data.error);
  EXPECT_EQ(*old_data.value, "existing-dst-bytes");
}

// Spec: Win32 CreateHardLinkW — NTFS allows at most 1023 additional hard links
// per file (1024 names in total); creating one more fails ERROR_TOO_MANY_LINKS
// without creating a directory entry.
TEST_F(LinksReparseTest, HardLinkCountLimitFailsTooManyLinks) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring dir = TestPath(L"many_links");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  const std::wstring base = dir + L"\\link_0.dat";
  ASSERT_TRUE(core::WriteFileContents(base, "link-limit"));

  auto base_info = core::QueryInfoByPath(base);
  ASSERT_TRUE(base_info) << core::FormatWin32Error(base_info.error);
  const uint64_t base_index = core::FileIndexOf(*base_info.value);

  for (DWORD i = 1; i < kMaxLinksPerFile; ++i) {
    const std::wstring link = dir + L"\\link_" + std::to_wstring(i) + L".dat";
    ASSERT_WIN32_TRUE(::CreateHardLinkW(link.c_str(), base.c_str(), nullptr))
        << "while creating additional hard link #" << i;
  }
  EXPECT_TRUE(CheckLinkFileIndexAndCount(base, base_index, kMaxLinksPerFile));

  const std::wstring overflow = dir + L"\\link_overflow.dat";
  EXPECT_WIN32_ERROR(::CreateHardLinkW(overflow.c_str(), base.c_str(), nullptr),
                     ERROR_TOO_MANY_LINKS);
  EXPECT_TRUE(helpers::CheckFileNotFound(overflow));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(base, base_index, kMaxLinksPerFile));
}

// Spec: [MS-FSA] 2.1.5.15.12 & 2.1.5.15.7 — renaming one hard link updates
// only that directory entry while preserving FileIndex and sibling links.
TEST_F(LinksReparseTest, HardLinkRenameOneLinkPreservesOtherLinks) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring link_a = TestPath(L"hl_ren_a.dat");
  const std::wstring link_b = TestPath(L"hl_ren_b.dat");
  const std::wstring link_a2 = TestPath(L"hl_ren_a_moved.dat");

  ASSERT_TRUE(core::WriteFileContents(link_a, "rename-one-link"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));
  auto orig_info = core::QueryInfoByPath(link_a);
  ASSERT_TRUE(orig_info) << core::FormatWin32Error(orig_info.error);
  const uint64_t expected_index = core::FileIndexOf(*orig_info.value);

  ASSERT_WIN32_TRUE(::MoveFileExW(link_a.c_str(), link_a2.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(link_a));

  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_a2, expected_index, DWORD{2}));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(link_b, expected_index, DWORD{2}));

  auto links = EnumerateHardLinks(link_b);
  ASSERT_TRUE(links) << core::FormatWin32Error(links.error);
  EXPECT_MULTISET_EQ(
      ExtractTempRelativePaths(*links.value, TempDir().Basename()),
      (std::vector<std::wstring>{L"hl_ren_a_moved.dat", L"hl_ren_b.dat"}));
}

// Spec: Win32 Symbolic Links — cyclic symbolic links fail normal opens with
// ERROR_CANT_RESOLVE_FILENAME while open-reparse-point and delete succeed.
TEST_F(LinksReparseTest, SymlinkLoopDetectedAsCantResolveFilename) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring link_a = TestPath(L"loop_a.dat");
  const std::wstring link_b = TestPath(L"loop_b.dat");

  ASSERT_TRUE(core::CreateSymlink(link_a, L"loop_b.dat",
                                  /*is_directory=*/false, Os().build));
  ASSERT_TRUE(core::CreateSymlink(link_b, L"loop_a.dat",
                                  /*is_directory=*/false, Os().build));

  // Following the loop fails with ERROR_CANT_RESOLVE_FILENAME.
  auto followed = core::CreateWin32File(link_a, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(followed);
  EXPECT_WIN32_ERROR_CODE(followed.error, ERROR_CANT_RESOLVE_FILENAME);

  // Opening with FILE_FLAG_OPEN_REPARSE_POINT and DeleteFileW both succeed.
  EXPECT_TRUE(CheckReparsePointTag(link_a, /*is_directory=*/false,
                                   IO_REPARSE_TAG_SYMLINK));

  EXPECT_WIN32_TRUE(::DeleteFileW(link_a.c_str()));
  EXPECT_WIN32_TRUE(::DeleteFileW(link_b.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.12 & Win32 Symbolic Links — moving a symlink
// moves the link itself and symlink attributes stay independent of target.
TEST_F(LinksReparseTest,
       RenameSymlinkRenamesLinkNotTargetAndIndependentMetadata) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target = TestPath(L"sym_ren_target.dat");
  const std::wstring link1 = TestPath(L"sym_ren_1.dat");
  const std::wstring link2 = TestPath(L"sym_ren_2.dat");

  ASSERT_TRUE(core::WriteFileContents(target, "target-stays-put"));
  auto target_info = core::QueryInfoByPath(target);
  ASSERT_TRUE(target_info) << core::FormatWin32Error(target_info.error);
  const uint64_t target_index = core::FileIndexOf(*target_info.value);

  ASSERT_TRUE(core::CreateSymlink(link1, L"sym_ren_target.dat",
                                  /*is_directory=*/false, Os().build));

  // MoveFileExW moves the symlink itself without moving or renaming the target.
  ASSERT_WIN32_TRUE(::MoveFileExW(link1.c_str(), link2.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(link1));
  EXPECT_TRUE(CheckLinkFileIndexAndCount(target, target_index, DWORD{1}));

  // Symlink metadata (e.g. FILE_ATTRIBUTE_HIDDEN) is independent of target.
  {
    auto rp_h = OpenReparsePointHandle(
        link2, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        /*is_directory=*/false);
    ASSERT_TRUE(rp_h) << core::FormatWin32Error(rp_h.error);
    auto rp_info = core::GetByHandleInfo(rp_h.value->get());
    ASSERT_TRUE(rp_info) << core::FormatWin32Error(rp_info.error);
    EXPECT_NE(rp_info.value->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT,
              DWORD{0});
    EXPECT_NE(core::FileIndexOf(*rp_info.value), target_index);

    nt::FileBasicInfo basic{};
    basic.file_attributes =
        rp_info.value->dwFileAttributes | FILE_ATTRIBUTE_HIDDEN;
    ASSERT_NT_SUCCESS(core::SetBasicInfo(rp_h.value->get(), basic).status);

    const DWORD link2_attrs = ::GetFileAttributesW(link2.c_str());
    const DWORD target_attrs = ::GetFileAttributesW(target.c_str());
    ASSERT_NE(link2_attrs, INVALID_FILE_ATTRIBUTES);
    ASSERT_NE(target_attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_NE(link2_attrs & FILE_ATTRIBUTE_HIDDEN, DWORD{0});
    EXPECT_EQ(target_attrs & FILE_ATTRIBUTE_HIDDEN, DWORD{0});

    // Clear HIDDEN on link2 (FILE_ATTRIBUTE_NORMAL never passes 0, which
    // FileBasicInformation treats as "no attribute change").
    basic.file_attributes = FILE_ATTRIBUTE_NORMAL;
    ASSERT_NT_SUCCESS(core::SetBasicInfo(rp_h.value->get(), basic).status);
  }

  // Opening link2 normally follows to target and GetFinalPathNameByHandleW
  // resolves to the target path.
  auto followed = core::CreateWin32File(link2, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(followed) << core::FormatWin32Error(followed.error);
  auto data = core::ReadAllBytes(followed.value->get());
  ASSERT_TRUE(data) << core::FormatWin32Error(data.error);
  EXPECT_EQ(*data.value, "target-stays-put");

  const std::wstring expected_suffix =
      L"\\" + TempDir().Basename() + L"\\sym_ren_target.dat";
  EXPECT_TRUE(CheckFinalPathEndsWith(followed.value->get(), expected_suffix));
}

// Spec: [MS-FSCC] 2.1.2.5 & Win32 RemoveDirectoryW — removing a directory
// junction succeeds even when its target directory contains nested files.
TEST_F(LinksReparseTest, RemoveJunctionPointingToNonEmptyDirectorySucceeds) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring target_dir = TestPath(L"junc_nonempty_target");
  const std::wstring nested_dir = target_dir + L"\\nested";
  const std::wstring file1 = target_dir + L"\\file1.dat";
  const std::wstring file2 = target_dir + L"\\file2.dat";
  const std::wstring deep = nested_dir + L"\\deep.dat";
  const std::wstring junction_dir = TestPath(L"junc_nonempty_link");

  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(nested_dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(file1, "payload-one"));
  ASSERT_TRUE(core::WriteFileContents(file2, "payload-two"));
  ASSERT_TRUE(core::WriteFileContents(deep, "payload-three"));

  auto created = core::CreateDirectoryJunction(junction_dir, target_dir);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  // Confirm junction resolves into nested content.
  EXPECT_TRUE(helpers::CheckFileContents(junction_dir + L"\\nested\\deep.dat",
                                         "payload-three"));

  // RemoveDirectoryW on the junction succeeds even though target is non-empty.
  ASSERT_WIN32_TRUE(::RemoveDirectoryW(junction_dir.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(junction_dir));

  // All target files and nested directory remain completely intact.
  EXPECT_TRUE(helpers::CheckFileContents(file1, "payload-one"));
  EXPECT_TRUE(helpers::CheckFileContents(file2, "payload-two"));
  EXPECT_TRUE(helpers::CheckFileContents(deep, "payload-three"));
}

// Spec: [MS-FSA] 2.1.5.10 FSCTL_SET_REPARSE_POINT — setting a mount-point
// junction on a non-empty directory fails ERROR_DIR_NOT_EMPTY until emptied.
TEST_F(LinksReparseTest, JunctionOnNonEmptyDirectoryFailsNotEmpty) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring target_dir = TestPath(L"junc_nonempty_src_target");
  const std::wstring non_empty_mount = TestPath(L"junc_nonempty_mount");
  const std::wstring blocker = non_empty_mount + L"\\blocker.dat";

  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(non_empty_mount.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(blocker, "blocker-payload"));

  // Setting an IO_REPARSE_TAG_MOUNT_POINT reparse point on a non-empty
  // directory must fail with ERROR_DIR_NOT_EMPTY.
  auto res = core::CreateDirectoryJunction(non_empty_mount, target_dir);
  EXPECT_FALSE(res);
  EXPECT_WIN32_ERROR_CODE(res.error, ERROR_DIR_NOT_EMPTY);

  const DWORD attrs = ::GetFileAttributesW(non_empty_mount.c_str());
  ASSERT_NE(attrs, INVALID_FILE_ATTRIBUTES);
  EXPECT_EQ(attrs & FILE_ATTRIBUTE_REPARSE_POINT, DWORD{0});
  EXPECT_TRUE(helpers::CheckFileContents(blocker, "blocker-payload"));

  // Once the directory is emptied, the same junction creation succeeds.
  ASSERT_WIN32_TRUE(::DeleteFileW(blocker.c_str()));
  auto created = core::CreateDirectoryJunction(non_empty_mount, target_dir);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(non_empty_mount.c_str()));
}

// Spec: [MS-FSA] 2.1.5.10 FSCTL_GET_REPARSE_POINT — sub-header buffers fail
// ERROR_INSUFFICIENT_BUFFER; header-sized buffers fail ERROR_MORE_DATA.
TEST_F(LinksReparseTest, GetReparsePointSmallBufferFailsMoreData) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring target_dir = TestPath(L"small_buf_target");
  const std::wstring small_buf_junc = TestPath(L"small_buf_junc");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));

  auto created = core::CreateDirectoryJunction(small_buf_junc, target_dir);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  {
    auto h = OpenReparsePointHandle(small_buf_junc, FILE_READ_ATTRIBUTES,
                                    /*is_directory=*/true);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    // Buffer smaller than REPARSE_GUID_DATA_BUFFER_HEADER_SIZE (24 bytes) fails
    // with ERROR_INSUFFICIENT_BUFFER (STATUS_BUFFER_TOO_SMALL).
    alignas(8) uint8_t tiny_buf[16]{};
    DWORD bytes_returned = 0;
    EXPECT_WIN32_ERROR(
        ::DeviceIoControl(h.value->get(), FSCTL_GET_REPARSE_POINT, nullptr, 0,
                          tiny_buf, static_cast<DWORD>(sizeof(tiny_buf)),
                          &bytes_returned, nullptr),
        ERROR_INSUFFICIENT_BUFFER);

    // Buffer >= 24 bytes (holds header) but smaller than the full mount-point
    // payload fails with ERROR_MORE_DATA (STATUS_BUFFER_OVERFLOW) and populates
    // the reparse header.
    alignas(8) uint8_t header_buf[32]{};
    bytes_returned = 0;
    EXPECT_WIN32_ERROR(
        ::DeviceIoControl(h.value->get(), FSCTL_GET_REPARSE_POINT, nullptr, 0,
                          header_buf, static_cast<DWORD>(sizeof(header_buf)),
                          &bytes_returned, nullptr),
        ERROR_MORE_DATA);
    ASSERT_GE(bytes_returned, DWORD{nt::kReparseDataBufferHeaderSize});
    const auto *partial_rdb =
        reinterpret_cast<const nt::ReparseDataBuffer *>(header_buf);
    EXPECT_EQ(partial_rdb->reparse_tag, ULONG{IO_REPARSE_TAG_MOUNT_POINT});
    EXPECT_GT(partial_rdb->reparse_data_length, USHORT{0});

    // Confirm the full query succeeds and exceeds the 32-byte probe buffer.
    auto full_data = core::GetReparsePointData(h.value->get());
    ASSERT_TRUE(full_data) << core::FormatWin32Error(full_data.error);
    ASSERT_GT(full_data.value->size(), sizeof(header_buf));
    const auto *rdb = reinterpret_cast<const nt::ReparseDataBuffer *>(
        full_data.value->data());
    EXPECT_EQ(rdb->reparse_tag, ULONG{IO_REPARSE_TAG_MOUNT_POINT});
  }

  EXPECT_WIN32_TRUE(::RemoveDirectoryW(small_buf_junc.c_str()));
}

// Spec: [MS-FSCC] 2.1.2.4 & Win32 File Streams — opening and creating
// alternate data streams through a file symlink targets the symlink target.
TEST_F(LinksReparseTest, FileSymlinkToAlternateDataStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target = TestPath(L"sym_ads_target.dat");
  const std::wstring link = TestPath(L"sym_ads_link.dat");

  ASSERT_TRUE(core::WriteFileContents(target, "base-data"));
  ASSERT_TRUE(core::WriteFileContents(target + L":ads1", "stream-via-symlink"));

  auto created =
      core::CreateSymlink(link, target, /*is_directory=*/false, Os().build);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  // Reading an alternate data stream through the file symlink resolves to the
  // target's stream.
  EXPECT_TRUE(
      helpers::CheckFileContents(link + L":ads1", "stream-via-symlink"));

  // Creating and writing a new alternate data stream through the symlink
  // materializes on the target file without disturbing the base stream.
  auto write_ads2 =
      core::WriteFileContents(link + L":ads2", "written-via-symlink");
  ASSERT_TRUE(write_ads2) << core::FormatWin32Error(write_ads2.error);

  EXPECT_TRUE(
      helpers::CheckFileContents(target + L":ads2", "written-via-symlink"));
  EXPECT_TRUE(helpers::CheckFileContents(link, "base-data"));
}

// Spec: [MS-FSCC] 2.4.17 FileHardLinkInformation — querying hard-link
// entries lists every link's parent FileIndex and leaf name across dirs.
TEST_F(LinksReparseTest, FileHardLinkInformationListsAllLinks) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring sub = TestPath(L"sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  auto root_dir_id =
      core::QueryPathFileIndex(TempDir().Path(), /*backup_semantics=*/true);
  auto sub_dir_id = core::QueryPathFileIndex(sub, /*backup_semantics=*/true);
  ASSERT_TRUE(root_dir_id) << core::FormatWin32Error(root_dir_id.error);
  ASSERT_TRUE(sub_dir_id) << core::FormatWin32Error(sub_dir_id.error);
  ASSERT_NE(*root_dir_id.value, *sub_dir_id.value);

  const std::wstring link_a = TestPath(L"hl_info_a.dat");
  const std::wstring link_b = TestPath(L"hl_info_b.dat");
  const std::wstring link_c = sub + L"\\hl_info_c.dat";

  ASSERT_TRUE(core::WriteFileContents(link_a, "hardlink-info-payload"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_c.c_str(), link_a.c_str(), nullptr));

  auto h = core::CreateWin32File(link_a, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 core::kShareAll, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

  EXPECT_TRUE(CheckHardLinkEntries(h.value->get(),
                                   {{*root_dir_id.value, L"hl_info_a.dat"},
                                    {*root_dir_id.value, L"hl_info_b.dat"},
                                    {*sub_dir_id.value, L"hl_info_c.dat"}}));

  // Deleting one hard link immediately reduces FileHardLinkInformation to 2.
  ASSERT_WIN32_TRUE(::DeleteFileW(link_b.c_str()));

  EXPECT_TRUE(CheckHardLinkEntries(h.value->get(),
                                   {{*root_dir_id.value, L"hl_info_a.dat"},
                                    {*sub_dir_id.value, L"hl_info_c.dat"}}));
}

// Spec: [MS-FSCC] 2.1.2.5 Mount Point & Win32 GetFinalPathNameByHandleW —
// traversing an intermediate directory junction reads, writes, creates files
// under the target tree, and resolves final paths to the target directory.
TEST_F(LinksReparseTest, JunctionIntermediateComponentTraversal) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring target_root = TestPath(L"junc_inter_target");
  const std::wstring target_sub = target_root + L"\\sub";
  const std::wstring target_file = target_sub + L"\\nested.dat";
  const std::wstring junc_root = TestPath(L"junc_inter_link");

  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_root.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_sub.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(target_file, "initial-junction-nested"));

  auto created = core::CreateDirectoryJunction(junc_root, target_root);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  const std::wstring via_junc_file = junc_root + L"\\sub\\nested.dat";
  const std::wstring via_junc_created = junc_root + L"\\sub\\created.dat";
  const std::wstring target_created = target_sub + L"\\created.dat";

  EXPECT_TRUE(
      helpers::CheckFileContents(via_junc_file, "initial-junction-nested"));

  // Writing and creating through the intermediate junction updates target_sub.
  ASSERT_TRUE(
      core::WriteFileContents(via_junc_file, "updated-through-junction"));
  EXPECT_TRUE(
      helpers::CheckFileContents(target_file, "updated-through-junction"));

  ASSERT_TRUE(
      core::WriteFileContents(via_junc_created, "created-through-junction"));
  EXPECT_TRUE(
      helpers::CheckFileContents(target_created, "created-through-junction"));

  const std::wstring expected_suffix =
      L"\\" + TempDir().Basename() + L"\\junc_inter_target\\sub\\nested.dat";
  EXPECT_TRUE(CheckPathFinalPathEndsWith(via_junc_file, expected_suffix));

  ASSERT_WIN32_TRUE(::RemoveDirectoryW(junc_root.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(junc_root));
  EXPECT_TRUE(
      helpers::CheckFileContents(target_created, "created-through-junction"));
}

// Spec: [MS-FSCC] 2.1.2.4 & 2.1.2.5 / Win32 Symbolic Links — chained file and
// directory symlinks and mixed symlink-to-junction intermediate path components
// resolve transitively for reads, writes, creation, and final-path queries.
TEST_F(LinksReparseTest, ChainedSymlinksAndIntermediateComponentTraversal) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();
  REQUIRE_LOCAL_VOLUME();

  // Part 1: Chained file symlinks sym_a -> sym_b -> chain_target.dat.
  const std::wstring target_file = TestPath(L"chain_target.dat");
  const std::wstring sym_b = TestPath(L"chain_sym_b.dat");
  const std::wstring sym_a = TestPath(L"chain_sym_a.dat");
  ASSERT_TRUE(core::WriteFileContents(target_file, "chained-file-v1"));

  auto target_info = core::QueryInfoByPath(target_file);
  ASSERT_TRUE(target_info) << core::FormatWin32Error(target_info.error);
  const uint64_t target_index = core::FileIndexOf(*target_info.value);

  auto res_b = core::CreateSymlink(sym_b, target_file, /*is_directory=*/false,
                                   Os().build);
  ASSERT_TRUE(res_b) << core::FormatWin32Error(res_b.error);
  auto res_a =
      core::CreateSymlink(sym_a, sym_b, /*is_directory=*/false, Os().build);
  ASSERT_TRUE(res_a) << core::FormatWin32Error(res_a.error);

  EXPECT_TRUE(helpers::CheckFileContents(sym_a, "chained-file-v1"));
  {
    auto followed = core::CreateWin32File(sym_a, GENERIC_READ, core::kShareAll,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(followed) << core::FormatWin32Error(followed.error);
    auto followed_info = core::GetByHandleInfo(followed.value->get());
    ASSERT_TRUE(followed_info) << core::FormatWin32Error(followed_info.error);
    EXPECT_EQ(core::FileIndexOf(*followed_info.value), target_index);
    const std::wstring expected_file_suffix =
        L"\\" + TempDir().Basename() + L"\\chain_target.dat";
    EXPECT_TRUE(
        CheckFinalPathEndsWith(followed.value->get(), expected_file_suffix));
  }

  ASSERT_TRUE(core::WriteFileContents(sym_a, "chained-file-v2"));
  EXPECT_TRUE(helpers::CheckFileContents(sym_b, "chained-file-v2"));
  EXPECT_TRUE(helpers::CheckFileContents(target_file, "chained-file-v2"));

  // Part 2: Chained directory symlinks sym_dir_a -> sym_dir_b -> real_dir\sub.
  const std::wstring real_dir = TestPath(L"chain_real_dir");
  const std::wstring real_sub = real_dir + L"\\sub";
  const std::wstring real_leaf = real_sub + L"\\leaf.dat";
  const std::wstring sym_dir_b = TestPath(L"chain_sym_dir_b");
  const std::wstring sym_dir_a = TestPath(L"chain_sym_dir_a");

  ASSERT_WIN32_TRUE(::CreateDirectoryW(real_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(real_sub.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(real_leaf, "chained-dir-v1"));

  auto dir_b_res = core::CreateSymlink(sym_dir_b, real_dir,
                                       /*is_directory=*/true, Os().build);
  ASSERT_TRUE(dir_b_res) << core::FormatWin32Error(dir_b_res.error);
  auto dir_a_res = core::CreateSymlink(sym_dir_a, sym_dir_b,
                                       /*is_directory=*/true, Os().build);
  ASSERT_TRUE(dir_a_res) << core::FormatWin32Error(dir_a_res.error);

  const std::wstring via_chain_leaf = sym_dir_a + L"\\sub\\leaf.dat";
  EXPECT_TRUE(helpers::CheckFileContents(via_chain_leaf, "chained-dir-v1"));
  ASSERT_TRUE(core::WriteFileContents(via_chain_leaf, "chained-dir-v2"));
  EXPECT_TRUE(helpers::CheckFileContents(real_leaf, "chained-dir-v2"));

  ASSERT_TRUE(core::WriteFileContents(sym_dir_a + L"\\sub\\sibling.dat",
                                      "created-via-chained-sym"));
  EXPECT_TRUE(helpers::CheckFileContents(real_sub + L"\\sibling.dat",
                                         "created-via-chained-sym"));

  const std::wstring expected_leaf_suffix =
      L"\\" + TempDir().Basename() + L"\\chain_real_dir\\sub\\leaf.dat";
  EXPECT_TRUE(CheckPathFinalPathEndsWith(via_chain_leaf, expected_leaf_suffix));

  // Part 3: Mixed symlink -> junction -> real_dir intermediate traversal.
  const std::wstring junc_dir = TestPath(L"chain_junc_dir");
  const std::wstring sym_to_junc = TestPath(L"chain_sym_to_junc");
  auto junc_res = core::CreateDirectoryJunction(junc_dir, real_dir);
  ASSERT_TRUE(junc_res) << core::FormatWin32Error(junc_res.error);
  auto sym_junc_res = core::CreateSymlink(sym_to_junc, junc_dir,
                                          /*is_directory=*/true, Os().build);
  ASSERT_TRUE(sym_junc_res) << core::FormatWin32Error(sym_junc_res.error);

  const std::wstring via_mixed_leaf = sym_to_junc + L"\\sub\\leaf.dat";
  EXPECT_TRUE(helpers::CheckFileContents(via_mixed_leaf, "chained-dir-v2"));
  EXPECT_TRUE(CheckPathFinalPathEndsWith(via_mixed_leaf, expected_leaf_suffix));
}

// Spec: [MS-FSA] FSCTL_DELETE_REPARSE_POINT — deleting a reparse point with a
// reparse tag that differs from the stored tag fails
// STATUS_IO_REPARSE_TAG_MISMATCH (ERROR_REPARSE_TAG_MISMATCH) and leaves the
// stored reparse point intact.
TEST_F(LinksReparseTest, DeleteReparsePointWithMismatchedTagFails) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  core::UniqueHandle h;
  ASSERT_TRUE(
      CreateFileWithAppReparsePoint(TestPath(L"del_tag_mismatch.dat"), &h));

  auto del_diff_tag = core::DeleteReparsePoint(h.get(), kAppTag2, &kAppGuid1);
  EXPECT_FALSE(del_diff_tag);
  EXPECT_WIN32_ERROR_CODE(del_diff_tag.error, ERROR_REPARSE_TAG_MISMATCH);

  EXPECT_TRUE(CheckCustomReparsePoint(h.get(), kAppTag1, kAppGuid1,
                                      kAppPayload1, sizeof(kAppPayload1)));
}

// Spec: [MS-FSA] FSCTL_DELETE_REPARSE_POINT — deleting a non-Microsoft reparse
// point with the stored tag but a different GUID fails
// STATUS_REPARSE_ATTRIBUTE_CONFLICT (ERROR_REPARSE_ATTRIBUTE_CONFLICT) and
// leaves the stored reparse point intact.
TEST_F(LinksReparseTest, DeleteReparsePointWithMismatchedGuidFails) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  core::UniqueHandle h;
  ASSERT_TRUE(
      CreateFileWithAppReparsePoint(TestPath(L"del_guid_mismatch.dat"), &h));

  auto del_diff_guid = core::DeleteReparsePoint(h.get(), kAppTag1, &kAppGuid2);
  EXPECT_FALSE(del_diff_guid);
  EXPECT_WIN32_ERROR_CODE(del_diff_guid.error,
                          ERROR_REPARSE_ATTRIBUTE_CONFLICT);

  EXPECT_TRUE(CheckCustomReparsePoint(h.get(), kAppTag1, kAppGuid1,
                                      kAppPayload1, sizeof(kAppPayload1)));
}

// Spec: [MS-FSA] FSCTL_SET_REPARSE_POINT — overwriting an existing reparse
// point with a different reparse tag fails STATUS_IO_REPARSE_TAG_MISMATCH
// (ERROR_REPARSE_TAG_MISMATCH) and leaves the stored reparse point intact.
//
// Windows 10 1809 / Server 2019 (build 17763+) enforces existing (tag, GUID)
// matching on FSCTL_SET_REPARSE_POINT overwrites; Windows Server 2016
// (build 14393) ntfs.sys allowed replacing a custom non-Microsoft reparse
// point in place with a different tag or GUID.
TEST_F(LinksReparseTest, SetReparsePointWithMismatchedTagFails) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  core::UniqueHandle h;
  ASSERT_TRUE(
      CreateFileWithAppReparsePoint(TestPath(L"set_tag_mismatch.dat"), &h));

  auto set_diff_tag = SetCustomReparsePoint(h.get(), kAppTag2, kAppGuid1,
                                            kAppPayload2, sizeof(kAppPayload2));
  EXPECT_FALSE(set_diff_tag);
  EXPECT_WIN32_ERROR_CODE(set_diff_tag.error, ERROR_REPARSE_TAG_MISMATCH);

  EXPECT_TRUE(CheckCustomReparsePoint(h.get(), kAppTag1, kAppGuid1,
                                      kAppPayload1, sizeof(kAppPayload1)));
}

// Spec: [MS-FSA] FSCTL_SET_REPARSE_POINT — overwriting an existing
// non-Microsoft reparse point with the stored tag but a different GUID fails
// STATUS_REPARSE_ATTRIBUTE_CONFLICT (ERROR_REPARSE_ATTRIBUTE_CONFLICT) and
// leaves the stored reparse point intact. Enforced from Windows 10 1809 /
// Server 2019 (see SetReparsePointWithMismatchedTagFails).
TEST_F(LinksReparseTest, SetReparsePointWithMismatchedGuidFails) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  core::UniqueHandle h;
  ASSERT_TRUE(
      CreateFileWithAppReparsePoint(TestPath(L"set_guid_mismatch.dat"), &h));

  auto set_diff_guid = SetCustomReparsePoint(
      h.get(), kAppTag1, kAppGuid2, kAppPayload2, sizeof(kAppPayload2));
  EXPECT_FALSE(set_diff_guid);
  EXPECT_WIN32_ERROR_CODE(set_diff_guid.error,
                          ERROR_REPARSE_ATTRIBUTE_CONFLICT);

  EXPECT_TRUE(CheckCustomReparsePoint(h.get(), kAppTag1, kAppGuid1,
                                      kAppPayload1, sizeof(kAppPayload1)));
}

// Spec: [MS-FSA] FSCTL_SET_REPARSE_POINT & FSCTL_DELETE_REPARSE_POINT — a
// request carrying the stored (tag, GUID) replaces the reparse payload in
// place, and a matching delete removes the reparse point.
TEST_F(LinksReparseTest, ReparsePointMatchingTagAndGuidUpdatesAndDeletes) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  core::UniqueHandle h;
  ASSERT_TRUE(
      CreateFileWithAppReparsePoint(TestPath(L"match_reparse.dat"), &h));

  auto set_match = SetCustomReparsePoint(h.get(), kAppTag1, kAppGuid1,
                                         kAppPayload2, sizeof(kAppPayload2));
  ASSERT_TRUE(set_match) << core::FormatWin32Error(set_match.error);
  EXPECT_TRUE(CheckCustomReparsePoint(h.get(), kAppTag1, kAppGuid1,
                                      kAppPayload2, sizeof(kAppPayload2)));

  auto del_match = core::DeleteReparsePoint(h.get(), kAppTag1, &kAppGuid1);
  ASSERT_TRUE(del_match) << core::FormatWin32Error(del_match.error);
  EXPECT_TRUE(CheckNoReparsePoint(h.get()));
}

// Spec: Win32 CreateSymbolicLinkW — without
// SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE, creating a symbolic link from
// a security context that lacks SeCreateSymbolicLinkPrivilege fails
// ERROR_PRIVILEGE_NOT_HELD and leaves no directory entry behind. The flag is
// deliberately omitted so Windows Developer Mode cannot waive the privilege.
// The check runs above the file system, so the outcome is expected to be the
// same on every file system that could otherwise store the link.
TEST_F(LinksReparseTest, SymlinkCreateWithoutPrivilegeFailsPrivilegeNotHeld) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);

  const std::wstring target = TestPath(L"nopriv_target.dat");
  const std::wstring link = TestPath(L"nopriv_link.dat");
  ASSERT_TRUE(core::WriteFileContents(target, "nopriv-target"));

  auto token = core::CreateTokenWithoutPrivilege(SE_CREATE_SYMBOLIC_LINK_NAME);
  ASSERT_TRUE(token) << core::FormatWin32Error(token.error);

  bool created = false;
  DWORD create_error = ERROR_SUCCESS;
  {
    core::ScopedImpersonation imp(token.value->get());
    ASSERT_TRUE(imp) << core::FormatWin32Error(imp.error());
    ::SetLastError(ERROR_SUCCESS);
    created = ::CreateSymbolicLinkW(link.c_str(), target.c_str(),
                                    nt::kSymbolicLinkFlagFile) != 0;
    create_error = ::GetLastError();
  }

  EXPECT_FALSE(created);
  EXPECT_WIN32_ERROR_CODE(create_error, ERROR_PRIVILEGE_NOT_HELD);
  EXPECT_TRUE(helpers::CheckFileNotFound(link));
  EXPECT_TRUE(helpers::CheckFileContents(target, "nopriv-target"));
}

// Spec: Win32 Symbolic Links — a file symbolic link (created without
// SYMBOLIC_LINK_FLAG_DIRECTORY) whose target is a directory is itself a
// non-directory reparse point; a data open through it resolves to the
// directory and fails ERROR_ACCESS_DENIED (STATUS_FILE_IS_A_DIRECTORY).
TEST_F(LinksReparseTest, FileSymlinkToDirectoryFailsDataOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target_dir = TestPath(L"mismatch_target_dir");
  const std::wstring link = TestPath(L"mismatch_file_link");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));

  auto created =
      core::CreateSymlink(link, target_dir, /*is_directory=*/false, Os().build);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  auto link_attrs = core::QueryWin32Attributes(link);
  ASSERT_TRUE(link_attrs) << core::FormatWin32Error(link_attrs.error);
  EXPECT_NE(*link_attrs.value & FILE_ATTRIBUTE_REPARSE_POINT, DWORD{0});
  EXPECT_EQ(*link_attrs.value & FILE_ATTRIBUTE_DIRECTORY, DWORD{0});

  auto opened = core::CreateWin32File(link, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(opened);
  EXPECT_WIN32_ERROR_CODE(opened.error, ERROR_ACCESS_DENIED);

  // The link is a file: DeleteFileW removes it and leaves the target directory.
  ASSERT_WIN32_TRUE(::DeleteFileW(link.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(link));
  auto target_attrs = core::QueryWin32Attributes(target_dir);
  ASSERT_TRUE(target_attrs) << core::FormatWin32Error(target_attrs.error);
  EXPECT_NE(*target_attrs.value & FILE_ATTRIBUTE_DIRECTORY, DWORD{0});
}

// Spec: Win32 Symbolic Links — a directory symbolic link (created with
// SYMBOLIC_LINK_FLAG_DIRECTORY) whose target is a file is itself a directory
// reparse point; a directory open (FILE_DIRECTORY_FILE) through it resolves to
// the file and fails STATUS_NOT_A_DIRECTORY.
TEST_F(LinksReparseTest, DirectorySymlinkToFileFailsDirectoryOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_SYMLINK_CREATION();

  const std::wstring target = TestPath(L"mismatch_target.dat");
  const std::wstring link = TestPath(L"mismatch_dir_link");
  ASSERT_TRUE(core::WriteFileContents(target, "mismatch-target"));

  auto created =
      core::CreateSymlink(link, target, /*is_directory=*/true, Os().build);
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);

  auto link_attrs = core::QueryWin32Attributes(link);
  ASSERT_TRUE(link_attrs) << core::FormatWin32Error(link_attrs.error);
  EXPECT_NE(*link_attrs.value & FILE_ATTRIBUTE_REPARSE_POINT, DWORD{0});
  EXPECT_NE(*link_attrs.value & FILE_ATTRIBUTE_DIRECTORY, DWORD{0});

  auto opened = core::NtCreateFilePath(
      TestNtPath(L"mismatch_dir_link"), FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0,
      core::kShareAll, nt::kFileOpen, nt::kFileDirectoryFile);
  EXPECT_NT_STATUS(opened.status, STATUS_NOT_A_DIRECTORY);

  // The link is a directory: RemoveDirectoryW removes it and leaves the target.
  opened.value.reset();
  ASSERT_WIN32_TRUE(::RemoveDirectoryW(link.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(link));
  EXPECT_TRUE(helpers::CheckFileContents(target, "mismatch-target"));
}

// Spec: [MS-FSCC] & FSCTL_SET_REPARSE_POINT — a reparse buffer of exactly
// 16 KiB (MAXIMUM_REPARSE_DATA_BUFFER_SIZE = 16384 bytes including the
// REPARSE_GUID_DATA_BUFFER header) is accepted and round-trips byte-for-byte.
TEST_F(LinksReparseTest, ReparseDataAtMaximumSizeRoundTrips) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const size_t max_payload =
      static_cast<size_t>(MAXIMUM_REPARSE_DATA_BUFFER_SIZE) -
      nt::kReparseGuidDataBufferHeaderSize;
  std::vector<uint8_t> payload(max_payload, 0);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
  }

  core::UniqueHandle h;
  ASSERT_TRUE(CreateFileForReparse(TestPath(L"max_reparse.dat"), &h));

  auto set = SetCustomReparsePoint(h.get(), kAppTag1, kAppGuid1, payload.data(),
                                   payload.size());
  ASSERT_TRUE(set) << core::FormatWin32Error(set.error);
  EXPECT_TRUE(CheckCustomReparsePoint(h.get(), kAppTag1, kAppGuid1,
                                      payload.data(), payload.size()));
}

// Spec: [MS-FSCC] & FSCTL_SET_REPARSE_POINT — a reparse buffer one byte
// larger than MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 KiB) is rejected with
// ERROR_INVALID_REPARSE_DATA (STATUS_IO_REPARSE_DATA_INVALID) without setting a
// reparse point.
TEST_F(LinksReparseTest, ReparseDataOverMaximumSizeFailsInvalidReparseData) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const size_t oversize_payload =
      static_cast<size_t>(MAXIMUM_REPARSE_DATA_BUFFER_SIZE) -
      nt::kReparseGuidDataBufferHeaderSize + 1;
  const std::vector<uint8_t> payload(oversize_payload, 0x5A);

  core::UniqueHandle h;
  ASSERT_TRUE(CreateFileForReparse(TestPath(L"oversize_reparse.dat"), &h));

  auto set = SetCustomReparsePoint(h.get(), kAppTag1, kAppGuid1, payload.data(),
                                   payload.size());
  EXPECT_FALSE(set);
  EXPECT_WIN32_ERROR_CODE(set.error, ERROR_INVALID_REPARSE_DATA);
  EXPECT_TRUE(CheckNoReparsePoint(h.get()));
}

void ForceLink_LinksReparse() {}

} // namespace fstest
