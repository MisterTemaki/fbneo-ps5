// FBNeo PS5: FBNeo's core behind a small API (fe_burn.h).
//
// What it does for the core is what FBNeo's frontends do (src/burner: bzip.cpp for the ROMs, gami.cpp and
// inpdipsw.cpp for the inputs and DIP switches, state.cpp for the saves), written again for this port.
//
// SPDX-License-Identifier: MIT

#include "fe_burn.h"

#include "burnint.h"
#include "unzip.h"
#include "version.h"

extern int bDrvOkay; // core/burn_frontend.cpp (cheat.cpp reads it)

#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

// ---- the ROM-set zips of the game being started -------------------------------------------------------------
struct ZipEntry
{
	std::string name; // as in the zip
	std::string lower; // without any folder part, lower case
	uint32_t crc = 0, size = 0;
};
struct Zip
{
	std::string set, path;
	unzFile uf = nullptr;
	std::vector<ZipEntry> entries;
};
struct RomLoc
{
	int zip = -1, entry = -1;
};

// One input of the driver (BurnDrvGetInputInfo) and what the pad gives it.
enum class Kind
{
	None, Up, Down, Left, Right, Button, Coin, Start, AxisX, AxisY, AxisZ, Up2, Down2, Left2, Right2,
	Service, Test, Reset, Tilt, Dip
};
struct In
{
	uint8_t type = 0; // BIT_*
	uint8_t* val = nullptr;
	uint16_t* sval = nullptr;
	Kind kind = Kind::None;
	int player = 0; // 0..3
	int button = 0; // 0..7 for Kind::Button
	std::string name, info;
};

struct DipOption
{
	int index; // in BurnDrvGetDIPInfo
	std::string name;
};
struct DipGroupInt
{
	std::string name;
	std::vector<DipOption> options;
	int def = 0;
};

struct G
{
	bool inited = false, loaded = false;
	burn::Paths paths;
	burn::Driver cur;
	void (*log)(const char*) = nullptr;
	std::vector<Zip> zips;
	std::vector<RomLoc> roms;

	// video: what the driver draws (pBurnDraw, nBurnBpp bytes a pixel), and the picture turned the right way up
	int bpp = 4;
	std::vector<uint8_t> draw;
	int draw_w = 0, draw_h = 0;
	std::vector<uint32_t> pic;
	int pic_w = 0, pic_h = 0;
	bool drawing = false; // inside a frame that is drawn (pBurnDraw set)

	// sound: 48 kHz stereo, a frame of it
	std::vector<int16_t> snd;
	int snd_frames = 0;

	// inputs and DIP switches (dip_value: what each DIP input holds, written before every frame)
	std::vector<In> inputs;
	std::vector<uint8_t> dip_value;
	int dip_offset = 0;
	std::vector<DipGroupInt> dips;
	int buttons = 0;
	bool analog = false;
	int reset_frames = 0;

	// the NVRAM areas as last written (or read)
	std::vector<uint8_t> nvram_last;
};
G g;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	size_t n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	if (!n)
		return;
	if (g.log)
		g.log(buf);
	else
		fprintf(stderr, "%s\n", buf);
}

// ---- the core's callbacks -----------------------------------------------------------------------------------
INT32 __cdecl CoreLog(INT32 status, TCHAR* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	// the core's messages are many and short (one per ROM, per chip): only the important ones, errors and warnings
	if (status == PRINT_NORMAL || status == PRINT_UI)
		return 0;
	Log("[core] %s", buf);
	return 0;
}

UINT32 __cdecl HighCol32(INT32 r, INT32 g_, INT32 b, INT32)
{
	return 0xff000000u | (UINT32(r & 0xff) << 16) | (UINT32(g_ & 0xff) << 8) | UINT32(b & 0xff);
}

UINT32 __cdecl HighCol16(INT32 r, INT32 g_, INT32 b, INT32)
{
	return (UINT32(r & 0xf8) << 8) | (UINT32(g_ & 0xfc) << 3) | UINT32((b & 0xf8) >> 3);
}

// Reads up to `cap` bytes of entry e of zip z into dest (straight in: no copy of the file, and a zip that claims a
// huge size can't make it allocate anything). *got: the bytes read.
bool ReadEntry(int z, int e, uint8_t* dest, size_t cap, size_t* got)
{
	*got = 0;
	Zip& zip = g.zips[size_t(z)];
	const ZipEntry& en = zip.entries[size_t(e)];
	if (unzLocateFile(zip.uf, en.name.c_str(), 1) != UNZ_OK || unzOpenCurrentFile(zip.uf) != UNZ_OK)
	{
		Log("[burn] %s: can't open %s in it (a damaged zip?)", zip.path.c_str(), en.name.c_str());
		return false;
	}
	const size_t want = std::min(size_t(en.size), cap);
	bool ok = true;
	while (ok && *got < want)
	{
		const unsigned chunk = unsigned(std::min(want - *got, size_t(1) << 30));
		const int n = unzReadCurrentFile(zip.uf, dest + *got, chunk);
		ok = n > 0;
		if (ok)
			*got += size_t(n);
	}
	// the zip checks the CRC when the whole file was read: a damaged ROM shows up here
	const int rc = unzCloseCurrentFile(zip.uf);
	ok = ok && (rc == UNZ_OK || (rc == UNZ_CRCERROR && want < en.size));
	if (!ok)
		Log("[burn] %s: can't read %s (a damaged zip? %zu of %zu bytes, %d)", zip.path.c_str(), en.name.c_str(), *got,
			want, rc);
	return ok;
}

// The core asks for ROM i of the driver (load.cpp): Dest has room for the ROM's length (BurnDrvGetRomInfo).
INT32 __cdecl LoadRom(UINT8* dest, INT32* wrote, INT32 i)
{
	if (wrote)
		*wrote = 0;
	struct BurnRomInfo ri = {};
	if (i < 0 || size_t(i) >= g.roms.size() || BurnDrvGetRomInfo(&ri, i) != 0)
		return 1;
	const RomLoc loc = g.roms[size_t(i)];
	if (loc.zip < 0)
	{
		char* name = nullptr;
		BurnDrvGetRomName(&name, i, 0);
		Log("[burn] ROM %d (%s) asked for, but it is not in the zips", i, name ? name : "?");
		return 1;
	}
	size_t n = 0;
	if (!ReadEntry(loc.zip, loc.entry, dest, size_t(ri.nLen), &n))
		return 1;
	if (wrote)
		*wrote = INT32(n);
	return 0;
}

void CloseZips()
{
	for (Zip& z : g.zips)
		if (z.uf)
			unzClose(z.uf);
	g.zips.clear();
	g.roms.clear();
}

bool OpenZip(Zip* z)
{
	z->uf = unzOpen(z->path.c_str());
	if (!z->uf)
	{
		Log("[burn] can't open %s as a zip", z->path.c_str());
		return false;
	}
	if (unzGoToFirstFile(z->uf) != UNZ_OK)
		return true; // an empty zip
	do
	{
		unz_file_info info = {};
		char name[512] = "";
		if (unzGetCurrentFileInfo(z->uf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
			continue;
		name[sizeof(name) - 1] = 0;
		ZipEntry e;
		e.name = name;
		const char* base = strrchr(name, '/');
		e.lower = Lower(base ? base + 1 : name);
		e.crc = uint32_t(info.crc);
		e.size = uint32_t(info.uncompressed_size);
		if (!e.lower.empty())
			z->entries.push_back(e);
	} while (unzGoToNextFile(z->uf) == UNZ_OK);
	return true;
}

// The driver's zips, in the core's order: the set, then its BIOS/board set, then its parents (BurnDrvGetZipName).
void OpenZips(const burn::ZipFinder& find, burn::RomCheck* check)
{
	CloseZips();
	for (UINT32 j = 0; j < 8; j++)
	{
		char* name = nullptr;
		if (BurnDrvGetZipName(&name, j) != 0 || !name || !*name)
			break;
		Zip z;
		z.set = name;
		check->sets.push_back(z.set);
		z.path = find(z.set);
		if (z.path.empty() || !OpenZip(&z))
			continue;
		g.zips.push_back(std::move(z));
	}
}

// Where each ROM of the driver is: by CRC (any name), then by any of its names (CRC differs: used anyway).
void FindRoms(burn::RomCheck* check)
{
	check->missing.clear();
	check->bad_crc.clear();
	g.roms.clear();
	for (UINT32 i = 0; i < 1024; i++)
	{
		struct BurnRomInfo ri = {};
		if (BurnDrvGetRomInfo(&ri, i) != 0)
			break;
		RomLoc loc;
		if (ri.nType != 0 && ri.nLen != 0)
		{
			if (ri.nCrc != 0)
				for (size_t z = 0; z < g.zips.size() && loc.zip < 0; z++)
					for (size_t e = 0; e < g.zips[z].entries.size(); e++)
						if (g.zips[z].entries[e].crc == ri.nCrc)
						{
							loc = RomLoc{int(z), int(e)};
							break;
						}
			char* first = nullptr;
			BurnDrvGetRomName(&first, i, 0);
			for (INT32 aka = 0; aka < 16 && loc.zip < 0; aka++)
			{
				char* name = nullptr;
				if (BurnDrvGetRomName(&name, i, aka) != 0 || !name || !*name)
					break;
				const std::string want = Lower(name);
				for (size_t z = 0; z < g.zips.size() && loc.zip < 0; z++)
					for (size_t e = 0; e < g.zips[z].entries.size(); e++)
						if (g.zips[z].entries[e].lower == want)
						{
							loc = RomLoc{int(z), int(e)};
							if (!(ri.nType & BRF_NODUMP) && ri.nCrc != 0)
								check->bad_crc.push_back(g.zips[z].set + ".zip: " + name);
							break;
						}
			}
			const bool needed = !(ri.nType & (BRF_OPT | BRF_NODUMP));
			if (loc.zip < 0 && needed)
				check->missing.push_back(std::string(first ? first : "?"));
		}
		g.roms.push_back(loc);
	}
	check->ok = check->missing.empty();
}

// ---- inputs --------------------------------------------------------------------------------------------------
Kind KindOf(const std::string& info, int* player, int* button)
{
	*player = 0;
	*button = 0;
	std::string s = info;
	if (s.size() > 3 && s[0] == 'p' && s[1] >= '1' && s[1] <= '4' && s[2] == ' ')
	{
		*player = s[1] - '1';
		s = s.substr(3);
	}
	else if (s.compare(0, 6, "mouse ") == 0)
		s = s.substr(6);
	if (s == "up" || s == "up_alt") return Kind::Up;
	if (s == "down" || s == "down_alt") return Kind::Down;
	if (s == "left") return Kind::Left;
	if (s == "right") return Kind::Right;
	if (s == "up 2") return Kind::Up2;
	if (s == "down 2") return Kind::Down2;
	if (s == "left 2") return Kind::Left2;
	if (s == "right 2") return Kind::Right2;
	if (s == "coin" || s == "coin 2") return Kind::Coin;
	if (s == "start") return Kind::Start;
	if (s.compare(0, 5, "fire ") == 0)
	{
		const int n = atoi(s.c_str() + 5);
		if (n >= 1 && n <= burn::kButtons)
		{
			*button = n - 1;
			return Kind::Button;
		}
		return Kind::None;
	}
	if (s.compare(0, 7, "button ") == 0) // "mouse button 1"
	{
		const int n = atoi(s.c_str() + 7);
		if (n >= 1 && n <= burn::kButtons)
		{
			*button = n - 1;
			return Kind::Button;
		}
		return Kind::None;
	}
	if (s == "x-axis") return Kind::AxisX;
	if (s == "y-axis") return Kind::AxisY;
	if (s == "z-axis") return Kind::AxisZ;
	if (s == "service" || s == "service1" || s == "service 1") return Kind::Service;
	if (s == "diag" || s == "test") return Kind::Test;
	if (s == "reset") return Kind::Reset;
	if (s == "tilt") return Kind::Tilt;
	return Kind::None;
}

void ScanInputs()
{
	g.inputs.clear();
	g.buttons = 0;
	g.analog = false;
	for (UINT32 i = 0; i < 1024; i++)
	{
		struct BurnInputInfo bii = {};
		if (BurnDrvGetInputInfo(&bii, i) != 0)
			break;
		In in;
		in.type = bii.nType;
		in.val = bii.pVal;
		in.sval = bii.pShortVal;
		in.name = bii.szName ? bii.szName : "";
		in.info = Lower(bii.szInfo ? bii.szInfo : "");
		if (bii.nType == BIT_DIPSWITCH)
			in.kind = Kind::Dip;
		else
			in.kind = KindOf(in.info, &in.player, &in.button);
		if (in.kind == Kind::Button && in.player == 0)
			g.buttons = std::max(g.buttons, in.button + 1);
		if ((in.type & BIT_GROUP_ANALOG) && (in.kind == Kind::AxisX || in.kind == Kind::AxisY || in.kind == Kind::AxisZ))
			g.analog = true;
		g.inputs.push_back(in);
	}
	g.dip_value.assign(g.inputs.size(), 0);
	for (size_t i = 0; i < g.inputs.size(); i++)
		if (g.inputs[i].kind == Kind::Dip && g.inputs[i].val)
			g.dip_value[i] = *g.inputs[i].val;
}

int16_t Clamp16(int v)
{
	return int16_t(std::max(-32768, std::min(32767, v)));
}

void ApplyInputs(const burn::Input& in)
{
	for (size_t i = 0; i < g.inputs.size(); i++)
	{
		const In& x = g.inputs[i];
		if (x.kind == Kind::Dip)
		{
			if (x.val)
				*x.val = g.dip_value[i];
			continue;
		}
		if (x.type == BIT_CONSTANT || !x.val)
			continue;
		const burn::PlayerInput& p = in.player[x.player];
		bool on = false;
		int axis = 0; // -128..127
		switch (x.kind)
		{
			case Kind::Up: on = p.up; break;
			case Kind::Down: on = p.down; break;
			case Kind::Left: on = p.left; break;
			case Kind::Right: on = p.right; break;
			case Kind::Up2: on = p.stick2_y < -64; break;
			case Kind::Down2: on = p.stick2_y > 64; break;
			case Kind::Left2: on = p.stick2_x < -64; break;
			case Kind::Right2: on = p.stick2_x > 64; break;
			case Kind::Button: on = p.button[x.button]; break;
			case Kind::Coin: on = p.coin; break;
			case Kind::Start: on = p.start; break;
			case Kind::Service: on = in.service; break;
			case Kind::Test: on = in.test; break;
			case Kind::Reset: on = in.reset || g.reset_frames > 0; break;
			case Kind::Tilt: on = in.tilt; break;
			case Kind::AxisX: axis = p.stick_x; break;
			case Kind::AxisY: axis = p.stick_y; break;
			case Kind::AxisZ: axis = p.stick2_y; break;
			default: break;
		}
		if (x.type & BIT_GROUP_ANALOG)
		{
			if (!x.sval)
				continue;
			if (x.kind == Kind::AxisX || x.kind == Kind::AxisY || x.kind == Kind::AxisZ)
			{
				// as FBNeo's frontends give a pad's stick: relative (trackball, dial) about +-0x3ff a frame at full
				// tilt, absolute (wheel, gun) as ProcessAnalog() reads it, +-0x800 from the centre
				if (axis > -12 && axis < 12)
					axis = 0; // the stick's dead zone
				*x.sval = uint16_t(Clamp16(x.type == BIT_ANALOG_REL ? axis * 8 : axis * 16));
			}
			else
				*x.sval = on ? 0xffff : 0x0001; // a digital button on an analog input (FBNeo's frontends do the same)
		}
		else
			*x.val = on ? 1 : 0;
	}
}

// ---- DIP switches (inpdipsw.cpp's rules) ---------------------------------------------------------------------
void WriteDips()
{
	for (size_t i = 0; i < g.inputs.size(); i++)
		if (g.inputs[i].kind == Kind::Dip && g.inputs[i].val)
			*g.inputs[i].val = g.dip_value[i];
}

void DipOffset()
{
	g.dip_offset = 0;
	struct BurnDIPInfo bdi = {};
	for (UINT32 i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
		if (bdi.nFlags == 0xF0)
		{
			g.dip_offset = bdi.nInput;
			break;
		}
}

uint8_t* DipSlot(int input)
{
	const int k = input + g.dip_offset;
	return k >= 0 && size_t(k) < g.dip_value.size() ? &g.dip_value[size_t(k)] : nullptr;
}

bool DipIsSet(UINT32 i)
{
	struct BurnDIPInfo bdi = {};
	if (BurnDrvGetDIPInfo(&bdi, i) != 0)
		return false;
	const uint8_t* v = DipSlot(bdi.nInput);
	if (!v || (*v & bdi.nMask) != bdi.nSetting)
		return false;
	const UINT8 flags = bdi.nFlags;
	for (UINT32 j = 1; j < UINT32(flags & 0x0f); j++)
	{
		if (BurnDrvGetDIPInfo(&bdi, i + j) != 0)
			return false;
		const uint8_t* w = DipSlot(bdi.nInput);
		if (!w)
			return false;
		const bool eq = (*w & bdi.nMask) == bdi.nSetting;
		if ((flags & 0x80) ? eq : !eq)
			return false;
	}
	return true;
}

void ScanDips()
{
	g.dips.clear();
	DipOffset();
	struct BurnDIPInfo bdi = {};
	for (UINT32 i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
	{
		if (bdi.nFlags >= 0xFF || !bdi.szText)
			continue;
		if (bdi.nFlags & 0x40)
		{
			DipGroupInt d;
			d.name = bdi.szText;
			g.dips.push_back(d);
		}
		else if (!g.dips.empty() && g.dips.back().options.size() < 64)
			g.dips.back().options.push_back(DipOption{int(i), bdi.szText});
	}
	g.dips.erase(std::remove_if(g.dips.begin(), g.dips.end(), [](const DipGroupInt& d) { return d.options.empty(); }),
		g.dips.end());
}

void DefaultDips()
{
	DipOffset();
	struct BurnDIPInfo bdi = {};
	for (UINT32 i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
		if (bdi.nFlags == 0xFF)
			if (uint8_t* v = DipSlot(bdi.nInput))
				*v = uint8_t((*v & ~bdi.nMask) | (bdi.nSetting & bdi.nMask));
	for (DipGroupInt& d : g.dips)
	{
		d.def = 0;
		for (size_t o = 0; o < d.options.size(); o++)
			if (DipIsSet(UINT32(d.options[o].index)))
			{
				d.def = int(o);
				break;
			}
	}
}

// ---- video ---------------------------------------------------------------------------------------------------
void SizeDrawBuffer()
{
	INT32 w = 0, h = 0;
	BurnDrvGetFullSize(&w, &h);
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
		w = h = 0;
	if (w != g.draw_w || h != g.draw_h)
	{
		g.draw_w = w;
		g.draw_h = h;
		// room to spare (two rows, and never under 1024x1024): a few drivers write a pixel past their last line,
		// and one that changes its size in the middle of a frame draws the rest of it before we can follow
		g.draw.assign(std::max(size_t(w) * (h + 2), size_t(1024) * 1024) * size_t(g.bpp), 0);
	}
	pBurnDraw = g.draw.empty() || !g.drawing ? nullptr : g.draw.data();
	nBurnPitch = g.draw_w * g.bpp;
	nBurnBpp = g.bpp;
}

inline uint32_t Px(int x, int y)
{
	if (g.bpp == 4)
		return reinterpret_cast<const uint32_t*>(g.draw.data())[size_t(y) * g.draw_w + x] | 0xff000000u;
	const uint16_t c = reinterpret_cast<const uint16_t*>(g.draw.data())[size_t(y) * g.draw_w + x];
	const uint32_t r = (c >> 11) & 31, gg = (c >> 5) & 63, b = c & 31;
	return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((gg << 2) | (gg >> 4)) << 8) | ((b << 3) | (b >> 2));
}

// The driver's picture, turned as FBNeo's SDL frontend shows it: vertical games 90 degrees counter-clockwise
// (clockwise when also flipped), flipped horizontal games 180 degrees.
void MakePicture()
{
	const int w = g.draw_w, h = g.draw_h;
	if (!w || !h)
		return;
	const uint32_t flags = g.cur.flags;
	const bool vertical = flags & BDF_ORIENTATION_VERTICAL;
	const bool flipped = flags & BDF_ORIENTATION_FLIPPED;
	g.pic_w = vertical ? h : w;
	g.pic_h = vertical ? w : h;
	g.pic.resize(size_t(w) * h);
	uint32_t* out = g.pic.data();
	if (!vertical && !flipped)
	{
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				out[size_t(y) * w + x] = Px(x, y);
	}
	else if (!vertical)
	{
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				out[size_t(h - 1 - y) * w + (w - 1 - x)] = Px(x, y);
	}
	else if (!flipped)
	{
		// counter-clockwise: (x, y) -> (y, w - 1 - x) in an h-wide picture
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				out[size_t(w - 1 - x) * h + y] = Px(x, y);
	}
	else
	{
		// clockwise: (x, y) -> (h - 1 - y, x)
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				out[size_t(x) * h + (h - 1 - y)] = Px(x, y);
	}
}

// ---- sound ---------------------------------------------------------------------------------------------------
constexpr int kRate = 48000;

void SizeSound()
{
	const int fps = nBurnFPS > 100 ? nBurnFPS : 6000;
	nBurnSoundRate = kRate;
	nBurnSoundLen = (kRate * 100 + fps / 2) / fps;
	const size_t need = size_t(nBurnSoundLen + 16) * 2;
	if (g.snd.size() < need)
		g.snd.assign(need, 0);
	pBurnSoundOut = g.snd.data();
}

// ---- saves ---------------------------------------------------------------------------------------------------
// The areas of a scan: their lengths (the layout) and their bytes, in the scan's order.
struct Scan
{
	std::vector<uint32_t> lens;
	std::vector<uint8_t> data;
	const uint8_t* src = nullptr; // writing back: the next bytes to give
	size_t left = 0;
	bool short_data = false;
};
Scan* s_scan = nullptr;

INT32 __cdecl ReadAcb(struct BurnArea* pba)
{
	s_scan->lens.push_back(pba->nLen);
	const uint8_t* p = static_cast<const uint8_t*>(pba->Data);
	if (pba->nLen && p)
		s_scan->data.insert(s_scan->data.end(), p, p + pba->nLen);
	else if (pba->nLen)
		s_scan->data.insert(s_scan->data.end(), pba->nLen, 0);
	return 0;
}

INT32 __cdecl LenAcb(struct BurnArea* pba)
{
	s_scan->lens.push_back(pba->nLen);
	return 0;
}

INT32 __cdecl WriteAcb(struct BurnArea* pba)
{
	if (pba->nLen > s_scan->left)
	{
		s_scan->short_data = true;
		return 0;
	}
	if (pba->Data && pba->nLen)
		memcpy(pba->Data, s_scan->src, pba->nLen);
	s_scan->src += pba->nLen;
	s_scan->left -= pba->nLen;
	return 0;
}

void DoScan(INT32 action, INT32 (__cdecl* acb)(struct BurnArea*), Scan* s)
{
	s_scan = s;
	BurnAcb = acb;
	INT32 min = 0;
	BurnAreaScan(action, &min);
	s_scan = nullptr;
}

// File: "FBNPS5" + kind ('S' state / 'N' NVRAM) + version '1', the set's name (32 bytes), the frame number, the
// number of areas, their lengths, then their bytes; all of it but the 8-byte magic zlib-compressed.
constexpr char kMagicState[8] = {'F', 'B', 'N', 'P', 'S', '5', 'S', '1'};
constexpr char kMagicNvram[8] = {'F', 'B', 'N', 'P', 'S', '5', 'N', '1'};

void Put32(std::vector<uint8_t>* v, uint32_t x)
{
	for (int i = 0; i < 4; i++)
		v->push_back(uint8_t(x >> (8 * i)));
}

uint32_t Get32(const uint8_t* p)
{
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool Pack(const char* magic, const Scan& s, std::vector<uint8_t>* out)
{
	std::vector<uint8_t> raw;
	char set[32] = {};
	strncpy(set, g.cur.name.c_str(), sizeof(set) - 1);
	raw.insert(raw.end(), set, set + sizeof(set));
	Put32(&raw, nCurrentFrame);
	Put32(&raw, uint32_t(s.lens.size()));
	for (uint32_t l : s.lens)
		Put32(&raw, l);
	raw.insert(raw.end(), s.data.begin(), s.data.end());
	uLongf zlen = compressBound(uLong(raw.size()));
	out->assign(8 + 4 + zlen, 0);
	memcpy(out->data(), magic, 8);
	const uint32_t rawlen = uint32_t(raw.size());
	for (int i = 0; i < 4; i++)
		(*out)[8 + size_t(i)] = uint8_t(rawlen >> (8 * i));
	if (compress2(out->data() + 12, &zlen, raw.data(), uLong(raw.size()), 3) != Z_OK)
		return false;
	out->resize(12 + zlen);
	return true;
}

// What Pack made -> the frame number, the layout and the bytes (checked against the set and the running layout).
bool Unpack(const char* magic, const std::vector<uint8_t>& in, const Scan& now, uint32_t* frame, std::vector<uint8_t>* data,
	std::string* error)
{
	if (in.size() < 12 || memcmp(in.data(), magic, 8) != 0)
	{
		*error = "not a FBNeo PS5 save of this kind";
		return false;
	}
	const uint32_t rawlen = Get32(in.data() + 8);
	if (rawlen < 40 || rawlen > 512u * 1024 * 1024)
	{
		*error = "a damaged save";
		return false;
	}
	std::vector<uint8_t> raw(rawlen);
	uLongf got = rawlen;
	if (uncompress(raw.data(), &got, in.data() + 12, uLong(in.size() - 12)) != Z_OK || got != rawlen)
	{
		*error = "a damaged save";
		return false;
	}
	char set[33] = {};
	memcpy(set, raw.data(), 32);
	if (g.cur.name != set)
	{
		*error = std::string("a save of another game (") + set + ")";
		return false;
	}
	*frame = Get32(raw.data() + 32);
	const uint32_t count = Get32(raw.data() + 36);
	if (count != now.lens.size() || 40 + size_t(count) * 4 > raw.size())
	{
		*error = "made by another version of the game's driver";
		return false;
	}
	size_t total = 0;
	for (uint32_t i = 0; i < count; i++)
	{
		const uint32_t l = Get32(raw.data() + 40 + size_t(i) * 4);
		if (l != now.lens[i])
		{
			*error = "made by another version of the game's driver";
			return false;
		}
		total += l;
	}
	const size_t off = 40 + size_t(count) * 4;
	if (raw.size() - off != total)
	{
		*error = "a damaged save";
		return false;
	}
	data->assign(raw.begin() + long(off), raw.end());
	return true;
}

bool ReadAll(const std::string& path, std::vector<uint8_t>* out)
{
	// a state or NVRAM file: never more than this (a stray big file in states/ must not take all the memory)
	constexpr long kMax = 256L * 1024 * 1024;
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > kMax)
		return false;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out->clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out->insert(out->end(), buf, buf + n);
	const bool ok = !ferror(f);
	fclose(f);
	return ok;
}

// path.part, flushed and closed without error, then renamed over path
bool WriteAtomic(const std::string& path, const std::vector<uint8_t>& data)
{
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

std::string NvramPath()
{
	return g.paths.saves + "/" + g.cur.name + ".nvram";
}

void LoadNvram()
{
	Scan now;
	DoScan(ACB_NVRAM | ACB_READ, ReadAcb, &now);
	g.nvram_last = now.data;
	if (now.lens.empty())
		return;
	std::vector<uint8_t> file, data;
	if (!ReadAll(NvramPath(), &file))
		return;
	uint32_t frame = 0;
	std::string error;
	if (!Unpack(kMagicNvram, file, now, &frame, &data, &error))
	{
		Log("[burn] NVRAM %s not used: %s", NvramPath().c_str(), error.c_str());
		return;
	}
	Scan w;
	w.src = data.data();
	w.left = data.size();
	DoScan(ACB_NVRAM | ACB_WRITE, WriteAcb, &w);
	g.nvram_last = data;
	Log("[burn] NVRAM loaded: %s (%zu bytes)", NvramPath().c_str(), data.size());
}
} // namespace

// ---- a driver changed its picture's size (burn_frontend.cpp's ReinitialiseVideo): follow it at once ----------
void FeBurnReinitialiseVideo()
{
	if (g.loaded || bDrvOkay)
	{
		SizeDrawBuffer();
		BurnRecalcPal(); // as FBNeo's VidReinit does (SetBurnHighCol)
	}
}

// ---- the core's ZipLoadOneFile (samples.cpp): one file of <arcName>.zip, by name; *Dest malloc'd when NULL ------
INT32 __cdecl ZipLoadOneFile(char* arcName, const char* fileName, void** Dest, INT32* pnWrote)
{
	if (pnWrote)
		*pnWrote = 0;
	const std::string path = std::string(arcName) + ".zip";
	unzFile uf = unzOpen(path.c_str());
	if (!uf)
		return 1;
	bool found = false;
	unz_file_info info = {};
	if (fileName)
	{
		if (unzGoToFirstFile(uf) == UNZ_OK)
			do
			{
				char name[512] = "";
				if (unzGetCurrentFileInfo(uf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
					continue;
				found = strcasecmp(name, fileName) == 0;
			} while (!found && unzGoToNextFile(uf) == UNZ_OK);
	}
	else
		found = unzGoToFirstFile(uf) == UNZ_OK && unzGetCurrentFileInfo(uf, &info, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK;
	if (!found || info.uncompressed_size > 256u * 1024 * 1024 || unzOpenCurrentFile(uf) != UNZ_OK)
	{
		unzClose(uf);
		return 1;
	}
	const bool own = *Dest == nullptr;
	if (own)
		*Dest = malloc(info.uncompressed_size ? info.uncompressed_size : 1);
	if (!*Dest)
	{
		unzCloseCurrentFile(uf);
		unzClose(uf);
		return 1;
	}
	const int n = info.uncompressed_size ? unzReadCurrentFile(uf, *Dest, unsigned(info.uncompressed_size)) : 0;
	const bool ok = unzCloseCurrentFile(uf) == UNZ_OK && n == int(info.uncompressed_size);
	unzClose(uf);
	if (!ok)
	{
		if (own)
		{
			free(*Dest);
			*Dest = nullptr;
		}
		return 1;
	}
	if (pnWrote)
		*pnWrote = n;
	return 0;
}

namespace burn
{
void SetLog(void (*log)(const char* line))
{
	g.log = log;
}

void SetHiscores(bool on)
{
	EnableHiscores = on ? 1 : 0;
}

std::string CoreVersion()
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d.%d.%d.%d", VER_MAJOR, VER_MINOR, VER_BETA, VER_ALPHA);
	return buf;
}

static void SetPath(TCHAR* dst, const std::string& dir)
{
	std::string d = dir.empty() ? "." : dir;
	if (d.back() != '/')
		d += '/';
	snprintf(dst, MAX_PATH, "%s", d.c_str());
}

bool Init(const Paths& paths)
{
	if (g.inited)
		return true;
	g.paths = paths;
	SetPath(szAppHiscorePath, paths.hiscore);
	SetPath(szAppSamplesPath, paths.samples);
	SetPath(szAppEEPROMPath, paths.saves);
	SetPath(szAppBlendPath, paths.blend);
	SetPath(szAppHDDPath, paths.saves);
	bprintf = CoreLog;
	BurnExtLoadRom = LoadRom;
	BurnHighCol = HighCol32;
	nBurnBpp = 4;
	nInterpolation = 3; // FBNeo's defaults for ADPCM/PCM (cubic) and FM (none)
	nFMInterpolation = 0;
	bBurnUseBlend = true;
	EnableHiscores = 1;
	if (BurnLibInit() != 0)
	{
		Log("[burn] BurnLibInit failed");
		return false;
	}
	g.inited = true;
	Log("[burn] FBNeo %s, %u drivers", CoreVersion().c_str(), nBurnDrvCount);
	return true;
}

void Exit()
{
	if (!g.inited)
		return;
	Unload();
	BurnLibExit();
	g.inited = false;
}

int DriverCount()
{
	return g.inited ? int(nBurnDrvCount) : 0;
}

static bool IsArcade(uint32_t hw, uint32_t genre)
{
	const uint32_t prefix = hw & 0x7f000000u;
	switch (prefix)
	{
		case HARDWARE_PREFIX_SEGA_MEGADRIVE:
		case HARDWARE_PREFIX_PCENGINE:
		case HARDWARE_PREFIX_SEGA_MASTER_SYSTEM:
		case HARDWARE_PREFIX_SEGA_SG1000:
		case HARDWARE_PREFIX_COLECO:
		case HARDWARE_PREFIX_SEGA_GAME_GEAR:
		case HARDWARE_PREFIX_MSX:
		case HARDWARE_PREFIX_SPECTRUM:
		case HARDWARE_PREFIX_NES:
		case HARDWARE_PREFIX_FDS:
		case HARDWARE_PREFIX_NGP:
		case HARDWARE_PREFIX_CHANNELF:
		case HARDWARE_PREFIX_SNES:
		case HARDWARE_PREFIX_ASTROHOME:
		case HARDWARE_PREFIX_GBA:
			return false;
		default:
			break;
	}
	// (the top bit, HARDWARE_PREFIX_CARTRIDGE, is set for the Neo Geo's cartridge games too: not a console mark)
	if ((hw & HARDWARE_PUBLIC_MASK) == uint32_t(HARDWARE_SNK_NEOCD))
		return false;
	(void)genre;
	return true;
}

bool GetDriver(int index, Driver* out)
{
	if (!g.inited || index < 0 || UINT32(index) >= nBurnDrvCount || g.loaded)
		return false;
	const UINT32 old = nBurnDrvActive;
	nBurnDrvActive = UINT32(index);
	auto text = [](UINT32 i) {
		const char* s = BurnDrvGetTextA(i);
		return std::string(s ? s : "");
	};
	Driver d;
	d.index = index;
	d.name = text(DRV_NAME);
	d.parent = text(DRV_PARENT);
	d.board = text(DRV_BOARDROM);
	d.title = text(DRV_FULLNAME);
	d.year = text(DRV_DATE);
	d.maker = text(DRV_MANUFACTURER);
	d.system = text(DRV_SYSTEM);
	d.hardware = uint32_t(BurnDrvGetHardwareCode());
	d.flags = uint32_t(BurnDrvGetFlags());
	d.genre = uint32_t(BurnDrvGetGenreFlags());
	d.players = BurnDrvGetMaxPlayers();
	d.working = (d.flags & BDF_GAME_WORKING) != 0;
	d.clone = (d.flags & BDF_CLONE) != 0;
	d.vertical = (d.flags & BDF_ORIENTATION_VERTICAL) != 0;
	d.bios = (d.flags & BDF_BOARDROM) != 0 || (d.genre & GBF_BIOS) != 0;
	d.arcade = IsArcade(d.hardware, d.genre);
	nBurnDrvActive = old;
	*out = d;
	return true;
}

int FindDriver(const std::string& name)
{
	if (!g.inited)
		return -1;
	const std::string want = Lower(name);
	const UINT32 old = nBurnDrvActive;
	int found = -1;
	for (UINT32 i = 0; i < nBurnDrvCount && found < 0; i++)
	{
		nBurnDrvActive = i;
		const char* n = BurnDrvGetTextA(DRV_NAME);
		if (n && want == n)
			found = int(i);
	}
	nBurnDrvActive = old;
	return found;
}

RomCheck CheckRoms(int driver, const ZipFinder& find)
{
	RomCheck check;
	if (!g.inited || g.loaded || driver < 0 || UINT32(driver) >= nBurnDrvCount)
		return check;
	const UINT32 old = nBurnDrvActive;
	nBurnDrvActive = UINT32(driver);
	OpenZips(find, &check);
	FindRoms(&check);
	CloseZips();
	nBurnDrvActive = old;
	return check;
}

bool Load(int driver, const ZipFinder& find, std::string* error, const std::string& dips)
{
	if (!g.inited || driver < 0 || UINT32(driver) >= nBurnDrvCount)
		return false;
	Unload();
	if (!GetDriver(driver, &g.cur))
		return false;
	nBurnDrvActive = UINT32(driver);
	RomCheck check;
	OpenZips(find, &check);
	FindRoms(&check);
	for (const std::string& b : check.bad_crc)
		Log("[burn] %s: found by name, with another CRC (used anyway)", b.c_str());
	if (!check.ok)
	{
		std::string sets, roms;
		for (const std::string& s : check.sets)
			sets += (sets.empty() ? "" : ", ") + s + ".zip";
		for (size_t i = 0; i < check.missing.size() && i < 6; i++)
			roms += (roms.empty() ? "" : ", ") + check.missing[i];
		if (check.missing.size() > 6)
			roms += " and " + std::to_string(check.missing.size() - 6) + " more";
		Log("[burn] %s: %zu ROM(s) missing (%s), looked in %s (%zu found)", g.cur.name.c_str(), check.missing.size(),
			roms.c_str(), sets.c_str(), g.zips.size());
		if (error)
			*error = "Missing ROMs for " + g.cur.name + ": " + roms + ". FBNeo PS5 looked in " + sets +
				" - the sets must match FBNeo's (" + CoreVersion() + ").";
		CloseZips();
		return false;
	}
	// the colour format before Init (the palettes are made there): 32 bits, or 16 for the drivers that need it
	g.bpp = (g.cur.flags & BDF_16BIT_ONLY) ? 2 : 4;
	nBurnBpp = g.bpp;
	BurnHighCol = g.bpp == 4 ? HighCol32 : HighCol16;
	ScanInputs();
	ScanDips();
	DefaultDips();
	if (!dips.empty())
		LoadDips(dips);
	// the DIP switches go into the driver before it starts (FBNeo's frontends: InputMake before BurnDrvInit), not
	// only before the first frame: the Neo Geo loads the BIOS its switches name, other drivers read their region
	WriteDips();
	SizeSound();
	g.draw_w = g.draw_h = 0;
	g.draw.clear();
	pBurnDraw = nullptr;
	g.drawing = false;
	const INT32 rc = BurnDrvInit();
	if (rc != 0)
	{
		Log("[burn] %s: BurnDrvInit failed (%d)", g.cur.name.c_str(), rc);
		BurnDrvExit();
		CloseZips();
		if (error)
			*error = "FBNeo could not start " + g.cur.name + " (the ROMs may be from another version of the set).";
		return false;
	}
	bDrvOkay = 1;
	// FBNeo's own frontends ask every driver for its whole palette once the video is set up (SetBurnHighCol ->
	// VidRecalcPal); some build it only then (Cabal: black screen without it)
	BurnRecalcPal();
	g.loaded = true;
	g.reset_frames = 0;
	SizeSound();
	SizeDrawBuffer();
	LoadNvram();
	Log("[burn] running %s \"%s\" (%s, %s %s), %dx%d, %.2f fps, %d buttons%s%s", g.cur.name.c_str(), g.cur.title.c_str(),
		g.cur.system.c_str(), g.cur.maker.c_str(), g.cur.year.c_str(), g.draw_w, g.draw_h, Fps(), g.buttons,
		g.analog ? ", analog" : "", g.cur.vertical ? ", vertical" : "");
	return true;
}

void Unload()
{
	if (!g.loaded)
		return;
	SaveNvram(false);
	BurnDrvExit();
	CloseZips(); // kept open while the game runs: a few drivers load ROMs again at a reset (the Neo Geo's BIOS)
	bDrvOkay = 0;
	g.loaded = false;
	pBurnDraw = nullptr;
	g.draw.clear();
	g.pic.clear();
	g.draw_w = g.draw_h = g.pic_w = g.pic_h = 0;
	g.inputs.clear();
	g.dips.clear();
	g.dip_value.clear();
	g.nvram_last.clear();
}

bool Loaded()
{
	return g.loaded;
}

const Driver& Current()
{
	return g.cur;
}

double Fps()
{
	return nBurnFPS > 100 ? nBurnFPS / 100.0 : 60.0;
}

double Aspect()
{
	INT32 x = 4, y = 3;
	if (g.loaded)
		BurnDrvGetAspect(&x, &y);
	if (x <= 0 || y <= 0)
	{
		x = 4;
		y = 3;
	}
	// the monitor's shape as the picture is shown (turned the right way up): a vertical game's is taller than wide
	// (the drivers give 3:4 or 4:3 for it)
	const double a = double(x) / y;
	return g.cur.vertical && a > 1.0 ? 1.0 / a : a;
}

int ButtonCount()
{
	return g.buttons;
}

std::string ButtonName(int i)
{
	for (const In& x : g.inputs)
		if (x.kind == Kind::Button && x.player == 0 && x.button == i)
		{
			std::string n = x.name;
			if (n.size() > 3 && (n.compare(0, 3, "P1 ") == 0 || n.compare(0, 3, "p1 ") == 0))
				n = n.substr(3);
			return n;
		}
	return "";
}

bool HasAnalog()
{
	return g.analog;
}

std::vector<DipGroup> Dips()
{
	std::vector<DipGroup> out;
	for (const DipGroupInt& d : g.dips)
	{
		DipGroup o;
		o.name = d.name;
		o.def = d.def;
		o.current = d.def;
		for (size_t i = 0; i < d.options.size(); i++)
		{
			o.options.push_back(d.options[i].name);
			if (DipIsSet(UINT32(d.options[i].index)))
				o.current = int(i);
		}
		out.push_back(o);
	}
	return out;
}

void SetDip(int group, int option)
{
	if (group < 0 || size_t(group) >= g.dips.size())
		return;
	const DipGroupInt& d = g.dips[size_t(group)];
	if (option < 0 || size_t(option) >= d.options.size())
		return;
	struct BurnDIPInfo bdi = {};
	if (BurnDrvGetDIPInfo(&bdi, UINT32(d.options[size_t(option)].index)) != 0)
		return;
	if (uint8_t* v = DipSlot(bdi.nInput))
		*v = uint8_t((*v & ~bdi.nMask) | (bdi.nSetting & bdi.nMask));
}

void ResetDips()
{
	DefaultDips();
}

// A group's key in the .dip file: its name, and "#2", "#3"... for the next groups of the same name (884 drivers repeat
// one: "Unused", "Unknown"...), so that each one keeps its own setting.
static std::vector<std::string> DipKeys()
{
	std::vector<std::string> keys;
	for (size_t i = 0; i < g.dips.size(); i++)
	{
		int seen = 0;
		for (size_t j = 0; j < i; j++)
			seen += g.dips[j].name == g.dips[i].name ? 1 : 0;
		keys.push_back(seen ? g.dips[i].name + "#" + std::to_string(seen + 1) : g.dips[i].name);
	}
	return keys;
}

bool SaveDips(const std::string& path)
{
	const std::vector<std::string> keys = DipKeys();
	const std::vector<DipGroup> dips = Dips();
	std::vector<uint8_t> text;
	for (size_t i = 0; i < dips.size() && i < keys.size(); i++)
		if (dips[i].current != dips[i].def)
		{
			const std::string line = keys[i] + "=" + dips[i].options[size_t(dips[i].current)] + "\n";
			text.insert(text.end(), line.begin(), line.end());
		}
	if (text.empty())
	{
		unlink(path.c_str());
		return true;
	}
	return WriteAtomic(path, text);
}

bool LoadDips(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return false;
	const std::vector<std::string> keys = DipKeys();
	char line[512];
	int applied = 0, unknown = 0;
	while (fgets(line, sizeof(line), f))
	{
		std::string s = line;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		const size_t eq = s.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = s.substr(0, eq), value = s.substr(eq + 1);
		bool done = false;
		for (size_t gi = 0; gi < keys.size() && !done; gi++)
			if (keys[gi] == key)
				for (size_t o = 0; o < g.dips[gi].options.size() && !done; o++)
					if (g.dips[gi].options[o].name == value)
					{
						SetDip(int(gi), int(o));
						applied++;
						done = true;
					}
		unknown += done ? 0 : 1;
	}
	fclose(f);
	Log("[burn] DIP switches from %s: %d set%s", path.c_str(), applied,
		unknown ? " (some lines don't match this driver's switches: left as they are)" : "");
	return true;
}

void RunFrame(const Input& in, bool draw)
{
	if (!g.loaded)
		return;
	ApplyInputs(in);
	if (g.reset_frames > 0)
		g.reset_frames--;
	SizeSound();
	g.drawing = draw;
	SizeDrawBuffer();
	BurnDrvFrame();
	g.snd_frames = nBurnSoundLen;
	if (draw && pBurnDraw)
		MakePicture();
	g.drawing = false;
	pBurnDraw = nullptr;
}

const uint32_t* Picture(int* w, int* h)
{
	*w = g.pic_w;
	*h = g.pic_h;
	return g.pic.empty() ? nullptr : g.pic.data();
}

const int16_t* Sound(int* frames)
{
	*frames = g.loaded ? g.snd_frames : 0;
	return g.snd.data();
}

void Reset()
{
	// the driver's reset input, held for two frames (the drivers that look at a change see one)
	if (g.loaded)
		g.reset_frames = 2;
}

bool StateToMemory(std::vector<uint8_t>* out)
{
	if (!g.loaded)
		return false;
	Scan s;
	DoScan(ACB_FULLSCAN | ACB_READ, ReadAcb, &s);
	return Pack(kMagicState, s, out);
}

bool StateFromMemory(const std::vector<uint8_t>& in)
{
	if (!g.loaded)
		return false;
	Scan now;
	DoScan(ACB_FULLSCAN | ACB_READ, LenAcb, &now);
	uint32_t frame = 0;
	std::vector<uint8_t> data;
	std::string error;
	if (!Unpack(kMagicState, in, now, &frame, &data, &error))
		return false;
	Scan w;
	w.src = data.data();
	w.left = data.size();
	DoScan(ACB_FULLSCAN | ACB_WRITE, WriteAcb, &w);
	nCurrentFrame = frame;
	BurnRecalcPal();
	return !w.short_data;
}

bool SaveState(const std::string& path)
{
	std::vector<uint8_t> data;
	return StateToMemory(&data) && WriteAtomic(path, data);
}

bool LoadState(const std::string& path, std::string* error)
{
	std::string why;
	if (!g.loaded)
		why = "no game running";
	std::vector<uint8_t> file;
	if (why.empty() && !ReadAll(path, &file))
		why = "can't read it";
	if (why.empty())
	{
		Scan now;
		DoScan(ACB_FULLSCAN | ACB_READ, LenAcb, &now);
		uint32_t frame = 0;
		std::vector<uint8_t> data;
		if (Unpack(kMagicState, file, now, &frame, &data, &why))
		{
			Scan w;
			w.src = data.data();
			w.left = data.size();
			DoScan(ACB_FULLSCAN | ACB_WRITE, WriteAcb, &w);
			nCurrentFrame = frame;
			BurnRecalcPal();
			return true;
		}
	}
	Log("[burn] state %s not loaded: %s", path.c_str(), why.c_str());
	if (error)
		*error = why;
	return false;
}

bool SaveNvram(bool force)
{
	if (!g.loaded)
		return false;
	Scan s;
	DoScan(ACB_NVRAM | ACB_READ, ReadAcb, &s);
	if (s.lens.empty() || (!force && s.data == g.nvram_last))
		return true;
	std::vector<uint8_t> file;
	const bool ok = Pack(kMagicNvram, s, &file) && WriteAtomic(NvramPath(), file);
	if (ok)
		g.nvram_last = s.data;
	Log("[burn] NVRAM -> %s (%zu bytes): %s", NvramPath().c_str(), s.data.size(), ok ? "ok" : "failed");
	return ok;
}
} // namespace burn
