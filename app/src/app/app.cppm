// Application shell: owns the subsystems and runs the event-driven main loop.
module;
#include <cstdint>
#include <optional>
#include <SDL3/SDL.h>

export module wb.app;

import wb.debug.imgui_layer;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.swapchain;
import wb.math;
import wb.platform.input;
import wb.platform.window;

export namespace wb
{
struct AppOptions
{
	// > 0: render this many frames continuously, then quit (automated smoke test)
	uint32_t smokeTestFrames = 0;
};

class App
{
public:
	explicit App(const AppOptions& p_Options) : m_Options(p_Options) {}

	// Runs until the window closes. Throws on fatal errors (gfx::UnsupportedGpuError for missing GPU support).
	void run();

private:
	void init();
	void shutdown();

	// Returns false when the app should quit
	bool pumpEvents(bool p_Block);
	void handleEvent(const SDL_Event& p_Event);
	void handleInput(const platform::InputEvent& p_Event);

	[[nodiscard]] bool needsRedraw() const;
	void recreateSwapchain();
	void renderFrame();
	void buildDebugUi();

	struct FrameStats
	{
		uint64_t frameCount = 0;
		uint64_t windowStartNs = 0;
		uint32_t windowFrames = 0;
		double fps = 0.0;
		double cpuFrameMs = 0.0;
	};

	AppOptions m_Options;
	platform::Window m_Window;
	platform::InputRouter m_Input;
	gfx::GraphicsContext m_Context;
	gfx::Swapchain m_Swapchain;
	gfx::FrameScheduler m_Frames;
	debug::ImGuiLayer m_ImGui;

	gfx::PresentPolicy m_PresentPolicy = gfx::PresentPolicy::VSync;
	bool m_Running = true;
	bool m_SwapchainDirty = false;
	bool m_DarkTheme = false;
	uint32_t m_RedrawFrames = 2; // frames still to draw after the last change (ImGui needs one extra to settle)
	uint64_t m_LastDebugRefreshNs = 0;
	std::optional<platform::PointerEvent> m_LastPointer;
	FrameStats m_Stats{};
};
} // namespace wb
