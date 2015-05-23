/* ----------------------------------------------------------- *
* Win32 helper functions                                       *
* Compilation on MSVC requires /Zc:wchar_t compiler option     *
* ----------------------------------------------------------- */
#ifndef _FUSE_WIN_H_
#define _FUSE_WIN_H_

#include <time.h>
#include <sys/types.h>

#ifdef _MSC_VER
#include <windows.h>
#endif

/** Only use the latest version on Windows */
#ifndef FUSE_USE_VERSION 
#define FUSE_USE_VERSION 27
#endif

#ifndef DEFAULT_FUSE_VOLUME_NAME
#define DEFAULT_FUSE_VOLUME_NAME "DOKAN"
#endif

#ifndef DEFAULT_FUSE_FILESYSTEM_NAME
#define DEFAULT_FUSE_FILESYSTEM_NAME "Dokan user-level file system"
#endif

#ifdef __cplusplus
extern "C" {
#endif
int win32_error_to_errno(int win_res);
int errno_to_win32_error(int err);

//This stuff is useful only on Windows in MSVC
#ifdef _MSC_VER
char** convert_args(int argc, wchar_t* argv[]);
void free_converted_args(int argc, char **argv);
#endif

#ifdef __cplusplus
};
#endif

extern wchar_t* Dokan_filesystem_name;
extern wchar_t* Dokan_volume_name;

/////////////////////////////////////////////////////////////////////
////// Type definitions for MINGW32
/////////////////////////////////////////////////////////////////////
#if defined(__MINGW32__) && !defined(UID_GID_DEF)
typedef unsigned int gid_t;
typedef unsigned int uid_t;
#endif

#if !defined(HAVE_STRUCT_TIMESPEC) && !defined(__CYGWIN__) && !defined(_TIMESPEC_DEFINED) /* win32 pthread.h defines it */
/* POSIX.1b structure for a time value.  This is like a `struct timeval' but
has nanoseconds instead of microseconds.  */
#define HAVE_STRUCT_TIMESPEC 1
#define _TIMESPEC_DEFINED
struct timespec
{
	time_t tv_sec;				/* Seconds.  */
	long int tv_nsec;           /* Nanoseconds.  */
};
#endif

#if defined(__MINGW32__) 
/** Use 64 bit offsets */
#define __USE_FILE_OFFSET64
//Block sizes
typedef unsigned __int64 fsfilcnt64_t;
typedef unsigned __int64 fsblkcnt64_t;

/** Transplanted from <sys/statvfs.h>*/
struct statvfs
{
	unsigned long int f_bsize;
	unsigned long int f_frsize;
	fsblkcnt64_t f_blocks;
	fsblkcnt64_t f_bfree;
	fsblkcnt64_t f_bavail;
	fsfilcnt64_t f_files;
	fsfilcnt64_t f_ffree;
	fsfilcnt64_t f_favail;
	unsigned long int f_fsid;
	unsigned long int f_flag;
	unsigned long int f_namemax;
};
struct flock {
	short l_type;
	short l_whence;
	off_t l_start;
	off_t l_len;
	pid_t l_pid;
};

#endif

/////////////////////////////////////////////////////////////////////
////// Type definitions for MSVC
/////////////////////////////////////////////////////////////////////
#if defined(_MSC_VER)
//UNIX compatibility
typedef unsigned int mode_t;
typedef unsigned int pid_t;
typedef unsigned int gid_t;
typedef unsigned int uid_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
typedef __int64 int64_t;

//OCTAL constants!
#define	S_IFLNK 0120000
#define	S_ISLNK(m)	(((m)&S_IFMT) == S_IFLNK)

/** Use 64 bit offsets */
#define __USE_FILE_OFFSET64
//Block sizes
typedef unsigned __int64 fsfilcnt64_t;
typedef unsigned __int64 fsblkcnt64_t;

/** Transplanted from <sys/statvfs.h>*/
struct statvfs
{
	unsigned long int f_bsize;
	unsigned long int f_frsize;
	fsblkcnt64_t f_blocks;
	fsblkcnt64_t f_bfree;
	fsblkcnt64_t f_bavail;
	fsfilcnt64_t f_files;
	fsfilcnt64_t f_ffree;
	fsfilcnt64_t f_favail;
	unsigned long int f_fsid;
	unsigned long int f_flag;
	unsigned long int f_namemax;
};

struct flock {
	short l_type;
	short l_whence;
	__int64 l_start;
	__int64 l_len;
	pid_t l_pid;
};

#endif

//We have a choice between CRT-compatible 32-bit off_t definition
//and a custom 64-bit definition
#define WIDE_OFF_T 1
#ifndef WIDE_OFF_T
#define FUSE_OFF_T off_t
#define FUSE_STAT stat

#else
#define FUSE_OFF_T __int64
#define FUSE_STAT _stati64
#if 0
struct stat64 {
	dev_t st_dev;
	ino_t st_ino;
	unsigned short st_mode;
	short st_nlink;
	short st_uid;
	short st_gid;
	dev_t st_rdev;
	FUSE_OFF_T st_size;
	time_t st_atime;
	time_t st_mtime;
	time_t st_ctime;
};
#endif
#endif


#define F_WRLCK	1
#define F_UNLCK	2
#define F_SETLK	6

#endif //_FUSE_WIN_H_
