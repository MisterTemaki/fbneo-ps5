#!/usr/bin/env python3
# Makes src/burn/drv/sega/model2_v.cpp and model2rd.ipp from MAME's src/mame/sega/model2_v.cpp and model2rd.ipp
# (mamedev/mame b7f483f): the geometrizer, the 3D rasterizer and the scanline renderers kept as they are but for
# MAME's framework (listed in the headers written below). Run once; the results are committed.
#
#   port_model2_video.py <mame>/src/mame/sega <fbneo>/src/burn/drv/sega
#
# The script: MIT (FBNeo PS5's ps5/ code); what it makes is MAME's code, BSD-3-Clause.
# SPDX-License-Identifier: MIT
import re
import sys

mame, out = sys.argv[1], sys.argv[2]

# ---- model2_v.cpp ---------------------------------------------------------------------------------------------------
lines = open(mame + '/model2_v.cpp').read().split('\n')
notes = '\n'.join(lines[3:87])          # MAME's notes on the hardware (lines 4-87, inside its comment block)
body = lines[100:2455]                  # "#define pz" .. the line before video_start
text = '\n'.join(body)

def sub_function(text, start_sig, new_body):
    i = text.index(start_sig)
    j = text.index('\n}\n', i) + 3
    return text[:i] + new_body + text[j:]

text = sub_function(text, 'void model2_state::raster_init(memory_region &texture_rom)', '''void model2_state::raster_init(u16* texture_rom, u32 texture_rom_bytes)
{
	m_raster = new raster_state();

	// (MAME asserts a power of two, 16-bit words)
	m_raster->texture_rom = texture_rom;
	m_raster->texture_rom_mask = (texture_rom_bytes / 2) - 1;
	// (the state is saved by the driver: model2_v_scan)
}
''')
text = sub_function(text, 'void model2_state::geo_init(memory_region &polygon_rom)', '''void model2_state::geo_init(u32* polygon_rom, u32 polygon_rom_bytes)
{
	m_geo = new geo_state();
	m_geo->state = this;

	// (MAME asserts a power of two, 32-bit words)
	m_geo->raster = m_raster;
	m_geo->polygon_rom = polygon_rom;
	m_geo->polygon_rom_mask = (polygon_rom_bytes / 4) - 1;
	// (the state is saved by the driver: model2_v_scan)
}
''')

rep = [
    ('util::sext(', 'm2_sext('),
    ('std::clamp(', 'm2_clamp('),
    ('logerror(', 'M2_LOG('),
    ('[[maybe_unused]] u64  code;', 'u64  code; (void)code;'),
    ('m_screen->frame_number()', 'm_frame_number'),
    ('const rectangle &cliprect', 'const m2_rectangle &cliprect'),
    ('	rectangle vp(', '	m2_rectangle vp('),
    ('bitmap_rgb32 &bitmap', 'm2_bitmap<u32> &bitmap'),
    ('raster_state *raster = m_raster.get();', 'raster_state *raster = m_raster;'),
    ('copybitmap_trans(bitmap, m_renderer->destmap(), 0, 0, 0, 0, cliprect, 0x00000000);',
     'm2_copybitmap_trans(bitmap, m_renderer->destmap(), cliprect, 0x00000000);'),
]
for a, b in rep:
    assert a in text, a
    text = text.replace(a, b)

# the polygon list is full: MAME stops (fatalerror); the port drops the polygon (logged once a frame)
old = '''			/* go through the clipped vertex list, adding polygons */
			polygon &poly = raster->poly_list[raster->poly_list_index++];

			if (raster->poly_list_index >= MAX_POLYGONS)
			{
				fatalerror("SEGA 3D: Max polygon limit exceeded\\n");
			}
'''
new = '''			/* go through the clipped vertex list, adding polygons */
			if (raster->poly_list_index >= MAX_POLYGONS - 1)
			{
				// (MAME stops here with a fatal error; the port drops the polygon)
				M2_LOG("SEGA 3D: Max polygon limit exceeded\\n");
				goto update_linking;
			}
			polygon &poly = raster->poly_list[raster->poly_list_index++];
'''
assert old in text
text = text.replace(old, new)
old = '''	/* update linking */
	switch ((attr >> 8) & 3)'''
assert text.count(old) == 1
text = text.replace(old, '''	/* update linking */
update_linking:
	switch ((attr >> 8) & 3)''')
# the poly variable is declared after the goto's label jump-over: put the block in braces
old = '''		if (clipped_verts > 2)
		{
			/* adjust and set the object z-sort value */'''
assert old in text
# (the goto jumps out of this block, which C++ allows: no initialisation is skipped at the label)

old = '''			default:
			{
				fatalerror("SEGA 3D: Unknown rasterizer command %08x\\n", raster->cur_command);
			}'''
assert old in text
text = text.replace(old, '''			default:
			{
				// MAME: fatalerror; the port drops the command
				M2_LOG("SEGA 3D: Unknown rasterizer command %08x\\n", raster->cur_command);
				raster->cur_command = 0;
			}''')

# The geometrizer's commands trust the counts and addresses they read (MAME reads and writes on past the buffer RAM,
# the polygon RAMs and the texture parameters): the port keeps each within its memory. Within one function only.
def patch_in(text, sig, old, new):
    i = text.index(sig)
    j = text.index('\n}\n', i)
    fn = text[i:j]
    assert fn.count(old) == 1, (sig, old)
    return text[:i] + fn.replace(old, new) + text[j:]

text = patch_in(text, 'const u32 *model2_state::geo_object_data(', '''	if (obc == 0)
		obc = 0xfffff;
''', '''	if (obc == 0)
		obc = 0xfffff;

	// FBNeo PS5: no more objects than words left in the RAM or ROM they are read from (each object is a word at least;
	// the last one's few words past the end are read from the zeroed guard, or the next region of the driver)
	{
		u32 const left = (oba & 0x01000000) || !(oba & 0x00800000) ? 0x8000 - (oba & 0x7fff) :
			(geo.polygon_rom_mask + 1) - (oba & geo.polygon_rom_mask);
		obc = std::min(obc, left);
	}
''')
text = patch_in(text, 'const u32 *model2_state::geo_direct_data(', '''	while (((attr = *input++) & 3) != 0)''',
    '''	while (geo_words_left(input) >= 9 && ((attr = *input++) & 3) != 0) // (FBNeo PS5: a whole one, in the buffer RAM)''')
text = patch_in(text, 'const u32 *model2_state::geo_texture_data(', '''	u32 count = *input++;''',
    '''	u32 count = std::min<u32>(*input++, geo_words_left(input)); // (FBNeo PS5: not past the buffer RAM)''')
text = patch_in(text, 'const u32 *model2_state::geo_polygon_data(', '''	count = *input++;
''', '''	count = *input++;
	// FBNeo PS5: not past the polygon RAM, nor the buffer RAM
	count = std::min(count, std::min<u32>(0x8000 - (address & 0x7fff), geo_words_left(input)));
''')
text = patch_in(text, 'const u32 *model2_state::geo_texture_parameters(', '''	index = (*input++) >> 2;''',
    '''	index = ((*input++) >> 2) & 0x1f; // (FBNeo PS5: one of the 32)''')
text = patch_in(text, 'const u32 *model2_state::geo_texture_parameters(', '''	u32 const count = *input++;''',
    '''	u32 const count = std::min<u32>(*input++, geo_words_left(input) / 2); // (FBNeo PS5: not past the buffer RAM)''')
text = patch_in(text, 'const u32 *model2_state::geo_log_data(', '''	u32 const count = *input++;''',
    '''	u32 const count = std::min<u32>(*input++, geo_words_left(input)); // (FBNeo PS5: not past the buffer RAM)''')
text = patch_in(text, 'const u32 *model2_state::geo_code_upload(', '''	u32 const count = *input++;''',
    '''	u32 const count = std::min<u32>(*input++, geo_words_left(input) / 3); // (FBNeo PS5: not past the buffer RAM)''')
text = patch_in(text, 'const u32 *model2_state::geo_test(', '''	u32 const blocks = *input++;''',
    '''	u32 const blocks = std::min<u32>(*input++, geo_words_left(input) / 3); // (FBNeo PS5: not past the buffer RAM)''')

for bad in ['fatalerror(', 'util::', 'machine()', 'memregion', 'save_item', 'std::make_unique', 'popmessage']:
    assert bad not in text, bad

hdr = '''// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
/*********************************************************************************************************************************

    FBNeo PS5 2.3: MAME's src/mame/sega/model2_v.cpp (mamedev/mame b7f483f), lines 101-2455, made by
    ps5/tools/mame_port/port_model2_video.py: MAME's geometrizer and rasterizer code, but for its framework --
    raster_init/geo_init take the ROMs' pointers (the driver saves the state), logerror logs through M2_LOG, MAME's
    rectangle and bitmap are model2.h's m2_rectangle and m2_bitmap,
    util::sext/std::clamp/copybitmap_trans are local helpers, a full polygon list drops the polygon and an unknown
    rasterizer command is dropped (MAME stops with fatalerror), and the geometrizer's commands stay within the buffer
    RAM, the polygon RAMs and the texture parameters (MAME trusts their counts and indexes). video_start and
    screen_update are the driver's (d_model2.cpp). MAME's notes:

'''
src_hdr = hdr + notes + '''
*********************************************************************************************************************************/

#include "model2.h"
#include "model2rd.ipp"

#include <cmath>
#include <limits>

'''
open(out + '/model2_v.cpp', 'w').write(src_hdr + text + '\n')

# ---- model2rd.ipp ---------------------------------------------------------------------------------------------------
rd = open(mame + '/model2rd.ipp').read()
rep = [
    ('#include "corefloat.h"\n', '#include "model2_helpers.h"\n'),
    ('#include <bit>\n', ''),
    ('s32 max_level = 30 - std::countl_zero(std::min(object.texwidth, object.texheight));',
     's32 max_level = 30 - m2_countl_zero(std::min(object.texwidth, object.texheight));'),
    ('std::clamp(', 'm2_clamp('),
    ('inline s32 ATTR_FORCE_INLINE fast_log2(float value)', 'static inline s32 fast_log2(float value)'),
    ('if (UNEXPECTED(value < 0.0F))', 'if (value < 0.0F)'),
    ('color = rgb_t(tr, tg, tb);', 'color = m2_rgb(tr, tg, tb);'),
    ('p[x] = rgb_t(tr, tg, tb);', 'p[x] = m2_rgb(tr, tg, tb);'),
]
for a, b in rep:
    assert a in rd, a
    rd = rd.replace(a, b)
rd = rd.replace('''// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
''', '''// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// FBNeo PS5 2.3: MAME's src/mame/sega/model2rd.ipp (mamedev/mame b7f483f), made by
// ps5/tools/mame_port/port_model2_video.py: MAME's code but for countl_zero, clamp and MAME's colour type, which are
// model2_helpers.h's (the core is C++11), and the forced-inline / branch-hint macros left out.
''', 1)
for bad in ['rgb_t(', 'std::countl_zero', 'std::clamp']:
    assert bad not in rd, bad
open(out + '/model2rd.ipp', 'w').write(rd)
