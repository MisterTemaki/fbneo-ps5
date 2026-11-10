#version 450
// ps5-slang-gpu: the program's menus, OSD and text (the CPU-drawn UI surface, by default 1920x1080, premultiplied)
// over the game picture. Pixel-exact: each output pixel reads its surface pixel, so at 4K every surface pixel
// becomes a 2x2 block, as sharp as at 1080p.
// SPDX-License-Identifier: MIT

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
} params;

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
	gl_Position = global.MVP * Position;
	vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

// MPC local HDR extension. Graphics white at 203 nits (BT.2408), BT.709 -> BT.2020, then ST.2084.
vec3 UiPq(vec3 nits)
{
	vec3 v = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
	return pow((vec3(0.8359375) + 18.8515625 * v) / (vec3(1.0) + 18.6875 * v), vec3(78.84375));
}

void main()
{
	ivec2 p = ivec2(min(vTexCoord * params.SourceSize.xy, params.SourceSize.xy - 1.0));
	FragColor = texelFetch(Source, p, 0);
	if (params.OutputSize.z < 0.0 && FragColor.a > 0.0)
	{
		vec3 light = pow(clamp(FragColor.rgb / FragColor.a, 0.0, 1.0), vec3(2.2)) * params.OutputSize.w;
		light = vec3(dot(light, vec3(0.6274, 0.3293, 0.0433)), dot(light, vec3(0.0691, 0.9195, 0.0114)),
			dot(light, vec3(0.0164, 0.0880, 0.8956)));
		// As in VLC-PS5's UI layer, compensate translucent coverage for blending on PQ codes.
		float alpha = FragColor.a >= 1.0 ? 1.0 :
			1.0 - UiPq(vec3(100.0 * pow(1.0 - FragColor.a, 2.2))).x / UiPq(vec3(100.0)).x;
		FragColor = vec4(UiPq(light) * alpha, alpha);
	}
}
