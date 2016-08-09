#include <stdafx.h>

/*
 * Copyright (c) 1998-2009 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 *
 *	File:	fsx.c
 *	Author:	Avadis Tevanian, Jr.
 *
 *	File system exerciser. 
 *
 *	Rewrite and enhancements 1998-2003 Conrad Minshall -- conrad@mac.com
 *
 *	Various features from Joe Sokol, Pat Dirks, and Clark Warner.
 *
 *	Small changes to work under Linux -- davej@suse.de
 *
 *	Sundry porting patches from Guy Harris 12/2001
 *
 *	Checks for mmap last-page zero fill.
 *
 *	Oct 2006: Now includes Wenguang's changes, Jim's Named Fork support,
 *		Peter's EA changes, and XILog additions
 *
 *	Various features/enhancements from Mike Mackovitch -- macko@apple.com
 *  
 *	Added no-cached r/w option May 2008 -- rtucker@apple.com, bsuinn@apple.com 
 *
 * Compile with:
 	cc -Wall -O3 fsx.c -o fsx
 	gcc -arch ppc -arch i386 -arch ppc64 -arch x86_64 -Wall -O3 fsx.c -o fsx -DXILOG -F/AppleInternal/Library/Frameworks -framework XILog
 *
 */

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include "win/mman.h"
#include "win/getopt.h"
#else
#include <sys/xattr.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#if defined(__APPLE__)
#include <sys/paths.h>
#endif
#include <sys/param.h>
#ifdef _UWIN
# include <limits.h>
# include <strings.h>
#endif
#include <fcntl.h>
#include <sys/mman.h>
#ifndef MAP_FILE
# define MAP_FILE 0
#endif
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <libgen.h>
#endif
#ifdef XILOG
# include <XILog/XILog.h>
#endif

#if defined(_WIN32)
#define MAXPATHLEN                      MAX_PATH
#undef _IOLBF
#define _IOLBF                          _IONBF

#define _PATH_FORKSPECIFIER             ":"
#define F_NOCACHE                       1000000
#define SIGHUP                          1000
#define SIGPIPE                         1000
#define SIGALRM                         1000
#define SIGXCPU                         1000
#define SIGXFSZ                         1000
#define SIGVTALRM                       1000
#define SIGUSR1                         1000
#define SIGUSR2                         1000

#define open(path, oflag, mode)         open(path, oflag | O_BINARY, mode)
#define signal(sig, func)               (SIGINT == (sig) ? signal(sig, (_crt_signal_t)func) : 0)

#if defined(_WIN64)
typedef __int64 ssize_t;
#define SSIZE_MAX                       _I64_MAX
#else
typedef int ssize_t;
#define SSIZE_MAX                       INT_MAX
#endif

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

int __acrt_errno_map_os_error(const DWORD err);

static inline int syserror(int err)
{
	//void __cdecl __acrt_errno_map_os_error(unsigned long const oserrno);
	__acrt_errno_map_os_error(0 == err ? GetLastError() : err);
	return -1;
}
static inline void bzero(void *s, size_t n)
{
    memset(s, 0, n);
}
static inline int fcntl(int fildes, int cmd, ...)
{
    return 0;
}
static inline int fsync(int fd)
{
    return FlushFileBuffers((HANDLE)_get_osfhandle(fd)) ? 0 : syserror(0);
}
static inline int ftruncate(int fd, off_t length)
{
    return _chsize(fd, length);
}
static inline int getopt_long(int argc, char **argv, char *optstring,
    const struct option *longopts, int *longindex)
{
    return getopt(argc, argv, optstring);
}
static inline int getpagesize(void)
{
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwAllocationGranularity;
}
static inline int getpid(void)
{
    return GetCurrentProcessId();
}
static inline unsigned int sleep(unsigned int seconds)
{
    Sleep(seconds * 1000);
    return 0;
}

char *initstate(unsigned long seed, char *state, long size);
long random(void);
char *setstate(const char *state);
void srandom(unsigned long seed);

/* extended attribute implementation */
#define EA_NAMEMAX                      (255)
#define EA_SIZEMAX                      (64 * 1024)

extern "C"
{
	typedef struct _FILE_GET_EA_INFORMATION {
		ULONG NextEntryOffset;
		UCHAR EaNameLength;
		CHAR EaName[1];
	} FILE_GET_EA_INFORMATION, *PFILE_GET_EA_INFORMATION;

	typedef struct _FILE_FULL_EA_INFORMATION {
		ULONG NextEntryOffset;
		UCHAR Flags;
		UCHAR EaNameLength;
		USHORT EaValueLength;
		CHAR EaName[1];
	} FILE_FULL_EA_INFORMATION, *PFILE_FULL_EA_INFORMATION;

	typedef struct _IO_STATUS_BLOCK {
		union {
			NTSTATUS Status;
			PVOID Pointer;
		};
		ULONG_PTR Information;
	} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

	NTSYSAPI NTSTATUS NTAPI NtQueryEaFile(
		IN HANDLE               FileHandle,
		OUT PIO_STATUS_BLOCK    IoStatusBlock,
		OUT PVOID               Buffer,
		IN ULONG                Length,
		IN BOOLEAN              ReturnSingleEntry,
		IN PVOID                EaList OPTIONAL,
		IN ULONG                EaListLength,
		IN PULONG               EaIndex OPTIONAL,
		IN BOOLEAN              RestartScan);

	NTSYSAPI NTSTATUS NTAPI NtSetEaFile(
		IN HANDLE               FileHandle,
		OUT PIO_STATUS_BLOCK    IoStatusBlock,
		IN PVOID                EaBuffer,
		IN ULONG                EaBufferSize);
}

static inline ssize_t fgetxattr(int fd, const char *name, void *value, size_t size,
    uint32_t position, int options)
{
    size_t namelen = strlen(name);
    if (namelen > EA_NAMEMAX)
    {
    	errno = ENAMETOOLONG;
    	return -1;
    }
    PFILE_FULL_EA_INFORMATION eainfo = (PFILE_FULL_EA_INFORMATION)malloc(EA_SIZEMAX);
    if (0 == eainfo)
    {
    	//errno = ENOMEM; /* malloc already sets this */
        return -1;
    }
    size_t geasize = sizeof(FILE_GET_EA_INFORMATION) + namelen;
    PFILE_GET_EA_INFORMATION geainfo = (PFILE_GET_EA_INFORMATION)_alloca(geasize);
    geainfo->NextEntryOffset = 0;
    geainfo->EaNameLength = (UCHAR)namelen;
    memcpy(geainfo->EaName, name, geainfo->EaNameLength + 1);
    IO_STATUS_BLOCK iosb;
    int errno_ = 0;
    ssize_t res = 0 <= NtQueryEaFile((HANDLE)_get_osfhandle(fd), &iosb,
        eainfo, EA_SIZEMAX, TRUE, geainfo, geasize, 0, FALSE) ? eainfo->EaValueLength : -1;
    if (0 < res)
    {
        if (res <= size)
            memcpy(value, eainfo->EaName + eainfo->EaNameLength + 1, res);
        else if (0 != value)
        {
        	errno_ = ERANGE;
            res = -1;
        }
    }
    else
    {
    	errno_ = EPERM; /* catchall error */
        res = -1;
    }
    free(eainfo);
    if (-1 == res)
    	errno = errno_;
    return res;
}
static inline int fsetxattr(int fd, const char *name, void *value, size_t size,
    uint32_t position, int options)
{
    size_t namelen = strlen(name);
    if (namelen > EA_NAMEMAX)
    {
    	errno = ENAMETOOLONG;
    	return -1;
    }
    size_t easize = offsetof(FILE_FULL_EA_INFORMATION, EaName) + namelen + 1 + size;
    if (easize > EA_SIZEMAX)
    {
    	errno = ERANGE;
        return -1;
    }
    PFILE_FULL_EA_INFORMATION eainfo = (PFILE_FULL_EA_INFORMATION)malloc(easize);
    if (0 == eainfo)
    {
    	//errno = ENOMEM; /* malloc already sets this */
        return -1;
    }
    eainfo->NextEntryOffset = 0;
    eainfo->Flags = 0;
    eainfo->EaNameLength = (UCHAR)namelen;
    eainfo->EaValueLength = (USHORT)size;
    memcpy(eainfo->EaName, name, eainfo->EaNameLength + 1);
    memcpy(eainfo->EaName + eainfo->EaNameLength + 1, value, eainfo->EaValueLength);
    IO_STATUS_BLOCK iosb;
    int res = 0 <= NtSetEaFile((HANDLE)_get_osfhandle(fd), &iosb, eainfo, easize) ? 0 : -1;
    free(eainfo);
    if (-1 == res)
    	errno = EPERM; /* catchall error */
    return res;
}
static inline int fremovexattr(int fd, const char *name, int options)
{
    return fsetxattr(fd, name, 0, 0, 0, options);
}
#endif

#if defined(__linux__)
#include <inttypes.h>

#define _PATH_FORKSPECIFIER    "/..namedfork/"

#define F_NOCACHE 1000000
#define fcntl(fd, cmd, ...)\
    (F_NOCACHE == cmd ? 0 : fcntl(fd, cmd, __VA_ARGS__))

#define fgetxattr(fd, name, value, size, position, options)\
    fgetxattr(fd, name, value, size)
#define fsetxattr(fd, name, value, size, position, options)\
    fsetxattr(fd, name, value, size, options)
#define fremovexattr(fd, name, options)\
    fremovexattr(fd, name)
#endif

#if defined(_WIN64) || defined(__linux__)
static size_t
strlcpy(char *dst, const char *src, size_t maxlen) {
    const size_t srclen = strlen(src);
    if (srclen < maxlen) {
        memcpy(dst, src, srclen+1);
    } else if (maxlen != 0) {
        memcpy(dst, src, maxlen-1);
        dst[maxlen-1] = '\0';
    }
    return srclen;
}
static size_t
strlcat(char *dst, const char *src, size_t maxlen) {
    const size_t srclen = strlen(src);
    const size_t dstlen = strnlen(dst, maxlen);
    if (dstlen == maxlen) return maxlen+srclen;
    if (srclen < maxlen-dstlen) {
        memcpy(dst+dstlen, src, srclen+1);
    } else {
        memcpy(dst+dstlen, src, maxlen-dstlen-1);
        dst[maxlen-1] = '\0';
    }
    return dstlen + srclen;
}
#endif

/*
 *	A log entry is an operation and a bunch of arguments.
 */

struct log_entry {
	int	opnum;
	int	operation;
	int	args[3];
};

#define	DEFAULT_LOGSIZE	1024

struct log_entry	*oplog;		/* the log */
int			logptr = 0;	/* current position in log */
int			logcount = 0;	/* total ops */

/*
 *	Define operations
 */

#define	OP_READ		1
#define OP_WRITE	2
#define OP_TRUNCATE	3
#define OP_CLOSEOPEN	4
#define OP_MAPREAD	5
#define OP_MAPWRITE	6
#define OP_SKIPPED	7
#define OP_NOCACHEREAD	8
#define OP_NOCACHEWRITE 9

int page_size;
int page_mask;

char	*good_buf;			/* a pointer to the correct data */
char	*temp_buf;			/* a pointer to the current data */
char	fname[MAXPATHLEN];		/* name of our test file */
int	fd;				/* fd for our test file */
int	ea_lastwrite = 0;		/* Size of the last EA Write */
char    *eaname;                        /* Name of the EA key */

off_t		file_size = 0;
off_t		biggest = 0;
char		state[256];
unsigned long	testcalls = 0;		/* calls to function "test" */

unsigned long	simulatedopcount = 0;	/* -b flag */
int	closeprob = 0;			/* -c flag */
int	debug = 0;			/* -v flag */
unsigned long	debugstart = 0;		/* -D flag */
int	ea = 0;                         /* -e flag */
unsigned long	maxfilelen = 256 * 1024;	/* -l flag */
int	sizechecks = 1;			/* -n flag disables them */
int	maxoplen = 64 * 1024;		/* -o flag */
int	quiet = 0;			/* -q flag */
unsigned long progressinterval = 0;	/* -p flag */
int	readbdy = 1;			/* -r flag */
int	style = 0;			/* -s flag */
int	truncbdy = 1;			/* -t flag */
int	writebdy = 1;			/* -w flag */
long	monitorstart = -1;		/* -m flag */
long	monitorend = -1;		/* -m flag */
int	lite = 0;			/* -L flag */
long	numops = -1;			/* -N flag */
int	randomoplen = 1;		/* -O flag disables it */
int	seed = 0;			/* -S flag */
int     mapped_writes = 1;	      /* -W flag disables */
int 	mapped_reads = 1;		/* -R flag disables it */
int	logsize = DEFAULT_LOGSIZE;	/* -G flag */
int	datasize = 4;			/* -T flag */
int	modsize = 0;
int	fsxgoodfd = -1;
FILE *	fsxlogf = NULL;
int badoff = -1;
int closeopen = 0;
int sync_before_close = 0; /* -y flag enables it */
int interactive = 0; /* -i flag interactive */
int interactiveSince = -1; /* -I flag when to start interactive */
int usehole = 1; // by default use hole (sparse file)
int slow_motion = 0;
long	duration = 0;	/* -d flag */
unsigned int pinginterval = 10000; // About every 30sec?
#ifdef XILOG
XILogRef xilogref;
#endif
int	gUseRandomNoCache = 0; /* -C randomly mix cached and un-cached r/w ops */

char *msgbuf;
int msgbuflen;

void dotruncate(unsigned size);
void writefileimage(void);

#define get_data_at(cp) \
	(((*(((unsigned char *)(cp)) + 0)) << 0) | \
	 ((datasize < 2) ? 0 : \
	  ((*(((unsigned char *)(cp)) + 1)) << 8)) | \
	 ((datasize < 4) ? 0 : \
	  (((*(((unsigned char *)(cp)) + 2)) << 16) | \
	   ((*(((unsigned char *)(cp)) + 3)) << 24))))

#define set_data_at(cp, val) \
	do { \
	(*(((unsigned char *)(cp)) + 0)) = ((val) >> 0) & 0xff; \
	if (datasize < 2) break; \
	(*(((unsigned char *)(cp)) + 1)) = (((val) >> 8) & 0xff); \
	if (datasize < 4) break; \
	(*(((unsigned char *)(cp)) + 2)) = (((val) >> 16) & 0xff); \
	(*(((unsigned char *)(cp)) + 3)) = (((val) >> 24) & 0xff); \
	} while (0)

#define SHOWLOGENTRY(OPSTART, OPEND) \
	(!quiet && \
	 ((progressinterval && (testcalls % progressinterval == 0)) || \
          (debug && \
	   ((monitorstart == -1) || \
	    (((OPEND) > monitorstart) && \
	     ((monitorend == -1) || ((OPSTART) <= monitorend)))))))

void docloseopen(void);

int parsetime(char *t)
{
	int i = 0;
	int secs = 0;
	char b[128]; bzero(b, 128);

	for (i=0; i < strlen(t); i++) {
		switch (t[i]) {
			case 's':
				secs += atoi(b);
				bzero(b, 128);
				break;
			case 'm':
				secs += atoi(b) * 60;
				bzero(b, 128);
				break;
			case 'h':
				secs += atoi(b) * 60 * 60;
				bzero(b, 128);
				break;
			case 'd':
				secs += atoi(b) * 60 * 60 * 24;
				bzero(b, 128);
				break;
			case 'w':
				secs += atoi(b) * 60 * 60 * 24 * 7;
				bzero(b, 128);
				break;
			case 'y':
				secs += atoi(b) * 60 * 60 * 24 * 365;
				bzero(b, 128);
				break;
			default:
				sprintf(b, "%s%c", b, t[i]);
		}
	}
	if (secs == 0) // maybe they just gave us a number?
		secs = atoi(t);
	return(secs);
}


void
mvwarnc(int code, const char *fmt, va_list ap)
{
	char buf[1024];

	ZeroMemory(buf, sizeof(buf));

	buf[0] = 'f';
	buf[1] = 's';
	buf[2] = 'x';
	buf[3] = ':';
	buf[4] = ' ';

	vsprintf_s(&buf[5], sizeof(buf) - 5, fmt, ap);

	size_t len = strlen(buf);

	if(len < sizeof(buf) - 3)
	{
		buf[len] = ':';
		++len;
		
		buf[len] = ' ';
		++len;
	}

	strcat_s(&buf[len], sizeof(buf) - len, strerror(code));

	Logger::WriteMessage(buf);
}


void
mwarn(const char * fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
#ifdef XILOG
	XILogMsg(fmt, ap);
	XILogMsg("%s", strerror(errno));
	va_end(ap);
	va_start(ap, fmt);
#endif
	mvwarnc(errno, fmt, ap);
	va_end(ap);
}


void
prt(char *fmt, ...)
{
	va_list args;
	char buf[1024];

	ZeroMemory(buf, sizeof(buf));

	va_start(args, fmt);
#ifdef XILOG
	XILogMsg(fmt, args);
	va_end(args);
	va_start(args, fmt);
#endif
	vsprintf_s(buf, fmt, args);
	
	va_end(args);

	Logger::WriteMessage(buf);

	if (fsxlogf) {
		va_start(args, fmt);
		vfprintf(fsxlogf, fmt, args);
		fflush(fsxlogf);
		va_end(args);
	}
}

void
prt2(char *fmt, ...)
{
	va_list args;
	char buf[1024];

	ZeroMemory(buf, sizeof(buf));

	va_start(args, fmt);

	vsprintf_s(buf, fmt, args);

	va_end(args);

	Logger::WriteMessage(buf);

	if (fsxlogf) {
		va_start(args, fmt);
		vfprintf(fsxlogf, fmt, args);
		va_end(args);
	}
}


void
prterr(char *prefix)
{
#ifdef XILOG
	XILogErr("%s%s%s", prefix, prefix ? ": " : "", strerror(errno));
#endif
	prt2("%s%s%s\n", prefix, prefix ? ": " : "", strerror(errno));
}


int
log4(int operation, int arg0, int arg1, int arg2)
{
	int logent = logptr;
	struct log_entry *le = &oplog[logent];
	le->opnum = logcount + 1;
	le->operation = operation;
	if (closeopen)
		le->operation = ~ le->operation;
	le->args[0] = arg0;
	le->args[1] = arg1;
	le->args[2] = arg2;
	logptr++;
	logcount++;
	if (msgbuflen) {
		snprintf(msgbuf, msgbuflen+1, "%d", logcount);
	}
	if (logptr >= logsize)
		logptr = 0;
	return logent;
}


void
offset_last_mod(int offset, int *write, int *tdown, int *tup)
{
	int	i, count, down, opnum, operation;
	struct log_entry	*lp;

	*write = *tdown = *tup = -1;

	opnum = logcount;
	i = logptr - 1;
	if (i < 0)
		i = logsize - 1;
	if (logcount < logsize) {
		count = logcount;
	} else {
		count = logsize;
	}
	for ( ; count > 0; count--, opnum--) {
		lp = &oplog[i];
		operation = lp->operation;
		if ((closeopen = operation < 0))
			operation = ~ operation;
		switch (operation) {
		case OP_MAPWRITE:
		case OP_WRITE:
			if (*write != -1)
				break;
			if ((offset >= lp->args[0]) && (offset < (lp->args[0] + lp->args[1])))
				*write = opnum;
			break;
		case OP_TRUNCATE:
			down = lp->args[0] < lp->args[1];
			if ((offset >= lp->args[!down]) && (offset < lp->args[!!down])) {
				if (down && (*tdown == -1))
					*tdown = opnum;
				else if (!down && (*tup == -1))
					*tup = opnum;
			}
			break;
		}
		if ((*write != -1) && (*tdown != -1) && (*tup != -1))
			return;
		i--;
		if (i < 0)
			i = logsize - 1;
	}
}

void
logentrydump(int logent, int opnum)
{
	struct log_entry *lp = &oplog[logent];
	int down;

	if (modsize)
		prt("%d(%d): ", opnum, opnum%modsize);
	else
		prt("%d: ", opnum);
	if ((closeopen = lp->operation < 0))
		lp->operation = ~ lp->operation;
		
	switch (lp->operation) {
	case OP_MAPREAD:
		prt("%-15s 0x%x (%d) thru 0x%x (%d)\t(0x%x (%d) bytes)", "MAPREAD",
		    lp->args[0], lp->args[0],
		    lp->args[0] + lp->args[1] - 1, lp->args[0] + lp->args[1] - 1,
		    lp->args[1], lp->args[1]);
		if ((badoff >= lp->args[0]) && (badoff < lp->args[0] + lp->args[1]))
		{
			prt("\t***RRRR***");
		}
		break;
	case OP_MAPWRITE:
		prt("%-15s 0x%x (%d) thru 0x%x (%d)\t(0x%x (%d) bytes)", "MAPWRITE",
		    lp->args[0], lp->args[0],
		    lp->args[0] + lp->args[1] - 1, lp->args[0] + lp->args[1] - 1,
		    lp->args[1], lp->args[1]);
		if ((badoff >= lp->args[0]) && (badoff < lp->args[0] + lp->args[1]))
		{
			prt("\t******WWWW");
		}
		break;
	case OP_READ:
		prt("%-15s 0x%x (%d) thru 0x%x (%d)\t(0x%x (%d) bytes)", "READ",
		    lp->args[0], lp->args[0],
		    lp->args[0] + lp->args[1] - 1, lp->args[0] + lp->args[1] - 1,
		    lp->args[1], lp->args[1]);
		if (badoff >= lp->args[0] &&
		    badoff < lp->args[0] + lp->args[1])
		{
			prt("\t***RRRR***");
		}
		break;
	case OP_WRITE:
		prt("%-15s 0x%x (%d) thru 0x%x (%d)\t(0x%x (%d) bytes)", "WRITE",
		    lp->args[0], lp->args[0],
		    lp->args[0] + lp->args[1] - 1, lp->args[0] + lp->args[1] - 1,
		    lp->args[1], lp->args[1]);
		if (lp->args[0] > lp->args[2])
		{
			prt(" HOLE");
		}
		else if ((lp->args[0] + lp->args[1]) > lp->args[2])
		{
			prt(" EXTEND");
		}
		if (((badoff >= lp->args[0]) || (badoff >= lp->args[2])) &&
		    (badoff < (lp->args[0] + lp->args[1])))
		{
			prt("\t***WWWW");
		}
		break;
	case OP_NOCACHEREAD:
		prt("%-15s 0x%x (%d) thru 0x%x (%d)\t(0x%x (%d) bytes)", "NOCACHEREAD",
		    lp->args[0], lp->args[0],
		    lp->args[0] + lp->args[1] - 1, lp->args[0] + lp->args[1] - 1,
		    lp->args[1], lp->args[1]);
		if (badoff >= lp->args[0] &&
		    badoff < lp->args[0] + lp->args[1])
		{
			prt("\t***RRRR***");
		}
		break;
	case OP_NOCACHEWRITE:
		prt("%-15s 0x%x (%d) thru 0x%x (%d)\t(0x%x (%d) bytes)", "NOCACHEWRITE",
		    lp->args[0], lp->args[0],
		    lp->args[0] + lp->args[1] - 1, lp->args[0] + lp->args[1] - 1,
		    lp->args[1], lp->args[1]);
		if (lp->args[0] > lp->args[2])
		{
			prt(" HOLE");
		}
		else if ((lp->args[0] + lp->args[1]) > lp->args[2])
		{
			prt(" EXTEND");
		}
		if (((badoff >= lp->args[0]) || (badoff >= lp->args[2])) &&
		    (badoff < (lp->args[0] + lp->args[1])))
		{
			prt("\t***WWWW");
		}
		break;
	case OP_TRUNCATE:
		down = lp->args[0] < lp->args[1];
		prt("%-15s from 0x%x (%d) to 0x%x (%d)",
		    down ? "TRUNCATE DOWN" : "TRUNCATE UP",
		    lp->args[1], lp->args[1],
		    lp->args[0], lp->args[0]);
		if ((badoff >= lp->args[!down]) &&
		    (badoff < lp->args[!!down]))
		{
			prt("\t******WWWW");
		}
		break;
	case OP_SKIPPED:
		prt("SKIPPED (no operation)");
		break;
	default:
		prt("BOGUS LOG ENTRY (operation code = %d)!",
		    lp->operation);
	}
	if (closeopen)
	{
		prt("\n\t\tCLOSE/OPEN");
	}
	prt("\n");
}


void
logdump(void)
{
	int	i, count, opnum;

	// don't dump log if we've been logging ops via debug
	if (debug) return;

	prt("LOG DUMP (%d total operations):\n", logcount);
	if (logcount < logsize) {
		i = 0;
		count = logcount;
		opnum = 1;
	} else {
		i = logptr;
		count = logsize;
		opnum = 1 + logcount - logsize;
	}
	for ( ; count > 0; count--, opnum++) {
		logentrydump(i, opnum);
		i++;
		if (i == logsize)
			i = 0;
	}
}


void
save_buffer(char *buffer, off_t bufferlength, int fd)
{
	off_t ret;
	ssize_t byteswritten;

	if (fd <= 0 || bufferlength == 0)
		return;

	if (bufferlength > SSIZE_MAX) {
		prt("fsx flaw: overflow in save_buffer\n");
		Assert::Fail(L"fsx flaw: overflow in save_buffer.", LINE_INFO());
		//exit(67);
	}
	if (lite) {
		off_t size_by_seek = lseek(fd, (off_t)0, SEEK_END);
		if (size_by_seek == (off_t)-1)
			prterr("save_buffer: lseek eof");
		else if (bufferlength > size_by_seek) {
			mwarn("WARNING: save_buffer: .fsxgood file too short... will save 0x%llx bytes instead of 0x%llx\n", (unsigned long long)size_by_seek,
			     (unsigned long long)bufferlength);
			bufferlength = size_by_seek;
		}
	}

	ret = lseek(fd, (off_t)0, SEEK_SET);
	if (ret == (off_t)-1)
		prterr("save_buffer: lseek 0");
	
	byteswritten = write(fd, buffer, (size_t)bufferlength);
	if (byteswritten != bufferlength) {
		if (byteswritten == -1)
			prterr("save_buffer write");
		else
			mwarn("WARNING: save_buffer: short write, 0x%x bytes instead of 0x%llx\n",
			     (unsigned)byteswritten,
			     (unsigned long long)bufferlength);
	}
}


void
failure(int status)
{
	if (fsxgoodfd >= 0) {
		if (good_buf) {
			save_buffer(good_buf, file_size, fsxgoodfd);
			prt("Correct content saved for comparison\n");
			prt("(maybe hexdump \"%s\" vs \"%s.fsxgood\")\n",
			    fname, fname);
		}
		close(fsxgoodfd);
	}
	prt("Seed was set to %d\n", seed);
	prt("Failure with status %d\n", status);
	//exit(status);
	Assert::Fail(L"failure()", LINE_INFO());
}


void
check_buffers(unsigned offset, unsigned size)
{
	unsigned int c, t;
	unsigned i = 0;
	unsigned n = 0;
	unsigned sizeleft = size;
	unsigned start, good, bad;
	int op, w, td, tu;

	start = good = bad = 0;
	op = -1;

	if (memcmp(good_buf + offset, temp_buf, size) != 0) {
		prt("data miscompare @ %d\n", offset);
		while (sizeleft > 0) {
			c = get_data_at(&good_buf[offset+i]);
			t = get_data_at(&temp_buf[i]);
			if (c != t) {
				if (n == 0) {
					start = offset + i;
					good = c;
					bad = t;
					op = ((t > 0) && (t <= logcount)) ? t : -1;
				}
				n+=datasize;
				if (badoff < 0) {
					badoff = offset + i;
					logdump();
					prt("data miscompare @ %d\n", offset);
					prt("%-10s %-10s %-10s %-10s %-8s Last: %-8s %-8s %-8s\n",
					    "OFFSET", "GOOD", "BAD", "LENGTH", "BADOP#",
					    "WRITE", "TRUNC-", "TRUNC+");
				}
			}
			i+=datasize;
			sizeleft-=datasize;
			if (n && ((c == t) || (sizeleft <= 0))) {
				w = td = tu = -1;
				offset_last_mod(start, &w, &td, &tu);
				prt("0x%08x 0x%08x 0x%08x 0x%08x %-8d       %-8d %-8d %-8d\n",
					start, good, bad, n, op, w, td, tu);
				if (c == t)
					n = 0;
			}
		}
		if (badoff == -1) {
			logdump();
			prt("transient data miscompare @ %d ????????\n", offset);
			prt("memcmp(%d,%d) failed but no differences found ????????\n", offset, size);
		}
		failure(110);
	}
}


void
check_size(void)
{
	struct stat	statbuf;
	off_t	size_by_seek;

	if (fstat(fd, &statbuf)) {
		prterr("check_size: fstat");
		statbuf.st_size = -1;
	}
	size_by_seek = lseek(fd, (off_t)0, SEEK_END);
	if (file_size != statbuf.st_size || file_size != size_by_seek) {
		logdump();
		prt("Size error: expected 0x%llx stat 0x%llx seek 0x%llx\n",
		    (unsigned long long)file_size,
		    (unsigned long long)statbuf.st_size,
		    (unsigned long long)size_by_seek);
		failure(120);
	}
}


void
check_trunc_hack(void)
{
	struct stat statbuf;
	ftruncate(fd, (off_t)0);

	if (!usehole)
		return;

	ftruncate(fd, (off_t)100000);
	fstat(fd, &statbuf);
	if (statbuf.st_size != (off_t)100000) {
		prt("no extend on truncate! not posix!\n");
		//exit(130);
		Assert::Fail(L"No extend on truncate! not posix!", LINE_INFO());
	}
	ftruncate(fd, (off_t)0);
}


void
doread(unsigned offset, unsigned size)
{
	off_t ret;
	unsigned iret;
	int logent;
	int cache_off = 0;

	if (ea)
		size = ea_lastwrite;

	offset -= offset % readbdy;
	if (size == 0) {
		if (debug && testcalls > simulatedopcount)
		{
			prt("skipping zero size read\n");
		}
		log4(OP_SKIPPED, OP_READ, offset, size);
		return;
	}
	if (size + offset > file_size) {
		if (debug && testcalls > simulatedopcount)
		{
			prt("skipping seek/read past end of file\n");
		}
		log4(OP_SKIPPED, OP_READ, offset, size);
		return;
	}
	
	/* When the gUseRandomNoCache option is enable (-C),
	 * randomly turn caching off for 30% of the read calls.
	 * Re-enable caching after the read is complete.
	 */
	if (gUseRandomNoCache && ((random() % 100) < 30)) {
		cache_off = 1;
		logent = log4(OP_NOCACHEREAD, offset, size, 0);
	} else {
		logent = log4(OP_READ, offset, size, 0);
	}

	if (SHOWLOGENTRY(offset, offset + size))
	{
		logentrydump(logent, logcount);
	}

	if (testcalls <= simulatedopcount)
		return;

	if (interactive) {
		printf("Hit return when ready...");
		getchar();
	}

	if (!ea) {
		ret = lseek(fd, (off_t)offset, SEEK_SET);
		if (ret == (off_t)-1) {
			logdump();
			prterr("doread: lseek");
			failure(140);
		}
				
		if (cache_off && (fcntl(fd, F_NOCACHE, 1) != 0)) { // turn data caching off
			logdump();
			prterr("doread: fcntl(F_NOCACHE, 1)");
			failure(201);
		}
		iret = read(fd, temp_buf, size);
		if (cache_off && (fcntl(fd, F_NOCACHE, 0) != 0)) {
			logdump();
			prterr("doread: fcntl(F_NOCACHE, 0)");
			failure(201);
		}
	} else {
		iret = fgetxattr(fd, eaname, temp_buf, size, 0, 0);
	}

	if (iret != size) {
		logdump();
		if (iret == -1)
			prterr("doread: read");
		else
			prt("short read: 0x%x bytes instead of 0x%x\n", iret, size);
		failure(141);
	}

	check_buffers(offset, size);
}


void
check_eofpage(char *s, unsigned offset, char *p, int size)
{
#if !defined(_WIN32)
    uintptr_t last_page, should_be_zero;

	if (offset + size <= (file_size & ~page_mask))
		return;
	/*
	 * we landed in the last page of the file
	 * test to make sure the VM system provided 0's 
	 * beyond the true end of the file mapping
	 * (as required by mmap def in 1996 posix 1003.1)
	 */
	last_page = ((uintptr_t)p + (offset & page_mask) + size) & ~page_mask;

	for (should_be_zero = last_page + (file_size & page_mask);
	     should_be_zero < last_page + page_size;
	     should_be_zero++)
		if (*(char *)should_be_zero) {
			logdump();
			prt("Mapped %s: non-zero data past EOF (0x%llx) page offset 0x%x is 0x%04x\n",
			    s, file_size - 1, should_be_zero & page_mask,
			    (*(char *)should_be_zero));
			failure(205);
		}
#endif
}


void
domapread(unsigned offset, unsigned size)
{
	unsigned pg_offset;
	unsigned map_size;
	char    *p;
	int logent;

	if (ea)
		return;

	offset -= offset % readbdy;
	if (size == 0) {
		if (debug && testcalls > simulatedopcount)
		{
			prt("skipping zero size read\n");
		}
		log4(OP_SKIPPED, OP_MAPREAD, offset, size);
		return;
	}
	if (size + offset > file_size) {
		if (debug && testcalls > simulatedopcount)
		{
			prt("skipping seek/read past end of file\n");
		}
		log4(OP_SKIPPED, OP_MAPREAD, offset, size);
		return;
	}

	logent = log4(OP_MAPREAD, offset, size, 0);

	if (SHOWLOGENTRY(offset, offset + size))
	{
		logentrydump(logent, logcount);
	}

	if (testcalls <= simulatedopcount)
		return;

	pg_offset = offset & page_mask;
	map_size  = pg_offset + size;

	if (interactive) {
		printf("Hit return when ready...");
		getchar();
	}

	if ((p = (char *)mmap(0, map_size, PROT_READ, MAP_FILE | MAP_SHARED, fd,
			      (off_t)(offset - pg_offset))) == (char *)-1) {
		logdump();
		prterr("domapread: mmap");
		failure(190);
	}
	memcpy(temp_buf, p + pg_offset, size);

	check_eofpage("Read", offset, p, size);

	if (munmap(p, map_size) != 0) {
		logdump();
		prterr("domapread: munmap");
		failure(191);
	}

	check_buffers(offset, size);
}


void
gendata(char *good_buf, unsigned offset, unsigned size)
{
	while (size > 0) {
		size -= datasize;
		set_data_at(&good_buf[offset], testcalls);
		offset += datasize;
	}
}


void
dowrite(unsigned offset, unsigned size)
{
	off_t ret;
	unsigned iret;
	int logent;
	int	cache_off = 0;
	
	offset -= offset % writebdy;
	if (size == 0) {
		if (debug && testcalls > simulatedopcount)
		{
			prt("skipping zero size write\n");
		}
		log4(OP_SKIPPED, OP_WRITE, offset, size);
		return;
	}
	
	/* When the gUseRandomNoCache option is enable (-C),
	 * randomly turn caching off for 30% of the write calls.
	 * Re-enable caching after the write is complete.
	 */
	if (gUseRandomNoCache && ((random() % 100) < 30)) { // turn data caching off
		cache_off = 1;
		logent = log4(OP_NOCACHEWRITE, offset, size, file_size);
	} else {
		logent = log4(OP_WRITE, offset, size, file_size);
	}

	gendata(good_buf, offset, size);

	if (SHOWLOGENTRY(offset, offset + size))
	{
		logentrydump(logent, logcount);
	}

	if (file_size < offset + size) {
		if (SHOWLOGENTRY(offset, offset + size))
		{
			prt("extend file size from 0x%x (%d) to 0x%x (%d)\n", (int)file_size, (int)file_size, offset+size, offset+size);
		}

		if (file_size < offset) {
			memset(good_buf + file_size, '\0', offset - file_size);
			if (!usehole) {
				off_t ret;
				unsigned iret;

				if (interactive) {
					printf("Hit return when ready...");
					getchar();
				}

				ret = lseek(fd, (off_t)file_size, SEEK_SET);
				if (ret == (off_t)-1) {
					logdump();
					prterr("dowrite: lseek");
					failure(150);
				}
				if (cache_off && (fcntl(fd, F_NOCACHE, 1) != 0)) { // turn data caching off
					logdump();
					prterr("dowrite: fcntl(F_NOCACHE, 1)");
					failure(201);
				}
				iret = write(fd, good_buf + file_size, offset - file_size);
				if (iret != offset - file_size) {
					logdump();
					if (iret == -1)
						prterr("dowrite: write 0s");
					else
					{
						prt("short write: 0x%x bytes instead of 0x%x\n",
							iret, size);
					}
					failure(151);
				}
				if (cache_off && (fcntl(fd, F_NOCACHE, 0) != 0)) {
					logdump();
					prterr("dowrite: fcntl(F_NOCACHE, 0)");
					failure(201);
				}
			}
		}
		file_size = offset + size;
		if (lite) {
			logdump();
			mwarn("WARNING: Lite file size bug in fsx!");
			failure(149);
		}
	}

	if (testcalls <= simulatedopcount)
		return;

	if (interactive) {
		printf("Hit return when ready...");
		getchar();
	}

	if (!ea) {
		ret = lseek(fd, (off_t)offset, SEEK_SET);
		if (ret == (off_t)-1) {
			logdump();
			prterr("dowrite: lseek");
			failure(150);
		}
		if (cache_off && (fcntl(fd, F_NOCACHE, 1) != 0)) { // turn data caching off
			logdump();
			prterr("dowrite: fcntl(F_NOCACHE, 1)");
			failure(201);
		}
		iret = write(fd, good_buf + offset, size);
		if (iret != size) {
			logdump();
			if (iret == -1)
				prterr("dowrite: write");
			else
			{
				prt("short write: 0x%x bytes instead of 0x%x\n",
						iret, size);
			}
			failure(151);
		}
		if (cache_off && (fcntl(fd, F_NOCACHE, 0) != 0)) {
			logdump();
			prterr("dowrite: fcntl(F_NOCACHE, 0)");
			failure(201);
		}
	} else {
		if (random() % 2000 == 0) {
			iret = fremovexattr(fd, eaname, 0);
			if (iret != 0) {
				logdump();
				prterr("ea_dowrite: removexattr");
				failure(151);
			}
		}
		iret = fsetxattr(fd, eaname, good_buf, size, 0, 0);
		ea_lastwrite = size;
		if (iret != 0) {
			logdump();
			prterr("ea_dowrite: setxattr");
			failure(151);
		}
	}
}


void
domapwrite(unsigned offset, unsigned size)
{
	unsigned pg_offset;
	unsigned map_size;
	char    *p;
	int logent;

	offset -= offset % writebdy;
	if (size == 0) {
		if (debug && testcalls > simulatedopcount)
		{
			prt("skipping zero size write\n");
		}
		log4(OP_SKIPPED, OP_MAPWRITE, offset, size);
		return;
	}

	if (file_size < offset + size) {
		if (file_size < offset)
			memset(good_buf + file_size, '\0', offset - file_size);
		if (lite) {
			logdump();
			mwarn("WARNING: Lite file size bug in fsx!");
			failure(200);
		}
		if (SHOWLOGENTRY(offset, offset + size))
		{
			prt("extend file size from 0x%x (%d) to 0x%x (%d)\n", (int)file_size, (int)file_size, offset+size, offset+size);
		}
		dotruncate(offset + size);
		if (closeopen)
			docloseopen();
		if (simulatedopcount > 0 && testcalls == simulatedopcount)
			writefileimage();
		testcalls++;
	}
	gendata(good_buf, offset, size);

	logent = log4(OP_MAPWRITE, offset, size, 0);

	if (SHOWLOGENTRY(offset, offset + size))
	{
		logentrydump(logent, logcount);
	}

	if (testcalls <= simulatedopcount)
		return;

	pg_offset = offset & page_mask;
	map_size  = pg_offset + size;

	if (interactive) {
		printf("Hit return when ready...");
		getchar();
	}

	if ((p = (char *)mmap(0, map_size, PROT_READ | PROT_WRITE,
			      MAP_FILE | MAP_SHARED, fd,
			      (off_t)(offset - pg_offset))) == (char *)-1) {
		logdump();
		prterr("domapwrite: mmap");
		failure(202);
	}
	memcpy(p + pg_offset, good_buf + offset, size);
	if (msync(p, map_size, 0) != 0) {
		logdump();
		prterr("domapwrite: msync");
		failure(203);
	}

	check_eofpage("Write", offset, p, size);

	if (munmap(p, map_size) != 0) {
		logdump();
		prterr("domapwrite: munmap");
		failure(204);
	}
}


void
dotruncate(unsigned size)
{
	off_t oldsize, start, end;
	int logent;

	oldsize = file_size;
	if (oldsize < size) {
		start = oldsize;
		end = size;
	} else {
		start = size;
		end = oldsize;
	}

	size -= size % truncbdy;
	if (size > biggest) {
		biggest = size;
		if (!quiet && testcalls > simulatedopcount)
		{
			prt("truncating to largest ever: 0x%x\n", size);
		}
	}

	logent = log4(OP_TRUNCATE, size, (unsigned)file_size, 0);

	if (size > file_size)
		memset(good_buf + file_size, '\0', size - file_size);
	file_size = size;
	
	if (SHOWLOGENTRY(start, end))
	{
		logentrydump(logent, logcount);
	}

	if (testcalls <= simulatedopcount)
		return;

	if (interactive) {
		printf("Hit return when ready...");
		getchar();
	}

	if (usehole || size <= oldsize) { // shrink the file or we want a sparse file
		if (ftruncate(fd, (off_t)size) == -1) {
			logdump();
			prt("ftruncate1: %x\n", size);
			prterr("dotruncate: ftruncate");
			failure(160);
		}
	} else { // write zeros instead of creating a sparse file to extend the file
		off_t ret;
		unsigned iret;
		ret = lseek(fd, (off_t)oldsize, SEEK_SET);
		if (ret == (off_t)-1) {
			logdump();
			prterr("dowrite: lseek");
			failure(150);
		}
		iret = write(fd, good_buf + oldsize, size - oldsize);
		if (iret != size - oldsize) {
			logdump();
			if (iret == -1)
				prterr("dotruncate: write");
			else
			{
				prt("short write: 0x%x bytes instead of 0x%x\n",
					iret, size);
			}
			failure(151);
		}
	}
}


void
writefileimage(void)
{
	ssize_t iret;

	if (lseek(fd, (off_t)0, SEEK_SET) == (off_t)-1) {
		logdump();
		prterr("writefileimage: lseek");
		failure(171);
	}
	iret = write(fd, good_buf, file_size);
	if ((off_t)iret != file_size) {
		logdump();
		if (iret == -1)
			prterr("writefileimage: write");
		else
		{
			prt("short write: 0x%x bytes instead of 0x%llx\n",
			    iret, (unsigned long long)file_size);
		}
		failure(172);
	}
	if (usehole) {
		if (lite ? 0 : ftruncate(fd, file_size) == -1) {
			logdump();
			prt("ftruncate2: %llx\n", (unsigned long long)file_size);
			prterr("writefileimage: ftruncate");
			failure(173);
		}
	}
}


void
docloseopen(void)
{ 
	if (testcalls <= simulatedopcount)
		return;

	if (debug)
	{
		prt("%lu close/open\n", testcalls);
	}
	if (sync_before_close && fsync(fd)) {
		logdump();
		prterr("docloseopen: fsync");
		failure(182);
	}
	if (close(fd)) {
		logdump();
		prterr("docloseopen: close");
		failure(180);
	}
	fd = open(fname, O_RDWR, 0);
	if (fd < 0) {
		logdump();
		prterr("docloseopen: open");
		failure(181);
	}
}


void
test(void)
{
	unsigned long	offset;
	unsigned long	size = maxoplen;
	unsigned long	rv = random();
	unsigned long	op = rv % (3 + !lite + mapped_writes);

	/* turn off the map read if necessary */

	if (op == 2 && !mapped_reads)
	    op = 0;

	if (simulatedopcount > 0 && testcalls == simulatedopcount)
		writefileimage();

	testcalls++;

	if (closeprob)
		closeopen = (rv >> 3) < (1 << 28) / closeprob;

	if (debugstart > 0 && testcalls >= debugstart)
		debug = 1;

	if (!quiet && testcalls < simulatedopcount && testcalls % 100000 == 0)
	{
		prt("%lu...\n", testcalls);
	}

	/*
	 * READ:	op = 0
	 * WRITE:	op = 1
	 * MAPREAD:     op = 2
	 * TRUNCATE:	op = 3
	 * MAPWRITE:    op = 3 or 4
	 */
	if (lite ? 0 : op == 3 && style == 0 && ea == 0) /* vanilla truncate? */
		dotruncate(((random() + datasize - 1) & ~(datasize - 1)) % maxfilelen);
	else {
		if (randomoplen)
			size = random() % (maxoplen+1);
		size = (size + datasize - 1) & ~(datasize - 1); // round up to multiple of datasize
		if (lite ? 0 : op == 3 && ea == 0)
			dotruncate(size);
		else {
			offset = random();
			offset &= ~(datasize - 1); // trunc to multiple of datasize
			if (op == 1 || op == (lite ? 3 : 4)) {
				offset %= maxfilelen;
				if (ea != 0)
					offset = 0;
				if (offset + size > maxfilelen)
					size = maxfilelen - offset;
				if (op != 1)
					domapwrite(offset, size);
				else
					dowrite(offset, size);
			} else {
				if (ea || !file_size) {
					offset = 0;
				} else {
					offset %= file_size;
					offset &= ~(datasize - 1);
				}
				if (offset + size > file_size) {
					size = file_size - offset;
					size &= ~(datasize - 1); // trunc to multiple of datasize
				}
				if (op != 0)
					domapread(offset, size);
				else
					doread(offset, size);
			}
		}
	}
	if (sizechecks && testcalls > simulatedopcount)
		check_size();
	if (closeopen)
		docloseopen();
}


void
cleanup(int sig)
{
	if (sig)
	{
		prt("signal %d\n", sig);
	}
	prt("testcalls = %lu\n", testcalls);
#ifdef XILOG
	XILogEndTestCase(xilogref, kXILogTestPassOnErrorLevel);
	XILogCloseLog(xilogref);
#endif
	//exit(0);
}


void
usage(void)
{
	Logger::WriteMessage(
	"usage: fsx [-ehinqvxCLMORW] [-b opnum] [-c Prob] [-d duration] [-f forkname] [-l logpath] [-m start:end] [-o oplen] [-p progressinterval] [-r readbdy] [-s style] [-t truncbdy] [-w writebdy] [-D startingop] [-F flen] [-G logsize] [-I opnum] [-N numops] [-P dirpath] [-S seed] [-T datasize] fname [xxxxx]\n\
	-b opnum: beginning operation number (default 1)\n\
	-c P: 1 in P chance of file close+open at each op (default infinity)\n\
	-d duration: number of hours for the tool to run\n\
	-e: tests using an extended attribute rather than a file\n\
        -f forkname: test the named fork of fname\n\
	-g logpath: path for .fsxlog file\n\
	-h: write 0s instead of creating holes (i.e. sparse file)\n\
	-i: interactive mode, hit return before performing the current operation\n\
	-l logpath: path for XILog file\n\
	-m startop:endop: monitor (print debug output) specified byte range (default 0:infinity)\n\
	-n: no verifications of file size\n\
	-o oplen: the upper bound on operation size (default 65536)\n\
	-p progressinterval: debug output at specified operation interval\n\
	-q: quieter operation\n\
	-r readbdy: 4096 would make reads page aligned (default 1)\n\
	-s style: 1 gives smaller truncates (default 0)\n\
	-t truncbdy: 4096 would make truncates page aligned (default 1)\n\
	-v: debug output for all operations\n\
	-w writebdy: 4096 would make writes page aligned (default 1)\n\
	-x: write output in XML (XILOG)\n\
	-y: call fsync before closing the file\n\
	-C mix cached and un-cached read/write ops\n\
	-D startingop: debug output starting at specified operation\n\
	-G logsize: #entries in oplog (default 1024)\n\
	-F flen: the upper bound on file size (default 262144)\n\
	-I: start interactive mode since operation opnum\n\
	-L: fsxLite - no file creations & no file size changes\n\
	-M: slow motion mode, wait 1 second before each op\n\
	-N numops: total # operations to do (default infinity)\n\
	-O: use oplen (see -o flag) for every op (default random)\n\
	-P dirpath: save .fsxlog and .fsxgood files in dirpath (default ./)\n\
	-R: mapped read operations DISabled\n\
	-S seed: for random # generator (default 0, gets timestamp+pid)\n\
	-T datasize: size of atomic data elements written to file [1,2,4] (default 4)\n\
	-W: mapped write operations DISabled\n\
	fname: this filename is REQUIRED (no default)\n\
	xxxxx: will be overwritten with operation #s, viewable in \"ps\"\n");
	
	Assert::IsTrue(false, L"Invalid arguments.", LINE_INFO());
}


int
getnum(char *s, char **e)
{
	int ret = -1;

	*e = (char *) 0;
	ret = strtol(s, e, 0);
	if (*e)
		switch (**e) {
		case 'b':
		case 'B':
			ret *= 512;
			*e = *e + 1;
			break;
		case 'k':
		case 'K':
			ret *= 1024;
			*e = *e + 1;
			break;
		case 'm':
		case 'M':
			ret *= 1024*1024;
			*e = *e + 1;
			break;
		case 'w':
		case 'W':
			ret *= 4;
			*e = *e + 1;
			break;
		}
	return (ret);
}


//int
//main(int argc, char **argv)
//{
//	int	ch;
//	char	*endp, **oargv = argv;
//	char goodfile[MAXPATHLEN]; bzero(goodfile, MAXPATHLEN);
//	char logfile[MAXPATHLEN]; bzero(logfile, MAXPATHLEN);
//	char forkname[MAXPATHLEN]; bzero(forkname, MAXPATHLEN);
//	char* logpath = NULL;
//	eaname = argv[0];
//	int xml = 0;
//
//#if defined(_WIN32)
//    eaname = "fsx.exe"; /* Windows does not look certain characters in EA's */
//#endif
//
//	goodfile[0] = 0;
//	logfile[0] = 0;
//	forkname[0] = 0;
//
//	page_size = getpagesize();
//	page_mask = page_size - 1;
//
//	setvbuf(stdout, (char *)0, _IOLBF, 0); /* line buffered stdout */
//
//	while ((ch = getopt_long(argc, argv, "b:c:d:ef:g:hil:m:no:p:qr:s:t:vw:xCD:F:G:I:LMN:OP:RS:T:W", NULL, NULL))
//	       != EOF)
//		switch (ch) {
//		case 'b':
//			simulatedopcount = getnum(optarg, &endp);
//			if (!quiet)
//				fprintf(stdout, "Will begin at operation %ld\n",
//					simulatedopcount);
//			if (simulatedopcount == 0)
//				usage();
//			simulatedopcount -= 1;
//			break;
//		case 'c':
//			closeprob = getnum(optarg, &endp);
//			if (!quiet)
//				fprintf(stdout, "Chance of close/open is 1 in %d\n", closeprob);
//			if (closeprob <= 0)
//				usage();
//			break;
//		case 'd':
//			duration = parsetime(optarg);
//			if(duration <= 0)
//				duration = 0;
//			printf("Running for %ld seconds\n", duration);
//			break;
//		case 'e':
//			ea = 1;
//			mapped_writes = 0;
//			mapped_reads = 0;
//			sizechecks = 0;
//			closeopen = 0;
//			maxoplen = 3802;
//			maxoplen &= ~(datasize - 1); // round down to multiple of datasize
//			maxfilelen = 3802;
//			maxfilelen &= ~(datasize - 1); // round down to multiple of datasize
//			printf("Writing into extended attribute\n");
//			break;
//		case 'f':
//			if (strlcpy(forkname, optarg, sizeof(forkname)) >= sizeof(forkname))
//				usage();
//			break;
//		case 'g':
//			if (strlcpy(logfile, optarg, sizeof(logfile)) >= sizeof(logfile))
//				usage();
//			if (strlcat(logfile, "/", sizeof(logfile)) >= sizeof(logfile))
//				usage();
//			break;
//		case 'h':
//			usehole = 0;
//			break;
//		case 'i':
//			interactive = 1;
//			break;
//		case 'l':
//			logpath = optarg;
//			break;
//		case 'm':
//			monitorstart = getnum(optarg, &endp);
//			if (monitorstart < 0)
//				usage();
//			if (!endp || *endp++ != ':')
//				usage();
//			monitorend = getnum(endp, &endp);
//			if (monitorend < 0)
//				usage();
//			if (monitorend == 0)
//				monitorend = -1; /* aka infinity */
//			debug = 1;
//		case 'n':
//			sizechecks = 0;
//			break;
//		case 'o':
//			maxoplen = getnum(optarg, &endp);
//			if (maxoplen <= 0)
//				usage();
//			break;
//		case 'p':
//			progressinterval = getnum(optarg, &endp);
//			if (progressinterval < 0)
//				usage();
//			break;
//		case 'q':
//			quiet = 1;
//			break;
//		case 'r':
//			readbdy = getnum(optarg, &endp);
//			if (readbdy <= 0)
//				usage();
//			break;
//		case 's':
//			style = getnum(optarg, &endp);
//			if (style < 0 || style > 1)
//				usage();
//			break;
//		case 't':
//			truncbdy = getnum(optarg, &endp);
//			if (truncbdy <= 0)
//				usage();
//			break;
//		case 'v':
//			debug = 1;
//			break;
//		case 'w':
//			writebdy = getnum(optarg, &endp);
//			if (writebdy <= 0)
//				usage();
//			break;
//		case 'x':
//			xml = 1;
//			break;
//		case 'y':
//			sync_before_close = 1;
//			break;
//		case 'C':
//			gUseRandomNoCache = 1;
//			break;
//		case 'D':
//			debugstart = getnum(optarg, &endp);
//			if (debugstart < 1)
//				usage();
//			break;
//		case 'F':
//			maxfilelen = getnum(optarg, &endp);
//			if (maxfilelen <= 0)
//				usage();
//			break;
//		case 'I':
//			interactiveSince = getnum(optarg, &endp);
//			break;
//		case 'G':
//			logsize = getnum(optarg, &endp);
//			if (logsize < 0)
//				usage();
//			break;
//		case 'L':
//			lite = 1;
//			break;
//		case 'M':
//			slow_motion = 1;
//			break;
//		case 'N':
//			numops = getnum(optarg, &endp);
//			if (numops < 0)
//				usage();
//			break;
//		case 'O':
//			randomoplen = 0;
//			break;
//		case 'P':
//			if (strlcpy(goodfile, optarg, sizeof(goodfile)) >= sizeof(goodfile))
//				usage();
//			if (strlcat(goodfile, "/", sizeof(goodfile)) >= sizeof(goodfile))
//				usage();
//			if (strlcpy(logfile, optarg, sizeof(logfile)) >= sizeof(logfile))
//				usage();
//			if (strlcat(logfile, "/", sizeof(logfile)) >= sizeof(logfile))
//				usage();
//			break;
//		case 'R':
//			mapped_reads = 0;
//			break;
//		case 'S':
//			seed = getnum(optarg, &endp);
//			if (seed < 0)
//				usage();
//			break;
//		case 'T':
//			datasize = getnum(optarg, &endp);
//			if ((datasize != 1) && (datasize != 2) && (datasize != 4))
//				usage();
//			break;
//		case 'W':
//			mapped_writes = 0;
//			if (!quiet)
//				fprintf(stdout, "mapped writes DISABLED\n");
//			break;
//
//		default:
//			usage();
//			/* NOTREACHED */
//		}
//
//	/* for (i = optind; i < argc; i++)
//		fname = argv[i]; */
//
//	fname[0] = 0;
//	argc -= optind;
//	argv += optind;
//	if (argc != 1 && argc != 2)
//		usage();
//	if (strlcpy(fname, argv[0], sizeof(fname)) >= sizeof(fname))
//		usage();
//
//	if (argc == 2) {
//		for (msgbuf = argv[1]; *msgbuf; msgbuf++)
//			*msgbuf = ' ';
//		msgbuflen = msgbuf - argv[1];
//		msgbuf = argv[1];
//	}
//
//	if (fname[0]) {
//		if (!quiet) {
//			printf("Using file %s\n", fname);
//		}
//	} else {
//		usage();
//	}
//
//	modsize = (datasize == 4) ? 0 : (datasize == 2) ? 1<<16 : 1<<8;
//	maxfilelen = (maxfilelen + datasize - 1) & ~(datasize - 1); // round up to multiple of datasize
//	maxoplen = (maxoplen + datasize - 1) & ~(datasize - 1); // round up to multiple of datasize
//
//	signal(SIGHUP,	cleanup);
//	signal(SIGINT,	cleanup);
//	signal(SIGPIPE,	cleanup);
//	signal(SIGALRM,	cleanup);
//	signal(SIGTERM,	cleanup);
//	signal(SIGXCPU,	cleanup);
//	signal(SIGXFSZ,	cleanup);
//	signal(SIGVTALRM,	cleanup);
//	signal(SIGUSR1,	cleanup);
//	signal(SIGUSR2,	cleanup);
//
//	// Open the log for writing
//#ifdef XILOG
//	xilogref = XILogOpenLogExtended(logpath, "fsx", "com.apple.xintegration", NULL, xml, 0, NULL,"ResultOwner","com.apple.fsx",NULL);
//	if(xilogref == NULL) {
//		fprintf(stderr, "Couldn't create log for path: %s\n", logpath);
//		exit(-1);
//	}
//
//	XILogBeginTestCase(xilogref,"Begin Tests", "Read/Write/MapRead/Truncate/MapWrite Tests");
//#endif
//
//	/*
//	 * create goodfile and logfile names from fname before potentially adding
//	 * a fork name to fname
//	 */
//	if (strlcat(goodfile, fname, sizeof(goodfile)) >= sizeof(goodfile))
//		usage();
//	if (strlcat(goodfile, ".fsxgood", sizeof(goodfile)) >= sizeof(goodfile))
//		usage();
//	if (strlcat(logfile, fname, sizeof(logfile)) >= sizeof(logfile))
//		usage();
//	if (strlcat(logfile, ".fsxlog", sizeof(logfile)) >= sizeof(logfile))
//		usage();
//	/*
//	 * add forkname to fname if forkname was supplied
//	 */
//	if (forkname[0] != 0) {
//		/* make sure fname exists if working with a named fork */
//		fd = open(fname, O_RDWR|(lite ? 0 : O_CREAT), 0666);
//		if (fd < 0) {
//			prterr(fname);
//			exit(91);
//		} else {
//			/* don't need it open */
//			close(fd);
//		}
//		if (strlcat(fname, _PATH_FORKSPECIFIER, sizeof(fname)) >= sizeof(fname))
//			usage();
//		if (strlcat(fname, forkname, sizeof(fname)) >= sizeof(fname))
//			usage();
//	}
//
//	oplog = (struct log_entry *) malloc(logsize * sizeof(struct log_entry));
//	if (!oplog) {
//		prt("unable to allocate %d entry log", logsize);
//		exit(99);
//	}
//
//	if (seed == 0)
//		seed = (time(NULL) + getpid()) | 1;
//	initstate(seed, state, 256);
//	setstate(state);
//	fd = open(fname, O_RDWR|(lite ? 0 : O_CREAT|O_TRUNC), 0666);
//	if (fd < 0) {
//		prterr(fname);
//		exit(91);
//	}
//	fsxgoodfd = open(goodfile, O_RDWR|O_CREAT|O_TRUNC, 0666);
//	if (fsxgoodfd < 0) {
//		prterr(goodfile);
//		exit(92);
//	}
//	fsxlogf = fopen(logfile, "w");
//	if (fsxlogf == NULL) {
//		prterr(logfile);
//		exit(93);
//	}
//	if (!quiet) {
//		prt("command line:");
//		while (*oargv)
//			prt(" %s", *oargv++);
//		prt("\n");
//	}
//	prt("Seed set to %d\n", seed);
//	if (lite) {
//		off_t ret;
//		file_size = maxfilelen = lseek(fd, (off_t)0, SEEK_END);
//		if (file_size == (off_t)-1) {
//			prterr(fname);
//			mwarn("WARNING: main: lseek eof");
//			exit(94);
//		}
//		ret = lseek(fd, (off_t)0, SEEK_SET);
//		if (ret == (off_t)-1) {
//			prterr(fname);
//			mwarn("WARNING: main: lseek 0");
//			exit(95);
//		}
//		maxfilelen &= ~(datasize - 1); // round down to multiple of datasize
//	}
//	good_buf = (char *) malloc(maxfilelen);
//	memset(good_buf, '\0', maxfilelen);
//	temp_buf = (char *) malloc(maxoplen);
//	memset(temp_buf, '\0', maxoplen);
//	if (lite) {	/* zero entire existing file */
//		ssize_t written;
//
//		written = write(fd, good_buf, (size_t)maxfilelen);
//		if (written != maxfilelen) {
//			if (written == -1) {
//				prterr(fname);
//				mwarn("WARNING: main: error on write");
//			} else
//				mwarn("WARNING: main: short write, 0x%x bytes instead of 0x%x\n",
//						(unsigned)written, maxfilelen);
//			exit(98);
//		}
//	} else 
//		if (!ea) check_trunc_hack();
//
//#ifdef XILOG
//	XIPing(0);
//#endif /* XILOG */
//
//	if(duration > 0) // time based cycling
//	{
//		time_t currentTime = time(NULL);
//		struct tm* tmPtr = localtime(&currentTime);
//		tmPtr->tm_sec += duration;
//		time_t stopTime = mktime(tmPtr);
//		while(time(NULL) < stopTime) {
//			if (slow_motion)
//				sleep(2);			/* wait for 1 second */
//			test();
//#ifdef XILOG
//			if((testcalls % pinginterval) == 0)
//				XIPing(testcalls);
//#endif
//		}	
//	}
//	else
//	{
//		while (numops == -1 || numops--) {
//			if (interactiveSince == testcalls) {
//				interactive = 1;
//				debug = 1;
//				quiet = 0;
//			}
//			if (slow_motion)
//				sleep(2);			/* wait for 1 second */
//			test();
//#ifdef XILOG
//			if((testcalls % pinginterval) == 0)
//				XIPing(testcalls);
//#endif
//		}
//	}
//
//	if (sync_before_close && fsync(fd)) {
//		logdump();
//		prterr("docloseopen: fsync");
//		failure(182);
//	}
//	if (close(fd)) {
//		logdump();
//		prterr("close");
//		failure(99);
//	}
//	if (!quiet)
//		prt("All operations - %lu - completed A-OK!\n", testcalls);
//
//#ifdef XILOG
//	XILogEndTestCase(xilogref, kXILogTestPassOnErrorLevel);
//	XILogCloseLog(xilogref);
//#endif
//
//	exit(0);
//	return 0;
//}

int fsx_main(int argc, char **argv)
{
	int	ch;
	char	*endp, **oargv = argv;
	char goodfile[MAXPATHLEN]; bzero(goodfile, MAXPATHLEN);
	char logfile[MAXPATHLEN]; bzero(logfile, MAXPATHLEN);
	char forkname[MAXPATHLEN]; bzero(forkname, MAXPATHLEN);
	char* logpath = NULL;
	eaname = argv[0];
	int xml = 0;

#if defined(_WIN32)
	eaname = "fsx.exe"; /* Windows does not look certain characters in EA's */
#endif

	goodfile[0] = 0;
	logfile[0] = 0;
	forkname[0] = 0;

	page_size = getpagesize();
	page_mask = page_size - 1;

	setvbuf(stdout, (char *)0, _IOLBF, 0); /* line buffered stdout */

	while((ch = getopt_long(argc, argv, "b:c:d:ef:g:hil:m:no:p:qr:s:t:vw:xCD:F:G:I:LMN:OP:RS:T:W", NULL, NULL))
		!= EOF)
		switch(ch) {
		case 'b':
			simulatedopcount = getnum(optarg, &endp);
			if(!quiet)
				fprintf(stdout, "Will begin at operation %ld\n",
					simulatedopcount);
			if(simulatedopcount == 0)
				usage();
			simulatedopcount -= 1;
			break;
		case 'c':
			closeprob = getnum(optarg, &endp);
			if(!quiet)
				fprintf(stdout, "Chance of close/open is 1 in %d\n", closeprob);
			if(closeprob <= 0)
				usage();
			break;
		case 'd':
			duration = parsetime(optarg);
			if(duration <= 0)
				duration = 0;
			printf("Running for %ld seconds\n", duration);
			break;
		case 'e':
			ea = 1;
			mapped_writes = 0;
			mapped_reads = 0;
			sizechecks = 0;
			closeopen = 0;
			maxoplen = 3802;
			maxoplen &= ~(datasize - 1); // round down to multiple of datasize
			maxfilelen = 3802;
			maxfilelen &= ~(datasize - 1); // round down to multiple of datasize
			printf("Writing into extended attribute\n");
			break;
		case 'f':
			if(strlcpy(forkname, optarg, sizeof(forkname)) >= sizeof(forkname))
				usage();
			break;
		case 'g':
			if(strlcpy(logfile, optarg, sizeof(logfile)) >= sizeof(logfile))
				usage();
			if(strlcat(logfile, "/", sizeof(logfile)) >= sizeof(logfile))
				usage();
			break;
		case 'h':
			usehole = 0;
			break;
		case 'i':
			interactive = 1;
			break;
		case 'l':
			logpath = optarg;
			break;
		case 'm':
			monitorstart = getnum(optarg, &endp);
			if(monitorstart < 0)
				usage();
			if(!endp || *endp++ != ':')
				usage();
			monitorend = getnum(endp, &endp);
			if(monitorend < 0)
				usage();
			if(monitorend == 0)
				monitorend = -1; /* aka infinity */
			debug = 1;
		case 'n':
			sizechecks = 0;
			break;
		case 'o':
			maxoplen = getnum(optarg, &endp);
			if(maxoplen <= 0)
				usage();
			break;
		case 'p':
			progressinterval = getnum(optarg, &endp);
			if(progressinterval < 0)
				usage();
			break;
		case 'q':
			quiet = 1;
			break;
		case 'r':
			readbdy = getnum(optarg, &endp);
			if(readbdy <= 0)
				usage();
			break;
		case 's':
			style = getnum(optarg, &endp);
			if(style < 0 || style > 1)
				usage();
			break;
		case 't':
			truncbdy = getnum(optarg, &endp);
			if(truncbdy <= 0)
				usage();
			break;
		case 'v':
			debug = 1;
			break;
		case 'w':
			writebdy = getnum(optarg, &endp);
			if(writebdy <= 0)
				usage();
			break;
		case 'x':
			xml = 1;
			break;
		case 'y':
			sync_before_close = 1;
			break;
		case 'C':
			gUseRandomNoCache = 1;
			break;
		case 'D':
			debugstart = getnum(optarg, &endp);
			if(debugstart < 1)
				usage();
			break;
		case 'F':
			maxfilelen = getnum(optarg, &endp);
			if(maxfilelen <= 0)
				usage();
			break;
		case 'I':
			interactiveSince = getnum(optarg, &endp);
			break;
		case 'G':
			logsize = getnum(optarg, &endp);
			if(logsize < 0)
				usage();
			break;
		case 'L':
			lite = 1;
			break;
		case 'M':
			slow_motion = 1;
			break;
		case 'N':
			numops = getnum(optarg, &endp);
			if(numops < 0)
				usage();
			break;
		case 'O':
			randomoplen = 0;
			break;
		case 'P':
			if(strlcpy(goodfile, optarg, sizeof(goodfile)) >= sizeof(goodfile))
				usage();
			if(strlcat(goodfile, "/", sizeof(goodfile)) >= sizeof(goodfile))
				usage();
			if(strlcpy(logfile, optarg, sizeof(logfile)) >= sizeof(logfile))
				usage();
			if(strlcat(logfile, "/", sizeof(logfile)) >= sizeof(logfile))
				usage();
			break;
		case 'R':
			mapped_reads = 0;
			break;
		case 'S':
			seed = getnum(optarg, &endp);
			if(seed < 0)
				usage();
			break;
		case 'T':
			datasize = getnum(optarg, &endp);
			if((datasize != 1) && (datasize != 2) && (datasize != 4))
				usage();
			break;
		case 'W':
			mapped_writes = 0;
			if(!quiet)
				fprintf(stdout, "mapped writes DISABLED\n");
			break;

		default:
			usage();
			/* NOTREACHED */
		}

	/* for (i = optind; i < argc; i++)
	fname = argv[i]; */

	fname[0] = 0;
	argc -= optind;
	argv += optind;
	if(argc != 1 && argc != 2)
		usage();
	if(strlcpy(fname, argv[0], sizeof(fname)) >= sizeof(fname))
		usage();

	if(argc == 2) {
		for(msgbuf = argv[1]; *msgbuf; msgbuf++)
			*msgbuf = ' ';
		msgbuflen = msgbuf - argv[1];
		msgbuf = argv[1];
	}

	if(fname[0]) {
		if(!quiet) {
			printf("Using file %s\n", fname);
		}
	}
	else {
		usage();
	}

	modsize = (datasize == 4) ? 0 : (datasize == 2) ? 1 << 16 : 1 << 8;
	maxfilelen = (maxfilelen + datasize - 1) & ~(datasize - 1); // round up to multiple of datasize
	maxoplen = (maxoplen + datasize - 1) & ~(datasize - 1); // round up to multiple of datasize

	/*signal(SIGHUP, cleanup);
	signal(SIGINT, cleanup);
	signal(SIGPIPE, cleanup);
	signal(SIGALRM, cleanup);
	signal(SIGTERM, cleanup);
	signal(SIGXCPU, cleanup);
	signal(SIGXFSZ, cleanup);
	signal(SIGVTALRM, cleanup);
	signal(SIGUSR1, cleanup);
	signal(SIGUSR2, cleanup);*/

	// Open the log for writing
#ifdef XILOG
	xilogref = XILogOpenLogExtended(logpath, "fsx", "com.apple.xintegration", NULL, xml, 0, NULL, "ResultOwner", "com.apple.fsx", NULL);
	if(xilogref == NULL) {
		fprintf(stderr, "Couldn't create log for path: %s\n", logpath);
		exit(-1);
	}

	XILogBeginTestCase(xilogref, "Begin Tests", "Read/Write/MapRead/Truncate/MapWrite Tests");
#endif

	/*
	* create goodfile and logfile names from fname before potentially adding
	* a fork name to fname
	*/
	if(strlcat(goodfile, fname, sizeof(goodfile)) >= sizeof(goodfile))
		usage();
	if(strlcat(goodfile, ".fsxgood", sizeof(goodfile)) >= sizeof(goodfile))
		usage();
	if(strlcat(logfile, fname, sizeof(logfile)) >= sizeof(logfile))
		usage();
	if(strlcat(logfile, ".fsxlog", sizeof(logfile)) >= sizeof(logfile))
		usage();
	/*
	* add forkname to fname if forkname was supplied
	*/
	if(forkname[0] != 0) {
		/* make sure fname exists if working with a named fork */
		fd = open(fname, O_RDWR | (lite ? 0 : O_CREAT), 0666);
		if(fd < 0) {
			prterr(fname);
			//exit(91);
			Assert::Fail(L"File does not exist.", LINE_INFO());
		}
		else {
			/* don't need it open */
			close(fd);
		}
		if(strlcat(fname, _PATH_FORKSPECIFIER, sizeof(fname)) >= sizeof(fname))
			usage();
		if(strlcat(fname, forkname, sizeof(fname)) >= sizeof(fname))
			usage();
	}

	oplog = (struct log_entry *) malloc(logsize * sizeof(struct log_entry));
	if(!oplog) {
		prt("unable to allocate %d entry log", logsize);
		//exit(99);
		Assert::Fail(L"Unable to allocate entry log.", LINE_INFO());
	}

	if(seed == 0)
		seed = (time(NULL) + getpid()) | 1;
	initstate(seed, state, 256);
	setstate(state);
	
	fd = open(fname, O_RDWR | (lite ? 0 : O_CREAT | O_TRUNC), 0666);
	
	if(fd < 0) {
		prterr(fname);
		//exit(91);
		Assert::Fail(L"Failed to open file.", LINE_INFO());
	}
	
	fsxgoodfd = open(goodfile, O_RDWR | O_CREAT | O_TRUNC, 0666);
	
	if(fsxgoodfd < 0) {

		prterr(goodfile);

		close(fd);

		//exit(92);
		Assert::Fail(L"Failed to open good file.", LINE_INFO());
	}

	fsxlogf = fopen(logfile, "w");

	if(fsxlogf == NULL) {

		prterr(logfile);

		close(fsxgoodfd);
		close(fd);

		//exit(93);
		Assert::Fail(L"Failed to open log file.", LINE_INFO());
	}

	if(!quiet) {
		prt("command line:");
		while(*oargv)
			prt(" %s", *oargv++);
		prt("\n");
	}

	prt("Seed set to %d\n", seed);

	if(lite) {
		
		off_t ret;
		file_size = maxfilelen = lseek(fd, (off_t)0, SEEK_END);

		if(file_size == (off_t)-1) {
			prterr(fname);
			mwarn("WARNING: main: lseek eof");

			fclose(fsxlogf);
			close(fsxgoodfd);
			close(fd);

			//exit(94);
			Assert::Fail(L"Failed to seek to end of file.", LINE_INFO());
		}

		ret = lseek(fd, (off_t)0, SEEK_SET);

		if(ret == (off_t)-1) {

			prterr(fname);
			mwarn("WARNING: main: lseek 0");

			fclose(fsxlogf);
			close(fsxgoodfd);
			close(fd);

			//exit(95);
			Assert::Fail(L"Failed to seek to file location.", LINE_INFO());
		}

		maxfilelen &= ~(datasize - 1); // round down to multiple of datasize
	}
	good_buf = (char *)malloc(maxfilelen);
	memset(good_buf, '\0', maxfilelen);
	temp_buf = (char *)malloc(maxoplen);
	memset(temp_buf, '\0', maxoplen);

	if(lite) {	/* zero entire existing file */
		ssize_t written;

		written = write(fd, good_buf, (size_t)maxfilelen);
		if(written != maxfilelen) {
			if(written == -1) {
				prterr(fname);
				mwarn("WARNING: main: error on write");
			}
			else
				mwarn("WARNING: main: short write, 0x%x bytes instead of 0x%x\n",
				(unsigned)written, maxfilelen);

			fclose(fsxlogf);
			close(fsxgoodfd);
			close(fd);

			//exit(98);
			Assert::Fail(L"Failed to zero file.", LINE_INFO());
		}
	}
	else
		if(!ea) check_trunc_hack();

#ifdef XILOG
	XIPing(0);
#endif /* XILOG */

	if(duration > 0) // time based cycling
	{
		time_t currentTime = time(NULL);
		struct tm* tmPtr = localtime(&currentTime);
		tmPtr->tm_sec += duration;
		time_t stopTime = mktime(tmPtr);
		while(time(NULL) < stopTime) {
			if(slow_motion)
				sleep(2);			/* wait for 1 second */
			test();
#ifdef XILOG
			if((testcalls % pinginterval) == 0)
				XIPing(testcalls);
#endif
		}
	}
	else
	{
		while(numops == -1 || numops--) {
			if(interactiveSince == testcalls) {
				interactive = 1;
				debug = 1;
				quiet = 0;
			}
			if(slow_motion)
				sleep(2);			/* wait for 1 second */
			test();
#ifdef XILOG
			if((testcalls % pinginterval) == 0)
				XIPing(testcalls);
#endif
		}
	}

	if(sync_before_close && fsync(fd)) {
		logdump();
		prterr("docloseopen: fsync");
		failure(182);
	}

	if(close(fd)) {

		logdump();
		prterr("close");

		fclose(fsxlogf);
		close(fsxgoodfd);

		failure(99);
	}

	if(!quiet)
		prt("All operations - %lu - completed A-OK!\n", testcalls);

	fclose(fsxlogf);
	close(fsxgoodfd);

#ifdef XILOG
	XILogEndTestCase(xilogref, kXILogTestPassOnErrorLevel);
	XILogCloseLog(xilogref);
#endif

	return 0;
}

