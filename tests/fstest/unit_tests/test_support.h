// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include "fstest/core/path.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"
#include "fstest/core/win_headers.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

namespace fstest::test_support {

inline std::wstring GetHostTempDir() {
  wchar_t buf[MAX_PATH + 1]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, buf);
  return (len > 0 && len < MAX_PATH)
             ? core::NormalizeWin32Path(std::wstring_view(buf, len))
             : L"";
}

// RAII wrapper around core::ScopedTempDir created under GetHostTempDir() that
// asserts creation and zero-debris teardown with GoogleTest EXPECT_* checks.
class ScopedTestDirectory {
public:
  explicit ScopedTestDirectory(std::wstring_view prefix = L"fst") {
    const std::wstring host_tmp = GetHostTempDir();
    EXPECT_FALSE(host_tmp.empty());
    if (host_tmp.empty()) {
      return;
    }
    auto dir = core::ScopedTempDir::Create(host_tmp, prefix);
    EXPECT_TRUE(dir.ok()) << core::FormatWin32Error(dir.error);
    if (dir.ok()) {
      dir_ = std::move(*dir.value);
    }
  }

  ~ScopedTestDirectory() {
    if (dir_.has_value()) {
      std::string err;
      EXPECT_TRUE(dir_->Cleanup(&err)) << err;
      dir_.reset();
    }
  }

  ScopedTestDirectory(const ScopedTestDirectory &) = delete;
  ScopedTestDirectory &operator=(const ScopedTestDirectory &) = delete;

  bool ok() const noexcept { return dir_.has_value(); }

  const std::wstring &Path() const {
    static const std::wstring kEmpty;
    EXPECT_TRUE(dir_.has_value());
    return dir_.has_value() ? dir_->Path() : kEmpty;
  }

  const std::wstring &ExtendedPath() const {
    static const std::wstring kEmpty;
    EXPECT_TRUE(dir_.has_value());
    return dir_.has_value() ? dir_->ExtendedPath() : kEmpty;
  }

  std::wstring ChildPath(std::wstring_view rel) const {
    EXPECT_TRUE(dir_.has_value());
    return dir_.has_value() ? dir_->ChildPath(rel) : std::wstring();
  }

  std::wstring ChildExtendedPath(std::wstring_view rel) const {
    EXPECT_TRUE(dir_.has_value());
    return dir_.has_value() ? dir_->ChildExtendedPath(rel) : std::wstring();
  }

  std::string ChildPathUtf8(std::string_view rel) const {
    EXPECT_TRUE(dir_.has_value());
    return dir_.has_value()
               ? core::WideToUtf8(dir_->ChildPath(core::Utf8ToWide(rel)))
               : std::string();
  }

private:
  std::optional<core::ScopedTempDir> dir_;
};

} // namespace fstest::test_support
