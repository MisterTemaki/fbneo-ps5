// FBNeo PS5 frontend: settings kept in /data/fbneo/fbneo-ps5.ini (key=value lines).
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace fe
{
struct Settings
{
	// video
	int scale = 0; // ps5video::Scale: fit, integer, stretch
	int shader = 1; // ps5crt::Shader: 0 off, 1 "CRT Easymode style" (the default for every game), ...
	int aspect = 0; // emu::kAspects: the game's monitor, square pixels, 16:9
	bool smooth = false; // bilinear when the scale isn't whole
	bool scanlines = false;
	bool show_fps = false;
	// audio
	bool audio = true;
	int volume = 100; // 0..100
	// emulation
	int ff_speed = 300; // % while R2 is held; 0 = as fast as possible
	bool hiscores = true; // FBNeo's high score saving (needs hiscore.dat in /data/fbneo/hiscore)
	int state_slot = 1; // 1..10
	// controls: the button layout (emu::kLayouts: auto, classic, fighting, custom) and the custom one: the PS5
	// button (index in emu::kPs5Buttons) of each arcade button, in emu::kArcadeButtons' order (Button 1..6, Coin, Start)
	int layout = 0;
	static constexpr int kButtons = 8;
	static constexpr int kDefaultButtons[kButtons] = {0, 1, 2, 3, 4, 5, 7, 6}; // Cross Circle Square Triangle L1 R1, touchpad, OPTIONS
	int buttons[kButtons] = {0, 1, 2, 3, 4, 5, 7, 6};
	void DefaultButtons();
	// library
	bool covers_download = true; // fetch flyers from libretro-thumbnails
	bool show_clones = true; // the clones (other versions) of a game
	bool show_incomplete = false; // the sets with ROMs missing
	bool debug_logs = true;      // boot.log and the others in /data/fbneo/logs (OrbisLogSetEnabled)
	int shelf_family = 0; // fe::Family the shelf shows
	std::string shelf_letter; // and its letter tab: "#", "A".."Z"
	std::string last_rom; // the shelf puts the selection on this game

	void Load();
	void Save() const;
};

Settings& Config();
} // namespace fe
