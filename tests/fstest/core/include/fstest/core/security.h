// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file security.h
/// \brief SDDL conversion, file security descriptors, token privilege queries,
/// impersonation token helpers, and teardown DACL recovery utilities.

#pragma once

#include "fstest/core/handle.h"
#include "fstest/core/status.h"

#include <string>
#include <string_view>

namespace fstest::core {

/// \brief Converts an SDDL string into a binary self-relative
/// `SECURITY_DESCRIPTOR` allocated via `LocalAlloc`.
/// \param sddl Input SDDL string.
/// \return Owned `SECURITY_DESCRIPTOR` buffer on success, or Win32 error.
Win32Result<UniqueLocalMem<void>>
SddlToSecurityDescriptor(std::wstring_view sddl);

/// \brief Converts a binary `SECURITY_DESCRIPTOR` to its SDDL string form.
/// \param sd Pointer to the binary security descriptor.
/// \param info `SECURITY_INFORMATION` bitmask selecting components to format.
/// \return SDDL string on success, or Win32 error.
Win32Result<std::wstring> SecurityDescriptorToSddl(PSECURITY_DESCRIPTOR sd,
                                                   SECURITY_INFORMATION info);

/// \brief Queries a file or directory's security descriptor as an SDDL string.
/// \param path File or directory path.
/// \param info Exact `SECURITY_INFORMATION` bitmask to query.
/// \return SDDL string on success, or Win32 error.
Win32Result<std::wstring> GetFileSddl(std::wstring_view path,
                                      SECURITY_INFORMATION info);

/// \brief Applies an SDDL string to a file or directory using the exact
/// requested `SECURITY_INFORMATION` mask.
/// \param path File or directory path.
/// \param info `SECURITY_INFORMATION` bitmask to set.
/// \param sddl SDDL string to parse and apply.
/// \return Success or Win32 error.
Win32Result<void> SetFileSddl(std::wstring_view path, SECURITY_INFORMATION info,
                              std::wstring_view sddl);

/// \brief Returns a `LocalAlloc`-owned copy of the current process token's
/// user SID.
/// \return Owned SID buffer on success, or Win32 error.
Win32Result<UniqueLocalMem<void>> GetCurrentUserSid();

/// \brief Returns the current process user SID formatted as a string (e.g.,
/// `S-1-5-21-...`).
/// \return User SID string on success, or Win32 error.
Win32Result<std::wstring> GetCurrentUserSidString();

/// \brief Queries whether the current process token is elevated.
/// \return True if elevated, false otherwise, or Win32 error.
Win32Result<bool> IsCurrentProcessElevated();

/// \brief Checks whether \p privilege_name is present in the current process
/// token (regardless of whether it is currently enabled).
/// \param privilege_name Win32 privilege name (e.g., `SE_SECURITY_NAME`).
/// \return True if present in token, false otherwise, or Win32 error.
Win32Result<bool> HasPrivilegeInToken(const wchar_t *privilege_name);

/// \brief Duplicates the current process token into an impersonation token
/// with \p privilege_name permanently removed (e.g., `SE_CHANGE_NOTIFY_NAME`
/// for `FILE_TRAVERSE` testing with ScopedImpersonation).
/// \param privilege_name Win32 privilege name to strip from the duplicate.
/// \return Impersonation token handle on success, or Win32 error.
Win32Result<UniqueHandle>
CreateTokenWithoutPrivilege(const wchar_t *privilege_name);

/// \brief Resets the DACL of an existing file, directory, or reparse point to
/// grant full control to the current user (attempting
/// `SeTakeOwnershipPrivilege` fallback on `STATUS_ACCESS_DENIED`).
///
/// The fallback enables `SeTakeOwnership`/`SeRestore`/`SeBackup` only on a
/// private thread impersonation token (`ImpersonateSelf`), never on the
/// process token, and always reverts before returning.
/// \param extended_path Extended `\\?\` (or `\\?\UNC\`) target path.
/// \param is_reparse_point When true, opens with `FILE_OPEN_REPARSE_POINT` so
/// symlinks/junctions are never followed to external targets.
/// \return Success or Win32 error.
Win32Result<void> ResetDaclToFullControl(std::wstring_view extended_path,
                                         bool is_reparse_point);

/// \brief RAII guard that pre-opens a pure `WRITE_DAC` handle (no data/delete
/// share conflict) before a test applies a restrictive DACL (such as `D:P` or
/// deny-read).
///
/// On scope exit, if still armed, restores a permissive DACL via the open
/// handle so per-test teardown succeeds even on filesystems that reject
/// subsequent path-based `WRITE_DAC` re-opens.
class ScopedDaclRestore {
public:
  ScopedDaclRestore() noexcept = default;
  ~ScopedDaclRestore();

  ScopedDaclRestore(const ScopedDaclRestore &) = delete;
  ScopedDaclRestore &operator=(const ScopedDaclRestore &) = delete;
  ScopedDaclRestore(ScopedDaclRestore &&other) noexcept;
  ScopedDaclRestore &operator=(ScopedDaclRestore &&other) noexcept;

  /// \brief Opens a pure `WRITE_DAC` handle to \p nt_path (with
  /// `FILE_OPEN_REPARSE_POINT`, so a junction/symlink itself is guarded rather
  /// than its target) and prepares
  /// \p restore_sddl for automatic restoration on scope exit.
  /// \param nt_path Native NT path (`\??\...`) to guard.
  /// \param restore_sddl Permissive SDDL to restore on exit.
  /// \return Armed guard on success, or an unarmed guard if setup failed.
  static ScopedDaclRestore
  Arm(std::wstring_view nt_path,
      std::wstring_view restore_sddl = L"D:P(A;;FA;;;WD)");

  /// \brief Returns true iff this guard holds an open `WRITE_DAC` handle and
  /// will restore on destruction.
  bool armed() const noexcept { return armed_; }

  /// \brief Immediately restores the permissive DACL (if armed) and closes the
  /// handle.
  void RestoreNow() noexcept;

  /// \brief Disarms the guard without restoring the DACL and closes the handle.
  void DisarmAndClose() noexcept;

private:
  UniqueHandle handle_;
  UniqueLocalMem<void> restore_sd_;
  bool armed_ = false;
};

} // namespace fstest::core
