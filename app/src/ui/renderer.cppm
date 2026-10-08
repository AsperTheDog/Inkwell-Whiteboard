// Draws a DrawList with Vulkan: one instanced quad per primitive, glyphs from an R8 atlas that is updated
// incrementally as new glyphs are rasterized.
module;
#include <array>
#include <cstdint>
#include <volk.h>

export module wb.ui.renderer;

import wb.gfx.buffer;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.image;
import wb.ui.draw;
import wb.ui.font;

export namespace wb::ui
{
class UiRenderer
{
public:
	void init(const gfx::GraphicsContext& p_Context, VkFormat p_ColorFormat);
	void destroy(const gfx::GraphicsContext& p_Context);

	// Outside a rendering scope: uploads glyphs added to the atlas and this frame's primitives
	void prepare(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::StagingBelt& p_Staging, VkCommandBuffer p_Cmd, uint32_t p_Slot, FontAtlas& p_Font, const DrawList& p_List);
	// Inside the rendering scope of the frame target
	void record(VkCommandBuffer p_Cmd, VkExtent2D p_Extent) const;

private:
	struct FrameResources
	{
		gfx::Buffer prims;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	};

	std::array<FrameResources, gfx::FRAMES_IN_FLIGHT> m_Frames{};
	uint32_t m_Slot = 0;
	uint32_t m_PrimCount = 0;

	gfx::Image m_Atlas;
	VkSampler m_Sampler = VK_NULL_HANDLE;
	bool m_AtlasInitialized = false;

	VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
};
} // namespace wb::ui
