// Application shell: owns the subsystems and runs the event-driven main loop.
module;
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
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
import wb.io.settings;
import wb.math;
import wb.platform.clipboard;
import wb.platform.input;
import wb.platform.window;
import wb.render.canvas_renderer;
import wb.render.image_store;
import wb.session;
import wb.tools.tool;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.renderer;
import wb.ui.theme;

namespace wb
{
// Swatches offered by the toolbar and the selection bar (RGBA8). Module-internal: shared by the app's source files.
inline constexpr std::array<uint32_t, 12> PALETTE{
	0x1F1F1FFFu, 0x757575FFu, 0xFFFFFFFFu, 0xE53935FFu, 0xFB8C00FFu, 0xFDD835FFu,
	0x43A047FFu, 0x00ACC1FFu, 0x1E88E5FFu, 0x5E35B1FFu, 0xD81B60FFu, 0x6D4C41FFu,
};
} // namespace wb

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
	// Smoke scenario: dark theme, and a panel to leave open in the final frame (pen, eraser, select, picker, menu, shortcuts, dialog)
	bool smokeDark = false;
	std::string smokeUi;
};

class App
{
public:
	explicit App(const AppOptions& p_Options) : m_Options(p_Options) {}

	// Runs until the window closes. Throws on fatal errors (gfx::UnsupportedGpuError for missing GPU support).
	void run();
	// A smoke-test check failed (see the log)
	[[nodiscard]] bool failed() const { return m_Failed; }

private:
	// Things that may have to wait for the "unsaved changes" question
	enum class Action : uint8_t
	{
		None,
		NewBoard,
		OpenFile,
		Quit,
	};
	enum class DialogKind : uint8_t
	{
		None,
		Open,
		Save,
		Pictures,
	};
	// The floating panel that is open, if any
	enum class Popup : uint8_t
	{
		None,
		Pen,
		Eraser,
		Select,
		Menu,
		SelectionColor,
	};

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
	void updateTheme(double p_Dt);
	void buildDebugPanel();

	// The user interface (app_ui.cpp, app_popovers.cpp, app_menu.cpp, app_selection.cpp)
	void buildCanvasOverlays();
	void buildTopBar();
	void buildToolbar();
	void buildZoomPill();
	void buildPopover();
	void buildPenPopover(ui::Rect2 p_Anchor);
	void buildEraserPopover(ui::Rect2 p_Anchor);
	void buildSelectPopover(ui::Rect2 p_Anchor);
	void buildSelectionBar();
	void buildMenu();
	void buildShortcuts();
	void buildDialogs();
	void buildToast();
	void toggleInterface();
	// Pictures (app_images.cpp)
	void importPictures(std::vector<platform::ClipboardPicture> p_Pictures, std::optional<Vec2> p_ScreenPosition);
	bool pasteFromSystemClipboard();
	void showInsertPictureDialog();
	void handleDroppedFile(const SDL_DropEvent& p_Event);
	void compressSelectedPictures();
	void toggleSelectedPlayback();
	[[nodiscard]] render::ImageClock imageClock() const { return render::ImageClock{ .seconds = m_AnimSeconds, .playing = m_PlayAnimations }; }
	void openPopup(Popup p_Popup);
	void closePopup(const char* p_Why = "");
	void togglePopup(Popup p_Popup);
	void pushRecentColor(Color p_Color);
	[[nodiscard]] bool modalOpen() const { return m_PromptOpen || m_RecoveryOpen || m_MessageOpen; }
	// Keyboard input that belongs to dialogs and popups; returns true when it was used
	bool handleUiKey(const SDL_KeyboardEvent& p_Event);
	// Colour grid shared by the pen popover and the selection bar. Returns true when a swatch was clicked.
	bool paletteGrid(ui::Rect2 p_Area, float p_Diameter, int p_Columns, const std::optional<Color>& p_Selected, Color& p_Picked);
	void addStressStrokes(uint32_t p_Count);
	void addStressStrokes(uint32_t p_Count, const Rect& p_Area);

	// Files (app_files.cpp)
	void initPersistence();
	void finishPersistence(bool p_DiscardRecovery);
	void loadSettings();
	void saveSettings();
	void requestAction(Action p_Action);
	void performAction(Action p_Action);
	void requestSave(bool p_SaveAs, Action p_After);
	void showOpenDialog();
	void showSaveDialog(Action p_After);
	void handleDialogResult();
	void saveToPath(const std::filesystem::path& p_Path, Action p_After);
	void openPath(const std::filesystem::path& p_Path);
	void updateTitle();
	void showToast(std::string p_Text);
	void showMessage(std::string p_Text);
	bool handleFileShortcut(const SDL_KeyboardEvent& p_Event);
	static void SDLCALL dialogCallback(void* p_User, const char* const* p_Files, int p_Filter);

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
	Session m_Session{ m_Editor };
	Settings m_Settings;
	std::filesystem::path m_PrefDir;     // empty: nothing is persisted (smoke test, or no writable location)
	std::string m_LastDirectory;         // where the file dialogs start

	gfx::PresentPolicy m_PresentPolicy = gfx::PresentPolicy::VSync;
	bool m_Running = true;
	bool m_SwapchainDirty = false;
	bool m_ShowDebug = false;
	bool m_DarkTheme = false;
	bool m_ShowGrid = true;
	bool m_ShowShortcuts = false;
	bool m_HideUi = false; // F2: only the board is drawn
	bool m_PlayAnimations = true;
	double m_AnimSeconds = 0.0; // animation clock: advances while animations play
	uint64_t m_AnimLastNs = 0;
	bool m_ExternalClipboardNewer = true; // something other than our own copy was put on the system clipboard last
	uint64_t m_SeenClipSerial = 0;
	uint32_t m_DropCount = 0;
	uint32_t m_RedrawFrames = 2; // frames still to draw after the last change (ImGui needs one extra to settle)
	uint64_t m_LastDebugRefreshNs = 0;
	uint64_t m_LastUpdateNs = 0;
	std::optional<platform::PointerEvent> m_LastPointer;
	FrameStats m_Stats{};

	gfx::Buffer m_ScreenshotBuffer;
	std::deque<std::variant<platform::PointerEvent, tools::BrushState>> m_SmokeQueue;
	uint64_t m_SmokeTimeNs = 0;
	uint32_t m_SmokeStep = 0;

	// User interface
	ui::FontAtlas m_FontAtlas;
	ui::Context m_Ui;
	ui::UiRenderer m_UiRenderer;
	ui::Theme m_Theme{};
	float m_ThemeBlend = 0.f; // 0 light .. 1 dark, eased
	uint64_t m_LastUiNs = 0;
	Popup m_Popup = Popup::None;
	platform::PointerDevice m_PopupDevice = platform::PointerDevice::Mouse; // the device that opened it: its tool decides when a tool popup closes
	bool m_PickerOpen = false;  // the pen popover shows the custom colour page
	ui::Hsv m_PickerHsv{ 0.6f, 0.7f, 0.9f };
	std::array<uint32_t, 5> m_RecentColors{}; // RGBA8, 0 = empty, most recent first
	int m_MenuSection = 0;
	std::array<ui::Rect2, tools::TOOL_KIND_COUNT> m_ToolRects{};
	ui::Rect2 m_ToolbarRect{};
	ui::Rect2 m_MenuAnchor{};
	ui::Rect2 m_SelectionBarRect{};

	std::array<SDL_Cursor*, tools::CURSOR_KIND_COUNT> m_Cursors{};
	tools::CursorKind m_CurrentCursor = tools::CursorKind::Default;
	bool m_CursorOverUi = false;
	bool m_PointerInWindow = false;

	// Unsaved-changes / recovery / error dialogs and the file dialog hand-off (dialogs run on SDL's own threads)
	Action m_PromptAction = Action::None;
	bool m_PromptOpen = false;
	bool m_QuitDiscard = false;
	std::optional<RecoveryInfo> m_Recovery;
	bool m_RecoveryOpen = false;
	std::string m_Message;
	bool m_MessageOpen = false;
	std::string m_Toast;
	uint64_t m_ToastUntilNs = 0;
	std::string m_LastTitle;
	bool m_Failed = false;

	uint32_t m_DialogEvent = 0;
	DialogKind m_DialogKind = DialogKind::None;
	Action m_DialogAfter = Action::None;
	std::mutex m_DialogMutex;
	bool m_DialogReady = false;
	std::vector<std::string> m_DialogPaths;
};
} // namespace wb
