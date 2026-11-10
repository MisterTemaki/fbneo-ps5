// FBNeo PS5 2.3 tests: just enough of MAME's emu.h for MAME's own i960.cpp and mb86233.cpp (copied here unmodified
// from mamedev/mame b7f483f, BSD-3-Clause) to build on their own, so the port's cores (src/cpu/i960, src/cpu/mb86233)
// can be run side by side with them (tests/test_cpus.cpp). The address spaces are test_space objects the test fills.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>
#include <memory>
#include <utility>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <bit>
#include <ostream>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u32 offs_t;

#define ATTR_COLD
#define NAME(x) x, #x
#define DWORD_ALIGNED(a) (((a) & 3) == 0)
#define WORD_ALIGNED(a) (((a) & 1) == 0)

template <typename T, typename U> constexpr T BIT(T x, U n) { return (x >> n) & T(1); }

inline u64 mulu_32x32(u32 a, u32 b) { return u64(a) * u64(b); }

namespace util {
template <typename T> constexpr std::make_signed_t<T> sext(T value, unsigned width)
{
	return std::make_signed_t<T>(value << (sizeof(T) * 8 - width)) >> (sizeof(T) * 8 - width);
}
class disasm_interface
{
public:
	virtual ~disasm_interface() = default;
};
}

[[noreturn]] inline void fatalerror(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	throw std::runtime_error(buf);
}

template <typename... Args> std::string string_format(const char* fmt, Args... args)
{
	char buf[256];
	snprintf(buf, sizeof(buf), fmt, args...);
	return buf;
}

enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };
enum { AS_PROGRAM = 0, AS_DATA = 1, AS_IO = 2 };
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3 };
enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };

// one address space: the test gives its accesses (by the space's own address units) and the BURST flag of each address
struct test_space
{
	u32 (*read32)(void* ctx, u32 a) = nullptr;
	void (*write32)(void* ctx, u32 a, u32 d, u32 mask) = nullptr;
	u16 (*flags)(void* ctx, u32 a) = nullptr;
	void* ctx = nullptr;
	template <typename T> void cache(T& t) { t.sp = this; }
	template <typename T> void specific(T& t) { t.sp = this; }
};

template <int AddrWidth, int Level, int AddrShift, endianness_t Endian> struct memory_access
{
	struct specific
	{
		test_space* sp = nullptr;
		// MAME masks accesses to the configured address width. The TGP can add a bank offset
		// after calculating a 16-bit effective address, which must wrap before reaching the bus.
		static constexpr offs_t address_mask = 0xffffffffu >> (32 - AddrWidth);
		// byte-addressed (AddrShift 0): MAME's 32-bit little-endian bus, sub-word lanes of the dword
		u8 read_byte(offs_t a) { return sp->read32(sp->ctx, a & ~3u) >> ((a & 3) * 8); }
		u16 read_word(offs_t a) { return sp->read32(sp->ctx, a & ~3u) >> ((a & 2) * 8); }
		u32 read_dword(offs_t a) { a &= address_mask; return AddrShift == 0 ? sp->read32(sp->ctx, a & ~3u) : sp->read32(sp->ctx, a); }
		std::pair<u8, u16> read_byte_flags(offs_t a) { u16 f = sp->flags(sp->ctx, a); return std::pair<u8, u16>(read_byte(a), f); }
		std::pair<u32, u16> read_dword_flags(offs_t a) { a &= address_mask; u16 f = sp->flags(sp->ctx, a); return std::pair<u32, u16>(read_dword(a), f); }
		void write_byte(offs_t a, u8 d) { sp->write32(sp->ctx, a & ~3u, u32(d) << ((a & 3) * 8), 0xffu << ((a & 3) * 8)); }
		void write_word(offs_t a, u16 d) { sp->write32(sp->ctx, a & ~3u, u32(d) << ((a & 2) * 8), 0xffffu << ((a & 2) * 8)); }
		void write_dword(offs_t a, u32 d) { a &= address_mask; AddrShift == 0 ? sp->write32(sp->ctx, a & ~3u, d, 0xffffffff) : sp->write32(sp->ctx, a, d, 0xffffffff); }
		u16 write_byte_flags(offs_t a, u8 d) { u16 f = sp->flags(sp->ctx, a); write_byte(a, d); return f; }
		u16 write_dword_flags(offs_t a, u32 d) { a &= address_mask; u16 f = sp->flags(sp->ctx, a); write_dword(a, d); return f; }
	};
	typedef specific cache;
};

struct address_space_config
{
	address_space_config(const char*, endianness_t, int, int, int) { }
};

struct machine_config { };
class device_t;
typedef const void* device_type;
#define DECLARE_DEVICE_TYPE(Type, Class) extern device_type Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) device_type Type = nullptr;

struct device_state_entry
{
	int m_index = 0;
	int index() const { return m_index; }
};

struct state_entry_ref
{
	state_entry_ref& formatstr(const char*) { return *this; }
	state_entry_ref& noshow() { return *this; }
};

class device_memory_interface
{
public:
	typedef std::vector<std::pair<int, const address_space_config*>> space_config_vector;
};

class device_t
{
public:
	device_t(const machine_config&, device_type, const char*, device_t*, u32) { }
	virtual ~device_t() = default;
	template <typename... Args> void logerror(Args...) { }
	template <typename T> void save_item(T&&, const char*) { }
	test_space m_spaces[8];
	test_space& space(int n) { return m_spaces[n]; }
protected:
	virtual void device_start() { }
	virtual void device_reset() { }
};

class cpu_device : public device_t
{
public:
	using space_config_vector = device_memory_interface::space_config_vector;
	cpu_device(const machine_config& mc, device_type t, const char* tag, device_t* owner, u32 clock) : device_t(mc, t, tag, owner, clock) { }
	void start() { device_start(); }
	void reset() { device_reset(); }
	void run() { execute_run(); }
	void set_input(int line, int state) { execute_set_input(line, state); }
	int* m_icountptr = nullptr;
protected:
	virtual uint32_t execute_min_cycles() const noexcept { return 1; }
	virtual uint32_t execute_max_cycles() const noexcept { return 1; }
	virtual u64 execute_clocks_to_cycles(u64 clocks) const noexcept { return clocks; }
	virtual u64 execute_cycles_to_clocks(u64 cycles) const noexcept { return cycles; }
	virtual void execute_run() { }
	virtual void execute_set_input(int, int) { }
	virtual space_config_vector memory_space_config() const { return {}; }
	virtual void state_string_export(const device_state_entry&, std::string&) const { }
	virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }
	template <typename T> state_entry_ref state_add(int, const char*, T&) { return state_entry_ref(); }
	void set_icountptr(int& i) { m_icountptr = &i; }
	void debugger_instruction_hook(u32) { }
	void standard_irq_callback(int, u32) { }
};
