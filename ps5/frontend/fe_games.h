// FBNeo PS5 frontend: the game library -- every arcade ROM set (.zip) under the ROM folders that FBNeo knows.
//
// A set is known by its file name, as in every FBNeo frontend: sf2ce.zip is "Street Fighter II' - Champion
// Edition (World 920513)". A set that needs others (its parent, sf2.zip; its BIOS, neogeo.zip) finds them
// anywhere in the ROM folders. Each set is checked once (by CRC, then by name, as FBNeo loads it) and the result
// kept in /data/fbneo/config/romcheck.txt until one of its zips changes: the shelf lists the complete sets (the
// incomplete ones too with the "Show incomplete sets" setting). BIOS sets (neogeo.zip, pgm.zip...) are not
// games and are not listed.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
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
	std::string path;      // the set's zip
	std::string file_base; // the set's name: "sf2ce"
	std::string ext;       // ".zip"
	System system = System::Arcade;
	std::string nointro;   // FBNeo's full name: "Street Fighter II' - Champion Edition (World 920513)" (the cover's)
	std::string title;     // what the shelf shows: "Street Fighter II' - Champion Edition"
	std::string region;    // the rest of the name: "World 920513"
	std::string parent_title; // the parent set's full name (a clone's cover falls back to it), or ""
	std::string year, maker, board; // "1992", "Capcom", "CPS1"
	Family family = Family::Other;
	int driver = -1;       // the core's driver index
	bool on_usb = false;
	bool name_by_crc = false; // (not used: sets are named by their file name)
	bool clone = false, vertical = false, working = true;
	bool complete = true;  // every needed ROM found
	int missing = 0;       // how many are not
};

// Every arcade set under the ROM folders (sub-folders included, 4 levels), sorted by title. The driver list must
// be up (burn::Init) and no game running (the check uses the core's tables).
std::vector<GameInfo> ScanGames();
// The zip of a ROM set by its name, from the last scan ("" when there is none): what burn::Load looks for.
std::string FindSetZip(const std::string& set);
} // namespace fe
