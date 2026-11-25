// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file path.h
/// \brief Win32, extended-length (`\\?\`), and NT (`\??\`) path normalization,
/// classification, and root resolution utilities.

#pragma once

#include "fstest/core/status.h"

#include <optional>
#include <string>
#include <string_view>

namespace fstest::core {

/// \brief Classification of a normalized filesystem root path.
enum class RootKind {
  kDriveLetter,     ///< Local drive root (`X:\` or `\\?\X:\`).
  kSubdirectory,    ///< Local subdirectory (`X:\dir`, `\\?\Volume{...}\sub`).
  kUncShare,        ///< UNC share root (`\\server\share`).
  kUncSubdirectory, ///< Subdirectory under a UNC share (`\\server\share\sub`).
  kVolumeGuid,      ///< Volume GUID root (`\\?\Volume{guid}\`).
};

/// \brief Returns the canonical lowercase string label for \p kind.
/// \param kind RootKind value.
/// \return String label such as `"drive_letter"` or `"unc_share"`.
std::string_view RootKindToString(RootKind kind) noexcept;

/// \brief Returns true iff \p kind represents a UNC share or UNC subdirectory.
inline constexpr bool IsUncKind(RootKind kind) noexcept {
  return kind == RootKind::kUncShare || kind == RootKind::kUncSubdirectory;
}

/// \brief Returns true iff \p kind represents a local (non-UNC) path.
inline constexpr bool IsLocalKind(RootKind kind) noexcept {
  return !IsUncKind(kind);
}

/// \brief Fully normalized representation of a test target root path across
/// Win32, extended-length, and NT namespaces.
struct NormalizedRoot {
  std::wstring original;   ///< Raw caller-supplied root path.
  std::wstring win32_root; ///< Normalized standard Win32 path.
  std::wstring
      extended_root;    ///< Extended verbatim `\\?\` (or `\\?\UNC\`) path.
  std::wstring nt_root; ///< Native NT `\??\` (or `\??\UNC\`) path.
  RootKind kind = RootKind::kSubdirectory; ///< Classified path kind.
  bool is_volume_root = false; ///< True if path is a volume or share root.
};

/// \brief Performs pure string normalization on a Win32 or extended path.
///
/// Converts `'/'` to `'\'`, collapses duplicate interior backslashes (while
/// preserving leading `'\\'` for UNC/device paths), normalizes `'X:'` to
/// `'X:\'`, and strips trailing backslashes except on drive roots (`X:\`),
/// extended drive roots (`\\?\X:\`), and volume GUID roots
/// (`\\?\Volume{...}\`).
/// \param raw_path Input path string.
/// \return Normalized path string.
std::wstring NormalizeWin32Path(std::wstring_view raw_path);

/// \brief Classifies a raw or normalized path into its RootKind.
/// \param path Input path string.
/// \return Classified RootKind.
RootKind ClassifyPathKind(std::wstring_view path);

/// \brief Converts a Win32 path into extended-length (`\\?\` / `\\?\UNC\`)
/// form.
/// \param path Input path string.
/// \return Extended-length Win32 path.
std::wstring ToExtendedWin32Path(std::wstring_view path);

/// \brief Converts a Win32 or extended path verbatim into an NT `\??\` path
/// without stripping trailing dots/spaces or rewriting DOS device names.
/// \param path Input path string.
/// \return Native NT namespace path.
std::wstring VerbatimToNtPath(std::wstring_view path);

/// \brief Converts a DOS path to an NT path using
/// `ntdll!RtlDosPathNameToNtPathName_U_WithStatus` (standard Win32 path
/// normalization semantics).
/// \param path Input Win32 path string.
/// \return NT path on success, or the failing NTSTATUS.
NtResult<std::wstring> DosPathToNtPath(std::wstring_view path);

/// \brief Joins a base directory path and relative path using a single
/// backslash, preserving trailing dots/spaces in \p rel verbatim.
/// \param base Base directory path.
/// \param rel Relative child path.
/// \return Joined path string.
std::wstring JoinPath(std::wstring_view base, std::wstring_view rel);

/// \brief Detects whether \p normalized_win32_path is a volume or mount root
/// using `GetVolumePathNameW` (with UNC component-count fallback).
/// \param normalized_win32_path Normalized Win32 path.
/// \return True if the path is a volume, mount, or UNC share root.
bool DetectIsVolumeRoot(std::wstring_view normalized_win32_path);

/// \brief Normalizes a target `--root` path, classifies its kind, computes
/// extended and NT forms, and resolves `is_volume_root`.
/// \param raw_path Raw target path.
/// \param is_volume_root_override Optional explicit override for
/// `is_volume_root`.
/// \return NormalizedRoot on success, or Win32 error code on failure.
Win32Result<NormalizedRoot>
NormalizeRootPath(std::wstring_view raw_path,
                  std::optional<bool> is_volume_root_override = std::nullopt);

/// \brief Probes that a normalized root is reachable and is a directory.
///
/// Queries `GetFileAttributesW` on `extended_root` (with a trailing `\` for
/// UNC share roots). For UNC share roots it falls back to `win32_root` with a
/// trailing `\` when the extended probe fails.
/// \param root Normalized root returned by NormalizeRootPath.
/// \return `ERROR_SUCCESS` when the root is a reachable directory,
/// `ERROR_DIRECTORY` when it exists but is not a directory, otherwise the
/// `GetLastError()` value of the failed probe.
DWORD ProbeRootDirectory(const NormalizedRoot &root) noexcept;

} // namespace fstest::core
