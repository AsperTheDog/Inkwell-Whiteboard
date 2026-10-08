// Dear ImGui, used for the developer overlay (F3) and, until the real UI lands, a temporary toolbar.
module;
#include <functional>
#include <SDL3/SDL.h>
#include <volk.h>

export module wb.debug.imgui_layer;

import wb.gfx.context;
import wb.gfx.swapchain;
import wb.platform.window;

export namespace wb::debug
{
class ImGuiLayer
{
public:
	void init(const gfx::GraphicsContext& p_Context, const platform::Window& p_Window, const gfx::Swapchain& p_Swapchain);
	void shutdown();

	// Every event goes to ImGui so its input state stays consistent; use wantsPointer()/wantsKeyboard() to decide
	// whether the app should also act on it.
	void processEvent(const SDL_Event& p_Event) const;
	[[nodiscard]] bool wantsPointer() const;
	[[nodiscard]] bool wantsKeyboard() const;

	// Builds this frame's UI with p_Build
	void buildFrame(const std::function<void()>& p_Build);
	// Records the draw data built by buildFrame() into the current rendering scope
	void record(VkCommandBuffer p_Cmd) const;

	void onSwapchainRecreated(const gfx::Swapchain& p_Swapchain) const;

private:
	bool m_Initialized = false;
	bool m_HasDrawData = false;
};
} // namespace wb::debug
