<!-- markdownlint-disable MD043 -->

# Dokan NTFS Conformance Gaps

This catalog documents every tracked NTFS conformance failure recorded in
[`baselines/dokan_memfs.json`](baselines/dokan_memfs.json) and
[`baselines/dokan_mirror.json`](baselines/dokan_mirror.json), grouped below into
root-cause clusters (`GAP-01`–`GAP-30`). The baseline JSON manifests are the
single source of truth via each entry's `"gap"` field; every cluster's
**Covered Tests** list below is generated from the manifests via
`python tests/fstest/scripts/check_conformance_docs.py --sync` and verified
in CI by
[`scripts/check_conformance_docs.py`](scripts/check_conformance_docs.py).

Every cluster pairs a verified Microsoft specification reference (`[MS-FSA]`,
`[MS-FSCC]`, or Win32/WDK documentation on Microsoft Learn) with the observed
behavior in `dokan3.sys` (`sys/`), `dokan3.dll` (`dokan/`), `memfs.exe`
(`samples/dokan_memfs/`), or `mirror.exe` (`samples/dokan_mirror/`). See
[`baselines/schema.md`](baselines/schema.md) for manifest semantics
(`gap`, `skip_crash`, `allow_xpass`, `os_min_build`, `os_max_build`).

---

## Top Critical Bugs

Four high-impact defects stand out because they crash the filesystem daemon,
break all UNC renames, or corrupt mapped-file and image-delete invariants:

1. **[GAP-01](#gap-01-recursive-move-crash-on-subtree-or-volume-root) —
   Infinite recursion crash in `memfs` (`Crash / Handle Leak`):**
   `fs_filenodes::move` in `samples/dokan_memfs/filenodes.cpp` does not reject
   moving a directory into its own descendant, causing unbounded recursion and
   a stack-overflow crash in `memfs.exe`.
2. **[GAP-03](#gap-03-unc-prefix-not-stripped-in-fully-qualified-renames) —
   Broken renames on UNC mounts (`Semantic Bug`):**
   `PopulateRenameEventInformations` in `sys/fileinfo.c` copies
   `targetFileObject->FileName` verbatim without stripping `Dcb->UNCName`,
   failing `MoveFileExW`, `ReplaceFileW`, and absolute-path
   `FileRenameInformation` tests on `mirror_unc`.
3. **[GAP-04](#gap-04-path-keyed-fcb-aliasing-on-mapped-rename) —
   Path-keyed FCB aliasing on rename over data-mapped files (`Semantic Bug`):**
   `DokanCompareFcb` and `DokanRenameFcb` in `sys/util/fcb.c` key
   `Vcb->FcbTable` strictly by path name; when a data-mapped file is replaced
   by rename, subsequent paging reads on the old section fault bytes from the
   replacement file at that path.
4. **[GAP-05](#gap-05-deleting-image-mapped-and-user-mapped-files) —
   Deleting `SEC_IMAGE`-mapped files despite `STATUS_CANNOT_DELETE`
   (`Semantic Bug`):** `DokanCompleteSetInformation` in `sys/fileinfo.c` calls
   `MmFlushImageSection(..., MmFlushForDelete)` only *after* user-mode
   `DeleteFile` already marked the file for deletion (failing to roll back
   backing disposition on `mirror`), and `dokan3.sys` retains
   `DOKAN_FCB_STATE_DELETE_PENDING` on cached post-unmap `SEC_IMAGE` FCBs
   (`memfs`).

---

## Executive Summary

Some tests fail for distinct root causes on different targets (for example,
`RenameMoveTest.RenameDirectoryIntoOwnSubtreeFails` hits the `memfs` crash in
`GAP-01` and the UNC rename bug in `GAP-03`). Exact per-target counts and
modifiers live in the JSON manifests under `baselines/`.

<!-- markdownlint-disable MD013 -->

| Cluster | Title | Affected Targets | Severity |
| :--- | :--- | :--- | :--- |
| [GAP-01](#gap-01-recursive-move-crash-on-subtree-or-volume-root) | Recursive Move Crash on Subtree or Volume Root | `memfs` | Crash / Handle Leak |
| [GAP-03](#gap-03-unc-prefix-not-stripped-in-fully-qualified-renames) | UNC Prefix Not Stripped in Fully Qualified Renames | `mirror_unc` | Semantic Bug |
| [GAP-04](#gap-04-path-keyed-fcb-aliasing-on-mapped-rename) | Path-Keyed FCB Aliasing on Mapped Rename | `memfs`, `mirror` (local) | Semantic Bug |
| [GAP-05](#gap-05-deleting-image-mapped-and-user-mapped-files) | Deleting Image-Mapped and User-Mapped Files | `memfs`, `mirror` | Semantic Bug |
| [GAP-06](#gap-06-supersede-maximum-allowed-and-read-only-opens) | `FILE_SUPERSEDE`, `MAXIMUM_ALLOWED`, and Read-Only Opens | `memfs`, `mirror` | Semantic Bug |
| [GAP-07](#gap-07-missing-write-path-byte-range-lock-enforcement) | Missing Write-Path Byte-Range Lock Enforcement | `memfs`, `mirror` | Semantic Bug |
| [GAP-08](#gap-08-mapped-view-coherence-and-no-buffering-alignment) | Mapped-View Coherence and `NO_BUFFERING` Alignment | `memfs`, `mirror`, `mirror_unc` | Semantic Bug |
| [GAP-09](#gap-09-changetime-aliasing-allocationsize-and-dir-sizing) | `ChangeTime` Aliasing, `AllocationSize`, and Dir Sizing | `memfs`, `mirror` | Semantic Bug |
| [GAP-10](#gap-10-memfs-timestamp-sentinels-updates-and-archive-bit) | `memfs` Timestamp Sentinels, Updates, and Archive Bit | `memfs` | Semantic Bug |
| [GAP-11](#gap-11-oplock-breaks-batch-ordering-and-complete-if-oplocked) | Oplock Breaks, Batch Ordering, and `FILE_COMPLETE_IF_OPLOCKED` | `memfs`, `mirror` | Semantic Bug |
| [GAP-12](#gap-12-directory-enumeration-classes-wildcards-and-ex-flags) | Directory Enumeration Classes, Wildcards, and Ex Flags | `memfs`, `mirror` | Semantic Bug |
| [GAP-13](#gap-13-file-info-queries-change-notify-and-mount-manager) | File Info Queries, Change Notify, and Mount Manager | `memfs`, `mirror`, `mirror_unc` | Semantic Bug |
| [GAP-14](#gap-14-trailing-backslash-and-mirror-non-verbatim-paths) | Trailing Backslash and `mirror` Non-Verbatim Paths | `memfs`, `mirror` | Semantic Bug |
| [GAP-15](#gap-15-memfs-case-sensitivity-unicode-folding-and-path-checks) | `memfs` Case Sensitivity, Unicode Folding, and Path Checks | `memfs` | Semantic Bug |
| [GAP-16](#gap-16-memfs-non-empty-dir-delete-and-rename-validation) | `memfs` Non-Empty Dir Delete and Rename Validation | `memfs` | Semantic Bug |
| [GAP-17](#gap-17-ads-enumeration-status-and-mirror-stream-rename) | ADS Enumeration Status and `mirror` Stream Rename | `memfs`, `mirror` | Semantic Bug |
| [GAP-18](#gap-18-memfs-alternate-data-stream-semantics) | `memfs` Alternate Data Stream Semantics | `memfs` | Semantic Bug |
| [GAP-19](#gap-19-mirror-privileges-impersonation-and-owner-rights) | `mirror` Privileges, Impersonation, and Owner Rights | `mirror` | Semantic Bug |
| [GAP-20](#gap-20-memfs-persistent-acls-inheritance-and-access-checks) | `memfs` Persistent ACLs, Inheritance, and Access Checks | `memfs` | Unimplemented NTFS Feature |
| [GAP-21](#gap-21-posix-delete-rename-and-extended-disposition-flags) | POSIX Delete/Rename and Extended Disposition Flags | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-22](#gap-22-reparse-points-symbolic-links-and-junctions) | Reparse Points, Symbolic Links, and Junctions | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-23](#gap-23-hard-links-filelinkinformation-and-enumeration) | Hard Links (`FileLinkInformation`) and Enumeration | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-24](#gap-24-sparse-files-and-zero-data-deallocation) | Sparse Files and `FSCTL_SET_ZERO_DATA` Deallocation | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-25](#gap-25-ntfs-file-compression) | NTFS File Compression (`FSCTL_SET_COMPRESSION`) | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-26](#gap-26-extended-attributes-query-and-set) | Extended Attributes (`IRP_MJ_QUERY_EA` / `SET_EA`) | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-27](#gap-27-open-by-file-id-and-ntfs-object-ids) | Open-by-File-ID and NTFS Object IDs | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-28](#gap-28-modern-info-classes-and-volume-feature-flags) | Modern Info Classes and Volume Feature Flags | `memfs`, `mirror` | Unimplemented NTFS Feature |
| [GAP-30](#gap-30-missing-vcb-lock-on-filerenameinformationex-fcb-rename) | Missing VCB Lock on `FileRenameInformationEx` FCB Rename | `memfs`, `mirror` | Crash / Handle Leak |

<!-- markdownlint-enable MD013 -->

---

## Known Skipped Coverage (Not Baselined)

Some conformance tests skip before asserting because the probed volume reports
an unsupported feature rather than failing an assertion. These skipped domains
are intentionally absent from `baselines/dokan_*.json` and tracked here instead:

- **8.3 Short Names (`REQUIRE_SHORT_NAMES_ENABLED()`):** In `dokan/directory.c`,
  `DokanWriteDirectoryEntry` (`:107-159`) leaves `ShortNameLength`
  zero-initialized for `FileBothDirectoryInformation`,
  `FileIdBothDirectoryInformation`, and `FileIdExtdBothDirectoryInformation`,
  discarding `WIN32_FIND_DATAW::cAlternateFileName` even when `mirror.c`
  populates it from an 8.3-enabled backing NTFS volume, and
  `DispatchSetInformation` (`dokan/setfile.c:283-329`) does not handle
  `FileShortNameInformation`. As a result, `ProbeShortNamesEnabled` observes
  `short_name_length == 0` and the five 8.3 tests
  (`DirEnumTest.FindFirstFileMatches8dot3ShortNameWildcard`,
  `InfoQueriesTest.NtQueryFileAlternateNameInformation`,
  `NamingPathsTest.SetFileShortNameExplicit`,
  `NamingPathsTest.ShortName8dot3CollisionSequence`, and
  `NamingPathsTest.ShortName8dot3GenerationAndLookup`) skip on both `memfs` and
  `mirror`.

---

## Root-Cause Clusters

### GAP-01: Recursive Move Crash on Subtree or Volume Root

- **Severity:** Crash / Handle Leak
- **Affected Targets:** `memfs` (`skip_crash: true`)

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.15.12 (`FileRenameInformation`)][ms-fsa-rename] and
[`FILE_RENAME_INFORMATION`][wdk-rename-info], renaming the volume root or
moving a directory into one of its own descendants is invalid and must fail
(for example with `STATUS_INVALID_PARAMETER` or `STATUS_ACCESS_DENIED`)
without mutating the directory hierarchy.

**Observed Dokan Behavior:**
While `DokanSetRenameInformation` (`dokan/setfile.c`) rejects renaming the
volume root `L"\\"` centrally with `STATUS_ACCESS_DENIED`, `fs_filenodes::move`
in `samples/dokan_memfs/filenodes.cpp` never checks whether `old_filename` is
an ancestor prefix of `new_filename`. It first calls `add(f, {})` (inserting
the moved directory inside its own descendant map) and then iterates
`list_folder(old_filename)`, calling `move(...)` recursively until stack
exhaustion crashes `memfs.exe`.

**Covered Tests:**

- `RenameMoveTest.RenameDirectoryIntoOwnSubtreeFails` (`memfs`, `skip_crash`)

### GAP-03: UNC Prefix Not Stripped in Fully Qualified Renames

- **Severity:** Semantic Bug
- **Affected Targets:** `mirror_unc`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.15.12 (`FileRenameInformation`)][ms-fsa-rename] and
[`FILE_RENAME_INFORMATION`][wdk-rename-info], fully qualified intra-volume
renames (`RootDirectory == NULL`) must resolve the target path relative to
the mounted share or volume root identically across local drive letters and
network UNC shares.

**Observed Dokan Behavior:**
While `DokanDispatchCreate` in `sys/create.c` strips
`RequestContext->Dcb->UNCName` from incoming create paths via
`RtlPrefixUnicodeString`, `PopulateRenameEventInformations` in `sys/fileinfo.c`
copies `targetFileObject->FileName` verbatim into `DOKAN_RENAME_INFORMATION`
without stripping `Dcb->UNCName`. User-mode `MirrorMoveFile`
(`samples/dokan_mirror/mirror.c`) then prepends `gRootDirectory` to a path
still starting with `\myfs\dokan\...`, causing every `MoveFileExW`,
`ReplaceFileW`, and absolute-path `FileRenameInformation` call on UNC mounts
to fail with `ERROR_PATH_NOT_FOUND` / `STATUS_OBJECT_PATH_NOT_FOUND`.

**Covered Tests:**

- `CreateOpenTest.ReOpenFileUpgradesAccessAfterRename` (`mirror_unc`)
- `CreateOpenTest.ReadOnlyDirectoryAllowsChildCreateModifyDelete` (`mirror_unc`)
- `InfoQueriesTest.FileId128BitInformation` (`mirror_unc`)
- `InfoQueriesTest.FileIndexStabilityAndUniqueness` (`mirror_unc`)
- `InfoQueriesTest.ReadDirectoryChangesCrossDirectoryMove` (`mirror_unc`)
- `InfoQueriesTest.ReadDirectoryChangesDirNameRenamed` (`mirror_unc`)
- `InfoQueriesTest.ReadDirectoryChangesRenameOldAndNewName` (`mirror_unc`)
- `LocksOplocksTest.OplockRhBrokenByMoveFileEx` (`mirror_unc`)
- `LocksOplocksTest.OplockRhBrokenByRenameAndDeleteWithShareDelete`
  (`mirror_unc`)
- `RenameMoveTest.CaseOnlyRenameFileAndDirectory` (`mirror_unc`)
- `RenameMoveTest.ConcurrentRenameAndEnumerateStress` (`mirror_unc`)
- `RenameMoveTest.FileRenameInfoExNonPosixBasicAndReplace` (`mirror_unc`)
- `RenameMoveTest.FileRenameInfoReplaceOpenTargetNonPosixFails` (`mirror_unc`)
- `RenameMoveTest.MoveDirectoryReplaceFailsAndAfterRemoveSucceeds`
  (`mirror_unc`)
- `RenameMoveTest.MoveFileAcrossSubdirectories` (`mirror_unc`)
- `RenameMoveTest.MoveFileBasicReplaceAndReadOnlyTarget` (`mirror_unc`)
- `RenameMoveTest.MoveNonEmptyDirectorySubtreeAcrossParents` (`mirror_unc`)
- `RenameMoveTest.RenameDeletePendingFileFails` (`mirror_unc`)
- `RenameMoveTest.RenameDirectoryIntoOwnSubtreeFails` (`mirror_unc`)
- `RenameMoveTest.RenameDirectoryOverExistingFileSemantics` (`mirror_unc`)
- `RenameMoveTest.RenameDirectoryWhileDirectoryHandleOpen` (`mirror_unc`)
- `RenameMoveTest.RenameDirectoryWithOpenDescendantFails` (`mirror_unc`)
- `RenameMoveTest.RenameFileOverExistingDirectoryFails` (`mirror_unc`)
- `RenameMoveTest.RenameOntoDeletePendingTargetFailsAccessDenied` (`mirror_unc`)
- `RenameMoveTest.RenameOverDataMappedTargetPreservesMappedView` (`mirror_unc`)
- `RenameMoveTest.RenamePreservesAlternateDataStreams` (`mirror_unc`)
- `RenameMoveTest.RenamePreservesFileCreationAndLastWriteTimes` (`mirror_unc`)
- `RenameMoveTest.RenamePreservesFileIndexAndData` (`mirror_unc`)
- `RenameMoveTest.RenameReadOnlySourceSucceedsAndKeepsReadOnly` (`mirror_unc`)
- `RenameMoveTest.RenameSameNameNoOp` (`mirror_unc`)
- `RenameMoveTest.RenameUpdatesBothParentDirLastWriteTimes` (`mirror_unc`)
- `RenameMoveTest.RenameUpdatesFileChangeTime` (`mirror_unc`)
- `RenameMoveTest.RenameWhileOpenWithShareDelete` (`mirror_unc`)
- `RenameMoveTest.RenameWhileOpenWithoutShareDeleteFails` (`mirror_unc`)
- `RenameMoveTest.ReplaceFileWAtomicSwapPreservesCreationTimeAndBackup`
  (`mirror_unc`)
- `SecurityAclTest.SecurityDescriptorPreservedAcrossIntraVolumeRename`
  (`mirror_unc`)
- `StreamsTest.StreamRenameToNonStreamTargetFailsInvalidParameter`
  (`mirror_unc`)
- `TimestampsAttrsTest.RenameUpdatesChangeTimeNotLastWriteTime` (`mirror_unc`)

### GAP-04: Path-Keyed FCB Aliasing on Mapped Rename

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror` without `mirror_unc` (`allow_xpass`
  on local `mirror`)

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.15.12 (`FileRenameInformation`)][ms-fsa-rename] and
[Reading and Writing From a File View][win32-file-view], replacing a file
target via `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` while an active data
section remains mapped (after its user handle was closed) must preserve the
original file's backing data in the existing mapped view rather than aliasing
the replacement file's contents.

**Observed Dokan Behavior:**
`DokanCompareFcb` and `DokanRenameFcb` in `sys/util/fcb.c` index
`Vcb->FcbTable` strictly by path string (`Fcb->FileName`). When a rename
replaces an existing mapped target, `DokanRenameFcb` marks the displaced FCB
`ReplacedByRename = TRUE` but leaves its `Fcb->FileName` unchanged. When
paging I/O (`DokanDispatchRead` in `sys/read.c`) later faults non-resident
pages for the old section, `dokan3.sys` dispatches that path name to user
mode (`MirrorReadFile` / `memfs_readfile`), which reads from the newly
renamed replacement file instead of the unlinked original object.

**Covered Tests:**

- `RenameMoveTest.RenameOverDataMappedTargetPreservesMappedView`
  (`memfs`; `mirror` without `mirror_unc`, `allow_xpass`)

### GAP-05: Deleting Image-Mapped and User-Mapped Files

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [`MmFlushImageSection`][wdk-mmflushimage],
[MS-FSA 2.1.5.1.2 (Open of an Existing File)][ms-fsa-open-existing], and
[MS-FSA 2.1.5.15.3 (`FileDispositionInformation`)][ms-fsa-disp], attempting to
mark a file mapped as an executable image (`SEC_IMAGE`) for deletion must fail
with `STATUS_CANNOT_DELETE` and leave the file intact after unmap, opening a
`SEC_IMAGE`-mapped file for write access (`GENERIC_WRITE`) must fail with
`STATUS_SHARING_VIOLATION` (`ERROR_SHARING_VIOLATION`), and opening a
data-mapped file with `DELETE` access is allowed while legacy
`FileDispositionInformation` rejects deletion while mapped.

**Observed Dokan Behavior:**
In `sys/fileinfo.c:876-881`, `DokanCompleteSetInformation` invokes
`MmFlushImageSection(&fcb->SectionObjectPointers, MmFlushForDelete)` only
*after* `DokanSetDispositionInformation` (`dokan/setfile.c`) has already
invoked user-mode `MirrorDeleteFile` (`samples/dokan_mirror/mirror.c:850-856`)
or `memfs_deletefile` (`samples/dokan_memfs/memfs_operations.cpp:507-521`). On
`mirror`, `MirrorDeleteFile` has already set `FileDispositionInfo = TRUE` on
the backing NTFS handle (including for data-mapped files, where
`MirrorCreateFile` succeeds on `DELETE` opens with `kShareAll`), and
`dokan3.sys` never sends a rollback request when returning
`STATUS_CANNOT_DELETE`, so the backing file is deleted as soon as the mapped
section unmaps. Separately, `memfs_deletefile` is stateless (no rollback leak),
but on `memfs` (`DeleteImageMappedFileFailsCannotDelete`), once an unmapped
`SEC_IMAGE` file is deleted via `DeleteFileW`, `sys/cleanup.c:138-145` removes
the node while Windows Memory Manager (builds `>= 17763`, passing on Server
2016 build 14393) retains a cached `ImageSectionObject` that delays
`IRP_MJ_CLOSE`; the deleted FCB remains in `Vcb->FcbTable` with
`DOKAN_FCB_STATE_DELETE_PENDING` still set (only cleared on GC reuse at
`sys/util/fcb.c:294`), so subsequent opens hit `DokanCheckShareAccess`
(`sys/create.c:294-295`) and fail with `STATUS_DELETE_PENDING` instead of
`STATUS_OBJECT_NAME_NOT_FOUND`. In `sys/create.c`, `DokanDispatchCreate` never
calls `MmFlushImageSection(&fcb->SectionObjectPointers, MmFlushForWrite)` on
write opens against an active `ImageSectionObject` (and skips
`DokanCheckShareAccess` altogether when `fcb->OpenCount == 1` after the mapping
handle was closed), allowing `GENERIC_WRITE` opens on `SEC_IMAGE`-mapped files
instead of failing `STATUS_SHARING_VIOLATION`, and never calls
`MmCanFileBeTruncated` on overwrite/supersede (`CREATE_ALWAYS` /
`TRUNCATE_EXISTING` / `FILE_SUPERSEDE`) against an active user-mapped section
(allowing truncation instead of `STATUS_USER_MAPPED_FILE`).

**Covered Tests:**

- `CreateOpenTest.OverwriteAndSupersedeOfUserMappedFileFail` (`memfs`, `mirror`)
- `DataIoTest.ImageMappedFileRejectsWriteOpen` (`memfs`, `mirror`)
- `DeleteTest.DeleteFileWOnMappedFileFailsBeforeWin11_24H2` (`mirror`)
- `DeleteTest.DeleteImageMappedFileFailsCannotDelete`
  (`memfs`, `os_min_build: 17763`, `allow_xpass`; `mirror`)
- `DeleteTest.DeleteMappedFileFailsCannotDelete` (`mirror`)
- `DeleteTest.DispositionInfoExForceImageSectionCheck` (`mirror`)

### GAP-06: Supersede, Maximum-Allowed, and Read-Only Opens

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.1.2 (Open of an Existing File)][ms-fsa-open-existing],
[MS-FSA 2.1.5.15.3 (`FileDispositionInformation`)][ms-fsa-disp], and
[`ZwCreateFile`][wdk-zwcreatefile], `FILE_SUPERSEDE` requires `DELETE`
compatibility (both `FILE_SHARE_DELETE` on existing handles and `DELETE` or
parent `FILE_DELETE_CHILD` DACL permission), `MAXIMUM_ALLOWED` on a
`FILE_ATTRIBUTE_READONLY` file must strip `FILE_WRITE_DATA` and
`FILE_APPEND_DATA` from granted access, explicit `FILE_APPEND_DATA` opens on
a read-only file must fail with `STATUS_ACCESS_DENIED`, and creating a child
inside a legacy delete-pending directory must fail with
`STATUS_DELETE_PENDING` (`ERROR_ACCESS_DENIED`).

**Observed Dokan Behavior:**
`DokanCheckShareAccess` in `sys/create.c` (`:1024-1027`) evaluates
`IoCheckShareAccess` against raw `DesiredAccess` without synthesizing `DELETE`
for `FILE_SUPERSEDE` or `FILE_WRITE_DATA` for `FILE_OVERWRITE` /
`FILE_OVERWRITE_IF`, and `DokanMapKernelToUserCreateFileFlags`
(`dokan/dokan.c`) collapses `FILE_SUPERSEDE` into `CREATE_ALWAYS`, causing
`MirrorCreateFile` to open backing files with `FILE_OVERWRITE_IF` semantics
and succeed even when `DELETE` is denied in the DACL. Neither `dokan3.sys`
nor the sample filesystems evaluate DACLs or strip write/append access bits
when resolving `MAXIMUM_ALLOWED` on read-only or restricted-DACL files. In
`samples/dokan_memfs/memfs_operations.cpp`, `memfs_createfile` checks
`desiredaccess & FILE_WRITE_DATA` for read-only files but omits
`FILE_APPEND_DATA`. Finally, `DokanCheckShareAccess` (`sys/create.c:294-295`)
checks `DokanFCBIsPendingDeletion` (`DOKAN_FCB_STATE_DELETE_PENDING`,
`sys/public.h:108`) only on the target child FCB, never on its parent
directory FCB, while `MirrorDeleteDirectory` (`mirror.c:861-915`) and
`memfs_deletedirectory` (`memfs_operations.cpp:524-536`) only check emptiness
(deferring deletion to `Cleanup` without marking the directory delete-pending).
In `memfs`, `memfs_createfile`
(`samples/dokan_memfs/memfs_operations.cpp:163-166`) deliberately replaces
existing attributes on `FILE_SUPERSEDE` per
[MS-FSA 2.1.5.1.2][ms-fsa-open-existing] prose instead of retaining
`FILE_ATTRIBUTE_TEMPORARY` as observed on real NTFS (an NTFS-observed vs
`[MS-FSA]` prose divergence; `mirror` matches NTFS here only because
`DokanMapKernelToUserCreateFileFlags` in `dokan/dokan.c` collapses
`FILE_SUPERSEDE` into `CREATE_ALWAYS`).

**Covered Tests:**

- `CreateOpenTest.MaximumAllowedGrantsEffectiveAccessAndQueriesFileAccessInfo`
  (`memfs`, `mirror`)
- `CreateOpenTest.NtSupersedeAndOverwriteIfMergeAttributes` (`memfs`)
- `CreateOpenTest.NtSupersedeRequiresDeleteSharingUnlikeOverwriteIf`
  (`memfs`, `mirror`)
- `CreateOpenTest.NtSupersedeVsOverwriteDaclAccess` (`mirror`)
- `CreateOpenTest.OverwriteWithReadOnlyAccessChecksWriteShare` (`memfs`)
- `CreateOpenTest.ReadOnlyFileBlocksWriteOpenAllowsReadAndWriteAttr` (`memfs`)
- `DeleteTest.CreateChildInDeletePendingDirFailsDeletePending`
  (`memfs`, `mirror`)
- `SecurityAclTest.MaximumAllowedGrantsOnlyDaclPermittedRights`
  (`memfs`, `mirror`)

### GAP-07: Missing Write-Path Byte-Range Lock Enforcement

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.4 (Write) and 2.1.5.8 (Byte-Range Lock)][ms-fsa-events],
[`FsRtlCheckLockForWriteAccess`][wdk-check-write-lock], and
[`LockFileEx`][win32-lockfileex], both exclusive and shared byte-range locks
must block conflicting writes from other handles (and shared locks must block
writes even from the locking handle) with `STATUS_FILE_LOCK_CONFLICT`
(`ERROR_LOCK_VIOLATION`), tracked independently per stream.

**Observed Dokan Behavior:**
`DokanCommonLockControl` (`sys/lock.c`) stores kernel-mode locks in
`fcb->FileLock` via `FsRtlProcessFileLock`, and `DokanDispatchRead`
(`sys/read.c:233`) calls `FsRtlCheckLockForReadAccess`, but
`DokanDispatchWrite` (`sys/write.c`) never calls
`FsRtlCheckLockForWriteAccess`. As a result, `WriteFile` succeeds across
conflicting exclusive and shared locks on both main and alternate streams. In
`memfs`, `StreamsTest.StreamIndependentByteRangeLocks` first hits `GAP-18`
(`memfs_helper::RemoveStreamType` at `memfs_helper.h:73-77` strips `:$DATA`
from `::$DATA`, leaving a trailing `:` that fails `CreateFileW` with
`ERROR_FILE_NOT_FOUND` before `LockFileEx`), with `GAP-07` as the secondary
blocker.

**Covered Tests:**

- `LocksOplocksTest.ExclusiveLockBlocksOverlappingWriteFromOtherHandle`
  (`memfs`, `mirror`)
- `LocksOplocksTest.SharedLocksBlockWritesOnAllHandles` (`memfs`, `mirror`)
- `StreamsTest.StreamIndependentByteRangeLocks` (`memfs`, `mirror`)

### GAP-08: Mapped-View Coherence and No-Buffering Alignment

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`, `mirror_unc`

**NTFS Specification (Microsoft proof):**
Per [Reading and Writing From a File View][win32-file-view], regular
`WriteFile` calls and active `MapViewOfFile` views of the same file are
coherent through the Windows Cache Manager. Per
[File Buffering][win32-file-buffering], `FILE_FLAG_NO_BUFFERING` requires
sector-aligned offsets and transfer sizes (rejecting unaligned offsets or
lengths with `ERROR_INVALID_PARAMETER`) and memory buffer addresses satisfying
the volume's `FileAlignmentInformation` (`DeviceObject->AlignmentRequirement`)
mask.

**Observed Dokan Behavior:**
`DokanDispatchWrite` (`sys/write.c`) invokes `CcFlushCache` and
`CcPurgeCacheSection`—which cannot purge pages actively mapped into a user
address space—and forwards writes directly to user mode rather than updating
the Cache Manager via `CcCopyWrite`, leaving active mapped views stale. For
`FILE_FLAG_NO_BUFFERING`, Dokan reports `FILE_BYTE_ALIGNMENT` (`0`), which is
valid for a software virtual device (the suite's
`NoBufferingBufferAddressHonorsAlignmentRequirement` test gates buffer-address
rejection on `FileAlignmentInformation`); however, two real Dokan defects
remain: (1) on `mirror_unc`, MUP/`dokan3.sys` accepts unaligned
`SetFilePointerEx` offsets instead of returning `ERROR_INVALID_PARAMETER`, and
(2) `DispatchWrite` (`dokan/write.c:109-113`) forwards
`(PCHAR)writeIoBatch->EventContext + BufferOffset` (whose address alignment
depends on `FileName.Length`) straight to `MirrorWriteFile`'s
`FILE_FLAG_NO_BUFFERING` backing handle, which fails on backing volumes whose
storage stack enforces stricter buffer-address alignment.

**Covered Tests:**

- `DataIoTest.MemoryMappedWriteFileVisibleInView` (`memfs`, `mirror`)
- `DataIoTest.NoBufferingAlignedRoundTrip` (`mirror`, `allow_xpass`)
- `DataIoTest.NoBufferingBufferAddressHonorsAlignmentRequirement`
  (`mirror`, `allow_xpass`)
- `DataIoTest.NoBufferingUnalignedSeekOffsetRejected` (`mirror_unc`)

### GAP-09: `ChangeTime` Aliasing, `AllocationSize`, and Dir Sizing

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.1.1–2.1.5.1.2 (`AllocationSize`)][ms-fsa-open-existing],
[MS-FSA 2.1.5.15.1 (`FileAllocationInformation`)][ms-fsa-alloc],
[MS-FSA 2.1.5.15.2 (`FileBasicInformation`)][ms-fsa-basic],
[MS-FSA 2.1.5.15.5 (`FileEndOfFileInformation`)][ms-fsa-eof], and
[`FILE_VALID_DATA_LENGTH_INFORMATION`][wdk-vdl-info], `ChangeTime` is stored
independently from `LastWriteTime` (updating on metadata and rename operations
without altering `LastWriteTime`), `AllocationSize` honors create-time
preallocation and persists independently above `EndOfFile`, setting
`FileValidDataLengthInformation` requires `SeManageVolumePrivilege` and
rejects values `< current VDL` or `> EndOfFile` with
`STATUS_INVALID_PARAMETER`, and setting EOF or allocation size on a directory
handle fails with `STATUS_INVALID_PARAMETER`.

**Observed Dokan Behavior:**
The `DOKAN_OPERATIONS` ABI (`dokan/dokan.h`) uses Win32
`BY_HANDLE_FILE_INFORMATION` and three-timestamp `SetFileTime`, which carry
neither `ChangeTime` nor independent `AllocationSize`, and `CREATE_CONTEXT`
(`sys/public.h:207-216`) / `DOKAN_OPERATIONS::ZwCreateFile` do not forward
create-time `Irp->Overlay.AllocationSize`. Consequently,
`DokanFillFileBasicInfo` (`dokan/fileinfo.c:46-47`) and
`DokanFillNetworkOpenInfo` copy `ftLastWriteTime` into `ChangeTime` (note that
this aliasing also makes any assertion checking only that `ChangeTime` equals
`LastWriteTime`—or advances alongside `LastWriteTime`—vacuous on `mirror` and
`memfs`), `DokanSetBasicInformation` (`dokan/setfile.c`) drops `ChangeTime`,
and `DokanFillFileStandardInfo` synthesizes `AllocationSize` by rounding
`EndOfFile` up via `ALIGN_ALLOCATION_SIZE`. Under
`FileValidDataLengthInformation`,
`DokanInitializeFcb` (`sys/util/fcb.c:54-57`) initializes `ValidDataLength`
to `MAXLONGLONG`, `sys/fileinfo.c` performs no `SeManageVolumePrivilege` or
bounds validation, and `DokanSetValidDataLengthInformation`
(`dokan/setfile.c:232-253`) forwards the request directly to
`DokanOperations->SetEndOfFile`. In addition, `MirrorSetAllocationSize` (`samples/dokan_mirror/mirror.c`)
ignores `AllocSize > fileSize`, and `DokanDispatchRead` (`sys/read.c:146`) /
`DokanDispatchWrite` (`sys/write.c:75`) return `STATUS_INVALID_PARAMETER`
instead of `STATUS_INVALID_DEVICE_REQUEST` (`ERROR_INVALID_FUNCTION`) on
directory handles.

**Covered Tests:**

- `CreateOpenTest.NtCreateFileInitialAllocationSize` (`memfs`, `mirror`)
- `DataIoTest.FileAllocationInfoPreallocateAndTruncate` (`memfs`, `mirror`)
- `DataIoTest.FileValidDataLengthExtend` (`memfs`, `mirror`)
- `DataIoTest.ReadWriteOnDirectoryHandleFailsInvalidFunction`
  (`memfs`, `mirror`)
- `RenameMoveTest.RenameUpdatesFileChangeTime`
  (`memfs`, `mirror` without `mirror_unc`)
- `TimestampsAttrsTest.ExplicitChangeTimeRoundTripsIndependently`
  (`memfs`, `mirror`)
- `TimestampsAttrsTest.MetadataChangeUpdatesChangeTimeNotLastWriteTime`
  (`memfs`, `mirror`)
- `TimestampsAttrsTest.RenameUpdatesChangeTimeNotLastWriteTime`
  (`memfs`, `mirror` without `mirror_unc`)
- `TimestampsAttrsTest.SecurityChangeUpdatesChangeTimeNotLastWriteTime`
  (`mirror`)

### GAP-10: `memfs` Timestamp Sentinels, Updates, and Archive Bit

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.15.2 (`FileBasicInformation`)][ms-fsa-basic],
[MS-FSA 2.1.5.15.5 (`FileEndOfFileInformation`)][ms-fsa-eof],
[MS-FSCC 2.6 (`File Attributes`)][ms-fscc-attrs], and Windows NTFS metadata
tunneling, writes, `SetEndOfFile`, and `TRUNCATE_EXISTING` advance
`LastWriteTime` and `ChangeTime` and set `FILE_ATTRIBUTE_ARCHIVE`; creating or
deleting a child advances the parent directory's `LastWriteTime`; explicit
timestamp sets freeze subsequent auto-updates on that handle; sentinel `-1`
freezes updates; sentinel `-2` (Windows 10 1809+) resumes auto-updates;
negative `FILETIME` values below `-2` fail with `STATUS_INVALID_PARAMETER`;
and 15-second metadata tunneling preserves `CreationTime` across rapid
delete/rename-and-recreate cycles.

**Observed Dokan Behavior:**
In `samples/dokan_memfs/memfs_operations.cpp`, `memfs_createfile` (`:156-162`)
sets `FILE_ATTRIBUTE_ARCHIVE` on create/truncate, but `memfs_writefile`
(`:327-374`), `memfs_setendoffile` (`:561-571`), and `memfs_setallocationsize`
(`:573-583`) never update `times.lastwrite` or re-set `FILE_ATTRIBUTE_ARCHIVE`
(breaking both `SetEndOfFile` and Win32 `TRUNCATE_EXISTING`, which truncates
via `SetAllocationSize`/`SetEndOfFile`). `memfs_setfiletime` (`:488-504`) stores any non-zero `FILETIME` verbatim
without tracking per-handle freeze/unfreeze state for explicit sets, `-1`, or
`-2`; and `memfs_readfile` never advances `times.lastaccess` when the host NTFS
`NtfsDisableLastAccessUpdate` registry policy enables last-access updates
(note: that policy key is `ntfs.sys`-private). In
`samples/dokan_memfs/filenodes.cpp`, `fs_filenodes::add`,
`fs_filenodes::remove`, and `fs_filenodes::move` never update parent directory
`filenode` `times.lastwrite`, and `memfs` implements no 15-second metadata
tunneling cache (`FsRtlAddToTunnelCache` / `FsRtlFindInTunnelCache`).

**Covered Tests:**

<!-- markdownlint-disable MD013 -->

- `NamingPathsTest.MetadataTunnelingPreservesCreationTime` (`memfs`)
- `RenameMoveTest.RenameUpdatesBothParentDirLastWriteTimes` (`memfs`)
- `TimestampsAttrsTest.ArchiveBitSetOnCreateAndWriteAfterClear` (`memfs`)
- `TimestampsAttrsTest.HandleExplicitSetFileTimeFreezesSubsequentAutoUpdateOnSameHandle`
  (`memfs`)
- `TimestampsAttrsTest.ParentDirLastWriteTimeOnChildCreateDeleteNotWrite`
  (`memfs`)
- `TimestampsAttrsTest.ReadAdvancesLastAccessTimeWhenUpdatesEnabled` (`memfs`)
- `TimestampsAttrsTest.SentinelMinusOneFreezesTimestampUpdates` (`memfs`)
- `TimestampsAttrsTest.SentinelMinusTwoRestoresTimestampUpdates` (`memfs`)
- `TimestampsAttrsTest.SetEndOfFileAndTruncateUpdateLastWriteAndArchive`
  (`memfs`)
- `TimestampsAttrsTest.WriteUpdatesLastWriteTimeAndChangeTime` (`memfs`)

<!-- markdownlint-enable MD013 -->

### GAP-11: Oplock Breaks, Batch Ordering, and Complete-If-Oplocked

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.1.2 (Open of an Existing File)][ms-fsa-open-existing],
[MS-FSA 2.1.5.18 (Server Requests an Oplock)][ms-fsa-oplock], and
[`ZwCreateFile`][wdk-zwcreatefile], creating a child entry inside a directory
holding a Read-Handle (`RH`) oplock must break the parent directory's oplock,
conflicting opens (including `ShareMode = 0`) must break an existing Batch
oplock *before* share-access validation so the holder can acknowledge with
`FSCTL_OPBATCH_ACK_CLOSE_PENDING` and close cleanly, and opens specifying
`FILE_COMPLETE_IF_OPLOCKED` against a conflicting oplock must initiate an
oplock break and return `STATUS_OPLOCK_BREAK_IN_PROGRESS`.

**Observed Dokan Behavior:**
`DokanDispatchCreate` (`sys/create.c`) checks oplocks only on the target
file's own FCB and never looks up or breaks an RH oplock held on the parent
directory FCB when creating a child file. For regular opens,
`DokanDispatchCreate` evaluates `DokanCheckShareAccess` (`IoCheckShareAccess`)
*before* calling `FsRtlCheckOplock`; on `STATUS_SHARING_VIOLATION`, it calls
only `FsRtlOplockBreakH` (`CACHE_HANDLE_LEVEL` oplocks) and never breaks a
legacy Batch oplock (`FSCTL_REQUEST_BATCH_OPLOCK`), so a conflicting
`ShareMode = 0` open fails immediately without signaling an oplock break. On
non-conflicting `FILE_COMPLETE_IF_OPLOCKED` opens (where both handles pass
`kShareAll`), `FsRtlCheckOplock` (`sys/create.c:1162`) *does* initiate the
break and returns `STATUS_OPLOCK_BREAK_IN_PROGRESS`, but `DokanDispatchCreate`
immediately overwrites `status` via
`FsRtlCheckOplockEx(..., OPLOCK_FLAG_OPLOCK_KEY_CHECK_ONLY, ...)`
(`sys/create.c:1185-1187`), and `DokanCompleteCreate` (`sys/create.c:1313`)
completes the IRP with user-mode `EventInfo->Status` (`STATUS_SUCCESS`).
(Separately, when a sharing conflict *does* occur alongside
`FILE_COMPLETE_IF_OPLOCKED`, `sys/create.c:1045-1139` bypasses
`FsRtlOplockBreakH` and fails with `STATUS_SHARING_VIOLATION`.)

**Covered Tests:**

- `LocksOplocksTest.DirectoryOplockBreaksOnChildCreate` (`memfs`, `mirror`)
- `LocksOplocksTest.OplockCompleteIfOplockedFlag` (`memfs`, `mirror`)
- `LocksOplocksTest.OplockOpbatchAckClosePendingReleasesOpener`
  (`memfs`, `mirror`)

### GAP-12: Directory Enumeration Classes, Wildcards, and Ex Flags

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.6 (Directory Query)][ms-fsa-events],
[MS-FSA 2.1.4.4 (Wildcard Expression Matching)][ms-fsa-wildcard],
[MS-FSCC 2.4 (`File Information Classes`)][ms-fscc-fileinfo], and
[`NtQueryDirectoryFileEx`][wdk-querydir-ex], directory enumeration must reject
regular-file handles with `STATUS_INVALID_PARAMETER`, populate real 64-bit and
128-bit `FileId` values across directory ID info classes, match `?`, `DOS_QM`
(`'>'`), and `DOS_DOT` (`'"'`) wildcards per NTFS rules without reading past
the string terminator, and honor `SL_RETURN_SINGLE_ENTRY` (`0x02`) without
advancing the scan cursor when `SL_NO_CURSOR_UPDATE` (`0x10`) is set.

**Observed Dokan Behavior:**
In `dokan/directory.c`, `DokanWriteDirectoryEntry` (`:107-159`) leaves `FileId`
zero-initialized for `FileIdFullDirectoryInformation`,
`FileIdBothDirectoryInformation`, `FileIdExtdDirectoryInformation`, and
`FileIdExtdBothDirectoryInformation`, causing both
`NtQueryDirectoryFileIdBothDirectoryInfoReportsFileIds` and
`NtQueryDirectoryIdFullAndIdExtdInfoClasses` to fail `FileId` verification on
`memfs` and `mirror`. Separately, `DokanQueryDirectory` and
`DokanCompleteDirectoryControl` (`sys/directory.c:121-153, 316`) ignore
`SL_NO_CURSOR_UPDATE` (`0x10`) alongside `SL_RETURN_SINGLE_ENTRY` (`0x02`)
(unconditionally advancing `ccb->Context`) and ignore non-initial
`QueryDirectory.FileName` patterns whenever `initial == FALSE`.

**Covered Tests:**

- `DirEnumTest.NtQueryDirectoryFileExQuerySingleEntryNoCursorUpdate`
  (`memfs`, `mirror`)
- `DirEnumTest.NtQueryDirectoryFileIdBothDirectoryInfoReportsFileIds`
  (`memfs`, `mirror`)
- `DirEnumTest.NtQueryDirectoryIdFullAndIdExtdInfoClasses` (`memfs`, `mirror`)

### GAP-13: File Info Queries, Change Notify, and Mount Manager

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`, `mirror_unc`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.12.3 (`FileAllInformation`)][ms-fsa-allinfo],
[MS-FSA 2.1.5.12.30 (`FileNormalizedNameInformation`)][ms-fsa-normname],
[MS-FSCC 2.7 (`Directory Change Notifications`)][ms-fscc-notify],
[`ReadDirectoryChangesExW`][win32-rdc-ex], and
[`GetFinalPathNameByHandleW`][win32-finalpath], `FileAllInformation` populates
granted access and file mode, `FileNormalizedNameInformation` returns
canonical on-disk casing, file-size growth emits `FILE_NOTIFY_CHANGE_SIZE`,
`ReadDirectoryChangesExW` delivers extended records, and volume-registered
mounts resolve DOS and volume-GUID paths (including on alternate data stream
handles).

**Observed Dokan Behavior:**
`DokanCompleteQueryInformation` (`sys/fileinfo.c:256-288`) runs `RtlZeroMemory`
and overwrites the entire `FILE_ALL_INFORMATION` buffer from user mode,
refilling only `Position`, `DeletePending`, and `Name`—wiping the
I/O-manager-prefilled `AccessInformation` and `ModeInformation` fields (unlike
FastFAT/NTFS, which skip those sub-structs). `DokanDispatchQueryInformation`
(`sys/fileinfo.c`) routes `FileNormalizedNameInformation` to
`FillNameInformation`, returning `Fcb->FileName` in caller casing rather than
canonical on-disk casing. `dokan3.sys` does not complete pending
`ReadDirectoryChangesW` watches when the watched directory becomes
delete-pending, `DokanInitializeFcb` (`sys/util/fcb.c:62`) initializes
`AdvancedFCBHeader.FileSize` to `4096` so `DokanCompleteWrite`
(`sys/write.c:352`) suppresses `FILE_NOTIFY_CHANGE_SIZE` on sub-4 KiB file
growth unless a host minifilter first updates `FileSize` via
`FileStandardInformation` (`sys/fileinfo.c:311`),
`DokanCompleteSetInformation` (`sys/fileinfo.c:919-925`) unconditionally fires
all four basic-info notify filters
(`ATTRIBUTES | LAST_WRITE | LAST_ACCESS | CREATION`) on every
`FileBasicInformation` set, `DokanCompleteWrite` (`sys/write.c:372`) sets
`DOKAN_FILE_CHANGE_LAST_WRITE` without `DokanDispatchCleanup`
(`sys/cleanup.c:117-120`) ever clearing it (emitting spurious
`FILE_ACTION_MODIFIED` notifications on subsequent read-only closes while the
FCB stays live), and `DokanDispatchDirectoryControl` (`sys/directory.c`)
does not handle `IRP_MN_NOTIFY_CHANGE_DIRECTORY_EX`, failing
`ReadDirectoryChangesExW` with `STATUS_INVALID_PARAMETER`. Finally,
non-Mount-Manager mounts (`/o` omitted) do not register with Mount Manager,
failing `GetFinalPathNameByHandleW(VOLUME_NAME_DOS)` on both base files and
alternate data streams (passing on `mirror_mntmgr` and
`mirror_removable_mntmgr`).

**Covered Tests:**

<!-- markdownlint-disable MD013 -->

- `InfoQueriesTest.GetFinalPathNameByHandleWFormats` (`mirror` without `/o`)
- `InfoQueriesTest.NtQueryFileInternalAndAllInfo` (`memfs`, `mirror`)
- `InfoQueriesTest.NtQueryFileNameAndNormalizedNameInfo`
  (`memfs`, `mirror` without `mirror_unc`; `mirror_unc`, `os_min_build: 17763`)
- `InfoQueriesTest.ReadDirectoryChangesBasicInfoFilterIsolation`
  (`memfs`, `mirror`)
- `InfoQueriesTest.ReadDirectoryChangesExExtendedInfo` (`memfs`, `mirror`)
- `InfoQueriesTest.ReadDirectoryChangesReadOnlyCloseAfterWriteNoSpuriousNotify`
  (`memfs`, `mirror`)
- `InfoQueriesTest.ReadDirectoryChangesSizeFilter`
  (`memfs`, `mirror`, `allow_xpass`)
- `InfoQueriesTest.ReadDirectoryChangesWatchedDirectoryDeletePendingCompletesWatch`
  (`memfs`, `mirror`)
- `StreamsTest.StreamWriteUpdatesBaseMetadataAndQueriesStreamName`
  (`mirror` without `/o`)

<!-- markdownlint-enable MD013 -->

### GAP-14: Trailing Backslash and `mirror` Non-Verbatim Paths

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [Naming Files, Paths, and Namespaces][win32-naming] and
[MS-FSA 2.1.5.1 (Open of a File)][ms-fsa-events], opening a regular file with
a trailing backslash fails (`STATUS_OBJECT_NAME_INVALID` /
`STATUS_NOT_A_DIRECTORY`), and `\\?\` verbatim paths bypass Win32 path
normalization—preserving trailing dots and spaces (`...`, `foo.`, `"foo "`),
allowing literal filenames matching DOS device names (`NUL`, `NUL.txt`), and
supporting paths beyond `MAX_PATH` (260 characters) with components up to 255
characters.

**Observed Dokan Behavior:**
`DokanDispatchCreate` (`sys/create.c`) unconditionally strips a trailing `\`
from `fileObject->FileName` before checking whether the object is a
directory, allowing regular files to be opened with a trailing backslash on
both `memfs` and `mirror`. In `samples/dokan_mirror/mirror.c`, although
`WIN10_ENABLE_LONG_PATH` defines `DOKAN_MAX_PATH = 32768` (`:39-45`),
`GetFilePath` concatenates a plain non-verbatim `gRootDirectory`
(`C:\FSTMP\...`, without a `\\?\` prefix) and passes it to Win32 `CreateFile`
/ `GetFileAttributes` / `SetFileInformationByHandle`, causing Win32 to strip
trailing dots/spaces, redirect DOS device names (including `<device>.<ext>` on
Windows builds `<= 21999`, i.e., `< 22000`), and reject paths exceeding
`MAX_PATH` (including 255-character components or stream names prefixed by
`gRootDirectory`).

**Covered Tests:**

- `CreateOpenTest.TrailingBackslashOnFileFailsNotADirectory` (`memfs`, `mirror`)
- `NamingPathsTest.ExtendedLengthLongPathsOverMaxPath` (`mirror`)
- `NamingPathsTest.LeadingSpacesLeadingDotsAndTripleDotNames` (`mirror`)
- `NamingPathsTest.MaxComponentLength255And256Rejected` (`mirror`)
- `NamingPathsTest.TrailingBackslashOnRegularFileFails` (`memfs`, `mirror`)
- `NamingPathsTest.TrailingDotsAndSpacesWin32VsVerbatim` (`mirror`)
- `NamingPathsTest.VerbatimDosDeviceNamesWithExtensionsAreLiteralFiles`
  (`mirror`, `os_max_build: 21999`)
- `NamingPathsTest.VerbatimReservedDosDeviceNamesAreLiteralFiles`
  (`mirror`, `skip_crash`)
- `NamingPathsTest.Win32CreateDirectoryConRejectedBefore24H2` (`mirror`)
- `NamingPathsTest.Win32DosDeviceNameWithExtensionRedirectedBefore24H2`
  (`mirror`)
- `NamingPathsTest.Win32NulDeviceNameNeverReachesFilesystem` (`mirror`)
- `StreamsTest.StreamNameLengthLimit255` (`mirror`)

### GAP-15: `memfs` Case Sensitivity, Unicode Folding, and Path Checks

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`

**NTFS Specification (Microsoft proof):**
Per [Naming Files, Paths, and Namespaces][win32-naming],
[MS-FSA 2.1.4.4 (Wildcard Expression Matching)][ms-fsa-wildcard], and
[MS-FSA 2.1.5.1 (Open of a File)][ms-fsa-events], standard NTFS volumes are
case-preserving and case-insensitive (both for path lookups using the volume
Unicode `$UpCase` table and for directory wildcard queries), reject invalid
filename characters (`< > : " / \ | ? *` and control characters `0x00`–`0x1F`),
and return `STATUS_OBJECT_PATH_NOT_FOUND` (`ERROR_PATH_NOT_FOUND`) when an
intermediate path component is a regular file.

**Observed Dokan Behavior:**
`memfs::start` (`samples/dokan_memfs/memfs.cpp:39-40`) unconditionally sets
`DOKAN_OPTION_CASE_SENSITIVE`, which causes `FilterDirectoryList`
(`dokan/directory.c`) to invoke
`DokanIsNameInExpression(..., IgnoreCase=FALSE)` during directory wildcard
enumeration. Likewise, `fs_filenodes` (`samples/dokan_memfs/filenodes.h`)
indexes nodes in an exact-match `std::unordered_map<std::wstring, ...>`
(`_filenodes`), breaking both ASCII and Unicode `$UpCase` case-insensitive
lookups. `memfs_createfile` (`samples/dokan_memfs/memfs_operations.cpp`)
performs no invalid-character validation, and
`fs_filenodes::find` (`samples/dokan_memfs/filenodes.cpp:140-144`) returns
`std::shared_ptr<filenode>` (`nullptr`) rather than an `NTSTATUS`;
`memfs_createfile` and `memfs_getfileInformation` translate `nullptr` into
`STATUS_OBJECT_NAME_NOT_FOUND` (`ERROR_FILE_NOT_FOUND`) without checking
whether an ancestor path component is missing or is a regular file (including
source opens during `MoveFileExW`).

**Covered Tests:**

- `DirEnumTest.NtQueryDirectoryCaseInsensitiveWildcards` (`memfs`)
- `NamingPathsTest.CaseInsensitiveLookupAndCasePreservation` (`memfs`)
- `NamingPathsTest.InvalidFilenameCharactersRejected` (`memfs`)
- `NamingPathsTest.ParentIsFilePathErrorsAllApis` (`memfs`)
- `NamingPathsTest.UnicodeCaseFoldingInsensitive` (`memfs`)
- `RenameMoveTest.RenameMissingSourceFailsNotFound` (`memfs`)

### GAP-16: `memfs` Non-Empty Dir Delete and Rename Validation

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.5 (Close)][ms-fsa-events],
[MS-FSA 2.1.5.15.3 (`FileDispositionInformation`)][ms-fsa-disp], and
[MS-FSA 2.1.5.15.12 (`FileRenameInformation`)][ms-fsa-rename], deleting a
non-empty directory via `FILE_FLAG_DELETE_ON_CLOSE` or `NtDeleteFile` must
fail and preserve its contents, renaming a path to itself is a no-op,
renaming a delete-pending file or a directory with open descendants must
fail, non-POSIX replace cannot overwrite an open target, and replacing a
writable file with a directory follows NTFS replace rules.

**Observed Dokan Behavior:**
In `samples/dokan_memfs/filenodes.cpp`, `fs_filenodes::remove` recursively
deletes all children whenever `memfs_cleanup` runs with `DeletePending` set
(bypassing `memfs_deletedirectory` emptiness checks on
`FILE_FLAG_DELETE_ON_CLOSE` and `NtDeleteFile`). In `fs_filenodes::move`,
self-renames hit `!replace_if_existing && new_f` and return
`STATUS_OBJECT_NAME_COLLISION`, the `f->is_directory || new_f->is_directory`
guard unconditionally returns `STATUS_ACCESS_DENIED`, and neither
delete-pending state nor open handles on the target or descendants are
validated.

**Covered Tests:**

- `DeleteTest.DeleteOnCloseNonEmptyDirectorySurvivesClose` (`memfs`)
- `DeleteTest.NtDeleteFileLeavesNonEmptyDirectoryIntact` (`memfs`)
- `RenameMoveTest.FileRenameInfoExNonPosixBasicAndReplace` (`memfs`)
- `RenameMoveTest.FileRenameInfoReplaceOpenTargetNonPosixFails` (`memfs`)
- `RenameMoveTest.RenameDeletePendingFileFails` (`memfs`)
- `RenameMoveTest.RenameDirectoryOverExistingFileSemantics` (`memfs`)
- `RenameMoveTest.RenameDirectoryWithOpenDescendantFails` (`memfs`)
- `RenameMoveTest.RenameOntoDeletePendingTargetFailsAccessDenied` (`memfs`)
- `RenameMoveTest.RenameSameNameNoOp` (`memfs`)

### GAP-17: ADS Enumeration Status and `mirror` Stream Rename

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.12.29 (`FileStreamInformation`)][ms-fsa-streams],
[MS-FSA 2.1.5.15.12 (`FileRenameInformation`)][ms-fsa-rename], and
[File Streams][win32-file-streams], querying `FileStreamInformation` on a
directory with no named data streams returns `STATUS_SUCCESS` with zero bytes
written, opening `dir::$DATA` fails with `STATUS_FILE_IS_A_DIRECTORY`, and
stream renames accept `:newname:$DATA` targets on the stream handle.

**Observed Dokan Behavior:**
For a directory with no named streams, `memfs_findstreams`
(`samples/dokan_memfs/memfs_operations.cpp:764-766`) explicitly returns
`STATUS_END_OF_FILE`, and `MirrorFindStreams`
(`samples/dokan_mirror/mirror.c:1435-1441`) converts `FindFirstStreamW`'s
`ERROR_HANDLE_EOF` to `STATUS_END_OF_FILE` (the error originates in the
user-mode filesystem callbacks, not `dokan3.sys`). For stream renames,
`PopulateRenameEventInformations` (`sys/fileinfo.c`) prepends the base file
path to `:newstream:$DATA`, so `NewFileName[0]` is always `L'\\'` when
reaching `MirrorMoveFile` (`samples/dokan_mirror/mirror.c`); its
`NewFileName[0] != ':'` branch prepends `gRootDirectory` and passes a full
path to `SetFileInformationByHandle(FileRenameInfo)`, which NTFS rejects. For
`dir::$DATA`, `MirrorCreateFile` loses `STATUS_FILE_IS_A_DIRECTORY` through
Win32 `ERROR_ACCESS_DENIED` -> `STATUS_ACCESS_DENIED`. Similarly, because
`dokan3.sys` keys FCBs by full stream path (`sys/create.c:686`), closing a
delete-pending base file while a named stream handle stays open drops the base
FCB while backing NTFS keeps the file delete-pending; subsequent opens reach
`MirrorCreateFile` (`mirror.c:453, 468`), where Win32 `CreateFileW` collapses
`STATUS_DELETE_PENDING` into `ERROR_ACCESS_DENIED` -> `STATUS_ACCESS_DENIED`.

**Covered Tests:**

- `StreamsTest.BaseFileLegacyDeletePendingWhileNamedStreamOpen` (`mirror`)
- `StreamsTest.DefaultStreamRenameToNamedStreamMovesData` (`mirror`)
- `StreamsTest.InvalidStreamSyntaxAndDirectoryDataStreamRejected` (`mirror`)
- `StreamsTest.NamedStreamRenameOntoEmptyDefaultStream` (`mirror`)
- `StreamsTest.NtQueryFileStreamInformationSmallBufferAndDir`
  (`memfs`, `mirror`)
- `StreamsTest.StreamCaseOnlyRenameAndSelfRename` (`mirror`)
- `StreamsTest.StreamRenameReplaceAndNoReplace` (`mirror`)

### GAP-18: `memfs` Alternate Data Stream Semantics

- **Severity:** Semantic Bug
- **Affected Targets:** `memfs`

**NTFS Specification (Microsoft proof):**
Per [File Streams][win32-file-streams],
[MS-FSA 2.1.5.1 (Open of a File)][ms-fsa-events],
[MS-FSA 2.1.5.12.29 (`FileStreamInformation`)][ms-fsa-streams], and
[MS-FSA 2.1.5.15.12 (`FileRenameInformation`)][ms-fsa-rename], alternate data
streams (ADS) belong to a single MFT file record: they share file attributes,
timestamps, and `FileIndex` with the base file (setting `FILE_ATTRIBUTE_ARCHIVE`
on stream writes), move together with the base file across renames, maintain
independent stream sizes and per-stream sharing modes, use case-insensitive
stream names, reject internal attribute types (such as `:$EA`) and `dir::$DATA`,
recognize `::$DATA` and directory `::$INDEX_ALLOCATION` /
`:$I30:$INDEX_ALLOCATION` aliases, reject non-stream rename targets (missing
leading `:`) and replacing non-empty target streams with
`STATUS_INVALID_PARAMETER`, block stream mutation on read-only files, and keep
the default stream accessible when an ADS is unlinked.

**Observed Dokan Behavior:**
`memfs` models each alternate stream as a separate case-sensitive `filenode`
object (`samples/dokan_memfs/filenode.h`, `memfs_helper.h`,
`memfs_operations.cpp`). `memfs_helper::RemoveStreamType` strips only trailing
`:$DATA` without validating stream syntax, internal types, directory aliases,
or explicit `::$DATA` deletes. Separate `filenode` instances keep divergent
timestamps and attributes, omit `FILE_ATTRIBUTE_ARCHIVE` updates on stream
writes and read-only checks on stream operations, fail `CopyFileW` /
`CopyFileExW` / `BackupRead` stream preservation, fail stream-path
`FindFirstStreamW` queries (`memfs_findstreams` at
`samples/dokan_memfs/memfs_operations.cpp:742-772` works on base files, but
looks up the stream node rather than its base file when queried via `f:bar`),
and fail independent per-stream sharing or default-stream access when deleting
an open ADS. In
`samples/dokan_memfs/filenodes.cpp`, `fs_filenodes::move` moves only the base
`filenode` without relocating its attached streams in `_filenodes`,
unconditionally replaces non-empty target streams when `ReplaceIfExists=TRUE`,
duplicates entries on case-only stream renames, and—via `memfs_movefile`
(`memfs_operations.cpp:538-559`)—accepts renaming a stream to a non-stream
path without a leading `:`, leaving the moved `filenode` with
`main_stream != nullptr` inside `_directoryPaths` (invisible to
`memfs_findfiles:431` and missed by `fs_filenodes::remove` on the base file),
which poisons temp-directory teardown with `ERROR_DIR_NOT_EMPTY` (`skip_crash`
on `StreamsTest.StreamRenameToNonStreamTargetFailsInvalidParameter`,
`StreamsTest.DefaultStreamRenameToNamedStreamMovesData`, and
`StreamsTest.NamedStreamRenameOntoEmptyDefaultStream`). Furthermore, because
`dokan3.sys` keys FCBs by full stream path (`sys/create.c:686`), a base file
and its named streams have independent `UncleanCount` and
`DOKAN_FCB_STATE_DELETE_PENDING` state, so closing the base handle deletes the
file on `memfs` even while a named stream handle remains open. `memfs` also
accepts stream names exceeding 255 characters without returning
`ERROR_INVALID_NAME`.

**Covered Tests:**

- `DataIoTest.CopyFileExPreservesAlternateStreams` (`memfs`)
- `RenameMoveTest.RenamePreservesAlternateDataStreams` (`memfs`)
- `StreamsTest.BackupReadAndBackupWritePreservesStreams` (`memfs`)
- `StreamsTest.BaseAttributeChangeVisibleThroughStream` (`memfs`)
- `StreamsTest.BaseFileLegacyDeletePendingWhileNamedStreamOpen` (`memfs`)
- `StreamsTest.BaseTimestampChangeVisibleThroughStream` (`memfs`)
- `StreamsTest.CopyFileWPreservesAlternateStreams` (`memfs`)
- `StreamsTest.DefaultDataStreamAliasCreateNewFailsFileExists` (`memfs`)
- `StreamsTest.DefaultStreamRenameToNamedStreamMovesData`
  (`memfs`, `skip_crash`)
- `StreamsTest.DeleteDefaultStreamByExplicitName` (`memfs`)
- `StreamsTest.DirectoryDefaultIndexAllocationAliases` (`memfs`)
- `StreamsTest.FileStreamInternalAttributeTypesRejected` (`memfs`)
- `StreamsTest.FindStreamsOnStreamPathEnumeratesAllStreams` (`memfs`)
- `StreamsTest.InvalidStreamSyntaxAndDirectoryDataStreamRejected` (`memfs`)
- `StreamsTest.NamedStreamRenameOntoEmptyDefaultStream` (`memfs`, `skip_crash`)
- `StreamsTest.PerStreamDataAndSizeIndependentOfDefaultStream` (`memfs`)
- `StreamsTest.ReadOnlyFileBlocksStreamCreateModifyAndDelete` (`memfs`)
- `StreamsTest.StreamCaseInsensitiveLookupAndPreservation` (`memfs`)
- `StreamsTest.StreamCaseOnlyRenameAndSelfRename` (`memfs`)
- `StreamsTest.StreamDeleteWhileOpenLegacyDeletePending` (`memfs`)
- `StreamsTest.StreamNameLengthLimit255` (`memfs`)
- `StreamsTest.StreamRenameReplaceNonEmptyTargetFailsInvalidParameter` (`memfs`)
- `StreamsTest.StreamRenameToNonStreamTargetFailsInvalidParameter`
  (`memfs`, `skip_crash`)
- `StreamsTest.StreamSharingModeIndependentPerStream` (`memfs`)
- `StreamsTest.StreamWriteUpdatesBaseMetadataAndQueriesStreamName` (`memfs`)

### GAP-19: `mirror` Privileges, Impersonation, and Owner Rights

- **Severity:** Semantic Bug
- **Affected Targets:** `mirror`

**NTFS Specification (Microsoft proof):**
Per [File Security and Access Rights][win32-file-security],
[Privilege Constants][win32-privileges], and
[MS-FSA 2.1.5.1.2 (Open of an Existing File)][ms-fsa-open-existing], the file
owner implicitly holds `READ_CONTROL` and `WRITE_DAC` even under an empty
`D:P` or deny-read DACL, `SeBackupPrivilege` / `SeRestorePrivilege` /
`SeTakeOwnershipPrivilege` bypass matching DACL restrictions when requested,
restricted tokens without `SeChangeNotifyPrivilege` enforce `FILE_TRAVERSE`,
and an execute-only DACL permits `SEC_IMAGE` mapping while denying
`FILE_READ_DATA`.

**Observed Dokan Behavior:**
In `samples/dokan_mirror/mirror.c`, `MirrorCreateFile` forwards opens through
Win32 `CreateFile`, which implicitly adds `FILE_READ_ATTRIBUTES | SYNCHRONIZE`
and causes owner-implicit `READ_CONTROL`/`WRITE_DAC` opens on empty or
deny-read DACLs to fail with `ERROR_ACCESS_DENIED`. `wmain` enables only
`SE_SECURITY_NAME` (`AddSeSecurityNamePrivilege`), never propagating caller
`SeBackupPrivilege`, `SeRestorePrivilege`, or `SeTakeOwnershipPrivilege`. By
default (`samples/mirror_test.ps1`), `mirror.exe` also runs without `/p`
caller impersonation (`mirror.c:1522, 1632-1634`), bypassing restricted-token
`FILE_TRAVERSE` and low-integrity `NO_WRITE_UP` checks. On execute-only files, `MirrorReadFile` fails backing reads (or reopens with
`GENERIC_READ`), causing `NtCreateSection(SEC_IMAGE)` to fail with
`STATUS_INVALID_FILE_FOR_SECTION`.

**Covered Tests:**

- `CreateOpenTest.BackupPrivilegeBypassesDenyDacl` (`mirror`)
- `CreateOpenTest.RestorePrivilegeBypassesWriteDenyDacl` (`mirror`)
- `SecurityAclTest.DirectoryTraverseAccessEnforcement` (`mirror`)
- `SecurityAclTest.FileExecuteOnlyAllowsImageMappingDeniesReadData` (`mirror`)
- `SecurityAclTest.LowIntegrityCallerDeniedWriteUpToUnlabeledFile` (`mirror`)
- `SecurityAclTest.NullDaclGrantsAllVsEmptyDaclDeniesAll` (`mirror`)
- `SecurityAclTest.OwnerImplicitReadControlAndWriteDac` (`mirror`)
- `SecurityAclTest.OwnerRestoresDaclUnderDenyReadAce` (`mirror`)
- `SecurityAclTest.TakeOwnershipWithPrivilegeAndWriteOwner` (`mirror`)

### GAP-20: `memfs` Persistent ACLs, Inheritance, and Access Checks

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`

**NTFS Specification (Microsoft proof):**
Per [File Security and Access Rights][win32-file-security],
[MS-FSA 2.1.5.1.2 (Open of an Existing File)][ms-fsa-open-existing], and
[MS-FSA 2.1.5.17 (Setting Security Information)][ms-fsa-setsec], NTFS volumes
store persistent security descriptors (shared between a base file and its
alternate streams), inherit ACEs according to `OI`/`CI`/`IO`/`NP` flags on
create and copy while preserving them across intra-volume rename, enforce
DACL evaluation (including explicit-deny precedence, `DELETE` vs
`FILE_DELETE_CHILD`, `FILE_TRAVERSE`, owner rights, handle access masks, and
privileges), map generic directory rights, and support `NtSetSecurityObject`
for DACLs, owners, and SACLs.

**Observed Dokan Behavior:**
`memfs_createfile` (`samples/dokan_memfs/memfs_operations.cpp:100`) carries an
unimplemented `// TODO Use AccessCheck to check security rights` comment: it
never evaluates DACLs, never propagates inheritable ACEs from parent
directories, and does not advertise `FILE_PERSISTENT_ACLS`. Although `memfs`
implements `memfs_getfilesecurity` and `memfs_setfilesecurity`
(`samples/dokan_memfs/memfs_operations.cpp:647-734`), `filenode::filenode`
(`samples/dokan_memfs/filenode.cpp:40-43`) leaves `descriptor == nullptr` and
`descriptor_size == 0` on default-created nodes (whenever no explicit
`SecurityDescriptor` is passed at create time). Subsequent `memfs_setfilesecurity` calls (`:717-728`) therefore pass a 0-byte
`HeapAlloc` buffer to `SetPrivateObjectSecurity`, which fails and rejects
`SetFileSecurity` / `NtSetSecurityObject` DACL and SACL updates.

**Covered Tests:**

<!-- markdownlint-disable MD013 -->

- `CreateOpenTest.BackupPrivilegeBypassesDenyDacl` (`memfs`)
- `CreateOpenTest.NtSupersedeVsOverwriteDaclAccess` (`memfs`)
- `CreateOpenTest.RestorePrivilegeBypassesWriteDenyDacl` (`memfs`)
- `CreateOpenTest.ZeroDesiredAccessOpensMetadataThroughExclusiveAndDenyDacl`
  (`memfs`)
- `InfoQueriesTest.ReadDirectoryChangesSecurityFilter` (`memfs`)
- `SecurityAclTest.CopyFileInheritsDestinationDaclUnlikeMove` (`memfs`)
- `SecurityAclTest.CreatorGroupSidExpandsOnInheritedAce` (`memfs`)
- `SecurityAclTest.CreatorOwnerSidExpandsOnInheritedAce` (`memfs`)
- `SecurityAclTest.DaclInheritanceFromParentDirectory` (`memfs`)
- `SecurityAclTest.DeleteRightVsParentDeleteChildRight` (`memfs`)
- `SecurityAclTest.DirectoryAddFileVsAddSubdirectoryRightsDistinct` (`memfs`)
- `SecurityAclTest.DirectoryGenericRightsMapToSpecificRights` (`memfs`)
- `SecurityAclTest.DirectoryListAndCreateFileAccessEnforcement` (`memfs`)
- `SecurityAclTest.DirectoryTraverseAccessEnforcement` (`memfs`)
- `SecurityAclTest.ExplicitDenyOverridesAllowAndExplicitChildAllowOverridesInheritedDeny`
  (`memfs`)
- `SecurityAclTest.FileExecuteOnlyAllowsImageMappingDeniesReadData` (`memfs`)
- `SecurityAclTest.GranularAttributeAndEaAccessRightsEnforcement` (`memfs`)
- `SecurityAclTest.InheritanceFlagsInheritOnlyAndNoPropagate` (`memfs`)
- `SecurityAclTest.LowIntegrityCallerDeniedWriteUpToUnlabeledFile` (`memfs`)
- `SecurityAclTest.NtQueryAndSetSecurityObjectDirect` (`memfs`)
- `SecurityAclTest.NullDaclGrantsAllVsEmptyDaclDeniesAll` (`memfs`)
- `SecurityAclTest.ObjectInheritOnlyVsContainerInheritOnly` (`memfs`)
- `SecurityAclTest.OwnerImplicitReadControlAndWriteDac` (`memfs`)
- `SecurityAclTest.RenameAllowedByParentDeleteChildWhenFileDeniesDelete`
  (`memfs`)
- `SecurityAclTest.RenameDeniedWithoutDeleteOrParentDeleteChild` (`memfs`)
- `SecurityAclTest.SaclGetAndSet` (`memfs`)
- `SecurityAclTest.SecurityDescriptorPreservedAcrossIntraVolumeRename` (`memfs`)
- `SecurityAclTest.SecurityObjectApisEnforceHandleAccessMasks` (`memfs`)
- `SecurityAclTest.SetDenyReadDaclEnforcesAccessDenied` (`memfs`)
- `SecurityAclTest.TakeOwnershipWithPrivilegeAndWriteOwner` (`memfs`)
- `SecurityAclTest.WriteDataAndAppendDataAccessEnforcement` (`memfs`)
- `StreamsTest.SecurityDescriptorSharedBetweenBaseFileAndAlternateStream`
  (`memfs`)
- `TimestampsAttrsTest.SecurityChangeUpdatesChangeTimeNotLastWriteTime`
  (`memfs`)

<!-- markdownlint-enable MD013 -->

### GAP-21: POSIX Delete, Rename, and Extended Disposition Flags

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.15.4 (`FileDispositionInformationEx`)][ms-fsa-disp-ex],
[`FILE_DISPOSITION_INFORMATION_EX`][wdk-disp-ex], and
[`FILE_RENAME_INFORMATION`][wdk-rename-info], Windows 10+ NTFS supports
`FILE_DISPOSITION_POSIX_SEMANTICS` (immediate namespace unlink while handles
or, on Windows 11 24H2+, data-mapped views remain open),
`FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE`, rejecting
`FILE_DISPOSITION_ON_CLOSE` on non-delete-on-close handles with
`STATUS_NOT_SUPPORTED`, and `FileRenameInformationEx` flags
`FILE_RENAME_POSIX_SEMANTICS` (including replacing open data-mapped targets on
Windows 10 1809+) and `FILE_RENAME_IGNORE_READONLY_ATTRIBUTE`.

**Observed Dokan Behavior:**
`DokanDispatchSetInformation` (`sys/fileinfo.c`) and
`DokanSetDispositionInformation` (`dokan/setfile.c`) ignore
`POSIX_SEMANTICS` and `ON_CLOSE` on `FileDispositionInformationEx`. While
`DokanSetDispositionInformation` honors
`FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE` to bypass its central read-only
guard (fixing `memfs`), `MirrorDeleteFile`
(`samples/dokan_mirror/mirror.c:850-856`) still calls Win32
`FileDispositionInfo` without `FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE`. On
Windows 11 24H2+ (build `>= 26100`), `DeleteFileW` uses POSIX unlink on
data-mapped files, which therefore fails `ERROR_ACCESS_DENIED` on both `memfs`
and `mirror`.
Likewise, `PopulateRenameEventInformations` (`sys/fileinfo.c`) and
`DokanSetRenameInformation` (`dokan/setfile.c`) forward only `ReplaceIfExists`
to `DOKAN_OPERATIONS::MoveFile`, discarding all `FileRenameInformationEx`
flags (and on `memfs`, `fs_filenodes::move` also fails the pre-check by
allowing non-POSIX overwrite of an open data-mapped target, per `GAP-16`).

**Covered Tests:**

- `DeleteTest.DeleteFileWOnMappedFilePosixUnlinksOnWin11_24H2`
  (`memfs`, `mirror`)
- `DeleteTest.DeleteFileWhileOpenPosix` (`memfs`, `mirror`)
- `DeleteTest.DispositionInfoExDeleteOnCloseFlag` (`memfs`, `mirror`)
- `DeleteTest.DispositionInfoExIgnoreReadOnly` (`mirror`)
- `DeleteTest.DispositionInfoExPosixOnEmptyDirectory` (`memfs`, `mirror`)
- `DeleteTest.DispositionInfoExPosixUnlinkAndReuseName` (`memfs`, `mirror`)
- `RenameMoveTest.FileRenameInfoExIgnoreReadOnlyTarget` (`memfs`, `mirror`)
- `RenameMoveTest.FileRenameInfoExPosixReplaceOpenTarget` (`memfs`, `mirror`)
- `RenameMoveTest.PosixRenameOntoDeletePendingTargetFailsDeletePending`
  (`memfs`, `mirror`)
- `RenameMoveTest.PosixRenameReplacesDataMappedTarget` (`memfs`, `mirror`)
- `StreamsTest.PosixDeleteFileWhileStreamHandleOpenUnlinksName`
  (`memfs`, `mirror`)

### GAP-22: Reparse Points, Symbolic Links, and Junctions

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [Reparse Points][win32-reparse],
[MS-FSA 2.1.5.10 (`FSCTL_SET_REPARSE_POINT` / `GET` / `DELETE`)][ms-fsa-events],
and [MS-FSCC 2.3 (`FSCTLs`)][ms-fscc-fsctl], NTFS supports setting, querying,
and deleting reparse points (symbolic links, directory junctions, and custom
GUID tags, rejecting tag/GUID mismatches), returns `STATUS_REPARSE` during
path resolution (including intermediate component traversal, chained symlinks,
`FILE_FLAG_OPEN_REPARSE_POINT`, and loop detection), and reports reparse tags
in `WIN32_FIND_DATAW::dwReserved0`, `FileAttributeTagInformation`, and
`FileIdExtdDirectoryInformation`.

**Observed Dokan Behavior:**
`DokanVolumeUserFsRequest` (`sys/fscontrol.c`) hardcodes
`STATUS_NOT_A_REPARSE_POINT` for `FSCTL_GET_REPARSE_POINT` and returns
`STATUS_INVALID_DEVICE_REQUEST` (`ERROR_INVALID_FUNCTION`) for
`FSCTL_SET_REPARSE_POINT` and `FSCTL_DELETE_REPARSE_POINT`.
`DokanFillFileAttributeTagInfo` (`dokan/fileinfo.c`) hardcodes
`ReparseTag = 0`, `DokanWriteDirectoryEntry` (`dokan/directory.c:107-159`)
leaves `ReparsePointTag` zero-initialized for `FileIdExtdDirectoryInformation`
and `FileIdExtdBothDirectoryInformation`, and `sys/create.c` has no
`STATUS_REPARSE` dispatch path.

**Covered Tests:**

- `DirEnumTest.FileIdExtdDirectoryInfoReportsReparseTag` (`memfs`, `mirror`)
- `InfoQueriesTest.NtQueryFileAttributeTagJunctionReparsePoint`
  (`memfs`, `mirror`)
- `LinksReparseTest.BrokenSymlinkOpenFailsAndOpenReparsePointSucceeds`
  (`memfs`, `mirror`)
- `LinksReparseTest.ChainedSymlinksAndIntermediateComponentTraversal`
  (`memfs`, `mirror`)
- `LinksReparseTest.CustomApplicationReparseTagRoundTrip` (`memfs`, `mirror`)
- `LinksReparseTest.DeleteReparsePointWithMismatchedGuidFails`
  (`memfs`, `mirror`)
- `LinksReparseTest.DeleteReparsePointWithMismatchedTagFails`
  (`memfs`, `mirror`)
- `LinksReparseTest.DirectoryJunctionCreateFollowAndRemove` (`memfs`, `mirror`)
- `LinksReparseTest.DirectorySymlinkFollowAndRemove` (`memfs`, `mirror`)
- `LinksReparseTest.DirectorySymlinkToFileFailsDirectoryOpen`
  (`memfs`, `mirror`)
- `LinksReparseTest.FileSymlinkFollowAndOpenReparsePoint` (`memfs`, `mirror`)
- `LinksReparseTest.FileSymlinkToAlternateDataStream` (`memfs`, `mirror`)
- `LinksReparseTest.FileSymlinkToDirectoryFailsDataOpen` (`memfs`, `mirror`)
- `LinksReparseTest.FindFilesReportsReparseTagInReserved0` (`memfs`, `mirror`)
- `LinksReparseTest.FindFilesReportsSymlinkReparseTagInReserved0`
  (`memfs`, `mirror`)
- `LinksReparseTest.GetAndDeleteReparsePointFsctl` (`memfs`, `mirror`)
- `LinksReparseTest.GetReparsePointSmallBufferFailsMoreData` (`memfs`, `mirror`)
- `LinksReparseTest.JunctionIntermediateComponentTraversal` (`memfs`, `mirror`)
- `LinksReparseTest.JunctionOnNonEmptyDirectoryFailsNotEmpty`
  (`memfs`, `mirror`)
- `LinksReparseTest.RelativeSymlinkResolution` (`memfs`, `mirror`)
- `LinksReparseTest.RemoveJunctionPointingToNonEmptyDirectorySucceeds`
  (`memfs`, `mirror`)
- `LinksReparseTest.RenameSymlinkRenamesLinkNotTargetAndIndependentMetadata`
  (`memfs`, `mirror`)
- `LinksReparseTest.ReparseDataAtMaximumSizeRoundTrips` (`memfs`, `mirror`)
- `LinksReparseTest.ReparseDataOverMaximumSizeFailsInvalidReparseData`
  (`memfs`, `mirror`)
- `LinksReparseTest.ReparsePointMatchingTagAndGuidUpdatesAndDeletes`
  (`memfs`, `mirror`)
- `LinksReparseTest.SetReparsePointWithMismatchedGuidFails` (`memfs`, `mirror`)
- `LinksReparseTest.SetReparsePointWithMismatchedTagFails` (`memfs`, `mirror`)
- `LinksReparseTest.SymlinkLoopDetectedAsCantResolveFilename`
  (`memfs`, `mirror`)

### GAP-23: Hard Links (`FileLinkInformation`) and Enumeration

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [Hard Links and Junctions][win32-hardlinks] and
[MS-FSA 2.1.5.15.7 (`FileLinkInformation`)][ms-fsa-link], NTFS allows
multiple directory entries to reference a single file record (sharing data,
`FileIndex`, metadata, and alternate streams while tracking `NumberOfLinks`,
supporting links to read-only files and renaming one link onto a sibling),
rejects hard links on directories with `STATUS_FILE_IS_A_DIRECTORY` /
`ERROR_ACCESS_DENIED`, and enumerates links via `FileHardLinkInformation`.

**Observed Dokan Behavior:**
`DispatchSetInformation` (`dokan/setfile.c`) and
`DokanEndDispatchGetFileInformation` (`dokan/fileinfo.c`) do not handle
`FileLinkInformation`, `FileLinkInformationEx`, or `FileHardLinkInformation`,
returning `STATUS_INVALID_PARAMETER` (`ERROR_INVALID_PARAMETER`), and
`DOKAN_OPERATIONS` defines no hard-link callback.

**Covered Tests:**

- `DeleteTest.DeleteOnCloseThroughHardLinkRemovesOnlyThatLink`
  (`memfs`, `mirror`)
- `DeleteTest.HardLinkLegacyDeletePreservesOtherLink` (`memfs`, `mirror`)
- `DeleteTest.HardLinkPosixUnlinkPreservesOtherLink` (`memfs`, `mirror`)
- `LinksReparseTest.FileHardLinkInformationListsAllLinks` (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkCountLimitFailsTooManyLinks` (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkCreateSharedDataAndFileIndex` (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkFileNameEnumeration` (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkOnDirectoryFailsAccessDenied` (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkOnReadOnlyFile` (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkOverExistingFileFailsAlreadyExists`
  (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkRenameOneLinkPreservesOtherLinks`
  (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkSharedMetadataAndUnlink` (`memfs`, `mirror`)
- `LinksReparseTest.HardLinkSharesAlternateDataStreams` (`memfs`, `mirror`)
- `LinksReparseTest.LinkInfoExPosixReplacesOpenTarget` (`memfs`, `mirror`)
- `RenameMoveTest.RenameHardLinkOntoSiblingHardLink` (`memfs`, `mirror`)
- `TimestampsAttrsTest.HardLinkCreationUpdatesChangeTimeNotLastWriteTime`
  (`memfs`, `mirror`)

### GAP-24: Sparse Files and Zero-Data Deallocation

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [Sparse Files][win32-sparse],
[MS-FSA 2.1.5.10 (`FSCTL_SET_SPARSE` / `FSCTL_SET_ZERO_DATA` /
`FSCTL_QUERY_ALLOCATED_RANGES`)][ms-fsa-events], and
[MS-FSCC 2.3 (`FSCTLs`)][ms-fscc-fsctl], NTFS tracks
`FILE_ATTRIBUTE_SPARSE_FILE` per stream (which cannot be cleared via
`SetFileAttributesW`; on Windows 8+ NTFS, `FSCTL_SET_SPARSE` with
`SetSparse = FALSE` clears the flag by allocating/materializing any sparse
holes while preserving data), zeroes ranges on both sparse and non-sparse files
via `FSCTL_SET_ZERO_DATA` (including sub-cluster ranges and 64-bit offsets over
4 GiB), deallocates sparse holes, and enumerates allocated extents via
`FSCTL_QUERY_ALLOCATED_RANGES` (including paginated small-buffer queries).

**Observed Dokan Behavior:**
`DokanVolumeUserFsRequest` (`sys/fscontrol.c`) implements none of
`FSCTL_SET_SPARSE`, `FSCTL_SET_ZERO_DATA`, or `FSCTL_QUERY_ALLOCATED_RANGES`,
falling through to `STATUS_INVALID_DEVICE_REQUEST` (`ERROR_INVALID_FUNCTION`),
and `DOKAN_OPERATIONS` exposes no generic FSCTL callback.

**Covered Tests:**

- `DataIoTest.QueryAllocatedRangesNegativeRangeFailsInvalidParameter`
  (`memfs`, `mirror`)
- `DataIoTest.QueryAllocatedRangesOnNonSparseFileReportsWholeFile`
  (`memfs`, `mirror`)
- `DataIoTest.SetZeroDataInvalidRangeFailsInvalidParameter` (`memfs`, `mirror`)
- `DataIoTest.SetZeroDataOnNonSparseFileZeroesWithoutSettingSparse`
  (`memfs`, `mirror`)
- `DataIoTest.SetZeroDataPastEofDoesNotExtendFile` (`memfs`, `mirror`)
- `DataIoTest.SparseClearFlagWithHoleSucceedsAndPreservesData`
  (`memfs`, `mirror`)
- `DataIoTest.SparseExtendWithSetEndOfFileLeavesHoleUnallocated`
  (`memfs`, `mirror`)
- `DataIoTest.SparseFile64BitOffsetsOver4GiB` (`memfs`, `mirror`)
- `DataIoTest.SparseQueryAllocatedRanges` (`memfs`, `mirror`)
- `DataIoTest.SparseQueryAllocatedRangesBufferPagination` (`memfs`, `mirror`)
- `DataIoTest.SparseSetAndQueryFlag` (`memfs`, `mirror`)
- `DataIoTest.SparseSetZeroDataSubClusterRangeZeroesOnlyRequestedBytes`
  (`memfs`, `mirror`)
- `DataIoTest.SparseZeroDataDeallocatesAndReadsZeroes` (`memfs`, `mirror`)
- `StreamsTest.SparseNamedStreamZeroDataAndAllocatedRanges` (`memfs`, `mirror`)
- `TimestampsAttrsTest.SetFileAttributesCannotClearSparseFlag`
  (`memfs`, `mirror`)

### GAP-25: NTFS File Compression

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [File Compression and Decompression][win32-compression],
[MS-FSA 2.1.5.10 (`FSCTL_SET_COMPRESSION`)][ms-fsa-events], and
[MS-FSCC 2.4 (`FileCompressionInformation`)][ms-fscc-fileinfo], NTFS supports
enabling and querying LZNT1 compression via `FSCTL_SET_COMPRESSION` /
`FSCTL_GET_COMPRESSION`, inherits directory compression onto newly created
children (unless `FILE_NO_COMPRESSION` is specified), updates
`FILE_ATTRIBUTE_COMPRESSED`, and reports compressed size via
`FileCompressionInformation`.

**Observed Dokan Behavior:**
`DokanVolumeUserFsRequest` (`sys/fscontrol.c`) rejects
`FSCTL_SET_COMPRESSION` and `FSCTL_GET_COMPRESSION` with
`STATUS_INVALID_DEVICE_REQUEST` (`ERROR_INVALID_FUNCTION`), and
`DokanEndDispatchGetFileInformation` (`dokan/fileinfo.c:410-413`) returns
`STATUS_NOT_IMPLEMENTED` for `FileCompressionInformation`.

**Covered Tests:**

- `DataIoTest.CompressionSetGetAndCompressedFileSize` (`memfs`, `mirror`)
- `DataIoTest.DirectoryCompressionInheritanceAndNoCompressionOption`
  (`memfs`, `mirror`)
- `InfoQueriesTest.NtQueryFileCompressionInformation` (`memfs`, `mirror`)

### GAP-26: Extended Attributes Query and Set

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.15.6 (`FileFullEaInformation`)][ms-fsa-ea] and
[MS-FSCC 2.4 (`File Information Classes`)][ms-fscc-fileinfo], NTFS persists
key-value Extended Attributes supplied at create time (`NtCreateFile`
`EaBuffer`) or via `NtSetEaFile`, returns them via `NtQueryEaFile` (including
`EaList` filtering, `FILE_NEED_EA`, and invalid-buffer rejection), and reports
`EaSize` in `FileEaInformation` and `FileFullDirectoryInformation`.

**Observed Dokan Behavior:**
`DriverEntry` (`sys/dokan.c:267-292`) never registers
`DriverObject->MajorFunction[IRP_MJ_QUERY_EA]` or
`DriverObject->MajorFunction[IRP_MJ_SET_EA]`, so the Windows I/O Manager's
default `IopInvalidDeviceRequest` fails `NtQueryEaFile` and `NtSetEaFile` with
`STATUS_INVALID_DEVICE_REQUEST` before `DokanDispatchRequest` is ever entered.
In addition, `DokanDispatchCreate` (`sys/create.c`) ignores incoming
`EaBuffer` payloads, and `dokan/fileinfo.c` / `dokan/directory.c` hardcode
`EaSize = 0`.

**Covered Tests:**

- `DirEnumTest.FileFullDirectoryInfoReportsEaSize` (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesCreateWithEaBuffer` (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesDeleteAndEmptyQuery` (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesEaListQueryReturnsRequestedSubset`
  (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesInconsistentEaListRejected`
  (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesInvalidEaNameRejected` (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesNeedEaBlocksNoEaKnowledgeOpen`
  (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesOnDirectory` (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesOptionalEaAllowsNoEaKnowledgeOpen`
  (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesOversizedEaSetRejected` (`memfs`, `mirror`)
- `InfoQueriesTest.ExtendedAttributesSetAndQuery` (`memfs`, `mirror`)

### GAP-27: Open-by-File-ID and NTFS Object IDs

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [`FILE_ID_DESCRIPTOR`][win32-fileid-desc],
[`ZwCreateFile`][wdk-zwcreatefile], and
[MS-FSA 2.1.5.10 (`FSCTL_CREATE_OR_GET_OBJECT_ID` /
`FSCTL_GET_OBJECT_ID`)][ms-fsa-events], NTFS supports opening files by 64-bit
File ID or 128-bit GUID Object ID via `FILE_OPEN_BY_FILE_ID` / `OpenFileById`
and managing per-file GUID Object IDs via Object ID FSCTLs.

**Observed Dokan Behavior:**
`DokanDispatchCreate` (`sys/create.c`) treats `fileObject->FileName` strictly
as a Unicode path string and never checks `FILE_OPEN_BY_FILE_ID`, while
`DokanVolumeUserFsRequest` (`sys/fscontrol.c`) returns
`STATUS_INVALID_DEVICE_REQUEST` (`ERROR_INVALID_FUNCTION`) for all Object ID
FSCTLs.

**Covered Tests:**

- `InfoQueriesTest.ObjectIdCreateGetAndOpenByObjectId` (`memfs`, `mirror`)
- `InfoQueriesTest.OpenFileByIdAndNtOpenByFileId` (`memfs`, `mirror`)

### GAP-28: Modern Info Classes and Volume Feature Flags

- **Severity:** Unimplemented NTFS Feature
- **Affected Targets:** `memfs`, `mirror`

**NTFS Specification (Microsoft proof):**
Per [`FILE_STANDARD_LINK_INFORMATION`][wdk-stdlink],
[`FILE_STAT_INFORMATION`][wdk-filestat],
[`FILE_CASE_SENSITIVE_INFORMATION`][wdk-casesens],
[MS-FSCC 2.5 (`FileFsAttributeInformation` and
`FileFsSectorSizeInformation`)][ms-fscc-fsinfo],
and [`GetVolumeInformationW`][win32-volinfo], modern NTFS supports querying
`FileStandardLinkInformation`, `FileStatInformation`, `FileStatLxInformation`,
and `FileFsSectorSizeInformation`, querying/setting per-directory
`FileCaseSensitiveInformation` (inherited by child directories), and
advertising accurate NTFS capability flags.

**Observed Dokan Behavior:**
`DokanEndDispatchGetFileInformation` (`dokan/fileinfo.c`) and
`DispatchSetInformation` (`dokan/setfile.c`) do not recognize
`FileStandardLinkInformation`, `FileStatInformation`,
`FileStatLxInformation`, or `FileCaseSensitiveInformation`, falling through
to `STATUS_INVALID_PARAMETER`. Likewise,
`DokanDispatchQueryVolumeInformation` (`sys/volume.c:55-234`) handles only
`FileFsVolumeInformation`, `FileFsSizeInformation`, `FileFsDeviceInformation`,
`FileFsAttributeInformation`, and `FileFsFullSizeInformation`, returning
`STATUS_INVALID_PARAMETER` for `FileFsSectorSizeInformation` and
`FileFsControlInformation`, while `DokanVolumeUserFsRequest`
(`sys/fscontrol.c`) rejects `FSCTL_READ_FILE_USN_DATA` with
`STATUS_INVALID_DEVICE_REQUEST` (`ERROR_INVALID_FUNCTION`). In volume
queries, `memfs_getvolumeinformation`
(`samples/dokan_memfs/memfs_operations.cpp`) omits persistent ACL, sparse,
reparse, hard-link, and EA flags, while `MirrorGetVolumeInformation`
(`samples/dokan_mirror/mirror.c:1304-1308`) sets `FILE_PERSISTENT_ACLS |
FILE_NAMED_STREAMS | FILE_UNICODE_ON_DISK | FILE_SUPPORTS_REMOTE_STORAGE` but
omits sparse, reparse, hard-link, EA, and `FILE_CASE_PRESERVED_NAMES` (and
passing `/b` overwrites those flags with only
`FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES`).

**Covered Tests:**

- `InfoQueriesTest.GetVolumeInformationNtfsFeatureFlags` (`memfs`, `mirror`)
- `InfoQueriesTest.NtQueryFileFsSectorSizeInformation` (`memfs`, `mirror`)
- `InfoQueriesTest.NtQueryFileStandardLinkInfo` (`memfs`, `mirror`)
- `InfoQueriesTest.NtQueryFileStatAndStatLxInformation` (`memfs`, `mirror`)
- `InfoQueriesTest.NtQueryVolumeFsControlInformation` (`memfs`, `mirror`)
- `InfoQueriesTest.ReadFileUsnDataMatchesFileIdParentAndName`
  (`memfs`, `mirror`)
- `NamingPathsTest.PerDirectoryCaseSensitivityFlag` (`memfs`, `mirror`)
- `NamingPathsTest.PerDirectoryCaseSensitivityInheritanceAndMoveSemantics`
  (`memfs`, `mirror`)
- `NamingPathsTest.PerDirectoryCaseSensitivityRejectedOnNonEmptyDirectory`
  (`memfs`, `mirror`)

### GAP-30: Missing VCB Lock on `FileRenameInformationEx` FCB Rename

- **Severity:** Crash / Handle Leak
- **Affected Targets:** `memfs`, `mirror` (`skip_crash: true`)

**NTFS Specification (Microsoft proof):**
Per [MS-FSA 2.1.5.15.12 (`FileRenameInformation`)][ms-fsa-rename] and
[`FILE_RENAME_INFORMATION`][wdk-rename-info], concurrent
`FileRenameInformationEx` (class 65) renames and `IRP_MJ_CREATE` path lookups
on the same namespace must synchronize FCB table mutations without memory
corruption or kernel crashes.

**Observed Dokan Behavior:**
In `sys/fileinfo.c:864-868`, `DokanCompleteSetInformation` acquires
`DokanVCBLockRW(RequestContext->Vcb)` only when
`infoClass == FileRenameInformation`, omitting `FileRenameInformationEx`. At
`sys/fileinfo.c:894-907`, both `FileRenameInformation` and
`FileRenameInformationEx` invoke `DokanRenameFcb` (`sys/util/fcb.c:489-500`),
mutating the `Vcb->FcbTable` AVL tree without holding the VCB lock and racing
concurrent `DokanDispatchCreate` FCB lookups (risking a kernel BSOD under
concurrent `FileRenameInformationEx` + `CreateFileW` stress; `skip_crash` is
set preemptively to prevent VM bugchecks until `sys/fileinfo.c:865` is fixed).

**Covered Tests:**

- `RenameMoveTest.ConcurrentFileRenameInfoExAndCreateStress`
  (`memfs`, `mirror`, `skip_crash`)

<!-- markdownlint-disable MD013 -->

[ms-fsa-events]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/da2364cd-37da-4e20-8ae3-d3493e8e50b1
[ms-fsa-open-existing]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/fcb0b2d4-1a91-4c74-9f79-c5c8e31a89f9
[ms-fsa-wildcard]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/38f095fc-efd0-4054-a690-3a1ec4b9015c
[ms-fsa-allinfo]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/f67e52b2-f327-463d-8621-e1262d1487ea
[ms-fsa-streams]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/b50fa789-22a4-4a41-b8ea-0b5c9ea0bebc
[ms-fsa-normname]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/074a169b-2a78-4389-9dbd-0275ccb9552d
[ms-fsa-alloc]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/3c415516-7787-430b-9db7-27bde29a39ca
[ms-fsa-basic]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/68e3bdae-4cf0-424a-93ef-97b5e408ec96
[ms-fsa-disp]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/d316718d-ef57-45db-9ff0-eb31e0b3c66f
[ms-fsa-disp-ex]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/114c000d-5aae-4e78-8314-e05ee73e73cc
[ms-fsa-eof]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/b15e0a0d-3e2b-42fa-b0f3-b3c99008985c
[ms-fsa-ea]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/837b03b6-963e-46cb-8457-4d7a8d9b1eef
[ms-fsa-link]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/df33b8a1-6385-48b4-8255-a0cd7c963a70
[ms-fsa-rename]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/48574d75-4dcf-448c-af9f-5ccab3d5c90a
[ms-fsa-setsec]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/4873729e-c85e-47a3-b457-3bf7962624d5
[ms-fsa-oplock]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/38f0898f-517f-47dc-9f06-e17f0e5eb46d
[ms-fscc-fsctl]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/e44db832-51e6-4034-81c8-ce79490e6df0
[ms-fscc-fileinfo]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/a9bcadbf-8d26-47ab-af96-3bc55627685e
[ms-fscc-fsinfo]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/9611b714-f5a0-4a87-9bb8-dc921869e5d4
[ms-fscc-attrs]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/b34bc2eb-1fbf-4729-b68e-ad7a99f18b3d
[ms-fscc-notify]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/3c678a3a-3c75-472d-82d2-5a415a999fa1
[win32-naming]: https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file
[win32-file-streams]: https://learn.microsoft.com/en-us/windows/win32/fileio/file-streams
[win32-file-buffering]: https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering
[win32-file-view]: https://learn.microsoft.com/en-us/windows/win32/memory/reading-and-writing-from-a-file-view
[win32-finalpath]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew
[win32-volinfo]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationw
[win32-lockfileex]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex
[win32-rdc-ex]: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesexw
[win32-fileid-desc]: https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_descriptor
[win32-hardlinks]: https://learn.microsoft.com/en-us/windows/win32/fileio/hard-links-and-junctions
[win32-file-security]: https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights
[win32-privileges]: https://learn.microsoft.com/en-us/windows/win32/secauthz/privilege-constants
[win32-sparse]: https://learn.microsoft.com/en-us/windows/win32/fileio/sparse-files
[win32-reparse]: https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-points
[win32-compression]: https://learn.microsoft.com/en-us/windows/win32/fileio/file-compression-and-decompression
[wdk-mmflushimage]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-mmflushimagesection
[wdk-check-write-lock]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-fsrtlchecklockforwriteaccess
[wdk-querydir-ex]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntquerydirectoryfileex
[wdk-zwcreatefile]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatefile
[wdk-disp-ex]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_disposition_information_ex
[wdk-rename-info]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
[wdk-vdl-info]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_valid_data_length_information
[wdk-casesens]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_case_sensitive_information
[wdk-filestat]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_stat_information
[wdk-stdlink]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_standard_link_information

<!-- markdownlint-enable MD013 -->
