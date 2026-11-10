// SPDX-License-Identifier: MIT
#include "ProsperoGpu.h"
#include "ProsperoCrt.h"
#include "ProsperoCrash.h"
#include "ProsperoVideo.h"
#include "OrbisPaths.h"
#include <cstdlib>
#include <exception>
#include <mutex>

#if FBNEO_GPU
#include "ps5gpu/gpu.h"
#ifndef __PROSPERO__
extern "C" void FBNeoHostGpuPresent(const uint8_t*, int, int, bool);
#endif
#endif

namespace ps5gpufront
{
namespace
{
bool enabled = true, active = false;
int previous_shader = -1;
std::mutex stats_mutex;
double shader_ms = 0;
unsigned shader_samples = 0;
}
void Configure(bool on) { enabled = on; }
bool Active() { return active; }
bool Init()
{
#if FBNEO_GPU
#ifndef __PROSPERO__
	const char* host = getenv("FBNEO_HOST_GPU");
	if (!host || *host != '1') return false; // Existing host regressions remain deterministic CPU tests.
#endif
	if (!enabled) { OrbisLog("[video] GPU disabled in settings; using CPU"); return false; }
	try
	{
		ps5gpu::Config c;
		c.app_name = "FBNeo PS5";
		c.log = [](const char* line) { OrbisLog("%s", line); };
		c.cache_file = OrbisRoot() + "/cache/gpu-pipelines.bin";
		c.shipped_dir = ""; // All supported presets are compiled into this build.
		c.thread_enter = crashlog::ArmThread;
		c.thread_leave = crashlog::DisarmThread;
#ifndef __PROSPERO__
		c.host_present = FBNeoHostGpuPresent;
		c.sync_build = true;
#endif
		ps5gpu::Configure(c);
		if (!ps5gpu::GuardBegin(OrbisRoot() + "/logs/gpu-starting")) return false;
#ifdef __PROSPERO__
		active = ps5gpu::Init(ps5video::kWidth, ps5video::kHeight, false, 0, 0);
#else
		active = ps5gpu::Init(ps5video::kWidth, ps5video::kHeight, true, ps5video::kWidth, ps5video::kHeight);
#endif
		if (active)
		{
			OrbisLog("[video] GPU renderer: %s, %dx%d", ps5gpu::DeviceName(), ps5gpu::OutWidth(), ps5gpu::OutHeight());
			return true; // The startup marker is cleared only after a successful presentation.
		}
	}
	catch (const std::exception& e) { OrbisLog("[video] GPU initialization failed: %s", e.what()); }
	ps5gpu::Shutdown();
	ps5gpu::GuardEnd();
	OrbisLog("[video] GPU unavailable; using CPU renderer");
#endif
	return false;
}
void Shutdown()
{
#if FBNEO_GPU
	ps5gpu::Shutdown();
	ps5gpu::GuardEnd();
#endif
	active = false;
	previous_shader = -1;
	std::lock_guard<std::mutex> lock(stats_mutex);
	shader_ms = 0; shader_samples = 0;
}
bool Present(const uint32_t* rgba, int x, int y, int w, int h, bool vsync)
{
#if FBNEO_GPU
	if (!active) return false;
	if (w <= 0 || h <= 0) { x = y = 0; w = ps5video::kWidth; h = ps5video::kHeight; }
	try
	{
		if (ps5gpu::Present(rgba, {x,y,w,h}, vsync))
		{
			ps5gpu::GuardEnd();
			// Read Vulkan timing only on the presentation thread. The emulation thread reads our snapshot.
			const double ms = ps5gpu::TakeShaderMs();
			if (ms > 0) { std::lock_guard<std::mutex> lock(stats_mutex); shader_ms += ms; ++shader_samples; }
			return true;
		}
	}
	catch (const std::exception& e) { OrbisLog("[video] GPU presentation failed: %s", e.what()); }
#else
	(void)rgba; (void)x; (void)y; (void)w; (void)h; (void)vsync;
#endif
	return false;
}
bool Draw(const uint32_t* argb, int w, int h, int x, int y, int dw, int dh, int shader, bool smooth, bool scanlines)
{
#if FBNEO_GPU
	if (!active) return false;
	const int preset = shader == 0 ? 0 : ps5gpu::FindShader(ps5crt::Name(ps5crt::Shader(shader)));
	if (shader != previous_shader)
	{
		OrbisLog("[video] %s shader: %s", preset >= 0 ? "GPU" : "CPU fallback", ps5crt::Name(ps5crt::Shader(shader)));
		previous_shader = shader;
	}
	if (preset < 0) { ps5gpu::HideGame(); return false; }
	try { ps5gpu::SetGame(argb, w, h, {x,y,dw,dh}, preset, smooth, scanlines); return true; }
	catch (const std::exception& e) { OrbisLog("[video] GPU frame refused: %s", e.what()); ps5gpu::HideGame(); }
#else
	(void)argb; (void)w; (void)h; (void)x; (void)y; (void)dw; (void)dh; (void)shader; (void)smooth; (void)scanlines;
#endif
	return false;
}
void Hide()
{
#if FBNEO_GPU
	if (active) ps5gpu::HideGame();
#endif
}
double TakeShaderMs()
{
	std::lock_guard<std::mutex> lock(stats_mutex);
	const double ms = shader_samples ? shader_ms / shader_samples : 0;
	shader_ms = 0; shader_samples = 0;
	return ms;
}
}
