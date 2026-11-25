// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fstest::cli {

// Matches a canonical test ID ("Suite.Test") against a GoogleTest
// --gtest_filter specification ("pos_patterns[-neg_patterns]", ':'-separated,
// supporting '*' and '?' wildcards). Mirrors GoogleTest: an empty filter
// selects nothing, while an empty positive section before '-' means "*".
bool MatchesGtestFilter(std::string_view filter, std::string_view test_id);

// Appends exact test IDs to the negative section of a GoogleTest filter string
// so they are excluded from execution while preserving existing filters. An
// empty filter (which selects nothing) is returned unchanged.
std::string
AppendNegativeGtestFilter(std::string_view existing_filter,
                          const std::vector<std::string> &exclude_ids);

// Returns true if GoogleTest treats "Suite.Test" as disabled (suite or test
// name starts with "DISABLED_" or contains "/DISABLED_").
bool IsDisabledTestId(std::string_view test_id);

// GoogleTest sharding parameters (GTEST_TOTAL_SHARDS / GTEST_SHARD_INDEX).
// total_shards <= 0 means "not sharded".
struct ShardSpec {
  int32_t total_shards = 0;
  int32_t shard_index = 0;
};

// Selects the skip_crash baseline IDs this process must pre-record as XFAIL
// (they are excluded from execution, so no GoogleTest event reports them):
// IDs that are registered, match `filter`, are not DISABLED_ (unless
// `also_run_disabled`), and, when sharded, are owned by this shard. Ownership
// is the index among runnable tests in registration order modulo
// total_shards, so every skip_crash test is recorded by exactly one shard.
// Returns IDs in registration order.
std::vector<std::string>
SelectSkipCrashPreRecords(const std::vector<std::string> &registered_in_order,
                          const std::set<std::string> &skip_crash_ids,
                          std::string_view filter, bool also_run_disabled,
                          const ShardSpec &shard);

} // namespace fstest::cli
