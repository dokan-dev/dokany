// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file status.h
/// \brief Lightweight NTSTATUS/Win32 result wrappers, symbolic status
/// formatting, and UTF-16/UTF-8 conversion helpers.

#pragma once

#include "fstest/core/nt_types.h"

#include <optional>
#include <string>
#include <string_view>

namespace fstest::core {

/// \brief Result type pairing an NTSTATUS code, I/O status information word,
/// and an optional success payload.
/// \tparam T Payload type produced on success.
template <typename T> struct NtResult {
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  ULONG_PTR information = 0;
  std::optional<T> value;

  /// \brief Returns true when status is a success NTSTATUS and a value is held.
  bool ok() const noexcept {
    return nt::NtSuccess(status) && value.has_value();
  }

  /// \brief Evaluates true iff ok() is true.
  explicit operator bool() const noexcept { return ok(); }
};

/// \brief Void specialization of NtResult carrying only status and information.
template <> struct NtResult<void> {
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  ULONG_PTR information = 0;

  /// \brief Returns true when status is a success NTSTATUS.
  bool ok() const noexcept { return nt::NtSuccess(status); }

  /// \brief Evaluates true iff ok() is true.
  explicit operator bool() const noexcept { return ok(); }
};

/// \brief Result type pairing a Win32 error code with an optional payload.
/// \tparam T Payload type produced on success.
template <typename T> struct Win32Result {
  DWORD error = ERROR_SUCCESS;
  std::optional<T> value;

  /// \brief Returns true when error is ERROR_SUCCESS and a value is held.
  bool ok() const noexcept {
    return error == ERROR_SUCCESS && value.has_value();
  }

  /// \brief Evaluates true iff ok() is true.
  explicit operator bool() const noexcept { return ok(); }
};

/// \brief Void specialization of Win32Result carrying only a Win32 error code.
template <> struct Win32Result<void> {
  DWORD error = ERROR_SUCCESS;

  /// \brief Returns true when error is ERROR_SUCCESS.
  bool ok() const noexcept { return error == ERROR_SUCCESS; }

  /// \brief Evaluates true iff ok() is true.
  explicit operator bool() const noexcept { return ok(); }
};

/// \brief Returns the symbolic name for an NTSTATUS code (or an 8-digit hex
/// fallback such as `"NTSTATUS_0xC0DE1234"`).
/// \param status NTSTATUS value to look up.
/// \return Symbolic status name.
std::string NtStatusName(NTSTATUS status);

/// \brief Formats an NTSTATUS code as `"STATUS_NAME (0xHEX)"` with an optional
/// system message description from `ntdll.dll`.
/// \param status NTSTATUS value to format.
/// \return Human-readable diagnostic string.
std::string FormatNtStatus(NTSTATUS status);

/// \brief Returns the symbolic name for a Win32 error code (or a numeric
/// fallback such as `"ERROR_999999"`).
/// \param error Win32 error code to look up.
/// \return Symbolic Win32 error name.
std::string Win32ErrorName(DWORD error);

/// \brief Formats a Win32 error code as `"ERROR_NAME (dec / 0xHEX): message"`.
/// \param error Win32 error code to format.
/// \return Human-readable diagnostic string.
std::string FormatWin32Error(DWORD error);

/// \brief Converts a UTF-16 wide string view to UTF-8 via WideCharToMultiByte.
/// \param wide Input UTF-16 view.
/// \return UTF-8 encoded string; empty on empty input or invalid UTF-16
/// (lone surrogates are rejected via `WC_ERR_INVALID_CHARS`, never replaced
/// with U+FFFD).
std::string WideToUtf8(std::wstring_view wide);

/// \brief Converts a UTF-8 string view to UTF-16 via MultiByteToWideChar.
/// \param utf8 Input UTF-8 view.
/// \return UTF-16 encoded wide string; empty on empty input or invalid UTF-8
/// (malformed sequences are rejected via `MB_ERR_INVALID_CHARS`, never
/// replaced with U+FFFD).
std::wstring Utf8ToWide(std::string_view utf8);

} // namespace fstest::core
