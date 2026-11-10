<!-- SPDX-License-Identifier: MIT -->
# ps5-slang-gpu

The GPU renderer every Temakix RetroStation program shares: RetroArch slang shader presets run with Vulkan through
RADV (Mesa's AMD driver, linked into the eboot), the same 26 presets in the same order and with the same names in every
program, the shipped presets read once from the app folder (`/app0/shaders/slang`), and the CPU as the fall-back. It is
Genesis Plus GX PS5's GPU renderer (rs1 95cf4b4: `ProsperoGpu.cpp`, `ProsperoSlangExt.cpp`, `ProsperoSlang.h`, our
own MIT code), taken out of it and made generic.

Licence: MIT (`LICENSE`), for the library's own code. It links only MIT / BSD / permissive code, so the
non-commercial programs (Snes9x, FBNeo, Genesis Plus GX) may use it as the GPL ones do. Nothing here has run on a PS5
yet.

| | |
|---|---|
| `include/ps5gpu/gpu.h` | the API a program uses (`ps5gpu::`) |
| `include/ps5gpu/slang.h` | the preset tables and the loader (`ps5slang::`), what the generated tables include |
| `src/gpu.cpp` | the renderer: Vulkan 1.0, VK_KHR_display on the console, off screen on the host; the slang passes, the UI surface over them, the pipeline cache, the shader builder thread, the scene quads (genplus's shelf) |
| `src/slang_loader.cpp` | the menu's list; `.slangp` / `.slang` read and compiled at run time with glslang (shipped presets and the player's own); the manifest check (size at start, SHA-256 before compiling, compiled from the bytes checked) |
| `src/common.cpp`, `src/sha256.cpp` | the configuration, the log, the start guard, `Fit`; SHA-256 |
| `src/stub.cpp` | the stand-in for a CPU-only build (`Init` false) |
| `shaders/presets.txt` | the menu: 8 presets built in (`own/*.slangp`, every file permissive) and 18 shipped as files (`file:`) |
| `shaders/plain.slang`, `ui.slang`, `own/` | the library's own shaders (the plain picture, the UI surface pass) and descriptors |
| `shaders/slang/` | the libretro/slang-shaders files those presets use, at `slang/REVISION` (e1d75632), unchanged, each under its own licence: data, see "Shaders" |
| `tools/` | `slang_compile.py` (built-in presets to SPIR-V tables, with glslangValidator), `shader_manifest.py` (the shipped presets' manifest table; `--install` the app folder's `shaders/slang`), `shader_licences.py` (every file's licence; `--check`), `check_no_shader_code.py` (no GPL shader text in a binary), `vendor.sh` / `check_vendor.py` |
| `mk/slang-gpu.mk` | the make fragment: sources, glslang, the tables, object rules, the RADV link's flags and checks |
| `third_party/glslang` | glslang 15.1.0 (BSD-3-Clause and others), `third_party/stb` stb_image 2.30 (public domain or MIT) |
| `tests/` | host tests on lavapipe (`make test`) |

## The API

```cpp
#include "ps5gpu/gpu.h"

ps5gpu::Config c;
c.log = [](const char* line) { MyLog("%s", line); };   // the program decides whether it is written
c.app_name = "Snes9x PS5";
c.cache_file = root + "/cache/gpu-pipelines.bin";     // pipelines built once load at once next time
c.params_file = root + "/shader-params.ini";          // the shader parameters set in the menu
c.user_shader_dir = root + "/shaders";                // the player's own presets ("" : none)
c.shipped_dir = "/app0/shaders/slang";                // the default
c.thread_enter = MyCrashArmThread;                    // optional: each thread the library starts
c.thread_leave = MyCrashDisarmThread;
#ifndef __PROSPERO__
c.host_present = MyHostFlip;                          // host tests: each frame read back (B8G8R8A8)
#endif
ps5gpu::Configure(c);

if (ps5gpu::GuardBegin(root + "/logs/gpu-starting") && ps5gpu::Init(1920, 1080, false, 0, 0))
    gpu = true;                                       // the GPU draws; GuardEnd() once the first frame is on screen
else
    { ps5gpu::GuardEnd(); /* the CPU renderer, as before */ }

// each frame
ps5gpu::Frame f;
f.pixels = core_frame; f.width = 256; f.height = 224; f.pitch = core_pitch;
f.format = ps5gpu::PixelFormat::RGB565;               // XRGB8888, XBGR8888, RGB565, XRGB1555
f.crop = {8, 0, 8, 0};                                // overscan, pixels off each edge
f.rotation = ps5gpu::Rotation::Rotate90;              // counter-clockwise, as libretro's SET_ROTATION
ps5gpu::Rect dst = ps5gpu::Fit(224, 256, 3.0 / 4.0, {0, 0, 1920, 1080}, integer_scale);
ps5gpu::SetFrame(f, dst, shader, smooth, scanlines); // or HideGame()
// the UI surface: transparent (0) where the picture shows, the program's OSD / pause menu drawn over it
ps5gpu::Present(ui_surface, dirty_rect, true);
```

- **Init(ui_w, ui_h, offscreen, out_w, out_h)**: false means no GPU (no driver, no display, a set-up step failed: the
  log says which); everything is then a no-op and the program draws on the CPU, as the fall-back must. On the console
  the display's mode closest to 60 Hz (the largest of those) is used: 4K on a 4K TV.
- **The UI surface** is `ui_w x ui_h`, `Config::ui_format` RGBA8 (bytes R, G, B, A; genplus's) or BGRA8, premultiplied
  by default (`ui_premultiplied = false` for straight alpha). It is laid over everything, pixel-exact (each surface
  pixel a 2x2 block at 4K). Only `dirty` is copied each Present (the whole surface the first time).
- **SetFrame** copies the frame (it may change after the call), converts it to B8G8R8A8, crops it, and keeps the
  rotation: the shader runs on the picture as the core made it and the last pass draws it turned into `dst` (scanlines
  follow the game's lines, as on a turned monitor; the `Rotation` semantic is set); a fullscreen preset (the Mega Bezel)
  is given the picture turned already, since it lays out the whole screen. A frame larger than the staging buffer
  grows it. `SetGame(argb, w, h, dst, ...)` is the XRGB8888, tightly packed call (genplus's).
- **dst** is in surface pixels; the GPU scales it to the output. `Fit(w, h, aspect, area, integer)` centres a picture
  at an aspect ratio (0: square pixels), as large as fits or at the largest whole multiple. For a turned picture pass
  the turned size and aspect. A fullscreen preset ignores dst (`ShaderFullscreen(i)`): make the whole surface
  transparent for it.
- **The menu**: `ShaderCount()` (0 is "Off", the plain picture with `smooth` and `scanlines`), `ShaderName(i)` (what
  to show: "<name>: damaged, reinstall" for a shipped preset whose files are missing or changed; translate it from
  `ShaderPresetName(i)` and `ShaderDamaged(i)`), `ShaderPresetName(i)` (the name to store: `shader_name=` in the
  program's ini), `FindShader(name)` (the index again, -1 if gone), `PassCount`, `Building()` (a shader is being
  built in the background: the picture goes on with the one it had). The order is `shaders/presets.txt`'s, then the
  player's own presets.
- **Shader parameters**: `ShaderParamCount(i)`, `ShaderParam(i, k, &info)`, `SetShaderParam(i, name, value)`,
  `ResetShaderParams(i)`, `SaveShaderParams()` (to `Config::params_file`, keyed by the preset's name).
- **GuardBegin / GuardEnd**: a start that dies while the GPU is being set up leaves the marker; the next start sees
  it, removes it and uses the CPU; the one after tries the GPU again.
- **Threads**: shaders are built on a thread with an 8 MiB stack (RADV's ACO and glslang recurse deep); a refused
  thread builds on the caller's. `Config::thread_enter / thread_leave` run on each such thread.
- `TakeShaderMs()` (the game passes' GPU time), `OutWidth/OutHeight`, `DeviceName`, `Built()` (false in the stub),
  `kVersion`. The scene (`Config::scene`, `SetScene`, `SceneTexture`...) is genplus's 3D shelf; other programs leave
  it off (no quad shader is compiled then).
- Host tests: `Config::sync_build` (each shader built before Present returns) and `check_all` (every preset read from
  files compiled at Init, the log says how many).

### Video pictures (1.1, `Config::video`)

A media player's decoded pictures, drawn by the GPU in one pass (`shaders/video.vert`, `shaders/video.frag`, the
library's own, MIT; compiled at build time into `video_spv.h`):

```cpp
c.video = true;                                       // before Init: the video pipeline (and its self-test)
...
ps5gpu::VideoFrame f;                                 // what the decoder gave
f.width = 3840; f.height = 2160;
f.layout = ps5gpu::VideoLayout::SemiPlanarUV;         // Planar, SemiPlanarUV (NV12, P010), SemiPlanarVU, Packed, Gray, PlanarGBR
f.bits = 10; f.msb = true;                            // 8, or 9..16 in 16-bit words, at the top (P010) or the bottom
f.chroma_shift_x = f.chroma_shift_y = 1;              // 4:2:0
f.plane[0] = {y, y_pitch}; f.plane[1] = {uv, uv_pitch};
f.matrix = ps5gpu::ColorMatrix::BT2020; f.full_range = false;
f.transfer = ps5gpu::VideoTransfer::Pq; f.peak_nits = 1000;
f.release = MyRelease; f.release_ctx = picture;      // when the planes may be reused
ps5gpu::VideoView v;
v.dst = video_rect;                                   // surface pixels; may reach past the surface
v.tone_map = ps5gpu::ToneMap::Bt2390; v.white_nits = 250;
v.filter = ps5gpu::VideoFilter::Bilinear;             // or Bicubic
if (!ps5gpu::SetVideo(f, v))                          // false: draw it on the CPU
    ...
ps5gpu::Present(ui_surface, dirty, true);             // the picture under the surface (transparent where it shows)
```

- **One pass**: each plane sampled where the output pixel falls and filtered in Y'CbCr (bilinear: the hardware's, or
  a tent widened by the shrink factor when the picture is made smaller; bicubic: Catmull-Rom, widened the same way),
  the codes normalised (limited or full range, 8 to 16 bits), the colour matrix (BT.601, BT.709, BT.2020 NCL, FCC,
  SMPTE 240M), and for HDR10 / HLG the tone mapping: to linear light (PQ's EOTF; HLG's inverse OETF and OOTF at 1000
  nits), BT.2020 to BT.709, BT.2390's EETF / Hable / Reinhard on max(R, G, B) from the film's peak to the white, gamma
  2.4. These are Media Player Classic PS5's CPU numbers (libswscale, then its mpc_tonemap.cpp), in float. Chroma is
  sited at the centre (libswscale's default). The result goes straight into the output (4K on a 4K TV), under the UI
  surface, which is composited over it as before.
- **Copies**: `SetVideo` copies the planes once (a memcpy per plane) into a host-visible staging buffer of a ring of
  three; the slot the frame in flight reads is never the one written, so it never waits for the GPU. A picture whose
  planes lie in memory imported with `ImportHostMemory(base, bytes)` (VK_EXT_external_memory_host; base and bytes
  multiples of `HostMemoryAlignment()`) isn't copied at all: the GPU copies it into its plane images itself, and the
  frame's `release` is called once the frame that read it is done (at the next Present). RADV on the PS5 has the
  extension (its winsys's buffer_from_ptr: the CPU's address, once the memory has GPU access); lavapipe has it too.
- **A picture taken is one the next Present can draw**: `SetVideo` makes the plane images a picture of a new size or
  format needs before it says yes (false, and `release` called, when it can't: the program draws it itself); the old
  images go once the frame that read them is done. Everything that can refuse a picture is checked before anything
  changes, so a refused one leaves the pending picture and its plane images as they were. A frame whose submission
  fails keeps its picture pending (staging slot or imported reference and all) for the next Present; without a new
  picture the images still hold the one shown (a paused film stays on screen), and the UI surface is copied whole
  next time. A game's frame is uploaded again from its staging buffer, its history and passes start over from
  UNDEFINED. (Host tests: `PS5GPU_TEST_SUBMIT_FAIL=1` fails each Present's submit; `PS5GPU_TEST_VIDEO_IMAGE_FAIL=n`
  the n-th set of plane images and later ones; `PS5GPU_TEST_OFFSCREEN_IN_FLIGHT=1` leaves each off-screen frame in
  flight until the next Present, as a display's is, its read-back handed over then.)
- **Chroma siting**: `VideoFrame::chroma_pos_x / y` (0: on the first luma pixel's centre, MPEG-2 / H.264 / HEVC's
  "left"; 0.5: centred, the default, libswscale's when the stream doesn't say).
- `SetVideoView` moves the same picture or changes its settings; `HideVideo` takes it away; `ReleaseVideo` lets go
  of every picture the GPU still holds (waiting for the frame in flight), before a program frees what they point into
  or releases its imports (at its exit, before `Shutdown`); `VideoFormatSupported`
  says whether a frame can be taken; `TakeVideoStats` gives the pictures taken (copied / in place / refused), the copy
  time, the GPU time of the frames that drew one, and Present's own CPU time and its waits.
- **Self-test** at Init: two flat pictures (8-bit NV12 BT.709 limited, 10-bit planar 4:4:4 BT.2020 full) drawn and read
  back against the same arithmetic on the CPU; a driver that gets them wrong leaves `VideoAvailable()` false (the log
  says so), and the program draws its pictures itself.
- With `Config::video`, the instance stays Vulkan 1.0 and takes `VK_KHR_get_physical_device_properties2` and
  `VK_KHR_external_memory_capabilities` where the driver has them (for the import's properties and buffers; a Vulkan
  1.1 instance crashed the PS5's driver at the physical-device step); without `video` nothing changes for a program
  (Vulkan 1.0, no pipeline, no extension). Init logs each of its steps, the GPU step's sub-steps too, so a crash in
  the driver names the last call.

## Vendoring it

A program carries a pinned copy, as it carries mitrt (ps5-native-runtime-mit):

```sh
# in this repository, with the work tree clean
tools/vendor.sh /home/claude/work/<program>/ps5/ps5-slang-gpu        # or tools/vendor.sh <dest> <commit>
```

`<dest>` is replaced by the commit's files, unchanged, and `VENDOR.txt` names the commit and every file with its
SHA-256. `tools/check_vendor.py <copy>` fails when a file there is changed, missing or added (`--against <this repo>`
also checks VENDOR.txt against the commit itself); the program's build runs it before anything of the copy is used
(`mk/slang-gpu.mk`, `SLANG_GPU_VENDOR_OK`). Don't edit the copy: change the library, commit, vendor again. The copy is
about 20 MB (glslang 5 MB, the shader files 14 MB); git stores a file the program already had (genplus's
`shaders/slang`) only once.

## Building it into a program

```make
SLANG_GPU := ps5-slang-gpu
SLANG_GPU_GEN := build/gen/slang-gpu
SLANG_GPU_MITRT := mitrt                      # the program's ps5-native-runtime-mit copy
SLANG_GPU_RADV_MIT := $(abspath ../../rsdeps/radv-mit)
SLANG_GPU_LINK_OPTS := --own-stdio-wrap       # only if the program wraps fclose / fflush itself (netfs)
include $(SLANG_GPU)/mk/slang-gpu.mk

# the PS5's objects: the library (with the program's flags and RADV's Vulkan headers), glslang, the tables
PS5_GPU_OBJ := $(call SLANG_GPU_OBJECTS,build/ps5/slang-gpu,$(PS5_CXX),-O2 -fPIC -march=znver2 $(SLANG_GPU_PS5_FLAGS),-O2 -fPIC -march=znver2)
# the CPU-only build: common.cpp + stub.cpp
PS5_STUB_OBJ := $(call SLANG_GPU_STUB_OBJECTS,build/ps5/slang-gpu,$(PS5_CXX),-O2 -fPIC -march=znver2)
# the host's (any Vulkan driver: link -lvulkan)
HOST_GPU_OBJ := $(call SLANG_GPU_OBJECTS,build/host/slang-gpu,$(HOST_CXX),-O1 -g $(HOST_SAN),-O2 -g1)
```

The program's own sources include `"ps5gpu/gpu.h"` with `$(SLANG_GPU_INC)`. The tables are made on the host by
`tools/slang_compile.py` (needs `glslangValidator`, glslang-tools 15.1.0) and `tools/shader_manifest.py`; every program
built from the same commit gets the same bytes (the tests check it), so the same `rs-shaders-manifest=<id>`.

The PS5 link (a RADV title on mitrt's MIT platform layer, as genplus links today):

```make
$(LLD) ... $(PIE_SCRIPTS) -e _start --no-dynamic-linker $(SLANG_GPU_LINK_FLAGS) ... -o $@ \
    $(EBOOT_OBJS) $(PS5_GPU_OBJ) $(SLANG_GPU_RADV_ARCHIVE) libps5rt_platform.a libc++.a libc++abi.a libunwind.a \
    --as-needed $(APP_STUBS) -L $(SDK_LIB) $(SDK_LIB)/libc.a -lpthread --trace >$(TRACE)
python3 tools/check_imports.py $@ $(SDK_LIB) $(SLANG_GPU_IMPORT_ALLOW)   # libSceAgc, libSceAgcDriver: RADV's
python3 tools/check_syscalls.py check $(TRACE)
$(call SLANG_GPU_LINK_CHECKS,$(TRACE),$@)      # RADV's provenance and archive, no GPL input, ps5rt_locale, _DefaultRuneLocale
# and the program's libc.a allow-list, with $(SLANG_GPU_LIBC_ALLOWED) (mitrt/platform/libc-allow.txt: BSD / ISC)
...
eboot.bin: ...
	$(call SLANG_GPU_NO_SHADER_CODE,$(PIE) $@)   # no GPL shader text in the PIE or the eboot
app: ...
	$(call SLANG_GPU_MANIFEST_ID,$(APP_OUT)/shaders-manifest.txt)   # RetroStation's make app checks it
```

- RADV is `rsdeps/radv-mit` (built against mitrt's MIT ps5platform headers; its PROVENANCE.txt says so and the link
  check refuses another archive). RADV's archive carries zlib 1.3.1: leave out a program's own copy of the same zlib.
- The platform layer is mitrt's (`platform/`: thread stacks, open_memstream, the C locale's `*_l` functions libc++'s
  iostreams need for glslang, `fstatat` & co, emutls); build `libps5rt_platform.a` from the program's mitrt copy as
  genplus's Makefile does (`PS5RT_PLATFORM_SRC`).
- `SLANG_GPU_NOTICES` adds the library's, glslang's and stb_image's notices to the program's NOTICES.txt (with RADV's,
  `third_party/radv/NOTICES.txt` in genplus, and compiler-rt's from mitrt).
- A non-commercial program (Snes9x, FBNeo, Genesis Plus GX) keeps its strict trace checks: no libps5platform, no
  PS5_Vulkan or PS5_PayloadSDK input, no GPL libc.a member (no-locale.o, netdb.o, syscalls.o).

## Shaders

`shaders/presets.txt` is the menu of every program, in Genesis Plus GX PS5 2.2.1's order:

- **built in** (`own/*.slangp`, the library's own MIT descriptors): crt-lottes (public domain), crt-lottes-fast
  (Unlicense), crt-hyllian-fast, crt-nobody, newpixie-mini, crt-blurPi-sharp, crt-blurPi-soft (MIT), monoCRT (public
  domain). `tools/shader_licences.py --check` fails the build if any file of one isn't public domain, Unlicense, CC0,
  MIT, BSD or zlib. Their SPIR-V is in the tables, in every eboot.
- **shipped as separate files** (`file:`): crt-easymode, crt-1tap, crt-2tap, crt-guest-advanced, -fast, -ntsc,
  crt-royale, crt-geom, crt-easymode-halation, crt-hyllian, crt-aperture, crt-pi, crt-consumer, crt-yah, zfast-crt,
  ScaleFX, Mega Bezel (Potato), Mega Bezel (Standard): 255 files of libretro/slang-shaders e1d75632, 13,847 KB, most of
  them GPL. **No program has their code**: the eboot carries their paths, sizes and SHA-256 only, and compiles them on
  the console with glslang, each file checked first. RetroStation's make app installs them once in the app folder,
  `shaders/slang/` (`tools/shader_manifest.py --install`: the files, `LICENSES.md` with every file's licence and the
  GPL texts, `MANIFEST`, `SHA256SUMS`), and checks that every program carries the folder's manifest id.
- A shipped preset whose file is missing (found at start by the sizes) or changed (found by the SHA-256 when it is
  compiled) is "damaged, reinstall": the plain picture is drawn, nothing is written to the folder.
- The player's own `.slangp` (anywhere under `Config::user_shader_dir`) and `.slang` (at its top) come after; a name
  already taken is "<name> (yours: <file>)".

## Integrating it (what each program does)

1. Vendor it, include the fragment, build the GPU objects into the PS5 and host builds, the stub into a CPU-only build.
2. Video start: `Configure`, `GuardBegin`, `Init`; on false the CPU renderer as before (logged). The renderer setting
   (GPU default, CPU as a setting) is the program's.
3. Each frame: the core's frame through `SetFrame` with the program's aspect / crop / rotation; the OSD and pause menu
   drawn into the UI surface, transparent over the picture (all of it for a fullscreen preset); `Present`.
4. The shader menu from `ShaderCount` / `ShaderName` / `ShaderDamaged`, its parameters page from `ShaderParam*`;
   `shader_name=` (by `ShaderPresetName` / `FindShader`) in the program's own ini, per game as the program does.
5. Frame pacing stays the program's: `Present(..., true)` waits for the display (FIFO) on the console.
6. Link checks above; host tests on lavapipe (`Config::host_present`, `sync_build`).

## Host tests

`make test` builds the tables, the library (ASan, UBSan) and glslang for the host and runs `tests/run-tests.sh` on
whatever Vulkan driver the host has (lavapipe): every preset of the menu compiled and drawing a test picture, the four
pixel formats with a pitch, the crop, the four rotations (the corners checked), a CRT preset and the Mega Bezel on a
turned picture, the UI surface (premultiplied RGBA8 and straight BGRA8) and its dirty rectangle, a frame larger than
the staging buffer, damaged / missing / absent shipped files (nothing written there), no Vulkan driver (Init false),
the start guard, the parameters file across two starts, 1.1's video pictures (every layout and bit depth, the
matrices and ranges, HDR tone mapping against the CPU arithmetic, the UI over them, place and zoom, both filters,
refusals, the ring, pictures read in place), SHA-256's FIPS 180-4 examples, Fit, the stub, the tables made
twice (the same bytes), the folder checks, `check_no_shader_code.py`, and `vendor.sh` / `check_vendor.py`.

## Versions

- 1.1 (2026-11-07, after the independent review): plane images made by SetVideo, a failed submission rolled back, the
  shown picture's description kept apart from the pending one's, chroma siting; the UI surface sampled bilinear when
  the output isn't a whole multiple of it (2560x1440), still pixel-exact at 4K and 1080p.
- 1.1 (2026-11-07): video pictures (`Config::video`, `SetVideo` and the rest above), for Media Player Classic PS5 1.6:
  additive; a program that doesn't set `Config::video` gets the 1.0 renderer unchanged (Present also reads the frame's
  GPU time when it waits for it, as SetFrame did).

- 1.0 (2026-11-07): taken from Genesis Plus GX PS5 rs1 95cf4b4; pixel formats, pitch, crop, rotation, the UI
  surface's format and alpha, `FindShader`, `Fit`, the start guard, a growing staging buffer, configurable paths and
  hooks, the CPU-only stub, the make fragment and the vendoring tools.
