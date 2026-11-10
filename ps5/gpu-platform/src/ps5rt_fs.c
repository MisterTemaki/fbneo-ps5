/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * File-system calls a title's libkernel lacks (the stub libkernel.so has none of them; only libkernel_sys, which a
 * title may not load, has fstatat and readlink), each on the calls it does have:
 *
 * - fstatat: a path from the current directory (AT_FDCWD) or an absolute one is stat / lstat (AT_SYMLINK_NOFOLLOW).
 *   A path relative to another directory's descriptor can't be resolved here (no openat, no path of a descriptor):
 *   ENOTSUP.
 * - dirfd: the system's DIR is opaque and its descriptor can't be read: -1 with ENOTSUP, which POSIX allows ("the
 *   implementation does not support the association of a file descriptor with a directory"). Mesa's disk cache, the
 *   one caller, then skips the files it would have stat'ed (fstatat on -1 fails).
 * - readlink: lstat says whether the path is a symbolic link. When it isn't, EINVAL (POSIX); when it is, the link
 *   can't be read: ENOSYS.
 * - posix_fallocate: the file is made at least offset + len bytes long with ftruncate when it is shorter (the space
 *   isn't reserved ahead, which posix_fallocate promises). Returns the error number, as POSIX says, and leaves errno.
 *
 * What libc++'s std::filesystem calls (glslang's messages use std::filesystem::absolute, which brings its whole object
 * in), on the same terms: openat, unlinkat (AT_REMOVEDIR: rmdir), fchmodat and utimensat (on utimes: UTIME_NOW,
 * UTIME_OMIT kept by reading the file's times first) for a path from AT_FDCWD or an absolute one, else ENOTSUP;
 * fchmodat with AT_SYMLINK_NOFOLLOW is ENOTSUP (no lchmod). pathconf: the limits of the SDK's <limits.h> (NAME_MAX,
 * PATH_MAX, LINK_MAX, PIPE_BUF), EINVAL for another name. link and symlink: a title's libkernel can't make links:
 * ENOSYS. isatty: a title has no terminal: 0, with ENOTTY (EBADF for a closed descriptor).
 */
#if !defined(__PROSPERO__) && !defined(__FreeBSD__)
/* host tests: glibc declares dirfd's argument nonnull, which would fold the null check away */
#define dirfd ps5rt_glibc_dirfd
#include <dirent.h>
#undef dirfd
#else
#include <dirent.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef LINK_MAX
#define LINK_MAX 32767 /* glibc (host tests) leaves it to the file system; the SDK's <sys/syslimits.h> has it */
#endif

int fstatat(int fd, const char *path, struct stat *st, int flag)
{
	if (!path || !st) {
		errno = EFAULT;
		return -1;
	}
	if (flag & ~AT_SYMLINK_NOFOLLOW) {
		errno = EINVAL;
		return -1;
	}
	if (path[0] == '/' || fd == AT_FDCWD)
		return (flag & AT_SYMLINK_NOFOLLOW) ? lstat(path, st) : stat(path, st);
	errno = fd < 0 ? EBADF : ENOTSUP;
	return -1;
}

int dirfd(DIR *dir)
{
	errno = dir ? ENOTSUP : EINVAL;
	return -1;
}

ssize_t readlink(const char *restrict path, char *restrict buf, size_t size)
{
	(void)buf;
	(void)size;
	struct stat st;
	if (!path) {
		errno = EFAULT;
		return -1;
	}
	if (lstat(path, &st) != 0)
		return -1;
	errno = S_ISLNK(st.st_mode) ? ENOSYS : EINVAL;
	return -1;
}

int posix_fallocate(int fd, off_t offset, off_t len)
{
	int saved = errno;
	int rc = 0;
	struct stat st;
	if (offset < 0 || len <= 0)
		rc = EINVAL;
	else if (offset > INT64_MAX - len)
		rc = EFBIG;
	else if (fstat(fd, &st) != 0)
		rc = errno;
	else if (!S_ISREG(st.st_mode))
		rc = ENODEV;
	else if (st.st_size < offset + len && ftruncate(fd, offset + len) != 0)
		rc = errno;
	errno = saved;
	return rc;
}

/* a path these can take: absolute, or from the current directory */
static int plain_path(int fd, const char *path)
{
	if (!path) {
		errno = EFAULT;
		return 0;
	}
	if (path[0] == '/' || fd == AT_FDCWD)
		return 1;
	errno = fd < 0 ? EBADF : ENOTSUP;
	return 0;
}

int openat(int fd, const char *path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	if (!plain_path(fd, path))
		return -1;
	return open(path, flags, mode);
}

int unlinkat(int fd, const char *path, int flag)
{
	if (flag & ~AT_REMOVEDIR) {
		errno = EINVAL;
		return -1;
	}
	if (!plain_path(fd, path))
		return -1;
	return (flag & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int fchmodat(int fd, const char *path, mode_t mode, int flag)
{
	if (flag & ~AT_SYMLINK_NOFOLLOW) {
		errno = EINVAL;
		return -1;
	}
	if (!plain_path(fd, path))
		return -1;
	if (flag & AT_SYMLINK_NOFOLLOW) {
		errno = ENOTSUP;
		return -1;
	}
	return chmod(path, mode);
}

int utimensat(int fd, const char *path, const struct timespec times[2], int flag)
{
	if (flag & ~AT_SYMLINK_NOFOLLOW) {
		errno = EINVAL;
		return -1;
	}
	if (!plain_path(fd, path))
		return -1;
	if (flag & AT_SYMLINK_NOFOLLOW) {
		errno = ENOTSUP;
		return -1;
	}
	if (!times)
		return utimes(path, NULL);
	struct timeval tv[2], now;
	struct stat st;
	int need_stat = times[0].tv_nsec == UTIME_OMIT || times[1].tv_nsec == UTIME_OMIT;
	if (need_stat && stat(path, &st) != 0)
		return -1;
	gettimeofday(&now, NULL);
	for (int i = 0; i < 2; i++) {
		if (times[i].tv_nsec == UTIME_NOW) {
			tv[i] = now;
		} else if (times[i].tv_nsec == UTIME_OMIT) {
			const struct timespec *t = i == 0 ? &st.st_atim : &st.st_mtim;
			tv[i].tv_sec = t->tv_sec;
			tv[i].tv_usec = t->tv_nsec / 1000;
		} else if (times[i].tv_nsec < 0 || times[i].tv_nsec >= 1000000000L) {
			errno = EINVAL;
			return -1;
		} else {
			tv[i].tv_sec = times[i].tv_sec;
			tv[i].tv_usec = times[i].tv_nsec / 1000;
		}
	}
	return utimes(path, tv);
}

long pathconf(const char *path, int name)
{
	struct stat st;
	if (!path) {
		errno = EFAULT;
		return -1;
	}
	if (stat(path, &st) != 0)
		return -1;
	switch (name) {
	case _PC_NAME_MAX: return NAME_MAX;
	case _PC_PATH_MAX: return PATH_MAX;
	case _PC_LINK_MAX: return LINK_MAX;
	case _PC_PIPE_BUF: return PIPE_BUF;
	default: errno = EINVAL; return -1;
	}
}

int link(const char *existing, const char *path)
{
	(void)existing;
	(void)path;
	errno = ENOSYS;
	return -1;
}

int symlink(const char *target, const char *path)
{
	(void)target;
	(void)path;
	errno = ENOSYS;
	return -1;
}

int isatty(int fd)
{
	struct stat st;
	errno = fstat(fd, &st) != 0 ? EBADF : ENOTTY;
	return 0;
}
