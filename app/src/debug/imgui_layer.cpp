module;
#include <algorithm>
#include <functional>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include "gfx/vk_check.hpp"

module wb.debug.imgui_layer;

import wb.gfx.context;
import wb.gfx.swapchain;
import wb.platform.window;

namespace wb::debug
{
namespace
{
void checkVkResult(const VkResult p_Result)
{
	if (p_Result < 0)
		throw gfx::VulkanError(p_Result, "ImGui Vulkan backend", __FILE__, __LINE__);
}

bool isPointerEvent(const Uint32 p_Type)
{
	switch (p_Type)
	{
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	case SDL_EVENT_MOUSE_WHEEL:
	case SDL_EVENT_PEN_DOWN:
	case SDL_EVENT_PEN_UP:
	case SDL_EVENT_PEN_MOTION:
	case SDL_EVENT_PEN_BUTTON_DOWN:
	case SDL_EVENT_PEN_BUTTON_UP:
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_MOTION:
		return true;
	default:
		return false;
	}
}

bool isKeyboardEvent(const Uint32 p_Type)
{
	return p_Type == SDL_EVENT_KEY_DOWN || p_Type == SDL_EVENT_KEY_UP || p_Type == SDL_EVENT_TEXT_INPUT || p_Type == SDL_EVENT_TEXT_EDITING;
}
} // namespace

void ImGuiLayer::init(const gfx::GraphicsContext& p_Context, const platform::Window& p_Window, const gfx::Swapchain& p_Swapchain)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& l_Io = ImGui::GetIO();
	l_Io.IniFilename = nullptr; // nothing worth persisting for a debug overlay
	l_Io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui::StyleColorsDark();
	ImGuiStyle& l_Style = ImGui::GetStyle();
	const float l_Scale = p_Window.displayScale();
	l_Style.ScaleAllSizes(l_Scale);
	l_Style.FontScaleDpi = l_Scale;
	l_Style.WindowRounding = 6.f * l_Scale;
	l_Style.Colors[ImGuiCol_WindowBg].w = 0.92f;

	ImGui_ImplSDL3_InitForVulkan(p_Window.handle());

	const VkFormat l_ColorFormat = p_Swapchain.format();
	ImGui_ImplVulkan_InitInfo l_Info{};
	l_Info.ApiVersion = VK_API_VERSION_1_3;
	l_Info.Instance = p_Context.instance();
	l_Info.PhysicalDevice = p_Context.physicalDevice();
	l_Info.Device = p_Context.device();
	l_Info.QueueFamily = p_Context.queueFamily();
	l_Info.Queue = p_Context.queue();
	l_Info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE * 2;
	l_Info.MinImageCount = std::max(2u, p_Swapchain.minImageCount());
	l_Info.ImageCount = std::max(l_Info.MinImageCount, p_Swapchain.imageCount());
	l_Info.UseDynamicRendering = true;
	l_Info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	l_Info.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &l_ColorFormat,
	};
	l_Info.CheckVkResultFn = checkVkResult;
	if (!ImGui_ImplVulkan_Init(&l_Info))
		throw std::runtime_error("ImGui_ImplVulkan_Init failed");

	m_Initialized = true;
}

void ImGuiLayer::shutdown()
{
	if (!m_Initialized)
		return;
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	m_Initialized = false;
}

bool ImGuiLayer::processEvent(const SDL_Event& p_Event)
{
	if (!m_Initialized)
		return false;

	// Always forward so ImGui's input state stays consistent (key/mouse releases while hidden, focus...)
	ImGui_ImplSDL3_ProcessEvent(&p_Event);
	if (!m_Visible)
		return false;

	if (isPointerEvent(p_Event.type))
		return wantsPointer();
	if (isKeyboardEvent(p_Event.type))
		return wantsKeyboard();
	return false;
}

bool ImGuiLayer::wantsPointer() const
{
	return m_Visible && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wantsKeyboard() const
{
	return m_Visible && ImGui::GetIO().WantCaptureKeyboard;
}

void ImGuiLayer::buildFrame(const std::function<void()>& p_Build)
{
	m_HasDrawData = false;
	if (!m_Visible)
		return;

	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	p_Build();
	ImGui::Render();
	m_HasDrawData = true;
}

void ImGuiLayer::record(const VkCommandBuffer p_Cmd) const
{
	if (!m_HasDrawData)
		return;
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), p_Cmd);
}

void ImGuiLayer::onSwapchainRecreated(const gfx::Swapchain& p_Swapchain) const
{
	if (m_Initialized)
		ImGui_ImplVulkan_SetMinImageCount(std::max(2u, p_Swapchain.minImageCount()));
}
} // namespace wb::debug
