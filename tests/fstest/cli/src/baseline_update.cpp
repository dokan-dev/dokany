// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "baseline.h"

#include "json_util.h"

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fstest::cli {
namespace {

void WidenEntryForHost(XfailEntry *entry, uint32_t os_build,
                       std::string_view target_arch) {
  if (entry->os_min_build.has_value() && os_build < *entry->os_min_build) {
    entry->os_min_build = os_build;
  }
  if (entry->os_max_build.has_value() && os_build > *entry->os_max_build) {
    entry->os_max_build = os_build;
  }
  if (!entry->arch.empty()) {
    const auto it =
        std::find(entry->arch.begin(), entry->arch.end(), target_arch);
    if (it == entry->arch.end()) {
      entry->arch.emplace_back(target_arch);
      std::sort(entry->arch.begin(), entry->arch.end());
    }
  }
}

// True when the entry covers exactly this arch and this OS build, i.e. it
// cannot cover any other host.
bool IsHostSpecific(const XfailEntry &entry, uint32_t os_build,
                    std::string_view target_arch) {
  return entry.arch.size() == 1 && entry.arch[0] == target_arch &&
         entry.os_min_build.has_value() && entry.os_max_build.has_value() &&
         *entry.os_min_build == os_build && *entry.os_max_build == os_build;
}

bool IsMultiArchIncluding(const XfailEntry &entry,
                          std::string_view target_arch) {
  return entry.arch.size() > 1 &&
         std::find(entry.arch.begin(), entry.arch.end(), target_arch) !=
             entry.arch.end();
}

// Outcomes that carry no reliable pass/fail signal for the baseline: the
// existing entry (if any) is preserved untouched and nothing is added.
bool PreservesExistingState(const ObservedTestResult &obs) {
  return obs.skipped || obs.fatal_setup || obs.fatal_teardown ||
         obs.exception || obs.skip_crash || (!obs.passed && !obs.failed);
}

struct BaselineHostContext {
  uint32_t os_build = 0;
  std::string_view target_arch;
  std::string_view default_reason;
  BaselineUpdateOptions options{};
};

struct BaselineIdState {
  const XfailEntry *parent_entry = nullptr;
  const XfailEntry *child_entry = nullptr;
  bool in_old_removes = false;
  bool parent_matches = false;
  bool parent_suppressible = false;
};

struct BaselineUpdateAccumulator {
  std::map<std::string, XfailEntry> new_xfails;
  std::set<std::string> new_removes;
  BaselineUpdateReport report;
};

bool FailUpdate(std::string *error_out, std::string message) {
  if (error_out != nullptr) {
    *error_out = std::move(message);
  }
  return false;
}

bool ShouldPrunePassingEntry(const XfailEntry &entry,
                             const BaselineHostContext &host) {
  return host.options.prune_passing ||
         IsHostSpecific(entry, host.os_build, host.target_arch);
}

std::set<std::string> CollectAllTestIds(
    const std::map<std::string, XfailEntry> &parent_entries,
    const std::map<std::string, XfailEntry> &old_xfails,
    const std::set<std::string> &old_removes,
    const std::unordered_map<std::string, ObservedTestResult> &obs_by_id) {
  std::set<std::string> all_ids;
  for (const auto &[id, _] : parent_entries) {
    all_ids.insert(id);
  }
  for (const auto &[id, _] : old_xfails) {
    all_ids.insert(id);
  }
  for (const std::string &id : old_removes) {
    all_ids.insert(id);
  }
  for (const auto &[id, _] : obs_by_id) {
    all_ids.insert(id);
  }
  return all_ids;
}

BaselineIdState
BuildIdState(const std::string &id,
             const std::map<std::string, XfailEntry> &parent_entries,
             const std::map<std::string, XfailEntry> &old_xfails,
             const std::set<std::string> &old_removes,
             const BaselineHostContext &host) {
  BaselineIdState state{};
  const auto parent_it = parent_entries.find(id);
  if (parent_it != parent_entries.end()) {
    state.parent_entry = &parent_it->second;
    state.parent_matches =
        state.parent_entry->MatchesHost(host.os_build, host.target_arch);
    state.parent_suppressible =
        state.parent_matches && !state.parent_entry->allow_xpass;
  }
  const auto child_it = old_xfails.find(id);
  if (child_it != old_xfails.end()) {
    state.child_entry = &child_it->second;
  }
  state.in_old_removes = old_removes.contains(id);
  return state;
}

void PreserveExistingEntry(const std::string &id, const BaselineIdState &state,
                           BaselineUpdateAccumulator *acc) {
  if (state.child_entry != nullptr) {
    acc->new_xfails.emplace(id, *state.child_entry);
  } else if (state.in_old_removes && state.parent_entry != nullptr) {
    acc->new_removes.insert(id);
  }
}

void ApplyFailedResult(const std::string &id, const BaselineIdState &state,
                       const BaselineHostContext &host,
                       BaselineUpdateAccumulator *acc) {
  if (state.child_entry != nullptr) {
    XfailEntry entry = *state.child_entry;
    if (!entry.MatchesHost(host.os_build, host.target_arch)) {
      WidenEntryForHost(&entry, host.os_build, host.target_arch);
      acc->report.widened.push_back(id);
    }
    acc->new_xfails.emplace(id, std::move(entry));
    return;
  }
  if (state.parent_matches) {
    // Parent actively covers this failure: omit from child xfails and ensure
    // it is NOT in remove_xfails (re-enabling a suppressed entry).
    if (state.in_old_removes) {
      acc->report.added.push_back(id);
    }
    return;
  }
  XfailEntry entry;
  entry.reason = std::string(host.default_reason);
  acc->new_xfails.emplace(id, std::move(entry));
  acc->report.added.push_back(id);
}

void ApplyPassedChildEntry(const std::string &id, const XfailEntry &child,
                           const BaselineIdState &state,
                           const BaselineHostContext &host,
                           BaselineUpdateAccumulator *acc) {
  if (!child.MatchesHost(host.os_build, host.target_arch) ||
      child.allow_xpass) {
    // Preserve entries for other OS/arch predicates or allow_xpass.
    acc->new_xfails.emplace(id, child);
    return;
  }
  if (IsMultiArchIncluding(child, host.target_arch)) {
    XfailEntry entry = child;
    entry.arch.erase(
        std::remove(entry.arch.begin(), entry.arch.end(), host.target_arch),
        entry.arch.end());
    acc->new_xfails.emplace(id, std::move(entry));
    acc->report.narrowed.push_back(id);
    return;
  }
  if (ShouldPrunePassingEntry(child, host)) {
    // Remove from child. If the parent also matches this host (and is not
    // allow_xpass), suppress it via remove_xfails so it does not re-activate;
    // the child override already hid it on every host.
    if (state.parent_suppressible) {
      acc->new_removes.insert(id);
    }
    acc->report.removed.push_back(id);
    return;
  }
  acc->new_xfails.emplace(id, child);
  acc->report.passed_kept.push_back(id);
}

void ApplyPassedParentEntry(const std::string &id, const XfailEntry &parent,
                            const BaselineIdState &state,
                            const BaselineHostContext &host,
                            BaselineUpdateAccumulator *acc) {
  if (state.in_old_removes) {
    acc->new_removes.insert(id);
    return;
  }
  if (!state.parent_suppressible) {
    return;
  }
  // remove_xfails suppresses the parent entry on every host, so only do it
  // when the entry cannot cover another host or pruning is requested.
  if (ShouldPrunePassingEntry(parent, host)) {
    acc->new_removes.insert(id);
    acc->report.removed.push_back(id);
  } else {
    acc->report.passed_kept.push_back(id);
  }
}

void ApplyPassedResult(const std::string &id, const BaselineIdState &state,
                       const BaselineHostContext &host,
                       BaselineUpdateAccumulator *acc) {
  if (state.child_entry != nullptr) {
    ApplyPassedChildEntry(id, *state.child_entry, state, host, acc);
  } else if (state.parent_entry != nullptr) {
    ApplyPassedParentEntry(id, *state.parent_entry, state, host, acc);
  }
}

void UpdateBaselineEntry(const std::string &id, const BaselineIdState &state,
                         const ObservedTestResult *obs,
                         const BaselineHostContext &host,
                         BaselineUpdateAccumulator *acc) {
  // Case A: Not executed, skipped, skip_crash, fatal setup/teardown, or
  // exception -> preserve untouched.
  if (obs == nullptr || PreservesExistingState(*obs)) {
    PreserveExistingEntry(id, state, acc);
    return;
  }
  // Case B: Failed on this run.
  if (obs->failed) {
    ApplyFailedResult(id, state, host, acc);
    return;
  }
  // Case C: Passed on this run. Only drop coverage for this host; entries
  // that may still be needed on other hosts/builds are kept unless
  // prune_passing is set.
  ApplyPassedResult(id, state, host, acc);
}

nlohmann::ordered_json SerializeXfailEntryToJson(const XfailEntry &entry) {
  nlohmann::ordered_json e = nlohmann::ordered_json::object();
  e["reason"] = entry.reason;
  if (!entry.gap.empty()) {
    e["gap"] = entry.gap;
  }
  if (entry.os_min_build.has_value()) {
    e["os_min_build"] = *entry.os_min_build;
  }
  if (entry.os_max_build.has_value()) {
    e["os_max_build"] = *entry.os_max_build;
  }
  if (!entry.arch.empty()) {
    std::vector<std::string> sorted_arch = entry.arch;
    std::sort(sorted_arch.begin(), sorted_arch.end());
    e["arch"] = std::move(sorted_arch);
  }
  if (entry.allow_xpass) {
    e["allow_xpass"] = true;
  }
  if (entry.skip_crash) {
    e["skip_crash"] = true;
  }
  return e;
}

nlohmann::ordered_json
SerializeTargetDefinitionToJson(const TargetDefinition &target) {
  nlohmann::ordered_json t = nlohmann::ordered_json::object();
  if (target.is_abstract) {
    t["abstract"] = true;
  }
  if (!target.extends.empty()) {
    t["extends"] = target.extends;
  }
  if (!target.extends_file.empty()) {
    t["extends_file"] = target.extends_file;
  }
  if (!target.description.empty()) {
    t["description"] = target.description;
  }

  nlohmann::ordered_json xfails_json = nlohmann::ordered_json::object();
  for (const auto &[id, entry] : target.xfails) {
    xfails_json[id] = SerializeXfailEntryToJson(entry);
  }
  t["xfails"] = std::move(xfails_json);

  if (!target.remove_xfails.empty()) {
    std::vector<std::string> sorted_removes = target.remove_xfails;
    std::sort(sorted_removes.begin(), sorted_removes.end());
    sorted_removes.erase(
        std::unique(sorted_removes.begin(), sorted_removes.end()),
        sorted_removes.end());
    t["remove_xfails"] = std::move(sorted_removes);
  }
  return t;
}

} // namespace

bool UpdateBaselineManifest(BaselineManifest *manifest,
                            const ResolvedBaseline &resolved, uint32_t os_build,
                            std::string_view target_arch,
                            std::string_view default_reason,
                            const std::vector<ObservedTestResult> &results,
                            const BaselineUpdateOptions &options,
                            BaselineUpdateReport *report,
                            std::string *error_out) {
  if (manifest == nullptr) {
    return FailUpdate(error_out, "Internal error: null manifest");
  }

  const auto target_it = manifest->targets.find(resolved.target_name);
  if (target_it == manifest->targets.end()) {
    return FailUpdate(error_out, "Target \"" + resolved.target_name +
                                     "\" not found in manifest for update");
  }

  TargetDefinition &target = target_it->second;
  const std::map<std::string, XfailEntry> &parent_entries =
      resolved.parent_entries;
  const std::map<std::string, XfailEntry> &old_xfails = target.xfails;
  const std::set<std::string> old_removes(target.remove_xfails.begin(),
                                          target.remove_xfails.end());

  std::unordered_map<std::string, ObservedTestResult> obs_by_id;
  for (const ObservedTestResult &r : results) {
    obs_by_id[r.test_id] = r;
  }

  const BaselineHostContext host{os_build, target_arch, default_reason,
                                 options};
  const std::set<std::string> all_ids =
      CollectAllTestIds(parent_entries, old_xfails, old_removes, obs_by_id);

  BaselineUpdateAccumulator acc;
  for (const std::string &id : all_ids) {
    const BaselineIdState state =
        BuildIdState(id, parent_entries, old_xfails, old_removes, host);
    const auto obs_it = obs_by_id.find(id);
    const ObservedTestResult *obs =
        obs_it == obs_by_id.end() ? nullptr : &obs_it->second;
    UpdateBaselineEntry(id, state, obs, host, &acc);
  }

  target.xfails = std::move(acc.new_xfails);
  target.remove_xfails.assign(acc.new_removes.begin(), acc.new_removes.end());
  if (report != nullptr) {
    *report = std::move(acc.report);
  }
  return true;
}

std::string FormatBaselineUpdateReport(const BaselineUpdateReport &report,
                                       bool prune_passing) {
  std::ostringstream os;
  os << "Baseline update: " << report.added.size() << " added, "
     << report.widened.size() << " widened, " << report.narrowed.size()
     << " narrowed, " << report.removed.size() << " removed\n";
  const auto emit = [&os](std::string_view label,
                          const std::vector<std::string> &ids) {
    for (const std::string &id : ids) {
      os << "  " << label << " " << id << "\n";
    }
  };
  emit("removed: ", report.removed);
  emit("narrowed:", report.narrowed);
  emit("added:   ", report.added);
  emit("widened: ", report.widened);
  if (!report.passed_kept.empty()) {
    os << report.passed_kept.size()
       << " xfail(s) passed on this host but were kept because they may still "
          "be needed on other hosts or OS builds";
    if (!prune_passing) {
      os << " (re-run with --update-baseline --prune-passing to remove them)";
    }
    os << ":\n";
    emit("kept:    ", report.passed_kept);
  }
  return os.str();
}

nlohmann::ordered_json
SerializeBaselineManifestToJson(const BaselineManifest &manifest) {
  nlohmann::ordered_json root = nlohmann::ordered_json::object();
  root["schema_version"] = manifest.schema_version;
  if (!manifest.description.empty()) {
    root["description"] = manifest.description;
  }

  nlohmann::ordered_json targets_json = nlohmann::ordered_json::object();

  // Emit targets in preserved declaration order first, then any added targets
  // in lexicographical order.
  std::set<std::string> emitted;
  for (const std::string &name : manifest.target_order) {
    const auto it = manifest.targets.find(name);
    if (it != manifest.targets.end() && emitted.insert(name).second) {
      targets_json[name] = SerializeTargetDefinitionToJson(it->second);
    }
  }
  for (const auto &[name, def] : manifest.targets) {
    if (emitted.insert(name).second) {
      targets_json[name] = SerializeTargetDefinitionToJson(def);
    }
  }

  root["targets"] = std::move(targets_json);
  return root;
}

std::string SerializeBaselineManifest(const BaselineManifest &manifest) {
  try {
    const nlohmann::ordered_json root =
        SerializeBaselineManifestToJson(manifest);
    try {
      return SerializeJsonDeterministic(root);
    } catch (const std::exception &) {
      return SerializeJsonDeterministic(
          root, nlohmann::ordered_json::error_handler_t::replace);
    }
  } catch (const std::exception &) {
    return {};
  }
}

} // namespace fstest::cli
