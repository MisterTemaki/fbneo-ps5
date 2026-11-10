# GPU image presentation/filtering, with the original software path available through GPU=0.
# SPDX-License-Identifier: MIT
SLANG_GPU := ps5-slang-gpu
SLANG_GPU_LIST := gpu-shaders/presets.txt
SLANG_GPU_RADV_MIT ?= $(abspath ../../rsdeps/radv-mit)
include $(SLANG_GPU)/mk/slang-gpu.mk
$(SLANG_GPU_PRESETS_CPP): $(wildcard gpu-shaders/*.slangp)

ifeq ($(GPU),1)
GPU_HOST_OBJ := $(call SLANG_GPU_OBJECTS,$(HOST_OUT)/gpu,$(HOST_CXX),$(HOST_OPT) $(HOST_SAN),-O2 -g1)
GPU_PS5_OBJ := $(call SLANG_GPU_OBJECTS,$(PS5_OUT)/gpu,$(PS5_CXX),$(PS5_OPT) $(SLANG_GPU_PS5_FLAGS),-O2 -fPIC -march=znver2)
GPU_HOST_LIBS := -lvulkan
$(HOST_OUT)/fbneo-ps5-host: $(GPU_HOST_OBJ)

GPU_PLATFORM_SRC := memstream locale fs qsort_r misc thread
GPU_PLATFORM_OBJ := $(GPU_PLATFORM_SRC:%=build/app/gpu-platform/ps5rt_%.o) \
	build/app/gpu-platform/crt_emutls.o build/app/gpu-platform/crt_x86.o build/app/gpu-ctype.o
build/app/gpu-platform/ps5rt_%.o: gpu-platform/src/ps5rt_%.c gpu-platform/include/ps5rt/platform.h
	@mkdir -p $(@D)
	$(PS5_CC) -std=gnu11 -O2 -fPIC -ffunction-sections -fdata-sections -Igpu-platform/include -c -o $@ $<
build/app/gpu-platform/crt_%.o: gpu-platform/third_party/compiler-rt-18.1.8/%.c
	@mkdir -p $(@D)
	$(PS5_CC) -std=gnu11 -O2 -fPIC -ffunction-sections -fdata-sections -c -o $@ $<
build/app/gpu-ctype.o: coreorbis/orbis-shims/ProsperoGpuCtype.c
	@mkdir -p $(@D)
	$(PS5_CC) -std=gnu11 -O2 -fPIC -Wall -Wextra -Werror -c -o $@ $<
build/app/libgpu-platform.a: $(GPU_PLATFORM_OBJ)
	$(PS5_PAYLOAD_SDK)/bin/llvm-ar rcsD $@ $^
$(PS5_OUT)/port/netfs/netfs_wrap.o: EXTRA_FLAGS += -DNETFS_CHAIN_PS5RT

# RADV embeds zlib. Keep the CPU/host archive intact and remove duplicate zlib members only from the GPU link's copy.
APP_CORE_ARCHIVE := $(PS5_CORE_OUT)/libfbneo-gpu.a
$(APP_CORE_ARCHIVE): $(PS5_CORE_OUT)/libfbneo.a gpu.mk
	cp $< $@
	$(PS5_PAYLOAD_SDK)/bin/llvm-ar d $@ $(notdir $(ZLIB_SRC:.c=.o))

build/app/gpu-radv.ok: $(SLANG_GPU_RADV_ARCHIVE) $(SLANG_GPU_RADV_MIT)/PROVENANCE.txt gpu.mk
	@mkdir -p $(@D)
	grep -q 'MIT ps5platform headers' $(SLANG_GPU_RADV_MIT)/PROVENANCE.txt
	@touch $@
$(SLANG_GPU_RADV_ARCHIVE):
	@echo "Set SLANG_GPU_RADV_MIT to the RADV directory (lib/libvulkan_radeon.ps5.a), or build with GPU=0."
	@exit 1

GPU_LINK_INPUTS = $(GPU_PS5_OBJ) --whole-archive $(SLANG_GPU_RADV_ARCHIVE) --no-whole-archive build/app/libgpu-platform.a
GPU_LINK_FLAGS := --wrap=pthread_create
GPU_IMPORT_STUBS = --gpu $(SDK_LIB)/libSceAgc.so $(SDK_LIB)/libSceAgcDriver.so
GPU_LINK_VERIFY = python3 tools/check_gpu_link.py $@ $(SLANG_GPU_RADV_ARCHIVE)
build/app/fbneo-pie.elf: $(GPU_PS5_OBJ) $(APP_CORE_ARCHIVE) build/app/libgpu-platform.a build/app/gpu-radv.ok tools/check_gpu_link.py

# New GPU integration tests exercise the real video adapter with offscreen Vulkan.
$(HOST_OUT)/test-gpu-video: tests/test_gpu_video.cpp $(HOST_OUT)/port/coreorbis/orbis-shims/ProsperoVideo.o \
	$(HOST_OUT)/port/coreorbis/orbis-shims/ProsperoGpu.o $(HOST_OUT)/port/coreorbis/orbis-shims/ProsperoCrt.o \
	$(HOST_OUT)/port/host/sce_host.o $(HOST_OUT)/port/frontend/fe_text.o $(GPU_HOST_OBJ)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(INCS) -o $@ $^ -lvulkan -lpthread
test-gpu: $(HOST_OUT)/test-gpu-video
	ASAN_OPTIONS=detect_leaks=0 FBNEO_HOST_GPU=1 $(HOST_OUT)/test-gpu-video
test: test-gpu
else
test-gpu:
	@echo "GPU integration tests require GPU=1."
	@exit 1
endif
.PHONY: test-gpu
