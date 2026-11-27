// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#ifndef TESTS_NP_UNIT_NP_TEST_SUPPORT_H_
#define TESTS_NP_UNIT_NP_TEST_SUPPORT_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dokan.h"
#include <winnetwk.h>
#include <npapi.h>

#include "np_mount_table.h"
#include "np_core.h"
#include "np_transport_live.h"
#include "np_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace dokan_np_test {

// Allocates virtual memory with a trailing PAGE_NOACCESS guard page.
// The writable region of `size_bytes` is placed immediately before the guard
// page (aligned up to `align` when `align > 1`, with trailing canary bytes
// between `size_bytes` and the guard page), so any 1-byte OOB write/read past
// the aligned boundary faults immediately with STATUS_ACCESS_VIOLATION, and
// any OOB write in the alignment slack is caught by CheckCanaries().
class PageGuardedBuffer {
public:
  PageGuardedBuffer(size_t size_bytes, size_t align = 1,
                    uint8_t fill_byte = 0xCD)
      : size_bytes_(size_bytes), align_(align == 0 ? 1 : align) {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const size_t page_size = si.dwPageSize ? si.dwPageSize : 4096;
    const size_t aligned_size = ((size_bytes_ + align_ - 1) / align_) * align_;
    const size_t data_pages =
        aligned_size == 0 ? 1 : ((aligned_size + page_size - 1) / page_size);
    const size_t data_bytes = data_pages * page_size;
    total_bytes_ = data_bytes + page_size;

    base_ = static_cast<uint8_t *>(::VirtualAlloc(
        nullptr, total_bytes_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (base_ != nullptr) {
      DWORD old_prot = 0;
      if (!::VirtualProtect(base_ + data_bytes, page_size, PAGE_NOACCESS,
                            &old_prot)) {
        ::VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
        return;
      }
      data_end_ = base_ + data_bytes;
      user_ptr_ = data_end_ - aligned_size;
      slack_bytes_ = aligned_size - size_bytes_;
      // Fill preceding slack on the page and trailing alignment slack with
      // 0xA5 canary bytes, and user region with fill_byte.
      std::memset(base_, 0xA5, static_cast<size_t>(user_ptr_ - base_));
      if (size_bytes_ > 0) {
        std::memset(user_ptr_, fill_byte, size_bytes_);
      }
      if (slack_bytes_ > 0) {
        std::memset(user_ptr_ + size_bytes_, 0xA5, slack_bytes_);
      }
    }
  }

  ~PageGuardedBuffer() {
    if (base_ != nullptr) {
      ::VirtualFree(base_, 0, MEM_RELEASE);
    }
  }

  PageGuardedBuffer(const PageGuardedBuffer &) = delete;
  PageGuardedBuffer &operator=(const PageGuardedBuffer &) = delete;

  void *data() const { return user_ptr_; }
  size_t size() const { return size_bytes_; }
  bool valid() const { return base_ != nullptr; }

  bool CheckCanaries() const {
    if (base_ == nullptr) {
      return false;
    }
    for (const uint8_t *p = base_; p < user_ptr_; ++p) {
      if (*p != 0xA5) {
        return false;
      }
    }
    for (size_t i = 0; i < slack_bytes_; ++i) {
      if (user_ptr_[size_bytes_ + i] != 0xA5) {
        return false;
      }
    }
    return true;
  }

private:
  uint8_t *base_ = nullptr;
  uint8_t *data_end_ = nullptr;
  uint8_t *user_ptr_ = nullptr;
  size_t size_bytes_ = 0;
  size_t align_ = 1;
  size_t slack_bytes_ = 0;
  size_t total_bytes_ = 0;
};

// Exact-sized HeapAlloc buffer (friendly to PageHeap / gflags /full).
class HeapGuardedBuffer {
public:
  explicit HeapGuardedBuffer(size_t size_bytes, uint8_t fill_byte = 0xCD)
      : size_bytes_(size_bytes) {
    ptr_ = static_cast<uint8_t *>(
        ::HeapAlloc(::GetProcessHeap(), 0, size_bytes == 0 ? 1 : size_bytes));
    if (ptr_ != nullptr && size_bytes > 0) {
      std::memset(ptr_, fill_byte, size_bytes);
    }
  }

  ~HeapGuardedBuffer() {
    if (ptr_ != nullptr) {
      ::HeapFree(::GetProcessHeap(), 0, ptr_);
    }
  }

  HeapGuardedBuffer(const HeapGuardedBuffer &) = delete;
  HeapGuardedBuffer &operator=(const HeapGuardedBuffer &) = delete;

  void *data() const { return size_bytes_ == 0 ? nullptr : ptr_; }
  void *nonnull_data() const { return ptr_; }
  size_t size() const { return size_bytes_; }

private:
  uint8_t *ptr_ = nullptr;
  size_t size_bytes_ = 0;
};

// Places an unterminated wide string right before a PAGE_NOACCESS guard page.
class GuardedUnterminatedWide {
public:
  explicit GuardedUnterminatedWide(std::wstring_view text)
      : buf_(text.size() * sizeof(wchar_t), sizeof(wchar_t), 0),
        len_(text.size()) {
    if (buf_.valid() && !text.empty()) {
      std::memcpy(buf_.data(), text.data(), text.size() * sizeof(wchar_t));
    }
  }

  const wchar_t *data() const {
    return static_cast<const wchar_t *>(buf_.data());
  }
  size_t length() const { return len_; }
  bool valid() const { return buf_.valid(); }

private:
  PageGuardedBuffer buf_;
  size_t len_ = 0;
};

struct FakeMountSpec {
  std::wstring remote_name;
  WCHAR drive_letter = 0;
  ULONG flags = 0;
  std::wstring transport_key;
};

struct RecordedCancel {
  std::wstring remote_name;
  WCHAR drive_letter = 0;
  ULONG flags = 0;
  std::wstring transport_key;
  BOOL force = FALSE;
};

struct RecordedAlias {
  std::wstring target_remote_name;
  WCHAR target_drive_letter = 0;
  ULONG target_flags = 0;
  std::wstring target_transport_key;
  WCHAR alias_drive_letter = 0;
};

class FakeTransport {
public:
  std::vector<FakeMountSpec> mounts;
  DWORD query_status = WN_SUCCESS;
  DWORD cancel_status = WN_SUCCESS;
  DWORD add_alias_status = WN_SUCCESS;
  bool remove_on_cancel = true;
  bool add_entry_on_alias = true;
  bool enable_add_drive_alias = true;
  ULONG query_count = 0;
  std::vector<RecordedCancel> cancels;
  std::vector<RecordedAlias> aliases;

  // Optional hook invoked during QueryMounts (for testing mutation races).
  void (*on_query_hook)(FakeTransport *self) = nullptr;

  DOKAN_NP_TRANSPORT MakeTransport() {
    DOKAN_NP_TRANSPORT t{};
    t.QueryMounts = &FakeTransport::QueryMountsThunk;
    t.CancelMount = &FakeTransport::CancelMountThunk;
    t.AddDriveAlias =
        enable_add_drive_alias ? &FakeTransport::AddDriveAliasThunk : nullptr;
    t.Context = this;
    return t;
  }

private:
  static DWORD QueryMountsThunk(PVOID ctx, DOKAN_NP_MOUNT_TABLE **table) {
    auto *self = static_cast<FakeTransport *>(ctx);
    ++self->query_count;
    if (self->on_query_hook != nullptr) {
      self->on_query_hook(self);
    }
    if (self->query_status != WN_SUCCESS) {
      *table = nullptr;
      return self->query_status;
    }

    DOKAN_NP_TABLE_BUILDER *builder = nullptr;
    DWORD err = DokanNpTableBuilderCreate(&builder);
    if (err != ERROR_SUCCESS) {
      return WN_OUT_OF_MEMORY;
    }
    for (const auto &m : self->mounts) {
      err = DokanNpTableBuilderAdd(
          builder, m.remote_name.data(), m.remote_name.size(), m.drive_letter,
          m.flags, m.transport_key.data(), m.transport_key.size());
      if (err != ERROR_SUCCESS && err != ERROR_INVALID_NAME) {
        DokanNpTableBuilderAbort(builder);
        return WN_OUT_OF_MEMORY;
      }
    }
    err = DokanNpTableBuilderFinish(builder, table);
    if (err != ERROR_SUCCESS) {
      return WN_OUT_OF_MEMORY;
    }
    return WN_SUCCESS;
  }

  static DWORD CancelMountThunk(PVOID ctx, const DOKAN_NP_MOUNT *mount,
                                BOOL force) {
    auto *self = static_cast<FakeTransport *>(ctx);
    RecordedCancel rec;
    if (mount != nullptr) {
      rec.remote_name = mount->RemoteName ? mount->RemoteName : L"";
      rec.drive_letter = mount->DriveLetter;
      rec.flags = mount->Flags;
      rec.transport_key = mount->TransportKey ? mount->TransportKey : L"";
    }
    rec.force = force;
    self->cancels.push_back(rec);

    if (self->cancel_status == WN_SUCCESS && self->remove_on_cancel &&
        mount != nullptr) {
      const auto it =
          std::find_if(self->mounts.begin(), self->mounts.end(),
                       [&](const FakeMountSpec &spec) {
                         return spec.drive_letter == mount->DriveLetter &&
                                spec.transport_key == rec.transport_key &&
                                spec.flags == mount->Flags;
                       });
      if (it != self->mounts.end()) {
        self->mounts.erase(it);
      }
    }
    return self->cancel_status;
  }

  static DWORD AddDriveAliasThunk(PVOID ctx, const DOKAN_NP_MOUNT *target,
                                  WCHAR drive_letter) {
    auto *self = static_cast<FakeTransport *>(ctx);
    RecordedAlias rec;
    if (target != nullptr) {
      rec.target_remote_name = target->RemoteName ? target->RemoteName : L"";
      rec.target_drive_letter = target->DriveLetter;
      rec.target_flags = target->Flags;
      rec.target_transport_key =
          target->TransportKey ? target->TransportKey : L"";
    }
    rec.alias_drive_letter = drive_letter;
    self->aliases.push_back(rec);

    if (self->add_alias_status == WN_SUCCESS && self->add_entry_on_alias &&
        target != nullptr) {
      FakeMountSpec alias;
      alias.remote_name = rec.target_remote_name;
      alias.drive_letter = drive_letter;
      alias.flags = DOKAN_NP_MOUNT_CANCELABLE | DOKAN_NP_MOUNT_ALIAS;
      alias.transport_key = std::wstring(1, drive_letter) + L":";
      self->mounts.push_back(std::move(alias));
    }
    return self->add_alias_status;
  }
};

// Verifies every non-NULL string pointer in `nr` lies completely (including NUL)
// inside `[buf_start, buf_start + buf_size)`, is WCHAR-aligned, and does not
// overlap the NETRESOURCEW array `[buf_start, buf_start + count * sizeof(NETRESOURCEW))`.
inline void AssertNetResourcePointersContained(const NETRESOURCEW *nr,
                                               DWORD count,
                                               const void *buf_start,
                                               DWORD buf_size) {
  const auto start_addr = reinterpret_cast<uintptr_t>(buf_start);
  const auto end_addr = start_addr + buf_size;
  const auto array_end_addr = start_addr + count * sizeof(NETRESOURCEW);
  ASSERT_LE(array_end_addr, end_addr);

  auto check_str = [&](LPCWSTR s, const char *field) {
    if (s == nullptr) {
      return;
    }
    const auto addr = reinterpret_cast<uintptr_t>(s);
    EXPECT_EQ(addr % alignof(WCHAR), 0u) << field;
    EXPECT_GE(addr, array_end_addr) << field;
    EXPECT_LT(addr, end_addr) << field;
    const size_t max_chars = (end_addr - addr) / sizeof(WCHAR);
    const size_t len = wcsnlen(s, max_chars);
    EXPECT_LT(len, max_chars) << field << " missing NUL inside buffer";
  };

  for (DWORD i = 0; i < count; ++i) {
    check_str(nr[i].lpLocalName, "lpLocalName");
    check_str(nr[i].lpRemoteName, "lpRemoteName");
    check_str(nr[i].lpComment, "lpComment");
    check_str(nr[i].lpProvider, "lpProvider");
  }
}

} // namespace dokan_np_test

#endif // TESTS_NP_UNIT_NP_TEST_SUPPORT_H_
