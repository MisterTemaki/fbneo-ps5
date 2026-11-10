// GPU presentation and shaders; the emulation core continues to produce a software framebuffer.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace ps5gpufront
{
void Configure(bool enabled);
bool Init();
bool Active();
void Shutdown();
bool Present(const uint32_t* rgba, int x, int y, int w, int h, bool vsync);
bool Draw(const uint32_t* argb, int w, int h, int x, int y, int dw, int dh, int shader, bool smooth, bool scanlines);
void Hide();
double TakeShaderMs();
}
