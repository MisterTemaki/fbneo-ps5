/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * The console's VideoOut calls (libSceVideoOut) as Mesa's VideoOut WSI (MIT) calls them: an MIT stand-in for the
 * payload SDK fork's header of this name. Values, layout and parameters are those of the WSI's own host model
 * (wsi_common_videoout.c, "#if !defined(__PROSPERO__)"), which models these calls for host builds
 * (platform/radv-shim/README.md). Only what the WSI uses.
 */
#ifndef PS5RT_PS5PLATFORM_VIDEOOUT_H
#define PS5RT_PS5PLATFORM_VIDEOOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5_VIDEO_OUT_USER_SYSTEM 0xff
#define PS5_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SDR UINT64_C(0x8000000000000000)
#define PS5_VIDEO_OUT_TILING_64KB_R_X 0u
/* the buffer attribute block sceVideoOutSetBufferAttribute2 fills */
#define PS5_VIDEO_OUT_ATTRIBUTE_BYTES 80
/* sceVideoOutGetFlipStatus's 64-bit words, and the one holding the shown flip's argument */
#define PS5_VIDEO_OUT_FLIP_STATUS_WORDS 16
#define PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT 3
#define PS5_VIDEO_OUT_FLIP_VSYNC 1
/* output modes for sceVideoOutIsOutputSupported / sceVideoOutConfigureOutput */
#define PS5_VIDEO_OUT_MODE_HIGH_FRAME_RATE 15u
#define PS5_VIDEO_OUT_MODE_RESTORE 1u

struct ps5_video_out_buffer {
	void *data;
	void *metadata;
	void *reserved[2];
};

int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
void sceVideoOutSetBufferAttribute2(void *attribute, uint64_t pixel_format, uint32_t tiling, uint32_t width,
	uint32_t height, uint64_t reserved1, uint32_t reserved2, uint64_t reserved3);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t start, struct ps5_video_out_buffer *buffers,
	int32_t count, void *attribute, int32_t reserved, void *option);
int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode, int64_t argument);
int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[PS5_VIDEO_OUT_FLIP_STATUS_WORDS]);
int sceVideoOutWaitVblank(int32_t handle);
int sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void *a, const void *b, const void *c);
int sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void *a, const void *b, const void *c);

#ifdef __cplusplus
}
#endif

#endif
