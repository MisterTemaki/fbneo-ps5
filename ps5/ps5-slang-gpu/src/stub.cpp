// ps5-slang-gpu: the renderer's stand-in for a build without the GPU (a program's CPU-only build: no RADV, no
// glslang, no shader tables): Init is false, so the program draws on the CPU; everything else is a no-op. Linked with
// src/common.cpp instead of gpu.cpp, slang_loader.cpp and the tables (mk/slang-gpu.mk, SLANG_GPU_STUB_SRC).
//
// SPDX-License-Identifier: MIT

#include "ps5gpu/gpu.h"
#include "internal.h"

#include <cstring>

namespace ps5gpu
{
bool Init(int, int, bool, int, int)
{
	internal::Log("[gpu] this build has no GPU renderer: the CPU draws the picture");
	return false;
}
bool Active() { return false; }
void Shutdown() {}
int OutWidth() { return 0; }
int OutHeight() { return 0; }
const char* DeviceName() { return ""; }
bool Built() { return false; }
int ShaderCount() { return 1; }
const char* ShaderName(int) { return "Off"; }
const char* ShaderPresetName(int) { return "Off"; }
int FindShader(const char* name) { return name && !strcmp(name, "Off") ? 0 : -1; }
int PassCount(int) { return 0; }
bool ShaderFullscreen(int) { return false; }
bool ShaderDamaged(int) { return false; }
bool Building() { return false; }
double TakeShaderMs() { return 0; }
int ShaderParamCount(int) { return 0; }
bool ShaderParam(int, int, ParamInfo*) { return false; }
void SetShaderParam(int, const char*, float) {}
void ResetShaderParams(int) {}
bool SaveShaderParams() { return true; }
void SetFrame(const Frame&, Rect, int, bool, bool) {}
void SetGame(const uint32_t*, int, int, Rect, int, bool, bool) {}
void HideGame() {}
bool Present(const uint32_t*, Rect, bool) { return false; }
bool SceneAvailable() { return false; }
bool SceneTexture(uint64_t, const uint32_t*, int, int, bool) { return false; }
bool HasSceneTexture(uint64_t) { return false; }
void SetScene(const std::vector<SceneQuad>&) {}
void ClearScene() {}
// 1.1: video
bool VideoAvailable() { return false; }
bool HdrOutputActive() { return false; }
bool VideoFormatSupported(const VideoFrame&) { return false; }
bool SetVideo(const VideoFrame& f, const VideoView&)
{
	if (f.release)
		f.release(f.release_ctx);
	return false;
}
void SetVideoView(const VideoView&) {}
void HideVideo() {}
void ReleaseVideo() {}
size_t HostMemoryAlignment() { return 0; }
HostMemory* ImportHostMemory(void*, size_t) { return nullptr; }
void ReleaseHostMemory(HostMemory*) {}
VideoStats TakeVideoStats() { return VideoStats(); }
} // namespace ps5gpu
