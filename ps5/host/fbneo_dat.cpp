// FBNeo PS5: the ROM DAT of the drivers this port compiles, written by FBNeo's own DAT writer (src/burner/dat.cpp,
// write_datfile with DAT_ARCADE_ONLY, as FBNeo's SDL frontend writes its "FBNeo_-_Arcade.dat"), with a header that
// names FBNeo PS5, its version and the FBNeo commit its core is built from. `fbneo_headless dat` (tools/make_dat.sh)
// calls it; the app does not have it.
//
// SPDX-License-Identifier: MIT

#include "../../src/burner/burner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// What misc.cpp (GameDecoration, the "Bootleg, Hack, ..." comment of each set) takes from FBNeo's video interface;
// nothing here draws.
UINT32(__cdecl* VidHighCol)(INT32 r, INT32 g, INT32 b, INT32 i) = NULL;
INT32 nVidFullscreen = 0;
INT32 bVidUseHardwareGamma = 0;
INT32 VidRecalcPal()
{
	return 0;
}
TCHAR szAppBurnVer[16] = "";

namespace
{
size_t Count(const std::string& s, const char* what)
{
	size_t n = 0;
	for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
		n++;
	return n;
}
} // namespace

// <path>: the DAT; <commit>, <date>: the FBNeo commit the core's sources are (git) and its date (YYYY-MM-DD);
// <leave_out>: the sets of the drivers built in that the app does not list (fe_burn.cpp's arcade check: FBNeo's arcade
// DAT has the Neo Geo CDZ's BIOS set, neocdz, and this port has no Neo Geo CD)
int FbneoPs5WriteDat(const char* path, const char* commit, const char* date, const std::vector<std::string>& leave_out)
{
	snprintf(szAppBurnVer, sizeof(szAppBurnVer), "%s", FBNEO_CORE_VERSION); // src/burn/version.h, as the Makefile reads it
	char* buf = NULL;
	size_t len = 0;
	FILE* mem = open_memstream(&buf, &len);
	if (!mem)
		return 1;
	const INT32 rc = write_datfile(DAT_ARCADE_ONLY, mem);
	fclose(mem);
	std::string dat(buf, len);
	free(buf);
	if (rc != 0)
		return 1;

	std::string left;
	for (const std::string& name : leave_out)
	{
		for (const char* open : {"\t<game name=\"", "\t<game isbios=\"yes\" name=\""})
		{
			const size_t g0 = dat.find(open + name + "\"");
			if (g0 == std::string::npos)
				continue;
			const char* close = "\t</game>\n";
			const size_t g1 = dat.find(close, g0);
			if (g1 == std::string::npos)
				return 1;
			dat.erase(g0, g1 + strlen(close) - g0);
			left += (left.empty() ? "" : ", ") + name;
		}
	}

	// FBNeo's header names FinalBurn Neo; this one names FBNeo PS5 and the exact core
	const size_t h0 = dat.find("\t<header>\n");
	const char* end_tag = "\t</header>\n";
	const size_t h1 = dat.find(end_tag);
	if (h0 == std::string::npos || h1 == std::string::npos || h1 < h0)
		return 1;
	const size_t sets = Count(dat, "\t<game ");
	const size_t bios = Count(dat, "\t<game isbios=\"yes\"");
	const std::string ver = FBNEO_PS5_VERSION, core = szAppBurnVer, full = commit, shortc = full.substr(0, 7);
	const std::string repo = std::string("https://github.com/") + FBNEO_UPDATE_REPO;
	std::string header;
	header += "\t<header>\n";
	header += "\t\t<name>FBNeo PS5 - Arcade Games</name>\n";
	header += "\t\t<description>FBNeo PS5 " + ver + " Arcade Games (FinalBurn Neo " + core + ", commit " + shortc + " of " +
		date + ")</description>\n";
	header += "\t\t<category>Standard DatFile</category>\n";
	header += "\t\t<version>" + ver + "</version>\n";
	header += std::string("\t\t<date>") + date + "</date>\n";
	header += "\t\t<author>FBNeo PS5 (MisterTemaki), with FinalBurn Neo's DAT writer</author>\n";
	header += "\t\t<homepage>" + repo + "</homepage>\n";
	header += "\t\t<url>" + repo + "</url>\n";
	header += "\t\t<comment>The ROM sets FBNeo PS5 " + ver + " plays: the " + std::to_string(sets) +
		" sets of the arcade drivers it compiles (" + std::to_string(bios) +
		" of them BIOS and board sets), written by FinalBurn Neo's src/burner/dat.cpp from FinalBurn Neo " + core +
		", finalburnneo/FBNeo commit " + full + " (" + date +
		"). FBNeo PS5 builds this core itself: not RetroArch, not the libretro core." +
		(left.empty() ? std::string() : " Left out, as the app does not list them: " + left + ".") + "</comment>\n";
	header += "\t\t<clrmamepro forcenodump=\"ignore\"/>\n";
	header += end_tag;
	dat.replace(h0, h1 + strlen(end_tag) - h0, header);
	if (dat.empty() || dat.back() != '\n')
		dat += '\n';

	FILE* f = fopen(path, "wb");
	if (!f)
		return 1;
	const bool ok = fwrite(dat.data(), 1, dat.size(), f) == dat.size();
	if (fclose(f) != 0 || !ok)
		return 1;
	fprintf(stderr, "%s: %zu sets (%zu BIOS and board sets)\n", path, sets, bios);
	return 0;
}
