// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/capabilities.h"

#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace fstest::core {
namespace {

// Single Category 4a volume-policy probe: creates a >8.3 throwaway file in a
// transient directory under root, queries FileBothDirectoryInformation for
// ShortNameLength > 0, and removes the transient directory immediately.
//
// Returns false (with *error_out set) when the probe itself could not run, so
// suite setup fails loudly instead of silently skipping every 8.3 test. A
// successful query reporting ShortNameLength == 0 returns true with
// *enabled_out == false (8.3 generation disabled or unsupported).
bool ProbeShortNamesEnabled(const NormalizedRoot &root, bool *enabled_out,
                            std::string *error_out) {
  *enabled_out = false;
  auto probe_dir = ScopedTempDir::Create(root.win32_root, L"fst83");
  if (!probe_dir) {
    *error_out = "8.3 short-name probe: failed to create probe directory "
                 "under '" +
                 WideToUtf8(root.win32_root) +
                 "': " + FormatWin32Error(probe_dir.error);
    return false;
  }

  constexpr std::wstring_view kLongName =
      L"long_probe_filename_for_8dot3_check.txt";
  const std::wstring file_path = probe_dir.value->ChildExtendedPath(kLongName);
  auto file_handle = CreateWin32File(file_path, GENERIC_WRITE, kShareAll,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  if (!file_handle) {
    *error_out = "8.3 short-name probe: CreateFileW(probe file) failed: " +
                 FormatWin32Error(file_handle.error);
    probe_dir.value->Cleanup();
    return false;
  }
  file_handle.value->reset();

  auto dir_handle = CreateWin32File(
      probe_dir.value->ExtendedPath(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
      kShareAll, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  if (!dir_handle) {
    *error_out = "8.3 short-name probe: CreateFileW(probe directory) failed: " +
                 FormatWin32Error(dir_handle.error);
    probe_dir.value->Cleanup();
    return false;
  }

  const auto &ntdll = Ntdll();
  if (ntdll.NtQueryDirectoryFile == nullptr) {
    *error_out = "8.3 short-name probe: ntdll!NtQueryDirectoryFile unavailable";
    dir_handle.value->reset();
    probe_dir.value->Cleanup();
    return false;
  }

  std::wstring pattern(kLongName);
  nt::UnicodeString pattern_us =
      nt::MakeUnicodeString(pattern.data(), pattern.size());
  std::vector<uint8_t> buf(1024, 0);
  nt::IoStatusBlock iosb{};
  const NTSTATUS st = ntdll.NtQueryDirectoryFile(
      dir_handle.value->get(), nullptr, nullptr, nullptr, &iosb, buf.data(),
      static_cast<ULONG>(buf.size()),
      nt::FileInfoClass::FileBothDirectoryInformation,
      /*return_single_entry=*/TRUE, &pattern_us, /*restart_scan=*/TRUE);
  dir_handle.value->reset();

  bool ok = false;
  if (!nt::NtSuccess(st)) {
    *error_out = "8.3 short-name probe: NtQueryDirectoryFile("
                 "FileBothDirectoryInformation) failed: " +
                 FormatNtStatus(st);
  } else if (iosb.information < offsetof(nt::FileBothDirInfo, file_name)) {
    *error_out = "8.3 short-name probe: NtQueryDirectoryFile("
                 "FileBothDirectoryInformation) returned " +
                 std::to_string(iosb.information) +
                 " bytes (< fixed FILE_BOTH_DIR_INFORMATION header).";
  } else {
    const auto *info =
        reinterpret_cast<const nt::FileBothDirInfo *>(buf.data());
    *enabled_out = (info->short_name_length > 0);
    ok = true;
  }

  probe_dir.value->Cleanup();
  return ok;
}

} // namespace

bool ParseFsAttributeInfo(std::span<const uint8_t> returned,
                          FsAttributeInfo *out) {
  constexpr size_t kHeaderBytes =
      offsetof(nt::FileFsAttributeInfo, file_system_name);
  static_assert(kHeaderBytes == 12, "FILE_FS_ATTRIBUTE_INFORMATION header");
  if (returned.size() < kHeaderBytes) {
    return false;
  }
  ULONG flags = 0;
  LONG max_component = 0;
  ULONG name_length = 0;
  std::memcpy(&flags,
              returned.data() +
                  offsetof(nt::FileFsAttributeInfo, file_system_attributes),
              sizeof(flags));
  std::memcpy(&max_component,
              returned.data() + offsetof(nt::FileFsAttributeInfo,
                                         maximum_component_name_length),
              sizeof(max_component));
  std::memcpy(&name_length,
              returned.data() +
                  offsetof(nt::FileFsAttributeInfo, file_system_name_length),
              sizeof(name_length));
  const size_t name_bytes =
      std::min<size_t>(name_length, returned.size() - kHeaderBytes) &
      ~static_cast<size_t>(1);
  out->fs_flags = flags;
  out->max_component_length = max_component;
  out->filesystem_name.assign(name_bytes / sizeof(wchar_t), L'\0');
  if (name_bytes > 0) {
    std::memcpy(out->filesystem_name.data(), returned.data() + kHeaderBytes,
                name_bytes);
  }
  return true;
}

VolumeCapabilities ProbeVolumeCapabilities(const NormalizedRoot &root,
                                           const OsInfo &os_info) {
  VolumeCapabilities caps{};
  const auto &ntdll = Ntdll();
  if (ntdll.NtQueryVolumeInformationFile == nullptr) {
    caps.probe_error = "ntdll!NtQueryVolumeInformationFile unavailable";
    return caps;
  }

  std::wstring open_path = root.extended_root;
  if (root.kind == RootKind::kUncShare && !open_path.empty() &&
      open_path.back() != L'\\') {
    open_path.push_back(L'\\');
  }
  auto handle =
      CreateWin32File(open_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, kShareAll,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  if (!handle && root.kind == RootKind::kUncShare) {
    std::wstring fallback = root.win32_root;
    if (!fallback.empty() && fallback.back() != L'\\') {
      fallback.push_back(L'\\');
    }
    handle =
        CreateWin32File(fallback, FILE_READ_ATTRIBUTES | SYNCHRONIZE, kShareAll,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
  }
  if (!handle) {
    caps.probe_error = "Failed to open root '" + WideToUtf8(root.win32_root) +
                       "': " + FormatWin32Error(handle.error);
    return caps;
  }

  // 1. FileFsAttributeInformation (STATUS_BUFFER_OVERFLOW still returns the
  // fixed header plus a truncated name).
  {
    std::vector<uint8_t> buf(1024, 0);
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryVolumeInformationFile(
        handle.value->get(), &iosb, buf.data(), static_cast<ULONG>(buf.size()),
        nt::FsInfoClass::FileFsAttributeInformation);
    if (!nt::NtSuccess(st) && st != STATUS_BUFFER_OVERFLOW) {
      caps.probe_error =
          "FileFsAttributeInformation failed: " + FormatNtStatus(st);
      return caps;
    }
    const size_t returned =
        std::min<size_t>(static_cast<size_t>(iosb.information), buf.size());
    FsAttributeInfo attr{};
    if (!ParseFsAttributeInfo(std::span<const uint8_t>(buf.data(), returned),
                              &attr)) {
      caps.probe_error = "FileFsAttributeInformation returned " +
                         std::to_string(returned) +
                         " bytes (< 12-byte fixed header).";
      return caps;
    }
    caps.fs_flags = attr.fs_flags;
    if (attr.max_component_length > 0) {
      caps.max_component_length =
          static_cast<uint32_t>(attr.max_component_length);
    }
    caps.filesystem_name = std::move(attr.filesystem_name);
  }

  // 2. FileFsSizeInformation
  {
    nt::FileFsSizeInfo size_info{};
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryVolumeInformationFile(
        handle.value->get(), &iosb, &size_info, sizeof(size_info),
        nt::FsInfoClass::FileFsSizeInformation);
    if (nt::NtSuccess(st)) {
      if (size_info.bytes_per_sector > 0) {
        caps.bytes_per_sector = size_info.bytes_per_sector;
      }
      if (size_info.sectors_per_allocation_unit > 0 &&
          caps.bytes_per_sector > 0) {
        caps.bytes_per_cluster =
            size_info.sectors_per_allocation_unit * caps.bytes_per_sector;
      }
    }
  }

  // 3. FileFsSectorSizeInformation (class 11; optional on older/custom FSs)
  caps.physical_bytes_per_sector = caps.bytes_per_sector;
  {
    nt::FileFsSectorSizeInfo sector_info{};
    nt::IoStatusBlock iosb{};
    const NTSTATUS st = ntdll.NtQueryVolumeInformationFile(
        handle.value->get(), &iosb, &sector_info, sizeof(sector_info),
        nt::FsInfoClass::FileFsSectorSizeInformation);
    if (nt::NtSuccess(st)) {
      if (sector_info.logical_bytes_per_sector > 0) {
        caps.bytes_per_sector = sector_info.logical_bytes_per_sector;
      }
      if (sector_info.physical_bytes_per_sector_for_atomicity > 0) {
        caps.physical_bytes_per_sector =
            sector_info.physical_bytes_per_sector_for_atomicity;
      } else {
        caps.physical_bytes_per_sector = caps.bytes_per_sector;
      }
    }
  }

  // Close probe handle before optional 8.3 probe.
  handle.value->reset();

  // Populate Category 4b capability flags from fs_flags.
  caps.read_only_volume = (caps.fs_flags & nt::kFileReadOnlyVolume) != 0;
  caps.named_streams = (caps.fs_flags & nt::kFileNamedStreams) != 0;
  caps.sparse_files = (caps.fs_flags & nt::kFileSupportsSparseFiles) != 0;
  caps.hard_links = (caps.fs_flags & nt::kFileSupportsHardLinks) != 0;
  caps.reparse_points = (caps.fs_flags & nt::kFileSupportsReparsePoints) != 0;
  caps.persistent_acls = (caps.fs_flags & nt::kFilePersistentAcls) != 0;
  caps.extended_attributes =
      (caps.fs_flags & nt::kFileSupportsExtendedAttributes) != 0;
  caps.case_preserved_names =
      (caps.fs_flags & nt::kFileCasePreservedNames) != 0;
  caps.case_sensitive_search =
      (caps.fs_flags & nt::kFileCaseSensitiveSearch) != 0;
  caps.file_compression = (caps.fs_flags & nt::kFileFileCompression) != 0;
  caps.object_ids = (caps.fs_flags & nt::kFileSupportsObjectIds) != 0;
  caps.open_by_file_id = (caps.fs_flags & nt::kFileSupportsOpenByFileId) != 0;
  caps.posix_unlink_rename =
      (caps.fs_flags & nt::kFileSupportsPosixUnlinkRename) != 0;
  caps.volume_quotas = (caps.fs_flags & nt::kFileVolumeQuotas) != 0;
  caps.usn_journal = (caps.fs_flags & nt::kFileSupportsUsnJournal) != 0;

  // Never probed empirically on root_path; defaults to true on Win10 1809+.
  caps.case_sensitive_dir = (os_info.build >= kBuildWin10_1809);
  // Deliberately not probed (no FileSystemAttributes bit exists for either);
  // see VolumeCapabilities::oplocks / change_notifications.
  caps.oplocks = true;
  caps.change_notifications = true;

  // Single Category 4a volume-policy probe (8.3 short names). A probe API
  // failure fails the whole capability probe rather than masquerading as
  // "8.3 disabled" and silently skipping every short-name test.
  if (!caps.read_only_volume) {
    std::string short_name_error;
    if (!ProbeShortNamesEnabled(root, &caps.short_names_enabled,
                                &short_name_error)) {
      caps.probe_error = std::move(short_name_error);
      return caps;
    }
  }

  caps.probe_succeeded = true;
  return caps;
}

} // namespace fstest::core
