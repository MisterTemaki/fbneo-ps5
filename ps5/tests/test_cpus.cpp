// FBNeo PS5 2.3: Sega Model 2's two new CPU cores on their own (make test-cpus):
//   - the i960KB (src/cpu/i960 + i960_intf.cpp): known-answer programs (arithmetic and the condition codes, add with
//     carry, compares and branches, call/ret six frames deep through the four-frame register cache, flushreg, the
//     loads and stores of every size, ldq/stq on a BURST page and on a FIFO page, MEMB addressing, system calls,
//     interrupts through the interrupt table with the return restoring PC/AC, the KB's floating point, modac, the
//     stall of a FIFO read in the middle of an ldq); then random programs run on the port's core and on MAME's own
//     i960.cpp (tests/mame_ref, unmodified, on a stand-in emu.h), which must end with the same registers, register
//     cache, floating point registers and memory -- some with interrupts raised in the middle;
//   - the MB86234 TGP (src/cpu/mb86233 + mb86233_intf.cpp): known answers (the float ALU, the loads, a loop with c0,
//     a call and return, the FIFO reads through the register file) and random programs against MAME's mb86233.cpp.
// No ROM: the programs are made here.
//
// SPDX-License-Identifier: BSD-3-Clause

#include "burnint.h"
#include "i960_intf.h"
#include "mb86233_intf.h"
#include "i960/i960.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <functional>

#include "mame_ref/emu.h"
#define private public
#define protected public
#include "mame_ref/i960.h"
#include "mame_ref/mb86233.h"
#undef private
#undef protected
#include "mb86233/mb86233.h"

// what the interfaces take from the rest of FBNeo
INT32(__cdecl* BurnAcb)(struct BurnArea* pba) = NULL;
void CpuCheatRegister(INT32, cpu_core_config*) {}
static INT32 __cdecl TestPrintf(INT32, TCHAR* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	return 0;
}
INT32(__cdecl* bprintf)(INT32 nStatus, TCHAR* szFormat, ...) = TestPrintf;

namespace
{
int g_failed = 0, g_passed = 0;

void Check(bool ok, const std::string& what)
{
	if (ok)
		g_passed++;
	else
	{
		g_failed++;
		printf("  FAIL: %s\n", what.c_str());
	}
}

std::string Hex(uint64_t v)
{
	char b[32];
	snprintf(b, sizeof(b), "%llx", (unsigned long long)v);
	return b;
}

uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull)
{
	const uint8_t* b = (const uint8_t*)p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 1099511628211ull;
	return h;
}

// =====================================================================================================================
// i960
// =====================================================================================================================
// memory: 1 MB of RAM at 0 (BURST); a FIFO page at 0x00f00000 (no BURST: every word of an ldq/stq is the same address)
// whose reads give 0x1000, 0x1001... and whose writes are logged; a "slow FIFO" word at 0x00f00100 that stalls the CPU
// the first time each value is read (MAME's FIFO: stall, then the access made again)
constexpr uint32_t kRam = 1 << 20;
constexpr uint32_t kFifo = 0x00f00000;

struct Bus
{
	std::vector<uint8_t> ram;
	uint32_t fifo_next = 0x1000;
	std::vector<uint32_t> fifo_log;
	bool slow_ready = false;
	int stalls = 0;
	bool port = true; // which core this bus belongs to (the stall call differs)
	i80960kb_device* ref = nullptr;

	uint32_t Read32(uint32_t a)
	{
		if (a < kRam) { uint32_t v; memcpy(&v, &ram[a], 4); return v; }
		if (a == kFifo + 0x100)
		{
			if (!slow_ready)
			{
				slow_ready = true;
				stalls++;
				if (port) I960Stall(); else ref->i960_stall();
				return 0;
			}
			slow_ready = false;
			return fifo_next++;
		}
		if ((a & ~0xfffu) == kFifo) return fifo_next++;
		return 0;
	}
	void Write32(uint32_t a, uint32_t d, uint32_t mask)
	{
		if (a < kRam)
		{
			uint32_t v;
			memcpy(&v, &ram[a], 4);
			v = (v & ~mask) | (d & mask);
			memcpy(&ram[a], &v, 4);
			return;
		}
		if ((a & ~0xfffu) == kFifo) fifo_log.push_back(d & mask);
	}
	uint16_t Flags(uint32_t a) { return a < kRam ? 1 : 0; }
};

Bus g_pbus; // the port's core

UINT8 PRd8(UINT32 a) { return g_pbus.Read32(a & ~3u) >> ((a & 3) * 8); }
UINT16 PRd16(UINT32 a) { return g_pbus.Read32(a & ~3u) >> ((a & 2) * 8); }
UINT32 PRd32(UINT32 a) { return g_pbus.Read32(a); }
void PWr8(UINT32 a, UINT8 d) { g_pbus.Write32(a & ~3u, uint32_t(d) << ((a & 3) * 8), 0xffu << ((a & 3) * 8)); }
void PWr16(UINT32 a, UINT16 d) { g_pbus.Write32(a & ~3u, uint32_t(d) << ((a & 2) * 8), 0xffffu << ((a & 2) * 8)); }
void PWr32(UINT32 a, UINT32 d) { g_pbus.Write32(a, d, 0xffffffff); }

// the reference's bus (MAME's i960.cpp on tests/mame_ref/emu.h)
u32 RRead32(void* c, u32 a) { return ((Bus*)c)->Read32(a); }
void RWrite32(void* c, u32 a, u32 d, u32 m) { ((Bus*)c)->Write32(a, d, m); }
u16 RFlags(void* c, u32 a) { return ((Bus*)c)->Flags(a); }

// ---- a small assembler -----------------------------------------------------------------------------------------------
enum { R_PFP = 0, R_SP = 1, R_RIP = 2, G0 = 16, FP = 31 };
uint32_t Reg(uint32_t major, uint32_t minor, int dst, int s1, int s2, bool lit1 = false, bool lit2 = false, bool m3 = false)
{
	return (major << 24) | (uint32_t(dst & 31) << 19) | (uint32_t(s2 & 31) << 14) | (m3 ? 0x2000 : 0) | (lit2 ? 0x1000 : 0) |
		(lit1 ? 0x800 : 0) | (minor << 7) | uint32_t(s1 & 31);
}
uint32_t Ctrl(uint32_t op, int32_t disp) { return (op << 24) | (uint32_t(disp) & 0xffffff); }
uint32_t Cobr(uint32_t op, int s1, int s2, int32_t disp, bool lit1 = false)
{
	return (op << 24) | (uint32_t(s1 & 31) << 19) | (uint32_t(s2 & 31) << 14) | (lit1 ? 0x2000 : 0) | (uint32_t(disp) & 0x1ffc);
}
uint32_t Mema(uint32_t op, int reg, int abase, uint32_t offset, bool use_abase = true)
{
	return (op << 24) | (uint32_t(reg & 31) << 19) | (uint32_t(abase & 31) << 14) | (use_abase ? 0x2000 : 0) | (offset & 0xfff);
}
uint32_t Memb(uint32_t op, int reg, int abase, uint32_t mode, uint32_t scale = 0, int index = 0)
{
	return (op << 24) | (uint32_t(reg & 31) << 19) | (uint32_t(abase & 31) << 14) | 0x1000 | (mode << 10) | (scale << 7) |
		uint32_t(index & 31);
}

struct Asm
{
	std::vector<uint32_t> w;
	uint32_t base;
	explicit Asm(uint32_t b) : base(b) { }
	uint32_t Here() const { return base + 4 * uint32_t(w.size()); }
	void Emit(uint32_t x) { w.push_back(x); }
	// a branch to `target` from here (b/call/bal: 24-bit displacement from this instruction)
	void B(uint32_t op, uint32_t target) { Emit(Ctrl(op, int32_t(target - Here()))); }
	void Mov(int dst, int src) { Emit(Reg(0x5c, 0xc, dst, src, 0)); }
	void Lit(int dst, uint32_t v) { Emit(Reg(0x5c, 0xc, dst, v & 31, 0, true)); } // mov literal (0..31)
	// lda: a 32-bit constant through MEMB mode 0xc (displacement word)
	void Const(int dst, uint32_t v) { Emit(Memb(0x8c, dst, 0, 0xc)); Emit(v); }
	void Ld(uint32_t op, int reg, int abase, uint32_t off) { Emit(Mema(op, reg, abase, off)); }
	void Halt() { Emit(Ctrl(0x08, 0)); } // b . (spins)
	void Put(std::vector<uint8_t>& ram) const { memcpy(&ram[base], w.data(), 4 * w.size()); }
};

// the processor's tables: SAT at 0x200 (system procedure table at 0x300), PRCB at 0x100, interrupt table at 0x400
// (handlers at 0x6000 + 0x40 * n for vector 8+n... only the vectors set), the interrupt stack and first frame at 0x40000
constexpr uint32_t kSat = 0x200, kPrcb = 0x100, kIntTab = 0x400, kSysProc = 0x300, kCode = 0x1000, kData = 0x10000;
constexpr uint32_t kStack = 0x40000;

void Tables(std::vector<uint8_t>& ram, uint32_t start)
{
	auto put = [&](uint32_t a, uint32_t v) { memcpy(&ram[a], &v, 4); };
	put(0, kSat);
	put(4, kPrcb);
	put(12, start);
	put(kPrcb + 20, kIntTab);
	put(kPrcb + 24, kStack);
	put(kSat + 152, kSysProc);
}

struct I960Regs
{
	uint32_t r[32], ac, pc, ip, icr, rcache[4][16], rcache_addr[4];
	int32_t rcache_pos;
	double fp[4];
	int immediate_irq;
};

template <typename C> I960Regs GetRegs(C* c)
{
	I960Regs s;
	memcpy(s.r, c->m_r, sizeof(s.r));
	s.ac = c->m_AC;
	s.pc = c->m_PC;
	s.ip = c->m_IP;
	s.icr = c->m_ICR;
	memcpy(s.rcache, c->m_rcache, sizeof(s.rcache));
	memcpy(s.rcache_addr, c->m_rcache_frame_addr, sizeof(s.rcache_addr));
	s.rcache_pos = c->m_rcache_pos;
	memcpy(s.fp, c->m_fp, sizeof(s.fp));
	s.immediate_irq = c->m_immediate_irq;
	return s;
}

void PortMap()
{
	I960MapMemory(&g_pbus.ram[0], 0, kRam - 1, MAP_RAM, I960_BURST);
	I960SetReadByteHandler(1, PRd8);
	I960SetReadWordHandler(1, PRd16);
	I960SetReadLongHandler(1, PRd32);
	I960SetWriteByteHandler(1, PWr8);
	I960SetWriteWordHandler(1, PWr16);
	I960SetWriteLongHandler(1, PWr32);
	I960MapHandler(1, kFifo, kFifo + 0xfff, MAP_RAM, 0);
}

// runs the port's core on `ram` for `cycles`, with optional interrupt events (cycle, line, state)
struct IrqEvent { int at, line, state; };
void RunPort(const std::vector<uint8_t>& ram, int cycles, const std::vector<IrqEvent>& ev = {})
{
	g_pbus = Bus();
	g_pbus.ram = ram;
	g_pbus.port = true;
	I960Exit();
	I960Init();
	PortMap();
	I960Reset();
	int done = 0;
	for (size_t i = 0; i <= ev.size(); i++)
	{
		const int until = i < ev.size() ? ev[i].at : cycles;
		while (done < until)
			done += I960Run(until - done);
		if (i < ev.size())
			I960SetIRQLine(ev[i].line, ev[i].state);
	}
}

// the same on MAME's core
struct RefRun
{
	machine_config mc;
	i80960kb_device* cpu = nullptr;
	Bus bus;
	~RefRun() { delete cpu; }
};
void RunRef(RefRun& rr, const std::vector<uint8_t>& ram, int cycles, const std::vector<IrqEvent>& ev = {})
{
	rr.cpu = new i80960kb_device(rr.mc, "ref", nullptr, 25000000);
	rr.bus.ram = ram;
	rr.bus.port = false;
	rr.bus.ref = rr.cpu;
	test_space& sp = rr.cpu->space(AS_PROGRAM);
	sp.read32 = RRead32;
	sp.write32 = RWrite32;
	sp.flags = RFlags;
	sp.ctx = &rr.bus;
	rr.cpu->start();
	rr.cpu->reset();
	int done = 0;
	for (size_t i = 0; i <= ev.size(); i++)
	{
		const int until = i < ev.size() ? ev[i].at : cycles;
		while (done < until)
		{
			// MAME's scheduler: a timeslice, then execute_run again (which takes a pending interrupt first)
			const int slice = until - done;
			rr.cpu->m_icount = slice;
			rr.cpu->run();
			done += slice - rr.cpu->m_icount;
		}
		if (i < ev.size())
			rr.cpu->set_input(ev[i].line, ev[i].state);
	}
}

uint32_t PortWord(uint32_t a) { uint32_t v; memcpy(&v, &g_pbus.ram[a], 4); return v; }

// ---- known answers ---------------------------------------------------------------------------------------------------
// each program ends with "b ." at a known place; the checks look at the port core's registers and RAM
struct Kat
{
	const char* name;
	std::function<void(Asm&)> build;
	std::function<void(const I960Regs&)> check;
	int cycles = 4000;
};

void RunI960Kats()
{
	std::vector<Kat> kats;

	kats.push_back({"integer arithmetic and logic", [](Asm& a) {
		a.Const(4, 0x12345678); a.Const(5, 0x0000f00f);
		a.Emit(Reg(0x59, 0x0, 6, 4, 5));          // addo r6 = r5 + r4
		a.Emit(Reg(0x59, 0x2, 7, 5, 4));          // subo r7 = r4 - r5
		a.Emit(Reg(0x70, 0x1, 8, 4, 5));          // mulo
		a.Emit(Reg(0x70, 0xb, 9, 5, 4));          // divo r9 = r4 / r5
		a.Emit(Reg(0x70, 0x8, 10, 5, 4));         // remo
		a.Emit(Reg(0x58, 0x1, 11, 4, 5));         // and
		a.Emit(Reg(0x58, 0x7, 12, 4, 5));         // or
		a.Emit(Reg(0x58, 0x6, 13, 4, 5));         // xor
		a.Emit(Reg(0x59, 0xc, 14, 4, 5, true));   // shlo r14 = r5 << 4
		a.Emit(Reg(0x59, 0xd, 15, 8, 4, true));   // rotate r15 = rotl(r4, 8)
		a.Const(G0, 0xfffffff1);
		a.Emit(Reg(0x59, 0xb, G0 + 1, 2, G0, true)); // shri g1 = -15 >> 2 = -4
		a.Emit(Reg(0x59, 0xa, G0 + 2, 2, G0, true)); // shrdi g2 = -15 / 4 = -3 (rounds toward zero)
		a.Emit(Reg(0x74, 0xb, G0 + 3, 4, G0, true)); // divi g3 = -15 / 4 = -3
		a.Emit(Reg(0x74, 0x9, G0 + 4, 4, G0, true)); // modi g4 = -15 mod 4 = 1
		a.Emit(Reg(0x74, 0x8, G0 + 5, 4, G0, true)); // remi g5 = -15 rem 4 = -3
		a.Emit(Reg(0x64, 0x1, G0 + 6, 4, 0));        // scanbit g6 = 28
		a.Emit(Reg(0x64, 0x0, G0 + 7, 5, 0));        // spanbit g7 (first 0 from the top of 0xf00f) = 31
		a.Emit(Reg(0x58, 0x3, G0 + 8, 31, 5, true)); // setbit g8 = r5 | 1<<31
		a.Emit(Reg(0x58, 0xc, G0 + 9, 0, 5, true));  // clrbit g9 = r5 & ~1
		a.Halt();
	}, [](const I960Regs& s) {
		Check(s.r[6] == 0x12354687, "addo");
		Check(s.r[7] == 0x12346669, "subo");
		Check(s.r[8] == uint32_t(0x12345678u * 0xf00fu), "mulo");
		Check(s.r[9] == 0x12345678u / 0xf00f, "divo");
		Check(s.r[10] == 0x12345678u % 0xf00f, "remo");
		Check(s.r[11] == (0x12345678u & 0xf00f) && s.r[12] == (0x12345678u | 0xf00f) && s.r[13] == (0x12345678u ^ 0xf00f), "and/or/xor");
		Check(s.r[14] == 0xf00f0, "shlo");
		Check(s.r[15] == 0x34567812, "rotate");
		Check(s.r[G0 + 1] == 0xfffffffc, "shri");
		Check(s.r[G0 + 2] == 0xfffffffd, "shrdi");
		Check(s.r[G0 + 3] == 0xfffffffd, "divi");
		Check(s.r[G0 + 4] == 1, "modi");
		Check(s.r[G0 + 5] == 0xfffffffd, "remi");
		Check(s.r[G0 + 6] == 28, "scanbit");
		Check(s.r[G0 + 7] == 31, "spanbit");
		Check(s.r[G0 + 8] == 0x8000f00f && s.r[G0 + 9] == 0xf00e, "setbit/clrbit");
	}});

	kats.push_back({"64-bit add with carry (addc), subc", [](Asm& a) {
		a.Const(4, 0xffffffff); a.Const(5, 0x00000001); // r5:r4 = 0x1ffffffff
		a.Const(6, 0x00000001); a.Const(7, 0x00000002); // r7:r6 = 0x200000001
		a.Emit(Reg(0x5a, 0x0, 0, 4, 4));                 // cmpo r4, r4: equal -> condition 010 (carry clear)
		a.Emit(Reg(0x5b, 0x0, 8, 6, 4));                 // addc r8 = r4 + r6 + C
		a.Emit(Reg(0x5b, 0x0, 9, 7, 5));                 // addc r9 = r5 + r7 + C
		a.Emit(Reg(0x64, 0x5, 10, 0, 0, true, true));    // modac r10 = AC (mask 0: unchanged)
		a.Emit(Reg(0x5b, 0x2, 11, 6, 4));                // subc r11 = r4 - r6 - !C
		a.Halt();
	}, [](const I960Regs& s) {
		Check(s.r[8] == 0x00000000 && s.r[9] == 0x00000004, "addc chain gives 0x400000000");
		Check((s.r[10] & 7) == 0, "addc's condition code (no carry out of the top)");
		Check(s.r[11] == 0xfffffffe, "subc");
	}});

	kats.push_back({"compare and branch loop (cmpibl, bg, be)", [](Asm& a) {
		a.Lit(4, 0); a.Lit(5, 0);
		const uint32_t loop = a.Here();
		a.Emit(Reg(0x59, 0x0, 4, 3, 4, true));            // addo r4 += 3
		a.Emit(Reg(0x59, 0x0, 5, 1, 5, true));            // addo r5 += 1
		a.Emit(Cobr(0x3c, 10, 5, int32_t(loop - a.Here()), true)); // cmpibl 10 < r5 ? no: loop while 10 > r5 ... (r5 < 10)
		a.Emit(Reg(0x5a, 0x1, 0, 30, 4, true));           // cmpi 30, r4 -> equal
		a.B(0x12, a.Here() + 8);                          // be +8
		a.Lit(6, 1);                                      // (skipped)
		a.Lit(7, 7);
		a.Halt();
	}, [](const I960Regs& s) {
		Check(s.r[5] == 10 && s.r[4] == 30, "loop ran 10 times");
		Check(s.r[6] == 0 && s.r[7] == 7, "be taken");
	}});

	kats.push_back({"call/ret six deep through the register cache, flushreg", [](Asm& a) {
		// each level: r4 = its depth, then call the next; after the return, r5 = r4 * 10 (r4 kept by the frame)
		std::vector<uint32_t> fix;
		a.Lit(4, 0);
		const uint32_t call0 = a.Here();
		a.Emit(0); // call level 1 (patched)
		a.Emit(Reg(0x70, 0x1, 5, 10, 4, true)); // mulo r5 = r4 * 10
		a.Mov(G0, 5);
		a.Halt();
		std::vector<uint32_t> level(7);
		std::vector<uint32_t> calls(7);
		for (int d = 1; d <= 6; d++)
		{
			level[d] = a.Here();
			a.Lit(4, d);
			calls[d] = a.Here();
			a.Emit(0); // call d+1 (patched) or flushreg at the bottom
			a.Emit(Reg(0x70, 0x1, 5, 10, 4, true));
			a.Emit(Reg(0x59, 0x0, G0 + d, 0, 5, true)); // g[d] = r5 (+0)
			a.Emit(Ctrl(0x0a, 0)); // ret
		}
		a.w[(call0 - a.base) / 4] = Ctrl(0x09, int32_t(level[1] - call0));
		for (int d = 1; d < 6; d++)
			a.w[(calls[d] - a.base) / 4] = Ctrl(0x09, int32_t(level[d + 1] - calls[d]));
		a.w[(calls[6] - a.base) / 4] = Reg(0x66, 0xd, 0, 0, 0); // flushreg at the deepest level
	}, [](const I960Regs& s) {
		bool ok = true;
		for (int d = 1; d <= 6; d++)
			ok = ok && s.r[G0 + d] == uint32_t(d * 10);
		Check(ok, "each frame kept its r4 (g1..g6 = 10..60): " + Hex(s.r[G0 + 1]) + " " + Hex(s.r[G0 + 6]));
		Check(s.r[G0] == 0, "the first frame's r4 back (0)");
		Check(s.rcache_pos == 0, "back to the top frame");
	}});

	kats.push_back({"loads and stores of every size, MEMB addressing", [](Asm& a) {
		a.Const(G0 + 12, kData);
		a.Const(4, 0x8899aabb);
		a.Ld(0x92, 4, G0 + 12, 0);                   // st r4, 0(g12)
		a.Ld(0x80, 5, G0 + 12, 0);                   // ldob r5 = 0xbb
		a.Ld(0xc0, 6, G0 + 12, 0);                   // ldib r6 = -0x45
		a.Ld(0x88, 7, G0 + 12, 2);                   // ldos r7 = 0x8899
		a.Ld(0xc8, 8, G0 + 12, 2);                   // ldis r8 = sign-extended 0x8899
		a.Ld(0x82, 4, G0 + 12, 9);                   // stob 0xbb at +9
		a.Ld(0x8a, 4, G0 + 12, 14);                  // stos 0xaabb at +14
		a.Lit(3, 2);
		a.Emit(Memb(0x90, 9, G0 + 12, 0x7, 2, 3));   // ld r9 = (g12 + r3 << 2) = word at +8
		a.Emit(Memb(0x90, 10, G0 + 12, 0xd)); a.Emit(12); // ld r10 = 12(g12)
		a.Const(12, 0x11111111); a.Const(13, 0x22222222); a.Const(14, 0x33333333); a.Const(15, 0x44444444);
		a.Ld(0xb2, 12, G0 + 12, 0x40);               // stq r12-r15 at +0x40 (BURST: four words)
		a.Ld(0xb0, G0 + 4, G0 + 12, 0x40);           // ldq g4-g7
		a.Ld(0x98, G0 + 8, G0 + 12, 0x44);           // ldl g8-g9 from +0x44
		a.Ld(0xa0, 12, G0 + 12, 0x48);               // ldt r12-r14 from +0x48 (0x33.., 0x44.., 0)
		a.Halt();
	}, [](const I960Regs& s) {
		Check(s.r[5] == 0xbb && s.r[6] == 0xffffffbb, "ldob/ldib");
		Check(s.r[7] == 0x8899 && s.r[8] == 0xffff8899, "ldos/ldis");
		Check(PortWord(kData + 8) == 0x0000bb00, "stob");
		Check(PortWord(kData + 12) == 0xaabb0000, "stos");
		Check(s.r[9] == 0x0000bb00, "MEMB abase + index << scale");
		Check(s.r[10] == 0xaabb0000, "MEMB displacement + abase");
		Check(s.r[G0 + 4] == 0x11111111 && s.r[G0 + 5] == 0x22222222 && s.r[G0 + 6] == 0x33333333 && s.r[G0 + 7] == 0x44444444, "stq/ldq (BURST)");
		Check(s.r[G0 + 8] == 0x22222222 && s.r[G0 + 9] == 0x33333333, "ldl");
		Check(s.r[12] == 0x33333333 && s.r[13] == 0x44444444 && s.r[14] == 0, "ldt");
	}});

	kats.push_back({"ldq/stq on a FIFO page (no BURST): one address", [](Asm& a) {
		a.Const(G0 + 13, kFifo);
		a.Ld(0xb0, 4, G0 + 13, 0);                   // ldq r4-r7 from the FIFO: 0x1000..0x1003
		a.Const(8, 0xa); a.Const(9, 0xb); a.Const(10, 0xc); a.Const(11, 0xd);
		a.Ld(0xb2, 8, G0 + 13, 0);                   // stq r8-r11 to the FIFO
		a.Ld(0x98, 12, G0 + 13, 4);                  // ldl r12-r13: 0x1004, 0x1005
		a.Halt();
	}, [](const I960Regs& s) {
		Check(s.r[4] == 0x1000 && s.r[5] == 0x1001 && s.r[6] == 0x1002 && s.r[7] == 0x1003, "ldq from the FIFO: 4 reads of one address");
		Check(g_pbus.fifo_log.size() == 4 && g_pbus.fifo_log[0] == 0xa && g_pbus.fifo_log[3] == 0xd, "stq to the FIFO: 4 writes");
		Check(s.r[12] == 0x1004 && s.r[13] == 0x1005, "ldl from the FIFO");
	}});

	kats.push_back({"a stalled FIFO read in the middle of an ldq is resumed", [](Asm& a) {
		a.Const(G0 + 13, kFifo + 0x100);
		a.Ld(0xb0, 4, G0 + 13, 0);                   // ldq r4-r7 from the slow FIFO: each word stalls once
		a.Lit(8, 9);
		a.Halt();
	}, [](const I960Regs& s) {
		Check(s.r[4] == 0x1000 && s.r[5] == 0x1001 && s.r[6] == 0x1002 && s.r[7] == 0x1003, "ldq resumed after stalls: " + Hex(s.r[4]) + " " + Hex(s.r[7]));
		Check(g_pbus.stalls == 4, "four stalls");
		Check(s.r[8] == 9, "the next instruction ran once");
	}});

	kats.push_back({"system call (calls through the SAT's procedure table)", [](Asm& a) {
		const uint32_t start = a.Here();
		a.Lit(4, 1);
		a.Emit(Reg(0x66, 0x0, 0, 2, 0, true));       // calls 2
		a.Mov(G0, 4);
		a.Halt();
		const uint32_t proc = a.Here();
		a.Lit(4, 5); a.Lit(G0 + 1, 22);
		a.Emit(Ctrl(0x0a, 0));
		(void)start;
		// the procedure table entry 2 (+48 + 8): written by the test's RAM setup below
		a.Emit(proc); // (kept here: the test copies it to kSysProc + 48 + 8)
	}, [](const I960Regs& s) {
		Check(s.r[G0 + 1] == 22, "the system procedure ran");
		Check(s.r[G0] == 1, "r4 back after the return");
	}});

	kats.push_back({"interrupt: vector from ICR, handler, return restores PC and AC", [](Asm& a) {
		// ICR = vector 0x48 for line 0 (priority 9), set with synmov to 0xff000004
		a.Const(4, 0x00000048); a.Const(5, 0xff000004);
		a.Const(6, kData + 0x100); a.Ld(0x92, 4, 6, 0);
		a.Emit(Reg(0x60, 0x0, 0, 5, 6));             // synmov (r5 = 0xff000004) <- (r6)
		// lower the priority to 0 (modpc: PC priority bits 16-20)
		a.Const(7, 0x001f0000); a.Lit(8, 0);
		a.Emit(Reg(0x65, 0x5, 8, 0, 7));             // modpc r8, mask r7 -> priority 0
		a.Emit(Reg(0x5a, 0x1, 0, 1, 3, true));       // cmpi 1, r3 (AC = something to keep)
		const uint32_t spin = a.Here();
		a.Emit(Reg(0x59, 0x0, 9, 1, 9, true));       // addo r9 += 1 (counts while waiting)
		a.Emit(Cobr(0x32, 0, G0 + 3, int32_t(spin - a.Here()), true)); // cmpobe 0, g3: loop until the handler set g3
		a.Emit(Reg(0x64, 0x5, 10, 0, 0, true, true)); // modac r10 = AC
		a.Halt();
		// handler at kCode + 0x400
		while (a.Here() < kCode + 0x400) a.Emit(Reg(0x5c, 0xc, 4, 4, 0)); // padding (mov r4, r4)
		a.Lit(G0 + 3, 1);
		a.Emit(Reg(0x5a, 0x0, 0, 4, 5));             // cmpo: change AC inside the handler
		a.Emit(Ctrl(0x0a, 0));                       // ret (type 7: PC and AC back)
	}, [](const I960Regs& s) {
		Check(s.r[G0 + 3] == 1, "the handler ran");
		Check(s.r[9] > 0, "the spin loop ran before it");
		Check((s.r[10] & 7) == 4, "the loop's last compare (0 < g3) after the return: " + Hex(s.r[10]));
		Check(s.pc == 0x00002002, "PC back after the return (priority 0, supervisor): " + Hex(s.pc));
	}, 6000});

	kats.push_back({"floating point (KB): cvtir, addr, mulr, divr, sqrtr, cmpr, cvtri, movrl", [](Asm& a) {
		a.Lit(4, 9);
		a.Emit(Reg(0x67, 0x4, 0, 4, 0, false, false, true));    // cvtir fp0 = 9.0
		a.Emit(Reg(0x68, 0x8, 1, 0, 0, true, false, true));     // sqrtr fp1 = sqrt(fp0) = 3 (src1 literal 0 = fp0)
		a.Emit(Reg(0x78, 0xc, 2, 1, 0, true, true, true));      // mulr fp2 = fp0 * fp1 = 27
		a.Emit(Reg(0x78, 0xf, 3, 0x16, 2, true, true, true));   // addr fp3 = fp2 + 1.0 = 28
		a.Emit(Reg(0x78, 0xb, 3, 0x16, 3, true, true, true));   // divr fp3 = fp3 / 1.0
		a.Emit(Reg(0x6c, 0x0, 5, 3, 0, true));                  // cvtri r5 = 28
		a.Emit(Reg(0x68, 0x5, 0, 2, 3, true, true));            // cmpr fp2, fp3: 28 vs 27 -> (src1 > src2?)
		a.Emit(Reg(0x64, 0x5, 6, 0, 0, true, true));            // modac r6 = AC
		a.Emit(Reg(0x78, 0xc, 7, 1, 1, true, true, false));     // mulr r7 = fp1 * fp1 = 9.0f (single, in a register)
		a.Emit(Reg(0x6d, 0x9, 8, 1, 0, true));                  // movrl r8:r9 = fp1 (3.0 double)
		a.Halt();
	}, [](const I960Regs& s) {
		Check(s.fp[0] == 9.0 && s.fp[1] == 3.0 && s.fp[2] == 27.0 && s.fp[3] == 28.0, "fp registers 9 3 27 28");
		Check(s.r[5] == 28, "cvtri");
		Check((s.r[6] & 7) == 4, "cmpr: src1 (27) < src2 (28) -> 100");
		float f; memcpy(&f, &s.r[7], 4);
		Check(f == 9.0f, "mulr to a register (single)");
		uint64_t d = s.r[8] | (uint64_t(s.r[9]) << 32); double dd; memcpy(&dd, &d, 8);
		Check(dd == 3.0, "movrl to a register pair");
	}});

	int n = 0;
	for (const Kat& k : kats)
	{
		Asm a(kCode);
		k.build(a);
		std::vector<uint8_t> ram(kRam, 0);
		Tables(ram, kCode);
		a.Put(ram);
		if (std::string(k.name).find("system call") == 0)
		{
			const uint32_t proc = a.w.back();
			memcpy(&ram[kSysProc + 48 + 8], &proc, 4);
		}
		if (std::string(k.name).find("interrupt") == 0)
		{
			// vector 0x48: interrupt table + 36 + (0x48 - 8) * 4
			const uint32_t h = kCode + 0x400;
			memcpy(&ram[kIntTab + 36 + (0x48 - 8) * 4], &h, 4);
		}
		const int before = g_failed;
		if (std::string(k.name).find("interrupt") == 0)
			RunPort(ram, k.cycles, {{1500, I960_IRQ0, 1}, {1600, I960_IRQ0, 0}});
		else
			RunPort(ram, k.cycles);
		k.check(GetRegs(I960Core()));
		// the same program on MAME's core must end the same
		RefRun rr;
		if (std::string(k.name).find("interrupt") == 0)
			RunRef(rr, ram, k.cycles, {{1500, 0, 1}, {1600, 0, 0}});
		else
			RunRef(rr, ram, k.cycles);
		const I960Regs p = GetRegs(I960Core()), r = GetRegs(rr.cpu);
		Check(memcmp(&p, &r, sizeof(p)) == 0 && g_pbus.ram == rr.bus.ram, std::string(k.name) + ": the port's core and MAME's end the same");
		printf("  %s: i960 %s\n", g_failed == before ? "ok" : "FAIL", k.name);
		n++;
	}
	printf("i960: %d known-answer programs\n", n);
}

// ---- random programs: the port's core against MAME's ------------------------------------------------------------------
struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 2654435761u + 1) { }
	uint32_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); }
	uint32_t Below(uint32_t n) { return Next() % n; }
};

const int kDst[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
int RDst(Rng& r) { return kDst[r.Below(sizeof(kDst) / sizeof(kDst[0]))]; }
int RSrc(Rng& r) { return r.Below(5) == 0 ? 3 : RDst(r); } // r3: a small index (never written)

void RandomOp(Rng& r, Asm& a, uint32_t block_end, int depth, const std::vector<uint32_t>& subs)
{
	const uint32_t k = r.Below(100);
	const bool l1 = r.Below(4) == 0, l2 = r.Below(6) == 0;
	const int d = RDst(r), s1 = l1 ? int(r.Below(32)) : RSrc(r), s2 = l2 ? int(r.Below(32)) : RSrc(r);
	if (k < 30)
	{
		static const uint32_t ops58[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf};
		static const uint32_t ops59[] = {0x0, 0x1, 0x2, 0x3, 0x8, 0xa, 0xb, 0xc, 0xd, 0xe};
		if (r.Below(2)) a.Emit(Reg(0x58, ops58[r.Below(15)], d, s1, s2, l1, l2));
		else a.Emit(Reg(0x59, ops59[r.Below(10)], d, s1, s2, l1, l2));
	}
	else if (k < 40)
	{
		static const uint32_t ops5a[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xc, 0xe};
		a.Emit(Reg(0x5a, ops5a[r.Below(10)], d, s1, s2, l1, l2));
	}
	else if (k < 45)
		a.Emit(Reg(0x5b, r.Below(2) ? 0x0 : 0x2, d, s1, s2, l1, l2));
	else if (k < 50)
	{
		// multiply / divide: the divisor a literal 1..31 (MAME's code divides by zero, which stops a process)
		static const uint32_t m70[] = {0x1, 0x8, 0xb}, m74[] = {0x1, 0x8, 0x9, 0xb};
		const int lit = 1 + int(r.Below(31));
		if (r.Below(2)) a.Emit(Reg(0x70, m70[r.Below(3)], d, lit, s2, true, l2));
		else a.Emit(Reg(0x74, m74[r.Below(4)], d, lit, s2, true, l2));
	}
	else if (k < 53)
	{
		const int de = d & ~1;
		a.Emit(Reg(0x67, 0x0, de, s1, s2, l1, l2)); // emul
	}
	else if (k < 56)
		a.Emit(Reg(0x64, r.Below(2), d, s1, 0, l1));  // spanbit / scanbit
	else if (k < 58)
		a.Emit(Reg(0x5c + r.Below(2), 0xc, d & ~1, s1 & ~1, 0, l1)); // mov / movl
	else if (k < 60)
		a.Emit(Reg(0x20 + r.Below(8), 0, d, 0, 0) & 0xfff80000); // testxx
	else if (k < 70)
	{
		// floating point: fp registers (dst m3 + 0..3) or g/r registers; literals 0..3 = fp0..fp3, 0x16 = 1.0
		static const uint32_t f78[] = {0xb, 0xc, 0xd, 0xf};
		static const uint32_t f68[] = {0x0, 0x2, 0x3, 0x5, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe};
		const bool fpdst = r.Below(2);
		const int fd = fpdst ? int(r.Below(4)) : d;
		const int fs1 = l1 ? (r.Below(5) == 4 ? 0x16 : int(r.Below(4))) : s1;
		const int fs2 = l2 ? (r.Below(5) == 4 ? 0x16 : int(r.Below(4))) : s2;
		const uint32_t w = r.Below(3);
		if (w == 0) a.Emit(Reg(0x78, f78[r.Below(4)], fd, fs1, fs2, l1, l2, fpdst));
		else if (w == 1) a.Emit(Reg(0x68, f68[r.Below(11)], fd, fs1, fs2, l1, l2, fpdst));
		else
		{
			static const uint32_t c67[] = {0x4, 0x7};
			static const uint32_t c6c[] = {0x0, 0x2, 0x9};
			if (r.Below(2)) a.Emit(Reg(0x67, c67[r.Below(2)], fd, s1, fs2, l1, l2, fpdst));
			else a.Emit(Reg(0x6c, c6c[r.Below(3)], (c6c[0] == 0 ? d : d), fs1, 0, l1, false, false));
		}
	}
	else if (k < 85)
	{
		// loads and stores around g28 (the data area), on the FIFO through g29
		static const uint32_t ld[] = {0x80, 0x82, 0x88, 0x8a, 0x90, 0x92, 0x98, 0x9a, 0xa0, 0xa2, 0xb0, 0xb2, 0xc0, 0xc2, 0xc8, 0xca, 0x8c};
		uint32_t op = ld[r.Below(17)];
		int reg = d;
		if (op == 0x98 || op == 0x9a) reg &= ~1;
		if (op == 0xa0 || op == 0xa2 || op == 0xb0 || op == 0xb2) reg &= ~3;
		if (reg < 4) reg = 4; // never r0-r3 (pfp, sp, rip, the index)
		const bool fifo = r.Below(8) == 0;
		const int base = fifo ? 29 : 28;
		const uint32_t off = fifo ? 0 : (r.Below(0x400) & ~3u) | (r.Below(4) == 0 ? r.Below(4) : 0);
		if (r.Below(4) == 0 && !fifo)
		{
			a.Emit(Memb(op, reg, base, 0x7, r.Below(3), 3)); // abase + r3 << scale
		}
		else
			a.Emit(Mema(op, reg, base, off));
	}
	else if (k < 92)
	{
		// a forward branch inside the block (to one of the next few words, never past its end)
		const uint32_t here = a.Here();
		uint32_t target = here + 4 * (1 + r.Below(4));
		if (target > block_end) target = block_end;
		const uint32_t c = r.Below(3);
		if (c == 0) a.Emit(Ctrl(0x10 + r.Below(8), int32_t(target - here))); // bno..bo
		else if (c == 1) a.Emit(Cobr(0x30 + r.Below(7), l1 ? int(r.Below(32)) : RSrc(r), RSrc(r), int32_t(target - here), l1));
		else a.Emit(Cobr(0x39 + r.Below(6), l1 ? int(r.Below(32)) : RSrc(r), RSrc(r), int32_t(target - here), l1));
	}
	else if (k < 96 && depth < int(subs.size()))
		a.Emit(Ctrl(0x09, int32_t(subs[depth] - a.Here()))); // call the next level down
	else if (k < 97)
		a.Emit(Reg(0x66, 0xd, 0, 0, 0)); // flushreg
	else
		a.Emit(Reg(0x64, 0x5, d, s1, s2, l1, l2)); // modac
}

void RunI960Random()
{
	int same = 0;
	const int kPrograms = 40;
	for (int seed = 1; seed <= kPrograms; seed++)
	{
		Rng r(seed);
		std::vector<uint8_t> ram(kRam, 0);
		for (uint32_t i = kData; i < kData + 0x2000; i++) ram[i] = uint8_t(r.Next());
		Tables(ram, kCode);
		// six levels of subroutines at kCode + 0x1000 * level; the main block at kCode
		const int kLevels = 6, kOps = 40;
		std::vector<uint32_t> subs;
		for (int l = 1; l <= kLevels; l++) subs.push_back(kCode + 0x1000 * l);
		for (int l = 0; l <= kLevels; l++)
		{
			Asm a(kCode + 0x1000 * l);
			if (l == 0)
			{
				a.Const(28, kData); a.Const(29, kFifo); a.Lit(3, r.Below(8));
				for (int g = 4; g <= 27; g++) a.Const(g, r.Next());
			}
			const uint32_t end = a.Here() + 4 * kOps * 3; // the block's last word (an op may be two words)
			while (a.Here() < end - 8)
				RandomOp(r, a, end - 8, l, subs);
			while (a.Here() < end) a.Emit(Reg(0x5c, 0xc, 4, 4, 0)); // mov r4, r4 (padding)
			if (l == 0) a.Halt();
			else a.Emit(Ctrl(0x0a, 0)); // ret
			a.Put(ram);
		}
		// interrupt vectors for lines 0 and 2 (ICR's reset value 0xff000000 gives line 3 vector 0xff only)
		const uint32_t h = kCode + 0x8000;
		{
			Asm a(h);
			a.Emit(Reg(0x59, 0x0, 27, 1, 27, true)); // g11 += 1
			a.Emit(Ctrl(0x0a, 0));
			a.Put(ram);
			memcpy(&ram[kIntTab + 36 + (0xff - 8) * 4], &h, 4);
		}
		std::vector<IrqEvent> ev;
		if (seed % 3 == 0)
		{
			ev.push_back({int(200 + r.Below(800)), I960_IRQ3, 1});
			ev.push_back({ev.back().at + 50, I960_IRQ3, 0});
		}
		const int cycles = 6000;
		RunPort(ram, cycles, ev);
		RefRun rr;
		bool threw = false;
		try { RunRef(rr, ram, cycles, ev); } catch (const std::exception& e) { threw = true; printf("    (MAME's core stopped: %s)\n", e.what()); }
		const I960Regs p = GetRegs(I960Core());
		const I960Regs q = threw ? p : GetRegs(rr.cpu);
		const bool eq = !threw && memcmp(&p, &q, sizeof(p)) == 0 && g_pbus.ram == rr.bus.ram && g_pbus.fifo_log == rr.bus.fifo_log;
		if (eq) same++;
		else
		{
			printf("  FAIL: i960 random program %d differs", seed);
			for (int i = 0; i < 32; i++) if (p.r[i] != q.r[i]) printf(" r%d %x/%x", i, p.r[i], q.r[i]);
			if (p.ac != q.ac) printf(" ac %x/%x", p.ac, q.ac);
			if (p.ip != q.ip) printf(" ip %x/%x", p.ip, q.ip);
			if (g_pbus.ram != rr.bus.ram) printf(" (memory)");
			printf("\n");
			g_failed++;
		}
		printf("random i960 %02d: regs %016llx mem %016llx%s\n", seed, (unsigned long long)Fnv(&p, sizeof(p)),
			(unsigned long long)Fnv(g_pbus.ram.data(), g_pbus.ram.size()), ev.empty() ? "" : " (interrupt)");
	}
	if (same == kPrograms) g_passed++;
	printf("  %s: i960: %d of %d random programs end the same on the port's core and MAME's\n", same == kPrograms ? "ok" : "FAIL", same, kPrograms);
}

// =====================================================================================================================
// MB86234 (TGP)
// =====================================================================================================================
// program 4K words; data 0x000-0x0ff and 0x200-0x3ff; io: reads give a function of the address, writes are logged;
// rf1 reads a FIFO (0x40000000, 0x40000001...), rf2 writes are logged
struct TgpBus
{
	uint32_t program[0x1000];
	uint32_t data[0x400];
	uint32_t fifo_next = 0x40000000;
	std::vector<uint32_t> io_log, rf_log;
	uint32_t ProgramRead(uint32_t a) { return a < 0x1000 ? program[a] : 0; }
	uint32_t DataRead(uint32_t a) { return (a < 0x100 || (a >= 0x200 && a < 0x400)) ? data[a] : 0; }
	void DataWrite(uint32_t a, uint32_t d) { if (a < 0x100 || (a >= 0x200 && a < 0x400)) data[a] = d; }
	uint32_t IoRead(uint32_t a) { return 0x3f800000 ^ (a * 0x9e3779b1u); }
	void IoWrite(uint32_t a, uint32_t d) { io_log.push_back(a); io_log.push_back(d); }
	uint32_t RfRead(uint32_t a) { return a == 1 ? fifo_next++ : 0; }
	void RfWrite(uint32_t a, uint32_t d) { rf_log.push_back(a); rf_log.push_back(d); }
};

TgpBus g_tbus;
UINT32 TDataR(UINT32 a) { return g_tbus.DataRead(a); }
void TDataW(UINT32 a, UINT32 d) { g_tbus.DataWrite(a, d); }
UINT32 TIoR(UINT32 a) { return g_tbus.IoRead(a); }
void TIoW(UINT32 a, UINT32 d) { g_tbus.IoWrite(a, d); }
UINT32 TRfR(UINT32 a) { return g_tbus.RfRead(a); }
void TRfW(UINT32 a, UINT32 d) { g_tbus.RfWrite(a, d); }

u32 TRProg(void* c, u32 a) { return ((TgpBus*)c)->ProgramRead(a); }
u32 TRData(void* c, u32 a) { return ((TgpBus*)c)->DataRead(a); }
void TWData(void* c, u32 a, u32 d, u32) { ((TgpBus*)c)->DataWrite(a, d); }
u32 TRIo(void* c, u32 a) { return ((TgpBus*)c)->IoRead(a); }
void TWIo(void* c, u32 a, u32 d, u32) { ((TgpBus*)c)->IoWrite(a, d); }
u32 TRRf(void* c, u32 a) { return ((TgpBus*)c)->RfRead(a); }
void TWRf(void* c, u32 a, u32 d, u32) { ((TgpBus*)c)->RfWrite(a, d); }
u16 TNoFlags(void*, u32) { return 0; }

struct TgpRegs
{
	u32 st, a, b, d, p;
	u16 pc, sp, b0, b1, x0, x1, i0, i1, vsmr, pcs[4], mask, m;
	u8 r, rpc, c0, c1, sft, vsm;
};
template <typename C> TgpRegs TGet(C* c)
{
	TgpRegs s;
	memset(&s, 0, sizeof(s));
	s.st = c->m_st; s.a = c->m_a; s.b = c->m_b; s.d = c->m_d; s.p = c->m_p;
	s.pc = c->m_pc; s.sp = c->m_sp; s.b0 = c->m_b0; s.b1 = c->m_b1; s.x0 = c->m_x0; s.x1 = c->m_x1; s.i0 = c->m_i0;
	s.i1 = c->m_i1; s.vsmr = c->m_vsmr; memcpy(s.pcs, c->m_pcs, sizeof(s.pcs)); s.mask = c->m_mask; s.m = c->m_m;
	s.r = c->m_r; s.rpc = c->m_rpc; s.c0 = c->m_c0; s.c1 = c->m_c1; s.sft = c->m_sft; s.vsm = c->m_vsm;
	return s;
}

void TgpPortRun(const TgpBus& init, int cycles)
{
	g_tbus = init;
	Mb86233Exit();
	Mb86233Init();
	Mb86233MapMemory(MB86233_PROGRAM, g_tbus.program, 0, 0xfff, MAP_READ);
	Mb86233MapMemory(MB86233_DATA, g_tbus.data, 0, 0xff, MAP_READ | MAP_WRITE);
	Mb86233MapMemory(MB86233_DATA, g_tbus.data + 0x200, 0x200, 0x3ff, MAP_READ | MAP_WRITE);
	Mb86233SetReadHandler(MB86233_DATA, TDataR);
	Mb86233SetWriteHandler(MB86233_DATA, TDataW);
	Mb86233SetReadHandler(MB86233_IO, TIoR);
	Mb86233SetWriteHandler(MB86233_IO, TIoW);
	Mb86233SetRfHandlers(TRfR, TRfW);
	Mb86233Reset();
	int done = 0;
	while (done < cycles)
		done += Mb86233Run(cycles - done);
}

struct TgpRef
{
	machine_config mc;
	mb86234_device* cpu = nullptr;
	TgpBus bus;
	~TgpRef() { delete cpu; }
};

void TgpRefRun(TgpRef& t, const TgpBus& init, int cycles)
{
	t.bus = init;
	t.cpu = new mb86234_device(t.mc, "tgp", nullptr, 50000000);
	auto bind = [&](int n, u32 (*rd)(void*, u32), void (*wr)(void*, u32, u32, u32)) {
		test_space& s = t.cpu->space(n);
		s.read32 = rd; s.write32 = wr; s.flags = TNoFlags; s.ctx = &t.bus;
	};
	bind(AS_PROGRAM, TRProg, nullptr);
	bind(AS_DATA, TRData, TWData);
	bind(AS_IO, TRIo, TWIo);
	bind(mb86233_device::AS_RF, TRRf, TWRf);
	t.cpu->start();
	t.cpu->reset();
	int done = 0;
	while (done < cycles)
	{
		const int slice = cycles - done;
		t.cpu->m_icount = slice;
		t.cpu->run();
		done += slice - t.cpu->m_icount;
	}
}

// the TGP's instruction words (MAME's decoding in execute_run)
uint32_t TLdi(int reg, int32_t v) { return (0x10u << 26) | (uint32_t(reg & 0x3f) << 24) | (uint32_t(v) & 0xffffff); }
uint32_t TMovRegReg(int from, int to, int alu = 0) { return (0x07u << 26) | (uint32_t(alu) << 21) | (7u << 18) | ((6u << 6 | uint32_t(to & 0x3f)) << 9) | uint32_t(from & 0x3f); }
uint32_t TMovMemReg(int addr, int to) { return (0x07u << 26) | (7u << 18) | ((3u << 6 | uint32_t(to & 0x3f)) << 9) | uint32_t(addr & 0x7f); } // mov mem (direct), reg
uint32_t TMovRegMem(int from, int addr) { return (0x07u << 26) | (7u << 18) | ((0u << 6 | uint32_t(from & 0x3f)) << 9) | uint32_t(addr & 0x7f); } // mov reg, mem
uint32_t TLab(int r1, int r2, int alu, int op) { return (0x00u << 26) | (uint32_t(alu) << 21) | (uint32_t(op) << 18) | (uint32_t(r2 & 0x1ff) << 9) | uint32_t(r1 & 0x1ff); }
uint32_t TBranch(int cond, int subtype, uint32_t data, bool invert = false) { return (0x2fu << 26) | (invert ? 0x40000000 : 0) | (uint32_t(cond) << 20) | (uint32_t(subtype) << 17) | (data & 0xffff); }
uint32_t TStmh(uint32_t m) { return (0x0du << 26) | (5u << 17) | (m & 0xffff); }

void RunTgpKats()
{
	// 1. float ALU: a = 2.5, b = 4.0 -> fml (p = a*b = 10), d = 1.5, fadd d = d + a = 4.0, fsbd; then the integer ALU
	{
		TgpBus b = TgpBus();
		memset(b.program, 0, sizeof(b.program));
		memset(b.data, 0, sizeof(b.data));
		auto f = [](float x) { uint32_t u; memcpy(&u, &x, 4); return u; };
		b.data[0x10] = f(2.5f); b.data[0x11] = f(4.0f); b.data[0x12] = f(1.5f);
		b.data[0x20] = 7; b.data[0x21] = 5;
		int pc = 0;
		b.program[pc++] = TStmh(1);                       // floating point mode
		b.program[pc++] = TMovMemReg(0x10, 0x10);         // a = data[0x10]
		b.program[pc++] = TMovMemReg(0x11, 0x13);         // b = data[0x11]
		b.program[pc++] = TMovMemReg(0x12, 0x19);         // d = data[0x12]
		b.program[pc++] = TMovRegReg(0x10, 0x10, 0x08);   // (a = a) with alu fml: p = a * b
		b.program[pc++] = TMovRegReg(0x10, 0x10, 0x06);   // alu fadd: d = d + a = 4.0
		b.program[pc++] = TMovRegMem(0x19, 0x30);         // data[0x30] = d
		b.program[pc++] = TMovRegMem(0x1c, 0x31);         // data[0x31] = p
		b.program[pc++] = TMovRegReg(0x10, 0x10, 0x07);   // fsbd: d = d - a = 1.5
		b.program[pc++] = TMovRegMem(0x19, 0x32);
		b.program[pc++] = TMovMemReg(0x20, 0x10);         // a = 7
		b.program[pc++] = TMovMemReg(0x21, 0x19);         // d = 5
		b.program[pc++] = TMovRegReg(0x10, 0x10, 0x1a);   // addd: d = d + a = 12
		b.program[pc++] = TMovRegMem(0x19, 0x33);
		b.program[pc++] = TMovRegReg(0x10, 0x10, 0x1b);   // subd: d = 12 - 7 = 5
		b.program[pc++] = TMovRegMem(0x19, 0x34);
		b.program[pc++] = TBranch(0x16, 0, pc);           // brif always: here (spin)
		TgpPortRun(b, 200);
		auto g = [](uint32_t u) { float x; memcpy(&x, &u, 4); return x; };
		Check(g(g_tbus.data[0x30]) == 4.0f, "TGP fadd (d = 1.5 + 2.5): " + Hex(g_tbus.data[0x30]));
		Check(g(g_tbus.data[0x31]) == 10.0f, "TGP fml (p = 2.5 * 4.0)");
		Check(g(g_tbus.data[0x32]) == 1.5f, "TGP fsbd");
		Check(g_tbus.data[0x33] == 12 && g_tbus.data[0x34] == 5, "TGP addd / subd");
		TgpRef t;
		TgpRefRun(t, b, 200);
		Check(memcmp(g_tbus.data, t.bus.data, sizeof(g_tbus.data)) == 0, "TGP ALU program: the port's core and MAME's end the same");
		printf("  ok?: TGP ALU program (%s)\n", g_failed ? "see above" : "checked");
	}
	// 2. a loop with c0 (brif zc0 counts it down), a call (bsif) and return (rtif), the FIFO read through rf1 and the
	//    output through rf2
	{
		TgpBus b = TgpBus();
		memset(b.program, 0, sizeof(b.program));
		memset(b.data, 0, sizeof(b.data));
		int pc = 0;
		b.program[pc++] = TStmh(1);
		b.program[pc++] = TLdi(0x0c, 5);                  // c0 = 5
		b.program[pc++] = TLdi(0x02, 0x40);               // x0 = 0x40
		const int loop = pc;
		b.program[pc++] = TMovRegReg(0x21, 0x19);         // d = rf1 (the FIFO)
		b.program[pc++] = TMovRegReg(0x19, 0x22);         // rf2 = d
		b.program[pc++] = TBranch(0x10, 0, loop, false);  // brif !zc0 (c0 != 1): loop, c0--
		const int call_at = pc;
		b.program[pc++] = TBranch(0x16, 2, 0x100);        // bsif 0x100
		b.program[pc++] = TLdi(0x13, 77);                 // b = 77 (after the return)
		b.program[pc++] = TBranch(0x16, 0, pc);           // spin
		b.program[0x100] = TLdi(0x10, 33);                // a = 33
		b.program[0x101] = TBranch(0x16, 5, 0);           // rtif
		(void)call_at;
		TgpPortRun(b, 300);
		const mb86233_core* c = Mb86233Core();
		Check(g_tbus.rf_log.size() == 10 && g_tbus.rf_log[1] == 0x40000000 && g_tbus.rf_log[9] == 0x40000004, "TGP: 5 FIFO words read and written (loop on c0): " + Hex(g_tbus.rf_log.size()));
		Check(c->m_a == 33 && c->m_b == 77, "TGP: bsif / rtif");
		TgpRef t;
		TgpRefRun(t, b, 300);
		const TgpRegs p = TGet(Mb86233Core()), q = TGet(t.cpu);
		Check(memcmp(&p, &q, sizeof(p)) == 0 && g_tbus.rf_log == t.bus.rf_log, "TGP loop program: the port's core and MAME's end the same");
	}
	printf("TGP: 2 known-answer programs\n");
}

void RunTgpRandom()
{
	int same = 0;
	const int kPrograms = 40;
	for (int seed = 1; seed <= kPrograms; seed++)
	{
		Rng r(seed + 1000);
		TgpBus b = TgpBus();
		for (auto& w : b.data) w = r.Next();
		// floats in part of the data RAM (the ALU's float ops on sane values too)
		for (int i = 0; i < 0x80; i++) { float x = float(int32_t(r.Below(2000)) - 1000) / 7.0f; memcpy(&b.data[0x200 + i], &x, 4); }
		int pc = 0;
		b.program[pc++] = TStmh(1 | (r.Below(4) << 1));
		while (pc < 0x400)
		{
			const uint32_t k = r.Below(100);
			uint32_t w;
			if (k < 25) w = TLab(r.Next() & 0x1ff, r.Next() & 0x1ff, r.Below(0x1c), r.Below(2) ? 3 : (r.Below(2) ? 4 : 0));
			else if (k < 60) w = (0x07u << 26) | (r.Below(0x1c) << 21) | (r.Below(8) << 18) | (r.Next() & 0x3ffff);
			else if (k < 70) w = TLdi(r.Below(0x40), int32_t(r.Next()) >> 8);
			else if (k < 75) w = (0x0eu << 26) | (r.Next() & 0x3ffffff);
			else if (k < 80) w = (0x0fu << 26) | (r.Below(0x1c) << 20) | (r.Below(4) << 17) | (r.Below(2) ? 0 : (r.Next() & 0x7fff)) | (r.Below(8) == 0 ? 0x8000 : 0);
			else
			{
				static const int conds[] = {0x00, 0x01, 0x02, 0x0a, 0x0b, 0x0c, 0x10, 0x11, 0x12, 0x16};
				const int sub = int(r.Below(7));
				w = TBranch(conds[r.Below(10)], sub == 4 ? 0 : sub, (r.Below(0x400)), r.Below(3) == 0) | (r.Below(2) ? 0x4000 : 0);
			}
			// rep counts: small (a rep of 255 is fine but slow to compare)
			b.program[pc++] = w;
		}
		const int cycles = 5000;
		TgpPortRun(b, cycles);
		TgpRef t;
		bool threw = false;
		try { TgpRefRun(t, b, cycles); } catch (const std::exception& e) { threw = true; printf("    (MAME's core stopped: %s)\n", e.what()); }
		const TgpRegs p = TGet(Mb86233Core());
		const TgpRegs q = threw ? p : TGet(t.cpu);
		const bool eq = !threw && memcmp(&p, &q, sizeof(p)) == 0 && memcmp(g_tbus.data, t.bus.data, sizeof(g_tbus.data)) == 0 &&
			g_tbus.io_log == t.bus.io_log && g_tbus.rf_log == t.bus.rf_log;
		if (eq) same++;
		else
		{
			printf("  FAIL: TGP random program %d differs (pc %x/%x d %x/%x)\n", seed, p.pc, q.pc, p.d, q.d);
			g_failed++;
		}
		printf("random tgp %02d: regs %016llx data %016llx io %zu rf %zu\n", seed, (unsigned long long)Fnv(&p, sizeof(p)),
			(unsigned long long)Fnv(g_tbus.data, sizeof(g_tbus.data)), g_tbus.io_log.size() / 2, g_tbus.rf_log.size() / 2);
	}
	if (same == kPrograms) g_passed++;
	printf("  %s: TGP: %d of %d random programs end the same on the port's core and MAME's\n", same == kPrograms ? "ok" : "FAIL", same, kPrograms);
}
} // namespace

int main()
{
	RunI960Kats();
	RunI960Random();
	RunTgpKats();
	RunTgpRandom();
	I960Exit();
	Mb86233Exit();
	printf("cpus test: %d checks ok, %d failed\n", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
