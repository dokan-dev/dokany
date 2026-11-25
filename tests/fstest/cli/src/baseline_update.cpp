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
    if (error_out != nullptr) {
      *error_out = "Internal error: null manifest";
    }
    return false;
  }

  const auto target_it = manifest->targets.find(resolved.target_name);
  if (target_it == manifest->targets.end()) {
    if (error_out != nullptr) {
      *error_out = "Target \"" + resolved.target_name +
                   "\" not found in manifest for update";
    }
    return false;
  }

  TargetDefinition &target = target_it->second;
  const std::map<std::string, XfailEntry> &parent_entries =
      resolved.parent_entries;
  const std::map<std::string, XfailEntry> old_xfails = target.xfails;
  const std::set<std::string> old_removes(target.remove_xfails.begin(),
                                          target.remove_xfails.end());

  std::unordered_map<std::string, ObservedTestResult> obs_by_id;
  for (const ObservedTestResult &r : results) {
    obs_by_id[r.test_id] = r;
  }

  // Collect all test IDs we need to evaluate.
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

  std::map<std::string, XfailEntry> new_xfails;
  std::set<std::string> new_removes;
  BaselineUpdateReport rep;

  for (const std::string &id : all_ids) {
    const auto parent_it = parent_entries.find(id);
    const bool in_parent = (parent_it != parent_entries.end());
    const bool parent_matches =
        in_parent && parent_it->second.MatchesHost(os_build, target_arch);
    const bool parent_suppressible =
        parent_matches && !parent_it->second.allow_xpass;

    const auto child_it = old_xfails.find(id);
    const bool in_child = (child_it != old_xfails.end());
    const bool in_old_removes = old_removes.contains(id);

    const auto obs_it = obs_by_id.find(id);
    // Case A: Not executed, skipped, skip_crash, fatal setup/teardown, or
    // exception -> preserve untouched.
    if (obs_it == obs_by_id.end() || PreservesExistingState(obs_it->second)) {
      if (in_child) {
        new_xfails.emplace(id, child_it->second);
      } else if (in_old_removes && in_parent) {
        new_removes.insert(id);
      }
      continue;
    }

    const ObservedTestResult &obs = obs_it->second;

    // Case B: Failed on this run.
    if (obs.failed) {
      if (in_child) {
        XfailEntry entry = child_it->second;
        if (!entry.MatchesHost(os_build, target_arch)) {
          WidenEntryForHost(&entry, os_build, target_arch);
          rep.widened.push_back(id);
        }
        new_xfails.emplace(id, std::move(entry));
      } else if (parent_matches) {
        // Parent actively covers this failure: omit from child xfails and
        // ensure it is NOT in remove_xfails (re-enabling a suppressed entry).
        if (in_old_removes) {
          rep.added.push_back(id);
        }
      } else {
        XfailEntry entry;
        entry.reason = std::string(default_reason);
        new_xfails.emplace(id, std::move(entry));
        rep.added.push_back(id);
      }
      continue;
    }

    // Case C: Passed on this run. Only drop coverage for this host; entries
    // that may still be needed on other hosts/builds are kept unless
    // prune_passing is set.
    if (in_child) {
      const XfailEntry &child = child_it->second;
      if (!child.MatchesHost(os_build, target_arch) || child.allow_xpass) {
        // Preserve entries for other OS/arch predicates or allow_xpass.
        new_xfails.emplace(id, child);
      } else if (IsMultiArchIncluding(child, target_arch)) {
        XfailEntry entry = child;
        entry.arch.erase(
            std::remove(entry.arch.begin(), entry.arch.end(), target_arch),
            entry.arch.end());
        new_xfails.emplace(id, std::move(entry));
        rep.narrowed.push_back(id);
      } else if (options.prune_passing ||
                 IsHostSpecific(child, os_build, target_arch)) {
        // Remove from child. If the parent also matches this host (and is not
        // allow_xpass), suppress it via remove_xfails so it does not
        // re-activate; the child override already hid it on every host.
        if (parent_suppressible) {
          new_removes.insert(id);
        }
        rep.removed.push_back(id);
      } else {
        new_xfails.emplace(id, child);
        rep.passed_kept.push_back(id);
      }
    } else if (in_parent) {
      if (in_old_removes) {
        new_removes.insert(id);
      } else if (parent_suppressible) {
        // remove_xfails suppresses the parent entry on every host, so only do
        // it when the entry cannot cover another host or pruning is requested.
        if (options.prune_passing ||
            IsHostSpecific(parent_it->second, os_build, target_arch)) {
          new_removes.insert(id);
          rep.removed.push_back(id);
        } else {
          rep.passed_kept.push_back(id);
        }
      }
    }
  }

  target.xfails = std::move(new_xfails);
  target.remove_xfails.assign(new_removes.begin(), new_removes.end());
  if (report != nullptr) {
    *report = std::move(rep);
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
  auto emit_target = [&](const TargetDefinition &target) {
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
        e["arch"] = sorted_arch;
      }
      if (entry.allow_xpass) {
        e["allow_xpass"] = true;
      }
      if (entry.skip_crash) {
        e["skip_crash"] = true;
      }
      xfails_json[id] = std::move(e);
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

    targets_json[target.name] = std::move(t);
  };

  for (const std::string &name : manifest.target_order) {
    const auto it = manifest.targets.find(name);
    if (it != manifest.targets.end() && emitted.insert(name).second) {
      emit_target(it->second);
    }
  }
  for (const auto &[name, def] : manifest.targets) {
    if (emitted.insert(name).second) {
      emit_target(def);
    }
  }

  root["targets"] = std::move(targets_json);
  return root;
}

std::string SerializeBaselineManifest(const BaselineManifest &manifest) {
  try {
    return SerializeJsonDeterministic(
        SerializeBaselineManifestToJson(manifest));
  } catch (const std::exception &) {
    try {
      return SerializeJsonDeterministic(
          SerializeBaselineManifestToJson(manifest),
          nlohmann::ordered_json::error_handler_t::replace);
    } catch (const std::exception &) {
      return {};
    }
  }
}

} // namespace fstest::cli
