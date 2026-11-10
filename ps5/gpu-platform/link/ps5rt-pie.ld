/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * The layout of a native PS5 title's PIE, before ps5-native-tool link turns it into eboot.elf (ld.lld -pie
 * -z max-page-size=0x4000 --eh-frame-hdr -T ps5rt-pie.ld --version-script ps5rt-local.map). Written for
 * ps5-native-runtime-mit from what the eboots' link needs (platform/link/README.md):
 *
 *   segment  flags  sections
 *   text     R E    .text, from address 0 (the ELF header and program headers are in no segment), its first
 *                   16 bytes padding: no function is at address 0, the null pointer
 *   rodata   R      .eh_frame_hdr, .eh_frame, .gcc_except_table, .rodata
 *   relro    RW     .data.rel.ro, .got, .preinit_array, .init_array, .fini_array, the dynamic symbol, hash,
 *                   string and relocation tables
 *   data     RW     .dynamic, .data, .bss
 *   dynamic         .dynamic
 *
 * each LOAD segment on a 16 KiB page of its own. Symbols for the start-up (ps5rt_start.cpp: the three init / fini
 * arrays' bounds, hidden) and for libunwind, which finds the unwind tables through them rather than through the
 * loader (__eh_frame_hdr_start / _end, __eh_frame_start / _end). A thread-local section is refused: the SDK's
 * compiler emulates TLS (-femulated-tls), and an eboot has no TLS segment.
 */
PHDRS
{
	text PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	relro PT_LOAD FLAGS(6);
	data PT_LOAD FLAGS(6);
	dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
	. = 0;
	/* nothing at address 0: no function's address is the null pointer (the first 16 bytes are padding, a trap) */
	.text : ALIGN(0x4000) { . = . + 0x10; *(.text .text.* .gnu.linkonce.t.*) } :text

	. = ALIGN(0x4000);
	.eh_frame_hdr : ALIGN(0x4000)
	{
		__eh_frame_hdr_start = .;
		*(.eh_frame_hdr)
		__eh_frame_hdr_end = .;
	} :rodata
	.eh_frame :
	{
		__eh_frame_start = .;
		KEEP(*(.eh_frame))
		__eh_frame_end = .;
	} :rodata
	.gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :rodata
	.rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) } :rodata

	. = ALIGN(0x4000);
	.data.rel.ro : { *(.data.rel.ro.local .data.rel.ro.local.*) *(.data.rel.ro .data.rel.ro.*) } :relro
	.got : { *(.got) *(.igot) *(.got.plt) *(.igot.plt) } :relro
	.preinit_array :
	{
		PROVIDE_HIDDEN(__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN(__preinit_array_end = .);
	} :relro
	.init_array :
	{
		PROVIDE_HIDDEN(__init_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP(*(.init_array .ctors))
		PROVIDE_HIDDEN(__init_array_end = .);
	} :relro
	.fini_array :
	{
		PROVIDE_HIDDEN(__fini_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
		KEEP(*(.fini_array .dtors))
		PROVIDE_HIDDEN(__fini_array_end = .);
	} :relro
	.dynsym : { *(.dynsym) } :relro
	.gnu.hash : { *(.gnu.hash) } :relro
	.hash : { *(.hash) } :relro
	.dynstr : { *(.dynstr) } :relro
	.rela.dyn : { *(.rela.dyn) *(.rela.plt) *(.rela.*) } :relro

	. = ALIGN(0x4000);
	.dynamic : { *(.dynamic) } :data :dynamic
	.data : { *(.data .data.* .gnu.linkonce.d.*) } :data
	.tdata : { *(.tdata .tdata.* .gnu.linkonce.td.*) } :data
	.tbss : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } :data
	.bss : { *(.dynbss) *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON) } :data

	/DISCARD/ : { *(.interp) *(.note.GNU-stack) *(.gnu_debuglink) }
}

ASSERT(SIZEOF(.tdata) == 0 && SIZEOF(.tbss) == 0, "ps5rt-pie.ld: thread-local data: build with -femulated-tls (an eboot has no TLS segment)");
