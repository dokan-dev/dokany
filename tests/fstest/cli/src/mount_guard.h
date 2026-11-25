// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include "fstest/core/win_headers.h"
#include "fstest/suite/test_config.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fstest::cli {

// Deterministic fault injection used only by fstest_self_test end-to-end tests
// (selected via the FSTEST_SELFTEST_FAULT environment variable). Production
// runs never set it.
enum class SelfTestFault {
  kNone,
  // before_test hook fails for every test (=> fatal setup).
  kBeforeTest,
  // after_test hook fails for every test (=> fatal teardown).
  kAfterTest,
  // Root probe reports the mount as lost once one test has completed.
  kMountLost,
};

// Parses "", "before_test", "after_test", or "mount_lost". Returns false for
// any other value.
bool ParseSelfTestFault(std::string_view value, SelfTestFault *out);

// Mount-liveness guard installed by dokan_fstest as LifecycleHooks
// before_test / after_test.
//
// Before every test it re-probes the target root. Once the root becomes
// unreachable (e.g. the filesystem under test crashed or was unmounted) it
// records "mount lost after <last completed test>", fails the current test
// (fatal setup, never masked as XFAIL), and invokes `on_mount_lost` so the
// caller can stop the run instead of failing every remaining test.
class MountLivenessGuard {
public:
  // Returns ERROR_SUCCESS while the root is a reachable directory, otherwise
  // the Win32 error describing why it is not (see core::ProbeRootDirectory).
  using RootProbe = std::function<DWORD()>;

  MountLivenessGuard(
      RootProbe probe, std::string root_display,
      std::function<void()> on_mount_lost = {},
      SelfTestFault fault = SelfTestFault::kNone,
      std::function<void(const std::string &)> on_skip_remaining = {});

  MountLivenessGuard(const MountLivenessGuard &) = delete;
  MountLivenessGuard &operator=(const MountLivenessGuard &) = delete;

  // Installs before_suite / before_test / after_test hooks referencing this
  // guard (which must outlive every test run using `hooks`). Other hooks are
  // left untouched.
  void Install(LifecycleHooks *hooks);

  bool BeforeSuite(std::string_view suite_name, std::string *error_out);
  bool BeforeTest(const TestLifecycleInfo &info, std::string *error_out);
  bool AfterTest(const TestLifecycleInfo &info, bool test_passed,
                 std::string *error_out);

  bool mount_lost() const noexcept { return mount_lost_; }
  // "mount lost after <test>: root '<root>' is not reachable (<error>)".
  const std::string &lost_message() const noexcept { return lost_message_; }
  const std::string &last_completed_test() const noexcept {
    return last_completed_test_;
  }
  uint32_t completed_tests() const noexcept { return completed_tests_; }

private:
  void RecordMountLost(DWORD err, std::string *error_out);
  RootProbe probe_;
  std::string root_display_;
  std::function<void()> on_mount_lost_;
  std::function<void(const std::string &)> on_skip_remaining_;
  SelfTestFault fault_ = SelfTestFault::kNone;
  bool mount_lost_ = false;
  std::string lost_message_;
  std::string last_completed_test_;
  uint32_t completed_tests_ = 0;
};

} // namespace fstest::cli
