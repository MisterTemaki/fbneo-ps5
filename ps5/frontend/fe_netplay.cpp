// Bounded rollback, state synchronization and nonblocking TCP for two FBNeo PS5 installations.
// SPDX-License-Identifier: MIT
#include "fe_netplay.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <new>

namespace netplay
{
namespace
{
constexpr uint32_t kMagic = 0x46424e31; // FBN1
constexpr size_t kBudget = 64u * 1024 * 1024;
enum Message : uint32_t { Hello = 1, Initial, Ready, Input, Hash, Paused, Ping };
double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Put(State& out, uint32_t n)
{
	for (int i = 24; i >= 0; i -= 8) out.push_back(uint8_t(n >> i));
}
uint32_t Get(const uint8_t* p)
{
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Nonblocking(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
#ifdef SO_NOSIGPIPE
	int yes = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0) return false;
#endif
	return true;
}
bool Again() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
}
uint32_t Checksum(const uint8_t* data, size_t size)
{
	uint32_t crc = ~0u;
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}
struct Session::Impl
{
	Hooks hooks;
	Status status;
	int listener = -1, fd = -1;
	std::string identity;
	State rx, tx;
	size_t sent = 0;
	size_t state_size = 0;
	bool hello = false;
	double last_rx = 0, last_tx = 0, started = 0;
	struct Frame { uint32_t number; State state; Pad local, remote; };
	std::deque<Frame> history;
	std::map<uint32_t, Pad> remote, local;
	std::map<uint32_t, uint32_t> hashes, peer_hashes;
	Pad previous = {};
	explicit Impl(Hooks h) : hooks(std::move(h)) {}
	~Impl() { Close(); }
	void Close()
	{
		if (fd >= 0) close(fd);
		if (listener >= 0) close(listener);
		fd = listener = -1;
		State().swap(rx); State().swap(tx); sent = 0;
		history.clear(); remote.clear(); local.clear(); hashes.clear(); peer_hashes.clear();
	}
	void Fail(const char* why)
	{
		status.phase = Phase::Failed;
		Close();
		status.error = why;
	}
	bool Begin(uint16_t port, const std::string& id, bool host)
	{
		Close(); status = {}; status.host = host; status.local_paused = true;
		identity = id; hello = false; previous = {}; state_size = 0;
		if (!port || id.empty() || id.size() > 2048 || !hooks.save || !hooks.load || !hooks.run)
		{ Fail("Invalid netplay setup."); return false; }
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0 || !Nonblocking(fd)) { Fail("Could not open a nonblocking netplay socket."); return false; }
		started = last_rx = last_tx = Now();
		return true;
	}
	void Queue(Message type, const State& body = {})
	{
		if (body.size() > kMaxState || tx.size() - sent + body.size() + 12 > kMaxState + 65536)
		{ Fail("Netplay send queue limit exceeded."); return; }
		if (sent) { tx.erase(tx.begin(), tx.begin() + long(sent)); sent = 0; }
		Put(tx, kMagic); Put(tx, type); Put(tx, uint32_t(body.size()));
		tx.insert(tx.end(), body.begin(), body.end());
	}
	void Connected()
	{
		int yes = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
		{ Fail("Could not configure low-latency netplay."); return; }
		status.phase = Phase::Synchronizing;
		started = last_rx = last_tx = Now();
		Queue(Hello, State(identity.begin(), identity.end()));
	}
	bool Save(State& state)
	{
		if (!hooks.save(state) || state.empty() || state.size() > kMaxState)
		{ Fail("This game's save state is unavailable or too large for netplay."); return false; }
		if (state_size && state_size != state.size())
		{ Fail("This game's state layout changed during netplay."); return false; }
		state_size = state.size();
		status.prediction_limit = int(std::min<size_t>(8, kBudget / state.size() - 1));
		return true;
	}
	void Receive(uint32_t type, const State& body)
	{
		if (type == Ping) { if (!body.empty()) Fail("Invalid heartbeat."); return; }
		if (type == Hello)
		{
			if (hello || status.phase != Phase::Synchronizing ||
				std::string(body.begin(), body.end()) != identity)
			{ Fail("The game, ROMs, DIP settings or emulator build do not match."); return; }
			hello = true;
			if (status.host)
			{
				State state;
				if (!Save(state)) return;
				// Both machines take the same restore path (palette/cache fixups included).
				if (!hooks.load(state)) { Fail("This game's state cannot be restored."); return; }
				Queue(Initial, state);
			}
			return;
		}
		if (!hello) { Fail("Netplay handshake is incomplete."); return; }
		if (type == Initial)
		{
			if (status.host || status.phase != Phase::Synchronizing || body.empty() || !hooks.load(body))
			{ Fail("The host's game state could not be restored."); return; }
			State state;
			if (!Save(state)) return;
			if (state != body) { Fail("This game does not restore its state deterministically."); return; }
			Queue(Ready); status.phase = Phase::Playing;
			Queue(Paused, State{uint8_t(status.local_paused)});
			return;
		}
		if (type == Ready)
		{
			if (!status.host || status.phase != Phase::Synchronizing || !body.empty())
			{ Fail("Invalid netplay ready message."); return; }
			status.phase = Phase::Playing;
			Queue(Paused, State{uint8_t(status.local_paused)});
			return;
		}
		if (status.phase != Phase::Playing) { Fail("Unexpected netplay message."); return; }
		if (type == Paused)
		{
			if (body.size() != 1 || body[0] > 1) { Fail("Invalid pause message."); return; }
			status.remote_paused = body[0] != 0;
		}
		else if (type == Input)
		{
			if (body.size() != 18) { Fail("Invalid input packet length."); return; }
			const uint32_t frame = Get(body.data());
			if (frame != status.remote_next || uint64_t(frame) > uint64_t(status.frame) + 32 ||
				remote.size() > 64 || (body[4] & 0xc0))
			{ Fail("Invalid netplay input sequence."); return; }
			Pad pad; std::copy(body.begin() + 4, body.end(), pad.begin());
			remote.emplace(frame, pad); ++status.remote_next;
		}
		else if (type == Hash)
		{
			if (body.size() != 8 || peer_hashes.size() > 16) { Fail("Invalid state checksum packet."); return; }
			const uint32_t frame = Get(body.data());
			if (uint64_t(frame) > uint64_t(status.frame) + 32 || frame % 60 || peer_hashes.count(frame))
			{ Fail("Invalid state checksum sequence."); return; }
			peer_hashes[frame] = Get(body.data() + 4);
		}
		else Fail("Unknown netplay packet.");
	}
	void Poll()
	{
		if (status.phase == Phase::Off || status.phase == Phase::Failed) return;
		if (status.phase == Phase::Listening)
		{
			fd = accept(listener, nullptr, nullptr);
			if (fd < 0) { if (!Again()) Fail("Could not accept the netplay connection."); return; }
			close(listener); listener = -1;
			if (!Nonblocking(fd)) { Fail("Could not configure the player socket."); return; }
			Connected();
		}
		if (status.phase == Phase::Connecting)
		{
			pollfd p = {fd, POLLOUT, 0};
			const int ready = poll(&p, 1, 0);
			if (ready < 0 && !Again()) { Fail("Netplay connection failed."); return; }
			if (ready > 0)
			{
				int error = 0; socklen_t size = sizeof(error);
				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
				{ Fail("Could not connect. Check the host IP, port and router forwarding."); return; }
				Connected();
			}
			else { if (Now() - started > 15) Fail("Netplay connection timed out."); return; }
		}
		if (status.phase == Phase::Failed) return;
		size_t budget = 512 * 1024;
		while (fd >= 0 && sent < tx.size() && budget)
		{
			const size_t count = std::min(budget, tx.size() - sent);
#ifdef MSG_NOSIGNAL
			const ssize_t n = send(fd, tx.data() + sent, count, MSG_NOSIGNAL);
#else
			const ssize_t n = send(fd, tx.data() + sent, count, 0);
#endif
			if (n < 0 && Again()) break;
			if (n <= 0) { Fail("The netplay connection was lost."); return; }
			sent += size_t(n); budget -= size_t(n); last_tx = Now();
		}
		if (sent == tx.size()) { tx.clear(); sent = 0; }
		budget = 512 * 1024;
		size_t messages = 0;
		while (fd >= 0 && budget)
		{
			uint8_t buffer[16384];
			const ssize_t n = recv(fd, buffer, std::min(sizeof(buffer), budget), 0);
			if (n < 0 && Again()) break;
			if (n <= 0) { Fail("The other player disconnected."); return; }
			last_rx = Now(); budget -= size_t(n);
			rx.insert(rx.end(), buffer, buffer + n);
			if (rx.size() > kMaxState + 65536) { Fail("Netplay receive limit exceeded."); return; }
			while (rx.size() >= 12)
			{
				const uint32_t magic = Get(rx.data()), type = Get(rx.data() + 4), size = Get(rx.data() + 8);
				if (magic != kMagic || type < Hello || type > Ping || size > kMaxState ||
					(type != Initial && size > 2048) ||
					(type == Initial && (status.host || !hello || status.phase != Phase::Synchronizing)))
				{ Fail("Invalid netplay packet header."); return; }
				if (rx.size() < size_t(size) + 12) break;
				if (++messages > 512) { Fail("Excessive netplay message rate."); return; }
				State body(rx.begin() + 12, rx.begin() + 12 + size);
				rx.erase(rx.begin(), rx.begin() + 12 + size);
				Receive(type, body);
				if (status.phase == Phase::Failed) return;
			}
		}
		if (Now() - last_rx > 30) { Fail("The other player stopped responding."); return; }
		if (status.phase == Phase::Synchronizing && Now() - started > 30)
		{ Fail("Netplay synchronization timed out."); return; }
		if (Now() - last_tx > 1 && tx.empty()) Queue(Ping);
	}
	Pad Predict(uint32_t frame) const
	{
		auto p = remote.upper_bound(frame);
		return p == remote.begin() ? previous : std::prev(p)->second;
	}
	void Run(const Pad& l, const Pad& r, bool visible)
	{
		if (status.host) hooks.run(l, r, visible); else hooks.run(r, l, visible);
	}
	bool Reconcile()
	{
		auto dirty = history.end();
		for (auto p = history.begin(); p != history.end(); ++p)
		{
			auto actual = remote.find(p->number);
			if (actual != remote.end() && actual->second != p->remote) { dirty = p; break; }
		}
		if (dirty != history.end())
		{
			if (!hooks.load(dirty->state)) { Fail("Rollback state restore failed."); return false; }
			++status.rollbacks;
			for (auto p = dirty; p != history.end(); ++p)
			{
				if (!Save(p->state)) return false;
				p->remote = Predict(p->number);
				Run(p->local, p->remote, false); ++status.replayed;
			}
		}
		for (const auto& frame : history)
			if (frame.number && frame.number % 60 == 0 && status.remote_next >= frame.number &&
				!hashes.count(frame.number))
			{
				const uint32_t hash = Checksum(frame.state.data(), frame.state.size());
				hashes[frame.number] = hash;
				State body; Put(body, frame.number); Put(body, hash); Queue(Hash, body);
				if (status.phase == Phase::Failed) return false;
			}
		for (auto p = peer_hashes.begin(); p != peer_hashes.end(); )
		{
			auto mine = hashes.find(p->first);
			if (mine == hashes.end()) { ++p; continue; }
			if (mine->second != p->second) { Fail("The game desynchronized. The session has been stopped."); return false; }
			p = peer_hashes.erase(p);
		}
		while (hashes.size() > 64) hashes.erase(hashes.begin());
		while (history.size() > 1 && history.front().number + 1 < status.remote_next)
		{ local.erase(history.front().number); history.pop_front(); }
		if (!history.empty())
			while (!remote.empty() && remote.begin()->first < history.front().number)
			{ previous = remote.begin()->second; remote.erase(remote.begin()); }
		return status.phase != Phase::Failed;
	}
	Result Advance(const Pad& pad)
	{
		Poll();
		if (status.phase == Phase::Failed) return Result::Failed;
		if (status.phase != Phase::Playing) return Result::Wait;
		if (!Reconcile()) return Result::Failed;
		if (status.local_paused || status.remote_paused) return Result::Wait;
		if (status.frame == std::numeric_limits<uint32_t>::max())
		{ Fail("Netplay frame counter exhausted."); return Result::Failed; }
		if (!local.count(status.frame))
		{
			local[status.frame] = pad;
			State body; Put(body, status.frame); body.insert(body.end(), pad.begin(), pad.end()); Queue(Input, body);
			if (status.phase == Phase::Failed) return Result::Failed;
		}
		Poll(); // send this frame's input immediately, not at the next display refresh
		if (status.phase == Phase::Failed) return Result::Failed;
		if (!Reconcile()) return Result::Failed;
		if (status.remote_paused) return Result::Wait;
		if (status.frame >= status.remote_next && status.frame - status.remote_next >= uint32_t(status.prediction_limit))
			return Result::Wait;
		Frame frame; frame.number = status.frame; frame.local = local.at(status.frame); frame.remote = Predict(status.frame);
		if (!Save(frame.state)) return Result::Failed;
		Run(frame.local, frame.remote, true);
		history.push_back(std::move(frame)); ++status.frame;
		if (!Reconcile()) return Result::Failed;
		return Result::Ran;
	}
};
Session::Session(Hooks h) : m(new Impl(std::move(h))) {}
Session::~Session() = default;
bool Session::Host(uint16_t port, const std::string& id)
{
	if (!m->Begin(port, id, true)) return false;
	int yes = 1; setsockopt(m->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(m->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(m->fd, 1) < 0)
	{ m->Fail("Could not host. The port may already be in use."); return false; }
	m->listener = m->fd; m->fd = -1; m->status.phase = Phase::Listening;
	return true;
}
bool Session::Join(const std::string& ip, uint16_t port, const std::string& id)
{
	if (!m->Begin(port, id, false)) return false;
	sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
	{ m->Fail("Enter the host's IPv4 address."); return false; }
	const int result = connect(m->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
	if (result < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
	{ m->Fail("Could not start the netplay connection."); return false; }
	m->status.phase = Phase::Connecting;
	return true;
}
void Session::Stop() { m->Close(); m->status.phase = Phase::Off; }
void Session::Poll()
{
	try { m->Poll(); } catch (const std::bad_alloc&) { m->Fail("Not enough memory for netplay."); }
}
void Session::Pause(bool paused)
{
	if (m->status.local_paused == paused) return;
	m->status.local_paused = paused;
	if (m->status.phase == Phase::Playing) m->Queue(Paused, State{uint8_t(paused)});
}
Result Session::Advance(const Pad& pad)
{
	try { return m->Advance(pad); }
	catch (const std::bad_alloc&) { m->Fail("Not enough memory for netplay."); return Result::Failed; }
}
const Status& Session::GetStatus() const { return m->status; }
bool Session::Active() const { return m->status.phase != Phase::Off && m->status.phase != Phase::Failed; }
} // namespace netplay
