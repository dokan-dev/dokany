// SPDX-License-Identifier: MIT

#include "fstest/suite/register_suites.h"

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/os_info.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/test_fixture.h"

#include <cstdint>
#include <ios>
#include <string>

namespace fstest {
namespace {

// Note on Dokan targets: dokan/fileinfo.c DokanFillFileBasicInfo (:48-49)
// reports ChangeTime as a copy of LastWriteTime. On Dokan mirror/memfs an
// assertion that ChangeTime equals LastWriteTime, or that ChangeTime advanced
// after an operation that also advances LastWriteTime, is therefore vacuous.
// Tests that pin ChangeTime semantics below also assert that LastWriteTime did
// NOT move, so the two timestamps cannot stand in for each other.

// Backdates all 4 timestamps via fstest::BackdateAllTimes and verifies that
// CreationTime, LastWriteTime and ChangeTime persisted before the test
// proceeds. LastAccessTime is deliberately not part of the precondition: an
// antivirus scanner or indexer opening the file between the set and the query
// may legitimately advance it, which would fail unrelated tests.
::testing::AssertionResult
BackdateAndVerifyAllTimes(const std::wstring &path, int64_t ticks,
                          bool is_directory = false) {
  if (!BackdateAllTimes(path, ticks, is_directory)) {
    return ::testing::AssertionFailure()
           << "BackdateAllTimes failed on '" << core::WideToUtf8(path) << "'";
  }
  const auto got = helpers::QueryPathBasicInfo(path);
  if (!got) {
    return ::testing::AssertionFailure()
           << "QueryPathBasicInfo failed on '" << core::WideToUtf8(path)
           << "': " << got.Describe();
  }
  if (got.value->creation_time.QuadPart != ticks ||
      got.value->last_write_time.QuadPart != ticks ||
      got.value->change_time.QuadPart != ticks) {
    return ::testing::AssertionFailure()
           << "Backdated timestamps did not persist cleanly on '"
           << core::WideToUtf8(path) << "'";
  }
  return ::testing::AssertionSuccess();
}

// Host-wide NTFS last-access-time update policy.
enum class LastAccessPolicy { kUnknown, kEnabled, kDisabled };

const char *LastAccessPolicyName(LastAccessPolicy policy) noexcept {
  switch (policy) {
  case LastAccessPolicy::kEnabled:
    return "updates enabled";
  case LastAccessPolicy::kDisabled:
    return "updates disabled";
  case LastAccessPolicy::kUnknown:
    break;
  }
  return "unknown";
}

// Reads HKLM\SYSTEM\CurrentControlSet\Control\FileSystem
// \NtfsDisableLastAccessUpdate. Bit 0 set means updates are disabled in every
// encoding: the legacy 0/1 values and the Windows 10 1803+ values 0x80000000
// (user managed, enabled), 0x80000001 (user managed, disabled), 0x80000002
// (system managed, enabled) and 0x80000003 (system managed, disabled).
LastAccessPolicy QueryHostLastAccessPolicy() noexcept {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                     L"NtfsDisableLastAccessUpdate", RRF_RT_REG_DWORD, nullptr,
                     &value, &size) != ERROR_SUCCESS) {
    return LastAccessPolicy::kUnknown;
  }
  return ((value & 1u) != 0u) ? LastAccessPolicy::kDisabled
                              : LastAccessPolicy::kEnabled;
}

::testing::AssertionResult ExpectMaskedFileAttrs(const std::wstring &path,
                                                 DWORD expected) {
  auto got = core::QueryWin32Attributes(path);
  if (!got) {
    return ::testing::AssertionFailure() << "QueryWin32Attributes failed: "
                                         << core::FormatWin32Error(got.error);
  }
  const DWORD path_masked = MaskPolicyAttributes(*got.value);
  if (path_masked != expected) {
    return ::testing::AssertionFailure()
           << "Path attributes mismatch: expected 0x" << std::hex << expected
           << ", got 0x" << path_masked;
  }

  auto h = core::CreateWin32File(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 core::kShareAll, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL);
  if (!h) {
    return ::testing::AssertionFailure()
           << "CreateWin32File failed: " << core::FormatWin32Error(h.error);
  }
  auto bhfi = core::GetByHandleInfo(h.value->get());
  if (!bhfi) {
    return ::testing::AssertionFailure()
           << "GetByHandleInfo failed: " << core::FormatWin32Error(bhfi.error);
  }
  const DWORD handle_masked =
      MaskPolicyAttributes(bhfi.value->dwFileAttributes);
  if (handle_masked != expected) {
    return ::testing::AssertionFailure()
           << "Handle attributes mismatch: expected 0x" << std::hex << expected
           << ", got 0x" << handle_masked;
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult SetAndVerifyAttrBitMask(const std::wstring &path,
                                                   DWORD set_attrs,
                                                   DWORD expected_set_bits,
                                                   DWORD expected_clear_bits) {
  ::SetLastError(ERROR_SUCCESS);
  const BOOL set_ok = ::SetFileAttributesW(path.c_str(), set_attrs);
  const DWORD set_err = ::GetLastError();
  if (set_ok == FALSE) {
    return ::testing::AssertionFailure()
           << "SetFileAttributesW(0x" << std::hex << set_attrs
           << ") failed: " << core::FormatWin32Error(set_err);
  }
  auto got = core::QueryWin32Attributes(path);
  if (!got) {
    return ::testing::AssertionFailure() << "QueryWin32Attributes failed: "
                                         << core::FormatWin32Error(got.error);
  }
  if ((*got.value & expected_set_bits) != expected_set_bits ||
      (*got.value & expected_clear_bits) != 0u) {
    return ::testing::AssertionFailure()
           << "Attribute mask mismatch: got 0x" << std::hex << *got.value
           << ", expected set=0x" << expected_set_bits << ", clear=0x"
           << expected_clear_bits;
  }
  return ::testing::AssertionSuccess();
}

} // namespace

// Skips unless the host-wide NTFS last-access policy equals `wanted`.
#define REQUIRE_HOST_LAST_ACCESS_POLICY(wanted)                                \
  do {                                                                         \
    const LastAccessPolicy fstest_atime_policy_ = QueryHostLastAccessPolicy(); \
    if (fstest_atime_policy_ != (wanted)) {                                    \
      this->RecordSkipCategory("os_policy");                                   \
      GTEST_SKIP() << "Requires host NtfsDisableLastAccessUpdate policy '"     \
                   << LastAccessPolicyName(wanted) << "' (host policy: '"      \
                   << LastAccessPolicyName(fstest_atime_policy_) << "').";     \
    }                                                                          \
  } while (false)

// Spec: Win32 SetFileTime / [MS-FSA] 2.1.5.15.2 -- explicit FILETIME values
// persist across close/reopen while nullptr parameters preserve timestamps.
TEST_F(TimestampsAttrsTest, SetFileTimeExplicitAndNullPreservesUnchanged) {
  const std::wstring path = TestPath(L"set_file_time.dat");

  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

  auto initial_info = core::GetByHandleInfo(h.value->get());
  ASSERT_TRUE(initial_info) << core::FormatWin32Error(initial_info.error);
  const uint64_t orig_write =
      helpers::FileTimeToU64(initial_info.value->ftLastWriteTime);

  const FILETIME ft_create = core::MakeUtcFileTime(2134, 1, 1, 0, 0, 0);
  const FILETIME ft_access = core::MakeUtcFileTime(2134, 10, 11, 12, 34, 56);
  const uint64_t target_create = helpers::FileTimeToU64(ft_create);
  const uint64_t target_access = helpers::FileTimeToU64(ft_access);
  ASSERT_GT(target_create, 0u);
  ASSERT_GT(target_access, 0u);

  ASSERT_WIN32_TRUE(::SetFileTime(h.value->get(), &ft_create, &ft_access,
                                  /*lpLastWriteTime=*/nullptr));

  // LastAccessTime is verified on the handle that set it: once that handle is
  // closed, an antivirus scanner or indexer may open the file and legitimately
  // advance LastAccessTime before a reopen could observe the explicit value.
  auto set_handle_info = core::GetByHandleInfo(h.value->get());
  ASSERT_TRUE(set_handle_info) << core::FormatWin32Error(set_handle_info.error);
  EXPECT_EQ(helpers::FileTimeToU64(set_handle_info.value->ftLastAccessTime),
            target_access);
  h.value->reset();

  // Reopen to verify CreationTime and LastWriteTime persisted cleanly.
  auto h_verify = core::CreateWin32File(
      path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_verify) << core::FormatWin32Error(h_verify.error);

  auto after_info = core::GetByHandleInfo(h_verify.value->get());
  ASSERT_TRUE(after_info) << core::FormatWin32Error(after_info.error);
  EXPECT_EQ(helpers::FileTimeToU64(after_info.value->ftCreationTime),
            target_create);
  EXPECT_EQ(helpers::FileTimeToU64(after_info.value->ftLastWriteTime),
            orig_write);

  h_verify.value->reset();
  ASSERT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- SetFileAttributesW cleanly
// transitions READONLY, SYSTEM, HIDDEN, ARCHIVE, and NORMAL on regular files.
TEST_F(TimestampsAttrsTest, SetGetFileAttributesTransitionMatrix) {
  const std::wstring path = TestPath(L"attr_matrix.dat");

  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }

  ASSERT_TRUE(ExpectMaskedFileAttrs(path, FILE_ATTRIBUTE_ARCHIVE));

  for (DWORD attr : {static_cast<DWORD>(FILE_ATTRIBUTE_READONLY),
                     static_cast<DWORD>(FILE_ATTRIBUTE_SYSTEM),
                     static_cast<DWORD>(FILE_ATTRIBUTE_HIDDEN),
                     static_cast<DWORD>(FILE_ATTRIBUTE_ARCHIVE),
                     static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL)}) {
    ASSERT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), attr));
    EXPECT_TRUE(ExpectMaskedFileAttrs(path, attr));
  }

  ASSERT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- SetFileAttributesW on a
// directory always preserves FILE_ATTRIBUTE_DIRECTORY across all transitions.
TEST_F(TimestampsAttrsTest, DirectoryAttributesPreserveDirectoryBit) {
  const std::wstring dir = TestPath(L"attr_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto expect_dir_attrs = [&](DWORD expected) -> ::testing::AssertionResult {
    auto got = core::QueryWin32Attributes(dir);
    if (!got) {
      return ::testing::AssertionFailure() << "QueryWin32Attributes failed: "
                                           << core::FormatWin32Error(got.error);
    }
    const DWORD masked = MaskPolicyAttributes(*got.value);
    if (masked != expected) {
      return ::testing::AssertionFailure()
             << "Directory attributes mismatch: expected 0x" << std::hex
             << expected << ", got 0x" << masked;
    }
    return ::testing::AssertionSuccess();
  };

  EXPECT_TRUE(expect_dir_attrs(FILE_ATTRIBUTE_DIRECTORY));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_READONLY));
  EXPECT_TRUE(
      expect_dir_attrs(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_SYSTEM));
  EXPECT_TRUE(
      expect_dir_attrs(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_SYSTEM));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN));
  EXPECT_TRUE(
      expect_dir_attrs(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL));
  EXPECT_TRUE(expect_dir_attrs(FILE_ATTRIBUTE_DIRECTORY));

  ASSERT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: [MS-FSCC] 2.4.7 / [MS-FSA] 2.1.5.15.2 -- CreationTime, LastAccessTime
// and LastWriteTime set through FileBasicInformation round-trip at bit-exact
// 100-nanosecond FILETIME resolution.
TEST_F(TimestampsAttrsTest, HundredNanosecondFiletimePrecisionRoundTrip) {
  const std::wstring path = TestPath(L"precision_100ns.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "100ns_precision"));

  const int64_t t_create =
      core::MakeUtcFileTimeInt64(2021, 3, 14, 15, 9, 26) + 1234567;
  const int64_t t_access =
      core::MakeUtcFileTimeInt64(2022, 6, 28, 1, 23, 45) + 7654321;
  const int64_t t_write =
      core::MakeUtcFileTimeInt64(2023, 9, 17, 8, 42, 19) + 3141593;

  {
    auto h = core::CreateWin32File(
        path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        core::kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    nt::FileBasicInfo set_info{};
    set_info.creation_time.QuadPart = t_create;
    set_info.last_access_time.QuadPart = t_access;
    set_info.last_write_time.QuadPart = t_write;
    set_info.file_attributes = 0;

    auto set_res = core::SetBasicInfo(h.value->get(), set_info);
    ASSERT_NT_SUCCESS(set_res.status);

    // LastAccessTime is verified on the handle that set it: after the close an
    // antivirus scanner or indexer may legitimately advance it.
    auto same_handle = core::QueryBasicInfo(h.value->get());
    ASSERT_NT_SUCCESS(same_handle.status);
    ASSERT_TRUE(same_handle.value.has_value());
    EXPECT_EQ(same_handle.value->last_access_time.QuadPart, t_access);
  }

  auto got = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(got) << got.Describe();
  EXPECT_EQ(got.value->creation_time.QuadPart, t_create);
  EXPECT_EQ(got.value->last_write_time.QuadPart, t_write);
}

// Spec: [MS-FSCC] 2.4.7 / [MS-FSA] 2.1.5.15.2 -- an explicit ChangeTime set
// through FileBasicInformation persists bit-exactly and independently of
// LastWriteTime. (Dokan reports ChangeTime as a copy of LastWriteTime, see the
// note above BackdateAndVerifyAllTimes, so this contract is kept separate from
// the other three timestamps.)
TEST_F(TimestampsAttrsTest, ExplicitChangeTimeRoundTripsIndependently) {
  const std::wstring path = TestPath(L"explicit_change_time.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "explicit_change_time"));

  const int64_t t_write =
      core::MakeUtcFileTimeInt64(2023, 9, 17, 8, 42, 19) + 3141593;
  const int64_t t_change =
      core::MakeUtcFileTimeInt64(2024, 11, 5, 22, 11, 33) + 9876543;

  {
    auto h = core::CreateWin32File(
        path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        core::kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    nt::FileBasicInfo set_info{};
    set_info.last_write_time.QuadPart = t_write;
    set_info.change_time.QuadPart = t_change;
    set_info.file_attributes = 0;

    auto set_res = core::SetBasicInfo(h.value->get(), set_info);
    ASSERT_NT_SUCCESS(set_res.status);
  }

  auto got = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(got) << got.Describe();
  EXPECT_EQ(got.value->change_time.QuadPart, t_change);
  EXPECT_EQ(got.value->last_write_time.QuadPart, t_write);
}

// Spec: [MS-FSA] 2.1.5.4 Write -- writing on a fresh handle advances
// LastWriteTime and ChangeTime while preserving CreationTime.
TEST_F(TimestampsAttrsTest, WriteUpdatesLastWriteTimeAndChangeTime) {
  const std::wstring path = TestPath(L"write_updates_ts.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "initial"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  {
    auto h2 = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);
    const std::string payload = "updated_payload";
    ASSERT_TRUE(
        core::WriteAllBytes(h2.value->get(), payload.data(), payload.size()));
  }

  auto got = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(got) << got.Describe();
  EXPECT_GT(got.value->last_write_time.QuadPart, kPast);
  EXPECT_GT(got.value->change_time.QuadPart, kPast);
  EXPECT_EQ(got.value->creation_time.QuadPart, kPast);
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- changing file attributes
// advances ChangeTime without modifying LastWriteTime or CreationTime.
TEST_F(TimestampsAttrsTest, MetadataChangeUpdatesChangeTimeNotLastWriteTime) {
  const std::wstring path = TestPath(L"meta_updates_ctime.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "metadata_only"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(
      path.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));

  auto got = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(got) << got.Describe();
  EXPECT_GT(got.value->change_time.QuadPart, kPast);
  EXPECT_EQ(got.value->last_write_time.QuadPart, kPast);
  EXPECT_EQ(got.value->creation_time.QuadPart, kPast);
}

// Spec: [MS-FSA] 2.1.5.1.1 / 2.1.5.4 / 2.1.5.15.3 -- parent directory
// LastWriteTime advances on child creation and deletion, but remains unchanged
// when data is written to an existing child file.
TEST_F(TimestampsAttrsTest, ParentDirLastWriteTimeOnChildCreateDeleteNotWrite) {
  const std::wstring dir = TestPath(L"parent_ts_dir");
  const std::wstring child = core::JoinPath(dir, L"child.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);

  // Creating a child file advances parent directory LastWriteTime.
  ASSERT_TRUE(BackdateAndVerifyAllTimes(dir, kPast, /*is_directory=*/true));
  ASSERT_TRUE(core::WriteFileContents(child, "initial_child"));

  auto after_create = helpers::QueryPathBasicInfo(dir);
  ASSERT_TRUE(after_create) << after_create.Describe();
  EXPECT_GT(after_create.value->last_write_time.QuadPart, kPast);

  // Writing data to the existing child must NOT advance parent LastWriteTime.
  ASSERT_TRUE(BackdateAndVerifyAllTimes(dir, kPast, /*is_directory=*/true));
  {
    auto h = core::CreateWin32File(child, GENERIC_WRITE, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    const std::string more = "additional_child_bytes_written";
    ASSERT_TRUE(core::WriteAllBytes(h.value->get(), more.data(), more.size()));
    ASSERT_WIN32_TRUE(::FlushFileBuffers(h.value->get()));
  }

  auto after_write = helpers::QueryPathBasicInfo(dir);
  ASSERT_TRUE(after_write) << after_write.Describe();
  EXPECT_EQ(after_write.value->last_write_time.QuadPart, kPast);

  // Deleting the child file advances parent directory LastWriteTime.
  ASSERT_WIN32_TRUE(::DeleteFileW(child.c_str()));

  auto after_delete = helpers::QueryPathBasicInfo(dir);
  ASSERT_TRUE(after_delete) << after_delete.Describe();
  EXPECT_GT(after_delete.value->last_write_time.QuadPart, kPast);
}

// Spec: [MS-FSA] 2.1.5.15.5 / 2.1.5.1.2 -- SetEndOfFile and TRUNCATE_EXISTING
// both advance LastWriteTime and set FILE_ATTRIBUTE_ARCHIVE.
TEST_F(TimestampsAttrsTest, SetEndOfFileAndTruncateUpdateLastWriteAndArchive) {
  const std::wstring path = TestPath(L"eof_trunc_ts_archive.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "initial_data_for_eof_truncation"));
  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);

  // Part 1: SetEndOfFile advances LastWriteTime and sets ARCHIVE.
  ASSERT_TRUE(SetAndVerifyAttrBitMask(path, FILE_ATTRIBUTE_NORMAL, 0u,
                                      FILE_ATTRIBUTE_ARCHIVE));
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    ASSERT_TRUE(SeekBegin(h.value->get(), 10));
    ASSERT_WIN32_TRUE(::SetEndOfFile(h.value->get()));
  }

  auto after_eof = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after_eof) << after_eof.Describe();
  EXPECT_GT(after_eof.value->last_write_time.QuadPart, kPast);
  EXPECT_GT(after_eof.value->change_time.QuadPart, kPast);
  EXPECT_NE(after_eof.value->file_attributes & FILE_ATTRIBUTE_ARCHIVE, 0u);

  // Part 2: TRUNCATE_EXISTING advances LastWriteTime and sets ARCHIVE.
  ASSERT_TRUE(SetAndVerifyAttrBitMask(path, FILE_ATTRIBUTE_NORMAL, 0u,
                                      FILE_ATTRIBUTE_ARCHIVE));
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }

  auto after_trunc = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after_trunc) << after_trunc.Describe();
  EXPECT_GT(after_trunc.value->last_write_time.QuadPart, kPast);
  EXPECT_GT(after_trunc.value->change_time.QuadPart, kPast);
  EXPECT_NE(after_trunc.value->file_attributes & FILE_ATTRIBUTE_ARCHIVE, 0u);
  EXPECT_TRUE(helpers::CheckFileContents(path, ""));
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- sentinel -1 suppresses
// automatic LastWriteTime updates for that handle until a new handle writes.
TEST_F(TimestampsAttrsTest, SentinelMinusOneFreezesTimestampUpdates) {
  const std::wstring path = TestPath(L"sentinel_minus_one.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "init"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  // Freeze LastWriteTime on H2 via -1, then write and close H2.
  {
    auto h2 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);

    nt::FileBasicInfo freeze{};
    freeze.last_write_time.QuadPart = -1;
    ASSERT_NT_SUCCESS(core::SetBasicInfo(h2.value->get(), freeze).status);

    const std::string data = "frozen_write_on_h2";
    ASSERT_TRUE(core::WriteAllBytes(h2.value->get(), data.data(), data.size()));
    ASSERT_WIN32_TRUE(::FlushFileBuffers(h2.value->get()));
  }

  auto after_h2 = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after_h2) << after_h2.Describe();
  EXPECT_EQ(after_h2.value->last_write_time.QuadPart, kPast);

  // Writing on a new handle H3 must resume normal LastWriteTime advancement.
  {
    auto h3 = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h3) << core::FormatWin32Error(h3.error);
    const std::string data = "resumed_write_on_h3";
    ASSERT_TRUE(core::WriteAllBytes(h3.value->get(), data.data(), data.size()));
  }

  auto after_h3 = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after_h3) << after_h3.Describe();
  EXPECT_GT(after_h3.value->last_write_time.QuadPart, kPast);
}

// Spec: WDK FILE_BASIC_INFORMATION / [MS-FSA] 2.1.5.15.2 -- sentinel -2
// restores automatic LastWriteTime updates on the same open handle after a -1
// freeze.
//
// OS gate: -2 is a later addition than -1, and Microsoft's documentation does
// not name the first build whose NTFS accepts it. kBuildWin10_1809 (17763) is
// therefore a conservative floor, NOT the documented introduction build: the
// behaviour has only been exercised on the real-NTFS CI images (Windows Server
// 2022 and Server 2025, see .github/workflows/fstest-ntfs.yml). Below the floor
// the test is skipped instead of guessing whether NTFS rejects -2 there.
TEST_F(TimestampsAttrsTest, SentinelMinusTwoRestoresTimestampUpdates) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  const std::wstring path = TestPath(L"sentinel_minus_two.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "init"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  {
    auto h2 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);

    // Freeze via -1 and write -> LastWriteTime stays at kPast.
    nt::FileBasicInfo freeze{};
    freeze.last_write_time.QuadPart = -1;
    ASSERT_NT_SUCCESS(core::SetBasicInfo(h2.value->get(), freeze).status);

    const std::string data1 = "step1_frozen";
    ASSERT_TRUE(
        core::WriteAllBytes(h2.value->get(), data1.data(), data1.size()));

    auto mid = core::QueryBasicInfo(h2.value->get());
    ASSERT_NT_SUCCESS(mid.status);
    ASSERT_TRUE(mid.value.has_value());
    EXPECT_EQ(mid.value->last_write_time.QuadPart, kPast);

    // Unfreeze via -2 on the SAME handle H2 and write -> must advance.
    nt::FileBasicInfo unfreeze{};
    unfreeze.last_write_time.QuadPart = -2;
    ASSERT_NT_SUCCESS(core::SetBasicInfo(h2.value->get(), unfreeze).status);

    const std::string data2 = "step2_unfrozen";
    ASSERT_TRUE(
        core::WriteAllBytes(h2.value->get(), data2.data(), data2.size()));
  }

  auto after = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after) << after.Describe();
  EXPECT_GT(after.value->last_write_time.QuadPart, kPast);
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- negative timestamp values
// strictly below -2 fail with STATUS_INVALID_PARAMETER.
TEST_F(TimestampsAttrsTest, InvalidNegativeTimestampLessThanMinusTwoFails) {
  const std::wstring path = TestPath(L"sentinel_invalid.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "bad_sentinel"));

  auto h = core::CreateWin32File(
      path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
      core::kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

  nt::FileBasicInfo bad_write{};
  bad_write.last_write_time.QuadPart = -3;
  EXPECT_NT_STATUS(core::SetBasicInfo(h.value->get(), bad_write).status,
                   STATUS_INVALID_PARAMETER);

  nt::FileBasicInfo bad_create{};
  bad_create.creation_time.QuadPart = -3;
  EXPECT_NT_STATUS(core::SetBasicInfo(h.value->get(), bad_create).status,
                   STATUS_INVALID_PARAMETER);

  nt::FileBasicInfo bad_deep{};
  bad_deep.last_write_time.QuadPart = -100;
  EXPECT_NT_STATUS(core::SetBasicInfo(h.value->get(), bad_deep).status,
                   STATUS_INVALID_PARAMETER);
}

// Spec: [MS-FSA] 2.1.5.15.2 / Win32 SetFileTime -- directory timestamps set via
// FILE_FLAG_BACKUP_SEMANTICS are applied at 100ns precision; CreationTime and
// LastWriteTime persist across reopen.
TEST_F(TimestampsAttrsTest, DirectoryTimestampsSetAndQuery) {
  const std::wstring dir = TestPath(L"dir_timestamps");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  const int64_t t_create =
      core::MakeUtcFileTimeInt64(2021, 5, 6, 7, 8, 9) + 1357913;
  const int64_t t_access =
      core::MakeUtcFileTimeInt64(2022, 7, 8, 9, 10, 11) + 2468024;
  const int64_t t_write =
      core::MakeUtcFileTimeInt64(2023, 9, 10, 11, 12, 13) + 3579135;

  {
    auto h = core::CreateWin32File(
        dir, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        core::kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    const FILETIME ft_c = helpers::Int64ToFileTime(t_create);
    const FILETIME ft_a = helpers::Int64ToFileTime(t_access);
    const FILETIME ft_w = helpers::Int64ToFileTime(t_write);
    ASSERT_WIN32_TRUE(::SetFileTime(h.value->get(), &ft_c, &ft_a, &ft_w));

    // LastAccessTime is verified on the handle that set it: once it is closed,
    // any enumeration of the directory (indexer, antivirus, Explorer) may
    // legitimately advance LastAccessTime before a reopen could observe it.
    auto set_handle_info = core::QueryBasicInfo(h.value->get());
    ASSERT_NT_SUCCESS(set_handle_info.status);
    ASSERT_TRUE(set_handle_info.value.has_value());
    EXPECT_EQ(set_handle_info.value->last_access_time.QuadPart, t_access);
  }

  auto h_verify = core::CreateWin32File(dir, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                        core::kShareAll, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(h_verify) << core::FormatWin32Error(h_verify.error);

  auto bhfi = core::GetByHandleInfo(h_verify.value->get());
  ASSERT_TRUE(bhfi) << core::FormatWin32Error(bhfi.error);
  EXPECT_EQ(helpers::FileTimeToU64(bhfi.value->ftCreationTime),
            static_cast<uint64_t>(t_create));
  EXPECT_EQ(helpers::FileTimeToU64(bhfi.value->ftLastWriteTime),
            static_cast<uint64_t>(t_write));

  auto basic = core::QueryBasicInfo(h_verify.value->get());
  ASSERT_NT_SUCCESS(basic.status);
  ASSERT_TRUE(basic.value.has_value());
  EXPECT_EQ(basic.value->creation_time.QuadPart, t_create);
  EXPECT_EQ(basic.value->last_write_time.QuadPart, t_write);
}

// Spec: [MS-FSA] 2.1.5.1.1 / 2.1.5.4 -- FILE_ATTRIBUTE_ARCHIVE is set on file
// creation, cleared by SetFileAttributesW(NORMAL), and restored on write.
TEST_F(TimestampsAttrsTest, ArchiveBitSetOnCreateAndWriteAfterClear) {
  const std::wstring path = TestPath(L"archive_bit.dat");

  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }

  auto attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_ARCHIVE, 0u);

  ASSERT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
  attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_EQ(*attrs.value & FILE_ATTRIBUTE_ARCHIVE, 0u);
  EXPECT_EQ(MaskPolicyAttributes(*attrs.value),
            static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL));

  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    const std::string dirty = "dirty_archive_write";
    ASSERT_TRUE(
        core::WriteAllBytes(h.value->get(), dirty.data(), dirty.size()));
  }

  attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_ARCHIVE, 0u);
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- FILE_ATTRIBUTE_TEMPORARY
// and FILE_ATTRIBUTE_NOT_CONTENT_INDEXED set and clear independently.
TEST_F(TimestampsAttrsTest, TemporaryAndNotContentIndexedAttributes) {
  const std::wstring path = TestPath(L"temp_and_nci.dat");

  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }

  constexpr DWORD kTemp = FILE_ATTRIBUTE_TEMPORARY;
  constexpr DWORD kNci = nt::kFileAttributeNotContentIndexed;

  EXPECT_TRUE(SetAndVerifyAttrBitMask(path, kTemp | FILE_ATTRIBUTE_ARCHIVE,
                                      kTemp, kNci));
  EXPECT_TRUE(SetAndVerifyAttrBitMask(path, kNci | FILE_ATTRIBUTE_ARCHIVE, kNci,
                                      kTemp));
  EXPECT_TRUE(SetAndVerifyAttrBitMask(path, kTemp | kNci, kTemp | kNci, 0u));
  EXPECT_TRUE(
      SetAndVerifyAttrBitMask(path, FILE_ATTRIBUTE_ARCHIVE, 0u, kTemp | kNci));
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- explicitly calling
// SetFileTime on a handle freezes automatic LastWriteTime updates on that
// handle, while subsequent handles resume advancing LastWriteTime.
TEST_F(TimestampsAttrsTest,
       HandleExplicitSetFileTimeFreezesSubsequentAutoUpdateOnSameHandle) {
  const std::wstring path = TestPath(L"explicit_sft_freeze.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "initial"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  const FILETIME ft_past = helpers::Int64ToFileTime(kPast);

  {
    auto h1 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

    ASSERT_WIN32_TRUE(
        ::SetFileTime(h1.value->get(), &ft_past, &ft_past, &ft_past));

    // Keep H1 open and write on H1 -> LastWriteTime stays frozen at kPast.
    const std::string data1 = "written_while_frozen_on_h1";
    ASSERT_TRUE(
        core::WriteAllBytes(h1.value->get(), data1.data(), data1.size()));
    ASSERT_WIN32_TRUE(::FlushFileBuffers(h1.value->get()));

    auto info = core::GetByHandleInfo(h1.value->get());
    ASSERT_TRUE(info) << core::FormatWin32Error(info.error);
    EXPECT_EQ(helpers::FileTimeToU64(info.value->ftLastWriteTime),
              static_cast<uint64_t>(kPast));
  }

  // Must remain at kPast even after H1 closes.
  auto after_h1 = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after_h1) << after_h1.Describe();
  EXPECT_EQ(after_h1.value->last_write_time.QuadPart, kPast);

  // Open fresh handle H2 and write -> LastWriteTime must advance.
  {
    auto h2 = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);
    const std::string data2 = "written_on_h2_advances";
    ASSERT_TRUE(
        core::WriteAllBytes(h2.value->get(), data2.data(), data2.size()));
  }

  auto after_h2 = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after_h2) << after_h2.Describe();
  EXPECT_GT(after_h2.value->last_write_time.QuadPart, kPast);
  EXPECT_EQ(after_h2.value->creation_time.QuadPart, kPast);
}

// Spec: [MS-FSA] 2.1.5.15.2 / Win32 GetFileAttributesExW -- setting
// FILE_ATTRIBUTE_DIRECTORY on a regular file is ignored, and
// GetFileAttributesExW matches GetFileInformationByHandle identically.
TEST_F(TimestampsAttrsTest,
       SetFileAttributesDirectoryBitIgnoredAndAttributesExMatches) {
  const std::wstring path = TestPath(L"attr_ex_match.dat");
  constexpr size_t kSize = 1234;
  ASSERT_TRUE(core::WriteFileContents(path, std::string(kSize, 'E')));

  const int64_t t_create =
      core::MakeUtcFileTimeInt64(2021, 2, 3, 4, 5, 6) + 1111111;
  const int64_t t_access =
      core::MakeUtcFileTimeInt64(2022, 3, 4, 5, 6, 7) + 2222222;
  const int64_t t_write =
      core::MakeUtcFileTimeInt64(2023, 4, 5, 6, 7, 8) + 3333333;

  {
    auto h = core::CreateWin32File(
        path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        core::kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    const FILETIME ft_c = helpers::Int64ToFileTime(t_create);
    const FILETIME ft_a = helpers::Int64ToFileTime(t_access);
    const FILETIME ft_w = helpers::Int64ToFileTime(t_write);
    ASSERT_WIN32_TRUE(::SetFileTime(h.value->get(), &ft_c, &ft_a, &ft_w));
  }

  // Attempting to set FILE_ATTRIBUTE_DIRECTORY on a regular file must ignore
  // the directory bit while applying FILE_ATTRIBUTE_ARCHIVE.
  ASSERT_WIN32_TRUE(::SetFileAttributesW(
      path.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE));

  auto raw_attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(raw_attrs) << core::FormatWin32Error(raw_attrs.error);
  EXPECT_EQ(*raw_attrs.value & FILE_ATTRIBUTE_DIRECTORY, 0u);
  EXPECT_EQ(MaskPolicyAttributes(*raw_attrs.value),
            static_cast<DWORD>(FILE_ATTRIBUTE_ARCHIVE));

  WIN32_FILE_ATTRIBUTE_DATA attr_ex{};
  ASSERT_WIN32_TRUE(
      ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr_ex));

  auto h = core::CreateWin32File(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 core::kShareAll, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  auto bhfi = core::GetByHandleInfo(h.value->get());
  ASSERT_TRUE(bhfi) << core::FormatWin32Error(bhfi.error);

  EXPECT_EQ(attr_ex.dwFileAttributes, bhfi.value->dwFileAttributes);
  EXPECT_EQ(attr_ex.nFileSizeHigh, bhfi.value->nFileSizeHigh);
  EXPECT_EQ(attr_ex.nFileSizeLow, bhfi.value->nFileSizeLow);
  EXPECT_EQ(attr_ex.nFileSizeHigh, 0u);
  EXPECT_EQ(attr_ex.nFileSizeLow, static_cast<DWORD>(kSize));
  // LastAccessTime is intentionally not compared: the two queries use separate
  // opens, and an antivirus scanner or indexer touching the file in between
  // (or since the SetFileTime handle closed) may legitimately advance it.
  EXPECT_EQ(helpers::FileTimeToU64(attr_ex.ftCreationTime),
            helpers::FileTimeToU64(bhfi.value->ftCreationTime));
  EXPECT_EQ(helpers::FileTimeToU64(attr_ex.ftLastWriteTime),
            helpers::FileTimeToU64(bhfi.value->ftLastWriteTime));
  EXPECT_EQ(helpers::FileTimeToU64(attr_ex.ftCreationTime),
            static_cast<uint64_t>(t_create));
  EXPECT_EQ(helpers::FileTimeToU64(attr_ex.ftLastWriteTime),
            static_cast<uint64_t>(t_write));
}

// Spec: [MS-FSA] 2.1.5.15.2 / 2.1.5.10 -- FILE_ATTRIBUTE_SPARSE_FILE is
// immutable to SetFileAttributesW and FileBasicInformation(NORMAL); only
// FSCTL_SET_SPARSE(FALSE) clears it.
TEST_F(TimestampsAttrsTest, SetFileAttributesCannotClearSparseFlag) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_attr_immutable.dat");
  {
    auto h = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                   core::kShareAll, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    auto sp = core::SetSparseFile(h.value->get(), true);
    ASSERT_TRUE(sp) << core::FormatWin32Error(sp.error);

    auto info = core::GetByHandleInfo(h.value->get());
    ASSERT_TRUE(info) << core::FormatWin32Error(info.error);
    EXPECT_NE(info.value->dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE, 0u);
  }

  auto attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_SPARSE_FILE, 0u);

  // SetFileAttributesW(FILE_ATTRIBUTE_NORMAL) must NOT clear SPARSE_FILE.
  EXPECT_TRUE(SetAndVerifyAttrBitMask(path, FILE_ATTRIBUTE_NORMAL,
                                      FILE_ATTRIBUTE_SPARSE_FILE,
                                      FILE_ATTRIBUTE_ARCHIVE));

  // NtSetInformationFile(FileBasicInformation, FILE_ATTRIBUTE_NORMAL) also
  // must NOT clear SPARSE_FILE.
  {
    auto h = core::CreateWin32File(
        path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        core::kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    nt::FileBasicInfo info{};
    info.file_attributes = FILE_ATTRIBUTE_NORMAL;
    ASSERT_NT_SUCCESS(core::SetBasicInfo(h.value->get(), info).status);

    auto got = core::QueryBasicInfo(h.value->get());
    ASSERT_NT_SUCCESS(got.status);
    ASSERT_TRUE(got.value.has_value());
    EXPECT_NE(got.value->file_attributes & FILE_ATTRIBUTE_SPARSE_FILE, 0u);
  }

  attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_SPARSE_FILE, 0u);

  // FSCTL_SET_SPARSE(FALSE) DOES clear FILE_ATTRIBUTE_SPARSE_FILE.
  {
    auto h = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                   core::kShareAll, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    auto cleared = core::SetSparseFile(h.value->get(), false);
    ASSERT_TRUE(cleared) << core::FormatWin32Error(cleared.error);

    auto info = core::GetByHandleInfo(h.value->get());
    ASSERT_TRUE(info) << core::FormatWin32Error(info.error);
    EXPECT_EQ(info.value->dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE, 0u);
  }

  attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_EQ(*attrs.value & FILE_ATTRIBUTE_SPARSE_FILE, 0u);
}

// Spec: [MS-FSA] FileRenameInformation -- renaming a file advances its
// ChangeTime while LastWriteTime and CreationTime stay untouched.
TEST_F(TimestampsAttrsTest, RenameUpdatesChangeTimeNotLastWriteTime) {
  const std::wstring src = TestPath(L"ctime_rename_src.dat");
  const std::wstring dst = TestPath(L"ctime_rename_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "rename_ctime"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(src, kPast));

  ASSERT_WIN32_TRUE(::MoveFileExW(src.c_str(), dst.c_str(), 0));

  auto got = helpers::QueryPathBasicInfo(dst);
  ASSERT_TRUE(got) << got.Describe();
  EXPECT_GT(got.value->change_time.QuadPart, kPast);
  EXPECT_EQ(got.value->last_write_time.QuadPart, kPast);
  EXPECT_EQ(got.value->creation_time.QuadPart, kPast);
}

// Spec: [MS-FSA] FileLinkInformation -- adding a hard link changes the file's
// metadata (link count), which advances its ChangeTime while LastWriteTime and
// CreationTime stay untouched.
TEST_F(TimestampsAttrsTest, HardLinkCreationUpdatesChangeTimeNotLastWriteTime) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring path = TestPath(L"ctime_link_target.dat");
  const std::wstring link = TestPath(L"ctime_link_alias.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "link_ctime"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  ASSERT_WIN32_TRUE(::CreateHardLinkW(link.c_str(), path.c_str(), nullptr));

  auto got = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(got) << got.Describe();
  EXPECT_GT(got.value->change_time.QuadPart, kPast);
  EXPECT_EQ(got.value->last_write_time.QuadPart, kPast);
  EXPECT_EQ(got.value->creation_time.QuadPart, kPast);
}

// Spec: [MS-FSA] set security information -- replacing a file's DACL advances
// its ChangeTime while LastWriteTime and CreationTime stay untouched.
TEST_F(TimestampsAttrsTest, SecurityChangeUpdatesChangeTimeNotLastWriteTime) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"ctime_security.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "security_ctime"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  // The new DACL grants Everyone full access, so it cannot lock the test (or
  // the temp-dir cleanup) out of the file and needs no restore guard.
  auto set_dacl =
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)");
  ASSERT_TRUE(set_dacl) << core::FormatWin32Error(set_dacl.error);

  auto got = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(got) << got.Describe();
  EXPECT_GT(got.value->change_time.QuadPart, kPast);
  EXPECT_EQ(got.value->last_write_time.QuadPart, kPast);
  EXPECT_EQ(got.value->creation_time.QuadPart, kPast);
}

// Spec: [MS-FSA] 2.1.5.15.2 FileBasicInformation -- FILE_ATTRIBUTE_TEMPORARY
// is not valid on a directory: setting it fails with STATUS_INVALID_PARAMETER
// (ERROR_INVALID_PARAMETER) and the directory's attributes are unchanged.
TEST_F(TimestampsAttrsTest, TemporaryAttributeRejectedOnDirectory) {
  const std::wstring dir = TestPath(L"temp_attr_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  EXPECT_WIN32_ERROR(
      ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_TEMPORARY),
      ERROR_INVALID_PARAMETER);

  auto attrs = core::QueryWin32Attributes(dir);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_DIRECTORY, 0u);
  EXPECT_EQ(*attrs.value & FILE_ATTRIBUTE_TEMPORARY, 0u);
}

// Spec: [MS-FSA] & Win32 ReadFile — while the host NTFS last-access policy
// (NtfsDisableLastAccessUpdate) disables last-access updates, reading a file's
// data through a fresh handle leaves LastAccessTime untouched.
TEST_F(TimestampsAttrsTest, ReadKeepsLastAccessTimeWhenUpdatesDisabled) {
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_HOST_LAST_ACCESS_POLICY(LastAccessPolicy::kDisabled);

  const std::wstring path = TestPath(L"atime_policy_disabled.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "atime_policy"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  auto before = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(before) << before.Describe();
  ASSERT_EQ(before.value->last_access_time.QuadPart, kPast);

  ASSERT_TRUE(helpers::CheckFileContents(path, "atime_policy"));

  auto after = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after) << after.Describe();
  EXPECT_EQ(after.value->last_access_time.QuadPart, kPast);
}

// Spec: [MS-FSA] & Win32 ReadFile — while the host NTFS last-access policy
// (NtfsDisableLastAccessUpdate) enables last-access updates, reading a file's
// data through a fresh handle and closing it advances LastAccessTime (and
// nothing else).
TEST_F(TimestampsAttrsTest, ReadAdvancesLastAccessTimeWhenUpdatesEnabled) {
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_HOST_LAST_ACCESS_POLICY(LastAccessPolicy::kEnabled);

  const std::wstring path = TestPath(L"atime_policy_enabled.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "atime_policy"));

  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAndVerifyAllTimes(path, kPast));

  ASSERT_TRUE(helpers::CheckFileContents(path, "atime_policy"));

  auto after = helpers::QueryPathBasicInfo(path);
  ASSERT_TRUE(after) << after.Describe();
  EXPECT_GT(after.value->last_access_time.QuadPart, kPast);
  EXPECT_EQ(after.value->last_write_time.QuadPart, kPast);
}

void ForceLink_TimestampsAttrs() {}

} // namespace fstest
