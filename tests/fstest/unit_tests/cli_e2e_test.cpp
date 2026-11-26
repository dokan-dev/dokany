// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "baseline.h"
#include "cli_args.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"
#include "fstest/core/win_headers.h"
#include "fstest/suite/test_metadata.h"
#include "json_util.h"
#include "test_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::cli {
namespace {

constexpr DWORD kSubprocessTimeoutMs = 60000;
constexpr DWORD kFullSuiteTimeoutMs = 180000;

// Fast, filesystem-agnostic suite tests used by the end-to-end cases (listed in
// registration order).
constexpr std::string_view kTestCreateNew =
    "CreateOpenTest.CreateNewSucceedsAndRejectsExisting";
constexpr std::string_view kTestCreateAlways =
    "CreateOpenTest.CreateAlwaysSetsAlreadyExistsLastError";
constexpr std::string_view kTestOpenExisting =
    "CreateOpenTest.OpenExistingNotFoundAndExisting";
constexpr std::string_view kTestMissingParent =
    "CreateOpenTest.CreateInMissingParentReturnsPathNotFound";

std::wstring W(std::string_view utf8) {
  return core::Utf8ToWide(std::string(utf8));
}

using test_support::GetHostTempDir;

std::wstring GetCurrentExecutablePath() {
  std::wstring buf(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 8; ++attempt) {
    ::SetLastError(ERROR_SUCCESS);
    const DWORD len = ::GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
    if (len == 0) {
      return {};
    }
    if (len < buf.size() && ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      buf.resize(len);
      return buf;
    }
    buf.resize(buf.size() * 2);
  }
  return {};
}

std::wstring FindSiblingCliExecutable() {
  const std::wstring self_path = GetCurrentExecutablePath();
  if (self_path.empty()) {
    return {};
  }
  const size_t sep = self_path.find_last_of(L"\\/");
  if (sep == std::wstring::npos) {
    return {};
  }
  const std::wstring candidate =
      self_path.substr(0, sep + 1) + L"dokan_fstest.exe";
  const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES ||
      (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return {};
  }
  return candidate;
}

std::wstring FindCommittedBaselinePath(const std::wstring &exe_path,
                                       std::wstring_view filename) {
  auto is_regular_file = [](const std::wstring &p) {
    const DWORD attrs = ::GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
  };
#ifdef FSTEST_SOURCE_DIR
  {
    const std::wstring src_dir =
        core::NormalizeWin32Path(core::Utf8ToWide(FSTEST_SOURCE_DIR));
    const std::wstring candidate =
        core::JoinPath(core::JoinPath(src_dir, L"baselines"), filename);
    if (is_regular_file(candidate)) {
      return candidate;
    }
  }
#endif
  std::wstring cur = exe_path;
  for (int depth = 0; depth < 8; ++depth) {
    const size_t sep = cur.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
      break;
    }
    cur.resize(sep);
    const std::wstring direct =
        core::JoinPath(core::JoinPath(cur, L"baselines"), filename);
    if (is_regular_file(direct)) {
      return direct;
    }
    const std::wstring nested = core::JoinPath(
        core::JoinPath(cur, L"tests\\fstest\\baselines"), filename);
    if (is_regular_file(nested)) {
      return nested;
    }
  }
  return {};
}

// Quotes an argument following Windows CRT CommandLineToArgvW rules, including
// doubling trailing backslashes before a closing quote.
std::wstring QuoteWindowsArg(std::wstring_view arg) {
  const bool needs_quotes =
      arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) {
    return std::wstring(arg);
  }
  std::wstring out;
  out.push_back(L'"');
  for (size_t i = 0; i < arg.size();) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      ++i;
    } else {
      out.append(backslashes, L'\\');
      out.push_back(arg[i]);
      ++i;
    }
  }
  out.push_back(L'"');
  return out;
}

std::wstring BuildCommandLine(const std::wstring &exe_path,
                              const std::vector<std::wstring> &args) {
  std::wstring cmd = QuoteWindowsArg(exe_path);
  for (const std::wstring &arg : args) {
    cmd.push_back(L' ');
    cmd += QuoteWindowsArg(arg);
  }
  return cmd;
}

class UniqueEnvStrings {
public:
  explicit UniqueEnvStrings(LPWCH ptr) noexcept : ptr_(ptr) {}
  ~UniqueEnvStrings() {
    if (ptr_ != nullptr) {
      ::FreeEnvironmentStringsW(ptr_);
    }
  }

  UniqueEnvStrings(const UniqueEnvStrings &) = delete;
  UniqueEnvStrings &operator=(const UniqueEnvStrings &) = delete;

  LPWCH get() const noexcept { return ptr_; }

private:
  LPWCH ptr_ = nullptr;
};

using EnvList = std::vector<std::pair<std::wstring, std::wstring>>;

bool ShouldSkipEnvKey(std::wstring_view key,
                      const std::vector<std::wstring> &strip_keys,
                      const EnvList &overrides) {
  for (const std::wstring &s : strip_keys) {
    if (core::EqualsIgnoreCase(key, s)) {
      return true;
    }
  }
  for (const auto &ov : overrides) {
    if (core::EqualsIgnoreCase(key, ov.first)) {
      return true;
    }
  }
  return false;
}

std::vector<wchar_t>
BuildChildEnvironmentBlock(const EnvList &overrides,
                           const std::vector<std::wstring> &strip_keys) {
  EnvList entries;
  UniqueEnvStrings raw(::GetEnvironmentStringsW());
  if (raw.get() != nullptr) {
    const wchar_t *p = raw.get();
    while (*p != L'\0') {
      const std::wstring_view item(p);
      p += item.size() + 1;
      // Preserve special drive-cwd entries starting with '=' (e.g. "=C:=...").
      const size_t eq = item.find(L'=', 1);
      if (eq == std::wstring_view::npos) {
        continue;
      }
      const std::wstring_view key = item.substr(0, eq);
      const std::wstring_view val = item.substr(eq + 1);
      if (!ShouldSkipEnvKey(key, strip_keys, overrides)) {
        entries.emplace_back(std::wstring(key), std::wstring(val));
      }
    }
  }
  for (const auto &ov : overrides) {
    entries.push_back(ov);
  }
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return ::CompareStringOrdinal(
               a.first.data(), static_cast<int>(a.first.size()), b.first.data(),
               static_cast<int>(b.first.size()),
               /*bIgnoreCase=*/TRUE) == CSTR_LESS_THAN;
  });

  std::vector<wchar_t> block;
  for (const auto &entry : entries) {
    block.insert(block.end(), entry.first.begin(), entry.first.end());
    block.push_back(L'=');
    block.insert(block.end(), entry.second.begin(), entry.second.end());
    block.push_back(L'\0');
  }
  if (block.empty()) {
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

struct SubprocessResult {
  bool launched = false;
  bool timed_out = false;
  DWORD launch_error = ERROR_SUCCESS;
  DWORD exit_code = 0;
  std::string stdout_text;
  std::string stderr_text;
};

struct RedirectedStdHandles {
  core::UniqueHandle stdin_h;
  core::UniqueHandle stdout_h;
  core::UniqueHandle stderr_h;
  DWORD error = ERROR_SUCCESS;

  bool ok() const noexcept {
    return error == ERROR_SUCCESS && stdin_h && stdout_h && stderr_h;
  }
  void CloseAll() noexcept {
    stdin_h.reset();
    stdout_h.reset();
    stderr_h.reset();
  }
};

RedirectedStdHandles OpenRedirectedStdHandles(const std::wstring &stdout_path,
                                              const std::wstring &stderr_path) {
  RedirectedStdHandles handles{};
  SECURITY_ATTRIBUTES inheritable_sa{};
  inheritable_sa.nLength = sizeof(inheritable_sa);
  inheritable_sa.bInheritHandle = TRUE;

  auto in_res = core::CreateWin32File(
      L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, &inheritable_sa);
  if (!in_res) {
    handles.error = in_res.error;
    return handles;
  }
  auto out_res = core::CreateWin32File(
      stdout_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, &inheritable_sa);
  if (!out_res) {
    handles.error = out_res.error;
    return handles;
  }
  auto err_res = core::CreateWin32File(
      stderr_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, &inheritable_sa);
  if (!err_res) {
    handles.error = err_res.error;
    return handles;
  }
  handles.stdin_h = std::move(*in_res.value);
  handles.stdout_h = std::move(*out_res.value);
  handles.stderr_h = std::move(*err_res.value);
  return handles;
}

SubprocessResult RunCliSubprocess(const std::wstring &exe_path,
                                  const std::vector<std::wstring> &args,
                                  const EnvList &env_overrides = {},
                                  DWORD timeout_ms = kSubprocessTimeoutMs) {
  SubprocessResult res{};
  const std::wstring host_tmp = GetHostTempDir();
  if (host_tmp.empty()) {
    res.launch_error = ERROR_PATH_NOT_FOUND;
    return res;
  }

  auto io_dir = core::ScopedTempDir::Create(host_tmp, L"fstio");
  if (!io_dir.ok()) {
    res.launch_error = io_dir.error;
    return res;
  }

  const std::wstring stdout_path = io_dir.value->ChildPath(L"stdout.log");
  const std::wstring stderr_path = io_dir.value->ChildPath(L"stderr.log");
  RedirectedStdHandles std_handles =
      OpenRedirectedStdHandles(stdout_path, stderr_path);
  if (!std_handles.ok()) {
    res.launch_error = std_handles.error;
    return res;
  }

  std::wstring cmd_line = BuildCommandLine(exe_path, args);
  std::vector<wchar_t> env_block = BuildChildEnvironmentBlock(
      env_overrides,
      {L"FSTEST_ROOT", L"GTEST_FILTER", L"GTEST_REPEAT", L"GTEST_LIST_TESTS",
       L"GTEST_FAIL_FAST", L"GTEST_TOTAL_SHARDS", L"GTEST_SHARD_INDEX",
       L"GTEST_ALSO_RUN_DISABLED_TESTS", L"FSTEST_SELFTEST_FAULT"});

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = std_handles.stdin_h.get();
  si.hStdOutput = std_handles.stdout_h.get();
  si.hStdError = std_handles.stderr_h.get();

  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(
      exe_path.c_str(), cmd_line.data(), nullptr, nullptr,
      /*bInheritHandles=*/TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
      env_block.data(), nullptr, &si, &pi);
  std_handles.CloseAll();
  if (ok == FALSE) {
    res.launch_error = ::GetLastError();
    return res;
  }

  core::UniqueHandle proc_h(pi.hProcess);
  core::UniqueHandle thread_h(pi.hThread);
  res.launched = true;

  if (::WaitForSingleObject(proc_h.get(), timeout_ms) == WAIT_TIMEOUT) {
    res.timed_out = true;
    ::TerminateProcess(proc_h.get(), 124);
    ::WaitForSingleObject(proc_h.get(), 5000);
  }
  ::GetExitCodeProcess(proc_h.get(), &res.exit_code);
  thread_h.reset();
  proc_h.reset();

  if (auto out_data = core::ReadFileContents(stdout_path); out_data.ok()) {
    res.stdout_text = std::move(*out_data.value);
  }
  if (auto err_data = core::ReadFileContents(stderr_path); err_data.ok()) {
    res.stderr_text = std::move(*err_data.value);
  }
  io_dir.value->Cleanup();
  return res;
}

class CliE2eTest : public ::testing::Test {
protected:
  void SetUp() override {
    exe_path_ = FindSiblingCliExecutable();
    if (exe_path_.empty()) {
      FAIL() << "Sibling dokan_fstest.exe not found alongside "
                "fstest_self_test.exe.";
      return;
    }
    work_dir_.emplace(L"fste2e");
    ASSERT_TRUE(work_dir_->ok());

    root_dir_ = work_dir_->ChildPath(L"root");
    ASSERT_NE(::CreateDirectoryW(root_dir_.c_str(), nullptr), FALSE)
        << core::FormatWin32Error(::GetLastError());
  }

  void TearDown() override { work_dir_.reset(); }

  std::wstring WriteBaselineFile(std::wstring_view filename,
                                 std::string_view json_content) {
    const std::wstring path_w = work_dir_->ChildPath(filename);
    const std::string path_utf8 = core::WideToUtf8(path_w);
    std::string err;
    EXPECT_TRUE(WriteTextFileUtf8(path_utf8, json_content, &err)) << err;
    return path_w;
  }

  void ReadAndParseJson(const std::wstring &json_path,
                        nlohmann::ordered_json *out) {
    std::string json_text;
    std::string err;
    ASSERT_TRUE(ReadTextFileUtf8(core::WideToUtf8(json_path), &json_text, &err))
        << err;
    ASSERT_TRUE(ParseJsonStrict(json_text, out, &err)) << err;
  }

  void VerifyXpassPolicy(std::wstring_view policy, DWORD expected_exit,
                         std::wstring_view file_stem) {
    const std::wstring baseline_path =
        WriteBaselineFile(std::wstring(file_stem) + L"_baseline.json", R"({
      "schema_version": 1,
      "targets": {
        "t": {
          "xfails": {
            "CreateOpenTest.CreateNewSucceedsAndRejectsExisting": {
              "reason": "synthetic e2e xfail"
            }
          }
        }
      }
    })");
    const std::wstring json_path =
        work_dir_->ChildPath(std::wstring(file_stem) + L"_out.json");

    const SubprocessResult res = RunCliSubprocess(
        exe_path_,
        {L"--root=" + root_dir_, L"--baseline=" + baseline_path, L"--target=t",
         L"--xpass-policy=" + std::wstring(policy),
         L"--gtest_filter=CreateOpenTest.CreateNewSucceedsAndRejectsExisting",
         L"--json-out=" + json_path});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, expected_exit) << res.stdout_text << "\n"
                                            << res.stderr_text;
    EXPECT_NE(res.stdout_text.find("[ XPASS! ]"), std::string::npos)
        << res.stdout_text;

    nlohmann::ordered_json report;
    ASSERT_NO_FATAL_FAILURE(ReadAndParseJson(json_path, &report));
    EXPECT_EQ(report["exit_code"].get<int>(), static_cast<int>(expected_exit));
    EXPECT_EQ(report["xpass_policy"].get<std::string>(),
              core::WideToUtf8(policy));
    EXPECT_EQ(report["summary"]["xpass"].get<uint32_t>(), 1u);
  }

  std::wstring exe_path_;
  std::optional<test_support::ScopedTestDirectory> work_dir_;
  std::wstring root_dir_;
};

TEST_F(CliE2eTest, HelpExitsZeroAndPrintsUsage) {
  const SubprocessResult res = RunCliSubprocess(exe_path_, {L"--help"});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 0UL) << res.stderr_text;
  EXPECT_NE(res.stdout_text.find("Usage:"), std::string::npos)
      << res.stdout_text;
  EXPECT_NE(res.stdout_text.find("--root=<path>"), std::string::npos)
      << res.stdout_text;
}

TEST_F(CliE2eTest, VersionExitsZeroAndPrintsVersion) {
  const SubprocessResult res = RunCliSubprocess(exe_path_, {L"--version"});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 0UL) << res.stderr_text;
  EXPECT_NE(res.stdout_text.find(FormatVersionText()), std::string::npos)
      << res.stdout_text;
}

TEST_F(CliE2eTest, UnknownFlagAndMissingRootExitTwo) {
  {
    const SubprocessResult bogus =
        RunCliSubprocess(exe_path_, {L"--bogus-flag"});
    ASSERT_TRUE(bogus.launched) << core::FormatWin32Error(bogus.launch_error);
    ASSERT_FALSE(bogus.timed_out);
    EXPECT_EQ(bogus.exit_code, 2UL) << bogus.stdout_text;
    EXPECT_NE(bogus.stderr_text.find("--bogus-flag"), std::string::npos)
        << bogus.stderr_text;
  }
  {
    const SubprocessResult missing_root = RunCliSubprocess(exe_path_, {});
    ASSERT_TRUE(missing_root.launched)
        << core::FormatWin32Error(missing_root.launch_error);
    ASSERT_FALSE(missing_root.timed_out);
    EXPECT_EQ(missing_root.exit_code, 2UL) << missing_root.stdout_text;
    EXPECT_NE(missing_root.stderr_text.find("--root"), std::string::npos)
        << missing_root.stderr_text;
  }
}

TEST_F(CliE2eTest, CleanupDirRejectsUnsafeAndRemovesSafeReadOnlyTree) {
  // Unsafe drive root must be refused with exit 2.
  {
    const SubprocessResult unsafe =
        RunCliSubprocess(exe_path_, {L"--cleanup-dir=C:\\"});
    ASSERT_TRUE(unsafe.launched) << core::FormatWin32Error(unsafe.launch_error);
    ASSERT_FALSE(unsafe.timed_out);
    EXPECT_EQ(unsafe.exit_code, 2UL) << unsafe.stdout_text;
    EXPECT_NE(unsafe.stderr_text.find("unsafe"), std::string::npos)
        << unsafe.stderr_text;
  }

  // Safe temp subdirectory containing a read-only file is removed cleanly.
  const std::wstring target = work_dir_->ChildPath(L"cleanup_victim");
  ASSERT_NE(::CreateDirectoryW(target.c_str(), nullptr), FALSE)
      << core::FormatWin32Error(::GetLastError());
  const std::wstring ro_file = core::JoinPath(target, L"readonly.dat");
  auto wr = core::WriteFileContents(ro_file, "readonly payload");
  ASSERT_TRUE(wr.ok()) << core::FormatWin32Error(wr.error);
  ASSERT_NE(::SetFileAttributesW(ro_file.c_str(), FILE_ATTRIBUTE_READONLY),
            FALSE)
      << core::FormatWin32Error(::GetLastError());

  const SubprocessResult safe =
      RunCliSubprocess(exe_path_, {L"--cleanup-dir=" + target});
  ASSERT_TRUE(safe.launched) << core::FormatWin32Error(safe.launch_error);
  ASSERT_FALSE(safe.timed_out);
  EXPECT_EQ(safe.exit_code, 0UL) << safe.stderr_text;
  EXPECT_EQ(::GetFileAttributesW(target.c_str()), INVALID_FILE_ATTRIBUTES);

  // A target that no longer exists is a successful no-op.
  const SubprocessResult absent =
      RunCliSubprocess(exe_path_, {L"--cleanup-dir=" + target});
  ASSERT_TRUE(absent.launched) << core::FormatWin32Error(absent.launch_error);
  EXPECT_EQ(absent.exit_code, 0UL) << absent.stderr_text;
}

TEST_F(CliE2eTest, CleanupDirCanonicalizesTrailingDotAndRejectsAliases) {
  // B-M2(e): "<dir>\." must delete exactly the validated canonical directory.
  const std::wstring target = work_dir_->ChildPath(L"dot_victim");
  ASSERT_NE(::CreateDirectoryW(target.c_str(), nullptr), FALSE)
      << core::FormatWin32Error(::GetLastError());
  ASSERT_TRUE(
      core::WriteFileContents(core::JoinPath(target, L"f.txt"), "x").ok());
  const SubprocessResult dot =
      RunCliSubprocess(exe_path_, {L"--cleanup-dir=" + target + L"\\."});
  ASSERT_TRUE(dot.launched) << core::FormatWin32Error(dot.launch_error);
  ASSERT_FALSE(dot.timed_out);
  EXPECT_EQ(dot.exit_code, 0UL) << dot.stdout_text << "\n" << dot.stderr_text;
  EXPECT_EQ(::GetFileAttributesW(target.c_str()), INVALID_FILE_ATTRIBUTES);
  EXPECT_NE(::GetFileAttributesW(root_dir_.c_str()), INVALID_FILE_ATTRIBUTES);

  // Aliases of protected system locations are refused with exit 2.
  for (const std::wstring &alias :
       {std::wstring(L"\\\\localhost\\C$\\Windows\\System32"),
        std::wstring(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\Windows"),
        std::wstring(L"C:\\Windows\\.")}) {
    const SubprocessResult res =
        RunCliSubprocess(exe_path_, {L"--cleanup-dir=" + alias});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 2UL) << core::WideToUtf8(alias) << "\n"
                                  << res.stderr_text;
    EXPECT_NE(res.stderr_text.find("unsafe"), std::string::npos)
        << res.stderr_text;
  }
}

TEST_F(CliE2eTest, CleanupDirDeletionFailureExitsTwo) {
  // m8: a deletion failure is an infrastructure error (exit 2), not a
  // conformance regression (exit 1).
  const std::wstring target = work_dir_->ChildPath(L"locked_victim");
  ASSERT_NE(::CreateDirectoryW(target.c_str(), nullptr), FALSE)
      << core::FormatWin32Error(::GetLastError());
  const std::wstring locked = core::JoinPath(target, L"locked.dat");
  ASSERT_TRUE(core::WriteFileContents(locked, "busy").ok());
  {
    // No FILE_SHARE_DELETE: the child cannot delete (or rename) the file.
    auto holder = core::CreateWin32File(locked, GENERIC_READ, FILE_SHARE_READ,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(holder.ok()) << core::FormatWin32Error(holder.error);

    const SubprocessResult res =
        RunCliSubprocess(exe_path_, {L"--cleanup-dir=" + target});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
    EXPECT_NE(res.stderr_text.find("Failed to remove"), std::string::npos)
        << res.stderr_text;
  }
  EXPECT_NE(::GetFileAttributesW(locked.c_str()), INVALID_FILE_ATTRIBUTES);
}

TEST_F(CliE2eTest, StrictSingleTestProducesValidJsonReport) {
  const std::wstring json_path = work_dir_->ChildPath(L"strict_report.json");
  const SubprocessResult res = RunCliSubprocess(
      exe_path_,
      {L"--root=" + root_dir_, L"--strict",
       L"--gtest_filter=CreateOpenTest.CreateNewSucceedsAndRejectsExisting",
       L"--json-out=" + json_path});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 0UL) << res.stdout_text << "\n" << res.stderr_text;

  nlohmann::ordered_json report;
  ASSERT_NO_FATAL_FAILURE(ReadAndParseJson(json_path, &report));

  EXPECT_EQ(report["schema_version"].get<int>(), 1);
  EXPECT_EQ(report["mode"].get<std::string>(), "strict");
  EXPECT_EQ(report["exit_code"].get<int>(), 0);
  EXPECT_EQ(report["summary"]["total"].get<uint32_t>(), 1u);
  EXPECT_EQ(report["summary"]["pass"].get<uint32_t>(), 1u);
  EXPECT_EQ(report["summary"]["fail"].get<uint32_t>(), 0u);
}

TEST_F(CliE2eTest, BaselineXpassPolicyFailExitsThree) {
  VerifyXpassPolicy(L"fail", /*expected_exit=*/3UL, L"xpass_fail");
}

TEST_F(CliE2eTest, BaselineXpassPolicyWarnExitsZero) {
  VerifyXpassPolicy(L"warn", /*expected_exit=*/0UL, L"xpass_warn");
}

TEST_F(CliE2eTest, UpdateBaselineRejectsGtestFilterEnvVar) {
  const std::string initial_json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {}
      }
    }
  })";
  const std::wstring baseline_path =
      WriteBaselineFile(L"env_guard.json", initial_json);

  const SubprocessResult res =
      RunCliSubprocess(exe_path_,
                       {L"--root=" + root_dir_, L"--baseline=" + baseline_path,
                        L"--target=t", L"--update-baseline"},
                       {{L"GTEST_FILTER", L"CreateOpenTest.*"}});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
  EXPECT_NE(res.stderr_text.find("GTEST_FILTER"), std::string::npos)
      << res.stderr_text;

  // Baseline file must remain untouched on guard failure.
  std::string after_json;
  std::string err;
  ASSERT_TRUE(
      ReadTextFileUtf8(core::WideToUtf8(baseline_path), &after_json, &err))
      << err;
  EXPECT_EQ(after_json, initial_json);
}

TEST_F(CliE2eTest, UpdateBaselineRejectsGtestRepeatEnvVar) {
  const std::string initial_json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {}
      }
    }
  })";
  const std::wstring baseline_path =
      WriteBaselineFile(L"repeat_env_guard.json", initial_json);

  const SubprocessResult res =
      RunCliSubprocess(exe_path_,
                       {L"--root=" + root_dir_, L"--baseline=" + baseline_path,
                        L"--target=t", L"--update-baseline"},
                       {{L"GTEST_REPEAT", L"2"}});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
  EXPECT_NE(res.stderr_text.find("GTEST_REPEAT"), std::string::npos)
      << res.stderr_text;

  std::string after_json;
  std::string err;
  ASSERT_TRUE(
      ReadTextFileUtf8(core::WideToUtf8(baseline_path), &after_json, &err))
      << err;
  EXPECT_EQ(after_json, initial_json);
}

TEST_F(CliE2eTest, UpdateBaselineHappyPathPreservesCrossHostEntries) {
  const core::OsInfo &os = core::GetOsInfo();
  const std::string build = std::to_string(os.build);
  const std::string arch = DefaultBinaryArch();
  // A: unpredicated stale entry that passes here -> kept (may fail elsewhere)
  //    and listed under "passed on this host".
  // B: host-specific entry (this exact build + arch) that passes -> removed.
  // C: skip_crash entry -> never executed, preserved verbatim incl. "gap".
  const std::string initial_json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.CreateNewSucceedsAndRejectsExisting": {
            "reason": "synthetic stale unpredicated xfail"
          },
          "CreateOpenTest.OpenExistingNotFoundAndExisting": {
            "reason": "synthetic host-specific xfail",
            "os_min_build": )" + build +
                                   R"(,
            "os_max_build": )" + build +
                                   R"(,
            "arch": [")" + arch + R"("]
          },
          "CreateOpenTest.CreateAlwaysSetsAlreadyExistsLastError": {
            "reason": "synthetic crash-skip",
            "gap": "GAP-42",
            "skip_crash": true
          }
        }
      }
    }
  })";
  const std::wstring in_path =
      WriteBaselineFile(L"update_in.json", initial_json);
  const std::wstring out_path = work_dir_->ChildPath(L"update_out.json");

  const SubprocessResult res =
      RunCliSubprocess(exe_path_,
                       {L"--root=" + root_dir_, L"--baseline=" + in_path,
                        L"--target=t", L"--update-baseline=" + out_path,
                        L"--xpass-policy=warn", L"--large-dir-count=10"},
                       {}, kFullSuiteTimeoutMs);
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 0UL) << res.stdout_text << "\n" << res.stderr_text;
  EXPECT_NE(res.stdout_text.find("Updated baseline written to:"),
            std::string::npos)
      << res.stdout_text;
  EXPECT_NE(res.stdout_text.find("passed on this host"), std::string::npos)
      << res.stdout_text;
  EXPECT_NE(res.stdout_text.find("--prune-passing"), std::string::npos)
      << res.stdout_text;

  BaselineManifest updated;
  std::string err;
  ASSERT_TRUE(
      LoadBaselineManifestFile(core::WideToUtf8(out_path), &updated, &err))
      << err;
  ASSERT_TRUE(updated.targets.contains("t"));
  const auto &xfails = updated.targets.at("t").xfails;
  EXPECT_TRUE(xfails.contains(std::string(kTestCreateNew)));
  EXPECT_FALSE(xfails.contains(std::string(kTestOpenExisting)));
  ASSERT_TRUE(xfails.contains(std::string(kTestCreateAlways)));
  EXPECT_TRUE(xfails.at(std::string(kTestCreateAlways)).skip_crash);
  EXPECT_EQ(xfails.at(std::string(kTestCreateAlways)).gap, "GAP-42");

  // Input baseline remains untouched when an explicit output path is given.
  std::string in_after;
  ASSERT_TRUE(ReadTextFileUtf8(core::WideToUtf8(in_path), &in_after, &err))
      << err;
  EXPECT_EQ(in_after, initial_json);
}

TEST_F(CliE2eTest, UpdateBaselinePrunePassingAndRebasesExtendsFile) {
  WriteBaselineFile(L"parent.json", R"({
    "schema_version": 1,
    "targets": {
      "base": {
        "abstract": true,
        "xfails": {
          "CreateOpenTest.CreateInMissingParentReturnsPathNotFound": {
            "reason": "synthetic unpredicated parent xfail"
          }
        }
      }
    }
  })");
  const std::wstring in_path = WriteBaselineFile(L"child.json", R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "extends_file": "parent.json",
        "extends": "base",
        "xfails": {
          "CreateOpenTest.CreateNewSucceedsAndRejectsExisting": {
            "reason": "synthetic stale unpredicated xfail"
          }
        }
      }
    }
  })");
  // m4: written into a different directory -> relative extends_file rebased.
  const std::wstring out_path =
      work_dir_->ChildPath(L"nested\\deeper\\pruned.json");

  const SubprocessResult res = RunCliSubprocess(
      exe_path_,
      {L"--root=" + root_dir_, L"--baseline=" + in_path, L"--target=t",
       L"--update-baseline=" + out_path, L"--prune-passing",
       L"--xpass-policy=warn", L"--large-dir-count=10"},
      {}, kFullSuiteTimeoutMs);
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 0UL) << res.stdout_text << "\n" << res.stderr_text;
  EXPECT_NE(res.stdout_text.find(std::string(kTestCreateNew)),
            std::string::npos)
      << res.stdout_text;

  BaselineManifest updated;
  std::string err;
  ASSERT_TRUE(
      LoadBaselineManifestFile(core::WideToUtf8(out_path), &updated, &err))
      << err;
  const TargetDefinition &t = updated.targets.at("t");
  // --prune-passing removes the passing unpredicated child entry and
  // suppresses the passing unpredicated parent entry.
  EXPECT_FALSE(t.xfails.contains(std::string(kTestCreateNew)));
  EXPECT_NE(std::find(t.remove_xfails.begin(), t.remove_xfails.end(),
                      std::string(kTestMissingParent)),
            t.remove_xfails.end());
  EXPECT_NE(t.extends_file, "parent.json");

  // The rewritten manifest still resolves its parent from the new location.
  ResolvedBaseline resolved;
  ASSERT_TRUE(ResolveBaselineFromFile(core::WideToUtf8(out_path), "t",
                                      core::GetOsInfo().build,
                                      DefaultBinaryArch(), &resolved, &err))
      << err;
  EXPECT_TRUE(
      resolved.parent_entries.contains(std::string(kTestMissingParent)));
  EXPECT_FALSE(
      resolved.active_entries.contains(std::string(kTestMissingParent)));
}

TEST_F(CliE2eTest, ValidateBaselineCommittedManifestsAndRejectsUnknownIds) {
  for (const std::wstring_view name :
       {std::wstring_view(L"dokan_memfs.json"),
        std::wstring_view(L"dokan_mirror.json")}) {
    const std::wstring manifest_path =
        FindCommittedBaselinePath(exe_path_, name);
    ASSERT_FALSE(manifest_path.empty())
        << "Could not locate committed baseline: " << core::WideToUtf8(name);

    const SubprocessResult res =
        RunCliSubprocess(exe_path_, {L"--validate-baseline=" + manifest_path});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 0UL) << res.stdout_text << "\n" << res.stderr_text;
    EXPECT_NE(res.stdout_text.find("Baseline valid:"), std::string::npos)
        << res.stdout_text;
  }

  // Inactive OS-gated typo must still fail --validate-baseline with exit 2.
  const std::wstring bad_manifest = WriteBaselineFile(L"bad_validate.json", R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.DoesNotExistAtAll": {
            "reason": "stale inactive typo",
            "os_min_build": 99999
          }
        }
      }
    }
  })");
  const SubprocessResult bad =
      RunCliSubprocess(exe_path_, {L"--validate-baseline=" + bad_manifest});
  ASSERT_TRUE(bad.launched) << core::FormatWin32Error(bad.launch_error);
  ASSERT_FALSE(bad.timed_out);
  EXPECT_EQ(bad.exit_code, 2UL) << bad.stdout_text << "\n" << bad.stderr_text;
  EXPECT_NE(bad.stderr_text.find("CreateOpenTest.DoesNotExistAtAll"),
            std::string::npos)
      << bad.stderr_text;
}

TEST_F(CliE2eTest, SkipCrashEntryRecordsCrashSkippedAndExitsZero) {
  const std::wstring baseline_path = WriteBaselineFile(L"skip_crash.json", R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.CreateNewSucceedsAndRejectsExisting": {
            "reason": "synthetic crash-skip e2e",
            "gap": "GAP-99",
            "skip_crash": true
          }
        }
      }
    }
  })");
  const std::wstring json_path = work_dir_->ChildPath(L"skip_crash_out.json");

  // A real executed test alongside the crash-skipped one (an all-skip run is
  // exit 2, see SkipCrashOnlyRunExitsTwo).
  const SubprocessResult res = RunCliSubprocess(
      exe_path_,
      {L"--root=" + root_dir_, L"--baseline=" + baseline_path, L"--target=t",
       L"--gtest_filter=" + W(kTestCreateNew) + L":" + W(kTestOpenExisting),
       L"--json-out=" + json_path});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 0UL) << res.stdout_text << "\n" << res.stderr_text;
  EXPECT_NE(res.stdout_text.find("[skip_crash]"), std::string::npos)
      << res.stdout_text;
  // The crash-skipped test never runs.
  EXPECT_EQ(res.stdout_text.find("[ RUN    ] " + std::string(kTestCreateNew)),
            std::string::npos)
      << res.stdout_text;

  nlohmann::ordered_json report;
  ASSERT_NO_FATAL_FAILURE(ReadAndParseJson(json_path, &report));
  EXPECT_EQ(report["exit_code"].get<int>(), 0);
  EXPECT_EQ(report["summary"]["total"].get<uint32_t>(), 2u);
  EXPECT_EQ(report["summary"]["pass"].get<uint32_t>(), 1u);
  EXPECT_EQ(report["summary"]["xfail"].get<uint32_t>(), 1u);
  EXPECT_EQ(report["summary"]["crash_skipped"].get<uint32_t>(), 1u);
  ASSERT_EQ(report["tests"].size(), 2u);
  EXPECT_TRUE(report["tests"][0]["skip_crash"].get<bool>());
}

TEST_F(CliE2eTest, SkipCrashOnlyRunExitsTwoAndEmptyFilterRunsNothing) {
  const std::wstring baseline_path =
      WriteBaselineFile(L"skip_crash_only.json", R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.CreateNewSucceedsAndRejectsExisting": {
            "reason": "synthetic crash-skip e2e",
            "gap": "GAP-99",
            "skip_crash": true
          }
        }
      }
    }
  })");

  // m3: nothing actually executed -> exit 2.
  {
    const SubprocessResult res = RunCliSubprocess(
        exe_path_, {L"--root=" + root_dir_, L"--baseline=" + baseline_path,
                    L"--target=t", L"--gtest_filter=" + W(kTestCreateNew)});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
    EXPECT_NE(res.stderr_text.find("every selected test was skipped"),
              std::string::npos)
        << res.stderr_text;
  }
  // m1: an empty --gtest_filter= selects nothing; skip_crash exclusion must
  // not widen it to "*-<ids>" (which would run the whole suite).
  {
    const SubprocessResult res = RunCliSubprocess(
        exe_path_, {L"--root=" + root_dir_, L"--baseline=" + baseline_path,
                    L"--target=t", L"--gtest_filter="});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
    EXPECT_NE(res.stderr_text.find("0 tests executed"), std::string::npos)
        << res.stderr_text;
    EXPECT_EQ(res.stdout_text.find("[ RUN    ]"), std::string::npos)
        << res.stdout_text;
  }
}

TEST_F(CliE2eTest, FatalSetupAndTeardownCannotBeMaskedByXfail) {
  const std::wstring baseline_path = WriteBaselineFile(L"mask.json", R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.CreateNewSucceedsAndRejectsExisting": {
            "reason": "synthetic xfail that must not mask harness failures",
            "gap": "GAP-99",
            "allow_xpass": true
          }
        }
      }
    }
  })");

  struct Case {
    const wchar_t *fault;
    const char *console_tag;
    const char *json_flag;
  };
  for (const Case &c :
       {Case{L"before_test", "(fatal setup)", "fatal_setup"},
        Case{L"after_test", "(fatal teardown)", "fatal_teardown"}}) {
    const std::wstring json_path =
        work_dir_->ChildPath(std::wstring(L"mask_") + c.fault + L".json");
    const SubprocessResult res = RunCliSubprocess(
        exe_path_,
        {L"--root=" + root_dir_, L"--baseline=" + baseline_path, L"--target=t",
         L"--gtest_filter=" + W(kTestCreateNew), L"--json-out=" + json_path},
        {{L"FSTEST_SELFTEST_FAULT", c.fault}});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 1UL) << c.json_flag << "\n"
                                  << res.stdout_text << "\n"
                                  << res.stderr_text;
    EXPECT_NE(res.stdout_text.find(c.console_tag), std::string::npos)
        << res.stdout_text;
    EXPECT_EQ(res.stdout_text.find("[  XFAIL ]"), std::string::npos)
        << res.stdout_text;

    nlohmann::ordered_json report;
    ASSERT_NO_FATAL_FAILURE(ReadAndParseJson(json_path, &report));
    EXPECT_EQ(report["summary"]["fail"].get<uint32_t>(), 1u);
    EXPECT_EQ(report["summary"]["xfail"].get<uint32_t>(), 0u);
    ASSERT_EQ(report["tests"].size(), 1u);
    EXPECT_EQ(report["tests"][0]["outcome"].get<std::string>(), "FAIL");
    EXPECT_TRUE(report["tests"][0][c.json_flag].get<bool>());
  }

  // Unknown fault names are rejected up front.
  const SubprocessResult bogus =
      RunCliSubprocess(exe_path_, {L"--root=" + root_dir_, L"--strict"},
                       {{L"FSTEST_SELFTEST_FAULT", L"bogus"}});
  ASSERT_TRUE(bogus.launched) << core::FormatWin32Error(bogus.launch_error);
  EXPECT_EQ(bogus.exit_code, 2UL) << bogus.stderr_text;
}

TEST_F(CliE2eTest, MountLostAbortsRemainingTestsWithExitTwo) {
  const std::wstring json_path = work_dir_->ChildPath(L"mount_lost.json");
  const SubprocessResult res =
      RunCliSubprocess(exe_path_,
                       {L"--root=" + root_dir_, L"--strict",
                        L"--gtest_filter=" + W(kTestCreateNew) + L":" +
                            W(kTestCreateAlways) + L":" + W(kTestOpenExisting),
                        L"--json-out=" + json_path},
                       {{L"FSTEST_SELFTEST_FAULT", L"mount_lost"}});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
  const std::string expected =
      "mount lost after " + std::string(kTestCreateNew);
  EXPECT_NE(res.stderr_text.find(expected), std::string::npos)
      << res.stderr_text;
  EXPECT_NE(res.stdout_text.find(expected), std::string::npos)
      << res.stdout_text;

  nlohmann::ordered_json report;
  ASSERT_NO_FATAL_FAILURE(ReadAndParseJson(json_path, &report));
  EXPECT_EQ(report["exit_code"].get<int>(), 2);
  // First test passed, second failed (fatal setup), third never ran.
  EXPECT_EQ(report["summary"]["total"].get<uint32_t>(), 3u);
  EXPECT_EQ(report["summary"]["pass"].get<uint32_t>(), 1u);
  EXPECT_EQ(report["summary"]["fail"].get<uint32_t>(), 1u);
  EXPECT_EQ(report["summary"]["skip"].get<uint32_t>(), 1u);
}

TEST_F(CliE2eTest, GtestFailFastEnvRejectedWithBaseline) {
  const std::wstring baseline_path = WriteBaselineFile(L"ff.json", R"({
    "schema_version": 1,
    "targets": {"t": {"xfails": {}}}
  })");
  const SubprocessResult res =
      RunCliSubprocess(exe_path_,
                       {L"--root=" + root_dir_, L"--baseline=" + baseline_path,
                        L"--target=t", L"--gtest_filter=" + W(kTestCreateNew)},
                       {{L"GTEST_FAIL_FAST", L"1"}});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
  EXPECT_NE(res.stderr_text.find("fail-fast"), std::string::npos)
      << res.stderr_text;
}

TEST_F(CliE2eTest, UpdateBaselineRejectsGtestSharding) {
  const std::string initial_json = R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {}
      }
    }
  })";
  const std::wstring baseline_path =
      WriteBaselineFile(L"shard_guard.json", initial_json);
  const SubprocessResult res = RunCliSubprocess(
      exe_path_,
      {L"--root=" + root_dir_, L"--baseline=" + baseline_path, L"--target=t",
       L"--update-baseline"},
      {{L"GTEST_TOTAL_SHARDS", L"2"}, {L"GTEST_SHARD_INDEX", L"0"}});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
  EXPECT_NE(res.stderr_text.find("GTEST_TOTAL_SHARDS"), std::string::npos)
      << res.stderr_text;
  std::string after_json;
  std::string err;
  ASSERT_TRUE(
      ReadTextFileUtf8(core::WideToUtf8(baseline_path), &after_json, &err))
      << err;
  EXPECT_EQ(after_json, initial_json);
}

TEST_F(CliE2eTest, FailExitsOneOnAccessDeniedRoot) {
  const std::wstring ro_root = work_dir_->ChildPath(L"ro_root");
  ASSERT_NE(::CreateDirectoryW(ro_root.c_str(), nullptr), FALSE)
      << core::FormatWin32Error(::GetLastError());

  const std::wstring ro_ext = core::ToExtendedWin32Path(ro_root);
  core::ScopedDaclRestore restore =
      core::ScopedDaclRestore::Arm(core::VerbatimToNtPath(ro_ext));
  ASSERT_TRUE(restore.armed());

  // Allow read/traverse (so --root check & volume probe succeed), but deny
  // FILE_ADD_FILE (0x2) | FILE_ADD_SUBDIRECTORY (0x4) with OICI inheritance so
  // CreateFileW fails ERROR_ACCESS_DENIED even if SeRestorePrivilege is enabled
  // in the test runner's token.
  auto set_res =
      core::SetFileSddl(ro_ext, DACL_SECURITY_INFORMATION,
                        L"D:P(D;OICI;0x00000006;;;WD)(A;OICI;FRFX;;;WD)");
  ASSERT_TRUE(set_res.ok()) << core::FormatWin32Error(set_res.error);

  const std::wstring json_path = work_dir_->ChildPath(L"fail_exit1.json");
  const SubprocessResult res = RunCliSubprocess(
      exe_path_,
      {L"--root=" + ro_root, L"--strict",
       L"--gtest_filter=CreateOpenTest.CreateNewSucceedsAndRejectsExisting",
       L"--json-out=" + json_path});

  // B-M4: the per-test temp directory cannot be created (fatal setup), so a
  // baseline XFAIL for the test must NOT mask the failure.
  const std::wstring xfail_baseline = WriteBaselineFile(L"ro_mask.json", R"({
    "schema_version": 1,
    "targets": {
      "t": {
        "xfails": {
          "CreateOpenTest.CreateNewSucceedsAndRejectsExisting": {
            "reason": "synthetic xfail"
          }
        }
      }
    }
  })");
  const SubprocessResult masked = RunCliSubprocess(
      exe_path_,
      {L"--root=" + ro_root, L"--baseline=" + xfail_baseline, L"--target=t",
       L"--gtest_filter=CreateOpenTest.CreateNewSucceedsAndRejectsExisting"});
  restore.RestoreNow();
  ASSERT_TRUE(masked.launched) << core::FormatWin32Error(masked.launch_error);
  ASSERT_FALSE(masked.timed_out);
  EXPECT_EQ(masked.exit_code, 1UL) << masked.stdout_text << "\n"
                                   << masked.stderr_text;
  EXPECT_NE(masked.stdout_text.find("(fatal setup)"), std::string::npos)
      << masked.stdout_text;

  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 1UL) << res.stdout_text << "\n" << res.stderr_text;

  nlohmann::ordered_json report;
  ASSERT_NO_FATAL_FAILURE(ReadAndParseJson(json_path, &report));
  EXPECT_EQ(report["exit_code"].get<int>(), 1);
  EXPECT_EQ(report["summary"]["fail"].get<uint32_t>(), 1u);
}

TEST_F(CliE2eTest, ExitTwoOnZeroTestsUnknownBaselineIdAndInvalidRoot) {
  // 1. 0 tests executed -> exit 2.
  {
    const SubprocessResult res =
        RunCliSubprocess(exe_path_, {L"--root=" + root_dir_, L"--strict",
                                     L"--gtest_filter=NoSuchSuite.NoSuchTest"});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
    EXPECT_NE(res.stderr_text.find("0 tests executed"), std::string::npos)
        << res.stderr_text;
  }

  // 2. Unknown baseline ID (even inactive) -> exit 2.
  {
    const std::wstring bad_baseline = WriteBaselineFile(L"unknown_id.json", R"({
      "schema_version": 1,
      "targets": {
        "t": {
          "xfails": {
            "DeleteTest.NonExistentId": {
              "reason": "typo",
              "os_min_build": 99999
            }
          }
        }
      }
    })");
    const SubprocessResult res = RunCliSubprocess(
        exe_path_,
        {L"--root=" + root_dir_, L"--baseline=" + bad_baseline, L"--target=t",
         L"--gtest_filter=CreateOpenTest.CreateNewSucceedsAndRejectsExisting"});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
    EXPECT_NE(res.stderr_text.find("DeleteTest.NonExistentId"),
              std::string::npos)
        << res.stderr_text;
  }

  // 3. Nonexistent --root -> exit 2.
  {
    const std::wstring missing_root = work_dir_->ChildPath(L"missing_root_dir");
    const SubprocessResult res =
        RunCliSubprocess(exe_path_, {L"--root=" + missing_root, L"--strict"});
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;
  }
}

TEST_F(CliE2eTest, CleanupDirRejectsGtestListTestsWithoutDeletingDirectory) {
  const std::wstring target = work_dir_->ChildPath(L"keep_me");
  ASSERT_NE(::CreateDirectoryW(target.c_str(), nullptr), FALSE)
      << core::FormatWin32Error(::GetLastError());
  const std::wstring canary = core::JoinPath(target, L"canary.txt");
  ASSERT_TRUE(core::WriteFileContents(canary, "intact").ok());

  const SubprocessResult res = RunCliSubprocess(
      exe_path_, {L"--cleanup-dir=" + target, L"--gtest_list_tests"});
  ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
  ASSERT_FALSE(res.timed_out);
  EXPECT_EQ(res.exit_code, 2UL) << res.stdout_text << "\n" << res.stderr_text;

  // Target directory and canary file must remain completely untouched.
  EXPECT_NE(::GetFileAttributesW(canary.c_str()), INVALID_FILE_ATTRIBUTES);
}

TEST_F(CliE2eTest, GtestListTestsAndEnvVarListAllTwelveDomainSuites) {
  auto verify_all_domains_listed = [](const SubprocessResult &res) {
    ASSERT_TRUE(res.launched) << core::FormatWin32Error(res.launch_error);
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 0UL) << res.stdout_text << "\n" << res.stderr_text;
    for (const DomainInfo &domain : GetAllDomains()) {
      const std::string header = std::string(domain.suite_name) + ".";
      EXPECT_NE(res.stdout_text.find(header), std::string::npos)
          << "Missing domain suite '" << header << "' in --gtest_list_tests "
          << "output";
    }
  };

  // Explicit CLI flag --gtest_list_tests.
  verify_all_domains_listed(
      RunCliSubprocess(exe_path_, {L"--gtest_list_tests"}));

  // Environment variable GTEST_LIST_TESTS=1 with no --root.
  verify_all_domains_listed(
      RunCliSubprocess(exe_path_, {}, {{L"GTEST_LIST_TESTS", L"1"}}));

  // Environment variable GTEST_LIST_TESTS=1 with --root.
  verify_all_domains_listed(RunCliSubprocess(
      exe_path_, {L"--root=" + root_dir_}, {{L"GTEST_LIST_TESTS", L"1"}}));

  // m7: GoogleTest's env parsing treats anything but "0" as true.
  verify_all_domains_listed(
      RunCliSubprocess(exe_path_, {}, {{L"GTEST_LIST_TESTS", L"false"}}));
}

} // namespace
} // namespace fstest::cli
