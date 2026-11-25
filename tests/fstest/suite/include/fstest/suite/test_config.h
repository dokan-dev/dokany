// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file test_config.h
/// \brief Process-wide `TestConfig`, capability overrides, timing parameters,
/// and embedder lifecycle hooks consumed by `NtfsTestBase`.

#pragma once

#include "fstest/core/capabilities.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fstest {

/// \brief Optional explicit per-capability overrides applied on top of probed
/// `VolumeCapabilities` and host OS probes.
///
/// Setting a boolean field to `false` causes the corresponding
/// `REQUIRE_FS_CAPABILITY` (or `REQUIRE_SYMLINK_CREATION` /
/// `REQUIRE_SHORT_NAMES_ENABLED`) check to skip even when
/// `TestConfig::respect_fs_capabilities` is `false`. Setting a field to `true`
/// forces the capability to be treated as supported even if probed `false`.
struct CapabilityOverrides {
  std::optional<bool> named_streams;
  std::optional<bool> sparse_files;
  std::optional<bool> hard_links;
  std::optional<bool> reparse_points;
  std::optional<bool> persistent_acls;
  std::optional<bool> extended_attributes;
  std::optional<bool> case_preserved_names;
  std::optional<bool> case_sensitive_dir;
  std::optional<bool> file_compression;
  std::optional<bool> object_ids;
  std::optional<bool> open_by_file_id;
  std::optional<bool> posix_unlink_rename;
  std::optional<bool> short_names;
  std::optional<bool> can_create_symlinks;
  std::optional<bool> oplocks;
  std::optional<bool> change_notifications;
  /// Overrides the sector alignment used by `FILE_FLAG_NO_BUFFERING` tests.
  std::optional<uint32_t> sector_size;
};

/// \brief Retry and timeout parameters used by `NtfsTestBase` and asynchronous tests.
struct TimingConfig {
  /// Maximum retry count for per-test temp-directory cleanup.
  uint32_t cleanup_retries = 5;
  /// Initial exponential backoff delay in milliseconds between cleanup retries.
  uint32_t cleanup_initial_backoff_ms = 10;
  /// Default timeout in milliseconds for async/overlapped/notification waits.
  uint32_t async_timeout_ms = 5000;
};

/// \brief Identifies the currently executing GoogleTest test case for lifecycle hooks.
struct TestLifecycleInfo {
  std::string suite_name;
  std::string test_name;
  /// Fully qualified `"<SuiteName>.<TestName>"` identifier.
  std::string full_name;
};

/// \brief Embedder-supplied lifecycle callbacks invoked by `NtfsTestBase`.
///
/// `dokan_fstest` itself installs `before_test` / `after_test` as a
/// mount-liveness guard: once the target root becomes unreachable mid-run it
/// aborts with `"mount lost after <last test>"` instead of failing every
/// remaining test.
struct LifecycleHooks {
  /// Runs once per test suite in `NtfsTestBase::SetUpTestSuite()`.
  ///
  /// Embedders may mount an in-process filesystem and optionally set
  /// `*active_root_out` to override `TestConfig::root_path` for this suite.
  /// Returning `false` marks suite setup as failed and causes `SetUp()` to
  /// immediately `FAIL()` every test in the suite (recording
  /// `fstest_fatal_setup = "1"`) without creating temp dirs or invoking
  /// `before_test` / `after_test`.
  std::function<bool(std::string_view suite_name, std::wstring *active_root_out,
                     std::string *error_out)>
      before_suite;

  /// Runs once per test suite in `NtfsTestBase::TearDownTestSuite()` whenever
  /// `before_suite` is unset or returned `true`.
  std::function<void(std::string_view suite_name)> after_suite;

  /// Runs in `NtfsTestBase::SetUp()` before creating the per-test
  /// `ScopedTempDir`. Returning `false` fails the test immediately and records
  /// `fstest_fatal_setup = "1"` (never masked as XFAIL).
  std::function<bool(const TestLifecycleInfo &info, std::string *error_out)>
      before_test;

  /// Runs in `NtfsTestBase::TearDown()` after the `ScopedTempDir` zero-debris
  /// check whenever suite setup succeeded (neither failed nor skipped) and
  /// `before_test` is unset or returned `true`. Returning `false` records
  /// `fstest_fatal_teardown = "1"` (never masked as XFAIL).
  std::function<bool(const TestLifecycleInfo &info, bool test_passed,
                     std::string *error_out)>
      after_test;
};

/// \brief Process-wide configuration consumed by `NtfsTestBase` and all suite tests.
///
/// Layer 2/3 invariant: `TestConfig` deliberately has NO "strict" mode boolean.
/// Tests in `suite` always assert the exact same NTFS contract regardless of
/// whether Layer 3 (`cli`) runs in `--strict` or `--baseline` mode.
struct TestConfig {
  /// Target filesystem root path (fallback: `FSTEST_ROOT` env variable).
  std::wstring root_path;
  /// Optional override for volume-root detection (`nullopt` = auto-detect).
  std::optional<bool> is_volume_root;
  /// Short prefix for per-test isolated subdirectories.
  std::wstring temp_prefix = L"fst";
  /// Number of entries created by the `LargeDirectory` enumeration test.
  uint32_t large_dir_entry_count = 600;
  /// When `false` (default), Category 4b capability checks run unconditionally
  /// to enforce strict NTFS expectations unless explicitly disabled in
  /// `capability_overrides`.
  bool respect_fs_capabilities = false;
  /// Optional pre-probed volume capabilities to avoid a duplicate probe.
  std::optional<core::VolumeCapabilities> preprobed_capabilities;
  /// Explicit per-capability overrides taking precedence over probed flags.
  CapabilityOverrides capability_overrides;
  /// Cleanup retry and async timeout parameters.
  TimingConfig timing;
  /// Optional embedder suite/test lifecycle hooks.
  LifecycleHooks hooks;
};

/// \brief Installs a new process-wide `TestConfig`.
void ConfigureSuite(TestConfig config);

/// \brief Returns a const reference to the active process-wide `TestConfig`.
const TestConfig &GetSuiteConfig();

/// \brief Resets the process-wide `TestConfig` to defaults.
void ResetSuiteConfig();

} // namespace fstest
