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
#include "streams_helpers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fstest {

namespace {

bool RenameStreamViaFileRenameInfo(HANDLE h, std::wstring_view new_stream_name,
                                   bool replace_if_exists, DWORD *error_out) {
  const size_t name_bytes = new_stream_name.size() * sizeof(wchar_t);
  const size_t total_size = std::max<size_t>(
      sizeof(FILE_RENAME_INFO),
      offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t));
  std::vector<uint8_t> buf(total_size, 0);
  auto *info = reinterpret_cast<FILE_RENAME_INFO *>(buf.data());
  info->ReplaceIfExists = replace_if_exists ? TRUE : FALSE;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(name_bytes);
  if (name_bytes > 0) {
    std::memcpy(info->FileName, new_stream_name.data(), name_bytes);
  }
  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok = ::SetFileInformationByHandle(h, FileRenameInfo, buf.data(),
                                               static_cast<DWORD>(buf.size()));
  const DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
  if (error_out != nullptr) {
    *error_out = err;
  }
  return ok != FALSE;
}

// Outcome of RenameStreamByPath: the open of the source stream and the
// FileRenameInfo call are reported separately so a failed open can never be
// mistaken for the rename status under test.
struct StreamRenameResult {
  DWORD open_error = ERROR_SUCCESS;
  DWORD rename_error = ERROR_SUCCESS;

  bool ok() const noexcept {
    return open_error == ERROR_SUCCESS && rename_error == ERROR_SUCCESS;
  }

  std::string Describe() const {
    std::ostringstream oss;
    if (open_error != ERROR_SUCCESS) {
      oss << "open(DELETE) failed: " << core::FormatWin32Error(open_error);
    } else {
      oss << "FileRenameInfo: " << core::FormatWin32Error(rename_error);
    }
    return oss.str();
  }
};

// Opens `source_path` (a stream or base-file path) with DELETE access, renames
// the opened stream to `new_stream_name` (e.g. L":new") via FileRenameInfo,
// and closes the handle before returning.
StreamRenameResult RenameStreamByPath(std::wstring_view source_path,
                                      std::wstring_view new_stream_name,
                                      bool replace_if_exists) {
  StreamRenameResult res{};
  auto h =
      core::CreateWin32File(source_path, DELETE | SYNCHRONIZE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  if (!h) {
    res.open_error = (h.error != ERROR_SUCCESS)
                         ? h.error
                         : static_cast<DWORD>(ERROR_GEN_FAILURE);
    return res;
  }
  RenameStreamViaFileRenameInfo(h.value->get(), new_stream_name,
                                replace_if_exists, &res.rename_error);
  return res;
}

class ScopedBackupContext {
public:
  ScopedBackupContext(HANDLE h, bool is_write) noexcept
      : handle_(h), is_write_(is_write) {}
  ~ScopedBackupContext() { Abort(); }

  ScopedBackupContext(const ScopedBackupContext &) = delete;
  ScopedBackupContext &operator=(const ScopedBackupContext &) = delete;

  LPVOID *put() noexcept { return &ctx_; }

  void Abort() noexcept {
    if (ctx_ != nullptr && handle_ != nullptr &&
        handle_ != INVALID_HANDLE_VALUE) {
      DWORD ignored = 0;
      if (is_write_) {
        ::BackupWrite(handle_, nullptr, 0, &ignored, /*bAbort=*/TRUE,
                      /*bProcessSecurity=*/FALSE, &ctx_);
      } else {
        ::BackupRead(handle_, nullptr, 0, &ignored, /*bAbort=*/TRUE,
                     /*bProcessSecurity=*/FALSE, &ctx_);
      }
      ctx_ = nullptr;
    }
  }

private:
  HANDLE handle_ = nullptr;
  bool is_write_ = false;
  LPVOID ctx_ = nullptr;
};

bool HasStreamSuffix(std::wstring_view path,
                     std::wstring_view base_and_stream) {
  if (core::EndsWithIgnoreCase(path, base_and_stream)) {
    return true;
  }
  const std::wstring with_type = std::wstring(base_and_stream) + L":$DATA";
  return core::EndsWithIgnoreCase(path, with_type);
}

} // namespace

// ---------------------------------------------------------------------------
// Domain 8: Streams — Operations, Locking, Sparse ADS, Backup & Security
// ---------------------------------------------------------------------------

// Spec: [MS-FSA] 2.1.5.12.29 & Win32 File Streams — each stream stores
// independent bytes and size while sharing FileIndex, attributes, and times.
TEST_F(StreamsTest, PerStreamDataAndSizeIndependentOfDefaultStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"per_stream_sizes.dat");
  const std::wstring stream_a = base + L":a";
  const std::wstring stream_b = base + L":b";

  const std::string data_main(100, 'M');
  const std::string data_a(250, 'A');
  const std::string data_b = "1234567";

  ASSERT_WIN32_OK(core::WriteFileContents(base, data_main));
  const int64_t backdated_time = core::MakeUtcFileTimeInt64(2020, 6, 1);
  ASSERT_TRUE(BackdateAllTimes(base, backdated_time));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_a, data_a));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_b, data_b));

  EXPECT_TRUE(helpers::CheckFileContents(base, data_main));
  EXPECT_TRUE(helpers::CheckFileContents(stream_a, data_a));
  EXPECT_TRUE(helpers::CheckFileContents(stream_b, data_b));

  auto info_main = core::QueryInfoByPath(base);
  auto info_a = core::QueryInfoByPath(stream_a);
  auto info_b = core::QueryInfoByPath(stream_b);
  ASSERT_WIN32_OK(info_main);
  ASSERT_WIN32_OK(info_a);
  ASSERT_WIN32_OK(info_b);

  // Sizes are independent per stream.
  EXPECT_EQ(core::FileSizeOf(*info_main.value), uint64_t{100});
  EXPECT_EQ(core::FileSizeOf(*info_a.value), uint64_t{250});
  EXPECT_EQ(core::FileSizeOf(*info_b.value), uint64_t{7});

  // FileIndex, attributes, and timestamps are shared across all streams.
  // Because base was backdated to 2020-06-01 before creating :a and :b,
  // filesystems that stamp per-stream creation times independently will
  // deterministically mismatch instead of passing within one clock tick.
  EXPECT_EQ(core::FileIndexOf(*info_main.value),
            core::FileIndexOf(*info_a.value));
  EXPECT_EQ(core::FileIndexOf(*info_main.value),
            core::FileIndexOf(*info_b.value));
  EXPECT_EQ(MaskPolicyAttributes(info_main.value->dwFileAttributes),
            MaskPolicyAttributes(info_a.value->dwFileAttributes));
  EXPECT_EQ(MaskPolicyAttributes(info_main.value->dwFileAttributes),
            MaskPolicyAttributes(info_b.value->dwFileAttributes));
  EXPECT_EQ(core::FileTimeToU64(info_main.value->ftCreationTime),
            static_cast<uint64_t>(backdated_time));
  EXPECT_EQ(core::FileTimeToU64(info_main.value->ftCreationTime),
            core::FileTimeToU64(info_a.value->ftCreationTime));
  EXPECT_EQ(core::FileTimeToU64(info_main.value->ftCreationTime),
            core::FileTimeToU64(info_b.value->ftCreationTime));
  EXPECT_EQ(core::FileTimeToU64(info_main.value->ftLastWriteTime),
            core::FileTimeToU64(info_a.value->ftLastWriteTime));
  EXPECT_EQ(core::FileTimeToU64(info_main.value->ftLastWriteTime),
            core::FileTimeToU64(info_b.value->ftLastWriteTime));

  // FindFirstStreamW reports accurate per-stream StreamSize as well.
  auto streams = core::ListStreamsWin32(base);
  ASSERT_WIN32_OK(streams);
  EXPECT_MULTISET_EQ(
      streams_internal::ExtractStreamNames(*streams.value),
      (std::vector<std::wstring>{L"::$DATA", L":a:$DATA", L":b:$DATA"}));
  for (const auto &entry : *streams.value) {
    if (entry.name == L"::$DATA") {
      EXPECT_EQ(entry.size, int64_t{100});
    } else if (entry.name == L":a:$DATA") {
      EXPECT_EQ(entry.size, int64_t{250});
    } else if (entry.name == L":b:$DATA") {
      EXPECT_EQ(entry.size, int64_t{7});
    }
  }
}

// Spec: [MS-FSA] 2.1.5.15.12 FileRenameInformation — stream renames move the
// payload to the new name and enforce ReplaceIfExists on collisions.
TEST_F(StreamsTest, StreamRenameReplaceAndNoReplace) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"rename_stream.dat");
  const std::wstring old_stream = base + L":old";
  const std::wstring new_stream = base + L":new";
  const std::wstring dst_stream = base + L":dst";

  ASSERT_WIN32_OK(core::WriteFileContents(base, "primary"));
  ASSERT_WIN32_OK(core::WriteFileContents(old_stream, "payload-old"));
  // Per WDK / [MS-FSA] FILE_RENAME_INFORMATION, ReplaceIfExists=TRUE replaces
  // an existing stream when it is a zero-length data stream.
  {
    auto dst_h =
        core::CreateWin32File(dst_stream, GENERIC_WRITE, core::kShareAll,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(dst_h);
  }

  // Rename :old -> :new (no collision).
  {
    const auto renamed = RenameStreamByPath(old_stream, L":new",
                                            /*replace_if_exists=*/false);
    ASSERT_TRUE(renamed.ok()) << renamed.Describe();
  }
  EXPECT_TRUE(streams_internal::CheckStreamAbsent(old_stream));
  EXPECT_TRUE(helpers::CheckFileContents(new_stream, "payload-old"));

  // Collision with :dst when ReplaceIfExists=FALSE fails ERROR_ALREADY_EXISTS.
  {
    const auto collided = RenameStreamByPath(new_stream, L":dst",
                                             /*replace_if_exists=*/false);
    ASSERT_WIN32_ERROR_CODE(collided.open_error, ERROR_SUCCESS);
    EXPECT_WIN32_ERROR_CODE(collided.rename_error, ERROR_ALREADY_EXISTS);
  }

  // Collision with :dst when ReplaceIfExists=TRUE succeeds and moves payload.
  {
    const auto replaced = RenameStreamByPath(new_stream, L":dst",
                                             /*replace_if_exists=*/true);
    ASSERT_TRUE(replaced.ok()) << replaced.Describe();
  }
  EXPECT_TRUE(streams_internal::CheckStreamAbsent(new_stream));
  EXPECT_TRUE(helpers::CheckFileContents(dst_stream, "payload-old"));
}

// Spec: [MS-FSA] 2.1.5.15.12 FileRenameInformation — a stream handle can only
// be renamed to another stream of the same file: a target without a leading
// ':' fails STATUS_INVALID_PARAMETER and leaves the stream untouched.
TEST_F(StreamsTest, StreamRenameToNonStreamTargetFailsInvalidParameter) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"rename_nonstream.dat");
  const std::wstring src_stream = base + L":src";

  ASSERT_WIN32_OK(core::WriteFileContents(base, "primary"));
  ASSERT_WIN32_OK(core::WriteFileContents(src_stream, "payload-src"));

  // Same-volume targets avoid CWD cross-device resolution.
  {
    auto h =
        core::CreateWin32File(src_stream, DELETE | SYNCHRONIZE, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    auto nt_bad =
        core::SetRenameInfo(h.value->get(), TestNtPath(L"missing_colon"),
                            /*root_dir=*/nullptr,
                            /*replace_if_exists=*/false);
    EXPECT_NT_STATUS(nt_bad.status, STATUS_INVALID_PARAMETER);

    DWORD err = ERROR_SUCCESS;
    EXPECT_FALSE(RenameStreamViaFileRenameInfo(
        h.value->get(), TestPath(L"missing_colon"),
        /*replace_if_exists=*/false, &err));
    EXPECT_WIN32_ERROR_CODE(err, ERROR_INVALID_PARAMETER);
  }

  EXPECT_TRUE(helpers::CheckFileContents(src_stream, "payload-src"));
  EXPECT_TRUE(helpers::CheckPathAbsent(TestPath(L"missing_colon")));
  EXPECT_TRUE(
      streams_internal::CheckStreamNames(base, {L"::$DATA", L":src:$DATA"}));
}

// Spec: [MS-FSA] 2.1.5.15.12 FileRenameInformation -- replacing
// a non-empty target stream with ReplaceIfExists=TRUE fails
// STATUS_INVALID_PARAMETER and leaves both streams intact.
TEST_F(StreamsTest, StreamRenameReplaceNonEmptyTargetFailsInvalidParameter) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"rename_nonempty_dst.dat");
  const std::wstring src_stream = base + L":src";
  const std::wstring dst_stream = base + L":dst";
  const std::string base_data = "base-data";
  const std::string src_data = "source-stream-payload";
  const std::string dst_data = "non-empty-target-payload";

  ASSERT_WIN32_OK(core::WriteFileContents(base, base_data));
  ASSERT_WIN32_OK(core::WriteFileContents(src_stream, src_data));
  ASSERT_WIN32_OK(core::WriteFileContents(dst_stream, dst_data));

  {
    auto h =
        core::CreateWin32File(src_stream, DELETE | SYNCHRONIZE, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    auto nt_res = core::SetRenameInfo(h.value->get(), L":dst",
                                      /*root_dir=*/nullptr,
                                      /*replace_if_exists=*/true);
    ASSERT_NT_STATUS(nt_res.status, STATUS_INVALID_PARAMETER);

    DWORD err = ERROR_SUCCESS;
    EXPECT_FALSE(RenameStreamViaFileRenameInfo(h.value->get(), L":dst",
                                               /*replace_if_exists=*/true,
                                               &err));
    EXPECT_WIN32_ERROR_CODE(err, ERROR_INVALID_PARAMETER);
  }

  // Both streams and primary stream remain untouched.
  EXPECT_TRUE(helpers::CheckFileContents(base, base_data));
  EXPECT_TRUE(helpers::CheckFileContents(src_stream, src_data));
  EXPECT_TRUE(helpers::CheckFileContents(dst_stream, dst_data));
  EXPECT_TRUE(streams_internal::CheckStreamNames(
      base, {L"::$DATA", L":src:$DATA", L":dst:$DATA"}));
}

// Spec: [MS-FSA] 2.1.5.15.5 FileEndOfFileInformation — truncating one stream
// via TRUNCATE_EXISTING or SetEndOfFile leaves sibling streams untouched.
TEST_F(StreamsTest, StreamTruncationIndependent) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"trunc_stream.dat");
  const std::wstring stream_a = base + L":a";
  const std::wstring stream_b = base + L":b";

  const std::string payload_main(500, 'D');
  const std::string payload_a(300, 'A');
  const std::string payload_b(200, 'B');

  ASSERT_WIN32_OK(core::WriteFileContents(base, payload_main));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_a, payload_a));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_b, payload_b));

  // TRUNCATE_EXISTING on default stream leaves :a and :b intact.
  {
    auto h = core::CreateWin32File(base, GENERIC_WRITE, core::kShareAll,
                                   TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);
  }
  EXPECT_TRUE(helpers::CheckFileContents(base, ""));
  EXPECT_TRUE(helpers::CheckFileContents(stream_a, payload_a));
  EXPECT_TRUE(helpers::CheckFileContents(stream_b, payload_b));

  // SetEndOfFile(0) on :a leaves :b intact.
  {
    auto h = core::CreateWin32File(stream_a, GENERIC_WRITE, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);
    ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
    ASSERT_WIN32_TRUE(::SetEndOfFile(h.value->get()));
  }
  EXPECT_TRUE(helpers::CheckFileContents(stream_a, ""));
  EXPECT_TRUE(helpers::CheckFileContents(stream_b, payload_b));
}

// Spec: [MS-FSA] 2.1.5.1.2 Open of an Existing File — share modes are
// enforced independently per stream across ::$DATA and sibling ADS.
TEST_F(StreamsTest, StreamSharingModeIndependentPerStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"share_stream.dat");
  const std::wstring stream_a = base + L":a";
  const std::wstring stream_b = base + L":b";
  const std::wstring stream_default = base + L"::$DATA";

  ASSERT_WIN32_OK(core::WriteFileContents(base, "main"));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_a, "alpha"));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_b, "beta"));

  // Exclusive write handle on :a.
  auto h_a1 = core::CreateWin32File(stream_a, GENERIC_WRITE, 0, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_a1);

  // Second open of :a fails sharing violation.
  auto h_a2 = core::CreateWin32File(stream_a, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(h_a2);
  EXPECT_WIN32_ERROR_CODE(h_a2.error, ERROR_SHARING_VIOLATION);

  // Exclusive opens on :b and ::$DATA succeed independently.
  auto h_b = core::CreateWin32File(stream_b, GENERIC_READ | GENERIC_WRITE, 0,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_WIN32_OK(h_b);

  auto h_def =
      core::CreateWin32File(stream_default, GENERIC_READ | GENERIC_WRITE, 0,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_WIN32_OK(h_def);
}

// Spec: [MS-FSA] 2.1.5.1.2 & 2.1.5.15.3 — FILE_ATTRIBUTE_READONLY blocks
// creating, writing, and deleting alternate streams while allowing reads.
TEST_F(StreamsTest, ReadOnlyFileBlocksStreamCreateModifyAndDelete) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"ro_streams.dat");
  const std::wstring existing_ads = base + L":existing";
  const std::wstring new_ads = base + L":newads";

  ASSERT_WIN32_OK(core::WriteFileContents(base, "base-ro"));
  ASSERT_WIN32_OK(core::WriteFileContents(existing_ads, "orig-ads"));
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(base.c_str(), FILE_ATTRIBUTE_READONLY));

  // Creating a new ADS on a READONLY file fails ERROR_ACCESS_DENIED.
  auto create_res =
      core::CreateWin32File(new_ads, GENERIC_WRITE, core::kShareAll, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(create_res);
  EXPECT_WIN32_ERROR_CODE(create_res.error, ERROR_ACCESS_DENIED);

  // Opening an existing ADS for write fails ERROR_ACCESS_DENIED.
  auto write_res =
      core::CreateWin32File(existing_ads, GENERIC_WRITE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(write_res);
  EXPECT_WIN32_ERROR_CODE(write_res.error, ERROR_ACCESS_DENIED);

  // Reading the existing ADS still succeeds.
  EXPECT_TRUE(helpers::CheckFileContents(existing_ads, "orig-ads"));

  // Deleting an existing ADS on a READONLY file fails ERROR_ACCESS_DENIED.
  EXPECT_WIN32_ERROR(::DeleteFileW(existing_ads.c_str()), ERROR_ACCESS_DENIED);

  // Clearing READONLY restores stream create/delete operations.
  ASSERT_WIN32_TRUE(::SetFileAttributesW(base.c_str(), FILE_ATTRIBUTE_NORMAL));
  ASSERT_WIN32_OK(core::WriteFileContents(new_ads, "now-writable"));
  EXPECT_WIN32_TRUE(::DeleteFileW(existing_ads.c_str()));
}

// Spec: [MS-FSA] 2.1.5.15.3 FileDispositionInformation — deleting a named ADS
// that another handle holds open leaves it delete-pending (new opens fail
// STATUS_DELETE_PENDING) until the last handle closes; the default stream
// stays accessible throughout.
TEST_F(StreamsTest, StreamDeleteWhileOpenLegacyDeletePending) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"del_open_stream.dat");
  const std::wstring s = base + L":s";
  ASSERT_WIN32_OK(core::WriteFileContents(base, "default-payload"));
  ASSERT_WIN32_OK(core::WriteFileContents(s, "stream-payload"));

  {
    auto h1 = core::CreateWin32File(s, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h1);

    {
      auto h2 = core::CreateWin32File(s, DELETE, core::kShareAll, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(h2);
      auto disp = core::SetDisposition(h2.value->get(), /*delete_file=*/true);
      ASSERT_NT_SUCCESS(disp.status);
    }

    // While H1 remains open, f:s is STATUS_DELETE_PENDING.
    auto reopen_s = core::NtOpenFilePath(
        TestNtPath(L"del_open_stream.dat:s"), FILE_READ_DATA | SYNCHRONIZE,
        core::kShareAll, nt::kFileSynchronousIoNonalert);
    EXPECT_FALSE(reopen_s);
    EXPECT_NT_STATUS(reopen_s.status, STATUS_DELETE_PENDING);

    // Default stream f::$DATA remains fully accessible.
    EXPECT_TRUE(
        helpers::CheckFileContents(base + L"::$DATA", "default-payload"));

    // Existing handle H1 continues reading stream bytes.
    auto read_h1 = core::ReadAllBytes(h1.value->get());
    ASSERT_WIN32_OK(read_h1);
    EXPECT_EQ(*read_h1.value, "stream-payload");
  }

  // Once H1 closes, :s is completely removed while base survives.
  EXPECT_TRUE(streams_internal::CheckStreamAbsent(s));
  EXPECT_TRUE(streams_internal::CheckStreamNames(base, {L"::$DATA"}));
}

// Spec: [MS-FSA] 2.1.5.15.4 FileDispositionInformationEx — on NTFS,
// FILE_DISPOSITION_POSIX_SEMANTICS applied to a named ADS that another handle
// holds open does NOT unlink the stream name immediately: the stream stays
// delete-pending (new opens fail STATUS_DELETE_PENDING) until the last stream
// handle closes, then it is removed.
TEST_F(StreamsTest, StreamPosixDeleteWhileOpenStaysDeletePending) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);
  // FileDispositionInformationEx requires Windows 10 1709 (build 16299).
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const std::wstring base = TestPath(L"del_open_posix_stream.dat");
  const std::wstring p = base + L":p";
  ASSERT_WIN32_OK(core::WriteFileContents(base, "default-payload"));
  ASSERT_WIN32_OK(core::WriteFileContents(p, "posix-stream"));

  {
    auto h1 = core::CreateWin32File(p, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h1);
    {
      auto h2 = core::CreateWin32File(p, DELETE, core::kShareAll, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(h2);
      auto disp = core::SetDispositionEx(
          h2.value->get(),
          nt::kFileDispositionDelete | nt::kFileDispositionPosixSemantics);
      ASSERT_NT_SUCCESS(disp.status);
    }

    auto reopen_p =
        core::NtOpenFilePath(TestNtPath(L"del_open_posix_stream.dat:p"),
                             FILE_READ_DATA | SYNCHRONIZE, core::kShareAll,
                             nt::kFileSynchronousIoNonalert);
    EXPECT_FALSE(reopen_p);
    EXPECT_NT_STATUS(reopen_p.status, STATUS_DELETE_PENDING);

    auto read_h1 = core::ReadAllBytes(h1.value->get());
    ASSERT_WIN32_OK(read_h1);
    EXPECT_EQ(*read_h1.value, "posix-stream");
  }

  EXPECT_TRUE(streams_internal::CheckStreamAbsent(p));
  EXPECT_TRUE(streams_internal::CheckStreamNames(base, {L"::$DATA"}));
}

// Spec: [MS-FSA] 2.1.5.15.4 FileDispositionInformationEx — POSIX-semantics
// deletion of a file whose ADS another handle holds open unlinks the file
// name immediately while the open ADS handle keeps reading its data.
TEST_F(StreamsTest, PosixDeleteFileWhileStreamHandleOpenUnlinksName) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);
  // FileDispositionInformationEx requires Windows 10 1709 (build 16299).
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const std::wstring f2 = TestPath(L"del_open_posix_file.dat");
  const std::wstring f2_ads = f2 + L":keep";
  ASSERT_WIN32_OK(core::WriteFileContents(f2, "file-main"));
  ASSERT_WIN32_OK(core::WriteFileContents(f2_ads, "file-ads-alive"));
  {
    auto h_ads = core::CreateWin32File(f2_ads, GENERIC_READ, core::kShareAll,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_ads);
    {
      auto h_del = core::CreateWin32File(f2, DELETE, core::kShareAll,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(h_del);
      auto disp = core::SetDispositionEx(
          h_del.value->get(),
          nt::kFileDispositionDelete | nt::kFileDispositionPosixSemantics);
      ASSERT_NT_SUCCESS(disp.status);
    }

    // Namespace entry is immediately gone while h_ads remains readable.
    EXPECT_TRUE(helpers::CheckFileNotFound(f2));

    auto read_ads = core::ReadAllBytes(h_ads.value->get());
    ASSERT_WIN32_OK(read_ads);
    EXPECT_EQ(*read_ads.value, "file-ads-alive");
  }
  EXPECT_TRUE(helpers::CheckFileNotFound(f2));
}

// Spec: [MS-FSA] 2.1.5.15.12 FileRenameInformation — direct case-only and
// self stream renames succeed as no-ops; two-step renames update casing.
TEST_F(StreamsTest, StreamCaseOnlyRenameAndSelfRename) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"case_rename_stream.dat");
  const std::wstring lower = base + L":mixed";
  const std::wstring mixed = base + L":MiXeD";

  ASSERT_WIN32_OK(core::WriteFileContents(lower, "preserve-bytes"));

  // On NTFS, a direct case-only stream rename (:mixed -> :MiXeD) succeeds as a
  // same-stream no-op (preserving bytes and original :mixed:$DATA casing),
  // whereas a two-step rename (:mixed -> :tmp -> :MiXeD) updates stream casing.
  {
    const auto renamed =
        RenameStreamByPath(lower, L":MiXeD", /*replace_if_exists=*/false);
    ASSERT_TRUE(renamed.ok()) << renamed.Describe();
  }
  EXPECT_TRUE(
      streams_internal::CheckStreamNames(base, {L"::$DATA", L":mixed:$DATA"}));
  EXPECT_TRUE(helpers::CheckFileContents(mixed, "preserve-bytes"));

  // Two-step rename (:mixed -> :tmp_case -> :MiXeD) updates exact stream case.
  {
    const auto to_tmp =
        RenameStreamByPath(lower, L":tmp_case", /*replace_if_exists=*/false);
    ASSERT_TRUE(to_tmp.ok()) << to_tmp.Describe();
    const auto to_mixed = RenameStreamByPath(base + L":tmp_case", L":MiXeD",
                                             /*replace_if_exists=*/false);
    ASSERT_TRUE(to_mixed.ok()) << to_mixed.Describe();
  }
  EXPECT_TRUE(
      streams_internal::CheckStreamNames(base, {L"::$DATA", L":MiXeD:$DATA"}));

  // Self-rename (:MiXeD -> :MiXeD) succeeds as a no-op.
  for (bool replace : {false, true}) {
    const auto self_rename = RenameStreamByPath(mixed, L":MiXeD", replace);
    ASSERT_TRUE(self_rename.ok()) << "Self-rename failed (replace=" << replace
                                  << "): " << self_rename.Describe();
  }
  EXPECT_TRUE(
      streams_internal::CheckStreamNames(base, {L"::$DATA", L":MiXeD:$DATA"}));
  EXPECT_TRUE(helpers::CheckFileContents(mixed, "preserve-bytes"));
}

// Spec: [MS-FSA] 2.1.5.15.12 FileRenameInformation — renaming the default
// data stream (handle opened on the base file) to a named stream moves the
// file's bytes into the named stream and leaves an empty default stream.
TEST_F(StreamsTest, DefaultStreamRenameToNamedStreamMovesData) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"rename_default_to_named.dat");
  const std::string payload = "default-stream-payload";
  ASSERT_WIN32_OK(core::WriteFileContents(base, payload));

  {
    const auto renamed =
        RenameStreamByPath(base, L":moved", /*replace_if_exists=*/false);
    ASSERT_TRUE(renamed.ok()) << renamed.Describe();
  }

  EXPECT_TRUE(helpers::CheckFileContents(base + L":moved", payload));
  EXPECT_TRUE(helpers::CheckFileContents(base, ""));
  EXPECT_TRUE(
      streams_internal::CheckStreamNames(base, {L"::$DATA", L":moved:$DATA"}));
}

// Spec: [MS-FSA] 2.1.5.15.12 FileRenameInformation — renaming a named stream
// onto the default data stream ("::$DATA") collides unless ReplaceIfExists is
// set, and with ReplaceIfExists replaces a zero-length default stream.
TEST_F(StreamsTest, NamedStreamRenameOntoEmptyDefaultStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"rename_named_to_default.dat");
  const std::wstring named = base + L":named";
  const std::string payload = "named-stream-payload";
  ASSERT_WIN32_OK(core::WriteFileContents(base, ""));
  ASSERT_WIN32_OK(core::WriteFileContents(named, payload));

  // The default stream always exists, so ReplaceIfExists=FALSE collides.
  {
    const auto collided =
        RenameStreamByPath(named, L"::$DATA", /*replace_if_exists=*/false);
    ASSERT_WIN32_ERROR_CODE(collided.open_error, ERROR_SUCCESS);
    EXPECT_WIN32_ERROR_CODE(collided.rename_error, ERROR_ALREADY_EXISTS);
  }
  EXPECT_TRUE(helpers::CheckFileContents(named, payload));

  {
    const auto replaced =
        RenameStreamByPath(named, L"::$DATA", /*replace_if_exists=*/true);
    ASSERT_TRUE(replaced.ok()) << replaced.Describe();
  }
  EXPECT_TRUE(helpers::CheckFileContents(base, payload));
  EXPECT_TRUE(streams_internal::CheckStreamAbsent(named));
  EXPECT_TRUE(streams_internal::CheckStreamNames(base, {L"::$DATA"}));
}

// Spec: [MS-FSA] 2.1.5.4 / 2.1.5.12.11 / Win32 GetFinalPathNameByHandleW --
// writing to an ADS advances base file LastWriteTime/ChangeTime and sets
// ARCHIVE, and open ADS handles report the :stream suffix in
// FileNameInformation and GetFinalPathNameByHandleW.
TEST_F(StreamsTest, StreamWriteUpdatesBaseMetadataAndQueriesStreamName) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"ads_meta_name.dat");
  const std::wstring ads = base + L":meta_stream";
  const std::wstring expected_suffix = L"\\ads_meta_name.dat:meta_stream";
  const std::string base_payload = "base-unchanged-bytes";
  const std::string ads_updated = "ads-updated-payload-bytes";

  ASSERT_WIN32_OK(core::WriteFileContents(base, base_payload));
  ASSERT_WIN32_OK(core::WriteFileContents(ads, "ads-initial"));

  // Clear ARCHIVE and backdate all four timestamps on the base file.
  ASSERT_WIN32_TRUE(::SetFileAttributesW(base.c_str(), FILE_ATTRIBUTE_NORMAL));
  const int64_t kPast = core::MakeUtcFileTimeInt64(2020, 1, 1);
  ASSERT_TRUE(BackdateAllTimes(base, kPast));

  {
    auto h_check = core::CreateWin32File(
        base, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_check);
    auto before = core::QueryBasicInfo(h_check.value->get());
    ASSERT_NT_SUCCESS(before.status);
    ASSERT_TRUE(before.value.has_value());
    ASSERT_EQ(before.value->last_write_time.QuadPart, kPast);
    ASSERT_EQ(before.value->change_time.QuadPart, kPast);
    ASSERT_EQ(before.value->file_attributes & FILE_ATTRIBUTE_ARCHIVE, 0u);
  }

  // Open :meta_stream, write new bytes, and query stream name via
  // FileNameInformation and GetFinalPathNameByHandleW while open.
  {
    auto h_ads = core::CreateWin32File(ads, GENERIC_READ | GENERIC_WRITE,
                                       core::kShareAll, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_ads);

    ASSERT_WIN32_OK(core::WriteAllBytes(h_ads.value->get(), ads_updated.data(),
                                        ads_updated.size()));
    ASSERT_WIN32_TRUE(::FlushFileBuffers(h_ads.value->get()));

    auto nt_name =
        core::QueryFileNameInfo(h_ads.value->get(), /*normalized=*/false);
    ASSERT_NT_SUCCESS(nt_name.status);
    ASSERT_TRUE(nt_name.value.has_value());
    EXPECT_TRUE(HasStreamSuffix(*nt_name.value, expected_suffix))
        << "FileNameInformation '" << core::WideToUtf8(*nt_name.value)
        << "' missing stream suffix '" << core::WideToUtf8(expected_suffix)
        << "'";

    auto final_none = core::QueryFinalPath(
        h_ads.value->get(), VOLUME_NAME_NONE | FILE_NAME_NORMALIZED);
    ASSERT_WIN32_OK(final_none);
    EXPECT_TRUE(HasStreamSuffix(*final_none.value, expected_suffix))
        << "GetFinalPathNameByHandleW(VOLUME_NAME_NONE) '"
        << core::WideToUtf8(*final_none.value) << "' missing stream suffix '"
        << core::WideToUtf8(expected_suffix) << "'";

    auto final_dos = core::QueryFinalPath(
        h_ads.value->get(), VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
    ASSERT_WIN32_OK(final_dos);
    EXPECT_TRUE(HasStreamSuffix(*final_dos.value, expected_suffix))
        << "GetFinalPathNameByHandleW(VOLUME_NAME_DOS) '"
        << core::WideToUtf8(*final_dos.value) << "' missing stream suffix '"
        << core::WideToUtf8(expected_suffix) << "'";
  }

  // Base file LastWriteTime and ChangeTime must advance and ARCHIVE must be
  // set while CreationTime and default stream contents remain unchanged.
  {
    auto h_base = core::CreateWin32File(
        base, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_base);
    auto after = core::QueryBasicInfo(h_base.value->get());
    ASSERT_NT_SUCCESS(after.status);
    ASSERT_TRUE(after.value.has_value());
    EXPECT_GT(after.value->last_write_time.QuadPart, kPast);
    EXPECT_GT(after.value->change_time.QuadPart, kPast);
    EXPECT_EQ(after.value->creation_time.QuadPart, kPast);
    EXPECT_NE(after.value->file_attributes & FILE_ATTRIBUTE_ARCHIVE, 0u);
  }

  EXPECT_TRUE(helpers::CheckFileContents(base, base_payload));
  EXPECT_TRUE(helpers::CheckFileContents(ads, ads_updated));
}

// Spec: [MS-FSA] 2.1.5.8 Byte-Range Lock — exclusive byte-range locks on
// one stream block peer handles on that stream without affecting siblings.
TEST_F(StreamsTest, StreamIndependentByteRangeLocks) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring base = TestPath(L"lock_streams.dat");
  const std::wstring stream_a = base + L":a";
  const std::wstring stream_b = base + L":b";
  const std::wstring stream_def = base + L"::$DATA";

  ASSERT_WIN32_OK(core::WriteFileContents(base, std::string(64, '0')));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_a, std::string(64, 'A')));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_b, std::string(64, 'B')));

  auto h_a1 = core::CreateWin32File(stream_a, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
  auto h_a2 = core::CreateWin32File(stream_a, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
  auto h_def = core::CreateWin32File(stream_def, GENERIC_READ | GENERIC_WRITE,
                                     core::kShareAll, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL);
  auto h_b = core::CreateWin32File(stream_b, GENERIC_READ | GENERIC_WRITE,
                                   core::kShareAll, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_a1);
  ASSERT_WIN32_OK(h_a2);
  ASSERT_WIN32_OK(h_def);
  ASSERT_WIN32_OK(h_b);

  // Exclusive lock on [0, 32) of f:a via h_a1.
  OVERLAPPED ov{};
  ASSERT_WIN32_TRUE(::LockFileEx(
      h_a1.value->get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
      32, 0, &ov));

  // Second handle on f:a is blocked for lock, read, and write in [0, 32).
  OVERLAPPED ov_try{};
  EXPECT_WIN32_ERROR(
      ::LockFileEx(h_a2.value->get(),
                   LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 32,
                   0, &ov_try),
      ERROR_LOCK_VIOLATION);

  char buf[16]{};
  DWORD xfer = 0;
  EXPECT_WIN32_ERROR(::ReadFile(h_a2.value->get(), buf, 16, &xfer, nullptr),
                     ERROR_LOCK_VIOLATION);
  EXPECT_WIN32_ERROR(
      ::WriteFile(h_a2.value->get(), "0123456789abcdef", 16, &xfer, nullptr),
      ERROR_LOCK_VIOLATION);

  // Default stream f::$DATA and sibling stream f:b are completely unaffected.
  for (HANDLE sibling : {h_def.value->get(), h_b.value->get()}) {
    OVERLAPPED ov_sib{};
    ASSERT_WIN32_TRUE(::LockFileEx(
        sibling, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 32, 0,
        &ov_sib));
    ASSERT_WIN32_TRUE(SeekBegin(sibling, 0));
    ASSERT_WIN32_TRUE(::ReadFile(sibling, buf, 16, &xfer, nullptr));
    EXPECT_EQ(xfer, DWORD{16});
    ASSERT_WIN32_TRUE(SeekBegin(sibling, 0));
    ASSERT_WIN32_TRUE(
        ::WriteFile(sibling, "fedcba9876543210", 16, &xfer, nullptr));
    EXPECT_EQ(xfer, DWORD{16});
    ASSERT_WIN32_TRUE(::UnlockFileEx(sibling, 0, 32, 0, &ov_sib));
  }

  // Unlocking h_a1 unblocks h_a2 immediately.
  ASSERT_WIN32_TRUE(::UnlockFileEx(h_a1.value->get(), 0, 32, 0, &ov));
  ASSERT_WIN32_TRUE(::LockFileEx(
      h_a2.value->get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
      32, 0, &ov_try));
  ASSERT_WIN32_TRUE(::ReadFile(h_a2.value->get(), buf, 16, &xfer, nullptr));
  EXPECT_EQ(xfer, DWORD{16});
  ASSERT_WIN32_TRUE(::UnlockFileEx(h_a2.value->get(), 0, 32, 0, &ov_try));
}

// Spec: [MS-FSA] 2.1.5.10 & [MS-FSCC] 2.3 — sparse FSCTLs deallocate holes
// and report allocated ranges on a named ADS without altering ::$DATA.
TEST_F(StreamsTest, SparseNamedStreamZeroDataAndAllocatedRanges) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  constexpr int64_t kChunk = 64 * 1024;       // 64 KiB NTFS compression unit
  constexpr int64_t kTotalSize = 16 * kChunk; // 1 MiB
  constexpr int64_t kHoleStart = 4 * kChunk;  // 256 KiB
  constexpr int64_t kHoleEnd = 12 * kChunk;   // 768 KiB (512 KiB hole)

  const std::wstring base = TestPath(L"sparse_ads.dat");
  const std::wstring sparse_ads = base + L":sparse";
  ASSERT_WIN32_OK(core::WriteFileContents(base, "primary-untouched"));

  auto h =
      core::CreateWin32File(sparse_ads, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h);

  auto set_sparse = core::SetSparseFile(h.value->get(), /*sparse=*/true);
  ASSERT_WIN32_OK(set_sparse);

  auto info = core::GetByHandleInfo(h.value->get());
  ASSERT_WIN32_OK(info);
  EXPECT_NE(info.value->dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE,
            DWORD{0});

  // Allocate full 1 MiB in 64 KiB chunks.
  const std::vector<uint8_t> fill_chunk(static_cast<size_t>(kChunk), 0x5A);
  for (int i = 0; i < 16; ++i) {
    ASSERT_WIN32_OK(core::WriteAllBytes(h.value->get(), fill_chunk.data(),
                                        fill_chunk.size()));
  }

  // Punch a 512 KiB sparse hole aligned to 64 KiB boundaries.
  auto zero_res = core::SetZeroData(h.value->get(), kHoleStart, kHoleEnd);
  ASSERT_WIN32_OK(zero_res);

  // Query allocated ranges across the full 1 MiB stream.
  auto ranges = core::QueryAllocatedRanges(h.value->get(), 0, kTotalSize);
  ASSERT_WIN32_OK(ranges);
  ASSERT_GE(ranges.value->size(), size_t{2});

  int64_t total_allocated = 0;
  bool covers_head = false;
  bool covers_tail = false;
  for (const auto &range : *ranges.value) {
    const int64_t start = range.FileOffset.QuadPart;
    const int64_t len = range.Length.QuadPart;
    const int64_t end = start + len;
    total_allocated += len;
    // No allocated range may overlap the zeroed [256 KiB, 768 KiB) hole.
    EXPECT_TRUE(end <= kHoleStart || start >= kHoleEnd)
        << "Range [" << start << ", " << end << ") overlaps zeroed hole ["
        << kHoleStart << ", " << kHoleEnd << ")";
    if (start <= 0 && end >= kHoleStart) {
      covers_head = true;
    }
    if (start <= kHoleEnd && end >= kTotalSize) {
      covers_tail = true;
    }
  }
  EXPECT_TRUE(covers_head);
  EXPECT_TRUE(covers_tail);
  EXPECT_LT(total_allocated, kTotalSize);

  // Verify reads: head == 0x5A, hole == 0x00, tail == 0x5A.
  auto read_chunk_at =
      [&](int64_t offset, uint8_t expected_byte) -> ::testing::AssertionResult {
    if (!SeekBegin(h.value->get(), offset)) {
      const DWORD seek_err = ::GetLastError();
      return ::testing::AssertionFailure()
             << "SetFilePointerEx(" << offset
             << ") failed: " << core::FormatWin32Error(seek_err);
    }
    std::vector<uint8_t> buf(static_cast<size_t>(kChunk), 0xFF);
    DWORD read = 0;
    if (::ReadFile(h.value->get(), buf.data(), static_cast<DWORD>(buf.size()),
                   &read, nullptr) == FALSE) {
      const DWORD read_err = ::GetLastError();
      return ::testing::AssertionFailure()
             << "ReadFile at offset " << offset
             << " failed: " << core::FormatWin32Error(read_err);
    }
    if (read != static_cast<DWORD>(kChunk)) {
      return ::testing::AssertionFailure()
             << "Short read at offset " << offset << ": got " << read
             << " bytes, expected " << kChunk;
    }
    if (!helpers::AllBytesEqual(buf, expected_byte)) {
      return ::testing::AssertionFailure()
             << "Chunk at offset " << offset
             << " did not match expected byte 0x" << std::hex
             << static_cast<unsigned int>(expected_byte);
    }
    return ::testing::AssertionSuccess();
  };
  EXPECT_TRUE(read_chunk_at(0, 0x5A));
  EXPECT_TRUE(read_chunk_at(kHoleStart, 0x00));
  EXPECT_TRUE(read_chunk_at(kHoleStart + 4 * kChunk, 0x00));
  EXPECT_TRUE(read_chunk_at(kHoleEnd, 0x5A));

  // Primary stream remains completely unaffected.
  EXPECT_TRUE(helpers::CheckFileContents(base, "primary-untouched"));
}

// Spec: Win32 BackupRead / BackupWrite — backup-stream serialization and
// restore preserve primary and alternate data stream names and payloads.
TEST_F(StreamsTest, BackupReadAndBackupWritePreservesStreams) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring src = TestPath(L"backup_src.dat");
  const std::wstring dst = TestPath(L"backup_dst.dat");
  const std::string primary_payload = "Primary stream data for backup test.";
  const std::string meta_payload = "Alternate stream :meta payload 12345.";
  const std::string extra_payload(4096, 'X');

  ASSERT_WIN32_OK(core::WriteFileContents(src, primary_payload));
  ASSERT_WIN32_OK(core::WriteFileContents(src + L":meta", meta_payload));
  ASSERT_WIN32_OK(core::WriteFileContents(src + L":extra", extra_payload));

  // Read primary + all named ADS into memory via BackupRead.
  std::vector<uint8_t> backup_bytes;
  {
    auto h_src =
        core::CreateWin32File(src, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_WIN32_OK(h_src);
    ScopedBackupContext read_ctx(h_src.value->get(), /*is_write=*/false);

    std::vector<uint8_t> chunk(8192, 0);
    for (;;) {
      DWORD bytes_read = 0;
      ASSERT_WIN32_TRUE(::BackupRead(
          h_src.value->get(), reinterpret_cast<LPBYTE>(chunk.data()),
          static_cast<DWORD>(chunk.size()), &bytes_read, /*bAbort=*/FALSE,
          /*bProcessSecurity=*/FALSE, read_ctx.put()));
      if (bytes_read == 0) {
        break;
      }
      backup_bytes.insert(backup_bytes.end(), chunk.begin(),
                          chunk.begin() + bytes_read);
    }
  }
  ASSERT_FALSE(backup_bytes.empty());

  // Restore into a brand-new file via BackupWrite.
  {
    auto h_dst = core::CreateWin32File(dst, GENERIC_WRITE, core::kShareAll,
                                       CREATE_NEW, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_WIN32_OK(h_dst);
    ScopedBackupContext write_ctx(h_dst.value->get(), /*is_write=*/true);

    size_t offset = 0;
    while (offset < backup_bytes.size()) {
      const DWORD to_write = static_cast<DWORD>(
          std::min<size_t>(backup_bytes.size() - offset, 8192));
      DWORD bytes_written = 0;
      ASSERT_WIN32_TRUE(
          ::BackupWrite(h_dst.value->get(),
                        reinterpret_cast<LPBYTE>(backup_bytes.data() + offset),
                        to_write, &bytes_written, /*bAbort=*/FALSE,
                        /*bProcessSecurity=*/FALSE, write_ctx.put()));
      ASSERT_GT(bytes_written, DWORD{0});
      offset += bytes_written;
    }
  }

  // Verify destination stream multiset and exact payloads match verbatim.
  EXPECT_TRUE(streams_internal::CheckStreamNames(
      dst, {L"::$DATA", L":meta:$DATA", L":extra:$DATA"}));
  EXPECT_TRUE(helpers::CheckFileContents(dst, primary_payload));
  EXPECT_TRUE(helpers::CheckFileContents(dst + L":meta", meta_payload));
  EXPECT_TRUE(helpers::CheckFileContents(dst + L":extra", extra_payload));
}

// Spec: [MS-FSA] 2.1.5.14 / 2.1.5.17 — base files and alternate data
// streams share a single security descriptor across DACL updates.
TEST_F(StreamsTest, SecurityDescriptorSharedBetweenBaseFileAndAlternateStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto user_sid = core::GetCurrentUserSidString();
  ASSERT_WIN32_OK(user_sid);

  const std::wstring base_path = TestPath(L"sd_ads.dat");
  const std::wstring stream_path = base_path + L":meta";
  ASSERT_WIN32_OK(core::WriteFileContents(base_path, "base-sd"));
  ASSERT_WIN32_OK(core::WriteFileContents(stream_path, "ads-sd"));

  auto dacl_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"sd_ads.dat"));
  ASSERT_TRUE(dacl_guard.armed());

  auto canonicalize_dacl =
      [](std::wstring_view sddl) -> core::Win32Result<std::wstring> {
    auto sd = core::SddlToSecurityDescriptor(sddl);
    if (!sd) {
      core::Win32Result<std::wstring> res{};
      res.error = sd.error;
      return res;
    }
    return core::SecurityDescriptorToSddl(sd.value->get(),
                                          DACL_SECURITY_INFORMATION);
  };

  // Apply a distinct DACL via the alternate stream path (including the current
  // user SID so both elevated and non-elevated runs retain full control).
  const std::wstring sddl_one =
      L"D:P(A;;FA;;;" + *user_sid.value + L")(A;;FR;;;WD)";
  auto expected_one = canonicalize_dacl(sddl_one);
  ASSERT_WIN32_OK(expected_one);

  auto set_stream =
      core::SetFileSddl(stream_path, DACL_SECURITY_INFORMATION, sddl_one);
  ASSERT_WIN32_OK(set_stream);

  auto base_sddl1 = core::GetFileSddl(base_path, DACL_SECURITY_INFORMATION);
  auto stream_sddl1 = core::GetFileSddl(stream_path, DACL_SECURITY_INFORMATION);
  ASSERT_WIN32_OK(base_sddl1);
  ASSERT_WIN32_OK(stream_sddl1);
  EXPECT_EQ(*base_sddl1.value, *expected_one.value);
  EXPECT_EQ(*stream_sddl1.value, *base_sddl1.value);

  // Update the DACL on the base file to a second distinct SDDL and verify the
  // alternate stream immediately reflects the change.
  const std::wstring sddl_two = L"D:P(A;;FA;;;" + *user_sid.value + L")";
  auto expected_two = canonicalize_dacl(sddl_two);
  ASSERT_WIN32_OK(expected_two);

  auto set_base =
      core::SetFileSddl(base_path, DACL_SECURITY_INFORMATION, sddl_two);
  ASSERT_WIN32_OK(set_base);

  auto base_sddl2 = core::GetFileSddl(base_path, DACL_SECURITY_INFORMATION);
  auto stream_sddl2 = core::GetFileSddl(stream_path, DACL_SECURITY_INFORMATION);
  ASSERT_WIN32_OK(base_sddl2);
  ASSERT_WIN32_OK(stream_sddl2);
  EXPECT_EQ(*base_sddl2.value, *expected_two.value);
  EXPECT_EQ(*stream_sddl2.value, *base_sddl2.value);
  EXPECT_NE(*stream_sddl2.value, *stream_sddl1.value);

  dacl_guard.RestoreNow();
}

void ForceLink_StreamsOps() {}

} // namespace fstest
