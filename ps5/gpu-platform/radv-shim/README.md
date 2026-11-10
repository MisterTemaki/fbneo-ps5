<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 MisterTemaki -->
# MIT ps5platform headers for building RADV

RADV's PS5 sources (Mesa, MIT: `src/amd/vulkan/winsys/ps5/radv_ps5_platform.c` and
`src/vulkan/wsi/wsi_common_videoout.c`) include three headers of the payload SDK fork, `ps5platform/agc.h`,
`kernel.h` and `videoout.h`, which are GPL-3.0-or-later. `include/ps5platform/` has MIT stand-ins with what those two
files use and nothing else: 18 constants, 2 structures and 22 prototypes of the console's own sce* exports. RADV built
against them (`build-radv-mit.sh`) has nothing of the fork in it.

How they were written: [../CLEANROOM.md](../CLEANROOM.md), "The ps5platform headers". The fork's headers' text was
not opened.

| header | what | where each item comes from |
|---|---|---|
| `videoout.h` | 9 constants, `struct ps5_video_out_buffer`, 9 VideoOut prototypes | the WSI's own host model (`wsi_common_videoout.c`, `#if !defined(__PROSPERO__)`), which defines these same constants, the structure and these functions, with their parameters, to run the swapchain on a PC |
| `kernel.h` | 8 constants, 8 prototypes | the values from RADV's compiled objects (release archive, the immediates at the call sites): protection `0x33` for CPU read / write + GPU read / write, `0x3` for CPU read / write, map flag `0x10` (fixed), alignment `0x4000` (the page), the least direct-memory alignment `0x10000` (`MAX2(alignment, ...)`), direct-memory type `12` (VideoOut's buffers). The CPU bits are FreeBSD's `PROT_READ` / `PROT_WRITE`; the GPU's two are read `0x10` and write `0x20`, the console's convention as homebrew documents it (RADV always asks for both, so only their sum `0x30` reaches the kernel). The prototypes: the call sites' arguments and how they use the results |
| `agc.h` | 1 constant, `struct ps5_agc_submit_description`, 5 AGC prototypes | `sceAgcInit`'s argument `8` (the compiled call); the structure's layout (16 bytes: the words' pointer, their count at 8, a byte at 12, three bytes) from the debug information of RADV's compiled winsys; RADV fills only `words` and `word_count`. The prototypes: the call sites |

`probe.py <include-dir>` compiles, one item at a time, a file that includes another set of headers and then states
each stand-in item (a macro's value in a `_Static_assert`, a prototype declared again, a structure's size and the
offsets of the members RADV uses). It prints only "same" or "DIFFERENT" per item: the compiler's diagnostics are
discarded, so the other headers' text is never shown. Against the fork's headers: 45 items, all the same (one, the
return type of `sceKernelGetDirectMemorySize`, was first written as `size_t` and differed; it is `int64_t`, which
is how RADV's call sites treat it: `pool <= 0`).

`build-radv-mit.sh <out>`: RADV again, as the existing archive was built (same patched Mesa tree, options, cross
file, build id, SDK), with only these headers in place of the fork's (in a copy of the SDK without `ps5platform/` or
`libps5platform.a`). Its PROVENANCE.txt names everything.

Result (`/home/claude/work/rsdeps/radv-mit`, PROVENANCE.txt): against the archive the launcher links
(`PS5_Vulkan/.deps/native/radv-ps5cemu`, the same tree built with the fork's headers), `compare-archives.sh` finds
all 703 members byte-identical once their debug information is stripped; the same defined and undefined symbols; and
the RADV test title links to the same eboot.elf with either archive.
