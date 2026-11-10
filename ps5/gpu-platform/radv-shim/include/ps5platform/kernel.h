/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * The console kernel's direct-memory and mapping calls, as RADV's PS5 winsys and VideoOut WSI (Mesa, MIT) call them:
 * an MIT stand-in for the payload SDK fork's header of this name, written from those call sites and the console's
 * observed behaviour (platform/radv-shim/README.md says where each value comes from). Only what RADV uses.
 */
#ifndef PS5RT_PS5PLATFORM_KERNEL_H
#define PS5RT_PS5PLATFORM_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* protection bits: the CPU's are FreeBSD's PROT_READ / PROT_WRITE, the GPU's above them */
#define PS5_KERNEL_PROT_CPU_READ 0x01
#define PS5_KERNEL_PROT_CPU_WRITE 0x02
#define PS5_KERNEL_PROT_GPU_READ 0x10
#define PS5_KERNEL_PROT_GPU_WRITE 0x20

/* a mapping at exactly the address given */
#define PS5_KERNEL_MAP_FIXED 0x10

/* the page the kernel maps in (16 KiB) and the least alignment of a direct-memory allocation (64 KiB) */
#define PS5_KERNEL_PAGE_SIZE 0x4000
#define PS5_KERNEL_DIRECT_ALIGNMENT 0x10000

/* the direct-memory type VideoOut's framebuffers are allocated with */
#define PS5_KERNEL_DIRECT_TYPE_CPU 12

/* Each returns 0 or the kernel's error code (0x8002xxxx). */
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment,
	int64_t *start_out, size_t *size_out);
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length, size_t alignment,
	int32_t type, int64_t *physical_out);
int32_t sceKernelReleaseDirectMemory(int64_t physical, size_t length);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int32_t protection, int32_t flags, int64_t physical,
	size_t alignment);
int32_t sceKernelReserveVirtualRange(void **address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMprotect(const void *address, size_t length, int32_t protection);
int32_t sceKernelMunmap(void *address, size_t length);

#ifdef __cplusplus
}
#endif

#endif
