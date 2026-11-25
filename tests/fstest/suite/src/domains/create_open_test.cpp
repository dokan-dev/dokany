// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/os_info.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <cstdint>
#include <ios>
#include <sstream>
#include <string>

namespace fstest {
namespace {

::testing::AssertionResult CheckFileSize(const std::wstring &path,
                                         int64_t expected_size) {
  auto info = core::QueryInfoByPath(path, /*backup_semantics=*/true);
  if (!info) {
    return ::testing::AssertionFailure()
           << "Failed to query info for '" << core::WideToUtf8(path)
           << "': " << core::FormatWin32Error(info.error);
  }
  const int64_t actual = static_cast<int64_t>(core::FileSizeOf(*info.value));
  if (actual != expected_size) {
    return ::testing::AssertionFailure()
           << "File size mismatch for '" << core::WideToUtf8(path)
           << "': actual=" << actual << ", expected=" << expected_size;
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckPolicyAttributes(const std::wstring &path,
                                                 DWORD expected_attrs) {
  auto attrs = core::QueryWin32Attributes(path);
  if (!attrs) {
    return ::testing::AssertionFailure()
           << "Failed to query attributes for '" << core::WideToUtf8(path)
           << "': " << core::FormatWin32Error(attrs.error);
  }
  const DWORD actual = MaskPolicyAttributes(*attrs.value);
  if (actual != expected_attrs) {
    std::ostringstream oss;
    oss << "Policy-masked attributes mismatch for '" << core::WideToUtf8(path)
        << "': actual=0x" << std::hex << std::uppercase << actual
        << ", expected=0x" << expected_attrs;
    return ::testing::AssertionFailure() << oss.str();
  }
  return ::testing::AssertionSuccess();
}

} // namespace

void ForceLink_CreateOpen() {}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1.1-2.1.5.1.2 -- CREATE_NEW creates
// absent files (ERROR_SUCCESS) and rejects existing ones (ERROR_FILE_EXISTS).
TEST_F(CreateOpenTest, CreateNewSucceedsAndRejectsExisting) {
  const std::wstring path = TestPath(L"create_new.dat");

  auto first = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(first);
  EXPECT_WIN32_ERROR_CODE(first.last_error, ERROR_SUCCESS);
  first.handle.reset();

  auto second = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(second.handle.valid());
  EXPECT_WIN32_ERROR_CODE(second.last_error, ERROR_FILE_EXISTS);

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  EXPECT_WIN32_ERROR(::DeleteFileW(path.c_str()), ERROR_FILE_NOT_FOUND);
}

// Spec: Win32 CreateFileW -- CREATE_ALWAYS returns ERROR_SUCCESS on first
// creation and ERROR_ALREADY_EXISTS when overwriting an existing file.
TEST_F(CreateOpenTest, CreateAlwaysSetsAlreadyExistsLastError) {
  const std::wstring path = TestPath(L"create_always.dat");

  auto first = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(first);
  EXPECT_WIN32_ERROR_CODE(first.last_error, ERROR_SUCCESS);
  first.handle.reset();

  auto second = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(second);
  EXPECT_WIN32_ERROR_CODE(second.last_error, ERROR_ALREADY_EXISTS);
  second.handle.reset();

  auto third = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(third);
  EXPECT_WIN32_ERROR_CODE(third.last_error, ERROR_ALREADY_EXISTS);
  third.handle.reset();

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: Win32 CreateFileW -- OPEN_ALWAYS creates an absent file with
// ERROR_SUCCESS and opens an existing file with ERROR_ALREADY_EXISTS.
TEST_F(CreateOpenTest, OpenAlwaysCreatesAndOpensWithAlreadyExists) {
  const std::wstring path = TestPath(L"open_always.dat");

  auto first = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(first);
  EXPECT_WIN32_ERROR_CODE(first.last_error, ERROR_SUCCESS);
  first.handle.reset();

  auto second = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(second);
  EXPECT_WIN32_ERROR_CODE(second.last_error, ERROR_ALREADY_EXISTS);
  second.handle.reset();

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1 -- OPEN_EXISTING fails
// ERROR_FILE_NOT_FOUND when absent and succeeds (ERROR_SUCCESS) when present.
TEST_F(CreateOpenTest, OpenExistingNotFoundAndExisting) {
  const std::wstring path = TestPath(L"open_existing.dat");

  auto missing = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(missing.handle.valid());
  EXPECT_WIN32_ERROR_CODE(missing.last_error, ERROR_FILE_NOT_FOUND);

  auto created = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(created);
  created.handle.reset();

  auto existing = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(existing);
  EXPECT_WIN32_ERROR_CODE(existing.last_error, ERROR_SUCCESS);
  existing.handle.reset();

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: Win32 CreateFileW -- TRUNCATE_EXISTING fails if absent, requires
// GENERIC_WRITE (else ERROR_INVALID_PARAMETER), and truncates existing data.
TEST_F(CreateOpenTest, TruncateExistingRequiresWriteAndTruncates) {
  const std::wstring path = TestPath(L"truncate_existing.dat");

  auto missing = core::CreateWin32FileEx(
      path, GENERIC_WRITE, 0, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(missing.handle.valid());
  EXPECT_WIN32_ERROR_CODE(missing.last_error, ERROR_FILE_NOT_FOUND);

  const std::string payload(42, 'A');
  auto write_res = core::WriteFileContents(path, payload);
  ASSERT_WIN32_OK(write_res);
  EXPECT_TRUE(CheckFileSize(path, 42));

  // TRUNCATE_EXISTING requires GENERIC_WRITE in Win32.
  auto ro_trunc =
      core::CreateWin32FileEx(path, GENERIC_READ, 0, TRUNCATE_EXISTING, 0);
  EXPECT_FALSE(ro_trunc.handle.valid());
  EXPECT_WIN32_ERROR_CODE(ro_trunc.last_error, ERROR_INVALID_PARAMETER);
  EXPECT_TRUE(CheckFileSize(path, 42));

  auto rw_trunc =
      core::CreateWin32FileEx(path, GENERIC_WRITE, 0, TRUNCATE_EXISTING, 0);
  ASSERT_WIN32_OK(rw_trunc);
  EXPECT_WIN32_ERROR_CODE(rw_trunc.last_error, ERROR_SUCCESS);
  rw_trunc.handle.reset();

  EXPECT_TRUE(CheckFileSize(path, 0));

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: Win32 CreateFileW & CreateDirectoryW / [MS-FSA] 2.1.5.1 -- creating a
// file or directory under a missing parent fails with ERROR_PATH_NOT_FOUND.
TEST_F(CreateOpenTest, CreateInMissingParentReturnsPathNotFound) {
  const std::wstring missing_child = TestPath(L"missing_parent\\child.dat");

  auto file_res = core::CreateWin32FileEx(missing_child, GENERIC_WRITE, 0,
                                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(file_res.handle.valid());
  EXPECT_WIN32_ERROR_CODE(file_res.last_error, ERROR_PATH_NOT_FOUND);

  EXPECT_WIN32_ERROR(::CreateDirectoryW(missing_child.c_str(), nullptr),
                     ERROR_PATH_NOT_FOUND);
}

// Spec: [MS-FSA] 2.1.5.1.1-2.1.5.1.2 -- CREATE_ALWAYS sets ARCHIVE plus
// requested READONLY/SYSTEM/HIDDEN attributes and rejects overwriting READONLY.
TEST_F(CreateOpenTest, CreateAlwaysAttributesAndReadOnlySystemHidden) {
  const std::wstring path = TestPath(L"create_attrs.dat");

  // CREATE_ALWAYS + FILE_ATTRIBUTE_NORMAL -> ARCHIVE (0x20).
  auto h1 = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h1);
  h1.value->reset();
  EXPECT_TRUE(CheckPolicyAttributes(path, DWORD{FILE_ATTRIBUTE_ARCHIVE}));

  // Overwriting a normal file with CREATE_ALWAYS + READONLY succeeds -> 0x21.
  auto h2 = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_READONLY);
  ASSERT_WIN32_OK(h2);
  h2.value->reset();
  EXPECT_TRUE(CheckPolicyAttributes(
      path, DWORD{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY}));

  // Overwriting an already-READONLY file with CREATE_ALWAYS fails
  // ACCESS_DENIED.
  auto h2_fail = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(h2_fail);
  EXPECT_WIN32_ERROR_CODE(h2_fail.error, ERROR_ACCESS_DENIED);

  ASSERT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));

  // CREATE_ALWAYS + SYSTEM -> 0x24.
  auto h3 = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_SYSTEM);
  ASSERT_WIN32_OK(h3);
  h3.value->reset();
  EXPECT_TRUE(CheckPolicyAttributes(
      path, DWORD{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_SYSTEM}));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));

  // CREATE_ALWAYS + HIDDEN -> 0x22.
  auto h4 = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_HIDDEN);
  ASSERT_WIN32_OK(h4);
  h4.value->reset();
  EXPECT_TRUE(CheckPolicyAttributes(
      path, DWORD{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN}));

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2 -- CREATE_ALWAYS fails ERROR_ACCESS_DENIED when an
// existing HIDDEN or SYSTEM attribute is omitted, but succeeds when matched.
TEST_F(CreateOpenTest, CreateAlwaysRejectsHiddenOrSystemMismatch) {
  struct AttrCase {
    const wchar_t *name;
    DWORD protected_attr;
  };
  constexpr AttrCase kCases[] = {
      {L"hidden_target.dat", FILE_ATTRIBUTE_HIDDEN},
      {L"system_target.dat", FILE_ATTRIBUTE_SYSTEM},
  };

  for (const auto &c : kCases) {
    SCOPED_TRACE(core::WideToUtf8(c.name));
    const std::wstring path = TestPath(c.name);
    auto created = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                         c.protected_attr);
    ASSERT_WIN32_OK(created);
    created.value->reset();

    auto mismatch =
        core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS, 0);
    EXPECT_FALSE(mismatch);
    EXPECT_WIN32_ERROR_CODE(mismatch.error, ERROR_ACCESS_DENIED);

    auto open_ok =
        core::CreateWin32File(path, GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    ASSERT_WIN32_OK(open_ok);
    open_ok.value->reset();

    auto match = core::CreateWin32FileEx(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                         c.protected_attr);
    ASSERT_WIN32_OK(match);
    EXPECT_WIN32_ERROR_CODE(match.last_error, ERROR_ALREADY_EXISTS);
    match.handle.reset();

    EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  }
}

// Spec: Win32 CreateDirectoryW / [MS-FSA] 2.1.5.1.1 -- CreateDirectoryW rejects
// existing files or directories with ERROR_ALREADY_EXISTS and sets DIRECTORY.
TEST_F(CreateOpenTest, CreateDirectoryCollisionsAndBasicAttributes) {
  const std::wstring file_path = TestPath(L"collide_file.dat");
  auto created_file = core::CreateWin32File(file_path, GENERIC_WRITE, 0,
                                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(created_file);
  created_file.value->reset();

  EXPECT_WIN32_ERROR(::CreateDirectoryW(file_path.c_str(), nullptr),
                     ERROR_ALREADY_EXISTS);
  ASSERT_WIN32_TRUE(::DeleteFileW(file_path.c_str()));

  const std::wstring dir_path = TestPath(L"basic_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_path.c_str(), nullptr));
  EXPECT_WIN32_ERROR(::CreateDirectoryW(dir_path.c_str(), nullptr),
                     ERROR_ALREADY_EXISTS);

  EXPECT_TRUE(CheckPolicyAttributes(dir_path, DWORD{FILE_ATTRIBUTE_DIRECTORY}));

  auto dir_h =
      core::CreateWin32File(dir_path, FILE_READ_ATTRIBUTES, core::kShareAll,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(dir_h);
  auto bhfi = core::GetByHandleInfo(dir_h.value->get());
  ASSERT_WIN32_OK(bhfi);
  EXPECT_EQ(MaskPolicyAttributes(bhfi.value->dwFileAttributes),
            DWORD{FILE_ATTRIBUTE_DIRECTORY});
  dir_h.value->reset();

  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir_path.c_str()));
}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1.2 -- OPEN_ALWAYS and OPEN_EXISTING
// preserve existing file data, whereas CREATE_ALWAYS truncates to 0 bytes.
TEST_F(CreateOpenTest, CreateAlwaysVsOpenDispositionsTruncation) {
  const std::wstring path = TestPath(L"disp_trunc.dat");
  const std::string payload(42, 'Z');
  auto write_res = core::WriteFileContents(path, payload);
  ASSERT_WIN32_OK(write_res);

  auto open_always = core::CreateWin32FileEx(
      path, GENERIC_WRITE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(open_always);
  EXPECT_WIN32_ERROR_CODE(open_always.last_error, ERROR_ALREADY_EXISTS);
  open_always.handle.reset();
  EXPECT_TRUE(CheckFileSize(path, 42));

  auto open_existing = core::CreateWin32FileEx(
      path, GENERIC_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(open_existing);
  open_existing.handle.reset();
  EXPECT_TRUE(CheckFileSize(path, 42));

  EXPECT_TRUE(helpers::CheckFileContents(path, payload));

  auto create_always = core::CreateWin32FileEx(
      path, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(create_always);
  EXPECT_WIN32_ERROR_CODE(create_always.last_error, ERROR_ALREADY_EXISTS);
  create_always.handle.reset();
  EXPECT_TRUE(CheckFileSize(path, 0));

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2.2 -- an open GENERIC_WRITE handle with
// FILE_SHARE_READ enforces symmetric share checks for read and delete opens.
TEST_F(CreateOpenTest, ShareModeWriteHandleConflicts) {
  const std::wstring path = TestPath(L"share_write.dat");

  auto h1 = core::CreateWin32File(path, GENERIC_WRITE, FILE_SHARE_READ,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h1);

  auto c1 = core::CreateWin32File(path, GENERIC_READ, FILE_SHARE_READ,
                                  OPEN_EXISTING, 0);
  EXPECT_FALSE(c1);
  EXPECT_WIN32_ERROR_CODE(c1.error, ERROR_SHARING_VIOLATION);

  auto ok_read = core::CreateWin32File(path, GENERIC_READ, FILE_SHARE_WRITE,
                                       OPEN_EXISTING, 0);
  ASSERT_WIN32_OK(ok_read);
  ok_read.value->reset();

  auto c2 =
      core::CreateWin32File(path, DELETE, FILE_SHARE_DELETE, OPEN_EXISTING, 0);
  EXPECT_FALSE(c2);
  EXPECT_WIN32_ERROR_CODE(c2.error, ERROR_SHARING_VIOLATION);

  auto c3 =
      core::CreateWin32File(path, DELETE, FILE_SHARE_WRITE, OPEN_EXISTING, 0);
  EXPECT_FALSE(c3);
  EXPECT_WIN32_ERROR_CODE(c3.error, ERROR_SHARING_VIOLATION);

  h1.value->reset();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2.2 -- an open GENERIC_READ handle with
// FILE_SHARE_WRITE allows only FILE_SHARE_READ writers and blocks DELETE opens.
TEST_F(CreateOpenTest, ShareModeReadHandleConflicts) {
  const std::wstring path = TestPath(L"share_read.dat");
  auto init = core::WriteFileContents(path, "seed");
  ASSERT_WIN32_OK(init);

  auto h1 = core::CreateWin32File(path, GENERIC_READ, FILE_SHARE_WRITE,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h1);

  auto c1 = core::CreateWin32File(path, GENERIC_WRITE, FILE_SHARE_WRITE,
                                  OPEN_EXISTING, 0);
  EXPECT_FALSE(c1);
  EXPECT_WIN32_ERROR_CODE(c1.error, ERROR_SHARING_VIOLATION);

  auto ok_write = core::CreateWin32File(path, GENERIC_WRITE, FILE_SHARE_READ,
                                        OPEN_EXISTING, 0);
  ASSERT_WIN32_OK(ok_write);
  ok_write.value->reset();

  auto c2 =
      core::CreateWin32File(path, DELETE, FILE_SHARE_DELETE, OPEN_EXISTING, 0);
  EXPECT_FALSE(c2);
  EXPECT_WIN32_ERROR_CODE(c2.error, ERROR_SHARING_VIOLATION);

  auto c3 =
      core::CreateWin32File(path, DELETE, FILE_SHARE_WRITE, OPEN_EXISTING, 0);
  EXPECT_FALSE(c3);
  EXPECT_WIN32_ERROR_CODE(c3.error, ERROR_SHARING_VIOLATION);

  h1.value->reset();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2.2 -- an open DELETE handle with FILE_SHARE_DELETE
// allows concurrent DELETE/FILE_SHARE_DELETE opens and rejects conflicts.
TEST_F(CreateOpenTest, ShareModeDeleteHandleConflicts) {
  const std::wstring path = TestPath(L"share_delete.dat");
  auto init = core::WriteFileContents(path, "seed");
  ASSERT_WIN32_OK(init);

  auto h1 = core::CreateWin32File(path, DELETE, FILE_SHARE_DELETE,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h1);

  auto c1 = core::CreateWin32File(path, GENERIC_WRITE, FILE_SHARE_DELETE,
                                  OPEN_EXISTING, 0);
  EXPECT_FALSE(c1);
  EXPECT_WIN32_ERROR_CODE(c1.error, ERROR_SHARING_VIOLATION);

  auto c2 =
      core::CreateWin32File(path, DELETE, FILE_SHARE_WRITE, OPEN_EXISTING, 0);
  EXPECT_FALSE(c2);
  EXPECT_WIN32_ERROR_CODE(c2.error, ERROR_SHARING_VIOLATION);

  auto ok_del =
      core::CreateWin32File(path, DELETE, FILE_SHARE_DELETE, OPEN_EXISTING, 0);
  ASSERT_WIN32_OK(ok_del);
  ok_del.value->reset();

  h1.value->reset();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2.2 -- opening a directory with share_mode=0 blocks
// a second conflicting directory open with ERROR_SHARING_VIOLATION.
TEST_F(CreateOpenTest, ShareModeDirectoryExclusiveOpen) {
  const std::wstring dir = TestPath(L"share_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h1 = core::CreateWin32File(dir, GENERIC_READ, 0, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(h1);

  auto h2 = core::CreateWin32File(dir, GENERIC_READ, 0, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS);
  EXPECT_FALSE(h2);
  EXPECT_WIN32_ERROR_CODE(h2.error, ERROR_SHARING_VIOLATION);

  h1.value->reset();
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: WDK NtCreateFile / [MS-FSA] 2.1.5.1 -- verifies all six NT create
// dispositions and exact IoStatus.Information values on absent/existing files.
TEST_F(CreateOpenTest, NtCreateFileAllSixDispositionsAndIoStatus) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;

  // FILE_CREATE: absent -> FILE_CREATED; existing -> NAME_COLLISION.
  {
    const std::wstring nt_path = TestNtPath(L"nt_disp_create.dat");
    auto r1 = core::NtCreateFilePath(nt_path, GENERIC_WRITE | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileCreate, kSyncFile);
    ASSERT_NT_STATUS(r1.status, STATUS_SUCCESS);
    EXPECT_EQ(r1.information, nt::kFileCreated);
    r1.value->reset();

    auto r2 = core::NtCreateFilePath(nt_path, GENERIC_WRITE | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileCreate, kSyncFile);
    EXPECT_NT_STATUS(r2.status, STATUS_OBJECT_NAME_COLLISION);
  }

  // FILE_OPEN: absent -> NAME_NOT_FOUND; existing -> FILE_OPENED.
  {
    const std::wstring path = TestPath(L"nt_disp_open.dat");
    const std::wstring nt_path = TestNtPath(L"nt_disp_open.dat");
    auto r1 = core::NtCreateFilePath(nt_path, GENERIC_READ | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileOpen, kSyncFile);
    EXPECT_NT_STATUS(r1.status, STATUS_OBJECT_NAME_NOT_FOUND);

    ASSERT_WIN32_OK(core::WriteFileContents(path, "open_data"));
    auto r2 = core::NtCreateFilePath(nt_path, GENERIC_READ | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileOpen, kSyncFile);
    ASSERT_NT_STATUS(r2.status, STATUS_SUCCESS);
    EXPECT_EQ(r2.information, nt::kFileOpened);
    r2.value->reset();
  }

  // FILE_OPEN_IF: absent -> FILE_CREATED; existing -> FILE_OPENED (keeps data).
  {
    const std::wstring path = TestPath(L"nt_disp_open_if.dat");
    const std::wstring nt_path = TestNtPath(L"nt_disp_open_if.dat");
    auto r1 = core::NtCreateFilePath(nt_path, GENERIC_WRITE | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileOpenIf, kSyncFile);
    ASSERT_NT_STATUS(r1.status, STATUS_SUCCESS);
    EXPECT_EQ(r1.information, nt::kFileCreated);
    ASSERT_WIN32_OK(core::WriteAllBytes(r1.value->get(), "keep_me", 7));
    r1.value->reset();

    auto r2 = core::NtCreateFilePath(
        nt_path, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
        FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOpenIf, kSyncFile);
    ASSERT_NT_STATUS(r2.status, STATUS_SUCCESS);
    EXPECT_EQ(r2.information, nt::kFileOpened);
    auto content = core::ReadAllBytes(r2.value->get());
    ASSERT_WIN32_OK(content);
    EXPECT_EQ(*content.value, "keep_me");
    r2.value->reset();
    EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  }

  // FILE_OVERWRITE: absent -> NAME_NOT_FOUND; existing -> FILE_OVERWRITTEN.
  {
    const std::wstring path = TestPath(L"nt_disp_overwrite.dat");
    const std::wstring nt_path = TestNtPath(L"nt_disp_overwrite.dat");
    auto r1 = core::NtCreateFilePath(nt_path, FILE_WRITE_DATA | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileOverwrite, kSyncFile);
    EXPECT_NT_STATUS(r1.status, STATUS_OBJECT_NAME_NOT_FOUND);

    ASSERT_WIN32_OK(core::WriteFileContents(path, "truncate_me"));
    auto r2 = core::NtCreateFilePath(nt_path, FILE_WRITE_DATA | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileOverwrite, kSyncFile);
    ASSERT_NT_STATUS(r2.status, STATUS_SUCCESS);
    EXPECT_EQ(r2.information, nt::kFileOverwritten);
    r2.value->reset();
    EXPECT_TRUE(CheckFileSize(path, 0));
  }

  // FILE_OVERWRITE_IF: absent -> FILE_CREATED; existing -> FILE_OVERWRITTEN.
  {
    const std::wstring path = TestPath(L"nt_disp_overwrite_if.dat");
    const std::wstring nt_path = TestNtPath(L"nt_disp_overwrite_if.dat");
    auto r1 = core::NtCreateFilePath(nt_path, FILE_WRITE_DATA | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileOverwriteIf, kSyncFile);
    ASSERT_NT_STATUS(r1.status, STATUS_SUCCESS);
    EXPECT_EQ(r1.information, nt::kFileCreated);
    ASSERT_WIN32_OK(core::WriteAllBytes(r1.value->get(), "old_bytes", 9));
    r1.value->reset();

    auto r2 = core::NtCreateFilePath(nt_path, FILE_WRITE_DATA | SYNCHRONIZE,
                                     FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                     nt::kFileOverwriteIf, kSyncFile);
    ASSERT_NT_STATUS(r2.status, STATUS_SUCCESS);
    EXPECT_EQ(r2.information, nt::kFileOverwritten);
    r2.value->reset();
    EXPECT_TRUE(CheckFileSize(path, 0));
  }

  // FILE_SUPERSEDE: absent -> FILE_CREATED; existing -> FILE_SUPERSEDED.
  {
    const std::wstring path = TestPath(L"nt_disp_supersede.dat");
    const std::wstring nt_path = TestNtPath(L"nt_disp_supersede.dat");
    auto r1 = core::NtCreateFilePath(
        nt_path, DELETE | FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
        core::kShareAll, nt::kFileSupersede, kSyncFile);
    ASSERT_NT_STATUS(r1.status, STATUS_SUCCESS);
    EXPECT_EQ(r1.information, nt::kFileCreated);
    ASSERT_WIN32_OK(core::WriteAllBytes(r1.value->get(), "supersede_me", 12));
    r1.value->reset();

    auto r2 = core::NtCreateFilePath(
        nt_path, DELETE | FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
        core::kShareAll, nt::kFileSupersede, kSyncFile);
    ASSERT_NT_STATUS(r2.status, STATUS_SUCCESS);
    EXPECT_EQ(r2.information, nt::kFileSuperseded);
    r2.value->reset();
    EXPECT_TRUE(CheckFileSize(path, 0));
  }
}

// Spec: WDK NtCreateFile / [MS-FSA] 2.1.5.1.1-2.1.5.1.2 -- supplying an
// initial AllocationSize on FILE_CREATE, FILE_OVERWRITE_IF, and FILE_SUPERSEDE
// preallocates >= requested bytes while leaving EndOfFile at 0.
TEST_F(CreateOpenTest, NtCreateFileInitialAllocationSize) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;
  constexpr int64_t kInitialAlloc = 65536;
  LARGE_INTEGER alloc_size{};
  alloc_size.QuadPart = kInitialAlloc;

  // FILE_CREATE with initial AllocationSize on an absent file.
  const std::wstring path = TestPath(L"nt_initial_alloc.dat");
  const std::wstring nt_path = TestNtPath(L"nt_initial_alloc.dat");
  {
    auto h = core::NtCreateFilePath(
        nt_path, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileCreate, kSyncFile,
        /*root_directory=*/nullptr, /*ea_buffer=*/nullptr, /*ea_length=*/0,
        &alloc_size);
    ASSERT_NT_STATUS(h.status, STATUS_SUCCESS);
    EXPECT_EQ(h.information, nt::kFileCreated);

    // Query while the handle is still open (NTFS trims excess preallocation on
    // cleanup).
    auto std_info = core::QueryStandardInfo(h.value->get());
    ASSERT_NT_STATUS(std_info.status, STATUS_SUCCESS);
    EXPECT_GE(std_info.value->allocation_size.QuadPart, kInitialAlloc);
    EXPECT_EQ(std_info.value->end_of_file.QuadPart, 0);
  }

  // FILE_OVERWRITE_IF with initial AllocationSize on an existing populated
  // file truncates EOF to 0 and preallocates >= 65536 bytes.
  ASSERT_WIN32_OK(core::WriteFileContents(path, "before_overwrite_if"));
  {
    auto h = core::NtCreateFilePath(
        nt_path, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOverwriteIf, kSyncFile,
        /*root_directory=*/nullptr, /*ea_buffer=*/nullptr, /*ea_length=*/0,
        &alloc_size);
    ASSERT_NT_STATUS(h.status, STATUS_SUCCESS);
    EXPECT_EQ(h.information, nt::kFileOverwritten);

    auto std_info = core::QueryStandardInfo(h.value->get());
    ASSERT_NT_STATUS(std_info.status, STATUS_SUCCESS);
    EXPECT_GE(std_info.value->allocation_size.QuadPart, kInitialAlloc);
    EXPECT_EQ(std_info.value->end_of_file.QuadPart, 0);
  }

  // FILE_SUPERSEDE with initial AllocationSize on an existing populated file
  // supersedes EOF to 0 and preallocates >= 65536 bytes.
  ASSERT_WIN32_OK(core::WriteFileContents(path, "before_supersede"));
  {
    auto h = core::NtCreateFilePath(
        nt_path, DELETE | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileSupersede, kSyncFile,
        /*root_directory=*/nullptr, /*ea_buffer=*/nullptr, /*ea_length=*/0,
        &alloc_size);
    ASSERT_NT_STATUS(h.status, STATUS_SUCCESS);
    EXPECT_EQ(h.information, nt::kFileSuperseded);

    auto std_info = core::QueryStandardInfo(h.value->get());
    ASSERT_NT_STATUS(std_info.status, STATUS_SUCCESS);
    EXPECT_GE(std_info.value->allocation_size.QuadPart, kInitialAlloc);
    EXPECT_EQ(std_info.value->end_of_file.QuadPart, 0);
  }
}

// Spec: [MS-FSA] 2.1.5.1.2 -- FILE_SUPERSEDE of an existing file is a
// delete-and-recreate, so it conflicts with an open that does not grant
// FILE_SHARE_DELETE (STATUS_SHARING_VIOLATION), whereas FILE_OVERWRITE_IF only
// needs FILE_SHARE_WRITE.
TEST_F(CreateOpenTest, NtSupersedeRequiresDeleteSharingUnlikeOverwriteIf) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;

  const std::wstring path = TestPath(L"sup_vs_ovr_share.dat");
  const std::wstring nt_path = TestNtPath(L"sup_vs_ovr_share.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "initial_payload"));

  auto h_reader = core::CreateWin32File(path, GENERIC_READ, FILE_SHARE_WRITE,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_reader);

  auto sup_res = core::NtCreateFilePath(
      nt_path, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nt::kFileSupersede, kSyncFile);
  EXPECT_NT_STATUS(sup_res.status, STATUS_SHARING_VIOLATION);
  if (sup_res) {
    sup_res.value->reset();
  }

  auto ovr_res = core::NtCreateFilePath(
      nt_path, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nt::kFileOverwriteIf, kSyncFile);
  ASSERT_NT_STATUS(ovr_res.status, STATUS_SUCCESS);
  EXPECT_EQ(ovr_res.information, nt::kFileOverwritten);
  ovr_res.value->reset();
  h_reader.value->reset();
}

// Spec: [MS-FSA] 2.1.5.1.2 -- FILE_OVERWRITE_IF and FILE_SUPERSEDE of an
// existing file truncate it to 0 bytes, set ARCHIVE, OR in newly requested
// non-protected attributes (FILE_ATTRIBUTE_OFFLINE) and report distinct
// IoStatus.Information values.
//
// The retention of FILE_ATTRIBUTE_TEMPORARY across FILE_SUPERSEDE asserted
// below is NTFS's observed behavior (the strict real-NTFS CI run is the
// authority for it), not something this test derives from [MS-FSA] prose.
TEST_F(CreateOpenTest, NtSupersedeAndOverwriteIfMergeAttributes) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;

  const std::wstring path = TestPath(L"sup_vs_ovr_attrs.dat");
  const std::wstring nt_path = TestNtPath(L"sup_vs_ovr_attrs.dat");

  auto init = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                    CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY);
  ASSERT_WIN32_OK(init);
  ASSERT_WIN32_OK(core::WriteAllBytes(init.value->get(), "temp_data", 9));
  init.value->reset();

  EXPECT_TRUE(CheckPolicyAttributes(
      path, DWORD{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY}));

  auto ovr = core::NtCreateFilePath(nt_path, FILE_WRITE_DATA | SYNCHRONIZE,
                                    FILE_ATTRIBUTE_NORMAL, core::kShareAll,
                                    nt::kFileOverwriteIf, kSyncFile);
  ASSERT_NT_STATUS(ovr.status, STATUS_SUCCESS);
  EXPECT_EQ(ovr.information, nt::kFileOverwritten);
  ovr.value->reset();

  EXPECT_TRUE(CheckFileSize(path, 0));
  EXPECT_TRUE(CheckPolicyAttributes(
      path, DWORD{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY}));

  ASSERT_WIN32_OK(
      core::WriteFileContents(path, "reseeded", FILE_ATTRIBUTE_NORMAL));

  auto sup = core::NtCreateFilePath(
      nt_path, DELETE | FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_OFFLINE,
      core::kShareAll, nt::kFileSupersede, kSyncFile);
  ASSERT_NT_STATUS(sup.status, STATUS_SUCCESS);
  EXPECT_EQ(sup.information, nt::kFileSuperseded);
  sup.value->reset();

  EXPECT_TRUE(CheckFileSize(path, 0));
  EXPECT_TRUE(CheckPolicyAttributes(path, DWORD{FILE_ATTRIBUTE_ARCHIVE |
                                                FILE_ATTRIBUTE_TEMPORARY |
                                                FILE_ATTRIBUTE_OFFLINE}));
}

// Spec: [MS-FSA] 2.1.5.1.2 -- when DELETE is denied in the DACL and the parent
// lacks FILE_DELETE_CHILD, FILE_SUPERSEDE fails STATUS_ACCESS_DENIED while
// FILE_OVERWRITE_IF succeeds with FILE_WRITE_DATA.
TEST_F(CreateOpenTest, NtSupersedeVsOverwriteDaclAccess) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;

  const std::wstring sub = TestPath(L"sup_acl_sub");
  const std::wstring target = TestPath(L"sup_acl_sub\\target.dat");
  const std::wstring nt_target = TestNtPath(L"sup_acl_sub\\target.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  auto sub_dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"sup_acl_sub"));
  ASSERT_TRUE(sub_dacl_guard.armed());
  ASSERT_WIN32_OK(core::WriteFileContents(target, "acl_payload"));
  auto target_dacl_guard = core::ScopedDaclRestore::Arm(nt_target);
  ASSERT_TRUE(target_dacl_guard.armed());

  // 0x001601BF grants all standard file/dir read+write+traverse+attr+WRITE_DAC
  // rights EXCEPT DELETE (0x10000) and FILE_DELETE_CHILD (0x40).
  ASSERT_WIN32_OK(core::SetFileSddl(sub, DACL_SECURITY_INFORMATION,
                                    L"D:P(A;;0x001601BF;;;WD)"));
  ASSERT_WIN32_OK(core::SetFileSddl(target, DACL_SECURITY_INFORMATION,
                                    L"D:P(D;;SD;;;WD)(A;;0x001601BF;;;WD)"));

  auto sup_denied = core::NtCreateFilePath(
      nt_target, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileSupersede, kSyncFile);
  EXPECT_NT_STATUS(sup_denied.status, STATUS_ACCESS_DENIED);
  if (sup_denied) {
    sup_denied.value->reset();
  }

  auto ovr_allowed = core::NtCreateFilePath(
      nt_target, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileOverwriteIf, kSyncFile);
  ASSERT_NT_STATUS(ovr_allowed.status, STATUS_SUCCESS);
  EXPECT_EQ(ovr_allowed.information, nt::kFileOverwritten);
  ovr_allowed.value->reset();

  auto reset_target =
      core::SetFileSddl(target, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)");
  EXPECT_WIN32_OK(reset_target);
  if (reset_target) {
    target_dacl_guard.DisarmAndClose();
  }
  auto reset_sub =
      core::SetFileSddl(sub, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)");
  EXPECT_WIN32_OK(reset_sub);
  if (reset_sub) {
    sub_dacl_guard.DisarmAndClose();
  }
}

// Spec: [MS-FSA] 2.1.5.1.2 -- FILE_OVERWRITE_IF and FILE_SUPERSEDE return
// STATUS_ACCESS_DENIED on HIDDEN/SYSTEM mismatch and succeed when matched.
TEST_F(CreateOpenTest, NtSupersedeAndOverwriteRejectHiddenOrSystemMismatch) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;

  for (const DWORD protected_attr :
       {DWORD{FILE_ATTRIBUTE_HIDDEN}, DWORD{FILE_ATTRIBUTE_SYSTEM}}) {
    const std::wstring rel = (protected_attr == FILE_ATTRIBUTE_HIDDEN)
                                 ? L"sup_hidden.dat"
                                 : L"sup_system.dat";
    const std::wstring path = TestPath(rel);
    const std::wstring nt_path = TestNtPath(rel);

    ASSERT_WIN32_OK(core::WriteFileContents(path, "protected", protected_attr));

    auto ovr_bad = core::NtCreateFilePath(
        nt_path, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
        core::kShareAll, nt::kFileOverwriteIf, kSyncFile);
    EXPECT_NT_STATUS(ovr_bad.status, STATUS_ACCESS_DENIED);
    if (ovr_bad) {
      ovr_bad.value->reset();
    }

    auto sup_bad = core::NtCreateFilePath(
        nt_path, DELETE | FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
        core::kShareAll, nt::kFileSupersede, kSyncFile);
    EXPECT_NT_STATUS(sup_bad.status, STATUS_ACCESS_DENIED);
    if (sup_bad) {
      sup_bad.value->reset();
    }

    EXPECT_TRUE(helpers::CheckFileContents(path, "protected"));

    auto ovr_ok = core::NtCreateFilePath(nt_path, FILE_WRITE_DATA | SYNCHRONIZE,
                                         protected_attr, core::kShareAll,
                                         nt::kFileOverwriteIf, kSyncFile);
    ASSERT_NT_STATUS(ovr_ok.status, STATUS_SUCCESS);
    EXPECT_EQ(ovr_ok.information, nt::kFileOverwritten);
    ASSERT_WIN32_OK(core::WriteAllBytes(ovr_ok.value->get(), "reseed", 6));
    ovr_ok.value->reset();

    auto sup_ok = core::NtCreateFilePath(
        nt_path, DELETE | FILE_WRITE_DATA | SYNCHRONIZE, protected_attr,
        core::kShareAll, nt::kFileSupersede, kSyncFile);
    ASSERT_NT_STATUS(sup_ok.status, STATUS_SUCCESS);
    EXPECT_EQ(sup_ok.information, nt::kFileSuperseded);
    sup_ok.value->reset();

    EXPECT_TRUE(CheckFileSize(path, 0));

    auto attrs = core::QueryWin32Attributes(path);
    ASSERT_WIN32_OK(attrs);
    EXPECT_NE(*attrs.value & protected_attr, 0UL);
  }
}

// Spec: Win32 CreateFileW / [MS-FSA] 2.1.5.1 -- file-creating dispositions
// aimed at an existing directory never replace it: with FILE_NON_DIRECTORY_FILE
// (including Win32 CreateFileW without FILE_FLAG_BACKUP_SEMANTICS), CREATE_NEW,
// CREATE_ALWAYS, FILE_CREATE, and FILE_OVERWRITE_IF all fail because the target
// is a directory (ERROR_ACCESS_DENIED / STATUS_FILE_IS_A_DIRECTORY); without
// FILE_NON_DIRECTORY_FILE, FILE_CREATE collides (STATUS_OBJECT_NAME_COLLISION).
TEST_F(CreateOpenTest, CreateNewAndCreateAlwaysOnExistingDirectoryFail) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;

  const std::wstring dir = TestPath(L"dir_collision");
  const std::wstring nt_dir = TestNtPath(L"dir_collision");
  const std::wstring child = TestPath(L"dir_collision\\keep.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(child, "keep"));

  auto create_new = core::CreateWin32FileEx(dir, GENERIC_WRITE, core::kShareAll,
                                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(create_new.handle.valid());
  EXPECT_WIN32_ERROR_CODE(create_new.last_error, ERROR_ACCESS_DENIED);
  create_new.handle.reset();

  auto create_always =
      core::CreateWin32FileEx(dir, GENERIC_WRITE, core::kShareAll,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(create_always.handle.valid());
  EXPECT_WIN32_ERROR_CODE(create_always.last_error, ERROR_ACCESS_DENIED);
  create_always.handle.reset();

  auto nt_create_nondir = core::NtCreateFilePath(
      nt_dir, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileCreate, kSyncFile);
  EXPECT_NT_STATUS(nt_create_nondir.status, STATUS_FILE_IS_A_DIRECTORY);
  if (nt_create_nondir) {
    nt_create_nondir.value->reset();
  }

  auto nt_create_any = core::NtCreateFilePath(
      nt_dir, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileCreate, nt::kFileSynchronousIoNonalert);
  EXPECT_NT_STATUS(nt_create_any.status, STATUS_OBJECT_NAME_COLLISION);
  if (nt_create_any) {
    nt_create_any.value->reset();
  }

  auto nt_overwrite_if = core::NtCreateFilePath(
      nt_dir, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileOverwriteIf, kSyncFile);
  EXPECT_NT_STATUS(nt_overwrite_if.status, STATUS_FILE_IS_A_DIRECTORY);
  if (nt_overwrite_if) {
    nt_overwrite_if.value->reset();
  }

  // The directory and its child are untouched.
  EXPECT_TRUE(CheckPolicyAttributes(dir, DWORD{FILE_ATTRIBUTE_DIRECTORY}));
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L"..", L"keep.dat"}));
  EXPECT_TRUE(helpers::CheckFileContents(child, "keep"));
}

// Spec: [MS-FSA] 2.1.5.1 -- a path with a trailing backslash can only name a
// directory: opening an existing file, or creating a new non-directory file,
// through such a name fails and creates nothing, while an existing directory
// opens normally.
//
// OS-build threshold provenance: Windows Server 2022 (build 20348) returns
// STATUS_OBJECT_NAME_INVALID (Win32 ERROR_INVALID_NAME), while Windows Server
// 2025 / Windows 11 24H2 (build 26100+) returns STATUS_NOT_A_DIRECTORY (Win32
// ERROR_DIRECTORY). Client builds between them (22000-22631) have not been
// measured; kBuildWin11_24H2 is the lowest build on which the new status is
// confirmed.
TEST_F(CreateOpenTest, TrailingBackslashOnFileFailsNotADirectory) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;
  const DWORD expected_win_err = (Os().build >= core::kBuildWin11_24H2)
                                     ? static_cast<DWORD>(ERROR_DIRECTORY)
                                     : static_cast<DWORD>(ERROR_INVALID_NAME);
  const NTSTATUS expected_nt_status = (Os().build >= core::kBuildWin11_24H2)
                                          ? STATUS_NOT_A_DIRECTORY
                                          : STATUS_OBJECT_NAME_INVALID;

  const std::wstring file = TestPath(L"trailing_slash.dat");
  const std::wstring file_slash = file + L"\\";
  const std::wstring nt_file_slash = TestNtPath(L"trailing_slash.dat") + L"\\";
  ASSERT_WIN32_OK(core::WriteFileContents(file, "slash_payload"));

  // Existing file.
  auto win_open =
      core::CreateWin32File(file_slash, GENERIC_READ, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(win_open);
  EXPECT_WIN32_ERROR_CODE(win_open.error, expected_win_err);

  auto nt_open = core::NtCreateFilePath(
      nt_file_slash, FILE_READ_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileOpen, kSyncFile);
  EXPECT_NT_STATUS(nt_open.status, expected_nt_status);
  if (nt_open) {
    nt_open.value->reset();
  }
  EXPECT_TRUE(helpers::CheckFileContents(file, "slash_payload"));

  // New file: nothing may be created under either spelling.
  const std::wstring absent = TestPath(L"trailing_slash_new.dat");
  const std::wstring nt_absent_slash =
      TestNtPath(L"trailing_slash_new.dat") + L"\\";
  auto nt_create = core::NtCreateFilePath(
      nt_absent_slash, FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileCreate, kSyncFile);
  EXPECT_NT_STATUS(nt_create.status, expected_nt_status);
  if (nt_create) {
    nt_create.value->reset();
  }
  EXPECT_TRUE(helpers::CheckFileNotFound(absent));

  // Positive control: a directory is reachable through a trailing backslash.
  const std::wstring dir = TestPath(L"trailing_slash_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  auto nt_dir = core::NtCreateFilePath(
      TestNtPath(L"trailing_slash_dir") + L"\\",
      FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL, core::kShareAll,
      nt::kFileOpen, nt::kFileDirectoryFile | nt::kFileSynchronousIoNonalert);
  EXPECT_NT_STATUS(nt_dir.status, STATUS_SUCCESS);
  if (nt_dir) {
    nt_dir.value->reset();
  }
}

// Spec: [MS-FSA] 2.1.5.1.2 -- truncating create dispositions (CREATE_ALWAYS,
// TRUNCATE_EXISTING, FILE_SUPERSEDE) fail STATUS_USER_MAPPED_FILE
// (ERROR_USER_MAPPED_FILE) while the file backs an active user-mapped section,
// leave its contents intact, and succeed once the view is unmapped.
TEST_F(CreateOpenTest, OverwriteAndSupersedeOfUserMappedFileFail) {
  constexpr ULONG kSyncFile =
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert;
  const std::string payload(8192, 'M');

  const std::wstring path = TestPath(L"mapped_overwrite.dat");
  const std::wstring nt_path = TestNtPath(L"mapped_overwrite.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, payload));

  // Declaration order: the view unmaps before the section handle closes.
  core::UniqueHandle mapping;
  core::UniqueViewOfFile view;
  {
    auto h_map = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                       core::kShareAll, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_map);
    ::SetLastError(ERROR_SUCCESS);
    mapping.reset(::CreateFileMappingW(h_map.value->get(), nullptr,
                                       PAGE_READWRITE, 0, 0, nullptr));
    const DWORD map_err = ::GetLastError();
    ASSERT_TRUE(mapping.valid()) << core::FormatWin32Error(map_err);
    ::SetLastError(ERROR_SUCCESS);
    view.reset(::MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0,
                               0, 0));
    const DWORD view_err = ::GetLastError();
    ASSERT_TRUE(view) << core::FormatWin32Error(view_err);
  }

  // The view is intentionally never dereferenced after this point: if a
  // filesystem wrongly truncates the file, touching it would fault.
  auto create_always =
      core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(create_always);
  EXPECT_WIN32_ERROR_CODE(create_always.error, ERROR_USER_MAPPED_FILE);
  if (create_always) {
    create_always.value->reset();
  }

  auto truncate =
      core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                            TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(truncate);
  EXPECT_WIN32_ERROR_CODE(truncate.error, ERROR_USER_MAPPED_FILE);
  if (truncate) {
    truncate.value->reset();
  }

  auto supersede = core::NtCreateFilePath(
      nt_path, DELETE | FILE_WRITE_DATA | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileSupersede, kSyncFile);
  EXPECT_NT_STATUS(supersede.status, STATUS_USER_MAPPED_FILE);
  if (supersede) {
    supersede.value->reset();
  }

  view.reset();
  mapping.reset();

  EXPECT_TRUE(helpers::CheckFileContents(path, payload));

  auto after_unmap =
      core::CreateWin32FileEx(path, GENERIC_WRITE, core::kShareAll,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(after_unmap);
  after_unmap.handle.reset();
  EXPECT_TRUE(CheckFileSize(path, 0));
}

} // namespace fstest
