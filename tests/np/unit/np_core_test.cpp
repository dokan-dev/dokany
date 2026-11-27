// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_support.h"

#include <set>
#include <string>
#include <vector>

namespace dokan_np_test {
namespace {

// Helper to avoid signed/unsigned mismatch warnings (C4389 / -Wsign-compare)
// when GoogleTest compares DWORD returns against long WN_*/ERROR_* constants.
inline DWORD Dw(long code) { return static_cast<DWORD>(code); }

TEST(NpCoreCapsTest, ReportsExactSupportedCapabilities) {
  EXPECT_EQ(DokanNpGetCaps(WNNC_SPEC_VERSION),
            static_cast<DWORD>(WNNC_SPEC_VERSION51));
  EXPECT_EQ(DokanNpGetCaps(WNNC_NET_TYPE),
            static_cast<DWORD>(WNNC_NET_RDR2SAMPLE));
  EXPECT_EQ(DokanNpGetCaps(WNNC_DRIVER_VERSION), 1u);
  EXPECT_EQ(
      DokanNpGetCaps(WNNC_CONNECTION),
      static_cast<DWORD>(WNNC_CON_GETCONNECTIONS | WNNC_CON_CANCELCONNECTION |
                         WNNC_CON_ADDCONNECTION | WNNC_CON_ADDCONNECTION3));
  EXPECT_EQ(DokanNpGetCaps(WNNC_ENUMERATION),
            static_cast<DWORD>(WNNC_ENUM_GLOBAL | WNNC_ENUM_LOCAL |
                               WNNC_ENUM_CONTEXT));
  EXPECT_EQ(DokanNpGetCaps(WNNC_START),
            static_cast<DWORD>(WNNC_WAIT_FOR_START));
  EXPECT_EQ(DokanNpGetCaps(WNNC_USER), 0u);
  EXPECT_EQ(DokanNpGetCaps(WNNC_DIALOG),
            static_cast<DWORD>(WNNC_DLG_GETRESOURCEPARENT |
                               WNNC_DLG_GETRESOURCEINFORMATION |
                               WNNC_DLG_FORMATNETWORKNAME));
  EXPECT_EQ(DokanNpGetCaps(WNNC_ADMIN), 0u);
  EXPECT_EQ(DokanNpGetCaps(0xFFFFu), 0u);
}

TEST(NpCoreGetConnectionTest, CharCountContractCaseInsensitivityAndFirstWins) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\SrvA\\First", L'M', DOKAN_NP_MOUNT_CANCELABLE, L"M:"},
      {L"\\\\SrvA\\Second", L'M', 0, L"M:"},
      {L"\\\\SrvB\\Driveless", 0, 0, L""},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  // Invalid local names.
  WCHAR out[64]{};
  DWORD len = 64;
  EXPECT_EQ(DokanNpGetConnection(&t, nullptr, out, &len), Dw(WN_NOT_CONNECTED));
  for (std::wstring bad : {L"", L"M", L"M:\\", L"1:", L"AB:"}) {
    len = 64;
    EXPECT_EQ(DokanNpGetConnection(&t, bad.data(), out, &len),
              Dw(WN_NOT_CONNECTED));
  }

  // Unconnected drive.
  WCHAR z_drive[] = L"Z:";
  len = 64;
  EXPECT_EQ(DokanNpGetConnection(&t, z_drive, out, &len), Dw(WN_NOT_CONNECTED));

  // Uppercase and lowercase drive letters match first entry ("\\SrvA\First" =
  // 12 chars + NUL = 13 WCHARs).
  WCHAR m_lower[] = L"m:";
  len = 12; // 1 short of 13 WCHARs.
  EXPECT_EQ(DokanNpGetConnection(&t, m_lower, out, &len), Dw(WN_MORE_DATA));
  EXPECT_EQ(len, 13u);

  // NULL buffer with 0 and >0 lengths returns WN_MORE_DATA with char count.
  len = 0;
  EXPECT_EQ(DokanNpGetConnection(&t, m_lower, nullptr, &len), Dw(WN_MORE_DATA));
  EXPECT_EQ(len, 13u);
  len = 100;
  EXPECT_EQ(DokanNpGetConnection(&t, m_lower, nullptr, &len), Dw(WN_MORE_DATA));
  EXPECT_EQ(len, 13u);

  // Exact size succeeds and leaves *lpnBufferLen untouched.
  len = 13;
  EXPECT_EQ(DokanNpGetConnection(&t, m_lower, out, &len), Dw(WN_SUCCESS));
  EXPECT_STREQ(out, L"\\\\SrvA\\First");
  EXPECT_EQ(len, 13u);

  // Larger buffer succeeds and MUST NOT shrink *lpnBufferLen.
  WCHAR m_upper[] = L"M:";
  len = 64;
  std::wmemset(out, L'X', 64);
  EXPECT_EQ(DokanNpGetConnection(&t, m_upper, out, &len), Dw(WN_SUCCESS));
  EXPECT_STREQ(out, L"\\\\SrvA\\First");
  EXPECT_EQ(len, 64u);
}

TEST(NpCoreGetUniversalNameTest, UniversalAndRemoteNameLevelsAndValidation) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\SrvA\\Share1", L'M', 0, L"M:"},
      {L"\\\\SrvA\\Shadow", L'M', 0, L"M:"},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  std::vector<uint8_t> buf(1024, 0);
  DWORD size = static_cast<DWORD>(buf.size());

  // Invalid local paths.
  for (const wchar_t *bad :
       {L"", L"M", L"M:foo", L"1:\\a", L"\\\\SrvA\\Share1"}) {
    size = static_cast<DWORD>(buf.size());
    EXPECT_EQ(DokanNpGetUniversalName(&t, bad, UNIVERSAL_NAME_INFO_LEVEL,
                                      buf.data(), &size),
              Dw(WN_BAD_LOCALNAME));
  }
  EXPECT_EQ(DokanNpGetUniversalName(&t, nullptr, UNIVERSAL_NAME_INFO_LEVEL,
                                    buf.data(), &size),
            Dw(WN_BAD_LOCALNAME));

  // Unknown info level.
  size = static_cast<DWORD>(buf.size());
  EXPECT_EQ(DokanNpGetUniversalName(&t, L"M:\\a", 99, buf.data(), &size),
            Dw(WN_BAD_LEVEL));

  // UNIVERSAL_NAME_INFO_LEVEL on root ("M:" and "M:\") and deep path.
  size = 0;
  EXPECT_EQ(DokanNpGetUniversalName(&t, L"M:\\dir\\file.txt",
                                    UNIVERSAL_NAME_INFO_LEVEL, nullptr, &size),
            Dw(WN_MORE_DATA));
  const DWORD expected_uni_bytes =
      sizeof(UNIVERSAL_NAME_INFOW) +
      static_cast<DWORD>((wcslen(L"\\\\SrvA\\Share1\\dir\\file.txt") + 1) *
                         sizeof(WCHAR));
  EXPECT_EQ(size, expected_uni_bytes);

  std::vector<uint8_t> exact_uni(size, 0);
  DWORD retry_size = size;
  ASSERT_EQ(DokanNpGetUniversalName(&t, L"M:\\dir\\file.txt",
                                    UNIVERSAL_NAME_INFO_LEVEL, exact_uni.data(),
                                    &retry_size),
            Dw(WN_SUCCESS));
  const auto *uni =
      reinterpret_cast<const UNIVERSAL_NAME_INFOW *>(exact_uni.data());
  EXPECT_STREQ(uni->lpUniversalName, L"\\\\SrvA\\Share1\\dir\\file.txt");

  // Root "M:" and "M:\"
  size = static_cast<DWORD>(buf.size());
  ASSERT_EQ(DokanNpGetUniversalName(&t, L"M:", UNIVERSAL_NAME_INFO_LEVEL,
                                    buf.data(), &size),
            Dw(WN_SUCCESS));
  EXPECT_STREQ(reinterpret_cast<const UNIVERSAL_NAME_INFOW *>(buf.data())
                   ->lpUniversalName,
               L"\\\\SrvA\\Share1");

  size = static_cast<DWORD>(buf.size());
  ASSERT_EQ(DokanNpGetUniversalName(&t, L"M:\\", UNIVERSAL_NAME_INFO_LEVEL,
                                    buf.data(), &size),
            Dw(WN_SUCCESS));
  EXPECT_STREQ(reinterpret_cast<const UNIVERSAL_NAME_INFOW *>(buf.data())
                   ->lpUniversalName,
               L"\\\\SrvA\\Share1\\");

  // REMOTE_NAME_INFO_LEVEL on deep path and root.
  size = 0;
  EXPECT_EQ(DokanNpGetUniversalName(&t, L"m:\\dir\\file.txt",
                                    REMOTE_NAME_INFO_LEVEL, nullptr, &size),
            Dw(WN_MORE_DATA));
  std::vector<uint8_t> exact_rem(size, 0);
  retry_size = size;
  ASSERT_EQ(DokanNpGetUniversalName(&t, L"m:\\dir\\file.txt",
                                    REMOTE_NAME_INFO_LEVEL, exact_rem.data(),
                                    &retry_size),
            Dw(WN_SUCCESS));
  const auto *rem =
      reinterpret_cast<const REMOTE_NAME_INFOW *>(exact_rem.data());
  EXPECT_STREQ(rem->lpUniversalName, L"\\\\SrvA\\Share1\\dir\\file.txt");
  EXPECT_STREQ(rem->lpConnectionName, L"\\\\SrvA\\Share1");
  EXPECT_STREQ(rem->lpRemainingPath, L"\\dir\\file.txt");

  size = static_cast<DWORD>(buf.size());
  ASSERT_EQ(DokanNpGetUniversalName(&t, L"M:", REMOTE_NAME_INFO_LEVEL,
                                    buf.data(), &size),
            Dw(WN_SUCCESS));
  rem = reinterpret_cast<const REMOTE_NAME_INFOW *>(buf.data());
  EXPECT_STREQ(rem->lpUniversalName, L"\\\\SrvA\\Share1");
  EXPECT_STREQ(rem->lpConnectionName, L"\\\\SrvA\\Share1");
  EXPECT_STREQ(rem->lpRemainingPath, L"");

  size = static_cast<DWORD>(buf.size());
  ASSERT_EQ(DokanNpGetUniversalName(&t, L"M:\\", REMOTE_NAME_INFO_LEVEL,
                                    buf.data(), &size),
            Dw(WN_SUCCESS));
  rem = reinterpret_cast<const REMOTE_NAME_INFOW *>(buf.data());
  EXPECT_STREQ(rem->lpUniversalName, L"\\\\SrvA\\Share1\\");
  EXPECT_STREQ(rem->lpConnectionName, L"\\\\SrvA\\Share1");
  EXPECT_STREQ(rem->lpRemainingPath, L"\\");
}

static std::vector<std::pair<std::wstring, std::wstring>>
EnumAll(const DOKAN_NP_TRANSPORT *t, DWORD scope, DWORD type, DWORD usage,
        LPNETRESOURCEW nr, DWORD page_buf_size = 4096,
        DWORD page_count = 0xFFFFFFFF) {
  HANDLE h = nullptr;
  DWORD err = DokanNpOpenEnum(t, scope, type, usage, nr, &h);
  EXPECT_EQ(err, Dw(WN_SUCCESS)) << "OpenEnum failed";
  if (err != WN_SUCCESS) {
    return {};
  }

  std::vector<std::pair<std::wstring, std::wstring>> items;
  std::vector<uint8_t> buf(page_buf_size);
  for (;;) {
    DWORD count = page_count;
    DWORD size = static_cast<DWORD>(buf.size());
    err = DokanNpEnumResource(h, &count, buf.data(), &size);
    if (err == WN_MORE_DATA) {
      buf.resize(size);
      continue;
    }
    if (err == WN_NO_MORE_ENTRIES) {
      break;
    }
    EXPECT_EQ(err, Dw(WN_SUCCESS));
    if (err != WN_SUCCESS) {
      break;
    }
    const auto *res = reinterpret_cast<const NETRESOURCEW *>(buf.data());
    AssertNetResourcePointersContained(res, count, buf.data(),
                                       static_cast<DWORD>(buf.size()));
    for (DWORD i = 0; i < count; ++i) {
      EXPECT_STREQ(res[i].lpProvider, DOKAN_NP_NAME);
      items.emplace_back(res[i].lpLocalName ? res[i].lpLocalName : L"<NULL>",
                         res[i].lpRemoteName ? res[i].lpRemoteName : L"<NULL>");
    }
  }
  EXPECT_EQ(DokanNpCloseEnum(h), Dw(WN_SUCCESS));
  return items;
}

TEST(NpCoreEnumTest, ConnectedGlobalNetContextAndFiltering) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\srv1\\share1", L'M', DOKAN_NP_MOUNT_CANCELABLE, L"M:"},
      {L"\\\\srv1\\share2", L'N', 0, L"N:"},
      {L"\\\\srv1\\share1", L'M', 0, L"M:"}, // dup drive M: -> first wins
      {L"\\\\srv2\\root\\sub1", 0, 0, L""},  // 3-component driveless
      {L"\\\\srv1\\share1", 0, DOKAN_NP_MOUNT_ALIAS, L""}, // alias skipped
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  // RESOURCE_CONNECTED: M:, N:, and driveless non-alias \\srv2\root\sub1.
  const auto connected =
      EnumAll(&t, RESOURCE_CONNECTED, RESOURCETYPE_ANY, 0, nullptr);
  ASSERT_EQ(connected.size(), 3u);
  EXPECT_EQ(connected[0].first, L"M:");
  EXPECT_EQ(connected[0].second, L"\\\\srv1\\share1");
  EXPECT_EQ(connected[1].first, L"N:");
  EXPECT_EQ(connected[1].second, L"\\\\srv1\\share2");
  EXPECT_EQ(connected[2].first, L"<NULL>");
  EXPECT_EQ(connected[2].second, L"\\\\srv2\\root\\sub1");

  // RESOURCE_GLOBALNET root -> deduped servers srv1, srv2.
  const auto root_servers =
      EnumAll(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, nullptr);
  ASSERT_EQ(root_servers.size(), 2u);
  EXPECT_EQ(root_servers[0].second, L"\\\\srv1");
  EXPECT_EQ(root_servers[1].second, L"\\\\srv2");

  // RESOURCE_GLOBALNET provider root ("Dokan3") -> deduped servers.
  WCHAR provider_name[] = DOKAN_NP_NAME;
  NETRESOURCEW nr_prov{};
  nr_prov.lpRemoteName = provider_name;
  const auto prov_servers =
      EnumAll(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &nr_prov);
  EXPECT_EQ(prov_servers, root_servers);

  // RESOURCE_GLOBALNET server -> filtered shares for that server only.
  WCHAR srv1_name[] = L"\\\\SRV1";
  NETRESOURCEW nr_srv1{};
  nr_srv1.lpRemoteName = srv1_name;
  const auto srv1_shares =
      EnumAll(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &nr_srv1);
  ASSERT_EQ(srv1_shares.size(), 2u);
  EXPECT_EQ(srv1_shares[0].second, L"\\\\srv1\\share1");
  EXPECT_EQ(srv1_shares[1].second, L"\\\\srv1\\share2");

  WCHAR srv2_name[] = L"\\\\srv2\\";
  NETRESOURCEW nr_srv2{};
  nr_srv2.lpRemoteName = srv2_name;
  const auto srv2_shares =
      EnumAll(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &nr_srv2);
  ASSERT_EQ(srv2_shares.size(), 1u);
  EXPECT_EQ(srv2_shares[0].second, L"\\\\srv2\\root\\sub1");

  // Intermediate path of 3-component UNC is NOT owned!
  HANDLE bad_h = nullptr;
  WCHAR inter_name[] = L"\\\\srv2\\root";
  NETRESOURCEW nr_inter{};
  nr_inter.lpRemoteName = inter_name;
  EXPECT_EQ(DokanNpOpenEnum(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0,
                            &nr_inter, &bad_h),
            Dw(WN_BAD_NETNAME));

  // Enumerating an owned mount returns WN_NOT_CONTAINER.
  WCHAR share_name[] = L"\\\\srv1\\share1";
  NETRESOURCEW nr_share{};
  nr_share.lpRemoteName = share_name;
  EXPECT_EQ(DokanNpOpenEnum(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0,
                            &nr_share, &bad_h),
            Dw(WN_NOT_CONTAINER));

  // Unknown server rejected (#952).
  WCHAR other_srv[] = L"\\\\other_server";
  NETRESOURCEW nr_other{};
  nr_other.lpRemoteName = other_srv;
  EXPECT_EQ(DokanNpOpenEnum(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0,
                            &nr_other, &bad_h),
            Dw(WN_BAD_NETNAME));

  // Provider mismatch -> WN_BAD_PROVIDER.
  WCHAR wrong_prov[] = L"LanmanWorkstation";
  NETRESOURCEW nr_bad_prov{};
  nr_bad_prov.lpProvider = wrong_prov;
  EXPECT_EQ(DokanNpOpenEnum(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0,
                            &nr_bad_prov, &bad_h),
            Dw(WN_BAD_PROVIDER));

  // RESOURCE_CONTEXT: NULL -> servers; non-NULL -> WN_BAD_VALUE (or
  // WN_BAD_PROVIDER if lpProvider mismatches).
  const auto ctx_servers =
      EnumAll(&t, RESOURCE_CONTEXT, RESOURCETYPE_ANY, 0, nullptr);
  EXPECT_EQ(ctx_servers, root_servers);
  EXPECT_EQ(DokanNpOpenEnum(&t, RESOURCE_CONTEXT, RESOURCETYPE_ANY, 0,
                            &nr_bad_prov, &bad_h),
            Dw(WN_BAD_PROVIDER));
  EXPECT_EQ(DokanNpOpenEnum(&t, RESOURCE_CONTEXT, RESOURCETYPE_ANY, 0, &nr_srv1,
                            &bad_h),
            Dw(WN_BAD_VALUE));

  // Unsupported scopes.
  EXPECT_EQ(DokanNpOpenEnum(&t, RESOURCE_REMEMBERED, RESOURCETYPE_ANY, 0,
                            nullptr, &bad_h),
            Dw(WN_NOT_SUPPORTED));
  EXPECT_EQ(DokanNpOpenEnum(&t, 6 /*RESOURCE_SHAREABLE*/, RESOURCETYPE_ANY, 0,
                            nullptr, &bad_h),
            Dw(WN_NOT_SUPPORTED));

  // Type & usage filters.
  EXPECT_TRUE(
      EnumAll(&t, RESOURCE_GLOBALNET, RESOURCETYPE_PRINT, 0, &nr_srv1).empty());
  EXPECT_TRUE(EnumAll(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY,
                      RESOURCEUSAGE_CONNECTABLE, nullptr)
                  .empty());
  EXPECT_TRUE(EnumAll(&t, RESOURCE_GLOBALNET, RESOURCETYPE_ANY,
                      RESOURCEUSAGE_CONTAINER, &nr_srv1)
                  .empty());
}

TEST(NpCoreEnumTest, SnapshotStabilityPaginationZeroCountAndOddBufferSize) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\srv1\\s1", L'M', 0, L"M:"},
      {L"\\\\srv1\\s2", L'N', 0, L"N:"},
      {L"\\\\srv2\\s3", L'O', 0, L"O:"},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  HANDLE h = nullptr;
  ASSERT_EQ(
      DokanNpOpenEnum(&t, RESOURCE_CONNECTED, RESOURCETYPE_ANY, 0, nullptr, &h),
      Dw(WN_SUCCESS));

  // Mutate underlying mounts after NPOpenEnum!
  fake.mounts.clear();
  fake.mounts.push_back({L"\\\\mutated\\x", L'Z', 0, L"Z:"});

  // *lpcCount == 0 -> WN_SUCCESS, 0 entries, cursor unchanged.
  std::vector<uint8_t> buf(256, 0);
  DWORD count = 0;
  DWORD size = static_cast<DWORD>(buf.size());
  EXPECT_EQ(DokanNpEnumResource(h, &count, buf.data(), &size), Dw(WN_SUCCESS));
  EXPECT_EQ(count, 0u);

  // Paginated resume: 1 entry at a time, starting from tiny odd buffer to force
  // WN_MORE_DATA then exact-size retry with odd size.
  std::vector<std::wstring> seen;
  for (int iter = 0; iter < 10; ++iter) {
    DWORD c = 1;
    DWORD tiny = 17; // odd & too small
    uint8_t small_buf[17]{};
    DWORD err = DokanNpEnumResource(h, &c, small_buf, &tiny);
    if (err == WN_NO_MORE_ENTRIES) {
      break;
    }
    ASSERT_EQ(err, Dw(WN_MORE_DATA));
    ASSERT_GT(static_cast<size_t>(tiny), sizeof(NETRESOURCEW));

    // Add +1 to make the buffer size odd and verify WCHAR alignment & pointer
    // containment!
    DWORD odd_size = tiny | 1u;
    PageGuardedBuffer guarded(odd_size, alignof(NETRESOURCEW));
    ASSERT_TRUE(guarded.valid());
    c = 1;
    DWORD reported = odd_size;
    ASSERT_EQ(DokanNpEnumResource(h, &c, guarded.data(), &reported),
              Dw(WN_SUCCESS));
    ASSERT_EQ(c, 1u);
    EXPECT_TRUE(guarded.CheckCanaries());
    const auto *nr = static_cast<const NETRESOURCEW *>(guarded.data());
    AssertNetResourcePointersContained(nr, 1, guarded.data(), odd_size);
    seen.emplace_back(nr->lpRemoteName);
  }
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0], L"\\\\srv1\\s1");
  EXPECT_EQ(seen[1], L"\\\\srv1\\s2");
  EXPECT_EQ(seen[2], L"\\\\srv2\\s3");

  EXPECT_EQ(DokanNpCloseEnum(h), Dw(WN_SUCCESS));
}

TEST(NpCoreResourceInfoAndParentTest, OwnershipCanonicalCasingAndParentWalk) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\MyServer\\MyShare", L'M', 0, L"M:"},
      {L"\\\\DeepSrv\\Root\\Sub1", 0, 0, L""},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  std::vector<uint8_t> buf(1024, 0);
  DWORD size = static_cast<DWORD>(buf.size());
  LPWSTR sys_part = nullptr;

  // Non-owned rejected BEFORE dwType check!
  WCHAR non_owned[] = L"\\\\Other\\Share";
  NETRESOURCEW nr{};
  nr.lpRemoteName = non_owned;
  nr.dwType = RESOURCETYPE_PRINT;
  EXPECT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_BAD_NETNAME));

  // Owned + wrong dwType -> WN_BAD_DEV_TYPE.
  WCHAR owned_upper[] = L"\\\\MYSERVER\\MYSHARE\\Dir\\File.txt";
  nr.lpRemoteName = owned_upper;
  nr.dwType = RESOURCETYPE_PRINT;
  size = static_cast<DWORD>(buf.size());
  EXPECT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_BAD_DEV_TYPE));

  // Provider mismatch -> WN_BAD_PROVIDER.
  WCHAR bad_prov[] = L"OtherProvider";
  nr.lpProvider = bad_prov;
  nr.dwType = RESOURCETYPE_DISK;
  EXPECT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_BAD_PROVIDER));
  nr.lpProvider = nullptr;

  // Subpath returns canonical table casing + *lplpSystem inside lpBuffer.
  size = static_cast<DWORD>(buf.size());
  sys_part = nullptr;
  ASSERT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_SUCCESS));
  const auto *out_nr = reinterpret_cast<const NETRESOURCEW *>(buf.data());
  EXPECT_STREQ(out_nr->lpRemoteName, L"\\\\MyServer\\MyShare");
  EXPECT_STREQ(out_nr->lpProvider, DOKAN_NP_NAME);
  ASSERT_NE(sys_part, nullptr);
  EXPECT_STREQ(sys_part, L"\\Dir\\File.txt");
  EXPECT_GE(reinterpret_cast<const uint8_t *>(sys_part), buf.data());
  EXPECT_LT(reinterpret_cast<const uint8_t *>(sys_part), buf.data() + size);

  // Trailing slash on exact share -> *lplpSystem == NULL.
  WCHAR exact_trail[] = L"\\\\myserver\\myshare\\";
  nr.lpRemoteName = exact_trail;
  size = static_cast<DWORD>(buf.size());
  sys_part = reinterpret_cast<LPWSTR>(0x1);
  ASSERT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_SUCCESS));
  EXPECT_STREQ(out_nr->lpRemoteName, L"\\\\MyServer\\MyShare");
  EXPECT_EQ(sys_part, nullptr);

  // 3-component UNC boundary check: \\DeepSrv\Root\Sub1_other and
  // \\DeepSrv\Root\other MUST NOT be claimed!
  WCHAR deep_other[] = L"\\\\DeepSrv\\Root\\other";
  nr.lpRemoteName = deep_other;
  size = static_cast<DWORD>(buf.size());
  EXPECT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_BAD_NETNAME));

  WCHAR deep_prefix[] = L"\\\\DeepSrv\\Root\\Sub1_extra";
  nr.lpRemoteName = deep_prefix;
  size = static_cast<DWORD>(buf.size());
  EXPECT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_BAD_NETNAME));

  WCHAR deep_sub[] = L"\\\\deepsrv\\root\\sub1\\a\\b";
  nr.lpRemoteName = deep_sub;
  size = static_cast<DWORD>(buf.size());
  ASSERT_EQ(
      DokanNpGetResourceInformation(&t, &nr, buf.data(), &size, &sys_part),
      Dw(WN_SUCCESS));
  EXPECT_STREQ(out_nr->lpRemoteName, L"\\\\DeepSrv\\Root\\Sub1");
  ASSERT_NE(sys_part, nullptr);
  EXPECT_STREQ(sys_part, L"\\a\\b");

  // GetResourceParent walk: subpath -> mount -> server -> provider root (NULL).
  size = static_cast<DWORD>(buf.size());
  nr.lpRemoteName = deep_sub;
  ASSERT_EQ(DokanNpGetResourceParent(&t, &nr, buf.data(), &size),
            Dw(WN_SUCCESS));
  EXPECT_STREQ(out_nr->lpRemoteName, L"\\\\DeepSrv\\Root\\Sub1");

  WCHAR deep_mount_trail[] = L"\\\\deepsrv\\root\\sub1\\";
  nr.lpRemoteName = deep_mount_trail;
  size = static_cast<DWORD>(buf.size());
  ASSERT_EQ(DokanNpGetResourceParent(&t, &nr, buf.data(), &size),
            Dw(WN_SUCCESS));
  EXPECT_STREQ(out_nr->lpRemoteName, L"\\\\DeepSrv");
  EXPECT_EQ(out_nr->dwDisplayType,
            static_cast<DWORD>(RESOURCEDISPLAYTYPE_SERVER));

  WCHAR srv_trail[] = L"\\\\deepsrv\\";
  nr.lpRemoteName = srv_trail;
  size = static_cast<DWORD>(buf.size());
  ASSERT_EQ(DokanNpGetResourceParent(&t, &nr, buf.data(), &size),
            Dw(WN_SUCCESS));
  EXPECT_EQ(out_nr->lpRemoteName, nullptr);
  EXPECT_STREQ(out_nr->lpProvider, DOKAN_NP_NAME);
  EXPECT_EQ(out_nr->dwDisplayType,
            static_cast<DWORD>(RESOURCEDISPLAYTYPE_NETWORK));

  // Non-owned -> WN_BAD_NETNAME.
  nr.lpRemoteName = non_owned;
  size = static_cast<DWORD>(buf.size());
  EXPECT_EQ(DokanNpGetResourceParent(&t, &nr, buf.data(), &size),
            Dw(WN_BAD_NETNAME));
}

TEST(NpCoreFormatNetworkNameTest, InEnumAndNonEnumCharCountContract) {
  WCHAR out[64]{};
  DWORD len = 64;

  WCHAR share[] = L"\\\\srv1\\myshare";
  EXPECT_EQ(DokanNpFormatNetworkName(share, out, &len, WNFMT_INENUM, 80),
            Dw(WN_SUCCESS));
  EXPECT_STREQ(out, L"myshare");
  EXPECT_EQ(len, 64u); // untouched on success

  WCHAR share_trail[] = L"\\\\srv1\\myshare\\";
  len = 64;
  EXPECT_EQ(DokanNpFormatNetworkName(share_trail, out, &len, WNFMT_INENUM, 80),
            Dw(WN_SUCCESS));
  EXPECT_STREQ(out, L"myshare");

  WCHAR deep[] = L"\\\\srv1\\root\\sub1\\";
  len = 64;
  EXPECT_EQ(DokanNpFormatNetworkName(deep, out, &len, WNFMT_INENUM, 80),
            Dw(WN_SUCCESS));
  EXPECT_STREQ(out, L"sub1");

  // Exact char count probe.
  len = 4; // 1 short of "sub1" + NUL (5)
  EXPECT_EQ(DokanNpFormatNetworkName(deep, out, &len, WNFMT_INENUM, 80),
            Dw(WN_MORE_DATA));
  EXPECT_EQ(len, 5u);

  // Without WNFMT_INENUM: full name preserved (trailing slash stripped).
  len = 64;
  EXPECT_EQ(DokanNpFormatNetworkName(share, out, &len, 0, 80), Dw(WN_SUCCESS));
  EXPECT_STREQ(out, L"\\\\srv1\\myshare");
}

TEST(NpCoreAddAndCancelConnectionTest, AliasDisambiguationAndValidation) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\srv1\\share1", L'M', DOKAN_NP_MOUNT_CANCELABLE, L"M:"},
      {L"\\\\srv1\\share1", L'Y',
       DOKAN_NP_MOUNT_CANCELABLE | DOKAN_NP_MOUNT_ALIAS, L"Y:"},
      {L"\\\\srv1\\locked", L'L', 0, L"L:"},
  };
  DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  // AddConnection: unowned UNC -> WN_BAD_NETNAME (even if M: is connected).
  WCHAR m_drive[] = L"M:";
  WCHAR unowned[] = L"\\\\other\\share";
  NETRESOURCEW nr{};
  nr.lpLocalName = m_drive;
  nr.lpRemoteName = unowned;
  EXPECT_EQ(DokanNpAddConnection3(&t, nullptr, &nr, nullptr, nullptr, 0),
            Dw(WN_BAD_NETNAME));

  // Owned + CONNECT_REDIRECT with empty local name -> WN_NOT_SUPPORTED.
  WCHAR owned[] = L"\\\\srv1\\share1";
  nr.lpLocalName = nullptr;
  nr.lpRemoteName = owned;
  EXPECT_EQ(DokanNpAddConnection3(&t, nullptr, &nr, nullptr, nullptr,
                                  CONNECT_REDIRECT),
            Dw(WN_NOT_SUPPORTED));

  // Owned deviceless without CONNECT_REDIRECT -> WN_SUCCESS.
  EXPECT_EQ(DokanNpAddConnection3(&t, nullptr, &nr, nullptr, nullptr, 0),
            Dw(WN_SUCCESS));

  // Invalid local name -> WN_BAD_LOCALNAME.
  WCHAR bad_local[] = L"1:";
  nr.lpLocalName = bad_local;
  EXPECT_EQ(DokanNpAddConnection3(&t, nullptr, &nr, nullptr, nullptr, 0),
            Dw(WN_BAD_LOCALNAME));

  // Duplicate letter -> WN_ALREADY_CONNECTED.
  nr.lpLocalName = m_drive;
  EXPECT_EQ(DokanNpAddConnection3(&t, nullptr, &nr, nullptr, nullptr, 0),
            Dw(WN_ALREADY_CONNECTED));

  // Free letter z: -> AddDriveAlias invoked on primary (non-alias) target!
  WCHAR z_lower[] = L"z:";
  nr.lpLocalName = z_lower;
  EXPECT_EQ(DokanNpAddConnection(&t, &nr, nullptr, nullptr), Dw(WN_SUCCESS));
  ASSERT_EQ(fake.aliases.size(), 1u);
  EXPECT_EQ(fake.aliases[0].alias_drive_letter, L'Z');
  EXPECT_EQ(fake.aliases[0].target_flags & DOKAN_NP_MOUNT_ALIAS, 0u);
  EXPECT_EQ(fake.aliases[0].target_drive_letter, L'M');

  // When AddDriveAlias == NULL -> WN_NOT_SUPPORTED.
  t.AddDriveAlias = nullptr;
  WCHAR x_drive[] = L"X:";
  nr.lpLocalName = x_drive;
  EXPECT_EQ(DokanNpAddConnection3(&t, nullptr, &nr, nullptr, nullptr, 0),
            Dw(WN_NOT_SUPPORTED));

  // CancelConnection("Y:") targets alias; CancelConnection("\\srv1\share1")
  // targets primary!
  WCHAR y_drive[] = L"y:";
  EXPECT_EQ(DokanNpCancelConnection(&t, y_drive, FALSE), Dw(WN_SUCCESS));
  ASSERT_EQ(fake.cancels.size(), 1u);
  EXPECT_EQ(fake.cancels[0].drive_letter, L'Y');
  EXPECT_NE(fake.cancels[0].flags & DOKAN_NP_MOUNT_ALIAS, 0u);

  EXPECT_EQ(DokanNpCancelConnection(&t, owned, TRUE), Dw(WN_SUCCESS));
  ASSERT_EQ(fake.cancels.size(), 2u);
  EXPECT_EQ(fake.cancels[1].drive_letter, L'M');
  EXPECT_EQ(fake.cancels[1].flags & DOKAN_NP_MOUNT_ALIAS, 0u);
  EXPECT_TRUE(fake.cancels[1].force);

  // Non-cancelable mount -> WN_ACCESS_DENIED without calling CancelMount.
  WCHAR locked_drive[] = L"L:";
  const size_t cancels_before = fake.cancels.size();
  EXPECT_EQ(DokanNpCancelConnection(&t, locked_drive, FALSE),
            Dw(WN_ACCESS_DENIED));
  EXPECT_EQ(fake.cancels.size(), cancels_before);
}

} // namespace
} // namespace dokan_np_test
