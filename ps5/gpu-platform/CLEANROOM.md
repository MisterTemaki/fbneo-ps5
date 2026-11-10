<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 MisterTemaki -->
# How the platform layer was written

Goal: what a RADV title needs that the PS5's system libc doesn't give, in place of the payload SDK fork's
`libps5platform.a` (mihawk-99/PS5_PayloadSDK, `platform/`) and PS5_Vulkan's `tools/radv-link.sh`, both
GPL-3.0-or-later. Everything here was written fresh, under MIT, from interface facts, by the method of the runtime's
[CLEANROOM.md](../CLEANROOM.md). Written by Claude (Anthropic) for MisterTemaki, November 2026.

## Not opened

- The SDK fork's `platform/src/*` and its `platform/` headers, and PS5_Vulkan's runtime: not opened, read or copied.
- The GPL members' sources in the SDK's `libc/` (`qsort_r.c`, `popen.c`, `dirfd.c`, `passwd.c`, `localtime.c`,
  `syslog.c`, `no-locale.c`, `syscalls.c`, `dlfcn.c` ...): only their licence lines were seen (below).
- The fork's installed headers `ps5platform/*.h` (which RADV's sources include): only their file names and licence
  lines (`SPDX-License-Identifier: GPL-3.0-or-later`, `Copyright (C) 2026 Mihawk`).

## What was seen of GPL files, to be exact

- **PS5_Vulkan `tools/radv-link.sh`** (the recipe the brief allowed as a list of names): its licence (GPL-3.0, from
  `LICENSE`), its line count, and, through `grep -o`, its link flags and the names its loops bind: `--wrap=` for
  pthread_create, pthread_join, pthread_detach, fclose, fflush and a loop of names; `--defsym=$name=ps5_$name` over
  three loops; `--defsym=localeconv=ps5_localeconv`; `--start-group`, `--end-group`, `--whole-archive`,
  `--version-script`; the six loop headers (`for name in ...`, `for flag in "${radv_link_flags[@]}"`,
  `for file in "$archive" "$platform" "$sdk_root/target/lib/libc++.a"`); the names (the heap functions malloc ...
  reallocarray, getline, getdelim, qsort_r, mkstemps, openlog, popen, pclose, open_memstream, `__xuname`, `__assert`,
  the regex functions, localtime_r, newlocale, freelocale, the `*_l` family, catopen / catgets / catclose,
  backtrace, `__cxa_thread_atexit_impl`, arc4random*, gmtime_r, statvfs, futimens, clock_nanosleep, getaddrinfo,
  if_nameindex, the directory functions, nl_langinfo(_l), getpwuid_r, posix_fallocate, access and the `*at` calls,
  memfd_create, strcasestr, mkstemp, readlink, link, symlink). No other line of the script was shown.
  Used: only as a list of areas to check. What this layer defines was decided from the link itself (below); the
  heap, `opendir` & co, `getline`, `access` and others on that list are not replaced.
- **The fork's git log**: `git log --oneline -3` showed three commit subjects: "platform: the title heap usable
  without its wraps", "platform: the title heap in arenas, one for each allocating thread", "platform: access, which
  a title's sandbox refuses". Nothing was taken from them.
- **The SDK's `libc/` sources** (in the fork and in the upstream ps5-payload-dev/sdk): for each file, the first
  licence keyword and copyright line of its first 40 lines; for the eight members used, their whole copyright comment
  (they are BSD-3-Clause and ISC; `libc-notices.txt`). `fnmatch.c`'s first 12 lines (its header comment, no licence),
  and the licence lines of `emutls.c`, `pmadvise.c`, `tfind.c`, `twalk.c`.
- **The SDK's `prospero-clang`** (GPL wrapper script): its first 30 lines, while finding the compiler. Nothing used.
- **RetroStation's `tools/link-launcher.sh`** (its committed m2 head, the lines a `grep` for radv / compiler-rt /
  emutls / wrap / defsym / libc.a matched): it links RADV with PS5_Vulkan's recipe and the fork's platform, and builds
  compiler-rt 18.1.8's emutls.c with one destructor round skipped and x86.c. Seen before the emutls change here was
  made; that change rests on upstream's own comment for Bionic (third_party/compiler-rt-18.1.8/README.md).

## Facts used, and where they came from

1. **Symbol lists** (`llvm-nm`):
   - what RADV's archive (`libvulkan_radeon.ps5.a`, PS5_Mesa 7b59ef2, release) leaves undefined and defines;
   - what the SDK's `libc++.a`, `libc++abi.a`, `libunwind.a` leave undefined;
   - what each system-module stub (`target/lib/*.so`) defines: so what the console's libc lacks;
   - what each SDK `libc.a` member defines and references (for `no-locale.o`, the names of the xlocale functions
     libc++ calls).
2. **The link** (`ld.lld --trace`, `--why-extract`, `-y`): trial links of the test title against RADV, the runtime,
   libc++, the stubs and `libc.a`, which named the 20 symbols missing and which `libc.a` member each would pull in.
3. **RADV's own sources (MIT)**, PS5_Mesa at 7b59ef2:
   - meson.build's `qsort_r` test (BSD form found), and the call sites in the archive (`objdump`: context fourth,
     `util_qsort_adapter` fifth);
   - `util/disk_cache_os.c` (dirfd + fstatat for the LRU scan, getpwuid_r for the cache folder, posix_fallocate),
     `util/anon_file.c` (`shm_open(SHM_ANON)` on FreeBSD), `util/disk_cache.h` (dladdr, and the build-id override),
     `amd/common/ac_gpu_info.c` (readlink);
   - `vulkan/wsi/wsi_common_videoout.c` and `amd/vulkan/winsys/ps5/*` (VK_KHR_display on VideoOut, the direct-memory
     pool, `radv_ps5_winsys_heap_usage`), for the test title. Their calls are to sce* exports only; the three
     `ps5platform` headers they include supply constants, structure layouts and prototypes (seen from the names the
     sources use, not from the headers).
4. **The SDK's headers** (FreeBSD 11.4, BSD licences): the prototypes of every function defined here, `_RuneLocale`
   and the `_CTYPE_` bits (`<runetype.h>`, `<_ctype.h>`, `<xlocale/_ctype.h>`: what the inline `isalpha_l` ... read),
   `struct lconv`, the `LC_*_MASK` values, `SHM_ANON`, `AT_FDCWD` / `AT_SYMLINK_NOFOLLOW`, the errno values. A diff of
   the include trees showed the fork's equal to /opt's but for `sys/_ucontext.h`, `sys/mman.h` and `ps5platform/`.
5. **Our own code**: the ports' `ProsperoThread.h` ("std::thread maps to a pthread with the SDK's default stack, which
   is small"), and the ports' RetroStation link (their Makefiles: link order, linker scripts, check tools).
6. **compiler-rt 18.1.8** from LLVM's repository (third_party/compiler-rt-18.1.8/README.md: paths and hashes).
7. **Public specifications**: POSIX.1-2008/2017 (open_memstream, fstatat, dirfd, readlink, posix_fallocate,
   getpwuid_r, localtime_r, popen, catopen / catgets, newlocale / freelocale, the C / POSIX locale), C11 (the "C"
   locale's character classes and `struct lconv` values), libc++'s use of `_CTYPE_` masks as `wctype_t` on FreeBSD.

## Added for Genesis Plus GX (phase 2)

glslang's `InfoSink.h` uses `std::filesystem::absolute`, which brings libc++'s `operations.cpp.o` into the link, and
with it `openat`, `unlinkat`, `fchmodat`, `utimensat`, `pathconf`, `link`, `symlink` and `isatty`, which the console's
libc lacks (`llvm-nm` of that object against the stubs). The 2.2.1 GPU build took them from the SDK fork's platform
layer; here they are written from POSIX, on the calls a title's libkernel has (`src/ps5rt_fs.c`).

## Choices made without a fact (to try on a console)

- 2 MiB for threads made without attributes (8 MiB, Linux's default, before the second M2 review; README.md "Thread
  stacks" has the measurement); the console's default if it refuses.
- open_memstream's object: `shm_open(SHM_ANON)` first, a named object unlinked at once next.
- The system heap stays (README.md, "Not done, on purpose").
- `dirfd` is ENOTSUP and `fstatat` relative to a folder descriptor ENOTSUP: the system's DIR is opaque, and a title
  has no openat.
- The locale functions take the global locale to be "C", as the titles leave it; this `localeconv` returns the C
  values and replaces the system's in a program that links the locale object.
- emutls frees a thread's block in the second pthread-key destructor round.

## The ps5platform headers (radv-shim/)

RADV's sources (MIT) include three headers of the fork that are GPL-3.0-or-later: `ps5platform/agc.h`, `kernel.h` and
`videoout.h` (`radv_ps5_platform.c`, `wsi_common_videoout.c`). `radv-shim/include/ps5platform/` replaces them, and
RADV is built again against the replacements (`radv-shim/build-radv-mit.sh`).

Not opened: the three headers' text (nor the fork's other `ps5platform/` headers). Facts used:
1. **What RADV uses of them**: the identifiers its two files name (every `PS5_*` constant, the two structures and the
   members they set, the sce* functions they call), in Mesa's sources at 7b59ef2 and in the patched tree the archive
   was built from (db54455), which name the same ones.
2. **The WSI's host model** (`wsi_common_videoout.c`, MIT): for host builds it defines the VideoOut constants,
   `struct ps5_video_out_buffer` and the nine VideoOut functions with their parameters. `videoout.h` is those.
3. **RADV's compiled objects** (the existing release archive, MIT code): the immediates at the call sites give the
   kernel constants' values and `sceAgcInit`'s argument; their debug information gives
   `struct ps5_agc_submit_description`'s layout. That debug information also shows the names the fork gives that
   structure's last two members ("flag", "padding"): seen, not used (the stand-in has its own names, RADV sets
   neither).
4. **The console's convention** for the GPU protection bits (read 0x10, write 0x20), as homebrew documents it, and
   FreeBSD's `PROT_READ` / `PROT_WRITE` for the CPU's.
5. **Compile probes** (`radv-shim/probe.py`): each stand-in item compiled against the fork's header, the diagnostics
   discarded, only same / different kept. 45 items; one (`sceKernelGetDirectMemorySize`'s return type, first written
   `size_t`) differed and was changed to `int64_t`, as RADV's call sites use it.

Build tooling read to rebuild RADV: PS5-ECAM 5.1.0's `tools/build-radv.sh` (GPL-3.0-or-later, a build script: how the
patched tree and its build are made), the recorded meson options (`radv-ps5-release.radv-options`), the cross-file
constants and PS5_Vulkan's `tooling/radv/ps5-cross.ini` (compiler paths, host machine). `build-radv-mit.sh` was
written from them; it runs Mesa's own meson build.

## The linker scripts (link/)

`link/ps5rt-pie.ld` and `link/ps5rt-local.map` replace the boilerplate's `ps5-pie.ld`, `ehframe.ld`,
`app-symbols.map` and `localize.map`. Not opened: those four files' text. Facts used: the output of the link that
names them (the test title's PIE and the ports' PIEs, through `readelf` and `nm`: the program headers, the section
order, addresses and alignments, the dynamic table, which symbols exist and where: the init / fini array bounds and
`__eh_frame*` that libunwind references, 16 bytes of padding before `_start` at 0x10, no exported symbol), and what
`ps5rt_start.cpp` and libunwind reference. Check: the same loaded bytes, and the same eboot.elf after
`ps5-native-tool link` (link/README.md).

ps5-native-tool (BlackBearReloaded's, GPL) stays as the build tool that turns the PIE into eboot.elf and signs it:
the review found that it injects no code into the eboot.
