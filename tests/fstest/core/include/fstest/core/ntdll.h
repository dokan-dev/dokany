// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file ntdll.h
/// \brief Dynamically resolved `ntdll.dll` native filesystem function table.

#pragma once

#include "fstest/core/nt_types.h"

namespace fstest::core {

/// \brief Table of `ntdll.dll` entry points resolved lazily via
/// `GetModuleHandleW(L"ntdll.dll")` and `GetProcAddress`.
struct NtdllApi {
  using FnNtCreateFile = NTSTATUS(NTAPI *)(
      PHANDLE file_handle, ACCESS_MASK desired_access,
      nt::ObjectAttributes *object_attributes,
      nt::IoStatusBlock *io_status_block, PLARGE_INTEGER allocation_size,
      ULONG file_attributes, ULONG share_access, ULONG create_disposition,
      ULONG create_options, PVOID ea_buffer, ULONG ea_length);

  using FnNtOpenFile =
      NTSTATUS(NTAPI *)(PHANDLE file_handle, ACCESS_MASK desired_access,
                        nt::ObjectAttributes *object_attributes,
                        nt::IoStatusBlock *io_status_block, ULONG share_access,
                        ULONG open_options);

  using FnNtClose = NTSTATUS(NTAPI *)(HANDLE handle);

  using FnNtDeleteFile =
      NTSTATUS(NTAPI *)(nt::ObjectAttributes *object_attributes);

  using FnNtReadFile = NTSTATUS(NTAPI *)(HANDLE file_handle, HANDLE event,
                                         PVOID apc_routine, PVOID apc_context,
                                         nt::IoStatusBlock *io_status_block,
                                         PVOID buffer, ULONG length,
                                         PLARGE_INTEGER byte_offset,
                                         PULONG key);

  using FnNtWriteFile = NTSTATUS(NTAPI *)(HANDLE file_handle, HANDLE event,
                                          PVOID apc_routine, PVOID apc_context,
                                          nt::IoStatusBlock *io_status_block,
                                          PVOID buffer, ULONG length,
                                          PLARGE_INTEGER byte_offset,
                                          PULONG key);

  using FnNtFlushBuffersFile =
      NTSTATUS(NTAPI *)(HANDLE file_handle, nt::IoStatusBlock *io_status_block);

  using FnNtFlushBuffersFileEx = NTSTATUS(NTAPI *)(
      HANDLE file_handle, ULONG flags, PVOID parameters, ULONG parameters_size,
      nt::IoStatusBlock *io_status_block);

  using FnNtQueryInformationFile = NTSTATUS(NTAPI *)(
      HANDLE file_handle, nt::IoStatusBlock *io_status_block,
      PVOID file_information, ULONG length, nt::FileInfoClass info_class);

  using FnNtSetInformationFile = NTSTATUS(NTAPI *)(
      HANDLE file_handle, nt::IoStatusBlock *io_status_block,
      PVOID file_information, ULONG length, nt::FileInfoClass info_class);

  using FnNtQueryDirectoryFile = NTSTATUS(NTAPI *)(
      HANDLE file_handle, HANDLE event, PVOID apc_routine, PVOID apc_context,
      nt::IoStatusBlock *io_status_block, PVOID file_information, ULONG length,
      nt::FileInfoClass info_class, BOOLEAN return_single_entry,
      nt::UnicodeString *file_name, BOOLEAN restart_scan);

  using FnNtQueryDirectoryFileEx = NTSTATUS(NTAPI *)(
      HANDLE file_handle, HANDLE event, PVOID apc_routine, PVOID apc_context,
      nt::IoStatusBlock *io_status_block, PVOID file_information, ULONG length,
      nt::FileInfoClass info_class, ULONG query_flags,
      nt::UnicodeString *file_name);

  using FnNtQueryVolumeInformationFile = NTSTATUS(NTAPI *)(
      HANDLE file_handle, nt::IoStatusBlock *io_status_block,
      PVOID fs_information, ULONG length, nt::FsInfoClass info_class);

  using FnNtSetVolumeInformationFile = NTSTATUS(NTAPI *)(
      HANDLE file_handle, nt::IoStatusBlock *io_status_block,
      PVOID fs_information, ULONG length, nt::FsInfoClass info_class);

  using FnNtQueryAttributesFile = NTSTATUS(NTAPI *)(
      nt::ObjectAttributes *object_attributes, nt::FileBasicInfo *file_info);

  using FnNtQueryFullAttributesFile =
      NTSTATUS(NTAPI *)(nt::ObjectAttributes *object_attributes,
                        nt::FileNetworkOpenInfo *file_info);

  using FnNtQuerySecurityObject = NTSTATUS(NTAPI *)(
      HANDLE handle, SECURITY_INFORMATION security_information,
      PSECURITY_DESCRIPTOR security_descriptor, ULONG length,
      PULONG length_needed);

  using FnNtSetSecurityObject = NTSTATUS(NTAPI *)(
      HANDLE handle, SECURITY_INFORMATION security_information,
      PSECURITY_DESCRIPTOR security_descriptor);

  using FnNtQueryEaFile = NTSTATUS(NTAPI *)(
      HANDLE file_handle, nt::IoStatusBlock *io_status_block, PVOID buffer,
      ULONG length, BOOLEAN return_single_entry, PVOID ea_list,
      ULONG ea_list_length, PULONG ea_index, BOOLEAN restart_scan);

  using FnNtSetEaFile = NTSTATUS(NTAPI *)(HANDLE file_handle,
                                          nt::IoStatusBlock *io_status_block,
                                          PVOID buffer, ULONG length);

  using FnNtLockFile = NTSTATUS(NTAPI *)(HANDLE file_handle, HANDLE event,
                                         PVOID apc_routine, PVOID apc_context,
                                         nt::IoStatusBlock *io_status_block,
                                         PLARGE_INTEGER byte_offset,
                                         PLARGE_INTEGER length, ULONG key,
                                         BOOLEAN fail_immediately,
                                         BOOLEAN exclusive_lock);

  using FnNtUnlockFile = NTSTATUS(NTAPI *)(HANDLE file_handle,
                                           nt::IoStatusBlock *io_status_block,
                                           PLARGE_INTEGER byte_offset,
                                           PLARGE_INTEGER length, ULONG key);

  using FnNtFsControlFile = NTSTATUS(NTAPI *)(
      HANDLE file_handle, HANDLE event, PVOID apc_routine, PVOID apc_context,
      nt::IoStatusBlock *io_status_block, ULONG fs_control_code,
      PVOID input_buffer, ULONG input_buffer_length, PVOID output_buffer,
      ULONG output_buffer_length);

  using FnNtCreateSection = NTSTATUS(NTAPI *)(
      PHANDLE section_handle, ACCESS_MASK desired_access,
      nt::ObjectAttributes *object_attributes, PLARGE_INTEGER maximum_size,
      ULONG section_page_protection, ULONG allocation_attributes,
      HANDLE file_handle);

  using FnRtlGetVersion =
      NTSTATUS(NTAPI *)(PRTL_OSVERSIONINFOEXW version_information);

  using FnRtlNtStatusToDosError = ULONG(NTAPI *)(NTSTATUS status);

  using FnRtlDosPathNameToNtPathNameUWithStatus =
      NTSTATUS(NTAPI *)(PCWSTR dos_file_name, nt::UnicodeString *nt_file_name,
                        PWSTR *file_part, PVOID reserved);

  using FnRtlFreeUnicodeString =
      VOID(NTAPI *)(nt::UnicodeString *unicode_string);

  FnNtCreateFile NtCreateFile = nullptr;
  FnNtOpenFile NtOpenFile = nullptr;
  FnNtClose NtClose = nullptr;
  FnNtDeleteFile NtDeleteFile = nullptr;
  FnNtReadFile NtReadFile = nullptr;
  FnNtWriteFile NtWriteFile = nullptr;
  FnNtFlushBuffersFile NtFlushBuffersFile = nullptr;
  FnNtFlushBuffersFileEx NtFlushBuffersFileEx = nullptr;
  FnNtQueryInformationFile NtQueryInformationFile = nullptr;
  FnNtSetInformationFile NtSetInformationFile = nullptr;
  FnNtQueryDirectoryFile NtQueryDirectoryFile = nullptr;
  FnNtQueryDirectoryFileEx NtQueryDirectoryFileEx = nullptr;
  FnNtQueryVolumeInformationFile NtQueryVolumeInformationFile = nullptr;
  FnNtSetVolumeInformationFile NtSetVolumeInformationFile = nullptr;
  FnNtQueryAttributesFile NtQueryAttributesFile = nullptr;
  FnNtQueryFullAttributesFile NtQueryFullAttributesFile = nullptr;
  FnNtQuerySecurityObject NtQuerySecurityObject = nullptr;
  FnNtSetSecurityObject NtSetSecurityObject = nullptr;
  FnNtQueryEaFile NtQueryEaFile = nullptr;
  FnNtSetEaFile NtSetEaFile = nullptr;
  FnNtLockFile NtLockFile = nullptr;
  FnNtUnlockFile NtUnlockFile = nullptr;
  FnNtFsControlFile NtFsControlFile = nullptr;
  FnNtCreateSection NtCreateSection = nullptr;
  FnRtlGetVersion RtlGetVersion = nullptr;
  FnRtlNtStatusToDosError RtlNtStatusToDosError = nullptr;
  FnRtlDosPathNameToNtPathNameUWithStatus
      RtlDosPathNameToNtPathName_U_WithStatus = nullptr;
  FnRtlFreeUnicodeString RtlFreeUnicodeString = nullptr;
};

/// \brief Returns the process-wide lazily initialized table of `ntdll.dll`
/// entry points.
/// \return Const reference to the resolved NtdllApi table.
const NtdllApi &Ntdll() noexcept;

} // namespace fstest::core
