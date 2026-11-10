// Model a legal consumer drain exactly after Free() returns to the producer.
// SPDX-License-Identifier: MIT
#include "../coreorbis/orbis-shims/ProsperoAudio.cpp"
#include <vector>
#include <cstdio>
static bool drain_on_free = false;
extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_enter(void*, void*) {}
extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* function, void*)
{
	if (drain_on_free && function == reinterpret_cast<void*>(&ps5audio::Free))
	{
		drain_on_free = false;
		// Free() has returned 384. A consumer may drain a grain before Push uses that result.
		ps5audio::g->tail.store(ps5audio::kGrain, std::memory_order_release);
	}
}
int main()
{
	ps5audio::g = new ps5audio::State();
	ps5audio::g->head = ps5audio::kCapacity - 384;
	std::vector<int16_t> samples(512 * 2, 123);
	drain_on_free = true;
	const int written = ps5audio::Push(samples.data(), 512);
	bool safe = written == 384 && !drain_on_free && ps5audio::g->head == ps5audio::kCapacity;
	for (int i = 0; i < written * 2 && safe; ++i)
		safe = ps5audio::g->ring[(ps5audio::kCapacity - 384) * 2 + i] == 123;
	delete ps5audio::g; ps5audio::g = nullptr;
	printf("%s concurrent drain never makes Push consume more than its input (%d frames)\n", safe ? "PASS" : "FAIL", written);
	return safe ? 0 : 1;
}
