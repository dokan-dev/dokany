// SPDX-License-Identifier: MIT

#include "fstest/suite/register_suites.h"

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/os_info.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/test_fixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {
namespace {

class AlignedBuffer {
public:
  AlignedBuffer() noexcept = default;
  AlignedBuffer(size_t size, size_t alignment) noexcept
      : ptr_(static_cast<uint8_t *>(::_aligned_malloc(size, alignment))),
        size_(ptr_ != nullptr ? size : 0) {}

  ~AlignedBuffer() { reset(); }

  AlignedBuffer(const AlignedBuffer &) = delete;
  AlignedBuffer &operator=(const AlignedBuffer &) = delete;

  AlignedBuffer(AlignedBuffer &&other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)),
        size_(std::exchange(other.size_, 0)) {}

  AlignedBuffer &operator=(AlignedBuffer &&other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = std::exchange(other.ptr_, nullptr);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }

  uint8_t *data() noexcept { return ptr_; }
  const uint8_t *data() const noexcept { return ptr_; }
  size_t size() const noexcept { return size_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  void reset() noexcept {
    if (ptr_ != nullptr) {
      ::_aligned_free(ptr_);
      ptr_ = nullptr;
      size_ = 0;
    }
  }

private:
  uint8_t *ptr_ = nullptr;
  size_t size_ = 0;
};

uint8_t PatternByte(size_t offset) noexcept {
  // Deterministic non-MZ benign byte pattern.
  return static_cast<uint8_t>(
      ((offset * 37u) + ((offset >> 11) * 17u) + 0x23u) & 0xFFu);
}

core::Win32Result<core::UniqueHandle>
OpenRwFile(std::wstring_view path, DWORD disposition = OPEN_EXISTING,
           DWORD flags = FILE_ATTRIBUTE_NORMAL) {
  return core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                               core::kShareAll, disposition, flags);
}

core::Win32Result<DWORD> OverlappedTransferAt(HANDLE h, uint64_t offset,
                                              void *buf, DWORD len,
                                              bool is_write, DWORD timeout_ms) {
  core::Win32Result<DWORD> res{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle ev(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!ev) {
    const DWORD err = ::GetLastError();
    res.error = (err != ERROR_SUCCESS) ? err : ERROR_GEN_FAILURE;
    return res;
  }

  OVERLAPPED ov = MakeOffsetOverlapped(offset, ev.get());
  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok = is_write ? ::WriteFile(h, buf, len, nullptr, &ov)
                           : ::ReadFile(h, buf, len, nullptr, &ov);
  return helpers::CompleteOverlappedIo(h, &ov, ev.get(), ok, timeout_ms);
}

core::Win32Result<DWORD> OverlappedWriteAt(HANDLE h, uint64_t offset,
                                           const void *buf, DWORD len,
                                           DWORD timeout_ms) {
  return OverlappedTransferAt(h, offset, const_cast<void *>(buf), len,
                              /*is_write=*/true, timeout_ms);
}

core::Win32Result<DWORD> OverlappedReadAt(HANDLE h, uint64_t offset, void *buf,
                                          DWORD len, DWORD timeout_ms) {
  return OverlappedTransferAt(h, offset, buf, len, /*is_write=*/false,
                              timeout_ms);
}

// Cancels `ov` on `file` if it is still in flight and blocks until the kernel
// is done with it, so a caller-owned OVERLAPPED / buffer can safely leave
// scope. Harmless when the request already completed: CancelIoEx then fails
// ERROR_NOT_FOUND and GetOverlappedResult returns without waiting.
void CancelAndDrainOverlapped(HANDLE file, OVERLAPPED *ov) noexcept {
  ::CancelIoEx(file, ov);
  DWORD ignored = 0;
  ::GetOverlappedResult(file, ov, &ignored, /*bWait=*/TRUE);
}

// Issues one overlapped ReadFile/WriteFile on an IOCP-bound handle and
// dequeues its completion packet. `ov` lives in this frame (and `buf` in the
// caller's), so once the request is in flight every exit path either has
// dequeued the packet for `&ov` or cancels and drains the request first; a
// timeout or a foreign packet can therefore never leave the kernel completing
// into a dead stack frame. (A drained request still leaves its packet queued
// on `iocp`; that packet only carries the stale pointer value, which later
// calls report as a mismatch instead of dereferencing.)
::testing::AssertionResult IocpTransferAt(HANDLE file, HANDLE iocp,
                                          ULONG_PTR expected_key,
                                          uint64_t offset, void *buf, DWORD len,
                                          bool is_write, DWORD timeout_ms) {
  OVERLAPPED ov = MakeOffsetOverlapped(offset);
  ::SetLastError(ERROR_SUCCESS);
  const BOOL immediate_ok = is_write ? ::WriteFile(file, buf, len, nullptr, &ov)
                                     : ::ReadFile(file, buf, len, nullptr, &ov);
  if (immediate_ok == FALSE) {
    const DWORD err = ::GetLastError();
    if (err != static_cast<DWORD>(ERROR_IO_PENDING)) {
      // Failed synchronously: nothing is in flight and no packet is queued.
      return ::testing::AssertionFailure()
             << (is_write ? "WriteFile" : "ReadFile")
             << " failed immediately with " << core::FormatWin32Error(err);
    }
  }

  DWORD bytes_transferred = 0;
  ULONG_PTR completion_key = 0;
  LPOVERLAPPED done_ov = nullptr;
  ::SetLastError(ERROR_SUCCESS);
  const BOOL gqcs_ok = ::GetQueuedCompletionStatus(
      iocp, &bytes_transferred, &completion_key, &done_ov, timeout_ms);
  const DWORD gqcs_err = (gqcs_ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();

  if (done_ov != &ov) {
    CancelAndDrainOverlapped(file, &ov);
    if (done_ov == nullptr) {
      return ::testing::AssertionFailure()
             << "GetQueuedCompletionStatus dequeued no packet within "
             << timeout_ms << " ms: " << core::FormatWin32Error(gqcs_err);
    }
    return ::testing::AssertionFailure()
           << "GetQueuedCompletionStatus dequeued a packet for a different "
              "OVERLAPPED (key="
           << completion_key << ", bytes=" << bytes_transferred << ")";
  }
  if (gqcs_ok == FALSE) {
    return ::testing::AssertionFailure()
           << (is_write ? "WriteFile" : "ReadFile") << " completed with "
           << core::FormatWin32Error(gqcs_err);
  }
  if (completion_key != expected_key || bytes_transferred != len) {
    return ::testing::AssertionFailure()
           << "IOCP packet mismatch: key=" << completion_key << " (expected "
           << expected_key << "), bytes=" << bytes_transferred << " (expected "
           << len << ")";
  }
  return ::testing::AssertionSuccess();
}

// Owns the OVERLAPPED blocks, events, and buffers of a batch of overlapped
// requests issued on one handle before any of them is waited on. Every slot is
// heap-allocated and address-stable, and the destructor cancels and drains
// whatever is still in flight, so an early ASSERT_* return can never let the
// kernel complete I/O into freed memory. Declare it after the file handle it
// uses so it is destroyed first.
class OverlappedBatch {
public:
  explicit OverlappedBatch(HANDLE file) noexcept : file_(file) {}

  ~OverlappedBatch() {
    for (auto &slot : slots_) {
      if (slot->in_flight) {
        ::CancelIoEx(file_, &slot->ov);
        DWORD ignored = 0;
        ::GetOverlappedResult(file_, &slot->ov, &ignored, /*bWait=*/TRUE);
      }
    }
  }

  OverlappedBatch(const OverlappedBatch &) = delete;
  OverlappedBatch &operator=(const OverlappedBatch &) = delete;

  // Issues one overlapped transfer of `buffer.size()` bytes at `offset`
  // without waiting for it and returns the index of its slot.
  core::Win32Result<size_t> Submit(uint64_t offset, std::vector<uint8_t> buffer,
                                   bool is_write) {
    core::Win32Result<size_t> res{};
    auto slot = std::make_unique<Slot>();
    ::SetLastError(ERROR_SUCCESS);
    slot->event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!slot->event.valid()) {
      const DWORD err = ::GetLastError();
      res.error = (err != ERROR_SUCCESS) ? err : ERROR_GEN_FAILURE;
      return res;
    }
    slot->buffer = std::move(buffer);
    slot->ov = MakeOffsetOverlapped(offset, slot->event.get());
    const DWORD len = static_cast<DWORD>(slot->buffer.size());
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok =
        is_write
            ? ::WriteFile(file_, slot->buffer.data(), len, nullptr, &slot->ov)
            : ::ReadFile(file_, slot->buffer.data(), len, nullptr, &slot->ov);
    if (ok == FALSE) {
      const DWORD err = ::GetLastError();
      if (err != static_cast<DWORD>(ERROR_IO_PENDING)) {
        res.error = (err != ERROR_SUCCESS) ? err : ERROR_GEN_FAILURE;
        return res;
      }
    }
    // Whether it completed inline (TRUE) or is pending, the result is
    // collected later through the slot's event and GetOverlappedResult.
    slot->in_flight = true;
    slots_.push_back(std::move(slot));
    res.error = ERROR_SUCCESS;
    res.value = slots_.size() - 1u;
    return res;
  }

  // Waits up to `timeout_ms` for slot `index` and returns the bytes
  // transferred. On timeout the slot stays in flight for the destructor.
  core::Win32Result<DWORD> Wait(size_t index, DWORD timeout_ms) {
    core::Win32Result<DWORD> res{};
    if (index >= slots_.size() || !slots_[index]->in_flight) {
      res.error = ERROR_INVALID_PARAMETER;
      return res;
    }
    Slot &slot = *slots_[index];
    DWORD transferred = 0;
    ::SetLastError(ERROR_SUCCESS);
    BOOL ok =
        ::GetOverlappedResult(file_, &slot.ov, &transferred, /*bWait=*/FALSE);
    DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
    if (ok == FALSE && err == static_cast<DWORD>(ERROR_IO_INCOMPLETE)) {
      // Still pending: wait on the request's own event, then collect.
      ::SetLastError(ERROR_SUCCESS);
      const DWORD wait_rc = ::WaitForSingleObject(slot.event.get(), timeout_ms);
      if (wait_rc != static_cast<DWORD>(WAIT_OBJECT_0)) {
        const DWORD wait_err = (wait_rc == static_cast<DWORD>(WAIT_TIMEOUT))
                                   ? static_cast<DWORD>(ERROR_TIMEOUT)
                                   : ::GetLastError();
        res.error = (wait_err != ERROR_SUCCESS) ? wait_err : ERROR_GEN_FAILURE;
        return res;
      }
      ::SetLastError(ERROR_SUCCESS);
      ok = ::GetOverlappedResult(file_, &slot.ov, &transferred,
                                 /*bWait=*/FALSE);
      err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
    }
    if (ok == FALSE) {
      res.error = (err != ERROR_SUCCESS) ? err : ERROR_GEN_FAILURE;
      if (res.error != static_cast<DWORD>(ERROR_IO_INCOMPLETE)) {
        slot.in_flight = false;
      }
      return res;
    }
    slot.in_flight = false;
    res.error = ERROR_SUCCESS;
    res.value = transferred;
    return res;
  }

  // Buffer of slot `index`; only meaningful once Wait(index) has succeeded.
  const std::vector<uint8_t> &Buffer(size_t index) const {
    return slots_[index]->buffer;
  }

private:
  struct Slot {
    OVERLAPPED ov{};
    core::UniqueHandle event;
    std::vector<uint8_t> buffer;
    bool in_flight = false;
  };

  HANDLE file_ = nullptr;
  std::vector<std::unique_ptr<Slot>> slots_;
};

bool SeekTo(HANDLE h, int64_t offset, DWORD method,
            int64_t *new_pos_out = nullptr) noexcept {
  LARGE_INTEGER dist{};
  dist.QuadPart = offset;
  LARGE_INTEGER pos{};
  if (::SetFilePointerEx(h, dist, &pos, method) == FALSE) {
    return false;
  }
  if (new_pos_out != nullptr) {
    *new_pos_out = pos.QuadPart;
  }
  return true;
}

::testing::AssertionResult CheckStandardInfoEof(HANDLE h, int64_t expected_eof,
                                                int64_t min_alloc = -1) {
  auto std_info = core::QueryStandardInfo(h);
  if (!std_info) {
    return ::testing::AssertionFailure()
           << "QueryStandardInfo failed: "
           << core::FormatNtStatus(std_info.status);
  }
  if (std_info.value->end_of_file.QuadPart != expected_eof) {
    return ::testing::AssertionFailure()
           << "Expected EndOfFile=" << expected_eof << ", got "
           << std_info.value->end_of_file.QuadPart;
  }
  if (min_alloc >= 0 && std_info.value->allocation_size.QuadPart < min_alloc) {
    return ::testing::AssertionFailure()
           << "Expected AllocationSize >= " << min_alloc << ", got "
           << std_info.value->allocation_size.QuadPart;
  }
  return ::testing::AssertionSuccess();
}

NTSTATUS SetValidDataLengthNt(HANDLE h, LONGLONG vdl) noexcept {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtSetInformationFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  nt::FileValidDataLengthInfo vdl_info{};
  vdl_info.valid_data_length.QuadPart = vdl;
  nt::IoStatusBlock iosb{};
  return ntdll.NtSetInformationFile(
      h, &iosb, &vdl_info, sizeof(vdl_info),
      nt::FileInfoClass::FileValidDataLengthInformation);
}

// NtReadFile / NtWriteFile at `byte_offset` with a stack IO_STATUS_BLOCK. Only
// valid on synchronous handles (FILE_SYNCHRONOUS_IO_*), which never return
// STATUS_PENDING. Should a handle pend anyway, the kernel would complete into
// `iosb` after this frame is gone, so wait for the request (the file object is
// signaled on completion), record an explicit failure, and return
// STATUS_PENDING, which no caller accepts as an expected status.
NTSTATUS NtTransferAt(HANDLE h, LONGLONG byte_offset, void *buf, ULONG len,
                      bool is_write, ULONG_PTR *info_out = nullptr) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtWriteFile == nullptr || ntdll.NtReadFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  nt::IoStatusBlock iosb{};
  LARGE_INTEGER offset{};
  offset.QuadPart = byte_offset;
  const NTSTATUS st = is_write
                          ? ntdll.NtWriteFile(h, nullptr, nullptr, nullptr,
                                              &iosb, buf, len, &offset, nullptr)
                          : ntdll.NtReadFile(h, nullptr, nullptr, nullptr,
                                             &iosb, buf, len, &offset, nullptr);
  if (st == STATUS_PENDING) {
    ::WaitForSingleObject(h, INFINITE);
    ADD_FAILURE() << (is_write ? "NtWriteFile" : "NtReadFile")
                  << " returned STATUS_PENDING: NtTransferAt requires a "
                     "synchronous handle (final status "
                  << core::FormatNtStatus(iosb.status) << ")";
    return STATUS_PENDING;
  }
  if (info_out != nullptr) {
    *info_out = iosb.information;
  }
  return st;
}

struct CopyProgressState {
  int calls = 0;
  LONGLONG last_total = -1;
  LONGLONG last_transferred = -1;
};

DWORD CALLBACK CopyProgressCallback(
    LARGE_INTEGER total_file_size, LARGE_INTEGER total_bytes_transferred,
    LARGE_INTEGER /*stream_size*/, LARGE_INTEGER /*stream_bytes_transferred*/,
    DWORD /*stream_number*/, DWORD /*callback_reason*/, HANDLE /*source_file*/,
    HANDLE /*destination_file*/, LPVOID data) {
  auto *state = static_cast<CopyProgressState *>(data);
  if (state != nullptr) {
    ++state->calls;
    state->last_total = total_file_size.QuadPart;
    state->last_transferred = total_bytes_transferred.QuadPart;
  }
  return PROGRESS_CONTINUE;
}

// Heap-allocated (std::shared_ptr) and self-contained: the worker dereferences
// nothing outside this object, so a worker that outlives a timed-out Join()
// can never touch the test's stack frame. `ok` / `error` may only be read
// after the worker has been joined.
struct StressWorkerContext {
  int tid = 0;
  int iterations = 0;
  std::wstring stress_dir;
  bool ok = true;
  std::string error;

  static DWORD WINAPI Run(LPVOID param) {
    auto *ctx = static_cast<StressWorkerContext *>(param);
    const int t = ctx->tid;
    const std::wstring &dir = ctx->stress_dir;

    auto fail = [&](const std::string &msg) {
      ctx->ok = false;
      ctx->error = msg;
    };

    for (int iter = 0; iter < ctx->iterations; ++iter) {
      const std::wstring filename =
          L"w" + std::to_wstring(t) + L"_i" + std::to_wstring(iter) + L".dat";
      const std::wstring fpath = core::JoinPath(dir, filename);

      const size_t payload_len =
          512u + static_cast<size_t>(((t * 31) + (iter * 17)) % 4096);
      std::vector<uint8_t> expected(payload_len);
      for (size_t i = 0; i < payload_len; ++i) {
        expected[i] = PatternByte(static_cast<size_t>(t * 997 + iter * 53) + i);
      }

      {
        auto h = OpenRwFile(fpath, CREATE_NEW);
        if (!h) {
          fail("CreateFileW failed on " + core::WideToUtf8(filename) + ": " +
               core::FormatWin32Error(h.error));
          return 0;
        }
        auto wr = core::WriteAllBytes(h.value->get(), expected.data(),
                                      expected.size());
        if (!wr) {
          fail("WriteAllBytes failed on " + core::WideToUtf8(filename) + ": " +
               core::FormatWin32Error(wr.error));
          return 0;
        }
        if (!SeekBegin(h.value->get(), 0)) {
          const DWORD seek_err = ::GetLastError();
          fail("SetFilePointerEx failed on " + core::WideToUtf8(filename) +
               ": " + core::FormatWin32Error(seek_err));
          return 0;
        }
        auto rd = core::ReadAllBytes(h.value->get());
        if (!rd) {
          fail("ReadAllBytes failed on " + core::WideToUtf8(filename) + ": " +
               core::FormatWin32Error(rd.error));
          return 0;
        }
        if (rd.value->size() != expected.size() ||
            std::memcmp(rd.value->data(), expected.data(), expected.size()) !=
                0) {
          fail("Data integrity mismatch on " + core::WideToUtf8(filename));
          return 0;
        }
      }

      // Enumerate the shared directory while peers create and delete their
      // own files. This worker's file is closed but not yet deleted, so it
      // must be listed exactly once, and no name may ever be returned twice.
      auto list = core::ListDirectoryWin32(dir);
      if (!list) {
        fail("ListDirectoryWin32 failed at iter " + std::to_string(iter) +
             ": " + core::FormatWin32Error(list.error));
        return 0;
      }
      std::vector<std::wstring> entries = *list.value;
      std::sort(entries.begin(), entries.end());
      const bool has_dot = std::binary_search(entries.begin(), entries.end(),
                                              std::wstring(L"."));
      const bool has_dotdot = std::binary_search(entries.begin(), entries.end(),
                                                 std::wstring(L".."));
      if (!has_dot || !has_dotdot) {
        fail("Directory listing missing . or .. at iter " +
             std::to_string(iter));
        return 0;
      }
      const auto duplicate = std::adjacent_find(entries.begin(), entries.end());
      if (duplicate != entries.end()) {
        fail("Directory listing returned '" + core::WideToUtf8(*duplicate) +
             "' more than once at iter " + std::to_string(iter));
        return 0;
      }
      if (!std::binary_search(entries.begin(), entries.end(), filename)) {
        fail("Directory listing is missing this worker's own file " +
             core::WideToUtf8(filename) + " at iter " + std::to_string(iter));
        return 0;
      }

      if (::DeleteFileW(fpath.c_str()) == FALSE) {
        const DWORD delete_err = ::GetLastError();
        fail("DeleteFileW failed on " + core::WideToUtf8(filename) + ": " +
             core::FormatWin32Error(delete_err));
        return 0;
      }
    }
    return 0;
  }
};

// Buffers and handle shared by the FILE_FLAG_NO_BUFFERING tests. Members are
// declared so the handle closes before the buffers are freed.
struct NoBufferingFile {
  uint32_t sector = 0;
  uint32_t total_size = 0; // Two sectors.
  AlignedBuffer write_buf; // `total_size` bytes of PatternByte, sector-aligned.
  AlignedBuffer read_buf;  // `total_size` zero bytes, sector-aligned.
  core::UniqueHandle handle; // Opened with FILE_FLAG_NO_BUFFERING.

  std::string_view Pattern() const noexcept {
    return std::string_view(reinterpret_cast<const char *>(write_buf.data()),
                            total_size);
  }
};

::testing::AssertionResult InitNoBufferingBuffers(uint32_t sector,
                                                  NoBufferingFile *out) {
  if (sector < 512u || (sector & (sector - 1u)) != 0u) {
    return ::testing::AssertionFailure() << "Effective sector size " << sector
                                         << " is not a power of two >= 512";
  }
  out->sector = sector;
  out->total_size = sector * 2u;
  out->write_buf = AlignedBuffer(out->total_size, sector);
  out->read_buf = AlignedBuffer(out->total_size, sector);
  if (!out->write_buf || !out->read_buf) {
    return ::testing::AssertionFailure()
           << "_aligned_malloc(" << out->total_size << ", " << sector
           << ") failed";
  }
  for (size_t i = 0; i < out->total_size; ++i) {
    out->write_buf.data()[i] = PatternByte(i);
  }
  std::memset(out->read_buf.data(), 0, out->total_size);
  return ::testing::AssertionSuccess();
}

// Creates `path` holding two sectors of PatternByte data through an ordinary
// buffered handle, then opens it with FILE_FLAG_NO_BUFFERING. The rejection
// tests use this so they never depend on the unbuffered write path, which is a
// contract of its own (NoBufferingAlignedRoundTrip).
::testing::AssertionResult OpenPatternedNoBufferingFile(std::wstring_view path,
                                                        uint32_t sector,
                                                        NoBufferingFile *out) {
  if (auto init = InitNoBufferingBuffers(sector, out); !init) {
    return init;
  }
  auto seeded = core::WriteFileContents(path, out->Pattern());
  if (!seeded) {
    return ::testing::AssertionFailure()
           << "Failed to seed '" << core::WideToUtf8(path)
           << "': " << core::FormatWin32Error(seeded.error);
  }
  auto h = OpenRwFile(path, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING);
  if (!h) {
    return ::testing::AssertionFailure()
           << "Failed to open '" << core::WideToUtf8(path)
           << "' with FILE_FLAG_NO_BUFFERING: "
           << core::FormatWin32Error(h.error);
  }
  out->handle = std::move(*h.value);
  return ::testing::AssertionSuccess();
}

} // namespace

// Spec: [MS-FSA] 2.1.5.15.5 FileEndOfFileInformation -- extending EOF zero-fills
// newly exposed bytes and shrinking truncates file size persistently across close.
TEST_F(DataIoTest, SetEndOfFileExtendAndShrink) {
  const std::wstring path = TestPath(L"eof_extend_shrink.dat");

  auto h = OpenRwFile(path, CREATE_ALWAYS);
  ASSERT_WIN32_OK(h);

  auto info = core::GetByHandleInfo(h.value->get());
  ASSERT_WIN32_OK(info);
  EXPECT_EQ(core::FileSizeOf(*info.value), 0u);

  FILE_END_OF_FILE_INFO eof_info{};
  eof_info.EndOfFile.QuadPart = 42;
  ASSERT_WIN32_TRUE(::SetFileInformationByHandle(
      h.value->get(), FileEndOfFileInfo, &eof_info, sizeof(eof_info)));

  info = core::GetByHandleInfo(h.value->get());
  ASSERT_WIN32_OK(info);
  EXPECT_EQ(core::FileSizeOf(*info.value), 42u);

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  auto data = core::ReadAllBytes(h.value->get());
  ASSERT_WIN32_OK(data);
  ASSERT_EQ(data.value->size(), 42u);
  EXPECT_TRUE(helpers::AllBytesEqual(*data.value, '\0'));

  eof_info.EndOfFile.QuadPart = 13;
  ASSERT_WIN32_TRUE(::SetFileInformationByHandle(
      h.value->get(), FileEndOfFileInfo, &eof_info, sizeof(eof_info)));

  info = core::GetByHandleInfo(h.value->get());
  ASSERT_WIN32_OK(info);
  EXPECT_EQ(core::FileSizeOf(*info.value), 13u);

  h.value->reset();

  EXPECT_TRUE(helpers::CheckFileContents(path, std::string(13u, '\0')));
  ASSERT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 -- sequential ReadFile/WriteFile,
// SetFilePointerEx origins, short read at EOF, and 0-byte reads at/past EOF.
TEST_F(DataIoTest, ReadWriteBasicAndSeekOffsets) {
  const std::wstring path = TestPath(L"rw_seek.dat");

  auto h = OpenRwFile(path, CREATE_ALWAYS);
  ASSERT_WIN32_OK(h);

  const std::string part1 = "Hello, ";
  const std::string part2 = "NTFS world!";
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h.value->get(), part1.data(), part1.size()));
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h.value->get(), part2.data(), part2.size()));

  int64_t pos = -1;
  ASSERT_WIN32_TRUE(SeekTo(h.value->get(), 0, FILE_CURRENT, &pos));
  EXPECT_EQ(pos, 18);

  ASSERT_WIN32_TRUE(SeekTo(h.value->get(), 0, FILE_BEGIN, &pos));
  EXPECT_EQ(pos, 0);

  char buf[64]{};
  DWORD bytes_read = 0;
  ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), buf, 5, &bytes_read, nullptr));
  EXPECT_EQ(bytes_read, 5u);
  EXPECT_EQ(std::string(buf, bytes_read), "Hello");

  ASSERT_WIN32_TRUE(SeekTo(h.value->get(), 2, FILE_CURRENT, &pos));
  EXPECT_EQ(pos, 7);
  ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), buf, 4, &bytes_read, nullptr));
  EXPECT_EQ(bytes_read, 4u);
  EXPECT_EQ(std::string(buf, bytes_read), "NTFS");

  // Straddling EOF returns exact remaining bytes + TRUE.
  ASSERT_WIN32_TRUE(SeekTo(h.value->get(), -6, FILE_END, &pos));
  EXPECT_EQ(pos, 12);
  ASSERT_WIN32_TRUE(
      ::ReadFile(h.value->get(), buf, sizeof(buf), &bytes_read, nullptr));
  EXPECT_EQ(bytes_read, 6u);
  EXPECT_EQ(std::string(buf, bytes_read), "world!");

  // Reading at EOF returns 0 bytes + TRUE.
  bytes_read = 99;
  ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), buf, 16, &bytes_read, nullptr));
  EXPECT_EQ(bytes_read, 0u);

  // Reading past EOF returns 0 bytes + TRUE and does not extend file size.
  ASSERT_WIN32_TRUE(SeekTo(h.value->get(), 100, FILE_BEGIN, &pos));
  EXPECT_EQ(pos, 100);
  bytes_read = 99;
  ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), buf, 16, &bytes_read, nullptr));
  EXPECT_EQ(bytes_read, 0u);

  LARGE_INTEGER size{};
  ASSERT_WIN32_TRUE(::GetFileSizeEx(h.value->get(), &size));
  EXPECT_EQ(size.QuadPart, 18);
}

// Spec: [MS-FSA] 2.1.5.4 Write -- writing past EOF extends EndOfFile and
// zero-fills every unwritten byte in the intervening gap.
TEST_F(DataIoTest, WriteBeyondEofZeroFillsGap) {
  const std::wstring path = TestPath(L"write_gap.dat");

  auto h = OpenRwFile(path, CREATE_ALWAYS);
  ASSERT_WIN32_OK(h);

  constexpr int64_t kGapOffset = 65536;
  const std::string tail = "GAP_END_PAYLOAD!";
  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), kGapOffset));
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h.value->get(), tail.data(), tail.size()));

  EXPECT_TRUE(CheckStandardInfoEof(
      h.value->get(), kGapOffset + static_cast<int64_t>(tail.size())));

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  auto contents = core::ReadAllBytes(h.value->get());
  ASSERT_WIN32_OK(contents);
  ASSERT_EQ(contents.value->size(),
            static_cast<size_t>(kGapOffset) + tail.size());
  EXPECT_TRUE(helpers::AllBytesEqual(
      std::string_view(contents.value->data(), static_cast<size_t>(kGapOffset)),
      '\0'));
  EXPECT_EQ(contents.value->substr(static_cast<size_t>(kGapOffset)), tail);
}

// Spec: [MS-FSA] 2.1.5.4 Write -- FILE_APPEND_DATA-only handles always append
// at current EOF, ignoring both file pointer position and OVERLAPPED.Offset.
TEST_F(DataIoTest, AppendOnlyHandleWritesAtCurrentEof) {
  const std::wstring path = TestPath(L"append_only.dat");

  auto init = core::WriteFileContents(path, "AAAA");
  ASSERT_WIN32_OK(init);

  // Synchronous append-only handle: seek to 0 must be ignored on WriteFile.
  {
    auto h = core::CreateWin32File(path, FILE_APPEND_DATA | SYNCHRONIZE,
                                   core::kShareAll, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
    const std::string chunk = "BBBB";
    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), chunk.data(), chunk.size()));
  }

  // Overlapped append-only handle: explicit OVERLAPPED.Offset = 0 must also
  // append at EOF.
  {
    auto h = core::CreateWin32File(
        path, FILE_APPEND_DATA | SYNCHRONIZE, core::kShareAll, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
    ASSERT_WIN32_OK(h);

    const std::string chunk = "CCCC";
    auto wr = OverlappedWriteAt(h.value->get(), 0, chunk.data(),
                                static_cast<DWORD>(chunk.size()),
                                Config().timing.async_timeout_ms);
    ASSERT_WIN32_OK(wr);
    EXPECT_EQ(*wr.value, static_cast<DWORD>(chunk.size()));
  }

  EXPECT_TRUE(helpers::CheckFileContents(path, "AAAABBBBCCCC"));
}

// Spec: Win32 FILE_FLAG_OVERLAPPED / [MS-FSA] 2.1.5.3-4 -- overlapped writes
// and reads issued one at a time (each completed before the next starts) at
// non-monotonic explicit offsets land where OVERLAPPED.Offset says, stay
// coherent across two handles, and leave zero-filled gaps. Requests that are
// in flight simultaneously are covered by
// OverlappedConcurrentWritesAndReadsOnOneHandle.
TEST_F(DataIoTest, OverlappedAsyncReadWriteExplicitOffsets) {
  const std::wstring path = TestPath(L"overlapped_io.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  auto h1 = OpenRwFile(path, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
  ASSERT_WIN32_OK(h1);

  struct ChunkSpec {
    uint64_t offset;
    std::string_view data;
  };
  constexpr std::array<ChunkSpec, 3> kH1Writes = {{
      {16384u, "SECOND_CHUNK_16!"},
      {0u, "FIRST_CHUNK_16B!"},
      {8192u, "THIRD_CHUNK_16B!"},
  }};
  for (const auto &chunk : kH1Writes) {
    auto wr =
        OverlappedWriteAt(h1.value->get(), chunk.offset, chunk.data.data(),
                          static_cast<DWORD>(chunk.data.size()), timeout_ms);
    ASSERT_WIN32_OK(wr);
    EXPECT_EQ(*wr.value, static_cast<DWORD>(chunk.data.size()));
  }

  // Second overlapped handle writing another offset must not disturb H1.
  auto h2 = OpenRwFile(path, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
  ASSERT_WIN32_OK(h2);
  constexpr ChunkSpec kH2Write = {24576u, "HANDLE2_CHUNK16!"};
  auto wr3 =
      OverlappedWriteAt(h2.value->get(), kH2Write.offset, kH2Write.data.data(),
                        static_cast<DWORD>(kH2Write.data.size()), timeout_ms);
  ASSERT_WIN32_OK(wr3);
  EXPECT_EQ(*wr3.value, static_cast<DWORD>(kH2Write.data.size()));

  // Read back out of order from H1 and verify zero-filled gap too.
  constexpr std::array<ChunkSpec, 4> kReadOrder = {{
      kH1Writes[2],
      kH1Writes[1],
      kH1Writes[0],
      kH2Write,
  }};
  char buf[16]{};
  for (const auto &expected : kReadOrder) {
    auto rd =
        OverlappedReadAt(h1.value->get(), expected.offset, buf,
                         static_cast<DWORD>(expected.data.size()), timeout_ms);
    ASSERT_WIN32_OK(rd);
    EXPECT_EQ(*rd.value, static_cast<DWORD>(expected.data.size()));
    EXPECT_EQ(std::string_view(buf, *rd.value), expected.data);
  }

  std::vector<uint8_t> gap(4096, 0xFF);
  auto rd_gap = OverlappedReadAt(h1.value->get(), 16, gap.data(),
                                 static_cast<DWORD>(gap.size()), timeout_ms);
  ASSERT_WIN32_OK(rd_gap);
  EXPECT_EQ(*rd_gap.value, static_cast<DWORD>(gap.size()));
  EXPECT_TRUE(helpers::AllBytesEqual(gap, 0x00));
}

// Spec: Win32 FILE_FLAG_OVERLAPPED / [MS-FSA] 2.1.5.3-4 -- several overlapped
// requests issued on one handle before any of them is waited on each transfer
// their full length at their own OVERLAPPED.Offset, independent of submission
// and completion order.
TEST_F(DataIoTest, OverlappedConcurrentWritesAndReadsOnOneHandle) {
  const std::wstring path = TestPath(L"overlapped_concurrent.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  constexpr size_t kRequests = 8;
  constexpr size_t kChunk = 65536;
  // Chunk indexes in submission order (deliberately neither ascending nor
  // descending file offsets).
  constexpr std::array<size_t, kRequests> kWriteOrder = {5, 0, 7, 2,
                                                         6, 1, 4, 3};
  constexpr std::array<size_t, kRequests> kReadOrder = {3, 6, 0, 5, 1, 7, 2, 4};

  auto h = OpenRwFile(path, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
  ASSERT_WIN32_OK(h);

  {
    OverlappedBatch writes(h.value->get());
    std::array<size_t, kRequests> slot_of{};
    for (const size_t chunk_index : kWriteOrder) {
      const size_t file_offset = chunk_index * kChunk;
      std::vector<uint8_t> chunk(kChunk);
      for (size_t i = 0; i < kChunk; ++i) {
        chunk[i] = PatternByte(file_offset + i);
      }
      auto submitted = writes.Submit(static_cast<uint64_t>(file_offset),
                                     std::move(chunk), /*is_write=*/true);
      ASSERT_WIN32_OK(submitted) << "WriteFile for chunk " << chunk_index;
      slot_of[chunk_index] = *submitted.value;
    }
    for (size_t chunk_index = 0; chunk_index < kRequests; ++chunk_index) {
      auto done = writes.Wait(slot_of[chunk_index], timeout_ms);
      ASSERT_WIN32_OK(done) << "Write completion for chunk " << chunk_index;
      EXPECT_EQ(*done.value, static_cast<DWORD>(kChunk))
          << "chunk " << chunk_index;
    }
  }

  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(),
                                   static_cast<int64_t>(kRequests * kChunk)));

  {
    OverlappedBatch reads(h.value->get());
    std::array<size_t, kRequests> slot_of{};
    for (const size_t chunk_index : kReadOrder) {
      auto submitted =
          reads.Submit(static_cast<uint64_t>(chunk_index * kChunk),
                       std::vector<uint8_t>(kChunk, 0xEE), /*is_write=*/false);
      ASSERT_WIN32_OK(submitted) << "ReadFile for chunk " << chunk_index;
      slot_of[chunk_index] = *submitted.value;
    }
    for (size_t chunk_index = 0; chunk_index < kRequests; ++chunk_index) {
      auto done = reads.Wait(slot_of[chunk_index], timeout_ms);
      ASSERT_WIN32_OK(done) << "Read completion for chunk " << chunk_index;
      ASSERT_EQ(*done.value, static_cast<DWORD>(kChunk))
          << "chunk " << chunk_index;
      const std::vector<uint8_t> &data = reads.Buffer(slot_of[chunk_index]);
      const size_t file_offset = chunk_index * kChunk;
      size_t first_bad = kChunk;
      for (size_t i = 0; i < kChunk; ++i) {
        if (data[i] != PatternByte(file_offset + i)) {
          first_bad = i;
          break;
        }
      }
      EXPECT_EQ(first_bad, kChunk)
          << "chunk " << chunk_index << " differs from the written pattern at "
          << "file offset " << (file_offset + first_bad);
    }
  }
}

// Spec: Win32 IOCP / CancelIoEx -- overlapped WriteFile/ReadFile on an IOCP
// handle post completion packets, CancelIoEx on an idle handle returns
// ERROR_NOT_FOUND, and CancelIoEx cancels a real pending IRP with
// ERROR_OPERATION_ABORTED.
TEST_F(DataIoTest, OverlappedAsyncReadWriteIocpAndCancelIoEx) {
  const std::wstring path = TestPath(L"overlapped_iocp.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  constexpr ULONG_PTR kCompletionKey = 0x1A2B3C4Du;

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle iocp(
      ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0));
  const DWORD iocp_err = ::GetLastError();
  ASSERT_TRUE(iocp.valid()) << core::FormatWin32Error(iocp_err);

  auto h = OpenRwFile(path, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
  ASSERT_WIN32_OK(h);

  ::SetLastError(ERROR_SUCCESS);
  HANDLE attached =
      ::CreateIoCompletionPort(h.value->get(), iocp.get(), kCompletionKey, 0);
  const DWORD attach_err = ::GetLastError();
  ASSERT_EQ(attached, iocp.get()) << core::FormatWin32Error(attach_err);

  std::string high_payload = "IOCP_HIGH_CHUNK!";
  std::string base_payload = "IOCP_BASE_CHUNK!";

  ASSERT_TRUE(IocpTransferAt(
      h.value->get(), iocp.get(), kCompletionKey, 8192, high_payload.data(),
      static_cast<DWORD>(high_payload.size()), /*is_write=*/true, timeout_ms));
  ASSERT_TRUE(IocpTransferAt(
      h.value->get(), iocp.get(), kCompletionKey, 0, base_payload.data(),
      static_cast<DWORD>(base_payload.size()), /*is_write=*/true, timeout_ms));

  char read_buf[16]{};
  ASSERT_TRUE(IocpTransferAt(h.value->get(), iocp.get(), kCompletionKey, 8192,
                             read_buf, sizeof(read_buf), /*is_write=*/false,
                             timeout_ms));
  EXPECT_EQ(std::string(read_buf, 16), high_payload);

  std::memset(read_buf, 0, sizeof(read_buf));
  ASSERT_TRUE(IocpTransferAt(h.value->get(), iocp.get(), kCompletionKey, 0,
                             read_buf, sizeof(read_buf), /*is_write=*/false,
                             timeout_ms));
  EXPECT_EQ(std::string(read_buf, 16), base_payload);

  std::array<uint8_t, 64> gap_buf{};
  gap_buf.fill(0xAA);
  ASSERT_TRUE(IocpTransferAt(h.value->get(), iocp.get(), kCompletionKey, 4096,
                             gap_buf.data(), static_cast<DWORD>(gap_buf.size()),
                             /*is_write=*/false, timeout_ms));
  EXPECT_TRUE(helpers::AllBytesEqual(gap_buf.data(), gap_buf.size(), 0x00));

  // CancelIoEx on an idle handle with zero pending IRPs returns ERROR_NOT_FOUND.
  EXPECT_WIN32_ERROR(::CancelIoEx(h.value->get(), nullptr), ERROR_NOT_FOUND);
  OVERLAPPED idle_ov = MakeOffsetOverlapped(0);
  EXPECT_WIN32_ERROR(::CancelIoEx(h.value->get(), &idle_ov), ERROR_NOT_FOUND);

  // CancelIoEx on a real pending overlapped LockFileEx IRP aborts it and
  // delivers ERROR_OPERATION_ABORTED to the bound IOCP.
  {
    auto h_blocker = OpenRwFile(path, OPEN_EXISTING);
    ASSERT_WIN32_OK(h_blocker);
    OVERLAPPED blk_ov = MakeOffsetOverlapped(0);
    ASSERT_WIN32_TRUE(
        ::LockFileEx(h_blocker.value->get(),
                     LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                     /*dwReserved=*/0, 64, 0, &blk_ov));
    bool blocker_locked = true;

    // Declared before `lock_guard` so `~PendingLockGuard` cancels, releases
    // `h_blocker` if needed, and blocks in `GetOverlappedResult(..., TRUE)`
    // before `pending_lock_ov` or `blk_ov` can leave scope on any exit path.
    OVERLAPPED pending_lock_ov = MakeOffsetOverlapped(0);
    struct PendingLockGuard {
      HANDLE file = INVALID_HANDLE_VALUE;
      HANDLE blocker = INVALID_HANDLE_VALUE;
      OVERLAPPED *blocker_ov = nullptr;
      bool *blocker_locked_flag = nullptr;
      OVERLAPPED *ov = nullptr;
      bool armed = false;

      void Drain() noexcept {
        if (!armed) {
          return;
        }
        armed = false;
        ::CancelIoEx(file, ov);
        if (blocker_locked_flag != nullptr && *blocker_locked_flag) {
          *blocker_locked_flag = false;
          ::UnlockFileEx(blocker, /*dwReserved=*/0, 64, 0, blocker_ov);
        }
        DWORD ignored = 0;
        ::GetOverlappedResult(file, ov, &ignored, /*bWait=*/TRUE);
      }

      ~PendingLockGuard() { Drain(); }
    } lock_guard{h.value->get(),  h_blocker.value->get(), &blk_ov,
                 &blocker_locked, &pending_lock_ov,       /*armed=*/false};

    ::SetLastError(ERROR_SUCCESS);
    const BOOL lock_ok =
        ::LockFileEx(h.value->get(), LOCKFILE_EXCLUSIVE_LOCK, /*dwReserved=*/0,
                     64, 0, &pending_lock_ov);
    const DWORD lock_err =
        (lock_ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
    if (lock_ok != FALSE || lock_err == ERROR_IO_PENDING) {
      lock_guard.armed = true;
    }
    ASSERT_EQ(lock_ok, FALSE);
    ASSERT_WIN32_ERROR_CODE(lock_err, ERROR_IO_PENDING);

    ASSERT_WIN32_TRUE(::CancelIoEx(h.value->get(), &pending_lock_ov));

    DWORD cancel_bytes = 0;
    ULONG_PTR cancel_key = 0;
    LPOVERLAPPED cancel_done_ov = nullptr;
    ::SetLastError(ERROR_SUCCESS);
    const BOOL gqcs_ok = ::GetQueuedCompletionStatus(
        iocp.get(), &cancel_bytes, &cancel_key, &cancel_done_ov, timeout_ms);
    const DWORD gqcs_err = ::GetLastError();
    if (cancel_done_ov == &pending_lock_ov) {
      lock_guard.armed = false;
    } else {
      // Timeout or stale foreign packet: drain `pending_lock_ov` before stack
      // teardown.
      lock_guard.Drain();
    }
    EXPECT_EQ(gqcs_ok, FALSE);
    EXPECT_EQ(cancel_done_ov, &pending_lock_ov);
    EXPECT_EQ(cancel_key, kCompletionKey);
    EXPECT_WIN32_ERROR_CODE(gqcs_err, ERROR_OPERATION_ABORTED);

    if (blocker_locked) {
      blocker_locked = false;
      ASSERT_WIN32_TRUE(::UnlockFileEx(h_blocker.value->get(), /*dwReserved=*/0,
                                       64, 0, &blk_ov));
    }
  }
}

// Spec: [MS-FSA] 2.1.5.15.5 FileEndOfFileInformation -- shrinking EOF discards
// truncated tail data so re-extending returns zeroes rather than stale bytes.
TEST_F(DataIoTest, SetEndOfFileShrinkAndReExtendZeroFillsOldData) {
  const std::wstring path = TestPath(L"shrink_reextend.dat");

  auto h = OpenRwFile(path, CREATE_ALWAYS);
  ASSERT_WIN32_OK(h);

  constexpr size_t kInitialSize = 16384;
  constexpr size_t kShrinkSize = 4096;
  std::vector<uint8_t> pattern(kInitialSize, 0xA5);
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h.value->get(), pattern.data(), pattern.size()));

  ASSERT_WIN32_TRUE(
      SeekBegin(h.value->get(), static_cast<int64_t>(kShrinkSize)));
  ASSERT_WIN32_TRUE(::SetEndOfFile(h.value->get()));

  LARGE_INTEGER sz{};
  ASSERT_WIN32_TRUE(::GetFileSizeEx(h.value->get(), &sz));
  EXPECT_EQ(sz.QuadPart, static_cast<LONGLONG>(kShrinkSize));

  ASSERT_WIN32_TRUE(
      SeekBegin(h.value->get(), static_cast<int64_t>(kInitialSize)));
  ASSERT_WIN32_TRUE(::SetEndOfFile(h.value->get()));

  ASSERT_WIN32_TRUE(::GetFileSizeEx(h.value->get(), &sz));
  EXPECT_EQ(sz.QuadPart, static_cast<LONGLONG>(kInitialSize));

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  auto contents = core::ReadAllBytes(h.value->get());
  ASSERT_WIN32_OK(contents);
  ASSERT_EQ(contents.value->size(), kInitialSize);

  const auto *bytes = reinterpret_cast<const uint8_t *>(contents.value->data());
  EXPECT_TRUE(helpers::AllBytesEqual(bytes, kShrinkSize, 0xA5));
  EXPECT_TRUE(helpers::AllBytesEqual(bytes + kShrinkSize,
                                     kInitialSize - kShrinkSize, 0x00));
}

// Spec: [MS-FSA] 2.1.5.15.1 FileAllocationInformation -- preallocation raises
// AllocationSize without moving EOF, and shrinking allocation truncates EOF.
TEST_F(DataIoTest, FileAllocationInfoPreallocateAndTruncate) {
  const std::wstring path = TestPath(L"alloc_info.dat");

  auto h = OpenRwFile(path, CREATE_ALWAYS);
  ASSERT_WIN32_OK(h);

  // Preallocate 64 KiB (guaranteed non-resident) and query on the SAME open
  // handle before closing (NTFS truncates excess preallocation on cleanup).
  ASSERT_NT_SUCCESS(core::SetAllocationSizeNt(h.value->get(), 65536).status);
  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(), 0, 65536));

  // Write 32 KiB, then shrink allocation to 4 KiB -> must truncate EOF to 4096.
  std::vector<uint8_t> payload(32768, 0x5C);
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h.value->get(), payload.data(), payload.size()));
  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(), 32768, 32768));

  ASSERT_NT_SUCCESS(core::SetAllocationSizeNt(h.value->get(), 4096).status);
  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(), 4096));
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 -- writes and overwrites on one open handle
// are immediately visible to concurrent reader handles without closing.
TEST_F(DataIoTest, MultiHandleReadWriteCoherence) {
  const std::wstring path = TestPath(L"coherence.dat");

  auto init = core::WriteFileContents(path, "INITIAL_0000");
  ASSERT_WIN32_OK(init);

  auto h_write = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_write);

  auto h_read = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_read);

  auto read1 = core::ReadAllBytes(h_read.value->get());
  ASSERT_WIN32_OK(read1);
  EXPECT_EQ(*read1.value, "INITIAL_0000");

  const std::string updated = "UPDATED_DATA_EXTENDED!";
  ASSERT_WIN32_OK(core::WriteAllBytes(h_write.value->get(), updated.data(),
                                      updated.size()));

  LARGE_INTEGER sz{};
  ASSERT_WIN32_TRUE(::GetFileSizeEx(h_read.value->get(), &sz));
  EXPECT_EQ(sz.QuadPart, static_cast<LONGLONG>(updated.size()));

  ASSERT_WIN32_TRUE(SeekBegin(h_read.value->get(), 0));
  auto read2 = core::ReadAllBytes(h_read.value->get());
  ASSERT_WIN32_OK(read2);
  EXPECT_EQ(*read2.value, updated);

  // Mid-file overwrite on H1 immediately visible on H2.
  ASSERT_WIN32_TRUE(SeekBegin(h_write.value->get(), 8));
  const std::string patch = "ABCD";
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h_write.value->get(), patch.data(), patch.size()));

  ASSERT_WIN32_TRUE(SeekBegin(h_read.value->get(), 0));
  auto read3 = core::ReadAllBytes(h_read.value->get());
  ASSERT_WIN32_OK(read3);
  EXPECT_EQ(*read3.value, "UPDATED_ABCD_EXTENDED!");
}

// Spec: Win32 File Buffering / [MS-FSA] 2.1.5.3-4 -- on a
// FILE_FLAG_NO_BUFFERING handle, sector-aligned writes and reads (aligned
// length, offset, and buffer address) round-trip data byte for byte.
TEST_F(DataIoTest, NoBufferingAlignedRoundTrip) {
  const std::wstring path = TestPath(L"no_buffering_roundtrip.dat");
  NoBufferingFile nb;
  ASSERT_TRUE(InitNoBufferingBuffers(EffectiveSectorSize(), &nb));

  auto h = OpenRwFile(path, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING);
  ASSERT_WIN32_OK(h);
  nb.handle = std::move(*h.value);

  DWORD transferred = 0;
  ASSERT_WIN32_TRUE(::WriteFile(nb.handle.get(), nb.write_buf.data(),
                                nb.total_size, &transferred, nullptr));
  EXPECT_EQ(transferred, nb.total_size);

  ASSERT_WIN32_TRUE(SeekBegin(nb.handle.get(), 0));
  transferred = 0;
  ASSERT_WIN32_TRUE(::ReadFile(nb.handle.get(), nb.read_buf.data(),
                               nb.total_size, &transferred, nullptr));
  EXPECT_EQ(transferred, nb.total_size);
  EXPECT_EQ(std::memcmp(nb.write_buf.data(), nb.read_buf.data(), nb.total_size),
            0);

  // Aligned read of the second sector only.
  std::memset(nb.read_buf.data(), 0, nb.total_size);
  ASSERT_WIN32_TRUE(
      SeekBegin(nb.handle.get(), static_cast<int64_t>(nb.sector)));
  transferred = 0;
  ASSERT_WIN32_TRUE(::ReadFile(nb.handle.get(), nb.read_buf.data(), nb.sector,
                               &transferred, nullptr));
  EXPECT_EQ(transferred, nb.sector);
  EXPECT_EQ(std::memcmp(nb.write_buf.data() + nb.sector, nb.read_buf.data(),
                        nb.sector),
            0);
}

// Spec: Win32 File Buffering -- on a FILE_FLAG_NO_BUFFERING handle a transfer
// length that is not a multiple of the sector size fails
// ERROR_INVALID_PARAMETER for both WriteFile and ReadFile.
TEST_F(DataIoTest, NoBufferingUnalignedLengthRejected) {
  const std::wstring path = TestPath(L"no_buffering_bad_len.dat");
  NoBufferingFile nb;
  ASSERT_TRUE(OpenPatternedNoBufferingFile(path, EffectiveSectorSize(), &nb));

  DWORD transferred = 0;
  for (const DWORD bad_len : {DWORD{1}, DWORD{13}}) {
    SCOPED_TRACE("length=" + std::to_string(bad_len));
    EXPECT_WIN32_ERROR(::WriteFile(nb.handle.get(), nb.write_buf.data(),
                                   bad_len, &transferred, nullptr),
                       ERROR_INVALID_PARAMETER);
    EXPECT_WIN32_ERROR(::ReadFile(nb.handle.get(), nb.read_buf.data(), bad_len,
                                  &transferred, nullptr),
                       ERROR_INVALID_PARAMETER);
  }

  // The rejected writes must not have modified the file.
  nb.handle.reset();
  EXPECT_TRUE(helpers::CheckFileContents(path, nb.Pattern()));
}

// Spec: Win32 File Buffering / [MS-FSCC] 2.4.40 FilePositionInformation -- on
// a FILE_FLAG_NO_BUFFERING handle, moving the file pointer to an offset that
// is not a multiple of the sector size fails ERROR_INVALID_PARAMETER.
TEST_F(DataIoTest, NoBufferingUnalignedSeekOffsetRejected) {
  const std::wstring path = TestPath(L"no_buffering_bad_seek.dat");
  NoBufferingFile nb;
  ASSERT_TRUE(OpenPatternedNoBufferingFile(path, EffectiveSectorSize(), &nb));

  LARGE_INTEGER li{};
  li.QuadPart = 13;
  EXPECT_WIN32_ERROR(
      ::SetFilePointerEx(nb.handle.get(), li, nullptr, FILE_BEGIN),
      ERROR_INVALID_PARAMETER);
}

// Spec: Win32 File Buffering -- on a FILE_FLAG_NO_BUFFERING handle, WriteFile
// and ReadFile with an explicit OVERLAPPED offset that is not a multiple of
// the sector size fail ERROR_INVALID_PARAMETER (aligned length and buffer).
TEST_F(DataIoTest, NoBufferingUnalignedExplicitOffsetRejected) {
  const std::wstring path = TestPath(L"no_buffering_bad_offset.dat");
  NoBufferingFile nb;
  ASSERT_TRUE(OpenPatternedNoBufferingFile(path, EffectiveSectorSize(), &nb));

  DWORD transferred = 0;
  OVERLAPPED ov = MakeOffsetOverlapped(13);
  EXPECT_WIN32_ERROR(::WriteFile(nb.handle.get(), nb.write_buf.data(),
                                 nb.sector, &transferred, &ov),
                     ERROR_INVALID_PARAMETER);
  EXPECT_WIN32_ERROR(::ReadFile(nb.handle.get(), nb.read_buf.data(), nb.sector,
                                &transferred, &ov),
                     ERROR_INVALID_PARAMETER);

  // The rejected write must not have modified the file.
  nb.handle.reset();
  EXPECT_TRUE(helpers::CheckFileContents(path, nb.Pattern()));
}

// Spec: Win32 File Buffering / [MS-FSCC] 2.4.3 FileAlignmentInformation -- a
// FILE_FLAG_NO_BUFFERING transfer (aligned offset and length) from a user
// buffer whose address violates the device's AlignmentRequirement mask fails
// ERROR_INVALID_PARAMETER; when the address satisfies the mask the transfer
// succeeds. The mask is device-specific (FILE_BYTE_ALIGNMENT == 0 accepts
// every address), so the expectation is derived from the queried mask instead
// of assuming that an odd address is always rejected.
TEST_F(DataIoTest, NoBufferingBufferAddressHonorsAlignmentRequirement) {
  const std::wstring path = TestPath(L"no_buffering_buf_addr.dat");
  NoBufferingFile nb;
  ASSERT_TRUE(OpenPatternedNoBufferingFile(path, EffectiveSectorSize(), &nb));

  auto alignment = helpers::QueryAlignmentRequirement(nb.handle.get());
  ASSERT_NT_SUCCESS(alignment.status);
  ASSERT_TRUE(alignment.value.has_value());
  const ULONG mask = *alignment.value;
  SCOPED_TRACE("FileAlignmentInformation mask=" + std::to_string(mask));
  // AlignmentRequirement is always one less than a power of two.
  ASSERT_EQ(mask & (mask + 1u), 0u);

  // total_size == sector * 2 keeps [1, 1 + sector) strictly in bounds.
  uint8_t *const odd_write = nb.write_buf.data() + 1;
  uint8_t *const odd_read = nb.read_buf.data() + 1;
  const bool write_misaligned = (reinterpret_cast<uintptr_t>(odd_write) &
                                 static_cast<uintptr_t>(mask)) != 0u;
  const bool read_misaligned = (reinterpret_cast<uintptr_t>(odd_read) &
                                static_cast<uintptr_t>(mask)) != 0u;

  DWORD transferred = 0;
  ASSERT_WIN32_TRUE(SeekBegin(nb.handle.get(), 0));
  if (write_misaligned) {
    EXPECT_WIN32_ERROR(::WriteFile(nb.handle.get(), odd_write, nb.sector,
                                   &transferred, nullptr),
                       ERROR_INVALID_PARAMETER);
  } else {
    ASSERT_WIN32_TRUE(::WriteFile(nb.handle.get(), odd_write, nb.sector,
                                  &transferred, nullptr));
    EXPECT_EQ(transferred, nb.sector);
  }

  ASSERT_WIN32_TRUE(SeekBegin(nb.handle.get(), 0));
  if (read_misaligned) {
    EXPECT_WIN32_ERROR(
        ::ReadFile(nb.handle.get(), odd_read, nb.sector, &transferred, nullptr),
        ERROR_INVALID_PARAMETER);
  } else {
    transferred = 0;
    ASSERT_WIN32_TRUE(::ReadFile(nb.handle.get(), odd_read, nb.sector,
                                 &transferred, nullptr));
    EXPECT_EQ(transferred, nb.sector);
    // The first sector holds the seeded pattern unless the write above was
    // accepted, in which case it holds the bytes written from `odd_write`.
    const uint8_t *const expected =
        write_misaligned ? nb.write_buf.data() : odd_write;
    EXPECT_EQ(std::memcmp(odd_read, expected, nb.sector), 0);
  }
}

// Spec: Win32 File Buffering / [MS-FSA] 2.1.5.3 -- a sector-aligned
// FILE_FLAG_NO_BUFFERING read that crosses a non-sector-multiple EOF succeeds
// and returns only the bytes up to EOF.
TEST_F(DataIoTest, NoBufferingShortReadAtNonSectorMultipleEof) {
  const std::wstring path = TestPath(L"no_buffering_short_eof.dat");
  NoBufferingFile nb;
  ASSERT_TRUE(InitNoBufferingBuffers(EffectiveSectorSize(), &nb));

  // Write 100 bytes through a buffered handle, then issue a sector-aligned
  // NO_BUFFERING read of `sector` bytes -> returns 100 bytes.
  constexpr size_t kShortLen = 100u;
  std::string short_payload(kShortLen, '\0');
  for (size_t i = 0; i < kShortLen; ++i) {
    short_payload[i] = static_cast<char>(PatternByte(i + 7u));
  }
  auto wr_short = core::WriteFileContents(path, short_payload);
  ASSERT_WIN32_OK(wr_short);

  auto h_short =
      core::CreateWin32File(path, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING);
  ASSERT_WIN32_OK(h_short);
  nb.handle = std::move(*h_short.value);

  DWORD transferred = 0;
  ASSERT_WIN32_TRUE(::ReadFile(nb.handle.get(), nb.read_buf.data(), nb.sector,
                               &transferred, nullptr));
  EXPECT_EQ(transferred, static_cast<DWORD>(kShortLen));
  EXPECT_EQ(std::memcmp(nb.read_buf.data(), short_payload.data(), kShortLen),
            0);
}

// Spec: [MS-FSA] 2.1.5.7 Flush -- writes on a FILE_FLAG_WRITE_THROUGH handle
// followed by FlushFileBuffers succeed and the data reads back intact after
// the handle is closed, while FlushFileBuffers on a handle without write
// access fails ERROR_ACCESS_DENIED.
//
// This deliberately does not claim to verify durability: on a cached file
// system the read-back is served from the same cache the write populated (and
// even an unbuffered read is preceded by an implicit cache flush), so whether
// the bytes reached stable storage is not observable without a crash or
// remount. What is falsifiable here is the success/failure contract of the
// flush call and that write-through I/O does not lose or corrupt data.
TEST_F(DataIoTest, WriteThroughAndFlushFileBuffers) {
  const std::wstring path = TestPath(L"write_through.dat");
  const std::string payload = "PERSISTENT_WRITE_THROUGH_PAYLOAD_2026";

  {
    auto h = OpenRwFile(path, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH);
    ASSERT_WIN32_OK(h);

    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), payload.data(), payload.size()));
    ASSERT_WIN32_TRUE(::FlushFileBuffers(h.value->get()));
  }

  auto h_ro = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_ro);

  auto data = core::ReadAllBytes(h_ro.value->get());
  ASSERT_WIN32_OK(data);
  EXPECT_EQ(*data.value, payload);

  EXPECT_WIN32_ERROR(::FlushFileBuffers(h_ro.value->get()),
                     ERROR_ACCESS_DENIED);
}

// Spec: [MS-FSA] 2.1.5.7 / WDK NtFlushBuffersFileEx -- data-only and
// data-sync-only flush flags succeed on writable handles while unknown flags
// fail STATUS_INVALID_PARAMETER and read-only handles fail STATUS_ACCESS_DENIED.
TEST_F(DataIoTest, NtFlushBuffersFileExModes) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtFlushBuffersFileEx, nullptr);

  const std::wstring path = TestPath(L"flush_ex_modes.dat");
  const std::string part1 = "FLUSH_EX_PART_1_";
  const std::string part2 = "FLUSH_EX_PART_2_";
  const std::string part3 = "FLUSH_EX_PART_3";

  {
    auto h = OpenRwFile(path, CREATE_ALWAYS);
    ASSERT_WIN32_OK(h);

    nt::IoStatusBlock iosb{};
    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), part1.data(), part1.size()));
    EXPECT_NT_STATUS(
        ntdll.NtFlushBuffersFileEx(h.value->get(), 0UL, nullptr, 0UL, &iosb),
        STATUS_SUCCESS);

    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), part2.data(), part2.size()));
    EXPECT_NT_STATUS(ntdll.NtFlushBuffersFileEx(h.value->get(),
                                                nt::kFlushFlagsFileDataOnly,
                                                nullptr, 0UL, &iosb),
                     STATUS_SUCCESS);

    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), part3.data(), part3.size()));
    EXPECT_NT_STATUS(ntdll.NtFlushBuffersFileEx(h.value->get(),
                                                nt::kFlushFlagsFileDataSyncOnly,
                                                nullptr, 0UL, &iosb),
                     STATUS_SUCCESS);

    // Non-null Parameters / non-zero ParametersSize must fail
    // STATUS_INVALID_PARAMETER per WDK NtFlushBuffersFileEx documentation.
    ULONG reserved_param = 0;
    EXPECT_NT_STATUS(ntdll.NtFlushBuffersFileEx(h.value->get(), 0UL,
                                                &reserved_param,
                                                sizeof(reserved_param), &iosb),
                     STATUS_INVALID_PARAMETER);
  }

  auto h_ro = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h_ro);

  nt::IoStatusBlock ro_iosb{};
  EXPECT_NT_STATUS(ntdll.NtFlushBuffersFileEx(h_ro.value->get(),
                                              nt::kFlushFlagsFileDataOnly,
                                              nullptr, 0UL, &ro_iosb),
                   STATUS_ACCESS_DENIED);
  EXPECT_NT_STATUS(ntdll.NtFlushBuffersFileEx(h_ro.value->get(),
                                              nt::kFlushFlagsFileDataSyncOnly,
                                              nullptr, 0UL, &ro_iosb),
                   STATUS_ACCESS_DENIED);

  auto data = core::ReadAllBytes(h_ro.value->get());
  ASSERT_WIN32_OK(data);
  EXPECT_EQ(*data.value, part1 + part2 + part3);
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 -- multi-megabyte writes across irregular
// chunk sizes preserve byte-for-byte integrity on random and sequential reads.
TEST_F(DataIoTest, LargeReadWriteMultiMegabyteIntegrity) {
  const std::wstring path = TestPath(L"large_4mib.dat");
  constexpr size_t kTotalBytes = 4u * 1024u * 1024u;
  constexpr std::array<size_t, 3> kChunkSizes = {65536u, 131072u, 13312u};

  {
    auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h);

    std::vector<uint8_t> chunk_buf(131072u);
    size_t offset = 0;
    size_t step = 0;
    while (offset < kTotalBytes) {
      const size_t len = std::min(kChunkSizes[step % kChunkSizes.size()],
                                  kTotalBytes - offset);
      for (size_t i = 0; i < len; ++i) {
        chunk_buf[i] = PatternByte(offset + i);
      }
      auto wr = core::WriteAllBytes(h.value->get(), chunk_buf.data(), len);
      ASSERT_WIN32_OK(wr);
      offset += len;
      ++step;
    }
  }

  auto h = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h);

  LARGE_INTEGER sz{};
  ASSERT_WIN32_TRUE(::GetFileSizeEx(h.value->get(), &sz));
  ASSERT_EQ(sz.QuadPart, static_cast<LONGLONG>(kTotalBytes));

  // Random-offset spot checks.
  constexpr std::array<size_t, 5> kSpotOffsets = {
      0u, 13311u, 524295u, (2u * 1024u * 1024u) - 19u, kTotalBytes - 4099u};
  std::vector<uint8_t> spot(4099u);
  for (size_t off : kSpotOffsets) {
    const size_t len = std::min(spot.size(), kTotalBytes - off);
    ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), static_cast<int64_t>(off)));
    DWORD bytes_read = 0;
    ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), spot.data(),
                                 static_cast<DWORD>(len), &bytes_read,
                                 nullptr));
    ASSERT_EQ(bytes_read, static_cast<DWORD>(len));
    for (size_t i = 0; i < len; ++i) {
      ASSERT_EQ(spot[i], PatternByte(off + i))
          << "Mismatch at offset " << (off + i);
    }
  }

  // Full sequential verification.
  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  std::vector<uint8_t> read_buf(65536u);
  size_t verified = 0;
  while (verified < kTotalBytes) {
    DWORD bytes_read = 0;
    ASSERT_WIN32_TRUE(::ReadFile(h.value->get(), read_buf.data(),
                                 static_cast<DWORD>(read_buf.size()),
                                 &bytes_read, nullptr));
    ASSERT_GT(bytes_read, 0u);
    for (DWORD i = 0; i < bytes_read; ++i) {
      ASSERT_EQ(read_buf[i], PatternByte(verified + i))
          << "Sequential mismatch at offset " << (verified + i);
    }
    verified += bytes_read;
  }
  EXPECT_EQ(verified, kTotalBytes);
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 -- 0-byte ReadFile succeeds transferring 0
// bytes and 0-byte WriteFile past EOF does not extend EndOfFile.
TEST_F(DataIoTest, ZeroByteReadAndZeroByteWritePastEofDoesNotExtend) {
  const std::wstring path = TestPath(L"zero_rw_past_eof.dat");
  const std::string initial = "1234567890";

  auto h = OpenRwFile(path, CREATE_ALWAYS);
  ASSERT_WIN32_OK(h);
  ASSERT_WIN32_OK(
      core::WriteAllBytes(h.value->get(), initial.data(), initial.size()));

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  uint8_t dummy = 0x55;
  DWORD transferred = 42;
  ASSERT_WIN32_TRUE(
      ::ReadFile(h.value->get(), &dummy, 0, &transferred, nullptr));
  EXPECT_EQ(transferred, 0u);

  // Seek far past EOF and perform a 0-byte write -> must NOT extend EndOfFile.
  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 65536));
  transferred = 42;
  ASSERT_WIN32_TRUE(
      ::WriteFile(h.value->get(), &dummy, 0, &transferred, nullptr));
  EXPECT_EQ(transferred, 0u);

  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(),
                                   static_cast<int64_t>(initial.size())));

  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));
  auto contents = core::ReadAllBytes(h.value->get());
  ASSERT_WIN32_OK(contents);
  EXPECT_EQ(*contents.value, initial);
}

// Spec: [MS-FSA] 2.1.5.4 Write -- OVERLAPPED Offset/OffsetHigh set to
// 0xFFFFFFFF atomically appends at current EOF regardless of file pointer.
TEST_F(DataIoTest, OverlappedWriteToEndOfFileAtomicAppend) {
  const std::wstring path = TestPath(L"ov_atomic_append.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  auto init = core::WriteFileContents(path, "HEAD_");
  ASSERT_WIN32_OK(init);

  // Synchronous handle with seek at 0 + OVERLAPPED(-1) appends at EOF.
  {
    auto h = OpenRwFile(path, OPEN_EXISTING);
    ASSERT_WIN32_OK(h);
    ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 0));

    OVERLAPPED ov = MakeOffsetOverlapped(0xFFFFFFFFFFFFFFFFULL);
    const std::string tail1 = "TAIL1_";
    DWORD written = 0;
    ASSERT_WIN32_TRUE(::WriteFile(h.value->get(), tail1.data(),
                                  static_cast<DWORD>(tail1.size()), &written,
                                  &ov));
    EXPECT_EQ(written, static_cast<DWORD>(tail1.size()));
  }

  // Overlapped handle with Offset=0xFFFFFFFF, OffsetHigh=0xFFFFFFFF appends.
  {
    auto h = OpenRwFile(path, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
    ASSERT_WIN32_OK(h);

    const std::string tail2 = "TAIL2";
    auto wr =
        OverlappedWriteAt(h.value->get(), 0xFFFFFFFFFFFFFFFFULL, tail2.data(),
                          static_cast<DWORD>(tail2.size()), timeout_ms);
    ASSERT_WIN32_OK(wr);
    EXPECT_EQ(*wr.value, static_cast<DWORD>(tail2.size()));
  }

  EXPECT_TRUE(helpers::CheckFileContents(path, "HEAD_TAIL1_TAIL2"));
}

// Spec: Win32 CopyFileExW -- copies primary data, invokes the progress
// callback, honors COPY_FILE_FAIL_IF_EXISTS, and overwrites writable targets.
TEST_F(DataIoTest, CopyFileExRespectsFailIfExists) {
  const std::wstring src = TestPath(L"copy_src.dat");
  const std::wstring dst = TestPath(L"copy_dst.dat");
  const std::string main_payload = "COPY_SRC_CONTENT_12345";

  ASSERT_WIN32_OK(core::WriteFileContents(src, main_payload));

  CopyProgressState progress{};
  ASSERT_WIN32_TRUE(::CopyFileExW(
      src.c_str(), dst.c_str(), &CopyProgressCallback, &progress, nullptr, 0));
  EXPECT_GT(progress.calls, 0);
  EXPECT_EQ(progress.last_total, static_cast<LONGLONG>(main_payload.size()));
  EXPECT_EQ(progress.last_transferred,
            static_cast<LONGLONG>(main_payload.size()));
  EXPECT_TRUE(helpers::CheckFileContents(dst, main_payload));

  // Update source, then verify COPY_FILE_FAIL_IF_EXISTS fails with
  // ERROR_FILE_EXISTS and preserves the existing destination content.
  const std::string updated_payload = "UPDATED_COPY_CONTENT_XYZ";
  ASSERT_WIN32_OK(core::WriteFileContents(src, updated_payload));
  EXPECT_WIN32_ERROR(::CopyFileExW(src.c_str(), dst.c_str(), nullptr, nullptr,
                                   nullptr, COPY_FILE_FAIL_IF_EXISTS),
                     ERROR_FILE_EXISTS);
  EXPECT_TRUE(helpers::CheckFileContents(dst, main_payload));

  // Overwriting a normal writable destination succeeds with updated content.
  ASSERT_WIN32_TRUE(
      ::CopyFileExW(src.c_str(), dst.c_str(), nullptr, nullptr, nullptr, 0));
  EXPECT_TRUE(helpers::CheckFileContents(dst, updated_payload));
}

// Spec: Win32 CopyFileExW / [MS-FSA] 2.1.5.1 -- CopyFileExW preserves named
// alternate data streams alongside default stream contents.
TEST_F(DataIoTest, CopyFileExPreservesAlternateStreams) {
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring src = TestPath(L"copy_ads_src.dat");
  const std::wstring dst = TestPath(L"copy_ads_dst.dat");
  const std::string main_payload = "COPY_ADS_MAIN_CONTENT_12345";
  const std::string ads_payload = "ADS_META_PAYLOAD_98765";

  ASSERT_WIN32_OK(core::WriteFileContents(src, main_payload));
  auto ads_wr = core::WriteFileContents(src + L":meta", ads_payload);
  ASSERT_WIN32_OK(ads_wr);

  ASSERT_WIN32_TRUE(
      ::CopyFileExW(src.c_str(), dst.c_str(), nullptr, nullptr, nullptr, 0));

  EXPECT_TRUE(helpers::CheckFileContents(dst, main_payload));
  EXPECT_TRUE(helpers::CheckFileContents(dst + L":meta", ads_payload));
}

// Spec: Win32 CopyFileExW / [MS-FSA] 2.1.5.1.2 -- overwriting a READONLY
// destination fails ERROR_ACCESS_DENIED and preserves destination contents.
TEST_F(DataIoTest, CopyFileExOverReadOnlyDestinationFailsAccessDenied) {
  const std::wstring src = TestPath(L"copy_ro_src.dat");
  const std::wstring dst = TestPath(L"copy_ro_dst.dat");
  const std::string src_payload = "NEW_SRC_PAYLOAD_BLOCKED";
  const std::string dst_payload = "ORIGINAL_READONLY_DST_DATA";

  ASSERT_WIN32_OK(core::WriteFileContents(src, src_payload));
  ASSERT_WIN32_OK(core::WriteFileContents(dst, dst_payload));
  ASSERT_WIN32_TRUE(::SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_READONLY));

  EXPECT_WIN32_ERROR(
      ::CopyFileExW(src.c_str(), dst.c_str(), nullptr, nullptr, nullptr, 0),
      ERROR_ACCESS_DENIED);

  auto dst_data = core::ReadFileContents(dst);
  EXPECT_WIN32_TRUE(::SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL));

  ASSERT_WIN32_OK(dst_data);
  EXPECT_EQ(*dst_data.value, dst_payload);
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 -- NtReadFile and NtWriteFile honor explicit
// ByteOffset, FILE_WRITE_TO_END_OF_FILE (-1) append, and STATUS_END_OF_FILE.
TEST_F(DataIoTest, NtReadWriteFileExplicitByteOffset) {
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtWriteFile, nullptr);
  ASSERT_NE(ntdll.NtReadFile, nullptr);

  const std::wstring nt_path = TestNtPath(L"nt_rw_offset.dat");
  auto h = core::NtCreateFilePath(
      nt_path, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOverwriteIf,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert);
  ASSERT_NT_SUCCESS(h.status);
  ASSERT_TRUE(h.value.has_value());

  const std::string header = "HEADER__";
  const std::string mid = "MID_DATA";
  const std::string eof_part = "_EOF_END";
  ULONG_PTR info = 0;

  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), 0,
                                const_cast<char *>(header.data()),
                                static_cast<ULONG>(header.size()),
                                /*is_write=*/true, &info),
                   STATUS_SUCCESS);
  EXPECT_EQ(info, static_cast<ULONG_PTR>(header.size()));

  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), 1024,
                                const_cast<char *>(mid.data()),
                                static_cast<ULONG>(mid.size()),
                                /*is_write=*/true, &info),
                   STATUS_SUCCESS);
  EXPECT_EQ(info, static_cast<ULONG_PTR>(mid.size()));

  // FILE_WRITE_TO_END_OF_FILE (ByteOffset.QuadPart == -1).
  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), -1,
                                const_cast<char *>(eof_part.data()),
                                static_cast<ULONG>(eof_part.size()),
                                /*is_write=*/true, &info),
                   STATUS_SUCCESS);
  EXPECT_EQ(info, static_cast<ULONG_PTR>(eof_part.size()));

  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(), 1040));

  // Explicit-offset reads.
  char buf[16]{};
  ASSERT_NT_STATUS(
      NtTransferAt(h.value->get(), 0, buf, 8, /*is_write=*/false, &info),
      STATUS_SUCCESS);
  EXPECT_EQ(info, ULONG_PTR{8});
  EXPECT_EQ(std::string(buf, 8), header);

  ASSERT_NT_STATUS(
      NtTransferAt(h.value->get(), 1024, buf, 16, /*is_write=*/false, &info),
      STATUS_SUCCESS);
  EXPECT_EQ(info, ULONG_PTR{16});
  EXPECT_EQ(std::string(buf, 16), mid + eof_part);

  // Gap reads back as zeroes.
  std::memset(buf, 0x77, sizeof(buf));
  ASSERT_NT_STATUS(
      NtTransferAt(h.value->get(), 500, buf, 16, /*is_write=*/false, &info),
      STATUS_SUCCESS);
  EXPECT_EQ(info, ULONG_PTR{16});
  EXPECT_TRUE(helpers::AllBytesEqual(std::string_view(buf, sizeof(buf)), '\0'));

  // Read past EOF returns STATUS_END_OF_FILE.
  EXPECT_NT_STATUS(
      NtTransferAt(h.value->get(), 2000, buf, 8, /*is_write=*/false),
      STATUS_END_OF_FILE);
}

// Spec: WDK NtReadFile / NtWriteFile -- a negative ByteOffset other than the
// special values FILE_WRITE_TO_END_OF_FILE (-1) and
// FILE_USE_FILE_POINTER_POSITION (-2) fails STATUS_INVALID_PARAMETER and
// leaves the file untouched. (The I/O manager validates this before the
// request reaches the file system; the test guards that a mounted file system
// never sees, and never acts on, such an offset.)
TEST_F(DataIoTest, NtReadWriteFileNegativeByteOffsetFailsInvalidParameter) {
  const std::wstring nt_path = TestNtPath(L"nt_rw_negative_offset.dat");
  auto h = core::NtCreateFilePath(
      nt_path, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOverwriteIf,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert);
  ASSERT_NT_SUCCESS(h.status);
  ASSERT_TRUE(h.value.has_value());

  const std::string original = "NEGATIVE_OFFSET_GUARD";
  ULONG_PTR info = 0;
  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), 0,
                                const_cast<char *>(original.data()),
                                static_cast<ULONG>(original.size()),
                                /*is_write=*/true, &info),
                   STATUS_SUCCESS);
  ASSERT_EQ(info, static_cast<ULONG_PTR>(original.size()));

  const std::string intruder = "XXXXXXXX";
  char buf[8]{};
  for (const LONGLONG bad_offset : {LONGLONG{-3}, LONGLONG{-4096},
                                    (std::numeric_limits<LONGLONG>::min)()}) {
    SCOPED_TRACE("ByteOffset=" + std::to_string(bad_offset));
    EXPECT_NT_STATUS(NtTransferAt(h.value->get(), bad_offset,
                                  const_cast<char *>(intruder.data()),
                                  static_cast<ULONG>(intruder.size()),
                                  /*is_write=*/true),
                     STATUS_INVALID_PARAMETER);
    EXPECT_NT_STATUS(NtTransferAt(h.value->get(), bad_offset, buf,
                                  static_cast<ULONG>(sizeof(buf)),
                                  /*is_write=*/false),
                     STATUS_INVALID_PARAMETER);
  }

  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(),
                                   static_cast<int64_t>(original.size())));
  std::string readback(original.size(), '\0');
  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), 0, readback.data(),
                                static_cast<ULONG>(readback.size()),
                                /*is_write=*/false, &info),
                   STATUS_SUCCESS);
  EXPECT_EQ(info, static_cast<ULONG_PTR>(original.size()));
  EXPECT_EQ(readback, original);
}

// Spec: WDK NtReadFile / NtWriteFile / [MS-FSA] 2.1.5.3-4 -- on a synchronous
// handle, ByteOffset FILE_USE_FILE_POINTER_POSITION (-2) transfers at the
// file object's current byte offset and advances it by the bytes transferred.
TEST_F(DataIoTest, NtReadWriteFileUseFilePointerPosition) {
  constexpr LONGLONG kUseFilePointerPosition = -2;

  const std::wstring nt_path = TestNtPath(L"nt_rw_file_pointer.dat");
  auto h = core::NtCreateFilePath(
      nt_path, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOverwriteIf,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert);
  ASSERT_NT_SUCCESS(h.status);
  ASSERT_TRUE(h.value.has_value());

  const std::string part_a = "AAAA";
  const std::string part_b = "BBBB";
  const std::string part_c = "CCCC";
  ULONG_PTR info = 0;
  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), 0,
                                const_cast<char *>(part_a.data()),
                                static_cast<ULONG>(part_a.size()),
                                /*is_write=*/true, &info),
                   STATUS_SUCCESS);
  ASSERT_EQ(info, static_cast<ULONG_PTR>(part_a.size()));

  // Two consecutive pointer-relative writes starting at offset 4 must land
  // back to back, proving the second one saw the advanced position.
  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 4));
  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), kUseFilePointerPosition,
                                const_cast<char *>(part_b.data()),
                                static_cast<ULONG>(part_b.size()),
                                /*is_write=*/true, &info),
                   STATUS_SUCCESS);
  EXPECT_EQ(info, static_cast<ULONG_PTR>(part_b.size()));
  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), kUseFilePointerPosition,
                                const_cast<char *>(part_c.data()),
                                static_cast<ULONG>(part_c.size()),
                                /*is_write=*/true, &info),
                   STATUS_SUCCESS);
  EXPECT_EQ(info, static_cast<ULONG_PTR>(part_c.size()));

  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(), 12));
  int64_t pos = -1;
  ASSERT_WIN32_TRUE(SeekTo(h.value->get(), 0, FILE_CURRENT, &pos));
  EXPECT_EQ(pos, 12);

  // Pointer-relative reads: 4 bytes from offset 2, then the 6 remaining
  // bytes, then STATUS_END_OF_FILE.
  char buf[16]{};
  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), 2));
  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), kUseFilePointerPosition, buf, 4,
                                /*is_write=*/false, &info),
                   STATUS_SUCCESS);
  ASSERT_EQ(info, ULONG_PTR{4});
  EXPECT_EQ(std::string(buf, 4), "AABB");

  ASSERT_NT_STATUS(NtTransferAt(h.value->get(), kUseFilePointerPosition, buf,
                                static_cast<ULONG>(sizeof(buf)),
                                /*is_write=*/false, &info),
                   STATUS_SUCCESS);
  ASSERT_EQ(info, ULONG_PTR{6});
  EXPECT_EQ(std::string(buf, 6), "BBCCCC");

  EXPECT_NT_STATUS(NtTransferAt(h.value->get(), kUseFilePointerPosition, buf,
                                static_cast<ULONG>(sizeof(buf)),
                                /*is_write=*/false),
                   STATUS_END_OF_FILE);
}

// Spec: [MS-FSA] 2.1.5.15.7 / 2.1.5.15.15 FileValidDataLengthInformation --
// SetFileValidData requires SeManageVolumePrivilege at open time, rejects
// VDL > EndOfFile and VDL shrink, and extends valid data length up to EOF.
TEST_F(DataIoTest, FileValidDataLengthExtend) {
  REQUIRE_LOCAL_VOLUME();

  // SE_MANAGE_VOLUME_NAME must be enabled BEFORE opening the privileged handle.
  core::ScopedPrivilege manage_vol(SE_MANAGE_VOLUME_NAME, true);
  if (!manage_vol) {
    RecordSkipCategory("privilege");
    GTEST_SKIP() << "SeManageVolumePrivilege unavailable: "
                 << core::FormatWin32Error(manage_vol.error());
  }

  const std::wstring path = TestPath(L"vdl_extend.dat");
  auto h = OpenRwFile(path, CREATE_ALWAYS);
  ASSERT_WIN32_OK(h);

  // Ensure file is not inherited-compressed (SetFileValidData rejects
  // compressed files on NTFS).
  auto info = core::GetByHandleInfo(h.value->get());
  ASSERT_WIN32_OK(info);
  if ((info.value->dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0) {
    auto decompress =
        core::SetCompressionState(h.value->get(), COMPRESSION_FORMAT_NONE);
    ASSERT_WIN32_OK(decompress);
  }

  constexpr LONGLONG kVdl1 = 65536;
  ASSERT_WIN32_TRUE(SeekBegin(h.value->get(), kVdl1));
  ASSERT_WIN32_TRUE(::SetEndOfFile(h.value->get()));

  // Negative check 1: handle opened without SeManageVolumePrivilege fails
  // SetFileValidData with ERROR_PRIVILEGE_NOT_HELD (1314).
  auto unpriv_token = core::CreateTokenWithoutPrivilege(SE_MANAGE_VOLUME_NAME);
  ASSERT_WIN32_OK(unpriv_token);
  {
    core::ScopedImpersonation imp(unpriv_token.value->get());
    ASSERT_TRUE(imp) << core::FormatWin32Error(imp.error());
    auto h_unpriv = OpenRwFile(path, OPEN_EXISTING);
    ASSERT_WIN32_OK(h_unpriv);
    EXPECT_WIN32_ERROR(::SetFileValidData(h_unpriv.value->get(), kVdl1),
                       ERROR_PRIVILEGE_NOT_HELD);
  }

  // Negative check 2: setting VDL > EndOfFile fails INVALID_PARAMETER.
  EXPECT_WIN32_ERROR(::SetFileValidData(h.value->get(), kVdl1 + 4096),
                     ERROR_INVALID_PARAMETER);
  EXPECT_NT_STATUS(SetValidDataLengthNt(h.value->get(), kVdl1 + 4096),
                   STATUS_INVALID_PARAMETER);

  // Positive check 1: privileged handle extends VDL to EOF via Win32.
  ASSERT_WIN32_TRUE(::SetFileValidData(h.value->get(), kVdl1));
  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(), kVdl1));

  // Negative check 3: shrinking VDL below current VDL fails
  // STATUS_INVALID_PARAMETER.
  EXPECT_NT_STATUS(SetValidDataLengthNt(h.value->get(), 4096),
                   STATUS_INVALID_PARAMETER);

  // Positive check 2: direct NT FileValidDataLengthInformation extends further.
  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtSetInformationFile, nullptr);
  constexpr LONGLONG kVdl2 = 131072;
  ASSERT_NT_SUCCESS(core::SetEndOfFileNt(h.value->get(), kVdl2).status);
  ASSERT_NT_SUCCESS(SetValidDataLengthNt(h.value->get(), kVdl2));
  EXPECT_TRUE(CheckStandardInfoEof(h.value->get(), kVdl2));
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 / Win32 ReadFile / WriteFile -- parallel
// create/write/read/verify/delete and directory enumeration complete cleanly
// without corruption or orphaned files.
TEST_F(DataIoTest, ConcurrentMultiThreadedReadWriteAndDirStress) {
  const std::wstring stress_dir = TestPath(L"stress_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(stress_dir.c_str(), nullptr));

  constexpr int kNumThreads = 4;
  constexpr int kIterations = 30;
  constexpr ULONGLONG kJoinBudgetMs = 120000;

  // Each worker context is heap-allocated and co-owned by its ScopedThread,
  // so a worker that is still running after a timed-out Join() keeps a live
  // context and never touches this frame. Contexts are declared before the
  // threads so the threads are torn down first.
  std::array<std::shared_ptr<StressWorkerContext>, kNumThreads> contexts;
  for (int t = 0; t < kNumThreads; ++t) {
    auto ctx = std::make_shared<StressWorkerContext>();
    ctx->tid = t;
    ctx->iterations = kIterations;
    ctx->stress_dir = stress_dir;
    contexts[static_cast<size_t>(t)] = std::move(ctx);
  }

  std::array<ScopedThread, kNumThreads> workers;
  for (int t = 0; t < kNumThreads; ++t) {
    const size_t i = static_cast<size_t>(t);
    ASSERT_TRUE(workers[i].Start(&StressWorkerContext::Run, contexts[i]))
        << core::FormatWin32Error(workers[i].LastError());
  }

  // Explicitly join every worker (sharing one overall time budget) before
  // looking at any result; a context whose worker did not exit is never read.
  const ULONGLONG deadline = ::GetTickCount64() + kJoinBudgetMs;
  bool all_joined = true;
  for (int t = 0; t < kNumThreads; ++t) {
    const size_t i = static_cast<size_t>(t);
    const ULONGLONG now = ::GetTickCount64();
    const DWORD remaining_ms =
        (now < deadline) ? static_cast<DWORD>(deadline - now) : DWORD{0};
    if (!workers[i].Join(remaining_ms)) {
      all_joined = false;
      ADD_FAILURE() << "Worker " << t << " did not exit within "
                    << kJoinBudgetMs << " ms: "
                    << core::FormatWin32Error(workers[i].LastError());
      continue;
    }
    EXPECT_TRUE(contexts[i]->ok)
        << "Worker " << t << " failed: " << contexts[i]->error;
  }
  ASSERT_TRUE(all_joined);

  EXPECT_TRUE(helpers::CheckDirEntries(stress_dir, {L".", L".."}));
}

// Spec: [MS-FSA] 2.1.5.15.1 / 2.1.5.15.5 -- setting FileEndOfFileInformation or
// FileAllocationInformation on a directory fails STATUS_INVALID_PARAMETER.
TEST_F(DataIoTest, SetEndOfFileAndAllocationOnDirectoryFails) {
  const std::wstring dir = TestPath(L"eof_alloc_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  auto h = core::CreateWin32File(
      dir, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
      core::kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(h);

  EXPECT_NT_STATUS(core::SetEndOfFileNt(h.value->get(), 64).status,
                   STATUS_INVALID_PARAMETER);
  EXPECT_NT_STATUS(core::SetAllocationSizeNt(h.value->get(), 64).status,
                   STATUS_INVALID_PARAMETER);
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.5.4 -- ReadFile and WriteFile on a directory
// handle fail STATUS_INVALID_DEVICE_REQUEST (Win32 ERROR_INVALID_FUNCTION) and
// leave the directory unchanged.
TEST_F(DataIoTest, ReadWriteOnDirectoryHandleFailsInvalidFunction) {
  const std::wstring dir = TestPath(L"rw_on_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));

  // FILE_LIST_DIRECTORY and FILE_ADD_FILE are the directory aliases of
  // FILE_READ_DATA and FILE_WRITE_DATA, so the I/O manager's access check
  // passes and both requests reach the file system.
  auto h = core::CreateWin32File(
      dir, FILE_LIST_DIRECTORY | FILE_ADD_FILE | SYNCHRONIZE, core::kShareAll,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(h);

  char buf[16] = "DIR_DATA_IO_REQ";
  DWORD transferred = 0;
  EXPECT_WIN32_ERROR(::ReadFile(h.value->get(), buf,
                                static_cast<DWORD>(sizeof(buf)), &transferred,
                                nullptr),
                     ERROR_INVALID_FUNCTION);
  EXPECT_WIN32_ERROR(::WriteFile(h.value->get(), buf,
                                 static_cast<DWORD>(sizeof(buf)), &transferred,
                                 nullptr),
                     ERROR_INVALID_FUNCTION);

  h.value->reset();
  EXPECT_TRUE(helpers::CheckDirEntries(dir, {L".", L".."}));
}

void ForceLink_DataIo() {}

} // namespace fstest
