// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "baseline.h"
#include "cli_args.h"
#include "fstest/core/capabilities.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"
#include "fstest/core/win_headers.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_config.h"
#include "fstest/suite/test_metadata.h"
#include "gtest_filter.h"
#include "mount_guard.h"
#include "scorecard.h"
#include "xfail_listener.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::string ReadEnvVarUtf8(const wchar_t *name) {
  const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::wstring buf(needed, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name, buf.data(), needed);
  if (written == 0 || written >= needed) {
    return {};
  }
  buf.resize(written);
  return fstest::core::WideToUtf8(buf);
}

bool VerifyDirectoryExists(const fstest::core::NormalizedRoot &norm,
                           const std::string &display_path,
                           const char *flag_name, std::string *error_out) {
  const DWORD err = fstest::core::ProbeRootDirectory(norm);
  if (err == ERROR_SUCCESS) {
    return true;
  }
  if (err == ERROR_DIRECTORY) {
    *error_out = std::string(flag_name) + " path is not a directory: \"" +
                 display_path + "\"";
  } else {
    *error_out = std::string(flag_name) + " path is not reachable: \"" +
                 display_path + "\" (" + fstest::core::FormatWin32Error(err) +
                 ")";
  }
  return false;
}

// Parses a GoogleTest sharding environment variable; returns -1 when unset or
// malformed (GoogleTest itself aborts on malformed values).
int32_t ReadShardEnvVar(const wchar_t *name) {
  const std::string text = ReadEnvVarUtf8(name);
  if (text.empty()) {
    return -1;
  }
  int32_t value = -1;
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return -1;
  }
  return value;
}

std::optional<bool> ToVolumeRootOverride(fstest::cli::VolumeRootMode mode) {
  switch (mode) {
  case fstest::cli::VolumeRootMode::kAuto:
    return std::nullopt;
  case fstest::cli::VolumeRootMode::kTrue:
    return true;
  case fstest::cli::VolumeRootMode::kFalse:
    return false;
  }
  return std::nullopt;
}

int RunValidateBaselineMode(const std::string &manifest_path) {
  fstest::cli::BaselineManifest manifest;
  std::string err;
  if (!fstest::cli::LoadBaselineManifestFile(manifest_path, &manifest, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 2;
  }

  const std::unordered_set<std::string> registered_ids =
      fstest::cli::CollectRegisteredTestIds(::testing::UnitTest::GetInstance());

  std::size_t concrete_targets = 0;
  std::size_t total_entries = 0;
  std::set<std::string> unique_ids;

  for (const std::string &target_name : manifest.target_order) {
    const auto it = manifest.targets.find(target_name);
    if (it == manifest.targets.end()) {
      continue;
    }
    const fstest::cli::TargetDefinition &def = it->second;
    if (def.is_abstract) {
      std::vector<std::string> unknown_abstract;
      for (const auto &[id, _] : def.xfails) {
        if (!registered_ids.contains(id)) {
          unknown_abstract.push_back(id);
        }
      }
      if (!unknown_abstract.empty()) {
        std::cerr << "Error: Abstract baseline target \"" << target_name
                  << "\" references " << unknown_abstract.size()
                  << " unknown test ID(s): " << unknown_abstract.front()
                  << "\n";
        return 2;
      }
      continue;
    }

    ++concrete_targets;
    fstest::cli::ResolvedBaseline resolved;
    if (!fstest::cli::ResolveBaselineFromLoadedManifest(
            manifest, manifest_path, target_name, /*os_build=*/0,
            /*target_arch=*/"x64", &resolved, &err)) {
      std::cerr << "Error: " << err << "\n";
      return 2;
    }
    std::vector<std::string> unknown_ids;
    if (!fstest::cli::ValidateBaselineTestIds(resolved, registered_ids,
                                              /*allow_unknown=*/false,
                                              &unknown_ids, &err)) {
      std::cerr << "Error: " << err << "\n";
      return 2;
    }
    total_entries += resolved.all_entries.size();
    for (const auto &[id, _] : resolved.all_entries) {
      unique_ids.insert(id);
    }
  }

  if (concrete_targets == 0) {
    std::cerr << "Error: Baseline manifest \"" << manifest_path
              << "\" contains no non-abstract targets\n";
    return 2;
  }

  std::cout << "Baseline valid: \"" << manifest_path << "\" ("
            << concrete_targets << " concrete target(s), " << total_entries
            << " total entries, " << unique_ids.size()
            << " unique test IDs verified against " << registered_ids.size()
            << " registered tests)\n";
  return 0;
}

int RunCleanupDirMode(const std::string &cleanup_dir) {
  const std::wstring target_w = fstest::core::Utf8ToWide(cleanup_dir);
  std::wstring canonical_target;
  std::string unsafe_reason;
  if (!fstest::core::ResolveSafeCleanupTarget(target_w, &canonical_target,
                                              &unsafe_reason)) {
    std::cerr << "Error: Refused unsafe --cleanup-dir target \"" << cleanup_dir
              << "\": " << unsafe_reason << "\n";
    return 2;
  }
  if (canonical_target.empty()) {
    // Target does not exist: nothing to delete.
    return 0;
  }
  // Delete exactly the canonical directory that was validated (never
  // re-resolve the raw user input).
  std::string rm_err;
  if (!fstest::core::ForceRemoveAll(canonical_target,
                                    fstest::core::CleanupOptions{}, &rm_err)) {
    std::cerr << "Error: Failed to remove \"" << cleanup_dir << "\": " << rm_err
              << "\n";
    return 2;
  }
  return 0;
}

std::vector<std::string> ConvertArgsToUtf8(int argc, wchar_t **argv) {
  std::vector<std::string> utf8_args;
  utf8_args.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
  for (int i = 0; i < argc; ++i) {
    utf8_args.push_back(
        fstest::core::WideToUtf8(argv[i] != nullptr ? argv[i] : L""));
  }
  return utf8_args;
}

bool InitGtestAndCheckFlags(const std::vector<std::string> &gtest_args) {
  std::vector<std::string> gtest_storage = gtest_args;
  std::vector<char *> gtest_argv;
  gtest_argv.reserve(gtest_storage.size() + 1);
  for (std::string &arg : gtest_storage) {
    gtest_argv.push_back(arg.data());
  }
  gtest_argv.push_back(nullptr);

  int gtest_argc = static_cast<int>(gtest_storage.size());
  ::testing::InitGoogleTest(&gtest_argc, gtest_argv.data());
  if (gtest_argc > 1) {
    std::cerr << "Error: Unrecognized GoogleTest flag: '" << gtest_argv[1]
              << "'\n";
    return false;
  }
  return true;
}

bool ValidateUpdateBaselineRuntimeFlags() {
  if (!ReadEnvVarUtf8(L"GTEST_TOTAL_SHARDS").empty() ||
      !ReadEnvVarUtf8(L"GTEST_SHARD_INDEX").empty()) {
    std::cerr << "Error: Flag '--update-baseline' cannot be used with "
                 "GoogleTest sharding (unset GTEST_TOTAL_SHARDS and "
                 "GTEST_SHARD_INDEX): each shard only observes a subset of "
                 "tests\n";
    return false;
  }
  // Only GoogleTest's default "*" runs every test; an empty filter runs none.
  const std::string &effective_filter = ::testing::GTEST_FLAG(filter);
  if (effective_filter != "*") {
    std::cerr << "Error: Flag '--update-baseline' cannot be used with "
                 "non-default GoogleTest filter '"
              << effective_filter
              << "' (unset GTEST_FILTER and omit --gtest_filter)\n";
    return false;
  }
  const int32_t effective_repeat = ::testing::GTEST_FLAG(repeat);
  if (effective_repeat != 1) {
    std::cerr << "Error: Flag '--update-baseline' cannot be used with "
                 "GoogleTest repeat count "
              << effective_repeat
              << " (unset GTEST_REPEAT and omit --gtest_repeat)\n";
    return false;
  }
  return true;
}

bool LoadAndValidateBaseline(const fstest::cli::CliOptions &opts,
                             uint32_t os_build,
                             fstest::cli::BaselineManifest *manifest,
                             fstest::cli::ResolvedBaseline *resolved) {
  std::string baseline_err;
  if (!fstest::cli::LoadBaselineManifestFile(opts.baseline_path, manifest,
                                             &baseline_err) ||
      !fstest::cli::ResolveBaselineFromLoadedManifest(
          *manifest, opts.baseline_path, opts.target, os_build,
          opts.target_arch, resolved, &baseline_err)) {
    std::cerr << "Error: " << baseline_err << "\n";
    return false;
  }

  // Validate active baseline test IDs against ALL compiled/registered tests
  // (before --gtest_filter).
  const std::unordered_set<std::string> registered_ids =
      fstest::cli::CollectRegisteredTestIds(::testing::UnitTest::GetInstance());
  std::vector<std::string> unknown_ids;
  if (!fstest::cli::ValidateBaselineTestIds(*resolved, registered_ids,
                                            opts.allow_unknown_xfails,
                                            &unknown_ids, &baseline_err)) {
    std::cerr << "Error: " << baseline_err << "\n";
    return false;
  }
  if (!unknown_ids.empty()) {
    std::cerr << "Warning: Baseline target \"" << resolved->target_name
              << "\" references " << unknown_ids.size()
              << " unknown test ID(s):";
    for (const std::string &id : unknown_ids) {
      std::cerr << " " << id;
    }
    std::cerr << "\n";
  }
  return true;
}

void PreRecordSkipCrashTests(const fstest::cli::ResolvedBaseline &resolved,
                             const fstest::cli::ShardSpec &shard,
                             int32_t repeat_count,
                             fstest::cli::Scorecard *scorecard) {
  const std::string active_filter = ::testing::GTEST_FLAG(filter);
  std::set<std::string> skip_crash_ids;
  for (const auto &[test_id, entry] : resolved.active_entries) {
    if (entry.skip_crash) {
      skip_crash_ids.insert(test_id);
    }
  }
  // Every shard excludes all skip_crash tests from execution, but only the
  // owning shard pre-records each one (and never DISABLED_ tests GoogleTest
  // would not run), so merged shard reports count each exactly once.
  const std::vector<std::string> pre_record_ids =
      fstest::cli::SelectSkipCrashPreRecords(
          fstest::cli::CollectRegisteredTestIdsInOrder(
              ::testing::UnitTest::GetInstance()),
          skip_crash_ids, active_filter,
          GTEST_FLAG_GET(also_run_disabled_tests), shard);
  for (const std::string &test_id : pre_record_ids) {
    const fstest::cli::XfailEntry &entry = resolved.active_entries.at(test_id);
    const std::size_t dot = test_id.find('.');
    const std::string_view suite_name =
        (dot == std::string::npos) ? std::string_view(test_id)
                                   : std::string_view(test_id).substr(0, dot);

    std::cout << "[  XFAIL ] " << test_id << " (0 ms) - [skip_crash] "
              << entry.reason << "\n";

    fstest::cli::TestRecord rec;
    rec.id = test_id;
    rec.domain = fstest::SuiteNameToDomainId(suite_name);
    rec.outcome = fstest::cli::TestOutcome::kXFail;
    rec.skip_crash = true;
    rec.xfail_reason = entry.reason;
    if (repeat_count > 1) {
      rec.iterations = static_cast<uint32_t>(repeat_count);
    }
    scorecard->RecordTest(std::move(rec));
  }
  if (!skip_crash_ids.empty()) {
    std::vector<std::string> crash_exclude_ids(skip_crash_ids.begin(),
                                               skip_crash_ids.end());
    std::cout.flush();
    ::testing::GTEST_FLAG(filter) = fstest::cli::AppendNegativeGtestFilter(
        active_filter, crash_exclude_ids);
  }
}

fstest::cli::ScorecardMetadata
BuildScorecardMetadata(const fstest::cli::CliOptions &opts,
                       const fstest::core::NormalizedRoot &norm_root,
                       const fstest::core::OsInfo &os_info,
                       const fstest::core::VolumeCapabilities &caps,
                       const std::string &target_name, int64_t duration_ms) {
  fstest::cli::ScorecardMetadata meta;
  meta.root_path = fstest::core::WideToUtf8(norm_root.win32_root);
  meta.mode = opts.baseline_path.empty() ? "strict" : "baseline";
  meta.baseline_path = opts.baseline_path;
  meta.target = target_name;
  meta.target_arch = opts.target_arch;
  meta.xpass_policy = opts.xpass_policy;
  meta.duration_ms = duration_ms;

  meta.os.major = os_info.major;
  meta.os.minor = os_info.minor;
  meta.os.build = os_info.build;
  meta.os.ubr = os_info.ubr;
  meta.os.is_server = os_info.is_server;
  meta.os.host_arch =
      std::string(fstest::core::ArchToString(os_info.host_arch));
  meta.os.process_arch =
      std::string(fstest::core::ArchToString(os_info.process_arch));
  meta.os.is_elevated = os_info.is_elevated;
  meta.os.can_create_symlinks = os_info.can_create_symlinks;

  meta.capabilities.filesystem_name =
      fstest::core::WideToUtf8(caps.filesystem_name);
  meta.capabilities.max_component_length = caps.max_component_length;
  meta.capabilities.fs_flags = caps.fs_flags;
  meta.capabilities.bytes_per_sector = caps.bytes_per_sector;
  meta.capabilities.physical_bytes_per_sector = caps.physical_bytes_per_sector;
  meta.capabilities.bytes_per_cluster = caps.bytes_per_cluster;
  meta.capabilities.is_volume_root = norm_root.is_volume_root;
  meta.capabilities.short_names_enabled = caps.short_names_enabled;
  meta.capabilities.respect_fs_capabilities = opts.respect_fs_caps;
  return meta;
}

void WarnUnmaskableBaselineSkips(
    const std::vector<fstest::cli::ObservedTestResult> &observed) {
  uint32_t unmaskable_count = 0;
  for (const auto &obs : observed) {
    if (obs.fatal_teardown || obs.fatal_setup || obs.exception) {
      ++unmaskable_count;
    }
  }
  if (unmaskable_count > 0) {
    std::cerr << "Warning: " << unmaskable_count
              << " test(s) had fatal setup/teardown failures or crashed and "
                 "were NOT added to or removed from baseline xfails.\n";
  }
}

bool ApplyBaselineUpdate(const fstest::cli::CliOptions &opts,
                         const fstest::cli::Scorecard &scorecard,
                         const fstest::core::OsInfo &os_info,
                         const fstest::cli::ResolvedBaseline &resolved_baseline,
                         fstest::cli::BaselineManifest *manifest) {
  const std::vector<fstest::cli::ObservedTestResult> observed =
      scorecard.ToObservedResults();
  WarnUnmaskableBaselineSkips(observed);

  std::string update_err;
  fstest::cli::BaselineUpdateOptions update_opts;
  update_opts.prune_passing = opts.prune_passing;
  fstest::cli::BaselineUpdateReport update_report;
  const std::string out_path = opts.update_baseline_path.empty()
                                   ? opts.baseline_path
                                   : opts.update_baseline_path;
  if (!fstest::cli::UpdateBaselineManifest(
          manifest, resolved_baseline, os_info.build, opts.target_arch,
          opts.default_reason, observed, update_opts, &update_report,
          &update_err) ||
      !fstest::cli::RebaseExtendsFilePaths(manifest, opts.baseline_path,
                                           out_path, &update_err)) {
    std::cerr << "Error: Failed to update baseline: " << update_err << "\n";
    return false;
  }

  std::cout << fstest::cli::FormatBaselineUpdateReport(update_report,
                                                       opts.prune_passing);
  const std::string serialized =
      fstest::cli::SerializeBaselineManifest(*manifest);
  if (serialized.empty()) {
    std::cerr << "Error: Failed to serialize updated baseline manifest\n";
    return false;
  }
  if (!fstest::cli::WriteTextFileUtf8(out_path, serialized, &update_err)) {
    std::cerr << "Error: Failed to write updated baseline \"" << out_path
              << "\": " << update_err << "\n";
    return false;
  }
  std::cout << "Updated baseline written to: " << out_path << "\n";
  return true;
}

void PrintRunDiagnostics(const fstest::cli::DomainCounts &summary,
                         const fstest::cli::MountLivenessGuard &mount_guard) {
  if (summary.total == 0) {
    std::cerr << "Error: 0 tests executed (check --gtest_filter or suite "
                 "registration)\n";
  } else if (summary.total == summary.skip + summary.crash_skipped) {
    std::cerr << "Error: every selected test was skipped or crash-skipped "
                 "(nothing was actually verified)\n";
  }
  if (mount_guard.mount_lost()) {
    std::cerr << "Error: " << mount_guard.lost_message()
              << " (remaining tests were not run)\n";
  }
}

bool ResolveAndProbeTarget(const fstest::cli::CliOptions &opts,
                           std::optional<bool> vol_override,
                           fstest::core::NormalizedRoot *norm_root_out,
                           fstest::core::VolumeCapabilities *caps_out) {
  const fstest::core::Win32Result<fstest::core::NormalizedRoot> norm_root =
      fstest::core::NormalizeRootPath(fstest::core::Utf8ToWide(opts.root),
                                      vol_override);
  if (!norm_root.ok()) {
    std::cerr << "Error: Invalid --root path \"" << opts.root << "\" ("
              << fstest::core::FormatWin32Error(norm_root.error) << ")\n";
    return false;
  }

  std::string dir_err;
  if (!VerifyDirectoryExists(*norm_root.value, opts.root, "--root", &dir_err)) {
    std::cerr << "Error: " << dir_err << "\n";
    return false;
  }

  const fstest::core::OsInfo &os_info = fstest::core::GetOsInfo();
  const fstest::core::VolumeCapabilities caps =
      fstest::core::ProbeVolumeCapabilities(*norm_root.value, os_info);
  if (!caps.probe_succeeded) {
    std::cerr << "Error: Failed to probe target volume capabilities for \""
              << opts.root << "\": "
              << (caps.probe_error.empty() ? "unknown error" : caps.probe_error)
              << "\n";
    return false;
  }

  *norm_root_out = *norm_root.value;
  *caps_out = caps;
  return true;
}

fstest::TestConfig
BuildSuiteConfig(const fstest::cli::CliOptions &opts,
                 const fstest::core::NormalizedRoot &norm_root,
                 std::optional<bool> vol_override,
                 const fstest::core::VolumeCapabilities &caps) {
  fstest::TestConfig suite_cfg;
  suite_cfg.root_path = norm_root.win32_root;
  suite_cfg.is_volume_root = vol_override;
  suite_cfg.preprobed_capabilities = caps;
  suite_cfg.temp_prefix = fstest::core::Utf8ToWide(opts.temp_prefix);
  suite_cfg.large_dir_entry_count = opts.large_dir_count;
  suite_cfg.respect_fs_capabilities = opts.respect_fs_caps;
  suite_cfg.timing.async_timeout_ms = opts.async_timeout_ms;
  return suite_cfg;
}

struct ConformanceRunContext {
  explicit ConformanceRunContext(const fstest::cli::CliOptions &opts_in)
      : opts(opts_in) {}

  const fstest::cli::CliOptions &opts;
  fstest::core::NormalizedRoot norm_root;
  fstest::core::OsInfo os_info;
  fstest::core::VolumeCapabilities caps;
  fstest::cli::BaselineManifest manifest;
  fstest::cli::ResolvedBaseline resolved_baseline;
  bool has_baseline = false;
  fstest::cli::Scorecard scorecard;
};

bool MaybeApplyBaselineUpdate(ConformanceRunContext *ctx,
                              const fstest::cli::DomainCounts &summary,
                              const fstest::cli::MountLivenessGuard &guard) {
  if (!ctx->opts.update_baseline) {
    return true;
  }
  if (guard.mount_lost()) {
    std::cerr << "Error: Baseline NOT updated: " << guard.lost_message()
              << "\n";
    return true;
  }
  if (summary.total == 0) {
    return true;
  }
  return ApplyBaselineUpdate(ctx->opts, ctx->scorecard, ctx->os_info,
                             ctx->resolved_baseline, &ctx->manifest);
}

int FinalizeAndReportRun(ConformanceRunContext *ctx,
                         const fstest::cli::MountLivenessGuard &mount_guard,
                         int64_t duration_ms) {
  const fstest::cli::DomainCounts summary = ctx->scorecard.Summary();
  int exit_code = fstest::cli::ComputeExitCode(summary, ctx->opts.xpass_policy);
  if (mount_guard.mount_lost()) {
    // Infrastructure failure: the filesystem under test went away.
    exit_code = 2;
  }
  if (!MaybeApplyBaselineUpdate(ctx, summary, mount_guard)) {
    exit_code = 2;
  }

  fstest::cli::ScorecardMetadata meta = BuildScorecardMetadata(
      ctx->opts, ctx->norm_root, ctx->os_info, ctx->caps,
      ctx->has_baseline ? ctx->resolved_baseline.target_name : "", duration_ms);
  meta.exit_code = exit_code;
  if (!ctx->opts.json_out_path.empty()) {
    std::string write_err;
    if (!fstest::cli::WriteScorecardJsonFile(ctx->opts.json_out_path, meta,
                                             ctx->scorecard, &write_err)) {
      std::cerr << "Error: Failed to write --json-out: " << write_err << "\n";
      exit_code = 2;
      meta.exit_code = exit_code;
    }
  }

  std::cout << fstest::cli::FormatScorecardTable(meta, ctx->scorecard);
  PrintRunDiagnostics(summary, mount_guard);
  return exit_code;
}

int ExecuteConformanceRun(const fstest::cli::CliOptions &opts,
                          fstest::cli::SelfTestFault selftest_fault) {
  ConformanceRunContext ctx(opts);
  const std::optional<bool> vol_override =
      ToVolumeRootOverride(opts.is_volume_root);
  if (!ResolveAndProbeTarget(opts, vol_override, &ctx.norm_root, &ctx.caps)) {
    return 2;
  }

  ctx.os_info = fstest::core::GetOsInfo();
  ctx.has_baseline = !opts.baseline_path.empty();
  if (ctx.has_baseline &&
      !LoadAndValidateBaseline(opts, ctx.os_info.build, &ctx.manifest,
                               &ctx.resolved_baseline)) {
    return 2;
  }

  fstest::TestConfig suite_cfg =
      BuildSuiteConfig(opts, ctx.norm_root, vol_override, ctx.caps);
  fstest::cli::MountLivenessGuard mount_guard(
      [&norm_root = ctx.norm_root]() {
        return fstest::core::ProbeRootDirectory(norm_root);
      },
      opts.root, []() { GTEST_FLAG_SET(fail_fast, true); }, selftest_fault,
      [](const std::string &msg) { GTEST_SKIP() << msg; });
  mount_guard.Install(&suite_cfg.hooks);
  fstest::ConfigureSuite(std::move(suite_cfg));

  const int32_t repeat_count = ::testing::GTEST_FLAG(repeat);
  if (ctx.has_baseline && repeat_count != 0) {
    const fstest::cli::ShardSpec shard{ReadShardEnvVar(L"GTEST_TOTAL_SHARDS"),
                                       ReadShardEnvVar(L"GTEST_SHARD_INDEX")};
    PreRecordSkipCrashTests(ctx.resolved_baseline, shard, repeat_count,
                            &ctx.scorecard);
  }

  ::testing::TestEventListeners &listeners =
      ::testing::UnitTest::GetInstance()->listeners();
  delete listeners.Release(listeners.default_result_printer());

  const std::map<std::string, fstest::cli::XfailEntry> *active_xfails =
      ctx.has_baseline ? &ctx.resolved_baseline.active_entries : nullptr;
  auto *xfail_listener = new fstest::cli::XfailEventListener(
      &ctx.scorecard, active_xfails, opts.verbose, &std::cout);
  listeners.Append(xfail_listener);

  const auto start_tp = std::chrono::steady_clock::now();
  (void)RUN_ALL_TESTS();
  const auto end_tp = std::chrono::steady_clock::now();
  const int64_t duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_tp - start_tp)
          .count();

  delete listeners.Release(xfail_listener);
  // The guard's hooks reference stack state; uninstall before it goes away.
  fstest::ResetSuiteConfig();

  return FinalizeAndReportRun(&ctx, mount_guard, duration_ms);
}

bool TryHandleEarlyCliMode(const fstest::cli::CliOptions &opts,
                           const std::vector<std::string> &utf8_args,
                           int *exit_code_out) {
  if (opts.show_help) {
    const std::string_view program_name =
        utf8_args.empty() ? std::string_view("dokan_fstest.exe")
                          : std::string_view(utf8_args[0]);
    std::cout << fstest::cli::FormatHelpText(program_name);
    *exit_code_out = 0;
    return true;
  }
  if (opts.show_version) {
    std::cout << fstest::cli::FormatVersionText() << "\n";
    *exit_code_out = 0;
    return true;
  }
  if (!opts.cleanup_dir.empty()) {
    *exit_code_out = RunCleanupDirMode(opts.cleanup_dir);
    return true;
  }
  return false;
}

bool ParseSelfTestFaultEnv(fstest::cli::SelfTestFault *fault_out) {
  const std::string fault_env = ReadEnvVarUtf8(L"FSTEST_SELFTEST_FAULT");
  if (!fstest::cli::ParseSelfTestFault(fault_env, fault_out)) {
    std::cerr << "Error: Invalid FSTEST_SELFTEST_FAULT value '" << fault_env
              << "'\n";
    return false;
  }
  return true;
}

bool ValidateRunPreconditions(const fstest::cli::CliOptions &opts) {
  if (!opts.baseline_path.empty() && GTEST_FLAG_GET(fail_fast)) {
    std::cerr << "Error: GoogleTest fail-fast mode (--gtest_fail_fast or "
                 "GTEST_FAIL_FAST) cannot be used with '--baseline'\n";
    return false;
  }
  if (opts.update_baseline && !ValidateUpdateBaselineRuntimeFlags()) {
    return false;
  }
  return true;
}

int RunCliMain(int argc, wchar_t **argv) {
  // Force linker retention of all 12 domain suites even without /WHOLEARCHIVE.
  fstest::LinkAllSuites();

  const std::vector<std::string> utf8_args = ConvertArgsToUtf8(argc, argv);
  const std::string env_root = ReadEnvVarUtf8(L"FSTEST_ROOT");
  const bool env_gtest_list =
      fstest::cli::IsGtestEnvBoolTrue(ReadEnvVarUtf8(L"GTEST_LIST_TESTS"));
  const fstest::cli::CliParseResult parsed =
      fstest::cli::ParseCliArgs(utf8_args, env_root, env_gtest_list);
  if (!parsed.ok) {
    std::cerr << "Error: " << parsed.error << "\n"
              << "Run with --help for usage.\n";
    return 2;
  }

  const fstest::cli::CliOptions &opts = parsed.options;
  int early_exit = 0;
  if (TryHandleEarlyCliMode(opts, utf8_args, &early_exit)) {
    return early_exit;
  }
  if (!InitGtestAndCheckFlags(opts.gtest_args)) {
    return 2;
  }
  if (!opts.validate_baseline_path.empty()) {
    return RunValidateBaselineMode(opts.validate_baseline_path);
  }

  fstest::cli::SelfTestFault selftest_fault = fstest::cli::SelfTestFault::kNone;
  if (!ParseSelfTestFaultEnv(&selftest_fault)) {
    return 2;
  }
  if (opts.gtest_list_tests || ::testing::GTEST_FLAG(list_tests)) {
    ::testing::GTEST_FLAG(list_tests) = true;
    return RUN_ALL_TESTS();
  }
  if (!ValidateRunPreconditions(opts)) {
    return 2;
  }
  return ExecuteConformanceRun(opts, selftest_fault);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    return RunCliMain(argc, argv);
  } catch (const std::exception &ex) {
    std::cerr << "Error: Unhandled exception: " << ex.what() << "\n";
    return 2;
  } catch (...) {
    std::cerr << "Error: Unhandled unknown exception.\n";
    return 2;
  }
}
