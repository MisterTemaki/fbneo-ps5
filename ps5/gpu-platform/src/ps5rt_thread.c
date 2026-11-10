/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * pthread_create with a usable stack for threads made without attributes (linked with --wrap=pthread_create).
 *
 * The console's default thread stack is small (seen by the ports: their workers set their own stacks, see
 * coreorbis/orbis-shims/ProsperoThread.h in fbneo-ps5 or snes9x-ps5). Mesa's threads (c11 thrd_create), std::thread and
 * most libraries create theirs with no attributes and expect a stack like Linux's (8 MiB), where Mesa is developed and
 * tested. Such a thread gets ps5rt_thread_stack_size() bytes instead (2 MiB unless the program says otherwise, 25 times
 * the most RADV was measured to use: README.md), unless the default is already at least that. If
 * the console refuses that stack (EAGAIN, ENOMEM), the thread is created on the default stack after all. A thread
 * created with attributes keeps whatever they say.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#include "ps5rt/platform.h"

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);

static _Atomic size_t g_stack_bytes = PS5RT_THREAD_STACK_DEFAULT;

void ps5rt_set_thread_stack_size(size_t bytes)
{
	atomic_store_explicit(&g_stack_bytes, bytes, memory_order_relaxed);
}

size_t ps5rt_thread_stack_size(void)
{
	return atomic_load_explicit(&g_stack_bytes, memory_order_relaxed);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
	size_t want = ps5rt_thread_stack_size();
	if (attr || want == 0)
		return __real_pthread_create(thread, attr, start, arg);

	pthread_attr_t own;
	if (pthread_attr_init(&own) != 0)
		return __real_pthread_create(thread, NULL, start, arg);
	size_t current = 0;
	int rc;
	if ((pthread_attr_getstacksize(&own, &current) == 0 && current >= want) ||
		pthread_attr_setstacksize(&own, want) != 0) {
		rc = __real_pthread_create(thread, NULL, start, arg);
	} else {
		rc = __real_pthread_create(thread, &own, start, arg);
		if (rc == EAGAIN || rc == ENOMEM)
			rc = __real_pthread_create(thread, NULL, start, arg);
	}
	pthread_attr_destroy(&own);
	return rc;
}
