#ifndef MB86233_INTF
#define MB86233_INTF

// Fujitsu MB86233 / MB86234 "TGP" (MAME's interpreter, src/cpu/mb86233). One chip (Sega Model 2's geometrizer DSP).
// MAME counts its clock / 3 as cycles: an instruction is one cycle (two for the floating point ALU operations), so
// Model 2's 50 MHz chip runs 16.67 M cycles a second.
//
// Its four spaces are 32-bit words addressed by word: program (fetch and "mov mem (o)"), data, io (external), and
// the register file (16 words: Model 2's FIFOs and bank register). Program, data and io are mapped in pages of 256
// words (memory, or the space's handler); the register file always goes to its handler.

typedef UINT32 (*pMb86233ReadHandler)(UINT32 a);
typedef void (*pMb86233WriteHandler)(UINT32 a, UINT32 d);

#define MB86233_PROGRAM 0
#define MB86233_DATA    1
#define MB86233_IO      2

void Mb86233Init();
void Mb86233Exit();
void Mb86233Reset();                   // MAME's device_reset (INPUT_LINE_RESET pulsed)
INT32 Mb86233Run(INT32 cycles);
void Mb86233RunEnd();
INT32 Mb86233Idle(INT32 cycles);
INT64 Mb86233TotalCycles();
void Mb86233NewFrame();
void Mb86233Stall();                   // MAME's stall(): the instruction reading is made again
void Mb86233SetHalt(INT32 halt);       // INPUT_LINE_HALT
INT32 Mb86233GetHalt();
void Mb86233SetGpio(INT32 n, INT32 state); // gpio0..3 (branch conditions)
UINT32 Mb86233GetPC();
INT32 Mb86233Scan(INT32 nAction);

// a page of 256 words (nStart, nEnd: word numbers)
INT32 Mb86233MapMemory(INT32 nSpace, UINT32* pMemory, UINT32 nStart, UINT32 nEnd, INT32 nType);
void Mb86233SetReadHandler(INT32 nSpace, pMb86233ReadHandler pHandler);
void Mb86233SetWriteHandler(INT32 nSpace, pMb86233WriteHandler pHandler);
void Mb86233SetRfHandlers(pMb86233ReadHandler pRead, pMb86233WriteHandler pWrite);

#endif
