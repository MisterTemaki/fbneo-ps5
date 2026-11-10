/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * qsort_r in the BSD form the SDK's <stdlib.h> (FreeBSD 11) declares, and that RADV was built for (Mesa's meson found
 * "BSD qsort_r"; its call sites pass the context fourth and util_qsort_adapter fifth):
 *
 *   void qsort_r(void *base, size_t nmemb, size_t size, void *thunk, int (*compar)(void *thunk, const void *, const void *));
 *
 * (not GNU's, whose context comes last and is the comparison's third argument). The console's libc has qsort and
 * qsort_s, not this. Insertion sort for short runs, heapsort otherwise: O(n log n) at worst, no recursion, no
 * allocation. Not stable, which qsort never promises.
 */
#include <stddef.h>
#include <stdint.h>

typedef int (*ps5rt_cmp_r)(void *, const void *, const void *);

void qsort_r(void *base, size_t nmemb, size_t size, void *thunk, ps5rt_cmp_r compar);

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
	if (a == b)
		return;
	while (size >= sizeof(uint64_t)) {
		uint64_t t;
		__builtin_memcpy(&t, a, sizeof t);
		__builtin_memcpy(a, b, sizeof t);
		__builtin_memcpy(b, &t, sizeof t);
		a += sizeof t;
		b += sizeof t;
		size -= sizeof t;
	}
	while (size--) {
		unsigned char t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

static void sift_down(unsigned char *base, size_t root, size_t n, size_t size, void *thunk, ps5rt_cmp_r cmp)
{
	for (;;) {
		size_t child = 2 * root + 1;
		if (child >= n)
			return;
		if (child + 1 < n && cmp(thunk, base + child * size, base + (child + 1) * size) < 0)
			child++;
		if (cmp(thunk, base + root * size, base + child * size) >= 0)
			return;
		swap_bytes(base + root * size, base + child * size, size);
		root = child;
	}
}

void qsort_r(void *base, size_t nmemb, size_t size, void *thunk, ps5rt_cmp_r compar)
{
	unsigned char *b = base;
	if (nmemb < 2 || size == 0)
		return;
	if (nmemb <= 12) {
		for (size_t i = 1; i < nmemb; i++)
			for (size_t j = i; j > 0 && compar(thunk, b + (j - 1) * size, b + j * size) > 0; j--)
				swap_bytes(b + (j - 1) * size, b + j * size, size);
		return;
	}
	for (size_t i = nmemb / 2; i-- > 0;)
		sift_down(b, i, nmemb, size, thunk, compar);
	for (size_t end = nmemb - 1; end > 0; end--) {
		swap_bytes(b, b + end * size, size);
		sift_down(b, 0, end, size, thunk, compar);
	}
}
