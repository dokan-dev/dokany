// SPDX-License-Identifier: MIT
// Exercise the public API using only opens and tokens belonging to this process.
#include "../../dokan/dokan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

bool HasGrantedAccess(HANDLE token, ACCESS_MASK expected);

static std::vector<BYTE> ownUser;
static LONG callbacks;

static bool TokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS info,
                      std::vector<BYTE> &buffer) {
  DWORD size = 0;
  GetTokenInformation(token, info, NULL, 0, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size)
    return false;
  buffer.resize(size);
  return GetTokenInformation(token, info, buffer.data(), size, &size) != FALSE;
}

static bool VerifyToken(HANDLE token, SECURITY_IMPERSONATION_LEVEL expected) {
  SECURITY_IMPERSONATION_LEVEL level;
  TOKEN_TYPE type;
  DWORD size;
  ACCESS_MASK access = TOKEN_QUERY;
  if (expected == SecurityImpersonation)
    access |= TOKEN_IMPERSONATE;

  std::vector<BYTE> user, groups;
  if (!HasGrantedAccess(token, access) ||
      !GetTokenInformation(token, TokenType, &type, sizeof(type), &size) ||
      type != TokenImpersonation ||
      !GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level), &size) ||
      level != expected || !TokenInfo(token, TokenUser, user) ||
      !EqualSid(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid,
                reinterpret_cast<TOKEN_USER *>(ownUser.data())->User.Sid) ||
      !TokenInfo(token, TokenGroups, groups)) {
    return false;
  }

  BOOL impersonated = ImpersonateLoggedOnUser(token);
  if (impersonated && !RevertToSelf())
    abort();
  return (impersonated != FALSE) == (expected == SecurityImpersonation);
}

static bool DisableImpersonationPrivilege(HANDLE token) {
  TOKEN_PRIVILEGES disable = {};
  disable.PrivilegeCount = 1;
  return LookupPrivilegeValueW(NULL, SE_IMPERSONATE_NAME, &disable.Privileges[0].Luid) &&
         AdjustTokenPrivileges(token, FALSE, &disable, 0, NULL, NULL) &&
         GetLastError() == ERROR_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
CheckCreate(LPCWSTR name, PDOKAN_IO_SECURITY_CONTEXT, ACCESS_MASK, ULONG, ULONG,
            ULONG, ULONG, PDOKAN_FILE_INFO info) {
  if (wcscmp(name, L"\\") == 0) {
    info->IsDirectory = TRUE;
    return STATUS_SUCCESS;
  }
  if (info->ProcessId != GetCurrentProcessId())
    return STATUS_OBJECT_NAME_NOT_FOUND;

  bool anonymous = wcscmp(name, L"\\anonymous") == 0;
  bool impersonation = wcscmp(name, L"\\impersonation") == 0;
  bool restrictedWorker = wcscmp(name, L"\\restricted-worker") == 0;
  if (!anonymous && !impersonation && !restrictedWorker && wcscmp(name, L"\\identity") != 0)
    return STATUS_OBJECT_NAME_NOT_FOUND;

  if (restrictedWorker) {
    if (!ImpersonateSelf(SecurityImpersonation))
      return STATUS_UNSUCCESSFUL;
    HANDLE threadToken;
    bool disabled = false;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                        TRUE, &threadToken)) {
      disabled = DisableImpersonationPrivilege(threadToken);
      CloseHandle(threadToken);
    }
    if (!disabled) {
      if (!RevertToSelf())
        abort();
      return STATUS_UNSUCCESSFUL;
    }
  }
  HANDLE token = DokanOpenRequestorToken(info);
  DWORD error = GetLastError();
  if (restrictedWorker && !RevertToSelf())
    abort();
  bool ok;
  if (token == INVALID_HANDLE_VALUE) {
    ok = anonymous && error == ERROR_BAD_IMPERSONATION_LEVEL;
  } else {
    ok = !anonymous && VerifyToken(token, impersonation ? SecurityImpersonation
                                                       : SecurityIdentification);
    CloseHandle(token);
  }
  if (!ok) {
    fwprintf(stderr, L"Unexpected requestor token for %ls\n", name);
    return STATUS_ACCESS_DENIED;
  }
  InterlockedIncrement(&callbacks);
  return STATUS_SUCCESS;
}

static bool OpenCase(LPCWSTR root, LPCWSTR name, DWORD flags) {
  std::wstring path = std::wstring(root) + name;
  LONG before = InterlockedCompareExchange(&callbacks, 0, 0);
  HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, flags, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    fwprintf(stderr, L"Open %ls (flags %#lx) failed with %lu\n",
             path.c_str(), flags, GetLastError());
    return false;
  }
  CloseHandle(file);
  return InterlockedCompareExchange(&callbacks, 0, 0) == before + 1;
}

static bool CheckHost(HANDLE processToken, LPCWSTR root) {
  LUID luid;
  std::vector<BYTE> buffer;
  if (!LookupPrivilegeValueW(NULL, SE_IMPERSONATE_NAME, &luid) ||
      !TokenInfo(processToken, TokenPrivileges, buffer))
    return false;
  const TOKEN_PRIVILEGES *privileges = reinterpret_cast<const TOKEN_PRIVILEGES *>(buffer.data());
  bool enabled = false;
  for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
    const LUID_AND_ATTRIBUTES &entry = privileges->Privileges[i];
    if (entry.Luid.LowPart == luid.LowPart && entry.Luid.HighPart == luid.HighPart) {
      enabled = (entry.Attributes & SE_PRIVILEGE_ENABLED) != 0;
      break;
    }
  }
  LPCWSTR normal = enabled ? L"impersonation" : L"identity";
  bool ok = OpenCase(root, normal, 0);
  ok = OpenCase(root, normal, SECURITY_SQOS_PRESENT | SECURITY_DELEGATION) && ok;
  ok = OpenCase(root, L"identity", SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION) && ok;
  ok = OpenCase(root, L"anonymous", SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS) && ok;

  if (enabled) {
    ok = OpenCase(root, L"restricted-worker", SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION) && ok;
    // Exercise an unprivileged process on the same mount and identity.
    if (!DisableImpersonationPrivilege(processToken))
      return false;
    ok = OpenCase(root, L"identity", SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION) && ok;
  } else {
    puts("The local impersonation case requires enabled SeImpersonatePrivilege.");
  }
  return ok;
}

// The mirror harness has already verified a normal open on this mount.
static bool CheckMirrorDenial(LPCWSTR mount, LPCWSTR backing, DWORD level) {
  std::wstring name = L"\\token-denial-" + std::to_wstring(GetCurrentProcessId()) +
                      L"-" + std::to_wstring(level);
  std::wstring path = std::wstring(mount) + name;
  std::wstring backingPath = std::wstring(backing) + name;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                            SECURITY_SQOS_PRESENT | level, NULL);
  if (file != INVALID_HANDLE_VALUE) {
    CloseHandle(file);
    fwprintf(stderr, L"Mirror unexpectedly created %ls\n", path.c_str());
    return false;
  }
  DWORD error = GetLastError();
  return (error == ERROR_ACCESS_DENIED || error == ERROR_BAD_IMPERSONATION_LEVEL ||
          error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_CANT_OPEN_ANONYMOUS) &&
         GetFileAttributesW(backingPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

int wmain(int argc, wchar_t **argv) {
  if (argc == 4 && wcscmp(argv[1], L"--mirror") == 0) {
    bool ok = CheckMirrorDenial(argv[2], argv[3], SECURITY_IDENTIFICATION);
    ok = CheckMirrorDenial(argv[2], argv[3], SECURITY_ANONYMOUS) && ok;
    return DokanRemoveMountPoint(argv[2]) && ok ? 0 : 1;
  }
  if (argc != 1)
    return 1;
  WCHAR root[] = L"Z:\\";
  DWORD drives = GetLogicalDrives();
  while (root[0] > L'C' && (drives & (1u << (root[0] - L'A'))))
    --root[0];
  if (!drives || root[0] <= L'C')
    return 1;

  HANDLE processToken;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                        &processToken))
    return 1;
  DOKAN_OPTIONS options = {};
  options.Version = DOKAN_VERSION;
  options.Options = DOKAN_OPTION_CURRENT_SESSION;
  options.MountPoint = root;
  DOKAN_OPERATIONS ops = {};
  ops.ZwCreateFile = CheckCreate;
  DOKAN_HANDLE mount = NULL;
  DokanInit();
  bool ok = TokenInfo(processToken, TokenUser, ownUser) &&
            DokanCreateFileSystem(&options, &ops, &mount) == DOKAN_SUCCESS;
  if (ok) {
    ok = CheckHost(processToken, root);
    DokanCloseHandle(mount);
  }
  DokanShutdown();
  CloseHandle(processToken);
  return ok ? 0 : 1;
}
