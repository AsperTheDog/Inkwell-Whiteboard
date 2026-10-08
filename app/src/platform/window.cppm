// SDL3 window: creation, sizes (logical points vs. pixels), DPI and the Vulkan surface.
module;
#include <cstdint>
#include <span>
#include <string_view>
#include <SDL3/SDL.h>
#include <volk.h>

export module wb.platform.window;

export namespace wb::platform
{
struct PixelSize
{
	uint32_t width = 0;
	uint32_t height = 0;

	[[nodiscard]] bool isZero() const { return width == 0 || height == 0; }
	bool operator==(const PixelSize&) const = default;
};

class Window
{
public:
	Window() = default;
	~Window();
	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;

	// Creates a maximized, resizable, high-DPI window. Throws std::runtime_error on failure.
	void create(std::string_view p_Title);
	void destroy();

	[[nodiscard]] SDL_Window* handle() const { return m_Window; }
	[[nodiscard]] SDL_WindowID id() const;

	// Drawable size in pixels (swapchain size)
	[[nodiscard]] PixelSize pixelSize() const;
	// Pixels per logical point (pointer events arrive in points)
	[[nodiscard]] float pixelDensity() const;
	// UI scale requested by the OS for this display (for sizing UI elements)
	[[nodiscard]] float displayScale() const;
	[[nodiscard]] bool isMinimized() const;

	[[nodiscard]] static std::span<const char* const> requiredInstanceExtensions();
	[[nodiscard]] VkSurfaceKHR createSurface(VkInstance p_Instance) const;

private:
	SDL_Window* m_Window = nullptr;
};
} // namespace wb::platform
