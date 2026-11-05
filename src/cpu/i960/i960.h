// license:BSD-3-Clause
// copyright-holders:Farfetch'd, R. Belmont
//
// FBNeo PS5 2.3: the Intel 80960KA/KB interpreter of MAME (src/devices/cpu/i960/i960.h/.cpp, mamedev/mame b7f483f)
// without MAME's device framework. The class keeps MAME's members and functions, so the instruction code
// (i960_ops.inc) is MAME's own; the address space is the port's page table (i960_intf.cpp). Internal to src/cpu/i960
// and i960_intf.cpp: drivers use i960_intf.h.
#ifndef FBNEO_CPU_I960_H
#define FBNEO_CPU_I960_H

#include <stdint.h>
#include <utility>

enum
{
	I960_PFP = 0,
	I960_SP  = 1,
	I960_RIP = 2,
	I960_FP  = 31
};

#ifndef I960_IRQ0
#define I960_IRQ0 0
#define I960_IRQ1 1
#define I960_IRQ2 2
#define I960_IRQ3 3
#endif

// The bus: what i960_intf.cpp gives the core (pages of memory or handlers, MAME's BURST flag per page).
uint8_t i960_bus_read8(uint32_t a);
uint16_t i960_bus_read16(uint32_t a);
uint32_t i960_bus_read32(uint32_t a);
void i960_bus_write8(uint32_t a, uint8_t d);
void i960_bus_write16(uint32_t a, uint16_t d);
void i960_bus_write32(uint32_t a, uint32_t d);
uint16_t i960_bus_flags(uint32_t a); // the page's flags (BURST), no access made
uint32_t i960_bus_fetch32(uint32_t a);
void i960_log(const char* fmt, ...);
void i960_fatal(const char* fmt, ...);
#define I960_LOG(...) i960_log(__VA_ARGS__)
// (MAME stops the machine there; the port logs the first few and goes on, a cycle spent so that code which is not
// i960 code -- a missing or stand-in ROM -- still lets the frame end)
#define I960_FATAL(...) (m_icount -= 1, i960_fatal(__VA_ARGS__))

// MAME's memory_access<32, 2, 0, ENDIANNESS_LITTLE>::specific / ::cache, as the instruction code calls them
struct i960_space
{
	uint8_t read_byte(uint32_t a) { return i960_bus_read8(a); }
	uint16_t read_word(uint32_t a) { return i960_bus_read16(a); }
	uint32_t read_dword(uint32_t a) { return i960_bus_read32(a); }
	std::pair<uint32_t, uint16_t> read_byte_flags(uint32_t a)
	{
		const uint16_t f = i960_bus_flags(a);
		return std::pair<uint32_t, uint16_t>(i960_bus_read8(a), f);
	}
	std::pair<uint32_t, uint16_t> read_dword_flags(uint32_t a)
	{
		const uint16_t f = i960_bus_flags(a);
		return std::pair<uint32_t, uint16_t>(i960_bus_read32(a), f);
	}
	void write_byte(uint32_t a, uint8_t d) { i960_bus_write8(a, d); }
	void write_word(uint32_t a, uint16_t d) { i960_bus_write16(a, d); }
	void write_dword(uint32_t a, uint32_t d) { i960_bus_write32(a, d); }
	uint16_t write_byte_flags(uint32_t a, uint8_t d)
	{
		const uint16_t f = i960_bus_flags(a);
		i960_bus_write8(a, d);
		return f;
	}
	uint16_t write_dword_flags(uint32_t a, uint32_t d)
	{
		const uint16_t f = i960_bus_flags(a);
		i960_bus_write32(a, d);
		return f;
	}
};
struct i960_cache
{
	uint32_t read_dword(uint32_t a) { return i960_bus_fetch32(a); }
};

class i960_core
{
public:
	static constexpr uint16_t BURST = 0x0001;

	void i960_stall()
	{
		m_stalled = true;
		m_IP = m_PIP;
	}

	void device_start();
	void device_reset();
	void execute_run();
	void execute_set_input(int inputnum, int state);

	enum { I960_RCACHE_SIZE = 4 };

	// MAME's private members (public here: i960_intf.cpp saves them, the tests look at them)
	void burst_stall_save(uint32_t t1, uint32_t t2, int index, int size, bool iswriteop);

	struct {
		uint32_t t1 = 0, t2 = 0;
		int index = 0, size = 0;
		bool burst_mode = false;
		bool iswriteop = false;
	}m_stall_state;
	bool m_stalled = false;
	bool m_irq_raised = false; // FBNeo: an interrupt raised while running (see i960_ops.inc's execute_run)

	uint32_t m_r[0x20];
	uint32_t m_rcache[I960_RCACHE_SIZE][0x10];
	uint32_t m_rcache_frame_addr[I960_RCACHE_SIZE];
	// rcache_pos = how deep in the stack we are.  0-(I960_RCACHE_SIZE-1) means in-cache.
	// I960_RCACHE_SIZE or greater means out of cache, must save to memory.
	int32_t m_rcache_pos = 0;

	double m_fp[4];

	uint32_t m_SAT = 0;
	uint32_t m_PRCB = 0;
	uint32_t m_PC = 0;
	uint32_t m_AC = 0;
	uint32_t m_IP = 0;
	uint32_t m_PIP = 0;
	uint32_t m_ICR = 0;

	int m_immediate_irq = 0;
	int m_immediate_vector = 0;
	int  m_immediate_pri = 0;

	int8_t m_irq_line_state[4];

	i960_cache m_cache;
	i960_space m_program;

	int m_icount = 0;

	uint32_t i960_read_dword_unaligned(uint32_t address);
	std::pair<uint32_t, uint16_t> i960_read_dword_unaligned_flags(uint32_t address);
	uint16_t i960_read_word_unaligned(uint32_t address);
	void i960_write_dword_unaligned(uint32_t address, uint32_t data);
	uint16_t i960_write_dword_unaligned_flags(uint32_t address, uint32_t data);
	void i960_write_word_unaligned(uint32_t address, uint16_t data);
	void send_iac(uint32_t adr);
	uint32_t get_ea(uint32_t opcode);
	uint32_t get_1_ri(uint32_t opcode);
	uint32_t get_2_ri(uint32_t opcode);
	uint64_t get_2_ri64(uint32_t opcode);
	void set_ri(uint32_t opcode, uint32_t val);
	void set_ri2(uint32_t opcode, uint32_t val, uint32_t val2);
	void set_ri64(uint32_t opcode, uint64_t val);
	double get_1_rif(uint32_t opcode);
	double get_2_rif(uint32_t opcode);
	void set_rif(uint32_t opcode, double val);
	double get_1_rifl(uint32_t opcode);
	double get_2_rifl(uint32_t opcode);
	void set_rifl(uint32_t opcode, double val);
	uint32_t get_1_ci(uint32_t opcode);
	uint32_t get_2_ci(uint32_t opcode);
	uint32_t get_disp(uint32_t opcode);
	uint32_t get_disp_s(uint32_t opcode);
	void cmp_s(int32_t v1, int32_t v2);
	void cmp_u(uint32_t v1, uint32_t v2);
	void concmp_s(int32_t v1, int32_t v2);
	void concmp_u(uint32_t v1, uint32_t v2);
	void cmp_d(double v1, double v2);
	void bxx(uint32_t opcode, int mask);
	void bxx_s(uint32_t opcode, int mask);
	void fxx(uint32_t opcode, int mask);
	void test(uint32_t opcode, int mask);
	double round_to_int(double val);
	void execute_op(uint32_t opcode);
	void execute_burst_stall_op(uint32_t opcode);
	void take_interrupt(int vector, int lvl);
	void check_immediate_irqs();
	void check_pending_irqs();
	void do_call(uint32_t adr, int type, uint32_t stack);
	void do_ret_0();
	void do_ret();
};

// the one core i960_intf.cpp runs (tests)
i960_core* I960Core();

#endif
