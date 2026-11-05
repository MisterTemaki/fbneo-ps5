// FBNeo PS5 frontend: the frame pacing's decisions that tests can check on their own (2.3).
//
// SPDX-License-Identifier: MIT
#pragma once

namespace emu
{
// Whether a frame of a game paced by the sound goes undrawn (its picture is drawn on a thread of its own, fe_emu.cpp):
// that thread is still drawing the last one, the sound queued is less than the wait for it (its expected rest) plus
// half the target, and the last frame was drawn (never two in a row). Sound in frames of 48 kHz.
inline bool SkipDrawing(bool picture_busy, int queued, int expected_wait, int target, bool last_skipped)
{
	return picture_busy && !last_skipped && queued < expected_wait + target / 2;
}
} // namespace emu
