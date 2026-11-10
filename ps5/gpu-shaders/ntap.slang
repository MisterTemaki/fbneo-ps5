#version 450
// GPU translation of the Ntap filter in ProsperoCrt.cpp (fishku's CC0 algorithm).
// This implementation uses the port's fixed defaults and its analytic periodic beam integral.
// SPDX-License-Identifier: MIT
#pragma parameter TWO_TAPS "Two source rows" 0.0 0.0 1.0 1.0
layout(push_constant) uniform Push { vec4 SourceSize; vec4 OutputSize; float TWO_TAPS; } p;
layout(std140,set=0,binding=0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location=0) in vec4 Position;
layout(location=1) in vec2 TexCoord;
layout(location=0) out vec2 uv;
void main() { gl_Position=global.MVP*Position; uv=TexCoord; }
#pragma stage fragment
layout(location=0) in vec2 uv;
layout(location=0) out vec4 FragColor;
layout(set=0,binding=2) uniform sampler2D Source;
vec3 beamWidth(vec3 signal) { return min(vec3(1.0),vec3(0.1)+0.85*pow(max(signal,vec3(0.0)),vec3(0.45))); }
vec3 integral(float y, vec3 width) { return floor(y)*width+clamp(vec3(fract(y)-0.5)+width*0.5,vec3(0.0),width); }
void main() {
    float sourceX=uv.x*p.SourceSize.x-0.5;
    float phase=2.0*fract(sourceX)-1.0;
    float slope=2.0; // 6 + (1 - 6) * H_SMOOTH, H_SMOOTH = 0.8
    float fraction=0.5+0.5*slope*phase/sqrt(1.0+(slope*slope-1.0)*phase*phase);
    float x=(floor(sourceX)+fraction+0.5)*p.SourceSize.z;
    float y=uv.y*p.SourceSize.y-0.31;
    float height=p.SourceSize.y*p.OutputSize.w;
    float lo=y-height*0.5, hi=y+height*0.5;
    vec3 light;
    if(p.TWO_TAPS<0.5) {
        vec3 signal=texture(Source,vec2(x,(floor(y)+0.5)*p.SourceSize.w)).rgb;
        vec3 width=beamWidth(signal);
        light=signal*signal*(integral(hi,width)-integral(lo,width))/height;
    } else {
        float row=floor(y-0.5);
        vec3 a=texture(Source,vec2(x,(row+0.5)*p.SourceSize.w)).rgb;
        vec3 b=texture(Source,vec2(x,(row+1.5)*p.SourceSize.w)).rgb;
        vec3 wa=beamWidth(a), wb=beamWidth(b);
        float split=clamp(row+1.0,lo,hi);
        light=(a*a*(integral(split,wa)-integral(lo,wa))+b*b*(integral(hi,wb)-integral(split,wb)))/height;
    }
    FragColor=vec4(sqrt(max(light,vec3(0.0))),1.0);
}
