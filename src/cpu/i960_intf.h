#ifndef I960_INTF
#define I960_INTF

// Intel 80960KA/KB (MAME's interpreter, src/cpu/i960), little-endian 32-bit bus.
// One CPU (Sega Model 2's main CPU). The address space is mapped in 4 KB pages, up to 0x1fffffff (0x20000000 and up:
// unmapped, reads 0). A page is memory (read, write and fetch pointers) or a handler (0..I960_MAXHANDLER-1; handler 0
// is the default "unmapped" one until a driver sets its own). Each page has MAME's flags for it (I960_BURST: a
// multi-word load or store (ldl/ldt/ldq/stl...) moves to the next word; without it, every word of the access goes to
// the same address -- MAME's flags(i960_cpu_device::BURST) on a map entry, which Model 2's FIFO ports leave out).

#define I960_MAXHANDLER   8
#define I960_BURST        0x0001

#define I960_IRQ0 0
#define I960_IRQ1 1
#define I960_IRQ2 2
#define I960_IRQ3 3

typedef UINT8 (*pI960ReadByteHandler)(UINT32 a);
typedef void (*pI960WriteByteHandler)(UINT32 a, UINT8 d);
typedef UINT16 (*pI960ReadWordHandler)(UINT32 a);   // a even
typedef void (*pI960WriteWordHandler)(UINT32 a, UINT16 d);
typedef UINT32 (*pI960ReadLongHandler)(UINT32 a);   // a a multiple of 4
typedef void (*pI960WriteLongHandler)(UINT32 a, UINT32 d);

void I960Init();
void I960Exit();
void I960Reset();                 // MAME's device_reset: SAT, PRCB and IP from the first words of the space
INT32 I960Run(INT32 cycles);      // returns the cycles run (a little more than asked when an instruction overruns)
void I960RunEnd();                // ends the current I960Run after this instruction
INT32 I960Idle(INT32 cycles);
INT64 I960TotalCycles();          // includes the current I960Run's cycles so far
void I960NewFrame();
void I960BurnCycles(INT32 cycles);// time spent in a handler (wait states, waiting on another chip)
void I960SetIRQLine(INT32 line, INT32 state); // I960_IRQ0..3, CPU_IRQSTATUS_ACK / CPU_IRQSTATUS_NONE
void I960Stall();                 // MAME's i960_stall(): the access in progress is made again later
void I960SetHalt(INT32 halt);     // MAME's INPUT_LINE_HALT (a halted CPU idles through I960Run)
INT32 I960GetHalt();
UINT32 I960GetPC();               // the instruction being run (MAME's PIP)
INT32 I960Scan(INT32 nAction);

INT32 I960MapMemory(UINT8* pMemory, UINT32 nStart, UINT32 nEnd, INT32 nType, UINT16 nFlags);
INT32 I960MapHandler(INT32 nHandler, UINT32 nStart, UINT32 nEnd, INT32 nType, UINT16 nFlags);

void I960SetReadByteHandler(INT32 i, pI960ReadByteHandler pHandler);
void I960SetWriteByteHandler(INT32 i, pI960WriteByteHandler pHandler);
void I960SetReadWordHandler(INT32 i, pI960ReadWordHandler pHandler);
void I960SetWriteWordHandler(INT32 i, pI960WriteWordHandler pHandler);
void I960SetReadLongHandler(INT32 i, pI960ReadLongHandler pHandler);
void I960SetWriteLongHandler(INT32 i, pI960WriteLongHandler pHandler);

// bus access as the CPU makes it (tests, cheats, the driver's own DMA)
UINT8 I960ReadByte(UINT32 a);
UINT32 I960ReadLong(UINT32 a);
void I960WriteByte(UINT32 a, UINT8 d);
void I960WriteLong(UINT32 a, UINT32 d);

extern struct cpu_core_config I960Config;

#endif
