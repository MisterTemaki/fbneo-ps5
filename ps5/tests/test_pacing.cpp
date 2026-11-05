// FBNeo PS5 2.3: the frame pacing's decisions (frontend/fe_pacing.h) on their own: when a frame of a game paced by the
// sound goes undrawn.
//
//   test_pacing
//
// SPDX-License-Identifier: MIT

#include "fe_pacing.h"

#include <cstdio>

int main()
{
	const int target = 2880; // 60 ms of 48 kHz, as fe_emu.cpp's kTargetQueued
	struct Case
	{
		bool busy;
		int queued, wait;
		bool last_skipped, skip;
		const char* what;
	} cases[] = {
		{false, 0, 0, false, false, "the picture's thread idle: drawn, whatever the sound"},
		{true, 2880, 0, false, false, "busy, the sound at its target, nothing left to wait: wait for it"},
		{true, 1439, 0, false, true, "busy, nothing left to wait, the sound just under half the target: not drawn"},
		{true, 1440, 0, false, false, "busy, nothing left to wait, the sound at half the target: wait"},
		{true, 2000, 600, false, true, "busy, the sound under the wait plus half the target: not drawn"},
		{true, 2040, 600, false, false, "busy, the sound at the wait plus half the target: wait"},
		{true, 0, 0, true, false, "the last frame not drawn: this one is (never two in a row)"},
		{true, 100, 5000, true, false, "the last frame not drawn, however low the sound: drawn"},
	};
	int ok = 0, failed = 0;
	for (const Case& c : cases)
	{
		const bool got = emu::SkipDrawing(c.busy, c.queued, c.wait, target, c.last_skipped);
		(got == c.skip ? ok : failed)++;
		printf("  %s: %s\n", got == c.skip ? "ok" : "FAIL", c.what);
	}
	printf("pacing test: %d checks ok, %d failed\n", ok, failed);
	return failed ? 1 : 0;
}
