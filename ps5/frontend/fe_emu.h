// FBNeo PS5 frontend: the running game -- FBNeo's core (fe_burn.h) with the PS5's picture, sound and pads.
//
// One RunFrame() is one frame of the arcade machine, called from the frontend's thread. The pads go to the
// game through the button layout (Settings, CONTROLS); the picture (turned the right way up for vertical games)
// goes through the CRT shaders to the screen; the sound (48 kHz) to the audio ring.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_burn.h"

#include <string>
#include <vector>

namespace emu
{
// The choices the settings menu offers.
struct Choice
{
	const char* name;
	const char* value;
};
extern const Choice kAspects[]; // the game's monitor (4:3, or 3:4 for vertical games), square pixels, 16:9
extern const int kAspectCount;
extern const Choice kLayouts[]; // auto, classic, fighting (6 buttons), custom, fighting with R1 R2
extern const int kLayoutCount;
constexpr int kCustomLayout = 3;
// The layouts in the order the settings screen steps through them (custom last).
int NextLayout(int layout, int dir);

// The PS5 buttons an arcade button can go to.
struct PadButton
{
	const char* name; // "Square"
	unsigned bit; // SCE_PAD_BUTTON_*; 0 = not assigned
};
// Cross, Circle, Square, Triangle, L1, R1, OPTIONS, touchpad, (none), L2, R2 -- L2 and R2 came last (1.6) so the
// numbers kept in fbneo-ps5.ini (btn_1=...) still mean the same buttons
extern const PadButton kPs5Buttons[];
extern const int kPs5ButtonCount;
constexpr int kNoButton = 8;
// The PS5 buttons in the order the settings screen steps through them (L2, R2 after R1; none last).
int NextPs5Button(int button, int dir);
// The arcade buttons of the custom layout (Settings::buttons), in its order.
struct ArcadeButton
{
	const char* name; // "Button 1"
	const char* key;  // its key in fbneo-ps5.ini: "btn_1"
};
constexpr int kArcadeButtonCount = 8; // Button 1..6, Coin, Start
extern const ArcadeButton kArcadeButtons[kArcadeButtonCount];
// The PS5 button (index in kPs5Buttons) of arcade button i (0..7) in layout `layout` for a game with
// `game_buttons` buttons (auto picks the fighting layout for 6-button games).
int LayoutButton(int layout, int i, int game_buttons);

bool InitCore();
void DeinitCore();

// Starts the set whose zip is `path` (its parent and BIOS sets are found in the library). On failure, *error
// says why (the missing ROMs listed).
bool LoadGame(const std::string& path, std::string* error);
void CloseGame(); // writes the NVRAM and ends the game
bool GameLoaded();
std::string GameName();   // the set: "sf2ce"
std::string GameTitle();  // "Street Fighter II': Champion Edition (World 920513)"
std::string SystemName(); // the board: "CPS1"

// Settings from fe::Config() -> the core and the frontend's picture/sound settings.
void ApplySettings();

enum class FrameResult
{
	Continue,
	OpenMenu, // L3 + R3 pressed together
	Stopped, // no game
};
// One frame: the pads and hot keys, the core's frame, the picture (and the messages) on the screen, the sound to
// the audio ring. Games at 58.5..61.5 Hz run at the display's 60 Hz (vsync paces them, the sound is resampled to
// match); the others are paced by the sound. The one hot key: L3 + R3 opens the pause menu (states, DIP switches,
// the machine's service button and test switch...); L2 and R2 are buttons like the others (1.7).
FrameResult RunFrame();

void Pause(); // nothing runs between frames; kept for the menus' symmetry
void Resume();

bool SaveState(int slot); // 1..10, /data/fbneo/states/<set>.state<slot>
bool LoadState(int slot);
bool StateExists(int slot);
void Reset(); // the machine's reset button
void PowerCycle(); // the game loaded again, as switching the machine off and on
// The machine's service button / test switch, held for a quarter of a second once the game runs again.
void PressService();
void PressTest();
void Osd(const std::string& text); // a message over the game for a few seconds

// The game's DIP switches (the pause menu): changes are kept per game in /data/fbneo/config/<set>.dip.
std::vector<burn::DipGroup> Dips();
void SetDip(int group, int option);
void ResetDips();
// The names the driver gives player 1's buttons ("Weak Punch"), "" past its last button.
std::string GameButtonName(int i);
int GameButtonCount();

// Redraws the last frame into the surface (the pause menu draws over it).
void RedrawLastFrame();
} // namespace emu
