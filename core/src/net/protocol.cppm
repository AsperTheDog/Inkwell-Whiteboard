// The messages two Inkwell instances exchange. Every message is one frame: u32 payload size, u8 type, payload (little
// endian). Everything that arrives is untrusted: sizes are capped before anything is allocated.
//
// All of it travels inside the encrypted channel of wb.net.connection, which swaps its own two key frames first. These
// are the messages after that:
//
//   host -> client   Challenge  (the salt of the password key)
//   client -> host   Hello      (version, name, proof of the password bound to the key exchange)
//   host -> client   Welcome    (your slot, the users, the host's own proof)   or   Reject (reason)
//   host -> client   Snapshot*  (the board, in chunks; the last one is flagged)
//   both ways        Ops        (a batch of edits, in order)
//   host -> clients  UserJoined / UserLeft / UserUpdate
//   both ways        AssetRequest (a picture/video id) answered by AssetChunk* (the file, in order)
module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

export module wb.net.protocol;

import wb.doc.object;
import wb.net.ops;
import wb.net.wire;
import wb.net.crypto;

export namespace wb::net
{
inline constexpr uint16_t PROTOCOL_VERSION = 3;
// Before the password has been checked a peer may only send tiny frames
inline constexpr uint32_t MAX_HANDSHAKE_FRAME = 1024;
// One frame: room for the largest single object plus its framing
inline constexpr uint32_t MAX_FRAME = static_cast<uint32_t>(MAX_OBJECT_BYTES) + (1u << 20);
inline constexpr size_t SNAPSHOT_CHUNK = 1u << 20;
inline constexpr size_t MAX_SNAPSHOT_BYTES = size_t{ 1 } << 30;
// Cursor updates farther apart than this are not sent: the latest position wins
inline constexpr int CURSOR_INTERVAL_MS = 33;
// Pictures and videos travel in pieces this big, at the lowest priority, so cursors and edits are never stuck behind them
inline constexpr size_t ASSET_CHUNK = 64u << 10;
// Files up to this size ride inside the board snapshot; bigger ones are streamed afterwards
inline constexpr size_t SNAPSHOT_INLINE_ASSET_BYTES = 256u << 10;
inline constexpr uint64_t MAX_ASSET_BYTES = uint64_t{ 64 } << 30;
inline constexpr size_t MAX_ASSET_NAME_BYTES = 512;
// Thumbnails are raw RGBA, at most this many pixels on a side
inline constexpr uint32_t MAX_PREVIEW_SIDE = 96;
inline constexpr size_t MAX_NAME_BYTES = 32;
inline constexpr size_t MAX_REASON_BYTES = 200;
inline constexpr uint32_t MAX_USERS = 64;
inline constexpr uint32_t MAX_OPS_PER_FRAME = 1u << 20;
// Live strokes (somebody is drawing): at most this many points in one message, and in one whole stroke
inline constexpr uint32_t MAX_STROKE_POINTS_PER_MESSAGE = 8192;
inline constexpr uint32_t MAX_LIVE_STROKE_POINTS = 1u << 20;

enum class MsgType : uint8_t
{
	Challenge = 1,
	Hello = 2,
	Welcome = 3,
	Reject = 4,
	Snapshot = 5,
	Ops = 6,
	UserJoined = 7,
	UserLeft = 8,
	AssetRequest = 9, // "send me this picture/video"
	AssetChunk = 10,  // a piece of one, answering a request
	AssetStalled = 13, // host -> everyone: this file cannot be had right now (its sender is not sending it)
	AssetPreview = 12, // a thumbnail of a file that is about to be sent
	Cursor = 11,      // where somebody's pointer is (about 30 times a second, only the latest matters)
	StrokePreview = 15, // a line somebody is drawing right now, before they lift the pen
	VideoState = 16,    // somebody pressed play/pause or moved the playhead of a video
	UserUpdate = 17,    // host -> everyone: a user's details changed (a UserInfo), e.g. the host made them read-only
};

struct UserInfo
{
	uint16_t slot = 0;
	std::string name;
	uint32_t color = 0xFFFFFFFFu; // RGBA8
	bool host = false;
	bool readOnly = false; // the host lets this user watch but not change the board
};

struct Challenge
{
	crypto::Salt salt{}; // fixed for the whole session
};
struct Hello
{
	uint16_t version = PROTOCOL_VERSION;
	std::string name;
	crypto::Digest proof{};
};
struct Welcome
{
	uint16_t slot = 0;
	std::vector<UserInfo> users; // everybody including the receiver, the host first
	crypto::Digest proof{};      // the host shows it knows the password too
};
struct Reject
{
	std::string reason;
};
struct SnapshotChunk
{
	bool last = false;
	std::vector<uint8_t> bytes;
};
struct OpsMessage
{
	std::vector<Op> ops;
};
struct UserLeft
{
	uint16_t slot = 0;
};
struct AssetRequest
{
	AssetId id = INVALID_ASSET_ID;
	// Chosen by the asker and echoed in every piece, so pieces of a transfer that was cancelled and asked for again are
	// told apart from the new one
	uint32_t ticket = 0;
};
// A pointer position in board coordinates. Clients send it without a slot; the host fills in the sender's and relays it.
struct CursorMessage
{
	uint16_t slot = 0;
	double x = 0.0;
	double y = 0.0;
	bool visible = false; // false: the pointer left the board
};
// A line being drawn, sent as it grows. The receiver keeps the first `base` points it already has of stroke `stroke`,
// replaces the rest with `points` (the end of a stroke is provisional and changes), and drops the whole thing on `end`.
// Clients send it without a slot; the host fills in the sender's and relays it.
struct StrokePreviewMessage
{
	uint16_t slot = 0;
	uint32_t stroke = 0; // counts up for each line the sender draws
	bool end = false;    // drawing is over (the finished line arrives as an ordinary edit)
	float color[4]{ 0.f, 0.f, 0.f, 1.f };
	double originX = 0.0; // where point (0, 0) is, in board coordinates
	double originY = 0.0;
	uint32_t base = 0;
	std::vector<StrokePoint> points;
};
// Where a video is and whether it plays. Shared by everybody; mute and volume are each person's own (loop travels with the object).
struct VideoStateMessage
{
	ObjectId id = INVALID_OBJECT_ID;
	bool playing = false;
	double position = 0.0; // seconds
};
// width == 0 means "no thumbnail could be made"
struct AssetPreviewMessage
{
	AssetId id = INVALID_ASSET_ID;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba;
};
// Pieces of one file come in order, so the first (offset 0) carries everything but the bytes and later ones repeat it
struct AssetChunk
{
	AssetId id = INVALID_ASSET_ID;
	uint32_t ticket = 0;
	uint64_t total = 0;  // size of the whole file
	uint64_t offset = 0; // where bytes start
	uint32_t width = 0;
	uint32_t height = 0;
	std::string name;
	std::vector<uint8_t> bytes;
};

// Trims, drops control characters and cuts at MAX_NAME_BYTES without splitting a UTF-8 sequence; never empty
[[nodiscard]] std::string sanitizeName(const std::string& p_Name);

[[nodiscard]] std::vector<uint8_t> encode(const Challenge& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const Hello& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const Welcome& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const Reject& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const SnapshotChunk& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const OpsMessage& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const UserInfo& p_Message); // UserJoined
[[nodiscard]] std::vector<uint8_t> encode(const UserLeft& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const AssetRequest& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const CursorMessage& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const StrokePreviewMessage& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const VideoStateMessage& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const AssetPreviewMessage& p_Message);
[[nodiscard]] std::vector<uint8_t> encode(const AssetChunk& p_Message);

[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, Challenge& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, Hello& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, Welcome& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, Reject& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, SnapshotChunk& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, OpsMessage& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, UserInfo& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, UserLeft& p_Message);
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, AssetRequest& p_Message);
// The pixel data must be exactly width * height * 4 bytes, within MAX_PREVIEW_SIDE
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, AssetPreviewMessage& p_Message);
// Positions must be finite and within +-1e12
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, CursorMessage& p_Message);
// A real object id and a position between 0 and a day's worth of seconds
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, VideoStateMessage& p_Message);
// Finite numbers only, colour within 0..1, at most MAX_STROKE_POINTS_PER_MESSAGE points
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, StrokePreviewMessage& p_Message);
// Checks the sizes against the limits and that the bytes stay inside the file
[[nodiscard]] bool decode(std::span<const uint8_t> p_Payload, AssetChunk& p_Message);
} // namespace wb::net
