<!-- markdownlint-disable MD043 -->

# NTFS Conformance Test Framework (`tests/fstest/`)

A standalone, clean-room **C++20 + GoogleTest** NTFS conformance test framework
for Windows filesystems. It verifies Win32 and native NT (`ntdll.dll`) semantics
across **12 filesystem domains** (16 balanced test files, each test carrying a
`// Spec:` citation) alongside unit and CLI subprocess self-tests
(`fstest_self_test.exe`). It runs identically against real NTFS volumes, Dokan
filesystems (drive letters, directory junctions, removable/Mount Manager mounts,
UNC shares), and subdirectories on virtual drives.

Licensed under the [MIT License](LICENSE).

---

## Architecture

The framework is organized into three strictly decoupled layers:

<!-- markdownlint-disable MD013 -->

| Layer | Target | Dependencies | Purpose |
| :--- | :--- | :--- | :--- |
| **Layer 1 (`core/`)** | `ntfs_helpers` | C++20 STL, Win32 (`advapi32.lib`), dynamic `ntdll.dll` | Exception-free, RTTI-free Win32/NT helpers, clean-room `fstest::nt` ABI types, RAII handles, path normalization, security/SDDL helpers, manifest-proof `RtlGetVersion` OS detection, capability probing, and forced per-test temp-directory cleanup (`ForceRemoveAll` / `ScopedTempDir`). |
| **Layer 2 (`suite/`)** | `ntfs_suite` (`ntfs_suite_base` + 12 domains / 16 files) | `ntfs_helpers`, `GTest::gtest` | Programmatically configured GoogleTest conformance suites (`fstest::ConfigureSuite`, `fstest::LinkAllSuites`) with zero CLI or filesystem-driver coupling. |
| **Layer 3 (`cli/`)** | `dokan_fstest.exe`, `fstest_self_test.exe` | `ntfs_suite`, `ntfs_helpers`, `GTest::gtest`, `nlohmann/json` | CLI runner, strict baseline/xfail inheritance engine, custom GTest listener, 80-column Conformance Scorecard + JSON reporter, safe `--cleanup-dir` and `--validate-baseline` utilities, and unit/CLI subprocess self-tests. |

<!-- markdownlint-enable MD013 -->

### Covered NTFS Domains

1. `create_open` (`CreateOpenTest`) — Win32 & `NtCreateFile` dispositions,
   access masks, share modes, `DELETE_ON_CLOSE`, `FILE_SUPERSEDE` vs
   `FILE_OVERWRITE_IF`, handle-relative opens.
2. `delete` (`DeleteTest`) — Legacy delete-pending
   (`FileDispositionInformation`), POSIX unlink
   (`FileDispositionInformationEx`), `IGNORE_READONLY`, non-empty/read-only
   dirs, mapped-file protection.
3. `dir_enum` (`DirEnumTest`) — `FindFirstFile[Ex]W`,
   `NtQueryDirectoryFile[Ex]` across directory info classes, small-buffer
   pagination, large-directory strict multiset verification, Win32 & DOS
   wildcards.
4. `rename_move` (`RenameMoveTest`) — `MoveFileExW`,
   `FileRenameInformation[Ex]`, POSIX replace, case-only rename, open-handle
   sharing rules, subtree checks, `ReplaceFileW`.
5. `data_io` (`DataIoTest`) — Sync/overlapped/append I/O, `SetEndOfFile`,
   `FileAllocationInformation`, `NO_BUFFERING` / `WRITE_THROUGH`, memory-mapped
   coherence, sparse files, compression.
6. `locks_oplocks` (`LocksOplocksTest`) — `LockFileEx`/`UnlockFileEx`
   shared/exclusive/overlapping/blocking locks, auto-release on close,
   `FSCTL_REQUEST_OPLOCK` breaks.
7. `timestamps_attrs` (`TimestampsAttrsTest`) — 100ns `FILETIME` precision,
   `ChangeTime` vs `LastWriteTime`, `-1`/`-2` freeze/unfreeze sentinels,
   attribute transitions.
8. `streams` (`StreamsTest`) — File & directory Alternate Data Streams (ADS),
   `FindFirstStreamW`, `FileStreamInformation` strict multiset, stream rename &
   delete, independent stream sizing/locking.
9. `security_acl` (`SecurityAclTest`) — SDDL creation/query/set, ACE
   inheritance, DACL enforcement (`DELETE` vs `FILE_DELETE_CHILD`,
   `FILE_TRAVERSE` via restricted token impersonation), SACLs.
10. `links_reparse` (`LinksReparseTest`) — Hard links (`CreateHardLinkW`,
    `NumberOfLinks`, `FileLinkInformation`), symlinks, local junctions,
    `FSCTL_{SET,GET,DELETE}_REPARSE_POINT`, custom GUID reparse tags.
11. `naming_paths` (`NamingPathsTest`) — Case-insensitive lookup & preservation,
    Unicode/BMP/surrogates, trailing dots/spaces & DOS devices (Win32 vs
    verbatim `\\?\`), long paths, 8.3 short names, per-directory case
    sensitivity.
12. `info_queries` (`InfoQueriesTest`) — Volume & file information classes,
    `FileIndex` / `OpenFileById`, `GetFinalPathNameByHandleW`,
    `ReadDirectoryChangesW`, Extended Attributes (EA), Object IDs.

### Scope & Explicit Non-Goals

The suite verifies **observable user-mode NTFS contracts** exposed through
Win32 and native NT (`ntdll.dll`) APIs across the 12 domains above (16 balanced
test files, each test carrying a `// Spec:` citation): handle lifecycle,
sharing and access checks, deletion and rename semantics, buffered and
unbuffered I/O, memory-mapped views, locking and oplocks, timestamps, streams,
ACLs, links and reparse points, path normalization, metadata tunneling
(automatically skipped with `skip_category="os_policy"` when the host disables
tunneling via `MaximumTunnelEntries == 0`), and file/volume queries. All
applicable conformance tests must pass unconditionally (`--strict`) on real
NTFS volumes; note that CI `--strict` runs target a subdirectory
(`C:\fstest_ntfs`), which skips the `REQUIRE_VOLUME_ROOT()` tests as well as
legacy-OS and 8.3 short-name-gated tests when disabled on the host volume.
Framework internals, baseline resolution, scorecard generation, and CLI
argument/subprocess semantics are independently verified by unit and CLI
subprocess self-tests in `fstest_self_test.exe`.

The following NTFS or volume-stack subsystems are **deliberately out of
scope** because they depend on kernel transaction managers, disk-filter
services, or host-global state outside a user-mode filesystem contract:

- **Transactional NTFS (TxF)** — deprecated kernel transaction APIs
  (`CreateFileTransactedW`, KTM rollback).
- **USN Change Journal administration** — volume-wide journal administration
  FSCTLs (`FSCTL_CREATE_USN_JOURNAL`, `FSCTL_READ_USN_JOURNAL`; per-file
  `FSCTL_READ_FILE_USN_DATA` is covered in `info_queries`).
- **Encrypting File System (EFS)** — certificate/DPAPI key-management
  encryption (`EncryptFileW`, `FILE_ATTRIBUTE_ENCRYPTED`).
- **NTFS Disk Quotas** — per-SID volume quota tracking and enforcement
  (`IDiskQuotaControl`, `IRP_MJ_QUERY_QUOTA` / `SET_QUOTA`).
- **VSS and BitLocker** — Volume Shadow Copy snapshots and full-volume disk
  encryption.

---

## Building

`tests/fstest/` uses standalone CMake (`>= 3.22`) and is deliberately excluded
from `dokan.sln`.

### Recommended Build Script (Windows)

```powershell
# Release x64 static (/MT) build + self-tests + import portability gate
.\scripts\build_fstest.ps1 `
  -Configuration Release `
  -VerifyStaticCrt `
  -WarningsAsErrors
```

Outputs are placed at:

- `tests\fstest\build\bin\Release\dokan_fstest.exe`
- `tests\fstest\build\bin\Release\fstest_self_test.exe`

#### `scripts/build_fstest.ps1` Parameters

<!-- markdownlint-disable MD013 -->

| Parameter | Default | Description |
| :--- | :--- | :--- |
| `-Configuration` | `Release` | Build configuration (`Release` or `Debug`). |
| `-BuildDir` | `tests\fstest\build` | Out-of-source CMake build directory. |
| `-Compiler` | `msvc` | Compiler toolchain (`msvc` or `clang-cl`; prefers VS-bundled LLVM `clang-cl.exe`). |
| `-CacheDir` | *(none)* | Local cache directory for pinned GoogleTest and `nlohmann/json` archives (SHA-256 verified against `deps.json`). |
| `-GTestSourceDir` | *(none)* | Local GoogleTest source directory (passed as `FSTEST_GTEST_SOURCE_DIR`). |
| `-JsonSourceDir` | *(none)* | Local `nlohmann/json` root or `include/` directory (passed as `FSTEST_JSON_SOURCE_DIR`). |
| `-SkipCli` | `false` | Sets `FSTEST_BUILD_CLI=OFF` and builds only `ntfs_helpers`, `ntfs_suite_base`, and `ntfs_suite` static libraries. |
| `-SkipSelfTest` | `false` | Skips executing `fstest_self_test.exe` after building. |
| `-VerifyStaticCrt` | `false` | Runs `dumpbin /imports` on `dokan_fstest.exe` to reject dynamic CRT/`dokan*` DLLs and enforce the Windows Server 2016 base-OS allowlist (`KERNEL32.dll`, `ADVAPI32.dll`, `ntdll.dll`, `bcrypt.dll`, `bcryptprimitives.dll`). Cannot be combined with `-SkipCli`. |
| `-WarningsAsErrors` | `false` | Sets `FSTEST_WERROR=ON` (`/WX` on MSVC/`clang-cl`). |
| `-CMakeExtraArgs` | `@()` | Extra CMake configure flags forwarded verbatim (for example `-DFSTEST_WIN32_WINNT=0x0602`). |

<!-- markdownlint-enable MD013 -->

### Direct CMake Build (Presets / Offline)

`CMakePresets.json` lives inside `tests/fstest/`, so invoke `cmake --preset`
and `cmake --build --preset` with `tests\fstest` as the working directory inside
a Visual Studio x64 Developer Prompt:

```powershell
Push-Location tests\fstest
cmake --preset msvc-x64-release
cmake --build --preset msvc-x64-release
.\build\bin\Release\fstest_self_test.exe
Pop-Location
```

Available presets in `tests/fstest/CMakePresets.json`:

- `msvc-x64-release` — Ninja + `cl` Release (`/MT`) in `tests/fstest/build`.
- `msvc-x64-debug` — Ninja + `cl` Debug (`/MTd`) in `tests/fstest/build-debug`.
- `clang-cl-x64` — Ninja + `clang-cl` Release (`/MT`, `FSTEST_WERROR=ON`) in
  `tests/fstest/build-clang-cl`.
- `clang-cl-portability` — Ninja + `clang-cl` Layer 1+2 portability build
  (`FSTEST_BUILD_CLI=OFF`, `FSTEST_WIN32_WINNT=0x0602`, empty
  `FSTEST_NTDDI_VERSION`, `FSTEST_DEFINE_UNDERSCORE_UNICODE=OFF`,
  `FSTEST_CORE_NO_EXCEPTIONS=ON`, `FSTEST_WERROR=ON`) targeting `ntfs_helpers`
  and `ntfs_suite`.

#### Dependency Resolution (`deps.json`: GoogleTest & `nlohmann/json`)

Dependency versions, archive URLs, filenames, and SHA-256 hashes are pinned
once in [`deps.json`](deps.json) (read natively by both `CMakeLists.txt` and
`scripts/build_fstest.ps1`). Both dependencies support 3-tier resolution
(online `FetchContent` with pinned SHA-256 by default, cached tarball/zip URLs,
local source directories, or system packages):

- Local offline source trees:
  `-DFSTEST_GTEST_SOURCE_DIR=<path> -DFSTEST_JSON_SOURCE_DIR=<path>`
- Cached archives:
  `-DFSTEST_GTEST_URL=C:/cache/googletest-1.17.0.tar.gz` and
  `-DFSTEST_JSON_URL=C:/cache/nlohmann-json-3.12.0-include.zip`
- System packages:
  `-DFSTEST_USE_SYSTEM_GTEST=ON -DFSTEST_USE_SYSTEM_JSON=ON`

---

## Running Tests

### Real NTFS Strict Conformance Check

```powershell
mkdir C:\fstest_ntfs
.\tests\fstest\build\bin\Release\dokan_fstest.exe `
  --root=C:\fstest_ntfs `
  --strict `
  --json-out=ntfs_strict.json

# Safe cleanup (no reparse following; handles \\?\ paths and locked DACLs)
.\tests\fstest\build\bin\Release\dokan_fstest.exe --cleanup-dir=C:\fstest_ntfs
```

### Running Against a Mounted Filesystem with a Baseline

```powershell
.\tests\fstest\build\bin\Release\dokan_fstest.exe `
  --root=M:\ `
  --baseline=tests\fstest\baselines\dokan_mirror.json `
  --target=mirror_drive `
  --target-arch=x64 `
  --json-out=mirror_drive.json
```

### Running Against a Subdirectory or Non-NTFS Virtual Drive

When `--root` points to a subdirectory (e.g., `V:\Work\sandbox`), volume-root
auto-detection (`GetVolumePathNameW`) automatically skips the volume-root-only
tests while running all subdirectory-safe tests inside isolated per-test
directories (`fst_<pid>_<seq>`).

```powershell
.\tests\fstest\build\bin\Release\dokan_fstest.exe `
  --root="V:\Work\sandbox" `
  --respect-fs-caps `
  --large-dir-count=100 `
  --async-timeout-ms=10000 `
  --baseline=my_vfs_baseline.json
```

### Common CLI Flags

<!-- markdownlint-disable MD013 -->

| Flag | Default | Description |
| :--- | :--- | :--- |
| `--root=<path>` | *(required)* | Target path (drive root, junction, subdirectory, or UNC; fallback: `FSTEST_ROOT`). |
| `--cleanup-dir=<path>` | *(none)* | Safely delete directory tree (non-following reparse points; refuses volume/UNC roots & system dirs). Standalone utility mode. |
| `--validate-baseline=<path>` | *(none)* | Validate baseline manifest schema, inheritance, and every test ID (including abstract targets and inactive OS/arch entries) against all compiled tests; exits `0` or `2`. Standalone utility mode. |
| `--strict` | `false` | Fail on every non-passing test (mutually exclusive with `--baseline`, `--target`, and `--update-baseline`). |
| `--baseline=<path>` | *(none)* | Path to a `schema_version: 1` baseline JSON file. |
| `--target=<name>` | *(auto if 1)* | Target profile inside `--baseline`. |
| `--target-arch=<x64\|x86\|arm64>` | binary arch | Architecture of the filesystem implementation under test. |
| `--xpass-policy=<fail\|warn>` | `fail` | Exit `3` on unexpected pass (`fail`) or warn and exit `0` (`warn`). |
| `--json-out=<path>` | *(none)* | Write structured `schema_version: 1` JSON scorecard report. |
| `--update-baseline[=<path>]` | *(none)* | Update baseline for `--target` deterministically (rejected with `--gtest_filter`, `GTEST_FILTER`, `--gtest_repeat != 1`, `GTEST_REPEAT != 1`, or `GTEST_TOTAL_SHARDS`). |
| `--prune-passing` | `false` | With `--update-baseline`: also remove passing entries that could still apply to other hosts (default keeps unpredicated/multi-build passing entries, narrows multi-arch lists, and lists them under `"passed on this host"`). |
| `--default-reason=<text>` | `"TODO: triage failure"` | Default `reason` for newly added `--update-baseline` entries. |
| `--respect-fs-caps` | `false` | Opt-in skip for tests whose required FS capability flag is reported absent. |
| `--is-volume-root=<auto\|true\|false>` | `auto` | Override volume-root auto-detection. |
| `--temp-prefix=<str>` | `fst` | Prefix for isolated per-test subdirectories. |
| `--large-dir-count=<N>` | `600` | Entry count for the large-directory enumeration stress test (`>= 10`). |
| `--async-timeout-ms=<ms>` | `5000` | Timeout in milliseconds for async/oplock/change-notify operations. |
| `--allow-unknown-xfails` | `false` | Warn instead of exiting `2` when a baseline references an unknown test ID. |
| `--verbose` | `false` | Print full failure traces even for expected `XFAIL` tests. |
| `--help`, `-h` | *(none)* | Print CLI usage summary and exit codes, then exit `0`. |
| `--version` | *(none)* | Print version string (`dokan_fstest 1.0.0`) and exit `0`. |
| `--gtest_*` | passthrough | Forwarded directly to GoogleTest (`--gtest_filter`, `--gtest_shuffle`, `--gtest_repeat`, `--gtest_output`, `--gtest_list_tests`, etc.). |

<!-- markdownlint-enable MD013 -->

#### Utility Mode Mutual Exclusion & Environment Variables

- **Strict utility-mode mutual exclusion:** `--cleanup-dir=<path>` and
  `--validate-baseline=<path>` are standalone utility modes. Combining either
  flag with the other, with any test-execution flag (`--root`, `--strict`,
  `--baseline`, `--verbose`, etc.), or with any `--gtest_*` flag fails
  immediately with exit code `2`.
- **`FSTEST_ROOT`:** Fallback target path used when `--root=<path>` is omitted.
- **`GTEST_FILTER`, `GTEST_REPEAT`, & `GTEST_TOTAL_SHARDS`:** Evaluated after
  GoogleTest initialization. A non-default `GTEST_FILTER` (anything other than
  empty or `*`), a `GTEST_REPEAT` value other than `1`, or sharding via
  `GTEST_TOTAL_SHARDS` blocks `--update-baseline` with exit code `2`, matching
  the `--gtest_filter` / `--gtest_repeat` guards against partial-run baseline
  corruption. `--gtest_fail_fast` is also rejected when `--baseline` is active
  (exit `2`).
- **`GTEST_LIST_TESTS`:** Setting `GTEST_LIST_TESTS` to any non-empty value
  other than `"0"` (matching GoogleTest's `BoolFromGTestEnv`) lists all
  registered tests and exits `0` without requiring `--root`, matching
  `--gtest_list_tests`.

### Exit Codes

<!-- markdownlint-disable MD013 -->

| Code | Meaning |
| :---: | :--- |
| `0` | Pass — zero unexpected `FAIL`s and zero disallowed `XPASS`es (or `--xpass-policy=warn`). |
| `1` | Regression — one or more unexpected `FAIL`s, fatal setup/teardown failure (`fstest_fatal_setup=1` or `fstest_fatal_teardown=1`), SEH/C++ exception, or suite/program-level failure. Takes precedence over `3`. |
| `2` | Usage, configuration, manifest, or infrastructure error — invalid/duplicate flags, unsafe `--cleanup-dir` or `--cleanup-dir` deletion failure, lost mount, malformed/duplicate-key JSON, schema error, inheritance cycle, unknown test ID, `0` tests executed, or all-`SKIP` run. |
| `3` | Unexpected pass (`XPASS`) — a baselined test with `allow_xpass: false` passed under `--xpass-policy=fail`. |

<!-- markdownlint-enable MD013 -->

### JSON Scorecard Schema (`--json-out`, `schema_version: 1`)

Passing `--json-out=<path>` writes a deterministically ordered UTF-8 JSON
report (`schema_version: 1`):

```json
{
  "schema_version": 1,
  "mode": "baseline",
  "root_path": "M:\\",
  "baseline_path": "tests/fstest/baselines/dokan_mirror.json",
  "target": "mirror_drive",
  "target_arch": "x64",
  "xpass_policy": "fail",
  "exit_code": 0,
  "duration_ms": 14820,
  "os": {
    "major": 10,
    "minor": 0,
    "build": 26100,
    "ubr": 3476,
    "is_server": true,
    "host_arch": "x64",
    "process_arch": "x64",
    "is_elevated": true,
    "can_create_symlinks": true
  },
  "capabilities": {
    "filesystem_name": "NTFS",
    "max_component_length": 255,
    "fs_flags": 65470719,
    "bytes_per_sector": 512,
    "physical_bytes_per_sector": 4096,
    "bytes_per_cluster": 4096,
    "is_volume_root": true,
    "short_names_enabled": false,
    "respect_fs_capabilities": false
  },
  "summary": {
    "total": 326,
    "pass": 225,
    "xfail": 99,
    "crash_skipped": 2,
    "xpass": 0,
    "xpass_allowed": 0,
    "fail": 0,
    "skip": 2,
    "score_pct": 69.44
  },
  "domains": {
    "create_open": {
      "total": 35,
      "pass": 32,
      "xfail": 3,
      "crash_skipped": 0,
      "xpass": 0,
      "xpass_allowed": 0,
      "fail": 0,
      "skip": 0,
      "score_pct": 91.43
    }
  },
  "tests": [
    {
      "id": "CreateOpenTest.CreateNewSucceedsAndRejectsExisting",
      "domain": "create_open",
      "outcome": "PASS",
      "fatal_teardown": false,
      "fatal_setup": false,
      "exception": false,
      "synthetic": false,
      "skip_crash": false,
      "duration_ms": 4,
      "iterations": 1,
      "xfail_reason": "",
      "skip_reason": "",
      "failure_messages": []
    }
  ]
}
```

<!-- markdownlint-disable MD013 -->

| Field | Type | Description |
| :--- | :--- | :--- |
| `schema_version` | integer | Always `1`. |
| `mode` | string | `"strict"` or `"baseline"`. |
| `root_path` | string | Normalized Win32 target root path. |
| `baseline_path`, `target` | string | Baseline manifest path and resolved target name (empty strings in `"strict"` mode). |
| `target_arch` | string | `"x64"`, `"x86"`, or `"arm64"`. |
| `xpass_policy` | string | `"fail"` or `"warn"`. |
| `exit_code`, `duration_ms` | integer | Final process exit code (`0`–`3`) and total run duration in milliseconds. |
| `os` | object | Host Windows version (`major`, `minor`, `build`, `ubr`, `is_server`), `host_arch`, `process_arch`, `is_elevated`, and `can_create_symlinks`. |
| `capabilities` | object | Probed volume properties (`filesystem_name`, `max_component_length`, `fs_flags`, sector/cluster sizes, `is_volume_root`, `short_names_enabled`, `respect_fs_capabilities`). |
| `summary` | counts object | Aggregate counts: `total`, `pass`, `xfail`, `crash_skipped` (subset of `xfail`), `xpass`, `xpass_allowed`, `fail`, `skip`, and `score_pct` (`(pass + xpass + xpass_allowed) / (total - skip) * 100.0`, rounded to 2 decimals, or `null` when `total == skip`). |
| `domains` | object | Per-domain counts objects in canonical 12-domain order (same schema as `summary`). |
| `tests` | array | Per-test records sorted lexicographically by `id`: `id`, `domain`, `outcome` (`"PASS"`, `"XFAIL"`, `"XPASS"`, `"XPASS_ALLOWED"`, `"FAIL"`, or `"SKIP"`), `fatal_teardown`, `fatal_setup`, `exception`, `synthetic`, `skip_crash`, `duration_ms`, `iterations` (aggregated across `--gtest_repeat`), `xfail_reason`, `skip_reason`, and `failure_messages`. |

<!-- markdownlint-enable MD013 -->

---

## CI Pipeline & Local Dokan Sample Harnesses

### Running `samples/memfs_test.ps1` and `samples/mirror_test.ps1` Locally

Both PowerShell harnesses mount the Dokan sample binaries, run `fsx.exe`, run
`dokan_fstest.exe` with the matching baseline target and `--target-arch`, and
run `IFSTest` (for drive-letter/junction mounts). When `-FsTestExe` is omitted
locally, the harness automatically locates `dokan_fstest.exe` (or builds it via
`scripts\build_fstest.ps1`). Run from the `samples\` directory in an elevated
PowerShell session after installing the Dokan driver:

```powershell
Push-Location samples

# Run memfs (x64 only) and capture JSON scorecards:
.\memfs_test.ps1 `
  -Memfs @("..\x64\Release\memfs.exe") `
  -FsTestExe "..\tests\fstest\build\bin\Release\dokan_fstest.exe" `
  -BaselinesDir "..\tests\fstest\baselines" `
  -ResultsDir "..\fstest-results" `
  -XpassPolicy fail

# Run selected mirror mount modes (x64):
.\mirror_test.ps1 `
  -Mirrors @("..\x64\Release\mirror.exe") `
  -ConfigNames @("drive", "driveMntMgr", "driveRemovableMntMgr", "netUnc") `
  -FsTestExe "..\tests\fstest\build\bin\Release\dokan_fstest.exe" `
  -BaselinesDir "..\tests\fstest\baselines" `
  -ResultsDir "..\fstest-results" `
  -XpassPolicy fail

Pop-Location
```

Supported `-ConfigNames` and baseline targets:

<!-- markdownlint-disable MD013 -->

| Harness | Config Name | Mount Arguments | Baseline Target |
| :--- | :--- | :--- | :--- |
| `samples/memfs_test.ps1` | `drive` | `/l M` | `memfs_drive` |
| `samples/mirror_test.ps1` | `drive` | `/l M` | `mirror_drive` |
| `samples/mirror_test.ps1` | `dirJunction` | `/l C:\DokanMount` | `mirror_junction` |
| `samples/mirror_test.ps1` | `driveRemovable` | `/l M /m` | `mirror_removable` |
| `samples/mirror_test.ps1` | `driveMntMgr` | `/l M /o` | `mirror_mntmgr` |
| `samples/mirror_test.ps1` | `driveRemovableMntMgr` | `/l M /o /m` | `mirror_removable_mntmgr` |
| `samples/mirror_test.ps1` | `netUnc` | `/l M /n \myfs\dokan` | `mirror_unc` |

<!-- markdownlint-enable MD013 -->

### GitHub Actions: Build and Test (`.github/workflows/build-and-test.yml`)

- **`Build, sign and package` job (`windows-2025-vs2026`):** Builds,
  test-signs, and packages the Dokan driver, libraries, samples, and installer
  (`DokanSetup-<version>.exe`, `dokan-<version>.zip`, and `dokan-test-payload`)
  **without** building or running `dokan_fstest`.
- **`Build test tools` job (`windows-2025-vs2026`):** Runs in parallel with
  `Build, sign and package`. Builds `fsx.exe` and `dokan_fstest.exe` via
  `scripts\build_fstest.ps1` with `-Configuration Release`, a cached
  `-CacheDir`, and `-VerifyStaticCrt` (compiling `/MT`, running
  `fstest_self_test.exe`, and enforcing the Windows Server 2016 base-OS import
  gate, without `-WarningsAsErrors`). Stages `dokan-test-tools` for the VM
  jobs and publishes `dokan-fstest-<version>.zip` containing
  `dokan_fstest.exe`, `baselines/`, `LICENSE`, `LICENSE-googletest.txt`, and
  `LICENSE-nlohmann-json.txt` (prefixed `pr-<N>-` on pull requests).
- **`Test (<os>, <shard>)` jobs:** Boot Windows Server 2016, 2019, 2022, and
  2025 VMs (QEMU/KVM on Linux runners), install `DokanSetup.exe` with Driver
  Verifier enabled on `dokan3.sys`, and run the test matrix split across two
  shards per OS (see `scripts/ci/README.md`):
  1. Real NTFS `--strict` gate on `C:\fstest_ntfs` (`ntfs_strict.json`,
     skipping `REQUIRE_VOLUME_ROOT()` and legacy-OS/8.3-gated tests).
  2. `samples\memfs_test.ps1` against both x64 and x86 `memfs.exe`.
  3. `samples\mirror_test.ps1` against x64 `mirror.exe` (all 6 mount configs)
     and x86 `mirror.exe` (`-ConfigNames @("drive", "netUnc")`).
  4. Scorecards uploaded in the `vm-logs-<os>-<shard>` artifacts
     (`fstest-results/*.json`).

### GitHub Actions (`.github/workflows/fstest-ntfs.yml`)

Runs on path-filtered pushes to `master` and pull requests (`tests/fstest/**`,
`scripts/build_fstest.ps1`, `scripts/fstest_common.ps1`,
`.github/workflows/fstest-ntfs.yml`), on a weekly `schedule:`, and on manual
`workflow_dispatch`:

- **`ntfs-strict` matrix (`windows-2022` and `windows-2025-vs2026`):**
  1. Runs `python tests/fstest/scripts/check_conformance_docs.py` (verifies
     `// Spec:` citations, zero orphan baseline IDs, `"gap"` field validity,
     `CONFORMANCE_GAPS.md` per-target/modifier JSON parity, and MIT SPDX
     headers).
  2. Builds with `scripts\build_fstest.ps1 -VerifyStaticCrt -WarningsAsErrors`
     (including all `fstest_self_test.exe` self-tests).
  3. Runs `dokan_fstest.exe --validate-baseline` on both
     `tests/fstest/baselines/dokan_memfs.json` and
     `tests/fstest/baselines/dokan_mirror.json`.
  4. Executes the real NTFS `--strict` conformance suite on `C:\fstest_ntfs`
     (skipping `REQUIRE_VOLUME_ROOT()` and legacy-OS/8.3-gated tests) and
     uploads `fstest-ntfs-scorecard-<os>`.
- **`clang-cl-portability` (`windows-2022`):**
  1. Builds Layer 1 + Layer 2 (`ntfs_helpers`, `ntfs_suite`) with
     `-Compiler clang-cl -SkipCli -WarningsAsErrors` under Win8
     `_WIN32_WINNT=0x0602`, empty `NTDDI_VERSION`, `UNICODE`-only
     (`FSTEST_DEFINE_UNDERSCORE_UNICODE=OFF`), and
     `FSTEST_CORE_NO_EXCEPTIONS=ON`.
  2. Builds the full suite + CLI with `-Compiler clang-cl -WarningsAsErrors`
     and executes all `fstest_self_test.exe` self-tests.

---

## Baseline Maintenance Workflow

See [`baselines/schema.md`](baselines/schema.md) for the complete JSON schema,
inheritance (`abstract`, `extends`, `extends_file`, `remove_xfails`), OS build /
architecture predicates, governance rules, and `--update-baseline` workflow.

All currently tracked Dokan xfail entries across `memfs` and `mirror` (grouped
by root cause with Microsoft specification proofs and Dokan source pointers)
are cataloged in [`CONFORMANCE_GAPS.md`](CONFORMANCE_GAPS.md).

---

## Contributor Workflow: Fixing a Dokan Gap or Adding a Test

### Fixing a Tracked Dokan Conformance Gap

1. **Pick a cluster** from [`CONFORMANCE_GAPS.md`](CONFORMANCE_GAPS.md) and
   locate the affected `Suite.Test` IDs in
   [`baselines/dokan_memfs.json`](baselines/dokan_memfs.json) or
   [`baselines/dokan_mirror.json`](baselines/dokan_mirror.json).
2. **Reproduce the failure directly** against your mounted Dokan target in
   `--strict --verbose` mode (which runs even `skip_crash` tests and prints
   full assertion traces):

   ```powershell
   .\tests\fstest\build\bin\Release\dokan_fstest.exe `
     --root=M:\ `
     --strict `
     --verbose `
     --gtest_filter=RenameMoveTest.RenameSameNameNoOp
   ```

3. **Implement the fix** in `sys/`, `dokan/`, `samples/dokan_memfs/`, or
   `samples/dokan_mirror/` and rebuild Dokan.
4. **Confirm `XPASS` detection** by running with the existing baseline under
   the default `--xpass-policy=fail`. For normal baselined failures, the newly
   passing test causes `dokan_fstest.exe` to exit with code `3` (`XPASS`):

   ```powershell
   .\tests\fstest\build\bin\Release\dokan_fstest.exe `
     --root=M:\ `
     --baseline=tests\fstest\baselines\dokan_memfs.json `
     --target=memfs_drive `
     --xpass-policy=fail
   ```

   **Special note for `skip_crash` and `allow_xpass` entries:** CI suppresses
   exit `3` on entries marked `"skip_crash": true` (excluded from execution in
   `--baseline` mode) or `"allow_xpass": true` (passing classifies as
   `XPASS_ALLOWED`, exit `0`). When fixing one of those gaps, you **must**
   verify the fix explicitly with `--strict --gtest_filter=<Suite.Test>` and
   **manually remove** the entry from the baseline JSON file.
5. **Remove the stale xfail entry** from `dokan_memfs.json` /
   `dokan_mirror.json` (manually, or via `--update-baseline --prune-passing` on
   a full, unfiltered run for non-`skip_crash`/non-`allow_xpass` entries) and
   regenerate the **Covered Tests** blocks in
   [`CONFORMANCE_GAPS.md`](CONFORMANCE_GAPS.md) with `--sync`:

   ```powershell
   python tests/fstest/scripts/check_conformance_docs.py --sync
   ```

   *(If removing the last test in a cluster, also remove that cluster's
   `### GAP-XX:` section and Executive Summary row.)*
6. **Run pre-PR validation checks** and verify the full suite exits `0`:

   ```powershell
   .\tests\fstest\build\bin\Release\dokan_fstest.exe `
     --validate-baseline=tests\fstest\baselines\dokan_memfs.json
   .\tests\fstest\build\bin\Release\dokan_fstest.exe `
     --validate-baseline=tests\fstest\baselines\dokan_mirror.json
   python tests/fstest/scripts/check_conformance_docs.py
   ```

### Adding a New Conformance Test

1. Place the new `TEST_F(<Domain>Test, <DescriptiveName>)` inside the matching
   file in `tests/fstest/suite/src/domains/*_test.cpp`.
2. Add a concise `// Spec:` header comment immediately above the `TEST_F`
   citing the authoritative `[MS-FSA]` / `[MS-FSCC]` section or Win32/WDK/NT
   API being verified:

   ```cpp
   // Spec: [MS-FSA] 2.1.5.15.12 FileRenameInformation -- renaming an object
   // to its own existing path is a no-op and succeeds without ReplaceIfExists.
   TEST_F(RenameMoveTest, RenameSameNameNoOp) {
     ...
   }
   ```

3. Validate the new test on a real NTFS volume first using `--strict` so the
   expected behavior is empirically grounded against Windows NTFS.
4. Run against `memfs` and `mirror` mounts. If Dokan exhibits a real gap, add
   an exact `Suite.Test` entry (never wildcards) with a root-cause `reason` and
   `"gap": "GAP-XX"` to the appropriate baseline JSON file, then run
   `python tests/fstest/scripts/check_conformance_docs.py --sync` to update
   [`CONFORMANCE_GAPS.md`](CONFORMANCE_GAPS.md).
5. Before opening a PR, run `--validate-baseline` on modified manifests and
   `python tests/fstest/scripts/check_conformance_docs.py` to confirm spec
   citations, baseline integrity, `CONFORMANCE_GAPS.md` sync, and SPDX headers.

---

## Embedding `core` + `suite` as a C++20 Library

`core/` (`ntfs_helpers`) and `suite/` (`ntfs_suite` / `ntfs_suite_base`) have
**zero dependency** on `dokan.dll`, `nlohmann/json`, `gmock`, or CLI flags, and
compile cleanly under both MSVC and `clang-cl` (including `-fno-exceptions` /
`/EHs-c-`, `-fno-rtti` / `/GR-`, `_WIN32_WINNT=0x0602`, and `UNICODE` without
`_UNICODE`) with no CMake-generated headers. All public headers under
`core/include/` and `suite/include/` carry `///` Doxygen documentation comments
and `SPDX-License-Identifier: MIT` license headers.

### CMake `add_subdirectory` Integration

Set `FSTEST_BUILD_CLI=OFF` before `add_subdirectory(...)` to embed only Layer 1
and Layer 2. When `FSTEST_BUILD_CLI=OFF`:

- `nlohmann/json` is not fetched or required.
- Your parent project's `CMAKE_MSVC_RUNTIME_LIBRARY` (`/MD`, `/MDd`, `/MT`, or
  `/MTd`), `CMAKE_BUILD_TYPE`, and
  `CMAKE_{RUNTIME,ARCHIVE,LIBRARY}_OUTPUT_DIRECTORY` settings are preserved
  untouched (`CMakeLists.txt` applies standalone `/MT` and output-dir defaults
  only when `PROJECT_IS_TOP_LEVEL` is true).
- Pre-existing `GTest::gtest` (and `nlohmann_json::nlohmann_json` when
  `FSTEST_BUILD_CLI=ON`) targets defined by the parent project are reused
  directly without re-fetching GoogleTest or mutating parent cache entries.

```cmake
# Parent project CMakeLists.txt (example using dynamic CRT /MD and system GTest)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(FSTEST_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(FSTEST_USE_SYSTEM_GTEST ON CACHE BOOL "" FORCE)
add_subdirectory(external/dokany/tests/fstest fstest_build)

# Option A: Full-suite embedding (all 12 domains / 16 translation units)
add_executable(my_fs_conformance_tests my_runner_main.cpp)
target_link_libraries(my_fs_conformance_tests PRIVATE ntfs_suite)
```

### Full-Suite vs. Partial-Domain Embedding

- **Full-suite embedding:** Link `ntfs_suite` (which transitively links
  `ntfs_suite_base`, `ntfs_helpers`, `advapi32`, and `GTest::gtest`) and call
  `fstest::LinkAllSuites()` from `fstest/suite/register_suites.h` in `main()`.
  This guarantees linker retention of all 16 domain translation units without
  requiring `/WHOLEARCHIVE`.
- **Partial-domain embedding:** Link `ntfs_suite_base` (adding `suite/src` as a
  private include directory on your domain target so internal domain headers
  resolve cleanly without exporting `suite/src` transitively) and compile only
  the selected `suite/src/domains/*_test.cpp` files. Do **not** call
  `fstest::LinkAllSuites()` (which references all 16 anchors); if compiling the
  selected domain files into an intermediate static library, call only the
  matching `fstest::ForceLink_<Domain>()` anchor(s) declared in
  `fstest/suite/register_suites.h`.

```cmake
# Option B: Partial-domain embedding (for example, delete + streams only)
add_library(my_selected_domains STATIC
    external/dokany/tests/fstest/suite/src/domains/delete_test.cpp
    external/dokany/tests/fstest/suite/src/domains/streams_test.cpp
    external/dokany/tests/fstest/suite/src/domains/streams_ops_test.cpp)
target_include_directories(my_selected_domains PRIVATE
    external/dokany/tests/fstest/suite/src)
target_link_libraries(my_selected_domains PUBLIC ntfs_suite_base)

add_executable(my_partial_runner my_partial_main.cpp)
target_link_libraries(my_partial_runner PRIVATE my_selected_domains)
```

In `my_partial_main.cpp`, invoke only the corresponding anchors before
`::testing::InitGoogleTest(&argc, argv)`:

```cpp
fstest::ForceLink_Delete();
fstest::ForceLink_Streams();
fstest::ForceLink_StreamsOps();
```

#### Domain Source Files & Linker Anchors

Four domains span two balanced translation units; include both files (and both
anchors) to embed the complete domain:

<!-- markdownlint-disable MD013 -->

| Domain | Source File(s) under `suite/src/domains/` | Linker Anchor(s) (`fstest/suite/register_suites.h`) |
| :--- | :--- | :--- |
| `create_open` | `create_open_test.cpp`, `create_open_security_test.cpp` | `ForceLink_CreateOpen()`, `ForceLink_CreateOpenSecurity()` |
| `delete` | `delete_test.cpp` | `ForceLink_Delete()` |
| `dir_enum` | `dir_enum_test.cpp` | `ForceLink_DirEnum()` |
| `rename_move` | `rename_move_test.cpp` | `ForceLink_RenameMove()` |
| `data_io` | `data_io_test.cpp`, `data_io_sparse_mmap_test.cpp` | `ForceLink_DataIo()`, `ForceLink_DataIoSparseMmap()` |
| `locks_oplocks` | `locks_oplocks_test.cpp` | `ForceLink_LocksOplocks()` |
| `timestamps_attrs` | `timestamps_attrs_test.cpp` | `ForceLink_TimestampsAttrs()` |
| `streams` | `streams_test.cpp`, `streams_ops_test.cpp` | `ForceLink_Streams()`, `ForceLink_StreamsOps()` |
| `security_acl` | `security_acl_test.cpp` | `ForceLink_SecurityAcl()` |
| `links_reparse` | `links_reparse_test.cpp` | `ForceLink_LinksReparse()` |
| `naming_paths` | `naming_paths_test.cpp` | `ForceLink_NamingPaths()` |
| `info_queries` | `info_queries_test.cpp`, `info_queries_notify_ea_test.cpp` | `ForceLink_InfoQueries()`, `ForceLink_InfoQueriesNotifyEa()` |

<!-- markdownlint-enable MD013 -->

### CMake Portability Options & Non-CMake Builds

<!-- markdownlint-disable MD013 -->

| CMake Option | Default | Effect |
| :--- | :--- | :--- |
| `FSTEST_BUILD_CLI` | `ON` | Build `dokan_fstest.exe` and `fstest_self_test.exe` (`OFF` skips `nlohmann/json`). |
| `FSTEST_WERROR` | `OFF` | Treat warnings as errors on `fstest` targets (`/WX` or `-Werror`; stripped from in-tree GoogleTest). |
| `FSTEST_WIN32_WINNT` | `"0x0A00"` | Sets `_WIN32_WINNT` on `ntfs_helpers` and `ntfs_suite*` (tested down to `"0x0602"` / Windows 8). |
| `FSTEST_NTDDI_VERSION` | `"0x0A000002"` | Sets `NTDDI_VERSION` on `ntfs_helpers` and `ntfs_suite*` (empty string `""` omits the macro). |
| `FSTEST_DEFINE_UNDERSCORE_UNICODE` | `ON` | Defines `_UNICODE` alongside `UNICODE` (`OFF` verifies `UNICODE`-only builds). |
| `FSTEST_CORE_NO_EXCEPTIONS` | `OFF` | Compiles `ntfs_helpers`, `ntfs_suite_base`, `ntfs_suite`, and in-tree `gtest` without C++ exceptions or RTTI (`/EHs-c- /GR-` or `-fno-exceptions -fno-rtti`, plus `_HAS_EXCEPTIONS=0`, `GTEST_HAS_EXCEPTIONS=0`). Requires `-DFSTEST_BUILD_CLI=OFF`. |

<!-- markdownlint-enable MD013 -->

If integrating into a non-CMake build system (MSBuild, Meson, custom scripts):

- **Required compile definitions:** `UNICODE`, `WIN32_LEAN_AND_MEAN`,
  `NOMINMAX`, and `_WIN32_WINNT=0x0602` (or newer, such as `0x0A00`).
  `_UNICODE` and `NTDDI_VERSION` are optional.
- **Include directories:** `core/include` for `ntfs_helpers`; `suite/include`
  and `suite/src` for `ntfs_suite_base` and domain `*_test.cpp` files; plus
  GoogleTest's `include/`.
- **System link libraries:** `advapi32.lib` only (all `ntdll.dll` routines are
  resolved dynamically via `GetProcAddress`).

### Programmatic Configuration & Lifecycle API

Configure Layer 2 before `RUN_ALL_TESTS()` using `fstest/suite/test_config.h`
and `fstest/suite/test_fixture.h`:

- **`fstest::TestConfig`:**
  - `root_path` (`std::wstring`): target mount or directory path (fallback:
    `FSTEST_ROOT`).
  - `is_volume_root` (`std::optional<bool>`): override auto-detection
    (`std::nullopt` = auto).
  - `temp_prefix` (`std::wstring`, default `L"fst"`): prefix for isolated
    per-test subdirectories.
  - `large_dir_entry_count` (`uint32_t`, default `600`): entry count for the
    large-directory enumeration stress test.
  - `respect_fs_capabilities` (`bool`, default `false`): when `true`, skips
    `REQUIRE_FS_CAPABILITY` tests whose probed volume flag is absent.
  - `preprobed_capabilities` (`std::optional<core::VolumeCapabilities>`): supply
    a pre-probed `core::ProbeVolumeCapabilities(...)` snapshot to avoid
    duplicate volume probes.
  - `capability_overrides` (`fstest::CapabilityOverrides`): explicit overrides
    that take precedence over probed flags — setting an override to `false`
    skips matching tests even when `respect_fs_capabilities == false`; setting
    it to `true` forces execution even if probed `false`. Supports
    `named_streams`, `sparse_files`, `hard_links`, `reparse_points`,
    `persistent_acls`, `extended_attributes`, `case_preserved_names`,
    `case_sensitive_dir`, `file_compression`, `object_ids`, `open_by_file_id`,
    `short_names`, `can_create_symlinks`, `oplocks`, `change_notifications`,
    and `sector_size` (`std::optional<uint32_t>` for `FILE_FLAG_NO_BUFFERING`).
  - `timing` (`fstest::TimingConfig`): `cleanup_retries` (default `5`),
    `cleanup_initial_backoff_ms` (default `10`), and `async_timeout_ms`
    (default `5000`).
  - `hooks` (`fstest::LifecycleHooks`): optional suite-level and per-test
    lifecycle callbacks (detailed below).
- **Configuration helpers:**
  - `fstest::ConfigureSuite(TestConfig)` installs a new process-wide config.
    Volume capabilities and root reachability are probed fresh per suite in
    `NtfsTestBase::SetUpTestSuite()` (never cached across suites).
  - `fstest::GetSuiteConfig()` returns a const reference to the active config.
  - `fstest::ResetSuiteConfig()` restores default `TestConfig` values (useful
    in multi-mount test harnesses).

#### Lifecycle Hooks (`fstest::LifecycleHooks`)

`dokan_fstest.exe` itself installs `before_test` / `after_test` as a
mount-liveness guard that aborts with `"mount lost after <last test>"` if the
target mount disappears mid-run. `TestLifecycleInfo` exposes `suite_name`,
`test_name`, and `full_name` (`"<SuiteName>.<TestName>"`) to per-test hooks:

<!-- markdownlint-disable MD013 -->

| Hook | Signature | Lifecycle & Failure Semantics |
| :--- | :--- | :--- |
| `before_suite` | `bool(std::string_view suite_name, std::wstring *active_root_out, std::string *error_out)` | Runs in `NtfsTestBase::SetUpTestSuite()`. May mount an in-process filesystem and override `*active_root_out`. Returning `false` fails every test in the suite with `fstest_fatal_setup="1"` (never masked as `XFAIL`) without invoking `after_suite`, `before_test`, or `after_test`. |
| `after_suite` | `void(std::string_view suite_name)` | Runs in `NtfsTestBase::TearDownTestSuite()` whenever `before_suite` is unset or returned `true`. |
| `before_test` | `bool(const TestLifecycleInfo &info, std::string *error_out)` | Runs in `NtfsTestBase::SetUp()` before creating `ScopedTempDir`. Returning `false` fails the test immediately with `fstest_fatal_setup="1"` (never masked as `XFAIL`). |
| `after_test` | `bool(const TestLifecycleInfo &info, bool test_passed, std::string *error_out)` | Runs in `NtfsTestBase::TearDown()` after zero-debris verification whenever `before_test` is unset or returned `true`. Returning `false` records `fstest_fatal_teardown="1"` (never masked as `XFAIL`). |

<!-- markdownlint-enable MD013 -->

```cpp
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_config.h"
#include "fstest/suite/test_fixture.h"

#include <gtest/gtest.h>

int main(int argc, char **argv) {
  fstest::LinkAllSuites();
  ::testing::InitGoogleTest(&argc, argv);

  fstest::TestConfig config;
  config.root_path = L"M:\\";
  config.respect_fs_capabilities = true;
  config.capability_overrides.object_ids = false;
  config.capability_overrides.sector_size = 4096;
  config.timing.async_timeout_ms = 10000;
  config.timing.cleanup_retries = 8;

  config.hooks.before_suite = [](std::string_view suite_name,
                                 std::wstring *active_root_out,
                                 std::string *error_out) -> bool {
    // Optional: mount your in-process filesystem and override *active_root_out.
    return true;
  };
  config.hooks.after_suite = [](std::string_view suite_name) {
    // Optional: unmount your in-process filesystem.
  };
  config.hooks.before_test = [](const fstest::TestLifecycleInfo &info,
                                std::string *error_out) -> bool {
    // Optional: reset per-test filesystem telemetry counters.
    return true;
  };
  config.hooks.after_test = [](const fstest::TestLifecycleInfo &info,
                               bool test_passed,
                               std::string *error_out) -> bool {
    // Optional: assert zero unexpected internal errors during the test.
    // Returning false records fstest_fatal_teardown="1".
    return true;
  };

  fstest::ConfigureSuite(std::move(config));
  const int rc = RUN_ALL_TESTS();
  fstest::ResetSuiteConfig();
  return rc;
}
```
