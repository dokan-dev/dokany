// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#ifndef TESTS_NP_INTEGRATION_NP_TEST_ENV_H_
#define TESTS_NP_INTEGRATION_NP_TEST_ENV_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dokan.h"
#include <winioctl.h>
#include <winnetwk.h>
#include <npapi.h>

#include <string>
#include <vector>

namespace dokan_np_integration {

struct TestConfig {
  std::wstring mirror_exe;
  std::wstring dokannp_dll;
  std::wstring secondary_user;
  std::wstring secondary_pass;
  bool test_live_registry = false;
  bool skip_registry_roundtrip = false;
  bool skip_shell_check = false;

  // Subprocess / Scheduled Task probe mode for cross-session & multi-user tests.
  bool probe_mode = false;
  std::wstring probe_drive;
  std::wstring probe_unc;
  bool probe_expect_visible = false;
  std::wstring probe_out;
};

TestConfig &GetTestConfig();

bool ParseCliFlags(int *argc, char **argv, std::string *error_out);
int RunProbeMode();

std::wstring ResolveMirrorExe();
bool IsProcessElevated();
bool IsDesktopShellAvailable();
DWORD GetCurrentProcessSessionId();

std::wstring FormatWin32Error(DWORD err);
std::wstring Utf8ToWide(const std::string &s);
std::string WideToUtf8(const std::wstring &w);
void DeleteDirectoryRecursive(const std::wstring &dir);

struct EnumEntry {
  DWORD scope = 0;
  DWORD type = 0;
  DWORD display_type = 0;
  DWORD usage = 0;
  std::wstring local_name;
  std::wstring remote_name;
  std::wstring provider;
};

DWORD EnumWNet(DWORD scope, DWORD type, DWORD usage, LPNETRESOURCEW nr,
               std::vector<EnumEntry> *out);

// Dynamically resolves and invokes WNetFormatNetworkNameW from mpr.dll
// (exported at runtime by mpr.dll, omitted from public <winnetwk.h> / mpr.lib).
DWORD CallWNetFormatNetworkNameW(LPCWSTR lpProvider, LPCWSTR lpRemoteName,
                                 LPWSTR lpFormattedName, LPDWORD lpnLength,
                                 DWORD dwFlags, DWORD dwAveCharPerLine);

// Runs a command line and captures combined stdout/stderr + exit code.
bool RunProcessCapture(const std::wstring &cmd_line, DWORD timeout_ms,
                       DWORD *exit_code_out, std::wstring *output_out);

// Allocates a currently-unused drive letter (e.g. L'Z' down to L'K') that is
// also not already reserved by another active MirrorMount in this process.
WCHAR AllocateFreeDriveLetter();
void ReleaseDriveLetter(WCHAR letter);

// RAII wrapper around a live mirror.exe network mount.
class MirrorMount {
public:
  MirrorMount() = default;
  ~MirrorMount();

  MirrorMount(const MirrorMount &) = delete;
  MirrorMount &operator=(const MirrorMount &) = delete;

  bool Start(const std::wstring &server_name, const std::wstring &share_name,
             bool enable_network_unmount, bool current_session_only,
             std::wstring *error_out);
  void Stop();
  bool WaitForExit(DWORD timeout_ms);

  WCHAR drive_letter() const { return drive_letter_; }
  std::wstring drive_name() const {
    return drive_letter_ ? (std::wstring(1, drive_letter_) + L":") : L"";
  }
  std::wstring drive_root() const {
    return drive_letter_ ? (std::wstring(1, drive_letter_) + L":\\") : L"";
  }
  const std::wstring &unc_name() const { return unc_name_; }
  const std::wstring &backing_dir() const { return backing_dir_; }
  bool active() const { return process_ != nullptr; }

private:
  static bool SendDokanRelease(const std::wstring &mount_point);

  WCHAR drive_letter_ = 0;
  std::wstring unc_name_;
  std::wstring backing_dir_;
  HANDLE process_ = nullptr;
};

} // namespace dokan_np_integration

#endif // TESTS_NP_INTEGRATION_NP_TEST_ENV_H_
