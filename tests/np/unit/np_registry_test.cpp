// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_support.h"

#include <cstring>
#include <string>
#include <vector>

namespace dokan_np_test {
namespace {

constexpr wchar_t kServiceNpKey[] =
    L"System\\CurrentControlSet\\Services\\doka"
    L"n" DOKAN_MAJOR_API_VERSION L"\\NetworkProvider";
constexpr wchar_t kServiceKey[] =
    L"System\\CurrentControlSet\\Services\\dokan" DOKAN_MAJOR_API_VERSION;
constexpr wchar_t kOrderKey[] =
    L"System\\CurrentControlSet\\Control\\NetworkProvider\\Order";
constexpr wchar_t kHwOrderKey[] =
    L"System\\CurrentControlSet\\Control\\NetworkProvider\\HwOrder";

class ScopedHkcuSandbox : public ::testing::Test {
protected:
  void SetUp() override {
    // Build a deterministic unique HKCU subkey without requiring Rpcrt4.
    const DWORD pid = ::GetCurrentProcessId();
    const DWORD tick = ::GetTickCount();
    static DWORD seq = 0;
    ++seq;
    subkey_path_ = L"Software\\DokanNpUnitTest\\run_" + std::to_wstring(pid) +
                   L"_" + std::to_wstring(tick) + L"_" + std::to_wstring(seq);
    ASSERT_EQ(::RegCreateKeyExW(HKEY_CURRENT_USER, subkey_path_.c_str(), 0,
                                nullptr, REG_OPTION_NON_VOLATILE,
                                KEY_ALL_ACCESS, nullptr, &root_, nullptr),
              ERROR_SUCCESS);
  }

  void TearDown() override {
    if (root_ != nullptr) {
      ::RegCloseKey(root_);
      root_ = nullptr;
    }
    if (!subkey_path_.empty()) {
      ::RegDeleteTreeW(HKEY_CURRENT_USER, subkey_path_.c_str());
    }
  }

  void SeedOrderValue(LPCWSTR key_path, DWORD type, const void *data,
                      DWORD bytes) {
    HKEY key = nullptr;
    ASSERT_EQ(::RegCreateKeyExW(root_, key_path, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                nullptr, &key, nullptr),
              ERROR_SUCCESS);
    ASSERT_EQ(::RegSetValueExW(key, L"ProviderOrder", 0, type,
                               static_cast<const BYTE *>(data), bytes),
              ERROR_SUCCESS);
    ::RegCloseKey(key);
  }

  void SeedOrderString(LPCWSTR key_path, const std::wstring &val,
                       DWORD type = REG_SZ, bool include_nul = true) {
    const DWORD bytes = static_cast<DWORD>(
        (val.size() + (include_nul ? 1u : 0u)) * sizeof(WCHAR));
    SeedOrderValue(key_path, type, val.c_str(), bytes);
  }

  std::pair<DWORD, std::wstring> ReadOrderString(LPCWSTR key_path) {
    HKEY key = nullptr;
    EXPECT_EQ(::RegOpenKeyExW(root_, key_path, 0, KEY_QUERY_VALUE, &key),
              ERROR_SUCCESS);
    DWORD type = 0;
    DWORD bytes = 0;
    EXPECT_EQ(::RegQueryValueExW(key, L"ProviderOrder", nullptr, &type, nullptr,
                                 &bytes),
              ERROR_SUCCESS);
    std::vector<WCHAR> buf(bytes / sizeof(WCHAR) + 2, L'\0');
    EXPECT_EQ(::RegQueryValueExW(key, L"ProviderOrder", nullptr, &type,
                                 reinterpret_cast<BYTE *>(buf.data()), &bytes),
              ERROR_SUCCESS);
    ::RegCloseKey(key);
    return {type, std::wstring(buf.data())};
  }

  HKEY root_ = nullptr;
  std::wstring subkey_path_;
};

TEST(NpRegistryPureTest, ProviderOrderAddTokenCases) {
  struct Case {
    const wchar_t *input;
    const wchar_t *expected; // nullptr means no change needed
  };
  const Case cases[] = {
      {nullptr, L"Dokan3"},
      {L"", L"Dokan3"},
      {L"   ", L"Dokan3"},
      {L"LanmanWorkstation", L"LanmanWorkstation,Dokan3"},
      {L"LanmanWorkstation,", L"LanmanWorkstation,Dokan3"},
      {L"LanmanWorkstation, , ", L"LanmanWorkstation,Dokan3"},
      {L"Dokan3", nullptr},
      {L"dokan3", nullptr},
      {L"LanmanWorkstation,Dokan3,RDPNP", nullptr},
      {L" LanmanWorkstation , dokan3 , RDPNP ", nullptr},
      {L"Dokan2,Dokan30,MyDokan3,Dokan3x",
       L"Dokan2,Dokan30,MyDokan3,Dokan3x,Dokan3"},
  };

  for (const auto &c : cases) {
    LPWSTR out = reinterpret_cast<LPWSTR>(0x1);
    ASSERT_EQ(DokanProviderOrderAdd(c.input, L"Dokan3", &out),
              static_cast<DWORD>(ERROR_SUCCESS));
    if (c.expected == nullptr) {
      EXPECT_EQ(out, nullptr) << "input=" << (c.input ? c.input : L"<null>");
    } else {
      ASSERT_NE(out, nullptr);
      EXPECT_STREQ(out, c.expected);
    }
    DokanProviderOrderFree(out);
  }
  DokanProviderOrderFree(nullptr);
}

TEST(NpRegistryPureTest, ProviderOrderRemoveTokenCases) {
  struct Case {
    const wchar_t *input;
    const wchar_t *expected; // nullptr means no change needed
  };
  const Case cases[] = {
      {nullptr, nullptr},
      {L"", nullptr},
      {L"LanmanWorkstation,RDPNP", nullptr},
      {L"Dokan2,Dokan30,Dokan3x", nullptr},
      {L"Dokan3", L""},
      {L"dokan3", L""},
      {L"Dokan3,LanmanWorkstation,RDPNP", L"LanmanWorkstation,RDPNP"},
      {L"LanmanWorkstation,Dokan3,RDPNP", L"LanmanWorkstation,RDPNP"},
      {L"LanmanWorkstation,RDPNP,Dokan3", L"LanmanWorkstation,RDPNP"},
      {L" Dokan3 , LanmanWorkstation , dokan3 , Dokan30 , RDPNP ",
       L"LanmanWorkstation,Dokan30,RDPNP"},
  };

  for (const auto &c : cases) {
    LPWSTR out = reinterpret_cast<LPWSTR>(0x1);
    ASSERT_EQ(DokanProviderOrderRemove(c.input, L"Dokan3", &out),
              static_cast<DWORD>(ERROR_SUCCESS));
    if (c.expected == nullptr) {
      EXPECT_EQ(out, nullptr) << "input=" << (c.input ? c.input : L"<null>");
    } else {
      ASSERT_NE(out, nullptr);
      EXPECT_STREQ(out, c.expected);
    }
    DokanProviderOrderFree(out);
  }
}

TEST_F(ScopedHkcuSandbox,
       InstallUninstallIdempotentAndPreservesTypesAndForeignTokens) {
  SeedOrderString(kOrderKey, L"LanmanWorkstation,Dokan2,RDPNP,Dokan30,",
                  REG_EXPAND_SZ);
  SeedOrderString(kHwOrderKey, L"RDPNP,LanmanWorkstation", REG_SZ);

  ASSERT_EQ(DokanNetworkProviderInstallAt(root_),
            static_cast<DWORD>(ERROR_SUCCESS));

  // Verify NetworkProvider key values.
  HKEY np_key = nullptr;
  ASSERT_EQ(::RegOpenKeyExW(root_, kServiceNpKey, 0, KEY_QUERY_VALUE, &np_key),
            ERROR_SUCCESS);
  DWORD cls = 0;
  DWORD cls_sz = sizeof(cls);
  DWORD cls_type = 0;
  ASSERT_EQ(::RegQueryValueExW(np_key, L"Class", nullptr, &cls_type,
                               reinterpret_cast<BYTE *>(&cls), &cls_sz),
            ERROR_SUCCESS);
  EXPECT_EQ(cls_type, static_cast<DWORD>(REG_DWORD));
  EXPECT_EQ(cls, static_cast<DWORD>(WN_NETWORK_CLASS));

  DWORD path_type = 0;
  WCHAR path_buf[256]{};
  DWORD path_sz = sizeof(path_buf);
  ASSERT_EQ(::RegQueryValueExW(np_key, L"ProviderPath", nullptr, &path_type,
                               reinterpret_cast<BYTE *>(path_buf), &path_sz),
            ERROR_SUCCESS);
  EXPECT_EQ(path_type, static_cast<DWORD>(REG_EXPAND_SZ));
  EXPECT_STREQ(path_buf, L"%SystemRoot%\\System32\\dokannp3.dll");
  ::RegCloseKey(np_key);

  auto [order_type, order_val] = ReadOrderString(kOrderKey);
  EXPECT_EQ(order_type, static_cast<DWORD>(REG_EXPAND_SZ));
  EXPECT_EQ(order_val, L"LanmanWorkstation,Dokan2,RDPNP,Dokan30,Dokan3");

  auto [hw_type, hw_val] = ReadOrderString(kHwOrderKey);
  EXPECT_EQ(hw_type, static_cast<DWORD>(REG_SZ));
  EXPECT_EQ(hw_val, L"RDPNP,LanmanWorkstation,Dokan3");

  // Second install is idempotent.
  ASSERT_EQ(DokanNetworkProviderInstallAt(root_),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(ReadOrderString(kOrderKey).second,
            L"LanmanWorkstation,Dokan2,RDPNP,Dokan30,Dokan3");

  // Uninstall removes Dokan3 while preserving foreign tokens & REG_EXPAND_SZ.
  ASSERT_EQ(DokanNetworkProviderUninstallAt(root_),
            static_cast<DWORD>(ERROR_SUCCESS));
  auto [after_type, after_val] = ReadOrderString(kOrderKey);
  EXPECT_EQ(after_type, static_cast<DWORD>(REG_EXPAND_SZ));
  EXPECT_EQ(after_val, L"LanmanWorkstation,Dokan2,RDPNP,Dokan30");
  EXPECT_EQ(ReadOrderString(kHwOrderKey).second, L"RDPNP,LanmanWorkstation");

  HKEY missing_np = nullptr;
  EXPECT_EQ(
      ::RegOpenKeyExW(root_, kServiceNpKey, 0, KEY_QUERY_VALUE, &missing_np),
      ERROR_FILE_NOT_FOUND);

  // Uninstall again when service key is completely deleted succeeds.
  ::RegDeleteTreeW(root_, kServiceKey);
  EXPECT_EQ(DokanNetworkProviderUninstallAt(root_),
            static_cast<DWORD>(ERROR_SUCCESS));
}

TEST_F(ScopedHkcuSandbox, HandlesUnterminatedRegSzAndRejectsDword) {
  // Missing HwOrder is fine; unterminated REG_SZ in Order is safe.
  SeedOrderString(kOrderKey, L"LanmanWorkstation", REG_SZ,
                  /*include_nul=*/false);
  ASSERT_EQ(DokanNetworkProviderInstallAt(root_),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(ReadOrderString(kOrderKey).second, L"LanmanWorkstation,Dokan3");

  // Non-string REG_DWORD rejected with ERROR_INVALID_DATATYPE and untouched.
  const DWORD bad_val = 42;
  SeedOrderValue(kOrderKey, REG_DWORD, &bad_val, sizeof(bad_val));
  EXPECT_EQ(DokanNetworkProviderInstallAt(root_),
            static_cast<DWORD>(ERROR_INVALID_DATATYPE));
  EXPECT_EQ(DokanNetworkProviderUninstallAt(root_),
            static_cast<DWORD>(ERROR_INVALID_DATATYPE));
}

} // namespace
} // namespace dokan_np_test
