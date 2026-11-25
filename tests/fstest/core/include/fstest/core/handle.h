// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file handle.h
/// \brief Move-only RAII wrappers for Win32/NT handles, search handles,
/// LocalAlloc buffers, mapped views, impersonation, and token privileges.

#pragma once

#include "fstest/core/win_headers.h"

#include <utility>

namespace fstest::core {
namespace detail {

struct Win32HandleTraitsBase {
  using pointer = HANDLE;
  static bool IsValid(HANDLE h) noexcept {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
  }
  static HANDLE Normalize(HANDLE h) noexcept {
    return IsValid(h) ? h : nullptr;
  }
};

struct HandleTraits : Win32HandleTraitsBase {
  static void Close(HANDLE h) noexcept { ::CloseHandle(h); }
};

struct FindHandleTraits : Win32HandleTraitsBase {
  static void Close(HANDLE h) noexcept { ::FindClose(h); }
};

template <typename T> struct LocalMemTraits {
  using pointer = T *;
  static bool IsValid(T *p) noexcept { return p != nullptr; }
  static T *Normalize(T *p) noexcept { return p; }
  static void Close(T *p) noexcept { ::LocalFree(p); }
};

struct ViewOfFileTraits {
  using pointer = void *;
  static bool IsValid(void *p) noexcept { return p != nullptr; }
  static void *Normalize(void *p) noexcept { return p; }
  static void Close(void *p) noexcept { ::UnmapViewOfFile(p); }
};

/// \brief Move-only RAII wrapper parameterized by a resource traits type.
template <typename Traits> class UniqueResource {
public:
  using pointer = typename Traits::pointer;

  UniqueResource() noexcept = default;

  /// \brief Takes ownership of \p h, normalizing any sentinel to `nullptr`.
  explicit UniqueResource(pointer h) noexcept : handle_(Traits::Normalize(h)) {}

  ~UniqueResource() { reset(); }

  UniqueResource(const UniqueResource &) = delete;
  UniqueResource &operator=(const UniqueResource &) = delete;

  UniqueResource(UniqueResource &&other) noexcept
      : handle_(Traits::Normalize(std::exchange(other.handle_, nullptr))) {}

  UniqueResource &operator=(UniqueResource &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, nullptr));
    }
    return *this;
  }

  /// \brief Returns the raw resource, or `nullptr` when empty/invalid.
  pointer get() const noexcept { return Traits::Normalize(handle_); }

  /// \brief Returns true iff a valid non-sentinel resource is owned.
  bool valid() const noexcept { return Traits::IsValid(handle_); }

  /// \brief Evaluates true iff valid() is true.
  explicit operator bool() const noexcept { return valid(); }

  /// \brief Resets any currently owned resource and returns the raw slot for
  /// Win32/NT out-parameters.
  pointer *put() noexcept {
    reset();
    return &handle_;
  }

  /// \brief Relinquishes ownership and returns the normalized resource.
  pointer release() noexcept {
    return Traits::Normalize(std::exchange(handle_, nullptr));
  }

  /// \brief Closes any currently owned resource (unless equal to \p new_handle)
  /// and takes ownership of \p new_handle.
  void reset(pointer new_handle = nullptr) noexcept {
    pointer normalized = Traits::Normalize(new_handle);
    pointer current = Traits::Normalize(handle_);
    if (normalized == current) {
      handle_ = normalized;
      return;
    }
    if (Traits::IsValid(current)) {
      Traits::Close(current);
    }
    handle_ = normalized;
  }

private:
  pointer handle_ = nullptr;
};

} // namespace detail

/// \brief Move-only RAII wrapper for Win32 and NT HANDLE values closed via
/// `CloseHandle`. Treats both `nullptr` and `INVALID_HANDLE_VALUE` as invalid.
using UniqueHandle = detail::UniqueResource<detail::HandleTraits>;

/// \brief Move-only RAII wrapper for `FindFirstFile*` / `FindFirstStream*` /
/// `FindFirstFileName*` search handles closed via `FindClose`.
using UniqueFindHandle = detail::UniqueResource<detail::FindHandleTraits>;

/// \brief Move-only RAII wrapper for `LocalAlloc` / SDDL / `FormatMessageW`
/// allocations freed via `LocalFree`.
/// \tparam T Pointed-to element type.
template <typename T>
using UniqueLocalMem = detail::UniqueResource<detail::LocalMemTraits<T>>;

/// \brief Move-only RAII wrapper for `MapViewOfFile` base addresses unmapped
/// via `UnmapViewOfFile`.
using UniqueViewOfFile = detail::UniqueResource<detail::ViewOfFileTraits>;

/// \brief Move-only RAII guard that calls `ImpersonateLoggedOnUser(token)` and
/// unconditionally invokes `RevertToSelf()` on scope exit.
class ScopedImpersonation {
public:
  ScopedImpersonation() noexcept = default;

  /// \brief Begins impersonating \p token on the calling thread.
  /// \param token Valid impersonation token handle.
  explicit ScopedImpersonation(HANDLE token) noexcept {
    if (token != nullptr && token != INVALID_HANDLE_VALUE) {
      if (::ImpersonateLoggedOnUser(token) != FALSE) {
        active_ = true;
      } else {
        error_ = ::GetLastError();
      }
    } else {
      error_ = ERROR_INVALID_HANDLE;
    }
  }

  ~ScopedImpersonation() { Revert(); }

  ScopedImpersonation(const ScopedImpersonation &) = delete;
  ScopedImpersonation &operator=(const ScopedImpersonation &) = delete;

  ScopedImpersonation(ScopedImpersonation &&other) noexcept
      : active_(std::exchange(other.active_, false)),
        error_(std::exchange(other.error_, ERROR_SUCCESS)) {}

  ScopedImpersonation &operator=(ScopedImpersonation &&other) noexcept {
    if (this != &other) {
      Revert();
      active_ = std::exchange(other.active_, false);
      error_ = std::exchange(other.error_, ERROR_SUCCESS);
    }
    return *this;
  }

  /// \brief Returns true if impersonation succeeded and is currently active.
  bool active() const noexcept { return active_; }

  /// \brief Returns the Win32 error code from `ImpersonateLoggedOnUser` on
  /// failure, or `ERROR_SUCCESS`.
  DWORD error() const noexcept { return error_; }

  /// \brief Evaluates true iff active() is true.
  explicit operator bool() const noexcept { return active_; }

  /// \brief Reverts thread impersonation if currently active.
  void Revert() noexcept {
    if (active_) {
      ::RevertToSelf();
      active_ = false;
    }
  }

private:
  bool active_ = false;
  DWORD error_ = ERROR_SUCCESS;
};

/// \brief Move-only RAII guard that enables or disables a named privilege on
/// the current process token and restores its previous state on destruction.
/// Implemented in `security.cpp`.
class ScopedPrivilege {
public:
  ScopedPrivilege() noexcept = default;

  /// \brief Adjusts \p privilege_name on the current process token.
  /// \param privilege_name Win32 privilege name (e.g., `SE_BACKUP_NAME`).
  /// \param enable True to enable the privilege, false to disable it.
  explicit ScopedPrivilege(const wchar_t *privilege_name,
                           bool enable = true) noexcept;
  ~ScopedPrivilege();

  ScopedPrivilege(const ScopedPrivilege &) = delete;
  ScopedPrivilege &operator=(const ScopedPrivilege &) = delete;

  ScopedPrivilege(ScopedPrivilege &&other) noexcept;
  ScopedPrivilege &operator=(ScopedPrivilege &&other) noexcept;

  /// \brief Returns true if the privilege adjustment succeeded and is active.
  bool active() const noexcept { return active_; }

  /// \brief Returns the Win32 error code captured when adjustment failed.
  DWORD error() const noexcept { return error_; }

  /// \brief Evaluates true iff active() is true.
  explicit operator bool() const noexcept { return active_; }

  /// \brief Restores the previous token privilege state and closes the token.
  void Restore() noexcept;

private:
  UniqueHandle token_;
  TOKEN_PRIVILEGES previous_state_{};
  bool active_ = false;
  DWORD error_ = ERROR_SUCCESS;
};

} // namespace fstest::core
