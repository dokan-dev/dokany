#include <windows.h>
#include "fuse.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <errno.h>

#ifdef __CYGWIN__
#define _BSD_SOURCE
#include <unistd.h>
#include <semaphore.h>
#endif

enum  {
	KEY_HELP,
	KEY_HELP_NOHEADER,
	KEY_VERSION,
};

struct helper_opts {
	int singlethread;
	int foreground;
	int fsname;
	char *mountpoint;
};

int fuse_session_exit(struct fuse_session *se);

#ifdef _MSC_VER
static char * realpath(const char *file_name, char *resolved_name)
{
	strcpy(resolved_name,file_name);
	return resolved_name;
}
#endif

#define FUSE_HELPER_OPT(t, p) { t, offsetof(struct helper_opts, p), 1 }

static const struct fuse_opt fuse_helper_opts[] = {
		FUSE_HELPER_OPT("-d",          foreground),
		FUSE_HELPER_OPT("debug",       foreground),
		FUSE_HELPER_OPT("-f",          foreground),
		FUSE_HELPER_OPT("-s",          singlethread),
		FUSE_HELPER_OPT("fsname=",     fsname),

		FUSE_OPT_KEY("-h",          KEY_HELP),
		FUSE_OPT_KEY("--help",      KEY_HELP),
		FUSE_OPT_KEY("-ho",         KEY_HELP_NOHEADER),
		FUSE_OPT_KEY("-V",          KEY_VERSION),
		FUSE_OPT_KEY("--version",   KEY_VERSION),
		FUSE_OPT_KEY("-d",          FUSE_OPT_KEY_KEEP),
		FUSE_OPT_KEY("debug",       FUSE_OPT_KEY_KEEP),
		FUSE_OPT_KEY("fsname=",     FUSE_OPT_KEY_KEEP),
		FUSE_OPT_END
};

static void usage(const char *progname)
{
	fprintf(stderr,
		"usage: %s mountpoint [options]\n\n", progname);
	fprintf(stderr,
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"\n");
}

static void helper_help(void)
{
	fprintf(stderr,
		"FUSE options:\n"
		"    -d   -o debug          enable debug output (implies -f)\n"
		"    -f                     foreground operation\n"
		"    -s                     disable multi-threaded operation\n"
		"\n"
		);
}

static void helper_version(void)
{
	fprintf(stderr, "FUSE library version: %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
}

static int fuse_helper_opt_proc(void *data, const char *arg, int key,
								struct fuse_args *outargs)
{
	struct helper_opts *hopts = (struct helper_opts*)data;

	switch (key) {
case KEY_HELP:
	usage(outargs->argv[0]);
	/* fall through */

case KEY_HELP_NOHEADER:
	helper_help();
	return fuse_opt_add_arg(outargs, "-h");

case KEY_VERSION:
	helper_version();
	return 1;

case FUSE_OPT_KEY_NONOPT:
	if (!hopts->mountpoint) {
		char mountpoint[MAX_PATH];
		/* We have to short-circuit realpath, because Cygwin's notion of paths
		   is not compatible with Dokan's. TODO: fix this!
		*/
		/*if (realpath(arg, mountpoint) == NULL) {
			fprintf(stderr, "fuse: bad mount point `%s': %s\n", arg, strerror(errno));
			return -1;
		}*/
		ZeroMemory(mountpoint, sizeof(mountpoint));
		strncpy(mountpoint,arg, sizeof(mountpoint) - 1);
		return fuse_opt_add_opt(&hopts->mountpoint, mountpoint);
	} else {
		fprintf(stderr, "fuse: invalid argument `%s'\n", arg);
		return -1;
	}

default:
	return 1;
	}
}

static int add_default_fsname(const char *progname, struct fuse_args *args)
{
	int res;
	char *fsname_opt;
	const char *basename = strrchr(progname, '/');
	if (basename == NULL)
		basename = progname;
	else if (basename[1] != '\0')
		basename++;

	fsname_opt = (char *) malloc(strlen(basename) + 64);
	if (fsname_opt == NULL) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		return -1;
	}
	sprintf(fsname_opt, "-ofsname=%s", basename);
	res = fuse_opt_add_arg(args, fsname_opt);
	free(fsname_opt);
	return res;
}

int fuse_parse_cmdline(struct fuse_args *args, char **mountpoint,
					   int *multithreaded, int *foreground)
{
	int res;
	struct helper_opts hopts;

	memset(&hopts, 0, sizeof(hopts));
	res = fuse_opt_parse(args, &hopts, fuse_helper_opts, fuse_helper_opt_proc);
	if (res == -1)
		return -1;

	if (!hopts.fsname) {
		res = add_default_fsname(args->argv[0], args);
		if (res == -1)
			goto err;
	}
	if (mountpoint)
		*mountpoint = hopts.mountpoint;
	else
		free(hopts.mountpoint);

	if (multithreaded)
		*multithreaded = !hopts.singlethread;
	if (foreground)
		*foreground = hopts.foreground;
	return 0;

err:
	free(hopts.mountpoint);
	return -1;
}

int fuse_daemonize(int foreground)
{
	if (!foreground) {
#ifdef __CYGWIN__
		int res = daemon(0, 0);
		if (res == -1) {
			perror("fuse: failed to daemonize program\n");
			return -1;
		}
#else
		/** No daemons on Windows but we detach from current console **/
		if (FreeConsole() == 0) {
			DWORD currentError = GetLastError();
			fprintf(stderr, "fuse: daemonize failed = %lu\n", currentError);
			return -1;
		}
#endif
	}
	return 0;
}

int fuse_version(void)
{
	return FUSE_VERSION;
}

#ifdef __CYGWIN__
static struct fuse_session *fuse_instance=NULL;

static void exit_handler(int sig)
{
	(void) sig;
	if (fuse_instance)
		fuse_session_exit(fuse_instance);
}

static int set_one_signal_handler(int sig, void (*handler)(int))
{
	struct sigaction sa;
	struct sigaction old_sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = handler;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;

	if (sigaction(sig, NULL, &old_sa) == -1) 
	{
		perror("fuse: cannot get old signal handler");
		return -1;
	}

	if (old_sa.sa_handler == SIG_DFL && sigaction(sig, &sa, NULL) == -1) 
	{
		perror("fuse: cannot set signal handler");
		return -1;
	}
	
	return 0;
}

int fuse_set_signal_handlers(struct fuse_session *se)
{	
	if (set_one_signal_handler(SIGHUP, exit_handler) == -1 ||
		set_one_signal_handler(SIGINT, exit_handler) == -1 ||
		set_one_signal_handler(SIGTERM, exit_handler) == -1 ||
		set_one_signal_handler(SIGPIPE, SIG_IGN) == -1)
		return -1;

	fuse_instance = se;
	return 0;
}

void fuse_remove_signal_handlers(struct fuse_session *se)
{

	if (fuse_instance != se)
		fprintf(stderr,
		"fuse: fuse_remove_signal_handlers: unknown session\n");
	else
		fuse_instance = NULL;

	set_one_signal_handler(SIGHUP, SIG_DFL);
	set_one_signal_handler(SIGINT, SIG_DFL);
	set_one_signal_handler(SIGTERM, SIG_DFL);
	set_one_signal_handler(SIGPIPE, SIG_DFL);
}

int my_sem_init(sem_t *sem, int pshared, int initial)
{
	*sem=(sem_t)CreateSemaphore (NULL, initial, SEM_VALUE_MAX, NULL);
	return *sem==NULL?-1:0;
}

int my_sem_destroy(sem_t *sem)
{
	if (CloseHandle(*sem)>0)
		return 0;
	return -1;
}

int my_sem_post (sem_t * sem)
{
	if (ReleaseSemaphore((HANDLE)*sem, 1, NULL)>0)
		return 0;
	return -1;
}

int my_sem_wait (sem_t * sem)
{
	if (WaitForSingleObject((HANDLE)*sem,INFINITE)==WAIT_OBJECT_0)
		return 0;
	return -1;
}

#else

int fuse_set_signal_handlers(struct fuse_session *se)
{
	return 0;
}

void fuse_remove_signal_handlers(struct fuse_session *se)
{
}

#endif //CYGWIN
