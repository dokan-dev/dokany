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
using ShBindToParentFn = HRESULT(WINAPI *)(PCIDLIST_ABSOLUTE, REFIID, void **,
                                           PCUITEMID_CHILD *);
using IlFreeFn = void(WINAPI *)(PIDLIST_RELATIVE);

struct ShellApis {
  HMODULE ole32 = nullptr;
  HMODULE shell32 = nullptr;
  CoInitExFn co_init = nullptr;
  CoUninitFn co_uninit = nullptr;
  CoTaskMemFreeFn co_free = nullptr;
  ShParseDisplayNameFn sh_parse = nullptr;
  ShGetDesktopFolderFn sh_desktop = nullptr;
  ShBindToParentFn sh_bind_to_parent = nullptr;
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
    sh_bind_to_parent = reinterpret_cast<ShBindToParentFn>(
        ::GetProcAddress(shell32, "SHBindToParent"));
    il_free = reinterpret_cast<IlFreeFn>(::GetProcAddress(shell32, "ILFree"));
    return co_init && co_uninit && co_free && sh_parse && sh_desktop &&
           sh_bind_to_parent;
  }

  void FreePidl(PIDLIST_ABSOLUTE pidl) const {
    if (pidl != nullptr) {
      if (il_free != nullptr) {
        il_free(pidl);
      } else {
        co_free(pidl);
      }
    }
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

struct ComInitGuard {
  CoUninitFn uninit = nullptr;
  bool active = false;
  ~ComInitGuard() {
    if (active && uninit != nullptr) {
      uninit();
    }
  }
};

template <typename T> struct ComReleaseGuard {
  T *ptr = nullptr;
  ~ComReleaseGuard() {
    if (ptr != nullptr) {
      ptr->Release();
    }
  }
};

struct PidlGuard {
  const ShellApis *apis = nullptr;
  PIDLIST_ABSOLUTE pidl = nullptr;
  ~PidlGuard() {
    if (apis != nullptr && pidl != nullptr) {
      apis->FreePidl(pidl);
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

  // Declare COM/interface/PIDL guards AFTER ShellApis so destruction releases
  // COM objects and calls CoUninitialize before FreeLibrary(ole32/shell32).
  const HRESULT co_hr = apis.co_init(nullptr, COINIT_APARTMENTTHREADED);
  ComInitGuard co_guard{apis.co_uninit, SUCCEEDED(co_hr)};
  if (FAILED(co_hr) && co_hr != RPC_E_CHANGED_MODE) {
    FAIL() << "CoInitializeEx failed: 0x" << std::hex << co_hr;
  }

  ComReleaseGuard<IShellFolder> desktop;
  HRESULT hr = apis.sh_desktop(&desktop.ptr);
  if (FAILED(hr) || desktop.ptr == nullptr) {
    if (!is_desktop && (hr == REGDB_E_CLASSNOTREG || hr == E_NOTIMPL ||
                        hr == CLASS_E_CLASSNOTAVAILABLE)) {
      GTEST_SKIP() << "Server Core: SHGetDesktopFolder returned 0x" << std::hex
                   << hr;
    }
    FAIL() << "SHGetDesktopFolder failed: 0x" << std::hex << hr;
  }

  // Scoped strictly to our active Dokan UNC path (NEVER enumerate all of
  // CLSID_NetworkPlaces).
  {
    PidlGuard share_pidl{&apis, nullptr};
    hr = apis.sh_parse(mount.unc_name().c_str(), nullptr, &share_pidl.pidl, 0,
                       nullptr);
    if (FAILED(hr) || share_pidl.pidl == nullptr) {
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

    // Bind to parent IShellFolder and query the child's SHGDN_INFOLDER name.
    PCUITEMID_CHILD child_rel = nullptr;
    ComReleaseGuard<IShellFolder> parent_folder;
    hr = apis.sh_bind_to_parent(share_pidl.pidl, IID_IShellFolder,
                                reinterpret_cast<void **>(&parent_folder.ptr),
                                &child_rel);
    EXPECT_TRUE(SUCCEEDED(hr)) << "SHBindToParent failed: 0x" << std::hex << hr;
    if (SUCCEEDED(hr) && parent_folder.ptr != nullptr && child_rel != nullptr) {
      STRRET sr{};
      hr = parent_folder.ptr->GetDisplayNameOf(child_rel, SHGDN_INFOLDER, &sr);
      EXPECT_TRUE(SUCCEEDED(hr))
          << "GetDisplayNameOf failed: 0x" << std::hex << hr;
      if (SUCCEEDED(hr)) {
        const std::wstring display =
            StrRetToWString(sr, child_rel, apis.co_free);
        EXPECT_EQ(_wcsicmp(display.c_str(), L"myshare"), 0)
            << "Unexpected Shell display name: " << WideToUtf8(display);
        EXPECT_EQ(display.find(DOKAN_NP_NAME), std::wstring::npos);
      }
    }
  }

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

  PidlGuard after_pidl{&apis, nullptr};
  const HRESULT after_hr = apis.sh_parse(mount.unc_name().c_str(), nullptr,
                                         &after_pidl.pidl, 0, nullptr);
  if (SUCCEEDED(after_hr) && after_pidl.pidl != nullptr) {
    ADD_FAILURE() << "SHParseDisplayName still resolved unmounted share "
                  << WideToUtf8(mount.unc_name());
  }
}

} // namespace
} // namespace dokan_np_integration
