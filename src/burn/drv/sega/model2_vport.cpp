// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// FBNeo PS5 2.3: what MAME's video_start (src/mame/sega/model2.cpp, mamedev/mame b7f483f) does for the Model 2 video
// state of model2_v.cpp, and the state a reset gives it -- the port's setup of MAME's code, kept apart from the
// driver (d_model2.cpp) so that the renderer's own test (ps5/tests/test_model2_video.cpp) builds the same state.
#include "burnint.h"
#include "model2.h"
#include "model2rd.ipp" // (the renderer made here: its scanline functions)

#include <new>

void model2_state::video_init(u16* texture_rom, u32 texture_rom_bytes, u32* polygon_rom, u32 polygon_rom_bytes)
{
	m_renderer = new model2_renderer(*this);

	/* initialize the hardware rasterizer */
	raster_init(texture_rom, texture_rom_bytes);

	/* initialize the geometry engine */
	geo_init(polygon_rom, polygon_rom_bytes);

	// convert color space; this works OK for most games
	// real cabinets probably have their monitors calibrated depending on the game
	// optimal settings for vf2, fvipers and schamp are bias 64, gain 51
	for (int i = 0; i < 256; i++)
	{
		double raw_value = std::max(((double)i - 64.0) * 255.0 / 191.0, 0.0);
		m_gamma_table[i] = (u8)raw_value;
	}
}

void model2_state::video_exit()
{
	delete m_renderer;
	m_renderer = nullptr;
	delete m_geo;
	m_geo = nullptr;
	delete m_raster;
	m_raster = nullptr;
}

void model2_state::video_reset()
{
	// what MAME's machine has after video_start (the renderer's offsets: until the game writes xhout / xvout)
	raster_state* raster = m_raster;
	const u16* trom = raster->texture_rom;
	const u32 tmask = raster->texture_rom_mask;
	raster->~raster_state();
	new (raster) raster_state();
	raster->texture_rom = trom;
	raster->texture_rom_mask = tmask;

	geo_state* geo = m_geo;
	const u32* prom = geo->polygon_rom;
	const u32 pmask = geo->polygon_rom_mask;
	geo->~geo_state();
	new (geo) geo_state();
	geo->polygon_rom = prom;
	geo->polygon_rom_mask = pmask;
	geo->raster = raster;
	geo->state = this;

	m_geo_read_start_address = 0;
	m_geo_mesh = false;
	m_render_test_mode = false;
	m_render_done = false;
	m_crtc_xoffset = m_crtc_yoffset = 0;
	m_frame_number = 0;
	m_renderer->set_xoffset(90);
	m_renderer->set_yoffset(-8);
	m_renderer->destmap().fill(0, m2_rectangle(0, 511, 0, 511));
	m_renderer->fillmap().fill(0, m2_rectangle(0, 511, 0, 511));
}

