/* ----------------------------------------------------------- *
* Win32 helper functions                                       *
* Compilation on MSVC requires /Zc:wchar_t compiler option     *
* ----------------------------------------------------------- */
#ifndef FUSE_WIN_H_
#define FUSE_WIN_H_

#include <time.h>
#include <sys/types.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
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
int ntstatus_error_to_errno(int win_res);
int errno_to_ntstatus_error(int err);
NTSTATUS lasterror_to_ntstatus(DWORD last_err);

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

#if !defined(HAVE_STRUCT_TIMESPEC) && !defined(__CYGWIN__) && !defined(_TIMESPEC_DEFINED) && defined(_CRT_NO_TIME_T) /* win32 pthread.h time.h defines it */
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
typedef struct timespec timestruc_t;
typedef unsigned short nlink_t;
typedef unsigned __int64 uint64_t;
typedef unsigned __int32 uint32_t;
typedef unsigned int blksize_t;
typedef unsigned __int64 blkcnt_t;

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
typedef struct timespec timestruc_t;
typedef unsigned int mode_t;
typedef unsigned short nlink_t;
typedef unsigned int pid_t;
typedef unsigned int gid_t;
typedef unsigned int uid_t;
typedef unsigned int blksize_t;
typedef unsigned __int64 blkcnt_t;
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
// #define FUSE_STAT _stati64
// use stat from cygwin instead for having more members and 
// being more compatible
// stat ported from cygwin sys/stat.h

// for binary compatible
#define FUSE_DEV_T uint64_t
#define FUSE_MODE_T uint32_t
#define FUSE_NLINK_T uint32_t
#define FUSE_UID_T uint32_t
#define FUSE_GID_T uint32_t
#define FUSE_BLKSIZE_T uint64_t
#define FUSE_BLKCNT_T uint64_t
typedef struct __fuse_timespec
{
	uint64_t tv_sec;				/* Seconds.  */
	uint64_t tv_nsec;           /* Nanoseconds.  */
} FUSE_TIMESPEC_T;

struct stat64_cygwin
{
	FUSE_DEV_T    st_dev;
	uint64_t      st_ino;
	FUSE_MODE_T   st_mode;
	FUSE_NLINK_T  st_nlink;
	FUSE_UID_T    st_uid;
	FUSE_GID_T    st_gid;
	FUSE_DEV_T    st_rdev;
	FUSE_OFF_T    st_size;
	FUSE_TIMESPEC_T   st_atim;
	FUSE_TIMESPEC_T   st_mtim;
	FUSE_TIMESPEC_T   st_ctim;
	FUSE_BLKSIZE_T    st_blksize;
	FUSE_BLKCNT_T     st_blocks;
	FUSE_TIMESPEC_T   st_birthtim;
};
/* The following breaks struct stat definiton in native Windows stats.h
* So whenever referencing st_atime|st_ctime|st_mtime, replacing is needed.
*/
/*
#define st_atime st_atim.tv_sec
#define st_ctime st_ctim.tv_sec
#define st_mtime st_mtim.tv_sec
*/
#define FUSE_STAT stat64_cygwin
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

#endif // FUSE_WIN_H_
