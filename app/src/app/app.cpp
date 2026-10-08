module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include "gfx/vk_check.hpp"

module wb.app;

import wb.debug.imgui_layer;
import wb.doc.commands;
import wb.doc.document;
import wb.doc.object;
import wb.brush.stroke_builder;
import wb.editor;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.swapchain;
import wb.math;
import wb.platform.input;
import wb.platform.window;
import wb.render.canvas_renderer;
import wb.tools.tool;
import wb.view.camera;

namespace wb
{
namespace
{
// Canvas and grid colors per theme (proper theming arrives with wb.ui)
constexpr Color LIGHT_CANVAS = Color::fromRgba8(0xF5F5F2FFu);
constexpr Color DARK_CANVAS = Color::fromRgba8(0x1F2023FFu);
constexpr Color LIGHT_GRID = Color::fromRgba8(0x00000030u);
constexpr Color DARK_GRID = Color::fromRgba8(0xFFFFFF22u);

constexpr std::array<uint32_t, 12> PALETTE{
	0x1F1F1FFFu, 0x757575FFu, 0xFFFFFFFFu, 0xE53935FFu, 0xFB8C00FFu, 0xFDD835FFu,
	0x43A047FFu, 0x00ACC1FFu, 0x1E88E5FFu, 0x5E35B1FFu, 0xD81B60FFu, 0x6D4C41FFu,
};

// While idle with the debug overlay open, refresh its numbers at this interval
constexpr uint64_t DEBUG_REFRESH_NS = 250'000'000ull;
constexpr VkDeviceSize STAGING_CHUNK_BYTES = 4ull * 1024 * 1024;

const char* deviceName(const platform::PointerDevice p_Device)
{
	switch (p_Device)
	{
	case platform::PointerDevice::Mouse:
		return "mouse";
	case platform::PointerDevice::Pen:
		return "pen";
	case platform::PointerDevice::Touch:
		return "touch";
	}
	return "?";
}

const char* phaseName(const platform::PointerPhase p_Phase)
{
	switch (p_Phase)
	{
	case platform::PointerPhase::Down:
		return "down";
	case platform::PointerPhase::Move:
		return "move";
	case platform::PointerPhase::Up:
		return "up";
	case platform::PointerPhase::Cancel:
		return "cancel";
	}
	return "?";
}

const char* gpuTypeName(const VkPhysicalDeviceType p_Type)
{
	switch (p_Type)
	{
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
		return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
		return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
		return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
		return "cpu (software)";
	default:
		return "other";
	}
}

ImVec4 toImVec4(const Color p_Color)
{
	return ImVec4(p_Color.r, p_Color.g, p_Color.b, p_Color.a);
}
} // namespace

void App::run()
{
	init();
	try
	{
		while (m_Running)
		{
			// Wait for the GPU before reading input: the freshest input then goes into the frame
			m_Frames.waitForSlot(m_Context);
			m_Frames.collectGarbage(m_Context);
			m_Staging.beginFrame(m_Frames.currentSlot());

			const bool l_CanRender = !m_Window.isMinimized() && !m_Window.pixelSize().isZero();
			if (!pumpEvents(!l_CanRender || !needsRedraw()))
				break;
			if (!l_CanRender || m_Window.isMinimized() || m_Window.pixelSize().isZero())
				continue;

			if (m_SwapchainDirty)
				recreateSwapchain();

			const VkExtent2D l_Extent = m_Swapchain.extent();
			m_Editor.setViewport(Vec2{ static_cast<float>(l_Extent.width), static_cast<float>(l_Extent.height) }, m_Window.displayScale());

			const uint64_t l_Now = SDL_GetTicksNS();
			const double l_Dt = m_LastUpdateNs == 0 ? 0.0 : static_cast<double>(l_Now - m_LastUpdateNs) * 1e-9;
			m_LastUpdateNs = l_Now;
			if (m_Editor.update(l_Dt))
				requestRedraw(1);

			if (m_Options.smokeTestFrames > 0)
				driveSmokeTest();

			updateCursor();
			if (!needsRedraw())
				continue;

			renderFrame();
		}
	}
	catch (...)
	{
		shutdown();
		throw;
	}
	shutdown();
}

void App::init()
{
	m_Window.create("Whiteboard");
	m_Context.init(m_Window);
	m_Swapchain.create(m_Context, m_Window.pixelSize(), m_PresentPolicy);
	m_Frames.init(m_Context);
	m_Staging.init(m_Context, STAGING_CHUNK_BYTES);
	m_Canvas.init(m_Context, m_Swapchain.format());
	m_Canvas.attach(m_Editor.document());
	m_ImGui.init(m_Context, m_Window, m_Swapchain);

	m_Cursors[static_cast<size_t>(tools::CursorKind::Default)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
	m_Cursors[static_cast<size_t>(tools::CursorKind::Crosshair)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
	m_Cursors[static_cast<size_t>(tools::CursorKind::Move)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
	m_Cursors[static_cast<size_t>(tools::CursorKind::Pointer)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
	m_CurrentCursor = tools::CursorKind::Default;

#ifdef WB_DEBUG
	m_ShowDebug = true;
#endif
	m_Stats.windowStartNs = SDL_GetTicksNS();
}

void App::shutdown()
{
	m_Context.waitIdle();
	for (SDL_Cursor*& l_Cursor : m_Cursors)
	{
		if (l_Cursor != nullptr)
			SDL_DestroyCursor(l_Cursor);
		l_Cursor = nullptr;
	}
	gfx::destroyBuffer(m_Context, m_ScreenshotBuffer);
	m_ImGui.shutdown();
	m_Canvas.destroy(m_Context);
	m_Staging.destroy(m_Context);
	m_Frames.destroy(m_Context);
	m_Swapchain.destroy(m_Context);
	m_Context.destroy();
	m_Window.destroy();
}

// ------------------------------------------------------------------------------------------------ events

bool App::pumpEvents(const bool p_Block)
{
	SDL_Event l_Event;
	if (p_Block)
	{
		// Sleep until something happens. With the overlay open, wake up periodically to refresh its stats.
		const int32_t l_Timeout = m_ShowDebug ? static_cast<int32_t>(DEBUG_REFRESH_NS / 1'000'000ull) : 1000;
		if (SDL_WaitEventTimeout(&l_Event, l_Timeout))
			handleEvent(l_Event);
	}
	while (SDL_PollEvent(&l_Event))
		handleEvent(l_Event);

	if (m_ShowDebug)
	{
		const uint64_t l_Now = SDL_GetTicksNS();
		if (l_Now - m_LastDebugRefreshNs >= DEBUG_REFRESH_NS)
		{
			m_LastDebugRefreshNs = l_Now;
			requestRedraw(1);
		}
	}
	return m_Running;
}

void App::requestRedraw(const uint32_t p_Frames)
{
	m_RedrawFrames = std::max(m_RedrawFrames, p_Frames);
}

void App::handleEvent(const SDL_Event& p_Event)
{
	m_ImGui.processEvent(p_Event);

	switch (p_Event.type)
	{
	case SDL_EVENT_QUIT:
	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		m_Running = false;
		return;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		m_SwapchainDirty = true;
		requestRedraw();
		return;
	case SDL_EVENT_WINDOW_EXPOSED:
	case SDL_EVENT_WINDOW_RESTORED:
	case SDL_EVENT_WINDOW_MAXIMIZED:
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		requestRedraw(1);
		return;
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		m_Editor.handleFocusLost();
		requestRedraw();
		return;
	case SDL_EVENT_KEY_DOWN:
		requestRedraw();
		if (p_Event.key.key == SDLK_F3 && !p_Event.key.repeat)
		{
			m_ShowDebug = !m_ShowDebug;
			return;
		}
		if (p_Event.key.key == SDLK_F11 && !p_Event.key.repeat)
		{
			const bool l_Fullscreen = (SDL_GetWindowFlags(m_Window.handle()) & SDL_WINDOW_FULLSCREEN) != 0;
			SDL_SetWindowFullscreen(m_Window.handle(), !l_Fullscreen);
			return;
		}
		if (!m_ImGui.wantsKeyboard())
			m_Editor.handleKeyDown(p_Event.key);
		return;
	case SDL_EVENT_KEY_UP:
		requestRedraw();
		m_Editor.handleKeyUp(p_Event.key); // releases (Space...) must never get lost
		return;
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_TEXT_EDITING:
		requestRedraw();
		return;
	default:
		break;
	}

	if (const std::optional<platform::InputEvent> l_Input = m_Input.translate(p_Event, m_Window.pixelDensity()))
	{
		handleInput(*l_Input);
	}
	else if (p_Event.type == SDL_EVENT_MOUSE_MOTION || p_Event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || p_Event.type == SDL_EVENT_MOUSE_BUTTON_UP)
	{
		requestRedraw(); // synthetic pen mouse events still drive ImGui hover
	}
	else if (m_ShowDebug && (p_Event.type == SDL_EVENT_PEN_AXIS || p_Event.type == SDL_EVENT_PEN_PROXIMITY_IN || p_Event.type == SDL_EVENT_PEN_PROXIMITY_OUT))
	{
		requestRedraw(); // keep the pen diagnostics live
	}
}

void App::handleInput(const platform::InputEvent& p_Event)
{
	requestRedraw();
	if (m_Options.smokeTestFrames > 0)
		return; // the scripted scenario owns the input; a real mouse over the window must not interfere

	const bool l_UiHasPointer = m_ImGui.wantsPointer() && !m_Editor.isBusy();

	if (const platform::PointerEvent* l_Pointer = std::get_if<platform::PointerEvent>(&p_Event))
	{
		m_LastPointer = *l_Pointer;
		m_CursorOverUi = l_UiHasPointer;
		// Releases always reach the editor so no gesture is left hanging
		const bool l_IsRelease = l_Pointer->phase == platform::PointerPhase::Up || l_Pointer->phase == platform::PointerPhase::Cancel;
		if (!l_UiHasPointer || l_IsRelease)
			m_Editor.handlePointer(*l_Pointer);
	}
	else if (const platform::WheelEvent* l_Wheel = std::get_if<platform::WheelEvent>(&p_Event))
	{
		if (!l_UiHasPointer)
			m_Editor.handleWheel(*l_Wheel);
	}
	else if (const platform::PinchEvent* l_Pinch = std::get_if<platform::PinchEvent>(&p_Event))
	{
		m_Editor.handlePinch(*l_Pinch);
	}
}

void App::updateCursor()
{
	const tools::CursorKind l_Wanted = m_CursorOverUi ? tools::CursorKind::Default : m_Editor.cursor();
	if (l_Wanted == m_CurrentCursor)
		return;
	m_CurrentCursor = l_Wanted;
	if (SDL_Cursor* l_Cursor = m_Cursors[static_cast<size_t>(l_Wanted)]; l_Cursor != nullptr)
		SDL_SetCursor(l_Cursor);
}

// ------------------------------------------------------------------------------------------------ rendering

bool App::needsRedraw() const
{
	return m_RedrawFrames > 0 || m_SwapchainDirty || m_Options.smokeTestFrames > 0;
}

void App::recreateSwapchain()
{
	m_Context.waitIdle();
	m_Swapchain.recreate(m_Context, m_Window.pixelSize(), m_PresentPolicy);
	m_ImGui.onSwapchainRecreated(m_Swapchain);
	m_SwapchainDirty = false;
	requestRedraw(1);
}

void App::renderFrame()
{
	const uint64_t l_CpuStart = SDL_GetTicksNS();

	m_ImGui.buildFrame([this] { buildUi(); });

	gfx::FrameStatus l_Status = gfx::FrameStatus::Ok;
	const std::optional<gfx::FrameTarget> l_Target = m_Frames.begin(m_Context, m_Swapchain, l_Status);
	if (!l_Target)
	{
		m_SwapchainDirty = true;
		return;
	}

	const VkCommandBuffer l_Cmd = l_Target->cmd;
	m_Frames.writeStartTimestamp(l_Cmd);

	const Camera& l_Camera = m_Editor.camera();
	const std::optional<render::LiveStrokeView> l_Live = m_Editor.liveStroke();
	{
		const gfx::DebugLabel l_Label(l_Cmd, "canvas uploads");
		m_Canvas.prepare(m_Context, m_Frames, m_Staging, l_Cmd, l_Target->slot, l_Camera, l_Live ? &*l_Live : nullptr);
	}

	gfx::transitionImage(l_Cmd, l_Target->image, {
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccess = VK_ACCESS_2_NONE,
		.dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	});

	const Color l_Background = m_DarkTheme ? DARK_CANVAS : LIGHT_CANVAS;
	{
		const gfx::DebugLabel l_Label(l_Cmd, "frame");
		gfx::beginColorRendering(l_Cmd, l_Target->view, l_Target->extent, VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue{ .float32 = { l_Background.r, l_Background.g, l_Background.b, 1.f } });
		m_Canvas.record(l_Cmd, l_Camera, render::GridStyle{ .enabled = m_ShowGrid, .dotColor = m_DarkTheme ? DARK_GRID : LIGHT_GRID });
		{
			const gfx::DebugLabel l_UiLabel(l_Cmd, "imgui");
			m_ImGui.record(l_Cmd);
		}
		vkCmdEndRendering(l_Cmd);
	}

	const bool l_LastSmokeFrame = m_Options.smokeTestFrames > 0 && m_Stats.frameCount + 1 >= m_Options.smokeTestFrames;
	const bool l_Capture = l_LastSmokeFrame && !m_Options.screenshotPath.empty() && m_Swapchain.supportsReadback();
	if (l_Capture)
	{
		captureScreenshot(l_Cmd, l_Target->image, l_Target->extent);
	}
	else
	{
		gfx::transitionImage(l_Cmd, l_Target->image, {
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.dstStage = VK_PIPELINE_STAGE_2_NONE,
			.dstAccess = VK_ACCESS_2_NONE,
		});
	}

	m_Frames.writeEndTimestamp(l_Cmd);
	if (m_Frames.end(m_Context, m_Swapchain, *l_Target) == gfx::FrameStatus::SwapchainOutOfDate)
		m_SwapchainDirty = true;
	if (l_Capture)
		writeScreenshot(l_Target->extent);

	if (m_RedrawFrames > 0)
		--m_RedrawFrames;
	if (l_LastSmokeFrame)
		m_Running = false;

	const uint64_t l_Now = SDL_GetTicksNS();
	m_Stats.cpuFrameMs = static_cast<double>(l_Now - l_CpuStart) * 1e-6;
	++m_Stats.frameCount;
	++m_Stats.windowFrames;
	if (l_Now - m_Stats.windowStartNs >= 1'000'000'000ull)
	{
		m_Stats.fps = static_cast<double>(m_Stats.windowFrames) * 1e9 / static_cast<double>(l_Now - m_Stats.windowStartNs);
		m_Stats.windowFrames = 0;
		m_Stats.windowStartNs = l_Now;
	}
}

// ------------------------------------------------------------------------------------------------ UI

void App::buildUi()
{
	buildToolbar();
	if (m_ShowDebug)
		buildDebugPanel();
}

// Temporary toolbar; replaced by the real UI (wb.ui) in a later milestone
void App::buildToolbar()
{
	const ImGuiViewport* l_Viewport = ImGui::GetMainViewport();
	const float l_Scale = m_Window.displayScale();
	ImGui::SetNextWindowPos(ImVec2(l_Viewport->WorkPos.x + l_Viewport->WorkSize.x * 0.5f, l_Viewport->WorkPos.y + l_Viewport->WorkSize.y - 16.f * l_Scale), ImGuiCond_Always, ImVec2(0.5f, 1.f));
	ImGui::SetNextWindowBgAlpha(0.9f);
	constexpr ImGuiWindowFlags l_Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
	if (!ImGui::Begin("##toolbar", nullptr, l_Flags))
	{
		ImGui::End();
		return;
	}

	tools::BrushState& l_Brush = m_Editor.brush();
	const float l_Swatch = 22.f * l_Scale;
	for (size_t i = 0; i < PALETTE.size(); ++i)
	{
		const Color l_Color = Color::fromRgba8(PALETTE[i]);
		const bool l_Selected = l_Color == l_Brush.color;
		ImGui::PushID(static_cast<int>(i));
		if (l_Selected)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f * l_Scale);
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.6f, 1.f, 1.f));
		}
		if (ImGui::ColorButton("##color", toImVec4(l_Color), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(l_Swatch, l_Swatch)))
			l_Brush.color = l_Color;
		if (l_Selected)
		{
			ImGui::PopStyleColor();
			ImGui::PopStyleVar();
		}
		ImGui::PopID();
		ImGui::SameLine();
	}

	ImGui::SameLine(0.f, 16.f * l_Scale);
	ImGui::SetNextItemWidth(140.f * l_Scale);
	ImGui::SliderFloat("##size", &l_Brush.sizePoints, 0.5f, 40.f, "size %.1f", ImGuiSliderFlags_Logarithmic);

	ImGui::SameLine(0.f, 16.f * l_Scale);
	ImGui::BeginDisabled(!m_Editor.history().canUndo());
	if (ImGui::Button("Undo"))
		m_Editor.undo();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_Editor.history().canRedo());
	if (ImGui::Button("Redo"))
		m_Editor.redo();
	ImGui::EndDisabled();

	ImGui::SameLine(0.f, 16.f * l_Scale);
	if (ImGui::Button("-"))
		m_Editor.zoomAroundCenter(1.0 / 1.25);
	ImGui::SameLine();
	if (ImGui::Button((std::to_string(static_cast<int>(std::round(m_Editor.camera().zoom() * 100.0))) + "%###zoom").c_str()))
		m_Editor.resetZoom();
	ImGui::SameLine();
	if (ImGui::Button("+"))
		m_Editor.zoomAroundCenter(1.25);
	ImGui::SameLine();
	if (ImGui::Button("Fit"))
		m_Editor.fitContent();

	ImGui::End();
}

void App::buildDebugPanel()
{
	const float l_Scale = m_Window.displayScale();
	ImGui::SetNextWindowPos(ImVec2(12.f * l_Scale, 12.f * l_Scale), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(400.f * l_Scale, 0.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Debug (F3)", &m_ShowDebug))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("%.0f fps (frames are drawn only on change)", m_Stats.fps);
	ImGui::Text("CPU %.2f ms | GPU %.3f ms", m_Stats.cpuFrameMs, m_Frames.gpuFrameMs());
	ImGui::Text("Frames drawn: %llu", static_cast<unsigned long long>(m_Stats.frameCount));

	// Validation status is always visible: it must stay clean
	const gfx::ValidationStats l_Validation = gfx::GraphicsContext::validationStats();
	if (m_Context.validationEnabled())
	{
		const bool l_Clean = l_Validation.errors == 0 && l_Validation.warnings == 0;
		ImGui::TextColored(l_Clean ? ImVec4(0.4f, 0.9f, 0.4f, 1.f) : ImVec4(1.f, 0.4f, 0.4f, 1.f), "Validation: %u errors, %u warnings", l_Validation.errors, l_Validation.warnings);
	}
	else
	{
		ImGui::TextDisabled("Validation: off");
	}

	if (ImGui::CollapsingHeader("Brush feel", ImGuiTreeNodeFlags_DefaultOpen))
	{
		BrushSettings& l_Settings = m_Editor.brushSettings();
		ImGui::SliderFloat("Smoothing min cutoff (Hz)", &l_Settings.smoothingMinCutoff, 0.2f, 20.f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SetItemTooltip("Lower = steadier lines when drawing slowly, but more lag");
		ImGui::SliderFloat("Smoothing speed beta", &l_Settings.smoothingBeta, 0.f, 0.2f, "%.4f");
		ImGui::SetItemTooltip("Higher = less lag when drawing fast");
		ImGui::SliderFloat("Pressure smoothing", &l_Settings.pressureSmoothing, 0.f, 0.9f);
		ImGui::SliderFloat("Pressure gamma", &l_Settings.pressureGamma, 0.2f, 3.f);
		ImGui::SliderFloat("Min width (fraction)", &l_Settings.minWidthFraction, 0.f, 1.f);
		ImGui::Checkbox("Simulate pressure for mouse", &l_Settings.simulatePressureForMouse);
		ImGui::SliderFloat("Taper start (px)", &l_Settings.taperStartPx, 0.f, 60.f);
		ImGui::SliderFloat("Taper end (px)", &l_Settings.taperEndPx, 0.f, 60.f);
		ImGui::SliderFloat("Resample spacing (px)", &l_Settings.resampleSpacingPx, 0.5f, 6.f);
		ImGui::SliderFloat("Simplify tolerance (px)", &l_Settings.simplifyTolerancePx, 0.f, 1.f);
		if (ImGui::Button("Reset brush settings"))
			l_Settings = BrushSettings{};
	}

	if (ImGui::CollapsingHeader("Canvas", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const Camera& l_Camera = m_Editor.camera();
		const render::CanvasStats& l_Canvas = m_Canvas.stats();
		ImGui::Text("Objects %zu | GPU strokes %u | visible %u", m_Editor.document().size(), l_Canvas.gpuStrokes, l_Canvas.visibleStrokes);
		ImGui::Text("Segments drawn %llu | uploads this frame %u", static_cast<unsigned long long>(l_Canvas.drawnSegments), l_Canvas.uploadsThisFrame);
		ImGui::Text("Point pool %llu / %llu", static_cast<unsigned long long>(l_Canvas.poolPointsUsed), static_cast<unsigned long long>(l_Canvas.poolPointsCapacity));
		ImGui::Text("Camera (%.2f, %.2f) zoom %.4f", l_Camera.center().x, l_Camera.center().y, l_Camera.zoom());
		ImGui::Text("Undo %zu | redo %zu", m_Editor.history().undoCount(), m_Editor.history().redoCount());
		ImGui::Checkbox("Grid", &m_ShowGrid);
		ImGui::SameLine();
		ImGui::Checkbox("Dark theme", &m_DarkTheme);
		if (ImGui::Button("Add 1k strokes"))
			addStressStrokes(1000);
		ImGui::SameLine();
		if (ImGui::Button("Add 10k strokes"))
			addStressStrokes(10000);
	}

	if (ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const platform::InputStats& l_Stats = m_Input.stats();
		ImGui::Text("Last device: %s | pointer events %llu", deviceName(l_Stats.lastDevice), static_cast<unsigned long long>(l_Stats.pointerEvents));
		ImGui::Text("Dropped synthetic mouse events: %llu", static_cast<unsigned long long>(l_Stats.droppedSyntheticMouse));
		if (m_LastPointer)
		{
			const platform::PointerEvent& l_Pointer = *m_LastPointer;
			ImGui::Text("Last: %s %s at (%.1f, %.1f) buttons 0x%X", deviceName(l_Pointer.device), phaseName(l_Pointer.phase), static_cast<double>(l_Pointer.position.x), static_cast<double>(l_Pointer.position.y), l_Pointer.buttons);
		}

		ImGui::SeparatorText("Pen");
		const platform::PenState& l_Pen = m_Input.penState();
		if (l_Pen.eventCount == 0)
		{
			ImGui::TextWrapped("No pen events received yet. For Wacom/Huion/XP-Pen tablets, enable \"Windows Ink\" in the tablet driver settings.");
		}
		else
		{
			ImGui::Text("Pen %u | %s%s%s", static_cast<unsigned>(l_Pen.id), l_Pen.inProximity ? "in range" : "out of range", l_Pen.down ? " | DOWN" : "", l_Pen.eraser ? " | ERASER" : "");
			ImGui::ProgressBar(l_Pen.pressure, ImVec2(-1.f, 0.f), nullptr);
			ImGui::Text("pressure %.3f | tilt (%.1f, %.1f) | rotation %.1f", static_cast<double>(l_Pen.pressure), static_cast<double>(l_Pen.tilt.x), static_cast<double>(l_Pen.tilt.y), static_cast<double>(l_Pen.rotation));
			ImGui::Text("buttons 0x%X | %.0f events/s | %llu total", l_Pen.buttons, l_Pen.eventsPerSecond, static_cast<unsigned long long>(l_Pen.eventCount));
		}
	}

	if (ImGui::CollapsingHeader("GPU"))
	{
		const gfx::DeviceInfo& l_Info = m_Context.info();
		ImGui::TextWrapped("%s (%s)", l_Info.name.c_str(), gpuTypeName(l_Info.type));
		ImGui::TextWrapped("Driver: %s", l_Info.driver.c_str());
		ImGui::Text("Vulkan %u.%u.%u | VRAM %.1f GiB", VK_API_VERSION_MAJOR(l_Info.apiVersion), VK_API_VERSION_MINOR(l_Info.apiVersion), VK_API_VERSION_PATCH(l_Info.apiVersion), static_cast<double>(l_Info.deviceLocalBytes) / (1024.0 * 1024.0 * 1024.0));
		ImGui::Text("Staging memory %.1f MiB", static_cast<double>(m_Staging.allocatedBytes()) / (1024.0 * 1024.0));
	}

	if (ImGui::CollapsingHeader("Display"))
	{
		const VkExtent2D l_Extent = m_Swapchain.extent();
		ImGui::Text("Swapchain %ux%u, %u images", l_Extent.width, l_Extent.height, m_Swapchain.imageCount());
		ImGui::Text("Format %s | %s", gfx::toString(m_Swapchain.format()), gfx::toString(m_Swapchain.presentMode()));
		ImGui::Text("Pixel density %.2f | display scale %.2f", m_Window.pixelDensity(), m_Window.displayScale());
		int l_Policy = static_cast<int>(m_PresentPolicy);
		if (ImGui::Combo("Present", &l_Policy, "VSync (FIFO)\0Low latency (MAILBOX)\0Immediate\0"))
		{
			m_PresentPolicy = static_cast<gfx::PresentPolicy>(l_Policy);
			m_SwapchainDirty = true;
		}
	}

	ImGui::End();
}

// Random scribbles around the current view, for performance testing (one undo step)
void App::addStressStrokes(const uint32_t p_Count)
{
	addStressStrokes(p_Count, m_Editor.camera().visibleWorldRect());
}

void App::addStressStrokes(const uint32_t p_Count, const Rect& p_Area)
{
	std::mt19937 l_Rng(static_cast<uint32_t>(SDL_GetTicksNS()));
	std::uniform_real_distribution<double> l_Unit(0.0, 1.0);
	const Camera& l_Camera = m_Editor.camera();
	const Rect& l_View = p_Area;
	const DVec2 l_Size = l_View.size();
	Document& l_Document = m_Editor.document();

	std::vector<std::unique_ptr<Object>> l_Objects;
	l_Objects.reserve(p_Count);
	for (uint32_t i = 0; i < p_Count; ++i)
	{
		StrokeData l_Stroke{};
		l_Stroke.style.color = Color::fromRgba8(PALETTE[static_cast<size_t>(l_Unit(l_Rng) * static_cast<double>(PALETTE.size())) % PALETTE.size()]);
		const float l_Radius = static_cast<float>((0.5 + l_Unit(l_Rng) * 3.0) / l_Camera.zoom());
		Vec2 l_Position{ 0.f };
		float l_Angle = static_cast<float>(l_Unit(l_Rng) * 2.0 * PI);
		const int l_Points = 40 + static_cast<int>(l_Unit(l_Rng) * 160.0);
		for (int k = 0; k < l_Points; ++k)
		{
			l_Stroke.points.push_back(StrokePoint{ .position = l_Position, .radius = l_Radius * static_cast<float>(0.6 + 0.4 * std::sin(k * 0.1)) });
			l_Angle += static_cast<float>((l_Unit(l_Rng) - 0.5) * 0.6);
			l_Position += Vec2{ std::cos(l_Angle), std::sin(l_Angle) } * static_cast<float>(3.0 / l_Camera.zoom());
		}

		auto l_Object = std::make_unique<Object>();
		l_Object->id = l_Document.allocateId();
		l_Object->transform = Affine2::translate(l_View.min + DVec2{ l_Unit(l_Rng) * l_Size.x, l_Unit(l_Rng) * l_Size.y });
		l_Object->payload = std::move(l_Stroke);
		l_Objects.push_back(std::move(l_Object));
	}
	m_Editor.history().execute(l_Document, std::make_unique<AddObjectsCommand>(std::move(l_Objects), "Stress test"));
}
} // namespace wb
