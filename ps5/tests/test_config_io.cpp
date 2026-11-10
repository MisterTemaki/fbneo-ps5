// Partial configuration reads and failed durability checks must never replace shares.txt.
// SPDX-License-Identifier: MIT
#include "../netfs/netfs_core.cpp"
#include <fstream>
#include <algorithm>
#include <unistd.h>

static std::string read_failure_path;
static bool fail_sync = false;
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" int __real_fsync(int);
static ssize_t FailingRead(void* cookie, char* buffer, size_t count)
{
	int& step = *static_cast<int*>(cookie);
	if (step++ == 0)
	{
		const std::string prefix = "# partial config\n";
		const size_t n = std::min(count, prefix.size()); memcpy(buffer, prefix.data(), n); return ssize_t(n);
	}
	errno = EIO; return -1;
}
static int CloseCookie(void* cookie) { delete static_cast<int*>(cookie); return 0; }
extern "C" FILE* __wrap_fopen(const char* path, const char* mode)
{
	if (!read_failure_path.empty() && read_failure_path == path)
	{
		cookie_io_functions_t ops{}; ops.read = FailingRead; ops.close = CloseCookie;
		return fopencookie(new int(0), "r", ops);
	}
	return __real_fopen(path, mode);
}
extern "C" int __wrap_fsync(int fd)
{
	if (fail_sync) { errno = EIO; return -1; }
	return __real_fsync(fd);
}
int main()
{
	char temporary[] = "/tmp/fbneo-config-io-XXXXXX";
	char* name = mkdtemp(temporary); if (!name) return 2;
	const std::string file = std::string(name) + "/shares.txt";
	std::ofstream(file) << "original";
	int failed = 0;
	auto check = [&](bool ok, const char* text) { printf("%s %s\n", ok ? "PASS" : "FAIL", text); failed += !ok; };
	std::string text; int err = 0;
	check(netfs::ReadWholeFile(file, text, err) && text == "original", "complete configuration read succeeds");
	text.clear(); read_failure_path = file;
	check(!netfs::ReadWholeFile(file, text, err) && err == EIO && text.empty(), "partial I/O error is reported and incomplete content discarded");
	read_failure_path.clear(); fail_sync = true;
	check(!netfs::WriteFileAtomic(file, "replacement"), "failed fsync refuses replacement");
	fail_sync = false; text.clear();
	check(netfs::ReadWholeFile(file, text, err) && text == "original", "failed durability check preserves original shares");
	check(access((file + ".part").c_str(), F_OK) != 0, "failed write leaves no temporary configuration");
	check(netfs::WriteFileAtomic(file, "complete"), "successful durable write commits");
	text.clear();check(netfs::ReadWholeFile(file, text, err) && text == "complete", "committed file is complete");
	unlink(file.c_str());rmdir(name);return failed ? 1 : 0;
}
