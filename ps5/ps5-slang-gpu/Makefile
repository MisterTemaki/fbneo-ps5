# SPDX-License-Identifier: MIT
# ps5-slang-gpu: the library's own checks on the host (Linux, any Vulkan driver: lavapipe).
#   make test        the tables generated, the library and glslang built for the host (ASan / UBSan on the library),
#                    then tests/run-tests.sh: every preset compiled and drawing, formats, crop, rotation, the UI
#                    surface, damaged and missing files, the CPU fall-back, the start guard, SHA-256, Fit, the vendored
#                    copy's check
#   make shaders-install DEST=<dir>   the shipped presets' files, LICENSES.md, MANIFEST and SHA256SUMS into <dir>
#                    (what RetroStation's make app does for /app0/shaders/slang)
#   make clean
# A program builds it through mk/slang-gpu.mk (README.md).

SLANG_GPU := .
SLANG_GPU_GEN := build/gen
SLANG_GPU_VENDOR_CHECK := 0
include mk/slang-gpu.mk

HOST_CXX ?= g++
HOST_SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_OBJ := $(call SLANG_GPU_OBJECTS,build/host,$(HOST_CXX),-O1 -g $(HOST_SAN),-O2 -g1)
HOST_STUB_OBJ := $(call SLANG_GPU_STUB_OBJECTS,build/host,$(HOST_CXX),-O1 -g $(HOST_SAN))

build/host/gpu-test: tests/gpu_test.cpp $(HOST_OBJ) $(SLANG_GPU_HEADERS)
	$(HOST_CXX) -std=c++17 -O1 -g $(HOST_SAN) $(SLANG_GPU_WARN) $(SLANG_GPU_INC) -o $@ tests/gpu_test.cpp $(HOST_OBJ) -lpthread -lvulkan
build/host/stub-test: tests/stub_test.cpp $(HOST_STUB_OBJ) $(SLANG_GPU_HEADERS)
	$(HOST_CXX) -std=c++17 -O1 -g $(HOST_SAN) $(SLANG_GPU_WARN) $(SLANG_GPU_INC) -o $@ tests/stub_test.cpp $(HOST_STUB_OBJ) -lpthread

test: build/host/gpu-test build/host/stub-test
	bash tests/run-tests.sh

shaders-install:
	@test -n "$(DEST)" || { echo "make shaders-install DEST=<dir>"; exit 1; }
	$(SLANG_GPU_PYTHON) tools/shader_manifest.py $(SLANG_GPU_LIST) build/shaders-check.cpp --install $(DEST)
	$(SLANG_GPU_PYTHON) tools/shader_manifest.py $(SLANG_GPU_LIST) build/shaders-check.cpp --check-folder $(DEST)

clean:
	rm -rf build

.PHONY: test shaders-install clean
.SECONDARY:
