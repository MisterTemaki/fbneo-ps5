// Malformed updates must be rejected before replacing installed files.
// SPDX-License-Identifier: MIT
#include "ProsperoUpdate.h"
#include "../coreorbis/orbis-shims/ProsperoInstall.cpp"
#include <filesystem>
#include <fstream>
#include <utility>
#include <cstdarg>

void OrbisLog(const char*, ...) {}
static std::string grow_path;
extern "C" size_t __real_fread(void*, size_t, size_t, FILE*);
extern "C" size_t __wrap_fread(void* data, size_t bytes, size_t count, FILE* file)
{
	if (!grow_path.empty())
	{
		std::ofstream out(grow_path, std::ios::binary | std::ios::app);
		out << "appended while comparing";
		out.close();
		grow_path.clear();
	}
	return __real_fread(data, bytes, count, file);
}
using Entry = std::pair<std::string, std::string>;
static void Word(std::vector<uint8_t>& b, uint32_t v, int bytes)
{
	for (int i = 0; i < bytes; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
static void Text(std::vector<uint8_t>& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
static std::vector<uint8_t> Zip(const std::vector<Entry>& files)
{
	std::vector<uint8_t> b, directory;
	for (const auto& e : files)
	{
		const uint32_t start = uint32_t(b.size());
		Word(b, 0x04034b50, 4); Word(b, 20, 2); Word(b, 0, 2); Word(b, 0, 2);
		Word(b, 0, 4); Word(b, 0, 4); Word(b, uint32_t(e.second.size()), 4); Word(b, uint32_t(e.second.size()), 4);
		Word(b, uint32_t(e.first.size()), 2); Word(b, 0, 2); Text(b, e.first); Text(b, e.second);
		Word(directory, 0x02014b50, 4); Word(directory, 20, 2); Word(directory, 20, 2);
		Word(directory, 0, 2); Word(directory, 0, 2); Word(directory, 0, 4); Word(directory, 0, 4);
		Word(directory, uint32_t(e.second.size()), 4); Word(directory, uint32_t(e.second.size()), 4);
		Word(directory, uint32_t(e.first.size()), 2); Word(directory, 0, 2); Word(directory, 0, 2);
		Word(directory, 0, 2); Word(directory, 0, 2); Word(directory, 0, 4); Word(directory, start, 4);
		Text(directory, e.first);
	}
	const uint32_t cd = uint32_t(b.size()); b.insert(b.end(), directory.begin(), directory.end());
	Word(b, 0x06054b50, 4); Word(b, 0, 2); Word(b, 0, 2);
	Word(b, uint32_t(files.size()), 2); Word(b, uint32_t(files.size()), 2);
	Word(b, uint32_t(directory.size()), 4); Word(b, cd, 4); Word(b, 0, 2);
	return b;
}
static std::vector<uint8_t> Release(std::vector<Entry> entries)
{
	std::string manifest;
	for (auto& e : entries)
	{
		manifest += update::Sha256Hex(reinterpret_cast<const uint8_t*>(e.second.data()), e.second.size()) + " *" + e.first + "\n";
		e.first = "PPSA99924/" + e.first;
	}
	entries.emplace_back("PPSA99924/manifest.sha256", manifest);
	return Zip(entries);
}
static std::string Read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
}
static void Write(const std::string& path, const std::string& s) { std::ofstream(path, std::ios::binary) << s; }
static int failed = 0, passed = 0;
static void Check(bool ok, const char* what)
{
	(ok ? passed : failed)++; printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}
int main(int argc, char** argv)
{
	char temporary[] = "/tmp/fbneo-update-safety-XXXXXX";
	char* folder = mkdtemp(temporary); if (!folder) return 2;
	const std::string root = folder, file = root + "/compare";
	std::vector<unsigned char> expected(1, 'x');
	Write(file, "x");
	if (argc > 1 && strcmp(argv[1], "--growth") == 0)
	{
		grow_path = file;
		Check(!SameContent(file, expected.data(), expected.size()), "a file growing during comparison is rejected safely");
	}
	else
	{
		Check(SameContent(file, expected.data(), expected.size()), "equal file is recognized");
		Write(file, "y"); Check(!SameContent(file, expected.data(), expected.size()), "different file is rejected");
		Write(file, ""); Check(!SameContent(file, expected.data(), expected.size()), "truncated file is rejected");
		Check(SameContent(file, nullptr, 0), "empty file matches empty content");
		Check(!SameContent(root, nullptr, 0), "directory is not file content");
		const std::string param = "{\"titleId\":\"PPSA99924\"}";
		int i = 0;
		auto install = [&](const std::vector<Entry>& entries, bool accept, const char* what) {
			update::Config c; c.title_id = "PPSA99924"; c.app_dir = root + "/app" + std::to_string(i++);
			std::filesystem::create_directories(c.app_dir + "/sce_sys");
			Write(c.app_dir + "/eboot.bin", "old"); Write(c.app_dir + "/sce_sys/param.json", param);
			const std::string error = update::InstallZip(Release(entries), "", c);
			Check(accept ? error.empty() && Read(c.app_dir + "/eboot.bin") == "new" :
				!error.empty() && Read(c.app_dir + "/eboot.bin") == "old" && Read(c.app_dir + "/sce_sys/param.json") == param, what);
		};
		install({{"eboot.bin", "new"}, {"sce_sys/param.json", param}}, true, "valid stored release installs");
		install({{"eboot.bin", "new"}, {"sce_sys/param.json", "{\"titleId\":\"PPSA00000\",\"note\":\"PPSA99924\"}"}}, false,
			"a title mentioned only in a note cannot authorize another title");
		install({{"eboot.bin", "new"}, {"sce_sys/param.json", param}, {std::string("eboot.bin\0extra", 15), "bad"}}, false,
			"embedded NUL cannot bypass staged writes");
		install({{"eboot.bin", "new"}, {"sce_sys/param.json", param}, {"eboot.bin.new", "reserved"}}, false,
			"release paths cannot collide with updater staging names");
		install({{"eboot.bin", "new"}, {"sce_sys/param.json", param}, {"../escape", "bad"}}, false,
			"parent traversal is rejected");
	}
	std::filesystem::remove_all(root);
	printf("update safety: %d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
