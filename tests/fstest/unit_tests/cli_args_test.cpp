// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "cli_args.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {
namespace {

TEST(CliArgsTest, MissingRootWithoutEnvFallbackFails) {
  const CliParseResult res = ParseCliArgs({"dokan_fstest.exe"});
  EXPECT_FALSE(res.ok);
  EXPECT_NE(res.error.find("--root"), std::string::npos) << res.error;
}

TEST(CliArgsTest, EnvRootFallbackUsedAndExplicitRootOverrides) {
  const CliParseResult fallback =
      ParseCliArgs({"dokan_fstest.exe"}, "C:\\fstest_env");
  ASSERT_TRUE(fallback.ok) << fallback.error;
  EXPECT_EQ(fallback.options.root, "C:\\fstest_env");

  const CliParseResult explicit_root =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\"}, "C:\\fstest_env");
  ASSERT_TRUE(explicit_root.ok) << explicit_root.error;
  EXPECT_EQ(explicit_root.options.root, "M:\\");
}

TEST(CliArgsTest, ParsesEqualsAndSpaceSeparatedFlagsAndRejectsReference) {
  const std::vector<std::string> argv = {
      "dokan_fstest.exe",
      "--root",
      "M:\\",
      "--baseline=baselines/dokan_mirror.json",
      "--target",
      "mirror_unc",
      "--target-arch=x86",
      "--xpass-policy",
      "warn",
      "--json-out",
      "out/report.json",
      "--respect-fs-caps",
      "--is-volume-root=false",
      "--temp-prefix=pfx",
      "--large-dir-count=120",
      "--async-timeout-ms=8000",
      "--allow-unknown-xfails",
      "--verbose",
      "--gtest_filter=CreateOpenTest.*",
      "--gtest_output=xml:out.xml",
      "--gtest_repeat=3",
  };

  const CliParseResult res = ParseCliArgs(argv);
  ASSERT_TRUE(res.ok) << res.error;
  EXPECT_EQ(res.options.root, "M:\\");
  EXPECT_EQ(res.options.baseline_path, "baselines/dokan_mirror.json");
  EXPECT_EQ(res.options.target, "mirror_unc");
  EXPECT_EQ(res.options.target_arch, "x86");
  EXPECT_EQ(res.options.xpass_policy, XpassPolicy::kWarn);
  EXPECT_EQ(res.options.json_out_path, "out/report.json");
  EXPECT_TRUE(res.options.respect_fs_caps);
  EXPECT_EQ(res.options.is_volume_root, VolumeRootMode::kFalse);
  EXPECT_EQ(res.options.temp_prefix, "pfx");
  EXPECT_EQ(res.options.large_dir_count, 120u);
  EXPECT_EQ(res.options.async_timeout_ms, 8000u);
  EXPECT_TRUE(res.options.allow_unknown_xfails);
  EXPECT_TRUE(res.options.verbose);
  EXPECT_TRUE(res.options.gtest_has_filter);
  EXPECT_FALSE(res.options.gtest_fail_fast);
  EXPECT_FALSE(res.options.prune_passing);
  EXPECT_EQ(res.options.gtest_repeat, 3);
  ASSERT_EQ(res.options.gtest_args.size(), 4u);
  EXPECT_EQ(res.options.gtest_args[0], "dokan_fstest.exe");
  EXPECT_EQ(res.options.gtest_args[1], "--gtest_filter=CreateOpenTest.*");
  EXPECT_EQ(res.options.gtest_args[2], "--gtest_output=xml:out.xml");
  EXPECT_EQ(res.options.gtest_args[3], "--gtest_repeat=3");

  // Removed --reference flag must be rejected as an unknown flag.
  const CliParseResult ref_rejected =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--reference=C:\\ref"});
  EXPECT_FALSE(ref_rejected.ok);
  EXPECT_NE(ref_rejected.error.find("--reference"), std::string::npos)
      << ref_rejected.error;
}

TEST(CliArgsTest, StrictMutualExclusion) {
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--strict",
                             "--baseline=b.json"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--strict",
                             "--target=mirror_drive"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--strict",
                             "--update-baseline"})
                   .ok);

  const CliParseResult strict_ok =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--strict"});
  ASSERT_TRUE(strict_ok.ok) << strict_ok.error;
  EXPECT_TRUE(strict_ok.options.strict);
}

TEST(CliArgsTest, CleanupDirStandaloneAndMutualExclusion) {
  const CliParseResult ok =
      ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\fstest_ntfs"});
  ASSERT_TRUE(ok.ok) << ok.error;
  EXPECT_EQ(ok.options.cleanup_dir, "C:\\fstest_ntfs");

  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a", "--root=M:\\"})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a", "--strict"}).ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--baseline=b.json"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--target=mirror_drive"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--update-baseline"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--json-out=r.json"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--gtest_list_tests"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--gtest_filter=*"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--respect-fs-caps"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--allow-unknown-xfails"})
                   .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a", "--verbose"})
          .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--validate-baseline=b.json"})
                   .ok);
}

TEST(CliArgsTest, ValidateBaselineStandaloneAndMutualExclusion) {
  const CliParseResult eq = ParseCliArgs(
      {"dokan_fstest.exe", "--validate-baseline=baselines/dokan_memfs.json"});
  ASSERT_TRUE(eq.ok) << eq.error;
  EXPECT_EQ(eq.options.validate_baseline_path, "baselines/dokan_memfs.json");

  const CliParseResult space =
      ParseCliArgs({"dokan_fstest.exe", "--validate-baseline",
                    "baselines/dokan_mirror.json"});
  ASSERT_TRUE(space.ok) << space.error;
  EXPECT_EQ(space.options.validate_baseline_path,
            "baselines/dokan_mirror.json");

  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline="}).ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline"}).ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline=b.json",
                             "--root=M:\\"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline=b.json",
                             "--cleanup-dir=C:\\a"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline=b.json",
                             "--strict"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline=b.json",
                             "--baseline=b.json"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline=b.json",
                             "--update-baseline"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline=b.json",
                             "--gtest_list_tests"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--validate-baseline=b.json",
                             "--verbose"})
                   .ok);
}

TEST(CliArgsTest, DefaultReasonRejectsBlankValues) {
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--default-reason="})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--default-reason=   "})
          .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--default-reason=\t\r\n "})
                   .ok);
}

TEST(CliArgsTest, TargetRequiresBaseline) {
  const CliParseResult res = ParseCliArgs(
      {"dokan_fstest.exe", "--root=M:\\", "--target=mirror_drive"});
  EXPECT_FALSE(res.ok);
  EXPECT_NE(res.error.find("--baseline"), std::string::npos) << res.error;
}

TEST(CliArgsTest, UpdateBaselineRequiresBaseline) {
  const CliParseResult res =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--update-baseline"});
  EXPECT_FALSE(res.ok);
  EXPECT_NE(res.error.find("--baseline"), std::string::npos) << res.error;
}

TEST(CliArgsTest, UpdateBaselineRejectsGtestFilterAndRepeat) {
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--gtest_filter=DeleteTest.*"})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--gtest_repeat=2"})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--gtest_repeat=0"})
          .ok);

  const CliParseResult repeat_one =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--gtest_repeat=1"});
  ASSERT_TRUE(repeat_one.ok) << repeat_one.error;
  EXPECT_TRUE(repeat_one.options.update_baseline);
  EXPECT_EQ(repeat_one.options.gtest_repeat, 1);
}

TEST(CliArgsTest, UpdateBaselineOptionalPathForms) {
  // Bare --update-baseline at end of argv -> in-place update.
  const CliParseResult bare =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline"});
  ASSERT_TRUE(bare.ok) << bare.error;
  EXPECT_TRUE(bare.options.update_baseline);
  EXPECT_TRUE(bare.options.update_baseline_path.empty());

  // --update-baseline=out.json
  const CliParseResult eq_path =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline=out.json"});
  ASSERT_TRUE(eq_path.ok) << eq_path.error;
  EXPECT_TRUE(eq_path.options.update_baseline);
  EXPECT_EQ(eq_path.options.update_baseline_path, "out.json");

  // --update-baseline out.json (space-separated path)
  const CliParseResult space_path =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "out.json"});
  ASSERT_TRUE(space_path.ok) << space_path.error;
  EXPECT_TRUE(space_path.options.update_baseline);
  EXPECT_EQ(space_path.options.update_baseline_path, "out.json");

  // --update-baseline --verbose must NOT treat --verbose as an output path.
  const CliParseResult next_flag = ParseCliArgs(
      {"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
       "--update-baseline", "--verbose", "--default-reason=triage me"});
  ASSERT_TRUE(next_flag.ok) << next_flag.error;
  EXPECT_TRUE(next_flag.options.update_baseline);
  EXPECT_TRUE(next_flag.options.update_baseline_path.empty());
  EXPECT_TRUE(next_flag.options.verbose);
  EXPECT_EQ(next_flag.options.default_reason, "triage me");

  // Empty inline path rejected.
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--baseline=b.json", "--update-baseline="})
                   .ok);
}

TEST(CliArgsTest, TargetArchValidationAndDefault) {
  EXPECT_TRUE(IsValidTargetArch("x64"));
  EXPECT_TRUE(IsValidTargetArch("x86"));
  EXPECT_TRUE(IsValidTargetArch("arm64"));
  EXPECT_FALSE(IsValidTargetArch(""));
  EXPECT_FALSE(IsValidTargetArch("X64"));
  EXPECT_FALSE(IsValidTargetArch("sparc"));
  EXPECT_TRUE(IsValidTargetArch(DefaultBinaryArch()));

  const CliParseResult default_arch =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\"});
  ASSERT_TRUE(default_arch.ok) << default_arch.error;
  EXPECT_EQ(default_arch.options.target_arch, DefaultBinaryArch());

  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--target-arch=sparc"})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--target-arch="}).ok);
}

TEST(CliArgsTest, XpassPolicyAndVolumeRootValidation) {
  const CliParseResult fail_policy =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--xpass-policy=fail",
                    "--is-volume-root=true"});
  ASSERT_TRUE(fail_policy.ok) << fail_policy.error;
  EXPECT_EQ(fail_policy.options.xpass_policy, XpassPolicy::kFail);
  EXPECT_EQ(fail_policy.options.is_volume_root, VolumeRootMode::kTrue);

  const CliParseResult auto_root = ParseCliArgs(
      {"dokan_fstest.exe", "--root=M:\\", "--is-volume-root=auto"});
  ASSERT_TRUE(auto_root.ok) << auto_root.error;
  EXPECT_EQ(auto_root.options.is_volume_root, VolumeRootMode::kAuto);

  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--xpass-policy=ignore"})
          .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--is-volume-root=maybe"})
                   .ok);
}

TEST(CliArgsTest, LargeDirCountAndAsyncTimeoutValidation) {
  const CliParseResult min_ok =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--large-dir-count=10",
                    "--async-timeout-ms=1"});
  ASSERT_TRUE(min_ok.ok) << min_ok.error;
  EXPECT_EQ(min_ok.options.large_dir_count, 10u);
  EXPECT_EQ(min_ok.options.async_timeout_ms, 1u);

  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--large-dir-count=9"})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--large-dir-count=abc"})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--large-dir-count=-5"})
          .ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--async-timeout-ms=0"})
          .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--async-timeout-ms=fast"})
                   .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--async-timeout-ms=-100"})
                   .ok);
}

TEST(CliArgsTest, TempPrefixValidation) {
  const std::vector<std::string> invalid_prefixes = {
      "",
      ".",
      "..",
      "a/b",
      "a\\b",
      "has space",
      "col:on",
      "wild*card",
      "quest?mark",
      "pipe|char",
      std::string(65, 'x'),
  };
  for (const std::string &bad : invalid_prefixes) {
    const CliParseResult res = ParseCliArgs(
        {"dokan_fstest.exe", "--root=M:\\", "--temp-prefix=" + bad});
    EXPECT_FALSE(res.ok) << "Expected rejection for --temp-prefix='" << bad
                         << "'";
  }

  const CliParseResult valid = ParseCliArgs(
      {"dokan_fstest.exe", "--root=M:\\", "--temp-prefix=fst_custom-1"});
  ASSERT_TRUE(valid.ok) << valid.error;
  EXPECT_EQ(valid.options.temp_prefix, "fst_custom-1");
}

TEST(CliArgsTest, GtestPassthroughAndListTests) {
  // --gtest_list_tests works without --root.
  const CliParseResult list_bare =
      ParseCliArgs({"dokan_fstest.exe", "--gtest_list_tests"});
  ASSERT_TRUE(list_bare.ok) << list_bare.error;
  EXPECT_TRUE(list_bare.options.gtest_list_tests);
  ASSERT_EQ(list_bare.options.gtest_args.size(), 2u);
  EXPECT_EQ(list_bare.options.gtest_args[1], "--gtest_list_tests");

  // --gtest_list_tests=1 bypasses --root, whereas --gtest_list_tests=0 does
  // not.
  const CliParseResult list_true =
      ParseCliArgs({"dokan_fstest.exe", "--gtest_list_tests=1"});
  ASSERT_TRUE(list_true.ok) << list_true.error;
  EXPECT_TRUE(list_true.options.gtest_list_tests);

  const CliParseResult list_false =
      ParseCliArgs({"dokan_fstest.exe", "--gtest_list_tests=0"});
  EXPECT_FALSE(list_false.ok);

  // m7: GoogleTest only treats a leading '0', 'f', or 'F' as false.
  for (const char *falsy : {"--gtest_list_tests=false", "--gtest_list_tests=F",
                            "--gtest_list_tests=f", "--gtest_list_tests=00"}) {
    const CliParseResult res = ParseCliArgs({"dokan_fstest.exe", falsy});
    EXPECT_FALSE(res.ok) << falsy;
  }
  for (const char *truthy : {"--gtest_list_tests=no", "--gtest_list_tests=NO",
                             "--gtest_list_tests=yes", "--gtest_list_tests="}) {
    const CliParseResult res = ParseCliArgs({"dokan_fstest.exe", truthy});
    ASSERT_TRUE(res.ok) << truthy << ": " << res.error;
    EXPECT_TRUE(res.options.gtest_list_tests) << truthy;
  }

  // Environment GTEST_LIST_TESTS=1 bypasses --root unless overridden to 0.
  const CliParseResult env_list =
      ParseCliArgs({"dokan_fstest.exe"}, "", /*env_gtest_list_tests=*/true);
  ASSERT_TRUE(env_list.ok) << env_list.error;
  EXPECT_TRUE(env_list.options.gtest_list_tests);

  const CliParseResult env_list_overridden =
      ParseCliArgs({"dokan_fstest.exe", "--gtest_list_tests=0"}, "",
                   /*env_gtest_list_tests=*/true);
  EXPECT_FALSE(env_list_overridden.ok);

  // Invalid --gtest_repeat value rejected cleanly.
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--gtest_repeat=notanint"})
                   .ok);

  // Arbitrary --gtest_* flags pass through untouched.
  const CliParseResult passthrough =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--gtest_shuffle",
                    "--gtest_random_seed=42", "--gtest_color=no"});
  ASSERT_TRUE(passthrough.ok) << passthrough.error;
  ASSERT_EQ(passthrough.options.gtest_args.size(), 4u);
  EXPECT_EQ(passthrough.options.gtest_args[1], "--gtest_shuffle");
  EXPECT_EQ(passthrough.options.gtest_args[2], "--gtest_random_seed=42");
  EXPECT_EQ(passthrough.options.gtest_args[3], "--gtest_color=no");
}

TEST(CliArgsTest, GtestBoolAndEnvParsingMatchGoogleTest) {
  // --gtest_<bool>=<value> (gtest ParseFlag): leading 0/f/F is false.
  EXPECT_TRUE(ParseGtestBoolValue(""));
  EXPECT_TRUE(ParseGtestBoolValue("1"));
  EXPECT_TRUE(ParseGtestBoolValue("true"));
  EXPECT_TRUE(ParseGtestBoolValue("no"));
  EXPECT_TRUE(ParseGtestBoolValue("NO"));
  EXPECT_FALSE(ParseGtestBoolValue("0"));
  EXPECT_FALSE(ParseGtestBoolValue("false"));
  EXPECT_FALSE(ParseGtestBoolValue("FALSE"));
  EXPECT_FALSE(ParseGtestBoolValue("f"));
  EXPECT_FALSE(ParseGtestBoolValue("fx"));

  // GTEST_* env vars (gtest BoolFromGTestEnv): anything but "0" is true, so
  // GTEST_LIST_TESTS=false is truthy; unset/empty is false.
  EXPECT_FALSE(IsGtestEnvBoolTrue(""));
  EXPECT_FALSE(IsGtestEnvBoolTrue("0"));
  EXPECT_TRUE(IsGtestEnvBoolTrue("1"));
  EXPECT_TRUE(IsGtestEnvBoolTrue("false"));
  EXPECT_TRUE(IsGtestEnvBoolTrue("FALSE"));
  EXPECT_TRUE(IsGtestEnvBoolTrue("no"));
  EXPECT_TRUE(IsGtestEnvBoolTrue("00"));
}

TEST(CliArgsTest, GtestFilterStarIsDefaultAndOtherFiltersBlockUpdateBaseline) {
  // m7: --gtest_filter=* is GoogleTest's default and is accepted with
  // --update-baseline (main.cpp applies the same rule to GTEST_FILTER).
  const CliParseResult star =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--gtest_filter=*"});
  ASSERT_TRUE(star.ok) << star.error;
  EXPECT_FALSE(star.options.gtest_has_filter);
  ASSERT_EQ(star.options.gtest_args.size(), 2u);
  EXPECT_EQ(star.options.gtest_args[1], "--gtest_filter=*");

  // An empty filter selects nothing (m1) and is therefore a real filter.
  const CliParseResult empty =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--gtest_filter="});
  EXPECT_FALSE(empty.ok);
  EXPECT_NE(empty.error.find("--gtest_filter"), std::string::npos)
      << empty.error;
  for (const char *filter :
       {"--gtest_filter=*.*", "--gtest_filter=*-A.B", "--gtest_filter=A.*"}) {
    EXPECT_FALSE(
        ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                      "--update-baseline", filter})
            .ok)
        << filter;
  }
  // Without --update-baseline every filter is forwarded untouched.
  const CliParseResult plain =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--gtest_filter=A.*"});
  ASSERT_TRUE(plain.ok) << plain.error;
  EXPECT_TRUE(plain.options.gtest_has_filter);
}

TEST(CliArgsTest, GtestFailFastRejectedWithBaseline) {
  // m2: the first XFAIL would make GoogleTest skip every remaining test.
  for (const char *flag :
       {"--gtest_fail_fast", "--gtest_fail_fast=1", "--gtest_fail_fast=true"}) {
    const CliParseResult res = ParseCliArgs(
        {"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json", flag});
    EXPECT_FALSE(res.ok) << flag;
    EXPECT_NE(res.error.find("--gtest_fail_fast"), std::string::npos)
        << res.error;
  }
  // Explicitly disabled fail-fast is harmless.
  const CliParseResult off =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--gtest_fail_fast=0"});
  ASSERT_TRUE(off.ok) << off.error;
  EXPECT_FALSE(off.options.gtest_fail_fast);
  // Strict mode may still fail fast.
  const CliParseResult strict = ParseCliArgs(
      {"dokan_fstest.exe", "--root=M:\\", "--strict", "--gtest_fail_fast"});
  ASSERT_TRUE(strict.ok) << strict.error;
  EXPECT_TRUE(strict.options.gtest_fail_fast);
}

TEST(CliArgsTest, RejectsOptionLookingValuesAndDuplicateValueFlags) {
  // m6: "--json-out --verbose" must not create a file named "--verbose".
  const CliParseResult swallowed = ParseCliArgs(
      {"dokan_fstest.exe", "--root=M:\\", "--json-out", "--verbose"});
  EXPECT_FALSE(swallowed.ok);
  EXPECT_NE(swallowed.error.find("--json-out"), std::string::npos)
      << swallowed.error;
  EXPECT_NE(swallowed.error.find("--verbose"), std::string::npos)
      << swallowed.error;
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root", "--strict"}).ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=--target=x"})
          .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--baseline=b.json", "--update-baseline=--x"})
                   .ok);
  // Single-dash values (e.g. negative numbers) still reach their validators.
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\",
                             "--large-dir-count", "-5"})
                   .ok);
  const CliParseResult dash_reason = ParseCliArgs(
      {"dokan_fstest.exe", "--root=M:\\", "--default-reason", "-see GAP-01-"});
  ASSERT_TRUE(dash_reason.ok) << dash_reason.error;
  EXPECT_EQ(dash_reason.options.default_reason, "-see GAP-01-");

  // m6: duplicate single-value flags are rejected (both syntaxes).
  for (const std::vector<std::string> &argv :
       std::vector<std::vector<std::string>>{
           {"dokan_fstest.exe", "--root=M:\\", "--root=N:\\"},
           {"dokan_fstest.exe", "--root", "M:\\", "--root=N:\\"},
           {"dokan_fstest.exe", "--root=M:\\", "--json-out=a.json",
            "--json-out=b.json"},
           {"dokan_fstest.exe", "--root=M:\\", "--baseline=a.json",
            "--target=t", "--target=u"},
           {"dokan_fstest.exe", "--root=M:\\", "--baseline=a.json",
            "--update-baseline", "--update-baseline=o.json"},
           {"dokan_fstest.exe", "--cleanup-dir=C:\\a", "--cleanup-dir=C:\\b"},
       }) {
    const CliParseResult res = ParseCliArgs(argv);
    EXPECT_FALSE(res.ok) << argv.back();
    EXPECT_NE(res.error.find("only be specified once"), std::string::npos)
        << res.error;
  }
  // Repeating a boolean flag is harmless.
  EXPECT_TRUE(ParseCliArgs(
                  {"dokan_fstest.exe", "--root=M:\\", "--verbose", "--verbose"})
                  .ok);
}

TEST(CliArgsTest, PrunePassingRequiresUpdateBaseline) {
  const CliParseResult ok =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--prune-passing"});
  ASSERT_TRUE(ok.ok) << ok.error;
  EXPECT_TRUE(ok.options.prune_passing);

  const CliParseResult default_off =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline"});
  ASSERT_TRUE(default_off.ok) << default_off.error;
  EXPECT_FALSE(default_off.options.prune_passing);

  const CliParseResult orphan =
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--prune-passing"});
  EXPECT_FALSE(orphan.ok);
  EXPECT_NE(orphan.error.find("--update-baseline"), std::string::npos)
      << orphan.error;
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--baseline=b.json",
                    "--update-baseline", "--prune-passing=yes"})
          .ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--cleanup-dir=C:\\a",
                             "--prune-passing"})
                   .ok);
  EXPECT_NE(FormatHelpText("x").find("--prune-passing"), std::string::npos);
}

TEST(CliArgsTest, HelpVersionAndSyntaxErrors) {
  const CliParseResult help_long = ParseCliArgs({"dokan_fstest.exe", "--help"});
  ASSERT_TRUE(help_long.ok) << help_long.error;
  EXPECT_TRUE(help_long.options.show_help);

  const CliParseResult help_short = ParseCliArgs({"dokan_fstest.exe", "-h"});
  ASSERT_TRUE(help_short.ok) << help_short.error;
  EXPECT_TRUE(help_short.options.show_help);

  const CliParseResult version =
      ParseCliArgs({"dokan_fstest.exe", "--version"});
  ASSERT_TRUE(version.ok) << version.error;
  EXPECT_TRUE(version.options.show_version);

  const std::string help_text = FormatHelpText("dokan_fstest.exe");
  EXPECT_NE(help_text.find("dokan_fstest.exe"), std::string::npos);
  EXPECT_NE(help_text.find("--root=<path>"), std::string::npos);
  EXPECT_NE(help_text.find("--validate-baseline=<path>"), std::string::npos);
  EXPECT_EQ(help_text.find("--reference"), std::string::npos);
  EXPECT_FALSE(FormatVersionText().empty());

  // Boolean flags reject '=value', unknown/positional args and missing values
  // are rejected.
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--verbose=true"}).ok);
  EXPECT_FALSE(
      ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "positional"}).ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root=M:\\", "--bogus"}).ok);
  EXPECT_FALSE(ParseCliArgs({"dokan_fstest.exe", "--root"}).ok);
}

} // namespace
} // namespace fstest::cli
