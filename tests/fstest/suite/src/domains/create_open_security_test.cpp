// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <algorithm>
#include <string>

namespace fstest {

void ForceLink_CreateOpenSecurity() {}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1.2 -- opening a directory requires
// FILE_FLAG_BACKUP_SEMANTICS; NT enforces FILE_DIRECTORY_FILE vs NON_DIRECTORY.
TEST_F(CreateOpenTest, OpenDirectoryRequiresBackupSemantics) {
  const std::wstring dir = TestPath(L"backup_sem_dir");
  const std::wstring file = TestPath(L"backup_sem_file.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(file, "regular"));

  auto no_backup = core::CreateWin32File(dir, GENERIC_READ, core::kShareAll,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(no_backup);
  EXPECT_WIN32_ERROR_CODE(no_backup.error, ERROR_ACCESS_DENIED);

  auto with_backup =
      core::CreateWin32File(dir, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(with_backup);
  with_backup.value->reset();

  auto file_as_dir = core::NtCreateFilePath(
      TestNtPath(L"backup_sem_file.dat"), FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0,
      core::kShareAll, nt::kFileOpen, nt::kFileDirectoryFile);
  EXPECT_NT_STATUS(file_as_dir.status, STATUS_NOT_A_DIRECTORY);

  auto dir_as_file = core::NtCreateFilePath(
      TestNtPath(L"backup_sem_dir"), FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0,
      core::kShareAll, nt::kFileOpen, nt::kFileNonDirectoryFile);
  EXPECT_NT_STATUS(dir_as_file.status, STATUS_FILE_IS_A_DIRECTORY);
}

// Spec: [MS-FSA] 2.1.5.3-2.1.5.4 -- handles opened read-only, write-only, or
// FILE_READ_ATTRIBUTES-only enforce granted access masks on ReadFile/WriteFile.
TEST_F(CreateOpenTest, AccessMaskEnforcementReadOnlyAndWriteOnly) {
  const std::wstring path = TestPath(L"access_mask.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "hello world"));

  // Read-only handle: ReadFile OK, WriteFile -> ERROR_ACCESS_DENIED.
  {
    auto h = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);
    auto data = core::ReadAllBytes(h.value->get());
    ASSERT_WIN32_OK(data);
    EXPECT_EQ(*data.value, "hello world");

    DWORD written = 0;
    const char buf[] = "nope";
    EXPECT_WIN32_ERROR(::WriteFile(h.value->get(), buf, 4, &written, nullptr),
                       ERROR_ACCESS_DENIED);
  }

  // Write-only handle: WriteFile OK, ReadFile -> ERROR_ACCESS_DENIED.
  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    char buf[16]{};
    DWORD bytes_read = 0;
    EXPECT_WIN32_ERROR(
        ::ReadFile(h.value->get(), buf, sizeof(buf), &bytes_read, nullptr),
        ERROR_ACCESS_DENIED);

    DWORD written = 0;
    const char update[] = "HELLO";
    EXPECT_WIN32_TRUE(
        ::WriteFile(h.value->get(), update, 5, &written, nullptr));
    EXPECT_EQ(written, 5UL);
  }

  // Metadata-only handle (FILE_READ_ATTRIBUTES): GetByHandleInfo OK,
  // ReadFile and WriteFile both fail ERROR_ACCESS_DENIED.
  {
    auto h = core::CreateWin32File(path, FILE_READ_ATTRIBUTES, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    auto info = core::GetByHandleInfo(h.value->get());
    ASSERT_WIN32_OK(info);
    EXPECT_EQ(info.value->nFileSizeLow, 11UL);

    char buf[16]{};
    DWORD transferred = 0;
    EXPECT_WIN32_ERROR(
        ::ReadFile(h.value->get(), buf, sizeof(buf), &transferred, nullptr),
        ERROR_ACCESS_DENIED);
    EXPECT_WIN32_ERROR(
        ::WriteFile(h.value->get(), "x", 1, &transferred, nullptr),
        ERROR_ACCESS_DENIED);
  }
}

// Spec: [MS-FSA] 2.1.5.1.2.2 -- FILE_READ_ATTRIBUTES and zero-access metadata
// opens bypass active data share-mode restrictions held with share=0.
TEST_F(CreateOpenTest, ShareModeAttributeOnlyHandleBypassesDataShare) {
  const std::wstring path = TestPath(L"attr_bypass.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "abcd"));

  auto h1 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, 0,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h1);

  auto data_blocked =
      core::CreateWin32File(path, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(data_blocked);
  EXPECT_WIN32_ERROR_CODE(data_blocked.error, ERROR_SHARING_VIOLATION);

  auto h_attr = core::CreateWin32File(path, FILE_READ_ATTRIBUTES, 0,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_attr);
  auto info1 = core::GetByHandleInfo(h_attr.value->get());
  ASSERT_WIN32_OK(info1);
  EXPECT_EQ(info1.value->nFileSizeLow, 4UL);
  h_attr.value->reset();

  auto h_zero =
      core::CreateWin32File(path, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_zero);
  auto info2 = core::GetByHandleInfo(h_zero.value->get());
  ASSERT_WIN32_OK(info2);
  EXPECT_EQ(info2.value->nFileSizeLow, 4UL);
  char buf[8]{};
  DWORD bytes_read = 0;
  EXPECT_WIN32_ERROR(
      ::ReadFile(h_zero.value->get(), buf, sizeof(buf), &bytes_read, nullptr),
      ERROR_ACCESS_DENIED);
  h_zero.value->reset();

  h1.value->reset();
}

// Spec: [MS-FSA] 2.1.5.1.2.2 / Win32 CreateFileW & NtCreateFile -- metadata,
// security, and synchronize-only opens bypass an active share_mode=0 data
// handle, whereas adding any data or delete access fails SHARING_VIOLATION.
TEST_F(CreateOpenTest, MetadataOnlyAccessBypassesZeroShareMode) {
  const std::wstring path = TestPath(L"meta_share_matrix.dat");
  const std::wstring nt_path = TestNtPath(L"meta_share_matrix.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "share_matrix_payload"));

  auto h_excl = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, 0,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_excl);

  struct AccessCase {
    const char *label;
    DWORD access;
  };
  constexpr AccessCase kAllowedMeta[] = {
      {"FILE_READ_ATTRIBUTES", FILE_READ_ATTRIBUTES},
      {"FILE_WRITE_ATTRIBUTES", FILE_WRITE_ATTRIBUTES},
      {"READ_CONTROL", READ_CONTROL},
      {"WRITE_DAC", WRITE_DAC},
      {"SYNCHRONIZE", SYNCHRONIZE},
  };

  for (const auto &c : kAllowedMeta) {
    SCOPED_TRACE(c.label);
    auto win_open = core::CreateWin32File(path, c.access, 0, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL);
    EXPECT_WIN32_OK(win_open);

    auto nt_open = core::NtCreateFilePath(
        nt_path, c.access, FILE_ATTRIBUTE_NORMAL, /*share_access=*/0,
        nt::kFileOpen, nt::kFileNonDirectoryFile);
    EXPECT_NT_STATUS(nt_open.status, STATUS_SUCCESS);
  }

  constexpr AccessCase kConflictingData[] = {
      {"FILE_READ_DATA", FILE_READ_DATA},
      {"FILE_WRITE_DATA", FILE_WRITE_DATA},
      {"FILE_APPEND_DATA", FILE_APPEND_DATA},
      {"DELETE", DELETE},
      {"FILE_WRITE_ATTRIBUTES|FILE_READ_DATA",
       FILE_WRITE_ATTRIBUTES | FILE_READ_DATA},
      {"READ_CONTROL|FILE_WRITE_DATA", READ_CONTROL | FILE_WRITE_DATA},
  };

  for (const auto &c : kConflictingData) {
    SCOPED_TRACE(c.label);
    auto win_blocked = core::CreateWin32File(
        path, c.access, core::kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(win_blocked);
    EXPECT_WIN32_ERROR_CODE(win_blocked.error, ERROR_SHARING_VIOLATION);

    auto nt_blocked = core::NtCreateFilePath(
        nt_path, c.access | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL, core::kShareAll,
        nt::kFileOpen, nt::kFileNonDirectoryFile);
    EXPECT_NT_STATUS(nt_blocked.status, STATUS_SHARING_VIOLATION);
  }
}

// Spec: [MS-FSA] 2.1.5.1.2.1 -- FILE_ATTRIBUTE_READONLY blocks GENERIC_WRITE
// and FILE_APPEND_DATA while permitting GENERIC_READ and FILE_WRITE_ATTRIBUTES.
TEST_F(CreateOpenTest, ReadOnlyFileBlocksWriteOpenAllowsReadAndWriteAttr) {
  const std::wstring path = TestPath(L"ro_access.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "readonly_data"));
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));

  auto w_open = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(w_open);
  EXPECT_WIN32_ERROR_CODE(w_open.error, ERROR_ACCESS_DENIED);

  auto app_open = core::CreateWin32File(path, FILE_APPEND_DATA, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(app_open);
  EXPECT_WIN32_ERROR_CODE(app_open.error, ERROR_ACCESS_DENIED);

  auto r_open = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(r_open);
  auto bytes = core::ReadAllBytes(r_open.value->get());
  ASSERT_WIN32_OK(bytes);
  EXPECT_EQ(*bytes.value, "readonly_data");
  r_open.value->reset();

  auto wa_open =
      core::CreateWin32File(path, FILE_WRITE_ATTRIBUTES, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(wa_open);
  wa_open.value->reset();

  EXPECT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
}

// Spec: [MS-FSA] 2.1.5.1 & 2.1.5.5 -- FILE_DELETE_ON_CLOSE removes on close,
// rejects READONLY files, and in raw NtCreateFile requires DELETE access.
TEST_F(CreateOpenTest, CreateFileDeleteOnCloseLifecycle) {
  // Win32 CREATE_ALWAYS | FILE_FLAG_DELETE_ON_CLOSE (Win32 auto-adds DELETE).
  {
    const std::wstring path = TestPath(L"doc_life.dat");
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE);
    ASSERT_WIN32_OK(h);

    auto listing = core::ListDirectoryWin32(TempDir().Path());
    ASSERT_WIN32_OK(listing);
    EXPECT_NE(std::find(listing.value->begin(), listing.value->end(),
                        std::wstring(L"doc_life.dat")),
              listing.value->end());

    h.value->reset();
    EXPECT_WIN32_ERROR(::DeleteFileW(path.c_str()), ERROR_FILE_NOT_FOUND);
  }

  // OPEN_EXISTING | FILE_FLAG_DELETE_ON_CLOSE on READONLY file fails.
  {
    const std::wstring path = TestPath(L"doc_ro.dat");
    auto h = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_READONLY);
    ASSERT_WIN32_OK(h);
    h.value->reset();

    // DELETE-only access: a GENERIC_WRITE open of a READONLY file is denied
    // regardless of FILE_FLAG_DELETE_ON_CLOSE, so only a non-write open proves
    // that delete-on-close itself is rejected (STATUS_CANNOT_DELETE).
    auto fail_del = core::CreateWin32File(path, DELETE | FILE_READ_ATTRIBUTES,
                                          core::kShareAll, OPEN_EXISTING,
                                          FILE_FLAG_DELETE_ON_CLOSE);
    EXPECT_FALSE(fail_del);
    EXPECT_WIN32_ERROR_CODE(fail_del.error, ERROR_ACCESS_DENIED);

    // The same open without FILE_FLAG_DELETE_ON_CLOSE succeeds, so the denial
    // above is attributable to delete-on-close alone.
    auto plain_del = core::CreateWin32File(path, DELETE | FILE_READ_ATTRIBUTES,
                                           core::kShareAll, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL);
    EXPECT_WIN32_OK(plain_del);
    if (plain_del) {
      plain_del.value->reset();
    }

    ASSERT_WIN32_TRUE(
        ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
    EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  }

  // Raw NtCreateFile(FILE_DELETE_ON_CLOSE) requires DELETE in DesiredAccess.
  {
    const std::wstring path = TestPath(L"nt_doc.dat");
    const std::wstring nt_path = TestNtPath(L"nt_doc.dat");

    auto no_delete = core::NtCreateFilePath(
        nt_path, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
        core::kShareAll, nt::kFileCreate,
        nt::kFileDeleteOnClose | nt::kFileNonDirectoryFile);
    EXPECT_NT_STATUS(no_delete.status, STATUS_INVALID_PARAMETER);

    auto with_delete = core::NtCreateFilePath(
        nt_path, DELETE | FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
        core::kShareAll, nt::kFileCreate,
        nt::kFileDeleteOnClose | nt::kFileNonDirectoryFile);
    ASSERT_NT_STATUS(with_delete.status, STATUS_SUCCESS);
    EXPECT_NE(::GetFileAttributesW(path.c_str()), INVALID_FILE_ATTRIBUTES);

    with_delete.value->reset();
    EXPECT_TRUE(helpers::CheckFileNotFound(path));
  }
}

// Spec: [MS-FSA] 2.1.5.1 -- traversing a regular file as a parent directory
// fails ERROR_PATH_NOT_FOUND (NT: STATUS_OBJECT_PATH_NOT_FOUND).
TEST_F(CreateOpenTest, ParentIsRegularFileReturnsPathNotFound) {
  const std::wstring file_parent = TestPath(L"parent_file.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(file_parent, "not_a_dir"));

  const std::wstring child = TestPath(L"parent_file.dat\\child.dat");
  auto open_res = core::CreateWin32File(child, GENERIC_WRITE, 0, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(open_res);
  EXPECT_WIN32_ERROR_CODE(open_res.error, ERROR_PATH_NOT_FOUND);

  EXPECT_WIN32_ERROR(::CreateDirectoryW(child.c_str(), nullptr),
                     ERROR_PATH_NOT_FOUND);

  auto nt_res = core::NtCreateFilePath(
      TestNtPath(L"parent_file.dat\\child.dat"), GENERIC_WRITE | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileCreate,
      nt::kFileNonDirectoryFile);
  EXPECT_NT_STATUS(nt_res.status, STATUS_OBJECT_PATH_NOT_FOUND);
}

// Spec: WDK NtCreateFile / [MS-FSA] 2.1.5.1 -- OBJECT_ATTRIBUTES.RootDirectory
// creates and opens direct and nested children relative to a directory handle.
TEST_F(CreateOpenTest, NtCreateFileRelativeToDirectoryHandle) {
  const std::wstring dir = TestPath(L"rel_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto dir_h = core::NtCreateFilePath(
      TestNtPath(L"rel_parent"),
      FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE |
          FILE_ADD_SUBDIRECTORY | SYNCHRONIZE,
      0, core::kShareAll, nt::kFileOpen,
      nt::kFileDirectoryFile | nt::kFileSynchronousIoNonalert);
  ASSERT_NT_STATUS(dir_h.status, STATUS_SUCCESS);

  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;
  auto child_create =
      core::OpenRelative(dir_h.value->get(), L"child.dat",
                         GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                         core::kShareAll, nt::kFileCreate, kSyncFile);
  ASSERT_NT_STATUS(child_create.status, STATUS_SUCCESS);
  EXPECT_EQ(child_create.information, nt::kFileCreated);
  ASSERT_WIN32_OK(
      core::WriteAllBytes(child_create.value->get(), "relative_io", 11));
  child_create.value->reset();

  auto child_open = core::OpenRelative(
      dir_h.value->get(), L"child.dat", GENERIC_READ | SYNCHRONIZE,
      core::kShareAll, nt::kFileOpen, kSyncFile);
  ASSERT_NT_STATUS(child_open.status, STATUS_SUCCESS);
  EXPECT_EQ(child_open.information, nt::kFileOpened);
  auto content = core::ReadAllBytes(child_open.value->get());
  ASSERT_WIN32_OK(content);
  EXPECT_EQ(*content.value, "relative_io");
  child_open.value->reset();

  // Nested relative subdirectory creation and child file creation.
  auto nested_dir = core::OpenRelative(
      dir_h.value->get(), L"nested", FILE_LIST_DIRECTORY | SYNCHRONIZE,
      core::kShareAll, nt::kFileCreate,
      nt::kFileDirectoryFile | nt::kFileSynchronousIoNonalert,
      FILE_ATTRIBUTE_DIRECTORY);
  ASSERT_NT_STATUS(nested_dir.status, STATUS_SUCCESS);
  nested_dir.value->reset();

  auto deep_file = core::OpenRelative(
      dir_h.value->get(), L"nested\\deep.dat", GENERIC_WRITE | SYNCHRONIZE,
      core::kShareAll, nt::kFileCreate, kSyncFile);
  ASSERT_NT_STATUS(deep_file.status, STATUS_SUCCESS);
  ASSERT_WIN32_OK(core::WriteAllBytes(deep_file.value->get(), "deep", 4));
  deep_file.value->reset();
  dir_h.value->reset();

  auto verify =
      core::ReadFileContents(TestPath(L"rel_parent\\nested\\deep.dat"));
  ASSERT_WIN32_OK(verify);
  EXPECT_EQ(*verify.value, "deep");
}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1.2.1 -- TRUNCATE_EXISTING fails
// with ERROR_ACCESS_DENIED on a READONLY file without modifying its contents.
TEST_F(CreateOpenTest, TruncateExistingOnReadOnlyFileFails) {
  const std::wstring path = TestPath(L"ro_trunc.dat");
  const std::string payload = "cannot_truncate_me";
  ASSERT_WIN32_OK(core::WriteFileContents(path, payload));
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));

  auto trunc_res = core::CreateWin32File(
      path, GENERIC_WRITE, 0, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(trunc_res);
  EXPECT_WIN32_ERROR_CODE(trunc_res.error, ERROR_ACCESS_DENIED);

  auto read_back = core::ReadFileContents(path);
  ASSERT_WIN32_OK(read_back);
  EXPECT_EQ(*read_back.value, payload);

  EXPECT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
}

// Spec: [MS-FSA] 2.1.5.1.1 -- CREATE_NEW with FILE_ATTRIBUTE_READONLY allows
// writes on the initial creation handle while blocking subsequent write opens.
TEST_F(CreateOpenTest, CreateNewReadOnlyAllowsWriteOnCreationHandle) {
  const std::wstring path = TestPath(L"create_new_ro.dat");
  const std::string payload = "written_during_create";

  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_READONLY);
  ASSERT_WIN32_OK(h);
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h.value->get(), payload.data(), payload.size()));
  h.value->reset();

  auto attrs = core::QueryWin32Attributes(path);
  ASSERT_WIN32_OK(attrs);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_READONLY, 0UL);

  auto read_back = core::ReadFileContents(path);
  ASSERT_WIN32_OK(read_back);
  EXPECT_EQ(*read_back.value, payload);

  auto reopen_write =
      core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(reopen_write);
  EXPECT_WIN32_ERROR_CODE(reopen_write.error, ERROR_ACCESS_DENIED);

  EXPECT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
}

// Spec: [MS-FSA] 2.1.5.1 -- FILE_ATTRIBUTE_READONLY on a directory does not
// prevent creating, modifying, renaming, or deleting child files inside it.
TEST_F(CreateOpenTest, ReadOnlyDirectoryAllowsChildCreateModifyDelete) {
  const std::wstring dir = TestPath(L"ro_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_READONLY));

  auto dir_attrs = core::QueryWin32Attributes(dir);
  ASSERT_WIN32_OK(dir_attrs);
  EXPECT_EQ(*dir_attrs.value &
                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY),
            DWORD{FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY});

  const std::wstring child = TestPath(L"ro_dir\\child.dat");
  const std::wstring renamed = TestPath(L"ro_dir\\renamed.dat");

  auto h_create = core::CreateWin32File(child, GENERIC_WRITE, core::kShareAll,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_create);
  ASSERT_WIN32_OK(core::WriteAllBytes(h_create.value->get(), "v1", 2));
  h_create.value->reset();

  auto h_mod = core::CreateWin32File(child, GENERIC_WRITE, core::kShareAll,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_mod);
  ASSERT_WIN32_OK(core::WriteAllBytes(h_mod.value->get(), "v2_mod", 6));
  h_mod.value->reset();

  ASSERT_WIN32_TRUE(::MoveFileExW(child.c_str(), renamed.c_str(), 0));

  auto read_back = core::ReadFileContents(renamed);
  ASSERT_WIN32_OK(read_back);
  EXPECT_EQ(*read_back.value, "v2_mod");

  ASSERT_WIN32_TRUE(::DeleteFileW(renamed.c_str()));
  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1.2 -- desired_access=0 opens a
// metadata handle past share=0 locks and data-deny DACLs while blocking reads.
TEST_F(CreateOpenTest,
       ZeroDesiredAccessOpensMetadataThroughExclusiveAndDenyDacl) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"zero_access.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "meta_payload"));

  // Part 1: Succeeds while H1 holds exclusive share=0.
  {
    auto h1 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, 0,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h1);

    auto h_zero =
        core::CreateWin32File(path, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_zero);

    auto info = core::GetByHandleInfo(h_zero.value->get());
    ASSERT_WIN32_OK(info);
    EXPECT_EQ(info.value->nFileSizeLow, 12UL);

    char buf[8]{};
    DWORD bytes_read = 0;
    EXPECT_WIN32_ERROR(
        ::ReadFile(h_zero.value->get(), buf, sizeof(buf), &bytes_read, nullptr),
        ERROR_ACCESS_DENIED);

    h_zero.value->reset();
    h1.value->reset();
  }

  // Part 2: Succeeds on a file whose DACL denies data read/write
  // (blocking GENERIC_READ) while permitting metadata/synchronize access.
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"zero_access.dat"));
  ASSERT_TRUE(dacl_guard.armed());

  // Deny FILE_READ_DATA|FILE_WRITE_DATA (0x3) so GENERIC_READ fails
  // ERROR_ACCESS_DENIED, while allowing FILE_READ_ATTRIBUTES|READ_CONTROL|
  // WRITE_DAC|SYNCHRONIZE (0x00160080) so CreateFileW(..., 0, ...) succeeds.
  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000003;;;WD)(A;;0x00160080;;;WD)"));

  auto gr_fail = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(gr_fail);
  EXPECT_WIN32_ERROR_CODE(gr_fail.error, ERROR_ACCESS_DENIED);

  auto h_zero = core::CreateWin32File(path, 0, core::kShareAll, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_zero);

  auto info = core::GetByHandleInfo(h_zero.value->get());
  ASSERT_WIN32_OK(info);
  EXPECT_EQ(info.value->nFileSizeLow, 12UL);
  h_zero.value->reset();

  auto reset_res =
      core::ResetDaclToFullControl(TestExtendedPath(L"zero_access.dat"), false);
  EXPECT_WIN32_OK(reset_res);
  if (reset_res) {
    dacl_guard.DisarmAndClose();
  }
}

// Spec: [MS-FSA] 2.1.5.1.2.1 & 2.1.5.12.1 / [MS-FSCC] 2.4.1 -- MAXIMUM_ALLOWED
// grants effective rights (stripping write on READONLY) in FileAccessInfo.
TEST_F(CreateOpenTest,
       MaximumAllowedGrantsEffectiveAccessAndQueriesFileAccessInfo) {
  const std::wstring path = TestPath(L"max_allowed.dat");
  const std::wstring nt_path = TestNtPath(L"max_allowed.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "max_data"));

  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;

  // Normal file: MAXIMUM_ALLOWED | SYNCHRONIZE grants read, write, append, and
  // attribute rights (FILE_SYNCHRONOUS_IO_NONALERT requires explicit
  // SYNCHRONIZE).
  {
    auto h_norm = core::NtCreateFilePath(nt_path, MAXIMUM_ALLOWED | SYNCHRONIZE,
                                         FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                         nt::kFileOpen, kSyncFile);
    ASSERT_NT_STATUS(h_norm.status, STATUS_SUCCESS);

    auto access = helpers::QueryFileAccessMask(h_norm.value->get());
    ASSERT_NT_STATUS(access.status, STATUS_SUCCESS);
    EXPECT_NE(*access.value & FILE_READ_DATA, 0UL);
    EXPECT_NE(*access.value & FILE_WRITE_DATA, 0UL);
    EXPECT_NE(*access.value & FILE_APPEND_DATA, 0UL);
    EXPECT_NE(*access.value & FILE_READ_ATTRIBUTES, 0UL);
    EXPECT_NE(*access.value & FILE_WRITE_ATTRIBUTES, 0UL);

    ASSERT_WIN32_OK(core::WriteAllBytes(h_norm.value->get(), "updated!", 8));
    h_norm.value->reset();
  }

  // Read-only file: MAXIMUM_ALLOWED | SYNCHRONIZE succeeds, grants read +
  // write-attributes, and strips FILE_WRITE_DATA / FILE_APPEND_DATA.
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
  {
    auto h_ro = core::NtCreateFilePath(nt_path, MAXIMUM_ALLOWED | SYNCHRONIZE,
                                       FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                       nt::kFileOpen, kSyncFile);
    ASSERT_NT_STATUS(h_ro.status, STATUS_SUCCESS);

    auto access = helpers::QueryFileAccessMask(h_ro.value->get());
    ASSERT_NT_STATUS(access.status, STATUS_SUCCESS);
    EXPECT_NE(*access.value & FILE_READ_DATA, 0UL);
    EXPECT_NE(*access.value & FILE_READ_ATTRIBUTES, 0UL);
    EXPECT_NE(*access.value & FILE_WRITE_ATTRIBUTES, 0UL);
    EXPECT_EQ(*access.value & FILE_WRITE_DATA, 0UL);
    EXPECT_EQ(*access.value & FILE_APPEND_DATA, 0UL);

    auto read_res = core::ReadAllBytes(h_ro.value->get());
    ASSERT_WIN32_OK(read_res);
    EXPECT_EQ(*read_res.value, "updated!");

    DWORD written = 0;
    EXPECT_WIN32_ERROR(
        ::WriteFile(h_ro.value->get(), "x", 1, &written, nullptr),
        ERROR_ACCESS_DENIED);
    h_ro.value->reset();
  }

  EXPECT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1.2.1 -- SeBackupPrivilege combined
// with FILE_FLAG_BACKUP_SEMANTICS bypasses a read-deny DACL on an open.
TEST_F(CreateOpenTest, BackupPrivilegeBypassesDenyDacl) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);
  REQUIRE_ADMIN();
  REQUIRE_PRIVILEGE(SE_BACKUP_NAME);

  const std::wstring path = TestPath(L"backup_deny.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "backup_secret_payload"));
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"backup_deny.dat"));
  ASSERT_TRUE(dacl_guard.armed());
  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(D;;GR;;;WD)"));

  // Without FILE_FLAG_BACKUP_SEMANTICS, GENERIC_READ must be denied.
  auto denied = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(denied);
  EXPECT_WIN32_ERROR_CODE(denied.error, ERROR_ACCESS_DENIED);

  // With SeBackupPrivilege enabled + FILE_FLAG_BACKUP_SEMANTICS, open succeeds.
  core::ScopedPrivilege backup_priv(SE_BACKUP_NAME, /*enable=*/true);
  ASSERT_TRUE(backup_priv.active())
      << core::FormatWin32Error(backup_priv.error());

  auto h_backup =
      core::CreateWin32File(path, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(h_backup);

  auto data = core::ReadAllBytes(h_backup.value->get());
  ASSERT_WIN32_OK(data);
  EXPECT_EQ(*data.value, "backup_secret_payload");
  h_backup.value->reset();
  backup_priv.Restore();

  auto reset_res =
      core::ResetDaclToFullControl(TestExtendedPath(L"backup_deny.dat"), false);
  EXPECT_WIN32_OK(reset_res);
  if (reset_res) {
    dacl_guard.DisarmAndClose();
  }
}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1.2.1 -- SeRestorePrivilege with
// FILE_FLAG_BACKUP_SEMANTICS bypasses a write-deny DACL for data/attr writes.
TEST_F(CreateOpenTest, RestorePrivilegeBypassesWriteDenyDacl) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);
  REQUIRE_PRIVILEGE(SE_RESTORE_NAME);

  const std::wstring path = TestPath(L"restore_deny.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "before_restore"));
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"restore_deny.dat"));
  ASSERT_TRUE(dacl_guard.armed());

  // Deny FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
  // FILE_WRITE_ATTRIBUTES (0x00000116) to Everyone while allowing
  // FILE_GENERIC_READ | WRITE_DAC (0x00160089) so CreateFileW's implicit
  // FILE_READ_ATTRIBUTES | SYNCHRONIZE and read-back verification succeed.
  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000116;;;WD)(A;;0x00160089;;;WD)"));

  // Without FILE_FLAG_BACKUP_SEMANTICS, GENERIC_WRITE must be denied.
  auto denied = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(denied);
  EXPECT_WIN32_ERROR_CODE(denied.error, ERROR_ACCESS_DENIED);

  // With SeRestorePrivilege enabled + FILE_FLAG_BACKUP_SEMANTICS, write open
  // bypasses the write-deny DACL.
  core::ScopedPrivilege restore_priv(SE_RESTORE_NAME, /*enable=*/true);
  ASSERT_TRUE(restore_priv.active())
      << core::FormatWin32Error(restore_priv.error());

  auto h_restore = core::CreateWin32File(
      path, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
      core::kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(h_restore);

  constexpr const char kUpdatedPayload[] = "after_restore_write!";
  auto write_res = core::WriteAllBytes(h_restore.value->get(), kUpdatedPayload,
                                       sizeof(kUpdatedPayload) - 1);
  ASSERT_WIN32_OK(write_res);
  h_restore.value->reset();
  restore_priv.Restore();

  auto read_back = core::ReadFileContents(path);
  ASSERT_WIN32_OK(read_back);
  EXPECT_EQ(*read_back.value, kUpdatedPayload);

  auto reset_res = core::ResetDaclToFullControl(
      TestExtendedPath(L"restore_deny.dat"), false);
  EXPECT_WIN32_OK(reset_res);
  if (reset_res) {
    dacl_guard.DisarmAndClose();
  }
}

// Spec: Win32 DuplicateHandle / [MS-FSA] 2.1.5.1.2.2 -- a duplicated handle
// references the same open, so the share_mode=0 reservation survives closing
// the original handle and is released only when the last duplicate closes.
TEST_F(CreateOpenTest, DuplicateHandlePreservesShareLock) {
  const std::wstring path = TestPath(L"dup_share.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "dup_payload"));

  auto h1 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, 0,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h1);

  core::UniqueHandle h_dup;
  ASSERT_WIN32_TRUE(::DuplicateHandle(::GetCurrentProcess(), h1.value->get(),
                                      ::GetCurrentProcess(), h_dup.put(), 0,
                                      FALSE, DUPLICATE_SAME_ACCESS));
  ASSERT_TRUE(h_dup.valid());

  // Close original handle; duplicate must still hold the exclusive share
  // lock.
  h1.value->reset();

  auto blocked = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(blocked);
  EXPECT_WIN32_ERROR_CODE(blocked.error, ERROR_SHARING_VIOLATION);

  auto data = core::ReadAllBytes(h_dup.get());
  ASSERT_WIN32_OK(data);
  EXPECT_EQ(*data.value, "dup_payload");

  h_dup.reset();

  auto unblocked = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(unblocked);
}

// Spec: Win32 ReOpenFile / [MS-FSA] 2.1.5.1 -- ReOpenFile opens the file
// behind an existing handle (not its original name), so it yields a handle
// with upgraded access even after the file was renamed.
TEST_F(CreateOpenTest, ReOpenFileUpgradesAccessAfterRename) {
  const std::wstring orig = TestPath(L"reopen_orig.dat");
  const std::wstring moved = TestPath(L"reopen_moved.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(orig, "before_reopen"));

  auto h_orig = core::CreateWin32File(orig, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_orig);

  ASSERT_WIN32_TRUE(::MoveFileExW(orig.c_str(), moved.c_str(), 0));

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle h_reopened(::ReOpenFile(
      h_orig.value->get(), GENERIC_READ | GENERIC_WRITE, core::kShareAll, 0));
  const DWORD reopen_err = ::GetLastError();
  ASSERT_TRUE(h_reopened.valid()) << core::FormatWin32Error(reopen_err);

  ASSERT_WIN32_OK(core::WriteAllBytes(h_reopened.get(), "after_reopen!", 13));
  h_reopened.reset();
  h_orig.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(orig));
  EXPECT_TRUE(helpers::CheckFileContents(moved, "after_reopen!"));
}

// Spec: WDK NtCreateFile / [MS-FSA] 2.1.5.1 -- FILE_DIRECTORY_FILE combined
// with FILE_NON_DIRECTORY_FILE is rejected with STATUS_INVALID_PARAMETER
// before any file is created.
TEST_F(CreateOpenTest, DirectoryAndNonDirectoryOptionsAreMutuallyExclusive) {
  auto bad_opts = core::NtCreateFilePath(
      TestNtPath(L"mut_excl.dat"), GENERIC_READ | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOpenIf,
      nt::kFileDirectoryFile | nt::kFileNonDirectoryFile);
  EXPECT_NT_STATUS(bad_opts.status, STATUS_INVALID_PARAMETER);
  EXPECT_TRUE(helpers::CheckFileNotFound(TestPath(L"mut_excl.dat")));
}

} // namespace fstest
