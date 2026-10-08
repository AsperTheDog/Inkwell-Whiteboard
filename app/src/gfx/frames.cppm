// Frames in flight, paced by one timeline semaphore.
//
// Usage per frame:
//   waitForSlot()            -- blocks until the GPU finished the frame that last used this slot; call it
//                               *before* polling input so the freshest input lands in the frame
//   begin() -> FrameTarget   -- acquires a swapchain image and opens the command buffer
//   ... record ...
//   end()                    -- submits and presents
//
// deferDestroy() queues a cleanup that runs once the GPU has finished every frame submitted so far and the one being recorded.
module;
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include <volk.h>

export module wb.gfx.frames;

import wb.gfx.context;
import wb.gfx.swapchain;

export namespace wb::gfx
{
inline constexpr uint32_t FRAMES_IN_FLIGHT = 2;

struct FrameTarget
{
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	uint32_t slot = 0;       // 0..FRAMES_IN_FLIGHT-1, for per-frame resources
	uint32_t imageIndex = 0; // swapchain image
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkExtent2D extent{};
	VkFormat format = VK_FORMAT_UNDEFINED;
};

enum class FrameStatus : uint8_t
{
	Ok,
	SwapchainOutOfDate, // recreate the swapchain, then try again
};

class FrameScheduler
{
public:
	void init(const GraphicsContext& p_Context);
	void destroy(const GraphicsContext& p_Context);

	void waitForSlot(const GraphicsContext& p_Context);
	[[nodiscard]] std::optional<FrameTarget> begin(const GraphicsContext& p_Context, const Swapchain& p_Swapchain, FrameStatus& p_Status);
	[[nodiscard]] FrameStatus end(const GraphicsContext& p_Context, const Swapchain& p_Swapchain, const FrameTarget& p_Target);

	// Brackets the GPU work to measure (call right after begin() / right before end())
	void writeStartTimestamp(VkCommandBuffer p_Cmd) const;
	void writeEndTimestamp(VkCommandBuffer p_Cmd) const;

	void deferDestroy(std::move_only_function<void()> p_Destroy);
	// Runs cleanups whose frames have completed
	void collectGarbage(const GraphicsContext& p_Context);

	// Slot of the frame being prepared (valid after waitForSlot())
	[[nodiscard]] uint32_t currentSlot() const { return m_Current; }
	[[nodiscard]] uint64_t submittedValue() const { return m_TimelineValue; }
	[[nodiscard]] uint64_t completedValue(const GraphicsContext& p_Context) const;
	[[nodiscard]] VkSemaphore timeline() const { return m_Timeline; }
	// GPU time of the most recently completed frame, in milliseconds (0 when unsupported)
	[[nodiscard]] double gpuFrameMs() const { return m_GpuFrameMs; }

private:
	struct Slot
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkSemaphore imageAcquired = VK_NULL_HANDLE;
		uint64_t timelineValue = 0; // value signaled when this slot's last submission finishes
		bool timestampsWritten = false;
	};

	struct PendingDestroy
	{
		uint64_t afterValue = 0;
		std::move_only_function<void()> destroy;
	};

	void readTimestamps(const GraphicsContext& p_Context, uint32_t p_Slot);

	std::array<Slot, FRAMES_IN_FLIGHT> m_Slots{};
	uint32_t m_Current = 0;
	VkSemaphore m_Timeline = VK_NULL_HANDLE;
	uint64_t m_TimelineValue = 0;
	std::vector<PendingDestroy> m_PendingDestroys;

	VkQueryPool m_Timestamps = VK_NULL_HANDLE; // 2 queries per slot
	float m_TimestampPeriodNs = 0.f;
	double m_GpuFrameMs = 0.0;
};
} // namespace wb::gfx
