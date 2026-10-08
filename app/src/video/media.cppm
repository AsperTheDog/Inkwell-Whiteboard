// Video playback: demuxing and decoding with FFmpeg, audio through an SDL audio stream.
//
// A Player decodes a video file held in memory on its own threads (pictures and sound are separate, each with its own
// demuxer) and hands out RGBA pictures when their time has come. The clock is the sound card's when the video has sound
// and the sound device works, the wall clock otherwise. Nothing here knows about Vulkan or the board.
module;
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module wb.video.media;

export namespace wb::video
{
// An encoded file shared between the board's asset table and the decoder threads
using Blob = std::shared_ptr<const std::vector<uint8_t>>;

struct VideoInfo
{
	uint32_t width = 0;    // pixels as shown: pixel aspect ratio and rotation applied
	uint32_t height = 0;
	double duration = 0.0; // seconds; 0 when the container does not say
	double fps = 0.0;
	bool hasAudio = false;
};

// Looks at the container and the first video stream without decoding a picture. Nothing when it is no video.
[[nodiscard]] std::optional<VideoInfo> probe(std::span<const uint8_t> p_Bytes);

// One decoded picture, shown from `pts` on
struct Frame
{
	std::vector<uint8_t> pixels; // RGBA8, width * height * 4
	double pts = 0.0;
	uint64_t serial = 0;         // grows with every picture the player produces
	uint64_t epoch = 0;          // which seek it belongs to
};

class Player
{
public:
	struct Options
	{
		uint32_t maxDimension = 1920; // longer side of the decoded pictures; bigger videos are scaled down
		bool loop = false;
		bool muted = true;
	};

	~Player();
	Player(const Player&) = delete;
	Player& operator=(const Player&) = delete;

	// Opens the file and starts decoding (paused at the start). May take a moment: call it off the UI thread.
	[[nodiscard]] static std::unique_ptr<Player> open(Blob p_Data, const Options& p_Options, std::string& p_Error);

	[[nodiscard]] const VideoInfo& info() const;

	void play();
	void pause();
	void seek(double p_Seconds);
	void setLoop(bool p_Loop);
	void setMuted(bool p_Muted);

	[[nodiscard]] bool playing() const;
	[[nodiscard]] bool ended() const; // stopped at the end of the video
	[[nodiscard]] bool looping() const;
	[[nodiscard]] bool muted() const;
	[[nodiscard]] bool audioWorks() const; // the sound plays (the video has sound and a device took it)
	[[nodiscard]] double position() const;
	[[nodiscard]] double duration() const;

	// Once per frame on the controlling thread: loops or stops at the end
	void tick();
	// The picture that belongs to the current position (null until the first one is decoded). The same object comes
	// back until the next picture is due.
	[[nodiscard]] std::shared_ptr<const Frame> currentFrame();

private:
	Player();
	struct Impl;
	std::unique_ptr<Impl> m_Impl;
};
} // namespace wb::video
