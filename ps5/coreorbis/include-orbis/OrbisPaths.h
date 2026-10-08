// FBNeo PS5: the /data/fbneo folder layout and the boot log (orbis-shims/orbis_paths.cpp).
//
//   /data/fbneo/roms         arcade ROM sets (.zip, named as FBNeo names them) and the BIOS sets they need
//                             (neogeo.zip, pgm.zip...); sub-folders are fine
//   /data/fbneo/saves        NVRAM (<set>.nvram), EEPROMs (<set>.nv)
//   /data/fbneo/states       save states, <set>.state1 .. <set>.state10
//   /data/fbneo/config       the games' DIP switches (<set>.dip), romcheck.txt (the ROM check of each set)
//   /data/fbneo/samples      sample sets (<name>.zip) for the games that play recorded sounds
//   /data/fbneo/hiscore      hiscore.dat (FBNeo's, to save high scores) and the scores (<set>.hi)
//   /data/fbneo/blend        FBNeo's blend tables (optional)
//   /data/fbneo/covers       box art (downloaded into covers/FBNeo, or your own <set>.png), wanted.txt
//   /data/fbneo/logs         boot.log (this run) and boot.prev.log (the run before); installer.log, helper.log
//   /data/fbneo/fbneo-ps5.ini  the frontend's settings
//
// USB drives are searched too: /mnt/usbN/fbneo/roms (N = 0..7) and /mnt/extN/fbneo/roms.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#ifndef ORBIS_ROOT_DEFAULT
#define ORBIS_ROOT_DEFAULT "/data/fbneo"
#endif

// Root folder (ORBIS_ROOT_DEFAULT; the host tests point it elsewhere with FBNEO_PS5_ROOT).
const std::string& OrbisRoot();
// <root>/<sub>, created at boot by OrbisPathsInit.
std::string OrbisDir(const char* sub);
// Creates the folder tree. Returns false when the root itself can't be created (no /data access).
bool OrbisPathsInit();
// Folders that hold ROMs and exist right now (internal first, then USB drives).
std::vector<std::string> OrbisRomRoots();

bool OrbisIsDir(const std::string& path);
bool OrbisIsFile(const std::string& path);
bool OrbisMkdirs(const std::string& path);

// <root>/logs/<name>.log (the run before kept as <name>.prev.log): boot.log for the emulator, installer.log
// for the installer/helper payload. Every line also goes to stdout (once the log is open). Lines logged before the file is open
// (before the jailbreak shows /data to the app) are kept and written first.
void OrbisLogOpen(const char* name = "boot");
void OrbisLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void OrbisLogClose();
// Debug logs on or off: the "Debug logs" setting (debug_logs= in fbneo-ps5.ini, on unless it says 0).
// OrbisLogOpen reads it, so the app, the installer and the helper all follow it; while off, OrbisLog writes
// nothing (no file, no stdout) and the log files of earlier runs are left as they are.
void OrbisLogSetEnabled(bool on);
// Reads the setting again and applies it (the helper, which keeps running, calls this for each request).
void OrbisLogRefresh();
bool OrbisLogEnabled();
// The open log's file descriptor, for the crash handler's signal-safe write(); -1 before it is open.
int OrbisLogFd();
