# SPDX-License-Identifier: MIT
# ps5-slang-gpu: the make fragment a program includes (README.md, "Building it into a program").
#
# Set before the include:
#   SLANG_GPU          the vendored copy (default ps5-slang-gpu, beside the Makefile)
#   SLANG_GPU_GEN      where the generated tables go (default build/gen/slang-gpu)
#   SLANG_GPU_LIST     the preset list (default the library's shaders/presets.txt: every program the same 26 presets)
#   SLANG_GPU_LICENCE_CHECK  1 (default): every built-in preset's files must be permissive (tools/shader_licences.py
#                      --check); 0 only for a list that isn't the library's (genplus's standalone list)
#   SLANG_GPU_VENDOR_CHECK   1 (default): the copy must be the files VENDOR.txt names (tools/check_vendor.py); 0 in
#                      the library's own checkout
#   SLANG_GPU_MITRT    the program's ps5-native-runtime-mit copy (default mitrt): the RADV link's flags and libc.a list
#   SLANG_GPU_RADV_MIT RADV built against mitrt's MIT ps5platform headers (default ../../rsdeps/radv-mit from the
#                      program's ps5/ folder)
#
# It gives:
#   SLANG_GPU_SRC, SLANG_GPU_STUB_SRC, SLANG_GPU_GLSLANG_SRC   the sources (the stub: a CPU-only build)
#   SLANG_GPU_INC                       -I for ps5gpu/gpu.h
#   SLANG_GPU_TABLES                    the two generated tables (built-in presets; the shipped presets' manifest)
#   SLANG_GPU_VIDEO_SPV                 1.1: the video pass's SPIR-V header (glslangValidator, from shaders/video.*)
#   SLANG_GPU_VENDOR_OK                 a stamp: the copy checked (every object and table depends on it)
#   $(call SLANG_GPU_OBJECTS,<dir>,<c++>,<flags>,<glslang flags>)  the GPU build's objects under <dir>, rules defined
#   $(call SLANG_GPU_STUB_OBJECTS,<dir>,<c++>,<flags>)              the CPU-only build's (common.cpp + stub.cpp)
#   SLANG_GPU_PS5_FLAGS                 -isystem RADV's Vulkan headers (the PS5 build's <flags>)
#   SLANG_GPU_RADV_ARCHIVE              RADV's archive, to link
#   SLANG_GPU_LINK_FLAGS                the platform layer's linker flags (SLANG_GPU_LINK_OPTS: e.g. --own-stdio-wrap)
#   SLANG_GPU_IMPORT_ALLOW              check_imports.py's --allow for the modules RADV needs (libSceAgc, AgcDriver)
#   SLANG_GPU_LIBC_ALLOWED              the SDK libc.a members a RADV link may take (mitrt/platform/libc-allow.txt)
#   SLANG_GPU_NOTICES                   "name=file" pairs for the program's NOTICES.txt (the library, glslang, stb_image)
#   $(call SLANG_GPU_LINK_CHECKS,<trace>,<elf>)       the GPU link's checks (recipe lines): RADV's provenance and
#                                       archive, no GPL input, the platform's locale, _DefaultRuneLocale ours
#   $(call SLANG_GPU_NO_SHADER_CODE,<binary>...)      no GPL shader text in the binaries (a recipe line)
#   $(call SLANG_GPU_MANIFEST_ID,<file>)              the manifest's id into <file> (shaders-manifest.txt, beside the eboot)

SLANG_GPU ?= ps5-slang-gpu
SLANG_GPU_GEN ?= build/gen/slang-gpu
SLANG_GPU_LIST ?= $(SLANG_GPU)/shaders/presets.txt
SLANG_GPU_LICENCE_CHECK ?= 1
SLANG_GPU_VENDOR_CHECK ?= 1
SLANG_GPU_MITRT ?= mitrt
SLANG_GPU_RADV_MIT ?= $(abspath ../../rsdeps/radv-mit)
SLANG_GPU_LINK_OPTS ?=
SLANG_GPU_PYTHON ?= python3 -B

SLANG_GPU_SRC := $(SLANG_GPU)/src/common.cpp $(SLANG_GPU)/src/gpu.cpp $(SLANG_GPU)/src/slang_loader.cpp $(SLANG_GPU)/src/sha256.cpp
SLANG_GPU_STUB_SRC := $(SLANG_GPU)/src/common.cpp $(SLANG_GPU)/src/stub.cpp
SLANG_GPU_HEADERS := $(wildcard $(SLANG_GPU)/include/ps5gpu/*.h $(SLANG_GPU)/src/*.h)
SLANG_GPU_INC := -I$(SLANG_GPU)/include
# glslang 15.1.0 (third_party/glslang/PROVENANCE.txt): the GLSL front end and SPIR-V back end, no HLSL, no SPIRV-Tools
SLANG_GPU_GLSLANG := $(SLANG_GPU)/third_party/glslang
SLANG_GPU_GLSLANG_SRC := $(sort $(wildcard $(SLANG_GPU_GLSLANG)/glslang/MachineIndependent/*.cpp \
	$(SLANG_GPU_GLSLANG)/glslang/MachineIndependent/preprocessor/*.cpp $(SLANG_GPU_GLSLANG)/glslang/GenericCodeGen/*.cpp)) \
	$(SLANG_GPU_GLSLANG)/glslang/OSDependent/Unix/ossource.cpp $(SLANG_GPU_GLSLANG)/glslang/ResourceLimits/ResourceLimits.cpp \
	$(addprefix $(SLANG_GPU_GLSLANG)/SPIRV/,GlslangToSpv.cpp SpvBuilder.cpp SpvPostProcess.cpp InReadableOrder.cpp Logger.cpp \
	SpvTools.cpp disassemble.cpp doc.cpp)
SLANG_GPU_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers

# ---- the copy, checked -------------------------------------------------------------------------------------------------
SLANG_GPU_VENDOR_OK := $(SLANG_GPU_GEN)/vendor.ok
ifeq ($(SLANG_GPU_VENDOR_CHECK),1)
$(SLANG_GPU_VENDOR_OK): $(SLANG_GPU)/VENDOR.txt $(shell find $(SLANG_GPU) -type f 2>/dev/null)
	@mkdir -p $(dir $@)
	$(SLANG_GPU_PYTHON) $(SLANG_GPU)/tools/check_vendor.py $(SLANG_GPU)
	@touch $@
else
$(SLANG_GPU_VENDOR_OK):
	@mkdir -p $(dir $@)
	@touch $@
endif

# ---- the tables (host tools: python3, glslangValidator 15.1.0) ---------------------------------------------------------
SLANG_GPU_PRESETS_CPP := $(SLANG_GPU_GEN)/slang_presets.cpp
SLANG_GPU_FILES_CPP := $(SLANG_GPU_GEN)/slang_files.cpp
SLANG_GPU_TABLES := $(SLANG_GPU_PRESETS_CPP) $(SLANG_GPU_FILES_CPP)
SLANG_GPU_TOOLS := $(SLANG_GPU)/tools/slang_compile.py $(SLANG_GPU)/tools/shader_licences.py $(SLANG_GPU)/tools/shader_manifest.py
SLANG_GPU_LIST_DIR := $(dir $(SLANG_GPU_LIST))
$(SLANG_GPU_PRESETS_CPP): $(SLANG_GPU_LIST) $(SLANG_GPU_TOOLS) $(SLANG_GPU_VENDOR_OK) \
		$(wildcard $(SLANG_GPU_LIST_DIR)*.slang $(SLANG_GPU_LIST_DIR)own/*.slangp)
	@mkdir -p $(dir $@)
	$(if $(filter 1,$(SLANG_GPU_LICENCE_CHECK)),$(SLANG_GPU_PYTHON) $(SLANG_GPU)/tools/shader_licences.py $(SLANG_GPU_LIST) --check >/dev/null)
	$(SLANG_GPU_PYTHON) $(SLANG_GPU)/tools/slang_compile.py $@.part --list $(SLANG_GPU_LIST)
	mv $@.part $@
$(SLANG_GPU_FILES_CPP): $(SLANG_GPU_LIST) $(SLANG_GPU_TOOLS) $(SLANG_GPU_VENDOR_OK) $(SLANG_GPU_LIST_DIR)slang/REVISION
	@mkdir -p $(dir $@)
	$(SLANG_GPU_PYTHON) $(SLANG_GPU)/tools/shader_manifest.py $(SLANG_GPU_LIST) $@
# 1.1: the video pass's SPIR-V (shaders/video.vert, video.frag: the library's own, MIT), for gpu.cpp
SLANG_GPU_VIDEO_SPV := $(SLANG_GPU_GEN)/video_spv.h
SLANG_GPU_GLSLANG_VALIDATOR ?= glslangValidator
$(SLANG_GPU_VIDEO_SPV): $(SLANG_GPU)/shaders/video.vert $(SLANG_GPU)/shaders/video.frag $(SLANG_GPU_VENDOR_OK)
	@mkdir -p $(dir $@)
	$(SLANG_GPU_GLSLANG_VALIDATOR) -V --target-env vulkan1.0 --vn kVideoVs -o $@.vs $(SLANG_GPU)/shaders/video.vert >/dev/null
	$(SLANG_GPU_GLSLANG_VALIDATOR) -V --target-env vulkan1.0 --vn kVideoFs -o $@.fs $(SLANG_GPU)/shaders/video.frag >/dev/null
	cat $@.vs $@.fs >$@.part && rm -f $@.vs $@.fs && mv $@.part $@

# ---- objects ------------------------------------------------------------------------------------------------------------
# $(1) the folder, $(2) the C++ compiler, $(3) the library's flags (optimisation, -fPIC, -march, sanitizers, the Vulkan
# headers' -isystem), $(4) glslang's and the tables' (no sanitizer needed)
define SLANG_GPU_RULES
$(1)/lib/%.o: $(SLANG_GPU)/src/%.cpp $(SLANG_GPU_HEADERS) $(SLANG_GPU_VENDOR_OK) $(SLANG_GPU_VIDEO_SPV)
	@mkdir -p $$(dir $$@)
	$(2) -std=c++17 $(3) $(SLANG_GPU_WARN) $(SLANG_GPU_INC) -I$(SLANG_GPU_GLSLANG) -I$(SLANG_GPU_GEN) -c -o $$@ $$<
$(1)/glslang/%.o: $(SLANG_GPU_GLSLANG)/%.cpp $(SLANG_GPU_VENDOR_OK)
	@mkdir -p $$(dir $$@)
	$(2) -std=c++17 $(4) -I$(SLANG_GPU_GLSLANG) -w -c -o $$@ $$<
$(1)/tables/slang_presets.o: $(SLANG_GPU_PRESETS_CPP) $(SLANG_GPU)/include/ps5gpu/slang.h
	@mkdir -p $$(dir $$@)
	$(2) -std=c++17 $(4) -O0 $(SLANG_GPU_INC) -c -o $$@ $$<
$(1)/tables/slang_files.o: $(SLANG_GPU_FILES_CPP) $(SLANG_GPU)/include/ps5gpu/slang.h
	@mkdir -p $$(dir $$@)
	$(2) -std=c++17 $(4) -O1 $(SLANG_GPU_INC) -c -o $$@ $$<
endef
SLANG_GPU_OBJECTS = $(eval $(call SLANG_GPU_RULES,$(1),$(2),$(3),$(4)))$(patsubst $(SLANG_GPU)/src/%.cpp,$(1)/lib/%.o,$(SLANG_GPU_SRC)) \
	$(patsubst $(SLANG_GPU_GLSLANG)/%.cpp,$(1)/glslang/%.o,$(SLANG_GPU_GLSLANG_SRC)) $(1)/tables/slang_presets.o $(1)/tables/slang_files.o
define SLANG_GPU_STUB_RULES
$(1)/stub/%.o: $(SLANG_GPU)/src/%.cpp $(SLANG_GPU_HEADERS) $(SLANG_GPU_VENDOR_OK)
	@mkdir -p $$(dir $$@)
	$(2) -std=c++17 $(3) $(SLANG_GPU_WARN) $(SLANG_GPU_INC) -c -o $$@ $$<
endef
SLANG_GPU_STUB_OBJECTS = $(eval $(call SLANG_GPU_STUB_RULES,$(1),$(2),$(3)))$(patsubst $(SLANG_GPU)/src/%.cpp,$(1)/stub/%.o,$(SLANG_GPU_STUB_SRC))

# ---- the PS5 link ------------------------------------------------------------------------------------------------------
SLANG_GPU_RADV_ARCHIVE := $(SLANG_GPU_RADV_MIT)/lib/libvulkan_radeon.ps5.a
SLANG_GPU_PS5_FLAGS := -isystem $(SLANG_GPU_RADV_MIT)/include
SLANG_GPU_LINK_FLAGS = $(shell sh $(SLANG_GPU_MITRT)/platform/tools/ps5rt-radv-link.sh --flags $(SLANG_GPU_LINK_OPTS))
SLANG_GPU_IMPORT_ALLOW := --allow libSceAgc.sprx --allow libSceAgcDriver.sprx
SLANG_GPU_LIBC_ALLOWED = $(shell sed -n 's/^\([A-Za-z0-9_]*\.o\)[ \t].*/\1/p' $(SLANG_GPU_MITRT)/platform/libc-allow.txt)
SLANG_GPU_NOTICES := "ps5-slang-gpu: the GPU renderer (MIT)=$(SLANG_GPU)/LICENSE" \
	"glslang (BSD-3-Clause and others)=$(SLANG_GPU_GLSLANG)/LICENSE.txt" \
	"stb_image 2.30 in ps5-slang-gpu (public domain, or MIT)=$(SLANG_GPU)/third_party/stb/LICENSE"

# $(1) the link's --trace output, $(2) the linked PIE. Each line fails the recipe (and removes the PIE) when its check does.
define SLANG_GPU_LINK_CHECKS
@grep -q "MIT ps5platform headers" $(SLANG_GPU_RADV_MIT)/PROVENANCE.txt || { echo "$(SLANG_GPU_RADV_ARCHIVE): its PROVENANCE.txt doesn't say it was built against the MIT ps5platform headers"; rm -f $(2); exit 1; }
@test -s $(1) || { echo "$(2): no trace $(1): the GPU link's checks can't run"; rm -f $(2); exit 1; }
@if grep -E '(^|/)proto/native/|(^|/)app_c(rt|pp_runtime)\.o|libps5platform|PS5_Vulkan/|PS5_PayloadSDK' $(1) | grep -v ': '; then echo "$(2): a GPL input (the boilerplate, PS5_Vulkan or the payload SDK fork) is linked in"; rm -f $(2); exit 1; fi
@if grep -v ': ' $(1) | grep 'libvulkan_radeon\.ps5\.a(' | grep -vF '$(SLANG_GPU_RADV_ARCHIVE)('; then echo "$(2): RADV from another archive than $(SLANG_GPU_RADV_ARCHIVE)"; rm -f $(2); exit 1; fi
@grep -q 'libps5rt_platform\.a(ps5rt_locale\.o)$$' $(1) || { echo "$(2): the platform layer's locale functions are not the ones linked (libc++'s iostreams, glslang)"; rm -f $(2); exit 1; }
@grep -q '/lib/gpu\.o$$' $(1) && grep -q '/lib/slang_loader\.o$$' $(1) || { echo "$(2): ps5-slang-gpu's renderer is not in the link"; rm -f $(2); exit 1; }
@$(or $(LLVM_NM),llvm-nm) -D $(2) >$(2).dynsym && test -s $(2).dynsym || { echo "$(2): llvm-nm -D gave no symbols: the _DefaultRuneLocale check can't run"; rm -f $(2) $(2).dynsym; exit 1; }
@if grep -E ' U _DefaultRuneLocale$$' $(2).dynsym; then echo "$(2): _DefaultRuneLocale comes from the system: mitrt's ps5rt_locale.o must define it"; rm -f $(2) $(2).dynsym; exit 1; fi; rm -f $(2).dynsym
endef

# $(1)... the binaries (the PIE, the signed eboot.bin)
SLANG_GPU_NO_SHADER_CODE = $(SLANG_GPU_PYTHON) $(SLANG_GPU)/tools/check_no_shader_code.py $(SLANG_GPU_LIST) $(1)
# the manifest's id (what the eboot carries as "rs-shaders-manifest=<id>") into $(1)
SLANG_GPU_MANIFEST_ID = $(SLANG_GPU_PYTHON) $(SLANG_GPU)/tools/shader_manifest.py $(SLANG_GPU_LIST) $(1).cpp --id-file $(1) >/dev/null && rm -f $(1).cpp
