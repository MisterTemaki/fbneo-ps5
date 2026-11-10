// ps5-slang-gpu: what every build has, with the GPU renderer or without it (src/stub.cpp): the configuration, the log,
// the start guard and Fit.
//
// SPDX-License-Identifier: MIT

#include "ps5gpu/gpu.h"
#include "internal.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5gpu
{
const char* const kVersion = "1.1";

namespace
{
Config g_config;
std::string g_marker; // GuardBegin's file, until GuardEnd
} // namespace

void Configure(const Config& config)
{
	g_config = config;
}

const Config& CurrentConfig()
{
	return g_config;
}

namespace internal
{
void Log(const char* fmt, ...)
{
	if (!g_config.log)
		return;
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	g_config.log(line);
}

void MakeParentDirs(const std::string& path)
{
	for (size_t s = path.find('/', 1); s != std::string::npos; s = path.find('/', s + 1))
	{
		const std::string dir = path.substr(0, s);
		if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST)
			return;
	}
}
} // namespace internal

bool GuardBegin(const std::string& marker_path)
{
	if (marker_path.empty())
		return true;
	if (access(marker_path.c_str(), F_OK) == 0)
	{
		unlink(marker_path.c_str());
		internal::Log("[gpu] the last start with the GPU didn't reach the screen: this one uses the CPU");
		return false;
	}
	internal::MakeParentDirs(marker_path);
	if (FILE* f = fopen(marker_path.c_str(), "w"))
	{
		fclose(f);
		g_marker = marker_path;
	}
	return true;
}

void GuardEnd()
{
	if (!g_marker.empty())
	{
		unlink(g_marker.c_str());
		g_marker.clear();
	}
}

Rect Fit(int src_w, int src_h, double aspect, Rect area, bool integer)
{
	if (src_w <= 0 || src_h <= 0 || area.w <= 0 || area.h <= 0)
		return {area.x, area.y, std::max(0, area.w), std::max(0, area.h)};
	if (!(aspect > 0) || !std::isfinite(aspect))
		aspect = double(src_w) / src_h;
	int w, h;
	if (integer)
	{
		// the largest whole multiple of the height that fits, the width at the aspect from it
		int k = std::max(1, area.h / src_h);
		for (; k > 1; k--)
			if (std::lround(src_h * k * aspect) <= area.w)
				break;
		h = src_h * k;
		w = int(std::lround(h * aspect));
		w = std::min(w, area.w);
		h = std::min(h, area.h);
	}
	else
	{
		w = area.w;
		h = int(std::lround(w / aspect));
		if (h > area.h)
		{
			h = area.h;
			w = int(std::lround(h * aspect));
		}
	}
	w = std::max(1, std::min(w, area.w));
	h = std::max(1, std::min(h, area.h));
	return {area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h};
}
} // namespace ps5gpu
