// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/security.h"

#include "fstest/core/file_ops.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/path.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace fstest::core {
namespace {

bool OpenCurrentProcessToken(DWORD desired_access, UniqueHandle *token_out,
                             DWORD *error_out) noexcept {
  if (::OpenProcessToken(::GetCurrentProcess(), desired_access,
                         token_out->put()) == FALSE) {
    if (error_out != nullptr) {
      *error_out = ::GetLastError();
    }
    return false;
  }
  return true;
}

bool LookupPrivilegeLuid(const wchar_t *privilege_name, LUID *luid_out,
                         DWORD *error_out) noexcept {
  if (::LookupPrivilegeValueW(nullptr, privilege_name, luid_out) == FALSE) {
    if (error_out != nullptr) {
      *error_out = ::GetLastError();
    }
    return false;
  }
  return true;
}

bool AdjustSinglePrivilege(HANDLE token, const wchar_t *privilege_name,
                           DWORD attributes, TOKEN_PRIVILEGES *previous_state,
                           DWORD *error_out) noexcept {
  LUID luid{};
  if (!LookupPrivilegeLuid(privilege_name, &luid, error_out)) {
    return false;
  }

  TOKEN_PRIVILEGES new_state{};
  new_state.PrivilegeCount = 1;
  new_state.Privileges[0].Luid = luid;
  new_state.Privileges[0].Attributes = attributes;

  DWORD prev_size = (previous_state != nullptr)
                        ? static_cast<DWORD>(sizeof(*previous_state))
                        : 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::AdjustTokenPrivileges(
          token, FALSE, &new_state, prev_size, previous_state,
          (previous_state != nullptr) ? &prev_size : nullptr) == FALSE) {
    if (error_out != nullptr) {
      *error_out = ::GetLastError();
    }
    return false;
  }
  if (error_out != nullptr) {
    *error_out = ::GetLastError();
  }
  return true;
}

// Generic two-pass Win32 buffer sizing helper (GetFileSecurityW,
// GetTokenInformation, etc.) that probes required length, allocates, and
// retrieves the payload.
template <typename QueryFn>
Win32Result<std::vector<uint8_t>>
QueryTwoPassWin32Buffer(QueryFn &&query_fn, DWORD zero_size_error) {
  Win32Result<std::vector<uint8_t>> result{};
  DWORD needed = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (query_fn(nullptr, 0, &needed) == FALSE) {
    const DWORD err = ::GetLastError();
    if (err != ERROR_INSUFFICIENT_BUFFER) {
      result.error = (err != ERROR_SUCCESS) ? err : zero_size_error;
      return result;
    }
  }
  if (needed == 0) {
    result.error = zero_size_error;
    return result;
  }
  std::vector<uint8_t> buf(needed, 0);
  ::SetLastError(ERROR_SUCCESS);
  if (query_fn(buf.data(), needed, &needed) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(buf);
  return result;
}

Win32Result<std::vector<uint8_t>>
QueryCurrentProcessTokenInfo(TOKEN_INFORMATION_CLASS info_class) {
  Win32Result<std::vector<uint8_t>> result{};
  UniqueHandle token;
  if (!OpenCurrentProcessToken(TOKEN_QUERY, &token, &result.error)) {
    return result;
  }
  return QueryTwoPassWin32Buffer(
      [&](void *buf, DWORD size, DWORD *needed) {
        return ::GetTokenInformation(token.get(), info_class, buf, size,
                                     needed);
      },
      ERROR_INSUFFICIENT_BUFFER);
}

DWORD NtStatusToDosErrorSafe(const NtdllApi &ntdll, NTSTATUS st) noexcept {
  return (ntdll.RtlNtStatusToDosError != nullptr)
             ? ntdll.RtlNtStatusToDosError(st)
             : static_cast<DWORD>(ERROR_GEN_FAILURE);
}

UniqueHandle OpenNtSecurityHandle(std::wstring_view nt_path, ACCESS_MASK access,
                                  ULONG open_options, NTSTATUS *status_out) {
  auto res = NtOpenFilePath(nt_path, access, kShareAll, open_options);
  if (status_out != nullptr) {
    *status_out = res.status;
  }
  return res ? std::move(*res.value) : UniqueHandle{};
}

// Enables privileges on a private impersonation token of the calling thread
// so the process token (shared by every thread) is never mutated. Uses
// ImpersonateSelf when the thread is not impersonating; otherwise impersonates
// a duplicate of the existing thread token and restores the original token on
// destruction. Every exit path reverts (RevertToSelf / SetThreadToken).
class ScopedThreadPrivileges {
public:
  ScopedThreadPrivileges() noexcept {
    HANDLE existing = nullptr;
    if (::OpenThreadToken(::GetCurrentThread(),
                          TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
                          /*OpenAsSelf=*/TRUE, &existing) != FALSE) {
      saved_token_.reset(existing);
      UniqueHandle dup;
      if (::DuplicateTokenEx(existing,
                             TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY |
                                 TOKEN_IMPERSONATE,
                             nullptr, SecurityImpersonation, TokenImpersonation,
                             dup.put()) == FALSE ||
          ::SetThreadToken(nullptr, dup.get()) == FALSE) {
        return;
      }
    } else if (::ImpersonateSelf(SecurityImpersonation) == FALSE) {
      return;
    }
    impersonating_ = true;

    HANDLE thread_token = nullptr;
    if (::OpenThreadToken(::GetCurrentThread(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          /*OpenAsSelf=*/TRUE, &thread_token) != FALSE) {
      token_.reset(thread_token);
    }
  }

  ~ScopedThreadPrivileges() {
    token_.reset();
    if (impersonating_) {
      if (saved_token_) {
        ::SetThreadToken(nullptr, saved_token_.get());
      } else {
        ::RevertToSelf();
      }
    }
  }

  ScopedThreadPrivileges(const ScopedThreadPrivileges &) = delete;
  ScopedThreadPrivileges &operator=(const ScopedThreadPrivileges &) = delete;

  /// Best-effort enable of \p privilege_name on the private thread token.
  void Enable(const wchar_t *privilege_name) noexcept {
    if (token_) {
      DWORD ignored = ERROR_SUCCESS;
      AdjustSinglePrivilege(token_.get(), privilege_name, SE_PRIVILEGE_ENABLED,
                            nullptr, &ignored);
    }
  }

private:
  UniqueHandle saved_token_; ///< Pre-existing impersonation token, if any.
  UniqueHandle token_;       ///< Private thread token being adjusted.
  bool impersonating_ = false;
};

} // namespace

ScopedPrivilege::ScopedPrivilege(const wchar_t *privilege_name,
                                 bool enable) noexcept {
  if (!OpenCurrentProcessToken(TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token_,
                               &error_)) {
    return;
  }

  const DWORD attrs = enable ? SE_PRIVILEGE_ENABLED : 0;
  if (!AdjustSinglePrivilege(token_.get(), privilege_name, attrs,
                             &previous_state_, &error_) ||
      error_ != ERROR_SUCCESS) {
    token_.reset();
    return;
  }

  active_ = true;
  error_ = ERROR_SUCCESS;
}

ScopedPrivilege::~ScopedPrivilege() { Restore(); }

ScopedPrivilege::ScopedPrivilege(ScopedPrivilege &&other) noexcept
    : token_(std::move(other.token_)), previous_state_(other.previous_state_),
      active_(std::exchange(other.active_, false)),
      error_(std::exchange(other.error_, ERROR_SUCCESS)) {}

ScopedPrivilege &ScopedPrivilege::operator=(ScopedPrivilege &&other) noexcept {
  if (this != &other) {
    Restore();
    token_ = std::move(other.token_);
    previous_state_ = other.previous_state_;
    active_ = std::exchange(other.active_, false);
    error_ = std::exchange(other.error_, ERROR_SUCCESS);
  }
  return *this;
}

void ScopedPrivilege::Restore() noexcept {
  if (active_ && token_) {
    ::AdjustTokenPrivileges(token_.get(), FALSE, &previous_state_, 0, nullptr,
                            nullptr);
  }
  active_ = false;
  token_.reset();
}

Win32Result<UniqueLocalMem<void>>
SddlToSecurityDescriptor(std::wstring_view sddl) {
  Win32Result<UniqueLocalMem<void>> result{};
  std::wstring sddl_str(sddl);
  UniqueLocalMem<void> sd;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl_str.c_str(), SDDL_REVISION_1, sd.put(), nullptr) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(sd);
  return result;
}

Win32Result<std::wstring> SecurityDescriptorToSddl(PSECURITY_DESCRIPTOR sd,
                                                   SECURITY_INFORMATION info) {
  Win32Result<std::wstring> result{};
  UniqueLocalMem<wchar_t> sddl_buf;
  if (::ConvertSecurityDescriptorToStringSecurityDescriptorW(
          sd, SDDL_REVISION_1, info, sddl_buf.put(), nullptr) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::wstring(sddl_buf.get());
  return result;
}

Win32Result<std::wstring> GetFileSddl(std::wstring_view path,
                                      SECURITY_INFORMATION info) {
  std::wstring path_str(path);
  auto sd_buf = QueryTwoPassWin32Buffer(
      [&](void *buf, DWORD size, DWORD *needed) {
        return ::GetFileSecurityW(path_str.c_str(), info, buf, size, needed);
      },
      ERROR_INVALID_SECURITY_DESCR);
  if (!sd_buf) {
    Win32Result<std::wstring> result{};
    result.error = sd_buf.error;
    return result;
  }
  return SecurityDescriptorToSddl(sd_buf.value->data(), info);
}

Win32Result<void> SetFileSddl(std::wstring_view path, SECURITY_INFORMATION info,
                              std::wstring_view sddl) {
  Win32Result<void> result{};
  auto sd = SddlToSecurityDescriptor(sddl);
  if (!sd) {
    result.error = sd.error;
    return result;
  }
  std::wstring path_str(path);
  if (::SetFileSecurityW(path_str.c_str(), info, sd.value->get()) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  return result;
}

Win32Result<UniqueLocalMem<void>> GetCurrentUserSid() {
  Win32Result<UniqueLocalMem<void>> result{};
  auto buf = QueryCurrentProcessTokenInfo(TokenUser);
  if (!buf) {
    result.error = buf.error;
    return result;
  }

  const auto *user = reinterpret_cast<const TOKEN_USER *>(buf.value->data());
  const DWORD sid_len = ::GetLengthSid(user->User.Sid);
  UniqueLocalMem<void> sid_copy(::LocalAlloc(LMEM_FIXED, sid_len));
  if (!sid_copy) {
    result.error = ::GetLastError();
    return result;
  }
  if (::CopySid(sid_len, sid_copy.get(), user->User.Sid) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }

  result.error = ERROR_SUCCESS;
  result.value = std::move(sid_copy);
  return result;
}

Win32Result<std::wstring> GetCurrentUserSidString() {
  Win32Result<std::wstring> result{};
  auto sid = GetCurrentUserSid();
  if (!sid) {
    result.error = sid.error;
    return result;
  }
  UniqueLocalMem<wchar_t> sid_str;
  if (::ConvertSidToStringSidW(sid.value->get(), sid_str.put()) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::wstring(sid_str.get());
  return result;
}

Win32Result<bool> IsCurrentProcessElevated() {
  Win32Result<bool> result{};
  UniqueHandle token;
  if (!OpenCurrentProcessToken(TOKEN_QUERY, &token, &result.error)) {
    return result;
  }
  TOKEN_ELEVATION elevation{};
  DWORD returned = 0;
  if (::GetTokenInformation(token.get(), TokenElevation, &elevation,
                            sizeof(elevation), &returned) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = (elevation.TokenIsElevated != 0);
  return result;
}

Win32Result<bool> HasPrivilegeInToken(const wchar_t *privilege_name) {
  Win32Result<bool> result{};
  LUID target_luid{};
  if (!LookupPrivilegeLuid(privilege_name, &target_luid, &result.error)) {
    return result;
  }

  auto buf = QueryCurrentProcessTokenInfo(TokenPrivileges);
  if (!buf) {
    result.error = buf.error;
    return result;
  }

  const auto *privs =
      reinterpret_cast<const TOKEN_PRIVILEGES *>(buf.value->data());
  bool found = false;
  for (DWORD i = 0; i < privs->PrivilegeCount; ++i) {
    const LUID &luid = privs->Privileges[i].Luid;
    if (luid.LowPart == target_luid.LowPart &&
        luid.HighPart == target_luid.HighPart) {
      found = true;
      break;
    }
  }
  result.error = ERROR_SUCCESS;
  result.value = found;
  return result;
}

Win32Result<UniqueHandle>
CreateTokenWithoutPrivilege(const wchar_t *privilege_name) {
  Win32Result<UniqueHandle> result{};
  UniqueHandle proc_token;
  if (!OpenCurrentProcessToken(TOKEN_DUPLICATE | TOKEN_QUERY, &proc_token,
                               &result.error)) {
    return result;
  }

  UniqueHandle dup_token;
  if (::DuplicateTokenEx(proc_token.get(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE |
                             TOKEN_QUERY,
                         nullptr, SecurityImpersonation, TokenImpersonation,
                         dup_token.put()) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }

  DWORD err = ERROR_SUCCESS;
  if (!AdjustSinglePrivilege(dup_token.get(), privilege_name,
                             SE_PRIVILEGE_REMOVED, nullptr, &err)) {
    result.error = err;
    return result;
  }
  if (err != ERROR_SUCCESS && err != ERROR_NOT_ALL_ASSIGNED) {
    result.error = err;
    return result;
  }

  result.error = ERROR_SUCCESS;
  result.value = std::move(dup_token);
  return result;
}

Win32Result<void> ResetDaclToFullControl(std::wstring_view extended_path,
                                         bool is_reparse_point) {
  Win32Result<void> result{};

  std::wstring sddl = L"D:P(A;;FA;;;WD)";
  auto sid_str = GetCurrentUserSidString();
  if (sid_str) {
    sddl = L"D:P(A;;FA;;;" + *sid_str.value + L")(A;;FA;;;BA)";
  }
  auto dacl_sd = SddlToSecurityDescriptor(sddl);
  if (!dacl_sd) {
    result.error = dacl_sd.error;
    return result;
  }

  // Use NtOpenFile directly (not CreateFileW): CreateFileW silently ORs
  // FILE_READ_ATTRIBUTES | SYNCHRONIZE into DesiredAccess, which fails
  // STATUS_ACCESS_DENIED under D:P(D;;FA;;;WD) even for the file owner,
  // whereas pure WRITE_DAC succeeds via the owner's implicit rights.
  const std::wstring nt_path = VerbatimToNtPath(extended_path);
  const auto &ntdll = Ntdll();
  const ULONG open_opts = nt::kFileOpenForBackupIntent |
                          (is_reparse_point ? nt::kFileOpenReparsePoint : 0UL);

  NTSTATUS st = STATUS_UNSUCCESSFUL;
  UniqueHandle handle =
      OpenNtSecurityHandle(nt_path, WRITE_DAC, open_opts, &st);
  if (!handle && st == STATUS_ACCESS_DENIED) {
    ScopedThreadPrivileges thread_privs;
    thread_privs.Enable(SE_TAKE_OWNERSHIP_NAME);
    thread_privs.Enable(SE_RESTORE_NAME);
    thread_privs.Enable(SE_BACKUP_NAME);
    auto user_sid = GetCurrentUserSid();
    if (user_sid) {
      NTSTATUS owner_st = STATUS_UNSUCCESSFUL;
      UniqueHandle owner_handle =
          OpenNtSecurityHandle(nt_path, WRITE_OWNER, open_opts, &owner_st);
      if (owner_handle && ntdll.NtSetSecurityObject != nullptr) {
        SECURITY_DESCRIPTOR owner_sd{};
        if (::InitializeSecurityDescriptor(
                &owner_sd, SECURITY_DESCRIPTOR_REVISION) != FALSE &&
            ::SetSecurityDescriptorOwner(&owner_sd, user_sid.value->get(),
                                         FALSE) != FALSE) {
          ntdll.NtSetSecurityObject(owner_handle.get(),
                                    OWNER_SECURITY_INFORMATION, &owner_sd);
        }
      }
    }
    handle = OpenNtSecurityHandle(nt_path, WRITE_DAC, open_opts, &st);
  }
  if (!handle) {
    result.error = NtStatusToDosErrorSafe(ntdll, st);
    return result;
  }
  if (ntdll.NtSetSecurityObject == nullptr) {
    result.error = ERROR_CALL_NOT_IMPLEMENTED;
    return result;
  }

  st = ntdll.NtSetSecurityObject(handle.get(), DACL_SECURITY_INFORMATION,
                                 dacl_sd.value->get());
  if (!nt::NtSuccess(st)) {
    result.error = NtStatusToDosErrorSafe(ntdll, st);
    return result;
  }
  result.error = ERROR_SUCCESS;
  return result;
}

ScopedDaclRestore::~ScopedDaclRestore() { RestoreNow(); }

ScopedDaclRestore::ScopedDaclRestore(ScopedDaclRestore &&other) noexcept
    : handle_(std::move(other.handle_)),
      restore_sd_(std::move(other.restore_sd_)),
      armed_(std::exchange(other.armed_, false)) {}

ScopedDaclRestore &
ScopedDaclRestore::operator=(ScopedDaclRestore &&other) noexcept {
  if (this != &other) {
    RestoreNow();
    handle_ = std::move(other.handle_);
    restore_sd_ = std::move(other.restore_sd_);
    armed_ = std::exchange(other.armed_, false);
  }
  return *this;
}

ScopedDaclRestore ScopedDaclRestore::Arm(std::wstring_view nt_path,
                                         std::wstring_view restore_sddl) {
  ScopedDaclRestore guard;
  const auto &ntdll = Ntdll();
  if (ntdll.NtOpenFile == nullptr || ntdll.NtSetSecurityObject == nullptr) {
    return guard;
  }

  auto sd = SddlToSecurityDescriptor(restore_sddl);
  if (!sd) {
    return guard;
  }

  NTSTATUS st = STATUS_UNSUCCESSFUL;
  // FILE_OPEN_REPARSE_POINT: guard the link itself, never its target.
  UniqueHandle h =
      OpenNtSecurityHandle(nt_path, WRITE_DAC, nt::kFileOpenReparsePoint, &st);
  if (!nt::NtSuccess(st) || !h.valid()) {
    return guard;
  }

  guard.handle_ = std::move(h);
  guard.restore_sd_ = std::move(*sd.value);
  guard.armed_ = true;
  return guard;
}

void ScopedDaclRestore::RestoreNow() noexcept {
  if (armed_ && handle_.valid() && restore_sd_.get() != nullptr) {
    const auto &ntdll = Ntdll();
    if (ntdll.NtSetSecurityObject != nullptr) {
      ntdll.NtSetSecurityObject(handle_.get(), DACL_SECURITY_INFORMATION,
                                restore_sd_.get());
    }
  }
  DisarmAndClose();
}

void ScopedDaclRestore::DisarmAndClose() noexcept {
  armed_ = false;
  handle_.reset();
  restore_sd_.reset();
}

} // namespace fstest::core
