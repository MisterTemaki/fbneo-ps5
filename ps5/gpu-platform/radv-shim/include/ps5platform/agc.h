/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * The console's AGC calls (libSceAgc, libSceAgcDriver) as RADV's PS5 winsys (Mesa, MIT) calls them: an MIT stand-in
 * for the payload SDK fork's header of this name, written from those call sites and the compiled winsys
 * (platform/radv-shim/README.md). Only what RADV uses.
 */
#ifndef PS5RT_PS5PLATFORM_AGC_H
#define PS5RT_PS5PLATFORM_AGC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the interface version sceAgcInit is given */
#define PS5_AGC_INIT_VERSION 8u

/* a draw command buffer handed to the GPU: its PM4 words and how many (16 bytes) */
struct ps5_agc_submit_description {
	void *words;
	uint32_t word_count;
	uint8_t flags;
	uint8_t reserved[3];
};

int32_t sceAgcInit(uint32_t version);
int32_t sceAgcSuspendPoint(void);
int32_t sceAgcDriverSubmitDcb(struct ps5_agc_submit_description *description);
int32_t sceAgcDriverSetTFRing(uint64_t address, uint32_t size);
int32_t sceAgcDriverSetHsOffchipParam(uint32_t granularity, uint32_t buffering);

#ifdef __cplusplus
}
#endif

#endif
