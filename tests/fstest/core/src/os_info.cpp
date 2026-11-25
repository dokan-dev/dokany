// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/os_info.h"

#include "fstest/core/ntdll.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"

#include <cstring>
#include <sstream>

namespace fstest::core {
namespace {

Arch DetectProcessArch() noexcept {
#if defined(_M_AMD64) || defined(__x86_64__)
  return Arch::kX64;
#elif defined(_M_ARM64) || defined(__aarch64__)
  return Arch::kArm64;
#elif defined(_M_IX86) || defined(__i386__)
  return Arch::kX86;
#else
  return Arch::kUnknown;
#endif
}

Arch MachineToArch(USHORT machine) noexcept {
  switch (machine) {
  case nt::kImageFileMachineI386:
    return Arch::kX86;
  case nt::kImageFileMachineAmd64:
    return Arch::kX64;
  case nt::kImageFileMachineArm64:
    return Arch::kArm64;
  default:
    return Arch::kUnknown;
  }
}

Arch DetectHostArch() noexcept {
  using FnIsWow64Process2 = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
  HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
  if (kernel32 != nullptr) {
    FARPROC proc = ::GetProcAddress(kernel32, "IsWow64Process2");
    if (proc != nullptr) {
      FnIsWow64Process2 is_wow64_2 = nullptr;
      static_assert(sizeof(is_wow64_2) == sizeof(proc));
      std::memcpy(&is_wow64_2, &proc, sizeof(is_wow64_2));
      USHORT process_machine = 0;
      USHORT native_machine = 0;
      if (is_wow64_2(::GetCurrentProcess(), &process_machine,
                     &native_machine) != FALSE) {
        const Arch mapped = MachineToArch(native_machine);
        if (mapped != Arch::kUnknown) {
          return mapped;
        }
      }
    }
  }

  SYSTEM_INFO si{};
  ::GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture) {
  case PROCESSOR_ARCHITECTURE_INTEL:
    return Arch::kX86;
  case PROCESSOR_ARCHITECTURE_AMD64:
    return Arch::kX64;
  case nt::kProcessorArchitectureArm64:
    return Arch::kArm64;
  default:
    return DetectProcessArch();
  }
}

uint32_t ReadWindowsUbr() noexcept {
  DWORD ubr = 0;
  DWORD size = sizeof(ubr);
  const LSTATUS st = ::RegGetValueW(
      HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
      L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
  return (st == ERROR_SUCCESS) ? ubr : 0;
}

// Host-only symlink creation probe: checks SeCreateSymbolicLinkPrivilege on the
// process token first, then (on Win10 1703+ / build >= 15063) probes Windows
// Developer Mode strictly inside host %TEMP% (NEVER on any test root_path).
bool ProbeHostCanCreateSymlinks(uint32_t os_build) {
  auto has_priv = HasPrivilegeInToken(SE_CREATE_SYMBOLIC_LINK_NAME);
  if (has_priv && *has_priv.value) {
    return true;
  }

  if (os_build < kBuildWin10_1703) {
    return false;
  }

  wchar_t temp_buf[MAX_PATH + 1]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, temp_buf);
  if (len == 0 || len >= MAX_PATH) {
    return false;
  }

  const std::wstring probe_dir =
      JoinPath(std::wstring_view(temp_buf, len),
               L"fst_symprobe_" + std::to_wstring(::GetCurrentProcessId()) +
                   L"_" + std::to_wstring(::GetTickCount64()));
  if (::CreateDirectoryW(probe_dir.c_str(), nullptr) == FALSE) {
    return false;
  }

  const std::wstring link_path = JoinPath(probe_dir, L"probe_link");
  const std::wstring target_path = JoinPath(probe_dir, L"probe_target");
  const DWORD flags = nt::kSymbolicLinkFlagAllowUnprivilegedCreate;

  ::SetLastError(ERROR_SUCCESS);
  const BOOLEAN ok =
      ::CreateSymbolicLinkW(link_path.c_str(), target_path.c_str(), flags);

  if (ok != 0) {
    ::DeleteFileW(link_path.c_str());
    ::RemoveDirectoryW(probe_dir.c_str());
    return true;
  }

  ::RemoveDirectoryW(probe_dir.c_str());
  return false;
}

// Host-only per-directory case-sensitivity probe: on Windows 10/Server builds
// without WSL / NtfsCaseSensitiveDirectoriesEnabled, ntfs.sys returns
// STATUS_NOT_SUPPORTED even for elevated admins. Probe strictly inside host
// %TEMP% (NEVER on any test root_path) so target-FS failures are never masked.
bool ProbeHostCanSetCaseSensitiveDir(uint32_t os_build, bool is_elevated) {
  if (os_build < kBuildWin10_1809 || !is_elevated) {
    return false;
  }
  const auto &ntdll = Ntdll();
  if (ntdll.NtSetInformationFile == nullptr) {
    return false;
  }

  wchar_t temp_buf[MAX_PATH + 1]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, temp_buf);
  if (len == 0 || len >= MAX_PATH) {
    return false;
  }

  const std::wstring probe_dir =
      JoinPath(std::wstring_view(temp_buf, len),
               L"fst_csprobe_" + std::to_wstring(::GetCurrentProcessId()) +
                   L"_" + std::to_wstring(::GetTickCount64()));
  if (::CreateDirectoryW(probe_dir.c_str(), nullptr) == FALSE) {
    return false;
  }

  bool supported = false;
  UniqueHandle dh(::CreateFileW(
      probe_dir.c_str(), FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (dh) {
    nt::FileCaseSensitiveInfo cs_info{nt::kFileCsFlagCaseSensitiveDir};
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtSetInformationFile(
        dh.get(), &iosb, &cs_info, sizeof(cs_info),
        nt::FileInfoClass::FileCaseSensitiveInformation);
    supported = nt::NtSuccess(st);
    dh.reset();
  }
  ::RemoveDirectoryW(probe_dir.c_str());
  return supported;
}

} // namespace

std::string_view ArchToString(Arch arch) noexcept {
  switch (arch) {
  case Arch::kX86:
    return "x86";
  case Arch::kX64:
    return "x64";
  case Arch::kArm64:
    return "arm64";
  case Arch::kUnknown:
    return "unknown";
  }
  return "unknown";
}

std::optional<Arch> ParseArch(std::string_view text) noexcept {
  if (text == "x64" || text == "X64" || text == "amd64" || text == "AMD64") {
    return Arch::kX64;
  }
  if (text == "x86" || text == "X86" || text == "win32" || text == "Win32") {
    return Arch::kX86;
  }
  if (text == "arm64" || text == "ARM64") {
    return Arch::kArm64;
  }
  return std::nullopt;
}

OsInfo ProbeOsInfo() {
  OsInfo info{};
  RTL_OSVERSIONINFOEXW osvi{};
  osvi.dwOSVersionInfoSize = sizeof(osvi);

  const auto &ntdll = Ntdll();
  if (ntdll.RtlGetVersion != nullptr &&
      nt::NtSuccess(ntdll.RtlGetVersion(&osvi))) {
    info.major = osvi.dwMajorVersion;
    info.minor = osvi.dwMinorVersion;
    info.build = osvi.dwBuildNumber;
    info.is_server = (osvi.wProductType != VER_NT_WORKSTATION);
  }
  info.ubr = ReadWindowsUbr();
  info.host_arch = DetectHostArch();
  info.process_arch = DetectProcessArch();

  if (auto elev = IsCurrentProcessElevated(); elev) {
    info.is_elevated = *elev.value;
  }
  if (auto sec = HasPrivilegeInToken(SE_SECURITY_NAME); sec) {
    info.has_security_privilege = *sec.value;
  }
  if (auto bak = HasPrivilegeInToken(SE_BACKUP_NAME); bak) {
    info.has_backup_privilege = *bak.value;
  }
  if (auto own = HasPrivilegeInToken(SE_TAKE_OWNERSHIP_NAME); own) {
    info.has_take_ownership_privilege = *own.value;
  }
  info.can_create_symlinks = ProbeHostCanCreateSymlinks(info.build);
  info.can_set_case_sensitive_dir =
      ProbeHostCanSetCaseSensitiveDir(info.build, info.is_elevated);

  std::ostringstream oss;
  oss << "Windows " << (info.is_server ? "Server " : "") << info.major << "."
      << info.minor << "." << info.build;
  if (info.ubr > 0) {
    oss << "." << info.ubr;
  }
  info.version_string = oss.str();
  return info;
}

const OsInfo &GetOsInfo() {
  static const OsInfo kInfo = ProbeOsInfo();
  return kInfo;
}

} // namespace fstest::core
