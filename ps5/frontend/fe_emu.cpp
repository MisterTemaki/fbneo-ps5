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
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoThread.h"
#include "ProsperoVideo.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

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
};
const int kPs5ButtonCount = int(sizeof(kPs5Buttons) / sizeof(kPs5Buttons[0]));

const ArcadeButton kArcadeButtons[kArcadeButtonCount] = {
	{"Button 1", "btn_1"}, {"Button 2", "btn_2"}, {"Button 3", "btn_3"}, {"Button 4", "btn_4"},
	{"Button 5", "btn_5"}, {"Button 6", "btn_6"}, {"Coin", "btn_coin"}, {"Start", "btn_start"},
};

int LayoutButton(int layout, int i, int game_buttons)
{
	// indexes in kPs5Buttons: 0 Cross, 1 Circle, 2 Square, 3 Triangle, 4 L1, 5 R1, 6 OPTIONS, 7 touchpad
	// classic (FBNeo's libretro pad): buttons 1-4 = Cross, Circle, Square, Triangle (Neo Geo A B C D), 5-6 = L1 R1
	static const int kClassic[kArcadeButtonCount] = {0, 1, 2, 3, 4, 5, 7, 6};
	// fighting (Capcom's 6 buttons, punches on top): Square Triangle R1 = 1 2 3, Cross Circle L1 = 4 5 6
	static const int kFighting[kArcadeButtonCount] = {2, 3, 5, 0, 1, 4, 7, 6};
	if (i < 0 || i >= kArcadeButtonCount)
		return kPs5ButtonCount - 1;
	switch (layout)
	{
		case 1: return kClassic[i];
		case 2: return kFighting[i];
		case 3:
		{
			const int b = fe::Config().buttons[i];
			return b >= 0 && b < kPs5ButtonCount ? b : kPs5ButtonCount - 1;
		}
		default: return game_buttons >= 6 ? kFighting[i] : kClassic[i];
	}
}
} // namespace emu

namespace
{
constexpr int kTargetQueued = ps5audio::kRate * 60 / 1000; // 60 ms of sound
constexpr int kRewindEvery = 3;       // frames between rewind snapshots
constexpr int kRewindEveryBig = 6;    // ... for a state over kRewindBigRaw (CPS-3's is 10 MB)
constexpr size_t kRewindBigRaw = 2u * 1024 * 1024;
constexpr size_t kRewindBytes = 192u * 1024 * 1024;
constexpr size_t kRewindMaxState = 8u * 1024 * 1024; // a bigger snapshot, compressed: no rewind for that game
constexpr size_t kRewindMaxRaw = 64u * 1024 * 1024;

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
	bool mute = false; // fast forward, rewind: drop the sound
	double frac = 0;
	int16_t prev_l = 0, prev_r = 0;
	std::vector<int16_t> out;
	std::vector<int16_t> silence; // the zeros that refill a dry ring

	// pads: the layout in use (PS5 button bit of each arcade button)
	unsigned map[emu::kArcadeButtonCount] = {};
	uint32_t prev_p1 = 0;
	bool wait_release = true;
	bool service = false, test = false;

	// fast forward, rewind
	bool turbo = false, rewinding = false;
	int rewind_tick = 0;
	int rewind_every = kRewindEvery;
	bool rewind_logged = false;

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

// The rewind's snapshots. 1.2 compressed each one on the game's thread, every 3 frames: CPS-3's state is 10 MB, and
// compressing it took longer than a frame (the game slowed down and stuttered). Now the game's thread only copies
// the state (StateToRaw); this thread compresses it. A snapshot that comes while the last one is still being
// compressed is skipped, so the game never waits.
struct Rewinder
{
	std::mutex lock;
	std::condition_variable wake;
	std::vector<uint8_t> raw;   // waiting to be compressed
	std::vector<uint8_t> spare; // a used buffer, kept for the next copy (its capacity)
	bool busy = false;        // raw holds one, or it is being compressed
	uint64_t generation = 0;  // RewindClear: a snapshot of before that is dropped
	uint64_t raw_generation = 0;
	bool quit = false;
	bool off = false; // the state is too big for the rewind
	std::deque<std::vector<uint8_t>> snaps;
	size_t bytes = 0;
	ps5::BigThread thread;

	void Run()
	{
		std::unique_lock<std::mutex> lk(lock);
		for (;;)
		{
			wake.wait(lk, [this] { return quit || busy; });
			if (quit)
				return;
			std::vector<uint8_t> in;
			in.swap(raw);
			const uint64_t gen = raw_generation;
			lk.unlock();
			std::vector<uint8_t> out;
			const bool ok = burn::CompressState(in, &out);
			lk.lock();
			busy = false;
			spare.swap(in);
			if (!ok || gen != generation)
				continue;
			if (out.size() > kRewindMaxState)
			{
				off = true;
				OrbisLog("[emu] rewind off for this game: a snapshot is %zu KB compressed", out.size() >> 10);
				continue;
			}
			bytes += out.size();
			snaps.push_back(std::move(out));
			while (bytes > kRewindBytes && !snaps.empty())
			{
				bytes -= snaps.front().size();
				snaps.pop_front();
			}
		}
	}

	void Start()
	{
		if (!thread.joinable())
			thread = ps5::BigThread([this] { Run(); }, 256 * 1024);
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> lk(lock);
			quit = true;
		}
		wake.notify_all();
		thread.join();
		quit = false;
	}
};
Rewinder r_;

std::string StatePath(int slot)
{
	return OrbisDir("states") + "/" + g.set + ".state" + std::to_string(slot);
}

std::string DipPath()
{
	return OrbisDir("config") + "/" + g.set + ".dip";
}

// ---- pads -> the core ----------------------------------------------------------------------------------------
void ApplyLayout()
{
	const int layout = fe::Config().layout % emu::kLayoutCount;
	const int n = burn::ButtonCount();
	for (int i = 0; i < emu::kArcadeButtonCount; i++)
		g.map[i] = emu::kPs5Buttons[emu::LayoutButton(layout, i, n)].bit;
}

int8_t Axis(uint8_t v)
{
	return int8_t(int(v) - 128);
}

burn::Input ReadPads()
{
	burn::Input in;
	ps5input::Poll();
	for (int p = 0; p < burn::kMaxPlayers && p < ps5input::kMaxPads; p++)
	{
		const ps5input::PadState s = ps5input::Snapshot(p);
		if (!s.connected)
			continue;
		// player 1 while the buttons that closed a menu (or opened it) are held: nothing reaches the game; with L2
		// held (the hot keys' modifier) the D-pad and OPTIONS / touchpad stay with the hot keys
		if (p == 0 && g.wait_release)
			continue;
		const bool l2 = p == 0 && (s.raw_buttons & SCE_PAD_BUTTON_L2);
		burn::PlayerInput& pi = in.player[p];
		if (!l2)
		{
			pi.up = s.buttons & SCE_PAD_BUTTON_UP; // the D-pad, or the left stick
			pi.down = s.buttons & SCE_PAD_BUTTON_DOWN;
			pi.left = s.buttons & SCE_PAD_BUTTON_LEFT;
			pi.right = s.buttons & SCE_PAD_BUTTON_RIGHT;
		}
		for (int b = 0; b < 6; b++)
			pi.button[b] = g.map[b] && (s.raw_buttons & g.map[b]);
		if (!l2)
		{
			pi.coin = g.map[6] && (s.raw_buttons & g.map[6]);
			pi.start = g.map[7] && (s.raw_buttons & g.map[7]);
		}
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

void RewindPush()
{
	{
		std::lock_guard<std::mutex> lk(r_.lock);
		if (r_.off || r_.busy)
			return; // too big, or the last one is still being compressed: skip this one
	}
	std::vector<uint8_t> raw;
	{
		std::lock_guard<std::mutex> lk(r_.lock);
		raw.swap(r_.spare);
	}
	const double t0 = Now();
	if (!burn::StateToRaw(&raw))
		return;
	if (!g.rewind_logged)
		OrbisLog("[emu] rewind: a snapshot is %zu KB, copied in %.1f ms (compressed on another thread)", raw.size() >> 10,
			(Now() - t0) * 1000.0);
	g.rewind_logged = true;
	g.rewind_every = raw.size() > kRewindBigRaw ? kRewindEveryBig : kRewindEvery;
	r_.Start();
	std::lock_guard<std::mutex> lk(r_.lock);
	if (raw.size() > kRewindMaxRaw)
	{
		r_.off = true;
		OrbisLog("[emu] rewind off for this game: its state is %zu MB", raw.size() >> 20);
		return;
	}
	r_.raw.swap(raw);
	r_.raw_generation = r_.generation;
	r_.busy = true;
	r_.wake.notify_one();
}

void RewindClear()
{
	std::lock_guard<std::mutex> lk(r_.lock);
	r_.generation++;
	r_.snaps.clear();
	r_.bytes = 0;
	g.rewind_tick = 0;
}

// The newest snapshot (removed), or false when there is none.
bool RewindPop(std::vector<uint8_t>* out)
{
	std::lock_guard<std::mutex> lk(r_.lock);
	if (r_.snaps.empty())
		return false;
	out->swap(r_.snaps.back());
	r_.bytes -= out->size();
	r_.snaps.pop_back();
	return true;
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
	r_.Stop();
	burn::Exit();
	g.inited = false;
}

void ApplySettings()
{
	const fe::Settings& s = fe::Config();
	burn::SetHiscores(s.hiscores);
	if (!s.rewind)
		RewindClear();
	if (g.loaded)
		ApplyLayout();
}

bool LoadGame(const std::string& path, std::string* error)
{
	if (!g.inited)
		return false;
	FBNEO_STAGE(Emu, "load game");
	CloseGame();
	// "<dir>/<zip>#<set>": a clone kept inside its parent's zip (a merged set); FBNeo finds its ROMs in the parent
	const size_t hash = path.find('#', path.find_last_of('/') + 1);
	const std::string zip = hash == std::string::npos ? path : path.substr(0, hash);
	std::string file = zip.substr(zip.find_last_of('/') + 1);
	std::string set = hash == std::string::npos ? file : path.substr(hash + 1);
	if (set.size() > 4 && strcasecmp(set.c_str() + set.size() - 4, ".zip") == 0)
		set = set.substr(0, set.size() - 4);
	for (char& c : set)
		c = char(tolower(uint8_t(c)));
	const int drv = burn::FindDriver(set);
	if (drv < 0)
	{
		OrbisLog("[emu] could not start %s: no FBNeo driver is called %s", path.c_str(), set.c_str());
		if (error)
			*error = file + " is not a ROM set FBNeo knows (the zip must keep its set name, as in FBNeo's lists).";
		return false;
	}
	const std::string dir = zip.substr(0, zip.find_last_of('/'));
	auto find = [&](const std::string& name) {
		if (name == set && hash == std::string::npos)
			return zip;
		std::string p = fe::FindSetZip(name);
		if (p.empty())
		{
			// not scanned (a game started from the command line): beside the game
			const std::string beside = dir + "/" + name + ".zip";
			if (FileExists(beside))
				p = beside;
		}
		return p;
	};
	ApplySettings();
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
	g.turbo = g.rewinding = false;
	g.rewind_every = kRewindEvery;
	g.rewind_logged = false;
	{
		std::lock_guard<std::mutex> lk(r_.lock);
		r_.off = false;
	}
	g.frac = 0;
	g.prev_l = g.prev_r = 0;
	g.frames = 0;
	g.fps_frames = 0;
	g.fps_since = Now();
	g.next_stats = Now() + 1.0;
	g.stats_reports = 0;
	g.next_nvram_check = Now() + 5.0;
	g.overlay_was_drawn = false;
	RewindClear();
	ApplyLayout();
	ps5video::InvalidateFrame();
	OrbisLog("[emu] running %s \"%s\" (%s), %.3f fps (%s), aspect %.4f, %d button(s)", g.set.c_str(), g.title.c_str(),
		g.board.c_str(), g.fps, g.vsynced ? "at 60 Hz, vsync" : "paced by the sound", burn::Aspect(), burn::ButtonCount());
	return true;
}

void CloseGame()
{
	if (!g.loaded)
		return;
	FBNEO_STAGE(Emu, "close game");
	burn::Unload(); // writes the NVRAM (and the core its EEPROM and high scores)
	g.loaded = false;
	g.frame.clear();
	RewindClear();
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
	g.osd = text;
	g.osd_until = Now() + 2.5;
}

bool SaveState(int slot)
{
	if (!g.loaded)
		return false;
	const bool ok = burn::SaveState(StatePath(slot));
	OrbisLog("[emu] save state %d: %s", slot, ok ? "ok" : "failed");
	Osd(ok ? "State saved to slot " + std::to_string(slot) : "Could not save the state");
	return ok;
}

bool LoadState(int slot)
{
	if (!g.loaded)
		return false;
	if (!FileExists(StatePath(slot)))
	{
		Osd("Slot " + std::to_string(slot) + " is empty");
		return false;
	}
	std::string why;
	const bool ok = burn::LoadState(StatePath(slot), &why);
	OrbisLog("[emu] load state %d: %s%s%s", slot, ok ? "ok" : "failed", ok ? "" : ": ", why.c_str());
	Osd(ok ? "State loaded from slot " + std::to_string(slot) : "Could not load the state: " + why);
	if (ok)
		RewindClear();
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
	RewindClear();
	OrbisLog("[emu] reset");
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
		ProsperoNotify("FBNeo PS5: the game could not be started again. %s", error.c_str());
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

std::string GameButtonName(int i)
{
	return g.loaded ? burn::ButtonName(i) : "";
}

int GameButtonCount()
{
	return g.loaded ? burn::ButtonCount() : 0;
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

	// -- hot keys (player 1)
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

	const bool l2 = (raw & SCE_PAD_BUTTON_L2) != 0;
	const bool r2 = (raw & SCE_PAD_BUTTON_R2) != 0;
	if (l2 && !r2)
	{
		if (pressed & SCE_PAD_BUTTON_UP)
			SaveState(cfg.state_slot);
		else if (pressed & SCE_PAD_BUTTON_DOWN)
			LoadState(cfg.state_slot);
		else if (pressed & (SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT))
		{
			cfg.state_slot = (pressed & SCE_PAD_BUTTON_RIGHT) ? cfg.state_slot % 10 + 1 : (cfg.state_slot + 8) % 10 + 1;
			Osd("State slot " + std::to_string(cfg.state_slot) + (StateExists(cfg.state_slot) ? " (used)" : " (empty)"));
			OrbisLog("[emu] state slot %d", cfg.state_slot);
			cfg.Save();
		}
	}
	// the machine's service and test buttons: L2 + OPTIONS, L2 + touchpad (held)
	const bool service = l2 && (raw & SCE_PAD_BUTTON_OPTIONS);
	const bool test = l2 && (raw & SCE_PAD_BUTTON_TOUCH_PAD);
	if (service != g.service || test != g.test)
		OrbisLog("[emu] service %s, test %s", service ? "on" : "off", test ? "on" : "off");
	g.service = service;
	g.test = test;
	bool rewind_off;
	size_t rewind_snaps, rewind_bytes;
	{
		std::lock_guard<std::mutex> lk(r_.lock);
		rewind_off = r_.off;
		rewind_snaps = r_.snaps.size();
		rewind_bytes = r_.bytes;
	}
	const bool want_rewind = cfg.rewind && l2 && r2 && !rewind_off;
	if (want_rewind != g.rewinding)
	{
		g.rewinding = want_rewind;
		OrbisLog("[emu] rewind %s (%zu snapshots, %zu MB)", want_rewind ? "on" : "off", rewind_snaps,
			rewind_bytes >> 20);
	}
	const bool want_turbo = r2 && !l2;
	if (want_turbo != g.turbo)
	{
		g.turbo = want_turbo;
		OrbisLog("[emu] fast forward %s", want_turbo ? "on" : "off");
	}

	// -- emulate
	FBNEO_STAGE(Emu, "frame");
	const burn::Input in = ReadPads();
	if (g.rewinding)
	{
		std::vector<uint8_t> snap;
		if (RewindPop(&snap))
			burn::StateFromMemory(snap);
		g.mute = true;
		burn::RunFrame(burn::Input(), true);
		TakePicture();
		g.mute = false;
	}
	else
	{
		const int runs = g.turbo ? (cfg.ff_speed == 0 ? 8 : std::max(2, (cfg.ff_speed + 50) / 100)) : 1;
		for (int i = 0; i < runs; i++)
		{
			const bool last = i == runs - 1;
			g.mute = g.turbo;
			burn::RunFrame(in, last); // fast forward: only the last frame of the batch is drawn
			if (last)
				TakePicture();
			FlushAudio();
			g.frames++;
			g.fps_frames++;
			if (cfg.rewind && ++g.rewind_tick >= g.rewind_every)
			{
				g.rewind_tick = 0;
				RewindPush();
			}
		}
		g.mute = false;
	}
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
		ps5video::Present(r.x, r.y, r.w, r.h, g.vsynced && !g.turbo);
	}

	// -- pacing on the sound: the games not at 60 Hz, or a ring filling up
	if (!g.turbo && !g.rewinding)
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
