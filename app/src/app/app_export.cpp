// App: export the board (or the selection) as a PNG. The board is drawn once more into an image of the wanted size
// in the middle of a normal frame, read back, and encoded on a worker thread.
module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <stb_image_write.h>
#include <volk.h>

module wb.app;

import wb.editor;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.gfx.image;
import wb.io.file;
import wb.math;
import wb.render.canvas_renderer;
import wb.session;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;
import wb.view.camera;

namespace wb
{
namespace
{
using ui::Rect2;

constexpr uint64_t MAX_EXPORT_PIXELS = 64ull << 20; // 256 MB of pixels
constexpr uint32_t MAX_EXPORT_SIDE = 16384;
constexpr std::array<double, 4> SCALES{ 1.0, 2.0, 3.0, 4.0 };
constexpr uint32_t WAIT_FRAMES_FOR_PICTURES = 600; // give up waiting for pictures to load after this many frames

void appendPng(void* p_Context, void* p_Data, const int p_Size)
{
	auto* l_Out = static_cast<std::vector<uint8_t>*>(p_Context);
	const auto* l_Bytes = static_cast<const uint8_t*>(p_Data);
	l_Out->insert(l_Out->end(), l_Bytes, l_Bytes + p_Size);
}
} // namespace

// ------------------------------------------------------------------------------------------------ plan

App::ExportPlan App::exportPlan() const
{
	ExportPlan l_Plan;
	Rect l_Region{};
	double l_Margin = 0.0;
	if (m_ExportOptions.selectionOnly && m_Editor.hasSelection())
	{
		l_Region = m_Editor.selection().bounds();
		l_Margin = 12.0;
	}
	else
	{
		l_Region = m_Editor.document().contentBounds();
		l_Margin = 32.0;
	}
	if (l_Region.isEmpty())
		return l_Plan;
	l_Region.min -= DVec2{ l_Margin };
	l_Region.max += DVec2{ l_Margin };
	const DVec2 l_Size = glm::max(l_Region.size(), DVec2{ 1.0 });

	const double l_Wanted = SCALES[static_cast<size_t>(std::clamp(m_ExportOptions.scale, 0, static_cast<int>(SCALES.size()) - 1))];
	const double l_MaxSide = std::min<double>(MAX_EXPORT_SIDE, m_Context.info().limits.maxImageDimension2D);
	double l_Scale = l_Wanted;
	l_Scale = std::min(l_Scale, l_MaxSide / std::max(l_Size.x, l_Size.y));
	l_Scale = std::min(l_Scale, std::sqrt(static_cast<double>(MAX_EXPORT_PIXELS) / (l_Size.x * l_Size.y)));
	l_Scale = std::clamp(l_Scale, Camera::MIN_ZOOM, Camera::MAX_ZOOM);

	l_Plan.region = l_Region;
	l_Plan.scale = l_Scale;
	l_Plan.extent = VkExtent2D{ std::max(1u, static_cast<uint32_t>(std::ceil(l_Size.x * l_Scale))), std::max(1u, static_cast<uint32_t>(std::ceil(l_Size.y * l_Scale))) };
	l_Plan.reduced = l_Scale < l_Wanted * 0.999;
	l_Plan.valid = static_cast<uint64_t>(l_Plan.extent.width) * l_Plan.extent.height <= MAX_EXPORT_PIXELS && l_Plan.extent.width <= MAX_EXPORT_SIDE && l_Plan.extent.height <= MAX_EXPORT_SIDE;
	return l_Plan;
}

// ------------------------------------------------------------------------------------------------ dialog

void App::openExportDialog()
{
	if (m_Editor.document().empty())
	{
		showToast("The board is empty, there is nothing to export");
		return;
	}
	m_ExportOptions.selectionOnly = m_Editor.hasSelection();
	m_ShowExport = true;
	closePopup("export dialog");
	requestRedraw();
}

void App::buildExportDialog()
{
	if (!m_ShowExport)
	{
		m_Ui.setAnim(ui::Context::id("dialog.export", 1), 0.f);
		return;
	}
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Fade = m_Ui.animFrom(ui::Context::id("dialog.export", 1), 0.f, 1.f, 20.f);

	const Rect2 l_Screen{ Vec2{ 0.f }, l_Viewport };
	m_Ui.beginPanel(l_Screen);
	l_Draw.setOpacity(l_Fade);
	l_Draw.rect(l_Screen, 0.f, l_Theme.scrim);

	const bool l_HasSelection = m_Editor.hasSelection();
	if (!l_HasSelection)
		m_ExportOptions.selectionOnly = false;
	const ExportPlan l_Plan = exportPlan();

	const float l_Pad = m_Ui.px(26.f);
	const float l_Width = std::min(m_Ui.px(440.f), l_Viewport.x - m_Ui.px(32.f));
	const float l_Inner = l_Width - l_Pad * 2.f;
	const float l_RowLabel = m_Ui.px(22.f);
	const float l_Control = m_Ui.px(40.f);
	const float l_Gap = m_Ui.px(16.f);
	const float l_Height = l_Pad + m_Ui.px(26.f) + m_Ui.px(14.f) + 3.f * (l_RowLabel + l_Control + l_Gap) + m_Ui.px(22.f) + m_Ui.px(8.f) + m_Ui.px(40.f) + l_Pad;
	const float l_Lift = (1.f - l_Fade) * m_Ui.px(10.f);
	const Rect2 l_Card = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, (l_Viewport.y - l_Height) * 0.5f + l_Lift }, Vec2{ l_Width, l_Height });
	m_Ui.panel(l_Card, m_Ui.px(22.f));

	const float l_Left = l_Card.min.x + l_Pad;
	float l_Y = l_Card.min.y + l_Pad;
	m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(13.f) }, "Export as PNG", 18.f, l_Theme.text);
	l_Y += m_Ui.px(26.f) + m_Ui.px(14.f);

	const auto l_Row = [&](const char* p_Key, const char* p_Title, const std::span<const std::string_view> p_Labels, int& p_Value)
	{
		m_Ui.label(Vec2{ l_Left, l_Y + l_RowLabel * 0.5f }, p_Title, 13.f, l_Theme.textMuted);
		l_Y += l_RowLabel;
		static constexpr std::array<ui::Icon, 4> NO_ICONS{ ui::Icon::None, ui::Icon::None, ui::Icon::None, ui::Icon::None };
		m_Ui.segmented(p_Key, Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_Control }), p_Labels, std::span<const ui::Icon>(NO_ICONS).first(p_Labels.size()), p_Value);
		l_Y += l_Control + l_Gap;
	};

	static constexpr std::array<std::string_view, 2> AREA_LABELS{ "Whole board", "Selection" };
	int l_Area = m_ExportOptions.selectionOnly ? 1 : 0;
	if (l_HasSelection)
	{
		l_Row("export.area", "Area", AREA_LABELS, l_Area);
	}
	else
	{
		m_Ui.label(Vec2{ l_Left, l_Y + l_RowLabel * 0.5f }, "Area", 13.f, l_Theme.textMuted);
		l_Y += l_RowLabel;
		m_Ui.label(Vec2{ l_Left, l_Y + l_Control * 0.5f }, "Everything on the board", 14.f, l_Theme.text);
		l_Y += l_Control + l_Gap;
	}
	m_ExportOptions.selectionOnly = l_Area == 1;

	static constexpr std::array<std::string_view, 4> SCALE_LABELS{ "1x", "2x", "3x", "4x" };
	l_Row("export.scale", "Resolution", SCALE_LABELS, m_ExportOptions.scale);

	static constexpr std::array<std::string_view, 2> BACKGROUND_LABELS{ "Board colour", "Transparent" };
	int l_Background = m_ExportOptions.transparent ? 1 : 0;
	l_Row("export.background", "Background", BACKGROUND_LABELS, l_Background);
	m_ExportOptions.transparent = l_Background == 1;

	std::string l_Info = "Nothing to export";
	if (l_Plan.valid)
	{
		l_Info = std::to_string(l_Plan.extent.width) + " x " + std::to_string(l_Plan.extent.height) + " pixels";
		if (l_Plan.reduced)
			l_Info += " (lowered to fit)";
	}
	l_Draw.text(Vec2{ l_Left, l_Y + m_Ui.px(11.f) }, l_Info, m_Ui.fontPx(13.f), l_Theme.textMuted);
	l_Y += m_Ui.px(22.f) + m_Ui.px(8.f);

	const float l_ButtonWidth = m_Ui.px(110.f);
	const Rect2 l_Export = Rect2::fromPosSize(Vec2{ l_Card.max.x - l_Pad - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, m_Ui.px(40.f) });
	const Rect2 l_Cancel = Rect2::fromPosSize(Vec2{ l_Export.min.x - m_Ui.px(10.f) - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, m_Ui.px(40.f) });
	if (m_Ui.button("export.cancel", l_Cancel, "Cancel", ui::ButtonStyle::Secondary))
		m_ShowExport = false;
	if (m_Ui.button("export.go", l_Export, "Export...", ui::ButtonStyle::Primary, l_Plan.valid))
	{
		m_ShowExport = false;
		showExportSaveDialog();
	}
	l_Draw.setOpacity(1.f);
}

void App::showExportSaveDialog()
{
	if (m_DialogKind != DialogKind::None)
		return;
	static const SDL_DialogFileFilter s_Filters[] = { { "PNG pictures", "png" } };
	m_DialogKind = DialogKind::ExportPng;
	const std::string l_Name = m_Session.displayName() + ".png";
	std::string l_Start = l_Name;
	if (!m_LastDirectory.empty())
		l_Start = pathToUtf8(pathFromUtf8(m_LastDirectory) / pathFromUtf8(l_Name));
	SDL_ShowSaveFileDialog(&App::dialogCallback, this, m_Window.handle(), s_Filters, 1, l_Start.c_str());
}

// ------------------------------------------------------------------------------------------------ rendering

void App::startExport(const std::filesystem::path& p_Path)
{
	const ExportPlan l_Plan = exportPlan();
	if (!l_Plan.valid)
	{
		showMessage("There is nothing to export.");
		return;
	}
	if (m_ExportJob || m_ExportWrite.valid())
	{
		showToast("Another export is still running");
		return;
	}
	ExportJob l_Job;
	l_Job.path = p_Path;
	if (!l_Job.path.has_extension())
		l_Job.path += ".png";
	l_Job.plan = l_Plan;
	l_Job.transparent = m_ExportOptions.transparent;
	l_Job.background = m_Theme.canvas;
	if (m_ExportOptions.selectionOnly && m_Editor.hasSelection())
	{
		l_Job.onlySelected = true;
		l_Job.only.insert(m_Editor.selection().ids().begin(), m_Editor.selection().ids().end());
	}
	m_ExportJob = std::move(l_Job);
	requestRedraw(4);
}

// True when the frame in progress should also draw the export image (pictures have to be loaded first)
bool App::exportReady()
{
	if (!m_ExportJob)
		return false;
	if (m_Canvas.images().busy() && m_ExportJob->waited < WAIT_FRAMES_FOR_PICTURES)
	{
		++m_ExportJob->waited;
		requestRedraw(2);
		return false;
	}
	return true;
}

void App::recordExport(const VkCommandBuffer p_Cmd)
{
	ExportJob& l_Job = *m_ExportJob;
	const VkExtent2D l_Extent = l_Job.plan.extent;
	const VkDeviceSize l_Bytes = static_cast<VkDeviceSize>(l_Extent.width) * l_Extent.height * 4;

	Camera l_Camera;
	l_Camera.setViewport(Vec2{ static_cast<float>(l_Extent.width), static_cast<float>(l_Extent.height) }, 1.f);
	l_Camera.setZoom(l_Job.plan.scale);
	l_Camera.setCenter(l_Job.plan.region.center());

	m_Canvas.setExportMode(l_Job.onlySelected ? &l_Job.only : nullptr, true);
	m_Canvas.prepare(m_Context, m_Frames, m_Staging, p_Cmd, gfx::EXPORT_SLOT, l_Camera, nullptr, imageClock());
	m_Canvas.setExportMode(nullptr, false);

	m_ExportImage = gfx::createImage2D(m_Context, l_Extent, m_Swapchain.format(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "export");
	m_ExportBuffer = gfx::createBuffer(m_Context, l_Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryKind::Readback, "export pixels");

	const gfx::DebugLabel l_Label(p_Cmd, "export");
	gfx::transitionImage(p_Cmd, m_ExportImage.handle, {
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccess = VK_ACCESS_2_NONE,
		.dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	});
	const Color l_Clear = l_Job.transparent ? Color{ 0.f, 0.f, 0.f, 0.f } : Color{ l_Job.background.r, l_Job.background.g, l_Job.background.b, 1.f };
	gfx::beginColorRendering(p_Cmd, m_ExportImage.view, l_Extent, VK_ATTACHMENT_LOAD_OP_CLEAR, VkClearColorValue{ .float32 = { l_Clear.r, l_Clear.g, l_Clear.b, l_Clear.a } });
	m_Canvas.record(p_Cmd, l_Camera, render::GridStyle{ .enabled = false });
	vkCmdEndRendering(p_Cmd);

	gfx::transitionImage(p_Cmd, m_ExportImage.handle, {
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
	});
	const VkBufferImageCopy l_Region{
		.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
		.imageExtent = { l_Extent.width, l_Extent.height, 1 },
	};
	vkCmdCopyImageToBuffer(p_Cmd, m_ExportImage.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_ExportBuffer.handle, 1, &l_Region);
	gfx::memoryBarrier(p_Cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	l_Job.recorded = true;
}

// After the frame that drew the export was submitted
void App::finishExport()
{
	if (!m_ExportJob || !m_ExportJob->recorded)
		return;
	ExportJob l_Job = std::move(*m_ExportJob);
	m_ExportJob.reset();

	m_Context.waitIdle();
	const VkExtent2D l_Extent = l_Job.plan.extent;
	const size_t l_Pixels = static_cast<size_t>(l_Extent.width) * l_Extent.height;
	std::vector<uint8_t> l_Rgba(l_Pixels * 4);
	std::memcpy(l_Rgba.data(), m_ExportBuffer.mapped, l_Rgba.size());
	gfx::destroyBuffer(m_Context, m_ExportBuffer);
	gfx::destroyImage(m_Context, m_ExportImage);

	const VkFormat l_Format = m_Swapchain.format();
	const bool l_Bgra = l_Format == VK_FORMAT_B8G8R8A8_UNORM || l_Format == VK_FORMAT_B8G8R8A8_SRGB;
	m_ExportWriteName = pathToUtf8(l_Job.path.filename());
	m_ExportWrite = std::async(std::launch::async, [l_Job = std::move(l_Job), l_Pixels, l_Bgra, l_Rgba = std::move(l_Rgba), l_Extent]() mutable -> std::string
	{
		uint8_t* l_Data = l_Rgba.data();
		for (size_t i = 0; i < l_Pixels; ++i)
		{
			uint8_t* l_Pixel = l_Data + i * 4;
			if (l_Bgra)
				std::swap(l_Pixel[0], l_Pixel[2]);
			if (!l_Job.transparent)
			{
				l_Pixel[3] = 255;
			}
			else if (l_Pixel[3] != 0 && l_Pixel[3] != 255)
			{
				// The board is drawn premultiplied; PNG is not
				const unsigned l_Alpha = l_Pixel[3];
				for (int c = 0; c < 3; ++c)
					l_Pixel[c] = static_cast<uint8_t>(std::min(255u, (l_Pixel[c] * 255u + l_Alpha / 2) / l_Alpha));
			}
		}
		std::vector<uint8_t> l_Png;
		if (stbi_write_png_to_func(&appendPng, &l_Png, static_cast<int>(l_Extent.width), static_cast<int>(l_Extent.height), 4, l_Data, static_cast<int>(l_Extent.width * 4)) == 0)
			return "The picture could not be encoded.";
		if (const IoResult l_Result = writeFileAtomic(l_Job.path, l_Png); !l_Result.ok)
			return l_Result.error;
		return {};
	});
	showToast("Exporting " + m_ExportWriteName + "...");
}

// Once per loop: reports a finished export
void App::pollExport()
{
	if (!m_ExportWrite.valid() || m_ExportWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	const std::string l_Error = m_ExportWrite.get();
	if (l_Error.empty())
		showToast("Exported " + m_ExportWriteName);
	else
		showMessage("The picture could not be saved.\n\n" + l_Error);
}
} // namespace wb
