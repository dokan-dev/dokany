// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/temp_dir.h"

#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <set>
#include <utility>
#include <vector>

namespace fstest::core {
namespace {

std::atomic<uint32_t> g_temp_dir_seq{0};

// Per-step backoff cap and total per-entry sleep budget applied on top of the
// caller-supplied CleanupOptions so huge `retries`/`initial_backoff_ms` values
// cannot stall teardown indefinitely.
constexpr DWORD kMaxBackoffMs = 500;
constexpr uint64_t kMaxTotalRetrySleepMs = 10000;

// Parent index of the root entry in RemoveTree's work stack.
constexpr size_t kNoParent = static_cast<size_t>(-1);

bool IsNotFoundError(DWORD err) noexcept {
  return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

bool IsRetryableDeleteError(DWORD err) noexcept {
  return err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED ||
         err == ERROR_DIR_NOT_EMPTY || err == ERROR_LOCK_VIOLATION;
}

bool IsSameOrSubpathIgnoreCase(std::wstring_view candidate,
                               std::wstring_view ancestor) noexcept {
  if (ancestor.empty() || candidate.size() < ancestor.size()) {
    return false;
  }
  if (!EqualsIgnoreCase(candidate.substr(0, ancestor.size()), ancestor)) {
    return false;
  }
  return candidate.size() == ancestor.size() ||
         candidate[ancestor.size()] == L'\\';
}

std::wstring GetEnvVarW(const wchar_t *name) {
  const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::wstring value(needed, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
  if (written == 0 || written >= needed) {
    return {};
  }
  value.resize(written);
  return value;
}

bool IsAsciiAlpha(wchar_t c) noexcept {
  return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

bool IsExtendedPrefix(std::wstring_view path) noexcept {
  return path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
         path[2] == L'?' && path[3] == L'\\';
}

bool IsDriveRelativePath(std::wstring_view path) noexcept {
  return path.size() >= 2 && IsAsciiAlpha(path[0]) && path[1] == L':' &&
         (path.size() == 2 || (path[2] != L'\\' && path[2] != L'/'));
}

std::wstring StripExtendedPrefixForCheck(std::wstring_view path) {
  std::wstring norm = NormalizeWin32Path(path);
  if (norm.size() >= 8 &&
      EqualsIgnoreCase(std::wstring_view(norm).substr(0, 8), L"\\\\?\\UNC\\")) {
    return L"\\\\" + norm.substr(8);
  }
  if (norm.size() >= 6 && norm.rfind(L"\\\\?\\", 0) == 0 && norm[5] == L':') {
    return NormalizeWin32Path(norm.substr(4));
  }
  return norm;
}

bool ContainsDotComponent(std::wstring_view raw_path,
                          bool include_single_dot) noexcept {
  size_t i = 0;
  while (i < raw_path.size()) {
    while (i < raw_path.size() &&
           (raw_path[i] == L'\\' || raw_path[i] == L'/')) {
      ++i;
    }
    const size_t start = i;
    while (i < raw_path.size() && raw_path[i] != L'\\' && raw_path[i] != L'/') {
      ++i;
    }
    const size_t len = i - start;
    if ((include_single_dot && len == 1 && raw_path[start] == L'.') ||
        (len == 2 && raw_path[start] == L'.' && raw_path[start + 1] == L'.')) {
      return true;
    }
  }
  return false;
}

bool StartsWithIgnoreCase(std::wstring_view text,
                          std::wstring_view prefix) noexcept {
  return text.size() >= prefix.size() &&
         EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool IsPathSeparator(wchar_t c) noexcept { return c == L'\\' || c == L'/'; }

// True for device-namespace paths that ForceRemoveAll must never be pointed
// at: `\\.\...`, NT `\??\...`, `//?/...` (non-canonical separators), and any
// `\\?\` path other than `\\?\X:\...`, `\\?\UNC\...` or `\\?\Volume{...}\...`
// (e.g. `\\?\GLOBALROOT\Device\HarddiskVolume1\...`).
bool IsDeviceNamespacePath(std::wstring_view p) noexcept {
  if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'?' && p[2] == L'?' &&
      p[3] == L'\\') {
    return true;
  }
  if (p.size() < 4 || !IsPathSeparator(p[0]) || !IsPathSeparator(p[1]) ||
      !IsPathSeparator(p[3])) {
    return false;
  }
  if (p[2] == L'.') {
    return true;
  }
  if (p[2] != L'?') {
    return false;
  }
  if (!IsExtendedPrefix(p)) {
    return true;
  }
  const std::wstring_view rest = p.substr(4);
  if (rest.size() >= 2 && IsAsciiAlpha(rest[0]) && rest[1] == L':' &&
      (rest.size() == 2 || rest[2] == L'\\')) {
    return false;
  }
  return !StartsWithIgnoreCase(rest, L"UNC\\") &&
         !StartsWithIgnoreCase(rest, L"Volume{");
}

// True for UNC administrative shares (`\\host\C$\...`, `\\host\ADMIN$\...`,
// `\\host\IPC$`, and their `\\?\UNC\host\...` forms), which alias whole local
// volumes and system directories behind a remote-looking path.
bool IsUncAdminShare(std::wstring_view p) noexcept {
  std::wstring_view rest;
  if (StartsWithIgnoreCase(p, L"\\\\?\\UNC\\")) {
    rest = p.substr(8);
  } else if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\' &&
             !IsExtendedPrefix(p)) {
    rest = p.substr(2);
  } else {
    return false;
  }
  while (!rest.empty() && rest.front() == L'\\') {
    rest.remove_prefix(1);
  }
  const size_t host_end = rest.find(L'\\');
  if (host_end == std::wstring_view::npos) {
    return false;
  }
  std::wstring_view share = rest.substr(host_end + 1);
  while (!share.empty() && share.front() == L'\\') {
    share.remove_prefix(1);
  }
  share = share.substr(0, share.find(L'\\'));
  if (share.size() == 2 && IsAsciiAlpha(share[0]) && share[1] == L'$') {
    return true;
  }
  return EqualsIgnoreCase(share, L"ADMIN$") || EqualsIgnoreCase(share, L"IPC$");
}

DWORD GetFullPathNameString(const std::wstring &input, std::wstring *out) {
  ::SetLastError(ERROR_SUCCESS);
  const DWORD needed = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    const DWORD err = ::GetLastError();
    return err != ERROR_SUCCESS ? err : ERROR_INVALID_NAME;
  }
  std::wstring full(needed, L'\0');
  ::SetLastError(ERROR_SUCCESS);
  const DWORD written =
      ::GetFullPathNameW(input.c_str(), needed, full.data(), nullptr);
  if (written == 0 || written >= needed) {
    const DWORD err = ::GetLastError();
    return err != ERROR_SUCCESS ? err : ERROR_INVALID_NAME;
  }
  full.resize(written);
  *out = std::move(full);
  return ERROR_SUCCESS;
}

/// Canonical identity of an opened directory in both the volume-GUID and DOS
/// namespaces (either may be empty: network files have no volume GUID path).
struct CanonicalDir {
  DWORD error = ERROR_SUCCESS;
  DWORD attrs = 0;
  std::wstring guid_path;
  std::wstring dos_path;
};

CanonicalDir OpenCanonicalDirectory(const std::wstring &path,
                                    bool open_reparse_point) {
  CanonicalDir info;
  DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
  if (open_reparse_point) {
    flags |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  const HANDLE raw =
      ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, flags, nullptr);
  const DWORD open_err = ::GetLastError();
  UniqueHandle h(raw);
  if (!h) {
    info.error = open_err != ERROR_SUCCESS ? open_err : ERROR_INVALID_HANDLE;
    return info;
  }
  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (::GetFileInformationByHandleEx(h.get(), FileAttributeTagInfo, &tag_info,
                                     sizeof(tag_info)) == FALSE) {
    const DWORD err = ::GetLastError();
    info.error = err != ERROR_SUCCESS ? err : ERROR_INVALID_FUNCTION;
    return info;
  }
  info.attrs = tag_info.FileAttributes;
  auto guid_res = fstest::core::QueryFinalPath(h.get(), FILE_NAME_NORMALIZED |
                                                            VOLUME_NAME_GUID);
  if (guid_res) {
    info.guid_path = std::move(*guid_res.value);
  }
  auto dos_res = fstest::core::QueryFinalPath(h.get(), FILE_NAME_NORMALIZED |
                                                           VOLUME_NAME_DOS);
  if (dos_res) {
    info.dos_path = std::move(*dos_res.value);
  }
  if (info.guid_path.empty() && info.dos_path.empty()) {
    const DWORD err =
        dos_res.error != ERROR_SUCCESS ? dos_res.error : guid_res.error;
    info.error = err != ERROR_SUCCESS ? err : ERROR_INVALID_NAME;
  }
  return info;
}

// Returns true iff some canonical form of `inner` equals or lies below some
// canonical form of `outer`.
bool CanonicalSameOrSubpath(const CanonicalDir &inner,
                            const CanonicalDir &outer) noexcept {
  const std::wstring *inner_forms[] = {&inner.guid_path, &inner.dos_path};
  const std::wstring *outer_forms[] = {&outer.guid_path, &outer.dos_path};
  for (const std::wstring *i : inner_forms) {
    for (const std::wstring *o : outer_forms) {
      if (!i->empty() && IsSameOrSubpathIgnoreCase(*i, *o)) {
        return true;
      }
    }
  }
  return false;
}

// Returns the path below the volume prefix of `\\?\Volume{...}\tail`,
// `\\?\X:\tail` or `X:\tail`, or empty for any other form.
std::wstring_view TailAfterVolumePrefix(std::wstring_view p) noexcept {
  if (StartsWithIgnoreCase(p, L"\\\\?\\Volume{")) {
    const size_t close = p.find(L"}\\");
    return close == std::wstring_view::npos ? std::wstring_view{}
                                            : p.substr(close + 2);
  }
  if (IsExtendedPrefix(p)) {
    p.remove_prefix(4);
  }
  if (p.size() >= 3 && IsAsciiAlpha(p[0]) && p[1] == L':' && p[2] == L'\\') {
    return p.substr(3);
  }
  return {};
}

constexpr std::wstring_view kProtectedTopLevelNames[] = {
    L"Windows",     L"Users", L"Program Files", L"Program Files (x86)",
    L"ProgramData",
};

bool IsProtectedTopLevelName(std::wstring_view tail) noexcept {
  for (const std::wstring_view name : kProtectedTopLevelNames) {
    if (EqualsIgnoreCase(tail, name)) {
      return true;
    }
  }
  return false;
}

std::wstring GetSystemWindowsDirectoryString() {
  wchar_t buf[MAX_PATH + 1]{};
  const UINT n = ::GetSystemWindowsDirectoryW(buf, MAX_PATH + 1);
  return (n > 0 && n <= MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

std::wstring GetSystemDirectoryString() {
  wchar_t buf[MAX_PATH + 1]{};
  const UINT n = ::GetSystemDirectoryW(buf, MAX_PATH + 1);
  return (n > 0 && n <= MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

struct ProtectedDir {
  std::wstring path;
  bool forbid_descendants = false; ///< Also reject anything below `path`.
};

std::vector<ProtectedDir> BuildProtectedDirs() {
  std::vector<ProtectedDir> dirs;
  auto add = [&dirs](std::wstring_view path, bool forbid_descendants) {
    std::wstring norm = NormalizeWin32Path(path);
    if (!norm.empty()) {
      dirs.push_back(ProtectedDir{std::move(norm), forbid_descendants});
    }
  };

  const std::wstring windows_dirs[] = {
      GetEnvVarW(L"SystemRoot"),
      GetEnvVarW(L"windir"),
      GetSystemWindowsDirectoryString(),
  };
  for (const std::wstring &win : windows_dirs) {
    if (win.empty()) {
      continue;
    }
    add(win, /*forbid_descendants=*/false);
    for (const wchar_t *child :
         {L"System32", L"SysWOW64", L"WinSxS", L"Boot"}) {
      add(JoinPath(win, child), /*forbid_descendants=*/true);
    }
  }
  add(GetSystemDirectoryString(), /*forbid_descendants=*/true);

  for (const wchar_t *env_name :
       {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramData",
        L"USERPROFILE", L"ALLUSERSPROFILE", L"PUBLIC"}) {
    add(GetEnvVarW(env_name), /*forbid_descendants=*/false);
  }

  const std::wstring system_drive = GetEnvVarW(L"SystemDrive");
  if (system_drive.size() == 2 && IsAsciiAlpha(system_drive[0]) &&
      system_drive[1] == L':') {
    for (const std::wstring_view name : kProtectedTopLevelNames) {
      add(system_drive + L"\\" + std::wstring(name),
          /*forbid_descendants=*/false);
    }
  }
  return dirs;
}

bool DeleteSingleEntryWithRetry(const std::wstring &ext_path, bool is_dir,
                                bool child_failed,
                                const CleanupOptions &options,
                                std::string *error_out) {
  constexpr DWORD kClearMask =
      FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;

  DWORD backoff_ms =
      std::clamp<DWORD>(options.initial_backoff_ms, 1, kMaxBackoffMs);
  // Widen before adding so `retries == UINT32_MAX` cannot wrap to 0 attempts.
  const uint64_t max_attempts = static_cast<uint64_t>(options.retries) + 1;
  uint64_t slept_ms = 0;
  DWORD last_error = ERROR_SUCCESS;

  for (uint64_t attempt = 0; attempt < max_attempts; ++attempt) {
    const DWORD attrs = ::GetFileAttributesW(ext_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      const DWORD attr_err = ::GetLastError();
      if (IsNotFoundError(attr_err)) {
        return true;
      }
    } else if ((attrs & kClearMask) != 0) {
      const DWORD desired =
          is_dir ? (attrs & ~kClearMask) : FILE_ATTRIBUTE_NORMAL;
      ::SetFileAttributesW(ext_path.c_str(),
                           desired != 0 ? desired : FILE_ATTRIBUTE_NORMAL);
    }

    const BOOL ok = is_dir ? ::RemoveDirectoryW(ext_path.c_str())
                           : ::DeleteFileW(ext_path.c_str());
    if (ok != FALSE) {
      return true;
    }

    last_error = ::GetLastError();
    if (IsNotFoundError(last_error)) {
      return true;
    }

    if (last_error == ERROR_ACCESS_DENIED) {
      ResetDaclToFullControl(ext_path, /*is_reparse_point=*/true);
      ::SetFileAttributesW(ext_path.c_str(), FILE_ATTRIBUTE_NORMAL);
      const BOOL retry_ok = is_dir ? ::RemoveDirectoryW(ext_path.c_str())
                                   : ::DeleteFileW(ext_path.c_str());
      if (retry_ok != FALSE) {
        return true;
      }
      last_error = ::GetLastError();
      if (IsNotFoundError(last_error)) {
        return true;
      }
    }

    // A directory whose child already failed to delete stays non-empty, so
    // retrying ERROR_DIR_NOT_EMPTY would only burn the backoff budget.
    if (is_dir && child_failed && last_error == ERROR_DIR_NOT_EMPTY) {
      break;
    }
    if (!IsRetryableDeleteError(last_error) || attempt + 1 >= max_attempts ||
        slept_ms + backoff_ms > kMaxTotalRetrySleepMs) {
      break;
    }
    ::Sleep(backoff_ms);
    slept_ms += backoff_ms;
    backoff_ms = std::min<DWORD>(backoff_ms * 2, kMaxBackoffMs);
  }

  if (error_out != nullptr) {
    *error_out = "Failed to remove '" + WideToUtf8(ext_path) +
                 "': " + FormatWin32Error(last_error);
  }
  return false;
}

// Queries attributes of `ext_path`, resetting a restrictive DACL once on
// ERROR_ACCESS_DENIED. Returns false on a hard error; sets `*absent_out` when
// the node does not exist.
bool QueryNodeAttributes(const std::wstring &ext_path, DWORD *attrs_out,
                         bool *absent_out, std::string *error_out) {
  *absent_out = false;
  DWORD attrs = ::GetFileAttributesW(ext_path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    DWORD err = ::GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
      ResetDaclToFullControl(ext_path, /*is_reparse_point=*/true);
      attrs = ::GetFileAttributesW(ext_path.c_str());
      err = attrs == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS;
    }
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      if (IsNotFoundError(err)) {
        *absent_out = true;
        return true;
      }
      *error_out = "GetFileAttributesW failed on '" + WideToUtf8(ext_path) +
                   "': " + FormatWin32Error(err);
      return false;
    }
  }
  *attrs_out = attrs;
  return true;
}

enum class EnumResult {
  kOk,      ///< All children listed.
  kPartial, ///< FindNextFileW failed mid-way; listed children are valid.
  kFailed,  ///< FindFirstFileW failed; directory cannot be processed.
};

// Lists the children of a regular directory (resetting the DACL once on
// access denied), preserving exact case so case-distinct names in
// case-sensitive directories (e.g. "foo.txt" and "FOO.txt") are all deleted.
EnumResult EnumerateChildren(const std::wstring &ext_path,
                             std::vector<std::wstring> *children,
                             std::string *error_out) {
  const std::wstring search_pattern = JoinPath(ext_path, L"*");
  WIN32_FIND_DATAW find_data{};
  UniqueFindHandle find(::FindFirstFileW(search_pattern.c_str(), &find_data));
  if (!find) {
    DWORD err = ::GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
      ResetDaclToFullControl(ext_path, /*is_reparse_point=*/true);
      find.reset(::FindFirstFileW(search_pattern.c_str(), &find_data));
      if (!find) {
        err = ::GetLastError();
      }
    }
    if (!find) {
      if (IsNotFoundError(err) || err == ERROR_NO_MORE_FILES) {
        return EnumResult::kOk;
      }
      *error_out = "FindFirstFileW failed on '" + WideToUtf8(ext_path) +
                   "': " + FormatWin32Error(err);
      return EnumResult::kFailed;
    }
  }

  std::set<std::wstring> seen_exact;
  do {
    const std::wstring_view name(find_data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    std::wstring name_str(name);
    if (seen_exact.insert(name_str).second) {
      children->push_back(std::move(name_str));
    }
  } while (::FindNextFileW(find.get(), &find_data) != FALSE);
  const DWORD find_err = ::GetLastError();
  find.reset();
  if (find_err != ERROR_NO_MORE_FILES) {
    *error_out = "FindNextFileW failed on '" + WideToUtf8(ext_path) +
                 "': " + FormatWin32Error(find_err);
    return EnumResult::kPartial;
  }
  return EnumResult::kOk;
}

// Post-order tree removal driven by an explicit heap-allocated work stack, so
// arbitrarily deep `\\?\` trees cannot overflow the thread stack. A directory
// entry stays on the stack (expanded) beneath its children and is removed once
// all of them have been popped. Reparse points are NEVER descended into: only
// the link itself is removed so junction/symlink targets are untouched.
bool RemoveTree(const std::wstring &root, const CleanupOptions &options,
                std::string *error_out) {
  struct WorkItem {
    std::wstring path;
    size_t parent = kNoParent; ///< Stack index of the parent directory.
    bool expanded = false;     ///< Children have been pushed.
    bool child_failed = false; ///< Some descendant could not be removed.
  };

  std::vector<WorkItem> stack;
  stack.push_back(WorkItem{root, kNoParent});
  std::string first_error;
  bool all_ok = true;
  // Records a failure and marks the directory at `dir_index` as non-empty.
  auto record_failure = [&](size_t dir_index, std::string err) {
    all_ok = false;
    if (first_error.empty()) {
      first_error = std::move(err);
    }
    if (dir_index != kNoParent) {
      stack[dir_index].child_failed = true;
    }
  };

  while (!stack.empty()) {
    const size_t top = stack.size() - 1;
    std::string err;

    if (stack[top].expanded) {
      const WorkItem item = std::move(stack[top]);
      stack.pop_back();
      if (!DeleteSingleEntryWithRetry(item.path, /*is_dir=*/true,
                                      item.child_failed, options, &err)) {
        record_failure(item.parent, std::move(err));
      }
      continue;
    }

    DWORD attrs = 0;
    bool absent = false;
    if (!QueryNodeAttributes(stack[top].path, &attrs, &absent, &err)) {
      const size_t parent = stack[top].parent;
      stack.pop_back();
      record_failure(parent, std::move(err));
      continue;
    }
    if (absent) {
      stack.pop_back();
      continue;
    }

    const bool is_reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_reparse || !is_dir) {
      const WorkItem item = std::move(stack[top]);
      stack.pop_back();
      if (!DeleteSingleEntryWithRetry(item.path, is_dir,
                                      /*child_failed=*/false, options, &err)) {
        record_failure(item.parent, std::move(err));
      }
      continue;
    }

    std::vector<std::wstring> children;
    const EnumResult enum_result =
        EnumerateChildren(stack[top].path, &children, &err);
    if (enum_result == EnumResult::kFailed) {
      const size_t parent = stack[top].parent;
      stack.pop_back();
      record_failure(parent, std::move(err));
      continue;
    }
    if (enum_result == EnumResult::kPartial) {
      record_failure(top, std::move(err));
    }

    stack[top].expanded = true;
    // Copy: push_back below may reallocate and invalidate stack[top].
    const std::wstring dir_path = stack[top].path;
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(WorkItem{JoinPath(dir_path, *it), top});
    }
  }

  if (!all_ok && error_out != nullptr) {
    *error_out = std::move(first_error);
  }
  return all_ok;
}

} // namespace

bool ResolveSafeCleanupTarget(const std::wstring &path,
                              std::wstring *canonical_out,
                              std::string *reason_out) {
  if (canonical_out != nullptr) {
    canonical_out->clear();
  }
  auto reject = [&](const std::string &reason) {
    if (reason_out != nullptr) {
      *reason_out = reason;
    }
    return false;
  };

  if (path.empty()) {
    return reject("Cleanup target path is empty.");
  }
  if (ContainsDotComponent(path, /*include_single_dot=*/false)) {
    return reject("Cleanup target path must not contain '..' components.");
  }
  if (IsDriveRelativePath(path)) {
    return reject("Refusing drive-relative cleanup target path: " +
                  WideToUtf8(path));
  }
  if (path.size() >= 4 && IsExtendedPrefix(path) &&
      StartsWithIgnoreCase(std::wstring_view(path).substr(4), L"GLOBALROOT")) {
    return reject("Refusing \\\\?\\GLOBALROOT cleanup target path: " +
                  WideToUtf8(path));
  }
  if (IsDeviceNamespacePath(path)) {
    return reject("Refusing device-namespace cleanup target path: " +
                  WideToUtf8(path));
  }

  // Canonicalize every non-verbatim path so `C:\x\.`, `C:\x.` and rooted
  // `\x` resolve to the directory that will actually be deleted.
  std::wstring resolved = path;
  if (IsExtendedPrefix(path)) {
    if (ContainsDotComponent(path, /*include_single_dot=*/true)) {
      return reject("Extended cleanup target path must not contain '.' or '..' "
                    "components.");
    }
  } else {
    const DWORD err = GetFullPathNameString(path, &resolved);
    if (err != ERROR_SUCCESS) {
      return reject("Failed to resolve cleanup target path: " +
                    FormatWin32Error(err));
    }
    if (IsDeviceNamespacePath(resolved)) {
      return reject("Refusing device-namespace cleanup target path: " +
                    WideToUtf8(resolved));
    }
  }

  const std::wstring norm = NormalizeWin32Path(resolved);
  if (IsUncAdminShare(norm)) {
    return reject("Refusing UNC administrative share cleanup target: " +
                  WideToUtf8(norm));
  }
  const RootKind kind = ClassifyPathKind(norm);
  if (kind == RootKind::kDriveLetter) {
    return reject("Refusing to clean up a drive root: " + WideToUtf8(norm));
  }
  if (kind == RootKind::kVolumeGuid) {
    return reject("Refusing to clean up a volume GUID root: " +
                  WideToUtf8(norm));
  }
  if (kind == RootKind::kUncShare) {
    return reject("Refusing to clean up a UNC share root: " + WideToUtf8(norm));
  }

  const std::wstring plain = StripExtendedPrefixForCheck(norm);
  if (ClassifyPathKind(plain) != RootKind::kSubdirectory &&
      ClassifyPathKind(plain) != RootKind::kUncSubdirectory) {
    return reject("Refusing to clean up a root path: " + WideToUtf8(plain));
  }
  if (DetectIsVolumeRoot(plain)) {
    return reject("Refusing to clean up a mounted volume root: " +
                  WideToUtf8(plain));
  }
  if (IsProtectedTopLevelName(TailAfterVolumePrefix(plain))) {
    return reject("Refusing to clean up protected top-level OS directory: " +
                  WideToUtf8(plain));
  }

  // Lexical pre-checks against the protected set (also covers targets that do
  // not exist yet, and hosts where a protected directory cannot be opened).
  const std::vector<ProtectedDir> protected_dirs = BuildProtectedDirs();
  for (const ProtectedDir &dir : protected_dirs) {
    if (IsSameOrSubpathIgnoreCase(dir.path, plain) ||
        (dir.forbid_descendants &&
         IsSameOrSubpathIgnoreCase(plain, dir.path))) {
      return reject("Refusing to clean up protected system directory: " +
                    WideToUtf8(plain));
    }
  }

  // Filesystem identity checks: open the target itself (never its reparse
  // target) and compare final paths, defeating 8.3 short names, volume GUID
  // aliases, mapped drives, and junction/symlink ancestors.
  const CanonicalDir target =
      OpenCanonicalDirectory(ToExtendedWin32Path(norm),
                             /*open_reparse_point=*/true);
  if (target.error != ERROR_SUCCESS) {
    if (IsNotFoundError(target.error)) {
      return true; // Nothing to delete; *canonical_out stays empty.
    }
    return reject("Failed to open cleanup target '" + WideToUtf8(norm) +
                  "': " + FormatWin32Error(target.error));
  }
  if ((target.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return reject("Refusing to clean up a reparse point (junction/symlink): " +
                  WideToUtf8(norm));
  }
  if ((target.attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return reject("Cleanup target is not a directory: " + WideToUtf8(norm));
  }

  const std::wstring &canonical =
      !target.guid_path.empty() ? target.guid_path : target.dos_path;
  for (const std::wstring *form : {&target.guid_path, &target.dos_path}) {
    if (form->empty()) {
      continue;
    }
    if (IsUncAdminShare(*form)) {
      return reject("Cleanup target resolves to a UNC administrative share: " +
                    WideToUtf8(*form));
    }
    const RootKind form_kind = ClassifyPathKind(*form);
    if (form_kind == RootKind::kDriveLetter ||
        form_kind == RootKind::kVolumeGuid ||
        form_kind == RootKind::kUncShare) {
      return reject("Cleanup target resolves to a volume or share root: " +
                    WideToUtf8(*form));
    }
    if (IsProtectedTopLevelName(TailAfterVolumePrefix(*form))) {
      return reject("Cleanup target resolves to a protected top-level OS "
                    "directory: " +
                    WideToUtf8(*form));
    }
  }

  for (const ProtectedDir &dir : protected_dirs) {
    const CanonicalDir prot =
        OpenCanonicalDirectory(ToExtendedWin32Path(dir.path),
                               /*open_reparse_point=*/false);
    if (prot.error != ERROR_SUCCESS) {
      continue;
    }
    if (CanonicalSameOrSubpath(prot, target) ||
        (dir.forbid_descendants && CanonicalSameOrSubpath(target, prot))) {
      return reject("Cleanup target '" + WideToUtf8(norm) +
                    "' resolves to protected system directory '" +
                    WideToUtf8(dir.path) + "' (canonical '" +
                    WideToUtf8(canonical) + "').");
    }
  }

  if (canonical_out != nullptr) {
    *canonical_out = canonical;
  }
  return true;
}

bool IsSafeCleanupTarget(std::wstring_view path, std::string *reason_out) {
  return ResolveSafeCleanupTarget(std::wstring(path), nullptr, reason_out);
}

bool ForceRemoveAll(std::wstring_view path, const CleanupOptions &options,
                    std::string *error_out) {
  if (path.empty()) {
    if (error_out != nullptr) {
      *error_out = "ForceRemoveAll called with empty path.";
    }
    return false;
  }

  // ToExtendedWin32Path keeps `\\?\Volume{GUID}\...` (as returned by
  // ResolveSafeCleanupTarget) and `\\?\UNC\...` verbatim.
  const std::wstring ext_path = ToExtendedWin32Path(path);
  std::string remove_err;
  const bool removed = RemoveTree(ext_path, options, &remove_err);
  if (!removed) {
    if (error_out != nullptr) {
      *error_out = remove_err;
    }
    return false;
  }

  // Zero-debris verification.
  const DWORD verify_attrs = ::GetFileAttributesW(ext_path.c_str());
  if (verify_attrs != INVALID_FILE_ATTRIBUTES) {
    if (error_out != nullptr) {
      *error_out = "Zero-debris check failed: path still exists after "
                   "removal: " +
                   WideToUtf8(ext_path);
    }
    return false;
  }
  const DWORD verify_err = ::GetLastError();
  if (!IsNotFoundError(verify_err)) {
    if (error_out != nullptr) {
      *error_out = "Zero-debris check unexpected error on '" +
                   WideToUtf8(ext_path) + "': " + FormatWin32Error(verify_err);
    }
    return false;
  }
  return true;
}

ScopedTempDir::~ScopedTempDir() {
  if (active_) {
    std::string ignored;
    Cleanup(&ignored);
  }
}

ScopedTempDir::ScopedTempDir(ScopedTempDir &&other) noexcept
    : path_(std::move(other.path_)),
      extended_path_(std::move(other.extended_path_)),
      basename_(std::move(other.basename_)),
      cleanup_options_(other.cleanup_options_),
      active_(std::exchange(other.active_, false)) {}

ScopedTempDir &ScopedTempDir::operator=(ScopedTempDir &&other) {
  if (this != &other) {
    if (active_) {
      std::string ignored;
      Cleanup(&ignored);
    }
    path_ = std::move(other.path_);
    extended_path_ = std::move(other.extended_path_);
    basename_ = std::move(other.basename_);
    cleanup_options_ = other.cleanup_options_;
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

Win32Result<ScopedTempDir>
ScopedTempDir::Create(std::wstring_view parent_dir, std::wstring_view prefix,
                      CleanupOptions cleanup_options) {
  Win32Result<ScopedTempDir> result{};
  if (parent_dir.empty()) {
    result.error = ERROR_INVALID_PARAMETER;
    return result;
  }

  const std::wstring norm_parent = NormalizeWin32Path(parent_dir);
  const std::wstring effective_prefix =
      prefix.empty() ? L"fst" : std::wstring(prefix);
  const DWORD pid = ::GetCurrentProcessId();

  for (int attempt = 0; attempt < 64; ++attempt) {
    const uint32_t seq =
        g_temp_dir_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    wchar_t suffix[32];
    std::swprintf(suffix, sizeof(suffix) / sizeof(suffix[0]), L"_%lu_%04u",
                  static_cast<unsigned long>(pid),
                  static_cast<unsigned int>(seq));
    const std::wstring basename = effective_prefix + suffix;
    const std::wstring full_path = JoinPath(norm_parent, basename);
    const std::wstring ext_path = ToExtendedWin32Path(full_path);

    if (::CreateDirectoryW(ext_path.c_str(), nullptr) != FALSE) {
      // Best-effort clear inherited NOT_CONTENT_INDEXED (0x2000) policy bit so
      // core attribute tests run cleanly on Windows Server temp volumes.
      const DWORD attrs = ::GetFileAttributesW(ext_path.c_str());
      if (attrs != INVALID_FILE_ATTRIBUTES &&
          (attrs & nt::kFileAttributeNotContentIndexed) != 0) {
        ::SetFileAttributesW(ext_path.c_str(),
                             attrs & ~nt::kFileAttributeNotContentIndexed);
      }

      ScopedTempDir dir;
      dir.path_ = full_path;
      dir.extended_path_ = ext_path;
      dir.basename_ = basename;
      dir.cleanup_options_ = cleanup_options;
      dir.active_ = true;

      result.error = ERROR_SUCCESS;
      result.value = std::move(dir);
      return result;
    }

    const DWORD err = ::GetLastError();
    if (err != ERROR_ALREADY_EXISTS) {
      result.error = err;
      return result;
    }
  }

  result.error = ERROR_ALREADY_EXISTS;
  return result;
}

bool ScopedTempDir::Cleanup(std::string *error_out) {
  if (!active_) {
    return true;
  }
  active_ = false;
  return ForceRemoveAll(extended_path_, cleanup_options_, error_out);
}

std::wstring ScopedTempDir::ChildPath(std::wstring_view rel) const {
  return JoinPath(path_, rel);
}

std::wstring ScopedTempDir::ChildExtendedPath(std::wstring_view rel) const {
  return JoinPath(extended_path_, rel);
}

} // namespace fstest::core
