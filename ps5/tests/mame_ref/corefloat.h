// FBNeo PS5 2.3 tests: MAME's corefloat.h helpers the reference cores use (see emu.h)
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstring>
#include <cstdint>
inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
