// FBNeo PS5 2.2: the new MIPS III core (src/cpu/mips3, upstream 0ce1ecf0a..892a35c8f, Killer Instinct's R4600) on its
// own, built four ways by `make test-mips3` (the cached interpreter or not, the idle-loop skip or not):
//   - known-answer programs: the opcodes the old core lacked (MOVZ/MOVN, DMULTU, DADD/DSUB, the unaligned pairs, LL/SC,
//     the branch-likely forms, traps, SYNC, CACHE, the FPU's loads and stores), exceptions and ERET, interrupts (the
//     Count/Compare timer and an external line), the TLB (map, refill, invalid, modified, TLBP/TLBR), code rewritten by
//     the CPU and by the host, handler memory with wait states, and idle loops (polling RAM, a countdown, Count);
//   - random programs (fixed seeds), whose registers, HI/LO and scratch memory are printed as one hash each: the four
//     builds must print the same lines (the Makefile compares them).
// No ROM: the programs are assembled here into RAM.
//
// SPDX-License-Identifier: MIT

#include "burnint.h"
#include "mips3_intf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#ifndef MIPS3_CACHED_INTERPRETER
#define MIPS3_CACHED_INTERPRETER 1
#endif
#ifndef MIPS3_IDLE_SKIP
#define MIPS3_IDLE_SKIP 1
#endif

// what mips3_intf.cpp and the core take from the rest of FBNeo
INT32(__cdecl* BurnAcb)(struct BurnArea* pba) = NULL;
void CpuCheatRegister(INT32, cpu_core_config*) {}

namespace
{
// ---- memory: 8 MB of RAM at 0, the boot ROM's 512 KB at 0x1fc00000 (writable here), an I/O page at 0x10000000 ----
UINT8* g_ram = nullptr;
UINT8* g_rom = nullptr;
constexpr UINT32 kRamSize = 8 << 20, kRomSize = 512 << 10;

struct IoLog
{
	int bytes = 0, halves = 0, words = 0, doubles = 0;
	UINT64 last = 0;
	int wait = 0; // cycles each I/O read costs (Mips3BurnCycles)
} g_io;

UINT8 IoReadByte(UINT32) { if (g_io.wait) Mips3BurnCycles(g_io.wait); return 0xab; }
UINT16 IoReadHalf(UINT32) { if (g_io.wait) Mips3BurnCycles(g_io.wait); return 0xcdef; }
UINT32 IoReadWord(UINT32) { if (g_io.wait) Mips3BurnCycles(g_io.wait); return 0x12345678; }
UINT64 IoReadDouble(UINT32) { if (g_io.wait) Mips3BurnCycles(g_io.wait); return 0x1122334455667788ULL; }
void IoWriteByte(UINT32, UINT8 d) { g_io.bytes++; g_io.last = d; }
void IoWriteHalf(UINT32, UINT16 d) { g_io.halves++; g_io.last = d; }
void IoWriteWord(UINT32 a, UINT32 d)
{
	g_io.words++;
	g_io.last = d;
	if ((a & 0xfff) == 0)
		Mips3SetIRQLine(0, 0); // the interrupt's acknowledge
}
void IoWriteDouble(UINT32, UINT64 d) { g_io.doubles++; g_io.last = d; }

UINT32 Rd32(UINT32 va) // kseg0 / kseg1 address of RAM
{
	const UINT32 pa = va & 0x1fffffff;
	UINT32 v;
	memcpy(&v, g_ram + pa, 4);
	return v;
}
UINT64 Rd64(UINT32 va)
{
	return Rd32(va) | (UINT64)Rd32(va + 4) << 32;
}
void Wr32(UINT32 va, UINT32 v)
{
	memcpy(g_ram + (va & 0x1fffffff), &v, 4);
}

// ---- a small assembler ----
enum Reg
{
	zero, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7, s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp,
	fp, ra
};

struct Asm
{
	UINT32 base;
	std::vector<UINT32> code;
	std::map<std::string, UINT32> labels;
	struct Fix { size_t at; std::string label; int kind; }; // 0: 16-bit branch offset, 1: 26-bit jump target
	std::vector<Fix> fixes;
	explicit Asm(UINT32 b) : base(b) {}

	UINT32 pc() const { return base + UINT32(code.size()) * 4; }
	UINT32 L(const std::string& n) { labels[n] = pc(); return pc(); }
	UINT32 at_(const std::string& n) const { return labels.at(n); }
	void w(UINT32 op) { code.push_back(op); }
	void R(int rs, int rt, int rd, int sa, int fn) { w(UINT32(rs) << 21 | UINT32(rt) << 16 | UINT32(rd) << 11 | UINT32(sa) << 6 | UINT32(fn)); }
	void I(int op, int rs, int rt, int imm) { w(UINT32(op) << 26 | UINT32(rs) << 21 | UINT32(rt) << 16 | (UINT32(imm) & 0xffff)); }
	void B(int op, int rs, int rt, const std::string& l) { fixes.push_back({code.size(), l, 0}); I(op, rs, rt, 0); }
	void J(int op, const std::string& l) { fixes.push_back({code.size(), l, 1}); w(UINT32(op) << 26); }

	// ALU
	void nop() { w(0); }
	void addu(int d, int s, int t) { R(s, t, d, 0, 0x21); }
	void add(int d, int s, int t) { R(s, t, d, 0, 0x20); }
	void subu(int d, int s, int t) { R(s, t, d, 0, 0x23); }
	void daddu(int d, int s, int t) { R(s, t, d, 0, 0x2d); }
	void dadd(int d, int s, int t) { R(s, t, d, 0, 0x2c); }
	void dsub(int d, int s, int t) { R(s, t, d, 0, 0x2e); }
	void or_(int d, int s, int t) { R(s, t, d, 0, 0x25); }
	void movz(int d, int s, int t) { R(s, t, d, 0, 0x0a); }
	void movn(int d, int s, int t) { R(s, t, d, 0, 0x0b); }
	void sll(int d, int t, int sa) { R(0, t, d, sa, 0x00); }
	void srl(int d, int t, int sa) { R(0, t, d, sa, 0x02); }
	void dsll(int d, int t, int sa) { R(0, t, d, sa, 0x38); }
	void dsrl(int d, int t, int sa) { R(0, t, d, sa, 0x3a); }
	void dsll32(int d, int t, int sa) { R(0, t, d, sa, 0x3c); }
	void dsrl32(int d, int t, int sa) { R(0, t, d, sa, 0x3e); }
	void dsra32(int d, int t, int sa) { R(0, t, d, sa, 0x3f); }
	void mfhi(int d) { R(0, 0, d, 0, 0x10); }
	void mflo(int d) { R(0, 0, d, 0, 0x12); }
	void dmult(int s, int t) { R(s, t, 0, 0, 0x1c); }
	void dmultu(int s, int t) { R(s, t, 0, 0, 0x1d); }
	void ddiv(int s, int t) { R(s, t, 0, 0, 0x1e); }
	void ddivu(int s, int t) { R(s, t, 0, 0, 0x1f); }
	void addiu(int t, int s, int imm) { I(0x09, s, t, imm); }
	void daddiu(int t, int s, int imm) { I(0x19, s, t, imm); }
	void ori(int t, int s, int imm) { I(0x0d, s, t, imm); }
	void andi(int t, int s, int imm) { I(0x0c, s, t, imm); }
	void lui(int t, int imm) { I(0x0f, 0, t, imm); }
	void li(int t, UINT32 v) { lui(t, int(v >> 16)); ori(t, t, int(v & 0xffff)); }
	void sync() { R(0, 0, 0, 0, 0x0f); }
	void syscall() { R(0, 0, 0, 0, 0x0c); }
	void brk() { R(0, 0, 0, 0, 0x0d); }
	void teq(int s, int t) { R(s, t, 0, 0, 0x34); }
	void tne(int s, int t) { R(s, t, 0, 0, 0x36); }
	void tgei(int s, int imm) { I(0x01, s, 0x08, imm); }
	void tlti(int s, int imm) { I(0x01, s, 0x0a, imm); }
	// loads / stores
	void lb(int t, int off, int s) { I(0x20, s, t, off); }
	void lbu(int t, int off, int s) { I(0x24, s, t, off); }
	void lh(int t, int off, int s) { I(0x21, s, t, off); }
	void lhu(int t, int off, int s) { I(0x25, s, t, off); }
	void lw(int t, int off, int s) { I(0x23, s, t, off); }
	void ld(int t, int off, int s) { I(0x37, s, t, off); }
	void sb(int t, int off, int s) { I(0x28, s, t, off); }
	void sh(int t, int off, int s) { I(0x29, s, t, off); }
	void sw(int t, int off, int s) { I(0x2b, s, t, off); }
	void sd(int t, int off, int s) { I(0x3f, s, t, off); }
	void lwl(int t, int off, int s) { I(0x22, s, t, off); }
	void lwr(int t, int off, int s) { I(0x26, s, t, off); }
	void swl(int t, int off, int s) { I(0x2a, s, t, off); }
	void swr(int t, int off, int s) { I(0x2e, s, t, off); }
	void ldl(int t, int off, int s) { I(0x1a, s, t, off); }
	void ldr(int t, int off, int s) { I(0x1b, s, t, off); }
	void sdl(int t, int off, int s) { I(0x2c, s, t, off); }
	void sdr(int t, int off, int s) { I(0x2d, s, t, off); }
	void ll(int t, int off, int s) { I(0x30, s, t, off); }
	void sc(int t, int off, int s) { I(0x38, s, t, off); }
	void lld(int t, int off, int s) { I(0x34, s, t, off); }
	void scd(int t, int off, int s) { I(0x3c, s, t, off); }
	void cache(int op, int off, int s) { I(0x2f, s, op, off); }
	void ldc1(int ft, int off, int s) { I(0x35, s, ft, off); }
	void sdc1(int ft, int off, int s) { I(0x3d, s, ft, off); }
	// branches and jumps
	void beq(int s, int t, const std::string& l) { B(0x04, s, t, l); }
	void bne(int s, int t, const std::string& l) { B(0x05, s, t, l); }
	void beql(int s, int t, const std::string& l) { B(0x14, s, t, l); }
	void bnel(int s, int t, const std::string& l) { B(0x15, s, t, l); }
	void blezl(int s, const std::string& l) { B(0x16, s, 0, l); }
	void bgtzl(int s, const std::string& l) { B(0x17, s, 0, l); }
	void bltz(int s, const std::string& l) { B(0x01, s, 0x00, l); }
	void bgezall(int s, const std::string& l) { B(0x01, s, 0x13, l); }
	void bltzall(int s, const std::string& l) { B(0x01, s, 0x12, l); }
	void j(const std::string& l) { J(0x02, l); }
	void jal(const std::string& l) { J(0x03, l); }
	void jr(int s) { R(s, 0, 0, 0, 0x08); }
	void jalr(int d, int s) { R(s, 0, d, 0, 0x09); }
	void halt() { std::string l = "halt" + std::to_string(code.size()); L(l); beq(zero, zero, l); nop(); }
	// COP0 / COP1
	void mfc0(int t, int d) { w(0x40000000u | UINT32(t) << 16 | UINT32(d) << 11); }
	void mtc0(int t, int d) { w(0x40800000u | UINT32(t) << 16 | UINT32(d) << 11); }
	void tlbr() { w(0x42000001); }
	void tlbwi() { w(0x42000002); }
	void tlbp() { w(0x42000008); }
	void eret() { w(0x42000018); }
	void mtc1(int t, int fs) { w(0x44800000u | UINT32(t) << 16 | UINT32(fs) << 11); }
	void mfc1(int t, int fs) { w(0x44000000u | UINT32(t) << 16 | UINT32(fs) << 11); }
	void fop(int fmt, int ft, int fs, int fd, int fn) { w(0x46000000u | UINT32(fmt) << 21 | UINT32(ft) << 16 | UINT32(fs) << 11 | UINT32(fd) << 6 | UINT32(fn)); }
	void bc1t(const std::string& l) { fixes.push_back({code.size(), l, 0}); w(0x45010000u); }

	void Link()
	{
		for (const Fix& f : fixes)
		{
			const UINT32 target = labels.at(f.label), here = base + UINT32(f.at) * 4;
			if (f.kind == 0)
				code[f.at] |= ((target - (here + 4)) >> 2) & 0xffff;
			else
				code[f.at] |= (target >> 2) & 0x03ffffff;
		}
	}
	void Put() // into RAM (kseg0 / kseg1 addresses) or the boot ROM (0xbfc00000)
	{
		Link();
		for (size_t i = 0; i < code.size(); i++)
		{
			const UINT32 va = base + UINT32(i) * 4;
			if ((va & 0x1fffffff) >= 0x1fc00000)
				memcpy(g_rom + ((va & 0x1fffffff) - 0x1fc00000), &code[i], 4);
			else
				Wr32(va, code[i]);
		}
	}
};

constexpr UINT32 kProg = 0x80001000, kRes = 0x80010000, kExc = 0x80007000, kData = 0x80011000;
// SR for the programs: CU0 and CU1 usable, BEV off (the vectors at 0x80000000 / 0x80000180), kernel mode
constexpr UINT32 kSr = 0x30000000;

// The exception handlers: each exception is logged at kExc (+0 count, +16*n: Cause, EPC, BadVAddr, Status); an
// interrupt is acknowledged (the I/O line, the timer) and resumed where it came; any other resumes at the word kExc+4
// when the program put an address there (cleared), else after the instruction. The refill vector sets kExc+8 first.
void PutHandlers()
{
	Asm r(0x80000000);
	r.lui(k0, 0x8000);
	r.ori(k0, k0, 0x7000);
	r.addiu(k1, zero, 0xaa);
	r.sw(k1, 8, k0);
	r.j("gen");
	r.nop();
	r.labels["gen"] = 0x80000180;
	r.Put();

	Asm h(0x80000180);
	h.lui(k1, 0x8000);
	h.ori(k1, k1, 0x7000);
	h.lw(k0, 0, k1);
	h.addiu(k0, k0, 1);
	h.sw(k0, 0, k1);
	h.sll(k0, k0, 4);
	h.addu(k0, k0, k1);
	h.mfc0(k1, 13);
	h.sw(k1, 0, k0);
	h.mfc0(k1, 14);
	h.sw(k1, 4, k0);
	h.mfc0(k1, 8);
	h.sw(k1, 8, k0);
	h.mfc0(k1, 12);
	h.sw(k1, 12, k0);
	h.mfc0(k1, 13);
	h.andi(k1, k1, 0x7c);
	h.bne(k1, zero, "notint");
	h.nop();
	h.lui(k0, 0xb000); // the I/O page's acknowledge, and the timer's (a write of Compare)
	h.sw(zero, 0, k0);
	h.addiu(k1, zero, -1);
	h.mtc0(k1, 11);
	h.eret();
	h.L("notint");
	h.lui(k0, 0x8000);
	h.ori(k0, k0, 0x7000);
	h.lw(k1, 4, k0);
	h.beq(k1, zero, "plus4");
	h.nop();
	h.sw(zero, 4, k0);
	h.mtc0(k1, 14);
	h.eret();
	h.L("plus4");
	h.mfc0(k1, 14);
	h.addiu(k1, k1, 4);
	h.mtc0(k1, 14);
	h.eret();
	h.Put();
}

// A fresh machine with `prog` at kProg; the boot ROM sets SR and jumps there.
void Boot(Asm& prog)
{
	memset(g_ram, 0, kRamSize);
	memset(g_rom, 0, kRomSize);
	g_io = IoLog();
	Mips3SetIRQLine(0, 0);
	Asm b(0xbfc00000);
	b.li(t0, kSr);
	b.mtc0(t0, 12);
	b.li(t0, kProg);
	b.jr(t0);
	b.nop();
	b.Put();
	PutHandlers();
	prog.Put();
	Mips3InvalidateRange(0, kRamSize - 1);
	Mips3InvalidateRange(0x1fc00000, 0x1fc00000 + kRomSize - 1);
	Mips3Reset();
}

// The program's prologue: every register cleared (they keep their values over a reset, as the R4600's do), s7 = the
// results, s6 = the data area
void Prologue(Asm& a)
{
	for (int r = 1; r < 32; r++)
		a.addiu(r, zero, 0);
	a.li(s7, kRes);
	a.li(s6, kData);
}

UINT32 Exc(int n, int field) { return Rd32(kExc + 16 * UINT32(n) + 4 * UINT32(field)); } // n from 1; 0 Cause 1 EPC 2 BadVAddr 3 SR
UINT32 ExcCount() { return Rd32(kExc); }
UINT32 Code(int n) { return (Exc(n, 0) >> 2) & 31; }

int g_pass = 0, g_fail = 0;
void Check(const char* name, bool ok, const std::string& got = "")
{
	printf("kat %-44s %s%s%s\n", name, ok ? "ok" : "FAIL", ok || got.empty() ? "" : ": ", ok ? "" : got.c_str());
	(ok ? g_pass : g_fail)++;
}
std::string Hex(UINT64 v)
{
	char b[24];
	snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
	return b;
}

void Run(int cycles = 200000) { Mips3Run(cycles); }

// ---- the known-answer programs ----
void Kats()
{
	{
		Asm a(kProg);
		Prologue(a);
		a.addiu(t0, zero, 5); a.addiu(t1, zero, 0); a.addiu(t2, zero, 7); a.addiu(t4, zero, 9);
		a.movz(t3, t0, t1); a.movn(t4, t0, t1); a.movn(t5, t2, t0);
		a.sw(t3, 0, s7); a.sw(t4, 4, s7); a.sw(t5, 8, s7);
		a.halt();
		Boot(a); Run();
		Check("MOVZ / MOVN", Rd32(kRes) == 5 && Rd32(kRes + 4) == 9 && Rd32(kRes + 8) == 7);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.daddiu(t0, zero, -1); a.addiu(t1, zero, 3); a.dmultu(t0, t1); a.mfhi(t2); a.mflo(t3);
		a.sd(t2, 0, s7); a.sd(t3, 8, s7);
		a.halt();
		Boot(a); Run();
		Check("DMULTU 64x64 -> 128", Rd64(kRes) == 2 && Rd64(kRes + 8) == 0xfffffffffffffffdULL, Hex(Rd64(kRes)) + " " + Hex(Rd64(kRes + 8)));
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.daddiu(t0, zero, -2); a.addiu(t1, zero, 3); a.dmult(t0, t1); a.mfhi(t2); a.mflo(t3);
		a.sd(t2, 0, s7); a.sd(t3, 8, s7);
		a.halt();
		Boot(a); Run();
		Check("DMULT signed", Rd64(kRes) == ~0ULL && Rd64(kRes + 8) == 0xfffffffffffffffaULL);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.daddiu(t0, zero, -7); a.addiu(t1, zero, 2); a.ddiv(t0, t1); a.mflo(t2); a.mfhi(t3);
		a.addiu(t4, zero, 16); a.addiu(t5, zero, 3); a.ddivu(t4, t5); a.mflo(t6); a.mfhi(t7);
		a.sd(t2, 0, s7); a.sd(t3, 8, s7); a.sd(t6, 16, s7); a.sd(t7, 24, s7);
		a.halt();
		Boot(a); Run();
		Check("DDIV / DDIVU", Rd64(kRes) == (UINT64)-3LL && Rd64(kRes + 8) == (UINT64)-1LL && Rd64(kRes + 16) == 5 && Rd64(kRes + 24) == 1);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.daddiu(t0, zero, 1); a.dsll32(t0, t0, 0); a.addiu(t1, zero, 5); a.dadd(t2, t0, t1); a.dsub(t3, t0, t1);
		a.sd(t2, 0, s7); a.sd(t3, 8, s7);
		a.halt();
		Boot(a); Run();
		Check("DADD / DSUB", Rd64(kRes) == 0x100000005ULL && Rd64(kRes + 8) == 0xfffffffbULL);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.daddiu(t0, zero, -1); a.dsrl(t0, t0, 1); a.addiu(t1, zero, 1); a.addiu(t2, zero, 0x55);
		const UINT32 at_dadd = a.pc();
		a.dadd(t2, t0, t1);
		a.sd(t2, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("DADD overflow: exception 12, rd unchanged", ExcCount() == 1 && Code(1) == 12 && Exc(1, 1) == at_dadd && Rd64(kRes) == 0x55);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.li(t0, 0x7fffffff); a.addiu(t1, zero, 1); a.addiu(t3, zero, 0x66);
		a.add(t3, t0, t1); a.addu(t4, t0, t1);
		a.sd(t3, 0, s7); a.sd(t4, 8, s7);
		a.halt();
		Boot(a); Run();
		Check("ADD overflow; ADDU sign-extends", ExcCount() == 1 && Code(1) == 12 && Rd64(kRes) == 0x66 && Rd64(kRes + 8) == 0xffffffff80000000ULL);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.li(t0, 0x33221100); a.sw(t0, 0, s6); a.li(t0, 0x77665544); a.sw(t0, 4, s6); a.li(t0, 0xbbaa9988); a.sw(t0, 8, s6);
		a.addiu(a0, s6, 1);
		a.lwr(t1, 0, a0); a.lwl(t1, 3, a0);
		a.ldr(t2, 0, a0); a.ldl(t2, 7, a0);
		a.sd(t1, 0, s7); a.sd(t2, 8, s7);
		a.halt();
		Boot(a); Run();
		Check("LWL/LWR and LDL/LDR, unaligned", Rd64(kRes) == 0x44332211ULL && Rd64(kRes + 8) == 0x8877665544332211ULL,
			Hex(Rd64(kRes)) + " " + Hex(Rd64(kRes + 8)));
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.li(t1, 0xa1b2c3d4); a.addiu(a0, s6, 0x11); a.swr(t1, 0, a0); a.swl(t1, 3, a0);
		a.li(t2, 0x44332211); a.li(t3, 0x88776655); a.dsll32(t3, t3, 0); a.dsll32(t2, t2, 0); a.dsrl32(t2, t2, 0); a.or_(t2, t2, t3);
		a.addiu(a1, s6, 0x21); a.sdr(t2, 0, a1); a.sdl(t2, 7, a1);
		a.halt();
		Boot(a); Run();
		Check("SWL/SWR and SDL/SDR, unaligned", Rd32(kData + 0x10) == 0xb2c3d400 && Rd32(kData + 0x14) == 0xa1 &&
			Rd32(kData + 0x20) == 0x33221100 && Rd32(kData + 0x24) == 0x77665544 && Rd32(kData + 0x28) == 0x88,
			Hex(Rd32(kData + 0x10)) + " " + Hex(Rd32(kData + 0x14)) + " " + Hex(Rd32(kData + 0x20)) + " " + Hex(Rd32(kData + 0x24)) + " " + Hex(Rd32(kData + 0x28)));
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.addiu(t0, zero, 5); a.sw(t0, 0, s6);
		a.ll(t0, 0, s6); a.addiu(t0, t0, 1); a.sc(t0, 0, s6); a.sw(t0, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("LL / SC succeeds", Rd32(kRes) == 1 && Rd32(kData) == 6);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.addiu(t0, zero, 5); a.sw(t0, 0, s6);
		a.ll(t0, 0, s6); a.syscall(); a.addiu(t0, t0, 1); a.sc(t0, 0, s6); a.sw(t0, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("SC fails after an exception (ERET)", Rd32(kRes) == 0 && Rd32(kData) == 5 && ExcCount() == 1 && Code(1) == 8);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.daddiu(t0, zero, 1); a.dsll32(t0, t0, 0); a.sd(t0, 0, s6);
		a.lld(t1, 0, s6); a.daddiu(t1, t1, 1); a.scd(t1, 0, s6); a.sd(t1, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("LLD / SCD", Rd64(kRes) == 1 && Rd64(kData) == 0x100000001ULL);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.addiu(t0, zero, 1);
		a.beql(t0, zero, "x1"); a.addiu(t1, zero, 7); // not taken: the delay slot is annulled
		a.L("x1");
		a.bnel(t0, zero, "x2"); a.addiu(t2, zero, 9); // taken: it runs
		a.addiu(t2, zero, 0x99);
		a.L("x2");
		a.sw(t1, 0, s7); a.sw(t2, 4, s7);
		a.halt();
		Boot(a); Run();
		Check("BEQL / BNEL: the delay slot annulled when not taken", Rd32(kRes) == 0 && Rd32(kRes + 4) == 9);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.daddiu(t0, zero, -1);
		a.bgtzl(t0, "y1"); a.addiu(t1, zero, 7);
		a.L("y1");
		a.blezl(t0, "y2"); a.addiu(t2, zero, 8);
		a.addiu(t2, zero, 0x99);
		a.L("y2");
		a.sw(t1, 0, s7); a.sw(t2, 4, s7);
		a.halt();
		Boot(a); Run();
		Check("BGTZL / BLEZL", Rd32(kRes) == 0 && Rd32(kRes + 4) == 8);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.addiu(t0, zero, 3);
		const UINT32 at1 = a.pc();
		a.bltzall(t0, "z9"); a.addiu(t1, zero, 7); // not taken: annulled, ra still written
		a.sw(ra, 0, s7);
		const UINT32 at2 = a.pc();
		a.bgezall(zero, "z1"); a.addiu(t2, zero, 9);
		a.addiu(t2, zero, 0x99);
		a.L("z1");
		a.sw(ra, 4, s7); a.sw(t1, 8, s7); a.sw(t2, 12, s7);
		a.halt();
		a.L("z9");
		a.halt();
		Boot(a); Run();
		Check("BLTZALL / BGEZALL: links, annuls", Rd32(kRes) == at1 + 8 && Rd32(kRes + 4) == at2 + 8 && Rd32(kRes + 8) == 0 && Rd32(kRes + 12) == 9);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.addiu(t0, zero, 3);
		const UINT32 at1 = a.pc();
		a.teq(t0, t0); a.tne(t0, t0); a.tgei(t0, 5);
		const UINT32 at2 = a.pc();
		a.tlti(t0, 5);
		a.halt();
		Boot(a); Run();
		Check("traps: TEQ, TNE, TGEI, TLTI", ExcCount() == 2 && Code(1) == 13 && Code(2) == 13 && Exc(1, 1) == at1 && Exc(2, 1) == at2);
	}
	{
		Asm a(kProg);
		Prologue(a);
		const UINT32 at1 = a.pc();
		a.syscall();
		const UINT32 at2 = a.pc();
		a.brk();
		a.addiu(t0, zero, 1); a.sw(t0, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("SYSCALL / BREAK: codes 8 and 9, EPC, then ERET", ExcCount() == 2 && Code(1) == 8 && Code(2) == 9 && Exc(1, 1) == at1 &&
			Exc(2, 1) == at2 && Rd32(kRes) == 1);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.sync(); a.cache(0x14, 0, s7); a.cache(0x01, 0, s6);
		a.addiu(t0, zero, 0x5a); a.sw(t0, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("SYNC and CACHE do nothing", ExcCount() == 0 && Rd32(kRes) == 0x5a);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.addiu(t0, zero, 3); a.mtc1(t0, 0); a.fop(20, 0, 0, 2, 0x21); // cvt.d.w f2, f0
		a.addiu(t1, zero, 4); a.mtc1(t1, 4); a.fop(20, 0, 4, 6, 0x21); // cvt.d.w f6, f4
		a.fop(17, 6, 2, 8, 2);  // mul.d f8 = f2 * f6 = 12
		a.fop(17, 2, 8, 8, 0);  // add.d f8 = f8 + f2 = 15
		a.fop(17, 0, 8, 10, 0x24); // cvt.w.d f10, f8
		a.mfc1(t2, 10); a.sw(t2, 0, s7);
		a.fop(17, 6, 2, 0, 0x3c); // c.lt.d f2, f6
		a.bc1t("fp1"); a.nop();
		a.halt();
		a.L("fp1");
		a.addiu(t3, zero, 1); a.sw(t3, 4, s7);
		a.sdc1(8, 8, s7);
		a.ldc1(12, 8, s7); a.fop(17, 0, 12, 14, 0x24); a.mfc1(t4, 14); a.sw(t4, 16, s7);
		a.halt();
		Boot(a); Run();
		Check("FPU: CVT, MUL.D, ADD.D, C.LT.D + BC1T, SDC1 / LDC1", Rd32(kRes) == 15 && Rd32(kRes + 4) == 1 && Rd64(kRes + 8) == 0x402e000000000000ULL &&
			Rd32(kRes + 16) == 15, Hex(Rd32(kRes)) + " " + Hex(Rd64(kRes + 8)));
	}
	{
		Asm a(kProg);
		Prologue(a);
		const UINT32 at1 = a.pc();
		a.jal("sub1"); a.nop();
		a.sw(ra, 0, s7);
		a.li(t9, 0); a.labels["sub2"] = 0; // patched below
		const size_t li_at = a.code.size() - 2;
		const UINT32 at2 = a.pc();
		a.jalr(s0, t9); a.nop();
		a.sw(s0, 4, s7);
		a.halt();
		const UINT32 sub1 = a.L("sub1");
		a.jr(ra); a.nop();
		const UINT32 sub2 = a.pc();
		a.jr(s0); a.nop();
		(void)sub1;
		a.code[li_at] = 0x3c000000u | UINT32(t9) << 16 | (sub2 >> 16);
		a.code[li_at + 1] = 0x34000000u | UINT32(t9) << 21 | UINT32(t9) << 16 | (sub2 & 0xffff);
		Boot(a); Run();
		Check("JAL / JALR link addresses", Rd32(kRes) == at1 + 8 && Rd32(kRes + 4) == at2 + 8);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.lui(t0, 0x8000); a.dsrl32(t1, t0, 0); a.dsra32(t2, t0, 0); a.dsll(t3, t1, 4);
		a.sd(t0, 0, s7); a.sd(t1, 8, s7); a.sd(t2, 16, s7); a.sd(t3, 24, s7);
		a.halt();
		Boot(a); Run();
		Check("LUI sign extension, DSRL32 / DSRA32 / DSLL", Rd64(kRes) == 0xffffffff80000000ULL && Rd64(kRes + 8) == 0xffffffffULL &&
			Rd64(kRes + 16) == ~0ULL && Rd64(kRes + 24) == 0xffffffff0ULL);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.li(t0, 0xa0012000); a.li(t1, 0x80012000); a.li(t2, 0xcafe1234);
		a.sw(t2, 0, t0); a.lw(t3, 0, t1); a.sw(t3, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("kseg1 and kseg0 are the same memory", Rd32(kRes) == 0xcafe1234);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.lui(a0, 0xb000);
		a.lb(t0, 0, a0); a.lbu(t1, 0, a0); a.lh(t2, 0, a0); a.lhu(t3, 0, a0); a.lw(t4, 0, a0); a.ld(t5, 0, a0);
		a.sd(t0, 0, s7); a.sd(t1, 8, s7); a.sd(t2, 16, s7); a.sd(t3, 24, s7); a.sd(t4, 32, s7); a.sd(t5, 40, s7);
		a.addiu(t6, zero, 0x42); a.sb(t6, 8, a0); a.sh(t6, 8, a0); a.sd(t6, 8, a0);
		a.halt();
		Boot(a); Run();
		Check("handler memory: every width, sign extension", Rd64(kRes) == 0xffffffffffffffabULL && Rd64(kRes + 8) == 0xab &&
			Rd64(kRes + 16) == 0xffffffffffffcdefULL && Rd64(kRes + 24) == 0xcdef && Rd64(kRes + 32) == 0x12345678 &&
			Rd64(kRes + 40) == 0x1122334455667788ULL && g_io.bytes == 1 && g_io.halves == 1 && g_io.doubles == 1 && g_io.last == 0x42);
	}
	{
		// wait states: ten I/O reads at 100 cycles each show in Count
		Asm a(kProg);
		Prologue(a);
		a.lui(a0, 0xb000);
		a.mfc0(t0, 9);
		for (int i = 0; i < 10; i++)
			a.lw(t1, 0, a0);
		a.mfc0(t2, 9);
		a.subu(t3, t2, t0); a.sw(t3, 0, s7);
		a.halt();
		Boot(a);
		g_io.wait = 100;
		Run();
		const UINT32 d = Rd32(kRes);
		Check("wait states (Mips3BurnCycles) reach Count", d >= 500 && d <= 510, Hex(d));
	}
	{
		Asm a(kProg);
		Prologue(a);
		const UINT32 at1 = a.pc();
		a.lw(t0, 1, s7);
		a.addiu(t1, zero, 0x11); a.sw(t1, 0, s7);
		a.sh(t0, 3, s7);
		a.halt();
		Boot(a); Run();
		Check("unaligned LW / SH: AdEL (4) / AdES (5), BadVAddr", ExcCount() == 2 && Code(1) == 4 && Exc(1, 2) == kRes + 1 &&
			Exc(1, 1) == at1 && Rd32(kRes) == 0x11 && Code(2) == 5 && Exc(2, 2) == kRes + 3);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.li(t0, 0); // patched: the resume address
		const size_t li_at = a.code.size() - 2;
		a.lui(t1, 0x8000); a.ori(t1, t1, 0x7000); a.sw(t0, 4, t1);
		const UINT32 at_b = a.pc();
		a.beq(zero, zero, "ds1"); a.lw(t2, 2, s7); // the delay slot faults
		a.halt();
		const UINT32 resume = a.L("ds1");
		a.addiu(t3, zero, 0x33); a.sw(t3, 0, s7);
		a.halt();
		a.code[li_at] = 0x3c000000u | UINT32(t0) << 16 | (resume >> 16);
		a.code[li_at + 1] = 0x34000000u | UINT32(t0) << 21 | UINT32(t0) << 16 | (resume & 0xffff);
		Boot(a); Run();
		Check("exception in a delay slot: BD set, EPC = the branch", ExcCount() == 1 && (Exc(1, 0) & 0x80000000u) && Exc(1, 1) == at_b &&
			Rd32(kRes) == 0x33);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.mfc0(t0, 9);
		for (int i = 0; i < 100; i++)
			a.nop();
		a.mfc0(t1, 9);
		a.subu(t2, t1, t0); a.sw(t2, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("Count runs at half the cycles", Rd32(kRes) == 50 || Rd32(kRes) == 51, Hex(Rd32(kRes)));
	}
	{
		// the timer: Compare = Count + 300, IE and IM7 on, then a loop polling the exception count (an idle loop)
		Asm a(kProg);
		Prologue(a);
		a.li(t0, kSr | 0x8001); a.mtc0(t0, 12);
		a.mfc0(t1, 9); a.addiu(t1, t1, 300); a.mtc0(t1, 11);
		a.lui(a0, 0x8000); a.ori(a0, a0, 0x7000);
		a.L("wait1");
		a.lw(t2, 0, a0); a.beq(t2, zero, "wait1"); a.nop();
		a.addiu(t3, zero, 1); a.sw(t3, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("Count / Compare: the timer interrupt (IP7)", ExcCount() == 1 && Code(1) == 0 && (Exc(1, 0) & 0x8000) && Rd32(kRes) == 1,
			Hex(ExcCount()) + " " + Hex(Exc(1, 0)));
	}
	{
		// an external line (IP2) raised by the host between two slices, acknowledged through the I/O page
		Asm a(kProg);
		Prologue(a);
		a.li(t0, kSr | 0x0401); a.mtc0(t0, 12);
		a.lui(a0, 0x8000); a.ori(a0, a0, 0x7000);
		a.L("wait2");
		a.lw(t2, 0, a0); a.beq(t2, zero, "wait2"); a.nop();
		a.addiu(t3, zero, 2); a.sw(t3, 0, s7);
		a.halt();
		Boot(a);
		Run(50000);
		const bool none_yet = ExcCount() == 0;
		Mips3SetIRQLine(0, 1);
		Run(50000);
		Check("external interrupt (IP2), acknowledged", none_yet && ExcCount() == 1 && Code(1) == 0 && (Exc(1, 0) & 0x400) && Rd32(kRes) == 2 &&
			g_io.words == 1);
	}
	{
		// a line raised while IE is off: taken right after the program turns IE on
		Asm a(kProg);
		Prologue(a);
		a.li(t0, kSr | 0x0400); a.mtc0(t0, 12);
		for (int i = 0; i < 20; i++)
			a.nop();
		a.lui(a0, 0x8000); a.ori(a0, a0, 0x7000); a.lw(t1, 0, a0); a.sw(t1, 0, s7);
		a.li(t0, kSr | 0x0401); a.mtc0(t0, 12);
		const UINT32 after = a.pc();
		a.lw(t1, 0, a0); a.sw(t1, 4, s7);
		a.halt();
		Boot(a);
		Mips3SetIRQLine(0, 1);
		Run();
		Check("masked while IE is off, taken once it is on", Rd32(kRes) == 0 && Rd32(kRes + 4) == 1 && Exc(1, 1) == after, Hex(Exc(1, 1)));
	}
	{
		// ERET: to EPC with EXL, to ErrorEPC with ERL
		Asm a(kProg);
		Prologue(a);
		a.li(t0, 0); const size_t p1 = a.code.size() - 2;
		a.mtc0(t0, 14); a.li(t1, kSr | 2); a.mtc0(t1, 12); a.eret();
		a.halt();
		const UINT32 l1 = a.L("e1");
		a.mfc0(t2, 12); a.sw(t2, 0, s7);
		a.li(t0, 0); const size_t p2 = a.code.size() - 2;
		a.mtc0(t0, 30); a.li(t1, kSr | 4); a.mtc0(t1, 12); a.eret();
		a.halt();
		const UINT32 l2 = a.L("e2");
		a.mfc0(t2, 12); a.sw(t2, 4, s7);
		a.halt();
		const size_t ats[2] = {p1, p2};
		const UINT32 vs[2] = {l1, l2};
		for (int i = 0; i < 2; i++)
		{
			a.code[ats[i]] = 0x3c000000u | UINT32(t0) << 16 | (vs[i] >> 16);
			a.code[ats[i] + 1] = 0x34000000u | UINT32(t0) << 21 | UINT32(t0) << 16 | (vs[i] & 0xffff);
		}
		Boot(a); Run();
		Check("ERET: EPC and EXL, ErrorEPC and ERL", Rd32(kRes) == kSr && Rd32(kRes + 4) == kSr, Hex(Rd32(kRes)) + " " + Hex(Rd32(kRes + 4)));
	}
	// the TLB: a 4 KB pair at VA 0x00400000 -> 0x20000 / 0x21000
	auto tlb_entry = [](Asm& a, int index, UINT32 hi, UINT32 lo0, UINT32 lo1) {
		a.addiu(t0, zero, index); a.mtc0(t0, 0);
		a.mtc0(zero, 5);
		a.li(t0, hi); a.mtc0(t0, 10);
		a.li(t0, lo0); a.mtc0(t0, 2);
		a.li(t0, lo1); a.mtc0(t0, 3);
		a.tlbwi();
	};
	{
		Asm a(kProg);
		Prologue(a);
		tlb_entry(a, 0, 0x00400000, 0x20 << 6 | 7, 0x21 << 6 | 7);
		a.li(a0, 0x00400004); a.li(t1, 0x600df00d); a.sw(t1, 0, a0);
		a.li(a1, 0x00401008); a.li(t2, 0x0badcafe); a.sw(t2, 0, a1); a.lw(t3, 0, a1); a.sw(t3, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("TLB: a mapped page pair", ExcCount() == 0 && Rd32(0x80020004) == 0x600df00d && Rd32(0x80021008) == 0x0badcafe && Rd32(kRes) == 0x0badcafe);
	}
	{
		Asm a(kProg);
		Prologue(a);
		a.li(a0, 0x00800000); a.lw(t0, 0, a0);
		a.halt();
		Boot(a); Run();
		Check("TLB refill: vector 0x80000000, TLBL, BadVAddr, EntryHi", ExcCount() == 1 && Code(1) == 2 && Exc(1, 2) == 0x00800000 &&
			Rd32(kExc + 8) == 0xaa);
	}
	{
		Asm a(kProg);
		Prologue(a);
		tlb_entry(a, 1, 0x00600000, 0x30 << 6 | 1, 0x31 << 6 | 1); // global, not valid
		a.li(a0, 0x00600010); a.lw(t0, 0, a0);
		a.halt();
		Boot(a); Run();
		Check("TLB invalid: TLBL at the general vector", ExcCount() == 1 && Code(1) == 2 && Rd32(kExc + 8) == 0 && Exc(1, 2) == 0x00600010);
	}
	{
		Asm a(kProg);
		Prologue(a);
		tlb_entry(a, 2, 0x00500000, 0x40 << 6 | 3, 0x41 << 6 | 3); // valid, not dirty
		a.li(a0, 0x00500000); a.lw(t0, 0, a0); a.sw(t0, 0, a0);
		a.halt();
		Boot(a); Run();
		Check("TLB modified: a store to a clean page (Mod, 1)", ExcCount() == 1 && Code(1) == 1 && Exc(1, 2) == 0x00500000);
	}
	{
		Asm a(kProg);
		Prologue(a);
		tlb_entry(a, 5, 0x00700000, 0x50 << 6 | 7, 0x51 << 6 | 7);
		a.mtc0(zero, 0); a.mtc0(zero, 2);
		a.li(t0, 0x00700000); a.mtc0(t0, 10); a.tlbp(); a.mfc0(t1, 0); a.sw(t1, 0, s7);
		a.tlbr(); a.mfc0(t2, 2); a.sw(t2, 4, s7);
		a.li(t0, 0x00900000); a.mtc0(t0, 10); a.tlbp(); a.mfc0(t3, 0); a.sw(t3, 8, s7);
		a.halt();
		Boot(a); Run();
		Check("TLBP finds the entry, TLBR reads it, a miss sets P", Rd32(kRes) == 5 && Rd32(kRes + 4) == (0x50 << 6 | 7) && (Rd32(kRes + 8) & 0x80000000u),
			Hex(Rd32(kRes)) + " " + Hex(Rd32(kRes + 4)) + " " + Hex(Rd32(kRes + 8)));
	}
	{
		// code the CPU rewrites: a subroutine run, its first instruction stored over, run again
		Asm a(kProg);
		Prologue(a);
		a.jal("sub"); a.nop();
		a.sw(v0, 0, s7);
		a.li(t0, 0x24020002); // addiu v0, zero, 2
		a.li(a0, 0); const size_t p = a.code.size() - 2;
		a.sw(t0, 0, a0);
		a.jal("sub"); a.nop();
		a.sw(v0, 4, s7);
		a.halt();
		const UINT32 sub = a.L("sub");
		a.addiu(v0, zero, 1); a.jr(ra); a.nop();
		a.code[p] = 0x3c000000u | UINT32(a0) << 16 | (sub >> 16);
		a.code[p + 1] = 0x34000000u | UINT32(a0) << 21 | UINT32(a0) << 16 | (sub & 0xffff);
		Boot(a); Run();
		Check("code rewritten by the CPU runs new", Rd32(kRes) == 1 && Rd32(kRes + 4) == 2);
	}
	{
		// code rewritten by the host (Mips3InvalidateRange) between two slices, the program waiting on a flag
		Asm a(kProg);
		Prologue(a);
		a.jal("hsub"); a.nop();
		a.sw(v0, 0, s7);
		a.L("hwait");
		a.lw(t0, 0x100, s7); a.beq(t0, zero, "hwait"); a.nop();
		a.jal("hsub"); a.nop();
		a.sw(v0, 4, s7);
		a.halt();
		const UINT32 hsub = a.L("hsub");
		a.addiu(v0, zero, 1); a.jr(ra); a.nop();
		Boot(a);
		Run(50000);
		Wr32(hsub, 0x24020003); // addiu v0, zero, 3
		Mips3InvalidateRange(hsub & 0x1fffffff, (hsub & 0x1fffffff) + 3);
		Wr32(kRes + 0x100, 1);
		Run(50000);
		Check("code rewritten by the host runs new", Rd32(kRes) == 1 && Rd32(kRes + 4) == 3);
	}
	{
		// an idle loop polling RAM: skipped to the slice's end, left when the host sets the flag
		Asm a(kProg);
		Prologue(a);
		a.L("poll");
		a.lw(t0, 0x200, s7); a.beq(t0, zero, "poll"); a.nop();
		a.addiu(t1, zero, 0x77); a.sw(t1, 0, s7);
		a.halt();
		Boot(a);
		const INT64 c0 = Mips3TotalCycles();
		const int ran1 = Mips3Run(100000);
		const bool waiting = Rd32(kRes) == 0;
		Wr32(kRes + 0x200, 1);
		Run(100000);
		Check("idle loop polling RAM: the slice's cycles, then on", waiting && ran1 >= 100000 && Mips3TotalCycles() - c0 >= 200000 && Rd32(kRes) == 0x77);
	}
	{
		// a countdown loop (addiu / bne): skipped in one go, the counter right at its end, Count follows
		Asm a(kProg);
		Prologue(a);
		a.mfc0(s0, 9);
		a.li(t0, 100000);
		a.L("cd");
		a.addiu(t0, t0, -1); a.bne(t0, zero, "cd"); a.nop();
		a.mfc0(s1, 9);
		a.sw(t0, 0, s7); a.subu(t1, s1, s0); a.sw(t1, 4, s7);
		a.halt();
		Boot(a); Run(1000000);
		const UINT32 d = Rd32(kRes + 4);
		Check("countdown loop: counter 0, Count ~ 3 cycles an iteration", Rd32(kRes) == 0 && d >= 150000 && d <= 150004, Hex(d));
	}
	{
		// a loop waiting on Count: left at the first iteration past the target
		Asm a(kProg);
		Prologue(a);
		a.mfc0(t0, 9); a.addiu(s0, t0, 5000);
		a.L("tw");
		a.mfc0(t1, 9); a.subu(t2, t1, s0); a.bltz(t2, "tw"); a.nop();
		a.sw(t2, 0, s7);
		a.halt();
		Boot(a); Run(100000);
		Check("loop on Count: left right after the target", (INT32)Rd32(kRes) >= 0 && Rd32(kRes) <= 2, Hex(Rd32(kRes)));
	}
	{
		// a delay slot in the next page (the block is split): the jump's delay slot runs once
		Asm a(0x80002ff4);
		a.addiu(t0, zero, 0);
		a.addiu(t0, t0, 1);
		a.j("far");
		a.addiu(t0, t0, 0x10); // at 0x80003000: the next page
		a.halt();
		a.L("far");
		a.li(s7, kRes); a.sw(t0, 0, s7);
		a.halt();
		Asm b(kProg);
		b.li(t1, 0x80002ff4); b.jr(t1); b.nop();
		Boot(b);
		a.Put();
		Mips3InvalidateRange(0, kRamSize - 1);
		Run();
		Check("a delay slot across a page boundary", Rd32(kRes) == 0x11, Hex(Rd32(kRes)));
	}
	{
		// a branch in a delay slot (Killer Instinct's boot ROM has one): the second branch's target follows the first's
		Asm a(kProg);
		Prologue(a);
		a.beq(zero, zero, "bd1");
		a.beq(zero, zero, "bd2");
		a.halt();
		a.L("bd1");
		a.addiu(t0, zero, 0x21); // the first target: one instruction, then the second branch's target
		a.addiu(t0, zero, 0x22);
		a.halt();
		a.L("bd2");
		a.sw(t0, 0, s7);
		a.halt();
		Boot(a); Run();
		Check("a taken branch in a delay slot", Rd32(kRes) == 0x21, Hex(Rd32(kRes)));
	}
}

// ---- random programs: the same results in every build ----
struct Rng
{
	UINT64 s;
	UINT32 Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return UINT32(s); }
	int Range(int n) { return int(Next() % UINT32(n)); }
};

void Random(int seed)
{
	Rng r{0x9e3779b97f4a7c15ULL * UINT64(seed + 1)};
	Asm a(kProg + UINT32(r.Range(16)) * 0x40 + 0xe00); // starts near a page's end now and then
	static const int regs[] = {t0, t1, t2, t3, t4, t5, t6, t7, s0, s1, s2, s3, s4, s5, v0, v1, a0, a1, a2, a3};
	auto R = [&] { return regs[r.Range(20)]; };
	a.li(s7, kRes);
	a.li(s6, kData);
	for (int reg : regs)
	{
		a.lui(reg, int(r.Next() & 0xffff)); a.ori(reg, reg, int(r.Next() & 0xffff));
		if (r.Range(2)) a.dsll32(at, reg, 0), a.daddu(reg, reg, at);
	}
	const int n = 150 + r.Range(100);
	int label = 0;
	for (int i = 0; i < n; i++)
	{
		const int d = R(), s = R(), t = R(), k = r.Range(36);
		switch (k)
		{
			case 0: a.addu(d, s, t); break;
			case 1: a.subu(d, s, t); break;
			case 2: a.R(s, t, d, 0, 0x24); break; // and
			case 3: a.or_(d, s, t); break;
			case 4: a.R(s, t, d, 0, 0x26); break; // xor
			case 5: a.R(s, t, d, 0, 0x27); break; // nor
			case 6: a.R(s, t, d, 0, 0x2a); break; // slt
			case 7: a.R(s, t, d, 0, 0x2b); break; // sltu
			case 8: a.daddu(d, s, t); break;
			case 9: a.R(s, t, d, 0, 0x2f); break; // dsubu
			case 10: a.R(0, t, d, r.Range(32), r.Range(2) ? 0x00 : 0x03); break; // sll / sra
			case 11: a.R(0, t, d, r.Range(32), 0x38 + 2 * r.Range(2)); break; // dsll / dsrl
			case 12: a.R(0, t, d, r.Range(32), 0x3c + r.Range(4)); break; // dsll32 / ... (0x3d is not one: RI, skipped below)
			case 13: a.R(s, t, d, 0, 0x04 + r.Range(4)); break; // sllv / (5: RI) / srlv / srav
			case 14: a.R(s, t, d, 0, 0x14 + r.Range(4)); break; // dsllv / (0x15: RI) / dsrlv / dsrav
			case 15: a.addiu(t, s, int(r.Next())); break;
			case 16: a.daddiu(t, s, int(r.Next())); break;
			case 17: a.I(0x0a + r.Range(5), s, t, int(r.Next())); break; // slti / sltiu / andi / ori / xori
			case 18: a.lui(t, int(r.Next())); break;
			case 19: a.R(s, t, 0, 0, 0x18 + r.Range(2)); break; // mult / multu
			case 20: a.R(s, t, 0, 0, 0x1c + r.Range(2)); break; // dmult / dmultu
			case 21: a.ori(t, t, 1); a.R(s, t, 0, 0, 0x1a + r.Range(2)); break; // div / divu (never by zero)
			case 22: a.ori(t, t, 1); a.R(s, t, 0, 0, 0x1e + r.Range(2)); break; // ddiv / ddivu
			case 23: a.mfhi(d); break;
			case 24: a.mflo(d); break;
			case 25: a.movz(d, s, t); break;
			case 26: a.movn(d, s, t); break;
			case 27: a.sw(t, 4 * r.Range(64), s6); break;
			case 28: a.lw(t, 4 * r.Range(64), s6); break;
			case 29: a.sd(t, 8 * r.Range(32), s6); break;
			case 30: a.ld(t, 8 * r.Range(32), s6); break;
			case 31: { const int o = r.Range(250); a.lwr(t, o, s6); a.lwl(t, o + 3, s6); break; }
			case 32: { const int o = r.Range(250); a.swr(t, o, s6); a.swl(t, o + 3, s6); break; }
			case 33: a.I(0x20 + r.Range(6), s6, t, r.Range(256) & ~1); break; // lb / lh / lwl / lw / lbu / lhu (aligned enough)
			case 34:
			{
				// a short forward branch with a random delay slot
				const std::string l = "f" + std::to_string(label++);
				switch (r.Range(4))
				{
					case 0: a.beq(s, t, l); break;
					case 1: a.bne(s, t, l); break;
					case 2: a.B(0x06, s, 0, l); break; // blez
					default: a.B(0x07, s, 0, l); break; // bgtz
				}
				a.addu(d, s, t);
				for (int j = r.Range(3); j > 0; j--)
					a.addiu(d, d, int(r.Next()));
				a.L(l);
				break;
			}
			default:
			{
				// a small loop
				const std::string l = "b" + std::to_string(label++);
				a.addiu(at, zero, 1 + r.Range(40));
				a.L(l);
				a.addu(d, d, s);
				a.R(0, d, d, 1 + r.Range(31), 0x02); // srl
				a.addiu(at, at, -1);
				a.bne(at, zero, l);
				a.R(s, d, t, 0, 0x26); // xor in the delay slot
				break;
			}
		}
		// the RI encodings above (0x3d, 0x05, 0x15) would raise an exception: replaced by a nop
		UINT32& last = a.code.back();
		if ((last >> 26) == 0 && ((last & 63) == 0x3d || (last & 63) == 0x05 || (last & 63) == 0x15))
			last = 0;
	}
	for (int i = 1; i < 32; i++)
		if (i != 26 && i != 27)
			a.sd(i, 8 * i, s7);
	a.mfhi(t0); a.sd(t0, 0x100, s7); a.mflo(t0); a.sd(t0, 0x108, s7);
	a.halt();
	Asm b(kProg);
	if ((a.base & ~0xfffu) == kProg)
		b.base = kProg - 0x40; // (never: programs start at kProg + 0xe00 or later)
	b.li(t0, a.base); b.jr(t0); b.nop();
	memset(g_ram, 0, kRamSize);
	Boot(b);
	a.Put();
	Mips3InvalidateRange(0, kRamSize - 1);
	Run(2000000);
	UINT64 h = 0xcbf29ce484222325ULL;
	for (UINT32 i = 0; i < 0x110; i++)
		h = (h ^ g_ram[(kRes & 0x1fffffff) + i]) * 0x100000001b3ULL;
	for (UINT32 i = 0; i < 0x200; i++)
		h = (h ^ g_ram[(kData & 0x1fffffff) + i]) * 0x100000001b3ULL;
	printf("random %2d: %d instructions, %016llx, %u exception(s)\n", seed, int(a.code.size()), (unsigned long long)h, ExcCount());
}
} // namespace

int main()
{
	g_ram = (UINT8*)calloc(1, kRamSize);
	g_rom = (UINT8*)calloc(1, kRomSize);
	Mips3Init();
	Mips3MapMemory(g_ram, 0x00000000, kRamSize - 1, MAP_RAM);
	Mips3MapMemory(g_rom, 0x1fc00000, 0x1fc00000 + kRomSize - 1, MAP_RAM);
	Mips3SetReadByteHandler(1, IoReadByte);
	Mips3SetReadHalfHandler(1, IoReadHalf);
	Mips3SetReadWordHandler(1, IoReadWord);
	Mips3SetReadDoubleHandler(1, IoReadDouble);
	Mips3SetWriteByteHandler(1, IoWriteByte);
	Mips3SetWriteHalfHandler(1, IoWriteHalf);
	Mips3SetWriteWordHandler(1, IoWriteWord);
	Mips3SetWriteDoubleHandler(1, IoWriteDouble);
	Mips3MapHandler(1, 0x10000000, 0x10000fff, MAP_READ | MAP_WRITE);
	printf("mips3 test: cached interpreter %d, idle skip %d\n", MIPS3_CACHED_INTERPRETER, MIPS3_IDLE_SKIP);
	Kats();
	for (int i = 0; i < 20; i++)
		Random(i);
	printf("mips3 test: %d known answers ok, %d failed\n", g_pass, g_fail);
	Mips3Exit();
	free(g_ram);
	free(g_rom);
	return g_fail ? 1 : 0;
}
