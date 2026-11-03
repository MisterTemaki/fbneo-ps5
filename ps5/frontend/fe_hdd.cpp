// FBNeo PS5 frontend (2.2): where a game's hard disk image is (fe_hdd.h).
// SPDX-License-Identifier: MIT

#include "fe_hdd.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

namespace fe
{
namespace hdd
{
namespace
{
bool IsDir(const std::string& p)
{
	struct stat st = {};
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& p)
{
	struct stat st = {};
	return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The entry of `dir` called `name`: that name when it is there as it is, else the first one equal to it without
// regard to case (the folder listed), else "". want_dir: a folder, else a file.
std::string Match(const std::string& dir, const std::string& name, bool want_dir, bool* too_long)
{
	const std::string exact = dir + "/" + name;
	if (exact.size() > kMaxPath)
	{
		*too_long = true;
		return "";
	}
	if (want_dir ? IsDir(exact) : IsFile(exact))
		return name;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return "";
	std::vector<std::string> found;
	while (const dirent* de = readdir(d))
		if (strcasecmp(de->d_name, name.c_str()) == 0)
			found.push_back(de->d_name);
	closedir(d);
	std::sort(found.begin(), found.end()); // the same choice every time when several differ only in case
	for (const std::string& f : found)
	{
		const std::string p = dir + "/" + f;
		if (p.size() > kMaxPath)
		{
			*too_long = true;
			continue;
		}
		if (want_dir ? IsDir(p) : IsFile(p))
			return f;
	}
	return "";
}

bool OnNetwork(const std::string& p)
{
	return p.compare(0, 5, "/smb/") == 0 || p.compare(0, 5, "/nfs/") == 0;
}
} // namespace

std::string DirOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::vector<std::string> Folders(const std::string& set_archive, const std::string& parent_archive,
	const std::vector<std::string>& roots)
{
	std::vector<std::string> f;
	if (!set_archive.empty())
		f.push_back(DirOf(set_archive));
	if (!parent_archive.empty())
		f.push_back(DirOf(parent_archive));
	f.insert(f.end(), roots.begin(), roots.end());
	return f;
}

Lookup FindImage(const std::string& set, const std::string& parent, const std::string& disk,
	const std::vector<std::string>& folders)
{
	Lookup r;
	const std::string owner = parent.empty() ? set : parent;
	// the file names: <disk>.chd, then <disk>.img; a disk named "<x>.img" by the driver: <x>.chd, then <x>.img
	std::string base = disk;
	if (base.size() > 4 && strcasecmp(base.c_str() + base.size() - 4, ".img") == 0)
		base = base.substr(0, base.size() - 4);
	const std::string names[2] = {base + ".chd", base + ".img"};
	r.need = owner + "/" + base + ".chd";
	std::vector<std::string> subs = {set};
	if (!parent.empty() && parent != set)
		subs.push_back(parent);
	std::vector<std::string> seen;
	bool too_long = false;
	for (std::string f : folders)
	{
		while (f.size() > 1 && f.back() == '/')
			f.pop_back();
		if (f.empty() || std::find(seen.begin(), seen.end(), f) != seen.end())
			continue;
		seen.push_back(f);
		if (r.folder.empty())
			r.folder = f + "/" + owner;
		for (const std::string& sub : subs)
		{
			const std::string d = Match(f, sub, true, &too_long);
			if (d.empty())
				continue;
			const std::string dir = f + "/" + d;
			for (const std::string& n : names)
			{
				const std::string file = Match(dir, n, false, &too_long);
				if (file.empty())
					continue;
				r.result = Find::Found;
				r.path = dir + "/" + file;
				r.folder = dir;
				r.on_network = OnNetwork(r.path);
				return r;
			}
		}
	}
	r.result = too_long ? Find::TooLong : Find::Missing;
	return r;
}
} // namespace hdd
} // namespace fe
