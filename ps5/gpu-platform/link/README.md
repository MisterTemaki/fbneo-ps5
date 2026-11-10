<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 MisterTemaki -->
# Linker scripts for a native PS5 title's PIE

MIT replacements for the boilerplate's `ps5-pie.ld`, `ehframe.ld`, `app-symbols.map` and `localize.map`
(BlackBearReloaded, GPL-3.0-or-later), which the ports' links name today:

- `ps5rt-pie.ld`: the sections and segments (the table at its top), the init / fini arrays' bounds for the start-up,
  the `.eh_frame_hdr` / `.eh_frame` bounds libunwind reads, 16 bytes of padding at address 0 so that no function's
  address is the null pointer, and a check that nothing has thread-local data.
- `ps5rt-local.map`: every symbol local; the dynamic symbol table holds only the imports.

`platform/tools/ps5rt-radv-link.sh --scripts` gives the flags (`-T .../ps5rt-pie.ld --version-script
.../ps5rt-local.map`), used with `-pie -z max-page-size=0x4000 --hash-style=gnu --eh-frame-hdr -e _start
--no-dynamic-linker`.

Written from the output the eboots' link has (the RADV test title's PIE, and the ports' PIEs: their program headers,
section table, dynamic table, symbols), not from the boilerplate's scripts' text. Check: the RADV test title linked
with these scripts and with the boilerplate's has the same ELF header, program headers, section table and dynamic
table and the same bytes in every loaded section, and `ps5-native-tool link` makes the same eboot.elf from both
(sha256 `49baecc0...`).

ps5-native-tool itself (BlackBearReloaded's) stays: it is a build tool, and the eboot it writes holds no code of its
own (the review found it injects none: it writes the SCE dynamic tables and process parameters around the PIE).
