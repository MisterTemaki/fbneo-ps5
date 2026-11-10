/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * Host tests only (Linux, glibc): the FreeBSD types and constants src/ps5rt_locale.c uses, which the console build
 * takes from the SDK's <runetype.h> and <_ctype.h>. The layout of _RuneLocale and the _CTYPE_ bits are the FreeBSD
 * interface the SDK headers declare (and that the inline ctype functions of <xlocale/_ctype.h> read); the console
 * build checks the size and offsets of this copy against them (ps5rt_locale.c).
 */
#ifndef PS5RT_HOST_CTYPE_H
#define PS5RT_HOST_CTYPE_H

#include <stddef.h>

typedef int __ct_rune_t;
typedef int __rune_t;

#define _CACHED_RUNES (1 << 8)

typedef struct {
	__rune_t __min;
	__rune_t __max;
	__rune_t __map;
	unsigned long *__types;
} _RuneEntry;

typedef struct {
	int __nranges;
	_RuneEntry *__ranges;
} _RuneRange;

typedef struct {
	char __magic[8];
	char __encoding[32];
	__rune_t (*__sgetrune)(const char *, size_t, char const **);
	int (*__sputrune)(__rune_t, char *, size_t, char **);
	__rune_t __invalid_rune;
	unsigned long __runetype[_CACHED_RUNES];
	__rune_t __maplower[_CACHED_RUNES];
	__rune_t __mapupper[_CACHED_RUNES];
	_RuneRange __runetype_ext;
	_RuneRange __maplower_ext;
	_RuneRange __mapupper_ext;
	void *__variable;
	int __variable_len;
} _RuneLocale;

#define _CTYPE_A 0x00000100L
#define _CTYPE_C 0x00000200L
#define _CTYPE_D 0x00000400L
#define _CTYPE_G 0x00000800L
#define _CTYPE_L 0x00001000L
#define _CTYPE_P 0x00002000L
#define _CTYPE_S 0x00004000L
#define _CTYPE_U 0x00008000L
#define _CTYPE_X 0x00010000L
#define _CTYPE_B 0x00020000L
#define _CTYPE_R 0x00040000L
#define _CTYPE_I 0x00080000L
#define _CTYPE_T 0x00100000L
#define _CTYPE_Q 0x00200000L
#define _CTYPE_N 0x00400000L
#define _CTYPE_SW1 0x40000000L

#endif
