module;
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module wb.net.connection;

import wb.net.protocol;
import wb.net.socket;

namespace wb::net
{
namespace
{
constexpr size_t HEADER_BYTES = 5;
// More than this waiting for one peer means it is not keeping up (or never will)
constexpr size_t MAX_QUEUED_BYTES = size_t{ 2 } << 30;
// Frames up to this size go out in one write, larger ones as header + payload
constexpr size_t COALESCE_BYTES = 64u << 10;
} // namespace

Connection::Connection(Socket p_Socket, FrameHandler p_OnFrame, CloseHandler p_OnClose)
	: m_Socket(std::move(p_Socket)), m_OnFrame(std::move(p_OnFrame)), m_OnClose(std::move(p_OnClose)), m_PeerAddress(m_Socket.peerAddress()), m_MaxFrame(MAX_HANDSHAKE_FRAME)
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
	m_Reader = std::thread([this] { readLoop(); });
	m_Writer = std::thread([this] { writeLoop(); });
}

void Connection::close(const std::string& p_Reason)
{
	if (m_Closed.exchange(true))
		return;
	m_Socket.shutdown();
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

void Connection::readLoop()
{
	while (!m_Closed.load())
	{
		std::array<uint8_t, HEADER_BYTES> l_Header{};
		if (!m_Socket.recvAll(l_Header))
			break;
		uint32_t l_Size = 0;
		std::memcpy(&l_Size, l_Header.data(), sizeof(l_Size));
		if (l_Size > m_MaxFrame.load())
		{
			close("Message too large");
			return;
		}
		Frame l_Frame;
		l_Frame.type = l_Header[4];
		l_Frame.payload.resize(l_Size);
		if (l_Size > 0 && !m_Socket.recvAll(l_Frame.payload))
			break;
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
		std::string l_DrainReason;
		{
			std::unique_lock l_Lock(m_Mutex);
			m_Wake.wait(l_Lock, [&] { return m_Closed.load() || m_Drain || !m_Queues[0].empty() || !m_Queues[1].empty() || !m_Queues[2].empty(); });
			if (m_Closed.load())
				return;
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
			if (l_Found)
			{
				m_QueuedBytes -= l_Frame.payload.size();
				if (l_WasBulk)
					m_QueuedBulkBytes -= l_Frame.payload.size();
			}
			else
				l_DrainReason = m_DrainReason;
		}
		if (!l_Found)
		{
			close(l_DrainReason);
			return;
		}

		const uint32_t l_Size = static_cast<uint32_t>(l_Frame.payload.size());
		std::array<uint8_t, HEADER_BYTES> l_Header{};
		std::memcpy(l_Header.data(), &l_Size, sizeof(l_Size));
		l_Header[4] = l_Frame.type;
		bool l_Ok = false;
		if (l_Frame.payload.size() <= COALESCE_BYTES)
		{
			std::vector<uint8_t> l_Packet(HEADER_BYTES + l_Frame.payload.size());
			std::memcpy(l_Packet.data(), l_Header.data(), HEADER_BYTES);
			if (!l_Frame.payload.empty())
				std::memcpy(l_Packet.data() + HEADER_BYTES, l_Frame.payload.data(), l_Frame.payload.size());
			l_Ok = m_Socket.sendAll(l_Packet);
		}
		else
		{
			l_Ok = m_Socket.sendAll(l_Header) && m_Socket.sendAll(l_Frame.payload);
		}
		if (!l_Ok)
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
