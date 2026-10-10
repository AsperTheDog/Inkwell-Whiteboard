// Video playback: demuxing and decoding with FFmpeg, audio through an SDL audio stream.
//
// A Player decodes a video file read from a ByteSource on its own threads (pictures and sound are separate, each with its own
// demuxer) and hands out RGBA pictures when their time has come. The clock is the sound card's when the video has sound
// and the sound device works, the wall clock otherwise. Nothing here knows about Vulkan or the board.
module;
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module wb.video.media;

import wb.io.blob;

export namespace wb::video
{
// The encoded file is read through a ByteSource, so a video of any size plays straight from where it is on disk (or from
// memory, or while it is still being downloaded: the decoder then waits for the bytes it needs).
using Source = std::shared_ptr<const ByteSource>;

// A sound track or a subtitle track of the file, as it describes itself
struct TrackInfo
{
	std::string language; // "eng", "spa"... as the file says; may be empty
	std::string title;
	std::string codec;
};

struct VideoInfo
{
	uint32_t width = 0;    // pixels as shown: pixel aspect ratio and rotation applied
	uint32_t height = 0;
	double duration = 0.0; // seconds; 0 when the container does not say
	double fps = 0.0;
	bool hasAudio = false;
	std::vector<TrackInfo> audioTracks;
	int defaultAudioTrack = 0;                // the one the file prefers
	std::vector<TrackInfo> subtitleTracks;    // only the ones made of text; picture subtitles (DVD, Blu-ray) are not shown
};

// Looks at the container and the first video stream without decoding a picture. Nothing when it is no video.
[[nodiscard]] std::optional<VideoInfo> probe(std::span<const uint8_t> p_Bytes);
[[nodiscard]] std::optional<VideoInfo> probe(const ByteSource& p_Source);

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
		float volume = 0.f; // sound gain, 0 = silent
		int audioTrack = -1;    // position among info().audioTracks; -1 or out of range: the file's own choice
		int subtitleTrack = -1; // position among info().subtitleTracks; -1: none
		// Set by the owner to give up on a player that is still opening (it may be waiting for a download)
		std::shared_ptr<std::atomic<bool>> cancel;
	};

	~Player();
	Player(const Player&) = delete;
	Player& operator=(const Player&) = delete;

	// Opens the file and starts decoding (paused at the start). May take a moment: call it off the UI thread.
	[[nodiscard]] static std::unique_ptr<Player> open(Source p_Data, const Options& p_Options, std::string& p_Error);

	[[nodiscard]] const VideoInfo& info() const;

	void play();
	void pause();
	void seek(double p_Seconds);
	void setLoop(bool p_Loop);
	void setVolume(float p_Gain);
	// Another sound track from the current place on, and subtitles from the next line on. Picture and clock go on.
	void setAudioTrack(int p_Track);
	void setSubtitleTrack(int p_Track);
	[[nodiscard]] int audioTrack() const;    // the one that plays
	[[nodiscard]] int subtitleTrack() const; // -1: none
	// The subtitle line to show at the current position (empty: none). Lines are separated by '\n'.
	[[nodiscard]] std::string subtitle() const;

	[[nodiscard]] bool playing() const;
	[[nodiscard]] bool ended() const; // stopped at the end of the video
	[[nodiscard]] bool looping() const;
	[[nodiscard]] bool audioWorks() const; // the sound plays (the video has sound and a device took it)
	// Waiting for the picture of a seek, or for pictures that are slow to come (a huge file, a slow disk, a download):
	// what is on screen is not what the clock says. Only true after a moment, so ordinary seeks do not flicker.
	[[nodiscard]] bool buffering() const;
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
