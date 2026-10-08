// Dear ImGui, used only for the developer overlay (toggled with F3). The real UI is custom (wb.ui).
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

	void setVisible(bool p_Visible) { m_Visible = p_Visible; }
	void toggle() { m_Visible = !m_Visible; }
	[[nodiscard]] bool isVisible() const { return m_Visible; }

	// Forwards an event to ImGui. Returns true when ImGui wants to own it (the app should then ignore it).
	bool processEvent(const SDL_Event& p_Event);
	[[nodiscard]] bool wantsPointer() const;
	[[nodiscard]] bool wantsKeyboard() const;

	// Builds the UI for this frame with p_Build (only when visible)
	void buildFrame(const std::function<void()>& p_Build);
	// Records the draw data built by buildFrame() into the current rendering scope
	void record(VkCommandBuffer p_Cmd) const;

	void onSwapchainRecreated(const gfx::Swapchain& p_Swapchain) const;

private:
	bool m_Initialized = false;
	bool m_Visible = false;
	bool m_HasDrawData = false;
};
} // namespace wb::debug
