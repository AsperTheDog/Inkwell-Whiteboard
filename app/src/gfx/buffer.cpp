module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "gfx/vk_check.hpp"
#include "gfx/vma.hpp"

module wb.gfx.buffer;

import wb.gfx.context;
import wb.gfx.frames;

namespace wb::gfx
{
Buffer createBuffer(const GraphicsContext& p_Context, const VkDeviceSize p_Size, const VkBufferUsageFlags p_Usage, const MemoryKind p_Memory, const char* p_Name)
{
	const VkBufferCreateInfo l_BufferInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = std::max<VkDeviceSize>(p_Size, 16),
		.usage = p_Usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	VmaAllocationCreateInfo l_AllocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };
	if (p_Memory == MemoryKind::Upload || p_Memory == MemoryKind::Readback)
	{
		l_AllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		l_AllocInfo.flags |= p_Memory == MemoryKind::Upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		// Coherent memory (always available) means no explicit flushes/invalidations
		l_AllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	}
	else
		l_AllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	Buffer l_Buffer{};
	VmaAllocationInfo l_Info{};
	WB_VK_CHECK(vmaCreateBuffer(p_Context.allocator(), &l_BufferInfo, &l_AllocInfo, &l_Buffer.handle, &l_Buffer.allocation, &l_Info));
	l_Buffer.size = l_BufferInfo.size;
	l_Buffer.mapped = static_cast<std::byte*>(l_Info.pMappedData);
	p_Context.setName(l_Buffer.handle, VK_OBJECT_TYPE_BUFFER, p_Name);
	return l_Buffer;
}

void destroyBuffer(const GraphicsContext& p_Context, Buffer& p_Buffer)
{
	if (p_Buffer.handle != VK_NULL_HANDLE)
		vmaDestroyBuffer(p_Context.allocator(), p_Buffer.handle, p_Buffer.allocation);
	p_Buffer = Buffer{};
}

void destroyBufferDeferred(const GraphicsContext& p_Context, FrameScheduler& p_Frames, Buffer& p_Buffer)
{
	if (!p_Buffer.isValid())
		return;
	p_Frames.deferDestroy([l_Allocator = p_Context.allocator(), l_Handle = p_Buffer.handle, l_Allocation = p_Buffer.allocation]
	{
		vmaDestroyBuffer(l_Allocator, l_Handle, l_Allocation);
	});
	p_Buffer = Buffer{};
}

void StagingBelt::init(const GraphicsContext&, const VkDeviceSize p_ChunkSize)
{
	m_ChunkSize = p_ChunkSize;
}

void StagingBelt::destroy(const GraphicsContext& p_Context)
{
	for (std::vector<Chunk>& l_Chunks : m_Chunks)
	{
		for (Chunk& l_Chunk : l_Chunks)
			destroyBuffer(p_Context, l_Chunk.buffer);
		l_Chunks.clear();
	}
}

void StagingBelt::beginFrame(const uint32_t p_Slot)
{
	m_Slot = p_Slot;
	for (Chunk& l_Chunk : m_Chunks[m_Slot])
		l_Chunk.used = 0;
}

StagingAllocation StagingBelt::allocate(const GraphicsContext& p_Context, const VkDeviceSize p_Size, const VkDeviceSize p_Alignment)
{
	std::vector<Chunk>& l_Chunks = m_Chunks[m_Slot];
	for (Chunk& l_Chunk : l_Chunks)
	{
		const VkDeviceSize l_Offset = (l_Chunk.used + p_Alignment - 1) / p_Alignment * p_Alignment;
		if (l_Offset + p_Size <= l_Chunk.buffer.size)
		{
			l_Chunk.used = l_Offset + p_Size;
			return StagingAllocation{
				.data = std::span<std::byte>(l_Chunk.buffer.mapped + l_Offset, static_cast<size_t>(p_Size)),
				.buffer = l_Chunk.buffer.handle,
				.offset = l_Offset,
			};
		}
	}

	Chunk l_Chunk{ .buffer = createBuffer(p_Context, std::max(m_ChunkSize, p_Size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryKind::Upload, "staging chunk"), .used = p_Size };
	const StagingAllocation l_Result{
		.data = std::span<std::byte>(l_Chunk.buffer.mapped, static_cast<size_t>(p_Size)),
		.buffer = l_Chunk.buffer.handle,
		.offset = 0,
	};
	l_Chunks.push_back(l_Chunk);
	return l_Result;
}

VkDeviceSize StagingBelt::allocatedBytes() const
{
	VkDeviceSize l_Total = 0;
	for (const std::vector<Chunk>& l_Chunks : m_Chunks)
	{
		for (const Chunk& l_Chunk : l_Chunks)
			l_Total += l_Chunk.buffer.size;
	}
	return l_Total;
}
} // namespace wb::gfx
