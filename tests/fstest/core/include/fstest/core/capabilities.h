// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file capabilities.h
/// \brief Target volume attribute, geometry, and feature-flag probing.

#pragma once

#include "fstest/core/os_info.h"
#include "fstest/core/path.h"

#include <cstdint>
#include <span>
#include <string>

namespace fstest::core {

/// \brief Probed filesystem properties and NTFS capability flags for a test
/// root volume.
struct VolumeCapabilities {
  // Reported volume properties (FileFsAttributeInformation / Size / SectorSize)
  std::wstring filesystem_name; ///< Reported filesystem name (e.g. NTFS).
  uint32_t max_component_length = 255; ///< Maximum filename component length.
  uint32_t fs_flags = 0;               ///< Raw `FileSystemAttributes` bitmask.
  uint32_t bytes_per_sector = 512;     ///< Logical sector size in bytes.
  uint32_t physical_bytes_per_sector = 512; ///< Physical sector size in bytes.
  uint32_t bytes_per_cluster = 4096;        ///< Allocation unit size in bytes.
  bool read_only_volume = false; ///< True if `FILE_READ_ONLY_VOLUME` is set.

  // Category 4a (single narrow volume-policy exception):
  // Probed empirically because NTFS disables 8.3 generation on non-system
  // volumes without reflecting it in FileSystemAttributes.
  bool short_names_enabled = false; ///< True if 8.3 short names are generated.

  // Category 4b (filesystem feature capabilities — tests run unconditionally
  // by default and skip ONLY when respect_fs_capabilities is enabled or an
  // explicit CapabilityOverrides entry is false).
  bool named_streams = false;         ///< `FILE_NAMED_STREAMS`.
  bool sparse_files = false;          ///< `FILE_SUPPORTS_SPARSE_FILES`.
  bool hard_links = false;            ///< `FILE_SUPPORTS_HARD_LINKS`.
  bool reparse_points = false;        ///< `FILE_SUPPORTS_REPARSE_POINTS`.
  bool persistent_acls = false;       ///< `FILE_PERSISTENT_ACLS`.
  bool extended_attributes = false;   ///< `FILE_SUPPORTS_EXTENDED_ATTRIBUTES`.
  bool case_preserved_names = false;  ///< `FILE_CASE_PRESERVED_NAMES`.
  bool case_sensitive_search = false; ///< `FILE_CASE_SENSITIVE_SEARCH`.
  bool file_compression = false;      ///< `FILE_FILE_COMPRESSION`.
  bool object_ids = false;            ///< `FILE_SUPPORTS_OBJECT_IDS`.
  bool open_by_file_id = false;       ///< `FILE_SUPPORTS_OPEN_BY_FILE_ID`.
  bool posix_unlink_rename = false;   ///< `FILE_SUPPORTS_POSIX_UNLINK_RENAME`.
  bool volume_quotas = false;         ///< `FILE_VOLUME_QUOTAS`.
  bool usn_journal = false;           ///< `FILE_SUPPORTS_USN_JOURNAL`.
  bool case_sensitive_dir = false; ///< Per-directory case sensitivity support.
  /// Oplock support. NOT probed: no FileSystemAttributes bit reports it, so
  /// it is assumed true and oplock tests run unconditionally (disable via
  /// CapabilityOverrides for filesystems without oplock support).
  bool oplocks = true;
  /// Directory change notification support. NOT probed for the same reason as
  /// `oplocks`; assumed true (disable via CapabilityOverrides).
  bool change_notifications = true;

  bool probe_succeeded = false; ///< True iff volume probing succeeded.
  /// Diagnostic message when probe failed (including an 8.3 short-name probe
  /// API failure, which is never reported as "short names disabled").
  std::string probe_error;
};

/// \brief Fields decoded from a `FILE_FS_ATTRIBUTE_INFORMATION` payload.
struct FsAttributeInfo {
  uint32_t fs_flags = 0;            ///< `FileSystemAttributes` bitmask.
  int32_t max_component_length = 0; ///< `MaximumComponentNameLength`.
  std::wstring filesystem_name;     ///< Name clamped to the returned bytes.
};

/// \brief Parses the bytes actually returned by
/// `FileFsAttributeInformation` (`IoStatusBlock.Information`).
///
/// Requires at least the 12-byte fixed header, clamps `FileSystemNameLength`
/// to the returned bytes (so a truncated `STATUS_BUFFER_OVERFLOW` reply or a
/// lying length cannot read past the buffer), and drops an odd trailing byte.
/// \param returned Returned payload (`buffer.first(iosb.information)`).
/// \param out Receives the decoded fields on success.
/// \return False iff \p returned is shorter than the fixed header.
bool ParseFsAttributeInfo(std::span<const uint8_t> returned,
                          FsAttributeInfo *out);

/// \brief Probes volume properties via `NtQueryVolumeInformationFile` on a
/// handle to \p root plus the single Category 4a 8.3 short-name probe.
///
/// Works uniformly across drive roots, subdirectories, directory junctions,
/// volume GUID paths, and UNC shares. On writable volumes the 8.3 probe must
/// complete: if it cannot create its transient file inside its transient
/// directory or query `FileBothDirectoryInformation`, `probe_succeeded` is
/// `false` and `probe_error` describes the failure. A root that rejects the
/// transient directory itself reports `short_names_enabled == false`; every
/// suite test then fails fatally creating its per-test temp directory.
/// \param root Normalized target root path.
/// \param os_info Host OS information snapshot.
/// \return Probed VolumeCapabilities (check `probe_succeeded`).
VolumeCapabilities ProbeVolumeCapabilities(const NormalizedRoot &root,
                                           const OsInfo &os_info);

} // namespace fstest::core
