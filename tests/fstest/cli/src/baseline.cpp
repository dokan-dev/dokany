// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "baseline.h"

#include "cli_args.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/status.h"
#include "fstest/core/win_headers.h"
#include "json_util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace fstest::cli {
namespace {

void SetError(std::string *error_out, std::string message) {
  if (error_out != nullptr) {
    *error_out = std::move(message);
  }
}

std::filesystem::path PathFromUtf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const std::filesystem::path &path) {
  const std::u8string u8 = path.u8string();
  return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

bool IsBlank(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  });
}

bool IsValidTestId(std::string_view id) {
  // ':' separates --gtest_filter patterns and a leading '-' starts the
  // negative section, so either would corrupt generated filters.
  if (id.empty() || id.front() == '-') {
    return false;
  }
  bool has_dot = false;
  for (char c : id) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc <= 0x20 || uc == 0x7f || c == '*' || c == '?' || c == ':') {
      return false;
    }
    has_dot = has_dot || (c == '.');
  }
  return has_dot;
}

// Matches ^GAP-[0-9]{2,}$ (ASCII digits only).
bool IsValidGapId(std::string_view gap) {
  constexpr std::string_view kPrefix = "GAP-";
  if (gap.size() < kPrefix.size() + 2 ||
      gap.substr(0, kPrefix.size()) != kPrefix) {
    return false;
  }
  return std::all_of(gap.begin() + kPrefix.size(), gap.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

bool ExtractUint32(const nlohmann::ordered_json &val, uint32_t *out) {
  constexpr uint64_t kMaxU32 = std::numeric_limits<uint32_t>::max();
  if (val.is_number_unsigned()) {
    const uint64_t u = val.get<uint64_t>();
    if (u <= kMaxU32) {
      *out = static_cast<uint32_t>(u);
      return true;
    }
  } else if (val.is_number_integer()) {
    const int64_t i = val.get<int64_t>();
    if (i >= 0 && static_cast<uint64_t>(i) <= kMaxU32) {
      *out = static_cast<uint32_t>(i);
      return true;
    }
  }
  return false;
}

std::string FormatFieldError(std::string_view field_name,
                             std::string_view context,
                             std::string_view expectation) {
  return "Field \"" + std::string(field_name) + "\" in " +
         std::string(context) + " " + std::string(expectation);
}

bool ValidateAllowedKeys(const nlohmann::ordered_json &obj,
                         std::initializer_list<std::string_view> allowed,
                         std::string_view context, std::string *error_out) {
  for (const auto &item : obj.items()) {
    const std::string_view key(item.key());
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      *error_out =
          "Unknown field \"" + item.key() + "\" in " + std::string(context);
      return false;
    }
  }
  return true;
}

bool ContainsNul(std::string_view s) {
  return s.find('\0') != std::string_view::npos;
}

bool ParseOptionalStringField(const nlohmann::ordered_json &obj,
                              const std::string &key, std::string_view context,
                              std::string *out, std::string *error_out) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return true;
  }
  if (!it->is_string()) {
    *error_out = FormatFieldError(key, context, "must be a string");
    return false;
  }
  if (ContainsNul(it->get_ref<const std::string &>())) {
    *error_out = FormatFieldError(key, context,
                                  "must not contain NUL (\\u0000) characters");
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

bool ParseNonEmptyStringField(const nlohmann::ordered_json &obj,
                              const std::string &key, std::string_view context,
                              bool required, std::string *out,
                              std::string *error_out) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    if (required) {
      *error_out =
          "Missing required field \"" + key + "\" in " + std::string(context);
      return false;
    }
    return true;
  }
  if (!it->is_string() || IsBlank(it->get<std::string>())) {
    *error_out = FormatFieldError(key, context, "must be a non-empty string");
    return false;
  }
  if (ContainsNul(it->get_ref<const std::string &>())) {
    *error_out = FormatFieldError(key, context,
                                  "must not contain NUL (\\u0000) characters");
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

bool ParseGapField(const nlohmann::ordered_json &obj, std::string_view context,
                   std::string *out, std::string *error_out) {
  const auto it = obj.find("gap");
  if (it == obj.end()) {
    return true;
  }
  if (!it->is_string() || !IsValidGapId(it->get_ref<const std::string &>())) {
    *error_out = FormatFieldError(
        "gap", context,
        "must be a CONFORMANCE_GAPS.md ID matching GAP-<2+ digits> (e.g. "
        "\"GAP-01\")");
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

bool ParseOptionalBoolField(const nlohmann::ordered_json &obj,
                            const std::string &key, std::string_view context,
                            bool *out, std::string *error_out) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return true;
  }
  if (!it->is_boolean()) {
    *error_out = FormatFieldError(key, context, "must be a boolean");
    return false;
  }
  *out = it->get<bool>();
  return true;
}

bool ParseOptionalUint32Field(const nlohmann::ordered_json &obj,
                              const std::string &key, std::string_view context,
                              std::optional<uint32_t> *out,
                              std::string *error_out) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return true;
  }
  uint32_t parsed = 0;
  if (!ExtractUint32(*it, &parsed)) {
    *error_out =
        FormatFieldError(key, context, "must be an unsigned 32-bit integer");
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseArchArrayField(const nlohmann::ordered_json &obj,
                         const std::string &test_id,
                         std::vector<std::string> *out,
                         std::string *error_out) {
  const auto it = obj.find("arch");
  if (it == obj.end()) {
    return true;
  }
  if (!it->is_array() || it->empty()) {
    *error_out = "Field \"arch\" in xfail entry \"" + test_id +
                 "\" must be a non-empty array of arch strings";
    return false;
  }
  std::unordered_set<std::string> seen_arch;
  for (const auto &elem : *it) {
    if (!elem.is_string()) {
      *error_out = "Field \"arch\" in xfail entry \"" + test_id +
                   "\" must contain only strings";
      return false;
    }
    const std::string arch_str = elem.get<std::string>();
    if (!IsValidTargetArch(arch_str)) {
      *error_out = "Invalid arch \"" + arch_str + "\" in xfail entry \"" +
                   test_id + "\" (expected one of: x64, x86, arm64)";
      return false;
    }
    if (!seen_arch.insert(arch_str).second) {
      *error_out = "Duplicate arch \"" + arch_str + "\" in xfail entry \"" +
                   test_id + "\"";
      return false;
    }
    out->push_back(arch_str);
  }
  return true;
}

bool ValidateXfailEntryConstraints(const std::string &target_name,
                                   const std::string &test_id,
                                   const XfailEntry &entry,
                                   std::string *error_out) {
  if (entry.skip_crash && entry.allow_xpass) {
    *error_out = "Xfail entry \"" + test_id + "\" of target \"" + target_name +
                 "\" cannot set both \"skip_crash\" and \"allow_xpass\"";
    return false;
  }
  if ((entry.skip_crash || entry.allow_xpass) && entry.gap.empty()) {
    *error_out = "Xfail entry \"" + test_id + "\" of target \"" + target_name +
                 "\" sets \"" +
                 (entry.skip_crash ? "skip_crash" : "allow_xpass") +
                 "\" and must cite a CONFORMANCE_GAPS.md \"gap\" "
                 "(e.g. \"gap\": \"GAP-01\")";
    return false;
  }
  if (entry.os_min_build && entry.os_max_build &&
      *entry.os_min_build > *entry.os_max_build) {
    *error_out = "Invalid build range in xfail entry \"" + test_id +
                 "\": os_min_build (" + std::to_string(*entry.os_min_build) +
                 ") > os_max_build (" + std::to_string(*entry.os_max_build) +
                 ")";
    return false;
  }
  return true;
}

bool ParseXfailEntry(const std::string &target_name, const std::string &test_id,
                     const nlohmann::ordered_json &obj, XfailEntry *out,
                     std::string *error_out) {
  const std::string ctx =
      "xfail entry \"" + test_id + "\" of target \"" + target_name + "\"";
  if (!IsValidTestId(test_id)) {
    *error_out = "Invalid test ID \"" + test_id + "\" in target \"" +
                 target_name +
                 "\" (must be exact canonical 'Suite.Test' with no wildcards)";
    return false;
  }
  if (!obj.is_object()) {
    *error_out = "Xfail entry \"" + test_id + "\" in target \"" + target_name +
                 "\" must be a JSON object";
    return false;
  }
  if (!ValidateAllowedKeys(obj,
                           {"reason", "gap", "os_min_build", "os_max_build",
                            "arch", "allow_xpass", "skip_crash"},
                           ctx, error_out)) {
    return false;
  }

  XfailEntry entry;
  if (!ParseNonEmptyStringField(obj, "reason", ctx, /*required=*/true,
                                &entry.reason, error_out) ||
      !ParseGapField(obj, ctx, &entry.gap, error_out) ||
      !ParseOptionalUint32Field(obj, "os_min_build", ctx, &entry.os_min_build,
                                error_out) ||
      !ParseOptionalUint32Field(obj, "os_max_build", ctx, &entry.os_max_build,
                                error_out) ||
      !ParseArchArrayField(obj, test_id, &entry.arch, error_out) ||
      !ParseOptionalBoolField(obj, "allow_xpass", ctx, &entry.allow_xpass,
                              error_out) ||
      !ParseOptionalBoolField(obj, "skip_crash", ctx, &entry.skip_crash,
                              error_out) ||
      !ValidateXfailEntryConstraints(target_name, test_id, entry, error_out)) {
    return false;
  }

  *out = std::move(entry);
  return true;
}

bool ParseXfailMapField(const std::string &target_name,
                        const nlohmann::ordered_json &target_obj,
                        std::map<std::string, XfailEntry> *out,
                        std::string *error_out) {
  const auto it = target_obj.find("xfails");
  if (it == target_obj.end()) {
    return true;
  }
  if (!it->is_object()) {
    *error_out = "Field \"xfails\" in target \"" + target_name +
                 "\" must be a JSON object";
    return false;
  }
  for (const auto &[test_id, entry_json] : it->items()) {
    XfailEntry entry;
    if (!ParseXfailEntry(target_name, test_id, entry_json, &entry, error_out)) {
      return false;
    }
    out->emplace(test_id, std::move(entry));
  }
  return true;
}

bool ParseRemoveXfailsField(const std::string &target_name,
                            const nlohmann::ordered_json &target_obj,
                            std::vector<std::string> *out,
                            std::string *error_out) {
  const auto it = target_obj.find("remove_xfails");
  if (it == target_obj.end()) {
    return true;
  }
  if (!it->is_array()) {
    *error_out = "Field \"remove_xfails\" in target \"" + target_name +
                 "\" must be a JSON array";
    return false;
  }
  std::unordered_set<std::string> seen;
  for (const auto &elem : *it) {
    if (!elem.is_string()) {
      *error_out = "Entries in \"remove_xfails\" of target \"" + target_name +
                   "\" must be strings";
      return false;
    }
    const std::string id = elem.get<std::string>();
    if (!IsValidTestId(id)) {
      *error_out = "Invalid test ID \"" + id +
                   "\" in \"remove_xfails\" of target \"" + target_name + "\"";
      return false;
    }
    if (!seen.insert(id).second) {
      *error_out = "Duplicate test ID \"" + id +
                   "\" in \"remove_xfails\" of target \"" + target_name + "\"";
      return false;
    }
    out->push_back(id);
  }
  return true;
}

bool ValidateTargetDefinition(const std::string &target_name,
                              const TargetDefinition &def,
                              std::string *error_out) {
  if (!def.extends_file.empty() && def.extends.empty()) {
    *error_out = "Target \"" + target_name +
                 "\" specifies \"extends_file\" without \"extends\"";
    return false;
  }
  if (!def.remove_xfails.empty() && def.extends.empty()) {
    *error_out = "Target \"" + target_name +
                 "\" specifies \"remove_xfails\" without \"extends\"";
    return false;
  }
  for (const std::string &id : def.remove_xfails) {
    if (def.xfails.contains(id)) {
      *error_out = "Test ID \"" + id + "\" cannot appear in both " +
                   "\"remove_xfails\" and \"xfails\" of target \"" +
                   target_name + "\"";
      return false;
    }
  }
  return true;
}

bool ParseTargetObject(const std::string &target_name,
                       const nlohmann::ordered_json &obj, TargetDefinition *out,
                       std::string *error_out) {
  if (IsBlank(target_name)) {
    *error_out = "Target name must be non-empty";
    return false;
  }
  if (ContainsNul(target_name)) {
    *error_out = "Target name must not contain NUL (\\u0000) characters";
    return false;
  }
  if (!obj.is_object()) {
    *error_out = "Target \"" + target_name + "\" must be a JSON object";
    return false;
  }
  const std::string ctx = "target \"" + target_name + "\"";
  if (!ValidateAllowedKeys(obj,
                           {"abstract", "description", "extends",
                            "extends_file", "xfails", "remove_xfails"},
                           ctx, error_out)) {
    return false;
  }

  TargetDefinition def;
  def.name = target_name;
  if (!ParseOptionalBoolField(obj, "abstract", ctx, &def.is_abstract,
                              error_out) ||
      !ParseOptionalStringField(obj, "description", ctx, &def.description,
                                error_out) ||
      !ParseNonEmptyStringField(obj, "extends", ctx, /*required=*/false,
                                &def.extends, error_out) ||
      !ParseNonEmptyStringField(obj, "extends_file", ctx, /*required=*/false,
                                &def.extends_file, error_out) ||
      !ParseXfailMapField(target_name, obj, &def.xfails, error_out) ||
      !ParseRemoveXfailsField(target_name, obj, &def.remove_xfails,
                              error_out) ||
      !ValidateTargetDefinition(target_name, def, error_out)) {
    return false;
  }

  *out = std::move(def);
  return true;
}

std::string JoinWithComma(const std::vector<std::string> &items,
                          std::size_t max_items) {
  std::ostringstream os;
  const std::size_t count = std::min(items.size(), max_items);
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) {
      os << ", ";
    }
    os << items[i];
  }
  return os.str();
}

bool SelectTargetName(const BaselineManifest &manifest,
                      std::string_view requested_target,
                      std::string *selected_out, std::string *error_out) {
  if (!requested_target.empty()) {
    const std::string name(requested_target);
    const auto it = manifest.targets.find(name);
    if (it == manifest.targets.end()) {
      *error_out = "Baseline target \"" + name + "\" not found in manifest";
      return false;
    }
    if (it->second.is_abstract) {
      *error_out = "Baseline target \"" + name +
                   "\" is abstract and cannot be selected directly";
      return false;
    }
    *selected_out = name;
    return true;
  }

  std::vector<std::string> concrete;
  for (const std::string &name : manifest.target_order) {
    const auto it = manifest.targets.find(name);
    if (it != manifest.targets.end() && !it->second.is_abstract) {
      concrete.push_back(name);
    }
  }
  if (concrete.empty()) {
    *error_out = "Baseline manifest contains no non-abstract targets";
    return false;
  }
  if (concrete.size() > 1) {
    *error_out =
        "Multiple targets in baseline manifest; specify --target=<name> "
        "(available: " +
        JoinWithComma(concrete, concrete.size()) + ")";
    return false;
  }
  *selected_out = concrete[0];
  return true;
}

std::string NormalizeFileKey(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  return PathToUtf8(ec ? path.lexically_normal() : canonical);
}

using StackEntry = std::pair<std::string, std::string>;

class ScopedStackPush {
public:
  ScopedStackPush(std::vector<StackEntry> *stack, StackEntry entry)
      : stack_(stack) {
    stack_->push_back(std::move(entry));
  }
  ~ScopedStackPush() { stack_->pop_back(); }

  ScopedStackPush(const ScopedStackPush &) = delete;
  ScopedStackPush &operator=(const ScopedStackPush &) = delete;

private:
  std::vector<StackEntry> *stack_;
};

std::string FormatCycleError(const std::vector<StackEntry> &active_stack,
                             const std::string &target_name) {
  std::ostringstream os;
  os << "Baseline inheritance cycle detected: ";
  for (const auto &item : active_stack) {
    os << item.second << " -> ";
  }
  os << target_name;
  return os.str();
}

bool ResolveParentFilePath(const std::filesystem::path &file_path,
                           const std::string &target_name,
                           const TargetDefinition &target,
                           bool allow_file_loading,
                           std::filesystem::path *parent_file_out,
                           std::string *error_out) {
  if (target.extends_file.empty()) {
    *parent_file_out = file_path;
    return true;
  }
  if (!allow_file_loading) {
    *error_out = "Target \"" + target_name + "\" uses \"extends_file\" (\"" +
                 target.extends_file +
                 "\"), which requires file-based resolution";
    return false;
  }
  const std::filesystem::path rel = PathFromUtf8(target.extends_file);
  // Normalize so ".." segments (e.g. from RebaseExtendsFilePaths) also work
  // with extended-length (\\?\) manifest paths, where Win32 does not.
  *parent_file_out = rel.is_absolute()
                         ? rel
                         : (file_path.parent_path() / rel).lexically_normal();
  return true;
}

bool ValidateRemoveXfailsInParent(
    const std::string &target_name, const TargetDefinition &target,
    const std::map<std::string, XfailEntry> &parent_entries,
    std::string *error_out) {
  for (const std::string &rem_id : target.remove_xfails) {
    if (!parent_entries.contains(rem_id)) {
      *error_out = "Target \"" + target_name + "\" lists \"" + rem_id +
                   "\" in \"remove_xfails\", but parent \"" + target.extends +
                   "\" does not define or inherit it";
      return false;
    }
  }
  return true;
}

std::map<std::string, XfailEntry>
ApplyTargetDelta(const std::map<std::string, XfailEntry> &parent_entries,
                 const TargetDefinition &target) {
  std::map<std::string, XfailEntry> effective = parent_entries;
  for (const std::string &rem_id : target.remove_xfails) {
    effective.erase(rem_id);
  }
  for (const auto &[id, entry] : target.xfails) {
    effective[id] = entry;
  }
  return effective;
}

struct ResolutionContext {
  bool allow_file_loading = true;
  std::unordered_map<std::string, BaselineManifest> cache;
  std::vector<StackEntry> active_stack;

  const BaselineManifest *GetOrLoad(const std::filesystem::path &file_path,
                                    std::string *norm_key_out,
                                    std::string *error_out) {
    const std::string key = NormalizeFileKey(file_path);
    *norm_key_out = key;
    if (const auto it = cache.find(key); it != cache.end()) {
      return &it->second;
    }
    if (!allow_file_loading) {
      *error_out = "Cannot load external manifest \"" + PathToUtf8(file_path) +
                   "\" in in-memory resolution mode";
      return nullptr;
    }
    BaselineManifest loaded;
    if (!LoadBaselineManifestFile(PathToUtf8(file_path), &loaded, error_out)) {
      return nullptr;
    }
    const auto inserted = cache.emplace(key, std::move(loaded));
    return &inserted.first->second;
  }

  bool ResolveRecursive(const std::filesystem::path &file_path,
                        const std::string &target_name,
                        std::map<std::string, XfailEntry> *resolved_out,
                        std::map<std::string, XfailEntry> *parent_out,
                        std::string *error_out) {
    std::string norm_key;
    const BaselineManifest *manifest =
        GetOrLoad(file_path, &norm_key, error_out);
    if (manifest == nullptr) {
      return false;
    }
    const auto target_it = manifest->targets.find(target_name);
    if (target_it == manifest->targets.end()) {
      *error_out = "Baseline parent target \"" + target_name +
                   "\" not found in \"" + PathToUtf8(file_path) + "\"";
      return false;
    }
    const StackEntry node{norm_key, target_name};
    if (std::find(active_stack.begin(), active_stack.end(), node) !=
        active_stack.end()) {
      *error_out = FormatCycleError(active_stack, target_name);
      return false;
    }

    ScopedStackPush stack_guard(&active_stack, node);
    const TargetDefinition &target = target_it->second;
    std::map<std::string, XfailEntry> parent_entries;
    if (!target.extends.empty()) {
      std::filesystem::path parent_file;
      std::map<std::string, XfailEntry> grandparent_ignored;
      if (!ResolveParentFilePath(file_path, target_name, target,
                                 allow_file_loading, &parent_file, error_out) ||
          !ResolveRecursive(parent_file, target.extends, &parent_entries,
                            &grandparent_ignored, error_out) ||
          !ValidateRemoveXfailsInParent(target_name, target, parent_entries,
                                        error_out)) {
        return false;
      }
    }

    *resolved_out = ApplyTargetDelta(parent_entries, target);
    *parent_out = std::move(parent_entries);
    return true;
  }
};

bool ResolveTargetInheritance(const BaselineManifest &manifest,
                              const std::string &manifest_path,
                              bool allow_file_loading,
                              std::string_view requested_target,
                              uint32_t os_build, std::string_view target_arch,
                              ResolvedBaseline *out, std::string *error_out) {
  std::string local_err;
  std::string *err = (error_out != nullptr) ? error_out : &local_err;

  std::string target_name;
  if (!SelectTargetName(manifest, requested_target, &target_name, err)) {
    return false;
  }

  const std::filesystem::path root_path = PathFromUtf8(manifest_path);
  ResolutionContext ctx;
  ctx.allow_file_loading = allow_file_loading;
  ctx.cache.emplace(NormalizeFileKey(root_path), manifest);

  ResolvedBaseline resolved;
  resolved.manifest_path = manifest_path;
  resolved.target_name = target_name;
  if (!ctx.ResolveRecursive(root_path, target_name, &resolved.all_entries,
                            &resolved.parent_entries, err)) {
    return false;
  }

  for (const auto &[id, entry] : resolved.all_entries) {
    if (entry.MatchesHost(os_build, target_arch)) {
      resolved.active_entries.emplace(id, entry);
    }
  }
  *out = std::move(resolved);
  return true;
}

bool ParseTargetsSection(const nlohmann::ordered_json &root,
                         BaselineManifest *manifest, std::string *error_out) {
  const auto targets_it = root.find("targets");
  if (targets_it == root.end()) {
    *error_out = "Missing required root field \"targets\"";
    return false;
  }
  if (!targets_it->is_object()) {
    *error_out = "Field \"targets\" must be a JSON object";
    return false;
  }
  for (const auto &[target_name, target_json] : targets_it->items()) {
    TargetDefinition def;
    if (!ParseTargetObject(target_name, target_json, &def, error_out)) {
      return false;
    }
    manifest->target_order.push_back(target_name);
    manifest->targets.emplace(target_name, std::move(def));
  }
  return true;
}

bool IsPathSeparator(std::filesystem::path::value_type c) {
  return c == '/' || c == '\\';
}

// Upper-cases a drive-letter root name ("c:" -> "C:") so that paths differing
// only in drive-letter case compare equal.
std::filesystem::path NormalizeDriveLetter(const std::filesystem::path &p) {
  std::filesystem::path::string_type root = p.root_name().native();
  if (root.size() != 2 || root[1] != ':' || root[0] < 'a' || root[0] > 'z') {
    return p;
  }
  root[0] = static_cast<std::filesystem::path::value_type>(root[0] - 'a' + 'A');
  return std::filesystem::path(root) / p.root_directory() / p.relative_path();
}

// Returns the volume a path lives on: the root name plus, for UNC paths
// (\\server\share), the share component.
std::filesystem::path VolumeOf(const std::filesystem::path &p) {
  const std::filesystem::path::string_type root = p.root_name().native();
  if (root.size() > 2 && IsPathSeparator(root[0]) && IsPathSeparator(root[1]) &&
      !p.relative_path().empty()) {
    return p.root_name() / p.root_directory() / *p.relative_path().begin();
  }
  return p.root_name();
}

// Absolute, normalized directory that relative extends_file values of the
// manifest at manifest_path resolve against (see ResolveParentFilePath).
bool AbsoluteManifestDir(const std::string &manifest_path,
                         std::filesystem::path *dir_out,
                         std::string *error_out) {
  std::filesystem::path dir = PathFromUtf8(manifest_path).parent_path();
  if (dir.empty()) {
    dir = ".";
  }
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(dir, ec);
  if (ec) {
    *error_out = "Failed to resolve the directory of baseline \"" +
                 manifest_path + "\": " + ec.message();
    return false;
  }
  *dir_out = NormalizeDriveLetter(abs.lexically_normal());
  return true;
}

} // namespace

bool XfailEntry::MatchesHost(uint32_t os_build,
                             std::string_view target_arch) const {
  if (os_min_build && os_build < *os_min_build) {
    return false;
  }
  if (os_max_build && os_build > *os_max_build) {
    return false;
  }
  return arch.empty() ||
         std::find(arch.begin(), arch.end(), target_arch) != arch.end();
}

bool ReadTextFileUtf8(const std::string &file_path, std::string *content_out,
                      std::string *error_out) {
  std::ifstream ifs(PathFromUtf8(file_path), std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    SetError(error_out,
             "Failed to open file for reading: \"" + file_path + "\"");
    return false;
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  if (ifs.bad()) {
    SetError(error_out, "Failed to read file: \"" + file_path + "\"");
    return false;
  }
  *content_out = ss.str();
  return true;
}

bool WriteTextFileUtf8(const std::string &file_path, std::string_view content,
                       std::string *error_out) {
  const std::filesystem::path dst = PathFromUtf8(file_path);
  if (dst.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
      SetError(error_out, "Failed to create parent directory for \"" +
                              file_path + "\": " + ec.message());
      return false;
    }
  }

  static std::atomic<uint64_t> tmp_seq{0};
  const uint64_t seq = tmp_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  std::filesystem::path tmp = dst;
  tmp += ".tmp." + std::to_string(::GetCurrentProcessId()) + "." +
         std::to_string(seq);

  const std::wstring tmp_w = tmp.wstring();
  const std::wstring dst_w = dst.wstring();
  {
    core::UniqueHandle file(::CreateFileW(tmp_w.c_str(), GENERIC_WRITE,
                                          /*dwShareMode=*/0, nullptr,
                                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                          nullptr));
    if (!file) {
      const DWORD err = ::GetLastError();
      SetError(error_out, "Failed to open temporary file for writing: \"" +
                              PathToUtf8(tmp) +
                              "\": " + core::FormatWin32Error(err));
      return false;
    }
    auto write_res =
        core::WriteAllBytes(file.get(), content.data(), content.size());
    DWORD write_err = write_res.error;
    // Persist the data before the rename publishes it, so a crash or power
    // loss cannot leave a renamed but empty/truncated manifest.
    if (write_err == ERROR_SUCCESS && ::FlushFileBuffers(file.get()) == FALSE) {
      write_err = ::GetLastError();
    }
    file.reset();
    if (write_err != ERROR_SUCCESS) {
      ::DeleteFileW(tmp_w.c_str());
      SetError(error_out, "Failed to write file: \"" + file_path +
                              "\": " + core::FormatWin32Error(write_err));
      return false;
    }
  }

  if (::MoveFileExW(tmp_w.c_str(), dst_w.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    const DWORD err = ::GetLastError();
    ::DeleteFileW(tmp_w.c_str());
    SetError(error_out, "Failed to atomically replace \"" + file_path +
                            "\": " + core::FormatWin32Error(err));
    return false;
  }
  return true;
}

bool ParseBaselineManifest(std::string_view json_text, BaselineManifest *out,
                           std::string *error_out) {
  std::string local_err;
  std::string *err = (error_out != nullptr) ? error_out : &local_err;

  nlohmann::ordered_json root;
  if (!ParseJsonStrict(json_text, &root, err)) {
    return false;
  }
  if (!root.is_object()) {
    *err = "Baseline manifest root must be a JSON object";
    return false;
  }
  if (!ValidateAllowedKeys(root, {"schema_version", "description", "targets"},
                           "baseline manifest", err)) {
    return false;
  }

  BaselineManifest manifest;
  std::optional<uint32_t> schema_version;
  if (!ParseOptionalUint32Field(root, "schema_version", "baseline manifest",
                                &schema_version, err)) {
    return false;
  }
  if (!schema_version.has_value()) {
    *err = "Missing required root field \"schema_version\"";
    return false;
  }
  if (*schema_version != 1) {
    *err = "Field \"schema_version\" must be integer 1";
    return false;
  }
  manifest.schema_version = *schema_version;

  if (!ParseOptionalStringField(root, "description", "baseline manifest",
                                &manifest.description, err) ||
      !ParseTargetsSection(root, &manifest, err)) {
    return false;
  }

  *out = std::move(manifest);
  return true;
}

bool LoadBaselineManifestFile(const std::string &file_path,
                              BaselineManifest *out, std::string *error_out) {
  std::string content;
  if (!ReadTextFileUtf8(file_path, &content, error_out)) {
    return false;
  }
  std::string parse_err;
  if (!ParseBaselineManifest(content, out, &parse_err)) {
    SetError(error_out, "In baseline \"" + file_path + "\": " + parse_err);
    return false;
  }
  return true;
}

bool ResolveBaselineFromLoadedManifest(const BaselineManifest &manifest,
                                       const std::string &manifest_path,
                                       std::string_view requested_target,
                                       uint32_t os_build,
                                       std::string_view target_arch,
                                       ResolvedBaseline *out,
                                       std::string *error_out) {
  return ResolveTargetInheritance(manifest, manifest_path,
                                  /*allow_file_loading=*/true, requested_target,
                                  os_build, target_arch, out, error_out);
}

bool ResolveBaselineFromFile(const std::string &file_path,
                             std::string_view requested_target,
                             uint32_t os_build, std::string_view target_arch,
                             ResolvedBaseline *out, std::string *error_out) {
  std::string local_err;
  std::string *err = (error_out != nullptr) ? error_out : &local_err;

  BaselineManifest manifest;
  if (!LoadBaselineManifestFile(file_path, &manifest, err)) {
    return false;
  }
  return ResolveBaselineFromLoadedManifest(
      manifest, file_path, requested_target, os_build, target_arch, out, err);
}

bool ResolveBaselineInMemory(const BaselineManifest &manifest,
                             std::string_view requested_target,
                             uint32_t os_build, std::string_view target_arch,
                             ResolvedBaseline *out, std::string *error_out) {
  return ResolveTargetInheritance(manifest, "<memory>",
                                  /*allow_file_loading=*/false,
                                  requested_target, os_build, target_arch, out,
                                  error_out);
}

bool RebaseExtendsFilePaths(BaselineManifest *manifest,
                            const std::string &from_manifest_path,
                            const std::string &to_manifest_path,
                            std::string *error_out) {
  std::string local_err;
  std::string *err = (error_out != nullptr) ? error_out : &local_err;
  if (manifest == nullptr) {
    *err = "Internal error: null manifest";
    return false;
  }

  std::filesystem::path from_dir;
  std::filesystem::path to_dir;
  if (!AbsoluteManifestDir(from_manifest_path, &from_dir, err) ||
      !AbsoluteManifestDir(to_manifest_path, &to_dir, err)) {
    return false;
  }
  if (from_dir == to_dir) {
    return true;
  }

  // Compute every rewrite first so a failure leaves *manifest untouched.
  std::vector<std::pair<TargetDefinition *, std::string>> rewrites;
  for (auto &[name, target] : manifest->targets) {
    if (target.extends_file.empty()) {
      continue;
    }
    const std::filesystem::path rel = PathFromUtf8(target.extends_file);
    if (rel.is_absolute()) {
      continue;
    }
    const std::filesystem::path resolved =
        NormalizeDriveLetter((from_dir / rel).lexically_normal());
    std::filesystem::path rebased;
    if (VolumeOf(resolved) == VolumeOf(to_dir)) {
      rebased = resolved.lexically_relative(to_dir);
    }
    if (rebased.empty()) {
      *err = "Cannot rebase \"extends_file\" (\"" + target.extends_file +
             "\") of target \"" + name + "\" from \"" + from_manifest_path +
             "\" to \"" + to_manifest_path + "\": \"" + PathToUtf8(resolved) +
             "\" is not reachable by a relative path (different drive or "
             "share); write the baseline next to the original or use an "
             "absolute \"extends_file\"";
      return false;
    }
    const std::u8string generic = rebased.generic_u8string();
    rewrites.emplace_back(
        &target, std::string(reinterpret_cast<const char *>(generic.data()),
                             generic.size()));
  }
  for (auto &[target, extends_file] : rewrites) {
    target->extends_file = std::move(extends_file);
  }
  return true;
}

bool ValidateBaselineTestIds(
    const ResolvedBaseline &baseline,
    const std::unordered_set<std::string> &registered_tests, bool allow_unknown,
    std::vector<std::string> *unknown_ids_out, std::string *error_out) {
  std::vector<std::string> unknown;
  for (const auto &entry : baseline.all_entries) {
    if (!registered_tests.contains(entry.first)) {
      unknown.push_back(entry.first);
    }
  }
  std::sort(unknown.begin(), unknown.end());

  if (unknown_ids_out != nullptr) {
    *unknown_ids_out = unknown;
  }
  if (unknown.empty() || allow_unknown) {
    return true;
  }

  constexpr std::size_t kPreviewLimit = 5;
  std::ostringstream os;
  os << "Baseline target \"" << baseline.target_name << "\" references "
     << unknown.size() << " unknown test ID(s) not compiled into this binary: "
     << JoinWithComma(unknown, kPreviewLimit);
  if (unknown.size() > kPreviewLimit) {
    os << " (and " << (unknown.size() - kPreviewLimit) << " more)";
  }
  SetError(error_out, os.str());
  return false;
}

} // namespace fstest::cli
