// FBNeo PS5: CRT shaders on the CPU (ProsperoCrt.h).
//
// The shaders ported here come from libretro's slang-shaders (https://github.com/libretro/slang-shaders, crt/),
// with their default parameters. Their authors and licences:
//   - crt-lottes, crt-lottes-fast: Timothy Lottes, public domain (crt-lottes-fast: "CRT Simple" / CRTS, public
//     domain); slang ports by hunterk and others.
//   - crt-1tap, crt-2tap: fishku, public domain (CC0).
//   - monoCRT: hunterk, public domain.
//   - newpixie-mini: Mattias Gustavsson, public domain (Unlicense) or MIT, at the user's choice; slang port by
//     hunterk. Used here under the Unlicense.
//   - crt-hyllian-fast: Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com, MIT licence (below); GLSL/slang port
//     by DariusG & hunterk, with cgwg's magenta/green dot mask.
//   - crt-nobody: Copyright (C) 2011-2025 Hyllian - sergiogdb@gmail.com, MIT licence (below).
//   - crt-blurPi: Oriol Ferrer Mesia (armadillu, http://uri.cat), MIT licence (below).
// The "Easymode style" shader is original code written for this port: it aims at the look of EasyMode's
// crt-easymode (flat screen, sharp Lanczos horizontal filter, scanlines that widen with brightness, an aperture
// grille), not at its code, which is GPL.
//
// MIT licence of crt-hyllian-fast, crt-nobody and crt-blurPi (their copyright lines are above):
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without
//   limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the following
//   conditions: The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//   TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// How a GPU shader becomes CPU work here: the screen rectangle is dw x dh, the source w x h. In the shaders,
// u = (X + 0.5) / dw and v = (Y + 0.5) / dh are the pixel's texture coordinates. Anything that depends only on a
// source line and on u (the horizontal filter) is computed once per source line and screen column into a "row
// buffer"; per screen pixel only the vertical part, the mask and a gamma table remain. Curved shaders keep a
// per-pixel map (where each screen pixel looks in the source) and read the row buffers between two columns.
//
// SPDX-License-Identifier: MIT
#include "ProsperoCrt.h"

#include "ProsperoThread.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ps5crt
{
namespace
{
// ---- small vector maths ------------------------------------------------------------------------------------
struct V3
{
	float r, g, b;
};
inline V3 operator+(V3 a, V3 b)
{
	return {a.r + b.r, a.g + b.g, a.b + b.b};
}
inline V3 operator-(V3 a, V3 b)
{
	return {a.r - b.r, a.g - b.g, a.b - b.b};
}
inline V3 operator*(V3 a, float s)
{
	return {a.r * s, a.g * s, a.b * s};
}
inline V3 operator*(V3 a, V3 b)
{
	return {a.r * b.r, a.g * b.g, a.b * b.b};
}
inline V3 Mix(V3 a, V3 b, float t)
{
	return a + (b - a) * t;
}
inline float Max3(V3 c)
{
	return std::max(c.r, std::max(c.g, c.b));
}
inline float Sat(float v)
{
	return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
inline float Fract(float v)
{
	return v - std::floor(v);
}
inline int Clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
inline float SmoothStep(float e0, float e1, float x)
{
	const float t = Sat((x - e0) / (e1 - e0));
	return t * t * (3.f - 2.f * t);
}
inline uint32_t Pack(uint8_t r, uint8_t g, uint8_t b) // the surface: 0xAABBGGRR
{
	return 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}
inline uint8_t To8(float v)
{
	return v <= 0.f ? 0 : (v >= 1.f ? 255 : uint8_t(v * 255.f + 0.5f));
}

// ---- tables --------------------------------------------------------------------------------------------------
// A [0,1] -> 8-bit transfer curve (gamma out), 65536 steps.
struct OutLut
{
	std::vector<uint8_t> t;
	template <class F>
	void Build(F f)
	{
		t.resize(65536);
		for (int i = 0; i < 65536; i++)
			t[size_t(i)] = To8(f(float(i) / 65535.f));
	}
	uint8_t operator()(float v) const
	{
		if (!(v > 0.f))
			return t[0];
		if (v >= 1.f)
			return t[65535];
		return t[size_t(v * 65535.f + 0.5f)];
	}
};
// A [0,1] -> float curve (gamma in after filtering), 4096 steps, linear between them.
struct InLut
{
	std::vector<float> t;
	template <class F>
	void Build(F f)
	{
		t.resize(4097);
		for (int i = 0; i <= 4096; i++)
			t[size_t(i)] = f(float(i) / 4096.f);
	}
	float operator()(float v) const
	{
		if (!(v > 0.f))
			return t[0];
		if (v >= 1.f)
			return t[4096];
		const float p = v * 4096.f;
		const int i = int(p);
		return t[size_t(i)] + (t[size_t(i) + 1] - t[size_t(i)]) * (p - float(i));
	}
};

float SrgbToLinear(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float LinearToSrgb(float c) // crt-lottes' ToSrgb
{
	return c < 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 0.41666f) - 0.055f;
}

// ---- the source picture as floats --------------------------------------------------------------------------
struct Src
{
	int w = 0, h = 0;
	std::vector<V3> px;
	// lut: the 8-bit value -> float (identity /255 when null)
	void Build(const uint32_t* argb, int sw, int sh, const float* lut)
	{
		w = sw;
		h = sh;
		px.resize(size_t(w) * h);
		for (size_t i = 0; i < px.size(); i++)
		{
			const uint32_t c = argb[i];
			const int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
			px[i] = lut ? V3{lut[r], lut[g], lut[b]} : V3{r / 255.f, g / 255.f, b / 255.f};
		}
	}
	V3 Clamp(int x, int y) const
	{
		return px[size_t(Clampi(y, 0, h - 1)) * w + Clampi(x, 0, w - 1)];
	}
	V3 Border(int x, int y) const // clamp_to_border: black outside
	{
		if (unsigned(x) >= unsigned(w) || unsigned(y) >= unsigned(h))
			return {0.f, 0.f, 0.f};
		return px[size_t(y) * w + x];
	}
	// nearest texel at texture coordinates (u, v), black outside
	V3 Nearest(float u, float v) const
	{
		return Border(int(std::floor(u * w)), int(std::floor(v * h)));
	}
	// bilinear at texture coordinates, black outside
	V3 Bilinear(float u, float v) const
	{
		const float x = u * w - 0.5f, y = v * h - 0.5f;
		const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
		const float fx = x - x0, fy = y - y0;
		return Mix(Mix(Border(x0, y0), Border(x0 + 1, y0), fx), Mix(Border(x0, y0 + 1), Border(x0 + 1, y0 + 1), fx), fy);
	}
};

// One value per source line (or per "virtual" line) and screen column.
struct Rows
{
	int n = 0, dw = 0;
	std::vector<V3> d;
	std::vector<V3> zero;
	void Size(int lines, int cols)
	{
		n = lines;
		dw = cols;
		d.resize(size_t(n) * dw);
		zero.assign(size_t(cols) + 2, V3{0.f, 0.f, 0.f});
	}
	V3* Row(int r)
	{
		return d.data() + size_t(r) * dw;
	}
	const V3* RowOr0(int r) const // black outside the picture
	{
		return unsigned(r) < unsigned(n) ? d.data() + size_t(r) * dw : zero.data();
	}
	const V3* RowClamp(int r) const
	{
		return d.data() + size_t(Clampi(r, 0, n - 1)) * dw;
	}
};
// Where a curved pixel reads the row buffers: two columns and the weight of the second; inside == false when
// the pixel looks outside the picture (black).
struct ColPos
{
	int c0, c1;
	float t;
	bool inside;
};
inline ColPos ColAt(int dw, float col)
{
	ColPos p;
	p.inside = col > -0.5f && col < float(dw) - 0.5f;
	const float c = std::max(0.f, col);
	p.c0 = std::min(int(c), dw - 1);
	p.c1 = std::min(p.c0 + 1, dw - 1);
	p.t = c - float(p.c0);
	return p;
}
inline V3 At(const V3* row, const ColPos& p)
{
	return Mix(row[p.c0], row[p.c1], p.t);
}

// ---- threads ---------------------------------------------------------------------------------------------------
class Pool
{
public:
	static Pool& Get()
	{
		static Pool* p = new Pool; // lives until the process ends: its threads wait for work forever
		return *p;
	}
	// fn(i0, i1) over [0, n) in chunks, on every thread (the caller's included); returns when all are done.
	void For(int n, const std::function<void(int, int)>& fn)
	{
		if (n <= 0)
			return;
		if (m_count <= 1 || n < 8)
		{
			fn(0, n);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_fn = &fn;
			m_n = n;
			m_chunks = std::min(n, m_count * 6);
			m_next = 0;
			m_pending = m_count - 1;
			m_gen++;
		}
		m_cv.notify_all();
		Work(&fn, n, std::min(n, m_count * 6));
		std::unique_lock<std::mutex> lock(m_lock);
		m_done.wait(lock, [this] { return m_pending == 0; });
		m_fn = nullptr;
	}

private:
	Pool()
	{
		unsigned n = std::thread::hardware_concurrency();
		n = std::clamp(n == 0 ? 4u : n, 1u, 6u);
		// m_count is the caller plus the threads that really started: For() waits for that many, so a thread the
		// system refused must not be counted (it would never answer, and the frame would wait forever)
		m_count = 1;
		for (unsigned i = 1; i < n; i++)
		{
			ps5::BigThread t([this] { Worker(); }, 256 * 1024);
			if (!t.joinable())
				break;
			m_threads.push_back(std::move(t));
			m_count++;
		}
	}
	void Work(const std::function<void(int, int)>* fn, int n, int chunks)
	{
		for (;;)
		{
			const int c = m_next.fetch_add(1);
			if (c >= chunks)
				return;
			const int i0 = int(int64_t(n) * c / chunks), i1 = int(int64_t(n) * (c + 1) / chunks);
			if (i0 < i1)
				(*fn)(i0, i1);
		}
	}
	void Worker()
	{
		uint64_t seen = 0;
		for (;;)
		{
			const std::function<void(int, int)>* fn;
			int n, chunks;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_cv.wait(lock, [&] { return m_gen != seen; });
				seen = m_gen;
				fn = m_fn;
				n = m_n;
				chunks = m_chunks;
			}
			if (fn)
				Work(fn, n, chunks);
			{
				std::lock_guard<std::mutex> lock(m_lock);
				if (--m_pending == 0)
					m_done.notify_one();
			}
		}
	}
	std::vector<ps5::BigThread> m_threads; // the pool lives as long as the app; never joined
	int m_count = 1;
	std::mutex m_lock;
	std::condition_variable m_cv, m_done;
	uint64_t m_gen = 0;
	int m_pending = 0;
	const std::function<void(int, int)>* m_fn = nullptr;
	int m_n = 0, m_chunks = 0;
	std::atomic<int> m_next{0};
};

void ParallelFor(int n, const std::function<void(int, int)>& fn)
{
	Pool::Get().For(n, fn);
}

// What every shader is given.
struct Job
{
	const uint32_t* argb;
	int w, h;
	uint32_t* surf; // the rectangle's top left
	int pitch;
	int dx, dw, dh; // dx: the rectangle's left edge on the screen (masks follow the screen's pixels)
	uint64_t frame;
};

// ===================================================================================================================
// Easymode style (original). Horizontal: 4-tap Lanczos (a = 2) on the gamma-encoded picture, its phase pushed a
// little towards the nearest texel for sharpness, ringing clamped to the two middle texels. Vertical: the two
// nearest lines blended with a smoothstep. Light: gamma 2.0 in; each line is a beam whose profile is a Gaussian
// that gets wider as the colour gets brighter, so dark lines show more black between them; aperture grille
// (R, G, B columns) at 30%; gamma 1/1.8 out with a 1.2 brightness boost.
// ===================================================================================================================
struct Easymode
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	std::vector<int> cx; // per column: the left one of the two middle texels
	std::vector<float> cwt; // per column: 4 weights
	Src src;
	Rows rows;
	OutLut out;
	static constexpr int kLevels = 64; // brightness steps of the beam table
	std::vector<float> beam; // per screen row: kLevels factors
	std::vector<int> row0;
	std::vector<float> rowt;

	static float Lanczos2(float x)
	{
		x = std::fabs(x);
		if (x < 1e-5f)
			return 1.f;
		if (x >= 2.f)
			return 0.f;
		const float px = 3.14159265f * x;
		return 2.f * std::sin(px) * std::sin(px * 0.5f) / (px * px);
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		cx.resize(size_t(dw));
		cwt.resize(size_t(dw) * 4);
		for (int c = 0; c < dw; c++)
		{
			const float p = (c + 0.5f) * w / dw - 0.5f;
			const int i = int(std::floor(p));
			float f = p - i;
			f += (f * f * (3.f - 2.f * f) - f) * 0.35f; // sharper: towards the nearer texel
			float k[4] = {Lanczos2(1.f + f), Lanczos2(f), Lanczos2(1.f - f), Lanczos2(2.f - f)};
			const float s = k[0] + k[1] + k[2] + k[3];
			cx[size_t(c)] = i;
			for (int j = 0; j < 4; j++)
				cwt[size_t(c) * 4 + j] = k[j] / s;
		}
		row0.resize(size_t(dh));
		rowt.resize(size_t(dh));
		beam.resize(size_t(dh) * kLevels);
		const bool lines = h < 400; // interlaced / high-resolution pictures: no scanlines
		for (int y = 0; y < dh; y++)
		{
			const float sy = (y + 0.5f) * h / dh;
			const float p = sy - 0.5f;
			const int r = int(std::floor(p));
			float t = p - r;
			t = t * t * (3.f - 2.f * t);
			row0[size_t(y)] = r;
			rowt[size_t(y)] = t;
			const float d = std::fabs(Fract(sy) - 0.5f) * 2.f; // 0 at a line's centre, 1 between two lines
			for (int l = 0; l < kLevels; l++)
			{
				const float b = float(l) / (kLevels - 1);
				float f = 1.f;
				if (lines)
				{
					const float width = 0.62f + 0.45f * b; // the beam widens with brightness
					const float g = std::exp(-2.0f * (d / width) * (d / width));
					const float floor_ = 0.30f + 0.32f * b; // light that bleeds between the lines
					f = floor_ + (1.f - floor_) * g;
				}
				beam[size_t(y) * kLevels + l] = f;
			}
		}
		rows.Size(h, dw);
		if (out.t.empty())
			out.Build([](float v) { return std::min(1.f, std::pow(v, 1.f / 1.8f) * 1.2f); });
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, nullptr);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int i = cx[size_t(c)];
					const float* k = &cwt[size_t(c) * 4];
					const V3 a = src.Clamp(i - 1, r), b = src.Clamp(i, r), cc = src.Clamp(i + 1, r), d = src.Clamp(i + 2, r);
					V3 v = a * k[0] + b * k[1] + cc * k[2] + d * k[3];
					// no ringing beyond the two middle texels
					v.r = std::min(std::max(v.r, std::min(b.r, cc.r)), std::max(b.r, cc.r));
					v.g = std::min(std::max(v.g, std::min(b.g, cc.g)), std::max(b.g, cc.g));
					v.b = std::min(std::max(v.b, std::min(b.b, cc.b)), std::max(b.b, cc.b));
					o[c] = v;
				}
			}
		});
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const V3* a = rows.RowClamp(row0[size_t(y)]);
				const V3* b = rows.RowClamp(row0[size_t(y)] + 1);
				const float t = rowt[size_t(y)];
				const float* bt = &beam[size_t(y) * kLevels];
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				int m = (j.dx) % 3;
				for (int c = 0; c < j.dw; c++)
				{
					V3 v = Mix(a[c], b[c], t);
					v = v * v; // gamma 2.0 in
					const float luma = 0.2126f * v.r + 0.7152f * v.g + 0.0722f * v.b;
					const float bright = Sat(0.5f * (Max3(v) + luma));
					v = v * bt[int(bright * (kLevels - 1) + 0.5f)];
					// aperture grille: the column's own phosphor at full strength, the other two at 70%
					const float mr = m == 0 ? 1.f : 0.7f, mg = m == 1 ? 1.f : 0.7f, mb = m == 2 ? 1.f : 0.7f;
					o[c] = Pack(out(v.r * mr), out(v.g * mg), out(v.b * mb));
					if (++m == 3)
						m = 0;
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-1tap / crt-2tap (fishku, CC0): MIN_THICK 0.1, MAX_THICK 0.95, H_SMOOTH 0.8, V_SMOOTH 1, SUBPX_POS 0.31,
// THICK_FALLOFF 0.45; bilinear source (filter_linear0 = true).
// ===================================================================================================================
struct Ntap
{
	bool two;
	explicit Ntap(bool two_taps) : two(two_taps) {}
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	std::vector<int> cx;
	std::vector<float> cf;
	Src src;
	Rows rows;
	float width_tab[1025]; // signal (1024 steps) -> beam width
	bool tables = false;
	OutLut sqrt_lut;
	float Width(float s) const
	{
		return width_tab[s <= 0.f ? 0 : (s >= 1.f ? 1024 : int(s * 1024.f + 0.5f))];
	}
	static constexpr float kMin = 0.1f, kMax = 0.95f, kHSmooth = 0.8f, kVSmooth = 1.f, kSubpx = 0.31f, kFalloff = 0.45f;

	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		cx.resize(size_t(dw));
		cf.resize(size_t(dw));
		const float slope = 6.f + (1.f - 6.f) * kHSmooth;
		for (int c = 0; c < dw; c++)
		{
			// get_sample_x: a smooth-sign curve inside each texel, then one bilinear tap
			const float src_x = (c + 0.5f) / dw * w - 0.5f;
			const float xi = std::floor(src_x);
			const float x = 2.f * (src_x - xi) - 1.f;
			const float off = 0.5f + 0.5f * slope * x / std::sqrt(1.f + (slope * slope - 1.f) * x * x);
			cx[size_t(c)] = int(xi); // the bilinear tap at xi + off + 0.5 blends texels xi and xi + 1
			cf[size_t(c)] = off;
		}
		rows.Size(h, dw);
		if (!tables)
		{
			for (int i = 0; i <= 1024; i++)
				width_tab[i] = std::min(kMin + (kMax - kMin) * std::pow(i / 1024.f, kFalloff), 1.f);
			sqrt_lut.Build([](float v) { return std::sqrt(v); });
			tables = true;
		}
	}
	// get_beam_prefix(y, width) = cell * width + clamp(phase - 0.5 + 0.5 width, 0, width), cell and phase of y
	struct Y
	{
		float cell, phase;
	};
	static Y Split(float y)
	{
		const float c = std::floor(y);
		return {c, y - c};
	}
	static float Prefix(Y y, float width)
	{
		return y.cell * width + std::min(std::max(y.phase - 0.5f + 0.5f * width, 0.f), width);
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, nullptr);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
					o[c] = Mix(src.Clamp(cx[size_t(c)], r), src.Clamp(cx[size_t(c)] + 1, r), cf[size_t(c)]);
			}
		});
		const float fh = float(j.h) / j.dh * kVSmooth;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float sy = (y + 0.5f) / j.dh * j.h - kSubpx;
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				if (!two)
				{
					const V3* s = rows.RowClamp(int(std::floor(sy)));
					const Y lo = Split(sy - 0.5f * fh), hi = Split(sy + 0.5f * fh);
					for (int c = 0; c < j.dw; c++)
					{
						const V3 sig = s[c];
						const float wr = Width(sig.r), wg = Width(sig.g), wb = Width(sig.b);
						const float ar = Prefix(hi, wr) - Prefix(lo, wr), ag = Prefix(hi, wg) - Prefix(lo, wg),
									ab = Prefix(hi, wb) - Prefix(lo, wb);
						o[c] = Pack(sqrt_lut(sig.r * sig.r * ar / fh), sqrt_lut(sig.g * sig.g * ag / fh),
							sqrt_lut(sig.b * sig.b * ab / fh));
					}
				}
				else
				{
					const int r0 = int(std::floor(sy - 0.5f));
					const V3* s0 = rows.RowClamp(r0);
					const V3* s1 = rows.RowClamp(r0 + 1);
					const float lbf = sy - 0.5f * fh, ubf = sy + 0.5f * fh;
					const Y lb = Split(lbf), ub = Split(ubf), split = Split(std::min(std::max(float(r0) + 1.f, lbf), ubf));
					for (int c = 0; c < j.dw; c++)
					{
						const V3 a = s0[c], b = s1[c];
						auto ch = [&](float sa, float sb) {
							const float wa = Width(sa), wb = Width(sb);
							const float area0 = Prefix(split, wa) - Prefix(lb, wa);
							const float area1 = Prefix(ub, wb) - Prefix(split, wb);
							return sqrt_lut((sa * sa * area0 + sb * sb * area1) / fh);
						};
						o[c] = Pack(ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b));
					}
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-hyllian-fast (Hyllian, MIT): MASK_INTENSITY 0.5, InputGamma 2.4, OutputGamma 2.2, BRIGHTBOOST 1.5,
// SCANLINES 0.72, SHARPER 0 (Catmull-Rom horizontally); nearest source.
// ===================================================================================================================
struct HyllianFast
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	std::vector<int> cx;
	std::vector<float> cwt;
	std::vector<uint8_t> parity;
	Src src;
	Rows rows;
	InLut gin;
	OutLut gout;
	static constexpr float kMask = 0.5f, kBoost = 1.5f, kScan = 0.72f;

	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		cx.resize(size_t(dw));
		cwt.resize(size_t(dw) * 4);
		parity.resize(size_t(dw));
		for (int c = 0; c < dw; c++)
		{
			// vTexCoord.x = TexCoord.x - 0.49999 texel
			const float X = (c + 0.5f) / dw * w - 0.49999f;
			const float xi = std::floor(X);
			const float fp = X - xi;
			const float l3 = fp * fp * fp, l2 = fp * fp, l1 = fp;
			cx[size_t(c)] = int(xi);
			cwt[size_t(c) * 4 + 0] = -0.5f * l3 + 1.0f * l2 - 0.5f * l1;
			cwt[size_t(c) * 4 + 1] = 1.5f * l3 - 2.5f * l2 + 1.0f;
			cwt[size_t(c) * 4 + 2] = -1.5f * l3 + 2.0f * l2 + 0.5f * l1;
			cwt[size_t(c) * 4 + 3] = 0.5f * l3 - 0.5f * l2;
			const float mod_factor = X / w * dw; // vTexCoord.x * OutputSize.x
			parity[size_t(c)] = uint8_t(int(std::floor(mod_factor - 2.f * std::floor(mod_factor / 2.f))) & 1);
		}
		rows.Size(h, dw);
		if (gin.t.empty())
		{
			gin.Build([](float v) { return std::pow(v, 2.4f); });
			gout.Build([](float v) { return std::pow(v, 1.f / 2.2f); });
		}
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, nullptr);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int i = cx[size_t(c)];
					const float* k = &cwt[size_t(c) * 4];
					const V3 v = src.Clamp(i - 1, r) * k[0] + src.Clamp(i, r) * k[1] + src.Clamp(i + 1, r) * k[2] +
						src.Clamp(i + 2, r) * k[3];
					o[c] = V3{gin(v.r), gin(v.g), gin(v.b)};
				}
			}
		});
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float Y = (y + 0.5f) / j.dh * j.h;
				const float fy = Fract(Y);
				const V3* s = rows.RowClamp(int(std::floor(Y)));
				const float d1 = Sat(1.5f - kScan - std::fabs(fy - 0.5f));
				const float d = d1 * d1 * (3.f + kBoost - 2.f * d1);
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				for (int c = 0; c < j.dw; c++)
				{
					const V3 v = s[c] * d;
					if (parity[size_t(c)] == 0)
						o[c] = Pack(gout(v.r), gout(v.g * (1.f - kMask)), gout(v.b));
					else
						o[c] = Pack(gout(v.r * (1.f - kMask)), gout(v.g), gout(v.b * (1.f - kMask)));
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-blurPi (Oriol Ferrer Mesia, MIT): scanlineGain 0.30, rgbExtraGain 0.10, blurGain 0.15, blurRadius 1.5.
// sharp: nearest source; soft: bilinear source.
// ===================================================================================================================
struct BlurPi
{
	bool soft;
	explicit BlurPi(bool linear) : soft(linear) {}
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	Rows main_rows, side_rows; // per source line: centre + left/right taps, and the centre tap alone
	static constexpr float kScan = 0.30f, kExtra = 0.10f, kBlur = 0.15f, kRadius = 1.5f;

	// One source line sampled horizontally at u (nearest or linear).
	V3 Tap(int r, float x) const // x in texels
	{
		if (!soft)
			return src.Border(int(std::floor(x)), r);
		const float p = x - 0.5f;
		const int i = int(std::floor(p));
		return Mix(src.Border(i, r), src.Border(i + 1, r), p - i);
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
		{
			key_w = j.w, key_h = j.h, key_dw = j.dw, key_dh = j.dh;
			main_rows.Size(j.h, j.dw);
			side_rows.Size(j.h, j.dw);
		}
		src.Build(j.argb, j.w, j.h, nullptr);
		const float g0 = (1.f - 0.75f * kBlur) * (1.f + kExtra), g1 = 0.25f * kBlur;
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* m = main_rows.Row(r);
				V3* s = side_rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const float x = (c + 0.5f) / j.dw * j.w;
					const V3 centre = Tap(r, x);
					m[c] = centre * g0 + (Tap(r, x - kRadius) + Tap(r, x + kRadius)) * g1;
					s[c] = centre * g1;
				}
			}
		});
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float Y = (y + 0.5f) / j.dh * j.h; // v * SourceSize.y
				// mod(int(v * OutputSize.y), 2): every other screen row at full light, the others at 70%
				const float scan = (y & 1) ? 1.f : 1.f - kScan;
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				auto line = [&](const Rows& rows, float yy, int c) {
					if (!soft)
						return rows.RowOr0(int(std::floor(yy)))[c];
					const float p = yy - 0.5f;
					const int i = int(std::floor(p));
					return Mix(rows.RowOr0(i)[c], rows.RowOr0(i + 1)[c], p - i);
				};
				for (int c = 0; c < j.dw; c++)
				{
					const V3 v = (line(main_rows, Y, c) + line(side_rows, Y + kRadius, c)) * scan;
					o[c] = Pack(To8(v.r), To8(v.g), To8(v.b));
				}
			}
		});
	}
};

// ===================================================================================================================
// monoCRT (hunterk, public domain): brightBoost 1, scanMax 2.2, horzSharp 2, gammaIn 2.2, gammaOut 2.2, white.
// The shader's beam jitter (a few ten-thousandths of the screen, random every frame) is left out.
// ===================================================================================================================
struct MonoCrt
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	std::vector<float> env; // per source line and column: the line's local brightness
	std::vector<float> lin_lut;
	InLut gout; // pow(1 / 2.2)

	void Render(const Job& j)
	{
		if (lin_lut.empty())
		{
			lin_lut.resize(256);
			for (int i = 0; i < 256; i++)
				lin_lut[size_t(i)] = std::pow(i / 255.f, 2.2f);
			gout.Build([](float v) { return std::pow(v, 1.f / 2.2f); });
		}
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
		{
			key_w = j.w, key_h = j.h, key_dw = j.dw, key_dh = j.dh;
			env.resize(size_t(j.h) * j.dw);
		}
		src.Build(j.argb, j.w, j.h, lin_lut.data());
		const int scale_y = int(float(j.dh) / j.h);
		const int samples = std::clamp((scale_y * 3 - 1) / 2, 1, 33);
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				float* e = &env[size_t(r) * j.dw];
				for (int c = 0; c < j.dw; c++)
				{
					const float u = (c + 0.5f) / j.dw + 0.01f; // vTexCoord.x is moved by 0.01
					float sum = 0.f;
					for (int i = 0; i < samples; i++)
					{
						const V3 s = src.Border(int(std::floor((u + float(i - 16) / j.dw) * j.w)), r);
						sum += 0.299f * s.r + 0.587f * s.g + 0.114f * s.b;
					}
					e[c] = gout(sum / samples);
				}
			}
		});
		const float amp = 0.6f / j.h, edge = (30.f - 1.f) / 10000.f;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float v = (y + 0.5f) / j.dh;
				const int row = Clampi(int(std::floor(v * j.h)), 0, j.h - 1);
				const float dist = std::fabs(v - (row + 0.5f) / j.h);
				const float* e = &env[size_t(row) * j.dw];
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				for (int c = 0; c < j.dw; c++)
				{
					const float half = e[c] * 2.2f * amp * 0.5f;
					const uint8_t g = To8(SmoothStep(half, half - edge, dist));
					o[c] = Pack(g, g, g);
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-lottes-fast (CRTS, Timothy Lottes, public domain): MASK 1, MASK_INTENSITY 0.5, SCANLINE_THINNESS 0.5,
// SCAN_BLUR 2.5, CURVATURE 0.02, TRINITRON_CURVE 0, CORNER 3, CRT_GAMMA 2.4; 4 taps across, 2 lines.
// ===================================================================================================================
struct LottesFast
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	Rows rows; // per source line: the 4-tap Gaussian at each column's (flat) position
	std::vector<float> map; // per pixel: source x, source y (texels), vin
	std::vector<float> scan_lut; // 1025 x (scanA, scanB)
	OutLut out;
	std::vector<float> lin;
	float tone_y = 0, tone_z = 0;
	std::vector<int> cx0;
	std::vector<float> cwt;
	static constexpr float kThin = 0.5f + 0.5f * 0.5f, kBlur = -2.5f, kMaskDark = 1.f - 0.5f, kCurv = 0.02f;

	static void Taps(float x, int* x0, float* wt) // 4 Gaussian weights around x (texels)
	{
		const float xs = std::floor(x - 1.5f) + 0.5f;
		*x0 = int(xs - 0.5f);
		const float off0 = x - xs;
		float s = 0.f;
		for (int k = 0; k < 4; k++)
		{
			const float o = off0 - float(k);
			wt[k] = std::exp2(kBlur * o * o);
			s += wt[k];
		}
		for (int k = 0; k < 4; k++)
			wt[k] /= s;
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		rows.Size(h, dw);
		cx0.resize(size_t(dw));
		cwt.resize(size_t(dw) * 4);
		for (int c = 0; c < dw; c++)
			Taps((c + 0.5f) / dw * w, &cx0[size_t(c)], &cwt[size_t(c) * 4]);
		map.resize(size_t(dw) * dh * 3);
		const float wx = kCurv, wy = 0.75f * kCurv; // warp: x * (1 + y^2 wx), y * (1 + x^2 wy)
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				float px = (x + 0.5f) * 2.f / dw - 1.f, py = (y + 0.5f) * 2.f / dh - 1.f;
				const float nx = px * (1.f + py * py * wx), ny = py * (1.f + px * px * wy);
				float vin = 1.f - ((1.f - Sat(nx * nx)) * (1.f - Sat(ny * ny))) * (0.998f + 0.001f * 3.f);
				vin = Sat(-vin * h + h);
				float* m = &map[(size_t(y) * dw + x) * 3];
				m[0] = nx * w * 0.5f + w * 0.5f;
				m[1] = ny * h * 0.5f + h * 0.5f;
				m[2] = vin;
			}
		if (scan_lut.empty())
		{
			scan_lut.resize(1025 * 2);
			for (int i = 0; i <= 1024; i++)
			{
				const float off = i / 1024.f, pi2 = 6.28318530717958f;
				scan_lut[size_t(i) * 2] = std::cos(std::min(0.5f, off * kThin) * pi2) * 0.5f + 0.5f;
				scan_lut[size_t(i) * 2 + 1] = std::cos(std::min(0.5f, -off * kThin + kThin) * pi2) * 0.5f + 0.5f;
			}
			out.Build(LinearToSrgb);
			lin.resize(256);
			for (int i = 0; i < 256; i++)
			{
				const float c = i / 255.f;
				lin[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow(c / 1.055f + 0.055f / 1.055f, 2.4f);
			}
			// CrtsTone(contrast 1, saturation 0, thin, mask): MASK 1 -> mask = 0.5 + mask * 0.5
			const float mask = 0.5f + kMaskDark * 0.5f;
			const float mid_out = 0.18f / ((1.5f - kThin) * (0.5f * mask + 0.5f));
			const float p_mid_in = 0.18f;
			tone_y = (-p_mid_in + mid_out) / ((1.f - p_mid_in) * mid_out);
			tone_z = (-p_mid_in * mid_out + p_mid_in) / (mid_out * (-p_mid_in) + mid_out);
		}
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, lin.data());
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* o = rows.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int x0 = cx0[size_t(c)];
					const float* wt = &cwt[size_t(c) * 4];
					o[c] = src.Border(x0, r) * wt[0] + src.Border(x0 + 1, r) * wt[1] + src.Border(x0 + 2, r) * wt[2] +
						src.Border(x0 + 3, r) * wt[3];
				}
			}
		});
		const float col_scale = float(j.dw) / j.w;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 3];
				for (int c = 0; c < j.dw; c++, m += 3)
				{
					const float px = m[0], py = m[1], vin = m[2];
					const float a = py - 0.5f;
					const int ra = int(std::floor(a));
					const float off = a - float(ra);
					const float* sl = &scan_lut[size_t(off * 1024.f + 0.5f) * 2];
					const ColPos cp = ColAt(j.dw, px * col_scale - 0.5f);
					if (!cp.inside || vin <= 0.f)
					{
						o[c] = Pack(0, 0, 0);
						continue;
					}
					V3 v = (At(rows.RowOr0(ra), cp) * sl[0] + At(rows.RowOr0(ra + 1), cp) * sl[1]) * vin;
					// CrtsMask (MASK 1): the column's phosphor dimmed, in thirds of the screen's pixels
					const int mx = (c + 3000000) % 3; // fract((x + 0.5) / 3) < 1/3, < 2/3, else
					if (mx == 0)
						v.r *= kMaskDark;
					else if (mx == 1)
						v.g *= kMaskDark;
					else
						v.b *= kMaskDark;
					// CrtsTone
					const float peak = std::max(1.f / (256.f * 65536.f), Max3(v));
					const float k = (peak / (peak * tone_y + tone_z)) / peak;
					o[c] = Pack(out(v.r * k), out(v.g * k), out(v.b * k));
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-lottes (Timothy Lottes, public domain): hardScan -8, hardPix -3, warpX 0.031, warpY 0.041, maskDark 0.5,
// maskLight 1.5, scaleInLinearGamma 1, shadowMask 3, brightBoost 1, bloom (-1.5, -2, 0.15), shape 2.
// ===================================================================================================================
struct Lottes
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	Rows h3, h5, h7; // per source line: Horz3 / Horz5 / Horz7 at each column's flat position
	std::vector<float> map; // per pixel: source x, source y (texels)
	std::vector<float> wlut; // 1025 x 8: the three scan weights and the five bloom weights by line distance
	std::vector<float> lin;
	OutLut out;
	std::vector<int> cxi; // per column: floor(x)
	std::vector<float> cw; // per column: 3 + 5 + 7 normalised Gaussian weights
	static constexpr float kHardScan = -8.f, kHardPix = -3.f, kWarpX = 0.031f, kWarpY = 0.041f, kDark = 0.5f,
						   kLight = 1.5f, kBloomPix = -1.5f, kBloomScan = -2.f, kBloom = 0.15f;

	static float Gaus(float pos, float scale)
	{
		return std::exp2(scale * pos * pos); // shape 2
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		h3.Size(h, dw);
		h5.Size(h, dw);
		h7.Size(h, dw);
		cxi.resize(size_t(dw));
		cw.resize(size_t(dw) * 15);
		for (int c = 0; c < dw; c++)
		{
			const float x = (c + 0.5f) / dw * w;
			const int xi = int(std::floor(x));
			const float dst = -(x - float(xi) - 0.5f); // Dist(pos).x
			cxi[size_t(c)] = xi;
			float* t = &cw[size_t(c) * 15];
			int at = 0;
			for (int taps : {3, 5, 7})
			{
				const float scale = taps == 7 ? kBloomPix : kHardPix;
				float sum = 0.f;
				for (int k = -(taps / 2); k <= taps / 2; k++)
					sum += (t[at + k + taps / 2] = Gaus(dst + float(k), scale)); // texel xi + k: Gaus(dst + k)
				for (int k = 0; k < taps; k++)
					t[at + k] /= sum;
				at += taps;
			}
		}
		map.resize(size_t(dw) * dh * 2);
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				float px = (x + 0.5f) / dw * 2.f - 1.f, py = (y + 0.5f) / dh * 2.f - 1.f;
				const float nx = px * (1.f + py * py * kWarpX), ny = py * (1.f + px * px * kWarpY);
				map[(size_t(y) * dw + x) * 2] = (nx * 0.5f + 0.5f) * w;
				map[(size_t(y) * dw + x) * 2 + 1] = (ny * 0.5f + 0.5f) * h;
			}
		if (wlut.empty())
		{
			wlut.resize(1025 * 8);
			for (int i = 0; i <= 1024; i++)
			{
				// dst = -(fract(y) - 0.5), from 0.5 down to -0.5
				const float dst = 0.5f - i / 1024.f;
				float* t = &wlut[size_t(i) * 8];
				t[0] = Gaus(dst - 1.f, kHardScan);
				t[1] = Gaus(dst, kHardScan);
				t[2] = Gaus(dst + 1.f, kHardScan);
				for (int k = 0; k < 5; k++)
					t[3 + k] = Gaus(dst + float(k - 2), kBloomScan);
			}
			lin.resize(256);
			for (int i = 0; i < 256; i++)
				lin[size_t(i)] = SrgbToLinear(i / 255.f);
			out.Build(LinearToSrgb);
		}
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		src.Build(j.argb, j.w, j.h, lin.data());
		ParallelFor(j.h, [&](int r0, int r1) {
			for (int r = r0; r < r1; r++)
			{
				V3* a = h3.Row(r);
				V3* b = h5.Row(r);
				V3* c7 = h7.Row(r);
				for (int c = 0; c < j.dw; c++)
				{
					const int xi = cxi[size_t(c)];
					const float* t = &cw[size_t(c) * 15];
					V3 t7[7];
					for (int k = 0; k < 7; k++)
						t7[k] = src.Border(xi - 3 + k, r);
					a[c] = t7[2] * t[0] + t7[3] * t[1] + t7[4] * t[2];
					b[c] = t7[1] * t[3] + t7[2] * t[4] + t7[3] * t[5] + t7[4] * t[6] + t7[5] * t[7];
					V3 s7{0.f, 0.f, 0.f};
					for (int k = 0; k < 7; k++)
						s7 = s7 + t7[k] * t[8 + k];
					c7[c] = s7;
				}
			}
		});
		const float col_scale = float(j.dw) / j.w;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 2];
				const int mask_row = 3 * y + 2;
				for (int c = 0; c < j.dw; c++, m += 2)
				{
					const float px = m[0], py = m[1];
					const int r = int(std::floor(py));
					const float* t = &wlut[size_t((py - float(r)) * 1024.f + 0.5f) * 8];
					const ColPos cp = ColAt(j.dw, px * col_scale - 0.5f);
					if (!cp.inside)
					{
						o[c] = Pack(0, 0, 0);
						continue;
					}
					V3 tri = At(h3.RowOr0(r - 1), cp) * t[0] + At(h5.RowOr0(r), cp) * t[1] + At(h3.RowOr0(r + 1), cp) * t[2];
					const V3 bloom = At(h5.RowOr0(r - 2), cp) * t[3] + At(h7.RowOr0(r - 1), cp) * t[4] +
						At(h7.RowOr0(r), cp) * t[5] + At(h7.RowOr0(r + 1), cp) * t[6] + At(h5.RowOr0(r + 2), cp) * t[7];
					tri = tri + bloom * kBloom;
					// shadowMask 3: fract(((x + 0.5) + 3 (y + 0.5)) / 6) in thirds -> (x + 3y + 2) mod 6, two per colour
					const int k6 = (c + mask_row) % 6;
					if (k6 < 2)
						tri.r *= kLight, tri.g *= kDark, tri.b *= kDark;
					else if (k6 < 4)
						tri.r *= kDark, tri.g *= kLight, tri.b *= kDark;
					else
						tri.r *= kDark, tri.g *= kDark, tri.b *= kLight;
					o[c] = Pack(out(tri.r), out(tri.g), out(tri.b));
				}
			}
		});
	}
};

// ===================================================================================================================
// crt-nobody (Hyllian, MIT), defaults: InputGamma 2.4, OutputGamma 2.2, BrightBoost 1, vignette off, beam width
// 0.80-1.0, scanline size 0.86, horizontal, phosphor layout 1 (magenta/green aperture), mask strength 1, mask
// gamma 2.4, RGB; curvature on: sphere, radius 6, corner 0.04 / 0.5, overscan 100%. Interlaced pictures (289 to 576
// lines) are shown field by field, as the shader does.
// ===================================================================================================================
struct Nobody
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0, key_ty = 0;
	Src src;
	Rows rows; // per virtual line (TextureSize.y of them): c(x) = A wx0 + B wx1 at each column's flat position
	std::vector<float> map; // per pixel: uv.x * TSx, uv.y, corner factor
	std::vector<float> lin;
	OutLut out;
	InLut mask_on; // 1 - (1 - c)^2.4
	static constexpr float kPixSize = 1.111111f, kScanSize = 0.86f, kBeamMin = 0.80f, kBeamMax = 1.f, kRadius = 6.f,
						   kCorner = 0.04f, kCornerSmooth = 0.005f;

	static float Wgt(float s)
	{
		s = std::min(std::max(s, -1.f), 1.f);
		s = 1.f - s * s;
		return s * s * s;
	}
	void Setup(int w, int h, int dw, int dh, int ty)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh, key_ty = ty;
		rows.Size(ty, dw);
		map.resize(size_t(dw) * dh * 3);
		const float r2 = kRadius * kRadius;
		const float aspy = float(dh) / dw;
		const float cornersize = kCorner * std::min(1.f, aspy);
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				// vTexCoord = 2 * TexCoord - 1 with curvature on; h_warp, sphere
				const float ux = ((x + 0.5f) / dw) * 2.f - 1.f, uy = ((y + 0.5f) / dh) * 2.f - 1.f;
				const float sphere = kRadius / std::sqrt(std::max(1e-6f, r2 + 1.f - (ux * ux + uy * uy)));
				const float wx = ux * sphere * 0.5f + 0.5f, wy = uy * sphere * 0.5f + 0.5f;
				// h_corner
				const float dxx = std::fabs((2.f * wx - 1.f) * 1.f) - (1.f - cornersize);
				const float dyy = std::fabs((2.f * wy - 1.f) * aspy) - (aspy - cornersize);
				const float mx = std::max(dxx, 0.f), my = std::max(dyy, 0.f);
				const float borderline = std::sqrt(mx * mx + my * my) + std::min(std::max(dxx, dyy), 0.f) - cornersize;
				const float cval = SmoothStep(kCornerSmooth, -kCornerSmooth, borderline);
				float* m = &map[(size_t(y) * dw + x) * 3];
				m[0] = wx * w;
				m[1] = wy;
				m[2] = cval;
			}
		if (lin.empty())
		{
			lin.resize(256);
			for (int i = 0; i < 256; i++)
				lin[size_t(i)] = std::pow(i / 255.f, 2.4f);
			out.Build([](float v) { return std::pow(v, 1.f / 2.2f); });
			mask_on.Build([](float c) { return 1.f - std::pow(1.f - c, 2.4f); });
		}
	}
	void Render(const Job& j)
	{
		const bool interlaced = j.h > 288 && j.h < 577;
		const int ty = interlaced ? j.h / 2 : j.h;
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh || ty != key_ty)
			Setup(j.w, j.h, j.dw, j.dh, ty);
		src.Build(j.argb, j.w, j.h, lin.data());
		const float field = interlaced ? float(j.frame & 1) : 0.f;
		const float cn_off_y = interlaced ? 0.5f + 0.5f * (field - 0.5f) : 0.5f;
		const float scan_off_y = interlaced ? 0.5f * field : 0.f;
		// c(x) on virtual line k: texels A (the one under x) and B (its neighbour on x's side), Wgt weights
		ParallelFor(ty, [&](int k0, int k1) {
			for (int k = k0; k < k1; k++)
			{
				const int sr = int(std::floor((float(k) + cn_off_y) / ty * j.h)); // nearest texel row of tc.y
				V3* o = rows.Row(k);
				for (int c = 0; c < j.dw; c++)
				{
					const float pc = (c + 0.5f) / j.dw * j.w;
					const float fl = std::floor(pc);
					float pos = pc - fl - 0.5f;
					const int dir = pos < 0.f ? -1 : (pos > 0.f ? 1 : 0);
					pos = std::fabs(pos);
					const V3 a = src.Border(int(fl), sr), b = src.Border(int(fl) + dir, sr);
					o[c] = a * Wgt(pos / kPixSize) + b * Wgt((1.f - pos) / kPixSize);
				}
			}
		});
		const float col_scale = float(j.dw) / j.w;
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 3];
				for (int c = 0; c < j.dw; c++, m += 3)
				{
					const float cval = m[2];
					if (cval <= 0.f)
					{
						o[c] = Pack(0, 0, 0);
						continue;
					}
					const float pcy = m[1] * ty - scan_off_y;
					const float fl = std::floor(pcy);
					float pos = pcy - fl - cn_off_y;
					const int dir = pos < 0.f ? -1 : (pos > 0.f ? 1 : 0);
					pos = std::fabs(pos);
					const int k = int(fl);
					const ColPos cp = ColAt(j.dw, m[0] * col_scale - 0.5f);
					const V3 c0 = At(rows.RowOr0(k), cp), c1 = At(rows.RowOr0(k + dir), cp);
					const float ssy0 = kScanSize * (kBeamMin + (kBeamMax - kBeamMin) * Max3(c0));
					const float ssy1 = kScanSize * (kBeamMin + (kBeamMax - kBeamMin) * Max3(c1));
					V3 v = c0 * Wgt(pos / ssy0) + c1 * Wgt((1.f - pos) / ssy1);
					// mask layout 1: magenta / green columns; mask gamma: lit channels 1 - (1 - c)^2.4, others kept
					const int mcol = int(std::floor(float(c) + 0.5f - j.dw * 0.5f)); // mask_coords.x
					v.r = Sat(v.r), v.g = Sat(v.g), v.b = Sat(v.b);
					if ((mcol & 1) == 0)
						v.r = mask_on(v.r), v.b = mask_on(v.b);
					else
						v.g = mask_on(v.g);
					o[c] = Pack(out(v.r * cval), out(v.g * cval), out(v.b * cval));
				}
			}
		});
	}
};

// ===================================================================================================================
// newpixie-mini (Mattias Gustavsson, Unlicense): curvature 2, vignette 1; bilinear source.
// ===================================================================================================================
struct NewpixieMini
{
	int key_w = 0, key_h = 0, key_dw = 0, key_dh = 0;
	Src src;
	std::vector<float> map; // per pixel: scuv.x, scuv.y (texture coordinates), vignette x scanline
	InLut gin; // pow(c, 2.2) * 1.25
	static constexpr float kCurv = 2.f, kVignette = 1.f;

	static void Curve(float& x, float& y)
	{
		x -= 0.5f, y -= 0.5f;
		x *= 0.925f, y *= 1.095f;
		x *= kCurv, y *= kCurv;
		x *= 1.f + (std::fabs(y) / 4.f) * (std::fabs(y) / 4.f);
		y *= 1.f + (std::fabs(x) / 3.f) * (std::fabs(x) / 3.f);
		x /= kCurv, y /= kCurv;
		x += 0.5f, y += 0.5f;
		x = x * 0.92f + 0.04f, y = y * 0.92f + 0.04f;
	}
	void Setup(int w, int h, int dw, int dh)
	{
		key_w = w, key_h = h, key_dw = dw, key_dh = dh;
		map.resize(size_t(dw) * dh * 3);
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
			{
				const float u = (x + 0.5f) / dw, v = (y + 0.5f) / dh;
				float cx = u, cy = v;
				Curve(cx, cy);
				cx = cx + (u - cx) * 0.4f; // mix(curve(uv), uv, 0.4)
				cy = cy + (v - cy) * 0.4f;
				const float scale = -0.101f;
				const float sx = cx * (1.f - scale) + scale / 2.f + 0.003f, sy = cy * (1.f - scale) + scale / 2.f - 0.001f;
				float vig = (1.f - 0.99f * kVignette) + 4.f * cx * cy * (1.f - cx) * (1.f - cy);
				vig = 1.3f * std::sqrt(std::max(0.f, vig));
				const float scans = Sat(0.35f + 0.18f * std::sin(cy * dh * 1.5f));
				float* m = &map[(size_t(y) * dw + x) * 3];
				m[0] = sx;
				m[1] = sy;
				m[2] = vig * std::pow(scans, 0.9f);
			}
		if (gin.t.empty())
			gin.Build([](float c) { return std::pow(c, 2.2f) * 1.25f; });
	}
	std::vector<float> plane[3]; // R, G, B planes
	float Tex(const float* p, int x, int y) const
	{
		return (unsigned(x) < unsigned(key_w) && unsigned(y) < unsigned(key_h)) ? p[size_t(y) * key_w + x] : 0.f;
	}
	// one channel, bilinear (black outside), at tsample's texture coordinates
	float Sample(float tx, float ty, int ch) const
	{
		const float x = (tx * 1.025f - 0.0125f) * key_w - 0.5f, y = (ty * 0.92f + 0.04f) * key_h - 0.5f;
		const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
		const float fx = x - x0, fy = y - y0;
		const float* p = plane[ch].data();
		float a, b, c, d;
		if (x0 >= 0 && y0 >= 0 && x0 + 1 < key_w && y0 + 1 < key_h)
		{
			const float* q = p + size_t(y0) * key_w + x0;
			a = q[0], b = q[1], c = q[key_w], d = q[key_w + 1];
		}
		else
			a = Tex(p, x0, y0), b = Tex(p, x0 + 1, y0), c = Tex(p, x0, y0 + 1), d = Tex(p, x0 + 1, y0 + 1);
		const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
		return gin(top + (bot - top) * fy);
	}
	static float Filmic(float c)
	{
		const float x = std::max(0.f, c - 0.004f);
		return (x * (6.2f * x + 0.5f)) / (x * (6.2f * x + 1.7f) + 0.06f);
	}
	void Render(const Job& j)
	{
		if (j.w != key_w || j.h != key_h || j.dw != key_dw || j.dh != key_dh)
			Setup(j.w, j.h, j.dw, j.dh);
		for (auto& p : plane)
			p.resize(size_t(j.w) * j.h);
		for (size_t i = 0; i < size_t(j.w) * j.h; i++)
		{
			const uint32_t c = j.argb[i];
			plane[0][i] = ((c >> 16) & 255) / 255.f;
			plane[1][i] = ((c >> 8) & 255) / 255.f;
			plane[2][i] = (c & 255) / 255.f;
		}
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float o_ = std::sin((y + 0.5f) * 1.5f) / j.dw * 0.25f; // the beam's horizontal wobble
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				const float* m = &map[size_t(y) * j.dw * 3];
				for (int c = 0; c < j.dw; c++, m += 3)
				{
					const float sx = m[0] + o_, sy = m[1];
					V3 col{Sample(sx + 0.0009f, sy + 0.0009f, 0) + 0.02f, Sample(sx, sy - 0.0011f, 1) + 0.02f,
						Sample(sx - 0.0015f, sy, 2) + 0.02f};
					auto curves = [](float x) {
						const float x2 = x * x;
						return std::min(std::max(x + x2 + x2 * x2 * x, 0.f), 10.f);
					};
					col = V3{curves(col.r), curves(col.g), curves(col.b)} * m[2];
					// vertical lines: 1 - 0.23 * clamp(mod(x, 3) / 2, 0, 1)
					static const float kLines[3] = {1.f - 0.23f * 0.25f, 1.f - 0.23f * 0.75f, 1.f - 0.23f};
					col = col * kLines[(j.dx + c) % 3]; // 1 - 0.23 clamp(mod(x + 0.5, 3) / 2, 0, 1)
					o[c] = Pack(To8(Filmic(col.r)), To8(Filmic(col.g)), To8(Filmic(col.b)));
				}
			}
		});
	}
};

// ===================================================================================================================
// ScaleFX + rAA + AA style. The first part is libretro's preset scalefx+rAA+aa-fast up to its anti-aliasing:
//   ScaleFX (Sp00kyFox, MIT), its five passes: the picture 3x, edges interpolated up to level 6, only colours of
//   the original; then rAA post-3x (Sp00kyFox, MIT), reverse anti-aliasing, horizontal then vertical.
// The preset ends with FXAA, guest(r)'s AA shader 4.0 (2x) and guest(r)'s deblur, which can't be built in here
// (the last two are GPL, FXAA's notice gives no permission to copy). Original code takes their place with the same
// purpose: an edge-directed smoothing at 3x, the scale to the screen (bilinear), and a deblur that pushes each pixel
// back towards the nearest of its texels' extremes (sharp edges, no ringing).
// Parameters: the preset's SFX_CLR 0.5, SFX_SAA 0, SFX_SCN 0; rAA's defaults (sharpness 2, smoothness 0.5,
// deviation 1). Texels outside the picture: the nearest edge one.
// scalefx-pass3's lines are copied as they are (&& before ||, as in GLSL): no parentheses added to them
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
struct ScaleFxRaa
{
	struct M4 // pass0 metric / pass1 strength
	{
		float x, y, z, w;
	};
	struct F4 // pass2: corners, horizontal and vertical edges, orientation
	{
		bool c[4], h[4], v[4], o[4];
	};
	struct S8 // pass3: the subpixel picked for each corner and middle
	{
		uint8_t crn[4], mid[4];
	};
	static constexpr float kClr = 0.5f; // SFX_CLR
	static constexpr bool kSaa = false; // SFX_SAA
	static constexpr bool kScn = false; // SFX_SCN
	static constexpr float kShr = 2.f, kSmt = 0.5f, kDvt = 1.f; // rAA

	int w = 0, h = 0, W = 0, H = 0; // source, and 3x
	std::vector<V3> src;
	std::vector<M4> m0, m1;
	std::vector<F4> p2;
	std::vector<S8> p3;
	std::vector<V3> big, tmp; // 3x pictures
	int key_dw = 0, key_dh = 0;
	std::vector<int> xa, xb; // per screen column: the two 3x texels it falls between (clamped)
	std::vector<float> xf;
	std::vector<uint32_t> packed; // the smoothed 3x picture in the surface's format

	template <class T>
	static const T& At(const std::vector<T>& v, int x, int y, int w_, int h_)
	{
		return v[size_t(Clampi(y, 0, h_ - 1)) * w_ + Clampi(x, 0, w_ - 1)];
	}
	const V3& S(int x, int y) const { return At(src, x, y, w, h); }
	const M4& A0(int x, int y) const { return At(m0, x, y, w, h); }
	const M4& A1(int x, int y) const { return At(m1, x, y, w, h); }
	const F4& A2(int x, int y) const { return At(p2, x, y, w, h); }

	// Reference: http://www.compuphase.com/cmetric.htm (scalefx-pass0)
	static float Dist(V3 a, V3 b)
	{
		const float r = 0.5f * (a.r + b.r);
		const V3 d = a - b;
		return std::sqrt((2.f + r) * d.r * d.r + 4.f * d.g * d.g + (3.f - r) * d.b * d.b) / 3.f;
	}
	// corner strength (scalefx-pass1)
	static float Str(float d, float ax, float ay, float bx, float by)
	{
		const float diff = ax - ay;
		const float wght1 = std::max(kClr - d, 0.f) / kClr;
		const float wght2 = Sat((1.f - d) + (std::min(ax, bx) + ax > std::min(ay, by) + ay ? diff : -diff));
		return (kSaa || 2.f * d < ax + ay) ? (wght1 * wght2) * (ax * ay) : 0.f;
	}
	// necessary but not sufficient junction condition for orthogonal edges (scalefx-pass2)
	static bool Clear(float cx, float cy, float ax, float ay, float bx, float by)
	{
		return cx >= std::max(std::min(ax, ay), std::min(bx, by)) && cy >= std::max(std::min(ax, by), std::min(bx, ay));
	}

	void Pass0()
	{
		ParallelFor(h, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < w; x++)
				{
					const V3 E = S(x, y);
					m0[size_t(y) * w + x] = {Dist(E, S(x - 1, y - 1)), Dist(E, S(x, y - 1)), Dist(E, S(x + 1, y - 1)),
						Dist(E, S(x + 1, y))};
				}
		});
	}
	void Pass1()
	{
		ParallelFor(h, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < w; x++)
				{
					const M4 &A = A0(x - 1, y - 1), &B = A0(x, y - 1);
					const M4 &D = A0(x - 1, y), &E = A0(x, y), &F = A0(x + 1, y);
					const M4 &G = A0(x - 1, y + 1), &Hh = A0(x, y + 1), &I = A0(x + 1, y + 1);
					m1[size_t(y) * w + x] = {Str(D.z, D.w, E.y, A.w, D.y), Str(F.x, E.w, E.y, B.w, F.y),
						Str(Hh.z, E.w, Hh.y, Hh.w, I.y), Str(Hh.x, D.w, Hh.y, G.w, G.y)};
				}
		});
	}
	// A junction of four pixels P0..P3 (clockwise from the top left), each one's corner c_k = (z, w, x, y)[k]:
	// strength jS and dominance jD (2 * the corner - its two neighbouring corners), then the majority vote.
	static void Junction(const M4& p0, const M4& p1, const M4& p2_, const M4& p3, float jS[4], float j[4])
	{
		const float jD[4] = {2.f * p0.z - (p0.y + p0.w), 2.f * p1.w - (p1.z + p1.x), 2.f * p2_.x - (p2_.w + p2_.y),
			2.f * p3.y - (p3.x + p3.z)};
		jS[0] = p0.z, jS[1] = p1.w, jS[2] = p2_.x, jS[3] = p3.y;
		for (int k = 0; k < 4; k++)
		{
			const float n1 = jD[(k + 1) & 3], n3 = jD[(k + 3) & 3], n2 = jD[(k + 2) & 3];
			const float v = (jD[k] > 0.f ? 1.f : 0.f) *
							((n1 <= 0.f ? 1.f : 0.f) * (n3 <= 0.f ? 1.f : 0.f) + (jD[k] + n2 > n1 + n3 ? 1.f : 0.f));
			j[k] = std::min(v, 1.f);
		}
	}
	// inject strength without creating new contradictions: E is pixel e of the junction
	static float Inject(const float jS[4], const float j[4], int e)
	{
		const int a = (e + 3) & 3, b = (e + 1) & 3, c = (e + 2) & 3;
		const float v = j[e] + (1.f - j[a]) * (1.f - j[b]) * (jS[e] > 0.f ? 1.f : 0.f) *
								   (j[c] + (jS[e] + jS[c] > jS[a] + jS[b] ? 1.f : 0.f));
		return std::min(v, 1.f);
	}
	void Pass2()
	{
		ParallelFor(h, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < w; x++)
				{
					const M4 &A = A0(x - 1, y - 1), &B = A0(x, y - 1);
					const M4 &D = A0(x - 1, y), &E = A0(x, y), &F = A0(x + 1, y);
					const M4 &G = A0(x - 1, y + 1), &Hh = A0(x, y + 1), &I = A0(x + 1, y + 1);
					const M4 &As = A1(x - 1, y - 1), &Bs = A1(x, y - 1), &Cs = A1(x + 1, y - 1);
					const M4 &Ds = A1(x - 1, y), &Es = A1(x, y), &Fs = A1(x + 1, y);
					const M4 &Gs = A1(x - 1, y + 1), &Hs = A1(x, y + 1), &Is = A1(x + 1, y + 1);
					float jSx[4], jx[4], jSy[4], jy[4], jSz[4], jz[4], jSw[4], jw[4];
					Junction(As, Bs, Es, Ds, jSx, jx);
					Junction(Bs, Cs, Fs, Es, jSy, jy);
					Junction(Es, Fs, Is, Hs, jSz, jz);
					Junction(Ds, Es, Hs, Gs, jSw, jw);
					float res[4] = {Inject(jSx, jx, 2), Inject(jSy, jy, 3), Inject(jSz, jz, 0), Inject(jSw, jw, 1)};
					// single pixel & end of line detection
					const float jE[4] = {jx[2], jy[3], jz[0], jw[1]};
					float out[4];
					for (int k = 0; k < 4; k++)
						out[k] = std::min(res[k] * (jE[k] + (1.f - res[(k + 3) & 3] * res[(k + 1) & 3])), 1.f);
					const bool clr[4] = {Clear(D.z, E.x, D.w, E.y, A.w, D.y), Clear(F.x, E.z, E.w, E.y, B.w, F.y),
						Clear(Hh.z, I.x, E.w, Hh.y, Hh.w, I.y), Clear(Hh.x, G.z, D.w, Hh.y, G.w, G.y)};
					const float hh[4] = {std::min(D.w, A.w), std::min(E.w, B.w), std::min(E.w, Hh.w), std::min(D.w, G.w)};
					const float vv[4] = {std::min(E.y, D.y), std::min(E.y, F.y), std::min(Hh.y, I.y), std::min(Hh.y, G.y)};
					const float ho[4] = {D.w, E.w, E.w, D.w}, vo[4] = {E.y, E.y, Hh.y, Hh.y};
					F4& f = p2[size_t(y) * w + x];
					for (int k = 0; k < 4; k++)
					{
						f.c[k] = out[k] > 0.5f;
						f.h[k] = hh[k] < vv[k] && clr[k];
						f.v[k] = hh[k] > vv[k] && clr[k];
						f.o[k] = hh[k] + ho[k] > vv[k] + vo[k];
					}
				}
		});
	}
	struct B4
	{
		bool x, y, z, w;
	};
	struct B2
	{
		bool x, y;
	};
	static B4 C(const F4& f) { return {f.c[0], f.c[1], f.c[2], f.c[3]}; }
	static B4 Hz(const F4& f) { return {f.h[0], f.h[1], f.h[2], f.h[3]}; }
	static B4 Vt(const F4& f) { return {f.v[0], f.v[1], f.v[2], f.v[3]}; }
	static B4 Or(const F4& f) { return {f.o[0], f.o[1], f.o[2], f.o[3]}; }
	void Pass3()
	{
		ParallelFor(h, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < w; x++)
				{
					const F4 &E = A2(x, y);
					const F4 &D = A2(x - 1, y), &D0 = A2(x - 2, y), &D1 = A2(x - 3, y);
					const F4 &F = A2(x + 1, y), &F0 = A2(x + 2, y), &F1 = A2(x + 3, y);
					const F4 &B = A2(x, y - 1), &B0 = A2(x, y - 2), &B1 = A2(x, y - 3);
					const F4 &Hh = A2(x, y + 1), &H0 = A2(x, y + 2), &H1 = A2(x, y + 3);
					const B4 Ec = C(E), Eh = Hz(E), Ev = Vt(E), Eo = Or(E);
					const B4 Dc = C(D), Dh = Hz(D), Do = Or(D), D0c = C(D0), D0h = Hz(D0), D1h = Hz(D1);
					const B4 Fc = C(F), Fh = Hz(F), Fo = Or(F), F0c = C(F0), F0h = Hz(F0), F1h = Hz(F1);
					const B4 Bc = C(B), Bv = Vt(B), Bo = Or(B), B0c = C(B0), B0v = Vt(B0), B1v = Vt(B1);
					const B4 Hc = C(Hh), Hv = Vt(Hh), Ho = Or(Hh), H0c = C(H0), H0v = Vt(H0), H1v = Vt(H1);
					// scalefx-pass3, as written there
					const bool lvl1x = Ec.x && (Dc.z || Bc.z || kScn);
					const bool lvl1y = Ec.y && (Fc.w || Bc.w || kScn);
					const bool lvl1z = Ec.z && (Fc.x || Hc.x || kScn);
					const bool lvl1w = Ec.w && (Dc.y || Hc.y || kScn);
					const B2 lvl2x{(Ec.x && Eh.y) && Dc.z, (Ec.y && Eh.x) && Fc.w};
					const B2 lvl2y{(Ec.y && Ev.z) && Bc.w, (Ec.z && Ev.y) && Hc.x};
					const B2 lvl2z{(Ec.w && Eh.z) && Dc.y, (Ec.z && Eh.w) && Fc.x};
					const B2 lvl2w{(Ec.x && Ev.w) && Bc.z, (Ec.w && Ev.x) && Hc.y};
					const B2 lvl3x{lvl2x.y && (Dh.y && Dh.x) && Fh.z, lvl2w.y && (Bv.w && Bv.x) && Hv.z};
					const B2 lvl3y{lvl2x.x && (Fh.x && Fh.y) && Dh.w, lvl2y.y && (Bv.z && Bv.y) && Hv.w};
					const B2 lvl3z{lvl2z.x && (Fh.w && Fh.z) && Dh.x, lvl2y.x && (Hv.y && Hv.z) && Bv.x};
					const B2 lvl3w{lvl2z.y && (Dh.z && Dh.w) && Fh.y, lvl2w.x && (Hv.x && Hv.w) && Bv.y};
					const B2 lvl4x{(Dc.x && Dh.y && Eh.x && Eh.y && Fh.x && Fh.y) && (D0c.z && D0h.w),
						(Bc.x && Bv.w && Ev.x && Ev.w && Hv.x && Hv.w) && (B0c.z && B0v.y)};
					const B2 lvl4y{(Fc.y && Fh.x && Eh.y && Eh.x && Dh.y && Dh.x) && (F0c.w && F0h.z),
						(Bc.y && Bv.z && Ev.y && Ev.z && Hv.y && Hv.z) && (B0c.w && B0v.x)};
					const B2 lvl4z{(Fc.z && Fh.w && Eh.z && Eh.w && Dh.z && Dh.w) && (F0c.x && F0h.y),
						(Hc.z && Hv.y && Ev.z && Ev.y && Bv.z && Bv.y) && (H0c.x && H0v.w)};
					const B2 lvl4w{(Dc.w && Dh.z && Eh.w && Eh.z && Fh.w && Fh.z) && (D0c.y && D0h.x),
						(Hc.w && Hv.x && Ev.w && Ev.x && Bv.w && Bv.x) && (H0c.y && H0v.z)};
					const B2 lvl5x{lvl4x.x && (F0h.x && F0h.y) && (D1h.z && D1h.w), lvl4y.x && (D0h.y && D0h.x) && (F1h.w && F1h.z)};
					const B2 lvl5y{lvl4y.y && (H0v.y && H0v.z) && (B1v.w && B1v.x), lvl4z.y && (B0v.z && B0v.y) && (H1v.x && H1v.w)};
					const B2 lvl5z{lvl4w.x && (F0h.w && F0h.z) && (D1h.y && D1h.x), lvl4z.x && (D0h.z && D0h.w) && (F1h.x && F1h.y)};
					const B2 lvl5w{lvl4x.y && (H0v.x && H0v.w) && (B1v.z && B1v.y), lvl4w.y && (B0v.w && B0v.x) && (H1v.y && H1v.z)};
					const B2 lvl6x{lvl5x.y && (D1h.y && D1h.x), lvl5w.y && (B1v.w && B1v.x)};
					const B2 lvl6y{lvl5x.x && (F1h.x && F1h.y), lvl5y.y && (B1v.z && B1v.y)};
					const B2 lvl6z{lvl5z.x && (F1h.w && F1h.z), lvl5y.x && (H1v.y && H1v.z)};
					const B2 lvl6w{lvl5z.y && (D1h.z && D1h.w), lvl5w.x && (H1v.x && H1v.w)};
					// subpixels - 0 = E, 1 = D, 2 = D0, 3 = F, 4 = F0, 5 = B, 6 = B0, 7 = H, 8 = H0
					S8& o = p3[size_t(y) * w + x];
					o.crn[0] = (lvl1x && Eo.x || lvl3x.x && Eo.y || lvl4x.x && Do.x || lvl6x.x && Fo.y) ? 5 : (lvl1x || lvl3x.y && !Eo.w || lvl4x.y && !Bo.x || lvl6x.y && !Ho.w) ? 1 : lvl3x.x ? 3 : lvl3x.y ? 7 : lvl4x.x ? 2 : lvl4x.y ? 6 : lvl6x.x ? 4 : lvl6x.y ? 8 : 0;
					o.crn[1] = (lvl1y && Eo.y || lvl3y.x && Eo.x || lvl4y.x && Fo.y || lvl6y.x && Do.x) ? 5 : (lvl1y || lvl3y.y && !Eo.z || lvl4y.y && !Bo.y || lvl6y.y && !Ho.z) ? 3 : lvl3y.x ? 1 : lvl3y.y ? 7 : lvl4y.x ? 4 : lvl4y.y ? 6 : lvl6y.x ? 2 : lvl6y.y ? 8 : 0;
					o.crn[2] = (lvl1z && Eo.z || lvl3z.x && Eo.w || lvl4z.x && Fo.z || lvl6z.x && Do.w) ? 7 : (lvl1z || lvl3z.y && !Eo.y || lvl4z.y && !Ho.z || lvl6z.y && !Bo.y) ? 3 : lvl3z.x ? 1 : lvl3z.y ? 5 : lvl4z.x ? 4 : lvl4z.y ? 8 : lvl6z.x ? 2 : lvl6z.y ? 6 : 0;
					o.crn[3] = (lvl1w && Eo.w || lvl3w.x && Eo.z || lvl4w.x && Do.w || lvl6w.x && Fo.z) ? 7 : (lvl1w || lvl3w.y && !Eo.x || lvl4w.y && !Ho.w || lvl6w.y && !Bo.x) ? 1 : lvl3w.x ? 3 : lvl3w.y ? 5 : lvl4w.x ? 2 : lvl4w.y ? 8 : lvl6w.x ? 4 : lvl6w.y ? 6 : 0;
					o.mid[0] = (lvl2x.x &&  Eo.x || lvl2x.y &&  Eo.y || lvl5x.x &&  Do.x || lvl5x.y &&  Fo.y) ? 5 : lvl2x.x ? 1 : lvl2x.y ? 3 : lvl5x.x ? 2 : lvl5x.y ? 4 : (Ec.x && Dc.z && Ec.y && Fc.w) ? ( Eo.x ?  Eo.y ? 5 : 3 : 1) : 0;
					o.mid[1] = (lvl2y.x && !Eo.y || lvl2y.y && !Eo.z || lvl5y.x && !Bo.y || lvl5y.y && !Ho.z) ? 3 : lvl2y.x ? 5 : lvl2y.y ? 7 : lvl5y.x ? 6 : lvl5y.y ? 8 : (Ec.y && Bc.w && Ec.z && Hc.x) ? (!Eo.y ? !Eo.z ? 3 : 7 : 5) : 0;
					o.mid[2] = (lvl2z.x &&  Eo.w || lvl2z.y &&  Eo.z || lvl5z.x &&  Do.w || lvl5z.y &&  Fo.z) ? 7 : lvl2z.x ? 1 : lvl2z.y ? 3 : lvl5z.x ? 2 : lvl5z.y ? 4 : (Ec.z && Fc.x && Ec.w && Dc.y) ? ( Eo.z ?  Eo.w ? 7 : 1 : 3) : 0;
					o.mid[3] = (lvl2w.x && !Eo.x || lvl2w.y && !Eo.w || lvl5w.x && !Bo.x || lvl5w.y && !Ho.w) ? 1 : lvl2w.x ? 5 : lvl2w.y ? 7 : lvl5w.x ? 6 : lvl5w.y ? 8 : (Ec.w && Hc.y && Ec.x && Bc.z) ? (!Eo.w ? !Eo.x ? 1 : 5 : 7) : 0;
				}
		});
	}
	void Pass4()
	{
		static const int kOff[9][2] = {{0, 0}, {-1, 0}, {-2, 0}, {1, 0}, {2, 0}, {0, -1}, {0, -2}, {0, 1}, {0, 2}};
		ParallelFor(H, [&](int y0, int y1) {
			for (int Y = y0; Y < y1; Y++)
			{
				const int sy = Y / 3, fy = Y % 3;
				V3* out = big.data() + size_t(Y) * W;
				for (int X = 0; X < W; X++)
				{
					const int sx = X / 3, fx = X % 3;
					const S8& e = p3[size_t(sy) * w + sx];
					const int sp = fy == 0 ? (fx == 0 ? e.crn[0] : fx == 1 ? e.mid[0] : e.crn[1])
								 : fy == 1 ? (fx == 0 ? e.mid[3] : fx == 1 ? 0 : e.mid[1])
										   : (fx == 0 ? e.crn[3] : fx == 1 ? e.mid[2] : e.crn[2]);
					out[X] = S(sx + kOff[sp][0], sy + kOff[sp][1]);
				}
			}
		});
	}

	// ---- rAA post-3x (Sp00kyFox): one direction; tx[0..14] are the texels -7..+7 around the pixel
	static float Len(V3 v) { return std::sqrt(v.r * v.r + v.g * v.g + v.b * v.b); }
	static V3 Cross(V3 a, V3 b) { return {a.g * b.b - a.b * b.g, a.b * b.r - a.r * b.b, a.r * b.g - a.g * b.r}; }
	static V3 Res2x(V3 pre2, V3 pre1, V3 px, V3 pos1, V3 pos2)
	{
		const V3 df[4] = {pre1 - pre2, px - pre1, pos1 - px, pos2 - pos1};
		auto edge = [](float c) { return c < 0.5f ? c : 1.f - c; };
		auto mag = [&](float pc, float d1c, float d2c) {
			return kShr * std::min(edge(pc), std::min(std::fabs(d1c), std::fabs(d2c)));
		};
		const V3 m = {mag(px.r, df[1].r, df[2].r), mag(px.g, df[1].g, df[2].g), mag(px.b, df[1].b, df[2].b)};
		const V3 t = ((df[1] + df[2]) * 7.f - (df[0] + df[3]) * 3.f) * (1.f / 16.f); // tilt
		auto lim = [](float mc, float tc) { return tc == 0.f ? 1.f : mc / std::fabs(tc); };
		const float a = std::min(1.f, std::min(lim(m.r, t.r), std::min(lim(m.g, t.g), lim(m.b, t.b))));
		const V3 t1 = {std::clamp(t.r, -m.r, m.r), std::clamp(t.g, -m.g, m.g), std::clamp(t.b, -m.b, m.b)};
		const V3 t2 = t * a;
		float d1 = Len(df[1]), d2 = Len(df[2]);
		d1 = d1 == 0.f ? 0.f : Len(Cross(df[1], t1)) / d1;
		d2 = d2 == 0.f ? 0.f : Len(Cross(df[2], t1)) / d2;
		const float wgt = std::min(1.f, std::max(d1, d2) / 0.8125f);
		return Mix(t1, t2, kDvt == 1.f ? wgt : std::pow(wgt, kDvt));
	}
	// One pixel: tex(n) is the texel n steps along the direction (clamped at the picture's edge) and dist(a) the
	// colour distance between texels a and a + 1 (0 past the edges, where clamped texels are equal).
	template <class Tex, class Dist>
	static V3 Raa(const Tex& T, const Dist& dist)
	{
		constexpr int rad = 7, scl = 3;
		int i1x = 0, i1y = 0, i2x = 0, i2y = 0;
		const V3 t0 = T(0), tp = T(1), tm = T(-1);
		const V3 df1 = tp - t0, df2 = t0 - tm;
		float d1x, d1y, d2x = dist(0), d2y = dist(-1), d3x = d2y, d3y = d2x;
		float sw = d2x + d2y;
		sw = sw == 0.f ? 1.f : (kSmt == 0.5f ? std::sqrt(Len(df1 - df2) / sw) : std::pow(Len(df1 - df2) / sw, kSmt));
		for (int i = 1; i < rad; i++)
		{
			d1x = d2x, d1y = d2y;
			d2x = d3x, d2y = d3y;
			d3x = dist(-i - 1);
			d3y = dist(i);
			const bool cx = std::max(d1x, d3x) < d2x, cy = std::max(d1y, d3y) < d2y;
			i2x = cx && i2x == 0 && i1x != 0 ? i : i2x;
			i2y = cy && i2y == 0 && i1y != 0 ? i : i2y;
			i1x = cx && i1x == 0 ? i : i1x;
			i1y = cy && i1y == 0 ? i : i1y;
		}
		i2x = i2x == 0 ? i1x + 1 : i2x;
		i2y = i2y == 0 ? i1y + 1 : i2y;
		const V3 t = Res2x(T(-i2x), T(-i1x), t0, T(i1y), T(i2y));
		const float dw = (i1x == 0 || i1y == 0) ? 0.f : 2.f * ((i1x - 1.f) / (i1x + i1y - 2.f)) - 1.f;
		const V3 res = t0 + t * ((scl - 1.f) / scl * sw * dw);
		const V3 lo = {std::min(std::min(tm.r, t0.r), tp.r), std::min(std::min(tm.g, t0.g), tp.g),
			std::min(std::min(tm.b, t0.b), tp.b)};
		const V3 hi = {std::max(std::max(tm.r, t0.r), tp.r), std::max(std::max(tm.g, t0.g), tp.g),
			std::max(std::max(tm.b, t0.b), tp.b)};
		return {std::clamp(res.r, lo.r, hi.r), std::clamp(res.g, lo.g, hi.g), std::clamp(res.b, lo.b, hi.b)};
	}
	static bool Same(const V3& a, const V3& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
	std::vector<float> dists; // per texel: the colour distance to the next one along the pass's direction
	void RaaPass(const std::vector<V3>& in, std::vector<V3>& out, bool vertical)
	{
		// the distances between neighbours, once (each pixel's search reads up to 14 of them)
		ParallelFor(H, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < W; x++)
				{
					const bool last = vertical ? y == H - 1 : x == W - 1;
					const size_t k = size_t(y) * W + x;
					dists[k] = last ? 0.f : Len(in[vertical ? k + W : k + 1] - in[k]);
				}
		});
		ParallelFor(H, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < W; x++)
				{
					const size_t k = size_t(y) * W + x;
					// rAA ends clamped between the pixel and its two neighbours: when they are the same colour the
					// result is the pixel itself (exactly), and most of a ScaleFX picture is such flat runs
					const V3& c = in[k];
					const V3& p = vertical ? At(in, x, y - 1, W, H) : At(in, x - 1, y, W, H);
					const V3& q = vertical ? At(in, x, y + 1, W, H) : At(in, x + 1, y, W, H);
					if (Same(c, p) && Same(c, q))
					{
						out[k] = c;
						continue;
					}
					const int pos = vertical ? y : x, len = vertical ? H : W;
					const size_t step = vertical ? size_t(W) : 1;
					const size_t base = k - size_t(pos) * step; // the row's / column's first texel
					auto T = [&](int n) -> V3 { return in[base + size_t(Clampi(pos + n, 0, len - 1)) * step]; };
					auto dist = [&](int a) -> float {
						const int at = pos + a;
						return (at < 0 || at >= len - 1) ? 0.f : dists[base + size_t(at) * step];
					};
					out[k] = Raa(T, dist);
				}
		});
	}

	// ---- the original end: edge-directed smoothing at 3x (in place of FXAA + AA 4.0)
	static float Luma(V3 c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }
	V3 Bilinear(const std::vector<V3>& img, float x, float y) const // x, y in texels (centres at +0.5)
	{
		const float px = x - 0.5f, py = y - 0.5f;
		const int ix = int(std::floor(px)), iy = int(std::floor(py));
		const float fx = px - ix, fy = py - iy;
		const V3 top = Mix(At(img, ix, iy, W, H), At(img, ix + 1, iy, W, H), fx);
		const V3 bot = Mix(At(img, ix, iy + 1, W, H), At(img, ix + 1, iy + 1, W, H), fx);
		return Mix(top, bot, fy);
	}
	void Smooth(const std::vector<V3>& in, std::vector<V3>& out)
	{
		ParallelFor(H, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
				for (int x = 0; x < W; x++)
				{
					const V3 c = At(in, x, y, W, H);
					const V3 n = At(in, x, y - 1, W, H), s = At(in, x, y + 1, W, H);
					const V3 e = At(in, x + 1, y, W, H), wv = At(in, x - 1, y, W, H);
					if (Same(c, n) && Same(c, s) && Same(c, e) && Same(c, wv))
					{
						out[size_t(y) * W + x] = c; // flat: nothing to smooth (the range would be 0)
						continue;
					}
					const float lc = Luma(c), ln = Luma(n), ls = Luma(s), le = Luma(e), lw = Luma(wv);
					const float lo = std::min(lc, std::min(std::min(ln, ls), std::min(le, lw)));
					const float hi = std::max(lc, std::max(std::max(ln, ls), std::max(le, lw)));
					const float range = hi - lo;
					V3& o = out[size_t(y) * W + x];
					if (range < 0.06f)
					{
						o = c;
						continue;
					}
					// along the edge: perpendicular to the luma gradient
					float dx = -(ls - ln), dy = le - lw;
					const float len = std::sqrt(dx * dx + dy * dy);
					if (len < 1e-4f)
					{
						o = c;
						continue;
					}
					dx = dx / len * 0.75f, dy = dy / len * 0.75f;
					const V3 a = Bilinear(in, x + 0.5f + dx, y + 0.5f + dy), b = Bilinear(in, x + 0.5f - dx, y + 0.5f - dy);
					const float k = std::min(0.5f, range * 1.5f);
					V3 r = Mix(c, (a + b) * 0.5f, k);
					// no new extremes
					auto cl = [](float v, float p, float q, float s2, float t, float u) {
						return std::clamp(v, std::min(std::min(p, q), std::min(std::min(s2, t), u)),
							std::max(std::max(p, q), std::max(std::max(s2, t), u)));
					};
					r = {cl(r.r, c.r, n.r, s.r, e.r, wv.r), cl(r.g, c.g, n.g, s.g, e.g, wv.g), cl(r.b, c.b, n.b, s.b, e.b, wv.b)};
					o = r;
				}
		});
	}

	void Render(const Job& j)
	{
		if (j.w != w || j.h != h)
		{
			w = j.w, h = j.h, W = 3 * w, H = 3 * h;
			src.resize(size_t(w) * h);
			m0.resize(src.size());
			m1.resize(src.size());
			p2.resize(src.size());
			p3.resize(src.size());
			big.resize(size_t(W) * H);
			tmp.resize(big.size());
			packed.resize(big.size());
			dists.resize(big.size());
			key_dw = 0;
		}
		if (j.dw != key_dw || j.dh != key_dh)
		{
			key_dw = j.dw, key_dh = j.dh;
			xa.resize(size_t(j.dw));
			xb.resize(size_t(j.dw));
			xf.resize(size_t(j.dw));
			for (int c = 0; c < j.dw; c++)
			{
				const float px = (c + 0.5f) / j.dw * W - 0.5f;
				const int i = int(std::floor(px));
				xa[size_t(c)] = Clampi(i, 0, W - 1);
				xb[size_t(c)] = Clampi(i + 1, 0, W - 1);
				xf[size_t(c)] = px - i;
			}
		}
		for (size_t i = 0; i < src.size(); i++)
		{
			const uint32_t c = j.argb[i];
			src[i] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f};
		}
		Pass0();
		Pass1();
		Pass2();
		Pass3();
		Pass4(); // -> big (3x)
		RaaPass(big, tmp, false);
		RaaPass(tmp, big, true);
		Smooth(big, tmp); // -> tmp
		ParallelFor(H, [&](int y0, int y1) {
			for (size_t k = size_t(y0) * W; k < size_t(y1) * W; k++)
				packed[k] = Pack(To8(tmp[k].r), To8(tmp[k].g), To8(tmp[k].b));
		});
		// to the screen: bilinear, then the deblur (towards the nearer extreme of the four texels, clamped)
		ParallelFor(j.dh, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++)
			{
				const float py = (y + 0.5f) / j.dh * H - 0.5f;
				const int iy = int(std::floor(py));
				const float fy = py - iy;
				const size_t r0 = size_t(Clampi(iy, 0, H - 1)) * W, r1 = size_t(Clampi(iy + 1, 0, H - 1)) * W;
				const uint32_t *k0 = packed.data() + r0, *k1 = packed.data() + r1;
				const V3 *t0 = tmp.data() + r0, *t1 = tmp.data() + r1;
				uint32_t* o = j.surf + size_t(y) * j.pitch;
				for (int c = 0; c < j.dw; c++)
				{
					const int ia = xa[size_t(c)], ib = xb[size_t(c)];
					const uint32_t pa = k0[ia];
					if (pa == k0[ib] && pa == k1[ia] && pa == k1[ib])
					{
						o[c] = pa; // four texels of one colour: that colour
						continue;
					}
					const float fx = xf[size_t(c)];
					const V3 &a = t0[ia], &b = t0[ib], &d = t1[ia], &e = t1[ib];
					const V3 v = Mix(Mix(a, b, fx), Mix(d, e, fx), fy);
					auto deblur = [](float s, float p, float q, float r, float t) {
						const float lo = std::min(std::min(p, q), std::min(r, t)), hi = std::max(std::max(p, q), std::max(r, t));
						const float mid = 0.5f * (lo + hi);
						return std::clamp(s + 0.6f * (s - mid), lo, hi);
					};
					o[c] = Pack(To8(deblur(v.r, a.r, b.r, d.r, e.r)), To8(deblur(v.g, a.g, b.g, d.g, e.g)),
						To8(deblur(v.b, a.b, b.b, d.b, e.b)));
				}
			}
		});
	}
};
#pragma GCC diagnostic pop

// Only the shader in use is kept: each one holds per-pixel tables of tens of MiB, so the previous one is freed
// when another is picked (the tables are built again if it comes back). Used from the game thread only; never
// destroyed at exit (as the Pool).
template <class T, class... A>
T& Use(Shader s, A... args)
{
	struct Current
	{
		Shader kind = Shader::Off;
		std::shared_ptr<void> obj;
	};
	static Current* c = new Current;
	if (c->kind != s || !c->obj)
	{
		c->obj.reset(); // free the old tables before building the new ones
		c->obj = std::make_shared<T>(args...);
		c->kind = s;
	}
	return *static_cast<T*>(c->obj.get());
}
} // namespace

const char* Name(Shader s)
{
	switch (s)
	{
		case Shader::Off: return "Off";
		case Shader::EasymodeStyle: return "CRT Easymode style";
		case Shader::Lottes: return "crt-lottes";
		case Shader::LottesFast: return "crt-lottes-fast";
		case Shader::OneTap: return "crt-1tap";
		case Shader::TwoTap: return "crt-2tap";
		case Shader::HyllianFast: return "crt-hyllian-fast";
		case Shader::Nobody: return "crt-nobody";
		case Shader::NewpixieMini: return "newpixie-mini";
		case Shader::BlurPiSharp: return "crt-blurPi-sharp";
		case Shader::BlurPiSoft: return "crt-blurPi-soft";
		case Shader::MonoCrt: return "monoCRT";
		case Shader::ScaleFxRaa: return "ScaleFX + rAA + AA style";
		default: return "?";
	}
}

void Render(Shader s, const uint32_t* argb, int w, int h, uint32_t* surface, int pitch, int dx, int dy, int dw, int dh,
	uint64_t frame)
{
	if (!argb || !surface || w <= 0 || h <= 0 || dw <= 0 || dh <= 0)
		return;
	const Job j{argb, w, h, surface + size_t(dy) * pitch + dx, pitch, dx, dw, dh, frame};
	switch (s)
	{
		case Shader::EasymodeStyle: Use<Easymode>(s).Render(j); break;
		case Shader::Lottes: Use<Lottes>(s).Render(j); break;
		case Shader::LottesFast: Use<LottesFast>(s).Render(j); break;
		case Shader::OneTap: Use<Ntap>(s, false).Render(j); break;
		case Shader::TwoTap: Use<Ntap>(s, true).Render(j); break;
		case Shader::HyllianFast: Use<HyllianFast>(s).Render(j); break;
		case Shader::Nobody: Use<Nobody>(s).Render(j); break;
		case Shader::NewpixieMini: Use<NewpixieMini>(s).Render(j); break;
		case Shader::BlurPiSharp: Use<BlurPi>(s, false).Render(j); break;
		case Shader::BlurPiSoft: Use<BlurPi>(s, true).Render(j); break;
		case Shader::MonoCrt: Use<MonoCrt>(s).Render(j); break;
		case Shader::ScaleFxRaa: Use<ScaleFxRaa>(s).Render(j); break;
		default: break;
	}
}
} // namespace ps5crt
