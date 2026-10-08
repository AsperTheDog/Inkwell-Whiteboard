module;
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>
#include "gfx/vk_check.hpp"

module wb.gfx.swapchain;

import wb.gfx.context;
import wb.platform.window;

namespace wb::gfx
{
namespace
{
VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& p_Formats)
{
	// UNORM targets: colors are authored in sRGB and blended in sRGB space, like every 2D design tool
	constexpr VkFormat l_Preferred[]{ VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32 };
	for (const VkFormat l_Format : l_Preferred)
	{
		for (const VkSurfaceFormatKHR& l_Candidate : p_Formats)
		{
			if (l_Candidate.format == l_Format && l_Candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				return l_Candidate;
		}
	}
	for (const VkSurfaceFormatKHR& l_Candidate : p_Formats)
	{
		if (l_Candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && l_Candidate.format != VK_FORMAT_UNDEFINED)
			return l_Candidate;
	}
	return p_Formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& p_Modes, const PresentPolicy p_Policy)
{
	const auto l_Has = [&](const VkPresentModeKHR p_Mode) { return std::ranges::find(p_Modes, p_Mode) != p_Modes.end(); };
	switch (p_Policy)
	{
	case PresentPolicy::Immediate:
		if (l_Has(VK_PRESENT_MODE_IMMEDIATE_KHR))
			return VK_PRESENT_MODE_IMMEDIATE_KHR;
		[[fallthrough]];
	case PresentPolicy::LowLatency:
		if (l_Has(VK_PRESENT_MODE_MAILBOX_KHR))
			return VK_PRESENT_MODE_MAILBOX_KHR;
		[[fallthrough]];
	case PresentPolicy::VSync:
		break;
	}
	return VK_PRESENT_MODE_FIFO_KHR; // guaranteed by the spec
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(const VkCompositeAlphaFlagsKHR p_Supported)
{
	constexpr VkCompositeAlphaFlagBitsKHR l_Order[]{ VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
	for (const VkCompositeAlphaFlagBitsKHR l_Mode : l_Order)
	{
		if ((p_Supported & l_Mode) != 0)
			return l_Mode;
	}
	return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
} // namespace

void Swapchain::create(const GraphicsContext& p_Context, const platform::PixelSize p_Size, const PresentPolicy p_Policy)
{
	build(p_Context, p_Size, p_Policy, VK_NULL_HANDLE);
}

void Swapchain::recreate(const GraphicsContext& p_Context, const platform::PixelSize p_Size, const PresentPolicy p_Policy)
{
	const VkSwapchainKHR l_Old = m_Swapchain;
	destroyImageResources(p_Context);
	build(p_Context, p_Size, p_Policy, l_Old);
	if (l_Old != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(p_Context.device(), l_Old, nullptr);
}

void Swapchain::build(const GraphicsContext& p_Context, const platform::PixelSize p_Size, const PresentPolicy p_Policy, const VkSwapchainKHR p_Old)
{
	const VkPhysicalDevice l_Physical = p_Context.physicalDevice();
	const VkSurfaceKHR l_Surface = p_Context.surface();
	const VkDevice l_Device = p_Context.device();

	VkSurfaceCapabilitiesKHR l_Caps{};
	WB_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(l_Physical, l_Surface, &l_Caps));

	uint32_t l_FormatCount = 0;
	WB_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(l_Physical, l_Surface, &l_FormatCount, nullptr));
	std::vector<VkSurfaceFormatKHR> l_Formats(l_FormatCount);
	WB_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(l_Physical, l_Surface, &l_FormatCount, l_Formats.data()));

	uint32_t l_ModeCount = 0;
	WB_VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(l_Physical, l_Surface, &l_ModeCount, nullptr));
	m_SupportedModes.resize(l_ModeCount);
	WB_VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(l_Physical, l_Surface, &l_ModeCount, m_SupportedModes.data()));

	m_Format = chooseFormat(l_Formats);
	m_PresentMode = choosePresentMode(m_SupportedModes, p_Policy);

	if (l_Caps.currentExtent.width != UINT32_MAX)
	{
		m_Extent = l_Caps.currentExtent;
	}
	else
	{
		m_Extent.width = std::clamp(p_Size.width, l_Caps.minImageExtent.width, l_Caps.maxImageExtent.width);
		m_Extent.height = std::clamp(p_Size.height, l_Caps.minImageExtent.height, l_Caps.maxImageExtent.height);
	}

	uint32_t l_ImageCount = std::max(l_Caps.minImageCount + 1, 3u);
	if (l_Caps.maxImageCount > 0)
		l_ImageCount = std::min(l_ImageCount, l_Caps.maxImageCount);
	m_MinImageCount = l_Caps.minImageCount;

	m_Readback = (l_Caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
	const VkSwapchainCreateInfoKHR l_CreateInfo{
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = l_Surface,
		.minImageCount = l_ImageCount,
		.imageFormat = m_Format.format,
		.imageColorSpace = m_Format.colorSpace,
		.imageExtent = m_Extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (l_Caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)),
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = (l_Caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0 ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : l_Caps.currentTransform,
		.compositeAlpha = chooseCompositeAlpha(l_Caps.supportedCompositeAlpha),
		.presentMode = m_PresentMode,
		.clipped = VK_TRUE,
		.oldSwapchain = p_Old,
	};
	WB_VK_CHECK(vkCreateSwapchainKHR(l_Device, &l_CreateInfo, nullptr, &m_Swapchain));

	uint32_t l_Count = 0;
	WB_VK_CHECK(vkGetSwapchainImagesKHR(l_Device, m_Swapchain, &l_Count, nullptr));
	m_Images.resize(l_Count);
	WB_VK_CHECK(vkGetSwapchainImagesKHR(l_Device, m_Swapchain, &l_Count, m_Images.data()));

	m_Views.resize(l_Count, VK_NULL_HANDLE);
	m_RenderFinished.resize(l_Count, VK_NULL_HANDLE);
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		const VkImageViewCreateInfo l_ViewInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_Images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = m_Format.format,
			.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
		};
		WB_VK_CHECK(vkCreateImageView(l_Device, &l_ViewInfo, nullptr, &m_Views[i]));

		constexpr VkSemaphoreCreateInfo l_SemaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		WB_VK_CHECK(vkCreateSemaphore(l_Device, &l_SemaphoreInfo, nullptr, &m_RenderFinished[i]));

		const std::string l_Suffix = " #" + std::to_string(i);
		p_Context.setName(m_Images[i], VK_OBJECT_TYPE_IMAGE, ("swapchain image" + l_Suffix).c_str());
		p_Context.setName(m_RenderFinished[i], VK_OBJECT_TYPE_SEMAPHORE, ("render finished" + l_Suffix).c_str());
	}

	spdlog::info("Swapchain {}x{}, {} images, format {}, present mode {}", m_Extent.width, m_Extent.height, l_Count, toString(m_Format.format), toString(m_PresentMode));
}

void Swapchain::destroyImageResources(const GraphicsContext& p_Context)
{
	const VkDevice l_Device = p_Context.device();
	for (const VkImageView l_View : m_Views)
		vkDestroyImageView(l_Device, l_View, nullptr);
	for (const VkSemaphore l_Semaphore : m_RenderFinished)
		vkDestroySemaphore(l_Device, l_Semaphore, nullptr);
	m_Views.clear();
	m_RenderFinished.clear();
	m_Images.clear();
}

void Swapchain::destroy(const GraphicsContext& p_Context)
{
	destroyImageResources(p_Context);
	if (m_Swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(p_Context.device(), m_Swapchain, nullptr);
		m_Swapchain = VK_NULL_HANDLE;
	}
}
} // namespace wb::gfx
