// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file test_fixture.h
/// \brief `NtfsTestBase` GoogleTest fixture and precondition gating macros
/// shared by every NTFS conformance domain suite.

#pragma once

#include "fstest/core/capabilities.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/core/temp_dir.h"
#include "fstest/suite/test_config.h"
#include "fstest/suite/test_metadata.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fstest {

/// \brief Category 4b filesystem capabilities gated by `REQUIRE_FS_CAPABILITY`.
///
/// Tests run unconditionally by default (`respect_fs_capabilities == false`)
/// and skip only when `TestConfig::respect_fs_capabilities` is `true` and the
/// volume does not advertise the feature, or when an explicit
/// `CapabilityOverrides` entry disables it.
enum class FsCapability {
  /// Alternate Data Streams (`FILE_NAMED_STREAMS`).
  kNamedStreams,
  /// Sparse files (`FILE_SUPPORTS_SPARSE_FILES`).
  kSparseFiles,
  /// Hard links (`FILE_SUPPORTS_HARD_LINKS`).
  kHardLinks,
  /// Reparse points (`FILE_SUPPORTS_REPARSE_POINTS`).
  kReparsePoints,
  /// Persistent ACLs (`FILE_PERSISTENT_ACLS`).
  kPersistentAcls,
  /// Extended attributes (`FILE_SUPPORTS_EXTENDED_ATTRIBUTES`).
  kExtendedAttributes,
  /// Case-preserving names (`FILE_CASE_PRESERVED_NAMES`).
  kCasePreservedNames,
  /// Per-directory case sensitivity (`FileCaseSensitiveInformation`).
  kCaseSensitiveDir,
  /// File compression (`FILE_FILE_COMPRESSION`).
  kFileCompression,
  /// Object IDs (`FILE_SUPPORTS_OBJECT_IDS`).
  kObjectIds,
  /// Open by file ID (`FILE_SUPPORTS_OPEN_BY_FILE_ID`).
  kOpenByFileId,
  /// POSIX immediate unlink/rename (`FILE_SUPPORTS_POSIX_UNLINK_RENAME`).
  kPosixUnlinkRename,
  /// Per-volume disk quotas (`FILE_VOLUME_QUOTAS`).
  kVolumeQuotas,
  /// USN change journal (`FILE_SUPPORTS_USN_JOURNAL`).
  kUsnJournal,
  /// Opportunistic locks (oplocks).
  kOplocks,
  /// Directory change notifications (`ReadDirectoryChangesW`).
  kChangeNotifications,
  /// Composite check of core NTFS feature flags.
  kFullNtfsFlags,
};

/// \brief Masks host volume indexing/scrubbing policy attribute bits that may be
/// inherited from real NTFS parent folders so core attribute assertions remain
/// portable across host volumes.
inline constexpr DWORD MaskPolicyAttributes(DWORD attrs) noexcept {
  return attrs &
         ~(nt::kFileAttributeNotContentIndexed | nt::kFileAttributeNoScrubData);
}

/// \brief Base GoogleTest fixture for all NTFS conformance domain test suites.
///
/// Manages suite-level root resolution, volume capability probing, embedder
/// lifecycle hooks (`before_suite` / `after_suite` / `before_test` /
/// `after_test`), and per-test isolated temporary directories with zero-debris
/// verification on teardown.
class NtfsTestBase : public ::testing::Test {
public:
  /// \brief Resolves the target root, runs `before_suite` if configured,
  /// verifies root accessibility, and probes volume capabilities for the
  /// suite (re-probed for every suite; never cached across suites).
  static void SetUpTestSuite();

  /// Invokes `after_suite` if configured and `before_suite` was unset or
  /// succeeded.
  static void TearDownTestSuite();

protected:
  /// \brief Checks suite setup status, invokes `before_test`, and creates the
  /// isolated per-test temporary directory. Any failure here records
  /// `kFatalSetupProperty` so the test can never be masked as XFAIL.
  void SetUp() override;

  /// Verifies zero-debris cleanup of the per-test temporary directory and
  /// invokes `after_test` when applicable.
  void TearDown() override;

  /// Returns the active process-wide `TestConfig`.
  const TestConfig &Config() const noexcept;
  /// Returns cached host Windows OS version and privilege metadata.
  const core::OsInfo &Os() const noexcept;
  /// Returns the normalized active target root for the current suite.
  const core::NormalizedRoot &Root() const noexcept;
  /// Returns the probed (or pre-probed) `VolumeCapabilities` for the suite.
  const core::VolumeCapabilities &Caps() const noexcept;

  /// Returns `true` if the active root is a volume or mount root.
  bool IsVolumeRoot() const noexcept;
  /// Returns `true` if the active root is a local (non-UNC) volume.
  bool IsLocalVolume() const noexcept;

  /// Returns the effective sector size for `FILE_FLAG_NO_BUFFERING` tests:
  /// `capability_overrides.sector_size` if set, else
  /// `max(bytes_per_sector, physical_bytes_per_sector, 4096)`.
  uint32_t EffectiveSectorSize() const noexcept;

  /// Returns the per-test isolated `ScopedTempDir`.
  const core::ScopedTempDir &TempDir() const noexcept;
  /// Builds a standard Win32 child path inside `TempDir()`.
  std::wstring TestPath(std::wstring_view rel) const;
  /// Builds an extended-length (`\\?\...`) child path inside `TempDir()`.
  std::wstring TestExtendedPath(std::wstring_view rel) const;
  /// Builds a normalized NT (`\??\...`) child path via `core::DosPathToNtPath`,
  /// recording a test failure before falling back if conversion fails.
  std::wstring TestNtPath(std::wstring_view rel) const;
  /// Builds a verbatim NT (`\??\...`) child path without Win32 normalization.
  std::wstring TestVerbatimNtPath(std::wstring_view rel) const;
  /// Returns the active Win32 root path for the suite.
  std::wstring VolumeRootPath() const;

  /// Returns `true` if symlink creation is allowed (respecting overrides).
  bool CanCreateSymlinks() const noexcept;
  /// Returns `true` if 8.3 short names are enabled (respecting overrides).
  bool ShortNamesEnabled() const noexcept;
  /// Returns `true` if the current process token holds `privilege_name`.
  bool HasPrivilege(const wchar_t *privilege_name) const;
  /// Evaluates whether a test gated on `cap` should be skipped, populating
  /// `*reason_out` with a diagnostic explanation when returning `true`.
  bool ShouldSkipForFsCapability(FsCapability cap,
                                 std::string *reason_out) const;

  /// Records `kSkipCategoryProperty` on the current GoogleTest `TestResult`.
  void RecordSkipCategory(const char *category);

private:
  core::ScopedTempDir temp_dir_;
  bool before_test_succeeded_ = false;
};

} // namespace fstest

// ---------------------------------------------------------------------------
// Precondition Gating Macros
// ---------------------------------------------------------------------------

/// \brief Skips the test unless the target root is a volume or mount root.
#define REQUIRE_VOLUME_ROOT()                                                  \
  do {                                                                         \
    if (!this->IsVolumeRoot()) {                                               \
      this->RecordSkipCategory("volume_root");                                 \
      GTEST_SKIP() << "Requires target root to be a volume or mount root.";    \
    }                                                                          \
  } while (false)

/// \brief Skips the test unless the target root is on a local (non-UNC) volume.
#define REQUIRE_LOCAL_VOLUME()                                                 \
  do {                                                                         \
    if (!this->IsLocalVolume()) {                                              \
      this->RecordSkipCategory("local_volume");                                \
      GTEST_SKIP() << "Requires a local (non-UNC) volume target.";             \
    }                                                                          \
  } while (false)

/// \brief Skips the test unless the host Windows build is at least `min_build`
/// (manifest-proof via `RtlGetVersion`).
#define REQUIRE_MIN_OS_BUILD(min_build)                                        \
  do {                                                                         \
    const uint32_t fstest_min_b_ = static_cast<uint32_t>(min_build);           \
    if (this->Os().build < fstest_min_b_) {                                    \
      this->RecordSkipCategory("os_build");                                    \
      GTEST_SKIP() << "Requires Windows build >= " << fstest_min_b_            \
                   << " (host build: " << this->Os().build << ").";            \
    }                                                                          \
  } while (false)

/// \brief Skips the test unless the host Windows build is at most `max_build`
/// (manifest-proof via `RtlGetVersion`).
#define REQUIRE_MAX_OS_BUILD(max_build)                                        \
  do {                                                                         \
    const uint32_t fstest_max_b_ = static_cast<uint32_t>(max_build);           \
    if (this->Os().build > fstest_max_b_) {                                    \
      this->RecordSkipCategory("os_build");                                    \
      GTEST_SKIP() << "Requires Windows build <= " << fstest_max_b_            \
                   << " (host build: " << this->Os().build << ").";            \
    }                                                                          \
  } while (false)

/// \brief Skips the test unless the current process runs with elevated Administrator
/// privileges (host-only check; never probes target `root_path`).
#define REQUIRE_ADMIN()                                                        \
  do {                                                                         \
    if (!this->Os().is_elevated) {                                             \
      this->RecordSkipCategory("privilege");                                   \
      GTEST_SKIP() << "Requires elevated Administrator privileges.";           \
    }                                                                          \
  } while (false)

/// \brief Skips the test unless the current process token holds `priv_name`.
#define REQUIRE_PRIVILEGE(priv_name)                                           \
  do {                                                                         \
    if (!this->HasPrivilege(priv_name)) {                                      \
      this->RecordSkipCategory("privilege");                                   \
      GTEST_SKIP() << "Requires host token privilege: "                        \
                   << ::fstest::core::WideToUtf8(priv_name) << ".";            \
    }                                                                          \
  } while (false)

/// \brief Skips the test unless symlink creation is available via
/// `SeCreateSymbolicLinkPrivilege` or Windows Developer Mode (respecting
/// `CapabilityOverrides::can_create_symlinks`).
#define REQUIRE_SYMLINK_CREATION()                                             \
  do {                                                                         \
    if (!this->CanCreateSymlinks()) {                                          \
      this->RecordSkipCategory("privilege");                                   \
      GTEST_SKIP() << "Requires SeCreateSymbolicLinkPrivilege or Windows "     \
                      "Developer Mode on the host.";                           \
    }                                                                          \
  } while (false)

/// \brief Skips the test unless the host OS supports per-directory case sensitivity.
#define REQUIRE_HOST_CASE_SENSITIVE_DIR()                                      \
  do {                                                                         \
    if (!this->Os().can_set_case_sensitive_dir) {                              \
      this->RecordSkipCategory("os_feature");                                  \
      GTEST_SKIP()                                                             \
          << "Per-directory case sensitivity is not enabled on this host OS."; \
    }                                                                          \
  } while (false)

/// \brief Category 4a host-volume-policy precondition: skips unless 8.3 short name
/// generation is enabled on the target volume (respecting overrides). A failed
/// short-name probe fails suite setup instead of reaching this skip.
#define REQUIRE_SHORT_NAMES_ENABLED()                                          \
  do {                                                                         \
    if (!this->ShortNamesEnabled()) {                                          \
      this->RecordSkipCategory("volume_policy");                               \
      GTEST_SKIP() << "8.3 short name generation is disabled or unsupported "  \
                      "on this volume.";                                       \
    }                                                                          \
  } while (false)

/// \brief Category 4b filesystem feature capability precondition. Runs unconditionally
/// by default (`respect_fs_capabilities == false`) unless explicitly disabled
/// via `CapabilityOverrides`.
#define REQUIRE_FS_CAPABILITY(cap)                                             \
  do {                                                                         \
    std::string fstest_cap_reason_;                                            \
    if (this->ShouldSkipForFsCapability((cap), &fstest_cap_reason_)) {         \
      this->RecordSkipCategory("fs_capability");                               \
      GTEST_SKIP() << fstest_cap_reason_;                                      \
    }                                                                          \
  } while (false)

/// \brief Shorthand for `REQUIRE_FS_CAPABILITY(FsCapability::kFullNtfsFlags)`.
#define REQUIRE_FULL_NTFS_FLAGS()                                              \
  REQUIRE_FS_CAPABILITY(::fstest::FsCapability::kFullNtfsFlags)
