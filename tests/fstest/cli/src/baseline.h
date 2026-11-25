// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include "gtest_filter.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace fstest::cli {

struct XfailEntry {
  std::string reason;
  // CONFORMANCE_GAPS.md citation ("GAP-<2+ digits>"); empty when absent.
  // Required when skip_crash or allow_xpass is set.
  std::string gap;
  std::optional<uint32_t> os_min_build;
  std::optional<uint32_t> os_max_build;
  std::vector<std::string> arch;
  bool allow_xpass = false;
  bool skip_crash = false;

  bool MatchesHost(uint32_t os_build, std::string_view target_arch) const;
};

struct TargetDefinition {
  std::string name;
  bool is_abstract = false;
  std::string description;
  std::string extends;
  std::string extends_file;
  std::map<std::string, XfailEntry> xfails;
  std::vector<std::string> remove_xfails;
};

struct BaselineManifest {
  uint32_t schema_version = 1;
  std::string description;
  std::vector<std::string> target_order;
  std::map<std::string, TargetDefinition> targets;
};

struct ResolvedBaseline {
  std::string manifest_path;
  std::string target_name;
  // All effective entries after inheritance (regardless of OS/arch predicates)
  std::map<std::string, XfailEntry> all_entries;
  // Entries whose OS/arch predicates match the current run
  std::map<std::string, XfailEntry> active_entries;
  // Resolved parent entries (empty if the target does not extend a parent)
  std::map<std::string, XfailEntry> parent_entries;
};

struct ObservedTestResult {
  std::string test_id;
  bool passed = false;
  bool failed = false;
  bool skipped = false;
  // fatal_setup, fatal_teardown, exception (SEH / C++ exception crash) and
  // skip_crash outcomes never add, remove, or change xfail entries.
  bool fatal_setup = false;
  bool fatal_teardown = false;
  bool exception = false;
  bool skip_crash = false;
};

struct BaselineUpdateOptions {
  // Also remove passing entries that still cover other hosts or builds
  // (unpredicated, multi-build, or single-arch-with-range entries).
  bool prune_passing = false;
};

struct BaselineUpdateReport {
  // Failed here; new xfail entry (or a suppressed parent entry re-enabled).
  std::vector<std::string> added;
  // Existing entry widened to cover this host.
  std::vector<std::string> widened;
  // Host arch dropped from a multi-arch list.
  std::vector<std::string> narrowed;
  // Entry deleted or suppressed via remove_xfails.
  std::vector<std::string> removed;
  // Passed here but kept (still covers other hosts); needs --prune-passing.
  std::vector<std::string> passed_kept;
};

// Reads a UTF-8 file from disk.
bool ReadTextFileUtf8(const std::string &file_path, std::string *content_out,
                      std::string *error_out);

// Writes a UTF-8 file to disk in binary mode (preserving LF endings), creating
// parent directories if needed.
bool WriteTextFileUtf8(const std::string &file_path, std::string_view content,
                       std::string *error_out);

// Parses and validates a schema_version 1 baseline manifest from JSON text.
bool ParseBaselineManifest(std::string_view json_text, BaselineManifest *out,
                           std::string *error_out);

// Loads and parses a baseline manifest file from disk.
bool LoadBaselineManifestFile(const std::string &file_path,
                              BaselineManifest *out, std::string *error_out);

// Resolves a target from an already-loaded manifest while using manifest_path
// to resolve any relative extends_file references (avoiding a redundant disk
// read of the root manifest). If requested_target is empty, auto-selects the
// sole non-abstract target.
bool ResolveBaselineFromLoadedManifest(const BaselineManifest &manifest,
                                       const std::string &manifest_path,
                                       std::string_view requested_target,
                                       uint32_t os_build,
                                       std::string_view target_arch,
                                       ResolvedBaseline *out,
                                       std::string *error_out);

// Resolves a target (including multi-level extends / extends_file inheritance,
// cycle detection, remove_xfails validation, and OS/arch predicate filtering).
// If requested_target is empty, auto-selects the sole non-abstract target.
bool ResolveBaselineFromFile(const std::string &file_path,
                             std::string_view requested_target,
                             uint32_t os_build, std::string_view target_arch,
                             ResolvedBaseline *out, std::string *error_out);

// Resolves a target within an already-parsed manifest (supports intra-file
// extends; rejects extends_file because no base filesystem path is provided).
bool ResolveBaselineInMemory(const BaselineManifest &manifest,
                             std::string_view requested_target,
                             uint32_t os_build, std::string_view target_arch,
                             ResolvedBaseline *out, std::string *error_out);

// Validates every effective baseline test ID (all_entries across all OS/arch
// predicates) against the full set of compiled / registered GTest test IDs
// (before --gtest_filter).
// Returns false if unknown IDs exist and allow_unknown is false.
bool ValidateBaselineTestIds(
    const ResolvedBaseline &baseline,
    const std::unordered_set<std::string> &registered_tests, bool allow_unknown,
    std::vector<std::string> *unknown_ids_out, std::string *error_out);

// Deterministically updates the target inside manifest using observed test
// outcomes without mutating any parent target, preserving unrelated OS/arch
// entries, skipped entries, allow_xpass entries, and existing human reasons.
//
// An active entry that passed on this host is only dropped for this host:
// a multi-arch list loses target_arch (narrowed); an entry pinned to exactly
// this arch and build (arch == {target_arch}, os_min_build == os_max_build ==
// os_build) is removed; any other entry (unpredicated, multi-build, ...) still
// covers other hosts and is kept and listed in passed_kept unless
// options.prune_passing is set. Inherited parent entries follow the same rule
// via remove_xfails. Every touched test ID is recorded in *report (nullable).
bool UpdateBaselineManifest(BaselineManifest *manifest,
                            const ResolvedBaseline &resolved, uint32_t os_build,
                            std::string_view target_arch,
                            std::string_view default_reason,
                            const std::vector<ObservedTestResult> &results,
                            const BaselineUpdateOptions &options,
                            BaselineUpdateReport *report,
                            std::string *error_out);

// Human-readable log: one line per removed/narrowed/added/widened id, plus a
// "passed on this host" section listing passed_kept with a hint to re-run with
// --prune-passing.
std::string FormatBaselineUpdateReport(const BaselineUpdateReport &report,
                                       bool prune_passing);

// Rewrites every relative extends_file in *manifest so it still resolves when
// the manifest is written to to_manifest_path instead of from_manifest_path
// (no-op when both are in the same directory). Fails with a clear error if a
// path cannot be expressed relatively (e.g. different drive or UNC share).
bool RebaseExtendsFilePaths(BaselineManifest *manifest,
                            const std::string &from_manifest_path,
                            const std::string &to_manifest_path,
                            std::string *error_out);

// Serializes a baseline manifest to deterministic ordered_json / UTF-8 text.
nlohmann::ordered_json
SerializeBaselineManifestToJson(const BaselineManifest &manifest);
std::string SerializeBaselineManifest(const BaselineManifest &manifest);

} // namespace fstest::cli
