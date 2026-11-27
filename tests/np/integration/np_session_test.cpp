// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_env.h"

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dokan_np_integration {
namespace {

// Minimal LSA types to avoid <ntsecapi.h>'s UNICODE_STRING redefinition
// against dokan/fileinfo.h.
struct NpLsaUnicodeString {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR Buffer;
};

struct NpLsaObjectAttributes {
  ULONG Length;
  HANDLE RootDirectory;
  NpLsaUnicodeString *ObjectName;
  ULONG Attributes;
  PVOID SecurityDescriptor;
  PVOID SecurityQualityOfService;
};

constexpr ACCESS_MASK kPolicyLookupNames = 0x00000800L;
constexpr ACCESS_MASK kPolicyCreateAccount = 0x00000010L;

using LsaOpenPolicyFn = NTSTATUS(NTAPI *)(NpLsaUnicodeString *,
                                          NpLsaObjectAttributes *, ACCESS_MASK,
                                          PVOID *);
using LsaEnumerateAccountRightsFn = NTSTATUS(NTAPI *)(PVOID, PSID,
                                                      NpLsaUnicodeString **,
                                                      PULONG);
using LsaAddAccountRightsFn = NTSTATUS(NTAPI *)(PVOID, PSID,
                                                NpLsaUnicodeString *, ULONG);
using LsaRemoveAccountRightsFn = NTSTATUS(NTAPI *)(PVOID, PSID, BOOLEAN,
                                                   NpLsaUnicodeString *, ULONG);
using LsaFreeMemoryFn = NTSTATUS(NTAPI *)(PVOID);
using LsaCloseFn = NTSTATUS(NTAPI *)(PVOID);

// Ensures a local account has SeBatchLogonRight for the duration of a
// Scheduled Task probe (CI images create dokan_ifstest via `net user /ADD`
// without pre-granting batch logon). Restores the prior state on destruction.
class BatchLogonRightGuard {
public:
  explicit BatchLogonRightGuard(const std::wstring &account_name) {
    if (account_name.empty()) {
      return;
    }
    HMODULE advapi = ::GetModuleHandleW(L"advapi32.dll");
    if (advapi == nullptr) {
      return;
    }
    auto lsa_open = reinterpret_cast<LsaOpenPolicyFn>(
        ::GetProcAddress(advapi, "LsaOpenPolicy"));
    auto lsa_enum = reinterpret_cast<LsaEnumerateAccountRightsFn>(
        ::GetProcAddress(advapi, "LsaEnumerateAccountRights"));
    auto lsa_add = reinterpret_cast<LsaAddAccountRightsFn>(
        ::GetProcAddress(advapi, "LsaAddAccountRights"));
    lsa_remove_ = reinterpret_cast<LsaRemoveAccountRightsFn>(
        ::GetProcAddress(advapi, "LsaRemoveAccountRights"));
    auto lsa_free = reinterpret_cast<LsaFreeMemoryFn>(
        ::GetProcAddress(advapi, "LsaFreeMemory"));
    lsa_close_ =
        reinterpret_cast<LsaCloseFn>(::GetProcAddress(advapi, "LsaClose"));
    if (lsa_open == nullptr || lsa_enum == nullptr || lsa_add == nullptr ||
        lsa_remove_ == nullptr || lsa_free == nullptr ||
        lsa_close_ == nullptr) {
      return;
    }

    DWORD sid_size = 0;
    DWORD dom_size = 0;
    SID_NAME_USE use = SidTypeUnknown;
    ::LookupAccountNameW(nullptr, account_name.c_str(), nullptr, &sid_size,
                         nullptr, &dom_size, &use);
    if (sid_size == 0) {
      return;
    }
    sid_.resize(sid_size);
    std::vector<WCHAR> domain(dom_size + 1);
    if (!::LookupAccountNameW(nullptr, account_name.c_str(), sid_.data(),
                              &sid_size, domain.data(), &dom_size, &use)) {
      sid_.clear();
      return;
    }

    NpLsaObjectAttributes oa{};
    if (lsa_open(nullptr, &oa, kPolicyLookupNames | kPolicyCreateAccount,
                 &policy_) != 0) {
      policy_ = nullptr;
      return;
    }

    NpLsaUnicodeString *rights = nullptr;
    ULONG count = 0;
    bool already_granted = false;
    if (lsa_enum(policy_, sid_.data(), &rights, &count) == 0) {
      for (ULONG i = 0; i < count; ++i) {
        const std::wstring r(rights[i].Buffer,
                             rights[i].Length / sizeof(WCHAR));
        if (_wcsicmp(r.c_str(), L"SeBatchLogonRight") == 0) {
          already_granted = true;
          break;
        }
      }
      lsa_free(rights);
    }

    if (!already_granted) {
      WCHAR right_buf[] = L"SeBatchLogonRight";
      NpLsaUnicodeString right_str{};
      right_str.Buffer = right_buf;
      right_str.Length = static_cast<USHORT>(wcslen(right_buf) * sizeof(WCHAR));
      right_str.MaximumLength =
          static_cast<USHORT>((wcslen(right_buf) + 1) * sizeof(WCHAR));
      if (lsa_add(policy_, sid_.data(), &right_str, 1) == 0) {
        granted_by_us_ = true;
      }
    }
  }

  ~BatchLogonRightGuard() {
    if (policy_ != nullptr) {
      if (granted_by_us_ && !sid_.empty() && lsa_remove_ != nullptr) {
        WCHAR right_buf[] = L"SeBatchLogonRight";
        NpLsaUnicodeString right_str{};
        right_str.Buffer = right_buf;
        right_str.Length =
            static_cast<USHORT>(wcslen(right_buf) * sizeof(WCHAR));
        right_str.MaximumLength =
            static_cast<USHORT>((wcslen(right_buf) + 1) * sizeof(WCHAR));
        lsa_remove_(policy_, sid_.data(), FALSE, &right_str, 1);
      }
      if (lsa_close_ != nullptr) {
        lsa_close_(policy_);
      }
    }
  }

  BatchLogonRightGuard(const BatchLogonRightGuard &) = delete;
  BatchLogonRightGuard &operator=(const BatchLogonRightGuard &) = delete;

private:
  PVOID policy_ = nullptr;
  LsaRemoveAccountRightsFn lsa_remove_ = nullptr;
  LsaCloseFn lsa_close_ = nullptr;
  std::vector<uint8_t> sid_;
  bool granted_by_us_ = false;
};

std::wstring GetSelfExePath() {
  WCHAR buf[MAX_PATH]{};
  const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : L"";
}

std::wstring GetSharedProbeDir() {
  WCHAR public_dir[MAX_PATH]{};
  DWORD len = ::GetEnvironmentVariableW(L"PUBLIC", public_dir, MAX_PATH);
  std::wstring base = (len > 0 && len < MAX_PATH)
                          ? std::wstring(public_dir, len)
                          : L"C:\\Users\\Public";
  const std::wstring probe_dir =
      base + L"\\DokanNpProbe_" + std::to_wstring(::GetCurrentProcessId());
  ::CreateDirectoryW(probe_dir.c_str(), nullptr);
  DWORD code = 0;
  std::wstring out;
  // Ensure non-admin secondary user probes can write output files here.
  RunProcessCapture(L"icacls.exe \"" + probe_dir +
                        L"\" /grant *S-1-5-32-545:(OI)(CI)F",
                    10000, &code, &out);
  return probe_dir;
}

bool RunScheduledProbeTask(const std::wstring &task_name,
                           const std::wstring &probe_dir,
                           const std::wstring &run_as_user,
                           const std::wstring &run_as_pass,
                           const std::wstring &drive_name,
                           const std::wstring &unc_name, bool expect_visible,
                           std::wstring *diag_out) {
  const std::wstring out_path = probe_dir + L"\\" + task_name + L".txt";
  ::DeleteFileW(out_path.c_str());

  const std::wstring exe = GetSelfExePath();
  // Escape inner quotes so schtasks /Create /TR "..." preserves them cleanly.
  std::wostringstream tr;
  tr << L"\\\"" << exe << L"\\\" --probe_mode --probe_drive=" << drive_name
     << L" --probe_unc=" << unc_name << L" --probe_expect_visible="
     << (expect_visible ? L"1" : L"0") << L" --probe_out=\\\"" << out_path
     << L"\\\"";

  std::wostringstream create_cmd;
  create_cmd << L"schtasks.exe /Create /TN \"" << task_name << L"\" /TR \""
             << tr.str() << L"\" /SC ONCE /ST 00:00 /F /RU \"" << run_as_user
             << L"\"";
  if (run_as_pass.empty()) {
    create_cmd << L" /RL HIGHEST";
  } else {
    create_cmd << L" /RP \"" << run_as_pass << L"\"";
  }

  DWORD code = 1;
  std::wstring out;
  const bool created =
      RunProcessCapture(create_cmd.str(), 15000, &code, &out) && code == 0;
  if (!created) {
    *diag_out =
        L"schtasks /Create failed (" + std::to_wstring(code) + L"): " + out;
    return false;
  }

  struct TaskGuard {
    std::wstring name;
    ~TaskGuard() {
      DWORD c = 0;
      std::wstring o;
      RunProcessCapture(L"schtasks.exe /Delete /TN \"" + name + L"\" /F", 10000,
                        &c, &o);
    }
  } guard{task_name};

  if (!RunProcessCapture(L"schtasks.exe /Run /TN \"" + task_name + L"\"", 15000,
                         &code, &out) ||
      code != 0) {
    *diag_out =
        L"schtasks /Run failed (" + std::to_wstring(code) + L"): " + out;
    return false;
  }

  const ULONGLONG deadline = ::GetTickCount64() + 20000ULL;
  while (::GetTickCount64() < deadline) {
    HANDLE hf =
        ::CreateFileW(out_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
      char buf[512]{};
      DWORD read_bytes = 0;
      ::ReadFile(hf, buf, sizeof(buf) - 1, &read_bytes, nullptr);
      ::CloseHandle(hf);
      ::DeleteFileW(out_path.c_str());
      const std::string text(buf, read_bytes);
      *diag_out = Utf8ToWide(text);
      return text.find("matched=1") != std::string::npos;
    }
    ::Sleep(150);
  }

  ::DeleteFileW(out_path.c_str());
  *diag_out = L"Timed out waiting for probe output file " + out_path;
  return false;
}

TEST(SessionIsolationTest,
     CurrentSessionMountIsolatedFromSession0AndAlternateUser) {
  if (!IsProcessElevated()) {
    GTEST_SKIP() << "Elevated rights required for Scheduled Task cross-session "
                    "probes";
  }

  MirrorMount session_mount;
  MirrorMount global_mount;
  std::wstring err;
  ASSERT_TRUE(session_mount.Start(L"dokantest_sess", L"per_session",
                                  /*enable_network_unmount=*/false,
                                  /*current_session_only=*/true, &err))
      << WideToUtf8(err);
  ASSERT_TRUE(global_mount.Start(L"dokantest_sess", L"global_share",
                                 /*enable_network_unmount=*/false,
                                 /*current_session_only=*/false, &err))
      << WideToUtf8(err);

  // Same-session verification: both mounts visible locally.
  {
    WCHAR buf[256]{};
    DWORD len = 256;
    EXPECT_EQ(
        ::WNetGetConnectionW(session_mount.drive_name().c_str(), buf, &len),
        static_cast<DWORD>(NO_ERROR));
    len = 256;
    EXPECT_EQ(
        ::WNetGetConnectionW(global_mount.drive_name().c_str(), buf, &len),
        static_cast<DWORD>(NO_ERROR));
  }

  const DWORD my_session = GetCurrentProcessSessionId();
  if (my_session == 0) {
    GTEST_SKIP() << "Already running in Session 0; cannot verify Session 1 vs "
                    "Session 0 isolation";
  }

  const std::wstring probe_dir = GetSharedProbeDir();
  struct ProbeDirGuard {
    std::wstring dir;
    ~ProbeDirGuard() { ::RemoveDirectoryW(dir.c_str()); }
  } dir_guard{probe_dir};

  // Session 0 (NT AUTHORITY\SYSTEM) must NOT see the /c per-session mount,
  // and MUST see the global mount.
  const std::wstring pid_str = std::to_wstring(::GetCurrentProcessId());
  std::wstring diag;
  EXPECT_TRUE(RunScheduledProbeTask(
      L"DokanNpProbeSysHidden_" + pid_str, probe_dir, L"NT AUTHORITY\\SYSTEM",
      L"", session_mount.drive_name(), session_mount.unc_name(),
      /*expect_visible=*/false, &diag))
      << WideToUtf8(diag);

  EXPECT_TRUE(RunScheduledProbeTask(
      L"DokanNpProbeSysGlobal_" + pid_str, probe_dir, L"NT AUTHORITY\\SYSTEM",
      L"", global_mount.drive_name(), global_mount.unc_name(),
      /*expect_visible=*/true, &diag))
      << WideToUtf8(diag);

  // If secondary user credentials are provided (e.g. dokan_ifstest in CI),
  // run Session-0 batch Scheduled Task probes under that user too.
  const TestConfig &cfg = GetTestConfig();
  if (!cfg.secondary_user.empty() && !cfg.secondary_pass.empty()) {
    BatchLogonRightGuard batch_guard(cfg.secondary_user);
    EXPECT_TRUE(RunScheduledProbeTask(
        L"DokanNpProbeUserHidden_" + pid_str, probe_dir, cfg.secondary_user,
        cfg.secondary_pass, session_mount.drive_name(),
        session_mount.unc_name(), /*expect_visible=*/false, &diag))
        << WideToUtf8(diag);

    EXPECT_TRUE(RunScheduledProbeTask(
        L"DokanNpProbeUserGlobal_" + pid_str, probe_dir, cfg.secondary_user,
        cfg.secondary_pass, global_mount.drive_name(), global_mount.unc_name(),
        /*expect_visible=*/true, &diag))
        << WideToUtf8(diag);
  }
}

} // namespace
} // namespace dokan_np_integration
