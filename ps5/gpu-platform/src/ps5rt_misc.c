/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * C library functions RADV calls that the console's libc doesn't export, each as a title can have it:
 *
 * - localtime_r: the system's localtime under a lock, copied into the caller's struct tm.
 * - getpwuid_r: a title has no user database: no entry (0, *result null), as POSIX has it for an unknown user. Mesa's
 *   disk cache then needs MESA_SHADER_CACHE_DIR (or XDG_CACHE_HOME / HOME) to find its folder.
 * - openlog / closelog: the system's syslog has no ident to set; they do nothing.
 * - popen / pclose: a title can't start processes: NULL with ENOSYS; pclose, -1 with ECHILD.
 * - dladdr: no symbol information for an address: 0 ("not found"), which every caller in Mesa handles.
 */
#if !defined(__PROSPERO__) && !defined(__FreeBSD__)
#define _GNU_SOURCE /* host tests: glibc's Dl_info */
#endif
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>

static pthread_mutex_t g_time_lock = PTHREAD_MUTEX_INITIALIZER;

struct tm *localtime_r(const time_t *restrict timep, struct tm *restrict result)
{
	if (!timep || !result) {
		errno = EINVAL;
		return NULL;
	}
	pthread_mutex_lock(&g_time_lock);
	struct tm *t = localtime(timep);
	if (t)
		*result = *t;
	pthread_mutex_unlock(&g_time_lock);
	return t ? result : NULL;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
	(void)uid;
	(void)pwd;
	(void)buf;
	(void)buflen;
	*result = NULL;
	return 0;
}

void openlog(const char *ident, int option, int facility)
{
	(void)ident;
	(void)option;
	(void)facility;
}

void closelog(void)
{
}

FILE *popen(const char *command, const char *mode)
{
	(void)command;
	(void)mode;
	errno = ENOSYS;
	return NULL;
}

int pclose(FILE *stream)
{
	(void)stream;
	errno = ECHILD;
	return -1;
}

int dladdr(const void *restrict addr, Dl_info *restrict info)
{
	(void)addr;
	(void)info;
	return 0;
}
