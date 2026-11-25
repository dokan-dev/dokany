// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/os_info.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {
namespace {

::testing::AssertionResult
CheckStreamsWithSizes(std::wstring_view path,
                      const std::map<std::wstring, int64_t> &expected) {
  auto streams = core::ListStreamsWin32(path);
  if (!streams) {
    return ::testing::AssertionFailure()
           << "ListStreamsWin32('" << core::WideToUtf8(path)
           << "') failed: " << core::FormatWin32Error(streams.error);
  }
  std::map<std::wstring, int64_t> actual;
  for (const auto &entry : *streams.value) {
    if (!actual.emplace(entry.name, entry.size).second) {
      return ::testing::AssertionFailure()
             << "Duplicate stream '" << core::WideToUtf8(entry.name) << "' on '"
             << core::WideToUtf8(path) << "'";
    }
  }
  if (actual != expected) {
    return ::testing::AssertionFailure()
           << "Stream set/sizes mismatch on '" << core::WideToUtf8(path)
           << "' (actual count=" << actual.size()
           << ", expected count=" << expected.size() << ")";
  }
  return ::testing::AssertionSuccess();
}

// Worker state for ConcurrentRenameAndEnumerateStress. Everything a worker
// thread dereferences lives in heap objects co-owned through std::shared_ptr
// (by the test body, by the per-worker context, and by ScopedThread), so a
// worker that outlives a Join() timeout never touches a dead stack frame.
struct RenameEnumStressShared {
  fstest::ScopedCriticalSection cs;
  core::UniqueHandle start_event;
  std::atomic<int> error_count{0};
  std::vector<std::string> errors;

  void RecordError(std::string msg) {
    error_count.fetch_add(1, std::memory_order_relaxed);
    fstest::ScopedCsLock lock(cs);
    if (errors.size() < 32) {
      errors.push_back(std::move(msg));
    }
  }
};

struct RenameWorkerContext {
  std::shared_ptr<RenameEnumStressShared> shared;
  std::wstring path_a;
  std::wstring path_b;
  int worker_id = 0;
  int iterations = 0;
};

struct EnumReaderContext {
  std::shared_ptr<RenameEnumStressShared> shared;
  std::wstring dir_path;
  int iterations = 0;
};

DWORD WINAPI RenameStressWorkerProc(LPVOID param) {
  auto *ctx = static_cast<RenameWorkerContext *>(param);
  if (ctx->shared->start_event.valid()) {
    ::WaitForSingleObject(ctx->shared->start_event.get(), 10000);
  }
  for (int i = 0; i < ctx->iterations; ++i) {
    ::SetLastError(ERROR_SUCCESS);
    if (::MoveFileExW(ctx->path_a.c_str(), ctx->path_b.c_str(), 0) == FALSE) {
      const DWORD err = ::GetLastError();
      ctx->shared->RecordError("worker " + std::to_string(ctx->worker_id) +
                               " iter " + std::to_string(i) +
                               " a->b failed: " + core::FormatWin32Error(err));
      return 1;
    }
    ::SetLastError(ERROR_SUCCESS);
    if (::MoveFileExW(ctx->path_b.c_str(), ctx->path_a.c_str(), 0) == FALSE) {
      const DWORD err = ::GetLastError();
      ctx->shared->RecordError("worker " + std::to_string(ctx->worker_id) +
                               " iter " + std::to_string(i) +
                               " b->a failed: " + core::FormatWin32Error(err));
      return 1;
    }
  }
  return 0;
}

DWORD WINAPI EnumStressReaderProc(LPVOID param) {
  auto *ctx = static_cast<EnumReaderContext *>(param);
  if (ctx->shared->start_event.valid()) {
    ::WaitForSingleObject(ctx->shared->start_event.get(), 10000);
  }
  for (int i = 0; i < ctx->iterations; ++i) {
    auto listing = core::ListDirectoryWin32(ctx->dir_path, L"*");
    if (!listing) {
      ctx->shared->RecordError("reader iter " + std::to_string(i) +
                               " ListDirectoryWin32 failed: " +
                               core::FormatWin32Error(listing.error));
      continue;
    }
    bool has_dot = false;
    bool has_dotdot = false;
    std::set<std::wstring> seen;
    for (const auto &entry : *listing.value) {
      if (entry == L".") {
        has_dot = true;
      } else if (entry == L"..") {
        has_dotdot = true;
      } else if (entry != L"ren_w0_a.dat" && entry != L"ren_w0_b.dat" &&
                 entry != L"ren_w1_a.dat" && entry != L"ren_w1_b.dat") {
        ctx->shared->RecordError(
            "reader iter " + std::to_string(i) +
            " unexpected entry: " + core::WideToUtf8(entry));
      }
      if (!seen.insert(entry).second) {
        ctx->shared->RecordError(
            "reader iter " + std::to_string(i) +
            " duplicate entry: " + core::WideToUtf8(entry));
      }
    }
    if (!has_dot || !has_dotdot) {
      ctx->shared->RecordError("reader iter " + std::to_string(i) +
                               " missing '.' or '..' entry");
    }
  }
  return 0;
}

// Layout shared by the cross-directory rename timestamp tests:
// <src_parent>\item.dat is moved to <dst_parent>\moved.dat after the file and
// both parents have had all four timestamps backdated to `backdate`.
struct BackdatedRenameLayout {
  std::wstring src_parent;
  std::wstring dst_parent;
  std::wstring src_file;
  std::wstring dst_file;
  int64_t backdate = 0;
};

::testing::AssertionResult
PrepareBackdatedCrossDirRename(std::wstring_view src_parent,
                               std::wstring_view dst_parent,
                               BackdatedRenameLayout *out) {
  out->src_parent = std::wstring(src_parent);
  out->dst_parent = std::wstring(dst_parent);
  out->src_file = out->src_parent + L"\\item.dat";
  out->dst_file = out->dst_parent + L"\\moved.dat";
  out->backdate = core::MakeUtcFileTimeInt64(2020, 1, 1);

  for (const std::wstring *dir : {&out->src_parent, &out->dst_parent}) {
    ::SetLastError(ERROR_SUCCESS);
    if (::CreateDirectoryW(dir->c_str(), nullptr) == FALSE) {
      const DWORD err = ::GetLastError();
      return ::testing::AssertionFailure()
             << "CreateDirectoryW('" << core::WideToUtf8(*dir)
             << "') failed: " << core::FormatWin32Error(err);
    }
  }
  if (auto wr = core::WriteFileContents(out->src_file, "ts_payload"); !wr) {
    return ::testing::AssertionFailure() << "Failed to write source file: "
                                         << core::FormatWin32Error(wr.error);
  }
  // Backdate the file first, then both parents, closing every handle so NTFS
  // does not freeze automatic timestamp updates on subsequent operations.
  if (!fstest::BackdateAllTimes(out->src_file, out->backdate)) {
    return ::testing::AssertionFailure() << "Failed to backdate source file";
  }
  if (!fstest::BackdateAllTimes(out->src_parent, out->backdate,
                                /*is_dir=*/true)) {
    return ::testing::AssertionFailure() << "Failed to backdate source parent";
  }
  if (!fstest::BackdateAllTimes(out->dst_parent, out->backdate,
                                /*is_dir=*/true)) {
    return ::testing::AssertionFailure() << "Failed to backdate target parent";
  }
  return ::testing::AssertionSuccess();
}

// Scratch directory on a volume other than the one under test (the host temp
// directory), with best-effort cleanup of every tracked file on destruction.
struct OtherVolumeScratch {
  OtherVolumeScratch() = default;
  OtherVolumeScratch(const OtherVolumeScratch &) = delete;
  OtherVolumeScratch &operator=(const OtherVolumeScratch &) = delete;

  ~OtherVolumeScratch() {
    for (const auto &file : files) {
      ::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
      ::DeleteFileW(file.c_str());
    }
    if (!dir.empty()) {
      ::RemoveDirectoryW(dir.c_str());
    }
  }

  // Returns <dir>\<name> and registers it for deletion on destruction.
  std::wstring Track(std::wstring_view name) {
    std::wstring path = dir;
    path.push_back(L'\\');
    path.append(name);
    files.push_back(path);
    return path;
  }

  std::wstring dir;
  std::vector<std::wstring> files;
};

// Creates a scratch directory under the host temp directory when that
// directory is provably on a different volume than `test_dir` (different
// volume serial number). Returns false with `*reason` set when no second
// volume can be determined; cross-volume tests then skip.
bool AcquireOtherVolumeScratch(std::wstring_view test_dir,
                               OtherVolumeScratch *out, std::string *reason) {
  auto test_info = core::QueryInfoByPath(test_dir, /*backup_semantics=*/true);
  if (!test_info) {
    *reason = "Cannot query the volume serial number of the test directory: " +
              core::FormatWin32Error(test_info.error);
    return false;
  }

  wchar_t temp_buf[MAX_PATH + 2] = {};
  ::SetLastError(ERROR_SUCCESS);
  const DWORD temp_len = ::GetTempPathW(
      static_cast<DWORD>(sizeof(temp_buf) / sizeof(temp_buf[0])), temp_buf);
  const DWORD temp_err = ::GetLastError();
  if (temp_len == 0 || temp_len >= sizeof(temp_buf) / sizeof(temp_buf[0])) {
    *reason = "GetTempPathW failed: " + core::FormatWin32Error(temp_err);
    return false;
  }
  std::wstring temp_root(temp_buf, temp_len);
  while (temp_root.size() > 3 && temp_root.back() == L'\\') {
    temp_root.pop_back();
  }

  auto temp_info = core::QueryInfoByPath(temp_root, /*backup_semantics=*/true);
  if (!temp_info) {
    *reason = "Cannot query the volume serial number of the host temp "
              "directory: " +
              core::FormatWin32Error(temp_info.error);
    return false;
  }
  if (temp_info.value->dwVolumeSerialNumber ==
      test_info.value->dwVolumeSerialNumber) {
    *reason = "Requires a second volume: the host temp directory '" +
              core::WideToUtf8(temp_root) +
              "' has the same volume serial number as the test directory.";
    return false;
  }

  std::wstring scratch = temp_root;
  if (scratch.back() != L'\\') {
    scratch.push_back(L'\\');
  }
  scratch += L"fstest_xvol_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
             std::to_wstring(::GetTickCount64());
  ::SetLastError(ERROR_SUCCESS);
  if (::CreateDirectoryW(scratch.c_str(), nullptr) == FALSE) {
    const DWORD err = ::GetLastError();
    *reason = "Cannot create a scratch directory on the second volume ('" +
              core::WideToUtf8(scratch) + "'): " + core::FormatWin32Error(err);
    return false;
  }
  out->dir = std::move(scratch);
  return true;
}

} // namespace

void ForceLink_RenameMove() {}

// Spec: Win32 MoveFileExW / [MS-FSA] 2.1.5.15.12 -- renames files, rejects
// collisions without REPLACE_EXISTING, and blocks replacing READONLY targets.
TEST_F(RenameMoveTest, MoveFileBasicReplaceAndReadOnlyTarget) {
  const std::wstring dir = TestPath(L"move_basic");
  const std::wstring foo = TestPath(L"move_basic\\foo.dat");
  const std::wstring bar = TestPath(L"move_basic\\bar.dat");
  const std::wstring baz = TestPath(L"move_basic\\baz.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(foo, "foo_payload"));
  ASSERT_TRUE(
      core::WriteFileContents(baz, "baz_payload", FILE_ATTRIBUTE_READONLY));

  ASSERT_WIN32_TRUE(::MoveFileExW(foo.c_str(), bar.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(foo));
  EXPECT_TRUE(core::QueryWin32Attributes(bar));

  // Colliding without MOVEFILE_REPLACE_EXISTING fails ERROR_ALREADY_EXISTS.
  EXPECT_WIN32_ERROR(::MoveFileExW(bar.c_str(), baz.c_str(), 0),
                     ERROR_ALREADY_EXISTS);
  EXPECT_TRUE(core::QueryWin32Attributes(bar));
  EXPECT_TRUE(core::QueryWin32Attributes(baz));

  // Replacing a READONLY target fails ERROR_ACCESS_DENIED.
  EXPECT_WIN32_ERROR(
      ::MoveFileExW(bar.c_str(), baz.c_str(), MOVEFILE_REPLACE_EXISTING),
      ERROR_ACCESS_DENIED);

  ASSERT_WIN32_TRUE(::SetFileAttributesW(baz.c_str(), FILE_ATTRIBUTE_NORMAL));
  ASSERT_WIN32_TRUE(
      ::MoveFileExW(bar.c_str(), baz.c_str(), MOVEFILE_REPLACE_EXISTING));

  EXPECT_TRUE(helpers::CheckFileNotFound(bar));
  EXPECT_TRUE(helpers::CheckFileContents(baz, "foo_payload"));

  EXPECT_WIN32_TRUE(::DeleteFileW(baz.c_str()));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: Win32 MoveFileExW / [MS-FSA] 2.1.5.15.12 -- renaming a directory over
// an existing directory fails even with REPLACE_EXISTING until removed.
TEST_F(RenameMoveTest, MoveDirectoryReplaceFailsAndAfterRemoveSucceeds) {
  const std::wstring dir = TestPath(L"move_dir_basic");
  const std::wstring foo = TestPath(L"move_dir_basic\\foo");
  const std::wstring bar = TestPath(L"move_dir_basic\\bar");
  const std::wstring baz = TestPath(L"move_dir_basic\\baz");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(foo.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(baz.c_str(), nullptr));

  ASSERT_WIN32_TRUE(::MoveFileExW(foo.c_str(), bar.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(foo));
  EXPECT_TRUE(core::QueryWin32Attributes(bar));

  EXPECT_WIN32_ERROR(::MoveFileExW(bar.c_str(), baz.c_str(), 0),
                     ERROR_ALREADY_EXISTS);
  EXPECT_TRUE(core::QueryWin32Attributes(bar));
  EXPECT_TRUE(core::QueryWin32Attributes(baz));

  EXPECT_WIN32_ERROR(
      ::MoveFileExW(bar.c_str(), baz.c_str(), MOVEFILE_REPLACE_EXISTING),
      ERROR_ACCESS_DENIED);
  EXPECT_TRUE(core::QueryWin32Attributes(bar));
  EXPECT_TRUE(core::QueryWin32Attributes(baz));

  ASSERT_WIN32_TRUE(::RemoveDirectoryW(baz.c_str()));
  ASSERT_WIN32_TRUE(
      ::MoveFileExW(bar.c_str(), baz.c_str(), MOVEFILE_REPLACE_EXISTING));
  EXPECT_TRUE(helpers::CheckFileNotFound(bar));
  EXPECT_TRUE(core::QueryWin32Attributes(baz));

  EXPECT_WIN32_TRUE(::RemoveDirectoryW(baz.c_str()));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: Win32 MoveFileExW / [MS-FSA] 2.1.5.15.12 -- moving a file across
// sibling subdirectories preserves data and removes the source entry.
TEST_F(RenameMoveTest, MoveFileAcrossSubdirectories) {
  const std::wstring dir = TestPath(L"cross_dir");
  const std::wstring foo = TestPath(L"cross_dir\\foo");
  const std::wstring bar = TestPath(L"cross_dir\\bar");
  const std::wstring src = TestPath(L"cross_dir\\foo\\hello.dat");
  const std::wstring dst = TestPath(L"cross_dir\\bar\\world.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(foo.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(bar.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(src, "cross_dir_payload"));

  ASSERT_WIN32_TRUE(::MoveFileExW(src.c_str(), dst.c_str(), 0));

  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "cross_dir_payload"));

  EXPECT_WIN32_TRUE(::DeleteFileW(dst.c_str()));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(bar.c_str()));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(foo.c_str()));
  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- case-only renames of files and directories
// update stored casing while preserving FileIndex and contents.
TEST_F(RenameMoveTest, CaseOnlyRenameFileAndDirectory) {
  REQUIRE_FS_CAPABILITY(FsCapability::kCasePreservedNames);

  // Case-only file rename with flags=0 and with MOVEFILE_REPLACE_EXISTING.
  {
    const std::wstring sub = TestPath(L"case_file_dir");
    const std::wstring p_low = TestPath(L"case_file_dir\\foo.dat");
    const std::wstring p_mix = TestPath(L"case_file_dir\\Foo.DAT");
    const std::wstring p_alt = TestPath(L"case_file_dir\\fOo.DaT");
    ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
    ASSERT_TRUE(core::WriteFileContents(p_low, "case_data"));

    auto orig_idx = core::QueryPathFileIndex(p_low);
    ASSERT_TRUE(orig_idx) << core::FormatWin32Error(orig_idx.error);

    ASSERT_WIN32_TRUE(::MoveFileExW(p_low.c_str(), p_mix.c_str(), 0));
    EXPECT_TRUE(helpers::CheckDirEntries(sub, {L".", L"..", L"Foo.DAT"}));

    {
      auto h = core::CreateWin32File(p_mix, GENERIC_READ, core::kShareAll,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
      ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
      auto idx1 = core::GetFileIndex(h.value->get());
      ASSERT_TRUE(idx1);
      EXPECT_EQ(*idx1.value, *orig_idx.value);

      auto fp1 = core::QueryFinalPath(h.value->get(),
                                      VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
      ASSERT_TRUE(fp1) << core::FormatWin32Error(fp1.error);
      EXPECT_TRUE(fp1.value->ends_with(L"\\Foo.DAT"))
          << "Final path: " << core::WideToUtf8(*fp1.value);

      auto data = core::ReadAllBytes(h.value->get());
      ASSERT_TRUE(data);
      EXPECT_EQ(*data.value, "case_data");
    }

    ASSERT_WIN32_TRUE(
        ::MoveFileExW(p_mix.c_str(), p_alt.c_str(), MOVEFILE_REPLACE_EXISTING));
    EXPECT_TRUE(helpers::CheckDirEntries(sub, {L".", L"..", L"fOo.DaT"}));

    auto idx2 = core::QueryPathFileIndex(p_alt);
    ASSERT_TRUE(idx2);
    EXPECT_EQ(*idx2.value, *orig_idx.value);
  }

  // Case-only directory rename with flags=0 and with MOVEFILE_REPLACE_EXISTING.
  {
    const std::wstring parent = TestPath(L"case_dir_parent");
    const std::wstring d_low = TestPath(L"case_dir_parent\\sub");
    const std::wstring d_up = TestPath(L"case_dir_parent\\SUB");
    const std::wstring d_mix = TestPath(L"case_dir_parent\\Sub");
    ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
    ASSERT_WIN32_TRUE(::CreateDirectoryW(d_low.c_str(), nullptr));

    auto orig_idx = core::QueryPathFileIndex(d_low, /*backup_semantics=*/true);
    ASSERT_TRUE(orig_idx) << core::FormatWin32Error(orig_idx.error);

    ASSERT_WIN32_TRUE(::MoveFileExW(d_low.c_str(), d_up.c_str(), 0));
    EXPECT_TRUE(helpers::CheckDirEntries(parent, {L".", L"..", L"SUB"}));

    {
      auto h =
          core::CreateWin32File(d_up, FILE_READ_ATTRIBUTES, core::kShareAll,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
      ASSERT_TRUE(h);
      auto fp = core::QueryFinalPath(h.value->get(),
                                     VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
      ASSERT_TRUE(fp) << core::FormatWin32Error(fp.error);
      EXPECT_TRUE(fp.value->ends_with(L"\\SUB"))
          << "Final path: " << core::WideToUtf8(*fp.value);
    }

    ASSERT_WIN32_TRUE(
        ::MoveFileExW(d_up.c_str(), d_mix.c_str(), MOVEFILE_REPLACE_EXISTING));
    EXPECT_TRUE(helpers::CheckDirEntries(parent, {L".", L"..", L"Sub"}));

    auto idx2 = core::QueryPathFileIndex(d_mix, /*backup_semantics=*/true);
    ASSERT_TRUE(idx2);
    EXPECT_EQ(*idx2.value, *orig_idx.value);
  }
}

// Spec: [MS-FSA] 2.1.5.15.12 -- same-directory and cross-directory renames
// preserve FileIndex, file attributes, and file data.
TEST_F(RenameMoveTest, RenamePreservesFileIndexAndData) {
  const std::wstring dir_a = TestPath(L"idx_dir_a");
  const std::wstring dir_b = TestPath(L"idx_dir_b");
  const std::wstring p1 = TestPath(L"idx_dir_a\\orig.dat");
  const std::wstring p2 = TestPath(L"idx_dir_a\\same_dir.dat");
  const std::wstring p3 = TestPath(L"idx_dir_b\\other_dir.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_b.c_str(), nullptr));
  ASSERT_TRUE(
      core::WriteFileContents(p1, "preserve_me_12345", FILE_ATTRIBUTE_HIDDEN));

  auto orig_idx = core::QueryPathFileIndex(p1);
  ASSERT_TRUE(orig_idx) << core::FormatWin32Error(orig_idx.error);
  auto orig_attrs = core::QueryWin32Attributes(p1);
  ASSERT_TRUE(orig_attrs);

  // Same-directory rename.
  ASSERT_WIN32_TRUE(::MoveFileExW(p1.c_str(), p2.c_str(), 0));
  auto idx2 = core::QueryPathFileIndex(p2);
  ASSERT_TRUE(idx2);
  EXPECT_EQ(*idx2.value, *orig_idx.value);
  auto attrs2 = core::QueryWin32Attributes(p2);
  ASSERT_TRUE(attrs2);
  EXPECT_EQ(MaskPolicyAttributes(*attrs2.value),
            MaskPolicyAttributes(*orig_attrs.value));
  EXPECT_TRUE(helpers::CheckFileContents(p2, "preserve_me_12345"));

  // Cross-directory move.
  ASSERT_WIN32_TRUE(::MoveFileExW(p2.c_str(), p3.c_str(), 0));
  auto idx3 = core::QueryPathFileIndex(p3);
  ASSERT_TRUE(idx3);
  EXPECT_EQ(*idx3.value, *orig_idx.value);
  auto attrs3 = core::QueryWin32Attributes(p3);
  ASSERT_TRUE(attrs3);
  EXPECT_EQ(MaskPolicyAttributes(*attrs3.value),
            MaskPolicyAttributes(*orig_attrs.value));
  EXPECT_TRUE(helpers::CheckFileContents(p3, "preserve_me_12345"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- a file open with FILE_SHARE_DELETE can be
// renamed while open, updating its final path and keeping the handle usable.
TEST_F(RenameMoveTest, RenameWhileOpenWithShareDelete) {
  const std::wstring src = TestPath(L"open_src.dat");
  const std::wstring dst = TestPath(L"open_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "hello"));

  auto h1 =
      core::CreateWin32File(src, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  ASSERT_WIN32_TRUE(::MoveFileExW(src.c_str(), dst.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(src));

  auto fp = core::QueryFinalPath(h1.value->get(),
                                 VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
  ASSERT_TRUE(fp) << core::FormatWin32Error(fp.error);
  EXPECT_TRUE(fp.value->ends_with(L"\\open_dst.dat"))
      << "Final path: " << core::WideToUtf8(*fp.value);

  // Handle remains usable for read/write after rename.
  LARGE_INTEGER zero{};
  ASSERT_WIN32_TRUE(
      ::SetFilePointerEx(h1.value->get(), zero, nullptr, FILE_END));
  ASSERT_TRUE(core::WriteAllBytes(h1.value->get(), "_world", 6));
  ASSERT_WIN32_TRUE(
      ::SetFilePointerEx(h1.value->get(), zero, nullptr, FILE_BEGIN));
  auto data = core::ReadAllBytes(h1.value->get());
  ASSERT_TRUE(data) << core::FormatWin32Error(data.error);
  EXPECT_EQ(*data.value, "hello_world");

  h1.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(dst, "hello_world"));
}

// Spec: [MS-FSA] 2.1.5.1.2.2 & 2.1.5.15.12 -- an open handle lacking
// FILE_SHARE_DELETE blocks MoveFileExW with ERROR_SHARING_VIOLATION.
TEST_F(RenameMoveTest, RenameWhileOpenWithoutShareDeleteFails) {
  const std::wstring src = TestPath(L"noshare_src.dat");
  const std::wstring dst = TestPath(L"noshare_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "locked"));

  auto h1 = core::CreateWin32File(src, GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  EXPECT_WIN32_ERROR(::MoveFileExW(src.c_str(), dst.c_str(), 0),
                     ERROR_SHARING_VIOLATION);

  EXPECT_TRUE(core::QueryWin32Attributes(src));
  EXPECT_TRUE(helpers::CheckFileNotFound(dst));

  h1.value->reset();
  EXPECT_WIN32_TRUE(::MoveFileExW(src.c_str(), dst.c_str(), 0));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- renaming a directory with an open descendant
// file handle fails ERROR_ACCESS_DENIED until the descendant handle closes.
TEST_F(RenameMoveTest, RenameDirectoryWithOpenDescendantFails) {
  const std::wstring top = TestPath(L"desc_top");
  const std::wstring sub = TestPath(L"desc_top\\sub");
  const std::wstring child = TestPath(L"desc_top\\sub\\child.dat");
  const std::wstring renamed = TestPath(L"desc_top_renamed");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(top.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(child, "descendant_data"));

  auto h_child = core::CreateWin32File(child, GENERIC_READ, core::kShareAll,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_child) << core::FormatWin32Error(h_child.error);

  EXPECT_WIN32_ERROR(::MoveFileExW(top.c_str(), renamed.c_str(), 0),
                     ERROR_ACCESS_DENIED);
  EXPECT_TRUE(core::QueryWin32Attributes(top));
  EXPECT_TRUE(helpers::CheckFileNotFound(renamed));

  h_child.value->reset();

  ASSERT_WIN32_TRUE(::MoveFileExW(top.c_str(), renamed.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileContents(
      TestPath(L"desc_top_renamed\\sub\\child.dat"), "descendant_data"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- moving a directory into itself or a descendant
// fails (ACCESS_DENIED / SHARING_VIOLATION, or INVALID_PARAMETER on 24H2+).
TEST_F(RenameMoveTest, RenameDirectoryIntoOwnSubtreeFails) {
  const std::wstring tree = TestPath(L"self_tree");
  const std::wstring sub = TestPath(L"self_tree\\sub");
  const std::wstring keep = TestPath(L"self_tree\\sub\\keep.dat");
  const std::wstring bad_deep = TestPath(L"self_tree\\sub\\moved");
  const std::wstring bad_direct = TestPath(L"self_tree\\moved");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(tree.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(keep, "intact"));

  // On NTFS, moving a directory into a deeper descendant fails
  // ERROR_ACCESS_DENIED (open target-parent descendant), and moving into itself
  // as a direct child fails ERROR_SHARING_VIOLATION (Win10/Server 2022) or
  // ERROR_INVALID_PARAMETER (Win11 24H2/Server 2025, per [MS-FSA]
  // STATUS_INVALID_PARAMETER).
  //
  // OS-build threshold provenance: ERROR_SHARING_VIOLATION was observed on
  // Windows Server 2022 (build 20348) and ERROR_INVALID_PARAMETER on Windows
  // Server 2025 / Windows 11 24H2 (build 26100), the two NTFS hosts this suite
  // runs on in CI. The change is undocumented and builds in between (Windows 11
  // 21H2-23H2, 22000-22631) have not been measured; kBuildWin11_24H2 is the
  // lowest build on which the new status is confirmed. If a 22xxx host fails
  // here, lower the threshold rather than accepting both codes.
  EXPECT_WIN32_ERROR(::MoveFileExW(tree.c_str(), bad_deep.c_str(), 0),
                     ERROR_ACCESS_DENIED);
  const DWORD expected_direct_err =
      (Os().build >= core::kBuildWin11_24H2)
          ? static_cast<DWORD>(ERROR_INVALID_PARAMETER)
          : static_cast<DWORD>(ERROR_SHARING_VIOLATION);
  EXPECT_WIN32_ERROR(::MoveFileExW(tree.c_str(), bad_direct.c_str(), 0),
                     expected_direct_err);

  EXPECT_TRUE(helpers::CheckFileContents(keep, "intact"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- replacing an existing empty or non-empty
// directory with a regular file fails ERROR_ACCESS_DENIED.
TEST_F(RenameMoveTest, RenameFileOverExistingDirectoryFails) {
  const std::wstring file = TestPath(L"src_file.dat");
  const std::wstring existing_dir = TestPath(L"target_dir");
  const std::wstring inside = TestPath(L"target_dir\\inside.dat");
  const std::wstring empty_dir = TestPath(L"empty_target_dir");
  ASSERT_TRUE(core::WriteFileContents(file, "file_bytes"));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(existing_dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(inside, "dir_child"));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(empty_dir.c_str(), nullptr));

  EXPECT_WIN32_ERROR(::MoveFileExW(file.c_str(), existing_dir.c_str(),
                                   MOVEFILE_REPLACE_EXISTING),
                     ERROR_ACCESS_DENIED);
  EXPECT_WIN32_ERROR(
      ::MoveFileExW(file.c_str(), empty_dir.c_str(), MOVEFILE_REPLACE_EXISTING),
      ERROR_ACCESS_DENIED);

  EXPECT_TRUE(helpers::CheckFileContents(file, "file_bytes"));
  EXPECT_TRUE(helpers::CheckFileContents(inside, "dir_child"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- replacing a file with a directory fails if
// REPLACE_EXISTING is unset or target is READONLY, and succeeds when writable.
TEST_F(RenameMoveTest, RenameDirectoryOverExistingFileSemantics) {
  const std::wstring src_dir = TestPath(L"src_dir");
  const std::wstring dir_item = TestPath(L"src_dir\\item.dat");
  const std::wstring target_file = TestPath(L"existing_file.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(src_dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(dir_item, "dir_item"));
  ASSERT_TRUE(core::WriteFileContents(target_file, "file_stays"));

  // Without MOVEFILE_REPLACE_EXISTING, colliding with an existing file fails
  // ERROR_ALREADY_EXISTS and leaves both intact.
  EXPECT_WIN32_ERROR(::MoveFileExW(src_dir.c_str(), target_file.c_str(), 0),
                     ERROR_ALREADY_EXISTS);

  // Over a READONLY target file, even MOVEFILE_REPLACE_EXISTING fails
  // ERROR_ACCESS_DENIED.
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(target_file.c_str(), FILE_ATTRIBUTE_READONLY));
  EXPECT_WIN32_ERROR(::MoveFileExW(src_dir.c_str(), target_file.c_str(),
                                   MOVEFILE_REPLACE_EXISTING),
                     ERROR_ACCESS_DENIED);
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(target_file.c_str(), FILE_ATTRIBUTE_NORMAL));

  // Per [MS-FSA] 2.1.5.15.12, NTFS allows replacing a writable regular file
  // with a directory when MOVEFILE_REPLACE_EXISTING is set.
  ASSERT_WIN32_TRUE(::MoveFileExW(src_dir.c_str(), target_file.c_str(),
                                  MOVEFILE_REPLACE_EXISTING));
  auto attrs = core::QueryWin32Attributes(target_file);
  ASSERT_TRUE(attrs) << core::FormatWin32Error(attrs.error);
  EXPECT_NE(*attrs.value & FILE_ATTRIBUTE_DIRECTORY, 0UL);
  EXPECT_TRUE(
      helpers::CheckFileContents(target_file + L"\\item.dat", "dir_item"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- renaming a file to its own existing path is a
// no-op preserving FileIndex and data with or without REPLACE_EXISTING.
TEST_F(RenameMoveTest, RenameSameNameNoOp) {
  const std::wstring path = TestPath(L"same_name.dat");
  ASSERT_TRUE(core::WriteFileContents(path, "same_content"));

  auto orig_idx = core::QueryPathFileIndex(path);
  ASSERT_TRUE(orig_idx) << core::FormatWin32Error(orig_idx.error);

  ASSERT_WIN32_TRUE(::MoveFileExW(path.c_str(), path.c_str(), 0));
  auto idx1 = core::QueryPathFileIndex(path);
  ASSERT_TRUE(idx1);
  EXPECT_EQ(*idx1.value, *orig_idx.value);
  EXPECT_TRUE(helpers::CheckFileContents(path, "same_content"));

  ASSERT_WIN32_TRUE(
      ::MoveFileExW(path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING));
  auto idx2 = core::QueryPathFileIndex(path);
  ASSERT_TRUE(idx2);
  EXPECT_EQ(*idx2.value, *orig_idx.value);
  EXPECT_TRUE(helpers::CheckFileContents(path, "same_content"));
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.42 -- FileRenameInformation with
// non-null RootDirectory moves the file relative to that directory handle.
TEST_F(RenameMoveTest, NtFileRenameInfoRootDirectoryRelative) {
  const std::wstring src_dir = TestPath(L"nt_ren_src");
  const std::wstring dst_dir = TestPath(L"nt_ren_dst");
  const std::wstring src_file = TestPath(L"nt_ren_src\\item.dat");
  const std::wstring dst_file = TestPath(L"nt_ren_dst\\moved_rel.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(src_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dst_dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(src_file, "relative_rename_bytes"));

  auto orig_idx = core::QueryPathFileIndex(src_file);
  ASSERT_TRUE(orig_idx) << core::FormatWin32Error(orig_idx.error);

  auto h_dst_dir = core::CreateWin32File(
      dst_dir,
      FILE_ADD_FILE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
      core::kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_TRUE(h_dst_dir) << core::FormatWin32Error(h_dst_dir.error);

  auto h_src =
      core::CreateWin32File(src_file, DELETE | SYNCHRONIZE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_src) << core::FormatWin32Error(h_src.error);

  auto ren = core::SetRenameInfo(h_src.value->get(), L"moved_rel.dat",
                                 h_dst_dir.value->get(),
                                 /*replace_if_exists=*/false);
  ASSERT_NT_STATUS(ren.status, STATUS_SUCCESS);

  h_src.value->reset();
  h_dst_dir.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(src_file));

  auto dst_idx = core::QueryPathFileIndex(dst_file);
  ASSERT_TRUE(dst_idx);
  EXPECT_EQ(*dst_idx.value, *orig_idx.value);
  EXPECT_TRUE(helpers::CheckFileContents(dst_file, "relative_rename_bytes"));
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.42 -- non-POSIX rename with
// ReplaceIfExists=TRUE fails STATUS_ACCESS_DENIED when target is open.
TEST_F(RenameMoveTest, FileRenameInfoReplaceOpenTargetNonPosixFails) {
  const std::wstring src = TestPath(L"nonposix_src.dat");
  const std::wstring dst = TestPath(L"nonposix_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "new_payload"));
  ASSERT_TRUE(core::WriteFileContents(dst, "old_payload"));

  auto h_target = core::CreateWin32File(dst, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_target) << core::FormatWin32Error(h_target.error);

  auto h_src = core::CreateWin32File(src, DELETE | SYNCHRONIZE, core::kShareAll,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_src) << core::FormatWin32Error(h_src.error);

  auto ren =
      core::SetRenameInfo(h_src.value->get(), TestNtPath(L"nonposix_dst.dat"),
                          nullptr, /*replace_if_exists=*/true);
  EXPECT_NT_STATUS(ren.status, STATUS_ACCESS_DENIED);

  h_src.value->reset();
  h_target.value->reset();

  EXPECT_TRUE(helpers::CheckFileContents(src, "new_payload"));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "old_payload"));
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.43 (Win10 1709+) -- POSIX rename
// replaces an open target while its existing handle retains original bytes.
TEST_F(RenameMoveTest, FileRenameInfoExPosixReplaceOpenTarget) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const std::wstring src = TestPath(L"posix_ren_src.dat");
  const std::wstring dst = TestPath(L"posix_ren_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "replacement_bytes"));
  ASSERT_TRUE(core::WriteFileContents(dst, "original_open_bytes"));

  auto h_target = core::CreateWin32File(dst, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_target) << core::FormatWin32Error(h_target.error);
  auto orig_idx = core::GetFileIndex(h_target.value->get());
  ASSERT_TRUE(orig_idx);

  auto h_src = core::CreateWin32File(
      src, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_src) << core::FormatWin32Error(h_src.error);
  auto src_idx = core::GetFileIndex(h_src.value->get());
  ASSERT_TRUE(src_idx);

  auto ren = core::SetRenameInfoEx(
      h_src.value->get(), TestNtPath(L"posix_ren_dst.dat"), nullptr,
      nt::kFileRenameReplaceIfExists | nt::kFileRenamePosixSemantics);
  ASSERT_NT_STATUS(ren.status, STATUS_SUCCESS);
  h_src.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(src));

  // New opens observe replacement bytes and source FileIndex.
  auto new_idx = core::QueryPathFileIndex(dst);
  ASSERT_TRUE(new_idx);
  EXPECT_EQ(*new_idx.value, *src_idx.value);
  EXPECT_NE(*new_idx.value, *orig_idx.value);
  EXPECT_TRUE(helpers::CheckFileContents(dst, "replacement_bytes"));

  // Already-open handle to replaced target still reads original bytes.
  auto old_data = core::ReadAllBytes(h_target.value->get());
  ASSERT_TRUE(old_data);
  EXPECT_EQ(*old_data.value, "original_open_bytes");

  h_target.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(dst, "replacement_bytes"));
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.43 (Win10 1809+) --
// FILE_RENAME_IGNORE_READONLY_ATTRIBUTE replaces a READONLY target directly.
TEST_F(RenameMoveTest, FileRenameInfoExIgnoreReadOnlyTarget) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  const std::wstring src = TestPath(L"ign_ro_src.dat");
  const std::wstring dst = TestPath(L"ign_ro_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "overwrite_ro_bytes"));
  ASSERT_TRUE(
      core::WriteFileContents(dst, "readonly_old", FILE_ATTRIBUTE_READONLY));

  auto h_src = core::CreateWin32File(src, DELETE | SYNCHRONIZE, core::kShareAll,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_src) << core::FormatWin32Error(h_src.error);

  // Without IGNORE_READONLY_ATTRIBUTE, replacing READONLY target fails.
  auto fail_ren =
      core::SetRenameInfoEx(h_src.value->get(), TestNtPath(L"ign_ro_dst.dat"),
                            nullptr, nt::kFileRenameReplaceIfExists);
  EXPECT_NT_STATUS(fail_ren.status, STATUS_ACCESS_DENIED);

  // With IGNORE_READONLY_ATTRIBUTE, replacing succeeds without stripping RO.
  auto ok_ren = core::SetRenameInfoEx(
      h_src.value->get(), TestNtPath(L"ign_ro_dst.dat"), nullptr,
      nt::kFileRenameReplaceIfExists | nt::kFileRenameIgnoreReadonlyAttribute);
  ASSERT_NT_STATUS(ok_ren.status, STATUS_SUCCESS);
  h_src.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "overwrite_ro_bytes"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- moving a non-empty directory subtree across
// parents preserves all descendant FileIndex values and file contents.
TEST_F(RenameMoveTest, MoveNonEmptyDirectorySubtreeAcrossParents) {
  const std::wstring parent_a = TestPath(L"sub_parent_a");
  const std::wstring parent_b = TestPath(L"sub_parent_b");
  const std::wstring tree = TestPath(L"sub_parent_a\\tree");
  const std::wstring nested = TestPath(L"sub_parent_a\\tree\\nested");
  const std::wstring root_file = TestPath(L"sub_parent_a\\tree\\root.dat");
  const std::wstring deep_file =
      TestPath(L"sub_parent_a\\tree\\nested\\deep.dat");

  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent_b.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(tree.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(nested.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(root_file, "root_payload"));
  ASSERT_TRUE(core::WriteFileContents(deep_file, "deep_payload"));

  auto idx_tree = core::QueryPathFileIndex(tree, /*backup_semantics=*/true);
  auto idx_nested = core::QueryPathFileIndex(nested, /*backup_semantics=*/true);
  auto idx_root = core::QueryPathFileIndex(root_file);
  auto idx_deep = core::QueryPathFileIndex(deep_file);
  ASSERT_TRUE(idx_tree);
  ASSERT_TRUE(idx_nested);
  ASSERT_TRUE(idx_root);
  ASSERT_TRUE(idx_deep);

  const std::wstring moved_tree = TestPath(L"sub_parent_b\\moved_tree");
  const std::wstring moved_nested =
      TestPath(L"sub_parent_b\\moved_tree\\nested");
  const std::wstring moved_root =
      TestPath(L"sub_parent_b\\moved_tree\\root.dat");
  const std::wstring moved_deep =
      TestPath(L"sub_parent_b\\moved_tree\\nested\\deep.dat");

  ASSERT_WIN32_TRUE(::MoveFileExW(tree.c_str(), moved_tree.c_str(), 0));

  EXPECT_TRUE(helpers::CheckFileNotFound(tree));
  EXPECT_TRUE(helpers::CheckDirEntries(parent_a, {L".", L".."}));
  EXPECT_TRUE(helpers::CheckDirEntries(parent_b, {L".", L"..", L"moved_tree"}));
  EXPECT_TRUE(helpers::CheckFileContents(moved_root, "root_payload"));
  EXPECT_TRUE(helpers::CheckFileContents(moved_deep, "deep_payload"));

  auto new_idx_tree =
      core::QueryPathFileIndex(moved_tree, /*backup_semantics=*/true);
  auto new_idx_nested =
      core::QueryPathFileIndex(moved_nested, /*backup_semantics=*/true);
  auto new_idx_root = core::QueryPathFileIndex(moved_root);
  auto new_idx_deep = core::QueryPathFileIndex(moved_deep);
  ASSERT_TRUE(new_idx_tree);
  ASSERT_TRUE(new_idx_nested);
  ASSERT_TRUE(new_idx_root);
  ASSERT_TRUE(new_idx_deep);
  EXPECT_EQ(*new_idx_tree.value, *idx_tree.value);
  EXPECT_EQ(*new_idx_nested.value, *idx_nested.value);
  EXPECT_EQ(*new_idx_root.value, *idx_root.value);
  EXPECT_EQ(*new_idx_deep.value, *idx_deep.value);
}

// Spec: [MS-FSA] 2.1.5.15.12 -- an open handle on a directory blocks rename
// without FILE_SHARE_DELETE and tracks the new path with FILE_SHARE_DELETE.
TEST_F(RenameMoveTest, RenameDirectoryWhileDirectoryHandleOpen) {
  const std::wstring dir = TestPath(L"dir_self_open");
  const std::wstring child = TestPath(L"dir_self_open\\child.dat");
  const std::wstring dst1 = TestPath(L"dir_self_blocked");
  const std::wstring dst2 = TestPath(L"dir_self_moved");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(child, "still_here"));

  // Open handle on directory itself WITHOUT FILE_SHARE_DELETE blocks rename.
  {
    auto h_locked =
        core::CreateWin32File(dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(h_locked) << core::FormatWin32Error(h_locked.error);

    EXPECT_WIN32_ERROR(::MoveFileExW(dir.c_str(), dst1.c_str(), 0),
                       ERROR_SHARING_VIOLATION);
  }

  // Open handle on directory itself WITH FILE_SHARE_DELETE allows rename.
  {
    auto h_shared = core::CreateWin32File(
        dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, core::kShareAll,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(h_shared) << core::FormatWin32Error(h_shared.error);

    ASSERT_WIN32_TRUE(::MoveFileExW(dir.c_str(), dst2.c_str(), 0));

    auto fp = core::QueryFinalPath(h_shared.value->get(),
                                   VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
    ASSERT_TRUE(fp) << core::FormatWin32Error(fp.error);
    EXPECT_TRUE(fp.value->ends_with(L"\\dir_self_moved"))
        << "Final path: " << core::WideToUtf8(*fp.value);
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(dir));
  EXPECT_TRUE(helpers::CheckFileContents(TestPath(L"dir_self_moved\\child.dat"),
                                         "still_here"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- renaming a delete-pending file fails via Win32
// (ERROR_ACCESS_DENIED) and NT FileRenameInformation (STATUS_ACCESS_DENIED).
TEST_F(RenameMoveTest, RenameDeletePendingFileFails) {
  const std::wstring src = TestPath(L"del_pend_src.dat");
  const std::wstring dst_a = TestPath(L"del_pend_dst_a.dat");
  const std::wstring dst_b = TestPath(L"del_pend_dst_b.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "doomed"));

  auto h1 = core::CreateWin32File(src, DELETE | SYNCHRONIZE, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h1) << core::FormatWin32Error(h1.error);

  auto disp = core::SetDisposition(h1.value->get(), /*delete_file=*/true);
  ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);

  EXPECT_WIN32_ERROR(::MoveFileExW(src.c_str(), dst_a.c_str(), 0),
                     ERROR_ACCESS_DENIED);

  auto nt_ren = core::SetRenameInfo(
      h1.value->get(), TestNtPath(L"del_pend_dst_b.dat"), nullptr, false);
  EXPECT_NT_STATUS(nt_ren.status, STATUS_ACCESS_DENIED);

  h1.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileNotFound(dst_a));
  EXPECT_TRUE(helpers::CheckFileNotFound(dst_b));
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.7 -- a cross-directory move
// updates LastWriteTime of both the source and the target parent directory.
TEST_F(RenameMoveTest, RenameUpdatesBothParentDirLastWriteTimes) {
  BackdatedRenameLayout layout;
  ASSERT_TRUE(PrepareBackdatedCrossDirRename(
      TestPath(L"ts_parent_src"), TestPath(L"ts_parent_dst"), &layout));

  auto pre_src_parent = helpers::QueryPathBasicInfo(layout.src_parent);
  ASSERT_TRUE(pre_src_parent) << pre_src_parent.Describe();
  ASSERT_EQ(pre_src_parent.value->last_write_time.QuadPart, layout.backdate);
  auto pre_dst_parent = helpers::QueryPathBasicInfo(layout.dst_parent);
  ASSERT_TRUE(pre_dst_parent) << pre_dst_parent.Describe();
  ASSERT_EQ(pre_dst_parent.value->last_write_time.QuadPart, layout.backdate);

  ASSERT_WIN32_TRUE(
      ::MoveFileExW(layout.src_file.c_str(), layout.dst_file.c_str(), 0));

  auto post_src_parent = helpers::QueryPathBasicInfo(layout.src_parent);
  ASSERT_TRUE(post_src_parent) << post_src_parent.Describe();
  EXPECT_GT(post_src_parent.value->last_write_time.QuadPart, layout.backdate);

  auto post_dst_parent = helpers::QueryPathBasicInfo(layout.dst_parent);
  ASSERT_TRUE(post_dst_parent) << post_dst_parent.Describe();
  EXPECT_GT(post_dst_parent.value->last_write_time.QuadPart, layout.backdate);
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.7 -- renaming a file updates its
// ChangeTime.
TEST_F(RenameMoveTest, RenameUpdatesFileChangeTime) {
  BackdatedRenameLayout layout;
  ASSERT_TRUE(PrepareBackdatedCrossDirRename(
      TestPath(L"ts_change_src"), TestPath(L"ts_change_dst"), &layout));

  auto pre_file = helpers::QueryPathBasicInfo(layout.src_file);
  ASSERT_TRUE(pre_file) << pre_file.Describe();
  ASSERT_EQ(pre_file.value->change_time.QuadPart, layout.backdate);

  ASSERT_WIN32_TRUE(
      ::MoveFileExW(layout.src_file.c_str(), layout.dst_file.c_str(), 0));

  auto post_file = helpers::QueryPathBasicInfo(layout.dst_file);
  ASSERT_TRUE(post_file) << post_file.Describe();
  EXPECT_GT(post_file.value->change_time.QuadPart, layout.backdate);
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.7 -- renaming a file preserves
// its CreationTime and LastWriteTime.
TEST_F(RenameMoveTest, RenamePreservesFileCreationAndLastWriteTimes) {
  BackdatedRenameLayout layout;
  ASSERT_TRUE(PrepareBackdatedCrossDirRename(
      TestPath(L"ts_keep_src"), TestPath(L"ts_keep_dst"), &layout));

  auto pre_file = helpers::QueryPathBasicInfo(layout.src_file);
  ASSERT_TRUE(pre_file) << pre_file.Describe();
  ASSERT_EQ(pre_file.value->last_write_time.QuadPart, layout.backdate);
  ASSERT_EQ(pre_file.value->creation_time.QuadPart, layout.backdate);

  ASSERT_WIN32_TRUE(
      ::MoveFileExW(layout.src_file.c_str(), layout.dst_file.c_str(), 0));

  auto post_file = helpers::QueryPathBasicInfo(layout.dst_file);
  ASSERT_TRUE(post_file) << post_file.Describe();
  EXPECT_EQ(post_file.value->last_write_time.QuadPart, layout.backdate);
  EXPECT_EQ(post_file.value->creation_time.QuadPart, layout.backdate);
}

// Spec: Win32 MoveFileExW / [MS-FSA] 2.1.5.15.12 -- replacing a data-mapped
// target with no open file handles succeeds while the view keeps old bytes.
TEST_F(RenameMoveTest, RenameOverDataMappedTargetPreservesMappedView) {
  const std::wstring src = TestPath(L"map_ren_src.dat");
  const std::wstring target = TestPath(L"map_ren_target.dat");
  const std::string src_bytes = "replacement_content";
  const std::string tgt_bytes = "mapped_target_content";
  ASSERT_TRUE(core::WriteFileContents(src, src_bytes));
  ASSERT_TRUE(core::WriteFileContents(target, tgt_bytes));

  auto h_target = core::CreateWin32File(target, GENERIC_READ | GENERIC_WRITE,
                                        core::kShareAll, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_target) << core::FormatWin32Error(h_target.error);

  // Declare h_map before view so RAII unmaps view before closing h_map.
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle h_map(::CreateFileMappingW(h_target.value->get(), nullptr,
                                                PAGE_READWRITE, 0, 0, nullptr));
  const DWORD map_err = ::GetLastError();
  ASSERT_TRUE(h_map.valid()) << core::FormatWin32Error(map_err);

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueViewOfFile view(
      ::MapViewOfFile(h_map.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
  const DWORD view_err = ::GetLastError();
  ASSERT_TRUE(view) << core::FormatWin32Error(view_err);

  // Close file handle so only the active data-mapped view pins the old target.
  h_target.value->reset();

  // On NTFS, MoveFileExW(REPLACE_EXISTING) replaces a data-mapped target when
  // no open file handles remain: target gets the replacement bytes while the
  // existing mapped view continues to observe the original content.
  ASSERT_WIN32_TRUE(
      ::MoveFileExW(src.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING));

  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(target, src_bytes));

  const std::string mapped_bytes(static_cast<const char *>(view.get()),
                                 tgt_bytes.size());
  EXPECT_EQ(mapped_bytes, tgt_bytes);

  view.reset();
  h_map.reset();
}

// Spec: Win32 ReplaceFileW -- atomically swaps replacement over target, writes
// old target content to backup, and preserves the target's CreationTime.
TEST_F(RenameMoveTest, ReplaceFileWAtomicSwapPreservesCreationTimeAndBackup) {
  const std::wstring target = TestPath(L"replace_target.dat");
  const std::wstring replacement = TestPath(L"replace_new.dat");
  const std::wstring backup = TestPath(L"replace_backup.dat");

  ASSERT_TRUE(core::WriteFileContents(target, "original_target_v1"));
  ASSERT_TRUE(core::WriteFileContents(replacement, "new_replacement_v2"));

  const int64_t orig_ctime = core::MakeUtcFileTimeInt64(2020, 1, 1);
  const int64_t repl_ctime = core::MakeUtcFileTimeInt64(2021, 6, 15, 12, 0, 0);
  ASSERT_TRUE(fstest::BackdateAllTimes(target, orig_ctime));
  ASSERT_TRUE(fstest::BackdateAllTimes(replacement, repl_ctime));

  ASSERT_WIN32_TRUE(::ReplaceFileW(target.c_str(), replacement.c_str(),
                                   backup.c_str(), 0, nullptr, nullptr));

  EXPECT_TRUE(helpers::CheckFileNotFound(replacement));
  EXPECT_TRUE(helpers::CheckFileContents(target, "new_replacement_v2"));
  EXPECT_TRUE(helpers::CheckFileContents(backup, "original_target_v1"));

  auto target_info = helpers::QueryPathBasicInfo(target);
  ASSERT_TRUE(target_info) << target_info.Describe();
  EXPECT_EQ(target_info.value->creation_time.QuadPart, orig_ctime);
}

// Spec: [MS-FSA] 2.1.5.15.12 & directory enumeration -- concurrent MoveFileExW
// renames and FindFirstFile/FindNextFile scans yield no errors or duplicates.
TEST_F(RenameMoveTest, ConcurrentRenameAndEnumerateStress) {
  const std::wstring sub = TestPath(L"concurrent_ren_enum");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring w0_a = TestPath(L"concurrent_ren_enum\\ren_w0_a.dat");
  const std::wstring w0_b = TestPath(L"concurrent_ren_enum\\ren_w0_b.dat");
  const std::wstring w1_a = TestPath(L"concurrent_ren_enum\\ren_w1_a.dat");
  const std::wstring w1_b = TestPath(L"concurrent_ren_enum\\ren_w1_b.dat");
  ASSERT_TRUE(core::WriteFileContents(w0_a, "w0_payload"));
  ASSERT_TRUE(core::WriteFileContents(w1_a, "w1_payload"));

  // All worker state is heap-allocated and co-owned by each ScopedThread, so a
  // worker that is still running after a Join() timeout keeps its context (and
  // the start event) alive instead of reading this stack frame.
  auto shared = std::make_shared<RenameEnumStressShared>();
  ::SetLastError(ERROR_SUCCESS);
  shared->start_event.reset(::CreateEventW(nullptr, /*bManualReset=*/TRUE,
                                           /*bInitialState=*/FALSE, nullptr));
  const DWORD event_err = ::GetLastError();
  ASSERT_TRUE(shared->start_event.valid()) << core::FormatWin32Error(event_err);

  auto make_worker_ctx = [&shared](const std::wstring &path_a,
                                   const std::wstring &path_b, int worker_id) {
    auto ctx = std::make_shared<RenameWorkerContext>();
    ctx->shared = shared;
    ctx->path_a = path_a;
    ctx->path_b = path_b;
    ctx->worker_id = worker_id;
    ctx->iterations = 25;
    return ctx;
  };
  auto reader_ctx = std::make_shared<EnumReaderContext>();
  reader_ctx->shared = shared;
  reader_ctx->dir_path = sub;
  reader_ctx->iterations = 40;

  ScopedThread worker0;
  ScopedThread worker1;
  ScopedThread reader;

  const bool ok0 =
      worker0.Start(&RenameStressWorkerProc, make_worker_ctx(w0_a, w0_b, 0));
  const bool ok1 =
      worker1.Start(&RenameStressWorkerProc, make_worker_ctx(w1_a, w1_b, 1));
  const bool okr = reader.Start(&EnumStressReaderProc, reader_ctx);

  // Release any started threads before asserting Start() success so no worker
  // stalls waiting on start_event during early return.
  ASSERT_WIN32_TRUE(::SetEvent(shared->start_event.get()));
  ASSERT_TRUE(ok0) << core::FormatWin32Error(worker0.LastError());
  ASSERT_TRUE(ok1) << core::FormatWin32Error(worker1.LastError());
  ASSERT_TRUE(okr) << core::FormatWin32Error(reader.LastError());

  // Explicitly join every worker, and only inspect results once all three
  // have exited: a worker that is still running may still be recording errors
  // and renaming files.
  const bool joined0 = worker0.Join(30000);
  const DWORD join_err0 = worker0.LastError();
  const bool joined1 = worker1.Join(30000);
  const DWORD join_err1 = worker1.LastError();
  const bool joinedr = reader.Join(30000);
  const DWORD join_errr = reader.LastError();
  ASSERT_TRUE(joined0) << "rename worker 0 did not exit: "
                       << core::FormatWin32Error(join_err0);
  ASSERT_TRUE(joined1) << "rename worker 1 did not exit: "
                       << core::FormatWin32Error(join_err1);
  ASSERT_TRUE(joinedr) << "enumeration reader did not exit: "
                       << core::FormatWin32Error(join_errr);

  std::string combined_errors;
  {
    fstest::ScopedCsLock lock(shared->cs);
    for (const auto &err : shared->errors) {
      if (!combined_errors.empty()) {
        combined_errors += "\n";
      }
      combined_errors += err;
    }
  }
  EXPECT_EQ(shared->error_count.load(std::memory_order_acquire), 0)
      << combined_errors;

  EXPECT_TRUE(helpers::CheckDirEntries(
      sub, {L".", L"..", L"ren_w0_a.dat", L"ren_w1_a.dat"}));
  EXPECT_TRUE(helpers::CheckFileContents(w0_a, "w0_payload"));
  EXPECT_TRUE(helpers::CheckFileContents(w1_a, "w1_payload"));
}

// Spec: [MS-FSA] 2.1.5.15.12 / Win32 MoveFileExW & CreateHardLinkW -- renaming
// one hard link onto a sibling hard link of the same file (with or without
// MOVEFILE_REPLACE_EXISTING) unlinks the source link and decrements
// NumberOfLinks to 1 while preserving FileIndex and data.
TEST_F(RenameMoveTest, RenameHardLinkOntoSiblingHardLink) {
  REQUIRE_FS_CAPABILITY(FsCapability::kHardLinks);

  // Part 1: MOVEFILE_REPLACE_EXISTING on sibling hard links.
  {
    const std::wstring link_a = TestPath(L"hl_ren_a.dat");
    const std::wstring link_b = TestPath(L"hl_ren_b.dat");
    ASSERT_TRUE(core::WriteFileContents(link_a, "hardlink_rename_payload"));
    ASSERT_WIN32_TRUE(
        ::CreateHardLinkW(link_b.c_str(), link_a.c_str(), nullptr));

    auto info_a = core::QueryInfoByPath(link_a);
    auto info_b = core::QueryInfoByPath(link_b);
    ASSERT_TRUE(info_a) << core::FormatWin32Error(info_a.error);
    ASSERT_TRUE(info_b) << core::FormatWin32Error(info_b.error);
    const uint64_t orig_index = core::FileIndexOf(*info_a.value);
    EXPECT_EQ(core::FileIndexOf(*info_b.value), orig_index);
    EXPECT_EQ(info_a.value->nNumberOfLinks, 2UL);

    ASSERT_WIN32_TRUE(::MoveFileExW(link_a.c_str(), link_b.c_str(),
                                    MOVEFILE_REPLACE_EXISTING));

    EXPECT_TRUE(helpers::CheckFileNotFound(link_a));
    auto info_after = core::QueryInfoByPath(link_b);
    ASSERT_TRUE(info_after) << core::FormatWin32Error(info_after.error);
    EXPECT_EQ(core::FileIndexOf(*info_after.value), orig_index);
    EXPECT_EQ(info_after.value->nNumberOfLinks, 1UL);
    EXPECT_TRUE(helpers::CheckFileContents(link_b, "hardlink_rename_payload"));
  }

  // Part 2: Without MOVEFILE_REPLACE_EXISTING, NTFS still allows renaming a
  // link onto another link of the same file (same FCB), unlinking the source.
  {
    const std::wstring link_c = TestPath(L"hl_ren_c.dat");
    const std::wstring link_d = TestPath(L"hl_ren_d.dat");
    ASSERT_TRUE(core::WriteFileContents(link_c, "hardlink_same_fcb_payload"));
    ASSERT_WIN32_TRUE(
        ::CreateHardLinkW(link_d.c_str(), link_c.c_str(), nullptr));

    auto info_c = core::QueryInfoByPath(link_c);
    ASSERT_TRUE(info_c) << core::FormatWin32Error(info_c.error);
    const uint64_t orig_index = core::FileIndexOf(*info_c.value);

    ASSERT_WIN32_TRUE(::MoveFileExW(link_c.c_str(), link_d.c_str(), 0));
    EXPECT_TRUE(helpers::CheckFileNotFound(link_c));
    auto info_after = core::QueryInfoByPath(link_d);
    ASSERT_TRUE(info_after) << core::FormatWin32Error(info_after.error);
    EXPECT_EQ(core::FileIndexOf(*info_after.value), orig_index);
    EXPECT_EQ(info_after.value->nNumberOfLinks, 1UL);
    EXPECT_TRUE(
        helpers::CheckFileContents(link_d, "hardlink_same_fcb_payload"));
  }
}

// Spec: [MS-FSA] 2.1.5.15.12 / Win32 MoveFileExW & FileRenameInformation --
// renaming a file preserves its default stream and all alternate data streams.
TEST_F(RenameMoveTest, RenamePreservesAlternateDataStreams) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring sub = TestPath(L"ads_ren_sub");
  const std::wstring src = TestPath(L"ads_ren_src.dat");
  const std::wstring mid = TestPath(L"ads_ren_mid.dat");
  const std::wstring final_path = TestPath(L"ads_ren_sub\\ads_ren_final.dat");
  const std::string main_data = "default_stream_data";
  const std::string ads1_data = "first_ads_payload";
  const std::string ads2_data = "second_ads_longer_payload";
  const std::map<std::wstring, int64_t> expected_streams = {
      {L"::$DATA", static_cast<int64_t>(main_data.size())},
      {L":ads1:$DATA", static_cast<int64_t>(ads1_data.size())},
      {L":ads2:$DATA", static_cast<int64_t>(ads2_data.size())},
  };

  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(src, main_data));
  ASSERT_TRUE(core::WriteFileContents(src + L":ads1", ads1_data));
  ASSERT_TRUE(core::WriteFileContents(src + L":ads2", ads2_data));
  ASSERT_TRUE(CheckStreamsWithSizes(src, expected_streams));

  // Same-directory rename via Win32 MoveFileExW preserves all streams.
  ASSERT_WIN32_TRUE(::MoveFileExW(src.c_str(), mid.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(mid, main_data));
  EXPECT_TRUE(helpers::CheckFileContents(mid + L":ads1", ads1_data));
  EXPECT_TRUE(helpers::CheckFileContents(mid + L":ads2", ads2_data));
  EXPECT_TRUE(CheckStreamsWithSizes(mid, expected_streams));

  // Cross-directory move via native NT FileRenameInformation preserves all
  // streams.
  {
    auto h_mid =
        core::CreateWin32File(mid, DELETE | SYNCHRONIZE, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h_mid) << core::FormatWin32Error(h_mid.error);
    auto ren = core::SetRenameInfo(
        h_mid.value->get(), TestNtPath(L"ads_ren_sub\\ads_ren_final.dat"),
        nullptr, /*replace_if_exists=*/false);
    ASSERT_NT_STATUS(ren.status, STATUS_SUCCESS);
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(mid));
  EXPECT_TRUE(helpers::CheckFileContents(final_path, main_data));
  EXPECT_TRUE(helpers::CheckFileContents(final_path + L":ads1", ads1_data));
  EXPECT_TRUE(helpers::CheckFileContents(final_path + L":ads2", ads2_data));
  EXPECT_TRUE(CheckStreamsWithSizes(final_path, expected_streams));
}

// Spec: [MS-FSA] 2.1.5.15.12 / [MS-FSCC] 2.4.43 (Win10 1809+) --
// FileRenameInformationEx with POSIX_SEMANTICS | REPLACE_IF_EXISTS replaces a
// target that has both an open handle and an active data-mapped view while
// keeping the old mapped view and open handle intact.
TEST_F(RenameMoveTest, PosixRenameReplacesDataMappedTarget) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);

  const std::wstring src = TestPath(L"posix_map_src.dat");
  const std::wstring target = TestPath(L"posix_map_target.dat");
  const std::wstring nt_target = TestNtPath(L"posix_map_target.dat");
  const std::string src_bytes = "posix_replacement_bytes";
  const std::string tgt_bytes = "posix_mapped_old_target";
  ASSERT_TRUE(core::WriteFileContents(src, src_bytes));
  ASSERT_TRUE(core::WriteFileContents(target, tgt_bytes));

  auto h_target = core::CreateWin32File(target, GENERIC_READ | GENERIC_WRITE,
                                        core::kShareAll, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_target) << core::FormatWin32Error(h_target.error);
  auto orig_idx = core::GetFileIndex(h_target.value->get());
  ASSERT_TRUE(orig_idx) << core::FormatWin32Error(orig_idx.error);

  // Declare h_map before view so RAII unmaps view before closing h_map.
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle h_map(::CreateFileMappingW(h_target.value->get(), nullptr,
                                                PAGE_READWRITE, 0, 0, nullptr));
  const DWORD map_err = ::GetLastError();
  ASSERT_TRUE(h_map.valid()) << core::FormatWin32Error(map_err);

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueViewOfFile view(
      ::MapViewOfFile(h_map.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
  const DWORD view_err = ::GetLastError();
  ASSERT_TRUE(view) << core::FormatWin32Error(view_err);

  auto h_src = core::CreateWin32File(
      src, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_src) << core::FormatWin32Error(h_src.error);
  auto src_idx = core::GetFileIndex(h_src.value->get());
  ASSERT_TRUE(src_idx) << core::FormatWin32Error(src_idx.error);

  // Non-POSIX replace fails while target handle is open.
  auto non_posix = core::SetRenameInfo(h_src.value->get(), nt_target, nullptr,
                                       /*replace_if_exists=*/true);
  ASSERT_NT_STATUS(non_posix.status, STATUS_ACCESS_DENIED);

  // POSIX rename replaces the open + data-mapped target cleanly.
  auto posix_ren = core::SetRenameInfoEx(h_src.value->get(), nt_target, nullptr,
                                         nt::kFileRenameReplaceIfExists |
                                             nt::kFileRenamePosixSemantics);
  ASSERT_NT_STATUS(posix_ren.status, STATUS_SUCCESS);
  h_src.value->reset();

  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  auto new_idx = core::QueryPathFileIndex(target);
  ASSERT_TRUE(new_idx) << core::FormatWin32Error(new_idx.error);
  EXPECT_EQ(*new_idx.value, *src_idx.value);
  EXPECT_NE(*new_idx.value, *orig_idx.value);
  EXPECT_TRUE(helpers::CheckFileContents(target, src_bytes));

  // Existing mapped view and open handle retain the unlinked original bytes.
  const std::string mapped_bytes(static_cast<const char *>(view.get()),
                                 tgt_bytes.size());
  EXPECT_EQ(mapped_bytes, tgt_bytes);

  ::SetLastError(ERROR_SUCCESS);
  const bool sought = fstest::SeekBegin(h_target.value->get(), 0);
  const DWORD seek_err = ::GetLastError();
  ASSERT_TRUE(sought) << core::FormatWin32Error(seek_err);
  auto old_read = core::ReadAllBytes(h_target.value->get());
  ASSERT_TRUE(old_read) << core::FormatWin32Error(old_read.error);
  EXPECT_EQ(*old_read.value, tgt_bytes);

  view.reset();
  h_map.reset();
  h_target.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(target, src_bytes));
}

// Spec: Win32 MoveFileExW / [MS-FSA] 2.1.5.1 -- renaming a source whose final
// component does not exist fails ERROR_FILE_NOT_FOUND, a source below a
// missing directory fails ERROR_PATH_NOT_FOUND, and no target is created.
TEST_F(RenameMoveTest, RenameMissingSourceFailsNotFound) {
  const std::wstring dir = TestPath(L"missing_src_dir");
  const std::wstring missing_file = TestPath(L"missing_src_dir\\absent.dat");
  const std::wstring missing_parent_file =
      TestPath(L"missing_src_dir\\absent_sub\\absent.dat");
  const std::wstring dst = TestPath(L"missing_src_dir\\dst.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  EXPECT_WIN32_ERROR(::MoveFileExW(missing_file.c_str(), dst.c_str(), 0),
                     ERROR_FILE_NOT_FOUND);
  EXPECT_WIN32_ERROR(::MoveFileExW(missing_parent_file.c_str(), dst.c_str(), 0),
                     ERROR_PATH_NOT_FOUND);

  EXPECT_TRUE(helpers::CheckFileNotFound(dst));
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L".."}));
}

// Spec: Win32 MoveFileExW / [MS-FSA] 2.1.5.15.12 -- renaming into a target
// parent directory that does not exist fails ERROR_PATH_NOT_FOUND
// (STATUS_OBJECT_PATH_NOT_FOUND), creates nothing, and leaves the source
// intact.
TEST_F(RenameMoveTest, RenameMissingTargetParentFailsPathNotFound) {
  const std::wstring src = TestPath(L"no_parent_src.dat");
  const std::wstring missing_parent = TestPath(L"no_such_parent");
  const std::wstring dst = TestPath(L"no_such_parent\\moved.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "stay_put"));

  EXPECT_WIN32_ERROR(::MoveFileExW(src.c_str(), dst.c_str(), 0),
                     ERROR_PATH_NOT_FOUND);
  EXPECT_WIN32_ERROR(
      ::MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING),
      ERROR_PATH_NOT_FOUND);

  {
    auto h_src =
        core::CreateWin32File(src, DELETE | SYNCHRONIZE, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h_src) << core::FormatWin32Error(h_src.error);
    auto nt_ren = core::SetRenameInfo(h_src.value->get(),
                                      TestNtPath(L"no_such_parent\\moved.dat"),
                                      nullptr, /*replace_if_exists=*/false);
    EXPECT_NT_STATUS(nt_ren.status, STATUS_OBJECT_PATH_NOT_FOUND);
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(missing_parent));
  EXPECT_TRUE(helpers::CheckFileContents(src, "stay_put"));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- FILE_ATTRIBUTE_READONLY on the source does not
// block a rename: the file moves, keeps READONLY, its FileIndex, and its data.
TEST_F(RenameMoveTest, RenameReadOnlySourceSucceedsAndKeepsReadOnly) {
  const std::wstring dir = TestPath(L"ro_src_dir");
  const std::wstring other_dir = TestPath(L"ro_src_other_dir");
  const std::wstring src = TestPath(L"ro_src_dir\\readonly.dat");
  const std::wstring same_dir_dst = TestPath(L"ro_src_dir\\renamed.dat");
  const std::wstring cross_dir_dst = TestPath(L"ro_src_other_dir\\moved.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(other_dir.c_str(), nullptr));
  ASSERT_TRUE(core::WriteFileContents(src, "readonly_payload",
                                      FILE_ATTRIBUTE_READONLY));

  auto orig_idx = core::QueryPathFileIndex(src);
  ASSERT_TRUE(orig_idx) << core::FormatWin32Error(orig_idx.error);

  // Same-directory rename.
  ASSERT_WIN32_TRUE(::MoveFileExW(src.c_str(), same_dir_dst.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  auto attrs1 = core::QueryWin32Attributes(same_dir_dst);
  ASSERT_TRUE(attrs1) << core::FormatWin32Error(attrs1.error);
  EXPECT_NE(*attrs1.value & FILE_ATTRIBUTE_READONLY, 0UL);

  // Cross-directory move.
  ASSERT_WIN32_TRUE(
      ::MoveFileExW(same_dir_dst.c_str(), cross_dir_dst.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(same_dir_dst));
  auto attrs2 = core::QueryWin32Attributes(cross_dir_dst);
  ASSERT_TRUE(attrs2) << core::FormatWin32Error(attrs2.error);
  EXPECT_NE(*attrs2.value & FILE_ATTRIBUTE_READONLY, 0UL);

  auto new_idx = core::QueryPathFileIndex(cross_dir_dst);
  ASSERT_TRUE(new_idx) << core::FormatWin32Error(new_idx.error);
  EXPECT_EQ(*new_idx.value, *orig_idx.value);
  EXPECT_TRUE(helpers::CheckFileContents(cross_dir_dst, "readonly_payload"));

  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(cross_dir_dst.c_str(), FILE_ATTRIBUTE_NORMAL));
}

// Spec: [MS-FSA] 2.1.5.15.12 -- a delete-pending file still occupies its name:
// MoveFileExW(REPLACE_EXISTING) onto it fails ERROR_ACCESS_DENIED and leaves
// the source intact; once the pending delete completes the rename succeeds.
TEST_F(RenameMoveTest, RenameOntoDeletePendingTargetFailsAccessDenied) {
  const std::wstring src = TestPath(L"dp_target_src.dat");
  const std::wstring target = TestPath(L"dp_target_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "replacement_payload"));
  ASSERT_TRUE(core::WriteFileContents(target, "doomed_target"));

  auto h_target =
      core::CreateWin32File(target, DELETE | SYNCHRONIZE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(h_target) << core::FormatWin32Error(h_target.error);
  auto disp = core::SetDisposition(h_target.value->get(), /*delete_file=*/true);
  ASSERT_NT_STATUS(disp.status, STATUS_SUCCESS);

  EXPECT_WIN32_ERROR(
      ::MoveFileExW(src.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING),
      ERROR_ACCESS_DENIED);
  EXPECT_TRUE(helpers::CheckFileContents(src, "replacement_payload"));

  // Closing the last handle completes the pending delete and frees the name.
  h_target.value->reset();
  EXPECT_TRUE(helpers::CheckFileNotFound(target));

  ASSERT_WIN32_TRUE(::MoveFileExW(src.c_str(), target.c_str(), 0));
  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(target, "replacement_payload"));
}

// Spec: Win32 MoveFileExW / [MS-FSA] 2.1.5.15.12 -- a rename whose target is
// on another volume fails ERROR_NOT_SAME_DEVICE (STATUS_NOT_SAME_DEVICE) when
// MOVEFILE_COPY_ALLOWED is not set, leaving the source intact and creating no
// target. (The cross-device check itself is made by the I/O manager; this is
// the control for the MOVEFILE_COPY_ALLOWED tests below.)
TEST_F(RenameMoveTest, CrossVolumeMoveWithoutCopyAllowedFailsNotSameDevice) {
  OtherVolumeScratch other;
  std::string skip_reason;
  if (!AcquireOtherVolumeScratch(TempDir().Path(), &other, &skip_reason)) {
    RecordSkipCategory("volume_policy");
    GTEST_SKIP() << skip_reason;
  }

  const std::wstring src = TestPath(L"xvol_plain_src.dat");
  const std::wstring dst = other.Track(L"xvol_plain_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "cross_volume_payload"));

  EXPECT_WIN32_ERROR(::MoveFileExW(src.c_str(), dst.c_str(), 0),
                     ERROR_NOT_SAME_DEVICE);
  EXPECT_WIN32_ERROR(
      ::MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING),
      ERROR_NOT_SAME_DEVICE);

  {
    auto h_src =
        core::CreateWin32File(src, DELETE | SYNCHRONIZE, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h_src) << core::FormatWin32Error(h_src.error);
    auto nt_ren = core::SetRenameInfo(h_src.value->get(), L"\\??\\" + dst,
                                      nullptr, /*replace_if_exists=*/false);
    EXPECT_NT_STATUS(nt_ren.status, STATUS_NOT_SAME_DEVICE);
  }

  EXPECT_TRUE(helpers::CheckFileContents(src, "cross_volume_payload"));
  EXPECT_TRUE(helpers::CheckFileNotFound(dst));
}

// Spec: Win32 MoveFileExW(MOVEFILE_COPY_ALLOWED) -- moving a file off the
// volume under test copies it to the other volume and then deletes the source.
TEST_F(RenameMoveTest, CrossVolumeMoveCopyAllowedOutOfVolumeDeletesSource) {
  OtherVolumeScratch other;
  std::string skip_reason;
  if (!AcquireOtherVolumeScratch(TempDir().Path(), &other, &skip_reason)) {
    RecordSkipCategory("volume_policy");
    GTEST_SKIP() << skip_reason;
  }

  const std::wstring src = TestPath(L"xvol_out_src.dat");
  const std::wstring dst = other.Track(L"xvol_out_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "moved_out_payload"));

  ASSERT_WIN32_TRUE(
      ::MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED));

  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "moved_out_payload"));
}

// Spec: Win32 MoveFileExW(MOVEFILE_COPY_ALLOWED) -- moving a file from another
// volume onto the volume under test creates the target with the full contents
// and removes the source.
TEST_F(RenameMoveTest, CrossVolumeMoveCopyAllowedIntoVolumeCreatesTarget) {
  OtherVolumeScratch other;
  std::string skip_reason;
  if (!AcquireOtherVolumeScratch(TempDir().Path(), &other, &skip_reason)) {
    RecordSkipCategory("volume_policy");
    GTEST_SKIP() << skip_reason;
  }

  const std::wstring src = other.Track(L"xvol_in_src.dat");
  const std::wstring dst = TestPath(L"xvol_in_dst.dat");
  ASSERT_TRUE(core::WriteFileContents(src, "moved_in_payload"));

  ASSERT_WIN32_TRUE(
      ::MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED));

  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "moved_in_payload"));
}

} // namespace fstest
