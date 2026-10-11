// One connection (over any Stream: a TCP socket, a Steam link) speaking length-prefixed frames. A reader thread delivers frames through a callback, a writer thread
// drains a priority queue, so a slow peer never blocks the editor or the other peers.
//
// Every connection is encrypted. The two ends first swap throwaway X25519 public keys (the only plaintext frames, never
// shown to the owner of the connection); after that each frame travels as
//   u32 payload size | ChaCha20-Poly1305( u8 type | payload ) | 16 byte tag
// with the size authenticated, and frames given to send() wait until the keys exist. Knowing who is on the other end is
// not the connection's job: the owner proves the password over transcript() once the first frame arrives.
module;
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

export module wb.net.connection;

import wb.net.crypto;
import wb.net.socket;
import wb.net.stream;

export namespace wb::net
{
struct Frame
{
	uint8_t type = 0;
	std::vector<uint8_t> payload;
};

enum class Priority : uint8_t
{
	Realtime, // cursors and other things only the latest of which matters; sent first
	Normal,   // the ordered stream: ops and the board
	Bulk,     // big files; sent when nothing else waits
};

class Connection
{
public:
	enum class Role : uint8_t
	{
		Server, // accepted the connection; speaks first
		Client, // made the connection
	};

	// Called on the reader thread for every frame
	using FrameHandler = std::function<void(Frame)>;
	// Called once, from whichever thread noticed, with a human readable reason
	using CloseHandler = std::function<void(const std::string&)>;

	Connection(std::unique_ptr<Stream> p_Stream, Role p_Role, FrameHandler p_OnFrame, CloseHandler p_OnClose);
	Connection(Socket p_Socket, Role p_Role, FrameHandler p_OnFrame, CloseHandler p_OnClose);
	~Connection();
	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	// Optional, set before start(): called on the writer thread after each Bulk frame has been written, so a sender can
	// refill the queue without polling
	void setBulkSentHandler(std::function<void()> p_Handler) { m_OnBulkSent = std::move(p_Handler); }
	// Bytes of Bulk frames still waiting to be written
	[[nodiscard]] size_t queuedBulkBytes();
	// Throws away the Bulk frames that have not started to be written
	void dropBulk();

	void start();
	void send(uint8_t p_Type, std::vector<uint8_t> p_Payload, Priority p_Priority = Priority::Normal);
	// Frames above this size close the connection. Raised once the peer is trusted (it started tiny, before login).
	void setMaxFrame(const uint32_t p_Bytes) { m_MaxFrame = p_Bytes; }
	// Stops both threads' work; the destructor joins them. Idempotent.
	void close(const std::string& p_Reason = "Disconnected");
	// Closes once everything queued so far has been written (a rejection must reach the peer before the door shuts)
	void closeWhenSent(const std::string& p_Reason);
	[[nodiscard]] bool closed() const { return m_Closed.load(); }
	[[nodiscard]] const std::string& peerAddress() const { return m_PeerAddress; }
	// Commits to both public keys of the key exchange; the same on both ends. Valid from the first delivered frame on.
	[[nodiscard]] const crypto::Digest& transcript() const { return m_Transcript; }

private:
	void readLoop();
	void writeLoop();

	bool exchangeKeys();

	std::unique_ptr<Stream> m_Stream;
	Role m_Role;
	FrameHandler m_OnFrame;
	CloseHandler m_OnClose;
	std::function<void()> m_OnBulkSent;
	std::string m_PeerAddress;
	std::atomic<uint32_t> m_MaxFrame;
	std::atomic<bool> m_Closed{ false };
	std::atomic<bool> m_Started{ false };

	// The key exchange. m_Secure is set once the keys below are in place (under m_Mutex); the sealing state belongs to the
	// writer thread, the opening state to the reader thread.
	crypto::KeyPair m_Ephemeral;
	crypto::Aead m_Seal;
	crypto::Aead m_Open;
	crypto::Digest m_Transcript{};
	std::atomic<bool> m_Secure{ false };
	bool m_KeyFramePending = false; // a plaintext key frame waits for the writer
	crypto::Key m_KeyFrame{};
	uint8_t m_KeyFrameType = 0;

	std::mutex m_Mutex;
	std::condition_variable m_Wake;
	std::deque<Frame> m_Queues[3];
	size_t m_QueuedBytes = 0;
	size_t m_QueuedBulkBytes = 0;
	bool m_Drain = false;
	std::string m_DrainReason;

	std::thread m_Reader;
	std::thread m_Writer;
};
} // namespace wb::net
