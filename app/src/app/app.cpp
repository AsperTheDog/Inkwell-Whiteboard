module;
#include <algorithm>
#include <cstdint>
#include <optional>
#include <variant>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include "gfx/vk_check.hpp"

module wb.app;

import wb.debug.imgui_layer;
import wb.gfx.commands;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.swapchain;
import wb.math;
import wb.platform.input;
import wb.platform.window;

namespace wb
{
namespace
{
// Canvas background per theme (UI theming proper arrives with wb.ui)
constexpr Color LIGHT_CANVAS = Color::fromRgba8(0xF5F5F2FFu);
constexpr Color DARK_CANVAS = Color::fromRgba8(0x1F2023FFu);

// While idle with the debug overlay open, refresh its numbers at this interval
constexpr uint64_t DEBUG_REFRESH_NS = 250'000'000ull;

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

			const bool l_CanRender = !m_Window.isMinimized() && !m_Window.pixelSize().isZero();
			if (!pumpEvents(!l_CanRender || !needsRedraw()))
				break;
			if (!l_CanRender || m_Window.isMinimized() || m_Window.pixelSize().isZero())
				continue;

			if (m_SwapchainDirty)
				recreateSwapchain();
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
	m_ImGui.init(m_Context, m_Window, m_Swapchain);
#ifdef WB_DEBUG
	m_ImGui.setVisible(true);
#endif
	m_Stats.windowStartNs = SDL_GetTicksNS();
}

void App::shutdown()
{
	m_Context.waitIdle();
	m_ImGui.shutdown();
	m_Frames.destroy(m_Context);
	m_Swapchain.destroy(m_Context);
	m_Context.destroy();
	m_Window.destroy();
}

bool App::pumpEvents(const bool p_Block)
{
	SDL_Event l_Event;
	if (p_Block)
	{
		// Sleep until something happens. With the overlay open, wake up periodically to refresh its stats.
		const int32_t l_Timeout = m_ImGui.isVisible() ? static_cast<int32_t>(DEBUG_REFRESH_NS / 1'000'000ull) : 1000;
		if (SDL_WaitEventTimeout(&l_Event, l_Timeout))
			handleEvent(l_Event);
	}
	while (SDL_PollEvent(&l_Event))
		handleEvent(l_Event);

	if (m_ImGui.isVisible())
	{
		const uint64_t l_Now = SDL_GetTicksNS();
		if (l_Now - m_LastDebugRefreshNs >= DEBUG_REFRESH_NS)
		{
			m_LastDebugRefreshNs = l_Now;
			m_RedrawFrames = std::max(m_RedrawFrames, 1u);
		}
	}
	return m_Running;
}

void App::handleEvent(const SDL_Event& p_Event)
{
	switch (p_Event.type)
	{
	case SDL_EVENT_QUIT:
	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		m_Running = false;
		return;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		m_SwapchainDirty = true;
		m_RedrawFrames = 2;
		return;
	case SDL_EVENT_WINDOW_EXPOSED:
	case SDL_EVENT_WINDOW_RESTORED:
	case SDL_EVENT_WINDOW_MAXIMIZED:
		m_RedrawFrames = std::max(m_RedrawFrames, 1u);
		return;
	case SDL_EVENT_KEY_DOWN:
		if (p_Event.key.key == SDLK_F3 && !p_Event.key.repeat)
		{
			m_ImGui.toggle();
			m_RedrawFrames = 2;
			return;
		}
		if (p_Event.key.key == SDLK_F11 && !p_Event.key.repeat)
		{
			const bool l_Fullscreen = (SDL_GetWindowFlags(m_Window.handle()) & SDL_WINDOW_FULLSCREEN) != 0;
			SDL_SetWindowFullscreen(m_Window.handle(), !l_Fullscreen);
			return;
		}
		break;
	default:
		break;
	}

	if (m_ImGui.processEvent(p_Event))
	{
		m_RedrawFrames = 2;
		return;
	}

	if (const std::optional<platform::InputEvent> l_Input = m_Input.translate(p_Event, m_Window.pixelDensity()))
		handleInput(*l_Input);
	else if (m_ImGui.isVisible() && (p_Event.type == SDL_EVENT_PEN_AXIS || p_Event.type == SDL_EVENT_PEN_PROXIMITY_IN || p_Event.type == SDL_EVENT_PEN_PROXIMITY_OUT))
		m_RedrawFrames = 2; // keep the pen diagnostics live
}

void App::handleInput(const platform::InputEvent& p_Event)
{
	if (const platform::PointerEvent* l_Pointer = std::get_if<platform::PointerEvent>(&p_Event))
		m_LastPointer = *l_Pointer;

	// Tools arrive in M1; for now any input only refreshes the screen
	m_RedrawFrames = 2;
}

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
	m_RedrawFrames = std::max(m_RedrawFrames, 1u);
}

void App::renderFrame()
{
	const uint64_t l_CpuStart = SDL_GetTicksNS();

	m_ImGui.buildFrame([this] { buildDebugUi(); });

	gfx::FrameStatus l_Status = gfx::FrameStatus::Ok;
	const std::optional<gfx::FrameTarget> l_Target = m_Frames.begin(m_Context, m_Swapchain, l_Status);
	if (!l_Target)
	{
		m_SwapchainDirty = true;
		return;
	}

	const VkCommandBuffer l_Cmd = l_Target->cmd;
	m_Frames.writeStartTimestamp(l_Cmd);

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
		m_ImGui.record(l_Cmd);
		vkCmdEndRendering(l_Cmd);
	}

	gfx::transitionImage(l_Cmd, l_Target->image, {
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.dstStage = VK_PIPELINE_STAGE_2_NONE,
		.dstAccess = VK_ACCESS_2_NONE,
	});

	m_Frames.writeEndTimestamp(l_Cmd);
	if (m_Frames.end(m_Context, m_Swapchain, *l_Target) == gfx::FrameStatus::SwapchainOutOfDate)
		m_SwapchainDirty = true;

	if (m_RedrawFrames > 0)
		--m_RedrawFrames;
	if (m_Options.smokeTestFrames > 0 && m_Stats.frameCount + 1 >= m_Options.smokeTestFrames)
		m_Running = false;

	// Stats
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

void App::buildDebugUi()
{
	const float l_Scale = m_Window.displayScale();
	ImGui::SetNextWindowPos(ImVec2(12.f * l_Scale, 12.f * l_Scale), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(380.f * l_Scale, 0.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Debug (F3)"))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("%.0f fps (frames drawn only on change)", m_Stats.fps);
	ImGui::Text("CPU %.2f ms | GPU %.3f ms", m_Stats.cpuFrameMs, m_Frames.gpuFrameMs());
	ImGui::Text("Frames drawn: %llu", static_cast<unsigned long long>(m_Stats.frameCount));

	if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const gfx::DeviceInfo& l_Info = m_Context.info();
		ImGui::TextWrapped("%s (%s)", l_Info.name.c_str(), gpuTypeName(l_Info.type));
		ImGui::TextWrapped("Driver: %s", l_Info.driver.c_str());
		ImGui::Text("Vulkan %u.%u.%u | VRAM %.1f GiB", VK_API_VERSION_MAJOR(l_Info.apiVersion), VK_API_VERSION_MINOR(l_Info.apiVersion), VK_API_VERSION_PATCH(l_Info.apiVersion), static_cast<double>(l_Info.deviceLocalBytes) / (1024.0 * 1024.0 * 1024.0));
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
	}

	if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
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
		ImGui::Checkbox("Dark theme", &m_DarkTheme);
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
			ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::Text("pressure %.3f", static_cast<double>(l_Pen.pressure));
			ImGui::Text("Tilt (%.1f, %.1f) | rotation %.1f | buttons 0x%X", static_cast<double>(l_Pen.tilt.x), static_cast<double>(l_Pen.tilt.y), static_cast<double>(l_Pen.rotation), l_Pen.buttons);
			ImGui::Text("%.0f events/s | %llu total", l_Pen.eventsPerSecond, static_cast<unsigned long long>(l_Pen.eventCount));
		}
	}

	ImGui::End();
}
} // namespace wb
