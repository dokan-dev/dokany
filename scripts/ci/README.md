# Continuous integration

Dokan's CI is the GitHub Actions workflow
[`.github/workflows/build-and-test.yml`](../../.github/workflows/build-and-test.yml) ("Build and Test").
It runs on pull requests (ignoring `README.md` and `documentations/**`), on pushes to `master`, on a 5-day `schedule:` keep-alive on `master`
(which restores cached VM images to prevent 7-day inactivity eviction without running tests), and on
demand (`workflow_dispatch`, with optional `os` and `crash_selftest` inputs). A companion workflow,
[`.github/workflows/cleanup-pr-caches.yml`](../../.github/workflows/cleanup-pr-caches.yml), cancels any in-flight `pull_request` runs and deletes
`refs/pull/<N>/merge` caches as soon as a pull request is closed or merged.

```text
            ┌───────────────────────────────────┐
            │ Validate CI scripts (Linux)       │  (unit tests for CI helpers)
            ├───────────────────────────────────┤
            │ Build, sign and package (Windows) │──► DokanSetup-<ver>.exe, dokan-<ver>.zip
            │                                   │──► dokan-test-payload ───────┐
 push / PR ─┼───────────────────────────────────┤                              │ (needs +
            │ Build test tools (Windows)        │──► dokan-fstest-<ver>.zip    │  download-artifact)
            │                                   │──► dokan-test-tools ─────────┤
            ├───────────────────────────────────┤                              ▼
            │ Prepare VM image (×4, cached)     │──────────────────────────► Test (<os>, shard1|shard2)
            └───────────────────────────────────┘                            (×8, Linux + QEMU/KVM)
                                                                                       │ crash or hang
                                                                                       ▼
                                                                               Find crash dumps
                                                                                       │ if any dump exists
                                                                                       ▼
                                                                        Analyze crash dumps (kd.exe)
```

## Jobs

| Job | Runner | What it does |
| :--- | :--- | :--- |
| `Validate CI scripts` | `ubuntu-latest` | [`test_ci_scripts.sh`](test_ci_scripts.sh): behavioral unit tests for `cleanup-pr-caches.yml` (`jq` run matcher), `scripts/run_ifstest.ps1` (`Find-IfsTestCreateProcFailure`), `scripts/ci/install_cygwin.ps1` (`Assert-CygwinGpgVerification`), and Driver Verifier checks in `vm/provisioning/verifier_helpers.ps1`. Skipped on `schedule` and `crash_selftest` runs. |
| `Build, sign and package` | `windows-2025-vs2026` | [`install_cygwin.ps1`](install_cygwin.ps1) + [`build_and_package.ps1`](build_and_package.ps1): builds Win32/x64/ARM64 and Cygwin + MSYS2 `dokan_fuse`, test-signs with `cert/DokanSign.pfx`, builds the WiX bundle. Publishes direct (`archive: false`) release artifacts `DokanSetup-<version>.exe` and `dokan-<version>.zip` (prefixed `pr-<N>-` on pull requests), plus `dokan-test-payload`. |
| `Build test tools` | `windows-2025-vs2026` | [`build_test_tools.ps1`](build_test_tools.ps1): builds `dokan_fstest.exe` (static CRT, Server 2016 import gate, self-tests) and `fsx.exe`. Publishes single-zipped (`archive: false`) `dokan-fstest-<version>.zip` (prefixed `pr-<N>-` on pull requests) and `dokan-test-tools`. Runs in parallel with the Dokan build; skipped on `schedule` and `crash_selftest` runs. |
| `Prepare VM image (<os>)` | `ubuntu-latest` | Computes the per-OS cache key via [`vm/image_cache_key.sh`](vm/image_cache_key.sh). On a cache miss, [`vm/bake_image.sh`](vm/bake_image.sh) installs Windows Server Core from Microsoft's evaluation ISO, provisions it (see below), and saves it to the Actions cache. On the 5-day `schedule:` run on `master`, restores each cached image to refresh its last-accessed time. |
| `Test (<os>, <shard>)` | `ubuntu-latest` | Depends on `Build, sign and package`, `Build test tools`, and `Prepare VM image`. Restores the cached image, downloads `dokan-test-payload` and `dokan-test-tools` via `actions/download-artifact`, and runs [`vm/run_vm_tests.sh`](vm/run_vm_tests.sh) (QEMU/KVM) and [`vm/run_guest_tests.ps1`](vm/run_guest_tests.ps1) inside the guest. |
| `Find crash dumps` / `Analyze crash dumps` | `ubuntu-latest` / `windows-2022` | When any `crash-dump-*-a<attempt>` artifact exists for the current run attempt, [`analyze_crash_dumps.ps1`](analyze_crash_dumps.ps1) runs `kd.exe` with `dokan3.sys`/`dokan3.pdb`, uploads the full reports in the `crash-analysis` artifact, and writes a summary to the job summary. |
| `Crash-capture self-test (server2022)` | `ubuntu-latest` | Manual only (`workflow_dispatch` `crash_selftest` input: `bugcheck` or `hang`). Depends only on `Prepare VM image (server2022)` and `Build, sign and package` (skips `Build test tools`), and crashes or hangs a Server 2022 VM with Dokan loaded to verify the dump and `kd.exe` pipeline end to end. |

`Test (<os>, <shard>)` declares `needs: [vm-image, build, build-test-tools]` and downloads
`dokan-test-payload` and `dokan-test-tools` directly via `actions/download-artifact` (no API polling),
while its `if:` expression still runs healthy OSes if a single OS image bake fails. Release artifacts
(`DokanSetup-<version>.exe`, `dokan-<version>.zip`, `dokan-fstest-<version>.zip`) are uploaded with
`archive: false` (no double-zipping) and prefixed with `pr-<N>-` on pull requests. Installer versions
use the formula `<BaseVersion>.<BuildVersion + 1000 + run number>` ([`get_version.ps1`](get_version.ps1)
with `BUILD_NUMBER_OFFSET=1000` and `BuildVersion=1000` in [`dokan_wix/version.xml`](../../dokan_wix/version.xml),
producing `3.0.0.2001+`), continuing after the last AppVeyor build (`3.0.0.1947`) while respecting
future `BuildVersion` hotfix bumps.

## Test coverage

Windows Server Core **2016 (14393)**, **2019 (17763)**, **2022 (20348)**, and
**2025 (26100)**, x64, with Driver Verifier (`/standard` plus `0x02000000` Code
integrity / HVCI checks for `dokan3.sys`) active from the first load of the
driver. Each OS runs two shards in parallel; together they match the full
former AppVeyor FsTest coverage:

| Shard | Tests |
| :--- | :--- |
| `shard1` | Real NTFS `dokan_fstest --strict` gate; `samples/memfs_test.ps1` (x64, x86); `samples/mirror_test.ps1` x64 (`drive`, `dirJunction`, `driveRemovable`, `netUnc`) |
| `shard2` | `samples/mirror_test.ps1` x64 (`driveMntMgr`, `driveRemovableMntMgr`), x86 (`drive`, `netUnc`) |

Each mount runs `fsx`, `dokan_fstest` against the `tests/fstest/baselines` manifests, and HLK IFSTest,
using the same `samples/*.ps1` harnesses as local runs. Because Win32-OpenSSH executes in non-interactive
Session 0 (where IFSTest multi-process security tests using `CreateProcessWithLogonW` fail with
`C0000095`), [`vm/run_guest_tests.ps1`](vm/run_guest_tests.ps1) trampolines into interactive Session 1
(`AutoAdminLogon` / `WinSta0\Default`) via a one-shot Scheduled Task and streams output back over SSH;
[`scripts/run_ifstest.ps1`](../run_ifstest.ps1) explicitly fails the run (in both CI and local runs) if
`SetOwnerSecurityTest` or `DirectoryTraverseTest` reports `IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE`.
Scorecards (`fstest-results/*.json`), install logs, `guest_tests.log`, the serial console log,
(on boot/SSH stalls, even if a retry recovers) `no-ssh-screen.ppm` and `boot-stall-diag.txt`
(host kernel/QEMU/`kvm_amd` parameters plus two QEMU monitor snapshots 5s apart; [`vm/common.sh`](vm/common.sh) also omits `hv_time` from `QEMU_CPU` to avoid an intermittent nested-AMD-KVM early-boot `AP #2 TPR=0xf0` deadlock), and
(on test failures) `guest_diagnostics.txt` (recent `System`/`Application`/`TaskScheduler`/`WMI-Activity`
events, live test processes, and trampoline state) are uploaded as `vm-logs-<os>-<shard>`.

## Crashes and hangs

- **BugCheck:** the guest writes a kernel memory dump and reboots; the host copies `C:\Windows\MEMORY.DMP`.
- **Hang:** if a shard exceeds its timeout, or SSH stops answering while the VM still runs, the host
  injects an NMI through the QEMU monitor. The image sets `CrashControl\NMICrashDump=1`, so Windows
  bugchecks with `0x80` and the hang becomes a regular dump.

Dumps are uploaded as `crash-dump-<os>-<shard>-a<run_attempt>` and analyzed by `kd.exe` with
`.reload /f dokan3.sys`, `!analyze -v`, `kP 30`, `dv /t /i /V`, `lmvm dokan3`, `!running -it`, `!locks`,
`!stacks 2 dokan`, and `!verifier 0x1`. The full raw output is uploaded in the `crash-analysis` artifact,
and key bugcheck/stack/lock/Dokan/Verifier sections are posted to the job summary.

**Known intermittent (Server 2025 `shard1` PatchGuard):** on Server 2025, `shard1` has occasionally hit
PatchGuard (`CRITICAL_STRUCTURE_CORRUPTION`, `0x109`, type `0x115`) under nested KVM during IFSTest. The
dumps show all Dokan threads idle and no lock contention, and it has not been reproduced outside nested
KVM. Rather than using a blanket `continue-on-error`, `build-and-test.yml` passes
`ALLOW_PATCHGUARD_0X109=true` to `run_vm_tests.sh` for `server2025` `shard1` only: if System Event 1001
matches both `0x109` and `0x115`, `run_vm_tests.sh` retries once on a fresh overlay, and tolerates it with
a warning only if `0x109`/`0x115` reproduces on the retry — while still uploading `MEMORY.DMP.7z` for
`kd.exe` analysis (`analyze_crash_dumps.ps1` posts the excerpt to the job summary and fails if
`MODULE_NAME`/`IMAGE_NAME` directly names `dokan3`, though asynchronous PatchGuard `0x109` usually blames
`nt`/`ntkrnlmp.exe` regardless, so the real protection is the other 7 matrix legs failing hard on `0x109`
plus reviewing the posted dump summary). Every other test failure, hang, or crash on Server 2025 `shard1`
fails hard.

## VM images

`bake_image.sh` reads per-OS settings (`ISO_URL`, `IMAGE_INDEX`) from
`scripts/ci/vm/os/<os>.env`, runs an unattended install
([`vm/provisioning/autounattend.xml`](vm/provisioning/autounattend.xml)), and
invokes
[`vm/provisioning/provision_guest.ps1`](vm/provisioning/provision_guest.ps1)
inside the guest: test signing on, `cert/DokanCA.cer` trusted, Driver Verifier
armed for `dokan3.sys` (`/standard` plus `0x02000000` Code integrity / HVCI
checks, preserving OS-specific extended flags via `VerifyDriverLevel` and
`VerifierSettingState`), kernel memory dumps and NMI dumps enabled, console
`AutoAdminLogon` for interactive Session 1, Windows Update and Defender
real-time scanning off, Win32-OpenSSH (key-only for `Administrator`) with a
per-image key, the `dokan_ifstest` user with password expiration disabled (`net
accounts /maxpwage:unlimited` so cached images outlive Windows' 42-day password
limit), and HLK IFSTest
([`scripts/install_wlk_ifstest.ps1`](../install_wlk_ifstest.ps1)). Provisioning
cleans the component store with DISM, disables hibernation, clears the pagefile
at bake shutdown, and runs `Optimize-Volume -DriveLetter C -ReTrim` (with QEMU
`discard=unmap` and `rotation_rate=1`) before `qemu-img convert` compacts the
disk; `actions/cache` compresses the result (~2.6–3.9 GiB per OS).

[`vm/image_cache_key.sh`](vm/image_cache_key.sh) computes the deterministic per-OS cache key shared by
`vm-image`, `vm-test`, and `crash-selftest`. It hashes `scripts/ci/vm/os/<os>.env`,
`scripts/ci/vm/bake_image.sh`, `scripts/ci/vm/common.sh`, `scripts/ci/vm/provisioning/**`,
`scripts/install_wlk_ifstest.ps1`, and `cert/DokanCA.cer`. Updating one `<os>.env` only rebakes that OS;
changing shared provisioning files rebakes all four images in parallel (~10–20 minutes).

**Evaluation ISO trade-offs and fork cache quota:** Windows Update is disabled during baking so images
stay fast and reproducible, meaning VMs run unpatched evaluation builds (note also that Windows Server
2016 reaches end of extended support in January 2027). All four cached images together exceed GitHub's
default 10 GB repository cache limit on personal forks; forks can test one OS at a time via the
`workflow_dispatch` `os` input (`server2016`, `server2019`, `server2022`, or `server2025`).

### Adding a Windows version

1. Add `scripts/ci/vm/os/<os>.env` with the Microsoft evaluation `ISO_URL` and Server Core WIM
   `IMAGE_INDEX`.
2. Check [`scripts/install_wlk_ifstest.ps1`](../install_wlk_ifstest.ps1) and add an OS-matched HLK kit
   entry (MSIs, CABs, and SHA-256 hashes) if the new Windows build requires a newer HLK.
3. Add `<os>` to the default `fromJSON(...)` OS lists in `jobs.vm-image.strategy.matrix.os` and
   `jobs.vm-test.strategy.matrix.os`, and to `on.workflow_dispatch.inputs.os.options` in
   [`.github/workflows/build-and-test.yml`](../../.github/workflows/build-and-test.yml).
4. Review [`tests/fstest/baselines/`](../../tests/fstest/baselines/) for OS-build predicates
   (`os_min_build` / `os_max_build`) and confirm repository Actions cache quota can accommodate another
   ~2.6–3.9 GiB image.

### Changing or rebalancing shards

1. Update [`vm/run_guest_tests.ps1`](vm/run_guest_tests.ps1): update `[ValidateSet(...)]`, the real-NTFS
   `--strict` gate condition (`if ($Shard -in @('all', 'shard1'))`), and the `switch ($Shard)` block
   (keeping `all` equal to the union of all shards).
2. Update [`.github/workflows/build-and-test.yml`](../../.github/workflows/build-and-test.yml): update
   `jobs.vm-test.strategy.matrix.shard`, and keep the `ALLOW_PATCHGUARD_0X109` env expression in
   `vm-test`'s `Run tests in VM` step (plus the matching `crash-dump-server2025-shard1` check in
   [`analyze_crash_dumps.ps1`](analyze_crash_dumps.ps1)) aligned with whichever shard runs the Server
   2025 IFSTest workload.

## Security model for pull requests

- **Runner trust boundary:** pull request code (build scripts, solutions, harnesses, and VM runner
  scripts) executes **directly on ephemeral GitHub-hosted Windows and Linux runners** — not only inside
  the guest VMs. Protection for the repository relies on GitHub's `pull_request` trigger isolation: zero
  secrets, top-level `permissions: {}` with minimal per-job read-only `GITHUB_TOKEN` (`contents: read` on
  build/test/analysis jobs; `actions: read` only on `find-crash-dumps`), and `persist-credentials: false`
  on every checkout. Test signing uses only the public test certificate already checked into `cert/`.
  **Never run this workflow on self-hosted runners and never add secrets or production signing keys to
  it.**
- **Guest isolation:** QEMU/KVM copy-on-write overlays (discarded after each shard) and `restrict=on`
  user-mode networking (no outbound access; only SSH forwarded to `127.0.0.1`) isolate kernel-mode driver
  installation, test-signing, Driver Verifier, and BugChecks/NMIs from the runner OS and keep test runs
  offline. During image baking, `bake_image.sh` enables outbound NAT so the guest can download Microsoft
  HLK packages.
- **Download integrity and cache isolation:** `setup-x86_64.exe` is GPG-verified by [`install_cygwin.ps1`](install_cygwin.ps1) against Cygwin's pinned
  key (`56405CF6FCC81574682A5D561A698DE9E2E56300`, fetched fresh each run with cached outage fallback and `GOODSIG` + `VALIDSIG` enforcement); Win32-OpenSSH (`bake_image.sh`), HLK IFSTest MSIs/CABs
  (`scripts/install_wlk_ifstest.ps1`), and `dokan_fstest` dependencies
  ([`tests/fstest/deps.json`](../../tests/fstest/deps.json)) are SHA-256 verified. Build caches (WDK
  NuGet, Cygwin, `fstest-deps`) are read-only on pull requests (`actions/cache/save` is skipped), and any
  VM-image caches created by a pull request are scoped to `refs/pull/<N>/merge` (so they cannot replace
  `master` images) and cleaned up (along with any in-flight `pull_request` runs) when the PR closes or merges by
  [`.github/workflows/cleanup-pr-caches.yml`](../../.github/workflows/cleanup-pr-caches.yml).

## Running locally

Any x86-64 Linux machine with `/dev/kvm` works (install the packages listed in
[`.github/actions/setup-qemu-kvm/action.yml`](../../.github/actions/setup-qemu-kvm/action.yml)). Do **not**
copy the `MODE="0666"` udev rule from that action onto a multi-user local Linux machine; instead add your
user to the `kvm` group (`sudo usermod -aG kvm "$USER"` and log back in).

```sh
scripts/ci/vm/bake_image.sh server2022 ~/dokan-vm/server2022
# Payload: the dokan-test-payload and dokan-test-tools artifacts of a CI run, unzipped into one folder.
scripts/ci/vm/run_vm_tests.sh ~/dokan-vm/server2022 ~/dokan-payload ~/dokan-results-1 shard1
# Override SSH_PORT (default 2222) when running multiple shards concurrently on the same host:
SSH_PORT=2223 scripts/ci/vm/run_vm_tests.sh ~/dokan-vm/server2022 ~/dokan-payload ~/dokan-results-2 shard2
```
