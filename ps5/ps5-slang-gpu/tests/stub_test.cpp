// ps5-slang-gpu: the CPU-only build's stand-in (src/stub.cpp): Init false, the menu "Off" only, every call harmless.
// SPDX-License-Identifier: MIT
#include "ps5gpu/gpu.h"

#include <cstdio>
#include <cstring>

int main()
{
	int fail = 0;
	auto check = [&](bool ok, const char* what) {
		printf("%s %s\n", ok ? "ok  " : "FAIL", what);
		fail += !ok;
	};
	ps5gpu::Config c;
	ps5gpu::Configure(c);
	uint32_t px[4] = {};
	ps5gpu::SetGame(px, 2, 2, {0, 0, 2, 2}, 3, false, false);
	check(!ps5gpu::Init(1920, 1080, false, 0, 0) && !ps5gpu::Active() && !ps5gpu::Built(), "the stub: Init false, not built");
	check(ps5gpu::ShaderCount() == 1 && !strcmp(ps5gpu::ShaderName(0), "Off") && ps5gpu::FindShader("Off") == 0 &&
			  ps5gpu::FindShader("crt-lottes") == -1 && !ps5gpu::Present(px, {0, 0, 1, 1}, true) && ps5gpu::SaveShaderParams(),
		"the stub: the menu is Off alone, Present false, nothing to save");
	const ps5gpu::Rect r = ps5gpu::Fit(320, 224, 4.0 / 3.0, {0, 0, 1920, 1080}, false);
	check(r.x == 240 && r.w == 1440 && r.h == 1080, "the stub build has Fit too");
	return fail;
}
