// FBNeo PS5 frontend: the favourite games (fe_favs.h).
// SPDX-License-Identifier: MIT

#include "fe_favs.h"

#include "OrbisPaths.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace fe
{
namespace favs
{
namespace
{
bool g_loaded = false;
std::vector<std::string> g_sets; // in the file's order, new ones at the end

std::string Clean(const char* s)
{
	std::string out(s);
	while (!out.empty() && isspace(static_cast<unsigned char>(out.back())))
		out.pop_back();
	size_t i = 0;
	while (i < out.size() && isspace(static_cast<unsigned char>(out[i])))
		i++;
	out.erase(0, i);
	for (char& c : out)
		c = char(tolower(static_cast<unsigned char>(c))); // FBNeo's set names are lower case
	return out;
}

bool Save()
{
	const std::string path = Path(), tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		OrbisLog("[favs] can't write %s: %s", tmp.c_str(), strerror(errno));
		return false;
	}
	for (const std::string& s : g_sets)
		fprintf(f, "%s\n", s.c_str());
	bool ok = ferror(f) == 0;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		remove(tmp.c_str());
		OrbisLog("[favs] can't save %s (disk full?)", path.c_str());
		return false;
	}
	OrbisLog("[favs] saved %s: %d favourite(s)", path.c_str(), int(g_sets.size()));
	return true;
}
} // namespace

std::string Path()
{
	return OrbisRoot() + "/favourites.txt";
}

void Load()
{
	if (g_loaded)
		return;
	g_loaded = true;
	g_sets.clear();
	FILE* f = fopen(Path().c_str(), "r");
	if (!f)
	{
		if (errno == ENOENT)
			OrbisLog("[favs] no %s yet: no favourites", Path().c_str());
		else
			OrbisLog("[favs] can't read %s (%s): no favourites", Path().c_str(), strerror(errno));
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		const std::string s = Clean(line);
		if (s.empty() || s[0] == '#' || s[0] == ';')
			continue;
		if (std::find(g_sets.begin(), g_sets.end(), s) == g_sets.end())
			g_sets.push_back(s);
	}
	const bool err = ferror(f) != 0;
	fclose(f);
	if (err)
		OrbisLog("[favs] read error in %s: %d favourite(s) read", Path().c_str(), int(g_sets.size()));
	else
		OrbisLog("[favs] %s: %d favourite(s)", Path().c_str(), int(g_sets.size()));
}

bool Has(const std::string& set)
{
	return std::find(g_sets.begin(), g_sets.end(), set) != g_sets.end();
}

bool Toggle(const std::string& set, bool* saved)
{
	const auto it = std::find(g_sets.begin(), g_sets.end(), set);
	const bool now = it == g_sets.end();
	if (now)
		g_sets.push_back(set);
	else
		g_sets.erase(it);
	OrbisLog("[favs] %s %s", set.c_str(), now ? "added" : "removed");
	const bool ok = Save();
	if (saved)
		*saved = ok;
	return now;
}

int Count()
{
	return int(g_sets.size());
}
} // namespace favs
} // namespace fe
