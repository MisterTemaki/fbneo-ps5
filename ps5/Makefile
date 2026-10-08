# FBNeo PS5
#   make ps5    -> build/ps5/FBNeoPS5.elf   the payload to send with the PS5 Payload Manager / ELF loader: it installs
#                  or updates the dashboard app /data/homebrew/$(TITLE_ID)/ (FBNeo itself, icon included) and stays
#                  running as the helper that lets the app reach /data
#   make app    -> build/app/$(TITLE_ID)/     the app folder on its own (eboot.bin, sce_module/, sce_sys/)
#   make host   -> build/host/                Linux builds of the same code (PS5 calls faked by host/sce_host.cpp)
#   make test   -> host builds + tests/run-tests.sh
#   make dist   -> build/dist/FBNeoPS5-v$(VERSION).elf (the release name)
#   make send   -> sends FBNeoPS5.elf to $(PS5_HOST):$(PS5_PORT) with the SDK's prospero-deploy
#
# The layout is PS5SX2's (and Snes9x PS5's, Genesis Plus GX PS5's): eboot.bin is the emulator, a native PS5 title
# (the console gives the controller to the title in front only), and the payload is its installer and helper:
#   1. build/ps5/FBNeoPS5-helper.elf   the helper alone (payload); the app sends it to the ELF loader
#   2. build/app/$(TITLE_ID)/          eboot.bin = FBNeo's core (src/burn, src/cpu: core.mk, arcade drivers only) +
#                                      the PS5 frontend + the helper from step 1 (native title, signed);
#                                      sce_module/libc.prx; sce_sys/
#   3. build/ps5/FBNeoPS5.elf          installer + helper, with the app folder from step 2 built in (eboot.bin
#                                      zlib-compressed: FBNeo is big)
#
# Dependencies: the ps5-payload-dev SDK (v0.42 or newer, https://github.com/ps5-payload-dev/sdk) in
# $(PS5_PAYLOAD_SDK), with clang/lld 18 on the host; perl and python3 (FBNeo's generated sources).

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION ?= 1.3
TITLE_ID ?= PPSA99012
APP_OUT := build/app/$(TITLE_ID)

HOST_CORE_OUT := build/host/core
PS5_CORE_OUT := build/ps5/core
include core.mk
CORE_VERSION := $(shell sed -n 's/^\#define VER_\(MAJOR\|MINOR\|BETA\|ALPHA\) *\([0-9]*\).*/\2/p' $(ROOT)/src/burn/version.h | paste -sd.)

# the app (eboot.bin): the PS5 layer and the frontend
EMU_SRC := coreorbis/main-boot.cpp \
	coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoVideo.cpp coreorbis/orbis-shims/ProsperoAudio.cpp \
	coreorbis/orbis-shims/ProsperoInput.cpp coreorbis/orbis-shims/ProsperoJailbreak.cpp \
	coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoCrt.cpp \
	frontend/fe_emu.cpp frontend/fe_menu.cpp frontend/fe_settings.cpp frontend/fe_text.cpp \
	frontend/fe_games.cpp frontend/fe_http.cpp frontend/fe_covers.cpp frontend/fe_shelf.cpp \
	frontend/fe_prefetch.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp
# the glue that talks to the core (built with the core's headers and defines)
GLUE_SRC := frontend/fe_burn.cpp core/burn_frontend.cpp
# minizip (zlib licence) for the ROM-set zips, built as C with FBNeo's zlib
EMU_C_SRC := frontend/third_party/unzip.c frontend/third_party/ioapi.c
# the payloads: the helper, and the installer (= helper + install, with zlib's inflate for the compressed eboot)
# (the helper also downloads the covers in the background: fe_coverworker.cpp, with the app's HTTPS code)
HELPER_SRC := coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoHelper.cpp coreorbis/orbis-shims/ProsperoCrash.cpp \
	frontend/fe_http.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp
INSTALLER_SRC := installer/installer_main.cpp $(HELPER_SRC) coreorbis/orbis-shims/ProsperoInstall.cpp
INFLATE_C_SRC := $(addprefix $(ROOT)/src/dep/libs/zlib/,adler32.c crc32.c inffast.c inflate.c inftrees.c uncompr.c zutil.c)

INCS := -Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Ifrontend -isystem $(ROOT)/src/dep/libs/zlib
GLUE_INCS = $(CORE_INCS) -Ifrontend -Ifrontend/third_party -Icoreorbis/include-orbis
DEFS := -DFBNEO_PS5_VERSION='"$(VERSION)"' -DFBNEO_CORE_VERSION='"$(CORE_VERSION)"' -DFBNEO_TITLE_ID='"$(TITLE_ID)"'
MINIZIP_FLAGS := -DNOUNCRYPT -I$(ROOT)/src/dep/libs/zlib -w
PORT_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers
GLUE_WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-write-strings

# stb_image in fe_covers.o has warnings of its own
%/port/frontend/fe_covers.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-but-set-variable -Wno-implicit-fallthrough
# the UI fonts (PS5SX2's: PCSX2's Roboto Regular and PromptFont; Font Awesome's brands) built into fe_text.o
%/port/frontend/fe_text.o: EXTRA_FLAGS = -DFE_FONT_DIR='"$(abspath frontend/assets/fonts)"' -Wno-error -Wno-sign-compare -Wno-unused-function
%/port/frontend/fe_text.o: frontend/assets/fonts/Roboto-Regular.ttf frontend/assets/fonts/promptfont.otf \
	frontend/assets/fonts/fa-brands-400.otf
# the helper payload is installer_main.cpp without the install step
%/port/installer/helper_main.o: EXTRA_FLAGS = -DFBNEO_HELPER_ONLY

# ---------------- PS5 ----------------
PS5_CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
PS5_OUT := build/ps5
# The PS5's CPU is Zen 2 (PS5SX2 builds with the same -march). The core at -O2: -O3 makes it much bigger for little.
PS5_OPT := -O3 -fPIC -march=znver2 -fno-strict-aliasing
PS5_CORE_OPT := -O2 -fPIC -march=znver2 -fno-strict-aliasing -Wno-c++11-narrowing
# the payload SDK's network libraries (its http2_get sample's), for the helper's cover downloads
PS5_PAYLOAD_LIBS := -lkernel_sys -lSceNet -lSceNetCtl -lSceSsl -lSceHttp2 -lpthread

$(eval $(call core_rules,$(PS5_CORE_OUT),$(PS5_CC),$(PS5_CXX),$(PS5_CORE_OPT)))

PS5_EMU_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(EMU_SRC)) $(patsubst %.c,$(PS5_OUT)/port/%.o,$(EMU_C_SRC)) \
	$(patsubst %.cpp,$(PS5_OUT)/glue/%.o,$(GLUE_SRC))
PS5_HELPER_OBJ := $(PS5_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(HELPER_SRC))
PS5_INSTALLER_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(INSTALLER_SRC)) \
	$(patsubst $(ROOT)/%.c,$(PS5_OUT)/inflate/%.o,$(INFLATE_C_SRC))

ps5: $(PS5_OUT)/FBNeoPS5.elf
core-ps5: $(PS5_CORE_OUT)/libfbneo.a

$(PS5_OUT)/port/%.o: %.c
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(MINIZIP_FLAGS) -c -o $@ $<

$(PS5_OUT)/inflate/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) -I$(ROOT)/src/dep/libs/zlib -w -c -o $@ $<

$(PS5_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(PS5_OUT)/glue/%.o: %.cpp frontend/fe_burn.h | $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=gnu++17 $(PS5_CORE_OPT) $(DEFS) $(CORE_DEFS) $(GLUE_INCS) $(GLUE_WARN) -fsigned-char -c -o $@ $<

$(PS5_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# 1. the helper payload (built into the app, which sends it to the ELF loader when no helper answers)
$(PS5_OUT)/FBNeoPS5-helper.elf: $(PS5_HELPER_OBJ)
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)

$(PS5_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(PS5_OUT)/FBNeoPS5-helper.elf
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(PS5_OUT)/FBNeoPS5-helper.elf)"' -c -o $@ $<

# 3. the installer + helper, with the app folder built in; eboot.bin as tools/pack_file.py packs it (zlib)
APP_FILES = $(APP_OUT)/eboot.bin $(APP_OUT)/sce_sys/param.json $(APP_OUT)/sce_sys/icon0.png $(APP_OUT)/sce_sys/pic0.dds \
	$(APP_OUT)/sce_sys/pic1.dds \
	$(APP_OUT)/sce_module/libc.prx
build/app/eboot.bin.z: $(APP_OUT)/eboot.bin tools/pack_file.py
	python3 tools/pack_file.py $< $@
build/app/%.dds.z: $(APP_OUT)/sce_sys/%.dds tools/pack_file.py
	python3 tools/pack_file.py $< $@
APP_DATA_DEFS = -DAPP_EBOOT_Z='"$(abspath build/app/eboot.bin.z)"' -DAPP_PARAM='"$(abspath $(APP_OUT)/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath $(APP_OUT)/sce_sys/icon0.png)"' -DAPP_PIC0_Z='"$(abspath build/app/pic0.dds.z)"' \
	-DAPP_LIBC='"$(abspath $(APP_OUT)/sce_module/libc.prx)"'

$(PS5_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h $(APP_FILES) \
		build/app/eboot.bin.z build/app/pic0.dds.z
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(APP_DATA_DEFS) -c -o $@ $<

$(PS5_OUT)/FBNeoPS5.elf: $(PS5_INSTALLER_OBJ) $(PS5_OUT)/install_data.o
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)
	@echo "built $@ ($$(stat -c %s $@) bytes)"

# the release file, named with the version: build/dist/FBNeoPS5-v$(VERSION).elf
DIST := build/dist
dist: $(PS5_OUT)/FBNeoPS5.elf
	@mkdir -p $(DIST)
	cp $< $(DIST)/FBNeoPS5-v$(VERSION).elf
	@ls -l $(DIST)

send: $(PS5_OUT)/FBNeoPS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $<

# ---------------- the app (eboot.bin = FBNeo) ----------------
# 2. A native PS5 title linked the way PS5SX2 links its eboot (ps5/coreorbis/link-vk.sh): the boilerplate's
# app_crt.cpp (_start -> _init_env -> main) and app_cpp_runtime.cpp, the emulator's objects, a PIE laid out by
# ps5-pie.ld (+ PS5SX2's ehframe.ld) against the SDK's system-module stubs and its static libc++/libc++abi/
# libunwind/libc, every defined symbol local (PS5SX2's genmap.py result: localize.map); then ps5-native-tool link
# (SCE dynamic tables, process parameters) and self --sign. The folder carries sce_module/libc.prx, the C runtime
# module a native title needs (without it the console stops the app with PRX_SCE_MODULE_LOAD_ERROR, 0xa0020102).
NATIVE := proto/native
NATIVE_TOOL := $(NATIVE)/build/host/ps5-native-tool
LIBC_BUILDER := $(NATIVE)/build/host/libc-builder
LLD ?= ld.lld
NATIVE_CLANG = PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) sh $(NATIVE)/tooling/prospero-clang18
SDK_LIB := $(PS5_PAYLOAD_SDK)/target/lib
# every system-module stub but the two PS5SX2 leaves out (link-vk.sh)
APP_STUBS = $(filter-out %/libScePosixForWebKit.so %/libSceGLSlimVSH.so,$(sort $(wildcard $(SDK_LIB)/*.so)))

$(NATIVE_TOOL): $(NATIVE)/tooling/native/native_app_builder.cpp $(NATIVE)/tooling/native/elf_object.cpp \
		$(NATIVE)/tooling/native/self_container.cpp $(NATIVE)/tooling/native/sce_module_writer.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -o $@ $^ -lz
HOST_CXX_NATIVE ?= clang++

$(LIBC_BUILDER): $(NATIVE)/tooling/native/libc_builder.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -Wall -Wextra -Werror -o $@ $<

# sce_module/libc.prx, deterministic: it must match the published digest (proto/native/libc.prx.sha256)
build/app/libc.prx: $(LIBC_BUILDER) $(NATIVE_TOOL) $(wildcard $(NATIVE)/tooling/native/runtime/*.txt)
	@mkdir -p $(dir $@)
	$(LIBC_BUILDER) $(NATIVE)/tooling/native/runtime/api-surface.txt $(NATIVE)/tooling/native/runtime/imports.txt build/app/libc.raw.elf
	$(NATIVE_TOOL) self --sign --in build/app/libc.raw.elf --out $@
	cd build/app && sha256sum --check --strict $(abspath $(NATIVE)/libc.prx.sha256)

$(APP_OUT)/sce_module/libc.prx: build/app/libc.prx
	@mkdir -p $(dir $@)
	cp $< $@

app: $(APP_FILES)

build/app/app_crt.o build/app/app_cpp_runtime.o: build/app/%.o: $(NATIVE)/tooling/native/%.cpp
	@mkdir -p $(dir $@)
	$(NATIVE_CLANG) -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
		-c -o $@ $<

EBOOT_OBJS = build/app/app_crt.o build/app/app_cpp_runtime.o $(PS5_EMU_OBJ) $(PS5_OUT)/helper_data.o
build/app/fbneo-pie.elf: $(EBOOT_OBJS) $(PS5_CORE_OUT)/libfbneo.a $(NATIVE)/tooling/native/ps5-pie.ld \
		$(NATIVE)/tooling/native/ehframe.ld $(NATIVE)/tooling/native/app-symbols.map $(NATIVE)/tooling/native/localize.map
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
		-T $(NATIVE)/tooling/native/ps5-pie.ld -T $(NATIVE)/tooling/native/ehframe.ld --eh-frame-hdr \
		--version-script $(NATIVE)/tooling/native/app-symbols.map --version-script $(NATIVE)/tooling/native/localize.map \
		-e _start --no-dynamic-linker -o $@ $(EBOOT_OBJS) $(PS5_CORE_OUT)/libfbneo.a \
		--as-needed $(APP_STUBS) -L $(SDK_LIB) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a $(SDK_LIB)/libc.a -lpthread

build/app/eboot.elf: build/app/fbneo-pie.elf $(NATIVE_TOOL)
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(SDK_LIB) --module-sdk 0x02000009 \
		--companion-sdk 0x08050001 --file-name eboot.elf
	python3 tools/prepare_console_elf.py --input $@ --output build/app/eboot-console-ready.elf

$(APP_OUT)/eboot.bin: build/app/eboot.elf $(NATIVE_TOOL)
	@mkdir -p $(dir $@)
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F
	$(NATIVE_TOOL) self --inspect --file $@

$(APP_OUT)/sce_sys/param.json: app/sce_sys/param.json
	@mkdir -p $(dir $@)
	sed 's/PPSA99012/$(TITLE_ID)/g' $< > $@

$(APP_OUT)/sce_sys/%.png: app/sce_sys/%.png
	@mkdir -p $(dir $@)
	cp $< $@

# pic0.dds (the home screen's background while the icon is selected) and pic1.dds (the launch background):
# 3840x2160 BC7_UNORM DX10 DDS without mipmaps, made from app/sce_sys/background-source.png (tools/make_art.py)
$(APP_OUT)/sce_sys/%.dds: app/sce_sys/%.dds
	@mkdir -p $(dir $@)
	cp $< $@

# ---------------- host ----------------
HOST_CC ?= gcc
HOST_CXX ?= g++
HOST_OUT := build/host
HOST_SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_OPT := -O1 -g -mavx2 -fno-strict-aliasing
# the core: AddressSanitizer only (UBSan would report the drivers' deliberate shifts and unaligned reads)
HOST_CORE_SAN ?= -fsanitize=address -fno-omit-frame-pointer
$(eval $(call core_rules,$(HOST_CORE_OUT),$(HOST_CC),$(HOST_CXX),-O1 -g1 $(HOST_CORE_SAN)))

HOST_EMU_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(EMU_SRC) host/sce_host.cpp) \
	$(patsubst %.c,$(HOST_OUT)/port/%.o,$(EMU_C_SRC)) $(patsubst %.cpp,$(HOST_OUT)/glue/%.o,$(GLUE_SRC))
HOST_HELPER_OBJ := $(HOST_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(HELPER_SRC) host/sce_host.cpp)
HOST_INSTALLER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(INSTALLER_SRC) host/sce_host.cpp) \
	$(patsubst $(ROOT)/%.c,$(HOST_OUT)/inflate/%.o,$(INFLATE_C_SRC))

core-host: $(HOST_CORE_OUT)/libfbneo.a
host: $(HOST_OUT)/fbneo-ps5-host $(HOST_OUT)/fbneo-ps5-installer $(HOST_OUT)/fbneo-ps5-helper $(HOST_OUT)/fbneo_headless
headless: $(HOST_OUT)/fbneo_headless

$(HOST_OUT)/port/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_OPT) $(MINIZIP_FLAGS) -c -o $@ $<

$(HOST_OUT)/inflate/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_OPT) -I$(ROOT)/src/dep/libs/zlib -w -c -o $@ $<

$(HOST_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/glue/%.o: %.cpp frontend/fe_burn.h | $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=gnu++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(CORE_DEFS) $(GLUE_INCS) $(GLUE_WARN) -fsigned-char -c -o $@ $<

$(HOST_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# The host installer carries a stand-in app folder (tests/fake-eboot.bin, packed as the real one, and
# tests/fake-libc.prx + the real param/icon/pic); the host app carries the host helper, which it hands to a fake
# ELF loader in the tests.
$(HOST_OUT)/fake-eboot.bin.z: tests/fake-eboot.bin tools/pack_file.py
	@mkdir -p $(dir $@)
	python3 tools/pack_file.py $< $@
$(HOST_OUT)/%.dds.z: app/sce_sys/%.dds tools/pack_file.py
	@mkdir -p $(dir $@)
	python3 tools/pack_file.py $< $@
HOST_APP_DEFS = -DAPP_EBOOT_Z='"$(abspath $(HOST_OUT)/fake-eboot.bin.z)"' -DAPP_PARAM='"$(abspath app/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath app/sce_sys/icon0.png)"' -DAPP_PIC0_Z='"$(abspath $(HOST_OUT)/pic0.dds.z)"' \
	-DAPP_LIBC='"$(abspath tests/fake-libc.prx)"'

$(HOST_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h \
		$(HOST_OUT)/fake-eboot.bin.z tests/fake-libc.prx app/sce_sys/param.json app/sce_sys/icon0.png \
		$(HOST_OUT)/pic0.dds.z
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(HOST_APP_DEFS) -c -o $@ $<

$(HOST_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(HOST_OUT)/fbneo-ps5-helper
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(HOST_OUT)/fbneo-ps5-helper)"' -c -o $@ $<

$(HOST_OUT)/fbneo-ps5-helper: $(HOST_HELPER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/fbneo-ps5-installer: $(HOST_INSTALLER_OBJ) $(HOST_OUT)/install_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/fbneo-ps5-host: $(HOST_EMU_OBJ) $(HOST_OUT)/helper_data.o $(HOST_CORE_OUT)/libfbneo.a
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread -lm

# the core and the glue alone (tests: ROM sets, states, DIP switches)
$(HOST_OUT)/fbneo_headless: $(HOST_OUT)/glue/host/fbneo_headless.o $(HOST_OUT)/glue/frontend/fe_burn.o \
		$(HOST_OUT)/glue/core/burn_frontend.o $(HOST_OUT)/port/frontend/third_party/unzip.o \
		$(HOST_OUT)/port/frontend/third_party/ioapi.o $(HOST_CORE_OUT)/libfbneo.a
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lm -lpthread

test: host
	bash tests/run-tests.sh

clean:
	rm -rf build

.PHONY: ps5 core-ps5 core-host host headless test send app dist clean
.SECONDARY:
