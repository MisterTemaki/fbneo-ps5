// FBNeo PS5: what FBNeo's frontend headers give src/burner/dat.cpp and misc.cpp before burner.h's own (an SDL build
// gets them from burner_sdl.h). Forced in front of those two files (-include) for `fbneo_headless dat`; the app does
// not use them.
//
// SPDX-License-Identifier: MIT
#ifndef FBNEO_PS5_DAT_H
#define FBNEO_PS5_DAT_H

#include "tchar.h"

typedef struct tagRECT
{
	int left, top, right, bottom;
} RECT;

// burner_sdl.h's: the version the frontend prints (dat.cpp's header)
extern TCHAR szAppBurnVer[16];
// burner_sdl.h's: the driver is running (misc.cpp)
extern int bDrvOkay;

#endif
