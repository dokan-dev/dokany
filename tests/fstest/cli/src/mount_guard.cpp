// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "mount_guard.h"

#include "fstest/core/status.h"

#include <utility>

namespace fstest::cli {

bool ParseSelfTestFault(std::string_view value, SelfTestFault *out) {
  if (value.empty()) {
    *out = SelfTestFault::kNone;
  } else if (value == "before_test") {
    *out = SelfTestFault::kBeforeTest;
  } else if (value == "after_test") {
    *out = SelfTestFault::kAfterTest;
  } else if (value == "mount_lost") {
    *out = SelfTestFault::kMountLost;
  } else {
    return false;
  }
  return true;
}

MountLivenessGuard::MountLivenessGuard(
    RootProbe probe, std::string root_display,
    std::function<void()> on_mount_lost, SelfTestFault fault,
    std::function<void(const std::string &)> on_skip_remaining)
    : probe_(std::move(probe)), root_display_(std::move(root_display)),
      on_mount_lost_(std::move(on_mount_lost)),
      on_skip_remaining_(std::move(on_skip_remaining)), fault_(fault) {}

void MountLivenessGuard::Install(LifecycleHooks *hooks) {
  hooks->before_suite = [this](std::string_view suite_name,
                               std::wstring * /*override_root*/,
                               std::string *error_out) {
    return BeforeSuite(suite_name, error_out);
  };
  hooks->before_test = [this](const TestLifecycleInfo &info,
                              std::string *error_out) {
    return BeforeTest(info, error_out);
  };
  hooks->after_test = [this](const TestLifecycleInfo &info, bool test_passed,
                             std::string *error_out) {
    return AfterTest(info, test_passed, error_out);
  };
}

void MountLivenessGuard::RecordMountLost(DWORD err, std::string *error_out) {
  mount_lost_ = true;
  lost_message_ = "mount lost after " +
                  (last_completed_test_.empty() ? std::string("<start of run>")
                                                : last_completed_test_) +
                  ": root '" + root_display_ + "' is not reachable (" +
                  core::FormatWin32Error(err) + ")";
  *error_out = lost_message_;
  if (on_mount_lost_) {
    on_mount_lost_();
  }
}

bool MountLivenessGuard::BeforeSuite(std::string_view /*suite_name*/,
                                     std::string *error_out) {
  if (mount_lost_) {
    *error_out = lost_message_;
    if (on_skip_remaining_) {
      on_skip_remaining_(lost_message_);
    }
    return true;
  }
  DWORD err = probe_ ? probe_() : static_cast<DWORD>(ERROR_SUCCESS);
  if (fault_ == SelfTestFault::kMountLost && completed_tests_ > 0) {
    err = ERROR_NOT_READY;
  }
  if (err == ERROR_SUCCESS) {
    return true;
  }
  RecordMountLost(err, error_out);
  return false;
}

bool MountLivenessGuard::BeforeTest(const TestLifecycleInfo &info,
                                    std::string *error_out) {
  (void)info;
  if (mount_lost_) {
    if (on_skip_remaining_) {
      on_skip_remaining_(lost_message_);
      return true;
    }
    *error_out = lost_message_;
    return false;
  }
  if (fault_ == SelfTestFault::kBeforeTest) {
    *error_out =
        "self-test fault injection (FSTEST_SELFTEST_FAULT=before_test)";
    return false;
  }

  DWORD err = probe_ ? probe_() : static_cast<DWORD>(ERROR_SUCCESS);
  if (fault_ == SelfTestFault::kMountLost && completed_tests_ > 0) {
    err = ERROR_NOT_READY;
  }
  if (err == ERROR_SUCCESS) {
    return true;
  }

  RecordMountLost(err, error_out);
  return false;
}

bool MountLivenessGuard::AfterTest(const TestLifecycleInfo &info,
                                   bool test_passed, std::string *error_out) {
  (void)test_passed;
  last_completed_test_ = info.full_name;
  ++completed_tests_;
  if (fault_ == SelfTestFault::kAfterTest) {
    *error_out = "self-test fault injection (FSTEST_SELFTEST_FAULT=after_test)";
    return false;
  }
  return true;
}

} // namespace fstest::cli
