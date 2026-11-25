// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "gtest_filter.h"

#include <cstddef>
#include <unordered_set>

namespace fstest::cli {
namespace {

bool MatchGlobPattern(std::string_view pattern, std::string_view text) {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star_p = std::string_view::npos;
  std::size_t star_t = 0;

  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star_p = p++;
      star_t = t;
    } else if (star_p != std::string_view::npos) {
      p = star_p + 1;
      t = ++star_t;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

bool MatchPatternList(std::string_view list, std::string_view text) {
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t colon = list.find(':', start);
    const std::string_view pat = (colon == std::string_view::npos)
                                     ? list.substr(start)
                                     : list.substr(start, colon - start);
    if (!pat.empty() && MatchGlobPattern(pat, text)) {
      return true;
    }
    if (colon == std::string_view::npos) {
      break;
    }
    start = colon + 1;
  }
  return false;
}

std::unordered_set<std::string_view>
CollectNegativeTokens(std::string_view filter, std::size_t dash_pos) {
  std::unordered_set<std::string_view> tokens;
  if (dash_pos == std::string_view::npos) {
    return tokens;
  }
  std::string_view neg = filter.substr(dash_pos + 1);
  while (!neg.empty()) {
    const std::size_t colon = neg.find(':');
    const std::string_view token = neg.substr(0, colon);
    if (!token.empty()) {
      tokens.insert(token);
    }
    if (colon == std::string_view::npos) {
      break;
    }
    neg.remove_prefix(colon + 1);
  }
  return tokens;
}

} // namespace

bool MatchesGtestFilter(std::string_view filter, std::string_view test_id) {
  // GoogleTest semantics: an empty --gtest_filter= has an empty positive
  // pattern list with no '-' section and therefore selects no tests.
  if (filter.empty()) {
    return false;
  }
  const std::size_t dash = filter.find('-');
  const std::string_view positive =
      (dash == std::string_view::npos) ? filter : filter.substr(0, dash);
  const std::string_view negative = (dash == std::string_view::npos)
                                        ? std::string_view{}
                                        : filter.substr(dash + 1);

  const bool pos_ok = positive.empty() || MatchPatternList(positive, test_id);
  if (!pos_ok) {
    return false;
  }
  if (!negative.empty() && MatchPatternList(negative, test_id)) {
    return false;
  }
  return true;
}

std::string
AppendNegativeGtestFilter(std::string_view existing_filter,
                          const std::vector<std::string> &exclude_ids) {
  const std::size_t dash = existing_filter.find('-');
  std::unordered_set<std::string_view> seen =
      CollectNegativeTokens(existing_filter, dash);

  // An empty filter selects nothing; never widen it to "*-<ids>".
  if (existing_filter.empty()) {
    return std::string();
  }

  std::vector<std::string_view> to_add;
  to_add.reserve(exclude_ids.size());
  for (const std::string &id : exclude_ids) {
    if (!id.empty() && seen.insert(id).second) {
      to_add.push_back(id);
    }
  }
  if (to_add.empty()) {
    return std::string(existing_filter);
  }

  std::string out(existing_filter);
  if (dash == std::string_view::npos) {
    out.push_back('-');
  } else if (existing_filter.back() != '-' && existing_filter.back() != ':') {
    out.push_back(':');
  }
  for (std::size_t i = 0; i < to_add.size(); ++i) {
    if (i > 0) {
      out.push_back(':');
    }
    out.append(to_add[i]);
  }
  return out;
}

bool IsDisabledTestId(std::string_view test_id) {
  auto is_disabled_name = [](std::string_view name) {
    return name.starts_with("DISABLED_") ||
           name.find("/DISABLED_") != std::string_view::npos;
  };
  const std::size_t dot = test_id.find('.');
  if (dot == std::string_view::npos) {
    return is_disabled_name(test_id);
  }
  return is_disabled_name(test_id.substr(0, dot)) ||
         is_disabled_name(test_id.substr(dot + 1));
}

std::vector<std::string>
SelectSkipCrashPreRecords(const std::vector<std::string> &registered_in_order,
                          const std::set<std::string> &skip_crash_ids,
                          std::string_view filter, bool also_run_disabled,
                          const ShardSpec &shard) {
  std::vector<std::string> selected;
  const bool sharded = shard.total_shards > 0;
  int64_t runnable_index = 0;
  for (const std::string &id : registered_in_order) {
    const bool runnable = (also_run_disabled || !IsDisabledTestId(id)) &&
                          MatchesGtestFilter(filter, id);
    if (!runnable) {
      continue;
    }
    const bool owned =
        !sharded || (runnable_index % shard.total_shards) == shard.shard_index;
    ++runnable_index;
    if (owned && skip_crash_ids.contains(id)) {
      selected.push_back(id);
    }
  }
  return selected;
}

} // namespace fstest::cli
