// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file temp_dir.h
/// \brief Safe cleanup target guard, resilient non-following recursive tree
/// deletion, and isolated per-test temporary directory RAII management.

#pragma once

#include "fstest/core/status.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fstest::core {

/// \brief Retry and exponential-backoff settings for ForceRemoveAll.
///
/// Regardless of these values, each per-entry backoff step is capped at
/// 500 ms and the total sleep spent retrying one entry at 10 s.
struct CleanupOptions {
  uint32_t retries = 5; ///< Number of retries after initial failure.
  uint32_t initial_backoff_ms = 10; ///< Initial retry sleep duration in ms.
};

/// \brief Safety guard for CLI `--cleanup-dir=<path>` that also resolves the
/// canonical path to hand to ForceRemoveAll.
///
/// Non-`\\?\` input is always resolved with `GetFullPathNameW` (so `C:\x\.`
/// and `C:\x.` canonicalize to `C:\x` before any check). Rejects empty paths,
/// drive-relative paths (`C:.`, `C:Windows`), `..` components, `.`/`..`
/// components in extended `\\?\` paths, `\\?\GLOBALROOT\...`, `\\.\` device
/// paths, UNC administrative shares (`\\host\C$\...`, `\\?\UNC\host\C$\...`),
/// drive roots, volume GUID roots, UNC share roots and mounted volume roots.
///
/// When the target exists it is opened with
/// `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT` and must be a
/// non-reparse directory. The target and every protected directory
/// (`%SystemRoot%` and its `System32`/`SysWOW64`/`WinSxS`/`Boot` children,
/// `Program Files`, `Program Files (x86)`, `ProgramData`, `Users`, the user
/// profile, and `<drive>:\Windows`-style top-level OS folders) are
/// canonicalized via `GetFinalPathNameByHandleW(VOLUME_NAME_GUID)` and compared
/// case-insensitively, rejecting a target that equals or is an ancestor of a
/// protected directory (or lies inside `System32`-class directories). This
/// defeats 8.3 short names, `\\?\Volume{...}` aliases, and junction/symlink
/// ancestors.
/// \param path Candidate directory path to validate.
/// \param canonical_out Optional output. On success receives the canonical
/// `\\?\Volume{GUID}\...` path to pass to ForceRemoveAll (or the
/// `VOLUME_NAME_DOS` form `\\?\X:\...` / `\\?\UNC\...` when the volume has no
/// GUID path, e.g. network shares), or an empty string when the target does
/// not exist (nothing to delete).
/// \param reason_out Optional output receiving the rejection explanation.
/// \return True iff \p path is safe for recursive deletion.
bool ResolveSafeCleanupTarget(const std::wstring &path,
                              std::wstring *canonical_out,
                              std::string *reason_out);

/// \brief Thin wrapper over ResolveSafeCleanupTarget that discards the
/// canonical path.
/// \param path Candidate directory path to validate.
/// \param reason_out Optional output receiving the rejection explanation.
/// \return True iff \p path is safe for recursive deletion.
bool IsSafeCleanupTarget(std::wstring_view path,
                         std::string *reason_out = nullptr);

/// \brief Resilient non-following recursive deletion.
///
/// Operates in extended `\\?\` (or `\\?\UNC\`, or `\\?\Volume{GUID}\` as
/// returned by ResolveSafeCleanupTarget) form using an explicit heap work
/// stack (no recursion, so arbitrarily deep trees are safe), inspects
/// `FILE_ATTRIBUTE_REPARSE_POINT` before descending (never following junctions
/// or symlinks), clears `R`/`H`/`S` attributes, resets restrictive DACLs using
/// `FILE_OPEN_REPARSE_POINT`, handles case-distinct filenames inside
/// case-sensitive directories, retries transient sharing/lock errors with
/// exponential backoff, and verifies zero debris remains on disk.
/// \param path File or directory tree path to remove.
/// \param options Retry and backoff configuration.
/// \param error_out Optional diagnostic message on failure.
/// \return True iff \p path was completely removed or was already absent.
bool ForceRemoveAll(std::wstring_view path, const CleanupOptions &options = {},
                    std::string *error_out = nullptr);

/// \brief Isolated per-test temporary directory
/// (`<parent_dir>\<prefix>_<pid>_<seq>`) with explicit zero-debris Cleanup()
/// and best-effort RAII destructor.
class ScopedTempDir {
public:
  ScopedTempDir() noexcept = default;
  ~ScopedTempDir();

  ScopedTempDir(const ScopedTempDir &) = delete;
  ScopedTempDir &operator=(const ScopedTempDir &) = delete;

  ScopedTempDir(ScopedTempDir &&other) noexcept;
  ScopedTempDir &operator=(ScopedTempDir &&other);

  /// \brief Creates a uniquely named temporary directory under \p parent_dir.
  /// \param parent_dir Parent directory path.
  /// \param prefix Subdirectory name prefix (defaults to `"fst"`).
  /// \param cleanup_options Retry options used during teardown.
  /// \return Active ScopedTempDir on success, or Win32 error code.
  static Win32Result<ScopedTempDir> Create(std::wstring_view parent_dir,
                                           std::wstring_view prefix = L"fst",
                                           CleanupOptions cleanup_options = {});

  /// \brief Runs ForceRemoveAll and verifies zero debris remains on disk.
  /// \param error_out Optional diagnostic message on failure.
  /// \return True iff the directory was completely removed (or already clean).
  bool Cleanup(std::string *error_out = nullptr);

  /// \brief Returns the normalized Win32 path of the temp directory.
  const std::wstring &Path() const noexcept { return path_; }

  /// \brief Returns the extended `\\?\` path of the temp directory.
  const std::wstring &ExtendedPath() const noexcept { return extended_path_; }

  /// \brief Returns the leaf directory name (`<prefix>_<pid>_<seq>`).
  const std::wstring &Basename() const noexcept { return basename_; }

  /// \brief Returns true iff this directory has not yet been cleaned up.
  bool IsActive() const noexcept { return active_; }

  /// \brief Joins \p rel onto Path().
  std::wstring ChildPath(std::wstring_view rel) const;

  /// \brief Joins \p rel onto ExtendedPath().
  std::wstring ChildExtendedPath(std::wstring_view rel) const;

private:
  std::wstring path_;
  std::wstring extended_path_;
  std::wstring basename_;
  CleanupOptions cleanup_options_{};
  bool active_ = false;
};

} // namespace fstest::core
