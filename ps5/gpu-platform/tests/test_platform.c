// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// Host tests of the platform layer (platform/tests/host_test.sh): its real sources built for Linux, their functions
// renamed t_<name> (glibc has its own), under AddressSanitizer. Linked with --wrap=pthread_create, --wrap=fclose and
// --wrap=fflush, as a RADV title is.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <nl_types.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "../include/ps5rt/platform.h"
#include "../src/ps5rt_host_ctype.h"

// ---- the functions under test, renamed ----
typedef struct {
	const char *dli_fname;
	void *dli_fbase;
	const char *dli_sname;
	void *dli_saddr;
} t_Dl_info;
void t_qsort_r(void *, size_t, size_t, void *, int (*)(void *, const void *, const void *));
struct tm *t_localtime_r(const time_t *, struct tm *);
int t_getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);
void t_openlog(const char *, int, int);
void t_closelog(void);
FILE *t_popen(const char *, const char *);
int t_pclose(FILE *);
int t_dladdr(const void *, t_Dl_info *);
int t_fstatat(int, const char *, struct stat *, int);
int t_dirfd(DIR *);
ssize_t t_readlink(const char *, char *, size_t);
int t_posix_fallocate(int, off_t, off_t);
int t_openat(int, const char *, int, ...);
int t_unlinkat(int, const char *, int);
int t_fchmodat(int, const char *, mode_t, int);
int t_utimensat(int, const char *, const struct timespec[2], int);
long t_pathconf(const char *, int);
int t_link(const char *, const char *);
int t_symlink(const char *, const char *);
int t_isatty(int);
FILE *t_open_memstream(char **, size_t *);
locale_t t_newlocale(int, const char *, locale_t);
locale_t t_duplocale(locale_t);
int t_freelocale(locale_t);
_RuneLocale *t___runes_for_locale(locale_t, int *);
extern const _RuneLocale t__DefaultRuneLocale;
unsigned long t____runetype_l(int, locale_t);
int t____tolower_l(int, locale_t);
int t____toupper_l(int, locale_t);
int t_iswctype_l(wint_t, wctype_t, locale_t);
int t____mb_cur_max_l(locale_t);
size_t t_mbrtowc_l(wchar_t *, const char *, size_t, mbstate_t *, locale_t);
size_t t_mbrlen_l(const char *, size_t, mbstate_t *, locale_t);
int t_mbtowc_l(wchar_t *, const char *, size_t, locale_t);
size_t t_mbsnrtowcs_l(wchar_t *, const char **, size_t, size_t, mbstate_t *, locale_t);
size_t t_mbsrtowcs_l(wchar_t *, const char **, size_t, mbstate_t *, locale_t);
size_t t_wcrtomb_l(char *, wchar_t, mbstate_t *, locale_t);
size_t t_wcsnrtombs_l(char *, const wchar_t **, size_t, size_t, mbstate_t *, locale_t);
wint_t t_btowc_l(int, locale_t);
int t_wctob_l(wint_t, locale_t);
int t_strcoll_l(const char *, const char *, locale_t);
size_t t_strxfrm_l(char *, const char *, size_t, locale_t);
int t_wcscoll_l(const wchar_t *, const wchar_t *, locale_t);
size_t t_wcsxfrm_l(wchar_t *, const wchar_t *, size_t, locale_t);
double t_strtod_l(const char *, char **, locale_t);
float t_strtof_l(const char *, char **, locale_t);
long double t_strtold_l(const char *, char **, locale_t);
long long t_strtoll_l(const char *, char **, int, locale_t);
unsigned long long t_strtoull_l(const char *, char **, int, locale_t);
size_t t_strftime_l(char *, size_t, const char *, const struct tm *, locale_t);
int t_snprintf_l(char *, size_t, locale_t, const char *, ...);
int t_asprintf_l(char **, locale_t, const char *, ...);
int t_sscanf_l(const char *, locale_t, const char *, ...);
struct lconv *t_localeconv_l(locale_t);
struct lconv *t_localeconv(void);
nl_catd t_catopen(const char *, int);
char *t_catgets(nl_catd, int, int, const char *);
int t_catclose(nl_catd);

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                              \
	do {                                                                                                              \
		if (cond)                                                                                                     \
			g_pass++;                                                                                                 \
		else {                                                                                                        \
			g_fail++;                                                                                                 \
			printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                    \
			printf(__VA_ARGS__);                                                                                      \
			printf("\n");                                                                                             \
		}                                                                                                             \
	} while (0)

static char g_dir[256];

static void path_in(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/%s", g_dir, name);
}

// ---- qsort_r (BSD argument order) ----
struct order {
	int calls;
	int descending;
	size_t key_size;
};

static int cmp_r(void *thunk, const void *a, const void *b)
{
	struct order *o = thunk;
	o->calls++;
	int r = memcmp(a, b, o->key_size);
	return o->descending ? -r : r;
}

static int cmp_plain(const void *a, const void *b, void *thunk)
{
	return cmp_r(thunk, a, b);
}

static void test_qsort_r(void)
{
	unsigned seed = 12345;
	const size_t sizes[] = {1, 3, 8, 12, 40};
	const size_t counts[] = {0, 1, 2, 5, 12, 13, 100, 1000, 4097};
	for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
		for (size_t ci = 0; ci < sizeof counts / sizeof *counts; ci++) {
			for (int desc = 0; desc < 2; desc++) {
				size_t sz = sizes[si], n = counts[ci];
				unsigned char *a = malloc(n * sz + 1), *b = malloc(n * sz + 1);
				for (size_t i = 0; i < n * sz; i++) {
					seed = seed * 1103515245u + 12345u;
					a[i] = (unsigned char)(seed >> 16) % (ci % 2 ? 4 : 256);   // many equal keys too
				}
				memcpy(b, a, n * sz);
				struct order o1 = {0, desc, sz}, o2 = {0, desc, sz};
				t_qsort_r(a, n, sz, &o1, cmp_r);
				qsort_r(b, n, sz, cmp_plain, &o2);   // glibc's, for the expected order
				CHECK(memcmp(a, b, n * sz) == 0, "size %zu count %zu desc %d", sz, n, desc);
				CHECK(n < 2 || o1.calls > 0, "the thunk reaches the comparison");
				free(a);
				free(b);
			}
		}
	}
	int one = 7;
	struct order o = {0, 0, sizeof(int)};
	t_qsort_r(&one, 1, sizeof one, &o, cmp_r);
	CHECK(o.calls == 0 && one == 7, "one element: no comparison");
}

// ---- localtime_r, getpwuid_r, syslog, popen, dladdr ----
static void test_misc(void)
{
	time_t now = time(NULL);
	struct tm mine, *ref = localtime(&now);
	struct tm expect = *ref;
	CHECK(t_localtime_r(&now, &mine) == &mine, "returns the caller's struct");
	CHECK(mine.tm_year == expect.tm_year && mine.tm_yday == expect.tm_yday && mine.tm_hour == expect.tm_hour &&
			  mine.tm_min == expect.tm_min,
		"same time as localtime");
	errno = 0;
	CHECK(t_localtime_r(NULL, &mine) == NULL && errno == EINVAL, "null time");

	struct passwd pw, *res = (struct passwd *)1;
	char buf[256];
	CHECK(t_getpwuid_r(getuid(), &pw, buf, sizeof buf, &res) == 0 && res == NULL, "no user entry, no error");

	t_openlog("ps5rt-test", 0, 0);
	t_closelog();
	CHECK(1, "openlog / closelog");

	errno = 0;
	CHECK(t_popen("true", "r") == NULL && errno == ENOSYS, "popen: no processes");
	errno = 0;
	CHECK(t_pclose(stdin) == -1 && errno == ECHILD, "pclose");

	t_Dl_info info;
	CHECK(t_dladdr((void *)test_misc, &info) == 0, "dladdr: no information");
}

// ---- fstatat, dirfd, readlink, posix_fallocate ----
static void test_fs(void)
{
	char file[300], link_path[300], missing[300];
	path_in(file, sizeof file, "file");
	path_in(link_path, sizeof link_path, "link");
	path_in(missing, sizeof missing, "missing");
	FILE *f = fopen(file, "w");
	fputs("hello", f);
	fclose(f);
	CHECK(symlink(file, link_path) == 0, "symlink for the test");

	struct stat st, ref;
	stat(file, &ref);
	CHECK(t_fstatat(AT_FDCWD, file, &st, 0) == 0 && st.st_ino == ref.st_ino && st.st_size == 5, "absolute path");
	CHECK(t_fstatat(12345, file, &st, 0) == 0 && st.st_ino == ref.st_ino, "absolute path, any descriptor");
	CHECK(t_fstatat(AT_FDCWD, link_path, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode), "NOFOLLOW: the link");
	CHECK(t_fstatat(AT_FDCWD, link_path, &st, 0) == 0 && S_ISREG(st.st_mode), "follows the link");
	char cwd[300];
	if (getcwd(cwd, sizeof cwd) && chdir(g_dir) == 0) {
		CHECK(t_fstatat(AT_FDCWD, "file", &st, 0) == 0 && st.st_ino == ref.st_ino, "relative to the current folder");
		CHECK(chdir(cwd) == 0, "back");
	}
	int dfd = open(g_dir, O_RDONLY | O_DIRECTORY);
	errno = 0;
	CHECK(t_fstatat(dfd, "file", &st, 0) == -1 && errno == ENOTSUP, "relative to a folder descriptor: ENOTSUP");
	close(dfd);
	errno = 0;
	CHECK(t_fstatat(-5, "file", &st, 0) == -1 && errno == EBADF, "bad descriptor");
	errno = 0;
	CHECK(t_fstatat(AT_FDCWD, file, &st, 0x4000) == -1 && errno == EINVAL, "unknown flag");
	errno = 0;
	CHECK(t_fstatat(AT_FDCWD, missing, &st, 0) == -1 && errno == ENOENT, "missing file");

	DIR *d = opendir(g_dir);
	errno = 0;
	CHECK(t_dirfd(d) == -1 && errno == ENOTSUP, "dirfd: ENOTSUP");
	closedir(d);
	errno = 0;
	CHECK(t_dirfd(NULL) == -1 && errno == EINVAL, "dirfd(NULL)");

	char lb[64];
	errno = 0;
	CHECK(t_readlink(file, lb, sizeof lb) == -1 && errno == EINVAL, "not a link: EINVAL");
	errno = 0;
	CHECK(t_readlink(link_path, lb, sizeof lb) == -1 && errno == ENOSYS, "a link: ENOSYS");
	errno = 0;
	CHECK(t_readlink(missing, lb, sizeof lb) == -1 && errno == ENOENT, "missing: ENOENT");

	int fd = open(file, O_RDWR);
	errno = 1234;
	CHECK(t_posix_fallocate(fd, 0, 100) == 0 && errno == 1234, "grows, errno kept");
	fstat(fd, &st);
	CHECK(st.st_size == 100, "size 100: %lld", (long long)st.st_size);
	CHECK(t_posix_fallocate(fd, 10, 20) == 0, "inside the file");
	fstat(fd, &st);
	CHECK(st.st_size == 100, "never shrinks: %lld", (long long)st.st_size);
	char head[6] = {0};
	CHECK(pread(fd, head, 5, 0) == 5, "read back");
	CHECK(strcmp(head, "hello") == 0, "content kept");
	CHECK(t_posix_fallocate(fd, -1, 10) == EINVAL, "negative offset");
	CHECK(t_posix_fallocate(fd, 0, 0) == EINVAL, "zero length");
	CHECK(t_posix_fallocate(fd, INT64_MAX - 5, 10) == EFBIG, "overflow");
	close(fd);
	CHECK(t_posix_fallocate(fd, 0, 10) == EBADF, "closed descriptor");
	int p[2];
	CHECK(pipe(p) == 0, "pipe");
	CHECK(t_posix_fallocate(p[1], 0, 10) == ENODEV, "a pipe");
	close(p[0]);
	close(p[1]);
	unlink(link_path);
	unlink(file);
}

// ---- what libc++'s std::filesystem calls ----
static void test_fs_at(void)
{
	char file[300], dir[300], other[300];
	path_in(file, sizeof file, "at-file");
	path_in(dir, sizeof dir, "at-dir");
	path_in(other, sizeof other, "at-other");
	int fd = t_openat(AT_FDCWD, file, O_CREAT | O_WRONLY | O_TRUNC, 0640);
	CHECK(fd >= 0, "openat(AT_FDCWD, O_CREAT)");
	struct stat st;
	CHECK(fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 0777) == (0640 & ~umask(022)), "created with its mode");
	if (fd >= 0)
		close(fd);
	umask(022);
	int dfd = open(g_dir, O_RDONLY | O_DIRECTORY);
	errno = 0;
	CHECK(t_openat(dfd, "at-file", O_RDONLY) == -1 && errno == ENOTSUP, "openat relative to a folder: ENOTSUP");
	CHECK(t_fchmodat(AT_FDCWD, file, 0604, 0) == 0 && stat(file, &st) == 0 && (st.st_mode & 0777) == 0604, "fchmodat");
	errno = 0;
	CHECK(t_fchmodat(AT_FDCWD, file, 0600, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOTSUP, "fchmodat NOFOLLOW: ENOTSUP");
	struct timespec ts[2] = {{1000000000, 500000000}, {1200000000, 250000000}};
	CHECK(t_utimensat(AT_FDCWD, file, ts, 0) == 0 && stat(file, &st) == 0 && st.st_atim.tv_sec == 1000000000 &&
			  st.st_mtim.tv_sec == 1200000000 && st.st_mtim.tv_nsec / 1000 == 250000,
		"utimensat sets both times (to the microsecond)");
	struct timespec omit_now[2] = {{0, UTIME_OMIT}, {0, UTIME_NOW}};
	time_t before = time(NULL);
	CHECK(t_utimensat(AT_FDCWD, file, omit_now, 0) == 0 && stat(file, &st) == 0 && st.st_atim.tv_sec == 1000000000 &&
			  st.st_mtim.tv_sec >= before,
		"UTIME_OMIT keeps the access time, UTIME_NOW sets the modification time to now");
	struct timespec bad[2] = {{0, 1000000000}, {0, 0}};
	errno = 0;
	CHECK(t_utimensat(AT_FDCWD, file, bad, 0) == -1 && errno == EINVAL, "a nanosecond count out of range: EINVAL");
	CHECK(t_utimensat(AT_FDCWD, file, NULL, 0) == 0, "utimensat(NULL): now");
	errno = 0;
	CHECK(t_utimensat(dfd, "at-file", NULL, 0) == -1 && errno == ENOTSUP, "utimensat relative to a folder: ENOTSUP");
	CHECK(t_pathconf(file, _PC_NAME_MAX) == NAME_MAX && t_pathconf(file, _PC_PATH_MAX) == PATH_MAX, "pathconf");
	errno = 0;
	CHECK(t_pathconf(file, 12345) == -1 && errno == EINVAL, "pathconf of an unknown name: EINVAL");
	errno = 0;
	CHECK(t_pathconf(other, _PC_NAME_MAX) == -1 && errno == ENOENT, "pathconf of a missing path: ENOENT");
	errno = 0;
	CHECK(t_link(file, other) == -1 && errno == ENOSYS && access(other, F_OK) != 0, "link: ENOSYS, nothing made");
	errno = 0;
	CHECK(t_symlink(file, other) == -1 && errno == ENOSYS && access(other, F_OK) != 0, "symlink: ENOSYS, nothing made");
	errno = 0;
	CHECK(t_isatty(0) == 0 && (errno == ENOTTY || errno == EBADF), "isatty: no terminal");
	errno = 0;
	CHECK(t_isatty(9999) == 0 && errno == EBADF, "isatty of a closed descriptor: EBADF");
	CHECK(mkdir(dir, 0755) == 0, "(a folder)");
	errno = 0;
	CHECK(t_unlinkat(AT_FDCWD, dir, 0) == -1, "unlinkat of a folder without AT_REMOVEDIR fails");
	CHECK(t_unlinkat(AT_FDCWD, dir, AT_REMOVEDIR) == 0 && access(dir, F_OK) != 0, "unlinkat(AT_REMOVEDIR): rmdir");
	CHECK(t_unlinkat(AT_FDCWD, file, 0) == 0 && access(file, F_OK) != 0, "unlinkat: unlink");
	errno = 0;
	CHECK(t_unlinkat(AT_FDCWD, file, 0x4000) == -1 && errno == EINVAL, "unlinkat with an unknown flag: EINVAL");
	close(dfd);
}

// ---- open_memstream ----
// realloc counted (host_test.sh links with --wrap=realloc): a flush with nothing new must not call it
static _Atomic int g_reallocs;
void *__real_realloc(void *p, size_t n);
void *__wrap_realloc(void *p, size_t n)
{
	g_reallocs++;
	return __real_realloc(p, n);
}

static void *flush_all(void *arg)
{
	(void)arg;
	fflush(NULL);
	return NULL;
}

static void test_memstream(void)
{
	char *buf = (char *)1;
	size_t size = 99;
	FILE *f = t_open_memstream(&buf, &size);
	CHECK(f != NULL, "opens");
	if (!f)
		return;
	CHECK(buf && buf[0] == '\0' && size == 0, "empty at first");
	fprintf(f, "hello %d", 42);
	CHECK(fflush(f) == 0, "fflush");
	CHECK(size == 8 && strcmp(buf, "hello 42") == 0, "after fflush: %zu '%s'", size, buf);
	fputs(", world", f);
	fflush(NULL);
	CHECK(size == 15 && strcmp(buf, "hello 42, world") == 0, "fflush(NULL) too: %zu '%s'", size, buf);
	// the buffer moves only when the stream has grown past it (the second M2 review): a flush with nothing new, another
	// thread's fflush(NULL) among them, leaves it where it is
	{
		char *before = buf;
		const int reallocs = g_reallocs;
		fflush(f);
		fflush(NULL);
		pthread_t t;
		CHECK(pthread_create(&t, NULL, flush_all, NULL) == 0 && pthread_join(t, NULL) == 0, "(another thread's fflush(NULL))");
		CHECK(buf == before && size == 15 && g_reallocs == reallocs, "nothing new: no realloc, the buffer stays where it was (%d)",
			g_reallocs - reallocs);
		int moved = 0;
		const int reallocs2 = g_reallocs;
		char *last = buf;
		for (int i = 0; i < 2000; i++) {
			fputc('x', f);
			fflush(f);
			moved += buf != last;
			last = buf;
		}
		CHECK(size == 2015 && buf[2014] == 'x' && buf[2015] == '\0', "2000 bytes one at a time: %zu", size);
		CHECK(moved <= 12 && g_reallocs - reallocs2 <= 12, "... the buffer grew by doubling: %d reallocs, %d moves in 2000 flushes",
			g_reallocs - reallocs2, moved);
		fseek(f, 15, SEEK_SET);
		fflush(f);
		CHECK(size == 15 && buf[15] == '\0' && buf == last, "a shorter size: no move, the zero byte at the position");
		fseek(f, 0, SEEK_END);
	}
	fseek(f, 5, SEEK_SET);
	fflush(f);
	CHECK(size == 5 && buf[5] == '\0' && !strncmp(buf, "hello", 5), "size is the position: %zu", size);
	fseek(f, 0, SEEK_END);
	// a large write, across many stdio buffers
	for (int i = 0; i < 100000; i++)
		fputs("0123456789", f);
	CHECK(fclose(f) == 0, "fclose");
	CHECK(size == 2015 + 1000000 && buf[size] == '\0' && !memcmp(buf + 2015, "0123456789", 10) &&
			  !memcmp(buf + size - 10, "0123456789", 10),
		"after fclose: %zu", size);
	free(buf);

	// two at once, closed in the other order
	char *b1, *b2;
	size_t s1, s2;
	FILE *f1 = t_open_memstream(&b1, &s1), *f2 = t_open_memstream(&b2, &s2);
	fputs("one", f1);
	fputs("two!", f2);
	fclose(f2);
	fputs("+", f1);
	fclose(f1);
	CHECK(s1 == 4 && !strcmp(b1, "one+") && s2 == 4 && !strcmp(b2, "two!"), "two streams: '%s' '%s'", b1, b2);
	free(b1);
	free(b2);

	// nothing written
	FILE *e = t_open_memstream(&b1, &s1);
	fclose(e);
	CHECK(s1 == 0 && b1 && b1[0] == '\0', "empty stream");
	free(b1);

	// an ordinary FILE goes through the same fclose / fflush
	char other[300];
	path_in(other, sizeof other, "plain");
	FILE *pf = fopen(other, "w");
	fputs("plain", pf);
	CHECK(fflush(pf) == 0 && fclose(pf) == 0, "ordinary files unaffected");
	struct stat st;
	CHECK(stat(other, &st) == 0 && st.st_size == 5, "written");
	unlink(other);

	errno = 0;
	CHECK(t_open_memstream(NULL, &s1) == NULL && errno == EINVAL, "null buffer pointer");
	errno = 0;
	CHECK(t_open_memstream(&b1, NULL) == NULL && errno == EINVAL, "null size pointer");
}

// ---- pthread_create's stack ----
static void *report_stack(void *arg)
{
	pthread_attr_t a;
	size_t *out = arg;
	*out = 0;
	if (pthread_getattr_np(pthread_self(), &a) == 0) {
		pthread_attr_getstacksize(&a, out);
		pthread_attr_destroy(&a);
	}
	return NULL;
}

static size_t stack_of_new_thread(const pthread_attr_t *attr)
{
	pthread_t t;
	size_t got = 0;
	if (pthread_create(&t, attr, report_stack, &got) != 0)
		return 0;
	pthread_join(t, NULL);
	return got;
}

static void test_thread(void)
{
	CHECK(ps5rt_thread_stack_size() == PS5RT_THREAD_STACK_DEFAULT, "default 2 MiB");
	pthread_attr_t a;
	pthread_attr_init(&a);
	size_t sys_default = 0;
	pthread_attr_getstacksize(&a, &sys_default);
	pthread_attr_destroy(&a);

	// first the default-sized threads: glibc keeps the stacks of finished threads and hands a bigger one out again
	ps5rt_set_thread_stack_size(0);
	size_t s = stack_of_new_thread(NULL);
	CHECK(s >= sys_default && s < (32u << 20), "0: the system's default: %zu (default %zu)", s, sys_default);

	ps5rt_set_thread_stack_size(sys_default / 2);
	s = stack_of_new_thread(NULL);
	CHECK(s >= sys_default && s < (32u << 20), "smaller than the default: the default: %zu", s);

	ps5rt_set_thread_stack_size(32u << 20);
	s = stack_of_new_thread(NULL);
	CHECK(s >= (32u << 20), "no attributes: the platform's stack: %zu", s);

	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 256 << 10);
	s = stack_of_new_thread(&a);
	pthread_attr_destroy(&a);
	CHECK(s >= (256u << 10) && s < (1u << 20), "with attributes: theirs: %zu", s);

	ps5rt_set_thread_stack_size((size_t)1 << 46);   // 64 TiB: refused, then the default
	s = stack_of_new_thread(NULL);
	CHECK(s >= sys_default && s < ((size_t)1 << 40), "a stack the system refuses: the default after all: %zu", s);
	ps5rt_set_thread_stack_size(PS5RT_THREAD_STACK_DEFAULT);
}

// ---- locale ----
static void test_locale(void)
{
	setlocale(LC_ALL, "C");
	locale_t c = t_newlocale(LC_ALL_MASK, "C", NULL);
	CHECK(c != NULL, "C");
	CHECK(t_newlocale(LC_CTYPE_MASK, "POSIX", NULL) == c && t_newlocale(LC_NUMERIC_MASK, "", c) == c, "POSIX, \"\"");
	errno = 0;
	CHECK(t_newlocale(LC_ALL_MASK, "fr_FR.UTF-8", NULL) == NULL && errno == ENOENT, "another locale: ENOENT");
	errno = 0;
	CHECK(t_newlocale(1 << 30, "C", NULL) == NULL && errno == EINVAL, "unknown mask");
	errno = 0;
	CHECK(t_newlocale(LC_ALL_MASK, NULL, NULL) == NULL && errno == EINVAL, "null name");
	CHECK(t_duplocale(c) == c && t_duplocale(LC_GLOBAL_LOCALE) == c, "duplocale");
	CHECK(t_freelocale(c) == 0, "freelocale");
	CHECK(t_freelocale(LC_GLOBAL_LOCALE) == -1, "freelocale of the global locale");

	int limit = 0;
	_RuneLocale *r = t___runes_for_locale(c, &limit);
	CHECK(r && limit == 256, "rune table, limit %d", limit);
	CHECK(r == &t__DefaultRuneLocale, "_DefaultRuneLocale is that table (libc++'s classic_table reads it): nothing from the system");
	CHECK(!memcmp(r->__magic, "RuneMagi", 8), "magic");
	int bad = 0;
	for (int ch = 0; ch < 256; ch++) {
		unsigned long t = r->__runetype[ch];
		int ok = !!(t & _CTYPE_A) == !!isalpha(ch) && !!(t & _CTYPE_D) == !!isdigit(ch) &&
				 !!(t & _CTYPE_X) == !!isxdigit(ch) && !!(t & _CTYPE_S) == !!isspace(ch) &&
				 !!(t & _CTYPE_B) == !!isblank(ch) && !!(t & _CTYPE_C) == !!iscntrl(ch) &&
				 !!(t & _CTYPE_R) == !!isprint(ch) && !!(t & _CTYPE_G) == !!isgraph(ch) &&
				 !!(t & _CTYPE_P) == !!ispunct(ch) && !!(t & _CTYPE_U) == !!isupper(ch) &&
				 !!(t & _CTYPE_L) == !!islower(ch) && r->__maplower[ch] == tolower(ch) &&
				 r->__mapupper[ch] == toupper(ch) && !!(t & _CTYPE_SW1) == !!isprint(ch);
		if (!ok) {
			bad++;
			printf("     class mismatch at %d: 0x%lx\n", ch, t);
		}
	}
	CHECK(bad == 0, "classes and case of 0-255 are the C locale's (glibc's \"C\"): %d differ", bad);
	CHECK((r->__runetype['7'] & 0xFF) == 7 && (r->__runetype['b'] & 0xFF) == 11 && (r->__runetype['F'] & 0xFF) == 15,
		"digit values in the low byte");
	CHECK(t____runetype_l(0x263A, c) == 0 && t____tolower_l(0x100, c) == 0x100 && t____toupper_l(-1, c) == -1,
		"beyond 255: nothing");
	CHECK(t____runetype_l('a', c) == r->__runetype['a'], "___runetype_l inside the table");
	CHECK(t_iswctype_l(L'a', _CTYPE_A, c) && !t_iswctype_l(L'1', _CTYPE_A, c) && t_iswctype_l(L' ', _CTYPE_S, c) &&
			  !t_iswctype_l(0xE9, _CTYPE_A, c) && !t_iswctype_l(0x4E00, _CTYPE_A, c),
		"iswctype_l on _CTYPE_ masks");

	// multibyte: every byte, back and forth
	CHECK(t____mb_cur_max_l(c) == 1, "MB_CUR_MAX 1");
	int rt = 0;
	for (int ch = 1; ch < 256; ch++) {
		char in = (char)ch, out = 0;
		wchar_t w = 0;
		mbstate_t st = {0};
		if (t_mbrtowc_l(&w, &in, 1, &st, c) != 1 || w != ch || t_wcrtomb_l(&out, w, &st, c) != 1 || out != in ||
			t_btowc_l(ch, c) != (wint_t)ch || t_wctob_l((wint_t)ch, c) != ch)
			rt++;
	}
	CHECK(rt == 0, "bytes 1-255 round-trip: %d don't", rt);
	mbstate_t st = {0};
	wchar_t w = 5;
	CHECK(t_mbrtowc_l(&w, "", 1, &st, c) == 0 && w == 0, "the null byte");
	CHECK(t_mbrtowc_l(&w, "a", 0, &st, c) == (size_t)-2, "n == 0");
	CHECK(t_mbrtowc_l(NULL, NULL, 0, &st, c) == 0, "s == NULL");
	CHECK(t_mbrlen_l("xy", 2, &st, c) == 1, "mbrlen_l");
	CHECK(t_mbtowc_l(&w, "Q", 1, c) == 1 && w == L'Q' && t_mbtowc_l(NULL, NULL, 0, c) == 0, "mbtowc_l");
	char ob = 0;
	errno = 0;
	CHECK(t_wcrtomb_l(&ob, 0x100, &st, c) == (size_t)-1 && errno == EILSEQ, "above 255: EILSEQ");
	CHECK(t_wcrtomb_l(NULL, L'a', &st, c) == 1, "wcrtomb_l(NULL)");
	CHECK(t_btowc_l(EOF, c) == WEOF && t_wctob_l(WEOF, c) == EOF && t_wctob_l(0x263A, c) == EOF, "EOF / WEOF");

	const char *src = "hello";
	wchar_t wb[16];
	CHECK(t_mbsrtowcs_l(NULL, &src, 0, &st, c) == 5 && src[0] == 'h', "count only");
	CHECK(t_mbsrtowcs_l(wb, &src, 16, &st, c) == 5 && src == NULL && !wcscmp(wb, L"hello"), "whole string");
	src = "hello";
	CHECK(t_mbsrtowcs_l(wb, &src, 3, &st, c) == 3 && src && !strcmp(src, "lo"), "len 3 stops, src after it");
	src = "hello";
	CHECK(t_mbsnrtowcs_l(wb, &src, 2, 16, &st, c) == 2 && src && !strcmp(src, "llo"), "nms 2");
	src = "hello";
	CHECK(t_mbsnrtowcs_l(NULL, &src, 4, 0, &st, c) == 4, "count with nms");
	const wchar_t *ws = L"wide";
	char nb[16];
	CHECK(t_wcsnrtombs_l(NULL, &ws, SIZE_MAX, 0, &st, c) == 4, "count bytes");
	CHECK(t_wcsnrtombs_l(nb, &ws, SIZE_MAX, 16, &st, c) == 4 && ws == NULL && !strcmp(nb, "wide"), "whole");
	ws = L"wide";
	CHECK(t_wcsnrtombs_l(nb, &ws, 2, 16, &st, c) == 2 && ws && !wcscmp(ws, L"de"), "nwc 2");
	const wchar_t badw[] = {L'o', L'k', 0x263A, 0};
	ws = badw;
	errno = 0;
	CHECK(t_wcsnrtombs_l(nb, &ws, SIZE_MAX, 16, &st, c) == (size_t)-1 && errno == EILSEQ && ws == badw + 2,
		"EILSEQ, src at the bad one");
	ws = badw;
	CHECK(t_wcsnrtombs_l(NULL, &ws, SIZE_MAX, 0, &st, c) == (size_t)-1, "EILSEQ counting");

	// collation
	CHECK(t_strcoll_l("abc", "abd", c) < 0 && t_strcoll_l("b", "a", c) > 0 && t_strcoll_l("Z", "a", c) < 0,
		"strcoll_l: code-point order");
	char xb[8];
	CHECK(t_strxfrm_l(xb, "abc", sizeof xb, c) == 3 && !strcmp(xb, "abc"), "strxfrm_l");
	CHECK(t_strxfrm_l(xb, "abcdefghij", sizeof xb, c) == 10, "strxfrm_l: too long, the length");
	CHECK(t_strxfrm_l(NULL, "abc", 0, c) == 3, "strxfrm_l(NULL, 0)");
	wchar_t wx[8];
	CHECK(t_wcscoll_l(L"a", L"b", c) < 0 && t_wcsxfrm_l(wx, L"ab", 8, c) == 2 && !wcscmp(wx, L"ab"), "wide");

	// numbers, time, formatted I/O
	char *end;
	CHECK(t_strtod_l("1.5e3x", &end, c) == 1500.0 && *end == 'x', "strtod_l");
	CHECK(t_strtof_l("0.25", NULL, c) == 0.25f && t_strtold_l("2.5", NULL, c) == 2.5L, "strtof_l, strtold_l");
	CHECK(t_strtoll_l("-0x7f", NULL, 16, c) == -127 && t_strtoull_l("777", NULL, 8, c) == 511, "strtoll_l / ull");
	struct tm tm = {.tm_year = 126, .tm_mon = 10, .tm_mday = 7, .tm_hour = 1, .tm_wday = 6};
	char tb[64];
	CHECK(t_strftime_l(tb, sizeof tb, "%Y-%m-%d %H %a", &tm, c) == 17 && !strcmp(tb, "2026-11-07 01 Sat"), "%s", tb);
	char pb[64];
	CHECK(t_snprintf_l(pb, sizeof pb, c, "%d %.2f %s", 7, 2.5, "x") == 8 && !strcmp(pb, "7 2.50 x"), "%s", pb);
	CHECK(t_snprintf_l(pb, 4, c, "%s", "truncated") == 9 && !strcmp(pb, "tru"), "snprintf_l truncates");
	char *ab = NULL;
	CHECK(t_asprintf_l(&ab, c, "%s-%d", "a", 12) == 4 && ab && !strcmp(ab, "a-12"), "asprintf_l");
	free(ab);
	int i1 = 0;
	double d1 = 0;
	CHECK(t_sscanf_l("12 3.5", c, "%d %lf", &i1, &d1) == 2 && i1 == 12 && d1 == 3.5, "sscanf_l");

	// conventions
	struct lconv *lc = t_localeconv_l(c);
	CHECK(lc && !strcmp(lc->decimal_point, ".") && !strcmp(lc->thousands_sep, "") && !strcmp(lc->grouping, "") &&
			  !strcmp(lc->currency_symbol, "") && lc->frac_digits == CHAR_MAX && lc->int_n_sign_posn == CHAR_MAX,
		"the C locale's lconv");
	CHECK(t_localeconv() == lc, "localeconv: the same");

	// catalogues
	errno = 0;
	nl_catd cat = t_catopen("anything", NL_CAT_LOCALE);
	CHECK(cat == (nl_catd)-1 && errno == ENOENT, "catopen: none");
	const char *dflt = "default text";
	CHECK(t_catgets(cat, 1, 2, dflt) == dflt, "catgets: the default");
	errno = 0;
	CHECK(t_catclose(cat) == -1 && errno == EBADF, "catclose");
}

int main(int argc, char **argv)
{
	// a scratch folder: inside argv[1] when given (host_test.sh's work folder)
	snprintf(g_dir, sizeof g_dir, "%s/ps5rt-platform-test-XXXXXX", argc > 1 ? argv[1] : "/tmp");
	if (!mkdtemp(g_dir)) {
		perror("mkdtemp");
		return 1;
	}
	char *abs = realpath(g_dir, NULL);   // absolute: the fstatat tests need one
	if (!abs) {
		perror("realpath");
		return 1;
	}
	snprintf(g_dir, sizeof g_dir, "%s", abs);
	free(abs);
	test_qsort_r();
	test_misc();
	test_fs();
	test_fs_at();
	test_memstream();
	test_thread();
	test_locale();
	rmdir(g_dir);
	printf("%s test_platform: %d checks, %d failed\n", g_fail ? "FAIL" : "ok  ", g_pass + g_fail, g_fail);
	return g_fail ? 1 : 0;
}
