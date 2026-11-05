// FBNeo PS5 frontend: the player's favourite games (2.3, fbneo-ps5#5), kept in /data/fbneo/favourites.txt.
//
// A favourite is a ROM set's name ("sf2ce"; a clone is its own entry), one per line, so the same set is a favourite on
// every storage tab (on USB and on the internal drive alike). Names of sets that aren't there today are kept: a game
// whose archive is missing stays a favourite. The file is read once, when the shelf first opens (a missing or unreadable
// file: no favourites, logged), and written whole on every change (.tmp, fsync, rename). The shelf's thread only.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace fe
{
namespace favs
{
// Reads the file the first time it is called; later calls do nothing.
void Load();
bool Has(const std::string& set);
// Adds the set, or removes it when it is a favourite already, and writes the file. Returns true when the set is now a
// favourite; *saved says whether the file was written (the change is kept in memory for this run either way).
bool Toggle(const std::string& set, bool* saved);
int Count();
std::string Path();
} // namespace favs
} // namespace fe
