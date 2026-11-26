// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "assertions.h"
#include "fstest/core/capabilities.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/test_config.h"
#include "fstest/suite/test_fixture.h"
#include "fstest/suite/test_metadata.h"
#include "mount_guard.h"
#include "test_support.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

namespace fstest {
namespace {

using test_support::ScopedTestDirectory;

class ScopedSuiteConfigReset {
public:
  ScopedSuiteConfigReset() { ResetSuiteConfig(); }
  ~ScopedSuiteConfigReset() { ResetSuiteConfig(); }

  ScopedSuiteConfigReset(const ScopedSuiteConfigReset &) = delete;
  ScopedSuiteConfigReset &operator=(const ScopedSuiteConfigReset &) = delete;
};

TEST(StatusFormattingTest, NtStatusNameKnownAndUnknown) {
  EXPECT_EQ(core::NtStatusName(STATUS_SUCCESS), "STATUS_SUCCESS");
  EXPECT_EQ(core::NtStatusName(STATUS_OBJECT_NAME_COLLISION),
            "STATUS_OBJECT_NAME_COLLISION");
  EXPECT_EQ(core::NtStatusName(STATUS_DELETE_PENDING), "STATUS_DELETE_PENDING");
  EXPECT_EQ(core::NtStatusName(nt::kStatusOplockBreakInProgress),
            "STATUS_OPLOCK_BREAK_IN_PROGRESS");

  // Clean-room Windows 10+ NTSTATUS constant 0xC00004B3.
  EXPECT_EQ(static_cast<ULONG>(nt::kStatusCaseDifferingNamesInDir),
            0xC00004B3UL);
  EXPECT_EQ(core::NtStatusName(nt::kStatusCaseDifferingNamesInDir),
            "STATUS_CASE_DIFFERING_NAMES_IN_DIR");

  // Unknown status falls back to deterministic 8-digit uppercase hex.
  const NTSTATUS unknown = static_cast<NTSTATUS>(0xC0DE1234L);
  EXPECT_EQ(core::NtStatusName(unknown), "NTSTATUS_0xC0DE1234");
}

TEST(StatusFormattingTest, FormatNtStatusIncludesNameAndHex) {
  const std::string denied = core::FormatNtStatus(STATUS_ACCESS_DENIED);
  EXPECT_NE(denied.find("STATUS_ACCESS_DENIED (0xC0000022)"), std::string::npos)
      << denied;

  const std::string case_diff =
      core::FormatNtStatus(nt::kStatusCaseDifferingNamesInDir);
  EXPECT_NE(case_diff.find("STATUS_CASE_DIFFERING_NAMES_IN_DIR (0xC00004B3)"),
            std::string::npos)
      << case_diff;

  const NTSTATUS unknown = static_cast<NTSTATUS>(0xC0DE1234L);
  const std::string unknown_fmt = core::FormatNtStatus(unknown);
  EXPECT_NE(unknown_fmt.find("NTSTATUS_0xC0DE1234 (0xC0DE1234)"),
            std::string::npos)
      << unknown_fmt;
}

TEST(StatusFormattingTest, Win32ErrorNameKnownAndUnknown) {
  EXPECT_EQ(core::Win32ErrorName(ERROR_SUCCESS), "ERROR_SUCCESS");
  EXPECT_EQ(core::Win32ErrorName(ERROR_FILE_NOT_FOUND), "ERROR_FILE_NOT_FOUND");
  EXPECT_EQ(core::Win32ErrorName(ERROR_ACCESS_DENIED), "ERROR_ACCESS_DENIED");
  EXPECT_EQ(core::Win32ErrorName(ERROR_SHARING_VIOLATION),
            "ERROR_SHARING_VIOLATION");
  EXPECT_EQ(core::Win32ErrorName(ERROR_DELETE_PENDING), "ERROR_DELETE_PENDING");

  EXPECT_EQ(core::Win32ErrorName(999999UL), "ERROR_999999");
}

TEST(StatusFormattingTest, FormatWin32ErrorIncludesNameDecimalAndHex) {
  const std::string sharing = core::FormatWin32Error(ERROR_SHARING_VIOLATION);
  EXPECT_NE(sharing.find("ERROR_SHARING_VIOLATION (32 / 0x20)"),
            std::string::npos)
      << sharing;

  const std::string unknown = core::FormatWin32Error(999999UL);
  EXPECT_NE(unknown.find("ERROR_999999 (999999 / 0xF423F)"), std::string::npos)
      << unknown;
}

TEST(AssertionsTest, Win32OkCoversWin32ResultAndOpenResult) {
  core::Win32Result<int> int_ok{};
  int_ok.error = ERROR_SUCCESS;
  int_ok.value = 42;
  EXPECT_WIN32_OK(int_ok);

  core::Win32Result<int> int_err{};
  int_err.error = ERROR_ACCESS_DENIED;
  const ::testing::AssertionResult int_fail =
      detail::CheckWin32Ok("int_err", int_err);
  EXPECT_FALSE(int_fail);
  EXPECT_NE(std::string(int_fail.message()).find("ERROR_ACCESS_DENIED"),
            std::string::npos)
      << int_fail.message();

  core::Win32Result<void> void_ok{};
  void_ok.error = ERROR_SUCCESS;
  EXPECT_WIN32_OK(void_ok);

  core::Win32Result<void> void_err{};
  void_err.error = ERROR_SHARING_VIOLATION;
  const ::testing::AssertionResult void_fail =
      detail::CheckWin32Ok("void_err", void_err);
  EXPECT_FALSE(void_fail);
  EXPECT_NE(std::string(void_fail.message()).find("ERROR_SHARING_VIOLATION"),
            std::string::npos)
      << void_fail.message();

  // CREATE_ALWAYS / OPEN_ALWAYS return a valid handle with
  // ERROR_ALREADY_EXISTS; EXPECT_WIN32_OK must treat that as ok().
  core::Win32OpenResult open_already{};
  open_already.handle.reset(::CreateEventW(nullptr, /*bManualReset=*/TRUE,
                                           /*bInitialState=*/FALSE, nullptr));
  ASSERT_TRUE(open_already.handle.valid());
  open_already.last_error = ERROR_ALREADY_EXISTS;
  EXPECT_WIN32_OK(open_already);

  core::Win32OpenResult open_err{};
  open_err.last_error = ERROR_FILE_NOT_FOUND;
  const ::testing::AssertionResult open_fail =
      detail::CheckWin32Ok("open_err", open_err);
  EXPECT_FALSE(open_fail);
  EXPECT_NE(std::string(open_fail.message()).find("ERROR_FILE_NOT_FOUND"),
            std::string::npos)
      << open_fail.message();
}

TEST(SecurityHelpersTest, ScopedDaclRestoreRestoresAccessOnDestruction) {
  ScopedTestDirectory dir(L"fstsdr");
  ASSERT_TRUE(dir.ok());

  const std::wstring file_ext = dir.ChildExtendedPath(L"deny_me.txt");
  auto wr = core::WriteFileContents(file_ext, "protected payload");
  ASSERT_TRUE(wr.ok()) << core::FormatWin32Error(wr.error);

  {
    const std::wstring nt_path = core::VerbatimToNtPath(file_ext);
    core::ScopedDaclRestore guard = core::ScopedDaclRestore::Arm(nt_path);
    ASSERT_TRUE(guard.armed());

    // Apply an explicit Deny-All DACL to Everyone.
    auto deny_res = core::SetFileSddl(file_ext, DACL_SECURITY_INFORMATION,
                                      L"D:P(D;;FA;;;WD)");
    ASSERT_TRUE(deny_res.ok()) << core::FormatWin32Error(deny_res.error);

    // Normal read and write opens must now be denied by the restrictive DACL.
    auto blocked_read = core::ReadFileContents(file_ext);
    EXPECT_FALSE(blocked_read.ok());
    EXPECT_EQ(blocked_read.error, static_cast<DWORD>(ERROR_ACCESS_DENIED));
    auto blocked_write =
        core::CreateWin32File(file_ext, GENERIC_WRITE, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(blocked_write.ok());
    EXPECT_EQ(blocked_write.error, static_cast<DWORD>(ERROR_ACCESS_DENIED));
  }

  // Destructor restored a permissive DACL; reading and deleting must succeed.
  auto restored_read = core::ReadFileContents(file_ext);
  ASSERT_TRUE(restored_read.ok())
      << core::FormatWin32Error(restored_read.error);
  EXPECT_EQ(*restored_read.value, "protected payload");
  EXPECT_NE(::DeleteFileW(file_ext.c_str()), FALSE)
      << core::FormatWin32Error(::GetLastError());
}

TEST(SecurityHelpersTest, ScopedDaclRestoreMoveAndExplicitRestoreNow) {
  ScopedTestDirectory dir(L"fstmv");
  ASSERT_TRUE(dir.ok());

  const std::wstring file_ext = dir.ChildExtendedPath(L"move_dacl.txt");
  ASSERT_TRUE(core::WriteFileContents(file_ext, "move payload").ok());

  const std::wstring nt_path = core::VerbatimToNtPath(file_ext);
  core::ScopedDaclRestore first = core::ScopedDaclRestore::Arm(nt_path);
  ASSERT_TRUE(first.armed());

  core::ScopedDaclRestore moved(std::move(first));
  EXPECT_FALSE(first.armed());
  EXPECT_TRUE(moved.armed());

  ASSERT_TRUE(
      core::SetFileSddl(file_ext, DACL_SECURITY_INFORMATION, L"D:P(D;;FA;;;WD)")
          .ok());
  EXPECT_FALSE(core::ReadFileContents(file_ext).ok());

  moved.RestoreNow();
  EXPECT_FALSE(moved.armed());

  auto content = core::ReadFileContents(file_ext);
  ASSERT_TRUE(content.ok()) << core::FormatWin32Error(content.error);
  EXPECT_EQ(*content.value, "move payload");
  EXPECT_NE(::DeleteFileW(file_ext.c_str()), FALSE);
}

TEST(SecurityHelpersTest, SetFileSddlGetFileSddlAndResetDacl) {
  ScopedTestDirectory dir(L"fstsddl");
  ASSERT_TRUE(dir.ok());

  const std::wstring file_ext = dir.ChildExtendedPath(L"sddl.txt");
  ASSERT_TRUE(core::WriteFileContents(file_ext, "acl check").ok());

  auto set_res = core::SetFileSddl(file_ext, DACL_SECURITY_INFORMATION,
                                   L"D:P(A;;FA;;;WD)");
  ASSERT_TRUE(set_res.ok()) << core::FormatWin32Error(set_res.error);

  auto got_res = core::GetFileSddl(file_ext, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(got_res.ok()) << core::FormatWin32Error(got_res.error);
  EXPECT_EQ(*got_res.value, L"D:P(A;;FA;;;WD)");

  // Deny all access, then recover cleanly via ResetDaclToFullControl.
  ASSERT_TRUE(
      core::SetFileSddl(file_ext, DACL_SECURITY_INFORMATION, L"D:P(D;;FA;;;WD)")
          .ok());
  EXPECT_FALSE(core::ReadFileContents(file_ext).ok());

  auto reset_res =
      core::ResetDaclToFullControl(file_ext, /*is_reparse_point=*/false);
  ASSERT_TRUE(reset_res.ok()) << core::FormatWin32Error(reset_res.error);

  auto restored = core::ReadFileContents(file_ext);
  ASSERT_TRUE(restored.ok()) << core::FormatWin32Error(restored.error);
  EXPECT_EQ(*restored.value, "acl check");
}

TEST(SuiteConfigTest, ConfigureSuiteAndResetSuiteConfig) {
  ScopedSuiteConfigReset reset_guard;

  const TestConfig &defaults = GetSuiteConfig();
  EXPECT_TRUE(defaults.root_path.empty());
  EXPECT_FALSE(defaults.is_volume_root.has_value());
  EXPECT_EQ(defaults.temp_prefix, L"fst");
  EXPECT_EQ(defaults.large_dir_entry_count, 600u);
  EXPECT_FALSE(defaults.respect_fs_capabilities);
  EXPECT_FALSE(defaults.preprobed_capabilities.has_value());
  EXPECT_EQ(defaults.timing.async_timeout_ms, 5000u);

  TestConfig custom;
  custom.root_path = L"M:\\";
  custom.is_volume_root = true;
  custom.temp_prefix = L"pfx";
  custom.large_dir_entry_count = 250;
  custom.respect_fs_capabilities = true;
  custom.timing.async_timeout_ms = 9000;
  custom.capability_overrides.named_streams = false;
  custom.capability_overrides.sector_size = 8192;

  core::VolumeCapabilities preprobed;
  preprobed.filesystem_name = L"MockFS";
  preprobed.probe_succeeded = true;
  preprobed.named_streams = true;
  custom.preprobed_capabilities = preprobed;

  ConfigureSuite(std::move(custom));
  const TestConfig &configured = GetSuiteConfig();
  EXPECT_EQ(configured.root_path, L"M:\\");
  ASSERT_TRUE(configured.is_volume_root.has_value());
  EXPECT_TRUE(*configured.is_volume_root);
  EXPECT_EQ(configured.temp_prefix, L"pfx");
  EXPECT_EQ(configured.large_dir_entry_count, 250u);
  EXPECT_TRUE(configured.respect_fs_capabilities);
  EXPECT_EQ(configured.timing.async_timeout_ms, 9000u);
  ASSERT_TRUE(configured.capability_overrides.named_streams.has_value());
  EXPECT_FALSE(*configured.capability_overrides.named_streams);
  ASSERT_TRUE(configured.capability_overrides.sector_size.has_value());
  EXPECT_EQ(*configured.capability_overrides.sector_size, 8192u);
  ASSERT_TRUE(configured.preprobed_capabilities.has_value());
  EXPECT_EQ(configured.preprobed_capabilities->filesystem_name, L"MockFS");

  ResetSuiteConfig();
  const TestConfig &after_reset = GetSuiteConfig();
  EXPECT_TRUE(after_reset.root_path.empty());
  EXPECT_FALSE(after_reset.is_volume_root.has_value());
  EXPECT_EQ(after_reset.temp_prefix, L"fst");
  EXPECT_FALSE(after_reset.respect_fs_capabilities);
  EXPECT_FALSE(after_reset.preprobed_capabilities.has_value());
  EXPECT_FALSE(after_reset.capability_overrides.named_streams.has_value());
  EXPECT_FALSE(after_reset.capability_overrides.sector_size.has_value());
}

TEST(SuiteMetadataTest, DomainMappingAndKnownSuiteChecks) {
  const auto &domains = GetAllDomains();
  ASSERT_EQ(domains.size(), 12u);

  std::set<std::string_view> seen_ids;
  std::set<std::string_view> seen_suites;
  for (const DomainInfo &info : domains) {
    EXPECT_FALSE(info.domain_id.empty());
    EXPECT_FALSE(info.suite_name.empty());
    EXPECT_TRUE(seen_ids.insert(info.domain_id).second);
    EXPECT_TRUE(seen_suites.insert(info.suite_name).second);
    EXPECT_TRUE(IsKnownSuiteName(info.suite_name));
    EXPECT_EQ(SuiteNameToDomainId(info.suite_name), info.domain_id);
  }

  EXPECT_EQ(SuiteNameToDomainId("CreateOpenTest"), "create_open");
  EXPECT_EQ(SuiteNameToDomainId("InfoQueriesTest"), "info_queries");
  EXPECT_EQ(SuiteNameToDomainId("UnknownCustomSuite"), "UnknownCustomSuite");
  EXPECT_FALSE(IsKnownSuiteName("UnknownCustomSuite"));
  EXPECT_FALSE(IsKnownSuiteName("createopentype"));
  EXPECT_FALSE(IsKnownSuiteName(""));
  EXPECT_EQ(std::string_view(kFatalTeardownProperty), "fstest_fatal_teardown");
  EXPECT_EQ(std::string_view(kFatalSetupProperty), "fstest_fatal_setup");
  EXPECT_EQ(std::string_view(kSkipCategoryProperty), "skip_category");
}

class FixturePreprobedAndOverridesTest : public NtfsTestBase {
public:
  static void SetUpTestSuite() {
    suite_root_dir_.emplace(L"fstfix");
    ASSERT_TRUE(suite_root_dir_->ok());

    core::VolumeCapabilities preprobed{};
    preprobed.filesystem_name = L"SyntheticPreprobedFS";
    preprobed.bytes_per_sector = 512;
    preprobed.physical_bytes_per_sector = 4096;
    preprobed.bytes_per_cluster = 4096;
    preprobed.named_streams = false;
    preprobed.sparse_files = true;
    preprobed.hard_links = false;
    preprobed.probe_succeeded = true;

    TestConfig cfg;
    cfg.root_path = suite_root_dir_->Path();
    cfg.respect_fs_capabilities = true;
    cfg.preprobed_capabilities = preprobed;
    cfg.capability_overrides.named_streams = true;
    cfg.capability_overrides.sparse_files = false;
    cfg.capability_overrides.sector_size = 8192;
    ConfigureSuite(std::move(cfg));

    NtfsTestBase::SetUpTestSuite();
  }

  static void TearDownTestSuite() {
    NtfsTestBase::TearDownTestSuite();
    ResetSuiteConfig();
    suite_root_dir_.reset();
  }

private:
  inline static std::optional<ScopedTestDirectory> suite_root_dir_;
};

TEST_F(FixturePreprobedAndOverridesTest,
       ReusesPreprobedCapabilitiesAndHonorsOverrides) {
  EXPECT_EQ(Caps().filesystem_name, L"SyntheticPreprobedFS");
  EXPECT_EQ(EffectiveSectorSize(), 8192u);

  // Override true takes precedence over probed false.
  std::string reason;
  EXPECT_FALSE(ShouldSkipForFsCapability(FsCapability::kNamedStreams, &reason));

  // Override false takes precedence over probed true.
  reason.clear();
  EXPECT_TRUE(ShouldSkipForFsCapability(FsCapability::kSparseFiles, &reason));
  EXPECT_NE(reason.find("CapabilityOverrides"), std::string::npos) << reason;

  // No override + respect_fs_capabilities=true falls back to probed false.
  reason.clear();
  EXPECT_TRUE(ShouldSkipForFsCapability(FsCapability::kHardLinks, &reason));
  EXPECT_NE(reason.find("HardLinks"), std::string::npos) << reason;
}

// Drives NtfsTestBase's suite/test lifecycle directly (outside GoogleTest's
// runner) so every LifecycleHooks path is exercised deterministically inside
// the calling plain TEST.
class FixtureHarness : public NtfsTestBase {
public:
  void TestBody() override {}
  void RunSetUp() { SetUp(); }
  void RunTearDown() { TearDown(); }
  using NtfsTestBase::Root;
  using NtfsTestBase::TempDir;
};

bool CurrentTestHasProperty(std::string_view key, std::string_view value) {
  const ::testing::TestInfo *info =
      ::testing::UnitTest::GetInstance()->current_test_info();
  if (info == nullptr || info->result() == nullptr) {
    return false;
  }
  const ::testing::TestResult &result = *info->result();
  for (int i = 0; i < result.test_property_count(); ++i) {
    const ::testing::TestProperty &prop = result.GetTestProperty(i);
    if (key == prop.key() && value == prop.value()) {
      return true;
    }
  }
  return false;
}

std::string CurrentFullTestName() {
  const ::testing::TestInfo *info =
      ::testing::UnitTest::GetInstance()->current_test_info();
  return info == nullptr
             ? std::string()
             : std::string(info->test_suite_name()) + "." + info->name();
}

bool DirectoryExists(const std::wstring &path) {
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Runs SetUp() while capturing (instead of reporting) its GoogleTest failures.
std::vector<::testing::TestPartResult>
RunSetUpCapturingFailures(FixtureHarness *h) {
  ::testing::TestPartResultArray failures;
  {
    ::testing::ScopedFakeTestPartResultReporter reporter(
        ::testing::ScopedFakeTestPartResultReporter::
            INTERCEPT_ONLY_CURRENT_THREAD,
        &failures);
    h->RunSetUp();
  }
  std::vector<::testing::TestPartResult> out;
  for (int i = 0; i < failures.size(); ++i) {
    out.push_back(failures.GetTestPartResult(i));
  }
  return out;
}

std::vector<::testing::TestPartResult>
RunTearDownCapturingFailures(FixtureHarness *h) {
  ::testing::TestPartResultArray failures;
  {
    ::testing::ScopedFakeTestPartResultReporter reporter(
        ::testing::ScopedFakeTestPartResultReporter::
            INTERCEPT_ONLY_CURRENT_THREAD,
        &failures);
    h->RunTearDown();
  }
  std::vector<::testing::TestPartResult> out;
  for (int i = 0; i < failures.size(); ++i) {
    out.push_back(failures.GetTestPartResult(i));
  }
  return out;
}

TEST(LifecycleHooksTest, AllHooksRunInOrderWithRootOverride) {
  ScopedSuiteConfigReset reset_guard;
  ScopedTestDirectory override_dir(L"fsthk");
  ASSERT_TRUE(override_dir.ok());
  const std::wstring override_root = override_dir.Path();

  std::vector<std::string> events;
  TestConfig cfg;
  // Unreachable on purpose: before_suite must override it.
  cfg.root_path = L"\\\\?\\Z:\\fstest-lifecycle-does-not-exist";
  cfg.hooks.before_suite = [&](std::string_view suite_name,
                               std::wstring *active_root_out,
                               std::string * /*error_out*/) {
    events.push_back("before_suite:" + std::string(suite_name));
    *active_root_out = override_root;
    return true;
  };
  cfg.hooks.after_suite = [&](std::string_view suite_name) {
    events.push_back("after_suite:" + std::string(suite_name));
  };
  cfg.hooks.before_test = [&](const TestLifecycleInfo &info,
                              std::string * /*error_out*/) {
    events.push_back("before_test:" + info.full_name);
    EXPECT_EQ(info.full_name, info.suite_name + "." + info.test_name);
    return true;
  };
  cfg.hooks.after_test = [&](const TestLifecycleInfo &info, bool test_passed,
                             std::string * /*error_out*/) {
    events.push_back("after_test:" + info.full_name + ":" +
                     (test_passed ? "passed" : "failed"));
    return true;
  };
  ConfigureSuite(std::move(cfg));

  std::wstring test_dir;
  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    h.RunSetUp();
    ASSERT_FALSE(::testing::Test::HasFailure());
    EXPECT_TRUE(
        core::EqualsIgnoreCase(core::NormalizeWin32Path(h.Root().win32_root),
                               core::NormalizeWin32Path(override_root)))
        << core::WideToUtf8(h.Root().win32_root);
    ASSERT_TRUE(h.TempDir().IsActive());
    test_dir = h.TempDir().Path();
    EXPECT_TRUE(DirectoryExists(test_dir));
    h.RunTearDown();
  }
  FixtureHarness::TearDownTestSuite();

  // Zero-debris: the per-test directory is gone after TearDown().
  EXPECT_FALSE(DirectoryExists(test_dir)) << core::WideToUtf8(test_dir);
  EXPECT_FALSE(CurrentTestHasProperty(kFatalSetupProperty, "1"));
  EXPECT_FALSE(CurrentTestHasProperty(kFatalTeardownProperty, "1"));

  const std::string suite = "LifecycleHooksTest";
  const std::string full = CurrentFullTestName();
  EXPECT_EQ(events, (std::vector<std::string>{
                        "before_suite:" + suite,
                        "before_test:" + full,
                        "after_test:" + full + ":passed",
                        "after_suite:" + suite,
                    }));
}

TEST(LifecycleHooksTest, BeforeTestFailureRecordsFatalSetupAndSkipsAfterTest) {
  ScopedSuiteConfigReset reset_guard;
  ScopedTestDirectory root_dir(L"fsthk");
  ASSERT_TRUE(root_dir.ok());

  int after_test_calls = 0;
  int after_suite_calls = 0;
  TestConfig cfg;
  cfg.root_path = root_dir.Path();
  cfg.hooks.before_test = [](const TestLifecycleInfo &,
                             std::string *error_out) {
    *error_out = "mount gone";
    return false;
  };
  cfg.hooks.after_test = [&](const TestLifecycleInfo &, bool, std::string *) {
    ++after_test_calls;
    return true;
  };
  cfg.hooks.after_suite = [&](std::string_view) { ++after_suite_calls; };
  ConfigureSuite(std::move(cfg));

  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    const std::vector<::testing::TestPartResult> failures =
        RunSetUpCapturingFailures(&h);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_TRUE(failures[0].fatally_failed());
    EXPECT_NE(std::string(failures[0].message())
                  .find("before_test hook failed: mount gone"),
              std::string::npos)
        << failures[0].message();
    EXPECT_TRUE(CurrentTestHasProperty(kFatalSetupProperty, "1"));
    // No temp dir is created after a failed before_test.
    EXPECT_FALSE(h.TempDir().IsActive());
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  FixtureHarness::TearDownTestSuite();

  // after_test only runs when before_test succeeded; after_suite still runs
  // because before_suite is unset.
  EXPECT_EQ(after_test_calls, 0);
  EXPECT_EQ(after_suite_calls, 1);
}

TEST(LifecycleHooksTest, BeforeSuiteFailureFailsSetUpAndSkipsOtherHooks) {
  ScopedSuiteConfigReset reset_guard;
  int before_test_calls = 0;
  int after_suite_calls = 0;
  TestConfig cfg;
  cfg.root_path = L"C:\\";
  cfg.hooks.before_suite = [](std::string_view, std::wstring *,
                              std::string *error_out) {
    *error_out = "embedder mount failed";
    return false;
  };
  cfg.hooks.before_test = [&](const TestLifecycleInfo &, std::string *) {
    ++before_test_calls;
    return true;
  };
  cfg.hooks.after_suite = [&](std::string_view) { ++after_suite_calls; };
  ConfigureSuite(std::move(cfg));

  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    const std::vector<::testing::TestPartResult> failures =
        RunSetUpCapturingFailures(&h);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(std::string(failures[0].message())
                  .find("Suite setup failed: embedder mount failed"),
              std::string::npos)
        << failures[0].message();
    EXPECT_TRUE(CurrentTestHasProperty(kFatalSetupProperty, "1"));
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  FixtureHarness::TearDownTestSuite();

  EXPECT_EQ(before_test_calls, 0);
  // after_suite is skipped when before_suite was set but did not succeed.
  EXPECT_EQ(after_suite_calls, 0);
}

void TriggerBeforeSuiteSkip(std::string_view reason) { GTEST_SKIP() << reason; }

TEST(LifecycleHooksTest,
     BeforeSuiteSkipDoesNotInvokeAfterTestWithoutBeforeTest) {
  // Runs in a death-test child process because GTEST_SKIP() inside
  // before_suite marks the current GoogleTest TestResult as skipped.
  EXPECT_EXIT(
      {
        ScopedSuiteConfigReset reset_guard;
        int after_test_calls = 0;
        int after_suite_calls = 0;
        TestConfig cfg;
        cfg.root_path = L"C:\\";
        cfg.hooks.before_suite = [](std::string_view, std::wstring *,
                                    std::string *error_out) {
          *error_out = "suite skipped in before_suite";
          TriggerBeforeSuiteSkip(*error_out);
          return true;
        };
        // Deliberately leave before_test unset to verify TearDown() checks
        // state.setup_skipped before invoking after_test.
        cfg.hooks.after_test = [&](const TestLifecycleInfo &, bool,
                                   std::string *) {
          ++after_test_calls;
          return true;
        };
        cfg.hooks.after_suite = [&](std::string_view) { ++after_suite_calls; };
        ConfigureSuite(std::move(cfg));

        FixtureHarness::SetUpTestSuite();
        bool saw_skip = false;
        bool teardown_clean = false;
        {
          FixtureHarness h;
          const std::vector<::testing::TestPartResult> setup_parts =
              RunSetUpCapturingFailures(&h);
          saw_skip = setup_parts.size() == 1u && setup_parts[0].skipped();
          teardown_clean = RunTearDownCapturingFailures(&h).empty();
        }
        FixtureHarness::TearDownTestSuite();

        if (!saw_skip || !teardown_clean || after_test_calls != 0 ||
            after_suite_calls != 0) {
          std::cerr << "saw_skip=" << saw_skip
                    << " teardown_clean=" << teardown_clean
                    << " after_test_calls=" << after_test_calls
                    << " after_suite_calls=" << after_suite_calls << "\n";
          std::fflush(nullptr);
          std::_Exit(1);
        }
        std::fflush(nullptr);
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(LifecycleHooksTest, TempDirCreationFailureRecordsFatalSetup) {
  ScopedSuiteConfigReset reset_guard;
  ScopedTestDirectory parent(L"fsthk");
  ASSERT_TRUE(parent.ok());
  const std::wstring root = parent.ChildPath(L"vanishing_root");
  ASSERT_NE(::CreateDirectoryW(root.c_str(), nullptr), FALSE)
      << core::FormatWin32Error(::GetLastError());

  int after_test_calls = 0;
  TestConfig cfg;
  cfg.root_path = root;
  cfg.hooks.before_test = [](const TestLifecycleInfo &, std::string *) {
    return true;
  };
  cfg.hooks.after_test = [&](const TestLifecycleInfo &, bool, std::string *) {
    ++after_test_calls;
    return true;
  };
  ConfigureSuite(std::move(cfg));

  FixtureHarness::SetUpTestSuite();
  // The root disappears after suite setup (e.g. an unmount between tests).
  ASSERT_NE(::RemoveDirectoryW(root.c_str()), FALSE)
      << core::FormatWin32Error(::GetLastError());
  {
    FixtureHarness h;
    const std::vector<::testing::TestPartResult> failures =
        RunSetUpCapturingFailures(&h);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(std::string(failures[0].message())
                  .find("Failed to create isolated temp directory"),
              std::string::npos)
        << failures[0].message();
    EXPECT_TRUE(CurrentTestHasProperty(kFatalSetupProperty, "1"));
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  FixtureHarness::TearDownTestSuite();
  // before_test succeeded, so after_test still runs for symmetry.
  EXPECT_EQ(after_test_calls, 1);
}

TEST(LifecycleHooksTest, AfterTestFailureRecordsFatalTeardown) {
  ScopedSuiteConfigReset reset_guard;
  ScopedTestDirectory root_dir(L"fsthk");
  ASSERT_TRUE(root_dir.ok());

  TestConfig cfg;
  cfg.root_path = root_dir.Path();
  cfg.hooks.after_test = [](const TestLifecycleInfo &, bool,
                            std::string *error_out) {
    *error_out = "leak detected";
    return false;
  };
  ConfigureSuite(std::move(cfg));

  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    EXPECT_EQ(RunSetUpCapturingFailures(&h).size(), 0u);
    const std::wstring test_dir = h.TempDir().Path();
    const std::vector<::testing::TestPartResult> failures =
        RunTearDownCapturingFailures(&h);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_FALSE(failures[0].fatally_failed());
    EXPECT_NE(std::string(failures[0].message())
                  .find("after_test hook failed: leak detected"),
              std::string::npos)
        << failures[0].message();
    EXPECT_TRUE(CurrentTestHasProperty(kFatalTeardownProperty, "1"));
    EXPECT_FALSE(DirectoryExists(test_dir));
  }
  FixtureHarness::TearDownTestSuite();
}

TEST(LifecycleHooksTest, RootIsReprobedForEverySuiteWithoutStaleCache) {
  ScopedSuiteConfigReset reset_guard;
  ScopedTestDirectory parent(L"fsthk");
  ASSERT_TRUE(parent.ok());
  const std::wstring root = parent.ChildPath(L"suite_root");
  ASSERT_NE(::CreateDirectoryW(root.c_str(), nullptr), FALSE);

  TestConfig cfg;
  cfg.root_path = root;
  ConfigureSuite(std::move(cfg));

  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    EXPECT_EQ(RunSetUpCapturingFailures(&h).size(), 0u);
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  FixtureHarness::TearDownTestSuite();

  // Unmount between suites: the next suite must notice (no probe cache).
  ASSERT_NE(::RemoveDirectoryW(root.c_str()), FALSE);
  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    const std::vector<::testing::TestPartResult> failures =
        RunSetUpCapturingFailures(&h);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(std::string(failures[0].message()).find("Suite setup failed"),
              std::string::npos)
        << failures[0].message();
    EXPECT_TRUE(CurrentTestHasProperty(kFatalSetupProperty, "1"));
  }
  FixtureHarness::TearDownTestSuite();
}

TEST(MountLivenessGuardTest, ParseSelfTestFault) {
  cli::SelfTestFault fault = cli::SelfTestFault::kAfterTest;
  EXPECT_TRUE(cli::ParseSelfTestFault("", &fault));
  EXPECT_EQ(fault, cli::SelfTestFault::kNone);
  EXPECT_TRUE(cli::ParseSelfTestFault("before_test", &fault));
  EXPECT_EQ(fault, cli::SelfTestFault::kBeforeTest);
  EXPECT_TRUE(cli::ParseSelfTestFault("after_test", &fault));
  EXPECT_EQ(fault, cli::SelfTestFault::kAfterTest);
  EXPECT_TRUE(cli::ParseSelfTestFault("mount_lost", &fault));
  EXPECT_EQ(fault, cli::SelfTestFault::kMountLost);
  EXPECT_FALSE(cli::ParseSelfTestFault("bogus", &fault));
  EXPECT_FALSE(cli::ParseSelfTestFault("AFTER_TEST", &fault));
}

TEST(MountLivenessGuardTest, ReportsLastCompletedTestAndAbortsOnce) {
  DWORD probe_result = ERROR_SUCCESS;
  int probe_calls = 0;
  int abort_calls = 0;
  cli::MountLivenessGuard guard(
      [&]() {
        ++probe_calls;
        return probe_result;
      },
      "M:\\", [&]() { ++abort_calls; });

  LifecycleHooks hooks;
  guard.Install(&hooks);
  ASSERT_TRUE(static_cast<bool>(hooks.before_suite));
  ASSERT_TRUE(static_cast<bool>(hooks.before_test));
  ASSERT_TRUE(static_cast<bool>(hooks.after_test));
  EXPECT_FALSE(static_cast<bool>(hooks.after_suite));

  const TestLifecycleInfo first{"CreateOpenTest", "First",
                                "CreateOpenTest.First"};
  const TestLifecycleInfo second{"CreateOpenTest", "Second",
                                 "CreateOpenTest.Second"};
  std::string err;
  EXPECT_TRUE(hooks.before_test(first, &err)) << err;
  EXPECT_TRUE(hooks.after_test(first, /*test_passed=*/false, &err)) << err;
  EXPECT_EQ(guard.last_completed_test(), "CreateOpenTest.First");
  EXPECT_EQ(guard.completed_tests(), 1u);
  EXPECT_FALSE(guard.mount_lost());

  probe_result = ERROR_NOT_READY;
  EXPECT_FALSE(hooks.before_test(second, &err));
  EXPECT_TRUE(guard.mount_lost());
  EXPECT_EQ(err, guard.lost_message());
  EXPECT_EQ(guard.lost_message().rfind(
                "mount lost after CreateOpenTest.First: root 'M:\\' is not "
                "reachable (ERROR_NOT_READY",
                0),
            0u)
      << guard.lost_message();
  EXPECT_EQ(abort_calls, 1);

  // Once lost, later tests fail immediately without re-probing or re-aborting.
  const int probes_before = probe_calls;
  probe_result = ERROR_SUCCESS;
  err.clear();
  EXPECT_FALSE(hooks.before_test(second, &err));
  EXPECT_EQ(err, guard.lost_message());
  EXPECT_EQ(probe_calls, probes_before);
  EXPECT_EQ(abort_calls, 1);
}

TEST(MountLivenessGuardTest, LostBeforeFirstTestAndFaultInjection) {
  cli::MountLivenessGuard lost_at_start(
      []() { return DWORD{ERROR_PATH_NOT_FOUND}; }, "X:\\gone");
  std::string err;
  EXPECT_FALSE(lost_at_start.BeforeTest({"S", "T", "S.T"}, &err));
  EXPECT_NE(err.find("mount lost after <start of run>"), std::string::npos)
      << err;

  cli::MountLivenessGuard before_fault([]() { return DWORD{ERROR_SUCCESS}; },
                                       "M:\\", {},
                                       cli::SelfTestFault::kBeforeTest);
  err.clear();
  EXPECT_FALSE(before_fault.BeforeTest({"S", "T", "S.T"}, &err));
  EXPECT_FALSE(before_fault.mount_lost());
  EXPECT_NE(err.find("before_test"), std::string::npos) << err;

  cli::MountLivenessGuard after_fault([]() { return DWORD{ERROR_SUCCESS}; },
                                      "M:\\", {},
                                      cli::SelfTestFault::kAfterTest);
  err.clear();
  EXPECT_TRUE(after_fault.BeforeTest({"S", "T", "S.T"}, &err));
  EXPECT_FALSE(after_fault.AfterTest({"S", "T", "S.T"}, true, &err));
  EXPECT_NE(err.find("after_test"), std::string::npos) << err;

  int aborts = 0;
  cli::MountLivenessGuard lost_fault([]() { return DWORD{ERROR_SUCCESS}; },
                                     "M:\\", [&]() { ++aborts; },
                                     cli::SelfTestFault::kMountLost);
  err.clear();
  EXPECT_TRUE(lost_fault.BeforeTest({"S", "A", "S.A"}, &err));
  EXPECT_TRUE(lost_fault.AfterTest({"S", "A", "S.A"}, true, &err));
  EXPECT_FALSE(lost_fault.BeforeTest({"S", "B", "S.B"}, &err));
  EXPECT_TRUE(lost_fault.mount_lost());
  EXPECT_NE(err.find("mount lost after S.A"), std::string::npos) << err;
  EXPECT_EQ(aborts, 1);
}

TEST(MountLivenessGuardTest, GuardInstalledInFixtureDetectsVanishedRoot) {
  ScopedSuiteConfigReset reset_guard;
  ScopedTestDirectory parent(L"fsthk");
  ASSERT_TRUE(parent.ok());
  const std::wstring root = parent.ChildPath(L"guarded_root");
  ASSERT_NE(::CreateDirectoryW(root.c_str(), nullptr), FALSE);

  auto norm = core::NormalizeRootPath(root, std::nullopt);
  ASSERT_TRUE(norm.ok()) << core::FormatWin32Error(norm.error);
  const core::NormalizedRoot guarded = *norm.value;
  int aborts = 0;
  cli::MountLivenessGuard guard(
      [&guarded]() { return core::ProbeRootDirectory(guarded); }, "guarded",
      [&]() { ++aborts; });

  TestConfig cfg;
  cfg.root_path = root;
  guard.Install(&cfg.hooks);
  ConfigureSuite(std::move(cfg));

  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    EXPECT_EQ(RunSetUpCapturingFailures(&h).size(), 0u);
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  EXPECT_EQ(guard.last_completed_test(), CurrentFullTestName());

  // The "mount" disappears between tests.
  ASSERT_NE(::RemoveDirectoryW(root.c_str()), FALSE);
  {
    FixtureHarness h;
    const std::vector<::testing::TestPartResult> failures =
        RunSetUpCapturingFailures(&h);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(std::string(failures[0].message())
                  .find("mount lost after " + CurrentFullTestName()),
              std::string::npos)
        << failures[0].message();
    EXPECT_TRUE(CurrentTestHasProperty(kFatalSetupProperty, "1"));
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  FixtureHarness::TearDownTestSuite();
  EXPECT_TRUE(guard.mount_lost());
  EXPECT_EQ(aborts, 1);
}

TEST(MountLivenessGuardTest, SuiteBoundaryVanishedRootTripsMountLost) {
  ScopedSuiteConfigReset reset_guard;
  ScopedTestDirectory parent(L"fstsb");
  ASSERT_TRUE(parent.ok());
  const std::wstring root = parent.ChildPath(L"suite_root");
  ASSERT_NE(::CreateDirectoryW(root.c_str(), nullptr), FALSE);

  auto norm = core::NormalizeRootPath(root, std::nullopt);
  ASSERT_TRUE(norm.ok()) << core::FormatWin32Error(norm.error);
  const core::NormalizedRoot guarded = *norm.value;
  int aborts = 0;
  cli::MountLivenessGuard guard(
      [&guarded]() { return core::ProbeRootDirectory(guarded); }, "suite_root",
      [&]() { ++aborts; });

  TestConfig cfg;
  cfg.root_path = root;
  guard.Install(&cfg.hooks);
  ConfigureSuite(std::move(cfg));

  // Suite 1 completes cleanly.
  FixtureHarness::SetUpTestSuite();
  {
    FixtureHarness h;
    EXPECT_EQ(RunSetUpCapturingFailures(&h).size(), 0u);
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  FixtureHarness::TearDownTestSuite();
  EXPECT_FALSE(guard.mount_lost());

  // Root vanishes between Suite 1 and Suite 2.
  ASSERT_NE(::RemoveDirectoryW(root.c_str()), FALSE);

  FixtureHarness::SetUpTestSuite();
  EXPECT_TRUE(guard.mount_lost());
  EXPECT_EQ(aborts, 1);
  {
    FixtureHarness h;
    const std::vector<::testing::TestPartResult> failures =
        RunSetUpCapturingFailures(&h);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(std::string(failures[0].message())
                  .find("mount lost after " + CurrentFullTestName()),
              std::string::npos)
        << failures[0].message();
    EXPECT_TRUE(CurrentTestHasProperty(kFatalSetupProperty, "1"));
    EXPECT_EQ(RunTearDownCapturingFailures(&h).size(), 0u);
  }
  FixtureHarness::TearDownTestSuite();
}

} // namespace
} // namespace fstest
