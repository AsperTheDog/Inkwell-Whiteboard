// VMA-backed 2D images with a view (textures such as the UI glyph atlas).
module;
#include <cstdint>
#include "gfx/vma.hpp"

export module wb.gfx.image;

import wb.gfx.context;

export namespace wb::gfx
{
struct Image
{
	VkImage handle = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkExtent2D extent{};
	VkFormat format = VK_FORMAT_UNDEFINED;

	[[nodiscard]] bool isValid() const { return handle != VK_NULL_HANDLE; }
};

// Single-mip, single-layer device-local image
[[nodiscard]] Image createImage2D(const GraphicsContext& p_Context, VkExtent2D p_Extent, VkFormat p_Format, VkImageUsageFlags p_Usage, const char* p_Name);
void destroyImage(const GraphicsContext& p_Context, Image& p_Image);
} // namespace wb::gfx
