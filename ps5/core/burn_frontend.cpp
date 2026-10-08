// FBNeo PS5: what FBNeo's core takes from its frontend (FBNeo's own frontends define these in src/burner and
// src/intf), for an arcade-only build with no IPS patches, no Neo Geo CD, no netplay and no recordings.
// The paths are set by fe_burn.cpp (burn::Init); the rest are the frontends' neutral values.
//
// SPDX-License-Identifier: MIT

#include "burnint.h"
#include "cd_interface.h"
#include "neocdlist.h"

#include <stdlib.h>
#include <string.h>

// folders (each ends with '/'), set by burn::Init
TCHAR szAppHiscorePath[MAX_PATH] = "";
TCHAR szAppSamplesPath[MAX_PATH] = "";
TCHAR szAppHDDPath[MAX_PATH] = "";
TCHAR szAppBlendPath[MAX_PATH] = "";
TCHAR szAppEEPROMPath[MAX_PATH] = "";

// the driver is running (cheat.cpp), no pause between frames (cps_run.cpp, lowpass2.cpp)
int bDrvOkay = 0;
INT32 bRunPause = 0;
INT32 nInputIntfMouseDivider = 1;

// ROM data files (romdata.cpp) and IPS patches (ips_manager.cpp): not used by this port
RomDataInfo* pRDI = NULL;
BurnRomInfo* pDataRomDesc = NULL;
bool bDoIpsPatch = false;
UINT32 nIpsDrvDefine = 0, nIpsMemExpLen[SND2_ROM + 1] = {0};
void IpsApplyPatches(UINT8*, char*, UINT32, bool) {}

// the date and time recordings replay (burn.cpp reads it only while one plays)
struct MovieExtInfo
{
	UINT32 year, month;
	UINT16 day, dayofweek;
	UINT32 hour, minute, second;
};
struct MovieExtInfo MovieInfo = {0, 0, 0, 0, 0, 0, 0};

INT32 is_netgame_or_recording()
{
	return 0;
}

// a driver changed its resolution (or aspect): fe_burn.cpp sizes its picture buffer again at once (it also checks
// before every frame)
void FeBurnReinitialiseVideo();
void Reinitialise()
{
	FeBurnReinitialiseVideo();
}
void ReinitialiseVideo()
{
	FeBurnReinitialiseVideo();
}

// TCHAR is char here: the text as it is (into pszOutString when given)
char* TCHARToANSI(const TCHAR* pszInString, char* pszOutString, INT32 nOutSize)
{
	if (pszOutString)
	{
		strncpy(pszOutString, pszInString, nOutSize > 0 ? nOutSize : 0);
		if (nOutSize > 0)
			pszOutString[nOutSize - 1] = 0;
		return pszOutString;
	}
	return (char*)pszInString;
}

// Neo Geo CD: not in this port (its drivers are not listed); the CD interface answers "no disc"
CDEmuStatusValue CDEmuStatus = idle;
TCHAR CDEmuImage[MAX_PATH] = "";
INT32 CDEmuStop() { return 1; }
INT32 CDEmuPlay(UINT8, UINT8, UINT8) { return 1; }
INT32 CDEmuLoadSector(INT32, char*) { return 0; }
UINT8* CDEmuReadTOC(INT32) { return NULL; }
UINT8* CDEmuReadQChannel() { return NULL; }
INT32 CDEmuGetSoundBuffer(INT16*, INT32) { return 1; }
INT32 CDEmuScan(INT32, INT32*) { return 0; }
TCHAR* GetIsoPath() { return NULL; }
INT32 NeoCDInfo_Init() { return 0; }
TCHAR* NeoCDInfo_Text(INT32) { return NULL; }
INT32 NeoCDInfo_ID() { return 0; }
void NeoCDInfo_SetTitle() {}
void NeoCDInfo_Exit() {}
