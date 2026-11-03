// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// FBNeo PS5 2.3: Sega Model 2's geometrizer and 3D rasterizer from MAME (src/mame/sega/model2.h, model2_v.cpp,
// model2rd.ipp; mamedev/mame b7f483f), without MAME's device framework: the structures below are MAME's, the class
// keeps the members and functions model2_v.cpp uses (model2_v.cpp is MAME's code, see its header), and the renderer
// is MAME's on model2_poly.h (poly.h's extents, no threads). The driver (d_model2.cpp) owns the memory these point at.
#ifndef FBNEO_MODEL2_H
#define FBNEO_MODEL2_H

#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include "model2_poly.h"
#include "model2_helpers.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

// MAME's bitmap_t, as the renderer uses it
template<typename T>
struct m2_bitmap
{
	std::vector<T> data;
	int width = 0, height = 0;
	void allocate(int w, int h) { width = w; height = h; data.assign(size_t(w) * h, T(0)); }
	T& pix(int y, int x = 0) { return data[size_t(y) * width + x]; }
	int rowpixels() const { return width; }
	void fill(T v, const m2_rectangle& r)
	{
		for (int y = std::max(r.min_y, 0); y <= std::min(r.max_y, height - 1); y++)
			for (int x = std::max(r.min_x, 0); x <= std::min(r.max_x, width - 1); x++)
				data[size_t(y) * width + x] = v;
	}
};

// MAME's copybitmap_trans with no offset
template<typename T>
static inline void m2_copybitmap_trans(m2_bitmap<T>& dst, m2_bitmap<T>& src, const m2_rectangle& r, uint32_t trans)
{
	for (int y = r.min_y; y <= r.max_y; y++)
	{
		const T* s = &src.pix(y, r.min_x);
		T* d = &dst.pix(y, r.min_x);
		for (int x = r.min_x; x <= r.max_x; x++, s++, d++)
			if (*s != trans)
				*d = *s;
	}
}

class model2_renderer;

struct m2_poly_extra_data;

class model2_state
{
public:
	struct plane;
	struct texture_parameter;
	struct polygon;
	struct quad_m2;
	struct raster_state;
	struct geo_state;

	// memory (the driver's)
	u32* m_textureram0 = nullptr;   // 0x200000 bytes
	u32* m_textureram1 = nullptr;
	u32* m_bufferram = nullptr;     // 0x20000 bytes
	u16* m_palram = nullptr;        // 0x2000 entries
	u16* m_colorxlat = nullptr;     // 0x6000 entries
	u8* m_lumaram = nullptr;        // 0x8000
	u16* m_fbvramA = nullptr;       // 0x40000 entries
	u16* m_fbvramB = nullptr;
	u8 m_gamma_table[256];
	model2_renderer* m_renderer = nullptr;
	raster_state* m_raster = nullptr;
	geo_state* m_geo = nullptr;

	u32 m_geo_read_start_address = 0;
	bool m_geo_mesh = false;
	bool m_render_test_mode = false;
	bool m_render_done = false;
	int16_t m_crtc_xoffset = 0, m_crtc_yoffset = 0;
	u32 m_frame_number = 0;

	// the port's setup (MAME's video_start; the ROMs: texture ROM 16-bit words, polygon ROM 32-bit words)
	void video_init(u16* texture_rom, u32 texture_rom_bytes, u32* polygon_rom, u32 polygon_rom_bytes);
	void video_exit();
	void video_reset();

	// MAME's
	void raster_init(u16* texture_rom, u32 texture_rom_bytes);
	void geo_init(u32* polygon_rom, u32 polygon_rom_bytes);
	void model2_3d_zclip_w(u32 data);
	u32 polygon_count_r();
	void horizontal_sync_w(u16 data);
	void vertical_sync_w(u16 data);
	void render_frame_start();
	void geo_parse();
	void render_polygons(m2_bitmap<u32>& bitmap, const m2_rectangle& cliprect);
	void draw_framebuffer(m2_bitmap<u32>& bitmap, const m2_rectangle& cliprect);

	const u32* geo_process_command(geo_state& geo, u32 opcode, const u32* input, bool* end_code);
	// geo commands
	const u32* geo_nop(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_object_data(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_direct_data(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_window_data(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_texture_data(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_polygon_data(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_texture_parameters(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_mode(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_zsort_mode(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_focal_distance(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_light_source(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_matrix_write(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_mesh_mode(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_mesh_draw(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_mesh_flush(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_translate_write(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_data_mem_push(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_test(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_end(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_dummy(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_log_data(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_lod(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_code_upload(geo_state& geo, u32 opcode, const u32* input);
	const u32* geo_code_jump(geo_state& geo, u32 opcode, const u32* input);
	// geo code drawing paths
	void geo_parse_np_ns(geo_state& geo, const u32* input, u32 count);
	void geo_parse_np_s(geo_state& geo, const u32* input, u32 count);
	void geo_parse_nn_ns(geo_state& geo, const u32* input, u32 count);
	void geo_parse_nn_s(geo_state& geo, const u32* input, u32 count);

	// raster functions
	// main data input port
	void model2_3d_push(raster_state* raster, u32 input);
	// polygon push path
	template <unsigned NumVerts>
	void model2_3d_process_polygon(raster_state* raster, u32 attr);

	// inliners
	inline void model2_3d_project(polygon* poly);
	inline u16 float_to_zval(float floatval, s32 z_adjust);
	inline bool check_culling(raster_state* raster, u32 attr, float min_z, float max_z);
};

/*****************************
 *
 * Modern polygon renderer
 *
 *****************************/

struct m2_poly_extra_data
{
	model2_state *  state;
	u32      lumabase;
	u32      colorbase;
	u8       checker;
	u32 *    texsheet[2];
	u32      texwidth;
	u32      texheight;
	u32      texx;
	u32      texy;
	u8       texwrapx;
	u8       texwrapy;
	u8       texmirrorx;
	u8       texmirrory;
	u8       utex;
	u8       utexminlod;
	u32      utexx;
	u32      utexy;
	s32      texlod;
	u8       luma;
};

typedef uint32_t rgb_t_value;

class model2_renderer : public m2_poly_manager<float, m2_poly_extra_data, 4>
{
public:
	using polygon = model2_state::polygon;
	typedef m2_rectangle rectangle;

	model2_renderer(model2_state& state) :
		m_state(state),
		m_xoffs(90),
		m_yoffs(-8)
	{
		m_render_callbacks[0] = { &model2_renderer::cb_solid0, this };
		m_render_callbacks[1] = { &model2_renderer::cb_solid1, this };
		m_render_callbacks[2] = { &model2_renderer::cb_tex0, this };
		m_render_callbacks[3] = { &model2_renderer::cb_tex1, this };
		m_destmap.allocate(512, 512);
		m_fillmap.allocate(512, 512);
	}

	m2_bitmap<u32>& destmap() { return m_destmap; }
	m2_bitmap<u8>& fillmap() { return m_fillmap; }

	void model2_3d_render(polygon* poly, const m2_rectangle& cliprect);
	void set_xoffset(int16_t xoffs) { m_xoffs = xoffs; }
	void set_yoffset(int16_t yoffs) { m_yoffs = yoffs; }

	template <bool Translucent>
	void draw_scanline_solid(int32_t scanline, const extent_t& extent, const m2_poly_extra_data& object, int threadid);

	template <bool Translucent>
	void draw_scanline_tex(int32_t scanline, const extent_t& extent, const m2_poly_extra_data& object, int threadid);

private:
	static void cb_solid0(void* o, int32_t s, const extent_t& e, const m2_poly_extra_data& d, int t) { ((model2_renderer*)o)->draw_scanline_solid<false>(s, e, d, t); }
	static void cb_solid1(void* o, int32_t s, const extent_t& e, const m2_poly_extra_data& d, int t) { ((model2_renderer*)o)->draw_scanline_solid<true>(s, e, d, t); }
	static void cb_tex0(void* o, int32_t s, const extent_t& e, const m2_poly_extra_data& d, int t) { ((model2_renderer*)o)->draw_scanline_tex<false>(s, e, d, t); }
	static void cb_tex1(void* o, int32_t s, const extent_t& e, const m2_poly_extra_data& d, int t) { ((model2_renderer*)o)->draw_scanline_tex<true>(s, e, d, t); }

	render_delegate m_render_callbacks[4];

	model2_state &m_state;
	m2_bitmap<u32> m_destmap;
	m2_bitmap<u8> m_fillmap;
	int16_t m_xoffs, m_yoffs;

	template <bool Translucent>
	u32 fetch_bilinear_texel(const m2_poly_extra_data& object, s32 miplevel, s32 fu, s32 fv);

	static u16 get_texel(u32 base_x, u32 base_y, int x, int y, const u32 *sheet);
};

typedef model2_renderer::vertex_t poly_vertex;


/*******************************************
 *
 *  Basic Data Types
 *
 *******************************************/

struct model2_state::plane
{
	plane() : normal(0, 0)
	{
		std::fill(std::begin(normal.p), std::end(normal.p), 0);
	}

	poly_vertex     normal;
	float           distance = 0;
};

struct model2_state::texture_parameter
{
	float           diffuse = 0;
	float           ambient = 0;
	u32             specular_control = 0;
	float           specular_scale = 0;
};

struct model2_state::polygon
{
	polygon()
	{
		for (poly_vertex &vertex : v)
		{
			vertex.x = vertex.y = 0;
			std::fill(std::begin(vertex.p), std::end(vertex.p), 0);
		}
	}

	void *          next = nullptr;
	poly_vertex     v[8];
	u8              num_vertices = 3;
	u16             z = 0;
	u16             texheader[4] = { 0, 0, 0, 0 };
	u8              luma = 0;
	s32             texlod = 0;
	int16_t         viewport[4] = { 0, 0, 0, 0 };
	int16_t         center[2] = { 0, 0 };
	u8              window = 0;
};

struct model2_state::quad_m2
{
	quad_m2()
	{
		for (poly_vertex &vertex : v)
		{
			vertex.x = vertex.y = 0;
			std::fill(std::begin(vertex.p), std::end(vertex.p), 0);
		}
	}

	poly_vertex     v[4];
	u16             z = 0;
	u16             texheader[4] = { 0, 0, 0, 0 };
	u8              luma = 0;
	s32             texlod = 0;
};

/*******************************************
 *
 *  Hardware 3D Rasterizer Internal State
 *
 *******************************************/

#define MAX_POLYGONS       32768

struct model2_state::raster_state
{
	raster_state()
	{
		std::fill(std::begin(command_buffer), std::end(command_buffer), 0);
		std::fill(std::begin(poly_sorted_list), std::end(poly_sorted_list), nullptr);
		std::fill(std::begin(texture_ram), std::end(texture_ram), 0);
		std::fill(std::begin(log_ram), std::end(log_ram), 0);
	}

//  u32             mode = 0;                       // bit 0 = Test Mode, bit 2 = Switch 60Hz(1)/30Hz(0) operation
	u16 const *     texture_rom = nullptr;          // Texture ROM data
	u32             texture_rom_mask = 0;           // Texture ROM address mask
	int16_t         viewport[4] = { 0, 0, 0, 0 };   // View port (startx,starty,endx,endy)
	int16_t         center[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } }; // Centers (eye 0[x,y],1[x,y],2[x,y],3[x,y])
	u16             center_sel = 0;                 // Selected center
	u32             reverse = 0;                    // Left/Right Reverse
	s32             z_adjust = 0;                   // ZSort Mode
	float           polygon_z = 0;                  // Current polygon z value
	u8              master_z_clip = 0;              // Master Z-Clip value
	u32             cur_command = 0;                // Current command
	u32             command_buffer[32];             // Command buffer
	u32             command_index = 0;              // Command buffer index
	polygon         poly_list[MAX_POLYGONS];        // Polygon list
	u32             poly_list_index = 0;            // Polygon list index
	polygon *       poly_sorted_list[0x10000];      // Sorted polygon list
	u16             min_z = 0;                      // Minimum sortable Z value
	u16             max_z = 0;                      // Maximum sortable Z value
	u16             texture_ram[0x10000];           // Texture RAM pointer
	u8              log_ram[0x8000];                // Log RAM pointer
	u8              cur_window = 0;                 // Current window
	plane           clip_plane[4][4];               // Polygon clipping planes
};

/*******************************************
 *
 *  Geometry Engine Internal State
 *
 *******************************************/

struct model2_state::geo_state
{
	geo_state() : focus(0, 0), light(0, 0)
	{
		std::fill(std::begin(matrix), std::end(matrix), 0);
		std::fill(std::begin(focus.p), std::end(focus.p), 0);
		std::fill(std::begin(light.p), std::end(light.p), 0);
		std::fill(std::begin(coef_table), std::end(coef_table), 0);
		std::fill(std::begin(polygon_ram0), std::end(polygon_ram0), 0);
		std::fill(std::begin(polygon_ram1), std::end(polygon_ram1), 0);
		std::fill(std::begin(mesh_uv), std::end(mesh_uv), 0);
	}

	raster_state *      raster = nullptr;
	u32                 mode = 0;                   // bit 0 = Enable Specular, bit 1 = Calculate Normals
	u32 const *         polygon_rom = nullptr;      // Polygon ROM data
	u32                 polygon_rom_mask = 0;       // Polygon ROM address mask
	u32                 mesh_begin = 0x30100;
	u32                 mesh_current = 0x30100;
	u32                 mesh_texture_start = 0x80c000;
	u32                 mesh_texture_begin = 0x80c000;
	u32                 mesh_texture_next = 0x80c000;
	u32                 mesh_texture_end = 0x80c000;
	u32                 mesh_mode = 0;
	u32                 mesh_uv[0x8000];
	float               matrix[12];                 // Current Transformation Matrix
	poly_vertex         focus;                      // Focus (x,y)
	poly_vertex         light;                      // Light Vector
	float               lod = 0;                    // LOD
	float               coef_table[32];             // Distane Coefficient table
	texture_parameter   texture_parameters[32];     // Texture parameters
	u32                 polygon_ram0[0x8000];       // Fast Polygon RAM pointer
	u32                 polygon_ram1[0x8000];       // Slow Polygon RAM pointer
	model2_state *      state = nullptr;
};

#endif
