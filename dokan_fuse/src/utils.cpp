#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <errno.h>
#include <sys/stat.h>
#include "utils.h"

typedef unsigned int ICONV_CHAR;

#if defined(__GNUC__) && __GNUC__ >= 3
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define unlikely(x) (x)
#endif

#define GET_A2(p) (*((unsigned short *)(p)))
#define PUT_A2(buf, c)                                                         \
  do {                                                                         \
    *((unsigned short *)(buf)) = (c);                                          \
  } while (0)

static const unsigned char utf8_lengths[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 0, 0,
};

static const unsigned char utf8_masks[7] = {0,    0x7f, 0x1f, 0x0f,
                                            0x07, 0x03, 0x01};

static size_t get_utf8(const unsigned char *p, size_t len, ICONV_CHAR *out) {
  ICONV_CHAR uc;
  size_t l;

  l = utf8_lengths[p[0]];
  if (unlikely(l == 0))
    return -EILSEQ;
  if (unlikely(len < l))
    return -EINVAL;

  len = l;
  uc = *p++ & utf8_masks[l];
  while (--l)
    uc = (uc << 6) | (*p++ & 0x3f);
  *out = uc;
  return len;
}

static size_t put_utf8(unsigned char *buf, ICONV_CHAR c) {
#define MASK(n) ((0xffffffffu << (n)) & 0xffffffffu)
  size_t o_len;
  unsigned mask;

  if ((c & MASK(7)) == 0) {
    *buf = (unsigned char)c;
    return 1;
  }

  o_len = 2;
  for (;;) {
    if ((c & MASK(11)) == 0)
      break;
    ++o_len;
    if ((c & MASK(16)) == 0)
      break;
    ++o_len;
    if ((c & MASK(21)) == 0)
      break;
    ++o_len;
    if ((c & MASK(26)) == 0)
      break;
    ++o_len;
    if ((c & MASK(31)) != 0)
      return -EINVAL;
  }

  buf += o_len;
  mask = 0xff80;
  for (;;) {
    *--buf = 0x80 | (c & 0x3f);
    c >>= 6;
    mask >>= 1;
    if (c < 0x40) {
      *--buf = mask | c;
      break;
    }
  }
  return o_len;
}

static size_t get_utf16(const unsigned char *p, size_t len, ICONV_CHAR *out) {
  ICONV_CHAR c;

  if (len < 2)
    return -EINVAL;
  c = GET_A2(p);
  if ((c & 0xfc00) == 0xd800 && len >= 4) {
    ICONV_CHAR c2 = GET_A2(p + 2);
    if ((c2 & 0xfc00) == 0xdc00) {
      *out = (c << 10) + c2 - ((0xd800 << 10) + 0xdc00 - 0x10000);
      return 4;
    }
  }
  *out = c;
  return 2;
}

static size_t put_utf16(unsigned char *buf, ICONV_CHAR c) {
  if (c >= 0x110000u)
    return -EILSEQ;
  if (c < 0x10000u) {
    PUT_A2(buf, c);
    return 2;
  }
  c -= 0x10000u;
  PUT_A2(buf, 0xd800 + (c >> 10));
  PUT_A2(buf + 2, 0xdc00 + (c & 0x3ffu));
  return 4;
}

typedef size_t (*get_conver_t)(const unsigned char *p, size_t len,
                               ICONV_CHAR *out);
typedef size_t (*put_convert_t)(unsigned char *buf, ICONV_CHAR c);

static size_t convert_char(get_conver_t get_func, put_convert_t put_func,
                           const void *src, size_t src_len, void *dest) {
  size_t il = src_len;
  const unsigned char *ib = (const unsigned char *)src;
  unsigned char *ob = (unsigned char *)dest;
  size_t total = 0;

  while (il) {
    ICONV_CHAR out_c = 0;
    size_t readed = get_func(ib, il, &out_c);
    if (unlikely(readed < 0))
      return -1;
    il -= readed;
    ib += readed;

    unsigned char dummy[8] = {0};
    size_t written = put_func(ob ? ob : dummy, out_c);
    if (unlikely(written < 0))
      return -1;

    if (ob)
      ob += written;
    total += written;
  }
  return total;
}

static char *wchar_to_utf8(const wchar_t *str) {
  if (str == NULL)
    return NULL;

  // Determine required length
  size_t ln = convert_char(get_utf16, put_utf8, str,
                           (wcslen(str) + 1) * sizeof(wchar_t), NULL);
  if (ln <= 0)
    return NULL;
  char *res = (char *)malloc(sizeof(char) * ln);
  if (res == NULL)
    return NULL;

  // Convert to Unicode
  convert_char(get_utf16, put_utf8, str, (wcslen(str) + 1) * sizeof(wchar_t),
               res);
  return res;
}

void utf8_to_wchar_buf(const char *src, wchar_t *res, int maxlen) {
  if (res == NULL || maxlen == 0)
    return;

  size_t ln = convert_char(get_utf8, put_utf16, src, strlen(src) + 1,
                           NULL); /* | raise_w32_error()*/
  ;
  if (ln <= 0 || ln / sizeof(wchar_t) > (size_t)maxlen) {
    *res = L'\0';
    return;
  }
  convert_char(get_utf8, put_utf16, src, strlen(src) + 1,
               res); /* | raise_w32_error()*/
  ;
}

void utf8_to_wchar_buf_old(const char *src, wchar_t *res, int maxlen) {
  if (res == NULL || maxlen == 0)
    return;

  int ln =
      MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0) /* | raise_w32_error()*/;
  if (ln >= maxlen) {
    *res = L'\0';
    return;
  }
  MultiByteToWideChar(CP_ACP, 0, src, -1, res,
                      (int)(strlen(src) + 1)) /* | raise_w32_error()*/;
}

std::string wchar_to_utf8_cstr(const wchar_t *str) {
  char *utf = wchar_to_utf8(str);
  std::string res(utf);
  free(utf);
  return res;
}

std::string unixify(const std::string &str) {
  // Replace slashes
  std::string res = str;
  for (size_t f = 0; f < res.size(); ++f) {
    char ch = res[f];
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
  res.dwLowDateTime = (DWORD)ll;
  res.dwHighDateTime = (DWORD)(ll >> 32);
  return res;
}

bool is_filetime_set(const FILETIME *ft) {
  if (ft == 0 || (ft->dwHighDateTime == 0 && ft->dwLowDateTime == 0))
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

extern "C" int ntstatus_error_to_errno(int win_res) {
  if (win_res == 0)
    return 0; // No error

  if (win_res < 0)
    win_res = -win_res;
  for (int f = 0; f < errtable_size; ++f)
    if (errtable[f].oscode == win_res)
      return errtable[f].errnocode;
  return EINVAL;
}

extern "C" int errno_to_ntstatus_error(int err) {
  if (err == 0)
    return 0; // No error

  if (err < 0)
    err = -err;
  for (int f = 0; f < errtable_size; ++f)
    if (errtable[f].errnocode == err)
      return errtable[f].oscode;
  return ERROR_INVALID_FUNCTION;
}

extern "C" char **convert_args(int argc, wchar_t *argv[]) {
  char **arr = (char **)malloc(sizeof(char *) * (argc + 1));
  if (arr == NULL)
    return NULL;
  for (int f = 0; f < argc; ++f)
    arr[f] = wchar_to_utf8(argv[f]);
  arr[argc] = NULL;
  return arr;
}

extern "C" void free_converted_args(int argc, char **argv) {
  for (int f = 0; f < argc; ++f)
    free(argv[f]);
  free(argv);
}

std::string extract_file_name(const std::string &str) {
  std::string extracted;

  for (std::string::const_reverse_iterator f = str.rbegin(), en = str.rend();
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
  for (std::string::const_reverse_iterator f = str.rbegin(), en = str.rend();
       f != en; ++f)
    if (*f == '/')
      return str.substr(0, en - f);
  return str;
}
