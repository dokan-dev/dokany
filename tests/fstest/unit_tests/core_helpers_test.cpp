// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/capabilities.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace fstest::core {
namespace {

std::wstring GetHostTempDir() {
  wchar_t buf[MAX_PATH + 1]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, buf);
  return (len > 0 && len < MAX_PATH)
             ? NormalizeWin32Path(std::wstring_view(buf, len))
             : L"";
}

template <typename T>
void StoreAt(std::vector<uint8_t> *buf, size_t offset, T v) {
  ASSERT_LE(offset + sizeof(T), buf->size());
  std::memcpy(buf->data() + offset, &v, sizeof(T));
}

void StoreWide(std::vector<uint8_t> *buf, size_t offset,
               std::wstring_view text) {
  ASSERT_LE(offset + text.size() * sizeof(wchar_t), buf->size());
  std::memcpy(buf->data() + offset, text.data(), text.size() * sizeof(wchar_t));
}

// Two-entry FILE_STREAM_INFORMATION chain: "::$DATA" (size 9) followed by
// ":alt:$DATA" (size 12). Returns the offset of the second entry.
size_t BuildStreamInfo(std::vector<uint8_t> *buf) {
  constexpr size_t kHeader = offsetof(nt::FileStreamInfo, stream_name);
  const std::wstring_view first = L"::$DATA";
  const std::wstring_view second = L":alt:$DATA";
  const size_t second_offset =
      (kHeader + first.size() * sizeof(wchar_t) + 7u) & ~size_t{7};
  buf->assign(second_offset + kHeader + second.size() * sizeof(wchar_t), 0);
  auto put_entry = [&](size_t at, ULONG next, std::wstring_view name,
                       LONGLONG size) {
    StoreAt<ULONG>(buf, at + offsetof(nt::FileStreamInfo, next_entry_offset),
                   next);
    StoreAt<ULONG>(buf, at + offsetof(nt::FileStreamInfo, stream_name_length),
                   static_cast<ULONG>(name.size() * sizeof(wchar_t)));
    StoreAt<LONGLONG>(buf, at + offsetof(nt::FileStreamInfo, stream_size),
                      size);
    StoreAt<LONGLONG>(
        buf, at + offsetof(nt::FileStreamInfo, stream_allocation_size), 4096);
    StoreWide(buf, at + kHeader, name);
  };
  put_entry(0, static_cast<ULONG>(second_offset), first, 9);
  put_entry(second_offset, 0, second, 12);
  return second_offset;
}

// FILE_LINKS_INFORMATION with `entries_returned` and two link entries
// ("one.txt" under parent 5, "two.txt" under parent 7). Returns the offset of
// the first entry's NextEntryOffset field.
size_t BuildHardLinkInfo(std::vector<uint8_t> *buf, ULONG entries_returned) {
  constexpr size_t kLinksHeader = offsetof(nt::FileLinksInfo, entry);
  constexpr size_t kEntryHeader = offsetof(nt::FileLinkEntryInfo, file_name);
  const std::wstring_view first = L"one.txt";
  const std::wstring_view second = L"two.txt";
  const size_t first_size =
      (kEntryHeader + first.size() * sizeof(wchar_t) + 7u) & ~size_t{7};
  buf->assign(kLinksHeader + first_size + kEntryHeader +
                  second.size() * sizeof(wchar_t),
              0);
  StoreAt<ULONG>(buf, offsetof(nt::FileLinksInfo, bytes_needed),
                 static_cast<ULONG>(buf->size()));
  StoreAt<ULONG>(buf, offsetof(nt::FileLinksInfo, entries_returned),
                 entries_returned);
  auto put_entry = [&](size_t at, ULONG next, LONGLONG parent,
                       std::wstring_view name) {
    StoreAt<ULONG>(buf, at + offsetof(nt::FileLinkEntryInfo, next_entry_offset),
                   next);
    StoreAt<LONGLONG>(buf, at + offsetof(nt::FileLinkEntryInfo, parent_file_id),
                      parent);
    StoreAt<ULONG>(buf, at + offsetof(nt::FileLinkEntryInfo, file_name_length),
                   static_cast<ULONG>(name.size()));
    StoreWide(buf, at + kEntryHeader, name);
  };
  put_entry(kLinksHeader, static_cast<ULONG>(first_size), 5, first);
  put_entry(kLinksHeader + first_size, 0, 7, second);
  return kLinksHeader;
}

// Returns whether `privilege_name` is present in `token`, and if so whether it
// is currently enabled.
struct PrivilegeState {
  bool present = false;
  bool enabled = false;
};

PrivilegeState QueryPrivilegeState(HANDLE token,
                                   const wchar_t *privilege_name) {
  PrivilegeState state;
  LUID luid{};
  if (::LookupPrivilegeValueW(nullptr, privilege_name, &luid) == FALSE) {
    return state;
  }
  DWORD needed = 0;
  ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &needed);
  if (needed == 0) {
    return state;
  }
  std::vector<uint8_t> buf(needed, 0);
  if (::GetTokenInformation(token, TokenPrivileges, buf.data(), needed,
                            &needed) == FALSE) {
    return state;
  }
  const auto *privs = reinterpret_cast<const TOKEN_PRIVILEGES *>(buf.data());
  for (DWORD i = 0; i < privs->PrivilegeCount; ++i) {
    const LUID_AND_ATTRIBUTES &p = privs->Privileges[i];
    if (p.Luid.LowPart == luid.LowPart && p.Luid.HighPart == luid.HighPart) {
      state.present = true;
      state.enabled = (p.Attributes & SE_PRIVILEGE_ENABLED) != 0;
    }
  }
  return state;
}

PrivilegeState QueryProcessPrivilegeState(const wchar_t *privilege_name) {
  HANDLE raw = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) == FALSE) {
    return {};
  }
  UniqueHandle token(raw);
  return QueryPrivilegeState(token.get(), privilege_name);
}

TEST(CoreHelpersTest, EaBufferEmptyAndSingleEntries) {
  EXPECT_TRUE(BuildEaBuffer({}).empty());
  EXPECT_TRUE(ParseEaBuffer(nullptr, 0).empty());
  EXPECT_TRUE(ParseEaBuffer(nullptr, 64).empty());

  const uint8_t dummy = 0;
  EXPECT_TRUE(ParseEaBuffer(&dummy, 0).empty());

  // Single entry with kFileNeedEa flag and binary payload containing NULs.
  const std::vector<EaEntry> single = {
      {nt::kFileNeedEa, "NEED_KEY", {0x00, 0xFF, 0x00, 0x42, 0x7F}},
  };
  const std::vector<uint8_t> raw = BuildEaBuffer(single);
  const size_t expected_size = offsetof(nt::FileFullEaInfo, ea_name) +
                               single[0].name.size() + 1 +
                               single[0].value.size();
  ASSERT_EQ(raw.size(), expected_size);

  const auto *hdr = reinterpret_cast<const nt::FileFullEaInfo *>(raw.data());
  EXPECT_EQ(hdr->next_entry_offset, 0UL);
  EXPECT_EQ(hdr->flags, nt::kFileNeedEa);
  EXPECT_EQ(hdr->ea_name_length, static_cast<UCHAR>(single[0].name.size()));
  EXPECT_EQ(hdr->ea_value_length, static_cast<USHORT>(single[0].value.size()));
  EXPECT_EQ(hdr->ea_name[single[0].name.size()], '\0');

  const std::vector<EaEntry> parsed = ParseEaBuffer(raw.data(), raw.size());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].flags, nt::kFileNeedEa);
  EXPECT_EQ(parsed[0].name, "NEED_KEY");
  EXPECT_EQ(parsed[0].value, single[0].value);
}

TEST(CoreHelpersTest, EaBufferMultipleEntriesAndFourByteAlignment) {
  // Entry 0: raw = 8 + 1 + 1 + 1 = 11 -> padded to 12 (1 zero pad byte)
  // Entry 1: raw = 8 + 2 + 1 + 2 = 13 -> padded to 16 (3 zero pad bytes)
  // Entry 2: raw = 8 + 3 + 1 + 0 = 12 -> already 4-byte aligned -> 12
  // Entry 3 (last): raw = 8 + 1 + 1 + 1 = 11 -> unpadded final entry
  const std::vector<EaEntry> entries = {
      {0, "A", {0x11}},
      {nt::kFileNeedEa, "BB", {0x22, 0x33}},
      {0, "CCC", {}},
      {nt::kFileNeedEa, "D", {0x44}},
  };

  const std::vector<uint8_t> raw = BuildEaBuffer(entries);
  ASSERT_EQ(raw.size(), 12u + 16u + 12u + 11u);

  const auto *hdr0 = reinterpret_cast<const nt::FileFullEaInfo *>(raw.data());
  EXPECT_EQ(hdr0->next_entry_offset, 12UL);
  EXPECT_EQ(hdr0->next_entry_offset % 4UL, 0UL);
  EXPECT_EQ(raw[11], 0u);

  const auto *hdr1 =
      reinterpret_cast<const nt::FileFullEaInfo *>(raw.data() + 12);
  EXPECT_EQ(hdr1->next_entry_offset, 16UL);
  EXPECT_EQ(hdr1->next_entry_offset % 4UL, 0UL);
  EXPECT_EQ(hdr1->flags, nt::kFileNeedEa);
  EXPECT_EQ(raw[25], 0u);
  EXPECT_EQ(raw[26], 0u);
  EXPECT_EQ(raw[27], 0u);

  const auto *hdr2 =
      reinterpret_cast<const nt::FileFullEaInfo *>(raw.data() + 28);
  EXPECT_EQ(hdr2->next_entry_offset, 12UL);
  EXPECT_EQ(hdr2->ea_value_length, 0u);

  const auto *hdr3 =
      reinterpret_cast<const nt::FileFullEaInfo *>(raw.data() + 40);
  EXPECT_EQ(hdr3->next_entry_offset, 0UL);
  EXPECT_EQ(hdr3->flags, nt::kFileNeedEa);

  const std::vector<EaEntry> parsed = ParseEaBuffer(raw.data(), raw.size());
  ASSERT_EQ(parsed.size(), entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    EXPECT_EQ(parsed[i].flags, entries[i].flags);
    EXPECT_EQ(parsed[i].name, entries[i].name);
    EXPECT_EQ(parsed[i].value, entries[i].value);
  }
}

TEST(CoreHelpersTest, EaBufferTruncatedAndCorruptedBufferSafety) {
  const std::vector<uint8_t> valid = BuildEaBuffer({
      {0, "FIRST", {'1', '2'}},
      {nt::kFileNeedEa, "SECOND", {'3', '4', '5'}},
  });
  ASSERT_GT(valid.size(), 16u);
  const auto *hdr0 = reinterpret_cast<const nt::FileFullEaInfo *>(valid.data());
  const size_t second_offset = hdr0->next_entry_offset;
  ASSERT_GT(second_offset, 0u);
  ASSERT_LT(second_offset, valid.size());

  // Header shorter than or equal to fixed prefix yields empty result.
  for (size_t len = 1; len <= offsetof(nt::FileFullEaInfo, ea_name); ++len) {
    EXPECT_TRUE(ParseEaBuffer(valid.data(), len).empty()) << "len=" << len;
  }

  // Truncation inside first entry's value yields empty result.
  EXPECT_TRUE(ParseEaBuffer(valid.data(), second_offset - 2).empty());

  // Truncation inside second entry's header or value returns only first entry.
  {
    const auto parsed = ParseEaBuffer(valid.data(), second_offset + 4);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].name, "FIRST");
  }
  {
    const auto parsed = ParseEaBuffer(valid.data(), valid.size() - 1);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].name, "FIRST");
  }

  // Corrupted ea_value_length exceeding remaining buffer size stops safely.
  {
    std::vector<uint8_t> bad_val = valid;
    auto *hdr = reinterpret_cast<nt::FileFullEaInfo *>(bad_val.data());
    hdr->ea_value_length = 4096;
    EXPECT_TRUE(ParseEaBuffer(bad_val.data(), bad_val.size()).empty());
  }

  // Corrupted ea_name_length exceeding remaining buffer size stops safely.
  {
    std::vector<uint8_t> bad_name = valid;
    auto *hdr = reinterpret_cast<nt::FileFullEaInfo *>(bad_name.data());
    hdr->ea_name_length = 250;
    EXPECT_TRUE(ParseEaBuffer(bad_name.data(), bad_name.size()).empty());
  }

  // Corrupted next_entry_offset jumping past buffer end stops after 1st entry.
  {
    std::vector<uint8_t> bad_next = valid;
    auto *hdr = reinterpret_cast<nt::FileFullEaInfo *>(bad_next.data());
    hdr->next_entry_offset = static_cast<ULONG>(bad_next.size() + 64);
    const auto parsed = ParseEaBuffer(bad_next.data(), bad_next.size());
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].name, "FIRST");
  }

  // Corrupted next_entry_offset near ULONG max must not wrap size_t offset.
  for (const ULONG huge_offset : {0xFFFFFFFFUL, 0xFFFFFFF0UL, 0x80000000UL}) {
    std::vector<uint8_t> overflow_next = valid;
    auto *hdr = reinterpret_cast<nt::FileFullEaInfo *>(overflow_next.data());
    hdr->next_entry_offset = huge_offset;
    const auto parsed =
        ParseEaBuffer(overflow_next.data(), overflow_next.size());
    ASSERT_EQ(parsed.size(), 1u) << "huge_offset=0x" << std::hex << huge_offset;
    EXPECT_EQ(parsed[0].name, "FIRST");
  }
}

TEST(CoreHelpersTest, UtcFileTimeAndBitPackingConversions) {
  constexpr uint64_t kTicksPerSecond = 10000000ULL;
  constexpr uint64_t kUnixEpochFileTime = 116444736000000000ULL;

  // Windows FILETIME epoch: 1601-01-01T00:00:00Z == 0.
  const FILETIME win_epoch = MakeUtcFileTime(1601, 1, 1, 0, 0, 0);
  EXPECT_EQ(win_epoch.dwLowDateTime, 0UL);
  EXPECT_EQ(win_epoch.dwHighDateTime, 0UL);
  EXPECT_EQ(FileTimeToU64(win_epoch), 0ULL);
  EXPECT_EQ(FileTimeToInt64(win_epoch), 0LL);
  EXPECT_EQ(MakeUtcFileTimeInt64(1601, 1, 1), 0LL);
  EXPECT_EQ(FileTimeToU64(MakeUtcFileTime(1601, 1, 1, 0, 0, 1)),
            kTicksPerSecond);

  // Unix epoch: 1970-01-01T00:00:00Z.
  const FILETIME unix_epoch = MakeUtcFileTime(1970, 1, 1);
  EXPECT_EQ(FileTimeToU64(unix_epoch), kUnixEpochFileTime);
  EXPECT_EQ(MakeUtcFileTimeInt64(1970, 1, 1, 0, 0, 0),
            static_cast<int64_t>(kUnixEpochFileTime));

  // Round-trip via SystemTime and arithmetic spacing.
  const FILETIME sample = MakeUtcFileTime(2026, 6, 15, 14, 30, 45);
  SYSTEMTIME st{};
  ASSERT_NE(::FileTimeToSystemTime(&sample, &st), FALSE);
  EXPECT_EQ(st.wYear, 2026u);
  EXPECT_EQ(st.wMonth, 6u);
  EXPECT_EQ(st.wDay, 15u);
  EXPECT_EQ(st.wHour, 14u);
  EXPECT_EQ(st.wMinute, 30u);
  EXPECT_EQ(st.wSecond, 45u);
  EXPECT_EQ(st.wMilliseconds, 0u);

  const int64_t t0 = MakeUtcFileTimeInt64(2026, 6, 15, 10, 0, 0);
  const int64_t t1 = MakeUtcFileTimeInt64(2026, 6, 15, 11, 0, 0);
  const int64_t t2 = MakeUtcFileTimeInt64(2026, 6, 16, 10, 0, 0);
  EXPECT_EQ(t1 - t0, 3600LL * static_cast<int64_t>(kTicksPerSecond));
  EXPECT_EQ(t2 - t0, 86400LL * static_cast<int64_t>(kTicksPerSecond));

  // Invalid date returns zero FILETIME cleanly.
  const FILETIME invalid = MakeUtcFileTime(2026, 13, 40);
  EXPECT_EQ(FileTimeToU64(invalid), 0ULL);

  // Direct FileTimeToU64 / FileTimeToInt64 bit packing.
  FILETIME packed{};
  packed.dwLowDateTime = 0x89ABCDEFUL;
  packed.dwHighDateTime = 0x01234567UL;
  EXPECT_EQ(FileTimeToU64(packed), 0x0123456789ABCDEFULL);
  EXPECT_EQ(FileTimeToInt64(packed), 0x0123456789ABCDEFLL);

  FILETIME high_bit{};
  high_bit.dwLowDateTime = 0x00000001UL;
  high_bit.dwHighDateTime = 0x80000000UL;
  EXPECT_EQ(FileTimeToU64(high_bit), 0x8000000000000001ULL);
  EXPECT_LT(FileTimeToInt64(high_bit), 0LL);
}

TEST(CoreHelpersTest, OrdinalStringComparisonHelpers) {
  // EqualsIgnoreCase
  EXPECT_TRUE(EqualsIgnoreCase(L"", L""));
  EXPECT_FALSE(EqualsIgnoreCase(L"", L"a"));
  EXPECT_FALSE(EqualsIgnoreCase(L"abc", L"ab"));
  EXPECT_TRUE(EqualsIgnoreCase(L"NtFs.TxT", L"ntfs.txt"));
  EXPECT_TRUE(EqualsIgnoreCase(L"Caf\u00E9", L"CAF\u00C9"));
  EXPECT_FALSE(EqualsIgnoreCase(L"abc", L"abd"));

  // EndsWithIgnoreCase
  EXPECT_TRUE(EndsWithIgnoreCase(L"", L""));
  EXPECT_TRUE(EndsWithIgnoreCase(L"C:\\Dir\\File.DAT", L""));
  EXPECT_TRUE(EndsWithIgnoreCase(L"C:\\Dir\\File.DAT", L".dat"));
  EXPECT_TRUE(EndsWithIgnoreCase(L"C:\\Dir\\File.DAT", L"file.dat"));
  EXPECT_TRUE(EndsWithIgnoreCase(L"File.DAT", L"file.dat"));
  EXPECT_FALSE(EndsWithIgnoreCase(L"short", L"longer_suffix"));
  EXPECT_FALSE(EndsWithIgnoreCase(L"prefix_body", L"prefix"));

  // EndsWithExact
  EXPECT_TRUE(EndsWithExact(L"", L""));
  EXPECT_TRUE(EndsWithExact(L"C:\\Dir\\File.DAT", L""));
  EXPECT_TRUE(EndsWithExact(L"C:\\Dir\\File.DAT", L".DAT"));
  EXPECT_FALSE(EndsWithExact(L"C:\\Dir\\File.DAT", L".dat"));
  EXPECT_TRUE(EndsWithExact(L"Exact", L"Exact"));
  EXPECT_FALSE(EndsWithExact(L"short", L"longer_suffix"));
  EXPECT_FALSE(EndsWithExact(L"prefix_body", L"prefix"));
}

TEST(CoreHelpersTest, GetOsInfoSanityInvariants) {
  const OsInfo &os = GetOsInfo();
  EXPECT_EQ(&os, &GetOsInfo());

  EXPECT_GE(os.major, 10u);
  EXPECT_GE(os.build, kBuildWin10_1607);
  EXPECT_NE(os.host_arch, Arch::kUnknown);
  EXPECT_NE(os.process_arch, Arch::kUnknown);

  const std::string_view host_arch_str = ArchToString(os.host_arch);
  const std::string_view proc_arch_str = ArchToString(os.process_arch);
  EXPECT_NE(host_arch_str, "unknown");
  EXPECT_NE(proc_arch_str, "unknown");
  EXPECT_EQ(ParseArch(host_arch_str), os.host_arch);
  EXPECT_EQ(ParseArch(proc_arch_str), os.process_arch);

  EXPECT_EQ(ParseArch("AMD64"), Arch::kX64);
  EXPECT_EQ(ParseArch("Win32"), Arch::kX86);
  EXPECT_EQ(ParseArch("ARM64"), Arch::kArm64);
  EXPECT_FALSE(ParseArch("mips").has_value());

  EXPECT_TRUE(os.version_string.starts_with("Windows ")) << os.version_string;
  EXPECT_NE(os.version_string.find(std::to_string(os.build)), std::string::npos)
      << os.version_string;

  if (!os.is_elevated || os.build < kBuildWin10_1809) {
    EXPECT_FALSE(os.can_set_case_sensitive_dir);
  }
}

TEST(CoreHelpersTest, ProbeVolumeCapabilitiesOnTempAndMissingPath) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  const auto norm = NormalizeRootPath(host_tmp);
  ASSERT_TRUE(norm.ok()) << FormatWin32Error(norm.error);

  const OsInfo &os = GetOsInfo();
  const VolumeCapabilities caps = ProbeVolumeCapabilities(*norm.value, os);
  ASSERT_TRUE(caps.probe_succeeded) << caps.probe_error;
  EXPECT_TRUE(caps.probe_error.empty());
  EXPECT_FALSE(caps.filesystem_name.empty());
  EXPECT_GT(caps.max_component_length, 0u);
  EXPECT_GT(caps.bytes_per_sector, 0u);
  EXPECT_EQ(caps.bytes_per_sector & (caps.bytes_per_sector - 1u), 0u);
  EXPECT_GE(caps.physical_bytes_per_sector, caps.bytes_per_sector);
  EXPECT_GE(caps.bytes_per_cluster, caps.bytes_per_sector);
  EXPECT_EQ(caps.bytes_per_cluster % caps.bytes_per_sector, 0u);
  EXPECT_FALSE(caps.read_only_volume);

  // Category 4b flags match the raw FileSystemAttributes bitmask.
  EXPECT_EQ(caps.named_streams, (caps.fs_flags & nt::kFileNamedStreams) != 0);
  EXPECT_EQ(caps.sparse_files,
            (caps.fs_flags & nt::kFileSupportsSparseFiles) != 0);
  EXPECT_EQ(caps.hard_links, (caps.fs_flags & nt::kFileSupportsHardLinks) != 0);
  EXPECT_EQ(caps.reparse_points,
            (caps.fs_flags & nt::kFileSupportsReparsePoints) != 0);
  EXPECT_EQ(caps.persistent_acls,
            (caps.fs_flags & nt::kFilePersistentAcls) != 0);
  EXPECT_EQ(caps.extended_attributes,
            (caps.fs_flags & nt::kFileSupportsExtendedAttributes) != 0);
  EXPECT_EQ(caps.case_preserved_names,
            (caps.fs_flags & nt::kFileCasePreservedNames) != 0);
  EXPECT_EQ(caps.case_sensitive_search,
            (caps.fs_flags & nt::kFileCaseSensitiveSearch) != 0);

  // Probing a guaranteed-nonexistent directory fails cleanly with error detail.
  auto dir = ScopedTempDir::Create(host_tmp, L"fstcap");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);
  const auto missing_norm =
      NormalizeRootPath(dir.value->ChildPath(L"does_not_exist"));
  ASSERT_TRUE(missing_norm.ok()) << FormatWin32Error(missing_norm.error);

  const VolumeCapabilities missing_caps =
      ProbeVolumeCapabilities(*missing_norm.value, os);
  EXPECT_FALSE(missing_caps.probe_succeeded);
  EXPECT_FALSE(missing_caps.probe_error.empty());

  std::string cleanup_err;
  EXPECT_TRUE(dir.value->Cleanup(&cleanup_err)) << cleanup_err;
}

TEST(CoreHelpersTest, UniqueHandleSelfResetAndInvalidHandlePut) {
  // Self-reset on a live kernel handle must be a no-op (not close it).
  UniqueHandle evt(::CreateEventW(nullptr, /*bManualReset=*/TRUE,
                                  /*bInitialState=*/FALSE, nullptr));
  ASSERT_TRUE(evt.valid());
  const HANDLE raw = evt.get();
  evt.reset(raw);
  EXPECT_TRUE(evt.valid());
  EXPECT_EQ(evt.get(), raw);
  EXPECT_NE(::SetEvent(evt.get()), FALSE) << FormatWin32Error(::GetLastError());

  // Out-parameter slot receiving INVALID_HANDLE_VALUE must report invalid and
  // never call CloseHandle(INVALID_HANDLE_VALUE) on reset/destruction.
  UniqueHandle out_slot;
  *out_slot.put() = INVALID_HANDLE_VALUE;
  EXPECT_FALSE(out_slot.valid());
  EXPECT_FALSE(static_cast<bool>(out_slot));
  EXPECT_EQ(out_slot.get(), nullptr);
  out_slot.reset();
  EXPECT_FALSE(out_slot.valid());

  // Self-reset on UniqueLocalMem must preserve the allocation.
  UniqueLocalMem<uint8_t> mem(
      static_cast<uint8_t *>(::LocalAlloc(LMEM_FIXED, 32)));
  ASSERT_TRUE(static_cast<bool>(mem));
  uint8_t *raw_mem = mem.get();
  raw_mem[0] = 0x5A;
  mem.reset(raw_mem);
  EXPECT_EQ(mem.get(), raw_mem);
  EXPECT_EQ(mem.get()[0], 0x5A);

  // Self-reset on UniqueFindHandle must keep the search handle open.
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  WIN32_FIND_DATAW fd{};
  UniqueFindHandle find(
      ::FindFirstFileW(JoinPath(host_tmp, L"*").c_str(), &fd));
  ASSERT_TRUE(find.valid());
  const HANDLE raw_find = find.get();
  find.reset(raw_find);
  EXPECT_TRUE(find.valid());
  EXPECT_EQ(find.get(), raw_find);
}

TEST(CoreHelpersTest, UniqueViewOfFileMappingSelfResetMoveAndRelease) {
  UniqueHandle mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                            PAGE_READWRITE, 0, 4096, nullptr));
  ASSERT_TRUE(mapping.valid()) << FormatWin32Error(::GetLastError());

  UniqueViewOfFile view(
      ::MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 4096));
  ASSERT_TRUE(view.valid()) << FormatWin32Error(::GetLastError());
  EXPECT_TRUE(static_cast<bool>(view));
  void *const raw = view.get();
  static_cast<uint8_t *>(raw)[0] = 0xAB;

  // Self-reset must be a no-op and keep the mapped view valid.
  view.reset(raw);
  EXPECT_EQ(view.get(), raw);
  EXPECT_EQ(static_cast<uint8_t *>(view.get())[0], 0xAB);

  // Move transfers ownership cleanly.
  UniqueViewOfFile moved(std::move(view));
  EXPECT_FALSE(view.valid());
  EXPECT_EQ(view.get(), nullptr);
  ASSERT_EQ(moved.get(), raw);
  EXPECT_EQ(static_cast<uint8_t *>(moved.get())[0], 0xAB);

  // release() relinquishes without unmapping; reset() re-takes ownership.
  void *const released = moved.release();
  EXPECT_EQ(released, raw);
  EXPECT_FALSE(moved.valid());
  moved.reset(released);
  EXPECT_EQ(moved.get(), raw);

  // put() unmaps the previous view and accepts a new mapped address.
  *moved.put() =
      ::MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 4096);
  ASSERT_TRUE(moved.valid()) << FormatWin32Error(::GetLastError());
  EXPECT_EQ(static_cast<uint8_t *>(moved.get())[0], 0xAB);
}

TEST(CoreHelpersTest, NtPathOverMaxLengthRejectedWithStatusNameTooLong) {
  const std::wstring huge_path(32767u, L'a');

  auto create_res =
      NtCreateFilePath(huge_path, GENERIC_READ, FILE_ATTRIBUTE_NORMAL,
                       kShareAll, nt::kFileOpen, nt::kFileNonDirectoryFile);
  EXPECT_FALSE(create_res.ok());
  EXPECT_EQ(create_res.status, STATUS_NAME_TOO_LONG);

  auto open_res = NtOpenFilePath(huge_path, GENERIC_READ, kShareAll,
                                 nt::kFileNonDirectoryFile);
  EXPECT_FALSE(open_res.ok());
  EXPECT_EQ(open_res.status, STATUS_NAME_TOO_LONG);

  // Oversized junction target path fails cleanly before touching disk.
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto dir = ScopedTempDir::Create(host_tmp, L"fstjlen");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);
  const std::wstring junc_path = dir.value->ChildPath(L"too_long_junc");
  const std::wstring huge_target = L"C:\\" + std::wstring(10000u, L'x');
  auto jres = CreateDirectoryJunction(junc_path, huge_target);
  EXPECT_FALSE(jres.ok());
  EXPECT_EQ(jres.error, static_cast<DWORD>(ERROR_FILENAME_EXCED_RANGE));
  EXPECT_EQ(::GetFileAttributesW(junc_path.c_str()), INVALID_FILE_ATTRIBUTES);

  std::string cleanup_err;
  EXPECT_TRUE(dir.value->Cleanup(&cleanup_err)) << cleanup_err;
}

TEST(CoreHelpersTest, ListStreamsNtQueryFileNameHardLinksAndAllocatedRanges) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());

  auto dir = ScopedTempDir::Create(host_tmp, L"fstops");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  const auto norm = NormalizeRootPath(dir.value->Path());
  ASSERT_TRUE(norm.ok()) << FormatWin32Error(norm.error);
  const VolumeCapabilities caps =
      ProbeVolumeCapabilities(*norm.value, GetOsInfo());
  ASSERT_TRUE(caps.probe_succeeded) << caps.probe_error;

  // 1. ListStreamsNt on file with alternate stream.
  if (caps.named_streams) {
    const std::wstring fpath = dir.value->ChildExtendedPath(L"streams.txt");
    ASSERT_TRUE(WriteFileContents(fpath, "main_data").ok());
    ASSERT_TRUE(WriteFileContents(fpath + L":alt", "alt_payload!").ok());

    auto fh = CreateWin32File(fpath, FILE_READ_ATTRIBUTES, kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh.ok()) << FormatWin32Error(fh.error);

    auto streams = ListStreamsNt(fh.value->get());
    ASSERT_TRUE(streams.ok()) << FormatNtStatus(streams.status);
    bool saw_default = false;
    bool saw_alt = false;
    for (const StreamEntry &s : *streams.value) {
      if (EqualsIgnoreCase(s.name, L"::$DATA")) {
        saw_default = true;
        EXPECT_EQ(s.size, 9);
      } else if (EqualsIgnoreCase(s.name, L":alt:$DATA")) {
        saw_alt = true;
        EXPECT_EQ(s.size, 12);
      }
    }
    EXPECT_TRUE(saw_default);
    EXPECT_TRUE(saw_alt);
  }

  // 2. QueryFileNameInfo on normal and >1024-byte nested long path (exercises
  // STATUS_BUFFER_OVERFLOW resize loop).
  {
    std::wstring deep_dir = dir.value->ExtendedPath();
    const std::wstring seg(100, L'n');
    for (int i = 0; i < 6; ++i) {
      deep_dir = JoinPath(deep_dir, seg + std::to_wstring(i));
      ASSERT_NE(::CreateDirectoryW(deep_dir.c_str(), nullptr), FALSE)
          << FormatWin32Error(::GetLastError());
    }
    const std::wstring deep_file = JoinPath(deep_dir, L"name_query_leaf.txt");
    ASSERT_TRUE(WriteFileContents(deep_file, "x").ok());

    auto fh = CreateWin32File(deep_file, FILE_READ_ATTRIBUTES, kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh.ok()) << FormatWin32Error(fh.error);

    auto name_res = QueryFileNameInfo(fh.value->get(), /*normalized=*/false);
    ASSERT_TRUE(name_res.ok()) << FormatNtStatus(name_res.status);
    EXPECT_TRUE(EndsWithIgnoreCase(*name_res.value, L"\\name_query_leaf.txt"))
        << WideToUtf8(*name_res.value);
    EXPECT_GT(name_res.value->size(), 550u);

    auto norm_res = QueryFileNameInfo(fh.value->get(), /*normalized=*/true);
    ASSERT_TRUE(norm_res.ok()) << FormatNtStatus(norm_res.status);
    EXPECT_TRUE(EndsWithIgnoreCase(*norm_res.value, L"\\name_query_leaf.txt"))
        << WideToUtf8(*norm_res.value);
  }

  // 3. QueryHardLinkEntries on a file with two hard links.
  if (caps.hard_links) {
    const std::wstring link1 = dir.value->ChildExtendedPath(L"hl_one.txt");
    const std::wstring link2 = dir.value->ChildExtendedPath(L"hl_two.txt");
    ASSERT_TRUE(WriteFileContents(link1, "linked").ok());
    ASSERT_NE(::CreateHardLinkW(link2.c_str(), link1.c_str(), nullptr), FALSE)
        << FormatWin32Error(::GetLastError());

    auto fh = CreateWin32File(link1, FILE_READ_ATTRIBUTES, kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh.ok()) << FormatWin32Error(fh.error);

    auto links = QueryHardLinkEntries(fh.value->get());
    ASSERT_TRUE(links.ok()) << FormatNtStatus(links.status);
    EXPECT_GE(links.value->size(), 2u);
    bool saw_one = false;
    bool saw_two = false;
    for (const auto &[parent_id, name] : *links.value) {
      EXPECT_NE(parent_id, 0);
      if (EqualsIgnoreCase(name, L"hl_one.txt")) {
        saw_one = true;
      } else if (EqualsIgnoreCase(name, L"hl_two.txt")) {
        saw_two = true;
      }
    }
    EXPECT_TRUE(saw_one);
    EXPECT_TRUE(saw_two);
  }

  // 4. QueryAllocatedRanges on a sparse file with two separated extents.
  if (caps.sparse_files) {
    const std::wstring sparse_path =
        dir.value->ChildExtendedPath(L"sparse_ranges.bin");
    auto fh = CreateWin32File(sparse_path, GENERIC_READ | GENERIC_WRITE,
                              kShareAll, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh.ok()) << FormatWin32Error(fh.error);
    ASSERT_TRUE(SetSparseFile(fh.value->get(), true).ok());

    const std::string block(4096, 'Z');
    ASSERT_TRUE(
        WriteAllBytes(fh.value->get(), block.data(), block.size()).ok());

    constexpr int64_t kSecondOffset = 256LL * 1024LL;
    LARGE_INTEGER pos{};
    pos.QuadPart = kSecondOffset;
    ASSERT_NE(::SetFilePointerEx(fh.value->get(), pos, nullptr, FILE_BEGIN),
              FALSE)
        << FormatWin32Error(::GetLastError());
    ASSERT_TRUE(
        WriteAllBytes(fh.value->get(), block.data(), block.size()).ok());

    auto ranges = QueryAllocatedRanges(
        fh.value->get(), 0, kSecondOffset + static_cast<int64_t>(block.size()));
    ASSERT_TRUE(ranges.ok()) << FormatWin32Error(ranges.error);
    ASSERT_GE(ranges.value->size(), 2u);
    EXPECT_EQ((*ranges.value)[0].FileOffset.QuadPart, 0LL);
    EXPECT_GT((*ranges.value)[0].Length.QuadPart, 0LL);
    const auto &last = ranges.value->back();
    EXPECT_LE(last.FileOffset.QuadPart, kSecondOffset);
    EXPECT_GE(last.FileOffset.QuadPart + last.Length.QuadPart,
              kSecondOffset + static_cast<int64_t>(block.size()));
  }

  std::string cleanup_err;
  EXPECT_TRUE(dir.value->Cleanup(&cleanup_err)) << cleanup_err;
}

TEST(CoreHelpersTest, EaBufferRejectsOversizedNameAndValue) {
  std::string err;
  const std::string max_name(255, 'N');
  const std::vector<uint8_t> max_value(65535, 0x5A);
  const std::vector<uint8_t> at_limit =
      BuildEaBuffer({{0, max_name, max_value}}, &err);
  ASSERT_FALSE(at_limit.empty()) << err;
  const std::vector<EaEntry> parsed =
      ParseEaBuffer(at_limit.data(), at_limit.size());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0].name.size(), 255u);
  EXPECT_EQ(parsed[0].value.size(), 65535u);

  err.clear();
  EXPECT_TRUE(
      BuildEaBuffer({{0, "ok", {1}}, {0, std::string(256, 'N'), {}}}, &err)
          .empty());
  EXPECT_NE(err.find("EA entry 1"), std::string::npos) << err;

  err.clear();
  EXPECT_TRUE(BuildEaBuffer({{0, "big", std::vector<uint8_t>(65536, 0)}}, &err)
                  .empty());
  EXPECT_NE(err.find("65536"), std::string::npos) << err;

  // The default (no error_out) overload also refuses instead of truncating.
  EXPECT_TRUE(BuildEaBuffer({{0, std::string(300, 'N'), {}}}).empty());
}

TEST(CoreHelpersTest, Utf8WideConversionRejectsInvalidInput) {
  // Valid supplementary-plane round trip still works.
  const std::wstring pair = L"a\U0001F4C4b";
  EXPECT_EQ(Utf8ToWide(WideToUtf8(pair)), pair);

  // Malformed UTF-8: bad continuation, overlong '/', truncated 3-byte
  // sequence, CESU-8 encoded surrogate, and a 0xFF byte.
  for (const std::string_view bad :
       {std::string_view("\xC3\x28"), std::string_view("\xC0\xAF"),
        std::string_view("ok\xE6\x97"), std::string_view("\xED\xA0\x80"),
        std::string_view("\xFF")}) {
    EXPECT_TRUE(Utf8ToWide(bad).empty())
        << "accepted invalid UTF-8 of length " << bad.size();
  }

  // Lone high and low surrogates.
  const std::wstring lone_high{L'a', static_cast<wchar_t>(0xD800), L'b'};
  const std::wstring lone_low{static_cast<wchar_t>(0xDC00)};
  const std::wstring reversed{static_cast<wchar_t>(0xDC00),
                              static_cast<wchar_t>(0xD800)};
  EXPECT_TRUE(WideToUtf8(lone_high).empty());
  EXPECT_TRUE(WideToUtf8(lone_low).empty());
  EXPECT_TRUE(WideToUtf8(reversed).empty());
}

TEST(CoreHelpersTest, ParseFsAttributeInfoClampsToReturnedBytes) {
  constexpr size_t kHeader =
      offsetof(nt::FileFsAttributeInfo, file_system_name);
  std::vector<uint8_t> buf(kHeader + 8, 0);
  StoreAt<ULONG>(&buf,
                 offsetof(nt::FileFsAttributeInfo, file_system_attributes),
                 0x00C700FFUL);
  StoreAt<LONG>(
      &buf, offsetof(nt::FileFsAttributeInfo, maximum_component_name_length),
      255);
  StoreAt<ULONG>(&buf,
                 offsetof(nt::FileFsAttributeInfo, file_system_name_length), 8);
  StoreWide(&buf, kHeader, L"NTFS");

  FsAttributeInfo info{};
  ASSERT_TRUE(ParseFsAttributeInfo(buf, &info));
  EXPECT_EQ(info.fs_flags, 0x00C700FFu);
  EXPECT_EQ(info.max_component_length, 255);
  EXPECT_EQ(info.filesystem_name, L"NTFS");

  // Fewer than 12 returned bytes is rejected.
  for (size_t len = 0; len < kHeader; ++len) {
    EXPECT_FALSE(
        ParseFsAttributeInfo(std::span<const uint8_t>(buf.data(), len), &info))
        << "len=" << len;
  }

  // Header only: empty name even though FileSystemNameLength claims 8 bytes.
  ASSERT_TRUE(ParseFsAttributeInfo(
      std::span<const uint8_t>(buf.data(), kHeader), &info));
  EXPECT_TRUE(info.filesystem_name.empty());

  // Truncated (STATUS_BUFFER_OVERFLOW-style) reply with an odd byte count and
  // a lying length field: clamp to whole WCHARs actually returned.
  StoreAt<ULONG>(&buf,
                 offsetof(nt::FileFsAttributeInfo, file_system_name_length),
                 0xFFFFFFFFUL);
  ASSERT_TRUE(ParseFsAttributeInfo(
      std::span<const uint8_t>(buf.data(), kHeader + 5), &info));
  EXPECT_EQ(info.filesystem_name, L"NT");
}

TEST(CoreHelpersTest, ParseStreamInfoBufferHandlesCorruption) {
  std::vector<uint8_t> buf;
  const size_t second = BuildStreamInfo(&buf);
  constexpr size_t kHeader = offsetof(nt::FileStreamInfo, stream_name);

  const auto ok = ParseStreamInfoBuffer(buf);
  ASSERT_EQ(ok.size(), 2u);
  EXPECT_EQ(ok[0].name, L"::$DATA");
  EXPECT_EQ(ok[0].size, 9);
  EXPECT_EQ(ok[0].allocation_size, 4096);
  EXPECT_EQ(ok[1].name, L":alt:$DATA");
  EXPECT_EQ(ok[1].size, 12);

  EXPECT_TRUE(ParseStreamInfoBuffer({}).empty());
  EXPECT_TRUE(
      ParseStreamInfoBuffer(std::span<const uint8_t>(buf.data(), kHeader - 1))
          .empty());

  // Truncated inside the second entry's header: only the first survives.
  EXPECT_EQ(ParseStreamInfoBuffer(
                std::span<const uint8_t>(buf.data(), second + kHeader - 1))
                .size(),
            1u);

  // Lying StreamNameLength is clamped to the returned bytes.
  {
    std::vector<uint8_t> bad = buf;
    StoreAt<ULONG>(&bad,
                   second + offsetof(nt::FileStreamInfo, stream_name_length),
                   0xFFFFFFFFUL);
    const auto parsed = ParseStreamInfoBuffer(bad);
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[1].name, L":alt:$DATA");
  }

  // Out-of-range NextEntryOffset values stop after the first entry.
  for (const ULONG next :
       {static_cast<ULONG>(buf.size() + 1), 0xFFFFFFFFUL, 0x80000000UL}) {
    std::vector<uint8_t> bad = buf;
    StoreAt<ULONG>(&bad, offsetof(nt::FileStreamInfo, next_entry_offset), next);
    EXPECT_EQ(ParseStreamInfoBuffer(bad).size(), 1u) << "next=" << next;
  }

  // A misaligned NextEntryOffset must not crash or read out of bounds.
  {
    std::vector<uint8_t> bad = buf;
    StoreAt<ULONG>(&bad, offsetof(nt::FileStreamInfo, next_entry_offset), 3);
    const auto parsed = ParseStreamInfoBuffer(bad);
    EXPECT_GE(parsed.size(), 1u);
    EXPECT_LE(parsed.size(), bad.size());
  }
}

TEST(CoreHelpersTest, ParseHardLinkBufferHandlesCorruption) {
  std::vector<uint8_t> buf;
  const size_t first = BuildHardLinkInfo(&buf, /*entries_returned=*/2);
  constexpr size_t kEntryHeader = offsetof(nt::FileLinkEntryInfo, file_name);

  const auto ok = ParseHardLinkBuffer(buf);
  ASSERT_EQ(ok.size(), 2u);
  EXPECT_EQ(ok[0].first, 5);
  EXPECT_EQ(ok[0].second, L"one.txt");
  EXPECT_EQ(ok[1].first, 7);
  EXPECT_EQ(ok[1].second, L"two.txt");

  EXPECT_TRUE(ParseHardLinkBuffer({}).empty());
  EXPECT_TRUE(
      ParseHardLinkBuffer(std::span<const uint8_t>(buf.data(), first)).empty());
  EXPECT_TRUE(ParseHardLinkBuffer(std::span<const uint8_t>(
                                      buf.data(), first + kEntryHeader - 1))
                  .empty());

  // EntriesReturned bounds the walk; a huge count stops at NextEntryOffset 0.
  {
    std::vector<uint8_t> one;
    BuildHardLinkInfo(&one, /*entries_returned=*/1);
    EXPECT_EQ(ParseHardLinkBuffer(one).size(), 1u);
    std::vector<uint8_t> many;
    BuildHardLinkInfo(&many, /*entries_returned=*/0xFFFFFFFFUL);
    EXPECT_EQ(ParseHardLinkBuffer(many).size(), 2u);
  }

  // Lying FileNameLength (WCHARs) is clamped to the returned bytes.
  {
    std::vector<uint8_t> bad = buf;
    ULONG first_next = 0;
    std::memcpy(&first_next,
                bad.data() + first +
                    offsetof(nt::FileLinkEntryInfo, next_entry_offset),
                sizeof(first_next));
    const size_t second = first + first_next;
    StoreAt<ULONG>(&bad,
                   second + offsetof(nt::FileLinkEntryInfo, file_name_length),
                   0x7FFFFFFFUL);
    const auto parsed = ParseHardLinkBuffer(bad);
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[1].second, L"two.txt");
  }

  // Out-of-range NextEntryOffset stops after the first entry.
  for (const ULONG next : {static_cast<ULONG>(buf.size()), 0xFFFFFFFFUL}) {
    std::vector<uint8_t> bad = buf;
    StoreAt<ULONG>(
        &bad, first + offsetof(nt::FileLinkEntryInfo, next_entry_offset), next);
    EXPECT_EQ(ParseHardLinkBuffer(bad).size(), 1u) << "next=" << next;
  }
}

TEST(CoreHelpersTest, ParseFileNameInfoBufferHandlesCorruption) {
  constexpr size_t kHeader = offsetof(nt::FileNameInfo, file_name);
  const std::wstring_view name = L"\\dir\\leaf.txt";
  std::vector<uint8_t> buf(kHeader + name.size() * sizeof(wchar_t), 0);
  StoreAt<ULONG>(&buf, offsetof(nt::FileNameInfo, file_name_length),
                 static_cast<ULONG>(name.size() * sizeof(wchar_t)));
  StoreWide(&buf, kHeader, name);

  const std::optional<std::wstring> ok = ParseFileNameInfoBuffer(buf);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, name);

  for (size_t len = 0; len < kHeader; ++len) {
    EXPECT_FALSE(
        ParseFileNameInfoBuffer(std::span<const uint8_t>(buf.data(), len))
            .has_value())
        << "len=" << len;
  }
  const auto header_only =
      ParseFileNameInfoBuffer(std::span<const uint8_t>(buf.data(), kHeader));
  ASSERT_TRUE(header_only.has_value());
  EXPECT_TRUE(header_only->empty());

  // Lying length and odd trailing byte: clamp to whole returned WCHARs.
  StoreAt<ULONG>(&buf, offsetof(nt::FileNameInfo, file_name_length),
                 0xFFFFFFFFUL);
  const auto clamped = ParseFileNameInfoBuffer(
      std::span<const uint8_t>(buf.data(), kHeader + 2 * sizeof(wchar_t) + 1));
  ASSERT_TRUE(clamped.has_value());
  EXPECT_EQ(*clamped, name.substr(0, 2));
}

TEST(CoreHelpersTest, ScopedPrivilegeEnablesAndRestoresProcessPrivilege) {
  // SeIncreaseWorkingSetPrivilege is granted (disabled) to standard users and
  // administrators alike and enabling it has no side effects on the tests.
  const wchar_t *kPriv = SE_INC_WORKING_SET_NAME;
  const PrivilegeState before = QueryProcessPrivilegeState(kPriv);
  if (!before.present) {
    GTEST_SKIP() << "Process token lacks SeIncreaseWorkingSetPrivilege.";
  }
  {
    ScopedPrivilege enabled(kPriv, /*enable=*/true);
    ASSERT_TRUE(enabled.active()) << FormatWin32Error(enabled.error());
    EXPECT_EQ(enabled.error(), static_cast<DWORD>(ERROR_SUCCESS));
    EXPECT_TRUE(QueryProcessPrivilegeState(kPriv).enabled);

    // Moving transfers the restore obligation exactly once.
    ScopedPrivilege moved(std::move(enabled));
    EXPECT_FALSE(enabled.active());
    EXPECT_TRUE(moved.active());
    EXPECT_TRUE(QueryProcessPrivilegeState(kPriv).enabled);
  }
  EXPECT_EQ(QueryProcessPrivilegeState(kPriv).enabled, before.enabled);

  // A privilege absent from the token reports ERROR_NOT_ALL_ASSIGNED.
  if (!QueryProcessPrivilegeState(SE_CREATE_TOKEN_NAME).present) {
    ScopedPrivilege missing(SE_CREATE_TOKEN_NAME, /*enable=*/true);
    EXPECT_FALSE(missing.active());
    EXPECT_EQ(missing.error(), static_cast<DWORD>(ERROR_NOT_ALL_ASSIGNED));
  }

  ScopedPrivilege bogus(L"SeFstestBogusPrivilege", /*enable=*/true);
  EXPECT_FALSE(bogus.active());
  EXPECT_EQ(bogus.error(), static_cast<DWORD>(ERROR_NO_SUCH_PRIVILEGE));
}

TEST(CoreHelpersTest, CreateTokenWithoutPrivilegeAndScopedImpersonation) {
  auto token = CreateTokenWithoutPrivilege(SE_CHANGE_NOTIFY_NAME);
  ASSERT_TRUE(token.ok()) << FormatWin32Error(token.error);
  EXPECT_FALSE(
      QueryPrivilegeState(token.value->get(), SE_CHANGE_NOTIFY_NAME).present);
  // The process token itself is untouched.
  EXPECT_TRUE(QueryProcessPrivilegeState(SE_CHANGE_NOTIFY_NAME).present);

  {
    ScopedImpersonation imp(token.value->get());
    ASSERT_TRUE(imp.active()) << FormatWin32Error(imp.error());
    HANDLE raw = nullptr;
    ASSERT_NE(::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY,
                                /*OpenAsSelf=*/TRUE, &raw),
              FALSE)
        << FormatWin32Error(::GetLastError());
    UniqueHandle thread_token(raw);
    EXPECT_FALSE(
        QueryPrivilegeState(thread_token.get(), SE_CHANGE_NOTIFY_NAME).present);

    ScopedImpersonation moved(std::move(imp));
    EXPECT_FALSE(imp.active());
    EXPECT_TRUE(moved.active());
  }
  HANDLE after = nullptr;
  const BOOL reopened = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY,
                                          /*OpenAsSelf=*/TRUE, &after);
  const DWORD reopen_err = ::GetLastError();
  UniqueHandle after_guard(after);
  EXPECT_EQ(reopened, FALSE) << "thread still impersonating after scope exit";
  EXPECT_EQ(reopen_err, static_cast<DWORD>(ERROR_NO_TOKEN));

  ScopedImpersonation invalid(nullptr);
  EXPECT_FALSE(invalid.active());
  EXPECT_EQ(invalid.error(), static_cast<DWORD>(ERROR_INVALID_HANDLE));

  // Removing a privilege the token never had still yields a usable token;
  // unknown privilege names fail cleanly.
  if (!QueryProcessPrivilegeState(SE_CREATE_TOKEN_NAME).present) {
    auto absent = CreateTokenWithoutPrivilege(SE_CREATE_TOKEN_NAME);
    EXPECT_TRUE(absent.ok()) << FormatWin32Error(absent.error);
  }
  auto bogus = CreateTokenWithoutPrivilege(L"SeFstestBogusPrivilege");
  EXPECT_FALSE(bogus.ok());
  EXPECT_EQ(bogus.error, static_cast<DWORD>(ERROR_NO_SUCH_PRIVILEGE));
}

TEST(CoreHelpersTest, QueryHardLinkEntriesRegrowsBuffer) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto dir = ScopedTempDir::Create(host_tmp, L"fsthlgrow");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);
  const auto norm = NormalizeRootPath(dir.value->Path());
  ASSERT_TRUE(norm.ok()) << FormatWin32Error(norm.error);
  if (!ProbeVolumeCapabilities(*norm.value, GetOsInfo()).hard_links) {
    std::string ignored;
    dir.value->Cleanup(&ignored);
    GTEST_SKIP() << "Host temp volume does not support hard links.";
  }

  // 40 links x (20-byte header + ~110 WCHARs) far exceeds the initial 4 KiB
  // query buffer, forcing the STATUS_BUFFER_OVERFLOW regrowth path.
  constexpr int kLinks = 40;
  const std::wstring stem(100, L'h');
  const std::wstring primary = dir.value->ChildExtendedPath(stem + L"_0");
  ASSERT_TRUE(WriteFileContents(primary, "linked").ok());
  for (int i = 1; i < kLinks; ++i) {
    const std::wstring link =
        dir.value->ChildExtendedPath(stem + L"_" + std::to_wstring(i));
    ASSERT_NE(::CreateHardLinkW(link.c_str(), primary.c_str(), nullptr), FALSE)
        << FormatWin32Error(::GetLastError());
  }

  {
    auto fh = CreateWin32File(primary, FILE_READ_ATTRIBUTES, kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh.ok()) << FormatWin32Error(fh.error);
    auto links = QueryHardLinkEntries(fh.value->get());
    ASSERT_TRUE(links.ok()) << FormatNtStatus(links.status);
    EXPECT_GT(links.information, ULONG_PTR{4096});
    ASSERT_EQ(links.value->size(), static_cast<size_t>(kLinks));
    for (const auto &[parent_id, name] : *links.value) {
      EXPECT_NE(parent_id, 0);
      EXPECT_TRUE(name.starts_with(stem + L"_")) << WideToUtf8(name);
    }
  }

  std::string cleanup_err;
  EXPECT_TRUE(dir.value->Cleanup(&cleanup_err)) << cleanup_err;
}

TEST(CoreHelpersTest, QueryAllocatedRangesPagesAcrossManyExtents) {
  const std::wstring host_tmp = GetHostTempDir();
  ASSERT_FALSE(host_tmp.empty());
  auto dir = ScopedTempDir::Create(host_tmp, L"fstpages");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);
  const auto norm = NormalizeRootPath(dir.value->Path());
  ASSERT_TRUE(norm.ok()) << FormatWin32Error(norm.error);
  if (!ProbeVolumeCapabilities(*norm.value, GetOsInfo()).sparse_files) {
    std::string ignored;
    dir.value->Cleanup(&ignored);
    GTEST_SKIP() << "Host temp volume does not support sparse files.";
  }

  {
    auto fh = CreateWin32File(dir.value->ChildExtendedPath(L"paged.bin"),
                              GENERIC_READ | GENERIC_WRITE, kShareAll,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(fh.ok()) << FormatWin32Error(fh.error);
    ASSERT_TRUE(SetSparseFile(fh.value->get(), true).ok());

    // 150 extents 1 MiB apart (well beyond the 64-entry page in
    // QueryAllocatedRanges and NTFS's 64 KiB sparse granularity).
    constexpr int kExtents = 150;
    constexpr int64_t kStride = 1024LL * 1024LL;
    const std::string block(4096, 'P');
    for (int i = 0; i < kExtents; ++i) {
      LARGE_INTEGER pos{};
      pos.QuadPart = i * kStride;
      ASSERT_NE(::SetFilePointerEx(fh.value->get(), pos, nullptr, FILE_BEGIN),
                FALSE)
          << FormatWin32Error(::GetLastError());
      ASSERT_TRUE(
          WriteAllBytes(fh.value->get(), block.data(), block.size()).ok());
    }

    const int64_t total =
        (kExtents - 1) * kStride + static_cast<int64_t>(block.size());
    auto ranges = QueryAllocatedRanges(fh.value->get(), 0, total);
    ASSERT_TRUE(ranges.ok()) << FormatWin32Error(ranges.error);
    ASSERT_GE(ranges.value->size(), static_cast<size_t>(kExtents));

    // Ranges are ordered, non-overlapping, and cover every written block.
    int64_t prev_end = -1;
    for (const auto &r : *ranges.value) {
      EXPECT_GE(r.FileOffset.QuadPart, prev_end);
      EXPECT_GT(r.Length.QuadPart, 0);
      prev_end = r.FileOffset.QuadPart + r.Length.QuadPart;
    }
    for (int i = 0; i < kExtents; ++i) {
      const int64_t start = i * kStride;
      bool covered = false;
      for (const auto &r : *ranges.value) {
        if (r.FileOffset.QuadPart <= start &&
            r.FileOffset.QuadPart + r.Length.QuadPart >=
                start + static_cast<int64_t>(block.size())) {
          covered = true;
          break;
        }
      }
      EXPECT_TRUE(covered) << "block " << i << " not reported";
    }
  }

  std::string cleanup_err;
  EXPECT_TRUE(dir.value->Cleanup(&cleanup_err)) << cleanup_err;
}

TEST(CoreHelpersTest, SetRenameAndLinkSupportSingleCharRelativeTarget) {
  auto dir = ScopedTempDir::Create(GetHostTempDir(), L"fst1c");
  ASSERT_TRUE(dir.ok()) << FormatWin32Error(dir.error);

  {
    const std::wstring src = dir.value->ChildExtendedPath(L"src.txt");
    ASSERT_TRUE(WriteFileContents(src, "single-char").ok());

    auto root_h = CreateWin32File(dir.value->ExtendedPath(),
                                  FILE_LIST_DIRECTORY | SYNCHRONIZE, kShareAll,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(root_h.ok()) << FormatWin32Error(root_h.error);

    auto file_h = CreateWin32File(src, DELETE | SYNCHRONIZE, kShareAll,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(file_h.ok()) << FormatWin32Error(file_h.error);

    // 1-WCHAR relative rename exercises sizeof(FileRenameInfo) (24B on x64) >
    // offsetof(FileRenameInfo, file_name) + 2 (22B).
    const NtResult<void> ren =
        SetRenameInfo(file_h.value->get(), L"b", root_h.value->get(),
                      /*replace_if_exists=*/false);
    ASSERT_TRUE(ren.ok()) << FormatNtStatus(ren.status);
    file_h.value->reset();

    auto link_src = CreateWin32File(
        dir.value->ChildExtendedPath(L"b"), FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        kShareAll, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(link_src.ok()) << FormatWin32Error(link_src.error);
    const NtResult<void> lnk =
        SetLinkInfo(link_src.value->get(), L"c", root_h.value->get(),
                    /*replace_if_exists=*/false);
    ASSERT_TRUE(lnk.ok()) << FormatNtStatus(lnk.status);
  }

  std::string cleanup_err;
  EXPECT_TRUE(dir.value->Cleanup(&cleanup_err)) << cleanup_err;
}

TEST(CoreHelpersTest,
     ParseEaBufferHandlesUnalignedAndRejectsOverlappingOffset) {
  const std::vector<EaEntry> input = {
      EaEntry{.flags = 0, .name = "Alpha", .value = {'1', '2'}},
      EaEntry{.flags = 0, .name = "Beta", .value = {'3', '4', '5'}},
  };
  std::string err;
  const std::vector<uint8_t> raw = BuildEaBuffer(input, &err);
  ASSERT_FALSE(raw.empty()) << err;

  // Shift buffer by 1 byte so the base pointer is intentionally unaligned.
  std::vector<uint8_t> unaligned(raw.size() + 1, 0);
  std::memcpy(unaligned.data() + 1, raw.data(), raw.size());
  const std::vector<EaEntry> parsed =
      ParseEaBuffer(unaligned.data() + 1, raw.size());
  ASSERT_EQ(parsed.size(), 2u);
  EXPECT_EQ(parsed[0].name, "Alpha");
  EXPECT_EQ(parsed[1].name, "Beta");

  // Corrupt NextEntryOffset to 4 (overlapping inside entry 0): parser must stop
  // after entry 0 instead of spinning on overlapping bytes.
  std::vector<uint8_t> corrupt = raw;
  const ULONG bad_next = 4;
  std::memcpy(corrupt.data(), &bad_next, sizeof(bad_next));
  const std::vector<EaEntry> stopped =
      ParseEaBuffer(corrupt.data(), corrupt.size());
  EXPECT_EQ(stopped.size(), 1u);
}

} // namespace
} // namespace fstest::core
