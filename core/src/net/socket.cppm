// Blocking TCP sockets (Winsock / POSIX) behind a small RAII type. No SDL: the collaboration code can be tested headless.
module;
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

export module wb.net.socket;

import wb.net.stream;

export namespace wb::net
{
class Socket final : public Stream
{
public:
	Socket() = default;
	~Socket() override;
	Socket(Socket&& p_Other) noexcept : m_Handle(p_Other.m_Handle) { p_Other.m_Handle = INVALID; }
	Socket& operator=(Socket&& p_Other) noexcept;
	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;

	[[nodiscard]] bool valid() const { return m_Handle != INVALID; }
	explicit operator bool() const { return valid(); }

	// Listens on every interface (IPv6 and IPv4 where available). Port 0 picks a free one (see localPort).
	// p_Trace (optional) receives one line per step taken, for diagnostics
	[[nodiscard]] static Socket listenTcp(uint16_t p_Port, std::string& p_Error, std::vector<std::string>* p_Trace = nullptr);
	// Resolves and connects, trying every address the name resolves to (at most p_TimeoutMs each), giving up as soon as
	// p_Cancel becomes true. On failure p_Error says what went wrong in plain words; p_Trace is called, from the calling thread, with each step as it happens.
	[[nodiscard]] static Socket connectTcp(const std::string& p_Host, uint16_t p_Port, int p_TimeoutMs, const std::atomic<bool>& p_Cancel, std::string& p_Error, const std::function<void(const std::string&)>& p_Trace = {});

	// Waits up to p_TimeoutMs for a peer; an invalid socket comes back on timeout or error
	[[nodiscard]] Socket accept(int p_TimeoutMs);
	// Both return false when the connection broke or was closed
	[[nodiscard]] bool sendAll(std::span<const uint8_t> p_Data) override;
	[[nodiscard]] bool recvAll(std::span<uint8_t> p_Data) override;

	// shutdown() wakes a thread blocked in recvAll/sendAll on this socket (it then fails). The handle stays valid until close.
	void shutdown() override;
	void close() override;

	[[nodiscard]] uint16_t localPort() const;
	[[nodiscard]] std::string peerAddress() const override;

private:
	static constexpr uintptr_t INVALID = ~static_cast<uintptr_t>(0);
	explicit Socket(const uintptr_t p_Handle) : m_Handle(p_Handle) {}

	uintptr_t m_Handle = INVALID;
};

// Splits "host", "host:port", "[v6]:port" or a bare IPv6 address. Returns false for an empty host or a bad port.
[[nodiscard]] bool parseEndpoint(const std::string& p_Text, uint16_t p_DefaultPort, std::string& p_Host, uint16_t& p_Port);
} // namespace wb::net
