// Two-peer PS5 netplay. Standalone protocol; not the RetroArch wire protocol.
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace netplay
{
using Pad = std::array<uint8_t, 14>; // digital bits, four signed axes, eight trigger pressures
using State = std::vector<uint8_t>;
constexpr uint16_t kDefaultPort = 55435;
constexpr size_t kMaxState = 16u * 1024 * 1024;
struct Hooks
{
	std::function<bool(State&)> save;
	std::function<bool(const State&)> load;
	std::function<void(const Pad&, const Pad&, bool)> run;
};
enum class Phase { Off, Listening, Connecting, Synchronizing, Playing, Failed };
enum class Result { Ran, Wait, Failed };
struct Status
{
	Phase phase = Phase::Off;
	bool host = false, local_paused = false, remote_paused = false;
	uint32_t frame = 0, remote_next = 0;
	uint64_t rollbacks = 0, replayed = 0;
	int prediction_limit = 8;
	std::string error;
};
class Session
{
public:
	explicit Session(Hooks hooks);
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;
	bool Host(uint16_t port, const std::string& identity);
	bool Join(const std::string& ipv4, uint16_t port, const std::string& identity);
	void Stop();
	void Poll(); // nonblocking; call while menus are open too; always the emulation/frontend thread
	void Pause(bool paused);
	Result Advance(const Pad& local);
	const Status& GetStatus() const;
	bool Active() const;
private:
	struct Impl;
	std::unique_ptr<Impl> m;
};
uint32_t Checksum(const uint8_t* data, size_t size);
} // namespace netplay
