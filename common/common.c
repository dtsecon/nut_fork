/* common.c - common useful functions

   Copyright (C) 2000  Russell Kroll <rkroll@exploits.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "common.h"

#include <ctype.h>
#include <syslog.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>

/* the reason we define UPS_VERSION as a static string, rather than a
	macro, is to make dependency tracking easier (only common.o depends
	on nut_version_macro.h), and also to prevent all sources from
	having to be recompiled each time the version changes (they only
	need to be re-linked). */
#if DMFREINDEXER_MAKECHECK
# define NUT_VERSION_MACRO "custom build"
#else
# include "nut_version.h"
#endif

const char *UPS_VERSION = NUT_VERSION_MACRO;

	int	nut_debug_level = 0;
	int	nut_log_level = 0;
	static	int	upslog_flags = UPSLOG_STDERR;

static void xbit_set(int *val, int flag)
{
	*val |= flag;
}

static void xbit_clear(int *val, int flag)
{
	*val ^= (*val & flag);
}

static int xbit_test(int val, int flag)
{
	return ((val & flag) == flag);
}

/* enable writing upslog_with_errno() and upslogx() type messages to
   the syslog */
void syslogbit_set(void)
{
	xbit_set(&upslog_flags, UPSLOG_SYSLOG);
}

/* get the syslog ready for us */
void open_syslog(const char *progname)
{
	int	opt;

	opt = LOG_PID;

	/* we need this to grab /dev/log before chroot */
#ifdef LOG_NDELAY
	opt |= LOG_NDELAY;
#endif

	openlog(progname, opt, LOG_FACILITY);

	switch (nut_log_level)
	{
#if HAVE_SETLOGMASK && HAVE_DECL_LOG_UPTO
	case 7:
		setlogmask(LOG_UPTO(LOG_EMERG));	/* system is unusable */
		break;
	case 6:
		setlogmask(LOG_UPTO(LOG_ALERT));	/* action must be taken immediately */
		break;
	case 5:
		setlogmask(LOG_UPTO(LOG_CRIT));		/* critical conditions */
		break;
	case 4:
		setlogmask(LOG_UPTO(LOG_ERR));		/* error conditions */
		break;
	case 3:
		setlogmask(LOG_UPTO(LOG_WARNING));	/* warning conditions */
		break;
	case 2:
		setlogmask(LOG_UPTO(LOG_NOTICE));	/* normal but significant condition */
		break;
	case 1:
		setlogmask(LOG_UPTO(LOG_INFO));		/* informational */
		break;
	case 0:
		setlogmask(LOG_UPTO(LOG_DEBUG));	/* debug-level messages */
		break;
	default:
                fatalx(EXIT_FAILURE, "Invalid log level threshold");
#else
	case 0:
		break;
	default:
		upslogx(LOG_INFO, "Changing log level threshold not possible");
		break;
#endif
	}
}

/* close ttys and become a daemon */
void background(void)
{
	int	pid;

	if ((pid = fork()) < 0)
		fatal_with_errno(EXIT_FAILURE, "Unable to enter background");

	xbit_set(&upslog_flags, UPSLOG_SYSLOG);
	xbit_clear(&upslog_flags, UPSLOG_STDERR);

	close(0);
	close(1);
	close(2);

	if (pid != 0)
		_exit(EXIT_SUCCESS);		/* parent */

	/* child */

	/* make fds 0-2 point somewhere defined */
	if (open("/dev/null", O_RDWR) != 0)
		fatal_with_errno(EXIT_FAILURE, "open /dev/null");

	if (dup(0) == -1)
		fatal_with_errno(EXIT_FAILURE, "dup");

	if (dup(0) == -1)
		fatal_with_errno(EXIT_FAILURE, "dup");

#ifdef HAVE_SETSID
	setsid();		/* make a new session to dodge signals */
#endif

	upslogx(LOG_INFO, "Startup successful");
}

/* do this here to keep pwd/grp stuff out of the main files */
struct passwd *get_user_pwent(const char *name)
{
	struct passwd *r;
	errno = 0;
	if ((r = getpwnam(name)))
		return r;

	/* POSIX does not specify that "user not found" is an error, so
	   some implementations of getpwnam() do not set errno when this
	   happens. */
	if (errno == 0)
		fatalx(EXIT_FAILURE, "user %s not found", name);
	else
		fatal_with_errno(EXIT_FAILURE, "getpwnam(%s)", name);

	return NULL;  /* to make the compiler happy */
}

/* change to the user defined in the struct */
void become_user(struct passwd *pw)
{
	/* if we can't switch users, then don't even try */
	if ((geteuid() != 0) && (getuid() != 0))
		return;

	if (getuid() == 0)
		if (seteuid(0))
			fatal_with_errno(EXIT_FAILURE, "getuid gave 0, but seteuid(0) failed");

	if (initgroups(pw->pw_name, pw->pw_gid) == -1)
		fatal_with_errno(EXIT_FAILURE, "initgroups");

	if (setgid(pw->pw_gid) == -1)
		fatal_with_errno(EXIT_FAILURE, "setgid");

	if (setuid(pw->pw_uid) == -1)
		fatal_with_errno(EXIT_FAILURE, "setuid");
}

/* drop down into a directory and throw away pointers to the old path */
void chroot_start(const char *path)
{
	if (chdir(path))
		fatal_with_errno(EXIT_FAILURE, "chdir(%s)", path);

	if (chroot(path))
		fatal_with_errno(EXIT_FAILURE, "chroot(%s)", path);

	if (chdir("/"))
		fatal_with_errno(EXIT_FAILURE, "chdir(/)");

	upsdebugx(1, "chrooted into %s", path);
}

/* drop off a pidfile for this process */
void writepid(const char *name)
{
	char	fn[SMALLBUF];
	FILE	*pidf;
	int	mask;

	/* use full path if present, else build filename in PIDPATH */
	if (*name == '/')
		snprintf(fn, sizeof(fn), "%s", name);
	else
		snprintf(fn, sizeof(fn), "%s/%s.pid", PIDPATH, name);

	mask = umask(022);
	pidf = fopen(fn, "w");

	if (pidf) {
		fprintf(pidf, "%d\n", (int) getpid());
		fclose(pidf);
	} else {
		upslog_with_errno(LOG_NOTICE, "writepid: fopen %s", fn);
	}

	umask(mask);
}

/* open pidfn, get the pid, then send it sig */
int sendsignalfn(const char *pidfn, int sig)
{
	char	buf[SMALLBUF];
	FILE	*pidf;
	int	pid, ret;

	pidf = fopen(pidfn, "r");
	if (!pidf) {
		upslog_with_errno(LOG_NOTICE, "fopen %s", pidfn);
		return -1;
	}

	if (fgets(buf, sizeof(buf), pidf) == NULL) {
		upslogx(LOG_NOTICE, "Failed to read pid from %s", pidfn);
		fclose(pidf);
		return -1;
	}

	pid = strtol(buf, (char **)NULL, 10);

	if (pid < 2) {
		upslogx(LOG_NOTICE, "Ignoring invalid pid number %d", pid);
		fclose(pidf);
		return -1;
	}

	/* see if this is going to work first */
	ret = kill(pid, 0);

	if (ret < 0) {
		perror("kill");
		fclose(pidf);
		return -1;
	}

	/* now actually send it */
	ret = kill(pid, sig);

	if (ret < 0) {
		perror("kill");
		fclose(pidf);
		return -1;
	}

	fclose(pidf);
	return 0;
}

int snprintfcat(char *dst, size_t size, const char *fmt, ...)
{
	va_list ap;
	size_t len = strlen(dst);
	int ret;

	size--;
	assert(len <= size);

	va_start(ap, fmt);
	ret = vsnprintf(dst + len, size - len, fmt, ap);
	va_end(ap);

	dst[size] = '\0';
	return len + ret;
}

/* lazy way to send a signal if the program uses the PIDPATH */
int sendsignal(const char *progname, int sig)
{
	char	fn[SMALLBUF];

	snprintf(fn, sizeof(fn), "%s/%s.pid", PIDPATH, progname);

	return sendsignalfn(fn, sig);
}

const char *xbasename(const char *file)
{
	const char *p = strrchr(file, '/');

	if (p == NULL)
		return file;
	return p + 1;
}

static void vupslog(int priority, const char *fmt, va_list va, int use_strerror)
{
	int	ret;
	char	buf[LARGEBUF];

	ret = vsnprintf(buf, sizeof(buf), fmt, va);

	if ((ret < 0) || (ret >= (int) sizeof(buf)))
		syslog(LOG_WARNING, "vupslog: vsnprintf needed more than %d bytes",
			LARGEBUF);

	if (use_strerror)
		snprintfcat(buf, sizeof(buf), ": %s", strerror(errno));

	if (nut_debug_level > 0) {
		static struct timeval	start = { 0 };
		struct timeval		now;

		gettimeofday(&now, NULL);

		if (start.tv_sec == 0) {
			start = now;
		}

		if (start.tv_usec > now.tv_usec) {
			now.tv_usec += 1000000;
			now.tv_sec -= 1;
		}

		fprintf(stderr, "%4.0f.%06ld\t", difftime(now.tv_sec, start.tv_sec), (long)(now.tv_usec - start.tv_usec));
	}

	if (xbit_test(upslog_flags, UPSLOG_STDERR))
		fprintf(stderr, "%s\n", buf);
	if (xbit_test(upslog_flags, UPSLOG_SYSLOG))
		syslog(priority, "%s", buf);
}

/* Return the default path for the directory containing configuration files */
const char * confpath(void)
{
	const char * path;

	if ((path = getenv("NUT_CONFPATH")) == NULL)
		path = CONFPATH;

	return path;
}

/* Return the default path for the directory containing state files */
const char * dflt_statepath(void)
{
	const char * path;

	if ((path = getenv("NUT_STATEPATH")) == NULL)
		path = STATEPATH;

	return path;
}

/* Return the alternate path for pid files */
const char * altpidpath(void)
{
#ifdef ALTPIDPATH
	return ALTPIDPATH;
#else
	return dflt_statepath();
#endif
}

/* logs the formatted string to any configured logging devices + the output of strerror(errno) */
void upslog_with_errno(int priority, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vupslog(priority, fmt, va, 1);
	va_end(va);
}

/* logs the formatted string to any configured logging devices */
void upslogx(int priority, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vupslog(priority, fmt, va, 0);
	va_end(va);
}

void upsdebug_with_errno(int level, const char *fmt, ...)
{
	va_list va;

	if (nut_debug_level < level)
		return;

/* For debugging output, we want to prepend the debug level so the user can
 * e.g. lower the level (less -D's on command line) to retain just the amount
 * of logging info he needs to see at the moment. Using '-DDDDD' all the time
 * is too brutal and needed high-level overview can be lost. This [D#] prefix
 * can help limit this debug stream quicker, than experimentally picking ;) */
	char fmt2[LARGEBUF];
	if (level > 0) {
		int ret;
		ret = snprintf(fmt2, sizeof(fmt2), "[D%d] %s", level, fmt);
		if ((ret < 0) || (ret >= (int) sizeof(fmt2))) {
			syslog(LOG_WARNING, "upsdebug_with_errno: snprintf needed more than %d bytes",
				LARGEBUF);
		} else {
			fmt = (const char *)fmt2;
		}
	}

	va_start(va, fmt);
	vupslog(LOG_DEBUG, fmt, va, 1);
	va_end(va);
}

void upsdebugx(int level, const char *fmt, ...)
{
	va_list va;

	if (nut_debug_level < level)
		return;

/* See comments above in upsdebug_with_errno() - they apply here too. */
	char fmt2[LARGEBUF];
	if (level > 0) {
		int ret;
		ret = snprintf(fmt2, sizeof(fmt2), "[D%d] %s", level, fmt);
		if ((ret < 0) || (ret >= (int) sizeof(fmt2))) {
			syslog(LOG_WARNING, "upsdebugx: snprintf needed more than %d bytes",
				LARGEBUF);
		} else {
			fmt = (const char *)fmt2;
		}
	}

	va_start(va, fmt);
	vupslog(LOG_DEBUG, fmt, va, 0);
	va_end(va);
}

/* dump message msg and len bytes from buf to upsdebugx(level) in
   hexadecimal. (This function replaces Philippe Marzouk's original
   dump_hex() function) */
void upsdebug_hex(int level, const char *msg, const void *buf, int len)
{
	char line[100];
	int n;	/* number of characters currently in line */
	int i;	/* number of bytes output from buffer */

	n = snprintf(line, sizeof(line), "%s: (%d bytes) =>", msg, len);

	for (i = 0; i < len; i++) {

		if (n > 72) {
			upsdebugx(level, "%s", line);
			line[0] = 0;
		}

		n = snprintfcat(line, sizeof(line), n ? " %02x" : "%02x",
			((unsigned char *)buf)[i]);
	}
	upsdebugx(level, "%s", line);
}

/* taken from www.asciitable.com */
static const char* ascii_symb[] = {
	"NUL",  /*  0x00    */
	"SOH",  /*  0x01    */
	"STX",  /*  0x02    */
	"ETX",  /*  0x03    */
	"EOT",  /*  0x04    */
	"ENQ",  /*  0x05    */
	"ACK",  /*  0x06    */
	"BEL",  /*  0x07    */
	"BS",   /*  0x08    */
	"TAB",  /*  0x09    */
	"LF",   /*  0x0A    */
	"VT",   /*  0x0B    */
	"FF",   /*  0x0C    */
	"CR",   /*  0x0D    */
	"SO",   /*  0x0E    */
	"SI",   /*  0x0F    */
	"DLE",  /*  0x10    */
	"DC1",  /*  0x11    */
	"DC2",  /*  0x12    */
	"DC3",  /*  0x13    */
	"DC4",  /*  0x14    */
	"NAK",  /*  0x15    */
	"SYN",  /*  0x16    */
	"ETB",  /*  0x17    */
	"CAN",  /*  0x18    */
	"EM",   /*  0x19    */
	"SUB",  /*  0x1A    */
	"ESC",  /*  0x1B    */
	"FS",   /*  0x1C    */
	"GS",   /*  0x1D    */
	"RS",   /*  0x1E    */
	"US"    /*  0x1F    */
};

/* dump message msg and len bytes from buf to upsdebugx(level) in ascii. */
void upsdebug_ascii(int level, const char *msg, const void *buf, int len)
{
	char line[256];
	int i;
	unsigned char ch;

	if (nut_debug_level < level)
		return;	/* save cpu cycles */

	snprintf(line, sizeof(line), "%s", msg);

	for (i=0; i<len; ++i) {
		ch = ((unsigned char *)buf)[i];

		if (ch < 0x20)
			snprintfcat(line, sizeof(line), "%3s ", ascii_symb[ch]);
		else if (ch >= 0x80)
			snprintfcat(line, sizeof(line), "%02Xh ", ch);
		else
			snprintfcat(line, sizeof(line), "'%c' ", ch);
	}

	upsdebugx(level, "%s", line);
}

static void vfatal(const char *fmt, va_list va, int use_strerror)
{
	if (xbit_test(upslog_flags, UPSLOG_STDERR_ON_FATAL))
		xbit_set(&upslog_flags, UPSLOG_STDERR);
	if (xbit_test(upslog_flags, UPSLOG_SYSLOG_ON_FATAL))
		xbit_set(&upslog_flags, UPSLOG_SYSLOG);

	vupslog(LOG_ERR, fmt, va, use_strerror);
}

void fatal_with_errno(int status, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vfatal(fmt, va, (errno > 0) ? 1 : 0);
	va_end(va);

	exit(status);
}

void fatalx(int status, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vfatal(fmt, va, 0);
	va_end(va);

	exit(status);
}

static const char *oom_msg = "Out of memory";

void *xmalloc(size_t size)
{
	void *p = malloc(size);

	if (p == NULL)
		fatal_with_errno(EXIT_FAILURE, "%s", oom_msg);
	return p;
}

void *xcalloc(size_t number, size_t size)
{
	void *p = calloc(number, size);

	if (p == NULL)
		fatal_with_errno(EXIT_FAILURE, "%s", oom_msg);
	return p;
}

void *xrealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);

	if (p == NULL)
		fatal_with_errno(EXIT_FAILURE, "%s", oom_msg);
	return p;
}

char *xstrdup(const char *string)
{
	char *p = strdup(string);

	if (p == NULL)
		fatal_with_errno(EXIT_FAILURE, "%s", oom_msg);
	return p;
}

/* Read up to buflen bytes from fd and return the number of bytes
   read. If no data is available within d_sec + d_usec, return 0.
   On error, a value < 0 is returned (errno indicates error). */
int select_read(const int fd, void *buf, const size_t buflen, const long d_sec, const long d_usec)
{
	int		ret;
	fd_set		fds;
	struct timeval	tv;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	tv.tv_sec = d_sec;
	tv.tv_usec = d_usec;

	ret = select(fd + 1, &fds, NULL, NULL, &tv);

	if (ret < 1) {
		return ret;
	}

	return read(fd, buf, buflen);
}

/* Write up to buflen bytes to fd and return the number of bytes
   written. If no data is available within d_sec + d_usec, return 0.
   On error, a value < 0 is returned (errno indicates error). */
int select_write(const int fd, const void *buf, const size_t buflen, const long d_sec, const long d_usec)
{
	int		ret;
	fd_set		fds;
	struct timeval	tv;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	tv.tv_sec = d_sec;
	tv.tv_usec = d_usec;

	ret = select(fd + 1, NULL, &fds, NULL, &tv);

	if (ret < 1) {
		return ret;
	}

	return write(fd, buf, buflen);
}


/* FIXME: would be good to get more from /etc/ld.so.conf[.d] and/or LD_LIBRARY_PATH */
const char * search_paths[] = {
	LIBDIR,
	"/usr"LIBDIR,
	"/usr/lib64",
	"/lib64",
	"/usr/lib",
	"/lib",
	"/usr/local/lib64",
	"/usr/local/lib",
	NULL
};

char * get_libname(const char* base_libname)
{
	DIR *dp;
	struct dirent *dirp;
	int index = 0;
	char *libname_path = NULL;
	char current_test_path[LARGEBUF];
	int base_libname_length = strlen(base_libname);

	for(index = 0 ; (search_paths[index] != NULL) && (libname_path == NULL) ; index++)
	{
		memset(current_test_path, 0, LARGEBUF);

		if ((dp = opendir(search_paths[index])) == NULL)
			continue;

		upsdebugx(2,"Looking for lib %s in directory #%d : %s", base_libname, index, search_paths[index]);
		while ((dirp = readdir(dp)) != NULL)
		{
			upsdebugx(5,"Comparing lib %s with dirpath %s", base_libname, dirp->d_name);
			int compres = strncmp(dirp->d_name, base_libname, base_libname_length);
			if(compres == 0) {
				snprintf(current_test_path, LARGEBUF, "%s/%s", search_paths[index], dirp->d_name);
				libname_path = realpath(current_test_path, NULL);
				upsdebugx(2,"Candidate path for lib %s is %s (realpath %s)", base_libname, current_test_path, (libname_path!=NULL)?libname_path:"NULL");
				if (libname_path != NULL)
					break;
			}
		}
		closedir(dp);
	}

	upsdebugx(1,"Looking for lib %s, found %s",
		base_libname, (libname_path!=NULL)?libname_path:"NULL");
	return libname_path;
}
