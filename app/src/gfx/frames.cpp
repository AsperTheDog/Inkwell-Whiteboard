module;
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "gfx/vk_check.hpp"

module wb.gfx.frames;

import wb.gfx.context;
import wb.gfx.swapchain;

namespace wb::gfx
{
void FrameScheduler::init(const GraphicsContext& p_Context)
{
	const VkDevice l_Device = p_Context.device();

	VkSemaphoreTypeCreateInfo l_TypeInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		.initialValue = 0,
	};
	const VkSemaphoreCreateInfo l_TimelineInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &l_TypeInfo };
	WB_VK_CHECK(vkCreateSemaphore(l_Device, &l_TimelineInfo, nullptr, &m_Timeline));
	p_Context.setName(m_Timeline, VK_OBJECT_TYPE_SEMAPHORE, "frame timeline");

	for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
	{
		Slot& l_Slot = m_Slots[i];
		const VkCommandPoolCreateInfo l_PoolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			.queueFamilyIndex = p_Context.queueFamily(),
		};
		WB_VK_CHECK(vkCreateCommandPool(l_Device, &l_PoolInfo, nullptr, &l_Slot.pool));

		const VkCommandBufferAllocateInfo l_AllocInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = l_Slot.pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		WB_VK_CHECK(vkAllocateCommandBuffers(l_Device, &l_AllocInfo, &l_Slot.cmd));

		constexpr VkSemaphoreCreateInfo l_SemaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		WB_VK_CHECK(vkCreateSemaphore(l_Device, &l_SemaphoreInfo, nullptr, &l_Slot.imageAcquired));

		const std::string l_Suffix = " #" + std::to_string(i);
		p_Context.setName(l_Slot.cmd, VK_OBJECT_TYPE_COMMAND_BUFFER, ("frame cmd" + l_Suffix).c_str());
		p_Context.setName(l_Slot.imageAcquired, VK_OBJECT_TYPE_SEMAPHORE, ("image acquired" + l_Suffix).c_str());
	}

	if (p_Context.info().timestamps)
	{
		const VkQueryPoolCreateInfo l_QueryInfo{
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP,
			.queryCount = FRAMES_IN_FLIGHT * 2,
		};
		WB_VK_CHECK(vkCreateQueryPool(l_Device, &l_QueryInfo, nullptr, &m_Timestamps));
		m_TimestampPeriodNs = p_Context.info().timestampPeriodNs;
	}
}

void FrameScheduler::destroy(const GraphicsContext& p_Context)
{
	const VkDevice l_Device = p_Context.device();
	vkDeviceWaitIdle(l_Device);

	for (PendingDestroy& l_Pending : m_PendingDestroys)
		l_Pending.destroy();
	m_PendingDestroys.clear();

	for (Slot& l_Slot : m_Slots)
	{
		vkDestroySemaphore(l_Device, l_Slot.imageAcquired, nullptr);
		vkDestroyCommandPool(l_Device, l_Slot.pool, nullptr);
		l_Slot = Slot{};
	}
	if (m_Timestamps != VK_NULL_HANDLE)
	{
		vkDestroyQueryPool(l_Device, m_Timestamps, nullptr);
		m_Timestamps = VK_NULL_HANDLE;
	}
	vkDestroySemaphore(l_Device, m_Timeline, nullptr);
	m_Timeline = VK_NULL_HANDLE;
}

uint64_t FrameScheduler::completedValue(const GraphicsContext& p_Context) const
{
	uint64_t l_Value = 0;
	WB_VK_CHECK(vkGetSemaphoreCounterValue(p_Context.device(), m_Timeline, &l_Value));
	return l_Value;
}

void FrameScheduler::waitForSlot(const GraphicsContext& p_Context)
{
	Slot& l_Slot = m_Slots[m_Current];
	if (l_Slot.timelineValue == 0)
		return;

	const VkSemaphoreWaitInfo l_WaitInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &m_Timeline,
		.pValues = &l_Slot.timelineValue,
	};
	WB_VK_CHECK(vkWaitSemaphores(p_Context.device(), &l_WaitInfo, UINT64_MAX));

	if (l_Slot.timestampsWritten)
	{
		readTimestamps(p_Context, m_Current);
		l_Slot.timestampsWritten = false;
	}
}

void FrameScheduler::readTimestamps(const GraphicsContext& p_Context, const uint32_t p_Slot)
{
	std::array<uint64_t, 2> l_Values{};
	const VkResult l_Result = vkGetQueryPoolResults(p_Context.device(), m_Timestamps, p_Slot * 2, 2, sizeof(l_Values), l_Values.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
	if (l_Result == VK_SUCCESS && l_Values[1] >= l_Values[0])
		m_GpuFrameMs = static_cast<double>(l_Values[1] - l_Values[0]) * static_cast<double>(m_TimestampPeriodNs) * 1e-6;
}

std::optional<FrameTarget> FrameScheduler::begin(const GraphicsContext& p_Context, const Swapchain& p_Swapchain, FrameStatus& p_Status)
{
	p_Status = FrameStatus::Ok;
	Slot& l_Slot = m_Slots[m_Current];
	const VkDevice l_Device = p_Context.device();

	uint32_t l_ImageIndex = 0;
	const VkResult l_Acquire = vkAcquireNextImageKHR(l_Device, p_Swapchain.handle(), UINT64_MAX, l_Slot.imageAcquired, VK_NULL_HANDLE, &l_ImageIndex);
	if (l_Acquire == VK_ERROR_OUT_OF_DATE_KHR)
	{
		p_Status = FrameStatus::SwapchainOutOfDate;
		return std::nullopt;
	}
	if (l_Acquire != VK_SUBOPTIMAL_KHR)
		WB_VK_CHECK(l_Acquire);

	WB_VK_CHECK(vkResetCommandPool(l_Device, l_Slot.pool, 0));
	constexpr VkCommandBufferBeginInfo l_BeginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	WB_VK_CHECK(vkBeginCommandBuffer(l_Slot.cmd, &l_BeginInfo));

	return FrameTarget{
		.cmd = l_Slot.cmd,
		.slot = m_Current,
		.imageIndex = l_ImageIndex,
		.image = p_Swapchain.image(l_ImageIndex),
		.view = p_Swapchain.view(l_ImageIndex),
		.extent = p_Swapchain.extent(),
		.format = p_Swapchain.format(),
	};
}

void FrameScheduler::writeStartTimestamp(const VkCommandBuffer p_Cmd) const
{
	if (m_Timestamps == VK_NULL_HANDLE)
		return;
	vkCmdResetQueryPool(p_Cmd, m_Timestamps, m_Current * 2, 2);
	vkCmdWriteTimestamp2(p_Cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_Timestamps, m_Current * 2);
}

void FrameScheduler::writeEndTimestamp(const VkCommandBuffer p_Cmd) const
{
	if (m_Timestamps == VK_NULL_HANDLE)
		return;
	vkCmdWriteTimestamp2(p_Cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_Timestamps, m_Current * 2 + 1);
}

FrameStatus FrameScheduler::end(const GraphicsContext& p_Context, const Swapchain& p_Swapchain, const FrameTarget& p_Target)
{
	Slot& l_Slot = m_Slots[m_Current];
	WB_VK_CHECK(vkEndCommandBuffer(p_Target.cmd));

	++m_TimelineValue;
	l_Slot.timelineValue = m_TimelineValue;
	l_Slot.timestampsWritten = m_Timestamps != VK_NULL_HANDLE;

	const VkSemaphore l_RenderFinished = p_Swapchain.renderFinished(p_Target.imageIndex);
	const VkSemaphoreSubmitInfo l_Wait{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = l_Slot.imageAcquired,
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	};
	const VkSemaphoreSubmitInfo l_Signals[]{
		{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = l_RenderFinished, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
		{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = m_Timeline, .value = m_TimelineValue, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
	};
	const VkCommandBufferSubmitInfo l_CmdInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = p_Target.cmd };
	const VkSubmitInfo2 l_Submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = 1,
		.pWaitSemaphoreInfos = &l_Wait,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &l_CmdInfo,
		.signalSemaphoreInfoCount = 2,
		.pSignalSemaphoreInfos = l_Signals,
	};
	WB_VK_CHECK(vkQueueSubmit2(p_Context.queue(), 1, &l_Submit, VK_NULL_HANDLE));

	const VkSwapchainKHR l_Swapchain = p_Swapchain.handle();
	const VkPresentInfoKHR l_Present{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &l_RenderFinished,
		.swapchainCount = 1,
		.pSwapchains = &l_Swapchain,
		.pImageIndices = &p_Target.imageIndex,
	};
	const VkResult l_PresentResult = vkQueuePresentKHR(p_Context.queue(), &l_Present);

	m_Current = (m_Current + 1) % FRAMES_IN_FLIGHT;

	if (l_PresentResult == VK_ERROR_OUT_OF_DATE_KHR || l_PresentResult == VK_SUBOPTIMAL_KHR)
		return FrameStatus::SwapchainOutOfDate;
	WB_VK_CHECK(l_PresentResult);
	return FrameStatus::Ok;
}

void FrameScheduler::deferDestroy(std::move_only_function<void()> p_Destroy)
{
	m_PendingDestroys.push_back(PendingDestroy{ .afterValue = m_TimelineValue + 1, .destroy = std::move(p_Destroy) });
}

void FrameScheduler::collectGarbage(const GraphicsContext& p_Context)
{
	if (m_PendingDestroys.empty())
		return;
	const uint64_t l_Completed = completedValue(p_Context);
	std::erase_if(m_PendingDestroys, [&](PendingDestroy& p_Pending)
	{
		if (p_Pending.afterValue > l_Completed)
			return false;
		p_Pending.destroy();
		return true;
	});
}
} // namespace wb::gfx
