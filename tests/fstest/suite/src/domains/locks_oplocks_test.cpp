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

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {
namespace {

constexpr DWORD kWaitObject0 = static_cast<DWORD>(WAIT_OBJECT_0);
constexpr DWORD kWaitTimeout = static_cast<DWORD>(WAIT_TIMEOUT);
constexpr DWORD kOplockRh = OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE;
constexpr DWORD kOplockRwh = OPLOCK_LEVEL_CACHE_READ |
                             OPLOCK_LEVEL_CACHE_WRITE |
                             OPLOCK_LEVEL_CACHE_HANDLE;

core::Win32Result<core::UniqueHandle>
OpenRw(const std::wstring &path, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
  return core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                               core::kShareAll, OPEN_EXISTING, flags);
}

BOOL LockRange(HANDLE h, uint64_t offset, uint64_t length,
               DWORD flags) noexcept {
  OVERLAPPED ov = MakeOffsetOverlapped(offset);
  const DWORD len_low = static_cast<DWORD>(length & 0xFFFFFFFFULL);
  const DWORD len_high = static_cast<DWORD>(length >> 32);
  return ::LockFileEx(h, flags, /*dwReserved=*/0, len_low, len_high, &ov);
}

BOOL UnlockRange(HANDLE h, uint64_t offset, uint64_t length) noexcept {
  OVERLAPPED ov = MakeOffsetOverlapped(offset);
  const DWORD len_low = static_cast<DWORD>(length & 0xFFFFFFFFULL);
  const DWORD len_high = static_cast<DWORD>(length >> 32);
  return ::UnlockFileEx(h, /*dwReserved=*/0, len_low, len_high, &ov);
}

void AssertLockConflict(HANDLE h, uint64_t offset, uint64_t length,
                        DWORD flags = LOCKFILE_EXCLUSIVE_LOCK |
                                      LOCKFILE_FAIL_IMMEDIATELY) {
  EXPECT_WIN32_ERROR(LockRange(h, offset, length, flags), ERROR_LOCK_VIOLATION);
}

core::Win32Result<std::string> ReadAt(HANDLE h, int64_t offset, DWORD len) {
  core::Win32Result<std::string> res{};
  if (!SeekBegin(h, offset)) {
    res.error = ::GetLastError();
    return res;
  }
  std::string buf(len, '\0');
  DWORD bytes_read = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::ReadFile(h, buf.data(), len, &bytes_read, nullptr) == FALSE) {
    res.error = ::GetLastError();
    return res;
  }
  buf.resize(bytes_read);
  res.error = ERROR_SUCCESS;
  res.value = std::move(buf);
  return res;
}

core::Win32Result<DWORD> WriteAt(HANDLE h, int64_t offset,
                                 std::string_view data) {
  core::Win32Result<DWORD> res{};
  if (!SeekBegin(h, offset)) {
    res.error = ::GetLastError();
    return res;
  }
  DWORD written = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written,
                  nullptr) == FALSE) {
    res.error = ::GetLastError();
    return res;
  }
  res.error = ERROR_SUCCESS;
  res.value = written;
  return res;
}

REQUEST_OPLOCK_INPUT_BUFFER MakeOplockRequest(DWORD level,
                                              DWORD flags) noexcept {
  REQUEST_OPLOCK_INPUT_BUFFER buf{};
  buf.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
  buf.StructureLength = sizeof(REQUEST_OPLOCK_INPUT_BUFFER);
  buf.RequestedOplockLevel = level;
  buf.Flags = flags;
  return buf;
}

// Ensures any in-flight overlapped IRP is cancelled and drained before its
// stack-scoped OVERLAPPED and output buffers unwind on an early ASSERT failure.
class PendingIoGuard {
public:
  PendingIoGuard() noexcept = default;
  PendingIoGuard(HANDLE h, OVERLAPPED *ov) noexcept
      : handle_(h), ov_(ov), armed_(h != nullptr && ov != nullptr) {}
  ~PendingIoGuard() { CancelAndDrain(); }

  PendingIoGuard(const PendingIoGuard &) = delete;
  PendingIoGuard &operator=(const PendingIoGuard &) = delete;

  void Arm(HANDLE h, OVERLAPPED *ov) noexcept {
    CancelAndDrain();
    handle_ = h;
    ov_ = ov;
    armed_ = (h != nullptr && ov != nullptr);
  }

  void Disarm() noexcept { armed_ = false; }
  bool armed() const noexcept { return armed_; }

  void CancelAndDrain() noexcept {
    if (!armed_ || handle_ == nullptr || ov_ == nullptr) {
      return;
    }
    armed_ = false;
    ::CancelIoEx(handle_, ov_);
    DWORD ignored = 0;
    ::GetOverlappedResult(handle_, ov_, &ignored, TRUE);
  }

private:
  HANDLE handle_ = nullptr;
  OVERLAPPED *ov_ = nullptr;
  bool armed_ = false;
};

// Heap-allocated (std::shared_ptr) worker context co-owned by ScopedThread: it
// owns copies of every path plus its own completion event, so a worker that
// outlives a timed-out Join() never touches the test's stack frame.
struct OplockWorkerContext {
  enum class Action {
    kOpen,
    kOpenRead,
    kNtCreate,
    kMove,
    kDelete,
    kSetRename,
    kSetDisposition,
  };

  Action action = Action::kOpen;
  std::wstring path;
  std::wstring target_path;
  core::UniqueHandle op_handle;
  bool delete_file = true;
  DWORD desired_access = GENERIC_READ;
  DWORD share_mode = core::kShareAll;
  DWORD creation_disposition = OPEN_EXISTING;
  ULONG nt_create_options = 0;
  core::UniqueHandle done_event;
  std::atomic<DWORD> err{ERROR_SUCCESS};
  std::atomic<NTSTATUS> nt_status{STATUS_UNSUCCESSFUL};
  std::atomic<bool> succeeded{false};
  std::string read_data;

  static DWORD WINAPI Run(LPVOID param) {
    auto *ctx = static_cast<OplockWorkerContext *>(param);
    if (ctx->action == Action::kOpen || ctx->action == Action::kOpenRead) {
      auto h2 = core::CreateWin32File(
          ctx->path, ctx->desired_access, ctx->share_mode,
          ctx->creation_disposition, FILE_ATTRIBUTE_NORMAL);
      if (!h2) {
        ctx->err.store(h2.error, std::memory_order_release);
      } else if (ctx->action == Action::kOpenRead) {
        auto rd = core::ReadAllBytes(h2.value->get());
        if (!rd) {
          ctx->err.store(rd.error, std::memory_order_release);
        } else {
          ctx->read_data = std::move(*rd.value);
          ctx->succeeded.store(true, std::memory_order_release);
        }
      } else {
        ctx->succeeded.store(true, std::memory_order_release);
      }
    } else if (ctx->action == Action::kNtCreate) {
      auto h2 = core::NtCreateFilePath(
          ctx->path, ctx->desired_access, FILE_ATTRIBUTE_NORMAL,
          ctx->share_mode, ctx->creation_disposition, ctx->nt_create_options);
      ctx->nt_status.store(h2.status, std::memory_order_release);
      ctx->succeeded.store(h2.value.has_value() && h2.value->valid(),
                           std::memory_order_release);
    } else if (ctx->action == Action::kMove) {
      ::SetLastError(ERROR_SUCCESS);
      if (::MoveFileExW(ctx->path.c_str(), ctx->target_path.c_str(), 0) !=
          FALSE) {
        ctx->succeeded.store(true, std::memory_order_release);
      } else {
        ctx->err.store(::GetLastError(), std::memory_order_release);
      }
    } else if (ctx->action == Action::kSetRename ||
               ctx->action == Action::kSetDisposition) {
      const core::NtResult<void> res =
          (ctx->action == Action::kSetRename)
              ? core::SetRenameInfo(ctx->op_handle.get(), ctx->target_path,
                                    nullptr, /*replace_if_exists=*/false)
              : core::SetDisposition(ctx->op_handle.get(), ctx->delete_file);
      ctx->nt_status.store(res.status, std::memory_order_release);
      ctx->op_handle.reset();
      if (nt::NtSuccess(res.status)) {
        ctx->succeeded.store(true, std::memory_order_release);
      } else {
        ctx->err.store(ERROR_GEN_FAILURE, std::memory_order_release);
      }
    } else {
      ::SetLastError(ERROR_SUCCESS);
      if (::DeleteFileW(ctx->path.c_str()) != FALSE) {
        ctx->succeeded.store(true, std::memory_order_release);
      } else {
        ctx->err.store(::GetLastError(), std::memory_order_release);
      }
    }
    ::SetEvent(ctx->done_event.get());
    return 0;
  }
};

::testing::AssertionResult StartPendingFsctl(HANDLE h, DWORD ioctl_code,
                                             void *in_buf, DWORD in_size,
                                             void *out_buf, DWORD out_size,
                                             OVERLAPPED *ov) {
  DWORD bytes_ret = 0;
  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok = ::DeviceIoControl(h, ioctl_code, in_buf, in_size, out_buf,
                                    out_size, &bytes_ret, ov);
  const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
  if (ok != FALSE) {
    return ::testing::AssertionFailure()
           << "Expected DeviceIoControl(0x" << std::hex << ioctl_code
           << ") to return ERROR_IO_PENDING, but it completed synchronously";
  }
  if (err != static_cast<DWORD>(ERROR_IO_PENDING)) {
    return ::testing::AssertionFailure()
           << "Expected ERROR_IO_PENDING from DeviceIoControl(0x" << std::hex
           << ioctl_code << "), got " << core::FormatWin32Error(err);
  }
  return ::testing::AssertionSuccess();
}

// Reusable RAII oplock test harness enforcing teardown order:
//   1. Cancel/drain pending overlapped IRPs (ack_guard, ov_guard)
//   2. Close file handle (unblocking any waiting peer worker thread)
//   3. Join worker thread (ScopedThread)
//   4. Destroy OVERLAPPED structs, output buffers, and event handles
// The peer worker never references harness or test stack storage: each worker
// run gets a fresh heap OplockWorkerContext co-owned by `worker`.
struct OplockHarness {
  core::UniqueHandle break_event;
  core::UniqueHandle ack_event;
  REQUEST_OPLOCK_OUTPUT_BUFFER out_buf{};
  REQUEST_OPLOCK_OUTPUT_BUFFER ack_out_buf{};
  OVERLAPPED ov{};
  OVERLAPPED ack_ov{};
  std::shared_ptr<OplockWorkerContext> worker_ctx;
  ScopedThread worker;
  core::UniqueHandle handle;
  PendingIoGuard ov_guard;
  PendingIoGuard ack_guard;

  ~OplockHarness() { CloseHandle(); }

  OplockHarness() = default;
  OplockHarness(const OplockHarness &) = delete;
  OplockHarness &operator=(const OplockHarness &) = delete;

  void CloseHandle() noexcept {
    ack_guard.CancelAndDrain();
    ov_guard.CancelAndDrain();
    handle.reset();
  }

  ::testing::AssertionResult Init() {
    for (core::UniqueHandle *ev : {&break_event, &ack_event}) {
      ::SetLastError(ERROR_SUCCESS);
      ev->reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
      const DWORD err = ::GetLastError();
      if (!ev->valid()) {
        return ::testing::AssertionFailure()
               << "CreateEventW failed: " << core::FormatWin32Error(err);
      }
    }
    ov = MakeOffsetOverlapped(0, break_event.get());
    ack_ov = MakeOffsetOverlapped(0, ack_event.get());
    return ::testing::AssertionSuccess();
  }

  // Manual-reset event signaled when the most recently started worker has
  // finished its action (nullptr before the first worker is started).
  HANDLE WorkerDoneEvent() const noexcept {
    return worker_ctx != nullptr ? worker_ctx->done_event.get() : nullptr;
  }

  // Replaces `worker_ctx` with a fresh heap context owning its own completion
  // event. A previous worker (if still running) keeps its own context alive
  // through `worker`, so contexts are never shared between runs.
  ::testing::AssertionResult NewWorkerContext() {
    auto ctx = std::make_shared<OplockWorkerContext>();
    ::SetLastError(ERROR_SUCCESS);
    ctx->done_event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const DWORD err = ::GetLastError();
    if (!ctx->done_event.valid()) {
      return ::testing::AssertionFailure()
             << "CreateEventW(worker done) failed: "
             << core::FormatWin32Error(err);
    }
    worker_ctx = std::move(ctx);
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult StartWorker() {
    if (!worker.Start(&OplockWorkerContext::Run, worker_ctx)) {
      return ::testing::AssertionFailure()
             << "Worker start failed: "
             << core::FormatWin32Error(worker.LastError());
    }
    return ::testing::AssertionSuccess();
  }

  // Fails if a previous oplock request issued through `ov` is still pending:
  // overwriting `ov` / `out_buf` under an in-flight IRP would corrupt it.
  ::testing::AssertionResult CheckNoPendingRequest(const char *caller) const {
    if (ov_guard.armed()) {
      return ::testing::AssertionFailure()
             << caller
             << " called while a previous oplock request is still pending; "
                "call WaitBreak() or CloseHandle() first";
    }
    return ::testing::AssertionSuccess();
  }

  // Fails if a previous acknowledgment issued through `ack_ov` is still
  // pending.
  ::testing::AssertionResult CheckNoPendingAck(const char *caller) const {
    if (ack_guard.armed()) {
      return ::testing::AssertionFailure()
             << caller
             << " called while a previous oplock acknowledgment is still "
                "pending; call WaitPendingAckBreak() or CloseHandle() first";
    }
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult
  Open(const std::wstring &path, DWORD access, DWORD share = core::kShareAll,
       DWORD disp = OPEN_EXISTING,
       DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED) {
    auto opened = core::CreateWin32File(path, access, share, disp, flags);
    if (!opened) {
      return ::testing::AssertionFailure()
             << "CreateWin32File failed: "
             << core::FormatWin32Error(opened.error);
    }
    handle = std::move(*opened.value);
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult RequestModern(DWORD level) {
    if (auto idle = CheckNoPendingRequest("RequestModern"); !idle) {
      return idle;
    }
    out_buf = REQUEST_OPLOCK_OUTPUT_BUFFER{};
    ::ResetEvent(break_event.get());
    ov = MakeOffsetOverlapped(0, break_event.get());
    REQUEST_OPLOCK_INPUT_BUFFER in_buf =
        MakeOplockRequest(level, REQUEST_OPLOCK_INPUT_FLAG_REQUEST);
    auto res =
        StartPendingFsctl(handle.get(), nt::kFsctlRequestOplock, &in_buf,
                          sizeof(in_buf), &out_buf, sizeof(out_buf), &ov);
    if (!res) {
      return res;
    }
    ov_guard.Arm(handle.get(), &ov);
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult RequestLegacy(DWORD ioctl_code) {
    if (auto idle = CheckNoPendingRequest("RequestLegacy"); !idle) {
      return idle;
    }
    ::ResetEvent(break_event.get());
    ov = MakeOffsetOverlapped(0, break_event.get());
    auto res = StartPendingFsctl(handle.get(), ioctl_code, nullptr, 0, nullptr,
                                 0, &ov);
    if (!res) {
      return res;
    }
    ov_guard.Arm(handle.get(), &ov);
    return ::testing::AssertionSuccess();
  }

  // Issues an oplock request FSCTL that the filesystem must reject
  // synchronously with `expected_error` (FSCTL_REQUEST_OPLOCK uses
  // `modern_level`; legacy FSCTLs pass no buffers). The request uses the
  // harness-owned OVERLAPPED and output buffer, never test stack storage: if
  // the filesystem wrongly grants the oplock (ERROR_IO_PENDING), the pending
  // IRP is cancelled and drained through ov_guard before this returns.
  ::testing::AssertionResult ExpectRequestRejected(DWORD ioctl_code,
                                                   DWORD expected_error,
                                                   DWORD modern_level = 0) {
    if (auto idle = CheckNoPendingRequest("ExpectRequestRejected"); !idle) {
      return idle;
    }
    const bool modern = (ioctl_code == nt::kFsctlRequestOplock);
    out_buf = REQUEST_OPLOCK_OUTPUT_BUFFER{};
    ::ResetEvent(break_event.get());
    ov = MakeOffsetOverlapped(0, break_event.get());
    REQUEST_OPLOCK_INPUT_BUFFER in_buf =
        MakeOplockRequest(modern_level, REQUEST_OPLOCK_INPUT_FLAG_REQUEST);
    void *in_ptr = modern ? &in_buf : nullptr;
    const DWORD in_size = modern ? static_cast<DWORD>(sizeof(in_buf)) : 0u;
    void *out_ptr = modern ? &out_buf : nullptr;
    const DWORD out_size = modern ? static_cast<DWORD>(sizeof(out_buf)) : 0u;
    DWORD bytes_ret = 0;
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok = ::DeviceIoControl(handle.get(), ioctl_code, in_ptr, in_size,
                                      out_ptr, out_size, &bytes_ret, &ov);
    const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
    if (ok != FALSE) {
      return ::testing::AssertionFailure()
             << "Expected oplock FSCTL 0x" << std::hex << ioctl_code
             << " to fail with " << core::FormatWin32Error(expected_error)
             << ", but it completed successfully";
    }
    if (err == static_cast<DWORD>(ERROR_IO_PENDING)) {
      ov_guard.Arm(handle.get(), &ov);
      ov_guard.CancelAndDrain();
      return ::testing::AssertionFailure()
             << "Expected oplock FSCTL 0x" << std::hex << ioctl_code
             << " to fail with " << core::FormatWin32Error(expected_error)
             << ", but the oplock was granted (request pended)";
    }
    if (err != expected_error) {
      return ::testing::AssertionFailure()
             << "Expected oplock FSCTL 0x" << std::hex << ioctl_code
             << " to fail with " << core::FormatWin32Error(expected_error)
             << ", got " << core::FormatWin32Error(err);
    }
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult StartOpenWorker(const std::wstring &path,
                                             DWORD access,
                                             DWORD disp = OPEN_EXISTING,
                                             DWORD share = core::kShareAll,
                                             bool read_contents = false) {
    if (auto fresh = NewWorkerContext(); !fresh) {
      return fresh;
    }
    worker_ctx->action = read_contents ? OplockWorkerContext::Action::kOpenRead
                                       : OplockWorkerContext::Action::kOpen;
    worker_ctx->path = path;
    worker_ctx->desired_access = access;
    worker_ctx->share_mode = share;
    worker_ctx->creation_disposition = disp;
    return StartWorker();
  }

  ::testing::AssertionResult
  RunNtCreateWorker(const std::wstring &nt_path, ACCESS_MASK access,
                    ULONG share, ULONG disp, ULONG create_options,
                    DWORD timeout_ms, NTSTATUS *status_out,
                    bool *handle_valid_out = nullptr) {
    if (auto fresh = NewWorkerContext(); !fresh) {
      return fresh;
    }
    worker_ctx->action = OplockWorkerContext::Action::kNtCreate;
    worker_ctx->path = nt_path;
    worker_ctx->desired_access = access;
    worker_ctx->share_mode = share;
    worker_ctx->creation_disposition = disp;
    worker_ctx->nt_create_options = create_options;
    if (auto started = StartWorker(); !started) {
      return started;
    }
    const DWORD wait_rc = ::WaitForSingleObject(WorkerDoneEvent(), timeout_ms);
    if (wait_rc != kWaitObject0) {
      return ::testing::AssertionFailure()
             << "NtCreateFile worker timed out (wait_rc=" << wait_rc << ")";
    }
    if (!worker.Join(timeout_ms)) {
      return ::testing::AssertionFailure()
             << "Worker join failed: "
             << core::FormatWin32Error(worker.LastError());
    }
    if (status_out != nullptr) {
      *status_out = worker_ctx->nt_status.load(std::memory_order_acquire);
    }
    if (handle_valid_out != nullptr) {
      *handle_valid_out = worker_ctx->succeeded.load(std::memory_order_acquire);
    }
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult
  StartPathActionWorker(OplockWorkerContext::Action action,
                        const std::wstring &path,
                        const std::wstring *target_path = nullptr) {
    if (auto fresh = NewWorkerContext(); !fresh) {
      return fresh;
    }
    worker_ctx->action = action;
    worker_ctx->path = path;
    if (target_path != nullptr) {
      worker_ctx->target_path = *target_path;
    }
    return StartWorker();
  }

  ::testing::AssertionResult
  StartSetRenameWorker(core::UniqueHandle op_handle,
                       const std::wstring &nt_target_path) {
    if (auto fresh = NewWorkerContext(); !fresh) {
      return fresh;
    }
    worker_ctx->action = OplockWorkerContext::Action::kSetRename;
    worker_ctx->op_handle = std::move(op_handle);
    worker_ctx->target_path = nt_target_path;
    return StartWorker();
  }

  ::testing::AssertionResult
  StartSetDispositionWorker(core::UniqueHandle op_handle, bool delete_file) {
    if (auto fresh = NewWorkerContext(); !fresh) {
      return fresh;
    }
    worker_ctx->action = OplockWorkerContext::Action::kSetDisposition;
    worker_ctx->op_handle = std::move(op_handle);
    worker_ctx->delete_file = delete_file;
    return StartWorker();
  }

  ::testing::AssertionResult WaitBreak(DWORD timeout_ms,
                                       DWORD *bytes_out = nullptr) {
    const DWORD wait_rc = ::WaitForSingleObject(break_event.get(), timeout_ms);
    if (wait_rc != kWaitObject0) {
      return ::testing::AssertionFailure()
             << "Timed out waiting for oplock break (wait_rc=" << wait_rc
             << ")";
    }
    DWORD bytes_ret = 0;
    ::SetLastError(ERROR_SUCCESS);
    if (::GetOverlappedResult(handle.get(), &ov, &bytes_ret, FALSE) == FALSE) {
      const DWORD err = ::GetLastError();
      return ::testing::AssertionFailure()
             << "GetOverlappedResult(break) failed: "
             << core::FormatWin32Error(err);
    }
    ov_guard.Disarm();
    if (bytes_out != nullptr) {
      *bytes_out = bytes_ret;
    }
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult SendModernAck(DWORD new_level) {
    if (auto idle = CheckNoPendingAck("SendModernAck"); !idle) {
      return idle;
    }
    ack_out_buf = REQUEST_OPLOCK_OUTPUT_BUFFER{};
    ::ResetEvent(ack_event.get());
    ack_ov = MakeOffsetOverlapped(0, ack_event.get());
    REQUEST_OPLOCK_INPUT_BUFFER ack_in =
        MakeOplockRequest(new_level, REQUEST_OPLOCK_INPUT_FLAG_ACK);
    DWORD ack_bytes = 0;
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok = ::DeviceIoControl(handle.get(), nt::kFsctlRequestOplock,
                                      &ack_in, sizeof(ack_in), &ack_out_buf,
                                      sizeof(ack_out_buf), &ack_bytes, &ack_ov);
    const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
    if (ok == FALSE) {
      if (err != static_cast<DWORD>(ERROR_IO_PENDING)) {
        return ::testing::AssertionFailure()
               << "FSCTL_REQUEST_OPLOCK(ACK) failed: "
               << core::FormatWin32Error(err);
      }
      ack_guard.Arm(handle.get(), &ack_ov);
    }
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult SendLegacyPendingAck(DWORD ioctl_code) {
    if (auto idle = CheckNoPendingAck("SendLegacyPendingAck"); !idle) {
      return idle;
    }
    ::ResetEvent(ack_event.get());
    ack_ov = MakeOffsetOverlapped(0, ack_event.get());
    auto res = StartPendingFsctl(handle.get(), ioctl_code, nullptr, 0, nullptr,
                                 0, &ack_ov);
    if (!res) {
      return res;
    }
    ack_guard.Arm(handle.get(), &ack_ov);
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult WaitPendingAckBreak(DWORD timeout_ms) {
    const DWORD wait_rc = ::WaitForSingleObject(ack_event.get(), timeout_ms);
    if (wait_rc != kWaitObject0) {
      return ::testing::AssertionFailure()
             << "Timed out waiting for downgraded oplock break (wait_rc="
             << wait_rc << ")";
    }
    DWORD bytes_ret = 0;
    ::SetLastError(ERROR_SUCCESS);
    if (::GetOverlappedResult(handle.get(), &ack_ov, &bytes_ret, FALSE) ==
        FALSE) {
      const DWORD err = ::GetLastError();
      return ::testing::AssertionFailure()
             << "GetOverlappedResult(ack_break) failed: "
             << core::FormatWin32Error(err);
    }
    ack_guard.Disarm();
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult SendLegacyImmediateAck(DWORD ioctl_code,
                                                    DWORD timeout_ms) {
    if (auto idle = CheckNoPendingAck("SendLegacyImmediateAck"); !idle) {
      return idle;
    }
    ::ResetEvent(ack_event.get());
    ack_ov = MakeOffsetOverlapped(0, ack_event.get());
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok = ::DeviceIoControl(handle.get(), ioctl_code, nullptr, 0,
                                      nullptr, 0, nullptr, &ack_ov);
    auto io_res = helpers::CompleteOverlappedIo(
        handle.get(), &ack_ov, ack_event.get(), ok, timeout_ms);
    if (!io_res) {
      return ::testing::AssertionFailure()
             << "Immediate ack FSCTL 0x" << std::hex << ioctl_code
             << " failed: " << core::FormatWin32Error(io_res.error);
    }
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult WaitWorkerClean(DWORD timeout_ms) {
    if (worker_ctx == nullptr) {
      return ::testing::AssertionFailure() << "No worker was started";
    }
    const DWORD wait_rc = ::WaitForSingleObject(WorkerDoneEvent(), timeout_ms);
    if (wait_rc != kWaitObject0) {
      return ::testing::AssertionFailure()
             << "Worker timed out waiting for completion (wait_rc=" << wait_rc
             << ")";
    }
    if (!worker.Join(timeout_ms)) {
      return ::testing::AssertionFailure()
             << "Worker join failed: "
             << core::FormatWin32Error(worker.LastError());
    }
    const DWORD err = worker_ctx->err.load(std::memory_order_acquire);
    if (err != ERROR_SUCCESS ||
        !worker_ctx->succeeded.load(std::memory_order_acquire)) {
      if (worker_ctx->action == OplockWorkerContext::Action::kSetRename ||
          worker_ctx->action == OplockWorkerContext::Action::kSetDisposition) {
        const NTSTATUS st =
            worker_ctx->nt_status.load(std::memory_order_acquire);
        return ::testing::AssertionFailure()
               << "Worker NT action failed: " << core::FormatNtStatus(st);
      }
      return ::testing::AssertionFailure()
             << "Worker action failed: " << core::FormatWin32Error(err);
    }
    return ::testing::AssertionSuccess();
  }
};

// Opens H1 with FILE_SHARE_DELETE and an RH oplock, verifies that opening H2
// with DELETE access does NOT break RH on create, then runs SetRenameInfo
// (when `nt_rename_target != nullptr`) or SetDisposition(TRUE) on H2 and
// verifies RH handle-cache break, ACK_REQUIRED, clean completion while H1 is
// still open, and teardown.
void VerifyRhBreakOnSetInfo(const std::wstring &path, DWORD timeout_ms,
                            const std::wstring *nt_rename_target) {
  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ, core::kShareAll));
  ASSERT_TRUE(harness.RequestModern(kOplockRh));

  auto h2 = core::CreateWin32File(path, DELETE | SYNCHRONIZE, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h2);
  EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout);

  if (nt_rename_target != nullptr) {
    ASSERT_TRUE(
        harness.StartSetRenameWorker(std::move(*h2.value), *nt_rename_target));
  } else {
    ASSERT_TRUE(harness.StartSetDispositionWorker(std::move(*h2.value),
                                                  /*delete_file=*/true));
  }

  DWORD bytes_ret = 0;
  ASSERT_TRUE(harness.WaitBreak(timeout_ms, &bytes_ret));
  EXPECT_EQ(bytes_ret,
            static_cast<DWORD>(sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRh);
  EXPECT_EQ(harness.out_buf.NewOplockLevel & OPLOCK_LEVEL_CACHE_HANDLE, 0u);
  EXPECT_NE(harness.out_buf.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED,
            0u);

  // Acknowledge without closing H1: with FILE_SHARE_DELETE granted, SetInfo
  // must complete cleanly once H1 drops handle caching.
  ASSERT_TRUE(harness.SendModernAck(harness.out_buf.NewOplockLevel));
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
  EXPECT_NT_STATUS(
      harness.worker_ctx->nt_status.load(std::memory_order_acquire),
      STATUS_SUCCESS);

  // Worker already closed H2; closing H1 completes any pending delete.
  harness.CloseHandle();
}

// Heap-allocated (std::shared_ptr) state shared by all stress workers; it owns
// the path so no worker ever dereferences the test's stack frame.
struct LockStressState {
  std::wstring path;
  size_t region_size = 0;
  int iterations = 0;
  DWORD timeout_ms = 0;
  ScopedCriticalSection cs;
  std::atomic<int> error_count{0};
  std::atomic<int> completed_iterations{0};
  std::vector<std::string> errors;

  void RecordError(int tid, int iter, const std::string &msg) {
    error_count.fetch_add(1, std::memory_order_relaxed);
    ScopedCsLock lock(cs);
    if (errors.size() < 16u) {
      errors.push_back("tid=" + std::to_string(tid) +
                       " iter=" + std::to_string(iter) + ": " + msg);
    }
  }
};

bool CompleteStressOverlappedOp(HANDLE h, HANDLE ev, OVERLAPPED *ov,
                                BOOL immediate_ok, DWORD timeout_ms,
                                DWORD *transferred_out, const char *op_name,
                                std::string *err_out) {
  auto res = helpers::CompleteOverlappedIo(h, ov, ev, immediate_ok, timeout_ms);
  if (!res) {
    *err_out =
        std::string(op_name) + " failed: " + core::FormatWin32Error(res.error);
    return false;
  }
  if (transferred_out != nullptr) {
    *transferred_out = *res.value;
  }
  return true;
}

bool TransferStressRegion(HANDLE h, HANDLE ev, uint8_t *buf, size_t len,
                          bool is_write, DWORD timeout_ms,
                          std::string *err_out) {
  const char *op_name = is_write ? "WriteFile" : "ReadFile";
  size_t total = 0;
  while (total < len) {
    if (::ResetEvent(ev) == FALSE) {
      const DWORD reset_err = ::GetLastError();
      *err_out = std::string("ResetEvent(") + op_name +
                 ") failed: " + core::FormatWin32Error(reset_err);
      return false;
    }
    OVERLAPPED ov = MakeOffsetOverlapped(static_cast<uint64_t>(total), ev);
    DWORD chunk = 0;
    const DWORD remaining = static_cast<DWORD>(len - total);
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok = is_write
                        ? ::WriteFile(h, buf + total, remaining, nullptr, &ov)
                        : ::ReadFile(h, buf + total, remaining, nullptr, &ov);
    if (!CompleteStressOverlappedOp(h, ev, &ov, ok, timeout_ms, &chunk, op_name,
                                    err_out)) {
      return false;
    }
    if (chunk == 0) {
      *err_out = std::string(op_name) + " transferred 0 bytes at offset " +
                 std::to_string(total);
      return false;
    }
    total += chunk;
  }
  return true;
}

struct LockStressWorkerContext {
  int tid = 0;
  std::shared_ptr<LockStressState> shared;

  static DWORD WINAPI Run(LPVOID param) {
    auto *ctx = static_cast<LockStressWorkerContext *>(param);
    const int worker_id = ctx->tid;
    LockStressState &state = *ctx->shared;
    const size_t region_size = state.region_size;

    auto h = OpenRw(state.path, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
    if (!h) {
      state.RecordError(worker_id, -1,
                        "CreateFileW failed: " +
                            core::FormatWin32Error(h.error));
      return 0;
    }

    ::SetLastError(ERROR_SUCCESS);
    core::UniqueHandle io_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const DWORD io_event_err = ::GetLastError();
    if (!io_event.valid()) {
      state.RecordError(worker_id, -1,
                        "CreateEventW failed: " +
                            core::FormatWin32Error(io_event_err));
      return 0;
    }

    std::vector<uint8_t> write_buf(region_size, 0);
    std::vector<uint8_t> read_buf(region_size, 0);

    for (int iter = 0; iter < state.iterations; ++iter) {
      if (::ResetEvent(io_event.get()) == FALSE) {
        const DWORD reset_err = ::GetLastError();
        state.RecordError(worker_id, iter,
                          "ResetEvent(lock) failed: " +
                              core::FormatWin32Error(reset_err));
        return 0;
      }

      OVERLAPPED lock_ov = MakeOffsetOverlapped(0, io_event.get());
      ::SetLastError(ERROR_SUCCESS);
      const BOOL lock_ok = ::LockFileEx(
          h.value->get(), LOCKFILE_EXCLUSIVE_LOCK, /*dwReserved=*/0,
          static_cast<DWORD>(region_size), 0, &lock_ov);
      std::string err_msg;
      if (!CompleteStressOverlappedOp(h.value->get(), io_event.get(), &lock_ov,
                                      lock_ok, state.timeout_ms, nullptr,
                                      "LockFileEx", &err_msg)) {
        state.RecordError(worker_id, iter, err_msg);
        return 0;
      }

      bool iter_ok = true;
      const uint8_t fill_byte =
          static_cast<uint8_t>((0x20 + worker_id + iter) & 0xFF);
      write_buf.assign(region_size, fill_byte);
      read_buf.assign(region_size, 0);

      if (!TransferStressRegion(
              h.value->get(), io_event.get(), write_buf.data(), region_size,
              /*is_write=*/true, state.timeout_ms, &err_msg)) {
        state.RecordError(worker_id, iter, err_msg);
        iter_ok = false;
      }

      if (iter_ok && !TransferStressRegion(h.value->get(), io_event.get(),
                                           read_buf.data(), region_size,
                                           /*is_write=*/false, state.timeout_ms,
                                           &err_msg)) {
        state.RecordError(worker_id, iter, err_msg);
        iter_ok = false;
      }

      if (iter_ok && !helpers::AllBytesEqual(read_buf, fill_byte)) {
        state.RecordError(worker_id, iter,
                          "Data interleaving mismatch in locked region");
        iter_ok = false;
      }

      if (::ResetEvent(io_event.get()) == FALSE) {
        const DWORD reset_err = ::GetLastError();
        state.RecordError(worker_id, iter,
                          "ResetEvent(unlock) failed: " +
                              core::FormatWin32Error(reset_err));
        return 0;
      }
      OVERLAPPED unlock_ov = MakeOffsetOverlapped(0, io_event.get());
      ::SetLastError(ERROR_SUCCESS);
      const BOOL unlock_ok =
          ::UnlockFileEx(h.value->get(), /*dwReserved=*/0,
                         static_cast<DWORD>(region_size), 0, &unlock_ov);
      if (!CompleteStressOverlappedOp(h.value->get(), io_event.get(),
                                      &unlock_ov, unlock_ok, state.timeout_ms,
                                      nullptr, "UnlockFileEx", &err_msg)) {
        state.RecordError(worker_id, iter, err_msg);
        return 0;
      }

      if (!iter_ok) {
        return 0;
      }
      state.completed_iterations.fetch_add(1, std::memory_order_relaxed);
    }

    return 0;
  }
};

// A suspended copy of the current executable, used purely as a second process
// that can hold a duplicated file handle. The child never executes a single
// instruction (CREATE_SUSPENDED, never resumed) and is terminated on scope
// exit, which also closes every handle duplicated into it.
class SuspendedSelfProcess {
public:
  SuspendedSelfProcess() noexcept = default;
  ~SuspendedSelfProcess() { Terminate(); }

  SuspendedSelfProcess(const SuspendedSelfProcess &) = delete;
  SuspendedSelfProcess &operator=(const SuspendedSelfProcess &) = delete;

  core::Win32Result<void> Start() {
    core::Win32Result<void> res{};
    auto self_exe = helpers::GetCurrentExecutablePath();
    if (!self_exe) {
      res.error = self_exe.error;
      return res;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    ::SetLastError(ERROR_SUCCESS);
    if (::CreateProcessW(self_exe.value->c_str(), nullptr, nullptr, nullptr,
                         /*bInheritHandles=*/FALSE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                         &si, &pi) == FALSE) {
      res.error = ::GetLastError();
      if (res.error == ERROR_SUCCESS) {
        res.error = ERROR_GEN_FAILURE;
      }
      return res;
    }
    process_.reset(pi.hProcess);
    thread_.reset(pi.hThread);
    res.error = ERROR_SUCCESS;
    return res;
  }

  HANDLE process() const noexcept { return process_.get(); }

  void Terminate() noexcept {
    if (process_.valid()) {
      ::TerminateProcess(process_.get(), 0);
      ::WaitForSingleObject(process_.get(), 5000);
    }
    thread_.reset();
    process_.reset();
  }

private:
  core::UniqueHandle process_;
  core::UniqueHandle thread_;
};

} // namespace

// Spec: [MS-FSA] 2.1.5.8 -- an exclusive byte-range lock makes overlapping
// exclusive and shared lock requests from another handle fail
// ERROR_LOCK_VIOLATION.
TEST_F(LocksOplocksTest, ExclusiveLockBlocksConflictingLocksFromOtherHandle) {
  const std::wstring path = TestPath(L"lock_exclusive_locks.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'A')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  // Conflicting exclusive and overlapping shared locks from H2 fail
  // ERROR_LOCK_VIOLATION.
  AssertLockConflict(h2.value->get(), 0, 100);
  AssertLockConflict(h2.value->get(), 50, 100, LOCKFILE_FAIL_IMMEDIATELY);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.4.10 / 2.1.5.2 -- an exclusive byte-range lock fails
// overlapping ReadFile from another handle with ERROR_LOCK_VIOLATION while
// non-overlapping reads succeed.
TEST_F(LocksOplocksTest, ExclusiveLockBlocksOverlappingReadFromOtherHandle) {
  const std::wstring path = TestPath(L"lock_exclusive_read.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'A')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  // Reads from H2 inside or straddling the locked range fail.
  EXPECT_WIN32_ERROR_CODE(ReadAt(h2.value->get(), 0, 50).error,
                          ERROR_LOCK_VIOLATION);
  EXPECT_WIN32_ERROR_CODE(ReadAt(h2.value->get(), 90, 20).error,
                          ERROR_LOCK_VIOLATION);

  // Non-overlapping read from H2 succeeds.
  auto rd_nonoverlap = ReadAt(h2.value->get(), 100, 16);
  ASSERT_WIN32_OK(rd_nonoverlap);
  EXPECT_EQ(*rd_nonoverlap.value, std::string(16, 'A'));

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.4.10 / 2.1.5.3 -- an exclusive byte-range lock fails
// overlapping WriteFile from another handle with ERROR_LOCK_VIOLATION and
// leaves the locked bytes unmodified.
TEST_F(LocksOplocksTest, ExclusiveLockBlocksOverlappingWriteFromOtherHandle) {
  const std::wstring path = TestPath(L"lock_exclusive_write.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'A')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  EXPECT_WIN32_ERROR_CODE(WriteAt(h2.value->get(), 0, "BLOCKED_WRITE").error,
                          ERROR_LOCK_VIOLATION);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  h1.value->reset();
  h2.value->reset();

  // The rejected write must not have reached the file.
  EXPECT_TRUE(helpers::CheckFileContents(path, std::string(256, 'A')));
}

// Spec: [MS-FSA] 2.1.4.10 / 2.1.5.8 -- the handle that owns an exclusive
// byte-range lock can still read and write inside the locked range.
TEST_F(LocksOplocksTest, ExclusiveLockOwnerCanReadAndWriteLockedRange) {
  const std::wstring path = TestPath(L"lock_exclusive_owner.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'A')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  auto rd_owner = ReadAt(h1.value->get(), 0, 16);
  ASSERT_WIN32_OK(rd_owner);
  EXPECT_EQ(*rd_owner.value, std::string(16, 'A'));

  const std::string owner_write = "LOCKED_BY_H1";
  auto wr_owner = WriteAt(h1.value->get(), 0, owner_write);
  ASSERT_WIN32_OK(wr_owner);
  EXPECT_EQ(*wr_owner.value, static_cast<DWORD>(owner_write.size()));

  auto rd_back =
      ReadAt(h1.value->get(), 0, static_cast<DWORD>(owner_write.size()));
  ASSERT_WIN32_OK(rd_back);
  EXPECT_EQ(*rd_back.value, owner_write);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.8 / 2.1.4.10 -- shared byte-range locks on the same
// range coexist across handles and still allow every holder to read it.
TEST_F(LocksOplocksTest, SharedLocksCoexistAcrossHandlesAndAllowReads) {
  const std::wstring path = TestPath(L"lock_shared_coexist.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'B')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100, LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 100, LOCKFILE_FAIL_IMMEDIATELY));

  for (HANDLE h : {h1.value->get(), h2.value->get()}) {
    auto rd = ReadAt(h, 0, 32);
    ASSERT_WIN32_OK(rd);
    EXPECT_EQ(*rd.value, std::string(32, 'B'));
  }

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.8 -- an exclusive lock request overlapping shared
// byte-range locks held by other handles fails ERROR_LOCK_VIOLATION.
TEST_F(LocksOplocksTest, SharedLocksBlockExclusiveLockFromOtherHandle) {
  const std::wstring path = TestPath(L"lock_shared_excl.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'B')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);
  auto h3 = OpenRw(path);
  ASSERT_WIN32_OK(h3);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100, LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 100, LOCKFILE_FAIL_IMMEDIATELY));

  AssertLockConflict(h3.value->get(), 0, 100);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.4.10 / 2.1.5.3 -- a shared byte-range lock fails WriteFile
// into the locked range with ERROR_LOCK_VIOLATION on every handle, including
// the handles that hold the shared lock.
TEST_F(LocksOplocksTest, SharedLocksBlockWritesOnAllHandles) {
  const std::wstring path = TestPath(L"lock_shared_write.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'B')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);
  auto h3 = OpenRw(path);
  ASSERT_WIN32_OK(h3);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100, LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 100, LOCKFILE_FAIL_IMMEDIATELY));

  for (HANDLE h : {h1.value->get(), h2.value->get(), h3.value->get()}) {
    EXPECT_WIN32_ERROR_CODE(WriteAt(h, 0, "NO_WRITE").error,
                            ERROR_LOCK_VIOLATION);
  }

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.8 / 2.1.5.9 -- shared byte-range locks stack per handle
// requiring matching unlocks, and same-handle shared-to-exclusive upgrade fails
// with ERROR_LOCK_VIOLATION.
TEST_F(LocksOplocksTest, SharedLockStackingAndSameHandleUpgradeConflict) {
  const std::wstring path = TestPath(L"lock_shared_stack.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'S')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  // Stack two identical shared locks on [0, 64) via H1.
  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 64, LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 64, LOCKFILE_FAIL_IMMEDIATELY));

  // Same-handle shared -> exclusive upgrade must conflict.
  AssertLockConflict(h1.value->get(), 0, 64);

  // First unlock drops refcount from 2 -> 1: H2 exclusive and H1 upgrade
  // remain blocked.
  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 64));
  AssertLockConflict(h2.value->get(), 0, 64);
  AssertLockConflict(h1.value->get(), 0, 64);

  // Second unlock drops refcount to 0: further unlock fails ERROR_NOT_LOCKED
  // and H2 can acquire the exclusive lock cleanly.
  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 64));
  EXPECT_WIN32_ERROR(UnlockRange(h1.value->get(), 0, 64), ERROR_NOT_LOCKED);

  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 64,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 64));
}

// Spec: [MS-FSA] 2.1.4.10 / 2.1.5.8 / 2.1.5.15.5 -- byte-range locks do not
// block memory-mapped I/O (MapViewOfFile) or SetEndOfFile from another handle.
TEST_F(LocksOplocksTest, ByteRangeLockDoesNotBlockSetEndOfFileOrMappedIo) {
  const std::wstring path = TestPath(L"lock_eof_mmap.dat");
  constexpr size_t kInitialSize = 8192u;
  ASSERT_WIN32_OK(
      core::WriteFileContents(path, std::string(kInitialSize, 'L')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 4096,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  // Sanity check: ordinary ReadFile from H2 inside [0, 4096) is blocked.
  EXPECT_WIN32_ERROR_CODE(ReadAt(h2.value->get(), 0, 16).error,
                          ERROR_LOCK_VIOLATION);

  // Memory-mapped read and write via H2 succeed despite H1's exclusive lock.
  const std::string mapped_patch = "MAPPED_PAST_LOCK";
  {
    ::SetLastError(ERROR_SUCCESS);
    core::UniqueHandle mapping(
        ::CreateFileMappingW(h2.value->get(), nullptr, PAGE_READWRITE, 0,
                             static_cast<DWORD>(kInitialSize), nullptr));
    const DWORD mapping_err = ::GetLastError();
    ASSERT_TRUE(mapping.valid()) << core::FormatWin32Error(mapping_err);
    ::SetLastError(ERROR_SUCCESS);
    core::UniqueViewOfFile view(::MapViewOfFile(
        mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, kInitialSize));
    const DWORD view_err = ::GetLastError();
    ASSERT_TRUE(view) << core::FormatWin32Error(view_err);

    auto *mapped = static_cast<char *>(view.get());
    EXPECT_TRUE(
        helpers::AllBytesEqual(std::string_view(mapped, kInitialSize), 'L'));
    std::memcpy(mapped, mapped_patch.data(), mapped_patch.size());
    ASSERT_WIN32_TRUE(::FlushViewOfFile(view.get(), kInitialSize));
  }

  // SetEndOfFile (both extend and truncate inside locked [0, 4096)) succeeds
  // from H2 while H1 still holds the exclusive lock.
  ASSERT_WIN32_TRUE(SeekBegin(h2.value->get(), 12288));
  ASSERT_WIN32_TRUE(::SetEndOfFile(h2.value->get()));
  LARGE_INTEGER sz{};
  ASSERT_WIN32_TRUE(::GetFileSizeEx(h2.value->get(), &sz));
  EXPECT_EQ(sz.QuadPart, 12288);

  ASSERT_WIN32_TRUE(SeekBegin(h2.value->get(), 2048));
  ASSERT_WIN32_TRUE(::SetEndOfFile(h2.value->get()));
  ASSERT_WIN32_TRUE(::GetFileSizeEx(h2.value->get(), &sz));
  EXPECT_EQ(sz.QuadPart, 2048);

  // H1 sees the mapped write written by H2.
  auto rd1 =
      ReadAt(h1.value->get(), 0, static_cast<DWORD>(mapped_patch.size()));
  ASSERT_WIN32_OK(rd1);
  EXPECT_EQ(*rd1.value, mapped_patch);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 4096));
}

// Spec: [MS-FSA] 2.1.5.8 Byte-Range Lock -- adjacent exclusive ranges coexist,
// boundary-straddling requests conflict, and locks beyond EOF do not grow EOF.
TEST_F(LocksOplocksTest, NonOverlappingRangesAndBeyondEofLock) {
  const std::wstring path = TestPath(L"lock_ranges.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'C')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 100, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  AssertLockConflict(h1.value->get(), 99, 2);
  AssertLockConflict(h2.value->get(), 99, 2);

  // Locking [1 MiB, 2 MiB) beyond EOF succeeds without changing EOF.
  constexpr uint64_t kOneMib = 1024ULL * 1024ULL;
  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), kOneMib, kOneMib,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  LARGE_INTEGER sz{};
  ASSERT_WIN32_TRUE(::GetFileSizeEx(h1.value->get(), &sz));
  EXPECT_EQ(sz.QuadPart, 256);

  AssertLockConflict(h2.value->get(), kOneMib, 1);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), kOneMib, kOneMib));
  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 100, 100));
}

// Spec: [MS-FSA] 2.1.5.5 / 2.1.5.9 -- UnlockFileEx explicitly releases a lock,
// and closing the owning handle automatically releases remaining locks.
TEST_F(LocksOplocksTest, UnlockAndAutoReleaseOnHandleClose) {
  const std::wstring path = TestPath(L"lock_release.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'R')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  AssertLockConflict(h2.value->get(), 0, 100);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));

  // Re-lock on H1 and close H1 without calling UnlockFileEx.
  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  h1.value->reset();

  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.9 Byte-Range Unlock -- unlocking an unlocked range or a
// range whose offset/length does not exactly match fails ERROR_NOT_LOCKED.
TEST_F(LocksOplocksTest, UnlockNonExistentOrMismatchedRangeFails) {
  const std::wstring path = TestPath(L"lock_mismatch.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'M')));

  auto h = OpenRw(path);
  ASSERT_WIN32_OK(h);

  EXPECT_WIN32_ERROR(UnlockRange(h.value->get(), 0, 100), ERROR_NOT_LOCKED);

  ASSERT_WIN32_TRUE(
      LockRange(h.value->get(), 10, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  EXPECT_WIN32_ERROR(UnlockRange(h.value->get(), 10, 99), ERROR_NOT_LOCKED);
  EXPECT_WIN32_ERROR(UnlockRange(h.value->get(), 10, 101), ERROR_NOT_LOCKED);
  EXPECT_WIN32_ERROR(UnlockRange(h.value->get(), 11, 100), ERROR_NOT_LOCKED);

  ASSERT_WIN32_TRUE(UnlockRange(h.value->get(), 10, 100));
  EXPECT_WIN32_ERROR(UnlockRange(h.value->get(), 10, 100), ERROR_NOT_LOCKED);
}

// Spec: [MS-FSA] 2.1.5.8 / Win32 LockFileEx -- omitting LOCKFILE_FAIL_IMMEDIATELY
// pends until the conflicting holder unlocks, then acquires the range cleanly.
TEST_F(LocksOplocksTest, BlockingLockUnblocksWhenUnlocked) {
  const std::wstring path = TestPath(L"lock_blocking.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'K')));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle lock_ev(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  const DWORD lock_ev_err = ::GetLastError();
  ASSERT_TRUE(lock_ev.valid()) << core::FormatWin32Error(lock_ev_err);
  OVERLAPPED lock_ov = MakeOffsetOverlapped(0, lock_ev.get());

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
  ASSERT_WIN32_OK(h2);
  PendingIoGuard lock_guard;

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  // Blocking overlapped LockFileEx synchronously returns ERROR_IO_PENDING once
  // queued in the filesystem, eliminating thread-scheduling races.
  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok = ::LockFileEx(h2.value->get(), LOCKFILE_EXCLUSIVE_LOCK,
                               /*dwReserved=*/0, 100, 0, &lock_ov);
  const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
  ASSERT_EQ(ok, FALSE);
  ASSERT_WIN32_ERROR_CODE(err, ERROR_IO_PENDING);
  lock_guard.Arm(h2.value->get(), &lock_ov);

  EXPECT_EQ(::WaitForSingleObject(lock_ev.get(), 100), kWaitTimeout);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_EQ(::WaitForSingleObject(lock_ev.get(), timeout_ms), kWaitObject0);

  DWORD ignored = 0;
  ASSERT_WIN32_TRUE(
      ::GetOverlappedResult(h2.value->get(), &lock_ov, &ignored, FALSE));
  lock_guard.Disarm();

  // Confirm H2 now owns the exclusive lock before unlocking.
  AssertLockConflict(h1.value->get(), 0, 100);
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.8 / Win32 LockFileEx / CancelIoEx -- CancelIoEx aborts a
// pending blocking LockFileEx IRP with ERROR_OPERATION_ABORTED.
TEST_F(LocksOplocksTest, CancelIoExAbortsPendingBlockingLock) {
  const std::wstring path = TestPath(L"lock_cancel_pending.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'X')));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle lock_ev(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  const DWORD lock_ev_err = ::GetLastError();
  ASSERT_TRUE(lock_ev.valid()) << core::FormatWin32Error(lock_ev_err);
  OVERLAPPED lock_ov = MakeOffsetOverlapped(0, lock_ev.get());

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED);
  ASSERT_WIN32_OK(h2);
  PendingIoGuard lock_guard;

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  ::SetLastError(ERROR_SUCCESS);
  const BOOL ok = ::LockFileEx(h2.value->get(), LOCKFILE_EXCLUSIVE_LOCK,
                               /*dwReserved=*/0, 100, 0, &lock_ov);
  const DWORD err = (ok != FALSE) ? ERROR_SUCCESS : ::GetLastError();
  ASSERT_EQ(ok, FALSE);
  ASSERT_WIN32_ERROR_CODE(err, ERROR_IO_PENDING);
  lock_guard.Arm(h2.value->get(), &lock_ov);

  ASSERT_WIN32_TRUE(::CancelIoEx(h2.value->get(), &lock_ov));
  ASSERT_EQ(::WaitForSingleObject(lock_ev.get(), timeout_ms), kWaitObject0);

  DWORD ignored = 0;
  EXPECT_WIN32_ERROR(
      ::GetOverlappedResult(h2.value->get(), &lock_ov, &ignored, FALSE),
      ERROR_OPERATION_ABORTED);
  lock_guard.Disarm();

  // H1 still holds the lock; once unlocked, H2 acquires and releases cleanly.
  AssertLockConflict(h2.value->get(), 0, 100);
  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.18 / 2.1.5.19 -- FSCTL_REQUEST_OPLOCK (RH) breaks to
// None on a conflicting write open and unblocks the opener on handle close.
TEST_F(LocksOplocksTest, OplockRequestReadAndBreakOnConflictingWriteOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_rh_break.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "initial_oplock_data"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));
  ASSERT_TRUE(harness.RequestModern(kOplockRh));

  // Conflicting write open (CREATE_ALWAYS / FILE_OVERWRITE_IF breaks RH -> 0).
  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_WRITE, CREATE_ALWAYS));

  DWORD bytes_ret = 0;
  ASSERT_TRUE(harness.WaitBreak(timeout_ms, &bytes_ret));
  EXPECT_EQ(bytes_ret,
            static_cast<DWORD>(sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRh);
  EXPECT_EQ(harness.out_buf.NewOplockLevel, 0u);
  // Breaking handle caching requires an acknowledgment ([MS-FSA] 2.1.4.12):
  // the overwriting opener stays blocked until H1 acknowledges or closes.
  EXPECT_NE(harness.out_buf.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED,
            0u);

  harness.CloseHandle();
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
}

// Spec: [MS-FSA] 2.1.5.18 -- a shared Read oplock (OPLOCK_LEVEL_CACHE_READ)
// does not break when a second handle opens and reads the file.
TEST_F(LocksOplocksTest, OplockReadSharedDoesNotBreakOnReadOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_read_shared.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "shared_read_data"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));
  ASSERT_TRUE(harness.RequestModern(OPLOCK_LEVEL_CACHE_READ));

  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_READ, OPEN_EXISTING,
                                      core::kShareAll, /*read_contents=*/true));
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
  EXPECT_EQ(harness.worker_ctx->read_data, "shared_read_data");

  // H1's read oplock must still be unbroken.
  EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout);
  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.4.10 / 2.1.5.8 -- a zero-length exclusive lock at offset
// 100 blocks strictly straddling reads while allowing boundary reads and exact
// (100, 0) unlock.
TEST_F(LocksOplocksTest, ZeroLengthByteRangeLockSemantics) {
  const std::wstring path = TestPath(L"lock_zero_len.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'Z')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 100, 0,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  // Per [MS-FSA] 2.1.4.10 (FsRtlCheckLockForReadAccess), a zero-length
  // exclusive lock at offset 100 blocks reads strictly straddling 100
  // (ReadStart < 100 < ReadEnd), while boundary-adjacent reads ending at 100
  // or starting at 100 succeed.
  auto rd_before = ReadAt(h2.value->get(), 0, 100);
  ASSERT_WIN32_OK(rd_before);
  EXPECT_EQ(rd_before.value->size(), 100u);

  auto rd_after = ReadAt(h2.value->get(), 100, 50);
  ASSERT_WIN32_OK(rd_after);
  EXPECT_EQ(rd_after.value->size(), 50u);

  EXPECT_WIN32_ERROR_CODE(ReadAt(h2.value->get(), 99, 2).error,
                          ERROR_LOCK_VIOLATION);
  EXPECT_WIN32_ERROR_CODE(ReadAt(h2.value->get(), 0, 200).error,
                          ERROR_LOCK_VIOLATION);

  EXPECT_WIN32_ERROR(UnlockRange(h1.value->get(), 101, 0), ERROR_NOT_LOCKED);
  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 100, 0));
  EXPECT_WIN32_ERROR(UnlockRange(h1.value->get(), 100, 0), ERROR_NOT_LOCKED);
}

// Spec: [MS-FSA] 2.1.5.8 / Win32 DuplicateHandle -- re-locking exclusively on
// the same handle fails, while duplicated handles share lock ownership until
// all duplicates close.
TEST_F(LocksOplocksTest,
       SameHandleExclusiveLockConflictAndDuplicateHandleOwnership) {
  const std::wstring path = TestPath(L"lock_dup_owner.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'D')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h3 = OpenRw(path);
  ASSERT_WIN32_OK(h3);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  AssertLockConflict(h1.value->get(), 0, 100);
  AssertLockConflict(h1.value->get(), 50, 100);

  // DuplicateHandle shares the same kernel FILE_OBJECT and lock ownership.
  core::UniqueHandle h1_dup;
  ASSERT_WIN32_TRUE(::DuplicateHandle(::GetCurrentProcess(), h1.value->get(),
                                      ::GetCurrentProcess(), h1_dup.put(), 0,
                                      FALSE, DUPLICATE_SAME_ACCESS));

  auto rd_dup = ReadAt(h1_dup.get(), 0, 16);
  ASSERT_WIN32_OK(rd_dup);
  EXPECT_EQ(*rd_dup.value, std::string(16, 'D'));

  const std::string dup_payload = "DUP_WRITE_OK____";
  auto wr_dup = WriteAt(h1_dup.get(), 0, dup_payload);
  ASSERT_WIN32_OK(wr_dup);
  EXPECT_EQ(*wr_dup.value, 16u);

  // Closing original H1 while h1_dup stays open must keep the lock active.
  h1.value->reset();

  AssertLockConflict(h3.value->get(), 0, 100);
  EXPECT_WIN32_ERROR_CODE(ReadAt(h3.value->get(), 0, 16).error,
                          ERROR_LOCK_VIOLATION);

  // Closing the duplicate releases the lock so independent H3 succeeds.
  h1_dup.reset();

  ASSERT_WIN32_TRUE(
      LockRange(h3.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  auto rd_h3 = ReadAt(h3.value->get(), 0, 16);
  ASSERT_WIN32_OK(rd_h3);
  EXPECT_EQ(*rd_h3.value, dup_payload);
  ASSERT_WIN32_TRUE(UnlockRange(h3.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.9 Byte-Range Unlock -- unlocking a partial sub-range of
// a locked extent fails ERROR_NOT_LOCKED and leaves the full lock intact.
TEST_F(LocksOplocksTest, UnlockPartialSubRangeFailsNotLocked) {
  const std::wstring path = TestPath(L"lock_partial_unlock.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'P')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  EXPECT_WIN32_ERROR(UnlockRange(h1.value->get(), 0, 50), ERROR_NOT_LOCKED);
  EXPECT_WIN32_ERROR(UnlockRange(h1.value->get(), 50, 50), ERROR_NOT_LOCKED);

  AssertLockConflict(h2.value->get(), 0, 100);

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 100));
  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.18 / 2.1.5.19 -- RWH oplock downgrades (dropping WRITE,
// retaining READ) on a conflicting read open and unblocks the peer upon ACK.
TEST_F(LocksOplocksTest, OplockRwhDowngradeToReadOnConflictingReadOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_rwh_downgrade.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "rwh_downgrade_payload"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ | GENERIC_WRITE));
  ASSERT_TRUE(harness.RequestModern(kOplockRwh));

  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_READ, OPEN_EXISTING));

  DWORD bytes_ret = 0;
  ASSERT_TRUE(harness.WaitBreak(timeout_ms, &bytes_ret));
  EXPECT_EQ(bytes_ret,
            static_cast<DWORD>(sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRwh);
  // On NTFS, RWH with no sharing conflict breaks to RH (R|H), stripping WRITE
  // and retaining READ caching.
  EXPECT_NE(harness.out_buf.NewOplockLevel & OPLOCK_LEVEL_CACHE_READ, 0u);
  EXPECT_EQ(harness.out_buf.NewOplockLevel & OPLOCK_LEVEL_CACHE_WRITE, 0u);
  EXPECT_NE(harness.out_buf.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED,
            0u);

  // Acknowledge the break while keeping H1 open; this must unblock H2.
  ASSERT_TRUE(harness.SendModernAck(harness.out_buf.NewOplockLevel));
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.5.1.2 -- NtCreateFile with FILE_COMPLETE_IF_OPLOCKED
// returns STATUS_OPLOCK_BREAK_IN_PROGRESS immediately without waiting for ACK.
TEST_F(LocksOplocksTest, OplockCompleteIfOplockedFlag) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_cio.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "complete_if_oplocked_data"));
  const std::wstring nt_path = TestNtPath(L"oplock_cio.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ | GENERIC_WRITE));
  ASSERT_TRUE(harness.RequestModern(kOplockRwh));

  // H2 must complete immediately WITHOUT H1 acknowledging or closing yet.
  NTSTATUS cio_status = STATUS_UNSUCCESSFUL;
  bool cio_valid = false;
  ASSERT_TRUE(harness.RunNtCreateWorker(
      nt_path, FILE_READ_DATA | SYNCHRONIZE, core::kShareAll, nt::kFileOpen,
      nt::kFileNonDirectoryFile | nt::kFileCompleteIfOplocked, timeout_ms,
      &cio_status, &cio_valid));

  EXPECT_NT_STATUS(cio_status, nt::kStatusOplockBreakInProgress);
  EXPECT_TRUE(cio_valid);

  DWORD bytes_ret = 0;
  ASSERT_TRUE(harness.WaitBreak(timeout_ms, &bytes_ret));
  EXPECT_EQ(bytes_ret,
            static_cast<DWORD>(sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRwh);
  // A non-conflicting read open strips WRITE caching, keeps READ caching, and
  // requires H1 to acknowledge the break (the FILE_COMPLETE_IF_OPLOCKED opener
  // merely does not wait for that acknowledgment).
  EXPECT_NE(harness.out_buf.NewOplockLevel & OPLOCK_LEVEL_CACHE_READ, 0u);
  EXPECT_EQ(harness.out_buf.NewOplockLevel & OPLOCK_LEVEL_CACHE_WRITE, 0u);
  EXPECT_NE(harness.out_buf.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED,
            0u);

  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.5.18 / 2.1.5.19 -- a directory RH oplock breaks to None
// when a child file is created inside the directory.
TEST_F(LocksOplocksTest, DirectoryOplockBreaksOnChildCreate) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring dir_path = TestPath(L"oplock_dir");
  const std::wstring child_path = TestPath(L"oplock_dir\\child.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_path.c_str(), nullptr));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(dir_path, FILE_LIST_DIRECTORY, core::kShareAll,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED));
  ASSERT_TRUE(harness.RequestModern(kOplockRh));

  ASSERT_TRUE(harness.StartOpenWorker(child_path, GENERIC_WRITE, CREATE_NEW));

  DWORD bytes_ret = 0;
  ASSERT_TRUE(harness.WaitBreak(timeout_ms, &bytes_ret));
  EXPECT_EQ(bytes_ret,
            static_cast<DWORD>(sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRh);
  EXPECT_EQ(harness.out_buf.NewOplockLevel, 0u);

  harness.CloseHandle();
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
}

// Spec: [MS-FSA] 2.1.5.1.2.2 / 2.1.5.18 -- while an asynchronous
// FILE_OPEN_REQUIRING_OPLOCK open exists, a conflicting
// FILE_OPEN_REQUIRING_OPLOCK open fails STATUS_CANNOT_BREAK_OPLOCK both before
// and after FSCTL_REQUEST_OPLOCK (without breaking H1), while a normal
// conflicting open breaks H1's oplock cleanly.
TEST_F(LocksOplocksTest, OpenRequiringOplockAtomicGrant) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_open_requiring.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "atomic_oplock_data"));
  const std::wstring nt_path = TestNtPath(L"oplock_open_requiring.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());

  // Open H1 with FILE_OPEN_REQUIRING_OPLOCK (asynchronous).
  auto h1 = core::NtCreateFilePath(
      nt_path, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOpen,
      nt::kFileNonDirectoryFile | nt::kFileOpenRequiringOplock);
  ASSERT_NT_SUCCESS(h1.status);
  ASSERT_TRUE(h1.value.has_value());
  harness.handle = std::move(*h1.value);

  // Contract 1: While H1 holds unacknowledged FILE_OPEN_REQUIRING_OPLOCK
  // (BEFORE FSCTL_REQUEST_OPLOCK), a conflicting open with
  // FILE_OPEN_REQUIRING_OPLOCK fails STATUS_CANNOT_BREAK_OPLOCK (0xC0000909).
  {
    NTSTATUS pre_status = STATUS_UNSUCCESSFUL;
    bool pre_valid = false;
    ASSERT_TRUE(harness.RunNtCreateWorker(
        nt_path, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
        /*share=*/0, nt::kFileOpen,
        nt::kFileNonDirectoryFile | nt::kFileOpenRequiringOplock, timeout_ms,
        &pre_status, &pre_valid));
    EXPECT_FALSE(pre_valid);
    EXPECT_NT_STATUS(pre_status, nt::kStatusCannotBreakOplock);
  }

  // Acquire RH oplock on H1 via FSCTL_REQUEST_OPLOCK.
  ASSERT_TRUE(harness.RequestModern(kOplockRh));

  // Contract 2: Conflicting FILE_OPEN_REQUIRING_OPLOCK open fails
  // STATUS_CANNOT_BREAK_OPLOCK WITHOUT breaking H1's RH oplock.
  {
    NTSTATUS post_status = STATUS_UNSUCCESSFUL;
    bool post_valid = false;
    ASSERT_TRUE(harness.RunNtCreateWorker(
        nt_path, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
        /*share=*/0, nt::kFileOpen,
        nt::kFileNonDirectoryFile | nt::kFileOpenRequiringOplock, timeout_ms,
        &post_status, &post_valid));
    EXPECT_FALSE(post_valid);
    EXPECT_NT_STATUS(post_status, nt::kStatusCannotBreakOplock);
    EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0),
              kWaitTimeout);
  }

  // A normal conflicting open (without FILE_OPEN_REQUIRING_OPLOCK) DOES break
  // H1 cleanly.
  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_WRITE, CREATE_ALWAYS));

  DWORD bytes_ret = 0;
  ASSERT_TRUE(harness.WaitBreak(timeout_ms, &bytes_ret));
  EXPECT_EQ(bytes_ret,
            static_cast<DWORD>(sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRh);
  EXPECT_EQ(harness.out_buf.NewOplockLevel, 0u);

  harness.CloseHandle();
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
}

// Spec: WDK NtCreateFile / CreateFile2 FILE_FLAG_OPEN_REQUIRING_OPLOCK -- NTFS
// accepts FILE_OPEN_REQUIRING_OPLOCK together with synchronous create options
// (FILE_SYNCHRONOUS_IO_NONALERT), and the handle may be closed without ever
// requesting the oplock, leaving no oplock state behind.
TEST_F(LocksOplocksTest, OpenRequiringOplockAcceptsSynchronousCreateOptions) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_open_requiring_sync.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "sync_atomic_oplock_data"));
  const std::wstring nt_path = TestNtPath(L"oplock_open_requiring_sync.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());

  auto h_sync = core::NtCreateFilePath(
      nt_path, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOpen,
      nt::kFileNonDirectoryFile | nt::kFileOpenRequiringOplock |
          nt::kFileSynchronousIoNonalert);
  ASSERT_NT_STATUS(h_sync.status, STATUS_SUCCESS);
  ASSERT_TRUE(h_sync.value.has_value());

  // No other operation is issued through the handle: the documented contract
  // only allows requesting the oplock or closing the handle at this point.
  h_sync.value->reset();

  // Nothing lingers after the close: a conflicting exclusive open completes
  // promptly (run on a worker so a stuck open cannot hang the test).
  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_READ | GENERIC_WRITE,
                                      OPEN_EXISTING, /*share=*/0));
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
}

// Spec: [MS-FSA] 2.1.5.18 / 2.1.5.19 -- acknowledging a legacy Level 1 oplock
// break with FSCTL_OPLOCK_BREAK_ACK_NO_2 releases the opener and drops the
// oplock directly to None without granting Level 2.
TEST_F(LocksOplocksTest, OplockBreakAckNo2TransitionsToNone) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_ack_no2.dat");
  const std::string payload = "ack_no2_payload";
  const std::string updated = "ack_no2_written";
  ASSERT_WIN32_OK(core::WriteFileContents(path, payload));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ | GENERIC_WRITE));
  ASSERT_TRUE(harness.RequestLegacy(nt::kFsctlRequestOplockLevel1));

  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_READ, OPEN_EXISTING));
  ASSERT_TRUE(harness.WaitBreak(timeout_ms));

  // FSCTL_OPLOCK_BREAK_ACK_NO_2 transitions directly to None and completes
  // immediately without pending a Level 2 oplock.
  ASSERT_TRUE(
      harness.SendLegacyImmediateAck(nt::kFsctlOplockBreakAckNo2, timeout_ms));
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));

  // Verify H1 holds no oplock (not Level 2): a concurrent write on H3 succeeds
  // cleanly while H1 remains open.
  {
    auto h3 = OpenRw(path);
    ASSERT_WIN32_OK(h3);
    auto wr3 = WriteAt(h3.value->get(), 0, updated);
    ASSERT_WIN32_OK(wr3);
    EXPECT_EQ(*wr3.value, static_cast<DWORD>(updated.size()));
    auto rd3 = ReadAt(h3.value->get(), 0, static_cast<DWORD>(updated.size()));
    ASSERT_WIN32_OK(rd3);
    EXPECT_EQ(*rd3.value, updated);
  }
  harness.CloseHandle();

  EXPECT_TRUE(helpers::CheckFileContents(path, updated));
}

// Spec: [MS-FSA] 2.1.5.18 / 2.1.5.19 -- acknowledging a Batch oplock break with
// FSCTL_OPBATCH_ACK_CLOSE_PENDING holds the conflicting opener until the
// oplock handle closes, then releases the opener cleanly.
TEST_F(LocksOplocksTest, OplockOpbatchAckClosePendingReleasesOpener) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_batch_close_pending.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "batch_close_pending_payload"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ | GENERIC_WRITE));
  ASSERT_TRUE(harness.RequestLegacy(nt::kFsctlRequestBatchOplock));

  ASSERT_TRUE(harness.StartOpenWorker(path, DELETE | GENERIC_WRITE,
                                      OPEN_EXISTING, /*share=*/0));
  ASSERT_TRUE(harness.WaitBreak(timeout_ms));

  ASSERT_TRUE(harness.SendLegacyImmediateAck(nt::kFsctlOpbatchAckClosePending,
                                             timeout_ms));

  // Opener is already inside CreateFileW (proven by the break received above)
  // and must stay blocked until H1 actually closes.
  EXPECT_EQ(::WaitForSingleObject(harness.WorkerDoneEvent(), 100),
            kWaitTimeout);

  // Closing H1 releases the close-pending wait and lets H2 succeed without
  // hitting ERROR_SHARING_VIOLATION.
  harness.CloseHandle();
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
}

// Spec: [MS-FSA] 2.1.5.18 / [MS-FSCC] 2.3 -- requesting an exclusive Level 1 or
// RWH oplock while two handles are open fails ERROR_OPLOCK_NOT_GRANTED.
TEST_F(LocksOplocksTest, ExclusiveOplockRejectedWithMultipleHandles) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_two_handles.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "two_open_handles"));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ | GENERIC_WRITE));

  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);

  // Legacy Level 1 must be rejected with ERROR_OPLOCK_NOT_GRANTED.
  EXPECT_TRUE(harness.ExpectRequestRejected(
      nt::kFsctlRequestOplockLevel1,
      static_cast<DWORD>(ERROR_OPLOCK_NOT_GRANTED)));

  // Modern RWH must also be rejected with ERROR_OPLOCK_NOT_GRANTED.
  EXPECT_TRUE(harness.ExpectRequestRejected(
      nt::kFsctlRequestOplock, static_cast<DWORD>(ERROR_OPLOCK_NOT_GRANTED),
      kOplockRwh));

  // Once H2 closes, H1 is the sole handle and RWH succeeds cleanly.
  h2.value->reset();
  ASSERT_TRUE(harness.RequestModern(kOplockRwh));
  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.5.18 / 2.1.5.19 -- acknowledging a Level 1 oplock break
// with FSCTL_OPLOCK_BREAK_ACKNOWLEDGE downgrades to Level 2 (pending), which
// then breaks to None upon a peer WriteFile.
TEST_F(LocksOplocksTest, Level1DowngradeToLevel2BrokenByWrite) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_l1_to_l2.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "level1_to_level2_payload"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ | GENERIC_WRITE));
  ASSERT_TRUE(harness.RequestLegacy(nt::kFsctlRequestOplockLevel1));

  // Read open breaks Level 1 -> Level 2.
  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_READ, OPEN_EXISTING));
  ASSERT_TRUE(harness.WaitBreak(timeout_ms));

  // Acknowledging with FSCTL_OPLOCK_BREAK_ACKNOWLEDGE returns ERROR_IO_PENDING
  // representing the newly granted Level 2 oplock and unblocks the reader.
  ASSERT_TRUE(harness.SendLegacyPendingAck(nt::kFsctlOplockBreakAcknowledge));
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));

  // Opening a peer RW handle does not break Level 2; WriteFile breaks it.
  auto h3 = OpenRw(path);
  ASSERT_WIN32_OK(h3);
  EXPECT_EQ(::WaitForSingleObject(harness.ack_event.get(), 0), kWaitTimeout);

  auto wr = WriteAt(h3.value->get(), 0, "BREAK_LEVEL2!");
  ASSERT_WIN32_OK(wr);

  ASSERT_TRUE(harness.WaitPendingAckBreak(timeout_ms));
  h3.value->reset();
  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.5.18 / 2.1.5.19 / [MS-FSCC] 2.3 --
// FSCTL_REQUEST_FILTER_OPLOCK on an asynchronous read handle (opened without
// FILE_RESERVE_OPFILTER) grants a filter oplock that breaks cleanly when
// another handle opens for write.
TEST_F(LocksOplocksTest, FilterOplockGrantedAndBrokenByWriteOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_filter.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "filter_oplock_payload"));
  const std::wstring nt_path = TestNtPath(L"oplock_filter.dat");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());

  // Open asynchronously without FILE_RESERVE_OPFILTER (which rejects opens if
  // any background AV/filter scanner holds a transient handle) and request a
  // filter oplock directly via FSCTL_REQUEST_FILTER_OPLOCK.
  auto h1 = core::NtCreateFilePath(
      nt_path, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOpen,
      nt::kFileNonDirectoryFile);
  ASSERT_NT_SUCCESS(h1.status);
  ASSERT_TRUE(h1.value.has_value());
  harness.handle = std::move(*h1.value);

  ASSERT_TRUE(harness.RequestLegacy(nt::kFsctlRequestFilterOplock));

  ASSERT_TRUE(harness.StartOpenWorker(path, GENERIC_WRITE, OPEN_EXISTING));
  ASSERT_TRUE(harness.WaitBreak(timeout_ms));

  ASSERT_TRUE(
      harness.SendLegacyImmediateAck(nt::kFsctlOplockBreakAckNo2, timeout_ms));
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.5.3 / 2.1.4.12 -- a peer WriteFile breaks a shared Read
// (R) oplock to None.
TEST_F(LocksOplocksTest, OplockReadBrokenByPeerWrite) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring path = TestPath(L"oplock_break_write.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "initial_write_payload"));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));
  ASSERT_TRUE(harness.RequestModern(OPLOCK_LEVEL_CACHE_READ));

  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);
  ASSERT_WIN32_OK(WriteAt(h2.value->get(), 0, "PEER_WRITE"));

  ASSERT_TRUE(harness.WaitBreak(timeout_ms));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel,
            static_cast<DWORD>(OPLOCK_LEVEL_CACHE_READ));
  EXPECT_EQ(harness.out_buf.NewOplockLevel, 0u);
}

// Spec: [MS-FSA] 2.1.5.15.5 / 2.1.4.12 -- a peer SetEndOfFile breaks a shared
// Read (R) oplock to None.
TEST_F(LocksOplocksTest, OplockReadBrokenByPeerSetEndOfFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring path = TestPath(L"oplock_break_trunc.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "initial_truncate_payload"));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));
  ASSERT_TRUE(harness.RequestModern(OPLOCK_LEVEL_CACHE_READ));

  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);
  ASSERT_WIN32_TRUE(SeekBegin(h2.value->get(), 4));
  ASSERT_WIN32_TRUE(::SetEndOfFile(h2.value->get()));

  ASSERT_TRUE(harness.WaitBreak(timeout_ms));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel,
            static_cast<DWORD>(OPLOCK_LEVEL_CACHE_READ));
  EXPECT_EQ(harness.out_buf.NewOplockLevel, 0u);
}

// Spec: [MS-FSA] 2.1.5.15.11 / 2.1.4.12 -- MoveFileExW breaks an RH oplock's
// handle caching (H1 omits FILE_SHARE_DELETE), so once H1 closes in response
// the rename succeeds instead of failing ERROR_SHARING_VIOLATION.
TEST_F(LocksOplocksTest, OplockRhBrokenByMoveFileEx) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring src = TestPath(L"oplock_break_move_src.dat");
  const std::wstring dst = TestPath(L"oplock_break_move_dst.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(src, "move_oplock_payload"));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(
      harness.Open(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE));
  ASSERT_TRUE(harness.RequestModern(kOplockRh));

  ASSERT_TRUE(harness.StartPathActionWorker(OplockWorkerContext::Action::kMove,
                                            src, &dst));
  ASSERT_TRUE(harness.WaitBreak(timeout_ms));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRh);
  EXPECT_EQ(harness.out_buf.NewOplockLevel & OPLOCK_LEVEL_CACHE_HANDLE, 0u);

  harness.CloseHandle();
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "move_oplock_payload"));
}

// Spec: [MS-FSA] 2.1.5.1.2 / 2.1.4.12 -- DeleteFileW breaks an RH oplock's
// handle caching (H1 omits FILE_SHARE_DELETE), so once H1 closes in response
// the delete succeeds instead of failing ERROR_SHARING_VIOLATION.
TEST_F(LocksOplocksTest, OplockRhBrokenByDeleteFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring path = TestPath(L"oplock_break_delete.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "delete_oplock_payload"));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(
      harness.Open(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE));
  ASSERT_TRUE(harness.RequestModern(kOplockRh));

  ASSERT_TRUE(harness.StartPathActionWorker(
      OplockWorkerContext::Action::kDelete, path));
  ASSERT_TRUE(harness.WaitBreak(timeout_ms));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel, kOplockRh);
  EXPECT_EQ(harness.out_buf.NewOplockLevel & OPLOCK_LEVEL_CACHE_HANDLE, 0u);

  harness.CloseHandle();
  ASSERT_TRUE(harness.WaitWorkerClean(timeout_ms));
  EXPECT_TRUE(helpers::CheckFileNotFound(path));
}

// Spec: [MS-FSA] 2.1.5.15.12 / 2.1.5.15.3 / 2.1.4.12 -- even when H1 holds an
// RH oplock with FILE_SHARE_DELETE (so H2's DELETE open succeeds without
// breaking the oplock on create), IRP_MJ_SET_INFORMATION(FileRenameInformation)
// and IRP_MJ_SET_INFORMATION(FileDispositionInformation) must break H1's
// handle-caching oplock and wait for acknowledgment before completing.
TEST_F(LocksOplocksTest, OplockRhBrokenByRenameAndDeleteWithShareDelete) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  // Sub-case 1: FileRenameInformation breaks RH when H1 has FILE_SHARE_DELETE.
  {
    SCOPED_TRACE("FileRenameInformation");
    const std::wstring src = TestPath(L"oplock_rh_share_del_ren_src.dat");
    const std::wstring dst = TestPath(L"oplock_rh_share_del_ren_dst.dat");
    const std::wstring nt_dst = TestNtPath(L"oplock_rh_share_del_ren_dst.dat");
    ASSERT_WIN32_OK(core::WriteFileContents(src, "rh_share_del_rename"));

    ASSERT_NO_FATAL_FAILURE(VerifyRhBreakOnSetInfo(src, timeout_ms, &nt_dst));
    EXPECT_TRUE(helpers::CheckFileNotFound(src));
    EXPECT_TRUE(helpers::CheckFileContents(dst, "rh_share_del_rename"));
  }

  // Sub-case 2: FileDispositionInformation(TRUE) breaks RH when H1 has
  // FILE_SHARE_DELETE.
  {
    SCOPED_TRACE("FileDispositionInformation");
    const std::wstring path = TestPath(L"oplock_rh_share_del_disp.dat");
    ASSERT_WIN32_OK(core::WriteFileContents(path, "rh_share_del_delete"));

    ASSERT_NO_FATAL_FAILURE(VerifyRhBreakOnSetInfo(path, timeout_ms, nullptr));
    EXPECT_TRUE(helpers::CheckFileNotFound(path));
  }
}

// Spec: [MS-FSA] 2.1.5.8 / 2.1.4.12 -- a byte-range lock starting at offset
// 4096 inside an 8 KiB file breaks a shared Read (R) oplock to None even when
// no handle has queried file size since reopen.
TEST_F(LocksOplocksTest, ByteRangeLockAbove4KiBBreaksReadOplock) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring path = TestPath(L"oplock_break_lock_above_4k.dat");
  // Write 8 KiB and close all handles so the next open starts from a fresh
  // FCB/stream state without a prior size query.
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(8192, 'O')));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));
  ASSERT_TRUE(harness.RequestModern(OPLOCK_LEVEL_CACHE_READ));

  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);
  EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout);

  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 4096, 4096,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  ASSERT_TRUE(harness.WaitBreak(timeout_ms));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel,
            static_cast<DWORD>(OPLOCK_LEVEL_CACHE_READ));
  EXPECT_EQ(harness.out_buf.NewOplockLevel, 0u);

  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 4096, 4096));
}

// Spec: [MS-FSA] 2.1.5.18 / [MS-FSCC] 2.3 -- requesting a legacy Level 2 or
// modern Read (R) oplock while another handle holds a byte-range lock on the
// file fails ERROR_OPLOCK_NOT_GRANTED; once unlocked, acquiring Read succeeds.
TEST_F(LocksOplocksTest, OplockRequestRejectedWhileByteRangeLockHeld) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const std::wstring path = TestPath(L"oplock_rejected_while_locked.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(8192, 'L')));

  auto h1 = OpenRw(path);
  ASSERT_WIN32_OK(h1);
  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 4096,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));

  EXPECT_TRUE(harness.ExpectRequestRejected(
      nt::kFsctlRequestOplockLevel2,
      static_cast<DWORD>(ERROR_OPLOCK_NOT_GRANTED)));
  EXPECT_TRUE(harness.ExpectRequestRejected(
      nt::kFsctlRequestOplock, static_cast<DWORD>(ERROR_OPLOCK_NOT_GRANTED),
      OPLOCK_LEVEL_CACHE_READ));

  ASSERT_WIN32_TRUE(UnlockRange(h1.value->get(), 0, 4096));
  ASSERT_TRUE(harness.RequestModern(OPLOCK_LEVEL_CACHE_READ));
  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.5.8 / 2.1.4.12 -- a byte-range lock taken through another
// open breaks a shared Read (R) oplock to None.
TEST_F(LocksOplocksTest, ByteRangeLockBreaksReadOplock) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring path = TestPath(L"oplock_break_lock_r.dat");
  // Only locks that start inside the file's allocation interfere with oplocks,
  // so use a non-resident file and lock its first page.
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(8192, 'O')));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));
  ASSERT_TRUE(harness.RequestModern(OPLOCK_LEVEL_CACHE_READ));

  // Opening a peer read/write handle alone does not break the R oplock.
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);
  EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout);

  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 4096,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  ASSERT_TRUE(harness.WaitBreak(timeout_ms));
  EXPECT_EQ(harness.out_buf.OriginalOplockLevel,
            static_cast<DWORD>(OPLOCK_LEVEL_CACHE_READ));
  EXPECT_EQ(harness.out_buf.NewOplockLevel, 0u);

  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 4096));
}

// Spec: [MS-FSA] 2.1.5.8 / 2.1.4.12 -- a byte-range lock breaks a legacy
// Level 2 oplock (FSCTL_REQUEST_OPLOCK_LEVEL_2) to None.
TEST_F(LocksOplocksTest, ByteRangeLockBreaksLevel2Oplock) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring path = TestPath(L"oplock_break_lock_l2.dat");
  // Only locks that start inside the file's allocation interfere with oplocks,
  // so use a non-resident file and lock its first page.
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(8192, 'O')));

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ));
  ASSERT_TRUE(harness.RequestLegacy(nt::kFsctlRequestOplockLevel2));

  // Opening a peer read/write handle alone does not break Level 2.
  auto h2 = OpenRw(path);
  ASSERT_WIN32_OK(h2);
  EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout);

  ASSERT_WIN32_TRUE(
      LockRange(h2.value->get(), 0, 4096,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  // The pending Level 2 request completes: the oplock is broken to None.
  ASSERT_TRUE(harness.WaitBreak(timeout_ms));

  ASSERT_WIN32_TRUE(UnlockRange(h2.value->get(), 0, 4096));
}

// Spec: [MS-FSA] 2.1.4.12 -- operations issued through the open that owns an
// exclusive (RWH) oplock carry that open's own oplock key and therefore never
// break it: read, write, and byte-range lock via the oplocked handle and via a
// DuplicateHandle copy of it (same open) leave the oplock intact.
//
// A second, independent open that shares the oplock key requires the
// kernel-only OPLOCK_KEY_ECP_CONTEXT extra create parameter (IoCreateFileEx),
// which user-mode CreateFileW / NtCreateFile cannot supply, so that variant is
// not testable here; the same-open operations below are the user-mode-visible
// part of the same-key rule.
TEST_F(LocksOplocksTest, OplockNotBrokenBySameOpenIoAndLock) {
  REQUIRE_FS_CAPABILITY(FsCapability::kOplocks);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  constexpr size_t kSize = 8192;
  const std::wstring path = TestPath(L"oplock_same_key.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(kSize, 'K')));

  ::SetLastError(ERROR_SUCCESS);
  core::UniqueHandle io_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  const DWORD io_event_err = ::GetLastError();
  ASSERT_TRUE(io_event.valid()) << core::FormatWin32Error(io_event_err);

  OplockHarness harness;
  ASSERT_TRUE(harness.Init());
  ASSERT_TRUE(harness.Open(path, GENERIC_READ | GENERIC_WRITE));
  ASSERT_TRUE(harness.RequestModern(kOplockRwh));

  // A duplicated handle refers to the same open (same FILE_OBJECT / key).
  core::UniqueHandle dup;
  ASSERT_WIN32_TRUE(::DuplicateHandle(
      ::GetCurrentProcess(), harness.handle.get(), ::GetCurrentProcess(),
      dup.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));

  for (HANDLE h : {harness.handle.get(), dup.get()}) {
    std::string err_msg;
    std::vector<uint8_t> write_buf(kSize, static_cast<uint8_t>('W'));
    ASSERT_TRUE(TransferStressRegion(h, io_event.get(), write_buf.data(),
                                     write_buf.size(), /*is_write=*/true,
                                     timeout_ms, &err_msg))
        << err_msg;
    EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout)
        << "write through the oplock owner's open broke its RWH oplock";

    std::vector<uint8_t> read_buf(kSize, 0);
    ASSERT_TRUE(TransferStressRegion(h, io_event.get(), read_buf.data(),
                                     read_buf.size(), /*is_write=*/false,
                                     timeout_ms, &err_msg))
        << err_msg;
    EXPECT_TRUE(helpers::AllBytesEqual(read_buf, static_cast<uint8_t>('W')));
    EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout)
        << "read through the oplock owner's open broke its RWH oplock";

    ASSERT_WIN32_TRUE(::ResetEvent(io_event.get()));
    OVERLAPPED lock_ov = MakeOffsetOverlapped(0, io_event.get());
    ::SetLastError(ERROR_SUCCESS);
    const BOOL lock_ok =
        ::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                     /*dwReserved=*/0, 4096, 0, &lock_ov);
    ASSERT_TRUE(CompleteStressOverlappedOp(h, io_event.get(), &lock_ov, lock_ok,
                                           timeout_ms, nullptr, "LockFileEx",
                                           &err_msg))
        << err_msg;
    EXPECT_EQ(::WaitForSingleObject(harness.break_event.get(), 0), kWaitTimeout)
        << "byte-range lock through the oplock owner's open broke its RWH "
           "oplock";

    ASSERT_WIN32_TRUE(::ResetEvent(io_event.get()));
    OVERLAPPED unlock_ov = MakeOffsetOverlapped(0, io_event.get());
    ::SetLastError(ERROR_SUCCESS);
    const BOOL unlock_ok =
        ::UnlockFileEx(h, /*dwReserved=*/0, 4096, 0, &unlock_ov);
    ASSERT_TRUE(CompleteStressOverlappedOp(h, io_event.get(), &unlock_ov,
                                           unlock_ok, timeout_ms, nullptr,
                                           "UnlockFileEx", &err_msg))
        << err_msg;
  }

  dup.reset();
  harness.CloseHandle();
}

// Spec: [MS-FSA] 2.1.5.8 / WDK IRP_MJ_LOCK_CONTROL (IRP_MN_UNLOCK_ALL) --
// byte-range locks are owned by the (open, process) pair. When the locking
// process closes its last handle to an open that another process still
// references through a duplicated handle, the locker's locks are released even
// though the open itself stays alive (no cleanup has happened yet).
//
// Cross-process approach: the second process is a suspended copy of the
// current executable that never runs; the test duplicates the locked handle
// into it with DuplicateHandle. No helper executable or self re-exec protocol
// exists in the suite, so I/O *issued by* the second process through the
// duplicate (e.g. being blocked by the first process's lock) is not exercised;
// everything asserted here is observed from the test process.
TEST_F(LocksOplocksTest, LockReleasedWhenOwnerProcessClosesDuplicatedHandle) {
  const std::wstring path = TestPath(L"lock_xproc_owner.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(256, 'X')));

  // H1 omits FILE_SHARE_DELETE so that the survival of its open (through the
  // duplicate held by the other process) is observable as a sharing violation.
  auto h1 = core::CreateWin32File(path, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(h1);
  auto h3 = OpenRw(path);
  ASSERT_WIN32_OK(h3);

  ASSERT_WIN32_TRUE(
      LockRange(h1.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));

  SuspendedSelfProcess child;
  auto started = child.Start();
  ASSERT_WIN32_OK(started) << "Failed to create the suspended helper process";

  // The duplicate lives in (and is owned by) the child process; it is closed
  // when `child` terminates the process.
  HANDLE remote_dup = nullptr;
  ASSERT_WIN32_TRUE(::DuplicateHandle(::GetCurrentProcess(), h1.value->get(),
                                      child.process(), &remote_dup, 0, FALSE,
                                      DUPLICATE_SAME_ACCESS));
  ASSERT_NE(remote_dup, nullptr);

  // While this process still holds H1, its exclusive lock is in force.
  AssertLockConflict(h3.value->get(), 0, 100);

  // Close this process's only handle to the locked open. The open survives in
  // the child (so there is no cleanup), yet this process's locks are released.
  h1.value->reset();

  // The open is still alive: its share mode (no FILE_SHARE_DELETE) still
  // blocks DeleteFileW.
  EXPECT_WIN32_ERROR(::DeleteFileW(path.c_str()), ERROR_SHARING_VIOLATION);

  // The lock is gone: an independent handle acquires the same range and reads
  // it.
  ASSERT_WIN32_TRUE(
      LockRange(h3.value->get(), 0, 100,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY));
  auto rd = ReadAt(h3.value->get(), 0, 16);
  ASSERT_WIN32_OK(rd);
  EXPECT_EQ(*rd.value, std::string(16, 'X'));
  ASSERT_WIN32_TRUE(UnlockRange(h3.value->get(), 0, 100));
}

// Spec: [MS-FSA] 2.1.5.8 / 2.1.5.9 -- concurrent overlapped exclusive locking,
// explicit-offset write/read, and unlock preserve strict mutual exclusion.
TEST_F(LocksOplocksTest, ConcurrentLockUnlockAndReadWriteStress) {
  const std::wstring path = TestPath(L"lock_rw_stress.dat");
  constexpr size_t kFileSize = 256;
  constexpr size_t kRegionSize = 64;
  constexpr int kNumThreads = 4;
  constexpr int kIterations = 25;

  ASSERT_WIN32_OK(core::WriteFileContents(path, std::string(kFileSize, '\0')));

  // Heap-allocated shared state and per-worker contexts are co-owned by the
  // worker threads (ScopedThread), so a worker that fails to exit within the
  // join timeout can never read or write this test's stack frame.
  auto shared = std::make_shared<LockStressState>();
  shared->path = path;
  shared->region_size = kRegionSize;
  shared->iterations = kIterations;
  shared->timeout_ms = Config().timing.async_timeout_ms;

  std::array<ScopedThread, kNumThreads> workers;
  for (int t = 0; t < kNumThreads; ++t) {
    auto ctx = std::make_shared<LockStressWorkerContext>();
    ctx->tid = t;
    ctx->shared = shared;
    ASSERT_TRUE(workers[static_cast<size_t>(t)].Start(
        &LockStressWorkerContext::Run, ctx))
        << core::FormatWin32Error(workers[static_cast<size_t>(t)].LastError());
  }

  // Explicitly join every worker before inspecting any shared result. Scale
  // the overall join budget beyond the single-op async_timeout_ms because all
  // 4 workers serialize 25 contended lock+write+read+unlock cycles on [0, 64).
  const DWORD join_timeout_ms =
      (std::max)(DWORD{60000},
                 static_cast<DWORD>(Config().timing.async_timeout_ms) * 12u);
  bool all_joined = true;
  for (int t = 0; t < kNumThreads; ++t) {
    ScopedThread &worker = workers[static_cast<size_t>(t)];
    if (!worker.Join(join_timeout_ms)) {
      all_joined = false;
      ADD_FAILURE() << "Worker " << t << " did not exit: "
                    << core::FormatWin32Error(worker.LastError());
    }
  }
  ASSERT_TRUE(all_joined)
      << "Not all stress workers exited; results are not inspected";

  std::string combined_errors;
  for (const auto &err : shared->errors) {
    if (!combined_errors.empty()) {
      combined_errors += "\n  ";
    }
    combined_errors += err;
  }
  EXPECT_EQ(shared->error_count.load(std::memory_order_acquire), 0)
      << "Worker errors:\n  " << combined_errors;
  EXPECT_TRUE(shared->errors.empty())
      << "Worker errors:\n  " << combined_errors;
  EXPECT_EQ(shared->completed_iterations.load(std::memory_order_acquire),
            kNumThreads * kIterations);

  auto final_data = core::ReadFileContents(path);
  ASSERT_WIN32_OK(final_data);
  ASSERT_EQ(final_data.value->size(), kFileSize);
  EXPECT_TRUE(helpers::AllBytesEqual(
      std::string_view(*final_data.value).substr(kRegionSize), '\0'));
}

void ForceLink_LocksOplocks() {}

} // namespace fstest
