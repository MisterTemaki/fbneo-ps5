// FBNeo PS5: the core and fe_burn.cpp on the host, without the PS5 layer (tests and checks).
//
//   fbneo_headless list [arcade|all]            the drivers: name, parent, board, hardware, flags, title
//   fbneo_headless roms <set>                   the set's ROMs: index, name, length, CRC, type
//   fbneo_headless nvram <rom dir> <set>          fills the set's NVRAM areas with 0x5a, unloads (fe_burn writes
//       <saves>/<set>.nvram), loads the set again and checks that the areas hold 0x5a
//   fbneo_headless dips <rom dir> <set>           moves the DIP switches off their defaults (a group named like an earlier
//       one stays as it is), saves them (SaveDips), loads the set again, reads them back (LoadDips) and checks that each
//       group has its setting back
//   fbneo_headless run <rom dir> <set> <frames> [out.ppm] [--state] [--dips] [--press N]
//       runs the set for N frames (no input but coin+start pressed at frames 60 and 90 and fire 1 every 8 frames
//       after 120), writes the last picture as a PPM, prints the picture size, the sound's level and a picture
//       checksum; --state also saves a state halfway, runs on, loads it back and checks that the same frames
//       follow (the same picture checksum); --dips lists the DIP switches.
//
// SPDX-License-Identifier: MIT

#include "fe_burn.h"

#include <sys/stat.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "burnint.h"

namespace
{
void Log(const char* line)
{
	fprintf(stderr, "%s\n", line);
}

uint64_t Hash(const uint32_t* p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

bool WritePpm(const char* path, const uint32_t* px, int w, int h)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++)
	{
		const uint8_t rgb[3] = {uint8_t(px[i] >> 16), uint8_t(px[i] >> 8), uint8_t(px[i])};
		fwrite(rgb, 1, 3, f);
	}
	return fclose(f) == 0;
}

burn::Input InputFor(uint64_t frame)
{
	burn::Input in;
	burn::PlayerInput& p = in.player[0];
	p.coin = frame >= 60 && frame < 64;
	p.start = frame >= 90 && frame < 94;
	p.button[0] = frame >= 120 && (frame / 8) % 2 == 0;
	p.right = frame >= 150 && (frame / 30) % 2 == 0;
	return in;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: see the comment at the top of fbneo_headless.cpp\n");
		return 2;
	}
	burn::SetLog(Log);
	const char* tmp = getenv("TMPDIR");
	const std::string scratch = std::string(tmp && *tmp ? tmp : "/tmp") + "/fbneo-headless";
	mkdir(scratch.c_str(), 0755);
	burn::Paths paths;
	paths.saves = paths.samples = paths.hiscore = paths.blend = paths.config = scratch;
	if (!burn::Init(paths))
		return 1;
	const std::string cmd = argv[1];
	if (cmd == "list")
	{
		const bool all = argc > 2 && std::string(argv[2]) == "all";
		int shown = 0;
		for (int i = 0; i < burn::DriverCount(); i++)
		{
			burn::Driver d;
			if (!burn::GetDriver(i, &d) || (!all && !d.arcade))
				continue;
			printf("%s\t%s\t%s\t%08x\t%08x\t%s\t%s\t%s\t%s\n", d.name.c_str(), d.parent.c_str(), d.board.c_str(), d.hardware,
				d.flags, d.system.c_str(), d.maker.c_str(), d.year.c_str(), d.title.c_str());
			shown++;
		}
		fprintf(stderr, "%d drivers\n", shown);
		return 0;
	}
	if (cmd == "roms" && argc > 2)
	{
		const int drv = burn::FindDriver(argv[2]);
		if (drv < 0)
			return 1;
		nBurnDrvActive = UINT32(drv);
		for (UINT32 i = 0; i < 1024; i++)
		{
			struct BurnRomInfo ri = {};
			if (BurnDrvGetRomInfo(&ri, i) != 0)
				break;
			char* name = nullptr;
			BurnDrvGetRomName(&name, i, 0);
			printf("%u\t%s\t%u\t%08x\t%08x\n", i, name ? name : "", ri.nLen, ri.nCrc, ri.nType);
		}
		for (UINT32 j = 0; j < 8; j++)
		{
			char* z = nullptr;
			if (BurnDrvGetZipName(&z, j) != 0 || !z)
				break;
			printf("zip\t%s\n", z);
		}
		if (const char* b = BurnDrvGetTextA(DRV_BOARDROM))
			printf("board\t%s\n", b);
		return 0;
	}
	if (cmd == "nvram" && argc > 3)
	{
		const std::string dir = argv[2];
		const int drv = burn::FindDriver(argv[3]);
		auto find = [&](const std::string& set) {
			const std::string p = dir + "/" + set + ".zip";
			struct stat st = {};
			return stat(p.c_str(), &st) == 0 ? p : std::string();
		};
		std::string error;
		if (drv < 0 || !burn::Load(drv, find, &error))
			return 3;
		static size_t bytes = 0;
		static bool all = true;
		bytes = 0;
		BurnAcb = [](struct BurnArea* ba) -> INT32 {
			if (ba->Data && ba->nLen)
				memset(ba->Data, 0x5a, ba->nLen);
			bytes += ba->nLen;
			return 0;
		};
		BurnAreaScan(ACB_NVRAM | ACB_WRITE, nullptr);
		printf("nvram %zu bytes filled\n", bytes);
		burn::Unload();
		if (!burn::Load(drv, find, &error))
			return 3;
		bytes = 0;
		all = true;
		BurnAcb = [](struct BurnArea* ba) -> INT32 {
			const UINT8* p = static_cast<const UINT8*>(ba->Data);
			for (UINT32 i = 0; p && i < ba->nLen; i++)
				all = all && p[i] == 0x5a;
			bytes += ba->nLen;
			return 0;
		};
		BurnAreaScan(ACB_NVRAM | ACB_READ, nullptr);
		printf("nvram %zu bytes read back: %s\n", bytes, all && bytes ? "same" : "DIFFERENT");
		burn::Unload();
		burn::Exit();
		return all && bytes ? 0 : 6;
	}
	if (cmd == "dips" && argc > 3)
	{
		const std::string dir = argv[2];
		const int drv = burn::FindDriver(argv[3]);
		auto find = [&](const std::string& set) {
			const std::string p = dir + "/" + set + ".zip";
			struct stat st = {};
			return stat(p.c_str(), &st) == 0 ? p : std::string();
		};
		std::string error;
		if (drv < 0 || !burn::Load(drv, find, &error))
			return 3;
		const std::string path = scratch + "/" + argv[3] + ".dip";
		std::vector<int> want;
		int changed = 0, repeated = 0;
		std::vector<burn::DipGroup> dips = burn::Dips();
		for (size_t i = 0; i < dips.size(); i++)
		{
			bool again = false;
			for (size_t j = 0; j < i && !again; j++)
				again = dips[j].name == dips[i].name;
			repeated += again ? 1 : 0;
			// a group whose name came before keeps its default: the first one's setting mustn't spill onto it
			if (!again && dips[i].options.size() > 1)
			{
				burn::SetDip(int(i), (dips[i].def + 1) % int(dips[i].options.size()));
				changed++;
			}
		}
		for (const burn::DipGroup& d : burn::Dips())
			want.push_back(d.current);
		const bool saved = burn::SaveDips(path);
		burn::Unload();
		if (!burn::Load(drv, find, &error))
			return 3;
		bool same = burn::LoadDips(path);
		dips = burn::Dips();
		for (size_t i = 0; i < dips.size() && i < want.size(); i++)
			if (dips[i].current != want[i])
			{
				printf("group %zu (%s): %d, saved %d\n", i, dips[i].name.c_str(), dips[i].current, want[i]);
				same = false;
			}
		printf("dips %zu group(s), %d repeated name(s), %d changed, saved %s, read back: %s\n", dips.size(), repeated, changed,
			saved ? "ok" : "FAILED", same && saved && dips.size() == want.size() ? "same" : "DIFFERENT");
		burn::Unload();
		burn::Exit();
		return same && saved ? 0 : 6;
	}
	if (cmd == "run" && argc > 4)
	{
		const std::string dir = argv[2];
		const int drv = burn::FindDriver(argv[3]);
		const long frames = atol(argv[4]);
		const char* ppm = argc > 5 && argv[5][0] != '-' ? argv[5] : nullptr;
		bool state = false, dips = false;
		for (int i = 5; i < argc; i++)
		{
			state = state || std::string(argv[i]) == "--state";
			dips = dips || std::string(argv[i]) == "--dips";
		}
		if (drv < 0)
		{
			fprintf(stderr, "no driver %s\n", argv[3]);
			return 1;
		}
		std::string error;
		auto find = [&](const std::string& set) {
			const std::string p = dir + "/" + set + ".zip";
			struct stat st = {};
			return stat(p.c_str(), &st) == 0 ? p : std::string();
		};
		if (!burn::Load(drv, find, &error))
		{
			printf("LOAD FAILED: %s\n", error.c_str());
			return 3;
		}
		if (dips)
			for (const burn::DipGroup& d : burn::Dips())
			{
				printf("dip\t%s\t%d/%zu\t", d.name.c_str(), d.current, d.options.size());
				for (size_t o = 0; o < d.options.size(); o++)
					printf("%s%s%s", o ? " | " : "", int(o) == d.def ? "*" : "", d.options[o].c_str());
				printf("\n");
			}
		double sum2 = 0;
		long samples = 0;
		std::vector<uint8_t> saved;
		uint64_t check_hash = 0, replay_hash = 0;
		const long half = frames / 2;
		for (long f = 0; f < frames; f++)
		{
			if (state && f == half && !burn::StateToMemory(&saved))
			{
				printf("STATE SAVE FAILED\n");
				return 4;
			}
			burn::RunFrame(InputFor(uint64_t(f)), true);
			int n = 0;
			const int16_t* s = burn::Sound(&n);
			for (int i = 0; i < n * 2; i++)
				sum2 += double(s[i]) * s[i];
			samples += n;
		}
		int w = 0, h = 0;
		const uint32_t* px = burn::Picture(&w, &h);
		const uint64_t hash = px ? Hash(px, size_t(w) * h) : 0;
		check_hash = hash;
		if (state)
		{
			if (!burn::StateFromMemory(saved))
			{
				printf("STATE LOAD FAILED\n");
				return 4;
			}
			for (long f = half; f < frames; f++)
				burn::RunFrame(InputFor(uint64_t(f)), true);
			px = burn::Picture(&w, &h);
			replay_hash = px ? Hash(px, size_t(w) * h) : 0;
		}
		printf("picture %dx%d hash %016" PRIx64 " fps %.2f aspect %.4f buttons %d sound %ld frames rms %.1f\n", w, h, hash,
			burn::Fps(), burn::Aspect(), burn::ButtonCount(), samples, samples ? std::sqrt(sum2 / (samples * 2.0)) : 0.0);
		if (state)
			printf("state replay %s (%016" PRIx64 ")\n", replay_hash == check_hash ? "same" : "DIFFERENT", replay_hash);
		if (ppm && px && !WritePpm(ppm, px, w, h))
			return 5;
		burn::Unload();
		burn::Exit();
		return 0;
	}
	fprintf(stderr, "unknown command\n");
	return 2;
}
