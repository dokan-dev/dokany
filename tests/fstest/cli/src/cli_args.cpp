// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "cli_args.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace fstest::cli {
namespace {

constexpr std::string_view kVersionString = "dokan_fstest 1.0.0";

bool ParseUint32Strict(std::string_view text, uint32_t *out) {
  if (text.empty()) {
    return false;
  }
  uint32_t value = 0;
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return false;
  }
  *out = value;
  return true;
}

bool ParseIntStrict(std::string_view text, int *out) {
  if (text.empty()) {
    return false;
  }
  int value = 0;
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return false;
  }
  *out = value;
  return true;
}

bool IsValidTempPrefix(std::string_view prefix) {
  if (prefix.empty() || prefix.size() > 64) {
    return false;
  }
  for (char c : prefix) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x21 || uc == 0x7f) {
      return false;
    }
    switch (c) {
    case '\\':
    case '/':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
      return false;
    default:
      break;
    }
  }
  if (prefix == "." || prefix == "..") {
    return false;
  }
  return true;
}

bool IsBlank(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  });
}

struct ParseState {
  ParseState(const std::vector<std::string> &args_in, CliOptions *opts_in)
      : args(args_in), opts(opts_in) {}

  const std::vector<std::string> &args;
  CliOptions *opts;
  std::size_t index = 1;
  int non_utility_flag_count = 0;
  std::set<std::string, std::less<>> seen_value_flags;
  bool saw_target_arch = false;
  std::string error;
};

// Splits "--flag=value" into flag/value, or consumes args[++index] for
// "--flag value". Returns false if a required value is missing or looks like
// another option ("--json-out --verbose" must not create a file "--verbose").
bool ExtractFlagValue(ParseState *state, std::string_view flag_name,
                      bool has_equals, std::string_view inline_value,
                      std::string *value_out) {
  if (has_equals) {
    *value_out = std::string(inline_value);
  } else {
    if (state->index + 1 >= state->args.size()) {
      state->error = "Missing value for flag '" + std::string(flag_name) + "'";
      return false;
    }
    ++state->index;
    *value_out = state->args[state->index];
  }
  if (std::string_view(*value_out).starts_with("--")) {
    state->error = "Invalid value for flag '" + std::string(flag_name) +
                   "': '" + *value_out +
                   "' looks like an option (missing value?)";
    return false;
  }
  return true;
}

CliParseResult MakeError(std::string message) {
  CliParseResult result;
  result.ok = false;
  result.error = std::move(message);
  return result;
}

enum class FlagStatus {
  kNotMatched,
  kOk,
  kError,
};

struct BoolFlagSpec {
  std::string_view name;
  bool CliOptions::*member;
};

constexpr BoolFlagSpec kBoolFlags[] = {
    {"--strict", &CliOptions::strict},
    {"--respect-fs-caps", &CliOptions::respect_fs_caps},
    {"--allow-unknown-xfails", &CliOptions::allow_unknown_xfails},
    {"--verbose", &CliOptions::verbose},
    {"--prune-passing", &CliOptions::prune_passing},
};

struct NonEmptyStringFlagSpec {
  std::string_view name;
  std::string CliOptions::*member;
  bool counts_as_non_utility;
  std::string_view empty_noun;
};

constexpr NonEmptyStringFlagSpec kNonEmptyStringFlags[] = {
    {"--root", &CliOptions::root, true, "path"},
    {"--cleanup-dir", &CliOptions::cleanup_dir, false, "path"},
    {"--validate-baseline", &CliOptions::validate_baseline_path, false, "path"},
    {"--baseline", &CliOptions::baseline_path, true, "path"},
    {"--target", &CliOptions::target, true, "name"},
    {"--json-out", &CliOptions::json_out_path, true, "path"},
};

bool HandleTargetArch(std::string val, ParseState *state) {
  if (!IsValidTargetArch(val)) {
    state->error = "Invalid value for '--target-arch': '" + val +
                   "' (expected one of: x64, x86, arm64)";
    return false;
  }
  state->opts->target_arch = std::move(val);
  state->saw_target_arch = true;
  return true;
}

bool HandleXpassPolicy(std::string val, ParseState *state) {
  if (val == "fail") {
    state->opts->xpass_policy = XpassPolicy::kFail;
    return true;
  }
  if (val == "warn") {
    state->opts->xpass_policy = XpassPolicy::kWarn;
    return true;
  }
  state->error = "Invalid value for '--xpass-policy': '" + val +
                 "' (expected 'fail' or 'warn')";
  return false;
}

bool HandleDefaultReason(std::string val, ParseState *state) {
  if (IsBlank(val)) {
    state->error = "Flag '--default-reason' requires a non-empty string";
    return false;
  }
  state->opts->default_reason = std::move(val);
  return true;
}

bool HandleVolumeRootMode(std::string val, ParseState *state) {
  if (val == "auto") {
    state->opts->is_volume_root = VolumeRootMode::kAuto;
    return true;
  }
  if (val == "true") {
    state->opts->is_volume_root = VolumeRootMode::kTrue;
    return true;
  }
  if (val == "false") {
    state->opts->is_volume_root = VolumeRootMode::kFalse;
    return true;
  }
  state->error = "Invalid value for '--is-volume-root': '" + val +
                 "' (expected 'auto', 'true', or 'false')";
  return false;
}

bool HandleTempPrefix(std::string val, ParseState *state) {
  if (!IsValidTempPrefix(val)) {
    state->error = "Invalid value for '--temp-prefix': '" + val + "'";
    return false;
  }
  state->opts->temp_prefix = std::move(val);
  return true;
}

bool HandleLargeDirCount(std::string val, ParseState *state) {
  uint32_t count = 0;
  if (!ParseUint32Strict(val, &count) || count < 10) {
    state->error = "Invalid value for '--large-dir-count': '" + val +
                   "' (expected integer >= 10)";
    return false;
  }
  state->opts->large_dir_count = count;
  return true;
}

bool HandleAsyncTimeoutMs(std::string val, ParseState *state) {
  uint32_t ms = 0;
  if (!ParseUint32Strict(val, &ms) || ms == 0) {
    state->error = "Invalid value for '--async-timeout-ms': '" + val +
                   "' (expected positive integer)";
    return false;
  }
  state->opts->async_timeout_ms = ms;
  return true;
}

struct ValueFlagSpec {
  std::string_view name;
  bool (*handler)(std::string val, ParseState *state);
};

constexpr ValueFlagSpec kValueFlags[] = {
    {"--target-arch", &HandleTargetArch},
    {"--xpass-policy", &HandleXpassPolicy},
    {"--default-reason", &HandleDefaultReason},
    {"--is-volume-root", &HandleVolumeRootMode},
    {"--temp-prefix", &HandleTempPrefix},
    {"--large-dir-count", &HandleLargeDirCount},
    {"--async-timeout-ms", &HandleAsyncTimeoutMs},
};

bool HandleGtestFlag(std::string_view arg, const std::string &raw,
                     ParseState *state) {
  CliOptions *opts = state->opts;
  opts->gtest_args.push_back(raw);
  if (arg == "--gtest_list_tests") {
    opts->gtest_list_tests = true;
  } else if (arg.starts_with("--gtest_list_tests=")) {
    opts->gtest_list_tests = ParseGtestBoolValue(
        arg.substr(std::string_view("--gtest_list_tests=").size()));
  } else if (arg.starts_with("--gtest_filter=")) {
    // Only GoogleTest's default "*" selects every test; anything else
    // (including an empty filter, which selects nothing) is a filter.
    opts->gtest_has_filter =
        arg.substr(std::string_view("--gtest_filter=").size()) != "*";
  } else if (arg == "--gtest_fail_fast") {
    opts->gtest_fail_fast = true;
  } else if (arg.starts_with("--gtest_fail_fast=")) {
    opts->gtest_fail_fast = ParseGtestBoolValue(
        arg.substr(std::string_view("--gtest_fail_fast=").size()));
  } else if (arg.starts_with("--gtest_repeat=")) {
    int repeat_val = 1;
    const std::string_view num_str =
        arg.substr(std::string_view("--gtest_repeat=").size());
    if (!ParseIntStrict(num_str, &repeat_val)) {
      state->error = "Invalid integer value for --gtest_repeat: '" +
                     std::string(num_str) + "'";
      return false;
    }
    opts->gtest_repeat = repeat_val;
  }
  return true;
}

FlagStatus TryHandleBoolFlag(std::string_view flag, bool has_eq,
                             ParseState *state) {
  for (const BoolFlagSpec &spec : kBoolFlags) {
    if (flag != spec.name) {
      continue;
    }
    ++state->non_utility_flag_count;
    if (has_eq) {
      state->error = "Flag '" + std::string(flag) + "' does not take a value";
      return FlagStatus::kError;
    }
    state->opts->*(spec.member) = true;
    return FlagStatus::kOk;
  }
  return FlagStatus::kNotMatched;
}

bool HandleOptionalUpdateBaseline(bool has_eq, std::string_view inline_val,
                                  ParseState *state) {
  state->opts->update_baseline = true;
  if (has_eq) {
    if (inline_val.empty()) {
      state->error = "Flag '--update-baseline=' requires a non-empty path";
      return false;
    }
    if (inline_val.starts_with("--")) {
      state->error = "Invalid value for flag '--update-baseline': '" +
                     std::string(inline_val) +
                     "' looks like an option (missing value?)";
      return false;
    }
    state->opts->update_baseline_path = std::string(inline_val);
  } else if (state->index + 1 < state->args.size() &&
             !state->args[state->index + 1].empty() &&
             state->args[state->index + 1][0] != '-') {
    ++state->index;
    state->opts->update_baseline_path = state->args[state->index];
  }
  return true;
}

FlagStatus TryHandleNonEmptyStringFlag(std::string_view flag, bool has_eq,
                                       std::string_view inline_val,
                                       ParseState *state) {
  for (const NonEmptyStringFlagSpec &spec : kNonEmptyStringFlags) {
    if (flag != spec.name) {
      continue;
    }
    if (spec.counts_as_non_utility) {
      ++state->non_utility_flag_count;
    }
    std::string val;
    if (!ExtractFlagValue(state, flag, has_eq, inline_val, &val)) {
      return FlagStatus::kError;
    }
    if (val.empty()) {
      state->error = "Flag '" + std::string(flag) + "' requires a non-empty " +
                     std::string(spec.empty_noun);
      return FlagStatus::kError;
    }
    state->opts->*(spec.member) = std::move(val);
    return FlagStatus::kOk;
  }
  return FlagStatus::kNotMatched;
}

bool HandleValueFlag(std::string_view flag, bool has_eq,
                     std::string_view inline_val, ParseState *state) {
  for (const ValueFlagSpec &spec : kValueFlags) {
    if (flag != spec.name) {
      continue;
    }
    ++state->non_utility_flag_count;
    std::string val;
    if (!ExtractFlagValue(state, flag, has_eq, inline_val, &val)) {
      return false;
    }
    return spec.handler(std::move(val), state);
  }
  state->error = "Unknown flag: '" + std::string(flag) + "'";
  return false;
}

bool ValidateUpdateBaselineOptions(const CliOptions &opts,
                                   std::string *error_out) {
  if (opts.prune_passing && !opts.update_baseline) {
    *error_out = "Flag '--prune-passing' requires '--update-baseline'";
    return false;
  }
  if (!opts.update_baseline) {
    return true;
  }
  if (opts.baseline_path.empty()) {
    *error_out = "Flag '--update-baseline' requires '--baseline=<path>'";
    return false;
  }
  if (opts.gtest_has_filter) {
    *error_out = "Flag '--update-baseline' cannot be used with a "
                 "non-default '--gtest_filter' (only '*' is allowed)";
    return false;
  }
  if (opts.gtest_repeat != 1) {
    *error_out =
        "Flag '--update-baseline' cannot be used with '--gtest_repeat != 1'";
    return false;
  }
  return true;
}

bool ValidateStrictAndBaselineFlags(const CliOptions &opts,
                                    std::string *error_out) {
  if (opts.strict && (!opts.baseline_path.empty() || !opts.target.empty() ||
                      opts.update_baseline)) {
    *error_out = "Flag '--strict' is mutually exclusive with '--baseline', "
                 "'--target', and '--update-baseline'";
    return false;
  }
  if (!opts.target.empty() && opts.baseline_path.empty()) {
    *error_out = "Flag '--target' requires '--baseline=<path>'";
    return false;
  }
  if (opts.gtest_fail_fast && !opts.baseline_path.empty()) {
    *error_out =
        "Flag '--gtest_fail_fast' cannot be used with '--baseline' (the first "
        "expected XFAIL would skip every remaining test)";
    return false;
  }
  return true;
}

bool ValidateExecutionOptions(CliOptions *opts, std::string_view env_root,
                              std::string *error_out) {
  if (opts->root.empty() && !env_root.empty()) {
    opts->root = std::string(env_root);
  }
  if (opts->root.empty()) {
    *error_out = "Missing required flag '--root=<path>' (or set FSTEST_ROOT)";
    return false;
  }
  if (!ValidateStrictAndBaselineFlags(*opts, error_out)) {
    return false;
  }
  return ValidateUpdateBaselineOptions(*opts, error_out);
}

bool ValidateAllParsedOptions(CliOptions *opts, std::string_view env_root,
                              int non_utility_flag_count,
                              std::string *error_out) {
  if (opts->show_help || opts->show_version) {
    return true;
  }
  if (!opts->cleanup_dir.empty()) {
    if (!opts->validate_baseline_path.empty() || non_utility_flag_count > 0) {
      *error_out = "Flag '--cleanup-dir' cannot be combined with other flags";
      return false;
    }
    return true;
  }
  if (!opts->validate_baseline_path.empty()) {
    if (non_utility_flag_count > 0) {
      *error_out =
          "Flag '--validate-baseline' cannot be combined with other flags";
      return false;
    }
    return true;
  }
  if (opts->gtest_list_tests) {
    return true;
  }
  return ValidateExecutionOptions(opts, env_root, error_out);
}

bool ProcessDashDashFlag(std::string_view arg, ParseState *state) {
  const std::size_t eq_pos = arg.find('=');
  const bool has_eq = (eq_pos != std::string_view::npos);
  const std::string_view flag = has_eq ? arg.substr(0, eq_pos) : arg;
  const std::string_view inline_val =
      has_eq ? arg.substr(eq_pos + 1) : std::string_view{};

  const FlagStatus bool_status = TryHandleBoolFlag(flag, has_eq, state);
  if (bool_status != FlagStatus::kNotMatched) {
    return bool_status == FlagStatus::kOk;
  }
  if (!state->seen_value_flags.emplace(flag).second) {
    state->error =
        "Flag '" + std::string(flag) + "' may only be specified once";
    return false;
  }
  if (flag == "--update-baseline") {
    ++state->non_utility_flag_count;
    return HandleOptionalUpdateBaseline(has_eq, inline_val, state);
  }
  const FlagStatus str_status =
      TryHandleNonEmptyStringFlag(flag, has_eq, inline_val, state);
  if (str_status != FlagStatus::kNotMatched) {
    return str_status == FlagStatus::kOk;
  }
  return HandleValueFlag(flag, has_eq, inline_val, state);
}

bool ProcessSingleCliArg(ParseState *state) {
  const std::string &raw = state->args[state->index];
  const std::string_view arg(raw);
  if (arg.starts_with("--gtest_")) {
    ++state->non_utility_flag_count;
    return HandleGtestFlag(arg, raw, state);
  }
  if (arg == "-h" || arg == "--help") {
    state->opts->show_help = true;
    return true;
  }
  if (arg == "--version") {
    state->opts->show_version = true;
    return true;
  }
  if (!arg.starts_with("--")) {
    state->error = "Unknown argument: '" + raw + "'";
    return false;
  }
  return ProcessDashDashFlag(arg, state);
}

} // namespace

bool ParseGtestBoolValue(std::string_view val) {
  // Mirrors GoogleTest's ParseFlag(bool): only a leading '0', 'f', or 'F'
  // means false; a bare flag or empty value means true.
  if (val.empty()) {
    return true;
  }
  return !(val.front() == '0' || val.front() == 'f' || val.front() == 'F');
}

bool IsGtestEnvBoolTrue(std::string_view val) {
  // Mirrors GoogleTest's BoolFromGTestEnv(): any value other than "0" is true
  // (so GTEST_LIST_TESTS=false is truthy); unset/empty means false.
  return !val.empty() && val != "0";
}

std::string DefaultBinaryArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#else
  return "x64";
#endif
}

bool IsValidTargetArch(std::string_view arch) {
  return arch == "x64" || arch == "x86" || arch == "arm64";
}

CliParseResult ParseCliArgs(const std::vector<std::string> &args,
                            std::string_view env_root,
                            bool env_gtest_list_tests) {
  CliParseResult result;
  CliOptions &opts = result.options;
  opts.gtest_list_tests = env_gtest_list_tests;
  opts.gtest_args.push_back(args.empty() ? "dokan_fstest" : args[0]);

  ParseState state(args, &opts);
  for (state.index = 1; state.index < args.size(); ++state.index) {
    if (!ProcessSingleCliArg(&state)) {
      return MakeError(std::move(state.error));
    }
  }

  if (!state.saw_target_arch) {
    opts.target_arch = DefaultBinaryArch();
  }

  if (!ValidateAllParsedOptions(&opts, env_root, state.non_utility_flag_count,
                                &state.error)) {
    return MakeError(std::move(state.error));
  }

  result.ok = true;
  return result;
}

std::string FormatHelpText(std::string_view program_name) {
  std::ostringstream os;
  os << "Usage: " << program_name
     << " --root=<path> [options] [--gtest_* options]\n"
     << "       " << program_name << " --cleanup-dir=<path>\n"
     << "       " << program_name << " --validate-baseline=<path>\n\n"
     << "Options:\n"
     << "  --root=<path>                  Target path (drive root, junction, "
        "subdir, or UNC)\n"
     << "                                 (fallback: FSTEST_ROOT env var)\n"
     << "  --cleanup-dir=<path>           Safely delete directory tree "
        "(non-following reparse)\n"
     << "  --validate-baseline=<path>     Validate baseline manifest schema & "
        "all test IDs\n"
     << "  --strict                       Strict NTFS mode (no baseline; all "
        "failures fatal)\n"
     << "  --baseline=<path>              Baseline manifest JSON file\n"
     << "  --target=<name>                Target profile in baseline manifest\n"
     << "  --target-arch=<x64|x86|arm64>  Architecture of filesystem under "
        "test (default: "
     << DefaultBinaryArch() << ")\n"
     << "  --xpass-policy=<fail|warn>     Exit 3 on unexpected pass ('fail', "
        "default) or warn\n"
     << "  --json-out=<path>              Write structured JSON scorecard "
        "report\n"
     << "  --update-baseline[=<path>]     Update baseline for --target "
        "(in-place if no path)\n"
     << "  --prune-passing                With --update-baseline: also remove "
        "passing entries that\n"
     << "                                 apply to other hosts "
        "(default: keep and list them)\n"
     << "  --default-reason=<text>        Default reason for new "
        "--update-baseline entries\n"
     << "  --respect-fs-caps              Skip tests whose required FS "
        "capability flag is absent\n"
     << "  --is-volume-root=<auto|true|false> Override volume-root "
        "auto-detection (default: auto)\n"
     << "  --temp-prefix=<str>            Per-test temp subdirectory prefix "
        "(default: fst)\n"
     << "  --large-dir-count=<N>          Entry count for large-directory "
        "test (default: 600, >=10)\n"
     << "  --async-timeout-ms=<ms>        Timeout for async/oplock operations "
        "(default: 5000)\n"
     << "  --allow-unknown-xfails         Warn instead of exit 2 on unknown "
        "baseline test IDs\n"
     << "  --verbose                      Print failure traces for expected "
        "XFAIL tests\n"
     << "  --help, -h                     Show this help message and exit 0\n"
     << "  --version                      Show version string and exit 0\n"
     << "  --gtest_*                      Forwarded directly to GoogleTest\n\n"
     << "Exit codes:\n"
     << "  0  Pass (no unexpected FAIL, no disallowed XPASS)\n"
     << "  1  Regression (unexpected FAIL, fatal setup/teardown failure, or "
        "crash)\n"
     << "  2  Usage, configuration, manifest, cleanup, lost-mount, "
        "0-tests-executed,\n"
     << "     or all-tests-skipped error\n"
     << "  3  Unexpected pass (disallowed XPASS under --xpass-policy=fail)\n";
  return os.str();
}

std::string FormatVersionText() { return std::string(kVersionString); }

} // namespace fstest::cli
