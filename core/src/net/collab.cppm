// A collaboration session: one instance hosts a board on a TCP port, others join with address + password.
//
// The host is the authority. Clients apply their own edits at once and send them to the host, which applies them (after
// checking them) and relays them to the others in the order it received them. Everything here runs on the main thread
// except the socket threads, which only exchange frames with the main thread through a locked queue; the Document is
// never touched off the main thread.
module;
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
#include <vector>

export module wb.net.collab;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.io.blob;
import wb.net.connection;
import wb.net.ops;
import wb.net.protocol;
import wb.net.replicator;
import wb.net.socket;

export namespace wb::net
{
enum class Role : uint8_t
{
	None,
	Host,
	Client,
};

enum class LinkState : uint8_t
{
	Offline,
	Connecting, // client: reaching the host
	Joining,    // client: logging in and receiving the board
	Online,
};

// What the session needs from the application. Everything but `wake` is called on the main thread (from tick/host/join/leave).
struct CollabHooks
{
	// The board as a .wbrd file
	std::function<std::vector<uint8_t>()> makeSnapshot;
	// Replaces the board (and clears the undo history) with a received .wbrd file. False when it cannot be read.
	std::function<bool(std::span<const uint8_t>)> loadSnapshot;
	// A remote edit changed (p_Removed false) or removed objects: the undo history must forget what would undo them
	std::function<void(std::span<const ObjectId>, bool p_Removed)> remoteChange;
	// Something to tell the user: a person joined or left, the link dropped. p_Error marks failures.
	std::function<void(const std::string&, bool p_Error)> message;
	// Optional: every line added to the diagnostics log (connection attempts, handshake steps, why a link closed)
	std::function<void(const std::string&)> log;
	// A file will never arrive (its sender left, or its objects were deleted): nothing in the undo history may bring back
	// an object that uses it
	std::function<void(AssetId)> assetLost;
	// Optional. Makes a small RGBA thumbnail (a still image, the first frame of a GIF or video) of an asset file, so the
	// people receiving it see something while it streams. Called on a worker thread of the session; nothing when the
	// file cannot be shown.
	std::function<std::optional<AssetPreview>(const std::shared_ptr<const ByteSource>&)> makePreview;
	// Any thread: the session has something to process, wake the main loop
	std::function<void()> wake;
};

class CollabSession
{
public:
	CollabSession(Document& p_Document, CollabHooks p_Hooks);
	~CollabSession();
	CollabSession(const CollabSession&) = delete;
	CollabSession& operator=(const CollabSession&) = delete;

	// Starts listening. False (with p_Error) when the port cannot be used.
	bool host(uint16_t p_Port, std::string p_Password, std::string p_Name, std::string& p_Error);
	// Connects in the background; progress and failure are reported through state() and the message hook
	void join(std::string p_Host, uint16_t p_Port, std::string p_Password, std::string p_Name);
	void leave();

	// Where big files that arrive are kept while they are received and played (they vanish when the program ends)
	void setCacheDirectory(std::filesystem::path p_Directory) { m_CacheDir = std::move(p_Directory); }

	// Call every loop iteration on the main thread: sends local edits, applies what arrived
	void tick();

	[[nodiscard]] Role role() const { return m_Role; }
	[[nodiscard]] LinkState state() const { return m_State; }
	[[nodiscard]] bool active() const { return m_Role != Role::None; }
	// Hosting, or a joined client: edits are being shared
	[[nodiscard]] bool online() const { return m_State == LinkState::Online; }
	[[nodiscard]] uint16_t localSlot() const { return m_LocalSlot; }
	[[nodiscard]] uint16_t listenPort() const { return m_ListenPort; }
	[[nodiscard]] const std::vector<UserInfo>& users() const { return m_Users; }
	[[nodiscard]] const UserInfo* user(uint16_t p_Slot) const;
	// The most recent connection diagnostics, oldest first. Kept after the session ends so a failed join can be explained.
	[[nodiscard]] const std::deque<std::string>& diagnostics() const { return m_Log; }
	// Bumps whenever the user table changes
	[[nodiscard]] uint64_t usersRevision() const { return m_UsersRevision; }

	// Pictures and videos being received. They arrive after the board, so objects show a placeholder until theirs lands.
	struct AssetProgress
	{
		uint32_t files = 0;     // still to arrive
		uint64_t received = 0;  // bytes of those that have started
		uint64_t expected = 0;  // size of those that have started
	};
	[[nodiscard]] AssetProgress assetProgress() const;
	// A file nobody is sending (its object was deleted mid-transfer and then brought back by someone else's undo). The
	// pictures wait as placeholders; whoever has the file gets the chance to send it again.
	[[nodiscard]] bool assetStalled(AssetId p_Id) const { return m_Stalled.contains(p_Id); }      // we lack it
	[[nodiscard]] bool assetReuploadable(AssetId p_Id) const { return m_Reuploadable.contains(p_Id); } // we have it, the others cannot get it
	[[nodiscard]] bool hasAssetNotices() const { return !m_Stalled.empty() || !m_Reuploadable.empty(); }
	// Offers the file to the host again so everyone can have it
	void reupload(AssetId p_Id);
	// Who made the file (empty when they are gone)
	[[nodiscard]] std::string assetOwnerName(AssetId p_Id) const;
	// How much of one file has arrived, 0..1; negative when that file is not being received
	[[nodiscard]] double assetFraction(AssetId p_Id) const;
	// Bumps whenever a picture or video finishes arriving (the board has to be redrawn)
	[[nodiscard]] uint64_t assetsRevision() const { return m_AssetsRevision; }

	// ---- pointers of the other people
	// Where this user's pointer is, in board coordinates (visible false: it is not over the board). Sent at most about 30
	// times a second, always the latest.
	void setLocalCursor(DVec2 p_World, bool p_Visible);
	// A local update is waiting for its turn to be sent: call tick() again soon
	[[nodiscard]] bool cursorPending() const { return m_CursorDirty && m_State == LinkState::Online; }
	struct CursorView
	{
		uint16_t slot = 0;
		DVec2 position{ 0.0 };
		bool visible = false;
		double ageSeconds = 0.0; // since the last update
	};
	[[nodiscard]] std::vector<CursorView> cursors() const;
	// Bumps whenever a pointer position arrives
	[[nodiscard]] uint64_t cursorsRevision() const { return m_CursorsRevision; }

	// ---- lines being drawn
	// The line this user is drawing right now (pen, highlighter, shape), so the others watch it grow. `stable` points never
	// change again; `tail` is the provisional end, replaced on every update. Call it every frame with the line, or nullptr
	// when none is being drawn; it is sent at most about 30 times a second, and only what is new.
	struct LocalStroke
	{
		DVec2 origin{ 0.0 };
		Color color{};
		std::span<const StrokePoint> stable;
		std::span<const StrokePoint> tail;
	};
	void setLocalStroke(const LocalStroke* p_Stroke);
	// An update is waiting for its turn to be sent: call tick() again soon
	[[nodiscard]] bool strokePending() const { return m_StrokeDirty && m_State == LinkState::Online; }
	struct StrokeView
	{
		uint16_t slot = 0;
		DVec2 origin{ 0.0 };
		Color color{};
		std::span<const StrokePoint> points; // valid until the next tick()
	};
	// What the others are drawing right now
	[[nodiscard]] std::vector<StrokeView> liveStrokes() const;
	// Bumps whenever somebody's line changes or ends
	[[nodiscard]] uint64_t strokesRevision() const { return m_StrokesRevision; }

	// ---- video playback: play/pause and the playhead are shared, loop and sound are each person's own
	// This user pressed play/pause or moved the playhead of video p_Id
	void sendVideoState(ObjectId p_Id, bool p_Playing, double p_Position);
	struct VideoEvent
	{
		ObjectId id = INVALID_OBJECT_ID;
		bool playing = false;
		double position = 0.0;
	};
	// What other people did to videos since the last call (including the state of the playing ones when joining)
	[[nodiscard]] std::vector<VideoEvent> takeVideoEvents();

private:
	struct Event
	{
		enum class Kind : uint8_t
		{
			Accepted,      // host: a socket connected
			Connected,     // client: the connection to the host is up
			ConnectFailed, // client
			FrameIn,
			Closed,
			Progress, // client: a step of the connection attempt (text)
			BulkSent, // a connection wrote a file piece: room for the next one
			Preview,  // the worker finished a thumbnail (frame.payload: AssetPreviewMessage)
		};
		Kind kind = Kind::FrameIn;
		uint32_t conn = 0;
		Socket socket;
		Frame frame;
		std::string text;
	};

	enum class PeerState : uint8_t
	{
		Handshake, // host: Challenge sent, waiting for Hello. client: waiting for Challenge / Welcome / board.
		Joined,
	};
	struct Peer
	{
		uint32_t id = 0;
		std::unique_ptr<Connection> connection;
		PeerState state = PeerState::Handshake;
		std::chrono::steady_clock::time_point since;
		std::array<uint8_t, 16> nonce{};
		std::string address;
		UserInfo info; // host side: who this peer is once joined
		bool challenged = false; // client side: the Hello has been sent
		// client side: the board arriving in chunks
		std::vector<uint8_t> snapshot;
		bool welcomed = false;
		// Files this peer asked for, to be streamed in order (the front one is the one in progress)
		struct Upload
		{
			AssetId id = INVALID_ASSET_ID;
			uint32_t ticket = 0; // the asker's: echoed in every piece
			uint64_t offset = 0;
			bool previewDone = false; // the thumbnail went out (or there is none): the file may follow
		};
		std::deque<Upload> uploads;
		std::chrono::steady_clock::time_point lastCursor; // host side: flood control
		std::chrono::steady_clock::time_point strokeWindow; // host side: line updates counted since then
		uint32_t strokeCount = 0;
		std::chrono::steady_clock::time_point videoWindow; // host side: video updates counted since then
		uint32_t videoCount = 0;
	};
	struct RemoteStroke
	{
		uint32_t stroke = 0;
		DVec2 origin{ 0.0 };
		Color color{};
		std::vector<StrokePoint> points;
		std::chrono::steady_clock::time_point updated;
	};
	struct RemoteCursor
	{
		DVec2 position{ 0.0 };
		bool visible = false;
		std::chrono::steady_clock::time_point updated;
	};
	// A file we asked a peer for. Nothing is allocated until its pieces arrive.
	struct Download
	{
		uint32_t from = 0; // peer id
		uint32_t ticket = 0;
		uint64_t total = 0; // 0 until the first piece
		uint32_t width = 0;
		uint32_t height = 0;
		std::string name;
		uint64_t received = 0;
		std::vector<uint8_t> bytes;        // small files wait here until they are whole
		std::shared_ptr<FileSource> file;  // big ones are written to a temporary file as they arrive (and can be played meanwhile)
	};
	struct Failures
	{
		uint32_t count = 0;
		std::chrono::steady_clock::time_point last;
	};

	void post(Event p_Event);
	void queueLocal(Op p_Op);
	void flushLocal();
	void sendOps(std::span<const Op> p_Ops, uint32_t p_Except);
	void setIdle(bool p_Idle);

	void handleEvent(Event& p_Event);
	void hostAccept(Event& p_Event);
	void hostFrame(Peer& p_Peer, Frame& p_Frame);
	void hostHello(Peer& p_Peer, const Frame& p_Frame);
	void hostOps(Peer& p_Peer, const Frame& p_Frame);
	void clientFrame(Peer& p_Peer, Frame& p_Frame);
	void clientSnapshot(Peer& p_Peer, const Frame& p_Frame);
	void applyRemoteOps(std::span<const Op> p_Ops, uint32_t p_From);
	std::unique_ptr<Connection> makeConnection(Socket p_Socket, uint32_t p_Id);

	// Pictures and videos: asking for the ones objects use but we lack, serving requests, receiving pieces
	void wantAssets(Peer& p_Source, std::span<const ObjectId> p_Objects);
	void wantAllMissingAssets(Peer& p_Source);
	void assetRequested(Peer& p_Peer, const Frame& p_Frame);
	void assetReceived(Peer& p_Peer, const Frame& p_Frame);
	void serveAssets(Peer& p_Peer);
	void queueUpload(Peer& p_Peer, AssetId p_Id, uint32_t p_Ticket);
	void resetAssetTransfers();
	void clearDownloads();
	[[nodiscard]] const ImageAsset* completeAsset(AssetId p_Id) const;
	// Forgets a transfer (and whoever waited for it)
	void dropDownload(AssetId p_Id, bool p_Lost = false);
	// Host: nobody can supply this file now. Everyone is told; the owner may offer it again.
	void stallAsset(AssetId p_Id);
	void assetStalledReceived(Peer& p_Peer, const Frame& p_Frame);
	void assetOffered(Peer& p_Peer, const Frame& p_Frame);
	// Objects brought back or changed here (an undo) whose file is missing: ask for it or note that it is stalled
	void resolveLocalMedia();
	[[nodiscard]] Peer* peerOfSlot(uint16_t p_Slot);
	// Stops the transfers nothing on the board needs any more (their objects were deleted)
	void pruneDownloads();
	// The sender is gone before a file finished: the transfers end and the objects that wait for them are removed
	void abandonDownloadsFrom(uint32_t p_PeerId, const std::string& p_Who);
	void cursorReceived(Peer& p_Peer, const Frame& p_Frame);
	void strokeReceived(Peer& p_Peer, const Frame& p_Frame);
	void applyStroke(const StrokePreviewMessage& p_Message);
	void sendStroke(bool p_End);
	void forgetStrokes(uint16_t p_Slot);
	void videoStateReceived(Peer& p_Peer, const Frame& p_Frame);
	void sendVideoStates(Peer& p_Peer);
	void previewReceived(Peer& p_Peer, const Frame& p_Frame);
	void sendPreview(Peer& p_Peer, AssetId p_Id);
	void previewFinished(const Frame& p_Frame);
	void startPreviewJob(AssetId p_Id);
	void previewWorker();
	void sendCursor();

	void dropPeer(uint32_t p_Id, const std::string& p_Reason);
	void reject(Peer& p_Peer, const std::string& p_Reason);
	void failClient(const std::string& p_Reason);
	void stop(const std::string& p_Goodbye);
	void rebuildUsers();
	uint16_t pickSlot();
	uint32_t pickColor() const;
	void tell(const std::string& p_Text, bool p_Error = false);
	void note(const std::string& p_Line);
	void broadcastOrder();

	Document& m_Document;
	CollabHooks m_Hooks;
	Replicator m_Replicator;
	std::optional<Replicator::Pause> m_Idle; // while offline nothing is turned into ops

	Role m_Role = Role::None;
	LinkState m_State = LinkState::Offline;
	std::string m_Password;
	std::string m_Name;
	uint16_t m_LocalSlot = 0;
	uint16_t m_ListenPort = 0;
	uint16_t m_NextSlot = 1;
	std::vector<UserInfo> m_Users;
	uint64_t m_UsersRevision = 0;

	std::unordered_map<uint32_t, Peer> m_Peers;
	std::vector<std::unique_ptr<Connection>> m_Dying; // told to close once their last frame is out
	uint32_t m_NextPeerId = 1;
	std::unordered_map<std::string, Failures> m_Failures;
	bool m_OrderDirty = false;
	std::chrono::steady_clock::time_point m_LastOrder;

	// Edits made here, waiting to be sent (in order, with repeated drags of one object merged)
	std::vector<Op> m_Pending;
	std::unordered_map<ObjectId, size_t> m_PendingMove;

	std::deque<std::string> m_Log;

	// Thumbnails being made (worker thread) and the ones that could not be
	std::mutex m_PreviewMutex;
	std::condition_variable m_PreviewWake;
	std::deque<std::pair<AssetId, std::shared_ptr<const ByteSource>>> m_PreviewJobs;
	std::unordered_set<AssetId> m_PreviewActive; // main thread: asked for, not finished
	std::unordered_set<AssetId> m_PreviewFailed;
	std::thread m_PreviewThread;
	bool m_PreviewStop = false;

	std::unordered_map<AssetId, Download> m_Downloads;
	uint64_t m_DownloadBytes = 0; // of the small ones, which are held in memory
	std::filesystem::path m_CacheDir;
	// host: peers that asked for a file the host does not have yet (it is on its way from another peer)
	struct Waiter
	{
		uint32_t peer = 0;
		uint32_t ticket = 0;
	};
	std::unordered_map<AssetId, std::vector<Waiter>> m_AssetWaiters;
	uint32_t m_NextTicket = 0;
	uint64_t m_AssetsRevision = 0;
	uint64_t m_PrunedRevision = 0; // document revision at the last pruneDownloads
	std::unordered_set<AssetId> m_Stalled;
	std::unordered_set<AssetId> m_Reuploadable;
	std::vector<ObjectId> m_LocalMedia; // objects added or changed here since the last tick

	std::unordered_map<uint16_t, RemoteCursor> m_Cursors;
	uint64_t m_CursorsRevision = 0;
	DVec2 m_LocalCursor{ 0.0 };
	bool m_LocalCursorVisible = false;
	bool m_CursorDirty = false;
	std::chrono::steady_clock::time_point m_LastCursorSent;

	// Lines the others are drawing (by user slot), and the last one each finished (late updates of it are ignored)
	std::unordered_map<uint16_t, RemoteStroke> m_RemoteStrokes;
	std::unordered_map<uint16_t, uint32_t> m_EndedStrokes;
	uint64_t m_StrokesRevision = 0;
	// The line drawn here: what the others have (m_StrokeSent stable points), and what they still lack
	bool m_StrokeActive = false;
	bool m_StrokeDirty = false;
	uint32_t m_StrokeId = 0;
	uint32_t m_StrokeSent = 0;
	uint32_t m_StrokeStable = 0;
	DVec2 m_StrokeOrigin{ 0.0 };
	Color m_StrokeColor{};
	std::vector<StrokePoint> m_StrokeUnsent;
	std::chrono::steady_clock::time_point m_LastStrokeSent;

	// Where every video that anybody played or moved was, so people joining later find them in the right place
	struct VideoClock
	{
		bool playing = false;
		double position = 0.0;
		std::chrono::steady_clock::time_point at;
	};
	std::unordered_map<ObjectId, VideoClock> m_VideoStates;
	std::vector<VideoEvent> m_VideoEvents;

	std::mutex m_InboxMutex;
	std::deque<Event> m_Inbox;

	Socket m_Listener;
	std::thread m_AcceptThread;
	std::thread m_ConnectThread;
	std::atomic<bool> m_Stop{ false };
};
} // namespace wb::net
