module;
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <exception>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module wb.net.connection;

import wb.net.crypto;
import wb.net.protocol;
import wb.net.socket;
import wb.net.stream;

namespace wb::net
{
namespace
{
// Plaintext frame (key exchange only): u32 size, u8 type, payload
constexpr size_t HEADER_BYTES = 5;
// Encrypted frame: u32 payload size, then type + payload + tag sealed together
constexpr size_t SEALED_HEADER_BYTES = 4;
constexpr uint8_t KEY_FRAME_SERVER = 0xF1;
constexpr uint8_t KEY_FRAME_CLIENT = 0xF2;
// What the last Inkwell version without encryption sent first: a 16 byte Challenge, type 1
constexpr uint8_t OLD_CHALLENGE_TYPE = 1;
// More than this waiting for one peer means it is not keeping up (or never will)
constexpr size_t MAX_QUEUED_BYTES = size_t{ 2 } << 30;
} // namespace

Connection::Connection(std::unique_ptr<Stream> p_Stream, const Role p_Role, FrameHandler p_OnFrame, CloseHandler p_OnClose)
	: m_Stream(std::move(p_Stream)), m_Role(p_Role), m_OnFrame(std::move(p_OnFrame)), m_OnClose(std::move(p_OnClose)), m_PeerAddress(m_Stream->peerAddress()), m_MaxFrame(MAX_HANDSHAKE_FRAME)
{
}

Connection::Connection(Socket p_Socket, const Role p_Role, FrameHandler p_OnFrame, CloseHandler p_OnClose)
	: Connection(std::make_unique<Socket>(std::move(p_Socket)), p_Role, std::move(p_OnFrame), std::move(p_OnClose))
{
}

Connection::~Connection()
{
	close();
	if (m_Reader.joinable())
		m_Reader.join();
	if (m_Writer.joinable())
		m_Writer.join();
}

void Connection::start()
{
	if (m_Started.exchange(true))
		return;
	try
	{
		m_Ephemeral = crypto::makeKeyPair();
	}
	catch (const std::exception&)
	{
		close("No secure random number generator");
		return;
	}
	if (m_Role == Role::Server)
	{
		// The server speaks first
		m_KeyFrame = m_Ephemeral.pub;
		m_KeyFrameType = KEY_FRAME_SERVER;
		m_KeyFramePending = true;
	}
	m_Reader = std::thread([this] { readLoop(); });
	m_Writer = std::thread([this] { writeLoop(); });
}

void Connection::close(const std::string& p_Reason)
{
	if (m_Closed.exchange(true))
		return;
	m_Stream->shutdown();
	{
		const std::lock_guard l_Lock(m_Mutex);
	}
	m_Wake.notify_all();
	if (m_OnClose)
		m_OnClose(p_Reason);
}

void Connection::closeWhenSent(const std::string& p_Reason)
{
	{
		const std::lock_guard l_Lock(m_Mutex);
		m_Drain = true;
		m_DrainReason = p_Reason;
	}
	m_Wake.notify_all();
}

void Connection::send(const uint8_t p_Type, std::vector<uint8_t> p_Payload, const Priority p_Priority)
{
	if (m_Closed.load())
		return;
	bool l_Overflow = false;
	{
		const std::lock_guard l_Lock(m_Mutex);
		m_QueuedBytes += p_Payload.size();
		if (m_QueuedBytes > MAX_QUEUED_BYTES)
		{
			l_Overflow = true;
		}
		else
		{
			if (p_Priority == Priority::Bulk)
				m_QueuedBulkBytes += p_Payload.size();
			m_Queues[static_cast<size_t>(p_Priority)].push_back(Frame{ .type = p_Type, .payload = std::move(p_Payload) });
		}
	}
	if (l_Overflow)
		close("Connection too slow");
	else
		m_Wake.notify_one();
}

// Reads the peer's public key and derives the session keys. Runs on the reader thread before anything else is read.
bool Connection::exchangeKeys()
{
	std::array<uint8_t, HEADER_BYTES> l_Header{};
	if (!m_Stream->recvAll(l_Header))
	{
		close("Connection lost");
		return false;
	}
	uint32_t l_Size = 0;
	std::memcpy(&l_Size, l_Header.data(), sizeof(l_Size));
	const uint8_t l_Expected = m_Role == Role::Server ? KEY_FRAME_CLIENT : KEY_FRAME_SERVER;
	if (l_Size != crypto::KEY_BYTES || l_Header[4] != l_Expected)
	{
		if (m_Role == Role::Client && l_Header[4] == OLD_CHALLENGE_TYPE && l_Size == 16)
			close("Incompatible version");
		else
			close("Not an Inkwell peer");
		return false;
	}
	crypto::Key l_PeerPublic{};
	if (!m_Stream->recvAll(l_PeerPublic))
	{
		close("Connection lost");
		return false;
	}
	crypto::SessionKeys l_Keys;
	if (!crypto::deriveSessionKeys(m_Ephemeral, l_PeerPublic, m_Role == Role::Server, l_Keys))
	{
		close("Invalid key exchange");
		return false;
	}
	{
		const std::lock_guard l_Lock(m_Mutex);
		m_Seal = crypto::Aead(l_Keys.send);
		m_Open = crypto::Aead(l_Keys.receive);
		m_Transcript = l_Keys.transcript;
		if (m_Role == Role::Client)
		{
			m_KeyFrame = m_Ephemeral.pub;
			m_KeyFrameType = KEY_FRAME_CLIENT;
			m_KeyFramePending = true;
		}
		m_Secure.store(true);
	}
	crypto::wipe(m_Ephemeral.secret);
	m_Wake.notify_all();
	return true;
}

void Connection::readLoop()
{
	if (!exchangeKeys())
		return;
	while (!m_Closed.load())
	{
		std::array<uint8_t, SEALED_HEADER_BYTES> l_Header{};
		if (!m_Stream->recvAll(l_Header))
			break;
		uint32_t l_Size = 0;
		std::memcpy(&l_Size, l_Header.data(), sizeof(l_Size));
		if (l_Size > m_MaxFrame.load())
		{
			close("Message too large");
			return;
		}
		// type + payload + tag
		std::vector<uint8_t> l_Body(size_t{ 1 } + l_Size + crypto::MAC_BYTES);
		if (!m_Stream->recvAll(l_Body))
			break;
		crypto::Mac l_Mac{};
		std::memcpy(l_Mac.data(), l_Body.data() + 1 + l_Size, crypto::MAC_BYTES);
		if (!m_Open.open(l_Header, std::span<uint8_t>(l_Body.data(), size_t{ 1 } + l_Size), l_Mac))
		{
			close("Corrupted or tampered data");
			return;
		}
		Frame l_Frame;
		l_Frame.type = l_Body[0];
		l_Body.erase(l_Body.begin());
		l_Body.resize(l_Size);
		l_Frame.payload = std::move(l_Body);
		if (m_OnFrame)
			m_OnFrame(std::move(l_Frame));
	}
	close("Connection lost");
}

void Connection::writeLoop()
{
	while (true)
	{
		Frame l_Frame;
		bool l_Found = false;
		bool l_WasBulk = false;
		bool l_SendKeyFrame = false;
		std::string l_DrainReason;
		{
			std::unique_lock l_Lock(m_Mutex);
			m_Wake.wait(l_Lock, [&] { return m_Closed.load() || m_Drain || m_KeyFramePending || (m_Secure.load() && (!m_Queues[0].empty() || !m_Queues[1].empty() || !m_Queues[2].empty())); });
			if (m_Closed.load())
				return;
			if (m_KeyFramePending)
			{
				l_SendKeyFrame = true;
				m_KeyFramePending = false;
			}
			else if (m_Secure.load())
			{
				for (std::deque<Frame>& l_Queue : m_Queues)
				{
					if (!l_Queue.empty())
					{
						l_Frame = std::move(l_Queue.front());
						l_Queue.pop_front();
						l_Found = true;
						l_WasBulk = &l_Queue == &m_Queues[static_cast<size_t>(Priority::Bulk)];
						break;
					}
				}
			}
			if (l_Found)
			{
				m_QueuedBytes -= l_Frame.payload.size();
				if (l_WasBulk)
					m_QueuedBulkBytes -= l_Frame.payload.size();
			}
			else if (!l_SendKeyFrame)
				l_DrainReason = m_DrainReason;
		}
		if (l_SendKeyFrame)
		{
			std::array<uint8_t, HEADER_BYTES + crypto::KEY_BYTES> l_Packet{};
			const uint32_t l_KeySize = crypto::KEY_BYTES;
			std::memcpy(l_Packet.data(), &l_KeySize, sizeof(l_KeySize));
			l_Packet[4] = m_KeyFrameType;
			std::memcpy(l_Packet.data() + HEADER_BYTES, m_KeyFrame.data(), crypto::KEY_BYTES);
			if (!m_Stream->sendAll(l_Packet))
			{
				close("Connection lost");
				return;
			}
			continue;
		}
		if (!l_Found)
		{
			close(l_DrainReason);
			return;
		}

		// u32 size | type | payload | tag, sealed in place in one buffer
		const uint32_t l_Size = static_cast<uint32_t>(l_Frame.payload.size());
		std::vector<uint8_t> l_Packet(SEALED_HEADER_BYTES + size_t{ 1 } + l_Frame.payload.size() + crypto::MAC_BYTES);
		std::memcpy(l_Packet.data(), &l_Size, sizeof(l_Size));
		l_Packet[SEALED_HEADER_BYTES] = l_Frame.type;
		if (!l_Frame.payload.empty())
			std::memcpy(l_Packet.data() + SEALED_HEADER_BYTES + 1, l_Frame.payload.data(), l_Frame.payload.size());
		l_Frame.payload = {};
		crypto::Mac l_Mac{};
		m_Seal.seal(std::span<const uint8_t>(l_Packet.data(), SEALED_HEADER_BYTES), std::span<uint8_t>(l_Packet.data() + SEALED_HEADER_BYTES, size_t{ 1 } + l_Size), l_Mac);
		std::memcpy(l_Packet.data() + SEALED_HEADER_BYTES + 1 + l_Size, l_Mac.data(), crypto::MAC_BYTES);
		if (!m_Stream->sendAll(l_Packet))
		{
			close("Connection lost");
			return;
		}
		if (l_WasBulk && m_OnBulkSent)
			m_OnBulkSent();
	}
}

size_t Connection::queuedBulkBytes()
{
	const std::lock_guard l_Lock(m_Mutex);
	return m_QueuedBulkBytes;
}

void Connection::dropBulk()
{
	const std::lock_guard l_Lock(m_Mutex);
	m_QueuedBytes -= m_QueuedBulkBytes;
	m_QueuedBulkBytes = 0;
	m_Queues[static_cast<size_t>(Priority::Bulk)].clear();
}
} // namespace wb::net
