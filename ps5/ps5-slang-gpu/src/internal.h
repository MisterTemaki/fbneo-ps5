// ps5-slang-gpu: what the renderer (gpu.cpp) and the preset loader (slang_loader.cpp) share.
//
// SPDX-License-Identifier: MIT
#pragma once
#include "ps5gpu/gpu.h"

#include <cstddef>
#include <functional>
#include <pthread.h>
#include <string>

namespace ps5gpu
{
namespace internal
{
// a printf-like line to Config::log (cut at 1 KiB)
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// every folder of `path` up to its last '/', made as needed
void MakeParentDirs(const std::string& path);

// A worker with a chosen stack. std::thread maps to a pthread with the system's default stack, which is small on the
// console, and ends the program when the system refuses it; RADV's shader compiler (ACO) and glslang's parser recurse
// deep. Every user checks joinable(): a thread the system refused must not be waited for, nor counted on.
class Thread
{
public:
	Thread() = default;
	Thread(const Thread&) = delete;
	Thread& operator=(const Thread&) = delete;
	// fn on a thread with a `stack_bytes` stack (the default stack if that one is refused)
	Thread(std::function<void()> fn, size_t stack_bytes)
	{
		auto* holder = new std::function<void()>(std::move(fn));
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, stack_bytes);
		int r = pthread_create(&m_thread, &attr, &Trampoline, holder);
		pthread_attr_destroy(&attr);
		if (r != 0)
			r = pthread_create(&m_thread, nullptr, &Trampoline, holder);
		if (r != 0)
		{
			delete holder;
			return;
		}
		m_joinable = true;
	}
	Thread(Thread&& o) noexcept : m_thread(o.m_thread), m_joinable(o.m_joinable) { o.m_joinable = false; }
	Thread& operator=(Thread&& o) noexcept
	{
		if (this != &o)
		{
			if (m_joinable)
				pthread_join(m_thread, nullptr);
			m_thread = o.m_thread;
			m_joinable = o.m_joinable;
			o.m_joinable = false;
		}
		return *this;
	}
	~Thread()
	{
		if (m_joinable)
			pthread_join(m_thread, nullptr);
	}
	bool joinable() const { return m_joinable; }
	void join()
	{
		if (m_joinable)
		{
			pthread_join(m_thread, nullptr);
			m_joinable = false;
		}
	}

private:
	static void* Trampoline(void* p)
	{
		auto* fn = static_cast<std::function<void()>*>(p);
		const Config& c = CurrentConfig();
		void* token = c.thread_enter ? c.thread_enter() : nullptr;
		(*fn)();
		if (c.thread_leave)
			c.thread_leave(token);
		delete fn;
		return nullptr;
	}
	pthread_t m_thread{};
	bool m_joinable = false;
};
} // namespace internal
} // namespace ps5gpu
