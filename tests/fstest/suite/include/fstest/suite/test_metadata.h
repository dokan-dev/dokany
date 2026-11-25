// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file test_metadata.h
/// \brief Scorecard domain mapping and GTest `TestResult` property keys shared
/// between Layer 2 (`suite`) and Layer 3 (`cli`).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fstest {

/// \brief GTest `TestResult` property key recorded as `"1"` on fatal teardown
/// errors (e.g. temp-dir debris or `after_test` failure). Shared between
/// Layer 2 (`suite`) and Layer 3 (`cli`) so fatal teardowns are never masked as
/// XFAIL.
inline constexpr const char *kFatalTeardownProperty = "fstest_fatal_teardown";

/// \brief GTest `TestResult` property key recorded as `"1"` when
/// `NtfsTestBase::SetUp()` fails before the test body runs (suite setup
/// failure, `before_test` hook failure, or per-test temp directory creation
/// failure). Layer 3 (`cli`) classifies such tests as unmaskable `FAIL`: a
/// baseline XFAIL never hides a broken harness or a lost mount.
inline constexpr const char *kFatalSetupProperty = "fstest_fatal_setup";

/// \brief GTest `TestResult` property key recording the skip category emitted
/// by precondition gating macros and in-test policy gates (`volume_root`,
/// `local_volume`, `os_build`, `privilege`, `os_feature`, `os_policy`,
/// `volume_policy`, `fs_capability`).
inline constexpr const char *kSkipCategoryProperty = "skip_category";

/// \brief Mapping between a canonical scorecard domain identifier and its GTest
/// suite.
struct DomainInfo {
  /// Stable snake_case domain identifier used in baselines and scorecards.
  std::string_view domain_id;
  /// Corresponding GoogleTest test suite name (e.g. `"CreateOpenTest"`).
  std::string_view suite_name;
};

/// \brief Returns all 12 canonical NTFS conformance domains in display order.
const std::vector<DomainInfo> &GetAllDomains();

/// \brief Maps a GTest suite name (e.g. `"CreateOpenTest"`) to its canonical
/// scorecard domain ID (e.g. `"create_open"`). Falls back to `suite_name` for
/// custom suites.
std::string SuiteNameToDomainId(std::string_view suite_name);

/// \brief Returns `true` if `suite_name` is one of the 12 built-in conformance
/// suites.
bool IsKnownSuiteName(std::string_view suite_name);

} // namespace fstest
