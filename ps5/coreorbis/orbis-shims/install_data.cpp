// FBNeo PS5: the dashboard app built into FBNeoPS5.elf (ProsperoInstall.h).
//
// The Makefile passes the files' paths: eboot.bin (FBNeo and its frontend) and the two DDS backgrounds packed by tools/pack_file.py,
// sce_sys/param.json, sce_sys/icon0.png (the FBNeo PS5 icon, ps5/app/sce_sys/icon0.png),
// sce_sys/pic0.dds + pic1.dds (the home screen backgrounds) and sce_module/libc.prx (the C runtime module a native title carries, as PS5SX2's app folder). Only the final FBNeoPS5.elf links this file.
//
// SPDX-License-Identifier: MIT

#include "ProsperoInstall.h"

#define FBNEO_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const unsigned char sym##_begin[];                                                              \
	extern "C" const unsigned char sym##_end[];

FBNEO_INCBIN(fbneo_app_eboot, APP_EBOOT_Z)
FBNEO_INCBIN(fbneo_app_param, APP_PARAM)
FBNEO_INCBIN(fbneo_app_icon0, APP_ICON0)
FBNEO_INCBIN(fbneo_app_pic0, APP_PIC0_Z)
FBNEO_INCBIN(fbneo_app_libc, APP_LIBC)

const EmbeddedAppFile* EmbeddedAppFiles()
{
	static const EmbeddedAppFile files[] = {
		{"sce_sys/param.json", fbneo_app_param_begin, fbneo_app_param_end, false},
		{"sce_sys/icon0.png", fbneo_app_icon0_begin, fbneo_app_icon0_end, false},
		{"sce_sys/pic0.dds", fbneo_app_pic0_begin, fbneo_app_pic0_end, true},
		{"sce_sys/pic1.dds", fbneo_app_pic0_begin, fbneo_app_pic0_end, true}, // the same image, embedded once
		{"sce_module/libc.prx", fbneo_app_libc_begin, fbneo_app_libc_end, false},
		{"eboot.bin", fbneo_app_eboot_begin, fbneo_app_eboot_end, true},
		{nullptr, nullptr, nullptr, false},
	};
	return files;
}
