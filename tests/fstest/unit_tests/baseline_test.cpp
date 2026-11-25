// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "baseline.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/path.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {
namespace {

std::wstring GetHostTempDir() {
  wchar_t buf[MAX_PATH + 1]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, buf);
  return (len > 0 && len < MAX_PATH)
             ? core::NormalizeWin32Path(std::wstring_view(buf, len))
             : L"";
}

class ScopedTestDirectory {
public:
  ScopedTestDirectory() {
    const std::wstring host_tmp = GetHostTempDir();
    EXPECT_FALSE(host_tmp.empty());
    auto dir = core::ScopedTempDir::Create(host_tmp, L"fstbl");
    EXPECT_TRUE(dir.ok()) << core::FormatWin32Error(dir.error);
    if (dir.ok()) {
      dir_ = std::move(*dir.value);
    }
  }

  ~ScopedTestDirectory() {
    if (dir_.has_value()) {
      std::string err;
      EXPECT_TRUE(dir_->Cleanup(&err)) << err;
      dir_.reset();
    }
  }

  ScopedTestDirectory(const ScopedTestDirectory &) = delete;
  ScopedTestDirectory &operator=(const ScopedTestDirectory &) = delete;

  std::string WriteFile(const std::string &rel, const std::string &content) {
    const std::string utf8 = ChildPathUtf8(rel);
    std::string err;
    EXPECT_TRUE(WriteTextFileUtf8(utf8, content, &err)) << err;
    return utf8;
  }

  std::string ChildPathUtf8(const std::string &rel) const {
    EXPECT_TRUE(dir_.has_value());
    return dir_.has_value()
               ? core::WideToUtf8(dir_->ChildPath(core::Utf8ToWide(rel)))
               : std::string();
  }

  std::wstring Path() const { return dir_.has_value() ? dir_->Path() : L""; }

private:
  std::optional<core::ScopedTempDir> dir_;
};

// Lists the names in dir, excluding "." and "..".
std::vector<std::wstring> ListChildren(const std::wstring &dir) {
  std::vector<std::wstring> names;
  auto entries = core::ListDirectoryWin32(dir);
  EXPECT_TRUE(entries.ok()) << core::FormatWin32Error(entries.error);
  if (!entries.ok()) {
    return names;
  }
  for (const std::wstring &name : *entries.value) {
    if (name != L"." && name != L"..") {
      names.push_back(name);
    }
  }
  return names;
}

// Expects ParseBaselineManifest to reject json with an error containing
// expected_error.
void ExpectParseError(const std::string &json,
                      const std::string &expected_error) {
  BaselineManifest m;
  std::string err;
  EXPECT_FALSE(ParseBaselineManifest(json, &m, &err))
      << "Expected parse failure for: " << json;
  EXPECT_NE(err.find(expected_error), std::string::npos)
      << "Input: " << json << "\nExpected error containing: " << expected_error
      << "\nActual error: " << err;
}

// Wraps a single xfail entry body for test ID "S.T" of target "t".
std::string ManifestWithEntry(const std::string &entry_body) {
  return R"({"schema_version": 1, "targets": {"t": {"xfails": {"S.T": )" +
         entry_body + "}}}}";
}

ObservedTestResult MakeObs(std::string id, bool passed, bool failed,
                           bool skipped = false) {
  ObservedTestResult r;
  r.test_id = std::move(id);
  r.passed = passed;
  r.failed = failed;
  r.skipped = skipped;
  return r;
}

TEST(BaselineTest, AutoSelectsSoleConcreteTargetAndRejectsAbstract) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "base": {
        "abstract": true,
        "xfails": {
          "CreateOpenTest.Foo": {"reason": "base gap"}
        }
      },
      "drive": {
        "extends": "base",
        "xfails": {}
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;

  ResolvedBaseline resolved;
  ASSERT_TRUE(
      ResolveBaselineInMemory(manifest, "", 20348, "x64", &resolved, &err))
      << err;
  EXPECT_EQ(resolved.target_name, "drive");
  EXPECT_EQ(resolved.active_entries.size(), 1u);
  EXPECT_TRUE(resolved.active_entries.contains("CreateOpenTest.Foo"));

  // Directly selecting an abstract target must fail.
  ResolvedBaseline bad;
  EXPECT_FALSE(
      ResolveBaselineInMemory(manifest, "base", 20348, "x64", &bad, &err));
  EXPECT_NE(err.find("abstract"), std::string::npos) << err;
}

TEST(BaselineTest, MultiLevelInheritanceOverrideAndRemoveXfails) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "grandparent": {
        "abstract": true,
        "xfails": {
          "Suite.T1": {"reason": "gp1"},
          "Suite.T2": {"reason": "gp2"}
        }
      },
      "parent": {
        "abstract": true,
        "extends": "grandparent",
        "xfails": {
          "Suite.T2": {"reason": "parent_override"},
          "Suite.T3": {"reason": "p3"}
        }
      },
      "child": {
        "extends": "parent",
        "xfails": {
          "Suite.T4": {"reason": "c4"}
        },
        "remove_xfails": ["Suite.T1"]
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;

  ResolvedBaseline resolved;
  ASSERT_TRUE(
      ResolveBaselineInMemory(manifest, "child", 26100, "x64", &resolved, &err))
      << err;

  EXPECT_FALSE(resolved.active_entries.contains("Suite.T1"));
  ASSERT_TRUE(resolved.active_entries.contains("Suite.T2"));
  EXPECT_EQ(resolved.active_entries.at("Suite.T2").reason, "parent_override");
  ASSERT_TRUE(resolved.active_entries.contains("Suite.T3"));
  ASSERT_TRUE(resolved.active_entries.contains("Suite.T4"));
  EXPECT_EQ(resolved.parent_entries.size(), 3u);
}

TEST(BaselineTest, ExtendsFileCrossManifestInheritance) {
  ScopedTestDirectory dir;
  dir.WriteFile("base.json", R"({
    "schema_version": 1,
    "targets": {
      "mirror_base": {
        "abstract": true,
        "xfails": {
          "DeleteTest.Posix": {
            "reason": "legacy delete-pending",
            "os_min_build": 18362
          },
          "InfoQueriesTest.Ea": {
            "reason": "no EA support"
          }
        }
      }
    }
  })");

  const std::string overlay_path = dir.WriteFile("overlay.json", R"({
    "schema_version": 1,
    "targets": {
      "custom_drive": {
        "extends_file": "base.json",
        "extends": "mirror_base",
        "xfails": {
          "StreamsTest.Ads": {"reason": "no ADS"}
        },
        "remove_xfails": ["InfoQueriesTest.Ea"]
      }
    }
  })");

  ResolvedBaseline on_2019;
  std::string err;
  ASSERT_TRUE(ResolveBaselineFromFile(overlay_path, "custom_drive", 17763,
                                      "x64", &on_2019, &err))
      << err;
  EXPECT_FALSE(on_2019.active_entries.contains("InfoQueriesTest.Ea"));
  EXPECT_FALSE(on_2019.active_entries.contains("DeleteTest.Posix"));
  EXPECT_TRUE(on_2019.active_entries.contains("StreamsTest.Ads"));

  ResolvedBaseline on_2025;
  ASSERT_TRUE(ResolveBaselineFromFile(overlay_path, "custom_drive", 26100,
                                      "x64", &on_2025, &err))
      << err;
  EXPECT_TRUE(on_2025.active_entries.contains("DeleteTest.Posix"));
  EXPECT_TRUE(on_2025.active_entries.contains("StreamsTest.Ads"));
}

TEST(BaselineTest, DetectsIntraFileAndCrossFileCycles) {
  const std::string intra_cycle = R"({
    "schema_version": 1,
    "targets": {
      "a": {"extends": "b", "xfails": {}},
      "b": {"extends": "a", "xfails": {}}
    }
  })";
  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(intra_cycle, &manifest, &err)) << err;
  ResolvedBaseline resolved;
  EXPECT_FALSE(
      ResolveBaselineInMemory(manifest, "a", 26100, "x64", &resolved, &err));
  EXPECT_NE(err.find("cycle"), std::string::npos) << err;

  ScopedTestDirectory dir;
  const std::string f1 = dir.WriteFile("f1.json", R"({
    "schema_version": 1,
    "targets": {
      "t1": {"extends_file": "f2.json", "extends": "t2", "xfails": {}}
    }
  })");
  dir.WriteFile("f2.json", R"({
    "schema_version": 1,
    "targets": {
      "t2": {"extends_file": "f1.json", "extends": "t1", "xfails": {}}
    }
  })");

  EXPECT_FALSE(
      ResolveBaselineFromFile(f1, "t1", 26100, "x64", &resolved, &err));
  EXPECT_NE(err.find("cycle"), std::string::npos) << err;
}

TEST(BaselineTest, RejectsSchemaErrorsUnknownKeysWildcardsAndStaleRemoves) {
  const std::vector<std::pair<std::string, std::string>> invalid_manifests = {
      // Unknown root key
      {R"({"schema_version": 1, "extra": 1, "targets": {}})",
       R"(Unknown field "extra" in baseline manifest)"},
      // Wrong schema_version
      {R"({"schema_version": 2, "targets": {}})",
       R"(Field "schema_version" must be integer 1)"},
      // Unknown target key
      {R"({"schema_version": 1, "targets": {"t": {"unknown_key": true}}})",
       R"(Unknown field "unknown_key" in target "t")"},
      // Unknown xfail entry key (typo)
      {ManifestWithEntry(R"({"reason": "r", "os_min_biuld": 18362})"),
       R"(Unknown field "os_min_biuld" in xfail entry "S.T" of target "t")"},
      // Missing reason
      {ManifestWithEntry(R"({"os_min_build": 18362})"),
       R"(Missing required field "reason" in xfail entry "S.T" of target "t")"},
      // Blank reason
      {ManifestWithEntry(R"({"reason": "   "})"),
       R"(Field "reason" in xfail entry "S.T" of target "t" must be a non-empty string)"},
      // Wildcard test ID
      {R"({"schema_version": 1, "targets": {"t": {"xfails": {"CreateOpenTest.*": {"reason": "bad"}}}}})",
       R"(Invalid test ID "CreateOpenTest.*" in target "t")"},
      // os_min_build > os_max_build
      {ManifestWithEntry(
           R"({"reason": "r", "os_min_build": 20000, "os_max_build": 18000})"),
       "os_min_build (20000) > os_max_build (18000)"},
      // Invalid arch
      {ManifestWithEntry(R"({"reason": "r", "arch": ["mips"]})"),
       R"(Invalid arch "mips" in xfail entry "S.T")"},
      // Empty arch array
      {ManifestWithEntry(R"({"reason": "r", "arch": []})"),
       R"(Field "arch" in xfail entry "S.T" must be a non-empty array)"},
      // extends_file without extends
      {R"({"schema_version": 1, "targets": {"t": {"extends_file": "other.json"}}})",
       R"(Target "t" specifies "extends_file" without "extends")"},
      // remove_xfails without extends
      {R"({"schema_version": 1, "targets": {"t": {"remove_xfails": ["S.T"]}}})",
       R"(Target "t" specifies "remove_xfails" without "extends")"},
      // Same test ID in both remove_xfails and xfails
      {R"({"schema_version": 1, "targets": {"p": {"abstract": true, "xfails": {"S.T": {"reason": "r"}}}, "c": {"extends": "p", "xfails": {"S.T": {"reason": "r2"}}, "remove_xfails": ["S.T"]}}})",
       R"(Test ID "S.T" cannot appear in both "remove_xfails" and "xfails" of target "c")"},
      // Duplicate JSON key
      {R"({"schema_version": 1, "targets": {"t": {"xfails": {"S.T": {"reason": "r1"}, "S.T": {"reason": "r2"}}}}})",
       R"(Duplicate JSON key "S.T")"},
      // skip_crash combined with allow_xpass
      {ManifestWithEntry(
           R"({"reason": "r", "gap": "GAP-01", "skip_crash": true, "allow_xpass": true})"),
       R"(cannot set both "skip_crash" and "allow_xpass")"},
      // Non-boolean skip_crash
      {ManifestWithEntry(R"({"reason": "r", "skip_crash": "yes"})"),
       R"(Field "skip_crash" in xfail entry "S.T" of target "t" must be a boolean)"},
      // m10: ':' would split a --gtest_filter pattern list.
      {R"({"schema_version": 1, "targets": {"t": {"xfails": {"Suite.A:B.C": {"reason": "r"}}}}})",
       R"(Invalid test ID "Suite.A:B.C" in target "t")"},
      // m10: a leading '-' would start the negative filter section.
      {R"({"schema_version": 1, "targets": {"t": {"xfails": {"-Suite.A": {"reason": "r"}}}}})",
       R"(Invalid test ID "-Suite.A" in target "t")"},
      {R"({"schema_version": 1, "targets": {"p": {"abstract": true, "xfails": {"S.A": {"reason": "r"}}}, "c": {"extends": "p", "remove_xfails": ["S.A:S.B"]}}})",
       R"(Invalid test ID "S.A:S.B" in "remove_xfails" of target "c")"},
      // os_min_build / os_max_build must be unsigned 32-bit integers.
      {ManifestWithEntry(R"({"reason": "r", "os_min_build": 4294967296})"),
       R"(Field "os_min_build" in xfail entry "S.T" of target "t" must be an unsigned 32-bit integer)"},
      {ManifestWithEntry(R"({"reason": "r", "os_min_build": -1})"),
       R"(Field "os_min_build" in xfail entry "S.T" of target "t" must be an unsigned 32-bit integer)"},
      {ManifestWithEntry(R"({"reason": "r", "os_min_build": 1.0})"),
       R"(Field "os_min_build" in xfail entry "S.T" of target "t" must be an unsigned 32-bit integer)"},
      {ManifestWithEntry(R"({"reason": "r", "os_max_build": "26100"})"),
       R"(Field "os_max_build" in xfail entry "S.T" of target "t" must be an unsigned 32-bit integer)"},
      // NUL (\u0000) in a test ID, reason, or target name.
      {R"({"schema_version": 1, "targets": {"t": {"xfails": {"S.\u0000T": {"reason": "r"}}}}})",
       R"(Invalid test ID "S.)"},
      {ManifestWithEntry(R"({"reason": "before\u0000after"})"),
       R"(Field "reason" in xfail entry "S.T" of target "t" must not contain NUL)"},
      {R"({"schema_version": 1, "targets": {"t\u0000x": {"xfails": {}}}})",
       "Target name must not contain NUL"},
      // Lone UTF-16 surrogates are rejected by the JSON lexer.
      {ManifestWithEntry(R"({"reason": "lone \ud800 high"})"),
       "surrogate U+D800..U+DBFF must be followed by U+DC00..U+DFFF"},
      {R"({"schema_version": 1, "targets": {"t": {"xfails": {"S.\ud800T": {"reason": "r"}}}}})",
       "surrogate U+D800..U+DBFF must be followed by U+DC00..U+DFFF"},
      {ManifestWithEntry(R"({"reason": "lone \udc00 low"})"), "surrogate"},
      // Invalid UTF-8 (stray continuation byte, truncated sequence, overlong
      // encoding) in a reason or a test ID.
      {ManifestWithEntry(std::string(R"({"reason": "bad )") + "\xff" + R"("})"),
       "ill-formed UTF-8 byte"},
      {ManifestWithEntry(std::string(R"({"reason": "bad )") + "\xc3" + R"("})"),
       "ill-formed UTF-8 byte"},
      {std::string(
           R"({"schema_version": 1, "targets": {"t": {"xfails": {"S.)") +
           "\xc0\xaf" + R"(T": {"reason": "r"}}}}})",
       "ill-formed UTF-8 byte"},
  };

  for (const auto &[bad, expected_error] : invalid_manifests) {
    ExpectParseError(bad, expected_error);
  }

  // Boundary: UINT32_MAX is accepted.
  BaselineManifest max_build;
  std::string max_err;
  ASSERT_TRUE(ParseBaselineManifest(
      ManifestWithEntry(R"({"reason": "r", "os_max_build": 4294967295})"),
      &max_build, &max_err))
      << max_err;
  EXPECT_EQ(max_build.targets.at("t").xfails.at("S.T").os_max_build,
            std::optional<uint32_t>(4294967295u));

  // Stale remove_xfails (not present in parent) fails at resolution time.
  const std::string stale_remove = R"({
    "schema_version": 1,
    "targets": {
      "p": {"abstract": true, "xfails": {"S.A": {"reason": "r"}}},
      "c": {"extends": "p", "remove_xfails": ["S.NotInParent"]}
    }
  })";
  BaselineManifest m;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(stale_remove, &m, &err)) << err;
  ResolvedBaseline resolved;
  EXPECT_FALSE(ResolveBaselineInMemory(m, "c", 26100, "x64", &resolved, &err));
  EXPECT_NE(err.find("S.NotInParent"), std::string::npos) << err;
}

TEST(BaselineTest, OsBuildAndArchPredicateMatching) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "S.Always": {"reason": "always"},
          "S.MinOnly": {"reason": "min", "os_min_build": 18362},
          "S.MaxOnly": {"reason": "max", "os_max_build": 17763},
          "S.Range": {"reason": "range", "os_min_build": 14393, "os_max_build": 17763},
          "S.X86Only": {"reason": "x86", "arch": ["x86"]},
          "S.X64AndArm": {"reason": "64bit", "arch": ["x64", "arm64"]}
        }
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;

  ResolvedBaseline s2016_x86;
  ASSERT_TRUE(
      ResolveBaselineInMemory(manifest, "t", 14393, "x86", &s2016_x86, &err));
  EXPECT_TRUE(s2016_x86.active_entries.contains("S.Always"));
  EXPECT_FALSE(s2016_x86.active_entries.contains("S.MinOnly"));
  EXPECT_TRUE(s2016_x86.active_entries.contains("S.MaxOnly"));
  EXPECT_TRUE(s2016_x86.active_entries.contains("S.Range"));
  EXPECT_TRUE(s2016_x86.active_entries.contains("S.X86Only"));
  EXPECT_FALSE(s2016_x86.active_entries.contains("S.X64AndArm"));

  ResolvedBaseline s2025_x64;
  ASSERT_TRUE(
      ResolveBaselineInMemory(manifest, "t", 26100, "x64", &s2025_x64, &err));
  EXPECT_TRUE(s2025_x64.active_entries.contains("S.Always"));
  EXPECT_TRUE(s2025_x64.active_entries.contains("S.MinOnly"));
  EXPECT_FALSE(s2025_x64.active_entries.contains("S.MaxOnly"));
  EXPECT_FALSE(s2025_x64.active_entries.contains("S.Range"));
  EXPECT_FALSE(s2025_x64.active_entries.contains("S.X86Only"));
  EXPECT_TRUE(s2025_x64.active_entries.contains("S.X64AndArm"));
}

TEST(BaselineTest, ValidatesActiveTestIdsAgainstAllRegisteredTests) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.Known": {"reason": "known"},
          "CreateOpenTest.Typo": {"reason": "typo"}
        }
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;

  ResolvedBaseline resolved;
  ASSERT_TRUE(
      ResolveBaselineInMemory(manifest, "t", 26100, "x64", &resolved, &err));

  const std::unordered_set<std::string> registered = {
      "CreateOpenTest.Known",
      "DeleteTest.Other",
  };

  std::vector<std::string> unknown;
  EXPECT_FALSE(ValidateBaselineTestIds(resolved, registered,
                                       /*allow_unknown=*/false, &unknown,
                                       &err));
  ASSERT_EQ(unknown.size(), 1u);
  EXPECT_EQ(unknown[0], "CreateOpenTest.Typo");
  EXPECT_NE(err.find("CreateOpenTest.Typo"), std::string::npos) << err;

  // With allow_unknown=true, returns true while still reporting unknown IDs.
  unknown.clear();
  EXPECT_TRUE(ValidateBaselineTestIds(resolved, registered,
                                      /*allow_unknown=*/true, &unknown, &err));
  ASSERT_EQ(unknown.size(), 1u);
  EXPECT_EQ(unknown[0], "CreateOpenTest.Typo");
}

constexpr const char *kCrossHostUpdateJson = R"({
    "schema_version": 1,
    "description": "Mirror baselines",
    "targets": {
      "mirror_base": {
        "abstract": true,
        "xfails": {
          "Suite.ParentStillFails": {"reason": "shared failure"},
          "Suite.ParentNowPassesOnChild": {"reason": "shared but child fixed"},
          "Suite.ParentWasRemovedNowFails": {"reason": "regressed on child"},
          "Suite.ParentHostSpecific": {
            "reason": "pinned to one host",
            "os_min_build": 26100,
            "os_max_build": 26100,
            "arch": ["x64"]
          }
        }
      },
      "mirror_unc": {
        "extends": "mirror_base",
        "xfails": {
          "Suite.ChildStillFails": {"reason": "human explanation kept"},
          "Suite.ChildNowPasses": {"reason": "fixed bug", "gap": "GAP-03"},
          "Suite.ChildMultiBuild": {
            "reason": "spans several builds",
            "os_min_build": 20000,
            "os_max_build": 26100
          },
          "Suite.ChildMultiArch": {
            "reason": "64-bit only",
            "arch": ["arm64", "x64"]
          },
          "Suite.ChildHostSpecific": {
            "reason": "pinned to one host",
            "os_min_build": 26100,
            "os_max_build": 26100,
            "arch": ["x64"]
          },
          "Suite.ChildOtherOs": {
            "reason": "Server 2016 only",
            "os_max_build": 14393
          },
          "Suite.ChildOtherArch": {
            "reason": "x86 only",
            "arch": ["x86"]
          },
          "Suite.ChildSkipped": {"reason": "skipped on this run"},
          "Suite.ChildFatalSetup": {"reason": "setup crash kept"},
          "Suite.ChildException": {"reason": "exception kept"},
          "Suite.ChildAllowXpass": {
            "reason": "intermittent race",
            "gap": "GAP-02",
            "allow_xpass": true
          }
        },
        "remove_xfails": ["Suite.ParentWasRemovedNowFails"]
      }
    }
  })";

std::vector<ObservedTestResult> CrossHostUpdateResults() {
  ObservedTestResult fatal_teardown =
      MakeObs("Suite.FatalTeardownFailure",
              /*passed=*/false, /*failed=*/true);
  fatal_teardown.fatal_teardown = true;
  ObservedTestResult fatal_setup = MakeObs("Suite.FatalSetupFailure",
                                           /*passed=*/false, /*failed=*/true);
  fatal_setup.fatal_setup = true;
  ObservedTestResult exception = MakeObs("Suite.ExceptionFailure",
                                         /*passed=*/false, /*failed=*/true);
  exception.exception = true;
  // Existing entries whose test hit a fatal setup failure / exception must be
  // preserved even though the raw outcome is not a plain failure.
  ObservedTestResult child_fatal_setup =
      MakeObs("Suite.ChildFatalSetup", /*passed=*/false, /*failed=*/true);
  child_fatal_setup.fatal_setup = true;
  ObservedTestResult child_exception =
      MakeObs("Suite.ChildException", /*passed=*/false, /*failed=*/true);
  child_exception.exception = true;

  return {
      MakeObs("Suite.ParentStillFails", false, true),
      MakeObs("Suite.ParentNowPassesOnChild", true, false),
      MakeObs("Suite.ParentWasRemovedNowFails", false, true),
      MakeObs("Suite.ParentHostSpecific", true, false),
      MakeObs("Suite.ChildStillFails", false, true),
      MakeObs("Suite.ChildNowPasses", true, false),
      MakeObs("Suite.ChildMultiBuild", true, false),
      MakeObs("Suite.ChildMultiArch", true, false),
      MakeObs("Suite.ChildHostSpecific", true, false),
      MakeObs("Suite.ChildOtherOs", true, false),
      MakeObs("Suite.ChildOtherArch", true, false),
      MakeObs("Suite.ChildSkipped", false, false, /*skipped=*/true),
      MakeObs("Suite.ChildAllowXpass", true, false),
      MakeObs("Suite.BrandNewFailure", false, true),
      fatal_teardown,
      fatal_setup,
      exception,
      child_fatal_setup,
      child_exception,
  };
}

TEST(BaselineTest,
     UpdateBaselinePreservesPredicatesSkipsReasonsAndParentDelta) {
  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(kCrossHostUpdateJson, &manifest, &err))
      << err;

  ResolvedBaseline resolved;
  ASSERT_TRUE(ResolveBaselineInMemory(manifest, "mirror_unc", 26100, "x64",
                                      &resolved, &err))
      << err;

  BaselineUpdateReport report;
  ASSERT_TRUE(UpdateBaselineManifest(&manifest, resolved, 26100, "x64",
                                     "TODO: triage", CrossHostUpdateResults(),
                                     BaselineUpdateOptions{}, &report, &err))
      << err;

  // Parent target must NEVER be mutated.
  const TargetDefinition &parent = manifest.targets.at("mirror_base");
  EXPECT_EQ(parent.xfails.size(), 4u);

  const TargetDefinition &child = manifest.targets.at("mirror_unc");

  // Inherited active failure stays out of child xfails.
  EXPECT_FALSE(child.xfails.contains("Suite.ParentStillFails"));
  // Previously removed parent failure that failed again is un-removed (not
  // duplicated in child xfails).
  EXPECT_FALSE(child.xfails.contains("Suite.ParentWasRemovedNowFails"));
  // Only the host-specific parent entry that passed is suppressed; the
  // unpredicated one may still be needed on other hosts, so it stays active.
  EXPECT_EQ(child.remove_xfails,
            std::vector<std::string>{"Suite.ParentHostSpecific"});

  // Existing human reason preserved.
  ASSERT_TRUE(child.xfails.contains("Suite.ChildStillFails"));
  EXPECT_EQ(child.xfails.at("Suite.ChildStillFails").reason,
            "human explanation kept");

  // Passing entries that still cover other hosts/builds are kept verbatim.
  ASSERT_TRUE(child.xfails.contains("Suite.ChildNowPasses"));
  EXPECT_EQ(child.xfails.at("Suite.ChildNowPasses").gap, "GAP-03");
  EXPECT_TRUE(child.xfails.contains("Suite.ChildMultiBuild"));
  // Multi-arch entry only drops this host's arch.
  ASSERT_TRUE(child.xfails.contains("Suite.ChildMultiArch"));
  EXPECT_EQ(child.xfails.at("Suite.ChildMultiArch").arch,
            std::vector<std::string>{"arm64"});
  // Entry pinned to exactly this arch + build is removed.
  EXPECT_FALSE(child.xfails.contains("Suite.ChildHostSpecific"));

  // Unmatched OS / arch / skipped / allow_xpass / fatal-setup / exception
  // entries preserved.
  EXPECT_TRUE(child.xfails.contains("Suite.ChildOtherOs"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildOtherArch"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildSkipped"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildAllowXpass"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildFatalSetup"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildException"));

  // Brand-new failure added with default reason and no gap.
  ASSERT_TRUE(child.xfails.contains("Suite.BrandNewFailure"));
  EXPECT_EQ(child.xfails.at("Suite.BrandNewFailure").reason, "TODO: triage");
  EXPECT_TRUE(child.xfails.at("Suite.BrandNewFailure").gap.empty());

  // Fatal setup / teardown failures and exceptions must NEVER be added.
  EXPECT_FALSE(child.xfails.contains("Suite.FatalTeardownFailure"));
  EXPECT_FALSE(child.xfails.contains("Suite.FatalSetupFailure"));
  EXPECT_FALSE(child.xfails.contains("Suite.ExceptionFailure"));

  // Every touched ID is reported (sorted by ID).
  EXPECT_EQ(report.added,
            (std::vector<std::string>{"Suite.BrandNewFailure",
                                      "Suite.ParentWasRemovedNowFails"}));
  EXPECT_TRUE(report.widened.empty());
  EXPECT_EQ(report.narrowed, std::vector<std::string>{"Suite.ChildMultiArch"});
  EXPECT_EQ(report.removed,
            (std::vector<std::string>{"Suite.ChildHostSpecific",
                                      "Suite.ParentHostSpecific"}));
  EXPECT_EQ(
      report.passed_kept,
      (std::vector<std::string>{"Suite.ChildMultiBuild", "Suite.ChildNowPasses",
                                "Suite.ParentNowPassesOnChild"}));

  const std::string log =
      FormatBaselineUpdateReport(report, /*prune_passing=*/false);
  EXPECT_NE(log.find("removed:  Suite.ChildHostSpecific"), std::string::npos)
      << log;
  EXPECT_NE(log.find("narrowed: Suite.ChildMultiArch"), std::string::npos)
      << log;
  EXPECT_NE(log.find("added:    Suite.BrandNewFailure"), std::string::npos)
      << log;
  EXPECT_NE(log.find("passed on this host"), std::string::npos) << log;
  EXPECT_NE(log.find("--prune-passing"), std::string::npos) << log;
  EXPECT_NE(log.find("Suite.ParentNowPassesOnChild"), std::string::npos) << log;

  // Deterministic serialization round-trip.
  const std::string serialized = SerializeBaselineManifest(manifest);
  BaselineManifest reparsed;
  ASSERT_TRUE(ParseBaselineManifest(serialized, &reparsed, &err)) << err;
  EXPECT_EQ(SerializeBaselineManifest(reparsed), serialized);
}

TEST(BaselineTest, UpdateBaselinePrunePassingRemovesMultiHostEntries) {
  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(kCrossHostUpdateJson, &manifest, &err))
      << err;

  ResolvedBaseline resolved;
  ASSERT_TRUE(ResolveBaselineInMemory(manifest, "mirror_unc", 26100, "x64",
                                      &resolved, &err))
      << err;

  BaselineUpdateOptions options;
  options.prune_passing = true;
  BaselineUpdateReport report;
  ASSERT_TRUE(UpdateBaselineManifest(&manifest, resolved, 26100, "x64",
                                     "TODO: triage", CrossHostUpdateResults(),
                                     options, &report, &err))
      << err;

  const TargetDefinition &child = manifest.targets.at("mirror_unc");
  EXPECT_FALSE(child.xfails.contains("Suite.ChildNowPasses"));
  EXPECT_FALSE(child.xfails.contains("Suite.ChildMultiBuild"));
  EXPECT_FALSE(child.xfails.contains("Suite.ChildHostSpecific"));
  // Multi-arch lists are still narrowed rather than deleted.
  ASSERT_TRUE(child.xfails.contains("Suite.ChildMultiArch"));
  EXPECT_EQ(child.xfails.at("Suite.ChildMultiArch").arch,
            std::vector<std::string>{"arm64"});
  // Other-host, allow_xpass, skipped, fatal-setup and exception entries are
  // still untouched.
  EXPECT_TRUE(child.xfails.contains("Suite.ChildOtherOs"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildOtherArch"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildAllowXpass"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildSkipped"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildFatalSetup"));
  EXPECT_TRUE(child.xfails.contains("Suite.ChildException"));
  // Unpredicated parent entry is now suppressed too.
  EXPECT_EQ(child.remove_xfails,
            (std::vector<std::string>{"Suite.ParentHostSpecific",
                                      "Suite.ParentNowPassesOnChild"}));

  EXPECT_TRUE(report.passed_kept.empty());
  EXPECT_EQ(report.removed,
            (std::vector<std::string>{
                "Suite.ChildHostSpecific", "Suite.ChildMultiBuild",
                "Suite.ChildNowPasses", "Suite.ParentHostSpecific",
                "Suite.ParentNowPassesOnChild"}));
  EXPECT_EQ(report.narrowed, std::vector<std::string>{"Suite.ChildMultiArch"});

  const std::string log =
      FormatBaselineUpdateReport(report, /*prune_passing=*/true);
  EXPECT_EQ(log.find("passed on this host"), std::string::npos) << log;
  EXPECT_NE(log.find("removed:  Suite.ParentNowPassesOnChild"),
            std::string::npos)
      << log;

  // A null report is accepted.
  BaselineManifest again;
  ASSERT_TRUE(ParseBaselineManifest(kCrossHostUpdateJson, &again, &err)) << err;
  EXPECT_TRUE(UpdateBaselineManifest(&again, resolved, 26100, "x64", "TODO",
                                     CrossHostUpdateResults(), options,
                                     /*report=*/nullptr, &err))
      << err;
}

TEST(BaselineTest, SkipCrashParsingPreservationAndGtestFilterHelpers) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "memfs_drive": {
        "xfails": {
          "NamingPathsTest.MountRootOperations": {
            "reason": "memfs infinite recursion moving root",
            "gap": "GAP-04",
            "skip_crash": true
          },
          "CreateOpenTest.RegularGap": {
            "reason": "regular xfail"
          }
        }
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;

  ResolvedBaseline resolved;
  ASSERT_TRUE(ResolveBaselineInMemory(manifest, "memfs_drive", 19045, "x64",
                                      &resolved, &err))
      << err;
  ASSERT_TRUE(
      resolved.active_entries.contains("NamingPathsTest.MountRootOperations"));
  EXPECT_TRUE(resolved.active_entries.at("NamingPathsTest.MountRootOperations")
                  .skip_crash);
  EXPECT_FALSE(
      resolved.active_entries.at("CreateOpenTest.RegularGap").skip_crash);

  // --update-baseline must preserve skip_crash entries untouched.
  ObservedTestResult crash_obs;
  crash_obs.test_id = "NamingPathsTest.MountRootOperations";
  crash_obs.failed = true;
  crash_obs.skip_crash = true;

  ObservedTestResult pass_obs;
  pass_obs.test_id = "CreateOpenTest.RegularGap";
  pass_obs.passed = true;

  BaselineUpdateReport report;
  ASSERT_TRUE(UpdateBaselineManifest(&manifest, resolved, 19045, "x64", "TODO",
                                     {crash_obs, pass_obs},
                                     BaselineUpdateOptions{}, &report, &err))
      << err;
  const TargetDefinition &updated = manifest.targets.at("memfs_drive");
  ASSERT_TRUE(updated.xfails.contains("NamingPathsTest.MountRootOperations"));
  EXPECT_TRUE(
      updated.xfails.at("NamingPathsTest.MountRootOperations").skip_crash);
  EXPECT_EQ(updated.xfails.at("NamingPathsTest.MountRootOperations").gap,
            "GAP-04");
  // The unpredicated entry may still be needed on other hosts: kept and
  // reported rather than deleted.
  EXPECT_TRUE(updated.xfails.contains("CreateOpenTest.RegularGap"));
  EXPECT_EQ(report.passed_kept,
            std::vector<std::string>{"CreateOpenTest.RegularGap"});
  EXPECT_TRUE(report.removed.empty());

  // Deterministic serialization retains skip_crash and gap.
  const std::string serialized = SerializeBaselineManifest(manifest);
  EXPECT_NE(serialized.find("\"skip_crash\": true"), std::string::npos);
  EXPECT_NE(serialized.find("\"gap\": \"GAP-04\""), std::string::npos);

  // GTest filter matching and negative-clause appending. An empty filter
  // selects nothing (GTest semantics) and is never rewritten.
  EXPECT_FALSE(MatchesGtestFilter("", "NamingPathsTest.MountRootOperations"));
  EXPECT_TRUE(MatchesGtestFilter("*", "NamingPathsTest.MountRootOperations"));
  EXPECT_TRUE(MatchesGtestFilter("NamingPathsTest.*",
                                 "NamingPathsTest.MountRootOperations"));
  EXPECT_FALSE(MatchesGtestFilter("StreamsTest.*",
                                  "NamingPathsTest.MountRootOperations"));
  EXPECT_FALSE(MatchesGtestFilter("*-NamingPathsTest.MountRootOperations",
                                  "NamingPathsTest.MountRootOperations"));
  EXPECT_TRUE(MatchesGtestFilter("-StreamsTest.*",
                                 "NamingPathsTest.MountRootOperations"));
  EXPECT_TRUE(MatchesGtestFilter("A.B?:C.*", "A.B1"));
  EXPECT_FALSE(MatchesGtestFilter("A.B?:C.*", "A.B12"));

  EXPECT_EQ(AppendNegativeGtestFilter("", {"A.B", "C.D"}), "");
  EXPECT_EQ(AppendNegativeGtestFilter("*", {"A.B"}), "*-A.B");
  EXPECT_EQ(AppendNegativeGtestFilter("*-X.Y", {"A.B", "C.D"}),
            "*-X.Y:A.B:C.D");
  EXPECT_EQ(AppendNegativeGtestFilter("-X.Y:", {"A.B"}), "-X.Y:A.B");
}

TEST(BaselineTest, ResolveFromLoadedManifestAvoidsReReadingRootFile) {
  ScopedTestDirectory dir;
  dir.WriteFile("parent.json", R"({
    "schema_version": 1,
    "targets": {
      "base": {
        "abstract": true,
        "xfails": {
          "CreateOpenTest.ParentGap": {"reason": "from parent"}
        }
      }
    }
  })");

  const std::string overlay_path = dir.WriteFile("overlay.json", R"({
    "schema_version": 1,
    "targets": {
      "child": {
        "extends_file": "parent.json",
        "extends": "base",
        "xfails": {
          "DeleteTest.ChildGap": {"reason": "from child"}
        }
      }
    }
  })");

  BaselineManifest loaded;
  std::string err;
  ASSERT_TRUE(LoadBaselineManifestFile(overlay_path, &loaded, &err)) << err;

  // Delete overlay.json on disk after loading so any redundant disk read of
  // the root manifest would fail immediately, while parent.json remains
  // reachable via manifest_path's parent directory.
  std::error_code ec;
  const std::filesystem::path overlay_fs(
      std::u8string(reinterpret_cast<const char8_t *>(overlay_path.data()),
                    overlay_path.size()));
  ASSERT_TRUE(std::filesystem::remove(overlay_fs, ec)) << ec.message();

  ResolvedBaseline resolved;
  ASSERT_TRUE(ResolveBaselineFromLoadedManifest(loaded, overlay_path, "child",
                                                26100, "x64", &resolved, &err))
      << err;
  EXPECT_EQ(resolved.manifest_path, overlay_path);
  EXPECT_EQ(resolved.target_name, "child");
  EXPECT_TRUE(resolved.active_entries.contains("CreateOpenTest.ParentGap"));
  EXPECT_TRUE(resolved.active_entries.contains("DeleteTest.ChildGap"));
}

TEST(BaselineTest, ValidatesInactiveOsAndArchEntriesAgainstRegisteredTests) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.Known": {"reason": "known"},
          "CreateOpenTest.FutureTypo": {
            "reason": "only on future build",
            "os_min_build": 99999
          },
          "DeleteTest.LegacyTypo": {
            "reason": "only on old build",
            "os_max_build": 1000
          },
          "StreamsTest.ArmTypo": {
            "reason": "only on arm64",
            "arch": ["arm64"]
          }
        }
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;

  // On build 26100 x64, only CreateOpenTest.Known is active, yet all three
  // inactive typos must still be caught by ValidateBaselineTestIds.
  ResolvedBaseline resolved;
  ASSERT_TRUE(
      ResolveBaselineInMemory(manifest, "t", 26100, "x64", &resolved, &err))
      << err;
  EXPECT_EQ(resolved.active_entries.size(), 1u);
  EXPECT_EQ(resolved.all_entries.size(), 4u);

  const std::unordered_set<std::string> registered = {"CreateOpenTest.Known"};
  std::vector<std::string> unknown;
  EXPECT_FALSE(ValidateBaselineTestIds(resolved, registered,
                                       /*allow_unknown=*/false, &unknown,
                                       &err));
  const std::vector<std::string> expected_unknown = {
      "CreateOpenTest.FutureTypo",
      "DeleteTest.LegacyTypo",
      "StreamsTest.ArmTypo",
  };
  EXPECT_EQ(unknown, expected_unknown);
}

TEST(BaselineTest,
     UpdateBaselineWidensPredicatesAndSuppressesOverriddenParent) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "parent": {
        "abstract": true,
        "xfails": {
          "Suite.OverriddenFixed": {"reason": "parent gap"}
        }
      },
      "child": {
        "extends": "parent",
        "xfails": {
          "Suite.OverriddenFixed": {
            "reason": "child custom reason",
            "os_min_build": 17763,
            "os_max_build": 17763,
            "arch": ["x64"]
          },
          "Suite.WidenMe": {
            "reason": "initially narrow",
            "gap": "GAP-07",
            "os_min_build": 20000,
            "os_max_build": 22000,
            "arch": ["x86"]
          }
        }
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;

  // 1. Failing below os_min_build and on a new arch widens both bounds/arch,
  // while passing a host-specific override of a parent xfail removes it from
  // child xfails AND adds it to remove_xfails so the parent entry does not
  // re-activate.
  ResolvedBaseline resolved1;
  ASSERT_TRUE(ResolveBaselineInMemory(manifest, "child", 17763, "x64",
                                      &resolved1, &err))
      << err;

  ObservedTestResult fixed_obs;
  fixed_obs.test_id = "Suite.OverriddenFixed";
  fixed_obs.passed = true;

  ObservedTestResult widen_low_obs;
  widen_low_obs.test_id = "Suite.WidenMe";
  widen_low_obs.failed = true;

  BaselineUpdateReport report1;
  ASSERT_TRUE(UpdateBaselineManifest(&manifest, resolved1, 17763, "x64", "TODO",
                                     {fixed_obs, widen_low_obs},
                                     BaselineUpdateOptions{}, &report1, &err))
      << err;
  EXPECT_EQ(report1.widened, std::vector<std::string>{"Suite.WidenMe"});
  EXPECT_EQ(report1.removed, std::vector<std::string>{"Suite.OverriddenFixed"});

  const TargetDefinition &child1 = manifest.targets.at("child");
  EXPECT_FALSE(child1.xfails.contains("Suite.OverriddenFixed"));
  ASSERT_EQ(child1.remove_xfails.size(), 1u);
  EXPECT_EQ(child1.remove_xfails[0], "Suite.OverriddenFixed");

  ASSERT_TRUE(child1.xfails.contains("Suite.WidenMe"));
  const XfailEntry &widened1 = child1.xfails.at("Suite.WidenMe");
  ASSERT_TRUE(widened1.os_min_build.has_value());
  ASSERT_TRUE(widened1.os_max_build.has_value());
  EXPECT_EQ(*widened1.os_min_build, 17763u);
  EXPECT_EQ(*widened1.os_max_build, 22000u);
  EXPECT_EQ(widened1.arch, (std::vector<std::string>{"x64", "x86"}));
  EXPECT_EQ(widened1.reason, "initially narrow");
  EXPECT_EQ(widened1.gap, "GAP-07");

  // 2. Failing above os_max_build on arm64 widens os_max_build and arch list.
  ResolvedBaseline resolved2;
  ASSERT_TRUE(ResolveBaselineInMemory(manifest, "child", 26100, "arm64",
                                      &resolved2, &err))
      << err;
  EXPECT_FALSE(resolved2.all_entries.contains("Suite.OverriddenFixed"));

  ASSERT_TRUE(UpdateBaselineManifest(&manifest, resolved2, 26100, "arm64",
                                     "TODO", {widen_low_obs},
                                     BaselineUpdateOptions{},
                                     /*report=*/nullptr, &err))
      << err;

  const XfailEntry &widened2 =
      manifest.targets.at("child").xfails.at("Suite.WidenMe");
  ASSERT_TRUE(widened2.os_min_build.has_value());
  ASSERT_TRUE(widened2.os_max_build.has_value());
  EXPECT_EQ(*widened2.os_min_build, 17763u);
  EXPECT_EQ(*widened2.os_max_build, 26100u);
  EXPECT_EQ(widened2.arch, (std::vector<std::string>{"arm64", "x64", "x86"}));
}

TEST(BaselineTest, WriteTextFileUtf8AtomicallyReplacesWithoutTempDebris) {
  ScopedTestDirectory dir;
  const std::string path = dir.WriteFile("atomic.json", "initial_payload\n");

  std::string err;
  ASSERT_TRUE(WriteTextFileUtf8(path, "updated_payload\n", &err)) << err;

  std::string read_back;
  ASSERT_TRUE(ReadTextFileUtf8(path, &read_back, &err)) << err;
  EXPECT_EQ(read_back, "updated_payload\n");

  auto entries = core::ListDirectoryWin32(dir.Path());
  ASSERT_TRUE(entries.ok()) << core::FormatWin32Error(entries.error);
  int file_count = 0;
  for (const std::wstring &name : *entries.value) {
    if (name == L"." || name == L"..") {
      continue;
    }
    ++file_count;
    EXPECT_TRUE(core::EqualsIgnoreCase(name, L"atomic.json"))
        << "Unexpected leftover file: " << core::WideToUtf8(name);
  }
  EXPECT_EQ(file_count, 1);
}

TEST(BaselineTest, WriteTextFileUtf8RenameFailureLeavesNoTempDebris) {
  ScopedTestDirectory dir;

  // Destination is an existing directory: MoveFileExW cannot replace it.
  const std::string dir_dest = dir.ChildPathUtf8("dest_is_dir.json");
  ASSERT_NE(::CreateDirectoryW(core::Utf8ToWide(dir_dest).c_str(), nullptr),
            FALSE)
      << core::FormatWin32Error(::GetLastError());
  std::string err;
  EXPECT_FALSE(WriteTextFileUtf8(dir_dest, "payload\n", &err));
  EXPECT_NE(err.find("Failed to atomically replace"), std::string::npos) << err;
  EXPECT_EQ(ListChildren(dir.Path()),
            std::vector<std::wstring>{L"dest_is_dir.json"});

  // Destination held open without FILE_SHARE_DELETE: the replace fails and
  // the original content survives.
  const std::string locked = dir.WriteFile("locked.json", "original\n");
  {
    core::UniqueHandle lock(
        ::CreateFileW(core::Utf8ToWide(locked).c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(lock.valid()) << core::FormatWin32Error(::GetLastError());
    err.clear();
    EXPECT_FALSE(WriteTextFileUtf8(locked, "replacement\n", &err));
    EXPECT_NE(err.find("Failed to atomically replace"), std::string::npos)
        << err;
  }
  std::string read_back;
  ASSERT_TRUE(ReadTextFileUtf8(locked, &read_back, &err)) << err;
  EXPECT_EQ(read_back, "original\n");

  std::vector<std::wstring> names = ListChildren(dir.Path());
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names,
            (std::vector<std::wstring>{L"dest_is_dir.json", L"locked.json"}));
}

TEST(BaselineTest, GapFieldParsesValidatesAndRoundTrips) {
  const std::string json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "S.Crash": {"reason": "crashes", "gap": "GAP-123", "skip_crash": true},
          "S.Flaky": {"reason": "race", "gap": "GAP-05", "allow_xpass": true},
          "S.Full": {
            "arch": ["x86", "x64"],
            "os_max_build": 26100,
            "os_min_build": 17763,
            "gap": "GAP-01",
            "reason": "every field"
          },
          "S.NoGap": {"reason": "plain entry", "skip_crash": false}
        }
      }
    }
  })";

  BaselineManifest manifest;
  std::string err;
  ASSERT_TRUE(ParseBaselineManifest(json, &manifest, &err)) << err;
  const auto &xfails = manifest.targets.at("t").xfails;
  EXPECT_EQ(xfails.at("S.Crash").gap, "GAP-123");
  EXPECT_EQ(xfails.at("S.Flaky").gap, "GAP-05");
  EXPECT_EQ(xfails.at("S.Full").gap, "GAP-01");
  EXPECT_TRUE(xfails.at("S.NoGap").gap.empty());

  // Serialization order: reason, gap, os_min_build, os_max_build, arch,
  // allow_xpass, skip_crash; absent gap is omitted.
  const std::string serialized = SerializeBaselineManifest(manifest);
  const std::size_t full = serialized.find("\"S.Full\"");
  ASSERT_NE(full, std::string::npos) << serialized;
  std::size_t prev = full;
  for (const char *key : {"\"reason\"", "\"gap\"", "\"os_min_build\"",
                          "\"os_max_build\"", "\"arch\""}) {
    const std::size_t pos = serialized.find(key, full);
    ASSERT_NE(pos, std::string::npos) << key << "\n" << serialized;
    EXPECT_GT(pos, prev) << key << "\n" << serialized;
    prev = pos;
  }
  const std::size_t crash = serialized.find("\"S.Crash\"");
  ASSERT_NE(crash, std::string::npos);
  EXPECT_LT(serialized.find("\"reason\"", crash),
            serialized.find("\"gap\": \"GAP-123\"", crash));
  EXPECT_LT(serialized.find("\"gap\": \"GAP-123\"", crash),
            serialized.find("\"skip_crash\": true", crash));
  const std::size_t no_gap = serialized.find("\"S.NoGap\"");
  ASSERT_NE(no_gap, std::string::npos);
  EXPECT_EQ(serialized.find("\"gap\"", no_gap), std::string::npos)
      << serialized;

  BaselineManifest reparsed;
  ASSERT_TRUE(ParseBaselineManifest(serialized, &reparsed, &err)) << err;
  EXPECT_EQ(SerializeBaselineManifest(reparsed), serialized);
  EXPECT_EQ(reparsed.targets.at("t").xfails.at("S.Crash").gap, "GAP-123");

  // Invalid "gap" values (wrong type, prefix, case, digit count, non-ASCII
  // digits, surrounding text).
  const std::string kGapError =
      R"(Field "gap" in xfail entry "S.T" of target "t" must be)";
  for (const std::string &bad_gap :
       {std::string(R"("GAP-1")"), std::string(R"("GAP-")"),
        std::string(R"("gap-01")"), std::string(R"("GAP-01a")"),
        std::string(R"(" GAP-01")"), std::string(R"("GAP-01 ")"),
        std::string(R"("GAP01")"), std::string(R"("")"),
        std::string("\"GAP-\xd9\xa0\xd9\xa1\""), std::string("1"),
        std::string("null"), std::string(R"(["GAP-01"])")}) {
    ExpectParseError(
        ManifestWithEntry(R"({"reason": "r", "gap": )" + bad_gap + "}"),
        kGapError);
  }

  // "gap" is required for skip_crash and allow_xpass entries.
  ExpectParseError(
      ManifestWithEntry(R"({"reason": "r", "skip_crash": true})"),
      R"(Xfail entry "S.T" of target "t" sets "skip_crash" and must cite a CONFORMANCE_GAPS.md "gap")");
  ExpectParseError(
      ManifestWithEntry(R"({"reason": "r", "allow_xpass": true})"),
      R"(Xfail entry "S.T" of target "t" sets "allow_xpass" and must cite a CONFORMANCE_GAPS.md "gap")");
}

TEST(BaselineTest, RebaseExtendsFilePathsKeepsParentsReachable) {
  ScopedTestDirectory dir;
  dir.WriteFile("parents\\base.json", R"({
    "schema_version": 1,
    "targets": {
      "base": {
        "abstract": true,
        "xfails": {"S.Parent": {"reason": "from parent"}}
      }
    }
  })");
  const std::string overlay = dir.WriteFile("overlay.json", R"({
    "schema_version": 1,
    "targets": {
      "child": {
        "extends_file": "parents/base.json",
        "extends": "base",
        "xfails": {"S.Child": {"reason": "from child"}}
      }
    }
  })");

  BaselineManifest original;
  std::string err;
  ASSERT_TRUE(LoadBaselineManifestFile(overlay, &original, &err)) << err;
  const auto extends_file_of = [](const BaselineManifest &m) {
    return m.targets.at("child").extends_file;
  };

  // Same directory: no-op.
  BaselineManifest same = original;
  ASSERT_TRUE(RebaseExtendsFilePaths(&same, overlay,
                                     dir.ChildPathUtf8("renamed.json"), &err))
      << err;
  EXPECT_EQ(extends_file_of(same), "parents/base.json");

  // Deeper directory: rewritten so the written manifest still resolves.
  const std::string nested = dir.ChildPathUtf8("out\\nested\\overlay.json");
  BaselineManifest deeper = original;
  ASSERT_TRUE(RebaseExtendsFilePaths(&deeper, overlay, nested, &err)) << err;
  EXPECT_EQ(extends_file_of(deeper), "../../parents/base.json");
  ASSERT_TRUE(
      WriteTextFileUtf8(nested, SerializeBaselineManifest(deeper), &err))
      << err;
  ResolvedBaseline resolved;
  ASSERT_TRUE(
      ResolveBaselineFromFile(nested, "child", 26100, "x64", &resolved, &err))
      << err;
  EXPECT_TRUE(resolved.active_entries.contains("S.Parent"));
  EXPECT_TRUE(resolved.active_entries.contains("S.Child"));

  // Rebasing back up into the parent's own directory.
  BaselineManifest sibling = deeper;
  ASSERT_TRUE(RebaseExtendsFilePaths(
      &sibling, nested, dir.ChildPathUtf8("parents\\overlay.json"), &err))
      << err;
  EXPECT_EQ(extends_file_of(sibling), "base.json");

  // Absolute extends_file values are left untouched.
  BaselineManifest absolute = original;
  const std::string base_abs = dir.ChildPathUtf8("parents\\base.json");
  absolute.targets.at("child").extends_file = base_abs;
  ASSERT_TRUE(RebaseExtendsFilePaths(&absolute, overlay, nested, &err)) << err;
  EXPECT_EQ(extends_file_of(absolute), base_abs);

  // Drive-letter case differences still produce a relative path.
  BaselineManifest drive_case = original;
  ASSERT_TRUE(RebaseExtendsFilePaths(&drive_case, "c:\\bl\\x\\overlay.json",
                                     "C:\\bl\\y\\overlay.json", &err))
      << err;
  EXPECT_EQ(extends_file_of(drive_case), "../x/parents/base.json");

  // Same UNC share: relative path across directories.
  BaselineManifest same_share = original;
  ASSERT_TRUE(
      RebaseExtendsFilePaths(&same_share, "\\\\srv\\share\\a\\overlay.json",
                             "\\\\srv\\share\\b\\c\\overlay.json", &err))
      << err;
  EXPECT_EQ(extends_file_of(same_share), "../../a/parents/base.json");

  // Different drive or UNC share: no relative path exists -> clear error and
  // the manifest is left untouched.
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"C:\\bl\\overlay.json", "Z:\\bl\\overlay.json"},
           {"\\\\srv\\share1\\overlay.json", "\\\\srv\\share2\\overlay.json"},
           {"C:\\bl\\overlay.json", "\\\\srv\\share\\overlay.json"}}) {
    BaselineManifest unreachable = original;
    err.clear();
    EXPECT_FALSE(RebaseExtendsFilePaths(&unreachable, from, to, &err))
        << from << " -> " << to;
    EXPECT_NE(
        err.find(
            R"(Cannot rebase "extends_file" ("parents/base.json") of target "child")"),
        std::string::npos)
        << err;
    EXPECT_NE(err.find("relative path"), std::string::npos) << err;
    EXPECT_EQ(extends_file_of(unreachable), "parents/base.json");
  }
}

} // namespace
} // namespace fstest::cli
