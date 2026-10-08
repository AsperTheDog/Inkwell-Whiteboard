// Draws text objects: glyph quads from a signed distance field atlas that grows as new glyphs are needed. The canvas
// renderer collects the glyphs of every visible text object in z-order with add(), then upload() sends them and the
// atlas rows that changed to the GPU, and bind()/draw() record the draws between strokes and pictures.
module;
#include <array>
#include <cstdint>
#include <vector>
#include <volk.h>

export module wb.render.text_renderer;

import wb.math;
import wb.gfx.buffer;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.image;
import wb.text.system;

export namespace wb::render
{
class TextRenderer
{
public:
	void init(const gfx::GraphicsContext& p_Context, VkFormat p_ColorFormat);
	void destroy(const gfx::GraphicsContext& p_Context);

	// Starts a frame's list of text draws
	void begin();
	// Adds one text object whose glyph records are [p_FirstGlyph, p_FirstGlyph + p_GlyphCount) of the list given to
	// upload(); returns the draw index for draw()
	uint32_t add(uint32_t p_FirstGlyph, uint32_t p_GlyphCount, const Affine2& p_LocalToScreen, Color p_Color);
	// Outside a rendering scope: uploads the atlas rows that changed and the frame's glyph records
	void upload(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::StagingBelt& p_Staging, VkCommandBuffer p_Cmd, uint32_t p_Slot, text::GlyphAtlas& p_Atlas, const std::vector<text::GpuGlyph>& p_Glyphs);

	// Inside the rendering scope
	void bind(VkCommandBuffer p_Cmd) const;
	void draw(VkCommandBuffer p_Cmd, VkExtent2D p_Extent, uint32_t p_Index) const;

	[[nodiscard]] uint32_t drawCount() const { return static_cast<uint32_t>(m_Draws.size()); }

private:
	struct Draw
	{
		uint32_t first = 0;
		uint32_t count = 0;
		Affine2 localToScreen{};
		Color color{};
	};

	struct FrameResources
	{
		gfx::Buffer glyphs;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	};

	std::array<FrameResources, gfx::FRAMES_IN_FLIGHT> m_Frames{};
	uint32_t m_Slot = 0;
	std::vector<Draw> m_Draws;

	gfx::Image m_Atlas;
	VkSampler m_Sampler = VK_NULL_HANDLE;
	bool m_AtlasInitialized = false;

	VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
};
} // namespace wb::render
