// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"
#include "streams_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fstest {

namespace {

::testing::AssertionResult CreateNewEmpty(std::wstring_view path,
                                          DWORD attrs = FILE_ATTRIBUTE_NORMAL) {
  auto h = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_NEW, attrs);
  if (!h) {
    return ::testing::AssertionFailure()
           << "CreateWin32File('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(h.error);
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckCreateNewExists(std::wstring_view path) {
  auto dup = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL);
  if (dup) {
    return ::testing::AssertionFailure()
           << "Expected CREATE_NEW on '" << core::WideToUtf8(path)
           << "' to fail ERROR_FILE_EXISTS, but it succeeded";
  }
  if (dup.error != static_cast<DWORD>(ERROR_FILE_EXISTS)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_FILE_EXISTS for '" << core::WideToUtf8(path)
           << "', got " << core::FormatWin32Error(dup.error);
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckSameFileIndex(std::wstring_view path_a,
                                              std::wstring_view path_b,
                                              bool a_is_dir = false) {
  auto info_a = core::QueryInfoByPath(path_a, /*backup_semantics=*/a_is_dir);
  if (!info_a) {
    return ::testing::AssertionFailure()
           << "QueryInfoByPath('" << core::WideToUtf8(path_a)
           << "') failed: " << core::FormatWin32Error(info_a.error);
  }
  auto info_b = core::QueryInfoByPath(path_b);
  if (!info_b) {
    return ::testing::AssertionFailure()
           << "QueryInfoByPath('" << core::WideToUtf8(path_b)
           << "') failed: " << core::FormatWin32Error(info_b.error);
  }
  const uint64_t idx_a = core::FileIndexOf(*info_a.value);
  const uint64_t idx_b = core::FileIndexOf(*info_b.value);
  if (idx_a != idx_b) {
    return ::testing::AssertionFailure()
           << "FileIndex mismatch between '" << core::WideToUtf8(path_a)
           << "' (" << idx_a << ") and '" << core::WideToUtf8(path_b) << "' ("
           << idx_b << ")";
  }
  return ::testing::AssertionSuccess();
}

// Verifies the per-file (MFT record) identity that every stream of one file
// shares: attributes, FileIndex, volume serial number, and link count.
// Timestamps are deliberately excluded: tests that cover timestamp sharing
// first pin them to a known value so the comparison is deterministic, and
// LastAccessTime is never compared because antivirus or indexer activity may
// bump it between the two queries.
::testing::AssertionResult
CheckSharedFileIdentity(std::wstring_view base, std::wstring_view stream,
                        BY_HANDLE_FILE_INFORMATION *base_out = nullptr) {
  auto base_info = core::QueryInfoByPath(base);
  if (!base_info) {
    return ::testing::AssertionFailure()
           << "QueryInfoByPath(base) failed: "
           << core::FormatWin32Error(base_info.error);
  }
  auto stream_info = core::QueryInfoByPath(stream);
  if (!stream_info) {
    return ::testing::AssertionFailure()
           << "QueryInfoByPath(stream) failed: "
           << core::FormatWin32Error(stream_info.error);
  }
  const BY_HANDLE_FILE_INFORMATION &a = *base_info.value;
  const BY_HANDLE_FILE_INFORMATION &b = *stream_info.value;
  if (base_out != nullptr) {
    *base_out = a;
  }
  std::ostringstream mismatches;
  if (MaskPolicyAttributes(a.dwFileAttributes) !=
      MaskPolicyAttributes(b.dwFileAttributes)) {
    mismatches << "\n  attributes: base=0x" << std::hex << a.dwFileAttributes
               << " stream=0x" << b.dwFileAttributes << std::dec;
  }
  if (core::FileIndexOf(a) != core::FileIndexOf(b)) {
    mismatches << "\n  FileIndex: base=" << core::FileIndexOf(a)
               << " stream=" << core::FileIndexOf(b);
  }
  if (a.dwVolumeSerialNumber != b.dwVolumeSerialNumber) {
    mismatches << "\n  volume serial: base=" << a.dwVolumeSerialNumber
               << " stream=" << b.dwVolumeSerialNumber;
  }
  if (a.nNumberOfLinks != b.nNumberOfLinks) {
    mismatches << "\n  link count: base=" << a.nNumberOfLinks
               << " stream=" << b.nNumberOfLinks;
  }
  const std::string text = mismatches.str();
  if (!text.empty()) {
    return ::testing::AssertionFailure()
           << "Base file and stream disagree on shared file metadata:" << text;
  }
  return ::testing::AssertionSuccess();
}

} // namespace

// ---------------------------------------------------------------------------
// Domain 8: Streams — Creation, Metadata Sync, Enumeration & Syntax
// ---------------------------------------------------------------------------

// Spec: [MS-FSA] 2.1.5.1.1 & Win32 File Streams — creating a named ADS on a
// missing file implicitly creates the base file, and both share one FileIndex.
TEST_F(StreamsTest, NamedStreamCreateImplicitlyCreatesBaseFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"implicit_a.dat");
  const std::wstring foo = base + L":foo";
  const std::wstring foo_data = base + L":foo:$DATA";

  ASSERT_TRUE(CreateNewEmpty(foo));
  for (const std::wstring &alias : {base, foo, foo_data}) {
    EXPECT_TRUE(CheckCreateNewExists(alias));
  }
  EXPECT_TRUE(CheckSameFileIndex(base, foo));

  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
  EXPECT_WIN32_ERROR(::DeleteFileW(foo.c_str()), ERROR_FILE_NOT_FOUND);
  EXPECT_WIN32_ERROR(::DeleteFileW(base.c_str()), ERROR_FILE_NOT_FOUND);
}

// Spec: [MS-FSA] 2.1.5.1.1 & Win32 File Streams — "f::$DATA" names the
// default data stream of f, so CREATE_NEW on it fails once f exists.
TEST_F(StreamsTest, DefaultDataStreamAliasCreateNewFailsFileExists) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"implicit_alias.dat");
  ASSERT_TRUE(CreateNewEmpty(base + L":foo"));
  EXPECT_TRUE(CheckCreateNewExists(base + L"::$DATA"));
  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.1 & Win32 File Streams — creating a stream through
// the explicit ":foo:$DATA" type syntax shares FileIndex with the base file.
TEST_F(StreamsTest, ExplicitDataTypeStreamCreateSharesFileIndex) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"implicit_b.dat");
  ASSERT_TRUE(CreateNewEmpty(base + L":foo:$DATA"));
  EXPECT_TRUE(CheckSameFileIndex(base, base + L":foo"));
  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.1 & Win32 CreateFileW — CREATE_ALWAYS on a named
// stream reports ERROR_SUCCESS when created and ERROR_ALREADY_EXISTS when
// overwriting.
TEST_F(StreamsTest, NamedStreamCreateAlwaysReportsAlreadyExists) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"implicit_c.dat");
  const std::wstring foo_data = base + L":foo:$DATA";
  {
    auto h = core::CreateWin32FileEx(foo_data, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);
    EXPECT_WIN32_ERROR_CODE(h.last_error, ERROR_SUCCESS);
  }
  {
    auto h = core::CreateWin32FileEx(foo_data, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);
    EXPECT_WIN32_ERROR_CODE(h.last_error, ERROR_ALREADY_EXISTS);
  }
  EXPECT_TRUE(CheckSameFileIndex(base, base + L":foo"));
  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.3 & Win32 File Streams — creating and deleting a
// named ADS on an existing file leaves the base file and FileIndex intact.
TEST_F(StreamsTest, FileStreamCreateOnExistingAndDeleteStreamOnly) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"existing_ads.dat");
  const std::wstring foo = base + L":foo";

  ASSERT_TRUE(CreateNewEmpty(base));
  EXPECT_TRUE(CheckCreateNewExists(base));
  ASSERT_TRUE(CreateNewEmpty(foo));
  EXPECT_TRUE(CheckCreateNewExists(foo));

  auto base_info = core::QueryInfoByPath(base);
  ASSERT_WIN32_OK(base_info);
  EXPECT_TRUE(CheckSameFileIndex(base, foo));

  EXPECT_WIN32_TRUE(::DeleteFileW(foo.c_str()));
  EXPECT_WIN32_ERROR(::DeleteFileW(foo.c_str()), ERROR_FILE_NOT_FOUND);

  // Base file must survive stream deletion.
  auto still_there = core::QueryInfoByPath(base);
  ASSERT_WIN32_OK(still_there);
  EXPECT_EQ(core::FileIndexOf(*still_there.value),
            core::FileIndexOf(*base_info.value));

  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
  EXPECT_WIN32_ERROR(::DeleteFileW(base.c_str()), ERROR_FILE_NOT_FOUND);
}

// Spec: [MS-FSA] 2.1.5.1.1 & Win32 File Streams — attributes passed when a
// named ADS implicitly creates its file are file-level (visible through the
// base file), and attributes passed to OPEN_EXISTING are ignored.
TEST_F(StreamsTest, StreamCreateAttributesApplyToBaseFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"meta_create.dat");
  const std::wstring foo = base + L":foo";

  ASSERT_TRUE(CreateNewEmpty(foo, FILE_ATTRIBUTE_HIDDEN));

  BY_HANDLE_FILE_INFORMATION base_info{};
  ASSERT_TRUE(CheckSharedFileIdentity(base, foo, &base_info));
  EXPECT_NE(base_info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN, DWORD{0});

  // OPEN_EXISTING ignores FILE_ATTRIBUTE_SYSTEM.
  {
    auto h = core::CreateWin32File(foo, GENERIC_WRITE, 0, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_SYSTEM);
    ASSERT_WIN32_OK(h);
  }
  ASSERT_TRUE(CheckSharedFileIdentity(base, foo, &base_info));
  EXPECT_EQ(base_info.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM, DWORD{0});
  EXPECT_NE(base_info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN, DWORD{0});

  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.2 & Win32 File Streams — file attributes are stored
// once per file, so SetFileAttributesW on the base file is observed through
// an alternate data stream.
TEST_F(StreamsTest, BaseAttributeChangeVisibleThroughStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"meta_attr_sync.dat");
  const std::wstring foo = base + L":foo";

  ASSERT_TRUE(CreateNewEmpty(base));
  ASSERT_TRUE(CreateNewEmpty(foo));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(base.c_str(), FILE_ATTRIBUTE_HIDDEN));
  {
    auto stream_info = core::QueryInfoByPath(foo);
    ASSERT_WIN32_OK(stream_info);
    EXPECT_NE(stream_info.value->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN,
              DWORD{0});
  }
  EXPECT_TRUE(CheckSharedFileIdentity(base, foo));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(base.c_str(), FILE_ATTRIBUTE_NORMAL));
  {
    auto stream_info = core::QueryInfoByPath(foo);
    ASSERT_WIN32_OK(stream_info);
    EXPECT_EQ(stream_info.value->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN,
              DWORD{0});
  }
  EXPECT_TRUE(CheckSharedFileIdentity(base, foo));

  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.2 & Win32 File Streams — timestamps are stored once
// per file, so SetFileTime on the base file is observed through an alternate
// data stream. CreationTime and LastWriteTime are pinned to distinct future
// values so the comparison cannot pass by clock coincidence; LastAccessTime
// is not compared (host antivirus/indexer activity may update it).
TEST_F(StreamsTest, BaseTimestampChangeVisibleThroughStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"meta_time_sync.dat");
  const std::wstring foo = base + L":foo";

  ASSERT_TRUE(CreateNewEmpty(base));
  ASSERT_TRUE(CreateNewEmpty(foo));

  const FILETIME ft_create = core::MakeUtcFileTime(2134, 1, 1, 0, 0, 0);
  const FILETIME ft_write = core::MakeUtcFileTime(2135, 10, 11, 12, 34, 56);
  {
    auto h = core::CreateWin32File(base, FILE_WRITE_ATTRIBUTES, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);
    ASSERT_WIN32_TRUE(
        ::SetFileTime(h.value->get(), &ft_create, nullptr, &ft_write));
  }

  auto base_info = core::QueryInfoByPath(base);
  ASSERT_WIN32_OK(base_info);
  EXPECT_EQ(core::FileTimeToU64(base_info.value->ftCreationTime),
            core::FileTimeToU64(ft_create));
  EXPECT_EQ(core::FileTimeToU64(base_info.value->ftLastWriteTime),
            core::FileTimeToU64(ft_write));

  auto stream_info = core::QueryInfoByPath(foo);
  ASSERT_WIN32_OK(stream_info);
  EXPECT_EQ(core::FileTimeToU64(stream_info.value->ftCreationTime),
            core::FileTimeToU64(ft_create));
  EXPECT_EQ(core::FileTimeToU64(stream_info.value->ftLastWriteTime),
            core::FileTimeToU64(ft_write));

  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1 & Win32 File Streams — opening non-$DATA internal
// NTFS attribute types such as :$EA via stream syntax fails access denied.
TEST_F(StreamsTest, FileStreamInternalAttributeTypesRejected) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"internal_attr.dat");
  ASSERT_TRUE(CreateNewEmpty(base + L":foo"));

  for (const std::wstring &bad : {base + L":foo:$EA", base + L":bar:$EA"}) {
    auto res = core::CreateWin32File(bad, GENERIC_WRITE, 0, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(res);
    EXPECT_WIN32_ERROR_CODE(res.error, ERROR_ACCESS_DENIED);
  }

  EXPECT_WIN32_TRUE(::DeleteFileW(base.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1 & Win32 File Streams — directories accept named
// :$DATA streams sharing FileIndex and remove cleanly with attached streams.
TEST_F(StreamsTest, DirectoryStreamCreateAndDelete) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring dir = TestPath(L"dir_ads");
  const std::wstring dir_foo = dir + L":foo";

  EXPECT_WIN32_ERROR(::CreateDirectoryW(dir_foo.c_str(), nullptr),
                     ERROR_DIRECTORY);
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  EXPECT_WIN32_ERROR(::CreateDirectoryW(dir.c_str(), nullptr),
                     ERROR_ALREADY_EXISTS);

  ASSERT_TRUE(CreateNewEmpty(dir_foo));
  EXPECT_TRUE(CheckCreateNewExists(dir_foo));
  EXPECT_TRUE(CheckSameFileIndex(dir, dir_foo, /*a_is_dir=*/true));

  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir_foo.c_str()), ERROR_DIRECTORY);
  EXPECT_WIN32_TRUE(::DeleteFileW(dir_foo.c_str()));
  EXPECT_WIN32_ERROR(::DeleteFileW(dir_foo.c_str()), ERROR_FILE_NOT_FOUND);
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir.c_str()), ERROR_FILE_NOT_FOUND);

  // Removing a directory succeeds even while a named data stream is attached.
  const std::wstring dir2 = TestPath(L"dir_ads_remove");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir2.c_str(), nullptr));
  ASSERT_TRUE(CreateNewEmpty(dir2 + L":foo:$DATA"));
  EXPECT_TRUE(CheckSameFileIndex(dir2, dir2 + L":foo", /*a_is_dir=*/true));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir2.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1 & Win32 File Streams — directory paths alias
// ::$INDEX_ALLOCATION and :$I30:$INDEX_ALLOCATION identically on create.
TEST_F(StreamsTest, DirectoryDefaultIndexAllocationAliases) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  constexpr std::array<std::wstring_view, 3> kAliases = {
      L"", L"::$INDEX_ALLOCATION", L":$I30:$INDEX_ALLOCATION"};

  for (size_t i = 0; i < kAliases.size(); ++i) {
    const std::wstring dir = TestPath(L"idx_alias_" + std::to_wstring(i + 1));
    const std::wstring created = dir + std::wstring(kAliases[i]);
    ASSERT_WIN32_TRUE(::CreateDirectoryW(created.c_str(), nullptr));
    for (size_t j = 0; j < kAliases.size(); ++j) {
      if (j == i) {
        continue;
      }
      const std::wstring dup = dir + std::wstring(kAliases[j]);
      EXPECT_WIN32_ERROR(::CreateDirectoryW(dup.c_str(), nullptr),
                         ERROR_ALREADY_EXISTS);
    }
    EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
  }
}

// Spec: Win32 FindFirstStreamW / FindNextStreamW — enumerating a base file
// returns the exact multiset of ::$DATA and named :$DATA streams.
TEST_F(StreamsTest, FindStreamsOnBaseFileStrictMultiset) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  // Plain file reports strictly {"::$DATA"}.
  const std::wstring f1 = TestPath(L"find_file_1.dat");
  ASSERT_TRUE(CreateNewEmpty(f1));
  EXPECT_TRUE(streams_internal::CheckStreamNames(f1, {L"::$DATA"}));
  ASSERT_WIN32_TRUE(::DeleteFileW(f1.c_str()));
  EXPECT_TRUE(streams_internal::CheckStreamsMissing(f1));

  // File with :foo and :bar (short syntax and explicit :$DATA suffix syntax).
  constexpr std::array<std::wstring_view, 2> kSuffixes = {L"", L":$DATA"};
  for (size_t idx = 0; idx < kSuffixes.size(); ++idx) {
    const std::wstring f =
        TestPath(L"find_file_" + std::to_wstring(idx + 2) + L".dat");
    const std::wstring type_sfx(kSuffixes[idx]);
    ASSERT_TRUE(CreateNewEmpty(f + L":foo" + type_sfx));
    ASSERT_TRUE(CreateNewEmpty(f + L":bar" + type_sfx));
    EXPECT_TRUE(streams_internal::CheckStreamNames(
        f, {L"::$DATA", L":foo:$DATA", L":bar:$DATA"}));

    ASSERT_WIN32_TRUE(::DeleteFileW(f.c_str()));
    EXPECT_TRUE(streams_internal::CheckStreamsMissing(f));
  }
}

// Spec: Win32 FindFirstStreamW — passing a stream path ("f:bar") enumerates
// every stream of the owning file f, not just the named stream.
TEST_F(StreamsTest, FindStreamsOnStreamPathEnumeratesAllStreams) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring f = TestPath(L"find_stream_path.dat");
  ASSERT_TRUE(CreateNewEmpty(f + L":foo"));
  ASSERT_TRUE(CreateNewEmpty(f + L":bar"));
  ASSERT_WIN32_TRUE(::DeleteFileW((f + L":foo").c_str()));

  EXPECT_TRUE(streams_internal::CheckStreamNames(f + L":bar",
                                                 {L"::$DATA", L":bar:$DATA"}));

  ASSERT_WIN32_TRUE(::DeleteFileW(f.c_str()));
  EXPECT_TRUE(streams_internal::CheckStreamsMissing(f));
}

// Spec: Win32 FindFirstStreamW — a plain directory returns ERROR_HANDLE_EOF,
// while directories with ADS enumerate only named :$DATA streams.
TEST_F(StreamsTest, FindStreamsOnDirectoryStrictMultiset) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  // Plain directory returns ERROR_HANDLE_EOF directly from FindFirstStreamW.
  const std::wstring d1 = TestPath(L"find_dir_1");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(d1.c_str(), nullptr));
  {
    WIN32_FIND_STREAM_DATA data{};
    ::SetLastError(ERROR_SUCCESS);
    core::UniqueFindHandle find(
        ::FindFirstStreamW(d1.c_str(), FindStreamInfoStandard, &data, 0));
    const DWORD err = ::GetLastError();
    EXPECT_FALSE(find.valid());
    EXPECT_WIN32_ERROR_CODE(err, ERROR_HANDLE_EOF);
  }
  ASSERT_WIN32_TRUE(::RemoveDirectoryW(d1.c_str()));
  EXPECT_TRUE(streams_internal::CheckStreamsMissing(d1));

  // Directories with :foo and :bar (short and explicit :$DATA syntax) have NO
  // ::$DATA entry.
  constexpr std::array<std::wstring_view, 2> kSuffixes = {L"", L":$DATA"};
  for (size_t idx = 0; idx < kSuffixes.size(); ++idx) {
    const std::wstring d = TestPath(L"find_dir_" + std::to_wstring(idx + 2));
    const std::wstring type_sfx(kSuffixes[idx]);
    ASSERT_WIN32_TRUE(::CreateDirectoryW(d.c_str(), nullptr));
    ASSERT_TRUE(CreateNewEmpty(d + L":foo" + type_sfx));
    ASSERT_TRUE(CreateNewEmpty(d + L":bar" + type_sfx));
    EXPECT_TRUE(
        streams_internal::CheckStreamNames(d, {L":foo:$DATA", L":bar:$DATA"}));
    ASSERT_WIN32_TRUE(::RemoveDirectoryW(d.c_str()));
    EXPECT_TRUE(streams_internal::CheckStreamsMissing(d));
  }
}

// Spec: Win32 FindFirstStreamW — stream enumeration scales past 100 named
// alternate streams without drops or duplicate entries.
TEST_F(StreamsTest, FindStreamsHundredStreamsStrictMultiset) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"hundred_streams.dat");
  std::vector<std::wstring> expected;
  expected.reserve(101);
  expected.push_back(L"::$DATA");

  for (int i = 0; i < 100; ++i) {
    const std::wstring suffix = L":strm" + std::to_wstring(i);
    ASSERT_TRUE(CreateNewEmpty(base + suffix)) << "stream index " << i;
    expected.push_back(suffix + L":$DATA");
  }

  EXPECT_TRUE(streams_internal::CheckStreamNames(base, expected));

  ASSERT_WIN32_TRUE(::DeleteFileW(base.c_str()));
  EXPECT_TRUE(streams_internal::CheckStreamsMissing(base));
}

// Spec: [MS-FSA] 2.1.5.12.29 FileStreamInformation — small buffers return
// STATUS_BUFFER_OVERFLOW; plain directories return STATUS_SUCCESS (0 bytes).
TEST_F(StreamsTest, NtQueryFileStreamInformationSmallBufferAndDir) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring base = TestPath(L"nt_stream_info.dat");
  const std::string main_data(17, 'P');
  const std::string alpha_data(43, 'Q');
  ASSERT_WIN32_OK(core::WriteFileContents(base, main_data));
  ASSERT_WIN32_OK(core::WriteFileContents(base + L":alpha", alpha_data));

  auto file_h = core::CreateWin32File(base, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                      core::kShareAll, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(file_h);

  // Buffer large enough for one entry (::$DATA = 40 bytes aligned) but too
  // small for both streams (::$DATA + :alpha:$DATA = 88 bytes) deterministically
  // returns STATUS_BUFFER_OVERFLOW with the first complete entry populated.
  std::vector<uint8_t> small_buf(48, 0);
  nt::IoStatusBlock iosb{};
  const NTSTATUS small_st =
      ntdll.NtQueryInformationFile(file_h.value->get(), &iosb, small_buf.data(),
                                   static_cast<ULONG>(small_buf.size()),
                                   nt::FileInfoClass::FileStreamInformation);
  EXPECT_NT_STATUS(small_st, STATUS_BUFFER_OVERFLOW);
  ASSERT_GE(static_cast<size_t>(iosb.information),
            offsetof(nt::FileStreamInfo, stream_name));
  ASSERT_LE(static_cast<size_t>(iosb.information), small_buf.size());
  ULONG next_entry_offset = 0;
  std::memcpy(&next_entry_offset,
              small_buf.data() +
                  offsetof(nt::FileStreamInfo, next_entry_offset),
              sizeof(next_entry_offset));
  EXPECT_EQ(next_entry_offset, ULONG{0});
  const auto partial_entries =
      core::ParseStreamInfoBuffer(std::span<const uint8_t>(
          small_buf.data(), static_cast<size_t>(iosb.information)));
  ASSERT_EQ(partial_entries.size(), size_t{1});
  EXPECT_TRUE(partial_entries[0].name == L"::$DATA" ||
              partial_entries[0].name == L":alpha:$DATA")
      << "Unexpected partial stream name: '"
      << core::WideToUtf8(partial_entries[0].name) << "'";

  // Properly sized query returns full multiset with exact StreamSize.
  auto nt_streams = core::ListStreamsNt(file_h.value->get());
  ASSERT_NT_SUCCESS(nt_streams.status);
  ASSERT_TRUE(nt_streams.value.has_value());
  EXPECT_MULTISET_EQ(streams_internal::ExtractStreamNames(*nt_streams.value),
                     (std::vector<std::wstring>{L"::$DATA", L":alpha:$DATA"}));
  for (const auto &entry : *nt_streams.value) {
    if (entry.name == L"::$DATA") {
      EXPECT_EQ(entry.size, int64_t{17});
    } else if (entry.name == L":alpha:$DATA") {
      EXPECT_EQ(entry.size, int64_t{43});
    }
  }

  // Plain directory with no ADS returns STATUS_SUCCESS and Information == 0.
  const std::wstring dir = TestPath(L"nt_stream_empty_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  auto dir_h = core::CreateWin32File(dir, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                     core::kShareAll, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(dir_h);

  std::vector<uint8_t> dir_buf(4096, 0);
  nt::IoStatusBlock dir_iosb{};
  const NTSTATUS dir_st = ntdll.NtQueryInformationFile(
      dir_h.value->get(), &dir_iosb, dir_buf.data(),
      static_cast<ULONG>(dir_buf.size()),
      nt::FileInfoClass::FileStreamInformation);
  EXPECT_NT_STATUS(dir_st, STATUS_SUCCESS);
  EXPECT_EQ(dir_iosb.information, ULONG_PTR{0});

  auto dir_streams = core::ListStreamsNt(dir_h.value->get());
  ASSERT_NT_SUCCESS(dir_streams.status);
  ASSERT_TRUE(dir_streams.value.has_value());
  EXPECT_TRUE(dir_streams.value->empty());
}

// Spec: [MS-FSA] 2.1.5.1 & Win32 File Streams — stream names match
// case-insensitively while preserving original creation casing in queries.
TEST_F(StreamsTest, StreamCaseInsensitiveLookupAndPreservation) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"case_stream.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(base + L":MixedStream", "case-data"));

  EXPECT_TRUE(helpers::CheckFileContents(base + L":mixedstream", "case-data"));
  EXPECT_TRUE(helpers::CheckFileContents(base + L":MIXEDSTREAM", "case-data"));
  EXPECT_TRUE(streams_internal::CheckStreamNames(
      base, {L"::$DATA", L":MixedStream:$DATA"}));
}

// Spec: [MS-FSA] 2.1.5.15.3 & Win32 File Streams — deleting a named ADS
// removes only that stream, while deleting ::$DATA removes the entire file.
TEST_F(StreamsTest, DeleteDefaultStreamByExplicitName) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring f1 = TestPath(L"del_default_1.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(f1, "main-1"));
  ASSERT_WIN32_OK(core::WriteFileContents(f1 + L":a", "alt-a"));

  // Deleting :a leaves f1 intact.
  ASSERT_WIN32_TRUE(::DeleteFileW((f1 + L":a").c_str()));
  EXPECT_WIN32_ERROR(::DeleteFileW((f1 + L":a").c_str()), ERROR_FILE_NOT_FOUND);
  EXPECT_TRUE(helpers::CheckFileContents(f1, "main-1"));

  // Deleting f1::$DATA deletes the entire file and attached alternate streams.
  ASSERT_WIN32_OK(core::WriteFileContents(f1 + L":b", "alt-b"));
  ASSERT_WIN32_TRUE(::DeleteFileW((f1 + L"::$DATA").c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(f1));
  auto r1_b = core::ReadFileContents(f1 + L":b");
  EXPECT_FALSE(r1_b);
  EXPECT_WIN32_ERROR_CODE(r1_b.error, ERROR_FILE_NOT_FOUND);

  // Deleting plain f2 also deletes all attached streams.
  const std::wstring f2 = TestPath(L"del_default_2.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(f2, "main-2"));
  ASSERT_WIN32_OK(core::WriteFileContents(f2 + L":c", "alt-c"));
  ASSERT_WIN32_TRUE(::DeleteFileW(f2.c_str()));
  auto r2_c = core::ReadFileContents(f2 + L":c");
  EXPECT_FALSE(r2_c);
  EXPECT_WIN32_ERROR_CODE(r2_c.error, ERROR_FILE_NOT_FOUND);
}

// Spec: [MS-FSA] 2.1.5.1 & Win32 File Streams — malformed stream syntax
// fails ERROR_INVALID_NAME and dir::$DATA fails STATUS_FILE_IS_A_DIRECTORY.
TEST_F(StreamsTest, InvalidStreamSyntaxAndDirectoryDataStreamRejected) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"invalid_syntax.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(base, "valid"));

  for (const std::wstring &bad :
       {base + L"::", base + L":a:b", base + L":a:$BOGUS"}) {
    auto res = core::CreateWin32File(bad, GENERIC_WRITE, core::kShareAll,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(res) << "Unexpectedly succeeded on " << core::WideToUtf8(bad);
    EXPECT_WIN32_ERROR_CODE(res.error, ERROR_INVALID_NAME);
  }

  // Opening/creating default unnamed ::$DATA stream on a directory fails.
  const std::wstring dir = TestPath(L"invalid_dir_data");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto dir_data_win32 =
      core::CreateWin32File(dir + L"::$DATA", GENERIC_WRITE, core::kShareAll,
                            OPEN_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS);
  EXPECT_FALSE(dir_data_win32);
  EXPECT_WIN32_ERROR_CODE(dir_data_win32.error, ERROR_ACCESS_DENIED);

  auto dir_data_nt = core::NtCreateFilePath(
      TestNtPath(L"invalid_dir_data::$DATA"), GENERIC_WRITE | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOpenIf,
      nt::kFileSynchronousIoNonalert);
  EXPECT_FALSE(dir_data_nt);
  EXPECT_NT_STATUS(dir_data_nt.status, STATUS_FILE_IS_A_DIRECTORY);
}

// Spec: [MS-FSA] 2.1.5.1 / WDK NtCreateFile -- opening or creating an alternate
// data stream relative to an open base file handle (RootDirectory = h_base,
// ObjectName = L":rel_ads") attaches the stream to the same file.
TEST_F(StreamsTest, HandleRelativeAlternateStreamOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"rel_ads_base.dat");
  const std::string base_payload = "base-stream-content";
  const std::string rel_payload = "handle-relative-ads-payload";
  ASSERT_WIN32_OK(core::WriteFileContents(base, base_payload));

  {
    auto h_base = core::CreateWin32File(
        base, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_base);

    // Create :rel_ads relative to the open base file handle.
    {
      auto h_rel = core::NtCreateFilePath(
          L":rel_ads",
          FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
          FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileCreate,
          nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert,
          /*root_directory=*/h_base.value->get());
      ASSERT_NT_SUCCESS(h_rel.status);
      ASSERT_TRUE(h_rel.value.has_value());
      EXPECT_EQ(h_rel.information, nt::kFileCreated);

      auto wr = core::WriteAllBytes(h_rel.value->get(), rel_payload.data(),
                                    rel_payload.size());
      ASSERT_WIN32_OK(wr);

      auto base_idx = core::GetFileIndex(h_base.value->get());
      auto rel_idx = core::GetFileIndex(h_rel.value->get());
      ASSERT_WIN32_OK(base_idx);
      ASSERT_WIN32_OK(rel_idx);
      EXPECT_EQ(*base_idx.value, *rel_idx.value);
    }

    // Reopen :rel_ads relative to h_base and read back.
    {
      auto h_rel_rd = core::NtCreateFilePath(
          L":rel_ads", FILE_READ_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
          core::kShareAll, nt::kFileOpen,
          nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert,
          /*root_directory=*/h_base.value->get());
      ASSERT_NT_SUCCESS(h_rel_rd.status);
      ASSERT_TRUE(h_rel_rd.value.has_value());
      auto rd = core::ReadAllBytes(h_rel_rd.value->get());
      ASSERT_WIN32_OK(rd);
      EXPECT_EQ(*rd.value, rel_payload);
    }
  }

  // Verify accessible via full Win32 path and visible in stream enumeration.
  EXPECT_TRUE(helpers::CheckFileContents(base, base_payload));
  EXPECT_TRUE(helpers::CheckFileContents(base + L":rel_ads", rel_payload));
  EXPECT_TRUE(streams_internal::CheckStreamNames(
      base, {L"::$DATA", L":rel_ads:$DATA"}));
}

// Spec: [MS-FSCC] 2.1.5.3 Streamname — a stream name is at most 255
// characters; a 256-character name is rejected as an invalid name.
TEST_F(StreamsTest, StreamNameLengthLimit255) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  // Extended-length path: base + ':' + 255 characters exceeds MAX_PATH.
  const std::wstring base = TestExtendedPath(L"name_len.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(base, "base"));

  const std::wstring max_name = L":" + std::wstring(255, L'n');
  const std::wstring over_name = L":" + std::wstring(256, L'n');

  ASSERT_WIN32_OK(core::WriteFileContents(base + max_name, "max-name"));
  EXPECT_TRUE(helpers::CheckFileContents(base + max_name, "max-name"));
  EXPECT_TRUE(streams_internal::CheckStreamNames(
      base, {L"::$DATA", max_name + L":$DATA"}));

  auto over =
      core::CreateWin32File(base + over_name, GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(over);
  EXPECT_WIN32_ERROR_CODE(over.error, ERROR_INVALID_NAME);
  EXPECT_TRUE(streams_internal::CheckStreamNames(
      base, {L"::$DATA", max_name + L":$DATA"}));
}

// Spec: [MS-FSCC] 2.1.5.3 Streamname — unlike file names, stream names may
// contain control characters (OLE property sets rely on
// ":\x05SummaryInformation").
TEST_F(StreamsTest, StreamNameAllowsControlCharacters) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"ctrl_name.dat");
  const std::wstring stream_name =
      std::wstring(L":\x05") + L"SummaryInformation";

  ASSERT_WIN32_OK(
      core::WriteFileContents(base + stream_name, "ole-property-set"));
  EXPECT_TRUE(
      helpers::CheckFileContents(base + stream_name, "ole-property-set"));
  EXPECT_TRUE(streams_internal::CheckStreamNames(
      base, {L"::$DATA", stream_name + L":$DATA"}));
}

// Spec: Win32 CopyFileW — copying a file copies its default data stream and
// every named alternate data stream, leaving the source untouched.
TEST_F(StreamsTest, CopyFileWPreservesAlternateStreams) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring src = TestPath(L"copy_src.dat");
  const std::wstring dst = TestPath(L"copy_dst.dat");
  const std::string primary_payload = "copy-primary-payload";
  const std::string meta_payload = "copy-meta-payload";
  const std::string extra_payload(4096, 'E');

  ASSERT_WIN32_OK(core::WriteFileContents(src, primary_payload));
  ASSERT_WIN32_OK(core::WriteFileContents(src + L":meta", meta_payload));
  ASSERT_WIN32_OK(core::WriteFileContents(src + L":extra", extra_payload));

  ASSERT_WIN32_TRUE(
      ::CopyFileW(src.c_str(), dst.c_str(), /*bFailIfExists=*/TRUE));

  EXPECT_TRUE(streams_internal::CheckStreamNames(
      dst, {L"::$DATA", L":meta:$DATA", L":extra:$DATA"}));
  EXPECT_TRUE(helpers::CheckFileContents(dst, primary_payload));
  EXPECT_TRUE(helpers::CheckFileContents(dst + L":meta", meta_payload));
  EXPECT_TRUE(helpers::CheckFileContents(dst + L":extra", extra_payload));

  EXPECT_TRUE(streams_internal::CheckStreamNames(
      src, {L"::$DATA", L":meta:$DATA", L":extra:$DATA"}));
  EXPECT_TRUE(helpers::CheckFileContents(src + L":meta", meta_payload));
}

void ForceLink_Streams() {}

} // namespace fstest
