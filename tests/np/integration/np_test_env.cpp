// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

#include "np_test_env.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace dokan_np_integration {

namespace {

std::mutex g_drive_mutex;
std::set<WCHAR> g_reserved_drives;
std::atomic<unsigned long> g_mount_seq{0};

bool FileExists(const std::wstring &path) {
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring GetTempBaseDir() {
  WCHAR buf[MAX_PATH]{};
  const DWORD len = ::GetTempPathW(MAX_PATH, buf);
  if (len == 0 || len >= MAX_PATH) {
    return L"C:\\Windows\\Temp";
  }
  std::wstring res(buf, len);
  while (!res.empty() && res.back() == L'\\') {
    res.pop_back();
  }
  return res;
}

void DeleteDirectoryRecursive(const std::wstring &dir) {
  if (dir.empty()) {
    return;
  }
  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
        continue;
      }
      const std::wstring child = dir + L"\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        DeleteDirectoryRecursive(child);
      } else {
        ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::DeleteFileW(child.c_str());
      }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
  }
  ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
  ::RemoveDirectoryW(dir.c_str());
}

std::wstring ReadEnvVar(const wchar_t *name) {
  const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) {
    return L"";
  }
  std::wstring val(needed, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name, val.data(), needed);
  if (written == 0 || written >= needed) {
    return L"";
  }
  val.resize(written);
  return val;
}

std::wstring GetExeDir() {
  WCHAR buf[MAX_PATH]{};
  const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    return L"";
  }
  std::wstring path(buf, len);
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

} // namespace

TestConfig &GetTestConfig() {
  static TestConfig cfg;
  return cfg;
}

std::wstring Utf8ToWide(const std::string &s) {
  if (s.empty()) {
    return L"";
  }
  const int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0) {
    return L"";
  }
  std::wstring out(static_cast<size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
  return out;
}

std::string WideToUtf8(const std::wstring &w) {
  if (w.empty()) {
    return "";
  }
  const int len =
      ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return "";
  }
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
  return out;
}

std::wstring FormatWin32Error(DWORD err) {
  LPWSTR msg = nullptr;
  const DWORD len = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
  std::wstring text = L"error " + std::to_wstring(err);
  if (len > 0 && msg != nullptr) {
    std::wstring detail(msg, len);
    while (!detail.empty() &&
           (detail.back() == L'\r' || detail.back() == L'\n' ||
            detail.back() == L' ')) {
      detail.pop_back();
    }
    text += L" (" + detail + L")";
  }
  if (msg != nullptr) {
    ::LocalFree(msg);
  }
  return text;
}

bool ParseCliFlags(int *argc, char **argv, std::string *error_out) {
  TestConfig &cfg = GetTestConfig();
  std::vector<char *> kept;
  kept.push_back(argv[0]);

  auto parse_kv = [](const std::string &arg, const char *prefix,
                     std::string *val) -> bool {
    const size_t plen = std::strlen(prefix);
    if (arg.compare(0, plen, prefix) == 0) {
      *val = arg.substr(plen);
      return true;
    }
    return false;
  };

  for (int i = 1; i < *argc; ++i) {
    const std::string arg = argv[i];
    std::string val;
    if (parse_kv(arg, "--mirror_exe=", &val)) {
      cfg.mirror_exe = Utf8ToWide(val);
    } else if (parse_kv(arg, "--dokannp_dll=", &val)) {
      cfg.dokannp_dll = Utf8ToWide(val);
    } else if (parse_kv(arg, "--secondary_user=", &val)) {
      cfg.secondary_user = Utf8ToWide(val);
    } else if (parse_kv(arg, "--secondary_pass=", &val)) {
      cfg.secondary_pass = Utf8ToWide(val);
    } else if (arg == "--test_live_registry") {
      cfg.test_live_registry = true;
    } else if (arg == "--skip_registry_roundtrip") {
      cfg.skip_registry_roundtrip = true;
    } else if (arg == "--skip_shell_check") {
      cfg.skip_shell_check = true;
    } else if (arg == "--probe_mode") {
      cfg.probe_mode = true;
    } else if (parse_kv(arg, "--probe_drive=", &val)) {
      cfg.probe_drive = Utf8ToWide(val);
    } else if (parse_kv(arg, "--probe_unc=", &val)) {
      cfg.probe_unc = Utf8ToWide(val);
    } else if (parse_kv(arg, "--probe_expect_visible=", &val)) {
      cfg.probe_expect_visible = (val == "1" || val == "true");
    } else if (parse_kv(arg, "--probe_out=", &val)) {
      cfg.probe_out = Utf8ToWide(val);
    } else if (arg.compare(0, 8, "--gtest_") == 0 || arg == "--help" ||
               arg == "-h") {
      kept.push_back(argv[i]);
    } else {
      *error_out = "Unknown flag: " + arg;
      return false;
    }
  }

  for (size_t j = 0; j < kept.size(); ++j) {
    argv[j] = kept[j];
  }
  *argc = static_cast<int>(kept.size());
  argv[*argc] = nullptr;
  return true;
}

int RunProbeMode() {
  const TestConfig &cfg = GetTestConfig();
  const DWORD session_id = GetCurrentProcessSessionId();

  bool conn_visible = false;
  DWORD conn_err = ERROR_SUCCESS;
  if (!cfg.probe_drive.empty()) {
    WCHAR remote[512]{};
    DWORD len = 512;
    conn_err = ::WNetGetConnectionW(cfg.probe_drive.c_str(), remote, &len);
    if (conn_err == NO_ERROR) {
      conn_visible = true;
    }
  }

  bool enum_visible = false;
  HANDLE h_enum = nullptr;
  const DWORD open_err = ::WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_ANY,
                                         0, nullptr, &h_enum);
  if (open_err == NO_ERROR) {
    std::vector<uint8_t> buf(16384);
    for (;;) {
      DWORD count = 0xFFFFFFFF;
      DWORD size = static_cast<DWORD>(buf.size());
      const DWORD enum_err =
          ::WNetEnumResourceW(h_enum, &count, buf.data(), &size);
      if (enum_err == ERROR_MORE_DATA) {
        buf.resize(size);
        continue;
      }
      if (enum_err != NO_ERROR) {
        break;
      }
      const auto *nr = reinterpret_cast<const NETRESOURCEW *>(buf.data());
      for (DWORD i = 0; i < count; ++i) {
        if (!cfg.probe_unc.empty() && nr[i].lpRemoteName != nullptr &&
            _wcsicmp(nr[i].lpRemoteName, cfg.probe_unc.c_str()) == 0) {
          enum_visible = true;
        }
        if (!cfg.probe_drive.empty() && nr[i].lpLocalName != nullptr &&
            _wcsicmp(nr[i].lpLocalName, cfg.probe_drive.c_str()) == 0) {
          enum_visible = true;
        }
      }
    }
    ::WNetCloseEnum(h_enum);
  }

  const bool visible = conn_visible || enum_visible;
  const bool matched = (visible == cfg.probe_expect_visible);

  std::ostringstream report;
  report << "session=" << session_id << " conn_visible=" << conn_visible
         << " conn_err=" << conn_err << " enum_visible=" << enum_visible
         << " open_err=" << open_err << " expected=" << cfg.probe_expect_visible
         << " matched=" << matched << "\n";
  const std::string text = report.str();

  if (!cfg.probe_out.empty()) {
    const std::wstring tmp = cfg.probe_out + L".tmp";
    HANDLE hf = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      ::WriteFile(hf, text.data(), static_cast<DWORD>(text.size()), &written,
                  nullptr);
      ::CloseHandle(hf);
      ::MoveFileExW(tmp.c_str(), cfg.probe_out.c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    }
  }
  return matched ? 0 : 1;
}

std::wstring ResolveMirrorExe() {
  const TestConfig &cfg = GetTestConfig();
  if (!cfg.mirror_exe.empty() && FileExists(cfg.mirror_exe)) {
    return cfg.mirror_exe;
  }

  std::vector<std::wstring> candidates;
  const std::wstring dokan_dir =
      ReadEnvVar(L"DokanLibrary" DOKAN_MAJOR_API_VERSION);
  if (!dokan_dir.empty()) {
#ifdef _WIN64
    candidates.push_back(dokan_dir + L"\\sample\\mirror\\mirror.exe");
#else
    candidates.push_back(dokan_dir + L"\\x86\\sample\\mirror\\mirror.exe");
#endif
  }

  const std::wstring exe_dir = GetExeDir();
  if (!exe_dir.empty()) {
    candidates.push_back(exe_dir + L"\\mirror.exe");
#ifdef _WIN64
    candidates.push_back(exe_dir +
                         L"\\..\\..\\..\\..\\x64\\Release\\mirror.exe");
    candidates.push_back(exe_dir + L"\\..\\..\\..\\..\\x64\\Debug\\mirror.exe");
#else
    candidates.push_back(exe_dir +
                         L"\\..\\..\\..\\..\\Win32\\Release\\mirror.exe");
    candidates.push_back(exe_dir +
                         L"\\..\\..\\..\\..\\Win32\\Debug\\mirror.exe");
#endif
  }

  for (const auto &c : candidates) {
    if (FileExists(c)) {
      return c;
    }
  }
  return L"";
}

bool IsProcessElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD ret = 0;
  const BOOL ok =
      ::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &ret);
  ::CloseHandle(token);
  return ok && elev.TokenIsElevated != 0;
}

bool IsDesktopShellAvailable() {
  WCHAR win_dir[MAX_PATH]{};
  const UINT n = ::GetWindowsDirectoryW(win_dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return false;
  }
  return FileExists(std::wstring(win_dir) + L"\\explorer.exe");
}

DWORD GetCurrentProcessSessionId() {
  DWORD session_id = 0;
  if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session_id)) {
    return static_cast<DWORD>(-1);
  }
  return session_id;
}

DWORD CallWNetFormatNetworkNameW(LPCWSTR lpProvider, LPCWSTR lpRemoteName,
                                 LPWSTR lpFormattedName, LPDWORD lpnLength,
                                 DWORD dwFlags, DWORD dwAveCharPerLine) {
  using WNetFormatNetworkNameWFn =
      DWORD(WINAPI *)(LPCWSTR, LPCWSTR, LPWSTR, LPDWORD, DWORD, DWORD);
  HMODULE mpr = ::GetModuleHandleW(L"mpr.dll");
  bool loaded = false;
  if (mpr == nullptr) {
    mpr = ::LoadLibraryW(L"mpr.dll");
    loaded = (mpr != nullptr);
  }
  if (mpr == nullptr) {
    const DWORD err = ::GetLastError();
    return err != 0 ? err : ERROR_MOD_NOT_FOUND;
  }
  auto fn = reinterpret_cast<WNetFormatNetworkNameWFn>(
      ::GetProcAddress(mpr, "WNetFormatNetworkNameW"));
  if (fn == nullptr) {
    const DWORD err = ::GetLastError();
    if (loaded) {
      ::FreeLibrary(mpr);
    }
    return err != 0 ? err : ERROR_PROC_NOT_FOUND;
  }
  const DWORD rc = fn(lpProvider, lpRemoteName, lpFormattedName, lpnLength,
                      dwFlags, dwAveCharPerLine);
  if (loaded) {
    ::FreeLibrary(mpr);
  }
  return rc;
}

bool RunProcessCapture(const std::wstring &cmd_line, DWORD timeout_ms,
                       DWORD *exit_code_out, std::wstring *output_out) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
    return false;
  }
  ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  std::wstring mutable_cmd = cmd_line;
  PROCESS_INFORMATION pi{};
  const BOOL ok =
      ::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  ::CloseHandle(write_pipe);
  if (!ok) {
    ::CloseHandle(read_pipe);
    return false;
  }

  std::string bytes;
  char chunk[4096];
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  bool timed_out = false;
  for (;;) {
    DWORD avail = 0;
    while (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr) &&
           avail > 0) {
      DWORD read_bytes = 0;
      const DWORD to_read =
          std::min<DWORD>(avail, static_cast<DWORD>(sizeof(chunk)));
      if (!::ReadFile(read_pipe, chunk, to_read, &read_bytes, nullptr) ||
          read_bytes == 0) {
        break;
      }
      bytes.append(chunk, read_bytes);
    }
    if (::WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) {
      while (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr) &&
             avail > 0) {
        DWORD read_bytes = 0;
        const DWORD to_read =
            std::min<DWORD>(avail, static_cast<DWORD>(sizeof(chunk)));
        if (!::ReadFile(read_pipe, chunk, to_read, &read_bytes, nullptr) ||
            read_bytes == 0) {
          break;
        }
        bytes.append(chunk, read_bytes);
      }
      break;
    }
    if (::GetTickCount64() >= deadline) {
      timed_out = true;
      ::TerminateProcess(pi.hProcess, 1);
      ::WaitForSingleObject(pi.hProcess, 2000);
      break;
    }
  }
  ::CloseHandle(read_pipe);

  DWORD code = 1;
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  if (exit_code_out != nullptr) {
    *exit_code_out = code;
  }
  if (output_out != nullptr) {
    int wlen = ::MultiByteToWideChar(
        CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wlen > 0) {
      output_out->resize(static_cast<size_t>(wlen));
      ::MultiByteToWideChar(CP_OEMCP, 0, bytes.data(),
                            static_cast<int>(bytes.size()), output_out->data(),
                            wlen);
    } else {
      output_out->clear();
    }
  }
  return !timed_out;
}

WCHAR AllocateFreeDriveLetter() {
  std::lock_guard<std::mutex> lock(g_drive_mutex);
  const DWORD mask = ::GetLogicalDrives();
  for (WCHAR ch = L'Z'; ch >= L'K'; --ch) {
    const DWORD bit = 1UL << (ch - L'A');
    if ((mask & bit) != 0 || g_reserved_drives.count(ch) != 0) {
      continue;
    }
    WCHAR dev_name[3] = {ch, L':', L'\0'};
    WCHAR target[MAX_PATH]{};
    if (::QueryDosDeviceW(dev_name, target, MAX_PATH) == 0 &&
        ::GetLastError() == ERROR_FILE_NOT_FOUND) {
      g_reserved_drives.insert(ch);
      return ch;
    }
  }
  return 0;
}

void ReleaseDriveLetter(WCHAR letter) {
  if (letter == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_drive_mutex);
  g_reserved_drives.erase(letter);
}

MirrorMount::~MirrorMount() { Stop(); }

bool MirrorMount::SendDokanRelease(const std::wstring &mount_point) {
  if (mount_point.empty()) {
    return false;
  }
  constexpr wchar_t kDokanGlobalDeviceName[] =
      L"\\\\.\\Dokan_" DOKAN_MAJOR_API_VERSION;
  HANDLE dev = ::CreateFileW(kDokanGlobalDeviceName, 0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
  if (dev == INVALID_HANDLE_VALUE) {
    return false;
  }
  const ULONG input_len =
      sizeof(DOKAN_UNICODE_STRING_INTERMEDIATE) + (MAX_PATH * sizeof(WCHAR));
  std::vector<uint8_t> buf(input_len, 0);
  auto *u = reinterpret_cast<PDOKAN_UNICODE_STRING_INTERMEDIATE>(buf.data());
  u->MaximumLength = MAX_PATH * sizeof(WCHAR);
  u->Length = static_cast<USHORT>(mount_point.size() * sizeof(WCHAR));
  std::memcpy(u->Buffer, mount_point.data(), u->Length);
  DWORD returned = 0;
  const BOOL ok = ::DeviceIoControl(dev, FSCTL_EVENT_RELEASE, buf.data(),
                                    input_len, nullptr, 0, &returned, nullptr);
  ::CloseHandle(dev);
  return ok != FALSE;
}

bool MirrorMount::Start(const std::wstring &server_name,
                        const std::wstring &share_name,
                        bool enable_network_unmount, bool current_session_only,
                        std::wstring *error_out) {
  Stop();

  const std::wstring mirror_exe = ResolveMirrorExe();
  if (mirror_exe.empty()) {
    *error_out = L"mirror.exe not found (pass --mirror_exe=...)";
    return false;
  }

  drive_letter_ = AllocateFreeDriveLetter();
  if (drive_letter_ == 0) {
    *error_out = L"No free drive letter available in K:..Z:";
    return false;
  }

  const unsigned long seq = ++g_mount_seq;
  backing_dir_ = GetTempBaseDir() + L"\\dokan_np_test_" +
                 std::to_wstring(::GetCurrentProcessId()) + L"_" +
                 std::to_wstring(seq);
  if (!::CreateDirectoryW(backing_dir_.c_str(), nullptr) &&
      ::GetLastError() != ERROR_ALREADY_EXISTS) {
    *error_out = L"Failed to create backing dir " + backing_dir_ + L": " +
                 FormatWin32Error(::GetLastError());
    ReleaseDriveLetter(drive_letter_);
    drive_letter_ = 0;
    return false;
  }

  // Create probe files inside the backing dir so mount readiness is
  // verifiable directly via Win32 file open.
  const std::wstring subdir = backing_dir_ + L"\\dir";
  ::CreateDirectoryW(subdir.c_str(), nullptr);
  for (const std::wstring &file :
       {backing_dir_ + L"\\probe.txt", subdir + L"\\file.txt"}) {
    HANDLE hf = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
      const char payload[] = "dokan-np-test";
      DWORD written = 0;
      ::WriteFile(hf, payload, sizeof(payload) - 1, &written, nullptr);
      ::CloseHandle(hf);
    }
  }

  unc_name_ = L"\\\\" + server_name + L"\\" + share_name;
  const std::wstring mirror_unc_arg = L"\\" + server_name + L"\\" + share_name;

  std::wostringstream cmd;
  cmd << L"\"" << mirror_exe << L"\" /r \"" << backing_dir_ << L"\" /l "
      << drive_letter_ << L": /n \"" << mirror_unc_arg << L"\"";
  if (enable_network_unmount) {
    cmd << L" /x";
  }
  if (current_session_only) {
    cmd << L" /c";
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring cmd_str = cmd.str();
  if (!::CreateProcessW(nullptr, cmd_str.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    *error_out = L"CreateProcessW failed for mirror.exe: " +
                 FormatWin32Error(::GetLastError());
    DeleteDirectoryRecursive(backing_dir_);
    backing_dir_.clear();
    ReleaseDriveLetter(drive_letter_);
    drive_letter_ = 0;
    return false;
  }
  ::CloseHandle(pi.hThread);
  process_ = pi.hProcess;

  const std::wstring probe_file = drive_root() + L"probe.txt";
  const std::wstring drv = drive_name();
  const ULONGLONG deadline = ::GetTickCount64() + 15000ULL;
  while (::GetTickCount64() < deadline) {
    if (::WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
      DWORD exit_code = 0;
      ::GetExitCodeProcess(process_, &exit_code);
      *error_out = L"mirror.exe exited prematurely with code " +
                   std::to_wstring(exit_code);
      Stop();
      return false;
    }
    if (FileExists(probe_file)) {
      WCHAR conn_buf[256]{};
      DWORD conn_len = 256;
      if (::WNetGetConnectionW(drv.c_str(), conn_buf, &conn_len) == NO_ERROR) {
        return true;
      }
    }
    ::Sleep(100);
  }

  *error_out = L"Timed out waiting for mount " + drv + L" (" + unc_name_ + L")";
  Stop();
  return false;
}

bool MirrorMount::WaitForExit(DWORD timeout_ms) {
  if (process_ == nullptr) {
    return true;
  }
  return ::WaitForSingleObject(process_, timeout_ms) == WAIT_OBJECT_0;
}

void MirrorMount::Stop() {
  if (process_ != nullptr) {
    if (::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
      if (drive_letter_ != 0) {
        SendDokanRelease(drive_name());
      }
      if (::WaitForSingleObject(process_, 5000) == WAIT_TIMEOUT) {
        ::TerminateProcess(process_, 1);
        ::WaitForSingleObject(process_, 5000);
      }
    }
    ::CloseHandle(process_);
    process_ = nullptr;
  }
  if (!backing_dir_.empty()) {
    DeleteDirectoryRecursive(backing_dir_);
    backing_dir_.clear();
  }
  if (drive_letter_ != 0) {
    ReleaseDriveLetter(drive_letter_);
    drive_letter_ = 0;
  }
  unc_name_.clear();
}

} // namespace dokan_np_integration
