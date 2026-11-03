// Burn - hard disk image module
//
// Gives drivers sector access to the hard disks listed by their GetHDDInfo/GetHDDName functions.
// Images can be MAME CHDs (hard disk type, compressed or not) or raw dumps, found in
// <szAppHDDPath><romset>/ then <szAppHDDPath><parent>/ -- or wherever the frontend's BurnHDDFindImageHook says.
// The image files are never written: sectors written by the game are kept in memory and saved in
// <szAppEEPROMPath><romset>.diff (<romset>_<n>.diff for disk n > 0), reloaded on the next run.
//
// FBNeo PS5 (2.2): the .diff names the disk it was made for (its header carries the image's SHA-1, v2); a .diff of
// another disk image is set aside (renamed, never deleted) and put back when that image is used again; saving is
// turned off for the session whenever the files can't be checked or renamed for sure; writes are atomic. See
// burn_hdd.cpp for the rules.

#ifndef BURN_HDD_H
#define BURN_HDD_H

#include <vector>

struct BurnHDD;

// Opens disk nIndex of the running driver. Returns NULL on failure.
BurnHDD* BurnHDDOpen(INT32 nIndex);

// Saves the diff file (if anything was written) and frees everything. Also takes a disk that is only half open (an
// allocation failed while it was being opened: BurnHDDCloseAll finds it).
void BurnHDDClose(BurnHDD* pDisk);

// Writes the diff file now, returns 0 on success (also done by BurnHDDClose). Nothing to write, or saving off: 0.
INT32 BurnHDDFlush(BurnHDD* pDisk);

UINT32 BurnHDDGetSectorSize(BurnHDD* pDisk);
UINT32 BurnHDDGetSectorCount(BurnHDD* pDisk);

// CHS geometry stored in a CHD (GDDD metadata). Returns 1 when the image has none (raw dumps).
INT32 BurnHDDGetGeometry(BurnHDD* pDisk, INT32* pnCylinders, INT32* pnHeads, INT32* pnSectors);

// Sector access, returns 0 on success. Out of range sectors read as zeroes and fail.
INT32 BurnHDDRead(BurnHDD* pDisk, UINT32 nLba, UINT32 nCount, void* pDest);
INT32 BurnHDDWrite(BurnHDD* pDisk, UINT32 nLba, UINT32 nCount, const void* pSrc);

// Forgets every write (back to the pristine image): an empty diff file replaces the old one.
void BurnHDDDiscardWrites(BurnHDD* pDisk);

// ---- FBNeo PS5 (2.2) ---------------------------------------------------------------------------------------------

// The frontend's lookup of disk nIndex's image (szName: the driver's name for it, "kinst"). Returns 0 with the path in
// szPath (nPathLen bytes), 1 when there is none, 2 when it would be found at a path that is too long. NULL: the
// lookup above (szAppHDDPath).
extern INT32 (*BurnHDDFindImageHook)(INT32 nIndex, const char* szName, char* szPath, INT32 nPathLen);

// Test-only (NULL unless a test sets it): called before each rename the module makes (the .diff set aside or put
// back); a non-zero return is a failed rename (errno as the hook left it).
extern INT32 (*BurnHDDRenameHook)(const char* szFrom, const char* szTo);

// Test-only (NULL unless a test sets it): called in place of fsync() on the .diff's .tmp; non-zero is a failed fsync.
extern INT32 (*BurnHDDSyncHook)(INT32 nFd);

// The last image the lookup found that could not be used, since BurnHDDReportClear: 0 none, 1 it could not be opened,
// 2 it is not a hard disk image FBNeo can read (not a hard disk CHD, damaged). *pszPath: its path.
INT32 BurnHDDOpenFailure(const char** pszPath);

// Every disk open (or half open) now.
INT32 BurnHDDFlushAll();		// BurnHDDFlush of each; 0 when all succeeded
void BurnHDDCloseAll();			// BurnHDDClose of each (a start that failed: the driver may not have closed them)
INT32 BurnHDDOpenCount();
BurnHDD* BurnHDDGetOpen(INT32 i);

// The periodic save (the frontend's): the game's thread takes the file's bytes (only when there are changes not
// written yet, and saving is on), another thread writes them with BurnHDDWriteDiffData (it touches no disk state),
// then the game's thread says how it went. Writes since the snapshot keep the disk dirty.
struct BurnHDDSnapshot {
	BurnHDD* pDisk;
	std::vector<UINT8> data;	// the whole .diff file
	char szPath[MAX_PATH];
	UINT32 nGeneration;
};
INT32 BurnHDDTakeSnapshot(BurnHDD* pDisk, BurnHDDSnapshot* pOut);	// 1: something to write (pOut filled)
void BurnHDDSnapshotDone(const BurnHDDSnapshot* pSnap, INT32 bOk);
// <path>.tmp, written, fsync'ed (its result checked), closed, then renamed over <path>. 0 on success. Any thread.
INT32 BurnHDDWriteDiffData(const char* szPath, const UINT8* pData, size_t nLen);

// Dirty: changes not written yet. SavingOff: this disk's changes are not saved this session (see szWhy below).
INT32 BurnHDDIsDirty(BurnHDD* pDisk);
INT32 BurnHDDSavingOff(BurnHDD* pDisk);
// Sectors of the image that could not be read (a network share that dropped...), since it was opened.
UINT32 BurnHDDReadErrors(BurnHDD* pDisk);
// The image's file, for the frontend's messages.
const char* BurnHDDImagePath(BurnHDD* pDisk);

// What happened to the saved disk changes while the disks were opened, since the last BurnHDDReportClear (the
// frontend clears it before each start and reads it after).
struct BurnHDDReport {
	char szDisk[64];			// the driver's name for the disk ("kinst")
	char szImage[MAX_PATH];		// the image used
	char szDiff[MAX_PATH];		// its .diff
	INT32 bPutBack;				// a .diff set aside earlier for this image was put back (szPutBack: its name)
	INT32 bSetAside;			// a .diff of another image was set aside (szSetAside: its new name)
	INT32 bSetAsideFailed;		// an old .diff could not be set aside: saving is off
	INT32 bPutBackFailed;		// this disk's saved changes could not be put back: saving is off
	INT32 bSavingOff;			// saving is off for this disk this session (szWhy)
	INT32 bShaMismatch;			// the CHD's SHA-1 is not the one the driver names
	INT32 nSetAsideCount;		// matching set-asides found (more than one: the newest was put back)
	char szSetAside[MAX_PATH];
	char szPutBack[MAX_PATH];
	char szWhy[256];
};
void BurnHDDReportClear();
INT32 BurnHDDReportCount();
const BurnHDDReport* BurnHDDGetReport(INT32 i);

#endif
