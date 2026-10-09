// FBNeo PS5 frontend: settings file.
// SPDX-License-Identifier: MIT

#include "fe_settings.h"

#include "OrbisPaths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace fe
{
namespace
{
std::string IniPath()
{
	return OrbisRoot() + "/fbneo-ps5.ini";
}

int Clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

const char* const kButtonKeys[Settings::kButtons] = {"btn_1", "btn_2", "btn_3", "btn_4", "btn_5", "btn_6", "btn_coin",
	"btn_start"};
constexpr int kPs5ButtonChoices = 11; // emu::kPs5ButtonCount
constexpr int kShaderCount = 13; // ps5crt::Shader::Count
constexpr int kLayoutCount = 5; // emu::kLayoutCount
} // namespace

constexpr int Settings::kDefaultButtons[Settings::kButtons];

void Settings::DefaultButtons()
{
	for (int i = 0; i < kButtons; i++)
		buttons[i] = kDefaultButtons[i];
}

Settings& Config()
{
	static Settings s;
	return s;
}

void Settings::Load()
{
	FILE* f = fopen(IniPath().c_str(), "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';')
			continue;
		*eq = 0;
		const std::string key = line;
		const char* val = eq + 1;
		const int n = atoi(val);
		if (key == "scale")
			scale = Clamp(n, 0, 2);
		else if (key == "shader")
			shader = Clamp(n, 0, kShaderCount - 1);
		else if (key == "aspect")
			aspect = Clamp(n, 0, 2);
		else if (key == "smooth")
			smooth = n != 0;
		else if (key == "scanlines")
			scanlines = n != 0;
		else if (key == "show_fps")
			show_fps = n != 0;
		else if (key == "audio")
			audio = n != 0;
		else if (key == "volume")
			volume = Clamp(n, 0, 100);
		else if (key == "hiscores")
			hiscores = n != 0;
		else if (key == "layout")
			layout = Clamp(n, 0, kLayoutCount - 1);
		else if (key.compare(0, 4, "btn_") == 0)
		{
			for (int i = 0; i < kButtons; i++)
				if (key == kButtonKeys[i])
					buttons[i] = Clamp(n, 0, kPs5ButtonChoices - 1);
		}
		else if (key == "state_slot")
			state_slot = Clamp(n, 1, 10);
		else if (key == "covers_download")
			covers_download = n != 0;
		else if (key == "show_clones")
			show_clones = n != 0;
		else if (key == "show_incomplete")
			show_incomplete = n != 0;
		else if (key == "debug_logs")
			debug_logs = n != 0;
		else if (key == "shelf_family")
			shelf_family = Clamp(n, 0, 64);
		else if (key == "shelf_letter")
			shelf_letter = std::string(val).substr(0, 1);
		else if (key == "last_rom")
			last_rom = val;
	}
	fclose(f);
	OrbisLog("[settings] loaded %s", IniPath().c_str());
}

void Settings::Save() const
{
	const std::string tmp = IniPath() + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		OrbisLog("[settings] can't write %s", tmp.c_str());
		return;
	}
	fprintf(f, "# FBNeo PS5\n");
	fprintf(f, "scale=%d\n", scale);
	fprintf(f, "shader=%d\n", shader);
	fprintf(f, "aspect=%d\n", aspect);
	fprintf(f, "smooth=%d\n", smooth ? 1 : 0);
	fprintf(f, "scanlines=%d\n", scanlines ? 1 : 0);
	fprintf(f, "show_fps=%d\n", show_fps ? 1 : 0);
	fprintf(f, "audio=%d\n", audio ? 1 : 0);
	fprintf(f, "volume=%d\n", volume);
	fprintf(f, "hiscores=%d\n", hiscores ? 1 : 0);
	fprintf(f, "layout=%d\n", layout);
	for (int i = 0; i < kButtons; i++)
		fprintf(f, "%s=%d\n", kButtonKeys[i], buttons[i]);
	fprintf(f, "state_slot=%d\n", state_slot);
	fprintf(f, "covers_download=%d\n", covers_download ? 1 : 0);
	fprintf(f, "show_clones=%d\n", show_clones ? 1 : 0);
	fprintf(f, "show_incomplete=%d\n", show_incomplete ? 1 : 0);
	fprintf(f, "debug_logs=%d\n", debug_logs ? 1 : 0);
	fprintf(f, "shelf_family=%d\n", shelf_family);
	fprintf(f, "shelf_letter=%s\n", shelf_letter.c_str());
	fprintf(f, "last_rom=%s\n", last_rom.c_str());
	bool ok = ferror(f) == 0;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), IniPath().c_str()) != 0)
	{
		remove(tmp.c_str());
		OrbisLog("[settings] can't save %s (disk full?)", IniPath().c_str());
	}
}
} // namespace fe
