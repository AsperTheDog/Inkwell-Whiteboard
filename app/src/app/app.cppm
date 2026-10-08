// Application shell: owns the subsystems and runs the event-driven main loop.
module;
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <volk.h>

export module wb.app;

import wb.debug.imgui_layer;
import wb.editor;
import wb.gfx.buffer;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.swapchain;
import wb.math;
import wb.platform.input;
import wb.platform.window;
import wb.render.canvas_renderer;
import wb.tools.tool;

export namespace wb
{
struct AppOptions
{
	// > 0: run the scripted smoke scenario for this many frames, then quit
	uint32_t smokeTestFrames = 0;
	// When set, the last frame is saved to this PNG file
	std::string screenshotPath;
	// Zoom of the final view in the smoke scenario
	double smokeZoom = 1.0;
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
	void updateCursor();

	[[nodiscard]] bool needsRedraw() const;
	void requestRedraw(uint32_t p_Frames = 2);
	void recreateSwapchain();
	void renderFrame();
	void buildUi();
	void buildToolbar();
	void buildDebugPanel();
	void addStressStrokes(uint32_t p_Count);
	void addStressStrokes(uint32_t p_Count, const Rect& p_Area);

	// Scripted input for --smoke-test (smoke_test.cpp)
	void driveSmokeTest();
	void queueStroke(platform::PointerDevice p_Device, const tools::BrushState& p_Brush, const std::vector<Vec2>& p_Points, const std::vector<float>& p_Pressures);
	void captureScreenshot(VkCommandBuffer p_Cmd, VkImage p_Image, VkExtent2D p_Extent);
	void writeScreenshot(VkExtent2D p_Extent);

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
	gfx::StagingBelt m_Staging;
	render::CanvasRenderer m_Canvas;
	debug::ImGuiLayer m_ImGui;
	Editor m_Editor;

	gfx::PresentPolicy m_PresentPolicy = gfx::PresentPolicy::VSync;
	bool m_Running = true;
	bool m_SwapchainDirty = false;
	bool m_ShowDebug = false;
	bool m_DarkTheme = false;
	bool m_ShowGrid = true;
	uint32_t m_RedrawFrames = 2; // frames still to draw after the last change (ImGui needs one extra to settle)
	uint64_t m_LastDebugRefreshNs = 0;
	uint64_t m_LastUpdateNs = 0;
	std::optional<platform::PointerEvent> m_LastPointer;
	FrameStats m_Stats{};

	gfx::Buffer m_ScreenshotBuffer;
	std::deque<std::variant<platform::PointerEvent, tools::BrushState>> m_SmokeQueue;
	uint64_t m_SmokeTimeNs = 0;
	uint32_t m_SmokeStep = 0;

	std::array<SDL_Cursor*, 4> m_Cursors{};
	tools::CursorKind m_CurrentCursor = tools::CursorKind::Default;
	bool m_CursorOverUi = false;
};
} // namespace wb
