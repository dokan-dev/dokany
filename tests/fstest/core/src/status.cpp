// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "fstest/core/status.h"

#include "fstest/core/handle.h"

#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>

namespace fstest::core {
namespace {

struct NtStatusEntry {
  NTSTATUS code;
  const char *name;
};

constexpr NtStatusEntry kNtStatusTable[] = {
    {STATUS_SUCCESS, "STATUS_SUCCESS"},
    {STATUS_PENDING, "STATUS_PENDING"},
    {STATUS_REPARSE, "STATUS_REPARSE"},
    {STATUS_MORE_ENTRIES, "STATUS_MORE_ENTRIES"},
    {STATUS_NOT_ALL_ASSIGNED, "STATUS_NOT_ALL_ASSIGNED"},
    {nt::kStatusOplockBreakInProgress, "STATUS_OPLOCK_BREAK_IN_PROGRESS"},
    {STATUS_OBJECT_NAME_EXISTS, "STATUS_OBJECT_NAME_EXISTS"},
    {STATUS_BUFFER_OVERFLOW, "STATUS_BUFFER_OVERFLOW"},
    {STATUS_NO_MORE_FILES, "STATUS_NO_MORE_FILES"},
    {nt::kStatusNoMoreEas, "STATUS_NO_MORE_EAS"},
    {nt::kStatusEaListInconsistent, "STATUS_EA_LIST_INCONSISTENT"},
    {STATUS_NO_MORE_ENTRIES, "STATUS_NO_MORE_ENTRIES"},
    {nt::kStatusStoppedOnSymlink, "STATUS_STOPPED_ON_SYMLINK"},
    {STATUS_UNSUCCESSFUL, "STATUS_UNSUCCESSFUL"},
    {STATUS_NOT_IMPLEMENTED, "STATUS_NOT_IMPLEMENTED"},
    {STATUS_INVALID_INFO_CLASS, "STATUS_INVALID_INFO_CLASS"},
    {STATUS_INFO_LENGTH_MISMATCH, "STATUS_INFO_LENGTH_MISMATCH"},
    {STATUS_ACCESS_VIOLATION, "STATUS_ACCESS_VIOLATION"},
    {STATUS_INVALID_HANDLE, "STATUS_INVALID_HANDLE"},
    {STATUS_INVALID_PARAMETER, "STATUS_INVALID_PARAMETER"},
    {STATUS_NO_SUCH_DEVICE, "STATUS_NO_SUCH_DEVICE"},
    {STATUS_NO_SUCH_FILE, "STATUS_NO_SUCH_FILE"},
    {STATUS_INVALID_DEVICE_REQUEST, "STATUS_INVALID_DEVICE_REQUEST"},
    {STATUS_END_OF_FILE, "STATUS_END_OF_FILE"},
    {STATUS_WRONG_VOLUME, "STATUS_WRONG_VOLUME"},
    {STATUS_NO_MEDIA_IN_DEVICE, "STATUS_NO_MEDIA_IN_DEVICE"},
    {STATUS_NONEXISTENT_SECTOR, "STATUS_NONEXISTENT_SECTOR"},
    {STATUS_MORE_PROCESSING_REQUIRED, "STATUS_MORE_PROCESSING_REQUIRED"},
    {STATUS_NO_MEMORY, "STATUS_NO_MEMORY"},
    {STATUS_CONFLICTING_ADDRESSES, "STATUS_CONFLICTING_ADDRESSES"},
    {STATUS_ACCESS_DENIED, "STATUS_ACCESS_DENIED"},
    {STATUS_BUFFER_TOO_SMALL, "STATUS_BUFFER_TOO_SMALL"},
    {STATUS_OBJECT_TYPE_MISMATCH, "STATUS_OBJECT_TYPE_MISMATCH"},
    {STATUS_OBJECT_NAME_INVALID, "STATUS_OBJECT_NAME_INVALID"},
    {STATUS_OBJECT_NAME_NOT_FOUND, "STATUS_OBJECT_NAME_NOT_FOUND"},
    {STATUS_OBJECT_NAME_COLLISION, "STATUS_OBJECT_NAME_COLLISION"},
    {STATUS_OBJECT_PATH_INVALID, "STATUS_OBJECT_PATH_INVALID"},
    {STATUS_OBJECT_PATH_NOT_FOUND, "STATUS_OBJECT_PATH_NOT_FOUND"},
    {STATUS_OBJECT_PATH_SYNTAX_BAD, "STATUS_OBJECT_PATH_SYNTAX_BAD"},
    {STATUS_DATA_ERROR, "STATUS_DATA_ERROR"},
    {STATUS_CRC_ERROR, "STATUS_CRC_ERROR"},
    {STATUS_SHARING_VIOLATION, "STATUS_SHARING_VIOLATION"},
    {STATUS_QUOTA_EXCEEDED, "STATUS_QUOTA_EXCEEDED"},
    {STATUS_INVALID_EA_NAME, "STATUS_INVALID_EA_NAME"},
    {nt::kStatusEaTooLarge, "STATUS_EA_TOO_LARGE"},
    {nt::kStatusNonexistentEaEntry, "STATUS_NONEXISTENT_EA_ENTRY"},
    {nt::kStatusNoEasOnFile, "STATUS_NO_EAS_ON_FILE"},
    {STATUS_EA_CORRUPT_ERROR, "STATUS_EA_CORRUPT_ERROR"},
    {STATUS_FILE_LOCK_CONFLICT, "STATUS_FILE_LOCK_CONFLICT"},
    {STATUS_LOCK_NOT_GRANTED, "STATUS_LOCK_NOT_GRANTED"},
    {STATUS_DELETE_PENDING, "STATUS_DELETE_PENDING"},
    {STATUS_CTL_FILE_NOT_SUPPORTED, "STATUS_CTL_FILE_NOT_SUPPORTED"},
    {STATUS_PRIVILEGE_NOT_HELD, "STATUS_PRIVILEGE_NOT_HELD"},
    {STATUS_INVALID_SECURITY_DESCR, "STATUS_INVALID_SECURITY_DESCR"},
    {STATUS_RANGE_NOT_LOCKED, "STATUS_RANGE_NOT_LOCKED"},
    {STATUS_DISK_FULL, "STATUS_DISK_FULL"},
    {STATUS_FILE_INVALID, "STATUS_FILE_INVALID"},
    {STATUS_MEDIA_WRITE_PROTECTED, "STATUS_MEDIA_WRITE_PROTECTED"},
    {STATUS_FILE_IS_A_DIRECTORY, "STATUS_FILE_IS_A_DIRECTORY"},
    {STATUS_NOT_SUPPORTED, "STATUS_NOT_SUPPORTED"},
    {STATUS_BAD_NETWORK_PATH, "STATUS_BAD_NETWORK_PATH"},
    {STATUS_NETWORK_ACCESS_DENIED, "STATUS_NETWORK_ACCESS_DENIED"},
    {STATUS_BAD_NETWORK_NAME, "STATUS_BAD_NETWORK_NAME"},
    {STATUS_NOT_SAME_DEVICE, "STATUS_NOT_SAME_DEVICE"},
    {STATUS_FILE_RENAMED, "STATUS_FILE_RENAMED"},
    {nt::kStatusOplockNotGranted, "STATUS_OPLOCK_NOT_GRANTED"},
    {STATUS_INVALID_OPLOCK_PROTOCOL, "STATUS_INVALID_OPLOCK_PROTOCOL"},
    {STATUS_INTERNAL_ERROR, "STATUS_INTERNAL_ERROR"},
    {STATUS_INVALID_PARAMETER_1, "STATUS_INVALID_PARAMETER_1"},
    {STATUS_INVALID_PARAMETER_2, "STATUS_INVALID_PARAMETER_2"},
    {STATUS_INVALID_PARAMETER_3, "STATUS_INVALID_PARAMETER_3"},
    {STATUS_INVALID_PARAMETER_4, "STATUS_INVALID_PARAMETER_4"},
    {STATUS_INVALID_PARAMETER_5, "STATUS_INVALID_PARAMETER_5"},
    {STATUS_INVALID_PARAMETER_6, "STATUS_INVALID_PARAMETER_6"},
    {STATUS_INVALID_PARAMETER_7, "STATUS_INVALID_PARAMETER_7"},
    {STATUS_INVALID_PARAMETER_8, "STATUS_INVALID_PARAMETER_8"},
    {STATUS_INVALID_PARAMETER_9, "STATUS_INVALID_PARAMETER_9"},
    {STATUS_INVALID_PARAMETER_10, "STATUS_INVALID_PARAMETER_10"},
    {STATUS_INVALID_PARAMETER_11, "STATUS_INVALID_PARAMETER_11"},
    {STATUS_INVALID_PARAMETER_12, "STATUS_INVALID_PARAMETER_12"},
    {STATUS_DIRECTORY_NOT_EMPTY, "STATUS_DIRECTORY_NOT_EMPTY"},
    {STATUS_FILE_CORRUPT_ERROR, "STATUS_FILE_CORRUPT_ERROR"},
    {STATUS_NOT_A_DIRECTORY, "STATUS_NOT_A_DIRECTORY"},
    {STATUS_NAME_TOO_LONG, "STATUS_NAME_TOO_LONG"},
    {STATUS_FILES_OPEN, "STATUS_FILES_OPEN"},
    {STATUS_CANCELLED, "STATUS_CANCELLED"},
    {STATUS_CANNOT_DELETE, "STATUS_CANNOT_DELETE"},
    {STATUS_FILE_DELETED, "STATUS_FILE_DELETED"},
    {STATUS_FILE_CLOSED, "STATUS_FILE_CLOSED"},
    {STATUS_OPEN_FAILED, "STATUS_OPEN_FAILED"},
    {STATUS_NOT_FOUND, "STATUS_NOT_FOUND"},
    {STATUS_USER_MAPPED_FILE, "STATUS_USER_MAPPED_FILE"},
    {STATUS_TOO_MANY_LINKS, "STATUS_TOO_MANY_LINKS"},
    {STATUS_FILE_IS_OFFLINE, "STATUS_FILE_IS_OFFLINE"},
    {STATUS_NOT_A_REPARSE_POINT, "STATUS_NOT_A_REPARSE_POINT"},
    {STATUS_IO_REPARSE_TAG_INVALID, "STATUS_IO_REPARSE_TAG_INVALID"},
    {STATUS_IO_REPARSE_TAG_MISMATCH, "STATUS_IO_REPARSE_TAG_MISMATCH"},
    {STATUS_IO_REPARSE_DATA_INVALID, "STATUS_IO_REPARSE_DATA_INVALID"},
    {STATUS_IO_REPARSE_TAG_NOT_HANDLED, "STATUS_IO_REPARSE_TAG_NOT_HANDLED"},
    {STATUS_REPARSE_POINT_NOT_RESOLVED, "STATUS_REPARSE_POINT_NOT_RESOLVED"},
    {STATUS_DIRECTORY_IS_A_REPARSE_POINT,
     "STATUS_DIRECTORY_IS_A_REPARSE_POINT"},
    {nt::kStatusCaseDifferingNamesInDir, "STATUS_CASE_DIFFERING_NAMES_IN_DIR"},
    {nt::kStatusCannotBreakOplock, "STATUS_CANNOT_BREAK_OPLOCK"},
};

struct Win32ErrorEntry {
  DWORD code;
  const char *name;
};

constexpr Win32ErrorEntry kWin32ErrorTable[] = {
    {ERROR_SUCCESS, "ERROR_SUCCESS"},
    {ERROR_INVALID_FUNCTION, "ERROR_INVALID_FUNCTION"},
    {ERROR_FILE_NOT_FOUND, "ERROR_FILE_NOT_FOUND"},
    {ERROR_PATH_NOT_FOUND, "ERROR_PATH_NOT_FOUND"},
    {ERROR_TOO_MANY_OPEN_FILES, "ERROR_TOO_MANY_OPEN_FILES"},
    {ERROR_ACCESS_DENIED, "ERROR_ACCESS_DENIED"},
    {ERROR_INVALID_HANDLE, "ERROR_INVALID_HANDLE"},
    {ERROR_NOT_ENOUGH_MEMORY, "ERROR_NOT_ENOUGH_MEMORY"},
    {ERROR_INVALID_DRIVE, "ERROR_INVALID_DRIVE"},
    {ERROR_CURRENT_DIRECTORY, "ERROR_CURRENT_DIRECTORY"},
    {ERROR_NOT_SAME_DEVICE, "ERROR_NOT_SAME_DEVICE"},
    {ERROR_NO_MORE_FILES, "ERROR_NO_MORE_FILES"},
    {ERROR_WRITE_PROTECT, "ERROR_WRITE_PROTECT"},
    {ERROR_BAD_UNIT, "ERROR_BAD_UNIT"},
    {ERROR_NOT_READY, "ERROR_NOT_READY"},
    {ERROR_CRC, "ERROR_CRC"},
    {ERROR_BAD_LENGTH, "ERROR_BAD_LENGTH"},
    {ERROR_SHARING_VIOLATION, "ERROR_SHARING_VIOLATION"},
    {ERROR_LOCK_VIOLATION, "ERROR_LOCK_VIOLATION"},
    {ERROR_HANDLE_EOF, "ERROR_HANDLE_EOF"},
    {ERROR_HANDLE_DISK_FULL, "ERROR_HANDLE_DISK_FULL"},
    {ERROR_NOT_SUPPORTED, "ERROR_NOT_SUPPORTED"},
    {ERROR_BAD_NETPATH, "ERROR_BAD_NETPATH"},
    {ERROR_NETWORK_ACCESS_DENIED, "ERROR_NETWORK_ACCESS_DENIED"},
    {ERROR_BAD_NET_NAME, "ERROR_BAD_NET_NAME"},
    {ERROR_FILE_EXISTS, "ERROR_FILE_EXISTS"},
    {ERROR_CANNOT_MAKE, "ERROR_CANNOT_MAKE"},
    {ERROR_INVALID_PARAMETER, "ERROR_INVALID_PARAMETER"},
    {ERROR_BROKEN_PIPE, "ERROR_BROKEN_PIPE"},
    {ERROR_DISK_FULL, "ERROR_DISK_FULL"},
    {ERROR_INSUFFICIENT_BUFFER, "ERROR_INSUFFICIENT_BUFFER"},
    {ERROR_INVALID_NAME, "ERROR_INVALID_NAME"},
    {ERROR_DIR_NOT_EMPTY, "ERROR_DIR_NOT_EMPTY"},
    {ERROR_BAD_PATHNAME, "ERROR_BAD_PATHNAME"},
    {ERROR_ALREADY_EXISTS, "ERROR_ALREADY_EXISTS"},
    {ERROR_FILENAME_EXCED_RANGE, "ERROR_FILENAME_EXCED_RANGE"},
    {ERROR_MORE_DATA, "ERROR_MORE_DATA"},
    {ERROR_NO_MORE_ITEMS, "ERROR_NO_MORE_ITEMS"},
    {ERROR_DIRECTORY, "ERROR_DIRECTORY"},
    {ERROR_NOT_LOCKED, "ERROR_NOT_LOCKED"},
    {ERROR_EA_LIST_INCONSISTENT, "ERROR_EA_LIST_INCONSISTENT"},
    {ERROR_EAS_DIDNT_FIT, "ERROR_EAS_DIDNT_FIT"},
    {ERROR_EAS_NOT_SUPPORTED, "ERROR_EAS_NOT_SUPPORTED"},
    {ERROR_OPERATION_ABORTED, "ERROR_OPERATION_ABORTED"},
    {ERROR_IO_INCOMPLETE, "ERROR_IO_INCOMPLETE"},
    {ERROR_IO_PENDING, "ERROR_IO_PENDING"},
    {ERROR_FILE_INVALID, "ERROR_FILE_INVALID"},
    {ERROR_TOO_MANY_LINKS, "ERROR_TOO_MANY_LINKS"},
    {ERROR_USER_MAPPED_FILE, "ERROR_USER_MAPPED_FILE"},
    {ERROR_PRIVILEGE_NOT_HELD, "ERROR_PRIVILEGE_NOT_HELD"},
    {ERROR_CANT_RESOLVE_FILENAME, "ERROR_CANT_RESOLVE_FILENAME"},
    {ERROR_NOT_A_REPARSE_POINT, "ERROR_NOT_A_REPARSE_POINT"},
    {ERROR_REPARSE_TAG_INVALID, "ERROR_REPARSE_TAG_INVALID"},
    {ERROR_REPARSE_TAG_MISMATCH, "ERROR_REPARSE_TAG_MISMATCH"},
    {ERROR_INVALID_REPARSE_DATA, "ERROR_INVALID_REPARSE_DATA"},
    {ERROR_CANT_ACCESS_FILE, "ERROR_CANT_ACCESS_FILE"},
    {ERROR_DELETE_PENDING, "ERROR_DELETE_PENDING"},
};

std::string TrimTrailingCrLf(std::string s) {
  while (!s.empty() &&
         (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

std::string FormatSystemMessage(DWORD code, HMODULE module) {
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS;
  flags |= (module != nullptr) ? FORMAT_MESSAGE_FROM_HMODULE
                               : FORMAT_MESSAGE_FROM_SYSTEM;
  UniqueLocalMem<wchar_t> msg_buf;
  const DWORD chars = ::FormatMessageW(
      flags, module, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(msg_buf.put()), 0, nullptr);
  if (chars == 0 || !msg_buf) {
    return {};
  }
  return TrimTrailingCrLf(WideToUtf8(std::wstring_view(msg_buf.get(), chars)));
}

} // namespace

std::string NtStatusName(NTSTATUS status) {
  for (const auto &entry : kNtStatusTable) {
    if (entry.code == status) {
      return entry.name;
    }
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "NTSTATUS_0x%08lX",
                static_cast<unsigned long>(static_cast<ULONG>(status)));
  return buf;
}

std::string FormatNtStatus(NTSTATUS status) {
  std::ostringstream oss;
  oss << NtStatusName(status) << " (0x" << std::uppercase << std::hex
      << std::setw(8) << std::setfill('0') << static_cast<ULONG>(status) << ")";
  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll != nullptr) {
    const std::string msg =
        FormatSystemMessage(static_cast<DWORD>(status), ntdll);
    if (!msg.empty()) {
      oss << ": " << msg;
    }
  }
  return oss.str();
}

std::string Win32ErrorName(DWORD error) {
  for (const auto &entry : kWin32ErrorTable) {
    if (entry.code == error) {
      return entry.name;
    }
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "ERROR_%lu",
                static_cast<unsigned long>(error));
  return buf;
}

std::string FormatWin32Error(DWORD error) {
  std::ostringstream oss;
  oss << Win32ErrorName(error) << " (" << error << " / 0x" << std::uppercase
      << std::hex << error << ")";
  const std::string msg = FormatSystemMessage(error, nullptr);
  if (!msg.empty()) {
    oss << ": " << msg;
  }
  return oss.str();
}

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty() ||
      wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  // WC_ERR_INVALID_CHARS rejects lone surrogates instead of silently emitting
  // U+FFFD, so invalid input returns empty as documented in status.h.
  const int required = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(required), '\0');
  const int written = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
      out.data(), required, nullptr, nullptr);
  if (written != required) {
    return {};
  }
  return out;
}

std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty() ||
      utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  // MB_ERR_INVALID_CHARS rejects malformed/overlong/surrogate-encoding UTF-8
  // instead of silently substituting U+FFFD.
  const int required =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), nullptr, 0);
  if (required <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(required), L'\0');
  const int written = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
      out.data(), required);
  if (written != required) {
    return {};
  }
  return out;
}

} // namespace fstest::core
