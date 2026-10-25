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

#include "ProsperoNotify.h"
#include "fe_games.h"
#include "fe_i18n.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "netfs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
};

int LayoutButton(int layout, int i, int game_buttons, const int* custom)
{
	// indexes in kPs5Buttons: 0 Cross, 1 Circle, 2 Square, 3 Triangle, 4 L1, 5 R1, 6 OPTIONS, 7 touchpad, 9 L2, 10 R2
	// classic (FBNeo's libretro pad): buttons 1-4 = Cross, Circle, Square, Triangle (Neo Geo A B C D), 5-6 = L1 R1
	static const int kClassic[kArcadeButtonCount] = {0, 1, 2, 3, 4, 5, 7, 6};
	// fighting (Capcom's 6 buttons, punches on top): Square Triangle R1 = 1 2 3, Cross Circle L1 = 4 5 6
	static const int kFighting[kArcadeButtonCount] = {2, 3, 5, 0, 1, 4, 7, 6};
	// fighting with the triggers (Capcom's own PS layout): Square Triangle R1 = 1 2 3, Cross Circle R2 = 4 5 6
	static const int kFightingR2[kArcadeButtonCount] = {2, 3, 5, 0, 1, 10, 7, 6};
	if (i < 0 || i >= kArcadeButtonCount)
		return kNoButton;
	switch (layout)
	{
		case 1: return kClassic[i];
		case 2: return kFighting[i];
		case 4: return kFightingR2[i];
		case kCustomLayout:
		{
			const int b = custom ? custom[i] : kNoButton;
			return b >= 0 && b < kPs5ButtonCount ? b : kNoButton;
		}
		// auto: Capcom's PlayStation layout for 6-button games since 2.0 (it was Fighting, heavy kick on L1)
		default: return game_buttons >= 6 ? kFightingR2[i] : kClassic[i];
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
	static const int kOrder[] = {0, 1, 2, 4, kCustomLayout};
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

	// pads: the layout in use (PS5 button bit of each arcade button)
	unsigned map[emu::kArcadeButtonCount] = {};
	std::string map_logged; // the layout last written to the log
	// the arcade player each controller plays (emu::kNoPlayer: none)
	int pad_player[emu::kPads] = {0, 1, 2, 3};
	std::string players_logged;
	// the game's own button layout (config/<set>.btn), or its parent set's; else the general one (fbneo-ps5.ini)
	emu::LayoutOwner owner = emu::LayoutOwner::General;
	emu::ButtonLayout game_layout;
	std::string parent, parent_title;
	// controller 1 (the foreground user's pad, whatever player it plays): the pause menu's combination, and nothing of
	// it reaches the game while the buttons that closed a menu (or opened it) are held
	uint32_t prev_p1 = 0;
	bool wait_release = true;
	bool service = false, test = false;

	// the pause menu's Service / Test: frames left to hold the machine's button
	int service_frames = 0, test_frames = 0;

	// saves
	double next_nvram_check = 0;

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

BtnFile ReadBtn(const std::string& path, emu::ButtonLayout* out)
{
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return BtnFile::None;
	emu::ButtonLayout l;
	// the controllers' players: a file without them (2.0's) keeps the general setting's
	for (int i = 0; i < emu::kPads; i++)
		l.players[i] = fe::Config().pad_player[i];
	bool general = false, has_layout = false;
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
		if (key.size() == 12 && key.compare(0, 11, "controller_") == 0 && key[11] >= '1' && key[11] < '1' + emu::kPads)
			l.players[key[11] - '1'] = std::max(0, std::min(emu::kPads, n)) - 1; // 0: none
	}
	fclose(f);
	fe::FixPadPlayers(l.players);
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

// l = nullptr: "layout=general"
bool WriteBtn(const std::string& path, const emu::ButtonLayout* l)
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
		// 2.1: the player each controller plays (0: none)
		for (int i = 0; i < emu::kPads; i++)
			fprintf(f, "controller_%d=%d\n", i + 1, l->players[i] + 1);
	}
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

// The layout the game starts with: its own file, else its parent's, else the general one.
void FindGameLayout()
{
	g.owner = emu::LayoutOwner::General;
	const std::string own = BtnPath(g.set);
	switch (ReadBtn(own, &g.game_layout))
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
	if (!g.parent.empty() && ReadBtn(BtnPath(g.parent), &g.game_layout) == BtnFile::Layout)
	{
		g.owner = emu::LayoutOwner::Parent;
		OrbisLog("[emu] button layout of the parent set: %s", BtnPath(g.parent).c_str());
		return;
	}
	OrbisLog("[emu] button layout: the general one");
}

// ---- pads -> the core ----------------------------------------------------------------------------------------
void ApplyLayout()
{
	const emu::ButtonLayout l = emu::Controls();
	const int n = burn::ButtonCount();
	std::string desc = emu::kLayouts[l.layout].name;
	if (l.layout == 0)
		desc += n >= 6 ? " = Fighting (R1 R2)" : " = Classic";
	desc += ":";
	for (int i = 0; i < emu::kArcadeButtonCount; i++)
	{
		const int b = l.Button(i, n);
		g.map[i] = emu::kPs5Buttons[b].bit;
		desc += std::string(" ") + (i < 6 ? std::to_string(i + 1) : i == 6 ? "coin" : "start") + "=" + emu::kPs5Buttons[b].name;
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
		OrbisLog("[emu] players (%s layout):%s", kOwner[int(emu::ControlsOwner())], players.c_str());
	}
}

// When a game starts with controllers on other players than their own, a message says so ("Controller 1: Player 3"):
// controller 1 and the other connected ones.
void ShowPlayers()
{
	std::string text;
	bool moved = false;
	for (int c = 0; c < emu::kPads; c++)
	{
		if (c > 0 && !ps5input::Snapshot(c).connected)
			continue;
		const int p = g.pad_player[c];
		moved = moved || p != c;
		text += (text.empty() ? "" : ",  ") +
			(p == emu::kNoPlayer ? Fmt(T("Controller %d: no player"), c + 1) : Fmt(T("Controller %d: Player %d"), c + 1, p + 1));
	}
	if (!moved)
		return;
	g.osd = text;
	g.osd_until = Now() + 4.0;
	OrbisLog("[emu] message: %s", text.c_str());
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
		// controller 1 while the buttons that closed a menu (or opened it) are held: nothing reaches the game
		if (c == 0 && g.wait_release)
			continue;
		burn::PlayerInput& pi = in.player[p];
		pi.up = s.buttons & SCE_PAD_BUTTON_UP; // the D-pad, or the left stick
		pi.down = s.buttons & SCE_PAD_BUTTON_DOWN;
		pi.left = s.buttons & SCE_PAD_BUTTON_LEFT;
		pi.right = s.buttons & SCE_PAD_BUTTON_RIGHT;
		for (int b = 0; b < 6; b++)
			pi.button[b] = g.map[b] && (s.raw_buttons & g.map[b]);
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
	if (queued < kTargetQueued / 4)
	{
		// the ring ran (nearly) dry: a game just started, or came back from a menu, or the frame was late. Fill
		// it with silence up to the target at once; the rate control alone (0.5%) would take ~10 s to get there.
		g.silence.assign(size_t(kTargetQueued - queued) * 2, 0);
		ps5audio::Push(g.silence.data(), kTargetQueued - queued);
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

void ShowOverlays(ps5video::Rect* damage)
{
	const fe::Settings& cfg = fe::Config();
	const double now = Now();
	bool drawn = false;
	if (cfg.show_fps)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.1f fps", g.fps_shown);
		const int w = fe::TextWidth(buf, 3);
		ps5video::FillRect(ps5video::kWidth - w - 60, 20, w + 40, 50, ps5video::Rgb(0, 0, 0));
		fe::DrawText(ps5video::kWidth - w - 40, 28, buf, 3, ps5video::Rgb(255, 255, 255));
		drawn = true;
	}
	if (!g.osd.empty() && now < g.osd_until)
	{
		const std::string text = fe::FitText(g.osd, 3, ps5video::kWidth - 160);
		const int w = fe::TextWidth(text.c_str(), 3);
		ps5video::DarkenRect(40, ps5video::kHeight - 100, w + 40, 60);
		fe::DrawText(60, ps5video::kHeight - 90, text.c_str(), 3, ps5video::Rgb(255, 255, 255));
		drawn = true;
	}
	if (drawn || g.overlay_was_drawn)
		*damage = ps5video::Rect{0, 0, ps5video::kWidth, ps5video::kHeight};
	if (!drawn && g.overlay_was_drawn)
		ps5video::InvalidateFrame(); // clear what the overlay left outside the picture
	g.overlay_was_drawn = drawn;
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
	if (!burn::Init(paths))
		return false;
	g.inited = true;
	return true;
}

void DeinitCore()
{
	if (!g.inited)
		return;
	CloseGame();
	burn::Exit();
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
	g.wait_release = true;
	g.prev_p1 = 0;
	g.service = g.test = false;
	g.service_frames = g.test_frames = 0;
	g.frac = 0;
	g.prev_l = g.prev_r = 0;
	g.frames = 0;
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
	ApplyLayout();
	ps5input::Poll();
	ShowPlayers();
	ps5video::InvalidateFrame();
	// realtime from here: the set was read as the driver started (from a share too); the shelf's check pass is stopped,
	// and nothing in netfs waits on the network for the game's thread unless the driver reads a ROM again (a reset)
	netfs_set_realtime(true);
	OrbisLog("[emu] running %s \"%s\" (%s), %.3f fps (%s), aspect %.4f, %d button(s)", g.set.c_str(), g.title.c_str(),
		g.board.c_str(), g.fps, g.vsynced ? "at 60 Hz, vsync" : "paced by the sound", burn::Aspect(), burn::ButtonCount());
	return true;
}

void CloseGame()
{
	if (!g.loaded)
		return;
	FBNEO_STAGE(Emu, "close game");
	OrbisLog("[emu] inputs the game got: %s", burn::InputReport().c_str());
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

void Pause() {}

void Resume()
{
	ApplySettings();
	g.wait_release = true;
	ps5video::InvalidateFrame();
	g.new_frame = true;
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
	const bool ok = burn::SaveState(StatePath(slot));
	OrbisLog("[emu] save state %d: %s", slot, ok ? "ok" : "failed");
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
	const bool ok = burn::LoadState(StatePath(slot), &why);
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
	return ControlsOwner() == LayoutOwner::General ? GeneralLayout() : g.game_layout;
}

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
	fe::FixPadPlayers(g.game_layout.players);
	g.owner = LayoutOwner::Game;
	const std::string path = BtnPath(g.set);
	const bool ok = WriteBtn(path, &g.game_layout);
	OrbisLog("[emu] button layout for %s only: %s (%s %s)", g.set.c_str(), kLayouts[l.layout].name, path.c_str(),
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
	if (!g.parent.empty() && ReadBtn(BtnPath(g.parent), nullptr) == BtnFile::Layout)
	{
		const bool ok = WriteBtn(path, nullptr);
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
	ApplyLayout();
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
	if (g.wait_release)
	{
		g.wait_release = (raw & (SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3 | SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE)) != 0;
		g.prev_p1 = raw;
	}
	const uint32_t pressed = raw & ~g.prev_p1;
	g.prev_p1 = raw;
	const uint32_t menu_combo = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3;
	if ((raw & menu_combo) == menu_combo && (pressed & menu_combo))
		return FrameResult::OpenMenu;

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
	burn::RunFrame(in, true);
	TakePicture();
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
	if (g.new_frame || (!g.osd.empty() && now < g.osd_until + 0.1) || cfg.show_fps)
	{
		g.new_frame = false;
		ps5video::Rect r = DrawLast();
		ShowOverlays(&r);
		ps5video::Present(r.x, r.y, r.w, r.h, g.vsynced);
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

	// -- NVRAM, every 5 s when it changed; the log, now and then
	if (now >= g.next_nvram_check)
	{
		g.next_nvram_check = now + 5.0;
		burn::SaveNvram(false);
	}
	if (now >= g.next_stats)
	{
		g.stats_reports++;
		g.next_stats = now + (g.stats_reports < 3 ? 5.0 : 60.0);
		const double shader_ms = ps5video::TakeShaderMs();
		OrbisLog("[emu] frame %llu, %.1f fps, audio queued %d (%.0f ms), underruns %llu, shader %.1f ms",
			(unsigned long long)g.frames, g.fps_shown, ps5audio::Queued(), ps5audio::Queued() * 1000.0 / ps5audio::kRate,
			(unsigned long long)ps5audio::Underruns(), shader_ms);
	}
	return FrameResult::Continue;
}
} // namespace emu
