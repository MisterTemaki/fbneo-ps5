// Real TCP loopback tests with a deterministic reversible machine and delayed peers.
// SPDX-License-Identifier: MIT
#include "fe_netplay.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstring>

using netplay::Session;
using netplay::State;
using netplay::Pad;
static int checks = 0;
static void Check(bool condition, const char* what)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
	std::fflush(stdout);
	assert(condition);
	++checks;
}
struct Machine
{
	uint64_t frame = 0, value = 17;
	size_t bytes = 128 * 1024;
	uint64_t executions = 0;
	bool diverge = false;
	netplay::Hooks Hooks()
	{
		return {
			[this](State& out) { out.assign(bytes, 0); std::memcpy(out.data(), &frame, 8); std::memcpy(out.data() + 8, &value, 8); return true; },
			[this](const State& in) { if (in.size() != bytes) return false; std::memcpy(&frame, in.data(), 8); std::memcpy(&value, in.data() + 8, 8); return true; },
			[this](const Pad& p1, const Pad& p2, bool) {
				for (size_t n = 0; n < p1.size(); ++n) value = (value ^ (p1[n] + uint64_t(p2[n]) * 257)) * 1099511628211ull;
				value ^= frame++; ++executions;
				if (diverge && frame == 100) value ^= 1;
			}
		};
	}
};
static uint16_t Port()
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd >= 0);
	sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
	socklen_t size = sizeof(address);
	assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
	close(fd);
	return ntohs(address.sin_port);
}
static void Connect(Session& host, Session& guest, const char* a = "same-build-game-dips", const char* b = "same-build-game-dips")
{
	const uint16_t port = Port();
	Check(host.Host(port, a), "host opens its listener");
	Check(guest.Join("127.0.0.1", port, b), "guest starts nonblocking connection");
	for (int n = 0; n < 2000; ++n)
	{
		host.Poll(); guest.Poll();
		if (host.GetStatus().phase == netplay::Phase::Playing && guest.GetStatus().phase == netplay::Phase::Playing) return;
		if (!host.Active() || !guest.Active()) return;
		usleep(1000);
	}
	Check(false, "handshake completes within two seconds");
}
static Pad Input(uint32_t frame, bool guest)
{
	Pad pad = {};
	pad[0] = uint8_t((frame / 9 + guest) & 0x3f);
	pad[1] = uint8_t(frame * (guest ? 19 : 7));
	for (size_t n = 2; n < pad.size(); ++n) pad[n] = uint8_t(frame * (n + 1) + guest);
	return pad;
}
int main()
{
	const char* message = "123456789";
	Check(netplay::Checksum(reinterpret_cast<const uint8_t*>(message), 9) == 0xcbf43926,
		  "state checksum matches standard CRC-32");
	{
		Machine a, b; b.value = 99;
		Session host(a.Hooks()), guest(b.Hooks());
		Connect(host, guest);
		Check(host.GetStatus().phase == netplay::Phase::Playing && a.value == b.value,
			  "host state synchronizes a guest with different initial state");
		for (int n = 0; n < 10; ++n) { host.Advance({}); guest.Advance({}); }
		Check(a.frame == 0 && b.frame == 0, "a session begins paused while the menu is open");
		host.Pause(false); guest.Pause(false);
		constexpr uint32_t frames = 480;
		for (int n = 0; n < 12000; ++n)
		{
			if (host.GetStatus().frame < frames) host.Advance(Input(host.GetStatus().frame, false)); else host.Poll();
			// Hold guest inputs for three display ticks out of four to exercise prediction and rollback.
			if (n % 4 == 0)
				for (int burst = 0; burst < 4 && guest.GetStatus().frame < frames; ++burst)
					guest.Advance(Input(guest.GetStatus().frame, true));
			guest.Poll();
			if (!host.Active() || !guest.Active())
			{
				std::fprintf(stderr, "host: %s; guest: %s\n", host.GetStatus().error.c_str(), guest.GetStatus().error.c_str());
				Check(false, "delayed network session remains connected");
			}
			if (host.GetStatus().frame == frames && guest.GetStatus().frame == frames) break;
			usleep(100);
		}
		host.Pause(true); guest.Pause(true);
		for (int n = 0; n < 40; ++n) { host.Advance({}); guest.Advance({}); usleep(1000); }
		Check(host.GetStatus().frame == frames && guest.GetStatus().frame == frames, "both peers run 480 frames");
		Check(a.frame == b.frame && a.value == b.value, "rollback converges both deterministic machines");
		Check(host.GetStatus().rollbacks + guest.GetStatus().rollbacks > 0, "late differing inputs actually trigger rollback");
		Check(host.GetStatus().replayed + guest.GetStatus().replayed > 0, "rollback re-executes historical frames");
		Check(host.GetStatus().phase == netplay::Phase::Playing && guest.GetStatus().phase == netplay::Phase::Playing,
			  "periodic checksums agree after rollback");
		Machine reference;
		auto hooks = reference.Hooks();
		for (uint32_t n = 0; n < frames; ++n) hooks.run(Input(n, false), Input(n, true), true);
		Check(a.value == reference.value, "network result matches an offline reference with exact inputs");
		host.Pause(false);
		for (int n = 0; n < 10; ++n) { host.Advance({}); guest.Poll(); usleep(1000); }
		Check(a.frame == frames, "one paused player pauses both machines");
		guest.Stop();
		for (int n = 0; n < 20 && host.Active(); ++n) { host.Poll(); usleep(1000); }
		Check(host.GetStatus().phase == netplay::Phase::Failed, "disconnect is detected without a blocking receive");
	}
	{
		Machine a, b; Session host(a.Hooks()), guest(b.Hooks());
		Connect(host, guest, "ROM A", "ROM B");
		Check(host.GetStatus().phase == netplay::Phase::Failed || guest.GetStatus().phase == netplay::Phase::Failed,
			  "mismatched ROM/build identity is refused");
	}
	{
		Machine a, b; a.bytes = b.bytes = 1024 * 1024;
		Session host(a.Hooks()), guest(b.Hooks()); Connect(host, guest);
		Check(host.GetStatus().phase == netplay::Phase::Playing && guest.GetStatus().phase == netplay::Phase::Playing,
			  "one-MiB initial state survives partial nonblocking reads and writes");
	}
	{
		Machine a, b; b.diverge = true;
		Session host(a.Hooks()), guest(b.Hooks()); Connect(host, guest);
		host.Pause(false); guest.Pause(false);
		for (int n = 0; n < 2000 && host.Active() && guest.Active(); ++n)
		{
			host.Advance({}); guest.Advance({}); usleep(1000);
		}
		Check(host.GetStatus().error.find("desynchronized") != std::string::npos ||
			  guest.GetStatus().error.find("desynchronized") != std::string::npos,
			  "a divergent core state is detected by periodic confirmed-state checksums");
	}
	{
		Machine a, b; a.bytes = b.bytes = netplay::kMaxState + 1;
		Session host(a.Hooks()), guest(b.Hooks()); Connect(host, guest);
		Check(host.GetStatus().phase == netplay::Phase::Failed,
			  "an oversized core snapshot is refused during synchronization");
	}
	{
		Machine a, b; Session host(a.Hooks()), guest(b.Hooks()); Connect(host, guest);
		host.Pause(false); guest.Pause(false);
		for (int n = 0; n < 10; ++n) { host.Poll(); guest.Poll(); }
		a.bytes *= 2;
		host.Advance({});
		Check(host.GetStatus().phase == netplay::Phase::Failed && host.GetStatus().error.find("layout") != std::string::npos,
		      "a changing serialized-state size cannot exceed the rollback memory budget");
	}
	{
		Machine a; Session host(a.Hooks()); uint16_t port = Port();
		Check(host.Host(port, "test"), "malformed-peer listener starts");
		Session collision(a.Hooks());
		Check(!collision.Host(port, "test"), "an occupied port reports failure without replacing the existing host");
		int fd = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in address = {};
		address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
		assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
		uint32_t packet[] = {htonl(0x46424e31), htonl(2), htonl(0xffffffff)};
		assert(send(fd, packet, sizeof(packet), 0) == sizeof(packet));
		for (int n = 0; n < 20 && host.Active(); ++n) { host.Poll(); usleep(1000); }
		Check(host.GetStatus().phase == netplay::Phase::Failed, "oversized state packet is rejected before allocation");
		close(fd);
		port = Port();
		Check(host.Host(port, "test"), "heartbeat-rate test listener starts");
		fd = socket(AF_INET, SOCK_STREAM, 0); address.sin_port = htons(port);
		assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
		uint32_t flood[600 * 3];
		for (size_t n = 0; n < 600; ++n)
		{ flood[n * 3] = htonl(0x46424e31); flood[n * 3 + 1] = htonl(7); flood[n * 3 + 2] = 0; }
		assert(send(fd, flood, sizeof(flood), 0) == sizeof(flood));
		for (int n = 0; n < 20 && host.Active(); ++n) { host.Poll(); usleep(1000); }
		Check(host.GetStatus().phase == netplay::Phase::Failed && host.GetStatus().error.find("rate") != std::string::npos,
		      "heartbeat flooding cannot monopolize the frontend thread");
		close(fd);
		Session guest(a.Hooks());
		Check(!guest.Join("not-an-ip", port, "test"), "invalid IPv4 address is refused");
		Check(!guest.Host(0, "test"), "invalid port is refused");
	}
	std::printf("netplay: %d checks passed\n", checks);
}
