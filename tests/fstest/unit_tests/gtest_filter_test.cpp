// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "gtest_filter.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {
namespace {

TEST(GtestFilterTest, MatchesEmptyAndCatchAllFilters) {
  // GoogleTest semantics: an empty --gtest_filter= selects no tests.
  EXPECT_FALSE(MatchesGtestFilter("", "CreateOpenTest.Basic"));
  EXPECT_FALSE(MatchesGtestFilter("", ""));
  EXPECT_TRUE(MatchesGtestFilter("*", "CreateOpenTest.Basic"));
  EXPECT_TRUE(MatchesGtestFilter("*.*", "CreateOpenTest.Basic"));
  EXPECT_TRUE(MatchesGtestFilter("*-", "CreateOpenTest.Basic"));
  EXPECT_TRUE(MatchesGtestFilter("-", "CreateOpenTest.Basic"));

  EXPECT_FALSE(MatchesGtestFilter("*-*", "CreateOpenTest.Basic"));
  EXPECT_FALSE(MatchesGtestFilter("-*", "CreateOpenTest.Basic"));
}

TEST(GtestFilterTest, MatchesExactAndStarWildcards) {
  EXPECT_TRUE(
      MatchesGtestFilter("CreateOpenTest.Basic", "CreateOpenTest.Basic"));
  EXPECT_FALSE(
      MatchesGtestFilter("CreateOpenTest.Basic", "CreateOpenTest.Basic2"));
  EXPECT_FALSE(
      MatchesGtestFilter("createopentest.basic", "CreateOpenTest.Basic"));

  EXPECT_TRUE(MatchesGtestFilter("CreateOpenTest.*", "CreateOpenTest.Basic"));
  EXPECT_TRUE(MatchesGtestFilter("*.Basic", "CreateOpenTest.Basic"));
  EXPECT_TRUE(MatchesGtestFilter("Create*Test.*ic", "CreateOpenTest.Basic"));
  EXPECT_TRUE(MatchesGtestFilter("Create***Basic", "CreateOpenTest.Basic"));
  EXPECT_TRUE(
      MatchesGtestFilter("CreateOpenTest.*Basic*", "CreateOpenTest.Basic"));
  EXPECT_FALSE(MatchesGtestFilter("DeleteTest.*", "CreateOpenTest.Basic"));
}

TEST(GtestFilterTest, MatchesQuestionMarkWildcard) {
  EXPECT_TRUE(MatchesGtestFilter("Suite.Case?", "Suite.Case1"));
  EXPECT_TRUE(MatchesGtestFilter("Suite.Case?", "Suite.CaseA"));
  EXPECT_FALSE(MatchesGtestFilter("Suite.Case?", "Suite.Case"));
  EXPECT_FALSE(MatchesGtestFilter("Suite.Case?", "Suite.Case12"));

  EXPECT_TRUE(MatchesGtestFilter("?.?", "A.B"));
  EXPECT_FALSE(MatchesGtestFilter("?.?", "AA.B"));
  EXPECT_TRUE(MatchesGtestFilter("Suite.?*?", "Suite.AB"));
  EXPECT_TRUE(MatchesGtestFilter("Suite.?*?", "Suite.A_middle_B"));
  EXPECT_FALSE(MatchesGtestFilter("Suite.?*?", "Suite.A"));
}

TEST(GtestFilterTest, MatchesMultipleColonSeparatedPatterns) {
  const std::string filter = "CreateOpenTest.One:DeleteTest.*:StreamsTest.T?";
  EXPECT_TRUE(MatchesGtestFilter(filter, "CreateOpenTest.One"));
  EXPECT_FALSE(MatchesGtestFilter(filter, "CreateOpenTest.Two"));
  EXPECT_TRUE(MatchesGtestFilter(filter, "DeleteTest.Anything"));
  EXPECT_TRUE(MatchesGtestFilter(filter, "StreamsTest.T9"));
  EXPECT_FALSE(MatchesGtestFilter(filter, "StreamsTest.T10"));
  EXPECT_FALSE(MatchesGtestFilter(filter, "RenameMoveTest.Basic"));

  // Empty segments between/around colons are ignored cleanly.
  EXPECT_TRUE(MatchesGtestFilter(":A.One::B.Two:", "A.One"));
  EXPECT_TRUE(MatchesGtestFilter(":A.One::B.Two:", "B.Two"));
  EXPECT_FALSE(MatchesGtestFilter(":A.One::B.Two:", "C.Three"));
  EXPECT_FALSE(MatchesGtestFilter(":", "A.One"));
}

TEST(GtestFilterTest, MatchesPositiveAndNegativeClauses) {
  // Negative-only filters match everything except excluded patterns.
  EXPECT_FALSE(MatchesGtestFilter("-DeleteTest.*", "DeleteTest.Posix"));
  EXPECT_TRUE(MatchesGtestFilter("-DeleteTest.*", "CreateOpenTest.Basic"));
  EXPECT_FALSE(
      MatchesGtestFilter("-DeleteTest.*:StreamsTest.Ads", "StreamsTest.Ads"));
  EXPECT_TRUE(
      MatchesGtestFilter("-DeleteTest.*:StreamsTest.Ads", "StreamsTest.Other"));

  // Combined positive and negative clauses: negative overrides positive.
  const std::string combined =
      "CreateOpenTest.*:DeleteTest.*-CreateOpenTest.Slow:DeleteTest.Crash?";
  EXPECT_TRUE(MatchesGtestFilter(combined, "CreateOpenTest.Fast"));
  EXPECT_FALSE(MatchesGtestFilter(combined, "CreateOpenTest.Slow"));
  EXPECT_TRUE(MatchesGtestFilter(combined, "DeleteTest.Normal"));
  EXPECT_FALSE(MatchesGtestFilter(combined, "DeleteTest.Crash1"));
  EXPECT_TRUE(MatchesGtestFilter(combined, "DeleteTest.Crash10"));
  EXPECT_FALSE(MatchesGtestFilter(combined, "StreamsTest.Fast"));

  // Exact positive overridden by identical negative.
  EXPECT_FALSE(MatchesGtestFilter("Suite.Same-Suite.Same", "Suite.Same"));
  // Empty negative section after '-' excludes nothing.
  EXPECT_TRUE(MatchesGtestFilter("Suite.*-", "Suite.Alpha"));
}

TEST(GtestFilterTest, AppendNegativeFilterWithoutExistingDash) {
  EXPECT_EQ(AppendNegativeGtestFilter("", {}), "");
  EXPECT_EQ(AppendNegativeGtestFilter("*", {}), "*");
  EXPECT_EQ(AppendNegativeGtestFilter("Suite.*-Suite.A", {}),
            "Suite.*-Suite.A");

  // An empty filter selects nothing and must never be widened to "*-<ids>".
  EXPECT_EQ(AppendNegativeGtestFilter("", {"A.One"}), "");
  EXPECT_EQ(AppendNegativeGtestFilter("", {"A.One", "B.Two"}), "");
  EXPECT_FALSE(
      MatchesGtestFilter(AppendNegativeGtestFilter("", {"A.One"}), "B.Two"));
  EXPECT_EQ(AppendNegativeGtestFilter("*", {"A.One", "B.Two"}),
            "*-A.One:B.Two");
  EXPECT_EQ(AppendNegativeGtestFilter("A.*:B.*", {"A.Slow", "B.Crash"}),
            "A.*:B.*-A.Slow:B.Crash");
}

TEST(GtestFilterTest, AppendNegativeFilterWithExistingDash) {
  EXPECT_EQ(AppendNegativeGtestFilter("*-X.Y", {"A.B", "C.D"}),
            "*-X.Y:A.B:C.D");
  EXPECT_EQ(AppendNegativeGtestFilter("A.*-A.First", {"A.Second"}),
            "A.*-A.First:A.Second");
  EXPECT_EQ(AppendNegativeGtestFilter("-X.Y:", {"A.B"}), "-X.Y:A.B");
  EXPECT_EQ(AppendNegativeGtestFilter("A.*-", {"A.B"}), "A.*-A.B");
  EXPECT_EQ(AppendNegativeGtestFilter("-", {"", "A.B", ""}), "-A.B");
  EXPECT_EQ(AppendNegativeGtestFilter("", {"", "A.B", ""}), "");
  EXPECT_EQ(AppendNegativeGtestFilter("A.*", {"", ""}), "A.*");
}

TEST(GtestFilterTest, AppendNegativeFilterIdempotencyAndMatchingIntegration) {
  const std::vector<std::string> exclusions = {"Suite.CrashA", "Suite.CrashB"};

  const std::string once = AppendNegativeGtestFilter("Suite.*", exclusions);
  EXPECT_EQ(once, "Suite.*-Suite.CrashA:Suite.CrashB");

  // Re-appending the same exclusions is strictly idempotent.
  const std::string twice = AppendNegativeGtestFilter(once, exclusions);
  EXPECT_EQ(twice, once);

  // Duplicates inside exclude_ids and entries already present in the negative
  // section are deduplicated while preserving order of newly added IDs.
  EXPECT_EQ(AppendNegativeGtestFilter(
                "*-Suite.CrashA",
                {"Suite.CrashA", "Suite.CrashB", "Suite.CrashB", "Suite.New"}),
            "*-Suite.CrashA:Suite.CrashB:Suite.New");

  // Presence in the positive section must NOT prevent appending to negative.
  const std::string exact_excluded =
      AppendNegativeGtestFilter("Suite.CrashA", {"Suite.CrashA"});
  EXPECT_EQ(exact_excluded, "Suite.CrashA-Suite.CrashA");
  EXPECT_FALSE(MatchesGtestFilter(exact_excluded, "Suite.CrashA"));

  // Integration with MatchesGtestFilter.
  EXPECT_FALSE(MatchesGtestFilter(twice, "Suite.CrashA"));
  EXPECT_FALSE(MatchesGtestFilter(twice, "Suite.CrashB"));
  EXPECT_TRUE(MatchesGtestFilter(twice, "Suite.Safe"));
  EXPECT_FALSE(MatchesGtestFilter(twice, "Other.Safe"));
}

TEST(GtestFilterTest, IsDisabledTestIdMirrorsGtestDisabledPrefix) {
  EXPECT_TRUE(IsDisabledTestId("DISABLED_Suite.Test"));
  EXPECT_TRUE(IsDisabledTestId("Suite.DISABLED_Test"));
  EXPECT_TRUE(IsDisabledTestId("Prefix/DISABLED_Suite.Test"));
  EXPECT_TRUE(IsDisabledTestId("Suite.Param/DISABLED_Case"));
  EXPECT_FALSE(IsDisabledTestId("Suite.Test"));
  EXPECT_FALSE(IsDisabledTestId("Suite.TestDISABLED_"));
  EXPECT_FALSE(IsDisabledTestId("NotDISABLED_Suite.Test"));
}

TEST(GtestFilterTest, SelectSkipCrashPreRecordsHonorsFilterDisabledAndShards) {
  const std::vector<std::string> registered = {
      "A.One", "A.Crash", "A.DISABLED_Crash", "B.Two", "B.Crash", "C.Three",
  };
  const std::set<std::string> crash = {"A.Crash", "A.DISABLED_Crash", "B.Crash",
                                       "Unknown.Crash"};

  // Unsharded: all registered, matching, non-disabled skip_crash IDs.
  EXPECT_EQ(SelectSkipCrashPreRecords(registered, crash, "*",
                                      /*also_run_disabled=*/false, {}),
            (std::vector<std::string>{"A.Crash", "B.Crash"}));
  EXPECT_EQ(
      SelectSkipCrashPreRecords(registered, crash, "*",
                                /*also_run_disabled=*/true, {}),
      (std::vector<std::string>{"A.Crash", "A.DISABLED_Crash", "B.Crash"}));
  // Filter restricts the set; an empty filter selects nothing (m1).
  EXPECT_EQ(SelectSkipCrashPreRecords(registered, crash, "B.*", false, {}),
            (std::vector<std::string>{"B.Crash"}));
  EXPECT_TRUE(
      SelectSkipCrashPreRecords(registered, crash, "", false, {}).empty());

  // Sharded: runnable (non-disabled, matching) indices are
  // A.One=0 A.Crash=1 B.Two=2 B.Crash=3 C.Three=4.
  const ShardSpec shard0{2, 0};
  const ShardSpec shard1{2, 1};
  EXPECT_TRUE(
      SelectSkipCrashPreRecords(registered, crash, "*", false, shard0).empty());
  EXPECT_EQ(SelectSkipCrashPreRecords(registered, crash, "*", false, shard1),
            (std::vector<std::string>{"A.Crash", "B.Crash"}));
  const ShardSpec three0{3, 0};
  const ShardSpec three1{3, 1};
  const ShardSpec three2{3, 2};
  EXPECT_EQ(SelectSkipCrashPreRecords(registered, crash, "*", false, three0),
            (std::vector<std::string>{"B.Crash"}));
  EXPECT_EQ(SelectSkipCrashPreRecords(registered, crash, "*", false, three1),
            (std::vector<std::string>{"A.Crash"}));
  EXPECT_TRUE(
      SelectSkipCrashPreRecords(registered, crash, "*", false, three2).empty());

  // Every skip_crash test is recorded by exactly one shard.
  std::vector<std::string> merged;
  for (int32_t idx = 0; idx < 4; ++idx) {
    for (const std::string &id : SelectSkipCrashPreRecords(
             registered, crash, "*", false, ShardSpec{4, idx})) {
      merged.push_back(id);
    }
  }
  std::sort(merged.begin(), merged.end());
  EXPECT_EQ(merged, (std::vector<std::string>{"A.Crash", "B.Crash"}));
}

} // namespace
} // namespace fstest::cli
