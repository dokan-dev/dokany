// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "cli_args.h"
#include "json_util.h"
#include "scorecard.h"
#include "test_support.h"
#include "xfail_listener.h"

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {
namespace {

TestRecord MakeTestRecord(std::string id, std::string domain,
                          TestOutcome outcome, int64_t duration_ms,
                          bool fatal_teardown = false,
                          std::string xfail_reason = {},
                          std::string skip_reason = {},
                          std::vector<std::string> failures = {}) {
  TestRecord r;
  r.id = std::move(id);
  r.domain = std::move(domain);
  r.outcome = outcome;
  r.fatal_teardown = fatal_teardown;
  r.duration_ms = duration_ms;
  r.xfail_reason = std::move(xfail_reason);
  r.skip_reason = std::move(skip_reason);
  r.failure_messages = std::move(failures);
  return r;
}

TEST(ScorecardTest, ClassificationStateMachine) {
  XfailEntry regular_xfail;
  regular_xfail.reason = "known bug";
  regular_xfail.allow_xpass = false;

  XfailEntry allowed_xfail;
  allowed_xfail.reason = "intermittent race";
  allowed_xfail.allow_xpass = true;

  // Without baseline
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kPass, false, nullptr),
            TestOutcome::kPass);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kFail, false, nullptr),
            TestOutcome::kFail);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kSkip, false, nullptr),
            TestOutcome::kSkip);

  // With regular baseline entry
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kFail, false, &regular_xfail),
            TestOutcome::kXFail);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kPass, false, &regular_xfail),
            TestOutcome::kXPass);
  // RAW_SKIP on baselined test is ALWAYS SKIP, never XPASS!
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kSkip, false, &regular_xfail),
            TestOutcome::kSkip);

  // With allow_xpass: true entry
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kFail, false, &allowed_xfail),
            TestOutcome::kXFail);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kPass, false, &allowed_xfail),
            TestOutcome::kXPassAllowed);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kSkip, false, &allowed_xfail),
            TestOutcome::kSkip);

  // fstest_fatal_teardown == "1" is NEVER masked as XFAIL or SKIP!
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kPass, true, nullptr),
            TestOutcome::kFail);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kFail, true, &regular_xfail),
            TestOutcome::kFail);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kPass, true, &allowed_xfail),
            TestOutcome::kFail);
  EXPECT_EQ(ClassifyOutcome(RawTestOutcome::kSkip, true, &regular_xfail),
            TestOutcome::kFail);
}

TEST(ScorecardTest, RepeatWorstOutcomeAggregation) {
  // Severity order: FAIL > XPASS > XFAIL > XPASS_ALLOWED > PASS > SKIP
  EXPECT_GT(OutcomeSeverity(TestOutcome::kFail),
            OutcomeSeverity(TestOutcome::kXPass));
  EXPECT_GT(OutcomeSeverity(TestOutcome::kXPass),
            OutcomeSeverity(TestOutcome::kXFail));
  EXPECT_GT(OutcomeSeverity(TestOutcome::kXFail),
            OutcomeSeverity(TestOutcome::kXPassAllowed));
  EXPECT_GT(OutcomeSeverity(TestOutcome::kXPassAllowed),
            OutcomeSeverity(TestOutcome::kPass));
  EXPECT_GT(OutcomeSeverity(TestOutcome::kPass),
            OutcomeSeverity(TestOutcome::kSkip));

  Scorecard sc;
  sc.RecordTest(
      MakeTestRecord("DataIoTest.Flaky", "data_io", TestOutcome::kPass, 5));
  sc.RecordTest(MakeTestRecord("DataIoTest.Flaky", "data_io",
                               TestOutcome::kXFail, 7, false, "known", "",
                               {"iter 2 failed"}));
  sc.RecordTest(MakeTestRecord("DataIoTest.Flaky", "data_io",
                               TestOutcome::kFail, 11, true, "", "",
                               {"iter 3 teardown debris"}));
  sc.RecordTest(
      MakeTestRecord("DataIoTest.Flaky", "data_io", TestOutcome::kPass, 4));

  const std::vector<TestRecord> tests = sc.Tests();
  ASSERT_EQ(tests.size(), 1u);
  EXPECT_EQ(tests[0].outcome, TestOutcome::kFail);
  EXPECT_TRUE(tests[0].fatal_teardown);
  EXPECT_EQ(tests[0].duration_ms, 27);
  EXPECT_EQ(tests[0].iterations, 4u);
  ASSERT_EQ(tests[0].failure_messages.size(), 2u);
  EXPECT_EQ(tests[0].failure_messages[0], "iter 2 failed");
  EXPECT_EQ(tests[0].failure_messages[1], "iter 3 teardown debris");

  // Skip followed by pass/fail must clear stale skip_reason.
  Scorecard skip_upgrade;
  skip_upgrade.RecordTest(MakeTestRecord("StreamsTest.SkipThenPass", "streams",
                                         TestOutcome::kSkip, 1, false, "",
                                         "[env] skipped on iter 1"));
  skip_upgrade.RecordTest(MakeTestRecord("StreamsTest.SkipThenPass", "streams",
                                         TestOutcome::kPass, 3));
  ASSERT_EQ(skip_upgrade.Tests().size(), 1u);
  EXPECT_EQ(skip_upgrade.Tests()[0].outcome, TestOutcome::kPass);
  EXPECT_EQ(skip_upgrade.Tests()[0].iterations, 2u);
  EXPECT_TRUE(skip_upgrade.Tests()[0].skip_reason.empty());

  // Repeated skips preserve skip_reason.
  Scorecard all_skip;
  all_skip.RecordTest(MakeTestRecord("StreamsTest.AlwaysSkip", "streams",
                                     TestOutcome::kSkip, 0, false, "",
                                     "[env] kept reason"));
  all_skip.RecordTest(MakeTestRecord("StreamsTest.AlwaysSkip", "streams",
                                     TestOutcome::kSkip, 0));
  ASSERT_EQ(all_skip.Tests().size(), 1u);
  EXPECT_EQ(all_skip.Tests()[0].outcome, TestOutcome::kSkip);
  EXPECT_EQ(all_skip.Tests()[0].skip_reason, "[env] kept reason");

  // BuildScorecardJson emits iterations count.
  ScorecardMetadata meta;
  const nlohmann::ordered_json json = BuildScorecardJson(meta, sc);
  ASSERT_EQ(json["tests"].size(), 1u);
  EXPECT_EQ(json["tests"][0]["iterations"].get<uint32_t>(), 4u);
}

TEST(ScorecardTest, ExitCodesAndPrecedence) {
  // 0 tests executed -> exit 2
  DomainCounts empty;
  EXPECT_EQ(ComputeExitCode(empty, XpassPolicy::kFail), 2);
  EXPECT_EQ(ComputeExitCode(empty, XpassPolicy::kWarn), 2);

  // Clean pass (including xfail, skip, xpass_allowed) -> exit 0
  DomainCounts clean;
  clean.total = 10;
  clean.pass = 6;
  clean.xfail = 2;
  clean.xpass_allowed = 1;
  clean.skip = 1;
  EXPECT_EQ(ComputeExitCode(clean, XpassPolicy::kFail), 0);

  // Disallowed XPASS -> exit 3 under kFail, exit 0 under kWarn
  DomainCounts with_xpass = clean;
  with_xpass.total += 1;
  with_xpass.xpass = 1;
  EXPECT_EQ(ComputeExitCode(with_xpass, XpassPolicy::kFail), 3);
  EXPECT_EQ(ComputeExitCode(with_xpass, XpassPolicy::kWarn), 0);

  // Both FAIL and XPASS -> exit 1 takes precedence over exit 3!
  DomainCounts both = with_xpass;
  both.total += 1;
  both.fail = 1;
  EXPECT_EQ(ComputeExitCode(both, XpassPolicy::kFail), 1);
  EXPECT_EQ(ComputeExitCode(both, XpassPolicy::kWarn), 1);

  // m3: every executed test skipped -> nothing verified -> exit 2.
  DomainCounts all_skip;
  all_skip.total = 3;
  all_skip.skip = 3;
  EXPECT_EQ(ComputeExitCode(all_skip, XpassPolicy::kFail), 2);
  EXPECT_EQ(ComputeExitCode(all_skip, XpassPolicy::kWarn), 2);

  // Skipped + crash-skipped (pre-recorded skip_crash XFAILs) only -> exit 2.
  DomainCounts skip_and_crash;
  skip_and_crash.total = 3;
  skip_and_crash.skip = 1;
  skip_and_crash.xfail = 2;
  skip_and_crash.crash_skipped = 2;
  EXPECT_EQ(ComputeExitCode(skip_and_crash, XpassPolicy::kFail), 2);

  // A single real XFAIL (actually executed) alongside skips is a valid run.
  DomainCounts one_real_xfail = skip_and_crash;
  one_real_xfail.total += 1;
  one_real_xfail.xfail += 1;
  EXPECT_EQ(ComputeExitCode(one_real_xfail, XpassPolicy::kFail), 0);

  // One pass among skips is a valid run.
  DomainCounts one_pass = all_skip;
  one_pass.total += 1;
  one_pass.pass = 1;
  EXPECT_EQ(ComputeExitCode(one_pass, XpassPolicy::kFail), 0);
}

TEST(ScorecardTest, UnmaskableFlagsPropagateToObservedResultsAndReports) {
  Scorecard sc;
  TestRecord setup;
  setup.id = "CreateOpenTest.SetupBroke";
  setup.domain = "create_open";
  setup.outcome = TestOutcome::kFail;
  setup.fatal_setup = true;
  sc.RecordTest(setup);

  TestRecord crash;
  crash.id = "DataIoTest.Crashed";
  crash.domain = "data_io";
  crash.outcome = TestOutcome::kFail;
  crash.exception = true;
  sc.RecordTest(crash);

  TestRecord suite_level;
  suite_level.id = "DeleteTest.<suite-level>";
  suite_level.domain = "delete";
  suite_level.outcome = TestOutcome::kFail;
  suite_level.synthetic = true;
  suite_level.failure_messages = {"SetUpTestSuite() threw"};
  sc.RecordTest(suite_level);

  // Repeated iteration merges flags with OR semantics.
  TestRecord setup_again = setup;
  setup_again.fatal_setup = false;
  setup_again.fatal_teardown = true;
  sc.RecordTest(setup_again);

  const std::vector<ObservedTestResult> observed = sc.ToObservedResults();
  // Synthetic suite-/program-level records are never baseline test IDs.
  ASSERT_EQ(observed.size(), 2u);
  EXPECT_EQ(observed[0].test_id, "CreateOpenTest.SetupBroke");
  EXPECT_TRUE(observed[0].failed);
  EXPECT_TRUE(observed[0].fatal_setup);
  EXPECT_TRUE(observed[0].fatal_teardown);
  EXPECT_FALSE(observed[0].exception);
  EXPECT_EQ(observed[1].test_id, "DataIoTest.Crashed");
  EXPECT_TRUE(observed[1].exception);
  EXPECT_FALSE(observed[1].fatal_setup);

  const DomainCounts summary = sc.Summary();
  EXPECT_EQ(summary.fail, 3u);
  EXPECT_EQ(ComputeExitCode(summary, XpassPolicy::kWarn), 1);

  ScorecardMetadata meta;
  meta.exit_code = 1;
  const std::string table = FormatScorecardTable(meta, sc);
  EXPECT_NE(table.find("[FAIL] CreateOpenTest.SetupBroke (fatal setup) (fatal "
                       "teardown)"),
            std::string::npos)
      << table;
  EXPECT_NE(table.find("[FAIL] DataIoTest.Crashed (exception)"),
            std::string::npos)
      << table;
  EXPECT_NE(table.find("[FAIL] DeleteTest.<suite-level>"), std::string::npos)
      << table;

  const nlohmann::ordered_json json = BuildScorecardJson(meta, sc);
  ASSERT_EQ(json["tests"].size(), 3u);
  EXPECT_TRUE(json["tests"][0]["fatal_setup"].get<bool>());
  EXPECT_TRUE(json["tests"][1]["exception"].get<bool>());
  EXPECT_TRUE(json["tests"][2]["synthetic"].get<bool>());
  EXPECT_FALSE(json["tests"][2]["fatal_setup"].get<bool>());
}

TEST(ScorecardTest, TableFormattingAndJsonReportSchema) {
  Scorecard sc;
  sc.RecordTest(MakeTestRecord("CreateOpenTest.Basic", "create_open",
                               TestOutcome::kPass, 12));
  sc.RecordTest(MakeTestRecord("DeleteTest.Posix", "delete",
                               TestOutcome::kXFail, 8, false,
                               "legacy delete-pending"));
  sc.RecordTest(MakeTestRecord("DeleteTest.Fixed", "delete",
                               TestOutcome::kXPass, 4, false, "old bug"));
  sc.RecordTest(MakeTestRecord("StreamsTest.Regression", "streams",
                               TestOutcome::kFail, 20, true, "", "",
                               {"streams_test.cpp:42: leftover debris"}));
  sc.RecordTest(MakeTestRecord("LinksReparseTest.Skipped", "links_reparse",
                               TestOutcome::kSkip, 0, false, "",
                               "[privilege] requires symlink privilege"));

  const DomainCounts summary = sc.Summary();
  ASSERT_EQ(summary.total, 5u);
  ASSERT_TRUE(summary.ScorePercent().has_value());
  // (Pass=1 + XPass=1 + XPassAllowed=0) / (Total=5 - Skip=1) = 50.0%
  EXPECT_NEAR(*summary.ScorePercent(), 50.0, 1e-6);

  ScorecardMetadata meta;
  meta.root_path = "M:\\";
  meta.mode = "baseline";
  meta.baseline_path = "baselines/dokan_mirror.json";
  meta.target = "mirror_drive";
  meta.target_arch = "x64";
  meta.xpass_policy = XpassPolicy::kFail;
  meta.exit_code = ComputeExitCode(summary, meta.xpass_policy);
  meta.duration_ms = 44;
  meta.os.major = 10;
  meta.os.minor = 0;
  meta.os.build = 26100;
  meta.os.host_arch = "x64";
  meta.os.process_arch = "x64";
  meta.capabilities.filesystem_name = "NTFS";
  meta.capabilities.bytes_per_sector = 512;
  meta.capabilities.physical_bytes_per_sector = 4096;
  meta.capabilities.bytes_per_cluster = 4096;
  meta.capabilities.is_volume_root = true;

  EXPECT_EQ(meta.exit_code, 1);

  const std::string table = FormatScorecardTable(meta, sc);
  EXPECT_NE(table.find("Domain"), std::string::npos);
  EXPECT_NE(table.find("create_open"), std::string::npos);
  EXPECT_NE(table.find("TOTAL"), std::string::npos);
  EXPECT_NE(table.find("[FAIL] StreamsTest.Regression (fatal teardown)"),
            std::string::npos);
  EXPECT_NE(table.find("[XPASS] DeleteTest.Fixed"), std::string::npos);

  // Verify every table/header row stays within 80 columns.
  {
    std::istringstream lines(table);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.find(" | ") != std::string::npos ||
          line.find("-+-") != std::string::npos ||
          line.find("====") != std::string::npos) {
        EXPECT_LE(line.size(), 80u) << "Line exceeded 80 cols: " << line;
      }
    }
  }

  // Verify JSON report schema and strict parseability.
  const nlohmann::ordered_json report = BuildScorecardJson(meta, sc);
  const std::string json_text = SerializeJsonDeterministic(report);
  nlohmann::ordered_json reparsed;
  std::string err;
  ASSERT_TRUE(ParseJsonStrict(json_text, &reparsed, &err)) << err;

  EXPECT_EQ(reparsed["schema_version"].get<int>(), 1);
  EXPECT_EQ(reparsed["mode"].get<std::string>(), "baseline");
  EXPECT_EQ(reparsed["target"].get<std::string>(), "mirror_drive");
  EXPECT_EQ(reparsed["target_arch"].get<std::string>(), "x64");
  EXPECT_EQ(reparsed["exit_code"].get<int>(), 1);
  EXPECT_EQ(reparsed["os"]["build"].get<uint32_t>(), 26100u);
  EXPECT_EQ(reparsed["capabilities"]["filesystem_name"].get<std::string>(),
            "NTFS");
  EXPECT_EQ(reparsed["summary"]["total"].get<uint32_t>(), 5u);
  EXPECT_EQ(reparsed["summary"]["fail"].get<uint32_t>(), 1u);
  EXPECT_EQ(reparsed["summary"]["xpass"].get<uint32_t>(), 1u);
  EXPECT_EQ(reparsed["summary"]["xfail"].get<uint32_t>(), 1u);
  EXPECT_EQ(reparsed["summary"]["crash_skipped"].get<uint32_t>(), 0u);
  EXPECT_EQ(reparsed["summary"]["skip"].get<uint32_t>(), 1u);
  EXPECT_TRUE(reparsed["domains"].contains("create_open"));
  EXPECT_TRUE(reparsed["domains"]["links_reparse"]["score_pct"].is_null());
  ASSERT_EQ(reparsed["tests"].size(), 5u);
  EXPECT_EQ(reparsed["tests"][0]["iterations"].get<uint32_t>(), 1u);

  // Verify skip_crash tests count in xfail + crash_skipped (staying in score
  // denominator) and render the CrashSkipped footer note.
  TestRecord crash_rec;
  crash_rec.id = "NamingPathsTest.MountRootOperations";
  crash_rec.domain = "naming_paths";
  crash_rec.outcome = TestOutcome::kXFail;
  crash_rec.skip_crash = true;
  crash_rec.xfail_reason = "daemon infinite recursion";
  sc.RecordTest(std::move(crash_rec));

  const DomainCounts with_crash = sc.Summary();
  EXPECT_EQ(with_crash.xfail, 2u);
  EXPECT_EQ(with_crash.crash_skipped, 1u);
  // Denominator is now 6 - 1 = 5; numerator is 2 -> 40.0%
  ASSERT_TRUE(with_crash.ScorePercent().has_value());
  EXPECT_NEAR(*with_crash.ScorePercent(), 40.0, 1e-6);

  const std::string crash_table = FormatScorecardTable(meta, sc);
  EXPECT_NE(crash_table.find("CrashSkipped: 1 (in XFail)"), std::string::npos);

  const nlohmann::ordered_json crash_json = BuildScorecardJson(meta, sc);
  EXPECT_EQ(crash_json["summary"]["crash_skipped"].get<uint32_t>(), 1u);
}

TEST(ScorecardTest, CollectRegisteredTestIdsFindsCompiledTests) {
  const std::unordered_set<std::string> ids =
      CollectRegisteredTestIds(::testing::UnitTest::GetInstance());
  EXPECT_TRUE(ids.contains("ScorecardTest.ClassificationStateMachine"));
  EXPECT_TRUE(ids.contains("ScorecardTest.ExitCodesAndPrecedence"));
}

TEST(ScorecardTest, WriteScorecardJsonFileHandlesInvalidUtf8WithoutThrowing) {
  test_support::ScopedTestDirectory dir(L"fstsc");
  ASSERT_TRUE(dir.ok());

  Scorecard sc;
  TestRecord rec;
  rec.id = "DataIoTest.InvalidUtf8Trace";
  rec.domain = "data_io";
  rec.outcome = TestOutcome::kFail;
  rec.failure_messages = {std::string("bad\xFF\xFE\x80utf8")};
  sc.RecordTest(std::move(rec));

  ScorecardMetadata meta;
  meta.mode = "strict";
  meta.exit_code = 1;

  const std::string out_path = dir.ChildPathUtf8("invalid_utf8_report.json");
  std::string err;
  ASSERT_TRUE(WriteScorecardJsonFile(out_path, meta, sc, &err)) << err;

  std::string raw_json;
  ASSERT_TRUE(ReadTextFileUtf8(out_path, &raw_json, &err)) << err;
  nlohmann::ordered_json parsed;
  ASSERT_TRUE(ParseJsonStrict(raw_json, &parsed, &err)) << err;
  ASSERT_EQ(parsed["tests"].size(), 1u);
  const std::string msg =
      parsed["tests"][0]["failure_messages"][0].get<std::string>();
  // Invalid bytes are replaced with U+FFFD (\xEF\xBF\xBD) rather than throwing.
  EXPECT_NE(msg.find("\xEF\xBF\xBD"), std::string::npos) << msg;
}

} // namespace
} // namespace fstest::cli
