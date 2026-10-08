// FBNeo PS5 frontend: the game library (fe_games.h).
// SPDX-License-Identifier: MIT

#include "fe_games.h"

#include "fe_burn.h"

#include "OrbisPaths.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
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

// The library's zips: set name (lower case) -> path; the internal folder first, then the USB drives.
std::mutex s_lock;
std::unordered_map<std::string, std::string> s_zips;

struct Zip
{
	std::string path;
	bool usb;
};

void Walk(const std::string& dir, int depth, bool usb, std::map<std::string, Zip>& out)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	std::vector<std::string> subdirs;
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		const std::string name = e->d_name;
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
		if (!S_ISREG(st.st_mode) || name.size() <= 4 || name.size() > 200)
			continue; // a FIFO or a device would block the zip reader forever
		if (strcasecmp(name.c_str() + name.size() - 4, ".zip") != 0)
			continue;
		const std::string set = Lower(name.substr(0, name.size() - 4));
		if (!out.count(set))
			out[set] = Zip{path, usb};
	}
	closedir(d);
	std::sort(subdirs.begin(), subdirs.end());
	for (const std::string& s : subdirs)
		Walk(s, depth + 1, usb, out);
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

struct CheckCache
{
	std::unordered_map<std::string, std::pair<std::string, int>> entries; // set -> (signature, missing)
	bool dirty = false;

	void Load()
	{
		FILE* f = fopen(CachePath().c_str(), "r");
		if (!f)
			return;
		char line[2048];
		while (fgets(line, sizeof(line), f))
		{
			std::string s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			const size_t t1 = s.find('\t');
			const size_t t2 = t1 == std::string::npos ? t1 : s.find('\t', t1 + 1);
			if (t2 == std::string::npos)
				continue;
			entries[s.substr(0, t1)] = {s.substr(t1 + 1, t2 - t1 - 1), atoi(s.c_str() + t2 + 1)};
		}
		fclose(f);
	}

	void Save()
	{
		if (!dirty)
			return;
		const std::string path = CachePath(), tmp = path + ".part";
		FILE* f = fopen(tmp.c_str(), "w");
		if (!f)
			return;
		bool ok = true;
		for (const auto& e : entries)
			ok = fprintf(f, "%s\t%s\t%d\n", e.first.c_str(), e.second.first.c_str(), e.second.second) > 0 && ok;
		ok = fflush(f) == 0 && ok;
		ok = fsync(fileno(f)) == 0 && ok;
		ok = fclose(f) == 0 && ok;
		if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
			unlink(tmp.c_str());
	}
};

// The sets a driver loads from (itself, its BIOS, its parents) and the state of their zips: "name:size:mtime;..."
std::string Signature(const burn::Driver& d, const std::vector<burn::Driver>& drivers,
	const std::unordered_map<std::string, int>& by_name)
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
	std::string sig;
	for (const std::string& s : sets)
	{
		auto z = s_zips.find(Lower(s));
		struct stat st = {};
		if (z != s_zips.end() && stat(z->second.c_str(), &st) == 0)
			sig += s + ":" + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtime) + ";";
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

std::string FindSetZip(const std::string& set)
{
	std::lock_guard<std::mutex> lk(s_lock);
	auto it = s_zips.find(Lower(set));
	return it == s_zips.end() ? std::string() : it->second;
}

std::vector<GameInfo> ScanGames()
{
	PrepareFolders();
	std::map<std::string, Zip> found;
	for (const std::string& root : OrbisRomRoots())
		Walk(root, 0, root.rfind("/mnt/", 0) == 0, found);
	{
		std::lock_guard<std::mutex> lk(s_lock);
		s_zips.clear();
		for (const auto& z : found)
			s_zips[z.first] = z.second.path;
	}

	// the drivers, by name
	std::vector<burn::Driver> drivers(size_t(std::max(0, burn::DriverCount())));
	// (a name can be two drivers: "neogeo" is the BIOS set and a driver that boots the BIOS alone; the first wins,
	// and a set other drivers name as their BIOS is never a game)
	std::unordered_map<std::string, int> by_name;
	std::unordered_map<std::string, bool> boards;
	for (int i = 0; i < burn::DriverCount(); i++)
		if (burn::GetDriver(i, &drivers[size_t(i)]))
		{
			by_name.emplace(drivers[size_t(i)].name, i);
			if (!drivers[size_t(i)].board.empty())
				boards[drivers[size_t(i)].board] = true;
		}

	CheckCache cache;
	cache.Load();
	std::vector<GameInfo> games;
	int unknown = 0, bios = 0, checked = 0;
	for (const auto& z : found)
	{
		auto it = by_name.find(z.first);
		if (it == by_name.end())
		{
			unknown++;
			continue;
		}
		const burn::Driver& d = drivers[size_t(it->second)];
		if (!d.arcade)
			continue;
		if (d.bios || boards.count(d.name))
		{
			bios++;
			continue;
		}
		GameInfo g;
		g.path = z.second.path;
		g.file_base = d.name;
		g.ext = ".zip";
		g.nointro = d.title;
		SplitName(d.title, &g.title, &g.region);
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
		g.on_usb = z.second.usb;
		g.clone = d.clone;
		g.vertical = d.vertical;
		g.working = d.working;
		const std::string sig = Signature(d, drivers, by_name);
		auto c = cache.entries.find(d.name);
		if (c != cache.entries.end() && c->second.first == sig)
			g.missing = c->second.second;
		else
		{
			const burn::RomCheck rc = burn::CheckRoms(d.index, [](const std::string& set) { return FindSetZip(set); });
			g.missing = int(rc.missing.size());
			cache.entries[d.name] = {sig, g.missing};
			cache.dirty = true;
			checked++;
			if (g.missing)
				OrbisLog("[games] %s: %d ROM(s) missing, first %s", d.name.c_str(), g.missing, rc.missing[0].c_str());
		}
		g.complete = g.missing == 0;
		games.push_back(std::move(g));
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
		complete += g.complete ? 1 : 0;
	OrbisLog("[games] %zu zip(s): %zu arcade set(s) (%d complete, %d checked now), %d BIOS set(s), %d not FBNeo's",
		found.size(), games.size(), complete, checked, bios, unknown);
	constexpr size_t kLogged = 200;
	for (size_t i = 0; i < games.size() && i < kLogged; i++)
	{
		const GameInfo& g = games[i];
		OrbisLog("[games]   %s -> \"%s\" [%s]%s", g.path.c_str(), g.nointro.c_str(), g.board.c_str(),
			g.complete ? "" : " (incomplete)");
	}
	if (games.size() > kLogged)
		OrbisLog("[games]   ... and %zu more", games.size() - kLogged);
	return games;
}
} // namespace fe
