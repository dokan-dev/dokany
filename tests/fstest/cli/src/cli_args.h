// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fstest::cli {

enum class XpassPolicy {
  kFail,
  kWarn,
};

enum class VolumeRootMode {
  kAuto,
  kTrue,
  kFalse,
};

struct CliOptions {
  // Primary target & mode flags
  std::string root;
  std::string cleanup_dir;
  std::string validate_baseline_path;
  bool strict = false;
  std::string baseline_path;
  std::string target;
  std::string target_arch;
  XpassPolicy xpass_policy = XpassPolicy::kFail;
  std::string json_out_path;

  // Baseline update flags
  bool update_baseline = false;
  std::string update_baseline_path;
  std::string default_reason = "TODO: triage failure";
  // Also remove passing entries that apply to other hosts (unpredicated,
  // multi-build); without it they are kept and listed.
  bool prune_passing = false;

  // Suite behavior & capability flags
  bool respect_fs_caps = false;
  VolumeRootMode is_volume_root = VolumeRootMode::kAuto;
  std::string temp_prefix = "fst";
  uint32_t large_dir_count = 600;
  uint32_t async_timeout_ms = 5000;

  // Validation & output flags
  bool allow_unknown_xfails = false;
  bool verbose = false;
  bool show_help = false;
  bool show_version = false;

  // Inspected GoogleTest flags (also forwarded in gtest_args)
  bool gtest_list_tests = false;
  // True for any --gtest_filter other than GoogleTest's default "*".
  bool gtest_has_filter = false;
  bool gtest_fail_fast = false;
  int gtest_repeat = 1;

  // Program name + forwarded --gtest_* flags for ::testing::InitGoogleTest
  std::vector<std::string> gtest_args;
};

struct CliParseResult {
  bool ok = false;
  CliOptions options;
  std::string error;
};

// Parses a --gtest_<bool_flag>=<value> value exactly like GoogleTest: empty
// or anything not starting with '0', 'f', or 'F' is true.
bool ParseGtestBoolValue(std::string_view val);

// Interprets a GTEST_* boolean environment variable exactly like GoogleTest's
// BoolFromGTestEnv(): any non-empty value other than "0" is true.
bool IsGtestEnvBoolTrue(std::string_view val);

// Returns the compiled binary's architecture ("x64", "x86", or "arm64").
std::string DefaultBinaryArch();

// Returns true iff arch is one of {"x64", "x86", "arm64"}.
bool IsValidTargetArch(std::string_view arch);

// Parses UTF-8 command-line arguments (args[0] = program name).
// env_root provides an optional fallback when --root is not specified.
// env_gtest_list_tests reflects GTEST_LIST_TESTS when set in the environment.
CliParseResult ParseCliArgs(const std::vector<std::string> &args,
                            std::string_view env_root = "",
                            bool env_gtest_list_tests = false);

std::string FormatHelpText(std::string_view program_name);
std::string FormatVersionText();

} // namespace fstest::cli
