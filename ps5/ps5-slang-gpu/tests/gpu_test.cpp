// ps5-slang-gpu's host test: the renderer off screen on the host's Vulkan driver (lavapipe), driven as a program drives
// it, the frames read back and checked. tests/run-tests.sh runs each mode in a process of its own.
//
//   gpu-test unit                         SHA-256, Fit, the menu's names without a GPU
//   gpu-test presets                      every preset of the menu compiled and drawing a test picture
//   gpu-test formats                      the four pixel formats, a pitch, the crop, the four rotations, the UI surface
//   gpu-test ui-straight                  a BGRA8 UI surface with straight alpha over the picture
//   gpu-test damaged <preset>             which presets are damaged at start, and <preset> after it is picked
//   gpu-test init                         Init's result (run with no Vulkan driver: the CPU fall-back)
//   gpu-test guard <marker>               the start guard over three starts
//   gpu-test params <file> set|get        a parameter set and saved, then read back by another process
//   gpu-test video                        1.1: video pictures: every layout and bit depth, the matrices and ranges, HDR
//                                         tone mapping against Media Player Classic PS5's CPU arithmetic, the UI surface
//                                         over them, place and zoom, the filters, refusals, the ring, in-place pictures
//
// GPU_TEST_SHIPPED names the shipped presets' folder (Config::shipped_dir). Each check prints "ok <what>" or "FAIL <what>"; the exit status is the number of failures. The log goes to stderr.
//
// SPDX-License-Identifier: MIT

#include "ps5gpu/gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_fail = 0;
void Check(bool ok, const std::string& what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	fflush(stdout);
	if (!ok)
		g_fail++;
}

std::vector<uint8_t> g_frame; // the last frame read back, B8G8R8A8
int g_w = 0, g_h = 0;
void HostPresent(const uint8_t* bgra, int w, int h, bool)
{
	g_frame.assign(bgra, bgra + size_t(w) * h * 4);
	g_w = w;
	g_h = h;
}

void LogLine(const char* line)
{
	fprintf(stderr, "%s\n", line);
}

struct Rgb
{
	int r, g, b;
};
Rgb At(int x, int y)
{
	const uint8_t* p = g_frame.data() + (size_t(y) * g_w + x) * 4;
	return {p[2], p[1], p[0]};
}
bool Near(Rgb a, Rgb b, int tol = 24)
{
	return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}
std::string Str(Rgb c)
{
	char b[48];
	snprintf(b, sizeof(b), "%d,%d,%d", c.r, c.g, c.b);
	return b;
}

ps5gpu::Config BaseConfig()
{
	ps5gpu::Config c;
	c.log = LogLine;
	c.app_name = "ps5-slang-gpu test";
	c.host_present = HostPresent;
	c.sync_build = true;
	if (const char* d = getenv("GPU_TEST_SHIPPED"))
		c.shipped_dir = d;
	return c;
}

// a test picture with something for every shader to work on: colour bars, a grey ramp, a checker, fine lines
std::vector<uint32_t> TestPicture(int w, int h)
{
	static const uint32_t bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000};
	std::vector<uint32_t> px(size_t(w) * h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			uint32_t c;
			if (y < h / 2)
				c = bars[x * 8 / w];
			else if (y < h * 3 / 4)
			{
				const uint32_t v = uint32_t(x * 255 / (w - 1));
				c = v << 16 | v << 8 | v;
			}
			else
				c = ((x / 8 + y / 8) & 1) ? 0xe0e0e0 : ((x & 1) ? 0x202080 : 0x802020);
			px[size_t(y) * w + x] = 0xff000000u | c;
		}
	return px;
}

// the readback's rectangle: something drawn (bright enough, more than a few colours)
bool Draws(ps5gpu::Rect r, std::string* why)
{
	int maxc = 0;
	std::vector<uint32_t> seen;
	for (int y = r.y; y < r.y + r.h; y += 3)
		for (int x = r.x; x < r.x + r.w; x += 3)
		{
			const Rgb c = At(x, y);
			maxc = std::max(maxc, std::max(c.r, std::max(c.g, c.b)));
			const uint32_t k = uint32_t(c.r >> 4) << 8 | uint32_t(c.g >> 4) << 4 | uint32_t(c.b >> 4);
			if (seen.size() < 64 && std::find(seen.begin(), seen.end(), k) == seen.end())
				seen.push_back(k);
		}
	char b[64];
	snprintf(b, sizeof(b), "brightest %d, %zu colours", maxc, seen.size());
	*why = b;
	return maxc > 60 && seen.size() >= 6;
}

int Unit()
{
	const std::string abc = "abc";
	Check(ps5slang::Sha256Hex(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()) ==
			  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"SHA-256 of \"abc\" (FIPS 180-4's example)");
	Check(ps5slang::Sha256Hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		"SHA-256 of nothing");
	std::string m(1000000, 'a');
	Check(ps5slang::Sha256Hex(reinterpret_cast<const uint8_t*>(m.data()), m.size()) ==
			  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
		"SHA-256 of a million 'a' (FIPS 180-4's example)");
	const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	Check(ps5slang::Sha256Hex(reinterpret_cast<const uint8_t*>(two.data()), two.size()) ==
			  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
		"SHA-256 of the two-block example");
	auto same = [](ps5gpu::Rect a, ps5gpu::Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
	const ps5gpu::Rect screen = {0, 0, 1920, 1080};
	Check(same(ps5gpu::Fit(320, 224, 4.0 / 3.0, screen, false), {240, 0, 1440, 1080}), "Fit: 4:3 on 16:9, as large as fits");
	Check(same(ps5gpu::Fit(320, 224, 4.0 / 3.0, screen, true), {362, 92, 1195, 896}), "Fit: whole multiple (4x the height)");
	Check(same(ps5gpu::Fit(256, 224, 0, screen, false), {343, 0, 1234, 1080}), "Fit: square pixels");
	const ps5gpu::Rect t = ps5gpu::Fit(224, 288, 3.0 / 4.0, screen, false);
	Check(same(t, {555, 0, 810, 1080}), "Fit: a turned arcade picture at 3:4 (" + std::to_string(t.x) + "," +
											std::to_string(t.w) + ")");
	Check(same(ps5gpu::Fit(320, 224, 4.0 / 3.0, {100, 50, 0, 10}, false), {100, 50, 0, 10}), "Fit: an empty area");
	Check(!ps5gpu::Active() && ps5gpu::ShaderCount() >= 1, "no GPU yet: not active");
	Check(std::string(ps5gpu::kVersion).size() > 0, std::string("the version: ") + ps5gpu::kVersion);
	return g_fail;
}

bool StartOffscreen(int ui_w, int ui_h, ps5gpu::Config c)
{
	ps5gpu::Configure(c);
	const bool ok = ps5gpu::Init(ui_w, ui_h, true, ui_w, ui_h);
	Check(ok, std::string("Init off screen on ") + (ok ? ps5gpu::DeviceName() : "no driver"));
	return ok;
}

// frames of the picture with `shader` until it is built (sync_build) and drawn twice
void Show(const ps5gpu::Frame& f, ps5gpu::Rect dst, int shader, const std::vector<uint32_t>& ui, bool smooth = false)
{
	for (int i = 0; i < 4; i++)
	{
		ps5gpu::SetFrame(f, dst, shader, smooth, false);
		ps5gpu::Present(ui.data(), ps5gpu::Rect{0, 0, 0, 0}, false);
	}
}

int Presets()
{
	const int W = 960, H = 540;
	if (!StartOffscreen(W, H, BaseConfig()))
		return g_fail;
	const std::vector<uint32_t> ui(size_t(W) * H, 0);
	const std::vector<uint32_t> pic = TestPicture(320, 224);
	ps5gpu::Frame f;
	f.pixels = pic.data();
	f.width = 320;
	f.height = 224;
	const ps5gpu::Rect dst = ps5gpu::Fit(320, 224, 4.0 / 3.0, {0, 0, W, H}, false);
	printf("menu: %d shaders\n", ps5gpu::ShaderCount());
	for (int i = 0; i < ps5gpu::ShaderCount(); i++)
	{
		Show(f, dst, i, ui);
		std::string why;
		const bool full = ps5gpu::ShaderFullscreen(i);
		const bool drew = Draws(full ? ps5gpu::Rect{0, 0, W, H} : dst, &why);
		Check(drew && !ps5gpu::ShaderDamaged(i) && ps5gpu::FindShader(ps5gpu::ShaderPresetName(i)) == i,
			std::string("shader ") + std::to_string(i) + " " + ps5gpu::ShaderName(i) + ": " +
				std::to_string(ps5gpu::PassCount(i)) + " pass(es), draws (" + why + ")" + (full ? ", fullscreen" : ""));
	}
	// a turned picture through a fullscreen preset (given to it turned already)
	const int potato = ps5gpu::FindShader("Mega Bezel (Potato)");
	const std::vector<uint32_t> tall = TestPicture(224, 288);
	ps5gpu::Frame t;
	t.pixels = tall.data();
	t.width = 224;
	t.height = 288;
	t.rotation = ps5gpu::Rotation::Rotate270;
	Show(t, ps5gpu::Fit(288, 224, 4.0 / 3.0, {0, 0, W, H}, false), potato, ui);
	std::string why;
	const bool drew = potato > 0 && Draws({0, 0, W, H}, &why);
	Check(drew, "Mega Bezel (Potato) with a turned picture draws (" + why + ")");
	ps5gpu::Shutdown();
	return g_fail;
}

// the 4x4 picture: top-left red, top-right green, bottom-left blue, bottom-right white (2x2 each)
const Rgb kRed = {255, 0, 0}, kGreen = {0, 255, 0}, kBlue = {0, 0, 255}, kWhite = {255, 255, 255};
std::vector<uint8_t> Quads(ps5gpu::PixelFormat fmt, int pad, int border, int& w, int& h, int& pitch)
{
	const int bpp = fmt == ps5gpu::PixelFormat::RGB565 || fmt == ps5gpu::PixelFormat::XRGB1555 ? 2 : 4;
	w = 4 + 2 * border;
	h = 4 + 2 * border;
	pitch = w * bpp + pad;
	std::vector<uint8_t> px(size_t(pitch) * h, 0x5a);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			Rgb c = {255, 0, 255}; // the border: magenta
			const int ix = x - border, iy = y - border;
			if (ix >= 0 && ix < 4 && iy >= 0 && iy < 4)
				c = iy < 2 ? (ix < 2 ? kRed : kGreen) : (ix < 2 ? kBlue : kWhite);
			uint8_t* p = px.data() + size_t(y) * pitch + size_t(x) * bpp;
			uint32_t v = 0;
			switch (fmt)
			{
				case ps5gpu::PixelFormat::XRGB8888: v = 0xff000000u | uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | uint32_t(c.b); break;
				case ps5gpu::PixelFormat::XBGR8888: v = 0xff000000u | uint32_t(c.b) << 16 | uint32_t(c.g) << 8 | uint32_t(c.r); break;
				case ps5gpu::PixelFormat::RGB565: v = uint32_t(c.r >> 3) << 11 | uint32_t(c.g >> 2) << 5 | uint32_t(c.b >> 3); break;
				case ps5gpu::PixelFormat::XRGB1555: v = uint32_t(c.r >> 3) << 10 | uint32_t(c.g >> 3) << 5 | uint32_t(c.b >> 3); break;
			}
			memcpy(p, &v, size_t(bpp));
		}
	return px;
}

void CheckQuads(const char* what, Rgb tl, Rgb tr, Rgb bl, Rgb br)
{
	// the picture in (0,0,400,400): each quadrant 200x200
	const Rgb a = At(100, 100), b = At(300, 100), c = At(100, 300), d = At(300, 300);
	Check(Near(a, tl) && Near(b, tr) && Near(c, bl) && Near(d, br),
		std::string(what) + ": " + Str(a) + " | " + Str(b) + " / " + Str(c) + " | " + Str(d));
}

int Formats()
{
	const int W = 400, H = 400;
	if (!StartOffscreen(W, H, BaseConfig()))
		return g_fail;
	std::vector<uint32_t> ui(size_t(W) * H, 0);
	const ps5gpu::Rect dst = {0, 0, 400, 400};
	struct
	{
		ps5gpu::PixelFormat f;
		const char* name;
	} fmts[] = {{ps5gpu::PixelFormat::XRGB8888, "XRGB8888"}, {ps5gpu::PixelFormat::XBGR8888, "XBGR8888"},
		{ps5gpu::PixelFormat::RGB565, "RGB565"}, {ps5gpu::PixelFormat::XRGB1555, "XRGB1555"}};
	for (const auto& fm : fmts)
	{
		int w, h, pitch;
		const std::vector<uint8_t> px = Quads(fm.f, 12, 0, w, h, pitch);
		ps5gpu::Frame f;
		f.pixels = px.data();
		f.width = w;
		f.height = h;
		f.pitch = pitch;
		f.format = fm.f;
		Show(f, dst, 0, ui);
		CheckQuads((std::string(fm.name) + ", a pitch with 12 bytes more").c_str(), kRed, kGreen, kBlue, kWhite);
	}
	// the crop: a magenta border of 1 pixel around the picture, cropped away
	{
		int w, h, pitch;
		const std::vector<uint8_t> px = Quads(ps5gpu::PixelFormat::RGB565, 0, 1, w, h, pitch);
		ps5gpu::Frame f;
		f.pixels = px.data();
		f.width = w;
		f.height = h;
		f.format = ps5gpu::PixelFormat::RGB565;
		f.crop.left = f.crop.top = f.crop.right = f.crop.bottom = 1;
		Show(f, dst, 0, ui);
		CheckQuads("the crop (1 pixel off each edge)", kRed, kGreen, kBlue, kWhite);
		const Rgb edge = At(2, 2);
		Check(!Near(edge, {255, 0, 255}, 60), "... no border left at the corner (" + Str(edge) + ")");
	}
	// (final review 2) a game frame whose submit fails: uploaded again by the next Present (not the one before, not garbage)
	{
		int w, h, pitch;
		const std::vector<uint8_t> px = Quads(ps5gpu::PixelFormat::XRGB8888, 0, 0, w, h, pitch);
		std::vector<uint8_t> turned(px.size());
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				memcpy(&turned[size_t(h - 1 - y) * pitch + size_t(w - 1 - x) * 4], &px[size_t(y) * pitch + size_t(x) * 4], 4);
		ps5gpu::Frame f;
		f.pixels = px.data();
		f.width = w;
		f.height = h;
		f.pitch = pitch;
		Show(f, dst, 0, ui);
		f.pixels = turned.data();
		ps5gpu::SetFrame(f, dst, 0, false, false);
		setenv("PS5GPU_TEST_SUBMIT_FAIL", "1", 1);
		const bool failed = !ps5gpu::Present(ui.data(), ps5gpu::Rect{0, 0, 0, 0}, false);
		unsetenv("PS5GPU_TEST_SUBMIT_FAIL");
		ps5gpu::Present(ui.data(), ps5gpu::Rect{0, 0, 0, 0}, false); // no new frame from the game
		Check(failed, "a game frame's submit failed (injected)");
		CheckQuads("... the next Present uploads it again", kWhite, kBlue, kGreen, kRed);
	}
	// the rotations, counter-clockwise, by the last pass (the plain picture)
	{
		int w, h, pitch;
		const std::vector<uint8_t> px = Quads(ps5gpu::PixelFormat::XRGB8888, 0, 0, w, h, pitch);
		ps5gpu::Frame f;
		f.pixels = px.data();
		f.width = w;
		f.height = h;
		f.rotation = ps5gpu::Rotation::Rotate90;
		Show(f, dst, 0, ui);
		CheckQuads("Rotate90 (counter-clockwise)", kGreen, kWhite, kRed, kBlue);
		f.rotation = ps5gpu::Rotation::Rotate180;
		Show(f, dst, 0, ui);
		CheckQuads("Rotate180", kWhite, kBlue, kGreen, kRed);
		f.rotation = ps5gpu::Rotation::Rotate270;
		Show(f, dst, 0, ui);
		CheckQuads("Rotate270", kBlue, kRed, kWhite, kGreen);
		// a turned picture of another shape: 8x4 (red left half, blue right) turned 90 into a tall rectangle
		std::vector<uint32_t> wide(32);
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 8; x++)
				wide[size_t(y) * 8 + x] = x < 4 ? 0xffff0000u : 0xff0000ffu;
		ps5gpu::Frame g;
		g.pixels = wide.data();
		g.width = 8;
		g.height = 4;
		g.rotation = ps5gpu::Rotation::Rotate90;
		Show(g, {100, 0, 200, 400}, 0, ui);
		const Rgb top = At(200, 100), bottom = At(200, 300), out = At(50, 200);
		Check(Near(top, kBlue) && Near(bottom, kRed) && Near(out, {0, 0, 0}),
			"Rotate90 of a wide picture into a tall rectangle: top " + Str(top) + ", bottom " + Str(bottom) + ", outside " +
				Str(out));
	}
	// a CRT preset with a turned picture still draws (its passes in the picture's orientation)
	{
		const std::vector<uint32_t> pic = TestPicture(224, 288);
		ps5gpu::Frame f;
		f.pixels = pic.data();
		f.width = 224;
		f.height = 288;
		f.rotation = ps5gpu::Rotation::Rotate90;
		const int lottes = ps5gpu::FindShader("crt-lottes");
		Show(f, {50, 100, 300, 200}, lottes, ui);
		std::string why;
		const bool drew = lottes > 0 && Draws({50, 100, 300, 200}, &why);
		Check(drew, "crt-lottes on a turned picture draws (" + why + ")");
	}
	// the UI surface (RGBA8, premultiplied) over the picture: opaque where it is, the picture where it is transparent
	{
		int w, h, pitch;
		const std::vector<uint8_t> px = Quads(ps5gpu::PixelFormat::XRGB8888, 0, 0, w, h, pitch);
		ps5gpu::Frame f;
		f.pixels = px.data();
		f.width = w;
		f.height = h;
		for (int y = 50; y < 150; y++)
			for (int x = 50; x < 150; x++)
				ui[size_t(y) * W + x] = 0xff00ffffu; // R 255, G 255, B 0, A 255
		for (int y = 250; y < 350; y++)
			for (int x = 250; x < 350; x++)
				ui[size_t(y) * W + x] = 0x80000000u; // black at half (premultiplied): the white below at half
		for (int i = 0; i < 2; i++)
		{
			ps5gpu::SetFrame(f, dst, 0, false, false);
			ps5gpu::Present(ui.data(), ps5gpu::Rect{0, 0, W, H}, false);
		}
		const Rgb a = At(100, 100), b = At(300, 300), c = At(300, 100);
		Check(Near(a, {255, 255, 0}) && Near(b, {127, 127, 127}) && Near(c, kGreen),
			"the UI surface over the picture: opaque " + Str(a) + ", half " + Str(b) + ", none " + Str(c));
		// only the dirty rectangle is copied: a change outside it isn't seen
		for (int y = 0; y < 40; y++)
			for (int x = 0; x < 40; x++)
				ui[size_t(y) * W + x] = 0xffffffffu;
		ps5gpu::SetFrame(f, dst, 0, false, false);
		ps5gpu::Present(ui.data(), ps5gpu::Rect{200, 200, 10, 10}, false);
		Check(Near(At(20, 20), kRed), "a change outside the dirty rectangle waits (" + Str(At(20, 20)) + ")");
		ps5gpu::HideGame();
		for (int i = 0; i < 31; i++)
			ps5gpu::Present(ui.data(), ps5gpu::Rect{0, 0, W, H}, false);
		Check(Near(At(300, 100), {0, 0, 0}) && Near(At(20, 20), kWhite), "HideGame: the picture gone, the surface stays");
	}
	// a frame larger than the first staging buffer (2048x1024): it grows
	{
		std::vector<uint32_t> big(size_t(2400) * 1200, 0xff00ff00u);
		ps5gpu::Frame f;
		f.pixels = big.data();
		f.width = 2400;
		f.height = 1200;
		std::fill(ui.begin(), ui.end(), 0);
		Show(f, dst, 0, ui);
		Check(Near(At(200, 200), kGreen), "a 2400x1200 frame (the staging buffer grows): " + Str(At(200, 200)));
	}
	ps5gpu::Shutdown();
	return g_fail;
}

int UiStraight()
{
	const int W = 200, H = 200;
	ps5gpu::Config c = BaseConfig();
	c.ui_format = ps5gpu::UiFormat::BGRA8;
	c.ui_premultiplied = false;
	if (!StartOffscreen(W, H, c))
		return g_fail;
	std::vector<uint32_t> ui(size_t(W) * H, 0x00ffffffu); // white at alpha 0: nothing
	for (int y = 0; y < 100; y++)
		for (int x = 0; x < 100; x++)
			ui[size_t(y) * W + x] = 0x80ff0000u; // BGRA8: A 0x80, R 255 (straight)
	std::vector<uint32_t> blue(16, 0xff0000ffu);
	ps5gpu::Frame f;
	f.pixels = blue.data();
	f.width = 4;
	f.height = 4;
	Show(f, {0, 0, W, H}, 0, ui);
	const Rgb a = At(50, 50), b = At(150, 150);
	Check(Near(a, {128, 0, 127}, 6) && Near(b, kBlue), "a BGRA8 surface, straight alpha: half red over blue " + Str(a) +
														   ", transparent white " + Str(b));
	ps5gpu::Shutdown();
	return g_fail;
}

int Damaged(const char* preset)
{
	const int W = 320, H = 240;
	if (!StartOffscreen(W, H, BaseConfig()))
		return g_fail;
	std::string list;
	for (int i = 1; i < ps5gpu::ShaderCount(); i++)
		if (ps5gpu::ShaderDamaged(i))
			list += std::string(list.empty() ? "" : ",") + ps5gpu::ShaderPresetName(i);
	printf("damaged at start: %s\n", list.empty() ? "none" : list.c_str());
	const int i = ps5gpu::FindShader(preset);
	if (i > 0)
	{
		std::vector<uint32_t> ui(size_t(W) * H, 0), red(64, 0xffff0000u);
		ps5gpu::Frame f;
		f.pixels = red.data();
		f.width = 8;
		f.height = 8;
		Show(f, {0, 0, W, H}, i, ui);
		const Rgb c = At(W / 2, H / 2);
		printf("%s after it was picked: %s, menu \"%s\", centre %s\n", preset, ps5gpu::ShaderDamaged(i) ? "damaged" : "ready",
			ps5gpu::ShaderName(i), Str(c).c_str());
		printf("plain picture: %s\n", Near(c, kRed, 4) ? "yes" : "no");
	}
	ps5gpu::Shutdown();
	return g_fail;
}

int InitOnly()
{
	ps5gpu::Configure(BaseConfig());
	const bool ok = ps5gpu::Init(64, 64, true, 64, 64);
	printf("init: %s\n", ok ? "gpu" : "cpu");
	ps5gpu::Shutdown();
	return 0;
}

int Guard(const char* marker)
{
	ps5gpu::Configure(BaseConfig());
	const bool first = ps5gpu::GuardBegin(marker); // a start that dies here leaves it
	const bool second = ps5gpu::GuardBegin(marker); // the next start: the CPU, the marker removed
	FILE* f = fopen(marker, "r");
	const bool gone = !f;
	if (f)
		fclose(f);
	const bool third = ps5gpu::GuardBegin(marker); // the one after it tries the GPU again
	ps5gpu::GuardEnd();                            // the first frame on screen
	f = fopen(marker, "r");
	const bool ended = !f;
	if (f)
		fclose(f);
	Check(first && !second && gone && third && ended,
		"the start guard: GPU, then CPU after a start that died (marker removed), then GPU again, removed at GuardEnd");
	return g_fail;
}

int Params(const char* file, const char* what)
{
	ps5gpu::Config c = BaseConfig();
	c.params_file = file;
	if (!StartOffscreen(64, 64, c))
		return g_fail;
	const int i = ps5gpu::FindShader("crt-lottes");
	if (!strcmp(what, "set"))
	{
		ps5gpu::ParamInfo p = {};
		Check(i > 0 && ps5gpu::ShaderParamCount(i) > 0 && ps5gpu::ShaderParam(i, 0, &p), "crt-lottes has parameters");
		ps5gpu::SetShaderParam(i, p.name, p.max);
		Check(ps5gpu::SaveShaderParams(), std::string("saved: ") + p.name + " = " + std::to_string(p.max));
	}
	else
	{
		ps5gpu::ParamInfo p = {};
		const bool got = i > 0 && ps5gpu::ShaderParam(i, 0, &p);
		Check(got && p.value == p.max && p.value != p.def,
			std::string("read back by another start: ") + (got ? p.name : "?") + " = " + std::to_string(p.value));
		ps5gpu::ResetShaderParams(i);
		Check(ps5gpu::ShaderParam(i, 0, &p) && p.value == p.def && ps5gpu::SaveShaderParams(),
			"reset to the preset's own value and saved (the file goes when nothing is set)");
	}
	ps5gpu::Shutdown();
	return g_fail;
}
} // namespace

// ---- 1.1: video pictures (gpu-test video) -------------------------------------------------------------------------------
namespace vt
{
constexpr int kOutW = 256, kOutH = 128; // the UI surface and the output
constexpr int kPicW = 64, kPicH = 32;   // the picture: four bars, 16 pixels wide
std::vector<uint32_t> g_ui(size_t(kOutW) * kOutH, 0);
int g_released = 0;
void Released(void*) { g_released++; }

// a bar's Y, Cb, Cr codes at `bits`
struct Ycc
{
	int y, cb, cr;
};
const Ycc kBars8[4] = {{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {170, 166, 16}}; // red, green, blue, yellow-ish

double Kr(ps5gpu::ColorMatrix m)
{
	switch (m)
	{
		case ps5gpu::ColorMatrix::BT601: return 0.299;
		case ps5gpu::ColorMatrix::BT2020: return 0.2627;
		case ps5gpu::ColorMatrix::FCC: return 0.30;
		case ps5gpu::ColorMatrix::SMPTE240M: return 0.212;
		default: return 0.2126;
	}
}
double Kb(ps5gpu::ColorMatrix m)
{
	switch (m)
	{
		case ps5gpu::ColorMatrix::BT601: return 0.114;
		case ps5gpu::ColorMatrix::BT2020: return 0.0593;
		case ps5gpu::ColorMatrix::FCC: return 0.11;
		case ps5gpu::ColorMatrix::SMPTE240M: return 0.087;
		default: return 0.0722;
	}
}

// the reference: R'G'B' 0..1 of codes, as BT.601/709/2020 define them
void Reference(int y, int cb, int cr, int bits, bool full, ps5gpu::ColorMatrix m, double* rgb)
{
	const double sh = double(1 << (bits - 8));
	double Y, Cb, Cr;
	if (full)
	{
		const double mx = double((1 << bits) - 1);
		Y = y / mx;
		Cb = (cb - double(1 << (bits - 1))) / mx;
		Cr = (cr - double(1 << (bits - 1))) / mx;
	}
	else
	{
		Y = (y - 16 * sh) / (219 * sh);
		Cb = (cb - 128 * sh) / (224 * sh);
		Cr = (cr - 128 * sh) / (224 * sh);
	}
	const double kr = Kr(m), kb = Kb(m), kg = 1 - kr - kb;
	rgb[0] = Y + 2 * (1 - kr) * Cr;
	rgb[1] = Y - 2 * kb * (1 - kb) / kg * Cb - 2 * kr * (1 - kr) / kg * Cr;
	rgb[2] = Y + 2 * (1 - kb) * Cb;
	for (int i = 0; i < 3; i++)
		rgb[i] = std::max(0.0, std::min(1.0, rgb[i]));
}

// Media Player Classic PS5's tone mapping (mpc_tonemap.cpp), in doubles: R'G'B' (PQ or HLG, BT.2020) -> SDR R'G'B'
double PqEotf(double e)
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double p = std::pow(std::max(e, 0.0), 1.0 / m2);
	return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
double PqInv(double nits)
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
	return std::pow((c1 + c2 * y) / (1 + c3 * y), m2);
}
double HableC(double x)
{
	const double A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
	return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}
double Curve(int mode, double L, double peak, double white)
{
	if (L <= 0)
		return 0;
	if (peak <= white)
		return L;
	if (mode == 2)
		return white * HableC(L / white * 2.0) / HableC(peak / white * 2.0);
	if (mode == 3)
	{
		const double x = L / white, lw2 = (peak / white) * (peak / white);
		return white * x * (1 + x / lw2) / (1 + x);
	}
	const double src = PqInv(peak), e1 = PqInv(L) / src, max_lum = PqInv(white) / src, ks = 1.5 * max_lum - 0.5;
	double e2 = e1;
	if (e1 > ks && ks < 1)
	{
		const double t = (e1 - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
		e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * max_lum;
	}
	return PqEotf(std::min(e2, 1.0) * src);
}
void ToneMapRef(double* rgb, bool hlg, int mode, double peak, double white)
{
	double lin[3];
	for (int i = 0; i < 3; i++)
	{
		const double e = rgb[i];
		lin[i] = hlg ? (e <= 0.5 ? e * e / 3.0 : (std::exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0) : PqEotf(e);
	}
	if (hlg)
	{
		const double ys = 0.2627 * lin[0] + 0.6780 * lin[1] + 0.0593 * lin[2];
		const double k = 1000.0 * std::pow(std::min(std::max(ys, 0.0), 1.0), 0.2);
		for (double& v : lin)
			v *= k;
	}
	const double m[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};
	double c[3];
	for (int i = 0; i < 3; i++)
		c[i] = std::max(0.0, m[i * 3] * lin[0] + m[i * 3 + 1] * lin[1] + m[i * 3 + 2] * lin[2]);
	const double top = hlg ? 1000.0 : std::max(peak, white);
	const double mx = std::min(std::max(c[0], std::max(c[1], c[2])), top);
	const double gain = mx > 0 ? Curve(mode, mx, top, white) / mx / white : 1.0 / white;
	for (int i = 0; i < 3; i++)
		rgb[i] = std::pow(std::min(1.0, c[i] * gain), 1.0 / 2.4);
}

// A picture of four bars in a layout, its planes in `store`
struct Pic
{
	ps5gpu::VideoFrame f;
	std::vector<uint8_t> store[3];
	Ycc bars[4];
};
void Fill(Pic& p, ps5gpu::VideoLayout layout, int bits, bool msb, int sx, int sy, const Ycc* bars8, int pad = 0)
{
	p.f = ps5gpu::VideoFrame();
	p.f.width = kPicW;
	p.f.height = kPicH;
	p.f.layout = layout;
	p.f.bits = bits;
	p.f.msb = msb;
	p.f.chroma_shift_x = sx;
	p.f.chroma_shift_y = sy;
	const int bps = bits > 8 ? 2 : 1;
	for (int i = 0; i < 4; i++)
	{
		const int s = bits - 8;
		p.bars[i] = {bars8[i].y << s, bars8[i].cb << s, bars8[i].cr << s};
	}
	auto put = [&](std::vector<uint8_t>& v, size_t at, int code) {
		if (bps == 1)
			v[at] = uint8_t(code);
		else
		{
			const int w = msb ? code << (16 - bits) : code;
			v[at] = uint8_t(w & 0xff);
			v[at + 1] = uint8_t(w >> 8);
		}
	};
	const int cw = (kPicW + (1 << sx) - 1) >> sx, ch = (kPicH + (1 << sy) - 1) >> sy;
	if (layout == ps5gpu::VideoLayout::Packed)
	{
		const int pitch = kPicW * 4 + pad;
		p.store[0].assign(size_t(pitch) * kPicH, 0);
		for (int y = 0; y < kPicH; y++)
			for (int x = 0; x < kPicW; x++)
			{
				const Ycc& b = bars8[x / 16]; // here: R, G, B codes
				uint8_t* px = &p.store[0][size_t(y) * pitch + x * 4];
				px[p.f.rgb_index[0]] = uint8_t(b.y);
				px[p.f.rgb_index[1]] = uint8_t(b.cb);
				px[p.f.rgb_index[2]] = uint8_t(b.cr);
			}
		p.f.plane[0] = {p.store[0].data(), pitch};
		return;
	}
	const int pitch0 = kPicW * bps + pad;
	p.store[0].assign(size_t(pitch0) * kPicH, 0);
	for (int y = 0; y < kPicH; y++)
		for (int x = 0; x < kPicW; x++)
			put(p.store[0], size_t(y) * pitch0 + x * bps, layout == ps5gpu::VideoLayout::PlanarGBR ? p.bars[x / 16].cb : p.bars[x / 16].y);
	p.f.plane[0] = {p.store[0].data(), pitch0};
	if (layout == ps5gpu::VideoLayout::Gray)
		return;
	if (layout == ps5gpu::VideoLayout::SemiPlanarUV || layout == ps5gpu::VideoLayout::SemiPlanarVU)
	{
		const int pitch = cw * 2 * bps + pad;
		p.store[1].assign(size_t(pitch) * ch, 0);
		const bool vu = layout == ps5gpu::VideoLayout::SemiPlanarVU;
		for (int y = 0; y < ch; y++)
			for (int x = 0; x < cw; x++)
			{
				const Ycc& b = p.bars[(x << sx) / 16];
				put(p.store[1], size_t(y) * pitch + x * 2 * bps, vu ? b.cr : b.cb);
				put(p.store[1], size_t(y) * pitch + x * 2 * bps + bps, vu ? b.cb : b.cr);
			}
		p.f.plane[1] = {p.store[1].data(), pitch};
		return;
	}
	const bool gbr = layout == ps5gpu::VideoLayout::PlanarGBR;
	const int w = gbr ? kPicW : cw, h = gbr ? kPicH : ch;
	for (int k = 1; k < 3; k++)
	{
		const int pitch = w * bps + pad;
		p.store[k].assign(size_t(pitch) * h, 0);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				const Ycc& b = p.bars[gbr ? x / 16 : (x << sx) / 16];
				// GBR: plane 0 G (cb slot), 1 B (cr slot), 2 R (y slot)
				put(p.store[k], size_t(y) * pitch + x * bps, gbr ? (k == 1 ? b.cr : b.y) : (k == 1 ? b.cb : b.cr));
			}
		p.f.plane[k] = {p.store[k].data(), pitch};
	}
}

void Present(int n = 2)
{
	for (int i = 0; i < n; i++)
		ps5gpu::Present(g_ui.data(), {0, 0, kOutW, kOutH}, false);
}

// each bar's centre on the output (the picture over the whole output: 64 output pixels a bar)
Rgb BarAt(int i)
{
	return At(i * 64 + 32, kOutH / 2);
}

void CheckBars(const char* what, const Pic& p, bool full, ps5gpu::ColorMatrix m, int tol, int hdr_mode = -1,
	bool hlg = false, double peak = 1000, double white = 250)
{
	int worst = 0;
	std::string detail;
	for (int i = 0; i < 4; i++)
	{
		double rgb[3];
		if (p.f.layout == ps5gpu::VideoLayout::Packed)
			rgb[0] = kBars8[i].y / 255.0, rgb[1] = kBars8[i].cb / 255.0, rgb[2] = kBars8[i].cr / 255.0;
		else if (p.f.layout == ps5gpu::VideoLayout::PlanarGBR)
		{
			const double mx = double((1 << p.f.bits) - 1);
			rgb[0] = p.bars[i].y / mx, rgb[1] = p.bars[i].cb / mx, rgb[2] = p.bars[i].cr / mx;
		}
		else if (p.f.layout == ps5gpu::VideoLayout::Gray)
			Reference(p.bars[i].y, 128 << (p.f.bits - 8), 128 << (p.f.bits - 8), p.f.bits, full, m, rgb);
		else
			Reference(p.bars[i].y, p.bars[i].cb, p.bars[i].cr, p.f.bits, full, m, rgb);
		if (hdr_mode > 0)
			ToneMapRef(rgb, hlg, hdr_mode, peak, white);
		const Rgb want = {int(std::lround(rgb[0] * 255)), int(std::lround(rgb[1] * 255)), int(std::lround(rgb[2] * 255))};
		const Rgb got = BarAt(i);
		worst = std::max({worst, std::abs(got.r - want.r), std::abs(got.g - want.g), std::abs(got.b - want.b)});
		detail += " " + Str(got) + "/" + Str(want);
	}
	Check(worst <= tol, std::string(what) + ": worst " + std::to_string(worst) + " (got/want" + detail + ")");
}

ps5gpu::VideoView FullView()
{
	ps5gpu::VideoView v;
	v.dst = {0, 0, kOutW, kOutH};
	return v;
}

int Video()
{
	ps5gpu::Config c = BaseConfig();
	c.video = true;
	if (!StartOffscreen(kOutW, kOutH, c))
		return g_fail;
	Check(ps5gpu::VideoAvailable(), "the video pipeline is there (its self-test passed)");
	using L = ps5gpu::VideoLayout;
	using M = ps5gpu::ColorMatrix;
	struct Case
	{
		const char* name;
		L layout;
		int bits;
		bool msb;
		int sx, sy;
		M m;
		bool full;
		int pad;
	};
	const Case cases[] = {
		{"yuv420p (BT.709, limited)", L::Planar, 8, false, 1, 1, M::BT709, false, 0},
		{"yuv420p, rows padded to an odd pitch (BT.601)", L::Planar, 8, false, 1, 1, M::BT601, false, 3},
		{"yuvj420p (BT.601, full range)", L::Planar, 8, false, 1, 1, M::BT601, true, 0},
		{"yuv422p", L::Planar, 8, false, 1, 0, M::BT709, false, 0},
		{"yuv444p", L::Planar, 8, false, 0, 0, M::BT709, false, 0},
		{"yuv410p (4:1:0)", L::Planar, 8, false, 2, 2, M::BT601, false, 0},
		{"yuv420p10le (BT.2020)", L::Planar, 10, false, 1, 1, M::BT2020, false, 0},
		{"yuv422p10le", L::Planar, 10, false, 1, 0, M::BT709, false, 0},
		{"yuv444p12le", L::Planar, 12, false, 0, 0, M::BT709, false, 0},
		{"yuv420p16le", L::Planar, 16, false, 1, 1, M::BT709, false, 0},
		{"nv12", L::SemiPlanarUV, 8, false, 1, 1, M::BT709, false, 0},
		{"nv21", L::SemiPlanarVU, 8, false, 1, 1, M::BT709, false, 0},
		{"p010le (bits at the top, BT.2020)", L::SemiPlanarUV, 10, true, 1, 1, M::BT2020, false, 0},
		{"the console decoder's 10-bit (semi-planar, bits at the bottom)", L::SemiPlanarUV, 10, false, 1, 1, M::BT2020, false, 0},
		{"nv16 (semi-planar 4:2:2)", L::SemiPlanarUV, 8, false, 1, 0, M::BT709, false, 0},
		{"gray", L::Gray, 8, false, 0, 0, M::BT709, false, 0},
		{"gray10le", L::Gray, 10, false, 0, 0, M::BT709, false, 0},
		{"gbrp", L::PlanarGBR, 8, false, 0, 0, M::BT709, true, 0},
		{"gbrp10le", L::PlanarGBR, 10, false, 0, 0, M::BT709, true, 0},
		{"FCC", L::Planar, 8, false, 1, 1, M::FCC, false, 0},
		{"SMPTE 240M", L::Planar, 8, false, 1, 1, M::SMPTE240M, false, 0},
	};
	for (const Case& cs : cases)
	{
		Pic p;
		Fill(p, cs.layout, cs.bits, cs.msb, cs.sx, cs.sy, kBars8, cs.pad);
		p.f.matrix = cs.m;
		p.f.full_range = cs.full;
		p.f.release = Released;
		g_released = 0;
		const bool took = ps5gpu::SetVideo(p.f, FullView());
		Check(took && g_released == 1, std::string(cs.name) + ": taken, copied, released at once");
		Present();
		CheckBars(cs.name, p, cs.full, cs.m, 2);
	}
	// packed RGB in two byte orders
	for (int order = 0; order < 2; order++)
	{
		Pic p;
		ps5gpu::VideoFrame tmp;
		Fill(p, L::Packed, 8, false, 0, 0, kBars8, order * 8);
		if (order == 1)
		{
			// BGRA: the same picture with R and B swapped in memory
			p.f.rgb_index[0] = 2, p.f.rgb_index[2] = 0;
			for (size_t i = 0; i + 3 < p.store[0].size(); i += 4)
				std::swap(p.store[0][i], p.store[0][i + 2]);
		}
		ps5gpu::SetVideo(p.f, FullView());
		Present();
		CheckBars(order ? "bgra (pitch padded)" : "rgba", p, true, M::BT709, 2);
	}
	// HDR: PQ (BT.2020, a 10-bit P010 picture) and HLG, each tone mapping against mpc_tonemap.cpp's arithmetic
	{
		const Ycc hdr8[4] = {{180, 110, 160}, {120, 150, 100}, {200, 128, 128}, {60, 128, 128}};
		const char* names[4] = {"off", "BT.2390", "Hable", "Reinhard"};
		for (int hlg = 0; hlg < 2; hlg++)
			for (int mode = 0; mode < 4; mode++)
			{
				Pic p;
				Fill(p, L::SemiPlanarUV, 10, true, 1, 1, hdr8);
				p.f.matrix = M::BT2020;
				p.f.transfer = hlg ? ps5gpu::VideoTransfer::Hlg : ps5gpu::VideoTransfer::Pq;
				p.f.peak_nits = 4000;
				ps5gpu::VideoView v = FullView();
				v.tone_map = ps5gpu::ToneMap(mode);
				v.white_nits = 203;
				ps5gpu::SetVideo(p.f, v);
				Present();
				CheckBars((std::string(hlg ? "HLG" : "HDR10 (PQ), peak 4000 nits") + ", tone mapping " + names[mode] + ", white 203").c_str(),
					p, false, M::BT2020, 3, mode, hlg != 0, 4000, 203);
			}
		// the film's peak below the white: no compression
		Pic p;
		Fill(p, L::SemiPlanarUV, 10, true, 1, 1, hdr8);
		p.f.matrix = M::BT2020;
		p.f.transfer = ps5gpu::VideoTransfer::Pq;
		p.f.peak_nits = 200;
		ps5gpu::VideoView v = FullView();
		ps5gpu::SetVideo(p.f, v);
		Present();
		CheckBars("PQ, peak 200 nits under a 250-nit white", p, false, M::BT2020, 3, 1, false, 200, 250);
	}
	// the UI surface over the picture: opaque, half transparent (premultiplied), transparent
	{
		Pic p;
		Fill(p, L::Planar, 8, false, 1, 1, kBars8);
		ps5gpu::SetVideo(p.f, FullView());
		std::fill(g_ui.begin(), g_ui.end(), 0u);
		for (int y = 0; y < kOutH; y++)
			for (int x = 0; x < 64; x++)
				g_ui[size_t(y) * kOutW + x] = 0xff00ff00u; // bar 0: opaque green
		for (int y = 0; y < kOutH; y++)
			for (int x = 64; x < 128; x++)
				g_ui[size_t(y) * kOutW + x] = 0x80000040u; // bar 1: (64, 0, 0) premultiplied, alpha 128
		Present();
		double g1[3];
		Reference(p.bars[1].y, p.bars[1].cb, p.bars[1].cr, 8, false, M::BT709, g1);
		const Rgb b0 = BarAt(0), b1 = BarAt(1);
		const Rgb want1 = {int(std::lround(64 + g1[0] * 255 * (127.0 / 255))), int(std::lround(g1[1] * 255 * (127.0 / 255))),
			int(std::lround(g1[2] * 255 * (127.0 / 255)))};
		Check(Near(b0, {0, 255, 0}, 0), "UI: an opaque pixel covers the picture (" + Str(b0) + ")");
		Check(Near(b1, want1, 2), "UI: a half-transparent one is blended over it, premultiplied (" + Str(b1) + " / " + Str(want1) + ")");
		double r2[3];
		Reference(p.bars[2].y, p.bars[2].cb, p.bars[2].cr, 8, false, M::BT709, r2);
		Check(Near(BarAt(2), {int(std::lround(r2[0] * 255)), int(std::lround(r2[1] * 255)), int(std::lround(r2[2] * 255))}, 2),
			"UI: where it is transparent, the picture shows");
		std::fill(g_ui.begin(), g_ui.end(), 0u);
		Present();
		ps5gpu::HideVideo();
		Present();
		Check(Near(BarAt(0), {0, 0, 0}, 0) && Near(BarAt(3), {0, 0, 0}, 0), "HideVideo: black under a transparent surface");
	}
	// place: a part of the screen; a zoom reaching past the surface
	{
		Pic p;
		Fill(p, L::Planar, 8, false, 1, 1, kBars8);
		ps5gpu::VideoView v;
		v.dst = {64, 32, 128, 64};
		ps5gpu::SetVideo(p.f, v);
		Present();
		double r0[3];
		Reference(p.bars[0].y, p.bars[0].cb, p.bars[0].cr, 8, false, M::BT709, r0);
		const Rgb red = {int(std::lround(r0[0] * 255)), int(std::lround(r0[1] * 255)), int(std::lround(r0[2] * 255))};
		Check(Near(At(10, 10), {0, 0, 0}, 0) && Near(At(64 + 16, 64), red, 2) && Near(At(200, 64), {0, 0, 0}, 0),
			"a rectangle of the screen: the picture there, black around it");
		v.dst = {-256, -64, 768, 256}; // 3x, centred: bar 1 and 2 on screen
		ps5gpu::SetVideoView(v);
		Present();
		double r1[3];
		Reference(p.bars[1].y, p.bars[1].cb, p.bars[1].cr, 8, false, M::BT709, r1);
		const Rgb grn = {int(std::lround(r1[0] * 255)), int(std::lround(r1[1] * 255)), int(std::lround(r1[2] * 255))};
		Check(Near(At(20, 64), grn, 2), "a zoom past the surface's edges: the middle of the picture fills it (" + Str(At(20, 64)) + ")");
	}
	// scaling: a one-pixel checkerboard made 2x smaller is grey (no aliasing), with either filter; a step upscaled 4x
	for (int filt = 0; filt < 2; filt++)
	{
		const int w = 512, h = 256;
		std::vector<uint8_t> y(size_t(w) * h), uv(size_t(w) * h / 2, 128);
		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++)
				y[size_t(j) * w + i] = ((i ^ j) & 1) ? 235 : 16;
		ps5gpu::VideoFrame f;
		f.width = w;
		f.height = h;
		f.layout = L::SemiPlanarUV;
		f.plane[0] = {y.data(), w};
		f.plane[1] = {uv.data(), w};
		ps5gpu::VideoView v = FullView();
		v.filter = filt ? ps5gpu::VideoFilter::Bicubic : ps5gpu::VideoFilter::Bilinear;
		ps5gpu::SetVideo(f, v);
		Present();
		int lo = 255, hi = 0;
		for (int j = 20; j < 100; j += 7)
			for (int i = 20; i < 230; i += 5)
			{
				lo = std::min(lo, At(i, j).g);
				hi = std::max(hi, At(i, j).g);
			}
		Check(lo >= 118 && hi <= 138, std::string(filt ? "bicubic" : "bilinear") + ": a 1-pixel checkerboard at half size is grey (" +
										   std::to_string(lo) + ".." + std::to_string(hi) + "), not aliased");
	}
	{
		// a black / white step, 16x8 -> 256x128: bilinear ramps, bicubic ramps steeper (and stays within 0..255)
		std::vector<uint8_t> y(16 * 8), uv(16 * 4, 128);
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 16; i++)
				y[j * 16 + i] = i < 8 ? 16 : 235;
		ps5gpu::VideoFrame f;
		f.width = 16;
		f.height = 8;
		f.layout = L::SemiPlanarUV;
		f.plane[0] = {y.data(), 16};
		f.plane[1] = {uv.data(), 16};
		int mid[2];
		for (int filt = 0; filt < 2; filt++)
		{
			ps5gpu::VideoView v = FullView();
			v.filter = filt ? ps5gpu::VideoFilter::Bicubic : ps5gpu::VideoFilter::Bilinear;
			ps5gpu::SetVideo(f, v);
			Present();
			mid[filt] = At(128 - 4, 64).g; // 4 pixels left of the edge (0.28 of a source pixel before it)
		}
		Check(mid[0] > 40 && mid[0] < 128 && mid[1] < mid[0], "upscaled 16x: bilinear ramps across the edge (" + std::to_string(mid[0]) +
																	"), bicubic is sharper (" + std::to_string(mid[1]) + ")");
	}
	// refused: nothing drawn from it, released anyway
	{
		Pic p;
		Fill(p, L::Planar, 8, false, 1, 1, kBars8);
		p.f.bits = 17;
		p.f.release = Released;
		g_released = 0;
		Check(!ps5gpu::SetVideo(p.f, FullView()) && g_released == 1, "17-bit samples: refused, released");
		p.f.bits = 8;
		p.f.plane[1].pitch = 4;
		g_released = 0;
		Check(!ps5gpu::SetVideo(p.f, FullView()) && g_released == 1 && !ps5gpu::VideoFormatSupported(p.f),
			"a pitch smaller than a row: refused, released");
	}
	// the ring: pictures set faster than presented are copied each time and never wait for the GPU
	{
		Pic p;
		Fill(p, L::Planar, 8, false, 1, 1, kBars8);
		ps5gpu::TakeVideoStats();
		for (int i = 0; i < 7; i++)
			ps5gpu::SetVideo(p.f, FullView());
		Present(1);
		for (int i = 0; i < 7; i++)
		{
			ps5gpu::SetVideo(p.f, FullView());
			Present(1);
		}
		const ps5gpu::VideoStats s = ps5gpu::TakeVideoStats();
		Check(s.frames == 14 && s.copied == 14 && s.in_place == 0, "14 pictures set, 8 presents: all copied, counted (" +
																	   std::to_string(s.frames) + ")");
		CheckBars("... the last one shown", p, false, M::BT709, 2);
		Check(s.copy_ms >= 0 && s.present_ms > 0, "the statistics: copy " + std::to_string(s.copy_ms) + " ms, present " +
													   std::to_string(s.present_ms) + " ms, wait " + std::to_string(s.wait_ms) + " ms");
	}
	// in place: the planes in imported memory, read by the GPU there, released once it has
	{
		const size_t align = ps5gpu::HostMemoryAlignment();
		Check(align > 0, "this driver imports host memory (VK_EXT_external_memory_host), alignment " + std::to_string(align));
		if (align > 0)
		{
			Pic p;
			Fill(p, L::SemiPlanarUV, 10, false, 1, 1, kBars8); // the console decoder's own layout
			const size_t bytes = (p.store[0].size() + p.store[1].size() + align - 1) / align * align;
			uint8_t* mem = static_cast<uint8_t*>(aligned_alloc(align, bytes));
			ps5gpu::HostMemory* hm = ps5gpu::ImportHostMemory(mem, bytes);
			Check(hm != nullptr, "the memory imported");
			memcpy(mem, p.store[0].data(), p.store[0].size());
			memcpy(mem + p.store[0].size(), p.store[1].data(), p.store[1].size());
			p.f.plane[0].data = mem;
			p.f.plane[1].data = mem + p.store[0].size();
			p.f.memory = hm;
			p.f.matrix = M::BT2020;
			p.f.release = Released;
			g_released = 0;
			ps5gpu::TakeVideoStats();
			Check(ps5gpu::SetVideo(p.f, FullView()) && g_released == 0, "a picture in it taken, not released yet (the GPU hasn't read it)");
			Present(1);
			Check(g_released == 1, "released once the frame that read it was done");
			memset(mem, 0, bytes); // the decoder reuses its buffer: the picture already shown stays
			Present(1);
			CheckBars("read in place (no CPU copy)", p, false, M::BT2020, 2);
			const ps5gpu::VideoStats s = ps5gpu::TakeVideoStats();
			Check(s.in_place == 1 && s.copied == 0, "counted as read in place");
			// outside the block: refused
			Pic q;
			Fill(q, L::SemiPlanarUV, 8, false, 1, 1, kBars8);
			q.f.memory = hm;
			q.f.release = Released;
			g_released = 0;
			Check(!ps5gpu::SetVideo(q.f, FullView()) && g_released == 1, "planes outside the imported block: refused, released");
			// waiting for upload when the memory goes: released, nothing read from it
			p.f.release = Released;
			g_released = 0;
			ps5gpu::SetVideo(p.f, FullView());
			ps5gpu::ReleaseHostMemory(hm);
			Check(g_released == 1, "ReleaseHostMemory with a picture still waiting in it: released, not read");
			Present(1);
			free(mem);
			Check(ps5gpu::ImportHostMemory(reinterpret_cast<void*>(uintptr_t(align + 1)), align) == nullptr,
				"an unaligned block: not imported");
		}
	}
	// the chroma siting: a Cr ramp across a 4:2:0 picture, read where centred and where "left" sited chroma sits
	{
		std::vector<uint8_t> y(size_t(kPicW) * kPicH, 126), u(size_t(kPicW / 2) * kPicH / 2, 128), v(u.size());
		for (int j = 0; j < kPicH / 2; j++)
			for (int i = 0; i < kPicW / 2; i++)
				v[size_t(j) * (kPicW / 2) + i] = uint8_t(16 + 7 * i);
		ps5gpu::VideoFrame f;
		f.width = kPicW;
		f.height = kPicH;
		f.layout = L::Planar;
		f.plane[0] = {y.data(), kPicW};
		f.plane[1] = {u.data(), kPicW / 2};
		f.plane[2] = {v.data(), kPicW / 2};
		auto red_at = [&](double pos) {
			// output column 66: luma u = 66.5 / 4; chroma texel coordinate u / 2 + offset; Cr linear between centres
			const double uu = 66.5 * kPicW / kOutW, off = 0.5 - (0.5 + pos) / 2;
			const double t = uu / 2 + off - 0.5, cr = 16 + 7 * t;
			double rgb[3];
			Reference(126, 128, 0, 8, false, M::BT709, rgb); // Y and Cb, Cr below
			const double Y = (126 - 16) / 219.0, Cr = (cr - 128) / 224.0;
			return std::lround(std::max(0.0, std::min(1.0, Y + 2 * (1 - 0.2126) * Cr)) * 255);
		};
		int got[2];
		for (int k = 0; k < 2; k++)
		{
			f.chroma_pos_x = k == 0 ? 0.5 : 0.0;
			ps5gpu::SetVideo(f, FullView());
			Present();
			got[k] = At(66, 64).r;
		}
		Check(std::abs(got[0] - red_at(0.5)) <= 1 && std::abs(got[1] - red_at(0.0)) <= 1 && got[1] - got[0] >= 2,
			"chroma siting: centred " + std::to_string(got[0]) + " (want " + std::to_string(red_at(0.5)) + "), left " +
				std::to_string(got[1]) + " (want " + std::to_string(red_at(0.0)) + ")");
	}
	ps5gpu::Shutdown();
	Check(!ps5gpu::VideoAvailable(), "after Shutdown: no video");
	return g_fail;
}

// review M1: a picture whose plane images can't be made is refused by SetVideo (the program draws it), never taken and
// left black; a picture of the size already shown still is (its images are there). Run with PS5GPU_TEST_VIDEO_IMAGE_FAIL.
int VideoImageFail()
{
	ps5gpu::Config c = BaseConfig();
	c.video = true;
	unsetenv("PS5GPU_TEST_VIDEO_IMAGE_FAIL"); // the self-test's images first
	if (!StartOffscreen(kOutW, kOutH, c))
		return g_fail;
	Pic p;
	Fill(p, ps5gpu::VideoLayout::Planar, 8, false, 1, 1, kBars8);
	Check(ps5gpu::SetVideo(p.f, FullView()), "a picture taken while images can be made");
	Present();
	setenv("PS5GPU_TEST_VIDEO_IMAGE_FAIL", "1", 1);
	Pic q;
	Fill(q, ps5gpu::VideoLayout::SemiPlanarUV, 10, true, 1, 1, kBars8); // another format: new images needed
	q.f.release = Released;
	g_released = 0;
	Check(!ps5gpu::SetVideo(q.f, FullView()) && g_released == 1, "another format, its images can't be made: refused, released");
	Present();
	CheckBars("... the picture shown before stays", p, false, ps5gpu::ColorMatrix::BT709, 2);
	Check(ps5gpu::SetVideo(p.f, FullView()), "the same format as shown: still taken (its images exist)");
	ps5gpu::Shutdown();
	return g_fail;
}
// 1.1 review: a failed submit, a refused picture and ReleaseVideo leave the pictures as they should be
int VideoKeep()
{
	ps5gpu::Config c = BaseConfig();
	c.video = true;
	unsetenv("PS5GPU_TEST_VIDEO_IMAGE_FAIL");
	unsetenv("PS5GPU_TEST_SUBMIT_FAIL");
	if (!StartOffscreen(kOutW, kOutH, c))
		return g_fail;
	using L = ps5gpu::VideoLayout;
	using M = ps5gpu::ColorMatrix;
	std::fill(g_ui.begin(), g_ui.end(), 0u);
	Pic p, q, r;
	Fill(p, L::Planar, 8, false, 1, 1, kBars8);
	Fill(q, L::SemiPlanarUV, 10, true, 1, 1, kBars8); // another format: plane images of its own
	Fill(r, L::Planar, 10, false, 1, 1, kBars8);
	Check(ps5gpu::SetVideo(p.f, FullView()), "a picture taken");
	Present();
	CheckBars("... shown", p, false, M::BT709, 2);
	// a new picture whose frame isn't submitted: still pending, drawn by the next Present
	Check(ps5gpu::SetVideo(q.f, FullView()), "another picture (another format) taken");
	setenv("PS5GPU_TEST_SUBMIT_FAIL", "1", 1);
	const bool failed = !ps5gpu::Present(g_ui.data(), {0, 0, kOutW, kOutH}, false);
	unsetenv("PS5GPU_TEST_SUBMIT_FAIL");
	Check(failed, "its frame's submit failed (injected)");
	Present(1);
	CheckBars("... the next Present draws it (kept pending)", q, false, M::BT709, 2);
	// no new picture, the submit failing (a paused film): the picture stays, and the UI's copy is made again
	for (int y = 0; y < kOutH; y++)
		for (int x = 0; x < 64; x++)
			g_ui[size_t(y) * kOutW + x] = 0xff00ff00u;
	setenv("PS5GPU_TEST_SUBMIT_FAIL", "1", 1);
	ps5gpu::Present(g_ui.data(), {0, 0, 64, kOutH}, false);
	unsetenv("PS5GPU_TEST_SUBMIT_FAIL");
	ps5gpu::Present(g_ui.data(), {0, 0, 0, 0}, false); // nothing new in the surface
	const Rgb b0 = BarAt(0);
	double r3[3];
	Reference(q.bars[3].y, q.bars[3].cb, q.bars[3].cr, 10, false, M::BT709, r3);
	Check(Near(b0, {0, 255, 0}, 0) &&
			  Near(BarAt(3), {int(std::lround(r3[0] * 255)), int(std::lround(r3[1] * 255)), int(std::lround(r3[2] * 255))}, 2),
		"a failed submit without a new picture: the picture stays (not black), the surface's change copied after all (" +
			Str(b0) + ")");
	std::fill(g_ui.begin(), g_ui.end(), 0u);
	Present();
	// a pending picture with plane images of its own, then a refused one: the pending one keeps them
	Check(ps5gpu::SetVideo(p.f, FullView()), "a picture of the first format pending (its own plane images)");
	setenv("PS5GPU_TEST_VIDEO_IMAGE_FAIL", "1", 1);
	r.f.release = Released;
	g_released = 0;
	Check(!ps5gpu::SetVideo(r.f, FullView()) && g_released == 1, "a third format, its images can't be made: refused");
	unsetenv("PS5GPU_TEST_VIDEO_IMAGE_FAIL");
	Present(1);
	CheckBars("... the pending picture is drawn with its images", p, false, M::BT709, 2);
	// ReleaseVideo: the picture the last frame read let go at once
	const size_t align = ps5gpu::HostMemoryAlignment();
	if (align)
	{
		Pic h;
		Fill(h, L::SemiPlanarUV, 8, false, 1, 1, kBars8);
		const size_t bytes = (h.store[0].size() + h.store[1].size() + align - 1) / align * align;
		uint8_t* mem = static_cast<uint8_t*>(aligned_alloc(align, bytes));
		ps5gpu::HostMemory* hm = ps5gpu::ImportHostMemory(mem, bytes);
		memcpy(mem, h.store[0].data(), h.store[0].size());
		memcpy(mem + h.store[0].size(), h.store[1].data(), h.store[1].size());
		h.f.plane[0].data = mem;
		h.f.plane[1].data = mem + h.store[0].size();
		h.f.memory = hm;
		h.f.release = Released;
		g_released = 0;
		ps5gpu::SetVideo(h.f, FullView());
		setenv("PS5GPU_TEST_SUBMIT_FAIL", "1", 1);
		ps5gpu::Present(g_ui.data(), {0, 0, 0, 0}, false);
		unsetenv("PS5GPU_TEST_SUBMIT_FAIL");
		Check(hm && g_released == 0, "an in-place picture whose frame wasn't submitted: still held (not released)");
		ps5gpu::ReleaseVideo();
		Check(g_released == 1, "ReleaseVideo: let go at once");
		// (final review 2) the frame left in flight, as on a display (its fence waited at the next Present): the picture
		// it reads is held until ReleaseVideo waits for it
		setenv("PS5GPU_TEST_OFFSCREEN_IN_FLIGHT", "1", 1);
		ps5gpu::SetVideo(h.f, FullView());
		Present(1);
		const int held = g_released;
		ps5gpu::ReleaseVideo();
		unsetenv("PS5GPU_TEST_OFFSCREEN_IN_FLIGHT");
		Check(held == 1 && g_released == 2, "... and the one the frame in flight reads: held until ReleaseVideo waited for it (" +
			std::to_string(held) + " then " + std::to_string(g_released) + ")");
		// the same frame left in flight: the next Present waits for it, then lets the picture go
		setenv("PS5GPU_TEST_OFFSCREEN_IN_FLIGHT", "1", 1);
		ps5gpu::SetVideo(h.f, FullView());
		Present(1);
		const int before = g_released;
		Present(1);
		unsetenv("PS5GPU_TEST_OFFSCREEN_IN_FLIGHT");
		Check(before == 2 && g_released == 3, "... or until the next Present waited for its fence (" + std::to_string(before) +
			" then " + std::to_string(g_released) + ")");
		Present(1);
		ps5gpu::ReleaseHostMemory(hm);
		free(mem);
	}
	ps5gpu::Shutdown();
	return g_fail;
}
} // namespace vt

int main(int argc, char** argv)
{
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "unit")
		return Unit();
	if (mode == "presets")
		return Presets();
	if (mode == "formats")
		return Formats();
	if (mode == "ui-straight")
		return UiStraight();
	if (mode == "damaged" && argc > 2)
		return Damaged(argv[2]);
	if (mode == "init")
		return InitOnly();
	if (mode == "guard" && argc > 2)
		return Guard(argv[2]);
	if (mode == "params" && argc > 3)
		return Params(argv[2], argv[3]);
	if (mode == "video")
		return vt::Video();
	if (mode == "video-image-fail")
		return vt::VideoImageFail();
	if (mode == "video-keep")
		return vt::VideoKeep();
	fprintf(stderr, "usage: gpu-test unit|presets|formats|ui-straight|damaged <preset>|init|guard <marker>|params <file> set|get\n");
	return 2;
}
