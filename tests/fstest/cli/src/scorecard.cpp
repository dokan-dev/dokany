// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "scorecard.h"

#include "fstest/suite/test_metadata.h"
#include "json_util.h"

#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>
#include <utility>

namespace fstest::cli {
namespace {

void AddRecordToCounts(DomainCounts *counts, const TestRecord &rec) {
  ++counts->total;
  switch (rec.outcome) {
  case TestOutcome::kPass:
    ++counts->pass;
    break;
  case TestOutcome::kXFail:
    ++counts->xfail;
    if (rec.skip_crash) {
      ++counts->crash_skipped;
    }
    break;
  case TestOutcome::kXPass:
    ++counts->xpass;
    break;
  case TestOutcome::kXPassAllowed:
    ++counts->xpass_allowed;
    break;
  case TestOutcome::kFail:
    ++counts->fail;
    break;
  case TestOutcome::kSkip:
    ++counts->skip;
    break;
  }
}

std::string FormatScoreCell(const DomainCounts &c) {
  const std::optional<double> pct = c.ScorePercent();
  if (!pct.has_value()) {
    return "    N/A";
  }
  std::ostringstream os;
  os << std::fixed << std::setprecision(1) << std::setw(6) << *pct << "%";
  return os.str();
}

std::string TruncateDomainLabel(std::string_view label, std::size_t width) {
  if (label.size() <= width) {
    return std::string(label);
  }
  if (width <= 3) {
    return std::string(label.substr(0, width));
  }
  return std::string(label.substr(0, width - 3)) + "...";
}

void AppendRow(std::ostringstream *os, std::string_view domain,
               const DomainCounts &c) {
  const uint32_t pass_col = c.pass + c.xpass_allowed;
  *os << std::left << std::setw(18) << TruncateDomainLabel(domain, 18) << " | "
      << std::right << std::setw(5) << c.total << " | " << std::setw(5)
      << pass_col << " | " << std::setw(5) << c.xfail << " | " << std::setw(5)
      << c.xpass << " | " << std::setw(4) << c.fail << " | " << std::setw(4)
      << c.skip << " | " << FormatScoreCell(c) << "\n";
}

std::string FirstNonEmptyLine(std::string_view text) {
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    const std::size_t last = line.find_last_not_of(" \t\r");
    if (last != std::string_view::npos) {
      line = line.substr(0, last + 1);
      const std::size_t first = line.find_first_not_of(" \t");
      if (first != std::string_view::npos) {
        return std::string(line.substr(first));
      }
    }
    if (nl == std::string_view::npos) {
      break;
    }
    text.remove_prefix(nl + 1);
  }
  return {};
}

// Orders domains in canonical suite order first, then any extra/custom domains
// in lexicographical order.
std::vector<std::pair<std::string, DomainCounts>>
OrderDomainsCanonically(const std::map<std::string, DomainCounts> &domains) {
  std::vector<std::pair<std::string, DomainCounts>> ordered;
  ordered.reserve(domains.size());
  std::map<std::string, DomainCounts> remaining = domains;
  for (const DomainInfo &info : GetAllDomains()) {
    const auto it = remaining.find(std::string(info.domain_id));
    if (it != remaining.end()) {
      ordered.emplace_back(it->first, it->second);
      remaining.erase(it);
    }
  }
  for (const auto &[domain, counts] : remaining) {
    ordered.emplace_back(domain, counts);
  }
  return ordered;
}

std::string_view XpassPolicyToString(XpassPolicy policy) {
  return policy == XpassPolicy::kWarn ? "warn" : "fail";
}

std::string_view ExitCodeLabel(int exit_code) {
  switch (exit_code) {
  case 0:
    return "PASS";
  case 1:
    return "FAIL";
  case 2:
    return "ERROR";
  case 3:
    return "XPASS";
  default:
    return "UNKNOWN";
  }
}

void AppendScorecardHeader(std::ostringstream *os,
                           const ScorecardMetadata &meta,
                           const std::string &bar, const std::string &sep) {
  *os << "\n" << bar << "\n";
  *os << "NTFS Conformance Scorecard";
  if (!meta.mode.empty()) {
    *os << " [mode: " << meta.mode;
    if (!meta.target.empty()) {
      *os << " | target: " << meta.target;
    }
    if (!meta.target_arch.empty()) {
      *os << " | arch: " << meta.target_arch;
    }
    *os << "]";
  }
  *os << "\n";
  if (!meta.root_path.empty()) {
    *os << "Root: " << meta.root_path;
    if (!meta.capabilities.filesystem_name.empty()) {
      *os << " (" << meta.capabilities.filesystem_name << ")";
    }
    if (meta.os.build > 0) {
      *os << " | OS build " << meta.os.build;
    }
    *os << "\n";
  }
  *os << bar << "\n";
  *os << "Domain             | Total |  Pass | XFail | XPass | Fail | Skip | "
         "  Score\n";
  *os << sep << "\n";
}

void AppendScorecardFooter(std::ostringstream *os,
                           const ScorecardMetadata &meta,
                           const DomainCounts &summary,
                           const std::string &bar) {
  *os << bar << "\n";
  *os << "Result: " << ExitCodeLabel(meta.exit_code) << " (exit "
      << meta.exit_code << ") | Elapsed: " << std::fixed << std::setprecision(2)
      << (static_cast<double>(meta.duration_ms) / 1000.0) << "s";
  if (summary.xpass_allowed > 0) {
    *os << " | XPassAllowed: " << summary.xpass_allowed << " (in Pass)";
  }
  if (summary.crash_skipped > 0) {
    *os << " | CrashSkipped: " << summary.crash_skipped << " (in XFail)";
  }
  *os << "\n";
}

template <typename FormatEntryFn>
void AppendDiagnosticsSection(std::ostringstream *os, std::string_view heading,
                              uint32_t count,
                              const std::vector<TestRecord> &tests,
                              TestOutcome target_outcome,
                              FormatEntryFn &&format_entry) {
  if (count == 0) {
    return;
  }
  *os << "\n" << heading << " (" << count << "):\n";
  for (const TestRecord &t : tests) {
    if (t.outcome == target_outcome) {
      format_entry(os, t);
    }
  }
}

nlohmann::ordered_json CountsToJson(const DomainCounts &c) {
  nlohmann::ordered_json j = nlohmann::ordered_json::object();
  j["total"] = c.total;
  j["pass"] = c.pass;
  j["xfail"] = c.xfail;
  j["crash_skipped"] = c.crash_skipped;
  j["xpass"] = c.xpass;
  j["xpass_allowed"] = c.xpass_allowed;
  j["fail"] = c.fail;
  j["skip"] = c.skip;
  const std::optional<double> pct = c.ScorePercent();
  if (pct.has_value()) {
    j["score_pct"] = std::round(*pct * 100.0) / 100.0;
  } else {
    j["score_pct"] = nullptr;
  }
  return j;
}

nlohmann::ordered_json OsInfoToJson(const OsReportInfo &os_info) {
  nlohmann::ordered_json j = nlohmann::ordered_json::object();
  j["major"] = os_info.major;
  j["minor"] = os_info.minor;
  j["build"] = os_info.build;
  j["ubr"] = os_info.ubr;
  j["is_server"] = os_info.is_server;
  j["host_arch"] = os_info.host_arch;
  j["process_arch"] = os_info.process_arch;
  j["is_elevated"] = os_info.is_elevated;
  j["can_create_symlinks"] = os_info.can_create_symlinks;
  return j;
}

nlohmann::ordered_json CapabilitiesToJson(const CapabilitiesReportInfo &cap) {
  nlohmann::ordered_json j = nlohmann::ordered_json::object();
  j["filesystem_name"] = cap.filesystem_name;
  j["max_component_length"] = cap.max_component_length;
  j["fs_flags"] = cap.fs_flags;
  j["bytes_per_sector"] = cap.bytes_per_sector;
  j["physical_bytes_per_sector"] = cap.physical_bytes_per_sector;
  j["bytes_per_cluster"] = cap.bytes_per_cluster;
  j["is_volume_root"] = cap.is_volume_root;
  j["short_names_enabled"] = cap.short_names_enabled;
  j["respect_fs_capabilities"] = cap.respect_fs_capabilities;
  return j;
}

nlohmann::ordered_json TestRecordToJson(const TestRecord &t) {
  nlohmann::ordered_json rec = nlohmann::ordered_json::object();
  rec["id"] = t.id;
  rec["domain"] = t.domain;
  rec["outcome"] = OutcomeToString(t.outcome);
  rec["fatal_teardown"] = t.fatal_teardown;
  rec["fatal_setup"] = t.fatal_setup;
  rec["exception"] = t.exception;
  rec["synthetic"] = t.synthetic;
  rec["skip_crash"] = t.skip_crash;
  rec["duration_ms"] = t.duration_ms;
  rec["iterations"] = t.iterations;
  rec["xfail_reason"] = t.xfail_reason;
  rec["skip_reason"] = t.skip_reason;
  rec["failure_messages"] = t.failure_messages;
  return rec;
}

} // namespace

TestOutcome ClassifyOutcome(RawTestOutcome raw, bool unmaskable_failure,
                            const XfailEntry *active_xfail) {
  if (unmaskable_failure) {
    return TestOutcome::kFail;
  }
  switch (raw) {
  case RawTestOutcome::kSkip:
    return TestOutcome::kSkip;
  case RawTestOutcome::kPass:
    if (active_xfail == nullptr) {
      return TestOutcome::kPass;
    }
    return active_xfail->allow_xpass ? TestOutcome::kXPassAllowed
                                     : TestOutcome::kXPass;
  case RawTestOutcome::kFail:
    return active_xfail != nullptr ? TestOutcome::kXFail : TestOutcome::kFail;
  }
  return TestOutcome::kFail;
}

int OutcomeSeverity(TestOutcome outcome) {
  switch (outcome) {
  case TestOutcome::kSkip:
    return 0;
  case TestOutcome::kPass:
    return 1;
  case TestOutcome::kXPassAllowed:
    return 2;
  case TestOutcome::kXFail:
    return 3;
  case TestOutcome::kXPass:
    return 4;
  case TestOutcome::kFail:
    return 5;
  }
  return 5;
}

TestOutcome MergeRepeatedOutcome(TestOutcome existing, TestOutcome incoming) {
  return OutcomeSeverity(incoming) > OutcomeSeverity(existing) ? incoming
                                                               : existing;
}

std::string_view OutcomeToString(TestOutcome outcome) {
  switch (outcome) {
  case TestOutcome::kPass:
    return "PASS";
  case TestOutcome::kXFail:
    return "XFAIL";
  case TestOutcome::kXPass:
    return "XPASS";
  case TestOutcome::kXPassAllowed:
    return "XPASS_ALLOWED";
  case TestOutcome::kFail:
    return "FAIL";
  case TestOutcome::kSkip:
    return "SKIP";
  }
  return "FAIL";
}

std::optional<double> DomainCounts::ScorePercent() const {
  if (total <= skip) {
    return std::nullopt;
  }
  const uint32_t denominator = total - skip;
  const uint32_t numerator = pass + xpass + xpass_allowed;
  return (static_cast<double>(numerator) * 100.0) /
         static_cast<double>(denominator);
}

void Scorecard::RecordTest(TestRecord record) {
  const auto it = records_.find(record.id);
  if (it == records_.end()) {
    records_.emplace(record.id, std::move(record));
    return;
  }

  TestRecord &existing = it->second;
  existing.outcome = MergeRepeatedOutcome(existing.outcome, record.outcome);
  existing.fatal_teardown = existing.fatal_teardown || record.fatal_teardown;
  existing.fatal_setup = existing.fatal_setup || record.fatal_setup;
  existing.exception = existing.exception || record.exception;
  existing.synthetic = existing.synthetic || record.synthetic;
  existing.skip_crash = existing.skip_crash || record.skip_crash;
  existing.duration_ms += record.duration_ms;
  existing.iterations += record.iterations;
  if (existing.xfail_reason.empty() && !record.xfail_reason.empty()) {
    existing.xfail_reason = std::move(record.xfail_reason);
  }
  if (existing.outcome != TestOutcome::kSkip) {
    existing.skip_reason.clear();
  } else if (existing.skip_reason.empty() && !record.skip_reason.empty()) {
    existing.skip_reason = std::move(record.skip_reason);
  }
  for (std::string &msg : record.failure_messages) {
    existing.failure_messages.push_back(std::move(msg));
  }
}

DomainCounts Scorecard::Summary() const {
  DomainCounts total;
  for (const auto &kv : records_) {
    AddRecordToCounts(&total, kv.second);
  }
  return total;
}

std::map<std::string, DomainCounts> Scorecard::ByDomain() const {
  std::map<std::string, DomainCounts> by_domain;
  for (const auto &kv : records_) {
    const TestRecord &rec = kv.second;
    const std::string key = rec.domain.empty() ? "unknown" : rec.domain;
    AddRecordToCounts(&by_domain[key], rec);
  }
  return by_domain;
}

std::vector<TestRecord> Scorecard::Tests() const {
  std::vector<TestRecord> out;
  out.reserve(records_.size());
  for (const auto &kv : records_) {
    out.push_back(kv.second);
  }
  return out;
}

std::vector<ObservedTestResult> Scorecard::ToObservedResults() const {
  std::vector<ObservedTestResult> out;
  out.reserve(records_.size());
  for (const auto &kv : records_) {
    const TestRecord &rec = kv.second;
    if (rec.synthetic) {
      continue;
    }
    ObservedTestResult obs;
    obs.test_id = rec.id;
    obs.fatal_teardown = rec.fatal_teardown;
    obs.fatal_setup = rec.fatal_setup;
    obs.exception = rec.exception;
    obs.skip_crash = rec.skip_crash;
    switch (rec.outcome) {
    case TestOutcome::kPass:
    case TestOutcome::kXPass:
    case TestOutcome::kXPassAllowed:
      obs.passed = true;
      break;
    case TestOutcome::kFail:
    case TestOutcome::kXFail:
      obs.failed = true;
      break;
    case TestOutcome::kSkip:
      obs.skipped = true;
      break;
    }
    out.push_back(std::move(obs));
  }
  return out;
}

int ComputeExitCode(const DomainCounts &summary, XpassPolicy xpass_policy) {
  if (summary.total == 0) {
    return 2;
  }
  if (summary.fail > 0) {
    return 1;
  }
  if (summary.xpass > 0 && xpass_policy == XpassPolicy::kFail) {
    return 3;
  }
  // Nothing actually executed (every test skipped or crash-skipped): treat as
  // a configuration error rather than a vacuous pass.
  if (summary.total == summary.skip + summary.crash_skipped) {
    return 2;
  }
  return 0;
}

std::string FormatScorecardTable(const ScorecardMetadata &meta,
                                 const Scorecard &scorecard) {
  const DomainCounts summary = scorecard.Summary();
  const std::vector<TestRecord> tests = scorecard.Tests();

  std::ostringstream os;
  const std::string bar(74, '=');
  const std::string sep =
      "-------------------+-------+-------+-------+-------+------+------+------"
      "--";

  AppendScorecardHeader(&os, meta, bar, sep);
  for (const auto &[domain, counts] :
       OrderDomainsCanonically(scorecard.ByDomain())) {
    AppendRow(&os, domain, counts);
  }
  os << sep << "\n";
  AppendRow(&os, "TOTAL", summary);
  AppendScorecardFooter(&os, meta, summary, bar);

  AppendDiagnosticsSection(
      &os, "Unexpected FAILURES", summary.fail, tests, TestOutcome::kFail,
      [](std::ostringstream *out, const TestRecord &t) {
        *out << "  [FAIL] " << t.id;
        if (t.fatal_setup) {
          *out << " (fatal setup)";
        }
        if (t.fatal_teardown) {
          *out << " (fatal teardown)";
        }
        if (t.exception) {
          *out << " (exception)";
        }
        *out << "\n";
        if (!t.failure_messages.empty()) {
          const std::string line =
              FirstNonEmptyLine(t.failure_messages.front());
          if (!line.empty()) {
            *out << "    " << line << "\n";
          }
        }
      });

  AppendDiagnosticsSection(
      &os, "Unexpected PASSES / XPASS", summary.xpass, tests,
      TestOutcome::kXPass, [](std::ostringstream *out, const TestRecord &t) {
        *out << "  [XPASS] " << t.id << "\n";
        if (!t.xfail_reason.empty()) {
          *out << "    Baseline reason: " << t.xfail_reason << "\n";
        }
      });

  return os.str();
}

nlohmann::ordered_json BuildScorecardJson(const ScorecardMetadata &meta,
                                          const Scorecard &scorecard) {
  nlohmann::ordered_json root = nlohmann::ordered_json::object();
  root["schema_version"] = 1;
  root["mode"] = meta.mode;
  root["root_path"] = meta.root_path;
  root["baseline_path"] = meta.baseline_path;
  root["target"] = meta.target;
  root["target_arch"] = meta.target_arch;
  root["xpass_policy"] = XpassPolicyToString(meta.xpass_policy);
  root["exit_code"] = meta.exit_code;
  root["duration_ms"] = meta.duration_ms;
  root["os"] = OsInfoToJson(meta.os);
  root["capabilities"] = CapabilitiesToJson(meta.capabilities);
  root["summary"] = CountsToJson(scorecard.Summary());

  nlohmann::ordered_json domains_json = nlohmann::ordered_json::object();
  for (const auto &[domain, counts] :
       OrderDomainsCanonically(scorecard.ByDomain())) {
    domains_json[domain] = CountsToJson(counts);
  }
  root["domains"] = std::move(domains_json);

  nlohmann::ordered_json tests_json = nlohmann::ordered_json::array();
  for (const TestRecord &t : scorecard.Tests()) {
    tests_json.push_back(TestRecordToJson(t));
  }
  root["tests"] = std::move(tests_json);

  return root;
}

bool WriteScorecardJsonFile(const std::string &file_path,
                            const ScorecardMetadata &meta,
                            const Scorecard &scorecard,
                            std::string *error_out) {
  try {
    const nlohmann::ordered_json report = BuildScorecardJson(meta, scorecard);
    const std::string serialized = SerializeJsonDeterministic(
        report, nlohmann::ordered_json::error_handler_t::replace);
    return WriteTextFileUtf8(file_path, serialized, error_out);
  } catch (const std::exception &ex) {
    if (error_out != nullptr) {
      *error_out = ex.what();
    }
    return false;
  }
}

} // namespace fstest::cli
