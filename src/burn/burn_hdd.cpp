// Burn - hard disk image module (CHD via libchdr, or raw dumps)
//
// FBNeo PS5 2.2 (the port's changes, see burn_hdd.h):
// - the image is found by the frontend (BurnHDDFindImageHook) and read one hunk at a time through the frontend's file
//   layer (fopen "rb", chd_open_file with CHD_OPEN_READ): never opened for writing, on any path;
// - the .diff (the game's writes) has a v2 header: magic, version, sector size, sector count, the entries, and the id
//   of the disk image it was made for: the CHD header's combined SHA-1 (v5 offset 84, v4 48, v3 80); for an all-zero
//   SHA-1 (v1/v2 CHDs) and raw images, a SHA-1 over the file's size, its first and last MB and 64 KiB samples spread
//   over it. All fields little-endian, byte by byte. A v1 .diff (no id) is still read, never put back;
// - a .diff made for another image is renamed aside to <set>.diff.<first 8 hex of ITS id> (taken: -2, -3...; no id:
//   .unknown), never deleted; a set-aside whose name AND full id match the image being opened is renamed back to
//   <set>.diff (the newest by mtime if several), the current one going aside under its own id;
// - "missing" is stat()'s ENOENT only: any other error checking an existing .diff or set-aside, any rename that fails,
//   a put-back that can't be read back, turns saving off for that disk this session (nothing is written over);
//   renames don't replace (link + unlink where links work, else a rename after stat said ENOENT);
// - writes are atomic: <set>.diff.tmp, fsync'ed (its result checked), renamed over <set>.diff.

#include "burnint.h"
#include "burn_hdd.h"
#include "chd.h"
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <new>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#ifdef __LIBRETRO__
#include <streams/file_stream_transforms.h>
#define hddFseek	fseek
#define hddFtell	ftell
#else
#ifdef _MSC_VER
#define hddFseek	_fseeki64
#define hddFtell	_ftelli64
#else
#define hddFseek	fseeko
#define hddFtell	ftello
#endif
#endif

#ifdef _UNICODE
#define HDD_NARROW_FMT	_T("%hs")
#else
#define HDD_NARROW_FMT	_T("%s")
#endif

#define HDD_CACHE_HUNKS		8
#define HDD_DIFF_MAGIC		"FBNHDIFF"
#define HDD_DIFF_VERSION	2
#define HDD_HDR1_SIZE		20		// v1: magic, version, sector size, entries
#define HDD_HDR2_SIZE		64		// v2: magic, version, sector size, sector count, entries, id[20], 0[20]
#define HDD_ID_SIZE			20

INT32 (*BurnHDDFindImageHook)(INT32 nIndex, const char* szName, char* szPath, INT32 nPathLen) = NULL;
INT32 (*BurnHDDRenameHook)(const char* szFrom, const char* szTo) = NULL;

struct BurnHDD {
	FILE* pFile;
	chd_file* pChd;					// NULL for raw dumps

	UINT32 nSectorSize;
	UINT32 nSectorCount;
	INT32 nCylinders, nHeads, nSectors;	// 0 when unknown

	// CHD hunk cache (direct mapped)
	UINT32 nHunkBytes;
	UINT32 nTotalHunks;
	UINT8* pCache;
	INT32 nCacheTag[HDD_CACHE_HUNKS];

	std::map<UINT32, std::vector<UINT8> > Diff;
	bool bDiffDirty;
	bool bDiffReady;				// the .diff was checked: saving may happen (unless bSaveOff)
	bool bSaveOff;					// saving is off for this disk this session
	UINT32 nGeneration;				// bumped by every write
	UINT32 nReadErrors;
	UINT8 Id[HDD_ID_SIZE];
	char szImage[MAX_PATH];
	TCHAR szDiffPath[MAX_PATH];
	char szDiffDir[MAX_PATH];
	char szDiffName[MAX_PATH];		// "kinst.diff"
	INT32 nReport;					// its entry in the report
};

// every disk open or being opened (BurnHDDCloseAll, BurnHDDFlushAll)
static std::vector<BurnHDD*> OpenDisks;
static std::vector<BurnHDDReport> Reports;

static void Put32(UINT8* p, UINT32 v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static UINT32 Get32(const UINT8* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24);
}

// ---- SHA-1 (the disk's id) ------------------------------------------------------------------------------------------

struct HddSha1 {
	UINT32 h[5];
	UINT64 len;
	UINT8 buf[64];
	UINT32 used;
};

static void Sha1Block(HddSha1* s, const UINT8* p)
{
	UINT32 w[80];
	for (INT32 i = 0; i < 16; i++) w[i] = ((UINT32)p[i * 4] << 24) | ((UINT32)p[i * 4 + 1] << 16) | ((UINT32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (INT32 i = 16; i < 80; i++) { UINT32 t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = (t << 1) | (t >> 31); }
	UINT32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
	for (INT32 i = 0; i < 80; i++) {
		UINT32 f, k;
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else { f = b ^ c ^ d; k = 0xca62c1d6; }
		UINT32 t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
		e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void Sha1Init(HddSha1* s)
{
	s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0;
	s->len = 0;
	s->used = 0;
}

static void Sha1Update(HddSha1* s, const UINT8* p, size_t n)
{
	s->len += n;
	while (n) {
		UINT32 take = 64 - s->used;
		if (take > n) take = (UINT32)n;
		memcpy(s->buf + s->used, p, take);
		s->used += take; p += take; n -= take;
		if (s->used == 64) { Sha1Block(s, s->buf); s->used = 0; }
	}
}

static void Sha1Final(HddSha1* s, UINT8* out)
{
	UINT64 bits = s->len * 8;
	UINT8 pad = 0x80;
	Sha1Update(s, &pad, 1);
	UINT8 zero = 0;
	while (s->used != 56) Sha1Update(s, &zero, 1);
	UINT8 l[8];
	for (INT32 i = 0; i < 8; i++) l[i] = (UINT8)(bits >> (56 - 8 * i));
	Sha1Update(s, l, 8);
	for (INT32 i = 0; i < 5; i++) { out[i * 4] = s->h[i] >> 24; out[i * 4 + 1] = s->h[i] >> 16; out[i * 4 + 2] = s->h[i] >> 8; out[i * 4 + 3] = s->h[i]; }
}

static void IdHex(const UINT8* id, char* out, INT32 nBytes)
{
	static const char* hex = "0123456789abcdef";
	for (INT32 i = 0; i < nBytes; i++) { out[i * 2] = hex[id[i] >> 4]; out[i * 2 + 1] = hex[id[i] & 15]; }
	out[nBytes * 2] = 0;
}

static bool IdZero(const UINT8* id)
{
	for (INT32 i = 0; i < HDD_ID_SIZE; i++) if (id[i]) return false;
	return true;
}

// The id of an image without a SHA-1 of its own: its size, its first and last MB, and 64 KiB at 62 points between.
// Through the frontend's file layer (a share too). Returns 0 on success.
static INT32 SampledId(FILE* fp, UINT8* id)
{
	if (hddFseek(fp, 0, SEEK_END) != 0) return 1;
	INT64 nSize = (INT64)hddFtell(fp);
	if (nSize <= 0) return 1;

	HddSha1 s;
	Sha1Init(&s);
	UINT8 sz[8];
	for (INT32 i = 0; i < 8; i++) sz[i] = (UINT8)((UINT64)nSize >> (8 * i));
	Sha1Update(&s, sz, 8);

	const INT64 kMb = 1 << 20, kSample = 64 << 10;
	std::vector<UINT8> buf((size_t)kMb);	// may throw std::bad_alloc: the caller's disk is registered, the start fails
	struct Span { INT64 at, len; };
	std::vector<Span> spans;
	spans.push_back(Span{0, std::min(kMb, nSize)});
	for (INT32 i = 1; i <= 62; i++) {
		INT64 at = (nSize - kSample) * i / 63;
		if (at < 0) at = 0;
		spans.push_back(Span{at, std::min(kSample, nSize - at)});
	}
	spans.push_back(Span{std::max((INT64)0, nSize - kMb), std::min(kMb, nSize)});
	for (size_t i = 0; i < spans.size(); i++) {
		if (hddFseek(fp, spans[i].at, SEEK_SET) != 0) return 1;
		if (fread(&buf[0], (size_t)spans[i].len, 1, fp) != 1) return 1;
		Sha1Update(&s, &buf[0], (size_t)spans[i].len);
	}
	Sha1Final(&s, id);
	hddFseek(fp, 0, SEEK_SET);
	return 0;
}

// ---- the report ---------------------------------------------------------------------------------------------------

static BurnHDDReport* Report(BurnHDD* pDisk)
{
	if (pDisk->nReport < 0 || pDisk->nReport >= (INT32)Reports.size()) return NULL;
	return &Reports[pDisk->nReport];
}

static void SaveOff(BurnHDD* pDisk, const char* szWhy)
{
	pDisk->bSaveOff = true;
	bprintf(PRINT_ERROR, _T("HDD: saving is off for %s this session: %s\n"), pDisk->szDiffName, szWhy);
	BurnHDDReport* r = Report(pDisk);
	if (r && !r->bSavingOff) {
		r->bSavingOff = 1;
		snprintf(r->szWhy, sizeof(r->szWhy), "%s", szWhy);
	}
}

void BurnHDDReportClear()
{
	Reports.clear();
	for (size_t i = 0; i < OpenDisks.size(); i++) OpenDisks[i]->nReport = -1;
}

INT32 BurnHDDReportCount()
{
	return (INT32)Reports.size();
}

const BurnHDDReport* BurnHDDGetReport(INT32 i)
{
	return (i >= 0 && i < (INT32)Reports.size()) ? &Reports[i] : NULL;
}

// ---- files ----------------------------------------------------------------------------------------------------------

// errno of stat(path): 0 when it is there
static INT32 StatErrno(const char* szPath, struct stat* st)
{
	struct stat tmp;
	if (stat(szPath, st ? st : &tmp) == 0) return 0;
	return errno ? errno : EIO;
}

// Renames without replacing: link + unlink where the file system has links, else a rename once stat said the new
// name is missing. 0 on success; on failure errno says why (EEXIST: the new name is taken).
static INT32 RenameNoReplace(const char* szFrom, const char* szTo)
{
	if (BurnHDDRenameHook && BurnHDDRenameHook(szFrom, szTo)) return 1;
	if (link(szFrom, szTo) == 0) {
		if (unlink(szFrom) == 0) return 0;
		INT32 e = errno;
		unlink(szTo);	// the old name stays the only one
		errno = e;
		return 1;
	}
	if (errno == EEXIST) return 1;
	// no links here (exFAT, FAT...): a rename, once the new name is surely missing
	INT32 e = StatErrno(szTo, NULL);
	if (e == 0) { errno = EEXIST; return 1; }
	if (e != ENOENT) { errno = e; return 1; }
	return rename(szFrom, szTo) == 0 ? 0 : 1;
}

INT32 BurnHDDWriteDiffData(const char* szPath, const UINT8* pData, size_t nLen)
{
	char szTmp[MAX_PATH + 8];
	snprintf(szTmp, sizeof(szTmp), "%s.tmp", szPath);
	FILE* fp = fopen(szTmp, "wb");
	if (fp == NULL) return 1;
	INT32 bOk = (nLen == 0 || fwrite(pData, nLen, 1, fp) == 1);
	bOk = (fflush(fp) == 0) && bOk;
	bOk = (fsync(fileno(fp)) == 0) && bOk;
	bOk = (fclose(fp) == 0) && bOk;
	if (!bOk || rename(szTmp, szPath) != 0) {
		unlink(szTmp);
		return 1;
	}
	return 0;
}

// ---- the .diff --------------------------------------------------------------------------------------------------------

struct DiffHeader {
	INT32 nVersion;		// 0: not a .diff of ours
	UINT32 nSectorSize, nSectorCount, nEntries;
	UINT8 Id[HDD_ID_SIZE];
	INT32 bHasId;
};

// Reads the header. 0: read (h->nVersion 0 when it is not a .diff); otherwise the errno of what failed.
static INT32 ReadDiffHeader(const char* szPath, DiffHeader* h, FILE** pfpOut)
{
	memset(h, 0, sizeof(*h));
	errno = 0;
	FILE* fp = fopen(szPath, "rb");
	if (fp == NULL) return errno ? errno : EIO;
	UINT8 hdr[HDD_HDR2_SIZE];
	size_t got = fread(hdr, 1, sizeof(hdr), fp);
	if (ferror(fp)) { fclose(fp); return EIO; }
	if (got >= HDD_HDR1_SIZE && memcmp(hdr, HDD_DIFF_MAGIC, 8) == 0) {
		UINT32 v = Get32(hdr + 8);
		if (v == 1) {
			h->nVersion = 1;
			h->nSectorSize = Get32(hdr + 12);
			h->nEntries = Get32(hdr + 16);
			hddFseek(fp, HDD_HDR1_SIZE, SEEK_SET);
		} else if (v == 2 && got == HDD_HDR2_SIZE) {
			h->nVersion = 2;
			h->nSectorSize = Get32(hdr + 12);
			h->nSectorCount = Get32(hdr + 16);
			h->nEntries = Get32(hdr + 20);
			memcpy(h->Id, hdr + 24, HDD_ID_SIZE);
			h->bHasId = !IdZero(h->Id);
		}
	}
	if (pfpOut && h->nVersion) *pfpOut = fp; else fclose(fp);
	return 0;
}

// The entries after the header. 0 on success (the map then holds them); otherwise errno.
static INT32 ReadDiffEntries(BurnHDD* pDisk, FILE* fp, const DiffHeader* h)
{
	std::vector<UINT8> entry(4 + pDisk->nSectorSize);
	for (UINT32 i = 0; i < h->nEntries; i++) {
		if (fread(&entry[0], entry.size(), 1, fp) != 1) {
			INT32 e = ferror(fp) ? EIO : 0;
			if (e) return e;
			bprintf(PRINT_ERROR, _T("HDD: %s ends early (%d of %d entries)\n"), pDisk->szDiffName, i, h->nEntries);
			break;
		}
		UINT32 nLba = Get32(&entry[0]);
		if (nLba >= pDisk->nSectorCount) continue;
		pDisk->Diff[nLba].assign(entry.begin() + 4, entry.end());
	}
	return 0;
}

static bool DiffIsMine(BurnHDD* pDisk, const DiffHeader* h)
{
	if (h->nVersion == 2)
		return h->bHasId && memcmp(h->Id, pDisk->Id, HDD_ID_SIZE) == 0 && h->nSectorSize == pDisk->nSectorSize &&
			h->nSectorCount == pDisk->nSectorCount;
	if (h->nVersion == 1)
		return h->nSectorSize == pDisk->nSectorSize;	// no id: taken for this disk, as up to 2.1
	return false;
}

// "<dir>/<name>.diff.<8 hex>" (and -2, -3... when taken), or ".unknown" for a .diff without an id. Returns 0 with the
// free name; otherwise errno (a name that can't be checked).
static INT32 FreeAsideName(BurnHDD* pDisk, const DiffHeader* h, char* szOut, INT32 nLen)
{
	char tag[24];
	if (h->nVersion == 2 && h->bHasId) {
		char hex[HDD_ID_SIZE * 2 + 1];
		IdHex(h->Id, hex, 4);
		snprintf(tag, sizeof(tag), "%s", hex);
	} else {
		snprintf(tag, sizeof(tag), "unknown");
	}
	for (INT32 n = 1; n < 1000; n++) {
		if (n == 1) snprintf(szOut, nLen, "%s.%s", pDisk->szDiffPath, tag);
		else snprintf(szOut, nLen, "%s.%s-%d", pDisk->szDiffPath, tag, n);
		INT32 e = StatErrno(szOut, NULL);
		if (e == ENOENT) return 0;
		if (e != 0) return e;
	}
	return EEXIST;
}

// Loads <set>.diff, setting a .diff of another image aside and putting back one of this image's.
static void OpenDiff(BurnHDD* pDisk)
{
	char szIdHex[HDD_ID_SIZE * 2 + 1];
	IdHex(pDisk->Id, szIdHex, HDD_ID_SIZE);
	BurnHDDReport* r = Report(pDisk);
	char szWhy[400];

	struct stat st;
	INT32 e = StatErrno(pDisk->szDiffPath, &st);
	bool bHaveDiff = false;
	if (e == 0) {
		DiffHeader h;
		FILE* fp = NULL;
		e = ReadDiffHeader(pDisk->szDiffPath, &h, &fp);
		if (e) {
			snprintf(szWhy, sizeof(szWhy), "%s could not be read (%s)", pDisk->szDiffName, strerror(e));
			SaveOff(pDisk, szWhy);
			return;
		}
		if (DiffIsMine(pDisk, &h)) {
			e = ReadDiffEntries(pDisk, fp, &h);
			fclose(fp);
			if (e) {
				pDisk->Diff.clear();
				snprintf(szWhy, sizeof(szWhy), "%s could not be read (%s)", pDisk->szDiffName, strerror(e));
				SaveOff(pDisk, szWhy);
				return;
			}
			bprintf(PRINT_IMPORTANT, _T("HDD: %d modified sectors loaded from %s%s\n"), (INT32)pDisk->Diff.size(), pDisk->szDiffPath,
				h.nVersion == 1 ? _T(" (a .diff without the disk's id: taken for this one)") : _T(""));
			pDisk->bDiffReady = true;
			return;
		}
		if (fp) fclose(fp);
		// another image's (or not a .diff at all): set aside under its own id
		char szAside[MAX_PATH + 32];
		e = FreeAsideName(pDisk, &h, szAside, sizeof(szAside));
		if (e || RenameNoReplace(pDisk->szDiffPath, szAside)) {
			INT32 err = e ? e : errno;
			snprintf(szWhy, sizeof(szWhy), "an old %s (made for another disk image) could not be set aside (%s)", pDisk->szDiffName, strerror(err));
			if (r) r->bSetAsideFailed = 1;
			SaveOff(pDisk, szWhy);
			return;
		}
		bprintf(PRINT_IMPORTANT, _T("HDD: %s was made for another disk image: set aside as %s\n"), pDisk->szDiffName, szAside);
		if (r) { r->bSetAside = 1; snprintf(r->szSetAside, sizeof(r->szSetAside), "%s", szAside); }
		bHaveDiff = false;
	} else if (e != ENOENT) {
		snprintf(szWhy, sizeof(szWhy), "%s could not be checked (%s)", pDisk->szDiffName, strerror(e));
		SaveOff(pDisk, szWhy);
		return;
	}
	(void)bHaveDiff;

	// <set>.diff is missing now: a set-aside of this image's goes back (its name AND its full id must match)
	char szPrefix[MAX_PATH + 16];
	char hex8[9];
	memcpy(hex8, szIdHex, 8);
	hex8[8] = 0;
	snprintf(szPrefix, sizeof(szPrefix), "%s.%s", pDisk->szDiffName, hex8);
	const size_t nPrefix = strlen(szPrefix);
	errno = 0;
	DIR* d = opendir(pDisk->szDiffDir);
	if (d == NULL) {
		e = errno ? errno : EIO;
		if (e == ENOENT) { pDisk->bDiffReady = true; return; }	// no saves folder yet: nothing to put back
		snprintf(szWhy, sizeof(szWhy), "the saves folder could not be read (%s)", strerror(e));
		SaveOff(pDisk, szWhy);
		return;
	}
	std::vector<std::string> names;
	while (struct dirent* de = readdir(d)) {
		const char* n = de->d_name;
		if (strncmp(n, szPrefix, nPrefix) != 0) continue;
		const char* rest = n + nPrefix;
		bool ok = (*rest == 0);
		if (!ok && rest[0] == '-' && rest[1] >= '1' && rest[1] <= '9') {
			ok = true;
			for (const char* c = rest + 1; *c; c++) if (*c < '0' || *c > '9') ok = false;
		}
		if (ok) names.push_back(n);
	}
	closedir(d);
	std::sort(names.begin(), names.end());

	std::string best;
	time_t best_mtime = 0;
	INT32 nMatches = 0;
	for (size_t i = 0; i < names.size(); i++) {
		const std::string path = std::string(pDisk->szDiffDir) + "/" + names[i];
		struct stat ast;
		e = StatErrno(path.c_str(), &ast);
		if (e == ENOENT) continue;
		DiffHeader h;
		if (e == 0) e = ReadDiffHeader(path.c_str(), &h, NULL);
		if (e) {
			snprintf(szWhy, sizeof(szWhy), "%s could not be checked (%s)", names[i].c_str(), strerror(e));
			if (r) r->bPutBackFailed = 1;
			SaveOff(pDisk, szWhy);
			return;
		}
		if (h.nVersion != 2 || !h.bHasId || memcmp(h.Id, pDisk->Id, HDD_ID_SIZE) != 0 || h.nSectorSize != pDisk->nSectorSize) continue;
		nMatches++;
		if (best.empty() || ast.st_mtime > best_mtime) {
			best = path;
			best_mtime = ast.st_mtime;
		}
	}
	if (best.empty()) {
		pDisk->bDiffReady = true;
		return;
	}
	if (nMatches > 1)
		bprintf(PRINT_IMPORTANT, _T("HDD: %d set-aside .diff files of this disk image: the newest goes back, %s\n"), nMatches, best.c_str());
	if (r) r->nSetAsideCount = nMatches;
	if (RenameNoReplace(best.c_str(), pDisk->szDiffPath)) {
		snprintf(szWhy, sizeof(szWhy), "%s could not be put back (%s)", best.c_str(), strerror(errno));
		if (r) r->bPutBackFailed = 1;
		SaveOff(pDisk, szWhy);
		return;
	}
	DiffHeader h;
	FILE* fp = NULL;
	e = ReadDiffHeader(pDisk->szDiffPath, &h, &fp);
	if (e == 0 && !DiffIsMine(pDisk, &h)) e = EINVAL;
	if (e == 0) e = ReadDiffEntries(pDisk, fp, &h);
	if (fp) fclose(fp);
	if (e) {
		pDisk->Diff.clear();
		snprintf(szWhy, sizeof(szWhy), "%s was put back but could not be read (%s)", pDisk->szDiffName, strerror(e));
		if (r) r->bPutBackFailed = 1;
		SaveOff(pDisk, szWhy);
		return;
	}
	bprintf(PRINT_IMPORTANT, _T("HDD: %s put back (%d modified sectors of this disk image)\n"), best.c_str(), (INT32)pDisk->Diff.size());
	if (r) { r->bPutBack = 1; snprintf(r->szPutBack, sizeof(r->szPutBack), "%s", best.c_str()); }
	pDisk->bDiffReady = true;
}

// ---- opening ----------------------------------------------------------------------------------------------------------

// the driver gives the disk name without extension
static FILE* OpenInDir(const TCHAR* szDir, const char* szName, const TCHAR* szExt, TCHAR* szPathOut)
{
	if (szDir == NULL || szDir[0] == 0) return NULL;

	_stprintf(szPathOut, _T("%s%s/") HDD_NARROW_FMT _T("%s"), szAppHDDPath, szDir, szName, szExt);
	return _tfopen(szPathOut, _T("rb"));
}

static INT32 OpenChd(BurnHDD* pDisk)
{
	// the combined SHA-1 the header carries (v3: offset 80, v4: 48, v5: 84), read before libchdr takes the file
	UINT8 raw[124];
	memset(raw, 0, sizeof(raw));
	hddFseek(pDisk->pFile, 0, SEEK_SET);
	size_t got = fread(raw, 1, sizeof(raw), pDisk->pFile);
	UINT32 nVersion = got >= 16 ? ((UINT32)raw[12] << 24 | (UINT32)raw[13] << 16 | (UINT32)raw[14] << 8 | raw[15]) : 0;
	INT32 nShaAt = nVersion == 5 ? 84 : nVersion == 4 ? 48 : nVersion == 3 ? 80 : -1;
	if (nShaAt >= 0 && got >= (size_t)nShaAt + HDD_ID_SIZE) memcpy(pDisk->Id, raw + nShaAt, HDD_ID_SIZE);
	hddFseek(pDisk->pFile, 0, SEEK_SET);

	if (chd_open_file(pDisk->pFile, CHD_OPEN_READ, NULL, &pDisk->pChd) != CHDERR_NONE) {
		pDisk->pChd = NULL;
		return 1;
	}

	const chd_header* pHeader = chd_get_header(pDisk->pChd);
	char szMeta[256];
	UINT32 nMetaLen = 0;

	if (pHeader == NULL || pHeader->hunkbytes == 0 ||
		chd_get_metadata(pDisk->pChd, HARD_DISK_METADATA_TAG, 0, szMeta, sizeof(szMeta) - 1, &nMetaLen, NULL, NULL) != CHDERR_NONE) {
		bprintf(PRINT_ERROR, _T("HDD: the CHD is not a hard disk image\n"));
		return 1;
	}
	szMeta[nMetaLen < sizeof(szMeta) ? nMetaLen : sizeof(szMeta) - 1] = 0;

	INT32 nCyls = 0, nHeads = 0, nSecs = 0, nBps = 0;
	if (sscanf(szMeta, HARD_DISK_METADATA_FORMAT, &nCyls, &nHeads, &nSecs, &nBps) != 4 || nBps <= 0 || pHeader->hunkbytes % nBps) {
		bprintf(PRINT_ERROR, _T("HDD: bad CHD geometry\n"));
		return 1;
	}

	pDisk->nCylinders = nCyls;
	pDisk->nHeads = nHeads;
	pDisk->nSectors = nSecs;
	pDisk->nSectorSize = nBps;
	pDisk->nSectorCount = (UINT32)(pHeader->logicalbytes / nBps);
	pDisk->nHunkBytes = pHeader->hunkbytes;
	pDisk->nTotalHunks = pHeader->totalhunks;

	pDisk->pCache = (UINT8*)malloc((size_t)pDisk->nHunkBytes * HDD_CACHE_HUNKS);
	if (pDisk->pCache == NULL) return 1;
	for (INT32 i = 0; i < HDD_CACHE_HUNKS; i++) pDisk->nCacheTag[i] = -1;

	// a v1/v2 CHD has no SHA-1 of the whole: its id is sampled from the file
	if (IdZero(pDisk->Id) && SampledId(pDisk->pFile, pDisk->Id)) return 1;

	return 0;
}

static INT32 OpenRaw(BurnHDD* pDisk)
{
	hddFseek(pDisk->pFile, 0, SEEK_END);
	INT64 nSize = (INT64)hddFtell(pDisk->pFile);
	hddFseek(pDisk->pFile, 0, SEEK_SET);

	pDisk->nSectorSize = 512;
	pDisk->nSectorCount = (UINT32)(nSize / 512);
	if (pDisk->nSectorCount == 0) return 1;

	return SampledId(pDisk->pFile, pDisk->Id);
}

static void Unregister(BurnHDD* pDisk)
{
	for (size_t i = 0; i < OpenDisks.size(); i++) {
		if (OpenDisks[i] == pDisk) {
			OpenDisks.erase(OpenDisks.begin() + i);
			return;
		}
	}
}

static void FreeDisk(BurnHDD* pDisk)
{
	Unregister(pDisk);
	if (pDisk->pChd) chd_close(pDisk->pChd);
	if (pDisk->pFile) fclose(pDisk->pFile);		// libchdr does not close a FILE* it was given
	free(pDisk->pCache);
	delete pDisk;
}

BurnHDD* BurnHDDOpen(INT32 nIndex)
{
	char* szName = NULL;
	if (BurnDrvGetHDDName(&szName, nIndex, 0) || szName == NULL) return NULL;

	TCHAR szPath[MAX_PATH];
	FILE* fp = NULL;
	if (BurnHDDFindImageHook) {
		INT32 rc = BurnHDDFindImageHook(nIndex, szName, szPath, MAX_PATH);
		if (rc == 0) {
			fp = _tfopen(szPath, _T("rb"));		// read only, always
			if (fp == NULL) bprintf(PRINT_ERROR, _T("HDD: can't open %s (%s)\n"), szPath, strerror(errno));
		}
		if (fp == NULL) {
			if (rc == 2) bprintf(PRINT_ERROR, _T("HDD: ") HDD_NARROW_FMT _T(": the image's path is too long\n"), szName);
			else bprintf(PRINT_ERROR, _T("HDD: ") HDD_NARROW_FMT _T(".chd not found\n"), szName);
			return NULL;
		}
	} else {
		static const TCHAR* szExts[] = { _T(".chd"), _T(".img") };
		for (INT32 i = 0; i < 2 && fp == NULL; i++) {
			fp = OpenInDir(BurnDrvGetText(DRV_NAME), szName, szExts[i], szPath);
			if (fp == NULL) fp = OpenInDir(BurnDrvGetText(DRV_PARENT), szName, szExts[i], szPath);
		}
		if (fp == NULL) {
			bprintf(PRINT_ERROR, _T("HDD: ") HDD_NARROW_FMT _T(".chd not found\n"), szName);
			return NULL;
		}
	}

	BurnHDD* pDisk = NULL;
	try {
		pDisk = new BurnHDD;
	} catch (...) {
		fclose(fp);
		throw;
	}
	// registered at once: an allocation that fails below leaves a half-open disk BurnHDDCloseAll still finds
	pDisk->pFile = fp;
	pDisk->pChd = NULL;
	pDisk->nSectorSize = pDisk->nSectorCount = 0;
	pDisk->nCylinders = pDisk->nHeads = pDisk->nSectors = 0;
	pDisk->nHunkBytes = pDisk->nTotalHunks = 0;
	pDisk->pCache = NULL;
	pDisk->bDiffDirty = false;
	pDisk->bDiffReady = false;
	pDisk->bSaveOff = false;
	pDisk->nGeneration = 0;
	pDisk->nReadErrors = 0;
	pDisk->nReport = -1;
	memset(pDisk->Id, 0, sizeof(pDisk->Id));
	snprintf(pDisk->szImage, sizeof(pDisk->szImage), "%s", szPath);
	pDisk->szDiffPath[0] = pDisk->szDiffDir[0] = pDisk->szDiffName[0] = 0;
	try {
		OpenDisks.push_back(pDisk);
	} catch (...) {
		fclose(fp);
		delete pDisk;
		throw;
	}

	BurnHDDReport rep;
	memset(&rep, 0, sizeof(rep));
	snprintf(rep.szDisk, sizeof(rep.szDisk), "%s", szName);
	snprintf(rep.szImage, sizeof(rep.szImage), "%s", szPath);
	Reports.push_back(rep);
	pDisk->nReport = (INT32)Reports.size() - 1;

	// a CHD is recognised by its signature, whatever the file extension
	char sig[8] = { 0 };
	bool bChd = (fread(sig, 8, 1, fp) == 1 && memcmp(sig, "MComprHD", 8) == 0);
	hddFseek(fp, 0, SEEK_SET);

	if (bChd ? OpenChd(pDisk) : OpenRaw(pDisk)) {
		bprintf(PRINT_ERROR, _T("HDD: can't use %s\n"), szPath);
		FreeDisk(pDisk);
		return NULL;
	}

	char szIdHex[HDD_ID_SIZE * 2 + 1];
	IdHex(pDisk->Id, szIdHex, HDD_ID_SIZE);
	if (bChd) {
		struct BurnHDDInfo hi;
		memset(&hi, 0, sizeof(hi));
		if (BurnDrvGetHDDInfo(&hi, nIndex) == 0 && hi.szSha1 && hi.szSha1[0] && strcasecmp(hi.szSha1, szIdHex) != 0) {
			bprintf(PRINT_ERROR, _T("HDD: warning: %s has SHA-1 %s, the driver names %s (another dump or version of the disk?)\n"),
				szPath, szIdHex, hi.szSha1);
			if (BurnHDDReport* r = Report(pDisk)) r->bShaMismatch = 1;
		}
	}

	const char* szSet = BurnDrvGetTextA(DRV_NAME);
	snprintf(pDisk->szDiffDir, sizeof(pDisk->szDiffDir), "%s", szAppEEPROMPath);
	size_t nDir = strlen(pDisk->szDiffDir);
	while (nDir > 1 && pDisk->szDiffDir[nDir - 1] == '/') pDisk->szDiffDir[--nDir] = 0;
	if (nIndex == 0) snprintf(pDisk->szDiffName, sizeof(pDisk->szDiffName), "%s.diff", szSet);
	else snprintf(pDisk->szDiffName, sizeof(pDisk->szDiffName), "%s_%d.diff", szSet, nIndex);
	snprintf(pDisk->szDiffPath, sizeof(pDisk->szDiffPath), "%s/%s", pDisk->szDiffDir, pDisk->szDiffName);
	if (BurnHDDReport* r = Report(pDisk)) snprintf(r->szDiff, sizeof(r->szDiff), "%s", pDisk->szDiffPath);
	OpenDiff(pDisk);

	bprintf(PRINT_IMPORTANT, _T("HDD: %s, %s, %d sectors of %d bytes, id %s\n"), szPath, bChd ? _T("CHD") : _T("raw"),
		pDisk->nSectorCount, pDisk->nSectorSize, szIdHex);

	return pDisk;
}

// ---- saving ---------------------------------------------------------------------------------------------------------

static void BuildDiff(BurnHDD* pDisk, std::vector<UINT8>* out)
{
	out->clear();
	out->reserve(HDD_HDR2_SIZE + pDisk->Diff.size() * (4 + pDisk->nSectorSize));
	UINT8 hdr[HDD_HDR2_SIZE];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, HDD_DIFF_MAGIC, 8);
	Put32(hdr + 8, HDD_DIFF_VERSION);
	Put32(hdr + 12, pDisk->nSectorSize);
	Put32(hdr + 16, pDisk->nSectorCount);
	Put32(hdr + 20, (UINT32)pDisk->Diff.size());
	memcpy(hdr + 24, pDisk->Id, HDD_ID_SIZE);
	out->insert(out->end(), hdr, hdr + sizeof(hdr));
	UINT8 lba[4];
	for (std::map<UINT32, std::vector<UINT8> >::iterator it = pDisk->Diff.begin(); it != pDisk->Diff.end(); ++it) {
		Put32(lba, it->first);
		out->insert(out->end(), lba, lba + 4);
		out->insert(out->end(), it->second.begin(), it->second.end());
	}
}

INT32 BurnHDDFlush(BurnHDD* pDisk)
{
	if (pDisk == NULL) return 1;
	if (!pDisk->bDiffDirty || !pDisk->bDiffReady || pDisk->bSaveOff) return 0;

	std::vector<UINT8> data;
	BuildDiff(pDisk, &data);
	const UINT32 nGen = pDisk->nGeneration;
	if (BurnHDDWriteDiffData(pDisk->szDiffPath, &data[0], data.size())) {
		bprintf(PRINT_ERROR, _T("HDD: can't write %s\n"), pDisk->szDiffPath);
		return 1;
	}
	if (pDisk->nGeneration == nGen) pDisk->bDiffDirty = false;
	return 0;
}

INT32 BurnHDDTakeSnapshot(BurnHDD* pDisk, BurnHDDSnapshot* pOut)
{
	if (pDisk == NULL || pOut == NULL || !pDisk->bDiffDirty || !pDisk->bDiffReady || pDisk->bSaveOff) return 0;
	BuildDiff(pDisk, &pOut->data);
	pOut->pDisk = pDisk;
	snprintf(pOut->szPath, sizeof(pOut->szPath), "%s", pDisk->szDiffPath);
	pOut->nGeneration = pDisk->nGeneration;
	return 1;
}

void BurnHDDSnapshotDone(const BurnHDDSnapshot* pSnap, INT32 bOk)
{
	if (pSnap == NULL || pSnap->pDisk == NULL) return;
	BurnHDD* pDisk = pSnap->pDisk;
	if (std::find(OpenDisks.begin(), OpenDisks.end(), pDisk) == OpenDisks.end()) return;	// closed meanwhile
	if (bOk && pDisk->nGeneration == pSnap->nGeneration) pDisk->bDiffDirty = false;
}

void BurnHDDClose(BurnHDD* pDisk)
{
	if (pDisk == NULL) return;
	if (std::find(OpenDisks.begin(), OpenDisks.end(), pDisk) == OpenDisks.end()) return;	// closed already

	BurnHDDFlush(pDisk);
	FreeDisk(pDisk);
}

INT32 BurnHDDFlushAll()
{
	INT32 nRet = 0;
	for (size_t i = 0; i < OpenDisks.size(); i++) nRet |= BurnHDDFlush(OpenDisks[i]);
	return nRet;
}

void BurnHDDCloseAll()
{
	while (!OpenDisks.empty()) BurnHDDClose(OpenDisks.back());
}

INT32 BurnHDDOpenCount()
{
	return (INT32)OpenDisks.size();
}

BurnHDD* BurnHDDGetOpen(INT32 i)
{
	return (i >= 0 && i < (INT32)OpenDisks.size()) ? OpenDisks[i] : NULL;
}

INT32 BurnHDDIsDirty(BurnHDD* pDisk)
{
	return pDisk && pDisk->bDiffDirty ? 1 : 0;
}

INT32 BurnHDDSavingOff(BurnHDD* pDisk)
{
	return pDisk && pDisk->bSaveOff ? 1 : 0;
}

UINT32 BurnHDDReadErrors(BurnHDD* pDisk)
{
	return pDisk ? pDisk->nReadErrors : 0;
}

const char* BurnHDDImagePath(BurnHDD* pDisk)
{
	return pDisk ? pDisk->szImage : "";
}

UINT32 BurnHDDGetSectorSize(BurnHDD* pDisk)
{
	return pDisk ? pDisk->nSectorSize : 0;
}

UINT32 BurnHDDGetSectorCount(BurnHDD* pDisk)
{
	return pDisk ? pDisk->nSectorCount : 0;
}

INT32 BurnHDDGetGeometry(BurnHDD* pDisk, INT32* pnCylinders, INT32* pnHeads, INT32* pnSectors)
{
	if (pDisk == NULL || pDisk->nCylinders == 0) return 1;

	if (pnCylinders) *pnCylinders = pDisk->nCylinders;
	if (pnHeads) *pnHeads = pDisk->nHeads;
	if (pnSectors) *pnSectors = pDisk->nSectors;
	return 0;
}

static INT32 ReadImageSector(BurnHDD* pDisk, UINT32 nLba, UINT8* pDest)
{
	if (pDisk->pChd == NULL) {
		if (hddFseek(pDisk->pFile, (INT64)nLba * pDisk->nSectorSize, SEEK_SET) != 0) return 1;
		return (fread(pDest, pDisk->nSectorSize, 1, pDisk->pFile) != 1);
	}

	UINT64 nOffset = (UINT64)nLba * pDisk->nSectorSize;
	UINT32 nHunk = (UINT32)(nOffset / pDisk->nHunkBytes);
	if (nHunk >= pDisk->nTotalHunks) return 1;

	INT32 nSlot = nHunk % HDD_CACHE_HUNKS;
	UINT8* pHunk = pDisk->pCache + (size_t)nSlot * pDisk->nHunkBytes;

	if (pDisk->nCacheTag[nSlot] != (INT32)nHunk) {
		if (chd_read(pDisk->pChd, nHunk, pHunk) != CHDERR_NONE) {
			pDisk->nCacheTag[nSlot] = -1;
			return 1;
		}
		pDisk->nCacheTag[nSlot] = nHunk;
	}

	memcpy(pDest, pHunk + (nOffset % pDisk->nHunkBytes), pDisk->nSectorSize);
	return 0;
}

INT32 BurnHDDRead(BurnHDD* pDisk, UINT32 nLba, UINT32 nCount, void* pDest)
{
	if (pDisk == NULL || pDest == NULL) return 1;

	UINT8* pDst = (UINT8*)pDest;
	INT32 nRet = 0;

	for (UINT32 i = 0; i < nCount; i++, nLba++, pDst += pDisk->nSectorSize) {
		std::map<UINT32, std::vector<UINT8> >::iterator it = pDisk->Diff.find(nLba);
		if (it != pDisk->Diff.end()) {
			memcpy(pDst, &it->second[0], pDisk->nSectorSize);
			continue;
		}

		if (nLba >= pDisk->nSectorCount) {
			memset(pDst, 0, pDisk->nSectorSize);
			nRet = 1;
		} else if (ReadImageSector(pDisk, nLba, pDst)) {
			memset(pDst, 0, pDisk->nSectorSize);
			pDisk->nReadErrors++;	// the frontend says so once (a share that dropped)
			nRet = 1;
		}
	}

	return nRet;
}

INT32 BurnHDDWrite(BurnHDD* pDisk, UINT32 nLba, UINT32 nCount, const void* pSrc)
{
	if (pDisk == NULL || pSrc == NULL) return 1;

	const UINT8* pS = (const UINT8*)pSrc;
	INT32 nRet = 0;

	for (UINT32 i = 0; i < nCount; i++, nLba++, pS += pDisk->nSectorSize) {
		if (nLba >= pDisk->nSectorCount) {
			nRet = 1;
			continue;
		}
		pDisk->Diff[nLba].assign(pS, pS + pDisk->nSectorSize);
		pDisk->bDiffDirty = true;
		pDisk->nGeneration++;
	}

	return nRet;
}

void BurnHDDDiscardWrites(BurnHDD* pDisk)
{
	if (pDisk == NULL) return;

	pDisk->Diff.clear();
	pDisk->bDiffDirty = true;	// an empty diff file replaces the old one
	pDisk->nGeneration++;
	BurnHDDFlush(pDisk);
}
