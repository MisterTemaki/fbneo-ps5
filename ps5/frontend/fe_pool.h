// FBNeo PS5 2.3: the core's worker threads (burn::SetParallel; the core's burn_parallel.h): the parts of a driver's
// work (Sega Model 2's 3D, by bands of lines) handed out one at a time to the workers and to the caller, which waits
// for them all. Only the threads that really started count: with none, the caller does every part (2.2's rule for
// every BigThread user: a refused thread is never waited for).
//
// SPDX-License-Identifier: MIT
#pragma once

#include "ProsperoThread.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <vector>

namespace fe
{
class PartPool
{
public:
	typedef void (*Job)(void* ctx, int part);

	explicit PartPool(int workers)
	{
		if (workers <= 0) return;
		// Reserve before starting workers: vector growth throwing with a live worker would join a thread
		// still waiting for its first job. Failed allocations fall back to the threads already started.
		try { m_threads.reserve(size_t(workers)); }
		catch (const std::bad_alloc&) { return; }
		for (int i = 0; i < workers; i++)
		{
			try
			{
				ps5::BigThread t([this] { Worker(); }, 256 * 1024);
				if (!t.joinable()) break;
				m_threads.push_back(std::move(t));
			}
			catch (const std::bad_alloc&) { break; }
		}
	}
	~PartPool()
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_quit = true;
			m_gen++;
		}
		m_cv.notify_all();
		for (auto& t : m_threads)
			t.join();
	}
	PartPool(const PartPool&) = delete;
	PartPool& operator=(const PartPool&) = delete;

	int Threads() const { return 1 + int(m_threads.size()); }

	// job(ctx, i) for each i < parts; returns when all are done
	void Run(Job job, void* ctx, int parts)
	{
		if (m_threads.empty() || parts <= 1)
		{
			for (int i = 0; i < parts; i++)
				job(ctx, i);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_job = job;
			m_ctx = ctx;
			m_parts = parts;
			m_next.store(0);
			m_busy = int(m_threads.size());
			m_gen++;
		}
		m_cv.notify_all();
		Work(job, ctx, parts);
		std::unique_lock<std::mutex> lock(m_lock);
		m_done.wait(lock, [this] { return m_busy == 0; });
		m_job = nullptr;
	}

private:
	void Work(Job job, void* ctx, int parts)
	{
		for (;;)
		{
			const int i = m_next.fetch_add(1);
			if (i >= parts)
				return;
			job(ctx, i);
		}
	}
	void Worker()
	{
		uint64_t seen = 0;
		for (;;)
		{
			Job job;
			void* ctx;
			int parts;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_cv.wait(lock, [&] { return m_gen != seen; });
				seen = m_gen;
				if (m_quit)
					return;
				job = m_job;
				ctx = m_ctx;
				parts = m_parts;
			}
			if (job)
				Work(job, ctx, parts);
			{
				std::lock_guard<std::mutex> lock(m_lock);
				if (--m_busy == 0)
					m_done.notify_one();
			}
		}
	}

	std::vector<ps5::BigThread> m_threads;
	std::mutex m_lock;
	std::condition_variable m_cv, m_done;
	uint64_t m_gen = 0;
	bool m_quit = false;
	Job m_job = nullptr;
	void* m_ctx = nullptr;
	int m_parts = 0;
	int m_busy = 0;
	std::atomic<int> m_next{0};
};
} // namespace fe
