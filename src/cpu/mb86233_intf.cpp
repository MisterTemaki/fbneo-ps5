// Fujitsu MB86233/MB86234 interface (FBNeo PS5 2.3): the paged spaces, the run loop and the state of the core in
// mb86233/ (MAME's interpreter, BSD-3-Clause, Olivier Galibert).
// SPDX-License-Identifier: BSD-3-Clause

#include "burnint.h"
#include "mb86233_intf.h"
#include "mb86233/mb86233.h"

#include <stdarg.h>

#define TGP_PAGE_SHIFT 8
#define TGP_PAGES      (0x10000 >> TGP_PAGE_SHIFT)

static mb86233_core* core = NULL;

static UINT32* pRead[3][TGP_PAGES];
static UINT32* pWrite[3][TGP_PAGES];
static pMb86233ReadHandler ReadH[3];
static pMb86233WriteHandler WriteH[3];
static pMb86233ReadHandler RfRead;
static pMb86233WriteHandler RfWrite;

static INT32 nCyclesToDo = 0;
static INT64 nTotalCycles = 0;
static INT32 bRunning = 0;
static INT32 bHalted = 0;
static INT32 bInitted = 0;

static UINT32 DefRead(UINT32) { return 0; }
static void DefWrite(UINT32, UINT32) { }

// ---- the spaces, as the core calls them -----------------------------------------------------------------------------
u32 mb86233_program_read(u32 a)
{
	UINT32* p = pRead[MB86233_PROGRAM][a >> TGP_PAGE_SHIFT];
	return p ? p[a & 0xff] : ReadH[MB86233_PROGRAM](a);
}

u32 mb86233_data_read(u32 a)
{
	UINT32* p = pRead[MB86233_DATA][a >> TGP_PAGE_SHIFT];
	return p ? p[a & 0xff] : ReadH[MB86233_DATA](a);
}

void mb86233_data_write(u32 a, u32 d)
{
	UINT32* p = pWrite[MB86233_DATA][a >> TGP_PAGE_SHIFT];
	if (p) p[a & 0xff] = d;
	else WriteH[MB86233_DATA](a, d);
}

u32 mb86233_io_read(u32 a)
{
	UINT32* p = pRead[MB86233_IO][a >> TGP_PAGE_SHIFT];
	return p ? p[a & 0xff] : ReadH[MB86233_IO](a);
}

void mb86233_io_write(u32 a, u32 d)
{
	UINT32* p = pWrite[MB86233_IO][a >> TGP_PAGE_SHIFT];
	if (p) p[a & 0xff] = d;
	else WriteH[MB86233_IO](a, d);
}

u32 mb86233_rf_read(u32 a)
{
	return RfRead(a);
}

void mb86233_rf_write(u32 a, u32 d)
{
	RfWrite(a, d);
}

void mb86233_log(const char* fmt, ...)
{
#if defined(FBNEO_DEBUG)
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bprintf(0, _T("mb86233: %s"), buf);
#else
	(void)fmt;
#endif
}

// ---- map ----------------------------------------------------------------------------------------------------------
INT32 Mb86233MapMemory(INT32 nSpace, UINT32* pMemory, UINT32 nStart, UINT32 nEnd, INT32 nType)
{
	if (nSpace < 0 || nSpace > 2 || nEnd > 0xffff) return 1;
	for (UINT32 page = nStart >> TGP_PAGE_SHIFT, i = 0; page <= (nEnd >> TGP_PAGE_SHIFT); page++, i++) {
		UINT32* p = pMemory ? pMemory + (i << TGP_PAGE_SHIFT) : NULL;
		if (nType & MAP_READ) pRead[nSpace][page] = p;
		if (nType & MAP_WRITE) pWrite[nSpace][page] = p;
	}
	return 0;
}

void Mb86233SetReadHandler(INT32 nSpace, pMb86233ReadHandler pHandler)
{
	if (nSpace >= 0 && nSpace <= 2) ReadH[nSpace] = pHandler ? pHandler : DefRead;
}

void Mb86233SetWriteHandler(INT32 nSpace, pMb86233WriteHandler pHandler)
{
	if (nSpace >= 0 && nSpace <= 2) WriteH[nSpace] = pHandler ? pHandler : DefWrite;
}

void Mb86233SetRfHandlers(pMb86233ReadHandler pRead, pMb86233WriteHandler pWrite)
{
	RfRead = pRead ? pRead : DefRead;
	RfWrite = pWrite ? pWrite : DefWrite;
}

// ---- run ----------------------------------------------------------------------------------------------------------
INT32 Mb86233Run(INT32 cycles)
{
	if (cycles <= 0) return 0;
	nCyclesToDo = cycles;
	bRunning = 1;
	if (bHalted) {
		core->m_icount = 0;
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

void Mb86233RunEnd()
{
	if (!bRunning) return;
	nCyclesToDo -= core->m_icount;
	core->m_icount = 0;
}

INT32 Mb86233Idle(INT32 cycles)
{
	nTotalCycles += cycles;
	return cycles;
}

INT64 Mb86233TotalCycles()
{
	if (bRunning)
		return nTotalCycles + (nCyclesToDo - core->m_icount);
	return nTotalCycles;
}

void Mb86233NewFrame()
{
}

void Mb86233Stall()
{
	core->stall();
}

void Mb86233SetHalt(INT32 halt)
{
	if (halt && !bHalted) {
		bHalted = 1;
		Mb86233RunEnd();
	} else if (!halt) {
		bHalted = 0;
	}
}

INT32 Mb86233GetHalt()
{
	return bHalted;
}

void Mb86233SetGpio(INT32 n, INT32 state)
{
	switch (n) {
		case 0: core->gpio0_w(state); break;
		case 1: core->gpio1_w(state); break;
		case 2: core->gpio2_w(state); break;
		case 3: core->gpio3_w(state); break;
	}
}

UINT32 Mb86233GetPC()
{
	return core->m_ppc;
}

// the core itself (src/cpu/mb86233/mb86233.h): the tests compare its registers with MAME's
mb86233_core* Mb86233Core()
{
	return core;
}

void Mb86233Reset()
{
	core->device_reset();
}

INT32 Mb86233Scan(INT32 nAction)
{
	if (nAction & ACB_DRIVER_DATA) {
		SCAN_VAR(core->m_ppc);
		SCAN_VAR(core->m_pc);
		SCAN_VAR(core->m_st);
		SCAN_VAR(core->m_sp);
		SCAN_VAR(core->m_a);
		SCAN_VAR(core->m_b);
		SCAN_VAR(core->m_d);
		SCAN_VAR(core->m_p);
		SCAN_VAR(core->m_r);
		SCAN_VAR(core->m_rpc);
		SCAN_VAR(core->m_c0);
		SCAN_VAR(core->m_c1);
		SCAN_VAR(core->m_b0);
		SCAN_VAR(core->m_b1);
		SCAN_VAR(core->m_x0);
		SCAN_VAR(core->m_x1);
		SCAN_VAR(core->m_i0);
		SCAN_VAR(core->m_i1);
		SCAN_VAR(core->m_sft);
		SCAN_VAR(core->m_vsm);
		SCAN_VAR(core->m_vsmr);
		SCAN_VAR(core->m_pcs);
		SCAN_VAR(core->m_mask);
		SCAN_VAR(core->m_m);
		SCAN_VAR(core->m_gpio0);
		SCAN_VAR(core->m_gpio1);
		SCAN_VAR(core->m_gpio2);
		SCAN_VAR(core->m_gpio3);
		SCAN_VAR(core->m_alu_stmask);
		SCAN_VAR(core->m_alu_stset);
		SCAN_VAR(core->m_alu_r1);
		SCAN_VAR(core->m_alu_r2);
		SCAN_VAR(core->m_stall);
		SCAN_VAR(nTotalCycles);
		SCAN_VAR(bHalted);
	}
	return 0;
}

void Mb86233Init()
{
	core = new mb86233_core();
	for (INT32 s = 0; s < 3; s++) {
		for (INT32 p = 0; p < TGP_PAGES; p++)
			pRead[s][p] = pWrite[s][p] = NULL;
		ReadH[s] = DefRead;
		WriteH[s] = DefWrite;
	}
	RfRead = DefRead;
	RfWrite = DefWrite;
	core->m_gpio0 = core->m_gpio1 = core->m_gpio2 = core->m_gpio3 = false;
	core->device_reset();
	nTotalCycles = 0;
	nCyclesToDo = 0;
	bRunning = bHalted = 0;
	bInitted = 1;
}

void Mb86233Exit()
{
	if (!bInitted) return;
	delete core;
	core = NULL;
	bInitted = 0;
}
