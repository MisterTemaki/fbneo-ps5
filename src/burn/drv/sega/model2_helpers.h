// FBNeo PS5 2.3: what MAME's headers (corefloat.h, bitops, rgb_t, copybitmap_trans, logerror) gave the Model 2 code
// ported from MAME (model2_v.cpp, model2rd.ipp, segaic24), for a C++11 core.
// SPDX-License-Identifier: BSD-3-Clause
#ifndef FBNEO_MODEL2_HELPERS_H
#define FBNEO_MODEL2_HELPERS_H

#include <stdint.h>
#include <string.h>

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static inline int32_t m2_sext(uint32_t v, int bits)
{
	return int32_t(v << (32 - bits)) >> (32 - bits);
}

template<typename T>
static inline T m2_clamp(T v, T lo, T hi)
{
	return v < lo ? lo : (hi < v ? hi : v);
}

static inline int m2_countl_zero(uint32_t v)
{
	return v ? __builtin_clz(v) : 32;
}

// MAME's rgb_t(r, g, b): alpha 0xff
static inline uint32_t m2_rgb(uint32_t r, uint32_t g, uint32_t b)
{
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

void m2_log(const char* fmt, ...);
#define M2_LOG(...) m2_log(__VA_ARGS__)

#endif
