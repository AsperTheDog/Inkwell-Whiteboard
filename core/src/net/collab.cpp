module;
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module wb.net.collab;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.io.blob;
import wb.io.serializer;
import wb.net.connection;
import wb.net.crypto;
import wb.net.ops;
import wb.net.protocol;
import wb.net.replicator;
import wb.net.socket;
import wb.net.wire;

namespace wb::net
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr std::array<uint32_t, 12> USER_COLORS{
	0xE53935FFu, 0x1E88E5FFu, 0x43A047FFu, 0xFB8C00FFu, 0x8E24AAFFu, 0x00ACC1FFu,
	0xD81B60FFu, 0x6D4C41FFu, 0x3949ABFFu, 0x7CB342FFu, 0xC0A000FFu, 0x546E7AFFu,
};
constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(10);
constexpr int CONNECT_TIMEOUT_MS = 6000; // per address
constexpr auto JOIN_TIMEOUT = std::chrono::seconds(30);
constexpr auto ORDER_INTERVAL = std::chrono::seconds(2);
constexpr auto STROKE_IDLE_LIMIT = std::chrono::seconds(8); // a drawn line nobody has heard of for this long is dropped
constexpr uint32_t MAX_STROKE_UPDATES_PER_SECOND = 400;
constexpr uint32_t MAX_VIDEO_UPDATES_PER_SECOND = 200; // scrubbing sends a few dozen
constexpr size_t MAX_VIDEO_STATES = 4096;
constexpr auto ATTEMPT_WINDOW = std::chrono::seconds(60);
constexpr uint32_t MAX_ATTEMPTS = 5;
// Ops go out in frames of about this size so a huge paste does not become one huge frame
constexpr size_t OPS_BATCH_BYTES = 8u << 20;
// File pieces are queued for a peer only while less than this is waiting, so a huge video is never copied into memory at once
constexpr size_t ASSET_QUEUE_BYTES = 8 * ASSET_CHUNK;
// Files a peer may have outstanding with us at once
constexpr size_t MAX_PENDING_ASSETS = 65536;
// Bytes of unfinished downloads we keep
constexpr uint64_t MAX_DOWNLOAD_BYTES = uint64_t{ 4 } << 30;
// Files this big get a line in the log
constexpr uint64_t BIG_LOG_BYTES = uint64_t{ 8 } << 20;

constexpr uint8_t type(const MsgType p_Type)
{
	return static_cast<uint8_t>(p_Type);
}

size_t approxSize(const Op& p_Op)
{
	if (const auto* l_Add = std::get_if<AddOp>(&p_Op))
		return l_Add->object.size() + 32;
	if (const auto* l_Update = std::get_if<UpdateOp>(&p_Op))
		return l_Update->object.size() + 32;
	if (const auto* l_Order = std::get_if<OrderOp>(&p_Op))
		return l_Order->ids.size() * sizeof(uint64_t) + 16;
	return 96;
}

std::vector<uint8_t> encodeOps(const std::span<const Op> p_Ops)
{
	ByteWriter l_Writer;
	l_Writer.u32(static_cast<uint32_t>(p_Ops.size()));
	for (const Op& l_Op : p_Ops)
		encodeOp(l_Writer, l_Op);
	return l_Writer.take();
}

} // namespace

CollabSession::CollabSession(Document& p_Document, CollabHooks p_Hooks)
	: m_Document(p_Document), m_Hooks(std::move(p_Hooks)), m_Replicator(p_Document, [this](Op p_Op) { queueLocal(std::move(p_Op)); })
{
	m_Replicator.setRemoteChangeHandler([this](const std::span<const ObjectId> p_Ids, const bool p_Removed)
	{
		if (m_Hooks.remoteChange)
			m_Hooks.remoteChange(p_Ids, p_Removed);
	});
	m_Idle.emplace(m_Replicator);
}

CollabSession::~CollabSession()
{
	stop("");
}

const UserInfo* CollabSession::user(const uint16_t p_Slot) const
{
	for (const UserInfo& l_User : m_Users)
	{
		if (l_User.slot == p_Slot)
			return &l_User;
	}
	return nullptr;
}

bool CollabSession::localReadOnly() const
{
	if (m_Role != Role::Client || m_State != LinkState::Online)
		return false;
	const UserInfo* l_Me = user(m_LocalSlot);
	return l_Me != nullptr && l_Me->readOnly;
}

bool CollabSession::setReadOnly(const uint16_t p_Slot, const bool p_ReadOnly)
{
	if (m_Role != Role::Host)
		return false;
	Peer* l_Peer = peerOfSlot(p_Slot);
	if (l_Peer == nullptr)
		return false;
	if (l_Peer->info.readOnly == p_ReadOnly)
		return true;
	l_Peer->info.readOnly = p_ReadOnly;
	note(l_Peer->info.name + (p_ReadOnly ? " is now read-only" : " can edit again"));
	rebuildUsers();
	const std::vector<uint8_t> l_Announce = encode(l_Peer->info);
	for (auto& [l_Id, l_Other] : m_Peers)
	{
		if (l_Other.state == PeerState::Joined)
			l_Other.connection->send(type(MsgType::UserUpdate), l_Announce);
	}
	return true;
}

void CollabSession::tell(const std::string& p_Text, const bool p_Error)
{
	if (m_Hooks.message)
		m_Hooks.message(p_Text, p_Error);
}

void CollabSession::note(const std::string& p_Line)
{
	const std::time_t l_Now = std::time(nullptr);
	std::tm l_Local{};
#ifdef _WIN32
	localtime_s(&l_Local, &l_Now);
#else
	localtime_r(&l_Now, &l_Local);
#endif
	char l_Stamp[16]{};
	std::strftime(l_Stamp, sizeof(l_Stamp), "%H:%M:%S ", &l_Local);
	std::string l_Text = l_Stamp + p_Line;
	if (m_Hooks.log)
		m_Hooks.log(l_Text);
	m_Log.push_back(std::move(l_Text));
	while (m_Log.size() > 200)
		m_Log.pop_front();
}

void CollabSession::setIdle(const bool p_Idle)
{
	if (p_Idle)
		m_Idle.emplace(m_Replicator);
	else
		m_Idle.reset();
}

void CollabSession::post(Event p_Event)
{
	{
		const std::lock_guard l_Lock(m_InboxMutex);
		m_Inbox.push_back(std::move(p_Event));
	}
	if (m_Hooks.wake)
		m_Hooks.wake();
}

// ------------------------------------------------------------------------------------------------ start / stop

bool CollabSession::host(const uint16_t p_Port, std::string p_Password, std::string p_Name, std::string& p_Error)
{
	if (m_Role != Role::None)
		stop("");
	m_Log.clear();
	std::vector<std::string> l_Trace;
	m_Listener = Socket::listenTcp(p_Port, p_Error, &l_Trace);
	for (const std::string& l_Line : l_Trace)
		note(l_Line);
	if (!m_Listener.valid())
	{
		if (p_Error.empty())
			p_Error = "Could not open the port";
		note("Could not host on port " + std::to_string(p_Port) + ": " + p_Error);
		return false;
	}
	m_ListenPort = m_Listener.localPort();
	note("Hosting on port " + std::to_string(m_ListenPort) + ". Waiting for people to join");
	m_Role = Role::Host;
	m_State = LinkState::Online;
	// Everybody who joins proves the password against this key, which is slow to make, so it is made once
	crypto::randomBytes(m_Salt);
	m_PasswordKey = crypto::PasswordKey(p_Password, m_Salt);
	crypto::wipe({ reinterpret_cast<uint8_t*>(p_Password.data()), p_Password.size() });
	m_Name = sanitizeName(p_Name);
	m_LocalSlot = 0;
	m_NextSlot = 1;
	m_Document.setIdSlot(0);
	m_Stop = false;
	rebuildUsers();
	setIdle(false);
	m_AcceptThread = std::thread([this]
	{
		while (!m_Stop.load())
		{
			Socket l_Peer = m_Listener.accept(200);
			if (l_Peer.valid())
			{
				Event l_Event;
				l_Event.kind = Event::Kind::Accepted;
				l_Event.socket = std::move(l_Peer);
				post(std::move(l_Event));
			}
		}
	});
	return true;
}

void CollabSession::join(std::string p_Host, const uint16_t p_Port, std::string p_Password, std::string p_Name)
{
	if (m_Role != Role::None)
		stop("");
	m_Role = Role::Client;
	m_State = LinkState::Connecting;
	m_Password = std::move(p_Password);
	m_Name = sanitizeName(p_Name);
	m_Stop = false;
	m_Log.clear();
	note("Joining " + p_Host + ":" + std::to_string(p_Port) + " as \"" + m_Name + "\"");
	m_ConnectThread = std::thread([this, l_Host = std::move(p_Host), p_Port]
	{
		std::string l_Error;
		// Each step reaches the log as it happens, so a connection that hangs shows where
		const auto l_Trace = [this](const std::string& p_Line)
		{
			Event l_Step;
			l_Step.kind = Event::Kind::Progress;
			l_Step.text = p_Line;
			post(std::move(l_Step));
		};
		Socket l_Socket = Socket::connectTcp(l_Host, p_Port, CONNECT_TIMEOUT_MS, m_Stop, l_Error, l_Trace);
		if (m_Stop.load())
			return;
		Event l_Event;
		if (l_Socket.valid())
		{
			l_Event.kind = Event::Kind::Connected;
			l_Event.socket = std::move(l_Socket);
		}
		else
		{
			l_Event.kind = Event::Kind::ConnectFailed;
			l_Event.text = std::move(l_Error);
		}
		post(std::move(l_Event));
	});
}

void CollabSession::leave()
{
	if (m_Role == Role::None)
		return;
	const bool l_Host = m_Role == Role::Host;
	stop(l_Host ? "The host ended the session" : "");
}

void CollabSession::stop(const std::string& p_Goodbye)
{
	m_Stop = true;
	if (m_ConnectThread.joinable())
		m_ConnectThread.join();
	if (m_AcceptThread.joinable())
		m_AcceptThread.join();
	if (m_PreviewThread.joinable())
	{
		{
			const std::lock_guard l_Lock(m_PreviewMutex);
			m_PreviewStop = true;
			m_PreviewJobs.clear();
		}
		m_PreviewWake.notify_all();
		m_PreviewThread.join();
		m_PreviewStop = false;
	}
	m_PreviewActive.clear();
	m_PreviewFailed.clear();
	m_Listener.close();

	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (!p_Goodbye.empty() && l_Peer.state == PeerState::Joined)
		{
			l_Peer.connection->send(type(MsgType::Reject), encode(Reject{ .reason = p_Goodbye }));
			l_Peer.connection->closeWhenSent("Session ended");
			m_Dying.push_back(std::move(l_Peer.connection));
		}
	}
	m_Peers.clear(); // connections that are not dying close and join here
	{
		const std::lock_guard l_Lock(m_InboxMutex);
		m_Inbox.clear();
	}
	m_Pending.clear();
	m_PendingMove.clear();
	clearDownloads();
	m_AssetWaiters.clear();
	m_Stalled.clear();
	m_LocalMedia.clear();
	++m_AssetsRevision;
	m_Cursors.clear();
	++m_CursorsRevision;
	m_CursorDirty = false;
	m_VideoStates.clear();
	m_VideoEvents.clear();
	m_RemoteStrokes.clear();
	m_EndedStrokes.clear();
	++m_StrokesRevision;
	m_StrokeActive = false;
	m_StrokeDirty = false;
	m_StrokeUnsent.clear();
	m_Users.clear();
	++m_UsersRevision;
	m_Failures.clear();
	m_Password.clear();
	m_PasswordKey = {};
	m_Role = Role::None;
	m_State = LinkState::Offline;
	m_ListenPort = 0;
	m_OrderDirty = false;
	setIdle(true);
	m_Stop = false;
}

void CollabSession::failClient(const std::string& p_Reason)
{
	note("Failed: " + p_Reason);
	tell(p_Reason, true);
	stop("");
}

// ------------------------------------------------------------------------------------------------ users

uint16_t CollabSession::pickSlot()
{
	// Never a slot that already appears in the ids of the board (a saved board may hold some from an earlier session)
	std::unordered_set<uint16_t> l_Used;
	l_Used.insert(0);
	for (const auto& [l_Id, l_Peer] : m_Peers)
		l_Used.insert(l_Peer.info.slot);
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
		l_Used.insert(idSlotOf(l_Object->id));
	for (const auto& [l_Id, l_Asset] : m_Document.assets())
		l_Used.insert(idSlotOf(l_Id));
	for (uint32_t l_Tries = 0; l_Tries < 65535; ++l_Tries)
	{
		const uint16_t l_Slot = m_NextSlot;
		m_NextSlot = static_cast<uint16_t>(m_NextSlot == 65535 ? 1 : m_NextSlot + 1);
		if (!l_Used.contains(l_Slot))
			return l_Slot;
	}
	return 0;
}

uint32_t CollabSession::pickColor() const
{
	for (const uint32_t l_Color : USER_COLORS)
	{
		const bool l_Taken = std::ranges::any_of(m_Users, [&](const UserInfo& p_User) { return p_User.color == l_Color; });
		if (!l_Taken)
			return l_Color;
	}
	return USER_COLORS[m_Users.size() % USER_COLORS.size()];
}

void CollabSession::rebuildUsers()
{
	m_Users.clear();
	if (m_Role == Role::Host)
		m_Users.push_back(UserInfo{ .slot = 0, .name = m_Name, .color = USER_COLORS[0], .host = true });
	std::vector<const UserInfo*> l_Joined;
	for (const auto& [l_Id, l_Peer] : m_Peers)
	{
		if (l_Peer.state == PeerState::Joined)
			l_Joined.push_back(&l_Peer.info);
	}
	std::ranges::sort(l_Joined, {}, &UserInfo::slot);
	for (const UserInfo* l_Info : l_Joined)
		m_Users.push_back(*l_Info);
	++m_UsersRevision;
}

// ------------------------------------------------------------------------------------------------ sending

void CollabSession::queueLocal(Op p_Op)
{
	if (m_State != LinkState::Online)
		return;
	if (m_Role == Role::Host && std::holds_alternative<AddOp>(p_Op))
		m_OrderDirty = true;
	if (std::holds_alternative<AddOp>(p_Op) || std::holds_alternative<UpdateOp>(p_Op))
		m_LocalMedia.push_back(opTarget(p_Op)); // looked at on the next tick: a picture whose file we lack needs handling

	// A drag produces a transform per object per frame: only the latest of a run matters
	if (std::holds_alternative<TransformOp>(p_Op) || std::holds_alternative<UpdateOp>(p_Op))
	{
		const ObjectId l_Id = opTarget(p_Op);
		const auto l_It = m_PendingMove.find(l_Id);
		const bool l_IsUpdate = std::holds_alternative<UpdateOp>(p_Op);
		if (l_It != m_PendingMove.end() && (l_IsUpdate || std::holds_alternative<TransformOp>(m_Pending[l_It->second])))
		{
			m_Pending[l_It->second] = std::move(p_Op);
			return;
		}
		m_PendingMove[l_Id] = m_Pending.size();
		m_Pending.push_back(std::move(p_Op));
		return;
	}
	if (std::holds_alternative<ClearOp>(p_Op))
	{
		m_PendingMove.clear();
		resetAssetTransfers();
	}
	else if (const ObjectId l_Target = opTarget(p_Op); l_Target != INVALID_OBJECT_ID)
		m_PendingMove.erase(l_Target);
	m_Pending.push_back(std::move(p_Op));
}

void CollabSession::sendOps(const std::span<const Op> p_Ops, const uint32_t p_Except)
{
	size_t l_Begin = 0;
	while (l_Begin < p_Ops.size())
	{
		size_t l_End = l_Begin;
		size_t l_Bytes = 0;
		while (l_End < p_Ops.size() && (l_End == l_Begin || l_Bytes + approxSize(p_Ops[l_End]) <= OPS_BATCH_BYTES))
			l_Bytes += approxSize(p_Ops[l_End++]);
		const std::vector<uint8_t> l_Payload = encodeOps(p_Ops.subspan(l_Begin, l_End - l_Begin));
		for (auto& [l_Id, l_Peer] : m_Peers)
		{
			if (l_Peer.state == PeerState::Joined && l_Id != p_Except)
				l_Peer.connection->send(type(MsgType::Ops), l_Payload);
		}
		l_Begin = l_End;
	}
}

void CollabSession::flushLocal()
{
	if (m_Pending.empty())
		return;
	if (m_State == LinkState::Online && !localReadOnly())
		sendOps(m_Pending, 0);
	m_Pending.clear();
	m_PendingMove.clear();
}

void CollabSession::broadcastOrder()
{
	OrderOp l_Order;
	l_Order.ids.reserve(m_Document.size());
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
		l_Order.ids.push_back(l_Object->id);
	const std::array<Op, 1> l_Ops{ Op(std::move(l_Order)) };
	sendOps(l_Ops, 0);
}

// ------------------------------------------------------------------------------------------------ tick

void CollabSession::tick()
{
	if (m_Role == Role::None)
		return;
	// Edits made since the last tick come before anything that arrived, as they happened first
	flushLocal();

	std::deque<Event> l_Events;
	{
		const std::lock_guard l_Lock(m_InboxMutex);
		l_Events.swap(m_Inbox);
	}
	for (Event& l_Event : l_Events)
	{
		if (m_Role == Role::None)
			break; // an event ended the session
		handleEvent(l_Event);
	}
	std::erase_if(m_Dying, [](const std::unique_ptr<Connection>& p_Connection) { return p_Connection->closed(); });
	if (m_Role == Role::None)
		return;

	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (!l_Peer.uploads.empty())
			serveAssets(l_Peer);
	}
	resolveLocalMedia();
	if (!m_Downloads.empty() && m_Document.revision() != m_PrunedRevision)
		pruneDownloads();
	if (m_CursorDirty && m_State == LinkState::Online && Clock::now() - m_LastCursorSent >= std::chrono::milliseconds(CURSOR_INTERVAL_MS))
		sendCursor();
	if (m_StrokeDirty && m_State == LinkState::Online && Clock::now() - m_LastStrokeSent >= std::chrono::milliseconds(CURSOR_INTERVAL_MS))
		sendStroke(false);
	if (!m_RemoteStrokes.empty())
	{
		// A line whose end never came (the connection to its author hiccuped) does not hang around
		const size_t l_Before = m_RemoteStrokes.size();
		std::erase_if(m_RemoteStrokes, [](const auto& p_Entry) { return Clock::now() - p_Entry.second.updated > STROKE_IDLE_LIMIT; });
		if (m_RemoteStrokes.size() != l_Before)
			++m_StrokesRevision;
	}

	const Clock::time_point l_Now = Clock::now();
	if (m_Role == Role::Host)
	{
		std::vector<uint32_t> l_Stale;
		for (const auto& [l_Id, l_Peer] : m_Peers)
		{
			if (l_Peer.state == PeerState::Handshake && l_Now - l_Peer.since > HANDSHAKE_TIMEOUT)
				l_Stale.push_back(l_Id);
		}
		for (const uint32_t l_Id : l_Stale)
			dropPeer(l_Id, "connected but never sent a login within 10 s: probably a port scan or another program");
		// Concurrent adds can leave peers with a different stacking order: the host settles it
		if (m_OrderDirty && l_Now - m_LastOrder > ORDER_INTERVAL)
		{
			m_OrderDirty = false;
			m_LastOrder = l_Now;
			if (!m_Peers.empty())
				broadcastOrder();
		}
	}
	else if (m_State == LinkState::Joining)
	{
		for (const auto& [l_Id, l_Peer] : m_Peers)
		{
			if (!l_Peer.challenged && l_Now - l_Peer.since > HANDSHAKE_TIMEOUT)
			{
				failClient("Connected, but the host never started the login within 10 s. Whatever accepted the connection is not an Inkwell host (a router forwarding to the wrong machine, or another program).");
				break;
			}
			if (l_Now - l_Peer.since > JOIN_TIMEOUT)
			{
				failClient(l_Peer.welcomed ? "The host stopped sending the board" : "The host did not answer the login");
				break;
			}
		}
	}
}

std::unique_ptr<Connection> CollabSession::makeConnection(Socket p_Socket, const Connection::Role p_Role, const uint32_t p_Id)
{
	auto l_Connection = std::make_unique<Connection>(
		std::move(p_Socket),
		p_Role,
		[this, p_Id](Frame p_Frame)
		{
			Event l_Event;
			l_Event.kind = Event::Kind::FrameIn;
			l_Event.conn = p_Id;
			l_Event.frame = std::move(p_Frame);
			post(std::move(l_Event));
		},
		[this, p_Id](const std::string& p_Reason)
		{
			Event l_Event;
			l_Event.kind = Event::Kind::Closed;
			l_Event.conn = p_Id;
			l_Event.text = p_Reason;
			post(std::move(l_Event));
		});
	l_Connection->setBulkSentHandler([this, p_Id]
	{
		Event l_Event;
		l_Event.kind = Event::Kind::BulkSent;
		l_Event.conn = p_Id;
		post(std::move(l_Event));
	});
	return l_Connection;
}

void CollabSession::handleEvent(Event& p_Event)
{
	switch (p_Event.kind)
	{
	case Event::Kind::Preview:
		previewFinished(p_Event.frame);
		break;
	case Event::Kind::BulkSent:
		if (const auto l_It = m_Peers.find(p_Event.conn); l_It != m_Peers.end())
			serveAssets(l_It->second);
		break;
	case Event::Kind::Progress:
		note(p_Event.text);
		break;
	case Event::Kind::Accepted:
		hostAccept(p_Event);
		break;
	case Event::Kind::Connected:
	{
		note("Connected. Waiting for the host to start the login");
		Peer l_Peer;
		l_Peer.id = m_NextPeerId++;
		l_Peer.since = Clock::now();
		const uint32_t l_Id = l_Peer.id;
		l_Peer.connection = makeConnection(std::move(p_Event.socket), Connection::Role::Client, l_Id);
		m_State = LinkState::Joining;
		l_Peer.connection->start();
		m_Peers.emplace(l_Id, std::move(l_Peer));
		break;
	}
	case Event::Kind::ConnectFailed:
		failClient(p_Event.text.empty() ? "Could not connect" : p_Event.text);
		break;
	case Event::Kind::FrameIn:
	{
		const auto l_It = m_Peers.find(p_Event.conn);
		if (l_It == m_Peers.end())
			break;
		l_It->second.since = Clock::now();
		if (m_Role == Role::Host)
			hostFrame(l_It->second, p_Event.frame);
		else
			clientFrame(l_It->second, p_Event.frame);
		break;
	}
	case Event::Kind::Closed:
	{
		const auto l_It = m_Peers.find(p_Event.conn);
		if (l_It == m_Peers.end())
			break;
		if (m_Role == Role::Host)
		{
			dropPeer(p_Event.conn, p_Event.text);
			break;
		}
		const Peer& l_Peer = l_It->second;
		const std::string l_Why = " (" + p_Event.text + ")";
		if (m_State == LinkState::Online)
			failClient("Disconnected from the host" + l_Why);
		else if (p_Event.text == "Incompatible version")
			failClient("This host runs an older version of Inkwell without encrypted sessions. Both sides need the same version.");
		else if (!l_Peer.challenged && (p_Event.text == "Not an Inkwell peer" || p_Event.text == "Invalid key exchange"))
			failClient("Reached something on that address and port, but it is not an Inkwell host: it sent data Inkwell does not understand (a web server, a router page or another program is using this port?)");
		else if (p_Event.text == "Corrupted or tampered data")
			failClient("The connection to the host delivered data that failed its integrity check, so it was cut. Somebody may be tampering with it.");
		else if (!l_Peer.challenged)
			failClient("The connection was accepted but closed before the login started" + l_Why + ". Whatever is on that port is not an Inkwell host, or a router/firewall cut the connection.");
		else if (!l_Peer.welcomed)
			failClient("The host closed the connection during the login" + l_Why);
		else
			failClient("The host closed the connection while sending the board" + l_Why);
		break;
	}
	}
}

// ------------------------------------------------------------------------------------------------ host

void CollabSession::hostAccept(Event& p_Event)
{
	if (m_Peers.size() + 1 >= MAX_USERS)
	{
		note("Refused a connection from " + p_Event.socket.peerAddress() + ": the session is full");
		return; // the socket closes as the event dies
	}
	Peer l_Peer;
	l_Peer.id = m_NextPeerId++;
	l_Peer.since = Clock::now();
	l_Peer.address = p_Event.socket.peerAddress();
	note("Incoming connection from " + l_Peer.address);
	const uint32_t l_Id = l_Peer.id;
	l_Peer.connection = makeConnection(std::move(p_Event.socket), Connection::Role::Server, l_Id);

	Challenge l_Challenge;
	l_Challenge.salt = m_Salt;
	l_Peer.connection->start();
	l_Peer.connection->send(type(MsgType::Challenge), encode(l_Challenge));
	m_Peers.emplace(l_Id, std::move(l_Peer));
}

void CollabSession::hostFrame(Peer& p_Peer, Frame& p_Frame)
{
	if (p_Peer.state == PeerState::Handshake)
	{
		if (p_Frame.type == type(MsgType::Hello))
			hostHello(p_Peer, p_Frame);
		else
			dropPeer(p_Peer.id, "Protocol error");
		return;
	}
	if (p_Frame.type == type(MsgType::Ops))
		hostOps(p_Peer, p_Frame);
	else if (p_Frame.type == type(MsgType::AssetRequest))
		assetRequested(p_Peer, p_Frame);
	else if (p_Frame.type == type(MsgType::AssetChunk))
		assetReceived(p_Peer, p_Frame);
	else if (p_Frame.type == type(MsgType::Cursor))
		cursorReceived(p_Peer, p_Frame);
	else if (p_Frame.type == type(MsgType::StrokePreview))
		strokeReceived(p_Peer, p_Frame);
	else if (p_Frame.type == type(MsgType::VideoState))
		videoStateReceived(p_Peer, p_Frame);
	else if (p_Frame.type == type(MsgType::AssetPreview))
		previewReceived(p_Peer, p_Frame);
	else
		dropPeer(p_Peer.id, "Protocol error");
}

void CollabSession::reject(Peer& p_Peer, const std::string& p_Reason)
{
	note("Rejected " + p_Peer.address + ": " + p_Reason);
	p_Peer.connection->send(type(MsgType::Reject), encode(Reject{ .reason = p_Reason }));
	p_Peer.connection->closeWhenSent("Rejected");
	m_Dying.push_back(std::move(p_Peer.connection));
	m_Peers.erase(p_Peer.id); // p_Peer is gone after this
}

void CollabSession::hostHello(Peer& p_Peer, const Frame& p_Frame)
{
	Hello l_Hello;
	if (!decode(p_Frame.payload, l_Hello))
	{
		dropPeer(p_Peer.id, "Protocol error");
		return;
	}
	const Clock::time_point l_Now = Clock::now();
	Failures& l_Failures = m_Failures[p_Peer.address];
	if (l_Failures.count >= MAX_ATTEMPTS && l_Now - l_Failures.last < ATTEMPT_WINDOW)
	{
		reject(p_Peer, "Too many wrong passwords, wait a minute");
		return;
	}
	if (l_Hello.version != PROTOCOL_VERSION)
	{
		reject(p_Peer, "This host runs an incompatible version of Inkwell");
		return;
	}
	if (!crypto::equal(l_Hello.proof, m_PasswordKey.proof(false, p_Peer.connection->transcript())))
	{
		l_Failures.count = l_Now - l_Failures.last < ATTEMPT_WINDOW ? l_Failures.count + 1 : 1;
		l_Failures.last = l_Now;
		reject(p_Peer, "Wrong password");
		return;
	}
	m_Failures.erase(p_Peer.address);

	const uint16_t l_Slot = pickSlot();
	if (l_Slot == 0)
	{
		reject(p_Peer, "The session is full");
		return;
	}
	// Name: unique among the people here
	std::string l_Name = sanitizeName(l_Hello.name);
	for (int l_Suffix = 2; std::ranges::any_of(m_Users, [&](const UserInfo& p_User) { return p_User.name == l_Name; }); ++l_Suffix)
		l_Name = sanitizeName(l_Hello.name).substr(0, MAX_NAME_BYTES - 5) + " " + std::to_string(l_Suffix);

	std::vector<uint8_t> l_Board = m_Hooks.makeSnapshot ? m_Hooks.makeSnapshot() : std::vector<uint8_t>{};
	if (l_Board.empty() || l_Board.size() > MAX_SNAPSHOT_BYTES)
	{
		reject(p_Peer, "The host could not prepare the board");
		return;
	}

	// Edits already queued reach the others first; the board below contains them already
	flushLocal();

	p_Peer.info = UserInfo{ .slot = l_Slot, .name = l_Name, .color = pickColor(), .host = false };
	Welcome l_Welcome;
	l_Welcome.slot = l_Slot;
	l_Welcome.users = m_Users;
	l_Welcome.users.push_back(p_Peer.info);
	l_Welcome.proof = m_PasswordKey.proof(true, p_Peer.connection->transcript());
	p_Peer.connection->send(type(MsgType::Welcome), encode(l_Welcome));
	for (size_t l_Offset = 0; l_Offset < l_Board.size(); l_Offset += SNAPSHOT_CHUNK)
	{
		const size_t l_Size = std::min(SNAPSHOT_CHUNK, l_Board.size() - l_Offset);
		SnapshotChunk l_Chunk;
		l_Chunk.last = l_Offset + l_Size >= l_Board.size();
		l_Chunk.bytes.assign(l_Board.begin() + static_cast<std::ptrdiff_t>(l_Offset), l_Board.begin() + static_cast<std::ptrdiff_t>(l_Offset + l_Size));
		p_Peer.connection->send(type(MsgType::Snapshot), encode(l_Chunk));
	}
	p_Peer.connection->setMaxFrame(MAX_FRAME);
	p_Peer.state = PeerState::Joined;

	const std::vector<uint8_t> l_Announce = encode(p_Peer.info);
	for (auto& [l_Id, l_Other] : m_Peers)
	{
		if (l_Other.state == PeerState::Joined && l_Id != p_Peer.id)
			l_Other.connection->send(type(MsgType::UserJoined), l_Announce);
	}
	sendVideoStates(p_Peer);
	rebuildUsers();
	note(p_Peer.address + " logged in as \"" + p_Peer.info.name + "\" and received the board (" + std::to_string(l_Board.size() / 1024) + " KiB)");
	tell(p_Peer.info.name + " joined");
}

void CollabSession::hostOps(Peer& p_Peer, const Frame& p_Frame)
{
	OpsMessage l_Message;
	if (!decode(p_Frame.payload, l_Message))
	{
		dropPeer(p_Peer.id, "Protocol error");
		return;
	}
	// The host does not take a read-only guest's word that it behaves: whatever it sends is ignored
	if (p_Peer.info.readOnly)
		return;
	// A peer may only create objects with ids from its own slot (so ids never collide), and may not wipe the board
	std::erase_if(l_Message.ops, [&](const Op& p_Op)
	{
		if (std::holds_alternative<ClearOp>(p_Op))
			return true;
		if (std::holds_alternative<AddOp>(p_Op))
			return idSlotOf(opTarget(p_Op)) != p_Peer.info.slot;
		return false;
	});
	// Locked objects cannot be moved, edited or deleted until unlocked (the editor enforces it on each machine; the host
	// does not take the client's word for it). The sender is sent the real object back so its copy does not drift.
	std::unordered_map<ObjectId, bool> l_Locks;
	std::vector<Op> l_Corrections;
	const auto l_Locked = [&](const ObjectId p_Id)
	{
		if (const auto l_It = l_Locks.find(p_Id); l_It != l_Locks.end())
			return l_It->second;
		const Object* l_Object = m_Document.find(p_Id);
		return l_Object != nullptr && l_Object->locked;
	};
	std::erase_if(l_Message.ops, [&](const Op& p_Op)
	{
		if (const auto* l_Lock = std::get_if<LockOp>(&p_Op))
		{
			l_Locks[l_Lock->id] = l_Lock->locked;
			return false;
		}
		const bool l_Edits = std::holds_alternative<RemoveOp>(p_Op) || std::holds_alternative<TransformOp>(p_Op) || std::holds_alternative<UpdateOp>(p_Op) || std::holds_alternative<GroupOp>(p_Op);
		const ObjectId l_Target = l_Edits ? opTarget(p_Op) : INVALID_OBJECT_ID;
		if (l_Target == INVALID_OBJECT_ID || !l_Locked(l_Target))
			return false;
		const Object* l_Object = m_Document.find(l_Target);
		if (l_Object != nullptr && !l_Locks.contains(l_Target))
		{
			if (std::holds_alternative<RemoveOp>(p_Op))
			{
				const size_t l_Index = m_Document.indexOf(l_Target);
				l_Corrections.push_back(AddOp{ .below = l_Index != SIZE_MAX && l_Index > 0 ? m_Document.objects()[l_Index - 1]->id : INVALID_OBJECT_ID, .locked = true, .object = encodeObject(*l_Object) });
					if (l_Object->group != INVALID_OBJECT_ID)
						l_Corrections.push_back(GroupOp{ .id = l_Target, .group = l_Object->group });
			}
			else
			{
				if (const auto* l_GroupEdit = std::get_if<GroupOp>(&p_Op))
					l_Corrections.push_back(GroupOp{ .id = l_GroupEdit->id, .group = l_Object->group });
				else
					l_Corrections.push_back(UpdateOp{ .object = encodeObject(*l_Object) });
			}
		}
		return true;
	});
	if (!l_Corrections.empty())
		p_Peer.connection->send(type(MsgType::Ops), encodeOps(l_Corrections));
	applyRemoteOps(l_Message.ops, p_Peer.id);
}

void CollabSession::dropPeer(const uint32_t p_Id, const std::string& p_Reason)
{
	const auto l_It = m_Peers.find(p_Id);
	if (l_It == m_Peers.end())
		return;
	const bool l_Joined = l_It->second.state == PeerState::Joined;
	const UserInfo l_Info = l_It->second.info;
	const std::string l_Address = l_It->second.address;
	m_Peers.erase(l_It);
	for (auto& [l_Asset, l_Waiting] : m_AssetWaiters)
		std::erase_if(l_Waiting, [&](const Waiter& p_Waiter) { return p_Waiter.peer == p_Id; });
	if (!l_Joined)
	{
		note(l_Address + " dropped before finishing the login (" + p_Reason + ")");
		return;
	}
	note(l_Info.name + " (" + l_Address + ") disconnected (" + p_Reason + ")");
	m_Cursors.erase(l_Info.slot);
	forgetStrokes(l_Info.slot);
	++m_CursorsRevision;
	rebuildUsers();
	const std::vector<uint8_t> l_Announce = encode(UserLeft{ .slot = l_Info.slot });
	for (auto& [l_Id, l_Other] : m_Peers)
	{
		if (l_Other.state == PeerState::Joined)
			l_Other.connection->send(type(MsgType::UserLeft), l_Announce);
	}
	tell(l_Info.name + " left");
	abandonDownloadsFrom(p_Id, l_Info.name);
}

// ------------------------------------------------------------------------------------------------ both

void CollabSession::applyRemoteOps(const std::span<const Op> p_Ops, const uint32_t p_From)
{
	std::vector<Op> l_Accepted;
	std::vector<ObjectId> l_Media; // objects that may use a file we do not have yet
	l_Accepted.reserve(p_Ops.size());
	for (const Op& l_Op : p_Ops)
	{
		if (!m_Replicator.applyRemote(l_Op).ok)
			continue;
		if (std::holds_alternative<AddOp>(l_Op))
			m_OrderDirty = true;
		if (std::holds_alternative<AddOp>(l_Op) || std::holds_alternative<UpdateOp>(l_Op))
			l_Media.push_back(opTarget(l_Op));
		else if (std::holds_alternative<ClearOp>(l_Op))
			resetAssetTransfers();
		if (m_Role == Role::Host)
			l_Accepted.push_back(l_Op);
	}
	if (!l_Accepted.empty())
		sendOps(l_Accepted, p_From);
	if (!l_Media.empty())
	{
		// Files come from whoever sent the edit: the host from the client, a client from the host
		const auto l_Source = m_Role == Role::Host ? m_Peers.find(p_From) : m_Peers.begin();
		if (l_Source != m_Peers.end())
			wantAssets(l_Source->second, l_Media);
	}
}

// ------------------------------------------------------------------------------------------------ pointers

void CollabSession::setLocalCursor(const DVec2 p_World, const bool p_Visible)
{
	if (p_Visible == m_LocalCursorVisible && (!p_Visible || (p_World.x == m_LocalCursor.x && p_World.y == m_LocalCursor.y)))
		return;
	if (p_Visible)
		m_LocalCursor = p_World; // a hidden pointer keeps its last place, so it fades out where it was
	m_LocalCursorVisible = p_Visible;
	m_CursorDirty = true;
}

void CollabSession::sendCursor()
{
	m_CursorDirty = false;
	m_LastCursorSent = Clock::now();
	const std::vector<uint8_t> l_Payload = encode(CursorMessage{ .slot = m_LocalSlot, .x = m_LocalCursor.x, .y = m_LocalCursor.y, .visible = m_LocalCursorVisible });
	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (l_Peer.state == PeerState::Joined)
			l_Peer.connection->send(type(MsgType::Cursor), l_Payload, Priority::Realtime);
	}
}

void CollabSession::cursorReceived(Peer& p_Peer, const Frame& p_Frame)
{
	CursorMessage l_Cursor;
	if (!decode(p_Frame.payload, l_Cursor))
	{
		if (m_Role == Role::Host)
			dropPeer(p_Peer.id, "Protocol error");
		else
			failClient("The host sent a damaged pointer update");
		return;
	}
	const Clock::time_point l_Now = Clock::now();
	if (m_Role == Role::Host)
	{
		// Whoever sends is who it is about; a flood is cut to what people can see anyway
		if (l_Cursor.visible && l_Now - p_Peer.lastCursor < std::chrono::milliseconds(10))
			return;
		p_Peer.lastCursor = l_Now;
		l_Cursor.slot = p_Peer.info.slot;
		const std::vector<uint8_t> l_Payload = encode(l_Cursor);
		for (auto& [l_Id, l_Other] : m_Peers)
		{
			if (l_Other.state == PeerState::Joined && l_Id != p_Peer.id)
				l_Other.connection->send(type(MsgType::Cursor), l_Payload, Priority::Realtime);
		}
	}
	else if (l_Cursor.slot == m_LocalSlot || user(l_Cursor.slot) == nullptr)
	{
		return; // ourselves, or somebody we have not been told about
	}
	RemoteCursor& l_Remote = m_Cursors[l_Cursor.slot];
	if (l_Cursor.visible || l_Remote.updated == Clock::time_point{})
		l_Remote.position = DVec2{ l_Cursor.x, l_Cursor.y };
	l_Remote.visible = l_Cursor.visible;
	l_Remote.updated = l_Now;
	++m_CursorsRevision;
}

// ------------------------------------------------------------------------------------------------ video playback

void CollabSession::sendVideoState(const ObjectId p_Id, const bool p_Playing, const double p_Position)
{
	if (m_State != LinkState::Online || p_Id == INVALID_OBJECT_ID)
		return;
	m_VideoStates[p_Id] = VideoClock{ .playing = p_Playing, .position = p_Position, .at = Clock::now() };
	const std::vector<uint8_t> l_Payload = encode(VideoStateMessage{ .id = p_Id, .playing = p_Playing, .position = std::clamp(p_Position, 0.0, 86400.0) });
	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (l_Peer.state == PeerState::Joined)
			l_Peer.connection->send(type(MsgType::VideoState), l_Payload);
	}
}

void CollabSession::videoStateReceived(Peer& p_Peer, const Frame& p_Frame)
{
	VideoStateMessage l_Message;
	if (!decode(p_Frame.payload, l_Message))
	{
		if (m_Role == Role::Host)
			dropPeer(p_Peer.id, "Protocol error");
		else
			failClient("The host sent a damaged video update");
		return;
	}
	if (m_Role == Role::Host)
	{
		if (p_Peer.info.readOnly)
			return; // a read-only guest does not control playback
		const Clock::time_point l_Now = Clock::now();
		if (l_Now - p_Peer.videoWindow > std::chrono::seconds(1))
		{
			p_Peer.videoWindow = l_Now;
			p_Peer.videoCount = 0;
		}
		if (++p_Peer.videoCount > MAX_VIDEO_UPDATES_PER_SECOND)
		{
			dropPeer(p_Peer.id, "Too many video updates");
			return;
		}
	}
	const Object* l_Object = m_Document.find(l_Message.id);
	if (l_Object == nullptr || l_Object->video() == nullptr)
		return; // not (or no longer) a video here
	m_VideoStates[l_Message.id] = VideoClock{ .playing = l_Message.playing, .position = l_Message.position, .at = Clock::now() };
	if (m_VideoStates.size() > MAX_VIDEO_STATES)
	{
		std::erase_if(m_VideoStates, [&](const auto& p_Entry) { const Object* l_Other = m_Document.find(p_Entry.first); return l_Other == nullptr || l_Other->video() == nullptr; });
	}
	if (m_VideoEvents.size() < MAX_VIDEO_STATES)
		m_VideoEvents.push_back(VideoEvent{ .id = l_Message.id, .playing = l_Message.playing, .position = l_Message.position });
	if (m_Role == Role::Host)
	{
		const std::vector<uint8_t> l_Payload = encode(l_Message);
		for (auto& [l_Id, l_Other] : m_Peers)
		{
			if (l_Other.state == PeerState::Joined && l_Id != p_Peer.id)
				l_Other.connection->send(type(MsgType::VideoState), l_Payload);
		}
	}
}

void CollabSession::sendVideoStates(Peer& p_Peer)
{
	const Clock::time_point l_Now = Clock::now();
	std::erase_if(m_VideoStates, [&](const auto& p_Entry) { const Object* l_Object = m_Document.find(p_Entry.first); return l_Object == nullptr || l_Object->video() == nullptr; });
	for (const auto& [l_Id, l_Clock] : m_VideoStates)
	{
		// A playing video has moved on since it was last heard about
		const double l_Elapsed = l_Clock.playing ? std::chrono::duration<double>(l_Now - l_Clock.at).count() : 0.0;
		p_Peer.connection->send(type(MsgType::VideoState), encode(VideoStateMessage{ .id = l_Id, .playing = l_Clock.playing, .position = std::clamp(l_Clock.position + l_Elapsed, 0.0, 86400.0) }));
	}
}

std::vector<CollabSession::VideoEvent> CollabSession::takeVideoEvents()
{
	std::vector<VideoEvent> l_Events;
	l_Events.swap(m_VideoEvents);
	return l_Events;
}

// ------------------------------------------------------------------------------------------------ lines being drawn

void CollabSession::setLocalStroke(const LocalStroke* p_Stroke)
{
	if (m_State != LinkState::Online)
	{
		m_StrokeActive = false;
		m_StrokeDirty = false;
		return;
	}
	if (p_Stroke == nullptr)
	{
		if (m_StrokeActive)
		{
			m_StrokeDirty = false;
			sendStroke(true);
			m_StrokeActive = false;
		}
		return;
	}
	// A different line than the one we were sending (it ended and another began between two calls)
	const bool l_Fresh = !m_StrokeActive || p_Stroke->stable.size() < m_StrokeSent || p_Stroke->origin.x != m_StrokeOrigin.x || p_Stroke->origin.y != m_StrokeOrigin.y;
	if (l_Fresh)
	{
		if (m_StrokeActive)
			sendStroke(true);
		++m_StrokeId;
		m_StrokeSent = 0;
		m_StrokeActive = true;
		m_LastStrokeSent = {};
	}
	m_StrokeOrigin = p_Stroke->origin;
	m_StrokeColor = p_Stroke->color;
	m_StrokeStable = static_cast<uint32_t>(p_Stroke->stable.size());
	if (p_Stroke->stable.size() + p_Stroke->tail.size() > MAX_LIVE_STROKE_POINTS)
		return; // far longer than anyone draws: the others see the finished line
	m_StrokeUnsent.assign(p_Stroke->stable.begin() + m_StrokeSent, p_Stroke->stable.end());
	m_StrokeUnsent.insert(m_StrokeUnsent.end(), p_Stroke->tail.begin(), p_Stroke->tail.end());
	m_StrokeDirty = !m_StrokeUnsent.empty();
}

void CollabSession::sendStroke(const bool p_End)
{
	m_StrokeDirty = false;
	m_LastStrokeSent = Clock::now();
	StrokePreviewMessage l_Message;
	l_Message.slot = m_LocalSlot;
	l_Message.stroke = m_StrokeId;
	l_Message.end = p_End;
	l_Message.color[0] = m_StrokeColor.r;
	l_Message.color[1] = m_StrokeColor.g;
	l_Message.color[2] = m_StrokeColor.b;
	l_Message.color[3] = m_StrokeColor.a;
	l_Message.originX = m_StrokeOrigin.x;
	l_Message.originY = m_StrokeOrigin.y;
	std::vector<std::vector<uint8_t>> l_Payloads;
	if (p_End)
	{
		l_Payloads.push_back(encode(l_Message));
	}
	else
	{
		for (size_t l_Offset = 0; l_Offset < m_StrokeUnsent.size(); l_Offset += MAX_STROKE_POINTS_PER_MESSAGE)
		{
			const size_t l_Count = std::min<size_t>(MAX_STROKE_POINTS_PER_MESSAGE, m_StrokeUnsent.size() - l_Offset);
			l_Message.base = m_StrokeSent + static_cast<uint32_t>(l_Offset);
			l_Message.points.assign(m_StrokeUnsent.begin() + l_Offset, m_StrokeUnsent.begin() + l_Offset + l_Count);
			l_Payloads.push_back(encode(l_Message));
		}
		m_StrokeSent = m_StrokeStable;
		m_StrokeUnsent.clear();
	}
	// Updates may be overtaken by newer ones, but the end must come after everything the line was part of
	const Priority l_Priority = p_End ? Priority::Normal : Priority::Realtime;
	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (l_Peer.state != PeerState::Joined)
			continue;
		for (const std::vector<uint8_t>& l_Payload : l_Payloads)
			l_Peer.connection->send(type(MsgType::StrokePreview), l_Payload, l_Priority);
	}
}

void CollabSession::strokeReceived(Peer& p_Peer, const Frame& p_Frame)
{
	StrokePreviewMessage l_Message;
	if (!decode(p_Frame.payload, l_Message))
	{
		if (m_Role == Role::Host)
			dropPeer(p_Peer.id, "Protocol error");
		else
			failClient("The host sent a damaged line update");
		return;
	}
	if (m_Role == Role::Host)
	{
		if (p_Peer.info.readOnly)
			return; // a read-only guest draws nothing
		// A person draws about 30 updates a second; far more is somebody trying to flood everyone
		const Clock::time_point l_Now = Clock::now();
		if (l_Now - p_Peer.strokeWindow > std::chrono::seconds(1))
		{
			p_Peer.strokeWindow = l_Now;
			p_Peer.strokeCount = 0;
		}
		if (++p_Peer.strokeCount > MAX_STROKE_UPDATES_PER_SECOND)
		{
			dropPeer(p_Peer.id, "Too many line updates");
			return;
		}
		l_Message.slot = p_Peer.info.slot;
		const std::vector<uint8_t> l_Payload = encode(l_Message);
		const Priority l_Priority = l_Message.end ? Priority::Normal : Priority::Realtime;
		for (auto& [l_Id, l_Other] : m_Peers)
		{
			if (l_Other.state == PeerState::Joined && l_Id != p_Peer.id)
				l_Other.connection->send(type(MsgType::StrokePreview), l_Payload, l_Priority);
		}
	}
	else if (l_Message.slot == m_LocalSlot || user(l_Message.slot) == nullptr)
	{
		return; // ourselves, or somebody we have not been told about
	}
	applyStroke(l_Message);
}

void CollabSession::applyStroke(const StrokePreviewMessage& p_Message)
{
	const auto l_Current = m_RemoteStrokes.find(p_Message.slot);
	if (p_Message.end)
	{
		uint32_t& l_Ended = m_EndedStrokes[p_Message.slot];
		l_Ended = std::max(l_Ended, p_Message.stroke);
		// Another line may already have started: the end of the earlier one is not about it
		if (l_Current != m_RemoteStrokes.end() && l_Current->second.stroke == p_Message.stroke)
		{
			m_RemoteStrokes.erase(l_Current);
			++m_StrokesRevision;
		}
		return;
	}
	if (const auto l_Ended = m_EndedStrokes.find(p_Message.slot); l_Ended != m_EndedStrokes.end() && p_Message.stroke <= l_Ended->second)
		return; // an update that was overtaken by the end of its line
	RemoteStroke* l_Stroke = nullptr;
	if (l_Current != m_RemoteStrokes.end() && l_Current->second.stroke == p_Message.stroke)
	{
		l_Stroke = &l_Current->second;
	}
	else
	{
		if (p_Message.base != 0)
			return; // the beginning of this line never reached us
		l_Stroke = &m_RemoteStrokes[p_Message.slot];
		*l_Stroke = RemoteStroke{};
		l_Stroke->stroke = p_Message.stroke;
	}
	if (p_Message.base > l_Stroke->points.size() || p_Message.base + p_Message.points.size() > MAX_LIVE_STROKE_POINTS)
		return; // a gap, or absurdly long
	l_Stroke->points.resize(p_Message.base);
	l_Stroke->points.insert(l_Stroke->points.end(), p_Message.points.begin(), p_Message.points.end());
	l_Stroke->origin = DVec2{ p_Message.originX, p_Message.originY };
	l_Stroke->color = Color{ .r = p_Message.color[0], .g = p_Message.color[1], .b = p_Message.color[2], .a = p_Message.color[3] };
	l_Stroke->updated = Clock::now();
	++m_StrokesRevision;
}

void CollabSession::forgetStrokes(const uint16_t p_Slot)
{
	if (m_RemoteStrokes.erase(p_Slot) != 0)
		++m_StrokesRevision;
	m_EndedStrokes.erase(p_Slot);
}

std::vector<CollabSession::StrokeView> CollabSession::liveStrokes() const
{
	std::vector<StrokeView> l_Views;
	l_Views.reserve(m_RemoteStrokes.size());
	for (const auto& [l_Slot, l_Stroke] : m_RemoteStrokes)
	{
		if (!l_Stroke.points.empty())
			l_Views.push_back(StrokeView{ .slot = l_Slot, .origin = l_Stroke.origin, .color = l_Stroke.color, .points = l_Stroke.points });
	}
	return l_Views;
}

std::vector<CollabSession::CursorView> CollabSession::cursors() const
{
	std::vector<CursorView> l_Views;
	const Clock::time_point l_Now = Clock::now();
	l_Views.reserve(m_Cursors.size());
	for (const auto& [l_Slot, l_Cursor] : m_Cursors)
		l_Views.push_back(CursorView{ .slot = l_Slot, .position = l_Cursor.position, .visible = l_Cursor.visible, .ageSeconds = std::chrono::duration<double>(l_Now - l_Cursor.updated).count() });
	return l_Views;
}

// ------------------------------------------------------------------------------------------------ pictures and videos

CollabSession::AssetProgress CollabSession::assetProgress() const
{
	AssetProgress l_Progress;
	l_Progress.files = static_cast<uint32_t>(m_Downloads.size());
	for (const auto& [l_Id, l_Download] : m_Downloads)
	{
		l_Progress.received += l_Download.received;
		l_Progress.expected += l_Download.total;
	}
	return l_Progress;
}

double CollabSession::assetFraction(const AssetId p_Id) const
{
	const auto l_It = m_Downloads.find(p_Id);
	if (l_It == m_Downloads.end())
		return -1.0;
	return l_It->second.total == 0 ? 0.0 : static_cast<double>(l_It->second.received) / static_cast<double>(l_It->second.total);
}

void CollabSession::resetAssetTransfers()
{
	// Asset ids restart with a new board, so nothing in flight may be mistaken for the new files
	clearDownloads();
	m_AssetWaiters.clear();
	m_Stalled.clear();
	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		l_Peer.uploads.clear();
		if (l_Peer.connection)
			l_Peer.connection->dropBulk();
	}
	++m_AssetsRevision;
}

// Forgets every transfer. A file that was being played while it arrived will never be whole: whoever waits for its
// bytes is let go.
void CollabSession::clearDownloads()
{
	for (auto& [l_Id, l_Download] : m_Downloads)
	{
		if (l_Download.file)
			l_Download.file->abandon();
		const ImageAsset* l_Asset = m_Document.findAsset(l_Id);
		if (l_Download.file && l_Asset != nullptr && l_Asset->stream == l_Download.file)
			m_Document.eraseAsset(l_Id);
	}
	m_Downloads.clear();
	m_DownloadBytes = 0;
}

// The asset, once it is whole (one still arriving is not there yet for anybody but a player)
const ImageAsset* CollabSession::completeAsset(const AssetId p_Id) const
{
	const ImageAsset* l_Asset = m_Document.findAsset(p_Id);
	return l_Asset != nullptr && l_Asset->complete() ? l_Asset : nullptr;
}

void CollabSession::sendPreview(Peer& p_Peer, const AssetId p_Id)
{
	const AssetPreview* l_Preview = m_Document.findPreview(p_Id);
	if (l_Preview == nullptr)
		return;
	p_Peer.connection->send(type(MsgType::AssetPreview), encode(AssetPreviewMessage{ .id = p_Id, .width = l_Preview->width, .height = l_Preview->height, .rgba = l_Preview->rgba }));
}

void CollabSession::startPreviewJob(const AssetId p_Id)
{
	if (!m_PreviewActive.insert(p_Id).second)
		return; // already being made
	const ImageAsset* l_Asset = completeAsset(p_Id);
	if (l_Asset == nullptr)
	{
		m_PreviewActive.erase(p_Id);
		return;
	}
	{
		const std::lock_guard l_Lock(m_PreviewMutex);
		m_PreviewJobs.emplace_back(p_Id, l_Asset->source()); // memory is copied: the worker must not depend on the board
	}
	if (!m_PreviewThread.joinable())
		m_PreviewThread = std::thread([this] { previewWorker(); });
	m_PreviewWake.notify_one();
}

void CollabSession::previewWorker()
{
	while (true)
	{
		std::pair<AssetId, std::shared_ptr<const ByteSource>> l_Job;
		{
			std::unique_lock l_Lock(m_PreviewMutex);
			m_PreviewWake.wait(l_Lock, [&] { return m_PreviewStop || !m_PreviewJobs.empty(); });
			if (m_PreviewStop)
				return;
			l_Job = std::move(m_PreviewJobs.front());
			m_PreviewJobs.pop_front();
		}
		AssetPreviewMessage l_Message;
		l_Message.id = l_Job.first;
		if (m_Hooks.makePreview)
		{
			if (const std::optional<AssetPreview> l_Preview = m_Hooks.makePreview(l_Job.second);
			    l_Preview && l_Preview->width > 0 && l_Preview->height > 0 && l_Preview->width <= MAX_PREVIEW_SIDE && l_Preview->height <= MAX_PREVIEW_SIDE && l_Preview->rgba.size() == static_cast<size_t>(l_Preview->width) * l_Preview->height * 4)
			{
				l_Message.width = l_Preview->width;
				l_Message.height = l_Preview->height;
				l_Message.rgba = l_Preview->rgba;
			}
		}
		Event l_Event;
		l_Event.kind = Event::Kind::Preview;
		l_Event.frame.payload = encode(l_Message);
		post(std::move(l_Event));
	}
}

void CollabSession::previewFinished(const Frame& p_Frame)
{
	AssetPreviewMessage l_Message;
	if (!decode(p_Frame.payload, l_Message))
		return;
	m_PreviewActive.erase(l_Message.id);
	if (l_Message.width == 0)
		m_PreviewFailed.insert(l_Message.id);
	else
		m_Document.setPreview(l_Message.id, AssetPreview{ .width = l_Message.width, .height = l_Message.height, .rgba = std::move(l_Message.rgba) });
	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (!l_Peer.uploads.empty())
			serveAssets(l_Peer);
	}
}

void CollabSession::previewReceived(Peer& p_Peer, const Frame& p_Frame)
{
	AssetPreviewMessage l_Message;
	if (!decode(p_Frame.payload, l_Message))
	{
		if (m_Role == Role::Host)
			dropPeer(p_Peer.id, "Protocol error");
		else
			failClient("The host sent a damaged thumbnail");
		return;
	}
	// Only for files we are waiting for from this peer
	const auto l_It = m_Downloads.find(l_Message.id);
	if (l_Message.width == 0 || l_It == m_Downloads.end() || l_It->second.from != p_Peer.id || m_Document.findAsset(l_Message.id) != nullptr)
		return;
	const AssetId l_Id = l_Message.id;
	m_Document.setPreview(l_Id, AssetPreview{ .width = l_Message.width, .height = l_Message.height, .rgba = std::move(l_Message.rgba) });
	++m_AssetsRevision;
	if (m_Role == Role::Host)
	{
		// Passed on to the ones who asked before it was here
		if (const auto l_Waiting = m_AssetWaiters.find(l_Id); l_Waiting != m_AssetWaiters.end())
		{
			for (const Waiter& l_Waiter : l_Waiting->second)
			{
				if (const auto l_Peer = m_Peers.find(l_Waiter.peer); l_Peer != m_Peers.end())
					sendPreview(l_Peer->second, l_Id);
			}
		}
	}
}

void CollabSession::dropDownload(const AssetId p_Id, const bool p_Lost)
{
	const auto l_It = m_Downloads.find(p_Id);
	if (l_It == m_Downloads.end())
		return;
	m_DownloadBytes -= l_It->second.bytes.size();
	if (l_It->second.file)
	{
		l_It->second.file->abandon();
		const ImageAsset* l_Partial = m_Document.findAsset(p_Id);
		if (l_Partial != nullptr && l_Partial->stream == l_It->second.file)
			m_Document.eraseAsset(p_Id);
	}
	m_Downloads.erase(l_It);
	m_AssetWaiters.erase(p_Id);
	++m_AssetsRevision;
	if (p_Lost && m_Hooks.assetLost)
		m_Hooks.assetLost(p_Id);
}

void CollabSession::pruneDownloads()
{
	m_PrunedRevision = m_Document.revision();
	if (m_Downloads.empty())
		return;
	std::unordered_set<AssetId> l_Used;
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
	{
		if (const ImageData* l_Image = l_Object->image())
			l_Used.insert(l_Image->asset);
		else if (const VideoData* l_Video = l_Object->video())
			l_Used.insert(l_Video->asset);
	}
	std::vector<AssetId> l_Unused;
	for (const auto& [l_Id, l_Download] : m_Downloads)
	{
		if (!l_Used.contains(l_Id))
			l_Unused.push_back(l_Id);
	}
	for (const AssetId l_Id : l_Unused)
		dropDownload(l_Id, true); // deleted while it was arriving: the delete cannot be undone
}

void CollabSession::abandonDownloadsFrom(const uint32_t p_PeerId, const std::string& p_Who)
{
	std::unordered_set<AssetId> l_Lost;
	for (const auto& [l_Id, l_Download] : m_Downloads)
	{
		if (l_Download.from == p_PeerId)
			l_Lost.insert(l_Id);
	}
	if (l_Lost.empty())
		return;
	// The objects that were waiting for those files have nothing to show: they go, for everybody
	std::vector<ObjectId> l_Remove;
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
	{
		const ImageData* l_Image = l_Object->image();
		const VideoData* l_Video = l_Object->video();
		const AssetId l_Asset = l_Image != nullptr ? l_Image->asset : (l_Video != nullptr ? l_Video->asset : INVALID_ASSET_ID);
		if (l_Lost.contains(l_Asset))
			l_Remove.push_back(l_Object->id);
	}
	for (const AssetId l_Id : l_Lost)
		dropDownload(l_Id, true);
	for (const ObjectId l_Id : l_Remove)
		(void)m_Document.take(l_Id); // an ordinary local edit: it is replicated to the others
	if (!l_Remove.empty() && m_Hooks.remoteChange)
		m_Hooks.remoteChange(l_Remove, true);
	const std::string l_What = l_Lost.size() == 1 ? "a picture/video" : std::to_string(l_Lost.size()) + " pictures/videos";
	note(p_Who + " left before " + l_What + " finished arriving: cancelled");
	tell(p_Who + " left before " + l_What + " finished arriving. " + (l_Remove.empty() ? "" : "Removed from the board."));
}

void CollabSession::wantAssets(Peer& p_Source, const std::span<const ObjectId> p_Objects)
{
	for (const ObjectId l_Object : p_Objects)
	{
		const Object* l_Found = m_Document.find(l_Object);
		if (l_Found == nullptr)
			continue;
		AssetId l_Asset = INVALID_ASSET_ID;
		if (const ImageData* l_Image = l_Found->image())
			l_Asset = l_Image->asset;
		else if (const VideoData* l_Video = l_Found->video())
			l_Asset = l_Video->asset;
		if (l_Asset == INVALID_ASSET_ID || m_Document.findAsset(l_Asset) != nullptr || m_Downloads.contains(l_Asset))
			continue;
		// A client may only supply files it made itself, so it cannot plant a file under someone else's id. When somebody
		// else brought the object back (an undo) while the owner is here, the file is stalled; the owner adding it to the board again starts a fresh transfer.
		if (m_Role == Role::Host && idSlotOf(l_Asset) != p_Source.info.slot)
		{
			if (peerOfSlot(idSlotOf(l_Asset)) != nullptr)
				stallAsset(l_Asset);
			continue;
		}
		if (m_Downloads.size() >= MAX_PENDING_ASSETS)
			return;
		m_Stalled.erase(l_Asset); // put on the board again by whoever has it: that is a fresh transfer from the start
		Download& l_Download = m_Downloads[l_Asset];
		l_Download.from = p_Source.id;
		l_Download.ticket = ++m_NextTicket;
		p_Source.connection->send(type(MsgType::AssetRequest), encode(AssetRequest{ .id = l_Asset, .ticket = l_Download.ticket }));
	}
}

void CollabSession::wantAllMissingAssets(Peer& p_Source)
{
	std::vector<ObjectId> l_Objects;
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
	{
		if (l_Object->image() != nullptr || l_Object->video() != nullptr)
			l_Objects.push_back(l_Object->id);
	}
	wantAssets(p_Source, l_Objects);
}

void CollabSession::queueUpload(Peer& p_Peer, const AssetId p_Id, const uint32_t p_Ticket)
{
	// Asked for again (the first transfer was cancelled): start over under the new ticket
	for (Peer::Upload& l_Upload : p_Peer.uploads)
	{
		if (l_Upload.id == p_Id)
		{
			l_Upload.ticket = p_Ticket;
			l_Upload.offset = 0;
			return;
		}
	}
	if (p_Peer.uploads.size() >= MAX_PENDING_ASSETS)
		return;
	p_Peer.uploads.push_back(Peer::Upload{ .id = p_Id, .ticket = p_Ticket });
}

void CollabSession::assetRequested(Peer& p_Peer, const Frame& p_Frame)
{
	AssetRequest l_Request;
	if (!decode(p_Frame.payload, l_Request))
	{
		if (m_Role == Role::Host)
			dropPeer(p_Peer.id, "Protocol error");
		else
			failClient("The host sent a damaged file request");
		return;
	}
	if (completeAsset(l_Request.id) != nullptr)
	{
		queueUpload(p_Peer, l_Request.id, l_Request.ticket);
		serveAssets(p_Peer);
	}
	else if (m_Role == Role::Host && m_Downloads.contains(l_Request.id))
	{
		// The file is on its way here from another person: pass it on when it lands
		std::vector<Waiter>& l_Waiting = m_AssetWaiters[l_Request.id];
		const auto l_Known = std::ranges::find_if(l_Waiting, [&](const Waiter& p_Waiter) { return p_Waiter.peer == p_Peer.id; });
		if (l_Known != l_Waiting.end())
		{
			l_Known->ticket = l_Request.ticket;
		}
		else if (l_Waiting.size() < MAX_USERS)
		{
			l_Waiting.push_back(Waiter{ .peer = p_Peer.id, .ticket = l_Request.ticket });
			if (m_Document.findPreview(l_Request.id) != nullptr)
				sendPreview(p_Peer, l_Request.id);
		}
	}
	else if (m_Role == Role::Host)
	{
		// Nobody is sending it: say so, instead of leaving the asker waiting
		if (m_Stalled.contains(l_Request.id))
			p_Peer.connection->send(type(MsgType::AssetStalled), encode(AssetRequest{ .id = l_Request.id }));
		else if (peerOfSlot(idSlotOf(l_Request.id)) != nullptr)
			stallAsset(l_Request.id);
		else
			p_Peer.connection->send(type(MsgType::AssetStalled), encode(AssetRequest{ .id = l_Request.id }));
	}
}

CollabSession::Peer* CollabSession::peerOfSlot(const uint16_t p_Slot)
{
	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (l_Peer.state == PeerState::Joined && l_Peer.info.slot == p_Slot)
			return &l_Peer;
	}
	return nullptr;
}

void CollabSession::stallAsset(const AssetId p_Id)
{
	if (!m_Stalled.insert(p_Id).second)
		return;
	++m_AssetsRevision;
	note("A file is stalled: nobody is sending it");
	const std::vector<uint8_t> l_Payload = encode(AssetRequest{ .id = p_Id });
	for (auto& [l_Id, l_Peer] : m_Peers)
	{
		if (l_Peer.state == PeerState::Joined)
			l_Peer.connection->send(type(MsgType::AssetStalled), l_Payload);
	}
}

void CollabSession::assetStalledReceived(Peer& p_Peer, const Frame& p_Frame)
{
	(void)p_Peer;
	AssetRequest l_Message;
	if (!decode(p_Frame.payload, l_Message))
	{
		failClient("The host sent a damaged message");
		return;
	}
	dropDownload(l_Message.id); // the transfer we waited for is not coming
	if (m_Document.findAsset(l_Message.id) == nullptr)
		m_Stalled.insert(l_Message.id);
	++m_AssetsRevision;
}

void CollabSession::resolveLocalMedia()
{
	if (m_LocalMedia.empty())
		return;
	std::vector<ObjectId> l_Objects;
	l_Objects.swap(m_LocalMedia);
	if (m_State != LinkState::Online)
		return;
	if (m_Role == Role::Client)
	{
		if (m_Peers.empty())
			return;
		wantAssets(m_Peers.begin()->second, l_Objects); // an undo can bring back a picture we gave up on
		return;
	}
	for (const ObjectId l_Object : l_Objects)
	{
		const Object* l_Found = m_Document.find(l_Object);
		if (l_Found == nullptr)
			continue;
		const AssetId l_Asset = l_Found->image() != nullptr ? l_Found->image()->asset : (l_Found->video() != nullptr ? l_Found->video()->asset : INVALID_ASSET_ID);
		if (l_Asset == INVALID_ASSET_ID || m_Document.findAsset(l_Asset) != nullptr || m_Downloads.contains(l_Asset))
			continue;
		if (peerOfSlot(idSlotOf(l_Asset)) != nullptr)
			stallAsset(l_Asset);
	}
}

void CollabSession::serveAssets(Peer& p_Peer)
{
	while (!p_Peer.uploads.empty() && p_Peer.connection->queuedBulkBytes() < ASSET_QUEUE_BYTES)
	{
		Peer::Upload& l_Upload = p_Peer.uploads.front();
		const ImageAsset* l_Asset = completeAsset(l_Upload.id);
		if (l_Asset == nullptr || l_Upload.offset >= l_Asset->size())
		{
			p_Peer.uploads.pop_front();
			continue;
		}
		// The thumbnail goes first so the other side has something to show while the file streams
		if (!l_Upload.previewDone)
		{
			if (m_Document.findPreview(l_Upload.id) != nullptr)
			{
				sendPreview(p_Peer, l_Upload.id);
				l_Upload.previewDone = true;
			}
			else if (m_PreviewFailed.contains(l_Upload.id) || !m_Hooks.makePreview)
			{
				l_Upload.previewDone = true;
			}
			else
			{
				startPreviewJob(l_Upload.id);
				return; // previewFinished() calls again
			}
		}
		AssetChunk l_Chunk;
		l_Chunk.id = l_Asset->id;
		l_Chunk.ticket = l_Upload.ticket;
		l_Chunk.total = l_Asset->size();
		l_Chunk.offset = l_Upload.offset;
		l_Chunk.width = l_Asset->width;
		l_Chunk.height = l_Asset->height;
		l_Chunk.name = l_Asset->name.substr(0, MAX_ASSET_NAME_BYTES);
		const size_t l_Size = std::min<uint64_t>(ASSET_CHUNK, l_Asset->size() - l_Upload.offset);
		l_Chunk.bytes.resize(l_Size);
		if (l_Asset->read(l_Upload.offset, l_Chunk.bytes.data(), l_Size) != l_Size)
		{
			// The file this was made from is gone (a drive was removed, say): the other side cannot be given the rest
			note("Could not read \"" + l_Asset->name + "\" to send it; the transfer was cancelled");
			p_Peer.uploads.pop_front();
			p_Peer.connection->send(type(MsgType::AssetStalled), encode(AssetRequest{ .id = l_Asset->id }));
			continue;
		}
		p_Peer.connection->send(type(MsgType::AssetChunk), encode(l_Chunk), Priority::Bulk);
		l_Upload.offset += l_Size;
		if (l_Upload.offset >= l_Asset->size())
			p_Peer.uploads.pop_front();
	}
}

void CollabSession::assetReceived(Peer& p_Peer, const Frame& p_Frame)
{
	AssetChunk l_Chunk;
	if (!decode(p_Frame.payload, l_Chunk))
	{
		if (m_Role == Role::Host)
			dropPeer(p_Peer.id, "Protocol error");
		else
			failClient("The host sent a damaged picture or video");
		return;
	}
	// Only files we asked this peer for are taken; anything else (also pieces of a board that has since been replaced) is ignored
	const auto l_It = m_Downloads.find(l_Chunk.id);
	if (l_It == m_Downloads.end() || l_It->second.from != p_Peer.id || l_It->second.ticket != l_Chunk.ticket)
		return; // not asked for, or a piece of a transfer that was cancelled since
	Download& l_Download = l_It->second;
	const AssetId l_Id = l_Chunk.id;
	const bool l_First = l_Download.total == 0;
	if (l_First ? l_Chunk.offset != 0 : (l_Chunk.offset != l_Download.received || l_Chunk.total != l_Download.total))
	{
		note("Dropped a damaged transfer of \"" + l_Download.name + "\"");
		dropDownload(l_Id);
		return;
	}
	if (!l_Download.file && m_DownloadBytes + l_Chunk.bytes.size() > MAX_DOWNLOAD_BYTES)
	{
		note("Dropped a transfer: too much data waiting to be completed");
		dropDownload(l_Id);
		return;
	}
	if (l_First)
	{
		l_Download.total = l_Chunk.total;
		l_Download.width = l_Chunk.width;
		l_Download.height = l_Chunk.height;
		l_Download.name = std::move(l_Chunk.name);
		if (l_Download.total >= BIG_LOG_BYTES)
			note("Receiving \"" + l_Download.name + "\" (" + std::to_string(l_Download.total >> 20) + " MiB)");
		if (l_Download.total >= LARGE_ASSET_BYTES)
		{
			// Too big for memory: the pieces go to a temporary file, and the video can be played while they arrive
			std::string l_Problem;
			const std::filesystem::path l_Folder = m_CacheDir.empty() ? std::filesystem::temp_directory_path() / "Inkwell" : m_CacheDir;
			l_Download.file = FileSource::createTemporary(l_Folder, l_Download.total, l_Problem);
			if (!l_Download.file)
			{
				const std::string l_Name = l_Download.name;
				note("Could not receive \"" + l_Name + "\": " + l_Problem);
				tell("\"" + l_Name + "\" could not be received: " + l_Problem);
				dropDownload(l_Id);
				m_Stalled.insert(l_Id);
				return;
			}
			ImageAsset l_Partial;
			l_Partial.id = l_Id;
			l_Partial.width = l_Download.width;
			l_Partial.height = l_Download.height;
			l_Partial.name = l_Download.name;
			l_Partial.stream = l_Download.file;
			if (!m_Document.insertAsset(std::move(l_Partial)))
			{
				note("A received file clashed with one already on the board and was dropped");
				l_Download.file->abandon();
				l_Download.file.reset();
				dropDownload(l_Id);
				return;
			}
		}
	}
	if (l_Download.file)
	{
		std::string l_Problem;
		if (!l_Download.file->append(l_Chunk.bytes, l_Problem))
		{
			const std::string l_Name = l_Download.name;
			note("Could not store \"" + l_Name + "\": " + l_Problem);
			tell("\"" + l_Name + "\" could not be received: " + l_Problem);
			dropDownload(l_Id);
			m_Stalled.insert(l_Id);
			return;
		}
	}
	else
	{
		l_Download.bytes.insert(l_Download.bytes.end(), l_Chunk.bytes.begin(), l_Chunk.bytes.end());
		m_DownloadBytes += l_Chunk.bytes.size();
	}
	l_Download.received += l_Chunk.bytes.size();
	if (l_Download.received < l_Download.total)
	{
		++m_AssetsRevision; // progress changed
		return;
	}

	// Whole. A big file is already on the board (it has been playable since its first piece); a small one is added now.
	if (!l_Download.file)
	{
		ImageAsset l_Asset;
		l_Asset.id = l_Id;
		l_Asset.width = l_Download.width;
		l_Asset.height = l_Download.height;
		l_Asset.name = std::move(l_Download.name);
		l_Asset.bytes = std::move(l_Download.bytes);
		m_DownloadBytes -= l_Asset.bytes.size();
		if (!m_Document.insertAsset(std::move(l_Asset)))
			note("A received file clashed with one already on the board and was dropped");
	}
	m_Downloads.erase(l_It);
	m_Stalled.erase(l_Id);
	++m_AssetsRevision;

	// Others that asked the host for it while it was still on its way
	if (const auto l_Waiting = m_AssetWaiters.find(l_Id); l_Waiting != m_AssetWaiters.end())
	{
		for (const Waiter& l_Waiter : l_Waiting->second)
		{
			if (const auto l_Peer = m_Peers.find(l_Waiter.peer); l_Peer != m_Peers.end())
			{
				queueUpload(l_Peer->second, l_Id, l_Waiter.ticket);
				serveAssets(l_Peer->second);
			}
		}
		m_AssetWaiters.erase(l_Waiting);
	}
}

// ------------------------------------------------------------------------------------------------ client

void CollabSession::clientFrame(Peer& p_Peer, Frame& p_Frame)
{
	const auto l_Protocol = [&] { failClient("The host sent something unexpected (message type " + std::to_string(p_Frame.type) + "). It may run a different version of Inkwell."); };
	switch (static_cast<MsgType>(p_Frame.type))
	{
	case MsgType::Challenge:
	{
		Challenge l_Challenge;
		if (p_Peer.challenged || !decode(p_Frame.payload, l_Challenge))
			return l_Protocol();
		p_Peer.challenged = true;
		// The host answers the Hello with Welcome and the board straight away. The reader thread would reject those big
		// frames before the main thread handled Welcome, so the limit has to be raised before the Hello goes out.
		p_Peer.connection->setMaxFrame(MAX_FRAME);
		note("Encrypted channel ready. Sending the name and password proof");
		m_PasswordKey = crypto::PasswordKey(m_Password, l_Challenge.salt);
		crypto::wipe({ reinterpret_cast<uint8_t*>(m_Password.data()), m_Password.size() });
		m_Password.clear();
		Hello l_Hello;
		l_Hello.name = m_Name;
		l_Hello.proof = m_PasswordKey.proof(false, p_Peer.connection->transcript());
		p_Peer.connection->send(type(MsgType::Hello), encode(l_Hello));
		return;
	}
	case MsgType::Reject:
	{
		Reject l_Reject;
		if (!decode(p_Frame.payload, l_Reject))
			return l_Protocol();
		failClient(l_Reject.reason.empty() ? "The host refused the connection" : l_Reject.reason);
		return;
	}
	case MsgType::Welcome:
	{
		Welcome l_Welcome;
		if (!p_Peer.challenged || p_Peer.welcomed || !decode(p_Frame.payload, l_Welcome))
			return l_Protocol();
		// Only the real host (or somebody who knows the password) can show this for exactly this key exchange
		if (!crypto::equal(l_Welcome.proof, m_PasswordKey.proof(true, p_Peer.connection->transcript())))
		{
			failClient("The host could not prove it knows the password for this connection. Somebody may be intercepting it.");
			return;
		}
		p_Peer.welcomed = true;
		note("Password accepted. Receiving the board");
		m_LocalSlot = l_Welcome.slot;
		m_Users = std::move(l_Welcome.users);
		++m_UsersRevision;
		p_Peer.connection->setMaxFrame(MAX_FRAME);
		return;
	}
	case MsgType::Snapshot:
		if (!p_Peer.welcomed || p_Peer.state == PeerState::Joined)
			return l_Protocol();
		clientSnapshot(p_Peer, p_Frame);
		return;
	case MsgType::Ops:
	{
		OpsMessage l_Message;
		if (p_Peer.state != PeerState::Joined || !decode(p_Frame.payload, l_Message))
			return l_Protocol();
		applyRemoteOps(l_Message.ops, 0);
		return;
	}
	case MsgType::UserJoined:
	{
		UserInfo l_User;
		if (p_Peer.state != PeerState::Joined || !decode(p_Frame.payload, l_User))
			return l_Protocol();
		if (user(l_User.slot) == nullptr)
		{
			m_Users.push_back(l_User);
			++m_UsersRevision;
			tell(l_User.name + " joined");
		}
		return;
	}
	case MsgType::UserUpdate:
	{
		UserInfo l_Update;
		if (p_Peer.state != PeerState::Joined || !decode(p_Frame.payload, l_Update))
			return l_Protocol();
		for (UserInfo& l_User : m_Users)
		{
			if (l_User.slot != l_Update.slot)
				continue;
			const bool l_Changed = l_User.readOnly != l_Update.readOnly;
			l_User.readOnly = l_Update.readOnly;
			++m_UsersRevision;
			if (l_Changed && l_Update.slot == m_LocalSlot)
			{
				if (l_Update.readOnly)
				{
					m_Pending.clear(); // what was not sent yet would be ignored anyway
					m_PendingMove.clear();
					m_StrokeActive = false;
					m_StrokeDirty = false;
				}
				tell(l_Update.readOnly ? "The host made you read-only: you can watch but not change the board" : "The host lets you edit again");
			}
			break;
		}
		return;
	}
	case MsgType::UserLeft:
	{
		UserLeft l_Left;
		if (p_Peer.state != PeerState::Joined || !decode(p_Frame.payload, l_Left))
			return l_Protocol();
		if (const UserInfo* l_User = user(l_Left.slot))
		{
			tell(l_User->name + " left");
			m_Cursors.erase(l_Left.slot);
			forgetStrokes(l_Left.slot);
			std::erase_if(m_Users, [&](const UserInfo& p_Other) { return p_Other.slot == l_Left.slot; });
			++m_UsersRevision;
		}
		return;
	}
	case MsgType::AssetRequest:
		if (p_Peer.state != PeerState::Joined)
			return l_Protocol();
		assetRequested(p_Peer, p_Frame);
		return;
	case MsgType::AssetChunk:
		if (p_Peer.state != PeerState::Joined)
			return l_Protocol();
		assetReceived(p_Peer, p_Frame);
		return;
	case MsgType::Cursor:
		if (p_Peer.state != PeerState::Joined)
			return l_Protocol();
		cursorReceived(p_Peer, p_Frame);
		return;
	case MsgType::StrokePreview:
		if (p_Peer.state != PeerState::Joined)
			return l_Protocol();
		strokeReceived(p_Peer, p_Frame);
		return;
	case MsgType::VideoState:
		if (p_Peer.state != PeerState::Joined)
			return l_Protocol();
		videoStateReceived(p_Peer, p_Frame);
		return;
	case MsgType::AssetPreview:
		if (p_Peer.state != PeerState::Joined)
			return l_Protocol();
		previewReceived(p_Peer, p_Frame);
		return;
	case MsgType::AssetStalled:
		if (p_Peer.state != PeerState::Joined)
			return l_Protocol();
		assetStalledReceived(p_Peer, p_Frame);
		return;
	default:
		return l_Protocol();
	}
}

void CollabSession::clientSnapshot(Peer& p_Peer, const Frame& p_Frame)
{
	SnapshotChunk l_Chunk;
	if (!decode(p_Frame.payload, l_Chunk) || p_Peer.snapshot.size() + l_Chunk.bytes.size() > MAX_SNAPSHOT_BYTES)
	{
		failClient("The host sent a damaged board");
		return;
	}
	p_Peer.snapshot.insert(p_Peer.snapshot.end(), l_Chunk.bytes.begin(), l_Chunk.bytes.end());
	if (!l_Chunk.last)
		return;

	std::vector<uint8_t> l_Board = std::move(p_Peer.snapshot);
	p_Peer.snapshot = {};
	bool l_Loaded = false;
	{
		// Loading replaces the whole board: that is not an edit to send back
		const Replicator::Pause l_Pause(m_Replicator);
		m_Document.setIdSlot(m_LocalSlot);
		l_Loaded = m_Hooks.loadSnapshot && m_Hooks.loadSnapshot(l_Board);
	}
	if (!l_Loaded)
	{
		failClient("Could not read the board from the host");
		return;
	}
	p_Peer.state = PeerState::Joined;
	m_State = LinkState::Online;
	note("Board received (" + std::to_string(l_Board.size() / 1024) + " KiB). You are in");
	m_Pending.clear();
	m_PendingMove.clear();
	setIdle(false);
	tell("Joined the session");
	// Big pictures and videos are not part of the board: ask for the ones it uses
	wantAllMissingAssets(p_Peer);
	if (!m_Downloads.empty())
		note("Requested " + std::to_string(m_Downloads.size()) + " picture(s)/video(s) from the host");
}
} // namespace wb::net
