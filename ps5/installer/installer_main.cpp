// FBNeo PS5: the payloads (FBNeoPS5.elf and FBNeoPS5-helper.elf).
//
// FBNeoPS5.elf, sent with the PS5 Payload Manager / the ELF loader (port 9021), is what PS5SX2's installer and
// helper are together:
//   1. it installs or updates the dashboard app in /data/homebrew/PPSA99012 (eboot.bin = FBNeo itself,
//      sce_module/libc.prx, sce_sys/param.json, icon0.png, pic0.dds, pic1.dds), built into it (install_data.cpp);
//   2. then it stays running as the helper that lets the app out of its sandbox (ProsperoJailbreak.h), until
//      the console is turned off. A second copy sent while one runs only installs.
// FBNeoPS5-helper.elf (FBNEO_HELPER_ONLY) is step 2 alone: the app carries it and sends it to the ELF
// loader itself when no helper answers (after a reboot, for instance).
//
// Nothing here writes outside /data/fbneo and /data/homebrew/PPSA99012.
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"
#include "ProsperoJailbreak.h"
#include "ProsperoNotify.h"
#ifndef FBNEO_HELPER_ONLY
#include "ProsperoInstall.h"
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#ifndef FBNEO_PS5_VERSION
#define FBNEO_PS5_VERSION "dev"
#endif

namespace
{
std::string g_ready_message;

void OnReady()
{
	if (!g_ready_message.empty())
		ProsperoNotify("%s", g_ready_message.c_str());
	ProsperoNotifyFlush();
}
} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const bool have_data = OrbisPathsInit();
#ifdef FBNEO_HELPER_ONLY
	OrbisLogOpen("helper");
	OrbisLog("[helper] FBNeo PS5 helper %s (built %s %s), pid %d%s", FBNEO_PS5_VERSION, __DATE__, __TIME__,
		int(getpid()), have_data ? "" : " (no /data: no log file)");
#else
	OrbisLogOpen("installer");
	OrbisLog("[installer] FBNeo PS5 %s (built %s %s), pid %d", FBNEO_PS5_VERSION, __DATE__, __TIME__, int(getpid()));
	if (!have_data)
		OrbisLog("[installer] can't create %s", OrbisRoot().c_str());

	const char* open_hint = "Open it from the FBNeo PS5 icon on the home screen.";
	char msg[512];
	switch (InstallApp())
	{
		case InstallResult::Installed:
			snprintf(msg, sizeof(msg), "FBNeo PS5 %s installed. %s", FBNEO_PS5_VERSION, open_hint);
			break;
		case InstallResult::Updated:
			snprintf(msg, sizeof(msg), "FBNeo PS5 updated to %s. %s", FBNEO_PS5_VERSION, open_hint);
			break;
		case InstallResult::UpToDate:
			snprintf(msg, sizeof(msg), "FBNeo PS5 %s is ready. %s", FBNEO_PS5_VERSION, open_hint);
			break;
		case InstallResult::Failed:
			snprintf(msg, sizeof(msg), "FBNeo PS5: could not install the app in %s (see %s/logs/installer.log)",
				AppInstallDir().c_str(), OrbisRoot().c_str());
			break;
		default:
			snprintf(msg, sizeof(msg), "FBNeo PS5: nothing to install");
			break;
	}
	if (SyncAppMeta() > 0)
	{
		const size_t len = strlen(msg);
		snprintf(msg + len, sizeof(msg) - len, " Home screen art updated (restart the PS5 if it doesn't show).");
	}
	OrbisLog("[installer] %s", msg);
	g_ready_message = msg;
#endif

	// the helper: runs until the console is turned off, or returns at once when one already runs
	if (!jailbreak::ServeHelper(OnReady))
	{
#ifndef FBNEO_HELPER_ONLY
		ProsperoNotify("%s", g_ready_message.c_str());
#endif
		OrbisLog("[helper] another helper is running: leaving");
	}
	ProsperoNotifyFlush();
	OrbisLogClose();
	return 0;
}
