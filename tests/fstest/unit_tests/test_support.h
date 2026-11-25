// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#pragma once

#include "fstest/core/path.h"
#include "fstest/core/win_headers.h"

#include <string>
#include <string_view>

namespace fstest::test_support {

inline std::wstring GetHostTempDir() {
  wchar_t buf[MAX_PATH + 1]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, buf);
  return (len > 0 && len < MAX_PATH)
             ? core::NormalizeWin32Path(std::wstring_view(buf, len))
             : L"";
}

} // namespace fstest::test_support
