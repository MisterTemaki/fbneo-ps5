// Thread-pool startup must survive allocation failures without waiting on sleeping workers.
// SPDX-License-Identifier: MIT
#include "fe_pool.h"
#include "../coreorbis/orbis-shims/ProsperoCrt.cpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

static std::atomic<int> fail_after{-1};
extern "C" void* __real__Znwm(size_t);
extern "C" void* __wrap__Znwm(size_t bytes)
{
	int left = fail_after.load();
	if (left > 0 && fail_after.fetch_sub(1) == 1)
	{
		fail_after = -1;
		throw std::bad_alloc();
	}
	return __real__Znwm(bytes);
}
namespace crashlog
{
void* ArmThread() { return nullptr; }
void DisarmThread(void*) {}
}
int main(int argc, char** argv)
{
	const int allocation = argc > 1 ? atoi(argv[1]) : -1;
	const bool crt = argc > 2;
	fail_after = allocation;
	try
	{
		if (crt)
		{
			auto& pool = ps5crt::Pool::Get();
			fail_after = -1;
			std::atomic<int> visits[31]{};
			pool.For(31, [&](int first, int last) { for (int i = first; i < last; ++i) visits[i]++; });
			for (const auto& n : visits) if (n != 1) return 1;
			printf("PASS CRT allocation %d: all rows completed exactly once\n", allocation);
			return 0;
		}
		fe::PartPool pool(4);
		fail_after = -1;
		std::atomic<int> visits[31]{};
		pool.Run([](void* context, int i) { static_cast<std::atomic<int>*>(context)[i]++; }, visits, 31);
		for (const auto& n : visits) if (n != 1) return 1;
		printf("PASS allocation %d: all parts completed exactly once on %d thread(s)\n", allocation, pool.Threads());
	}
	catch (const std::bad_alloc&)
	{
		fail_after = -1;
		fprintf(stderr, "FAIL pool did not fall back after allocation %d\n", allocation);
		return 1;
	}
}
