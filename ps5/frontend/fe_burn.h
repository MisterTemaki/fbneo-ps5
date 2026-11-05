// FBNeo PS5: FBNeo's core ("burn") behind a small API, the way FBNeo's own frontends and its libretro port host it.
//
// The core runs one driver at a time (nBurnDrvActive). This layer answers what the core asks of its frontend: the
// ROMs (BurnExtLoadRom, from the game's ROM-set .zip or .7z and its parent's and BIOS's), the colour format (BurnHighCol),
// its log (bprintf), the folders (hiscores, samples, NVRAM), and the few functions FBNeo's frontends provide. It
// gives each frame as 32-bit pixels turned the right way up (vertical games are rotated), and the frame's sound as
// 48 kHz stereo. It does not touch the PS5: the host tests run it as it is.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_hdd.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
	std::string disk;    // 2.2: the hard disk image it needs ("kinst": Killer Instinct), "" for none
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
// Finds the archive of a ROM set by its name ("sf2", "neogeo"): the full path of its .zip or .7z, or "" when there is
// none. Called on the thread that asks (CheckRoms may run on a background thread).
using ZipFinder = std::function<std::string(const std::string& set)>;

struct RomCheck
{
	bool ok = false;
	std::vector<std::string> missing; // the essential ROMs not found: "s92e_23b.8f"
	std::vector<std::string> bad_crc; // found by name with another CRC (used anyway, as FBNeo does): "sf2ce.zip: ..."
	int optional = 0, optional_found = 0; // the ROMs the driver can do without (a BIOS set's other versions)
	std::vector<std::string> sets;    // the sets it looked for: "sf2ce", "sf2"
	std::vector<std::string> files;   // the archives it read: "sf2ce.zip", "sf2.7z"
	std::vector<std::string> unreadable; // archives found that could not be opened: "sf2.7z"
	bool read_error = false; // one of them could not be read (a share that dropped, a cancel...): the result may change
	std::string why;          // a .7z's problem, in English, for the player ("" for none)
	// CheckRoms: each set looked for and the archive it found, as "<set>" + ":<size>:<mtime>" (".7z:<size>:<mtime>" for
	// a .7z), ":-" when find() had none, "?" when it could not be looked at
	std::vector<std::pair<std::string, std::string>> stamps;
};
// Checks that every ROM the driver needs is in its archives (by CRC, then by name). Reads only the archives'
// directories. Safe on another thread while no game runs (the driver tables are read under a lock). cancel (when
// given): once it turns true the reads stop, and the result is a read error.
RomCheck CheckRoms(int driver, const ZipFinder& find, const std::atomic<bool>* cancel = nullptr);

// 2.2: the ROM folders a hard disk image is also looked for in, after the set's and its parent's archive folders (the
// app's: fe::RomRoots; none when not set).
void SetRomFolders(std::function<std::vector<std::string>()> folders);
// Where a driver's hard disk image is (fe_hdd.h), the archives found by `find`. Missing with an empty `need` for a driver
// without a disk. Any thread while no game runs (the driver tables are read under the lock).
fe::hdd::Lookup FindDisk(int driver, const ZipFinder& find);

// Starts the driver. On failure *error says why (missing ROMs listed; a missing hard disk image and where to put it;
// "Not enough memory to start the game." when an allocation failed while the driver started).
// `dips`: the game's saved DIP switches (LoadDips' file), set with the defaults before the driver starts, as
// FBNeo's frontends do (many drivers read them as they start: the Neo Geo its BIOS, others their region).
bool Load(int driver, const ZipFinder& find, std::string* error, const std::string& dips = "");
void Unload(); // writes the NVRAM (and the hard disk's changes)
bool Loaded();
const Driver& Current();

// ---- the hard disk's changes (2.2: Killer Instinct writes its audits and settings to its disk) ----------------------
// What happened to the saved disk changes when the game last started (that start only: cleared at each Load).
struct DiskReport
{
	std::string disk, image, diff; // "kinst", the image, its .diff
	std::string set_aside_as;     // set aside: the .diff's new name
	std::string put_back_from;    // put back: the set-aside's name
	std::string why;              // saving off: why (English, for the log)
	bool put_back = false, set_aside = false, set_aside_failed = false, put_back_failed = false, saving_off = false;
	bool sha_mismatch = false;
	int set_asides = 0;           // set-asides of this image found (more than one: the newest went back)
};
std::vector<DiskReport> LastStartDisks();
// the image's lookup at the last Load (a start that failed for want of it: Missing or TooLong)
fe::hdd::Lookup LastDiskLookup();
bool HasDisk();          // the running game has a hard disk open
bool HddDirty();         // changes not written yet (of a disk whose saving is on)
bool HddSavingOff();     // a disk whose changes are not saved this session
uint32_t HddReadErrors(); // sectors of the image that could not be read (a share that dropped)
bool HddOnNetwork();     // the image is on a network share
// Writes every disk's changes now (the game's thread). True when all were written or there was nothing to write.
bool HddFlush();
// The periodic save: the game's thread takes the changes (Take: false when there is nothing to write), any thread
// writes them from the job's own bytes (Write), then the game's thread says how it went (Done, before the next Take or
// any Flush / Unload).
class HddJob
{
public:
	HddJob();
	~HddJob();
	bool Take();
	bool Write(); // true when every disk's file was written
	void Done(bool ok);
	bool Empty() const;
	size_t Bytes() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};
#ifndef __PROSPERO__
// The host tests: writes `count` sectors from lba, filled with `fill`, as the game's IDE controller would; reads them.
void HddTestWrite(uint32_t lba, uint32_t count, uint8_t fill);
void HddTestRead(uint32_t lba, uint32_t count);
#endif

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
	// How far the pad's trigger (L2/R2) that gives button N is pressed, 0..255; 0 when that button is not on a trigger.
	// Sega Model 2's analog button inputs (Daytona USA's accelerator and brake) take it; every other input, and every
	// other game's, takes button[] as in 2.2.
	uint8_t pressure[kButtons] = {};
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
// 2.3: player 1's fire button i (0..kButtons-1) is an analog input (a pedal: Daytona USA's accelerator and brake)
bool AnalogButton(int i);
// What each player's digital inputs got since the game started, for the log: "player 1 nothing, player 2 up fire1 coin".
std::string InputReport();

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
// The same in memory.
bool StateToMemory(std::vector<uint8_t>* out);
bool StateFromMemory(const std::vector<uint8_t>& in);
// NVRAM / memory card areas (ACB_NVRAM): read at load, written by SaveNvram (and Unload) when they changed.
bool SaveNvram(bool force);

// FBNeo's high score saving (hiscore.dat in the hiscore folder), from the next Load on.
void SetHiscores(bool on);

// The core's log lines go here (OrbisLog on the PS5).
void SetLog(void (*log)(const char* line));
// 2.3: worker threads for a driver's work that splits into parts (Sega Model 2's 3D, by bands of lines; the core's
// burn_parallel.h). run(job, ctx, parts) calls job(ctx, i) for each i < parts, on the caller and the workers, and
// returns when all are done; threads counts the caller too. Without it (or threads 1) the parts run in turn.
typedef void (*ParallelJob)(void* ctx, int part);
void SetParallel(void (*run)(ParallelJob job, void* ctx, int parts), int threads);
// The texts Load's *error shows on the screen go through this (fe::T in the app: the interface's language).
void SetTranslate(const char* (*tr)(const char* english));
} // namespace burn
