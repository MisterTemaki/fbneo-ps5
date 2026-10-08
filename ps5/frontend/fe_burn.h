// FBNeo PS5: FBNeo's core ("burn") behind a small API, the way FBNeo's own frontends and its libretro port host it.
//
// The core runs one driver at a time (nBurnDrvActive). This layer answers what the core asks of its frontend: the
// ROMs (BurnExtLoadRom, from the game's ROM-set .zip and its parent's and BIOS's), the colour format (BurnHighCol),
// its log (bprintf), the folders (hiscores, samples, NVRAM), and the few functions FBNeo's frontends provide. It
// gives each frame as 32-bit pixels turned the right way up (vertical games are rotated), and the frame's sound as
// 48 kHz stereo. It does not touch the PS5: the host tests run it as it is.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace burn
{
// ---- the drivers ------------------------------------------------------------------------------------------------
struct Driver
{
	int index = -1;
	std::string name;   // the ROM set: "sf2ce" (sf2ce.zip)
	std::string parent; // "sf2" for a clone, "" otherwise
	std::string board;  // the BIOS set it also needs: "neogeo", "pgm"... or ""
	std::string title;  // "Street Fighter II' - Champion Edition (World 920513)"
	std::string year, maker, system; // "1992", "Capcom", "CPS1"
	uint32_t hardware = 0; // HARDWARE_* (BurnDrvGetHardwareCode)
	uint32_t flags = 0;    // BDF_*
	uint32_t genre = 0;    // GBF_*
	int players = 0;
	bool working = false, clone = false, vertical = false, bios = false;
	bool arcade = false; // an arcade machine (the home consoles' drivers are not listed by this port)
};

// BurnLibInit and the callbacks. Paths end without '/': saves (NVRAM, EEPROM, hiscores), samples, blend tables.
struct Paths
{
	std::string saves, samples, hiscore, blend, config;
};
bool Init(const Paths& paths);
void Exit();
int DriverCount();
// The driver's details (the core's tables, read through nBurnDrvActive; never while a game runs).
bool GetDriver(int index, Driver* out);
int FindDriver(const std::string& name); // -1 when unknown
std::string CoreVersion(); // "1.0.0.3"

// ---- a game ------------------------------------------------------------------------------------------------------
// Finds the .zip of a ROM set by its name ("sf2", "neogeo"): its full path, or "" when there is none.
using ZipFinder = std::function<std::string(const std::string& set)>;

struct RomCheck
{
	bool ok = false;
	std::vector<std::string> missing; // "sf2ce.zip: s92e_23b.8f" (essential ROMs only)
	std::vector<std::string> bad_crc; // found by name with another CRC (used anyway, as FBNeo does)
	std::vector<std::string> sets;    // the zips it looked in: "sf2ce", "sf2"
};
// Checks that every ROM the driver needs is in its zips (by CRC, then by name).
RomCheck CheckRoms(int driver, const ZipFinder& find);

// Starts the driver. On failure *error says why (missing ROMs listed).
// `dips`: the game's saved DIP switches (LoadDips' file), set with the defaults before the driver starts, as
// FBNeo's frontends do (many drivers read them as they start: the Neo Geo its BIOS, others their region).
bool Load(int driver, const ZipFinder& find, std::string* error, const std::string& dips = "");
void Unload(); // writes the NVRAM
bool Loaded();
const Driver& Current();

// The game's frame rate (nBurnFPS / 100) and the picture's shape on its monitor (4:3, or 3:4 for vertical games).
double Fps();
double Aspect();

// ---- input --------------------------------------------------------------------------------------------------------
constexpr int kMaxPlayers = 4;
constexpr int kButtons = 8; // "fire 1" .. "fire 8"
struct PlayerInput
{
	bool up = false, down = false, left = false, right = false;
	bool button[kButtons] = {};
	bool coin = false, start = false;
	int8_t stick_x = 0, stick_y = 0; // the left stick, for the games with analog controls (-128..127)
	int8_t stick2_x = 0, stick2_y = 0; // the right stick (dial / second axis pairs)
	uint8_t trigger_l = 0, trigger_r = 0; // pedals
};
struct Input
{
	PlayerInput player[kMaxPlayers];
	bool service = false, test = false, reset = false, tilt = false;
};
// How many fire buttons player 1 has ("p1 fire N") and their names in the driver ("Weak Punch").
int ButtonCount();
std::string ButtonName(int i);
bool HasAnalog();

// ---- DIP switches -------------------------------------------------------------------------------------------------
struct DipGroup
{
	std::string name;                 // "Difficulty"
	std::vector<std::string> options; // "Easy", "Normal"...
	int current = 0, def = 0;
};
std::vector<DipGroup> Dips();
void SetDip(int group, int option);
void ResetDips(); // the driver's defaults
// The DIPs that differ from the defaults, as "<group>=<option>" lines (per game in config/<set>.dip).
bool SaveDips(const std::string& path);
bool LoadDips(const std::string& path);

// ---- running ------------------------------------------------------------------------------------------------------
// One frame. draw = false skips the picture (fast forward). Sound: 48 kHz stereo, *frames of it.
void RunFrame(const Input& in, bool draw);
// The last picture, 0xAARRGGBB, turned the right way up.
const uint32_t* Picture(int* w, int* h);
const int16_t* Sound(int* frames);
void Reset(); // the machine's reset (the driver's own)

// ---- saves -------------------------------------------------------------------------------------------------------
// States: the driver's whole state (BurnAreaScan), gzip-compressed, with the set's name and the layout checked on load.
bool SaveState(const std::string& path);
bool LoadState(const std::string& path, std::string* error);
// The same in memory (rewind).
bool StateToMemory(std::vector<uint8_t>* out);
bool StateFromMemory(const std::vector<uint8_t>& in);
// NVRAM / memory card areas (ACB_NVRAM): read at load, written by SaveNvram (and Unload) when they changed.
bool SaveNvram(bool force);

// FBNeo's high score saving (hiscore.dat in the hiscore folder), from the next Load on.
void SetHiscores(bool on);

// The core's log lines go here (OrbisLog on the PS5).
void SetLog(void (*log)(const char* line));
} // namespace burn
