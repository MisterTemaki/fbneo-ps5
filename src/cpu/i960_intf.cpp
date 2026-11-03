// Intel 80960KA/KB interface (FBNeo PS5 2.3): the page-mapped bus, the run loop and the state of the core in
// i960/ (MAME's interpreter, BSD-3-Clause, Farfetch'd, R. Belmont).
// SPDX-License-Identifier: BSD-3-Clause

#include "burnint.h"
#include "i960_intf.h"
#include "i960/i960.h"

#include <stdarg.h>

#define I960_PAGE_SHIFT   12
#define I960_PAGE_MASK    0xfff
#define I960_SPACE        0x20000000u
#define I960_PAGES        (I960_SPACE >> I960_PAGE_SHIFT)
#define I960_IS_PTR(p)    ((uintptr_t)(p) >= I960_MAXHANDLER)

static i960_core* core = NULL;

static UINT8** pRead = NULL;
static UINT8** pWrite = NULL;
static UINT8** pFetch = NULL;
static UINT16* pFlags = NULL;

static pI960ReadByteHandler ReadByteH[I960_MAXHANDLER];
static pI960WriteByteHandler WriteByteH[I960_MAXHANDLER];
static pI960ReadWordHandler ReadWordH[I960_MAXHANDLER];
static pI960WriteWordHandler WriteWordH[I960_MAXHANDLER];
static pI960ReadLongHandler ReadLongH[I960_MAXHANDLER];
static pI960WriteLongHandler WriteLongH[I960_MAXHANDLER];

// run state
static INT32 nCyclesToDo = 0;
static INT64 nTotalCycles = 0;
static INT32 bRunning = 0;
static INT32 bHalted = 0;

static INT32 bInitted = 0;

static UINT8 DefReadByte(UINT32) { return 0; }
static UINT16 DefReadWord(UINT32) { return 0; }
static UINT32 DefReadLong(UINT32) { return 0; }
static void DefWriteByte(UINT32, UINT8) { }
static void DefWriteWord(UINT32, UINT16) { }
static void DefWriteLong(UINT32, UINT32) { }

// ---- the bus, as the core calls it ---------------------------------------------------------------------------------
uint8_t i960_bus_read8(uint32_t a)
{
	if (a >= I960_SPACE) return 0;
	UINT8* p = pRead[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) return p[a & I960_PAGE_MASK];
	return ReadByteH[(uintptr_t)p](a);
}

uint16_t i960_bus_read16(uint32_t a)
{
	a &= ~1u;
	if (a >= I960_SPACE) return 0;
	UINT8* p = pRead[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) return *(UINT16*)(p + (a & I960_PAGE_MASK));
	return ReadWordH[(uintptr_t)p](a);
}

uint32_t i960_bus_read32(uint32_t a)
{
	a &= ~3u; // MAME's native-width access
	if (a >= I960_SPACE) return 0;
	UINT8* p = pRead[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) return *(UINT32*)(p + (a & I960_PAGE_MASK));
	return ReadLongH[(uintptr_t)p](a);
}

void i960_bus_write8(uint32_t a, uint8_t d)
{
	if (a >= I960_SPACE) return;
	UINT8* p = pWrite[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) { p[a & I960_PAGE_MASK] = d; return; }
	WriteByteH[(uintptr_t)p](a, d);
}

void i960_bus_write16(uint32_t a, uint16_t d)
{
	a &= ~1u;
	if (a >= I960_SPACE) return;
	UINT8* p = pWrite[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) { *(UINT16*)(p + (a & I960_PAGE_MASK)) = d; return; }
	WriteWordH[(uintptr_t)p](a, d);
}

void i960_bus_write32(uint32_t a, uint32_t d)
{
	a &= ~3u;
	if (a >= I960_SPACE) return;
	UINT8* p = pWrite[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) { *(UINT32*)(p + (a & I960_PAGE_MASK)) = d; return; }
	WriteLongH[(uintptr_t)p](a, d);
}

uint16_t i960_bus_flags(uint32_t a)
{
	if (a >= I960_SPACE) return 0;
	return pFlags[a >> I960_PAGE_SHIFT];
}

uint32_t i960_bus_fetch32(uint32_t a)
{
	a &= ~3u;
	if (a >= I960_SPACE) return 0;
	UINT8* p = pFetch[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) return *(UINT32*)(p + (a & I960_PAGE_MASK));
	return i960_bus_read32(a);
}

void i960_log(const char* fmt, ...)
{
#if defined(FBNEO_DEBUG)
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bprintf(0, _T("%S"), buf);
#else
	(void)fmt;
#endif
}

// MAME stops there (fatalerror); the port logs the first few and carries on
static INT32 nFatalCount = 0;
void i960_fatal(const char* fmt, ...)
{
	if (nFatalCount >= 16)
		return;
	nFatalCount++;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bprintf(PRINT_ERROR, _T("%S"), buf);
}

// ---- map --------------------------------------------------------------------------------------------------------
INT32 I960MapMemory(UINT8* pMemory, UINT32 nStart, UINT32 nEnd, INT32 nType, UINT16 nFlags)
{
	if (nStart >= I960_SPACE) return 1;
	if (nEnd >= I960_SPACE) nEnd = I960_SPACE - 1;
	for (UINT32 page = nStart >> I960_PAGE_SHIFT, i = 0; page <= (nEnd >> I960_PAGE_SHIFT); page++, i++) {
		UINT8* p = pMemory + (i << I960_PAGE_SHIFT);
		if (nType & MAP_READ)  pRead[page] = p;
		if (nType & MAP_WRITE) pWrite[page] = p;
		if (nType & MAP_FETCH) pFetch[page] = p;
		pFlags[page] = nFlags;
	}
	return 0;
}

INT32 I960MapHandler(INT32 nHandler, UINT32 nStart, UINT32 nEnd, INT32 nType, UINT16 nFlags)
{
	if (nHandler < 0 || nHandler >= I960_MAXHANDLER || nStart >= I960_SPACE) return 1;
	if (nEnd >= I960_SPACE) nEnd = I960_SPACE - 1;
	for (UINT32 page = nStart >> I960_PAGE_SHIFT; page <= (nEnd >> I960_PAGE_SHIFT); page++) {
		if (nType & MAP_READ)  pRead[page] = (UINT8*)(uintptr_t)nHandler;
		if (nType & MAP_WRITE) pWrite[page] = (UINT8*)(uintptr_t)nHandler;
		if (nType & MAP_FETCH) pFetch[page] = (UINT8*)(uintptr_t)nHandler;
		pFlags[page] = nFlags;
	}
	return 0;
}

#define I960_SET_HANDLER(name, type, table) \
void name(INT32 i, type pHandler) { if (i >= 0 && i < I960_MAXHANDLER) table[i] = pHandler; }

I960_SET_HANDLER(I960SetReadByteHandler, pI960ReadByteHandler, ReadByteH)
I960_SET_HANDLER(I960SetWriteByteHandler, pI960WriteByteHandler, WriteByteH)
I960_SET_HANDLER(I960SetReadWordHandler, pI960ReadWordHandler, ReadWordH)
I960_SET_HANDLER(I960SetWriteWordHandler, pI960WriteWordHandler, WriteWordH)
I960_SET_HANDLER(I960SetReadLongHandler, pI960ReadLongHandler, ReadLongH)
I960_SET_HANDLER(I960SetWriteLongHandler, pI960WriteLongHandler, WriteLongH)

UINT8 I960ReadByte(UINT32 a) { return i960_bus_read8(a); }
UINT32 I960ReadLong(UINT32 a) { return i960_bus_read32(a); }
void I960WriteByte(UINT32 a, UINT8 d) { i960_bus_write8(a, d); }
void I960WriteLong(UINT32 a, UINT32 d) { i960_bus_write32(a, d); }

// ---- run --------------------------------------------------------------------------------------------------------
INT32 I960Run(INT32 cycles)
{
	if (cycles <= 0) return 0;
	nCyclesToDo = cycles;
	bRunning = 1;

	if (bHalted) {
		core->m_icount = 0; // halted: the time passes
	} else {
		core->m_icount = cycles;
		core->execute_run();
	}

	bRunning = 0;
	const INT32 done = nCyclesToDo - core->m_icount;
	nTotalCycles += done;
	nCyclesToDo = core->m_icount = 0;
	return done;
}

void I960RunEnd()
{
	if (!bRunning) return;
	nCyclesToDo -= core->m_icount;
	core->m_icount = 0;
}

INT32 I960Idle(INT32 cycles)
{
	nTotalCycles += cycles;
	return cycles;
}

INT64 I960TotalCycles()
{
	if (bRunning)
		return nTotalCycles + (nCyclesToDo - core->m_icount);
	return nTotalCycles;
}

void I960NewFrame()
{
	// nothing: the driver's timers count from the total
}

void I960BurnCycles(INT32 cycles)
{
	if (bRunning)
		core->m_icount -= cycles;
	else
		nTotalCycles += cycles;
}

void I960SetIRQLine(INT32 line, INT32 state)
{
	if (line < 0 || line > 3) return;
	core->execute_set_input(line, state ? 1 : 0);
	// raised while the CPU runs (by one of its own accesses): taken before its next instruction (MAME ends the
	// timeslice and execute_run checks it first thing)
	if (bRunning && core->m_immediate_irq)
		core->m_irq_raised = true;
}

void I960Stall()
{
	core->i960_stall();
}

void I960SetHalt(INT32 halt)
{
	if (halt && !bHalted) {
		bHalted = 1;
		I960RunEnd(); // MAME suspends the CPU at the end of the instruction
	} else if (!halt) {
		bHalted = 0;
	}
}

INT32 I960GetHalt()
{
	return bHalted;
}

UINT32 I960GetPC()
{
	return core->m_PIP;
}

// ---- reset / state ----------------------------------------------------------------------------------------------
void I960Reset()
{
	core->device_reset();
	bHalted = 0;
	core->m_stalled = false;
	core->m_irq_raised = false;
}

INT32 I960Scan(INT32 nAction)
{
	if (nAction & ACB_DRIVER_DATA) {
		SCAN_VAR(core->m_IP);
		SCAN_VAR(core->m_PIP);
		SCAN_VAR(core->m_SAT);
		SCAN_VAR(core->m_PRCB);
		SCAN_VAR(core->m_PC);
		SCAN_VAR(core->m_AC);
		SCAN_VAR(core->m_ICR);
		SCAN_VAR(core->m_r);
		SCAN_VAR(core->m_fp);
		SCAN_VAR(core->m_rcache);
		SCAN_VAR(core->m_rcache_frame_addr);
		SCAN_VAR(core->m_rcache_pos);
		SCAN_VAR(core->m_immediate_irq);
		SCAN_VAR(core->m_immediate_vector);
		SCAN_VAR(core->m_immediate_pri);
		SCAN_VAR(core->m_stalled);
		SCAN_VAR(core->m_stall_state.index);
		SCAN_VAR(core->m_stall_state.size);
		SCAN_VAR(core->m_stall_state.t1);
		SCAN_VAR(core->m_stall_state.t2);
		SCAN_VAR(core->m_stall_state.burst_mode);
		SCAN_VAR(core->m_stall_state.iswriteop);
		SCAN_VAR(core->m_irq_line_state);
		SCAN_VAR(core->m_irq_raised);
		SCAN_VAR(nTotalCycles);
		SCAN_VAR(bHalted);
	}
	return 0;
}

// ---- cpu core config (cheats) -----------------------------------------------------------------------------------
static void I960Open(INT32) { }
static void I960Close() { }
static INT32 I960GetActive() { return 0; }
static INT32 I960CoreTotalCycles() { return (INT32)I960TotalCycles(); }
static void I960CoreSetIRQ(INT32, INT32 line, INT32 state) { I960SetIRQLine(line, state); }
static void I960CoreReset() { I960Reset(); }
static INT32 I960CoreScan(INT32 nAction) { return I960Scan(nAction); }
static void I960CoreExit() { I960Exit(); }
static void I960CheatWrite(UINT32 a, UINT8 d)
{
	// RAM only (a handler page may be a FIFO or a register)
	if (a >= I960_SPACE) return;
	UINT8* p = pWrite[a >> I960_PAGE_SHIFT];
	if (I960_IS_PTR(p)) p[a & I960_PAGE_MASK] = d;
}
static UINT8 I960CheatRead(UINT32 a)
{
	if (a >= I960_SPACE) return 0;
	UINT8* p = pRead[a >> I960_PAGE_SHIFT];
	return I960_IS_PTR(p) ? p[a & I960_PAGE_MASK] : 0;
}

cpu_core_config I960Config =
{
	"i960",
	I960Open,
	I960Close,
	I960CheatRead,
	I960CheatWrite,
	I960GetActive,
	I960CoreTotalCycles,
	I960NewFrame,
	I960Idle,
	I960CoreSetIRQ,
	I960Run,
	I960RunEnd,
	I960CoreReset,
	I960CoreScan,
	I960CoreExit,
	I960_SPACE,
	0
};

void I960Init()
{
	core = new i960_core();
	core->device_start();
	pRead = (UINT8**)calloc(I960_PAGES, sizeof(UINT8*));
	pWrite = (UINT8**)calloc(I960_PAGES, sizeof(UINT8*));
	pFetch = (UINT8**)calloc(I960_PAGES, sizeof(UINT8*));
	pFlags = (UINT16*)calloc(I960_PAGES, sizeof(UINT16));
	for (INT32 i = 0; i < I960_MAXHANDLER; i++) {
		ReadByteH[i] = DefReadByte;
		ReadWordH[i] = DefReadWord;
		ReadLongH[i] = DefReadLong;
		WriteByteH[i] = DefWriteByte;
		WriteWordH[i] = DefWriteWord;
		WriteLongH[i] = DefWriteLong;
	}
	nTotalCycles = 0;
	nCyclesToDo = 0;
	bRunning = bHalted = 0;
	nFatalCount = 0;
	CpuCheatRegister(0, &I960Config);
	bInitted = 1;
}

void I960Exit()
{
	if (!bInitted) return;
	delete core;
	core = NULL;
	free(pRead); pRead = NULL;
	free(pWrite); pWrite = NULL;
	free(pFetch); pFetch = NULL;
	free(pFlags); pFlags = NULL;
	bInitted = 0;
}
