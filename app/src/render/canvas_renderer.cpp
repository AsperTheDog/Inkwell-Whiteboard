module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>
#include "gfx/vk_check.hpp"

module wb.render.canvas_renderer;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.view.camera;
import wb.util.range_allocator;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.pipeline;
import wb.render.image_store;
import wb.render.video_store;
import wb.render.text_renderer;
import wb.text.system;

namespace wb::render
{
namespace
{
// GPU layouts (must match the Slang shaders)
struct GpuPoint
{
	float x;
	float y;
	float radius;
	float unused;
};
static_assert(sizeof(GpuPoint) == 16);

struct GpuStrokeInstance
{
	float linear[4]; // column-major 2x2
	float translation[2];
	float radiusScale;
	uint32_t flags;
	float color[4];
	uint32_t firstPoint;
	uint32_t pointCount;
	uint32_t padding[2];
};
static_assert(sizeof(GpuStrokeInstance) == 64);

struct StrokePushConstants
{
	float viewportSize[2];
	float minRadius;
	float aaWidth;
};
static_assert(sizeof(StrokePushConstants) == 16);

struct GridLevel
{
	float offset[2];
	float spacing;
	float alpha;
};

struct GridPushConstants
{
	GridLevel fine;
	GridLevel coarse;
	float color[4];
	float dotRadius;
	float padding[3];
};
static_assert(sizeof(GridPushConstants) == 64);

constexpr uint32_t STROKE_FLAG_LIVE = 1;
constexpr uint64_t INITIAL_POOL_POINTS = 1u << 16;
constexpr double GRID_BASE_SPACING = 24.0; // world units
constexpr double GRID_MIN_SPACING_PX = 14.0;

GpuStrokeInstance makeInstance(const Affine2& p_LocalToScreen, const Color p_Color, const uint32_t p_First, const uint32_t p_Count, const uint32_t p_Flags)
{
	return GpuStrokeInstance{
		.linear = {
			static_cast<float>(p_LocalToScreen.linear[0][0]),
			static_cast<float>(p_LocalToScreen.linear[0][1]),
			static_cast<float>(p_LocalToScreen.linear[1][0]),
			static_cast<float>(p_LocalToScreen.linear[1][1]),
		},
		.translation = { static_cast<float>(p_LocalToScreen.translation.x), static_cast<float>(p_LocalToScreen.translation.y) },
		.radiusScale = static_cast<float>(p_LocalToScreen.uniformScale()),
		.flags = p_Flags,
		.color = { p_Color.r, p_Color.g, p_Color.b, p_Color.a },
		.firstPoint = p_First,
		.pointCount = p_Count,
		.padding = { 0, 0 },
	};
}

uint32_t vertexCountFor(const uint32_t p_PointCount)
{
	return 6u * (std::max(p_PointCount, 2u) - 1u);
}

GridLevel makeGridLevel(const DVec2 p_OriginScreen, const double p_Spacing, const float p_Alpha)
{
	return GridLevel{
		.offset = { static_cast<float>(std::fmod(p_OriginScreen.x, p_Spacing)), static_cast<float>(std::fmod(p_OriginScreen.y, p_Spacing)) },
		.spacing = static_cast<float>(p_Spacing),
		.alpha = p_Alpha,
	};
}
} // namespace

void CanvasRenderer::init(const gfx::GraphicsContext& p_Context, const VkFormat p_ColorFormat)
{
	const VkDevice l_Device = p_Context.device();

	const VkDescriptorSetLayoutBinding l_Bindings[]{
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
	};
	const VkDescriptorSetLayoutCreateInfo l_LayoutInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 3,
		.pBindings = l_Bindings,
	};
	WB_VK_CHECK(vkCreateDescriptorSetLayout(l_Device, &l_LayoutInfo, nullptr, &m_SetLayout));

	const VkDescriptorPoolSize l_PoolSize{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 * gfx::FRAMES_IN_FLIGHT };
	const VkDescriptorPoolCreateInfo l_PoolInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = gfx::FRAMES_IN_FLIGHT,
		.poolSizeCount = 1,
		.pPoolSizes = &l_PoolSize,
	};
	WB_VK_CHECK(vkCreateDescriptorPool(l_Device, &l_PoolInfo, nullptr, &m_DescriptorPool));

	std::array<VkDescriptorSetLayout, gfx::FRAMES_IN_FLIGHT> l_Layouts{};
	l_Layouts.fill(m_SetLayout);
	std::array<VkDescriptorSet, gfx::FRAMES_IN_FLIGHT> l_Sets{};
	const VkDescriptorSetAllocateInfo l_AllocInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = m_DescriptorPool,
		.descriptorSetCount = gfx::FRAMES_IN_FLIGHT,
		.pSetLayouts = l_Layouts.data(),
	};
	WB_VK_CHECK(vkAllocateDescriptorSets(l_Device, &l_AllocInfo, l_Sets.data()));
	for (uint32_t i = 0; i < gfx::FRAMES_IN_FLIGHT; ++i)
		m_Frames[i].descriptorSet = l_Sets[i];

	createPipelines(p_Context, p_ColorFormat);
	m_Images.init(p_Context, p_ColorFormat);
	m_Videos.init(p_Context, m_Images);
	m_Text.init(p_Context, p_ColorFormat);

	m_MaxPoolPoints = p_Context.info().limits.maxStorageBufferRange / sizeof(GpuPoint);
	const uint64_t l_Initial = std::min(INITIAL_POOL_POINTS, m_MaxPoolPoints);
	m_PointPool = gfx::createBuffer(p_Context, l_Initial * sizeof(GpuPoint), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryKind::DeviceLocal, "stroke point pool");
	m_PoolAllocator.reset(l_Initial);
}

void CanvasRenderer::createPipelines(const gfx::GraphicsContext& p_Context, const VkFormat p_ColorFormat)
{
	const VkDevice l_Device = p_Context.device();
	const VkShaderModule l_StrokeModule = gfx::loadShaderModule(p_Context, "stroke");
	const VkShaderModule l_GridModule = gfx::loadShaderModule(p_Context, "grid");

	m_StrokeLayout = gfx::createPipelineLayout(p_Context, std::span(&m_SetLayout, 1), sizeof(StrokePushConstants), "stroke layout");
	m_GridLayout = gfx::createPipelineLayout(p_Context, {}, sizeof(GridPushConstants), "grid layout");

	m_StrokePipeline = gfx::createGraphicsPipeline(p_Context, { .module = l_StrokeModule, .layout = m_StrokeLayout, .colorFormat = p_ColorFormat, .name = "stroke pipeline" });
	m_GridPipeline = gfx::createGraphicsPipeline(p_Context, { .module = l_GridModule, .layout = m_GridLayout, .colorFormat = p_ColorFormat, .name = "grid pipeline" });

	vkDestroyShaderModule(l_Device, l_StrokeModule, nullptr);
	vkDestroyShaderModule(l_Device, l_GridModule, nullptr);
}

void CanvasRenderer::destroy(const gfx::GraphicsContext& p_Context)
{
	detach();
	m_Videos.destroy(p_Context);
	m_Images.destroy(p_Context);
	m_Text.destroy(p_Context);
	const VkDevice l_Device = p_Context.device();
	for (FrameResources& l_Frame : m_Frames)
	{
		gfx::destroyBuffer(p_Context, l_Frame.instances);
		gfx::destroyBuffer(p_Context, l_Frame.livePoints);
	}
	gfx::destroyBuffer(p_Context, m_PointPool);
	vkDestroyPipeline(l_Device, m_StrokePipeline, nullptr);
	vkDestroyPipeline(l_Device, m_GridPipeline, nullptr);
	vkDestroyPipelineLayout(l_Device, m_StrokeLayout, nullptr);
	vkDestroyPipelineLayout(l_Device, m_GridLayout, nullptr);
	vkDestroyDescriptorPool(l_Device, m_DescriptorPool, nullptr);
	vkDestroyDescriptorSetLayout(l_Device, m_SetLayout, nullptr);
	m_StrokePipeline = m_GridPipeline = VK_NULL_HANDLE;
	m_StrokeLayout = m_GridLayout = VK_NULL_HANDLE;
	m_DescriptorPool = VK_NULL_HANDLE;
	m_SetLayout = VK_NULL_HANDLE;
}

void CanvasRenderer::attach(Document& p_Document)
{
	detach();
	m_Document = &p_Document;
	m_Document->addListener(this);
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
		onObjectAdded(*l_Object);
}

void CanvasRenderer::detach()
{
	if (m_Document == nullptr)
		return;
	m_Document->removeListener(this);
	m_Document = nullptr;
	onDocumentCleared();
}

// ------------------------------------------------------------------------------------------------ document sync

void CanvasRenderer::onObjectAdded(const Object& p_Object)
{
	if (p_Object.stroke() != nullptr)
		m_PendingUploads.insert(p_Object.id);
}

void CanvasRenderer::onObjectChanged(const Object& p_Object, const uint32_t p_Changes)
{
	// Transform and style live in the per-frame instance data; only geometry needs an upload
	if (p_Object.stroke() != nullptr && (p_Changes & ObjectChange::Geometry) != 0)
		m_PendingUploads.insert(p_Object.id);
}

void CanvasRenderer::onObjectRemoved(const Object& p_Object)
{
	m_PendingUploads.erase(p_Object.id);
	if (p_Object.video() != nullptr)
		m_Videos.forget(p_Object.id);
	const auto l_It = m_Strokes.find(p_Object.id);
	if (l_It == m_Strokes.end())
		return;
	releaseRange(l_It->second.offset, l_It->second.count);
	m_Strokes.erase(l_It);
}

void CanvasRenderer::onDocumentCleared()
{
	m_Images.clear();
	m_Videos.clear();
	for (const auto& [l_Id, l_Gpu] : m_Strokes)
		releaseRange(l_Gpu.offset, l_Gpu.count);
	m_Strokes.clear();
	m_PendingUploads.clear();
}

void CanvasRenderer::releaseRange(const uint64_t p_Offset, const uint64_t p_Count)
{
	// Frames already submitted may still read the range
	m_PendingFrees.push_back(PendingFree{ .afterTimeline = m_CurrentTimeline, .offset = p_Offset, .count = p_Count });
}

void CanvasRenderer::processFrees(const uint64_t p_CompletedTimeline)
{
	std::erase_if(m_PendingFrees, [&](const PendingFree& p_Free)
	{
		if (p_Free.afterTimeline > p_CompletedTimeline)
			return false;
		m_PoolAllocator.free(p_Free.offset, p_Free.count);
		return true;
	});
}

// ------------------------------------------------------------------------------------------------ uploads

void CanvasRenderer::ensureCapacity(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::Buffer& p_Buffer, const VkDeviceSize p_Bytes, const char* p_Name)
{
	if (p_Buffer.isValid() && p_Buffer.size >= p_Bytes)
		return;
	gfx::destroyBufferDeferred(p_Context, p_Frames, p_Buffer);
	const VkDeviceSize l_Size = std::max<VkDeviceSize>(p_Bytes + p_Bytes / 2, 64 * 1024);
	p_Buffer = gfx::createBuffer(p_Context, l_Size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryKind::Upload, p_Name);
}

void CanvasRenderer::ensurePoolCapacity(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, const VkCommandBuffer p_Cmd, const uint64_t p_ExtraPoints)
{
	const uint64_t l_OldCapacity = m_PoolAllocator.capacity();
	uint64_t l_NewCapacity = l_OldCapacity;
	while (l_NewCapacity < m_PoolAllocator.used() + p_ExtraPoints + l_OldCapacity / 4)
		l_NewCapacity *= 2;
	l_NewCapacity = std::min(l_NewCapacity, m_MaxPoolPoints);
	if (l_NewCapacity <= l_OldCapacity)
		throw std::runtime_error("Stroke point pool exhausted (device storage buffer limit reached)");

	gfx::Buffer l_NewPool = gfx::createBuffer(p_Context, l_NewCapacity * sizeof(GpuPoint), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryKind::DeviceLocal, "stroke point pool");
	const VkBufferCopy l_Copy{ .srcOffset = 0, .dstOffset = 0, .size = l_OldCapacity * sizeof(GpuPoint) };
	vkCmdCopyBuffer(p_Cmd, m_PointPool.handle, l_NewPool.handle, 1, &l_Copy);
	// The uploads that follow may overwrite reused ranges of the copied data
	gfx::memoryBarrier(p_Cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

	gfx::destroyBufferDeferred(p_Context, p_Frames, m_PointPool);
	m_PointPool = l_NewPool;
	m_PoolAllocator.grow(l_NewCapacity);
}

void CanvasRenderer::uploadPending(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::StagingBelt& p_Staging, const VkCommandBuffer p_Cmd)
{
	m_Stats.uploadsThisFrame = 0;
	if (m_PendingUploads.empty() || m_Document == nullptr)
		return;

	bool l_Copied = false;
	for (const ObjectId l_Id : m_PendingUploads)
	{
		const Object* l_Object = m_Document->find(l_Id);
		const StrokeData* l_Stroke = l_Object != nullptr ? l_Object->stroke() : nullptr;
		if (l_Stroke == nullptr || l_Stroke->points.empty())
			continue;

		// Replace any previous version of this stroke
		if (const auto l_It = m_Strokes.find(l_Id); l_It != m_Strokes.end())
		{
			releaseRange(l_It->second.offset, l_It->second.count);
			m_Strokes.erase(l_It);
		}

		const uint64_t l_Count = l_Stroke->points.size();
		std::optional<uint64_t> l_Offset = m_PoolAllocator.allocate(l_Count);
		if (!l_Offset)
		{
			ensurePoolCapacity(p_Context, p_Frames, p_Cmd, l_Count);
			l_Offset = m_PoolAllocator.allocate(l_Count);
			if (!l_Offset)
				throw std::runtime_error("Stroke point pool allocation failed after growing");
		}

		const gfx::StagingAllocation l_Staging = p_Staging.allocate(p_Context, l_Count * sizeof(GpuPoint));
		GpuPoint* l_Dst = reinterpret_cast<GpuPoint*>(l_Staging.data.data());
		for (size_t i = 0; i < l_Count; ++i)
		{
			const StrokePoint& l_Point = l_Stroke->points[i];
			l_Dst[i] = GpuPoint{ .x = l_Point.position.x, .y = l_Point.position.y, .radius = l_Point.radius, .unused = 0.f };
		}

		const VkBufferCopy l_Copy{ .srcOffset = l_Staging.offset, .dstOffset = *l_Offset * sizeof(GpuPoint), .size = l_Count * sizeof(GpuPoint) };
		vkCmdCopyBuffer(p_Cmd, l_Staging.buffer, m_PointPool.handle, 1, &l_Copy);
		m_Strokes[l_Id] = GpuStroke{ .offset = *l_Offset, .count = static_cast<uint32_t>(l_Count) };
		l_Copied = true;
		++m_Stats.uploadsThisFrame;
	}
	m_PendingUploads.clear();

	if (l_Copied)
		gfx::memoryBarrier(p_Cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

// ------------------------------------------------------------------------------------------------ per frame

void CanvasRenderer::prepare(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::StagingBelt& p_Staging, const VkCommandBuffer p_Cmd, const uint32_t p_Slot, const Camera& p_Camera, const LiveStrokeView* p_Live, const ImageClock& p_Clock)
{
	m_Slot = p_Slot;
	m_Images.update(p_Context, p_Frames, p_Cmd);
	m_Videos.update(p_Context, p_Frames, p_Cmd);
	m_ImageDraws.clear();
	m_VisibleVideos.clear();
	m_Text.begin();
	m_TextGlyphs.clear();
	m_TextIncomplete = false;
	m_Animating = false;
	m_CurrentTimeline = p_Frames.submittedValue() + 1;
	processFrees(p_Frames.completedValue(p_Context));
	uploadPending(p_Context, p_Frames, p_Staging, p_Cmd);

	// Visible strokes, back to front
	FrameResources& l_Frame = m_Frames[p_Slot];
	m_Draws.clear();
	std::vector<GpuStrokeInstance> l_Instances;
	l_Instances.reserve(m_Strokes.size() + 1);

	const Affine2 l_WorldToScreen = p_Camera.worldToScreenTransform();
	const Rect l_Visible = p_Camera.visibleWorldRect();
	m_Stats.drawnSegments = 0;

	if (m_Document != nullptr)
	{
		for (const std::unique_ptr<Object>& l_Object : m_Document->objects())
		{
			if (const ImageData* l_Image = l_Object->image())
			{
				if (!l_Object->worldBounds().inflated(2.0 / p_Camera.pixelsPerUnit()).intersects(l_Visible))
					continue;
				const ImageLookup l_Lookup = m_Images.lookup(*m_Document, *l_Image, p_Clock);
				m_Draws.push_back(Draw{ .image = static_cast<uint32_t>(m_ImageDraws.size()) });
				m_ImageDraws.push_back(ImageDraw{ .set = l_Lookup.set, .layer = l_Lookup.layer, .localToScreen = l_WorldToScreen * l_Object->transform, .size = l_Image->size });
				m_Animating = m_Animating || l_Lookup.animatedAndPlaying;
				continue;
			}
			if (const VideoData* l_Video = l_Object->video())
			{
				if (!l_Object->worldBounds().inflated(2.0 / p_Camera.pixelsPerUnit()).intersects(l_Visible))
					continue;
				const VideoLookup l_Lookup = m_Videos.lookup(*m_Document, l_Object->id, *l_Video);
				m_VisibleVideos.push_back(l_Object->id);
				m_Draws.push_back(Draw{ .image = static_cast<uint32_t>(m_ImageDraws.size()) });
				m_ImageDraws.push_back(ImageDraw{ .set = l_Lookup.set, .layer = l_Lookup.layer, .localToScreen = l_WorldToScreen * l_Object->transform, .size = l_Video->size });
				continue;
			}
			if (const TextData* l_Text = l_Object->text())
			{
				if (m_TextSystem == nullptr || !l_Object->worldBounds().inflated(2.0 / p_Camera.pixelsPerUnit()).intersects(l_Visible))
					continue;
				const size_t l_FirstGlyph = m_TextGlyphs.size();
				if (!m_TextSystem->buildGlyphs(*l_Text, m_TextGlyphs))
					m_TextIncomplete = true;
				const uint32_t l_Count = static_cast<uint32_t>(m_TextGlyphs.size() - l_FirstGlyph);
				if (l_Count > 0)
					m_Draws.push_back(Draw{ .text = m_Text.add(static_cast<uint32_t>(l_FirstGlyph), l_Count, l_WorldToScreen * l_Object->transform, l_Text->color) });
				continue;
			}
			const StrokeData* l_Stroke = l_Object->stroke();
			if (l_Stroke == nullptr)
				continue;
			const auto l_It = m_Strokes.find(l_Object->id);
			if (l_It == m_Strokes.end())
				continue;
			// Inflate by a pixel so minimum-width strokes at the edge are not culled
			if (!l_Object->worldBounds().inflated(2.0 / p_Camera.pixelsPerUnit()).intersects(l_Visible))
				continue;

			const GpuStroke& l_Gpu = l_It->second;
			m_Draws.push_back(Draw{ .instance = static_cast<uint32_t>(l_Instances.size()), .vertexCount = vertexCountFor(l_Gpu.count) });
			l_Instances.push_back(makeInstance(l_WorldToScreen * l_Object->transform, l_Stroke->style.color, static_cast<uint32_t>(l_Gpu.offset), l_Gpu.count, 0));
			m_Stats.drawnSegments += std::max(l_Gpu.count, 2u) - 1u;
		}
	}

	// The stroke being drawn goes on top
	uint64_t l_LiveCount = 0;
	if (p_Live != nullptr)
		l_LiveCount = p_Live->committed.size() + p_Live->tail.size();
	ensureCapacity(p_Context, p_Frames, l_Frame.livePoints, std::max<uint64_t>(l_LiveCount, 1) * sizeof(GpuPoint), "live stroke points");
	if (l_LiveCount > 0)
	{
		GpuPoint* l_Dst = reinterpret_cast<GpuPoint*>(l_Frame.livePoints.mapped);
		size_t l_Index = 0;
		for (const std::span<const StrokePoint> l_Part : { p_Live->committed, p_Live->tail })
		{
			for (const StrokePoint& l_Point : l_Part)
				l_Dst[l_Index++] = GpuPoint{ .x = l_Point.position.x, .y = l_Point.position.y, .radius = l_Point.radius, .unused = 0.f };
		}
		m_Draws.push_back(Draw{ .instance = static_cast<uint32_t>(l_Instances.size()), .vertexCount = vertexCountFor(static_cast<uint32_t>(l_LiveCount)) });
		l_Instances.push_back(makeInstance(l_WorldToScreen * Affine2::translate(p_Live->origin), p_Live->color, 0, static_cast<uint32_t>(l_LiveCount), STROKE_FLAG_LIVE));
	}

	if (m_TextSystem != nullptr)
		m_Text.upload(p_Context, p_Frames, p_Staging, p_Cmd, p_Slot, m_TextSystem->atlas(), m_TextGlyphs);

	ensureCapacity(p_Context, p_Frames, l_Frame.instances, std::max<size_t>(l_Instances.size(), 1) * sizeof(GpuStrokeInstance), "stroke instances");
	if (!l_Instances.empty())
		std::memcpy(l_Frame.instances.mapped, l_Instances.data(), l_Instances.size() * sizeof(GpuStrokeInstance));

	// The slot's previous frame has completed, so its set can be rewritten
	const VkDescriptorBufferInfo l_BufferInfos[]{
		{ .buffer = m_PointPool.handle, .offset = 0, .range = VK_WHOLE_SIZE },
		{ .buffer = l_Frame.livePoints.handle, .offset = 0, .range = VK_WHOLE_SIZE },
		{ .buffer = l_Frame.instances.handle, .offset = 0, .range = VK_WHOLE_SIZE },
	};
	std::array<VkWriteDescriptorSet, 3> l_Writes{};
	for (uint32_t i = 0; i < 3; ++i)
	{
		l_Writes[i] = VkWriteDescriptorSet{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = l_Frame.descriptorSet,
			.dstBinding = i,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &l_BufferInfos[i],
		};
	}
	vkUpdateDescriptorSets(p_Context.device(), static_cast<uint32_t>(l_Writes.size()), l_Writes.data(), 0, nullptr);

	m_Stats.gpuStrokes = static_cast<uint32_t>(m_Strokes.size());
	m_Stats.visibleStrokes = static_cast<uint32_t>(m_Draws.size());
	m_Stats.poolPointsUsed = m_PoolAllocator.used();
	m_Stats.poolPointsCapacity = m_PoolAllocator.capacity();
}

void CanvasRenderer::record(const VkCommandBuffer p_Cmd, const Camera& p_Camera, const GridStyle& p_Grid) const
{
	const DVec2 l_Viewport = p_Camera.viewport();
	gfx::setViewportAndScissor(p_Cmd, VkExtent2D{ static_cast<uint32_t>(l_Viewport.x), static_cast<uint32_t>(l_Viewport.y) });

	if (p_Grid.enabled)
	{
		const gfx::DebugLabel l_Label(p_Cmd, "grid");
		const double l_PixelScale = p_Camera.pixelScale();
		const double l_MinSpacing = GRID_MIN_SPACING_PX * l_PixelScale;
		const double l_BaseSpacing = GRID_BASE_SPACING * p_Camera.pixelsPerUnit();
		// Pick the level whose spacing lies in [min, 5 * min)
		const double l_Level = std::floor(std::log(l_BaseSpacing / l_MinSpacing) / std::log(5.0));
		const double l_Fine = l_BaseSpacing / std::pow(5.0, l_Level);
		const double l_T = std::clamp((l_Fine - l_MinSpacing) / (1.5 * l_MinSpacing), 0.0, 1.0);
		const float l_FineAlpha = static_cast<float>(l_T * l_T * (3.0 - 2.0 * l_T));
		const DVec2 l_OriginScreen = p_Camera.worldToScreen(DVec2{ 0.0 });

		const GridPushConstants l_Push{
			.fine = makeGridLevel(l_OriginScreen, l_Fine, l_FineAlpha),
			.coarse = makeGridLevel(l_OriginScreen, l_Fine * 5.0, 1.f),
			.color = { p_Grid.dotColor.r, p_Grid.dotColor.g, p_Grid.dotColor.b, p_Grid.dotColor.a },
			.dotRadius = static_cast<float>(1.1 * l_PixelScale),
			.padding = { 0.f, 0.f, 0.f },
		};
		vkCmdBindPipeline(p_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GridPipeline);
		vkCmdPushConstants(p_Cmd, m_GridLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(l_Push), &l_Push);
		vkCmdDraw(p_Cmd, 3, 1, 0, 0);
	}

	if (m_Draws.empty())
		return;

	const gfx::DebugLabel l_Label(p_Cmd, "board");
	const StrokePushConstants l_Push{
		.viewportSize = { static_cast<float>(l_Viewport.x), static_cast<float>(l_Viewport.y) },
		.minRadius = 0.5f,
		.aaWidth = 1.f,
	};
	const VkExtent2D l_Extent{ static_cast<uint32_t>(l_Viewport.x), static_cast<uint32_t>(l_Viewport.y) };
	bool l_StrokesBound = false;
	bool l_ImagesBound = false;
	bool l_TextBound = false;
	for (const Draw& l_Draw : m_Draws)
	{
		if (l_Draw.text != NO_IMAGE)
		{
			if (!l_TextBound)
			{
				m_Text.bind(p_Cmd);
				l_TextBound = true;
				l_StrokesBound = false;
				l_ImagesBound = false;
			}
			m_Text.draw(p_Cmd, l_Extent, l_Draw.text);
			continue;
		}
		if (l_Draw.image != NO_IMAGE)
		{
			if (!l_ImagesBound)
			{
				m_Images.bind(p_Cmd);
				l_ImagesBound = true;
				l_StrokesBound = false;
				l_TextBound = false;
			}
			m_Images.draw(p_Cmd, l_Extent, m_ImageDraws[l_Draw.image]);
			continue;
		}
		if (!l_StrokesBound)
		{
			vkCmdBindPipeline(p_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StrokePipeline);
			vkCmdBindDescriptorSets(p_Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StrokeLayout, 0, 1, &m_Frames[m_Slot].descriptorSet, 0, nullptr);
			vkCmdPushConstants(p_Cmd, m_StrokeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(l_Push), &l_Push);
			l_StrokesBound = true;
			l_ImagesBound = false;
			l_TextBound = false;
		}
		vkCmdDraw(p_Cmd, l_Draw.vertexCount, 1, 0, l_Draw.instance);
	}
}
} // namespace wb::render
