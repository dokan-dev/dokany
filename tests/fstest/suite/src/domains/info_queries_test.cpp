// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace fstest {
namespace {

// Opens the directory `dir_path` with FILE_READ_ATTRIBUTES | SYNCHRONIZE and
// full sharing for handle-based metadata and volume-information queries.
core::Win32Result<core::UniqueHandle>
OpenDirectoryForQuery(std::wstring_view dir_path) {
  return core::CreateWin32File(dir_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               core::kShareAll, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS);
}

// [MS-FSCC] FILE_FS_CONTROL_INFORMATION (FileFsControlInformation).
struct FileFsControlInfo {
  LARGE_INTEGER free_space_start_filtering;
  LARGE_INTEGER free_space_threshold;
  LARGE_INTEGER free_space_stop_filtering;
  LARGE_INTEGER default_quota_threshold;
  LARGE_INTEGER default_quota_limit;
  ULONG file_system_control_flags;
};
static_assert(sizeof(FileFsControlInfo) == 48);
static_assert(offsetof(FileFsControlInfo, file_system_control_flags) == 40);

// FILE_VC_VALID_MASK: every FileSystemControlFlags bit defined for
// FILE_FS_CONTROL_INFORMATION (FILE_VC_QUOTA_TRACK through
// FILE_VC_QUOTAS_REBUILDING).
constexpr ULONG kFileVcValidMask = 0x000003FFUL;

} // namespace

// Spec: GetVolumeInformationByHandleW agrees with FileFsAttributeInformation
// on filesystem name, MaxComponentLength (255), and volume capability flags.
TEST_F(InfoQueriesTest, GetVolumeInformationBasicFields) {
  auto dir_h = OpenDirectoryForQuery(TempDir().ExtendedPath());
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  wchar_t vol_name[MAX_PATH + 1]{};
  wchar_t fs_name[MAX_PATH + 1]{};
  DWORD serial = 0;
  DWORD max_comp = 0;
  DWORD flags = 0;
  ASSERT_WIN32_TRUE(::GetVolumeInformationByHandleW(
      dir_h.value->get(), vol_name,
      static_cast<DWORD>(sizeof(vol_name) / sizeof(vol_name[0])), &serial,
      &max_comp, &flags, fs_name,
      static_cast<DWORD>(sizeof(fs_name) / sizeof(fs_name[0]))));

  const std::wstring win32_fs(fs_name);
  EXPECT_FALSE(win32_fs.empty());
  EXPECT_EQ(max_comp, 255u);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryVolumeInformationFile, nullptr);
  std::vector<uint8_t> buf(1024, 0);
  nt::IoStatusBlock iosb{};
  const NTSTATUS st = ntdll.NtQueryVolumeInformationFile(
      dir_h.value->get(), &iosb, buf.data(), static_cast<ULONG>(buf.size()),
      nt::FsInfoClass::FileFsAttributeInformation);
  ASSERT_NT_SUCCESS(st);

  const auto *attr =
      reinterpret_cast<const nt::FileFsAttributeInfo *>(buf.data());
  const size_t max_bytes =
      buf.size() - offsetof(nt::FileFsAttributeInfo, file_system_name);
  const size_t name_bytes =
      std::min<size_t>(attr->file_system_name_length, max_bytes);
  const std::wstring nt_fs(attr->file_system_name,
                           name_bytes / sizeof(wchar_t));
  EXPECT_FALSE(nt_fs.empty());
  EXPECT_EQ(nt_fs, win32_fs);
  EXPECT_EQ(attr->maximum_component_name_length, 255L);
  EXPECT_EQ(attr->file_system_attributes, flags);
}

// Spec: [MS-FSCC] 2.5.1 / Win32 GetVolumeInformationByHandleW — full-NTFS
// volumes advertise named streams, persistent ACLs, sparse, reparse points,
// hard links, EAs, case preservation, and Unicode on disk.
TEST_F(InfoQueriesTest, GetVolumeInformationNtfsFeatureFlags) {
  REQUIRE_FULL_NTFS_FLAGS();

  auto dir_h = OpenDirectoryForQuery(TempDir().ExtendedPath());
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  DWORD max_comp = 0;
  DWORD flags = 0;
  ASSERT_WIN32_TRUE(::GetVolumeInformationByHandleW(
      dir_h.value->get(), nullptr, 0, nullptr, &max_comp, &flags, nullptr, 0));

  EXPECT_NE(flags & nt::kFileNamedStreams, 0u) << "Missing FILE_NAMED_STREAMS";
  EXPECT_NE(flags & nt::kFilePersistentAcls, 0u)
      << "Missing FILE_PERSISTENT_ACLS";
  EXPECT_NE(flags & nt::kFileSupportsSparseFiles, 0u)
      << "Missing FILE_SUPPORTS_SPARSE_FILES";
  EXPECT_NE(flags & nt::kFileSupportsReparsePoints, 0u)
      << "Missing FILE_SUPPORTS_REPARSE_POINTS";
  EXPECT_NE(flags & nt::kFileSupportsHardLinks, 0u)
      << "Missing FILE_SUPPORTS_HARD_LINKS";
  EXPECT_NE(flags & nt::kFileSupportsExtendedAttributes, 0u)
      << "Missing FILE_SUPPORTS_EXTENDED_ATTRIBUTES";
  EXPECT_NE(flags & nt::kFileCasePreservedNames, 0u)
      << "Missing FILE_CASE_PRESERVED_NAMES";
  EXPECT_NE(flags & nt::kFileUnicodeOnDisk, 0u)
      << "Missing FILE_UNICODE_ON_DISK";
}

// Spec: GetDiskFreeSpaceExW and FileFsSizeInformation report consistent
// non-zero volume capacity and power-of-two sector geometry (>= 512 bytes).
TEST_F(InfoQueriesTest, GetDiskFreeSpaceExValidGeometry) {
  ULARGE_INTEGER free_avail{};
  ULARGE_INTEGER total_bytes{};
  ULARGE_INTEGER total_free{};
  ASSERT_WIN32_TRUE(::GetDiskFreeSpaceExW(TempDir().Path().c_str(), &free_avail,
                                          &total_bytes, &total_free));
  EXPECT_GT(total_bytes.QuadPart, 0ULL);
  EXPECT_LE(free_avail.QuadPart, total_free.QuadPart);
  EXPECT_LE(total_free.QuadPart, total_bytes.QuadPart);

  auto dir_h = OpenDirectoryForQuery(TempDir().ExtendedPath());
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryVolumeInformationFile, nullptr);
  nt::FileFsSizeInfo fs_size{};
  nt::IoStatusBlock iosb{};
  const NTSTATUS st = ntdll.NtQueryVolumeInformationFile(
      dir_h.value->get(), &iosb, &fs_size, sizeof(fs_size),
      nt::FsInfoClass::FileFsSizeInformation);
  ASSERT_NT_SUCCESS(st);

  EXPECT_GT(fs_size.total_allocation_units.QuadPart, 0LL);
  EXPECT_GE(fs_size.bytes_per_sector, 512u);
  EXPECT_EQ(fs_size.bytes_per_sector & (fs_size.bytes_per_sector - 1u), 0u)
      << "BytesPerSector is not a power of two: " << fs_size.bytes_per_sector;
  EXPECT_GE(fs_size.sectors_per_allocation_unit, 1u);
}

// Spec: GetFileInformationByHandle returns a non-zero FileIndex that is
// distinct across files, identical across handles, and stable across rename.
TEST_F(InfoQueriesTest, FileIndexStabilityAndUniqueness) {
  const std::wstring path_a = TestPath(L"idx_a.dat");
  const std::wstring path_b = TestPath(L"idx_b.dat");
  const std::wstring path_a_renamed = TestPath(L"idx_a_renamed.dat");
  ASSERT_TRUE(core::WriteFileContents(path_a, "alpha"));
  ASSERT_TRUE(core::WriteFileContents(path_b, "beta"));

  uint64_t id_a1 = 0;
  {
    auto ha1 = core::CreateWin32File(path_a, GENERIC_READ, core::kShareAll,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    auto ha2 = core::CreateWin32File(path_a, GENERIC_READ, core::kShareAll,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    auto hb = core::CreateWin32File(path_b, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(ha1) << core::FormatWin32Error(ha1.error);
    ASSERT_TRUE(ha2) << core::FormatWin32Error(ha2.error);
    ASSERT_TRUE(hb) << core::FormatWin32Error(hb.error);

    auto r_a1 = core::GetFileIndex(ha1.value->get());
    auto r_a2 = core::GetFileIndex(ha2.value->get());
    auto r_b = core::GetFileIndex(hb.value->get());
    ASSERT_TRUE(r_a1);
    ASSERT_TRUE(r_a2);
    ASSERT_TRUE(r_b);

    id_a1 = *r_a1.value;
    EXPECT_NE(id_a1, 0ULL);
    EXPECT_EQ(*r_a2.value, id_a1);
    EXPECT_NE(*r_b.value, id_a1);
  }

  ASSERT_WIN32_TRUE(::MoveFileExW(path_a.c_str(), path_a_renamed.c_str(), 0));
  auto ha_after =
      core::CreateWin32File(path_a_renamed, GENERIC_READ, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(ha_after) << core::FormatWin32Error(ha_after.error);
  auto r_after = core::GetFileIndex(ha_after.value->get());
  ASSERT_TRUE(r_after);
  EXPECT_EQ(*r_after.value, id_a1);
}

// Spec: [MS-FSCC] 2.4.7/47 — FileBasicInformation and FileStandardInformation
// report accurate timestamps, attributes, sizes, link counts, and directory bit.
TEST_F(InfoQueriesTest, NtQueryFileBasicAndStandardInfo) {
  const std::wstring file_path = TestPath(L"basic_std.dat");
  const std::wstring dir_path = TestPath(L"basic_std_dir");
  const std::string payload(123, 'S');
  ASSERT_TRUE(core::WriteFileContents(
      file_path, payload, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_path.c_str(), nullptr));

  {
    auto fh = core::CreateWin32File(
        file_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

    auto basic = core::QueryBasicInfo(fh.value->get());
    ASSERT_NT_SUCCESS(basic.status);
    EXPECT_GT(basic.value->creation_time.QuadPart, 0LL);
    EXPECT_GT(basic.value->last_write_time.QuadPart, 0LL);
    EXPECT_GT(basic.value->change_time.QuadPart, 0LL);
    EXPECT_NE(MaskPolicyAttributes(basic.value->file_attributes) &
                  FILE_ATTRIBUTE_HIDDEN,
              0u);

    auto std_info = core::QueryStandardInfo(fh.value->get());
    ASSERT_NT_SUCCESS(std_info.status);
    EXPECT_EQ(std_info.value->end_of_file.QuadPart,
              static_cast<LONGLONG>(payload.size()));
    EXPECT_GE(std_info.value->allocation_size.QuadPart,
              std_info.value->end_of_file.QuadPart);
    EXPECT_EQ(std_info.value->number_of_links, 1u);
    EXPECT_EQ(std_info.value->delete_pending, FALSE);
    EXPECT_EQ(std_info.value->directory, FALSE);
  }

  {
    auto dh = OpenDirectoryForQuery(dir_path);
    ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);

    auto basic = core::QueryBasicInfo(dh.value->get());
    ASSERT_NT_SUCCESS(basic.status);
    EXPECT_NE(basic.value->file_attributes & FILE_ATTRIBUTE_DIRECTORY, 0u);

    auto std_info = core::QueryStandardInfo(dh.value->get());
    ASSERT_NT_SUCCESS(std_info.status);
    EXPECT_EQ(std_info.value->number_of_links, 1u);
    EXPECT_EQ(std_info.value->delete_pending, FALSE);
    EXPECT_EQ(std_info.value->directory, TRUE);
  }
}

// Spec: [MS-FSCC] 2.4.2/27 — FileInternalInformation matches Win32 FileIndex,
// and FileAllInformation overflows tiny buffers and fills all sub-structures.
TEST_F(InfoQueriesTest, NtQueryFileInternalAndAllInfo) {
  const std::wstring file_path = TestPath(L"all_info.dat");
  const std::string payload(77, 'Q');
  ASSERT_TRUE(core::WriteFileContents(file_path, payload));

  auto fh = core::CreateWin32File(file_path, GENERIC_READ, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  auto win32_idx = core::GetFileIndex(fh.value->get());
  ASSERT_TRUE(win32_idx) << core::FormatWin32Error(win32_idx.error);

  auto internal = core::QueryInternalInfo(fh.value->get());
  ASSERT_NT_SUCCESS(internal.status);
  EXPECT_EQ(static_cast<uint64_t>(internal.value->index_number.QuadPart),
            *win32_idx.value);

  auto basic = core::QueryBasicInfo(fh.value->get());
  ASSERT_NT_SUCCESS(basic.status);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  // Undersized buffer triggers STATUS_BUFFER_OVERFLOW when name does not fit
  // and populates FileNameLength > 0.
  {
    nt::FileAllInfo tiny{};
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &tiny, sizeof(tiny),
        nt::FileInfoClass::FileAllInformation);
    EXPECT_NT_STATUS(st, STATUS_BUFFER_OVERFLOW);
    EXPECT_GT(tiny.name_information.file_name_length, 0u);
  }

  // Full query with adequate buffer.
  std::vector<uint8_t> buf(4096, 0);
  nt::IoStatusBlock iosb{};
  const NTSTATUS st = ntdll.NtQueryInformationFile(
      fh.value->get(), &iosb, buf.data(), static_cast<ULONG>(buf.size()),
      nt::FileInfoClass::FileAllInformation);
  ASSERT_NT_SUCCESS(st);

  const auto *all = reinterpret_cast<const nt::FileAllInfo *>(buf.data());
  EXPECT_EQ(MaskPolicyAttributes(all->basic_information.file_attributes),
            MaskPolicyAttributes(basic.value->file_attributes));
  EXPECT_EQ(all->standard_information.end_of_file.QuadPart,
            static_cast<LONGLONG>(payload.size()));
  EXPECT_GE(all->standard_information.allocation_size.QuadPart,
            static_cast<LONGLONG>(payload.size()));
  EXPECT_EQ(all->standard_information.number_of_links, 1u);
  EXPECT_EQ(all->standard_information.delete_pending, FALSE);
  EXPECT_EQ(all->standard_information.directory, FALSE);
  EXPECT_EQ(all->internal_information.index_number.QuadPart,
            internal.value->index_number.QuadPart);
  EXPECT_EQ(all->ea_information.ea_size, 0u);
  EXPECT_NE(all->access_information.access_flags & FILE_READ_DATA, 0u);
  EXPECT_EQ(all->position_information.current_byte_offset.QuadPart, 0LL);
  EXPECT_NE(all->mode_information.mode & nt::kFileSynchronousIoNonalert, 0u);
  const ULONG align = all->alignment_information.alignment_requirement;
  EXPECT_EQ(align & (align + 1u), 0u);

  const size_t name_offset = offsetof(nt::FileAllInfo, name_information) +
                             offsetof(nt::FileNameInfo, file_name);
  ASSERT_GT(all->name_information.file_name_length, 0u);
  ASSERT_LE(name_offset + all->name_information.file_name_length, buf.size());
  const std::wstring name(all->name_information.file_name,
                          all->name_information.file_name_length /
                              sizeof(wchar_t));
  const std::wstring expected_suffix =
      L"\\" + TempDir().Basename() + L"\\all_info.dat";
  EXPECT_TRUE(core::EndsWithIgnoreCase(name, expected_suffix))
      << "FileAllInformation name '" << core::WideToUtf8(name)
      << "' does not end with '" << core::WideToUtf8(expected_suffix) << "'";
}

// Spec: [MS-FSCC] 2.4.32/35 — FileNameInformation reflects the opened path and
// FileNormalizedNameInformation returns canonical casing and long names.
TEST_F(InfoQueriesTest, NtQueryFileNameAndNormalizedNameInfo) {
  const std::wstring sub = TestPath(L"SubDir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  const std::wstring canonical_file = core::JoinPath(sub, L"MixedFile.txt");
  ASSERT_TRUE(core::WriteFileContents(canonical_file, "name-query"));

  const std::wstring open_lower = TestPath(L"subdir\\mixedfile.txt");
  auto fh = core::CreateWin32File(
      open_lower, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  const std::wstring expected_suffix =
      L"\\" + TempDir().Basename() + L"\\SubDir\\MixedFile.txt";

  auto name_res =
      core::QueryFileNameInfo(fh.value->get(), /*normalized=*/false);
  ASSERT_NT_SUCCESS(name_res.status);
  EXPECT_TRUE(core::EndsWithIgnoreCase(*name_res.value, expected_suffix))
      << "FileNameInformation '" << core::WideToUtf8(*name_res.value)
      << "' does not end with '" << core::WideToUtf8(expected_suffix) << "'";

  // FileNormalizedNameInformation (class 48) is supported on local NTFS on all
  // builds and over SMB/UNC starting in Windows 10 1809 / Server 2019 (17763).
  if (IsLocalVolume() || Os().build >= core::kBuildWin10_1809) {
    auto norm_res =
        core::QueryFileNameInfo(fh.value->get(), /*normalized=*/true);
    ASSERT_NT_SUCCESS(norm_res.status);
    EXPECT_TRUE(
        core::EndsWithExact(*norm_res.value, L"\\SubDir\\MixedFile.txt"))
        << "FileNormalizedNameInformation '"
        << core::WideToUtf8(*norm_res.value)
        << "' does not end with canonical '\\SubDir\\MixedFile.txt'";

    if (ShortNamesEnabled()) {
      const std::wstring long_name = L"Normalized Long Filename Test.txt";
      const std::wstring long_path = core::JoinPath(sub, long_name);
      ASSERT_TRUE(core::WriteFileContents(long_path, "short-norm"));

      WIN32_FIND_DATAW fd{};
      core::UniqueFindHandle find(::FindFirstFileW(long_path.c_str(), &fd));
      ASSERT_TRUE(find);
      ASSERT_NE(fd.cAlternateFileName[0], L'\0')
          << "ShortNamesEnabled() is true, expected non-empty 8.3 short name "
             "for '"
          << core::WideToUtf8(long_name) << "'";
      auto sh = core::CreateWin32File(
          core::JoinPath(sub, fd.cAlternateFileName),
          FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll, OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL);
      ASSERT_TRUE(sh) << core::FormatWin32Error(sh.error);
      auto long_norm =
          core::QueryFileNameInfo(sh.value->get(), /*normalized=*/true);
      ASSERT_NT_SUCCESS(long_norm.status);
      EXPECT_TRUE(
          core::EndsWithExact(*long_norm.value, L"\\SubDir\\" + long_name))
          << "Normalized name via 8.3 handle was '"
          << core::WideToUtf8(*long_norm.value) << "'";
    }
  }
}

// Spec: [MS-FSCC] 2.4.48 — FileStandardLinkInformation reports accessible and
// total link counts, delete-pending state, and directory flag on Win10 1809+.
TEST_F(InfoQueriesTest, NtQueryFileStandardLinkInfo) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  const std::wstring file_path = TestPath(L"std_link.dat");
  const std::wstring dir_path = TestPath(L"std_link_dir");
  ASSERT_TRUE(core::WriteFileContents(file_path, "links"));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_path.c_str(), nullptr));

  {
    auto fh = core::CreateWin32File(
        file_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);
    auto info = core::QueryStandardLinkInfo(fh.value->get());
    ASSERT_NT_SUCCESS(info.status);
    EXPECT_EQ(info.value->number_of_accessible_links, 1u);
    EXPECT_EQ(info.value->total_number_of_links, 1u);
    EXPECT_EQ(info.value->delete_pending, FALSE);
    EXPECT_EQ(info.value->directory, FALSE);
  }

  {
    auto dh = OpenDirectoryForQuery(dir_path);
    ASSERT_TRUE(dh) << core::FormatWin32Error(dh.error);
    auto info = core::QueryStandardLinkInfo(dh.value->get());
    ASSERT_NT_SUCCESS(info.status);
    EXPECT_EQ(info.value->number_of_accessible_links, 1u);
    EXPECT_EQ(info.value->total_number_of_links, 1u);
    EXPECT_EQ(info.value->delete_pending, FALSE);
    EXPECT_EQ(info.value->directory, TRUE);
  }
}

// Spec: GetFinalPathNameByHandleW (VOLUME_NAME_DOS and VOLUME_NAME_NONE)
// returns normalized paths tracking case renames and cross-directory moves.
TEST_F(InfoQueriesTest, GetFinalPathNameByHandleWFormats) {
  const std::wstring dir_a = TestPath(L"dir_a");
  const std::wstring dir_b = TestPath(L"dir_b");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_b.c_str(), nullptr));

  const std::wstring orig = core::JoinPath(dir_a, L"orig.txt");
  const std::wstring case_renamed = core::JoinPath(dir_a, L"Orig.TXT");
  const std::wstring moved = core::JoinPath(dir_b, L"Moved.TXT");
  ASSERT_TRUE(core::WriteFileContents(orig, "final-path-data"));

  auto check_final = [&](std::wstring_view path,
                         std::wstring_view expected_rel_suffix) {
    auto fh = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

    auto dos = core::QueryFinalPath(fh.value->get(),
                                    VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
    ASSERT_TRUE(dos) << core::FormatWin32Error(dos.error);
    EXPECT_EQ(dos.value->rfind(L"\\\\?\\", 0), size_t{0})
        << "VOLUME_NAME_DOS path: " << core::WideToUtf8(*dos.value);
    EXPECT_TRUE(core::EndsWithExact(*dos.value, expected_rel_suffix))
        << "VOLUME_NAME_DOS '" << core::WideToUtf8(*dos.value)
        << "' expected suffix '" << core::WideToUtf8(expected_rel_suffix)
        << "'";

    auto none = core::QueryFinalPath(fh.value->get(),
                                     VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
    ASSERT_TRUE(none) << core::FormatWin32Error(none.error);
    ASSERT_FALSE(none.value->empty());
    EXPECT_EQ((*none.value)[0], L'\\');
    EXPECT_NE(none.value->rfind(L"\\\\?\\", 0), size_t{0});
    EXPECT_TRUE(core::EndsWithExact(*none.value, expected_rel_suffix))
        << "VOLUME_NAME_NONE '" << core::WideToUtf8(*none.value)
        << "' expected suffix '" << core::WideToUtf8(expected_rel_suffix)
        << "'";
  };

  check_final(orig, L"\\" + TempDir().Basename() + L"\\dir_a\\orig.txt");
  ASSERT_FALSE(HasFatalFailure());

  ASSERT_WIN32_TRUE(::MoveFileExW(orig.c_str(), case_renamed.c_str(), 0));
  check_final(case_renamed,
              L"\\" + TempDir().Basename() + L"\\dir_a\\Orig.TXT");
  ASSERT_FALSE(HasFatalFailure());

  ASSERT_WIN32_TRUE(::MoveFileExW(case_renamed.c_str(), moved.c_str(), 0));
  check_final(moved, L"\\" + TempDir().Basename() + L"\\dir_b\\Moved.TXT");
}

// Spec: OpenFileById (64-bit FileIdType) and NtCreateFile(FILE_OPEN_BY_FILE_ID)
// open the target file directly by 64-bit index with matching data and ID.
TEST_F(InfoQueriesTest, OpenFileByIdAndNtOpenByFileId) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOpenByFileId);

  const std::wstring file_path = TestPath(L"by_id.dat");
  const std::string payload = "opened-by-file-id-payload";
  ASSERT_TRUE(core::WriteFileContents(file_path, payload));

  auto fh = core::CreateWin32File(file_path, GENERIC_READ, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  auto internal = core::QueryInternalInfo(fh.value->get());
  ASSERT_NT_SUCCESS(internal.status);
  auto expected_idx = core::GetFileIndex(fh.value->get());
  ASSERT_TRUE(expected_idx) << core::FormatWin32Error(expected_idx.error);
  fh.value->reset();

  auto dir_h = OpenDirectoryForQuery(TempDir().ExtendedPath());
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  // 1. Win32 OpenFileById (64-bit FileIdType)
  {
    FILE_ID_DESCRIPTOR desc{};
    desc.dwSize = sizeof(desc);
    desc.Type = FileIdType;
    desc.FileId = internal.value->index_number;

    ::SetLastError(ERROR_SUCCESS);
    core::UniqueHandle h_by_id(::OpenFileById(
        dir_h.value->get(), &desc, GENERIC_READ, core::kShareAll, nullptr, 0));
    const DWORD open_err = ::GetLastError();
    ASSERT_TRUE(h_by_id) << "OpenFileById failed: "
                         << core::FormatWin32Error(open_err);

    auto rd = core::ReadAllBytes(h_by_id.get());
    ASSERT_TRUE(rd) << core::FormatWin32Error(rd.error);
    EXPECT_EQ(*rd.value, payload);

    auto idx = core::GetFileIndex(h_by_id.get());
    ASSERT_TRUE(idx);
    EXPECT_EQ(*idx.value, *expected_idx.value);
  }

  // 2. NT NtCreateFile(FILE_OPEN_BY_FILE_ID) relative to directory handle
  {
    const auto &ntdll = core::Ntdll();
    ASSERT_NE(ntdll.NtCreateFile, nullptr);

    LONGLONG raw_id = internal.value->index_number.QuadPart;
    nt::UnicodeString id_us{};
    id_us.length = static_cast<USHORT>(sizeof(raw_id));
    id_us.maximum_length = static_cast<USHORT>(sizeof(raw_id));
    id_us.buffer = reinterpret_cast<PWSTR>(&raw_id);

    nt::ObjectAttributes oa =
        nt::MakeObjectAttributes(&id_us, /*attributes=*/0, dir_h.value->get());
    nt::IoStatusBlock iosb{};
    core::UniqueHandle h_nt;
    const NTSTATUS st = ntdll.NtCreateFile(
        h_nt.put(), FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa,
        &iosb, nullptr, 0, core::kShareAll, nt::kFileOpen,
        nt::kFileOpenByFileId | nt::kFileSynchronousIoNonalert, nullptr, 0);
    ASSERT_NT_SUCCESS(st);
    ASSERT_TRUE(h_nt);

    auto rd = core::ReadAllBytes(h_nt.get());
    ASSERT_TRUE(rd) << core::FormatWin32Error(rd.error);
    EXPECT_EQ(*rd.value, payload);

    auto idx = core::GetFileIndex(h_nt.get());
    ASSERT_TRUE(idx);
    EXPECT_EQ(*idx.value, *expected_idx.value);
  }
}

// Spec: [MS-FSCC] 2.4.26 — FileIdInformation returns a non-zero 128-bit FileId
// and volume serial that are unique per file and stable across rename.
TEST_F(InfoQueriesTest, FileId128BitInformation) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring path_a = TestPath(L"id128_a.dat");
  const std::wstring path_b = TestPath(L"id128_b.dat");
  const std::wstring path_a_renamed = TestPath(L"id128_a_renamed.dat");
  ASSERT_TRUE(core::WriteFileContents(path_a, "128a"));
  ASSERT_TRUE(core::WriteFileContents(path_b, "128b"));

  auto query_id128 = [&](HANDLE h) -> nt::FileIdInfo {
    nt::IoStatusBlock iosb{};
    nt::FileIdInfo info{};
    const NTSTATUS st = ntdll.NtQueryInformationFile(
        h, &iosb, &info, sizeof(info), nt::FileInfoClass::FileIdInformation);
    EXPECT_NT_SUCCESS(st);
    return info;
  };

  nt::FileIdInfo id_a1{};
  {
    auto ha1 = core::CreateWin32File(path_a, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                     core::kShareAll, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL);
    auto ha2 = core::CreateWin32File(path_a, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                     core::kShareAll, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL);
    auto hb = core::CreateWin32File(path_b, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                    core::kShareAll, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(ha1);
    ASSERT_TRUE(ha2);
    ASSERT_TRUE(hb);

    id_a1 = query_id128(ha1.value->get());
    const nt::FileIdInfo id_a2 = query_id128(ha2.value->get());
    const nt::FileIdInfo id_b = query_id128(hb.value->get());

    EXPECT_NE(id_a1.volume_serial_number, 0ULL);
    EXPECT_EQ(id_a2.volume_serial_number, id_a1.volume_serial_number);
    EXPECT_EQ(id_b.volume_serial_number, id_a1.volume_serial_number);
    EXPECT_FALSE(helpers::AllBytesEqual(id_a1.file_id.identifier,
                                        sizeof(id_a1.file_id.identifier), 0));
    EXPECT_EQ(std::memcmp(id_a1.file_id.identifier, id_a2.file_id.identifier,
                          sizeof(nt::FileId128)),
              0);
    EXPECT_NE(std::memcmp(id_a1.file_id.identifier, id_b.file_id.identifier,
                          sizeof(nt::FileId128)),
              0);
  }

  ASSERT_WIN32_TRUE(::MoveFileExW(path_a.c_str(), path_a_renamed.c_str(), 0));
  auto ha_after = core::CreateWin32File(
      path_a_renamed, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(ha_after);
  const nt::FileIdInfo id_after = query_id128(ha_after.value->get());
  EXPECT_EQ(id_after.volume_serial_number, id_a1.volume_serial_number);
  EXPECT_EQ(std::memcmp(id_after.file_id.identifier, id_a1.file_id.identifier,
                        sizeof(nt::FileId128)),
            0);
}

// Spec: [MS-FSCC] 2.4.40 FilePositionInformation — tracks synchronous
// read/write offsets and honors explicit NtSetInformationFile seeks.
TEST_F(InfoQueriesTest, NtQueryAndSetFilePositionInformation) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);
  ASSERT_NE(ntdll.NtSetInformationFile, nullptr);

  const std::wstring file_path = TestPath(L"pos_info.dat");
  ASSERT_TRUE(core::WriteFileContents(file_path, "0123456789abcdef"));

  auto query_pos = [&](HANDLE h) -> LONGLONG {
    nt::IoStatusBlock iosb{};
    nt::FilePositionInfo info{};
    const NTSTATUS st = ntdll.NtQueryInformationFile(
        h, &iosb, &info, sizeof(info),
        nt::FileInfoClass::FilePositionInformation);
    EXPECT_NT_SUCCESS(st);
    return info.current_byte_offset.QuadPart;
  };

  auto fh = core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                                  core::kShareAll, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);
  EXPECT_EQ(query_pos(fh.value->get()), 0LL);

  char buf[8]{};
  DWORD read = 0;
  ASSERT_WIN32_TRUE(::ReadFile(fh.value->get(), buf, 5, &read, nullptr));
  EXPECT_EQ(read, 5u);
  EXPECT_EQ(std::string_view(buf, read), "01234");
  EXPECT_EQ(query_pos(fh.value->get()), 5LL);

  nt::FilePositionInfo set_pos{};
  set_pos.current_byte_offset.QuadPart = 10;
  nt::IoStatusBlock iosb{};
  ASSERT_NT_SUCCESS(ntdll.NtSetInformationFile(
      fh.value->get(), &iosb, &set_pos, sizeof(set_pos),
      nt::FileInfoClass::FilePositionInformation));
  EXPECT_EQ(query_pos(fh.value->get()), 10LL);

  ASSERT_WIN32_TRUE(::ReadFile(fh.value->get(), buf, 4, &read, nullptr));
  EXPECT_EQ(read, 4u);
  EXPECT_EQ(std::string_view(buf, read), "abcd");
  EXPECT_EQ(query_pos(fh.value->get()), 14LL);

  DWORD written = 0;
  ASSERT_WIN32_TRUE(::WriteFile(fh.value->get(), "XY", 2, &written, nullptr));
  EXPECT_EQ(written, 2u);
  EXPECT_EQ(query_pos(fh.value->get()), 16LL);
}

// Spec: [MS-FSCC] 2.4.3/31 — FileModeInformation reflects synchronous,
// write-through, and no-buffering flags; AlignmentRequirement is a valid mask.
TEST_F(InfoQueriesTest, NtQueryFileModeAndAlignmentInformation) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring file_path = TestPath(L"mode_align.dat");
  ASSERT_TRUE(core::WriteFileContents(file_path, "mode-alignment"));

  auto query_mode = [&](HANDLE h) -> ULONG {
    nt::IoStatusBlock iosb{};
    nt::FileModeInfo info{};
    const NTSTATUS st = ntdll.NtQueryInformationFile(
        h, &iosb, &info, sizeof(info), nt::FileInfoClass::FileModeInformation);
    EXPECT_NT_SUCCESS(st);
    return info.mode;
  };

  // Standard synchronous handle.
  {
    auto h = core::CreateWin32File(file_path, GENERIC_READ, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    const ULONG mode = query_mode(h.value->get());
    EXPECT_NE(mode & nt::kFileSynchronousIoNonalert, 0u);
    EXPECT_EQ(mode & nt::kFileWriteThrough, 0u);
    EXPECT_EQ(mode & nt::kFileNoIntermediateBuffering, 0u);

    nt::IoStatusBlock iosb{};
    nt::FileAlignmentInfo align{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        h.value->get(), &iosb, &align, sizeof(align),
        nt::FileInfoClass::FileAlignmentInformation));
    EXPECT_EQ(align.alignment_requirement & (align.alignment_requirement + 1u),
              0u)
        << "Invalid AlignmentRequirement mask: 0x" << std::hex
        << align.alignment_requirement;
  }

  // Synchronous + WRITE_THROUGH + NO_BUFFERING.
  {
    auto h = core::CreateWin32File(
        file_path, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
        FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    const ULONG mode = query_mode(h.value->get());
    EXPECT_NE(mode & nt::kFileSynchronousIoNonalert, 0u);
    EXPECT_NE(mode & nt::kFileWriteThrough, 0u);
    EXPECT_NE(mode & nt::kFileNoIntermediateBuffering, 0u);
  }

  // Overlapped/async handle clears synchronous mode flags.
  {
    auto h = core::CreateWin32File(file_path, GENERIC_READ, core::kShareAll,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    const ULONG mode = query_mode(h.value->get());
    EXPECT_EQ(mode & nt::kFileSynchronousIoNonalert, 0u);
    EXPECT_EQ(mode & nt::kFileSynchronousIoAlert, 0u);
  }
}

// Spec: [MS-FSCC] 2.4.34 — FileNetworkOpenInformation, NtQueryAttributesFile,
// and NtQueryFullAttributesFile agree on attributes, sizes, and timestamps.
TEST_F(InfoQueriesTest, NtQueryNetworkOpenAndPathAttributesFastQueries) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);
  ASSERT_NE(ntdll.NtQueryAttributesFile, nullptr);
  ASSERT_NE(ntdll.NtQueryFullAttributesFile, nullptr);

  const std::wstring file_path = TestPath(L"fast_query.dat");
  const std::string payload(321, 'F');
  ASSERT_TRUE(core::WriteFileContents(
      file_path, payload, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));

  auto fh = core::CreateWin32File(file_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                  core::kShareAll, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  auto bhfi = core::GetByHandleInfo(fh.value->get());
  ASSERT_TRUE(bhfi) << core::FormatWin32Error(bhfi.error);
  auto basic = core::QueryBasicInfo(fh.value->get());
  ASSERT_NT_SUCCESS(basic.status);

  nt::FileNetworkOpenInfo net_open{};
  {
    nt::IoStatusBlock iosb{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &net_open, sizeof(net_open),
        nt::FileInfoClass::FileNetworkOpenInformation));
  }
  fh.value->reset();

  EXPECT_EQ(net_open.end_of_file.QuadPart,
            static_cast<LONGLONG>(payload.size()));
  EXPECT_GE(net_open.allocation_size.QuadPart,
            static_cast<LONGLONG>(payload.size()));
  EXPECT_EQ(MaskPolicyAttributes(net_open.file_attributes),
            MaskPolicyAttributes(bhfi.value->dwFileAttributes));
  EXPECT_EQ(net_open.creation_time.QuadPart,
            basic.value->creation_time.QuadPart);
  EXPECT_EQ(net_open.last_write_time.QuadPart,
            basic.value->last_write_time.QuadPart);
  EXPECT_EQ(net_open.change_time.QuadPart, basic.value->change_time.QuadPart);

  // Path-based NtQueryAttributesFile & NtQueryFullAttributesFile.
  std::wstring nt_path = TestNtPath(L"fast_query.dat");
  nt::UnicodeString us = nt::MakeUnicodeString(nt_path.data(), nt_path.size());
  nt::ObjectAttributes oa = nt::MakeObjectAttributes(&us);

  nt::FileBasicInfo attr_basic{};
  ASSERT_NT_SUCCESS(ntdll.NtQueryAttributesFile(&oa, &attr_basic));
  EXPECT_EQ(MaskPolicyAttributes(attr_basic.file_attributes),
            MaskPolicyAttributes(bhfi.value->dwFileAttributes));
  // OS-build threshold provenance: On Server 2016 (build 14393) and Server 2019
  // (build 17763), NTFS's NtQueryAttributesFile fast path populates
  // FileAttributes accurately while leaving CreationTime/LastWriteTime zeroed
  // (whereas NtQueryFullAttributesFile below always returns full timestamps).
  // Starting on Server 2022 (build 20348) and Server 2025 / Windows 11 24H2
  // (build 26100+), NTFS populates timestamps on NtQueryAttributesFile too, so
  // zeroed timestamps are a hard conformance failure on build >= 20348.
  if (Os().build >= core::kBuildServer2022 ||
      attr_basic.creation_time.QuadPart != 0) {
    EXPECT_EQ(attr_basic.creation_time.QuadPart,
              basic.value->creation_time.QuadPart);
  }
  if (Os().build >= core::kBuildServer2022 ||
      attr_basic.last_write_time.QuadPart != 0) {
    EXPECT_EQ(attr_basic.last_write_time.QuadPart,
              basic.value->last_write_time.QuadPart);
  }

  nt::FileNetworkOpenInfo full_net{};
  ASSERT_NT_SUCCESS(ntdll.NtQueryFullAttributesFile(&oa, &full_net));
  EXPECT_EQ(full_net.end_of_file.QuadPart,
            static_cast<LONGLONG>(payload.size()));
  EXPECT_EQ(full_net.allocation_size.QuadPart,
            net_open.allocation_size.QuadPart);
  EXPECT_EQ(MaskPolicyAttributes(full_net.file_attributes),
            MaskPolicyAttributes(bhfi.value->dwFileAttributes));
  EXPECT_EQ(full_net.creation_time.QuadPart, net_open.creation_time.QuadPart);
  EXPECT_EQ(full_net.last_write_time.QuadPart,
            net_open.last_write_time.QuadPart);

  // Non-existent path fails STATUS_OBJECT_NAME_NOT_FOUND.
  std::wstring missing_nt = TestNtPath(L"definitely_missing.dat");
  nt::UnicodeString missing_us =
      nt::MakeUnicodeString(missing_nt.data(), missing_nt.size());
  nt::ObjectAttributes missing_oa = nt::MakeObjectAttributes(&missing_us);
  nt::FileBasicInfo unused_basic{};
  EXPECT_NT_STATUS(ntdll.NtQueryAttributesFile(&missing_oa, &unused_basic),
                   STATUS_OBJECT_NAME_NOT_FOUND);
}

// Spec: [MS-FSCC] 2.4.6 FileAttributeTagInformation — returns ReparseTag==0
// and matching non-reparse attributes for regular files.
TEST_F(InfoQueriesTest, NtQueryFileAttributeTagRegularFile) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring plain_path = TestPath(L"plain_tag.dat");
  ASSERT_TRUE(
      core::WriteFileContents(plain_path, "no-reparse",
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));

  auto fh = core::CreateWin32File(
      plain_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  auto basic = core::QueryBasicInfo(fh.value->get());
  ASSERT_NT_SUCCESS(basic.status);

  nt::IoStatusBlock iosb{};
  nt::FileAttributeTagInfo tag_info{};
  ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
      fh.value->get(), &iosb, &tag_info, sizeof(tag_info),
      nt::FileInfoClass::FileAttributeTagInformation));
  EXPECT_EQ(MaskPolicyAttributes(tag_info.file_attributes),
            MaskPolicyAttributes(basic.value->file_attributes));
  EXPECT_EQ(tag_info.file_attributes & FILE_ATTRIBUTE_REPARSE_POINT, 0u);
  EXPECT_EQ(tag_info.reparse_tag, 0u);
}

// Spec: [MS-FSCC] 2.4.6 FileAttributeTagInformation — returns
// FILE_ATTRIBUTE_REPARSE_POINT and IO_REPARSE_TAG_MOUNT_POINT for junctions.
TEST_F(InfoQueriesTest, NtQueryFileAttributeTagJunctionReparsePoint) {
  REQUIRE_FS_CAPABILITY(FsCapability::kReparsePoints);
  REQUIRE_LOCAL_VOLUME();

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring target_dir = TestPath(L"tag_target");
  const std::wstring junc_dir = TestPath(L"tag_junction");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(target_dir.c_str(), nullptr));
  auto jres = core::CreateDirectoryJunction(junc_dir, target_dir);
  ASSERT_TRUE(jres) << core::FormatWin32Error(jres.error);

  {
    auto jh = core::CreateWin32File(
        junc_dir, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    ASSERT_TRUE(jh) << core::FormatWin32Error(jh.error);

    nt::IoStatusBlock iosb{};
    nt::FileAttributeTagInfo tag_info{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        jh.value->get(), &iosb, &tag_info, sizeof(tag_info),
        nt::FileInfoClass::FileAttributeTagInformation));
    EXPECT_NE(tag_info.file_attributes & FILE_ATTRIBUTE_REPARSE_POINT, 0u);
    EXPECT_NE(tag_info.file_attributes & FILE_ATTRIBUTE_DIRECTORY, 0u);
    EXPECT_EQ(tag_info.reparse_tag,
              static_cast<ULONG>(IO_REPARSE_TAG_MOUNT_POINT));
  }

  ASSERT_WIN32_TRUE(::RemoveDirectoryW(junc_dir.c_str()));
}

// Spec: FileStatInformation (68) and FileStatLxInformation (70) agree with
// basic, standard, and internal file info and report effective access rights
// plus zeroed LX metadata on non-WSL NTFS files.
TEST_F(InfoQueriesTest, NtQueryFileStatAndStatLxInformation) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring file_path = TestPath(L"stat_info.dat");
  const std::string payload(256, 'T');
  ASSERT_TRUE(core::WriteFileContents(
      file_path, payload, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE));

  // FileStatLxInformation reads Linux EA metadata and requires FILE_READ_EA in
  // addition to FILE_READ_ATTRIBUTES.
  auto fh = core::CreateWin32File(
      file_path, FILE_READ_ATTRIBUTES | FILE_READ_EA | SYNCHRONIZE,
      core::kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  auto basic = core::QueryBasicInfo(fh.value->get());
  auto std_info = core::QueryStandardInfo(fh.value->get());
  auto internal = core::QueryInternalInfo(fh.value->get());
  ASSERT_NT_SUCCESS(basic.status);
  ASSERT_NT_SUCCESS(std_info.status);
  ASSERT_NT_SUCCESS(internal.status);

  auto verify_stat_fields = [&](const auto &s) {
    EXPECT_EQ(s.file_id.QuadPart, internal.value->index_number.QuadPart);
    EXPECT_EQ(s.creation_time.QuadPart, basic.value->creation_time.QuadPart);
    EXPECT_EQ(s.last_write_time.QuadPart,
              basic.value->last_write_time.QuadPart);
    EXPECT_EQ(s.change_time.QuadPart, basic.value->change_time.QuadPart);
    EXPECT_EQ(s.end_of_file.QuadPart, static_cast<LONGLONG>(payload.size()));
    EXPECT_EQ(s.allocation_size.QuadPart,
              std_info.value->allocation_size.QuadPart);
    EXPECT_EQ(MaskPolicyAttributes(s.file_attributes),
              MaskPolicyAttributes(basic.value->file_attributes));
    EXPECT_EQ(s.reparse_tag, 0u);
    EXPECT_EQ(s.number_of_links, 1u);
    EXPECT_NE(s.effective_access & FILE_READ_DATA, 0u);
  };

  // FileStatInformation (class 68)
  {
    nt::IoStatusBlock iosb{};
    nt::FileStatInfo stat{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &stat, sizeof(stat),
        nt::FileInfoClass::FileStatInformation));
    verify_stat_fields(stat);
  }

  // FileStatLxInformation (class 70)
  {
    nt::IoStatusBlock iosb{};
    nt::FileStatLxInfo stat_lx{};
    stat_lx.lx_flags = 0xFFFFFFFFu;
    stat_lx.lx_uid = 0xFFFFFFFFu;
    stat_lx.lx_gid = 0xFFFFFFFFu;
    stat_lx.lx_mode = 0xFFFFFFFFu;
    stat_lx.lx_device_id_major = 0xFFFFFFFFu;
    stat_lx.lx_device_id_minor = 0xFFFFFFFFu;
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &stat_lx, sizeof(stat_lx),
        nt::FileInfoClass::FileStatLxInformation));
    verify_stat_fields(stat_lx);
    EXPECT_EQ(stat_lx.lx_flags, 0u);
    EXPECT_EQ(stat_lx.lx_uid, 0u);
    EXPECT_EQ(stat_lx.lx_gid, 0u);
    EXPECT_EQ(stat_lx.lx_mode, 0u);
    EXPECT_EQ(stat_lx.lx_device_id_major, 0u);
    EXPECT_EQ(stat_lx.lx_device_id_minor, 0u);
  }
}

// Spec: [MS-FSCC] 2.5.4/9/10 — FileFsVolumeInformation,
// FileFsDeviceInformation, and FileFsFullSizeInformation report valid volume data.
TEST_F(InfoQueriesTest, NtQueryVolumeFsVolumeDeviceAndFullSizeInfo) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryVolumeInformationFile, nullptr);

  auto dir_h = OpenDirectoryForQuery(TempDir().ExtendedPath());
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  DWORD win32_serial = 0;
  ASSERT_WIN32_TRUE(::GetVolumeInformationByHandleW(dir_h.value->get(), nullptr,
                                                    0, &win32_serial, nullptr,
                                                    nullptr, nullptr, 0));

  // 1. FileFsVolumeInformation (class 1)
  {
    std::vector<uint8_t> buf(1024, 0);
    nt::IoStatusBlock iosb{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryVolumeInformationFile(
        dir_h.value->get(), &iosb, buf.data(), static_cast<ULONG>(buf.size()),
        nt::FsInfoClass::FileFsVolumeInformation));
    ASSERT_GE(iosb.information, offsetof(nt::FileFsVolumeInfo, volume_label));
    const auto *vol =
        reinterpret_cast<const nt::FileFsVolumeInfo *>(buf.data());
    EXPECT_EQ(vol->volume_serial_number, win32_serial);
    EXPECT_LE(static_cast<size_t>(vol->volume_label_length),
              buf.size() - offsetof(nt::FileFsVolumeInfo, volume_label));
  }

  // 2. FileFsDeviceInformation (class 4)
  {
    nt::IoStatusBlock iosb{};
    nt::FileFsDeviceInfo dev{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryVolumeInformationFile(
        dir_h.value->get(), &iosb, &dev, sizeof(dev),
        nt::FsInfoClass::FileFsDeviceInformation));
    const bool valid_device_type =
        dev.device_type == FILE_DEVICE_DISK ||
        dev.device_type == FILE_DEVICE_NETWORK_FILE_SYSTEM ||
        dev.device_type == FILE_DEVICE_VIRTUAL_DISK ||
        dev.device_type == FILE_DEVICE_CD_ROM ||
        dev.device_type == FILE_DEVICE_FILE_SYSTEM;
    EXPECT_TRUE(valid_device_type)
        << "Unexpected FileFsDeviceInformation.DeviceType: 0x" << std::hex
        << dev.device_type;
  }

  // 3. FileFsFullSizeInformation (class 7)
  {
    nt::IoStatusBlock iosb{};
    nt::FileFsFullSizeInfo full_size{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryVolumeInformationFile(
        dir_h.value->get(), &iosb, &full_size, sizeof(full_size),
        nt::FsInfoClass::FileFsFullSizeInformation));
    EXPECT_GT(full_size.total_allocation_units.QuadPart, 0LL);
    EXPECT_LE(full_size.caller_available_allocation_units.QuadPart,
              full_size.actual_available_allocation_units.QuadPart);
    EXPECT_LE(full_size.actual_available_allocation_units.QuadPart,
              full_size.total_allocation_units.QuadPart);
    EXPECT_GE(full_size.bytes_per_sector, 512u);
    EXPECT_EQ(full_size.bytes_per_sector & (full_size.bytes_per_sector - 1u),
              0u);
    EXPECT_GE(full_size.sectors_per_allocation_unit, 1u);
  }
}

// Spec: [MS-FSA] "Server Requests a Query of File Information",
// FilePositionInformation — explicit-offset overlapped WriteFile and ReadFile
// I/O never advances CurrentByteOffset in FilePositionInformation.
TEST_F(InfoQueriesTest, OverlappedHandleFilePositionRemainsZero) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring file_path = TestPath(L"ovl_pos_zero.dat");
  auto fh = core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                                  core::kShareAll, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle ev(::CreateEventW(nullptr, /*bManualReset=*/TRUE,
                                       /*bInitialState=*/FALSE, nullptr));
  const DWORD event_err = ::GetLastError();
  ASSERT_TRUE(ev) << core::FormatWin32Error(event_err);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  std::string payload(64, '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>('A' + (i % 26));
  }

  // 1. Async WriteFile of 64 bytes at Offset=0.
  {
    ASSERT_WIN32_TRUE(::ResetEvent(ev.get()));
    OVERLAPPED write_ov = MakeOffsetOverlapped(/*offset=*/0, ev.get());
    const BOOL ok =
        ::WriteFile(fh.value->get(), payload.data(),
                    static_cast<DWORD>(payload.size()), nullptr, &write_ov);
    auto written = helpers::CompleteOverlappedIo(fh.value->get(), &write_ov,
                                                 ev.get(), ok, timeout_ms);
    ASSERT_TRUE(written) << core::FormatWin32Error(written.error);
    EXPECT_EQ(*written.value, static_cast<DWORD>(payload.size()));
  }

  // 2. Async ReadFile of 32 bytes at Offset=16.
  {
    ASSERT_WIN32_TRUE(::ResetEvent(ev.get()));
    std::string read_buf(32, '\0');
    OVERLAPPED read_ov = MakeOffsetOverlapped(/*offset=*/16, ev.get());
    const BOOL ok =
        ::ReadFile(fh.value->get(), read_buf.data(),
                   static_cast<DWORD>(read_buf.size()), nullptr, &read_ov);
    auto bytes_read = helpers::CompleteOverlappedIo(fh.value->get(), &read_ov,
                                                    ev.get(), ok, timeout_ms);
    ASSERT_TRUE(bytes_read) << core::FormatWin32Error(bytes_read.error);
    EXPECT_EQ(*bytes_read.value, 32u);
    EXPECT_EQ(read_buf, payload.substr(16, 32));
  }

  // 3. FilePositionInformation must remain 0 on an overlapped handle.
  nt::IoStatusBlock iosb{};
  nt::FilePositionInfo pos{};
  pos.current_byte_offset.QuadPart = -1;
  const NTSTATUS st =
      ntdll.NtQueryInformationFile(fh.value->get(), &iosb, &pos, sizeof(pos),
                                   nt::FileInfoClass::FilePositionInformation);
  ASSERT_NT_STATUS(st, STATUS_SUCCESS);
  EXPECT_EQ(pos.current_byte_offset.QuadPart, 0LL);
}

// Spec: [MS-FSCC] 2.5.1 — querying FileFsAttributeInformation into a truncated
// buffer returns STATUS_BUFFER_OVERFLOW with the copied prefix length.
TEST_F(InfoQueriesTest, FileFsAttributeInfoSmallBufferOverflow) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryVolumeInformationFile, nullptr);

  auto dir_h = OpenDirectoryForQuery(TempDir().ExtendedPath());
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  std::vector<uint8_t> full_buf(1024, 0);
  nt::IoStatusBlock full_iosb{};
  ASSERT_NT_SUCCESS(ntdll.NtQueryVolumeInformationFile(
      dir_h.value->get(), &full_iosb, full_buf.data(),
      static_cast<ULONG>(full_buf.size()),
      nt::FsInfoClass::FileFsAttributeInformation));

  const auto *full_attr =
      reinterpret_cast<const nt::FileFsAttributeInfo *>(full_buf.data());
  const size_t full_max_bytes =
      full_buf.size() - offsetof(nt::FileFsAttributeInfo, file_system_name);
  ASSERT_GT(full_attr->file_system_name_length, 0u);
  ASSERT_LE(static_cast<size_t>(full_attr->file_system_name_length),
            full_max_bytes);
  const std::wstring full_name(full_attr->file_system_name,
                               full_attr->file_system_name_length /
                                   sizeof(wchar_t));
  ASSERT_GT(full_name.size(), size_t{1})
      << "FileSystemName must contain at least 2 characters to test truncation";

  const size_t trunc_chars = (full_name.size() > 2) ? size_t{2} : size_t{1};
  const size_t small_size =
      offsetof(nt::FileFsAttributeInfo, file_system_name) +
      trunc_chars * sizeof(wchar_t);
  std::vector<uint8_t> small_buf(small_size, 0);
  nt::IoStatusBlock small_iosb{};
  const NTSTATUS st = ntdll.NtQueryVolumeInformationFile(
      dir_h.value->get(), &small_iosb, small_buf.data(),
      static_cast<ULONG>(small_buf.size()),
      nt::FsInfoClass::FileFsAttributeInformation);
  ASSERT_NT_STATUS(st, STATUS_BUFFER_OVERFLOW);

  const auto *small_attr =
      reinterpret_cast<const nt::FileFsAttributeInfo *>(small_buf.data());
  EXPECT_EQ(small_attr->file_system_attributes,
            full_attr->file_system_attributes);
  EXPECT_EQ(small_attr->maximum_component_name_length,
            full_attr->maximum_component_name_length);
  // On STATUS_BUFFER_OVERFLOW, NTFS writes the number of FileSystemName bytes
  // actually copied into the truncated buffer (not the untruncated full length)
  // and sets IoStatus.Information to the total bytes written.
  EXPECT_EQ(small_attr->file_system_name_length,
            static_cast<ULONG>(trunc_chars * sizeof(wchar_t)));
  EXPECT_EQ(small_iosb.information, small_size);
  const std::wstring prefix(small_attr->file_system_name, trunc_chars);
  EXPECT_EQ(prefix, full_name.substr(0, trunc_chars));
}

// Spec: [MS-FSCC] 2.4.5 FileAlternateNameInformation — NtQueryInformationFile
// returns the 8.3 short name matching FindFirstFileW for long filenames, and
// STATUS_SUCCESS with the file's own name for names already in 8.3 form.
TEST_F(InfoQueriesTest, NtQueryFileAlternateNameInformation) {
  REQUIRE_SHORT_NAMES_ENABLED();

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  const std::wstring long_path = TestPath(L"AlternateNameQueryExample.txt");
  const std::wstring short_only_path = TestPath(L"ABC123.TXT");
  ASSERT_TRUE(core::WriteFileContents(long_path, "alt-name-long"));
  ASSERT_TRUE(core::WriteFileContents(short_only_path, "alt-name-83"));

  WIN32_FIND_DATAW fd{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueFindHandle find(::FindFirstFileW(long_path.c_str(), &fd));
  const DWORD find_err = ::GetLastError();
  ASSERT_TRUE(find) << core::FormatWin32Error(find_err);
  ASSERT_NE(fd.cAlternateFileName[0], L'\0')
      << "Expected generated 8.3 alternate name for long filename";

  {
    auto fh = core::CreateWin32File(
        long_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

    std::vector<uint8_t> buf(512, 0);
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, buf.data(), static_cast<ULONG>(buf.size()),
        nt::FileInfoClass::FileAlternateNameInformation);
    ASSERT_NT_SUCCESS(st);
    ASSERT_GE(iosb.information, offsetof(nt::FileNameInfo, file_name));
    const auto *alt = reinterpret_cast<const nt::FileNameInfo *>(buf.data());
    ASSERT_GT(alt->file_name_length, 0u);
    ASSERT_EQ(alt->file_name_length % sizeof(wchar_t), size_t{0});
    ASSERT_LE(offsetof(nt::FileNameInfo, file_name) + alt->file_name_length,
              buf.size());
    const std::wstring alt_name(alt->file_name,
                                alt->file_name_length / sizeof(wchar_t));
    EXPECT_TRUE(core::EqualsIgnoreCase(alt_name, fd.cAlternateFileName))
        << "FileAlternateNameInformation '" << core::WideToUtf8(alt_name)
        << "' != FindFirstFileW cAlternateFileName '"
        << core::WideToUtf8(fd.cAlternateFileName) << "'";
  }

  // A file whose primary name is already valid 8.3 still returns
  // STATUS_SUCCESS on FileAlternateNameInformation with its own 8.3 name
  // when short names are enabled on NTFS.
  {
    auto sh = core::CreateWin32File(
        short_only_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(sh) << core::FormatWin32Error(sh.error);

    std::vector<uint8_t> buf(512, 0);
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryInformationFile(
        sh.value->get(), &iosb, buf.data(), static_cast<ULONG>(buf.size()),
        nt::FileInfoClass::FileAlternateNameInformation);
    ASSERT_NT_STATUS(st, STATUS_SUCCESS);
    ASSERT_GE(iosb.information, offsetof(nt::FileNameInfo, file_name));
    const auto *alt = reinterpret_cast<const nt::FileNameInfo *>(buf.data());
    ASSERT_GT(alt->file_name_length, 0u);
    ASSERT_EQ(alt->file_name_length % sizeof(wchar_t), size_t{0});
    ASSERT_LE(offsetof(nt::FileNameInfo, file_name) + alt->file_name_length,
              buf.size());
    const std::wstring alt_name(alt->file_name,
                                alt->file_name_length / sizeof(wchar_t));
    EXPECT_TRUE(core::EqualsIgnoreCase(alt_name, L"abc123.txt"))
        << "Expected 'abc123.txt', got '" << core::WideToUtf8(alt_name) << "'";
  }
}

// Spec: [MS-FSCC] 2.4.9 FileCompressionInformation — NtQueryInformationFile
// reports COMPRESSION_FORMAT_NONE initially and COMPRESSION_FORMAT_LZNT1 with
// CompressedFileSize < logical size after FSCTL_SET_COMPRESSION.
TEST_F(InfoQueriesTest, NtQueryFileCompressionInformation) {
  REQUIRE_FS_CAPABILITY(FsCapability::kFileCompression);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryInformationFile, nullptr);

  constexpr size_t kLogicalSize = 256u * 1024u;
  const std::wstring file_path = TestPath(L"compression_info.dat");
  auto fh =
      core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  const std::string compressible(kLogicalSize, 'A');
  ASSERT_TRUE(core::WriteAllBytes(fh.value->get(), compressible.data(),
                                  compressible.size()));
  ASSERT_WIN32_TRUE(::FlushFileBuffers(fh.value->get()));

  // Initially uncompressed.
  {
    nt::IoStatusBlock iosb{};
    nt::FileCompressionInfo before{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &before, sizeof(before),
        nt::FileInfoClass::FileCompressionInformation));
    EXPECT_EQ(before.compression_format,
              static_cast<USHORT>(COMPRESSION_FORMAT_NONE));
    EXPECT_EQ(before.compressed_file_size.QuadPart,
              static_cast<LONGLONG>(kLogicalSize));
  }

  // Enable LZNT1 compression and verify FileCompressionInformation updates.
  auto set_comp =
      core::SetCompressionState(fh.value->get(), COMPRESSION_FORMAT_LZNT1);
  ASSERT_TRUE(set_comp) << core::FormatWin32Error(set_comp.error);
  ASSERT_WIN32_TRUE(::FlushFileBuffers(fh.value->get()));

  {
    nt::IoStatusBlock iosb{};
    nt::FileCompressionInfo after{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &after, sizeof(after),
        nt::FileInfoClass::FileCompressionInformation));
    EXPECT_EQ(after.compression_format,
              static_cast<USHORT>(COMPRESSION_FORMAT_LZNT1));
    EXPECT_GT(after.compressed_file_size.QuadPart, 0LL);
    EXPECT_LT(after.compressed_file_size.QuadPart,
              static_cast<LONGLONG>(kLogicalSize));
    EXPECT_GT(after.compression_unit_shift, 0u);
  }

  // Disable compression and confirm format returns to COMPRESSION_FORMAT_NONE.
  auto clear_comp =
      core::SetCompressionState(fh.value->get(), COMPRESSION_FORMAT_NONE);
  ASSERT_TRUE(clear_comp) << core::FormatWin32Error(clear_comp.error);
  {
    nt::IoStatusBlock iosb{};
    nt::FileCompressionInfo restored{};
    ASSERT_NT_SUCCESS(ntdll.NtQueryInformationFile(
        fh.value->get(), &iosb, &restored, sizeof(restored),
        nt::FileInfoClass::FileCompressionInformation));
    EXPECT_EQ(restored.compression_format,
              static_cast<USHORT>(COMPRESSION_FORMAT_NONE));
  }
}

// Spec: [MS-FSCC] 2.5.7 FileFsSectorSizeInformation —
// NtQueryVolumeInformationFile reports valid power-of-two logical and physical
// sector sizes consistent with FileFsSizeInformation.
TEST_F(InfoQueriesTest, NtQueryFileFsSectorSizeInformation) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryVolumeInformationFile, nullptr);

  auto dir_h = OpenDirectoryForQuery(TempDir().ExtendedPath());
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  nt::IoStatusBlock size_iosb{};
  nt::FileFsSizeInfo fs_size{};
  ASSERT_NT_SUCCESS(ntdll.NtQueryVolumeInformationFile(
      dir_h.value->get(), &size_iosb, &fs_size, sizeof(fs_size),
      nt::FsInfoClass::FileFsSizeInformation));

  nt::IoStatusBlock sec_iosb{};
  nt::FileFsSectorSizeInfo sec{};
  ASSERT_NT_SUCCESS(ntdll.NtQueryVolumeInformationFile(
      dir_h.value->get(), &sec_iosb, &sec, sizeof(sec),
      nt::FsInfoClass::FileFsSectorSizeInformation));

  EXPECT_GE(sec.logical_bytes_per_sector, 512u);
  EXPECT_EQ(sec.logical_bytes_per_sector & (sec.logical_bytes_per_sector - 1u),
            0u)
      << "LogicalBytesPerSector is not a power of two: "
      << sec.logical_bytes_per_sector;
  EXPECT_EQ(sec.logical_bytes_per_sector, fs_size.bytes_per_sector);

  EXPECT_GE(sec.physical_bytes_per_sector_for_atomicity,
            sec.logical_bytes_per_sector);
  EXPECT_EQ(sec.physical_bytes_per_sector_for_atomicity &
                (sec.physical_bytes_per_sector_for_atomicity - 1u),
            0u);

  EXPECT_GE(sec.physical_bytes_per_sector_for_performance,
            sec.logical_bytes_per_sector);
  EXPECT_EQ(sec.physical_bytes_per_sector_for_performance &
                (sec.physical_bytes_per_sector_for_performance - 1u),
            0u);

  EXPECT_GE(sec.file_system_effective_physical_bytes_per_sector_for_atomicity,
            sec.logical_bytes_per_sector);
}

// Spec: [MS-FSCC] FileStandardInformation — DeletePending is TRUE while a
// delete disposition is set on the file through any handle and returns to
// FALSE once the disposition is cleared.
TEST_F(InfoQueriesTest, NtQueryFileStandardInfoDeletePendingTracksDisposition) {
  const std::wstring file_path = TestPath(L"delete_pending.dat");
  ASSERT_TRUE(core::WriteFileContents(file_path, "delete-pending"));

  auto fh = core::CreateWin32File(
      file_path, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);

  auto before = core::QueryStandardInfo(fh.value->get());
  ASSERT_NT_SUCCESS(before.status);
  EXPECT_EQ(before.value->delete_pending, FALSE);

  ASSERT_NT_SUCCESS(
      core::SetDisposition(fh.value->get(), /*delete_file=*/true).status);
  auto pending = core::QueryStandardInfo(fh.value->get());
  ASSERT_NT_SUCCESS(pending.status);
  EXPECT_EQ(pending.value->delete_pending, TRUE);

  ASSERT_NT_SUCCESS(
      core::SetDisposition(fh.value->get(), /*delete_file=*/false).status);
  auto cleared = core::QueryStandardInfo(fh.value->get());
  ASSERT_NT_SUCCESS(cleared.status);
  EXPECT_EQ(cleared.value->delete_pending, FALSE);
}

// Spec: [MS-FSCC] FileFsControlInformation — NTFS returns a complete
// FILE_FS_CONTROL_INFORMATION (quota defaults and control flags) whose
// FileSystemControlFlags contain only defined FILE_VC_* bits.
TEST_F(InfoQueriesTest, NtQueryVolumeFsControlInformation) {
  REQUIRE_FULL_NTFS_FLAGS();
  REQUIRE_LOCAL_VOLUME();

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryVolumeInformationFile, nullptr);

  // FILE_LIST_DIRECTORY shares its bit with FILE_READ_DATA, which satisfies
  // the I/O manager's access requirement for querying this class.
  auto dir_h = core::CreateWin32File(
      TempDir().ExtendedPath(),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(dir_h) << core::FormatWin32Error(dir_h.error);

  nt::IoStatusBlock iosb{};
  FileFsControlInfo control{};
  control.file_system_control_flags = 0xFFFFFFFFUL;
  ASSERT_NT_SUCCESS(ntdll.NtQueryVolumeInformationFile(
      dir_h.value->get(), &iosb, &control, static_cast<ULONG>(sizeof(control)),
      nt::FsInfoClass::FileFsControlInformation));
  EXPECT_EQ(iosb.information, sizeof(FileFsControlInfo));
  EXPECT_EQ(control.file_system_control_flags & ~kFileVcValidMask, 0u)
      << "Undefined FileSystemControlFlags bits: 0x" << std::hex
      << control.file_system_control_flags;
}

// Spec: FSCTL_READ_FILE_USN_DATA (NTFS change journal) returns a USN_RECORD_V2
// for any open file carrying its 64-bit file reference number, its parent
// directory's file reference number, and its final path component.
TEST_F(InfoQueriesTest, ReadFileUsnDataMatchesFileIdParentAndName) {
  REQUIRE_FULL_NTFS_FLAGS();
  REQUIRE_LOCAL_VOLUME();

  const std::wstring file_name = L"usn_record.dat";
  const std::wstring file_path = TestPath(file_name);
  ASSERT_TRUE(core::WriteFileContents(file_path, "usn-data"));

  auto parent_idx =
      core::QueryPathFileIndex(TempDir().Path(), /*backup_semantics=*/true);
  ASSERT_TRUE(parent_idx) << core::FormatWin32Error(parent_idx.error);

  auto fh = core::CreateWin32File(file_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                  core::kShareAll, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(fh) << core::FormatWin32Error(fh.error);
  auto internal = core::QueryInternalInfo(fh.value->get());
  ASSERT_NT_SUCCESS(internal.status);

  // 8-byte aligned 1 KiB output buffer (a USN_RECORD_V2 with a short name
  // needs well under 128 bytes).
  std::vector<uint64_t> buf(128, 0ULL);
  const DWORD buf_bytes = static_cast<DWORD>(buf.size() * sizeof(uint64_t));
  DWORD returned = 0;
  ASSERT_WIN32_TRUE(::DeviceIoControl(fh.value->get(), FSCTL_READ_FILE_USN_DATA,
                                      nullptr, 0, buf.data(), buf_bytes,
                                      &returned, nullptr));
  ASSERT_GE(static_cast<size_t>(returned), offsetof(USN_RECORD, FileName));
  ASSERT_LE(returned, buf_bytes);

  const auto *rec = reinterpret_cast<const USN_RECORD *>(buf.data());
  EXPECT_LE(rec->RecordLength, returned);
  EXPECT_EQ(rec->MajorVersion, static_cast<WORD>(2));
  EXPECT_EQ(static_cast<uint64_t>(rec->FileReferenceNumber),
            static_cast<uint64_t>(internal.value->index_number.QuadPart));
  EXPECT_EQ(static_cast<uint64_t>(rec->ParentFileReferenceNumber),
            *parent_idx.value);

  const size_t name_offset = static_cast<size_t>(rec->FileNameOffset);
  const size_t name_bytes = static_cast<size_t>(rec->FileNameLength);
  ASSERT_GE(name_offset, offsetof(USN_RECORD, FileName));
  ASSERT_EQ(name_bytes % sizeof(wchar_t), size_t{0});
  ASSERT_LE(name_offset + name_bytes, static_cast<size_t>(returned));
  const std::wstring rec_name(
      reinterpret_cast<const wchar_t *>(
          reinterpret_cast<const uint8_t *>(buf.data()) + name_offset),
      name_bytes / sizeof(wchar_t));
  EXPECT_TRUE(core::EqualsIgnoreCase(rec_name, file_name))
      << "USN_RECORD FileName '" << core::WideToUtf8(rec_name) << "' != '"
      << core::WideToUtf8(file_name) << "'";
}

void ForceLink_InfoQueries() {}

} // namespace fstest
