// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/suite/test_metadata.h"
#include "xfail_listener.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {
namespace {

TEST(XfailListenerTest, FormatSkipReasonCombinesCategoryAndReason) {
  EXPECT_EQ(FormatSkipReason("", ""), "");
  EXPECT_EQ(FormatSkipReason("", "requires admin"), "requires admin");
  EXPECT_EQ(FormatSkipReason("privilege", ""), "[privilege]");
  EXPECT_EQ(FormatSkipReason("privilege", "requires admin"),
            "[privilege] requires admin");
}

TEST(XfailListenerTest, FormatsAndRecordsPass) {
  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, /*active_xfails=*/nullptr, /*verbose=*/false,
                              &out);

  listener.HandleTestStart("CreateOpenTest", "BasicCreate");

  TestEndEvent ev;
  ev.test_suite_name = "CreateOpenTest";
  ev.test_name = "BasicCreate";
  ev.raw_outcome = RawTestOutcome::kPass;
  ev.duration_ms = 12;
  listener.HandleTestEnd(ev);

  EXPECT_EQ(out.str(), "[ RUN    ] CreateOpenTest.BasicCreate\n"
                       "[  PASS  ] CreateOpenTest.BasicCreate (12 ms)\n");

  const std::vector<TestRecord> tests = sc.Tests();
  ASSERT_EQ(tests.size(), 1u);
  EXPECT_EQ(tests[0].id, "CreateOpenTest.BasicCreate");
  EXPECT_EQ(tests[0].domain, "create_open");
  EXPECT_EQ(tests[0].outcome, TestOutcome::kPass);
  EXPECT_FALSE(tests[0].fatal_teardown);
  EXPECT_EQ(tests[0].duration_ms, 12);
  EXPECT_EQ(tests[0].iterations, 1u);
}

TEST(XfailListenerTest, FormatsAndRecordsFailWithIndentedMultilineOutput) {
  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, /*active_xfails=*/nullptr, /*verbose=*/false,
                              &out);

  TestEndEvent ev;
  ev.test_suite_name = "DeleteTest";
  ev.test_name = "PosixDelete";
  ev.raw_outcome = RawTestOutcome::kFail;
  ev.duration_ms = 19;
  ev.failure_messages = {
      "delete_test.cpp:42: Expected STATUS_SUCCESS\r\nActual: "
      "STATUS_ACCESS_DENIED",
      "second diagnostic",
  };
  listener.HandleTestEnd(ev);

  EXPECT_EQ(out.str(), "[  FAIL  ] DeleteTest.PosixDelete (19 ms)\n"
                       "    delete_test.cpp:42: Expected STATUS_SUCCESS\n"
                       "    Actual: STATUS_ACCESS_DENIED\n"
                       "    second diagnostic\n");

  const std::vector<TestRecord> tests = sc.Tests();
  ASSERT_EQ(tests.size(), 1u);
  EXPECT_EQ(tests[0].domain, "delete");
  EXPECT_EQ(tests[0].outcome, TestOutcome::kFail);
  ASSERT_EQ(tests[0].failure_messages.size(), 2u);
  EXPECT_EQ(tests[0].failure_messages[1], "second diagnostic");
}

TEST(XfailListenerTest, FormatsAndRecordsSkipWithAndWithoutCategory) {
  std::map<std::string, XfailEntry> active;
  active["LinksReparseTest.SymlinkCheck"].reason = "known gap";

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, &out);

  // Skip with category and reason (even on a baselined test -> stays SKIP).
  TestEndEvent with_cat;
  with_cat.test_suite_name = "LinksReparseTest";
  with_cat.test_name = "SymlinkCheck";
  with_cat.raw_outcome = RawTestOutcome::kSkip;
  with_cat.duration_ms = 1;
  with_cat.skip_category = "privilege";
  with_cat.raw_skip_reason = "requires symlink privilege";
  listener.HandleTestEnd(with_cat);

  // Skip without category, plain reason.
  TestEndEvent plain_reason;
  plain_reason.test_suite_name = "StreamsTest";
  plain_reason.test_name = "AdsCheck";
  plain_reason.raw_outcome = RawTestOutcome::kSkip;
  plain_reason.duration_ms = 0;
  plain_reason.raw_skip_reason = "unsupported on target";
  listener.HandleTestEnd(plain_reason);

  // Skip with neither category nor reason.
  TestEndEvent bare_skip;
  bare_skip.test_suite_name = "InfoQueriesTest";
  bare_skip.test_name = "BareSkip";
  bare_skip.raw_outcome = RawTestOutcome::kSkip;
  bare_skip.duration_ms = 0;
  listener.HandleTestEnd(bare_skip);

  EXPECT_EQ(out.str(),
            "[  SKIP  ] LinksReparseTest.SymlinkCheck (1 ms) - [privilege] "
            "requires symlink privilege\n"
            "[  SKIP  ] StreamsTest.AdsCheck (0 ms) - unsupported on target\n"
            "[  SKIP  ] InfoQueriesTest.BareSkip (0 ms)\n");

  const DomainCounts summary = sc.Summary();
  EXPECT_EQ(summary.total, 3u);
  EXPECT_EQ(summary.skip, 3u);
  EXPECT_EQ(summary.xpass, 0u);

  const std::vector<TestRecord> tests = sc.Tests();
  ASSERT_EQ(tests.size(), 3u);
  // Records are sorted lexicographically by id.
  EXPECT_EQ(tests[0].id, "InfoQueriesTest.BareSkip");
  EXPECT_EQ(tests[0].skip_reason, "");
  EXPECT_EQ(tests[1].id, "LinksReparseTest.SymlinkCheck");
  EXPECT_EQ(tests[1].skip_reason, "[privilege] requires symlink privilege");
  EXPECT_EQ(tests[2].id, "StreamsTest.AdsCheck");
  EXPECT_EQ(tests[2].skip_reason, "unsupported on target");
}

TEST(XfailListenerTest, XfailConciseByDefaultAndDetailedWhenVerbose) {
  std::map<std::string, XfailEntry> active;
  active["DataIoTest.SparseZero"].reason = "sparse ranges not supported";

  TestEndEvent ev;
  ev.test_suite_name = "DataIoTest";
  ev.test_name = "SparseZero";
  ev.raw_outcome = RawTestOutcome::kFail;
  ev.duration_ms = 7;
  ev.failure_messages = {"data_io_test.cpp:100: FSCTL_SET_SPARSE failed"};

  // Concise (verbose = false): suppresses failure trace lines on console,
  // while still capturing them in Scorecard.
  Scorecard sc_concise;
  std::ostringstream out_concise;
  XfailEventListener concise(&sc_concise, &active, /*verbose=*/false,
                             &out_concise);
  concise.HandleTestEnd(ev);

  EXPECT_EQ(out_concise.str(),
            "[  XFAIL ] DataIoTest.SparseZero (7 ms) - sparse ranges not "
            "supported\n");
  ASSERT_EQ(sc_concise.Tests().size(), 1u);
  EXPECT_EQ(sc_concise.Tests()[0].outcome, TestOutcome::kXFail);
  EXPECT_EQ(sc_concise.Tests()[0].xfail_reason, "sparse ranges not supported");
  ASSERT_EQ(sc_concise.Tests()[0].failure_messages.size(), 1u);

  // Verbose (verbose = true): prints indented failure trace lines.
  Scorecard sc_verbose;
  std::ostringstream out_verbose;
  XfailEventListener verbose(&sc_verbose, &active, /*verbose=*/true,
                             &out_verbose);
  verbose.HandleTestEnd(ev);

  EXPECT_EQ(out_verbose.str(),
            "[  XFAIL ] DataIoTest.SparseZero (7 ms) - sparse ranges not "
            "supported\n"
            "    data_io_test.cpp:100: FSCTL_SET_SPARSE failed\n");
  ASSERT_EQ(sc_verbose.Tests().size(), 1u);
  EXPECT_EQ(sc_verbose.Tests()[0].outcome, TestOutcome::kXFail);
}

TEST(XfailListenerTest, FormatsAndRecordsXpass) {
  std::map<std::string, XfailEntry> active;
  active["RenameMoveTest.ReplaceOpen"].reason = "previously failed";
  active["RenameMoveTest.ReplaceOpen"].allow_xpass = false;

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, &out);

  TestEndEvent ev;
  ev.test_suite_name = "RenameMoveTest";
  ev.test_name = "ReplaceOpen";
  ev.raw_outcome = RawTestOutcome::kPass;
  ev.duration_ms = 5;
  listener.HandleTestEnd(ev);

  EXPECT_EQ(out.str(),
            "[ XPASS! ] RenameMoveTest.ReplaceOpen (5 ms) - Baseline reason: "
            "previously failed\n");
  ASSERT_EQ(sc.Tests().size(), 1u);
  EXPECT_EQ(sc.Tests()[0].outcome, TestOutcome::kXPass);
  EXPECT_EQ(sc.Tests()[0].xfail_reason, "previously failed");
}

TEST(XfailListenerTest, FormatsAndRecordsXpassAllowed) {
  std::map<std::string, XfailEntry> active;
  active["LocksOplocksTest.OplockRace"].reason = "timing-sensitive race";
  active["LocksOplocksTest.OplockRace"].allow_xpass = true;

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, &out);

  TestEndEvent ev;
  ev.test_suite_name = "LocksOplocksTest";
  ev.test_name = "OplockRace";
  ev.raw_outcome = RawTestOutcome::kPass;
  ev.duration_ms = 9;
  listener.HandleTestEnd(ev);

  EXPECT_EQ(out.str(),
            "[ XPASS* ] LocksOplocksTest.OplockRace (9 ms) - allow_xpass: "
            "timing-sensitive race\n");
  ASSERT_EQ(sc.Tests().size(), 1u);
  EXPECT_EQ(sc.Tests()[0].outcome, TestOutcome::kXPassAllowed);
  EXPECT_EQ(sc.Tests()[0].xfail_reason, "timing-sensitive race");
}

TEST(XfailListenerTest, FatalTeardownOverridesXfailToFail) {
  std::map<std::string, XfailEntry> active;
  active["SecurityAclTest.RestrictiveDacl"].reason = "known ACL gap";

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, &out);

  TestEndEvent ev;
  ev.test_suite_name = "SecurityAclTest";
  ev.test_name = "RestrictiveDacl";
  ev.raw_outcome = RawTestOutcome::kFail;
  ev.fatal_teardown = true;
  ev.duration_ms = 23;
  ev.failure_messages = {"teardown failed: debris left on target"};
  listener.HandleTestEnd(ev);

  EXPECT_EQ(out.str(),
            "[  FAIL  ] SecurityAclTest.RestrictiveDacl (23 ms) (fatal "
            "teardown)\n"
            "    teardown failed: debris left on target\n");

  const std::vector<TestRecord> tests = sc.Tests();
  ASSERT_EQ(tests.size(), 1u);
  EXPECT_EQ(tests[0].outcome, TestOutcome::kFail);
  EXPECT_TRUE(tests[0].fatal_teardown);
  EXPECT_EQ(tests[0].domain, "security_acl");
}

TEST(XfailListenerTest, OnTestStartAndEndWithLiveGtestTestInfoAndNullSinks) {
  const ::testing::TestInfo *current =
      ::testing::UnitTest::GetInstance()->current_test_info();
  ASSERT_NE(current, nullptr);

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, /*active_xfails=*/nullptr, /*verbose=*/false,
                              &out);
  listener.OnTestStart(*current);
  listener.OnTestEnd(*current);

  const std::string rendered = out.str();
  EXPECT_NE(rendered.find("[ RUN    ] XfailListenerTest."
                          "OnTestStartAndEndWithLiveGtestTestInfoAndNullSinks"),
            std::string::npos)
      << rendered;
  EXPECT_NE(rendered.find("[  PASS  ] XfailListenerTest."
                          "OnTestStartAndEndWithLiveGtestTestInfoAndNullSinks"),
            std::string::npos)
      << rendered;
  ASSERT_EQ(sc.Tests().size(), 1u);
  EXPECT_EQ(sc.Tests()[0].outcome, TestOutcome::kPass);

  // Null scorecard and null output stream must be handled safely.
  XfailEventListener null_listener(/*scorecard=*/nullptr,
                                   /*active_xfails=*/nullptr, /*verbose=*/false,
                                   /*out=*/nullptr);
  null_listener.OnTestStart(*current);
  null_listener.OnTestEnd(*current);
}

TEST(XfailListenerTest, FatalSetupOverridesXfailToFail) {
  std::map<std::string, XfailEntry> active;
  active["CreateOpenTest.Masked"].reason = "known gap";

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, &out);

  TestEndEvent ev;
  ev.test_suite_name = "CreateOpenTest";
  ev.test_name = "Masked";
  ev.raw_outcome = RawTestOutcome::kFail;
  ev.fatal_setup = true;
  ev.duration_ms = 3;
  ev.failure_messages = {"before_test hook failed: mount lost after X"};
  listener.HandleTestEnd(ev);

  // The baseline reason must not be attributed to an unmaskable failure.
  EXPECT_EQ(out.str(), "[  FAIL  ] CreateOpenTest.Masked (3 ms) (fatal setup)\n"
                       "    before_test hook failed: mount lost after X\n");
  const std::vector<TestRecord> tests = sc.Tests();
  ASSERT_EQ(tests.size(), 1u);
  EXPECT_EQ(tests[0].outcome, TestOutcome::kFail);
  EXPECT_TRUE(tests[0].fatal_setup);
  EXPECT_TRUE(tests[0].xfail_reason.empty());
  EXPECT_EQ(ComputeExitCode(sc.Summary(), XpassPolicy::kWarn), 1);
}

TEST(XfailListenerTest, IsExceptionFailureMessageMatchesGtestFormats) {
  EXPECT_TRUE(IsExceptionFailureMessage(
      "SEH exception with code 0xc0000005 thrown in the test body."));
  EXPECT_TRUE(IsExceptionFailureMessage(
      "C++ exception with description \"bad_alloc\" thrown in SetUp()."));
  EXPECT_TRUE(IsExceptionFailureMessage(
      "Unknown C++ exception thrown in the test body."));
  EXPECT_FALSE(IsExceptionFailureMessage(""));
  EXPECT_FALSE(IsExceptionFailureMessage(
      "Expected equality of these values:\n  status\n  STATUS_SUCCESS"));
  // A test merely mentioning exceptions is not a crash.
  EXPECT_FALSE(IsExceptionFailureMessage("SEH exception expected but none"));
}

TEST(XfailListenerTest, PopulateEventFromRealPartResultsAndProperties) {
  using ::testing::TestPartResult;
  using ::testing::TestProperty;

  // Real failed result: non-fatal + fatal failures, no properties.
  {
    TestEndEvent ev;
    PopulateEventFromParts(
        {TestPartResult(TestPartResult::kSuccess, "a.cpp", 1, "ok"),
         TestPartResult(TestPartResult::kNonFatalFailure, "a.cpp", 10,
                        "Expected: true"),
         TestPartResult(TestPartResult::kFatalFailure, nullptr, -1,
                        "Fatal: boom")},
        {}, &ev);
    EXPECT_EQ(ev.raw_outcome, RawTestOutcome::kFail);
    EXPECT_FALSE(ev.exception);
    EXPECT_FALSE(ev.fatal_setup);
    EXPECT_FALSE(ev.fatal_teardown);
    ASSERT_EQ(ev.failure_messages.size(), 2u);
    EXPECT_EQ(ev.failure_messages[0], "a.cpp:10: Expected: true");
    EXPECT_EQ(ev.failure_messages[1], "Fatal: boom");
  }
  // Real skipped result with skip category.
  {
    TestEndEvent ev;
    PopulateEventFromParts(
        {TestPartResult(TestPartResult::kSkip, "b.cpp", 5, "needs admin"),
         TestPartResult(TestPartResult::kSkip, "b.cpp", 6, "second skip")},
        {TestProperty(fstest::kSkipCategoryProperty, "privilege")}, &ev);
    EXPECT_EQ(ev.raw_outcome, RawTestOutcome::kSkip);
    EXPECT_EQ(ev.skip_category, "privilege");
    EXPECT_EQ(ev.raw_skip_reason, "needs admin");
    EXPECT_TRUE(ev.failure_messages.empty());
  }
  // A failure after GTEST_SKIP still counts as FAIL (TestResult semantics).
  {
    TestEndEvent ev;
    PopulateEventFromParts(
        {TestPartResult(TestPartResult::kSkip, "c.cpp", 1, "skip"),
         TestPartResult(TestPartResult::kNonFatalFailure, "c.cpp", 2, "late")},
        {}, &ev);
    EXPECT_EQ(ev.raw_outcome, RawTestOutcome::kFail);
  }
  // Empty result => PASS.
  {
    TestEndEvent ev;
    PopulateEventFromParts({}, {}, &ev);
    EXPECT_EQ(ev.raw_outcome, RawTestOutcome::kPass);
  }
  // SEH and C++ exception parts are flagged as exceptions.
  for (const char *msg :
       {"SEH exception with code 0xc0000005 thrown in the test body.",
        "C++ exception with description \"vector\" thrown in the test body.",
        "Unknown C++ exception thrown in TearDown()."}) {
    TestEndEvent ev;
    PopulateEventFromParts({TestPartResult(TestPartResult::kFatalFailure,
                                           "unknown file", -1, msg)},
                           {}, &ev);
    EXPECT_EQ(ev.raw_outcome, RawTestOutcome::kFail) << msg;
    EXPECT_TRUE(ev.exception) << msg;
  }
  // fatal_setup / fatal_teardown properties are recognised only with "1".
  {
    TestEndEvent ev;
    PopulateEventFromParts(
        {TestPartResult(TestPartResult::kFatalFailure, "f.cpp", 3,
                        "Suite setup failed: root gone")},
        {TestProperty(fstest::kFatalSetupProperty, "1"),
         TestProperty(fstest::kFatalTeardownProperty, "1")},
        &ev);
    EXPECT_TRUE(ev.fatal_setup);
    EXPECT_TRUE(ev.fatal_teardown);
  }
  {
    TestEndEvent ev;
    PopulateEventFromParts({},
                           {TestProperty(fstest::kFatalSetupProperty, "0"),
                            TestProperty(fstest::kFatalTeardownProperty, "")},
                           &ev);
    EXPECT_FALSE(ev.fatal_setup);
    EXPECT_FALSE(ev.fatal_teardown);
  }
}

TEST(XfailListenerTest, ExceptionsCannotBeMaskedByXfail) {
  std::map<std::string, XfailEntry> active;
  active["DataIoTest.Crashes"].reason = "known gap";
  active["DataIoTest.Crashes"].allow_xpass = true;

  for (const char *msg :
       {"SEH exception with code 0xc0000005 thrown in the test body.",
        "C++ exception with description \"oops\" thrown in the test body."}) {
    Scorecard sc;
    std::ostringstream out;
    XfailEventListener listener(&sc, &active, /*verbose=*/false, &out);

    TestEndEvent ev;
    ev.test_suite_name = "DataIoTest";
    ev.test_name = "Crashes";
    PopulateEventFromParts(
        {::testing::TestPartResult(::testing::TestPartResult::kFatalFailure,
                                   "unknown file", -1, msg)},
        {}, &ev);
    listener.HandleTestEnd(ev);

    const std::vector<TestRecord> tests = sc.Tests();
    ASSERT_EQ(tests.size(), 1u);
    EXPECT_EQ(tests[0].outcome, TestOutcome::kFail) << msg;
    EXPECT_TRUE(tests[0].exception);
    EXPECT_NE(
        out.str().find("[  FAIL  ] DataIoTest.Crashes (0 ms) (exception)"),
        std::string::npos)
        << out.str();
  }

  // A plain assertion failure on the same test is still masked as XFAIL.
  Scorecard sc;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, nullptr);
  TestEndEvent ev;
  ev.test_suite_name = "DataIoTest";
  ev.test_name = "Crashes";
  PopulateEventFromParts(
      {::testing::TestPartResult(::testing::TestPartResult::kNonFatalFailure,
                                 "data_io_test.cpp", 7, "Expected: success")},
      {}, &ev);
  listener.HandleTestEnd(ev);
  ASSERT_EQ(sc.Tests().size(), 1u);
  EXPECT_EQ(sc.Tests()[0].outcome, TestOutcome::kXFail);
}

TEST(XfailListenerTest, SuiteAndProgramLevelFailuresRecordUnmaskableFail) {
  // Even if a (hypothetical) baseline entry names the synthetic id, it stays
  // FAIL.
  std::map<std::string, XfailEntry> active;
  active["DeleteTest.<suite-level>"].reason = "cannot mask";

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, &out);

  // No failure messages => nothing recorded.
  listener.HandleOutOfTestFailure("DeleteTest", kSuiteLevelTestName, {});
  EXPECT_TRUE(sc.Tests().empty());

  listener.HandleOutOfTestFailure(
      "DeleteTest", kSuiteLevelTestName,
      {"unknown file: C++ exception with description \"x\" thrown in "
       "SetUpTestSuite()."});
  listener.HandleOutOfTestFailure(kProgramLevelSuiteName, kProgramLevelTestName,
                                  {"env.cpp:3: Environment SetUp failed"});

  const std::vector<TestRecord> tests = sc.Tests();
  ASSERT_EQ(tests.size(), 2u);
  EXPECT_EQ(tests[0].id, "<program>.<program-level>");
  EXPECT_EQ(tests[0].outcome, TestOutcome::kFail);
  EXPECT_TRUE(tests[0].synthetic);
  EXPECT_FALSE(tests[0].exception);
  EXPECT_EQ(tests[1].id, "DeleteTest.<suite-level>");
  EXPECT_EQ(tests[1].domain, "delete");
  EXPECT_EQ(tests[1].outcome, TestOutcome::kFail);
  EXPECT_TRUE(tests[1].synthetic);
  EXPECT_TRUE(tests[1].exception);
  EXPECT_TRUE(tests[1].xfail_reason.empty());
  ASSERT_EQ(tests[1].failure_messages.size(), 1u);

  EXPECT_NE(out.str().find("[  FAIL  ] DeleteTest.<suite-level> (0 ms) "
                           "(exception) (outside test body)"),
            std::string::npos)
      << out.str();
  EXPECT_EQ(ComputeExitCode(sc.Summary(), XpassPolicy::kWarn), 1);
  // Synthetic records never feed --update-baseline.
  EXPECT_TRUE(sc.ToObservedResults().empty());
}

TEST(XfailListenerTest, LiveSuiteAndProgramEndWithoutAdHocFailuresAreNoOps) {
  const ::testing::UnitTest *unit_test = ::testing::UnitTest::GetInstance();
  const ::testing::TestSuite *suite = unit_test->current_test_suite();
  ASSERT_NE(suite, nullptr);

  Scorecard sc;
  std::ostringstream out;
  XfailEventListener listener(&sc, /*active_xfails=*/nullptr,
                              /*verbose=*/false, &out);
  listener.OnTestSuiteEnd(*suite);
  listener.OnTestProgramEnd(*unit_test);
  EXPECT_TRUE(sc.Tests().empty());
  EXPECT_TRUE(out.str().empty()) << out.str();
}

TEST(XfailListenerTest, LiveFatalSetupPropertyIsReadFromRealTestResult) {
  const ::testing::TestInfo *current =
      ::testing::UnitTest::GetInstance()->current_test_info();
  ASSERT_NE(current, nullptr);
  ::testing::Test::RecordProperty(fstest::kFatalSetupProperty, "1");

  std::map<std::string, XfailEntry> active;
  active[std::string(current->test_suite_name()) + "." + current->name()]
      .reason = "would mask";
  Scorecard sc;
  XfailEventListener listener(&sc, &active, /*verbose=*/false, nullptr);
  listener.OnTestEnd(*current);
  ASSERT_EQ(sc.Tests().size(), 1u);
  EXPECT_TRUE(sc.Tests()[0].fatal_setup);
  EXPECT_EQ(sc.Tests()[0].outcome, TestOutcome::kFail);
}

TEST(XfailListenerTest, CollectRegisteredTestIdsInOrderMatchesSetAndOrder) {
  const ::testing::UnitTest *unit_test = ::testing::UnitTest::GetInstance();
  const std::vector<std::string> ordered =
      CollectRegisteredTestIdsInOrder(unit_test);
  const std::unordered_set<std::string> as_set =
      CollectRegisteredTestIds(unit_test);
  EXPECT_EQ(ordered.size(), as_set.size());
  // Tests within one suite keep declaration order.
  const auto first =
      std::find(ordered.begin(), ordered.end(),
                "XfailListenerTest.FormatSkipReasonCombinesCategoryAndReason");
  const auto later = std::find(ordered.begin(), ordered.end(),
                               "XfailListenerTest.FormatsAndRecordsPass");
  ASSERT_NE(first, ordered.end());
  ASSERT_NE(later, ordered.end());
  EXPECT_LT(first, later);
  EXPECT_TRUE(CollectRegisteredTestIdsInOrder(nullptr).empty());
}

} // namespace
} // namespace fstest::cli
