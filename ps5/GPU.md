# GPU rendering

The Vulkan/RADV backend uploads the framebuffer produced by FBNeo's CPU core as a texture, then performs scaling, CRT filtering, interface composition and presentation. This follows the division used by the [official Direct3D 9 frontend](https://github.com/finalburnneo/FBNeo/blob/master/src/intf/video/win32/vid_directx9.cpp). The emulation core and Model 2 rasterizer continue to run on the CPU.

PS5 logs supplied on November 10, 2026 confirm installation of 2.4, Vulkan/RADV initialization, 3840x2160 output at 59.940 Hz, and the Easymode style GPU filter. Sampled rates settle at 57.5 fps in Daytona USA and 59.9 fps in Cadillacs and Dinosaurs and Contra. Crime Fighters also starts. Both sessions exit with status 0. See [validation and limits](VALIDATION-2.4.md).

These logs do not validate every filter or extended sessions. Netplay between two physical PS5 consoles remains untested. Linux tests use Lavapipe, a software Vulkan driver; its timings are kept separate from console measurements.

## Behavior

- `GPU=1` is the default build setting. On PS5, `gpu=1` is the default configuration; set `gpu=0` in `fbneo-ps5.ini` and restart the app to use software output.
- With no CRT filter selected, the GPU presents the texture while preserving aspect ratio, scale, smoothing and scanlines.
- All eleven existing CRT filters have GPU implementations: Easymode style, Lottes, Lottes Fast, 1tap, 2tap, Hyllian Fast, Nobody, Newpixie Mini, BlurPi Sharp, BlurPi Soft and MonoCRT. Configuration names and IDs are unchanged. CPU and GPU implementations are not required to be pixel-identical.
- ScaleFX + rAA + AA style retains its CPU filter and presents the resulting image through the GPU.
- The interface is still drawn on the CPU. During gameplay, its surface uses premultiplied transparency so that text, HUD overlays and pause-menu dimming compose over the GPU image.
- Initialization failure selects software output. Submission failure recovers the current game frame and interface through CPU output. An incomplete-startup marker makes the next launch select software output to avoid repeated startup failures.
- Vulkan timing is collected on the presentation thread and passed to the emulation thread through a mutex-protected snapshot.

## Build and test

CPU-only builds remain available through `make GPU=0 host` and `make GPU=0 build/app/fbneo-pie.elf`. GPU builds add these dependencies to the existing requirements:

| Environment | Additional requirements |
|---|---|
| Host build tools | Python 3, `glslangValidator` 15.1.0 |
| Linux executable and tests | Vulkan headers and loader (`libvulkan`), a working Vulkan ICD; Lavapipe supports functional testing |
| PS5 cross-compilation | The existing SDK, RADV built for PS5, Vulkan headers and `PROVENANCE.txt` identifying the MIT platform headers |

`SLANG_GPU_RADV_MIT` must point to a directory containing `include/`, `lib/libvulkan_radeon.ps5.a` and `PROVENANCE.txt`. The prebuilt driver is not included in this repository. The release build reused the existing local dependency cache.

Run these commands from `ps5/`, with the appropriate tools and paths:

```sh
make GPU=1 host
make GPU=1 test-gpu
make GPU=1 build/app/fbneo-pie.elf \
  PS5_PAYLOAD_SDK=/path/to/sdk \
  SLANG_GPU_RADV_MIT=/path/to/radv-mit
make GPU=1 dat
make GPU=1 dist \
  PS5_PAYLOAD_SDK=/path/to/sdk \
  SLANG_GPU_RADV_MIT=/path/to/radv-mit
```

The Linux frontend uses CPU output by default to keep existing regressions reproducible. `FBNEO_HOST_GPU=1` enables Vulkan in the complete frontend; `test-gpu` sets it automatically. The GPU test fails if Vulkan cannot initialize, rather than counting CPU fallback as a GPU pass.

`build/app/fbneo-pie.elf` is an intermediate development executable. It does not contain the installer and does not install the app. A copy named `fbneo-gpu-pie.elf` has the same role.

`make dist GPU=1` produces the complete **`build/dist/FBNeoPS5-v2.4.elf` installer**, the update ZIP and the DAT. The installer contains the native app, metadata, artwork, C runtime module and GPU dependency notices. The build checks the files embedded in the final ELF and rejects an intermediate without those files. The interface identifies the app as `FBNeo 1.0.0.3 - PS5 2.4`.

## Integration and provenance

- `coreorbis/orbis-shims/ProsperoGpu.cpp` adapts the port's video API to the Vulkan library.
- `gpu.mk` defines objects, dependencies, the integration test and native linking. Duplicate zlib objects are removed only from a copy of the core archive used for the GPU link.
- `gpu-shaders/` contains the supported preset list and original MIT translations of Easymode style and Ntap. The GPL Easymode shader is not embedded.
- `ps5-slang-gpu/` is pinned to base commit `5531e732bc76b4eb1c1165f3044e5c0471fe44fa` with the local patches documented in `VENDOR.txt`. The delivered file hashes are checked; vendor files remain unchanged.
- `gpu-platform/` contains the existing MIT C platform layer and its notices, including compiler-rt 18.1.8 helpers under Apache-2.0 WITH LLVM-exception.
- The GPU library is MIT; glslang and stb retain their licenses under `ps5-slang-gpu/third_party/`. Embedded shader files are checked for permissive licenses during generation.
- Linking checks imports against SDK stubs and keeps the app's C++ runtime static. Explicit `--gpu` mode admits only the two additional graphics modules, `libSceAgc` and `libSceAgcDriver`. Incompatible modules, external C++ runtime imports and direct SDK syscalls remain rejected. Implemented RADV entry points are also checked after linking.

See [2.4 validation](VALIDATION-2.4.md) for the release artifact hashes and the scope of completed checks.
