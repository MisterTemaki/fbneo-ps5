/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * ps5-native-runtime-mit's platform layer: what a RADV title (an eboot with Mesa's RADV linked in) needs from the C
 * library that the PS5's system libc (libSceLibcInternal, libkernel) doesn't give, and that the payload SDK's libc.a has
 * only under the GPL. Each function's behaviour is in its source file; how it was written: platform/CLEANROOM.md.
 *
 * Link: tools/ps5rt-radv-link.sh prints the flags (the --wrap list) and the archive to name before the system stubs.
 */
#ifndef PS5RT_PLATFORM_H
#define PS5RT_PLATFORM_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Threads (src/ps5rt_thread.c, --wrap=pthread_create): a thread created with no attributes (pthread_create(..., NULL,
 * ...), which is how Mesa's threads, std::thread and most libraries make theirs) gets a stack of this many bytes
 * instead of the console's default, which is small. 0 leaves such threads on the default. The default is
 * PS5RT_THREAD_STACK_DEFAULT, 2 MiB (8 MiB, Linux's, until the second M2 review: measured on the PC, RADV's deepest
 * thread used 81 KiB, see README.md); a thread created with attributes keeps the stack they ask for. */
#define PS5RT_THREAD_STACK_DEFAULT ((size_t)2 << 20)
void ps5rt_set_thread_stack_size(size_t bytes);
size_t ps5rt_thread_stack_size(void);

/* open_memstream (src/ps5rt_memstream.c): its buffer and size are brought up to date when the stream is flushed or
 * closed, so fflush and fclose must reach these. The link's --wrap=fclose / --wrap=fflush do that through
 * src/ps5rt_stdio_wrap.c. A program that wraps fclose / fflush itself (a network-share layer, say) links with
 * ps5rt-radv-link.sh --own-stdio-wrap and calls these from its own __wrap_fclose / __wrap_fflush instead of
 * __real_fclose / __real_fflush. */
int ps5rt_fclose(FILE *stream);
int ps5rt_fflush(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
