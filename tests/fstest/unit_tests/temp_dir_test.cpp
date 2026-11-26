// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/file_ops.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"
#include "test_support.h"

#include "gtest/gtest.h"

#include <string>
#include <vector>

namespace fstest::core {
namespace {

using test_support::GetHostTempDir;

bool PathExists(const std::wstring &ext_path) {
  return ::GetFileAttributesW(ext_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring GetWindowsDir() {
  wchar_t buf[MAX_PATH + 1]{};
  const UINT len = ::GetSystemWindowsDirectoryW(buf, MAX_PATH + 1);
  return (len > 0 && len <= MAX_PATH) ? std::wstring(buf, len) : L"";
}

// Rewrites an existing absolute local path as `\\?\Volume{GUID}\<rest>`, or
// returns empty when its volume has no GUID name.
std::wstring ToVolumeGuidAlias(const std::wstring &path) {
  wchar_t mount[MAX_PATH + 1]{};
  if (::GetVolumePathNameW(path.c_str(), mount, MAX_PATH + 1) == FALSE) {
    return L"";
  }
  wchar_t guid[64]{};
  if (::GetVolumeNameForVolumeMountPointW(mount, guid, 64) == FALSE) {
    return L"";
  }
  const std::wstring mount_str(mount);
  const std::wstring norm = NormalizeWin32Path(path);
  if (norm.size() < mount_str.size() ||
      !EqualsIgnoreCase(std::wstring_view(norm).substr(0, mount_str.size()),
                        mount_str)) {
    return L"";
  }
  return std::wstring(guid) + norm.substr(mount_str.size());
}

void ExpectUnsafe(const std::wstring &path) {
  std::wstring canonical = L"sentinel";
  std::string reason;
  EXPECT_FALSE(ResolveSafeCleanupTarget(path, &canonical, &reason))
      << "accepted: " << WideToUtf8(path) << " -> " << WideToUtf8(canonical);
  EXPECT_FALSE(reason.empty()) << WideToUtf8(path);
  EXPECT_TRUE(canonical.empty()) << WideToUtf8(path);
}

// Applies a deny-everyone DACL to a reparse point itself (not its target).
void LockReparseNode(const std::wstring &path) {
  auto deny_sd = SddlToSecurityDescriptor(L"D:P(D;;FA;;;WD)");
  ASSERT_TRUE(deny_sd.ok()) << FormatWin32Error(deny_sd.error);
  auto rh = CreateWin32File(
      ToExtendedWin32Path(path), WRITE_DAC, kShareAll, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
  ASSERT_TRUE(rh.ok()) << FormatWin32Error(rh.error);
  ASSERT_NE(::SetKernelObjectSecurity(rh.value->get(),
                                      DACL_SECURITY_INFORMATION,
                                      deny_sd.value->get()),
            FALSE)
      << FormatWin32Error(::GetLastError());
}

// Creates a directory symlink, or reports why the host cannot.
bool TryCreateDirSymlink(const std::wstring &link, const std::wstring &target,
                         std::string *why) {
  const OsInfo &os = GetOsInfo();
  if (!os.can_create_symlinks) {
    *why = "host lacks SeCreateSymbolicLinkPrivilege / Developer Mode";
    return false;
  }
  auto res = CreateSymlink(link, target, /*is_directory=*/true, os.build);
  if (!res.ok()) {
    *why = "CreateSymlink failed: " + FormatWin32Error(res.error);
    return false;
  }
  return true;
}

TEST(TempDirTest, IsSafeCleanupTargetRejectsUnsafePaths) {
  std::string reason;
  EXPECT_FALSE(IsSafeCleanupTarget(L"", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:.", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:Windows", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\.", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\Windows\\.", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"\\\\?\\C:\\", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"\\\\?\\C:\\.", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"\\\\?\\C:\\Windows\\.", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"\\\\?\\C:\\fstest_ntfs\\..", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"\\\\server\\share", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"\\\\?\\UNC\\server\\share", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"\\\\?\\Unc\\server\\share", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(
      L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\Windows", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\Windows\\System32", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\Windows\\System32\\drivers", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\Program Files", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\Users", &reason));
  EXPECT_FALSE(IsSafeCleanupTarget(L"C:\\fstest_ntfs\\..\\Windows", &reason));

  // Absent targets are safe with an empty canonical path (nothing to delete).
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  std::wstring canonical = L"sentinel";
  EXPECT_TRUE(ResolveSafeCleanupTarget(
      JoinPath(host_tmp, L"fst_safe_sub_absent_987654"), &canonical, &reason))
      << reason;
  EXPECT_TRUE(canonical.empty()) << WideToUtf8(canonical);
  EXPECT_TRUE(IsSafeCleanupTarget(
      JoinPath(host_tmp, L"fst_safe_sub_absent_987654"), &reason))
      << reason;

  // Null optional out-parameters must be handled safely on both accept and
  // reject paths.
  EXPECT_FALSE(ResolveSafeCleanupTarget(L"C:\\", nullptr, nullptr));
  EXPECT_TRUE(ResolveSafeCleanupTarget(
      JoinPath(host_tmp, L"fst_safe_sub_absent_987654"), nullptr, nullptr));
}

TEST(TempDirTest, ResolveSafeCleanupTargetReportsDistinctRejectionReasons) {
  struct ReasonCase {
    const wchar_t *path;
    const char *expected_substr;
  };
  const ReasonCase cases[] = {
      {L"", "empty"},
      {L"C:\\fstest_ntfs\\..\\Windows", "'..'"},
      {L"C:Windows", "drive-relative"},
      {L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\Windows", "GLOBALROOT"},
      {L"\\\\.\\C:\\fstest_ntfs", "device-namespace"},
      {L"\\\\?\\C:\\fstest_ntfs\\.", "Extended cleanup target"},
      {L"\\\\localhost\\C$\\Windows", "UNC administrative share"},
      {L"C:\\", "drive root"},
      {L"\\\\?\\Volume{12345678-1234-1234-1234-1234567890ab}\\",
       "volume GUID root"},
      {L"\\\\server\\share", "UNC share root"},
      {L"D:\\Windows", "protected top-level OS directory"},
      {L"C:\\Windows\\System32", "protected system directory"},
  };
  for (const ReasonCase &c : cases) {
    std::wstring canonical = L"sentinel";
    std::string reason;
    EXPECT_FALSE(ResolveSafeCleanupTarget(c.path, &canonical, &reason))
        << WideToUtf8(c.path);
    EXPECT_TRUE(canonical.empty()) << WideToUtf8(c.path);
    EXPECT_NE(reason.find(c.expected_substr), std::string::npos)
        << "path=" << WideToUtf8(c.path) << " reason=" << reason;
  }
}

TEST(TempDirTest, ResolveSafeCleanupTargetCanonicalizesExistingDirectory) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto dir = ScopedTempDir::Create(host_tmp, L"fstsafe");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);
  ASSERT_TRUE(
      WriteFileContents(dir.value->ChildExtendedPath(L"f.txt"), "x").ok());

  std::string reason;
  std::wstring canonical;
  ASSERT_TRUE(ResolveSafeCleanupTarget(dir.value->Path(), &canonical, &reason))
      << reason;
  ASSERT_FALSE(canonical.empty());
  EXPECT_EQ(canonical.rfind(L"\\\\?\\", 0), 0u) << WideToUtf8(canonical);
  EXPECT_TRUE(EndsWithIgnoreCase(canonical, dir.value->Basename()))
      << WideToUtf8(canonical);

  // Aliases of the same directory (trailing `.`, trailing-dot name, extended
  // form, and volume GUID form) all resolve to the identical canonical path.
  std::vector<std::wstring> aliases = {
      dir.value->Path() + L"\\.",
      dir.value->Path() + L".",
      dir.value->ExtendedPath(),
  };
  const std::wstring guid_alias = ToVolumeGuidAlias(dir.value->Path());
  if (!guid_alias.empty()) {
    aliases.push_back(guid_alias);
  }
  for (const std::wstring &alias : aliases) {
    std::wstring alias_canonical;
    EXPECT_TRUE(ResolveSafeCleanupTarget(alias, &alias_canonical, &reason))
        << WideToUtf8(alias) << ": " << reason;
    EXPECT_TRUE(EqualsIgnoreCase(alias_canonical, canonical))
        << WideToUtf8(alias) << " -> " << WideToUtf8(alias_canonical);
  }

  // A regular file is not a cleanup directory.
  {
    std::wstring file_canonical = L"sentinel";
    std::string file_reason;
    EXPECT_FALSE(ResolveSafeCleanupTarget(dir.value->ChildPath(L"f.txt"),
                                          &file_canonical, &file_reason));
    EXPECT_TRUE(file_canonical.empty());
    EXPECT_NE(file_reason.find("not a directory"), std::string::npos)
        << file_reason;
  }

  // The canonical form is accepted verbatim by ForceRemoveAll.
  std::string err;
  EXPECT_TRUE(ForceRemoveAll(canonical, {}, &err)) << err;
  EXPECT_FALSE(PathExists(dir.value->ExtendedPath()));
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
}

TEST(TempDirTest, ResolveSafeCleanupTargetRejectsShortNameAlias) {
  const std::wstring program_files =
      NormalizeWin32Path(std::wstring_view(L"C:\\Program Files"));
  wchar_t env_buf[MAX_PATH + 1]{};
  const DWORD env_len =
      ::GetEnvironmentVariableW(L"ProgramFiles", env_buf, MAX_PATH + 1);
  const std::wstring long_path = (env_len > 0 && env_len <= MAX_PATH)
                                     ? std::wstring(env_buf, env_len)
                                     : program_files;
  wchar_t short_buf[MAX_PATH + 1]{};
  const DWORD short_len =
      ::GetShortPathNameW(long_path.c_str(), short_buf, MAX_PATH + 1);
  if (short_len == 0 || short_len > MAX_PATH ||
      EqualsIgnoreCase(std::wstring_view(short_buf, short_len), long_path)) {
    GTEST_SKIP() << "No 8.3 short name for " << WideToUtf8(long_path)
                 << " on this volume.";
  }
  const std::wstring short_path(short_buf, short_len);
  ExpectUnsafe(short_path);
  ExpectUnsafe(short_path + L"\\.");
}

TEST(TempDirTest, ResolveSafeCleanupTargetRejectsUncAdminShares) {
  for (const wchar_t *path : {
           L"\\\\localhost\\C$\\Windows\\System32",
           L"\\\\localhost\\c$\\fstest_ntfs",
           L"\\\\127.0.0.1\\D$\\data",
           L"\\\\host\\ADMIN$\\System32",
           L"\\\\host\\IPC$\\pipe",
           L"//localhost/C$/Windows",
           L"\\\\?\\UNC\\localhost\\C$\\Windows\\System32",
           L"\\\\?\\unc\\localhost\\admin$\\x",
       }) {
    ExpectUnsafe(path);
  }
}

TEST(TempDirTest, ResolveSafeCleanupTargetRejectsDeviceNamespacePaths) {
  for (const wchar_t *path : {
           L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\Windows",
           L"\\\\?\\globalroot\\Device\\HarddiskVolume1\\fst",
           L"\\\\?\\HarddiskVolume1\\fst",
           L"\\\\.\\C:\\fstest_ntfs",
           L"//./C:/fstest_ntfs",
           L"//?/C:/fstest_ntfs",
           L"\\??\\C:\\fstest_ntfs",
       }) {
    ExpectUnsafe(path);
  }
}

TEST(TempDirTest, ResolveSafeCleanupTargetRejectsVolumeGuidSystemAlias) {
  const std::wstring windir = GetWindowsDir();
  ASSERT_FALSE(windir.empty());
  const std::wstring guid_windir = ToVolumeGuidAlias(windir);
  if (guid_windir.empty()) {
    GTEST_SKIP() << "System volume has no volume GUID name.";
  }
  ExpectUnsafe(guid_windir);
  ExpectUnsafe(JoinPath(guid_windir, L"System32"));
  ExpectUnsafe(JoinPath(guid_windir, L"System32\\drivers"));
  // Trailing `.` on Win32 forms is resolved before comparison.
  ExpectUnsafe(JoinPath(windir, L"System32\\."));
  ExpectUnsafe(windir + L".");
}

TEST(TempDirTest, ResolveSafeCleanupTargetRejectsJunctionAncestor) {
  const std::wstring windir = GetWindowsDir();
  ASSERT_FALSE(windir.empty());
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto dir = ScopedTempDir::Create(host_tmp, L"fstjanc");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  const std::wstring junc = dir.value->ChildPath(L"to_windows");
  auto jres = CreateDirectoryJunction(junc, windir);
  ASSERT_TRUE(jres.ok()) << FormatWin32Error(jres.error);

  {
    std::wstring junc_canonical = L"sentinel";
    std::string junc_reason;
    EXPECT_FALSE(ResolveSafeCleanupTarget(JoinPath(junc, L"System32"),
                                          &junc_canonical, &junc_reason));
    EXPECT_TRUE(junc_canonical.empty());
    EXPECT_NE(junc_reason.find("protected system directory"), std::string::npos)
        << junc_reason;
  }
  ExpectUnsafe(JoinPath(junc, L"System32\\drivers"));
  // The junction itself is a reparse point, never a cleanup directory.
  {
    std::wstring rp_canonical = L"sentinel";
    std::string rp_reason;
    EXPECT_FALSE(ResolveSafeCleanupTarget(junc, &rp_canonical, &rp_reason));
    EXPECT_TRUE(rp_canonical.empty());
    EXPECT_NE(rp_reason.find("reparse point"), std::string::npos) << rp_reason;
  }

  // Cleanup removes only the junction; %WINDIR% is untouched.
  std::string err;
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
  EXPECT_TRUE(PathExists(JoinPath(windir, L"System32")));
}

TEST(TempDirTest, ResolveSafeCleanupTargetRejectsSymlinkAncestor) {
  const std::wstring windir = GetWindowsDir();
  ASSERT_FALSE(windir.empty());
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto dir = ScopedTempDir::Create(host_tmp, L"fstsanc");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  const std::wstring link = dir.value->ChildPath(L"to_windows");
  std::string why;
  if (!TryCreateDirSymlink(link, windir, &why)) {
    std::string err;
    EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
    GTEST_SKIP() << "Directory symlinks unavailable: " << why;
  }

  ExpectUnsafe(JoinPath(link, L"System32"));
  ExpectUnsafe(link);

  std::string err;
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
  EXPECT_TRUE(PathExists(JoinPath(windir, L"System32")));
}

TEST(TempDirTest, ForceRemoveAllDoesNotFollowSymlinksOutOfTree) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto victim = ScopedTempDir::Create(host_tmp, L"fstsymv");
  ASSERT_TRUE(victim.ok()) << FormatWin32Error(victim.error);
  auto outside = ScopedTempDir::Create(host_tmp, L"fstsymo");
  ASSERT_TRUE(outside.ok()) << FormatWin32Error(outside.error);

  const std::wstring canary = outside.value->ChildExtendedPath(L"canary.txt");
  ASSERT_TRUE(WriteFileContents(canary, "must_survive").ok());

  const OsInfo &os = GetOsInfo();
  std::string why;
  if (!TryCreateDirSymlink(victim.value->ChildPath(L"dir_link"),
                           outside.value->Path(), &why)) {
    std::string err;
    EXPECT_TRUE(victim.value->Cleanup(&err)) << err;
    EXPECT_TRUE(outside.value->Cleanup(&err)) << err;
    GTEST_SKIP() << "Symlinks unavailable: " << why;
  }
  auto file_link = CreateSymlink(victim.value->ChildPath(L"file_link.txt"),
                                 outside.value->ChildPath(L"canary.txt"),
                                 /*is_directory=*/false, os.build);
  ASSERT_TRUE(file_link.ok()) << FormatWin32Error(file_link.error);

  std::string err;
  EXPECT_TRUE(victim.value->Cleanup(&err)) << err;
  auto content = ReadFileContents(canary);
  ASSERT_TRUE(content.ok()) << FormatWin32Error(content.error);
  EXPECT_EQ(*content.value, "must_survive");
  EXPECT_TRUE(outside.value->Cleanup(&err)) << err;
}

TEST(TempDirTest, ForceRemoveAllHandlesVeryDeepTreeWithoutRecursion) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto dir = ScopedTempDir::Create(host_tmp, L"fstdeep");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  // 1500 levels of single-character directories (~3000-char \\?\ path). A
  // recursive walk with a WIN32_FIND_DATAW + path buffer per frame would
  // exceed the default 1 MB thread stack while staying fast on CI VMs.
  constexpr int kDepth = 1500;
  std::wstring cur = dir.value->ExtendedPath();
  for (int i = 0; i < kDepth; ++i) {
    cur.append(L"\\d");
    ASSERT_NE(::CreateDirectoryW(cur.c_str(), nullptr), FALSE)
        << "depth " << i << ": " << FormatWin32Error(::GetLastError());
  }
  ASSERT_TRUE(WriteFileContents(JoinPath(cur, L"leaf.dat"), "deep").ok());

  const std::wstring root_ext = dir.value->ExtendedPath();
  std::string err;
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
  EXPECT_FALSE(PathExists(root_ext));
}

TEST(TempDirTest, ForceRemoveAllHandlesAbsentPath) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  const std::wstring missing =
      JoinPath(host_tmp, L"fst_nonexistent_cleanup_target_987654");
  std::string err;
  EXPECT_TRUE(ForceRemoveAll(missing, {}, &err)) << err;
}

TEST(TempDirTest, ForceRemoveAllClearsReadOnlyHiddenSystem) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto dir = ScopedTempDir::Create(host_tmp, L"fstrhs");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  const std::wstring nested = dir.value->ChildExtendedPath(L"sub");
  ASSERT_NE(::CreateDirectoryW(nested.c_str(), nullptr), FALSE);

  const std::wstring file = JoinPath(nested, L"locked.dat");
  auto wr = WriteFileContents(file, "payload");
  ASSERT_TRUE(wr.ok()) << FormatWin32Error(wr.error);

  constexpr DWORD kRhs =
      FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
  ASSERT_NE(::SetFileAttributesW(file.c_str(), kRhs), FALSE);
  ASSERT_NE(::SetFileAttributesW(nested.c_str(), kRhs), FALSE);

  const std::wstring root_ext = dir.value->ExtendedPath();
  std::string err;
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
  EXPECT_FALSE(PathExists(root_ext));
}

TEST(TempDirTest, ForceRemoveAllResetsRestrictiveDacls) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto dir = ScopedTempDir::Create(host_tmp, L"fstdacl");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  const std::wstring nested = dir.value->ChildExtendedPath(L"locked_dir");
  ASSERT_NE(::CreateDirectoryW(nested.c_str(), nullptr), FALSE);

  const std::wstring file = JoinPath(nested, L"deny.dat");
  auto wr = WriteFileContents(file, "secret");
  ASSERT_TRUE(wr.ok()) << FormatWin32Error(wr.error);

  // Deny all access to Everyone on both the file and its parent directory.
  ASSERT_TRUE(
      SetFileSddl(file, DACL_SECURITY_INFORMATION, L"D:P(D;;FA;;;WD)").ok());
  ASSERT_TRUE(
      SetFileSddl(nested, DACL_SECURITY_INFORMATION, L"D:P(D;;FA;;;WD)").ok());

  const std::wstring root_ext = dir.value->ExtendedPath();
  std::string err;
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
  EXPECT_FALSE(PathExists(root_ext));
}

TEST(TempDirTest, ForceRemoveAllDoesNotFollowJunction) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto victim = ScopedTempDir::Create(host_tmp, L"fstjunc");
  ASSERT_TRUE(victim.ok()) << FormatWin32Error(victim.error);

  auto target = ScopedTempDir::Create(host_tmp, L"fsttgt");
  ASSERT_TRUE(target.ok()) << FormatWin32Error(target.error);

  const std::wstring canary = target.value->ChildExtendedPath(L"canary.txt");
  ASSERT_TRUE(WriteFileContents(canary, "must_survive").ok());

  const std::wstring junction = victim.value->ChildPath(L"junc");
  auto jres = CreateDirectoryJunction(junction, target.value->Path());
  ASSERT_TRUE(jres.ok()) << FormatWin32Error(jres.error);

  // Also test ACL-locked reparse point removal without following target.
  const std::wstring locked_junc = victim.value->ChildPath(L"locked_junc");
  ASSERT_TRUE(CreateDirectoryJunction(locked_junc, target.value->Path()).ok());
  auto deny_sd = SddlToSecurityDescriptor(L"D:P(D;;FA;;;WD)");
  ASSERT_TRUE(deny_sd.ok());
  auto jh = CreateWin32File(
      ToExtendedWin32Path(locked_junc), WRITE_DAC,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
  ASSERT_TRUE(jh.ok()) << FormatWin32Error(jh.error);
  ASSERT_NE(::SetKernelObjectSecurity(jh.value->get(),
                                      DACL_SECURITY_INFORMATION,
                                      deny_sd.value->get()),
            FALSE);
  jh.value->reset();

  const std::wstring victim_ext = victim.value->ExtendedPath();
  std::string err;
  EXPECT_TRUE(victim.value->Cleanup(&err)) << err;
  EXPECT_FALSE(PathExists(victim_ext));

  // Target directory and canary file must remain completely untouched.
  EXPECT_TRUE(PathExists(canary));
  auto content = ReadFileContents(canary);
  ASSERT_TRUE(content.ok()) << FormatWin32Error(content.error);
  EXPECT_EQ(*content.value, "must_survive");

  EXPECT_TRUE(target.value->Cleanup(&err)) << err;
}

TEST(TempDirTest, ForceRemoveAllPreservesTargetDaclOfAccessDeniedReparsePoint) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto user_sid = GetCurrentUserSidString();
  ASSERT_TRUE(user_sid.ok()) << FormatWin32Error(user_sid.error);

  auto victim = ScopedTempDir::Create(host_tmp, L"fstrpd");
  ASSERT_TRUE(victim.ok()) << FormatWin32Error(victim.error);

  auto target = ScopedTempDir::Create(host_tmp, L"fstrpt");
  ASSERT_TRUE(target.ok()) << FormatWin32Error(target.error);

  // Give target a custom protected DACL distinct from ResetDaclToFullControl's
  // output so any accidental follow-and-reset on target is detected.
  const std::wstring target_sddl =
      L"D:P(A;;FA;;;" + *user_sid.value + L")(A;;FR;;;WD)";
  auto set_tgt = SetFileSddl(target.value->ExtendedPath(),
                             DACL_SECURITY_INFORMATION, target_sddl);
  ASSERT_TRUE(set_tgt.ok()) << FormatWin32Error(set_tgt.error);
  auto before_sddl =
      GetFileSddl(target.value->ExtendedPath(), DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(before_sddl.ok()) << FormatWin32Error(before_sddl.error);

  auto deny_sd = SddlToSecurityDescriptor(L"D:P(D;;FA;;;WD)");
  ASSERT_TRUE(deny_sd.ok()) << FormatWin32Error(deny_sd.error);

  auto lock_reparse_node = [&](const std::wstring &path) {
    auto rh = CreateWin32File(
        ToExtendedWin32Path(path), WRITE_DAC, kShareAll, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    ASSERT_TRUE(rh.ok()) << FormatWin32Error(rh.error);
    ASSERT_NE(::SetKernelObjectSecurity(rh.value->get(),
                                        DACL_SECURITY_INFORMATION,
                                        deny_sd.value->get()),
              FALSE)
        << FormatWin32Error(::GetLastError());
  };

  // 1. Direct ForceRemoveAll on an access-denied junction.
  const std::wstring direct_junc = victim.value->ChildPath(L"direct_junc");
  ASSERT_TRUE(CreateDirectoryJunction(direct_junc, target.value->Path()).ok());
  ASSERT_NO_FATAL_FAILURE(lock_reparse_node(direct_junc));

  std::string err;
  EXPECT_TRUE(ForceRemoveAll(direct_junc, {}, &err)) << err;
  EXPECT_FALSE(PathExists(ToExtendedWin32Path(direct_junc)));

  // 2. Recursive cleanup over an access-denied junction (the symlink variant
  // is ForceRemoveAllPreservesTargetDaclOfAccessDeniedSymlink).
  const std::wstring nested_junc = victim.value->ChildPath(L"nested_junc");
  ASSERT_TRUE(CreateDirectoryJunction(nested_junc, target.value->Path()).ok());
  ASSERT_NO_FATAL_FAILURE(lock_reparse_node(nested_junc));

  EXPECT_TRUE(victim.value->Cleanup(&err)) << err;

  auto after_sddl =
      GetFileSddl(target.value->ExtendedPath(), DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(after_sddl.ok()) << FormatWin32Error(after_sddl.error);
  EXPECT_EQ(*after_sddl.value, *before_sddl.value);

  EXPECT_TRUE(target.value->Cleanup(&err)) << err;
}

TEST(TempDirTest, ForceRemoveAllPreservesTargetDaclOfAccessDeniedSymlink) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto user_sid = GetCurrentUserSidString();
  ASSERT_TRUE(user_sid.ok()) << FormatWin32Error(user_sid.error);

  auto victim = ScopedTempDir::Create(host_tmp, L"fstrps");
  ASSERT_TRUE(victim.ok()) << FormatWin32Error(victim.error);
  auto target = ScopedTempDir::Create(host_tmp, L"fstrpst");
  ASSERT_TRUE(target.ok()) << FormatWin32Error(target.error);

  const std::wstring locked_sym = victim.value->ChildPath(L"locked_sym");
  std::string why;
  if (!TryCreateDirSymlink(locked_sym, target.value->Path(), &why)) {
    std::string err;
    EXPECT_TRUE(victim.value->Cleanup(&err)) << err;
    EXPECT_TRUE(target.value->Cleanup(&err)) << err;
    GTEST_SKIP() << "Directory symlinks unavailable: " << why;
  }

  const std::wstring target_sddl =
      L"D:P(A;;FA;;;" + *user_sid.value + L")(A;;FR;;;WD)";
  auto set_tgt = SetFileSddl(target.value->ExtendedPath(),
                             DACL_SECURITY_INFORMATION, target_sddl);
  ASSERT_TRUE(set_tgt.ok()) << FormatWin32Error(set_tgt.error);
  auto before_sddl =
      GetFileSddl(target.value->ExtendedPath(), DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(before_sddl.ok()) << FormatWin32Error(before_sddl.error);

  ASSERT_NO_FATAL_FAILURE(LockReparseNode(locked_sym));

  std::string err;
  EXPECT_TRUE(victim.value->Cleanup(&err)) << err;

  auto after_sddl =
      GetFileSddl(target.value->ExtendedPath(), DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(after_sddl.ok()) << FormatWin32Error(after_sddl.error);
  EXPECT_EQ(*after_sddl.value, *before_sddl.value);
  EXPECT_TRUE(target.value->Cleanup(&err)) << err;
}

TEST(TempDirTest, ForceRemoveAllHandlesExtendedLongAndVerbatimPaths) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto dir = ScopedTempDir::Create(host_tmp, L"fstlong");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  // Build nested path exceeding MAX_PATH (~350 chars).
  std::wstring cur = dir.value->ExtendedPath();
  const std::wstring seg(60, L'a');
  for (int i = 0; i < 6; ++i) {
    cur = JoinPath(cur, seg + std::to_wstring(i));
    ASSERT_NE(::CreateDirectoryW(cur.c_str(), nullptr), FALSE)
        << FormatWin32Error(::GetLastError());
  }
  const std::wstring deep_file = JoinPath(cur, L"deep_payload.dat");
  ASSERT_TRUE(WriteFileContents(deep_file, "deep").ok());

  // Verbatim trailing dot, trailing space, and DOS device name via \\?\.
  ASSERT_TRUE(
      WriteFileContents(dir.value->ChildExtendedPath(L"trailing_dot."), "dot")
          .ok());
  ASSERT_TRUE(WriteFileContents(
                  dir.value->ChildExtendedPath(L"trailing_space "), "space")
                  .ok());
  ASSERT_TRUE(
      WriteFileContents(dir.value->ChildExtendedPath(L"NUL"), "nul_file").ok());

  const std::wstring root_ext = dir.value->ExtendedPath();
  std::string err;
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
  EXPECT_FALSE(PathExists(root_ext));
}

TEST(TempDirTest, ForceRemoveAllHandlesCaseSensitiveCollidingNames) {
  const OsInfo &os = GetOsInfo();
  if (os.build < 17763 || !os.is_elevated) {
    GTEST_SKIP()
        << "Per-directory case sensitivity requires Windows build >= 17763 and "
           "Administrator privileges.";
  }

  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto dir = ScopedTempDir::Create(host_tmp, L"fstcs");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  const std::wstring cs_dir = dir.value->ChildExtendedPath(L"cs");
  ASSERT_NE(::CreateDirectoryW(cs_dir.c_str(), nullptr), FALSE);

  auto dh =
      CreateWin32File(cs_dir, FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(dh.ok()) << FormatWin32Error(dh.error);

  nt::FileCaseSensitiveInfo cs_info{nt::kFileCsFlagCaseSensitiveDir};
  nt::IoStatusBlock iosb{};
  const NTSTATUS st = Ntdll().NtSetInformationFile(
      dh.value->get(), &iosb, &cs_info, sizeof(cs_info),
      nt::FileInfoClass::FileCaseSensitiveInformation);
  dh.value->reset();

  if (!nt::NtSuccess(st)) {
    std::string err;
    EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
    GTEST_SKIP() << "Host %TEMP% volume did not accept "
                    "FileCaseSensitiveInformation: "
                 << FormatNtStatus(st);
  }

  const std::wstring lower = JoinPath(cs_dir, L"foo.txt");
  const std::wstring upper = JoinPath(cs_dir, L"FOO.txt");
  ASSERT_TRUE(WriteFileContents(lower, "lower").ok());
  ASSERT_TRUE(WriteFileContents(upper, "UPPER").ok());

  auto r_lower = ReadFileContents(lower);
  auto r_upper = ReadFileContents(upper);
  ASSERT_TRUE(r_lower.ok());
  ASSERT_TRUE(r_upper.ok());
  EXPECT_EQ(*r_lower.value, "lower");
  EXPECT_EQ(*r_upper.value, "UPPER");

  const std::wstring root_ext = dir.value->ExtendedPath();
  std::string err;
  EXPECT_TRUE(dir.value->Cleanup(&err)) << err;
  EXPECT_FALSE(PathExists(root_ext));
}

} // namespace
} // namespace fstest::core
