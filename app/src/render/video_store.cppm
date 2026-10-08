// GPU side of videos. A video that is on screen gets a Player (opened on a worker thread) and a small ring of
// mipmapped textures; every picture the player has due is copied into the next texture of the ring, which the board
// then draws like a still picture.
module;
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <volk.h>

export module wb.render.video_store;

import wb.doc.object;
import wb.doc.document;
import wb.gfx.buffer;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.image;
import wb.render.image_store;
import wb.video.media;

export namespace wb::render
{
// What the viewer needs to show about a video
struct VideoStatus
{
	bool known = false; // the store has an entry for it (it was on screen)
	bool ready = false;
	bool failed = false;
	bool playing = false;
	bool ended = false;
	bool hasAudio = false;
	bool audioWorks = false;
	double position = 0.0;
	double duration = 0.0;
	std::string error;
};

struct VideoLookup
{
	VkDescriptorSet set = VK_NULL_HANDLE;
	float layer = -1.f; // < 0: still opening (placeholder)
};

class VideoStore
{
public:
	void init(const gfx::GraphicsContext& p_Context, ImageStore& p_Images);
	// Stops every player, then frees the textures
	void destroy(const gfx::GraphicsContext& p_Context);

	// Outside a rendering scope, once per frame: finishes openings, advances the players and uploads due pictures
	void update(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, VkCommandBuffer p_Cmd);

	// What to draw for a video that is on screen this frame; starts opening it on first use. Never blocks.
	[[nodiscard]] VideoLookup lookup(const Document& p_Document, ObjectId p_Id, const VideoData& p_Video);

	[[nodiscard]] VideoStatus status(ObjectId p_Id) const;
	// Work with players that may not exist yet: the request is kept and applied when the video has opened
	void setPlaying(ObjectId p_Id, bool p_Playing);
	void seek(ObjectId p_Id, double p_Seconds);

	// The object left the board (its player stops at once; the textures go at the next update)
	void forget(ObjectId p_Id);
	void clear();
	// A video plays out of sight: the app has to wake up now and then so it can loop or stop
	[[nodiscard]] bool playingInBackground() const { return m_PlayingInBackground; }

	// Frames must keep coming: a visible video plays, is opening or waits for its first picture
	[[nodiscard]] bool animating() const { return m_Animating; }

private:
	static constexpr size_t RING = 3;

	struct OpenJob
	{
		std::atomic<bool> done{ false };
		std::unique_ptr<video::Player> player;
		std::string error;
	};

	struct Texture
	{
		gfx::Image image;
		VkDescriptorSet set = VK_NULL_HANDLE;
	};

	struct Entry
	{
		enum class State : uint8_t
		{
			Opening,
			Ready,
			Failed,
		};
		State state = State::Opening;
		AssetId asset = INVALID_ASSET_ID;
		std::shared_ptr<OpenJob> job;
		std::unique_ptr<video::Player> player;
		video::VideoInfo info;
		std::array<Texture, RING> textures{};
		std::array<gfx::Buffer, gfx::FRAMES_IN_FLIGHT> staging{};
		size_t next = 0;                  // texture the next picture goes into
		int shown = -1;                   // texture on screen
		uint64_t uploadedSerial = 0;
		bool wantPlay = false;
		std::optional<double> wantSeek;
		bool loop = false;
		float gain = 0.f;
		uint64_t seenTick = 0;
		std::chrono::steady_clock::time_point seenTime{};
		double resumeAt = 0.0;
		std::string error;
	};

	void startOpen(Entry& p_Entry, const Document& p_Document);
	bool createTextures(const gfx::GraphicsContext& p_Context, Entry& p_Entry);
	void upload(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, VkCommandBuffer p_Cmd, Entry& p_Entry, const video::Frame& p_Frame);
	void release(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, Entry& p_Entry);

	ImageStore* m_Images = nullptr;
	std::unordered_map<ObjectId, Entry> m_Entries;
	std::shared_ptr<std::atomic<int>> m_ActiveThreads = std::make_shared<std::atomic<int>>(0);
	std::shared_ptr<std::vector<VkDescriptorSet>> m_FreeSets = std::make_shared<std::vector<VkDescriptorSet>>();
	std::vector<Entry> m_Retired;
	std::unordered_map<ObjectId, double> m_Resume; // where a video was when it left the screen
	std::unordered_map<AssetId, std::weak_ptr<const std::vector<uint8_t>>> m_Blobs;
	uint64_t m_Tick = 0;
	uint32_t m_MaxDimension = 1920;
	bool m_Animating = false;
	bool m_PlayingInBackground = false;
};
} // namespace wb::render
