// ps5-slang-gpu: a video picture in one pass (gpu.cpp, SetVideo). For each pixel of the output, in this order:
//   1. each plane sampled where the pixel falls (Y, then Cb Cr: planar, semi-planar UV or VU, gray, packed RGB, planar
//      GBR), filtered in the picture's own Y'CbCr: bilinear (the hardware's, or a tent widened by the shrink factor when
//      the picture is made smaller), or bicubic (Catmull-Rom, widened the same way); chroma where the stream sites it
//      (centred: libswscale's default; "left" for MPEG-2 / H.264 / HEVC: VideoFrame::chroma_pos_x / y);
//   2. the codes normalised (8 to 16 bits, in 16-bit words at the bottom or the top, limited or full range);
//   3. the colour matrix (BT.601, BT.709, BT.2020 non-constant luminance, FCC, SMPTE 240M) to R'G'B', clipped to 0..1;
//   4. for HDR10 (PQ) and HLG with tone mapping on: to linear light (PQ's EOTF in nits; HLG's inverse OETF and its
//      OOTF at 1000 nits), BT.2020 to BT.709 primaries, the highlight curve on max(R, G, B) from the film's peak to the
//      picture's white (BT.2390's EETF, Hable's filmic curve, Reinhard's), then gamma 2.4 for the screen.
// The same numbers as Media Player Classic PS5's CPU path (libswscale, then mpc_tonemap.cpp), done in float instead of
// its tables.
//
// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec2 Tc;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D P0;
layout(set = 0, binding = 1) uniform sampler2D P1;
layout(set = 0, binding = 2) uniform sampler2D P2;
layout(std140, set = 0, binding = 3) uniform V
{
	vec4 lsize; // plane 0: w, h, 1/w, 1/h (texels)
	vec4 csize; // planes 1, 2
	vec4 ratio; // source texels per output pixel: plane 0 x, y; planes 1, 2 x, y
	vec4 norm;  // Y = v * norm.x + norm.y; C = v * norm.z + norm.w (v: the texture's value)
	vec4 m0, m1, m2; // R', G', B' = dot(mN.xyz, (Y, Cb, Cr))
	vec4 tm; // the curve's top (nits), the white (nits), 1 / white, 0
	vec4 pq; // BT.2390: PqInv(top), PqInv(white) / PqInv(top), ks, 0
	vec4 coff; // 1.1: the chroma siting, an offset in chroma texels (0: centred)
	ivec4 mode; // layout, transfer, tone map, filter
	ivec4 swz;  // packed RGB: the component of R, G, B; w: the hardware's bilinear may be used (1)
}
u;

const int kPlanar = 0, kSemiUV = 1, kSemiVU = 2, kPacked = 3, kGray = 4, kPlanarGBR = 5;
const int kSdr = 0, kPq = 1, kHlg = 2;
const int kOff = 0, kBt2390 = 1, kHable = 2, kReinhard = 3;
const int kMaxTaps = 17;

float Kernel(float x, int filt)
{
	x = abs(x);
	if (filt == 0)
		return max(0.0, 1.0 - x);
	// Catmull-Rom (B = 0, C = 0.5)
	if (x < 1.0)
		return (1.5 * x - 2.5) * x * x + 1.0;
	if (x < 2.0)
		return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
	return 0.0;
}

// The plane at pos (texels, centres at i + 0.5), r source texels per output pixel.
vec4 Fetch(sampler2D t, vec2 pos, vec4 size, vec2 r)
{
	const int filt = u.mode.w;
	if (filt == 0 && r.x <= 1.0 && r.y <= 1.0 && u.swz.w == 1)
		return textureLod(t, pos * size.zw, 0.0);
	const float s = filt == 0 ? 1.0 : 2.0;
	const vec2 rr = clamp(r, vec2(1.0), vec2(filt == 0 ? 8.0 : 4.0));
	const vec2 c = pos - 0.5;
	const vec2 lo = floor(c - s * rr) + 1.0;
	const vec2 hi = floor(c + s * rr);
	vec4 acc = vec4(0.0);
	float wsum = 0.0;
	for (int j = 0; j < kMaxTaps; j++)
	{
		const float y = lo.y + float(j);
		if (y > hi.y)
			break;
		const float wy = Kernel((y - c.y) / rr.y, filt);
		if (wy == 0.0)
			continue;
		for (int i = 0; i < kMaxTaps; i++)
		{
			const float x = lo.x + float(i);
			if (x > hi.x)
				break;
			const float w = Kernel((x - c.x) / rr.x, filt) * wy;
			if (w == 0.0)
				continue;
			// a texel's centre: the same value through a linear or a nearest sampler; clamped at the edges
			acc += w * textureLod(t, (clamp(vec2(x, y), vec2(0.0), size.xy - 1.0) + 0.5) * size.zw, 0.0);
			wsum += w;
		}
	}
	return wsum != 0.0 ? acc / wsum : vec4(0.0);
}

float PqEotf(float e) // signal 0..1 -> nits
{
	const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const float p = pow(max(e, 0.0), 1.0 / m2);
	return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float PqInv(float nits) // nits -> signal 0..1
{
	const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const float y = pow(max(nits, 0.0) / 10000.0, m1);
	return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float HlgInvOetf(float e) // signal 0..1 -> scene light 0..1
{
	const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
	return e <= 0.5 ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
}

float Hable(float x)
{
	const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
	return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}

// nits in -> nits out, from [0, top] to [0, white]
float Curve(float L)
{
	const float top = u.tm.x, white = u.tm.y;
	if (L <= 0.0)
		return 0.0;
	if (top <= white)
		return L;
	if (u.mode.z == kHable)
		return white * Hable(L / white * 2.0) / Hable(top / white * 2.0);
	if (u.mode.z == kReinhard)
	{
		const float x = L / white, lw2 = (top / white) * (top / white);
		return white * x * (1.0 + x / lw2) / (1.0 + x);
	}
	const float src = u.pq.x, max_lum = u.pq.y, ks = u.pq.z;
	const float e1 = PqInv(L) / src;
	float e2 = e1;
	if (e1 > ks && ks < 1.0)
	{
		const float t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
		e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * max_lum;
	}
	return PqEotf(min(e2, 1.0) * src);
}

vec3 ToneMap(vec3 e)
{
	vec3 lin;
	if (u.mode.y == kHlg)
	{
		lin = vec3(HlgInvOetf(e.r), HlgInvOetf(e.g), HlgInvOetf(e.b));
		const float ys = dot(lin, vec3(0.2627, 0.6780, 0.0593));
		lin *= 1000.0 * pow(clamp(ys, 0.0, 1.0), 0.2);
	}
	else
		lin = vec3(PqEotf(e.r), PqEotf(e.g), PqEotf(e.b));
	// BT.2020 -> BT.709, linear light
	vec3 c = vec3(dot(vec3(1.6605, -0.5876, -0.0728), lin), dot(vec3(-0.1246, 1.1329, -0.0083), lin),
		dot(vec3(-0.0182, -0.1006, 1.1187), lin));
	c = max(c, vec3(0.0));
	const float mx = min(max(c.r, max(c.g, c.b)), u.tm.x);
	const float gain = mx > 0.0 ? Curve(mx) / mx * u.tm.z : u.tm.z;
	c = min(c * gain, vec3(1.0));
	return pow(c, vec3(1.0 / 2.4));
}

void main()
{
	const int layout_ = u.mode.x;
	const vec4 a = Fetch(P0, Tc * u.lsize.xy, u.lsize, u.ratio.xy);
	vec3 rgb;
	if (layout_ == kPacked)
		rgb = vec3(a[u.swz.x], a[u.swz.y], a[u.swz.z]);
	else
	{
		vec2 cbcr = vec2(0.0);
		vec3 gbr = vec3(0.0);
		const vec2 cp = Tc * u.csize.xy + u.coff.xy;
		if (layout_ == kPlanar || layout_ == kPlanarGBR)
		{
			gbr.x = a.r;
			gbr.y = Fetch(P1, cp, u.csize, u.ratio.zw).r;
			gbr.z = Fetch(P2, cp, u.csize, u.ratio.zw).r;
			cbcr = gbr.yz;
		}
		else if (layout_ == kSemiUV || layout_ == kSemiVU)
		{
			const vec2 uv = Fetch(P1, cp, u.csize, u.ratio.zw).rg;
			cbcr = layout_ == kSemiUV ? uv : uv.yx;
		}
		if (layout_ == kPlanarGBR)
			rgb = clamp(vec3(gbr.z, gbr.x, gbr.y) * u.norm.x + u.norm.y, 0.0, 1.0);
		else
		{
			const vec3 ycc = vec3(a.r * u.norm.x + u.norm.y, (cbcr * u.norm.z + u.norm.w) * (layout_ == kGray ? 0.0 : 1.0));
			rgb = vec3(dot(u.m0.xyz, ycc), dot(u.m1.xyz, ycc), dot(u.m2.xyz, ycc));
		}
	}
	rgb = clamp(rgb, 0.0, 1.0);
	if (u.tm.w > 0.5) // Native HDR10 output: preserve PQ; convert HLG/SDR to absolute light and encode PQ.
	{
		if (u.mode.y != kPq)
		{
			vec3 light;
			if (u.mode.y == kHlg)
			{
				light = vec3(HlgInvOetf(rgb.r), HlgInvOetf(rgb.g), HlgInvOetf(rgb.b));
				light *= 1000.0 * pow(max(dot(light, vec3(0.2627, 0.6780, 0.0593)), 0.0), 0.2);
			}
			else
			{
				light = pow(rgb, vec3(2.4)) * 203.0;
				light = vec3(dot(light, vec3(0.6274, 0.3293, 0.0433)), dot(light, vec3(0.0691, 0.9195, 0.0114)),
					dot(light, vec3(0.0164, 0.0880, 0.8956)));
			}
			rgb = vec3(PqInv(light.r), PqInv(light.g), PqInv(light.b));
		}
	}
	else if (u.mode.y != kSdr && u.mode.z != kOff)
		rgb = ToneMap(rgb);
	FragColor = vec4(rgb, 1.0);
}
