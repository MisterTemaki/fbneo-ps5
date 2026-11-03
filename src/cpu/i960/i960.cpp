// license:BSD-3-Clause
// copyright-holders:Farfetch'd, R. Belmont
//
// FBNeo PS5 2.3: MAME's i960 interpreter (src/devices/cpu/i960/i960.cpp, mamedev/mame b7f483f) without MAME's device
// framework. The instructions are in i960_ops.inc (MAME's code); this file has what MAME's device_start/device_reset
// did and the small helpers MAME's headers gave it.

#include "i960.h"

#include <string.h>
#include <math.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <iterator>

// ---- what MAME's emu.h / corefloat.h / bitops gave the instruction code ----------------------------------------
#ifndef BIT
#define BIT(x, n) (((x) >> (n)) & 1)
#endif
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define DWORD_ALIGNED(a) (((a) & 3) == 0)
#define WORD_ALIGNED(a) (((a) & 1) == 0)

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static inline int32_t i960_sext(uint32_t v, int bits)
{
	return int32_t(v << (32 - bits)) >> (32 - bits);
}
static inline uint32_t i960_rotl(uint32_t v, uint32_t n)
{
	n &= 31;
	return n ? (v << n) | (v >> (32 - n)) : v;
}
// x86's cvttsd2si / cvtsd2si result for NaN and out-of-range values: the "integer indefinite" value
static inline uint32_t i960_d2i32(double v)
{
	if (!(v >= -2147483648.0 && v < 2147483648.0))
		return 0x80000000u;
	return uint32_t(int32_t(v));
}
static inline uint64_t i960_d2i64(double v)
{
	if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
		return 0x8000000000000000ull;
	return uint64_t(int64_t(v));
}
static inline uint32_t i960_div_s(uint32_t a, uint32_t b)
{
	if (b == 0)
		return 0;
	if (a == 0x80000000u && b == 0xffffffffu)
		return 0x80000000u;
	return uint32_t(int32_t(a) / int32_t(b));
}
static inline uint32_t i960_rem_s(uint32_t a, uint32_t b)
{
	if (b == 0 || (a == 0x80000000u && b == 0xffffffffu))
		return 0;
	return uint32_t(int32_t(a) % int32_t(b));
}

#ifdef _MSC_VER
#include <cfloat>
#define logb _logb
#endif

#include "i960_ops.inc"

// ---- MAME's device_start / device_reset ---------------------------------------------------------------------------
void i960_core::device_start()
{
	memset(m_r, 0, sizeof(m_r));
	memset(m_rcache, 0, sizeof(m_rcache));
	m_immediate_vector = 0;
	m_immediate_pri = 0;
	std::fill(std::begin(m_rcache_frame_addr), std::end(m_rcache_frame_addr), 0);
	std::fill(std::begin(m_fp), std::end(m_fp), 0.0);
	std::fill(std::begin(m_irq_line_state), std::end(m_irq_line_state), 0);
	m_PIP = 0;
	m_stalled = false;
	m_rcache_pos = 0;
	m_SAT = m_PRCB = m_PC = m_AC = m_IP = m_ICR = 0;
	m_immediate_irq = 0;
	m_icount = 0;
}

void i960_core::device_reset()
{
	m_SAT        = m_program.read_dword(0);
	m_PRCB       = m_program.read_dword(4);
	m_IP         = m_program.read_dword(12);
	m_PC         = 0x001f2002;
	m_AC         = 0;
	m_ICR       = 0xff000000;
	m_immediate_irq = 0;

	memset(m_r, 0, sizeof(m_r));
	memset(m_rcache, 0, sizeof(m_rcache));

	m_r[I960_FP] = m_program.read_dword(m_PRCB+24);
	m_r[I960_SP] = m_r[I960_FP] + 64;
	m_rcache_pos = 0;

	m_stall_state.burst_mode = false;
}
