module;
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>
#include "gfx/vk_check.hpp"
#include "gfx/vma.hpp"

module wb.render.image_store;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.image;
import wb.gfx.pipeline;
import wb.image.codec;

namespace wb::render
{
struct ImageStore::Job
{
	std::atomic<bool> done{ false };
	std::optional<image::Decoded> result;
	std::string error;
};

namespace
{
constexpr int MAX_DECODER_THREADS = 4;
constexpr uint32_t SETS_PER_POOL = 128;
constexpr VkFormat TEXTURE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr size_t UPLOADS_PER_FRAME = 2;

struct PushConstants
{
	float viewportSize[2];
	float inflate;
	float layer;
	float linear[4];
	float translation[2];
	float size[2];
	float opacity;
	float padding[3];
};
static_assert(sizeof(PushConstants) == 64);

uint32_t mipCountFor(const uint32_t p_Width, const uint32_t p_Height)
{
	uint32_t l_Count = 1;
	for (uint32_t l_Size = std::max(p_Width, p_Height); l_Size > 1; l_Size >>= 1)
		++l_Count;
	return l_Count;
}
} // namespace

void ImageStore::init(const gfx::GraphicsContext& p_Context, const VkFormat p_ColorFormat)
{
	const VkDevice l_Device = p_Context.device();
	const VkPhysicalDeviceLimits& l_Limits = p_Context.info().limits;
	m_MaxDimension = std::min<uint32_t>(l_Limits.maxImageDimension2D, 16384);
	m_MaxLayers = std::max<uint32_t>(1, std::min<uint32_t>(l_Limits.maxImageArrayLayers, 2048));
	m_ActiveThreads = std::make_shared<std::atomic<int>>(0);

	const VkSamplerCreateInfo l_SamplerInfo{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.minLod = 0.f,
		.maxLod = VK_LOD_CLAMP_NONE,
	};
	WB_VK_CHECK(vkCreateSampler(l_Device, &l_SamplerInfo, nullptr, &m_Sampler));

	const VkDescriptorSetLayoutBinding l_Binding{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
	const VkDescriptorSetLayoutCreateInfo l_LayoutInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &l_Binding };
	WB_VK_CHECK(vkCreateDescriptorSetLayout(l_Device, &l_LayoutInfo, nullptr, &m_SetLayout));

	const VkShaderModule l_Module = gfx::loadShaderModule(p_Context, "image");
	m_Layout = gfx::createPipelineLayout(p_Context, std::span(&m_SetLayout, 1), sizeof(PushConstants), "image layout");
	m_Pipeline = gfx::createGraphicsPipeline(p_Context, { .module = l_Module, .layout = m_Layout, .colorFormat = p_ColorFormat, .name = "image pipeline" });
	vkDestroyShaderModule(l_Device, l_Module, nullptr);
}

void ImageStore::destroy(const gfx::GraphicsContext& p_Context)
{
	// Decoder threads hold only their own data, but must not outlive the program
	for (int l_Wait = 0; l_Wait < 20000 && m_ActiveThreads && m_ActiveThreads->load() > 0; ++l_Wait)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	const VkDevice l_Device = p_Context.device();
	for (auto& [l_Id, l_Entry] : m_Entries)
		gfx::destroyImage(p_Context, l_Entry.image);
	m_Entries.clear();
	for (gfx::Image& l_Image : m_Retired)
		gfx::destroyImage(p_Context, l_Image);
	m_Retired.clear();
	gfx::destroyImage(p_Context, m_Placeholder);
	for (const VkDescriptorPool l_Pool : m_Pools)
		vkDestroyDescriptorPool(l_Device, l_Pool, nullptr);
	m_Pools.clear();
	vkDestroyPipeline(l_Device, m_Pipeline, nullptr);
	vkDestroyPipelineLayout(l_Device, m_Layout, nullptr);
	vkDestroyDescriptorSetLayout(l_Device, m_SetLayout, nullptr);
	vkDestroySampler(l_Device, m_Sampler, nullptr);
	m_Pipeline = VK_NULL_HANDLE;
	m_Layout = VK_NULL_HANDLE;
	m_SetLayout = VK_NULL_HANDLE;
	m_Sampler = VK_NULL_HANDLE;
}

VkDescriptorSet ImageStore::allocateSet(const gfx::GraphicsContext& p_Context, const VkImageView p_View)
{
	const VkDevice l_Device = p_Context.device();
	VkDescriptorSet l_Set = VK_NULL_HANDLE;
	for (int l_Attempt = 0; l_Attempt < 2; ++l_Attempt)
	{
		if (m_Pools.empty() || m_PoolUsed >= SETS_PER_POOL)
		{
			const VkDescriptorPoolSize l_Size{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = SETS_PER_POOL };
			const VkDescriptorPoolCreateInfo l_PoolInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SETS_PER_POOL, .poolSizeCount = 1, .pPoolSizes = &l_Size };
			VkDescriptorPool l_Pool = VK_NULL_HANDLE;
			WB_VK_CHECK(vkCreateDescriptorPool(l_Device, &l_PoolInfo, nullptr, &l_Pool));
			m_Pools.push_back(l_Pool);
			m_PoolUsed = 0;
		}
		const VkDescriptorSetAllocateInfo l_AllocInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = m_Pools.back(), .descriptorSetCount = 1, .pSetLayouts = &m_SetLayout };
		if (vkAllocateDescriptorSets(l_Device, &l_AllocInfo, &l_Set) == VK_SUCCESS)
		{
			++m_PoolUsed;
			break;
		}
		m_PoolUsed = SETS_PER_POOL; // full: the next attempt makes a new pool
	}
	const VkDescriptorImageInfo l_ImageInfo{ .sampler = m_Sampler, .imageView = p_View, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	const VkWriteDescriptorSet l_Write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = l_Set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &l_ImageInfo };
	vkUpdateDescriptorSets(l_Device, 1, &l_Write, 0, nullptr);
	return l_Set;
}

bool ImageStore::upload(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, const VkCommandBuffer p_Cmd, const image::Decoded& p_Decoded, gfx::Image& p_Image, VkDescriptorSet& p_Set)
{
	const uint32_t l_Layers = static_cast<uint32_t>(std::min<size_t>(p_Decoded.frames.size(), m_MaxLayers));
	if (l_Layers == 0 || p_Decoded.width == 0 || p_Decoded.height == 0)
		return false;
	const uint32_t l_Mips = mipCountFor(p_Decoded.width, p_Decoded.height);
	p_Image = gfx::createImage2D(p_Context, VkExtent2D{ p_Decoded.width, p_Decoded.height }, TEXTURE_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "picture", l_Mips, l_Layers, true);

	const VkDeviceSize l_LayerBytes = static_cast<VkDeviceSize>(p_Decoded.width) * p_Decoded.height * 4;
	gfx::Buffer l_Staging = gfx::createBuffer(p_Context, l_LayerBytes * l_Layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryKind::Upload, "picture upload");
	std::vector<VkBufferImageCopy> l_Copies;
	l_Copies.reserve(l_Layers);
	for (uint32_t i = 0; i < l_Layers; ++i)
	{
		std::memcpy(l_Staging.mapped + static_cast<size_t>(i) * l_LayerBytes, p_Decoded.frames[i].data(), static_cast<size_t>(l_LayerBytes));
		l_Copies.push_back(VkBufferImageCopy{
			.bufferOffset = i * l_LayerBytes,
			.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = i, .layerCount = 1 },
			.imageExtent = { p_Decoded.width, p_Decoded.height, 1 },
		});
	}

	gfx::transitionImage(p_Cmd, p_Image.handle, {
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_NONE,
		.srcAccess = VK_ACCESS_2_NONE,
		.dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
	});
	vkCmdCopyBufferToImage(p_Cmd, l_Staging.handle, p_Image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(l_Copies.size()), l_Copies.data());

	// Mip chain: each level is blitted from the one above
	for (uint32_t l_Level = 1; l_Level < l_Mips; ++l_Level)
	{
		gfx::transitionImage(p_Cmd, p_Image.handle, {
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
			.srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStage = VK_PIPELINE_STAGE_2_BLIT_BIT,
			.dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
			.baseMip = l_Level - 1,
			.mipCount = 1,
		});
		const auto l_Extent = [&](const uint32_t p_Level) { return VkOffset3D{ static_cast<int32_t>(std::max(1u, p_Decoded.width >> p_Level)), static_cast<int32_t>(std::max(1u, p_Decoded.height >> p_Level)), 1 }; };
		const VkImageBlit l_Blit{
			.srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = l_Level - 1, .baseArrayLayer = 0, .layerCount = l_Layers },
			.srcOffsets = { VkOffset3D{ 0, 0, 0 }, l_Extent(l_Level - 1) },
			.dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = l_Level, .baseArrayLayer = 0, .layerCount = l_Layers },
			.dstOffsets = { VkOffset3D{ 0, 0, 0 }, l_Extent(l_Level) },
		};
		vkCmdBlitImage(p_Cmd, p_Image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p_Image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &l_Blit, VK_FILTER_LINEAR);
	}

	// Everything to shader-read: the upper levels are in TRANSFER_SRC, the last one still in TRANSFER_DST
	if (l_Mips > 1)
	{
		gfx::transitionImage(p_Cmd, p_Image.handle, {
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcStage = VK_PIPELINE_STAGE_2_BLIT_BIT,
			.srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
			.dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.baseMip = 0,
			.mipCount = l_Mips - 1,
		});
	}
	gfx::transitionImage(p_Cmd, p_Image.handle, {
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
		.srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		.dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		.baseMip = l_Mips - 1,
		.mipCount = 1,
	});

	gfx::destroyBufferDeferred(p_Context, p_Frames, l_Staging);
	p_Set = allocateSet(p_Context, p_Image.view);
	return true;
}

void ImageStore::startJobs()
{
	int l_Running = 0;
	for (const auto& [l_Id, l_Entry] : m_Entries)
		l_Running += l_Entry.state == Entry::State::Decoding ? 1 : 0;

	while (l_Running < MAX_DECODER_THREADS && !m_Queue.empty())
	{
		std::pair<AssetId, std::vector<uint8_t>> l_Item = std::move(m_Queue.front());
		m_Queue.pop_front();
		const AssetId l_Asset = l_Item.first;
		const auto l_It = m_Entries.find(l_Asset);
		if (l_It == m_Entries.end() || l_It->second.state != Entry::State::Queued)
			continue;

		auto l_Job = std::make_shared<Job>();
		l_It->second.job = l_Job;
		l_It->second.state = Entry::State::Decoding;
		++l_Running;

		m_ActiveThreads->fetch_add(1);
		image::DecodeLimits l_Limits;
		l_Limits.maxDimension = m_MaxDimension;
		l_Limits.maxBytes = 768ull * 1024 * 1024;
		std::thread([l_Job, l_Active = m_ActiveThreads, l_Limits, l_Data = std::move(l_Item.second)]
		{
			std::string l_Error;
			l_Job->result = image::decode(l_Data, l_Limits, l_Error);
			l_Job->error = std::move(l_Error);
			l_Job->done.store(true, std::memory_order_release);
			l_Active->fetch_sub(1);
		}).detach();
	}
}

void ImageStore::update(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, const VkCommandBuffer p_Cmd)
{
	// The placeholder panel needs a texture bound even though it is never sampled
	if (!m_PlaceholderReady)
	{
		image::Decoded l_Pixel;
		l_Pixel.width = 1;
		l_Pixel.height = 1;
		l_Pixel.frames.push_back(std::vector<uint8_t>{ 0, 0, 0, 0 });
		l_Pixel.delaysMs.push_back(100);
		m_PlaceholderReady = upload(p_Context, p_Frames, p_Cmd, l_Pixel, m_Placeholder, m_PlaceholderSet);
	}

	for (gfx::Image& l_Image : m_Retired)
	{
		p_Frames.deferDestroy([l_Allocator = p_Context.allocator(), l_Device = p_Context.device(), l_Handle = l_Image.handle, l_View = l_Image.view, l_Allocation = l_Image.allocation]
		{
			vkDestroyImageView(l_Device, l_View, nullptr);
			vmaDestroyImage(l_Allocator, l_Handle, l_Allocation);
		});
		l_Image = gfx::Image{};
	}
	m_Retired.clear();

	startJobs();

	size_t l_Uploads = 0;
	for (auto& [l_Id, l_Entry] : m_Entries)
	{
		if (l_Entry.state != Entry::State::Decoding || !l_Entry.job->done.load(std::memory_order_acquire) || l_Uploads >= UPLOADS_PER_FRAME)
			continue;
		const std::shared_ptr<Job> l_Job = std::move(l_Entry.job);
		if (!l_Job->result)
		{
			l_Entry.state = Entry::State::Failed;
			l_Entry.error = l_Job->error;
			spdlog::warn("Picture {} could not be decoded: {}", l_Id, l_Job->error);
			continue;
		}
		const image::Decoded& l_Decoded = *l_Job->result;
		if (!upload(p_Context, p_Frames, p_Cmd, l_Decoded, l_Entry.image, l_Entry.set))
		{
			l_Entry.state = Entry::State::Failed;
			l_Entry.error = "The picture could not be uploaded.";
			continue;
		}
		++l_Uploads;
		l_Entry.width = l_Decoded.width;
		l_Entry.height = l_Decoded.height;
		l_Entry.frames = l_Entry.image.layers;
		l_Entry.delaysMs.assign(l_Decoded.delaysMs.begin(), l_Decoded.delaysMs.begin() + static_cast<std::ptrdiff_t>(l_Entry.frames));
		l_Entry.totalMs = 0;
		for (const uint32_t l_Delay : l_Entry.delaysMs)
			l_Entry.totalMs += l_Delay;
		l_Entry.state = Entry::State::Ready;
		if (l_Decoded.truncated)
			spdlog::warn("Picture {}: only the first {} frames fit in memory", l_Id, l_Entry.frames);
	}

	m_Running = 0;
	for (const auto& [l_Id, l_Entry] : m_Entries)
		m_Running += l_Entry.state == Entry::State::Decoding ? 1 : 0;
}

uint32_t ImageStore::frameFor(const Entry& p_Entry, const double p_Seconds)
{
	if (p_Entry.frames <= 1 || p_Entry.totalMs == 0)
		return 0;
	const double l_Ms = std::fmod(std::max(p_Seconds, 0.0) * 1000.0, static_cast<double>(p_Entry.totalMs));
	double l_Accumulated = 0.0;
	for (uint32_t i = 0; i < p_Entry.frames; ++i)
	{
		l_Accumulated += static_cast<double>(p_Entry.delaysMs[i]);
		if (l_Ms < l_Accumulated)
			return i;
	}
	return p_Entry.frames - 1;
}

ImageLookup ImageStore::lookup(const Document& p_Document, const ImageData& p_Image, const ImageClock& p_Clock)
{
	ImageLookup l_Result;
	l_Result.set = m_PlaceholderSet;
	auto l_It = m_Entries.find(p_Image.asset);
	if (l_It == m_Entries.end())
	{
		const ImageAsset* l_Asset = p_Document.findAsset(p_Image.asset);
		Entry l_Entry;
		if (l_Asset == nullptr)
		{
			l_Entry.state = Entry::State::Failed;
			l_Entry.error = "The picture is missing from the board.";
		}
		else
		{
			m_Queue.emplace_back(p_Image.asset, l_Asset->bytes);
		}
		l_It = m_Entries.emplace(p_Image.asset, std::move(l_Entry)).first;
	}
	const Entry& l_Entry = l_It->second;
	if (l_Entry.state != Entry::State::Ready)
		return l_Result;

	l_Result.ready = true;
	l_Result.set = l_Entry.set;
	l_Result.layer = static_cast<float>(p_Image.playing ? frameFor(l_Entry, p_Clock.seconds) : std::min(p_Image.frame, l_Entry.frames - 1));
	l_Result.animatedAndPlaying = l_Entry.frames > 1 && p_Image.playing && p_Clock.playing;
	return l_Result;
}

ImageInfo ImageStore::info(const AssetId p_Asset) const
{
	ImageInfo l_Info;
	const auto l_It = m_Entries.find(p_Asset);
	if (l_It == m_Entries.end())
		return l_Info;
	const Entry& l_Entry = l_It->second;
	l_Info.ready = l_Entry.state == Entry::State::Ready;
	l_Info.failed = l_Entry.state == Entry::State::Failed;
	l_Info.error = l_Entry.error;
	if (l_Info.ready)
	{
		l_Info.width = l_Entry.width;
		l_Info.height = l_Entry.height;
		l_Info.frames = l_Entry.frames;
		l_Info.durationMs = l_Entry.totalMs;
	}
	return l_Info;
}

uint32_t ImageStore::frameAt(const AssetId p_Asset, const ImageClock& p_Clock) const
{
	const auto l_It = m_Entries.find(p_Asset);
	if (l_It == m_Entries.end() || l_It->second.state != Entry::State::Ready)
		return 0;
	return frameFor(l_It->second, p_Clock.seconds);
}

void ImageStore::clear()
{
	for (auto& [l_Id, l_Entry] : m_Entries)
	{
		if (l_Entry.image.isValid())
			m_Retired.push_back(l_Entry.image);
	}
	m_Entries.clear();
	m_Queue.clear();
}

void ImageStore::bind(const VkCommandBuffer p_Cmd) const
{
	vkCmdBindPipeline(p_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
}

void ImageStore::draw(const VkCommandBuffer p_Cmd, const VkExtent2D p_Extent, const ImageDraw& p_Draw) const
{
	if (p_Draw.set == VK_NULL_HANDLE)
		return;
	const double l_Scale = std::max(p_Draw.localToScreen.uniformScale(), 1e-9);
	const PushConstants l_Push{
		.viewportSize = { static_cast<float>(p_Extent.width), static_cast<float>(p_Extent.height) },
		.inflate = static_cast<float>(1.0 / l_Scale),
		.layer = p_Draw.layer,
		.linear = {
			static_cast<float>(p_Draw.localToScreen.linear[0][0]),
			static_cast<float>(p_Draw.localToScreen.linear[0][1]),
			static_cast<float>(p_Draw.localToScreen.linear[1][0]),
			static_cast<float>(p_Draw.localToScreen.linear[1][1]),
		},
		.translation = { static_cast<float>(p_Draw.localToScreen.translation.x), static_cast<float>(p_Draw.localToScreen.translation.y) },
		.size = { p_Draw.size.x, p_Draw.size.y },
		.opacity = 1.f,
		.padding = { 0.f, 0.f, 0.f },
	};
	vkCmdBindDescriptorSets(p_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Layout, 0, 1, &p_Draw.set, 0, nullptr);
	vkCmdPushConstants(p_Cmd, m_Layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(l_Push), &l_Push);
	vkCmdDraw(p_Cmd, 6, 1, 0, 0);
}
} // namespace wb::render
