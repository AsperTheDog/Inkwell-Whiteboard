// --smoke-test: a scripted session that exercises the drawing and rendering paths under validation, and
// optionally (--screenshot) saves the final frame so the output can be inspected.
module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <stb_image_write.h>
#include "gfx/vk_check.hpp"

module wb.app;

import wb.editor;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.math;
import wb.platform.input;
import wb.tools.tool;
import wb.view.camera;

namespace wb
{
namespace
{
constexpr uint64_t EVENT_INTERVAL_NS = 4'000'000; // 250 Hz, like a typical pen
constexpr size_t EVENTS_PER_FRAME = 6;

std::vector<Vec2> sampleCurve(const int p_Count, const auto& p_Function)
{
	std::vector<Vec2> l_Points;
	for (int i = 0; i <= p_Count; ++i)
		l_Points.push_back(p_Function(static_cast<float>(i) / static_cast<float>(p_Count)));
	return l_Points;
}
} // namespace

void App::queueStroke(const platform::PointerDevice p_Device, const tools::BrushState& p_Brush, const std::vector<Vec2>& p_Points, const std::vector<float>& p_Pressures)
{
	m_SmokeQueue.emplace_back(p_Brush);
	for (size_t i = 0; i < p_Points.size(); ++i)
	{
		platform::PointerEvent l_Event{
			.phase = i == 0 ? platform::PointerPhase::Down : platform::PointerPhase::Move,
			.device = p_Device,
			.button = i == 0 ? platform::PointerButton::Primary : platform::PointerButton::None,
			.buttons = platform::ButtonMask::Primary,
			.position = p_Points[i],
			.pressure = p_Pressures.empty() ? 1.f : p_Pressures[std::min(i, p_Pressures.size() - 1)],
		};
		m_SmokeQueue.emplace_back(l_Event);
	}
	platform::PointerEvent l_Up{
		.phase = platform::PointerPhase::Up,
		.device = p_Device,
		.button = platform::PointerButton::Primary,
		.position = p_Points.back(),
		.pressure = 0.f,
	};
	m_SmokeQueue.emplace_back(l_Up);
}

void App::driveSmokeTest()
{
	requestRedraw(1);
	if (m_SmokeTimeNs == 0)
		m_SmokeTimeNs = SDL_GetTicksNS();

	// Feed queued input a few events per frame so live strokes are rendered too
	if (!m_SmokeQueue.empty())
	{
		for (size_t i = 0; i < EVENTS_PER_FRAME && !m_SmokeQueue.empty(); ++i)
		{
			auto& l_Item = m_SmokeQueue.front();
			if (platform::PointerEvent* l_Event = std::get_if<platform::PointerEvent>(&l_Item))
			{
				m_SmokeTimeNs += EVENT_INTERVAL_NS;
				l_Event->timestampNs = m_SmokeTimeNs;
				m_Editor.handlePointer(*l_Event);
			}
			else
			{
				m_Editor.brush() = std::get<tools::BrushState>(l_Item);
			}
			m_SmokeQueue.pop_front();
		}
		return;
	}

	const DVec2 l_Viewport = m_Editor.camera().viewport();
	const Vec2 l_C{ static_cast<float>(l_Viewport.x * 0.5), static_cast<float>(l_Viewport.y * 0.5) };
	const float l_S = m_Window.displayScale();

	switch (m_SmokeStep++)
	{
	case 0:
	{
		m_Editor.lookAt(DVec2{ 0.0 }, 1.0);
		// Far-away strokes: exercise pool growth and culling without cluttering the view
		addStressStrokes(3000, Rect::fromPoints(DVec2{ 1e6 }, DVec2{ 1e6 + 5000.0 }));

		// Pressure ramp along a sine wave
		const std::vector<Vec2> l_Sine = sampleCurve(150, [&](const float p_T) { return l_C + Vec2{ (p_T - 0.5f) * 1100.f * l_S, (-260.f + 70.f * std::sin(p_T * 12.f)) * l_S }; });
		std::vector<float> l_Pressures;
		for (size_t i = 0; i < l_Sine.size(); ++i)
			l_Pressures.push_back(std::sin(static_cast<float>(i) / static_cast<float>(l_Sine.size() - 1) * 3.14159f));
		queueStroke(platform::PointerDevice::Pen, { .color = Color::fromRgba8(0x1E88E5FFu), .sizePoints = 10.f }, l_Sine, l_Pressures);

		// Mouse line (constant width) and a dot
		queueStroke(platform::PointerDevice::Mouse, { .color = Color::fromRgba8(0x1F1F1FFFu), .sizePoints = 3.f },
			sampleCurve(60, [&](const float p_T) { return l_C + Vec2{ (p_T - 0.5f) * 1100.f * l_S, (-120.f + 30.f * p_T) * l_S }; }), {});
		queueStroke(platform::PointerDevice::Mouse, { .color = Color::fromRgba8(0xE53935FFu), .sizePoints = 14.f }, { l_C + Vec2{ -560.f * l_S, -40.f * l_S } }, {});

		// Spiral with growing pressure
		const std::vector<Vec2> l_Spiral = sampleCurve(220, [&](const float p_T) {
			const float l_Angle = p_T * 6.f * 3.14159f;
			const float l_Radius = (8.f + 110.f * p_T) * l_S;
			return l_C + Vec2{ -320.f * l_S + std::cos(l_Angle) * l_Radius, 150.f * l_S + std::sin(l_Angle) * l_Radius };
		});
		std::vector<float> l_SpiralPressure;
		for (size_t i = 0; i < l_Spiral.size(); ++i)
			l_SpiralPressure.push_back(0.2f + 0.8f * static_cast<float>(i) / static_cast<float>(l_Spiral.size()));
		queueStroke(platform::PointerDevice::Pen, { .color = Color::fromRgba8(0x5E35B1FFu), .sizePoints = 6.f }, l_Spiral, l_SpiralPressure);

		// Thick zigzag: sharp joins
		std::vector<Vec2> l_Zigzag;
		for (int i = 0; i <= 8; ++i)
		{
			const Vec2 l_A = l_C + Vec2{ (-20.f + static_cast<float>(i) * 70.f) * l_S, (i % 2 == 0 ? 60.f : 230.f) * l_S };
			const Vec2 l_B = l_C + Vec2{ (-20.f + static_cast<float>(i + 1) * 70.f) * l_S, (i % 2 == 0 ? 230.f : 60.f) * l_S };
			for (int k = 0; k < 8; ++k)
				l_Zigzag.push_back(l_A + (l_B - l_A) * (static_cast<float>(k) / 8.f));
		}
		queueStroke(platform::PointerDevice::Pen, { .color = Color::fromRgba8(0xFB8C00FFu), .sizePoints = 18.f }, l_Zigzag, { 1.f });

		// Hairlines: below one pixel they must fade, not vanish or alias
		for (int i = 0; i < 4; ++i)
		{
			const float l_Y = (300.f + static_cast<float>(i) * 14.f) * l_S;
			queueStroke(platform::PointerDevice::Mouse, { .color = Color::fromRgba8(0x43A047FFu), .sizePoints = 0.25f + static_cast<float>(i) * 0.35f },
				sampleCurve(40, [&](const float p_T) { return l_C + Vec2{ (-600.f + p_T * 500.f) * l_S, l_Y + p_T * 40.f * l_S }; }), {});
		}
		return;
	}
	case 1:
		m_Editor.undo();
		return;
	case 2:
		m_Editor.redo();
		return;
	case 3:
	{
		// Segment eraser: a vertical sweep across the sine wave and the mouse line
		m_Editor.setTool(tools::ToolKind::Eraser);
		m_Editor.eraser() = { .mode = tools::EraserMode::Segment, .sizePoints = 24.f };
		queueStroke(platform::PointerDevice::Mouse, {}, sampleCurve(40, [&](const float p_T) { return l_C + Vec2{ -100.f * l_S, (-340.f + p_T * 380.f) * l_S }; }), {});
		return;
	}
	case 4:
		m_Editor.undo();
		return;
	case 5:
		m_Editor.redo();
		return;
	case 6:
	{
		// Whole-stroke eraser through the hairlines
		m_Editor.eraser().mode = tools::EraserMode::Stroke;
		queueStroke(platform::PointerDevice::Mouse, {}, sampleCurve(20, [&](const float p_T) { return l_C + Vec2{ -500.f * l_S, (290.f + p_T * 90.f) * l_S }; }), {});
		return;
	}
	case 7:
		m_Editor.setTool(tools::ToolKind::Pen);
		// Final view: zoomed around the drawing
		m_Editor.flyTo(m_Editor.camera().center(), m_Options.smokeZoom);
		return;
	default:
		return;
	}
}

void App::captureScreenshot(const VkCommandBuffer p_Cmd, const VkImage p_Image, const VkExtent2D p_Extent)
{
	const VkDeviceSize l_Bytes = static_cast<VkDeviceSize>(p_Extent.width) * p_Extent.height * 4;
	if (!m_ScreenshotBuffer.isValid() || m_ScreenshotBuffer.size < l_Bytes)
	{
		gfx::destroyBuffer(m_Context, m_ScreenshotBuffer);
		m_ScreenshotBuffer = gfx::createBuffer(m_Context, l_Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryKind::Readback, "screenshot");
	}

	gfx::transitionImage(p_Cmd, p_Image, {
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
	});
	const VkBufferImageCopy l_Region{
		.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
		.imageExtent = { p_Extent.width, p_Extent.height, 1 },
	};
	vkCmdCopyImageToBuffer(p_Cmd, p_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_ScreenshotBuffer.handle, 1, &l_Region);
	gfx::transitionImage(p_Cmd, p_Image, {
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccess = VK_ACCESS_2_NONE,
		.dstStage = VK_PIPELINE_STAGE_2_NONE,
		.dstAccess = VK_ACCESS_2_NONE,
	});
	gfx::memoryBarrier(p_Cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

void App::writeScreenshot(const VkExtent2D p_Extent)
{
	m_Context.waitIdle();
	const size_t l_Pixels = static_cast<size_t>(p_Extent.width) * p_Extent.height;
	std::vector<uint8_t> l_Rgba(l_Pixels * 4);
	std::memcpy(l_Rgba.data(), m_ScreenshotBuffer.mapped, l_Rgba.size());

	const VkFormat l_Format = m_Swapchain.format();
	if (l_Format == VK_FORMAT_B8G8R8A8_UNORM || l_Format == VK_FORMAT_B8G8R8A8_SRGB)
	{
		for (size_t i = 0; i < l_Pixels; ++i)
			std::swap(l_Rgba[i * 4], l_Rgba[i * 4 + 2]);
	}
	for (size_t i = 0; i < l_Pixels; ++i)
		l_Rgba[i * 4 + 3] = 255;

	const int l_Ok = stbi_write_png(m_Options.screenshotPath.c_str(), static_cast<int>(p_Extent.width), static_cast<int>(p_Extent.height), 4, l_Rgba.data(), static_cast<int>(p_Extent.width * 4));
	if (l_Ok != 0)
		spdlog::info("Screenshot saved to {}", m_Options.screenshotPath);
	else
		spdlog::error("Could not write screenshot {}", m_Options.screenshotPath);
}
} // namespace wb
