// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/path.h"
#include "fstest/core/status.h"

#include "gtest/gtest.h"

#include <string>

namespace fstest::core {
namespace {

TEST(PathTest, NormalizeWin32PathVariants) {
  EXPECT_EQ(NormalizeWin32Path(L"M:"), L"M:\\");
  EXPECT_EQ(NormalizeWin32Path(L"M:\\"), L"M:\\");
  EXPECT_EQ(NormalizeWin32Path(L"M:/"), L"M:\\");
  EXPECT_EQ(NormalizeWin32Path(L"C:\\fstest_ntfs\\"), L"C:\\fstest_ntfs");
  EXPECT_EQ(NormalizeWin32Path(L"C:/a//b/c/"), L"C:\\a\\b\\c");
  EXPECT_EQ(NormalizeWin32Path(L"\\\\myfs\\dokan\\"), L"\\\\myfs\\dokan");
  EXPECT_EQ(NormalizeWin32Path(L"\\\\myfs\\dokan\\sub\\"),
            L"\\\\myfs\\dokan\\sub");
  EXPECT_EQ(NormalizeWin32Path(
                L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}"),
            L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\");
  // Extended verbatim path preserves trailing dot and space.
  EXPECT_EQ(NormalizeWin32Path(L"\\\\?\\C:\\dir\\foo. "),
            L"\\\\?\\C:\\dir\\foo. ");
}

TEST(PathTest, ClassifyPathKindAndLocalVsUnc) {
  EXPECT_EQ(ClassifyPathKind(L"M:"), RootKind::kDriveLetter);
  EXPECT_EQ(ClassifyPathKind(L"M:\\"), RootKind::kDriveLetter);
  EXPECT_EQ(ClassifyPathKind(L"\\\\?\\M:\\"), RootKind::kDriveLetter);
  EXPECT_TRUE(IsLocalKind(RootKind::kDriveLetter));
  EXPECT_FALSE(IsUncKind(RootKind::kDriveLetter));

  EXPECT_EQ(ClassifyPathKind(L"C:\\fstest_ntfs"), RootKind::kSubdirectory);
  EXPECT_EQ(ClassifyPathKind(L"G:\\My Drive\\sandbox"),
            RootKind::kSubdirectory);
  EXPECT_TRUE(IsLocalKind(RootKind::kSubdirectory));

  EXPECT_EQ(ClassifyPathKind(L"\\\\myfs\\dokan"), RootKind::kUncShare);
  EXPECT_EQ(ClassifyPathKind(L"\\\\?\\UNC\\myfs\\dokan"), RootKind::kUncShare);
  EXPECT_TRUE(IsUncKind(RootKind::kUncShare));
  EXPECT_FALSE(IsLocalKind(RootKind::kUncShare));

  EXPECT_EQ(ClassifyPathKind(L"\\\\myfs\\dokan\\sub"),
            RootKind::kUncSubdirectory);
  EXPECT_EQ(ClassifyPathKind(L"\\\\?\\UNC\\myfs\\dokan\\sub\\leaf"),
            RootKind::kUncSubdirectory);
  EXPECT_TRUE(IsUncKind(RootKind::kUncSubdirectory));

  EXPECT_EQ(ClassifyPathKind(
                L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\"),
            RootKind::kVolumeGuid);
  EXPECT_TRUE(IsLocalKind(RootKind::kVolumeGuid));

  const RootKind vol_sub = ClassifyPathKind(
      L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\subdir");
  EXPECT_EQ(vol_sub, RootKind::kSubdirectory);
  EXPECT_TRUE(IsLocalKind(vol_sub));
  EXPECT_FALSE(IsUncKind(vol_sub));
  EXPECT_EQ(ClassifyPathKind(
                L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\a\\b\\"),
            RootKind::kSubdirectory);

  EXPECT_EQ(ClassifyPathKind(L"\\\\?\\Unc\\myfs\\dokan"), RootKind::kUncShare);
  EXPECT_EQ(ClassifyPathKind(L"\\\\?\\Unc\\myfs\\dokan\\sub"),
            RootKind::kUncSubdirectory);
}

TEST(PathTest, ToExtendedWin32PathConversions) {
  EXPECT_EQ(ToExtendedWin32Path(L"M:"), L"\\\\?\\M:\\");
  EXPECT_EQ(ToExtendedWin32Path(L"M:\\"), L"\\\\?\\M:\\");
  EXPECT_EQ(ToExtendedWin32Path(L"C:\\fstest_ntfs"), L"\\\\?\\C:\\fstest_ntfs");
  EXPECT_EQ(ToExtendedWin32Path(L"\\\\myfs\\dokan"),
            L"\\\\?\\UNC\\myfs\\dokan");
  EXPECT_EQ(ToExtendedWin32Path(L"\\\\myfs\\dokan\\sub"),
            L"\\\\?\\UNC\\myfs\\dokan\\sub");
  EXPECT_EQ(ToExtendedWin32Path(
                L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\"),
            L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\");
}

TEST(PathTest, VerbatimToNtPathPreservesEdgeCases) {
  EXPECT_EQ(VerbatimToNtPath(L"M:\\"), L"\\??\\M:\\");
  EXPECT_EQ(VerbatimToNtPath(L"C:\\dir\\foo. "), L"\\??\\C:\\dir\\foo. ");
  EXPECT_EQ(VerbatimToNtPath(L"\\\\?\\C:\\dir\\NUL"), L"\\??\\C:\\dir\\NUL");
  EXPECT_EQ(VerbatimToNtPath(L"\\\\myfs\\dokan"), L"\\??\\UNC\\myfs\\dokan");
  EXPECT_EQ(VerbatimToNtPath(L"\\\\?\\UNC\\myfs\\dokan\\sub"),
            L"\\??\\UNC\\myfs\\dokan\\sub");
  EXPECT_EQ(VerbatimToNtPath(
                L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\"),
            L"\\??\\Volume{12345678-1234-1234-1234-1234567890ab}\\");
  EXPECT_EQ(VerbatimToNtPath(L"\\??\\C:\\already"), L"\\??\\C:\\already");
}

TEST(PathTest, JoinPathPreservesVerbatimSuffix) {
  EXPECT_EQ(JoinPath(L"M:\\", L"foo"), L"M:\\foo");
  EXPECT_EQ(JoinPath(L"C:\\dir", L"foo"), L"C:\\dir\\foo");
  EXPECT_EQ(JoinPath(L"C:\\dir", L"\\foo"), L"C:\\dir\\foo");
  EXPECT_EQ(JoinPath(L"\\\\?\\C:\\dir", L"trailing. "),
            L"\\\\?\\C:\\dir\\trailing. ");
}

TEST(PathTest, DosPathToNtPathSystemRoot) {
  auto res = DosPathToNtPath(L"C:\\Windows");
  ASSERT_TRUE(res.ok()) << FormatNtStatus(res.status);
  EXPECT_EQ(*res.value, L"\\??\\C:\\Windows");
}

TEST(PathTest, NormalizeRootPathAutoAndOverride) {
  auto drive = NormalizeRootPath(L"C:\\");
  ASSERT_TRUE(drive.ok()) << FormatWin32Error(drive.error);
  EXPECT_EQ(drive.value->kind, RootKind::kDriveLetter);
  EXPECT_EQ(drive.value->win32_root, L"C:\\");
  EXPECT_EQ(drive.value->extended_root, L"\\\\?\\C:\\");
  EXPECT_EQ(drive.value->nt_root, L"\\??\\C:\\");
  EXPECT_TRUE(drive.value->is_volume_root);

  wchar_t temp_buf[MAX_PATH + 1]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, temp_buf);
  ASSERT_GT(len, 0u);
  auto sub = NormalizeRootPath(std::wstring_view(temp_buf, len));
  ASSERT_TRUE(sub.ok()) << FormatWin32Error(sub.error);
  EXPECT_EQ(sub.value->kind, RootKind::kSubdirectory);
  EXPECT_FALSE(sub.value->is_volume_root);

  auto forced_root =
      NormalizeRootPath(std::wstring_view(temp_buf, len), /*override=*/true);
  ASSERT_TRUE(forced_root.ok());
  EXPECT_TRUE(forced_root.value->is_volume_root);
}

TEST(PathTest, Utf8WideRoundTrip) {
  const std::wstring original = L"caf\u00E9_\u65E5\u672C\u8A9E_\U0001F4C4.txt";
  const std::string utf8 = WideToUtf8(original);
  EXPECT_FALSE(utf8.empty());
  EXPECT_EQ(Utf8ToWide(utf8), original);
  EXPECT_EQ(WideToUtf8(L""), "");
  EXPECT_EQ(Utf8ToWide(""), L"");
}

TEST(PathTest, NormalizeRootPathCanonicalizesDotAndRelativeRoots) {
  // Absolute non-verbatim roots always go through GetFullPathNameW (no disk
  // access, so a non-existent drive letter is fine). The override avoids
  // volume-root probing of that drive.
  struct Case {
    std::wstring_view raw;
    std::wstring_view win32_root;
    RootKind kind;
  };
  const Case cases[] = {
      {L"Q:\\a\\..\\b", L"Q:\\b", RootKind::kSubdirectory},
      {L"Q:\\dir.", L"Q:\\dir", RootKind::kSubdirectory},
      {L"Q:\\dir\\.", L"Q:\\dir", RootKind::kSubdirectory},
      {L"Q:\\dir\\sub\\..\\", L"Q:\\dir", RootKind::kSubdirectory},
      {L"Q:/x/./y", L"Q:\\x\\y", RootKind::kSubdirectory},
      {L"Q:", L"Q:\\", RootKind::kDriveLetter},
      {L"\\\\srv\\share\\a\\..\\b", L"\\\\srv\\share\\b",
       RootKind::kUncSubdirectory},
  };
  for (const Case &c : cases) {
    auto res = NormalizeRootPath(c.raw, /*is_volume_root_override=*/false);
    ASSERT_TRUE(res.ok()) << WideToUtf8(c.raw) << ": "
                          << FormatWin32Error(res.error);
    EXPECT_EQ(res.value->win32_root, c.win32_root) << WideToUtf8(c.raw);
    EXPECT_EQ(res.value->kind, c.kind) << WideToUtf8(c.raw);
    EXPECT_EQ(res.value->original, c.raw);
  }

  // Relative roots resolve against the current directory.
  wchar_t cwd[MAX_PATH + 1]{};
  const DWORD cwd_len = ::GetCurrentDirectoryW(MAX_PATH + 1, cwd);
  ASSERT_GT(cwd_len, 0u);
  ASSERT_LE(cwd_len, static_cast<DWORD>(MAX_PATH));
  const std::wstring expected_rel = NormalizeWin32Path(
      JoinPath(std::wstring_view(cwd, cwd_len), L"fst_rel_root\\leaf"));
  auto rel = NormalizeRootPath(L"fst_rel_root\\.\\leaf",
                               /*is_volume_root_override=*/false);
  ASSERT_TRUE(rel.ok()) << FormatWin32Error(rel.error);
  EXPECT_EQ(rel.value->win32_root, expected_rel);

  // Verbatim \\?\ roots are never rewritten.
  auto verbatim = NormalizeRootPath(L"\\\\?\\Q:\\a\\..\\b. ",
                                    /*is_volume_root_override=*/false);
  ASSERT_TRUE(verbatim.ok()) << FormatWin32Error(verbatim.error);
  EXPECT_EQ(verbatim.value->win32_root, L"\\\\?\\Q:\\a\\..\\b. ");
}

} // namespace
} // namespace fstest::core
