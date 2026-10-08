// Window swapchain. Owns the images, their views and one "render finished" semaphore per image
// (presentation waits on it; indexing by image avoids reusing a semaphore that a present may still hold).
module;
#include <cstdint>
#include <span>
#include <vector>
#include <volk.h>

export module wb.gfx.swapchain;

import wb.gfx.context;
import wb.platform.window;

export namespace wb::gfx
{
enum class PresentPolicy : uint8_t
{
	VSync,      // FIFO: no tearing, capped at the display rate (always available)
	LowLatency, // MAILBOX when available, else FIFO
	Immediate,  // IMMEDIATE when available (may tear), else MAILBOX, else FIFO
};

class Swapchain
{
public:
	void create(const GraphicsContext& p_Context, platform::PixelSize p_Size, PresentPolicy p_Policy);
	// Recreates in place; the caller must make sure the GPU no longer uses the old images.
	void recreate(const GraphicsContext& p_Context, platform::PixelSize p_Size, PresentPolicy p_Policy);
	void destroy(const GraphicsContext& p_Context);

	[[nodiscard]] VkSwapchainKHR handle() const { return m_Swapchain; }
	[[nodiscard]] VkFormat format() const { return m_Format.format; }
	[[nodiscard]] VkExtent2D extent() const { return m_Extent; }
	[[nodiscard]] VkPresentModeKHR presentMode() const { return m_PresentMode; }
	[[nodiscard]] uint32_t imageCount() const { return static_cast<uint32_t>(m_Images.size()); }
	[[nodiscard]] uint32_t minImageCount() const { return m_MinImageCount; }
	[[nodiscard]] VkImage image(const uint32_t p_Index) const { return m_Images[p_Index]; }
	[[nodiscard]] VkImageView view(const uint32_t p_Index) const { return m_Views[p_Index]; }
	[[nodiscard]] VkSemaphore renderFinished(const uint32_t p_Index) const { return m_RenderFinished[p_Index]; }
	[[nodiscard]] std::span<const VkPresentModeKHR> supportedPresentModes() const { return m_SupportedModes; }
	[[nodiscard]] bool isValid() const { return m_Swapchain != VK_NULL_HANDLE; }

private:
	void build(const GraphicsContext& p_Context, platform::PixelSize p_Size, PresentPolicy p_Policy, VkSwapchainKHR p_Old);
	void destroyImageResources(const GraphicsContext& p_Context);

	VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
	VkSurfaceFormatKHR m_Format{};
	VkExtent2D m_Extent{};
	VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
	uint32_t m_MinImageCount = 2;
	std::vector<VkImage> m_Images;
	std::vector<VkImageView> m_Views;
	std::vector<VkSemaphore> m_RenderFinished;
	std::vector<VkPresentModeKHR> m_SupportedModes;
};
} // namespace wb::gfx
