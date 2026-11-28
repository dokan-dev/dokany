// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_env.h"
#include "np_registry.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace dokan_np_integration {
namespace {

bool ReadEntireFile(const std::wstring &path, std::vector<uint8_t> *out) {
  HANDLE hf =
      ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hf == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER sz{};
  if (!::GetFileSizeEx(hf, &sz) || sz.QuadPart <= 0 ||
      sz.QuadPart > 64LL * 1024 * 1024) {
    ::CloseHandle(hf);
    return false;
  }
  out->resize(static_cast<size_t>(sz.QuadPart));
  DWORD read_bytes = 0;
  const BOOL ok = ::ReadFile(hf, out->data(), static_cast<DWORD>(out->size()),
                             &read_bytes, nullptr);
  ::CloseHandle(hf);
  if (!ok || read_bytes != out->size()) {
    out->clear();
    return false;
  }
  return true;
}

bool RvaToFileOffset(DWORD rva, const IMAGE_SECTION_HEADER *sections,
                     WORD num_sections, size_t file_size, size_t *offset_out) {
  if (rva == 0) {
    return false;
  }
  for (WORD i = 0; i < num_sections; ++i) {
    const DWORD va = sections[i].VirtualAddress;
    const DWORD raw_size = sections[i].SizeOfRawData;
    const DWORD raw_ptr = sections[i].PointerToRawData;
    if (raw_size == 0 ||
        static_cast<uint64_t>(raw_ptr) + raw_size > file_size) {
      continue;
    }
    if (rva >= va && rva - va < raw_size) {
      *offset_out = static_cast<size_t>(raw_ptr) + (rva - va);
      return true;
    }
  }
  return false;
}

bool ReadCStringAtRva(const std::vector<uint8_t> &bytes, DWORD rva,
                      const IMAGE_SECTION_HEADER *sections, WORD num_sections,
                      std::string *out) {
  size_t off = 0;
  if (!RvaToFileOffset(rva, sections, num_sections, bytes.size(), &off)) {
    return false;
  }
  out->clear();
  while (off < bytes.size() && bytes[off] != 0) {
    out->push_back(static_cast<char>(bytes[off]));
    ++off;
  }
  return off < bytes.size();
}

bool ParsePeImports(const std::wstring &path, std::vector<std::string> *imports,
                    std::vector<std::string> *delay_imports,
                    std::string *error_out) {
  imports->clear();
  delay_imports->clear();

  std::vector<uint8_t> bytes;
  if (!ReadEntireFile(path, &bytes)) {
    *error_out = "Failed to read PE file: " + WideToUtf8(path);
    return false;
  }
  if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
    *error_out = "File smaller than IMAGE_DOS_HEADER";
    return false;
  }
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(bytes.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    *error_out = "Invalid DOS header";
    return false;
  }
  const size_t nt_off = static_cast<size_t>(dos->e_lfanew);
  if (nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > bytes.size()) {
    *error_out = "Truncated NT header";
    return false;
  }
  DWORD pe_sig = 0;
  std::memcpy(&pe_sig, bytes.data() + nt_off, sizeof(DWORD));
  if (pe_sig != IMAGE_NT_SIGNATURE) {
    *error_out = "Invalid PE signature";
    return false;
  }

  const auto *fh = reinterpret_cast<const IMAGE_FILE_HEADER *>(
      bytes.data() + nt_off + sizeof(DWORD));
  const size_t opt_off = nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
  const size_t sec_off = opt_off + fh->SizeOfOptionalHeader;
  if (sec_off + fh->NumberOfSections * sizeof(IMAGE_SECTION_HEADER) >
      bytes.size()) {
    *error_out = "Truncated section headers";
    return false;
  }
  const auto *sections =
      reinterpret_cast<const IMAGE_SECTION_HEADER *>(bytes.data() + sec_off);

  if (fh->SizeOfOptionalHeader < sizeof(WORD)) {
    *error_out = "Missing OptionalHeader";
    return false;
  }
  WORD magic = 0;
  std::memcpy(&magic, bytes.data() + opt_off, sizeof(WORD));

  const IMAGE_DATA_DIRECTORY *dirs = nullptr;
  DWORD num_dirs = 0;
  if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
      fh->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
    const auto *opt32 = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32 *>(
        bytes.data() + opt_off);
    dirs = opt32->DataDirectory;
    num_dirs = opt32->NumberOfRvaAndSizes;
  } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
             fh->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
    const auto *opt64 = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64 *>(
        bytes.data() + opt_off);
    dirs = opt64->DataDirectory;
    num_dirs = opt64->NumberOfRvaAndSizes;
  } else {
    *error_out = "Unsupported PE OptionalHeader magic";
    return false;
  }

  if (num_dirs > IMAGE_DIRECTORY_ENTRY_IMPORT &&
      dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress != 0) {
    size_t imp_off = 0;
    if (!RvaToFileOffset(dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress,
                         sections, fh->NumberOfSections, bytes.size(),
                         &imp_off)) {
      *error_out = "Invalid Import Directory RVA";
      return false;
    }
    while (imp_off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= bytes.size()) {
      IMAGE_IMPORT_DESCRIPTOR desc{};
      std::memcpy(&desc, bytes.data() + imp_off, sizeof(desc));
      if (desc.Name == 0 && desc.FirstThunk == 0 &&
          desc.OriginalFirstThunk == 0) {
        break;
      }
      std::string dll_name;
      if (!ReadCStringAtRva(bytes, desc.Name, sections, fh->NumberOfSections,
                            &dll_name)) {
        *error_out = "Invalid import DLL name RVA";
        return false;
      }
      imports->push_back(dll_name);
      imp_off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }
  }

  if (num_dirs > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT &&
      dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress != 0) {
    size_t dimp_off = 0;
    if (!RvaToFileOffset(
            dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress, sections,
            fh->NumberOfSections, bytes.size(), &dimp_off)) {
      *error_out = "Invalid Delay-Load Import Directory RVA";
      return false;
    }
    while (dimp_off + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= bytes.size()) {
      IMAGE_DELAYLOAD_DESCRIPTOR desc{};
      std::memcpy(&desc, bytes.data() + dimp_off, sizeof(desc));
      if (desc.DllNameRVA == 0) {
        break;
      }
      std::string dll_name;
      if (!ReadCStringAtRva(bytes, desc.DllNameRVA, sections,
                            fh->NumberOfSections, &dll_name)) {
        *error_out = "Invalid delay-load import DLL name RVA";
        return false;
      }
      delay_imports->push_back(dll_name);
      dimp_off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
    }
  }
  return true;
}

void AssertDokanNpImportsKernel32Only(const std::wstring &dll_path) {
  std::vector<std::string> imports;
  std::vector<std::string> delay_imports;
  std::string err;
  ASSERT_TRUE(ParsePeImports(dll_path, &imports, &delay_imports, &err)) << err;
  ASSERT_EQ(imports.size(), 1u)
      << WideToUtf8(dll_path) << " imports unexpected DLLs";
  EXPECT_EQ(_stricmp(imports[0].c_str(), "KERNEL32.dll"), 0)
      << "Unexpected import: " << imports[0];
  EXPECT_TRUE(delay_imports.empty())
      << WideToUtf8(dll_path) << " has delay-load imports";
}

TEST(HygieneTest, InstalledDokanNpImportsOnlyKernel32) {
  const std::wstring dll_name = L"dokannp" DOKAN_MAJOR_API_VERSION L".dll";
  const TestConfig &cfg = GetTestConfig();
  if (!cfg.dokannp_dll.empty()) {
    ASSERT_NE(::GetFileAttributesW(cfg.dokannp_dll.c_str()),
              INVALID_FILE_ATTRIBUTES)
        << WideToUtf8(cfg.dokannp_dll);
    AssertDokanNpImportsKernel32Only(cfg.dokannp_dll);
    return;
  }

  WCHAR sys_dir[MAX_PATH]{};
  ASSERT_GT(::GetSystemDirectoryW(sys_dir, MAX_PATH), 0u);
  const std::wstring native_path = std::wstring(sys_dir) + L"\\" + dll_name;

  // Disable WOW64 FS redirection when running as 32-bit process on 64-bit OS
  // so System32 inspects the real 64-bit dokannp3.dll too.
  using Wow64DisableFn = BOOL(WINAPI *)(PVOID *);
  using Wow64RevertFn = BOOL(WINAPI *)(PVOID);
  using GetWow64DirFn = UINT(WINAPI *)(LPWSTR, UINT);
  HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
  auto wow_disable = k32 ? reinterpret_cast<Wow64DisableFn>(::GetProcAddress(
                               k32, "Wow64DisableWow64FsRedirection"))
                         : nullptr;
  auto wow_revert = k32 ? reinterpret_cast<Wow64RevertFn>(::GetProcAddress(
                              k32, "Wow64RevertWow64FsRedirection"))
                        : nullptr;
  auto get_wow64_dir = k32 ? reinterpret_cast<GetWow64DirFn>(::GetProcAddress(
                                 k32, "GetSystemWow64DirectoryW"))
                           : nullptr;

  struct Wow64RedirGuard {
    Wow64RevertFn revert = nullptr;
    PVOID old_value = nullptr;
    bool disabled = false;
    ~Wow64RedirGuard() {
      if (disabled && revert != nullptr) {
        revert(old_value);
      }
    }
  };

  bool checked_any = false;
  {
    Wow64RedirGuard redir_guard;
    redir_guard.revert = wow_revert;
    if (wow_disable != nullptr && wow_disable(&redir_guard.old_value)) {
      redir_guard.disabled = true;
    }
    const DWORD attrs = ::GetFileAttributesW(native_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
      AssertDokanNpImportsKernel32Only(native_path);
      checked_any = true;
    }
  }

  if (get_wow64_dir != nullptr) {
    WCHAR wow_dir[MAX_PATH]{};
    if (get_wow64_dir(wow_dir, MAX_PATH) > 0) {
      const std::wstring wow_path = std::wstring(wow_dir) + L"\\" + dll_name;
      if (::GetFileAttributesW(wow_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        AssertDokanNpImportsKernel32Only(wow_path);
        checked_any = true;
      }
    }
  }

  if (!checked_any) {
    GTEST_SKIP() << "Installed " << WideToUtf8(dll_name)
                 << " not found in System32/SysWOW64 (pass --dokannp_dll=...)";
  }
}

TEST(HygieneTest, ZeroReleaseOutputDebugStringSpamAcrossWNetCalls) {
  // Declared before dbwin so ~DbwinGuard() joins listener before captured_self
  // is destroyed even on early ASSERT_* returns.
  std::vector<std::string> captured_self;
  struct DbwinGuard {
    HANDLE buf_ready = nullptr;
    HANDLE data_ready = nullptr;
    HANDLE mapping = nullptr;
    void *view = nullptr;
    std::atomic<bool> stop{false};
    std::thread listener;

    ~DbwinGuard() {
      stop.store(true);
      if (listener.joinable()) {
        listener.join();
      }
      if (view != nullptr) {
        ::UnmapViewOfFile(view);
      }
      if (mapping != nullptr) {
        ::CloseHandle(mapping);
      }
      if (data_ready != nullptr) {
        ::CloseHandle(data_ready);
      }
      if (buf_ready != nullptr) {
        ::CloseHandle(buf_ready);
      }
    }
  } dbwin;

  dbwin.buf_ready =
      ::CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY");
  if (dbwin.buf_ready == nullptr || ::GetLastError() == ERROR_ALREADY_EXISTS) {
    GTEST_SKIP() << "DBWIN_BUFFER_READY already owned by another debugger";
  }
  dbwin.data_ready = ::CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY");
  dbwin.mapping = ::CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, L"DBWIN_BUFFER");
  if (dbwin.data_ready == nullptr || dbwin.mapping == nullptr ||
      ::GetLastError() == ERROR_ALREADY_EXISTS) {
    GTEST_SKIP() << "DBWIN_BUFFER already held by another debugger";
  }

  dbwin.view = ::MapViewOfFile(dbwin.mapping, FILE_MAP_READ, 0, 0, 4096);
  if (dbwin.view == nullptr) {
    GTEST_SKIP() << "MapViewOfFile(DBWIN_BUFFER) failed";
  }

  const DWORD self_pid = ::GetCurrentProcessId();
  HANDLE listener_ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(listener_ready, nullptr);
  dbwin.listener = std::thread([&]() {
    ::SetEvent(dbwin.buf_ready);
    ::SetEvent(listener_ready);
    while (!dbwin.stop.load()) {
      const DWORD rc = ::WaitForSingleObject(dbwin.data_ready, 50);
      if (rc == WAIT_OBJECT_0) {
        DWORD msg_pid = 0;
        std::memcpy(&msg_pid, dbwin.view, sizeof(DWORD));
        const char *text =
            static_cast<const char *>(dbwin.view) + sizeof(DWORD);
        const size_t max_len = 4096 - sizeof(DWORD);
        const size_t len = strnlen(text, max_len);
        if (msg_pid == self_pid) {
          captured_self.emplace_back(text, len);
        }
        ::SetEvent(dbwin.buf_ready);
      }
    }
  });
  ::WaitForSingleObject(listener_ready, 5000);
  ::CloseHandle(listener_ready);

  {
    MirrorMount mount;
    std::wstring err;
    ASSERT_TRUE(mount.Start(L"dokantest_dbg", L"quietshare",
                            /*enable_network_unmount=*/true,
                            /*current_session_only=*/false, &err))
        << WideToUtf8(err);
    WCHAR conn[256]{};
    DWORD conn_len = 256;
    EXPECT_EQ(::WNetGetConnectionW(mount.drive_name().c_str(), conn, &conn_len),
              static_cast<DWORD>(NO_ERROR));

    uint8_t uni_buf[1024]{};
    DWORD uni_sz = sizeof(uni_buf);
    EXPECT_EQ(
        ::WNetGetUniversalNameW((mount.drive_root() + L"probe.txt").c_str(),
                                UNIVERSAL_NAME_INFO_LEVEL, uni_buf, &uni_sz),
        static_cast<DWORD>(NO_ERROR));

    HANDLE h = nullptr;
    ASSERT_EQ(
        ::WNetOpenEnumW(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, nullptr, &h),
        static_cast<DWORD>(NO_ERROR));
    uint8_t enum_buf[4096]{};
    DWORD cnt = 0xFFFFFFFF;
    DWORD esz = sizeof(enum_buf);
    const DWORD enum_rc = ::WNetEnumResourceW(h, &cnt, enum_buf, &esz);
    EXPECT_TRUE(enum_rc == NO_ERROR || enum_rc == ERROR_NO_MORE_ITEMS);
    ::WNetCloseEnum(h);

    std::wstring unc = mount.unc_name();
    WCHAR prov[64]{};
    wcscpy_s(prov, DOKAN_NP_NAME);
    NETRESOURCEW nr{};
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpRemoteName = unc.data();
    nr.lpProvider = prov;
    LPWSTR sys_part = nullptr;
    DWORD info_sz = sizeof(uni_buf);
    EXPECT_EQ(::WNetGetResourceInformationW(&nr, uni_buf, &info_sz, &sys_part),
              static_cast<DWORD>(NO_ERROR));
    info_sz = sizeof(uni_buf);
    EXPECT_EQ(::WNetGetResourceParentW(&nr, uni_buf, &info_sz),
              static_cast<DWORD>(NO_ERROR));

    WCHAR fmt[64]{};
    DWORD fmt_len = 64;
    EXPECT_EQ(CallWNetFormatNetworkNameW(prov, unc.data(), fmt, &fmt_len,
                                         WNFMT_INENUM, 80),
              static_cast<DWORD>(NO_ERROR));
  }

  dbwin.stop.store(true);
  dbwin.listener.join();

  EXPECT_TRUE(captured_self.empty())
      << "Unexpected OutputDebugString emitted during WNet calls: "
      << (captured_self.empty() ? "" : captured_self[0]);
}

TEST(HygieneTest, RegistryInstallUninstallRoundTrip) {
  const TestConfig &cfg = GetTestConfig();
  if (cfg.skip_registry_roundtrip) {
    GTEST_SKIP() << "--skip_registry_roundtrip set";
  }

  // Always run isolated HKCU round-trip.
  const std::wstring subkey =
      L"Software\\DokanNpIntTest_" + std::to_wstring(::GetCurrentProcessId());
  HKEY hkcu_root = nullptr;
  ASSERT_EQ(::RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                              &hkcu_root, nullptr),
            ERROR_SUCCESS);
  struct HkcuGuard {
    HKEY h;
    std::wstring path;
    ~HkcuGuard() {
      if (h != nullptr) {
        ::RegCloseKey(h);
      }
      ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
  } hkcu_guard{hkcu_root, subkey};

  HKEY order_key = nullptr;
  ASSERT_EQ(::RegCreateKeyExW(
                hkcu_root,
                L"System\\CurrentControlSet\\Control\\NetworkProvider\\Order",
                0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                &order_key, nullptr),
            ERROR_SUCCESS);
  const wchar_t seed[] = L"LanmanWorkstation,RDPNP,Dokan2";
  ASSERT_EQ(::RegSetValueExW(order_key, L"ProviderOrder", 0, REG_SZ,
                             reinterpret_cast<const BYTE *>(seed),
                             sizeof(seed)),
            ERROR_SUCCESS);
  ::RegCloseKey(order_key);

  auto read_hkcu_order = [&]() -> std::wstring {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(
            hkcu_root,
            L"System\\CurrentControlSet\\Control\\NetworkProvider\\Order", 0,
            KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) {
      return L"";
    }
    WCHAR buf[512]{};
    DWORD sz = sizeof(buf);
    const DWORD rc = ::RegQueryValueExW(k, L"ProviderOrder", nullptr, nullptr,
                                        reinterpret_cast<BYTE *>(buf), &sz);
    ::RegCloseKey(k);
    return rc == ERROR_SUCCESS ? std::wstring(buf) : L"";
  };

  EXPECT_EQ(DokanNetworkProviderInstallAt(hkcu_root),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(read_hkcu_order(), L"LanmanWorkstation,RDPNP,Dokan2,Dokan3");
  EXPECT_EQ(DokanNetworkProviderInstallAt(hkcu_root),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(read_hkcu_order(), L"LanmanWorkstation,RDPNP,Dokan2,Dokan3");
  EXPECT_EQ(DokanNetworkProviderUninstallAt(hkcu_root),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(read_hkcu_order(), L"LanmanWorkstation,RDPNP,Dokan2");
  EXPECT_EQ(DokanNetworkProviderInstallAt(hkcu_root),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(read_hkcu_order(), L"LanmanWorkstation,RDPNP,Dokan2,Dokan3");

  // Live HKLM round-trip only when explicitly requested in VM.
  if (cfg.test_live_registry) {
    if (!IsProcessElevated()) {
      GTEST_SKIP() << "--test_live_registry requires elevation";
    }
    HKEY live_order = nullptr;
    ASSERT_EQ(::RegOpenKeyExW(
                  HKEY_LOCAL_MACHINE,
                  L"SYSTEM\\CurrentControlSet\\Control\\NetworkProvider\\Order",
                  0, KEY_QUERY_VALUE | KEY_SET_VALUE, &live_order),
              ERROR_SUCCESS);
    DWORD type = 0;
    DWORD bytes = 0;
    ASSERT_EQ(::RegQueryValueExW(live_order, L"ProviderOrder", nullptr, &type,
                                 nullptr, &bytes),
              ERROR_SUCCESS);
    std::vector<WCHAR> orig_buf(bytes / sizeof(WCHAR) + 1, L'\0');
    ASSERT_EQ(::RegQueryValueExW(live_order, L"ProviderOrder", nullptr, &type,
                                 reinterpret_cast<BYTE *>(orig_buf.data()),
                                 &bytes),
              ERROR_SUCCESS);
    const std::wstring orig_order(orig_buf.data());
    EXPECT_NE(orig_order.find(DOKAN_NP_NAME), std::wstring::npos)
        << "Live ProviderOrder missing Dokan3: " << WideToUtf8(orig_order);

    struct LiveRestoreGuard {
      HKEY k;
      DWORD t;
      std::vector<WCHAR> data;
      DWORD b;
      ~LiveRestoreGuard() {
        ::RegSetValueExW(k, L"ProviderOrder", 0, t,
                         reinterpret_cast<const BYTE *>(data.data()), b);
        ::RegCloseKey(k);
        DokanNetworkProviderInstallAt(HKEY_LOCAL_MACHINE);
      }
    } live_guard{live_order, type, orig_buf, bytes};

    ASSERT_EQ(DokanNetworkProviderUninstallAt(HKEY_LOCAL_MACHINE),
              static_cast<DWORD>(ERROR_SUCCESS));
    WCHAR after_un[2048]{};
    DWORD after_sz = sizeof(after_un);
    ASSERT_EQ(::RegQueryValueExW(live_order, L"ProviderOrder", nullptr, nullptr,
                                 reinterpret_cast<BYTE *>(after_un), &after_sz),
              ERROR_SUCCESS);
    const std::wstring after_str(after_un);
    if (orig_order.find(L"LanmanWorkstation") != std::wstring::npos) {
      EXPECT_NE(after_str.find(L"LanmanWorkstation"), std::wstring::npos);
    }
    if (orig_order.find(L"RDPNP") != std::wstring::npos) {
      EXPECT_NE(after_str.find(L"RDPNP"), std::wstring::npos);
    }

    ASSERT_EQ(DokanNetworkProviderInstallAt(HKEY_LOCAL_MACHINE),
              static_cast<DWORD>(ERROR_SUCCESS));
    WCHAR after_re[2048]{};
    DWORD re_sz = sizeof(after_re);
    ASSERT_EQ(::RegQueryValueExW(live_order, L"ProviderOrder", nullptr, nullptr,
                                 reinterpret_cast<BYTE *>(after_re), &re_sz),
              ERROR_SUCCESS);
    EXPECT_NE(std::wstring(after_re).find(DOKAN_NP_NAME), std::wstring::npos);
  }
}

} // namespace
} // namespace dokan_np_integration
