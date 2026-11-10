// ps5-slang-gpu: the video picture's quad (gpu.cpp, SetVideo). No vertex buffer: the four corners come from
// gl_VertexIndex, the rectangle (clip space) from the push constants. Tc runs 0..1 across the picture.
//
// SPDX-License-Identifier: MIT
#version 450
layout(push_constant) uniform Push
{
	vec4 rect; // x0, y0, x1, y1 in clip space
}
pc;
layout(location = 0) out vec2 Tc;
void main()
{
	const vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
	Tc = c;
	gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, c), 0.0, 1.0);
}
