// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_env.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dokan_np_integration {
namespace {

std::wstring ToLower(std::wstring s) {
  for (WCHAR &c : s) {
    c = static_cast<WCHAR>(std::towlower(c));
  }
  return s;
}

bool HasRemote(const std::vector<EnumEntry> &list, const std::wstring &unc) {
  return std::any_of(list.begin(), list.end(), [&](const EnumEntry &e) {
    return _wcsicmp(e.remote_name.c_str(), unc.c_str()) == 0;
  });
}

void VerifyGetConnection(const MirrorMount &m1) {
  WCHAR tiny[2]{};
  DWORD len = 1;
  EXPECT_EQ(::WNetGetConnectionW(m1.drive_name().c_str(), tiny, &len),
            static_cast<DWORD>(ERROR_MORE_DATA));
  EXPECT_GT(len, 1u);

  std::vector<WCHAR> exact(len, L'\0');
  DWORD exact_len = len;
  ASSERT_EQ(
      ::WNetGetConnectionW(m1.drive_name().c_str(), exact.data(), &exact_len),
      static_cast<DWORD>(NO_ERROR));
  EXPECT_EQ(_wcsicmp(exact.data(), m1.unc_name().c_str()), 0);

  const std::wstring lower_drive = ToLower(m1.drive_name());
  WCHAR buf[256]{};
  DWORD buf_len = 256;
  ASSERT_EQ(::WNetGetConnectionW(lower_drive.c_str(), buf, &buf_len),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_EQ(_wcsicmp(buf, m1.unc_name().c_str()), 0);

  WCHAR sys_drive[MAX_PATH]{};
  if (::GetSystemDirectoryW(sys_drive, MAX_PATH) >= 2) {
    const std::wstring sys_drv = std::wstring(1, sys_drive[0]) + L":";
    buf_len = 256;
    EXPECT_EQ(::WNetGetConnectionW(sys_drv.c_str(), buf, &buf_len),
              static_cast<DWORD>(ERROR_NOT_CONNECTED));
  }
}

void VerifyGetUniversalName(const MirrorMount &m1) {
  const std::wstring root_path = m1.drive_root();
  const std::wstring deep_path = m1.drive_root() + L"dir\\file.txt";
  std::vector<uint8_t> buf(2048);

  DWORD sz = static_cast<DWORD>(buf.size());
  ASSERT_EQ(::WNetGetUniversalNameW(root_path.c_str(),
                                    UNIVERSAL_NAME_INFO_LEVEL, buf.data(), &sz),
            static_cast<DWORD>(NO_ERROR));
  const auto *uni_root =
      reinterpret_cast<const UNIVERSAL_NAME_INFOW *>(buf.data());
  EXPECT_EQ(
      _wcsicmp(uni_root->lpUniversalName, (m1.unc_name() + L"\\").c_str()), 0);

  sz = static_cast<DWORD>(buf.size());
  ASSERT_EQ(::WNetGetUniversalNameW(deep_path.c_str(),
                                    UNIVERSAL_NAME_INFO_LEVEL, buf.data(), &sz),
            static_cast<DWORD>(NO_ERROR));
  const auto *uni_deep =
      reinterpret_cast<const UNIVERSAL_NAME_INFOW *>(buf.data());
  EXPECT_EQ(_wcsicmp(uni_deep->lpUniversalName,
                     (m1.unc_name() + L"\\dir\\file.txt").c_str()),
            0);

  sz = static_cast<DWORD>(buf.size());
  ASSERT_EQ(::WNetGetUniversalNameW(deep_path.c_str(), REMOTE_NAME_INFO_LEVEL,
                                    buf.data(), &sz),
            static_cast<DWORD>(NO_ERROR));
  const auto *rem_deep =
      reinterpret_cast<const REMOTE_NAME_INFOW *>(buf.data());
  EXPECT_EQ(_wcsicmp(rem_deep->lpUniversalName,
                     (m1.unc_name() + L"\\dir\\file.txt").c_str()),
            0);
  EXPECT_EQ(_wcsicmp(rem_deep->lpConnectionName, m1.unc_name().c_str()), 0);
  EXPECT_EQ(_wcsicmp(rem_deep->lpRemainingPath, L"\\dir\\file.txt"), 0);
}

void VerifyEnumeration(const std::wstring &srv_a, const std::wstring &srv_b,
                       const MirrorMount &m1, const MirrorMount &m2,
                       const MirrorMount &m3) {
  std::vector<EnumEntry> connected;
  ASSERT_EQ(
      EnumWNet(RESOURCE_CONNECTED, RESOURCETYPE_ANY, 0, nullptr, &connected),
      static_cast<DWORD>(NO_ERROR));
  auto has_conn = [&](const std::wstring &drv, const std::wstring &unc) {
    return std::any_of(
        connected.begin(), connected.end(), [&](const EnumEntry &e) {
          return _wcsicmp(e.local_name.c_str(), drv.c_str()) == 0 &&
                 _wcsicmp(e.remote_name.c_str(), unc.c_str()) == 0;
        });
  };
  EXPECT_TRUE(has_conn(m1.drive_name(), m1.unc_name()));
  EXPECT_TRUE(has_conn(m2.drive_name(), m2.unc_name()));
  EXPECT_TRUE(has_conn(m3.drive_name(), m3.unc_name()));

  // Global root -> find Dokan provider -> enumerate servers (deduped!).
  std::vector<EnumEntry> top;
  ASSERT_EQ(EnumWNet(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, nullptr, &top),
            static_cast<DWORD>(NO_ERROR));
  auto prov_it = std::find_if(top.begin(), top.end(), [](const EnumEntry &e) {
    return _wcsicmp(e.remote_name.c_str(), DOKAN_NP_NAME) == 0 ||
           _wcsicmp(e.provider.c_str(), DOKAN_NP_NAME) == 0;
  });
  ASSERT_NE(prov_it, top.end()) << "Dokan3 provider missing in root enum";

  std::wstring prov_remote_buf = prov_it->remote_name;
  WCHAR prov_name[64]{};
  wcscpy_s(prov_name, DOKAN_NP_NAME);
  NETRESOURCEW nr_prov{};
  nr_prov.dwScope = RESOURCE_GLOBALNET;
  nr_prov.dwType = RESOURCETYPE_ANY;
  nr_prov.dwUsage = RESOURCEUSAGE_CONTAINER;
  nr_prov.lpRemoteName =
      prov_remote_buf.empty() ? nullptr : prov_remote_buf.data();
  nr_prov.lpProvider = prov_name;

  std::vector<EnumEntry> servers;
  ASSERT_EQ(
      EnumWNet(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &nr_prov, &servers),
      static_cast<DWORD>(NO_ERROR));
  int srv_a_count = 0;
  int srv_b_count = 0;
  for (const auto &s : servers) {
    if (_wcsicmp(s.remote_name.c_str(), (L"\\\\" + srv_a).c_str()) == 0) {
      ++srv_a_count;
    } else if (_wcsicmp(s.remote_name.c_str(), (L"\\\\" + srv_b).c_str()) ==
               0) {
      ++srv_b_count;
    }
  }
  EXPECT_EQ(srv_a_count, 1) << "Server A must be deduplicated cleanly";
  EXPECT_EQ(srv_b_count, 1) << "Server B must appear cleanly";

  // Enumerate shares on Server A -> strictly share1 & share2 (no share3).
  std::wstring srv_a_unc = L"\\\\" + srv_a;
  NETRESOURCEW nr_srv_a{};
  nr_srv_a.dwScope = RESOURCE_GLOBALNET;
  nr_srv_a.dwType = RESOURCETYPE_ANY;
  nr_srv_a.dwUsage = RESOURCEUSAGE_CONTAINER;
  nr_srv_a.lpRemoteName = srv_a_unc.data();
  nr_srv_a.lpProvider = prov_name;
  std::vector<EnumEntry> srv_a_shares;
  ASSERT_EQ(EnumWNet(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &nr_srv_a,
                     &srv_a_shares),
            static_cast<DWORD>(NO_ERROR));
  ASSERT_EQ(srv_a_shares.size(), 2u);
  EXPECT_TRUE(HasRemote(srv_a_shares, m1.unc_name()));
  EXPECT_TRUE(HasRemote(srv_a_shares, m2.unc_name()));
  EXPECT_FALSE(HasRemote(srv_a_shares, m3.unc_name()));

  // Negative check #952: unrelated nonexistent server MUST NOT return Dokan
  // shares!
  WCHAR bogus_srv[] = L"\\\\unrelated_nonexistent_server_952\\share";
  NETRESOURCEW nr_bogus{};
  nr_bogus.dwScope = RESOURCE_GLOBALNET;
  nr_bogus.dwType = RESOURCETYPE_ANY;
  nr_bogus.lpRemoteName = bogus_srv;
  std::vector<EnumEntry> bogus_out;
  const DWORD bogus_err =
      EnumWNet(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &nr_bogus, &bogus_out);
  EXPECT_NE(bogus_err, static_cast<DWORD>(NO_ERROR));
  EXPECT_TRUE(bogus_out.empty());

  // RESOURCE_CONTEXT sees Dokan servers.
  std::vector<EnumEntry> ctx_entries;
  const DWORD ctx_err =
      EnumWNet(RESOURCE_CONTEXT, RESOURCETYPE_ANY, 0, nullptr, &ctx_entries);
  EXPECT_EQ(ctx_err, static_cast<DWORD>(NO_ERROR));
  EXPECT_TRUE(HasRemote(ctx_entries, L"\\\\" + srv_a));
  EXPECT_TRUE(HasRemote(ctx_entries, L"\\\\" + srv_b));
}

void VerifyResourceInfoAndParent(const std::wstring &srv_a,
                                 const MirrorMount &m1) {
  std::vector<uint8_t> buf(2048);
  DWORD sz = static_cast<DWORD>(buf.size());
  LPWSTR sys_part = nullptr;

  std::wstring deep_unc = m1.unc_name() + L"\\dir\\file.txt";
  NETRESOURCEW nr{};
  nr.dwType = RESOURCETYPE_DISK;
  nr.lpRemoteName = deep_unc.data();
  ASSERT_EQ(::WNetGetResourceInformationW(&nr, buf.data(), &sz, &sys_part),
            static_cast<DWORD>(NO_ERROR));
  const auto *info_nr = reinterpret_cast<const NETRESOURCEW *>(buf.data());
  EXPECT_EQ(_wcsicmp(info_nr->lpRemoteName, m1.unc_name().c_str()), 0);
  EXPECT_EQ(_wcsicmp(info_nr->lpProvider, DOKAN_NP_NAME), 0);
  ASSERT_NE(sys_part, nullptr);
  EXPECT_EQ(_wcsicmp(sys_part, L"\\dir\\file.txt"), 0);

  // Trailing slash on share.
  std::wstring trail_unc = m1.unc_name() + L"\\";
  nr.lpRemoteName = trail_unc.data();
  sz = static_cast<DWORD>(buf.size());
  sys_part = nullptr;
  ASSERT_EQ(::WNetGetResourceInformationW(&nr, buf.data(), &sz, &sys_part),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_EQ(_wcsicmp(info_nr->lpRemoteName, m1.unc_name().c_str()), 0);
  EXPECT_EQ(sys_part, nullptr);

  // Non-Dokan UNC MUST NOT be claimed (#952).
  WCHAR other_unc[] = L"\\\\unrelated_nonexistent_server_952\\share";
  nr.lpRemoteName = other_unc;
  sz = static_cast<DWORD>(buf.size());
  EXPECT_NE(::WNetGetResourceInformationW(&nr, buf.data(), &sz, &sys_part),
            static_cast<DWORD>(NO_ERROR));

  // Parent walk: subpath -> share -> server -> provider root (NULL).
  // WNetGetResourceParentW requires lpProvider to route to the provider.
  WCHAR prov_name[64]{};
  wcscpy_s(prov_name, DOKAN_NP_NAME);
  nr.lpProvider = prov_name;
  nr.lpRemoteName = deep_unc.data();
  sz = static_cast<DWORD>(buf.size());
  ASSERT_EQ(::WNetGetResourceParentW(&nr, buf.data(), &sz),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_EQ(_wcsicmp(info_nr->lpRemoteName, m1.unc_name().c_str()), 0);

  std::wstring share_unc = m1.unc_name();
  nr.lpRemoteName = share_unc.data();
  sz = static_cast<DWORD>(buf.size());
  ASSERT_EQ(::WNetGetResourceParentW(&nr, buf.data(), &sz),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_EQ(_wcsicmp(info_nr->lpRemoteName, (L"\\\\" + srv_a).c_str()), 0);

  std::wstring srv_unc = L"\\\\" + srv_a;
  nr.lpRemoteName = srv_unc.data();
  sz = static_cast<DWORD>(buf.size());
  ASSERT_EQ(::WNetGetResourceParentW(&nr, buf.data(), &sz),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_EQ(info_nr->lpRemoteName, nullptr);
  EXPECT_EQ(_wcsicmp(info_nr->lpProvider, DOKAN_NP_NAME), 0);

  // WNetFormatNetworkNameW(..., WNFMT_INENUM) returns clean share name.
  WCHAR fmt[128]{};
  DWORD fmt_len = 128;
  ASSERT_EQ(CallWNetFormatNetworkNameW(prov_name, share_unc.data(), fmt,
                                       &fmt_len, WNFMT_INENUM, 80),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_STREQ(fmt, L"share1");
}

void VerifyAddConnection(const MirrorMount &m1) {
  std::wstring remote = m1.unc_name();
  NETRESOURCEW nr{};
  nr.dwType = RESOURCETYPE_DISK;
  nr.lpRemoteName = remote.data();
  // Owned deviceless connection succeeds.
  EXPECT_EQ(::WNetAddConnection3W(nullptr, &nr, nullptr, nullptr, 0),
            static_cast<DWORD>(NO_ERROR));

  // Duplicate local letter fails with ERROR_ALREADY_ASSIGNED.
  std::wstring dup_local = m1.drive_name();
  nr.lpLocalName = dup_local.data();
  EXPECT_EQ(::WNetAddConnection2W(&nr, nullptr, nullptr, 0),
            static_cast<DWORD>(ERROR_ALREADY_ASSIGNED));

  // Unowned UNC rejected.
  WCHAR bad_remote[] = L"\\\\unrelated_nonexistent_server_952\\share";
  nr.lpLocalName = nullptr;
  nr.lpRemoteName = bad_remote;
  EXPECT_NE(::WNetAddConnection2W(&nr, nullptr, nullptr, 0),
            static_cast<DWORD>(NO_ERROR));
}

void VerifyNetUse(const MirrorMount &m1) {
  DWORD code = 1;
  std::wstring net_out;
  ASSERT_TRUE(RunProcessCapture(L"net.exe use", 15000, &code, &net_out));
  EXPECT_EQ(code, 0u);
  const std::wstring lower_out = ToLower(net_out);
  EXPECT_NE(lower_out.find(ToLower(m1.drive_name())), std::wstring::npos)
      << WideToUtf8(net_out);
  EXPECT_NE(lower_out.find(ToLower(m1.unc_name())), std::wstring::npos)
      << WideToUtf8(net_out);
}

void VerifyCancelConnection(const std::wstring &srv_a, MirrorMount &m1,
                            const MirrorMount &m2, MirrorMount &m3) {
  EXPECT_EQ(::WNetCancelConnection2W(m2.drive_name().c_str(), 0, FALSE),
            static_cast<DWORD>(ERROR_ACCESS_DENIED));

  WCHAR sys_drive[MAX_PATH]{};
  if (::GetSystemDirectoryW(sys_drive, MAX_PATH) >= 2) {
    const std::wstring sys_drv = std::wstring(1, sys_drive[0]) + L":";
    EXPECT_EQ(::WNetCancelConnection2W(sys_drv.c_str(), 0, FALSE),
              static_cast<DWORD>(ERROR_NOT_CONNECTED));
  }

  // Cancel by drive letter.
  const std::wstring m1_drv = m1.drive_name();
  const std::wstring m1_unc = m1.unc_name();
  ASSERT_EQ(::WNetCancelConnection2W(m1_drv.c_str(), 0, FALSE),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_TRUE(m1.WaitForExit(10000));

  WCHAR buf[256]{};
  DWORD len = 256;
  EXPECT_EQ(::WNetGetConnectionW(m1_drv.c_str(), buf, &len),
            static_cast<DWORD>(ERROR_NOT_CONNECTED));

  std::wstring srv_a_unc = L"\\\\" + srv_a;
  WCHAR prov_name[64]{};
  wcscpy_s(prov_name, DOKAN_NP_NAME);
  NETRESOURCEW nr_srv_a{};
  nr_srv_a.dwScope = RESOURCE_GLOBALNET;
  nr_srv_a.dwType = RESOURCETYPE_ANY;
  nr_srv_a.dwUsage = RESOURCEUSAGE_CONTAINER;
  nr_srv_a.lpRemoteName = srv_a_unc.data();
  nr_srv_a.lpProvider = prov_name;
  std::vector<EnumEntry> remaining;
  ASSERT_EQ(
      EnumWNet(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &nr_srv_a, &remaining),
      static_cast<DWORD>(NO_ERROR));
  ASSERT_EQ(remaining.size(), 1u);
  EXPECT_EQ(_wcsicmp(remaining[0].remote_name.c_str(), m2.unc_name().c_str()),
            0);
  EXPECT_NE(_wcsicmp(remaining[0].remote_name.c_str(), m1_unc.c_str()), 0);

  // Cancel by UNC name on a cancelable mount (m3).
  const std::wstring m3_drv = m3.drive_name();
  ASSERT_EQ(::WNetCancelConnection2W(m3.unc_name().c_str(), 0, FALSE),
            static_cast<DWORD>(NO_ERROR));
  EXPECT_TRUE(m3.WaitForExit(10000));
  len = 256;
  EXPECT_EQ(::WNetGetConnectionW(m3_drv.c_str(), buf, &len),
            static_cast<DWORD>(ERROR_NOT_CONNECTED));
}

TEST(MprApiTest, EndToEndWNetApisAndUnmountBehavior) {
  const std::wstring srv_a = L"dokantest_a";
  const std::wstring srv_b = L"dokantest_b";

  MirrorMount m1;
  MirrorMount m2;
  MirrorMount m3;
  std::wstring err;
  ASSERT_TRUE(m1.Start(srv_a, L"share1", /*enable_network_unmount=*/true,
                       /*current_session_only=*/false, &err))
      << WideToUtf8(err);
  ASSERT_TRUE(m2.Start(srv_a, L"share2", /*enable_network_unmount=*/false,
                       /*current_session_only=*/false, &err))
      << WideToUtf8(err);
  ASSERT_TRUE(m3.Start(srv_b, L"share3", /*enable_network_unmount=*/true,
                       /*current_session_only=*/false, &err))
      << WideToUtf8(err);

  VerifyGetConnection(m1);
  VerifyGetUniversalName(m1);
  VerifyEnumeration(srv_a, srv_b, m1, m2, m3);
  VerifyResourceInfoAndParent(srv_a, m1);
  VerifyAddConnection(m1);
  VerifyNetUse(m1);
  VerifyCancelConnection(srv_a, m1, m2, m3);
}

} // namespace
} // namespace dokan_np_integration
