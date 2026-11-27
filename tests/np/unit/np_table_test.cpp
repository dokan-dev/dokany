// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_support.h"

#include <cstring>
#include <string>
#include <vector>

namespace dokan_np_test {
namespace {

TEST(NpTableBuilderTest, EmptyFinishAndNullFreeAreSafe) {
  DOKAN_NP_TABLE_BUILDER *builder = nullptr;
  ASSERT_EQ(DokanNpTableBuilderCreate(&builder),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_NE(builder, nullptr);

  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableBuilderFinish(builder, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->Count, 0u);

  DokanNpTableFree(table);
  DokanNpTableFree(nullptr);
}

TEST(NpTableBuilderTest, AbortAfterAddingCleansUpCleanly) {
  DOKAN_NP_TABLE_BUILDER *builder = nullptr;
  ASSERT_EQ(DokanNpTableBuilderCreate(&builder),
            static_cast<DWORD>(ERROR_SUCCESS));
  const std::wstring unc = L"\\srv\\share";
  const std::wstring key = L"M:";
  ASSERT_EQ(DokanNpTableBuilderAdd(builder, unc.data(), unc.size(), L'M',
                                   DOKAN_NP_MOUNT_CANCELABLE, key.data(),
                                   key.size()),
            static_cast<DWORD>(ERROR_SUCCESS));
  DokanNpTableBuilderAbort(builder);
  DokanNpTableBuilderAbort(nullptr);
}

TEST(NpTableBuilderTest, NormalizesLeadingAndTrailingBackslashesAndCase) {
  DOKAN_NP_TABLE_BUILDER *builder = nullptr;
  ASSERT_EQ(DokanNpTableBuilderCreate(&builder),
            static_cast<DWORD>(ERROR_SUCCESS));

  // 1 leading backslash normalized to 2; lowercase drive letter uppercased.
  const std::wstring u1 = L"\\Srv1\\ShareA";
  const std::wstring k1 = L"m:";
  EXPECT_EQ(DokanNpTableBuilderAdd(builder, u1.data(), u1.size(), L'm',
                                   DOKAN_NP_MOUNT_CANCELABLE, k1.data(),
                                   k1.size()),
            static_cast<DWORD>(ERROR_SUCCESS));

  // 2 leading backslashes + trailing backslash stripped; driveless (0).
  const std::wstring u2 = L"\\\\Srv2\\ShareB\\";
  EXPECT_EQ(DokanNpTableBuilderAdd(builder, u2.data(), u2.size(), 0, 0, L"", 0),
            static_cast<DWORD>(ERROR_SUCCESS));

  // 3-component UNC with trailing backslash + ALIAS flag.
  const std::wstring u3 = L"\\Srv3\\Root\\Sub1\\";
  const std::wstring k3 = L"Z:";
  EXPECT_EQ(
      DokanNpTableBuilderAdd(builder, u3.data(), u3.size(), L'z',
                             DOKAN_NP_MOUNT_CANCELABLE | DOKAN_NP_MOUNT_ALIAS,
                             k3.data(), k3.size()),
      static_cast<DWORD>(ERROR_SUCCESS));

  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableBuilderFinish(builder, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->Count, 3u);

  EXPECT_STREQ(table->Entries[0].RemoteName, L"\\\\Srv1\\ShareA");
  EXPECT_EQ(table->Entries[0].RemoteNameLength, 13u);
  EXPECT_EQ(table->Entries[0].DriveLetter, L'M');
  EXPECT_EQ(table->Entries[0].Flags, DOKAN_NP_MOUNT_CANCELABLE);
  EXPECT_STREQ(table->Entries[0].TransportKey, L"m:");

  EXPECT_STREQ(table->Entries[1].RemoteName, L"\\\\Srv2\\ShareB");
  EXPECT_EQ(table->Entries[1].RemoteNameLength, 13u);
  EXPECT_EQ(table->Entries[1].DriveLetter, 0);
  EXPECT_EQ(table->Entries[1].Flags, 0u);
  EXPECT_STREQ(table->Entries[1].TransportKey, L"");

  EXPECT_STREQ(table->Entries[2].RemoteName, L"\\\\Srv3\\Root\\Sub1");
  EXPECT_EQ(table->Entries[2].RemoteNameLength, 16u);
  EXPECT_EQ(table->Entries[2].DriveLetter, L'Z');
  EXPECT_EQ(table->Entries[2].Flags,
            DOKAN_NP_MOUNT_CANCELABLE | DOKAN_NP_MOUNT_ALIAS);
  EXPECT_STREQ(table->Entries[2].TransportKey, L"Z:");

  DokanNpTableFree(table);
}

TEST(NpTableBuilderTest, SupportsLongFqdnAndDeepUncOver64Chars) {
  const std::wstring long_unc =
      L"\\\\fileserver-01.production.region-west.example.internal"
      L"\\team-shared-workspace\\project-alpha\\subvolume";
  ASSERT_GT(long_unc.size(), 64u);

  DOKAN_NP_TABLE_BUILDER *builder = nullptr;
  ASSERT_EQ(DokanNpTableBuilderCreate(&builder),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_EQ(DokanNpTableBuilderAdd(builder, long_unc.data(), long_unc.size(),
                                   L'N', 0, L"N:", 2),
            static_cast<DWORD>(ERROR_SUCCESS));

  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableBuilderFinish(builder, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_EQ(table->Count, 1u);
  EXPECT_EQ(table->Entries[0].RemoteNameLength,
            static_cast<ULONG>(long_unc.size()));
  EXPECT_STREQ(table->Entries[0].RemoteName, long_unc.c_str());

  DokanNpTableFree(table);
}

TEST(NpTableBuilderTest, ReadsUnterminatedStringsWithoutOobAgainstGuardPage) {
  GuardedUnterminatedWide unc(L"\\guarded-srv\\guarded-share");
  GuardedUnterminatedWide key(L"G:");
  ASSERT_TRUE(unc.valid());
  ASSERT_TRUE(key.valid());

  DOKAN_NP_TABLE_BUILDER *builder = nullptr;
  ASSERT_EQ(DokanNpTableBuilderCreate(&builder),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_EQ(DokanNpTableBuilderAdd(builder, unc.data(), unc.length(), L'G',
                                   DOKAN_NP_MOUNT_CANCELABLE, key.data(),
                                   key.length()),
            static_cast<DWORD>(ERROR_SUCCESS));

  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableBuilderFinish(builder, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_EQ(table->Count, 1u);
  EXPECT_STREQ(table->Entries[0].RemoteName, L"\\\\guarded-srv\\guarded-share");
  EXPECT_STREQ(table->Entries[0].TransportKey, L"G:");
  DokanNpTableFree(table);
}

TEST(NpTableBuilderTest, RejectsMalformedUncEmbeddedNulBadDriveAndBadFlags) {
  DOKAN_NP_TABLE_BUILDER *builder = nullptr;
  ASSERT_EQ(DokanNpTableBuilderCreate(&builder),
            static_cast<DWORD>(ERROR_SUCCESS));

  const std::wstring bad_uncs[] = {
      L"",
      L"\\",
      L"\\\\",
      L"srv\\share",
      L"\\\\\\srv\\share",
      L"\\\\\\\\srv\\share",
      L"\\\\srv",
      L"\\\\srv\\",
      L"\\\\srv\\\\share",
      L"\\\\srv\\share\\\\sub",
      L"\\\\\\a\\b",
  };
  for (const auto &u : bad_uncs) {
    EXPECT_EQ(
        DokanNpTableBuilderAdd(builder, u.data(), u.size(), L'M', 0, L"M:", 2),
        static_cast<DWORD>(ERROR_INVALID_NAME))
        << "u=" << ::testing::PrintToString(u);
  }

  // Embedded NUL in UncName.
  const wchar_t nul_unc[] = L"\\srv\0share";
  EXPECT_EQ(DokanNpTableBuilderAdd(builder, nul_unc, 10, L'M', 0, L"M:", 2),
            static_cast<DWORD>(ERROR_INVALID_NAME));

  // Embedded NUL in TransportKey.
  const wchar_t nul_key[] = L"M\0:";
  EXPECT_NE(
      DokanNpTableBuilderAdd(builder, L"\\srv\\share", 10, L'M', 0, nul_key, 3),
      static_cast<DWORD>(ERROR_SUCCESS));

  // Invalid drive letters.
  for (const WCHAR bad_drive :
       {L'0', L'9', L'[', L'@', L'\\', static_cast<WCHAR>(0x00C4)}) {
    EXPECT_NE(DokanNpTableBuilderAdd(builder, L"\\srv\\share", 10, bad_drive, 0,
                                     L"M:", 2),
              static_cast<DWORD>(ERROR_SUCCESS));
  }

  // Unknown Flags bits.
  EXPECT_NE(DokanNpTableBuilderAdd(builder, L"\\srv\\share", 10, L'M', 0x4UL,
                                   L"M:", 2),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_NE(DokanNpTableBuilderAdd(builder, L"\\srv\\share", 10, L'M',
                                   0x80000000UL, L"M:", 2),
            static_cast<DWORD>(ERROR_SUCCESS));

  // Length cap > 32767.
  std::wstring too_long(32770, L'a');
  too_long[0] = L'\\';
  too_long[1] = L'\\';
  too_long[10] = L'\\';
  EXPECT_NE(DokanNpTableBuilderAdd(builder, too_long.data(), too_long.size(), 0,
                                   0, L"", 0),
            static_cast<DWORD>(ERROR_SUCCESS));

  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableBuilderFinish(builder, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(table->Count, 0u);
  DokanNpTableFree(table);
}

TEST(NpTableBuilderTest, EnforcesMaxEntryCountCap) {
  DOKAN_NP_TABLE_BUILDER *builder = nullptr;
  ASSERT_EQ(DokanNpTableBuilderCreate(&builder),
            static_cast<DWORD>(ERROR_SUCCESS));

  const std::wstring u = L"\\s\\sh";
  DWORD last_err = ERROR_SUCCESS;
  for (int i = 0; i < 4096; ++i) {
    last_err =
        DokanNpTableBuilderAdd(builder, u.data(), u.size(), 0, 0, L"", 0);
    ASSERT_EQ(last_err, static_cast<DWORD>(ERROR_SUCCESS)) << "i=" << i;
  }
  // 4097th entry must be rejected cleanly without overflowing.
  last_err = DokanNpTableBuilderAdd(builder, u.data(), u.size(), 0, 0, L"", 0);
  EXPECT_NE(last_err, static_cast<DWORD>(ERROR_SUCCESS));

  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableBuilderFinish(builder, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  EXPECT_EQ(table->Count, 4096u);
  DokanNpTableFree(table);
}

TEST(NpTransportLiveMappingTest, EmptyMountPointInfoListYieldsEmptyTable) {
  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableFromMountPointInfo(nullptr, 0, 1, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->Count, 0u);
  DokanNpTableFree(table);
}

TEST(NpTransportLiveMappingTest,
     FiltersSessionsNormalizesMountsAndHandles64CharUnc) {
  // Place a 1-element array right before a PAGE_NOACCESS guard page with a
  // full 64-char unterminated UNCName to verify zero OOB reads past index 63.
  PageGuardedBuffer guard_arr(sizeof(DOKAN_MOUNT_POINT_INFO),
                              alignof(DOKAN_MOUNT_POINT_INFO), 0);
  ASSERT_TRUE(guard_arr.valid());
  auto *guarded_info = static_cast<DOKAN_MOUNT_POINT_INFO *>(guard_arr.data());
  guarded_info->Type = DOKAN_NETWORK_FILE_SYSTEM;
  wcscpy_s(guarded_info->MountPoint, L"\\DosDevices\\N:");
  // Fill all 64 WCHARs of UNCName AND all 64 WCHARs of DeviceName with NO NUL
  // terminator so an unbounded wcslen(UNCName) visibly overruns into
  // DeviceName (128 chars instead of 64). Also test 260-char unterminated
  // MountPoint below.
  const wchar_t prefix[] = L"\\server64\\";
  for (size_t i = 0; i < 64; ++i) {
    guarded_info->UNCName[i] =
        i < 10 ? prefix[i] : static_cast<wchar_t>(L'a' + (i % 26));
    guarded_info->DeviceName[i] = L'D';
  }
  guarded_info->SessionId = 42;
  guarded_info->MountOptions = DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT;

  DOKAN_NP_MOUNT_TABLE *guarded_table = nullptr;
  ASSERT_EQ(DokanNpTableFromMountPointInfo(guarded_info, 1, 42, &guarded_table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_EQ(guarded_table->Count, 1u);
  // 1 leading '\' expanded to 2 leading '\\' -> 65 WCHARs total.
  EXPECT_EQ(guarded_table->Entries[0].RemoteNameLength, 65u);
  EXPECT_EQ(guarded_table->Entries[0].DriveLetter, L'N');
  EXPECT_EQ(guarded_table->Entries[0].Flags, DOKAN_NP_MOUNT_CANCELABLE);
  EXPECT_STREQ(guarded_table->Entries[0].TransportKey, L"N:");
  DokanNpTableFree(guarded_table);
  EXPECT_TRUE(guard_arr.CheckCanaries());

  // Also test 260-char unterminated MountPoint abutting non-NUL UNCName.
  const wchar_t mp_pfx[] = L"\\DosDevices\\C:\\mnt\\";
  for (size_t i = 0; i < MAX_PATH; ++i) {
    guarded_info->MountPoint[i] =
        i < 19 ? mp_pfx[i] : static_cast<wchar_t>(L'm');
  }
  guarded_table = nullptr;
  ASSERT_EQ(DokanNpTableFromMountPointInfo(guarded_info, 1, 42, &guarded_table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_EQ(guarded_table->Count, 1u);
  EXPECT_EQ(guarded_table->Entries[0].DriveLetter, 0);
  EXPECT_EQ(wcslen(guarded_table->Entries[0].TransportKey),
            static_cast<size_t>(MAX_PATH - 12));
  DokanNpTableFree(guarded_table);
  EXPECT_TRUE(guard_arr.CheckCanaries());

  // Multi-entry synthetic array covering all live mapping rules.
  std::vector<DOKAN_MOUNT_POINT_INFO> infos(6);
  std::memset(infos.data(), 0, infos.size() * sizeof(DOKAN_MOUNT_POINT_INFO));

  // 0: 63-char NUL-terminated UNC, global session (-1), lowercase \dosdevices\z:.
  infos[0].Type = DOKAN_NETWORK_FILE_SYSTEM;
  wcscpy_s(infos[0].MountPoint, L"\\dosdevices\\z:");
  for (size_t i = 0; i < 63; ++i) {
    infos[0].UNCName[i] = i < 6 ? L"\\srv0\\"[i] : L'x';
  }
  infos[0].UNCName[63] = L'\0';
  infos[0].SessionId = static_cast<ULONG>(-1);
  infos[0].MountOptions = 0;

  // 1: Folder mount \DosDevices\C:\mnt\x -> DriveLetter MUST be 0 (not 'C'!).
  infos[1].Type = DOKAN_NETWORK_FILE_SYSTEM;
  wcscpy_s(infos[1].MountPoint, L"\\DosDevices\\C:\\mnt\\x");
  wcscpy_s(infos[1].UNCName, L"\\srv1\\folder_mount");
  infos[1].SessionId = 7;
  infos[1].MountOptions = DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT;

  // 2: Mismatched session (SessionId = 99 vs CurrentSessionId = 7) -> excluded.
  infos[2].Type = DOKAN_NETWORK_FILE_SYSTEM;
  wcscpy_s(infos[2].MountPoint, L"\\DosDevices\\Y:");
  wcscpy_s(infos[2].UNCName, L"\\srv2\\other_session");
  infos[2].SessionId = 99;
  infos[2].MountOptions = DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT;

  // 3: Disk-mode mount (empty UNCName) -> excluded.
  infos[3].Type = DOKAN_DISK_FILE_SYSTEM;
  wcscpy_s(infos[3].MountPoint, L"\\DosDevices\\D:");
  infos[3].UNCName[0] = L'\0';
  infos[3].SessionId = static_cast<ULONG>(-1);

  // 4: Driveless / empty MountPoint with valid UNC -> kept with DriveLetter=0.
  infos[4].Type = DOKAN_NETWORK_FILE_SYSTEM;
  infos[4].MountPoint[0] = L'\0';
  wcscpy_s(infos[4].UNCName, L"\\srv3\\driveless");
  infos[4].SessionId = 7;
  infos[4].MountOptions = 0;

  // 5: Standard matching session drive mount with unmount enabled.
  infos[5].Type = DOKAN_NETWORK_FILE_SYSTEM;
  wcscpy_s(infos[5].MountPoint, L"\\DosDevices\\M:");
  wcscpy_s(infos[5].UNCName, L"\\srv1\\share1");
  infos[5].SessionId = 7;
  infos[5].MountOptions = DOKAN_EVENT_ENABLE_NETWORK_UNMOUNT;

  DOKAN_NP_MOUNT_TABLE *table = nullptr;
  ASSERT_EQ(DokanNpTableFromMountPointInfo(
                infos.data(), static_cast<ULONG>(infos.size()), 7, &table),
            static_cast<DWORD>(ERROR_SUCCESS));
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->Count, 4u);

  // Entry 0: 63-char UNC -> 64 WCHARs after '\\' normalization, drive Z.
  EXPECT_EQ(table->Entries[0].RemoteNameLength, 64u);
  EXPECT_EQ(table->Entries[0].DriveLetter, L'Z');
  EXPECT_EQ(table->Entries[0].Flags, 0u);

  // Entry 1: folder mount -> DriveLetter == 0, TransportKey == C:\mnt\x.
  EXPECT_STREQ(table->Entries[1].RemoteName, L"\\\\srv1\\folder_mount");
  EXPECT_EQ(table->Entries[1].DriveLetter, 0);
  EXPECT_EQ(table->Entries[1].Flags, DOKAN_NP_MOUNT_CANCELABLE);
  EXPECT_STREQ(table->Entries[1].TransportKey, L"C:\\mnt\\x");

  // Entry 2 (from infos[4]): driveless.
  EXPECT_STREQ(table->Entries[2].RemoteName, L"\\\\srv3\\driveless");
  EXPECT_EQ(table->Entries[2].DriveLetter, 0);
  EXPECT_EQ(table->Entries[2].Flags, 0u);

  // Entry 3 (from infos[5]): M: cancelable.
  EXPECT_STREQ(table->Entries[3].RemoteName, L"\\\\srv1\\share1");
  EXPECT_EQ(table->Entries[3].DriveLetter, L'M');
  EXPECT_EQ(table->Entries[3].Flags, DOKAN_NP_MOUNT_CANCELABLE);
  EXPECT_STREQ(table->Entries[3].TransportKey, L"M:");

  DokanNpTableFree(table);
}

} // namespace
} // namespace dokan_np_test
