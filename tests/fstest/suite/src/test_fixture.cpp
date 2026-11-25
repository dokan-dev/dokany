// SPDX-License-Identifier: MIT

#include "fstest/suite/test_fixture.h"

#include "fstest/core/status.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace fstest {
namespace {

struct SuiteRuntimeState {
  bool setup_failed = false;
  bool setup_skipped = false;
  std::string setup_error;
  std::string active_suite_name;
  bool before_suite_ran = false;
  core::NormalizedRoot active_root;
  core::VolumeCapabilities active_caps;
};

SuiteRuntimeState &RuntimeState() {
  static SuiteRuntimeState state;
  return state;
}

std::wstring ReadEnvVarW(const wchar_t *name) {
  const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::wstring value(needed, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
  if (written == 0 || written >= needed) {
    return {};
  }
  value.resize(written);
  return value;
}

TestLifecycleInfo CurrentLifecycleInfo() {
  TestLifecycleInfo info{};
  const ::testing::TestInfo *ti =
      ::testing::UnitTest::GetInstance()->current_test_info();
  if (ti != nullptr) {
    info.suite_name = ti->test_suite_name();
    info.test_name = ti->name();
    info.full_name = info.suite_name + "." + info.test_name;
  }
  return info;
}

const char *FsCapabilityName(FsCapability cap) noexcept {
  switch (cap) {
  case FsCapability::kNamedStreams:
    return "NamedStreams (FILE_NAMED_STREAMS)";
  case FsCapability::kSparseFiles:
    return "SparseFiles (FILE_SUPPORTS_SPARSE_FILES)";
  case FsCapability::kHardLinks:
    return "HardLinks (FILE_SUPPORTS_HARD_LINKS)";
  case FsCapability::kReparsePoints:
    return "ReparsePoints (FILE_SUPPORTS_REPARSE_POINTS)";
  case FsCapability::kPersistentAcls:
    return "PersistentAcls (FILE_PERSISTENT_ACLS)";
  case FsCapability::kExtendedAttributes:
    return "ExtendedAttributes (FILE_SUPPORTS_EXTENDED_ATTRIBUTES)";
  case FsCapability::kCasePreservedNames:
    return "CasePreservedNames (FILE_CASE_PRESERVED_NAMES)";
  case FsCapability::kCaseSensitiveDir:
    return "CaseSensitiveDir (FileCaseSensitiveInformation)";
  case FsCapability::kFileCompression:
    return "FileCompression (FILE_FILE_COMPRESSION)";
  case FsCapability::kObjectIds:
    return "ObjectIds (FILE_SUPPORTS_OBJECT_IDS)";
  case FsCapability::kOpenByFileId:
    return "OpenByFileId (FILE_SUPPORTS_OPEN_BY_FILE_ID)";
  case FsCapability::kPosixUnlinkRename:
    return "PosixUnlinkRename (FILE_SUPPORTS_POSIX_UNLINK_RENAME)";
  case FsCapability::kOplocks:
    return "Oplocks";
  case FsCapability::kChangeNotifications:
    return "ChangeNotifications";
  case FsCapability::kFullNtfsFlags:
    return "FullNtfsFeatureFlags";
  }
  return "UnknownCapability";
}

std::optional<bool> LookupOverride(const CapabilityOverrides &ov,
                                   FsCapability cap) noexcept {
  switch (cap) {
  case FsCapability::kNamedStreams:
    return ov.named_streams;
  case FsCapability::kSparseFiles:
    return ov.sparse_files;
  case FsCapability::kHardLinks:
    return ov.hard_links;
  case FsCapability::kReparsePoints:
    return ov.reparse_points;
  case FsCapability::kPersistentAcls:
    return ov.persistent_acls;
  case FsCapability::kExtendedAttributes:
    return ov.extended_attributes;
  case FsCapability::kCasePreservedNames:
    return ov.case_preserved_names;
  case FsCapability::kCaseSensitiveDir:
    return ov.case_sensitive_dir;
  case FsCapability::kFileCompression:
    return ov.file_compression;
  case FsCapability::kObjectIds:
    return ov.object_ids;
  case FsCapability::kOpenByFileId:
    return ov.open_by_file_id;
  case FsCapability::kPosixUnlinkRename:
    return ov.posix_unlink_rename;
  case FsCapability::kOplocks:
    return ov.oplocks;
  case FsCapability::kChangeNotifications:
    return ov.change_notifications;
  case FsCapability::kFullNtfsFlags:
    return std::nullopt;
  }
  return std::nullopt;
}

bool LookupProbedCapability(const core::VolumeCapabilities &caps,
                            FsCapability cap) noexcept {
  switch (cap) {
  case FsCapability::kNamedStreams:
    return caps.named_streams;
  case FsCapability::kSparseFiles:
    return caps.sparse_files;
  case FsCapability::kHardLinks:
    return caps.hard_links;
  case FsCapability::kReparsePoints:
    return caps.reparse_points;
  case FsCapability::kPersistentAcls:
    return caps.persistent_acls;
  case FsCapability::kExtendedAttributes:
    return caps.extended_attributes;
  case FsCapability::kCasePreservedNames:
    return caps.case_preserved_names;
  case FsCapability::kCaseSensitiveDir:
    return caps.case_sensitive_dir;
  case FsCapability::kFileCompression:
    return caps.file_compression;
  case FsCapability::kObjectIds:
    return caps.object_ids;
  case FsCapability::kOpenByFileId:
    return caps.open_by_file_id;
  case FsCapability::kPosixUnlinkRename:
    return caps.posix_unlink_rename;
  case FsCapability::kOplocks:
    return caps.oplocks;
  case FsCapability::kChangeNotifications:
    return caps.change_notifications;
  case FsCapability::kFullNtfsFlags:
    return caps.named_streams && caps.persistent_acls && caps.sparse_files &&
           caps.reparse_points && caps.hard_links && caps.extended_attributes &&
           caps.case_preserved_names &&
           ((caps.fs_flags & nt::kFileUnicodeOnDisk) != 0);
  }
  return false;
}

} // namespace

void NtfsTestBase::SetUpTestSuite() {
  auto &state = RuntimeState();
  state = SuiteRuntimeState{};

  const ::testing::TestSuite *suite =
      ::testing::UnitTest::GetInstance()->current_test_suite();
  if (suite != nullptr && suite->name() != nullptr) {
    state.active_suite_name = suite->name();
  }

  const TestConfig &cfg = GetSuiteConfig();
  std::wstring effective_root = cfg.root_path;
  bool has_override_root = false;

  if (cfg.hooks.before_suite) {
    std::wstring override_root;
    std::string hook_err;
    if (!cfg.hooks.before_suite(state.active_suite_name, &override_root,
                                &hook_err)) {
      state.setup_failed = true;
      state.setup_error =
          hook_err.empty() ? "before_suite hook returned false." : hook_err;
      return;
    }
    if (::testing::Test::IsSkipped()) {
      state.setup_skipped = true;
      state.setup_error = hook_err;
      return;
    }
    state.before_suite_ran = true;
    if (!override_root.empty()) {
      has_override_root = true;
      effective_root = std::move(override_root);
    }
  }

  if (effective_root.empty()) {
    effective_root = ReadEnvVarW(L"FSTEST_ROOT");
  }
  if (effective_root.empty()) {
    state.setup_failed = true;
    state.setup_error =
        "No target root_path configured (pass --root=<path> or set "
        "FSTEST_ROOT).";
    return;
  }

  auto norm_res = core::NormalizeRootPath(effective_root, cfg.is_volume_root);
  if (!norm_res) {
    state.setup_failed = true;
    state.setup_error = "Invalid root path '" +
                        core::WideToUtf8(effective_root) +
                        "': " + core::FormatWin32Error(norm_res.error);
    return;
  }

  // Probe reachability and capabilities for every suite (never cached) so an
  // unmount or remount between suites can never be masked by a stale probe.
  const DWORD probe_err = core::ProbeRootDirectory(*norm_res.value);
  if (probe_err == ERROR_DIRECTORY) {
    state.setup_failed = true;
    state.setup_error = "Target root '" +
                        core::WideToUtf8(norm_res.value->win32_root) +
                        "' is not a directory.";
    return;
  }
  if (probe_err != ERROR_SUCCESS) {
    state.setup_failed = true;
    state.setup_error =
        "Target root '" + core::WideToUtf8(norm_res.value->win32_root) +
        "' is not accessible: " + core::FormatWin32Error(probe_err);
    return;
  }

  core::VolumeCapabilities caps{};
  if (!has_override_root && cfg.preprobed_capabilities.has_value() &&
      cfg.preprobed_capabilities->probe_succeeded) {
    caps = *cfg.preprobed_capabilities;
  } else {
    caps = core::ProbeVolumeCapabilities(*norm_res.value, core::GetOsInfo());
    if (!caps.probe_succeeded) {
      state.setup_failed = true;
      state.setup_error = caps.probe_error.empty()
                              ? "Volume capability probe failed."
                              : caps.probe_error;
      return;
    }
  }

  state.active_root = std::move(*norm_res.value);
  state.active_caps = std::move(caps);
}

void NtfsTestBase::TearDownTestSuite() {
  auto &state = RuntimeState();
  const TestConfig &cfg = GetSuiteConfig();
  if (cfg.hooks.after_suite &&
      (!cfg.hooks.before_suite || state.before_suite_ran)) {
    cfg.hooks.after_suite(state.active_suite_name);
  }
  state.before_suite_ran = false;
}

void NtfsTestBase::SetUp() {
  before_test_succeeded_ = false;

  const auto &state = RuntimeState();
  if (state.setup_skipped) {
    GTEST_SKIP() << state.setup_error;
  }
  if (state.setup_failed) {
    RecordProperty(kFatalSetupProperty, "1");
    FAIL() << "Suite setup failed: " << state.setup_error;
  }

  const TestConfig &cfg = GetSuiteConfig();
  if (cfg.hooks.before_test) {
    std::string hook_err;
    if (!cfg.hooks.before_test(CurrentLifecycleInfo(), &hook_err)) {
      RecordProperty(kFatalSetupProperty, "1");
      FAIL() << "before_test hook failed: "
             << (hook_err.empty() ? "(no message)" : hook_err);
    }
    if (::testing::Test::IsSkipped()) {
      return;
    }
    before_test_succeeded_ = true;
  }

  core::CleanupOptions cleanup_opts{};
  cleanup_opts.retries = cfg.timing.cleanup_retries;
  cleanup_opts.initial_backoff_ms = cfg.timing.cleanup_initial_backoff_ms;

  auto dir_res = core::ScopedTempDir::Create(state.active_root.win32_root,
                                             cfg.temp_prefix, cleanup_opts);
  if (!dir_res) {
    RecordProperty(kFatalSetupProperty, "1");
    FAIL() << "Failed to create isolated temp directory under '"
           << core::WideToUtf8(state.active_root.win32_root)
           << "': " << core::FormatWin32Error(dir_res.error);
  }

  temp_dir_ = std::move(*dir_res.value);
}

void NtfsTestBase::TearDown() {
  const auto &state = RuntimeState();
  if (state.setup_failed || state.setup_skipped) {
    return;
  }

  if (temp_dir_.IsActive()) {
    std::string cleanup_err;
    if (!temp_dir_.Cleanup(&cleanup_err)) {
      RecordProperty(kFatalTeardownProperty, "1");
      ADD_FAILURE() << "Per-test temp directory teardown failed (debris left "
                       "on target filesystem): "
                    << cleanup_err;
    }
  }

  const TestConfig &cfg = GetSuiteConfig();
  if (cfg.hooks.after_test &&
      (!cfg.hooks.before_test || before_test_succeeded_)) {
    std::string hook_err;
    const bool test_passed = !::testing::Test::HasFailure();
    if (!cfg.hooks.after_test(CurrentLifecycleInfo(), test_passed, &hook_err)) {
      RecordProperty(kFatalTeardownProperty, "1");
      ADD_FAILURE() << "after_test hook failed: "
                    << (hook_err.empty() ? "(no message)" : hook_err);
    }
  }
}

const TestConfig &NtfsTestBase::Config() const noexcept {
  return GetSuiteConfig();
}

const core::OsInfo &NtfsTestBase::Os() const noexcept {
  return core::GetOsInfo();
}

const core::NormalizedRoot &NtfsTestBase::Root() const noexcept {
  return RuntimeState().active_root;
}

const core::VolumeCapabilities &NtfsTestBase::Caps() const noexcept {
  return RuntimeState().active_caps;
}

bool NtfsTestBase::IsVolumeRoot() const noexcept {
  return Root().is_volume_root;
}

bool NtfsTestBase::IsLocalVolume() const noexcept {
  return core::IsLocalKind(Root().kind);
}

uint32_t NtfsTestBase::EffectiveSectorSize() const noexcept {
  const auto &ov = Config().capability_overrides;
  if (ov.sector_size.has_value() && *ov.sector_size > 0) {
    return *ov.sector_size;
  }
  const auto &caps = Caps();
  return std::max(
      {caps.bytes_per_sector, caps.physical_bytes_per_sector, uint32_t{4096}});
}

const core::ScopedTempDir &NtfsTestBase::TempDir() const noexcept {
  return temp_dir_;
}

std::wstring NtfsTestBase::TestPath(std::wstring_view rel) const {
  return temp_dir_.ChildPath(rel);
}

std::wstring NtfsTestBase::TestExtendedPath(std::wstring_view rel) const {
  return temp_dir_.ChildExtendedPath(rel);
}

std::wstring NtfsTestBase::TestNtPath(std::wstring_view rel) const {
  const std::wstring win32_path = TestPath(rel);
  auto nt_res = core::DosPathToNtPath(win32_path);
  if (nt_res) {
    return std::move(*nt_res.value);
  }
  ADD_FAILURE() << "core::DosPathToNtPath('" << core::WideToUtf8(win32_path)
                << "') failed with " << core::FormatNtStatus(nt_res.status)
                << "; falling back to TestVerbatimNtPath.";
  return TestVerbatimNtPath(rel);
}

std::wstring NtfsTestBase::TestVerbatimNtPath(std::wstring_view rel) const {
  return core::VerbatimToNtPath(TestExtendedPath(rel));
}

std::wstring NtfsTestBase::VolumeRootPath() const { return Root().win32_root; }

bool NtfsTestBase::CanCreateSymlinks() const noexcept {
  const auto &ov = Config().capability_overrides;
  if (ov.can_create_symlinks.has_value()) {
    return *ov.can_create_symlinks;
  }
  return Os().can_create_symlinks;
}

bool NtfsTestBase::ShortNamesEnabled() const noexcept {
  const auto &ov = Config().capability_overrides;
  if (ov.short_names.has_value()) {
    return *ov.short_names;
  }
  return Caps().short_names_enabled;
}

bool NtfsTestBase::HasPrivilege(const wchar_t *privilege_name) const {
  auto res = core::HasPrivilegeInToken(privilege_name);
  return res && *res.value;
}

bool NtfsTestBase::ShouldSkipForFsCapability(FsCapability cap,
                                             std::string *reason_out) const {
  const TestConfig &cfg = Config();

  if (cap == FsCapability::kFullNtfsFlags) {
    static constexpr FsCapability kFullNtfsComponents[] = {
        FsCapability::kNamedStreams,       FsCapability::kPersistentAcls,
        FsCapability::kSparseFiles,        FsCapability::kReparsePoints,
        FsCapability::kHardLinks,          FsCapability::kExtendedAttributes,
        FsCapability::kCasePreservedNames,
    };
    for (const FsCapability component : kFullNtfsComponents) {
      const std::optional<bool> comp_override =
          LookupOverride(cfg.capability_overrides, component);
      if (comp_override.has_value() && !*comp_override) {
        if (reason_out != nullptr) {
          *reason_out = std::string("Capability explicitly disabled via "
                                    "CapabilityOverrides: ") +
                        FsCapabilityName(cap) +
                        " (disabled component: " + FsCapabilityName(component) +
                        ")";
        }
        return true;
      }
    }

    if (!cfg.respect_fs_capabilities) {
      return false;
    }

    const core::VolumeCapabilities &caps = Caps();
    for (const FsCapability component : kFullNtfsComponents) {
      const std::optional<bool> comp_override =
          LookupOverride(cfg.capability_overrides, component);
      const bool supported = comp_override.has_value()
                                 ? *comp_override
                                 : LookupProbedCapability(caps, component);
      if (!supported) {
        if (reason_out != nullptr) {
          *reason_out =
              std::string("Target filesystem does not advertise capability: ") +
              FsCapabilityName(cap) +
              " (missing: " + FsCapabilityName(component) + ")";
        }
        return true;
      }
    }
    if ((caps.fs_flags & nt::kFileUnicodeOnDisk) == 0) {
      if (reason_out != nullptr) {
        *reason_out =
            std::string("Target filesystem does not advertise capability: ") +
            FsCapabilityName(cap) + " (missing: FILE_UNICODE_ON_DISK)";
      }
      return true;
    }
    return false;
  }

  const std::optional<bool> override_val =
      LookupOverride(cfg.capability_overrides, cap);
  if (override_val.has_value()) {
    if (!*override_val) {
      if (reason_out != nullptr) {
        *reason_out = std::string("Capability explicitly disabled via "
                                  "CapabilityOverrides: ") +
                      FsCapabilityName(cap);
      }
      return true;
    }
    return false;
  }

  if (!cfg.respect_fs_capabilities) {
    return false;
  }

  if (!LookupProbedCapability(Caps(), cap)) {
    if (reason_out != nullptr) {
      *reason_out =
          std::string("Target filesystem does not advertise capability: ") +
          FsCapabilityName(cap);
    }
    return true;
  }
  return false;
}

void NtfsTestBase::RecordSkipCategory(const char *category) {
  RecordProperty(kSkipCategoryProperty, category);
}

} // namespace fstest
