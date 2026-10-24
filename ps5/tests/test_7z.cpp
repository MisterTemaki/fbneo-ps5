// FBNeo PS5: the 7z reader (frontend/fe_7z.cpp, on FBNeo's lib7z) on the archives of tests/7z/
// (tests/make_7z_fixtures.py made them): each method the decoder has, a solid block of several files read the way a
// set's ROMs are (ExtractTo, the block kept), what the directory says, and every way an archive can be refused.
//   test-7z <tests/7z>
// SPDX-License-Identifier: MIT
#include "fe_7z.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

using fe::sevenzip::Archive;
using fe::sevenzip::Status;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, label)                                                     \
	do                                                                         \
	{                                                                          \
		if (cond)                                                              \
			g_pass++;                                                          \
		else                                                                   \
		{                                                                      \
			g_fail++;                                                          \
			printf("  FAIL: %s (test_7z.cpp:%d)\n", label, __LINE__);          \
		}                                                                      \
	} while (0)

static std::string g_dir;

static std::string P(const char* name)
{
	return g_dir + "/" + name;
}

static uint32_t Crc(const std::vector<uint8_t>& d)
{
	return uint32_t(crc32(0, d.data(), uInt(d.size())));
}

// The archive's one file, rom.bin, decoded: its size and CRC as the directory says. Returns its bytes.
static std::vector<uint8_t> OneRom(const char* file)
{
	Archive a;
	const Status st = a.Open(P(file));
	std::string label = std::string(file) + ": opens";
	CHECK(st == Status::Ok, label.c_str());
	std::vector<uint8_t> out;
	if (st != Status::Ok)
		return out;
	label = std::string(file) + ": one file, rom.bin, 16 KiB, its CRC in the directory";
	CHECK(a.Entries().size() == 1 && a.Entries()[0].name == "rom.bin" && a.Entries()[0].size == 16384 && a.Entries()[0].has_crc,
		label.c_str());
	label = std::string(file) + ": decoded, with the directory's CRC";
	CHECK(a.Extract(0, &out, 1u << 20) == Status::Ok && out.size() == 16384 && Crc(out) == a.Entries()[0].crc, label.c_str());
	return out;
}

static Status OpenStatus(const char* file)
{
	Archive a;
	return a.Open(P(file));
}

static Status ExtractStatus(const char* file, size_t i, uint64_t max = 1u << 20)
{
	Archive a;
	const Status st = a.Open(P(file));
	if (st != Status::Ok)
		return st;
	std::vector<uint8_t> out;
	return a.Extract(i, &out, max);
}

static Status ExtractToStatus(const char* file, size_t i)
{
	Archive a;
	const Status st = a.Open(P(file));
	if (st != Status::Ok)
		return st;
	std::vector<uint8_t> out(1 << 20);
	size_t got = 0;
	return a.ExtractTo(i, out.data(), out.size(), &got);
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: test-7z <tests/7z>\n");
		return 2;
	}
	g_dir = argv[1];

	const std::vector<uint8_t> rom = OneRom("lzma.7z");
	for (const char* f : {"lzma2.7z", "ppmd.7z", "bcj.7z", "delta.7z"})
	{
		const std::vector<uint8_t> same = OneRom(f);
		const std::string label = std::string(f) + ": the same bytes as lzma.7z's";
		CHECK(!rom.empty() && same == rom, label.c_str());
	}

	// a solid block of three files, read as a set's ROMs are: each copied out of the block decoded once and kept
	{
		Archive a;
		CHECK(a.Open(P("solid.7z")) == Status::Ok && a.Entries().size() == 3, "solid.7z: three files");
		if (a.Entries().size() == 3)
		{
			CHECK(a.Entries()[0].name == "readme.txt" && a.Entries()[1].name == "roms/a.bin" && a.Entries()[2].name == "b.bin",
				"solid.7z: the names in the archive's order, one with its folder ('/')");
			CHECK(a.Check(2) == Status::Ok && a.Check(5) == Status::NotFound, "solid.7z: Check says each can be decoded");
			std::vector<uint8_t> b(a.Entries()[2].size), x(a.Entries()[1].size), part(100);
			size_t got = 0;
			CHECK(a.ExtractTo(2, b.data(), b.size(), &got) == Status::Ok && got == b.size() && Crc(b) == a.Entries()[2].crc,
				"solid.7z: the last file first, into the caller's buffer");
			CHECK(a.ExtractTo(1, x.data(), x.size(), &got) == Status::Ok && got == x.size() && Crc(x) == a.Entries()[1].crc,
				"solid.7z: then the middle one, from the kept block");
			CHECK(rom.size() == x.size() + b.size() && memcmp(rom.data(), x.data(), x.size()) == 0 &&
					  memcmp(rom.data() + x.size(), b.data(), b.size()) == 0,
				"solid.7z: the two halves of rom.bin");
			CHECK(a.ExtractTo(1, part.data(), part.size(), &got) == Status::Ok && got == 100 &&
					  memcmp(part.data(), x.data(), 100) == 0,
				"solid.7z: a buffer smaller than the file gets its first bytes, nothing past it");
			std::vector<uint8_t> big(a.Entries()[0].size + 4096, 0xee);
			CHECK(a.ExtractTo(0, big.data(), big.size(), &got) == Status::Ok && got == a.Entries()[0].size &&
					  memcmp(big.data(), "A test archive", 14) == 0 && big[got] == 0xee,
				"solid.7z: a bigger buffer gets the file's bytes only (the rest untouched)");
			a.Release();
			CHECK(a.ExtractTo(1, x.data(), x.size(), &got) == Status::Ok && Crc(x) == a.Entries()[1].crc,
				"solid.7z: after Release the block is decoded again");
		}
	}
	{
		Archive a;
		CHECK(a.Open(P("empty.7z")) == Status::Ok && a.Entries().empty(), "empty.7z: opens, no files");
		std::vector<uint8_t> out;
		CHECK(a.Extract(0, &out, 1u << 20) == Status::NotFound, "empty.7z: nothing to extract");
	}
	// the sets the app's tests use: their directories
	{
		Archive a;
		bool roms = a.Open(P("pacman.7z")) == Status::Ok && !a.Entries().empty();
		for (const fe::sevenzip::Entry& e : a.Entries())
			roms = roms && e.has_crc && !e.is_dir;
		CHECK(roms, "pacman.7z: a set's ROMs, each with its CRC");
	}

	// refused, each with its reason, and nothing crashes
	CHECK(OpenStatus("missing.7z") == Status::NotFound, "a file that isn't there: not found");
	CHECK(OpenStatus("../make_7z_fixtures.py") == Status::Broken, "not a 7z archive (a text file): broken");
	CHECK(OpenStatus("truncated.7z") == Status::Broken, "truncated.7z (cut in half): broken");
	CHECK(ExtractStatus("corrupt.7z", 0) == Status::Broken, "corrupt.7z (its data changed): broken (CRC or data error)");
	CHECK(ExtractToStatus("corrupt.7z", 0) == Status::Broken, "corrupt.7z: the same through ExtractTo (the bad block not kept)");
	CHECK(ExtractStatus("bzip2.7z", 0) == Status::Unsupported, "bzip2.7z: a method the decoder doesn't have");
	CHECK(ExtractStatus("aes.7z", 0) == Status::Encrypted && ExtractToStatus("aes.7z", 0) == Status::Encrypted,
		"aes.7z: encrypted data, refused as such");
	{
		Archive a;
		CHECK(a.Open(P("aes.7z")) == Status::Ok && a.Entries().size() == 1 && a.Entries()[0].name == "rom.bin" &&
				  a.Check(0) == Status::Encrypted,
			"aes.7z: its names are readable, Check says encrypted");
	}
	CHECK(OpenStatus("aeshdr.7z") == Status::Encrypted, "aeshdr.7z (names encrypted): refused at open, as encrypted");
	CHECK(OpenStatus("ponpoko.7z") == Status::Ok && ExtractToStatus("ponpoko.7z", 0) == Status::Encrypted,
		"ponpoko.7z (a set, encrypted): listed, its ROMs refused");
	CHECK(ExtractStatus("ppmdbig.7z", 0) == Status::NoMemory, "ppmdbig.7z: a 1 GiB PPMd model, over the decoder's budget: refused");
	CHECK(OpenStatus("many.7z") == Status::TooBig, "many.7z: 70,000 files, more than an archive may list: refused at open");
	{
		std::atomic<bool> cancel{true};
		Archive a;
		CHECK(a.Open(P("lzma2.7z"), &cancel) == Status::ReadError, "a cancel set before the open: every read fails, nothing is decoded");
		std::atomic<bool> go{false};
		Archive b;
		CHECK(b.Open(P("lzma2.7z"), &go) == Status::Ok, "the same archive with the cancel flag down opens");
		go = true;
		std::vector<uint8_t> out;
		CHECK(b.Extract(0, &out, 1u << 20) == Status::ReadError && out.empty(), "a cancel set after the open: the decode stops at its next read");
	}
	CHECK(ExtractStatus("lzma2.7z", 0, 1000) == Status::TooBig, "Extract: a file over the caller's limit, refused before decoding");
	{
		Archive a;
		CHECK(a.Open(P("bigblock.7z")) == Status::Ok && a.Entries().size() == 2 && a.Check(0) == Status::TooBig,
			"bigblock.7z: a small file in a 513 MiB solid block: listed, Check says too big");
		std::vector<uint8_t> out(1 << 20);
		size_t got = 1;
		CHECK(a.ExtractTo(0, out.data(), out.size(), &got) == Status::TooBig && got == 0,
			"bigblock.7z: refused before anything is decoded");
	}
	CHECK(ExtractStatus("lzma2.7z", 5) == Status::NotFound && ExtractToStatus("lzma2.7z", 5) == Status::NotFound,
		"an index past the last file: not found");

	printf("7z: passed %d, failed %d\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
