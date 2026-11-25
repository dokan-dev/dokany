// SPDX-License-Identifier: MIT

#pragma once

#include "fstest/core/file_ops.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/status.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fstest::detail {

inline ::testing::AssertionResult CheckNtSuccess(const char *expr,
                                                 NTSTATUS status) {
  if (nt::NtSuccess(status)) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected NT_SUCCESS(" << expr << "), got "
         << core::FormatNtStatus(status);
}

inline ::testing::AssertionResult CheckNtStatus(const char *expr,
                                                const char *expected_expr,
                                                NTSTATUS actual,
                                                NTSTATUS expected) {
  if (actual == expected) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected " << expr << " == " << expected_expr << " ("
         << core::FormatNtStatus(expected) << "), got "
         << core::FormatNtStatus(actual);
}

inline ::testing::AssertionResult CheckWin32True(const char *expr, BOOL ok,
                                                 DWORD last_error) {
  if (ok != FALSE) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected " << expr << " to succeed, failed with "
         << core::FormatWin32Error(last_error);
}

inline ::testing::AssertionResult CheckWin32Error(const char *expr,
                                                  const char *expected_expr,
                                                  BOOL ok, DWORD actual_error,
                                                  DWORD expected_error) {
  if (ok == FALSE && actual_error == expected_error) {
    return ::testing::AssertionSuccess();
  }
  if (ok != FALSE) {
    return ::testing::AssertionFailure()
           << "Expected " << expr << " to fail with " << expected_expr << " ("
           << core::FormatWin32Error(expected_error) << "), but it succeeded.";
  }
  return ::testing::AssertionFailure()
         << "Expected " << expr << " to fail with " << expected_expr << " ("
         << core::FormatWin32Error(expected_error) << "), got "
         << core::FormatWin32Error(actual_error);
}

// Thunk evaluators for EXPECT_WIN32_TRUE / ASSERT_WIN32_TRUE and
// EXPECT_WIN32_ERROR / ASSERT_WIN32_ERROR. Clearing SetLastError before
// evaluating the thunk guarantees stale errors are never misattributed, while
// returning ::testing::AssertionResult through EXPECT_TRUE / ASSERT_TRUE
// preserves clean failure messages and full `<<` diagnostic streaming.
template <typename Fn>
inline ::testing::AssertionResult EvalWin32True(const char *expr, Fn &&fn) {
  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok = fn() ? TRUE : FALSE;
  const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
  return CheckWin32True(expr, ok, err);
}

template <typename Fn>
inline ::testing::AssertionResult
EvalWin32Error(const char *expr, const char *expected_expr, Fn &&fn,
               DWORD expected_error) {
  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok = fn() ? TRUE : FALSE;
  const DWORD err = ::GetLastError();
  return CheckWin32Error(expr, expected_expr, ok, err, expected_error);
}

inline ::testing::AssertionResult CheckWin32ErrorCode(const char *expr,
                                                      const char *expected_expr,
                                                      DWORD actual_error,
                                                      DWORD expected_error) {
  if (actual_error == expected_error) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected " << expr << " == " << expected_expr << " ("
         << core::FormatWin32Error(expected_error) << "), got "
         << core::FormatWin32Error(actual_error);
}

template <typename T>
inline ::testing::AssertionResult
CheckWin32Ok(const char *expr, const core::Win32Result<T> &res) {
  if (res.ok()) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected " << expr << " to succeed, failed with "
         << core::FormatWin32Error(res.error);
}

inline ::testing::AssertionResult
CheckWin32Ok(const char *expr, const core::Win32OpenResult &res) {
  if (res.ok()) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected " << expr << " to succeed, failed with "
         << core::FormatWin32Error(res.last_error);
}

inline std::string FormatElement(const std::wstring &s) {
  return "\"" + core::WideToUtf8(s) + "\"";
}

inline std::string FormatElement(const std::string &s) {
  return "\"" + s + "\"";
}

template <typename T> inline std::string FormatElement(const T &v) {
  std::ostringstream oss;
  oss << v;
  return oss.str();
}

// Strict multiset comparison preserving exact multiplicities (explicitly
// catching duplicate directory/stream entries and reporting missing, extra, and
// duplicated items).
template <typename T>
inline ::testing::AssertionResult
CheckMultisetEq(const char *actual_expr, const char *expected_expr,
                const std::vector<T> &actual, const std::vector<T> &expected) {
  std::map<T, size_t> actual_counts;
  for (const auto &item : actual) {
    ++actual_counts[item];
  }
  std::map<T, size_t> expected_counts;
  for (const auto &item : expected) {
    ++expected_counts[item];
  }

  if (actual_counts == expected_counts) {
    return ::testing::AssertionSuccess();
  }

  std::ostringstream oss;
  oss << "Multiset mismatch between " << actual_expr
      << " (size=" << actual.size() << ") and " << expected_expr
      << " (size=" << expected.size() << "):";

  for (const auto &[key, exp_count] : expected_counts) {
    const auto it = actual_counts.find(key);
    const size_t act_count = (it == actual_counts.end()) ? 0 : it->second;
    if (act_count < exp_count) {
      oss << "\n  missing " << FormatElement(key) << " (expected " << exp_count
          << ", got " << act_count << ")";
    } else if (act_count > exp_count) {
      oss << "\n  duplicate/excess " << FormatElement(key) << " (expected "
          << exp_count << ", got " << act_count << ")";
    }
  }
  for (const auto &[key, act_count] : actual_counts) {
    if (expected_counts.find(key) == expected_counts.end()) {
      oss << "\n  unexpected " << FormatElement(key) << " (count " << act_count
          << ")";
    }
  }
  return ::testing::AssertionFailure() << oss.str();
}

} // namespace fstest::detail

#define EXPECT_NT_SUCCESS(expr)                                                \
  EXPECT_PRED_FORMAT1(::fstest::detail::CheckNtSuccess, (expr))

#define ASSERT_NT_SUCCESS(expr)                                                \
  ASSERT_PRED_FORMAT1(::fstest::detail::CheckNtSuccess, (expr))

#define EXPECT_NT_STATUS(expr, expected)                                       \
  EXPECT_PRED_FORMAT2(::fstest::detail::CheckNtStatus, (expr), (expected))

#define ASSERT_NT_STATUS(expr, expected)                                       \
  ASSERT_PRED_FORMAT2(::fstest::detail::CheckNtStatus, (expr), (expected))

#define EXPECT_WIN32_OK(expr)                                                  \
  EXPECT_PRED_FORMAT1(::fstest::detail::CheckWin32Ok, (expr))

#define ASSERT_WIN32_OK(expr)                                                  \
  ASSERT_PRED_FORMAT1(::fstest::detail::CheckWin32Ok, (expr))

#define EXPECT_WIN32_TRUE(expr)                                                \
  EXPECT_TRUE(::fstest::detail::EvalWin32True(#expr, [&]() { return (expr); }))

#define ASSERT_WIN32_TRUE(expr)                                                \
  ASSERT_TRUE(::fstest::detail::EvalWin32True(#expr, [&]() { return (expr); }))

#define EXPECT_WIN32_ERROR(expr, expected_err)                                 \
  EXPECT_TRUE(::fstest::detail::EvalWin32Error(                                \
      #expr, #expected_err, [&]() { return (expr); },                          \
      static_cast<DWORD>(expected_err)))

#define ASSERT_WIN32_ERROR(expr, expected_err)                                 \
  ASSERT_TRUE(::fstest::detail::EvalWin32Error(                                \
      #expr, #expected_err, [&]() { return (expr); },                          \
      static_cast<DWORD>(expected_err)))

#define EXPECT_WIN32_ERROR_CODE(expr, expected_err)                            \
  EXPECT_PRED_FORMAT2(::fstest::detail::CheckWin32ErrorCode, (expr),           \
                      (expected_err))

#define ASSERT_WIN32_ERROR_CODE(expr, expected_err)                            \
  ASSERT_PRED_FORMAT2(::fstest::detail::CheckWin32ErrorCode, (expr),           \
                      (expected_err))

#define EXPECT_MULTISET_EQ(actual_vec, expected_vec)                           \
  EXPECT_PRED_FORMAT2(::fstest::detail::CheckMultisetEq, (actual_vec),         \
                      (expected_vec))

#define ASSERT_MULTISET_EQ(actual_vec, expected_vec)                           \
  ASSERT_PRED_FORMAT2(::fstest::detail::CheckMultisetEq, (actual_vec),         \
                      (expected_vec))
