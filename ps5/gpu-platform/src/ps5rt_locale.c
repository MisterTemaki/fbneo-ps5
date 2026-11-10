/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * The xlocale functions (FreeBSD's *_l family, as the SDK's <xlocale.h> declares them) that libc++ and Mesa call and
 * the console's libc doesn't export, for the one locale a title has: "C" (also "POSIX", and "" since a title has no
 * locale environment). Another name is ENOENT, as for a locale that isn't installed; libc++ then throws its
 * runtime_error, as on any system without that locale.
 *
 * - newlocale / duplocale give the one C locale object; freelocale accepts it. LC_GLOBAL_LOCALE is treated as C too:
 *   a title keeps the global locale at "C", the start-up default (these functions don't follow a setlocale).
 * - Character classes and case (__runes_for_locale, which the inline isalpha_l ... toupper_l of <xlocale/_ctype.h>
 *   read, and ___runetype_l / ___tolower_l / ___toupper_l, iswctype_l): the C standard's "C" classes for 0-127,
 *   nothing for 128-255 and beyond. The low 8 bits of a digit's type hold its value, as <runetype.h> says.
 * - Multibyte conversions (mbrtowc_l, mbrlen_l, mbtowc_l, mbsrtowcs_l, mbsnrtowcs_l, wcrtomb_l, wcsnrtombs_l, btowc_l,
 *   wctob_l, ___mb_cur_max_l): a single-byte locale where every byte is the wide character of the same value, so any
 *   byte string converts and converts back (MB_CUR_MAX 1); a wide character above 255 is EILSEQ.
 * - Collation (strcoll_l, strxfrm_l, wcscoll_l, wcsxfrm_l): the C locale's, code-point order.
 * - Numbers, time and formatted I/O (strtod_l, strtof_l, strtold_l, strtoll_l, strtoull_l, strftime_l, snprintf_l,
 *   asprintf_l, sscanf_l and their v-forms): the system's own functions, which are the C locale's while the global
 *   locale is "C".
 * - localeconv_l and localeconv: the C locale's struct lconv ("." and CHAR_MAX everywhere else). localeconv is the
 *   system's too; a program that links this object gets this one, the same values.
 * - catopen / catgets / catclose: a title has no message catalogues; catgets gives back its default string.
 */
#if !defined(__PROSPERO__) && !defined(__FreeBSD__)
/* host tests: glibc's freelocale returns void, FreeBSD's (this one) int */
#define _GNU_SOURCE
#define freelocale ps5rt_glibc_freelocale
#include <locale.h>
#undef freelocale
#include "ps5rt_host_ctype.h"
#else
#include <locale.h>
#include <runetype.h>
#include <xlocale.h>
#endif

#include <errno.h>
#include <limits.h>
#include <nl_types.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

/* ---- the C locale object ---- */

static const char g_c_locale_object = 'C';
#define C_LOCALE ((locale_t)(uintptr_t)&g_c_locale_object)

static int is_c_name(const char *name)
{
	return name && (!strcmp(name, "C") || !strcmp(name, "POSIX") || !name[0]);
}

locale_t newlocale(int mask, const char *locale, locale_t base)
{
	(void)base;
	if ((mask & ~LC_ALL_MASK) || !locale) {
		errno = EINVAL;
		return NULL;
	}
	if (!is_c_name(locale)) {
		errno = ENOENT;
		return NULL;
	}
	return C_LOCALE;
}

locale_t duplocale(locale_t base)
{
	(void)base;
	return C_LOCALE;
}

int freelocale(locale_t loc)
{
	if (loc != C_LOCALE) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* ---- character classes: the C standard's, for 0-127 ---- */

#define IN(c, a, b) ((c) >= (a) && (c) <= (b))
#define C_UPPER(c) IN(c, 'A', 'Z')
#define C_LOWER(c) IN(c, 'a', 'z')
#define C_DIGIT(c) IN(c, '0', '9')
#define C_HEXLETTER(c) (IN(c, 'A', 'F') || IN(c, 'a', 'f'))
#define C_SPACE(c) ((c) == ' ' || IN(c, '\t', '\r'))
#define C_BLANK(c) ((c) == ' ' || (c) == '\t')
#define C_CNTRL(c) (IN(c, 0, 31) || (c) == 127)
#define C_PRINT(c) IN(c, 32, 126)
#define C_GRAPH(c) IN(c, 33, 126)
#define C_PUNCT(c) (C_GRAPH(c) && !C_UPPER(c) && !C_LOWER(c) && !C_DIGIT(c))
#define C_VALUE(c) (C_DIGIT(c) ? (c) - '0' : IN(c, 'A', 'F') ? (c) - 'A' + 10 : IN(c, 'a', 'f') ? (c) - 'a' + 10 : 0)
#define C_TYPE(c)                                                                                                    \
	((unsigned long)((C_UPPER(c) ? _CTYPE_U | _CTYPE_A : 0) | (C_LOWER(c) ? _CTYPE_L | _CTYPE_A : 0) |               \
					 (C_DIGIT(c) ? _CTYPE_D : 0) | (C_DIGIT(c) || C_HEXLETTER(c) ? _CTYPE_X : 0) |                     \
					 (C_SPACE(c) ? _CTYPE_S : 0) | (C_BLANK(c) ? _CTYPE_B : 0) | (C_CNTRL(c) ? _CTYPE_C : 0) |          \
					 (C_PRINT(c) ? _CTYPE_R | _CTYPE_SW1 : 0) | (C_GRAPH(c) ? _CTYPE_G : 0) |                          \
					 (C_PUNCT(c) ? _CTYPE_P : 0) | C_VALUE(c)))
#define C_LOWERED(c) (C_UPPER(c) ? (c) + 32 : (c))
#define C_UPPERED(c) (C_LOWER(c) ? (c) - 32 : (c))

#define T1(F, n) F(n),
#define T4(F, n) T1(F, n) T1(F, (n) + 1) T1(F, (n) + 2) T1(F, (n) + 3)
#define T16(F, n) T4(F, n) T4(F, (n) + 4) T4(F, (n) + 8) T4(F, (n) + 12)
#define T64(F, n) T16(F, n) T16(F, (n) + 16) T16(F, (n) + 32) T16(F, (n) + 48)
#define T256(F) T64(F, 0) T64(F, 64) T64(F, 128) T64(F, 192)

/* The C locale's table under the name the SDK's <runetype.h> gives it (extern const _RuneLocale _DefaultRuneLocale):
 * libc++'s ctype<char>::classic_table() and the inline __isctype read it. Defined here, the title takes no
 * locale-related data from the system either (before, a program that linked only this object imported the console's
 * libSceLibcInternal copy: the second M2 review). __runes_for_locale gives the same table. */
const _RuneLocale _DefaultRuneLocale = {
	.__magic = {'R', 'u', 'n', 'e', 'M', 'a', 'g', 'i'}, /* eight-byte ABI tag, no trailing NUL */
	.__encoding = "NONE",
	.__invalid_rune = 0xFFFD,
	.__runetype = {T256(C_TYPE)},
	.__maplower = {T256(C_LOWERED)},
	.__mapupper = {T256(C_UPPERED)},
};

#if defined(__PROSPERO__) || defined(__FreeBSD__)
_Static_assert(sizeof(((_RuneLocale *)0)->__runetype) / sizeof(unsigned long) == 256, "_CACHED_RUNES is 256");
#endif

_RuneLocale *__runes_for_locale(locale_t loc, int *mb_sb_limit)
{
	(void)loc;
	if (mb_sb_limit)
		*mb_sb_limit = _CACHED_RUNES;
	return (_RuneLocale *)&_DefaultRuneLocale;
}

/* beyond the cached 256: nothing in the C locale */
unsigned long ___runetype_l(__ct_rune_t c, locale_t loc)
{
	(void)loc;
	return (c >= 0 && c < _CACHED_RUNES) ? _DefaultRuneLocale.__runetype[c] : 0;
}

__ct_rune_t ___tolower_l(__ct_rune_t c, locale_t loc)
{
	(void)loc;
	return (c >= 0 && c < _CACHED_RUNES) ? _DefaultRuneLocale.__maplower[c] : c;
}

__ct_rune_t ___toupper_l(__ct_rune_t c, locale_t loc)
{
	(void)loc;
	return (c >= 0 && c < _CACHED_RUNES) ? _DefaultRuneLocale.__mapupper[c] : c;
}

/* wctype_t is a _CTYPE_ mask on FreeBSD (libc++ passes its ctype_base masks, the same bits) */
int iswctype_l(wint_t wc, wctype_t charclass, locale_t loc)
{
	return (___runetype_l((__ct_rune_t)wc, loc) & charclass) != 0;
}

/* ---- multibyte: one byte, one wide character of the same value ---- */

int ___mb_cur_max_l(locale_t loc)
{
	(void)loc;
	return 1;
}

size_t mbrtowc_l(wchar_t *restrict pwc, const char *restrict s, size_t n, mbstate_t *restrict ps, locale_t loc)
{
	(void)ps;
	(void)loc;
	if (!s)
		return 0;
	if (n == 0)
		return (size_t)-2;
	unsigned char c = (unsigned char)*s;
	if (pwc)
		*pwc = (wchar_t)c;
	return c ? 1 : 0;
}

size_t mbrlen_l(const char *restrict s, size_t n, mbstate_t *restrict ps, locale_t loc)
{
	return mbrtowc_l(NULL, s, n, ps, loc);
}

int mbtowc_l(wchar_t *restrict pwc, const char *restrict s, size_t n, locale_t loc)
{
	(void)loc;
	if (!s)
		return 0;   /* no shift states */
	if (n == 0) {
		errno = EILSEQ;
		return -1;
	}
	unsigned char c = (unsigned char)*s;
	if (pwc)
		*pwc = (wchar_t)c;
	return c ? 1 : 0;
}

size_t mbsnrtowcs_l(wchar_t *restrict dst, const char **restrict src, size_t nms, size_t len, mbstate_t *restrict ps,
	locale_t loc)
{
	(void)ps;
	(void)loc;
	const unsigned char *s = (const unsigned char *)*src;
	size_t count = 0;
	if (!dst) {
		while (count < nms && s[count])
			count++;
		return count;
	}
	while (count < len) {
		if (nms == 0) {
			*src = (const char *)s;
			return count;
		}
		dst[count] = (wchar_t)*s;
		if (*s == 0) {
			*src = NULL;
			return count;
		}
		s++;
		nms--;
		count++;
	}
	*src = (const char *)s;
	return count;
}

size_t mbsrtowcs_l(wchar_t *restrict dst, const char **restrict src, size_t len, mbstate_t *restrict ps, locale_t loc)
{
	return mbsnrtowcs_l(dst, src, SIZE_MAX, len, ps, loc);
}

size_t wcrtomb_l(char *restrict s, wchar_t wc, mbstate_t *restrict ps, locale_t loc)
{
	(void)ps;
	(void)loc;
	if (!s)
		return 1;   /* wcrtomb(internal buffer, L'\0', ps) */
	long v = (long)wc;
	if (v < 0 || v > 0xFF) {
		errno = EILSEQ;
		return (size_t)-1;
	}
	*s = (char)(unsigned char)v;
	return 1;
}

size_t wcsnrtombs_l(char *restrict dst, const wchar_t **restrict src, size_t nwc, size_t len, mbstate_t *restrict ps,
	locale_t loc)
{
	(void)ps;
	(void)loc;
	const wchar_t *w = *src;
	size_t count = 0;
	if (!dst) {
		for (; count < nwc && w[count]; count++) {
			long v = (long)w[count];
			if (v < 0 || v > 0xFF) {
				errno = EILSEQ;
				return (size_t)-1;
			}
		}
		return count;
	}
	while (count < len) {
		if (nwc == 0) {
			*src = w;
			return count;
		}
		long v = (long)*w;
		if (v < 0 || v > 0xFF) {
			*src = w;
			errno = EILSEQ;
			return (size_t)-1;
		}
		dst[count] = (char)(unsigned char)v;
		if (v == 0) {
			*src = NULL;
			return count;
		}
		w++;
		nwc--;
		count++;
	}
	*src = w;
	return count;
}

wint_t btowc_l(int c, locale_t loc)
{
	(void)loc;
	if (c == EOF)
		return WEOF;
	return (wint_t)(unsigned char)c;
}

int wctob_l(wint_t wc, locale_t loc)
{
	(void)loc;
	long v = (long)wc;
	if (wc == WEOF || v < 0 || v > 0xFF)
		return EOF;
	return (int)v;
}

/* ---- collation: code-point order ---- */

int strcoll_l(const char *a, const char *b, locale_t loc)
{
	(void)loc;
	return strcmp(a, b);
}

size_t strxfrm_l(char *restrict dst, const char *restrict src, size_t n, locale_t loc)
{
	(void)loc;
	size_t len = strlen(src);
	if (len < n)
		memcpy(dst, src, len + 1);
	return len;
}

int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t loc)
{
	(void)loc;
	return wcscmp(a, b);
}

size_t wcsxfrm_l(wchar_t *restrict dst, const wchar_t *restrict src, size_t n, locale_t loc)
{
	(void)loc;
	size_t len = wcslen(src);
	if (len < n)
		wmemcpy(dst, src, len + 1);
	return len;
}

/* ---- numbers, time, formatted I/O: the system's, in the global "C" locale ---- */

double strtod_l(const char *restrict s, char **restrict end, locale_t loc)
{
	(void)loc;
	return strtod(s, end);
}

float strtof_l(const char *restrict s, char **restrict end, locale_t loc)
{
	(void)loc;
	return strtof(s, end);
}

long double strtold_l(const char *restrict s, char **restrict end, locale_t loc)
{
	(void)loc;
	return strtold(s, end);
}

long long strtoll_l(const char *restrict s, char **restrict end, int base, locale_t loc)
{
	(void)loc;
	return strtoll(s, end, base);
}

unsigned long long strtoull_l(const char *restrict s, char **restrict end, int base, locale_t loc)
{
	(void)loc;
	return strtoull(s, end, base);
}

size_t strftime_l(char *restrict s, size_t max, const char *restrict format, const struct tm *restrict tm,
	locale_t loc)
{
	(void)loc;
	return strftime(s, max, format, tm);
}

int vsnprintf_l(char *restrict s, size_t n, locale_t loc, const char *restrict format, va_list ap)
{
	(void)loc;
	return vsnprintf(s, n, format, ap);
}

int snprintf_l(char *restrict s, size_t n, locale_t loc, const char *restrict format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = vsnprintf_l(s, n, loc, format, ap);
	va_end(ap);
	return r;
}

int vasprintf_l(char **ret, locale_t loc, const char *format, va_list ap)
{
	(void)loc;
	return vasprintf(ret, format, ap);
}

int asprintf_l(char **ret, locale_t loc, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = vasprintf_l(ret, loc, format, ap);
	va_end(ap);
	return r;
}

int vsscanf_l(const char *restrict s, locale_t loc, const char *restrict format, va_list ap)
{
	(void)loc;
	return vsscanf(s, format, ap);
}

int sscanf_l(const char *restrict s, locale_t loc, const char *restrict format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = vsscanf_l(s, loc, format, ap);
	va_end(ap);
	return r;
}

/* ---- the C locale's conventions ---- */

static struct lconv g_c_lconv = {
	.decimal_point = ".",
	.thousands_sep = "",
	.grouping = "",
	.int_curr_symbol = "",
	.currency_symbol = "",
	.mon_decimal_point = "",
	.mon_thousands_sep = "",
	.mon_grouping = "",
	.positive_sign = "",
	.negative_sign = "",
	.int_frac_digits = CHAR_MAX,
	.frac_digits = CHAR_MAX,
	.p_cs_precedes = CHAR_MAX,
	.p_sep_by_space = CHAR_MAX,
	.n_cs_precedes = CHAR_MAX,
	.n_sep_by_space = CHAR_MAX,
	.p_sign_posn = CHAR_MAX,
	.n_sign_posn = CHAR_MAX,
	.int_p_cs_precedes = CHAR_MAX,
	.int_n_cs_precedes = CHAR_MAX,
	.int_p_sep_by_space = CHAR_MAX,
	.int_n_sep_by_space = CHAR_MAX,
	.int_p_sign_posn = CHAR_MAX,
	.int_n_sign_posn = CHAR_MAX,
};

struct lconv *localeconv_l(locale_t loc)
{
	(void)loc;
	return &g_c_lconv;
}

struct lconv *localeconv(void)
{
	return &g_c_lconv;
}

/* ---- message catalogues: none ---- */

nl_catd catopen(const char *name, int flag)
{
	(void)name;
	(void)flag;
	errno = ENOENT;
	return (nl_catd)-1;
}

char *catgets(nl_catd catd, int set_id, int msg_id, const char *s)
{
	(void)set_id;
	(void)msg_id;
	errno = catd == (nl_catd)-1 ? EBADF : ENOMSG;
	return (char *)s;
}

int catclose(nl_catd catd)
{
	(void)catd;
	errno = EBADF;
	return -1;
}
