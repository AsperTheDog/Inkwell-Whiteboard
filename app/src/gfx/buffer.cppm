// VMA-backed buffers and a per-frame staging belt for uploads.
module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "gfx/vma.hpp"

export module wb.gfx.buffer;

import wb.gfx.context;
import wb.gfx.frames;

export namespace wb::gfx
{
enum class MemoryKind : uint8_t
{
	DeviceLocal, // GPU-only; filled through transfers
	Upload,      // host-visible, persistently mapped, written sequentially by the CPU
	Readback,    // host-visible, persistently mapped, read by the CPU (GPU -> CPU copies)
};

struct Buffer
{
	VkBuffer handle = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	std::byte* mapped = nullptr; // Upload buffers only

	[[nodiscard]] bool isValid() const { return handle != VK_NULL_HANDLE; }
};

[[nodiscard]] Buffer createBuffer(const GraphicsContext& p_Context, VkDeviceSize p_Size, VkBufferUsageFlags p_Usage, MemoryKind p_Memory, const char* p_Name);
void destroyBuffer(const GraphicsContext& p_Context, Buffer& p_Buffer);
// Destroys the buffer once the GPU is done with the frames submitted so far
void destroyBufferDeferred(const GraphicsContext& p_Context, FrameScheduler& p_Frames, Buffer& p_Buffer);

struct StagingAllocation
{
	std::span<std::byte> data;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
};

// Linear allocator for upload data, one set of chunks per frame in flight. Memory handed out in a frame stays
// valid until that frame's slot comes around again.
class StagingBelt
{
public:
	void init(const GraphicsContext& p_Context, VkDeviceSize p_ChunkSize);
	void destroy(const GraphicsContext& p_Context);

	// Call after FrameScheduler::waitForSlot() for p_Slot
	void beginFrame(uint32_t p_Slot);
	[[nodiscard]] StagingAllocation allocate(const GraphicsContext& p_Context, VkDeviceSize p_Size, VkDeviceSize p_Alignment = 16);

	[[nodiscard]] VkDeviceSize allocatedBytes() const;

private:
	struct Chunk
	{
		Buffer buffer;
		VkDeviceSize used = 0;
	};

	VkDeviceSize m_ChunkSize = 0;
	uint32_t m_Slot = 0;
	std::array<std::vector<Chunk>, FRAMES_IN_FLIGHT> m_Chunks{};
};
} // namespace wb::gfx
