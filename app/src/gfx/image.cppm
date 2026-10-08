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
	uint32_t mipLevels = 1;
	uint32_t layers = 1;

	[[nodiscard]] bool isValid() const { return handle != VK_NULL_HANDLE; }
};

// Device-local 2D image. p_Array makes the view a 2D array (even with one layer) so shaders can use one texture type.
[[nodiscard]] Image createImage2D(const GraphicsContext& p_Context, VkExtent2D p_Extent, VkFormat p_Format, VkImageUsageFlags p_Usage, const char* p_Name, uint32_t p_MipLevels = 1, uint32_t p_Layers = 1, bool p_Array = false);
void destroyImage(const GraphicsContext& p_Context, Image& p_Image);
} // namespace wb::gfx
