// FBNeo PS5 2.3: Sega Model 2 (original board), Daytona USA -- ported from MAME (mamedev/mame b7f483f):
//   src/mame/sega/model2.cpp / model2.h (model2o_state::daytona; R. Belmont, Olivier Galibert, ElSemi, Angelo Salese,
//   Matthew Daniels), model2_v.cpp / model2rd.ipp (here model2_v.cpp, model2rd.ipp: the geometrizer and rasterizer),
//   src/mame/sega/segaic24.cpp (the System 24 tilemaps; Olivier Galibert), src/mame/sega/model1io.cpp (the Model 1
//   I/O board; Dirk Best) with 315_5338a.cpp and src/devices/machine/msm6253.cpp (AJR), src/mame/shared/segam1audio.cpp
//   (the Model 1 sound board; R. Belmont), src/devices/machine/i8251.cpp (the uPD71051 link; Wilbert Pol, Miodrag
//   Milanovic), src/devices/machine/gen_fifo.cpp (the copro FIFOs; Olivier Galibert), src/mame/sega/m2comm.cpp (the
//   link board, off: Ariane Fugmann). All BSD-3-Clause, as this file is.
//
// The CPUs are MAME's: the i960KB (src/cpu/i960) at 25 MHz and the TGP, an MB86234 (src/cpu/mb86233) at 50 MHz, its
// program uploaded by the i960; the 68000 (Musashi), the YM3438 and two MultiPCM (FBNeo's) on the sound board; a Z80
// on the I/O board. What is not emulated: the drive board (force feedback: its Z80, 315-5296 and MSM6253; the game
// writes to it and reads nothing back, as in MAME), the link board (one cabinet; MAME's m2comm without the
// simulation), the lamps and coin counters.
//
// SPDX-License-Identifier: BSD-3-Clause

#include "tiles_generic.h"
#include "m68000_intf.h"
#include "z80_intf.h"
#include "i960_intf.h"
#include "mb86233_intf.h"
#include "burn_ym2612.h"
#include "multipcm.h"
#include "eeprom.h"
#include "model2.h"
#include "burn_parallel.h"

#include <stdarg.h>
#include <math.h>
#include <time.h>

// ---------------------------------------------------------------------------------------------------------------------
// timing: the screen is MAME's raw params (32 MHz / 2, 656 x 424): a line is 41 us, 1025 i960 cycles, 410 68000 cycles,
// 164 Z80 cycles; vblank starts at line 384. The TGP's cycles are MAME's clock / 3 (16.67 MHz): 2/3 of the i960's.
#define M2_I960_CLOCK     25000000
#define M2_LINES          424
#define M2_VBLANK_LINE    384
#define M2_LINE_CYCLES    1025
#define M2_FRAME_CYCLES   (M2_LINES * M2_LINE_CYCLES)
#define M2_SND_CLOCK      10000000
#define M2_SND_LINE       410
#define M2_IO_LINE        164
#define M2_WIDTH          496
#define M2_HEIGHT         384

static UINT8 *AllMem, *MemEnd, *AllRam, *RamEnd;
static UINT8 *DrvI960ROM;     // 0x200000 (maincpu)
static UINT8 *DrvMainData;    // 0x1000000 used of the 0x2000000 region (the rest reads 0)
static UINT8 *DrvCoproData;   // 0x400000 used of 0x800000
static UINT8 *DrvPolyROM;     // 0x1000000
static UINT8 *DrvTexROM;      // 0x1000000
static UINT8 *DrvTGPTables;   // 0x40000
static UINT8 *Drv68KROM;      // 0x40000 (sound board)
static UINT8 *DrvPCM1, *DrvPCM2; // 0x400000 each
static UINT8 *DrvIOROM;       // 0x10000 (I/O board)
static UINT8 *DrvZeroPage;    // 0x1000 of zeros

static UINT8 *DrvWorkRAM;     // 0x100000
static UINT8 *DrvRAM200;      // 0x20000 (model2o)
static UINT8 *DrvBufferRAM;   // 0x20000
static UINT8 *DrvTileRAM;     // 0x10000
static UINT8 *DrvCharRAM;     // 0x80000
static UINT8 *DrvPalRAM;      // 0x4000
static UINT8 *DrvColorXlat;   // 0xc000
static UINT8 *DrvBackupRAM;   // 0x4000 (NVRAM, "backup1")
static UINT8 *DrvCommRAM;     // 0x4000 (m2comm shared RAM)
static UINT8 *DrvCtrlRAM;     // 0x1000 (0xe00000 CPU control)
static UINT8 *DrvTexRAM0, *DrvTexRAM1; // 0x200000 each
static UINT8 *DrvLumaRAM;     // 0x8000
static UINT8 *DrvFbVRAMA, *DrvFbVRAMB; // 0x80000 each
static UINT32 *DrvTGPProgram; // 0x1000 words
static UINT32 *DrvTGPData;    // 0x400 words (0x000-0x0ff, 0x200-0x3ff)
static UINT8 *Drv68KRAM;      // 0x10000
static UINT8 *DrvIORAM;       // 0x2000
static UINT8 *DrvDPRAM;       // 0x800 (MB8421)

static UINT32 *DrvPalette;    // 0x2000 pens (MAME's palette: alpha 0xff)
static UINT8 DrvRecalc;

static model2_state *m2 = NULL;
static model2_renderer *m2_renderer = NULL;
static m2_bitmap<UINT32> *screen_bitmap = NULL;  // 496 x 384, the frame as MAME's screen_update makes it
static m2_bitmap<UINT32> *sys24_bitmap = NULL;
static UINT8 *char_shadow = NULL;                // the char RAM as the tile pixmaps were last made from it
static UINT16 *tile_cache = NULL;                // 4 x 0x1000: the tile word each pixmap tile was made from
static UINT16 *tile_pix = NULL;                  // 4 x 512 x 512 pens
static UINT8 *tile_flags = NULL;                 // 4 x 512 x 512 (MAME's tilemap flags: category | LAYER0)
static UINT8 *char_dirty = NULL;                 // 0x4000
static INT32 tiles_all_dirty = 1;

// ---- the driver's state (saved) -------------------------------------------------------------------------------------
static UINT32 timervals[4], timerorig[4];
static INT32 timerrun[4];
static INT64 timer_start[4], timer_fire[4];
static UINT32 intreq, intena;
static INT32 intena_pending;
static UINT32 intena_pending_value;
static INT64 intena_pending_at;
static UINT32 coproctl, coprocnt, geoctl, geocnt;
static UINT32 videocontrol;
static UINT32 framenum;
static UINT32 geo_write_start_address;
static bool render_unk, render_mode, palette_dirty;
static UINT32 copro_tgp_bank_reg, copro_sincos_base, copro_inv_base, copro_isqrt_base, copro_atan_base[4];
static UINT8 gearsel;
static UINT8 comm_cn, comm_fg, comm_zfg;
static UINT8 io_port_value[7], io_port_config, io_serial_output, io_cmd;
static UINT16 io_address;
static UINT8 io_secondary;
static UINT8 adc_shift;
static UINT8 mpcm_bank[2];
static UINT8 shift_prev[2];

// i960/TGP halt (MAME's INPUT_LINE_HALT of each, one line for every reason)
static INT32 i960_halt_line, tgp_halt_line;

// ---- the FIFOs (MAME's generic_fifo_u32: 8 values, then the overflow the writer waits on) ---------------------------
#define M2_FIFO_CAP   8
#define M2_FIFO_RING  64
struct m2_fifo {
	UINT32 v[M2_FIFO_RING];
	INT32 head, count;
	UINT8 empty_triggered, full_triggered;
};
static m2_fifo fifo_in, fifo_out; // copro in (i960 -> TGP), copro out (TGP -> i960)

static void fifo_clear(m2_fifo *f)
{
	f->head = f->count = 0;
	f->full_triggered = 0; // MAME: clear() fires on_fifo_unfull if full was triggered, and leaves empty_triggered
}

static inline UINT32 fifo_pop_raw(m2_fifo *f)
{
	UINT32 d = f->v[f->head];
	f->head = (f->head + 1) % M2_FIFO_RING;
	f->count--;
	return d;
}

static inline void fifo_push_raw(m2_fifo *f, UINT32 d)
{
	if (f->count >= M2_FIFO_RING) {
		bprintf(PRINT_ERROR, _T("model2: FIFO overflow, value dropped\n"));
		return;
	}
	f->v[(f->head + f->count) % M2_FIFO_RING] = d;
	f->count++;
}

// ---- inputs ---------------------------------------------------------------------------------------------------------
static UINT8 DrvJoy1[8];   // IN0 bits (active low)
static UINT8 DrvJoy2[8];   // IN1 bits
static UINT8 DrvJoy3[8];   // gear buttons (shift up / down)
static UINT8 DrvReset;
static INT16 DrvAnalogPort0 = 0; // steering
static INT16 DrvAnalogPort1 = 0; // accelerator
static INT16 DrvAnalogPort2 = 0; // brake
static UINT8 DrvInputs[2];
static UINT8 AnalogValue[3];

#define A(a, b, c, d) {a, b, (UINT8*)(c), d}

static struct BurnInputInfo DaytonaInputList[] = {
	{"P1 Coin",          BIT_DIGITAL,    DrvJoy1 + 0,  "p1 coin"   },
	{"P1 Start",         BIT_DIGITAL,    DrvJoy1 + 4,  "p1 start"  },
	A("P1 Steering",     BIT_ANALOG_REL, &DrvAnalogPort0, "p1 x-axis"),
	A("P1 Accelerator",  BIT_ANALOG_REL, &DrvAnalogPort1, "p1 fire 1"),
	A("P1 Brake",        BIT_ANALOG_REL, &DrvAnalogPort2, "p1 fire 2"),
	{"P1 Shift Up",      BIT_DIGITAL,    DrvJoy3 + 0,  "p1 fire 3" },
	{"P1 Shift Down",    BIT_DIGITAL,    DrvJoy3 + 1,  "p1 fire 4" },
	{"P1 VR1 Red",       BIT_DIGITAL,    DrvJoy1 + 5,  "p1 fire 5" },
	{"P1 VR2 Blue",      BIT_DIGITAL,    DrvJoy1 + 6,  "p1 fire 6" },
	{"P1 VR3 Yellow",    BIT_DIGITAL,    DrvJoy1 + 7,  "p1 fire 7" },
	{"P1 VR4 Green",     BIT_DIGITAL,    DrvJoy2 + 0,  "p1 fire 8" },
	{"P2 Coin",          BIT_DIGITAL,    DrvJoy1 + 1,  "p2 coin"   },

	{"Reset",            BIT_DIGITAL,    &DrvReset,    "reset"     },
	{"Service",          BIT_DIGITAL,    DrvJoy1 + 3,  "service"   },
	{"Test",             BIT_DIGITAL,    DrvJoy1 + 2,  "diag"      },
};

STDINPUTINFO(Daytona)

// ---- logging, and where the time goes (with the port's debug logs on: one line a second) ---------------------------
void m2_log(const char* fmt, ...)
{
#if defined(FBNEO_DEBUG)
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bprintf(0, _T("model2: %s"), buf);
#else
	(void)fmt;
#endif
}

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline UINT64 m2_ticks() { return __rdtsc(); }
#else
static inline UINT64 m2_ticks() { return (UINT64)clock() * 1000; }
#endif
enum { PART_I960, PART_TGP, PART_SND, PART_IO, PART_GEO, PART_3D, PART_2D, PART_COUNT };
static UINT64 part_ticks[PART_COUNT];
static INT32 perf_frames;
static UINT64 perf_tick_start;
static struct timespec perf_ts_start;
static INT64 perf_tgp_ahead; // TGP cycles run ahead of the i960 (it waited for the TGP), this second
static INT32 perf_polys;

static void perf_report()
{
	if (++perf_frames < 57) return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	const double secs = (now.tv_sec - perf_ts_start.tv_sec) + (now.tv_nsec - perf_ts_start.tv_nsec) / 1e9;
	const UINT64 ticks = m2_ticks() - perf_tick_start;
	if (secs > 0.1 && ticks > 0) {
		const double ms_per_tick = secs * 1000.0 / (double)ticks;
		const double f = 1.0 / perf_frames;
		bprintf(PRINT_IMPORTANT, _T("model2: ms a frame (%d frames, %.2f s): i960 %.2f, TGP %.2f, sound %.2f, I/O %.2f, geometrizer %.2f, 3D %.2f, 2D %.2f; TGP ahead %d cycles a frame, %d polygons a frame\n"),
			perf_frames, secs, part_ticks[PART_I960] * ms_per_tick * f, part_ticks[PART_TGP] * ms_per_tick * f,
			part_ticks[PART_SND] * ms_per_tick * f, part_ticks[PART_IO] * ms_per_tick * f, part_ticks[PART_GEO] * ms_per_tick * f,
			part_ticks[PART_3D] * ms_per_tick * f,
			part_ticks[PART_2D] * ms_per_tick * f, (INT32)(perf_tgp_ahead / perf_frames), perf_polys / perf_frames);
	}
	memset(part_ticks, 0, sizeof(part_ticks));
	perf_frames = 0;
	perf_tgp_ahead = 0;
	perf_polys = 0;
	perf_tick_start = m2_ticks();
	perf_ts_start = now;
}

// ---- palette (MAME's palette_w / colorxlat_w / the refresh in screen_update) ----------------------------------------
static inline UINT32 pen_from(UINT16 palcolor)
{
	const UINT16 *colorxlat = (UINT16*)DrvColorXlat;
	UINT8 r = colorxlat[(0x0080 >> 1) + (((palcolor >> 0) & 0x1f) << 8)];
	UINT8 g = colorxlat[(0x4080 >> 1) + (((palcolor >> 5) & 0x1f) << 8)];
	UINT8 b = colorxlat[(0x8080 >> 1) + (((palcolor >> 10) & 0x1f) << 8)];
	r = m2->m_gamma_table[r];
	g = m2->m_gamma_table[g];
	b = m2->m_gamma_table[b];
	return 0xff000000 | (r << 16) | (g << 8) | b;
}

static void palette_recalc_all()
{
	const UINT16 *pal = (UINT16*)DrvPalRAM;
	for (INT32 i = 0; i < 0x2000; i++)
		DrvPalette[i] = pen_from(pal[i]);
}

static void palette_write16(UINT32 offset, UINT16 data, UINT16 mem_mask)
{
	UINT16 *pal = (UINT16*)DrvPalRAM;
	offset &= 0x1fff;
	pal[offset] = (pal[offset] & ~mem_mask) | (data & mem_mask);
	DrvPalette[offset] = pen_from(pal[offset]);
}

static void colorxlat_write16(UINT32 offset, UINT16 data, UINT16 mem_mask)
{
	UINT16 *cx = (UINT16*)DrvColorXlat;
	if (offset >= 0x6000) return;
	cx[offset] = (cx[offset] & ~mem_mask) | (data & mem_mask);
	// if writing to the scroll color table, mark the palette as dirty
	if ((offset & 0xff) == 0x80 / 2)
		palette_dirty = true;
}

// ---- System 24 tilemaps (MAME's segaic24.cpp: segas24_tile_device, the rgb32 drawing Model 1/2 use) ------------------
// Four 64x64 tilemaps of 8x8 tiles, 4 bpp, kept as MAME's tilemap pixmaps (pen = colour * 16 + pixel) and flag maps
// (TILEMAP_PIXEL_LAYER0 0x10 for an opaque pixel | the tile's category, bit 15 of its word). A tile is drawn again when
// its word or its character changed (the character RAM is compared with the copy its tiles were made from).
#define S24_LAYER0      0x10
#define S24_DRAW_OPAQUE 0x80
#define S24_TILE_MASK   0x3fff

static void tiles_update()
{
	const UINT16 *tile_ram = (UINT16*)DrvTileRAM;
	const UINT16 *char_ram = (UINT16*)DrvCharRAM;

	for (INT32 c = 0; c < 0x4000; c++) {
		UINT8 *now = DrvCharRAM + c * 32;
		UINT8 *was = char_shadow + c * 32;
		if (tiles_all_dirty || memcmp(now, was, 32)) {
			memcpy(was, now, 32);
			char_dirty[c] = 1;
		} else {
			char_dirty[c] = 0;
		}
	}

	for (INT32 layer = 0; layer < 4; layer++) {
		for (INT32 t = 0; t < 0x1000; t++) {
			const UINT16 val = tile_ram[t | (layer << 12)];
			const UINT32 code = val & S24_TILE_MASK;
			UINT16 &cached = tile_cache[(layer << 12) | t];
			if (!tiles_all_dirty && val == cached && !char_dirty[code])
				continue;
			cached = val;
			const UINT16 color = ((val >> 7) & 0xff) << 4;
			const UINT8 category = (val & 0x8000) ? 1 : 0;
			const UINT16 *ch = char_ram + code * 16;
			const INT32 tx = (t & 63) * 8, ty = (t >> 6) * 8;
			UINT16 *pix = tile_pix + (layer << 18) + (ty << 9) + tx;
			UINT8 *fl = tile_flags + (layer << 18) + (ty << 9) + tx;
			for (INT32 y = 0; y < 8; y++, pix += 512, fl += 512) {
				// MAME's char_layout with the little-endian xor: pixel 0 is the top nibble of the row's first word
				const UINT32 row = ch[y * 2] | (ch[y * 2 + 1] << 16);
				for (INT32 x = 0; x < 8; x++) {
					const INT32 shift = (x < 4) ? (12 - 4 * x) : (28 - 4 * (x - 4));
					const UINT8 p = (row >> shift) & 0x0f;
					pix[x] = color | p;
					fl[x] = (p ? S24_LAYER0 : 0) | category;
				}
			}
		}
	}
	tiles_all_dirty = 0;
}

// MAME's tilemap_t::draw for these maps: scrollx / scrolly as set_scrollx / set_scrolly take them (the picture at x
// shows the map's (x + scrollx) & 511), the flags a pixel needs as configure_blit_parameters makes them
static void tilemap_draw(m2_bitmap<UINT32> &bitmap, const m2_rectangle &clip, INT32 layer, INT32 scrollx, INT32 scrolly, INT32 flags)
{
	if (clip.min_x > clip.max_x || clip.min_y > clip.max_y) return;
	const UINT8 category = flags & 0x0f;
	const UINT8 mask = (flags & S24_DRAW_OPAQUE) ? 0x0f : (0x0f | S24_LAYER0);
	const UINT8 value = (flags & S24_DRAW_OPAQUE) ? category : (category | S24_LAYER0);
	const UINT16 *pixmap = tile_pix + (layer << 18);
	const UINT8 *flagsmap = tile_flags + (layer << 18);
	for (INT32 y = clip.min_y; y <= clip.max_y; y++) {
		const INT32 sy = (y + scrolly) & 511;
		UINT32 *dst = &bitmap.pix(y, 0);
		for (INT32 x = clip.min_x; x <= clip.max_x; x++) {
			const INT32 sx = (x + scrollx) & 511;
			if ((flagsmap[(sy << 9) | sx] & mask) == value)
				dst[x] = DrvPalette[pixmap[(sy << 9) | sx]];
		}
	}
}

// segas24_tile_device::draw_rect (the rgb32 one)
static void s24_draw_rect(INT32 layer, m2_bitmap<UINT32> &dm, const UINT16 *mask, UINT16 tpri, INT32 flags, INT32 win,
	INT32 sx, INT32 sy, INT32 xx1, INT32 yy1, INT32 xx2, INT32 yy2)
{
	const UINT16 *source = tile_pix + (layer << 18) + (sy << 9) + sx;
	const UINT8  *trans = tile_flags + (layer << 18) + (sy << 9) + sx;
	UINT32       *dest = &dm.pix(yy1, xx1);
	const UINT32 *pens = DrvPalette;

	tpri |= S24_LAYER0;

	mask += yy1*4;
	yy2 -= yy1;

	while(xx1 >= 128) {
		xx1 -= 128;
		xx2 -= 128;
		mask++;
	}

	for(INT32 y=0; y<yy2; y++) {
		const UINT16 *src   = source;
		const UINT8  *srct  = trans;
		UINT32 *dst         = dest;
		const UINT16 *mask1 = mask;
		INT32 llx = xx2;
		INT32 cur_x = xx1;

		while(llx > 0) {
			UINT16 m = *mask1++;

			if(win)
				m = ~m;

			if(!cur_x && llx>=128) {
				// Fast paths for the 128-pixels without side clipping case

				if(!m) {
					// 1- 128 pixels from this layer
					for(INT32 x=0; x<128; x++) {
						if(*srct++ == tpri || flags & S24_DRAW_OPAQUE)
							*dst = pens[*src];
						src++;
						dst++;
					}

				} else if(m == 0xffff) {
					// 2- 128 pixels from the other layer
					src += 128;
					srct += 128;
					dst += 128;

				} else {
					// 3- 128 pixels from both layers
					for(INT32 x=0; x<128; x+=8) {
						if(!(m & 0x8000)) {
							for(INT32 xx=0; xx<8; xx++)
								if(srct[xx] == tpri || flags & S24_DRAW_OPAQUE)
									dst[xx] = pens[src[xx]];
						}
						src += 8;
						srct += 8;
						dst += 8;
						m <<= 1;
					}
				}
			} else {
				// Clipped path
				INT32 llx1 = llx >= 128 ? 128 : llx;

				if(!m) {
					// 1- 128 pixels from this layer
					for(INT32 x = cur_x; x<llx1; x++) {
						if(*srct++ == tpri || flags & S24_DRAW_OPAQUE)
							*dst = pens[*src];
						src++;
						dst++;
					}

				} else if(m == 0xffff) {
					// 2- 128 pixels from the other layer
					src += 128 - cur_x;
					srct += 128 - cur_x;
					dst += 128 - cur_x;

				} else {
					// 3- 128 pixels from both layers
					INT32 x;
					for(x=cur_x; x<llx1; x++) {
						if((*srct++ == tpri || flags & S24_DRAW_OPAQUE) && !(m & (0x8000 >> (x >> 3))))
							*dst = pens[*src];

						src++;
						dst++;
					}
				}
			}
			llx -= 128;
			cur_x = 0;
		}
		source += 512;
		trans  += 512;
		dest   += dm.rowpixels();
		mask   += 4;
	}
}

// segas24_tile_device::draw_common (lpri, the priority bitmap's, is not used by Model 2)
static void s24_draw(m2_bitmap<UINT32> &bitmap, const m2_rectangle &cliprect, INT32 layer, INT32 flags)
{
	const UINT16 *tile_ram = (UINT16*)DrvTileRAM;
	UINT16 hscr = tile_ram[0x5000+(layer >> 1)];
	UINT16 vscr = tile_ram[0x5004+(layer >> 1)];
	UINT16 ctrl = tile_ram[0x5004+((layer >> 1) & 2)];
	const UINT16 *mask = tile_ram + (layer & 4 ? 0x6800 : 0x6000);
	UINT16 tpri = layer & 1;

	layer >>= 1;

	// Layer disable
	if(vscr & 0x8000)
		return;

	if(ctrl & 0x6000) {
		// Special window/scroll modes
		if(layer & 1)
			return;

		const INT32 scrolly = vscr & 0x1ff;

		if(hscr & 0x8000) {
			const UINT16 *hscrtb = tile_ram + 0x4000 + 0x200*layer;

			switch((ctrl & 0x6000) >> 13) {
			case 1: {
				UINT16 v = (-vscr) & 0x1ff;
				if(!((-vscr) & 0x200))
					layer ^= 1;
				for(INT32 y=cliprect.top(); y<=cliprect.bottom(); y++) {
					m2_rectangle c = cliprect;
					INT32 l1 = layer;
					if(y >= v)
						l1 ^= 1;

					c.min_y = c.max_y = y;

					hscr = hscrtb[y];

					UINT16 h = hscr & 0x1ff;
					tilemap_draw(bitmap, c, l1, -h, scrolly, tpri | flags);
				}
				break;
			}
			case 2: case 3: {
				for(INT32 y=cliprect.top(); y<=cliprect.bottom(); y++) {
					m2_rectangle c1 = cliprect;
					m2_rectangle c2 = cliprect;
					INT32 l1 = layer;

					hscr = hscrtb[y];

					UINT16 h = hscr & 0x1ff;

					if(c1.right() >= h)
						c1.max_x = h-1;
					if(c2.left() < h)
						c2.min_x = h;
					if(!(hscr & 0x200))
						l1 ^= 1;

					c1.min_y = c1.max_y = y;
					c2.min_y = c2.max_y = y;

					tilemap_draw(bitmap, c1, l1, -h, scrolly, tpri | flags);
					tilemap_draw(bitmap, c2, l1^1, -h, scrolly, tpri | flags);
				}
				break;
			}
			}

		} else {
			const INT32 scrollx = -(hscr & 0x1ff);

			switch((ctrl & 0x6000) >> 13) {
			case 1: {
				m2_rectangle c1 = cliprect;
				m2_rectangle c2 = cliprect;
				UINT16 v;
				v = (-vscr) & 0x1ff;
				if(c1.bottom() >= v)
					c1.max_y = v-1;
				if(c2.top() < v)
					c2.min_y = v;
				if(!((-vscr) & 0x200))
					layer ^= 1;

				tilemap_draw(bitmap, c1, layer, scrollx, scrolly, tpri | flags);
				tilemap_draw(bitmap, c2, layer^1, scrollx, scrolly, tpri | flags);
				break;
			}
			case 2: case 3: {
				m2_rectangle c1 = cliprect;
				m2_rectangle c2 = cliprect;
				UINT16 h;
				h = (+hscr) & 0x1ff;
				if(c1.right() >= h)
					c1.max_x = h-1;
				if(c2.left() < h)
					c2.min_x = h;
				if(!((+hscr) & 0x200))
					layer ^= 1;

				tilemap_draw(bitmap, c1, layer, scrollx, scrolly, tpri | flags);
				tilemap_draw(bitmap, c2, layer^1, scrollx, scrolly, tpri | flags);
				break;
			}
			}
		}

	} else {
		INT32 win = layer & 1;

		if(hscr & 0x8000) {
			const UINT16 *hscrtb = tile_ram + 0x4000 + 0x200*layer;
			vscr &= 0x1ff;

			for(INT32 y=0; y<384; y++) {
				hscr = (-hscrtb[y]) & 0x1ff;
				if(hscr + 496 <= 512) {
					// Horizontal split unnecessary
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr, vscr,        0,        y,      496,      y+1);
				} else {
					// Horizontal split necessary
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr, vscr,        0,        y, 512-hscr,      y+1);
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win,    0, vscr, 512-hscr,        y,      496,      y+1);
				}
				vscr = (vscr + 1) & 0x1ff;
			}
		} else {
			hscr = (-hscr) & 0x1ff;
			vscr = (+vscr) & 0x1ff;

			if(hscr + 496 <= 512) {
				// Horizontal split unnecessary
				if(vscr + 384 <= 512) {
					// Vertical split unnecessary
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr, vscr,        0,        0,      496,      384);
				} else {
					// Vertical split necessary
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr, vscr,        0,        0,      496, 512-vscr);
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr,    0,        0, 512-vscr,      496,      384);

				}
			} else {
				// Horizontal split necessary
				if(vscr + 384 <= 512) {
					// Vertical split unnecessary
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr, vscr,        0,        0, 512-hscr,      384);
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win,    0, vscr, 512-hscr,        0,      496,      384);
				} else {
					// Vertical split necessary
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr, vscr,        0,        0, 512-hscr, 512-vscr);
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win,    0, vscr, 512-hscr,        0,      496, 512-vscr);
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win, hscr,    0,        0, 512-vscr, 512-hscr,      384);
					s24_draw_rect(layer, bitmap, mask, tpri, flags, win,    0,    0, 512-hscr, 512-vscr,      496,      384);
				}
			}
		}
	}
}

// ---- the screen (MAME's model2_state::screen_update) ----------------------------------------------------------------
static void screen_update()
{
	const m2_rectangle cliprect(0, M2_WIDTH - 1, 0, M2_HEIGHT - 1);

	UINT64 t0 = m2_ticks();

	// if the scroll color table was written to, we need to refresh the palette (MAME keeps refreshing once it was:
	// the same pens, made again only when the table changes here)
	if (palette_dirty) {
		const UINT16 *pal = (UINT16*)DrvPalRAM;
		for (INT32 i = 0; i < 0x1000; i++)
			DrvPalette[i] = pen_from(pal[i]);
		palette_dirty = false;
	}

	tiles_update();

	screen_bitmap->fill(DrvPalette[0], cliprect);
	sys24_bitmap->fill(0, cliprect);

	// draw tilemap B as opaque
	for (INT32 layer = 3; layer >= 2; layer--)
		s24_draw(*sys24_bitmap, cliprect, layer << 1, S24_DRAW_OPAQUE);

	for (INT32 layer = 1; layer >= 0; layer--)
		s24_draw(*sys24_bitmap, cliprect, layer << 1, 0);

	m2_copybitmap_trans(*screen_bitmap, *sys24_bitmap, cliprect, 0);

	UINT64 t1 = m2_ticks();

	/* tell the rasterizer we're starting a frame */
	perf_polys += m2->m_raster->poly_list_index;
	if (m2->m_render_test_mode == true)
		m2->draw_framebuffer(*screen_bitmap, cliprect);
	else
		m2->render_polygons(*screen_bitmap, cliprect);

	UINT64 t2 = m2_ticks();

	sys24_bitmap->fill(0, cliprect);

	for (INT32 layer = 3; layer >= 0; layer--)
		s24_draw(*sys24_bitmap, cliprect, (layer<<1) | 1, 0);

	m2_copybitmap_trans(*screen_bitmap, *sys24_bitmap, cliprect, 0);

	UINT64 t3 = m2_ticks();
	part_ticks[PART_2D] += (t1 - t0) + (t3 - t2);
	part_ticks[PART_3D] += t2 - t1;
}

// ---- interrupts (MAME's irq_update, irq_*_w) -------------------------------------------------------------------------
static void irq_update()
{
	I960SetIRQLine(I960_IRQ0, (intreq & 0x001) ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
	I960SetIRQLine(I960_IRQ1, (intreq & 0x002) ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
	I960SetIRQLine(I960_IRQ2, (intreq & 0x3fc) ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
	I960SetIRQLine(I960_IRQ3, (intreq & 0xc00) ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
}

static INT32 uart_txrdy(INT32 side);
static INT32 uart_rxrdy(INT32 side);

// MAME's sound_ready_w: the main board's uart raises interrupt 10 while it has a byte or room for one
static void sound_ready()
{
	const UINT32 line = 1 << 10;
	if ((uart_txrdy(0) || uart_rxrdy(0)) && (intena & line)) {
		intreq |= line;
		irq_update();
	}
}

// irq_mask_delayed_update (MAME delays the mask write by 80 ns, two cycles: vcop2 needs it)
static void irq_mask_delayed_update()
{
	intena_pending = 0;
	intena = intena_pending_value;
	sound_ready();
}

// ---- timers (MAME's model2_timers: they count down at 25 MHz, the i960's clock, and raise IRQ2) -------------------
static UINT32 timers_r(INT32 offset)
{
	if (timerrun[offset]) {
		const INT64 cur = I960TotalCycles() - timer_start[offset];
		timervals[offset] = timerorig[offset] - (UINT32)cur;
	}
	return timervals[offset];
}

static void timers_w(INT32 offset, UINT32 data, UINT32 mem_mask)
{
	timervals[offset] = (timervals[offset] & ~mem_mask) | (data & mem_mask);
	timerorig[offset] = timervals[offset];
	timer_start[offset] = I960TotalCycles();
	timer_fire[offset] = timer_start[offset] + timerorig[offset];
	timerrun[offset] = 1;
	I960RunEnd(); // the run loop looks at the timers again
}

static void timer_cb(INT32 n)
{
	if (timerrun[n] == 0)
		return;
	const UINT32 line = 1 << (n + 2);
	if (intena & line) {
		intreq |= line;
		irq_update();
	}
	timervals[n] = 0xfffff;
	timerrun[n] = 0;
}

// ---- the TGP (MB86234) and its FIFOs ----------------------------------------------------------------------------------
// The TGP runs behind the i960 and is brought up to the i960's time whenever the i960 looks at it (a FIFO access, its
// control register) and at the end of every slice of the frame. When the i960 reads the empty output FIFO, the TGP runs
// on until it answers (MAME halts the i960 until it does) and the i960 is charged the time it waited. When the TGP
// waits on an empty input FIFO or a full output FIFO it is halted, as MAME's FIFO callbacks halt it.
#define TGP_AHEAD_LIMIT 2000000   // TGP cycles the i960 waits for an answer at most (then MAME's stall: it waits halted)

static INT64 tgp_now_i960()  // the TGP's time, in i960 cycles
{
	return (Mb86233TotalCycles() * 3) / 2;
}

static void tgp_run_to(INT64 i960_time)
{
	const INT64 target = (i960_time * 2) / 3;
	INT64 now = Mb86233TotalCycles();
	if (now >= target) return;
	UINT64 t0 = m2_ticks();
	while (now < target) {
		INT32 todo = (INT32)((target - now) > 100000 ? 100000 : (target - now));
		if (Mb86233GetHalt())
			Mb86233Idle(todo);
		else
			Mb86233Run(todo);
		now = Mb86233TotalCycles();
	}
	part_ticks[PART_TGP] += m2_ticks() - t0;
}

static void tgp_sync()
{
	tgp_run_to(I960TotalCycles());
}

static void tgp_set_halt(INT32 state)
{
	tgp_halt_line = state;
	Mb86233SetHalt(state);
}

static void i960_set_halt(INT32 state)
{
	i960_halt_line = state;
	I960SetHalt(state);
}

// the TGP's side (register file: rf1 reads the input FIFO, rf2 writes the output FIFO)
static UINT32 tgp_fifo_in_pop()
{
	if (fifo_in.count == 0) {
		// on_fifo_empty_pre_sync: the read is made again; post_sync: the TGP halts until the i960 pushes
		Mb86233Stall();
		fifo_in.empty_triggered = 1;
		tgp_set_halt(1);
		return 0;
	}
	UINT32 d = fifo_pop_raw(&fifo_in);
	if (fifo_in.count <= M2_FIFO_CAP && fifo_in.full_triggered) {
		// the overflow went in: the i960 goes on
		fifo_in.full_triggered = 0;
		i960_set_halt(0);
	}
	return d;
}

static void tgp_fifo_out_push(UINT32 d)
{
	fifo_push_raw(&fifo_out, d);
	if (fifo_out.count > M2_FIFO_CAP) {
		// overflow: the TGP halts until the i960 makes room
		fifo_out.full_triggered = 1;
		tgp_set_halt(1);
	} else if (fifo_out.empty_triggered) {
		// on_fifo_unempty: the i960 waited for this
		fifo_out.empty_triggered = 0;
		i960_set_halt(0);
	}
}

// the i960's side
static void i960_fifo_in_push(UINT32 d)
{
	tgp_sync();
	fifo_push_raw(&fifo_in, d);
	if (fifo_in.count <= M2_FIFO_CAP) {
		if (fifo_in.empty_triggered) {
			fifo_in.empty_triggered = 0;
			tgp_set_halt(0);
		}
		return;
	}
	// overflow: MAME halts the i960 until the TGP has taken one. Run the TGP now, the i960 waiting for it.
	if (fifo_in.empty_triggered) {
		fifo_in.empty_triggered = 0;
		tgp_set_halt(0);
	}
	const INT64 start = Mb86233TotalCycles();
	UINT64 t0 = m2_ticks();
	while (fifo_in.count > M2_FIFO_CAP && !Mb86233GetHalt() && Mb86233TotalCycles() - start < TGP_AHEAD_LIMIT)
		Mb86233Run(64);
	part_ticks[PART_TGP] += m2_ticks() - t0;
	perf_tgp_ahead += Mb86233TotalCycles() - start;
	const INT64 wait = tgp_now_i960() - I960TotalCycles();
	if (wait > 0) I960BurnCycles((INT32)wait);
	if (fifo_in.count > M2_FIFO_CAP) {
		fifo_in.full_triggered = 1;
		i960_set_halt(1);
	}
}

static UINT32 i960_fifo_out_pop()
{
	tgp_sync();
	if (fifo_out.count == 0) {
		// MAME stalls and halts the i960 until the TGP pushes: run the TGP on now, the i960 waiting for it
		const INT64 start = Mb86233TotalCycles();
		UINT64 t0 = m2_ticks();
		while (fifo_out.count == 0 && !Mb86233GetHalt() && Mb86233TotalCycles() - start < TGP_AHEAD_LIMIT)
			Mb86233Run(32);
		part_ticks[PART_TGP] += m2_ticks() - t0;
		perf_tgp_ahead += Mb86233TotalCycles() - start;
		const INT64 wait = tgp_now_i960() - I960TotalCycles();
		if (wait > 0) I960BurnCycles((INT32)wait);
		if (fifo_out.count == 0) {
			// the TGP waits on the i960 (or has no program): the read is made again when it answers
			I960Stall();
			fifo_out.empty_triggered = 1;
			i960_set_halt(1);
			return 0;
		}
	}
	UINT32 d = fifo_pop_raw(&fifo_out);
	if (fifo_out.count <= M2_FIFO_CAP && fifo_out.full_triggered) {
		fifo_out.full_triggered = 0;
		tgp_set_halt(0);
	}
	return d;
}

// MAME's copro_* (model2_tgp_state)
static void copro_halt()
{
	tgp_set_halt(1);
}

static void copro_boot()
{
	tgp_set_halt(0);
	Mb86233Reset(); // pulse_input_line(INPUT_LINE_RESET)
}

static void copro_ctl1_w(UINT32 data, UINT32 mem_mask)
{
	tgp_sync();
	// did hi bit change?
	if ((data ^ coproctl) == 0x80000000) {
		if (data & 0x80000000) {
			coprocnt = 0;
			copro_halt();
		} else {
			copro_boot();
		}
	}
	coproctl = (coproctl & ~mem_mask) | (data & mem_mask);
}

static void copro_function_port_w(UINT32 offset, UINT32 data)
{
	UINT32 d = data & 0x800fffff;
	UINT32 a = (offset >> 2) & 0xff;
	d |= a << 23;
	i960_fifo_in_push(d);
}

static void copro_fifo_w(UINT32 data)
{
	if (coproctl & 0x80000000) {
		DrvTGPProgram[coprocnt & 0xfff] = data;
		coprocnt++;
	} else {
		i960_fifo_in_push(data);
	}
}

static UINT32 fifo_control_r()
{
	tgp_sync();
	return (fifo_out.count == 0) ? 1 : 0;
}

// the TGP's io space: the tables, or the memory window (bank register bits 22-23)
static UINT32 copro_tgp_memory_r(UINT32 offset)
{
	UINT32 adr = (copro_tgp_bank_reg & 0xff0000) | offset;

	if (adr & 0x800000) {
		adr &= (0x800000 >> 2) - 1;
		return (adr < (0x400000 >> 2)) ? ((UINT32*)DrvCoproData)[adr] : 0;
	}

	if (adr & 0x400000) {
		adr &= 0x7fff;
		return ((UINT32*)DrvBufferRAM)[adr];
	}

	return 0;
}

static void copro_tgp_memory_w(UINT32 offset, UINT32 data)
{
	UINT32 adr = (copro_tgp_bank_reg & 0xff0000) | offset;
	if (adr & 0x400000) {
		adr &= 0x7fff;
		((UINT32*)DrvBufferRAM)[adr] = data;
	}
}

static UINT32 copro_sincos_r(UINT32 offset)
{
	const UINT32 *tables = (UINT32*)DrvTGPTables;
	UINT32 ang = copro_sincos_base + offset * 0x4000;
	UINT32 index = ang & 0x3fff;
	if (ang & 0x4000)
		index = std::min(0x4000 - int(index), 0x3fff);
	UINT32 result = tables[index];
	if (ang & 0x8000)
		result ^= 0x80000000;
	return result;
}

static UINT32 copro_inv_r(UINT32 offset)
{
	const UINT32 *tables = (UINT32*)DrvTGPTables;
	UINT32 index = ((copro_inv_base >> 9) & 0x3ffe) | (offset & 1);
	UINT32 result = tables[index | 0x8000];
	UINT8 bexp = (copro_inv_base >> 23) & 0xff;
	UINT8 exp = (result >> 23) + (0x7f - bexp);
	result = (result & 0x007fffff) | (exp << 23);
	if (copro_inv_base & 0x80000000 && offset)
		result |= 0x80000000;
	return result;
}

static UINT32 copro_isqrt_r(UINT32 offset)
{
	const UINT32 *tables = (UINT32*)DrvTGPTables;
	UINT32 index = 0x2000 ^ (((copro_isqrt_base>> 10) & 0x3ffe) | (offset & 1));
	UINT32 result = tables[index | 0xc000];
	UINT8 bexp = (copro_isqrt_base >> 24) & 0x7f;
	UINT8 exp = (result >> 23) + (0x3f - bexp);
	result = (result & 0x807fffff) | (exp << 23);
	if (!(offset & 1))
		result &= 0x7fffffff;
	return result;
}

static UINT32 copro_atan_r()
{
	const UINT32 *tables = (UINT32*)DrvTGPTables;
	UINT8 ie = 0x88 - (copro_atan_base[3] >> 23);

	bool s0 = copro_atan_base[0] & 0x80000000;
	bool s1 = copro_atan_base[1] & 0x80000000;
	bool s2 = (copro_atan_base[0] & 0x7fffffff) <= (copro_atan_base[1] & 0x7fffffff);

	UINT32 im = copro_atan_base[3] & 0x7fffff;
	UINT32 index = ie <= 0x17 ? (im | 0x800000) >> ie : 0;
	// (MAME's 0x4000 -> 0x3fff. An index of 0x10000 or more reads past the 0x10000-word table: the port makes it 0x3fff
	// too. That is only for an exponent byte of 0x81-0x88 (ie <= 7); MAME reads past the table then)
	if (index == 0x4000 || index >= 0x10000)
		index = 0x3fff;

	UINT32 result = tables[index | 0x4000];

	if (s0 ^ s1 ^ s2)
		result >>= 16;
	if (s2)
		result += 0x4000;
	if ((s0 && !s2) || (s1 && s2))
		result += 0x8000;

	return result & 0xffff;
}

static UINT32 tgp_io_read(UINT32 a)
{
	if (copro_tgp_bank_reg & 0xc00000)
		return copro_tgp_memory_r(a);
	switch (a) {
		case 0x20: case 0x21: case 0x22: case 0x23: return copro_sincos_r(a - 0x20);
		case 0x24: case 0x25: case 0x26: case 0x27: return copro_atan_r();
		case 0x28: case 0x29: return copro_inv_r(a - 0x28);
		case 0x2a: case 0x2b: return copro_isqrt_r(a - 0x2a);
	}
	return 0;
}

static void tgp_io_write(UINT32 a, UINT32 d)
{
	if (copro_tgp_bank_reg & 0xc00000) {
		copro_tgp_memory_w(a, d);
		return;
	}
	switch (a) {
		case 0x20: case 0x21: case 0x22: case 0x23: copro_sincos_base = d; break;
		case 0x24: case 0x25: case 0x26: case 0x27:
			copro_atan_base[a - 0x24] = d;
			Mb86233SetGpio(0, (copro_atan_base[0] & 0x7fffffff) <= (copro_atan_base[1] & 0x7fffffff));
			break;
		case 0x28: case 0x29: copro_inv_base = d; break;
		case 0x2a: case 0x2b: copro_isqrt_base = d; break;
	}
}

static UINT32 tgp_rf_read(UINT32 a)
{
	if (a == 1)
		return tgp_fifo_in_pop();
	return 0;
}

static void tgp_rf_write(UINT32 a, UINT32 d)
{
	switch (a) {
		case 0: break; // leds? busy flag?
		case 2: tgp_fifo_out_push(d); break;
		case 3: copro_tgp_bank_reg = d; break; // the view: copro_tgp_bank_reg & 0xc00000 (tgp_io_read)
	}
}

static UINT32 tgp_data_unmapped_r(UINT32) { return 0; }
static void tgp_data_unmapped_w(UINT32, UINT32) { }

// ---- the geometrizer's input (MAME's geo_* in model2.cpp) -----------------------------------------------------------
static void push_geo_data(UINT32 data)
{
	((UINT32*)DrvBufferRAM)[(geo_write_start_address / 4) & 0x7fff] = data;
	geo_write_start_address += 4;
}

static void geo_ctl1_w(UINT32 data)
{
	// did hi bit change?
	if ((data ^ geoctl) == 0x80000000) {
		if (data & 0x80000000)
			geocnt = 0;
	}
	geoctl = data;
}

static void geo_prg_w(UINT32 data)
{
	if (geoctl & 0x80000000)
		geocnt++;
	else
		push_geo_data(data);
}

static UINT32 geo_r(UINT32 offset)
{
	INT32 address = offset * 4;
	if (address == 0x2008)
		return geo_write_start_address;
	else if (address == 0x3008)
		return m2->m_geo_read_start_address;
	return 0;
}

static void geo_w(UINT32 offset, UINT32 data)
{
	INT32 address = offset * 4;

	if (address < 0x1000) {
		if (data & 0x80000000) {
			UINT32 r = 0;
			r |= data & 0x800fffff;
			r |= ((address >> 4) & 0x3f) << 23;
			push_geo_data(r);
		} else {
			if ((address & 0xf) == 0) {
				UINT32 r = 0;
				r |= data & 0x000fffff;
				r |= ((address >> 4) & 0x3f) << 23;
				if ((address >> 4) & 0xc0) {
					UINT8 function = (address >> 4) & 0x3f;
					if (function == 1)
						r |= ((address >> 10) & 3) << 29; // Eye Mode, used by Sega Rally on car select
				}
				push_geo_data(r);
			}
		}
	} else if (address == 0x1008) {
		geo_write_start_address = data & 0xfffff;
	} else if (address == 0x3008) {
		m2->m_geo_read_start_address = data & 0xfffff;
	}
	// (MAME stops with a fatal error on any other address)
}

static UINT32 videoctl_r()
{
	UINT8 fn;
	if (render_mode == false)
		fn = (framenum & 2) << 1;
	else
		fn = (framenum & 1) << 2;
	return fn | (videocontrol & 3);
}

static UINT32 render_mode_r()
{
	return (render_unk << 14) | (render_mode << 2) | (m2->m_render_test_mode << 0);
}

static void render_mode_w(UINT32 data)
{
	m2->m_render_test_mode = (data & 1) != 0;
	render_mode = ((data >> 2) & 1) != 0;
	render_unk = ((data >> 14) & 1) != 0;
}

// ---- the uPD71051 / i8251 link between the main board and the sound board (MAME's i8251, by bytes) -----------------
// MAME runs both chips bit by bit from a 500 kHz clock (16 x 31.25 kHz); here a character takes its frame's time
// (start + data + parity + stop bits, times the mode's clock factor) and reaches the other chip's receiver when the
// sending chip's shift register empties, as it does in MAME.
#define UART_TX_READY 0x01
#define UART_RX_READY 0x02
#define UART_TX_EMPTY 0x04
#define UART_PE       0x08
#define UART_OE       0x10
#define UART_FE       0x20
#define UART_CLOCK_CYCLES 50 // i960 cycles a 500 kHz clock

struct m2_uart {
	UINT8 mode, command, status, tx_data, rx_data, tx_shift;
	UINT8 next_mode;       // the next control write is the mode byte
	UINT8 sync_left;       // synchronous mode: sync characters still to come
	UINT8 tx_busy, delayed_tx_en, cts;
	INT32 frame;           // i960 cycles a character
	INT64 tx_done;         // when the shift register empties
};
static m2_uart uart[2]; // 0: main board (i960), 1: sound board (68000)
static INT32 snd_irq_line; // the 68000's IRQ 2 (the sound board uart's RxRDY)

static INT32 uart_tx_enabled(INT32 side)
{
	return (uart[side].command & 1) && !uart[side].cts;
}

static INT32 uart_txrdy(INT32 side)
{
	return uart_tx_enabled(side) && (uart[side].status & UART_TX_READY) != 0;
}

static INT32 uart_rxrdy(INT32 side)
{
	if (!(uart[side].command & 0x04)) return 0;
	return (uart[side].status & UART_RX_READY) != 0;
}

static void uart_update_lines(INT32 side)
{
	if (side == 0)
		sound_ready();
	else {
		const INT32 line = uart_rxrdy(1);
		// MAME's i8251 rxrdy callback: the 68000's IRQ 2 follows at once (the frame also sets it at each line)
		if (line != snd_irq_line)
			SekSetIRQLine(0, 2, line ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
		snd_irq_line = line;
	}
}

static void uart_reset(INT32 side)
{
	m2_uart &u = uart[side];
	memset(&u, 0, sizeof(u));
	u.next_mode = 1;
	u.status = UART_TX_EMPTY | UART_TX_READY;
	u.cts = 0; // write_cts(0) in the machine's / the sound board's reset
	u.frame = 10 * UART_CLOCK_CYCLES;
	u.tx_done = -1;
}

static void uart_start_tx(INT32 side, INT64 now)
{
	m2_uart &u = uart[side];
	u.tx_shift = u.tx_data;
	u.tx_busy = 1;
	u.tx_done = now + u.frame;
	u.status &= ~UART_TX_EMPTY;
	u.status |= UART_TX_READY;
}

static void uart_check_for_tx_start(INT32 side, INT64 now)
{
	m2_uart &u = uart[side];
	if (uart_tx_enabled(side) && !u.tx_busy && (u.status & UART_TX_READY) == 0)
		uart_start_tx(side, now);
}

static void uart_receive(INT32 side, UINT8 ch)
{
	m2_uart &u = uart[side];
	if (!(u.command & 0x04)) return; // receiver disabled: MAME's receive_clock samples nothing
	u.rx_data = ch;
	if (u.status & UART_RX_READY)
		u.status |= UART_OE;
	u.status |= UART_RX_READY;
	uart_update_lines(side);
}

// the characters whose time came (both chips), up to `now` (i960 cycles)
static void uart_run(INT64 now)
{
	for (;;) {
		INT32 side = -1;
		INT64 when = now + 1;
		for (INT32 s = 0; s < 2; s++)
			if (uart[s].tx_busy && uart[s].tx_done <= now && uart[s].tx_done < when) {
				when = uart[s].tx_done;
				side = s;
			}
		if (side < 0) break;
		m2_uart &u = uart[side];
		u.tx_busy = 0;
		uart_receive(side ^ 1, u.tx_shift);
		if ((u.status & UART_TX_READY) == 0 && (uart_tx_enabled(side) || u.delayed_tx_en))
			uart_start_tx(side, when);
		else
			u.status |= UART_TX_EMPTY;
		uart_update_lines(side);
	}
}

static void uart_mode_w(INT32 side, UINT8 data)
{
	m2_uart &u = uart[side];
	u.mode = data;
	const INT32 data_bits = ((data >> 2) & 3) + 5;
	const INT32 parity = (data & 0x10) ? 1 : 0;
	if (data & 0x03) {
		// asynchronous: x1, x16, x64; stop bits 1, 1.5, 2 (counted in half bits)
		static const INT32 factor[4] = { 1, 1, 16, 64 };
		static const INT32 stop_halves[4] = { 2, 2, 3, 4 };
		const INT32 halves = 2 * (1 + data_bits + parity) + stop_halves[(data >> 6) & 3];
		u.frame = (halves * factor[data & 3] * UART_CLOCK_CYCLES) / 2;
		u.sync_left = 0;
	} else {
		// synchronous (not used by Model 2): one or two sync characters follow
		u.frame = (data_bits + parity) * UART_CLOCK_CYCLES;
		u.sync_left = (data & 0x80) ? 1 : 2;
	}
	u.next_mode = 0;
}

static void uart_command_w(INT32 side, UINT8 data, INT64 now)
{
	m2_uart &u = uart[side];
	u.command = data;
	if (data & 0x10)
		u.status &= ~(UART_PE | UART_OE | UART_FE);
	if (data & 0x40)
		u.next_mode = 1;
	uart_check_for_tx_start(side, now);
	uart_update_lines(side);
}

static UINT8 uart_read(INT32 side, INT32 offset, INT64 now)
{
	uart_run(now);
	m2_uart &u = uart[side];
	if (offset & 1)
		return 0x80 | u.status; // DSR (not driven: 1)
	u.status &= ~UART_RX_READY;
	uart_update_lines(side);
	return u.rx_data;
}

static void uart_write(INT32 side, INT32 offset, UINT8 data, INT64 now)
{
	uart_run(now);
	m2_uart &u = uart[side];
	if (offset & 1) {
		if (u.sync_left) { u.sync_left--; return; }
		if (u.next_mode)
			uart_mode_w(side, data);
		else
			uart_command_w(side, data, now);
		return;
	}
	u.tx_data = data;
	u.status &= ~UART_TX_READY;
	uart_update_lines(side);
	u.delayed_tx_en = uart_tx_enabled(side);
	uart_check_for_tx_start(side, now);
	uart_update_lines(side);
}

// the sound board's time, in i960 cycles (it runs behind the main board in each slice)
static INT64 frame_base;  // the i960's time at the start of this frame
static INT64 snd_time_now()
{
	return frame_base + ((INT64)SekTotalCycles() * 5) / 2;
}

// ---- the i960's memory map (MAME's model2_base_mem + model2_tgp_mem + model2o_mem) -----------------------------------
// handler 1: the registers; 2: palette / colour table writes; 3: texture RAM writes; 4: luma RAM
static const UINT8 tgp_id[16] = { 0,'T','A','H',0,'A','K','O',0,'Z','A','K',0,'M','T','K' };

// the 8-bit and 16-bit devices on the 32-bit bus, by byte
static INT32 m2_byte_device(UINT32 a)
{
	if ((a & 0xfffffff0) == 0x00980030) return 1;            // tgpid
	if ((a & 0xfffefffc) == 0x01a04000) return 2;            // m2comm cn / fg (and the mirror)
	if ((a & 0xfffff000) == 0x01c00000) return 3;            // MB8421 dual port RAM, umask 0x00ff00ff
	if ((a & 0xfffffffc) == 0x01c80000) return 4;            // uart, umask16 0x00ff
	return 0;
}

static UINT8 m2_read8_device(INT32 dev, UINT32 a)
{
	switch (dev) {
		case 1: return tgp_id[a & 0xf];
		case 2:
			if ((a & 3) == 0) return comm_cn | 0xfe;
			if ((a & 3) == 2) return comm_fg | ((~comm_zfg << 7) & 0x80) | 0x7e;
			return 0;
		case 3:
			if (a & 1) return 0;
			return DrvDPRAM[((((a - 0x01c00000) >> 2) << 1) | ((a >> 1) & 1)) & 0x7ff];
		case 4:
			if (a & 1) return 0;
			return uart_read(0, (a >> 1) & 1, I960TotalCycles());
	}
	return 0;
}

static void m2_write8_device(INT32 dev, UINT32 a, UINT8 d)
{
	switch (dev) {
		case 2:
			if ((a & 3) == 0) {
				comm_cn = d & 1;
				if (!comm_cn) { comm_zfg = 0; comm_cn = 0; comm_fg = 0; } // device_reset
			} else if ((a & 3) == 2) {
				comm_fg = d & 1;
			}
			break;
		case 3:
			if (a & 1) return;
			DrvDPRAM[((((a - 0x01c00000) >> 2) << 1) | ((a >> 1) & 1)) & 0x7ff] = d;
			break;
		case 4:
			if (a & 1) return;
			uart_write(0, (a >> 1) & 1, d, I960TotalCycles());
			break;
	}
}

static UINT32 m2_read32_regs(UINT32 a)
{
	if (a >= 0x00800000 && a <= 0x00803fff) return geo_r((a - 0x00800000) >> 2);
	if (a >= 0x00804000 && a <= 0x00807fff) return 0xffffffff; // geo_prg_r
	if (a >= 0x00880000 && a <= 0x00883fff) return 0;          // copro function port: write only
	if (a >= 0x00884000 && a <= 0x00887fff) return i960_fifo_out_pop();
	switch (a) {
		case 0x00980000: return coproctl;
		case 0x00980004: return fifo_control_r();
		case 0x0098000c: return videoctl_r();
		case 0x00e80000: return intreq;
		case 0x00e80004: return intena;
		case 0x00f00000: case 0x00f00004: case 0x00f00008: case 0x00f0000c: return timers_r((a >> 2) & 3);
	}
	if (a >= 0x10000000 && a <= 0x101fffff) return render_mode_r();
	if (a >= 0x10400000 && a <= 0x105fffff) return m2->polygon_count_r();
	return 0;
}

static void m2_write32_regs(UINT32 a, UINT32 d, UINT32 mem_mask)
{
	if (a >= 0x00800000 && a <= 0x00803fff) { geo_w((a - 0x00800000) >> 2, d); return; }
	if (a >= 0x00804000 && a <= 0x00807fff) { geo_prg_w(d); return; }
	if (a >= 0x00880000 && a <= 0x00883fff) { copro_function_port_w((a - 0x00880000) >> 2, d); return; }
	if (a >= 0x00884000 && a <= 0x00887fff) { copro_fifo_w(d); return; }
	switch (a) {
		case 0x00980000: copro_ctl1_w(d, mem_mask); return;
		case 0x00980008: geo_ctl1_w(d); return;
		case 0x0098000c: videocontrol = (videocontrol & ~mem_mask) | (d & mem_mask); return;
		case 0x00e80000: intreq &= d; irq_update(); return;
		case 0x00e80004: {
			UINT32 temp = intena;
			temp = (temp & ~mem_mask) | (d & mem_mask);
			// delay IRQ mask update by 2 cycles (80 ns); vcop2 needs this
			intena_pending = 1;
			intena_pending_value = temp;
			intena_pending_at = I960TotalCycles() + 2;
			I960RunEnd();
			return;
		}
		case 0x00f00000: case 0x00f00004: case 0x00f00008: case 0x00f0000c: timers_w((a >> 2) & 3, d, mem_mask); return;
		case 0x0181c000: m2->model2_3d_zclip_w(d); return;
	}
	if ((a & 0xffeffffc) == 0x01040000) { if (mem_mask & 0xffff) m2->horizontal_sync_w(d & 0xffff); return; }
	if ((a & 0xffeffffc) == 0x01060000) { if (mem_mask & 0xffff) m2->vertical_sync_w(d & 0xffff); return; }
	if (a >= 0x10000000 && a <= 0x101fffff) { render_mode_w(d); return; }
	// 0x01020000 (ABSEL), 0x01070000 (video sync switch), 0x10800000: nothing
}

static UINT8 m2_read_byte(UINT32 a)
{
	const INT32 dev = m2_byte_device(a);
	UINT8 v = dev ? m2_read8_device(dev, a) : (m2_read32_regs(a & ~3) >> ((a & 3) * 8));
	return v;
}

static UINT16 m2_read_word(UINT32 a)
{
	const INT32 dev = m2_byte_device(a);
	if (dev) return m2_read8_device(dev, a) | (m2_read8_device(dev, a + 1) << 8);
	return m2_read32_regs(a & ~3) >> ((a & 2) * 8);
}

static UINT32 m2_read_long(UINT32 a)
{
	const INT32 dev = m2_byte_device(a);
	UINT32 v;
	if (dev)
		v = m2_read8_device(dev, a) | (m2_read8_device(dev, a + 1) << 8) | (m2_read8_device(dev, a + 2) << 16) | (m2_read8_device(dev, a + 3) << 24);
	else
		v = m2_read32_regs(a);
	return v;
}

static void m2_write_byte(UINT32 a, UINT8 d)
{
	const INT32 dev = m2_byte_device(a);
	if (dev) { m2_write8_device(dev, a, d); return; }
	const INT32 sh = (a & 3) * 8;
	m2_write32_regs(a & ~3, (UINT32)d << sh, 0xffu << sh);
}

static void m2_write_word(UINT32 a, UINT16 d)
{
	const INT32 dev = m2_byte_device(a);
	if (dev) { m2_write8_device(dev, a, d & 0xff); m2_write8_device(dev, a + 1, d >> 8); return; }
	const INT32 sh = (a & 2) * 8;
	m2_write32_regs(a & ~3, (UINT32)d << sh, 0xffffu << sh);
}

static void m2_write_long(UINT32 a, UINT32 d)
{
	const INT32 dev = m2_byte_device(a);
	if (dev) {
		for (INT32 i = 0; i < 4; i++)
			m2_write8_device(dev, a + i, d >> (i * 8));
		return;
	}
	m2_write32_regs(a, d, 0xffffffff);
}

// handler 2: palette (0x01800000) and colour table (0x01810000) writes, 16-bit entries
static void pal_write16(UINT32 a, UINT16 d, UINT16 mask)
{
	if (a >= 0x01810000) colorxlat_write16((a - 0x01810000) >> 1, d, mask);
	else palette_write16((a - 0x01800000) >> 1, d, mask);
}
static void pal_write_byte(UINT32 a, UINT8 d) { pal_write16(a & ~1, (a & 1) ? (d << 8) : d, (a & 1) ? 0xff00 : 0x00ff); }
static void pal_write_word(UINT32 a, UINT16 d) { pal_write16(a, d, 0xffff); }
static void pal_write_long(UINT32 a, UINT32 d) { pal_write16(a, d & 0xffff, 0xffff); pal_write16(a + 2, d >> 16, 0xffff); }

// handler 3: texture RAM writes (MAME's tex0_w / tex1_w: a dword write puts its low 16 bits in half a word of the RAM)
static void tex_write32(UINT32 a, UINT32 data)
{
	UINT32 *ram = (UINT32*)((a & 0x00400000) ? DrvTexRAM1 : DrvTexRAM0);
	UINT32 offset = (a & 0x1fffff) >> 2;
	if ((offset & 1) == 0) {
		ram[offset >> 1] &= 0xffff0000;
		ram[offset >> 1] |= data & 0xffff;
	} else {
		ram[offset >> 1] &= 0x0000ffff;
		ram[offset >> 1] |= (data & 0xffff) << 16;
	}
}
static void tex_write_byte(UINT32 a, UINT8 d) { tex_write32(a & ~3, (UINT32)d << ((a & 3) * 8)); }
static void tex_write_word(UINT32 a, UINT16 d) { tex_write32(a & ~3, (UINT32)d << ((a & 2) * 8)); }
static void tex_write_long(UINT32 a, UINT32 d) { tex_write32(a, d); }

// handler 4: luma RAM (umask32 0x000000ff: one byte a dword)
static UINT8 luma_read_byte(UINT32 a) { return (a & 3) ? 0 : DrvLumaRAM[((a - 0x12800000) >> 2) & 0x7fff]; }
static UINT16 luma_read_word(UINT32 a) { return luma_read_byte(a); }
static UINT32 luma_read_long(UINT32 a) { return luma_read_byte(a); }
static void luma_write_byte(UINT32 a, UINT8 d) { if (!(a & 3)) DrvLumaRAM[((a - 0x12800000) >> 2) & 0x7fff] = d; }
static void luma_write_word(UINT32 a, UINT16 d) { luma_write_byte(a, d & 0xff); }
static void luma_write_long(UINT32 a, UINT32 d) { luma_write_byte(a, d & 0xff); }

static void m2_map_rom_alias(UINT8 *mem, UINT32 start, UINT32 len)
{
	I960MapMemory(mem, start, start + len - 1, MAP_READ | MAP_FETCH, I960_BURST);
}

static INT32 main_data_used = 0; // bytes of main_data loaded (0x900000 or 0xa00000)

static void i960_map()
{
	I960SetReadByteHandler(1, m2_read_byte);
	I960SetReadWordHandler(1, m2_read_word);
	I960SetReadLongHandler(1, m2_read_long);
	I960SetWriteByteHandler(1, m2_write_byte);
	I960SetWriteWordHandler(1, m2_write_word);
	I960SetWriteLongHandler(1, m2_write_long);
	I960SetWriteByteHandler(2, pal_write_byte);
	I960SetWriteWordHandler(2, pal_write_word);
	I960SetWriteLongHandler(2, pal_write_long);
	I960SetWriteByteHandler(3, tex_write_byte);
	I960SetWriteWordHandler(3, tex_write_word);
	I960SetWriteLongHandler(3, tex_write_long);
	I960SetReadByteHandler(4, luma_read_byte);
	I960SetReadWordHandler(4, luma_read_word);
	I960SetReadLongHandler(4, luma_read_long);
	I960SetWriteByteHandler(4, luma_write_byte);
	I960SetWriteWordHandler(4, luma_write_word);
	I960SetWriteLongHandler(4, luma_write_long);

	// 0x00000000 program ROM (writes ignored), 0x00200000 RAM, 0x00220000 the ROM's second half again
	I960MapMemory(DrvI960ROM, 0x00000000, 0x001fffff, MAP_READ | MAP_FETCH, I960_BURST);
	I960MapMemory(DrvRAM200, 0x00200000, 0x0021ffff, MAP_RAM | MAP_FETCH, I960_BURST);
	I960MapMemory(DrvI960ROM + 0x20000, 0x00220000, 0x0023ffff, MAP_READ | MAP_FETCH, I960_BURST);
	I960MapMemory(DrvWorkRAM, 0x00500000, 0x005fffff, MAP_RAM | MAP_FETCH, I960_BURST);

	I960MapHandler(1, 0x00800000, 0x00803fff, MAP_RAM, 0);           // geo_r / geo_w
	I960MapHandler(1, 0x00804000, 0x00807fff, MAP_RAM, I960_BURST);  // geo_prg
	I960MapHandler(1, 0x00880000, 0x00883fff, MAP_RAM, I960_BURST);  // copro function port
	I960MapHandler(1, 0x00884000, 0x00887fff, MAP_RAM, 0);           // copro FIFO
	for (UINT32 m = 0; m < 4; m++)                                   // buffer RAM, mirror 0x60000
		I960MapMemory(DrvBufferRAM, 0x00900000 + m * 0x20000, 0x0091ffff + m * 0x20000, MAP_RAM | MAP_FETCH, I960_BURST);
	I960MapHandler(1, 0x00980000, 0x00980fff, MAP_RAM, 0);
	I960MapMemory(DrvCtrlRAM, 0x00e00000, 0x00e00fff, MAP_RAM, 0);  // CPU control (wait states)
	I960MapHandler(1, 0x00e80000, 0x00e80fff, MAP_RAM, 0);
	I960MapHandler(1, 0x00f00000, 0x00f00fff, MAP_RAM, 0);

	for (UINT32 m = 0; m < 2; m++) {
		const UINT32 b = m * 0x100000;
		I960MapMemory(DrvTileRAM, 0x01000000 + b, 0x0100ffff + b, MAP_RAM, I960_BURST);
		I960MapMemory(DrvTileRAM, 0x01010000 + b, 0x0101ffff + b, MAP_RAM, I960_BURST);
		I960MapHandler(1, 0x01020000 + b, 0x01020fff + b, MAP_RAM, I960_BURST); // ABSEL
		I960MapHandler(1, 0x01040000 + b, 0x01040fff + b, MAP_RAM, 0);          // xhout
		I960MapHandler(1, 0x01060000 + b, 0x01060fff + b, MAP_RAM, 0);          // xvout
		I960MapHandler(1, 0x01070000 + b, 0x01070fff + b, MAP_RAM, 0);
		I960MapMemory(DrvCharRAM, 0x01080000 + b, 0x010fffff + b, MAP_RAM, I960_BURST);
	}

	I960MapMemory(DrvPalRAM, 0x01800000, 0x01803fff, MAP_READ, I960_BURST);
	I960MapHandler(2, 0x01800000, 0x01803fff, MAP_WRITE, I960_BURST);
	I960MapMemory(DrvColorXlat, 0x01810000, 0x0181bfff, MAP_READ, I960_BURST);
	I960MapHandler(2, 0x01810000, 0x0181bfff, MAP_WRITE, I960_BURST);
	I960MapHandler(1, 0x0181c000, 0x0181cfff, MAP_RAM, 0);           // z clip
	for (UINT32 m = 0; m < 2; m++) {
		I960MapMemory(DrvCommRAM, 0x01a00000 + m * 0x10000, 0x01a03fff + m * 0x10000, MAP_RAM, I960_BURST);
		I960MapHandler(1, 0x01a04000 + m * 0x10000, 0x01a04fff + m * 0x10000, MAP_RAM, 0);
	}
	I960MapHandler(1, 0x01c00000, 0x01c00fff, MAP_RAM, 0);           // dual port RAM
	I960MapHandler(1, 0x01c80000, 0x01c80fff, MAP_RAM, 0);           // uart
	I960MapMemory(DrvBackupRAM, 0x01d00000, 0x01d03fff, MAP_RAM, I960_BURST);

	// 0x02000000 data ROMs: what the set has, its last MB again up to 0x02ffffff (MAME's ROM_COPYs), then zeros
	m2_map_rom_alias(DrvMainData, 0x02000000, main_data_used);
	for (UINT32 a = 0x02000000 + main_data_used; a < 0x03000000; a += 0x100000)
		m2_map_rom_alias(DrvMainData + main_data_used - 0x100000, a, 0x100000);
	for (UINT32 a = 0x03000000; a < 0x04000000; a += 0x1000)
		m2_map_rom_alias(DrvZeroPage, a, 0x1000);
	for (UINT32 a = 0x06000000; a < 0x07000000; a += 0x1000)       // "extra" data: main_data + 0x1000000 (none)
		m2_map_rom_alias(DrvZeroPage, a, 0x1000);

	I960MapHandler(1, 0x10000000, 0x101fffff, MAP_RAM, 0);           // render mode
	I960MapHandler(1, 0x10400000, 0x105fffff, MAP_RAM, 0);           // polygon count
	I960MapHandler(1, 0x10800000, 0x10800fff, MAP_RAM, 0);
	I960MapMemory(DrvFbVRAMA, 0x11600000, 0x1167ffff, MAP_RAM, I960_BURST);
	I960MapMemory(DrvFbVRAMB, 0x11680000, 0x116fffff, MAP_RAM, I960_BURST);
	for (UINT32 m = 0; m < 2; m++) {
		I960MapMemory(DrvTexRAM0, 0x12000000 + m * 0x200000, 0x121fffff + m * 0x200000, MAP_READ, I960_BURST);
		I960MapHandler(3, 0x12000000 + m * 0x200000, 0x121fffff + m * 0x200000, MAP_WRITE, I960_BURST);
		I960MapMemory(DrvTexRAM1, 0x12400000 + m * 0x200000, 0x125fffff + m * 0x200000, MAP_READ, I960_BURST);
		I960MapHandler(3, 0x12400000 + m * 0x200000, 0x125fffff + m * 0x200000, MAP_WRITE, I960_BURST);
	}
	I960MapHandler(4, 0x12800000, 0x1281ffff, MAP_RAM, I960_BURST);
}

// ---- the Model 1 I/O board (MAME's model1io: Z80 + 315-5338A + MSM6253 + 93C45, the MB8421's left side) -----------
static const UINT8 gearvalue[5] = { 0, 2, 1, 6, 5 }; // MAME's daytona_gearbox_r: N, 1st, 2nd, 3rd, 4th

static UINT8 io_in_port(INT32 port)
{
	switch (port) {
		case 1: return io_secondary ? 0xff : DrvInputs[0];        // IN0 (or DSW1, all off)
		case 2: return io_secondary ? 0xff : DrvInputs[1];        // IN1 (or DSW2)
		case 3: return 0xff;                                      // (nothing, or DSW3)
		case 4: return 0xff;                                      // drive board: nothing to read back
		case 6: return (EEPROMRead() ? 0x80 : 0) | 0x70 | 0x0f;   // eeprom do, the board's buttons (none pressed)
	}
	return 0xff;
}

static void io_out_port(INT32 port, UINT8 data)
{
	switch (port) {
		case 0:
			// 7 eeprom clk, 6 cs, 5 di, 4 pe, 1 led, 0 control switch
			EEPROMWriteBit((data >> 5) & 1);
			EEPROMSetCSLine((data & 0x40) ? EEPROM_CLEAR_LINE : EEPROM_ASSERT_LINE);
			EEPROMSetClockLine((data & 0x80) ? EEPROM_ASSERT_LINE : EEPROM_CLEAR_LINE);
			io_secondary = data & 1;
			break;
		case 4: break; // drive board commands (force feedback, not emulated)
		case 5: break; // lamps and coin counters
	}
}

static UINT8 io_analog(INT32 n)
{
	if (io_secondary) return 0xff;
	return (n < 3) ? AnalogValue[n] : 0xff;
}

static UINT8 __fastcall io_read(UINT16 a)
{
	if (a >= 0x8000 && a <= 0x800f) {
		const INT32 offset = a & 0x0f;
		if (offset <= 6) {
			if ((io_port_config >> offset) & 1)
				return io_in_port(offset);
			return io_port_value[offset];
		}
		switch (offset) {
			case 0x08: return io_port_config;
			case 0x0a: return io_serial_output;
			case 0x0b: return io_cmd;
			case 0x0c: return DrvDPRAM[io_address & 0x7ff];
			case 0x0d: return 0x08; // status: transfer finished, command acknowledged
		}
		return 0xff;
	}
	if (a >= 0xc000 && a <= 0xc003) {
		// MSM6253: the shift register's top bit (the rest of the byte: the unmapped value, 0)
		const UINT8 bit = (adc_shift >> 7) & 1;
		adc_shift <<= 1;
		return bit;
	}
	return 0;
}

static void __fastcall io_write(UINT16 a, UINT8 d)
{
	if (a >= 0x8000 && a <= 0x800f) {
		const INT32 offset = a & 0x0f;
		if (offset <= 6) {
			io_port_value[offset] = d;
			io_out_port(offset, d); // always output, even if set to input (MAME's comment: bingoct needs it)
			return;
		}
		switch (offset) {
			case 0x08: {
				UINT8 changed = d ^ io_port_config;
				for (INT32 i = 0; i < 7; i++)
					if (((changed >> i) & 1) && ((d >> i) & 1) == 0)
						io_out_port(i, io_port_value[i]);
				io_port_config = d;
				break;
			}
			case 0x09:
				io_cmd = d;
				switch (d) {
					case 0x00: io_address = (io_address & 0xff00) | io_serial_output; break;
					case 0x01: io_address = (io_address & 0x00ff) | (io_serial_output << 8); break;
					case 0x07: DrvDPRAM[io_address & 0x7ff] = io_serial_output; break;
					case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
						DrvDPRAM[d & 0x07] = io_serial_output;
						break;
				}
				break;
			case 0x0a: io_serial_output = d; break;
		}
		return;
	}
	if (a >= 0xc000 && a <= 0xc003) {
		adc_shift = io_analog(a & 3); // fill the shift register from the A/D latch
		return;
	}
}

// ---- the Model 1 sound board (MAME's segam1audio: 68000 + YM3438 + 2 MultiPCM + its own i8251) ---------------------
static void mpcm_bank_w(INT32 chip, UINT16 data)
{
	mpcm_bank[chip] = data & 3;
	MultiPCMSetBankChip(chip, mpcm_bank[chip] * 0x100000, mpcm_bank[chip] * 0x100000);
}

static UINT8 __fastcall snd_read_byte(UINT32 a)
{
	if ((a & 0xfffffc) == 0xc20000) return (a & 1) ? uart_read(1, (a >> 1) & 1, snd_time_now()) : 0;
	if ((a & 0xfffff8) == 0xc40000 || (a & 0xfffff8) == 0xc60000) return (a & 1) ? MultiPCMRead() : 0;
	if ((a & 0xfffff8) == 0xd00000) return (a & 1) ? BurnYM2612Read(0, (a >> 1) & 3) : 0;
	return 0;
}

static UINT16 __fastcall snd_read_word(UINT32 a)
{
	return snd_read_byte(a | 1);
}

static void __fastcall snd_write_byte(UINT32 a, UINT8 d)
{
	if ((a & 0xfffffc) == 0xc20000) { if (a & 1) uart_write(1, (a >> 1) & 1, d, snd_time_now()); return; }
	if ((a & 0xfffff8) == 0xc40000) { if (a & 1) MultiPCMWriteChip(0, (a >> 1) & 3, d); return; }
	if ((a & 0xfffff8) == 0xc60000) { if (a & 1) MultiPCMWriteChip(1, (a >> 1) & 3, d); return; }
	if ((a & 0xfffffe) == 0xc50000) { if (a & 1) mpcm_bank_w(0, d); return; }
	if ((a & 0xfffffe) == 0xc70000) { if (a & 1) mpcm_bank_w(1, d); return; }
	if ((a & 0xfffff8) == 0xd00000) { if (a & 1) BurnYM2612Write(0, (a >> 1) & 3, d); return; }
}

static void __fastcall snd_write_word(UINT32 a, UINT16 d)
{
	snd_write_byte(a | 1, d & 0xff);
}

// ---- memory ---------------------------------------------------------------------------------------------------------
static INT32 MemIndex()
{
	UINT8 *Next; Next = AllMem;

	DrvI960ROM      = Next; Next += 0x0200000;
	DrvMainData     = Next; Next += 0x0a00000;
	DrvCoproData    = Next; Next += 0x0400000;
	DrvPolyROM      = Next; Next += 0x1000000 + 0x40; // (+ 16 zeroed words: the geometrizer's last object link)
	DrvTexROM       = Next; Next += 0x1000000;
	DrvTGPTables    = Next; Next += 0x0040000;
	Drv68KROM       = Next; Next += 0x0040000;
	DrvPCM1         = Next; Next += 0x0400000;
	DrvPCM2         = Next; Next += 0x0400000;
	DrvIOROM        = Next; Next += 0x0010000;
	DrvZeroPage     = Next; Next += 0x0001000;

	DrvPalette      = (UINT32*)Next; Next += 0x2000 * sizeof(UINT32);
	DrvBackupRAM    = Next; Next += 0x0004000; // NVRAM: scanned on its own

	char_shadow     = Next; Next += 0x0080000;
	tile_cache      = (UINT16*)Next; Next += 0x4000 * sizeof(UINT16);
	tile_pix        = (UINT16*)Next; Next += 4 * 512 * 512 * sizeof(UINT16);
	tile_flags      = Next; Next += 4 * 512 * 512;
	char_dirty      = Next; Next += 0x4000;

	AllRam          = Next;

	DrvWorkRAM      = Next; Next += 0x0100000;
	DrvRAM200       = Next; Next += 0x0020000;
	DrvBufferRAM    = Next; Next += 0x0020000 + 0x100; // (+ 64 zeroed words: model2.h's geo_words_left)
	DrvTileRAM      = Next; Next += 0x0010000;
	DrvCharRAM      = Next; Next += 0x0080000;
	DrvPalRAM       = Next; Next += 0x0004000;
	DrvColorXlat    = Next; Next += 0x000c000;
	DrvCommRAM      = Next; Next += 0x0004000;
	DrvCtrlRAM      = Next; Next += 0x0001000;
	DrvTexRAM0      = Next; Next += 0x0200000;
	DrvTexRAM1      = Next; Next += 0x0200000;
	DrvLumaRAM      = Next; Next += 0x0008000;
	DrvFbVRAMA      = Next; Next += 0x0080000;
	DrvFbVRAMB      = Next; Next += 0x0080000;
	DrvTGPProgram   = (UINT32*)Next; Next += 0x1000 * sizeof(UINT32);
	DrvTGPData      = (UINT32*)Next; Next += 0x0400 * sizeof(UINT32);
	Drv68KRAM       = Next; Next += 0x0010000;
	DrvIORAM        = Next; Next += 0x0002000;
	DrvDPRAM        = Next; Next += 0x0000800;

	RamEnd          = Next;

	MemEnd          = Next;

	return 0;
}

// ---- ROMs -----------------------------------------------------------------------------------------------------------
// nType's low bits: the region. The 32-bit regions take their ROMs in pairs (MAME's ROM_LOAD32_WORD: the first ROM's
// words at +0, the second's at +2), the k-th pair at k * the region's stride.
enum { R_MAINCPU = 1, R_MAINDATA, R_COPRO, R_POLY, R_TEX, R_COMM, R_SNDCPU, R_PCM1, R_PCM2, R_TGPTAB, R_OTHER, R_DRIVE };

static INT32 DrvLoadRoms()
{
	INT32 pair[16] = { 0 };
	INT32 half[16] = { 0 };
	INT32 snd_ofs = 0, pcm1_ofs = 0, pcm2_ofs = 0;
	INT32 last_data_len = 0;
	char *name;

	for (INT32 i = 0; BurnDrvGetRomName(&name, i, 0) == 0 && i < 0x80; i++) {
		struct BurnRomInfo ri;
		ri.nType = 0;
		ri.nLen = 0;
		BurnDrvGetRomInfo(&ri, i);
		const INT32 region = ri.nType & 0x0f;
		if (region == 0 || (ri.nType & BRF_OPT)) continue;
		UINT8 *dest = NULL;
		INT32 stride = 0;
		switch (region) {
			case R_MAINCPU:  dest = DrvI960ROM;   stride = 0x400000; break;
			case R_MAINDATA: dest = DrvMainData;  stride = 0x400000; last_data_len = ri.nLen; break;
			case R_COPRO:    dest = DrvCoproData; stride = 0x400000; break;
			case R_POLY:     dest = DrvPolyROM;   stride = 0x400000; break;
			case R_TEX:      dest = DrvTexROM;    stride = 0x800000; break;
			case R_TGPTAB:   dest = DrvTGPTables; stride = 0x040000; break;
			case R_SNDCPU:
				if (BurnLoadRom(Drv68KROM + snd_ofs, i, 1)) return 1;
				snd_ofs += ri.nLen;
				continue;
			case R_PCM1:
				if (BurnLoadRom(DrvPCM1 + pcm1_ofs, i, 1)) return 1;
				pcm1_ofs += ri.nLen;
				continue;
			case R_PCM2:
				if (BurnLoadRom(DrvPCM2 + pcm2_ofs, i, 1)) return 1;
				pcm2_ofs += ri.nLen;
				continue;
			default:
				continue;
		}
		if (BurnLoadRomExt(dest + pair[region] * stride + half[region] * 2, i, 4, LD_GROUP(2))) return 1;
		if (half[region]) pair[region]++;
		half[region] ^= 1;
	}

	// the I/O board's ROM (the board set's first)
	if (BurnLoadRom(DrvIOROM, 0x80, 1)) return 1;

	// (the sound ROMs are MAME's ROM_LOAD16_WORD_SWAP: that is the byte order of FBNeo's 68000 memory already)

	main_data_used = (pair[R_MAINDATA] - 1) * 0x400000 + 2 * last_data_len;
	if (main_data_used < 0x900000 || main_data_used > 0xa00000) {
		bprintf(PRINT_ERROR, _T("model2: unexpected data ROM layout (%x)\n"), main_data_used);
		return 1;
	}
	return 0;
}

// ---- the game's settings in the I/O board's EEPROM: Link ID "Single", country USA --------------------------------------
// Daytona USA keeps its settings (Link ID, cabinet, country, difficulty...) in the I/O board's 93C45: "SEGA", a version,
// a CRC-16 (XMODEM: polynomial 0x1021, from 0) of bytes 0x0a-0x7f at 0x08 (little endian), then the settings; byte
// 0x0b is the Link ID (0 Single, 1 Master, 2 Slave). The game's factory setting is Master, and a Master waits for the
// other cabinets at "NETWORK CHECKING" -- the link board is not emulated (as in MAME, which stops there until the test
// menu says Single). So the port keeps the Link ID at Single: in a saved EEPROM when the game starts, and on the very
// first start (a blank EEPROM) the game is reset once, right after it has written its factory settings.
// Byte 0x1b is the country (0 USA, 1 JPN, 2 EXPORT; 0x7c follows it, as the test menu writes them), and the factory
// setting of this ROM (MAME: "Japan, Revision A") is JPN: the game in Japanese. The port makes it USA (English) at
// every start, with the Link ID: a country chosen in the test menu (GAME SYSTEM) lasts until the game is closed.
#define SETTINGS_LINK_ID    0x0b
#define SETTINGS_COUNTRY    0x1b
#define SETTINGS_COUNTRY2   0x7c
static INT32 io_eeprom_was_blank = 0;

static UINT16 settings_crc(const UINT8 *b)
{
	UINT16 c = 0;
	for (INT32 i = 0x0a; i < 0x80; i++) {
		c ^= b[i] << 8;
		for (INT32 k = 0; k < 8; k++)
			c = (c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1);
	}
	return c;
}

static INT32 settings_valid(const UINT8 *b)
{
	return memcmp(b, "SEGA", 4) == 0 && (b[8] | (b[9] << 8)) == settings_crc(b);
}

// 1 when a setting was changed: the Link ID to Single, the country to USA (0x7c with it when it follows the country),
// the CRC made again
static INT32 settings_fix()
{
	UINT8 b[0x80];
	for (INT32 i = 0; i < 0x80; i++) b[i] = EEPROMReadByte(i);
	if (!settings_valid(b))
		return 0;
	const UINT8 link = b[SETTINGS_LINK_ID], country = b[SETTINGS_COUNTRY];
	if (link == 0 && country == 0)
		return 0;
	if (country != 0) {
		if (b[SETTINGS_COUNTRY2] == country)
			b[SETTINGS_COUNTRY2] = 0;
		b[SETTINGS_COUNTRY] = 0;
		bprintf(PRINT_IMPORTANT, _T("model2: the I/O board's settings say country %s: set to USA (English)\n"),
			country == 1 ? _T("JPN") : country == 2 ? _T("EXPORT") : _T("(other)"));
	}
	if (link != 0) {
		b[SETTINGS_LINK_ID] = 0;
		bprintf(PRINT_IMPORTANT, _T("model2: the I/O board's settings say Link ID %s: set to Single (no link board)\n"),
			link == 1 ? _T("Master") : _T("Slave"));
	}
	const UINT16 c = settings_crc(b);
	b[8] = c & 0xff;
	b[9] = c >> 8;
	for (INT32 i = 0; i < 0x80; i++) EEPROMWriteByte(i, b[i]);
	return 1;
}

static INT32 settings_blank_at_start()
{
	UINT8 b[0x80];
	for (INT32 i = 0; i < 0x80; i++) b[i] = EEPROMReadByte(i);
	return !settings_valid(b);
}

// ---- reset ----------------------------------------------------------------------------------------------------------
static void tgp_map();

static INT32 DrvDoReset()
{
	memset(AllRam, 0, RamEnd - AllRam);

	// MAME's machine_reset (model2_state, model2_tgp_state)
	intreq = intena = 0;
	intena_pending = 0;
	coproctl = coprocnt = 0;
	geoctl = geocnt = 0;
	for (INT32 i = 0; i < 4; i++) {
		timervals[i] = 0xfffff;
		timerorig[i] = 0;
		timerrun[i] = 0;
		timer_start[i] = timer_fire[i] = 0;
	}
	// initialize bufferram to a sane default
	for (INT32 i = 0; i < 0x20000/4; i++)
		((UINT32*)DrvBufferRAM)[i] = 0x07800f0f;
	fifo_clear(&fifo_in);
	fifo_clear(&fifo_out);
	fifo_in.empty_triggered = fifo_out.empty_triggered = 0;
	geo_write_start_address = 0;
	m2->m_geo_read_start_address = 0;
	videocontrol = 0;
	framenum = 0;
	render_unk = render_mode = false;
	palette_dirty = false;
	copro_tgp_bank_reg = copro_sincos_base = copro_inv_base = copro_isqrt_base = 0;
	memset(copro_atan_base, 0, sizeof(copro_atan_base));
	gearsel = 0;
	shift_prev[0] = shift_prev[1] = 0;
	comm_cn = comm_fg = comm_zfg = 0;
	memset(io_port_value, 0xff, sizeof(io_port_value));
	io_port_config = io_serial_output = io_cmd = 0;
	io_address = 0;
	io_secondary = 0;
	adc_shift = 0;
	mpcm_bank[0] = mpcm_bank[1] = 0;
	uart_reset(0);
	uart_reset(1);
	snd_irq_line = 0;

	m2->video_reset();
	tiles_all_dirty = 1;
	palette_recalc_all();

	I960Reset();
	i960_halt_line = 0;
	irq_update();

	Mb86233Reset();
	// hold TGP in halt until we have code
	tgp_set_halt(1);

	SekOpen(0);
	SekReset();
	SekClose();

	ZetOpen(0);
	ZetReset();
	ZetClose();

	BurnYM2612Reset();
	MultiPCMResetChip(0);
	MultiPCMResetChip(1);
	mpcm_bank_w(0, 0);
	mpcm_bank_w(1, 0);
	EEPROMReset();

	frame_base = I960TotalCycles();
	return 0;
}

static void model2_video_scan(INT32 nAction)
{
	model2_state::raster_state *raster = m2->m_raster;
	model2_state::geo_state *geo = m2->m_geo;

	// MAME's save_items of raster_init / geo_init / video_start, and what MAME leaves out but a state needs here: the
	// clip planes, the window, the texture parameters, the focus and light, and the last 3D picture (shown again when
	// the geometrizer has no new frame). The polygon list itself is not saved (MAME neither): a state is always taken
	// between frames, when it has been rendered.
	SCAN_VAR(raster->min_z);
	SCAN_VAR(raster->max_z);
	SCAN_VAR(raster->command_buffer);
	SCAN_VAR(raster->command_index);
	SCAN_VAR(raster->cur_command);
	SCAN_VAR(raster->master_z_clip);
	SCAN_VAR(raster->polygon_z);
	SCAN_VAR(raster->z_adjust);
	SCAN_VAR(raster->reverse);
	SCAN_VAR(raster->viewport);
	SCAN_VAR(raster->center);
	SCAN_VAR(raster->center_sel);
	SCAN_VAR(raster->texture_ram);
	SCAN_VAR(raster->log_ram);
	SCAN_VAR(raster->cur_window);
	for (INT32 i = 0; i < 4; i++)
		for (INT32 j = 0; j < 4; j++) {
			SCAN_VAR(raster->clip_plane[i][j].normal.x);
			SCAN_VAR(raster->clip_plane[i][j].normal.y);
			SCAN_VAR(raster->clip_plane[i][j].normal.p);
			SCAN_VAR(raster->clip_plane[i][j].distance);
		}

	SCAN_VAR(geo->mode);
	SCAN_VAR(geo->matrix);
	SCAN_VAR(geo->mesh_begin);
	SCAN_VAR(geo->mesh_current);
	SCAN_VAR(geo->mesh_texture_start);
	SCAN_VAR(geo->mesh_texture_begin);
	SCAN_VAR(geo->mesh_texture_next);
	SCAN_VAR(geo->mesh_texture_end);
	SCAN_VAR(geo->mesh_mode);
	SCAN_VAR(geo->mesh_uv);
	SCAN_VAR(geo->lod);
	SCAN_VAR(geo->coef_table);
	SCAN_VAR(geo->polygon_ram0);
	SCAN_VAR(geo->polygon_ram1);
	SCAN_VAR(geo->focus.x);
	SCAN_VAR(geo->focus.y);
	SCAN_VAR(geo->focus.p);
	SCAN_VAR(geo->light.x);
	SCAN_VAR(geo->light.y);
	SCAN_VAR(geo->light.p);
	SCAN_VAR(geo->texture_parameters);

	SCAN_VAR(m2->m_geo_read_start_address);
	SCAN_VAR(m2->m_render_test_mode);
	SCAN_VAR(m2->m_render_done);
	SCAN_VAR(m2->m_crtc_xoffset);
	SCAN_VAR(m2->m_crtc_yoffset);
	SCAN_VAR(m2->m_frame_number);

	ScanVar(m2_renderer->destmap().data.data(), 512 * 512 * sizeof(UINT32), "model2 3D picture");

	// the renderer's own offsets (90, -8 until the game's first xhout / xvout write: not the CRTC values then)
	INT16 renderer_xoffs = m2_renderer->xoffset(), renderer_yoffs = m2_renderer->yoffset();
	SCAN_VAR(renderer_xoffs);
	SCAN_VAR(renderer_yoffs);

	if (nAction & ACB_WRITE) {
		raster->poly_list_index = 0;
		std::fill(std::begin(raster->poly_sorted_list), std::end(raster->poly_sorted_list), nullptr);
		m2_renderer->set_xoffset(renderer_xoffs);
		m2_renderer->set_yoffset(renderer_yoffs);
	}
}

// ---- the TGP's spaces (MAME's copro_tgp_*_map) ------------------------------------------------------------------------
static void tgp_map()
{
	Mb86233MapMemory(MB86233_PROGRAM, DrvTGPProgram, 0x0000, 0x0fff, MAP_READ);
	Mb86233MapMemory(MB86233_DATA, DrvTGPData + 0x000, 0x0000, 0x00ff, MAP_READ | MAP_WRITE);
	Mb86233MapMemory(MB86233_DATA, DrvTGPData + 0x200, 0x0200, 0x03ff, MAP_READ | MAP_WRITE);
	Mb86233SetReadHandler(MB86233_DATA, tgp_data_unmapped_r);
	Mb86233SetWriteHandler(MB86233_DATA, tgp_data_unmapped_w);
	Mb86233SetReadHandler(MB86233_IO, tgp_io_read);
	Mb86233SetWriteHandler(MB86233_IO, tgp_io_write);
	Mb86233SetRfHandlers(tgp_rf_read, tgp_rf_write);
}

// ---- init / exit ------------------------------------------------------------------------------------------------------
static void parallel_warmup(void*, INT32)
{
}

static INT32 DrvInit()
{
	// MAME's screen: 32 MHz / 2, 656 x 424 -> 57.524 Hz
	BurnSetRefreshRate(16000000.0 / (656.0 * 424.0));

	BurnAllocMemIndex();

	if (DrvLoadRoms()) return 1;

	// MAME's NVRAM "backup1" starts as DEFAULT_ALL_1 (the frontend loads the saved one over it)
	memset(DrvBackupRAM, 0xff, 0x4000);

	m2 = new model2_state();
	m2->m_textureram0 = (u32*)DrvTexRAM0;
	m2->m_textureram1 = (u32*)DrvTexRAM1;
	m2->m_bufferram = (u32*)DrvBufferRAM;
	m2->m_palram = (u16*)DrvPalRAM;
	m2->m_colorxlat = (u16*)DrvColorXlat;
	m2->m_lumaram = DrvLumaRAM;
	m2->m_fbvramA = (u16*)DrvFbVRAMA;
	m2->m_fbvramB = (u16*)DrvFbVRAMB;
	m2->video_init((u16*)DrvTexROM, 0x1000000, (u32*)DrvPolyROM, 0x1000000);
	m2_renderer = m2->m_renderer;
	screen_bitmap = new m2_bitmap<UINT32>();
	screen_bitmap->allocate(M2_WIDTH, M2_HEIGHT);
	sys24_bitmap = new m2_bitmap<UINT32>();
	sys24_bitmap->allocate(M2_WIDTH, M2_HEIGHT + 4);

	I960Init();
	i960_map();

	Mb86233Init();
	tgp_map();

	SekInit(0, 0x68000);
	SekOpen(0);
	SekMapMemory(Drv68KROM,            0x000000, 0x03ffff, MAP_ROM);
	SekMapMemory(Drv68KROM + 0x20000,  0x080000, 0x09ffff, MAP_ROM); // the upper ROM socket again
	SekMapMemory(Drv68KRAM,            0xf00000, 0xf0ffff, MAP_RAM);
	SekSetReadByteHandler(0,  snd_read_byte);
	SekSetReadWordHandler(0,  snd_read_word);
	SekSetWriteByteHandler(0, snd_write_byte);
	SekSetWriteWordHandler(0, snd_write_word);
	SekClose();

	ZetInit(0);
	ZetOpen(0);
	ZetMapMemory(DrvIOROM, 0x0000, 0x3fff, MAP_ROM);
	ZetMapMemory(DrvIORAM, 0x4000, 0x5fff, MAP_RAM);
	ZetSetReadHandler(io_read);
	ZetSetWriteHandler(io_write);
	ZetClose();

	EEPROMInit(&eeprom_interface_93C46); // (loads <saves>/<set>.nv when there is one)
	io_eeprom_was_blank = settings_blank_at_start();
	if (!io_eeprom_was_blank)
		settings_fix(); // (before the game runs: no reset)

	// sound: MAME's routes (YM3438 0.30, each MultiPCM 0.5, of the board's output). The MultiPCM make their words at
	// 10 MHz / 224 (44.64 kHz, the board's measured WORDCLK); FBNeo's core divides its clock by 180.
	SekOpen(0);
	BurnYM3438Init(1, 8000000, NULL, 0);
	BurnTimerAttachSek(M2_SND_CLOCK);
	BurnYM2612SetRoute(0, BURN_SND_YM2612_YM2612_ROUTE_1, 0.30, BURN_SND_ROUTE_LEFT);
	BurnYM2612SetRoute(0, BURN_SND_YM2612_YM2612_ROUTE_2, 0.30, BURN_SND_ROUTE_RIGHT);
	SekClose();
	MultiPCMInitChip(0, 10000000 * 180 / 224, DrvPCM1, 1);
	MultiPCMInitChip(1, 10000000 * 180 / 224, DrvPCM2, 1);
	MultiPCMSetVolumeChip(0, 0.5);
	MultiPCMSetVolumeChip(1, 0.5);
	MultiPCMSetMameModeChip(0, 1);
	MultiPCMSetMameModeChip(1, 1);

	GenericTilesInit();

	// the frontend's worker threads (the 3D, by bands of lines: model2_poly.h) started now, not at the first 3D frame
	if (nBurnParallelThreads > 1)
		BurnParallelRun(parallel_warmup, NULL, nBurnParallelThreads);
	bprintf(PRINT_IMPORTANT, _T("model2: the 3D drawn on %d thread(s)\n"), nBurnParallelThreads);

	DrvDoReset();

	memset(part_ticks, 0, sizeof(part_ticks));
	perf_frames = 0;
	perf_tick_start = m2_ticks();
	clock_gettime(CLOCK_MONOTONIC, &perf_ts_start);

	return 0;
}

static INT32 DrvExit()
{
	GenericTilesExit();

	I960Exit();
	Mb86233Exit();
	SekExit();
	ZetExit();
	BurnYM2612Exit();
	MultiPCMExit();
	EEPROMExit();

	if (m2) {
		m2->video_exit();
		delete m2;
		m2 = NULL;
	}
	m2_renderer = NULL;
	delete screen_bitmap;
	screen_bitmap = NULL;
	delete sys24_bitmap;
	sys24_bitmap = NULL;

	BurnFreeMemIndex();

	return 0;
}

// ---- a frame ----------------------------------------------------------------------------------------------------------
static void vblank_begin()
{
	// MAME's screen_vblank (then screen_update, the screen being VIDEO_UPDATE_AFTER_VBLANK)
	framenum++;
	m2->m_frame_number = framenum;

	// if 60 Hz mode or frame number is even, trigger geometrizer to start new frame
	if ((videocontrol & 1) == 0 || (framenum & 1) == 0) {
		UINT64 t0 = m2_ticks();
		m2->geo_parse();
		part_ticks[PART_GEO] += m2_ticks() - t0;
	}

	const UINT32 line = 1 << 0;
	if (intena & line) {
		intreq |= line;
		irq_update();
	}

	screen_update();
}

// the i960 up to `target`, stopping at each timer and at the delayed mask write
static void run_i960(INT64 target)
{
	UINT64 t0 = m2_ticks();
	UINT64 tgp_before = part_ticks[PART_TGP];
	for (;;) {
		INT64 now = I960TotalCycles();

		// events that are due
		if (intena_pending && intena_pending_at <= now)
			irq_mask_delayed_update();
		for (INT32 i = 0; i < 4; i++)
			if (timerrun[i] && timer_fire[i] <= now)
				timer_cb(i);

		if (now >= target) break;

		INT64 next = target;
		if (intena_pending && intena_pending_at < next) next = intena_pending_at;
		for (INT32 i = 0; i < 4; i++)
			if (timerrun[i] && timer_fire[i] < next) next = timer_fire[i];

		INT32 todo = (INT32)(next - now);
		if (todo < 1) todo = 1;
		if (I960GetHalt()) {
			// halted on a FIFO: the TGP may let it go on before the slice ends
			if (todo > 64) todo = 64;
			I960Idle(todo);
			tgp_sync();
		} else {
			I960Run(todo);
		}
	}
	tgp_sync();
	const UINT64 tgp_in = part_ticks[PART_TGP] - tgp_before;
	part_ticks[PART_I960] += (m2_ticks() - t0) - tgp_in;
}

static void DrvMakeInputs()
{
	// IN0: coin 1, coin 2, test, service, start, VR1, VR2, VR3 (active low)
	DrvInputs[0] = 0xff;
	for (INT32 i = 0; i < 8; i++)
		DrvInputs[0] ^= (DrvJoy1[i] & 1) << i;

	// the gear lever from the two shift buttons: N, then 1st to 4th (MAME's GEARS port: one button a gear)
	if (DrvJoy3[0] && !shift_prev[0] && gearsel < 4) gearsel++;
	if (DrvJoy3[1] && !shift_prev[1] && gearsel > 1) gearsel--;
	shift_prev[0] = DrvJoy3[0];
	shift_prev[1] = DrvJoy3[1];

	// IN1: VR4 (active low), the lever's value in bits 4-6
	DrvInputs[1] = 0xff;
	DrvInputs[1] ^= (DrvJoy2[0] & 1);
	DrvInputs[1] = (DrvInputs[1] & ~0x70) | (gearvalue[gearsel] << 4);

	AnalogValue[0] = ProcessAnalog(DrvAnalogPort0, 0, INPUT_DEADZONE, 0x20, 0xe0);
	AnalogValue[1] = ProcessAnalog(DrvAnalogPort1, 0, INPUT_DEADZONE | INPUT_LINEAR | INPUT_MIGHTBEDIGITAL, 0x20, 0xe0);
	AnalogValue[2] = ProcessAnalog(DrvAnalogPort2, 0, INPUT_DEADZONE | INPUT_LINEAR | INPUT_MIGHTBEDIGITAL, 0x20, 0xe0);
}

static INT32 DrvDraw();

static INT32 DrvFrame()
{
	if (DrvReset)
		DrvDoReset();

	if (DrvRecalc) {
		palette_recalc_all();
		DrvRecalc = 0;
	}

	DrvMakeInputs();

	SekNewFrame();
	ZetNewFrame();

	frame_base = I960TotalCycles();
	const INT32 nCyclesTotal[2] = { M2_SND_LINE * M2_LINES, M2_IO_LINE * M2_LINES };

	SekOpen(0);
	ZetOpen(0);
	for (INT32 line = 0; line < M2_LINES; line++) {
		if (line == M2_VBLANK_LINE)
			vblank_begin();

		const INT64 line_end = frame_base + (INT64)(line + 1) * M2_LINE_CYCLES;
		run_i960(line_end);

		UINT64 t0 = m2_ticks();
		SekSetIRQLine(2, snd_irq_line ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
		BurnTimerUpdate((line + 1) * M2_SND_LINE);
		UINT64 t1 = m2_ticks();
		ZetRun((line + 1) * M2_IO_LINE - ZetTotalCycles());
		UINT64 t2 = m2_ticks();
		part_ticks[PART_SND] += t1 - t0;
		part_ticks[PART_IO] += t2 - t1;

		uart_run(line_end);
		if (!uart[0].tx_busy)
			sound_ready(); // MAME's i8251 updates its TxRDY output every bit time while it does not send
	}
	BurnTimerEndFrame(nCyclesTotal[0]);
	if (pBurnSoundOut) {
		UINT64 t0 = m2_ticks();
		BurnYM2612Update(pBurnSoundOut, nBurnSoundLen);
		MultiPCMUpdateChip(0, pBurnSoundOut, nBurnSoundLen);
		MultiPCMUpdateChip(1, pBurnSoundOut, nBurnSoundLen);
		part_ticks[PART_SND] += m2_ticks() - t0;
	}
	ZetClose();
	SekClose();

	if (pBurnDraw)
		DrvDraw();

#if !defined(__PROSPERO__)
	// (host tests only: FBNEO_HOST_M2_FACTORY=<file of 128 bytes> writes them to a blank EEPROM at frame 8, as the
	// game writes its factory settings at its first start)
	static INT32 host_factory_done = 0;
	if (io_eeprom_was_blank && framenum == 8 && !host_factory_done) {
		const char *path = getenv("FBNEO_HOST_M2_FACTORY");
		FILE *f = path ? fopen(path, "rb") : NULL;
		UINT8 b[0x80];
		if (f) {
			if (fread(b, 1, 0x80, f) == 0x80) {
				for (INT32 i = 0; i < 0x80; i++) EEPROMWriteByte(i, b[i]);
				host_factory_done = 1;
			}
			fclose(f);
		}
	}
#endif

	// the first start with a blank EEPROM: once the game has written its factory settings (Link ID Master, country
	// JPN), Single, USA and a reset (the game has read them already)
	if (io_eeprom_was_blank && (framenum & 31) == 0 && !settings_blank_at_start()) {
		io_eeprom_was_blank = 0;
		bprintf(PRINT_IMPORTANT, _T("model2: the factory settings written: Link ID %d, country %d, byte 0x7c %d\n"),
			EEPROMReadByte(SETTINGS_LINK_ID), EEPROMReadByte(SETTINGS_COUNTRY), EEPROMReadByte(SETTINGS_COUNTRY2));
		if (settings_fix()) {
			bprintf(PRINT_IMPORTANT, _T("model2: the first start's settings changed: reset\n"));
			DrvDoReset();
		}
	}

	perf_report();

	return 0;
}

static INT32 DrvDraw()
{
	if (DrvRecalc) {
		palette_recalc_all();
		DrvRecalc = 0;
	}
	// the picture of the last vblank (MAME's screen_update, made in vblank_begin)
	UINT8 *dst = pBurnDraw;
	for (INT32 y = 0; y < M2_HEIGHT; y++, dst += nBurnPitch) {
		const UINT32 *src = &screen_bitmap->pix(y, 0);
		if (nBurnBpp == 4) {
			UINT32 *d = (UINT32*)dst;
			for (INT32 x = 0; x < M2_WIDTH; x++)
				d[x] = BurnHighCol((src[x] >> 16) & 0xff, (src[x] >> 8) & 0xff, src[x] & 0xff, 0);
		} else if (nBurnBpp == 2) {
			UINT16 *d = (UINT16*)dst;
			for (INT32 x = 0; x < M2_WIDTH; x++)
				d[x] = BurnHighCol((src[x] >> 16) & 0xff, (src[x] >> 8) & 0xff, src[x] & 0xff, 0);
		} else {
			for (INT32 x = 0; x < M2_WIDTH; x++) {
				const UINT32 c = BurnHighCol((src[x] >> 16) & 0xff, (src[x] >> 8) & 0xff, src[x] & 0xff, 0);
				dst[x * nBurnBpp + 0] = c; dst[x * nBurnBpp + 1] = c >> 8; dst[x * nBurnBpp + 2] = c >> 16;
			}
		}
	}
	return 0;
}

static INT32 DrvScan(INT32 nAction, INT32 *pnMin)
{
	struct BurnArea ba;

	if (pnMin) *pnMin = 0x029702;

	if (nAction & ACB_MEMORY_RAM) {
		memset(&ba, 0, sizeof(ba));
		ba.Data   = AllRam;
		ba.nLen   = RamEnd - AllRam;
		ba.szName = "All Ram";
		BurnAcb(&ba);
	}

	if (nAction & ACB_NVRAM) {
		memset(&ba, 0, sizeof(ba));
		ba.Data   = DrvBackupRAM;
		ba.nLen   = 0x4000;
		ba.szName = "Backup RAM";
		BurnAcb(&ba);
	}

	if (nAction & ACB_DRIVER_DATA) {
		I960Scan(nAction);
		Mb86233Scan(nAction);
		SekScan(nAction);
		ZetScan(nAction);

		BurnYM2612Scan(nAction, pnMin);
		MultiPCMScanChip(0, nAction, pnMin);
		MultiPCMScanChip(1, nAction, pnMin);
		EEPROMScan(nAction, pnMin);
		{
			// the I/O board's EEPROM (FBNeo's eeprom.cpp keeps its contents out of states)
			UINT8 ee[128];
			for (INT32 i = 0; i < 128; i++) ee[i] = EEPROMReadByte(i);
			ScanVar(ee, sizeof(ee), "I/O board EEPROM");
			if (nAction & ACB_WRITE)
				for (INT32 i = 0; i < 128; i++) EEPROMWriteByte(i, ee[i]);
		}

		model2_video_scan(nAction);

		SCAN_VAR(timervals);
		SCAN_VAR(timerorig);
		SCAN_VAR(timerrun);
		SCAN_VAR(timer_start);
		SCAN_VAR(timer_fire);
		SCAN_VAR(intreq);
		SCAN_VAR(intena);
		SCAN_VAR(intena_pending);
		SCAN_VAR(intena_pending_value);
		SCAN_VAR(intena_pending_at);
		SCAN_VAR(coproctl);
		SCAN_VAR(coprocnt);
		SCAN_VAR(geoctl);
		SCAN_VAR(geocnt);
		SCAN_VAR(videocontrol);
		SCAN_VAR(framenum);
		SCAN_VAR(geo_write_start_address);
		SCAN_VAR(render_unk);
		SCAN_VAR(render_mode);
		SCAN_VAR(palette_dirty);
		SCAN_VAR(copro_tgp_bank_reg);
		SCAN_VAR(copro_sincos_base);
		SCAN_VAR(copro_inv_base);
		SCAN_VAR(copro_isqrt_base);
		SCAN_VAR(copro_atan_base);
		SCAN_VAR(gearsel);
		SCAN_VAR(shift_prev);
		SCAN_VAR(comm_cn);
		SCAN_VAR(comm_fg);
		SCAN_VAR(comm_zfg);
		SCAN_VAR(io_port_value);
		SCAN_VAR(io_port_config);
		SCAN_VAR(io_serial_output);
		SCAN_VAR(io_cmd);
		SCAN_VAR(io_address);
		SCAN_VAR(io_secondary);
		SCAN_VAR(adc_shift);
		SCAN_VAR(mpcm_bank);
		SCAN_VAR(fifo_in);
		SCAN_VAR(fifo_out);
		SCAN_VAR(uart);
		SCAN_VAR(snd_irq_line);
		SCAN_VAR(i960_halt_line);
		SCAN_VAR(tgp_halt_line);
		SCAN_VAR(io_eeprom_was_blank);
	}

	if (nAction & ACB_WRITE) {
		tiles_all_dirty = 1;
		palette_recalc_all();
		MultiPCMSetBankChip(0, mpcm_bank[0] * 0x100000, mpcm_bank[0] * 0x100000);
		MultiPCMSetBankChip(1, mpcm_bank[1] * 0x100000, mpcm_bank[1] * 0x100000);
	}

	return 0;
}

// ---- the sets (MAME's ROM_START blocks; the regions in nType's low bits) ----------------------------------------------
#define M2_ROM(r, f) ((r) | (f))

// MAME's MODEL2_CPU_BOARD: the TGP's tables, and the 1/x and 1/sqrt(x) tables (other_data, not used)
#define MODEL2_CPU_BOARD_ROMS \
	{ "opr-14742a.45",   0x020000, 0x90c6b117, M2_ROM(R_TGPTAB, BRF_PRG | BRF_ESS) }, \
	{ "opr-14743a.46",   0x020000, 0xae7f446b, M2_ROM(R_TGPTAB, BRF_PRG | BRF_ESS) }, \
	{ "opr-14744.58",    0x020000, 0x730ea9e0, M2_ROM(R_OTHER, BRF_OPT) }, \
	{ "opr-14745.59",    0x020000, 0x4c934d96, M2_ROM(R_OTHER, BRF_OPT) }, \
	{ "opr-14746.62",    0x020000, 0x2a266cbd, M2_ROM(R_OTHER, BRF_OPT) }, \
	{ "opr-14747.63",    0x020000, 0xa4ad5e19, M2_ROM(R_OTHER, BRF_OPT) },

// the 838-10646 drive board (force feedback: not emulated) and the link board's program (not emulated)
#define DAYTONA_DRIVE_BOARD_ROMS \
	{ "epr-16488a.ic12", 0x010000, 0x546c5d1a, M2_ROM(R_DRIVE, BRF_OPT) }, \
	{ "epr-16488.ic12",  0x010000, 0x4f0b8114, M2_ROM(R_DRIVE, BRF_OPT) },

#define DAYTONA_COMMON_ROMS \
	{ "mpr-16537.ic28",  0x200000, 0x36b7c35a, M2_ROM(R_COPRO, BRF_PRG | BRF_ESS) }, \
	{ "mpr-16536.ic29",  0x200000, 0x6d6afed9, M2_ROM(R_COPRO, BRF_PRG | BRF_ESS) }, \
	{ "mpr-16523.ic16",  0x200000, 0x2f484d42, M2_ROM(R_POLY, BRF_GRA) }, \
	{ "mpr-16518.ic20",  0x200000, 0xdf683bf7, M2_ROM(R_POLY, BRF_GRA) }, \
	{ "mpr-16524.ic17",  0x200000, 0x34658bd7, M2_ROM(R_POLY, BRF_GRA) }, \
	{ "mpr-16519.ic21",  0x200000, 0xfacd1c81, M2_ROM(R_POLY, BRF_GRA) }, \
	{ "mpr-16525.ic18",  0x200000, 0xfb517521, M2_ROM(R_POLY, BRF_GRA) }, \
	{ "mpr-16520.ic22",  0x200000, 0xd66bd9bd, M2_ROM(R_POLY, BRF_GRA) },

static struct BurnRomInfo daytonaRomDesc[] = {
	{ "epr-16722a.12",   0x020000, 0x48b94318, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },
	{ "epr-16723a.13",   0x020000, 0x8af8b32d, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16528.10",    0x200000, 0x9ce591f6, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16529.11",    0x200000, 0xf7095eaf, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16808.8",     0x200000, 0x44f1f5a0, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16809.9",     0x200000, 0x37a2dd12, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-16724a.6",    0x080000, 0x469f10fd, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-16725a.7",    0x080000, 0xba0df8db, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },

	DAYTONA_COMMON_ROMS
	{ "mpr-16772.ic19",  0x200000, 0x770ed912, M2_ROM(R_POLY, BRF_GRA) },
	{ "mpr-16771.ic23",  0x200000, 0xa2205124, M2_ROM(R_POLY, BRF_GRA) },

	{ "mpr-16522.25",    0x200000, 0x55d39a57, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16521.24",    0x200000, 0xaf1934fb, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16770.27",    0x200000, 0xf9fa7bfb, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16769.26",    0x200000, 0xe57429e9, M2_ROM(R_TEX, BRF_GRA) },

	{ "epr-16726.bin",   0x020000, 0xc179b8c7, M2_ROM(R_COMM, BRF_OPT) },

	{ "epr-16720.7",     0x020000, 0x8e73cffd, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },
	{ "epr-16721.8",     0x020000, 0x1bb3b7b7, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16491.32",    0x200000, 0x89920903, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16492.33",    0x200000, 0x459e701b, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16493.4",     0x200000, 0x9990db15, M2_ROM(R_PCM2, BRF_SND) },
	{ "mpr-16494.5",     0x200000, 0x600e1d6c, M2_ROM(R_PCM2, BRF_SND) },

	MODEL2_CPU_BOARD_ROMS
	DAYTONA_DRIVE_BOARD_ROMS
};

static struct BurnRomInfo daytonaseRomDesc[] = {
	{ "epr-17369a.12",   0x020000, 0x3bc6ca62, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },
	{ "epr-17370a.13",   0x020000, 0x5d1c74e4, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16528.10",    0x200000, 0x9ce591f6, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16529.11",    0x200000, 0xf7095eaf, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16808.8",     0x200000, 0x44f1f5a0, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16809.9",     0x200000, 0x37a2dd12, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-17371.6",     0x080000, 0x7478f0d2, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-17372.7",     0x080000, 0x308a06a9, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },

	DAYTONA_COMMON_ROMS
	{ "mpr-16772.ic19",  0x200000, 0x770ed912, M2_ROM(R_POLY, BRF_GRA) },
	{ "mpr-16771.ic23",  0x200000, 0xa2205124, M2_ROM(R_POLY, BRF_GRA) },

	{ "mpr-16522.25",    0x200000, 0x55d39a57, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16521.24",    0x200000, 0xaf1934fb, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16770.27",    0x200000, 0xf9fa7bfb, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16769.26",    0x200000, 0xe57429e9, M2_ROM(R_TEX, BRF_GRA) },

	{ "epr-16726.bin",   0x020000, 0xc179b8c7, M2_ROM(R_COMM, BRF_OPT) },

	{ "epr-16720.7",     0x020000, 0x8e73cffd, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },
	{ "epr-16721.8",     0x020000, 0x1bb3b7b7, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16491.32",    0x200000, 0x89920903, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16492.33",    0x200000, 0x459e701b, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16493.4",     0x200000, 0x9990db15, M2_ROM(R_PCM2, BRF_SND) },
	{ "mpr-16494.5",     0x200000, 0x600e1d6c, M2_ROM(R_PCM2, BRF_SND) },

	MODEL2_CPU_BOARD_ROMS
	DAYTONA_DRIVE_BOARD_ROMS
};

static struct BurnRomInfo daytona93RomDesc[] = {
	{ "epr-16530a.12",   0x020000, 0x39e962b5, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },
	{ "epr-16531a.13",   0x020000, 0x693126eb, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16528.10",    0x200000, 0x9ce591f6, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16529.11",    0x200000, 0xf7095eaf, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16526.8",     0x200000, 0x5273b8b5, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16527.9",     0x200000, 0xfc4cb0ef, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-16534a.6",    0x100000, 0x1bb0d72d, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-16535a.7",    0x100000, 0x459a8bfb, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },

	DAYTONA_COMMON_ROMS
	{ "epr-16646.ic19",  0x080000, 0x7ba9fd6b, M2_ROM(R_POLY, BRF_GRA) },
	{ "epr-16645.ic23",  0x080000, 0x78fe0b8a, M2_ROM(R_POLY, BRF_GRA) },

	{ "mpr-16522.25",    0x200000, 0x55d39a57, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16521.24",    0x200000, 0xaf1934fb, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16517.27",    0x200000, 0x4705d3dd, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16516.26",    0x200000, 0xa260d45d, M2_ROM(R_TEX, BRF_GRA) },

	{ "epr-16726.bin",   0x020000, 0xc179b8c7, M2_ROM(R_COMM, BRF_OPT) },

	{ "epr-16489.7",     0x020000, 0xc20e543e, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },
	{ "epr-16490.8",     0x020000, 0xc24edaab, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16491.32",    0x200000, 0x89920903, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16492.33",    0x200000, 0x459e701b, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16493.4",     0x200000, 0x9990db15, M2_ROM(R_PCM2, BRF_SND) },
	{ "mpr-16494.5",     0x200000, 0x600e1d6c, M2_ROM(R_PCM2, BRF_SND) },

	MODEL2_CPU_BOARD_ROMS
	// (MAME: BAD_DUMP, unconfirmed for this set)
	DAYTONA_DRIVE_BOARD_ROMS
};

static struct BurnRomInfo daytonasRomDesc[] = {
	{ "epr-17965.ic12",  0x020000, 0xf022b3da, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },
	{ "epr-17966.ic13",  0x020000, 0xf9e4ece5, M2_ROM(R_MAINCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16528.10",    0x200000, 0x9ce591f6, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16529.11",    0x200000, 0xf7095eaf, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16808.8",     0x200000, 0x44f1f5a0, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "mpr-16809.9",     0x200000, 0x37a2dd12, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-17967.ic6",   0x080000, 0xa94d8690, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },
	{ "epr-17968.ic7",   0x080000, 0x9d5a92c6, M2_ROM(R_MAINDATA, BRF_PRG | BRF_ESS) },

	DAYTONA_COMMON_ROMS
	{ "mpr-16772.ic19",  0x200000, 0x770ed912, M2_ROM(R_POLY, BRF_GRA) },
	{ "mpr-16771.ic23",  0x200000, 0xa2205124, M2_ROM(R_POLY, BRF_GRA) },

	{ "mpr-16522.25",    0x200000, 0x55d39a57, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16521.24",    0x200000, 0xaf1934fb, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16770.27",    0x200000, 0xf9fa7bfb, M2_ROM(R_TEX, BRF_GRA) },
	{ "mpr-16769.26",    0x200000, 0xe57429e9, M2_ROM(R_TEX, BRF_GRA) },

	{ "epr-16726.bin",   0x020000, 0xc179b8c7, M2_ROM(R_COMM, BRF_OPT) },

	{ "epr-16720.7",     0x020000, 0x8e73cffd, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },
	{ "epr-16721.8",     0x020000, 0x1bb3b7b7, M2_ROM(R_SNDCPU, BRF_PRG | BRF_ESS) },

	{ "mpr-16491.32",    0x200000, 0x89920903, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16492.33",    0x200000, 0x459e701b, M2_ROM(R_PCM1, BRF_SND) },
	{ "mpr-16493.4",     0x200000, 0x9990db15, M2_ROM(R_PCM2, BRF_SND) },
	{ "mpr-16494.5",     0x200000, 0x600e1d6c, M2_ROM(R_PCM2, BRF_SND) },

	MODEL2_CPU_BOARD_ROMS
	// (MAME: BAD_DUMP, unconfirmed for this set)
	DAYTONA_DRIVE_BOARD_ROMS
};

// the Model 1 I/O board's ROM (MAME's model1io device: its BIOS choice for Daytona USA is EPR-14869C)
static struct BurnRomInfo emptyRomDesc[] = {
	{ "",                    0,          0, 0 },
};

static struct BurnRomInfo Model1ioRomDesc[] = {
	{ "epr-14869c.25",   0x010000, 0x24b68e64, BRF_PRG | BRF_ESS | BRF_BIOS },
	{ "epr-14869.25",    0x010000, 0x6187cd7a, BRF_PRG | BRF_OPT | BRF_BIOS },
	{ "epr-14869b.25",   0x010000, 0x2d093304, BRF_PRG | BRF_OPT | BRF_BIOS },
};

STD_ROM_PICK(Model1io)
STD_ROM_FN(Model1io)

STDROMPICKEXT(daytona, daytona, Model1io)
STD_ROM_FN(daytona)
STDROMPICKEXT(daytonase, daytonase, Model1io)
STD_ROM_FN(daytonase)
STDROMPICKEXT(daytona93, daytona93, Model1io)
STD_ROM_FN(daytona93)
STDROMPICKEXT(daytonas, daytonas, Model1io)
STD_ROM_FN(daytonas)

struct BurnDriver BurnDrvModel1io = {
	"model1io", NULL, NULL, NULL, "1992",
	"Sega Model 1 I/O Board\0", "BIOS only", "Sega", "Sega Model 1 I/O",
	NULL, NULL, NULL, NULL,
	BDF_BOARDROM, 0, HARDWARE_SEGA_MODEL2, GBF_BIOS, 0,
	NULL, Model1ioRomInfo, Model1ioRomName, NULL, NULL, NULL, NULL, DaytonaInputInfo, NULL,
	NULL, NULL, NULL, NULL, NULL,
	NULL, 0x2000, M2_WIDTH, M2_HEIGHT, 4, 3
};

struct BurnDriver BurnDrvDaytona = {
	"daytona", NULL, "model1io", NULL, "1994",
	"Daytona USA (Revision A)\0", NULL, "Sega", "Sega Model 2",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING, 1, HARDWARE_SEGA_MODEL2, GBF_RACING, 0,
	NULL, daytonaRomInfo, daytonaRomName, NULL, NULL, NULL, NULL, DaytonaInputInfo, NULL,
	DrvInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x2000,
	M2_WIDTH, M2_HEIGHT, 4, 3
};

struct BurnDriver BurnDrvDaytonase = {
	"daytonase", "daytona", "model1io", NULL, "1994",
	"Daytona USA Special Edition (Revision A)\0", NULL, "Sega", "Sega Model 2",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE, 1, HARDWARE_SEGA_MODEL2, GBF_RACING, 0,
	NULL, daytonaseRomInfo, daytonaseRomName, NULL, NULL, NULL, NULL, DaytonaInputInfo, NULL,
	DrvInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x2000,
	M2_WIDTH, M2_HEIGHT, 4, 3
};

struct BurnDriver BurnDrvDaytona93 = {
	"daytona93", "daytona", "model1io", NULL, "1993",
	"Daytona USA\0", NULL, "Sega", "Sega Model 2",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE, 1, HARDWARE_SEGA_MODEL2, GBF_RACING, 0,
	NULL, daytona93RomInfo, daytona93RomName, NULL, NULL, NULL, NULL, DaytonaInputInfo, NULL,
	DrvInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x2000,
	M2_WIDTH, M2_HEIGHT, 4, 3
};

struct BurnDriver BurnDrvDaytonas = {
	"daytonas", "daytona", "model1io", NULL, "1994",
	"Daytona USA (with Saturn advertisements)\0", NULL, "Sega", "Sega Model 2",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE, 1, HARDWARE_SEGA_MODEL2, GBF_RACING, 0,
	NULL, daytonasRomInfo, daytonasRomName, NULL, NULL, NULL, NULL, DaytonaInputInfo, NULL,
	DrvInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x2000,
	M2_WIDTH, M2_HEIGHT, 4, 3
};
