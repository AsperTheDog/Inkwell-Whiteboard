module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

module wb.steam;

import wb.io.locale;
import wb.net.protocol;
import wb.net.stream;

namespace wb::steam
{
namespace
{
// ------------------------------------------------------------------------------------------------ the Steam flat C API
// Just the pieces used here, declared by hand (see steam.cppm). The layouts follow steamnetworkingtypes.h and
// steam_api_common.h; the sizes that matter are checked below so a wrong guess fails to compile instead of misbehaving.

using HSteamPipe = int32_t;
using HConn = uint32_t;
using HListen = uint32_t;
using HPollGroup = uint32_t;
using SteamId = uint64_t;
using ApiCall = uint64_t;
using Ptr = void*;

constexpr int RESULT_OK = 1;
constexpr int RESULT_LIMIT_EXCEEDED = 25;
constexpr int SEND_RELIABLE_NO_NAGLE = 8 | 1;
constexpr int STATE_CONNECTING = 1;
constexpr int STATE_CONNECTED = 3;
constexpr int STATE_CLOSED_BY_PEER = 4;
constexpr int STATE_PROBLEM = 5;
constexpr int CALLBACK_API_CALL_COMPLETED = 703;
constexpr const char* CONNECT_ARGUMENT = "+connect_steam"; // also what Steam passes on the command line when it starts the program for a join
constexpr int CALLBACK_RICH_PRESENCE_JOIN = 337; // "Join Game" in the friends list, or an invite accepted
constexpr int CALLBACK_LOBBY_JOIN = 333;         // Join Game / an invite for a session that is a Steam lobby
constexpr int CALLBACK_LOBBY_ENTER = 504;
constexpr int CALLBACK_LOBBY_CREATED = 513;
constexpr int LOBBY_TYPE_FRIENDS_ONLY = 1; // seen by friends only, never in a search
constexpr int LOBBY_MAX_MEMBERS = 64;
constexpr int VIRTUAL_PORT = 0;
constexpr size_t SEND_CHUNK = 64 * 1024; // well below the send buffer, so a full buffer is only a short wait
constexpr int MAX_RECEIVE_BATCH = 64;
constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(30);

struct NetIdentity
{
	int type;
	int size;
	union
	{
		uint64_t steamId;
		char raw[128];
	};
};
static_assert(sizeof(NetIdentity) == 136);

struct NetIpAddr
{
	uint8_t ip[16];
	uint16_t port;
};

struct NetConnectionInfo
{
	NetIdentity identity;
	int64_t userData;
	HListen listenSocket;
	NetIpAddr address;
	uint16_t pad;
	uint32_t popRemote;
	uint32_t popRelay;
	int state;
	int endReason;
	char endDebug[128];
	char description[128];
	int flags;
	uint32_t reserved[63];
};
static_assert(sizeof(NetConnectionInfo) == 696);
static_assert(offsetof(NetConnectionInfo, state) == 176);
static_assert(offsetof(NetConnectionInfo, endDebug) == 184);

struct StatusChanged
{
	HConn conn;
	NetConnectionInfo info;
	int oldState;
};
static_assert(sizeof(StatusChanged) == 712);
static_assert(offsetof(StatusChanged, info) == 8);

// Only the start of SteamNetworkingMessage_t, which has been stable for years
struct NetMessage
{
	void* data;
	int size;
	HConn conn;
};

struct CallbackMsg
{
	int32_t user;
	int callback;
	uint8_t* param;
	int size;
};
static_assert(sizeof(CallbackMsg) == 24);

#pragma pack(push, 4)
struct ApiCallCompleted
{
	ApiCall call;
	int callback;
	uint32_t size;
};
struct LobbyCreated
{
	int result;
	uint64_t lobby;
};
struct JoinRequested
{
	uint64_t friendId;
	char connect[256];
};
#pragma pack(pop)
static_assert(sizeof(JoinRequested) == 264);
static_assert(sizeof(LobbyCreated) == 12);

using StatusCallback = void (*)(StatusChanged*);

// A function the library exports as SteamAPI_<name>
#define STEAM_FN(p_Return, p_Name, ...) \
	using p_Name##_t = p_Return (*)(__VA_ARGS__); \
	p_Name##_t p_Name = nullptr;

struct Api
{
	SDL_SharedObject* library = nullptr;

	STEAM_FN(int, InitFlat, char*)
	STEAM_FN(void, Shutdown)
	STEAM_FN(void, ManualDispatch_Init)
	STEAM_FN(void, ManualDispatch_RunFrame, HSteamPipe)
	STEAM_FN(bool, ManualDispatch_GetNextCallback, HSteamPipe, CallbackMsg*)
	STEAM_FN(void, ManualDispatch_FreeLastCallback, HSteamPipe)
	STEAM_FN(bool, ManualDispatch_GetAPICallResult, HSteamPipe, ApiCall, void*, int, int, bool*)
	STEAM_FN(HSteamPipe, GetHSteamPipe)

	STEAM_FN(SteamId, ISteamUser_GetSteamID, Ptr)
	STEAM_FN(const char*, ISteamFriends_GetPersonaName, Ptr)
	STEAM_FN(bool, ISteamFriends_SetRichPresence, Ptr, const char*, const char*)
	STEAM_FN(void, ISteamFriends_ClearRichPresence, Ptr)
	STEAM_FN(const char*, ISteamFriends_GetFriendRichPresence, Ptr, SteamId, const char*)

	STEAM_FN(void, ISteamNetworkingUtils_InitRelayNetworkAccess, Ptr)
	STEAM_FN(void, ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged, Ptr, StatusCallback)
	STEAM_FN(void, SteamNetworkingIdentity_SetSteamID64, NetIdentity*, SteamId)
	STEAM_FN(SteamId, SteamNetworkingIdentity_GetSteamID64, NetIdentity*)
	STEAM_FN(void, SteamNetworkingMessage_t_Release, NetMessage*)

	STEAM_FN(HListen, ISteamNetworkingSockets_CreateListenSocketP2P, Ptr, int, int, const void*)
	STEAM_FN(HConn, ISteamNetworkingSockets_ConnectP2P, Ptr, const NetIdentity*, int, int, const void*)
	STEAM_FN(int, ISteamNetworkingSockets_AcceptConnection, Ptr, HConn)
	STEAM_FN(bool, ISteamNetworkingSockets_CloseConnection, Ptr, HConn, int, const char*, bool)
	STEAM_FN(bool, ISteamNetworkingSockets_CloseListenSocket, Ptr, HListen)
	STEAM_FN(HPollGroup, ISteamNetworkingSockets_CreatePollGroup, Ptr)
	STEAM_FN(bool, ISteamNetworkingSockets_DestroyPollGroup, Ptr, HPollGroup)
	STEAM_FN(bool, ISteamNetworkingSockets_SetConnectionPollGroup, Ptr, HConn, HPollGroup)
	STEAM_FN(int, ISteamNetworkingSockets_SendMessageToConnection, Ptr, HConn, const void*, uint32_t, int, int64_t*)
	STEAM_FN(int, ISteamNetworkingSockets_ReceiveMessagesOnPollGroup, Ptr, HPollGroup, NetMessage**, int)
	STEAM_FN(void, ISteamNetworkingSockets_RunCallbacks, Ptr)

	STEAM_FN(ApiCall, ISteamMatchmaking_CreateLobby, Ptr, int, int)
	STEAM_FN(bool, ISteamMatchmaking_SetLobbyData, Ptr, SteamId, const char*, const char*)
	STEAM_FN(const char*, ISteamMatchmaking_GetLobbyData, Ptr, SteamId, const char*)
	STEAM_FN(void, ISteamMatchmaking_LeaveLobby, Ptr, SteamId)
	STEAM_FN(ApiCall, ISteamMatchmaking_JoinLobby, Ptr, SteamId)

	Ptr sockets = nullptr;
	Ptr utils = nullptr;
	Ptr matchmaking = nullptr;
	Ptr user = nullptr;
	Ptr friends = nullptr;
};

// Looks the function up; an exported name that is missing makes the whole table unusable
#define STEAM_LOAD(p_Name) \
	p_Api.p_Name = reinterpret_cast<Api::p_Name##_t>(SDL_LoadFunction(p_Api.library, "SteamAPI_" #p_Name)); \
	l_Missing += p_Api.p_Name == nullptr ? (" " #p_Name) : "";

// Interface accessors carry the interface version in their name (SteamAPI_SteamUser_v023, ...), and every SDK release may
// bump it. The newest one the library has is used: the functions called through it keep their signatures between versions.
Ptr callAccessor(SDL_SharedObject* p_Library, const std::string& p_Prefix)
{
	for (int l_Version = 60; l_Version >= 1; --l_Version)
	{
		char l_Suffix[8];
		std::snprintf(l_Suffix, sizeof(l_Suffix), "%03d", l_Version);
		if (const SDL_FunctionPointer l_Function = SDL_LoadFunction(p_Library, (p_Prefix + l_Suffix).c_str()))
			return reinterpret_cast<Ptr (*)()>(l_Function)();
	}
	return nullptr;
}

std::string loadApi(Api& p_Api)
{
#ifdef _WIN32
	constexpr const char* LIBRARY = "steam_api64.dll";
#elif defined(__APPLE__)
	constexpr const char* LIBRARY = "libsteam_api.dylib";
#else
	constexpr const char* LIBRARY = "libsteam_api.so";
#endif
	p_Api.library = SDL_LoadObject(LIBRARY);
	if (p_Api.library == nullptr)
		return std::string(LIBRARY) + " not found";
	std::string l_Missing;
	STEAM_LOAD(InitFlat)
	STEAM_LOAD(Shutdown)
	STEAM_LOAD(ManualDispatch_Init)
	STEAM_LOAD(ManualDispatch_RunFrame)
	STEAM_LOAD(ManualDispatch_GetNextCallback)
	STEAM_LOAD(ManualDispatch_FreeLastCallback)
	STEAM_LOAD(ManualDispatch_GetAPICallResult)
	STEAM_LOAD(GetHSteamPipe)
	STEAM_LOAD(ISteamUser_GetSteamID)
	STEAM_LOAD(ISteamFriends_GetPersonaName)
	STEAM_LOAD(ISteamFriends_SetRichPresence)
	STEAM_LOAD(ISteamFriends_ClearRichPresence)
	STEAM_LOAD(ISteamFriends_GetFriendRichPresence)
	STEAM_LOAD(ISteamNetworkingUtils_InitRelayNetworkAccess)
	STEAM_LOAD(ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged)
	STEAM_LOAD(SteamNetworkingIdentity_SetSteamID64)
	STEAM_LOAD(SteamNetworkingIdentity_GetSteamID64)
	STEAM_LOAD(SteamNetworkingMessage_t_Release)
	STEAM_LOAD(ISteamNetworkingSockets_CreateListenSocketP2P)
	STEAM_LOAD(ISteamNetworkingSockets_ConnectP2P)
	STEAM_LOAD(ISteamNetworkingSockets_AcceptConnection)
	STEAM_LOAD(ISteamNetworkingSockets_CloseConnection)
	STEAM_LOAD(ISteamNetworkingSockets_CloseListenSocket)
	STEAM_LOAD(ISteamNetworkingSockets_CreatePollGroup)
	STEAM_LOAD(ISteamNetworkingSockets_DestroyPollGroup)
	STEAM_LOAD(ISteamNetworkingSockets_SetConnectionPollGroup)
	STEAM_LOAD(ISteamNetworkingSockets_SendMessageToConnection)
	STEAM_LOAD(ISteamNetworkingSockets_ReceiveMessagesOnPollGroup)
	STEAM_LOAD(ISteamNetworkingSockets_RunCallbacks)
	STEAM_LOAD(ISteamMatchmaking_CreateLobby)
	STEAM_LOAD(ISteamMatchmaking_SetLobbyData)
	STEAM_LOAD(ISteamMatchmaking_GetLobbyData)
	STEAM_LOAD(ISteamMatchmaking_LeaveLobby)
	STEAM_LOAD(ISteamMatchmaking_JoinLobby)
	if (!l_Missing.empty())
		return "the Steam library is too old or too new, missing:" + l_Missing;
	return {};
}

// Where the (single, process wide) Steam state is, for the C callback that cannot carry a pointer
std::atomic<void*> g_Impl{ nullptr };
} // namespace

// ------------------------------------------------------------------------------------------------ the connections

namespace
{
struct ListenState;

// One P2P connection, shared by the pump (which fills it) and the Stream (which drains it)
struct Link
{
	HConn conn = 0;
	HListen listenSocket = 0; // incoming ones: which listener they came through
	std::string peer;
	std::shared_ptr<ListenState> owner; // incoming ones until handed to the session
	bool handedOut = false;

	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::vector<uint8_t>> chunks;
	size_t front = 0; // bytes of chunks.front() already read
	bool connected = false;
	bool closed = false;
	std::string reason;
};

struct ListenState
{
	HListen handle = 0;
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::unique_ptr<net::Stream>> ready;
	bool closed = false;
};
} // namespace

struct Steam::Impl : std::enable_shared_from_this<Steam::Impl>
{
	Api api;
	std::atomic<bool> alive{ false };
	HSteamPipe pipe = 0;
	HPollGroup group = 0;

	// P2P: everything that touches the maps happens with `big` held (the pump holds it while it runs callbacks)
	std::recursive_mutex big;
	std::unordered_map<HConn, std::shared_ptr<Link>> links;
	std::shared_ptr<ListenState> listener;
	std::vector<HListen> retired; // closed listeners whose connections are still open
	std::thread pumpThread;
	std::atomic<bool> pumpStop{ false };

	// Lobbies (main thread)
	struct Pending
	{
		ApiCall call = 0;
		int callback = 0;
	};
	std::vector<Pending> pending;
	SteamId lobby = 0;       // our own, once created
	bool lobbyWanted = false; // setJoinable(true) and not yet false
	bool lobbyCreating = false;
	std::string userIdText;

	// Friends joining through Steam's own buttons (main thread)
	bool joinable = false;
	std::deque<uint64_t> joinRequests;
	void lobbyJoinRequested(SteamId p_Lobby);

	void startPump();
	void pumpLoop();
	void onStatus(StatusChanged& p_Change);
	void receiveAll();
	void retireCheck();
	void dropLink(HConn p_Conn);
	void closeLink(const std::shared_ptr<Link>& p_Link);
	void publishLobby();
	void collect(ApiCall p_Call, int p_Callback);
	void shutdown();
};

namespace
{
void statusTrampoline(StatusChanged* p_Change)
{
	if (auto* l_Impl = static_cast<Steam::Impl*>(g_Impl.load()))
		l_Impl->onStatus(*p_Change);
}
} // namespace

namespace
{
class SteamStream final : public net::Stream
{
public:
	SteamStream(std::shared_ptr<Steam::Impl> p_Impl, std::shared_ptr<Link> p_Link) : m_Impl(std::move(p_Impl)), m_Link(std::move(p_Link)) {}
	~SteamStream() override { close(); }

	bool sendAll(std::span<const uint8_t> p_Data) override
	{
		while (!p_Data.empty())
		{
			{
				const std::lock_guard l_Lock(m_Link->mutex);
				if (m_Link->closed)
					return false;
			}
			if (!m_Impl->alive.load())
				return false;
			const size_t l_Chunk = std::min(p_Data.size(), SEND_CHUNK);
			const int l_Result = m_Impl->api.ISteamNetworkingSockets_SendMessageToConnection(m_Impl->api.sockets, m_Link->conn, p_Data.data(), static_cast<uint32_t>(l_Chunk), SEND_RELIABLE_NO_NAGLE, nullptr);
			if (l_Result == RESULT_OK)
			{
				p_Data = p_Data.subspan(l_Chunk);
			}
			else if (l_Result == RESULT_LIMIT_EXCEEDED)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2)); // the send buffer is full: the link is slower than the writer
			}
			else
			{
				return false;
			}
		}
		return true;
	}

	bool recvAll(std::span<uint8_t> p_Data) override
	{
		std::unique_lock l_Lock(m_Link->mutex);
		while (!p_Data.empty())
		{
			m_Link->wake.wait(l_Lock, [&] { return !m_Link->chunks.empty() || m_Link->closed; });
			if (m_Link->chunks.empty())
				return false;
			const std::vector<uint8_t>& l_Front = m_Link->chunks.front();
			const size_t l_Count = std::min(p_Data.size(), l_Front.size() - m_Link->front);
			std::memcpy(p_Data.data(), l_Front.data() + m_Link->front, l_Count);
			p_Data = p_Data.subspan(l_Count);
			m_Link->front += l_Count;
			if (m_Link->front == l_Front.size())
			{
				m_Link->chunks.pop_front();
				m_Link->front = 0;
			}
		}
		return true;
	}

	void shutdown() override
	{
		{
			const std::lock_guard l_Lock(m_Link->mutex);
			m_Link->closed = true;
		}
		m_Link->wake.notify_all();
	}

	void close() override
	{
		shutdown();
		if (!m_Closed.exchange(true) && m_Impl->alive.load())
			m_Impl->closeLink(m_Link);
	}

	std::string peerAddress() const override { return m_Link->peer; }

private:
	std::shared_ptr<Steam::Impl> m_Impl;
	std::shared_ptr<Link> m_Link;
	std::atomic<bool> m_Closed{ false };
};

class SteamListener final : public net::Listener
{
public:
	SteamListener(std::shared_ptr<Steam::Impl> p_Impl, std::shared_ptr<ListenState> p_State) : m_Impl(std::move(p_Impl)), m_State(std::move(p_State)) {}
	~SteamListener() override { close(); }

	std::unique_ptr<net::Stream> accept(const int p_TimeoutMs) override
	{
		std::unique_lock l_Lock(m_State->mutex);
		m_State->wake.wait_for(l_Lock, std::chrono::milliseconds(p_TimeoutMs), [&] { return !m_State->ready.empty() || m_State->closed; });
		if (m_State->ready.empty())
			return nullptr;
		std::unique_ptr<net::Stream> l_Stream = std::move(m_State->ready.front());
		m_State->ready.pop_front();
		return l_Stream;
	}

	void close() override
	{
		// Streams nobody took are destroyed outside the lock: closing one needs the pump's lock, which the pump holds while it
		// waits for this one
		std::deque<std::unique_ptr<net::Stream>> l_Unclaimed;
		{
			const std::lock_guard l_Lock(m_State->mutex);
			if (m_State->closed)
				return;
			m_State->closed = true;
			l_Unclaimed = std::move(m_State->ready);
			m_State->ready.clear();
		}
		m_State->wake.notify_all();
		l_Unclaimed.clear();
		if (!m_Impl->alive.load())
			return;
		// Closing the listen socket would cut the people already connected through it, before the session had the chance
		// to say goodbye to them. It is closed by the pump once the last one is gone.
		const std::lock_guard l_Big(m_Impl->big);
		if (m_Impl->listener == m_State)
			m_Impl->listener.reset();
		m_Impl->retired.push_back(m_State->handle);
	}

private:
	std::shared_ptr<Steam::Impl> m_Impl;
	std::shared_ptr<ListenState> m_State;
};
} // namespace

void Steam::Impl::startPump()
{
	if (pumpThread.joinable())
		return;
	pumpStop = false;
	pumpThread = std::thread([this] { pumpLoop(); });
}

void Steam::Impl::pumpLoop()
{
	while (!pumpStop.load())
	{
		bool l_Busy = false;
		{
			const std::lock_guard l_Lock(big);
			api.ISteamNetworkingSockets_RunCallbacks(api.sockets);
			receiveAll();
			retireCheck();
			l_Busy = !links.empty() || listener != nullptr;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(l_Busy ? 1 : 50));
	}
}

void Steam::Impl::receiveAll()
{
	NetMessage* l_Messages[MAX_RECEIVE_BATCH];
	while (true)
	{
		const int l_Count = api.ISteamNetworkingSockets_ReceiveMessagesOnPollGroup(api.sockets, group, l_Messages, MAX_RECEIVE_BATCH);
		if (l_Count <= 0)
			return;
		for (int i = 0; i < l_Count; ++i)
		{
			NetMessage* l_Message = l_Messages[i];
			if (const auto l_It = links.find(l_Message->conn); l_It != links.end() && l_Message->size > 0)
			{
				Link& l_Link = *l_It->second;
				const auto* l_Bytes = static_cast<const uint8_t*>(l_Message->data);
				{
					const std::lock_guard l_Lock(l_Link.mutex);
					if (!l_Link.closed)
						l_Link.chunks.emplace_back(l_Bytes, l_Bytes + l_Message->size);
				}
				l_Link.wake.notify_all();
			}
			api.SteamNetworkingMessage_t_Release(l_Message);
		}
	}
}

void Steam::Impl::retireCheck()
{
	for (size_t i = 0; i < retired.size();)
	{
		const HListen l_Handle = retired[i];
		const bool l_InUse = std::ranges::any_of(links, [&](const auto& p_Entry) { return p_Entry.second->listenSocket == l_Handle; });
		if (l_InUse)
		{
			++i;
			continue;
		}
		api.ISteamNetworkingSockets_CloseListenSocket(api.sockets, l_Handle);
		retired.erase(retired.begin() + static_cast<std::ptrdiff_t>(i));
	}
}

void Steam::Impl::dropLink(const HConn p_Conn)
{
	api.ISteamNetworkingSockets_CloseConnection(api.sockets, p_Conn, 0, nullptr, false);
	links.erase(p_Conn);
}

// The stream is done with the connection: what it still has queued is sent, then it closes
void Steam::Impl::closeLink(const std::shared_ptr<Link>& p_Link)
{
	const std::lock_guard l_Lock(big);
	if (links.erase(p_Link->conn) > 0)
		api.ISteamNetworkingSockets_CloseConnection(api.sockets, p_Link->conn, 0, "Closed", true);
}

// Runs on the pump thread, inside RunCallbacks, with `big` held
void Steam::Impl::onStatus(StatusChanged& p_Change)
{
	const int l_State = p_Change.info.state;
	const bool l_Ended = l_State == STATE_CLOSED_BY_PEER || l_State == STATE_PROBLEM;
	const auto l_It = links.find(p_Change.conn);
	if (l_It == links.end())
	{
		// Somebody new at a listen socket of ours
		if (l_State == STATE_CONNECTING && p_Change.info.listenSocket != 0 && listener && !listener->closed && listener->handle == p_Change.info.listenSocket)
		{
			if (api.ISteamNetworkingSockets_AcceptConnection(api.sockets, p_Change.conn) != RESULT_OK)
			{
				api.ISteamNetworkingSockets_CloseConnection(api.sockets, p_Change.conn, 0, nullptr, false);
				return;
			}
			auto l_Link = std::make_shared<Link>();
			l_Link->conn = p_Change.conn;
			l_Link->listenSocket = p_Change.info.listenSocket;
			l_Link->owner = listener;
			l_Link->peer = "steam:" + std::to_string(api.SteamNetworkingIdentity_GetSteamID64(&p_Change.info.identity));
			api.ISteamNetworkingSockets_SetConnectionPollGroup(api.sockets, p_Change.conn, group);
			links.emplace(p_Change.conn, std::move(l_Link));
		}
		else if (l_State == STATE_CONNECTING || l_Ended)
		{
			api.ISteamNetworkingSockets_CloseConnection(api.sockets, p_Change.conn, 0, nullptr, false);
		}
		return;
	}

	const std::shared_ptr<Link> l_Link = l_It->second;
	if (l_State == STATE_CONNECTED)
	{
		{
			const std::lock_guard l_Lock(l_Link->mutex);
			l_Link->connected = true;
		}
		l_Link->wake.notify_all();
		if (l_Link->owner && !l_Link->handedOut)
		{
			l_Link->handedOut = true;
			const std::shared_ptr<ListenState> l_Owner = std::move(l_Link->owner);
			{
				const std::lock_guard l_Lock(l_Owner->mutex);
				if (!l_Owner->closed)
					l_Owner->ready.push_back(std::make_unique<SteamStream>(shared_from_this(), l_Link));
			}
			l_Owner->wake.notify_all();
		}
	}
	else if (l_Ended)
	{
		{
			const std::lock_guard l_Lock(l_Link->mutex);
			l_Link->closed = true;
			l_Link->reason = p_Change.info.endDebug;
		}
		l_Link->wake.notify_all();
		// Nobody got the stream (the person gave up while connecting): the connection is ours to free
		if (l_Link->owner && !l_Link->handedOut)
			dropLink(p_Change.conn);
	}
}

// ------------------------------------------------------------------------------------------------ lobbies

void Steam::Impl::publishLobby()
{
	if (lobby == 0)
		return;
	// What a Join Game or an accepted invite needs to find the host (a lobby's data is only readable from inside it)
	api.ISteamMatchmaking_SetLobbyData(api.matchmaking, lobby, "host", userIdText.c_str());
}

// An asynchronous request finished
void Steam::Impl::collect(const ApiCall p_Call, const int p_Callback)
{
	const auto l_It = std::ranges::find(pending, p_Call, &Pending::call);
	if (l_It == pending.end())
		return;
	const int l_Expected = l_It->callback;
	pending.erase(l_It);
	alignas(8) uint8_t l_Buffer[64]{};
	bool l_Failed = false;
	const bool l_Got = api.ManualDispatch_GetAPICallResult(pipe, p_Call, l_Buffer, sizeof(l_Buffer), l_Expected, &l_Failed);
	(void)p_Callback;
	if (l_Expected == CALLBACK_LOBBY_ENTER)
	{
		if (!l_Got || l_Failed)
		{
			spdlog::warn("steam: joining the lobby failed");
			return;
		}
		SteamId l_Entered = 0;
		uint32_t l_Response = 0;
		std::memcpy(&l_Entered, l_Buffer, sizeof(l_Entered));
		std::memcpy(&l_Response, l_Buffer + 16, sizeof(l_Response)); // after the id, the permissions and the locked flag
		const char* l_Host = api.ISteamMatchmaking_GetLobbyData(api.matchmaking, l_Entered, "host");
		const uint64_t l_HostId = l_Host != nullptr ? std::strtoull(l_Host, nullptr, 10) : 0;
		spdlog::info("steam: lobby {} answered {} with host {}", l_Entered, l_Response, l_HostId);
		if (l_Entered != lobby)
			api.ISteamMatchmaking_LeaveLobby(api.matchmaking, l_Entered);
		if (l_HostId != 0)
			joinRequests.push_back(l_HostId);
	}
	else if (l_Expected == CALLBACK_LOBBY_CREATED)
	{
		lobbyCreating = false;
		if (!l_Got || l_Failed)
			return;
		const auto* l_Created = reinterpret_cast<const LobbyCreated*>(l_Buffer);
		if (l_Created->result != RESULT_OK)
			return;
		if (!lobbyWanted)
		{
			api.ISteamMatchmaking_LeaveLobby(api.matchmaking, l_Created->lobby); // stopped while it was being made
			return;
		}
		lobby = l_Created->lobby;
		publishLobby();
	}
}

void Steam::Impl::shutdown()
{
	if (!alive.exchange(false))
		return;
	pumpStop = true;
	if (pumpThread.joinable())
		pumpThread.join();
	g_Impl = nullptr;
	if (lobby != 0)
		api.ISteamMatchmaking_LeaveLobby(api.matchmaking, lobby);
	{
		const std::lock_guard l_Lock(big);
		for (auto& [l_Conn, l_Link] : links)
		{
			{
				const std::lock_guard l_LinkLock(l_Link->mutex);
				l_Link->closed = true;
			}
			l_Link->wake.notify_all();
			api.ISteamNetworkingSockets_CloseConnection(api.sockets, l_Conn, 0, nullptr, false);
		}
		links.clear();
		if (listener)
			api.ISteamNetworkingSockets_CloseListenSocket(api.sockets, listener->handle);
		for (const HListen l_Handle : retired)
			api.ISteamNetworkingSockets_CloseListenSocket(api.sockets, l_Handle);
		if (group != 0)
			api.ISteamNetworkingSockets_DestroyPollGroup(api.sockets, group);
	}
	api.Shutdown();
}

// ------------------------------------------------------------------------------------------------ Steam

Steam::Steam() = default;

Steam::~Steam()
{
	if (m_Impl)
		m_Impl->shutdown();
	if (m_Impl && m_Impl->api.library != nullptr)
		SDL_UnloadObject(m_Impl->api.library);
}

bool Steam::init()
{
	if (m_Available)
		return true;
	if (const char* l_Off = SDL_getenv("WB_STEAM"); l_Off != nullptr && std::string_view(l_Off) == "0")
	{
		m_InitNote = "disabled by WB_STEAM=0";
		return false;
	}
	auto l_Impl = std::make_shared<Impl>();
	std::string l_Problem = loadApi(l_Impl->api);
	Api& l_Api = l_Impl->api;
	if (l_Problem.empty())
	{
		// No app id is chosen here: Steam takes it from steam_appid.txt next to the program (or from Steam itself when it
		// starts the program), and without one it refuses, which just means no Steam options.
		char l_Error[1024]{};
		if (l_Api.InitFlat(l_Error) != 0)
			l_Problem = l_Error[0] != '\0' ? std::string(l_Error) : std::string("Steam is not running or has no app id for this program");
	}
	if (l_Problem.empty())
	{
		l_Api.ManualDispatch_Init();
		l_Impl->pipe = l_Api.GetHSteamPipe();
		l_Api.sockets = callAccessor(l_Api.library, "SteamAPI_SteamNetworkingSockets_SteamAPI_v");
		l_Api.utils = callAccessor(l_Api.library, "SteamAPI_SteamNetworkingUtils_SteamAPI_v");
		l_Api.matchmaking = callAccessor(l_Api.library, "SteamAPI_SteamMatchmaking_v");
		l_Api.user = callAccessor(l_Api.library, "SteamAPI_SteamUser_v");
		l_Api.friends = callAccessor(l_Api.library, "SteamAPI_SteamFriends_v");
		std::string l_Lacking;
		l_Lacking += l_Api.sockets == nullptr ? " SteamNetworkingSockets" : "";
		l_Lacking += l_Api.utils == nullptr ? " SteamNetworkingUtils" : "";
		l_Lacking += l_Api.matchmaking == nullptr ? " SteamMatchmaking" : "";
		l_Lacking += l_Api.user == nullptr ? " SteamUser" : "";
		l_Lacking += l_Api.friends == nullptr ? " SteamFriends" : "";
		if (!l_Lacking.empty())
		{
			l_Problem = "the Steam library does not have these interfaces:" + l_Lacking;
			l_Api.Shutdown();
		}
	}
	if (!l_Problem.empty())
	{
		if (l_Api.library != nullptr)
			SDL_UnloadObject(l_Api.library);
		l_Api.library = nullptr;
		m_InitNote = std::move(l_Problem);
		return false;
	}

	l_Impl->alive = true;
	g_Impl = l_Impl.get();
	l_Api.ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged(l_Api.utils, &statusTrampoline);
	l_Api.ISteamNetworkingUtils_InitRelayNetworkAccess(l_Api.utils); // so the first connection does not wait for it
	l_Impl->group = l_Api.ISteamNetworkingSockets_CreatePollGroup(l_Api.sockets);
	m_UserId = l_Api.ISteamUser_GetSteamID(l_Api.user);
	l_Impl->userIdText = std::to_string(m_UserId);
	if (const char* l_Name = l_Api.ISteamFriends_GetPersonaName(l_Api.friends))
		m_UserName = l_Name;
	m_Impl = std::move(l_Impl);
	m_Available = true;
	return true;
}

void Steam::pump()
{
	if (!m_Available)
		return;
	Impl& l_Impl = *m_Impl;
	l_Impl.api.ManualDispatch_RunFrame(l_Impl.pipe);
	CallbackMsg l_Message{};
	while (l_Impl.api.ManualDispatch_GetNextCallback(l_Impl.pipe, &l_Message))
	{
		if (l_Message.callback == CALLBACK_API_CALL_COMPLETED)
		{
			const auto* l_Done = reinterpret_cast<const ApiCallCompleted*>(l_Message.param);
			l_Impl.collect(l_Done->call, l_Done->callback);
		}
		else if (l_Message.callback == CALLBACK_LOBBY_JOIN && l_Message.size >= 8)
		{
			SteamId l_Lobby = 0;
			std::memcpy(&l_Lobby, l_Message.param, sizeof(l_Lobby));
			spdlog::info("steam: join request through lobby {}", l_Lobby);
			l_Impl.lobbyJoinRequested(l_Lobby);
		}
		if (l_Message.callback == CALLBACK_RICH_PRESENCE_JOIN && l_Message.size >= static_cast<int>(sizeof(JoinRequested)))
		{
			// The connect string is what joinable() published: "+connect_steam <host steam id>"
			auto* l_Join = reinterpret_cast<JoinRequested*>(l_Message.param);
			l_Join->connect[sizeof(l_Join->connect) - 1] = '\0';
			spdlog::info("steam: join request from {} with '{}'", l_Join->friendId, l_Join->connect);
			if (const uint64_t l_Host = parseConnectString(l_Join->connect); l_Host != 0)
				l_Impl.joinRequests.push_back(l_Host);
		}
		l_Impl.api.ManualDispatch_FreeLastCallback(l_Impl.pipe);
	}
}

// The session behind a lobby is hosted by the Steam user named in the lobby's data. That is only readable from inside the
// lobby, so it is joined for a moment (what Steam expects of a program answering Join Game) and left again.
void Steam::Impl::lobbyJoinRequested(const SteamId p_Lobby)
{
	const ApiCall l_Call = api.ISteamMatchmaking_JoinLobby(api.matchmaking, p_Lobby);
	if (l_Call == 0)
	{
		spdlog::warn("steam: could not join lobby {} to find its host", p_Lobby);
		return;
	}
	pending.push_back({ l_Call, CALLBACK_LOBBY_ENTER });
}

void Steam::setJoinable(const bool p_Joinable)
{
	if (!m_Available || m_Impl->joinable == p_Joinable)
		return;
	Impl& l_Impl = *m_Impl;
	l_Impl.joinable = p_Joinable;
	// Steam shows "Join Game" for friends, and lets this user invite them, while a connect string is published and a
	// friends-only lobby exists (it is what Steam hands to this program when someone joins that way)
	if (p_Joinable)
	{
		const bool l_Set = l_Impl.api.ISteamFriends_SetRichPresence(l_Impl.api.friends, "connect", (std::string(CONNECT_ARGUMENT) + " " + l_Impl.userIdText).c_str());
		const char* l_Read = l_Impl.api.ISteamFriends_GetFriendRichPresence(l_Impl.api.friends, m_UserId, "connect");
		spdlog::info("steam: friends can join now (rich presence connect: set {}, reads back '{}')", l_Set ? "ok" : "REFUSED", l_Read != nullptr ? l_Read : "");
		l_Impl.lobbyWanted = true;
		if (l_Impl.lobby == 0 && !l_Impl.lobbyCreating)
		{
			const ApiCall l_Call = l_Impl.api.ISteamMatchmaking_CreateLobby(l_Impl.api.matchmaking, LOBBY_TYPE_FRIENDS_ONLY, LOBBY_MAX_MEMBERS);
			if (l_Call != 0)
			{
				l_Impl.pending.push_back({ l_Call, CALLBACK_LOBBY_CREATED });
				l_Impl.lobbyCreating = true;
			}
		}
	}
	else
	{
		l_Impl.api.ISteamFriends_ClearRichPresence(l_Impl.api.friends);
		l_Impl.lobbyWanted = false;
		if (l_Impl.lobby != 0)
		{
			l_Impl.api.ISteamMatchmaking_LeaveLobby(l_Impl.api.matchmaking, l_Impl.lobby);
			l_Impl.lobby = 0;
		}
		spdlog::info("steam: friends can no longer join");
	}
}

uint64_t Steam::takeJoinRequest()
{
	if (!m_Available || m_Impl->joinRequests.empty())
		return 0;
	const uint64_t l_Host = m_Impl->joinRequests.back(); // the latest click wins
	m_Impl->joinRequests.clear();
	return l_Host;
}

uint64_t Steam::parseConnectString(const std::string_view p_Text)
{
	const size_t l_At = p_Text.find(CONNECT_ARGUMENT);
	if (l_At == std::string_view::npos)
		return 0;
	uint64_t l_Id = 0;
	size_t i = l_At + std::string_view(CONNECT_ARGUMENT).size();
	while (i < p_Text.size() && !(p_Text[i] >= '0' && p_Text[i] <= '9'))
		++i;
	for (; i < p_Text.size() && p_Text[i] >= '0' && p_Text[i] <= '9'; ++i)
		l_Id = l_Id * 10 + static_cast<uint64_t>(p_Text[i] - '0');
	return l_Id;
}

bool Steam::busy() const
{
	return m_Available && !m_Impl->pending.empty();
}

std::unique_ptr<net::Listener> Steam::listen()
{
	if (!m_Available)
		return nullptr;
	Impl& l_Impl = *m_Impl;
	auto l_State = std::make_shared<ListenState>();
	{
		const std::lock_guard l_Lock(l_Impl.big);
		l_State->handle = l_Impl.api.ISteamNetworkingSockets_CreateListenSocketP2P(l_Impl.api.sockets, VIRTUAL_PORT, 0, nullptr);
		if (l_State->handle == 0)
			return nullptr;
		l_Impl.listener = l_State;
	}
	l_Impl.startPump();
	return std::make_unique<SteamListener>(m_Impl, std::move(l_State));
}

net::Connector Steam::connector(const uint64_t p_Host)
{
	std::shared_ptr<Impl> l_Impl = m_Impl;
	return [l_Impl, p_Host](const std::atomic<bool>& p_Cancel, std::string& p_Error, const std::function<void(const std::string&)>& p_Trace) -> std::unique_ptr<net::Stream>
	{
		if (!l_Impl || !l_Impl->alive.load())
		{
			p_Error = tr("steam.unavailable");
			return nullptr;
		}
		p_Trace("Contacting Steam user " + std::to_string(p_Host) + " through the Steam relay network");
		l_Impl->startPump();
		auto l_Link = std::make_shared<Link>();
		l_Link->peer = "steam:" + std::to_string(p_Host);
		{
			const std::lock_guard l_Lock(l_Impl->big);
			NetIdentity l_Identity{};
			l_Impl->api.SteamNetworkingIdentity_SetSteamID64(&l_Identity, p_Host);
			l_Link->conn = l_Impl->api.ISteamNetworkingSockets_ConnectP2P(l_Impl->api.sockets, &l_Identity, VIRTUAL_PORT, 0, nullptr);
			if (l_Link->conn == 0)
			{
				p_Error = tr("steam.startFailed");
				return nullptr;
			}
			l_Impl->links.emplace(l_Link->conn, l_Link);
			l_Impl->api.ISteamNetworkingSockets_SetConnectionPollGroup(l_Impl->api.sockets, l_Link->conn, l_Impl->group);
		}

		const auto l_Deadline = std::chrono::steady_clock::now() + CONNECT_TIMEOUT;
		std::unique_lock l_Lock(l_Link->mutex);
		while (!l_Link->connected && !l_Link->closed && !p_Cancel.load() && std::chrono::steady_clock::now() < l_Deadline)
			l_Link->wake.wait_for(l_Lock, std::chrono::milliseconds(100));
		if (l_Link->connected)
		{
			l_Lock.unlock();
			p_Trace("Connected through Steam");
			return std::make_unique<SteamStream>(l_Impl, l_Link);
		}
		const bool l_Cancelled = p_Cancel.load();
		const std::string l_Reason = l_Link->reason;
		l_Lock.unlock();
		l_Impl->closeLink(l_Link);
		if (l_Cancelled)
			p_Error = tr("net.reason.cancelled");
		else if (!l_Reason.empty())
			p_Error = tr("steam.unreachable", { l_Reason });
		else
			p_Error = tr("steam.noAnswer", { std::to_string(std::chrono::duration_cast<std::chrono::seconds>(CONNECT_TIMEOUT).count()) });
		return nullptr;
	};
}

} // namespace wb::steam
