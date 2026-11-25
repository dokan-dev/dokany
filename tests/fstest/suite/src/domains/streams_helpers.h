// SPDX-License-Identifier: MIT

#pragma once

// Private helpers shared by the Streams domain translation units
// (streams_test.cpp and streams_ops_test.cpp). Not part of the embedder API.

#include "assertions.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/status.h"

#include "gtest/gtest.h"

#include <string>
#include <string_view>
#include <vector>

namespace fstest::streams_internal {

// Projects stream enumeration entries onto their names (e.g. "::$DATA").
inline std::vector<std::wstring>
ExtractStreamNames(const std::vector<core::StreamEntry> &entries) {
  std::vector<std::wstring> names;
  names.reserve(entries.size());
  for (const auto &entry : entries) {
    names.push_back(entry.name);
  }
  return names;
}

// Enumerates `path` via FindFirstStreamW / FindNextStreamW and compares the
// stream names against `expected` as a strict multiset.
inline ::testing::AssertionResult
CheckStreamNames(std::wstring_view path,
                 const std::vector<std::wstring> &expected) {
  auto streams = core::ListStreamsWin32(path);
  if (!streams) {
    return ::testing::AssertionFailure()
           << "ListStreamsWin32('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(streams.error);
  }
  const std::string label =
      "ListStreamsWin32('" + core::WideToUtf8(path) + "')";
  return detail::CheckMultisetEq(label.c_str(), "expected",
                                 ExtractStreamNames(*streams.value), expected);
}

// Verifies that stream enumeration of `path` fails ERROR_FILE_NOT_FOUND.
inline ::testing::AssertionResult CheckStreamsMissing(std::wstring_view path) {
  auto missing = core::ListStreamsWin32(path);
  if (missing) {
    return ::testing::AssertionFailure()
           << "Expected ListStreamsWin32('" << core::WideToUtf8(path)
           << "') to fail ERROR_FILE_NOT_FOUND, but it succeeded";
  }
  if (missing.error != static_cast<DWORD>(ERROR_FILE_NOT_FOUND)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_FILE_NOT_FOUND for ListStreamsWin32('"
           << core::WideToUtf8(path) << "'), got "
           << core::FormatWin32Error(missing.error);
  }
  return ::testing::AssertionSuccess();
}

// Verifies that opening `path` for read fails ERROR_FILE_NOT_FOUND.
inline ::testing::AssertionResult CheckStreamAbsent(std::wstring_view path) {
  auto missing = core::ReadFileContents(path);
  if (missing) {
    return ::testing::AssertionFailure()
           << "Expected '" << core::WideToUtf8(path)
           << "' to be absent (ERROR_FILE_NOT_FOUND), but it opened";
  }
  if (missing.error != static_cast<DWORD>(ERROR_FILE_NOT_FOUND)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_FILE_NOT_FOUND for '" << core::WideToUtf8(path)
           << "', got " << core::FormatWin32Error(missing.error);
  }
  return ::testing::AssertionSuccess();
}

} // namespace fstest::streams_internal
