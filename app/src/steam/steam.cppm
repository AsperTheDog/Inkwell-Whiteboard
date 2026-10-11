// Optional Steam integration: P2P connections through Steam's relay network (ISteamNetworkingSockets), and Steam's own Join Game
// and invite buttons for friends. Nothing here is required: the Steam client library is loaded at run time, and when it is
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
#include <string_view>
#include <vector>

export module wb.steam;

import wb.net.stream;

export namespace wb::steam
{
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

	// Main thread, every loop iteration: collects Steam's answers and join requests
	void pump();
	// A request to Steam is on its way: the loop should come back soon
	[[nodiscard]] bool busy() const;

	// ---- joining from Steam itself (friends list "Join Game", invites)
	// While a session is hosted through Steam its host is joinable: friends get Join Game and the invite button works (a friends-only
	// lobby and a rich presence string; nothing is listed publicly)
	void setJoinable(bool p_Joinable);
	// The host somebody asked to join (a click on Join Game or an accepted invite while running), 0 if none
	[[nodiscard]] uint64_t takeJoinRequest();
	// The host in a "+connect_steam <id>" string (what Steam also puts on the command line when it starts the program), 0 if none
	[[nodiscard]] static uint64_t parseConnectString(std::string_view p_Text);

	// ---- the P2P transport
	// Accepts people connecting to this user through Steam (until the listener is closed or destroyed). One at a time.
	[[nodiscard]] std::unique_ptr<net::Listener> listen();
	// Connects to the Steam user p_Host, who is hosting
	[[nodiscard]] net::Connector connector(uint64_t p_Host);

	struct Impl; // internal

private:
	std::shared_ptr<Impl> m_Impl;
	bool m_Available = false;
	std::string m_UserName;
	std::string m_InitNote;
	uint64_t m_UserId = 0;
};
} // namespace wb::steam
