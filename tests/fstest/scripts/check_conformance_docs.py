#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dokan Project Contributors
"""Validate tests/fstest citations, baselines, CONFORMANCE_GAPS.md, and SPDX."""

# Verifies four invariants across tests/fstest/:
#   1. Every TEST_F in suite/src/domains/*_test.cpp is preceded by a `// Spec:`
#      comment citing [MS-FSA], [MS-FSCC], or an explicit Win32/WDK/NT API name.
#   2. Every baselines/dokan_*.json manifest is strict JSON (no duplicate keys),
#      resolves inheritance cleanly for every concrete target, has zero orphan
#      test IDs, and assigns a valid `"gap": "GAP-XX"` (`^GAP-[0-9]{2,}$`) to
#      every xfail entry (including mandatory `"gap"` on `skip_crash` and
#      `allow_xpass` entries).
#   3. CONFORMANCE_GAPS.md Executive Summary rows match all `### GAP-XX:`
#      sections 1-to-1, every JSON `"gap"` references an existing section, and
#      every section's `**Covered Tests:**` list matches the deterministic
#      output generated from baselines/dokan_*.json (regenerable via `--sync`).
#   4. Every `.h` and `.cpp` source file under tests/fstest/ begins with
#      `// SPDX-License-Identifier: MIT`.

from __future__ import annotations

import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import re
import sys
import textwrap

SPDX_FIRST_LINE = "// SPDX-License-Identifier: MIT"

TEST_F_RE = re.compile(
    r"^\s*TEST_F\s*\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*\)",
    re.MULTILINE,
)

GAP_ID_RE = re.compile(r"^GAP-[0-9]{2,}$")
ALLOWED_ARCHS = frozenset({"x64", "x86", "arm64"})
ALLOWED_ENTRY_KEYS = frozenset({
    "reason",
    "gap",
    "os_min_build",
    "os_max_build",
    "arch",
    "allow_xpass",
    "skip_crash",
})
ALLOWED_TARGET_KEYS = frozenset({
    "abstract",
    "description",
    "extends",
    "extends_file",
    "xfails",
    "remove_xfails",
})
UINT32_MAX = (1 << 32) - 1
MAX_LINE_LEN = 80

# Matches formal protocol/API citations inside `// Spec:` blocks so prose-only
# comments without an authoritative spec or API reference are caught.
SPEC_CITATION_RE = re.compile(
    r"\[MS-(?:FSA|FSCC)\]"
    r"|\b(?:Win32|WDK|NT)\s+[A-Z][A-Za-z0-9_]+"
    r"|\b(?:Nt|Zw|Rtl|FsRtl|Io|Mm)[A-Z][A-Za-z0-9_]+"
    r"|\bFSCTL_[A-Z0-9_]+\b"
    r"|\bFile[A-Z][A-Za-z0-9_]*Information(?:Ex)?\b"
    r"|\bFileFs[A-Z][A-Za-z0-9_]*Information\b"
    r"|\b(?:CreateFile(?:2|[AW])?|CreateDirectory(?:Ex)?[AW]?|"
    r"DeleteFile[AW]?|RemoveDirectory[AW]?|MoveFile(?:Ex|WithProgress)?[AW]?|"
    r"ReplaceFile[AW]?|CopyFile(?:Ex|2)?[AW]?|ReadFile(?:Ex)?|WriteFile(?:Ex)?|"
    r"LockFile(?:Ex)?|UnlockFile(?:Ex)?|FlushFileBuffers|FlushViewOfFile|"
    r"MapViewOfFile(?:Ex)?|UnmapViewOfFile|CreateFileMapping[AW]?|"
    r"FindFirstFile(?:Ex)?[AW]?|FindNextFile[AW]?|FindFirstStreamW|"
    r"FindFirstFileNameW|GetFileAttributes(?:Ex)?[AW]?|SetFileAttributes[AW]?|"
    r"GetFileInformationByHandle(?:Ex)?|SetFileInformationByHandle|"
    r"GetFinalPathNameByHandle[AW]?|GetVolumeInformation(?:ByHandle)?[AW]?|"
    r"GetVolumePathName[AW]?|GetDiskFreeSpace(?:Ex)?[AW]?|SetEndOfFile|"
    r"SetFilePointer(?:Ex)?|SetFileTime|GetFileTime|GetFileSecurity[AW]?|"
    r"SetFileSecurity[AW]?|ReadDirectoryChanges(?:Ex)?W|DeviceIoControl|"
    r"DuplicateHandle|ReOpenFile|OpenFileById|CreateHardLink[AW]?|"
    r"CreateSymbolicLink[AW]?|BackupRead|BackupWrite|AccessCheck|"
    r"GetNamedSecurityInfo[AW]?|SetNamedSecurityInfo[AW]?|"
    r"GetShortPathName[AW]?|GetLongPathName[AW]?|GetFullPathName[AW]?|"
    r"SetFileValidData|SetFileShortName[AW]?|CancelIo(?:Ex)?)\b"
)

SKIP_DIR_NAMES = frozenset(
    {"ci-bin", "third_party", "_deps", "out", ".git", "__pycache__"}
)

# Splits off the trailing markdown reference-link block at EOF (including an
# optional preceding markdownlint MD013 comment).
REF_FOOTER_RE = re.compile(
    r"(?m)(?:\n<!-- markdownlint-disable MD013 -->\n+)?"
    r"\n(?=\[[a-z0-9-]+\]:\s+http)"
)

Modifiers = tuple[bool, bool, int | None, int | None, tuple[str, ...]]


def _should_skip_dir(name: str) -> bool:
  return name in SKIP_DIR_NAMES or name == "build" or name.startswith("build-")


def _line_number_at(text: str, offset: int) -> int:
  return text.count("\n", 0, offset) + 1


def _is_uint32(val: object) -> bool:
  return not isinstance(val, bool) and isinstance(val, int) and (
      0 <= val <= UINT32_MAX
  )


def _is_valid_test_id(test_id: str) -> bool:
  if not test_id or test_id.startswith("-"):
    return False
  if any(ord(ch) <= 0x20 or ord(ch) == 0x7F or ch in "*?:" for ch in test_id):
    return False
  return "." in test_id


def _reject_duplicate_keys(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
  obj: dict[str, object] = {}
  for k, v in pairs:
    if k in obj:
      raise ValueError(f"Duplicate JSON key '{k}'")
    obj[k] = v
  return obj


def _extract_modifiers(entry: dict[str, object]) -> Modifiers:
  arch_raw = entry.get("arch")
  arch = tuple(sorted(arch_raw)) if isinstance(arch_raw, list) else ()
  return (
      bool(entry.get("skip_crash", False)),
      bool(entry.get("allow_xpass", False)),
      entry.get("os_min_build"),  # type: ignore[return-value]
      entry.get("os_max_build"),  # type: ignore[return-value]
      arch,
  )


def _extract_preceding_comment_block(
    lines: list[str], line_no: int
) -> list[str]:
  block: list[str] = []
  idx = line_no - 2
  while idx >= 0 and lines[idx].strip().startswith("//"):
    block.append(lines[idx].strip())
    idx -= 1
  block.reverse()
  return block


def _check_single_domain_file(
    path: Path, rel: str, test_ids: set[str]
) -> list[str]:
  errors: list[str] = []
  text = path.read_text(encoding="utf-8")
  lines = text.splitlines()
  for match in TEST_F_RE.finditer(text):
    test_id = f"{match.group(1)}.{match.group(2)}"
    line_no = _line_number_at(text, match.start())
    if test_id in test_ids:
      errors.append(f"{rel}:{line_no}: Duplicate TEST_F ID '{test_id}'")
    else:
      test_ids.add(test_id)

    block = _extract_preceding_comment_block(lines, line_no)
    spec_start = next(
        (i for i, ln in enumerate(block) if ln.startswith("// Spec:")),
        None,
    )
    if spec_start is None:
      errors.append(
          f"{rel}:{line_no}: {test_id} is missing a preceding '// Spec:' "
          "comment"
      )
      continue

    spec_text = " ".join(ln[2:].strip() for ln in block[spec_start:])
    if not SPEC_CITATION_RE.search(spec_text):
      errors.append(
          f"{rel}:{line_no}: {test_id} '// Spec:' comment lacks a "
          "[MS-FSA]/[MS-FSCC] or Win32/WDK/NT API citation: "
          f"{block[spec_start]}"
      )
  return errors


def check_domain_tests_and_specs(
    fstest_dir: Path,
) -> tuple[set[str], list[str]]:
  """Collect all TEST_F IDs and verify preceding // Spec: citations."""
  test_ids: set[str] = set()
  domains_dir = fstest_dir / "suite" / "src" / "domains"
  if not domains_dir.is_dir():
    return test_ids, [f"Domains directory not found: {domains_dir}"]

  domain_files = sorted(domains_dir.glob("*_test.cpp"))
  if not domain_files:
    return test_ids, [f"No *_test.cpp files found under {domains_dir}"]

  errors: list[str] = []
  for path in domain_files:
    rel = path.relative_to(fstest_dir).as_posix()
    errors.extend(_check_single_domain_file(path, rel, test_ids))
  return test_ids, errors


def _load_manifest_cache(manifest_path: Path, cache: dict[Path, dict]) -> dict:
  key = manifest_path.resolve()
  if key not in cache:
    cache[key] = json.loads(
        key.read_text(encoding="utf-8"),
        object_pairs_hook=_reject_duplicate_keys,
    )
  return cache[key]


def _resolve_target_effective_xfails(
    manifest_path: Path,
    target_name: str,
    cache: dict[Path, dict],
    stack: list[tuple[Path, str]],
) -> dict[str, dict]:
  key = (manifest_path.resolve(), target_name)
  if key in stack:
    raise ValueError(f"Inheritance cycle involving '{target_name}'")
  data = _load_manifest_cache(manifest_path, cache)
  targets = data.get("targets", {})
  if target_name not in targets:
    raise ValueError(
        f"Target '{target_name}' not found in {manifest_path.name}"
    )

  tdef = targets[target_name]
  effective: dict[str, dict] = {}
  extends = tdef.get("extends")
  if extends:
    extends_file = tdef.get("extends_file")
    parent_path = (
        (manifest_path.parent / extends_file) if extends_file else manifest_path
    )
    effective = _resolve_target_effective_xfails(
        parent_path, extends, cache, stack + [key]
    )

  for rem_id in tdef.get("remove_xfails", []):
    if rem_id not in effective:
      raise ValueError(
          f"remove_xfails ID '{rem_id}' is not present in parent '{extends}'"
      )
    effective.pop(rem_id)
  effective.update(tdef.get("xfails", {}))
  return effective


def _validate_entry_flags(ctx: str, entry: dict[str, object]) -> list[str]:
  errors: list[str] = []
  skip_crash = entry.get("skip_crash", False)
  allow_xpass = entry.get("allow_xpass", False)
  if not isinstance(skip_crash, bool):
    errors.append(f"{ctx}: 'skip_crash' must be a boolean")
  if not isinstance(allow_xpass, bool):
    errors.append(f"{ctx}: 'allow_xpass' must be a boolean")
  if skip_crash is True and allow_xpass is True:
    errors.append(f"{ctx}: cannot set both 'skip_crash' and 'allow_xpass'")
  return errors


def _validate_entry_os_bounds(ctx: str, entry: dict[str, object]) -> list[str]:
  errors: list[str] = []
  os_min = entry.get("os_min_build")
  os_max = entry.get("os_max_build")
  if os_min is not None and not _is_uint32(os_min):
    errors.append(f"{ctx}: 'os_min_build' must be a uint32 (got {os_min!r})")
  if os_max is not None and not _is_uint32(os_max):
    errors.append(f"{ctx}: 'os_max_build' must be a uint32 (got {os_max!r})")
  if (
      isinstance(os_min, int)
      and not isinstance(os_min, bool)
      and isinstance(os_max, int)
      and not isinstance(os_max, bool)
      and os_min > os_max
  ):
    errors.append(f"{ctx}: os_min_build ({os_min}) > os_max_build ({os_max})")
  return errors


def _validate_entry_arch(ctx: str, entry: dict[str, object]) -> list[str]:
  if "arch" not in entry:
    return []
  arch = entry["arch"]
  if not isinstance(arch, list) or not arch:
    return [f"{ctx}: 'arch' must be a non-empty list"]
  errors: list[str] = []
  seen: set[str] = set()
  for item in arch:
    if not isinstance(item, str) or item not in ALLOWED_ARCHS:
      errors.append(
          f"{ctx}: invalid arch value {item!r} "
          f"(expected one of {sorted(ALLOWED_ARCHS)})"
      )
    elif item in seen:
      errors.append(f"{ctx}: duplicate arch value '{item}'")
    else:
      seen.add(item)
  return errors


def _validate_xfail_entry(
    rel: str, target_name: str, test_id: str, entry: object
) -> list[str]:
  ctx = f"{rel}: target '{target_name}' entry '{test_id}'"
  if not isinstance(entry, dict):
    return [f"{ctx}: must be a JSON object"]

  errors: list[str] = []
  unknown = sorted(set(entry.keys()) - ALLOWED_ENTRY_KEYS)
  if unknown:
    errors.append(f"{ctx}: unknown field(s): {', '.join(unknown)}")

  reason = entry.get("reason")
  if not isinstance(reason, str) or not reason.strip():
    errors.append(f"{ctx}: 'reason' must be a non-empty string")

  gap = entry.get("gap")
  if not isinstance(gap, str) or not GAP_ID_RE.match(gap):
    errors.append(
        f"{ctx}: 'gap' is required and must match ^GAP-[0-9]{{2,}}$ "
        f"(got {gap!r})"
    )

  errors.extend(_validate_entry_flags(ctx, entry))
  errors.extend(_validate_entry_os_bounds(ctx, entry))
  errors.extend(_validate_entry_arch(ctx, entry))
  return errors


def _validate_target_xfails(
    rel: str,
    target_name: str,
    xfails: dict[str, object],
    compiled_test_ids: set[str],
) -> list[str]:
  errors: list[str] = []
  for test_id, entry in sorted(xfails.items()):
    if not _is_valid_test_id(test_id):
      errors.append(
          f"{rel}: target '{target_name}' invalid test ID '{test_id}'"
      )
    elif test_id not in compiled_test_ids:
      errors.append(
          f"{rel}: orphan baseline test ID '{test_id}' in target "
          f"'{target_name}' xfails (not found in suite/src/domains/)"
      )
    errors.extend(_validate_xfail_entry(rel, target_name, test_id, entry))
  return errors


def _validate_target_remove_xfails(
    rel: str,
    target_name: str,
    remove_xfails: list[object],
    xfails: dict[str, object],
    compiled_test_ids: set[str],
) -> list[str]:
  errors: list[str] = []
  seen_rem: set[str] = set()
  for rem_id in remove_xfails:
    if not isinstance(rem_id, str) or not _is_valid_test_id(rem_id):
      errors.append(
          f"{rel}: target '{target_name}' invalid remove_xfails ID {rem_id!r}"
      )
      continue
    if rem_id in seen_rem:
      errors.append(
          f"{rel}: duplicate remove_xfails ID '{rem_id}' in target "
          f"'{target_name}'"
      )
    seen_rem.add(rem_id)
    if rem_id in xfails:
      errors.append(
          f"{rel}: test ID '{rem_id}' appears in both 'xfails' and "
          f"'remove_xfails' of target '{target_name}'"
      )
    if rem_id not in compiled_test_ids:
      errors.append(
          f"{rel}: orphan baseline test ID '{rem_id}' in target "
          f"'{target_name}' remove_xfails (not found in suite/src/domains/)"
      )
  return errors


def _validate_target_header(
    rel: str, target_name: str, tdef: dict[str, object]
) -> tuple[dict[str, object] | None, list[object] | None, list[str]]:
  errors: list[str] = []
  unknown_tkeys = sorted(set(tdef.keys()) - ALLOWED_TARGET_KEYS)
  if unknown_tkeys:
    errors.append(
        f"{rel}: target '{target_name}' has unknown field(s): "
        f"{', '.join(unknown_tkeys)}"
    )
  xfails = tdef.get("xfails", {})
  remove_xfails = tdef.get("remove_xfails", [])
  if not isinstance(xfails, dict):
    errors.append(
        f"{rel}: target '{target_name}' 'xfails' must be a JSON object"
    )
    return None, None, errors
  if not isinstance(remove_xfails, list):
    errors.append(
        f"{rel}: target '{target_name}' 'remove_xfails' must be a list"
    )
    return None, None, errors
  if remove_xfails and not tdef.get("extends"):
    errors.append(
        f"{rel}: target '{target_name}' specifies 'remove_xfails' without "
        "'extends'"
    )
  if tdef.get("extends_file") and not tdef.get("extends"):
    errors.append(
        f"{rel}: target '{target_name}' specifies 'extends_file' without "
        "'extends'"
    )
  return xfails, remove_xfails, errors


def _check_single_target(
    manifest_path: Path,
    rel: str,
    target_name: str,
    tdef: object,
    compiled_test_ids: set[str],
    cache: dict[Path, dict],
) -> tuple[dict[str, dict] | None, list[str]]:
  if not isinstance(tdef, dict):
    return None, [f"{rel}: target '{target_name}' must be a JSON object"]

  xfails, remove_xfails, errors = _validate_target_header(
      rel, target_name, tdef
  )
  if xfails is None or remove_xfails is None:
    return None, errors

  errors.extend(
      _validate_target_xfails(rel, target_name, xfails, compiled_test_ids)
  )
  errors.extend(
      _validate_target_remove_xfails(
          rel, target_name, remove_xfails, xfails, compiled_test_ids
      )
  )
  try:
    effective = _resolve_target_effective_xfails(
        manifest_path, target_name, cache, []
    )
  except ValueError as exc:
    errors.append(f"{rel}: target '{target_name}': {exc}")
    return None, errors
  return effective, errors


def _record_concrete_target(
    rel: str,
    family: str,
    target_name: str,
    tdef: object,
    effective: dict[str, dict] | None,
    concrete_resolved: dict[str, dict[str, dict]],
    family_targets: dict[str, set[str]],
    errors: list[str],
) -> int:
  if (
      effective is None
      or not isinstance(tdef, dict)
      or tdef.get("abstract", False)
  ):
    return 0
  if target_name in concrete_resolved:
    errors.append(
        f"{rel}: duplicate concrete target name '{target_name}' across "
        "manifests"
    )
  concrete_resolved[target_name] = effective
  family_targets[family].add(target_name)
  return 1


def _check_manifest_file(
    manifest_path: Path,
    fstest_dir: Path,
    compiled_test_ids: set[str],
    cache: dict[Path, dict],
    concrete_resolved: dict[str, dict[str, dict]],
    family_targets: dict[str, set[str]],
) -> list[str]:
  rel = manifest_path.relative_to(fstest_dir).as_posix()
  family = manifest_path.stem.removeprefix("dokan_")
  try:
    data = _load_manifest_cache(manifest_path, cache)
  except (OSError, json.JSONDecodeError, ValueError) as exc:
    return [f"{rel}: failed to parse JSON: {exc}"]

  errors: list[str] = []
  if data.get("schema_version") != 1:
    errors.append(
        f"{rel}: 'schema_version' must be 1 "
        f"(got {data.get('schema_version')!r})"
    )

  targets = data.get("targets")
  if not isinstance(targets, dict) or not targets:
    errors.append(f"{rel}: 'targets' must be a non-empty JSON object")
    return errors

  concrete_in_file = 0
  for target_name, tdef in targets.items():
    effective, target_errs = _check_single_target(
        manifest_path, rel, target_name, tdef, compiled_test_ids, cache
    )
    errors.extend(target_errs)
    concrete_in_file += _record_concrete_target(
        rel,
        family,
        target_name,
        tdef,
        effective,
        concrete_resolved,
        family_targets,
        errors,
    )

  if concrete_in_file == 0:
    errors.append(f"{rel}: manifest contains no non-abstract targets")
  return errors


def check_baselines(
    fstest_dir: Path, compiled_test_ids: set[str]
) -> tuple[dict[str, dict[str, dict]], dict[str, set[str]], list[str]]:
  """Check all baselines/dokan_*.json for schema, gaps, and orphan IDs."""
  concrete_resolved: dict[str, dict[str, dict]] = {}
  family_targets: dict[str, set[str]] = defaultdict(set)
  baselines_dir = fstest_dir / "baselines"
  manifest_files = sorted(baselines_dir.glob("dokan_*.json"))
  if not manifest_files:
    return (
        concrete_resolved,
        family_targets,
        [f"No dokan_*.json baseline files found under {baselines_dir}"],
    )

  errors: list[str] = []
  cache: dict[Path, dict] = {}
  for manifest_path in manifest_files:
    errors.extend(
        _check_manifest_file(
            manifest_path,
            fstest_dir,
            compiled_test_ids,
            cache,
            concrete_resolved,
            family_targets,
        )
    )
  return concrete_resolved, family_targets, errors


def _parse_gap_table_ids(text: str) -> tuple[list[str], list[str]]:
  """Parse Executive Summary GAP-XX rows from CONFORMANCE_GAPS.md."""
  errors: list[str] = []
  table_ids: list[str] = []
  seen: set[str] = set()
  for match in re.finditer(
      r"^\|\s*\[(GAP-[0-9]{2,})\][^|]*\|",
      text,
      re.MULTILINE,
  ):
    gid = match.group(1)
    if gid in seen:
      errors.append(
          f"CONFORMANCE_GAPS.md: duplicate Executive Summary row for {gid}"
      )
    seen.add(gid)
    table_ids.append(gid)
  return table_ids, errors


def _format_family_subset(
    fam: str, present: set[str], fam_all: set[str]
) -> list[str]:
  if present == fam_all:
    return [f"`{fam}`"]
  non_mntmgr = {t for t in fam_all if "mntmgr" not in t}
  if len(fam_all) > 2 and len(present) >= 2 and present == non_mntmgr:
    return [f"`{fam}` without `/o`"]
  if len(fam_all) > 2 and len(fam_all - present) == 1:
    excluded = next(iter(fam_all - present))
    return [f"`{fam}` without `{excluded}`"]
  return [f"`{t}`" for t in sorted(present)]


def _compact_targets(
    targets_set: set[str], family_targets: dict[str, set[str]]
) -> list[str]:
  """Format a target subset concisely (e.g. `mirror`, `mirror` without `/o`)."""
  tokens: list[str] = []
  remaining = set(targets_set)
  for fam in sorted(family_targets):
    fam_all = family_targets[fam]
    present = remaining & fam_all
    if not present:
      continue
    remaining -= present
    tokens.extend(_format_family_subset(fam, present, fam_all))
  tokens.extend(f"`{t}`" for t in sorted(remaining))
  return tokens


def _render_modifier_tokens(mods: Modifiers) -> list[str]:
  skip_crash, allow_xpass, os_min, os_max, arch = mods
  tokens: list[str] = []
  if os_min is not None:
    tokens.append(f"`os_min_build: {os_min}`")
  if os_max is not None:
    tokens.append(f"`os_max_build: {os_max}`")
  if arch:
    tokens.append(f"`arch: {', '.join(arch)}`")
  if allow_xpass:
    tokens.append("`allow_xpass`")
  if skip_crash:
    tokens.append("`skip_crash`")
  return tokens


def _render_covered_tests_block(
    tests_for_gap: dict[str, dict[str, Modifiers]],
    family_targets: dict[str, set[str]],
) -> str:
  """Render the deterministic markdown bullet list for one GAP-XX section."""
  lines: list[str] = []
  for test_id in sorted(tests_for_gap):
    by_mods: dict[Modifiers, set[str]] = defaultdict(set)
    for target_name, mods in tests_for_gap[test_id].items():
      by_mods[mods].add(target_name)

    clauses: list[tuple[str, Modifiers, str]] = []
    for mods, tset in by_mods.items():
      tokens = _compact_targets(tset, family_targets) + _render_modifier_tokens(
          mods
      )
      clause_text = ", ".join(tokens)
      clauses.append((min(tset), mods, clause_text))
    clauses.sort(key=lambda item: (item[0], item[1]))
    spec = "; ".join(c[2] for c in clauses)

    single = f"- `{test_id}` ({spec})"
    if len(single) <= MAX_LINE_LEN:
      lines.append(single)
    else:
      lines.append(f"- `{test_id}`")
      wrapped = textwrap.wrap(
          f"({spec})",
          width=MAX_LINE_LEN,
          initial_indent="  ",
          subsequent_indent="  ",
          break_long_words=False,
          break_on_hyphens=False,
      )
      lines.extend(wrapped)

  if any(len(ln) > MAX_LINE_LEN for ln in lines):
    return (
        "<!-- markdownlint-disable MD013 -->\n\n"
        + "\n".join(lines)
        + "\n\n<!-- markdownlint-enable MD013 -->"
    )
  return "\n".join(lines)


def _split_gap_sections(
    body_text: str,
) -> tuple[str, list[tuple[str, str]], set[str], list[str]]:
  parts = re.split(r"(?m)^(?=### GAP-[0-9]{2,}:)", body_text)
  preamble = parts[0]
  section_ids: set[str] = set()
  parsed_sections: list[tuple[str, str]] = []
  errors: list[str] = []
  for sec in parts[1:]:
    match = re.match(r"^### (GAP-[0-9]{2,}):", sec)
    if match is None:
      continue
    gid = match.group(1)
    if gid in section_ids:
      errors.append(
          f"CONFORMANCE_GAPS.md: duplicate section heading for {gid}"
      )
    section_ids.add(gid)
    parsed_sections.append((gid, sec))
  return preamble, parsed_sections, section_ids, errors


def _check_table_section_parity(
    table_ids: list[str], section_ids: set[str]
) -> list[str]:
  errors: list[str] = []
  missing_sec = sorted(set(table_ids) - section_ids)
  missing_tbl = sorted(section_ids - set(table_ids))
  if missing_sec:
    errors.append(
        "CONFORMANCE_GAPS.md: cluster(s) in Executive Summary table missing "
        f"section: {', '.join(missing_sec)}"
    )
  if missing_tbl:
    errors.append(
        "CONFORMANCE_GAPS.md: cluster section(s) missing from Executive "
        f"Summary table: {', '.join(missing_tbl)}"
    )
  return errors


def _group_entries_by_gap(
    concrete_resolved: dict[str, dict[str, dict]], section_ids: set[str]
) -> tuple[dict[str, dict[str, dict[str, Modifiers]]], list[str]]:
  by_gap: dict[str, dict[str, dict[str, Modifiers]]] = defaultdict(
      lambda: defaultdict(dict)
  )
  errors: list[str] = []
  for target_name, entries in sorted(concrete_resolved.items()):
    for test_id, entry in sorted(entries.items()):
      gap = entry.get("gap")
      if not isinstance(gap, str) or not GAP_ID_RE.match(gap):
        continue
      if gap not in section_ids:
        errors.append(
            f"CONFORMANCE_GAPS.md: manifest target '{target_name}' entry "
            f"'{test_id}' references unknown cluster '{gap}'"
        )
        continue
      by_gap[gap][test_id][target_name] = _extract_modifiers(entry)
  return by_gap, errors


def _rebuild_gap_sections(
    parsed_sections: list[tuple[str, str]],
    by_gap: dict[str, dict[str, dict[str, Modifiers]]],
    family_targets: dict[str, set[str]],
) -> tuple[list[str], list[str], list[str]]:
  rebuilt: list[str] = []
  drifted: list[str] = []
  errors: list[str] = []
  total = len(parsed_sections)
  for idx, (gid, sec) in enumerate(parsed_sections):
    if "**Covered Tests:**" not in sec:
      errors.append(
          f"CONFORMANCE_GAPS.md: section {gid} is missing '**Covered Tests:**'"
      )
      rebuilt.append(sec)
      continue

    tests_for_gap = by_gap.get(gid, {})
    if not tests_for_gap:
      errors.append(
          f"CONFORMANCE_GAPS.md: section {gid} has zero matching baseline "
          "entries in baselines/dokan_*.json (remove obsolete cluster or fix "
          "'gap' tags)"
      )
      rebuilt.append(sec)
      continue

    prefix = sec.split("**Covered Tests:**", 1)[0]
    rendered_block = _render_covered_tests_block(tests_for_gap, family_targets)
    trailing_sep = "\n\n" if idx < total - 1 else "\n"
    new_sec = f"{prefix}**Covered Tests:**\n\n{rendered_block}{trailing_sep}"
    if new_sec != sec:
      drifted.append(gid)
    rebuilt.append(new_sec)
  return rebuilt, drifted, errors


def check_or_sync_conformance_gaps_doc(
    fstest_dir: Path,
    concrete_resolved: dict[str, dict[str, dict]],
    family_targets: dict[str, set[str]],
    sync: bool,
) -> tuple[list[str], list[str]]:
  """Check (or regenerate when sync=True) CONFORMANCE_GAPS.md from JSON."""
  doc_path = fstest_dir / "CONFORMANCE_GAPS.md"
  if not doc_path.is_file():
    return [f"Missing conformance gaps document: {doc_path}"], []

  original_text = doc_path.read_text(encoding="utf-8")
  table_ids, errors = _parse_gap_table_ids(original_text)
  footer_match = REF_FOOTER_RE.search(original_text)
  body_text = (
      original_text[: footer_match.start()] if footer_match else original_text
  )
  footer_text = (
      original_text[footer_match.start() :] if footer_match else ""
  )

  preamble, parsed_sections, section_ids, sec_errs = _split_gap_sections(
      body_text
  )
  errors.extend(sec_errs)
  errors.extend(_check_table_section_parity(table_ids, section_ids))
  by_gap, group_errs = _group_entries_by_gap(concrete_resolved, section_ids)
  errors.extend(group_errs)

  rebuilt_sections, drifted_gaps, rebuild_errs = _rebuild_gap_sections(
      parsed_sections, by_gap, family_targets
  )
  errors.extend(rebuild_errs)

  rendered_text = preamble + "".join(rebuilt_sections) + footer_text
  if not rendered_text.endswith("\n"):
    rendered_text += "\n"

  if rendered_text != original_text:
    if sync:
      doc_path.write_text(rendered_text, encoding="utf-8", newline="\n")
    else:
      gap_list = ", ".join(drifted_gaps) if drifted_gaps else "(formatting)"
      errors.append(
          "CONFORMANCE_GAPS.md Covered Tests out of sync with "
          f"baselines/dokan_*.json in: {gap_list}. "
          "Run: python tests/fstest/scripts/check_conformance_docs.py --sync"
      )
  return errors, drifted_gaps


def check_spdx_headers(fstest_dir: Path) -> list[str]:
  """Verify every .h/.cpp file under tests/fstest/ starts with MIT SPDX."""
  errors: list[str] = []
  for root, dirs, files in os.walk(fstest_dir):
    dirs[:] = sorted(d for d in dirs if not _should_skip_dir(d))
    for fname in sorted(files):
      if not (fname.endswith(".h") or fname.endswith(".cpp")):
        continue
      fpath = Path(root) / fname
      rel = fpath.relative_to(fstest_dir).as_posix()
      with fpath.open("r", encoding="utf-8", errors="replace") as fh:
        first_line = fh.readline().rstrip("\r\n")
      if first_line != SPDX_FIRST_LINE:
        errors.append(
            f"{rel}:1: missing '{SPDX_FIRST_LINE}' header (found: "
            f"{first_line!r})"
        )
  return errors


def main(argv: list[str] | None = None) -> int:
  parser = argparse.ArgumentParser(
      description="Validate tests/fstest spec citations, baselines, "
      "CONFORMANCE_GAPS.md parity, and MIT SPDX headers."
  )
  parser.add_argument(
      "--fstest-dir",
      type=Path,
      default=Path(__file__).resolve().parent.parent,
      help="Path to tests/fstest directory (default: auto-detected).",
  )
  parser.add_argument(
      "--sync",
      action="store_true",
      help="Regenerate every cluster's '**Covered Tests:**' block in "
      "CONFORMANCE_GAPS.md from baselines/dokan_*.json.",
  )
  args = parser.parse_args(argv)
  fstest_dir = args.fstest_dir.resolve()

  compiled_ids, spec_errors = check_domain_tests_and_specs(fstest_dir)
  concrete_resolved, family_targets, baseline_errors = check_baselines(
      fstest_dir, compiled_ids
  )
  doc_errors, synced_gaps = check_or_sync_conformance_gaps_doc(
      fstest_dir,
      concrete_resolved,
      family_targets,
      sync=args.sync and not baseline_errors,
  )
  spdx_errors = check_spdx_headers(fstest_dir)

  all_sections = [
      ("Spec citation checks", spec_errors),
      ("Baseline manifest checks", baseline_errors),
      ("CONFORMANCE_GAPS.md sync checks", doc_errors),
      ("SPDX license header checks", spdx_errors),
  ]

  total_errors = sum(len(errs) for _, errs in all_sections)
  if total_errors == 0:
    unique_ids = {
        tid for entries in concrete_resolved.values() for tid in entries
    }
    sync_note = (
        f" (synced {', '.join(synced_gaps)})"
        if args.sync and synced_gaps
        else ""
    )
    print(
        f"OK: {len(compiled_ids)} conformance tests, "
        f"{len(concrete_resolved)} concrete baseline targets, "
        f"{len(unique_ids)} unique baselined IDs, CONFORMANCE_GAPS.md "
        f"Covered Tests{sync_note}, and all SPDX headers verified."
    )
    return 0

  for title, errs in all_sections:
    if errs:
      print(f"\n=== {title} ({len(errs)} error(s)) ===", file=sys.stderr)
      for err in errs:
        print(f"  {err}", file=sys.stderr)
  print(
      f"\nFAILED: {total_errors} conformance doc/hygiene error(s).",
      file=sys.stderr,
  )
  return 1


if __name__ == "__main__":
  raise SystemExit(main())
