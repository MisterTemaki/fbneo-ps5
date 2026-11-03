// FBNeo PS5 2.2: Killer Instinct's hard disk without the game -- FBNeo's core (burn_hdd.cpp, the IDE controller of
// src/burn/drv/midway/ide.cpp, libchdr) with the port's lookup (frontend/fe_hdd.cpp) and glue (fe_burn.cpp), on
// synthetic images (tests/make_chd.py: CHD v5 uncompressed and zlib-compressed, with and without a SHA-1, and a raw
// image; never a real one):
//   - where the image is looked for (MAME's layout: the set's folder, a clone's parent's, the folders in order, any
//     case, .img after .chd, a path too long, missing);
//   - the IDE controller on it: IDENTIFY, CHS and LBA reads, the IRQ / DRQ order of reads and writes;
//   - the game's writes: only in <saves>/<set>.diff (the image never changes), read back after a reload, the v2 header
//     with the image's id, atomic writes, a .diff of another image set aside and put back (A -> B -> A), "-2" names,
//     the newest of several, renames that fail, errors other than ENOENT, a v1 .diff;
//   - an allocation that fails while a raw image is being opened: the half-open disk is closed (no descriptor left).
//
//   test-kinst-hdd <dir with A.chd B.chd Z.chd R.img>
//
// SPDX-License-Identifier: MIT

#include "burnint.h"
#include "burn_hdd.h"
#include "ide.h"

#include "fe_burn.h"
#include "fe_hdd.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// ---- a failing allocation on demand: the next one of 1 MiB or more throws std::bad_alloc ----
static bool g_fail_big = false;
void* operator new(size_t n)
{
	if (g_fail_big && n >= (1u << 20))
	{
		g_fail_big = false;
		throw std::bad_alloc();
	}
	if (void* p = malloc(n ? n : 1))
		return p;
	throw std::bad_alloc();
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

namespace
{
int g_ok = 0, g_bad = 0;
void Check(bool ok, const std::string& what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what.c_str());
	(ok ? g_ok : g_bad)++;
}

std::string g_dir, g_saves, g_image;
// the frontend's lookup, as the test wants it: the image g_image ("" = none)
INT32 TestFind(INT32, const char*, char* path, INT32 len)
{
	if (g_image.empty())
		return 1;
	snprintf(path, size_t(len), "%s", g_image.c_str());
	return 0;
}

std::string ReadFile(const std::string& p)
{
	std::string s;
	if (FILE* f = fopen(p.c_str(), "rb"))
	{
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0)
			s.append(buf, n);
		fclose(f);
	}
	return s;
}
void WriteFile(const std::string& p, const std::string& s)
{
	if (FILE* f = fopen(p.c_str(), "wb"))
	{
		fwrite(s.data(), 1, s.size(), f);
		fclose(f);
	}
}
bool Exists(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}
void Mkdirs(const std::string& p)
{
	std::string cur;
	for (size_t i = 0; i <= p.size(); i++)
		if (i == p.size() || p[i] == '/')
		{
			cur = p.substr(0, i);
			if (!cur.empty())
				mkdir(cur.c_str(), 0755);
		}
}
void Touch(const std::string& p) { WriteFile(p, "x"); }
void Rm(const std::string& p) { unlink(p.c_str()); }
void ClearSaves()
{
	if (DIR* d = opendir(g_saves.c_str()))
	{
		while (dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				Rm(g_saves + "/" + e->d_name);
		closedir(d);
	}
}
int Fds()
{
	int n = 0;
	if (DIR* d = opendir("/proc/self/fd"))
	{
		while (readdir(d))
			n++;
		closedir(d);
	}
	return n;
}
std::vector<std::string> Saves()
{
	std::vector<std::string> v;
	if (DIR* d = opendir(g_saves.c_str()))
	{
		while (dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				v.push_back(e->d_name);
		closedir(d);
	}
	return v;
}
UINT32 Le32(const std::string& s, size_t at)
{
	return UINT32(UINT8(s[at])) | UINT32(UINT8(s[at + 1])) << 8 | UINT32(UINT8(s[at + 2])) << 16 | UINT32(UINT8(s[at + 3])) << 24;
}
std::string Hex8(const std::string& s, size_t at)
{
	char b[9];
	for (int i = 0; i < 4; i++)
		snprintf(b + 2 * i, 3, "%02x", UINT8(s[at + size_t(i)]));
	return b;
}
// the SHA-1 a CHD v5's header carries (offset 84)
std::string ChdSha1(const std::string& path) { return ReadFile(path).substr(84, 20); }

// the stand-in images' sectors (tests/make_chd.py)
bool SectorIs(const UINT8* buf, UINT32 n, int seed)
{
	if (Le32(std::string((const char*)buf, 4), 0) != n)
		return false;
	for (int i = 0; i < 508; i++)
		if (buf[4 + i] != UINT8((n * 7 + UINT32(i) * 13 + UINT32(seed) * 31) & 0xff))
			return false;
	return true;
}

// ---- the IDE controller ----
int g_irq = 0, g_irq_raises = 0;
void Irq(int state)
{
	if (state && !g_irq)
		g_irq_raises++;
	g_irq = state;
}

enum { R_DATA = 0, R_ERROR = 1, R_COUNT = 2, R_SECTOR = 3, R_CYL_LO = 4, R_CYL_HI = 5, R_HEAD = 6, R_STATUS = 7 };
constexpr unsigned kDrq = 8;

ide::ide_disk* g_ide = nullptr;
bool OpenIde(const std::string& image)
{
	delete g_ide;
	g_ide = new ide::ide_disk();
	g_ide->set_irq_callback(Irq);
	g_image = image;
	return g_ide->load_hdd_image(0) == 0;
}
void CloseIde()
{
	delete g_ide;
	g_ide = nullptr;
}
void ReadData(UINT8* out)
{
	for (int i = 0; i < 256; i++)
	{
		const unsigned w = g_ide->read(R_DATA);
		out[2 * i] = UINT8(w);
		out[2 * i + 1] = UINT8(w >> 8);
	}
}
// a read of `count` sectors at LBA `lba` (LBA mode), each one checked against the image's pattern / `fill`
bool ReadLba(UINT32 lba, int count, int seed, int fill = -1, bool* irq_order = nullptr)
{
	g_ide->write(R_COUNT, unsigned(count));
	g_ide->write(R_SECTOR, lba & 0xff);
	g_ide->write(R_CYL_LO, (lba >> 8) & 0xff);
	g_ide->write(R_CYL_HI, (lba >> 16) & 0xff);
	g_ide->write(R_HEAD, 0xe0 | ((lba >> 24) & 0x0f));
	g_irq_raises = 0;
	g_ide->write(R_STATUS, 0x20);
	bool ok = true, order = true;
	for (int s = 0; s < count; s++)
	{
		// each sector: the interrupt, then DRQ while its data is read
		order = order && g_irq_raises == s + 1 && (g_ide->read_alternate(6) & kDrq);
		g_ide->read(R_STATUS); // acknowledges the interrupt
		UINT8 buf[512];
		ReadData(buf);
		if (fill >= 0)
		{
			for (int i = 0; i < 512; i++)
				ok = ok && buf[i] == UINT8(fill);
		}
		else
			ok = ok && SectorIs(buf, lba + UINT32(s), seed);
	}
	order = order && !(g_ide->read_alternate(6) & kDrq);
	if (irq_order)
		*irq_order = order;
	return ok;
}
// writes `count` sectors filled with `fill` at LBA lba
bool WriteLba(UINT32 lba, int count, UINT8 fill, bool* irq_order = nullptr)
{
	g_ide->write(R_COUNT, unsigned(count));
	g_ide->write(R_SECTOR, lba & 0xff);
	g_ide->write(R_CYL_LO, (lba >> 8) & 0xff);
	g_ide->write(R_CYL_HI, (lba >> 16) & 0xff);
	g_ide->write(R_HEAD, 0xe0 | ((lba >> 24) & 0x0f));
	g_irq_raises = 0;
	g_ide->write(R_STATUS, 0x30);
	// no interrupt before the first sector of a write; DRQ asks for its data
	bool order = g_irq_raises == 0 && (g_ide->read_alternate(6) & kDrq);
	for (int s = 0; s < count; s++)
	{
		for (int i = 0; i < 256; i++)
			g_ide->write(R_DATA, unsigned(fill) | unsigned(fill) << 8);
		// after each sector, an interrupt (the next one's DRQ, or the end)
		order = order && g_irq_raises == s + 1;
		g_ide->read(R_STATUS);
	}
	order = order && !(g_ide->read_alternate(6) & kDrq);
	if (irq_order)
		*irq_order = order;
	return order;
}

BurnHDD* Disk() { return BurnHDDGetOpen(0); }
const BurnHDDReport* Rep() { return BurnHDDReportCount() > 0 ? BurnHDDGetReport(BurnHDDReportCount() - 1) : nullptr; }

int g_rename_fail = 0; // which rename fails: 1 the set-aside, 2 the put-back (to <set>.diff)
INT32 RenameHook(const char* from, const char* to)
{
	const bool to_diff = strlen(to) > 5 && strcmp(to + strlen(to) - 5, ".diff") == 0;
	if ((g_rename_fail == 1 && !to_diff) || (g_rename_fail == 2 && to_diff))
	{
		errno = EIO;
		return 1;
	}
	(void)from;
	return 0;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	g_dir = argv[1];
	g_saves = g_dir + "/saves";
	Mkdirs(g_saves);
	burn::Paths paths;
	paths.saves = g_saves;
	paths.samples = paths.hiscore = paths.blend = paths.config = g_dir;
	burn::SetLog([](const char* l) { fprintf(stderr, "%s\n", l); });
	if (!burn::Init(paths))
		return 2;
	const std::string A = g_dir + "/A.chd", B = g_dir + "/B.chd", Z = g_dir + "/Z.chd", R = g_dir + "/R.img";
	const std::string a_sum = ReadFile(A), b_sum = ReadFile(B);

	// ---- the lookup (fe_hdd.cpp) ----
	printf("lookup (MAME's layout)\n");
	{
		using fe::hdd::Find;
		const std::string roms = g_dir + "/roms", usb = g_dir + "/usb", far = g_dir + "/far";
		Mkdirs(roms + "/kinst");
		Mkdirs(usb + "/kinst14");
		Touch(roms + "/kinst/kinst.chd");
		fe::hdd::Lookup l = fe::hdd::FindImage("kinst", "", "kinst", fe::hdd::Folders(roms + "/kinst.zip", "", {usb}));
		Check(l.result == Find::Found && l.path == roms + "/kinst/kinst.chd" && !l.on_network, "kinst: <rom folder>/kinst/kinst.chd beside kinst.zip");
		l = fe::hdd::FindImage("kinst14", "kinst", "kinst", fe::hdd::Folders(usb + "/kinst14.zip", roms + "/kinst.zip", {usb, roms}));
		Check(l.result == Find::Found && l.path == roms + "/kinst/kinst.chd", "a clone (kinst14) uses its parent's kinst/kinst.chd");
		Touch(usb + "/kinst14/kinst.chd");
		l = fe::hdd::FindImage("kinst14", "kinst", "kinst", fe::hdd::Folders(usb + "/kinst14.zip", roms + "/kinst.zip", {roms, usb}));
		Check(l.result == Find::Found && l.path == usb + "/kinst14/kinst.chd", "... its own kinst14/kinst.chd first, in its own archive's folder first");
		Mkdirs(far + "/KINST2");
		Touch(far + "/KINST2/Kinst2.CHD");
		l = fe::hdd::FindImage("kinst2", "", "kinst2", fe::hdd::Folders(roms + "/kinst2.zip", "", {roms, far}));
		Check(l.result == Find::Found && l.path == far + "/KINST2/Kinst2.CHD", "folder and file names of any case, in any ROM folder after the archive's");
		Mkdirs(roms + "/kinst13");
		Touch(roms + "/kinst13/kinst.img");
		l = fe::hdd::FindImage("kinst13", "kinst", "kinst", fe::hdd::Folders(roms + "/kinst13.zip", roms + "/kinst.zip", {}));
		Check(l.result == Find::Found && l.path == roms + "/kinst13/kinst.img", "an old kinst.img in the set's own folder");
		Touch(roms + "/kinst13/kinst.chd");
		l = fe::hdd::FindImage("kinst13", "kinst", "kinst.img", fe::hdd::Folders(roms + "/kinst13.zip", roms + "/kinst.zip", {}));
		Check(l.result == Find::Found && l.path == roms + "/kinst13/kinst.chd", ".chd before .img (a disk the driver names kinst.img too)");
		std::string deep = g_dir;
		while (deep.size() < 250)
			deep += "/deeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeep";
		Mkdirs(deep + "/kinst");
		Touch(deep + "/kinst/kinst.chd");
		l = fe::hdd::FindImage("kinst", "", "kinst", fe::hdd::Folders(deep + "/kinst.zip", "", {}));
		Check(l.result == Find::TooLong, "a path over 259 characters: refused as too long");
		l = fe::hdd::FindImage("kinst210", "kinst2", "kinst2", fe::hdd::Folders(usb + "/kinst210.zip", "", {usb}));
		Check(l.result == Find::Missing && l.need == "kinst2/kinst2.chd" && l.folder == usb + "/kinst2", "missing: kinst2/kinst2.chd, expected beside the set's archive");
		const int drv = burn::FindDriver("kinst14");
		l = burn::FindDisk(drv, [&](const std::string& set) { return set == "kinst" ? roms + "/kinst.zip" : std::string(); });
		Check(drv >= 0 && l.result == Find::Found && l.path == roms + "/kinst/kinst.chd" && l.need == "kinst/kinst.chd", "burn::FindDisk: the driver's disk (kinst) through the archives found");
	}

	BurnHDDFindImageHook = TestFind;
	BurnHDDRenameHook = RenameHook;
	nBurnDrvActive = UINT32(burn::FindDriver("kinst"));
	const std::string diff = g_saves + "/kinst.diff";

	printf("the IDE controller on a CHD\n");
	{
		ClearSaves();
		Check(OpenIde(A), "kinst: A.chd (uncompressed CHD v5) opens");
		Check(BurnHDDGetSectorCount(Disk()) == 8 * 13 * 47 && Rep() && Rep()->bShaMismatch, "8 x 13 x 47 sectors; its SHA-1 isn't the driver's: warned");
		g_irq_raises = 0;
		g_ide->write(R_STATUS, 0xec);
		const bool irq = g_irq_raises == 1 && (g_ide->read_alternate(6) & kDrq);
		g_ide->read(R_STATUS);
		UINT8 id[512];
		ReadData(id);
		auto word = [&](int i) { return unsigned(id[2 * i]) | unsigned(id[2 * i + 1]) << 8; };
		Check(irq && word(1) == 8 && word(3) == 13 && word(6) == 47 && word(5) == 512 && !(g_ide->read_alternate(6) & kDrq),
			"IDENTIFY: an interrupt, DRQ, the CHD's geometry (8 cylinders, 13 heads, 47 sectors)");
		// CHS: cylinder 2, head 3, sector 5 = ((2 * 13 + 3) * 47) + 5 - 1
		g_ide->write(R_COUNT, 1);
		g_ide->write(R_SECTOR, 5);
		g_ide->write(R_CYL_LO, 2);
		g_ide->write(R_CYL_HI, 0);
		g_ide->write(R_HEAD, 0xa0 | 3);
		g_ide->write(R_STATUS, 0x20);
		g_ide->read(R_STATUS);
		UINT8 buf[512];
		ReadData(buf);
		Check(SectorIs(buf, (2 * 13 + 3) * 47 + 4, 1), "a CHS read: cylinder 2, head 3, sector 5");
		bool order = false;
		Check(ReadLba(1000, 3, 1, -1, &order) && order, "an LBA read of 3 sectors across hunks: an interrupt and DRQ for each, DRQ off after");
		Check(WriteLba(77, 2, 0x5a, &order) && order, "a write of 2 sectors: DRQ first, no interrupt until each sector is in");
		Check(ReadLba(77, 2, 1, 0x5a), "... read back from the changes");
		Check(BurnHDDIsDirty(Disk()) && !Exists(diff), "... kept in memory until saved");
		Check(BurnHDDFlush(Disk()) == 0 && Exists(diff) && !Exists(diff + ".tmp"), "saved: kinst.diff, no .tmp left");
		const std::string d = ReadFile(diff);
		Check(d.size() == 64 + 2 * 516 && d.compare(0, 8, "FBNHDIFF") == 0 && Le32(d, 8) == 2 && Le32(d, 12) == 512 &&
			Le32(d, 16) == 8 * 13 * 47 && Le32(d, 20) == 2 && d.substr(24, 20) == ChdSha1(A) && d.substr(44, 20) == std::string(20, '\0'),
			"the v2 header: magic, version 2, sector size, sector count, entries, the CHD's SHA-1");
		CloseIde();
		Check(ReadFile(A) == a_sum, "the image is unchanged");
		Check(OpenIde(A) && ReadLba(77, 2, 1, 0x5a) && ReadLba(79, 1, 1), "reloaded: the changes are there, the rest from the image");
		// an atomic write: a .tmp that can't be made leaves the .diff as it was
		WriteLba(10, 1, 0x11);
		mkdir((diff + ".tmp").c_str(), 0755);
		Check(BurnHDDFlush(Disk()) != 0 && ReadFile(diff) == d && BurnHDDIsDirty(Disk()), "a write that fails: kinst.diff as it was, still to save");
		rmdir((diff + ".tmp").c_str());
		CloseIde();
		Check(ReadFile(A) == a_sum, "the image is still unchanged (CHD_OPEN_READ, \"rb\")");
	}

	printf("compressed and raw images\n");
	{
		ClearSaves();
		Check(OpenIde(B) && ReadLba(0, 2, 2) && ReadLba(8 * 13 * 47 - 9, 9, 2), "B.chd (zlib-compressed v5): first and last sectors");
		CloseIde();
		UINT8 id1[20], id2[20];
		Check(OpenIde(Z) && ReadLba(500, 1, 3), "Z.chd (no SHA-1, as a v1/v2 CHD): read");
		WriteLba(1, 1, 0x21);
		BurnHDDFlush(Disk());
		memcpy(id1, ReadFile(diff).substr(24, 20).data(), 20);
		CloseIde();
		Rm(diff);
		OpenIde(Z);
		WriteLba(1, 1, 0x21);
		BurnHDDFlush(Disk());
		memcpy(id2, ReadFile(diff).substr(24, 20).data(), 20);
		CloseIde();
		Check(memcmp(id1, id2, 20) == 0 && memcmp(id1, std::string(20, '\0').data(), 20) != 0, "... its id sampled from the file: not zero, the same each time");
		Rm(diff);
		Check(OpenIde(R) && ReadLba(4000, 2, 4), "R.img (a raw image): read");
		g_ide->write(R_STATUS, 0xec);
		g_ide->read(R_STATUS);
		UINT8 id[512];
		ReadData(id);
		Check(id[6] == 13 && id[12] == 47, "... Killer Instinct's geometry (13 heads, 47 sectors)");
		CloseIde();
	}

	printf("the .diff of another image: set aside, put back\n");
	const std::string a8 = ChdSha1(A).empty() ? "" : Hex8(ChdSha1(A), 0), b8 = Hex8(ChdSha1(B), 0);
	{
		ClearSaves();
		OpenIde(A);
		WriteLba(100, 1, 0xa1);
		CloseIde();
		const std::string a_diff = ReadFile(diff);
		Check(OpenIde(B) && Rep() && Rep()->bSetAside && std::string(Rep()->szSetAside) == diff + "." + a8 && Exists(diff + "." + a8) &&
			!Exists(diff) && ReadFile(diff + "." + a8) == a_diff,
			"A's kinst.diff, with B: set aside as kinst.diff." + a8);
		Check(ReadLba(100, 1, 2), "... B reads its own sector");
		WriteLba(101, 1, 0xb1);
		CloseIde();
		Check(OpenIde(A) && Rep() && Rep()->bPutBack && Rep()->bSetAside && std::string(Rep()->szSetAside) == diff + "." + b8 &&
			std::string(Rep()->szPutBack) == diff + "." + a8 && Exists(diff + "." + b8) && !Exists(diff + "." + a8),
			"A again: B's set aside as kinst.diff." + b8 + ", A's put back");
		Check(ReadLba(100, 1, 1, 0xa1) && ReadLba(101, 1, 1), "... A's changes back, none of B's");
		CloseIde();
		Check(ReadFile(A) == a_sum && ReadFile(B) == b_sum, "neither image written");
		// -2: the name is taken (by a file that isn't a .diff of A's)
		OpenIde(B); // A's goes aside again
		CloseIde();
		Check(Exists(diff + "." + a8) && Exists(diff), "B's back, A's aside (kinst.diff." + a8 + ")");
		Rm(diff);
		WriteFile(diff, ReadFile(diff + "." + a8)); // A's diff as kinst.diff again ...
		WriteFile(diff + "." + a8, "not a diff");   // ... and its usual name taken
		OpenIde(B);
		Check(Rep() && Rep()->bSetAside && std::string(Rep()->szSetAside) == diff + "." + a8 + "-2" && ReadFile(diff + "." + a8) == "not a diff",
			"a taken name: kinst.diff." + a8 + "-2, the other file left alone");
		CloseIde();
		Check(OpenIde(A) && Rep() && Rep()->bPutBack && std::string(Rep()->szPutBack) == diff + "." + a8 + "-2" && ReadLba(100, 1, 1, 0xa1),
			"... put back from the -2 name (its id matches; the plain name's doesn't)");
		CloseIde();
	}

	printf("several set-asides, the newest by its date; a v1 .diff\n");
	{
		ClearSaves();
		OpenIde(A);
		WriteLba(5, 1, 0x01);
		CloseIde();
		const std::string x1 = ReadFile(diff);
		Rm(diff);
		OpenIde(A);
		WriteLba(5, 1, 0x02);
		CloseIde();
		const std::string x2 = ReadFile(diff);
		Rm(diff);
		WriteFile(diff + "." + a8, x1);
		WriteFile(diff + "." + a8 + "-2", x2);
		struct utimbuf old = {1000000000, 1000000000};
		utime((diff + "." + a8).c_str(), &old);
		Check(OpenIde(A) && Rep() && Rep()->bPutBack && Rep()->nSetAsideCount == 2 && ReadLba(5, 1, 1, 0x02) && Exists(diff + "." + a8),
			"two set-asides of A: the newer one (-2) put back, the older kept");
		CloseIde();
		ClearSaves();
		// v1: magic, version 1, sector size, entries; then LBA + sector
		std::string v1 = "FBNHDIFF";
		auto put32 = [&](UINT32 v) { for (int i = 0; i < 4; i++) v1 += char((v >> (8 * i)) & 0xff); };
		put32(1);
		put32(512);
		put32(1);
		put32(42);
		v1 += std::string(512, char(0x42));
		WriteFile(diff, v1);
		Check(OpenIde(A) && ReadLba(42, 1, 1, 0x42) && Rep() && !Rep()->bSetAside, "a v1 kinst.diff (no id): read for this disk, as up to 2.1");
		WriteLba(43, 1, 0x43);
		CloseIde();
		const std::string now = ReadFile(diff);
		Check(Le32(now, 8) == 2 && now.substr(24, 20) == ChdSha1(A), "... written back as v2 with the disk's id");
		ClearSaves();
		WriteFile(diff + "." + a8, v1);
		Check(OpenIde(A) && Rep() && !Rep()->bPutBack && ReadLba(42, 1, 1), "a v1 file under a set-aside name is never put back");
		CloseIde();
	}

	printf("what can't be checked or renamed: saving off, nothing written over\n");
	{
		ClearSaves();
		OpenIde(A);
		WriteLba(7, 1, 0x77);
		CloseIde();
		const std::string a_diff = ReadFile(diff);
		g_rename_fail = 1;
		Check(OpenIde(B) && Rep() && Rep()->bSetAsideFailed && Rep()->bSavingOff && BurnHDDSavingOff(Disk()),
			"the set-aside's rename fails: saving off, said as such");
		WriteLba(8, 1, 0x88);
		CloseIde();
		Check(ReadFile(diff) == a_diff && Saves().size() == 1, "... A's kinst.diff not written over, nothing else made");
		g_rename_fail = 0;
		OpenIde(B); // aside now
		CloseIde();
		g_rename_fail = 2;
		Check(OpenIde(A) && Rep() && Rep()->bPutBackFailed && Rep()->bSavingOff && !Rep()->bPutBack, "the put-back's rename fails: saving off, said as such");
		WriteLba(9, 1, 0x99);
		CloseIde();
		Check(Exists(diff + "." + a8) && ReadFile(diff + "." + a8) == a_diff, "... A's set-aside still there, as it was");
		g_rename_fail = 0;
		// an error other than ENOENT: the saves "folder" is a file (ENOTDIR)
		ClearSaves();
		const std::string keep = szAppEEPROMPath;
		WriteFile(g_dir + "/notadir", "file");
		snprintf(szAppEEPROMPath, MAX_PATH, "%s/notadir/", g_dir.c_str());
		Check(OpenIde(A) && Rep() && Rep()->bSavingOff && strstr(Rep()->szWhy, "Not a directory"), "kinst.diff can't be checked (ENOTDIR, not ENOENT): saving off");
		WriteLba(1, 1, 1);
		Check(BurnHDDFlush(Disk()) == 0 && BurnHDDIsDirty(Disk()), "... nothing written");
		CloseIde();
		snprintf(szAppEEPROMPath, MAX_PATH, "%s", keep.c_str());
		Check(OpenIde(A) && Rep() && !Rep()->bSavingOff, "a missing kinst.diff (ENOENT): saving on");
		CloseIde();
	}

	printf("the periodic save's pieces\n");
	{
		ClearSaves();
		OpenIde(A);
		WriteLba(3, 1, 0x33);
		BurnHDDSnapshot s;
		Check(BurnHDDTakeSnapshot(Disk(), &s) == 1 && s.data.size() == 64 + 516, "a snapshot of the changes");
		WriteLba(4, 1, 0x44); // after the snapshot
		Check(BurnHDDWriteDiffData(s.szPath, s.data.data(), s.data.size()) == 0, "written from the snapshot's own bytes");
		BurnHDDSnapshotDone(&s, 1);
		Check(BurnHDDIsDirty(Disk()), "a write after the snapshot keeps the disk to save");
		BurnHDDFlush(Disk());
		Check(!BurnHDDIsDirty(Disk()) && Le32(ReadFile(diff), 20) == 2, "then saved whole");
		CloseIde();
	}

	printf("an allocation that fails while a raw image opens\n");
	{
		ClearSaves();
		const int fds = Fds();
		bool threw = false;
		g_fail_big = true;
		try
		{
			OpenIde(R);
		}
		catch (const std::bad_alloc&)
		{
			threw = true;
		}
		g_fail_big = false;
		Check(threw && BurnHDDOpenCount() == 1 && Fds() == fds + 1, "std::bad_alloc out of the open; the half-open disk registered (its file open)");
		BurnHDDCloseAll();
		Check(BurnHDDOpenCount() == 0 && Fds() == fds, "BurnHDDCloseAll closes it: no descriptor left (/proc/self/fd)");
		CloseIde(); // its m_disk was never set: nothing closed twice
		g_fail_big = true;
		try
		{
			OpenIde(R);
		}
		catch (const std::bad_alloc&)
		{
		}
		g_fail_big = false;
		BurnHDDClose(BurnHDDGetOpen(0));
		Check(BurnHDDOpenCount() == 0 && Fds() == fds, "BurnHDDClose takes a half-open disk too");
		CloseIde();
		Check(OpenIde(R) && ReadLba(10, 1, 4), "the next open works");
		CloseIde();
	}

	BurnHDDRenameHook = nullptr;
	BurnHDDFindImageHook = nullptr;
	burn::Exit();
	printf("test-kinst-hdd: %d checks ok, %d failed\n", g_ok, g_bad);
	return g_bad ? 1 : 0;
}
