#version 450
// GPU version of FBNeo PS5's original Easymode-style filter (ProsperoCrt.cpp).
// Lanczos2 horizontal reconstruction, smooth vertical blend, brightness-dependent beam and RGB grille.
// SPDX-License-Identifier: MIT
layout(push_constant) uniform Push { vec4 SourceSize; vec4 OutputSize; } p;
layout(std140, set=0, binding=0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location=0) in vec4 Position;
layout(location=1) in vec2 TexCoord;
layout(location=0) out vec2 uv;
void main() { gl_Position = global.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location=0) in vec2 uv;
layout(location=0) out vec4 FragColor;
layout(set=0, binding=2) uniform sampler2D Source;
float lanczos(float x) {
    x=abs(x); if(x<0.00001) return 1.0; if(x>=2.0) return 0.0;
    float q=3.14159265*x; return 2.0*sin(q)*sin(q*0.5)/(q*q);
}
vec3 sampleAt(ivec2 q) { return texelFetch(Source, clamp(q,ivec2(0),ivec2(p.SourceSize.xy)-1),0).rgb; }
vec3 rowAt(int y, int x, vec4 weights) {
    vec3 b=sampleAt(ivec2(x,y)), c=sampleAt(ivec2(x+1,y));
    return clamp(sampleAt(ivec2(x-1,y))*weights.x+b*weights.y+c*weights.z+sampleAt(ivec2(x+2,y))*weights.w,min(b,c),max(b,c));
}
void main() {
    vec2 q=uv*p.SourceSize.xy-0.5;
    ivec2 i=ivec2(floor(q)); vec2 f=fract(q);
    float t=mix(f.x,smoothstep(0.0,1.0,f.x),0.35);
    vec4 weights=vec4(lanczos(1.0+t),lanczos(t),lanczos(1.0-t),lanczos(2.0-t));
    weights/=dot(weights,vec4(1.0));
    vec3 c=mix(rowAt(i.y,i.x,weights),rowAt(i.y+1,i.x,weights),smoothstep(0.0,1.0,f.y));
    c*=c;
    float bright=clamp(0.5*(max(c.r,max(c.g,c.b))+dot(c,vec3(0.2126,0.7152,0.0722))),0.0,1.0);
    bright=floor(bright*63.0+0.5)/63.0;
    if(p.SourceSize.y<400.0) {
        float d=abs(fract(uv.y*p.SourceSize.y)-0.5)*2.0;
        float width=0.62+0.45*bright;
        float floorLight=0.30+0.32*bright;
        c*=mix(floorLight,1.0,exp(-2.0*(d/width)*(d/width)));
    }
    int column=int(floor(gl_FragCoord.x))%3;
    c*=column==0?vec3(1.0,0.7,0.7):(column==1?vec3(0.7,1.0,0.7):vec3(0.7,0.7,1.0));
    FragColor=vec4(clamp(pow(max(c,vec3(0.0)),vec3(1.0/1.8))*1.2,0.0,1.0),1.0);
}
