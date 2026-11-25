// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/os_info.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace fstest {
namespace {

// FILE_SUPPORTS_POSIX_UNLINK_RENAME (FileFsAttributeInformation /
// GetVolumeInformationW): the filesystem implements POSIX unlink/rename
// (FileDispositionInformationEx / FileRenameInformationEx POSIX_SEMANTICS).
constexpr DWORD kFileSupportsPosixUnlinkRename = 0x00000400UL;

NTSTATUS CallNtDeleteFile(const std::wstring &nt_path) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtDeleteFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  std::wstring path_buf(nt_path);
  nt::UnicodeString us =
      nt::MakeUnicodeString(path_buf.data(), path_buf.size());
  nt::ObjectAttributes oa = nt::MakeObjectAttributes(&us);
  return ntdll.NtDeleteFile(&oa);
}

core::Win32Result<core::UniqueHandle>
OpenForDelete(std::wstring_view path, DWORD access = DELETE, DWORD flags = 0) {
  return core::CreateWin32File(path, access, core::kShareAll, OPEN_EXISTING,
                               flags);
}

NTSTATUS TryNtOpenExisting(std::wstring_view nt_path,
                           ULONG create_options = nt::kFileNonDirectoryFile) {
  auto res =
      core::NtCreateFilePath(nt_path, FILE_READ_DATA | SYNCHRONIZE, 0,
                             core::kShareAll, nt::kFileOpen, create_options);
  return res.status;
}

::testing::AssertionResult CheckDeletePending(HANDLE h, bool expected_pending) {
  auto std_info = core::QueryStandardInfo(h);
  if (!std_info) {
    return ::testing::AssertionFailure()
           << "QueryStandardInfo failed: "
           << core::FormatNtStatus(std_info.status);
  }
  const bool actual = (std_info.value->delete_pending != FALSE);
  if (actual != expected_pending) {
    return ::testing::AssertionFailure()
           << "Expected delete_pending="
           << (expected_pending ? "true" : "false") << ", got "
           << (actual ? "true" : "false");
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckIsDirectory(std::wstring_view path) {
  auto attrs = core::QueryWin32Attributes(path);
  if (!attrs) {
    return ::testing::AssertionFailure()
           << "Expected directory '" << core::WideToUtf8(path)
           << "' to exist: " << core::FormatWin32Error(attrs.error);
  }
  if ((*attrs.value & FILE_ATTRIBUTE_DIRECTORY) == 0UL) {
    return ::testing::AssertionFailure()
           << "Expected FILE_ATTRIBUTE_DIRECTORY on '" << core::WideToUtf8(path)
           << "'";
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult
CheckDeletePendingRejectsOpens(std::wstring_view path,
                               std::wstring_view nt_path) {
  const NTSTATUS nt_st = TryNtOpenExisting(nt_path);
  if (nt_st != STATUS_DELETE_PENDING) {
    return ::testing::AssertionFailure()
           << "Expected NtCreateFile('" << core::WideToUtf8(nt_path)
           << "') == STATUS_DELETE_PENDING, got "
           << core::FormatNtStatus(nt_st);
  }
  auto win32_open = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                          OPEN_EXISTING, 0);
  if (win32_open) {
    return ::testing::AssertionFailure()
           << "Expected CreateFileW('" << core::WideToUtf8(path)
           << "') to fail ERROR_ACCESS_DENIED while delete-pending, but it "
              "succeeded";
  }
  if (win32_open.error != static_cast<DWORD>(ERROR_ACCESS_DENIED)) {
    return ::testing::AssertionFailure()
           << "Expected CreateFileW('" << core::WideToUtf8(path)
           << "') to fail ERROR_ACCESS_DENIED, got "
           << core::FormatWin32Error(win32_open.error);
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult
CheckPosixUnlinkedReuse(HANDLE orig_handle, uint64_t orig_index,
                        std::wstring_view dir, std::wstring_view file,
                        std::string_view orig_payload,
                        std::string_view new_payload) {
  auto missing = core::CreateWin32File(file, GENERIC_READ, core::kShareAll,
                                       OPEN_EXISTING, 0);
  if (missing || missing.error != static_cast<DWORD>(ERROR_FILE_NOT_FOUND)) {
    return ::testing::AssertionFailure()
           << "Expected unlinked '" << core::WideToUtf8(file)
           << "' to fail ERROR_FILE_NOT_FOUND, got "
           << (missing ? "success" : core::FormatWin32Error(missing.error));
  }
  if (auto dir_res = helpers::CheckDirEntries(dir, {L".", L".."}); !dir_res) {
    return dir_res;
  }

  auto h_new =
      core::CreateWin32File(file, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  if (!h_new) {
    return ::testing::AssertionFailure()
           << "Failed to recreate '" << core::WideToUtf8(file)
           << "': " << core::FormatWin32Error(h_new.error);
  }
  if (auto wr = core::WriteAllBytes(h_new.value->get(), new_payload.data(),
                                    new_payload.size());
      !wr) {
    return ::testing::AssertionFailure() << "Failed writing recreated file: "
                                         << core::FormatWin32Error(wr.error);
  }
  auto new_index = core::GetFileIndex(h_new.value->get());
  if (!new_index) {
    return ::testing::AssertionFailure()
           << "GetFileIndex failed on recreated file: "
           << core::FormatWin32Error(new_index.error);
  }
  if (*new_index.value == orig_index) {
    return ::testing::AssertionFailure()
           << "Expected recreated file to have a distinct FileIndex from "
           << orig_index;
  }
  h_new.value->reset();

  ::SetLastError(ERROR_SUCCESS);
  if (!fstest::SeekBegin(orig_handle, 0)) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "SeekBegin on original handle failed: "
           << core::FormatWin32Error(err);
  }
  auto orig_data = core::ReadAllBytes(orig_handle);
  if (!orig_data) {
    return ::testing::AssertionFailure()
           << "ReadAllBytes on original handle failed: "
           << core::FormatWin32Error(orig_data.error);
  }
  if (*orig_data.value != orig_payload) {
    return ::testing::AssertionFailure()
           << "Original handle content mismatch: actual='" << *orig_data.value
           << "', expected='" << orig_payload << "'";
  }
  return ::testing::AssertionSuccess();
}

// Holds a file mapping and mapped view; declaration order guarantees `view`
// unmaps before `mapping` closes on teardown.
struct MappedFileGuard {
  core::UniqueHandle mapping;
  core::UniqueViewOfFile view;
};

::testing::AssertionResult CreateMappedView(std::wstring_view path,
                                            DWORD file_access, DWORD protect,
                                            DWORD view_access,
                                            MappedFileGuard *out) {
  auto h_file = core::CreateWin32File(path, file_access, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  if (!h_file) {
    return ::testing::AssertionFailure()
           << "Failed to open '" << core::WideToUtf8(path)
           << "' for mapping: " << core::FormatWin32Error(h_file.error);
  }
  ::SetLastError(ERROR_SUCCESS);
  out->mapping.reset(::CreateFileMappingW(h_file.value->get(), nullptr, protect,
                                          0, 0, nullptr));
  const DWORD map_err = ::GetLastError();
  if (!out->mapping.valid()) {
    return ::testing::AssertionFailure()
           << "CreateFileMappingW failed: " << core::FormatWin32Error(map_err);
  }
  ::SetLastError(ERROR_SUCCESS);
  out->view.reset(::MapViewOfFile(out->mapping.get(), view_access, 0, 0, 0));
  const DWORD view_err = ::GetLastError();
  if (!out->view) {
    out->mapping.reset();
    return ::testing::AssertionFailure()
           << "MapViewOfFile failed: " << core::FormatWin32Error(view_err);
  }
  // Close the file handle so only the active mapped view pins the file.
  h_file.value->reset();
  return ::testing::AssertionSuccess();
}

} // namespace

void ForceLink_Delete() {}

// Spec: [MS-FSA] 2.1.5.1.2 & 2.1.5.15.3 / Win32 DeleteFileW --
// FILE_ATTRIBUTE_READONLY blocks DeleteFileW and FILE_FLAG_DELETE_ON_CLOSE
// (STATUS_CANNOT_DELETE -> ERROR_ACCESS_DENIED). The delete-on-close open asks
// for DELETE access only so the rejection can only come from the delete path
// (a GENERIC_WRITE open of a READONLY file is denied regardless).
TEST_F(DeleteTest, ReadOnlyFileCannotBeDeleted) {
  const std::wstring path = TestPath(L"readonly_del.dat");
  auto h = core::CreateWin32File(path, GENERIC_WRITE, 0, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_READONLY);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  h.value->reset();

  auto doc_delete = core::CreateWin32File(
      path, DELETE, core::kShareAll, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE);
  EXPECT_FALSE(doc_delete);
  EXPECT_WIN32_ERROR_CODE(doc_delete.error, ERROR_ACCESS_DENIED);

  EXPECT_WIN32_ERROR(::DeleteFileW(path.c_str()), ERROR_ACCESS_DENIED);

  auto still_there = core::QueryWin32Attributes(path);
  ASSERT_TRUE(still_there) << core::FormatWin32Error(still_there.error);
  EXPECT_NE(*still_there.value & FILE_ATTRIBUTE_READONLY, 0UL);

  ASSERT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  EXPECT_WIN32_ERROR(::DeleteFileW(path.c_str()), ERROR_FILE_NOT_FOUND);
}

// Spec: Win32 DeleteFileW & RemoveDirectoryW -- RemoveDirectoryW rejects files
// (ERROR_DIRECTORY) and DeleteFileW rejects directories (ERROR_ACCESS_DENIED).
TEST_F(DeleteTest, DirectoryDeleteFileFailsAndRemoveDirectorySucceeds) {
  const std::wstring file = TestPath(L"regular_not_dir.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(file, "file"));
  EXPECT_WIN32_ERROR(::RemoveDirectoryW(file.c_str()), ERROR_DIRECTORY);
  EXPECT_WIN32_TRUE(::DeleteFileW(file.c_str()));

  const std::wstring dir = TestPath(L"empty_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  EXPECT_WIN32_ERROR(::DeleteFileW(dir.c_str()), ERROR_ACCESS_DENIED);
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir.c_str()), ERROR_FILE_NOT_FOUND);
}

// Spec: [MS-FSA] 2.1.5.15.3 / Win32 RemoveDirectoryW -- removing a directory
// with child files or subdirectories fails ERROR_DIR_NOT_EMPTY until emptied.
TEST_F(DeleteTest, NonEmptyDirectoryCannotBeRemoved) {
  const std::wstring dir = TestPath(L"nonempty_dir");
  const std::wstring inner_file = TestPath(L"nonempty_dir\\inner.dat");
  const std::wstring inner_sub = TestPath(L"nonempty_dir\\inner_sub");

  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(inner_file, "child"));

  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir.c_str()), ERROR_DIR_NOT_EMPTY);

  ASSERT_WIN32_TRUE(::CreateDirectoryW(inner_sub.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::DeleteFileW(inner_file.c_str()));
  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir.c_str()), ERROR_DIR_NOT_EMPTY);

  ASSERT_WIN32_TRUE(::RemoveDirectoryW(inner_sub.c_str()));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.3 / Win32 RemoveDirectoryW -- a READONLY directory
// fails RemoveDirectoryW with ERROR_ACCESS_DENIED until READONLY is cleared.
TEST_F(DeleteTest, ReadOnlyDirectoryCannotBeRemoved) {
  const std::wstring dir = TestPath(L"readonly_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_READONLY));

  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir.c_str()), ERROR_ACCESS_DENIED);

  ASSERT_WIN32_TRUE(::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.3 / [MS-FSCC] 2.4.11 & 2.4.47 -- FileDispositionInfo
// sets DeletePending and rejects new opens with STATUS_DELETE_PENDING.
TEST_F(DeleteTest, DispositionInfoLegacyDeletePendingBlocksOpen) {
  const std::wstring path = TestPath(L"legacy_pending.dat");
  const std::wstring nt_path = TestNtPath(L"legacy_pending.dat");

  auto h1 =
      core::CreateWin32File(path, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  // Before marking delete, concurrent shared opens succeed.
  auto h_read = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, 0);
  ASSERT_TRUE(h_read) << core::FormatWin32Error(h_read.error);
  h_read.value->reset();

  // Explicitly use legacy FileDispositionInformation (class 13) so behavior is
  // deterministic across all Windows builds.
  {
    auto h_del = OpenForDelete(path);
    ASSERT_TRUE(h_del) << core::FormatWin32Error(h_del.error);
    auto disp = core::SetDisposition(h_del.value->get(), /*delete_file=*/true);
    ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);
  }

  EXPECT_TRUE(CheckDeletePending(h1.value->get(), true));
  EXPECT_TRUE(CheckDeletePendingRejectsOpens(path, nt_path));

  h1.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(path));
}

// Spec: [MS-FSA] 2.1.5.15.3 & 2.1.5.5 -- legacy delete-pending keeps the entry
// visible in directory listings and parent non-empty until the last close.
TEST_F(DeleteTest, DispositionInfoLegacyDeletePendingDirVisibility) {
  const std::wstring dir = TestPath(L"legacy_vis_dir");
  const std::wstring file = TestPath(L"legacy_vis_dir\\foo.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h1 = core::CreateWin32File(file, GENERIC_WRITE, core::kShareAll,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  {
    auto h_del = OpenForDelete(file);
    ASSERT_TRUE(h_del) << core::FormatWin32Error(h_del.error);
    auto disp = core::SetDisposition(h_del.value->get(), /*delete_file=*/true);
    ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);
  }

  // Legacy FileDispositionInformation keeps the entry visible until last close.
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L"..", L"foo.dat"}));
  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir.c_str()), ERROR_DIR_NOT_EMPTY);

  h1.value->reset();

  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L".."}));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: Win32 DeleteFileW / [MS-FSA] 2.1.5.15.3 & 2.1.5.5 -- semantics shared
// by legacy delete-pending and POSIX unlink: DeleteFileW succeeds while another
// FILE_SHARE_DELETE handle is open, that handle keeps full access to the data,
// and the file is gone once the last handle closes.
TEST_F(DeleteTest, DeleteFileWhileOpenKeepsHandleUsableUntilLastClose) {
  const std::wstring dir = TestPath(L"win32_open_del");
  const std::wstring file = TestPath(L"win32_open_del\\item.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h1 =
      core::CreateWin32File(file, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);
  ASSERT_WIN32_OK(core::WriteAllBytes(h1.value->get(), "still_readable", 14));

  ASSERT_WIN32_TRUE(::DeleteFileW(file.c_str()));

  // The surviving handle still reads the original bytes.
  ASSERT_WIN32_TRUE(fstest::SeekBegin(h1.value->get(), 0));
  auto data = core::ReadAllBytes(h1.value->get());
  ASSERT_TRUE(data) << core::FormatWin32Error(data.error);
  EXPECT_EQ(*data.value, "still_readable");

  h1.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(file));
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L".."}));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: Win32 DeleteFileW / [MS-FSA] 2.1.5.15.3 -- without POSIX unlink,
// deleting an open file enters delete-pending: new opens fail
// STATUS_DELETE_PENDING and the name stays listed until the last close.
//
// kernel32 DeleteFileW uses POSIX unlink only on Windows 10 1903+ *and* only
// when the filesystem implements it; it falls back to legacy
// FileDispositionInformation otherwise. The legacy contract therefore applies
// on pre-1903 hosts and on any filesystem that does not advertise
// FILE_SUPPORTS_POSIX_UNLINK_RENAME (so it stays enforced on modern hosts for
// such filesystems instead of being skipped by an OS-build gate).
TEST_F(DeleteTest, DeleteFileWhileOpenLegacy) {
  if (Os().build >= core::kBuildWin10_1903 &&
      (Caps().fs_flags & kFileSupportsPosixUnlinkRename) != 0UL) {
    RecordSkipCategory("fs_capability");
    GTEST_SKIP() << "Filesystem advertises FILE_SUPPORTS_POSIX_UNLINK_RENAME "
                    "on Windows 10 1903+ (host build: "
                 << Os().build
                 << "); DeleteFileW uses POSIX unlink (see "
                    "DeleteFileWhileOpenPosix).";
  }

  const std::wstring dir = TestPath(L"win32_legacy_del");
  const std::wstring file = TestPath(L"win32_legacy_del\\item.dat");
  const std::wstring nt_file = TestNtPath(L"win32_legacy_del\\item.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h1 =
      core::CreateWin32File(file, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  ASSERT_WIN32_TRUE(::DeleteFileW(file.c_str()));

  EXPECT_TRUE(CheckDeletePending(h1.value->get(), true));
  EXPECT_TRUE(CheckDeletePendingRejectsOpens(file, nt_file));
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L"..", L"item.dat"}));
  EXPECT_WIN32_ERROR(::RemoveDirectoryW(dir.c_str()), ERROR_DIR_NOT_EMPTY);
  h1.value->reset();
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L".."}));

  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: Win32 DeleteFileW / [MS-FSA] 2.1.5.15.4 (Win10 1903+) -- deleting an
// open file performs POSIX unlink so the name can be immediately reused.
TEST_F(DeleteTest, DeleteFileWhileOpenPosix) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1903);
  REQUIRE_FS_CAPABILITY(FsCapability::kPosixUnlinkRename);

  const std::wstring dir = TestPath(L"win32_posix_del");
  const std::wstring file = TestPath(L"win32_posix_del\\item.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h1 =
      core::CreateWin32File(file, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);
  ASSERT_WIN32_OK(core::WriteAllBytes(h1.value->get(), "original_bytes", 14));

  auto orig_index = core::GetFileIndex(h1.value->get());
  ASSERT_TRUE(orig_index) << core::FormatWin32Error(orig_index.error);

  ASSERT_WIN32_TRUE(::DeleteFileW(file.c_str()));

  // On Windows 10 1903+, DeleteFileW issues POSIX unlink so the name vanishes
  // immediately while h1 stays valid.
  ASSERT_TRUE(CheckPosixUnlinkedReuse(h1.value->get(), *orig_index.value, dir,
                                      file, "original_bytes",
                                      "new_generation"));

  h1.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(file, "new_generation"));
}

// Spec: [MS-FSA] 2.1.5.15.3 / [MS-FSCC] 2.4.11 -- clearing DeleteFile=FALSE
// before the last handle closes cancels delete-pending and preserves the file.
TEST_F(DeleteTest, DispositionInfoCancelBeforeLastClose) {
  const std::wstring path = TestPath(L"cancel_delete.dat");
  const std::wstring nt_path = TestNtPath(L"cancel_delete.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "survive_cancel"));

  auto h1 = core::CreateWin32File(path, GENERIC_READ | DELETE, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  auto mark = core::SetDisposition(h1.value->get(), /*delete_file=*/true);
  ASSERT_NT_STATUS(mark.status, STATUS_SUCCESS);

  EXPECT_TRUE(CheckDeletePending(h1.value->get(), true));
  EXPECT_NT_STATUS(TryNtOpenExisting(nt_path), STATUS_DELETE_PENDING);

  auto unmark = core::SetDisposition(h1.value->get(), /*delete_file=*/false);
  ASSERT_NT_STATUS(unmark.status, STATUS_SUCCESS);

  EXPECT_TRUE(CheckDeletePending(h1.value->get(), false));

  h1.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(path, "survive_cancel"));
}

// Spec: [MS-FSA] 2.1.5.5 -- closing a FILE_FLAG_DELETE_ON_CLOSE handle marks
// delete-pending (uncancelable by FileDispositionInfo) while others stay open.
TEST_F(DeleteTest, DeleteOnCloseWhileOtherHandleOpen) {
  const std::wstring path = TestPath(L"doc_other.dat");
  const std::wstring nt_path = TestNtPath(L"doc_other.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "doc_content"));

  auto h1 = core::CreateWin32File(path, GENERIC_READ | DELETE, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  auto h2 = OpenForDelete(path, DELETE, FILE_FLAG_DELETE_ON_CLOSE);
  ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);

  // Per [MS-FSA] 2.1.5.15.3, FileDispositionInformation({FALSE}) succeeds
  // (STATUS_SUCCESS) by clearing the link disposition flag, yet cannot cancel
  // FILE_DELETE_ON_CLOSE once stamped on handle h2.
  EXPECT_NT_STATUS(
      core::SetDisposition(h2.value->get(), /*delete_file=*/false).status,
      STATUS_SUCCESS);
  EXPECT_NT_STATUS(
      core::SetDisposition(h1.value->get(), /*delete_file=*/false).status,
      STATUS_SUCCESS);

  // Closing h2 triggers delete-pending state while h1 remains open.
  h2.value->reset();

  EXPECT_NT_STATUS(TryNtOpenExisting(nt_path), STATUS_DELETE_PENDING);
  EXPECT_TRUE(CheckDeletePending(h1.value->get(), true));

  auto read_h1 = core::ReadAllBytes(h1.value->get());
  ASSERT_TRUE(read_h1) << core::FormatWin32Error(read_h1.error);
  EXPECT_EQ(*read_h1.value, "doc_content");

  h1.value->reset();
  EXPECT_TRUE(helpers::CheckFileNotFound(path));
}

// Spec: [MS-FSA] 2.1.5.5 -- FILE_FLAG_DELETE_ON_CLOSE on an empty directory
// (opened with FILE_FLAG_BACKUP_SEMANTICS) removes it when the handle closes.
TEST_F(DeleteTest, DeleteOnCloseEmptyDirectoryRemovedOnClose) {
  const std::wstring dir = TestPath(L"doc_empty_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h = OpenForDelete(
      dir, DELETE, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_DELETE_ON_CLOSE);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

  // Still present while the delete-on-close handle is open.
  EXPECT_TRUE(CheckIsDirectory(dir));

  h.value->reset();
  EXPECT_TRUE(helpers::CheckFileNotFound(dir));
}

// Spec: [MS-FSA] 2.1.5.5 -- FILE_FLAG_DELETE_ON_CLOSE on a non-empty directory:
// the open succeeds, cleanup silently declines to delete, and both the
// directory and its child remain intact after close.
TEST_F(DeleteTest, DeleteOnCloseNonEmptyDirectorySurvivesClose) {
  const std::wstring dir = TestPath(L"doc_nonempty_dir");
  const std::wstring child = TestPath(L"doc_nonempty_dir\\stay.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(child, "stay_put"));

  auto h = OpenForDelete(
      dir, DELETE, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_DELETE_ON_CLOSE);
  ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  h.value->reset();

  EXPECT_TRUE(CheckIsDirectory(dir));
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L"..", L"stay.dat"}));
  EXPECT_TRUE(helpers::CheckFileContents(child, "stay_put"));
}

// Spec: [MS-FSA] 2.1.5.1.2.2 -- an open handle without FILE_SHARE_DELETE
// blocks DeleteFileW and DELETE opens with ERROR_SHARING_VIOLATION.
TEST_F(DeleteTest, DeleteWithoutShareDeleteFailsSharingViolation) {
  const std::wstring path = TestPath(L"no_share_delete.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "locked"));

  auto h1 = core::CreateWin32File(path, GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  EXPECT_WIN32_ERROR(::DeleteFileW(path.c_str()), ERROR_SHARING_VIOLATION);

  auto del_open =
      core::CreateWin32File(path, DELETE, core::kShareAll, OPEN_EXISTING, 0);
  EXPECT_FALSE(del_open);
  EXPECT_WIN32_ERROR_CODE(del_open.error, ERROR_SHARING_VIOLATION);

  h1.value->reset();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.4 / [MS-FSCC] 2.4.12 (Win10 1709+) -- POSIX
// disposition unlinks on deleting-handle close, allowing immediate name reuse.
TEST_F(DeleteTest, DispositionInfoExPosixUnlinkAndReuseName) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const std::wstring dir = TestPath(L"posix_ex_dir");
  const std::wstring file = TestPath(L"posix_ex_dir\\target.dat");
  const std::wstring nt_file = TestNtPath(L"posix_ex_dir\\target.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h1 =
      core::CreateWin32File(file, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);
  ASSERT_WIN32_OK(core::WriteAllBytes(h1.value->get(), "old_posix_data", 14));

  auto orig_index = core::GetFileIndex(h1.value->get());
  ASSERT_TRUE(orig_index) << core::FormatWin32Error(orig_index.error);

  auto h2 = OpenForDelete(file);
  ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);

  auto disp = core::SetDispositionEx(h2.value->get(),
                                     nt::kFileDispositionDelete |
                                         nt::kFileDispositionPosixSemantics);
  ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);

  // While deleting handle h2 is still open, new opens observe DELETE_PENDING.
  EXPECT_NT_STATUS(TryNtOpenExisting(nt_file), STATUS_DELETE_PENDING);

  // Closing h2 immediately unlinks the name even though h1 remains open.
  h2.value->reset();

  ASSERT_TRUE(CheckPosixUnlinkedReuse(h1.value->get(), *orig_index.value, dir,
                                      file, "old_posix_data", "reused_name!"));

  h1.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(file, "reused_name!"));
}

// Spec: [MS-FSA] 2.1.5.15.4 / [MS-FSCC] 2.4.12 (Win10 1809+) --
// FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE deletes a READONLY file directly.
TEST_F(DeleteTest, DispositionInfoExIgnoreReadOnly) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  const std::wstring path = TestPath(L"ignore_ro_del.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "ro_ex_data"));
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));

  // Without IGNORE_READONLY_ATTRIBUTE, FileDispositionInformationEx fails
  // STATUS_CANNOT_DELETE on a READONLY file.
  {
    auto h_plain = OpenForDelete(path, DELETE | FILE_WRITE_ATTRIBUTES);
    ASSERT_TRUE(h_plain) << core::FormatWin32Error(h_plain.error);
    auto fail_res = core::SetDispositionEx(h_plain.value->get(),
                                           nt::kFileDispositionDelete);
    EXPECT_NT_STATUS(fail_res.status, STATUS_CANNOT_DELETE);
  }

  // With IGNORE_READONLY_ATTRIBUTE, deleting a READONLY file succeeds
  // without clearing FILE_ATTRIBUTE_READONLY first.
  {
    auto h_ok = OpenForDelete(path, DELETE | FILE_WRITE_ATTRIBUTES);
    ASSERT_TRUE(h_ok) << core::FormatWin32Error(h_ok.error);
    auto ok_res = core::SetDispositionEx(
        h_ok.value->get(), nt::kFileDispositionDelete |
                               nt::kFileDispositionIgnoreReadonlyAttribute);
    ASSERT_NT_STATUS(ok_res.status, STATUS_SUCCESS);
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(path));
}

// Spec: [MS-FSA] 2.1.5.15.4 / [MS-FSCC] 2.4.12 (Win10 1709+) -- POSIX
// disposition unlinks an open empty directory immediately for name reuse.
TEST_F(DeleteTest, DispositionInfoExPosixOnEmptyDirectory) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const std::wstring parent = TestPath(L"posix_dir_parent");
  const std::wstring sub = TestPath(L"posix_dir_parent\\empty_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  auto h1 = core::CreateWin32File(
      sub, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, core::kShareAll,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);
  auto orig_index = core::GetFileIndex(h1.value->get());
  ASSERT_TRUE(orig_index) << core::FormatWin32Error(orig_index.error);

  {
    auto h2 = OpenForDelete(sub, DELETE, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(h2) << core::FormatWin32Error(h2.error);
    auto disp = core::SetDispositionEx(h2.value->get(),
                                       nt::kFileDispositionDelete |
                                           nt::kFileDispositionPosixSemantics);
    ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);
  }

  // Directory is unlinked immediately while h1 stays open.
  EXPECT_TRUE(helpers::CheckDirEntries(parent, {L".", L".."}));

  // Recreate same directory name while h1 is still open.
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  auto h_new = core::CreateWin32File(sub, FILE_READ_ATTRIBUTES, core::kShareAll,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(h_new) << core::FormatWin32Error(h_new.error);
  auto new_index = core::GetFileIndex(h_new.value->get());
  ASSERT_TRUE(new_index) << core::FormatWin32Error(new_index.error);
  EXPECT_NE(*new_index.value, *orig_index.value);

  h_new.value->reset();
  h1.value->reset();

  EXPECT_TRUE(CheckIsDirectory(sub));
}

// Spec: [MS-FSA] 2.1.5.15.3 -- legacy FileDispositionInformation (class 13)
// rejects a file with an active writable data-mapped view with
// STATUS_CANNOT_DELETE on all OS builds; the failed request leaves the file
// fully intact, and it becomes deletable once the view is unmapped.
TEST_F(DeleteTest, DeleteMappedFileFailsCannotDelete) {
  constexpr const char kMappedPayload[] = "mapped_file_content_bytes";
  const std::wstring path = TestPath(L"mapped_delete.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, kMappedPayload));

  MappedFileGuard mapped;
  ASSERT_TRUE(CreateMappedView(path, GENERIC_READ | GENERIC_WRITE,
                               PAGE_READWRITE, FILE_MAP_READ | FILE_MAP_WRITE,
                               &mapped));

  {
    auto h_del = OpenForDelete(path, DELETE | FILE_READ_ATTRIBUTES);
    ASSERT_TRUE(h_del) << core::FormatWin32Error(h_del.error);
    auto disp = core::SetDisposition(h_del.value->get(), /*delete_file=*/true);
    ASSERT_NT_STATUS(disp.status, STATUS_CANNOT_DELETE);
    EXPECT_TRUE(CheckDeletePending(h_del.value->get(), false));
  }

  // The rejected disposition must not take effect when its handle closes.
  // Use ASSERT_TRUE before dereferencing the mapped view so a non-conformant
  // filesystem that wrongly deleted the backing file fails cleanly instead of
  // raising STATUS_IN_PAGE_ERROR (0xc0000006) on a non-resident page.
  ASSERT_TRUE(helpers::CheckFileContents(path, kMappedPayload));
  EXPECT_EQ(std::memcmp(mapped.view.get(), kMappedPayload,
                        sizeof(kMappedPayload) - 1),
            0);

  mapped.view.reset();
  mapped.mapping.reset();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(path));
}

// OS-build threshold for the two DeleteFileW-on-data-mapped-file tests below:
// up to Windows 11 23H2 / Server 2022 NTFS fails every delete flavor (including
// the POSIX unlink DeleteFileW attempts first) with STATUS_CANNOT_DELETE while
// a writable data section is mapped; Windows 11 24H2 / Server 2025 (build
// 26100) NTFS lets POSIX unlink proceed. The boundary was established
// empirically on the GitHub-hosted windows-2022 (20348) and windows-2025
// (26100) images; client builds between them (22000-22631) are classified with
// the pre-24H2 behavior and were not individually verified.

// Spec: Win32 DeleteFileW / [MS-FSA] 2.1.5.15.3 (before Windows 11 24H2) --
// DeleteFileW on a file with an active writable data-mapped view fails
// ERROR_ACCESS_DENIED and leaves the file intact until the view is unmapped.
TEST_F(DeleteTest, DeleteFileWOnMappedFileFailsBeforeWin11_24H2) {
  REQUIRE_MAX_OS_BUILD(core::kBuildWin11_24H2 - 1U);

  constexpr const char kMappedPayload[] = "mapped_file_content_bytes";
  const std::wstring path = TestPath(L"mapped_delete_w_legacy.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, kMappedPayload));

  MappedFileGuard mapped;
  ASSERT_TRUE(CreateMappedView(path, GENERIC_READ | GENERIC_WRITE,
                               PAGE_READWRITE, FILE_MAP_READ | FILE_MAP_WRITE,
                               &mapped));

  EXPECT_WIN32_ERROR(::DeleteFileW(path.c_str()), ERROR_ACCESS_DENIED);
  EXPECT_TRUE(helpers::CheckFileContents(path, kMappedPayload));

  mapped.view.reset();
  mapped.mapping.reset();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(path));
}

// Spec: Win32 DeleteFileW / [MS-FSA] 2.1.5.15.4 (Windows 11 24H2 / Server
// 2025+) -- DeleteFileW's POSIX unlink succeeds on a file with an active
// writable data-mapped view, removing the name immediately while the mapped
// view stays valid and readable.
TEST_F(DeleteTest, DeleteFileWOnMappedFilePosixUnlinksOnWin11_24H2) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin11_24H2);

  constexpr const char kMappedPayload[] = "mapped_file_content_bytes";
  const std::wstring path = TestPath(L"mapped_delete_w_posix.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, kMappedPayload));

  MappedFileGuard mapped;
  ASSERT_TRUE(CreateMappedView(path, GENERIC_READ | GENERIC_WRITE,
                               PAGE_READWRITE, FILE_MAP_READ | FILE_MAP_WRITE,
                               &mapped));

  ASSERT_WIN32_TRUE(::DeleteFileW(path.c_str()));
  EXPECT_EQ(std::memcmp(mapped.view.get(), kMappedPayload,
                        sizeof(kMappedPayload) - 1),
            0);
  EXPECT_TRUE(helpers::CheckFileNotFound(path));
  mapped.view.reset();
  mapped.mapping.reset();
}

// Spec: [MS-FSA] 2.1.5.15.3 / Win32 CreateFileMappingW(SEC_IMAGE) -- an active
// SEC_IMAGE mapping blocks deletion with STATUS_CANNOT_DELETE on all OS builds.
TEST_F(DeleteTest, DeleteImageMappedFileFailsCannotDelete) {
  const std::wstring img_path = TestPath(L"mapped_image.exe");
  auto copy_res = fstest::CopyCurrentExecutableTo(img_path);
  ASSERT_TRUE(copy_res) << core::FormatWin32Error(copy_res.error);

  MappedFileGuard mapped;
  ASSERT_TRUE(CreateMappedView(img_path, GENERIC_READ | GENERIC_EXECUTE,
                               PAGE_READONLY | SEC_IMAGE, FILE_MAP_READ,
                               &mapped));

  EXPECT_WIN32_ERROR(::DeleteFileW(img_path.c_str()), ERROR_ACCESS_DENIED);

  {
    auto h_img_del = OpenForDelete(img_path);
    ASSERT_TRUE(h_img_del) << core::FormatWin32Error(h_img_del.error);
    auto img_disp =
        core::SetDisposition(h_img_del.value->get(), /*delete_file=*/true);
    EXPECT_NT_STATUS(img_disp.status, STATUS_CANNOT_DELETE);
  }

  mapped.view.reset();
  mapped.mapping.reset();

  EXPECT_WIN32_TRUE(::DeleteFileW(img_path.c_str()));
  EXPECT_TRUE(helpers::CheckFileNotFound(img_path));
}

// Spec: Win32 DuplicateHandle / [MS-FSA] 2.1.5.5 -- closing the original
// handle keeps delete-pending alive until the final duplicated handle closes.
TEST_F(DeleteTest, DuplicateHandleKeepsDeletePendingAliveUntilLastClose) {
  const std::wstring path = TestPath(L"dup_delete_pending.dat");
  const std::wstring nt_path = TestNtPath(L"dup_delete_pending.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "dup_delete_payload"));

  auto h1 = core::CreateWin32File(path, GENERIC_READ | DELETE, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  core::UniqueHandle h_dup;
  ASSERT_WIN32_TRUE(::DuplicateHandle(::GetCurrentProcess(), h1.value->get(),
                                      ::GetCurrentProcess(), h_dup.put(), 0,
                                      FALSE, DUPLICATE_SAME_ACCESS));
  ASSERT_TRUE(h_dup.valid());

  auto disp = core::SetDisposition(h1.value->get(), /*delete_file=*/true);
  ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);

  // Close original handle; duplicate keeps the FILE_OBJECT and DELETE_PENDING
  // alive.
  h1.value->reset();

  EXPECT_NT_STATUS(TryNtOpenExisting(nt_path), STATUS_DELETE_PENDING);
  EXPECT_TRUE(CheckDeletePending(h_dup.get(), true));

  auto data = core::ReadAllBytes(h_dup.get());
  ASSERT_TRUE(data) << core::FormatWin32Error(data.error);
  EXPECT_EQ(*data.value, "dup_delete_payload");

  // Closing the final duplicate handle completes deletion.
  h_dup.reset();
  EXPECT_TRUE(helpers::CheckFileNotFound(path));
}

// Spec: WDK NtDeleteFile / [MS-FSA] 2.1.5.5 -- NtDeleteFile deletes a file by
// path without a caller-held handle and fails STATUS_OBJECT_NAME_NOT_FOUND
// when the path is absent.
TEST_F(DeleteTest, NtDeleteFileDeletesFileAndReportsMissingPath) {
  const std::wstring file = TestPath(L"nt_del_file.dat");
  const std::wstring nt_file = TestNtPath(L"nt_del_file.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(file, "direct_nt_delete"));

  EXPECT_NT_STATUS(CallNtDeleteFile(nt_file), STATUS_SUCCESS);
  EXPECT_TRUE(helpers::CheckFileNotFound(file));

  // Missing path returns STATUS_OBJECT_NAME_NOT_FOUND.
  EXPECT_NT_STATUS(CallNtDeleteFile(nt_file), STATUS_OBJECT_NAME_NOT_FOUND);
}

// Spec: WDK NtDeleteFile / [MS-FSA] 2.1.5.5 -- NtDeleteFile removes an empty
// directory.
TEST_F(DeleteTest, NtDeleteFileRemovesEmptyDirectory) {
  const std::wstring dir = TestPath(L"nt_del_empty_dir");
  const std::wstring nt_dir = TestNtPath(L"nt_del_empty_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  EXPECT_NT_STATUS(CallNtDeleteFile(nt_dir), STATUS_SUCCESS);
  EXPECT_TRUE(helpers::CheckFileNotFound(dir));
}

// Spec: WDK NtDeleteFile / [MS-FSA] 2.1.5.5 -- NtDeleteFile opens with
// FILE_DELETE_ON_CLOSE and closes immediately: on a non-empty directory the
// call returns STATUS_SUCCESS, cleanup silently declines to delete, and both
// the directory and its child survive intact.
TEST_F(DeleteTest, NtDeleteFileLeavesNonEmptyDirectoryIntact) {
  const std::wstring dir = TestPath(L"nt_del_dir");
  const std::wstring nt_dir = TestNtPath(L"nt_del_dir");
  const std::wstring child = TestPath(L"nt_del_dir\\child.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(child, "child"));

  EXPECT_NT_STATUS(CallNtDeleteFile(nt_dir), STATUS_SUCCESS);
  EXPECT_TRUE(CheckIsDirectory(dir));
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L"..", L"child.dat"}));
  EXPECT_TRUE(helpers::CheckFileContents(child, "child"));
}

// Spec: [MS-FSA] 2.1.5.15.4 / [MS-FSCC] 2.4.12 (Win10 1809+) -- ON_CLOSE fails
// NOT_SUPPORTED unless opened DELETE_ON_CLOSE, and cancels it when cleared.
TEST_F(DeleteTest, DispositionInfoExDeleteOnCloseFlag) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  const std::wstring path = TestPath(L"disp_ex_on_close.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "on_close_payload"));

  // Per [MS-FSCC] 2.4.12 / [MS-FSA], FILE_DISPOSITION_ON_CLOSE cannot be set on
  // a handle that was not originally opened with FILE_DELETE_ON_CLOSE.
  {
    auto h = OpenForDelete(path, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    auto disp =
        core::SetDispositionEx(h.value->get(), nt::kFileDispositionDelete |
                                                   nt::kFileDispositionOnClose);
    EXPECT_NT_STATUS(disp.status, STATUS_NOT_SUPPORTED);
  }

  // Opening with FILE_FLAG_DELETE_ON_CLOSE and clearing via
  // FILE_DISPOSITION_ON_CLOSE | FILE_DISPOSITION_DO_NOT_DELETE cancels the
  // handle's delete-on-close state so the file survives handle closure.
  {
    auto h = OpenForDelete(path, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);

    auto clear_doc =
        core::SetDispositionEx(h.value->get(), nt::kFileDispositionDoNotDelete |
                                                   nt::kFileDispositionOnClose);
    ASSERT_NT_STATUS(clear_doc.status, STATUS_SUCCESS);
  }

  EXPECT_TRUE(helpers::CheckFileContents(path, "on_close_payload"));
}

// Spec: [MS-FSA] 2.1.5.15.3 -- legacy delete-pending on one hard link leaves
// the sibling link readable, and the last close removes only that link and
// decrements NumberOfLinks.
TEST_F(DeleteTest, HardLinkLegacyDeletePreservesOtherLink) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring hl_a = TestPath(L"hl_a.dat");
  const std::wstring hl_b = TestPath(L"hl_b.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(hl_a, "hardlink_del_payload"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(hl_b.c_str(), hl_a.c_str(), nullptr));

  auto info_init = core::QueryInfoByPath(hl_a);
  ASSERT_TRUE(info_init) << core::FormatWin32Error(info_init.error);
  EXPECT_EQ(info_init.value->nNumberOfLinks, 2UL);

  // Legacy delete-pending on hl_a must leave sibling link hl_b readable while
  // h_a remains open.
  auto h_a = core::CreateWin32File(hl_a, GENERIC_READ | DELETE, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_a) << core::FormatWin32Error(h_a.error);
  auto disp = core::SetDisposition(h_a.value->get(), /*delete_file=*/true);
  ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);

  EXPECT_TRUE(helpers::CheckFileContents(hl_b, "hardlink_del_payload"));

  h_a.value->reset();
  EXPECT_TRUE(helpers::CheckFileNotFound(hl_a));

  auto info_b1 = core::QueryInfoByPath(hl_b);
  ASSERT_TRUE(info_b1) << core::FormatWin32Error(info_b1.error);
  EXPECT_EQ(info_b1.value->nNumberOfLinks, 1UL);
  EXPECT_TRUE(helpers::CheckFileContents(hl_b, "hardlink_del_payload"));
}

// Spec: [MS-FSA] 2.1.5.15.4 / [MS-FSCC] 2.4.12 (Win10 1709+) -- POSIX unlink of
// one hard link removes that name and decrements NumberOfLinks as soon as the
// deleting handle closes, even while another handle opened through the same
// link stays open and readable.
TEST_F(DeleteTest, HardLinkPosixUnlinkPreservesOtherLink) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring hl_b = TestPath(L"hl_posix_b.dat");
  const std::wstring hl_c = TestPath(L"hl_posix_c.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(hl_b, "hardlink_del_payload"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(hl_c.c_str(), hl_b.c_str(), nullptr));

  auto info_two = core::QueryInfoByPath(hl_b);
  ASSERT_TRUE(info_two) << core::FormatWin32Error(info_two.error);
  EXPECT_EQ(info_two.value->nNumberOfLinks, 2UL);

  auto h_c_keep = core::CreateWin32File(hl_c, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_c_keep) << core::FormatWin32Error(h_c_keep.error);

  {
    auto h_c = OpenForDelete(hl_c, DELETE | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h_c) << core::FormatWin32Error(h_c.error);
    auto posix_disp = core::SetDispositionEx(
        h_c.value->get(),
        nt::kFileDispositionDelete | nt::kFileDispositionPosixSemantics);
    ASSERT_NT_STATUS(posix_disp.status, STATUS_SUCCESS);
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(hl_c));

  auto info_b2 = core::QueryInfoByPath(hl_b);
  ASSERT_TRUE(info_b2) << core::FormatWin32Error(info_b2.error);
  EXPECT_EQ(info_b2.value->nNumberOfLinks, 1UL);

  auto keep_data = core::ReadAllBytes(h_c_keep.value->get());
  ASSERT_TRUE(keep_data) << core::FormatWin32Error(keep_data.error);
  EXPECT_EQ(*keep_data.value, "hardlink_del_payload");

  h_c_keep.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(hl_b, "hardlink_del_payload"));
}

// Spec: [MS-FSA] 2.1.5.5 -- FILE_FLAG_DELETE_ON_CLOSE opened through one hard
// link removes only that link on close: the sibling link keeps the data and
// NumberOfLinks drops to 1.
TEST_F(DeleteTest, DeleteOnCloseThroughHardLinkRemovesOnlyThatLink) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  const std::wstring link_a = TestPath(L"doc_hl_a.dat");
  const std::wstring link_b = TestPath(L"doc_hl_b.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(link_a, "doc_hardlink_payload"));
  ASSERT_WIN32_TRUE(::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));

  {
    auto h_doc = OpenForDelete(link_b, DELETE | FILE_READ_ATTRIBUTES,
                               FILE_FLAG_DELETE_ON_CLOSE);
    ASSERT_TRUE(h_doc) << core::FormatWin32Error(h_doc.error);
    auto std_info = core::QueryStandardInfo(h_doc.value->get());
    ASSERT_TRUE(std_info) << core::FormatNtStatus(std_info.status);
    EXPECT_EQ(std_info.value->number_of_links, 2UL);
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(link_b));
  auto info_a = core::QueryInfoByPath(link_a);
  ASSERT_TRUE(info_a) << core::FormatWin32Error(info_a.error);
  EXPECT_EQ(info_a.value->nNumberOfLinks, 1UL);
  EXPECT_TRUE(helpers::CheckFileContents(link_a, "doc_hardlink_payload"));
}

// Spec: [MS-FSA] 2.1.5.15.4 / [MS-FSCC] 2.4.12 (Win10 1709+) --
// FILE_DISPOSITION_POSIX_SEMANTICS does not by itself bypass
// FILE_ATTRIBUTE_READONLY: without FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE
// the request fails STATUS_CANNOT_DELETE and the file survives the close.
TEST_F(DeleteTest, DispositionInfoExPosixOnReadOnlyFailsCannotDelete) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const std::wstring path = TestPath(L"posix_ro_del.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "posix_ro_data"));
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));

  {
    auto h = OpenForDelete(path, DELETE | FILE_READ_ATTRIBUTES);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    auto disp = core::SetDispositionEx(h.value->get(),
                                       nt::kFileDispositionDelete |
                                           nt::kFileDispositionPosixSemantics);
    EXPECT_NT_STATUS(disp.status, STATUS_CANNOT_DELETE);
    EXPECT_TRUE(CheckDeletePending(h.value->get(), false));
  }

  EXPECT_TRUE(helpers::CheckFileContents(path, "posix_ro_data"));
  auto attrs = core::QueryWin32Attributes(path);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_READONLY, 0UL);

  ASSERT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSCC] 2.4.12 (Win10 1809+) --
// FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK makes FileDispositionInformationEx
// fail STATUS_CANNOT_DELETE while the file backs an active SEC_IMAGE section
// (with or without FILE_DISPOSITION_POSIX_SEMANTICS), and is a no-op for a file
// that backs no image section.
TEST_F(DeleteTest, DispositionInfoExForceImageSectionCheck) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  constexpr ULONG kForcedDelete =
      nt::kFileDispositionDelete | nt::kFileDispositionForceImageSectionCheck;

  // Image-mapped file: the forced image-section check rejects deletion.
  const std::wstring img_path = TestPath(L"force_img_check.exe");
  auto copy_res = fstest::CopyCurrentExecutableTo(img_path);
  ASSERT_TRUE(copy_res) << core::FormatWin32Error(copy_res.error);
  {
    MappedFileGuard mapped;
    ASSERT_TRUE(CreateMappedView(img_path, GENERIC_READ | GENERIC_EXECUTE,
                                 PAGE_READONLY | SEC_IMAGE, FILE_MAP_READ,
                                 &mapped));

    auto h_img = OpenForDelete(img_path, DELETE | FILE_READ_ATTRIBUTES);
    ASSERT_TRUE(h_img) << core::FormatWin32Error(h_img.error);
    auto disp = core::SetDispositionEx(h_img.value->get(), kForcedDelete);
    EXPECT_NT_STATUS(disp.status, STATUS_CANNOT_DELETE);
    auto posix_disp = core::SetDispositionEx(
        h_img.value->get(), kForcedDelete | nt::kFileDispositionPosixSemantics);
    EXPECT_NT_STATUS(posix_disp.status, STATUS_CANNOT_DELETE);
    EXPECT_TRUE(CheckDeletePending(h_img.value->get(), false));
  }
  auto img_attrs = core::QueryWin32Attributes(img_path);
  EXPECT_TRUE(img_attrs) << core::FormatWin32Error(img_attrs.error);

  // Plain data file (no image section): the flag does not block deletion.
  const std::wstring plain_path = TestPath(L"force_img_check_plain.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(plain_path, "no_image_section"));
  {
    auto h_plain = OpenForDelete(plain_path);
    ASSERT_TRUE(h_plain) << core::FormatWin32Error(h_plain.error);
    auto disp = core::SetDispositionEx(h_plain.value->get(), kForcedDelete);
    ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);
  }
  EXPECT_TRUE(helpers::CheckFileNotFound(plain_path));
}

// Spec: [MS-FSA] 2.1.5.15.3 / [MS-FSCC] 2.4.11 -- FileDispositionInformation
// with DeleteFile=TRUE on a non-empty directory fails immediately with
// STATUS_DIRECTORY_NOT_EMPTY, contrasting with FILE_DELETE_ON_CLOSE.
TEST_F(DeleteTest, FileDispositionInfoOnNonEmptyDirFailsImmediately) {
  const std::wstring dir = TestPath(L"disp_nonempty_dir");
  const std::wstring child = TestPath(L"disp_nonempty_dir\\child.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(child, "keep_dir_nonempty"));

  {
    auto h_dir = OpenForDelete(dir, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(h_dir) << core::FormatWin32Error(h_dir.error);

    auto disp = core::SetDisposition(h_dir.value->get(), /*delete_file=*/true);
    EXPECT_NT_STATUS(disp.status, STATUS_DIRECTORY_NOT_EMPTY);
    EXPECT_TRUE(CheckDeletePending(h_dir.value->get(), false));
  }

  EXPECT_TRUE(CheckIsDirectory(dir));
  EXPECT_TRUE(helpers::CheckFileContents(child, "keep_dir_nonempty"));

  // Once emptied, FileDispositionInformation(TRUE) succeeds and deletes on
  // close.
  ASSERT_WIN32_TRUE(::DeleteFileW(child.c_str()));
  {
    auto h_dir = OpenForDelete(dir, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(h_dir) << core::FormatWin32Error(h_dir.error);
    auto disp = core::SetDisposition(h_dir.value->get(), /*delete_file=*/true);
    ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);
    EXPECT_TRUE(CheckDeletePending(h_dir.value->get(), true));
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(dir));
}

// Spec: [MS-FSA] 2.1.5.1 & 2.1.5.15.3 -- creating a child file or subdirectory
// inside a legacy delete-pending directory fails STATUS_DELETE_PENDING /
// ERROR_ACCESS_DENIED until disposition is cleared.
TEST_F(DeleteTest, CreateChildInDeletePendingDirFailsDeletePending) {
  const std::wstring dir = TestPath(L"dp_parent_dir");
  const std::wstring child_file = TestPath(L"dp_parent_dir\\new_file.dat");
  const std::wstring nt_child_file = TestNtPath(L"dp_parent_dir\\new_file.dat");
  const std::wstring child_sub = TestPath(L"dp_parent_dir\\new_sub");
  const std::wstring nt_child_sub = TestNtPath(L"dp_parent_dir\\new_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h_keep = core::CreateWin32File(
      dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      core::kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(h_keep) << core::FormatWin32Error(h_keep.error);

  auto h_del = OpenForDelete(dir, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(h_del) << core::FormatWin32Error(h_del.error);

  auto mark = core::SetDisposition(h_del.value->get(), /*delete_file=*/true);
  ASSERT_NT_STATUS(mark.status, STATUS_SUCCESS);
  EXPECT_TRUE(CheckDeletePending(h_keep.value->get(), true));

  // Creating a child file or subdirectory inside the delete-pending directory
  // fails with STATUS_DELETE_PENDING (NT) and ERROR_ACCESS_DENIED (Win32).
  auto nt_file = core::NtCreateFilePath(
      nt_child_file, GENERIC_WRITE | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileCreate, nt::kFileNonDirectoryFile);
  EXPECT_NT_STATUS(nt_file.status, STATUS_DELETE_PENDING);

  auto win_file =
      core::CreateWin32File(child_file, GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(win_file);
  EXPECT_WIN32_ERROR_CODE(win_file.error, ERROR_ACCESS_DENIED);

  auto nt_sub = core::NtCreateFilePath(
      nt_child_sub, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_ATTRIBUTE_DIRECTORY,
      core::kShareAll, nt::kFileCreate, nt::kFileDirectoryFile);
  EXPECT_NT_STATUS(nt_sub.status, STATUS_DELETE_PENDING);

  EXPECT_WIN32_ERROR(::CreateDirectoryW(child_sub.c_str(), nullptr),
                     ERROR_ACCESS_DENIED);

  // Clearing delete-pending restores child creation cleanly.
  auto unmark = core::SetDisposition(h_del.value->get(), /*delete_file=*/false);
  ASSERT_NT_STATUS(unmark.status, STATUS_SUCCESS);
  EXPECT_TRUE(CheckDeletePending(h_keep.value->get(), false));

  h_del.value->reset();
  h_keep.value->reset();

  ASSERT_WIN32_OK(core::WriteFileContents(child_file, "restored_child"));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(child_sub.c_str(), nullptr));
  EXPECT_TRUE(helpers::CheckFileContents(child_file, "restored_child"));
  EXPECT_TRUE(CheckIsDirectory(child_sub));
}

} // namespace fstest
