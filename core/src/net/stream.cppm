// What a collaboration connection needs from the thing underneath it: an ordered, reliable byte stream. TCP sockets are one
// (wb.net.socket); anything else that can carry bytes (a Steam P2P link, an in-memory pipe in tests) can be another.
module;
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

export module wb.net.stream;

export namespace wb::net
{
class Stream
{
public:
	virtual ~Stream() = default;

	// Both return false when the connection broke or was closed
	[[nodiscard]] virtual bool sendAll(std::span<const uint8_t> p_Data) = 0;
	[[nodiscard]] virtual bool recvAll(std::span<uint8_t> p_Data) = 0;
	// Wakes a thread blocked in recvAll/sendAll on this stream (it then fails). Safe to call from any thread.
	virtual void shutdown() = 0;
	virtual void close() = 0;
	// Who is on the other end, for the log ("203.0.113.5", "steam:7656...")
	[[nodiscard]] virtual std::string peerAddress() const = 0;
};

// Hands out the streams of people connecting to a host. Called from a thread of its own.
class Listener
{
public:
	virtual ~Listener() = default;

	// Waits up to p_TimeoutMs for somebody; null on timeout or when closed
	[[nodiscard]] virtual std::unique_ptr<Stream> accept(int p_TimeoutMs) = 0;
	// Stops accepting and wakes a blocked accept
	virtual void close() = 0;
};

// Makes the one outgoing connection of a client. Runs on a thread of its own; gives up as soon as p_Cancel becomes true.
// On failure it returns null and says why in plain words in p_Error. p_Trace receives one line per step as it happens.
using Connector = std::function<std::unique_ptr<Stream>(const std::atomic<bool>& p_Cancel, std::string& p_Error, const std::function<void(const std::string&)>& p_Trace)>;
} // namespace wb::net
