// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
//
// FBNeo PS5 2.3: the Fujitsu MB86233/MB86234 (TGP) interpreter of MAME (src/devices/cpu/mb86233, mamedev/mame b7f483f)
// without MAME's device framework. The class keeps MAME's members and functions, so the instruction code
// (mb86233_ops.inc) is MAME's own; the four address spaces (program, data, io, register file) are the port's
// (mb86233_intf.cpp). Internal to src/cpu/mb86233 and mb86233_intf.cpp: drivers use mb86233_intf.h.
#ifndef FBNEO_CPU_MB86233_H
#define FBNEO_CPU_MB86233_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

// the spaces, as mb86233_intf.cpp gives them (addresses are 32-bit word numbers, MAME's -2 shift)
u32 mb86233_program_read(u32 a);
u32 mb86233_data_read(u32 a);
void mb86233_data_write(u32 a, u32 d);
u32 mb86233_io_read(u32 a);
void mb86233_io_write(u32 a, u32 d);
u32 mb86233_rf_read(u32 a);
void mb86233_rf_write(u32 a, u32 d);
void mb86233_log(const char* fmt, ...);
#define MB86233_LOG(...) mb86233_log(__VA_ARGS__)

struct mb86233_space_program
{
	u32 read_dword(u32 a) { return mb86233_program_read(a & 0xffff); }
};
struct mb86233_space_data
{
	u32 read_dword(u32 a) { return mb86233_data_read(a & 0xffff); }
	void write_dword(u32 a, u32 d) { mb86233_data_write(a & 0xffff, d); }
};
struct mb86233_space_io
{
	u32 read_dword(u32 a) { return mb86233_io_read(a & 0xffff); }
	void write_dword(u32 a, u32 d) { mb86233_io_write(a & 0xffff, d); }
};
struct mb86233_space_rf
{
	u32 read_dword(u32 a) { return mb86233_rf_read(a & 0xf); }
	void write_dword(u32 a, u32 d) { mb86233_rf_write(a & 0xf, d); }
};

class mb86233_core
{
public:
	enum st_flags {
		F_ZRC  = 0x00000001,
		F_ZRD  = 0x00000002,
		F_SGC  = 0x00000004,
		F_SGD  = 0x00000008,
		F_CPC  = 0x00000010,
		F_CPD  = 0x00000020,
		F_OVC  = 0x00000040,
		F_OVD  = 0x00000080,
		F_UNC  = 0x00000100,
		F_UND  = 0x00000200,
		F_DVZC = 0x00000400,
		F_DVZD = 0x00000800,
		F_CA   = 0x00001000,
		F_CPP  = 0x00002000,
		F_OVM  = 0x00004000,
		F_UNM  = 0x00008000,
		F_SIF0 = 0x00010000,
		F_SIF1 = 0x00020000,
		F_SOF0 = 0x00040000,

		F_PIF  = 0x00100000,
		F_POF  = 0x00200000,
		F_PAIF = 0x00400000,
		F_PAOF = 0x00800000,
		F_F0S  = 0x01000000,
		F_F1S  = 0x02000000,
		F_IT   = 0x04000000,
		F_ZX0  = 0x08000000,
		F_ZX1  = 0x10000000,
		F_ZX2  = 0x20000000,
		F_ZC0  = 0x40000000,
		F_ZC1  = 0x80000000
	};

	void stall() { m_stall = true; }

	void gpio0_w(int state);
	void gpio1_w(int state);
	void gpio2_w(int state);
	void gpio3_w(int state);

	void device_reset();
	void execute_run();

	// MAME's private members (public here: mb86233_intf.cpp saves them, the tests look at them)
	mb86233_space_program m_cache;
	mb86233_space_program m_program;
	mb86233_space_data m_data;
	mb86233_space_io m_io;
	mb86233_space_rf m_rf;

	int m_icount = 0;

	u32 m_st = 0, m_a = 0, m_b = 0, m_d = 0, m_p = 0;
	u32 m_alu_stmask = 0, m_alu_stset = 0, m_alu_r1 = 0, m_alu_r2 = 0;
	u16 m_ppc = 0, m_pc = 0, m_sp = 0, m_b0 = 0, m_b1 = 0, m_x0 = 0, m_x1 = 0, m_i0 = 0, m_i1 = 0, m_vsmr = 0, m_pcs[4] = {0, 0, 0, 0}, m_mask = 0, m_m = 0;
	u8 m_r = 0, m_rpc = 0, m_c0 = 0, m_c1 = 0, m_sft = 0, m_vsm = 0;
	bool m_gpio0 = false, m_gpio1 = false, m_gpio2 = false, m_gpio3 = false;

	bool m_stall = false;

	static u32 set_exp(u32 val, u32 exp);
	static u32 set_mant(u32 val, u32 mant);
	static u32 get_exp(u32 val);
	static u32 get_mant(u32 val);

	void alu_update_st();
	void alu_pre(u32 alu);
	void alu_post_1(u32 alu);
	void alu_post_2(u32 alu);
	u16 ea_pre_0(u32 r);
	void ea_post_0(u32 r);
	u16 ea_pre_1(u32 r);
	void ea_post_1(u32 r);
	void pcs_push();
	void pcs_pop();
	inline void stset_set_sz_int(u32 val);
	inline void stset_set_sz_fp(u32 val);

	u32 read_reg(u32 r);
	void write_reg(u32 r, u32 v);
	void write_mem_internal_1(u32 r, u32 v, bool bank);
	void write_mem_external_1(u32 r, u32 v);
	void write_mem_io_1(u32 r, u32 v);
};

// the one core mb86233_intf.cpp runs (tests)
mb86233_core* Mb86233Core();

#endif
