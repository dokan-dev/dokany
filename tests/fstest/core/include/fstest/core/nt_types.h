// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file nt_types.h
/// \brief Clean-room NT native ABI structures, information class enums, and
/// Windows 10+ bitflag constants scoped strictly inside `namespace fstest::nt`.
///
/// Zero global `#define` macros are introduced so this header compiles cleanly
/// alongside private host kernel headers and under `-D_WIN32_WINNT=0x0602`.

#pragma once

#include "fstest/core/win_headers.h"

#include <cstddef>
#include <cstdint>

namespace fstest::nt {

/// \name NTSTATUS Severity Helpers
/// \{

/// \brief Returns true iff \p status indicates success (`status >= 0`).
inline constexpr bool NtSuccess(NTSTATUS status) noexcept {
  return status >= 0;
}

/// \brief Returns true iff \p status has informational severity (bits 31:30 == 1).
inline constexpr bool NtInformation(NTSTATUS status) noexcept {
  return (static_cast<ULONG>(status) >> 30) == 1UL;
}

/// \brief Returns true iff \p status has warning severity (bits 31:30 == 2).
inline constexpr bool NtWarning(NTSTATUS status) noexcept {
  return (static_cast<ULONG>(status) >> 30) == 2UL;
}

/// \brief Returns true iff \p status has error severity (bits 31:30 == 3).
inline constexpr bool NtError(NTSTATUS status) noexcept {
  return (static_cast<ULONG>(status) >> 30) == 3UL;
}
/// \}

/// \name Supplemental NTSTATUS Constants
/// Constants that may be absent from older Windows SDK headers.
/// \{
inline constexpr NTSTATUS kStatusOplockBreakInProgress =
    static_cast<NTSTATUS>(0x00000108L);
inline constexpr NTSTATUS kStatusNoMoreEas = static_cast<NTSTATUS>(0x80000012L);
inline constexpr NTSTATUS kStatusEaListInconsistent =
    static_cast<NTSTATUS>(0x80000014L);
inline constexpr NTSTATUS kStatusStoppedOnSymlink =
    static_cast<NTSTATUS>(0x8000002DL);
inline constexpr NTSTATUS kStatusEaTooLarge =
    static_cast<NTSTATUS>(0xC0000050L);
inline constexpr NTSTATUS kStatusNonexistentEaEntry =
    static_cast<NTSTATUS>(0xC0000051L);
inline constexpr NTSTATUS kStatusNoEasOnFile =
    static_cast<NTSTATUS>(0xC0000052L);
inline constexpr NTSTATUS kStatusInvalidSecurityDescr =
    static_cast<NTSTATUS>(0xC0000079L);
inline constexpr NTSTATUS kStatusOplockNotGranted =
    static_cast<NTSTATUS>(0xC00000E2L);
inline constexpr NTSTATUS kStatusReparseTagMismatch =
    static_cast<NTSTATUS>(0xC0000277L);
inline constexpr NTSTATUS kStatusCaseDifferingNamesInDir =
    static_cast<NTSTATUS>(0xC00004B3L);
inline constexpr NTSTATUS kStatusCannotBreakOplock =
    static_cast<NTSTATUS>(0xC0000909L);
/// \}

/// \brief Maximum UTF-16 code unit count representable in a NUL-terminated
/// `UNICODE_STRING` without overflowing 16-bit `MaximumLength`.
inline constexpr size_t kMaxUnicodeStringChars = 32766u;

// ---------------------------------------------------------------------------
// Object attributes & NT create / disposition / rename / link / query flags
// ---------------------------------------------------------------------------
inline constexpr ULONG kObjInherit = 0x00000002UL;
inline constexpr ULONG kObjCaseInsensitive = 0x00000040UL;
inline constexpr ULONG kObjOpenIf = 0x00000080UL;

// NtCreateFile CreateDisposition values
inline constexpr ULONG kFileSupersede = 0x00000000UL;
inline constexpr ULONG kFileOpen = 0x00000001UL;
inline constexpr ULONG kFileCreate = 0x00000002UL;
inline constexpr ULONG kFileOpenIf = 0x00000003UL;
inline constexpr ULONG kFileOverwrite = 0x00000004UL;
inline constexpr ULONG kFileOverwriteIf = 0x00000005UL;

// IoStatusBlock.information values returned by NtCreateFile / NtOpenFile
inline constexpr ULONG_PTR kFileSuperseded = 0;
inline constexpr ULONG_PTR kFileOpened = 1;
inline constexpr ULONG_PTR kFileCreated = 2;
inline constexpr ULONG_PTR kFileOverwritten = 3;
inline constexpr ULONG_PTR kFileExists = 4;
inline constexpr ULONG_PTR kFileDoesNotExist = 5;

// NtCreateFile / NtOpenFile CreateOptions flags
inline constexpr ULONG kFileDirectoryFile = 0x00000001UL;
inline constexpr ULONG kFileWriteThrough = 0x00000002UL;
inline constexpr ULONG kFileSequentialOnly = 0x00000004UL;
inline constexpr ULONG kFileNoIntermediateBuffering = 0x00000008UL;
inline constexpr ULONG kFileSynchronousIoAlert = 0x00000010UL;
inline constexpr ULONG kFileSynchronousIoNonalert = 0x00000020UL;
inline constexpr ULONG kFileNonDirectoryFile = 0x00000040UL;
inline constexpr ULONG kFileCreateTreeConnection = 0x00000080UL;
inline constexpr ULONG kFileCompleteIfOplocked = 0x00000100UL;
inline constexpr ULONG kFileNoEaKnowledge = 0x00000200UL;
inline constexpr ULONG kFileOpenRemoteInstance = 0x00000400UL;
inline constexpr ULONG kFileRandomAccess = 0x00000800UL;
inline constexpr ULONG kFileDeleteOnClose = 0x00001000UL;
inline constexpr ULONG kFileOpenByFileId = 0x00002000UL;
inline constexpr ULONG kFileOpenForBackupIntent = 0x00004000UL;
inline constexpr ULONG kFileNoCompression = 0x00008000UL;
inline constexpr ULONG kFileOpenRequiringOplock = 0x00010000UL;
inline constexpr ULONG kFileDisallowExclusive = 0x00020000UL;
inline constexpr ULONG kFileReserveOpfilter = 0x00100000UL;
inline constexpr ULONG kFileOpenReparsePoint = 0x00200000UL;
inline constexpr ULONG kFileOpenNoRecall = 0x00400000UL;
inline constexpr ULONG kFileOpenForFreeSpaceQuery = 0x00800000UL;

// FileDispositionInformationEx flags (Windows 10 RS1+)
inline constexpr ULONG kFileDispositionDoNotDelete = 0x00000000UL;
inline constexpr ULONG kFileDispositionDelete = 0x00000001UL;
inline constexpr ULONG kFileDispositionPosixSemantics = 0x00000002UL;
inline constexpr ULONG kFileDispositionForceImageSectionCheck = 0x00000004UL;
inline constexpr ULONG kFileDispositionOnClose = 0x00000008UL;
inline constexpr ULONG kFileDispositionIgnoreReadonlyAttribute = 0x00000010UL;

// FileRenameInformationEx flags (Windows 10 RS1+)
inline constexpr ULONG kFileRenameReplaceIfExists = 0x00000001UL;
inline constexpr ULONG kFileRenamePosixSemantics = 0x00000002UL;
inline constexpr ULONG kFileRenameSuppressPinStateInheritance = 0x00000004UL;
inline constexpr ULONG kFileRenameSuppressStorageReserveInheritance =
    0x00000008UL;
inline constexpr ULONG kFileRenameNoIncreaseAvailableSpace = 0x00000010UL;
inline constexpr ULONG kFileRenameNoDecreaseAvailableSpace = 0x00000020UL;
inline constexpr ULONG kFileRenameIgnoreReadonlyAttribute = 0x00000040UL;
inline constexpr ULONG kFileRenameForceResizeTargetSr = 0x00000080UL;
inline constexpr ULONG kFileRenameForceResizeSourceSr = 0x00000100UL;

// FileLinkInformationEx flags (Windows 10 RS5+)
inline constexpr ULONG kFileLinkReplaceIfExists = 0x00000001UL;
inline constexpr ULONG kFileLinkPosixSemantics = 0x00000002UL;
inline constexpr ULONG kFileLinkSuppressStorageReserveInheritance =
    0x00000008UL;
inline constexpr ULONG kFileLinkNoIncreaseAvailableSpace = 0x00000010UL;
inline constexpr ULONG kFileLinkNoDecreaseAvailableSpace = 0x00000020UL;
inline constexpr ULONG kFileLinkIgnoreReadonlyAttribute = 0x00000040UL;
inline constexpr ULONG kFileLinkForceResizeTargetSr = 0x00000080UL;
inline constexpr ULONG kFileLinkForceResizeSourceSr = 0x00000100UL;

// FileCaseSensitiveInformation flags (Windows 10 1803+)
inline constexpr ULONG kFileCsFlagCaseSensitiveDir = 0x00000001UL;

// CreateSymbolicLinkW and ReparseDataBuffer flags
inline constexpr DWORD kSymbolicLinkFlagFile = 0x00000000UL;
inline constexpr DWORD kSymbolicLinkFlagDirectory = 0x00000001UL;
inline constexpr DWORD kSymbolicLinkFlagAllowUnprivilegedCreate = 0x00000002UL;
inline constexpr ULONG kSymlinkFlagRelative = 0x00000001UL;

// NtQueryDirectoryFileEx QueryFlags (SL_*)
inline constexpr ULONG kSlRestartScan = 0x00000001UL;
inline constexpr ULONG kSlReturnSingleEntry = 0x00000002UL;
inline constexpr ULONG kSlIndexSpecified = 0x00000004UL;
inline constexpr ULONG kSlReturnOnDiskEntriesOnly = 0x00000008UL;
inline constexpr ULONG kSlNoCursorUpdate = 0x00000010UL;

// NtFlushBuffersFileEx flags
inline constexpr ULONG kFlushFlagsFileDataOnly = 0x00000001UL;
inline constexpr ULONG kFlushFlagsNoSync = 0x00000002UL;
inline constexpr ULONG kFlushFlagsFileDataSyncOnly = 0x00000004UL;

// ReadDirectoryChangesExW information class constants (Windows 10 1709+).
inline constexpr ULONG kReadDirectoryNotifyInformation = 1UL;
inline constexpr ULONG kReadDirectoryNotifyExtendedInformation = 2UL;

// Extended Attribute entry flags
inline constexpr UCHAR kFileNeedEa = 0x80;

// Volume indexing/scrubbing policy attributes
inline constexpr DWORD kFileAttributeNotContentIndexed = 0x00002000UL;
inline constexpr DWORD kFileAttributeNoScrubData = 0x00020000UL;

// FileSystemAttributes flags (safe under _WIN32_WINNT=0x0602)
inline constexpr ULONG kFileCaseSensitiveSearch = 0x00000001UL;
inline constexpr ULONG kFileCasePreservedNames = 0x00000002UL;
inline constexpr ULONG kFileUnicodeOnDisk = 0x00000004UL;
inline constexpr ULONG kFilePersistentAcls = 0x00000008UL;
inline constexpr ULONG kFileFileCompression = 0x00000010UL;
inline constexpr ULONG kFileVolumeQuotas = 0x00000020UL;
inline constexpr ULONG kFileSupportsSparseFiles = 0x00000040UL;
inline constexpr ULONG kFileSupportsReparsePoints = 0x00000080UL;
inline constexpr ULONG kFileSupportsRemoteStorage = 0x00000100UL;
inline constexpr ULONG kFileSupportsPosixUnlinkRename = 0x00000400UL;
inline constexpr ULONG kFileVolumeIsCompressed = 0x00008000UL;
inline constexpr ULONG kFileSupportsObjectIds = 0x00010000UL;
inline constexpr ULONG kFileSupportsEncryption = 0x00020000UL;
inline constexpr ULONG kFileNamedStreams = 0x00040000UL;
inline constexpr ULONG kFileReadOnlyVolume = 0x00080000UL;
inline constexpr ULONG kFileSequentialWriteOnce = 0x00100000UL;
inline constexpr ULONG kFileSupportsTransactions = 0x00200000UL;
inline constexpr ULONG kFileSupportsHardLinks = 0x00400000UL;
inline constexpr ULONG kFileSupportsExtendedAttributes = 0x00800000UL;
inline constexpr ULONG kFileSupportsOpenByFileId = 0x01000000UL;
inline constexpr ULONG kFileSupportsUsnJournal = 0x02000000UL;

// Architecture constants safe under _WIN32_WINNT=0x0602
inline constexpr USHORT kImageFileMachineUnknown = 0x0000;
inline constexpr USHORT kImageFileMachineI386 = 0x014C;
inline constexpr USHORT kImageFileMachineAmd64 = 0x8664;
inline constexpr USHORT kImageFileMachineArm64 = 0xAA64;
inline constexpr WORD kProcessorArchitectureArm64 = 12;

// ---------------------------------------------------------------------------
// Oplock FSCTL Control Codes ([MS-FSCC] Section 2.3 / <winioctl.h>)
// ---------------------------------------------------------------------------
// User-mode applications request and acknowledge oplocks via DeviceIoControl
// using FSCTL_REQUEST_OPLOCK (REQUEST_OPLOCK_INPUT_BUFFER with
// REQUEST_OPLOCK_INPUT_FLAG_REQUEST / REQUEST_OPLOCK_INPUT_FLAG_ACK and
// OPLOCK_LEVEL_CACHE_{READ,HANDLE,WRITE}), legacy level/batch/filter FSCTLs,
// break-ack FSCTLs (FSCTL_OPLOCK_BREAK_ACKNOWLEDGE,
// FSCTL_OPLOCK_BREAK_ACK_NO_2, FSCTL_OPBATCH_ACK_CLOSE_PENDING), and create
// options FILE_OPEN_REQUIRING_OPLOCK / FILE_COMPLETE_IF_OPLOCKED.
// Oplock keys (OPLOCK_KEY_ECP_CONTEXT / DUAL_OPLOCK_KEY_ECP_CONTEXT) are
// kernel-mode Extra Create Parameters (IoCreateFileEx / FltCreateFileEx2) and
// cannot be passed directly through user-mode CreateFileW/CreateFile2 or
// NtCreateFile.
inline constexpr ULONG kFsctlRequestOplockLevel1 = 0x00090000UL;
inline constexpr ULONG kFsctlRequestOplockLevel2 = 0x00090004UL;
inline constexpr ULONG kFsctlRequestBatchOplock = 0x00090008UL;
inline constexpr ULONG kFsctlOplockBreakAcknowledge = 0x0009000CUL;
inline constexpr ULONG kFsctlOpbatchAckClosePending = 0x00090010UL;
inline constexpr ULONG kFsctlOplockBreakNotify = 0x00090014UL;
inline constexpr ULONG kFsctlOplockBreakAckNo2 = 0x00090050UL;
inline constexpr ULONG kFsctlRequestFilterOplock = 0x0009005CUL;
inline constexpr ULONG kFsctlRequestOplock = 0x00090240UL;

// ---------------------------------------------------------------------------
// NT Information Class Enums ([MS-FSCC] Section 2.4 & Section 2.5)
// ---------------------------------------------------------------------------
/// \brief [MS-FSCC] Section 2.4 File Information Classes (plus WDK `<ntifs.h>`
/// classes).
enum class FileInfoClass : ULONG {
  FileDirectoryInformation = 1,
  FileFullDirectoryInformation = 2,
  FileBothDirectoryInformation = 3,
  FileBasicInformation = 4,
  FileStandardInformation = 5,
  FileInternalInformation = 6,
  FileEaInformation = 7,
  FileAccessInformation = 8,
  FileNameInformation = 9,
  FileRenameInformation = 10,
  FileLinkInformation = 11,
  FileNamesInformation = 12,
  FileDispositionInformation = 13,
  FilePositionInformation = 14,
  FileFullEaInformation = 15,
  FileModeInformation = 16,
  FileAlignmentInformation = 17,
  FileAllInformation = 18,
  FileAllocationInformation = 19,
  FileEndOfFileInformation = 20,
  FileAlternateNameInformation = 21,
  FileStreamInformation = 22,
  FileCompressionInformation = 28,
  FileObjectIdInformation = 29,
  FileReparsePointInformation = 33,
  FileNetworkOpenInformation = 34,
  FileAttributeTagInformation = 35,
  FileIdBothDirectoryInformation = 37,
  FileIdFullDirectoryInformation = 38,
  FileValidDataLengthInformation = 39,
  FileShortNameInformation = 40,
  FileHardLinkInformation = 46,
  FileLinksInformation = 46,
  FileNormalizedNameInformation = 48,
  FileStandardLinkInformation = 54,
  FileIdInformation = 59,
  FileIdExtdDirectoryInformation = 60,
  FileDispositionInformationEx = 64,
  FileRenameInformationEx = 65,
  FileStatInformation = 68,
  FileStatLxInformation = 70,
  FileCaseSensitiveInformation = 71,
  FileLinkInformationEx = 72,
};

/// \brief [MS-FSCC] Section 2.5 File System Information Classes.
enum class FsInfoClass : ULONG {
  FileFsVolumeInformation = 1,
  FileFsLabelInformation = 2,
  FileFsSizeInformation = 3,
  FileFsDeviceInformation = 4,
  FileFsAttributeInformation = 5,
  FileFsControlInformation = 6,
  FileFsFullSizeInformation = 7,
  FileFsObjectIdInformation = 8,
  FileFsDriverPathInformation = 9,
  FileFsVolumeFlagsInformation = 10,
  FileFsSectorSizeInformation = 11,
};

// ---------------------------------------------------------------------------
// Core NT ABI Structures
// ---------------------------------------------------------------------------
/// \brief Native NT `UNICODE_STRING` layout.
struct UnicodeString {
  USHORT length = 0;
  USHORT maximum_length = 0;
  PWSTR buffer = nullptr;
};

/// \brief Native NT `OBJECT_ATTRIBUTES` layout.
struct ObjectAttributes {
  ULONG length = sizeof(ObjectAttributes);
  HANDLE root_directory = nullptr;
  UnicodeString *object_name = nullptr;
  ULONG attributes = 0;
  PVOID security_descriptor = nullptr;
  PVOID security_quality_of_service = nullptr;
};

/// \brief Native NT `IO_STATUS_BLOCK` layout.
struct IoStatusBlock {
  union {
    NTSTATUS status;
    PVOID pointer;
  };
  ULONG_PTR information = 0;
};

/// \brief Builds a `UnicodeString` referencing \p buffer of \p wchar_count
/// UTF-16 code units (caller must ensure `wchar_count <= kMaxUnicodeStringChars`).
inline UnicodeString MakeUnicodeString(wchar_t *buffer,
                                       size_t wchar_count) noexcept {
  UnicodeString us{};
  const size_t byte_len = wchar_count * sizeof(wchar_t);
  us.length = static_cast<USHORT>(byte_len);
  us.maximum_length = static_cast<USHORT>(byte_len + sizeof(wchar_t));
  us.buffer = buffer;
  return us;
}

/// \brief Initializes an `ObjectAttributes` structure for native NT calls.
inline ObjectAttributes
MakeObjectAttributes(UnicodeString *object_name,
                     ULONG attributes = kObjCaseInsensitive,
                     HANDLE root_directory = nullptr,
                     PVOID security_descriptor = nullptr) noexcept {
  ObjectAttributes oa{};
  oa.length = sizeof(ObjectAttributes);
  oa.root_directory = root_directory;
  oa.object_name = object_name;
  oa.attributes = attributes;
  oa.security_descriptor = security_descriptor;
  oa.security_quality_of_service = nullptr;
  return oa;
}

// ---------------------------------------------------------------------------
// File Information Structures ([MS-FSCC] Section 2.4 & WDK <ntifs.h>)
// ---------------------------------------------------------------------------
/// \brief 128-bit NTFS file identifier (`FILE_ID_128`).
struct FileId128 {
  UCHAR identifier[16];
};

/// \brief [MS-FSCC] Section 2.4.7 `FileBasicInformation`.
struct FileBasicInfo {
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  ULONG file_attributes;
};

/// \brief [MS-FSCC] Section 2.4.47 `FileStandardInformation`.
struct FileStandardInfo {
  LARGE_INTEGER allocation_size;
  LARGE_INTEGER end_of_file;
  ULONG number_of_links;
  BOOLEAN delete_pending;
  BOOLEAN directory;
};

/// \brief [MS-FSCC] Section 2.4.27 `FileInternalInformation`.
struct FileInternalInfo {
  LARGE_INTEGER index_number;
};

/// \brief [MS-FSCC] Section 2.4.13 `FileEaInformation`.
struct FileEaInfo {
  ULONG ea_size;
};

/// \brief [MS-FSCC] Section 2.4.1 `FileAccessInformation`.
struct FileAccessInfo {
  ACCESS_MASK access_flags;
};

/// \brief [MS-FSCC] Section 2.4.40 `FilePositionInformation`.
struct FilePositionInfo {
  LARGE_INTEGER current_byte_offset;
};

/// \brief [MS-FSCC] Section 2.4.31 `FileModeInformation`.
struct FileModeInfo {
  ULONG mode;
};

/// \brief [MS-FSCC] Section 2.4.3 `FileAlignmentInformation`.
struct FileAlignmentInfo {
  ULONG alignment_requirement;
};

/// \brief [MS-FSCC] Section 2.4.32 `FileNameInformation` / Section 2.4.35
/// `FileNormalizedNameInformation`.
struct FileNameInfo {
  ULONG file_name_length;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.2 `FileAllInformation`.
struct FileAllInfo {
  FileBasicInfo basic_information;
  FileStandardInfo standard_information;
  FileInternalInfo internal_information;
  FileEaInfo ea_information;
  FileAccessInfo access_information;
  FilePositionInfo position_information;
  FileModeInfo mode_information;
  FileAlignmentInfo alignment_information;
  FileNameInfo name_information;
};

/// \brief [MS-FSCC] Section 2.4.4 `FileAllocationInformation`.
struct FileAllocationInfo {
  LARGE_INTEGER allocation_size;
};

/// \brief [MS-FSCC] Section 2.4.14 `FileEndOfFileInformation`.
struct FileEndOfFileInfo {
  LARGE_INTEGER end_of_file;
};

/// \brief [MS-FSCC] Section 2.4.50 `FileValidDataLengthInformation`.
struct FileValidDataLengthInfo {
  LARGE_INTEGER valid_data_length;
};

/// \brief [MS-FSCC] Section 2.4.11 `FileDispositionInformation`.
struct FileDispositionInfo {
  BOOLEAN delete_file;
};

/// \brief [MS-FSCC] Section 2.4.12 `FileDispositionInformationEx`.
struct FileDispositionInfoEx {
  ULONG flags;
};

/// \brief [MS-FSCC] Section 2.4.42 `FileRenameInformation` / Section 2.4.43
/// `FileRenameInformationEx`.
struct FileRenameInfo {
  union {
    BOOLEAN replace_if_exists;
    ULONG flags;
  };
  HANDLE root_directory;
  ULONG file_name_length;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.28 `FileLinkInformation` (and
/// `FileLinkInformationEx`, class 72).
struct FileLinkInfo {
  union {
    BOOLEAN replace_if_exists;
    ULONG flags;
  };
  HANDLE root_directory;
  ULONG file_name_length;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.10 `FileDirectoryInformation`.
struct FileDirectoryInfo {
  ULONG next_entry_offset;
  ULONG file_index;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER end_of_file;
  LARGE_INTEGER allocation_size;
  ULONG file_attributes;
  ULONG file_name_length;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.15 `FileFullDirectoryInformation`.
struct FileFullDirInfo {
  ULONG next_entry_offset;
  ULONG file_index;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER end_of_file;
  LARGE_INTEGER allocation_size;
  ULONG file_attributes;
  ULONG file_name_length;
  ULONG ea_size;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.8 `FileBothDirectoryInformation`.
struct FileBothDirInfo {
  ULONG next_entry_offset;
  ULONG file_index;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER end_of_file;
  LARGE_INTEGER allocation_size;
  ULONG file_attributes;
  ULONG file_name_length;
  ULONG ea_size;
  CCHAR short_name_length;
  WCHAR short_name[12];
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.33 `FileNamesInformation`.
struct FileNamesInfo {
  ULONG next_entry_offset;
  ULONG file_index;
  ULONG file_name_length;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.22 `FileIdBothDirectoryInformation`.
struct FileIdBothDirInfo {
  ULONG next_entry_offset;
  ULONG file_index;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER end_of_file;
  LARGE_INTEGER allocation_size;
  ULONG file_attributes;
  ULONG file_name_length;
  ULONG ea_size;
  CCHAR short_name_length;
  WCHAR short_name[12];
  LARGE_INTEGER file_id;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.24 `FileIdFullDirectoryInformation`.
struct FileIdFullDirInfo {
  ULONG next_entry_offset;
  ULONG file_index;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER end_of_file;
  LARGE_INTEGER allocation_size;
  ULONG file_attributes;
  ULONG file_name_length;
  ULONG ea_size;
  LARGE_INTEGER file_id;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.23 `FileIdExtdDirectoryInformation`.
struct FileIdExtdDirInfo {
  ULONG next_entry_offset;
  ULONG file_index;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER end_of_file;
  LARGE_INTEGER allocation_size;
  ULONG file_attributes;
  ULONG file_name_length;
  ULONG ea_size;
  ULONG reparse_point_tag;
  FileId128 file_id;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.49 `FileStreamInformation`.
struct FileStreamInfo {
  ULONG next_entry_offset;
  ULONG stream_name_length;
  LARGE_INTEGER stream_size;
  LARGE_INTEGER stream_allocation_size;
  WCHAR stream_name[1];
};

/// \brief [MS-FSCC] Section 2.4.9 `FileCompressionInformation`.
struct FileCompressionInfo {
  LARGE_INTEGER compressed_file_size;
  USHORT compression_format;
  UCHAR compression_unit_shift;
  UCHAR chunk_shift;
  UCHAR cluster_shift;
  UCHAR reserved[3];
};

/// \brief [MS-FSCC] Section 2.4.34 `FileNetworkOpenInformation`.
struct FileNetworkOpenInfo {
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER allocation_size;
  LARGE_INTEGER end_of_file;
  ULONG file_attributes;
};

/// \brief [MS-FSCC] Section 2.4.6 `FileAttributeTagInformation`.
struct FileAttributeTagInfo {
  ULONG file_attributes;
  ULONG reparse_tag;
};

/// \brief [MS-FSCC] Section 2.4.48 `FileStandardLinkInformation`.
struct FileStandardLinkInfo {
  ULONG number_of_accessible_links;
  ULONG total_number_of_links;
  BOOLEAN delete_pending;
  BOOLEAN directory;
};

/// \brief [MS-FSCC] Section 2.4.17.1 `FILE_LINK_ENTRY_INFORMATION` (WDK
/// `<ntifs.h>`). Note: `file_name_length` is in `WCHAR` characters, not bytes.
struct FileLinkEntryInfo {
  ULONG next_entry_offset;
  LONGLONG parent_file_id;
  ULONG file_name_length;
  WCHAR file_name[1];
};

/// \brief [MS-FSCC] Section 2.4.17 `FileHardLinkInformation` /
/// `FILE_LINKS_INFORMATION` (WDK `<ntifs.h>`, class 46).
struct FileLinksInfo {
  ULONG bytes_needed;
  ULONG entries_returned;
  FileLinkEntryInfo entry;
};

/// \brief [MS-FSCC] Section 2.4.26 `FileIdInformation`.
struct FileIdInfo {
  ULONGLONG volume_serial_number;
  FileId128 file_id;
};

/// \brief WDK `<ntifs.h>` `FILE_STAT_INFORMATION` (`FileStatInformation` = 68,
/// Win10 1709+).
struct FileStatInfo {
  LARGE_INTEGER file_id;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER allocation_size;
  LARGE_INTEGER end_of_file;
  ULONG file_attributes;
  ULONG reparse_tag;
  ULONG number_of_links;
  ACCESS_MASK effective_access;
};

/// \brief WDK `<ntifs.h>` `FILE_STAT_LX_INFORMATION` (`FileStatLxInformation`
/// = 70, Win10 1709+).
struct FileStatLxInfo {
  LARGE_INTEGER file_id;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER allocation_size;
  LARGE_INTEGER end_of_file;
  ULONG file_attributes;
  ULONG reparse_tag;
  ULONG number_of_links;
  ACCESS_MASK effective_access;
  ULONG lx_flags;
  ULONG lx_uid;
  ULONG lx_gid;
  ULONG lx_mode;
  ULONG lx_device_id_major;
  ULONG lx_device_id_minor;
};

/// \brief WDK `<ntifs.h>` `FILE_CASE_SENSITIVE_INFORMATION`
/// (`FileCaseSensitiveInformation` = 71, Win10 1803+).
struct FileCaseSensitiveInfo {
  ULONG flags;
};

/// \brief [MS-FSCC] Section 2.4.16 `FileFullEaInformation`.
struct FileFullEaInfo {
  ULONG next_entry_offset;
  UCHAR flags;
  UCHAR ea_name_length;
  USHORT ea_value_length;
  CHAR ea_name[1];
};

/// \brief [MS-FSCC] Section 2.4.16.1 `FILE_GET_EA_INFORMATION`.
struct FileGetEaInfo {
  ULONG next_entry_offset;
  UCHAR ea_name_length;
  CHAR ea_name[1];
};

/// \brief [MS-FSCC] Section 2.7.2 `FILE_NOTIFY_EXTENDED_INFORMATION` (Win10
/// 1709+).
struct FileNotifyExtendedInfo {
  ULONG next_entry_offset;
  ULONG action;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_modification_time;
  LARGE_INTEGER last_change_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER allocated_length;
  LARGE_INTEGER file_size;
  ULONG file_attributes;
  ULONG reparse_point_tag;
  LARGE_INTEGER file_id;
  LARGE_INTEGER parent_file_id;
  ULONG file_name_length;
  WCHAR file_name[1];
};

// ---------------------------------------------------------------------------
// Volume Information Structures ([MS-FSCC] Section 2.5)
// ---------------------------------------------------------------------------

/// \brief [MS-FSCC] Section 2.5.9 `FileFsVolumeInformation`.
struct FileFsVolumeInfo {
  LARGE_INTEGER volume_creation_time;
  ULONG volume_serial_number;
  ULONG volume_label_length;
  BOOLEAN supports_objects;
  WCHAR volume_label[1];
};

/// \brief [MS-FSCC] Section 2.5.8 `FileFsSizeInformation`.
struct FileFsSizeInfo {
  LARGE_INTEGER total_allocation_units;
  LARGE_INTEGER available_allocation_units;
  ULONG sectors_per_allocation_unit;
  ULONG bytes_per_sector;
};

/// \brief [MS-FSCC] Section 2.5.10 `FileFsDeviceInformation`.
struct FileFsDeviceInfo {
  ULONG device_type;
  ULONG characteristics;
};

/// \brief [MS-FSCC] Section 2.5.1 `FileFsAttributeInformation`.
struct FileFsAttributeInfo {
  ULONG file_system_attributes;
  LONG maximum_component_name_length;
  ULONG file_system_name_length;
  WCHAR file_system_name[1];
};

/// \brief [MS-FSCC] Section 2.5.4 `FileFsFullSizeInformation`.
struct FileFsFullSizeInfo {
  LARGE_INTEGER total_allocation_units;
  LARGE_INTEGER caller_available_allocation_units;
  LARGE_INTEGER actual_available_allocation_units;
  ULONG sectors_per_allocation_unit;
  ULONG bytes_per_sector;
};

/// \brief [MS-FSCC] Section 2.5.7 `FileFsSectorSizeInformation`.
struct FileFsSectorSizeInfo {
  ULONG logical_bytes_per_sector;
  ULONG physical_bytes_per_sector_for_atomicity;
  ULONG physical_bytes_per_sector_for_performance;
  ULONG file_system_effective_physical_bytes_per_sector_for_atomicity;
  ULONG flags;
  ULONG byte_offset_for_sector_alignment;
  ULONG byte_offset_for_partition_alignment;
};

// ---------------------------------------------------------------------------
// Reparse Point Structures ([MS-FSCC] Section 2.1.2)
// ---------------------------------------------------------------------------

/// \brief [MS-FSCC] Section 2.1.2 `REPARSE_DATA_BUFFER` (Section 2.1.2.4
/// Symbolic Link, Section 2.1.2.5 Mount Point).
struct ReparseDataBuffer {
  ULONG reparse_tag;
  USHORT reparse_data_length;
  USHORT reserved;
  union {
    struct {
      USHORT substitute_name_offset;
      USHORT substitute_name_length;
      USHORT print_name_offset;
      USHORT print_name_length;
      ULONG flags;
      WCHAR path_buffer[1];
    } symbolic_link;
    struct {
      USHORT substitute_name_offset;
      USHORT substitute_name_length;
      USHORT print_name_offset;
      USHORT print_name_length;
      WCHAR path_buffer[1];
    } mount_point;
    struct {
      UCHAR data_buffer[1];
    } generic;
  };
};

/// \brief [MS-FSCC] Section 2.1.2.3 `REPARSE_GUID_DATA_BUFFER`.
struct ReparseGuidDataBuffer {
  ULONG reparse_tag;
  USHORT reparse_data_length;
  USHORT reserved;
  GUID reparse_guid;
  UCHAR data_buffer[1];
};

/// \brief Fixed header byte size preceding the payload of ReparseDataBuffer.
inline constexpr size_t kReparseDataBufferHeaderSize =
    offsetof(ReparseDataBuffer, generic);
/// \brief Fixed header byte size preceding the payload of ReparseGuidDataBuffer.
inline constexpr size_t kReparseGuidDataBufferHeaderSize =
    offsetof(ReparseGuidDataBuffer, data_buffer);

// ---------------------------------------------------------------------------
// Compile-time ABI Layout Verification (64-bit and 32-bit Windows)
// ---------------------------------------------------------------------------
static_assert(sizeof(UnicodeString) == 2 * sizeof(void *));
static_assert(sizeof(ObjectAttributes) == 6 * sizeof(void *));
static_assert(sizeof(IoStatusBlock) == 2 * sizeof(void *));

static_assert(sizeof(FileBasicInfo) == 40);
static_assert(offsetof(FileBasicInfo, file_attributes) == 32);
static_assert(sizeof(FileStandardInfo) == 24);
static_assert(offsetof(FileStandardInfo, delete_pending) == 20);
static_assert(offsetof(FileStandardInfo, directory) == 21);
static_assert(sizeof(FileInternalInfo) == 8);
static_assert(sizeof(FileAllInfo) == 104);
static_assert(offsetof(FileAllInfo, standard_information) == 40);
static_assert(offsetof(FileAllInfo, name_information) == 96);

static_assert(sizeof(FileDispositionInfo) == 1);
static_assert(sizeof(FileDispositionInfoEx) == 4);
static_assert(offsetof(FileRenameInfo, root_directory) == sizeof(void *));
static_assert(offsetof(FileRenameInfo, file_name_length) == 2 * sizeof(void *));
static_assert(offsetof(FileRenameInfo, file_name) == 2 * sizeof(void *) + 4);
static_assert(offsetof(FileLinkInfo, root_directory) == sizeof(void *));
static_assert(offsetof(FileLinkInfo, file_name) == 2 * sizeof(void *) + 4);

static_assert(offsetof(FileDirectoryInfo, file_name) == 64);
static_assert(offsetof(FileFullDirInfo, file_name) == 68);
static_assert(offsetof(FileBothDirInfo, short_name) == 70);
static_assert(offsetof(FileBothDirInfo, file_name) == 94);
static_assert(offsetof(FileNamesInfo, file_name) == 12);
static_assert(offsetof(FileIdBothDirInfo, file_id) == 96);
static_assert(offsetof(FileIdBothDirInfo, file_name) == 104);
static_assert(offsetof(FileIdFullDirInfo, file_id) == 72);
static_assert(offsetof(FileIdFullDirInfo, file_name) == 80);
static_assert(offsetof(FileIdExtdDirInfo, file_id) == 72);
static_assert(offsetof(FileIdExtdDirInfo, file_name) == 88);

static_assert(offsetof(FileStreamInfo, stream_size) == 8);
static_assert(offsetof(FileStreamInfo, stream_name) == 24);
static_assert(sizeof(FileCompressionInfo) == 16);
static_assert(sizeof(FileNetworkOpenInfo) == 56);
static_assert(sizeof(FileAttributeTagInfo) == 8);
static_assert(sizeof(FileStandardLinkInfo) == 12);
static_assert(offsetof(FileLinkEntryInfo, parent_file_id) == 8);
static_assert(offsetof(FileLinkEntryInfo, file_name_length) == 16);
static_assert(offsetof(FileLinkEntryInfo, file_name) == 20);
static_assert(sizeof(FileLinkEntryInfo) == 24);
static_assert(offsetof(FileLinksInfo, entries_returned) == 4);
static_assert(offsetof(FileLinksInfo, entry) == 8);
static_assert(sizeof(FileLinksInfo) == 32);
static_assert(sizeof(FileIdInfo) == 24);
static_assert(sizeof(FileStatInfo) == 72);
static_assert(sizeof(FileStatLxInfo) == 96);
static_assert(sizeof(FileCaseSensitiveInfo) == 4);
static_assert(offsetof(FileFullEaInfo, ea_name) == 8);
static_assert(offsetof(FileGetEaInfo, ea_name) == 5);
static_assert(offsetof(FileNotifyExtendedInfo, file_id) == 64);
static_assert(offsetof(FileNotifyExtendedInfo, parent_file_id) == 72);
static_assert(offsetof(FileNotifyExtendedInfo, file_name_length) == 80);
static_assert(offsetof(FileNotifyExtendedInfo, file_name) == 84);
static_assert(sizeof(FileNotifyExtendedInfo) == 88);

static_assert(offsetof(FileFsVolumeInfo, volume_label) == 18);
static_assert(sizeof(FileFsSizeInfo) == 24);
static_assert(sizeof(FileFsDeviceInfo) == 8);
static_assert(offsetof(FileFsAttributeInfo, file_system_name) == 12);
static_assert(sizeof(FileFsFullSizeInfo) == 32);
static_assert(sizeof(FileFsSectorSizeInfo) == 28);

static_assert(kReparseDataBufferHeaderSize == 8);
static_assert(kReparseGuidDataBufferHeaderSize == 24);

} // namespace fstest::nt
