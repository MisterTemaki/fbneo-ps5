/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * open_memstream (POSIX.1-2008), which the console's libc doesn't have (Mesa prints into one: u_memstream, the
 * debug dumps).
 *
 * The libc has no funopen / fopencookie, so the stream is an ordinary FILE on an anonymous shared-memory object
 * (shm_open(SHM_ANON), as Mesa's own os_create_anonymous_file makes them on FreeBSD; else a named object unlinked at
 * once). When the program flushes or closes the stream, the object's bytes are copied into the caller's buffer:
 * *bufp then points at them, followed by a zero byte, and *sizep is the smaller of the bytes written and the stream's
 * position, as POSIX says. That needs fflush / fclose to come here: ps5rt_fflush / ps5rt_fclose, reached through the
 * link's --wrap=fflush / --wrap=fclose (src/ps5rt_stdio_wrap.c, or a program's own wrappers).
 *
 * The buffer is malloc'd, so the caller frees it after fclose, as usual. It moves only when the stream has grown past
 * what it holds (then to at least twice its size): a flush with nothing new, the fflush(NULL) of another thread among
 * them, leaves *bufp where it was (the second M2 review: every flush reallocated it before).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ps5rt/platform.h"

int __real_fclose(FILE *stream);
int __real_fflush(FILE *stream);

struct memstream {
	FILE *file;
	int fd;
	char *buf;
	size_t cap; /* bytes buf holds, its zero byte counted */
	char **bufp;
	size_t *sizep;
	struct memstream *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct memstream *g_streams;
static _Atomic int g_count;   /* streams open: fflush / fclose skip the lock while there are none */
static _Atomic unsigned g_serial;

static int anonymous_fd(void)
{
	int fd;
#ifdef SHM_ANON
	fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
	if (fd >= 0)
		return fd;
#endif
	char name[64];
	for (int tries = 0; tries < 8; tries++) {
		snprintf(name, sizeof name, "/ps5rt-memstream-%ld-%u", (long)getpid(),
			atomic_fetch_add_explicit(&g_serial, 1, memory_order_relaxed));
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
		if (errno != EEXIST)
			break;
	}
	return -1;
}

/* Copies the object's bytes into the caller's buffer. The stream has just been flushed. */
static int publish(struct memstream *m)
{
	struct stat st;
	off_t pos = ftello(m->file);
	if (fstat(m->fd, &st) != 0)
		return -1;
	off_t len = st.st_size;
	if (pos >= 0 && pos < len)
		len = pos;
	if (len < 0 || (unsigned long long)len >= (unsigned long long)SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	size_t n = (size_t)len;
	char *b = m->buf;
	if (n + 1 > m->cap) {
		size_t cap = m->cap > SIZE_MAX / 2 ? n + 1 : m->cap * 2;
		if (cap < n + 1)
			cap = n + 1;
		b = realloc(m->buf, cap);
		if (!b) {
			errno = ENOMEM;
			return -1;
		}
		m->buf = b;
		m->cap = cap;
	}
	size_t done = 0;
	while (done < n) {
		ssize_t r = pread(m->fd, b + done, n - done, (off_t)done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		done += (size_t)r;
	}
	b[done] = '\0';
	*m->bufp = b;
	*m->sizep = done;
	return done == n ? 0 : -1;
}

FILE *open_memstream(char **bufp, size_t *sizep)
{
	if (!bufp || !sizep) {
		errno = EINVAL;
		return NULL;
	}
	struct memstream *m = calloc(1, sizeof *m);
	if (!m)
		return NULL;
	m->buf = malloc(1);
	if (!m->buf) {
		free(m);
		return NULL;
	}
	m->buf[0] = '\0';
	m->cap = 1;
	m->fd = anonymous_fd();
	if (m->fd < 0) {
		int e = errno;
		free(m->buf);
		free(m);
		errno = e;
		return NULL;
	}
	m->file = fdopen(m->fd, "w");
	if (!m->file) {
		int e = errno;
		close(m->fd);
		free(m->buf);
		free(m);
		errno = e;
		return NULL;
	}
	m->bufp = bufp;
	m->sizep = sizep;
	*bufp = m->buf;
	*sizep = 0;
	pthread_mutex_lock(&g_lock);
	m->next = g_streams;
	g_streams = m;
	atomic_fetch_add(&g_count, 1);
	pthread_mutex_unlock(&g_lock);
	return m->file;
}

int ps5rt_fflush(FILE *stream)
{
	int rc = __real_fflush(stream);
	if (atomic_load(&g_count) == 0)
		return rc;
	pthread_mutex_lock(&g_lock);
	for (struct memstream *m = g_streams; m; m = m->next) {
		if (stream && m->file != stream)
			continue;
		if (publish(m) != 0)
			rc = EOF;
		if (stream)
			break;
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}

int ps5rt_fclose(FILE *stream)
{
	struct memstream *m = NULL;
	if (stream && atomic_load(&g_count) != 0) {
		pthread_mutex_lock(&g_lock);
		for (struct memstream **p = &g_streams; *p; p = &(*p)->next) {
			if ((*p)->file == stream) {
				m = *p;
				*p = m->next;
				atomic_fetch_sub(&g_count, 1);
				break;
			}
		}
		pthread_mutex_unlock(&g_lock);
	}
	if (!m)
		return __real_fclose(stream);
	int rc = __real_fflush(stream);
	if (publish(m) != 0)
		rc = EOF;
	if (__real_fclose(stream) != 0)   /* closes m->fd too */
		rc = EOF;
	free(m);   /* the buffer is the caller's now */
	return rc;
}
