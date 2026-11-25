// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "xfail_listener.h"

#include "fstest/suite/test_metadata.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest::cli {
namespace {

std::string_view SafeStr(const char *s) noexcept {
  return (s != nullptr) ? std::string_view(s) : std::string_view{};
}

std::string_view FirstNonEmpty(const char *primary,
                               const char *fallback) noexcept {
  const std::string_view p = SafeStr(primary);
  return !p.empty() ? p : SafeStr(fallback);
}

std::string MakeCanonicalTestId(std::string_view suite_name,
                                std::string_view test_name) {
  std::string id;
  id.reserve(suite_name.size() + 1 + test_name.size());
  id.append(suite_name);
  id.push_back('.');
  id.append(test_name);
  return id;
}

std::string MakeCanonicalTestId(const ::testing::TestInfo &test_info) {
  return MakeCanonicalTestId(SafeStr(test_info.test_suite_name()),
                             SafeStr(test_info.name()));
}

void PrintIndentedLines(std::ostream &out, std::string_view text) {
  std::istringstream iss{std::string(text)};
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    out << "    " << line << "\n";
  }
}

std::string_view OutcomeConsoleTag(TestOutcome outcome) noexcept {
  switch (outcome) {
  case TestOutcome::kPass:
    return "[  PASS  ]";
  case TestOutcome::kSkip:
    return "[  SKIP  ]";
  case TestOutcome::kXFail:
    return "[  XFAIL ]";
  case TestOutcome::kXPassAllowed:
    return "[ XPASS* ]";
  case TestOutcome::kXPass:
    return "[ XPASS! ]";
  case TestOutcome::kFail:
    return "[  FAIL  ]";
  }
  return "[  FAIL  ]";
}

std::string FormatFailurePart(const ::testing::TestPartResult &part) {
  std::ostringstream msg;
  if (part.file_name() != nullptr) {
    msg << part.file_name() << ":" << part.line_number() << ": ";
  }
  msg << FirstNonEmpty(part.message(), part.summary());
  return msg.str();
}

std::vector<std::string>
CollectFailureMessages(const ::testing::TestResult &result) {
  std::vector<std::string> messages;
  const int part_count = result.total_part_count();
  for (int i = 0; i < part_count; ++i) {
    const ::testing::TestPartResult &part = result.GetTestPartResult(i);
    if (part.failed()) {
      messages.push_back(FormatFailurePart(part));
    }
  }
  return messages;
}

void PopulateEventFromResult(const ::testing::TestResult &result,
                             TestEndEvent *event) {
  event->duration_ms = static_cast<int64_t>(result.elapsed_time());

  std::vector<::testing::TestPartResult> parts;
  const int part_count = result.total_part_count();
  parts.reserve(static_cast<std::size_t>(part_count > 0 ? part_count : 0));
  for (int i = 0; i < part_count; ++i) {
    parts.push_back(result.GetTestPartResult(i));
  }
  std::vector<::testing::TestProperty> props;
  const int prop_count = result.test_property_count();
  props.reserve(static_cast<std::size_t>(prop_count > 0 ? prop_count : 0));
  for (int i = 0; i < prop_count; ++i) {
    props.push_back(result.GetTestProperty(i));
  }
  PopulateEventFromParts(parts, props, event);
}

void AppendOutcomeDetail(std::ostream &out, TestOutcome outcome,
                         const TestEndEvent &event,
                         std::string_view skip_reason,
                         std::string_view xfail_reason) {
  auto append_if_non_empty = [&](std::string_view prefix,
                                 std::string_view detail) {
    if (!detail.empty()) {
      out << prefix << detail;
    }
  };
  switch (outcome) {
  case TestOutcome::kPass:
    break;
  case TestOutcome::kSkip:
    append_if_non_empty(" - ", skip_reason);
    break;
  case TestOutcome::kXFail:
    append_if_non_empty(" - ", xfail_reason);
    break;
  case TestOutcome::kXPassAllowed:
    append_if_non_empty(" - allow_xpass: ", xfail_reason);
    break;
  case TestOutcome::kXPass:
    append_if_non_empty(" - Baseline reason: ", xfail_reason);
    break;
  case TestOutcome::kFail:
    if (event.fatal_setup) {
      out << " (fatal setup)";
    }
    if (event.fatal_teardown) {
      out << " (fatal teardown)";
    }
    if (event.exception) {
      out << " (exception)";
    }
    if (event.synthetic) {
      out << " (outside test body)";
    }
    break;
  }
}

void WriteTestEndOutput(std::ostream &out, const std::string &test_id,
                        TestOutcome outcome, const TestEndEvent &event,
                        std::string_view skip_reason,
                        std::string_view xfail_reason, bool verbose) {
  out << OutcomeConsoleTag(outcome) << " " << test_id << " ("
      << event.duration_ms << " ms)";
  AppendOutcomeDetail(out, outcome, event, skip_reason, xfail_reason);
  out << "\n";

  if (outcome == TestOutcome::kFail ||
      (outcome == TestOutcome::kXFail && verbose)) {
    for (const std::string &msg : event.failure_messages) {
      PrintIndentedLines(out, msg);
    }
  }
  out.flush();
}

} // namespace

bool IsExceptionFailureMessage(std::string_view message) {
  return message.find("SEH exception with code") != std::string_view::npos ||
         message.find("C++ exception with description") !=
             std::string_view::npos ||
         message.find("Unknown C++ exception thrown in") !=
             std::string_view::npos;
}

void PopulateEventFromParts(const std::vector<::testing::TestPartResult> &parts,
                            const std::vector<::testing::TestProperty> &props,
                            TestEndEvent *event) {
  for (const ::testing::TestProperty &prop : props) {
    const std::string_view key = SafeStr(prop.key());
    const std::string_view val = SafeStr(prop.value());
    if (key == fstest::kFatalTeardownProperty && val == "1") {
      event->fatal_teardown = true;
    } else if (key == fstest::kFatalSetupProperty && val == "1") {
      event->fatal_setup = true;
    } else if (key == fstest::kSkipCategoryProperty) {
      event->skip_category = std::string(val);
    }
  }

  bool any_failed = false;
  bool any_skipped = false;
  for (const ::testing::TestPartResult &part : parts) {
    if (part.failed()) {
      any_failed = true;
      if (IsExceptionFailureMessage(SafeStr(part.message()))) {
        event->exception = true;
      }
      event->failure_messages.push_back(FormatFailurePart(part));
    } else if (part.skipped()) {
      any_skipped = true;
      if (event->raw_skip_reason.empty()) {
        event->raw_skip_reason =
            std::string(FirstNonEmpty(part.summary(), part.message()));
      }
    }
  }

  if (any_failed) {
    event->raw_outcome = RawTestOutcome::kFail;
  } else if (any_skipped) {
    event->raw_outcome = RawTestOutcome::kSkip;
  } else {
    event->raw_outcome = RawTestOutcome::kPass;
  }
}

std::vector<std::string>
CollectRegisteredTestIdsInOrder(const ::testing::UnitTest *unit_test) {
  std::vector<std::string> ids;
  if (unit_test == nullptr) {
    return ids;
  }
  const int suite_count = unit_test->total_test_suite_count();
  for (int s = 0; s < suite_count; ++s) {
    const ::testing::TestSuite *suite = unit_test->GetTestSuite(s);
    if (suite == nullptr) {
      continue;
    }
    const int test_count = suite->total_test_count();
    for (int t = 0; t < test_count; ++t) {
      const ::testing::TestInfo *info = suite->GetTestInfo(t);
      if (info != nullptr) {
        ids.push_back(MakeCanonicalTestId(*info));
      }
    }
  }
  return ids;
}

std::unordered_set<std::string>
CollectRegisteredTestIds(const ::testing::UnitTest *unit_test) {
  const std::vector<std::string> ordered =
      CollectRegisteredTestIdsInOrder(unit_test);
  return std::unordered_set<std::string>(ordered.begin(), ordered.end());
}

std::string FormatSkipReason(std::string_view skip_category,
                             std::string_view raw_skip_reason) {
  if (skip_category.empty()) {
    return std::string(raw_skip_reason);
  }
  std::string out;
  out.reserve(skip_category.size() + raw_skip_reason.size() + 3);
  out.push_back('[');
  out.append(skip_category);
  out.push_back(']');
  if (!raw_skip_reason.empty()) {
    out.push_back(' ');
    out.append(raw_skip_reason);
  }
  return out;
}

XfailEventListener::XfailEventListener(
    Scorecard *scorecard,
    const std::map<std::string, XfailEntry> *active_xfails, bool verbose,
    std::ostream *out)
    : scorecard_(scorecard), active_xfails_(active_xfails), verbose_(verbose),
      out_(out) {}

void XfailEventListener::OnTestStart(const ::testing::TestInfo &test_info) {
  HandleTestStart(SafeStr(test_info.test_suite_name()),
                  SafeStr(test_info.name()));
}

void XfailEventListener::HandleTestStart(std::string_view test_suite_name,
                                         std::string_view test_name) {
  if (out_ == nullptr) {
    return;
  }
  *out_ << "[ RUN    ] " << MakeCanonicalTestId(test_suite_name, test_name)
        << "\n";
  out_->flush();
}

void XfailEventListener::OnTestEnd(const ::testing::TestInfo &test_info) {
  TestEndEvent event;
  event.test_suite_name = std::string(SafeStr(test_info.test_suite_name()));
  event.test_name = std::string(SafeStr(test_info.name()));
  if (const ::testing::TestResult *result = test_info.result();
      result != nullptr) {
    PopulateEventFromResult(*result, &event);
  }
  HandleTestEnd(event);
}

void XfailEventListener::OnTestSuiteEnd(
    const ::testing::TestSuite &test_suite) {
  const ::testing::TestResult &ad_hoc = test_suite.ad_hoc_test_result();
  if (!ad_hoc.Failed()) {
    return;
  }
  HandleOutOfTestFailure(SafeStr(test_suite.name()), kSuiteLevelTestName,
                         CollectFailureMessages(ad_hoc));
}

void XfailEventListener::OnTestProgramEnd(
    const ::testing::UnitTest &unit_test) {
  const ::testing::TestResult &ad_hoc = unit_test.ad_hoc_test_result();
  if (!ad_hoc.Failed()) {
    return;
  }
  HandleOutOfTestFailure(kProgramLevelSuiteName, kProgramLevelTestName,
                         CollectFailureMessages(ad_hoc));
}

void XfailEventListener::HandleOutOfTestFailure(
    std::string_view suite_name, std::string_view test_name,
    const std::vector<std::string> &failure_messages) {
  if (failure_messages.empty()) {
    return;
  }
  TestEndEvent event;
  event.test_suite_name = std::string(suite_name);
  event.test_name = std::string(test_name);
  event.raw_outcome = RawTestOutcome::kFail;
  event.synthetic = true;
  event.exception = std::any_of(
      failure_messages.begin(), failure_messages.end(),
      [](const std::string &msg) { return IsExceptionFailureMessage(msg); });
  event.failure_messages = failure_messages;
  HandleTestEnd(event);
}

void XfailEventListener::HandleTestEnd(const TestEndEvent &event) {
  const std::string test_id =
      MakeCanonicalTestId(event.test_suite_name, event.test_name);
  const std::string domain = fstest::SuiteNameToDomainId(event.test_suite_name);
  std::string skip_reason =
      FormatSkipReason(event.skip_category, event.raw_skip_reason);

  const XfailEntry *active_entry = nullptr;
  if (active_xfails_ != nullptr) {
    const auto it = active_xfails_->find(test_id);
    if (it != active_xfails_->end()) {
      active_entry = &it->second;
    }
  }

  const bool unmaskable = event.fatal_teardown || event.fatal_setup ||
                          event.exception || event.synthetic;
  const TestOutcome outcome =
      ClassifyOutcome(event.raw_outcome, unmaskable, active_entry);
  // Never attribute a baseline reason to an unmaskable failure.
  const std::string_view xfail_reason =
      (active_entry != nullptr && !unmaskable)
          ? std::string_view(active_entry->reason)
          : std::string_view{};

  if (out_ != nullptr) {
    WriteTestEndOutput(*out_, test_id, outcome, event, skip_reason,
                       xfail_reason, verbose_);
  }

  if (scorecard_ != nullptr) {
    TestRecord record;
    record.id = test_id;
    record.domain = domain;
    record.outcome = outcome;
    record.fatal_teardown = event.fatal_teardown;
    record.fatal_setup = event.fatal_setup;
    record.exception = event.exception;
    record.synthetic = event.synthetic;
    record.duration_ms = event.duration_ms;
    record.xfail_reason = std::string(xfail_reason);
    record.skip_reason = std::move(skip_reason);
    record.failure_messages = event.failure_messages;
    record.iterations = 1;
    scorecard_->RecordTest(std::move(record));
  }
}

} // namespace fstest::cli
