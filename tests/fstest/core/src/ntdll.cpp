// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/ntdll.h"

#include <cstring>

namespace fstest::core {
namespace {

template <typename Fn> Fn LoadProc(HMODULE mod, const char *name) noexcept {
  if (mod == nullptr) {
    return nullptr;
  }
  FARPROC proc = ::GetProcAddress(mod, name);
  if (proc == nullptr) {
    return nullptr;
  }
  Fn fn = nullptr;
  static_assert(sizeof(fn) == sizeof(proc));
  std::memcpy(&fn, &proc, sizeof(fn));
  return fn;
}

NtdllApi InitNtdllApi() noexcept {
  NtdllApi api{};
  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return api;
  }

  api.NtCreateFile = LoadProc<NtdllApi::FnNtCreateFile>(ntdll, "NtCreateFile");
  api.NtOpenFile = LoadProc<NtdllApi::FnNtOpenFile>(ntdll, "NtOpenFile");
  api.NtClose = LoadProc<NtdllApi::FnNtClose>(ntdll, "NtClose");
  api.NtDeleteFile = LoadProc<NtdllApi::FnNtDeleteFile>(ntdll, "NtDeleteFile");
  api.NtReadFile = LoadProc<NtdllApi::FnNtReadFile>(ntdll, "NtReadFile");
  api.NtWriteFile = LoadProc<NtdllApi::FnNtWriteFile>(ntdll, "NtWriteFile");
  api.NtFlushBuffersFile =
      LoadProc<NtdllApi::FnNtFlushBuffersFile>(ntdll, "NtFlushBuffersFile");
  api.NtFlushBuffersFileEx =
      LoadProc<NtdllApi::FnNtFlushBuffersFileEx>(ntdll, "NtFlushBuffersFileEx");
  api.NtQueryInformationFile = LoadProc<NtdllApi::FnNtQueryInformationFile>(
      ntdll, "NtQueryInformationFile");
  api.NtSetInformationFile =
      LoadProc<NtdllApi::FnNtSetInformationFile>(ntdll, "NtSetInformationFile");
  api.NtQueryDirectoryFile =
      LoadProc<NtdllApi::FnNtQueryDirectoryFile>(ntdll, "NtQueryDirectoryFile");
  api.NtQueryDirectoryFileEx = LoadProc<NtdllApi::FnNtQueryDirectoryFileEx>(
      ntdll, "NtQueryDirectoryFileEx");
  api.NtQueryVolumeInformationFile =
      LoadProc<NtdllApi::FnNtQueryVolumeInformationFile>(
          ntdll, "NtQueryVolumeInformationFile");
  api.NtSetVolumeInformationFile =
      LoadProc<NtdllApi::FnNtSetVolumeInformationFile>(
          ntdll, "NtSetVolumeInformationFile");
  api.NtQueryAttributesFile = LoadProc<NtdllApi::FnNtQueryAttributesFile>(
      ntdll, "NtQueryAttributesFile");
  api.NtQueryFullAttributesFile =
      LoadProc<NtdllApi::FnNtQueryFullAttributesFile>(
          ntdll, "NtQueryFullAttributesFile");
  api.NtQuerySecurityObject = LoadProc<NtdllApi::FnNtQuerySecurityObject>(
      ntdll, "NtQuerySecurityObject");
  api.NtSetSecurityObject =
      LoadProc<NtdllApi::FnNtSetSecurityObject>(ntdll, "NtSetSecurityObject");
  api.NtQueryEaFile =
      LoadProc<NtdllApi::FnNtQueryEaFile>(ntdll, "NtQueryEaFile");
  api.NtSetEaFile = LoadProc<NtdllApi::FnNtSetEaFile>(ntdll, "NtSetEaFile");
  api.NtLockFile = LoadProc<NtdllApi::FnNtLockFile>(ntdll, "NtLockFile");
  api.NtUnlockFile = LoadProc<NtdllApi::FnNtUnlockFile>(ntdll, "NtUnlockFile");
  api.NtFsControlFile =
      LoadProc<NtdllApi::FnNtFsControlFile>(ntdll, "NtFsControlFile");
  api.NtCreateSection =
      LoadProc<NtdllApi::FnNtCreateSection>(ntdll, "NtCreateSection");
  api.RtlGetVersion =
      LoadProc<NtdllApi::FnRtlGetVersion>(ntdll, "RtlGetVersion");
  api.RtlNtStatusToDosError = LoadProc<NtdllApi::FnRtlNtStatusToDosError>(
      ntdll, "RtlNtStatusToDosError");
  api.RtlDosPathNameToNtPathName_U_WithStatus =
      LoadProc<NtdllApi::FnRtlDosPathNameToNtPathNameUWithStatus>(
          ntdll, "RtlDosPathNameToNtPathName_U_WithStatus");
  api.RtlFreeUnicodeString =
      LoadProc<NtdllApi::FnRtlFreeUnicodeString>(ntdll, "RtlFreeUnicodeString");

  return api;
}

} // namespace

const NtdllApi &Ntdll() noexcept {
  static const NtdllApi kApi = InitNtdllApi();
  return kApi;
}

} // namespace fstest::core
