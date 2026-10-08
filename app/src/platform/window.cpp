module;
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>

module wb.platform.window;

namespace wb::platform
{
Window::~Window()
{
	destroy();
}

void Window::create(const std::string_view p_Title)
{
	const std::string l_Title{ p_Title };
	constexpr SDL_WindowFlags l_Flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED;
	m_Window = SDL_CreateWindow(l_Title.c_str(), 1280, 800, l_Flags);
	if (m_Window == nullptr)
		throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
	SDL_SetWindowMinimumSize(m_Window, 480, 320);
}

void Window::destroy()
{
	if (m_Window != nullptr)
	{
		SDL_DestroyWindow(m_Window);
		m_Window = nullptr;
	}
}

SDL_WindowID Window::id() const
{
	return SDL_GetWindowID(m_Window);
}

PixelSize Window::pixelSize() const
{
	int l_Width = 0;
	int l_Height = 0;
	SDL_GetWindowSizeInPixels(m_Window, &l_Width, &l_Height);
	return PixelSize{ .width = static_cast<uint32_t>(l_Width > 0 ? l_Width : 0), .height = static_cast<uint32_t>(l_Height > 0 ? l_Height : 0) };
}

float Window::pixelDensity() const
{
	const float l_Density = SDL_GetWindowPixelDensity(m_Window);
	return l_Density > 0.f ? l_Density : 1.f;
}

float Window::displayScale() const
{
	const float l_Scale = SDL_GetWindowDisplayScale(m_Window);
	return l_Scale > 0.f ? l_Scale : 1.f;
}

bool Window::isMinimized() const
{
	return (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) != 0;
}

std::span<const char* const> Window::requiredInstanceExtensions()
{
	Uint32 l_Count = 0;
	const char* const* l_Extensions = SDL_Vulkan_GetInstanceExtensions(&l_Count);
	if (l_Extensions == nullptr)
		throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
	return { l_Extensions, l_Count };
}

VkSurfaceKHR Window::createSurface(const VkInstance p_Instance) const
{
	VkSurfaceKHR l_Surface = VK_NULL_HANDLE;
	if (!SDL_Vulkan_CreateSurface(m_Window, p_Instance, nullptr, &l_Surface))
		throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
	return l_Surface;
}
} // namespace wb::platform
