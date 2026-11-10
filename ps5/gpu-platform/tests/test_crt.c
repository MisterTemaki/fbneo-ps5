// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// Host test of the compiler-rt pieces the platform archive carries (platform/tests/host_test.sh): emutls.c, built as
// for the console (its destructor round delayed), behind thread-local variables compiled with -femulated-tls; and
// x86.c's __cpu_model behind __builtin_cpu_supports.
#include <cpuid.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static __thread int g_value = 5;
static __thread char g_block[4096];
static pthread_key_t g_key;
static int g_fail, g_pass;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_seen_in_destructor[8];

#define CHECK(cond, ...)                                                                                              \
	do {                                                                                                              \
		pthread_mutex_lock(&g_lock);                                                                                  \
		if (cond)                                                                                                     \
			g_pass++;                                                                                                 \
		else {                                                                                                        \
			g_fail++;                                                                                                 \
			printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                    \
			printf(__VA_ARGS__);                                                                                      \
			printf("\n");                                                                                             \
		}                                                                                                             \
		pthread_mutex_unlock(&g_lock);                                                                                \
	} while (0)

// a pthread-key destructor that reads a TLS variable, as libc++abi's thread_local destructors do when the C library
// has no __cxa_thread_atexit_impl: emutls must not have dropped the thread's block before it runs
static void key_destructor(void *arg)
{
	int id = (int)(long)arg - 1;
	g_seen_in_destructor[id] = g_value;
}

static void *worker(void *arg)
{
	int id = (int)(long)arg;
	int zero = 1;
	for (size_t i = 0; i < sizeof g_block; i++)
		zero &= g_block[i] == 0;
	CHECK(g_value == 5 && zero, "thread %d starts with the initial values", id);
	g_value = 100 + id;
	memset(g_block, id, sizeof g_block);
	pthread_setspecific(g_key, (void *)(long)(id + 1));   // non-null, or the destructor isn't called
	for (volatile int spin = 0; spin < 100000; spin++)
		;
	CHECK(g_value == 100 + id && g_block[4095] == id, "thread %d keeps its own values", id);
	return NULL;
}

int main(void)
{
	g_value = 1;   // the main thread's TLS block exists before the key: emutls's key comes first
	pthread_key_create(&g_key, key_destructor);
	pthread_t t[8];
	for (long i = 0; i < 8; i++)
		pthread_create(&t[i], NULL, worker, (void *)i);
	for (int i = 0; i < 8; i++)
		pthread_join(t[i], NULL);
	CHECK(g_value == 1, "the main thread's value is its own");
	for (int i = 0; i < 8; i++)
		CHECK(g_seen_in_destructor[i] == 100 + i, "thread %d's TLS still there in a key destructor: %d", i,
			g_seen_in_destructor[i]);

	char vendor[13] = {0};
	unsigned a, b, c, d;
	__get_cpuid(0, &a, &b, &c, &d);
	memcpy(vendor, &b, 4);
	memcpy(vendor + 4, &d, 4);
	memcpy(vendor + 8, &c, 4);
	extern struct {
		unsigned vendor, type, subtype;
		unsigned features[1];
	} __cpu_model;
	unsigned want = !strcmp(vendor, "GenuineIntel") ? 1 : !strcmp(vendor, "AuthenticAMD") ? 2 : 0;
	CHECK(want == 0 || __cpu_model.vendor == want, "__cpu_model's vendor %u for %s", __cpu_model.vendor, vendor);
	CHECK(__builtin_cpu_supports("sse2"), "sse2 on x86-64");
	printf("%s test_crt: %d checks, %d failed\n", g_fail ? "FAIL" : "ok  ", g_pass + g_fail, g_fail);
	return g_fail ? 1 : 0;
}
