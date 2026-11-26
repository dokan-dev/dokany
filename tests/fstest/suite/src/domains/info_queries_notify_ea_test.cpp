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
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {
namespace {

// Alternate-data-stream notify filters and actions. <winnt.h> only defines the
// file/directory values (the stream ones live in the WDK's <ntifs.h>); the
// values below are those of [MS-FSCC] FILE_NOTIFY_INFORMATION and the
// CompletionFilter of change-notify requests.
constexpr DWORD kFileNotifyChangeStreamName = 0x00000200UL;
constexpr DWORD kFileNotifyChangeStreamSize = 0x00000400UL;
constexpr DWORD kFileNotifyChangeStreamWrite = 0x00000800UL;
constexpr DWORD kFileActionAddedStream = 0x00000006UL;
constexpr DWORD kFileActionModifiedStream = 0x00000008UL;

using FnReadDirectoryChangesExW = BOOL(WINAPI *)(
    HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength, BOOL bWatchSubtree,
    DWORD dwNotifyFilter, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
    ULONG read_directory_notify_information_class);

struct NotifyRecord {
  DWORD action = 0;
  std::wstring filename;
  int64_t file_id = 0;
  ULONG file_attributes = 0;
  bool has_extended_info = false;
};

// Overlapped ReadDirectoryChangesW watcher bound to one directory handle.
//
// The kernel latches the notify filter and the bWatchSubtree flag of the
// FIRST request issued on a directory handle and keeps queueing matching
// events on that handle between requests. Tests must therefore use one watcher
// (one handle) per filter, and a fresh watcher whenever events queued by an
// earlier phase must not be observable.
class AsyncDirWatcher {
public:
  AsyncDirWatcher() = default;
  ~AsyncDirWatcher() { CancelIfPending(); }

  AsyncDirWatcher(const AsyncDirWatcher &) = delete;
  AsyncDirWatcher &operator=(const AsyncDirWatcher &) = delete;

  void EnableExtendedInfo(FnReadDirectoryChangesExW fn) noexcept {
    read_dir_changes_ex_ = fn;
  }

  DWORD Open(std::wstring_view dir_path, size_t buffer_size = 8192) {
    CancelIfPending();
    auto h = core::CreateWin32File(
        dir_path, FILE_LIST_DIRECTORY, core::kShareAll, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED);
    if (!h) {
      return h.error;
    }
    core::UniqueHandle ev(::CreateEventW(nullptr, /*bManualReset=*/TRUE,
                                         /*bInitialState=*/FALSE, nullptr));
    if (!ev) {
      return ::GetLastError();
    }
    // 8-byte aligned storage satisfies both FILE_NOTIFY_INFORMATION and
    // nt::FileNotifyExtendedInfo.
    const size_t qwords =
        (buffer_size + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    auto io = std::make_unique<IoState>();
    io->buf.assign(qwords, 0ULL);
    dir_handle_ = std::move(*h.value);
    event_ = std::move(ev);
    buf_bytes_ = buffer_size;
    io_ = std::move(io);
    return ERROR_SUCCESS;
  }

  DWORD Arm(DWORD notify_filter, bool watch_subtree) {
    CancelIfPending();
    if (!dir_handle_ || !io_) {
      return ERROR_INVALID_HANDLE;
    }
    if (::ResetEvent(event_.get()) == FALSE) {
      return ::GetLastError();
    }
    std::memset(&io_->ov, 0, sizeof(io_->ov));
    io_->ov.hEvent = event_.get();
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok =
        (read_dir_changes_ex_ != nullptr)
            ? read_dir_changes_ex_(dir_handle_.get(), io_->buf.data(),
                                   static_cast<DWORD>(buf_bytes_),
                                   watch_subtree ? TRUE : FALSE, notify_filter,
                                   nullptr, &io_->ov, nullptr,
                                   nt::kReadDirectoryNotifyExtendedInformation)
            : ::ReadDirectoryChangesW(
                  dir_handle_.get(), io_->buf.data(),
                  static_cast<DWORD>(buf_bytes_), watch_subtree ? TRUE : FALSE,
                  notify_filter, nullptr, &io_->ov, nullptr);
    if (ok == FALSE) {
      const DWORD err = ::GetLastError();
      return (err != ERROR_SUCCESS) ? err
                                    : static_cast<DWORD>(ERROR_GEN_FAILURE);
    }
    pending_ = true;
    return ERROR_SUCCESS;
  }

  DWORD Wait(DWORD timeout_ms, std::vector<NotifyRecord> *out,
             DWORD *bytes_out = nullptr, NTSTATUS *nt_status_out = nullptr) {
    if (!pending_) {
      return ERROR_INVALID_PARAMETER;
    }
    ::SetLastError(ERROR_SUCCESS);
    const DWORD wait_rc = ::WaitForSingleObject(event_.get(), timeout_ms);
    if (wait_rc != static_cast<DWORD>(WAIT_OBJECT_0)) {
      const DWORD wait_err = (wait_rc == static_cast<DWORD>(WAIT_TIMEOUT))
                                 ? static_cast<DWORD>(ERROR_TIMEOUT)
                                 : ::GetLastError();
      CancelIfPending();
      return (wait_err != ERROR_SUCCESS)
                 ? wait_err
                 : static_cast<DWORD>(ERROR_GEN_FAILURE);
    }
    DWORD transferred = 0;
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok =
        ::GetOverlappedResult(dir_handle_.get(), &io_->ov, &transferred,
                              /*bWait=*/FALSE);
    const DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
    pending_ = false;
    if (bytes_out != nullptr) {
      *bytes_out = transferred;
    }
    if (nt_status_out != nullptr) {
      *nt_status_out = static_cast<NTSTATUS>(io_->ov.Internal);
    }
    if (ok == FALSE) {
      return (err != ERROR_SUCCESS) ? err
                                    : static_cast<DWORD>(ERROR_GEN_FAILURE);
    }
    if (static_cast<size_t>(transferred) > buf_bytes_) {
      ADD_FAILURE() << "ReadDirectoryChanges reported transferred="
                    << transferred << " exceeding buffer size " << buf_bytes_;
      return ERROR_INVALID_DATA;
    }
    if (out != nullptr && transferred > 0) {
      const auto *raw = reinterpret_cast<const uint8_t *>(io_->buf.data());
      if (read_dir_changes_ex_ != nullptr) {
        ParseExtendedNotifyBuffer(raw, transferred, buf_bytes_, out);
      } else {
        ParseStandardNotifyBuffer(raw, transferred, buf_bytes_, out);
      }
    }
    return ERROR_SUCCESS;
  }

  core::Win32Result<DWORD> CancelAndAwaitResult() noexcept {
    core::Win32Result<DWORD> res{};
    if (!pending_ || !dir_handle_) {
      res.error = ERROR_INVALID_PARAMETER;
      return res;
    }
    ::SetLastError(ERROR_SUCCESS);
    if (::CancelIoEx(dir_handle_.get(), &io_->ov) == FALSE) {
      const DWORD err = ::GetLastError();
      res.error =
          (err != ERROR_SUCCESS) ? err : static_cast<DWORD>(ERROR_GEN_FAILURE);
      return res;
    }
    DWORD transferred = 0;
    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok =
        ::GetOverlappedResult(dir_handle_.get(), &io_->ov, &transferred,
                              /*bWait=*/TRUE);
    const DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
    pending_ = false;
    if (ok != FALSE) {
      res.error = ERROR_SUCCESS;
      res.value = transferred;
    } else {
      res.error =
          (err != ERROR_SUCCESS) ? err : static_cast<DWORD>(ERROR_GEN_FAILURE);
    }
    return res;
  }

  // Closes the directory handle while a request is pending and waits for the
  // filesystem to complete that request. On ERROR_SUCCESS, `*status_out` and
  // `*bytes_out` receive the raw completion NTSTATUS and byte count stored in
  // the OVERLAPPED (GetOverlappedResult cannot be used: the handle is closed,
  // and it reports success-severity statuses such as STATUS_NOTIFY_CLEANUP as
  // plain success).
  //
  // If the request does not complete in time it can no longer be cancelled
  // (the handle is gone), so the OVERLAPPED and notify buffer are deliberately
  // leaked instead of being freed under a request the kernel may still
  // complete.
  DWORD CloseHandleAndAwaitCompletion(DWORD timeout_ms, NTSTATUS *status_out,
                                      ULONG_PTR *bytes_out) noexcept {
    if (!pending_ || !dir_handle_ || !io_ || status_out == nullptr ||
        bytes_out == nullptr) {
      return ERROR_INVALID_PARAMETER;
    }
    dir_handle_.reset();
    pending_ = false;
    ::SetLastError(ERROR_SUCCESS);
    const DWORD wait_rc = ::WaitForSingleObject(event_.get(), timeout_ms);
    if (wait_rc != static_cast<DWORD>(WAIT_OBJECT_0)) {
      const DWORD wait_err = (wait_rc == static_cast<DWORD>(WAIT_TIMEOUT))
                                 ? static_cast<DWORD>(ERROR_TIMEOUT)
                                 : ::GetLastError();
      (void)io_.release();
      return (wait_err != ERROR_SUCCESS)
                 ? wait_err
                 : static_cast<DWORD>(ERROR_GEN_FAILURE);
    }
    *status_out = static_cast<NTSTATUS>(io_->ov.Internal);
    *bytes_out = io_->ov.InternalHigh;
    return ERROR_SUCCESS;
  }

  void CancelIfPending() noexcept {
    if (pending_ && dir_handle_) {
      ::CancelIoEx(dir_handle_.get(), &io_->ov);
      DWORD ignored = 0;
      ::GetOverlappedResult(dir_handle_.get(), &io_->ov, &ignored,
                            /*bWait=*/TRUE);
      pending_ = false;
    }
  }

private:
  static void ParseStandardNotifyBuffer(const uint8_t *data, DWORD bytes,
                                        size_t buf_len,
                                        std::vector<NotifyRecord> *out) {
    EXPECT_LE(static_cast<size_t>(bytes), buf_len);
    if (static_cast<size_t>(bytes) > buf_len) {
      return;
    }
    constexpr size_t kHeaderBytes = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    size_t offset = 0;
    const size_t total = static_cast<size_t>(bytes);
    while (offset + kHeaderBytes <= total) {
      const uint8_t *base = data + offset;
      DWORD next_entry_offset = 0;
      DWORD action = 0;
      DWORD file_name_length = 0;
      std::memcpy(&next_entry_offset,
                  base + offsetof(FILE_NOTIFY_INFORMATION, NextEntryOffset),
                  sizeof(next_entry_offset));
      std::memcpy(&action, base + offsetof(FILE_NOTIFY_INFORMATION, Action),
                  sizeof(action));
      std::memcpy(&file_name_length,
                  base + offsetof(FILE_NOTIFY_INFORMATION, FileNameLength),
                  sizeof(file_name_length));
      const size_t name_start = offset + kHeaderBytes;
      const size_t max_bytes = total - name_start;
      const size_t name_bytes = static_cast<size_t>(file_name_length);
      EXPECT_EQ(name_bytes % sizeof(wchar_t), size_t{0});
      EXPECT_LE(name_bytes, max_bytes);
      if (name_bytes > max_bytes) {
        break;
      }
      NotifyRecord rec{};
      rec.action = action;
      const size_t char_count = name_bytes / sizeof(wchar_t);
      rec.filename.assign(char_count, L'\0');
      if (char_count > 0) {
        std::memcpy(rec.filename.data(), base + kHeaderBytes,
                    char_count * sizeof(wchar_t));
      }
      out->push_back(std::move(rec));
      if (next_entry_offset == 0) {
        break;
      }
      if ((next_entry_offset % 4u) != 0u) {
        ADD_FAILURE() << "FILE_NOTIFY_INFORMATION.NextEntryOffset "
                      << next_entry_offset << " is not 4-byte aligned";
        break;
      }
      if (static_cast<size_t>(next_entry_offset) < kHeaderBytes + name_bytes ||
          static_cast<size_t>(next_entry_offset) > total - offset) {
        ADD_FAILURE() << "FILE_NOTIFY_INFORMATION.NextEntryOffset "
                      << next_entry_offset << " out of bounds for entry size "
                      << (kHeaderBytes + name_bytes) << " and remaining bytes "
                      << (total - offset);
        break;
      }
      offset += next_entry_offset;
    }
  }

  static void ParseExtendedNotifyBuffer(const uint8_t *data, DWORD bytes,
                                        size_t buf_len,
                                        std::vector<NotifyRecord> *out) {
    EXPECT_LE(static_cast<size_t>(bytes), buf_len);
    if (static_cast<size_t>(bytes) > buf_len) {
      return;
    }
    constexpr size_t kHeaderBytes =
        offsetof(nt::FileNotifyExtendedInfo, file_name);
    size_t offset = 0;
    const size_t total = static_cast<size_t>(bytes);
    while (offset + kHeaderBytes <= total) {
      // FILE_NOTIFY_EXTENDED_INFORMATION entries are 4-byte aligned while
      // LARGE_INTEGER fields have 8-byte C++ alignment; load via std::memcpy.
      const uint8_t *base = data + offset;
      ULONG next_entry_offset = 0;
      ULONG action = 0;
      LARGE_INTEGER file_id{};
      ULONG file_attributes = 0;
      ULONG file_name_length = 0;
      std::memcpy(&next_entry_offset,
                  base +
                      offsetof(nt::FileNotifyExtendedInfo, next_entry_offset),
                  sizeof(next_entry_offset));
      std::memcpy(&action, base + offsetof(nt::FileNotifyExtendedInfo, action),
                  sizeof(action));
      std::memcpy(&file_id,
                  base + offsetof(nt::FileNotifyExtendedInfo, file_id),
                  sizeof(file_id));
      std::memcpy(&file_attributes,
                  base + offsetof(nt::FileNotifyExtendedInfo, file_attributes),
                  sizeof(file_attributes));
      std::memcpy(&file_name_length,
                  base + offsetof(nt::FileNotifyExtendedInfo, file_name_length),
                  sizeof(file_name_length));
      const size_t name_start = offset + kHeaderBytes;
      const size_t max_bytes = total - name_start;
      const size_t name_bytes = static_cast<size_t>(file_name_length);
      EXPECT_EQ(name_bytes % sizeof(wchar_t), size_t{0});
      EXPECT_LE(name_bytes, max_bytes);
      if (name_bytes > max_bytes) {
        break;
      }
      NotifyRecord rec{};
      rec.action = action;
      const size_t char_count = name_bytes / sizeof(wchar_t);
      rec.filename.assign(char_count, L'\0');
      if (char_count > 0) {
        std::memcpy(rec.filename.data(), base + kHeaderBytes,
                    char_count * sizeof(wchar_t));
      }
      rec.file_id = file_id.QuadPart;
      rec.file_attributes = file_attributes;
      rec.has_extended_info = true;
      out->push_back(std::move(rec));
      if (next_entry_offset == 0) {
        break;
      }
      if ((next_entry_offset % 4u) != 0u) {
        ADD_FAILURE() << "FileNotifyExtendedInfo.next_entry_offset "
                      << next_entry_offset << " is not 4-byte aligned";
        break;
      }
      if (static_cast<size_t>(next_entry_offset) < kHeaderBytes + name_bytes ||
          static_cast<size_t>(next_entry_offset) > total - offset) {
        ADD_FAILURE() << "FileNotifyExtendedInfo.next_entry_offset "
                      << next_entry_offset << " out of bounds for entry size "
                      << (kHeaderBytes + name_bytes) << " and remaining bytes "
                      << (total - offset);
        break;
      }
      offset += next_entry_offset;
    }
  }

  // Memory the kernel writes to while a request is in flight. Heap-allocated
  // so CloseHandleAndAwaitCompletion() can leak it when a request can neither
  // be cancelled nor observed to complete.
  struct IoState {
    OVERLAPPED ov{};
    std::vector<uint64_t> buf;
  };

  core::UniqueHandle dir_handle_;
  core::UniqueHandle event_;
  std::unique_ptr<IoState> io_;
  size_t buf_bytes_ = 0;
  FnReadDirectoryChangesExW read_dir_changes_ex_ = nullptr;
  bool pending_ = false;
};

const NotifyRecord *FindNotifyEvent(const std::vector<NotifyRecord> &records,
                                    DWORD action, std::wstring_view filename) {
  for (const auto &rec : records) {
    if (rec.action == action &&
        core::EqualsIgnoreCase(rec.filename, filename)) {
      return &rec;
    }
  }
  return nullptr;
}

bool HasNotifyEvent(const std::vector<NotifyRecord> &records, DWORD action,
                    std::wstring_view filename) {
  return FindNotifyEvent(records, action, filename) != nullptr;
}

// True iff `records` holds `action` for an alternate data stream of
// `base_name`: the reported name is `base_name` followed by a ':'-introduced
// stream suffix (the exact suffix form, ":name" versus ":name:$DATA", is not
// pinned).
bool HasStreamNotifyEvent(const std::vector<NotifyRecord> &records,
                          DWORD action, std::wstring_view base_name) {
  for (const auto &rec : records) {
    if (rec.action != action || rec.filename.size() <= base_name.size() ||
        rec.filename[base_name.size()] != L':') {
      continue;
    }
    if (core::EqualsIgnoreCase(
            std::wstring_view(rec.filename).substr(0, base_name.size()),
            base_name)) {
      return true;
    }
  }
  return false;
}

template <typename Predicate>
core::Win32Result<bool> CollectUntil(AsyncDirWatcher *watcher, DWORD filter,
                                     bool watch_subtree, DWORD timeout_ms,
                                     std::vector<NotifyRecord> *accum,
                                     Predicate &&done_pred) {
  core::Win32Result<bool> result{};
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    const ULONGLONG now = ::GetTickCount64();
    const DWORD remaining =
        (now < deadline) ? static_cast<DWORD>(deadline - now) : 0u;
    const DWORD rc = watcher->Wait(remaining, accum);
    if (rc != ERROR_SUCCESS) {
      result.error = rc;
      return result;
    }
    if (done_pred(*accum)) {
      result.error = ERROR_SUCCESS;
      result.value = true;
      return result;
    }
    if (::GetTickCount64() >= deadline) {
      result.error = ERROR_TIMEOUT;
      return result;
    }
    const DWORD arm_err = watcher->Arm(filter, watch_subtree);
    if (arm_err != ERROR_SUCCESS) {
      result.error = arm_err;
      return result;
    }
  }
}

void WriteAndFlushExistingFile(const std::wstring &path,
                               std::string_view payload) {
  auto fh = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);
  ASSERT_WIN32_OK(
      core::WriteAllBytes(fh.value->get(), payload.data(), payload.size()));
  ::FlushFileBuffers(fh.value->get());
}

core::Win32Result<bool>
WaitForModifiedNotify(AsyncDirWatcher *watcher, DWORD filter, DWORD timeout_ms,
                      std::wstring_view file_name,
                      std::vector<NotifyRecord> *events_out) {
  return CollectUntil(
      watcher, filter, /*watch_subtree=*/false, timeout_ms, events_out,
      [file_name](const std::vector<NotifyRecord> &r) {
        return HasNotifyEvent(r, FILE_ACTION_MODIFIED, file_name);
      });
}

std::vector<uint8_t>
BuildGetEaListBuffer(const std::vector<std::string> &names) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < names.size(); ++i) {
    const size_t name_len = std::min<size_t>(names[i].size(), 255u);
    const size_t raw_size = offsetof(nt::FileGetEaInfo, ea_name) + name_len + 1;
    const bool is_last = (i + 1 == names.size());
    const size_t padded_size =
        is_last ? raw_size : ((raw_size + size_t{3}) & ~size_t{3});
    const size_t base = out.size();
    out.resize(base + padded_size, 0);
    auto *hdr = reinterpret_cast<nt::FileGetEaInfo *>(out.data() + base);
    hdr->next_entry_offset = is_last ? 0UL : static_cast<ULONG>(padded_size);
    hdr->ea_name_length = static_cast<UCHAR>(name_len);
    std::memcpy(hdr->ea_name, names[i].data(), name_len);
    hdr->ea_name[name_len] = '\0';
  }
  return out;
}

const core::EaEntry *FindEa(const std::vector<core::EaEntry> &entries,
                            std::string_view name) {
  for (const auto &e : entries) {
    if (e.name == name) {
      return &e;
    }
  }
  return nullptr;
}

NTSTATUS SetEaEntries(HANDLE handle,
                      const std::vector<core::EaEntry> &entries) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtSetEaFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  std::vector<uint8_t> ea_buf = core::BuildEaBuffer(entries);
  nt::IoStatusBlock iosb{};
  return ntdll.NtSetEaFile(handle, &iosb, ea_buf.data(),
                           static_cast<ULONG>(ea_buf.size()));
}

core::NtResult<ULONG> QueryEaSize(HANDLE handle) {
  core::NtResult<ULONG> result{};
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  nt::IoStatusBlock iosb{};
  nt::FileEaInfo info{};
  result.status = ntdll.NtQueryInformationFile(
      handle, &iosb, &info, sizeof(info), nt::FileInfoClass::FileEaInformation);
  if (nt::NtSuccess(result.status)) {
    result.value = info.ea_size;
  }
  return result;
}

core::NtResult<std::vector<core::EaEntry>> QueryAllEas(HANDLE handle,
                                                       size_t buf_size = 4096) {
  core::NtResult<std::vector<core::EaEntry>> result{};
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryEaFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  std::vector<uint8_t> out_buf(buf_size, 0);
  nt::IoStatusBlock iosb{};
  result.status = ntdll.NtQueryEaFile(handle, &iosb, out_buf.data(),
                                      static_cast<ULONG>(out_buf.size()),
                                      /*return_single_entry=*/FALSE, nullptr, 0,
                                      nullptr, /*restart_scan=*/TRUE);
  if (nt::NtSuccess(result.status)) {
    result.value = core::ParseEaBuffer(out_buf.data(), iosb.information);
  }
  return result;
}

} // namespace

// Spec: ReadDirectoryChangesW delivers FILE_ACTION_ADDED, MODIFIED, and REMOVED
// notifications matching filename, write, and size filters.
TEST_F(InfoQueriesTest, ReadDirectoryChangesFileCreateModifyDelete) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_crud");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring target = core::JoinPath(watch_dir, L"note.txt");

  constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                            FILE_NOTIFY_CHANGE_LAST_WRITE |
                            FILE_NOTIFY_CHANGE_SIZE;
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  // Each phase watches through its own directory handle, opened only after
  // the previous phase's mutation has completed and its file handle is closed.
  // Events are queued per directory handle, so records still queued by an
  // earlier phase (for example a FILE_ACTION_MODIFIED trailing the create)
  // can never satisfy a later phase's expectation.

  // 1. Create -> FILE_ACTION_ADDED
  {
    AsyncDirWatcher watcher;
    ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(watcher.Arm(kFilter, /*watch_subtree=*/false),
                            ERROR_SUCCESS);
    {
      auto fh = core::CreateWin32File(target, GENERIC_WRITE, core::kShareAll,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(fh);
    }
    std::vector<NotifyRecord> added_events;
    auto added_res = CollectUntil(
        &watcher, kFilter, /*watch_subtree=*/false, timeout_ms, &added_events,
        [](const std::vector<NotifyRecord> &recs) {
          return HasNotifyEvent(recs, FILE_ACTION_ADDED, L"note.txt");
        });
    EXPECT_WIN32_OK(added_res) << "Waiting for FILE_ACTION_ADDED on note.txt";
  }

  // 2. Modify -> FILE_ACTION_MODIFIED
  {
    AsyncDirWatcher watcher;
    ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(watcher.Arm(kFilter, /*watch_subtree=*/false),
                            ERROR_SUCCESS);
    {
      auto fh = core::CreateWin32File(target, GENERIC_WRITE, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(fh);
      ASSERT_WIN32_OK(core::WriteAllBytes(fh.value->get(), "updated-body", 12));
      ::FlushFileBuffers(fh.value->get());
    }
    std::vector<NotifyRecord> mod_events;
    auto mod_res = CollectUntil(
        &watcher, kFilter, /*watch_subtree=*/false, timeout_ms, &mod_events,
        [](const std::vector<NotifyRecord> &recs) {
          return HasNotifyEvent(recs, FILE_ACTION_MODIFIED, L"note.txt");
        });
    EXPECT_WIN32_OK(mod_res) << "Waiting for FILE_ACTION_MODIFIED on note.txt";
  }

  // 3. Delete -> FILE_ACTION_REMOVED
  {
    AsyncDirWatcher watcher;
    ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(watcher.Arm(kFilter, /*watch_subtree=*/false),
                            ERROR_SUCCESS);
    ASSERT_WIN32_TRUE(::DeleteFileW(target.c_str()));
    std::vector<NotifyRecord> rem_events;
    auto rem_res = CollectUntil(
        &watcher, kFilter, /*watch_subtree=*/false, timeout_ms, &rem_events,
        [](const std::vector<NotifyRecord> &recs) {
          return HasNotifyEvent(recs, FILE_ACTION_REMOVED, L"note.txt");
        });
    EXPECT_WIN32_OK(rem_res) << "Waiting for FILE_ACTION_REMOVED on note.txt";
  }
}

// Spec: ReadDirectoryChangesW emits paired FILE_ACTION_RENAMED_OLD_NAME and
// FILE_ACTION_RENAMED_NEW_NAME records for an in-directory file rename.
TEST_F(InfoQueriesTest, ReadDirectoryChangesRenameOldAndNewName) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_rename");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring before = core::JoinPath(watch_dir, L"before.txt");
  const std::wstring after = core::JoinPath(watch_dir, L"after.txt");
  ASSERT_WIN32_OK(core::WriteFileContents(before, "rename-me"));

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);

  ASSERT_WIN32_TRUE(::MoveFileExW(before.c_str(), after.c_str(), 0));

  std::vector<NotifyRecord> events;
  auto rename_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false,
      timeout_ms, &events, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_RENAMED_OLD_NAME,
                              L"before.txt") &&
               HasNotifyEvent(recs, FILE_ACTION_RENAMED_NEW_NAME, L"after.txt");
      });
  EXPECT_WIN32_OK(rename_res)
      << "Waiting for RENAMED_OLD_NAME/RENAMED_NEW_NAME pair";
}

// Spec: ReadDirectoryChangesW with bWatchSubtree=TRUE reports relative nested
// paths ("sub\\nested.txt") for adds and deletes in child directories.
TEST_F(InfoQueriesTest, ReadDirectoryChangesSubtreeRecursive) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_tree");
  const std::wstring sub = core::JoinPath(watch_dir, L"sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  const std::wstring nested = core::JoinPath(sub, L"nested.txt");

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);

  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/true),
      ERROR_SUCCESS);
  ASSERT_WIN32_OK(core::WriteFileContents(nested, "deep-notify"));

  std::vector<NotifyRecord> add_events;
  auto add_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/true,
      timeout_ms, &add_events, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_ADDED, L"sub\\nested.txt");
      });
  EXPECT_WIN32_OK(add_res)
      << "Waiting for recursive FILE_ACTION_ADDED on sub\\nested.txt";

  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/true),
      ERROR_SUCCESS);
  ASSERT_WIN32_TRUE(::DeleteFileW(nested.c_str()));

  std::vector<NotifyRecord> del_events;
  auto del_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/true,
      timeout_ms, &del_events, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_REMOVED, L"sub\\nested.txt");
      });
  EXPECT_WIN32_OK(del_res)
      << "Waiting for recursive FILE_ACTION_REMOVED on sub\\nested.txt";
}

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_ATTRIBUTES reports
// FILE_ACTION_MODIFIED when a direct child's attributes change.
TEST_F(InfoQueriesTest, ReadDirectoryChangesAttributesFilter) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring watch_dir = TestPath(L"watch_attributes");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring target = core::JoinPath(watch_dir, L"target.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(target, "abc"));

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_ATTRIBUTES, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_HIDDEN));

  std::vector<NotifyRecord> attr_events;
  auto attr_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_ATTRIBUTES, /*watch_subtree=*/false,
      timeout_ms, &attr_events, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_MODIFIED, L"target.dat");
      });
  EXPECT_WIN32_OK(attr_res) << "Waiting for FILE_NOTIFY_CHANGE_ATTRIBUTES";
}

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_SIZE alone reports
// FILE_ACTION_MODIFIED when a direct child grows by less than one cluster, and
// stays silent for an attribute-only change on the same child.
TEST_F(InfoQueriesTest, ReadDirectoryChangesSizeFilter) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const DWORD quiet_ms = std::min<DWORD>(timeout_ms, 250u);
  const std::wstring watch_dir = TestPath(L"watch_size");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring target = core::JoinPath(watch_dir, L"target.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(target, "abc"));

  // A data write sets FILE_ATTRIBUTE_ARCHIVE on NTFS. Set it up front so the
  // write below changes no attribute and FILE_NOTIFY_CHANGE_SIZE is the only
  // reason a FILE_ACTION_MODIFIED record can be produced for it.
  ASSERT_WIN32_TRUE(
      ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_ARCHIVE));
  auto attrs = core::QueryWin32Attributes(target);
  ASSERT_WIN32_OK(attrs);
  ASSERT_NE(*attrs.value & FILE_ATTRIBUTE_ARCHIVE, 0u);

  // This directory handle only ever carries the SIZE filter: the kernel
  // latches the filter of the first request issued on a handle.
  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);

  // Negative control: an attribute-only change must not complete the watch.
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_SIZE, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  ASSERT_WIN32_TRUE(::SetFileAttributesW(
      target.c_str(), FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN));
  std::vector<NotifyRecord> unexpected;
  EXPECT_WIN32_ERROR_CODE(watcher.Wait(quiet_ms, &unexpected), ERROR_TIMEOUT);
  EXPECT_TRUE(unexpected.empty())
      << "Attribute-only change completed a FILE_NOTIFY_CHANGE_SIZE watch";

  // Positive: grow the file from 3 to 512 bytes.
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_SIZE, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  {
    auto fh = core::CreateWin32File(target, GENERIC_WRITE, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(fh);
    const std::string grown(512, 'Z');
    ASSERT_WIN32_OK(
        core::WriteAllBytes(fh.value->get(), grown.data(), grown.size()));
    ::FlushFileBuffers(fh.value->get());
  }
  std::vector<NotifyRecord> size_events;
  auto size_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_SIZE, /*watch_subtree=*/false, timeout_ms,
      &size_events, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_MODIFIED, L"target.dat");
      });
  EXPECT_WIN32_OK(size_res) << "Waiting for FILE_NOTIFY_CHANGE_SIZE";
}

// Spec: when more changes occur between two ReadDirectoryChangesW calls than
// the notify buffer can hold, the next call reports the overflow instead of
// records: ERROR_NOTIFY_ENUM_DIR, surfaced through overlapped completion as a
// successful result with zero bytes transferred.
TEST_F(InfoQueriesTest, ReadDirectoryChangesSmallBufferOverflow) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const std::wstring ovf_dir = TestPath(L"watch_overflow");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(ovf_dir.c_str(), nullptr));

  // Establish the handle's 64-byte notify buffer with a first completed call.
  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(ovf_dir, /*buffer_size=*/64),
                          ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  ASSERT_WIN32_OK(
      core::WriteFileContents(core::JoinPath(ovf_dir, L"seed.txt"), "seed"));
  std::vector<NotifyRecord> seed_events;
  ASSERT_WIN32_ERROR_CODE(watcher.Wait(timeout_ms, &seed_events),
                          ERROR_SUCCESS);

  // Burst creations while no ReadDirectoryChangesW call is active so the
  // 64-byte kernel queue overflows deterministically.
  for (int i = 0; i < 50; ++i) {
    wchar_t name_buf[48];
    std::swprintf(name_buf, sizeof(name_buf) / sizeof(name_buf[0]),
                  L"overflow_trigger_entry_%04d.dat", i);
    auto fh = core::CreateWin32File(core::JoinPath(ovf_dir, name_buf),
                                    GENERIC_WRITE, core::kShareAll, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(fh);
  }

  const DWORD arm_err =
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false);
  if (arm_err == ERROR_NOTIFY_ENUM_DIR) {
    // The overflow was reported synchronously by the call itself.
    SUCCEED();
  } else {
    ASSERT_WIN32_ERROR_CODE(arm_err, ERROR_SUCCESS);
    DWORD bytes_returned = 0xFFFFFFFFu;
    NTSTATUS completion_status = STATUS_UNSUCCESSFUL;
    std::vector<NotifyRecord> ovf_events;
    const DWORD wait_err = watcher.Wait(timeout_ms, &ovf_events,
                                        &bytes_returned, &completion_status);
    if (wait_err == ERROR_NOTIFY_ENUM_DIR) {
      SUCCEED();
    } else {
      // STATUS_NOTIFY_ENUM_DIR has success severity, so GetOverlappedResult
      // reports ERROR_SUCCESS with 0 bytes transferred. Verify the raw
      // OVERLAPPED.Internal status so STATUS_SUCCESS or STATUS_NOTIFY_CLEANUP
      // cannot pass falsely.
      EXPECT_WIN32_ERROR_CODE(wait_err, ERROR_SUCCESS);
      EXPECT_EQ(bytes_returned, 0u);
      EXPECT_NT_STATUS(completion_status, STATUS_NOTIFY_ENUM_DIR);
    }
  }
}

// Spec: Windows 10 1709+ ReadDirectoryChangesExW with ExtendedInformation
// delivers FileNotifyExtendedInfo carrying matching 64-bit FileId and attrs.
TEST_F(InfoQueriesTest, ReadDirectoryChangesExExtendedInfo) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1709);

  HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
  FARPROC proc = (kernel32 != nullptr)
                     ? ::GetProcAddress(kernel32, "ReadDirectoryChangesExW")
                     : nullptr;
  if (proc == nullptr) {
    RecordSkipCategory("os_build");
    GTEST_SKIP() << "ReadDirectoryChangesExW is unavailable on this host OS.";
  }
  FnReadDirectoryChangesExW read_dir_changes_ex = nullptr;
  static_assert(sizeof(read_dir_changes_ex) == sizeof(proc));
  std::memcpy(&read_dir_changes_ex, &proc, sizeof(read_dir_changes_ex));

  const std::wstring watch_dir = TestPath(L"watch_ext");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring file_path = core::JoinPath(watch_dir, L"ext_notify.dat");

  constexpr DWORD kFilter =
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  watcher.EnableExtendedInfo(read_dir_changes_ex);
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(watcher.Arm(kFilter, /*watch_subtree=*/false),
                          ERROR_SUCCESS);

  ASSERT_WIN32_OK(core::WriteFileContents(file_path, "hello-ext-notify"));

  auto expected_id = core::QueryPathFileIndex(file_path);
  ASSERT_WIN32_OK(expected_id);
  ASSERT_NE(*expected_id.value, 0ULL);

  std::vector<NotifyRecord> events;
  auto found_added = CollectUntil(
      &watcher, kFilter, /*watch_subtree=*/false, timeout_ms, &events,
      [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_ADDED, L"ext_notify.dat");
      });
  EXPECT_WIN32_OK(found_added)
      << "Waiting for FILE_ACTION_ADDED extended notification on "
         "ext_notify.dat";

  const NotifyRecord *rec =
      FindNotifyEvent(events, FILE_ACTION_ADDED, L"ext_notify.dat");
  if (rec != nullptr) {
    EXPECT_TRUE(rec->has_extended_info);
    EXPECT_NE(rec->file_id, 0LL);
    EXPECT_EQ(static_cast<uint64_t>(rec->file_id), *expected_id.value);
    EXPECT_NE(rec->file_attributes, 0u);
  }
}

// Spec: [MS-FSA] "Server Requests Change Notifications for a Directory" /
// Win32 ReadDirectoryChangesW — unwatched filter events and subdirectory
// mutations under bWatchSubtree=FALSE do not fire while matching direct-child
// events do.
TEST_F(InfoQueriesTest, ReadDirectoryChangesFilterExclusionAndSubtreeFlag) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_exclusion");
  const std::wstring sub_dir = core::JoinPath(watch_dir, L"sub");
  const std::wstring target = core::JoinPath(watch_dir, L"target.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(target, "seed"));

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const DWORD quiet_ms = std::min<DWORD>(timeout_ms, 250u);

  // 1. Attribute change must NOT trigger a FILE_NOTIFY_CHANGE_SIZE-only watch,
  // while a subsequent size change DOES trigger it.
  {
    AsyncDirWatcher watcher;
    ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(
        watcher.Arm(FILE_NOTIFY_CHANGE_SIZE, /*watch_subtree=*/false),
        ERROR_SUCCESS);

    ASSERT_WIN32_TRUE(
        ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_HIDDEN));
    std::vector<NotifyRecord> unexpected;
    EXPECT_WIN32_ERROR_CODE(watcher.Wait(quiet_ms, &unexpected), ERROR_TIMEOUT);
    EXPECT_TRUE(unexpected.empty());

    ASSERT_WIN32_ERROR_CODE(
        watcher.Arm(FILE_NOTIFY_CHANGE_SIZE, /*watch_subtree=*/false),
        ERROR_SUCCESS);
    {
      auto fh = core::CreateWin32File(target, GENERIC_WRITE, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(fh);
      // Grow past a 4 KiB cluster/page boundary for the positive filter-armed
      // control (sub-cluster size growth is covered separately by
      // ReadDirectoryChangesSizeFilter).
      const std::string grown(8192, 'S');
      ASSERT_WIN32_OK(
          core::WriteAllBytes(fh.value->get(), grown.data(), grown.size()));
      ::FlushFileBuffers(fh.value->get());
    }
    std::vector<NotifyRecord> size_events;
    auto size_res = CollectUntil(
        &watcher, FILE_NOTIFY_CHANGE_SIZE, /*watch_subtree=*/false, timeout_ms,
        &size_events, [](const std::vector<NotifyRecord> &recs) {
          return HasNotifyEvent(recs, FILE_ACTION_MODIFIED, L"target.dat");
        });
    EXPECT_WIN32_OK(size_res) << "Waiting for active FILE_NOTIFY_CHANGE_SIZE";
  }

  // 2. Subdirectory creation must NOT trigger a bWatchSubtree=FALSE watch,
  // while a direct-child creation DOES trigger it.
  {
    AsyncDirWatcher watcher;
    ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(
        watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false),
        ERROR_SUCCESS);

    ASSERT_WIN32_OK(core::WriteFileContents(
        core::JoinPath(sub_dir, L"nested_ignored.txt"), "sub-only"));
    std::vector<NotifyRecord> unexpected;
    EXPECT_WIN32_ERROR_CODE(watcher.Wait(quiet_ms, &unexpected), ERROR_TIMEOUT);
    EXPECT_TRUE(unexpected.empty());

    ASSERT_WIN32_ERROR_CODE(
        watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false),
        ERROR_SUCCESS);
    ASSERT_WIN32_OK(core::WriteFileContents(
        core::JoinPath(watch_dir, L"direct_child.txt"), "direct"));
    std::vector<NotifyRecord> direct_events;
    auto direct_res = CollectUntil(
        &watcher, FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false,
        timeout_ms, &direct_events, [](const std::vector<NotifyRecord> &recs) {
          return HasNotifyEvent(recs, FILE_ACTION_ADDED, L"direct_child.txt");
        });
    EXPECT_WIN32_OK(direct_res) << "Waiting for direct_child.txt add";
  }
}

// Spec: [MS-FSA] "Server Requests Change Notifications for a Directory" /
// Win32 ReadDirectoryChangesW — FileBasicInformation mutations fire only the
// notify filters matching the fields actually modified: SetFileAttributesW
// must not fire LAST_WRITE or CREATION, and SetFileTime must not fire
// ATTRIBUTES.
TEST_F(InfoQueriesTest, ReadDirectoryChangesBasicInfoFilterIsolation) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_basic_isolation");
  const std::wstring target = core::JoinPath(watch_dir, L"target.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(target, "seed"));

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const DWORD quiet_ms = std::min<DWORD>(timeout_ms, 250u);

  // 1. SetFileAttributesW must NOT fire LAST_WRITE or CREATION, while a real
  // write sentinel proves the watch is active.
  {
    constexpr DWORD kTimeFilter =
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    AsyncDirWatcher watcher;
    ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(watcher.Arm(kTimeFilter, /*watch_subtree=*/false),
                            ERROR_SUCCESS);

    ASSERT_WIN32_TRUE(
        ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_HIDDEN));
    std::vector<NotifyRecord> unexpected;
    EXPECT_WIN32_ERROR_CODE(watcher.Wait(quiet_ms, &unexpected), ERROR_TIMEOUT);
    EXPECT_TRUE(unexpected.empty());

    ASSERT_WIN32_ERROR_CODE(watcher.Arm(kTimeFilter, /*watch_subtree=*/false),
                            ERROR_SUCCESS);
    ASSERT_NO_FATAL_FAILURE(
        WriteAndFlushExistingFile(target, "updated-for-last-write"));
    std::vector<NotifyRecord> sentinel_events;
    EXPECT_WIN32_OK(WaitForModifiedNotify(&watcher, kTimeFilter, timeout_ms,
                                          L"target.dat", &sentinel_events))
        << "Waiting for active LAST_WRITE sentinel on target.dat";
  }

  // 2. SetFileTime must NOT fire FILE_NOTIFY_CHANGE_ATTRIBUTES, while a real
  // attribute change sentinel proves the watch is active.
  {
    AsyncDirWatcher watcher;
    ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(
        watcher.Arm(FILE_NOTIFY_CHANGE_ATTRIBUTES, /*watch_subtree=*/false),
        ERROR_SUCCESS);

    const FILETIME ft_create = core::MakeUtcFileTime(2022, 6, 15, 12, 0, 0);
    const FILETIME ft_write = core::MakeUtcFileTime(2023, 7, 20, 14, 30, 0);
    {
      auto fh = core::CreateWin32File(
          target, FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(fh);
      ASSERT_WIN32_TRUE(::SetFileTime(fh.value->get(), &ft_create,
                                      /*lpLastAccessTime=*/nullptr, &ft_write));
    }
    std::vector<NotifyRecord> unexpected;
    EXPECT_WIN32_ERROR_CODE(watcher.Wait(quiet_ms, &unexpected), ERROR_TIMEOUT);
    EXPECT_TRUE(unexpected.empty());

    ASSERT_WIN32_ERROR_CODE(
        watcher.Arm(FILE_NOTIFY_CHANGE_ATTRIBUTES, /*watch_subtree=*/false),
        ERROR_SUCCESS);
    ASSERT_WIN32_TRUE(
        ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_ARCHIVE));
    std::vector<NotifyRecord> sentinel_events;
    EXPECT_WIN32_OK(
        WaitForModifiedNotify(&watcher, FILE_NOTIFY_CHANGE_ATTRIBUTES,
                              timeout_ms, L"target.dat", &sentinel_events))
        << "Waiting for active ATTRIBUTES sentinel on target.dat";
  }
}

// Spec: [MS-FSA] "Server Requests Change Notifications for a Directory" /
// Win32 ReadDirectoryChangesW — after a write handle closes and its
// FILE_NOTIFY_CHANGE_LAST_WRITE event is delivered, closing a read-only handle
// must not emit a spurious FILE_NOTIFY_CHANGE_LAST_WRITE notification.
TEST_F(InfoQueriesTest,
       ReadDirectoryChangesReadOnlyCloseAfterWriteNoSpuriousNotify) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_ro_close_after_write");
  const std::wstring target = core::JoinPath(watch_dir, L"target.dat");
  const std::wstring sentinel = core::JoinPath(watch_dir, L"sentinel.dat");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::WriteFileContents(target, "seed"));
  ASSERT_WIN32_OK(core::WriteFileContents(sentinel, "sentinel"));

  const DWORD timeout_ms = Config().timing.async_timeout_ms;
  const DWORD quiet_ms = std::min<DWORD>(timeout_ms, 250u);

  // Keep a read-only handle open across the write so the FCB stays live (no
  // FCB recycle between the writer close and the reader close), then also open
  // and close a fresh read-only handle after the write completes.
  auto ro_overlapping =
      core::CreateWin32File(target, GENERIC_READ, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(ro_overlapping);

  {
    AsyncDirWatcher write_watcher;
    ASSERT_WIN32_ERROR_CODE(write_watcher.Open(watch_dir), ERROR_SUCCESS);
    ASSERT_WIN32_ERROR_CODE(write_watcher.Arm(FILE_NOTIFY_CHANGE_LAST_WRITE,
                                              /*watch_subtree=*/false),
                            ERROR_SUCCESS);
    ASSERT_NO_FATAL_FAILURE(
        WriteAndFlushExistingFile(target, "written-before-ro-close"));
    std::vector<NotifyRecord> write_events;
    ASSERT_WIN32_OK(
        WaitForModifiedNotify(&write_watcher, FILE_NOTIFY_CHANGE_LAST_WRITE,
                              timeout_ms, L"target.dat", &write_events))
        << "Waiting for initial FILE_ACTION_MODIFIED on target.dat";
  }

  // Fresh watcher starts with an empty notify queue after the writer closed.
  AsyncDirWatcher ro_watcher;
  ASSERT_WIN32_ERROR_CODE(ro_watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      ro_watcher.Arm(FILE_NOTIFY_CHANGE_LAST_WRITE, /*watch_subtree=*/false),
      ERROR_SUCCESS);

  // Close the overlapping read-only handle and open/read/close a second
  // read-only handle; neither modifies the file.
  ro_overlapping.value->reset();
  {
    auto ro_after = core::CreateWin32File(target, GENERIC_READ, core::kShareAll,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(ro_after);
    auto data = core::ReadAllBytes(ro_after.value->get());
    ASSERT_WIN32_OK(data);
  }

  std::vector<NotifyRecord> unexpected;
  EXPECT_WIN32_ERROR_CODE(ro_watcher.Wait(quiet_ms, &unexpected),
                          ERROR_TIMEOUT);
  EXPECT_FALSE(HasNotifyEvent(unexpected, FILE_ACTION_MODIFIED, L"target.dat"))
      << "Spurious FILE_ACTION_MODIFIED on target.dat after read-only close";

  // Positive control: mutating sentinel.dat proves the watch is active and
  // delivers only sentinel.dat.
  ASSERT_WIN32_ERROR_CODE(
      ro_watcher.Arm(FILE_NOTIFY_CHANGE_LAST_WRITE, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  ASSERT_NO_FATAL_FAILURE(WriteAndFlushExistingFile(sentinel, "sentinel-ping"));
  std::vector<NotifyRecord> sentinel_events;
  EXPECT_WIN32_OK(
      WaitForModifiedNotify(&ro_watcher, FILE_NOTIFY_CHANGE_LAST_WRITE,
                            timeout_ms, L"sentinel.dat", &sentinel_events))
      << "Waiting for sentinel FILE_ACTION_MODIFIED on sentinel.dat";
  EXPECT_FALSE(
      HasNotifyEvent(sentinel_events, FILE_ACTION_MODIFIED, L"target.dat"));
}

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_DIR_NAME reports
// FILE_ACTION_ADDED when a child directory is created.
TEST_F(InfoQueriesTest, ReadDirectoryChangesDirNameAdded) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_dir_added");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring child = core::JoinPath(watch_dir, L"sub_a");
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_DIR_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  ASSERT_WIN32_TRUE(::CreateDirectoryW(child.c_str(), nullptr));

  std::vector<NotifyRecord> add_recs;
  auto add_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_DIR_NAME, /*watch_subtree=*/false,
      timeout_ms, &add_recs, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_ADDED, L"sub_a");
      });
  EXPECT_WIN32_OK(add_res) << "Waiting for DIR_NAME ADDED";
}

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_DIR_NAME reports
// FILE_ACTION_REMOVED when a child directory is removed.
TEST_F(InfoQueriesTest, ReadDirectoryChangesDirNameRemoved) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_dir_removed");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring child = core::JoinPath(watch_dir, L"sub_b");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(child.c_str(), nullptr));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_DIR_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  ASSERT_WIN32_TRUE(::RemoveDirectoryW(child.c_str()));

  std::vector<NotifyRecord> rem_recs;
  auto rem_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_DIR_NAME, /*watch_subtree=*/false,
      timeout_ms, &rem_recs, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_REMOVED, L"sub_b");
      });
  EXPECT_WIN32_OK(rem_res) << "Waiting for DIR_NAME REMOVED";
}

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_DIR_NAME reports paired
// FILE_ACTION_RENAMED_OLD_NAME and FILE_ACTION_RENAMED_NEW_NAME records when a
// child directory is renamed in place.
TEST_F(InfoQueriesTest, ReadDirectoryChangesDirNameRenamed) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_dir_renamed");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring dir1 = core::JoinPath(watch_dir, L"sub_a");
  const std::wstring dir2 = core::JoinPath(watch_dir, L"sub_b");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir1.c_str(), nullptr));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_DIR_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  ASSERT_WIN32_TRUE(::MoveFileExW(dir1.c_str(), dir2.c_str(), 0));

  std::vector<NotifyRecord> ren_recs;
  auto ren_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_DIR_NAME, /*watch_subtree=*/false,
      timeout_ms, &ren_recs, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_RENAMED_OLD_NAME, L"sub_a") &&
               HasNotifyEvent(recs, FILE_ACTION_RENAMED_NEW_NAME, L"sub_b");
      });
  EXPECT_WIN32_OK(ren_res) << "Waiting for DIR_NAME RENAMED pair";
}

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_SECURITY reports
// FILE_ACTION_MODIFIED when a direct child's DACL is replaced.
TEST_F(InfoQueriesTest, ReadDirectoryChangesSecurityFilter) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring watch_dir = TestPath(L"watch_security");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring sec_file = core::JoinPath(watch_dir, L"sec_target.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(sec_file, "sec-notify"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_SECURITY, /*watch_subtree=*/false),
      ERROR_SUCCESS);

  auto set_dacl = core::SetFileSddl(sec_file, DACL_SECURITY_INFORMATION,
                                    L"D:P(A;;FA;;;WD)");
  ASSERT_WIN32_OK(set_dacl);

  std::vector<NotifyRecord> sec_recs;
  auto sec_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_SECURITY, /*watch_subtree=*/false,
      timeout_ms, &sec_recs, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_MODIFIED, L"sec_target.dat");
      });
  EXPECT_WIN32_OK(sec_res) << "Waiting for FILE_NOTIFY_CHANGE_SECURITY";
}

// Spec: a recursive ReadDirectoryChangesW watch reports a move between two
// child directories as FILE_ACTION_REMOVED from the source directory plus
// FILE_ACTION_ADDED in the destination directory, never as a
// RENAMED_OLD_NAME/RENAMED_NEW_NAME pair.
TEST_F(InfoQueriesTest, ReadDirectoryChangesCrossDirectoryMove) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_cross_move");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring dir_a = core::JoinPath(watch_dir, L"move_a");
  const std::wstring dir_b = core::JoinPath(watch_dir, L"move_b");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_b.c_str(), nullptr));
  const std::wstring src = core::JoinPath(dir_a, L"cross.dat");
  const std::wstring dst = core::JoinPath(dir_b, L"cross.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(src, "cross-dir"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/true),
      ERROR_SUCCESS);

  ASSERT_WIN32_TRUE(::MoveFileExW(src.c_str(), dst.c_str(), 0));
  std::vector<NotifyRecord> move_recs;
  auto move_res = CollectUntil(
      &watcher, FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/true,
      timeout_ms, &move_recs, [](const std::vector<NotifyRecord> &recs) {
        return HasNotifyEvent(recs, FILE_ACTION_REMOVED,
                              L"move_a\\cross.dat") &&
               HasNotifyEvent(recs, FILE_ACTION_ADDED, L"move_b\\cross.dat");
      });
  EXPECT_WIN32_OK(move_res) << "Waiting for cross-directory REMOVED+ADDED";
  EXPECT_FALSE(HasNotifyEvent(move_recs, FILE_ACTION_RENAMED_OLD_NAME,
                              L"move_a\\cross.dat"));
  EXPECT_FALSE(HasNotifyEvent(move_recs, FILE_ACTION_RENAMED_NEW_NAME,
                              L"move_b\\cross.dat"));
}

// Spec: CancelIoEx aborts a pending ReadDirectoryChangesW request, which
// completes with ERROR_OPERATION_ABORTED.
TEST_F(InfoQueriesTest, ReadDirectoryChangesCancelIoEx) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_cancel");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);

  auto cancel_res = watcher.CancelAndAwaitResult();
  EXPECT_FALSE(cancel_res);
  EXPECT_WIN32_ERROR_CODE(cancel_res.error, ERROR_OPERATION_ABORTED);
}

// Spec: closing the directory handle completes its pending change-notify
// request with STATUS_NOTIFY_CLEANUP (Win32 ERROR_NOTIFY_CLEANUP) and no
// records. STATUS_NOTIFY_CLEANUP has success severity, so the raw completion
// status is read from the OVERLAPPED instead of GetOverlappedResult.
TEST_F(InfoQueriesTest,
       ReadDirectoryChangesHandleCloseCompletesWithNotifyCleanup) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_cleanup");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);

  NTSTATUS completion_status = STATUS_UNSUCCESSFUL;
  ULONG_PTR completion_bytes = static_cast<ULONG_PTR>(-1);
  ASSERT_WIN32_ERROR_CODE(
      watcher.CloseHandleAndAwaitCompletion(timeout_ms, &completion_status,
                                            &completion_bytes),
      ERROR_SUCCESS);
  EXPECT_NT_STATUS(completion_status, STATUS_NOTIFY_CLEANUP);
  EXPECT_EQ(completion_bytes, ULONG_PTR{0});
}

// Spec: marking the watched directory itself for deletion completes its
// pending change-notify request with STATUS_DELETE_PENDING (Win32
// ERROR_ACCESS_DENIED) while the watching handle is still open.
TEST_F(InfoQueriesTest,
       ReadDirectoryChangesWatchedDirectoryDeletePendingCompletesWatch) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);

  const std::wstring watch_dir = TestPath(L"watch_deleted");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(FILE_NOTIFY_CHANGE_FILE_NAME, /*watch_subtree=*/false),
      ERROR_SUCCESS);

  // Classic (non-POSIX) delete disposition through a second handle: the
  // directory stays delete-pending until the watcher's handle closes.
  auto del_h =
      core::CreateWin32File(watch_dir, DELETE | SYNCHRONIZE, core::kShareAll,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(del_h);
  ASSERT_NT_SUCCESS(
      core::SetDisposition(del_h.value->get(), /*delete_file=*/true).status);

  std::vector<NotifyRecord> recs;
  EXPECT_WIN32_ERROR_CODE(watcher.Wait(timeout_ms, &recs), ERROR_ACCESS_DENIED);
  EXPECT_TRUE(recs.empty());
}

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_STREAM_NAME reports
// FILE_ACTION_ADDED_STREAM when a named stream is created on a direct child.
TEST_F(InfoQueriesTest, ReadDirectoryChangesStreamNameAddedStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring watch_dir = TestPath(L"watch_stream_add");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring host = core::JoinPath(watch_dir, L"host.txt");
  ASSERT_WIN32_OK(core::WriteFileContents(host, "host"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(
      watcher.Arm(kFileNotifyChangeStreamName, /*watch_subtree=*/false),
      ERROR_SUCCESS);
  {
    auto sh =
        core::CreateWin32File(host + L":side", GENERIC_WRITE, core::kShareAll,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(sh);
  }

  std::vector<NotifyRecord> recs;
  auto res = CollectUntil(
      &watcher, kFileNotifyChangeStreamName, /*watch_subtree=*/false,
      timeout_ms, &recs, [](const std::vector<NotifyRecord> &r) {
        return HasStreamNotifyEvent(r, kFileActionAddedStream, L"host.txt");
      });
  EXPECT_WIN32_OK(res) << "Waiting for FILE_ACTION_ADDED_STREAM on host.txt";
}

// Note: Real Windows 11 NTFS does not deliver FILE_ACTION_REMOVED_STREAM via
// ReadDirectoryChangesW when deleting an alternate data stream (whether via
// DeleteFileW, FileDispositionInformation, or FILE_FLAG_DELETE_ON_CLOSE), so
// stream-removal notification is intentionally not asserted here.

// Spec: ReadDirectoryChangesW with FILE_NOTIFY_CHANGE_STREAM_SIZE |
// FILE_NOTIFY_CHANGE_STREAM_WRITE reports FILE_ACTION_MODIFIED_STREAM after an
// existing named stream of a direct child is extended and its handle closed.
TEST_F(InfoQueriesTest, ReadDirectoryChangesStreamSizeWriteModifiedStream) {
  REQUIRE_FS_CAPABILITY(FsCapability::kChangeNotifications);
  REQUIRE_FS_CAPABILITY(FsCapability::kNamedStreams);

  const std::wstring watch_dir = TestPath(L"watch_stream_modify");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(watch_dir.c_str(), nullptr));
  const std::wstring host = core::JoinPath(watch_dir, L"host.txt");
  const std::wstring stream = host + L":side";
  ASSERT_WIN32_OK(core::WriteFileContents(host, "host"));
  ASSERT_WIN32_OK(core::WriteFileContents(stream, "side"));
  const DWORD timeout_ms = Config().timing.async_timeout_ms;

  constexpr DWORD kFilter =
      kFileNotifyChangeStreamSize | kFileNotifyChangeStreamWrite;
  AsyncDirWatcher watcher;
  ASSERT_WIN32_ERROR_CODE(watcher.Open(watch_dir), ERROR_SUCCESS);
  ASSERT_WIN32_ERROR_CODE(watcher.Arm(kFilter, /*watch_subtree=*/false),
                          ERROR_SUCCESS);
  {
    auto sh = core::CreateWin32File(stream, GENERIC_WRITE, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(sh);
    const std::string grown(256, 'S');
    ASSERT_WIN32_OK(
        core::WriteAllBytes(sh.value->get(), grown.data(), grown.size()));
    ::FlushFileBuffers(sh.value->get());
  }

  std::vector<NotifyRecord> recs;
  auto res = CollectUntil(
      &watcher, kFilter, /*watch_subtree=*/false, timeout_ms, &recs,
      [](const std::vector<NotifyRecord> &r) {
        return HasStreamNotifyEvent(r, kFileActionModifiedStream, L"host.txt");
      });
  EXPECT_WIN32_OK(res) << "Waiting for FILE_ACTION_MODIFIED_STREAM on host.txt";
}

// Spec: [MS-FSCC] 2.4.16 — NtSetEaFile/NtQueryEaFile preserve binary values,
// uppercase EA names, update FileEaInformation, and honor single-entry restart.
TEST_F(InfoQueriesTest, ExtendedAttributesSetAndQuery) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryEaFile, nullptr);

  const std::wstring file_path = TestPath(L"ea_set_query.dat");
  auto fh =
      core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);

  const std::vector<uint8_t> bin_val = {0x00, 0x01, 0x7F, 0x80, 0xFF};
  const std::vector<uint8_t> text_val = {'a', 'l', 'i', 'c', 'e'};
  ASSERT_NT_SUCCESS(SetEaEntries(
      fh.value->get(), {{0, "author", text_val}, {0, "bin_val", bin_val}}));

  // Verify FileEaInformation reports non-zero EaSize.
  auto ea_size = QueryEaSize(fh.value->get());
  ASSERT_NT_SUCCESS(ea_size.status);
  EXPECT_GT(*ea_size.value, 0u);

  // Full batch query: NTFS uppercases EA names and preserves binary values.
  {
    auto all_eas = QueryAllEas(fh.value->get());
    ASSERT_NT_SUCCESS(all_eas.status);
    ASSERT_EQ(all_eas.value->size(), size_t{2});
    const core::EaEntry *ea_author = FindEa(*all_eas.value, "AUTHOR");
    const core::EaEntry *ea_bin = FindEa(*all_eas.value, "BIN_VAL");
    ASSERT_NE(ea_author, nullptr);
    ASSERT_NE(ea_bin, nullptr);
    EXPECT_EQ(ea_author->value, text_val);
    EXPECT_EQ(ea_bin->value, bin_val);
  }

  // ReturnSingleEntry + RestartScan walk.
  {
    std::vector<uint8_t> out_buf(1024, 0);
    std::vector<std::string> walked_names;
    bool first = true;
    for (int iter = 0; iter < 16; ++iter) {
      nt::IoStatusBlock iosb{};
      const NTSTATUS st = ntdll.NtQueryEaFile(
          fh.value->get(), &iosb, out_buf.data(),
          static_cast<ULONG>(out_buf.size()), /*return_single_entry=*/TRUE,
          nullptr, 0, nullptr, /*restart_scan=*/first ? TRUE : FALSE);
      first = false;
      if (st == nt::kStatusNoMoreEas) {
        break;
      }
      ASSERT_NT_SUCCESS(st);
      const std::vector<core::EaEntry> single =
          core::ParseEaBuffer(out_buf.data(), iosb.information);
      ASSERT_EQ(single.size(), size_t{1});
      walked_names.push_back(single[0].name);
    }
    const std::vector<std::string> expected_names = {"AUTHOR", "BIN_VAL"};
    EXPECT_MULTISET_EQ(walked_names, expected_names);

    // RestartScan=TRUE resets cursor back to the first EA.
    ASSERT_FALSE(walked_names.empty());
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryEaFile(
        fh.value->get(), &iosb, out_buf.data(),
        static_cast<ULONG>(out_buf.size()), /*return_single_entry=*/TRUE,
        nullptr, 0, nullptr, /*restart_scan=*/TRUE);
    ASSERT_NT_SUCCESS(st);
    const std::vector<core::EaEntry> restarted =
        core::ParseEaBuffer(out_buf.data(), iosb.information);
    ASSERT_EQ(restarted.size(), size_t{1});
    EXPECT_EQ(restarted[0].name, walked_names[0]);
  }
}

// Spec: [MS-FSA] 2.1.5.1 — NtCreateFile with an EA buffer persists both
// FILE_NEED_EA and optional EAs across handle close and reopen.
TEST_F(InfoQueriesTest, ExtendedAttributesCreateWithEaBuffer) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const std::vector<uint8_t> req_val = {'v', '1'};
  const std::vector<uint8_t> opt_val = {'o', 'k'};
  std::vector<uint8_t> ea_buf = core::BuildEaBuffer({
      {nt::kFileNeedEa, "req_meta", req_val},
      {0, "opt_meta", opt_val},
  });

  const std::wstring nt_path = TestNtPath(L"ea_create.dat");
  auto created = core::NtCreateFilePath(
      nt_path, FILE_GENERIC_READ | FILE_GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileCreate,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert, nullptr,
      ea_buf.data(), static_cast<ULONG>(ea_buf.size()));
  ASSERT_NT_SUCCESS(created.status);

  auto check_eas = [&](HANDLE h) {
    auto parsed = QueryAllEas(h);
    ASSERT_NT_SUCCESS(parsed.status);
    ASSERT_EQ(parsed.value->size(), size_t{2});
    const core::EaEntry *ea_req = FindEa(*parsed.value, "REQ_META");
    const core::EaEntry *ea_opt = FindEa(*parsed.value, "OPT_META");
    ASSERT_NE(ea_req, nullptr);
    ASSERT_NE(ea_opt, nullptr);
    EXPECT_EQ(ea_req->flags, nt::kFileNeedEa);
    EXPECT_EQ(ea_req->value, req_val);
    EXPECT_EQ(ea_opt->flags, 0u);
    EXPECT_EQ(ea_opt->value, opt_val);
  };

  check_eas(created.value->get());
  ASSERT_FALSE(HasFatalFailure());
  created.value->reset();

  auto reopened = core::CreateWin32File(TestPath(L"ea_create.dat"),
                                        GENERIC_READ, core::kShareAll,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(reopened);
  check_eas(reopened.value->get());
}

// Spec: [MS-FSCC] 2.4.16 — files without EAs return STATUS_NO_EAS_ON_FILE and
// setting an EA with EaValueLength==0 removes it and resets EaSize to 0.
TEST_F(InfoQueriesTest, ExtendedAttributesDeleteAndEmptyQuery) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const std::wstring file_path = TestPath(L"ea_delete.dat");
  auto fh =
      core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);

  auto initial_size = QueryEaSize(fh.value->get());
  ASSERT_NT_SUCCESS(initial_size.status);
  EXPECT_EQ(*initial_size.value, 0u);
  EXPECT_NT_STATUS(QueryAllEas(fh.value->get(), /*buf_size=*/1024).status,
                   nt::kStatusNoEasOnFile);

  // Add an EA, confirm presence.
  ASSERT_NT_SUCCESS(
      SetEaEntries(fh.value->get(), {{0, "temp_ea", {'x', 'y', 'z'}}}));
  auto populated_size = QueryEaSize(fh.value->get());
  ASSERT_NT_SUCCESS(populated_size.status);
  EXPECT_GT(*populated_size.value, 0u);

  // Delete the EA by setting EaValueLength == 0.
  ASSERT_NT_SUCCESS(SetEaEntries(fh.value->get(), {{0, "temp_ea", {}}}));
  auto cleared_size = QueryEaSize(fh.value->get());
  ASSERT_NT_SUCCESS(cleared_size.status);
  EXPECT_EQ(*cleared_size.value, 0u);
  EXPECT_NT_STATUS(QueryAllEas(fh.value->get(), /*buf_size=*/1024).status,
                   nt::kStatusNoEasOnFile);
}

// Spec: [MS-FSCC] 2.4.16 — NtQueryEaFile with a FILE_GET_EA_INFORMATION EaList
// returns exactly the requested subset of a file's EAs.
TEST_F(InfoQueriesTest, ExtendedAttributesEaListQueryReturnsRequestedSubset) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQueryEaFile, nullptr);

  const std::wstring list_path = TestPath(L"ea_list_filter.dat");
  auto fh =
      core::CreateWin32File(list_path, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);

  const std::vector<uint8_t> val_one = {'1', '1', '1'};
  const std::vector<uint8_t> val_two = {'2', '2', '2', '2'};
  const std::vector<uint8_t> val_three = {'3', '3', '3', '3', '3'};
  ASSERT_NT_SUCCESS(
      SetEaEntries(fh.value->get(), {
                                        {0, "ea_one", val_one},
                                        {0, "ea_two", val_two},
                                        {0, "ea_three", val_three},
                                    }));

  std::vector<uint8_t> get_ea_list =
      BuildGetEaListBuffer({"ea_one", "ea_three"});
  std::vector<uint8_t> out_buf(4096, 0);
  nt::IoStatusBlock iosb{};
  const NTSTATUS st = ntdll.NtQueryEaFile(
      fh.value->get(), &iosb, out_buf.data(),
      static_cast<ULONG>(out_buf.size()), /*return_single_entry=*/FALSE,
      get_ea_list.data(), static_cast<ULONG>(get_ea_list.size()), nullptr,
      /*restart_scan=*/TRUE);
  ASSERT_NT_SUCCESS(st);

  const std::vector<core::EaEntry> subset =
      core::ParseEaBuffer(out_buf.data(), iosb.information);
  ASSERT_EQ(subset.size(), size_t{2});
  const core::EaEntry *e1 = FindEa(subset, "EA_ONE");
  const core::EaEntry *e3 = FindEa(subset, "EA_THREE");
  ASSERT_NE(e1, nullptr);
  ASSERT_NE(e3, nullptr);
  EXPECT_EQ(FindEa(subset, "EA_TWO"), nullptr);
  EXPECT_EQ(e1->value, val_one);
  EXPECT_EQ(e3->value, val_three);
}

// Spec: [MS-FSA] 2.1.5.1 — a file whose EAs all lack FILE_NEED_EA can be
// opened with FILE_NO_EA_KNOWLEDGE.
TEST_F(InfoQueriesTest, ExtendedAttributesOptionalEaAllowsNoEaKnowledgeOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const std::wstring file_path = TestPath(L"ea_optional.dat");
  {
    auto fh = core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                                    core::kShareAll, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(fh);
    ASSERT_NT_SUCCESS(
        SetEaEntries(fh.value->get(), {{0, "opt_ea", {'o', 'k'}}}));
    auto ea_size = QueryEaSize(fh.value->get());
    ASSERT_NT_SUCCESS(ea_size.status);
    ASSERT_GT(*ea_size.value, 0u);
  }

  auto opt_open =
      core::NtOpenFilePath(TestNtPath(L"ea_optional.dat"),
                           FILE_READ_DATA | SYNCHRONIZE, core::kShareAll,
                           nt::kFileNoEaKnowledge | nt::kFileNonDirectoryFile |
                               nt::kFileSynchronousIoNonalert);
  EXPECT_NT_SUCCESS(opt_open.status);
}

// Spec: [MS-FSA] 2.1.5.1 — a file carrying a FILE_NEED_EA extended attribute
// rejects opens that specify FILE_NO_EA_KNOWLEDGE with STATUS_ACCESS_DENIED,
// while opens without that option succeed.
TEST_F(InfoQueriesTest, ExtendedAttributesNeedEaBlocksNoEaKnowledgeOpen) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const std::wstring need_nt = TestNtPath(L"ea_need_block.dat");
  std::vector<uint8_t> need_buf =
      core::BuildEaBuffer({{nt::kFileNeedEa, "req_ea", {'!'}}});
  auto created = core::NtCreateFilePath(
      need_nt, FILE_GENERIC_READ | FILE_GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileCreate,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert, nullptr,
      need_buf.data(), static_cast<ULONG>(need_buf.size()));
  ASSERT_NT_SUCCESS(created.status);
  created.value->reset();

  auto blocked = core::NtOpenFilePath(
      need_nt, FILE_READ_DATA | SYNCHRONIZE, core::kShareAll,
      nt::kFileNoEaKnowledge | nt::kFileNonDirectoryFile |
          nt::kFileSynchronousIoNonalert);
  EXPECT_NT_STATUS(blocked.status, STATUS_ACCESS_DENIED);

  auto allowed = core::NtOpenFilePath(
      need_nt, FILE_READ_DATA | SYNCHRONIZE, core::kShareAll,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert);
  EXPECT_NT_SUCCESS(allowed.status);
}

// Spec: [MS-FSCC] 2.4.16 — NtSetEaFile rejects a structurally inconsistent
// FILE_FULL_EA_INFORMATION list with STATUS_EA_LIST_INCONSISTENT and stores
// nothing. (The I/O manager validates the list layout before the request
// reaches the filesystem.)
TEST_F(InfoQueriesTest, ExtendedAttributesInconsistentEaListRejected) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtSetEaFile, nullptr);

  const std::wstring invalid_path = TestPath(L"ea_inconsistent.dat");
  auto fh =
      core::CreateWin32File(invalid_path, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);

  // NextEntryOffset pointing far past buffer end is structurally inconsistent.
  std::vector<uint8_t> bad_buf(32, 0);
  auto *bad_hdr = reinterpret_cast<nt::FileFullEaInfo *>(bad_buf.data());
  bad_hdr->next_entry_offset = 4096;
  bad_hdr->flags = 0;
  bad_hdr->ea_name_length = 3;
  bad_hdr->ea_value_length = 2;
  std::memcpy(bad_hdr->ea_name, "bad\0ok", 6);
  nt::IoStatusBlock iosb{};
  const NTSTATUS bad_st =
      ntdll.NtSetEaFile(fh.value->get(), &iosb, bad_buf.data(),
                        static_cast<ULONG>(bad_buf.size()));
  EXPECT_NT_STATUS(bad_st, nt::kStatusEaListInconsistent);

  auto ea_size = QueryEaSize(fh.value->get());
  ASSERT_NT_SUCCESS(ea_size.status);
  EXPECT_EQ(*ea_size.value, 0u);
}

// Spec: [MS-FSCC] 2.4.16 — NtSetEaFile rejects a well-formed EA list whose
// packed size exceeds NTFS's 64 KiB per-file EA limit with
// STATUS_EA_TOO_LARGE and stores nothing.
TEST_F(InfoQueriesTest, ExtendedAttributesOversizedEaSetRejected) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtSetEaFile, nullptr);

  const std::wstring big_path = TestPath(L"ea_oversized.dat");
  auto fh =
      core::CreateWin32File(big_path, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);

  const std::vector<uint8_t> large_chunk(40000, 0x41);
  std::vector<uint8_t> oversized = core::BuildEaBuffer({
      {0, "big_ea_1", large_chunk},
      {0, "big_ea_2", large_chunk},
  });
  ASSERT_GT(oversized.size(), size_t{65536});
  nt::IoStatusBlock big_iosb{};
  const NTSTATUS big_st =
      ntdll.NtSetEaFile(fh.value->get(), &big_iosb, oversized.data(),
                        static_cast<ULONG>(oversized.size()));
  EXPECT_NT_STATUS(big_st, nt::kStatusEaTooLarge);

  auto ea_size = QueryEaSize(fh.value->get());
  ASSERT_NT_SUCCESS(ea_size.status);
  EXPECT_EQ(*ea_size.value, 0u);
}

// Spec: [MS-FSCC] 2.4.16 — an EA name containing a character that is not
// permitted in EA names ('?' here) is rejected with STATUS_INVALID_EA_NAME
// and nothing is stored.
TEST_F(InfoQueriesTest, ExtendedAttributesInvalidEaNameRejected) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const std::wstring file_path = TestPath(L"ea_bad_name.dat");
  auto fh =
      core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                            core::kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);

  EXPECT_NT_STATUS(SetEaEntries(fh.value->get(), {{0, "bad?name", {'x'}}}),
                   STATUS_INVALID_EA_NAME);

  auto ea_size = QueryEaSize(fh.value->get());
  ASSERT_NT_SUCCESS(ea_size.status);
  EXPECT_EQ(*ea_size.value, 0u);
}

// Spec: [MS-FSCC] 2.4.16 — directories carry extended attributes exactly like
// files: an EA set on a directory handle is returned (upper-cased name,
// unchanged value) by NtQueryEaFile and counted by FileEaInformation.
TEST_F(InfoQueriesTest, ExtendedAttributesOnDirectory) {
  REQUIRE_FS_CAPABILITY(FsCapability::kExtendedAttributes);

  const std::wstring dir_path = TestPath(L"ea_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_path.c_str(), nullptr));
  auto dh = core::CreateWin32File(
      dir_path,
      FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      core::kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  ASSERT_WIN32_OK(dh);

  const std::vector<uint8_t> dir_val = {'d', 'i', 'r', 0x00, 0xFF};
  ASSERT_NT_SUCCESS(SetEaEntries(dh.value->get(), {{0, "dir_meta", dir_val}}));

  auto ea_size = QueryEaSize(dh.value->get());
  ASSERT_NT_SUCCESS(ea_size.status);
  EXPECT_GT(*ea_size.value, 0u);

  auto all_eas = QueryAllEas(dh.value->get());
  ASSERT_NT_SUCCESS(all_eas.status);
  ASSERT_EQ(all_eas.value->size(), size_t{1});
  const core::EaEntry *ea = FindEa(*all_eas.value, "DIR_META");
  ASSERT_NE(ea, nullptr);
  EXPECT_EQ(ea->value, dir_val);
}

// Spec: FSCTL_CREATE_OR_GET/GET/DELETE_OBJECT_ID manage 16-byte Object IDs
// that resolve via OpenFileById(ObjectIdType) without deleting the file.
TEST_F(InfoQueriesTest, ObjectIdCreateGetAndOpenByObjectId) {
  REQUIRE_FS_CAPABILITY(FsCapability::kObjectIds);
  REQUIRE_LOCAL_VOLUME();

  const std::wstring file_path = TestPath(L"oid_target.dat");
  const std::string payload = "object-id-payload";
  ASSERT_WIN32_OK(core::WriteFileContents(file_path, payload));

  auto fh = core::CreateWin32File(file_path, GENERIC_READ | GENERIC_WRITE,
                                  core::kShareAll, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL);
  ASSERT_WIN32_OK(fh);

  auto orig_idx = core::GetFileIndex(fh.value->get());
  ASSERT_WIN32_OK(orig_idx);

  // Fresh file has no Object ID yet.
  {
    FILE_OBJECTID_BUFFER probe{};
    DWORD returned = 0;
    EXPECT_WIN32_ERROR(::DeviceIoControl(fh.value->get(), FSCTL_GET_OBJECT_ID,
                                         nullptr, 0, &probe, sizeof(probe),
                                         &returned, nullptr),
                       ERROR_FILE_NOT_FOUND);
  }

  // Create or get Object ID.
  FILE_OBJECTID_BUFFER created_oid{};
  {
    DWORD returned = 0;
    ASSERT_WIN32_TRUE(::DeviceIoControl(
        fh.value->get(), FSCTL_CREATE_OR_GET_OBJECT_ID, nullptr, 0,
        &created_oid, sizeof(created_oid), &returned, nullptr));
    EXPECT_EQ(returned, static_cast<DWORD>(sizeof(FILE_OBJECTID_BUFFER)));
    EXPECT_FALSE(helpers::AllBytesEqual(created_oid.ObjectId,
                                        sizeof(created_oid.ObjectId), 0));
  }

  // FSCTL_GET_OBJECT_ID returns the exact same 16-byte ObjectId.
  {
    FILE_OBJECTID_BUFFER fetched_oid{};
    DWORD returned = 0;
    ASSERT_WIN32_TRUE(::DeviceIoControl(
        fh.value->get(), FSCTL_GET_OBJECT_ID, nullptr, 0, &fetched_oid,
        sizeof(fetched_oid), &returned, nullptr));
    EXPECT_EQ(std::memcmp(created_oid.ObjectId, fetched_oid.ObjectId,
                          sizeof(created_oid.ObjectId)),
              0);
  }

  // OpenFileById with ObjectIdType opens the exact same file.
  {
    auto dir_h = core::CreateWin32File(
        TempDir().ExtendedPath(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        core::kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_WIN32_OK(dir_h);

    FILE_ID_DESCRIPTOR desc{};
    desc.dwSize = sizeof(desc);
    desc.Type = ObjectIdType;
    static_assert(sizeof(desc.ObjectId) == sizeof(created_oid.ObjectId));
    std::memcpy(&desc.ObjectId, created_oid.ObjectId, sizeof(desc.ObjectId));

    ::SetLastError(ERROR_SUCCESS);
    core::UniqueHandle h_oid(::OpenFileById(
        dir_h.value->get(), &desc, GENERIC_READ, core::kShareAll, nullptr, 0));
    const DWORD open_err = ::GetLastError();
    ASSERT_TRUE(h_oid) << "OpenFileById(ObjectIdType) failed: "
                       << core::FormatWin32Error(open_err);

    auto rd = core::ReadAllBytes(h_oid.get());
    ASSERT_WIN32_OK(rd);
    EXPECT_EQ(*rd.value, payload);

    auto idx = core::GetFileIndex(h_oid.get());
    ASSERT_WIN32_OK(idx);
    EXPECT_EQ(*idx.value, *orig_idx.value);
  }

  // Delete Object ID metadata only; underlying file survives intact.
  {
    DWORD returned = 0;
    ASSERT_WIN32_TRUE(::DeviceIoControl(fh.value->get(), FSCTL_DELETE_OBJECT_ID,
                                        nullptr, 0, nullptr, 0, &returned,
                                        nullptr));
  }
  {
    FILE_OBJECTID_BUFFER after_del{};
    DWORD returned = 0;
    EXPECT_WIN32_ERROR(::DeviceIoControl(fh.value->get(), FSCTL_GET_OBJECT_ID,
                                         nullptr, 0, &after_del,
                                         sizeof(after_del), &returned, nullptr),
                       ERROR_FILE_NOT_FOUND);
  }
  fh.value->reset();

  EXPECT_TRUE(helpers::CheckFileContents(file_path, payload));
}

void ForceLink_InfoQueriesNotifyEa() {}

} // namespace fstest
