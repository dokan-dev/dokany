// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include "baseline.h"
#include "cli_args.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fstest::cli {

enum class RawTestOutcome {
  kPass,
  kFail,
  kSkip,
};

enum class TestOutcome {
  kPass,
  kXFail,
  kXPass,
  kXPassAllowed,
  kFail,
  kSkip,
};

// Classifies a single test execution according to the xfail state machine.
// An unmaskable failure (fatal setup, fatal teardown, SEH/C++ exception, or a
// suite-/program-level failure) ALWAYS maps to kFail regardless of baseline or
// raw status. RAW_SKIP ALWAYS maps to kSkip (never kXPass).
TestOutcome ClassifyOutcome(RawTestOutcome raw, bool unmaskable_failure,
                            const XfailEntry *active_xfail);

// Worst-outcome aggregation across --gtest_repeat=N iterations:
// FAIL > XPASS > XFAIL > XPASS_ALLOWED > PASS > SKIP
int OutcomeSeverity(TestOutcome outcome);
TestOutcome MergeRepeatedOutcome(TestOutcome existing, TestOutcome incoming);

std::string_view OutcomeToString(TestOutcome outcome);

struct TestRecord {
  std::string id;
  std::string domain;
  TestOutcome outcome = TestOutcome::kPass;
  bool fatal_teardown = false;
  // NtfsTestBase::SetUp() failed (kFatalSetupProperty) before the test body.
  bool fatal_setup = false;
  // GoogleTest caught an SEH or C++ exception escaping the test.
  bool exception = false;
  // Synthetic "<Suite>.<suite-level>" / "<program>.<program-level>" record for
  // failures outside any test body (never a baseline test ID).
  bool synthetic = false;
  bool skip_crash = false;
  int64_t duration_ms = 0;
  std::string xfail_reason;
  std::string skip_reason;
  std::vector<std::string> failure_messages;
  uint32_t iterations = 1;
};

struct DomainCounts {
  uint32_t total = 0;
  uint32_t pass = 0;
  uint32_t xfail = 0;
  uint32_t crash_skipped = 0;
  uint32_t xpass = 0;
  uint32_t xpass_allowed = 0;
  uint32_t fail = 0;
  uint32_t skip = 0;

  // (pass + xpass + xpass_allowed) / (total - skip) * 100.0, or nullopt if
  // total == skip.
  std::optional<double> ScorePercent() const;
};

struct OsReportInfo {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t build = 0;
  uint32_t ubr = 0;
  bool is_server = false;
  std::string host_arch;
  std::string process_arch;
  bool is_elevated = false;
  bool can_create_symlinks = false;
};

struct CapabilitiesReportInfo {
  std::string filesystem_name;
  uint32_t max_component_length = 0;
  uint32_t fs_flags = 0;
  uint32_t bytes_per_sector = 0;
  uint32_t physical_bytes_per_sector = 0;
  uint32_t bytes_per_cluster = 0;
  bool is_volume_root = false;
  bool short_names_enabled = false;
  bool respect_fs_capabilities = false;
};

struct ScorecardMetadata {
  std::string root_path;
  std::string mode; // "strict" or "baseline"
  std::string baseline_path;
  std::string target;
  std::string target_arch;
  XpassPolicy xpass_policy = XpassPolicy::kFail;
  int exit_code = 0;
  int64_t duration_ms = 0;
  OsReportInfo os;
  CapabilitiesReportInfo capabilities;
};

class Scorecard {
public:
  // Records one iteration of a test. Repeated calls with the same test id
  // aggregate via MergeRepeatedOutcome and retain all failure messages.
  void RecordTest(TestRecord record);

  DomainCounts Summary() const;
  std::map<std::string, DomainCounts> ByDomain() const;

  // All recorded tests in lexicographical test-id order.
  std::vector<TestRecord> Tests() const;

  // Converts recorded tests to ObservedTestResult entries for
  // --update-baseline (synthetic suite-/program-level records are omitted).
  std::vector<ObservedTestResult> ToObservedResults() const;

private:
  std::map<std::string, TestRecord> records_;
};

// Computes process exit code (0 = pass, 1 = regression/FAIL, 2 = 0 tests ran
// or every test was skipped / crash-skipped, 3 = disallowed XPASS under
// --xpass-policy=fail). Exit 1 precedes exit 3.
int ComputeExitCode(const DomainCounts &summary, XpassPolicy xpass_policy);

// Renders the 80-column ASCII scorecard table and concise FAIL/XPASS
// diagnostics for stdout.
std::string FormatScorecardTable(const ScorecardMetadata &meta,
                                 const Scorecard &scorecard);

// Builds the schema_version: 1 structured JSON conformance report.
nlohmann::ordered_json BuildScorecardJson(const ScorecardMetadata &meta,
                                          const Scorecard &scorecard);

// Writes the structured JSON report to file_path (creating parent dirs).
bool WriteScorecardJsonFile(const std::string &file_path,
                            const ScorecardMetadata &meta,
                            const Scorecard &scorecard, std::string *error_out);

} // namespace fstest::cli
