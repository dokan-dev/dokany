// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_support.h"

#include <algorithm>
#include <set>
#include <vector>

namespace dokan_np_test {
namespace {

std::vector<DWORD> BoundarySizes(DWORD exact, DWORD min_struct = 0) {
  std::set<DWORD> sizes = {0, 1, 2, 3};
  if (min_struct > 0) {
    if (min_struct > 1) {
      sizes.insert(min_struct - 1);
    }
    sizes.insert(min_struct);
    sizes.insert(min_struct + 1);
  }
  if (exact > 1) {
    sizes.insert(exact - 1);
  }
  sizes.insert(exact);
  sizes.insert(exact + 1);
  sizes.insert(exact + 2);
  sizes.insert(exact | 1u);
  return std::vector<DWORD>(sizes.begin(), sizes.end());
}

TEST(NpFuzzTest, GetConnectionBoundaryGuardPages) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\fuzz-srv\\fuzz-share", L'M', 0, L"M:"},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  WCHAR m_drive[] = L"M:";
  DWORD exact_chars = 0;
  ASSERT_EQ(DokanNpGetConnection(&t, m_drive, nullptr, &exact_chars),
            static_cast<DWORD>(WN_MORE_DATA));
  ASSERT_GT(exact_chars, 1u);

  for (DWORD chars :
       {0UL, 1UL, 2UL, exact_chars - 1, exact_chars, exact_chars + 1}) {
    for (const bool use_nonnull_zero :
         (chars == 0 ? std::vector<bool>{false, true}
                     : std::vector<bool>{true})) {
      PageGuardedBuffer buf(static_cast<size_t>(chars) * sizeof(WCHAR),
                            sizeof(WCHAR));
      ASSERT_TRUE(buf.valid());
      DWORD len = chars;
      auto *out = use_nonnull_zero ? static_cast<LPWSTR>(buf.data()) : nullptr;
      const DWORD err = DokanNpGetConnection(&t, m_drive, out, &len);
      EXPECT_TRUE(buf.CheckCanaries()) << "chars=" << chars;
      if (chars < exact_chars) {
        EXPECT_EQ(err, static_cast<DWORD>(WN_MORE_DATA)) << "chars=" << chars;
        EXPECT_EQ(len, exact_chars);
      } else {
        EXPECT_EQ(err, static_cast<DWORD>(WN_SUCCESS)) << "chars=" << chars;
        EXPECT_EQ(len, chars);
        EXPECT_STREQ(out, L"\\\\fuzz-srv\\fuzz-share");
      }

      // HeapAlloc exact-sized allocation for PageHeap.
      HeapGuardedBuffer hbuf(static_cast<size_t>(chars) * sizeof(WCHAR));
      DWORD hlen = chars;
      const DWORD herr = DokanNpGetConnection(
          &t, m_drive,
          use_nonnull_zero ? static_cast<LPWSTR>(hbuf.nonnull_data()) : nullptr,
          &hlen);
      EXPECT_EQ(herr, err);
    }
  }
}

TEST(NpFuzzTest, FormatNetworkNameBoundaryGuardPages) {
  WCHAR input[] = L"\\\\fuzz-srv\\fuzz-share\\";
  DWORD exact_chars = 0;
  ASSERT_EQ(
      DokanNpFormatNetworkName(input, nullptr, &exact_chars, WNFMT_INENUM, 80),
      static_cast<DWORD>(WN_MORE_DATA));
  ASSERT_EQ(exact_chars, 11u); // "fuzz-share" + NUL

  for (DWORD chars :
       {0UL, 1UL, 2UL, exact_chars - 1, exact_chars, exact_chars + 1}) {
    for (const bool use_nonnull : (chars == 0 ? std::vector<bool>{false, true}
                                              : std::vector<bool>{true})) {
      PageGuardedBuffer buf(static_cast<size_t>(chars) * sizeof(WCHAR),
                            sizeof(WCHAR));
      ASSERT_TRUE(buf.valid());
      DWORD len = chars;
      auto *out = use_nonnull ? static_cast<LPWSTR>(buf.data()) : nullptr;
      const DWORD err =
          DokanNpFormatNetworkName(input, out, &len, WNFMT_INENUM, 80);
      EXPECT_TRUE(buf.CheckCanaries()) << "chars=" << chars;
      if (chars < exact_chars) {
        EXPECT_EQ(err, static_cast<DWORD>(WN_MORE_DATA));
        EXPECT_EQ(len, exact_chars);
      } else {
        EXPECT_EQ(err, static_cast<DWORD>(WN_SUCCESS));
        EXPECT_EQ(len, chars);
        EXPECT_STREQ(out, L"fuzz-share");
      }
    }
  }
}

TEST(NpFuzzTest, GetUniversalNameBoundaryGuardPages) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\fuzz-srv\\fuzz-share", L'M', 0, L"M:"},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  for (const DWORD level : {static_cast<DWORD>(UNIVERSAL_NAME_INFO_LEVEL),
                            static_cast<DWORD>(REMOTE_NAME_INFO_LEVEL)}) {
    DWORD exact = 0;
    ASSERT_EQ(DokanNpGetUniversalName(&t, L"M:\\sub\\file.txt", level, nullptr,
                                      &exact),
              static_cast<DWORD>(WN_MORE_DATA));
    const DWORD min_struct =
        level == UNIVERSAL_NAME_INFO_LEVEL
            ? static_cast<DWORD>(sizeof(UNIVERSAL_NAME_INFOW))
            : static_cast<DWORD>(sizeof(REMOTE_NAME_INFOW));

    for (DWORD sz : BoundarySizes(exact, min_struct)) {
      for (const bool use_nonnull : (sz == 0 ? std::vector<bool>{false, true}
                                             : std::vector<bool>{true})) {
        PageGuardedBuffer buf(sz, sizeof(void *));
        ASSERT_TRUE(buf.valid());
        DWORD reported = sz;
        void *out = use_nonnull ? buf.data() : nullptr;
        const DWORD err = DokanNpGetUniversalName(&t, L"M:\\sub\\file.txt",
                                                  level, out, &reported);
        EXPECT_TRUE(buf.CheckCanaries()) << "level=" << level << " sz=" << sz;
        if (sz < exact) {
          EXPECT_EQ(err, static_cast<DWORD>(WN_MORE_DATA))
              << "level=" << level << " sz=" << sz;
          EXPECT_EQ(reported, exact);
        } else {
          EXPECT_EQ(err, static_cast<DWORD>(WN_SUCCESS))
              << "level=" << level << " sz=" << sz;
        }

        HeapGuardedBuffer hbuf(sz);
        DWORD hreported = sz;
        EXPECT_EQ(DokanNpGetUniversalName(
                      &t, L"M:\\sub\\file.txt", level,
                      use_nonnull ? hbuf.nonnull_data() : nullptr, &hreported),
                  err);
      }
    }
  }
}

TEST(NpFuzzTest, ResourceInformationAndParentBoundaryGuardPages) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\fuzz-srv\\fuzz-share", L'M', 0, L"M:"},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  WCHAR sub[] = L"\\\\fuzz-srv\\fuzz-share\\nested\\item";
  NETRESOURCEW nr{};
  nr.lpRemoteName = sub;

  // GetResourceInformation
  DWORD info_exact = 0;
  LPWSTR sys_part = nullptr;
  ASSERT_EQ(
      DokanNpGetResourceInformation(&t, &nr, nullptr, &info_exact, &sys_part),
      static_cast<DWORD>(WN_MORE_DATA));
  for (DWORD sz :
       BoundarySizes(info_exact, static_cast<DWORD>(sizeof(NETRESOURCEW)))) {
    for (const bool use_nonnull :
         (sz == 0 ? std::vector<bool>{false, true} : std::vector<bool>{true})) {
      PageGuardedBuffer buf(sz, alignof(NETRESOURCEW));
      ASSERT_TRUE(buf.valid());
      DWORD reported = sz;
      sys_part = nullptr;
      void *out = use_nonnull ? buf.data() : nullptr;
      const DWORD err =
          DokanNpGetResourceInformation(&t, &nr, out, &reported, &sys_part);
      EXPECT_TRUE(buf.CheckCanaries()) << "info sz=" << sz;
      if (sz < info_exact) {
        EXPECT_EQ(err, static_cast<DWORD>(WN_MORE_DATA)) << "info sz=" << sz;
        EXPECT_EQ(reported, info_exact);
      } else {
        EXPECT_EQ(err, static_cast<DWORD>(WN_SUCCESS)) << "info sz=" << sz;
        AssertNetResourcePointersContained(
            static_cast<const NETRESOURCEW *>(out), 1, out, sz);
        ASSERT_NE(sys_part, nullptr);
        const auto sys_addr = reinterpret_cast<uintptr_t>(sys_part);
        const auto buf_addr = reinterpret_cast<uintptr_t>(out);
        EXPECT_EQ(sys_addr % alignof(WCHAR), 0u);
        EXPECT_GE(sys_addr, buf_addr + sizeof(NETRESOURCEW));
        EXPECT_LT(sys_addr, buf_addr + sz);
      }
    }
  }

  // GetResourceParent (subpath, share, and server->root)
  WCHAR srv[] = L"\\\\fuzz-srv";
  for (LPWSTR target : {sub, srv}) {
    nr.lpRemoteName = target;
    DWORD parent_exact = 0;
    ASSERT_EQ(DokanNpGetResourceParent(&t, &nr, nullptr, &parent_exact),
              static_cast<DWORD>(WN_MORE_DATA));
    for (DWORD sz : BoundarySizes(parent_exact,
                                  static_cast<DWORD>(sizeof(NETRESOURCEW)))) {
      for (const bool use_nonnull : (sz == 0 ? std::vector<bool>{false, true}
                                             : std::vector<bool>{true})) {
        PageGuardedBuffer buf(sz, alignof(NETRESOURCEW));
        ASSERT_TRUE(buf.valid());
        DWORD reported = sz;
        void *out = use_nonnull ? buf.data() : nullptr;
        const DWORD err = DokanNpGetResourceParent(&t, &nr, out, &reported);
        EXPECT_TRUE(buf.CheckCanaries()) << "parent sz=" << sz;
        if (sz < parent_exact) {
          EXPECT_EQ(err, static_cast<DWORD>(WN_MORE_DATA))
              << "parent sz=" << sz;
          EXPECT_EQ(reported, parent_exact);
        } else {
          EXPECT_EQ(err, static_cast<DWORD>(WN_SUCCESS)) << "parent sz=" << sz;
          AssertNetResourcePointersContained(
              static_cast<const NETRESOURCEW *>(out), 1, out, sz);
        }
      }
    }
  }
}

TEST(NpFuzzTest, EnumResourceBoundaryGuardPagesAcrossScopes) {
  FakeTransport fake;
  fake.mounts = {
      {L"\\\\fuzz-srv1\\share1", L'M', 0, L"M:"},
      {L"\\\\fuzz-srv2\\share2", 0, 0, L""},
  };
  const DOKAN_NP_TRANSPORT t = fake.MakeTransport();

  for (const DWORD scope : {static_cast<DWORD>(RESOURCE_CONNECTED),
                            static_cast<DWORD>(RESOURCE_GLOBALNET),
                            static_cast<DWORD>(RESOURCE_CONTEXT)}) {
    HANDLE h = nullptr;
    ASSERT_EQ(DokanNpOpenEnum(&t, scope, RESOURCETYPE_ANY, 0, nullptr, &h),
              static_cast<DWORD>(WN_SUCCESS));

    // Probe exact bytes for 1 entry.
    DWORD count = 1;
    DWORD exact = 0;
    ASSERT_EQ(DokanNpEnumResource(h, &count, nullptr, &exact),
              static_cast<DWORD>(WN_MORE_DATA));
    EXPECT_EQ(DokanNpCloseEnum(h), static_cast<DWORD>(WN_SUCCESS));

    for (DWORD sz :
         BoundarySizes(exact, static_cast<DWORD>(sizeof(NETRESOURCEW)))) {
      for (const bool use_nonnull : (sz == 0 ? std::vector<bool>{false, true}
                                             : std::vector<bool>{true})) {
        ASSERT_EQ(DokanNpOpenEnum(&t, scope, RESOURCETYPE_ANY, 0, nullptr, &h),
                  static_cast<DWORD>(WN_SUCCESS));
        PageGuardedBuffer buf(sz, alignof(NETRESOURCEW));
        ASSERT_TRUE(buf.valid());
        DWORD c = 0xFFFFFFFF;
        DWORD reported = sz;
        void *out = use_nonnull ? buf.data() : nullptr;
        const DWORD err = DokanNpEnumResource(h, &c, out, &reported);
        EXPECT_TRUE(buf.CheckCanaries()) << "scope=" << scope << " sz=" << sz;
        if (sz < exact) {
          EXPECT_EQ(err, static_cast<DWORD>(WN_MORE_DATA))
              << "scope=" << scope << " sz=" << sz;
          EXPECT_GE(reported, exact);
        } else {
          EXPECT_EQ(err, static_cast<DWORD>(WN_SUCCESS))
              << "scope=" << scope << " sz=" << sz;
          EXPECT_GE(c, 1u);
          AssertNetResourcePointersContained(
              static_cast<const NETRESOURCEW *>(out), c, out, sz);
        }
        EXPECT_EQ(DokanNpCloseEnum(h), static_cast<DWORD>(WN_SUCCESS));
      }
    }
  }
}

} // namespace
} // namespace dokan_np_test
