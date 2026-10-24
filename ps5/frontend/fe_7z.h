// FBNeo PS5 frontend: ROM sets in a .7z (sf2ce.7z), read with the LZMA SDK's C 7z decoder -- FBNeo's own copy,
// src/dep/libs/lib7z (LZMA SDK 25.01, unchanged), the one FBNeo's own frontends use. The reader itself is NES FCEUmm PS5
// 1.1's (and Beetle PCE PS5's), not FBNeo's src/burner/un7z.cpp: see fe_burn.cpp, "7z".
//
// The archive is read through the app's own stdio calls (fopen, fread, fseeko...): a .7z on a network share goes
// through netfs like any file. Listing an archive decodes nothing but its directory, which carries each file's size
// and CRC32 (the ROM check finds a set's ROMs in it as in a zip's directory). Extracting decodes the "folder" (7-Zip's
// solid block) that holds the file, whole, into memory: a block bigger than kMaxBlock is refused before anything is
// decoded. A set is usually one solid block (7-Zip's default), so ExtractTo keeps the last block it decoded until
// Release(): the next ROM of the same block is copied from it, not decoded again.
// What is decoded: LZMA, LZMA2, PPMd, BCJ, BCJ2, ARM/ARM64/ARMT/PPC/SPARC/IA64/RISC-V filters, Delta, stored. Not:
// BZip2, Deflate, Zstandard... (7-Zip's C++ methods), nor encrypted archives (no AES).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fe
{
namespace sevenzip
{
// The biggest solid block decoded (the whole block is decoded to reach any file in it). A set packed by 7-Zip with its
// defaults is one block of the whole set: 512 MiB takes every FBNeo arcade set but the biggest PGM2 ones (kof98umh).
constexpr uint64_t kMaxBlock = 512ull << 20;

enum class Status
{
	Ok,
	NotFound,     // no such file / no file the caller wants
	ReadError,    // the file could not be opened or read (a share that dropped...)
	Broken,       // not a 7z archive, or a damaged one (a CRC that doesn't match, a cut file)
	Unsupported,  // a method the decoder doesn't have (BZip2, Deflate...)
	Encrypted,    // the file's data, or the archive's names, are encrypted (AES)
	TooBig,       // the file is over the caller's limit, its block over kMaxBlock, or the archive lists too many files
	NoMemory,
};
// For the log: "ok", "broken archive"...
const char* StatusName(Status s);

struct Entry
{
	std::string name; // UTF-8, with its folders ("Games/Kirby.nes")
	uint64_t size = 0;
	bool has_crc = false;
	uint32_t crc = 0;
	bool is_dir = false;
};

class Archive
{
public:
	Archive();
	~Archive();
	Archive(const Archive&) = delete;
	Archive& operator=(const Archive&) = delete;

	// Opens the archive and reads its directory (nothing is decompressed but the directory itself). cancel (when
	// given): once it turns true every read of the archive fails (ReadError), so a decode stops at its next read.
	// An archive with encrypted names is Encrypted; one that lists more than 65,536 files, TooBig.
	Status Open(const std::string& path, const std::atomic<bool>* cancel = nullptr);
	const std::vector<Entry>& Entries() const;
	// Decodes entry i into out. max_size: the biggest file accepted. An encrypted file is Encrypted whatever its size.
	// Everything the decoder allocates for the archive comes out of one budget of kMaxBlock + 72 MiB (the block, the
	// directory, an encoded header, PPMd's model): more is NoMemory.
	Status Extract(size_t i, std::vector<uint8_t>* out, uint64_t max_size);
	// Copies up to `cap` bytes of entry i to dest (*got: how many), decoding its block unless it is the block kept from
	// the last call. The block stays (in the budget) until Release() or the next block.
	Status ExtractTo(size_t i, uint8_t* dest, size_t cap, size_t* got);
	void Release(); // lets go of the kept block
	// The size of the block entry i is in (0 for an empty file or a bad index), whether that block is the one kept,
	// and the kept block's size (0 when none).
	uint64_t BlockBytes(size_t i) const;
	bool Kept(size_t i) const;
	size_t KeptBytes() const;
	// Whether entry i can be decoded at all, without decoding it: Ok, Encrypted, TooBig (its block), NotFound.
	Status Check(size_t i) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};
} // namespace sevenzip
} // namespace fe
