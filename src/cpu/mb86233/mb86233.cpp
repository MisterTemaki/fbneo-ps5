// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
//
// FBNeo PS5 2.3: MAME's MB86233/MB86234 interpreter (src/devices/cpu/mb86233/mb86233.cpp, mamedev/mame b7f483f)
// without MAME's device framework. The code is in mb86233_ops.inc (MAME's); this file has the helpers MAME's headers
// gave it. MAME's notes on the chip:
/*
  Driver based on the initial reverse-engineering of Elsemi, extended,
  generalized and made to look more like a cpu since then thanks in
  part to a "manual" that barely deserves the name.

  The 86232 has 512 32-bits dwords of triple-port memory (1 write, 2
  read).  The 86233/86234 have instead two normal (1 read, 1 write,
  non-simultaneous) independent ram banks, one of 256 dwords and one
  of 512.

  The cpu can theorically work in either floating point (32-bits ieee
  flots) or fixed point (32/36/48 bits registers) modes.  All sega
  programs start by activating floating point and staying there, so
  fixed point is not implemented.

  Interrupts are not implemented at this point.
*/

#include "mb86233.h"

#include <string.h>
#include <math.h>
#include <algorithm>
#include <iterator>

static inline u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static inline float u2f(u32 u) { float f; memcpy(&f, &u, 4); return f; }

static inline s32 mb86233_sext(u32 v, int bits)
{
	return s32(v << (32 - bits)) >> (32 - bits);
}

// x86's cvttss2si result for NaN and out-of-range values: the "integer indefinite" value
static inline u32 mb86233_f2i32(float v)
{
	if (!(v >= -2147483648.0f && v < 2147483648.0f))
		return 0x80000000u;
	return u32(s32(v));
}

#include "mb86233_ops.inc"
