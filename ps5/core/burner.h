// FBNeo PS5: what lowpass2.cpp (the one file of FBNeo's frontend the drivers use) takes from burner.h.
//
// SPDX-License-Identifier: MIT
#ifndef FBNEO_PS5_BURNER_H
#define FBNEO_PS5_BURNER_H
#include "burnint.h"
// paused: the frontend runs no frames then (lowpass2 checks it); FBNeo PS5 never filters while paused
extern INT32 bRunPause;
#endif
