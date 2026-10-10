module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

module wb.net.socket;

namespace wb::net
{
namespace
{
#ifdef _WIN32
using Native = SOCKET;
constexpr Native NATIVE_INVALID = INVALID_SOCKET;

void ensureStarted()
{
	static const bool s_Started = []
	{
		WSADATA l_Data{};
		return WSAStartup(MAKEWORD(2, 2), &l_Data) == 0;
	}();
	(void)s_Started;
}

int lastError()
{
	return WSAGetLastError();
}

bool wouldBlock(const int p_Error)
{
	return p_Error == WSAEWOULDBLOCK || p_Error == WSAEINPROGRESS;
}

void closeNative(const Native p_Socket)
{
	closesocket(p_Socket);
}

bool setBlocking(const Native p_Socket, const bool p_Blocking)
{
	u_long l_NonBlocking = p_Blocking ? 0 : 1;
	return ioctlsocket(p_Socket, FIONBIO, &l_NonBlocking) == 0;
}

int pollOne(const Native p_Socket, const short p_Events, const int p_TimeoutMs)
{
	WSAPOLLFD l_Fd{ .fd = p_Socket, .events = p_Events, .revents = 0 };
	const int l_Result = WSAPoll(&l_Fd, 1, p_TimeoutMs);
	return l_Result > 0 ? static_cast<int>(l_Fd.revents) : l_Result;
}

std::string errorText(const int p_Error)
{
	const char* l_Meaning = nullptr;
	switch (p_Error)
	{
	case WSAETIMEDOUT: l_Meaning = "timed out"; break;
	case WSAECONNREFUSED: l_Meaning = "connection refused"; break;
	case WSAENETUNREACH: l_Meaning = "network unreachable"; break;
	case WSAEHOSTUNREACH: l_Meaning = "host unreachable"; break;
	case WSAEHOSTDOWN: l_Meaning = "host is down"; break;
	case WSAECONNRESET: l_Meaning = "connection reset"; break;
	case WSAECONNABORTED: l_Meaning = "connection aborted"; break;
	case WSAEACCES: l_Meaning = "access denied (blocked by a firewall or security software?)"; break;
	case WSAEADDRINUSE: l_Meaning = "port already in use"; break;
	case WSAEADDRNOTAVAIL: l_Meaning = "address not available"; break;
	case WSAEAFNOSUPPORT: l_Meaning = "address family not supported"; break;
	case WSANOTINITIALISED: l_Meaning = "networking is not available"; break;
	default: break;
	}
	return (l_Meaning != nullptr ? std::string(l_Meaning) : std::string("network error")) + " (code " + std::to_string(p_Error) + ")";
}

constexpr int SEND_FLAGS = 0;
#else
using Native = int;
constexpr Native NATIVE_INVALID = -1;

void ensureStarted() {}

int lastError()
{
	return errno;
}

bool wouldBlock(const int p_Error)
{
	return p_Error == EWOULDBLOCK || p_Error == EAGAIN || p_Error == EINPROGRESS;
}

void closeNative(const Native p_Socket)
{
	::close(p_Socket);
}

bool setBlocking(const Native p_Socket, const bool p_Blocking)
{
	const int l_Flags = fcntl(p_Socket, F_GETFL, 0);
	if (l_Flags < 0)
		return false;
	return fcntl(p_Socket, F_SETFL, p_Blocking ? (l_Flags & ~O_NONBLOCK) : (l_Flags | O_NONBLOCK)) == 0;
}

int pollOne(const Native p_Socket, const short p_Events, const int p_TimeoutMs)
{
	pollfd l_Fd{ .fd = p_Socket, .events = p_Events, .revents = 0 };
	const int l_Result = ::poll(&l_Fd, 1, p_TimeoutMs);
	return l_Result > 0 ? static_cast<int>(l_Fd.revents) : l_Result;
}

std::string errorText(const int p_Error)
{
	return std::string(std::strerror(p_Error)) + " (code " + std::to_string(p_Error) + ")";
}

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif
#endif

Native toNative(const uintptr_t p_Handle)
{
	return static_cast<Native>(p_Handle);
}

void tune(const Native p_Socket)
{
	const int l_On = 1;
	setsockopt(p_Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&l_On), sizeof(l_On));
}

bool parsePort(const std::string& p_Text, uint16_t& p_Port)
{
	if (p_Text.empty() || p_Text.size() > 5)
		return false;
	unsigned l_Value = 0;
	for (const char l_Char : p_Text)
	{
		if (l_Char < '0' || l_Char > '9')
			return false;
		l_Value = l_Value * 10 + static_cast<unsigned>(l_Char - '0');
	}
	if (l_Value == 0 || l_Value > 65535)
		return false;
	p_Port = static_cast<uint16_t>(l_Value);
	return true;
}

std::string addressText(const sockaddr* p_Address, const socklen_t p_Size)
{
	char l_Host[NI_MAXHOST]{};
	if (getnameinfo(p_Address, p_Size, l_Host, sizeof(l_Host), nullptr, 0, NI_NUMERICHOST) != 0)
		return "?";
	return p_Address->sa_family == AF_INET6 ? "[" + std::string(l_Host) + "]" : std::string(l_Host);
}

void trace(std::vector<std::string>* p_Trace, std::string p_Line)
{
	if (p_Trace != nullptr)
		p_Trace->push_back(std::move(p_Line));
}

// The error code that makes a failed connect worth explaining to someone setting up a port forward
bool isTimeout(const int p_Error)
{
#ifdef _WIN32
	return p_Error == WSAETIMEDOUT;
#else
	return p_Error == ETIMEDOUT;
#endif
}

bool isRefused(const int p_Error)
{
#ifdef _WIN32
	return p_Error == WSAECONNREFUSED;
#else
	return p_Error == ECONNREFUSED;
#endif
}
} // namespace

Socket::~Socket()
{
	close();
}

Socket& Socket::operator=(Socket&& p_Other) noexcept
{
	if (this != &p_Other)
	{
		close();
		m_Handle = std::exchange(p_Other.m_Handle, INVALID);
	}
	return *this;
}

void Socket::close()
{
	if (m_Handle != INVALID)
	{
		closeNative(toNative(m_Handle));
		m_Handle = INVALID;
	}
}

void Socket::shutdown()
{
	if (m_Handle == INVALID)
		return;
#ifdef _WIN32
	::shutdown(toNative(m_Handle), SD_BOTH);
#else
	::shutdown(toNative(m_Handle), SHUT_RDWR);
#endif
}

Socket Socket::listenTcp(const uint16_t p_Port, std::string& p_Error, std::vector<std::string>* p_Trace)
{
	ensureStarted();
	// Dual stack first so both IPv4 and IPv6 peers can join; plain IPv4 where IPv6 is unavailable
	for (const int l_Family : { AF_INET6, AF_INET })
	{
		const Native l_Socket = ::socket(l_Family, SOCK_STREAM, IPPROTO_TCP);
		if (l_Socket == NATIVE_INVALID)
		{
			p_Error = errorText(lastError());
			trace(p_Trace, std::string("Cannot create an ") + (l_Family == AF_INET6 ? "IPv6" : "IPv4") + " socket: " + p_Error);
			continue;
		}
#ifndef _WIN32
		const int l_Reuse = 1;
		setsockopt(l_Socket, SOL_SOCKET, SO_REUSEADDR, &l_Reuse, sizeof(l_Reuse));
#endif
		bool l_Bound = false;
		if (l_Family == AF_INET6)
		{
			const int l_V6Only = 0;
			setsockopt(l_Socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&l_V6Only), sizeof(l_V6Only));
			sockaddr_in6 l_Address{};
			l_Address.sin6_family = AF_INET6;
			l_Address.sin6_addr = in6addr_any;
			l_Address.sin6_port = htons(p_Port);
			l_Bound = ::bind(l_Socket, reinterpret_cast<const sockaddr*>(&l_Address), sizeof(l_Address)) == 0;
		}
		else
		{
			sockaddr_in l_Address{};
			l_Address.sin_family = AF_INET;
			l_Address.sin_addr.s_addr = htonl(INADDR_ANY);
			l_Address.sin_port = htons(p_Port);
			l_Bound = ::bind(l_Socket, reinterpret_cast<const sockaddr*>(&l_Address), sizeof(l_Address)) == 0;
		}
		if (!l_Bound || ::listen(l_Socket, 16) != 0)
		{
			p_Error = errorText(lastError());
			trace(p_Trace, std::string("Cannot listen on ") + (l_Family == AF_INET6 ? "IPv6" : "IPv4") + " port " + std::to_string(p_Port) + ": " + p_Error);
			closeNative(l_Socket);
			if (l_Bound)
				return {}; // the port is the problem, not the address family
			continue;
		}
		trace(p_Trace, std::string("Listening on every ") + (l_Family == AF_INET6 ? "IPv6 and IPv4" : "IPv4") + " address");
		return Socket(static_cast<uintptr_t>(l_Socket));
	}
	return {};
}

Socket Socket::connectTcp(const std::string& p_Host, const uint16_t p_Port, const int p_TimeoutMs, const std::atomic<bool>& p_Cancel, std::string& p_Error, const std::function<void(const std::string&)>& p_Trace)
{
	ensureStarted();
	const auto say = [&](const std::string& p_Line)
	{
		if (p_Trace)
			p_Trace(p_Line);
	};
	say("Looking up \"" + p_Host + "\"");
	addrinfo l_Hints{};
	l_Hints.ai_family = AF_UNSPEC;
	l_Hints.ai_socktype = SOCK_STREAM;
	l_Hints.ai_protocol = IPPROTO_TCP;
	addrinfo* l_List = nullptr;
	const int l_Lookup = getaddrinfo(p_Host.c_str(), std::to_string(p_Port).c_str(), &l_Hints, &l_List);
	if (l_Lookup != 0 || l_List == nullptr)
	{
#ifdef _WIN32
		const std::string l_Why = std::to_string(l_Lookup);
#else
		const std::string l_Why = std::string(gai_strerror(l_Lookup)) + ", " + std::to_string(l_Lookup);
#endif
		p_Error = "Could not find \"" + p_Host + "\": the name does not resolve. Check the spelling, or use the IP address instead.";
		say("Lookup failed (" + l_Why + ")");
		return {};
	}
	{
		std::string l_Found = "Resolved to";
		for (const addrinfo* l_Entry = l_List; l_Entry != nullptr; l_Entry = l_Entry->ai_next)
			l_Found += " " + addressText(l_Entry->ai_addr, static_cast<socklen_t>(l_Entry->ai_addrlen));
		say(l_Found);
	}

	Socket l_Result;
	size_t l_Tried = 0;
	size_t l_Timeouts = 0;
	size_t l_Refused = 0;
	std::string l_LastReason;
	for (const addrinfo* l_Entry = l_List; l_Entry != nullptr && !l_Result.valid() && !p_Cancel.load(); l_Entry = l_Entry->ai_next)
	{
		// Each address gets its own time: one that silently drops packets must not use up the others' chance
		const std::string l_Target = addressText(l_Entry->ai_addr, static_cast<socklen_t>(l_Entry->ai_addrlen)) + ":" + std::to_string(p_Port);
		const auto l_Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_TimeoutMs);
		++l_Tried;
		say("Trying " + l_Target + " (waiting up to " + std::to_string(p_TimeoutMs / 1000) + " s for an answer)");
		const Native l_Socket = ::socket(l_Entry->ai_family, l_Entry->ai_socktype, l_Entry->ai_protocol);
		if (l_Socket == NATIVE_INVALID)
		{
			l_LastReason = errorText(lastError());
			say("Cannot use " + l_Target + ": " + l_LastReason);
			continue;
		}
		setBlocking(l_Socket, false);
		bool l_Connected = ::connect(l_Socket, l_Entry->ai_addr, static_cast<int>(l_Entry->ai_addrlen)) == 0;
		int l_Failure = 0;
		bool l_TimedOut = false;
		if (!l_Connected)
		{
			l_Failure = lastError();
			if (wouldBlock(l_Failure))
			{
				l_Failure = 0;
				l_TimedOut = true;
				while (!p_Cancel.load() && std::chrono::steady_clock::now() < l_Deadline)
				{
					const int l_Events = pollOne(l_Socket, POLLOUT, 100);
					if (l_Events > 0)
					{
						int l_Status = 0;
						socklen_t l_Size = sizeof(l_Status);
						getsockopt(l_Socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&l_Status), &l_Size);
						l_Connected = l_Status == 0;
						l_Failure = l_Status;
						l_TimedOut = false;
						break;
					}
					if (l_Events < 0)
					{
						l_Failure = lastError();
						l_TimedOut = false;
						break;
					}
				}
			}
		}
		if (!l_Connected)
		{
			closeNative(l_Socket);
			if (p_Cancel.load())
				break;
			if (l_TimedOut || isTimeout(l_Failure))
			{
				++l_Timeouts;
				l_LastReason = "no answer after " + std::to_string(p_TimeoutMs / 1000) + " s";
			}
			else
			{
				if (isRefused(l_Failure))
					++l_Refused;
				l_LastReason = errorText(l_Failure);
			}
			say("Connecting to " + l_Target + " failed: " + l_LastReason);
			continue;
		}
		setBlocking(l_Socket, true);
		tune(l_Socket);
		say("Connected to " + l_Target);
		l_Result = Socket(static_cast<uintptr_t>(l_Socket));
	}
	freeaddrinfo(l_List);
	if (l_Result.valid())
		return l_Result;
	if (p_Cancel.load())
	{
		p_Error = "Cancelled";
	}
	else if (l_Refused == l_Tried)
	{
		p_Error = "\"" + p_Host + "\" refused the connection on port " + std::to_string(p_Port) + ". The machine is reachable but nothing is listening there: "
		          "the host is not hosting yet, the port number is wrong, or a firewall is rejecting it.";
	}
	else if (l_Timeouts == l_Tried)
	{
		p_Error = "No answer from \"" + p_Host + "\" on port " + std::to_string(p_Port) + " after " + std::to_string(p_TimeoutMs / 1000) + " s. "
		          "Packets are being dropped on the way: the IP address is wrong (a private 192.168.x.x / 10.x.x.x address does not work over the internet), "
		          "the router is not forwarding this port to the host, or the host's firewall blocks inkwell.";
	}
	else
	{
		p_Error = "Could not connect to \"" + p_Host + "\" on port " + std::to_string(p_Port) + ": " + l_LastReason;
	}
	return l_Result;
}

Socket Socket::accept(const int p_TimeoutMs)
{
	if (m_Handle == INVALID || pollOne(toNative(m_Handle), POLLIN, p_TimeoutMs) <= 0)
		return {};
	const Native l_Peer = ::accept(toNative(m_Handle), nullptr, nullptr);
	if (l_Peer == NATIVE_INVALID)
		return {};
	setBlocking(l_Peer, true);
	tune(l_Peer);
	return Socket(static_cast<uintptr_t>(l_Peer));
}

bool Socket::sendAll(std::span<const uint8_t> p_Data)
{
	if (m_Handle == INVALID)
		return false;
	while (!p_Data.empty())
	{
		const int l_Chunk = static_cast<int>(std::min<size_t>(p_Data.size(), 1u << 20));
		const auto l_Sent = ::send(toNative(m_Handle), reinterpret_cast<const char*>(p_Data.data()), l_Chunk, SEND_FLAGS);
		if (l_Sent <= 0)
		{
#ifndef _WIN32
			if (l_Sent < 0 && errno == EINTR)
				continue;
#endif
			return false;
		}
		p_Data = p_Data.subspan(static_cast<size_t>(l_Sent));
	}
	return true;
}

bool Socket::recvAll(std::span<uint8_t> p_Data)
{
	if (m_Handle == INVALID)
		return false;
	while (!p_Data.empty())
	{
		const int l_Chunk = static_cast<int>(std::min<size_t>(p_Data.size(), 1u << 20));
		const auto l_Got = ::recv(toNative(m_Handle), reinterpret_cast<char*>(p_Data.data()), l_Chunk, 0);
		if (l_Got <= 0)
		{
#ifndef _WIN32
			if (l_Got < 0 && errno == EINTR)
				continue;
#endif
			return false;
		}
		p_Data = p_Data.subspan(static_cast<size_t>(l_Got));
	}
	return true;
}

uint16_t Socket::localPort() const
{
	if (m_Handle == INVALID)
		return 0;
	sockaddr_storage l_Address{};
	socklen_t l_Size = sizeof(l_Address);
	if (getsockname(toNative(m_Handle), reinterpret_cast<sockaddr*>(&l_Address), &l_Size) != 0)
		return 0;
	if (l_Address.ss_family == AF_INET6)
		return ntohs(reinterpret_cast<const sockaddr_in6*>(&l_Address)->sin6_port);
	return ntohs(reinterpret_cast<const sockaddr_in*>(&l_Address)->sin_port);
}

std::string Socket::peerAddress() const
{
	if (m_Handle == INVALID)
		return {};
	sockaddr_storage l_Address{};
	socklen_t l_Size = sizeof(l_Address);
	if (getpeername(toNative(m_Handle), reinterpret_cast<sockaddr*>(&l_Address), &l_Size) != 0)
		return {};
	char l_Host[NI_MAXHOST]{};
	if (getnameinfo(reinterpret_cast<const sockaddr*>(&l_Address), l_Size, l_Host, sizeof(l_Host), nullptr, 0, NI_NUMERICHOST) != 0)
		return {};
	return l_Host;
}

bool parseEndpoint(const std::string& p_Text, const uint16_t p_DefaultPort, std::string& p_Host, uint16_t& p_Port)
{
	const size_t l_Begin = p_Text.find_first_not_of(" \t");
	if (l_Begin == std::string::npos)
		return false;
	const std::string l_Text = p_Text.substr(l_Begin, p_Text.find_last_not_of(" \t") - l_Begin + 1);

	std::string l_Host;
	std::string l_PortText;
	if (l_Text.front() == '[')
	{
		const size_t l_Close = l_Text.find(']');
		if (l_Close == std::string::npos)
			return false;
		l_Host = l_Text.substr(1, l_Close - 1);
		if (l_Close + 1 < l_Text.size())
		{
			if (l_Text[l_Close + 1] != ':')
				return false;
			l_PortText = l_Text.substr(l_Close + 2);
		}
	}
	else if (std::ranges::count(l_Text, ':') == 1)
	{
		const size_t l_Colon = l_Text.find(':');
		l_Host = l_Text.substr(0, l_Colon);
		l_PortText = l_Text.substr(l_Colon + 1);
	}
	else
	{
		l_Host = l_Text; // a name, an IPv4 address or a bare IPv6 address
	}
	if (l_Host.empty())
		return false;
	uint16_t l_Port = p_DefaultPort;
	if (!l_PortText.empty() && !parsePort(l_PortText, l_Port))
		return false;
	p_Host = std::move(l_Host);
	p_Port = l_Port;
	return true;
}
} // namespace wb::net
