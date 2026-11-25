// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fstest {
namespace {

core::Win32Result<void> SetProtectedInheritableDacl(std::wstring_view path,
                                                    std::wstring_view sddl) {
  core::Win32Result<void> result{};
  auto sd = core::SddlToSecurityDescriptor(sddl);
  if (!sd) {
    result.error = sd.error;
    return result;
  }
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  PACL dacl = nullptr;
  if (::GetSecurityDescriptorDacl(sd.value->get(), &dacl_present, &dacl,
                                  &dacl_defaulted) == FALSE ||
      dacl_present == FALSE) {
    result.error = ERROR_INVALID_SECURITY_DESCR;
    return result;
  }
  std::wstring path_buf(path);
  result.error = ::SetNamedSecurityInfoW(
      path_buf.data(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
      nullptr, dacl, nullptr);
  return result;
}

core::Win32Result<core::UniqueHandle>
CreateFileWithSddl(std::wstring_view path, std::wstring_view sddl,
                   DWORD desired_access = GENERIC_WRITE, DWORD share_mode = 0) {
  core::Win32Result<core::UniqueHandle> result{};
  auto sd = core::SddlToSecurityDescriptor(sddl);
  if (!sd) {
    result.error = sd.error;
    return result;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd.value->get();
  sa.bInheritHandle = FALSE;
  return core::CreateWin32File(path, desired_access, share_mode, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, &sa);
}

core::Win32Result<std::vector<uint8_t>>
QueryRawFileSecurity(std::wstring_view path, SECURITY_INFORMATION info) {
  core::Win32Result<std::vector<uint8_t>> result{};
  std::wstring path_str(path);
  DWORD needed = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::GetFileSecurityW(path_str.c_str(), info, nullptr, 0, &needed) ==
      FALSE) {
    const DWORD err = ::GetLastError();
    if (err != ERROR_INSUFFICIENT_BUFFER) {
      result.error = err;
      return result;
    }
  }
  if (needed == 0) {
    result.error = ERROR_INVALID_SECURITY_DESCR;
    return result;
  }
  std::vector<uint8_t> buf(needed, 0);
  if (::GetFileSecurityW(path_str.c_str(), info, buf.data(), needed, &needed) ==
      FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(buf);
  return result;
}

core::NtResult<std::vector<uint8_t>>
QueryHandleSecurityNt(HANDLE h, SECURITY_INFORMATION info) {
  core::NtResult<std::vector<uint8_t>> result{};
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQuerySecurityObject == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  ULONG needed = 0;
  NTSTATUS st = ntdll.NtQuerySecurityObject(h, info, nullptr, 0, &needed);
  if (st != STATUS_BUFFER_TOO_SMALL) {
    result.status = nt::NtSuccess(st) ? nt::kStatusInvalidSecurityDescr : st;
    return result;
  }
  if (needed == 0) {
    result.status = nt::kStatusInvalidSecurityDescr;
    return result;
  }
  std::vector<uint8_t> buf(needed, 0);
  ULONG returned = 0;
  st = ntdll.NtQuerySecurityObject(h, info, buf.data(), needed, &returned);
  result.status = st;
  result.information = returned;
  if (nt::NtSuccess(st)) {
    result.value = std::move(buf);
  }
  return result;
}

core::Win32Result<core::UniqueLocalMem<void>>
StringToSid(std::wstring_view sid_str) {
  core::Win32Result<core::UniqueLocalMem<void>> result{};
  std::wstring s(sid_str);
  core::UniqueLocalMem<void> sid;
  if (::ConvertStringSidToSidW(s.c_str(), sid.put()) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(sid);
  return result;
}

bool GetDaclAndInfo(PSECURITY_DESCRIPTOR sd, PACL *dacl_out,
                    ACL_SIZE_INFORMATION *acl_info_out) noexcept {
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  PACL dacl = nullptr;
  if (::GetSecurityDescriptorDacl(sd, &dacl_present, &dacl, &dacl_defaulted) ==
          FALSE ||
      dacl_present == FALSE || dacl == nullptr) {
    return false;
  }
  ACL_SIZE_INFORMATION acl_info{};
  if (::GetAclInformation(dacl, &acl_info, sizeof(acl_info),
                          AclSizeInformation) == FALSE) {
    return false;
  }
  *dacl_out = dacl;
  *acl_info_out = acl_info;
  return true;
}

bool ExtractAceMaskAndSid(LPVOID raw_ace, ACCESS_MASK *mask_out,
                          PSID *sid_out) noexcept {
  const auto *hdr = static_cast<const ACE_HEADER *>(raw_ace);
  if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE) {
    auto *allow = static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
    *mask_out = allow->Mask;
    *sid_out = &allow->SidStart;
    return true;
  }
  if (hdr->AceType == ACCESS_DENIED_ACE_TYPE) {
    auto *deny = static_cast<ACCESS_DENIED_ACE *>(raw_ace);
    *mask_out = deny->Mask;
    *sid_out = &deny->SidStart;
    return true;
  }
  return false;
}

int FindMatchingAceIndex(PSECURITY_DESCRIPTOR sd, BYTE ace_type,
                         BYTE required_flags, BYTE forbidden_flags,
                         ACCESS_MASK required_mask, PSID target_sid) {
  PACL dacl = nullptr;
  ACL_SIZE_INFORMATION acl_info{};
  if (!GetDaclAndInfo(sd, &dacl, &acl_info)) {
    return -1;
  }
  for (DWORD i = 0; i < acl_info.AceCount; ++i) {
    LPVOID raw_ace = nullptr;
    if (::GetAce(dacl, i, &raw_ace) == FALSE || raw_ace == nullptr) {
      continue;
    }
    const auto *hdr = static_cast<const ACE_HEADER *>(raw_ace);
    if (hdr->AceType != ace_type ||
        (hdr->AceFlags & required_flags) != required_flags ||
        (hdr->AceFlags & forbidden_flags) != 0) {
      continue;
    }
    ACCESS_MASK ace_mask = 0;
    PSID ace_sid = nullptr;
    if (ExtractAceMaskAndSid(raw_ace, &ace_mask, &ace_sid) &&
        ace_mask == required_mask && ::EqualSid(ace_sid, target_sid) != FALSE) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool HasAnyInheritedAce(PSECURITY_DESCRIPTOR sd) {
  PACL dacl = nullptr;
  ACL_SIZE_INFORMATION acl_info{};
  if (!GetDaclAndInfo(sd, &dacl, &acl_info)) {
    return false;
  }
  for (DWORD i = 0; i < acl_info.AceCount; ++i) {
    LPVOID raw_ace = nullptr;
    if (::GetAce(dacl, i, &raw_ace) != FALSE && raw_ace != nullptr) {
      const auto *hdr = static_cast<const ACE_HEADER *>(raw_ace);
      if ((hdr->AceFlags & INHERITED_ACE) != 0) {
        return true;
      }
    }
  }
  return false;
}

bool HasAceForSid(PSECURITY_DESCRIPTOR sd, PSID target_sid) {
  PACL dacl = nullptr;
  ACL_SIZE_INFORMATION acl_info{};
  if (!GetDaclAndInfo(sd, &dacl, &acl_info)) {
    return false;
  }
  for (DWORD i = 0; i < acl_info.AceCount; ++i) {
    LPVOID raw_ace = nullptr;
    if (::GetAce(dacl, i, &raw_ace) == FALSE || raw_ace == nullptr) {
      continue;
    }
    ACCESS_MASK unused_mask = 0;
    PSID ace_sid = nullptr;
    if (ExtractAceMaskAndSid(raw_ace, &unused_mask, &ace_sid) &&
        ::EqualSid(ace_sid, target_sid) != FALSE) {
      return true;
    }
  }
  return false;
}

bool HasAnyUnmappedGenericRightsAce(PSECURITY_DESCRIPTOR sd) {
  PACL dacl = nullptr;
  ACL_SIZE_INFORMATION acl_info{};
  if (!GetDaclAndInfo(sd, &dacl, &acl_info)) {
    return false;
  }
  constexpr ACCESS_MASK kGenericBits =
      GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL;
  for (DWORD i = 0; i < acl_info.AceCount; ++i) {
    LPVOID raw_ace = nullptr;
    if (::GetAce(dacl, i, &raw_ace) == FALSE || raw_ace == nullptr) {
      continue;
    }
    ACCESS_MASK ace_mask = 0;
    PSID unused_sid = nullptr;
    if (ExtractAceMaskAndSid(raw_ace, &ace_mask, &unused_sid) &&
        (ace_mask & kGenericBits) != 0) {
      return true;
    }
  }
  return false;
}

// RAII guard that restores a file's owner SID via an open WRITE_OWNER handle
// on scope exit so ownership tests never leak modified ownership on failure.
class ScopedOwnerRestore {
public:
  ScopedOwnerRestore(HANDLE write_owner_handle,
                     PSID original_owner_sid) noexcept
      : handle_(write_owner_handle), owner_sid_(original_owner_sid) {}
  ~ScopedOwnerRestore() { RestoreNow(); }

  ScopedOwnerRestore(const ScopedOwnerRestore &) = delete;
  ScopedOwnerRestore &operator=(const ScopedOwnerRestore &) = delete;

  void RestoreNow() noexcept {
    if (!armed_ || handle_ == nullptr || owner_sid_ == nullptr) {
      return;
    }
    const auto &ntdll = core::Ntdll();
    if (ntdll.NtSetSecurityObject != nullptr) {
      SECURITY_DESCRIPTOR sd{};
      if (::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) !=
              FALSE &&
          ::SetSecurityDescriptorOwner(&sd, owner_sid_, FALSE) != FALSE) {
        ntdll.NtSetSecurityObject(handle_, OWNER_SECURITY_INFORMATION, &sd);
      }
    }
    armed_ = false;
  }

  void Disarm() noexcept { armed_ = false; }

private:
  HANDLE handle_ = nullptr;
  PSID owner_sid_ = nullptr;
  bool armed_ = true;
};

// Returns a LocalAlloc-owned copy of the SID held by a single-SID token
// information class whose structure starts with a PSID member (TokenOwner or
// TokenPrimaryGroup) on the current process token.
core::Win32Result<core::UniqueLocalMem<void>>
QueryCurrentTokenSingleSid(TOKEN_INFORMATION_CLASS info_class) {
  static_assert(offsetof(TOKEN_OWNER, Owner) == 0);
  static_assert(offsetof(TOKEN_PRIMARY_GROUP, PrimaryGroup) == 0);

  core::Win32Result<core::UniqueLocalMem<void>> result{};
  core::UniqueHandle token;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put()) ==
      FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  DWORD needed = 0;
  ::SetLastError(ERROR_SUCCESS);
  if (::GetTokenInformation(token.get(), info_class, nullptr, 0, &needed) ==
      FALSE) {
    const DWORD err = ::GetLastError();
    if (err != ERROR_INSUFFICIENT_BUFFER) {
      result.error = err;
      return result;
    }
  }
  if (needed < sizeof(PSID)) {
    result.error = ERROR_INVALID_SID;
    return result;
  }
  std::vector<uint8_t> buf(needed, 0);
  if (::GetTokenInformation(token.get(), info_class, buf.data(), needed,
                            &needed) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  PSID token_sid = nullptr;
  std::memcpy(&token_sid, buf.data(), sizeof(token_sid));
  if (token_sid == nullptr || ::IsValidSid(token_sid) == FALSE) {
    result.error = ERROR_INVALID_SID;
    return result;
  }
  const DWORD sid_len = ::GetLengthSid(token_sid);
  core::UniqueLocalMem<void> sid_copy(::LocalAlloc(LMEM_FIXED, sid_len));
  if (!sid_copy) {
    result.error = ::GetLastError();
    return result;
  }
  if (::CopySid(sid_len, sid_copy.get(), token_sid) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(sid_copy);
  return result;
}

core::Win32Result<core::UniqueLocalMem<void>> QueryCurrentTokenOwnerSid() {
  return QueryCurrentTokenSingleSid(TokenOwner);
}

core::Win32Result<core::UniqueLocalMem<void>>
QueryCurrentTokenPrimaryGroupSid() {
  return QueryCurrentTokenSingleSid(TokenPrimaryGroup);
}

// Duplicates the current process token into an impersonation token lowered to
// the Low mandatory integrity level (S-1-16-4096). Lowering the integrity
// level of a duplicated token requires no privilege.
core::Win32Result<core::UniqueHandle> CreateLowIntegrityImpersonationToken() {
  core::Win32Result<core::UniqueHandle> result{};
  core::UniqueHandle proc_token;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                         proc_token.put()) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  core::UniqueHandle low_token;
  if (::DuplicateTokenEx(proc_token.get(),
                         TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE |
                             TOKEN_ADJUST_DEFAULT,
                         nullptr, SecurityImpersonation, TokenImpersonation,
                         low_token.put()) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  auto low_sid = StringToSid(L"S-1-16-4096"); // Low Mandatory Level
  if (!low_sid) {
    result.error = low_sid.error;
    return result;
  }
  TOKEN_MANDATORY_LABEL label{};
  label.Label.Sid = low_sid.value->get();
  label.Label.Attributes = SE_GROUP_INTEGRITY;
  const DWORD label_size =
      static_cast<DWORD>(sizeof(label)) + ::GetLengthSid(low_sid.value->get());
  if (::SetTokenInformation(low_token.get(), TokenIntegrityLevel, &label,
                            label_size) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  result.error = ERROR_SUCCESS;
  result.value = std::move(low_token);
  return result;
}

// Queries FileAccessInformation: the access mask granted to handle `h`.
core::NtResult<ACCESS_MASK> QueryFileAccessMask(HANDLE h) {
  core::NtResult<ACCESS_MASK> result{};
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  nt::IoStatusBlock iosb{};
  nt::FileAccessInfo info{};
  result.status = ntdll.NtQueryInformationFile(
      h, &iosb, &info, sizeof(info), nt::FileInfoClass::FileAccessInformation);
  result.information = iosb.information;
  if (nt::NtSuccess(result.status)) {
    result.value = info.access_flags;
  }
  return result;
}

struct DaclInspection {
  bool present = false;
  bool is_null = false;
  DWORD ace_count = 0;
};

core::Win32Result<DaclInspection> InspectFileDacl(std::wstring_view path) {
  core::Win32Result<DaclInspection> result{};
  auto buf = QueryRawFileSecurity(path, DACL_SECURITY_INFORMATION);
  if (!buf) {
    result.error = buf.error;
    return result;
  }
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  if (::GetSecurityDescriptorDacl(buf.value->data(), &present, &dacl,
                                  &defaulted) == FALSE) {
    result.error = ::GetLastError();
    return result;
  }
  DaclInspection info{};
  info.present = (present != FALSE);
  info.is_null = (dacl == nullptr);
  if (info.present && dacl != nullptr) {
    ACL_SIZE_INFORMATION acl_info{};
    if (::GetAclInformation(dacl, &acl_info, sizeof(acl_info),
                            AclSizeInformation) == FALSE) {
      result.error = ::GetLastError();
      return result;
    }
    info.ace_count = acl_info.AceCount;
  }
  result.error = ERROR_SUCCESS;
  result.value = info;
  return result;
}

void ExpectAccessDeniedOpen(std::wstring_view path, DWORD desired_access) {
  auto opened = core::CreateWin32File(path, desired_access, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(opened);
  EXPECT_WIN32_ERROR_CODE(opened.error, ERROR_ACCESS_DENIED);
}

// Creates `path` with a permissive protected DACL and opens it by NT path with
// READ_CONTROL | WRITE_DAC | WRITE_OWNER for NtSetSecurityObject validation
// tests. Callers must wrap the call in ASSERT_NO_FATAL_FAILURE.
void CreateSetSecurityTarget(const std::wstring &path,
                             const std::wstring &nt_path,
                             core::UniqueHandle *handle_out) {
  ASSERT_NE(core::Ntdll().NtSetSecurityObject, nullptr);
  {
    auto created = CreateFileWithSddl(path, L"D:P(A;;FA;;;WD)", GENERIC_WRITE,
                                      core::kShareAll);
    ASSERT_TRUE(created) << core::FormatWin32Error(created.error);
  }
  auto opened = core::NtOpenFilePath(
      nt_path, READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE,
      core::kShareAll, 0);
  ASSERT_NT_SUCCESS(opened.status);
  ASSERT_TRUE(opened.value.has_value());
  *handle_out = std::move(*opened.value);
}

} // namespace

// ---------------------------------------------------------------------------
// Domain 9: Security & ACLs
// ---------------------------------------------------------------------------

// Spec: [MS-FSA] 2.1.5.1.1 / 2.1.5.14 / 2.1.5.17 — creating a file with an
// SDDL security descriptor maps generic rights and persists DACL updates.
TEST_F(SecurityAclTest, CreateFileWithSddlAndGetSetDacl) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"sddl_file.dat");
  {
    auto h = CreateFileWithSddl(path, L"D:P(A;;GA;;;WD)");
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }

  auto sddl1 = core::GetFileSddl(path, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(sddl1) << core::FormatWin32Error(sddl1.error);
  EXPECT_EQ(*sddl1.value, L"D:P(A;;FA;;;WD)");

  auto set_res = core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                                   L"D:P(A;;GA;;;WD)(A;;GR;;;SY)");
  ASSERT_TRUE(set_res) << core::FormatWin32Error(set_res.error);

  auto sddl2 = core::GetFileSddl(path, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(sddl2) << core::FormatWin32Error(sddl2.error);
  EXPECT_EQ(*sddl2.value, L"D:P(A;;FA;;;WD)(A;;FR;;;SY)");

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — a deny-read DACL blocks
// GENERIC_READ opens with ERROR_ACCESS_DENIED.
TEST_F(SecurityAclTest, SetDenyReadDaclEnforcesAccessDenied) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"deny_read.dat");
  {
    auto h = CreateFileWithSddl(path, L"D:P(A;;GA;;;WD)");
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }
  {
    auto h = core::CreateWin32File(path, GENERIC_READ, 0, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }

  auto dacl_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"deny_read.dat"));
  ASSERT_TRUE(dacl_guard.armed());
  auto deny_res =
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(D;;GR;;;WD)");
  ASSERT_TRUE(deny_res) << core::FormatWin32Error(deny_res.error);

  auto blocked = core::CreateWin32File(path, GENERIC_READ, 0, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(blocked);
  EXPECT_WIN32_ERROR_CODE(blocked.error, ERROR_ACCESS_DENIED);

  // Restore the permissive DACL through the WRITE_DAC handle pre-opened by the
  // guard. The owner's path-based WRITE_DAC restore under this DACL is a
  // separate contract (OwnerRestoresDaclUnderDenyReadAce).
  dacl_guard.RestoreNow();

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — the owner's implicit
// WRITE_DAC is not revoked by a Deny GENERIC_READ ACE (which also denies
// READ_CONTROL), so the owner can still replace the DACL by path.
TEST_F(SecurityAclTest, OwnerRestoresDaclUnderDenyReadAce) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"deny_read_owner_restore.dat");
  {
    auto h = CreateFileWithSddl(path, L"D:P(A;;GA;;;WD)");
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }

  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"deny_read_owner_restore.dat"));
  ASSERT_TRUE(dacl_guard.armed());
  auto deny_res =
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(D;;GR;;;WD)");
  ASSERT_TRUE(deny_res) << core::FormatWin32Error(deny_res.error);

  // Path-based restore relies solely on the owner's implicit WRITE_DAC (do not
  // rely on the guard's pre-opened handle or parent FILE_DELETE_CHILD).
  auto restore =
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)");
  EXPECT_TRUE(restore) << core::FormatWin32Error(restore.error);

  auto restored_sddl = core::GetFileSddl(path, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(restored_sddl) << core::FormatWin32Error(restored_sddl.error);
  EXPECT_EQ(*restored_sddl.value, L"D:P(A;;FA;;;WD)");

  dacl_guard.RestoreNow();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.1 / 2.1.5.14 / 2.1.5.17 — directories apply create
// SDDL security descriptors and persist subsequent DACL updates.
TEST_F(SecurityAclTest, CreateDirectoryWithSddlAndGetSetDacl) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring dir = TestPath(L"sddl_dir");
  auto sd = core::SddlToSecurityDescriptor(L"D:P(A;;GA;;;WD)");
  ASSERT_TRUE(sd) << core::FormatWin32Error(sd.error);

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd.value->get();
  sa.bInheritHandle = FALSE;

  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), &sa));

  auto sddl1 = core::GetFileSddl(dir, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(sddl1) << core::FormatWin32Error(sddl1.error);
  EXPECT_EQ(*sddl1.value, L"D:P(A;;FA;;;WD)");

  auto set_res = core::SetFileSddl(dir, DACL_SECURITY_INFORMATION,
                                   L"D:P(A;;GA;;;WD)(A;;GR;;;SY)");
  ASSERT_TRUE(set_res) << core::FormatWin32Error(set_res.error);

  auto sddl2 = core::GetFileSddl(dir, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(sddl2) << core::FormatWin32Error(sddl2.error);
  EXPECT_EQ(*sddl2.value, L"D:P(A;;FA;;;WD)(A;;FR;;;SY)");

  EXPECT_WIN32_TRUE(::RemoveDirectoryW(dir.c_str()));
}

// Spec: [MS-FSA] 2.1.5.14 & Win32 GetFileSecurityW — zero-byte probes report
// required size and selective OWNER, GROUP, and DACL queries stay isolated.
TEST_F(SecurityAclTest, QueryOwnerGroupAndDaclSelectively) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"selective_query.dat");
  auto created = CreateFileWithSddl(path, L"D:P(A;;FA;;;WD)");
  ASSERT_TRUE(created) << core::FormatWin32Error(created.error);
  created.value->reset();

  // Zero-byte probe must return ERROR_INSUFFICIENT_BUFFER and required size.
  DWORD needed = 0;
  EXPECT_WIN32_ERROR(::GetFileSecurityW(path.c_str(),
                                        OWNER_SECURITY_INFORMATION |
                                            GROUP_SECURITY_INFORMATION |
                                            DACL_SECURITY_INFORMATION,
                                        nullptr, 0, &needed),
                     ERROR_INSUFFICIENT_BUFFER);
  EXPECT_GT(needed, DWORD{0});

  // Selective OWNER query.
  {
    auto owner_buf = QueryRawFileSecurity(path, OWNER_SECURITY_INFORMATION);
    ASSERT_TRUE(owner_buf) << core::FormatWin32Error(owner_buf.error);
    PSID owner_sid = nullptr;
    BOOL owner_defaulted = FALSE;
    ASSERT_WIN32_TRUE(::GetSecurityDescriptorOwner(
        owner_buf.value->data(), &owner_sid, &owner_defaulted));
    ASSERT_NE(owner_sid, nullptr);
    EXPECT_TRUE(::IsValidSid(owner_sid) != FALSE);

    auto owner_sddl = core::SecurityDescriptorToSddl(
        owner_buf.value->data(), OWNER_SECURITY_INFORMATION);
    ASSERT_TRUE(owner_sddl) << core::FormatWin32Error(owner_sddl.error);
    EXPECT_EQ(owner_sddl.value->rfind(L"O:", 0), size_t{0});
    EXPECT_EQ(owner_sddl.value->find(L"G:"), std::wstring::npos);
    EXPECT_EQ(owner_sddl.value->find(L"D:"), std::wstring::npos);
  }

  // Selective GROUP query.
  {
    auto group_buf = QueryRawFileSecurity(path, GROUP_SECURITY_INFORMATION);
    ASSERT_TRUE(group_buf) << core::FormatWin32Error(group_buf.error);
    PSID group_sid = nullptr;
    BOOL group_defaulted = FALSE;
    ASSERT_WIN32_TRUE(::GetSecurityDescriptorGroup(
        group_buf.value->data(), &group_sid, &group_defaulted));
    ASSERT_NE(group_sid, nullptr);
    EXPECT_TRUE(::IsValidSid(group_sid) != FALSE);

    auto group_sddl = core::SecurityDescriptorToSddl(
        group_buf.value->data(), GROUP_SECURITY_INFORMATION);
    ASSERT_TRUE(group_sddl) << core::FormatWin32Error(group_sddl.error);
    EXPECT_EQ(group_sddl.value->rfind(L"G:", 0), size_t{0});
    EXPECT_EQ(group_sddl.value->find(L"O:"), std::wstring::npos);
    EXPECT_EQ(group_sddl.value->find(L"D:"), std::wstring::npos);
  }

  // Selective DACL query.
  {
    auto dacl_buf = QueryRawFileSecurity(path, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(dacl_buf) << core::FormatWin32Error(dacl_buf.error);
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    ASSERT_WIN32_TRUE(::GetSecurityDescriptorDacl(
        dacl_buf.value->data(), &dacl_present, &dacl, &dacl_defaulted));
    EXPECT_TRUE(dacl_present != FALSE);
    EXPECT_NE(dacl, nullptr);

    auto dacl_sddl = core::SecurityDescriptorToSddl(dacl_buf.value->data(),
                                                    DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(dacl_sddl) << core::FormatWin32Error(dacl_sddl.error);
    EXPECT_EQ(*dacl_sddl.value, L"D:P(A;;FA;;;WD)");
  }
}

// Spec: [MS-FSA] 2.1.5.1.1 & Win32 ACE Inheritance — OI|CI parent ACEs
// propagate with INHERITED_ACE to children unless blocked by D:P.
TEST_F(SecurityAclTest, DaclInheritanceFromParentDirectory) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto user_sid = core::GetCurrentUserSid();
  auto user_sid_str = core::GetCurrentUserSidString();
  auto wd_sid = StringToSid(L"S-1-1-0");
  ASSERT_TRUE(user_sid) << core::FormatWin32Error(user_sid.error);
  ASSERT_TRUE(user_sid_str) << core::FormatWin32Error(user_sid_str.error);
  ASSERT_TRUE(wd_sid) << core::FormatWin32Error(wd_sid.error);

  // Use concrete current user SID (never SDDL OW / S-1-3-4) and
  // SetNamedSecurityInfoW so SE_DACL_AUTO_INHERITED is set and Windows stamps
  // INHERITED_ACE (ID) on inherited child ACEs.
  const std::wstring parent_sddl =
      L"D:P(A;OICI;FR;;;WD)(A;OICI;FA;;;" + *user_sid_str.value + L")";
  const std::wstring parent = TestPath(L"inherit_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  auto parent_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"inherit_parent"));
  ASSERT_TRUE(parent_guard.armed());
  ASSERT_WIN32_OK(SetProtectedInheritableDacl(parent, parent_sddl));

  // Child file created with nullptr security descriptor inherits ACEs with ID.
  const std::wstring child_file = parent + L"\\child_file.dat";
  ASSERT_WIN32_OK(core::CreateWin32File(child_file, GENERIC_WRITE,
                                        core::kShareAll, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL));
  {
    auto buf = QueryRawFileSecurity(child_file, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   INHERITED_ACE,
                                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                   FILE_GENERIC_READ, wd_sid.value->get()),
              0);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   INHERITED_ACE,
                                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                   FILE_ALL_ACCESS, user_sid.value->get()),
              0);
  }

  // Child subdirectory created with nullptr SD inherits ACEs with OI|CI|ID.
  const std::wstring child_dir = parent + L"\\child_dir";
  ASSERT_WIN32_TRUE(::CreateDirectoryW(child_dir.c_str(), nullptr));
  {
    auto buf = QueryRawFileSecurity(child_dir, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE |
                                       INHERITED_ACE,
                                   0, FILE_GENERIC_READ, wd_sid.value->get()),
              0);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE |
                                       INHERITED_ACE,
                                   0, FILE_ALL_ACCESS, user_sid.value->get()),
              0);
  }

  // Protected child file created with D:P(...) blocks inheritance completely.
  const std::wstring prot_file = parent + L"\\protected_child.dat";
  auto prot_h = CreateFileWithSddl(prot_file, L"D:P(A;;FA;;;WD)", GENERIC_WRITE,
                                   core::kShareAll);
  ASSERT_TRUE(prot_h) << core::FormatWin32Error(prot_h.error);
  prot_h.value->reset();
  {
    auto buf = QueryRawFileSecurity(prot_file, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    EXPECT_FALSE(HasAnyInheritedAce(buf.value->data()));
    auto sddl = core::GetFileSddl(prot_file, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(sddl) << core::FormatWin32Error(sddl.error);
    EXPECT_EQ(*sddl.value, L"D:P(A;;FA;;;WD)");
  }

  parent_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — FILE_WRITE_DATA and
// FILE_APPEND_DATA rights are enforced independently on file opens.
TEST_F(SecurityAclTest, WriteDataAndAppendDataAccessEnforcement) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"write_append.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "seed"));
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"write_append.dat"));
  ASSERT_TRUE(dacl_guard.armed());

  // DACL granting only FILE_GENERIC_READ blocks write and append.
  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(A;;FR;;;WD)"));
  {
    EXPECT_TRUE(helpers::CheckFileContents(path, "seed"));
    ExpectAccessDeniedOpen(path, GENERIC_WRITE);
    ExpectAccessDeniedOpen(path, FILE_WRITE_DATA);
    ExpectAccessDeniedOpen(path, FILE_APPEND_DATA);
  }

  // DACL granting FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL |
  // SYNCHRONIZE (0x00120084) allows FILE_APPEND_DATA while blocking
  // FILE_WRITE_DATA.
  ASSERT_WIN32_OK(core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                                    L"D:P(A;;0x00120084;;;WD)"));
  {
    ExpectAccessDeniedOpen(path, FILE_WRITE_DATA);

    auto ad = core::CreateWin32File(path, FILE_APPEND_DATA, core::kShareAll,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(ad) << core::FormatWin32Error(ad.error);
    auto wr = core::WriteAllBytes(ad.value->get(), "+tail", 5);
    ASSERT_TRUE(wr) << core::FormatWin32Error(wr.error);
  }

  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  dacl_guard.DisarmAndClose();
  EXPECT_TRUE(helpers::CheckFileContents(path, "seed+tail"));
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — deleting a file succeeds
// when either child DELETE or parent FILE_DELETE_CHILD is granted.
TEST_F(SecurityAclTest, DeleteRightVsParentDeleteChildRight) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring parent = TestPath(L"del_rights_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  auto parent_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"del_rights_parent"));
  ASSERT_TRUE(parent_guard.armed());

  // (a) Parent grants FILE_DELETE_CHILD (FA); child denies DELETE (0x00010000).
  //     DeleteFileW succeeds via parent's FILE_DELETE_CHILD right.
  ASSERT_WIN32_OK(
      core::SetFileSddl(parent, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  const std::wstring file_a = parent + L"\\a.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(file_a, "a"));
  {
    auto file_a_guard =
        core::ScopedDaclRestore::Arm(TestNtPath(L"del_rights_parent\\a.dat"));
    ASSERT_TRUE(file_a_guard.armed());
    ASSERT_WIN32_OK(
        core::SetFileSddl(file_a, DACL_SECURITY_INFORMATION,
                          L"D:P(D;;0x00010000;;;WD)(A;;0x001E01FF;;;WD)"));
    ::SetLastError(ERROR_SUCCESS);
    const BOOL del_ok = ::DeleteFileW(file_a.c_str());
    const DWORD del_err = del_ok ? ERROR_SUCCESS : ::GetLastError();
    EXPECT_TRUE(del_ok != FALSE) << core::FormatWin32Error(del_err);
    if (del_ok != FALSE) {
      file_a_guard.DisarmAndClose();
    } else {
      file_a_guard.RestoreNow();
    }
  }

  // (b) Parent denies FILE_DELETE_CHILD (0x00000040); child grants DELETE (FA).
  //     DeleteFileW succeeds via child's DELETE right.
  ASSERT_WIN32_OK(
      core::SetFileSddl(parent, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000040;;;WD)(A;;0x001F01BF;;;WD)"));
  const std::wstring file_b = parent + L"\\b.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(file_b, "b"));
  ASSERT_WIN32_OK(
      core::SetFileSddl(file_b, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  EXPECT_WIN32_TRUE(::DeleteFileW(file_b.c_str()));

  // (c) Neither parent (no FILE_DELETE_CHILD) nor child (no DELETE) grants
  //     delete -> DeleteFileW fails ERROR_ACCESS_DENIED.
  const std::wstring file_c = parent + L"\\c.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(file_c, "c"));
  auto file_c_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"del_rights_parent\\c.dat"));
  ASSERT_TRUE(file_c_guard.armed());
  ASSERT_WIN32_OK(
      core::SetFileSddl(file_c, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00010000;;;WD)(A;;0x001E01FF;;;WD)"));
  EXPECT_WIN32_ERROR(::DeleteFileW(file_c.c_str()), ERROR_ACCESS_DENIED);
  EXPECT_NE(::GetFileAttributesW(file_c.c_str()), INVALID_FILE_ATTRIBUTES);

  // Restore permissive DACLs and clean up.
  ASSERT_WIN32_OK(
      core::SetFileSddl(file_c, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  file_c_guard.DisarmAndClose();
  ASSERT_WIN32_OK(
      core::SetFileSddl(parent, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  parent_guard.DisarmAndClose();
  EXPECT_WIN32_TRUE(::DeleteFileW(file_c.c_str()));
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — FILE_LIST_DIRECTORY and
// FILE_ADD_FILE are enforced independently on directory operations.
TEST_F(SecurityAclTest, DirectoryListAndCreateFileAccessEnforcement) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring dir = TestPath(L"dir_acl_enforce");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  auto dir_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"dir_acl_enforce"));
  ASSERT_TRUE(dir_guard.armed());
  ASSERT_WIN32_OK(core::WriteFileContents(dir + L"\\seed.dat", "seed"));

  // Deny FILE_LIST_DIRECTORY (0x0001), allow all other directory rights.
  ASSERT_WIN32_OK(
      core::SetFileSddl(dir, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000001;;;WD)(A;;0x001F01FE;;;WD)"));
  {
    auto listing = core::ListDirectoryWin32(dir);
    EXPECT_FALSE(listing);
    EXPECT_WIN32_ERROR_CODE(listing.error, ERROR_ACCESS_DENIED);

    // FILE_ADD_FILE is still granted, so creating a child file succeeds.
    auto created = core::CreateWin32File(dir + L"\\created.dat", GENERIC_WRITE,
                                         core::kShareAll, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL);
    EXPECT_TRUE(created) << core::FormatWin32Error(created.error);
  }

  // Deny FILE_ADD_FILE (0x0002), allow FILE_LIST_DIRECTORY and other rights.
  ASSERT_WIN32_OK(
      core::SetFileSddl(dir, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000002;;;WD)(A;;0x001F01FD;;;WD)"));
  {
    auto blocked = core::CreateWin32File(dir + L"\\blocked.dat", GENERIC_WRITE,
                                         core::kShareAll, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(blocked);
    EXPECT_WIN32_ERROR_CODE(blocked.error, ERROR_ACCESS_DENIED);

    auto listing = core::ListDirectoryWin32(dir);
    ASSERT_TRUE(listing) << core::FormatWin32Error(listing.error);
    EXPECT_MULTISET_EQ(
        *listing.value,
        (std::vector<std::wstring>{L".", L"..", L"seed.dat", L"created.dat"}));
  }

  ASSERT_WIN32_OK(
      core::SetFileSddl(dir, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  dir_guard.DisarmAndClose();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — without
// SeChangeNotifyPrivilege, directory FILE_TRAVERSE gates child path opens.
TEST_F(SecurityAclTest, DirectoryTraverseAccessEnforcement) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring dir = TestPath(L"traverse_dir");
  const std::wstring child = dir + L"\\child.dat";
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  auto dir_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"traverse_dir"));
  ASSERT_TRUE(dir_guard.armed());
  ASSERT_WIN32_OK(core::WriteFileContents(child, "traverse-ok"));
  ASSERT_WIN32_OK(
      core::SetFileSddl(child, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));

  auto no_bypass_token =
      core::CreateTokenWithoutPrivilege(SE_CHANGE_NOTIFY_NAME);
  ASSERT_TRUE(no_bypass_token) << core::FormatWin32Error(no_bypass_token.error);

  // Deny FILE_TRAVERSE (0x0020) on dir while allowing other rights.
  ASSERT_WIN32_OK(
      core::SetFileSddl(dir, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000020;;;WD)(A;;0x001F01DF;;;WD)"));
  {
    core::ScopedImpersonation imp(no_bypass_token.value->get());
    ASSERT_TRUE(imp.active()) << core::FormatWin32Error(imp.error());

    ExpectAccessDeniedOpen(child, GENERIC_READ);
  }

  // Grant FILE_TRAVERSE (0x0020) while denying FILE_LIST_DIRECTORY (0x0001).
  ASSERT_WIN32_OK(
      core::SetFileSddl(dir, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000001;;;WD)(A;;0x001F01FE;;;WD)"));
  {
    core::ScopedImpersonation imp(no_bypass_token.value->get());
    ASSERT_TRUE(imp.active()) << core::FormatWin32Error(imp.error());

    auto list_denied = core::ListDirectoryWin32(dir);
    EXPECT_FALSE(list_denied);
    EXPECT_WIN32_ERROR_CODE(list_denied.error, ERROR_ACCESS_DENIED);

    EXPECT_TRUE(helpers::CheckFileContents(child, "traverse-ok"));
  }

  ASSERT_WIN32_OK(
      core::SetFileSddl(dir, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  dir_guard.DisarmAndClose();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — an object owner retains
// implicit READ_CONTROL and WRITE_DAC even under an empty protected DACL.
TEST_F(SecurityAclTest, OwnerImplicitReadControlAndWriteDac) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"owner_implicit.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "owner-secret"));

  // Empty protected DACL denies all explicit access.
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"owner_implicit.dat"));
  ASSERT_TRUE(dacl_guard.armed());
  ASSERT_WIN32_OK(core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P"));

  auto read_denied =
      core::CreateWin32File(path, GENERIC_READ, core::kShareAll, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(read_denied);
  EXPECT_WIN32_ERROR_CODE(read_denied.error, ERROR_ACCESS_DENIED);

  auto write_denied =
      core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(write_denied);
  EXPECT_WIN32_ERROR_CODE(write_denied.error, ERROR_ACCESS_DENIED);

  // Use NtOpenFilePath with pure READ_CONTROL | WRITE_DAC (CreateFileW silently
  // adds FILE_READ_ATTRIBUTES | SYNCHRONIZE, which is denied by D:P).
  auto owner_h =
      core::NtOpenFilePath(TestNtPath(L"owner_implicit.dat"),
                           READ_CONTROL | WRITE_DAC, core::kShareAll, 0);
  ASSERT_NT_SUCCESS(owner_h.status);

  auto queried =
      QueryHandleSecurityNt(owner_h.value->get(), DACL_SECURITY_INFORMATION);
  ASSERT_NT_SUCCESS(queried.status);
  auto sddl = core::SecurityDescriptorToSddl(queried.value->data(),
                                             DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(sddl) << core::FormatWin32Error(sddl.error);
  EXPECT_EQ(*sddl.value, L"D:P");

  // Restore permissive DACL through the owner's WRITE_DAC handle.
  auto restore_sd = core::SddlToSecurityDescriptor(L"D:P(A;;FA;;;WD)");
  ASSERT_TRUE(restore_sd) << core::FormatWin32Error(restore_sd.error);
  const NTSTATUS set_st = core::Ntdll().NtSetSecurityObject(
      owner_h.value->get(), DACL_SECURITY_INFORMATION, restore_sd.value->get());
  ASSERT_NT_SUCCESS(set_st);
  owner_h.value->reset();
  dacl_guard.DisarmAndClose();

  auto content = core::ReadFileContents(path);
  ASSERT_TRUE(content) << core::FormatWin32Error(content.error);
  EXPECT_EQ(*content.value, "owner-secret");
}

// Spec: [MS-FSA] 2.1.5.14 / 2.1.5.17 — SeSecurityPrivilege enables
// ACCESS_SYSTEM_SECURITY handles to set and round-trip audit SACL ACEs.
TEST_F(SecurityAclTest, SaclGetAndSet) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);
  REQUIRE_PRIVILEGE(SE_SECURITY_NAME);

  const std::wstring path = TestPath(L"sacl_test.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "audit-payload"));

  core::ScopedPrivilege sec_priv(SE_SECURITY_NAME, /*enable=*/true);
  ASSERT_TRUE(sec_priv.active()) << core::FormatWin32Error(sec_priv.error());

  auto h = core::NtOpenFilePath(TestNtPath(L"sacl_test.dat"),
                                ACCESS_SYSTEM_SECURITY | READ_CONTROL,
                                core::kShareAll, 0);
  ASSERT_NT_SUCCESS(h.status);

  auto sacl_sd = core::SddlToSecurityDescriptor(L"S:P(AU;FA;GA;;;WD)");
  ASSERT_TRUE(sacl_sd) << core::FormatWin32Error(sacl_sd.error);

  ASSERT_NE(core::Ntdll().NtSetSecurityObject, nullptr);
  const NTSTATUS set_st = core::Ntdll().NtSetSecurityObject(
      h.value->get(), SACL_SECURITY_INFORMATION, sacl_sd.value->get());
  ASSERT_NT_SUCCESS(set_st);

  struct SaclRestoreGuard {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~SaclRestoreGuard() {
      auto empty_sacl = core::SddlToSecurityDescriptor(L"S:P");
      if (empty_sacl && core::Ntdll().NtSetSecurityObject != nullptr) {
        core::Ntdll().NtSetSecurityObject(handle, SACL_SECURITY_INFORMATION,
                                          empty_sacl.value->get());
      }
    }
  } sacl_restore{h.value->get()};

  auto queried =
      QueryHandleSecurityNt(h.value->get(), SACL_SECURITY_INFORMATION);
  ASSERT_NT_SUCCESS(queried.status);
  auto sddl = core::SecurityDescriptorToSddl(queried.value->data(),
                                             SACL_SECURITY_INFORMATION);
  ASSERT_TRUE(sddl) << core::FormatWin32Error(sddl.error);
  // Generic GA maps to file-specific FA in NTFS audit ACEs.
  EXPECT_EQ(*sddl.value, L"S:P(AU;FA;FA;;;WD)");
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — a NULL DACL grants all
// access whereas a present empty DACL denies non-owner-implicit access.
TEST_F(SecurityAclTest, NullDaclGrantsAllVsEmptyDaclDeniesAll) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"null_vs_empty_dacl.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "initial"));

  // NULL DACL (D:NO_ACCESS_CONTROL): present with nullptr PACL -> grants all.
  ASSERT_WIN32_OK(core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                                    L"D:NO_ACCESS_CONTROL"));
  {
    auto dacl_info = InspectFileDacl(path);
    ASSERT_TRUE(dacl_info) << core::FormatWin32Error(dacl_info.error);
    EXPECT_TRUE(dacl_info.value->present);
    EXPECT_TRUE(dacl_info.value->is_null);

    auto r = core::ReadFileContents(path);
    ASSERT_TRUE(r) << core::FormatWin32Error(r.error);
    EXPECT_EQ(*r.value, "initial");

    ASSERT_WIN32_OK(core::WriteFileContents(path, "updated-under-null-dacl"));
  }

  // Empty protected DACL (D:P): present with non-null 0-ACE PACL -> denies all
  // non-owner-implicit access while preserving owner READ_CONTROL | WRITE_DAC.
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"null_vs_empty_dacl.dat"));
  ASSERT_TRUE(dacl_guard.armed());
  ASSERT_WIN32_OK(core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P"));
  {
    auto dacl_info = InspectFileDacl(path);
    ASSERT_TRUE(dacl_info) << core::FormatWin32Error(dacl_info.error);
    EXPECT_TRUE(dacl_info.value->present);
    ASSERT_FALSE(dacl_info.value->is_null);
    EXPECT_EQ(dacl_info.value->ace_count, DWORD{0});

    ExpectAccessDeniedOpen(path, GENERIC_READ);
    ExpectAccessDeniedOpen(path, GENERIC_WRITE);

    // Owner still has implicit READ_CONTROL | WRITE_DAC.
    auto owner_h =
        core::NtOpenFilePath(TestNtPath(L"null_vs_empty_dacl.dat"),
                             READ_CONTROL | WRITE_DAC, core::kShareAll, 0);
    ASSERT_NT_SUCCESS(owner_h.status);
  }

  // Switching back to NULL DACL allows delete directly.
  ASSERT_WIN32_OK(core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                                    L"D:NO_ACCESS_CONTROL"));
  dacl_guard.DisarmAndClose();
  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: Win32 Order of ACEs in a DACL — explicit Deny overrides explicit
// Allow, while explicit child Allow precedes and overrides inherited Deny.
TEST_F(SecurityAclTest,
       ExplicitDenyOverridesAllowAndExplicitChildAllowOverridesInheritedDeny) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto wd_sid = StringToSid(L"S-1-1-0");
  ASSERT_TRUE(wd_sid) << core::FormatWin32Error(wd_sid.error);

  // Part A: Explicit Deny beats Explicit Allow on the same object.
  const std::wstring f1 = TestPath(L"explicit_deny_wins.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(f1, "part-a"));
  auto f1_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"explicit_deny_wins.dat"));
  ASSERT_TRUE(f1_guard.armed());
  // Deny FILE_READ_DATA (0x1), Allow FILE_ALL_ACCESS (FA).
  ASSERT_WIN32_OK(core::SetFileSddl(f1, DACL_SECURITY_INFORMATION,
                                    L"D:P(D;;0x00000001;;;WD)(A;;FA;;;WD)"));
  {
    ExpectAccessDeniedOpen(f1, GENERIC_READ);

    // FILE_WRITE_DATA is not denied and succeeds.
    auto w = core::CreateWin32File(f1, FILE_WRITE_DATA, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    EXPECT_TRUE(w) << core::FormatWin32Error(w.error);
  }
  ASSERT_WIN32_OK(
      core::SetFileSddl(f1, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  f1_guard.DisarmAndClose();

  // Part B: Explicit Allow on child overrides Inherited Deny from parent.
  const std::wstring parent = TestPath(L"inherited_deny_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  auto parent_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"inherited_deny_parent"));
  ASSERT_TRUE(parent_guard.armed());
  ASSERT_WIN32_OK(
      core::SetFileSddl(parent, DACL_SECURITY_INFORMATION,
                        L"D:P(D;OIIO;0x00000001;;;WD)(A;OICI;FA;;;WD)"));

  std::wstring child = parent + L"\\child.dat";
  {
    auto h = core::CreateWin32File(child, GENERIC_WRITE, core::kShareAll,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    ASSERT_WIN32_OK(core::WriteAllBytes(h.value->get(), "explicit-wins", 13));
  }
  auto child_guard = core::ScopedDaclRestore::Arm(
      TestNtPath(L"inherited_deny_parent\\child.dat"));
  ASSERT_TRUE(child_guard.armed());

  // Inherited Deny FILE_READ_DATA initially blocks reading child.
  ExpectAccessDeniedOpen(child, GENERIC_READ);

  // Apply explicit Allow FILE_ALL_ACCESS on child while keeping unprotected
  // inheritance so parent's inherited Deny remains in the DACL after it.
  auto explicit_sd = core::SddlToSecurityDescriptor(L"D:(A;;FA;;;WD)");
  ASSERT_TRUE(explicit_sd) << core::FormatWin32Error(explicit_sd.error);
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  PACL explicit_dacl = nullptr;
  ASSERT_WIN32_TRUE(::GetSecurityDescriptorDacl(explicit_sd.value->get(),
                                                &dacl_present, &explicit_dacl,
                                                &dacl_defaulted));
  ASSERT_TRUE(dacl_present != FALSE && explicit_dacl != nullptr);

  const DWORD set_err = ::SetNamedSecurityInfoW(
      child.data(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, explicit_dacl, nullptr);
  ASSERT_EQ(set_err, DWORD{ERROR_SUCCESS}) << core::FormatWin32Error(set_err);

  // Confirm both explicit Allow and inherited Deny are present in canonical
  // order (explicit Allow precedes inherited Deny).
  auto child_sd = QueryRawFileSecurity(child, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(child_sd) << core::FormatWin32Error(child_sd.error);
  const int explicit_allow_idx =
      FindMatchingAceIndex(child_sd.value->data(), ACCESS_ALLOWED_ACE_TYPE, 0,
                           INHERITED_ACE, FILE_ALL_ACCESS, wd_sid.value->get());
  const int inherited_deny_idx = FindMatchingAceIndex(
      child_sd.value->data(), ACCESS_DENIED_ACE_TYPE, INHERITED_ACE, 0,
      FILE_READ_DATA, wd_sid.value->get());
  EXPECT_GE(explicit_allow_idx, 0);
  EXPECT_GE(inherited_deny_idx, 0);
  EXPECT_LT(explicit_allow_idx, inherited_deny_idx);

  // Explicit Allow wins over inherited Deny: read now succeeds.
  EXPECT_TRUE(helpers::CheckFileContents(child, "explicit-wins"));

  ASSERT_WIN32_OK(core::SetFileSddl(parent, DACL_SECURITY_INFORMATION,
                                    L"D:P(A;OICI;FA;;;WD)"));
  child_guard.RestoreNow();
  parent_guard.DisarmAndClose();
}

// Spec: Win32 ACE Inheritance — INHERIT_ONLY applies only to children, and
// NO_PROPAGATE_INHERIT limits ACE propagation to one directory level.
TEST_F(SecurityAclTest, InheritanceFlagsInheritOnlyAndNoPropagate) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto au_sid = StringToSid(L"S-1-5-11"); // Authenticated Users
  auto wd_sid = StringToSid(L"S-1-1-0");  // Everyone
  ASSERT_TRUE(au_sid) << core::FormatWin32Error(au_sid.error);
  ASSERT_TRUE(wd_sid) << core::FormatWin32Error(wd_sid.error);

  // Part A: IO (INHERIT_ONLY_ACE) does not apply to parent itself, but becomes
  // effective on child objects.
  const std::wstring io_dir = TestPath(L"io_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(io_dir.c_str(), nullptr));
  auto io_dir_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"io_parent"));
  ASSERT_TRUE(io_dir_guard.armed());
  ASSERT_WIN32_OK(
      core::SetFileSddl(io_dir, DACL_SECURITY_INFORMATION,
                        L"D:P(D;OIIO;0x00000001;;;WD)(A;OICI;FA;;;WD)"));

  // Listing io_dir succeeds because the Deny 0x1 ACE has INHERIT_ONLY_ACE.
  {
    auto listing = core::ListDirectoryWin32(io_dir);
    ASSERT_TRUE(listing) << core::FormatWin32Error(listing.error);
  }

  // Child file inherits Deny FILE_READ_DATA (0x1) without IO, blocking reads.
  const std::wstring io_leaf = io_dir + L"\\leaf.dat";
  ASSERT_WIN32_OK(core::CreateWin32File(io_leaf, GENERIC_WRITE, core::kShareAll,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL));
  auto io_leaf_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"io_parent\\leaf.dat"));
  ASSERT_TRUE(io_leaf_guard.armed());
  ExpectAccessDeniedOpen(io_leaf, GENERIC_READ);

  ASSERT_WIN32_OK(core::SetFileSddl(io_leaf, DACL_SECURITY_INFORMATION,
                                    L"D:P(A;;FA;;;WD)"));
  io_leaf_guard.DisarmAndClose();
  ASSERT_WIN32_OK(core::SetFileSddl(io_dir, DACL_SECURITY_INFORMATION,
                                    L"D:P(A;OICI;FA;;;WD)"));
  io_dir_guard.DisarmAndClose();

  // Part B: NP (NO_PROPAGATE_INHERIT_ACE) propagates 1 level to child_dir
  // (stripping OI/CI), and does NOT propagate to grandchild_dir or
  // grandchild.dat.
  const std::wstring np_root = TestPath(L"np_root");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(np_root.c_str(), nullptr));
  auto np_root_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"np_root"));
  ASSERT_TRUE(np_root_guard.armed());
  ASSERT_WIN32_OK(SetProtectedInheritableDacl(
      np_root, L"D:P(A;OICINP;0x00000008;;;AU)(A;OICI;FA;;;WD)"));

  const std::wstring child_dir = np_root + L"\\child_dir";
  ASSERT_WIN32_TRUE(::CreateDirectoryW(child_dir.c_str(), nullptr));
  {
    auto buf = QueryRawFileSecurity(child_dir, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    // Child dir inherits AU 0x8 with INHERITED_ACE and OI|CI cleared by NP.
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   INHERITED_ACE,
                                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                   FILE_READ_EA, au_sid.value->get()),
              0);
    // Non-NP WD FA ACE still carries OI|CI|ID.
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE |
                                       INHERITED_ACE,
                                   0, FILE_ALL_ACCESS, wd_sid.value->get()),
              0);
  }

  const std::wstring grandchild_dir = child_dir + L"\\grandchild_dir";
  const std::wstring grandchild_file = child_dir + L"\\grandchild.dat";
  ASSERT_WIN32_TRUE(::CreateDirectoryW(grandchild_dir.c_str(), nullptr));
  ASSERT_WIN32_OK(core::CreateWin32File(grandchild_file, GENERIC_WRITE,
                                        core::kShareAll, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL));

  // Neither grandchild receives the NP AU 0x8 ACE, while both receive WD FA.
  for (const std::wstring &gc : {grandchild_dir, grandchild_file}) {
    auto buf = QueryRawFileSecurity(gc, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    EXPECT_EQ(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   0, 0, FILE_READ_EA, au_sid.value->get()),
              -1);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   INHERITED_ACE, 0, FILE_ALL_ACCESS,
                                   wd_sid.value->get()),
              0);
  }

  np_root_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — FILE_WRITE_ATTRIBUTES
// and FILE_WRITE_EA are enforced independently on attribute and EA opens.
TEST_F(SecurityAclTest, GranularAttributeAndEaAccessRightsEnforcement) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"granular_attr_ea.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "attr-ea-data"));
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"granular_attr_ea.dat"));
  ASSERT_TRUE(dacl_guard.armed());

  // Deny FILE_WRITE_ATTRIBUTES (0x0100) and FILE_WRITE_EA (0x0010), allow FR.
  ASSERT_WIN32_OK(core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                                    L"D:P(D;;0x00000110;;;WD)(A;;FR;;;WD)"));

  // Reading data succeeds.
  EXPECT_TRUE(helpers::CheckFileContents(path, "attr-ea-data"));

  // SetFileAttributesW and FILE_WRITE_ATTRIBUTES / FILE_WRITE_EA opens fail.
  EXPECT_WIN32_ERROR(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN),
                     ERROR_ACCESS_DENIED);
  ExpectAccessDeniedOpen(path, FILE_WRITE_ATTRIBUTES);
  ExpectAccessDeniedOpen(path, FILE_WRITE_EA);

  // Grant FILE_WRITE_ATTRIBUTES (FR | 0x100 = 0x00120189) while keeping
  // FILE_WRITE_EA (0x10) denied.
  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000010;;;WD)(A;;0x00120189;;;WD)"));

  ASSERT_WIN32_TRUE(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_ARCHIVE));
  {
    auto h = core::CreateWin32File(path, FILE_WRITE_ATTRIBUTES, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    const FILETIME ft = core::MakeUtcFileTime(2025, 6, 15);
    EXPECT_WIN32_TRUE(::SetFileTime(h.value->get(), nullptr, nullptr, &ft));
  }
  ExpectAccessDeniedOpen(path, FILE_WRITE_EA);

  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  dacl_guard.DisarmAndClose();
}

// Spec: [MS-FSA] 2.1.5.15.12 & Win32 MoveFileExW / FileRenameInformation —
// intra-volume rename preserves both inherited and explicit DACLs without
// re-inheriting the destination directory's DACL.
TEST_F(SecurityAclTest, SecurityDescriptorPreservedAcrossIntraVolumeRename) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto au_sid = StringToSid(L"S-1-5-11");     // Authenticated Users
  auto bu_sid = StringToSid(L"S-1-5-32-545"); // BUILTIN\Users
  ASSERT_TRUE(au_sid) << core::FormatWin32Error(au_sid.error);
  ASSERT_TRUE(bu_sid) << core::FormatWin32Error(bu_sid.error);

  const std::wstring dir_a = TestPath(L"sd_move_src_dir");
  const std::wstring dir_b = TestPath(L"sd_move_dst_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_b.c_str(), nullptr));

  auto dir_a_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"sd_move_src_dir"));
  ASSERT_TRUE(dir_a_guard.armed());
  auto dir_b_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"sd_move_dst_dir"));
  ASSERT_TRUE(dir_b_guard.armed());

  // Distinct inheritable marker ACEs on dir_a (AU 0x8) vs dir_b (BU 0x10).
  ASSERT_WIN32_OK(SetProtectedInheritableDacl(
      dir_a, L"D:P(A;OICI;0x00000008;;;AU)(A;OICI;FA;;;WD)"));
  ASSERT_WIN32_OK(SetProtectedInheritableDacl(
      dir_b, L"D:P(A;OICI;0x00000010;;;BU)(A;OICI;FA;;;WD)"));

  auto check_preserved_dir_a_dacl = [&](std::wstring_view target_path) {
    auto sd_buf = QueryRawFileSecurity(target_path, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(sd_buf) << core::FormatWin32Error(sd_buf.error);
    EXPECT_GE(FindMatchingAceIndex(sd_buf.value->data(),
                                   ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE, 0,
                                   FILE_READ_EA, au_sid.value->get()),
              0);
    EXPECT_EQ(FindMatchingAceIndex(sd_buf.value->data(),
                                   ACCESS_ALLOWED_ACE_TYPE, 0, 0, FILE_WRITE_EA,
                                   bu_sid.value->get()),
              -1);
  };

  // 1. Win32 MoveFileExW across directories preserves original dir_a DACL.
  const std::wstring win32_src = dir_a + L"\\moved_win32.dat";
  const std::wstring win32_dst = dir_b + L"\\moved_win32.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(win32_src, "move-win32-sd"));
  ASSERT_NO_FATAL_FAILURE(check_preserved_dir_a_dacl(win32_src));
  ASSERT_WIN32_TRUE(::MoveFileExW(win32_src.c_str(), win32_dst.c_str(), 0));
  ASSERT_NO_FATAL_FAILURE(check_preserved_dir_a_dacl(win32_dst));

  // 2. NT FileRenameInformation relative to dir_b preserves original dir_a DACL.
  const std::wstring nt_src = dir_a + L"\\moved_nt.dat";
  const std::wstring nt_dst = dir_b + L"\\moved_nt.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(nt_src, "move-nt-sd"));
  {
    auto dir_b_h = core::CreateWin32File(
        dir_b, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_TRUE(dir_b_h) << core::FormatWin32Error(dir_b_h.error);
    auto file_h =
        core::CreateWin32File(nt_src, DELETE | SYNCHRONIZE, core::kShareAll,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(file_h) << core::FormatWin32Error(file_h.error);
    auto ren = core::SetRenameInfo(file_h.value->get(), L"moved_nt.dat",
                                   dir_b_h.value->get(),
                                   /*replace_if_exists=*/false);
    ASSERT_NT_SUCCESS(ren.status);
  }
  ASSERT_NO_FATAL_FAILURE(check_preserved_dir_a_dacl(nt_dst));

  // 3. Explicit protected DACL is also preserved across intra-volume rename.
  const std::wstring explicit_src = dir_a + L"\\moved_explicit.dat";
  const std::wstring explicit_dst = dir_b + L"\\moved_explicit.dat";
  {
    auto h = CreateFileWithSddl(explicit_src, L"D:P(A;;FR;;;AU)(A;;FA;;;WD)",
                                GENERIC_WRITE, core::kShareAll);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
  }
  ASSERT_WIN32_TRUE(
      ::MoveFileExW(explicit_src.c_str(), explicit_dst.c_str(), 0));
  {
    auto after_sddl =
        core::GetFileSddl(explicit_dst, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(after_sddl) << core::FormatWin32Error(after_sddl.error);
    EXPECT_EQ(*after_sddl.value, L"D:P(A;;FR;;;AU)(A;;FA;;;WD)");
  }

  dir_b_guard.RestoreNow();
  dir_a_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.17 & Win32 Privileges — SeTakeOwnershipPrivilege
// permits WRITE_OWNER opens and owner updates even under read-only DACLs.
TEST_F(SecurityAclTest, TakeOwnershipWithPrivilegeAndWriteOwner) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);
  REQUIRE_ADMIN();
  REQUIRE_PRIVILEGE(SE_TAKE_OWNERSHIP_NAME);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtSetSecurityObject, nullptr);

  auto user_sid = core::GetCurrentUserSid();
  auto ba_sid = StringToSid(L"S-1-5-32-544"); // BUILTIN\Administrators
  ASSERT_TRUE(user_sid) << core::FormatWin32Error(user_sid.error);
  ASSERT_TRUE(ba_sid) << core::FormatWin32Error(ba_sid.error);

  const std::wstring path = TestPath(L"take_ownership.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "owner-change"));
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"take_ownership.dat"));
  ASSERT_TRUE(dacl_guard.armed());
  // Restrict DACL to read-only (no WRITE_OWNER) so opening WRITE_OWNER relies
  // on SeTakeOwnershipPrivilege.
  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(A;;FR;;;WD)"));

  core::ScopedPrivilege take_own_priv(SE_TAKE_OWNERSHIP_NAME, /*enable=*/true);
  ASSERT_TRUE(take_own_priv.active())
      << core::FormatWin32Error(take_own_priv.error());

  auto h = core::NtOpenFilePath(TestNtPath(L"take_ownership.dat"),
                                WRITE_OWNER | READ_CONTROL, core::kShareAll, 0);
  ASSERT_NT_SUCCESS(h.status);

  auto set_and_verify_owner = [&](PSID target_sid) {
    SECURITY_DESCRIPTOR sd{};
    ASSERT_WIN32_TRUE(
        ::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION));
    ASSERT_WIN32_TRUE(::SetSecurityDescriptorOwner(&sd, target_sid, FALSE));
    const NTSTATUS st = ntdll.NtSetSecurityObject(
        h.value->get(), OWNER_SECURITY_INFORMATION, &sd);
    ASSERT_NT_SUCCESS(st);

    auto queried =
        QueryHandleSecurityNt(h.value->get(), OWNER_SECURITY_INFORMATION);
    ASSERT_NT_SUCCESS(queried.status);
    PSID actual_owner = nullptr;
    BOOL defaulted = FALSE;
    ASSERT_WIN32_TRUE(::GetSecurityDescriptorOwner(queried.value->data(),
                                                   &actual_owner, &defaulted));
    ASSERT_NE(actual_owner, nullptr);
    EXPECT_TRUE(::EqualSid(actual_owner, target_sid) != FALSE);
  };

  // Start from current user SID, arm RAII owner restore before switching to BA,
  // then explicitly restore current user SID.
  ASSERT_NO_FATAL_FAILURE(set_and_verify_owner(user_sid.value->get()));
  ScopedOwnerRestore owner_guard(h.value->get(), user_sid.value->get());
  ASSERT_NO_FATAL_FAILURE(set_and_verify_owner(ba_sid.value->get()));
  ASSERT_NO_FATAL_FAILURE(set_and_verify_owner(user_sid.value->get()));
  owner_guard.Disarm();

  h.value->reset();
  ASSERT_WIN32_OK(
      core::SetFileSddl(path, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)"));
  dacl_guard.DisarmAndClose();
}

// Spec: [MS-FSA] 2.1.5.14 / 2.1.5.17 — NtQuerySecurityObject reports exact
// STATUS_BUFFER_TOO_SMALL lengths and round-trips NtSetSecurityObject DACLs.
TEST_F(SecurityAclTest, NtQueryAndSetSecurityObjectDirect) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQuerySecurityObject, nullptr);
  ASSERT_NE(ntdll.NtSetSecurityObject, nullptr);

  const std::wstring path = TestPath(L"nt_sec_direct.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "nt-sec"));
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"nt_sec_direct.dat"));
  ASSERT_TRUE(dacl_guard.armed());

  auto h = core::NtOpenFilePath(TestNtPath(L"nt_sec_direct.dat"),
                                READ_CONTROL | WRITE_DAC, core::kShareAll, 0);
  ASSERT_NT_SUCCESS(h.status);

  // Zero-byte and undersized probes must return STATUS_BUFFER_TOO_SMALL with
  // exact required length.
  ULONG needed = 0;
  NTSTATUS st = ntdll.NtQuerySecurityObject(
      h.value->get(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
  EXPECT_NT_STATUS(st, STATUS_BUFFER_TOO_SMALL);
  ASSERT_GT(needed, ULONG{0});

  alignas(8) uint8_t tiny[4]{};
  ULONG needed_tiny = 0;
  st = ntdll.NtQuerySecurityObject(h.value->get(), DACL_SECURITY_INFORMATION,
                                   tiny, sizeof(tiny), &needed_tiny);
  EXPECT_NT_STATUS(st, STATUS_BUFFER_TOO_SMALL);
  EXPECT_EQ(needed_tiny, needed);

  std::vector<uint8_t> initial_buf(needed, 0);
  ULONG returned = 0;
  st = ntdll.NtQuerySecurityObject(h.value->get(), DACL_SECURITY_INFORMATION,
                                   initial_buf.data(), needed, &returned);
  ASSERT_NT_SUCCESS(st);

  // Set exact DACL via NtSetSecurityObject and round-trip via
  // NtQuerySecurityObject.
  const std::wstring target_sddl = L"D:P(A;;FR;;;WD)(A;;FA;;;SY)";
  auto target_sd = core::SddlToSecurityDescriptor(target_sddl);
  ASSERT_TRUE(target_sd) << core::FormatWin32Error(target_sd.error);
  st = ntdll.NtSetSecurityObject(h.value->get(), DACL_SECURITY_INFORMATION,
                                 target_sd.value->get());
  ASSERT_NT_SUCCESS(st);

  auto roundtrip =
      QueryHandleSecurityNt(h.value->get(), DACL_SECURITY_INFORMATION);
  ASSERT_NT_SUCCESS(roundtrip.status);
  auto roundtrip_sddl = core::SecurityDescriptorToSddl(
      roundtrip.value->data(), DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(roundtrip_sddl) << core::FormatWin32Error(roundtrip_sddl.error);
  EXPECT_EQ(*roundtrip_sddl.value, target_sddl);

  // Restore full access for teardown.
  auto restore_sd = core::SddlToSecurityDescriptor(L"D:P(A;;FA;;;WD)");
  ASSERT_TRUE(restore_sd) << core::FormatWin32Error(restore_sd.error);
  EXPECT_NT_SUCCESS(ntdll.NtSetSecurityObject(
      h.value->get(), DACL_SECURITY_INFORMATION, restore_sd.value->get()));
  dacl_guard.DisarmAndClose();
}

// Spec: [MS-FSA] 2.1.5.1.1 / Win32 CopyFileW & CopyFileExW — copied files
// inherit the destination directory's DACL rather than carrying either
// inherited or explicit source DACL ACEs.
TEST_F(SecurityAclTest, CopyFileInheritsDestinationDaclUnlikeMove) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto user_sid = core::GetCurrentUserSid();
  auto user_sid_str = core::GetCurrentUserSidString();
  auto wd_sid = StringToSid(L"S-1-1-0");      // Everyone
  auto au_sid = StringToSid(L"S-1-5-11");     // Authenticated Users
  auto bu_sid = StringToSid(L"S-1-5-32-545"); // BUILTIN\Users
  ASSERT_TRUE(user_sid) << core::FormatWin32Error(user_sid.error);
  ASSERT_TRUE(user_sid_str) << core::FormatWin32Error(user_sid_str.error);
  ASSERT_TRUE(wd_sid) << core::FormatWin32Error(wd_sid.error);
  ASSERT_TRUE(au_sid) << core::FormatWin32Error(au_sid.error);
  ASSERT_TRUE(bu_sid) << core::FormatWin32Error(bu_sid.error);

  const std::wstring src_dir = TestPath(L"copy_dacl_src");
  const std::wstring dst_dir = TestPath(L"copy_dacl_dst");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(src_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dst_dir.c_str(), nullptr));

  auto src_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"copy_dacl_src"));
  ASSERT_TRUE(src_guard.armed());
  auto dst_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"copy_dacl_dst"));
  ASSERT_TRUE(dst_guard.armed());

  const std::wstring src_sddl =
      L"D:P(A;OICI;FR;;;WD)(A;OICI;FA;;;" + *user_sid_str.value + L")";
  const std::wstring dst_sddl =
      L"D:P(A;OICI;FR;;;BU)(A;OICI;FA;;;" + *user_sid_str.value + L")";
  auto set_src = SetProtectedInheritableDacl(src_dir, src_sddl);
  ASSERT_TRUE(set_src) << core::FormatWin32Error(set_src.error);
  auto set_dst = SetProtectedInheritableDacl(dst_dir, dst_sddl);
  ASSERT_TRUE(set_dst) << core::FormatWin32Error(set_dst.error);

  auto verify_inherited_dst_dacl = [&](std::wstring_view copied_path) {
    auto copied_sddl =
        core::GetFileSddl(copied_path, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(copied_sddl) << core::FormatWin32Error(copied_sddl.error);
    EXPECT_EQ(copied_sddl.value->find(L";;;WD)"), std::wstring::npos)
        << "Copied file unexpectedly retained WD ACE: "
        << core::WideToUtf8(*copied_sddl.value);
    EXPECT_NE(copied_sddl.value->find(L";;;BU)"), std::wstring::npos)
        << "Copied file did not inherit destination BU ACE: "
        << core::WideToUtf8(*copied_sddl.value);

    auto buf = QueryRawFileSecurity(copied_path, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    EXPECT_FALSE(HasAceForSid(buf.value->data(), wd_sid.value->get()));
    EXPECT_FALSE(HasAceForSid(buf.value->data(), au_sid.value->get()));
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   INHERITED_ACE, 0, FILE_GENERIC_READ,
                                   bu_sid.value->get()),
              0);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   INHERITED_ACE, 0, FILE_ALL_ACCESS,
                                   user_sid.value->get()),
              0);
  };

  // 1. CopyFileW of a file with inherited src_dir DACL inherits dst_dir DACL.
  const std::wstring src_inherited = src_dir + L"\\inherited_src.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(src_inherited, "copy-dacl-payload"));
  {
    auto buf = QueryRawFileSecurity(src_inherited, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   INHERITED_ACE, 0, FILE_GENERIC_READ,
                                   wd_sid.value->get()),
              0);
    EXPECT_FALSE(HasAceForSid(buf.value->data(), bu_sid.value->get()));
  }

  const std::wstring dst_win32 = dst_dir + L"\\copied_win32.dat";
  ASSERT_WIN32_TRUE(::CopyFileW(src_inherited.c_str(), dst_win32.c_str(),
                                /*bFailIfExists=*/FALSE));
  EXPECT_TRUE(helpers::CheckFileContents(dst_win32, "copy-dacl-payload"));
  ASSERT_NO_FATAL_FAILURE(verify_inherited_dst_dacl(dst_win32));

  // 2. CopyFileExW of a file with an explicit protected DACL (AU FR + WD FA)
  // still inherits dst_dir's DACL rather than carrying the explicit ACEs.
  const std::wstring src_explicit = src_dir + L"\\explicit_src.dat";
  {
    auto h = CreateFileWithSddl(src_explicit, L"D:P(A;;FR;;;AU)(A;;FA;;;WD)",
                                GENERIC_WRITE, core::kShareAll);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), "explicit-src-payload", 20));
  }
  const std::wstring dst_ex = dst_dir + L"\\copied_ex.dat";
  ASSERT_WIN32_TRUE(::CopyFileExW(src_explicit.c_str(), dst_ex.c_str(), nullptr,
                                  nullptr, nullptr, 0));
  EXPECT_TRUE(helpers::CheckFileContents(dst_ex, "explicit-src-payload"));
  ASSERT_NO_FATAL_FAILURE(verify_inherited_dst_dacl(dst_ex));

  src_guard.RestoreNow();
  dst_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — execute-only DACLs deny
// FILE_READ_DATA and data mappings while allowing SEC_IMAGE sections.
TEST_F(SecurityAclTest, FileExecuteOnlyAllowsImageMappingDeniesReadData) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring exe_copy = TestPath(L"exec_only_image.exe");
  auto copied = CopyCurrentExecutableTo(exe_copy);
  ASSERT_TRUE(copied) << core::FormatWin32Error(copied.error);

  // Grant FILE_GENERIC_EXECUTE (FILE_EXECUTE | FILE_READ_ATTRIBUTES |
  // READ_CONTROL | SYNCHRONIZE = 0x001200A0) without FILE_READ_DATA.
  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"exec_only_image.exe"));
  ASSERT_TRUE(dacl_guard.armed());
  auto set_res = core::SetFileSddl(exe_copy, DACL_SECURITY_INFORMATION,
                                   L"D:P(A;;0x001200A0;;;WD)");
  ASSERT_TRUE(set_res) << core::FormatWin32Error(set_res.error);

  // Opening for FILE_READ_DATA is denied by the execute-only DACL.
  ExpectAccessDeniedOpen(exe_copy, FILE_READ_DATA);

  // Opening with FILE_EXECUTE | SYNCHRONIZE succeeds, denies ReadFile and
  // data-section mapping, and permits execute-only SEC_IMAGE section creation
  // via NtCreateSection (the loader path for execute-only PE images).
  {
    const auto &ntdll = core::Ntdll();
    ASSERT_NE(ntdll.NtCreateSection, nullptr);

    auto h_exec = core::CreateWin32File(exe_copy, FILE_EXECUTE | SYNCHRONIZE,
                                        core::kShareAll, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h_exec) << core::FormatWin32Error(h_exec.error);

    char read_buf[16]{};
    DWORD bytes_read = 0;
    EXPECT_WIN32_ERROR(::ReadFile(h_exec.value->get(), read_buf,
                                  static_cast<DWORD>(sizeof(read_buf)),
                                  &bytes_read, nullptr),
                       ERROR_ACCESS_DENIED);

    ::SetLastError(ERROR_SUCCESS);
    core::UniqueHandle data_mapping(::CreateFileMappingW(
        h_exec.value->get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    const DWORD map_err = ::GetLastError();
    EXPECT_FALSE(data_mapping.valid());
    EXPECT_WIN32_ERROR_CODE(map_err, ERROR_ACCESS_DENIED);

    HANDLE raw_sec = nullptr;
    const NTSTATUS st =
        ntdll.NtCreateSection(&raw_sec, SECTION_MAP_EXECUTE, nullptr, nullptr,
                              PAGE_EXECUTE, SEC_IMAGE, h_exec.value->get());
    core::UniqueHandle sec(raw_sec);
    EXPECT_NT_STATUS(st, STATUS_SUCCESS);
    EXPECT_TRUE(sec.valid());
  }

  dacl_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 Privileges — requesting
// ACCESS_SYSTEM_SECURITY without SeSecurityPrivilege fails privilege check.
TEST_F(SecurityAclTest, AccessSystemSecurityDeniedWithoutSeSecurityPrivilege) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"sacl_priv_check.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "sacl-priv-test"));

  // Explicitly disable SeSecurityPrivilege on the current process token. If the
  // token does not possess SeSecurityPrivilege (e.g. non-elevated runs),
  // ScopedPrivilege fails harmlessly with ERROR_NOT_ALL_ASSIGNED and the
  // privilege is already absent.
  core::ScopedPrivilege disable_sec(SE_SECURITY_NAME, /*enable=*/false);
  ASSERT_TRUE(disable_sec.active() ||
              disable_sec.error() == ERROR_NOT_ALL_ASSIGNED)
      << core::FormatWin32Error(disable_sec.error());

  auto denied =
      core::CreateWin32File(path, ACCESS_SYSTEM_SECURITY, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(denied);
  EXPECT_WIN32_ERROR_CODE(denied.error, ERROR_PRIVILEGE_NOT_HELD);
}

// Spec: [MS-FSA] 2.1.5.1.1 & Win32 ACE Inheritance — an inheritable
// CREATOR OWNER (CO) ACE expands on child files to the creator owner's SID.
TEST_F(SecurityAclTest, CreatorOwnerSidExpandsOnInheritedAce) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto expected_owner_sid = QueryCurrentTokenOwnerSid();
  auto co_sid = StringToSid(L"S-1-3-0"); // CREATOR OWNER
  auto wd_sid = StringToSid(L"S-1-1-0"); // Everyone
  ASSERT_TRUE(expected_owner_sid)
      << core::FormatWin32Error(expected_owner_sid.error);
  ASSERT_TRUE(co_sid) << core::FormatWin32Error(co_sid.error);
  ASSERT_TRUE(wd_sid) << core::FormatWin32Error(wd_sid.error);

  const std::wstring parent = TestPath(L"co_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  auto parent_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"co_parent"));
  ASSERT_TRUE(parent_guard.armed());

  // Grant inheritable GENERIC_ALL to CREATOR OWNER (inherit-only on parent)
  // and non-inheritable full control to Everyone on the parent directory only.
  auto set_parent =
      SetProtectedInheritableDacl(parent, L"D:P(A;OICIIO;GA;;;CO)(A;;FA;;;WD)");
  ASSERT_TRUE(set_parent) << core::FormatWin32Error(set_parent.error);

  {
    auto parent_buf = QueryRawFileSecurity(parent, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(parent_buf) << core::FormatWin32Error(parent_buf.error);
    EXPECT_TRUE(HasAceForSid(parent_buf.value->data(), co_sid.value->get()));
  }

  const std::wstring child_file = parent + L"\\co_child.dat";
  {
    auto h = core::CreateWin32File(child_file, GENERIC_WRITE, core::kShareAll,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    ASSERT_WIN32_OK(
        core::WriteAllBytes(h.value->get(), "creator-owner-ok", 16));
  }
  auto child_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"co_parent\\co_child.dat"));
  ASSERT_TRUE(child_guard.armed());

  // Verify the child file's owner SID equals the current token's default owner
  // (TokenOwner: TokenUser for standard users, Administrators when elevated).
  {
    auto owner_buf =
        QueryRawFileSecurity(child_file, OWNER_SECURITY_INFORMATION);
    ASSERT_TRUE(owner_buf) << core::FormatWin32Error(owner_buf.error);
    PSID child_owner_sid = nullptr;
    BOOL owner_defaulted = FALSE;
    ASSERT_WIN32_TRUE(::GetSecurityDescriptorOwner(
        owner_buf.value->data(), &child_owner_sid, &owner_defaulted));
    ASSERT_NE(child_owner_sid, nullptr);
    EXPECT_TRUE(::EqualSid(child_owner_sid, expected_owner_sid.value->get()) !=
                FALSE);
  }

  // Child's inherited DACL must expand CO (S-1-3-0) into an effective
  // INHERITED_ACE granting FILE_ALL_ACCESS to the creator owner's concrete SID.
  auto child_sddl = core::GetFileSddl(child_file, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(child_sddl) << core::FormatWin32Error(child_sddl.error);
  EXPECT_EQ(child_sddl.value->find(L";;;CO)"), std::wstring::npos)
      << "Child DACL must expand CO, got: "
      << core::WideToUtf8(*child_sddl.value);
  EXPECT_EQ(child_sddl.value->find(L"S-1-3-0"), std::wstring::npos)
      << "Child DACL must not retain literal S-1-3-0, got: "
      << core::WideToUtf8(*child_sddl.value);

  auto child_buf = QueryRawFileSecurity(child_file, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(child_buf) << core::FormatWin32Error(child_buf.error);
  EXPECT_FALSE(HasAceForSid(child_buf.value->data(), co_sid.value->get()));
  EXPECT_FALSE(HasAceForSid(child_buf.value->data(), wd_sid.value->get()));
  EXPECT_GE(FindMatchingAceIndex(
                child_buf.value->data(), ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE,
                OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE,
                FILE_ALL_ACCESS, expected_owner_sid.value->get()),
            0)
      << "Expected expanded inherited FILE_ALL_ACCESS ACE for creator owner in "
      << core::WideToUtf8(*child_sddl.value);

  // Expanded ACE grants effective read access to the creating user.
  EXPECT_TRUE(helpers::CheckFileContents(child_file, "creator-owner-ok"));

  child_guard.RestoreNow();
  parent_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — FILE_ADD_FILE (0x0002)
// and FILE_ADD_SUBDIRECTORY (0x0004) are enforced independently on dirs.
TEST_F(SecurityAclTest, DirectoryAddFileVsAddSubdirectoryRightsDistinct) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  constexpr DWORD kAddFileOnlyMask = FILE_ADD_FILE | FILE_LIST_DIRECTORY |
                                     FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
                                     READ_CONTROL | SYNCHRONIZE;
  constexpr DWORD kAddSubdirOnlyMask =
      FILE_ADD_SUBDIRECTORY | FILE_LIST_DIRECTORY | FILE_TRAVERSE |
      FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
  static_assert(kAddFileOnlyMask == 0x001200A3UL);
  static_assert(kAddSubdirOnlyMask == 0x001200A5UL);

  const std::wstring dir_a = TestPath(L"add_file_only_dir");
  const std::wstring dir_b = TestPath(L"add_subdir_only_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_a.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir_b.c_str(), nullptr));

  auto dir_a_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"add_file_only_dir"));
  ASSERT_TRUE(dir_a_guard.armed());
  auto dir_b_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"add_subdir_only_dir"));
  ASSERT_TRUE(dir_b_guard.armed());

  // Dir A: grants FILE_ADD_FILE (0x0002) without FILE_ADD_SUBDIRECTORY.
  auto set_a = core::SetFileSddl(dir_a, DACL_SECURITY_INFORMATION,
                                 L"D:P(A;;0x001200A3;;;WD)");
  ASSERT_TRUE(set_a) << core::FormatWin32Error(set_a.error);
  {
    const std::wstring allowed_file = dir_a + L"\\allowed_file.dat";
    auto created_file =
        core::CreateWin32File(allowed_file, GENERIC_WRITE, core::kShareAll,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(created_file) << core::FormatWin32Error(created_file.error);
    ASSERT_WIN32_OK(
        core::WriteAllBytes(created_file.value->get(), "file-ok", 7));

    const std::wstring blocked_sub = dir_a + L"\\blocked_subdir";
    EXPECT_WIN32_ERROR(::CreateDirectoryW(blocked_sub.c_str(), nullptr),
                       ERROR_ACCESS_DENIED);
    EXPECT_TRUE(helpers::CheckFileNotFound(blocked_sub));
  }

  // Dir B: grants FILE_ADD_SUBDIRECTORY (0x0004) without FILE_ADD_FILE.
  auto set_b = core::SetFileSddl(dir_b, DACL_SECURITY_INFORMATION,
                                 L"D:P(A;;0x001200A5;;;WD)");
  ASSERT_TRUE(set_b) << core::FormatWin32Error(set_b.error);
  {
    const std::wstring allowed_sub = dir_b + L"\\allowed_subdir";
    ASSERT_WIN32_TRUE(::CreateDirectoryW(allowed_sub.c_str(), nullptr));
    const DWORD sub_attrs = ::GetFileAttributesW(allowed_sub.c_str());
    ASSERT_NE(sub_attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_NE(sub_attrs & FILE_ATTRIBUTE_DIRECTORY, DWORD{0});

    const std::wstring blocked_file = dir_b + L"\\blocked_file.dat";
    auto denied_file =
        core::CreateWin32File(blocked_file, GENERIC_WRITE, core::kShareAll,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(denied_file);
    EXPECT_WIN32_ERROR_CODE(denied_file.error, ERROR_ACCESS_DENIED);
    EXPECT_TRUE(helpers::CheckFileNotFound(blocked_file));
  }

  dir_b_guard.RestoreNow();
  dir_a_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.1 / Win32 ACE Inheritance — OBJECT_INHERIT_ACE
// (OI)-only propagates effectively to files and as INHERIT_ONLY to subdirs,
// whereas CONTAINER_INHERIT_ACE (CI)-only propagates solely to subdirs.
TEST_F(SecurityAclTest, ObjectInheritOnlyVsContainerInheritOnly) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto au_sid = StringToSid(L"S-1-5-11"); // Authenticated Users
  ASSERT_TRUE(au_sid) << core::FormatWin32Error(au_sid.error);

  // Part 1: (OI)-only ACE on parent directory.
  {
    const std::wstring oi_parent = TestPath(L"oi_only_parent");
    ASSERT_WIN32_TRUE(::CreateDirectoryW(oi_parent.c_str(), nullptr));
    auto oi_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"oi_only_parent"));
    ASSERT_TRUE(oi_guard.armed());
    ASSERT_WIN32_OK(SetProtectedInheritableDacl(
        oi_parent, L"D:P(A;OI;0x00000008;;;AU)(A;OICI;FA;;;WD)"));

    const std::wstring child_file = oi_parent + L"\\child.dat";
    ASSERT_WIN32_OK(core::WriteFileContents(child_file, "oi-file"));
    {
      auto buf = QueryRawFileSecurity(child_file, DACL_SECURITY_INFORMATION);
      ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
      EXPECT_GE(
          FindMatchingAceIndex(
              buf.value->data(), ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE,
              OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE,
              FILE_READ_EA, au_sid.value->get()),
          0);
    }

    const std::wstring child_dir = oi_parent + L"\\child_dir";
    ASSERT_WIN32_TRUE(::CreateDirectoryW(child_dir.c_str(), nullptr));
    {
      auto buf = QueryRawFileSecurity(child_dir, DACL_SECURITY_INFORMATION);
      ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
      // Subdirectory receives (OI)-only ACE as OI|IO|ID (inherit-only, not
      // effective on the directory itself).
      EXPECT_GE(FindMatchingAceIndex(
                    buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                    OBJECT_INHERIT_ACE | INHERIT_ONLY_ACE | INHERITED_ACE,
                    CONTAINER_INHERIT_ACE, FILE_READ_EA, au_sid.value->get()),
                0);
    }

    const std::wstring gc_file = child_dir + L"\\grandchild.dat";
    ASSERT_WIN32_OK(core::WriteFileContents(gc_file, "oi-gc-file"));
    {
      auto buf = QueryRawFileSecurity(gc_file, DACL_SECURITY_INFORMATION);
      ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
      EXPECT_GE(
          FindMatchingAceIndex(
              buf.value->data(), ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE,
              OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE,
              FILE_READ_EA, au_sid.value->get()),
          0);
    }

    oi_guard.RestoreNow();
  }

  // Part 2: (CI)-only ACE on parent directory.
  {
    const std::wstring ci_parent = TestPath(L"ci_only_parent");
    ASSERT_WIN32_TRUE(::CreateDirectoryW(ci_parent.c_str(), nullptr));
    auto ci_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"ci_only_parent"));
    ASSERT_TRUE(ci_guard.armed());
    ASSERT_WIN32_OK(SetProtectedInheritableDacl(
        ci_parent, L"D:P(A;CI;0x00000010;;;AU)(A;OICI;FA;;;WD)"));

    const std::wstring child_file = ci_parent + L"\\child.dat";
    ASSERT_WIN32_OK(core::WriteFileContents(child_file, "ci-file"));
    {
      auto buf = QueryRawFileSecurity(child_file, DACL_SECURITY_INFORMATION);
      ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
      EXPECT_EQ(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                     0, 0, FILE_WRITE_EA, au_sid.value->get()),
                -1);
    }

    const std::wstring child_dir = ci_parent + L"\\child_dir";
    ASSERT_WIN32_TRUE(::CreateDirectoryW(child_dir.c_str(), nullptr));
    {
      auto buf = QueryRawFileSecurity(child_dir, DACL_SECURITY_INFORMATION);
      ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
      EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                     CONTAINER_INHERIT_ACE | INHERITED_ACE,
                                     OBJECT_INHERIT_ACE | INHERIT_ONLY_ACE,
                                     FILE_WRITE_EA, au_sid.value->get()),
                0);
    }

    const std::wstring gc_dir = child_dir + L"\\grandchild_dir";
    const std::wstring gc_file = child_dir + L"\\grandchild.dat";
    ASSERT_WIN32_TRUE(::CreateDirectoryW(gc_dir.c_str(), nullptr));
    ASSERT_WIN32_OK(core::WriteFileContents(gc_file, "ci-gc-file"));
    {
      auto dir_buf = QueryRawFileSecurity(gc_dir, DACL_SECURITY_INFORMATION);
      ASSERT_TRUE(dir_buf) << core::FormatWin32Error(dir_buf.error);
      EXPECT_GE(FindMatchingAceIndex(dir_buf.value->data(),
                                     ACCESS_ALLOWED_ACE_TYPE,
                                     CONTAINER_INHERIT_ACE | INHERITED_ACE,
                                     OBJECT_INHERIT_ACE | INHERIT_ONLY_ACE,
                                     FILE_WRITE_EA, au_sid.value->get()),
                0);

      auto file_buf = QueryRawFileSecurity(gc_file, DACL_SECURITY_INFORMATION);
      ASSERT_TRUE(file_buf) << core::FormatWin32Error(file_buf.error);
      EXPECT_EQ(FindMatchingAceIndex(file_buf.value->data(),
                                     ACCESS_ALLOWED_ACE_TYPE, 0, 0,
                                     FILE_WRITE_EA, au_sid.value->get()),
                -1);
    }

    ci_guard.RestoreNow();
  }
}

// Spec: [MS-FSA] 2.1.5.14 / 2.1.5.17 — NtQuerySecurityObject requires
// READ_CONTROL and NtSetSecurityObject enforces WRITE_DAC and WRITE_OWNER on
// the handle access mask.
TEST_F(SecurityAclTest, SecurityObjectApisEnforceHandleAccessMasks) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const auto &ntdll = core::Ntdll();
  ASSERT_NE(ntdll.NtQuerySecurityObject, nullptr);
  ASSERT_NE(ntdll.NtSetSecurityObject, nullptr);

  const std::wstring path = TestPath(L"sec_handle_masks.dat");
  const std::wstring nt_path = TestNtPath(L"sec_handle_masks.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "mask-enforcement"));
  auto dacl_guard = core::ScopedDaclRestore::Arm(nt_path);
  ASSERT_TRUE(dacl_guard.armed());

  auto dacl_sd = core::SddlToSecurityDescriptor(L"D:P(A;;FA;;;WD)");
  ASSERT_TRUE(dacl_sd) << core::FormatWin32Error(dacl_sd.error);
  auto owner_sid = QueryCurrentTokenOwnerSid();
  ASSERT_TRUE(owner_sid) << core::FormatWin32Error(owner_sid.error);
  SECURITY_DESCRIPTOR owner_sd{};
  ASSERT_WIN32_TRUE(
      ::InitializeSecurityDescriptor(&owner_sd, SECURITY_DESCRIPTOR_REVISION));
  ASSERT_WIN32_TRUE(
      ::SetSecurityDescriptorOwner(&owner_sd, owner_sid.value->get(), FALSE));

  // 1. Handle without READ_CONTROL fails NtQuerySecurityObject with
  // STATUS_ACCESS_DENIED.
  {
    auto h = core::NtOpenFilePath(nt_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                  core::kShareAll, 0);
    ASSERT_NT_SUCCESS(h.status);
    std::vector<uint8_t> buf(256, 0);
    ULONG needed = 0;
    EXPECT_NT_STATUS(ntdll.NtQuerySecurityObject(
                         h.value->get(), DACL_SECURITY_INFORMATION, buf.data(),
                         static_cast<ULONG>(buf.size()), &needed),
                     STATUS_ACCESS_DENIED);
    EXPECT_NT_STATUS(ntdll.NtQuerySecurityObject(
                         h.value->get(), OWNER_SECURITY_INFORMATION, buf.data(),
                         static_cast<ULONG>(buf.size()), &needed),
                     STATUS_ACCESS_DENIED);
  }

  // 2. Handle with READ_CONTROL only can query, but fails NtSetSecurityObject
  // for DACL (missing WRITE_DAC) and OWNER (missing WRITE_OWNER).
  {
    auto h = core::NtOpenFilePath(nt_path, READ_CONTROL | SYNCHRONIZE,
                                  core::kShareAll, 0);
    ASSERT_NT_SUCCESS(h.status);
    auto queried =
        QueryHandleSecurityNt(h.value->get(), DACL_SECURITY_INFORMATION);
    ASSERT_NT_SUCCESS(queried.status);

    EXPECT_NT_STATUS(ntdll.NtSetSecurityObject(h.value->get(),
                                               DACL_SECURITY_INFORMATION,
                                               dacl_sd.value->get()),
                     STATUS_ACCESS_DENIED);
    EXPECT_NT_STATUS(ntdll.NtSetSecurityObject(
                         h.value->get(), OWNER_SECURITY_INFORMATION, &owner_sd),
                     STATUS_ACCESS_DENIED);
  }

  // 3. Handle with READ_CONTROL | WRITE_DAC (no WRITE_OWNER) succeeds on DACL
  // updates and fails OWNER updates with STATUS_ACCESS_DENIED.
  {
    auto h = core::NtOpenFilePath(
        nt_path, READ_CONTROL | WRITE_DAC | SYNCHRONIZE, core::kShareAll, 0);
    ASSERT_NT_SUCCESS(h.status);
    EXPECT_NT_SUCCESS(ntdll.NtSetSecurityObject(
        h.value->get(), DACL_SECURITY_INFORMATION, dacl_sd.value->get()));
    EXPECT_NT_STATUS(ntdll.NtSetSecurityObject(
                         h.value->get(), OWNER_SECURITY_INFORMATION, &owner_sd),
                     STATUS_ACCESS_DENIED);
  }

  dacl_guard.DisarmAndClose();
}

// Spec: [MS-FSA] 2.1.5.17 / Win32 File Security — directory generic rights
// (GENERIC_READ/WRITE/EXECUTE) map to NTFS specific directory rights.
TEST_F(SecurityAclTest, DirectoryGenericRightsMapToSpecificRights) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto user_sid_str = core::GetCurrentUserSidString();
  auto wd_sid = StringToSid(L"S-1-1-0");      // Everyone
  auto au_sid = StringToSid(L"S-1-5-11");     // Authenticated Users
  auto bu_sid = StringToSid(L"S-1-5-32-545"); // BUILTIN\Users
  ASSERT_TRUE(user_sid_str) << core::FormatWin32Error(user_sid_str.error);
  ASSERT_TRUE(wd_sid) << core::FormatWin32Error(wd_sid.error);
  ASSERT_TRUE(au_sid) << core::FormatWin32Error(au_sid.error);
  ASSERT_TRUE(bu_sid) << core::FormatWin32Error(bu_sid.error);

  // GR/GW/GX -> NTFS specific directory rights FILE_LIST_DIRECTORY,
  // FILE_ADD_FILE, FILE_TRAVERSE, etc.
  const std::wstring dir = TestPath(L"dir_generic_map");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(dir.c_str(), nullptr));
  auto dir_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"dir_generic_map"));
  ASSERT_TRUE(dir_guard.armed());

  const std::wstring generic_sddl =
      L"D:P(A;;GR;;;WD)(A;;GW;;;AU)(A;;GX;;;BU)(A;;FA;;;" +
      *user_sid_str.value + L")";
  auto set_res =
      core::SetFileSddl(dir, DACL_SECURITY_INFORMATION, generic_sddl);
  ASSERT_TRUE(set_res) << core::FormatWin32Error(set_res.error);

  {
    auto buf = QueryRawFileSecurity(dir, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);

    static_assert((FILE_GENERIC_READ & FILE_LIST_DIRECTORY) != 0);
    static_assert((FILE_GENERIC_WRITE & FILE_ADD_FILE) != 0);
    static_assert((FILE_GENERIC_WRITE & FILE_ADD_SUBDIRECTORY) != 0);
    static_assert((FILE_GENERIC_EXECUTE & FILE_TRAVERSE) != 0);

    EXPECT_FALSE(HasAnyUnmappedGenericRightsAce(buf.value->data()));
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   0, INHERITED_ACE, FILE_GENERIC_READ,
                                   wd_sid.value->get()),
              0);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   0, INHERITED_ACE, FILE_GENERIC_WRITE,
                                   au_sid.value->get()),
              0);
    EXPECT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_ALLOWED_ACE_TYPE,
                                   0, INHERITED_ACE, FILE_GENERIC_EXECUTE,
                                   bu_sid.value->get()),
              0);
  }
  dir_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.17 & NtSetSecurityObject — OWNER_SECURITY_INFORMATION
// with a security descriptor that carries no owner SID fails
// STATUS_INVALID_SECURITY_DESCR.
TEST_F(SecurityAclTest, SetSecurityRejectsOwnerInfoWithoutOwnerSid) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  core::UniqueHandle h;
  ASSERT_NO_FATAL_FAILURE(
      CreateSetSecurityTarget(TestPath(L"invalid_sd_no_owner.dat"),
                              TestNtPath(L"invalid_sd_no_owner.dat"), &h));

  SECURITY_DESCRIPTOR empty_sd{};
  ASSERT_WIN32_TRUE(
      ::InitializeSecurityDescriptor(&empty_sd, SECURITY_DESCRIPTOR_REVISION));
  EXPECT_NT_STATUS(core::Ntdll().NtSetSecurityObject(
                       h.get(), OWNER_SECURITY_INFORMATION, &empty_sd),
                   nt::kStatusInvalidSecurityDescr);
}

// Spec: [MS-FSA] 2.1.5.17 & NtSetSecurityObject — GROUP_SECURITY_INFORMATION
// with a security descriptor that carries no group SID fails
// STATUS_INVALID_SECURITY_DESCR.
TEST_F(SecurityAclTest, SetSecurityRejectsGroupInfoWithoutGroupSid) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  core::UniqueHandle h;
  ASSERT_NO_FATAL_FAILURE(
      CreateSetSecurityTarget(TestPath(L"invalid_sd_no_group.dat"),
                              TestNtPath(L"invalid_sd_no_group.dat"), &h));

  SECURITY_DESCRIPTOR empty_sd{};
  ASSERT_WIN32_TRUE(
      ::InitializeSecurityDescriptor(&empty_sd, SECURITY_DESCRIPTOR_REVISION));
  EXPECT_NT_STATUS(core::Ntdll().NtSetSecurityObject(
                       h.get(), GROUP_SECURITY_INFORMATION, &empty_sd),
                   nt::kStatusInvalidSecurityDescr);
}

// Spec: NtSetSecurityObject — a security descriptor whose Revision is not
// SECURITY_DESCRIPTOR_REVISION fails STATUS_UNKNOWN_REVISION.
TEST_F(SecurityAclTest, SetSecurityRejectsUnknownSdRevision) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring nt_path = TestNtPath(L"invalid_sd_bad_rev.dat");
  core::UniqueHandle h;
  ASSERT_NO_FATAL_FAILURE(CreateSetSecurityTarget(
      TestPath(L"invalid_sd_bad_rev.dat"), nt_path, &h));
  auto dacl_guard = core::ScopedDaclRestore::Arm(nt_path);
  ASSERT_TRUE(dacl_guard.armed());

  SECURITY_DESCRIPTOR bad_rev{};
  bad_rev.Revision = 99;
  EXPECT_NT_STATUS(core::Ntdll().NtSetSecurityObject(
                       h.get(), DACL_SECURITY_INFORMATION, &bad_rev),
                   STATUS_UNKNOWN_REVISION);

  dacl_guard.RestoreNow();
}

// Spec: NtSetSecurityObject — a self-relative security descriptor whose DACL
// header is truncated (AclSize < sizeof(ACL)) fails STATUS_INVALID_ACL.
TEST_F(SecurityAclTest, SetSecurityRejectsTruncatedDaclHeader) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring nt_path = TestNtPath(L"invalid_sd_bad_acl.dat");
  core::UniqueHandle h;
  ASSERT_NO_FATAL_FAILURE(CreateSetSecurityTarget(
      TestPath(L"invalid_sd_bad_acl.dat"), nt_path, &h));
  auto dacl_guard = core::ScopedDaclRestore::Arm(nt_path);
  ASSERT_TRUE(dacl_guard.armed());

  std::vector<uint8_t> corrupt_sd(
      sizeof(SECURITY_DESCRIPTOR_RELATIVE) + sizeof(ACL), 0);
  auto *rel_sd =
      reinterpret_cast<SECURITY_DESCRIPTOR_RELATIVE *>(corrupt_sd.data());
  rel_sd->Revision = SECURITY_DESCRIPTOR_REVISION;
  rel_sd->Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
  rel_sd->Dacl = static_cast<DWORD>(sizeof(SECURITY_DESCRIPTOR_RELATIVE));
  auto *bad_acl = reinterpret_cast<ACL *>(corrupt_sd.data() +
                                          sizeof(SECURITY_DESCRIPTOR_RELATIVE));
  bad_acl->AclRevision = ACL_REVISION;
  bad_acl->AclSize = 4; // Smaller than sizeof(ACL) (8 bytes).

  EXPECT_NT_STATUS(core::Ntdll().NtSetSecurityObject(
                       h.get(), DACL_SECURITY_INFORMATION, corrupt_sd.data()),
                   STATUS_INVALID_ACL);

  dacl_guard.RestoreNow();
}

// Spec: [MS-FSCC] 2.4.1 FileAccessInformation — generic rights requested at
// open are mapped to file-specific rights and reported verbatim as the
// handle's granted access.
TEST_F(SecurityAclTest, FileAccessInformationReportsMappedGenericRead) {
  const std::wstring path = TestPath(L"access_info_generic.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(path, "access-info"));

  auto h = core::NtOpenFilePath(TestNtPath(L"access_info_generic.dat"),
                                GENERIC_READ, core::kShareAll, 0);
  ASSERT_NT_SUCCESS(h.status);
  ASSERT_TRUE(h.value.has_value());

  auto access = QueryFileAccessMask(h.value->get());
  ASSERT_NT_SUCCESS(access.status);
  ASSERT_TRUE(access.value.has_value());
  EXPECT_EQ(*access.value, static_cast<ACCESS_MASK>(FILE_GENERIC_READ));
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 Access Mask Format — MAXIMUM_ALLOWED
// resolves to the rights the DACL grants plus the owner's implicit
// READ_CONTROL | WRITE_DAC, never to rights the DACL withholds.
TEST_F(SecurityAclTest, MaximumAllowedGrantsOnlyDaclPermittedRights) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  // DELETE (derivable from the parent's FILE_DELETE_CHILD) and WRITE_OWNER
  // (derivable from an enabled SeTakeOwnershipPrivilege) depend on state
  // outside this file's DACL, so they are excluded from the comparison.
  constexpr ACCESS_MASK kDaclControlledMask =
      FILE_ALL_ACCESS & ~static_cast<ACCESS_MASK>(DELETE | WRITE_OWNER);
  constexpr ACCESS_MASK kExpectedGranted = FILE_GENERIC_READ | WRITE_DAC;
  constexpr ACCESS_MASK kUnresolvedBits = MAXIMUM_ALLOWED | GENERIC_READ |
                                          GENERIC_WRITE | GENERIC_EXECUTE |
                                          GENERIC_ALL;
  static_assert(kDaclControlledMask == 0x001601FFUL);
  static_assert(kExpectedGranted == 0x00160089UL);

  const std::wstring path = TestPath(L"max_allowed_dacl.dat");
  const std::wstring nt_path = TestNtPath(L"max_allowed_dacl.dat");
  {
    // Everyone is granted FILE_GENERIC_READ only.
    auto created = CreateFileWithSddl(path, L"D:P(A;;FR;;;WD)");
    ASSERT_TRUE(created) << core::FormatWin32Error(created.error);
  }
  auto dacl_guard = core::ScopedDaclRestore::Arm(nt_path);
  ASSERT_TRUE(dacl_guard.armed());

  {
    auto h = core::NtOpenFilePath(nt_path, MAXIMUM_ALLOWED, core::kShareAll, 0);
    ASSERT_NT_SUCCESS(h.status);
    ASSERT_TRUE(h.value.has_value());

    auto access = QueryFileAccessMask(h.value->get());
    ASSERT_NT_SUCCESS(access.status);
    ASSERT_TRUE(access.value.has_value());
    EXPECT_EQ(*access.value & kDaclControlledMask, kExpectedGranted);
    EXPECT_EQ(*access.value & kUnresolvedBits, ACCESS_MASK{0});
  }

  dacl_guard.RestoreNow();
}

// Spec: Win32 Mandatory Integrity Control & SetFileSecurityW — a mandatory
// label ACE set with LABEL_SECURITY_INFORMATION persists in the file's SACL and
// is readable with READ_CONTROL (no SeSecurityPrivilege).
TEST_F(SecurityAclTest, MandatoryLabelRoundTripsViaLabelSecurityInformation) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"integrity_label.dat");
  {
    auto created = CreateFileWithSddl(path, L"D:P(A;;FA;;;WD)");
    ASSERT_TRUE(created) << core::FormatWin32Error(created.error);
  }

  // Low mandatory level (S-1-16-4096) with NO_WRITE_UP; lowering a label to or
  // below the caller's own integrity level needs no SeRelabelPrivilege.
  auto set_label =
      core::SetFileSddl(path, LABEL_SECURITY_INFORMATION, L"S:(ML;;NW;;;LW)");
  ASSERT_TRUE(set_label) << core::FormatWin32Error(set_label.error);

  auto label = core::GetFileSddl(path, LABEL_SECURITY_INFORMATION);
  ASSERT_TRUE(label) << core::FormatWin32Error(label.error);
  EXPECT_NE(label.value->find(L"(ML;;NW;;;LW)"), std::wstring::npos)
      << "Expected Low mandatory label ACE, got: "
      << core::WideToUtf8(*label.value);

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: Win32 Mandatory Integrity Control & AccessCheck — a file without an
// explicit label is treated as Medium with NO_WRITE_UP, so a Low-integrity
// caller is denied write access even when the DACL grants Everyone full
// control, while read-up stays allowed.
TEST_F(SecurityAclTest, LowIntegrityCallerDeniedWriteUpToUnlabeledFile) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring path = TestPath(L"integrity_no_write_up.dat");
  {
    auto created = CreateFileWithSddl(path, L"D:P(A;;FA;;;WD)");
    ASSERT_TRUE(created) << core::FormatWin32Error(created.error);
  }

  auto low_token = CreateLowIntegrityImpersonationToken();
  ASSERT_TRUE(low_token) << core::FormatWin32Error(low_token.error);
  {
    core::ScopedImpersonation imp(low_token.value->get());
    ASSERT_TRUE(imp.active()) << core::FormatWin32Error(imp.error());

    // Control: the Low-integrity caller can still open the file for read.
    auto read_ok = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    EXPECT_TRUE(read_ok) << core::FormatWin32Error(read_ok.error);

    ExpectAccessDeniedOpen(path, FILE_WRITE_DATA);
  }

  EXPECT_WIN32_TRUE(::DeleteFileW(path.c_str()));
}

// Spec: Win32 ACE Inheritance — an inheritable CREATOR GROUP (CG, S-1-3-1)
// ACE expands on child files to the creator's primary group SID.
TEST_F(SecurityAclTest, CreatorGroupSidExpandsOnInheritedAce) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto expected_group_sid = QueryCurrentTokenPrimaryGroupSid();
  auto cg_sid = StringToSid(L"S-1-3-1"); // CREATOR GROUP
  ASSERT_TRUE(expected_group_sid)
      << core::FormatWin32Error(expected_group_sid.error);
  ASSERT_TRUE(cg_sid) << core::FormatWin32Error(cg_sid.error);

  const std::wstring parent = TestPath(L"cg_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  auto parent_guard = core::ScopedDaclRestore::Arm(TestNtPath(L"cg_parent"));
  ASSERT_TRUE(parent_guard.armed());

  // Inheritable GENERIC_ALL for CREATOR GROUP (inherit-only on the parent)
  // plus inheritable full control for Everyone so the child stays accessible
  // regardless of how the primary group is enabled in the caller's token.
  auto set_parent = SetProtectedInheritableDacl(
      parent, L"D:P(A;OICIIO;GA;;;CG)(A;OICI;FA;;;WD)");
  ASSERT_TRUE(set_parent) << core::FormatWin32Error(set_parent.error);

  const std::wstring child_file = parent + L"\\cg_child.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(child_file, "creator-group"));

  // The child's group SID is the creating token's primary group.
  {
    auto group_buf =
        QueryRawFileSecurity(child_file, GROUP_SECURITY_INFORMATION);
    ASSERT_TRUE(group_buf) << core::FormatWin32Error(group_buf.error);
    PSID child_group_sid = nullptr;
    BOOL group_defaulted = FALSE;
    ASSERT_WIN32_TRUE(::GetSecurityDescriptorGroup(
        group_buf.value->data(), &child_group_sid, &group_defaulted));
    ASSERT_NE(child_group_sid, nullptr);
    EXPECT_TRUE(::EqualSid(child_group_sid, expected_group_sid.value->get()) !=
                FALSE);
  }

  // The inherited ACE must name the concrete primary group SID, be effective
  // (no inherit flags on a file), and map GENERIC_ALL to FILE_ALL_ACCESS.
  auto child_buf = QueryRawFileSecurity(child_file, DACL_SECURITY_INFORMATION);
  ASSERT_TRUE(child_buf) << core::FormatWin32Error(child_buf.error);
  EXPECT_FALSE(HasAceForSid(child_buf.value->data(), cg_sid.value->get()));
  EXPECT_GE(FindMatchingAceIndex(
                child_buf.value->data(), ACCESS_ALLOWED_ACE_TYPE, INHERITED_ACE,
                OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE,
                FILE_ALL_ACCESS, expected_group_sid.value->get()),
            0);

  parent_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — access is evaluated when a
// handle is opened; a later DACL change does not revoke rights already granted
// to an open handle.
TEST_F(SecurityAclTest, DaclChangeDoesNotRevokeOpenHandleAccess) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  auto wd_sid = StringToSid(L"S-1-1-0"); // Everyone
  ASSERT_TRUE(wd_sid) << core::FormatWin32Error(wd_sid.error);

  const std::wstring path = TestPath(L"open_handle_survives.dat");
  {
    auto created = CreateFileWithSddl(path, L"D:P(A;;FA;;;WD)");
    ASSERT_TRUE(created) << core::FormatWin32Error(created.error);
    auto seeded =
        core::WriteAllBytes(created.value->get(), "still-readable", 14);
    ASSERT_TRUE(seeded) << core::FormatWin32Error(seeded.error);
  }

  auto reader = core::CreateWin32File(path, GENERIC_READ, core::kShareAll,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  ASSERT_TRUE(reader) << core::FormatWin32Error(reader.error);

  auto dacl_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"open_handle_survives.dat"));
  ASSERT_TRUE(dacl_guard.armed());

  // Deny FILE_READ_DATA (0x1) to Everyone while the reader handle stays open.
  auto deny_res = core::SetFileSddl(path, DACL_SECURITY_INFORMATION,
                                    L"D:P(D;;0x00000001;;;WD)(A;;FA;;;WD)");
  ASSERT_TRUE(deny_res) << core::FormatWin32Error(deny_res.error);
  {
    // Precondition: the Deny ACE is stored on the file.
    auto buf = QueryRawFileSecurity(path, DACL_SECURITY_INFORMATION);
    ASSERT_TRUE(buf) << core::FormatWin32Error(buf.error);
    ASSERT_GE(FindMatchingAceIndex(buf.value->data(), ACCESS_DENIED_ACE_TYPE, 0,
                                   INHERITED_ACE, FILE_READ_DATA,
                                   wd_sid.value->get()),
              0);
  }

  auto data = core::ReadAllBytes(reader.value->get());
  ASSERT_TRUE(data) << core::FormatWin32Error(data.error);
  EXPECT_EQ(*data.value, "still-readable");

  reader.value->reset();
  dacl_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — renaming needs DELETE on
// the source, which the parent's FILE_DELETE_CHILD grants even when the file's
// own DACL denies DELETE.
TEST_F(SecurityAclTest, RenameAllowedByParentDeleteChildWhenFileDeniesDelete) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring parent = TestPath(L"ren_delchild_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  auto parent_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"ren_delchild_parent"));
  ASSERT_TRUE(parent_guard.armed());

  // Parent grants FILE_DELETE_CHILD (part of FA).
  auto set_parent =
      core::SetFileSddl(parent, DACL_SECURITY_INFORMATION, L"D:P(A;;FA;;;WD)");
  ASSERT_TRUE(set_parent) << core::FormatWin32Error(set_parent.error);

  const std::wstring src = parent + L"\\src.dat";
  const std::wstring dst = parent + L"\\renamed.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(src, "rename-me"));
  auto file_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"ren_delchild_parent\\src.dat"));
  ASSERT_TRUE(file_guard.armed());

  // File denies DELETE (0x00010000) and allows every other right.
  auto set_file =
      core::SetFileSddl(src, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00010000;;;WD)(A;;0x001E01FF;;;WD)");
  ASSERT_TRUE(set_file) << core::FormatWin32Error(set_file.error);

  {
    auto h = core::CreateWin32File(src, DELETE | SYNCHRONIZE, core::kShareAll,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(h) << core::FormatWin32Error(h.error);
    // Simple (same-directory) rename: bare target name, no root directory.
    auto ren = core::SetRenameInfo(h.value->get(), L"renamed.dat", nullptr,
                                   /*replace_if_exists=*/false);
    EXPECT_NT_SUCCESS(ren.status);
  }
  EXPECT_TRUE(helpers::CheckFileNotFound(src));
  EXPECT_TRUE(helpers::CheckFileContents(dst, "rename-me"));

  file_guard.RestoreNow();
  parent_guard.RestoreNow();
}

// Spec: [MS-FSA] 2.1.5.1.2 & Win32 File Security — renaming fails with
// ERROR_ACCESS_DENIED when neither the file (DELETE) nor its parent
// (FILE_DELETE_CHILD) grants delete access.
TEST_F(SecurityAclTest, RenameDeniedWithoutDeleteOrParentDeleteChild) {
  REQUIRE_FS_CAPABILITY(FsCapability::kPersistentAcls);

  const std::wstring parent = TestPath(L"ren_no_delchild_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent.c_str(), nullptr));
  auto parent_guard =
      core::ScopedDaclRestore::Arm(TestNtPath(L"ren_no_delchild_parent"));
  ASSERT_TRUE(parent_guard.armed());

  const std::wstring src = parent + L"\\src.dat";
  const std::wstring dst = parent + L"\\renamed.dat";
  ASSERT_WIN32_OK(core::WriteFileContents(src, "stay-put"));
  auto file_guard = core::ScopedDaclRestore::Arm(
      TestNtPath(L"ren_no_delchild_parent\\src.dat"));
  ASSERT_TRUE(file_guard.armed());

  // Parent denies FILE_DELETE_CHILD (0x00000040) but still allows FILE_ADD_FILE
  // so the failure is attributable to the missing delete access alone.
  auto set_parent =
      core::SetFileSddl(parent, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00000040;;;WD)(A;;0x001F01BF;;;WD)");
  ASSERT_TRUE(set_parent) << core::FormatWin32Error(set_parent.error);
  // File denies DELETE (0x00010000) and allows every other right.
  auto set_file =
      core::SetFileSddl(src, DACL_SECURITY_INFORMATION,
                        L"D:P(D;;0x00010000;;;WD)(A;;0x001E01FF;;;WD)");
  ASSERT_TRUE(set_file) << core::FormatWin32Error(set_file.error);

  EXPECT_WIN32_ERROR(::MoveFileExW(src.c_str(), dst.c_str(), 0),
                     ERROR_ACCESS_DENIED);
  EXPECT_TRUE(helpers::CheckFileContents(src, "stay-put"));
  EXPECT_TRUE(helpers::CheckFileNotFound(dst));

  file_guard.RestoreNow();
  parent_guard.RestoreNow();
}

void ForceLink_SecurityAcl() {}

} // namespace fstest
