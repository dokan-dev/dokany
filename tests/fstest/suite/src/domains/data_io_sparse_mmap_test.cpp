// SPDX-License-Identifier: MIT

#include "fstest/suite/register_suites.h"

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/status.h"
#include "fstest/suite/test_fixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {
namespace {

// Windows 8 / Server 2012: the oldest build this suite supports, and the first
// whose NTFS accepts FSCTL_SET_SPARSE(FALSE) on a file that still has holes.
constexpr uint32_t kBuildWin8 = 9200;

uint64_t CombineU64(DWORD high, DWORD low) noexcept {
  return (static_cast<uint64_t>(high) << 32) | static_cast<uint64_t>(low);
}

core::Win32Result<uint64_t> QueryCompressedSize(const std::wstring &path) {
  core::Win32Result<uint64_t> res{};
  DWORD high = 0;
  ::SetLastError(ERROR_SUCCESS);
  const DWORD low = ::GetCompressedFileSizeW(path.c_str(), &high);
  if (low == INVALID_FILE_SIZE) {
    const DWORD err = ::GetLastError();
    if (err != ERROR_SUCCESS) {
      res.error = err;
      return res;
    }
  }
  res.error = ERROR_SUCCESS;
  res.value = CombineU64(high, low);
  return res;
}

struct MappedFileView {
  // Declared in RAII teardown order: view -> mapping -> file.
  core::UniqueHandle file;
  core::UniqueHandle mapping;
  core::UniqueViewOfFile view;

  char *bytes() const noexcept { return static_cast<char *>(view.get()); }

  void UnmapAndCloseMapping() noexcept {
    view.reset();
    mapping.reset();
  }

  void Reset() noexcept {
    UnmapAndCloseMapping();
    file.reset();
  }
};

core::Win32Result<void> OpenMappedView(std::wstring_view path,
                                       DWORD desired_access, DWORD fl_protect,
                                       DWORD map_access, size_t map_size,
                                       MappedFileView *out) {
  core::Win32Result<void> res{};
  out->Reset();

  auto h = core::CreateWin32File(path, desired_access, core::kShareAll,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  if (!h) {
    res.error = h.error;
    return res;
  }

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle mapping(
      ::CreateFileMappingW(h.value->get(), nullptr, fl_protect, 0,
                           static_cast<DWORD>(map_size), nullptr));
  const DWORD mapping_err = ::GetLastError();
  if (!mapping.valid()) {
    res.error =
        (mapping_err != ERROR_SUCCESS) ? mapping_err : ERROR_GEN_FAILURE;
    return res;
  }

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueViewOfFile view(
      ::MapViewOfFile(mapping.get(), map_access, 0, 0, map_size));
  const DWORD view_err = ::GetLastError();
  if (!view) {
    res.error = (view_err != ERROR_SUCCESS) ? view_err : ERROR_GEN_FAILURE;
    return res;
  }

  out->file = std::move(*h.value);
  out->mapping = std::move(mapping);
  out->view = std::move(view);
  res.error = ERROR_SUCCESS;
  return res;
}

core::Win32Result<core::UniqueHandle>
CreateSparseFile(std::wstring_view path, int64_t initial_eof = 0) {
  core::Win32Result<core::UniqueHandle> res{};
  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  if (!h) {
    res.error = h.error;
    return res;
  }

  auto sp = core::SetSparseFile(h.value->get(), /*sparse=*/true);
  if (!sp) {
    res.error = sp.error;
    return res;
  }

  if (initial_eof > 0) {
    if (!SeekBegin(h.value->get(), initial_eof) ||
        ::SetEndOfFile(h.value->get()) == FALSE) {
      const DWORD err = ::GetLastError();
      res.error = (err != ERROR_SUCCESS) ? err : ERROR_GEN_FAILURE;
      return res;
    }
  }

  res.error = ERROR_SUCCESS;
  res.value = std::move(*h.value);
  return res;
}

core::Win32Result<void> WriteFilledBytes(HANDLE h, size_t total_size,
                                         uint8_t value,
                                         size_t chunk_size = 65536u) {
  core::Win32Result<void> res{};
  std::vector<uint8_t> chunk(std::min(total_size, chunk_size), value);
  size_t remaining = total_size;
  while (remaining > 0) {
    const size_t step = std::min(remaining, chunk.size());
    auto wr = core::WriteAllBytes(h, chunk.data(), step);
    if (!wr) {
      res.error = wr.error;
      return res;
    }
    remaining -= step;
  }
  res.error = ERROR_SUCCESS;
  return res;
}

::testing::AssertionResult CheckHandleAttributeBit(HANDLE h, DWORD bit,
                                                   bool expected_set) {
  auto info = core::GetByHandleInfo(h);
  if (!info) {
    return ::testing::AssertionFailure()
           << "GetByHandleInfo failed: " << core::FormatWin32Error(info.error);
  }
  const bool actual_set = (info.value->dwFileAttributes & bit) != 0u;
  if (actual_set != expected_set) {
    return ::testing::AssertionFailure()
           << "Expected attribute bit 0x" << std::hex << bit
           << (expected_set ? " to be set" : " to be clear")
           << ", got attributes 0x" << info.value->dwFileAttributes;
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckFileSize(HANDLE h, int64_t expected_size) {
  LARGE_INTEGER sz{};
  if (::GetFileSizeEx(h, &sz) == FALSE) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "GetFileSizeEx failed: " << core::FormatWin32Error(err);
  }
  if (sz.QuadPart != expected_size) {
    return ::testing::AssertionFailure()
           << "Expected file size " << expected_size << ", got " << sz.QuadPart;
  }
  return ::testing::AssertionSuccess();
}

bool RangeCoveredBy(const std::vector<FILE_ALLOCATED_RANGE_BUFFER> &ranges,
                    int64_t start, int64_t length) noexcept {
  int64_t covered_until = start;
  const int64_t end = start + length;
  for (const auto &r : ranges) {
    const int64_t r_start = r.FileOffset.QuadPart;
    const int64_t r_end = r_start + r.Length.QuadPart;
    if (r_start <= covered_until && r_end > covered_until) {
      covered_until = std::min(end, r_end);
      if (covered_until >= end) {
        return true;
      }
    }
  }
  return covered_until >= end;
}

bool RangeOverlapsAny(const std::vector<FILE_ALLOCATED_RANGE_BUFFER> &ranges,
                      int64_t start, int64_t length) noexcept {
  const int64_t end = start + length;
  for (const auto &r : ranges) {
    const int64_t r_start = r.FileOffset.QuadPart;
    const int64_t r_end = r_start + r.Length.QuadPart;
    if (r_start < end && r_end > start) {
      return true;
    }
  }
  return false;
}

::testing::AssertionResult VerifyFilledChunkAt(HANDLE h, int64_t offset,
                                               size_t len, uint8_t expected) {
  if (!SeekBegin(h, offset)) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "SetFilePointerEx(" << offset
           << ") failed: " << core::FormatWin32Error(err);
  }
  std::vector<uint8_t> buf(len, static_cast<uint8_t>(expected ^ 0xFFu));
  DWORD bytes_read = 0;
  if (::ReadFile(h, buf.data(), static_cast<DWORD>(len), &bytes_read,
                 nullptr) == FALSE) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "ReadFile at offset " << offset
           << " failed: " << core::FormatWin32Error(err);
  }
  if (bytes_read != static_cast<DWORD>(len)) {
    return ::testing::AssertionFailure()
           << "Short read at offset " << offset << ": got " << bytes_read
           << " bytes, expected " << len;
  }
  if (!helpers::AllBytesEqual(buf, expected)) {
    return ::testing::AssertionFailure()
           << "Byte mismatch in [" << offset << ", " << (offset + len)
           << "): expected all 0x" << std::hex
           << static_cast<unsigned int>(expected);
  }
  return ::testing::AssertionSuccess();
}

// Page-aligned, zero-initialized block (VirtualAlloc) for
// FILE_FLAG_NO_BUFFERING transfers.
class PageAlignedBuffer {
public:
  explicit PageAlignedBuffer(size_t size) noexcept
      : ptr_(static_cast<uint8_t *>(::VirtualAlloc(
            nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {}

  ~PageAlignedBuffer() {
    if (ptr_ != nullptr) {
      ::VirtualFree(ptr_, 0, MEM_RELEASE);
    }
  }

  PageAlignedBuffer(const PageAlignedBuffer &) = delete;
  PageAlignedBuffer &operator=(const PageAlignedBuffer &) = delete;

  uint8_t *data() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
  uint8_t *ptr_ = nullptr;
};

// Opens `path` with FILE_FLAG_NO_BUFFERING, reads exactly `size` bytes into a
// page-aligned buffer, and stores them in `*out`.
::testing::AssertionResult ReadFileUnbuffered(const std::wstring &path,
                                              size_t size, std::string *out) {
  PageAlignedBuffer buf(size);
  if (!buf) {
    return ::testing::AssertionFailure()
           << "VirtualAlloc(" << size << ") failed";
  }
  auto h =
      core::CreateWin32File(path, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING);
  if (!h) {
    return ::testing::AssertionFailure()
           << "Unbuffered open of '" << core::WideToUtf8(path)
           << "' failed: " << core::FormatWin32Error(h.error);
  }
  DWORD bytes_read = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::ReadFile(h.value->get(), buf.data(), static_cast<DWORD>(size),
                 &bytes_read, nullptr) == FALSE) {
    const DWORD err = ::GetLastError();
    return ::testing::AssertionFailure()
           << "Unbuffered ReadFile failed: " << core::FormatWin32Error(err);
  }
  if (bytes_read != static_cast<DWORD>(size)) {
    return ::testing::AssertionFailure()
           << "Unbuffered short read: got " << bytes_read << " bytes, expected "
           << size;
  }
  out->assign(reinterpret_cast<const char *>(buf.data()), size);
  return ::testing::AssertionSuccess();
}

// Issues one raw FSCTL_QUERY_ALLOCATED_RANGES for [offset, offset + length)
// and returns its Win32 status (ERROR_SUCCESS when the call succeeded), with
// no client-side validation of the arguments.
DWORD RawQueryAllocatedRangesError(HANDLE h, int64_t offset,
                                   int64_t length) noexcept {
  FILE_ALLOCATED_RANGE_BUFFER query{};
  query.FileOffset.QuadPart = offset;
  query.Length.QuadPart = length;
  FILE_ALLOCATED_RANGE_BUFFER out[4]{};
  DWORD bytes_ret = 0;
  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok =
      ::DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES, &query,
                        static_cast<DWORD>(sizeof(query)), out,
                        static_cast<DWORD>(sizeof(out)), &bytes_ret, nullptr);
  if (ok != FALSE) {
    return ERROR_SUCCESS;
  }
  const DWORD err = ::GetLastError();
  return (err != ERROR_SUCCESS) ? err : static_cast<DWORD>(ERROR_GEN_FAILURE);
}

} // namespace

// Spec: Win32 MapViewOfFile / FlushViewOfFile -- bytes stored through a
// PAGE_READWRITE view are returned by ReadFile on the mapped file's handle
// once the view has been flushed.
// (Mapping a 0-byte file is covered by
// MemoryMappedZeroByteFileFailsFileInvalid.)
TEST_F(DataIoTest, MemoryMappedViewWriteVisibleToReadFile) {
  const std::wstring path = TestPath(L"mmap_view_to_read.dat");
  constexpr size_t kMapSize = 8192;
  auto wr = core::WriteFileContents(path, std::string(kMapSize, 'A'));
  ASSERT_WIN32_OK(wr);

  MappedFileView mv;
  auto open_res =
      OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                     FILE_MAP_ALL_ACCESS, kMapSize, &mv);
  ASSERT_WIN32_OK(open_res);

  char *mapped = mv.bytes();
  EXPECT_TRUE(helpers::AllBytesEqual(std::string_view(mapped, kMapSize), 'A'));

  const std::string from_map = "MAPPED_WRITE_01!";
  std::memcpy(mapped, from_map.data(), from_map.size());
  ASSERT_WIN32_TRUE(::FlushViewOfFile(mv.view.get(), kMapSize));

  ASSERT_WIN32_TRUE(SeekBegin(mv.file.get(), 0));
  char read_buf[16]{};
  DWORD bytes_read = 0;
  ASSERT_WIN32_TRUE(
      ::ReadFile(mv.file.get(), read_buf, 16, &bytes_read, nullptr));
  EXPECT_EQ(bytes_read, 16u);
  EXPECT_EQ(std::string(read_buf, 16), from_map);

  mv.Reset();
}

// Spec: Win32 MapViewOfFile -- WriteFile on a mapped file is immediately
// visible through an already mapped (and already faulted-in) view of it,
// because file I/O and the mapped section share one cached image.
TEST_F(DataIoTest, MemoryMappedWriteFileVisibleInView) {
  const std::wstring path = TestPath(L"mmap_write_to_view.dat");
  constexpr size_t kMapSize = 8192;
  auto wr = core::WriteFileContents(path, std::string(kMapSize, 'A'));
  ASSERT_WIN32_OK(wr);

  MappedFileView mv;
  auto open_res =
      OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                     FILE_MAP_ALL_ACCESS, kMapSize, &mv);
  ASSERT_WIN32_OK(open_res);

  // Touch every page first so the view holds the pre-write image.
  char *mapped = mv.bytes();
  ASSERT_TRUE(helpers::AllBytesEqual(std::string_view(mapped, kMapSize), 'A'));

  const std::string from_file = "FILE_WRITE_TO_MM";
  ASSERT_WIN32_TRUE(SeekBegin(mv.file.get(), 4096));
  ASSERT_WIN32_OK(
      core::WriteAllBytes(mv.file.get(), from_file.data(), from_file.size()));
  EXPECT_EQ(std::string(mapped + 4096, from_file.size()), from_file);

  mv.Reset();
}

// Spec: [MS-FSA] 2.1.5.15.5 FileEndOfFileInformation -- shrinking a file with
// an active user-mapped section fails ERROR_USER_MAPPED_FILE until unmapped.
TEST_F(DataIoTest, TruncateMappedFileFailsUserMappedFile) {
  const std::wstring path = TestPath(L"trunc_mapped.dat");
  constexpr size_t kInitialSize = 8192;
  constexpr int64_t kShrinkSize = 4096;

  auto init = core::WriteFileContents(path, std::string(kInitialSize, 'M'));
  ASSERT_WIN32_OK(init);

  MappedFileView mv;
  auto open_res =
      OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                     FILE_MAP_ALL_ACCESS, kInitialSize, &mv);
  ASSERT_WIN32_OK(open_res);

  ASSERT_WIN32_TRUE(SeekBegin(mv.file.get(), kShrinkSize));
  EXPECT_WIN32_ERROR(::SetEndOfFile(mv.file.get()), ERROR_USER_MAPPED_FILE);

  auto nt_shrink = core::SetEndOfFileNt(mv.file.get(), kShrinkSize);
  EXPECT_NT_STATUS(nt_shrink.status, STATUS_USER_MAPPED_FILE);
  EXPECT_TRUE(CheckFileSize(mv.file.get(), static_cast<int64_t>(kInitialSize)));

  // Unmap and close the section handle, then shrink must succeed.
  mv.UnmapAndCloseMapping();

  ASSERT_WIN32_TRUE(SeekBegin(mv.file.get(), kShrinkSize));
  ASSERT_WIN32_TRUE(::SetEndOfFile(mv.file.get()));
  EXPECT_TRUE(CheckFileSize(mv.file.get(), kShrinkSize));
}

// Spec: [MS-FSA] 2.1.5.15.1 FileAllocationInformation -- shrinking allocation
// size below an active user-mapped section fails STATUS_USER_MAPPED_FILE and
// leaves EndOfFile and data intact until unmapped.
TEST_F(DataIoTest, FileAllocationInfoTruncateMappedFileFailsUserMappedFile) {
  const std::wstring path = TestPath(L"trunc_alloc_mapped.dat");
  constexpr size_t kInitialSize = 16384u;
  constexpr int64_t kShrinkSize = 4096;
  const std::string original(kInitialSize, 'A');

  ASSERT_WIN32_OK(core::WriteFileContents(path, original));

  MappedFileView mv;
  auto open_res =
      OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                     FILE_MAP_ALL_ACCESS, kInitialSize, &mv);
  ASSERT_WIN32_OK(open_res);

  auto nt_shrink = core::SetAllocationSizeNt(mv.file.get(), kShrinkSize);
  EXPECT_NT_STATUS(nt_shrink.status, STATUS_USER_MAPPED_FILE);
  EXPECT_TRUE(CheckFileSize(mv.file.get(), static_cast<int64_t>(kInitialSize)));
  EXPECT_TRUE(
      helpers::AllBytesEqual(std::string_view(mv.bytes(), kInitialSize), 'A'));

  // Unmap and close the section handle, then allocation shrink must succeed.
  mv.UnmapAndCloseMapping();

  ASSERT_NT_SUCCESS(
      core::SetAllocationSizeNt(mv.file.get(), kShrinkSize).status);
  EXPECT_TRUE(CheckFileSize(mv.file.get(), kShrinkSize));

  mv.Reset();
  EXPECT_TRUE(helpers::CheckFileContents(
      path, std::string(static_cast<size_t>(kShrinkSize), 'A')));
}

// Spec: Win32 CreateFileMappingW -- PAGE_READWRITE with MaximumSize > EOF
// extends the underlying file and zero-fills the newly extended region.
TEST_F(DataIoTest, MemoryMappedCreateFileMappingExtendsEof) {
  const std::wstring path = TestPath(L"mmap_extend_eof.dat");
  const std::string initial = "0123456789";
  constexpr DWORD kExtendedSize = 8192;

  auto init = core::WriteFileContents(path, initial);
  ASSERT_WIN32_OK(init);

  {
    auto h = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                   core::kShareAll, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    ::SetLastError(ERROR_SUCCESS);
    core::UniqueHandle mapping(::CreateFileMappingW(
        h.value->get(), nullptr, PAGE_READWRITE, 0, kExtendedSize, nullptr));
    const DWORD mapping_err = ::GetLastError();
    ASSERT_TRUE(mapping.valid()) << core::FormatWin32Error(mapping_err);
  }

  auto h_verify = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_verify);
  EXPECT_TRUE(CheckFileSize(h_verify.value->get(), kExtendedSize));

  auto contents = core::ReadAllBytes(h_verify.value->get());
  ASSERT_WIN32_OK(contents);
  ASSERT_EQ(contents.value->size(), static_cast<size_t>(kExtendedSize));
  EXPECT_EQ(contents.value->substr(0, initial.size()), initial);
  EXPECT_TRUE(helpers::AllBytesEqual(
      std::string_view(*contents.value).substr(initial.size()), '\0'));
}

// Spec: Win32 CreateFileMappingW / MapViewOfFile -- PAGE_WRITECOPY with
// FILE_MAP_COPY mutates private copy-on-write pages without altering disk data.
TEST_F(DataIoTest, MemoryMappedWriteCopyDoesNotModifyFile) {
  const std::wstring path = TestPath(L"mmap_writecopy.dat");
  constexpr size_t kMapSize = 8192;
  const std::string original(kMapSize, 'C');

  auto init = core::WriteFileContents(path, original);
  ASSERT_WIN32_OK(init);

  {
    MappedFileView mv;
    auto open_res = OpenMappedView(path, GENERIC_READ, PAGE_WRITECOPY,
                                   FILE_MAP_COPY, kMapSize, &mv);
    ASSERT_WIN32_OK(open_res);

    char *mapped = mv.bytes();
    EXPECT_TRUE(
        helpers::AllBytesEqual(std::string_view(mapped, kMapSize), 'C'));

    const std::string cow_head = "COW_MUTATED_HEAD";
    const std::string cow_page2 = "COW_MUTATED_PG02";
    std::memcpy(mapped, cow_head.data(), cow_head.size());
    std::memcpy(mapped + 4096, cow_page2.data(), cow_page2.size());

    EXPECT_EQ(std::string(mapped, cow_head.size()), cow_head);
    EXPECT_EQ(std::string(mapped + 4096, cow_page2.size()), cow_page2);
    EXPECT_TRUE(helpers::AllBytesEqual(
        std::string_view(mapped + cow_head.size(), 4096u - cow_head.size()),
        'C'));
  }

  EXPECT_TRUE(helpers::CheckFileContents(path, original));
}

// Spec: Win32 CreateFileMappingW -- mapping a 0-byte file with PAGE_READONLY
// and MaximumSize == 0 fails with ERROR_FILE_INVALID (1006).
TEST_F(DataIoTest, MemoryMappedZeroByteFileFailsFileInvalid) {
  const std::wstring path = TestPath(L"mmap_zero_ro.dat");
  ASSERT_WIN32_OK(helpers::CreateEmptyFileResult(path));

  auto h_ro = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_ro);

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle mapping(::CreateFileMappingW(
      h_ro.value->get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
  const DWORD err = ::GetLastError();
  EXPECT_FALSE(mapping.valid());
  EXPECT_WIN32_ERROR_CODE(err, ERROR_FILE_INVALID);
  EXPECT_TRUE(CheckFileSize(h_ro.value->get(), 0));
}

// Spec: Win32 FlushViewOfFile -- flushing a PAGE_READWRITE mapped view succeeds
// and the modified pages are returned by ReadFile after the view is unmapped
// and the file reopened.
//
// On a cached file system this read-back comes from the same cached image the
// view modified, so it cannot tell whether FlushViewOfFile wrote anything; it
// verifies the flush call succeeds and no mapped write is lost.
// MemoryMappedFlushViewOfFileReachesUnbufferedReader checks that the flushed
// pages actually reached the file system.
TEST_F(DataIoTest, MemoryMappedFlushViewOfFilePersistsData) {
  const std::wstring path = TestPath(L"mmap_flush_view.dat");
  constexpr size_t kMapSize = 16384;
  constexpr char kInitByte = 'F';

  auto init = core::WriteFileContents(path, std::string(kMapSize, kInitByte));
  ASSERT_WIN32_OK(init);

  const std::string mark0 = "FLUSH_PAGE_0_DATA";
  const std::string mark1 = "FLUSH_PAGE_1_DATA";
  const std::string mark3 = "FLUSH_PAGE_3_DATA";

  {
    MappedFileView mv;
    auto open_res =
        OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                       FILE_MAP_WRITE, kMapSize, &mv);
    ASSERT_WIN32_OK(open_res);

    char *mapped = mv.bytes();
    std::memcpy(mapped, mark0.data(), mark0.size());
    std::memcpy(mapped + 4096, mark1.data(), mark1.size());
    std::memcpy(mapped + 12288, mark3.data(), mark3.size());
    ASSERT_WIN32_TRUE(::FlushViewOfFile(mv.view.get(), kMapSize));
  }

  auto data = core::ReadFileContents(path);
  ASSERT_WIN32_OK(data);
  ASSERT_EQ(data.value->size(), kMapSize);
  EXPECT_EQ(data.value->substr(0, mark0.size()), mark0);
  EXPECT_EQ(data.value->substr(4096, mark1.size()), mark1);
  EXPECT_EQ(data.value->substr(12288, mark3.size()), mark3);
  EXPECT_TRUE(helpers::AllBytesEqual(
      std::string_view(data.value->data() + 8192, 4096u), kInitByte));
}

// Spec: Win32 FlushViewOfFile -- after FlushViewOfFile returns, the modified
// pages have been written to the file system: a FILE_FLAG_NO_BUFFERING reader
// (which bypasses the cached image the view shares) sees them while the view
// is still mapped. Without the flush, pages dirtied only through the view are
// not yet known to the cache and the unbuffered read would return stale data.
TEST_F(DataIoTest, MemoryMappedFlushViewOfFileReachesUnbufferedReader) {
  const std::wstring path = TestPath(L"mmap_flush_unbuffered.dat");
  const size_t sector = EffectiveSectorSize();
  ASSERT_GE(sector, 512u);
  const size_t map_size = sector * 4u;
  constexpr char kInitByte = 'U';

  std::string expected(map_size, kInitByte);
  auto init = core::WriteFileContents(path, expected);
  ASSERT_WIN32_OK(init);

  const std::string mark0 = "UNBUF_PAGE_0_DATA";
  const std::string mark1 = "UNBUF_PAGE_1_DATA";
  const std::string mark3 = "UNBUF_PAGE_3_DATA";
  expected.replace(0, mark0.size(), mark0);
  expected.replace(sector, mark1.size(), mark1);
  expected.replace(sector * 3u, mark3.size(), mark3);

  MappedFileView mv;
  auto open_res = OpenMappedView(path, GENERIC_READ | GENERIC_WRITE,
                                 PAGE_READWRITE, FILE_MAP_WRITE, map_size, &mv);
  ASSERT_WIN32_OK(open_res);

  char *mapped = mv.bytes();
  std::memcpy(mapped, mark0.data(), mark0.size());
  std::memcpy(mapped + sector, mark1.data(), mark1.size());
  std::memcpy(mapped + sector * 3u, mark3.data(), mark3.size());
  ASSERT_WIN32_TRUE(::FlushViewOfFile(mv.view.get(), map_size));

  // Read the whole file through an unbuffered handle while the view is still
  // mapped.
  std::string actual;
  ASSERT_TRUE(ReadFileUnbuffered(path, map_size, &actual));
  const std::string_view actual_sv(actual);
  EXPECT_EQ(actual_sv.substr(0, mark0.size()), mark0);
  EXPECT_EQ(actual_sv.substr(sector, mark1.size()), mark1);
  EXPECT_EQ(actual_sv.substr(sector * 3u, mark3.size()), mark3);
  // Everything else, including the untouched third page, is unchanged.
  EXPECT_EQ(std::memcmp(actual.data(), expected.data(), map_size), 0);
}

// Spec: Win32 MapViewOfFile / UnmapViewOfFile -- pages dirtied through a
// PAGE_READWRITE view are not lost when the view is unmapped and the section
// and file handles are closed WITHOUT FlushViewOfFile: the file system must
// accept the resulting paging writes after handle cleanup, so a later open
// reads the modified data.
TEST_F(DataIoTest, MemoryMappedDirtyPagesPersistAfterCloseWithoutFlush) {
  const std::wstring path = TestPath(L"mmap_dirty_no_flush.dat");
  constexpr size_t kMapSize = 12288;
  constexpr char kInitByte = 'D';

  std::string expected(kMapSize, kInitByte);
  auto init = core::WriteFileContents(path, expected);
  ASSERT_WIN32_OK(init);

  const std::string mark0 = "DIRTY_PAGE_0_DATA";
  const std::string mark1 = "DIRTY_PAGE_1_DATA";
  const std::string mark2 = "DIRTY_PAGE_2_DATA";
  expected.replace(100, mark0.size(), mark0);
  expected.replace(4096 + 200, mark1.size(), mark1);
  expected.replace(8192 + 300, mark2.size(), mark2);

  {
    MappedFileView mv;
    auto open_res =
        OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                       FILE_MAP_WRITE, kMapSize, &mv);
    ASSERT_WIN32_OK(open_res);

    char *mapped = mv.bytes();
    std::memcpy(mapped + 100, mark0.data(), mark0.size());
    std::memcpy(mapped + 4096 + 200, mark1.data(), mark1.size());
    std::memcpy(mapped + 8192 + 300, mark2.data(), mark2.size());

    // Deliberately no FlushViewOfFile / FlushFileBuffers: unmap the view,
    // close the section, then close the file handle.
    mv.Reset();
  }

  EXPECT_TRUE(helpers::CheckFileContents(path, expected));
}

// Spec: Win32 MapViewOfFile / FlushViewOfFile / [MS-FSA] 2.1.5.4 -- pages
// dirtied through a PAGE_READWRITE view after the originating file handle has
// already been closed (IRP_MJ_CLEANUP / FO_CLEANUP_COMPLETE completed before
// dirtying) must be persisted via post-cleanup paging writes and returned by a
// subsequent FILE_FLAG_NO_BUFFERING reader.
TEST_F(DataIoTest, MemoryMappedDirtyPagesPersistAfterHandleCleanupBeforeWrite) {
  const std::wstring path = TestPath(L"mmap_post_cleanup_write.dat");
  const size_t sector = EffectiveSectorSize();
  ASSERT_GE(sector, 512u);
  const size_t map_size = sector * 2u;
  constexpr char kInitByte = 'P';

  std::string expected(map_size, kInitByte);
  ASSERT_WIN32_OK(core::WriteFileContents(path, expected));

  const std::string mark0 = "POST_CLEANUP_PG0!";
  const std::string mark1 = "POST_CLEANUP_PG1!";
  expected.replace(64, mark0.size(), mark0);
  expected.replace(sector + 128, mark1.size(), mark1);

  {
    MappedFileView mv;
    auto open_res =
        OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                       FILE_MAP_WRITE, map_size, &mv);
    ASSERT_WIN32_OK(open_res);

    // Close the file handle FIRST so IRP_MJ_CLEANUP completes before any page
    // is dirtied; subsequent flush/unmap must issue post-cleanup paging writes.
    mv.file.reset();

    char *mapped = mv.bytes();
    std::memcpy(mapped + 64, mark0.data(), mark0.size());
    std::memcpy(mapped + sector + 128, mark1.data(), mark1.size());
    ASSERT_WIN32_TRUE(::FlushViewOfFile(mv.view.get(), map_size));
    mv.UnmapAndCloseMapping();
  }

  std::string actual;
  ASSERT_TRUE(ReadFileUnbuffered(path, map_size, &actual));
  const std::string_view actual_sv(actual);
  EXPECT_EQ(actual_sv.substr(64, mark0.size()), mark0);
  EXPECT_EQ(actual_sv.substr(sector + 128, mark1.size()), mark1);
  EXPECT_EQ(std::memcmp(actual.data(), expected.data(), map_size), 0);
}

// Spec: [MS-FSA] 2.1.5.4 / Win32 MapViewOfFile -- paging writes must never
// extend EndOfFile: dirtying trailing page slack past a non-page-aligned file
// size and flushing must preserve EndOfFile and persist only [0, EndOfFile).
TEST_F(DataIoTest, MemoryMappedNonPageAlignedDirtyTailPreservesEof) {
  const std::wstring path = TestPath(L"mmap_non_page_aligned_tail.dat");
  constexpr size_t kFileSize = 5000u;
  constexpr size_t kMinRegionSize = 8192u;
  constexpr char kInitByte = 'T';

  std::string expected(kFileSize, kInitByte);
  ASSERT_WIN32_OK(core::WriteFileContents(path, expected));

  const std::string head_mark = "TAIL_TEST_HEAD_0";
  const std::string page2_mark = "TAIL_TEST_PAGE_1";
  expected.replace(0, head_mark.size(), head_mark);
  expected.replace(4096, page2_mark.size(), page2_mark);

  {
    MappedFileView mv;
    auto open_res =
        OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                       FILE_MAP_WRITE, /*map_size=*/0, &mv);
    ASSERT_WIN32_OK(open_res);

    MEMORY_BASIC_INFORMATION mbi{};
    ASSERT_EQ(::VirtualQuery(mv.view.get(), &mbi, sizeof(mbi)), sizeof(mbi));
    ASSERT_GE(mbi.RegionSize, kMinRegionSize);

    char *mapped = mv.bytes();
    std::memcpy(mapped, head_mark.data(), head_mark.size());
    std::memcpy(mapped + 4096, page2_mark.data(), page2_mark.size());
    // Dirty the trailing page slack [5000, 8192) past EndOfFile.
    std::memset(mapped + kFileSize, 0x5A, kMinRegionSize - kFileSize);
    ASSERT_WIN32_TRUE(::FlushViewOfFile(mv.view.get(), 0));
    mv.Reset();
  }

  auto h_verify = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_verify);
  EXPECT_TRUE(
      CheckFileSize(h_verify.value->get(), static_cast<int64_t>(kFileSize)));
  auto contents = core::ReadAllBytes(h_verify.value->get());
  ASSERT_WIN32_OK(contents);
  EXPECT_EQ(*contents.value, expected);
}

// Spec: [MS-FSA] 2.1.5.15.5 FileEndOfFileInformation -- extending EndOfFile
// succeeds while a user-mapped section is active (only truncation below the
// mapped size is refused), the mapped view stays intact, and the newly
// exposed bytes read as zeroes.
TEST_F(DataIoTest, ExtendMappedFileSucceedsWithActiveSection) {
  const std::wstring path = TestPath(L"extend_mapped.dat");
  constexpr size_t kInitialSize = 8192;
  constexpr int64_t kWin32ExtendedSize = 16384;
  constexpr int64_t kNtExtendedSize = 24576;

  auto init = core::WriteFileContents(path, std::string(kInitialSize, 'M'));
  ASSERT_WIN32_OK(init);

  MappedFileView mv;
  auto open_res =
      OpenMappedView(path, GENERIC_READ | GENERIC_WRITE, PAGE_READWRITE,
                     FILE_MAP_ALL_ACCESS, kInitialSize, &mv);
  ASSERT_WIN32_OK(open_res);

  ASSERT_WIN32_TRUE(SeekBegin(mv.file.get(), kWin32ExtendedSize));
  ASSERT_WIN32_TRUE(::SetEndOfFile(mv.file.get()));
  EXPECT_TRUE(CheckFileSize(mv.file.get(), kWin32ExtendedSize));

  ASSERT_NT_SUCCESS(
      core::SetEndOfFileNt(mv.file.get(), kNtExtendedSize).status);
  EXPECT_TRUE(CheckFileSize(mv.file.get(), kNtExtendedSize));

  // The existing view still shows the original bytes.
  EXPECT_TRUE(
      helpers::AllBytesEqual(std::string_view(mv.bytes(), kInitialSize), 'M'));

  mv.Reset();

  std::string expected(kInitialSize, 'M');
  expected.append(static_cast<size_t>(kNtExtendedSize) - kInitialSize, '\0');
  EXPECT_TRUE(helpers::CheckFileContents(path, expected));
}

// Spec: [MS-FSA] 2.1.5.1.2 / Win32 CreateFileMappingW(SEC_IMAGE) -- while an
// executable is mapped as an image section, opening it with GENERIC_WRITE
// fails ERROR_SHARING_VIOLATION and GENERIC_READ opens still succeed; write
// opens succeed again once the image section is gone.
// (Truncating under a data section is covered by
// TruncateMappedFileFailsUserMappedFile.)
TEST_F(DataIoTest, ImageMappedFileRejectsWriteOpen) {
  const std::wstring img_path = TestPath(L"sec_image_write_reject.exe");
  auto copy_res = CopyCurrentExecutableTo(img_path);
  ASSERT_WIN32_OK(copy_res);

  auto h_img = core::CreateWin32File(img_path, GENERIC_READ | GENERIC_EXECUTE,
                                     core::kShareAll, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_img);

  // Declare mapping before view so RAII unmaps view before closing section.
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle img_map(::CreateFileMappingW(
      h_img.value->get(), nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr));
  const DWORD map_err = ::GetLastError();
  ASSERT_TRUE(img_map.valid()) << core::FormatWin32Error(map_err);

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueViewOfFile img_view(
      ::MapViewOfFile(img_map.get(), FILE_MAP_READ, 0, 0, 0));
  const DWORD view_err = ::GetLastError();
  ASSERT_TRUE(img_view) << core::FormatWin32Error(view_err);

  // Close the file handle so only the active SEC_IMAGE mapping pins the file.
  h_img.value->reset();

  auto h_write = core::CreateWin32File(img_path, GENERIC_WRITE, core::kShareAll,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(h_write);
  EXPECT_WIN32_ERROR_CODE(h_write.error, ERROR_SHARING_VIOLATION);
  if (h_write) {
    // Do not keep an unexpected write handle open across the checks below.
    h_write.value->reset();
  }

  {
    auto h_read = core::CreateWin32File(img_path, GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_read);
  }

  // Unmap and close section -> write open succeeds once unmapped.
  img_view.reset();
  img_map.reset();

  auto h_write_after =
      core::CreateWin32File(img_path, GENERIC_WRITE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_WIN32_OK(h_write_after);
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 -- FSCTL_SET_SPARSE sets and clears
// FILE_ATTRIBUTE_SPARSE_FILE when the stream contains no sparse ranges.
TEST_F(DataIoTest, SparseSetAndQueryFlag) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_flag.dat");
  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h);

  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));

  // Explicitly test null input buffer on FSCTL_SET_SPARSE (defaults to TRUE).
  DWORD bytes_ret = 0;
  ASSERT_WIN32_TRUE(::DeviceIoControl(h.value->get(), FSCTL_SET_SPARSE, nullptr,
                                      0, nullptr, 0, &bytes_ret, nullptr));
  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, true));

  auto path_attrs = core::QueryWin32Attributes(path);
  ASSERT_WIN32_OK(path_attrs);
  EXPECT_NE(*path_attrs.value & FILE_ATTRIBUTE_SPARSE_FILE, 0u);

  // Clear sparse flag via FILE_SET_SPARSE_BUFFER{FALSE} on a 0-byte file.
  auto cleared = core::SetSparseFile(h.value->get(), false);
  ASSERT_WIN32_OK(cleared);
  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));

  // Also test SetSparse=TRUE then SetSparse=FALSE on a fully allocated 64 KiB
  // file (zero sparse ranges).
  ASSERT_WIN32_OK(WriteFilledBytes(h.value->get(), 65536u, 0x33));

  ASSERT_WIN32_OK(core::SetSparseFile(h.value->get(), true));
  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, true));

  ASSERT_WIN32_OK(core::SetSparseFile(h.value->get(), false));
  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 -- FSCTL_SET_ZERO_DATA deallocates
// holes on sparse files, reads zeroes, and reduces GetCompressedFileSizeW.
TEST_F(DataIoTest, SparseZeroDataDeallocatesAndReadsZeroes) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_zero.dat");
  constexpr size_t kChunk = 65536u;
  constexpr size_t kFileSize = 1024u * 1024u;

  {
    auto h = CreateSparseFile(path);
    ASSERT_WIN32_OK(h);

    // Fully write 1 MiB of non-zero bytes first and flush so cached pages are
    // committed before punching holes.
    auto wr = WriteFilledBytes(h.value->get(), kFileSize, 0x5A, kChunk);
    ASSERT_WIN32_OK(wr);
    ASSERT_WIN32_TRUE(::FlushFileBuffers(h.value->get()));

    // Zero [64 KiB, 512 KiB) (448 KiB hole, 64 KiB aligned).
    auto z1 = core::SetZeroData(h.value->get(), static_cast<int64_t>(kChunk),
                                static_cast<int64_t>(8u * kChunk));
    ASSERT_WIN32_OK(z1);

    // Verify [0, 64 KiB) intact, [64 KiB, 512 KiB) zeroed, [512 KiB, 576 KiB)
    // intact.
    ASSERT_TRUE(VerifyFilledChunkAt(h.value->get(), 0, kChunk, 0x5A));
    for (size_t off = kChunk; off < 8u * kChunk; off += kChunk) {
      ASSERT_TRUE(VerifyFilledChunkAt(h.value->get(), static_cast<int64_t>(off),
                                      kChunk, 0x00));
    }
    ASSERT_TRUE(VerifyFilledChunkAt(
        h.value->get(), static_cast<int64_t>(8u * kChunk), kChunk, 0x5A));

    // Zero [0, 256 KiB) across allocated and already-sparse regions.
    auto z2 =
        core::SetZeroData(h.value->get(), 0, static_cast<int64_t>(4u * kChunk));
    ASSERT_WIN32_OK(z2);
    ASSERT_TRUE(VerifyFilledChunkAt(h.value->get(), 0, kChunk, 0x00));
  }

  // [64 KiB, 512 KiB) was punched out by the first FSCTL_SET_ZERO_DATA (seven
  // 64 KiB chunks) and [0, 64 KiB) by the second, so at least seven chunks of
  // the 1 MiB file must no longer be allocated.
  auto comp = QueryCompressedSize(path);
  ASSERT_WIN32_OK(comp);
  EXPECT_LE(*comp.value, static_cast<uint64_t>(kFileSize - 7u * kChunk));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 -- FSCTL_QUERY_ALLOCATED_RANGES
// reports allocated extents and excludes zeroed and unwritten sparse holes.
TEST_F(DataIoTest, SparseQueryAllocatedRanges) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_ranges.dat");
  constexpr int64_t kChunk = 65536;
  constexpr int64_t kFileSize = 1024 * 1024;

  auto h = CreateSparseFile(path, kFileSize);
  ASSERT_WIN32_OK(h);

  // Write non-zero data at [0, 64 KiB), [256 KiB, 320 KiB), and [512 KiB, 576
  // KiB), then punch out [64 KiB, 512 KiB).
  std::vector<uint8_t> block(static_cast<size_t>(kChunk), 0x4B);
  for (int64_t off : {int64_t{0}, 4 * kChunk, 8 * kChunk}) {
    ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), off));
    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), block.data(), block.size()));
  }

  auto zero_res = core::SetZeroData(h.value->get(), kChunk, 8 * kChunk);
  ASSERT_WIN32_OK(zero_res);

  auto ranges = core::QueryAllocatedRanges(h.value->get(), 0, kFileSize);
  ASSERT_WIN32_OK(ranges);

  ASSERT_GE(ranges.value->size(), 2u);
  EXPECT_TRUE(RangeCoveredBy(*ranges.value, 0, kChunk));
  EXPECT_TRUE(RangeCoveredBy(*ranges.value, 8 * kChunk, kChunk));
  EXPECT_FALSE(RangeOverlapsAny(*ranges.value, kChunk, 7 * kChunk));
  EXPECT_FALSE(
      RangeOverlapsAny(*ranges.value, 9 * kChunk, kFileSize - 9 * kChunk));
}

// Spec: [MS-FSA] 2.1.5.15.5 / 2.1.5.10 -- extending a 0-byte sparse file via
// SetEndOfFile leaves zero allocated extents and reads back as zeroes.
TEST_F(DataIoTest, SparseExtendWithSetEndOfFileLeavesHoleUnallocated) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_extend_eof.dat");
  constexpr int64_t kFileSize = 1024 * 1024;

  {
    auto h = CreateSparseFile(path, kFileSize);
    ASSERT_WIN32_OK(h);

    auto ranges = core::QueryAllocatedRanges(h.value->get(), 0, kFileSize);
    ASSERT_WIN32_OK(ranges);
    EXPECT_TRUE(ranges.value->empty());

    // Full 1 MiB read must return all zeroes.
    ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
    std::vector<uint8_t> buf(65536u, 0xFF);
    int64_t total_read = 0;
    while (total_read < kFileSize) {
      DWORD bytes_read = 0;
      ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), buf.data(),
                                   static_cast<DWORD>(buf.size()), &bytes_read,
                                   nullptr));
      ASSERT_GT(bytes_read, 0u);
      EXPECT_TRUE(helpers::AllBytesEqual(buf.data(), bytes_read, 0x00));
      total_read += bytes_read;
    }
    EXPECT_EQ(total_read, kFileSize);
  }

  auto comp = QueryCompressedSize(path);
  ASSERT_WIN32_OK(comp);
  EXPECT_EQ(*comp.value, 0u);
}

// Spec: [MS-FSCC] 2.3 -- FSCTL_QUERY_ALLOCATED_RANGES returns ERROR_MORE_DATA
// when output buffer is smaller than the allocated extent list and paginates.
TEST_F(DataIoTest, SparseQueryAllocatedRangesBufferPagination) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_paginate.dat");
  constexpr int64_t kChunk = 65536;
  constexpr int64_t kFileSize = 1024 * 1024;
  constexpr std::array<int64_t, 5> kExtentOffsets = {0, 3 * kChunk, 6 * kChunk,
                                                     9 * kChunk, 12 * kChunk};

  auto h = CreateSparseFile(path, kFileSize);
  ASSERT_WIN32_OK(h);

  std::vector<uint8_t> block(static_cast<size_t>(kChunk), 0x6D);
  for (int64_t off : kExtentOffsets) {
    ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), off));
    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), block.data(), block.size()));
  }

  // Output buffer holds 2 entries, so 5 extents must trigger ERROR_MORE_DATA
  // and paginate cleanly.
  std::vector<FILE_ALLOCATED_RANGE_BUFFER> collected;
  bool saw_more_data = false;
  int64_t cursor = 0;
  for (int iter = 0; iter < 32 && cursor < kFileSize; ++iter) {
    FILE_ALLOCATED_RANGE_BUFFER query{};
    query.FileOffset.QuadPart = cursor;
    query.Length.QuadPart = kFileSize - cursor;

    std::array<FILE_ALLOCATED_RANGE_BUFFER, 2> page{};
    DWORD bytes_ret = 0;
    const BOOL ok = ::DeviceIoControl(
        h.value->get(), FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query),
        page.data(), static_cast<DWORD>(sizeof(page)), &bytes_ret, nullptr);
    const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
    if (ok == FALSE) {
      ASSERT_WIN32_ERROR_CODE(err, ERROR_MORE_DATA);
      saw_more_data = true;
    }

    ASSERT_EQ(bytes_ret % sizeof(FILE_ALLOCATED_RANGE_BUFFER), 0u);
    const size_t count = bytes_ret / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
    if (count == 0) {
      ASSERT_NE(ok, FALSE);
      break;
    }

    for (size_t i = 0; i < count; ++i) {
      const int64_t next_cursor =
          page[i].FileOffset.QuadPart + page[i].Length.QuadPart;
      ASSERT_GT(next_cursor, cursor);
      collected.push_back(page[i]);
      cursor = next_cursor;
    }
    if (ok != FALSE) {
      break;
    }
  }

  EXPECT_TRUE(saw_more_data);
  ASSERT_EQ(collected.size(), kExtentOffsets.size());
  for (size_t i = 0; i < kExtentOffsets.size(); ++i) {
    EXPECT_EQ(collected[i].FileOffset.QuadPart, kExtentOffsets[i]);
    EXPECT_EQ(collected[i].Length.QuadPart, kChunk);
  }
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 -- FSCTL_SET_ZERO_DATA on a
// non-sparse file zeroes the range in place without setting SPARSE_FILE.
TEST_F(DataIoTest, SetZeroDataOnNonSparseFileZeroesWithoutSettingSparse) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"non_sparse_zero_data.dat");
  constexpr size_t kFileSize = 128u * 1024u;
  constexpr size_t kZeroStart = 4096u;
  constexpr size_t kZeroEnd = 69632u;

  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h);

  auto wr = WriteFilledBytes(h.value->get(), kFileSize,
                             static_cast<uint8_t>('Z'), kFileSize);
  ASSERT_WIN32_OK(wr);
  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));

  auto zero_res =
      core::SetZeroData(h.value->get(), static_cast<int64_t>(kZeroStart),
                        static_cast<int64_t>(kZeroEnd));
  ASSERT_WIN32_OK(zero_res);

  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));
  auto attrs = core::QueryWin32Attributes(path);
  ASSERT_WIN32_OK(attrs);
  EXPECT_EQ(*attrs.value & FILE_ATTRIBUTE_SPARSE_FILE, 0u);

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  auto contents = core::ReadAllBytes(h.value->get());
  ASSERT_WIN32_OK(contents);
  ASSERT_EQ(contents.value->size(), kFileSize);

  const std::string_view sv(*contents.value);
  EXPECT_TRUE(helpers::AllBytesEqual(sv.substr(0, kZeroStart), 'Z'));
  EXPECT_TRUE(helpers::AllBytesEqual(
      sv.substr(kZeroStart, kZeroEnd - kZeroStart), '\0'));
  EXPECT_TRUE(helpers::AllBytesEqual(sv.substr(kZeroEnd), 'Z'));
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 / 2.1.5.8 / 2.1.5.10 / [MS-FSCC] 2.3 --
// 64-bit offsets >4 GiB work cleanly across SetFilePointerEx, explicit-offset
// OVERLAPPED I/O, LockFileEx, FSCTL_QUERY_ALLOCATED_RANGES, and
// FSCTL_SET_ZERO_DATA on sparse files.
TEST_F(DataIoTest, SparseFile64BitOffsetsOver4GiB) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_over_4gib.dat");
  constexpr int64_t kFiveGiB = 5LL * 1024LL * 1024LL * 1024LL;
  constexpr DWORD kPatternLen = 4096u;
  constexpr int64_t kTotalSize = kFiveGiB + static_cast<int64_t>(kPatternLen);

  // Probe with a 1 MiB hole first. A file system that accepts FSCTL_SET_SPARSE
  // but still backs holes with real storage (or RAM) must be caught here,
  // before this test asks it to extend the file to 5 GiB.
  constexpr int64_t kProbeSize = 1024 * 1024;
  auto h = CreateSparseFile(path, kProbeSize);
  ASSERT_WIN32_OK(h);
  auto probe_ranges = core::QueryAllocatedRanges(h.value->get(), 0, kProbeSize);
  ASSERT_WIN32_OK(probe_ranges);
  ASSERT_TRUE(probe_ranges.value->empty())
      << "A 1 MiB sparse probe reports " << probe_ranges.value->size()
      << " allocated range(s); refusing to extend the file to 5 GiB";

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), kTotalSize));
  ASSERT_WIN32_TRUE(::SetEndOfFile(h.value->get()));
  EXPECT_TRUE(CheckFileSize(h.value->get(), kTotalSize));

  std::vector<uint8_t> pattern(kPatternLen);
  for (size_t i = 0; i < kPatternLen; ++i) {
    pattern[i] = static_cast<uint8_t>(((i * 37u) + 0x23u) | 0x01u);
  }

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), kFiveGiB));
  auto wr = core::WriteAllBytes(h.value->get(), pattern.data(), pattern.size());
  ASSERT_WIN32_OK(wr);

  // Read back via SetFilePointerEx.
  std::vector<uint8_t> sync_buf(kPatternLen, 0);
  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), kFiveGiB));
  DWORD bytes_read = 0;
  ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), sync_buf.data(), kPatternLen,
                               &bytes_read, nullptr));
  EXPECT_EQ(bytes_read, kPatternLen);
  EXPECT_EQ(sync_buf, pattern);

  // Read back via explicit 64-bit OVERLAPPED (OffsetHigh != 0).
  {
    auto h_ov = core::CreateWin32File(
        path, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
    ASSERT_WIN32_OK(h_ov);

    ::SetLastError(ERROR_SUCCESS);
    core::UniqueHandle ev(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const DWORD ev_err = ::GetLastError();
    ASSERT_TRUE(ev.valid()) << core::FormatWin32Error(ev_err);

    std::vector<uint8_t> ov_buf(kPatternLen, 0);
    OVERLAPPED ov =
        MakeOffsetOverlapped(static_cast<uint64_t>(kFiveGiB), ev.get());
    EXPECT_EQ(ov.OffsetHigh, 1u);
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok =
        ::ReadFile(h_ov.value->get(), ov_buf.data(), kPatternLen, nullptr, &ov);
    auto ov_res = helpers::CompleteOverlappedIo(
        h_ov.value->get(), &ov, ev.get(), ok, Config().timing.async_timeout_ms);
    ASSERT_WIN32_OK(ov_res);
    EXPECT_EQ(*ov_res.value, kPatternLen);
    EXPECT_EQ(ov_buf, pattern);
  }

  // Byte-range lock at 5 GiB must conflict at 5 GiB without colliding with
  // the 32-bit truncated alias (1 GiB).
  {
    auto h2 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h2);

    OVERLAPPED lock_ov = MakeOffsetOverlapped(static_cast<uint64_t>(kFiveGiB));
    ASSERT_WIN32_TRUE(::LockFileEx(
        h.value->get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
        /*dwReserved=*/0, kPatternLen, 0, &lock_ov));

    OVERLAPPED conflict_ov =
        MakeOffsetOverlapped(static_cast<uint64_t>(kFiveGiB));
    EXPECT_WIN32_ERROR(
        ::LockFileEx(h2.value->get(),
                     LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                     /*dwReserved=*/0, kPatternLen, 0, &conflict_ov),
        ERROR_LOCK_VIOLATION);

    OVERLAPPED low32_ov =
        MakeOffsetOverlapped(static_cast<uint64_t>(kFiveGiB) & 0xFFFFFFFFULL);
    ASSERT_WIN32_TRUE(::LockFileEx(
        h2.value->get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
        /*dwReserved=*/0, kPatternLen, 0, &low32_ov));
    ASSERT_WIN32_TRUE(::UnlockFileEx(h2.value->get(), /*dwReserved=*/0,
                                     kPatternLen, 0, &low32_ov));

    ASSERT_WIN32_TRUE(::UnlockFileEx(h.value->get(), /*dwReserved=*/0,
                                     kPatternLen, 0, &lock_ov));
  }

  // Allocated range query must cover [5 GiB, 5 GiB + 4096) and leave [0, 5 GiB)
  // unallocated.
  auto ranges = core::QueryAllocatedRanges(h.value->get(), 0, kTotalSize);
  ASSERT_WIN32_OK(ranges);
  EXPECT_TRUE(RangeCoveredBy(*ranges.value, kFiveGiB, kPatternLen));
  EXPECT_FALSE(RangeOverlapsAny(*ranges.value, 0, kFiveGiB));

  // Zero out [5 GiB, 5 GiB + 4096) and verify zeroed readback.
  auto z = core::SetZeroData(h.value->get(), kFiveGiB, kTotalSize);
  ASSERT_WIN32_OK(z);
  ASSERT_TRUE(VerifyFilledChunkAt(h.value->get(), kFiveGiB, kPatternLen, 0x00));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 FSCTL_SET_SPARSE -- on Windows 8 /
// Server 2012 and later, FSCTL_SET_SPARSE(FALSE) succeeds on a file that still
// contains a deallocated hole: FILE_ATTRIBUTE_SPARSE_FILE is cleared, the hole
// keeps reading as zeroes, and data outside the hole is intact.
//
// Only Windows Vista through Windows 7 / Server 2008 R2 restricted clearing
// the flag to files without sparse regions (ERROR_INVALID_PARAMETER); that
// range is below the suite's minimum supported build, so the gate documents
// the threshold instead of accepting both outcomes. Clearing the flag on a
// fully allocated file is covered by SparseSetAndQueryFlag.
TEST_F(DataIoTest, SparseClearFlagWithHoleSucceedsAndPreservesData) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);
  REQUIRE_MIN_OS_BUILD(kBuildWin8);

  const std::wstring path = TestPath(L"sparse_clear_with_hole.dat");
  constexpr size_t kChunk = 65536u;
  constexpr size_t kFileSize = 2u * kChunk; // 128 KiB

  auto h = CreateSparseFile(path);
  ASSERT_WIN32_OK(h);

  ASSERT_WIN32_OK(WriteFilledBytes(h.value->get(), kFileSize, 0x55, kChunk));
  auto punch =
      core::SetZeroData(h.value->get(), 0, static_cast<int64_t>(kChunk));
  ASSERT_WIN32_OK(punch);
  ASSERT_WIN32_TRUE(::FlushFileBuffers(h.value->get()));

  // Confirm a real sparse hole exists before testing FSCTL_SET_SPARSE(FALSE).
  auto ranges_with_hole = core::QueryAllocatedRanges(
      h.value->get(), 0, static_cast<int64_t>(kFileSize));
  ASSERT_WIN32_OK(ranges_with_hole);
  ASSERT_FALSE(RangeOverlapsAny(*ranges_with_hole.value, 0,
                                static_cast<int64_t>(kChunk)));

  auto clear_with_hole = core::SetSparseFile(h.value->get(), false);
  ASSERT_WIN32_OK(clear_with_hole);
  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));
  auto std_info = core::QueryStandardInfo(h.value->get());
  ASSERT_NT_SUCCESS(std_info.status);
  ASSERT_TRUE(std_info.value.has_value());
  EXPECT_GE(std_info.value->allocation_size.QuadPart,
            static_cast<LONGLONG>(kFileSize));
  EXPECT_TRUE(VerifyFilledChunkAt(h.value->get(), 0, kChunk, 0x00));
  EXPECT_TRUE(VerifyFilledChunkAt(h.value->get(), static_cast<int64_t>(kChunk),
                                  kChunk, 0x55));

  h.value->reset();
  auto comp_after = QueryCompressedSize(path);
  ASSERT_WIN32_OK(comp_after);
  EXPECT_GE(*comp_after.value, static_cast<uint64_t>(kFileSize));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 FSCTL_SET_ZERO_DATA -- an unaligned
// sub-cluster range on a sparse file zeroes exactly the requested bytes and
// leaves the adjacent bytes of the same cluster untouched.
TEST_F(DataIoTest, SparseSetZeroDataSubClusterRangeZeroesOnlyRequestedBytes) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"sparse_subcluster_zero.dat");
  constexpr size_t kChunk = 65536u;
  constexpr size_t kFileSize = 2u * kChunk; // 128 KiB

  auto h = CreateSparseFile(path);
  ASSERT_WIN32_OK(h);
  ASSERT_WIN32_OK(WriteFilledBytes(h.value->get(), kFileSize, 0xA7, kChunk));

  constexpr size_t kZeroStart = 1000u;
  constexpr size_t kZeroEnd = 3000u;
  auto sub_zero =
      core::SetZeroData(h.value->get(), static_cast<int64_t>(kZeroStart),
                        static_cast<int64_t>(kZeroEnd));
  ASSERT_WIN32_OK(sub_zero);

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  auto contents = core::ReadAllBytes(h.value->get());
  ASSERT_WIN32_OK(contents);
  ASSERT_EQ(contents.value->size(), kFileSize);

  const auto *bytes = reinterpret_cast<const uint8_t *>(contents.value->data());
  EXPECT_TRUE(helpers::AllBytesEqual(bytes, kZeroStart, 0xA7));
  EXPECT_TRUE(
      helpers::AllBytesEqual(bytes + kZeroStart, kZeroEnd - kZeroStart, 0x00));
  EXPECT_TRUE(
      helpers::AllBytesEqual(bytes + kZeroEnd, kFileSize - kZeroEnd, 0xA7));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 FSCTL_SET_ZERO_DATA -- a range whose
// BeyondFinalZero precedes FileOffset, or whose FileOffset is negative, fails
// ERROR_INVALID_PARAMETER and leaves the file untouched.
TEST_F(DataIoTest, SetZeroDataInvalidRangeFailsInvalidParameter) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"zero_data_bad_range.dat");
  constexpr size_t kFileSize = 65536u;
  const std::string original(kFileSize, 'Q');
  ASSERT_WIN32_OK(core::WriteFileContents(path, original));

  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h);

  auto inverted = core::SetZeroData(h.value->get(), 8192, 4096);
  EXPECT_FALSE(inverted);
  EXPECT_WIN32_ERROR_CODE(inverted.error, ERROR_INVALID_PARAMETER);

  auto negative = core::SetZeroData(h.value->get(), -4096, 4096);
  EXPECT_FALSE(negative);
  EXPECT_WIN32_ERROR_CODE(negative.error, ERROR_INVALID_PARAMETER);

  EXPECT_TRUE(CheckFileSize(h.value->get(), static_cast<int64_t>(kFileSize)));
  h.value->reset();
  EXPECT_TRUE(helpers::CheckFileContents(path, original));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 FSCTL_SET_ZERO_DATA -- a range that
// reaches or lies entirely past EndOfFile succeeds, zeroes only the bytes
// below EndOfFile, and never extends the file.
TEST_F(DataIoTest, SetZeroDataPastEofDoesNotExtendFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"zero_data_past_eof.dat");
  constexpr size_t kFileSize = 65536u;
  constexpr size_t kZeroStart = 32768u;
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(kFileSize, 'Q')));

  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h);

  // [32 KiB, 256 KiB) straddles EndOfFile (64 KiB).
  auto straddle =
      core::SetZeroData(h.value->get(), static_cast<int64_t>(kZeroStart),
                        static_cast<int64_t>(4u * kFileSize));
  ASSERT_WIN32_OK(straddle);
  EXPECT_TRUE(CheckFileSize(h.value->get(), static_cast<int64_t>(kFileSize)));

  // [128 KiB, 256 KiB) lies entirely past EndOfFile.
  auto beyond =
      core::SetZeroData(h.value->get(), static_cast<int64_t>(2u * kFileSize),
                        static_cast<int64_t>(4u * kFileSize));
  EXPECT_WIN32_OK(beyond);
  EXPECT_TRUE(CheckFileSize(h.value->get(), static_cast<int64_t>(kFileSize)));

  h.value->reset();
  std::string expected(kZeroStart, 'Q');
  expected.append(kFileSize - kZeroStart, '\0');
  EXPECT_TRUE(helpers::CheckFileContents(path, expected));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 FSCTL_QUERY_ALLOCATED_RANGES -- on a
// fully written file that is not sparse, the query succeeds and reports the
// whole written range as allocated.
TEST_F(DataIoTest, QueryAllocatedRangesOnNonSparseFileReportsWholeFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"ranges_non_sparse.dat");
  constexpr size_t kFileSize = 128u * 1024u;

  auto h =
      core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h);
  ASSERT_WIN32_OK(WriteFilledBytes(h.value->get(), kFileSize, 0x52));
  ASSERT_WIN32_TRUE(::FlushFileBuffers(h.value->get()));
  ASSERT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));

  auto ranges = core::QueryAllocatedRanges(h.value->get(), 0,
                                           static_cast<int64_t>(kFileSize));
  ASSERT_WIN32_OK(ranges);
  ASSERT_FALSE(ranges.value->empty());
  EXPECT_EQ(ranges.value->front().FileOffset.QuadPart, 0);
  EXPECT_TRUE(
      RangeCoveredBy(*ranges.value, 0, static_cast<int64_t>(kFileSize)));

  // Querying must not turn the file sparse.
  EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                      FILE_ATTRIBUTE_SPARSE_FILE, false));
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 FSCTL_QUERY_ALLOCATED_RANGES -- a
// negative FileOffset or a negative Length fails ERROR_INVALID_PARAMETER.
TEST_F(DataIoTest, QueryAllocatedRangesNegativeRangeFailsInvalidParameter) {
  REQUIRE_FS_CAPABILITY(FsCapability::kSparseFiles);

  const std::wstring path = TestPath(L"ranges_negative.dat");
  constexpr int64_t kFileSize = 1024 * 1024;
  auto h = CreateSparseFile(path, kFileSize);
  ASSERT_WIN32_OK(h);

  // Control: the same raw request with a valid range succeeds.
  EXPECT_WIN32_ERROR_CODE(
      RawQueryAllocatedRangesError(h.value->get(), 0, kFileSize),
      ERROR_SUCCESS);

  EXPECT_WIN32_ERROR_CODE(
      RawQueryAllocatedRangesError(h.value->get(), -4096, kFileSize),
      ERROR_INVALID_PARAMETER);
  EXPECT_WIN32_ERROR_CODE(
      RawQueryAllocatedRangesError(h.value->get(), 0, -4096),
      ERROR_INVALID_PARAMETER);
}

// Spec: [MS-FSA] 2.1.5.10 / [MS-FSCC] 2.3 -- FSCTL_SET_COMPRESSION and
// FSCTL_GET_COMPRESSION toggle LZNT1 compression, shrink physical size on disk,
// and restore uncompressed physical size and data integrity when cleared.
TEST_F(DataIoTest, CompressionSetGetAndCompressedFileSize) {
  REQUIRE_FS_CAPABILITY(FsCapability::kFileCompression);

  const std::wstring path = TestPath(L"compressed.dat");
  constexpr size_t kFileSize = 1024u * 1024u;

  {
    auto h = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                   core::kShareAll, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    auto set_comp =
        core::SetCompressionState(h.value->get(), COMPRESSION_FORMAT_DEFAULT);
    ASSERT_WIN32_OK(set_comp);
    EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                        FILE_ATTRIBUTE_COMPRESSED, true));

    auto got_comp = core::GetCompressionState(h.value->get());
    ASSERT_WIN32_OK(got_comp);
    EXPECT_EQ(*got_comp.value, static_cast<USHORT>(COMPRESSION_FORMAT_LZNT1));

    ASSERT_WIN32_OK(
        WriteFilledBytes(h.value->get(), kFileSize, static_cast<uint8_t>('A')));
    ASSERT_WIN32_TRUE(::FlushFileBuffers(h.value->get()));
  }

  auto comp = QueryCompressedSize(path);
  ASSERT_WIN32_OK(comp);
  EXPECT_LT(*comp.value, static_cast<uint64_t>(kFileSize));

  // Verify compressed content readback integrity.
  auto content = core::ReadFileContents(path);
  ASSERT_WIN32_OK(content);
  ASSERT_EQ(content.value->size(), kFileSize);
  EXPECT_TRUE(helpers::AllBytesEqual(*content.value, 'A'));

  // Clear compression with COMPRESSION_FORMAT_NONE.
  {
    auto h = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                   core::kShareAll, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    auto clear_comp =
        core::SetCompressionState(h.value->get(), COMPRESSION_FORMAT_NONE);
    ASSERT_WIN32_OK(clear_comp);
    EXPECT_TRUE(CheckHandleAttributeBit(h.value->get(),
                                        FILE_ATTRIBUTE_COMPRESSED, false));

    auto got_comp = core::GetCompressionState(h.value->get());
    ASSERT_WIN32_OK(got_comp);
    EXPECT_EQ(*got_comp.value, static_cast<USHORT>(COMPRESSION_FORMAT_NONE));
  }

  // After clearing compression, data remains intact and physical size is
  // uncompressed (>= logical file size).
  auto after_clear = core::ReadFileContents(path);
  ASSERT_WIN32_OK(after_clear);
  ASSERT_EQ(after_clear.value->size(), kFileSize);
  EXPECT_TRUE(helpers::AllBytesEqual(*after_clear.value, 'A'));

  auto comp_after = QueryCompressedSize(path);
  ASSERT_WIN32_OK(comp_after);
  EXPECT_GE(*comp_after.value, static_cast<uint64_t>(kFileSize));
}

// Spec: [MS-FSA] 2.1.5.1.1 / 2.1.5.10 / [MS-FSCC] 2.3 -- setting
// FSCTL_SET_COMPRESSION on a directory causes child files to inherit
// FILE_ATTRIBUTE_COMPRESSED unless created with FILE_NO_COMPRESSION.
TEST_F(DataIoTest, DirectoryCompressionInheritanceAndNoCompressionOption) {
  REQUIRE_FS_CAPABILITY(FsCapability::kFileCompression);

  const std::wstring dir = TestPath(L"comp_inherit_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  {
    auto h_dir = core::CreateWin32File(dir, GENERIC_READ | GENERIC_WRITE,
                                       core::kShareAll, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_WIN32_OK(h_dir);

    auto set_comp = core::SetCompressionState(h_dir.value->get(),
                                              COMPRESSION_FORMAT_DEFAULT);
    ASSERT_WIN32_OK(set_comp);
    EXPECT_TRUE(CheckHandleAttributeBit(h_dir.value->get(),
                                        FILE_ATTRIBUTE_COMPRESSED, true));
    auto got_dir_comp = core::GetCompressionState(h_dir.value->get());
    ASSERT_WIN32_OK(got_dir_comp);
    EXPECT_EQ(*got_dir_comp.value,
              static_cast<USHORT>(COMPRESSION_FORMAT_LZNT1));
  }

  // Standard child file inherits FILE_ATTRIBUTE_COMPRESSED.
  const std::wstring inherited_path =
      TestPath(L"comp_inherit_dir\\inherited.dat");
  {
    auto h_child = core::CreateWin32File(
        inherited_path, GENERIC_READ | GENERIC_WRITE, core::kShareAll,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_child);
    EXPECT_TRUE(CheckHandleAttributeBit(h_child.value->get(),
                                        FILE_ATTRIBUTE_COMPRESSED, true));
    auto got_comp = core::GetCompressionState(h_child.value->get());
    ASSERT_WIN32_OK(got_comp);
    EXPECT_EQ(*got_comp.value, static_cast<USHORT>(COMPRESSION_FORMAT_LZNT1));
  }

  // Child created via NtCreateFile with FILE_NO_COMPRESSION does NOT inherit
  // FILE_ATTRIBUTE_COMPRESSED.
  const std::wstring no_comp_nt_path =
      TestNtPath(L"comp_inherit_dir\\no_comp.dat");
  {
    auto h_no_comp = core::NtCreateFilePath(
        no_comp_nt_path,
        FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileCreate,
        nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert |
            nt::kFileNoCompression);
    ASSERT_NT_SUCCESS(h_no_comp.status);
    ASSERT_TRUE(h_no_comp.value.has_value());

    EXPECT_TRUE(CheckHandleAttributeBit(h_no_comp.value->get(),
                                        FILE_ATTRIBUTE_COMPRESSED, false));
    auto got_comp = core::GetCompressionState(h_no_comp.value->get());
    ASSERT_WIN32_OK(got_comp);
    EXPECT_EQ(*got_comp.value, static_cast<USHORT>(COMPRESSION_FORMAT_NONE));
  }
}

void ForceLink_DataIoSparseMmap() {}

} // namespace fstest
