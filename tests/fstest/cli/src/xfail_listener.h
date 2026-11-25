// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include "baseline.h"
#include "scorecard.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {

// Collects all compiled/registered GoogleTest test IDs ("Suite.Test") from
// unit_test, ignoring --gtest_filter / should_run() state.
std::unordered_set<std::string>
CollectRegisteredTestIds(const ::testing::UnitTest *unit_test);

// Same as CollectRegisteredTestIds, preserving GoogleTest registration order
// (the order sharding assigns tests in).
std::vector<std::string>
CollectRegisteredTestIdsInOrder(const ::testing::UnitTest *unit_test);

// Assembles a human-readable skip reason with an optional "[category]" prefix.
std::string FormatSkipReason(std::string_view skip_category,
                             std::string_view raw_skip_reason);

// Test name of the synthetic record capturing failures recorded on a
// TestSuite's ad-hoc result (SetUpTestSuite / TearDownTestSuite), reported as
// "<Suite>.<suite-level>".
inline constexpr std::string_view kSuiteLevelTestName = "<suite-level>";
// Synthetic "<program>.<program-level>" record capturing failures recorded on
// the UnitTest's ad-hoc result (global Environment SetUp/TearDown, assertions
// outside any test).
inline constexpr std::string_view kProgramLevelSuiteName = "<program>";
inline constexpr std::string_view kProgramLevelTestName = "<program-level>";

// Pure data snapshot of a completed test case for classification, console
// formatting, and scorecard capture.
struct TestEndEvent {
  std::string test_suite_name;
  std::string test_name;
  RawTestOutcome raw_outcome = RawTestOutcome::kPass;
  bool fatal_teardown = false;
  // kFatalSetupProperty == "1": NtfsTestBase::SetUp() failed.
  bool fatal_setup = false;
  // A failure part reports an SEH or C++ exception caught by GoogleTest.
  bool exception = false;
  // Synthetic suite-/program-level record (see kSuiteLevelTestName).
  bool synthetic = false;
  int64_t duration_ms = 0;
  std::string skip_category;
  std::string raw_skip_reason;
  std::vector<std::string> failure_messages;
};

// Returns true if a GoogleTest failure message reports an SEH exception
// ("SEH exception with code ...") or a C++ exception ("C++ exception with
// description ..." / "Unknown C++ exception thrown in ...").
bool IsExceptionFailureMessage(std::string_view message);

// Fills raw_outcome, duration-independent flags (fatal_setup, fatal_teardown,
// exception), skip category/reason, and failure messages of *event from the
// part results and properties of one test (mirrors TestResult semantics:
// any failed part => kFail, else any skipped part => kSkip, else kPass).
void PopulateEventFromParts(const std::vector<::testing::TestPartResult> &parts,
                            const std::vector<::testing::TestProperty> &props,
                            TestEndEvent *event);

// Custom GoogleTest listener replacing the default console printer while
// preserving XML/JSON output listeners. Executes every non-filtered test,
// enforces unmaskable FAIL classification for fstest_fatal_setup="1",
// fstest_fatal_teardown="1", SEH/C++ exceptions, and suite-/program-level
// (outside any test body) failures, keeps
// RAW_SKIP from ever becoming XPASS, aggregates worst outcomes across
// --gtest_repeat iterations, and suppresses expected XFAIL noise unless
// verbose is enabled.
class XfailEventListener : public ::testing::EmptyTestEventListener {
public:
  XfailEventListener(Scorecard *scorecard,
                     const std::map<std::string, XfailEntry> *active_xfails,
                     bool verbose, std::ostream *out = &std::cout);

  void OnTestStart(const ::testing::TestInfo &test_info) override;
  void OnTestEnd(const ::testing::TestInfo &test_info) override;
  // Records "<Suite>.<suite-level>" FAIL when the suite's ad-hoc result failed.
  void OnTestSuiteEnd(const ::testing::TestSuite &test_suite) override;
  // Records "<program>.<program-level>" FAIL when the UnitTest's ad-hoc result
  // failed.
  void OnTestProgramEnd(const ::testing::UnitTest &unit_test) override;

  // Testable entry points invoked directly by the GoogleTest callbacks.
  void HandleTestStart(std::string_view test_suite_name,
                       std::string_view test_name);
  void HandleTestEnd(const TestEndEvent &event);
  // Records an unmaskable synthetic FAIL "<suite_name>.<test_name>" carrying
  // failure_messages (no-op when failure_messages is empty).
  void HandleOutOfTestFailure(std::string_view suite_name,
                              std::string_view test_name,
                              const std::vector<std::string> &failure_messages);

private:
  Scorecard *scorecard_ = nullptr;
  const std::map<std::string, XfailEntry> *active_xfails_ = nullptr;
  bool verbose_ = false;
  std::ostream *out_ = nullptr;
};

} // namespace fstest::cli
