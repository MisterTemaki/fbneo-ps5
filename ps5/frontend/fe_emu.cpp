// FBNeo PS5 frontend: the running game (fe_emu.h).
//
// Frame pacing, as in Snes9x PS5 and Genesis Plus GX PS5:
//   - games at 58.5..61.5 Hz (most arcade boards: CPS-1/2 59.6, Neo Geo 59.2, 60...): Present waits for the flip,
//     so the console's 60 Hz vsync paces the emulation; the game then runs that much faster than its board (at
//     most 2.5%), and its sound is resampled from 48 kHz x 60 / fps so that it keeps up, the rate steered by up to
//     0.5% to keep about 60 ms queued.
//   - the others (50 Hz, 53 Hz, 54.7 Hz, 57.5 Hz boards...): the audio clock paces (wait while more than 60 ms
//     is queued); the flips don't wait.
//   - In any case, if more than 120 ms is queued (vsync not blocking for some reason), wait on audio.
//   - Fast forward (hold R2): several frames per flip, no sound.
//
// SPDX-License-Identifier: MIT

#include "fe_emu.h"
#include "fe_pool.h"

#include "ProsperoNotify.h"
#include "fe_games.h"
#include "fe_i18n.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoCrt.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoThread.h"
#include "ProsperoVideo.h"

#include "netfs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <new>
#include <vector>

static_assert(emu::kPads == fe::Settings::kPads && emu::kPads == ps5input::kMaxPads, "one setting per controller");

namespace emu
{
const Choice kAspects[] = {
	{"The game's monitor", ""},
	{"Square pixels", ""},
	{"16:9 (stretched)", ""},
};
const int kAspectCount = int(sizeof(kAspects) / sizeof(kAspects[0]));

const Choice kLayouts[] = {
	{"Auto", ""},
	{"Classic", ""},
	{"Fighting (6 buttons)", ""},
	{"Custom", ""},
	{"Fighting (R1 R2)", ""},
	{"Driving", ""},
};
const int kLayoutCount = int(sizeof(kLayouts) / sizeof(kLayouts[0]));

const PadButton kPs5Buttons[] = {
	{"Cross", SCE_PAD_BUTTON_CROSS},
	{"Circle", SCE_PAD_BUTTON_CIRCLE},
	{"Square", SCE_PAD_BUTTON_SQUARE},
	{"Triangle", SCE_PAD_BUTTON_TRIANGLE},
	{"L1", SCE_PAD_BUTTON_L1},
	{"R1", SCE_PAD_BUTTON_R1},
	{"OPTIONS", SCE_PAD_BUTTON_OPTIONS},
	{"Touchpad", SCE_PAD_BUTTON_TOUCH_PAD},
	{"None", 0},
	{"L2", SCE_PAD_BUTTON_L2},
	{"R2", SCE_PAD_BUTTON_R2},
};
const int kPs5ButtonCount = int(sizeof(kPs5Buttons) / sizeof(kPs5Buttons[0]));

const ArcadeButton kArcadeButtons[kArcadeButtonCount] = {
	{"Button 1", "btn_1"}, {"Button 2", "btn_2"}, {"Button 3", "btn_3"}, {"Button 4", "btn_4"},
	{"Button 5", "btn_5"}, {"Button 6", "btn_6"}, {"Coin", "btn_coin"}, {"Start", "btn_start"},
	{"Button 7", "btn_7"}, {"Button 8", "btn_8"},
};
const int kArcadeButtonRows[kArcadeButtonCount] = {0, 1, 2, 3, 4, 5, 8, 9, 6, 7};

int GameButtonOf(int i)
{
	static const int kGame[kArcadeButtonCount] = {0, 1, 2, 3, 4, 5, -1, -1, 6, 7};
	return i >= 0 && i < kArcadeButtonCount ? kGame[i] : -1;
}

int LayoutButton(int layout, int i, int game_buttons, const int* custom, bool pedals)
{
	// indexes in kPs5Buttons: 0 Cross, 1 Circle, 2 Square, 3 Triangle, 4 L1, 5 R1, 6 OPTIONS, 7 touchpad, 8 none, 9 L2,
	// 10 R2; the arrays: Button 1..6, Coin, Start, Button 7, Button 8
	// classic (FBNeo's libretro pad): buttons 1-4 = Cross, Circle, Square, Triangle (Neo Geo A B C D), 5-6 = L1 R1
	static const int kClassic[kArcadeButtonCount] = {0, 1, 2, 3, 4, 5, 7, 6, kNoButton, kNoButton};
	// fighting (Capcom's 6 buttons, punches on top): Square Triangle R1 = 1 2 3, Cross Circle L1 = 4 5 6
	static const int kFighting[kArcadeButtonCount] = {2, 3, 5, 0, 1, 4, 7, 6, kNoButton, kNoButton};
	// fighting with the triggers (Capcom's own PS layout): Square Triangle R1 = 1 2 3, Cross Circle R2 = 4 5 6
	static const int kFightingR2[kArcadeButtonCount] = {2, 3, 5, 0, 1, 10, 7, 6, kNoButton, kNoButton};
	// 2.3, driving (Daytona USA: accelerator, brake, shift up, shift down, the four view buttons): the pedals on R2 and
	// L2 (pressed as far as the trigger is), the gears on R1 / L1, the views on Square Triangle Circle Cross
	static const int kDriving[kArcadeButtonCount] = {10, 9, 5, 4, 2, 3, 7, 6, 1, 0};
	if (i < 0 || i >= kArcadeButtonCount)
		return kNoButton;
	switch (layout)
	{
		case 1: return kClassic[i];
		case 2: return kFighting[i];
		case 4: return kFightingR2[i];
		case 5: return kDriving[i];
		case kCustomLayout:
		{
			const int b = custom ? custom[i] : kNoButton;
			return b >= 0 && b < kPs5ButtonCount ? b : kNoButton;
		}
		// auto: Capcom's PlayStation layout for 6-button games since 2.0 (it was Fighting, heavy kick on L1); 2.3:
		// Driving for the games with more than six buttons whose first two are pedals
		default:
			if (pedals && game_buttons > 6)
				return kDriving[i];
			return game_buttons >= 6 ? kFightingR2[i] : kClassic[i];
	}
}
namespace
{
int Next(const int* order, int n, int cur, int dir)
{
	int at = 0;
	for (int i = 0; i < n; i++)
		if (order[i] == cur)
			at = i;
	return order[((at + dir) % n + n) % n];
}
} // namespace

int NextLayout(int layout, int dir)
{
	static const int kOrder[] = {0, 1, 2, 4, 5, kCustomLayout};
	return Next(kOrder, int(sizeof(kOrder) / sizeof(kOrder[0])), layout, dir);
}

int NextPs5Button(int button, int dir)
{
	static const int kOrder[] = {0, 1, 2, 3, 4, 5, 9, 10, 6, 7, kNoButton};
	return Next(kOrder, int(sizeof(kOrder) / sizeof(kOrder[0])), button, dir);
}

int NextPlayer(int player, int dir)
{
	static const int kOrder[] = {0, 1, 2, 3, kNoPlayer};
	return Next(kOrder, int(sizeof(kOrder) / sizeof(kOrder[0])), player, dir);
}

void ChoosePlayer(int* players, int pad, int player)
{
	if (pad < 0 || pad >= kPads)
		return;
	const int old = players[pad];
	players[pad] = player;
	if (player == kNoPlayer || player == old)
		return;
	for (int o = 0; o < kPads; o++)
	{
		if (o == pad || players[o] != player)
			continue;
		players[o] = old;
		if (old == kNoPlayer)
		{
			// the lowest player nobody else has (there is one: `pad` had none)
			bool taken[kPads] = {};
			for (int i = 0; i < kPads; i++)
				if (i != o && players[i] >= 0 && players[i] < kPads)
					taken[players[i]] = true;
			for (int p = 0; p < kPads; p++)
				if (!taken[p])
				{
					players[o] = p;
					break;
				}
		}
	}
}
} // namespace emu

namespace
{
using fe::T;

// printf into a std::string (the translated formats)
std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

// Why a state wasn't loaded (burn::LoadState, in English for the log), in the interface's language.
std::string WhyShown(const std::string& why)
{
	const std::string other = "a save of another game (";
	if (why.compare(0, other.size(), other) == 0 && !why.empty() && why.back() == ')')
		return Fmt(T("a save of another game (%s)"), why.substr(other.size(), why.size() - other.size() - 1).c_str());
	return T(why.c_str());
}

constexpr int kTargetQueued = ps5audio::kRate * 60 / 1000; // 60 ms of sound
constexpr int kOsdWidth = ps5video::kWidth - 160; // the widest line of a message over the game
double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

bool FileExists(const std::string& p)
{
	struct stat st = {};
	return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void CoreLogLine(const char* line)
{
	OrbisLog("%s", line);
}

struct State
{
	bool inited = false;
	bool loaded = false;
	std::string path, set, title, board;
	int driver = -1;

	// video
	std::vector<uint32_t> frame;
	int fw = 0, fh = 0;
	bool new_frame = false;
	double fps = 60.0;
	bool vsynced = true; // the game runs at the display's 60 Hz (see the top of the file)
	double in_rate = 48000.0; // the core's sound, as it plays at the speed the game runs

	// sound: the core's samples of this frame, and the resampler's state
	std::vector<int16_t> in;
	bool mute = false; // fast forward: drop the sound
	double frac = 0;
	int16_t prev_l = 0, prev_r = 0;
	std::vector<int16_t> out;
	std::vector<int16_t> silence; // the zeros that refill a dry ring
	// 2.3: for the log -- the refills (each an audible gap: the game ran late), the slowest frames since the last line
	uint64_t refills = 0, refill_frames = 0;
	double slowest_ms = 0, slowest_core_ms = 0, last_frame_at = 0;
	bool audio_fresh = true; // the game just started or came back from the pause menu: its refill is not a gap

	// pads: the layout in use (PS5 button bit of each arcade button)
	unsigned map[emu::kArcadeButtonCount] = {};
	std::string map_logged; // the layout last written to the log
	// the arcade player each controller plays (emu::kNoPlayer: none)
	int pad_player[emu::kPads] = {0, 1, 2, 3};
	std::string players_logged;
	// the game's own button layout (config/<set>.btn), or its parent set's; else the general one (fbneo-ps5.ini)
	emu::LayoutOwner owner = emu::LayoutOwner::General;
	emu::ButtonLayout game_layout;
	// game_layout.players are the game's own (its file has controller_N keys, or they were changed in the game and differ
	// from the general setting's); else the game follows the general setting's players
	// the game's own players (game_pl: its file's controller_N keys, or its parent's, or changed in the game and different
	// from the general setting's); else (game_players false) the general setting's. Kept apart from the buttons: a file
	// can say "layout=general" (the buttons follow the general layout) and still have players of its own.
	bool game_players = false;
	int game_pl[emu::kPads] = {0, 1, 2, 3};
	bool players_parent = false; // game_pl came from the parent set's file
	bool players_from_file = false; // game_players as the game started (its file, or its parent's, had them)
	bool players_changed = false; // changed in the pause menu: the message again when the game resumes
	// the controllers connected (bit per slot) when the players message was last made, and a change seen since (frames
	// it has lasted: a user signing out moves the later users' pads up a slot, onto that slot's player)
	unsigned pads_shown = 0, pads_seen = 0;
	int pads_stable = 0;
	std::string parent, parent_title;
	// controller 1 (the foreground user's pad, whatever player it plays): the pause menu's combination
	uint32_t prev_p1 = 0;
	// the controllers (bit per slot) whose buttons that closed a menu (or opened it, or joined on Choose players) are
	// still held: nothing of them reaches the game until they are let go
	unsigned wait_release = 0xf;
	bool service = false, test = false;

	// the pause menu's Service / Test: frames left to hold the machine's button
	int service_frames = 0, test_frames = 0;

	// saves
	double next_nvram_check = 0;
	// 2.3: the periodic NVRAM save's file written on a thread of its own (burn::NvramTake / NvramDone)
	ps5::BigThread nv_writer;
	std::atomic<bool> nv_busy{false};
	std::atomic<bool> nv_result{false};
	std::vector<uint8_t> nv_file, nv_data;
	std::string nv_path;
	bool nv_threads = true; // false once a thread was refused: written on the game's thread
	double nv_started = 0;

	// 2.2: the hard disk's changes (Killer Instinct writes its audits and settings to its disk): every 5 s while they
	// change, the game's thread takes them and a thread of their own writes them (the game goes on meanwhile); the
	// pause menu, a save state and the end of the game write them at once, after that thread is done
	burn::HddJob hdd_job;
	ps5::BigThread hdd_writer;
	std::atomic<bool> hdd_busy{false};
	std::atomic<bool> hdd_result{false};
	std::atomic<bool> hdd_oom{false};
	bool hdd_threads = true;  // false once a thread was refused: written at the pause menu and at the end only
	bool hdd_failed = false;  // a write failed since the last one that succeeded (its notice given)
	bool hdd_read_noticed = false;
	double next_hdd_save = 0;
	uint64_t hdd_started_frame = 0;

	// messages and the FPS counter
	std::string osd;
	double osd_until = 0;
	bool overlay_was_drawn = false;
	int fps_frames = 0;
	double fps_since = 0, fps_shown = 0;
	uint64_t frames = 0;
	double next_stats = 0;
	int stats_reports = 0;
};
State g;


std::string StatePath(int slot)
{
	return OrbisDir("states") + "/" + g.set + ".state" + std::to_string(slot);
}

std::string DipPath()
{
	return OrbisDir("config") + "/" + g.set + ".dip";
}

// ---- button layouts per game (2.0): config/<set>.btn, the same key=value lines as fbneo-ps5.ini ---------------------
std::string BtnPath(const std::string& set)
{
	return OrbisDir("config") + "/" + set + ".btn";
}

emu::ButtonLayout GeneralLayout()
{
	const fe::Settings& c = fe::Config();
	emu::ButtonLayout l;
	l.layout = ((c.layout % emu::kLayoutCount) + emu::kLayoutCount) % emu::kLayoutCount;
	for (int i = 0; i < emu::kArcadeButtonCount; i++)
		l.buttons[i] = c.buttons[i];
	for (int i = 0; i < emu::kPads; i++)
		l.players[i] = c.pad_player[i];
	return l;
}

// What a .btn file holds: nothing (no file), a layout, or "layout=general" (the game follows the general layout
// even though its parent set has a layout of its own: "Default controls" in a clone).
enum class BtnFile
{
	None,
	Layout,
	General,
};

// players (when given): the controllers' players the file has (controller_N keys, 2.1), and *has_players whether it has
// any; a file without them (2.0's, or a game whose players were never changed) follows the general setting's players.
// A "layout=general" file can have them too (the buttons follow the general layout, the players are the game's).
BtnFile ReadBtn(const std::string& path, emu::ButtonLayout* out, int* players = nullptr, bool* has_players = nullptr)
{
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return BtnFile::None;
	emu::ButtonLayout l;
	for (int i = 0; i < emu::kPads; i++)
		l.players[i] = fe::Config().pad_player[i];
	const emu::ButtonLayout base = l; // the general setting's players: what the file's lines are read over
	bool general = false, has_layout = false;
	unsigned named = 0; // the controllers the file gives a player
	// spaces around the key and the value are allowed (a file edited by hand)
	auto trim = [](std::string t) {
		const size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
		return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
	};
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq = 0;
		const std::string key = trim(line), val = trim(eq + 1);
		const int n = atoi(val.c_str());
		if (key == "layout")
		{
			// "general", or a layout's number: anything else is not a layout
			general = val == "general";
			has_layout = general || (!val.empty() && val.find_first_not_of("0123456789") == std::string::npos);
			l.layout = std::max(0, std::min(emu::kLayoutCount - 1, n));
		}
		for (int i = 0; i < emu::kArcadeButtonCount; i++)
			if (key == emu::kArcadeButtons[i].key)
				l.buttons[i] = std::max(0, std::min(emu::kPs5ButtonCount - 1, n));
		// the player's number, 0 for none; a value that is not a number is ignored (that controller keeps its player)
		if (key.size() == 12 && key.compare(0, 11, "controller_") == 0 && key[11] >= '1' && key[11] < '1' + emu::kPads &&
			!val.empty() && val.find_first_not_of("0123456789") == std::string::npos)
		{
			l.players[key[11] - '1'] = std::max(0, std::min(emu::kPads, n)) - 1;
			named |= 1u << (key[11] - '1');
		}
	}
	fclose(f);
	fe::FixPadPlayers(l.players, named, base.players, path.substr(path.find_last_of('/') + 1).c_str());
	if (has_players)
		*has_players = named != 0 && has_layout;
	if (players)
		std::copy(l.players, l.players + emu::kPads, players);
	// an empty or damaged file (no valid layout= line) is ignored: the game follows its parent or the general layout
	if (!has_layout)
	{
		OrbisLog("[emu] %s has no valid layout= line: ignored", path.c_str());
		return BtnFile::None;
	}
	if (general)
		return BtnFile::General;
	if (out)
		*out = l;
	return BtnFile::Layout;
}

// l = nullptr: "layout=general"; players (when given): the controllers' players too (the game has players of its own)
bool WriteBtn(const std::string& path, const emu::ButtonLayout* l, const int* players)
{
	fe::PrepareFolders();
	const std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return false;
	fprintf(f, "# FBNeo PS5: this game's button layout (Settings, CONTROLS, from the pause menu)\n");
	if (!l)
		fprintf(f, "layout=general\n");
	else
	{
		fprintf(f, "layout=%d\n", l->layout);
		for (int i = 0; i < emu::kArcadeButtonCount; i++)
			fprintf(f, "%s=%d\n", emu::kArcadeButtons[i].key, l->buttons[i]);
	}
	// 2.1: the player each controller plays (0: none), only when this game has its own
	for (int i = 0; players && i < emu::kPads; i++)
		fprintf(f, "controller_%d=%d\n", i + 1, players[i] + 1);
	bool ok = fflush(f) == 0 && ferror(f) == 0;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		remove(tmp.c_str());
		return false;
	}
	return true;
}

// The layout the game starts with: its own file, else its parent's, else the general one. The players the same way: its
// own file's (a file without them: the general setting's), else its parent's file's, else the general setting's.
void FindGameLayout()
{
	g.owner = emu::LayoutOwner::General;
	g.game_players = g.players_parent = false;
	const std::string own = BtnPath(g.set);
	const BtnFile mine = ReadBtn(own, &g.game_layout, g.game_pl, &g.game_players);
	switch (mine)
	{
		case BtnFile::Layout:
			g.owner = emu::LayoutOwner::Game;
			OrbisLog("[emu] button layout of this game: %s", own.c_str());
			return;
		case BtnFile::General:
			OrbisLog("[emu] button layout: the general one (%s says so)", own.c_str());
			return;
		case BtnFile::None: break;
	}
	if (!g.parent.empty())
	{
		emu::ButtonLayout l;
		const BtnFile theirs = ReadBtn(BtnPath(g.parent), &l, g.game_pl, &g.game_players);
		g.players_parent = g.game_players;
		if (theirs == BtnFile::Layout)
		{
			g.game_layout = l;
			g.owner = emu::LayoutOwner::Parent;
			OrbisLog("[emu] button layout of the parent set: %s", BtnPath(g.parent).c_str());
			return;
		}
	}
	OrbisLog("[emu] button layout: the general one");
}

// Whether the parent set's file would give this clone something of its own (a button layout, or players): then the
// clone needs a file saying "layout=general" to follow the general ones.
bool ParentHasControls()
{
	if (g.parent.empty())
		return false;
	bool players = false;
	const BtnFile f = ReadBtn(BtnPath(g.parent), nullptr, nullptr, &players);
	return f == BtnFile::Layout || players;
}

// Writes the game's file after a change: its buttons (or "layout=general"), and its players when it has its own; no file
// when it has neither (and none is needed to keep a clone off its parent's layout).
bool SaveGameFile()
{
	const std::string path = BtnPath(g.set);
	const int* players = g.game_players ? g.game_pl : nullptr;
	if (g.owner == emu::LayoutOwner::Parent && players)
		g.owner = emu::LayoutOwner::Game; // its own file now: the parent's buttons, copied, with its players
	if (g.owner == emu::LayoutOwner::Game)
		return WriteBtn(path, &g.game_layout, players);
	if (g.owner == emu::LayoutOwner::Parent)
		return true; // nothing of its own
	if (players || ParentHasControls())
		return WriteBtn(path, nullptr, players);
	return !FileExists(path) || remove(path.c_str()) == 0;
}

// ---- pads -> the core ----------------------------------------------------------------------------------------
void ApplyLayout()
{
	const emu::ButtonLayout l = emu::Controls();
	const int n = burn::ButtonCount();
	std::string desc = emu::kLayouts[l.layout].name;
	if (l.layout == 0)
		desc += n > 6 && emu::GamePedals() ? " = Driving" : n >= 6 ? " = Fighting (R1 R2)" : " = Classic";
	desc += ":";
	for (int r = 0; r < emu::kArcadeButtonCount; r++)
	{
		const int i = emu::kArcadeButtonRows[r];
		const int b = l.Button(i, n);
		g.map[i] = emu::kPs5Buttons[b].bit;
		// (buttons 7 and 8 only for the games that have them: the line stays as it was for the others)
		const int gb = emu::GameButtonOf(i);
		if (gb >= 6 && gb >= n)
			continue;
		desc += std::string(" ") + (gb >= 0 ? std::to_string(gb + 1) : i == 6 ? "coin" : "start") + "=" + emu::kPs5Buttons[b].name;
	}
	static const char* const kOwner[] = {"general", "this game's", "the parent set's"};
	if (desc != g.map_logged)
	{
		g.map_logged = desc;
		OrbisLog("[emu] buttons (%s layout) %s", kOwner[int(emu::ControlsOwner())], desc.c_str());
	}
	std::string players;
	for (int c = 0; c < emu::kPads; c++)
	{
		g.pad_player[c] = l.players[c];
		players += " controller " + std::to_string(c + 1) + "=" +
			(l.players[c] == emu::kNoPlayer ? std::string("none") : "player " + std::to_string(l.players[c] + 1));
	}
	if (players != g.players_logged)
	{
		g.players_logged = players;
		OrbisLog("[emu] players (%s layout):%s", !g.loaded || !g.game_players ? kOwner[0] : g.players_parent ? kOwner[2] : kOwner[1],
			players.c_str());
	}
}

unsigned ConnectedPads()
{
	unsigned m = 0;
	for (int c = 0; c < emu::kPads; c++)
		if (ps5input::Snapshot(c).connected)
			m |= 1u << c;
	return m;
}

// Who plays whom, for a few seconds over the game ("Controller 1: Player 3"): controller 1 and the other connected
// ones. Shown when a game starts with a controller on another player than its own, or with controller 1 on no player
// of this game (none, or one the game doesn't have: "Player 4 (not in this game)", and once "This game has 2 players.");
// and (always = true) when the
// connected controllers change during the game.
void ShowPlayers(bool always)
{
	const int players = emu::GamePlayers();
	g.pads_shown = g.pads_seen = ConnectedPads();
	g.pads_stable = 0;
	std::vector<std::string> parts;
	bool show = always, beyond = false;
	for (int c = 0; c < emu::kPads; c++)
	{
		if (c > 0 && !(g.pads_shown & (1u << c)))
			continue;
		const int p = g.pad_player[c];
		const bool past = p != emu::kNoPlayer && players > 0 && p >= players;
		beyond = beyond || past;
		show = show || p != c || (c == 0 && (past || p == emu::kNoPlayer));
		parts.push_back(p == emu::kNoPlayer ? Fmt(T("Controller %d: no player"), c + 1)
			: past ? Fmt(T("Controller %d: Player %d (not in this game)"), c + 1, p + 1)
				   : Fmt(T("Controller %d: Player %d"), c + 1, p + 1));
	}
	if (!show)
		return;
	const size_t controllers = parts.size();
	if (beyond)
		parts.push_back(players == 1 ? std::string(T("This game has 1 player.")) : Fmt(T("This game has %d players."), players));
	// as many lines as it takes (ShowOverlays draws one per '\n'), each part whole on its line; the controllers
	// separated by commas, the player count a sentence of its own
	std::string text, line, log;
	for (size_t i = 0; i < parts.size(); i++)
	{
		const std::string sep = i < controllers ? "," : ".";
		const std::string longer = line.empty() ? parts[i] : line + sep + "  " + parts[i];
		if (!line.empty() && fe::TextWidth(longer.c_str(), 3) > kOsdWidth)
		{
			text += (text.empty() ? "" : "\n") + line + sep;
			line = parts[i];
		}
		else
			line = longer;
		log += (log.empty() ? "" : sep + "  ") + parts[i];
	}
	text += (text.empty() ? "" : "\n") + line;
	g.osd = text;
	g.osd_until = Now() + 4.0;
	OrbisLog("[emu] message: %s", log.c_str());
}

// Each frame: the connected controllers changed, and stayed so for half a second (not a read that failed once): who
// plays whom again.
void WatchPads()
{
	const unsigned now = ConnectedPads();
	if (now != g.pads_seen)
	{
		g.pads_seen = now;
		g.pads_stable = 0;
	}
	if (g.pads_seen != g.pads_shown && ++g.pads_stable >= 30)
	{
		OrbisLog("[emu] the connected controllers changed (%x -> %x)", g.pads_shown, g.pads_seen);
		ShowPlayers(true);
	}
}

int8_t Axis(uint8_t v)
{
	return int8_t(int(v) - 128);
}

burn::Input ReadPads()
{
	burn::Input in;
	ps5input::Poll();
	// each controller to the player it plays (Settings, CONTROLS; 2.1): a controller on Player 2 is player 2's stick,
	// buttons, coin and start. Each player has one controller at most (fe::FixPadPlayers).
	for (int c = 0; c < ps5input::kMaxPads && c < emu::kPads; c++)
	{
		const int p = g.pad_player[c];
		if (p < 0 || p >= burn::kMaxPlayers)
			continue;
		const ps5input::PadState s = ps5input::Snapshot(c);
		if (!s.connected)
			continue;
		// a controller whose buttons that closed a menu (or opened it, or joined) are held: nothing reaches the game
		if (g.wait_release & (1u << c))
			continue;
		burn::PlayerInput& pi = in.player[p];
		pi.up = s.buttons & SCE_PAD_BUTTON_UP; // the D-pad, or the left stick
		pi.down = s.buttons & SCE_PAD_BUTTON_DOWN;
		pi.left = s.buttons & SCE_PAD_BUTTON_LEFT;
		pi.right = s.buttons & SCE_PAD_BUTTON_RIGHT;
		for (int i = 0; i < emu::kArcadeButtonCount; i++)
		{
			const int b = emu::GameButtonOf(i);
			if (b < 0)
				continue;
			pi.button[b] = g.map[i] && (s.raw_buttons & g.map[i]);
			// 2.3: a button on a trigger also says how far it is pressed (a game's pedal takes that; the others the button)
			if (g.map[i] == SCE_PAD_BUTTON_L2)
				pi.pressure[b] = s.l2;
			else if (g.map[i] == SCE_PAD_BUTTON_R2)
				pi.pressure[b] = s.r2;
		}
		pi.coin = g.map[6] && (s.raw_buttons & g.map[6]);
		pi.start = g.map[7] && (s.raw_buttons & g.map[7]);
		pi.stick_x = Axis(s.lx);
		pi.stick_y = Axis(s.ly);
		pi.stick2_x = Axis(s.rx);
		pi.stick2_y = Axis(s.ry);
	}
	in.service = g.service;
	in.test = g.test;
	return in;
}

// ---- sound: the core's 48 kHz (at the speed the game runs) -> 48 kHz, the rate steered by the ring's fill -------
void FlushAudio()
{
	int frames = 0;
	const int16_t* s = burn::Sound(&frames);
	const fe::Settings& cfg = fe::Config();
	if (frames <= 0 || !cfg.audio || g.mute || !ps5audio::Available())
		return;
	g.in.assign(s, s + size_t(frames) * 2);
	int queued = ps5audio::Queued();
	const bool fresh = g.audio_fresh;
	g.audio_fresh = false;
	if (queued < kTargetQueued / 4)
	{
		// the ring ran (nearly) dry: a game just started, or came back from a menu, or the frame was late. Fill
		// it with silence up to the target at once; the rate control alone (0.5%) would take ~10 s to get there.
		g.silence.assign(size_t(kTargetQueued - queued) * 2, 0);
		ps5audio::Push(g.silence.data(), kTargetQueued - queued);
		if (!fresh)
		{
			g.refills++;
			g.refill_frames += uint64_t(kTargetQueued - queued);
			if (g.refills <= 20)
				OrbisLog("[emu] audio: %d ms left queued at frame %llu: %d ms of silence added (a gap: the game ran late)",
					queued * 1000 / ps5audio::kRate, (unsigned long long)g.frames, (kTargetQueued - queued) * 1000 / ps5audio::kRate);
		}
		queued = kTargetQueued;
	}
	double adj = double(kTargetQueued - queued) / kTargetQueued;
	adj = 1.0 + std::max(-1.0, std::min(1.0, adj)) * 0.005;
	const double step = g.in_rate / (ps5audio::kRate * adj); // input frames per output frame
	const int vol = cfg.volume * 256 / 100;
	g.out.clear();
	double pos = g.frac; // position in [prev, in...]: 0 = prev, 1 = in[0]
	while (pos < double(frames))
	{
		const int i = int(pos);
		const int t = int((pos - i) * 256);
		const int16_t l0 = i == 0 ? g.prev_l : g.in[size_t(i - 1) * 2];
		const int16_t r0 = i == 0 ? g.prev_r : g.in[size_t(i - 1) * 2 + 1];
		const int16_t l1 = g.in[size_t(i) * 2];
		const int16_t r1 = g.in[size_t(i) * 2 + 1];
		const int l = (l0 * (256 - t) + l1 * t) >> 8;
		const int r = (r0 * (256 - t) + r1 * t) >> 8;
		g.out.push_back(int16_t(std::max(-32768, std::min(32767, l * vol >> 8))));
		g.out.push_back(int16_t(std::max(-32768, std::min(32767, r * vol >> 8))));
		pos += step;
	}
	g.frac = pos - double(frames);
	g.prev_l = g.in[size_t(frames - 1) * 2];
	g.prev_r = g.in[size_t(frames - 1) * 2 + 1];
	ps5audio::Push(g.out.data(), int(g.out.size() / 2));
}

void TakePicture()
{
	int w = 0, h = 0;
	const uint32_t* px = burn::Picture(&w, &h);
	if (!px || w <= 0 || h <= 0)
		return;
	g.fw = w;
	g.fh = h;
	g.frame.assign(px, px + size_t(w) * h);
	g.new_frame = true;
}

// What the overlays show (taken on the game's thread; drawn by whichever thread draws the picture)
struct Overlay
{
	bool fps = false;
	double fps_shown = 0;
	std::string osd; // empty: none now
};

Overlay TakeOverlay()
{
	Overlay o;
	o.fps = fe::Config().show_fps;
	o.fps_shown = g.fps_shown;
	if (!g.osd.empty() && Now() < g.osd_until)
		o.osd = g.osd;
	return o;
}

void DrawOverlay(const Overlay& o, ps5video::Rect* damage)
{
	bool drawn = false;
	if (o.fps)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.1f fps", o.fps_shown);
		const int w = fe::TextWidth(buf, 3);
		ps5video::FillRect(ps5video::kWidth - w - 60, 20, w + 40, 50, ps5video::Rgb(0, 0, 0));
		fe::DrawText(ps5video::kWidth - w - 40, 28, buf, 3, ps5video::Rgb(255, 255, 255));
		drawn = true;
	}
	if (!o.osd.empty())
	{
		// one line per '\n' (the players message), the last one where a single line goes
		std::vector<std::string> lines;
		int w = 0;
		for (size_t a = 0; a <= o.osd.size();)
		{
			size_t b = o.osd.find('\n', a);
			if (b == std::string::npos)
				b = o.osd.size();
			lines.push_back(fe::FitText(o.osd.substr(a, b - a), 3, kOsdWidth));
			w = std::max(w, fe::TextWidth(lines.back().c_str(), 3));
			a = b + 1;
		}
		const int n = int(lines.size()), top = ps5video::kHeight - 100 - (n - 1) * 50;
		ps5video::DarkenRect(40, top, w + 40, 60 + (n - 1) * 50);
		for (int i = 0; i < n; i++)
			fe::DrawText(60, top + 10 + i * 50, lines[size_t(i)].c_str(), 3, ps5video::Rgb(255, 255, 255));
		drawn = true;
	}
	if (drawn || g.overlay_was_drawn)
		*damage = ps5video::Rect{0, 0, ps5video::kWidth, ps5video::kHeight};
	if (!drawn && g.overlay_was_drawn)
		ps5video::InvalidateFrame(); // clear what the overlay left outside the picture
	g.overlay_was_drawn = drawn;
}

void ShowOverlays(ps5video::Rect* damage)
{
	DrawOverlay(TakeOverlay(), damage);
}

// ---- 2.3: the picture of a game paced by the sound, drawn on a thread of its own -------------------------------------
// A game not at 60 Hz (Daytona USA's 57.52) is paced by its sound: one frame must take less than 1/fps. The picture
// (the shader, the overlays, the copy to the scan-out buffer and its flip, which waits for the flip before it) took the
// game's thread 8-9 ms of Daytona's 17.4 ms frame on a PS5, after the core's 9-10 ms: the race ran at ~54 fps and the
// sound ran dry (gaps). Here the game's thread hands frame N to this thread and goes on with frame N+1 while it is
// drawn. When that thread is still drawing the last one: the game's thread waits for it, unless the sound queued is
// low, then this frame is not drawn (never two in a row; counted in the log). The 60 Hz games (paced by the flips'
// vsync) draw on the game's thread as before. Every other user of the screen (the pause menu, the end of the game)
// waits until this thread is done (PictureIdle).
struct Picture
{
	ps5::BigThread thread;
	std::mutex lock;
	std::condition_variable work, idle;
	bool busy = false;
	// the job
	std::vector<uint32_t> frame;
	int fw = 0, fh = 0;
	double aspect = 1.0;
	int scale = 0, shader = 0;
	bool smooth = false, scanlines = false;
	Overlay overlay;
	// for the log (under the lock)
	double draw_ms = 0, draw_max_ms = 0, wait_ms = 0;
	uint64_t draws = 0, skips = 0, skips_total = 0;
	bool last_skipped = false;
	double last_ms = 0, handed_at = 0; // the last drawing's time; when this one was handed over
};
Picture* g_picture = nullptr;
bool g_picture_refused = false;
std::atomic<double> g_inline_draw_ms{0}; // (the 60 Hz games: drawn on the game's thread, for the log)
double g_inline_draw_max_ms = 0;
uint64_t g_inline_draws = 0;

#ifndef __PROSPERO__
// the host tests: FBNEO_HOST_PICTURE_DELAY_MS (the picture's thread that much slower), FBNEO_HOST_DRAW_SKIPS=1 (frames
// may go undrawn: off by default, so that the tests' screen dumps stay the same frames however fast the machine is)
double HostPictureDelayMs()
{
	const char* e = getenv("FBNEO_HOST_PICTURE_DELAY_MS");
	return e ? atof(e) : 0.0;
}
bool HostDrawSkips()
{
	const char* e = getenv("FBNEO_HOST_DRAW_SKIPS");
	return e && atoi(e) != 0;
}
#endif

void PictureLoop(Picture* p)
{
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lk(p->lock);
			p->work.wait(lk, [p] { return p->busy; });
		}
		const double t0 = Now();
		// the shader on 4 threads at most here (this one and 3): the game's thread and the core's 3 workers run meanwhile
		ps5crt::SetThreadLimit(4);
		ps5video::Rect r = p->frame.empty() ? ps5video::Rect{0, 0, 0, 0} :
			ps5video::DrawFrame(p->frame.data(), p->fw, p->fh, p->fh, p->aspect, ps5video::Scale(p->scale), p->smooth,
				p->scanlines, p->shader);
		ps5crt::SetThreadLimit(0);
		DrawOverlay(p->overlay, &r);
		ps5video::Present(r.x, r.y, r.w, r.h, false);
#ifndef __PROSPERO__
		if (const double d = HostPictureDelayMs(); d > 0)
			usleep(useconds_t(d * 1000.0));
#endif
		const double ms = (Now() - t0) * 1000.0;
		{
			std::lock_guard<std::mutex> lk(p->lock);
			p->draw_ms += ms;
			p->draw_max_ms = std::max(p->draw_max_ms, ms);
			p->last_ms = ms;
			p->draws++;
			p->busy = false;
		}
		p->idle.notify_all();
	}
}

// the thread, the first time a game paced by the sound runs (it lives as long as the app); null: draw on the game's
Picture* PictureThread()
{
	if (g_picture || g_picture_refused)
		return g_picture;
	Picture* p = new (std::nothrow) Picture;
	if (p)
	{
		p->thread = ps5::BigThread([p] { PictureLoop(p); }, 256 * 1024);
		if (!p->thread.joinable())
		{
			delete p;
			p = nullptr;
		}
	}
	if (!p)
	{
		g_picture_refused = true;
		OrbisLog("[emu] no thread for the picture (the system refused it): drawn on the game's thread");
		return nullptr;
	}
	g_picture = p;
	OrbisLog("[emu] the picture of the games paced by the sound is drawn on a thread of its own");
	return p;
}

// waits until the picture's thread has drawn what it was given (before anything else uses the screen)
void PictureIdle()
{
	if (!g_picture)
		return;
	std::unique_lock<std::mutex> lk(g_picture->lock);
	g_picture->idle.wait(lk, [] { return !g_picture->busy; });
}

} // namespace

// 2.3: whether a frame of a game paced by the sound goes undrawn (fe_emu.h): waiting for the picture's thread would let
// the sound run low (a frame not drawn costs less than a gap in the sound)
bool emu::SkipDrawing(bool picture_busy, int queued, int expected_wait, int target, bool last_skipped)
{
	return picture_busy && !last_skipped && queued < expected_wait + target / 2;
}

namespace
{
double FrameAspect();

// hands this frame to the picture's thread (true), or draws nothing (a skip); false: no thread, the caller draws
bool PictureSubmit()
{
	Picture* p = PictureThread();
	if (!p)
		return false;
	const fe::Settings& cfg = fe::Config();
	std::unique_lock<std::mutex> lk(p->lock);
	if (p->busy)
	{
		bool may_skip = cfg.audio && ps5audio::Available();
#ifndef __PROSPERO__
		may_skip = may_skip && HostDrawSkips();
#endif
		// the rest of its drawing, expected: as long as the last one took, less the time since it was handed over
		const double rest_ms = std::max(0.0, p->last_ms - (Now() - p->handed_at) * 1000.0);
		const int rest = int(rest_ms * ps5audio::kRate / 1000.0);
		if (may_skip && emu::SkipDrawing(true, ps5audio::Queued(), rest, kTargetQueued, p->last_skipped))
		{
			p->skips++;
			p->skips_total++;
			p->last_skipped = true;
			if (p->skips_total <= 20)
				OrbisLog("[emu] frame %llu not drawn: the picture's thread was still drawing the last one, %d ms of sound "
						 "queued", (unsigned long long)g.frames, ps5audio::Queued() * 1000 / ps5audio::kRate);
			g.new_frame = true; // (drawn with the next one)
			return true;
		}
		const double t0 = Now();
		p->idle.wait(lk, [p] { return !p->busy; });
		p->wait_ms += (Now() - t0) * 1000.0;
	}
	p->last_skipped = false;
	p->frame.assign(g.frame.begin(), g.frame.end());
	p->fw = g.fw;
	p->fh = g.fh;
	p->aspect = FrameAspect();
	p->scale = cfg.scale;
	p->smooth = cfg.smooth;
	p->scanlines = cfg.scanlines;
	p->shader = cfg.shader;
	p->overlay = TakeOverlay();
	p->handed_at = Now();
	p->busy = true;
	lk.unlock();
	p->work.notify_one();
	return true;
}

// ---- the hard disk's changes (2.2) --------------------------------------------------------------------------------------
std::string BaseName(const std::string& p)
{
	return p.substr(p.find_last_of('/') + 1);
}

// A write that failed: logged each time, one notice until a write succeeds at the pause menu or a save state.
void HddFailed(const char* when)
{
	OrbisLog("[emu] hard disk: the changes could not be written (%s)", when);
	if (g.hdd_failed)
		return;
	g.hdd_failed = true;
	ProsperoNotify("%s", T("FBNeo PS5: the hard disk's changes could not be saved (is the console's storage full?)"));
}

// The periodic writer's result, once it is done (wait: join it first).
void HddCollect(bool wait)
{
	if (!g.hdd_writer.joinable() || (!wait && g.hdd_busy.load()))
		return;
	g.hdd_writer.join();
	const bool ok = g.hdd_result.load();
	g.hdd_job.Done(ok);
	if (g.hdd_oom.exchange(false))
		OrbisLog("[emu] hard disk: out of memory in the writer: the changes are kept, written later");
	OrbisLog("[emu] hard disk: periodic save %s (frames %llu-%llu)", ok ? "done" : "failed",
		(unsigned long long)g.hdd_started_frame, (unsigned long long)g.frames);
	if (!ok)
		HddFailed("periodic save");
}

// Every disk's changes now (the pause menu, a save state, the end of the game): after the periodic writer. clear: a
// success ends the failed state (another failure gets its notice again).
bool HddFlushNow(const char* when, bool clear)
{
	HddCollect(true);
	if (!burn::HasDisk())
		return true;
	const bool dirty = burn::HddDirty();
	const bool ok = burn::HddFlush();
	if (dirty)
		OrbisLog("[emu] hard disk: changes written at the %s: %s", when, ok ? "ok" : "FAILED");
	if (!ok)
		HddFailed(when);
	else if (clear)
		g.hdd_failed = false;
	return ok;
}

// 2.3: the periodic NVRAM writer's result, once it is done (wait: join it first)
void NvCollect(bool wait)
{
	if (!g.nv_writer.joinable() || (!wait && g.nv_busy.load()))
		return;
	g.nv_writer.join();
	burn::NvramDone(g.nv_path, g.nv_data, g.nv_result.load());
	const double ms = (Now() - g.nv_started) * 1000.0;
	if (ms >= 50.0)
		OrbisLog("[emu] NVRAM: the periodic write took %.0f ms (on its own thread: the game went on)", ms);
}

// every 5 s: when the NVRAM changed, the game's thread takes it and a thread of its own writes the file (its fsync can
// take a while), the game going on; one save at a time; burn::Unload writes what is left at the end, after it
void NvPeriodic()
{
	NvCollect(false);
	if (g.nv_writer.joinable())
		return;
	if (!burn::NvramTake(&g.nv_file, &g.nv_data, &g.nv_path))
		return;
	g.nv_started = Now();
	if (g.nv_threads)
	{
		g.nv_busy = true;
		g.nv_writer = ps5::BigThread(
			[] {
				bool ok = false;
				try
				{
					ok = burn::NvramWriteFile(g.nv_path, g.nv_file);
				}
				catch (const std::bad_alloc&)
				{
				}
				g.nv_result = ok;
				g.nv_busy = false;
			},
			256 * 1024);
		if (g.nv_writer.joinable())
			return;
		g.nv_busy = false;
		g.nv_threads = false;
		OrbisLog("[emu] NVRAM: no thread for the periodic save (the system refused it): written on the game's thread");
	}
	burn::NvramDone(g.nv_path, g.nv_data, burn::NvramWriteFile(g.nv_path, g.nv_file));
}

// every 5 s while there are changes (the host tests: FBNEO_HOST_HDD_PERIOD_S)
double HddPeriod()
{
#ifndef __PROSPERO__
	if (const char* e = getenv("FBNEO_HOST_HDD_PERIOD_S"); e && atof(e) > 0)
		return atof(e);
#endif
	return 5.0;
}

void HddPeriodic(double now)
{
	if (!burn::HasDisk())
		return;
	HddCollect(false);
	if (now < g.next_hdd_save)
		return;
	g.next_hdd_save = now + HddPeriod();
	if (g.hdd_busy.load() || g.hdd_writer.joinable() || !g.hdd_threads || !burn::HddDirty())
		return;
	try
	{
		if (!g.hdd_job.Take())
			return;
		g.hdd_busy = true;
		g.hdd_started_frame = g.frames;
		OrbisLog("[emu] hard disk: periodic save of %zu bytes started (frame %llu)", g.hdd_job.Bytes(),
			(unsigned long long)g.frames);
		g.hdd_writer = ps5::BigThread(
			[] {
				bool ok = false;
				try
				{
					ok = g.hdd_job.Write();
				}
				catch (const std::bad_alloc&)
				{
					g.hdd_oom = true;
				}
				g.hdd_result = ok;
				g.hdd_busy = false;
			},
			256 * 1024);
		if (!g.hdd_writer.joinable())
		{
			g.hdd_busy = false;
			g.hdd_threads = false;
			g.hdd_job.Done(false);
			OrbisLog("[emu] hard disk: no thread for the periodic save (the system refused it): the changes are written at "
					 "the pause menu and at the end");
		}
	}
	catch (const std::bad_alloc&)
	{
		g.hdd_busy = false;
		g.hdd_job.Done(false);
		OrbisLog("[emu] hard disk: out of memory taking the changes: kept, written later");
	}
}

#ifndef __PROSPERO__
// The host tests (FBNEO_HOST_HDD_WRITES="<frame>:<lba>:<count>:<fill>;...", FBNEO_HOST_HDD_READS="<frame>:<lba>:<count>;..."):
// the game's IDE controller writing / reading sectors at those frames (the stand-in ROMs run no game code that would)
void HostDiskWrites()
{
	// (2.3: FBNEO_HOST_NVRAM_FILL="<frame>:<fill>;...": the NVRAM changed at those frames)
	if (const char* r = getenv("FBNEO_HOST_NVRAM_FILL"))
		for (const char* p = r; *p;)
		{
			unsigned long long frame = 0;
			unsigned fill = 0;
			if (sscanf(p, "%llu:%u", &frame, &fill) == 2 && frame == g.frames)
				burn::NvramTestFill(uint8_t(fill));
			const char* semi = strchr(p, ';');
			if (!semi)
				break;
			p = semi + 1;
		}
	if (!burn::HasDisk())
		return;
	if (const char* r = getenv("FBNEO_HOST_HDD_READS"))
		for (const char* p = r; *p;)
		{
			unsigned long long frame = 0;
			unsigned lba = 0, count = 0;
			if (sscanf(p, "%llu:%u:%u", &frame, &lba, &count) == 3 && frame == g.frames)
				burn::HddTestRead(lba, count);
			const char* semi = strchr(p, ';');
			if (!semi)
				break;
			p = semi + 1;
		}
	const char* e = getenv("FBNEO_HOST_HDD_WRITES");
	if (!e)
		return;
	for (const char* p = e; *p;)
	{
		unsigned long long frame = 0;
		unsigned lba = 0, count = 0, fill = 0;
		if (sscanf(p, "%llu:%u:%u:%u", &frame, &lba, &count, &fill) == 4 && frame == g.frames)
		{
			burn::HddTestWrite(lba, count, uint8_t(fill));
			OrbisLog("[emu] host: the IDE wrote %u sector(s) at %u (frame %llu)", count, lba, frame);
		}
		const char* semi = strchr(p, ';');
		if (!semi)
			break;
		p = semi + 1;
	}
}
#endif

// What happened to the saved disk changes as the game started: said once, for this start only.
void ReportDisks()
{
	g.hdd_failed = false;
	g.hdd_read_noticed = false;
	g.hdd_threads = true;
	g.next_hdd_save = Now() + HddPeriod();
	for (const burn::DiskReport& d : burn::LastStartDisks())
	{
		const std::string diff = BaseName(d.diff), aside = BaseName(d.set_aside_as), back = BaseName(d.put_back_from);
		OrbisLog("[emu] hard disk %s: %s%s%s%s%s%s%s", d.disk.c_str(), d.image.c_str(),
			d.set_aside ? (", " + diff + " set aside as " + aside).c_str() : "", d.put_back ? (", put back " + back).c_str() : "",
			d.set_aside_failed ? ", an old .diff could not be set aside" : "",
			d.put_back_failed ? ", its saved changes could not be put back" : "",
			d.saving_off ? (", saving off: " + d.why).c_str() : "", d.sha_mismatch ? ", not the SHA-1 the driver names" : "");
		if (d.set_aside)
			ProsperoNotify(T("FBNeo PS5: %s was made for another hard disk image: kept as %s"), diff.c_str(), aside.c_str());
		if (d.put_back)
			ProsperoNotify(T("FBNeo PS5: this hard disk image's saved changes were put back (%s)"), back.c_str());
		if (d.set_aside_failed)
			ProsperoNotify("%s", T("FBNeo PS5: an old .diff could not be set aside: saving is off for this game"));
		else if (d.put_back_failed)
			ProsperoNotify("%s", T("FBNeo PS5: this disk's saved changes could not be put back: saving is off for this game"));
		else if (d.saving_off)
			ProsperoNotify(T("FBNeo PS5: %s could not be checked: saving is off for this game"), diff.c_str());
	}
}

// A sector of the image that could not be read (a share that dropped): said once a game.
void WatchDiskReads()
{
	if (g.hdd_read_noticed || !burn::HasDisk())
		return;
	const uint32_t n = burn::HddReadErrors();
	if (!n)
		return;
	g.hdd_read_noticed = true;
	OrbisLog("[emu] hard disk: %u sector(s) of the image could not be read%s", n,
		burn::HddOnNetwork() ? " (it is on a network share)" : "");
	ProsperoNotify("%s", T("FBNeo PS5: parts of the hard disk image could not be read (a network share that dropped?)"));
}

double FrameAspect()
{
	const fe::Settings& cfg = fe::Config();
	switch (cfg.aspect % emu::kAspectCount)
	{
		case 1: return g.fh > 0 ? double(g.fw) / g.fh : 4.0 / 3.0;
		case 2: return 16.0 / 9.0;
		default: return burn::Aspect();
	}
}

ps5video::Rect DrawLast()
{
	if (g.frame.empty())
		return ps5video::Rect{0, 0, 0, 0};
	const fe::Settings& cfg = fe::Config();
	return ps5video::DrawFrame(g.frame.data(), g.fw, g.fh, g.fh, FrameAspect(), ps5video::Scale(cfg.scale), cfg.smooth,
		cfg.scanlines, cfg.shader);
}

// 2.3: the core's worker threads (Sega Model 2's 3D), started the first time a driver asks for them (its init), stopped
// with the core: three workers and the emulation thread; the core is told how many really started
constexpr int kPartWorkers = 3;
fe::PartPool* g_parts = nullptr;
void RunParts(burn::ParallelJob job, void* ctx, int parts)
{
	if (!g_parts)
	{
		g_parts = new (std::nothrow) fe::PartPool(kPartWorkers);
		if (g_parts)
			OrbisLog("[emu] %d worker thread(s) for the core", g_parts->Threads() - 1);
		// the threads that really started (none: the core then makes one part, not bands for threads it hasn't)
		burn::SetParallel(RunParts, g_parts ? g_parts->Threads() : 1);
	}
	if (!g_parts)
	{
		for (int i = 0; i < parts; i++)
			job(ctx, i);
		return;
	}
	g_parts->Run(job, ctx, parts);
}

void SetTiming()
{
	g.fps = burn::Fps();
	g.vsynced = std::fabs(g.fps - 60.0) <= 1.5;
	g.in_rate = g.vsynced ? 48000.0 * 60.0 / g.fps : 48000.0;
}
} // namespace

namespace emu
{
bool InitCore()
{
	if (g.inited)
		return true;
	FBNEO_STAGE(Emu, "init");
	burn::SetLog(CoreLogLine);
	burn::SetTranslate(fe::T);
	burn::Paths paths;
	paths.saves = OrbisDir("saves");
	paths.samples = OrbisDir("samples");
	paths.hiscore = OrbisDir("hiscore");
	paths.blend = OrbisDir("blend");
	paths.config = OrbisDir("config");
	fe::PrepareFolders();
	burn::SetRomFolders([] { return fe::RomRoots(); }); // 2.2: where else a hard disk image may be
	if (!burn::Init(paths))
		return false;
	burn::SetParallel(RunParts, 1 + kPartWorkers);
	g.inited = true;
	return true;
}

void DeinitCore()
{
	if (!g.inited)
		return;
	CloseGame();
	burn::Exit();
	burn::SetParallel(nullptr, 1);
	delete g_parts;
	g_parts = nullptr;
	g.inited = false;
}

void ApplySettings()
{
	const fe::Settings& s = fe::Config();
	burn::SetHiscores(s.hiscores);
	if (g.loaded)
		ApplyLayout();
}

bool LoadGame(const std::string& path, std::string* error)
{
	if (!g.inited)
		return false;
	FBNEO_STAGE(Emu, "load game");
	CloseGame();
	// "<dir>/<zip>#<set>": a clone kept inside its parent's archive (a merged set); FBNeo finds its ROMs in the parent
	const size_t hash = path.find('#', path.find_last_of('/') + 1);
	const std::string zip = hash == std::string::npos ? path : path.substr(0, hash);
	std::string file = zip.substr(zip.find_last_of('/') + 1);
	std::string set = hash == std::string::npos ? file : path.substr(hash + 1);
	if (set.size() > 4 && strcasecmp(set.c_str() + set.size() - 4, ".zip") == 0)
		set = set.substr(0, set.size() - 4);
	else if (set.size() > 3 && strcasecmp(set.c_str() + set.size() - 3, ".7z") == 0)
		set = set.substr(0, set.size() - 3);
	for (char& c : set)
		c = char(tolower(uint8_t(c)));
	const int drv = burn::FindDriver(set);
	if (drv < 0)
	{
		OrbisLog("[emu] could not start %s: no FBNeo driver is called %s", path.c_str(), set.c_str());
		if (error)
			*error = Fmt(T("%s is not a ROM set FBNeo knows (the zip must keep its set name, as in FBNeo's lists)."), file.c_str());
		return false;
	}
	const std::string dir = zip.substr(0, zip.find_last_of('/'));
	auto find = [&](const std::string& name) {
		if (name == set && hash == std::string::npos)
			return zip;
		std::string p = fe::FindSetZip(name);
		// not scanned (a game started from the command line): beside the game
		for (const char* ext : {".zip", ".7z"})
			if (p.empty() && FileExists(dir + "/" + name + ext))
				p = dir + "/" + name + ext;
		return p;
	};
	ApplySettings();
	// the parent set (its button layout, for a clone without one), read before the driver starts
	{
		burn::Driver dd, pd;
		g.parent.clear();
		g.parent_title.clear();
		if (burn::GetDriver(drv, &dd) && !dd.parent.empty())
		{
			g.parent = dd.parent;
			g.parent_title = burn::GetDriver(burn::FindDriver(dd.parent), &pd) ? pd.title : dd.parent;
		}
	}
	OrbisLog("[emu] loading %s (driver %d)", path.c_str(), drv);
	// the saved DIP switches go in before the driver starts (it may read them then: BIOS, region)
	const std::string dips = OrbisDir("config") + "/" + set + ".dip";
	if (!burn::Load(drv, find, error, dips))
		return false;
	const burn::Driver& d = burn::Current();
	g.path = path;
	g.set = d.name;
	g.title = d.title;
	g.board = d.system;
	g.driver = drv;
	if (FileExists(dips))
		OrbisLog("[emu] DIP switches: %s", dips.c_str());
	SetTiming();
	g.frame.clear();
	g.fw = g.fh = 0;
	g.in.clear();
	g.mute = false;
	g.loaded = true;
	g.wait_release = 0xf;
	g.players_changed = false;
	g.prev_p1 = 0;
	g.service = g.test = false;
	g.service_frames = g.test_frames = 0;
	g.frac = 0;
	g.prev_l = g.prev_r = 0;
	g.frames = 0;
	g.refills = g.refill_frames = 0;
	g.slowest_ms = g.slowest_core_ms = g.last_frame_at = 0;
	g.audio_fresh = true;
	g.fps_frames = 0;
	g.fps_since = Now();
	g.next_stats = Now() + 1.0;
	g.stats_reports = 0;
	g.next_nvram_check = Now() + 5.0;
	g.overlay_was_drawn = false;
	g.map_logged.clear();
	g.players_logged.clear();
	g.osd.clear();
	FindGameLayout();
	g.players_from_file = g.game_players;
	ApplyLayout();
	ps5input::Poll();
	ShowPlayers(false);
	ps5video::InvalidateFrame();
	// realtime from here: the set was read as the driver started (from a share too); the shelf's check pass is stopped,
	// and nothing in netfs waits on the network for the game's thread unless the driver reads a ROM again (a reset)
	netfs_set_realtime(true);
	OrbisLog("[emu] running %s \"%s\" (%s), %.3f fps (%s), aspect %.4f, %d button(s)", g.set.c_str(), g.title.c_str(),
		g.board.c_str(), g.fps, g.vsynced ? "at 60 Hz, vsync" : "paced by the sound", burn::Aspect(), burn::ButtonCount());
	ReportDisks();
	return true;
}

void CloseGame()
{
	if (!g.loaded)
		return;
	FBNEO_STAGE(Emu, "close game");
	PictureIdle();
	if (g_picture && g_picture->skips_total)
		OrbisLog("[emu] frames not drawn in this game: %llu", (unsigned long long)g_picture->skips_total);
	if (g_picture)
	{
		g_picture->skips_total = 0;
		g_picture->last_skipped = false;
	}
	OrbisLog("[emu] inputs the game got: %s", burn::InputReport().c_str());
	HddFlushNow("game's end", false); // the periodic writer done first, then the hard disk's changes
	NvCollect(true); // the periodic NVRAM writer done first: Unload writes what changed since
	burn::Unload(); // writes the NVRAM (and the core its EEPROM and high scores)
	g.loaded = false;
	g.frame.clear();
	netfs_set_realtime(false);
	OrbisLog("[emu] game closed");
}

bool GameLoaded()
{
	return g.loaded;
}

std::string GameName()
{
	return g.set;
}

std::string GameTitle()
{
	return g.title;
}

std::string SystemName()
{
	return g.loaded ? g.board : "";
}

void Pause()
{
	PictureIdle();
	HddFlushNow("pause menu", true); // the hard disk's changes, at once
}

void Resume()
{
	ApplySettings();
	g.wait_release = 0xf;
	if (g.players_changed)
	{
		g.players_changed = false;
		ShowPlayers(true); // the players changed in the pause menu: who plays whom now
	}
	ps5video::InvalidateFrame();
	g.new_frame = true;
	g.last_frame_at = 0; // (the time in the pause menu is not a slow frame)
	g.audio_fresh = true;
}

void Osd(const std::string& text)
{
	g.osd = T(text.c_str());
	g.osd_until = Now() + 2.5;
}

bool SaveState(int slot)
{
	if (!g.loaded)
		return false;
	bool ok = false;
	try
	{
		ok = burn::SaveState(StatePath(slot));
	}
	catch (const std::bad_alloc&)
	{
		OrbisLog("[emu] save state %d: out of memory", slot); // 2.2: operator new throws (it trapped up to 2.1)
	}
	OrbisLog("[emu] save state %d: %s", slot, ok ? "ok" : "failed");
	HddFlushNow("save state", true); // and the hard disk's changes with it
	Osd(ok ? Fmt(T("State saved to slot %d"), slot) : "Could not save the state");
	return ok;
}

bool LoadState(int slot)
{
	if (!g.loaded)
		return false;
	if (!FileExists(StatePath(slot)))
	{
		Osd(Fmt(T("Slot %d is empty"), slot));
		return false;
	}
	std::string why;
	bool ok = false;
	try
	{
		ok = burn::LoadState(StatePath(slot), &why);
	}
	catch (const std::bad_alloc&)
	{
		// 2.2: a damaged state file can declare 512 MiB: the allocation's std::bad_alloc is the load failing
		why = "a damaged save";
		OrbisLog("[emu] load state %d: out of memory reading it (a damaged state?)", slot);
	}
	OrbisLog("[emu] load state %d: %s%s%s", slot, ok ? "ok" : "failed", ok ? "" : ": ", why.c_str());
	Osd(ok ? Fmt(T("State loaded from slot %d"), slot) : Fmt(T("Could not load the state: %s"), WhyShown(why).c_str()));
	return ok;
}

bool StateExists(int slot)
{
	return g.loaded && FileExists(StatePath(slot));
}

void Reset()
{
	if (!g.loaded)
		return;
	burn::Reset();
	OrbisLog("[emu] reset");
}

void PressService()
{
	g.service_frames = 15; // a quarter of a second
	OrbisLog("[emu] service button (pause menu)");
}

void PressTest()
{
	g.test_frames = 15;
	OrbisLog("[emu] test switch (pause menu)");
}

void PowerCycle()
{
	if (!g.loaded)
		return;
	const std::string path = g.path;
	CloseGame();
	std::string error;
	if (!LoadGame(path, &error))
	{
		OrbisLog("[emu] power cycle: %s", error.c_str());
		ProsperoNotify(T("FBNeo PS5: the game could not be started again. %s"), error.c_str());
	}
}

std::vector<burn::DipGroup> Dips()
{
	return g.loaded ? burn::Dips() : std::vector<burn::DipGroup>();
}

void SetDip(int group, int option)
{
	if (!g.loaded)
		return;
	burn::SetDip(group, option);
	const bool ok = burn::SaveDips(DipPath());
	OrbisLog("[emu] DIP switch %d -> %d (%s %s)", group, option, DipPath().c_str(), ok ? "saved" : "not saved");
}

void ResetDips()
{
	if (!g.loaded)
		return;
	burn::ResetDips();
	burn::SaveDips(DipPath());
	OrbisLog("[emu] DIP switches back to the game's defaults");
}

LayoutOwner ControlsOwner()
{
	return g.loaded ? g.owner : LayoutOwner::General;
}

std::string ParentTitle()
{
	return g.loaded ? g.parent_title : "";
}

ButtonLayout Controls()
{
	ButtonLayout l = ControlsOwner() == LayoutOwner::General ? GeneralLayout() : g.game_layout;
	for (int i = 0; i < kPads; i++)
		l.players[i] = g.loaded && g.game_players ? g.game_pl[i] : fe::Config().pad_player[i];
	return l;
}

namespace
{
// The game's players become `players`: kept for this game when they already were (its file, or its parent's, had them)
// or now differ from the general setting's; else it follows the general setting's.
void TakePlayers(const int* players)
{
	int p[kPads];
	std::copy(players, players + kPads, p);
	fe::FixPadPlayers(p);
	bool differ = false, changed = false;
	const ButtonLayout before = Controls();
	for (int i = 0; i < kPads; i++)
	{
		differ = differ || p[i] != fe::Config().pad_player[i];
		changed = changed || p[i] != before.players[i];
	}
	g.game_players = g.players_from_file || differ;
	if (changed)
		g.players_parent = false;
	std::copy(p, p + kPads, g.game_pl);
	g.players_changed = g.players_changed || changed;
}
} // namespace

void SetControls(const ButtonLayout& l)
{
	if (!g.loaded)
	{
		fe::Settings& c = fe::Config();
		c.layout = l.layout;
		for (int i = 0; i < kArcadeButtonCount; i++)
			c.buttons[i] = l.buttons[i];
		for (int i = 0; i < kPads; i++)
			c.pad_player[i] = l.players[i];
		fe::FixPadPlayers(c.pad_player);
		return;
	}
	g.game_layout = l;
	TakePlayers(l.players);
	g.owner = LayoutOwner::Game;
	const bool ok = SaveGameFile();
	OrbisLog("[emu] button layout for %s only: %s (%s %s)", g.set.c_str(), kLayouts[l.layout].name, BtnPath(g.set).c_str(),
		ok ? "saved" : "not saved");
	ApplyLayout();
}

void SetPlayers(const int* players)
{
	if (!g.loaded)
	{
		fe::Settings& c = fe::Config();
		std::copy(players, players + kPads, c.pad_player);
		fe::FixPadPlayers(c.pad_player);
		return;
	}
	TakePlayers(players);
	const bool ok = SaveGameFile();
	OrbisLog("[emu] players for %s only (%s): %s", g.set.c_str(), g.game_players ? "its own" : "the general ones",
		ok ? "saved" : "not saved");
	ApplyLayout();
}

void DefaultControls()
{
	if (!g.loaded)
	{
		fe::Config().DefaultButtons();
		fe::Config().DefaultPadPlayers();
		fe::Config().layout = 0;
		return;
	}
	const std::string path = BtnPath(g.set);
	// a clone whose parent set has a layout: its file says "general" (without a file it would follow its parent)
	// (or players: a "layout=general" file with controller_N keys, as Choose players writes)
	if (ParentHasControls())
	{
		const bool ok = WriteBtn(path, nullptr, nullptr);
		OrbisLog("[emu] %s: back to the general button layout (%s %s)", g.set.c_str(), path.c_str(), ok ? "says so" : "not saved");
	}
	else
	{
		const bool had = FileExists(path);
		const bool removed = had && remove(path.c_str()) == 0;
		OrbisLog("[emu] %s: back to the general button layout (%s)", g.set.c_str(),
			!had ? "it had no file" : removed ? (path + " removed").c_str() : (path + " could not be removed").c_str());
	}
	g.owner = LayoutOwner::General;
	g.game_players = g.players_from_file = g.players_parent = false;
	ApplyLayout();
}

bool GamePedals()
{
	return g.loaded && burn::AnalogButton(0) && burn::AnalogButton(1);
}

std::string GameButtonName(int i)
{
	return g.loaded ? burn::ButtonName(i) : "";
}

int GameButtonCount()
{
	return g.loaded ? burn::ButtonCount() : 0;
}

int GamePlayers()
{
	return g.loaded ? burn::Current().players : 0;
}

void RedrawLastFrame()
{
	PictureIdle();
	DrawLast();
}

FrameResult RunFrame()
{
	if (!g.loaded)
		return FrameResult::Stopped;
	fe::Settings& cfg = fe::Config();

	// -- the pause menu's combination (controller 1, whatever player it plays)
	ps5input::Poll();
	const uint32_t raw = ps5input::Pad(0).raw_buttons;
	WatchPads();
	// each controller is let through once it has let go of the buttons the menus use
	const uint32_t menu_buttons = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3 | SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE |
		SCE_PAD_BUTTON_SQUARE | SCE_PAD_BUTTON_OPTIONS;
	if (g.wait_release & 1u)
		g.prev_p1 = raw;
	for (int c = 0; c < emu::kPads; c++)
		if ((g.wait_release & (1u << c)) && !(ps5input::Pad(c).raw_buttons & menu_buttons))
			g.wait_release &= ~(1u << c);
	const uint32_t pressed = raw & ~g.prev_p1;
	g.prev_p1 = raw;
	const uint32_t menu_combo = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3;
	if ((raw & menu_combo) == menu_combo && (pressed & menu_combo))
	{
		PictureIdle(); // the last frame drawn before the menu draws
		return FrameResult::OpenMenu;
	}

	// the machine's service and test buttons: held for a few frames after the pause menu's Service / Test
	g.service = g.service_frames > 0;
	g.test = g.test_frames > 0;
	if (g.service_frames > 0)
		g.service_frames--;
	if (g.test_frames > 0)
		g.test_frames--;

	// -- emulate
	FBNEO_STAGE(Emu, "frame");
	const burn::Input in = ReadPads();
#ifndef __PROSPERO__
	// the host tests (FBNEO_HOST_FRAME_LOG=N): how long the core's frame takes, for the first N frames
	static const long frame_log = getenv("FBNEO_HOST_FRAME_LOG") ? atol(getenv("FBNEO_HOST_FRAME_LOG")) : 0;
#endif
	const double t_core = Now();
	burn::RunFrame(in, true);
	const double core_ms = (Now() - t_core) * 1000.0;
	g.slowest_core_ms = std::max(g.slowest_core_ms, core_ms);
	if (g.last_frame_at > 0)
		g.slowest_ms = std::max(g.slowest_ms, (t_core - g.last_frame_at) * 1000.0);
	g.last_frame_at = t_core;
#ifndef __PROSPERO__
	if (frame_log && g.frames < uint64_t(frame_log))
		OrbisLog("[emu] frame %llu: core %.2f ms", (unsigned long long)g.frames, core_ms);
#endif
#ifndef __PROSPERO__
	HostDiskWrites();
#endif
	TakePicture();
	// 2.3: a game paced by the sound hands its picture to the picture's thread before its sound goes out (whether this
	// frame may go undrawn depends on the sound queued before it); the 60 Hz games draw after it, as before
	bool want_draw = g.new_frame || (!g.osd.empty() && Now() < g.osd_until + 0.1) || cfg.show_fps;
	if (want_draw && !g.vsynced)
	{
		g.new_frame = false;
		if (PictureSubmit())
			want_draw = false; // (handed over, or not drawn: then g.new_frame again, for the next frame)
		else
			g.new_frame = true;
	}
	FlushAudio();
	g.frames++;
	g.fps_frames++;
	if (std::fabs(burn::Fps() - g.fps) > 0.01)
	{
		// a driver that changed its refresh rate (a few do, at start)
		SetTiming();
		OrbisLog("[emu] the game now runs at %.3f fps (%s)", g.fps, g.vsynced ? "vsync" : "paced by the sound");
	}

	// -- picture
	const double now = Now();
	if (now - g.fps_since >= 1.0)
	{
		g.fps_shown = g.fps_frames / (now - g.fps_since);
		g.fps_frames = 0;
		g.fps_since = now;
	}
	if (want_draw)
	{
		g.new_frame = false;
		{
			PictureIdle(); // (a game whose rate changed to 60 Hz: the picture's thread done first)
			const double t0 = Now();
			ps5video::Rect r = DrawLast();
			ShowOverlays(&r);
			ps5video::Present(r.x, r.y, r.w, r.h, g.vsynced);
			const double ms = (Now() - t0) * 1000.0;
			g_inline_draw_ms = g_inline_draw_ms + ms;
			g_inline_draw_max_ms = std::max(g_inline_draw_max_ms, ms);
			g_inline_draws++;
		}
	}

	// -- pacing on the sound: the games not at 60 Hz, or a ring filling up
	{
		if (cfg.audio && ps5audio::Available())
		{
			const int limit = g.vsynced ? kTargetQueued * 2 : kTargetQueued;
			const double give_up = Now() + 0.1;
			while (ps5audio::Queued() > limit && Now() < give_up)
				usleep(1000);
		}
		else if (!g.vsynced)
		{
			static double next = 0;
			if (next < now - 0.1)
				next = now;
			next += 1.0 / g.fps;
			if (next > Now())
				usleep(useconds_t((next - Now()) * 1e6));
		}
	}

	// -- NVRAM, every 5 s when it changed; the hard disk's changes the same, on a thread; the log, now and then
	if (now >= g.next_nvram_check)
	{
		g.next_nvram_check = now + 5.0;
		NvPeriodic();
	}
	NvCollect(false); // (the periodic NVRAM writer's result, when it is done)
	HddPeriodic(now);
	if (g.frames % 60 == 0)
		WatchDiskReads();
	if (now >= g.next_stats)
	{
		g.stats_reports++;
		g.next_stats = now + (g.stats_reports < 3 ? 5.0 : 60.0);
		PictureIdle(); // (the shader's time is the picture thread's)
		const double shader_ms = ps5video::TakeShaderMs();
		// the picture: drawn on its own thread (draw + present a frame, the game's thread waiting for it, frames not
		// drawn) or on the game's
		char pic[160];
		if (g_picture && g_picture->draws + g_picture->skips > 0)
		{
			snprintf(pic, sizeof pic, "picture on its thread %.1f ms a frame (slowest %.1f), the game waited %.1f ms in all, "
				"%llu frame(s) not drawn", g_picture->draw_ms / double(std::max<uint64_t>(1, g_picture->draws)),
				g_picture->draw_max_ms, g_picture->wait_ms, (unsigned long long)g_picture->skips);
			g_picture->draw_ms = g_picture->draw_max_ms = g_picture->wait_ms = 0;
			g_picture->draws = g_picture->skips = 0;
		}
		else
			snprintf(pic, sizeof pic, "picture %.1f ms a frame (slowest %.1f)",
				g_inline_draws ? g_inline_draw_ms / double(g_inline_draws) : 0.0, g_inline_draw_max_ms);
		g_inline_draw_ms = 0;
		g_inline_draw_max_ms = 0;
		g_inline_draws = 0;
		OrbisLog("[emu] frame %llu, %.1f fps, audio queued %d (%.0f ms), underruns %llu, shader %.1f ms; slowest frame "
				 "%.1f ms (core %.1f ms), silence refills %llu (%.0f ms); %s",
			(unsigned long long)g.frames, g.fps_shown, ps5audio::Queued(), ps5audio::Queued() * 1000.0 / ps5audio::kRate,
			(unsigned long long)ps5audio::Underruns(), shader_ms, g.slowest_ms, g.slowest_core_ms,
			(unsigned long long)g.refills, g.refill_frames * 1000.0 / ps5audio::kRate, pic);
		g.slowest_ms = g.slowest_core_ms = 0;
	}
	return FrameResult::Continue;
}
} // namespace emu
