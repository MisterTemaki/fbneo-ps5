// FBNeo PS5 2.3: the CRT shaders' thread pool (coreorbis/orbis-shims/ProsperoCrt.cpp) with a thread limit, as the
// picture thread of a game paced by the sound sets it (4): a pool of 6 (FBNEO_HOST_SHADER_THREADS, the console's
// size) draws the same picture with every limit, and with a limit For() waits only for the workers it uses (the
// others are not woken for it, and don't answer).
//
//   test_crt_pool
//
// SPDX-License-Identifier: MIT

#include "ProsperoCrt.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// (ProsperoThread.h's threads arm the crash log; not linked here)
namespace crashlog
{
void* ArmThread()
{
	return nullptr;
}
void DisarmThread(void*)
{
}
} // namespace crashlog

static int g_passed = 0, g_failed = 0;
static void Check(bool ok, const std::string& what)
{
	(ok ? g_passed : g_failed)++;
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what.c_str());
}

int main()
{
	setenv("FBNEO_HOST_SHADER_THREADS", "6", 1);
	const int w = 496, h = 384, dw = 1440, dh = 1080;
	std::vector<uint32_t> frame(size_t(w) * h);
	uint32_t seed = 2463534242u;
	for (uint32_t& p : frame)
	{
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		p = 0xff000000u | (seed & 0xffffff);
	}
	auto draw = [&](int limit, double* ms) {
		ps5crt::SetThreadLimit(limit);
		std::vector<uint32_t> out(size_t(dw) * dh);
		const auto t0 = std::chrono::steady_clock::now();
		ps5crt::Render(ps5crt::Shader::EasymodeStyle, frame.data(), w, h, out.data(), dw, 0, 0, dw, dh, 7);
		if (ms)
			*ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		ps5crt::SetThreadLimit(0);
		return out;
	};
	const std::vector<uint32_t> all = draw(0, nullptr), four = draw(4, nullptr), one = draw(1, nullptr);
	Check(all == four && all == one, "the same picture on 6 threads, on 4 (the limit) and on 1");

	// the answers For() waited for: 3 workers a call with the limit 4, 5 without
	uint64_t f0, a0, f1, a1;
	ps5crt::HostPoolStats(&f0, &a0);
	double t4 = 0, t6 = 0, ms;
	for (int i = 0; i < 8; i++)
	{
		draw(4, &ms);
		t4 += ms;
	}
	ps5crt::HostPoolStats(&f1, &a1);
	const uint64_t fors4 = f1 - f0, answers4 = a1 - a0;
	Check(fors4 > 0 && answers4 == 3 * fors4,
		"limit 4 of 6: each For() waits for 3 workers (" + std::to_string(answers4) + " answers, " + std::to_string(fors4) +
			" calls)");
	for (int i = 0; i < 8; i++)
	{
		draw(0, &ms);
		t6 += ms;
	}
	uint64_t f2, a2;
	ps5crt::HostPoolStats(&f2, &a2);
	Check(f2 - f1 > 0 && a2 - a1 == 5 * (f2 - f1), "no limit: each For() waits for the 5 workers");
	printf("  (CRT Easymode style 1440x1080 on this machine: %.1f ms a picture limited to 4, %.1f ms on 6)\n", t4 / 8, t6 / 8);
	printf("crt pool test: %d checks ok, %d failed\n", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
