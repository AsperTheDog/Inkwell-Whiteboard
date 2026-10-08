module;
#include <cstdint>
#include "gfx/vk_check.hpp"
#include "gfx/vma.hpp"

module wb.gfx.image;

import wb.gfx.context;

namespace wb::gfx
{
Image createImage2D(const GraphicsContext& p_Context, const VkExtent2D p_Extent, const VkFormat p_Format, const VkImageUsageFlags p_Usage, const char* p_Name)
{
	const VkImageCreateInfo l_ImageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = p_Format,
		.extent = { p_Extent.width, p_Extent.height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = p_Usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	const VmaAllocationCreateInfo l_AllocInfo{ .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };

	Image l_Image{};
	WB_VK_CHECK(vmaCreateImage(p_Context.allocator(), &l_ImageInfo, &l_AllocInfo, &l_Image.handle, &l_Image.allocation, nullptr));
	l_Image.extent = p_Extent;
	l_Image.format = p_Format;
	p_Context.setName(l_Image.handle, VK_OBJECT_TYPE_IMAGE, p_Name);

	const VkImageViewCreateInfo l_ViewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = l_Image.handle,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = p_Format,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
	};
	WB_VK_CHECK(vkCreateImageView(p_Context.device(), &l_ViewInfo, nullptr, &l_Image.view));
	return l_Image;
}

void destroyImage(const GraphicsContext& p_Context, Image& p_Image)
{
	if (p_Image.view != VK_NULL_HANDLE)
		vkDestroyImageView(p_Context.device(), p_Image.view, nullptr);
	if (p_Image.handle != VK_NULL_HANDLE)
		vmaDestroyImage(p_Context.allocator(), p_Image.handle, p_Image.allocation);
	p_Image = Image{};
}
} // namespace wb::gfx
