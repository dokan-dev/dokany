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

// Splits "--flag=value" into flag/value, or consumes args[++index] for
// "--flag value". Returns false if a required value is missing or looks like
// another option ("--json-out --verbose" must not create a file "--verbose").
bool ExtractFlagValue(const std::vector<std::string> &args, std::size_t *index,
                      std::string_view flag_name, bool has_equals,
                      std::string_view inline_value, std::string *value_out,
                      std::string *error_out) {
  if (has_equals) {
    *value_out = std::string(inline_value);
  } else {
    if (*index + 1 >= args.size()) {
      *error_out = "Missing value for flag '" + std::string(flag_name) + "'";
      return false;
    }
    ++(*index);
    *value_out = args[*index];
  }
  if (std::string_view(*value_out).starts_with("--")) {
    *error_out = "Invalid value for flag '" + std::string(flag_name) + "': '" +
                 *value_out + "' looks like an option (missing value?)";
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

  if (!args.empty()) {
    opts.gtest_args.push_back(args[0]);
  } else {
    opts.gtest_args.push_back("dokan_fstest");
  }

  bool saw_target_arch = false;
  int non_utility_flag_count = 0;
  std::set<std::string, std::less<>> seen_value_flags;

  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string &raw = args[i];
    const std::string_view arg(raw);

    // Forward all --gtest_* flags untouched while inspecting filter/list/repeat
    if (arg.starts_with("--gtest_")) {
      ++non_utility_flag_count;
      opts.gtest_args.push_back(raw);
      if (arg == "--gtest_list_tests") {
        opts.gtest_list_tests = true;
      } else if (arg.starts_with("--gtest_list_tests=")) {
        opts.gtest_list_tests = ParseGtestBoolValue(
            arg.substr(std::string_view("--gtest_list_tests=").size()));
      } else if (arg.starts_with("--gtest_filter=")) {
        // Only GoogleTest's default "*" selects every test; anything else
        // (including an empty filter, which selects nothing) is a filter.
        opts.gtest_has_filter =
            arg.substr(std::string_view("--gtest_filter=").size()) != "*";
      } else if (arg == "--gtest_fail_fast") {
        opts.gtest_fail_fast = true;
      } else if (arg.starts_with("--gtest_fail_fast=")) {
        opts.gtest_fail_fast = ParseGtestBoolValue(
            arg.substr(std::string_view("--gtest_fail_fast=").size()));
      } else if (arg.starts_with("--gtest_repeat=")) {
        int repeat_val = 1;
        const std::string_view num_str =
            arg.substr(std::string_view("--gtest_repeat=").size());
        if (!ParseIntStrict(num_str, &repeat_val)) {
          return MakeError("Invalid integer value for --gtest_repeat: '" +
                           std::string(num_str) + "'");
        }
        opts.gtest_repeat = repeat_val;
      }
      continue;
    }

    if (arg == "-h" || arg == "--help") {
      opts.show_help = true;
      continue;
    }
    if (arg == "--version") {
      opts.show_version = true;
      continue;
    }

    if (!arg.starts_with("--")) {
      return MakeError("Unknown argument: '" + raw + "'");
    }

    const std::size_t eq_pos = arg.find('=');
    const bool has_eq = (eq_pos != std::string_view::npos);
    const std::string_view flag = has_eq ? arg.substr(0, eq_pos) : arg;
    const std::string_view inline_val =
        has_eq ? arg.substr(eq_pos + 1) : std::string_view{};

    // Boolean flags (reject "=value")
    if (flag == "--strict" || flag == "--respect-fs-caps" ||
        flag == "--allow-unknown-xfails" || flag == "--verbose" ||
        flag == "--prune-passing") {
      ++non_utility_flag_count;
      if (has_eq) {
        return MakeError("Flag '" + std::string(flag) +
                         "' does not take a value");
      }
      if (flag == "--strict") {
        opts.strict = true;
      } else if (flag == "--respect-fs-caps") {
        opts.respect_fs_caps = true;
      } else if (flag == "--allow-unknown-xfails") {
        opts.allow_unknown_xfails = true;
      } else if (flag == "--verbose") {
        opts.verbose = true;
      } else if (flag == "--prune-passing") {
        opts.prune_passing = true;
      }
      continue;
    }

    // Every remaining flag takes (at most) one value: reject duplicates instead
    // of silently letting the last occurrence win.
    if (!seen_value_flags.emplace(flag).second) {
      return MakeError("Flag '" + std::string(flag) +
                       "' may only be specified once");
    }

    // Optional-value flag: --update-baseline[=<path>]
    if (flag == "--update-baseline") {
      ++non_utility_flag_count;
      opts.update_baseline = true;
      if (has_eq) {
        if (inline_val.empty()) {
          return MakeError(
              "Flag '--update-baseline=' requires a non-empty path");
        }
        if (inline_val.starts_with("--")) {
          return MakeError("Invalid value for flag '--update-baseline': '" +
                           std::string(inline_val) +
                           "' looks like an option (missing value?)");
        }
        opts.update_baseline_path = std::string(inline_val);
      } else if (i + 1 < args.size() && !args[i + 1].empty() &&
                 args[i + 1][0] != '-') {
        ++i;
        opts.update_baseline_path = args[i];
      }
      continue;
    }

    // Value-taking flags
    std::string val;
    std::string val_err;
    if (flag == "--root") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val.empty()) {
        return MakeError("Flag '--root' requires a non-empty path");
      }
      opts.root = std::move(val);
    } else if (flag == "--cleanup-dir") {
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val.empty()) {
        return MakeError("Flag '--cleanup-dir' requires a non-empty path");
      }
      opts.cleanup_dir = std::move(val);
    } else if (flag == "--validate-baseline") {
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val.empty()) {
        return MakeError(
            "Flag '--validate-baseline' requires a non-empty path");
      }
      opts.validate_baseline_path = std::move(val);
    } else if (flag == "--baseline") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val.empty()) {
        return MakeError("Flag '--baseline' requires a non-empty path");
      }
      opts.baseline_path = std::move(val);
    } else if (flag == "--target") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val.empty()) {
        return MakeError("Flag '--target' requires a non-empty name");
      }
      opts.target = std::move(val);
    } else if (flag == "--target-arch") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (!IsValidTargetArch(val)) {
        return MakeError("Invalid value for '--target-arch': '" + val +
                         "' (expected one of: x64, x86, arm64)");
      }
      opts.target_arch = std::move(val);
      saw_target_arch = true;
    } else if (flag == "--xpass-policy") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val == "fail") {
        opts.xpass_policy = XpassPolicy::kFail;
      } else if (val == "warn") {
        opts.xpass_policy = XpassPolicy::kWarn;
      } else {
        return MakeError("Invalid value for '--xpass-policy': '" + val +
                         "' (expected 'fail' or 'warn')");
      }
    } else if (flag == "--json-out") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val.empty()) {
        return MakeError("Flag '--json-out' requires a non-empty path");
      }
      opts.json_out_path = std::move(val);
    } else if (flag == "--default-reason") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (IsBlank(val)) {
        return MakeError("Flag '--default-reason' requires a non-empty string");
      }
      opts.default_reason = std::move(val);
    } else if (flag == "--is-volume-root") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (val == "auto") {
        opts.is_volume_root = VolumeRootMode::kAuto;
      } else if (val == "true") {
        opts.is_volume_root = VolumeRootMode::kTrue;
      } else if (val == "false") {
        opts.is_volume_root = VolumeRootMode::kFalse;
      } else {
        return MakeError("Invalid value for '--is-volume-root': '" + val +
                         "' (expected 'auto', 'true', or 'false')");
      }
    } else if (flag == "--temp-prefix") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      if (!IsValidTempPrefix(val)) {
        return MakeError("Invalid value for '--temp-prefix': '" + val + "'");
      }
      opts.temp_prefix = std::move(val);
    } else if (flag == "--large-dir-count") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      uint32_t count = 0;
      if (!ParseUint32Strict(val, &count) || count < 10) {
        return MakeError("Invalid value for '--large-dir-count': '" + val +
                         "' (expected integer >= 10)");
      }
      opts.large_dir_count = count;
    } else if (flag == "--async-timeout-ms") {
      ++non_utility_flag_count;
      if (!ExtractFlagValue(args, &i, flag, has_eq, inline_val, &val,
                            &val_err)) {
        return MakeError(val_err);
      }
      uint32_t ms = 0;
      if (!ParseUint32Strict(val, &ms) || ms == 0) {
        return MakeError("Invalid value for '--async-timeout-ms': '" + val +
                         "' (expected positive integer)");
      }
      opts.async_timeout_ms = ms;
    } else {
      return MakeError("Unknown flag: '" + std::string(flag) + "'");
    }
  }

  if (!saw_target_arch) {
    opts.target_arch = DefaultBinaryArch();
  }

  // Early-exit modes do not require --root or cross-flag checks.
  if (opts.show_help || opts.show_version) {
    result.ok = true;
    return result;
  }

  // Utility mode: --cleanup-dir=<path>
  if (!opts.cleanup_dir.empty()) {
    if (!opts.validate_baseline_path.empty() || non_utility_flag_count > 0) {
      return MakeError(
          "Flag '--cleanup-dir' cannot be combined with other flags");
    }
    result.ok = true;
    return result;
  }

  // Utility mode: --validate-baseline=<path>
  if (!opts.validate_baseline_path.empty()) {
    if (non_utility_flag_count > 0) {
      return MakeError(
          "Flag '--validate-baseline' cannot be combined with other flags");
    }
    result.ok = true;
    return result;
  }

  // Listing mode: --gtest_list_tests does not require --root.
  if (opts.gtest_list_tests) {
    result.ok = true;
    return result;
  }

  // Normal test execution validation.
  if (opts.root.empty() && !env_root.empty()) {
    opts.root = std::string(env_root);
  }
  if (opts.root.empty()) {
    return MakeError(
        "Missing required flag '--root=<path>' (or set FSTEST_ROOT)");
  }

  if (opts.strict) {
    if (!opts.baseline_path.empty() || !opts.target.empty() ||
        opts.update_baseline) {
      return MakeError(
          "Flag '--strict' is mutually exclusive with '--baseline', "
          "'--target', and '--update-baseline'");
    }
  }

  if (!opts.target.empty() && opts.baseline_path.empty()) {
    return MakeError("Flag '--target' requires '--baseline=<path>'");
  }

  if (opts.gtest_fail_fast && !opts.baseline_path.empty()) {
    return MakeError(
        "Flag '--gtest_fail_fast' cannot be used with '--baseline' (the first "
        "expected XFAIL would skip every remaining test)");
  }

  if (opts.prune_passing && !opts.update_baseline) {
    return MakeError("Flag '--prune-passing' requires '--update-baseline'");
  }

  if (opts.update_baseline) {
    if (opts.baseline_path.empty()) {
      return MakeError("Flag '--update-baseline' requires '--baseline=<path>'");
    }
    if (opts.gtest_has_filter) {
      return MakeError("Flag '--update-baseline' cannot be used with a "
                       "non-default '--gtest_filter' (only '*' is allowed)");
    }
    if (opts.gtest_repeat != 1) {
      return MakeError(
          "Flag '--update-baseline' cannot be used with '--gtest_repeat != 1'");
    }
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
