// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file handle.h
/// \brief Move-only RAII wrappers for Win32/NT handles, search handles,
/// LocalAlloc buffers, mapped views, impersonation, and token privileges.

#pragma once

#include "fstest/core/win_headers.h"

#include <utility>

namespace fstest::core {

/// \brief Move-only RAII wrapper for Win32 and NT HANDLE values closed via
/// `CloseHandle`. Treats both `nullptr` and `INVALID_HANDLE_VALUE` as invalid.
class UniqueHandle {
public:
  UniqueHandle() noexcept = default;

  /// \brief Takes ownership of \p h, normalizing `INVALID_HANDLE_VALUE` to
  /// `nullptr`.
  explicit UniqueHandle(HANDLE h) noexcept : handle_(Normalize(h)) {}

  ~UniqueHandle() { reset(); }

  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;

  UniqueHandle(UniqueHandle &&other) noexcept
      : handle_(Normalize(std::exchange(other.handle_, nullptr))) {}

  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, nullptr));
    }
    return *this;
  }

  /// \brief Returns the raw handle, or `nullptr` when empty/invalid.
  HANDLE get() const noexcept { return Normalize(handle_); }

  /// \brief Returns true iff a valid non-null, non-`INVALID_HANDLE_VALUE`
  /// handle is owned.
  bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  /// \brief Evaluates true iff valid() is true.
  explicit operator bool() const noexcept { return valid(); }

  /// \brief Resets any currently owned handle and returns the raw slot for
  /// Win32/NT out-parameters.
  HANDLE *put() noexcept {
    reset();
    return &handle_;
  }

  /// \brief Relinquishes ownership and returns the normalized handle.
  HANDLE release() noexcept {
    return Normalize(std::exchange(handle_, nullptr));
  }

  /// \brief Closes any currently owned handle (unless equal to \p new_handle)
  /// and takes ownership of \p new_handle.
  void reset(HANDLE new_handle = nullptr) noexcept {
    HANDLE normalized = Normalize(new_handle);
    HANDLE current = Normalize(handle_);
    if (normalized == current) {
      handle_ = normalized;
      return;
    }
    if (current != nullptr) {
      ::CloseHandle(current);
    }
    handle_ = normalized;
  }

private:
  static HANDLE Normalize(HANDLE h) noexcept {
    return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
  }

  HANDLE handle_ = nullptr;
};

/// \brief Move-only RAII wrapper for `FindFirstFile*` / `FindFirstStream*` /
/// `FindFirstFileName*` search handles closed via `FindClose`.
class UniqueFindHandle {
public:
  UniqueFindHandle() noexcept = default;

  /// \brief Takes ownership of \p h, normalizing `INVALID_HANDLE_VALUE` to
  /// `nullptr`.
  explicit UniqueFindHandle(HANDLE h) noexcept : handle_(Normalize(h)) {}

  ~UniqueFindHandle() { reset(); }

  UniqueFindHandle(const UniqueFindHandle &) = delete;
  UniqueFindHandle &operator=(const UniqueFindHandle &) = delete;

  UniqueFindHandle(UniqueFindHandle &&other) noexcept
      : handle_(Normalize(std::exchange(other.handle_, nullptr))) {}

  UniqueFindHandle &operator=(UniqueFindHandle &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, nullptr));
    }
    return *this;
  }

  /// \brief Returns the search handle, or `nullptr` when empty/invalid.
  HANDLE get() const noexcept { return Normalize(handle_); }

  /// \brief Returns true iff a valid search handle is owned.
  bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  /// \brief Evaluates true iff valid() is true.
  explicit operator bool() const noexcept { return valid(); }

  /// \brief Relinquishes ownership and returns the normalized search handle.
  HANDLE release() noexcept {
    return Normalize(std::exchange(handle_, nullptr));
  }

  /// \brief Closes any currently owned search handle (unless equal to
  /// \p new_handle) and takes ownership of \p new_handle.
  void reset(HANDLE new_handle = nullptr) noexcept {
    HANDLE normalized = Normalize(new_handle);
    HANDLE current = Normalize(handle_);
    if (normalized == current) {
      handle_ = normalized;
      return;
    }
    if (current != nullptr) {
      ::FindClose(current);
    }
    handle_ = normalized;
  }

private:
  static HANDLE Normalize(HANDLE h) noexcept {
    return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
  }

  HANDLE handle_ = nullptr;
};

/// \brief Move-only RAII wrapper for `LocalAlloc` / SDDL / `FormatMessageW`
/// allocations freed via `LocalFree`.
/// \tparam T Pointed-to element type.
template <typename T> class UniqueLocalMem {
public:
  UniqueLocalMem() noexcept = default;

  /// \brief Takes ownership of \p ptr.
  explicit UniqueLocalMem(T *ptr) noexcept : ptr_(ptr) {}

  ~UniqueLocalMem() { reset(); }

  UniqueLocalMem(const UniqueLocalMem &) = delete;
  UniqueLocalMem &operator=(const UniqueLocalMem &) = delete;

  UniqueLocalMem(UniqueLocalMem &&other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)) {}

  UniqueLocalMem &operator=(UniqueLocalMem &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.ptr_, nullptr));
    }
    return *this;
  }

  /// \brief Returns the owned pointer or `nullptr`.
  T *get() const noexcept { return ptr_; }

  /// \brief Evaluates true iff a non-null pointer is owned.
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  /// \brief Resets any owned allocation and returns the raw slot for out-params.
  T **put() noexcept {
    reset();
    return &ptr_;
  }

  /// \brief Relinquishes ownership and returns the raw pointer.
  T *release() noexcept { return std::exchange(ptr_, nullptr); }

  /// \brief Frees any owned allocation (unless equal to \p new_ptr) and takes
  /// ownership of \p new_ptr.
  void reset(T *new_ptr = nullptr) noexcept {
    if (new_ptr == ptr_) {
      return;
    }
    if (ptr_ != nullptr) {
      ::LocalFree(ptr_);
    }
    ptr_ = new_ptr;
  }

private:
  T *ptr_ = nullptr;
};

/// \brief Move-only RAII wrapper for `MapViewOfFile` base addresses unmapped
/// via `UnmapViewOfFile`.
class UniqueViewOfFile {
public:
  UniqueViewOfFile() noexcept = default;

  /// \brief Takes ownership of mapped view \p view.
  explicit UniqueViewOfFile(void *view) noexcept : view_(view) {}

  ~UniqueViewOfFile() { reset(); }

  UniqueViewOfFile(const UniqueViewOfFile &) = delete;
  UniqueViewOfFile &operator=(const UniqueViewOfFile &) = delete;

  UniqueViewOfFile(UniqueViewOfFile &&other) noexcept
      : view_(std::exchange(other.view_, nullptr)) {}

  UniqueViewOfFile &operator=(UniqueViewOfFile &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.view_, nullptr));
    }
    return *this;
  }

  /// \brief Returns the mapped view base address or `nullptr`.
  void *get() const noexcept { return view_; }

  /// \brief Evaluates true iff a non-null view is owned.
  explicit operator bool() const noexcept { return view_ != nullptr; }

  /// \brief Unmaps any owned view (unless equal to \p new_view) and takes
  /// ownership of \p new_view.
  void reset(void *new_view = nullptr) noexcept {
    if (new_view == view_) {
      return;
    }
    if (view_ != nullptr) {
      ::UnmapViewOfFile(view_);
    }
    view_ = new_view;
  }

private:
  void *view_ = nullptr;
};

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
