// GPU side of pictures. An asset is decoded on a worker thread the first time a visible picture needs it, then
// uploaded as one mipmapped 2D-array texture (a layer per animation frame) with its own descriptor set.
module;
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <volk.h>

export module wb.render.image_store;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.gfx.buffer;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.image;
import wb.image.codec;

export namespace wb::render
{
// The animation clock: pictures show the frame that belongs to `seconds`. While not playing, animations hold still.
struct ImageClock
{
	double seconds = 0.0;
	bool playing = true;
};

// What to draw for one picture this frame
struct ImageLookup
{
	VkDescriptorSet set = VK_NULL_HANDLE;
	float layer = -1.f; // < 0: still loading (placeholder)
	bool ready = false;
	bool animatedAndPlaying = false; // the next frame will look different
};

struct ImageInfo
{
	bool ready = false;
	bool failed = false;
	uint32_t width = 0;  // pixels of the texture (may be smaller than the file when scaled to fit the GPU)
	uint32_t height = 0;
	uint32_t frames = 1;
	uint32_t durationMs = 0;
	std::string error;
};

// One picture to draw
struct ImageDraw
{
	VkDescriptorSet set = VK_NULL_HANDLE;
	float layer = -1.f;
	Affine2 localToScreen{};
	Vec2 size{ 0.f };
};

class ImageStore
{
public:
	void init(const gfx::GraphicsContext& p_Context, VkFormat p_ColorFormat);
	// Waits for decoder threads, then frees everything
	void destroy(const gfx::GraphicsContext& p_Context);

	// Outside a rendering scope, once per frame: starts queued decodes, uploads finished ones
	void update(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, VkCommandBuffer p_Cmd);

	// Starts decoding on first use. Never blocks.
	[[nodiscard]] ImageLookup lookup(const Document& p_Document, const ImageData& p_Image, const ImageClock& p_Clock);
	[[nodiscard]] ImageInfo info(AssetId p_Asset) const;
	// The frame an animation shows at the clock's time (to freeze it there)
	[[nodiscard]] uint32_t frameAt(AssetId p_Asset, const ImageClock& p_Clock) const;
	[[nodiscard]] bool animated(AssetId p_Asset) const { return info(p_Asset).frames > 1; }

	// Forgets every texture (the board was replaced)
	void clear();
	// Decodes or uploads are still in flight: keep drawing frames
	[[nodiscard]] bool busy() const { return !m_Queue.empty() || m_Running > 0; }

	void bind(VkCommandBuffer p_Cmd) const;
	void draw(VkCommandBuffer p_Cmd, VkExtent2D p_Extent, const ImageDraw& p_Draw) const;
	[[nodiscard]] VkDescriptorSet placeholderSet() const { return m_PlaceholderSet; }

	// Descriptor sets that bind a 2D-array texture for the picture pipeline (the video store makes its own textures)
	VkDescriptorSet allocateSet(const gfx::GraphicsContext& p_Context, VkImageView p_View);
	// Points an existing set at another texture; the set must not be in use by a frame in flight
	void writeSet(const gfx::GraphicsContext& p_Context, VkDescriptorSet p_Set, VkImageView p_View) const;

private:
	struct Job;
	struct Entry
	{
		enum class State : uint8_t
		{
			Queued,
			Decoding,
			Ready,
			Failed,
		};
		State state = State::Queued;
		std::shared_ptr<Job> job;
		gfx::Image image;
		VkDescriptorSet set = VK_NULL_HANDLE;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t frames = 1;
		std::vector<uint32_t> delaysMs;
		uint32_t totalMs = 0;
		std::string error;
	};

	struct Retired
	{
		gfx::Image image;
	};

	void startJobs();
	// Creates the texture for p_Frames and records its upload; returns the descriptor set
	bool upload(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, VkCommandBuffer p_Cmd, const image::Decoded& p_Decoded, gfx::Image& p_Image, VkDescriptorSet& p_Set);
	static uint32_t frameFor(const Entry& p_Entry, double p_Seconds);

	std::unordered_map<AssetId, Entry> m_Entries;
	std::deque<std::pair<AssetId, std::vector<uint8_t>>> m_Queue;
	std::shared_ptr<std::atomic<int>> m_ActiveThreads;
	int m_Running = 0;
	std::vector<gfx::Image> m_Retired;
	uint32_t m_MaxDimension = 8192;
	uint32_t m_MaxLayers = 256;

	gfx::Image m_Placeholder;
	VkDescriptorSet m_PlaceholderSet = VK_NULL_HANDLE;
	bool m_PlaceholderReady = false;

	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
	std::vector<VkDescriptorPool> m_Pools;
	uint32_t m_PoolUsed = 0;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
};
} // namespace wb::render
