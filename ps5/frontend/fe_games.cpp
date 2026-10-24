// FBNeo PS5 frontend: the game library (fe_games.h).
// SPDX-License-Identifier: MIT

#include "fe_games.h"

#include "fe_burn.h"

#include "OrbisPaths.h"
#include "ProsperoThread.h"
#include "netfs.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fe
{
namespace
{
const SystemInfo kSystems[size_t(System::Count)] = {
	{"arcade", "Arcade", "FBNeo_-_Arcade_Games", Family::All, "FBNeo"},
};

const char* const kFamilyNames[size_t(Family::Count)] = {
	"All games", "Capcom", "Neo Geo", "Sega", "Konami", "Taito", "Toaplan, Cave, Psikyo", "Data East", "Irem",
	"Midway", "IGS", "Classics", "Other",
};

std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

// FBNeo's hardware codes (burn.h: HARDWARE_PREFIX_*) -> the shelf's tabs
Family FamilyOf(uint32_t hw)
{
	switch ((hw >> 24) & 0x7f)
	{
		case 0x01: case 0x07: case 0x09: case 0x14: return Family::Capcom; // CPS-1, CPS-2, CPS-3, misc
		case 0x05: return Family::NeoGeo;
		case 0x02: return Family::Sega;
		case 0x03: return Family::Konami;
		case 0x0b: return Family::Taito;
		case 0x04: case 0x06: case 0x0d: return Family::Toaplan; // Toaplan, Cave, Psikyo
		case 0x13: return Family::DataEast;
		case 0x11: return Family::Irem;
		case 0x1b: return Family::Midway;
		case 0x08: case 0x23: return Family::Igs; // PGM, PGM2
		case 0x00: case 0x0f: case 0x10: return Family::Classics; // pre-90s, Pac-Man, Galaxian
		default: return Family::Other;
	}
}

// The library's archives: set name (lower case) -> path; the internal folder first, then the USB drives, then the shares.
std::mutex s_lock;
std::unordered_map<std::string, std::string> s_zips;
std::vector<BiosStatus> s_bios; // the last scan's (BiosReport)

struct Zip
{
	std::string path;
	bool usb = false, net = false;
	long long size = 0, mtime = 0; // from the folder walk's stat: the ROM check cache's signatures use them
};
// set (lower case) -> "size:mtime" of its archive, from the last walk (Signature reads it instead of a stat per set and
// per driver: thousands of them on a USB drive with a big library); and the sets whose archive is on a share
std::unordered_map<std::string, std::string> s_zip_stamp;
std::unordered_map<std::string, bool> s_zip_net;

bool HasExt(const std::string& name, const char* ext)
{
	const size_t n = strlen(ext);
	return name.size() > n && strcasecmp(name.c_str() + name.size() - n, ext) == 0;
}

std::string DirOf(const std::string& path)
{
	return path.substr(0, path.find_last_of('/'));
}

void Walk(const std::string& dir, int depth, bool usb, bool net, std::map<std::string, Zip>& out, const ScanProgress& progress)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	std::vector<std::string> names, subdirs;
	while (dirent* e = readdir(d))
		if (e->d_name[0] != '.')
			names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end()); // the same order on every drive and share
	for (const std::string& name : names)
	{
		const std::string path = dir + "/" + name;
		struct stat st = {};
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if (depth < 4)
				subdirs.push_back(path);
			continue;
		}
		if (!S_ISREG(st.st_mode) || name.size() <= 3 || name.size() > 200)
			continue; // a FIFO or a device would block the zip reader forever
		const bool zip = HasExt(name, ".zip"), sevenz = HasExt(name, ".7z");
		if (!zip && !sevenz)
			continue;
		const std::string set = Lower(name.substr(0, name.size() - (zip ? 4 : 3)));
		auto it = out.find(set);
		// the first one found wins; in one folder a set's .zip over its .7z
		if (it != out.end() && !(zip && HasExt(it->second.path, ".7z") && DirOf(it->second.path) == dir))
			continue;
		out[set] = Zip{path, usb, net, (long long)st.st_size, (long long)st.st_mtime};
		if (progress)
			progress(0, out.size(), 0);
	}
	for (const std::string& s : subdirs)
		Walk(s, depth + 1, usb, net, out, progress);
}

// "Street Fighter II' - Champion Edition (World 920513)" -> the title and the part in brackets
void SplitName(const std::string& full, std::string* title, std::string* region)
{
	const size_t a = full.find(" (");
	*title = full.substr(0, a);
	region->clear();
	if (a != std::string::npos)
	{
		const size_t b = full.rfind(')');
		*region = full.substr(a + 2, b != std::string::npos && b > a + 2 ? b - a - 2 : std::string::npos);
	}
}

// ---- the ROM check cache: set -> the zips it was checked with (size and time of each) and what was missing ------
std::string CachePath()
{
	return OrbisDir("config") + "/romcheck.txt";
}

std::string Header()
{
	return "# FBNeo " + burn::CoreVersion() + ", " + std::to_string(burn::DriverCount()) + " drivers\n";
}

// One entry: the signature of the archives it was made with, the essential ROMs missing, and for a BIOS set (the BIOS
// sets screen) its optional ROMs and the names of the missing ones. A line: set \t sig \t missing [\t optional \t
// found \t name,name...] (older lines have the first three fields only).
struct CheckEntry
{
	std::string sig;
	int missing = 0;
	bool detail = false;
	int optional = 0, optional_found = 0;
	std::vector<std::string> names;
};

// One per process: the scan (the shelf's thread) and NetCheck's pass (its own) share it, each under the lock.
struct CheckCache
{
	std::mutex lock;
	std::unordered_map<std::string, CheckEntry> entries;
	bool dirty = false;
	bool loaded = false;
	std::string header; // the FBNeo the file was loaded for

	// Reads the file the first time (or again when FBNeo changed: another version's drivers may list other ROMs).
	void Begin()
	{
		std::lock_guard<std::mutex> g(lock);
		const std::string want = Header();
		if (loaded && header == want)
			return;
		loaded = true;
		header = want;
		entries.clear();
		FILE* f = fopen(CachePath().c_str(), "r");
		if (!f)
			return;
		char line[4096];
		if (!fgets(line, sizeof(line), f) || want != line)
		{
			fclose(f);
			OrbisLog("[games] romcheck.txt is from another FBNeo: every set is checked again");
			dirty = true;
			return;
		}
		while (fgets(line, sizeof(line), f))
		{
			std::string s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			std::vector<std::string> fields;
			for (size_t at = 0;;)
			{
				const size_t t = s.find('\t', at);
				fields.push_back(s.substr(at, t == std::string::npos ? std::string::npos : t - at));
				if (t == std::string::npos)
					break;
				at = t + 1;
			}
			if (fields.size() < 3)
				continue;
			CheckEntry e;
			e.sig = fields[1];
			e.missing = atoi(fields[2].c_str());
			if (fields.size() >= 6)
			{
				e.detail = true;
				e.optional = atoi(fields[3].c_str());
				e.optional_found = atoi(fields[4].c_str());
				for (size_t at = 0; at < fields[5].size();)
				{
					const size_t c = fields[5].find(',', at);
					e.names.push_back(fields[5].substr(at, c == std::string::npos ? std::string::npos : c - at));
					at = c == std::string::npos ? fields[5].size() : c + 1;
				}
			}
			entries[fields[0]] = std::move(e);
		}
		fclose(f);
	}

	bool Find(const std::string& set, const std::string& sig, CheckEntry* out)
	{
		std::lock_guard<std::mutex> g(lock);
		auto it = entries.find(set);
		if (it == entries.end() || it->second.sig != sig)
			return false;
		*out = it->second;
		return true;
	}

	void Put(const std::string& set, CheckEntry e)
	{
		std::lock_guard<std::mutex> g(lock);
		entries[set] = std::move(e);
		dirty = true;
	}

	void Save()
	{
		std::lock_guard<std::mutex> g(lock);
		if (!dirty)
			return;
		const std::string path = CachePath(), tmp = path + ".part";
		FILE* f = fopen(tmp.c_str(), "w");
		if (!f)
			return;
		bool ok = fputs(header.c_str(), f) >= 0;
		for (const auto& e : entries)
		{
			ok = fprintf(f, "%s\t%s\t%d", e.first.c_str(), e.second.sig.c_str(), e.second.missing) > 0 && ok;
			if (e.second.detail)
			{
				std::string names;
				for (const std::string& n : e.second.names)
					names += (names.empty() ? "" : ",") + n;
				ok = fprintf(f, "\t%d\t%d\t%s", e.second.optional, e.second.optional_found, names.c_str()) >= 0 && ok;
			}
			ok = fputc('\n', f) != EOF && ok;
		}
		ok = fflush(f) == 0 && ok;
		ok = fsync(fileno(f)) == 0 && ok;
		ok = fclose(f) == 0 && ok;
		if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
			unlink(tmp.c_str());
		else
			dirty = false;
	}
};

CheckCache& SharedCheckCache()
{
	static CheckCache* c = new CheckCache; // never freed: the check pass may still run at exit
	return *c;
}

// What the scan left for NetCheck: sets whose check needs an archive on a share.
struct PendingCheck
{
	std::string set; // the cache's key: the driver's name
	int driver = -1;
	std::string sig; // the archives as the scan saw them
	bool merged = false; // a clone looked for in its parent's archive (a ROM found only by name counts as missing)
	bool bios = false;   // a BIOS set: its details are kept for the BIOS sets screen
};
std::vector<PendingCheck> s_pending; // under s_lock

CheckEntry EntryFrom(const burn::RomCheck& rc, const std::string& sig, bool merged, bool bios)
{
	CheckEntry e;
	e.sig = sig;
	e.missing = int(rc.missing.size() + (merged ? rc.bad_crc.size() : 0));
	if (bios)
	{
		e.detail = true;
		e.optional = rc.optional;
		e.optional_found = rc.optional_found;
		e.names = rc.missing;
	}
	return e;
}

// The sets a driver loads from (itself, its BIOS, its parents) and the state of their archives: "name:size:mtime;..."
// (a .7z: "name.7z:size:mtime"). *net: one of them is on a share.
std::string Signature(const burn::Driver& d, const std::vector<burn::Driver>& drivers,
	const std::unordered_map<std::string, int>& by_name, bool* net)
{
	std::vector<std::string> sets = {d.name};
	if (!d.board.empty())
		sets.push_back(d.board);
	std::string p = d.parent;
	for (int depth = 0; depth < 4 && !p.empty(); depth++)
	{
		sets.push_back(p);
		auto it = by_name.find(p);
		p = it == by_name.end() ? "" : drivers[size_t(it->second)].parent;
	}
	*net = false;
	std::string sig;
	for (const std::string& s : sets)
	{
		// the same "size:mtime" the stat gave, from the walk that has just stat'ed every archive
		const std::string key = Lower(s);
		auto z = s_zip_stamp.find(key);
		if (z != s_zip_stamp.end())
		{
			auto n = s_zip_net.find(key);
			*net = *net || (n != s_zip_net.end() && n->second);
			sig += s + z->second + ";";
		}
		else
			sig += s + ":-;";
	}
	return sig;
}
} // namespace

const SystemInfo& Info(System s)
{
	return kSystems[size_t(s) < size_t(System::Count) ? size_t(s) : 0];
}

const char* FamilyName(Family f)
{
	return kFamilyNames[size_t(f) < size_t(Family::Count) ? size_t(f) : 0];
}

void PrepareFolders()
{
	OrbisMkdirs(OrbisDir("roms"));
	OrbisMkdirs(OrbisDir("covers") + "/" + kSystems[0].folder);
	OrbisMkdirs(OrbisDir("config"));
	OrbisMkdirs(OrbisDir("samples"));
	OrbisMkdirs(OrbisDir("hiscore"));
}

std::vector<BiosStatus> BiosReport()
{
	std::lock_guard<std::mutex> lk(s_lock);
	return s_bios;
}

std::string FindSetZip(const std::string& set)
{
	std::lock_guard<std::mutex> lk(s_lock);
	auto it = s_zips.find(Lower(set));
	return it == s_zips.end() ? std::string() : it->second;
}

std::vector<std::string> RomRoots()
{
	std::vector<std::string> roots = OrbisRomRoots();
	for (const std::string& r : netfs_mount_points())
		roots.push_back(r);
	return roots;
}

std::vector<GameInfo> ScanGames(const ScanProgress& progress)
{
	PrepareFolders();
	std::map<std::string, Zip> found;
	// the console's folders first (internal, USB drives), then the shares: a set on both is the console's
	for (const std::string& root : RomRoots())
		Walk(root, 0, root.rfind("/mnt/", 0) == 0, netfs_is_network_path(root.c_str()), found, progress);
	{
		std::unordered_map<std::string, std::string> zips;
		for (const auto& z : found)
			zips[z.first] = z.second.path;
		std::lock_guard<std::mutex> lk(s_lock);
		s_zips.swap(zips); // whole: NetCheck's thread may be looking sets up
	}
	s_zip_stamp.clear();
	s_zip_net.clear();
	int on_net = 0;
	for (const auto& z : found)
	{
		s_zip_stamp[z.first] = (HasExt(z.second.path, ".7z") ? ".7z:" : ":") + std::to_string(z.second.size) + ":" +
			std::to_string(z.second.mtime);
		s_zip_net[z.first] = z.second.net;
		on_net += z.second.net ? 1 : 0;
	}

	// the drivers, by name
	std::vector<burn::Driver> drivers(size_t(std::max(0, burn::DriverCount())));
	// (a name can be two drivers: "neogeo" is the BIOS set and a driver that boots the BIOS alone; the first wins,
	// and a set other drivers name as their BIOS is never a game)
	std::unordered_map<std::string, int> by_name;
	std::unordered_map<std::string, bool> boards;
	std::map<std::string, int> board_drivers; // a BIOS set -> how many arcade drivers need it
	for (int i = 0; i < burn::DriverCount(); i++)
		if (burn::GetDriver(i, &drivers[size_t(i)]))
		{
			by_name.emplace(drivers[size_t(i)].name, i);
			if (!drivers[size_t(i)].board.empty())
			{
				boards[drivers[size_t(i)].board] = true;
				if (drivers[size_t(i)].arcade)
					board_drivers[drivers[size_t(i)].board]++;
			}
		}

	CheckCache& cache = SharedCheckCache();
	cache.Begin();
	std::vector<PendingCheck> pending;
	std::vector<GameInfo> games;
	int unknown = 0, bios = 0, checked = 0, merged = 0, merged_checked = 0;
	size_t left = 0; // sets left to NetCheck
	// Missing ROMs of a driver: from the cache while its archives are unchanged, else counted now; -1 when an archive
	// is on a share (NetCheck counts them). A clone looked for in its parent's archive (counter == &merged_checked) also
	// counts the ROMs found only by name: a parent's ROM of the same name isn't the clone's
	auto missing_of = [&](const burn::Driver& d, int* counter) {
		bool net = false;
		const std::string sig = Signature(d, drivers, by_name, &net);
		CheckEntry e;
		if (cache.Find(d.name, sig, &e))
			return e.missing;
		const bool is_merged = counter == &merged_checked;
		if (net)
		{
			pending.push_back(PendingCheck{d.name, d.index, sig, is_merged, false});
			return -1;
		}
		const burn::RomCheck rc = burn::CheckRoms(d.index, [](const std::string& set) { return FindSetZip(set); });
		e = EntryFrom(rc, sig, is_merged, false);
		cache.Put(d.name, e);
		(*counter)++;
		if (e.missing && counter == &checked && !rc.missing.empty())
			OrbisLog("[games] %s: %d ROM(s) missing, first %s", d.name.c_str(), e.missing, rc.missing[0].c_str());
		return e.missing;
	};
	auto add = [&](const burn::Driver& d, const std::string& path, const Zip& z, int missing) {
		GameInfo g;
		g.path = path;
		g.file_base = d.name;
		g.ext = HasExt(z.path, ".7z") ? ".7z" : ".zip";
		g.nointro = d.title;
		SplitName(d.title, &g.title, &g.region);
		g.parent = d.parent;
		g.bios = d.board;
		if (!d.parent.empty())
		{
			auto pt = by_name.find(d.parent);
			if (pt != by_name.end())
				g.parent_title = drivers[size_t(pt->second)].title;
		}
		g.year = d.year;
		g.maker = d.maker;
		g.board = d.system;
		g.family = FamilyOf(d.hardware);
		g.driver = d.index;
		g.on_usb = z.usb;
		g.on_network = z.net;
		g.clone = d.clone;
		g.vertical = d.vertical;
		g.working = d.working;
		g.check_pending = missing < 0; // listed until NetCheck says otherwise
		g.missing = std::max(0, missing);
		g.complete = missing <= 0;
		games.push_back(std::move(g));
	};
	auto is_game = [&](const burn::Driver& d) { return d.arcade && !d.bios && !boards.count(d.name); };
	size_t looked = 0;
	for (const auto& z : found)
	{
		if (progress)
			progress(1, looked++, found.size());
		auto it = by_name.find(z.first);
		if (it == by_name.end())
		{
			unknown++;
			continue;
		}
		const burn::Driver& d = drivers[size_t(it->second)];
		if (!d.arcade)
			continue;
		if (!is_game(d))
		{
			bios++;
			continue;
		}
		add(d, z.second.path, z.second, missing_of(d, &checked));
	}
	// Clones kept inside their parent's archive (merged sets, as FBNeo's own frontends read them): a clone with no
	// archive of its own whose parent's archive is here is listed as "<parent archive>#<clone>" when all its ROMs are found
	for (const burn::Driver& d : drivers)
	{
		if (!d.clone || d.parent.empty() || !is_game(d) || found.count(Lower(d.name)))
			continue;
		auto pz = found.find(Lower(d.parent));
		if (pz == found.end())
			continue;
		if (missing_of(d, &merged_checked) == 0)
		{
			add(d, pz->second.path + "#" + d.name, pz->second, 0);
			merged++;
		}
	}

	// the BIOS sets: found or not, complete or not, and how many of your games need each
	{
		std::vector<BiosStatus> report;
		for (const auto& b : board_drivers)
		{
			auto it = by_name.find(b.first);
			if (it == by_name.end())
				continue;
			const burn::Driver& bd = drivers[size_t(it->second)];
			if (!bd.bios)
				continue; // a game a few clones also load from (sfa2ur1...), not a BIOS
			BiosStatus st;
			st.set = b.first;
			st.drivers = b.second;
			st.title = bd.title;
			for (const GameInfo& g : games)
				st.games += g.bios == b.first ? 1 : 0;
			auto z = found.find(Lower(b.first));
			if (z != found.end())
			{
				st.path = z->second.path;
				st.on_network = z->second.net;
				bool net = false;
				const std::string sig = Signature(bd, drivers, by_name, &net);
				CheckEntry e;
				if (cache.Find(bd.name, sig, &e) && e.detail)
				{
					st.missing = e.names;
					st.optional = e.optional;
					st.optional_found = e.optional_found;
				}
				else if (net)
				{
					st.pending = true;
					pending.push_back(PendingCheck{bd.name, bd.index, sig, false, true});
				}
				else
				{
					const burn::RomCheck rc = burn::CheckRoms(bd.index, [](const std::string& set) { return FindSetZip(set); });
					cache.Put(bd.name, EntryFrom(rc, sig, false, true));
					st.missing = rc.missing;
					st.optional = rc.optional;
					st.optional_found = rc.optional_found;
				}
			}
			const std::string what = st.path.empty() ? "not found"
				: st.pending ? "on a share, checked in the background"
				: st.missing.empty() ? "OK"
				: "incomplete, " + std::to_string(st.missing.size()) + " ROM(s) missing, first " + st.missing[0];
			OrbisLog("[bios] %s (%s): %s; needed by %d of your games (%d in FBNeo)",
				st.path.empty() ? (st.set + ".zip").c_str() : st.path.substr(st.path.find_last_of('/') + 1).c_str(),
				st.title.c_str(), what.c_str(), st.games, st.drivers);
			report.push_back(std::move(st));
		}
		// the ones your games need first (missing before OK), then the rest by name
		std::stable_sort(report.begin(), report.end(), [](const BiosStatus& a, const BiosStatus& b) {
			if ((a.games > 0) != (b.games > 0))
				return a.games > 0;
			if (a.games > 0 && a.ok() != b.ok())
				return !a.ok();
			return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
		});
		std::lock_guard<std::mutex> lk(s_lock);
		s_bios = std::move(report);
		// BIOS sets first: the BIOS sets screen, and a game's check needs its BIOS's archive read anyway
		std::stable_sort(pending.begin(), pending.end(), [](const PendingCheck& a, const PendingCheck& b) { return a.bios && !b.bios; });
		left = pending.size();
		s_pending = std::move(pending);
	}
	cache.Save();

	std::sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const int c = strcasecmp(a.title.c_str(), b.title.c_str());
		if (c != 0)
			return c < 0;
		if (a.clone != b.clone)
			return !a.clone; // the parent first
		return strcasecmp(a.region.c_str(), b.region.c_str()) < 0;
	});
	int complete = 0;
	for (const GameInfo& g : games)
		complete += g.complete && !g.check_pending ? 1 : 0;
	int sevenz = 0;
	for (const auto& z : found)
		sevenz += HasExt(z.second.path, ".7z") ? 1 : 0;
	OrbisLog("[games] %zu archive(s): %zu arcade set(s) (%d complete, %d checked now), %d BIOS set(s), %d not FBNeo's; %d "
		"clone(s) in a parent's archive (%d checked now); %d .7z, %d on network shares (%d set(s) left to the background "
		"check)", found.size(), games.size(), complete, checked, bios, unknown, merged, merged_checked, sevenz, on_net,
		int(left));
	constexpr size_t kLogged = 200;
	for (size_t i = 0; i < games.size() && i < kLogged; i++)
	{
		const GameInfo& g = games[i];
		OrbisLog("[games]   %s -> \"%s\" [%s]%s", g.path.c_str(), g.nointro.c_str(), g.board.c_str(),
			g.check_pending ? " (not checked yet)" : g.complete ? "" : " (incomplete)");
	}
	if (games.size() > kLogged)
		OrbisLog("[games]   ... and %zu more", games.size() - kLogged);
	return games;
}

// ---- the check pass for sets on shares ----

// Whether a check saw the archives the scan's signature names (the same size and date, the same extension; a set that
// had none, none). A share that dropped or reconnected between the scan and the check leaves find() with no archive or
// another one: that result is not the set's.
bool StampsMatch(const std::string& sig, const std::vector<std::pair<std::string, std::string>>& stamps)
{
	for (const auto& st : stamps)
	{
		const std::string name = Lower(st.first);
		for (size_t at = 0; at < sig.size();)
		{
			size_t end = sig.find(';', at);
			if (end == std::string::npos)
				end = sig.size();
			const std::string piece = sig.substr(at, end - at);
			at = end + 1;
			const size_t cut = piece.find_first_of(".:");
			if (cut == std::string::npos || Lower(piece.substr(0, cut)) != name)
				continue;
			if (piece.substr(cut) != st.second)
				return false;
		}
	}
	return true;
}

// A path on a share that is not connected now (its archive could not be read for that, not for a fault of its own).
bool OnShareNotReady(const std::string& path)
{
	if (path.empty() || !netfs_is_network_path(path.c_str()))
		return false;
	for (const std::string& m : netfs_mount_points())
		if (path.size() > m.size() && path.compare(0, m.size(), m) == 0 && path[m.size()] == '/')
			return false;
	return true;
}

struct NetCheck::Impl
{
	std::mutex lock;
	std::deque<PendingCheck> todo;
	// every set queued, with the signature it was queued with: one is checked once per visit of the shelf (a read error
	// is tried again at the next visit), unless its share was not connected or its archives changed meanwhile: then it
	// is let go, and the next scan that sees it queues it again
	std::set<std::string> tried;
	std::atomic<bool> stop{false};
	std::atomic<int> resolved{0};
	std::atomic<bool> busy{false};
	ps5::BigThread thread;

	void Run()
	{
		CheckCache& cache = SharedCheckCache();
		int done = 0, since_save = 0, read_errors = 0, later = 0;
		for (;;)
		{
			PendingCheck p;
			{
				std::lock_guard<std::mutex> g(lock);
				if (stop.load() || todo.empty() || netfs_realtime())
				{
					busy = false; // under the lock: an Add() from now on starts a new pass
					break;
				}
				p = todo.front();
				todo.pop_front();
			}
			const burn::RomCheck rc =
				burn::CheckRoms(p.driver, [](const std::string& set) { return FindSetZip(set); }, &stop);
			if (stop.load())
			{
				// cut short (a game, Settings, Square): nothing kept, and it is the first one when the pass goes on
				std::lock_guard<std::mutex> g(lock);
				todo.push_front(p);
				break;
			}
			// the archives the scan saw, or another state of the shares (one dropped or reconnecting since the scan)
			bool share_down = !StampsMatch(p.sig, rc.stamps);
			for (const auto& st : rc.stamps)
				share_down = share_down || OnShareNotReady(FindSetZip(st.first));
			if (share_down)
			{
				// not the set's result: nothing kept, and the next scan that sees its archives queues it again
				later++;
				{
					std::lock_guard<std::mutex> g(lock);
					tried.erase(p.set + "\t" + p.sig);
				}
				OrbisLog("[games] %s: its share is not connected, or its archives changed since the scan: checked when "
					"the shelf sees them again", p.set.c_str());
				continue;
			}
			if (rc.read_error)
			{
				// not the archive's fault (a link that dropped, the watchdog): nothing kept, the next visit of the shelf
				// checks it again
				read_errors++;
				OrbisLog("[games] %s: an archive could not be read (%s): checked again next time", p.set.c_str(),
					rc.unreadable.empty() ? "?" : rc.unreadable[0].c_str());
				continue;
			}
			const CheckEntry e = EntryFrom(rc, p.sig, p.merged, p.bios);
			cache.Put(p.set, e);
			if (e.missing && !p.merged && !rc.missing.empty())
				OrbisLog("[games] %s: %d ROM(s) missing, first %s", p.set.c_str(), e.missing, rc.missing[0].c_str());
			done++;
			resolved++;
			if (++since_save >= 20)
			{
				cache.Save();
				since_save = 0;
			}
		}
		cache.Save();
		std::string extra;
		if (read_errors)
			extra += ", " + std::to_string(read_errors) + " not readable";
		if (later)
			extra += ", " + std::to_string(later) + " left for when their share is back";
		OrbisLog("[games] network check pass: %d set(s) checked%s%s", done, extra.c_str(), stop.load() ? " (stopped)" : "");
	}
};

NetCheck::NetCheck() : m_i(new Impl) {}

NetCheck::~NetCheck()
{
	Stop();
}

void NetCheck::Add()
{
	std::vector<PendingCheck> list;
	{
		std::lock_guard<std::mutex> lk(s_lock);
		list = s_pending;
	}
	size_t added = 0;
	bool start = false;
	{
		std::lock_guard<std::mutex> g(m_i->lock);
		for (const PendingCheck& p : list)
			if (m_i->tried.insert(p.set + "\t" + p.sig).second)
			{
				m_i->todo.push_back(p);
				added++;
			}
		// nothing new may still leave work (what a Stop() cut short); a running pass takes the new ones too
		if (!m_i->todo.empty() && !m_i->busy.load())
			start = m_i->busy = true;
	}
	if (added)
		OrbisLog("[games] checking %zu set(s) on network shares in the background", added);
	if (!start)
		return;
	m_i->thread.join(); // a pass that has just ended
	m_i->stop = false;
	m_i->thread = ps5::BigThread([this] { m_i->Run(); }, 256 * 1024);
}

void NetCheck::Stop()
{
	m_i->stop = true;
	m_i->thread.join();
	m_i->busy = false;
}

int NetCheck::Resolved() const
{
	return m_i->resolved.load();
}

bool NetCheck::Busy() const
{
	return m_i->busy.load();
}
} // namespace fe
