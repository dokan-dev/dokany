#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <errno.h>
#include <sys/stat.h>
#include "utils.h"

int utf8_to_wchar_buf(const char* src, wchar_t* res, int maxlen) {
  if (res == nullptr || maxlen <= 0)
    return -1;

  int ln = MultiByteToWideChar(CP_UTF8, 0, src, -1, res, maxlen);
  if (ln <= 0) {
    *res = L'\0';
    return -1;
  }

  // This api replaces illegal sequences with U+FFFD,
  // so we mark the conversion as failed.
  for (size_t i = 0; i < ln; i++) {
    if (res[i] == 0xfffd) {
      *res = L'\0';
      return -1;
    }
  }
  return ln;
}

void utf8_to_wchar_buf_old(const char *src, wchar_t *res, int maxlen) {
  if (res == nullptr || maxlen == 0)
    return;

  int ln =
      MultiByteToWideChar(CP_ACP, 0, src, -1, nullptr, 0) /* | raise_w32_error()*/;
  if (ln >= maxlen) {
    *res = L'\0';
    return;
  }
  MultiByteToWideChar(CP_ACP, 0, src, -1, res,
                      static_cast<int>(strlen(src) + 1)) /* | raise_w32_error()*/;
}

static char *wchar_to_utf8(const wchar_t *src) {
  if (src == nullptr)
    return nullptr;

  int ln = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
  auto res = static_cast<char *>(malloc(sizeof(char) * ln));
  WideCharToMultiByte(CP_UTF8, 0, src, -1, res, ln, nullptr, nullptr);
  return res;
}

std::string wchar_to_utf8_cstr(const wchar_t *str) {
  std::string res;
  auto cstr = wchar_to_utf8(str);
  if (cstr != nullptr) {
    res = std::string(cstr);
    free(cstr);
  }
  return res;
}

std::string unixify(const std::string &str) {
  // Replace slashes
  std::string res = str;
  for (size_t f = 0; f < res.size(); ++f) {
    auto ch = res[f];
    if (ch == '\\')
      res[f] = '/';
  }
  // Remove the trailing slash
  if (res.size() > 1 && res[res.size() - 1] == '/')
    res.resize(res.size() - 1);

  return res;
}

FILETIME unixTimeToFiletime(time_t t) {
  // Note that LONGLONG is a 64-bit value
  LONGLONG ll;

  ll = Int32x32To64(t, 10000000) + 116444736000000000LL;
  FILETIME res;
  res.dwLowDateTime = static_cast<DWORD>(ll);
  res.dwHighDateTime = static_cast<DWORD>(ll >> 32);
  return res;
}

bool is_filetime_set(const FILETIME *ft) {
  if (ft == nullptr || (ft->dwHighDateTime == 0 && ft->dwLowDateTime == 0))
    return false;
  return true;
}

time_t filetimeToUnixTime(const FILETIME *ft) {
  if (!is_filetime_set(ft))
    return 0;

  ULONGLONG ll = (ULONGLONG(ft->dwHighDateTime) << 32) + ft->dwLowDateTime;
  return time_t((ll - 116444736000000000LL) / 10000000LL);
}

///////////////////////////////////////////////////////////////////////////////////////
////// Error mapping and arguments conversion
///////////////////////////////////////////////////////////////////////////////////////
struct errentry {
  NTSTATUS oscode; /* OS return value */
  int errnocode;   /* System V error code */
};

static const struct errentry errtable[] = {
    {STATUS_NOT_IMPLEMENTED, EINVAL},       /* 1 */
    {STATUS_OBJECT_NAME_NOT_FOUND, ENOENT}, /* 2 */
    {STATUS_OBJECT_PATH_NOT_FOUND, ENOENT}, /* 3 */
    {STATUS_TOO_MANY_OPENED_FILES, EMFILE}, /* 4 */
    {STATUS_ACCESS_DENIED, EACCES},         /* 5 */
    {STATUS_INVALID_HANDLE, EBADF},         /* 6 */
    {STATUS_DISK_CORRUPT_ERROR, ENOMEM},    /* 7 */
    {STATUS_NO_MEMORY, ENOMEM},             /* 8 */
    {STATUS_INVALID_ADDRESS, ENOMEM},       /* 9 */
    {STATUS_VARIABLE_NOT_FOUND, E2BIG},     /* 10 */
    //{  ERROR_BAD_FORMAT,             ENOEXEC   },  /* 11 */ Need to convert it
    //to NTSTATUS
    //{  ERROR_INVALID_ACCESS,         EINVAL    },  /* 12 */
    //{  ERROR_INVALID_DATA,           EINVAL    },  /* 13 */
    //{  ERROR_INVALID_DRIVE,          ENOENT    },  /* 15 */
    //{  ERROR_CURRENT_DIRECTORY,      EACCES    },  /* 16 */
    {STATUS_NOT_SAME_DEVICE, EXDEV},        /* 17 */
    {STATUS_NO_MORE_FILES, ENOENT},         /* 18 */
    {STATUS_FILE_LOCK_CONFLICT, EACCES},    /* 33 */
    {STATUS_BAD_NETWORK_PATH, ENOENT},      /* 53 */
    {STATUS_NETWORK_ACCESS_DENIED, EACCES}, /* 65 */
    {STATUS_BAD_NETWORK_NAME, ENOENT},      /* 67 */
    {STATUS_OBJECT_NAME_COLLISION, EEXIST}, /* 183 */
    {STATUS_OBJECT_NAME_COLLISION, EEXIST}, /* 80 */
    {STATUS_CANNOT_MAKE, EACCES},           /* 82 */
    //{  ERROR_FAIL_I24,               EACCES    },  /* 83 */
    {STATUS_INVALID_PARAMETER, EINVAL}, /* 87 */
    //{  ERROR_NO_PROC_SLOTS,          EAGAIN    },  /* 89 */
    //{  ERROR_DRIVE_LOCKED,           EACCES    },  /* 108 */
    {STATUS_PIPE_BROKEN, EPIPE}, /* 109 */
    {STATUS_DISK_FULL, ENOSPC},  /* 112 */
    //{  ERROR_INVALID_TARGET_HANDLE,  EBADF     },  /* 114 */
    {STATUS_INVALID_HANDLE, EINVAL}, /* 124 */
    {STATUS_NOT_FOUND, ECHILD},      /* 128 */
    //{  ERROR_CHILD_NOT_COMPLETE,     ECHILD    },  /* 129 */
    //{  ERROR_DIRECT_ACCESS_HANDLE,   EBADF     },  /* 130 */
    //{  ERROR_NEGATIVE_SEEK,          EINVAL    },  /* 131 */
    //{  ERROR_SEEK_ON_DEVICE,         EACCES    },  /* 132 */
    {STATUS_DIRECTORY_NOT_EMPTY, ENOTEMPTY}, /* 145 */
    {STATUS_NOT_LOCKED, EACCES},             /* 158 */
    {STATUS_OBJECT_PATH_SYNTAX_BAD, ENOENT}, /* 161 */
    //{  ERROR_MAX_THRDS_REACHED,      EAGAIN    },  /* 164 */
    {STATUS_LOCK_NOT_GRANTED, EACCES}, /* 167 */
    {STATUS_NAME_TOO_LONG, ENOENT},    /* 206 */
    //{  ERROR_NESTING_NOT_ALLOWED,    EAGAIN    },  /* 215 */
    {STATUS_QUOTA_EXCEEDED, ENOMEM} /* 1816 */
};
const int errtable_size = sizeof(errtable) / sizeof(errtable[0]);

extern "C" int ntstatus_error_to_errno(long win_res) {
  if (win_res == 0)
    return 0; // No error

  if (win_res < 0)
    win_res = -win_res;
  for (auto f = 0; f < errtable_size; ++f)
    if (errtable[f].oscode == win_res)
      return errtable[f].errnocode;
  return EINVAL;
}

extern "C" long errno_to_ntstatus_error(int err) {
  if (err == 0)
    return 0; // No error

  if (err < 0)
    err = -err;
  for (auto f = 0; f < errtable_size; ++f)
    if (errtable[f].errnocode == err)
      return errtable[f].oscode;
  return STATUS_NOT_IMPLEMENTED;
}

extern "C" char **convert_args(int argc, wchar_t *argv[]) {
  auto arr = static_cast<char **>(malloc(sizeof(char *) * (argc + 1)));
  if (arr == nullptr)
    return nullptr;
  for (auto f = 0; f < argc; ++f)
    arr[f] = wchar_to_utf8(argv[f]);
  arr[argc] = nullptr;
  return arr;
}

extern "C" void free_converted_args(int argc, char **argv) {
  for (int f = 0; f < argc; ++f)
    free(argv[f]);
  free(argv);
}

std::string extract_file_name(const std::string &str) {
  std::string extracted;

  for (auto f = str.rbegin(), en = str.rend();
       f != en; ++f)
    if (*f == '/') {
      extracted = str.substr(en - f - 1);
      break;
    }

  if (extracted.empty())
    return str;
  if (extracted[0] == '/')
    return extracted.substr(1);
  return extracted;
}

std::string extract_dir_name(const std::string &str) {
  for (auto f = str.rbegin(), en = str.rend();
       f != en; ++f)
    if (*f == '/')
      return str.substr(0, en - f);
  return str;
}
