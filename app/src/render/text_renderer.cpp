module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <volk.h>
#include "gfx/vk_check.hpp"

module wb.render.text_renderer;

import wb.math;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.image;
import wb.gfx.pipeline;
import wb.text.system;

namespace wb::render
{
namespace
{
struct PushConstants
{
	float viewportSize[2];
	float padding0[2];
	float linear[4]; // column-major 2x2
	float translation[2];
	float padding1[2];
	float color[4];
};
static_assert(sizeof(PushConstants) == 64);

constexpr VkDeviceSize MIN_GLYPH_BUFFER_BYTES = 64 * 1024;
} // namespace

void TextRenderer::init(const gfx::GraphicsContext& p_Context, const VkFormat p_ColorFormat)
{
	const VkDevice l_Device = p_Context.device();

	const VkDescriptorSetLayoutBinding l_Bindings[]{
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
	};
	const VkDescriptorSetLayoutCreateInfo l_LayoutInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = l_Bindings };
	WB_VK_CHECK(vkCreateDescriptorSetLayout(l_Device, &l_LayoutInfo, nullptr, &m_SetLayout));

	const VkDescriptorPoolSize l_PoolSizes[]{
		{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = gfx::RENDER_SLOTS },
		{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = gfx::RENDER_SLOTS },
	};
	const VkDescriptorPoolCreateInfo l_PoolInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = gfx::RENDER_SLOTS, .poolSizeCount = 2, .pPoolSizes = l_PoolSizes };
	WB_VK_CHECK(vkCreateDescriptorPool(l_Device, &l_PoolInfo, nullptr, &m_DescriptorPool));

	std::array<VkDescriptorSetLayout, gfx::RENDER_SLOTS> l_Layouts{};
	l_Layouts.fill(m_SetLayout);
	std::array<VkDescriptorSet, gfx::RENDER_SLOTS> l_Sets{};
	const VkDescriptorSetAllocateInfo l_AllocInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = m_DescriptorPool, .descriptorSetCount = gfx::RENDER_SLOTS, .pSetLayouts = l_Layouts.data() };
	WB_VK_CHECK(vkAllocateDescriptorSets(l_Device, &l_AllocInfo, l_Sets.data()));

	m_Atlas = gfx::createImage2D(p_Context, VkExtent2D{ text::GlyphAtlas::SIZE, text::GlyphAtlas::SIZE }, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "text glyph atlas");

	const VkSamplerCreateInfo l_SamplerInfo{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	WB_VK_CHECK(vkCreateSampler(l_Device, &l_SamplerInfo, nullptr, &m_Sampler));

	for (uint32_t i = 0; i < gfx::RENDER_SLOTS; ++i)
	{
		m_Frames[i].descriptorSet = l_Sets[i];
		m_Frames[i].glyphs = gfx::createBuffer(p_Context, MIN_GLYPH_BUFFER_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryKind::Upload, "text glyphs");
		const VkDescriptorImageInfo l_ImageInfo{ .sampler = m_Sampler, .imageView = m_Atlas.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet l_Write{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = l_Sets[i],
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &l_ImageInfo,
		};
		vkUpdateDescriptorSets(l_Device, 1, &l_Write, 0, nullptr);
	}

	const VkShaderModule l_Module = gfx::loadShaderModule(p_Context, "text");
	m_Layout = gfx::createPipelineLayout(p_Context, std::span(&m_SetLayout, 1), sizeof(PushConstants), "text layout");
	m_Pipeline = gfx::createGraphicsPipeline(p_Context, { .module = l_Module, .layout = m_Layout, .colorFormat = p_ColorFormat, .name = "text pipeline" });
	vkDestroyShaderModule(l_Device, l_Module, nullptr);
}

void TextRenderer::destroy(const gfx::GraphicsContext& p_Context)
{
	const VkDevice l_Device = p_Context.device();
	for (FrameResources& l_Frame : m_Frames)
		gfx::destroyBuffer(p_Context, l_Frame.glyphs);
	gfx::destroyImage(p_Context, m_Atlas);
	vkDestroySampler(l_Device, m_Sampler, nullptr);
	vkDestroyPipeline(l_Device, m_Pipeline, nullptr);
	vkDestroyPipelineLayout(l_Device, m_Layout, nullptr);
	vkDestroyDescriptorPool(l_Device, m_DescriptorPool, nullptr);
	vkDestroyDescriptorSetLayout(l_Device, m_SetLayout, nullptr);
	m_Sampler = VK_NULL_HANDLE;
	m_Pipeline = VK_NULL_HANDLE;
	m_Layout = VK_NULL_HANDLE;
	m_DescriptorPool = VK_NULL_HANDLE;
	m_SetLayout = VK_NULL_HANDLE;
}

void TextRenderer::begin()
{
	m_Draws.clear();
}

uint32_t TextRenderer::add(const uint32_t p_FirstGlyph, const uint32_t p_GlyphCount, const Affine2& p_LocalToScreen, const Color p_Color)
{
	m_Draws.push_back(Draw{ .first = p_FirstGlyph, .count = p_GlyphCount, .localToScreen = p_LocalToScreen, .color = p_Color });
	return static_cast<uint32_t>(m_Draws.size() - 1);
}

void TextRenderer::upload(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::StagingBelt& p_Staging, const VkCommandBuffer p_Cmd, const uint32_t p_Slot, text::GlyphAtlas& p_Atlas, const std::vector<text::GpuGlyph>& p_Glyphs)
{
	m_Slot = p_Slot;
	constexpr uint32_t ATLAS_SIZE = text::GlyphAtlas::SIZE;

	uint32_t l_FirstRow = 0, l_RowCount = 0;
	bool l_HasRows = p_Atlas.takeDirtyRows(l_FirstRow, l_RowCount);
	if (!m_AtlasInitialized)
	{
		l_FirstRow = 0;
		l_RowCount = ATLAS_SIZE;
		l_HasRows = true;
	}
	if (l_HasRows)
	{
		const VkDeviceSize l_RowBytes = ATLAS_SIZE;
		const gfx::StagingAllocation l_Staging = p_Staging.allocate(p_Context, l_RowBytes * l_RowCount, 16);
		std::memcpy(l_Staging.data.data(), p_Atlas.pixels() + static_cast<size_t>(l_FirstRow) * ATLAS_SIZE, static_cast<size_t>(l_RowBytes * l_RowCount));

		gfx::transitionImage(p_Cmd, m_Atlas.handle, {
			.oldLayout = m_AtlasInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcStage = m_AtlasInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
			.srcAccess = m_AtlasInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
			.dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
			.dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		});
		const VkBufferImageCopy l_Region{
			.bufferOffset = l_Staging.offset,
			.bufferRowLength = ATLAS_SIZE,
			.bufferImageHeight = l_RowCount,
			.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
			.imageOffset = { 0, static_cast<int32_t>(l_FirstRow), 0 },
			.imageExtent = { ATLAS_SIZE, l_RowCount, 1 },
		};
		vkCmdCopyBufferToImage(p_Cmd, l_Staging.buffer, m_Atlas.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &l_Region);
		gfx::transitionImage(p_Cmd, m_Atlas.handle, {
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
			.srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		});
		m_AtlasInitialized = true;
	}

	FrameResources& l_Frame = m_Frames[p_Slot];
	const VkDeviceSize l_Needed = std::max<VkDeviceSize>(p_Glyphs.size() * sizeof(text::GpuGlyph), 16);
	if (l_Frame.glyphs.size < l_Needed)
	{
		gfx::destroyBufferDeferred(p_Context, p_Frames, l_Frame.glyphs);
		l_Frame.glyphs = gfx::createBuffer(p_Context, std::max<VkDeviceSize>(l_Needed + l_Needed / 2, MIN_GLYPH_BUFFER_BYTES), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryKind::Upload, "text glyphs");
	}
	if (!p_Glyphs.empty())
		std::memcpy(l_Frame.glyphs.mapped, p_Glyphs.data(), p_Glyphs.size() * sizeof(text::GpuGlyph));

	const VkDescriptorBufferInfo l_BufferInfo{ .buffer = l_Frame.glyphs.handle, .offset = 0, .range = VK_WHOLE_SIZE };
	const VkWriteDescriptorSet l_Write{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = l_Frame.descriptorSet,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &l_BufferInfo,
	};
	vkUpdateDescriptorSets(p_Context.device(), 1, &l_Write, 0, nullptr);
}

void TextRenderer::bind(const VkCommandBuffer p_Cmd) const
{
	vkCmdBindPipeline(p_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
	vkCmdBindDescriptorSets(p_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Layout, 0, 1, &m_Frames[m_Slot].descriptorSet, 0, nullptr);
}

void TextRenderer::draw(const VkCommandBuffer p_Cmd, const VkExtent2D p_Extent, const uint32_t p_Index) const
{
	const Draw& l_Draw = m_Draws[p_Index];
	if (l_Draw.count == 0)
		return;
	const PushConstants l_Push{
		.viewportSize = { static_cast<float>(p_Extent.width), static_cast<float>(p_Extent.height) },
		.padding0 = { 0.f, 0.f },
		.linear = {
			static_cast<float>(l_Draw.localToScreen.linear[0][0]),
			static_cast<float>(l_Draw.localToScreen.linear[0][1]),
			static_cast<float>(l_Draw.localToScreen.linear[1][0]),
			static_cast<float>(l_Draw.localToScreen.linear[1][1]),
		},
		.translation = { static_cast<float>(l_Draw.localToScreen.translation.x), static_cast<float>(l_Draw.localToScreen.translation.y) },
		.padding1 = { 0.f, 0.f },
		.color = { l_Draw.color.r, l_Draw.color.g, l_Draw.color.b, l_Draw.color.a },
	};
	vkCmdPushConstants(p_Cmd, m_Layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(l_Push), &l_Push);
	vkCmdDraw(p_Cmd, 6, l_Draw.count, 0, l_Draw.first);
}
} // namespace wb::render
