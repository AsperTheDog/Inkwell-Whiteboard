// Optional Steam integration: P2P connections through Steam's relay network (ISteamNetworkingSockets) and lobbies to find
// other people's sessions. Nothing here is required: the Steam client library is loaded at run time, and when it is
// missing, Steam is not running, or the program was not started with an app id, available() stays false and the rest of
// the application never mentions Steam.
//
// Only the few functions of the flat C API that are needed are declared (steam.cpp); no Steamworks SDK header or library is
// needed to build. Everything except the P2P transport is for the main thread; the transport (the Listener and Stream
// handed out) is used from the session's own threads.
module;
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module wb.steam;

import wb.net.stream;

export namespace wb::steam
{
// A session somebody advertises in a Steam lobby
struct Lobby
{
	uint64_t id = 0;   // the lobby
	uint64_t host = 0; // SteamID of the person hosting; connect to this
	std::string name;
	uint32_t users = 0;
	bool password = false;
};

class Steam
{
public:
	Steam();
	~Steam();
	Steam(const Steam&) = delete;
	Steam& operator=(const Steam&) = delete;

	// Tries to load the Steam library and reach the running client. Quiet on failure: returns false and nothing else
	// happens. WB_STEAM=0 skips it. The app id comes from steam_appid.txt next to the program, never from here.
	bool init();
	[[nodiscard]] bool available() const { return m_Available; }
	// The signed in person: their Steam name and 64 bit id
	[[nodiscard]] const std::string& userName() const { return m_UserName; }
	[[nodiscard]] uint64_t userId() const { return m_UserId; }
	// A line saying why init() did not work (for the log)
	[[nodiscard]] const std::string& initNote() const { return m_InitNote; }

	// Main thread, every loop iteration: collects the answers to lobby requests
	void pump();
	// A lobby request is on its way: the loop should come back soon
	[[nodiscard]] bool busy() const;

	// ---- the P2P transport
	// Accepts people connecting to this user through Steam (until the listener is closed or destroyed). One at a time.
	[[nodiscard]] std::unique_ptr<net::Listener> listen();
	// Connects to the Steam user p_Host, who is hosting
	[[nodiscard]] net::Connector connector(uint64_t p_Host);

	// ---- lobbies: finding sessions
	// Lists the session lobbies that are public (compatible version only), when the answer arrives see lobbies()
	void refreshLobbies();
	[[nodiscard]] bool searching() const;
	[[nodiscard]] const std::vector<Lobby>& lobbies() const;
	// Bumps whenever the list or the searching state changes
	[[nodiscard]] uint64_t lobbiesRevision() const;

	// Lists this user's session so others find it. Replaces a previous listing.
	void advertise(const std::string& p_Title, bool p_Password, uint32_t p_Users);
	void setAdvertUsers(uint32_t p_Users);
	void stopAdvert();
	[[nodiscard]] bool advertising() const;

	struct Impl; // internal

private:
	std::shared_ptr<Impl> m_Impl;
	bool m_Available = false;
	std::string m_UserName;
	std::string m_InitNote;
	uint64_t m_UserId = 0;
};
} // namespace wb::steam
