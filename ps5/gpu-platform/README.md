<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 MisterTemaki -->
# The platform layer for RADV titles

`build/libps5rt_platform.a` (`make`): what a native PS5 title with Mesa's RADV linked in (an eboot that draws with
Vulkan) needs from the C library and the console's libc (libSceLibcInternal, libkernel) doesn't give. It replaces,
for such a title, the payload SDK fork's `libps5platform.a` and PS5_Vulkan's link recipe (`tools/radv-link.sh`),
both GPL-3.0-or-later. MIT, written from interface facts: [CLEANROOM.md](CLEANROOM.md).

Nothing here has run on a PS5 yet. It is built, unit-tested on Linux, and linked into a test title whose link is
checked; the console test is still to come.

## What it has

| file | functions | behaviour |
|---|---|---|
| `src/ps5rt_thread.c` | `__wrap_pthread_create` | a thread made without attributes gets a 2 MiB stack, not the console's small default; back to the default if the console refuses it. `ps5rt_set_thread_stack_size()` changes it, 0 turns it off (see "Thread stacks" below) |
| `src/ps5rt_memstream.c`, `src/ps5rt_stdio_wrap.c` | `open_memstream`, `__wrap_fclose`, `__wrap_fflush` | a FILE on an anonymous shared-memory object; the caller's buffer and size are brought up to date at fflush / fclose |
| `src/ps5rt_locale.c` | `newlocale`, `duplocale`, `freelocale`, the `*_l` functions libc++ and Mesa call, `__runes_for_locale`, `___runetype_l` / `___tolower_l` / `___toupper_l`, `localeconv`, `localeconv_l`, `catopen` / `catgets` / `catclose` | the C locale (also "POSIX" and ""), the only one a title has |
| `src/ps5rt_fs.c` | `fstatat`, `dirfd`, `readlink`, `posix_fallocate`; for libc++'s std::filesystem: `openat`, `unlinkat`, `fchmodat`, `utimensat`, `pathconf`, `link`, `symlink`, `isatty` | on stat / lstat / ftruncate / open / unlink / rmdir / chmod / utimes, for paths from AT_FDCWD or absolute ones; what can't be done is ENOTSUP / ENOSYS |
| `src/ps5rt_qsort_r.c` | `qsort_r` | BSD argument order, as RADV was built for |
| `src/ps5rt_misc.c` | `localtime_r`, `getpwuid_r`, `openlog`, `closelog`, `popen`, `pclose`, `dladdr` | what a title can have: no users, no processes, no symbol information |
| `third_party/compiler-rt-18.1.8/` | `__emutls_get_address`, `__cpu_model` | LLVM's own emutls.c and x86.c (Apache-2.0 WITH LLVM-exception), one line changed: its README |
| `libc-allow.txt`, `libc-notices.txt` | | the payload SDK `libc.a` members such a link may take (BSD-3-Clause, ISC), and their notices, which ship with the program |
| `link/` | | MIT linker script and version script for the PIE, in place of the boilerplate's ([link/README.md](link/README.md)) |
| `radv-shim/` | | MIT `ps5platform/agc.h`, `kernel.h`, `videoout.h` for building RADV, a probe against other headers, and the RADV build ([radv-shim/README.md](radv-shim/README.md)) |
| `tools/ps5rt-radv-link.sh` | | the link's flags, scripts and inputs, in order |
| `tools/check_link.py` | | the link's checks: imports, raw system calls, every input |
| `radv-title/radv_title.c` | | the test title (`make radv-title`) |
| `tests/` | | host tests (`make test`) |

Each source file's header says what each function does and why.

## Link

```
ld.lld -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu --eh-frame-hdr -e _start --no-dynamic-linker \
    $(ps5rt-radv-link.sh --scripts) \
    $(ps5rt-radv-link.sh --flags) -o title-pie.elf \
    ps5rt_start.o ps5rt_new.o <the program's objects> libvulkan_radeon.ps5.a $(ps5rt-radv-link.sh --archive) \
    $SDK/target/lib/libc++.a libc++abi.a libunwind.a --as-needed <the SDK's stubs, not libkernel_sys / libkernel_web> \
    -L $SDK/target/lib $SDK/target/lib/libc.a -lpthread @syscalls-y.rsp --trace >title-pie.trace
check_link.py imports title-pie.elf $SDK/target/lib [--allow-module ...]
check_link.py syscalls title-pie.trace
check_link.py trace title-pie.trace --sdk-lib $SDK/target/lib --libc-allow $(ps5rt-radv-link.sh --libc-allow) \
    --own <objects folder> --archive libvulkan_radeon.ps5.a --archive $(ps5rt-radv-link.sh --archive) --require ps5rt_start.o
```

The Makefile's `radv-title` target is this link, whole. RADV there is the one built against the MIT headers
(`radv-shim/`, `/home/claude/work/rsdeps/radv-mit/lib/libvulkan_radeon.ps5.a`). A program that wraps fclose / fflush itself (netfs) takes
`--flags --own-stdio-wrap` and calls `ps5rt_fclose` / `ps5rt_fflush` from its own wrappers.

The test title's link loads 7 members of the SDK's `libc.a`: `__xuname.o`, `assert.o`, `mktemp.o`, `regcomp.o`,
`regexec.o`, `regfree.o` (BSD-3-Clause) and `arc4random.o` (ISC); no GPL member, no `libps5platform.a`, no
start-up of BlackBearReloaded's boilerplate or of the SDK. A probe with libc++'s iostreams, locales, regex, fstream,
random_device and threads loads only `assert.o` and `arc4random.o` (no `no-locale.o`).

## Thread stacks

A thread made without attributes gets 2 MiB (`PS5RT_THREAD_STACK_DEFAULT`). It was 8 MiB, Linux's default, until the
second M2 review asked whether that much was needed: if the console commits thread stacks at once, every such thread
(RADV's, std::thread's, a program's audio and notification threads) takes 8 MiB of the title's memory. Measured on the
PC on 2026-11-07, each thread made without attributes given an 8 MiB stack filled with a pattern, its deepest byte read
at its end (an LD_PRELOAD interposer of pthread_create, not committed):
- RADV (Mesa 25.2.8, its null winsys as a GFX1013, the PS5's GPU family), the 139 graphics pipelines of all 28 shader
  presets of Genesis Plus GX PS5 2.2.1 (Mega Bezel, crt-royale and crt-guest-advanced among them) compiled on such a
  thread with the shader cache on: that thread used at most 81 KiB, RADV's shader cache threads (`disk$`) 17 KiB.
- Genesis Plus GX's GPU renderer on lavapipe (its host build, AddressSanitizer on, crt-royale compiled and drawn): the
  program's own threads used at most 63 KiB, lavapipe's 35 KiB.

The PS5's RADV adds threads the PC's doesn't run (the winsys's submission kick, the VideoOut flip thread); both are
loops around one system call. 2 MiB leaves a margin of 25 over the deepest thread measured. A program that needs more
calls `ps5rt_set_thread_stack_size()`.

## Not done, on purpose

- **The heap.** The title keeps the system's malloc (the MIT runtime's `libc.prx` hands it to libkernel). Nothing
  known says it runs out with RADV: RADV's GPU memory is direct memory it allocates itself, and its CPU-side
  allocations are small next to what the emulators already take from malloc on consoles. Replacing malloc would also
  mix two heaps (the system libc's own `strdup`, `getline`, ... allocate from its heap). If the console test shows the
  heap running out, a direct-memory heap is the next step. The test title logs the direct-memory pool to help see it.
- **The directory functions.** The system's `opendir` / `readdir` / `closedir` are proven on consoles by every port, so
  they stay; only `dirfd`, which the console lacks, is here (ENOTSUP: the system's DIR is opaque).

## To try on a console

The test title (`build/radv-title/app/`: `eboot.bin`, `sce_module/libc.prx`; an app folder still needs its `sce_sys/`)
writes each step to `/data/ps5rt-radv-title.txt`: instance, device, display mode, swapchain, the first frame, 180
frames timed, the direct-memory pool. What would show up there first:
- `shm_open(SHM_ANON)` (open_memstream) works in a title;
- a 2 MiB stack is granted to RADV's threads;
- libkernel runs at least two pthread-key destructor rounds (else each thread's TLS block is never freed);
- RADV is happy with the system heap.
