// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_env.h"

#include <objbase.h>
#include <shlobj.h>

#include <string>

#include <gtest/gtest.h>

namespace dokan_np_integration {
namespace {

using CoInitExFn = HRESULT(WINAPI *)(LPVOID, DWORD);
using CoUninitFn = void(WINAPI *)();
using CoTaskMemFreeFn = void(WINAPI *)(LPVOID);
using ShParseDisplayNameFn = HRESULT(WINAPI *)(PCWSTR, IBindCtx *,
                                               PIDLIST_ABSOLUTE *, SFGAOF,
                                               SFGAOF *);
using ShGetDesktopFolderFn = HRESULT(WINAPI *)(IShellFolder **);
using IlFreeFn = void(WINAPI *)(PIDLIST_RELATIVE);

struct ShellApis {
  HMODULE ole32 = nullptr;
  HMODULE shell32 = nullptr;
  CoInitExFn co_init = nullptr;
  CoUninitFn co_uninit = nullptr;
  CoTaskMemFreeFn co_free = nullptr;
  ShParseDisplayNameFn sh_parse = nullptr;
  ShGetDesktopFolderFn sh_desktop = nullptr;
  IlFreeFn il_free = nullptr;

  bool Load() {
    ole32 = ::LoadLibraryW(L"ole32.dll");
    shell32 = ::LoadLibraryW(L"shell32.dll");
    if (ole32 == nullptr || shell32 == nullptr) {
      return false;
    }
    co_init =
        reinterpret_cast<CoInitExFn>(::GetProcAddress(ole32, "CoInitializeEx"));
    co_uninit =
        reinterpret_cast<CoUninitFn>(::GetProcAddress(ole32, "CoUninitialize"));
    co_free = reinterpret_cast<CoTaskMemFreeFn>(
        ::GetProcAddress(ole32, "CoTaskMemFree"));
    sh_parse = reinterpret_cast<ShParseDisplayNameFn>(
        ::GetProcAddress(shell32, "SHParseDisplayName"));
    sh_desktop = reinterpret_cast<ShGetDesktopFolderFn>(
        ::GetProcAddress(shell32, "SHGetDesktopFolder"));
    il_free = reinterpret_cast<IlFreeFn>(::GetProcAddress(shell32, "ILFree"));
    return co_init && co_uninit && co_free && sh_parse && sh_desktop;
  }

  ~ShellApis() {
    if (shell32 != nullptr) {
      ::FreeLibrary(shell32);
    }
    if (ole32 != nullptr) {
      ::FreeLibrary(ole32);
    }
  }
};

std::wstring StrRetToWString(const STRRET &sr, PCUITEMID_CHILD pidl,
                             CoTaskMemFreeFn co_free) {
  std::wstring out;
  switch (sr.uType) {
  case STRRET_WSTR:
    if (sr.pOleStr != nullptr) {
      out = sr.pOleStr;
      co_free(sr.pOleStr);
    }
    break;
  case STRRET_OFFSET:
    if (pidl != nullptr && sr.uOffset < pidl->mkid.cb) {
      const char *ptr = reinterpret_cast<const char *>(pidl) + sr.uOffset;
      const size_t max_bytes = pidl->mkid.cb - sr.uOffset;
      const size_t nbytes = strnlen(ptr, max_bytes);
      const int wlen = ::MultiByteToWideChar(
          CP_ACP, 0, ptr, static_cast<int>(nbytes), nullptr, 0);
      if (wlen > 0) {
        out.resize(static_cast<size_t>(wlen));
        ::MultiByteToWideChar(CP_ACP, 0, ptr, static_cast<int>(nbytes),
                              out.data(), wlen);
      }
    }
    break;
  case STRRET_CSTR: {
    const size_t nbytes = strnlen(sr.cStr, sizeof(sr.cStr));
    const int len = ::MultiByteToWideChar(CP_ACP, 0, sr.cStr,
                                          static_cast<int>(nbytes), nullptr, 0);
    if (len > 0) {
      out.resize(static_cast<size_t>(len));
      ::MultiByteToWideChar(CP_ACP, 0, sr.cStr, static_cast<int>(nbytes),
                            out.data(), len);
    }
    break;
  }
  default:
    break;
  }
  return out;
}

TEST(ShellNamespaceTest, ScopedShareDisplayNameAndDisappearanceAfterUnmount) {
  MirrorMount mount;
  std::wstring err;
  ASSERT_TRUE(mount.Start(L"dokantest_sh", L"myshare",
                          /*enable_network_unmount=*/true,
                          /*current_session_only=*/false, &err))
      << WideToUtf8(err);

  // Always enforce WNetFormatNetworkNameW regardless of Server Core vs Desktop.
  {
    WCHAR fmt[128]{};
    DWORD len = 128;
    WCHAR prov[] = DOKAN_NP_NAME;
    std::wstring unc = mount.unc_name();
    ASSERT_EQ(CallWNetFormatNetworkNameW(prov, unc.data(), fmt, &len,
                                         WNFMT_INENUM, 80),
              static_cast<DWORD>(NO_ERROR));
    EXPECT_STREQ(fmt, L"myshare");
  }

  if (GetTestConfig().skip_shell_check) {
    GTEST_SKIP() << "--skip_shell_check set";
  }

  const bool is_desktop = IsDesktopShellAvailable();
  ShellApis apis;
  if (!apis.Load()) {
    if (!is_desktop) {
      GTEST_SKIP() << "Server Core: ole32/shell32 Shell APIs unavailable";
    }
    FAIL() << "Failed to load ole32/shell32 Shell APIs on Desktop Windows";
  }

  const HRESULT co_hr = apis.co_init(nullptr, COINIT_APARTMENTTHREADED);
  const bool need_uninit = SUCCEEDED(co_hr);
  if (FAILED(co_hr) && co_hr != RPC_E_CHANGED_MODE) {
    FAIL() << "CoInitializeEx failed: 0x" << std::hex << co_hr;
  }

  auto free_pidl = [&](PIDLIST_ABSOLUTE pidl) {
    if (pidl != nullptr) {
      if (apis.il_free != nullptr) {
        apis.il_free(pidl);
      } else {
        apis.co_free(pidl);
      }
    }
  };

  IShellFolder *desktop = nullptr;
  HRESULT hr = apis.sh_desktop(&desktop);
  if (FAILED(hr) || desktop == nullptr) {
    if (need_uninit) {
      apis.co_uninit();
    }
    if (!is_desktop && (hr == REGDB_E_CLASSNOTREG || hr == E_NOTIMPL ||
                        hr == CLASS_E_CLASSNOTAVAILABLE)) {
      GTEST_SKIP() << "Server Core: SHGetDesktopFolder returned 0x" << std::hex
                   << hr;
    }
    FAIL() << "SHGetDesktopFolder failed: 0x" << std::hex << hr;
  }

  // Scoped strictly to our active Dokan UNC path (NEVER enumerate all of
  // CLSID_NetworkPlaces).
  PIDLIST_ABSOLUTE share_pidl = nullptr;
  hr =
      apis.sh_parse(mount.unc_name().c_str(), nullptr, &share_pidl, 0, nullptr);
  if (FAILED(hr) || share_pidl == nullptr) {
    desktop->Release();
    if (need_uninit) {
      apis.co_uninit();
    }
    if (!is_desktop && (hr == REGDB_E_CLASSNOTREG || hr == E_NOTIMPL ||
                        hr == CLASS_E_CLASSNOTAVAILABLE ||
                        hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))) {
      GTEST_SKIP() << "Server Core: Shell network namespace unavailable for "
                      "SHParseDisplayName (hr=0x"
                   << std::hex << hr << ")";
    }
    FAIL() << "SHParseDisplayName(" << WideToUtf8(mount.unc_name())
           << ") failed: 0x" << std::hex << hr;
  }

  // Walk to parent IShellFolder and ask SHGDN_INFOLDER display name.
  PCUITEMID_CHILD child_rel = nullptr;
  IShellFolder *parent_folder = nullptr;
  // Find SHBindToParent dynamically on shell32 so Server Core imports remain
  // zero-risk.
  using ShBindToParentFn =
      HRESULT(WINAPI *)(PCIDLIST_ABSOLUTE, REFIID, void **, PCUITEMID_CHILD *);
  auto sh_bind_to_parent = reinterpret_cast<ShBindToParentFn>(
      ::GetProcAddress(apis.shell32, "SHBindToParent"));
  if (sh_bind_to_parent != nullptr) {
    hr = sh_bind_to_parent(share_pidl, IID_IShellFolder,
                           reinterpret_cast<void **>(&parent_folder),
                           &child_rel);
    EXPECT_TRUE(SUCCEEDED(hr)) << "SHBindToParent failed: 0x" << std::hex << hr;
    if (SUCCEEDED(hr) && parent_folder != nullptr && child_rel != nullptr) {
      STRRET sr{};
      hr = parent_folder->GetDisplayNameOf(child_rel, SHGDN_INFOLDER, &sr);
      EXPECT_TRUE(SUCCEEDED(hr))
          << "GetDisplayNameOf failed: 0x" << std::hex << hr;
      if (SUCCEEDED(hr)) {
        const std::wstring display =
            StrRetToWString(sr, child_rel, apis.co_free);
        EXPECT_EQ(_wcsicmp(display.c_str(), L"myshare"), 0)
            << "Unexpected Shell display name: " << WideToUtf8(display);
        EXPECT_EQ(display.find(DOKAN_NP_NAME), std::wstring::npos);
      }
      parent_folder->Release();
    }
  } else {
    STRRET sr{};
    hr = desktop->GetDisplayNameOf(share_pidl, SHGDN_INFOLDER, &sr);
    EXPECT_TRUE(SUCCEEDED(hr))
        << "desktop->GetDisplayNameOf failed: 0x" << std::hex << hr;
    if (SUCCEEDED(hr)) {
      const std::wstring display = StrRetToWString(
          sr, reinterpret_cast<PCUITEMID_CHILD>(share_pidl), apis.co_free);
      EXPECT_EQ(_wcsicmp(display.c_str(), L"myshare"), 0)
          << "Unexpected Shell display name: " << WideToUtf8(display);
    }
  }
  free_pidl(share_pidl);

  // Unmount via WNetCancelConnection2W and assert disappearance (#952, #1326).
  ASSERT_EQ(::WNetCancelConnection2W(mount.drive_name().c_str(), 0, FALSE),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_TRUE(mount.WaitForExit(10000));

  std::vector<uint8_t> info_buf(1024);
  DWORD info_sz = static_cast<DWORD>(info_buf.size());
  LPWSTR sys_part = nullptr;
  std::wstring unc_copy = mount.unc_name();
  NETRESOURCEW nr{};
  nr.dwType = RESOURCETYPE_DISK;
  nr.lpRemoteName = unc_copy.data();
  EXPECT_NE(
      ::WNetGetResourceInformationW(&nr, info_buf.data(), &info_sz, &sys_part),
      static_cast<DWORD>(NO_ERROR));

  PIDLIST_ABSOLUTE after_pidl = nullptr;
  const HRESULT after_hr =
      apis.sh_parse(mount.unc_name().c_str(), nullptr, &after_pidl, 0, nullptr);
  if (SUCCEEDED(after_hr) && after_pidl != nullptr) {
    free_pidl(after_pidl);
    ADD_FAILURE() << "SHParseDisplayName still resolved unmounted share "
                  << WideToUtf8(mount.unc_name());
  }

  desktop->Release();
  if (need_uninit) {
    apis.co_uninit();
  }
}

} // namespace
} // namespace dokan_np_integration
