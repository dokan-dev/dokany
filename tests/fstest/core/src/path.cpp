// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/path.h"

#include "fstest/core/ntdll.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace fstest::core {
namespace {

bool IsAsciiAlpha(wchar_t c) noexcept {
  return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

bool StartsWithCaseInsensitive(std::wstring_view text,
                               std::wstring_view prefix) noexcept {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::towlower(text[i]) != std::towlower(prefix[i])) {
      return false;
    }
  }
  return true;
}

bool IsVolumeGuidRoot(std::wstring_view p) noexcept {
  // Matches \\?\Volume{...}\ or \\?\Volume{...} (no trailing path components).
  if (!StartsWithCaseInsensitive(p, L"\\\\?\\Volume{")) {
    return false;
  }
  const size_t close_brace = p.find(L'}', 11);
  if (close_brace == std::wstring_view::npos) {
    return false;
  }
  const size_t after = close_brace + 1;
  return after == p.size() || (after + 1 == p.size() && p[after] == L'\\');
}

size_t CountPathComponents(std::wstring_view tail) noexcept {
  size_t count = 0;
  size_t i = 0;
  while (i < tail.size()) {
    while (i < tail.size() && tail[i] == L'\\') {
      ++i;
    }
    if (i < tail.size()) {
      ++count;
      while (i < tail.size() && tail[i] != L'\\') {
        ++i;
      }
    }
  }
  return count;
}

bool IsAbsolutePath(std::wstring_view p) noexcept {
  if (p.size() >= 2 && IsAsciiAlpha(p[0]) && p[1] == L':') {
    return p.size() == 2 || p[2] == L'\\' || p[2] == L'/';
  }
  if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') &&
      (p[1] == L'\\' || p[1] == L'/')) {
    return true;
  }
  return false;
}

std::wstring EnsureTrailingBackslash(std::wstring_view p) {
  std::wstring out(p);
  if (!out.empty() && out.back() != L'\\') {
    out.push_back(L'\\');
  }
  return out;
}

std::optional<std::wstring> ResolveFullPath(std::wstring_view path) {
  std::wstring input(path);
  const DWORD needed = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    return std::nullopt;
  }
  std::wstring buf(needed, L'\0');
  const DWORD written =
      ::GetFullPathNameW(input.c_str(), needed, buf.data(), nullptr);
  if (written == 0 || written >= needed) {
    return std::nullopt;
  }
  buf.resize(written);
  return buf;
}

} // namespace

std::string_view RootKindToString(RootKind kind) noexcept {
  switch (kind) {
  case RootKind::kDriveLetter:
    return "drive_letter";
  case RootKind::kSubdirectory:
    return "subdirectory";
  case RootKind::kUncShare:
    return "unc_share";
  case RootKind::kUncSubdirectory:
    return "unc_subdirectory";
  case RootKind::kVolumeGuid:
    return "volume_guid";
  }
  return "unknown";
}

std::wstring NormalizeWin32Path(std::wstring_view raw_path) {
  if (raw_path.empty()) {
    return {};
  }

  // Extended verbatim paths (\\?\...): preserve verbatim characters, only
  // normalize trailing backslash on non-root paths and ensure trailing '\' on
  // Volume{guid} roots and drive roots.
  if (raw_path.size() >= 4 && raw_path[0] == L'\\' && raw_path[1] == L'\\' &&
      raw_path[2] == L'?' && raw_path[3] == L'\\') {
    std::wstring out(raw_path);
    if (IsVolumeGuidRoot(out)) {
      if (out.back() != L'\\') {
        out.push_back(L'\\');
      }
      return out;
    }
    if (out.size() == 6 && IsAsciiAlpha(out[4]) && out[5] == L':') {
      out.push_back(L'\\');
      return out;
    }
    if (out.size() == 7 && IsAsciiAlpha(out[4]) && out[5] == L':' &&
        out[6] == L'\\') {
      return out;
    }
    while (out.size() > 7 && out.back() == L'\\') {
      out.pop_back();
    }
    return out;
  }

  // Standard Win32 path: convert '/' -> '\' and collapse duplicate interior
  // backslashes while keeping leading '\\' for UNC paths.
  std::wstring out;
  out.reserve(raw_path.size() + 1);
  const bool is_unc =
      (raw_path.size() >= 2 && (raw_path[0] == L'\\' || raw_path[0] == L'/') &&
       (raw_path[1] == L'\\' || raw_path[1] == L'/'));
  size_t i = 0;
  if (is_unc) {
    out.append(L"\\\\");
    i = 2;
    while (i < raw_path.size() &&
           (raw_path[i] == L'\\' || raw_path[i] == L'/')) {
      ++i;
    }
  }
  for (; i < raw_path.size(); ++i) {
    wchar_t c = (raw_path[i] == L'/') ? L'\\' : raw_path[i];
    if (c == L'\\' && !out.empty() && out.back() == L'\\') {
      continue;
    }
    out.push_back(c);
  }

  // Normalize bare drive letter "M:" -> "M:\".
  if (out.size() == 2 && IsAsciiAlpha(out[0]) && out[1] == L':') {
    out.push_back(L'\\');
    return out;
  }

  // Keep drive root "M:\" intact; strip trailing '\' elsewhere.
  if (out.size() == 3 && IsAsciiAlpha(out[0]) && out[1] == L':' &&
      out[2] == L'\\') {
    return out;
  }
  while (out.size() > 3 && out.back() == L'\\') {
    out.pop_back();
  }
  return out;
}

RootKind ClassifyPathKind(std::wstring_view path) {
  const std::wstring norm = NormalizeWin32Path(path);
  if (norm.empty()) {
    return RootKind::kSubdirectory;
  }

  if (IsVolumeGuidRoot(norm)) {
    return RootKind::kVolumeGuid;
  }

  if (StartsWithCaseInsensitive(norm, L"\\\\?\\UNC\\")) {
    const size_t parts = CountPathComponents(std::wstring_view(norm).substr(8));
    return (parts <= 2) ? RootKind::kUncShare : RootKind::kUncSubdirectory;
  }

  if (norm.size() >= 7 && norm.rfind(L"\\\\?\\", 0) == 0 &&
      IsAsciiAlpha(norm[4]) && norm[5] == L':' && norm[6] == L'\\') {
    return (norm.size() == 7) ? RootKind::kDriveLetter
                              : RootKind::kSubdirectory;
  }

  if (norm.rfind(L"\\\\?\\", 0) == 0) {
    return RootKind::kSubdirectory;
  }

  if (norm.size() >= 2 && norm[0] == L'\\' && norm[1] == L'\\') {
    const size_t parts = CountPathComponents(std::wstring_view(norm).substr(2));
    return (parts <= 2) ? RootKind::kUncShare : RootKind::kUncSubdirectory;
  }

  if (norm.size() == 3 && IsAsciiAlpha(norm[0]) && norm[1] == L':' &&
      norm[2] == L'\\') {
    return RootKind::kDriveLetter;
  }

  return RootKind::kSubdirectory;
}

std::wstring ToExtendedWin32Path(std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
      path[2] == L'?' && path[3] == L'\\') {
    return std::wstring(path);
  }
  if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'?' &&
      path[2] == L'?' && path[3] == L'\\') {
    return L"\\\\?\\" + std::wstring(path.substr(4));
  }

  std::wstring norm = NormalizeWin32Path(path);
  if (!IsAbsolutePath(norm)) {
    if (auto full = ResolveFullPath(norm); full.has_value()) {
      norm = NormalizeWin32Path(*full);
    }
  }
  if (norm.rfind(L"\\\\?\\", 0) == 0) {
    return norm;
  }
  if (norm.size() >= 2 && norm[0] == L'\\' && norm[1] == L'\\') {
    return L"\\\\?\\UNC\\" + norm.substr(2);
  }
  return L"\\\\?\\" + norm;
}

std::wstring VerbatimToNtPath(std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'?' &&
      path[2] == L'?' && path[3] == L'\\') {
    return std::wstring(path);
  }
  if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
      path[2] == L'?' && path[3] == L'\\') {
    return L"\\??\\" + std::wstring(path.substr(4));
  }
  if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
    return L"\\??\\UNC\\" + std::wstring(path.substr(2));
  }
  if (path.size() == 2 && IsAsciiAlpha(path[0]) && path[1] == L':') {
    return L"\\??\\" + std::wstring(path) + L"\\";
  }
  if (path.size() >= 3 && IsAsciiAlpha(path[0]) && path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    std::wstring out = L"\\??\\";
    out.reserve(4 + path.size());
    for (wchar_t c : path) {
      out.push_back(c == L'/' ? L'\\' : c);
    }
    return out;
  }

  const std::wstring ext = ToExtendedWin32Path(path);
  if (ext.rfind(L"\\\\?\\", 0) == 0) {
    return L"\\??\\" + ext.substr(4);
  }
  return ext;
}

NtResult<std::wstring> DosPathToNtPath(std::wstring_view path) {
  NtResult<std::wstring> result{};
  const auto &ntdll = Ntdll();
  if (ntdll.RtlDosPathNameToNtPathName_U_WithStatus == nullptr ||
      ntdll.RtlFreeUnicodeString == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }

  std::wstring input(path);
  nt::UnicodeString nt_name{};
  result.status = ntdll.RtlDosPathNameToNtPathName_U_WithStatus(
      input.c_str(), &nt_name, nullptr, nullptr);
  if (nt::NtSuccess(result.status)) {
    if (nt_name.buffer != nullptr && nt_name.length > 0) {
      result.value.emplace(nt_name.buffer, nt_name.length / sizeof(wchar_t));
    } else {
      result.value.emplace();
    }
    ntdll.RtlFreeUnicodeString(&nt_name);
  }
  return result;
}

std::wstring JoinPath(std::wstring_view base, std::wstring_view rel) {
  if (base.empty()) {
    return std::wstring(rel);
  }
  if (rel.empty()) {
    return std::wstring(base);
  }
  while (!rel.empty() && rel.front() == L'\\') {
    rel.remove_prefix(1);
  }
  std::wstring out(base);
  if (out.back() != L'\\') {
    out.push_back(L'\\');
  }
  out.append(rel);
  return out;
}

bool DetectIsVolumeRoot(std::wstring_view normalized_win32_path) {
  const RootKind kind = ClassifyPathKind(normalized_win32_path);
  if (kind == RootKind::kDriveLetter || kind == RootKind::kVolumeGuid ||
      kind == RootKind::kUncShare) {
    return true;
  }
  if (kind == RootKind::kUncSubdirectory) {
    return false;
  }

  const std::wstring root_with_slash =
      EnsureTrailingBackslash(normalized_win32_path);
  std::vector<wchar_t> vol_buf(
      std::max<size_t>(root_with_slash.size() + 64, 1024), L'\0');
  if (::GetVolumePathNameW(root_with_slash.c_str(), vol_buf.data(),
                           static_cast<DWORD>(vol_buf.size())) != FALSE) {
    const std::wstring vol_norm =
        EnsureTrailingBackslash(NormalizeWin32Path(vol_buf.data()));
    const std::wstring root_norm =
        EnsureTrailingBackslash(NormalizeWin32Path(normalized_win32_path));
    return ::CompareStringOrdinal(root_norm.c_str(), -1, vol_norm.c_str(), -1,
                                  /*bIgnoreCase=*/TRUE) == CSTR_EQUAL;
  }

  return false;
}

Win32Result<NormalizedRoot>
NormalizeRootPath(std::wstring_view raw_path,
                  std::optional<bool> is_volume_root_override) {
  Win32Result<NormalizedRoot> result{};
  if (raw_path.empty()) {
    result.error = ERROR_INVALID_PARAMETER;
    return result;
  }

  // Always canonicalize non-verbatim input so `M:\a\..\b`, `M:\dir.` and
  // relative roots resolve to the directory the OS will actually open. A bare
  // `M:` means the drive root here (not the per-drive current directory).
  std::wstring resolved(raw_path);
  const bool is_verbatim = resolved.size() >= 4 && resolved[0] == L'\\' &&
                           (resolved[1] == L'\\' || resolved[1] == L'?') &&
                           resolved[2] == L'?' && resolved[3] == L'\\';
  if (resolved.size() == 2 && IsAsciiAlpha(resolved[0]) &&
      resolved[1] == L':') {
    resolved.push_back(L'\\');
  }
  if (!is_verbatim) {
    auto full = ResolveFullPath(resolved);
    if (!full.has_value() || full->empty()) {
      result.error = ::GetLastError();
      if (result.error == ERROR_SUCCESS) {
        result.error = ERROR_INVALID_NAME;
      }
      return result;
    }
    resolved = std::move(*full);
  }

  NormalizedRoot root{};
  root.original = std::wstring(raw_path);
  root.win32_root = NormalizeWin32Path(resolved);
  root.extended_root = ToExtendedWin32Path(root.win32_root);
  root.nt_root = VerbatimToNtPath(root.extended_root);
  root.kind = ClassifyPathKind(root.win32_root);
  root.is_volume_root = is_volume_root_override.has_value()
                            ? *is_volume_root_override
                            : DetectIsVolumeRoot(root.win32_root);

  result.error = ERROR_SUCCESS;
  result.value = std::move(root);
  return result;
}

DWORD ProbeRootDirectory(const NormalizedRoot &root) noexcept {
  const bool is_unc_share = root.kind == RootKind::kUncShare;
  std::wstring probe_path = root.extended_root;
  if (is_unc_share && !probe_path.empty() && probe_path.back() != L'\\') {
    probe_path.push_back(L'\\');
  }
  ::SetLastError(ERROR_SUCCESS);
  DWORD attrs = ::GetFileAttributesW(probe_path.c_str());
  DWORD err = attrs == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : 0;
  if (attrs == INVALID_FILE_ATTRIBUTES && is_unc_share) {
    std::wstring fallback = root.win32_root;
    if (!fallback.empty() && fallback.back() != L'\\') {
      fallback.push_back(L'\\');
    }
    ::SetLastError(ERROR_SUCCESS);
    attrs = ::GetFileAttributesW(fallback.c_str());
    err = attrs == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : 0;
  }
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return err != ERROR_SUCCESS ? err : ERROR_PATH_NOT_FOUND;
  }
  if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return ERROR_DIRECTORY;
  }
  return ERROR_SUCCESS;
}

} // namespace fstest::core
