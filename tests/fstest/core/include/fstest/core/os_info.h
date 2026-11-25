// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file os_info.h
/// \brief Host Windows OS version, architecture, privilege, and host-only
/// feature probing utilities.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fstest::core {

/// \name Named Windows OS Build Numbers
/// \{
inline constexpr uint32_t kBuildWin10_1607 = 14393; ///< Server 2016 / RS1.
inline constexpr uint32_t kBuildWin10_1703 =
    15063; ///< RS2 (unprivileged symlinks).
inline constexpr uint32_t kBuildWin10_1709 = 16299;     ///< RS3.
inline constexpr uint32_t kBuildWin10_1803 = 17134;     ///< RS4.
inline constexpr uint32_t kBuildWin10_1809 = 17763;     ///< Server 2019 / RS5.
inline constexpr uint32_t kBuildWin10_1903Prev = 18361; ///< Pre-19H1 threshold.
inline constexpr uint32_t kBuildWin10_1903 =
    18362; ///< 19H1 (POSIX `DeleteFileW`).
inline constexpr uint32_t kBuildServer2022 = 20348; ///< Server 2022.
inline constexpr uint32_t kBuildWin11_21H2 =
    22000; ///< Win11 21H2 (DOS-device path overhaul).
inline constexpr uint32_t kBuildWin11_24H2 = 26100; ///< Server 2025 / 24H2.
/// \}

/// \brief Windows CPU architecture identifier.
enum class Arch {
  kUnknown, ///< Unrecognized architecture.
  kX86,     ///< 32-bit x86 (Win32 / i386).
  kX64,     ///< 64-bit x86-64 (AMD64).
  kArm64,   ///< 64-bit ARM (ARM64 / AArch64).
};

/// \brief Converts an Arch enum value to its canonical lowercase string.
/// \param arch Architecture enum value.
/// \return `"x86"`, `"x64"`, `"arm64"`, or `"unknown"`.
std::string_view ArchToString(Arch arch) noexcept;

/// \brief Parses an architecture string (case-tolerant aliases such as
/// `"AMD64"` or `"Win32"`) into an Arch enum value.
/// \param text Architecture label to parse.
/// \return Parsed Arch or `std::nullopt` if unrecognized.
std::optional<Arch> ParseArch(std::string_view text) noexcept;

/// \brief Snapshot of host Windows version, architecture, token privileges,
/// and host-only environment capabilities.
struct OsInfo {
  uint32_t major = 0;              ///< OS major version (from `RtlGetVersion`).
  uint32_t minor = 0;              ///< OS minor version.
  uint32_t build = 0;              ///< OS build number.
  uint32_t ubr = 0;                ///< Update Build Revision (from registry).
  bool is_server = false;          ///< True on Windows Server SKUs.
  Arch host_arch = Arch::kUnknown; ///< Native OS architecture.
  Arch process_arch = Arch::kUnknown;  ///< Current process architecture.
  bool is_elevated = false;            ///< True if running elevated.
  bool has_security_privilege = false; ///< Holds `SeSecurityPrivilege`.
  bool has_backup_privilege = false;   ///< Holds `SeBackupPrivilege`.
  bool has_take_ownership_privilege =
      false;                        ///< Holds `SeTakeOwnershipPrivilege`.
  bool can_create_symlinks = false; ///< Host allows creating symbolic links.
  bool can_set_case_sensitive_dir =
      false;                  ///< Host NTFS supports per-dir case sensitivity.
  std::string version_string; ///< Formatted version summary string.
};

/// \brief Probes host OS version (via manifest-proof `RtlGetVersion`),
/// architecture (via `IsWow64Process2` with `GetNativeSystemInfo` fallback),
/// elevation, privileges, and host-only symlink/case-sensitivity probes
/// strictly under host `%TEMP%` (never on any test root).
/// \return Populated OsInfo snapshot.
OsInfo ProbeOsInfo();

/// \brief Returns the process-wide cached OsInfo probed once on first call.
/// \return Const reference to cached OsInfo.
const OsInfo &GetOsInfo();

} // namespace fstest::core
