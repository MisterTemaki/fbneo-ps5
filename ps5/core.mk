# FBNeo PS5: FBNeo's core (src/burn, src/cpu, the libraries it uses), arcade drivers only, as FBNeo's own makefiles
# build it (makefile.burn_rules, makefile.sdl2), for the PS5 or the host. Included by the Makefile.
#
#   $(call core_rules,<out dir>,<C compiler>,<C++ compiler>,<flags>)   -> <out dir>/libfbneo.a
#
# SPDX-License-Identifier: MIT

ROOT := ..
FBNEO_SRC := $(shell python3 tools/fbneo_sources.py $(ROOT) sources)
# zlib (FBNeo's copy): the core's states (and the frontend's zips)
ZLIB_SRC := $(addprefix src/dep/libs/zlib/,adler32.c compress.c crc32.c deflate.c inffast.c inflate.c inftrees.c \
	trees.c uncompr.c zutil.c)
CORE_ALL_SRC := $(FBNEO_SRC) $(ZLIB_SRC)

CORE_DIRS := $(sort $(dir $(FBNEO_SRC))) src/burn/devices src/burn/snd src/cpu src/dep/libs/zlib src/intf/cd \
	src/dep/libs/libchdr/include src/dep/libs/lzma/include src/intf/audio src/burner
CORE_INCS = -Icore -I$(GEN_DIR) $(addprefix -I$(ROOT)/,$(CORE_DIRS))
# makefile.sdl2's defines for a little-endian, 64-bit build (no x86 assembly, no 7-Zip, no fastcall)
CORE_DEFS := -DUSE_SPEEDHACKS -DLSB_FIRST -D__fastcall= -DNO_VIZ -D_LARGEFILE64_SOURCE=0 -D_FILE_OFFSET_BITS=64 \
	-DHAVE_UNISTD_H -DBUILD_PS5 -DCHDR_SYSTEM_ZLIB -DZ7_ST -D_7ZIP_ST
CORE_CXXFLAGS := -std=gnu++11 -fno-strict-aliasing -fsigned-char -w -Wno-write-strings
CORE_CFLAGS := -std=gnu99 -fno-strict-aliasing -fsigned-char -w

GEN_DIR := build/generated
GEN_STAMP := $(GEN_DIR)/.stamp
$(GEN_STAMP): tools/gen_headers.sh tools/fbneo_sources.py $(ROOT)/makefile.burn_rules
	sh tools/gen_headers.sh $(ROOT) $(GEN_DIR) $(HOST_CC_GEN) $(HOST_CXX_GEN)
	touch $@
HOST_CC_GEN ?= cc
HOST_CXX_GEN ?= c++

# $(1) out dir, $(2) CC, $(3) CXX, $(4) flags
define core_rules
$(1)/%.o: $(ROOT)/%.cpp | $(GEN_STAMP)
	@mkdir -p $$(dir $$@)
	@$(3) $(4) $(CORE_CXXFLAGS) $(CORE_DEFS) $(CORE_INCS) -c -o $$@ $$<
$(1)/%.o: $(ROOT)/%.c | $(GEN_STAMP)
	@mkdir -p $$(dir $$@)
	@$(2) $(4) $(CORE_CFLAGS) $(CORE_DEFS) $(CORE_INCS) -c -o $$@ $$<
$(1)/m68kops.o: $(GEN_STAMP)
	@$(2) $(4) $(CORE_CFLAGS) $(CORE_DEFS) $(CORE_INCS) -c -o $$@ $(GEN_DIR)/m68kops.c
$(1)/libfbneo.a: $(patsubst %.c,$(1)/%.o,$(patsubst %.cpp,$(1)/%.o,$(CORE_ALL_SRC))) $(1)/m68kops.o
	@rm -f $$@
	@echo "  AR $$@ ($$(words $$^) objects)"
	@$(AR) rcs $$@ $$^
endef
# (the generated headers are order-only for the core's objects: burn.o holds the driver list, rebuilt with it)
$(HOST_CORE_OUT)/src/burn/burn.o $(PS5_CORE_OUT)/src/burn/burn.o: $(GEN_STAMP)
