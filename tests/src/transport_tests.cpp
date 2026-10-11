// Collaboration over transports that are not TCP sockets (what a Steam link is to the session): an in-memory pipe stands in.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.io.serializer;
import wb.net.collab;
import wb.net.connection;
import wb.net.stream;

namespace
{
// One direction of a pipe
struct Channel
{
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<uint8_t> bytes;
	bool closed = false;
};

class PipeEnd final : public wb::net::Stream
{
public:
	PipeEnd(std::shared_ptr<Channel> p_In, std::shared_ptr<Channel> p_Out, std::string p_Peer) : m_In(std::move(p_In)), m_Out(std::move(p_Out)), m_Peer(std::move(p_Peer)) {}
	~PipeEnd() override { shutdown(); }

	bool sendAll(const std::span<const uint8_t> p_Data) override
	{
		{
			const std::lock_guard l_Lock(m_Out->mutex);
			if (m_Out->closed)
				return false;
			m_Out->bytes.insert(m_Out->bytes.end(), p_Data.begin(), p_Data.end());
		}
		m_Out->wake.notify_all();
		return true;
	}

	bool recvAll(std::span<uint8_t> p_Data) override
	{
		std::unique_lock l_Lock(m_In->mutex);
		for (uint8_t& l_Byte : p_Data)
		{
			m_In->wake.wait(l_Lock, [&] { return !m_In->bytes.empty() || m_In->closed; });
			if (m_In->bytes.empty())
				return false;
			l_Byte = m_In->bytes.front();
			m_In->bytes.pop_front();
		}
		return true;
	}

	void shutdown() override
	{
		for (Channel* l_Channel : { m_In.get(), m_Out.get() })
		{
			{
				const std::lock_guard l_Lock(l_Channel->mutex);
				l_Channel->closed = true;
			}
			l_Channel->wake.notify_all();
		}
	}
	void close() override { shutdown(); }
	std::string peerAddress() const override { return m_Peer; }

private:
	std::shared_ptr<Channel> m_In;
	std::shared_ptr<Channel> m_Out;
	std::string m_Peer;
};

// The place people "connect" to
struct Switchboard
{
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::unique_ptr<wb::net::Stream>> waiting;
	bool closed = false;
	std::atomic<int> connections{ 0 };
};

class PipeListener final : public wb::net::Listener
{
public:
	explicit PipeListener(std::shared_ptr<Switchboard> p_Board) : m_Board(std::move(p_Board)) {}

	std::unique_ptr<wb::net::Stream> accept(const int p_TimeoutMs) override
	{
		std::unique_lock l_Lock(m_Board->mutex);
		m_Board->wake.wait_for(l_Lock, std::chrono::milliseconds(p_TimeoutMs), [&] { return !m_Board->waiting.empty() || m_Board->closed; });
		if (m_Board->waiting.empty())
			return nullptr;
		auto l_Stream = std::move(m_Board->waiting.front());
		m_Board->waiting.pop_front();
		return l_Stream;
	}
	void close() override
	{
		{
			const std::lock_guard l_Lock(m_Board->mutex);
			m_Board->closed = true;
		}
		m_Board->wake.notify_all();
	}

private:
	std::shared_ptr<Switchboard> m_Board;
};

wb::net::Connector pipeConnector(const std::shared_ptr<Switchboard>& p_Board, const bool p_Reachable = true)
{
	return [p_Board, p_Reachable](const std::atomic<bool>&, std::string& p_Error, const std::function<void(const std::string&)>& p_Trace) -> std::unique_ptr<wb::net::Stream>
	{
		p_Trace("Dialling the switchboard");
		if (!p_Reachable)
		{
			p_Error = "Nobody answers on the switchboard";
			return nullptr;
		}
		auto l_ToHost = std::make_shared<Channel>();
		auto l_ToClient = std::make_shared<Channel>();
		{
			const std::lock_guard l_Lock(p_Board->mutex);
			p_Board->waiting.push_back(std::make_unique<PipeEnd>(l_ToHost, l_ToClient, "pipe:host-side"));
		}
		p_Board->wake.notify_all();
		++p_Board->connections;
		return std::make_unique<PipeEnd>(l_ToClient, l_ToHost, "pipe:client-side");
	};
}

struct Node
{
	wb::Document doc;
	std::vector<std::string> errors;
	std::unique_ptr<wb::net::CollabSession> session;

	Node()
	{
		wb::net::CollabHooks l_Hooks;
		l_Hooks.makeSnapshot = [this] { return wb::serializeBoardShell(doc, wb::BoardMeta{}, SIZE_MAX); };
		l_Hooks.loadSnapshot = [this](const std::span<const uint8_t> p_Bytes)
		{
			wb::BoardMeta l_Meta;
			return wb::deserializeBoard(p_Bytes, doc, l_Meta, true).ok;
		};
		l_Hooks.message = [this](const std::string& p_Text, const bool p_Error)
		{
			if (p_Error)
				errors.push_back(p_Text);
		};
		session = std::make_unique<wb::net::CollabSession>(doc, std::move(l_Hooks));
	}
};

bool pumpUntil(const std::vector<Node*>& p_Nodes, const std::function<bool()>& p_Done, const int p_TimeoutMs = 8000)
{
	const auto l_End = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_TimeoutMs);
	while (std::chrono::steady_clock::now() < l_End)
	{
		for (Node* l_Node : p_Nodes)
			l_Node->session->tick();
		if (p_Done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return false;
}

std::unique_ptr<wb::Object> makeStroke(wb::Document& p_Document, const float p_X)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate({ static_cast<double>(p_X), 1.0 });
	l_Object->payload = wb::StrokeData{
		.style = {},
		.points = { { .position = { 0.f, 0.f }, .radius = 1.f }, { .position = { p_X, 3.f }, .radius = 2.f } },
	};
	return l_Object;
}
} // namespace

TEST(Transport, ASessionRunsOverAnyStreamWithoutAPort)
{
	Node l_Host;
	Node l_Ana;
	l_Host.doc.insert(makeStroke(l_Host.doc, 1.f));

	auto l_Board = std::make_shared<Switchboard>();
	ASSERT_TRUE(l_Host.session->hostRemote("secret", "Hosty"));
	l_Host.session->addListener(std::make_unique<PipeListener>(l_Board), "pipe");
	EXPECT_EQ(l_Host.session->role(), wb::net::Role::Host);
	EXPECT_EQ(l_Host.session->listenPort(), 0); // no TCP port was opened

	l_Ana.session->joinWith(pipeConnector(l_Board), "the switchboard", "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Ana.session->online(); })) << (l_Ana.errors.empty() ? "timeout" : l_Ana.errors.back());
	EXPECT_EQ(l_Ana.doc.objects().size(), l_Host.doc.objects().size());

	// The login still depends on the password, whatever carries the bytes
	Node l_Mallory;
	l_Mallory.session->joinWith(pipeConnector(l_Board), "the switchboard", "wrong", "Mallory");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Mallory }, [&] { return !l_Mallory.session->active(); }));
	EXPECT_FALSE(l_Mallory.errors.empty());
	EXPECT_EQ(l_Host.session->users().size(), 2u);

	auto l_Stroke = makeStroke(l_Ana.doc, 5.f);
	const wb::ObjectId l_Id = l_Stroke->id;
	l_Ana.doc.insert(std::move(l_Stroke));
	EXPECT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Host.doc.find(l_Id) != nullptr; }));
}

TEST(Transport, PeopleOnDifferentTransportsShareOneBoard)
{
	Node l_Host;
	Node l_Tcp;
	Node l_Pipe;
	l_Host.doc.insert(makeStroke(l_Host.doc, 1.f));

	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "pw", "Hosty", l_Error)) << l_Error;
	auto l_Board = std::make_shared<Switchboard>();
	l_Host.session->addListener(std::make_unique<PipeListener>(l_Board), "pipe");
	ASSERT_NE(l_Host.session->listenPort(), 0);

	l_Tcp.session->join("127.0.0.1", l_Host.session->listenPort(), "pw", "Tcp");
	l_Pipe.session->joinWith(pipeConnector(l_Board), "the switchboard", "pw", "Pipe");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Tcp, &l_Pipe }, [&] { return l_Tcp.session->online() && l_Pipe.session->online() && l_Host.session->users().size() == 3; }));

	// An edit made on one transport reaches the person on the other, relayed by the host
	auto l_Stroke = makeStroke(l_Tcp.doc, 9.f);
	const wb::ObjectId l_Id = l_Stroke->id;
	l_Tcp.doc.insert(std::move(l_Stroke));
	EXPECT_TRUE(pumpUntil({ &l_Host, &l_Tcp, &l_Pipe }, [&] { return l_Pipe.doc.find(l_Id) != nullptr; }));

	// The host ending the session reaches both
	l_Host.session->leave();
	EXPECT_TRUE(pumpUntil({ &l_Tcp, &l_Pipe }, [&] { return !l_Tcp.session->active() && !l_Pipe.session->active(); }));
}

TEST(Transport, ATransportThatCannotConnectReportsWhy)
{
	Node l_Ana;
	auto l_Board = std::make_shared<Switchboard>();
	l_Ana.session->joinWith(pipeConnector(l_Board, false), "the switchboard", "pw", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Ana }, [&] { return !l_Ana.session->active(); }));
	ASSERT_FALSE(l_Ana.errors.empty());
	EXPECT_NE(l_Ana.errors.back().find("Nobody answers"), std::string::npos);
}

TEST(Transport, AConnectionWorksOverAnyStream)
{
	using wb::net::Connection;
	auto l_ToServer = std::make_shared<Channel>();
	auto l_ToClient = std::make_shared<Channel>();
	std::mutex l_Mutex;
	std::vector<uint8_t> l_Got;
	Connection l_Server(std::make_unique<PipeEnd>(l_ToServer, l_ToClient, "client"), Connection::Role::Server, [&](wb::net::Frame p_Frame)
	{
		const std::lock_guard l_Lock(l_Mutex);
		l_Got = p_Frame.payload;
	}, [](const std::string&) {});
	Connection l_Client(std::make_unique<PipeEnd>(l_ToClient, l_ToServer, "server"), Connection::Role::Client, [](wb::net::Frame) {}, [](const std::string&) {});
	EXPECT_EQ(l_Client.peerAddress(), "server");
	l_Server.start();
	l_Client.start();
	l_Client.send(9, { 1, 2, 3, 4, 5 });
	for (int i = 0; i < 500; ++i)
	{
		{
			const std::lock_guard l_Lock(l_Mutex);
			if (!l_Got.empty())
				break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const std::lock_guard l_Lock(l_Mutex);
	EXPECT_EQ(l_Got, (std::vector<uint8_t>{ 1, 2, 3, 4, 5 }));
}
