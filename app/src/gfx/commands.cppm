// Small command-recording helpers (synchronization2 barriers, dynamic rendering, debug labels).
module;
#include <cstdint>
#include <volk.h>

export module wb.gfx.commands;

export namespace wb::gfx
{
struct ImageTransition
{
	VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_NONE;
	VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
	VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_NONE;
	VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	uint32_t baseMip = 0;
	uint32_t mipCount = VK_REMAINING_MIP_LEVELS;
	uint32_t baseLayer = 0;
	uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS;
};

inline void transitionImage(const VkCommandBuffer p_Cmd, const VkImage p_Image, const ImageTransition& p_Transition)
{
	const VkImageMemoryBarrier2 l_Barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = p_Transition.srcStage,
		.srcAccessMask = p_Transition.srcAccess,
		.dstStageMask = p_Transition.dstStage,
		.dstAccessMask = p_Transition.dstAccess,
		.oldLayout = p_Transition.oldLayout,
		.newLayout = p_Transition.newLayout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = p_Image,
		.subresourceRange = {
			.aspectMask = p_Transition.aspect,
			.baseMipLevel = p_Transition.baseMip,
			.levelCount = p_Transition.mipCount,
			.baseArrayLayer = p_Transition.baseLayer,
			.layerCount = p_Transition.layerCount,
		},
	};
	const VkDependencyInfo l_Dependency{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &l_Barrier,
	};
	vkCmdPipelineBarrier2(p_Cmd, &l_Dependency);
}

inline void memoryBarrier(const VkCommandBuffer p_Cmd, const VkPipelineStageFlags2 p_SrcStage, const VkAccessFlags2 p_SrcAccess, const VkPipelineStageFlags2 p_DstStage, const VkAccessFlags2 p_DstAccess)
{
	const VkMemoryBarrier2 l_Barrier{
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		.srcStageMask = p_SrcStage,
		.srcAccessMask = p_SrcAccess,
		.dstStageMask = p_DstStage,
		.dstAccessMask = p_DstAccess,
	};
	const VkDependencyInfo l_Dependency{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &l_Barrier,
	};
	vkCmdPipelineBarrier2(p_Cmd, &l_Dependency);
}

// Begins dynamic rendering into a single color attachment
inline void beginColorRendering(const VkCommandBuffer p_Cmd, const VkImageView p_View, const VkExtent2D p_Extent, const VkAttachmentLoadOp p_LoadOp, const VkClearColorValue p_Clear = {})
{
	const VkRenderingAttachmentInfo l_Color{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = p_View,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = p_LoadOp,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .color = p_Clear },
	};
	const VkRenderingInfo l_Info{
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { .offset = { 0, 0 }, .extent = p_Extent },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &l_Color,
	};
	vkCmdBeginRendering(p_Cmd, &l_Info);
}

// Full-target viewport (y down, matching window coordinates) and scissor
inline void setViewportAndScissor(const VkCommandBuffer p_Cmd, const VkExtent2D p_Extent)
{
	const VkViewport l_Viewport{
		.x = 0.f,
		.y = 0.f,
		.width = static_cast<float>(p_Extent.width),
		.height = static_cast<float>(p_Extent.height),
		.minDepth = 0.f,
		.maxDepth = 1.f,
	};
	const VkRect2D l_Scissor{ .offset = { 0, 0 }, .extent = p_Extent };
	vkCmdSetViewport(p_Cmd, 0, 1, &l_Viewport);
	vkCmdSetScissor(p_Cmd, 0, 1, &l_Scissor);
}

// Set by GraphicsContext once VK_EXT_debug_utils is enabled on the instance
inline bool g_DebugLabelsEnabled = false;

// RAII debug label region (no-op unless VK_EXT_debug_utils is enabled)
class DebugLabel
{
public:
	DebugLabel(const VkCommandBuffer p_Cmd, const char* p_Name) : m_Cmd(p_Cmd)
	{
		if (!g_DebugLabelsEnabled)
		{
			m_Cmd = VK_NULL_HANDLE;
			return;
		}
		const VkDebugUtilsLabelEXT l_Label{ .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = p_Name, .color = { 1.f, 1.f, 1.f, 1.f } };
		vkCmdBeginDebugUtilsLabelEXT(m_Cmd, &l_Label);
	}
	~DebugLabel()
	{
		if (m_Cmd != VK_NULL_HANDLE)
			vkCmdEndDebugUtilsLabelEXT(m_Cmd);
	}
	DebugLabel(const DebugLabel&) = delete;
	DebugLabel& operator=(const DebugLabel&) = delete;

private:
	VkCommandBuffer m_Cmd;
};
} // namespace wb::gfx
