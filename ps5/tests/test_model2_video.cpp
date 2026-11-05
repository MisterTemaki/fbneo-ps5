// FBNeo PS5 2.3: Sega Model 2's renderer on its own (make test-model2-video): MAME's rasterizer (model2_v.cpp and
// model2rd.ipp, src/burn/drv/sega) with the port's poly manager (model2_poly.h: the primitives kept until wait() and
// drawn by bands of lines, on worker threads when there are some), on synthetic polygons -- no ROM:
//   - flat polygons: their colour from the palette, the colour tables and the luma (MAME's colorxlat lookup), the
//     front-to-back fill (the nearer one wins where they overlap), the checker (every other pixel), a five-sided one
//     (render_polygon), the viewport clip;
//   - textured ones (a synthetic texture sheet, the luma table) and a translucent one;
//   - (2.3 review) the geometrizer's commands with counts and indexes that would run past its memories (MAME's code
//     trusts them): a polygon RAM copy of 2^32 words, a texture parameter index of 2^30, direct data and texture data
//     that run to the end of the buffer RAM (where each stops: the pointer returned, the words written), object lists
//     that never end from the end of the slow RAM, the fast RAM and the polygon ROM -- under ASan, none reads or
//     writes past its memory and the zeroed words kept after it;
//   - the picture is the same drawn in one part, in 16 bands done backwards and in 16 bands on four threads (the
//     bands are independent), and it is the golden one (its checksum, kGolden below; --write saves
//     the picture and shows its checksum).
//
//   test_model2_video [--write <out.ppm>]
//
// SPDX-License-Identifier: BSD-3-Clause

#include "burnint.h"
#include "burn_parallel.h"
#include "model2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define pz p[0]
#define pu p[1]
#define pv p[2]

// what model2_v.cpp takes from the driver
void m2_log(const char*, ...) {}
INT32(__cdecl* BurnAcb)(struct BurnArea* pba) = NULL;
static INT32 __cdecl TestPrintf(INT32, TCHAR*, ...) { return 0; }
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

uint64_t Fnv(const std::vector<uint32_t>& v)
{
	uint64_t h = 1469598103934665603ull;
	for (uint32_t x : v)
		for (int i = 0; i < 4; i++)
			h = (h ^ ((x >> (8 * i)) & 0xff)) * 1099511628211ull;
	return h;
}

constexpr int kW = 496, kH = 384;

struct Video
{
	std::vector<u16> tex_rom = std::vector<u16>(0x10000);
	std::vector<u32> poly_rom = std::vector<u32>(0x1000 + 16); // (+ 16 zeroed words, as the driver has)
	std::vector<u32> tex0 = std::vector<u32>(0x200000 / 4), tex1 = std::vector<u32>(0x200000 / 4);
	std::vector<u32> buffer = std::vector<u32>(0x20000 / 4 + 64); // (+ 64 zeroed words, as the driver has)
	std::vector<u16> pal = std::vector<u16>(0x2000), xlat = std::vector<u16>(0x6000);
	std::vector<u8> luma = std::vector<u8>(0x8000);
	std::vector<u16> fbA = std::vector<u16>(0x40000), fbB = std::vector<u16>(0x40000);
	model2_state st;

	Video()
	{
		st.m_textureram0 = tex0.data();
		st.m_textureram1 = tex1.data();
		st.m_bufferram = buffer.data();
		st.m_palram = pal.data();
		st.m_colorxlat = xlat.data();
		st.m_lumaram = luma.data();
		st.m_fbvramA = fbA.data();
		st.m_fbvramB = fbB.data();
		st.video_init(tex_rom.data(), u32(tex_rom.size() * 2), poly_rom.data(), 0x1000 * 4);
		st.video_reset();
		st.m_renderer->set_xoffset(0);
		st.m_renderer->set_yoffset(0);
		// an identity gamma (the colours below are then the tables' own)
		for (int i = 0; i < 256; i++)
			st.m_gamma_table[i] = u8(i);
		// colour tables: component c (0..31) at luma l (0..63) -> c * 4 + l (red), c * 8 (green), l * 4 (blue)
		for (int c = 0; c < 32; c++)
			for (int l = 0; l < 64; l++)
			{
				xlat[0x0000 / 2 + (c << 8) + l] = u16(std::min(255, c * 4 + l));
				xlat[0x4000 / 2 + (c << 8) + l] = u16(c * 8);
				xlat[0x8000 / 2 + (c << 8) + l] = u16(l * 4);
			}
		// palette (the polygons' colour bases at 0x1000 +): 1 = c(20, 3, 0), 2 = c(2, 25, 0), 3 = c(9, 9, 31)
		pal[0x1000 + 1] = u16(20 | (3 << 5));
		pal[0x1000 + 2] = u16(2 | (25 << 5));
		pal[0x1000 + 3] = u16(9 | (9 << 5) | (31 << 10));
		pal[0x1000 + 4] = u16(31 | (31 << 5) | (31 << 10));
		// the luma table: texel t (0..127) -> t / 2
		for (int i = 0; i < 0x8000; i++)
			luma[i] = u8((i & 0x7f) >> 1);
		// texture sheets: a synthetic pattern
		uint32_t seed = 12345;
		for (u32& w : tex0)
		{
			seed = seed * 1103515245u + 12345u;
			w = seed;
		}
		for (size_t i = 0; i < tex1.size(); i++)
			tex1[i] = u32(i * 0x01010101u) ^ 0xf0f00f0fu;
	}
	~Video() { st.video_exit(); }

	// a polygon in screen space: pz 1 (the projection is then x, 384 - y), z its sort key (smaller: nearer)
	model2_state::polygon& Poly(std::initializer_list<std::pair<float, float>> pts, u16 z, u16 colorbase, u8 luma_v,
		u16 th0 = 0, u16 th2 = 0)
	{
		model2_state::raster_state* r = st.m_raster;
		model2_state::polygon& p = r->poly_list[r->poly_list_index++];
		p = model2_state::polygon();
		int i = 0;
		for (const auto& xy : pts)
		{
			p.v[i].x = xy.first;
			p.v[i].y = float(384) - xy.second;
			p.v[i].pz = 1.0f;
			p.v[i].pu = xy.first * 8.0f;  // texture u, v (model2_3d_render divides them by 8)
			p.v[i].pv = xy.second * 8.0f;
			i++;
		}
		p.num_vertices = u8(i);
		p.z = z;
		p.texheader[0] = th0;
		p.texheader[1] = 0;
		p.texheader[2] = th2;
		p.texheader[3] = u16(colorbase << 6);
		p.luma = luma_v;
		p.viewport[0] = 0;
		p.viewport[1] = 0;
		p.viewport[2] = kW - 1;
		p.viewport[3] = kH - 1;
		p.window = 0;
		p.next = r->poly_sorted_list[z];
		r->poly_sorted_list[z] = &p;
		r->min_z = std::min(r->min_z, z);
		r->max_z = std::max(r->max_z, z);
		return p;
	}

	// the scene (made again before each picture: rendering projects the polygons in place)
	void Scene()
	{
		st.render_frame_start();
		model2_state::raster_state* r = st.m_raster;
		r->cur_window = 0;
		// a red quad far back, a green triangle in front of part of it
		Poly({{20, 20}, {300, 20}, {300, 200}, {20, 200}}, 400, 1, 0x80);
		Poly({{150, 60}, {380, 120}, {200, 260}}, 200, 2, 0x40);
		// a blue checker quad (texheader 0 bit 15)
		Poly({{320, 220}, {470, 220}, {470, 360}, {320, 360}}, 300, 3, 0xfc, 0x8000);
		// a five-sided one, white, partly off the viewport's left
		model2_state::polygon& pent = Poly({{-30, 260}, {60, 230}, {120, 300}, {80, 370}, {-10, 360}}, 500, 4, 0x20);
		(void)pent;
		// textured (bit 14): 64x64 from sheet 0 at (0, 0); and a translucent textured one (bits 14 and 13) from sheet 1
		Poly({{30, 220}, {280, 210}, {300, 370}, {40, 350}}, 600, 1, 0xff, u16(0x4000 | (1 << 0) | (1 << 3)), 0);
		Poly({{100, 30}, {460, 40}, {450, 180}, {120, 190}}, 100, 2, 0xff, u16(0x6000 | (2 << 0) | (2 << 3)),
			u16(0x1000 | 2));
	}

	std::vector<u32> Picture()
	{
		m2_bitmap<u32> bm;
		bm.allocate(kW, kH);
		bm.fill(0xff000000, m2_rectangle(0, kW - 1, 0, kH - 1));
		Scene();
		st.m_render_done = false;
		st.render_polygons(bm, m2_rectangle(0, kW - 1, 0, kH - 1));
		return bm.data;
	}
};

// the ways the core's parts can be run
int g_mode = 0; // 0 in turn; 1 backwards; 2 on threads
void RunParts(BurnParallelJob job, void* ctx, INT32 parts)
{
	if (g_mode == 1)
	{
		for (int i = parts - 1; i >= 0; i--)
			job(ctx, i);
		return;
	}
	std::vector<std::thread> t;
	for (int w = 0; w < 4; w++)
		t.emplace_back([=] {
			for (int i = w; i < parts; i += 4)
				job(ctx, i);
		});
	for (auto& x : t)
		x.join();
}

uint32_t Px(const std::vector<u32>& p, int x, int y)
{
	return p[size_t(y) * kW + x] & 0xffffff;
}

bool WritePpm(const char* path, const std::vector<u32>& p)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	fprintf(f, "P6\n%d %d\n255\n", kW, kH);
	for (u32 c : p)
	{
		const unsigned char rgb[3] = {u8(c >> 16), u8(c >> 8), u8(c)};
		fwrite(rgb, 1, 3, f);
	}
	return fclose(f) == 0;
}
} // namespace

// the golden picture's checksum (tests/test_model2_video --write shows it)
static const uint64_t kGolden = 0xbafaa34a4328051bull;

int main(int argc, char** argv)
{
	Video v;

	// one part (no threads)
	BurnParallelRunHook = NULL;
	nBurnParallelThreads = 1;
	const std::vector<u32> one = v.Picture();

	// 16 bands in turn, backwards, on four threads
	BurnParallelRunHook = RunParts;
	nBurnParallelThreads = 4;
	g_mode = 1;
	const std::vector<u32> back = v.Picture();
	g_mode = 2;
	const std::vector<u32> threads = v.Picture();
	BurnParallelRunHook = NULL;
	nBurnParallelThreads = 1;

	Check(back == one, "16 bands drawn backwards: the same picture as one part");
	Check(threads == one, "16 bands on four threads: the same picture as one part");

	// flat colours: MAME's lookup. Red quad: colour (20, 3, 0), luma 0x80 >> 2 = 32 -> (20*4+32, 3*8, 32*4)
	Check(Px(one, 60, 100) == ((112u << 16) | (24u << 8) | 128u), "the red quad's colour (palette, colour tables, luma)");
	// the green triangle in front of it (luma 0x40 >> 2 = 16): (2*4+16, 25*8, 16*4), where the quad is too
	Check(Px(one, 215, 195) == ((24u << 16) | (200u << 8) | 64u) && Px(one, 40, 195) == ((112u << 16) | (24u << 8) | 128u),
		"the nearer green triangle wins where they overlap");
	// the translucent textured quad, nearest of all, over both: not their colours, not the background
	Check(Px(one, 300, 100) != ((24u << 16) | (200u << 8) | 64u) && Px(one, 300, 100) != 0, "the translucent quad over them");
	// the checker quad: every other pixel
	const uint32_t c0 = Px(one, 400, 300), c1 = Px(one, 401, 300), c2 = Px(one, 402, 300);
	Check(c0 != c1 && c0 == c2 && (c0 == 0 || c1 == 0), "the checker quad: every other pixel");
	// the pentagon: drawn, clipped at x 0 (column 0 has it, the background (black) beyond its edges)
	Check(Px(one, 2, 340) != 0 && Px(one, 50, 290) != 0 && Px(one, 150, 380) == 0, "the five-sided polygon, clipped at the left");
	// textured: more than a few colours in it, and none black inside (its texels go through the luma table)
	{
		std::vector<uint32_t> seen;
		bool holes = false;
		for (int y = 260; y < 330; y++)
			for (int x = 140; x < 240; x++)
			{
				const uint32_t c = Px(one, x, y);
				holes = holes || c == 0;
				if (std::find(seen.begin(), seen.end(), c) == seen.end())
					seen.push_back(c);
			}
		Check(seen.size() >= 8, "the textured quad: " + std::to_string(seen.size()) + " colours");
		Check(!holes, "the textured quad: no holes");
	}
	const uint64_t h = Fnv(one);
	char hs[32];
	snprintf(hs, sizeof(hs), "%016llx", (unsigned long long)h);
	if (argc > 2 && std::string(argv[1]) == "--write")
	{
		Check(WritePpm(argv[2], one), std::string("writing ") + argv[2]);
		printf("  picture checksum %s\n", hs);
	}
	else
		Check(h == kGolden, std::string("the golden picture (checksum ") + hs + ")");

	// the geometrizer's commands kept within its memories (each stream from the buffer RAM's start; ASan would stop a
	// read or write past the buffer RAM, the polygon RAMs or the texture parameters)
	{
		model2_state::geo_state* geo = v.st.m_geo;
		auto run = [&](std::initializer_list<u32> words, u32 fill) {
			std::fill(v.buffer.begin(), v.buffer.begin() + 0x20000 / 4, fill);
			size_t i = 0;
			for (u32 w : words)
				v.buffer[i++] = w;
			v.st.m_geo_read_start_address = 0;
			v.st.geo_parse();
		};
		std::fill(std::begin(geo->polygon_ram1), std::end(geo->polygon_ram1), 0x5a5a5a5au);
		// polygon data (5) into the slow RAM's last 16 words, 0xffffffff of them
		run({5u << 23, 0x00007ff0, 0xffffffffu}, 0x11111111u);
		bool ram1 = true, guard = true;
		for (u32 w : geo->polygon_ram1)
			ram1 = ram1 && w == 0x5a5a5a5au;
		for (u32 w : geo->polygon_ram_guard)
			guard = guard && w == 0;
		Check(geo->polygon_ram0[0x7fff] == 0x11111111u && ram1 && guard,
			"geometrizer: a polygon RAM copy of 2^32 words stops at the RAM's end");
		// texture parameters (6) at index 0x3fffffff: one of the 32
		run({6u << 23, 0xfffffffcu, 1, 0x00000042u, 0x3f800000u, 0x0f << 23}, 0);
		Check(geo->texture_parameters[31].diffuse == float(0x42), "geometrizer: a texture parameter index of 2^30 is one of the 32");
		// the commands below are called on their own (input: the word after the opcode), the rasterizer's command reset
		model2_state::raster_state* raster = v.st.m_raster;
		const u32* const end = v.buffer.data() + 0x20000 / 4;
		auto call = [&](const u32* (model2_state::*cmd)(model2_state::geo_state&, u32, const u32*),
			std::initializer_list<u32> words, u32 fill) {
			std::fill(v.buffer.begin(), v.buffer.begin() + 0x20000 / 4, fill);
			size_t i = 0;
			for (u32 w : words)
				v.buffer[i++] = w;
			raster->cur_command = 0;
			raster->command_index = 0;
			return (v.st.*cmd)(*geo, v.buffer[0], v.buffer.data() + 1);
		};
		// direct data (2) whose quads never end (attribute 1), to the end of the buffer RAM: whole quads only (an
		// attribute and 8 words), then the list ends -- the rasterizer gets its closing 0 as the next attribute
		{
			const u32* r = call(&model2_state::geo_direct_data, {2u << 23}, 0x00000001u);
			size_t p = 9, e = 0x20000 / 4;
			while (p < e) { p++; if (e - p < 8) break; p += 8; }
			Check(r == v.buffer.data() + p && r <= end && raster->cur_command == 0,
				"geometrizer: direct data that runs to the end of the buffer RAM stops there, after its last whole quad");
		}
		// texture data (4) of 2^32 words to the texture RAM: the words left in the buffer RAM (0x7ffd) are written
		{
			std::fill(std::begin(raster->texture_ram), std::end(raster->texture_ram), u16(0));
			const u32* r = call(&model2_state::geo_texture_data, {4u << 23, 0x00800000u, 0xffffffffu}, 0x1234u);
			Check(r == end && raster->texture_ram[0x7ffc] == 0x1234 && raster->texture_ram[0x7ffd] == 0 &&
				raster->cur_command == 0, "geometrizer: texture data of 2^32 words stops at the end of the buffer RAM");
		}
		// object data (1), 0xfffff objects whose quads never end (attribute 1, 10 words each with the normal) from the
		// last 8 words of: the slow RAM (on into the fast RAM, as in MAME), the fast RAM, the polygon ROM -- each stops
		// at its end (its last link's words past it read from the 16 words kept after it, here 1s too: the list is not
		// ended by their zeros; under ASan, nothing is read past them), and the rasterizer gets its closing 0 after a
		// whole quad
		geo->mode = 0;
		std::fill(std::begin(geo->polygon_ram0), std::end(geo->polygon_ram0), 0x00000001u);
		std::fill(std::begin(geo->polygon_ram1), std::end(geo->polygon_ram1), 0x00000001u);
		std::fill(std::begin(geo->polygon_ram_guard), std::end(geo->polygon_ram_guard), 0x00000001u);
		std::fill(v.poly_rom.begin(), v.poly_rom.end(), 0x00000001u);
		const struct { u32 oba; const char* what; } lists[] = {
			{ 0x00007ff8u, "the slow RAM (on into the fast RAM)" },
			{ 0x01007ff8u, "the fast RAM" },
			{ 0x00800ff8u, "the polygon ROM" },
		};
		for (const auto& l : lists)
		{
			const u32* r = call(&model2_state::geo_object_data, {1u << 23, 0, 0, l.oba, 0x000fffffu}, 0);
			Check(r == v.buffer.data() + 5 && raster->cur_command == 0,
				std::string("geometrizer: an object list that never ends, from the end of ") + l.what + ", stops at its end");
		}
		std::fill(std::begin(geo->polygon_ram1), std::end(geo->polygon_ram1), 0u);
		std::fill(std::begin(geo->polygon_ram_guard), std::end(geo->polygon_ram_guard), 0u);
		std::fill(v.poly_rom.begin(), v.poly_rom.end(), 0u);
		raster->cur_command = 0;
		raster->command_index = 0;
		v.st.render_frame_start();
	}

	printf("model2 video test: %d checks ok, %d failed\n", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
