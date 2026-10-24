// FBNeo PS5 frontend: the game library -- every arcade ROM set (.zip or .7z) under the ROM folders that FBNeo knows:
// the console's (/data/fbneo/roms, fbneo/roms on USB drives) and the network shares that are connected.
//
// A set is known by its file name, as in every FBNeo frontend: sf2ce.zip (or sf2ce.7z) is "Street Fighter II' -
// Champion Edition (World 920513)". A set that needs others (its parent, sf2.zip; its BIOS, neogeo.zip) finds them
// anywhere in the ROM folders, the console's first (the internal drive, then USB drives), then the shares. Each set is
// checked once (by CRC, then by name, as FBNeo loads it: only the archives' directories are read) and the result kept
// in /data/fbneo/config/romcheck.txt until one of its archives changes: the shelf lists the complete sets (the
// incomplete ones too with the "Show incomplete sets" setting). BIOS sets (neogeo.zip, pgm.zip...) are not games and
// are not listed. A set whose check needs an archive on a share is not checked by the scan (a big library would
// crawl): it is listed as it is, and NetCheck checks it in the background.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fe
{
// One "system" for the covers and the cache folders: FBNeo's arcade games.
enum class System : uint8_t
{
	Arcade,
	Count
};

// The shelf's tabs: the makers and boards players look for.
enum class Family : uint8_t
{
	All,
	Capcom,   // CPS-1, CPS-2, CPS-3, Capcom's other boards
	NeoGeo,   // MVS / AES
	Sega,
	Konami,
	Taito,
	Toaplan,  // and Cave, Psikyo: the shoot 'em up makers
	DataEast,
	Irem,
	Midway,
	Igs,      // PGM, PGM2
	Classics, // the late 70s and 80s: Pac-Man, Galaxian and the rest
	Other,
	Count
};

struct SystemInfo
{
	const char* id;     // "arcade"
	const char* name;   // "Arcade"
	const char* thumbs; // the libretro-thumbnails repository: "FBNeo_-_Arcade_Games"
	Family family;
	const char* folder; // "FBNeo": its folder in covers/
};
const SystemInfo& Info(System s);
const char* FamilyName(Family f); // "All games", "Capcom", "Neo Geo"...

// Creates the folders the library uses (roms, covers/FBNeo, config, samples, hiscore).
void PrepareFolders();

struct GameInfo
{
	std::string path;      // the set's archive ("<parent's archive>#<set>" for a clone inside its parent's)
	std::string file_base; // the set's name: "sf2ce"
	std::string ext;       // ".zip" or ".7z"
	System system = System::Arcade;
	std::string nointro;   // FBNeo's full name: "Street Fighter II' - Champion Edition (World 920513)" (the cover's)
	std::string title;     // what the shelf shows: "Street Fighter II' - Champion Edition"
	std::string region;    // the rest of the name: "World 920513"
	std::string parent_title; // the parent set's full name (a clone's cover falls back to it), or ""
	std::string parent;       // the parent set's name: "sf2" (a clone), or ""
	std::string bios;         // the BIOS set it also needs: "neogeo", "pgm"... or ""
	std::string year, maker, board; // "1992", "Capcom", "CPS1"
	Family family = Family::Other;
	int driver = -1;       // the core's driver index
	bool on_usb = false;
	bool on_network = false;  // on a share (/smb/..., /nfs/...)
	bool check_pending = false; // its ROM check needs a share's archives and is not done yet (NetCheck): listed as complete
	bool name_by_crc = false; // (not used: sets are named by their file name)
	bool clone = false, vertical = false, working = true;
	bool complete = true;  // every needed ROM found
	int missing = 0;       // how many are not
};

// Every arcade set under the ROM folders (sub-folders included, 4 levels), sorted by title. The driver list must
// be up (burn::Init) and no game running (the check uses the core's tables).
// progress (when given) is told how far the scan is: stage 0 while the folders are listed (done = zips found so far,
// total = 0), stage 1 while each set is checked (done of total). It is called often: it decides itself how often
// to draw.
using ScanProgress = std::function<void(int stage, size_t done, size_t total)>;
std::vector<GameInfo> ScanGames(const ScanProgress& progress = nullptr);
// The BIOS sets (the sets FBNeo's drivers name as their board: neogeo, pgm, decocass...), as the last scan found
// them: Settings, BIOS sets, shows this.
struct BiosStatus
{
	std::string set;   // "neogeo"
	std::string title; // "Neo Geo"
	std::string path;  // its archive, or "" when there is none
	std::vector<std::string> missing; // the ROMs it must have that it hasn't (its archive found)
	int optional = 0, optional_found = 0; // other versions it can have (the Neo Geo's BIOSes)
	int games = 0;   // games of your library that need it
	int drivers = 0; // FBNeo's games that need it
	bool on_network = false; // its archive is on a share
	bool pending = false;    // on a share and not checked yet (NetCheck does it)
	bool ok() const { return !path.empty() && missing.empty() && !pending; }
};
std::vector<BiosStatus> BiosReport();

// The archive of a ROM set by its name, from the last scan ("" when there is none): what burn::Load looks for. Any
// thread.
std::string FindSetZip(const std::string& set);

// The folders to look for games in: OrbisRomRoots() plus the network shares that are connected (in memory, no I/O).
std::vector<std::string> RomRoots();

// Checks the sets on shares in the background (Snes9x PS5's design for naming network games, by thatboialex, as NES
// FCEUmm PS5's NetNaming does): the scan doesn't open the archives on a share; this does, one set at a time (their
// directories only: burn::CheckRoms), for the sets the last scan left unchecked (games, clones in a parent's archive,
// BIOS sets), and keeps each result in romcheck.txt, so the next ScanGames lists them as checked. A result that a read
// error made (a share that dropped) is not kept. Add() starts or extends the pass with the last scan's list; it stops
// by itself when a game starts (netfs_realtime) and when Stop() or the destructor is called (leaving the shelf: the
// pass reads the core's driver tables, which a running game owns).
class NetCheck
{
public:
	NetCheck();
	~NetCheck();
	void Add();
	void Stop();
	int Resolved() const; // sets checked so far
	bool Busy() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_i;
};
} // namespace fe
