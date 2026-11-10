# FBNeo PS5 2.4 validation

This release combines GPU presentation, experimental rollback netplay and compatibility/stability fixes. The emulation core remains FBNeo 1.0.0.3. Console evidence below comes from user-supplied PS5 logs; automated frontend tests run on Linux with simulated PS5 services.

## Initial PS5 hardware results

Logs supplied on November 10, 2026 show the 2.4 installer replacing the previous 2.3 app, writing the new metadata and GPU notices, and installing a 73,447,566-byte `eboot.bin`. Both subsequent boots identify FBNeo PS5 2.4 and initialize `PlayStation 5 GPU (RADV NAVI21)` through Vulkan/RADV.

VideoOut selects **3840x2160 at 59.940 Hz**, with three presentation images. The observed filter is **CRT Easymode style**, running as one GPU pass. This is the final output resolution; game framebuffers and the Model 2 rasterizer remain CPU-generated.

| Game | Observed results |
|---|---|
| Daytona USA | Target 57.520 fps; samples of 57.6, 57.5 and 57.5 fps; GPU filter 0.4–0.5 ms; zero skipped frames and silence refills in the sampled reports |
| Cadillacs and Dinosaurs | Initial sample 62.1 fps, then 59.9 and 59.9 fps; GPU filter 0.5 ms |
| Crime Fighters | Started successfully; one initial sample of 63.1 fps and a 0.6 ms GPU filter; no sustained-rate claim from this short sample |
| Contra | Vertical game; initial sample 62.5 fps, then 59.9 and 59.9 fps; GPU filter 0.4–0.5 ms |

Both logged app sessions end with **exit status 0**. No GPU error, device loss, CPU recovery or crash signal is recorded. Daytona loads and writes NVRAM successfully. Audio underrun counts stay constant within the sampled sequences for Daytona, Cadillacs and Contra; the cumulative counter also includes intentional silence during menus and pauses.

These are initial hardware results. They do not establish visual correctness for every filter, compatibility with every board, extended-session stability, physical GPU-failure recovery or netplay between two PS5 consoles. The supplied logs contain no netplay session.

## Automated checks

| Check | Result |
|---|---|
| Native build/imports/C++ runtime | Passed |
| Direct SDK syscalls in the app | None found by the link checker |
| Implemented RADV entry points retained | 338 |
| Real app files embedded in the installer | All 7 verified byte for byte, including decompression |
| Intermediate PIE presented as an installer | Correctly rejected |
| GPU adapter on Vulkan/Lavapipe, ASan/UBSan | 26 checks, 0 failures |
| Complete Vulkan frontend, groups 1, 2 and 6 | 53 passed, 0 failed |
| Software frontend regressions, groups 1, 2, 6, 12, 29, 36, 44, 46 and 47 | 206 passed, 0 failed |
| Audio and pacing frontend, group 7 | 25 passed, 0 failed |
| Final 2.4 installer/update/frontend checks, groups 1, 2, 9, 27 and 44 | 90 passed, 0 failed |
| Simulated 2.3 to 2.4 upgrade | App, metadata and notices updated; ROM, state and settings fixtures preserved byte for byte |
| CPU-only Linux build and game smoke test | Passed; no Vulkan loader dependency |
| C platform layer and compiler-rt helpers | 318 and 27 checks, 0 failures |
| i960/TGP reference comparison and Model 2 video | 77 and 17 checks, 0 failures |
| Netplay unit tests | 35 checks passed |
| Two complete Vulkan frontends over local TCP | Both exit 0; real rollback/replay, pause, disconnect and offline-state restoration passed |
| 7z / CHD-HDD / CRT pool / pacing | 50 / 74 / 3 / 8 checks passed |
| MIPS III | Four configurations and 20 random programs agreed |
| Allocation, configuration, update, audio and image-resize regressions | Passed |
| Update ZIP | Integrity and all 7 manifest hashes verified |
| DAT | All 8,393 game definitions preserved; release metadata updated |

These totals describe separate runs and must not be added as a count of unique tests. Earlier development runs and final packaging checks overlap. Lavapipe is a software driver; its timings are not PS5 performance measurements. GPU/frontend test processes disable leak detection while retaining ASan/UBSan memory-access and undefined-behavior checks.

## Artifacts

The installer and app identify the port as **2.4**, with title ID `PPSA99924`, content version `02.040.001`, and source build ID:

```text
8556ec07bf31519c82c789fecb746edbdf62e24166440c96b78437ecb8b5e791
```

| File | Bytes | SHA-256 |
|---|---:|---|
| `FBNeoPS5-v2.4.elf` | 30,572,728 | `cc666b4a6282ad3c49182e28e7356934b444d661372d450a42521a34ce39a41e` |
| `FBNeoPS5-v2.4-PPSA99924.zip` | 91,941,418 | `8594d2586b6d280642a4373222afb88ed830605d56cb389eb6e23dfb0b840743` |
| `FBNeoPS5-v2.4-arcade.dat` | 14,164,662 | `3f95e8e3754d9db3f8e46bc263ba56f08e7c919d04af1b5a882b63230ef8b4f6` |

The ELF is the complete installer. Intermediate `fbneo-pie.elf` and `fbneo-gpu-pie.elf` files are not release installers. Private raw logs and local audit working files are not part of the public source or release assets.
