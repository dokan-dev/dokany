<!-- markdownlint-disable MD043 -->

# NTFS Conformance Baseline Manifest Schema (`schema_version: 1`)

This document describes the JSON schema, validation rules, and maintenance
workflow for `dokan_fstest.exe` xfail baseline manifests.

## Purpose

A baseline manifest records **known, tracked NTFS conformance gaps** for a
specific filesystem implementation or mount configuration (for example,
`memfs_drive` or `mirror_unc`).

Key principles:

- **Baselined tests still execute on every run** unless explicitly marked
  `"skip_crash": true`. Normal failing tests execute and classify as `XFAIL`
  (exit `0`), so hangs or regressions in the remaining suite still surface.
  `"skip_crash": true` is reserved strictly for tracked target-daemon crash or
  deadlock bugs that kill the mount mid-run; it is ignored in `--strict` mode
  and still counts as `XFAIL` (never `PASS` or `SKIP`) in the conformance score.
- **Fixes surface immediately as `XPASS`.** When a baselined test begins
  passing, `dokan_fstest.exe` exits with code `3` (under the default
  `--xpass-policy=fail`) so stale entries are promptly removed.
- **Setup, teardown, crash, and suite failures are never masked.** Any leftover
  test directory debris, `after_test` failure (`fstest_fatal_teardown=1`),
  fatal setup or mount-loss failure (`fstest_fatal_setup=1`), SEH/C++
  exception, or suite/program-level failure is always classified as `FAIL`
  (exit `1`), even if the test appears in a baseline.
- **Precondition skips are never misclassified as `XPASS`.** If a baselined test
  calls `GTEST_SKIP()` (for example, because the host OS build is below the
  test's minimum requirement), its outcome is `SKIP`.
- **Exact test IDs only.** Wildcards (`*`, `?`), filter separators (`:`), and
  leading `-` characters are strictly rejected so newly added tests are never
  silently swallowed and generated `--gtest_filter` strings cannot be corrupted.

## Top-Level Schema

```json
{
  "schema_version": 1,
  "description": "Human-readable description of this baseline file",
  "targets": {
    "<target_name>": { ... }
  }
}
```

| Field | Type | Required | Description |
| :--- | :--- | :---: | :--- |
| `schema_version` | integer | Yes | Must be `1`. |
| `description` | string | No | Optional summary of the manifest. |
| `targets` | object | Yes | Map of `<target_name>` to target definitions. |

Strict JSON parsing is enforced: duplicate object keys at any nesting level,
comments, trailing commas, and unknown schema fields fail immediately with exit
code `2`.

## Target Object Schema

```json
{
  "abstract": false,
  "extends": "mirror_base",
  "extends_file": "dokan_mirror.json",
  "description": "Optional target description",
  "xfails": {
    "SuiteName.TestName": { ... }
  },
  "remove_xfails": [
    "SuiteName.InheritedTestPassingOnThisTarget"
  ]
}
```

<!-- markdownlint-disable MD013 -->

| Field | Type | Required | Description |
| :--- | :--- | :---: | :--- |
| `abstract` | boolean | No | Default `false`. Abstract targets serve as reusable bases and cannot be selected directly via `--target`. |
| `extends` | string | No | Name of a parent target to inherit `xfails` from. Multi-level inheritance is supported; cycles fail with exit `2`. |
| `extends_file` | string | No | Optional (currently unused in committed Dokan manifests; supported for external overlays). Relative (or absolute) path to another manifest file containing the parent named by `extends`. Requires `extends`. |
| `description` | string | No | Optional human-readable description of the mount/target mode. |
| `xfails` | object | No | Map of exact canonical GoogleTest IDs (`SuiteName.TestName`) to xfail entry objects. Child entries override parent entries with the same ID. |
| `remove_xfails` | array of strings | No | Exact test IDs inherited from the parent that pass on this child target. Requires `extends`, every ID must exist in the resolved parent, and an ID cannot appear in both `remove_xfails` and `xfails` of the same target. |

<!-- markdownlint-enable MD013 -->

If `--target=<name>` is omitted on the command line, `dokan_fstest.exe`
automatically selects the sole non-abstract target in the file (or exits `2` if
zero or multiple concrete targets exist).

## Xfail Entry Schema

```json
{
  "reason": "Root cause explanation or issue reference",
  "gap": "GAP-01",
  "os_min_build": 18362,
  "os_max_build": 26100,
  "arch": ["x64", "x86"],
  "allow_xpass": false,
  "skip_crash": false
}
```

<!-- markdownlint-disable MD013 -->

| Field | Type | Required | Description |
| :--- | :--- | :---: | :--- |
| `reason` | string | Yes | Non-empty explanation of why the test fails on this target. |
| `gap` | string | Conditional | `CONFORMANCE_GAPS.md` cluster ID matching `^GAP-[0-9]{2,}$` (ASCII digits, e.g. `"GAP-01"`). **Required** by `dokan_fstest.exe` whenever `skip_crash` or `allow_xpass` is `true`, and required on every entry in committed `baselines/dokan_*.json` manifests (enforced by `check_conformance_docs.py`). Optional on plain entries in external/third-party manifests; preserved and serialized right after `reason` by `--update-baseline`. |
| `os_min_build` | uint32 | No | Entry applies only when host Windows build (`RtlGetVersion`) `>= os_min_build`. Omit when the test itself already enforces the same `REQUIRE_MIN_OS_BUILD(...)` gate (skipped tests classify as `SKIP`, never `XPASS`). |
| `os_max_build` | uint32 | No | Entry applies only when host Windows build `<= os_max_build`. Must satisfy `os_min_build <= os_max_build` when both are set. |
| `arch` | array of strings | No | Optional (currently unused in committed Dokan manifests). Non-empty list of unique values in `{"x64", "x86", "arm64"}`. Entry applies only when `--target-arch` is in this list (omitted = matches all architectures). |
| `allow_xpass` | boolean | No | Default `false`. Escape hatch for known intermittent/racy target bugs: a pass classifies as `XPASS_ALLOWED` (exit `0`) rather than `XPASS` (exit `3`). Requires `"gap"` and cannot be combined with `skip_crash: true`. |
| `skip_crash` | boolean | No | Default `false`. Escape hatch for tracked target-daemon crash, deadlock, or unrecoverable handle-leak bugs that kill the mount or poison teardown mid-run. Honored only in `--baseline` mode (never `--strict`), excludes the test from execution, records it as `XFAIL` (keeping it in the conformance score denominator), and is preserved by `--update-baseline`. Requires `"gap"` and cannot be combined with `allow_xpass: true`. |

<!-- markdownlint-enable MD013 -->

## Validation Rules

Before executing tests in `--baseline` mode, `dokan_fstest.exe` resolves target
inheritance and validates **every effective xfail entry** in `all_entries`
(including entries whose `os_min_build`, `os_max_build`, or `arch` predicates
are inactive on the current host OS or `--target-arch`) against **all compiled
and registered tests** in the binary (ignoring `--gtest_filter`) via
`ValidateBaselineTestIds`. Unknown or renamed test IDs fail immediately with
exit code `2` unless `--allow-unknown-xfails` is passed (which downgrades
unknown IDs to warnings).

Standalone `--validate-baseline=<path>` validates a manifest offline without
requiring `--root` or executing any tests:

- Verifies strict JSON syntax, schema version, allowed keys, and entry
  constraints.
- Validates every `xfails` test ID declared on `abstract` targets directly
  against all compiled and registered tests.
- Resolves inheritance (`extends`, `extends_file`, `remove_xfails`, cycle
  detection) for every concrete target and validates all entries in
  `all_entries` (including inactive `os_min_build`, `os_max_build`, and
  `arch`-gated entries) with strict unknown-ID checking.
- Requires at least one non-abstract target in the manifest. Exits `0` when
  valid or `2` on any schema, inheritance, or unknown-ID error.

## Governance for `skip_crash` and `allow_xpass`

Because both escape hatches suppress normal CI `XPASS` (exit `3`) notifications,
strict governance applies to all Dokan baseline manifests:

- **Mandatory structured `"gap"` citation:** Every `"skip_crash": true` and
  `"allow_xpass": true` entry **must** set `"gap": "GAP-XX"` matching a cluster
  in [`../CONFORMANCE_GAPS.md`](../CONFORMANCE_GAPS.md) (enforced by both
  `dokan_fstest.exe --validate-baseline` and
  `python tests/fstest/scripts/check_conformance_docs.py`). In committed Dokan
  manifests (`baselines/dokan_*.json`), `check_conformance_docs.py` requires
  `"gap"` on every xfail entry and generates/checks each cluster's
  **Covered Tests** list in `CONFORMANCE_GAPS.md` (run with `--sync` to
  regenerate).
- **Explicit `--strict` verification and manual removal on fix:** In
  `--baseline` mode, `"skip_crash": true` excludes the test from execution and
  `"allow_xpass": true` classifies a passing test as `XPASS_ALLOWED` (exit `0`).
  Consequently, fixing the underlying defect will **not** trigger an automatic
  exit `3` (`XPASS`) failure in CI. Contributors fixing a `skip_crash` or
  `allow_xpass` gap **must** explicitly verify the fix with
  `--strict --gtest_filter=<Suite.Test>`, manually remove the entry from the
  baseline JSON file, regenerate
  [`../CONFORMANCE_GAPS.md`](../CONFORMANCE_GAPS.md) with
  `python tests/fstest/scripts/check_conformance_docs.py --sync`, and run
  `--validate-baseline`.

## Maintenance Guidelines

1. **Keep abstract bases clean.** Because `remove_xfails` is a flat list without
   per-OS or per-arch predicates, place only failures that apply uniformly
   across all child modes (or share identical OS/arch predicates across all
   child modes) inside an abstract base like `mirror_base`. Lift mode-divergent
   entries into individual child targets.
2. **Updating baselines deterministically:**

   ```powershell
   dokan_fstest.exe `
     --root=M:\ `
     --baseline=tests\fstest\baselines\dokan_mirror.json `
     --target=mirror_drive `
     --target-arch=x64 `
     --update-baseline `
     --default-reason="TODO: investigate Dokan mirror failure"
   ```

   `--update-baseline` guarantees:

   - Existing human-written `reason` strings, `"gap"` IDs, and metadata are
     preserved for unchanged failures.
   - Entries whose `os_min_build`/`os_max_build`/`arch` predicates did not match
     the current run are **never deleted** (updating on Windows Server 2025
     preserves Windows Server 2016 entries).
   - **Cross-host safety (`--prune-passing`):** By default, passing entries that
     could still fail on other hosts (unpredicated entries, multi-build ranges,
     or multi-arch lists) are **not** deleted outright: multi-arch lists are
     narrowed by dropping only the current `--target-arch`, and other passing
     entries are kept and reported under `"passed on this host"`. Pass
     `--prune-passing` alongside `--update-baseline` to explicitly remove them.
   - Entries for `SKIP`ped tests, `skip_crash: true`, and `allow_xpass: true`
     entries are preserved.
   - Parent targets (e.g., `mirror_base`) are never mutated when updating a
     child target: inherited active failures are omitted from the child's
     `xfails`, and parent entries that pass on the child are placed in
     `remove_xfails` (when pruned). Relative `extends_file` paths are rebased
     when writing `--update-baseline=<out_path>` into a different directory.
   - Output is serialized deterministically (2-space indent, fixed key order
     `reason`, `gap`, `os_min_build`, `os_max_build`, `arch`, `allow_xpass`,
     `skip_crash`, lexicographically sorted `xfails` and `remove_xfails`, LF
     line endings, trailing newline).
   - `--update-baseline` refuses to run alongside `--gtest_filter`, a
     non-default `GTEST_FILTER` environment variable, `--gtest_repeat != 1`,
     `GTEST_REPEAT != 1`, or `GTEST_TOTAL_SHARDS` (exit `2`) to prevent
     partial-run baseline corruption.
   - After editing Dokan baseline manifests, run
     `python tests/fstest/scripts/check_conformance_docs.py --sync` to update
     `CONFORMANCE_GAPS.md`, then verify with
     `dokan_fstest.exe --validate-baseline=<path>` and
     `python tests/fstest/scripts/check_conformance_docs.py`.
3. **External overlays:** Out-of-tree projects can write a standalone manifest
   (with no `extends`) or overlay an upstream manifest via `extends_file`:

   ```json
   {
     "schema_version": 1,
     "description": "Custom filesystem overlay",
     "targets": {
       "custom_drive": {
         "extends_file": "dokan_mirror.json",
         "extends": "mirror_base",
         "xfails": {
           "StreamsTest.FileAlternateDataStreamCreateReadWrite": {
             "reason": "Alternate data streams not implemented"
           }
         }
       }
     }
   }
   ```
