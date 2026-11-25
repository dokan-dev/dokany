// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file file_ops.h
/// \brief Win32 and native NT (`ntdll`) filesystem operation wrappers for
/// opening, I/O, metadata queries/mutations, sparse/compression FSCTLs,
/// directory/stream enumeration, reparse points, and Extended Attributes.

#pragma once

#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/status.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest::core {

/// \brief Share-mode bitmask permitting concurrent read, write, and delete.
inline constexpr DWORD kShareAll =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

/// \brief Result of CreateWin32FileEx preserving `GetLastError()` on success
/// (e.g., `ERROR_ALREADY_EXISTS` for `CREATE_ALWAYS` / `OPEN_ALWAYS`).
struct Win32OpenResult {
  UniqueHandle handle;              ///< Opened handle (invalid on failure).
  DWORD last_error = ERROR_SUCCESS; ///< Preserved `GetLastError()` code.

  /// \brief Returns true iff a valid handle was returned.
  bool ok() const noexcept { return handle.valid(); }

  /// \brief Evaluates true iff ok() is true.
  explicit operator bool() const noexcept { return ok(); }
};

/// \brief Parsed entry from Win32 `FindFirstStreamW` or NT
/// `FileStreamInformation` enumeration.
struct StreamEntry {
  std::wstring name; ///< Stream name (e.g. `::$DATA` or `:alt:$DATA`).
  int64_t size = 0;  ///< Stream end-of-file length in bytes.
  int64_t allocation_size = 0; ///< Stream allocation size in bytes.
};

/// \brief Parsed or serialized NTFS Extended Attribute (`FILE_FULL_EA_INFORMATION`).
struct EaEntry {
  UCHAR flags = 0;            ///< EA flags (e.g., `nt::kFileNeedEa`).
  std::string name;           ///< ASCII EA name (up to 255 bytes).
  std::vector<uint8_t> value; ///< Raw binary EA value payload.
};

/// \brief Direct `NtCreateFile` wrapper using an explicit NT path or a
/// handle-relative path.
///
/// Rejects paths of length `>= 32767` UTF-16 code units with
/// `STATUS_NAME_TOO_LONG` before populating `UNICODE_STRING`.
/// \param nt_path Full NT path (`\??\...`) or relative path when
/// \p root_directory is non-null.
/// \param desired_access NT `ACCESS_MASK`.
/// \param file_attributes `FILE_ATTRIBUTE_*` flags.
/// \param share_access `FILE_SHARE_*` mask.
/// \param create_disposition `nt::kFile*` disposition constant.
/// \param create_options `nt::kFile*` create options bitmask.
/// \param root_directory Optional directory handle for relative opens.
/// \param ea_buffer Optional `FILE_FULL_EA_INFORMATION` buffer.
/// \param ea_length Byte length of \p ea_buffer.
/// \param allocation_size Optional initial allocation size.
/// \param obj_attributes `OBJ_*` flags (default `nt::kObjCaseInsensitive`).
/// \param security_descriptor Optional `SECURITY_DESCRIPTOR` pointer.
/// \return Opened UniqueHandle, NTSTATUS, and `IoStatusBlock.information`.
NtResult<UniqueHandle>
NtCreateFilePath(std::wstring_view nt_path, ACCESS_MASK desired_access,
                 ULONG file_attributes, ULONG share_access,
                 ULONG create_disposition, ULONG create_options,
                 HANDLE root_directory = nullptr, void *ea_buffer = nullptr,
                 ULONG ea_length = 0, PLARGE_INTEGER allocation_size = nullptr,
                 ULONG obj_attributes = nt::kObjCaseInsensitive,
                 void *security_descriptor = nullptr);

/// \brief Direct `NtOpenFile` wrapper.
///
/// Rejects paths of length `>= 32767` UTF-16 code units with
/// `STATUS_NAME_TOO_LONG`.
/// \param nt_path Full NT path (`\??\...`) or relative path.
/// \param desired_access NT `ACCESS_MASK`.
/// \param share_access `FILE_SHARE_*` mask.
/// \param open_options `nt::kFile*` open options bitmask.
/// \param root_directory Optional directory handle for relative opens.
/// \param obj_attributes `OBJ_*` flags (default `nt::kObjCaseInsensitive`).
/// \return Opened UniqueHandle, NTSTATUS, and `IoStatusBlock.information`.
NtResult<UniqueHandle>
NtOpenFilePath(std::wstring_view nt_path, ACCESS_MASK desired_access,
               ULONG share_access, ULONG open_options,
               HANDLE root_directory = nullptr,
               ULONG obj_attributes = nt::kObjCaseInsensitive);

/// \brief Opens or creates a child entry relative to an open directory handle.
/// \param dir_handle Open parent directory handle.
/// \param rel_path Child relative path.
/// \param desired_access NT `ACCESS_MASK`.
/// \param share_access `FILE_SHARE_*` mask.
/// \param create_disposition `nt::kFile*` disposition constant.
/// \param create_options `nt::kFile*` create options bitmask.
/// \param file_attributes `FILE_ATTRIBUTE_*` flags.
/// \return Opened UniqueHandle and NTSTATUS result.
NtResult<UniqueHandle>
OpenRelative(HANDLE dir_handle, std::wstring_view rel_path,
             ACCESS_MASK desired_access, ULONG share_access,
             ULONG create_disposition, ULONG create_options,
             ULONG file_attributes = FILE_ATTRIBUTE_NORMAL);

/// \brief Explicit `CreateFileW` RAII wrapper returning a Win32Result.
/// \param path File or directory path.
/// \param desired_access Win32 desired access mask.
/// \param share_mode `FILE_SHARE_*` bitmask.
/// \param creation_disposition `CREATE_*` / `OPEN_*` / `TRUNCATE_EXISTING`.
/// \param flags_and_attributes `FILE_ATTRIBUTE_*` and `FILE_FLAG_*` bitmask.
/// \param security_attributes Optional `SECURITY_ATTRIBUTES`.
/// \return Owned UniqueHandle on success, or Win32 error code.
Win32Result<UniqueHandle>
CreateWin32File(std::wstring_view path, DWORD desired_access, DWORD share_mode,
                DWORD creation_disposition,
                DWORD flags_and_attributes = FILE_ATTRIBUTE_NORMAL,
                LPSECURITY_ATTRIBUTES security_attributes = nullptr);

/// \brief Explicit `CreateFileW` RAII wrapper that preserves `GetLastError()`
/// even on success (e.g., `ERROR_ALREADY_EXISTS` for `CREATE_ALWAYS` /
/// `OPEN_ALWAYS`).
Win32OpenResult
CreateWin32FileEx(std::wstring_view path, DWORD desired_access,
                  DWORD share_mode, DWORD creation_disposition,
                  DWORD flags_and_attributes = FILE_ATTRIBUTE_NORMAL,
                  LPSECURITY_ATTRIBUTES security_attributes = nullptr);

/// \name Synchronous Read/Write Helpers
/// \{

/// \brief Writes all \p size bytes from \p data to handle \p h in chunks.
Win32Result<void> WriteAllBytes(HANDLE h, const void *data, size_t size);

/// \brief Creates or overwrites \p path and writes \p data completely.
Win32Result<void>
WriteFileContents(std::wstring_view path, std::string_view data,
                  DWORD flags_and_attributes = FILE_ATTRIBUTE_NORMAL);

/// \brief Reads from handle \p h until EOF and returns the accumulated bytes.
Win32Result<std::string> ReadAllBytes(HANDLE h);

/// \brief Opens \p path for reading and returns its complete contents.
Win32Result<std::string> ReadFileContents(std::wstring_view path);
/// \}

/// \name NT Metadata Query and Set Helpers
/// \{

/// \brief Queries `FileBasicInformation` via `NtQueryInformationFile`.
NtResult<nt::FileBasicInfo> QueryBasicInfo(HANDLE h);

/// \brief Sets `FileBasicInformation` via `NtSetInformationFile`.
NtResult<void> SetBasicInfo(HANDLE h, const nt::FileBasicInfo &info);

/// \brief Queries `FileStandardInformation` via `NtQueryInformationFile`.
NtResult<nt::FileStandardInfo> QueryStandardInfo(HANDLE h);

/// \brief Queries `FileInternalInformation` via `NtQueryInformationFile`.
NtResult<nt::FileInternalInfo> QueryInternalInfo(HANDLE h);

/// \brief Queries `FileStandardLinkInformation` via `NtQueryInformationFile`.
NtResult<nt::FileStandardLinkInfo> QueryStandardLinkInfo(HANDLE h);

/// \brief Queries `FileHardLinkInformation` and returns `(parent_file_id,
/// file_name)` pairs for every link referencing the file.
NtResult<std::vector<std::pair<int64_t, std::wstring>>>
QueryHardLinkEntries(HANDLE h);

/// \brief Bounds-checked parser for the bytes returned by
/// `FileHardLinkInformation` (`FILE_LINKS_INFORMATION`).
///
/// Stops at `EntriesReturned`, a zero or out-of-range `NextEntryOffset`, or a
/// truncated entry header; clamps each `FileNameLength` (in WCHARs) to the
/// returned bytes.
/// \param returned Returned payload (`buffer.first(iosb.information)`).
/// \return `(parent_file_id, file_name)` pairs decoded before any corruption.
std::vector<std::pair<int64_t, std::wstring>>
ParseHardLinkBuffer(std::span<const uint8_t> returned);

/// \brief Queries `FileNameInformation` (or `FileNormalizedNameInformation`
/// when \p normalized is true) with bounded buffer growth.
NtResult<std::wstring> QueryFileNameInfo(HANDLE h, bool normalized = false);

/// \brief Bounds-checked parser for the bytes returned by
/// `FileNameInformation` / `FileNormalizedNameInformation`.
///
/// Clamps `FileNameLength` (in bytes) to the returned bytes and drops an odd
/// trailing byte.
/// \param returned Returned payload (`buffer.first(iosb.information)`).
/// \return File name, or `std::nullopt` when \p returned is shorter than the
/// 4-byte `FileNameLength` header.
std::optional<std::wstring>
ParseFileNameInfoBuffer(std::span<const uint8_t> returned);

/// \brief Sets `FileDispositionInformation` (`DeleteFile` boolean).
NtResult<void> SetDisposition(HANDLE h, bool delete_file);

/// \brief Sets `FileDispositionInformationEx` using `nt::kFileDisposition*`
/// flags.
NtResult<void> SetDispositionEx(HANDLE h, ULONG flags);

/// \brief Sets `FileRenameInformation` with legacy `ReplaceIfExists` flag.
NtResult<void> SetRenameInfo(HANDLE h, std::wstring_view target_name,
                             HANDLE root_dir, bool replace_if_exists);

/// \brief Sets `FileRenameInformationEx` with `nt::kFileRename*` flags.
NtResult<void> SetRenameInfoEx(HANDLE h, std::wstring_view target_name,
                               HANDLE root_dir, ULONG flags);

/// \brief Sets `FileLinkInformation` with legacy `ReplaceIfExists` flag.
NtResult<void> SetLinkInfo(HANDLE h, std::wstring_view target_name,
                           HANDLE root_dir, bool replace_if_exists);

/// \brief Sets `FileLinkInformationEx` with `nt::kFileLink*` flags.
NtResult<void> SetLinkInfoEx(HANDLE h, std::wstring_view target_name,
                             HANDLE root_dir, ULONG flags);

/// \brief Sets `FileEndOfFileInformation` to \p end_of_file bytes.
NtResult<void> SetEndOfFileNt(HANDLE h, int64_t end_of_file);

/// \brief Sets `FileAllocationInformation` to \p allocation_size bytes.
NtResult<void> SetAllocationSizeNt(HANDLE h, int64_t allocation_size);
/// \}

/// \name Sparse and Compression FSCTL Wrappers
/// \{

/// \brief Sends `FSCTL_SET_SPARSE` on handle \p h.
Win32Result<void> SetSparseFile(HANDLE h, bool sparse = true);

/// \brief Sends `FSCTL_SET_ZERO_DATA` for `[file_offset, beyond_final_zero)`.
Win32Result<void> SetZeroData(HANDLE h, int64_t file_offset,
                              int64_t beyond_final_zero);

/// \brief Paginates `FSCTL_QUERY_ALLOCATED_RANGES` across
/// `[offset, offset + length)` and returns all allocated extents.
Win32Result<std::vector<FILE_ALLOCATED_RANGE_BUFFER>>
QueryAllocatedRanges(HANDLE h, int64_t offset, int64_t length);

/// \brief Sends `FSCTL_SET_COMPRESSION` with \p format (e.g.,
/// `COMPRESSION_FORMAT_DEFAULT` or `COMPRESSION_FORMAT_NONE`).
Win32Result<void> SetCompressionState(HANDLE h, USHORT format);

/// \brief Queries compression format via `FSCTL_GET_COMPRESSION`.
Win32Result<USHORT> GetCompressionState(HANDLE h);
/// \}

/// \name Directory and Stream Enumeration Helpers
/// \{

/// \brief Enumerates \p dir_path matching \p pattern via `FindFirstFileW` /
/// `FindNextFileW`, strictly verifying terminal `ERROR_NO_MORE_FILES` and
/// preserving duplicates.
Win32Result<std::vector<std::wstring>>
ListDirectoryWin32(std::wstring_view dir_path,
                   std::wstring_view pattern = L"*");

/// \brief Enumerates streams via `FindFirstStreamW` / `FindNextStreamW`,
/// strictly verifying terminal `ERROR_HANDLE_EOF`.
Win32Result<std::vector<StreamEntry>> ListStreamsWin32(std::wstring_view path);

/// \brief Enumerates streams via `NtQueryInformationFile(FileStreamInformation)`.
NtResult<std::vector<StreamEntry>> ListStreamsNt(HANDLE h);

/// \brief Bounds-checked parser for the bytes returned by
/// `FileStreamInformation` (`FILE_STREAM_INFORMATION` chain).
///
/// Stops at a zero or out-of-range `NextEntryOffset` or a truncated entry
/// header; clamps each `StreamNameLength` (in bytes) to the returned bytes.
/// \param returned Returned payload (`buffer.first(iosb.information)`).
/// \return Stream entries decoded before any corruption.
std::vector<StreamEntry>
ParseStreamInfoBuffer(std::span<const uint8_t> returned);
/// \}

/// \name Symlink and Reparse Point Helpers
/// \{

/// \brief Creates a file or directory symbolic link via `CreateSymbolicLinkW`.
///
/// Guards `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` (`0x2`) with
/// `os_build >= 15063` so Windows Server 2016 (build 14393) does not reject
/// the flag with `ERROR_INVALID_PARAMETER`.
Win32Result<void> CreateSymlink(std::wstring_view link_path,
                                std::wstring_view target_path,
                                bool is_directory, uint32_t os_build);

/// \brief Creates a local directory junction (`IO_REPARSE_TAG_MOUNT_POINT`) at
/// \p junction_dir pointing to \p target_dir.
///
/// Creates \p junction_dir if absent (cleaning it up if setting the reparse
/// point fails) and validates total buffer size against
/// `MAXIMUM_REPARSE_DATA_BUFFER_SIZE`.
Win32Result<void> CreateDirectoryJunction(std::wstring_view junction_dir,
                                          std::wstring_view target_dir);

/// \brief Queries raw reparse point payload via `FSCTL_GET_REPARSE_POINT`.
Win32Result<std::vector<uint8_t>> GetReparsePointData(HANDLE h);

/// \brief Deletes a reparse point tag via `FSCTL_DELETE_REPARSE_POINT`.
Win32Result<void> DeleteReparsePoint(HANDLE h, ULONG reparse_tag,
                                     const GUID *reparse_guid = nullptr);
/// \}

/// \name Extended Attribute (EA) Serialization Helpers
/// \{

/// \brief Serializes \p entries into a 4-byte-aligned `FILE_FULL_EA_INFORMATION`
/// buffer suitable for `NtCreateFile` or `NtSetEaFile`.
///
/// Never truncates: an entry whose name exceeds 255 bytes (`EaNameLength` is a
/// `UCHAR`) or whose value exceeds 65535 bytes (`EaValueLength` is a `USHORT`)
/// makes the whole call fail.
/// \param entries EA entries to serialize.
/// \param error_out Optional diagnostic set when an entry exceeds the limits.
/// \return Serialized buffer, or an empty vector when \p entries is empty or
/// any entry exceeds the limits (check \p error_out to distinguish).
std::vector<uint8_t> BuildEaBuffer(const std::vector<EaEntry> &entries,
                                   std::string *error_out = nullptr);

/// \brief Bounds-checked parser for `FILE_FULL_EA_INFORMATION` buffers.
std::vector<EaEntry> ParseEaBuffer(const uint8_t *data, size_t size);
/// \}

/// \name Handle and Path Info Helpers
/// \{

/// \brief Wraps `GetFileInformationByHandle`.
Win32Result<BY_HANDLE_FILE_INFORMATION> GetByHandleInfo(HANDLE h);

/// \brief Returns the 64-bit file index for open handle \p h.
Win32Result<uint64_t> GetFileIndex(HANDLE h);

/// \brief Wraps `GetFileAttributesW` returning a Win32Result.
Win32Result<DWORD> QueryWin32Attributes(std::wstring_view path);

/// \brief Opens \p path for `FILE_READ_ATTRIBUTES` and queries
/// `BY_HANDLE_FILE_INFORMATION`.
Win32Result<BY_HANDLE_FILE_INFORMATION>
QueryInfoByPath(std::wstring_view path, bool backup_semantics = false);

/// \brief Queries the 64-bit file index of \p path.
Win32Result<uint64_t> QueryPathFileIndex(std::wstring_view path,
                                         bool backup_semantics = false);

/// \brief Queries `GetFinalPathNameByHandleW` with dynamic buffer growth.
Win32Result<std::wstring> QueryFinalPath(HANDLE handle,
                                         DWORD flags = FILE_NAME_NORMALIZED |
                                                       VOLUME_NAME_DOS);

/// \brief Combines `nFileIndexHigh` and `nFileIndexLow` into a 64-bit index.
inline constexpr uint64_t
FileIndexOf(const BY_HANDLE_FILE_INFORMATION &info) noexcept {
  return (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
         static_cast<uint64_t>(info.nFileIndexLow);
}

/// \brief Combines `nFileSizeHigh` and `nFileSizeLow` into a 64-bit size.
inline constexpr uint64_t
FileSizeOf(const BY_HANDLE_FILE_INFORMATION &info) noexcept {
  return (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
         static_cast<uint64_t>(info.nFileSizeLow);
}

/// \brief Converts a `FILETIME` to an unsigned 64-bit 100ns tick count.
inline constexpr uint64_t FileTimeToU64(FILETIME ft) noexcept {
  return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
         static_cast<uint64_t>(ft.dwLowDateTime);
}

/// \brief Converts a `FILETIME` to a signed 64-bit 100ns tick count.
inline constexpr int64_t FileTimeToInt64(FILETIME ft) noexcept {
  return static_cast<int64_t>(FileTimeToU64(ft));
}

/// \brief Constructs a UTC `FILETIME` from calendar components (zero on
/// invalid date).
FILETIME MakeUtcFileTime(WORD year, WORD month, WORD day, WORD hour = 0,
                         WORD minute = 0, WORD second = 0) noexcept;

/// \brief Constructs a signed 64-bit UTC `FILETIME` value from calendar
/// components.
int64_t MakeUtcFileTimeInt64(WORD year, WORD month, WORD day, WORD hour = 0,
                             WORD minute = 0, WORD second = 0) noexcept;
/// \}

/// \name Ordinal Wide-String Comparison Helpers
/// \{

/// \brief Performs ordinal case-insensitive equality comparison via
/// `CompareStringOrdinal`.
bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept;

/// \brief Returns true iff \p text ends with \p suffix (ordinal case-insensitive).
bool EndsWithIgnoreCase(std::wstring_view text,
                        std::wstring_view suffix) noexcept;

/// \brief Returns true iff \p text ends with \p suffix (exact code-unit match).
bool EndsWithExact(std::wstring_view text, std::wstring_view suffix) noexcept;
/// \}

} // namespace fstest::core
