// FBNeo PS5 frontend (2.2): where a game's hard disk image is (Killer Instinct 1 and 2), MAME's layout:
//
//   <rom folder>/<set>/<disk>.chd        kinst/kinst.chd next to kinst.zip
//
// A clone looks in its own folder, then its parent's (kinst14/kinst.chd, then kinst/kinst.chd). The ROM folders, in
// order: the one the set's archive is in, the one its parent's archive is in, then every ROM folder (the internal
// drive, USB drives, extended storage, network shares). The file: <disk>.chd first, then an old <disk>.img (a disk the
// driver names "<x>.img" is looked for as <x>.chd too). Folder and file names are matched without regard to case by
// listing the folder (an exact match first). A path over kMaxPath characters is refused ("too long", as DiskImage's).
// Only lists folders and stats files: no image is opened here.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace fe
{
namespace hdd
{
constexpr size_t kMaxPath = 259;

enum class Find
{
	Found,
	Missing,
	TooLong, // found only where the path is too long, or every folder to look in is
};

struct Lookup
{
	Find result = Find::Missing;
	std::string path;      // Found: the image
	std::string folder;    // Found: the folder it is in; Missing / TooLong: where it is expected (the set's own, beside its archive)
	std::string need;      // "kinst/kinst.chd": what to put next to the ROM zip (the parent's folder for a clone)
	bool on_network = false; // the image is on a network share
};

// set "kinst14", parent "kinst" ("" for a parent set), disk "kinst" (the driver's name for it); folders: the ROM
// folders in the order above (duplicates are skipped).
Lookup FindImage(const std::string& set, const std::string& parent, const std::string& disk,
	const std::vector<std::string>& folders);

// "<dir>" of "<dir>/<file>" ("" for "")
std::string DirOf(const std::string& path);

// The folders to look in: the set's archive's, its parent's archive's (either "" when there is none), then `roots`.
std::vector<std::string> Folders(const std::string& set_archive, const std::string& parent_archive,
	const std::vector<std::string>& roots);
} // namespace hdd
} // namespace fe
