// SPDX-License-Identifier: MIT

#pragma once

#include "assertions.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/status.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {

// Non-copyable RAII wrapper around a Win32 CRITICAL_SECTION (avoids standard
// mutex headers, keeping suite targets compatible with
// /EHs-c- -D_HAS_EXCEPTIONS=0).
class ScopedCriticalSection {
public:
  ScopedCriticalSection() noexcept { ::InitializeCriticalSection(&cs_); }
  ~ScopedCriticalSection() { ::DeleteCriticalSection(&cs_); }

  ScopedCriticalSection(const ScopedCriticalSection &) = delete;
  ScopedCriticalSection &operator=(const ScopedCriticalSection &) = delete;

  void Lock() noexcept { ::EnterCriticalSection(&cs_); }
  void Unlock() noexcept { ::LeaveCriticalSection(&cs_); }
  CRITICAL_SECTION *get() noexcept { return &cs_; }

private:
  CRITICAL_SECTION cs_{};
};

// Scoped RAII lock guard for ScopedCriticalSection or raw CRITICAL_SECTION*.
class ScopedCsLock {
public:
  explicit ScopedCsLock(ScopedCriticalSection &cs) noexcept : cs_(cs.get()) {
    ::EnterCriticalSection(cs_);
  }
  explicit ScopedCsLock(CRITICAL_SECTION *cs) noexcept : cs_(cs) {
    if (cs_ != nullptr) {
      ::EnterCriticalSection(cs_);
    }
  }
  ~ScopedCsLock() {
    if (cs_ != nullptr) {
      ::LeaveCriticalSection(cs_);
    }
  }

  ScopedCsLock(const ScopedCsLock &) = delete;
  ScopedCsLock &operator=(const ScopedCsLock &) = delete;

private:
  CRITICAL_SECTION *cs_ = nullptr;
};

// Builds a zero-initialized OVERLAPPED positioned at a 64-bit byte offset.
inline OVERLAPPED MakeOffsetOverlapped(uint64_t offset,
                                       HANDLE event = nullptr) noexcept {
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  ov.hEvent = event;
  return ov;
}

// Seeks a file handle to an absolute byte offset from FILE_BEGIN.
inline bool SeekBegin(HANDLE h, int64_t offset) noexcept {
  LARGE_INTEGER dist{};
  dist.QuadPart = offset;
  return ::SetFilePointerEx(h, dist, nullptr, FILE_BEGIN) != FALSE;
}

// Move-only RAII wrapper around a Win32 CreateThread handle.
//
// The worker context is always heap-allocated and co-owned by the thread
// wrapper (std::shared_ptr): `start_routine` receives `context.get()`. Callers
// must explicitly Join(timeout_ms) before inspecting results. If a worker is
// still running when Join() times out and the wrapper is destroyed (or
// restarted), ~ScopedThread() waits a bounded grace timeout, emits
// ADD_FAILURE(), closes the thread handle, and intentionally leaks one strong
// reference to the context so the hung worker can never read or write freed
// memory (and never a dead stack frame). Everything a worker dereferences must
// therefore live inside the shared context (or be otherwise immortal); kernel
// handles referenced by value are safe (a closed handle fails cleanly).
class ScopedThread {
public:
  static constexpr DWORD kDestructorJoinTimeoutMs = 5000;

  ScopedThread() noexcept = default;
  ScopedThread(LPTHREAD_START_ROUTINE start_routine,
               std::shared_ptr<void> context) noexcept {
    Start(start_routine, std::move(context));
  }

  ~ScopedThread() { TeardownJoin(); }

  ScopedThread(const ScopedThread &) = delete;
  ScopedThread &operator=(const ScopedThread &) = delete;

  ScopedThread(ScopedThread &&other) noexcept
      : handle_(std::move(other.handle_)), context_(std::move(other.context_)),
        last_error_(std::exchange(other.last_error_, ERROR_SUCCESS)) {}

  ScopedThread &operator=(ScopedThread &&other) noexcept {
    if (this != &other) {
      TeardownJoin();
      handle_ = std::move(other.handle_);
      context_ = std::move(other.context_);
      last_error_ = std::exchange(other.last_error_, ERROR_SUCCESS);
    }
    return *this;
  }

  // Starts `start_routine(context.get())` on a new thread. `context` must be
  // non-null: the wrapper keeps it alive for as long as the worker may run.
  bool Start(LPTHREAD_START_ROUTINE start_routine,
             std::shared_ptr<void> context) noexcept {
    TeardownJoin();
    last_error_ = ERROR_SUCCESS;
    if (start_routine == nullptr || context == nullptr) {
      last_error_ = ERROR_INVALID_PARAMETER;
      return false;
    }
    ::SetLastError(ERROR_SUCCESS);
    HANDLE raw =
        ::CreateThread(nullptr, 0, start_routine, context.get(), 0, nullptr);
    if (raw == nullptr) {
      last_error_ = ::GetLastError();
      if (last_error_ == ERROR_SUCCESS) {
        last_error_ = ERROR_GEN_FAILURE;
      }
      return false;
    }
    handle_.reset(raw);
    context_ = std::move(context);
    return true;
  }

  // Waits for the worker. Returns true iff the worker has exited (or was never
  // started). On timeout the thread stays joinable and LastError() is
  // ERROR_TIMEOUT.
  bool Join(DWORD timeout_ms = INFINITE) noexcept {
    if (!handle_.valid()) {
      return true;
    }
    ::SetLastError(ERROR_SUCCESS);
    const DWORD wait = ::WaitForSingleObject(handle_.get(), timeout_ms);
    if (wait == static_cast<DWORD>(WAIT_OBJECT_0)) {
      handle_.reset();
      context_.reset();
      last_error_ = ERROR_SUCCESS;
      return true;
    }
    if (wait == static_cast<DWORD>(WAIT_TIMEOUT)) {
      last_error_ = ERROR_TIMEOUT;
    } else {
      last_error_ = ::GetLastError();
      if (last_error_ == ERROR_SUCCESS) {
        last_error_ = ERROR_GEN_FAILURE;
      }
    }
    return false;
  }

  bool Joinable() const noexcept { return handle_.valid(); }
  explicit operator bool() const noexcept { return handle_.valid(); }
  HANDLE Handle() const noexcept { return handle_.get(); }
  DWORD LastError() const noexcept { return last_error_; }

private:
  void TeardownJoin() noexcept {
    if (!handle_.valid()) {
      context_.reset();
      return;
    }
    if (!Join(kDestructorJoinTimeoutMs)) {
      ADD_FAILURE() << "ScopedThread timed out after "
                    << kDestructorJoinTimeoutMs
                    << " ms waiting for worker thread during teardown ("
                    << core::FormatWin32Error(last_error_)
                    << "); closing handle and leaking the worker context.";
      handle_.reset();
      // Deliberate leak: the worker is still running and still dereferences
      // the context. Pin one strong reference forever instead of freeing it.
      // (Plain `new`: allocation failure terminates rather than freeing the
      // context under a live worker.)
      if (context_ != nullptr) {
        auto *pinned = new std::shared_ptr<void>(std::move(context_));
        static_cast<void>(pinned);
      }
      context_.reset();
    }
  }

  core::UniqueHandle handle_;
  std::shared_ptr<void> context_;
  DWORD last_error_ = ERROR_SUCCESS;
};

// Backdates all four NT basic timestamps (Creation, LastAccess, LastWrite,
// Change) on a dedicated handle and closes it immediately so handle-level
// timestamp freezing does not interfere with subsequent operations.
inline bool BackdateAllTimes(const std::wstring &path, int64_t time_100ns,
                             bool is_dir = false) {
  const DWORD flags =
      is_dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
  auto opened = core::CreateWin32File(path, FILE_WRITE_ATTRIBUTES,
                                      core::kShareAll, OPEN_EXISTING, flags);
  if (!opened) {
    return false;
  }
  nt::FileBasicInfo info{};
  info.creation_time.QuadPart = time_100ns;
  info.last_access_time.QuadPart = time_100ns;
  info.last_write_time.QuadPart = time_100ns;
  info.change_time.QuadPart = time_100ns;
  info.file_attributes = 0;
  return core::SetBasicInfo(opened.value->get(), info).ok();
}

namespace detail {

inline std::string FormatBytesPreview(std::string_view data) {
  constexpr size_t kMaxPreview = 128;
  const size_t limit = std::min(data.size(), kMaxPreview);
  std::ostringstream oss;
  oss << '"';
  for (size_t i = 0; i < limit; ++i) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    if (c >= 0x20u && c <= 0x7Eu && c != '"' && c != '\\') {
      oss << static_cast<char>(c);
    } else {
      oss << "\\x" << std::hex << std::uppercase << std::setw(2)
          << std::setfill('0') << static_cast<unsigned int>(c) << std::dec;
    }
  }
  oss << '"';
  if (data.size() > kMaxPreview) {
    oss << "... (" << data.size() << " bytes total)";
  }
  return oss.str();
}

} // namespace detail

namespace helpers {

// Returns the full Win32 path of the currently running executable image using a
// growable buffer up to 32768 wide characters for GetModuleFileNameW.
inline core::Win32Result<std::wstring> GetCurrentExecutablePath() {
  core::Win32Result<std::wstring> result{};
  std::wstring self_exe(1024, L'\0');
  for (;;) {
    ::SetLastError(ERROR_SUCCESS);
    const DWORD len = ::GetModuleFileNameW(nullptr, self_exe.data(),
                                           static_cast<DWORD>(self_exe.size()));
    if (len == 0) {
      result.error = ::GetLastError();
      if (result.error == ERROR_SUCCESS) {
        result.error = ERROR_GEN_FAILURE;
      }
      return result;
    }
    if (len < self_exe.size()) {
      self_exe.resize(len);
      result.error = ERROR_SUCCESS;
      result.value = std::move(self_exe);
      return result;
    }
    if (self_exe.size() >= 32768) {
      result.error = ERROR_INSUFFICIENT_BUFFER;
      return result;
    }
    self_exe.assign(self_exe.size() * 2, L'\0');
  }
}

// Verifies that `path` does not exist and querying its Win32 attributes fails
// specifically with ERROR_FILE_NOT_FOUND.
inline ::testing::AssertionResult CheckFileNotFound(std::wstring_view path) {
  auto attrs = core::QueryWin32Attributes(path);
  if (attrs) {
    std::ostringstream oss;
    oss << "Expected '" << core::WideToUtf8(path)
        << "' to not exist (ERROR_FILE_NOT_FOUND), but it exists with "
           "attributes 0x"
        << std::hex << std::uppercase << *attrs.value;
    return ::testing::AssertionFailure() << oss.str();
  }
  if (attrs.error != static_cast<DWORD>(ERROR_FILE_NOT_FOUND)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_FILE_NOT_FOUND for '" << core::WideToUtf8(path)
           << "', got " << core::FormatWin32Error(attrs.error);
  }
  return ::testing::AssertionSuccess();
}

// Verifies that `path` is absent (querying its Win32 attributes fails with
// either ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND).
inline ::testing::AssertionResult CheckPathAbsent(std::wstring_view path) {
  auto attrs = core::QueryWin32Attributes(path);
  if (attrs) {
    std::ostringstream oss;
    oss << "Expected '" << core::WideToUtf8(path)
        << "' to be absent (ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND), "
           "but it exists with attributes 0x"
        << std::hex << std::uppercase << *attrs.value;
    return ::testing::AssertionFailure() << oss.str();
  }
  if (attrs.error != static_cast<DWORD>(ERROR_FILE_NOT_FOUND) &&
      attrs.error != static_cast<DWORD>(ERROR_PATH_NOT_FOUND)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND for '"
           << core::WideToUtf8(path) << "', got "
           << core::FormatWin32Error(attrs.error);
  }
  return ::testing::AssertionSuccess();
}

// Verifies a Win32 open of a regular file with a trailing path separator
// failed with ERROR_INVALID_NAME or ERROR_DIRECTORY (both observed across
// Windows 10, Windows 11, and Windows Server builds).
inline ::testing::AssertionResult CheckTrailingSlashWin32Error(DWORD err) {
  if (err == static_cast<DWORD>(ERROR_INVALID_NAME) ||
      err == static_cast<DWORD>(ERROR_DIRECTORY)) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected ERROR_INVALID_NAME or ERROR_DIRECTORY, got "
         << core::FormatWin32Error(err);
}

// Verifies an NT open of a regular file with a trailing path separator failed
// with STATUS_OBJECT_NAME_INVALID or STATUS_NOT_A_DIRECTORY (both observed
// across Windows 10, Windows 11, and Windows Server builds).
inline ::testing::AssertionResult CheckTrailingSlashNtStatus(NTSTATUS st) {
  if (st == STATUS_OBJECT_NAME_INVALID || st == STATUS_NOT_A_DIRECTORY) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "Expected STATUS_OBJECT_NAME_INVALID or STATUS_NOT_A_DIRECTORY, "
            "got "
         << core::FormatNtStatus(st);
}

// Opens `path`, reads all bytes via core::ReadFileContents / ReadAllBytes,
// and compares against `expected`, returning a diagnostic AssertionResult.
inline ::testing::AssertionResult CheckFileContents(std::wstring_view path,
                                                    std::string_view expected) {
  auto data = core::ReadFileContents(path);
  if (!data) {
    return ::testing::AssertionFailure()
           << "Failed to read '" << core::WideToUtf8(path)
           << "': " << core::FormatWin32Error(data.error);
  }
  const std::string &actual = *data.value;
  if (actual == expected) {
    return ::testing::AssertionSuccess();
  }
  const size_t common = std::min(actual.size(), expected.size());
  size_t diff_pos = 0;
  while (diff_pos < common && actual[diff_pos] == expected[diff_pos]) {
    ++diff_pos;
  }
  std::ostringstream oss;
  oss << "Content mismatch for '" << core::WideToUtf8(path)
      << "' (actual size=" << actual.size()
      << ", expected size=" << expected.size() << ", first diff at offset "
      << diff_pos
      << "): actual=" << ::fstest::detail::FormatBytesPreview(actual)
      << ", expected=" << ::fstest::detail::FormatBytesPreview(expected);
  return ::testing::AssertionFailure() << oss.str();
}

// Enumerates `dir_path` via core::ListDirectoryWin32 and compares entries as a
// strict multiset (preserving exact multiplicities and detecting duplicates).
inline ::testing::AssertionResult
CheckDirEntries(std::wstring_view dir_path,
                const std::vector<std::wstring> &expected_sorted) {
  auto listing = core::ListDirectoryWin32(dir_path);
  if (!listing) {
    return ::testing::AssertionFailure()
           << "Failed to enumerate directory '" << core::WideToUtf8(dir_path)
           << "': " << core::FormatWin32Error(listing.error);
  }
  const std::string actual_label =
      "ListDirectoryWin32('" + core::WideToUtf8(dir_path) + "')";
  return ::fstest::detail::CheckMultisetEq(actual_label.c_str(), "expected",
                                           *listing.value, expected_sorted);
}

// Returns true iff every byte in [buf, buf + len) equals `value`.
inline bool AllBytesEqual(const uint8_t *buf, size_t len,
                          uint8_t value) noexcept {
  for (size_t i = 0; i < len; ++i) {
    if (buf[i] != value) {
      return false;
    }
  }
  return true;
}

// Returns true iff every byte in `buf` equals `value`.
inline bool AllBytesEqual(const std::vector<uint8_t> &buf,
                          uint8_t value) noexcept {
  return AllBytesEqual(buf.data(), buf.size(), value);
}

// Returns true iff every character in `buf` equals `value`.
inline bool AllBytesEqual(std::string_view buf, char value) noexcept {
  for (const char c : buf) {
    if (c != value) {
      return false;
    }
  }
  return true;
}

// Completes an overlapped Win32 I/O operation (ReadFile, WriteFile,
// DeviceIoControl, LockFileEx, UnlockFileEx). Must be called immediately after
// the overlapped API call so ::GetLastError() reflects its status when
// `initial_ok == FALSE`. On wait timeout or failure, captures the wait error
// before calling CancelIoEx + GetOverlappedResult so timeouts always report
// ERROR_TIMEOUT and wait errors are never clobbered.
//
// The post-cancel GetOverlappedResult(bWait=TRUE) drain is deliberately
// unbounded: `ov` (and usually the I/O buffer) is caller stack memory, so
// returning while the request is still in flight would let the kernel complete
// it into a dead frame. CancelIoEx only *requests* cancellation; a filesystem
// that never completes the IRP hangs this call, which the outer test-runner
// timeout reports - a hang is the safe failure mode, memory corruption is not.
inline core::Win32Result<DWORD>
CompleteOverlappedIo(HANDLE handle, OVERLAPPED *ov, HANDLE event,
                     BOOL initial_ok, DWORD timeout_ms) {
  core::Win32Result<DWORD> res{};
  if (ov == nullptr) {
    res.error = ERROR_INVALID_PARAMETER;
    return res;
  }
  if (initial_ok == FALSE) {
    const DWORD initial_err = ::GetLastError();
    if (initial_err != static_cast<DWORD>(ERROR_IO_PENDING)) {
      res.error =
          (initial_err != ERROR_SUCCESS) ? initial_err : ERROR_GEN_FAILURE;
      return res;
    }
    ::SetLastError(ERROR_SUCCESS);
    const DWORD wait_rc = ::WaitForSingleObject(event, timeout_ms);
    if (wait_rc != static_cast<DWORD>(WAIT_OBJECT_0)) {
      const DWORD wait_err = (wait_rc == static_cast<DWORD>(WAIT_TIMEOUT))
                                 ? static_cast<DWORD>(ERROR_TIMEOUT)
                                 : ::GetLastError();
      ::CancelIoEx(handle, ov);
      DWORD ignored = 0;
      ::GetOverlappedResult(handle, ov, &ignored, /*bWait=*/TRUE);
      res.error = (wait_err != ERROR_SUCCESS) ? wait_err : ERROR_GEN_FAILURE;
      return res;
    }
  }

  DWORD transferred = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::GetOverlappedResult(handle, ov, &transferred, /*bWait=*/FALSE) ==
      FALSE) {
    res.error = ::GetLastError();
    if (res.error == ERROR_SUCCESS) {
      res.error = ERROR_GEN_FAILURE;
    }
    return res;
  }
  res.error = ERROR_SUCCESS;
  res.value = transferred;
  return res;
}

// Result of helpers::QueryPathBasicInfo. Unlike a bare NtResult, it preserves
// the real Win32 error from CreateFileW when the path cannot be opened:
//   - open failed:  error = CreateFileW error (never ERROR_SUCCESS),
//                   status = STATUS_UNSUCCESSFUL, value empty.
//   - query failed: error = ERROR_SUCCESS, status = failing NTSTATUS from
//                   NtQueryInformationFile(FileBasicInformation), value empty.
//   - success:      error = ERROR_SUCCESS, status = STATUS_SUCCESS, value set.
struct PathBasicInfoResult {
  DWORD error = ERROR_SUCCESS;
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  std::optional<nt::FileBasicInfo> value;

  bool ok() const noexcept {
    return error == ERROR_SUCCESS && nt::NtSuccess(status) && value.has_value();
  }
  explicit operator bool() const noexcept { return ok(); }

  // Human-readable failure description for `<<` diagnostics.
  std::string Describe() const {
    if (ok()) {
      return "ok";
    }
    if (error != ERROR_SUCCESS) {
      return "open failed: " + core::FormatWin32Error(error);
    }
    return "FileBasicInformation query failed: " + core::FormatNtStatus(status);
  }
};

// Opens `path` (file or directory; FILE_FLAG_BACKUP_SEMANTICS is always passed)
// with FILE_READ_ATTRIBUTES and full sharing, queries FileBasicInformation, and
// closes the handle before returning. FILE_READ_ATTRIBUTES opens do not update
// timestamps and do not conflict with any share mode.
inline PathBasicInfoResult QueryPathBasicInfo(std::wstring_view path) {
  PathBasicInfoResult res{};
  auto opened =
      core::CreateWin32File(path, FILE_READ_ATTRIBUTES, core::kShareAll,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  if (!opened) {
    res.error =
        (opened.error != ERROR_SUCCESS) ? opened.error : ERROR_GEN_FAILURE;
    return res;
  }
  auto info = core::QueryBasicInfo(opened.value->get());
  res.status = info.status;
  if (info.ok()) {
    res.value = *info.value;
  } else if (nt::NtSuccess(res.status)) {
    res.status = STATUS_UNSUCCESSFUL;
  }
  return res;
}

// Queries FileAlignmentInformation ([MS-FSCC] 2.4.3): the device's buffer
// alignment mask (FILE_BYTE_ALIGNMENT = 0, FILE_WORD_ALIGNMENT = 1, ...,
// FILE_512_BYTE_ALIGNMENT = 0x1FF). For FILE_FLAG_NO_BUFFERING I/O, a user
// buffer address is misaligned iff `(address & mask) != 0`.
inline core::NtResult<ULONG> QueryAlignmentRequirement(HANDLE h) {
  core::NtResult<ULONG> res{};
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    res.status = STATUS_NOT_IMPLEMENTED;
    return res;
  }
  nt::FileAlignmentInfo info{};
  nt::IoStatusBlock iosb{};
  res.status = ntdll.NtQueryInformationFile(
      h, &iosb, &info, static_cast<ULONG>(sizeof(info)),
      nt::FileInfoClass::FileAlignmentInformation);
  res.information = iosb.information;
  if (nt::NtSuccess(res.status)) {
    res.value = info.alignment_requirement;
  }
  return res;
}

// Queries FileAccessInformation: the access mask granted to handle `h`.
inline core::NtResult<ACCESS_MASK> QueryFileAccessMask(HANDLE h) {
  core::NtResult<ACCESS_MASK> res{};
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    res.status = STATUS_NOT_IMPLEMENTED;
    return res;
  }
  nt::IoStatusBlock iosb{};
  nt::FileAccessInfo info{};
  res.status = ntdll.NtQueryInformationFile(
      h, &iosb, &info, static_cast<ULONG>(sizeof(info)),
      nt::FileInfoClass::FileAccessInformation);
  res.information = iosb.information;
  if (nt::NtSuccess(res.status)) {
    res.value = info.access_flags;
  }
  return res;
}

// Packs an unsigned 64-bit 100ns tick count into a Win32 FILETIME.
inline constexpr FILETIME U64ToFileTime(uint64_t ticks) noexcept {
  FILETIME ft{};
  ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
  ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return ft;
}

// Packs a signed 64-bit 100ns NT timestamp into a Win32 FILETIME.
inline constexpr FILETIME Int64ToFileTime(int64_t ticks) noexcept {
  return U64ToFileTime(static_cast<uint64_t>(ticks));
}

// Creates a new empty file at `path` (CREATE_NEW) and immediately closes the
// handle, returning the Win32 status without embedding ASSERT_* macros.
inline core::Win32Result<void> CreateEmptyFileResult(std::wstring_view path) {
  core::Win32Result<void> res{};
  auto h = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  if (!h) {
    res.error = h.error;
    return res;
  }
  res.error = ERROR_SUCCESS;
  return res;
}

} // namespace helpers

// Copies the currently running executable image to dest_path.
inline core::Win32Result<void>
CopyCurrentExecutableTo(const std::wstring &dest_path) {
  core::Win32Result<void> result{};
  auto self_exe = helpers::GetCurrentExecutablePath();
  if (!self_exe) {
    result.error = self_exe.error;
    return result;
  }

  ::SetLastError(ERROR_SUCCESS);
  if (::CopyFileW(self_exe.value->c_str(), dest_path.c_str(),
                  /*bFailIfExists=*/FALSE) == FALSE) {
    result.error = ::GetLastError();
    if (result.error == ERROR_SUCCESS) {
      result.error = ERROR_GEN_FAILURE;
    }
    return result;
  }
  result.error = ERROR_SUCCESS;
  return result;
}

} // namespace fstest
