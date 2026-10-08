// --smoke-test: a scripted session that exercises the drawing and rendering paths under validation, and
// optionally (--screenshot) saves the final frame so the output can be inspected.
module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <stb_image_write.h>
#include "gfx/vk_check.hpp"

module wb.app;

import wb.doc.document;
import wb.doc.object;
import wb.doc.selection;
import wb.doc.history;
import wb.platform.clipboard;
import wb.render.image_store;
import wb.editor;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.io.file;
import wb.session;
import wb.math;
import wb.platform.input;
import wb.tools.select;
import wb.tools.tool;
import wb.view.camera;

namespace wb
{
namespace
{
constexpr uint64_t EVENT_INTERVAL_NS = 4'000'000; // 250 Hz, like a typical pen
constexpr size_t EVENTS_PER_FRAME = 6;

Rect s_SmokeBounds{}; // selection bounds remembered between scenario steps
std::vector<ObjectId> s_SmokePictures;
size_t s_SmokeObjectsBefore = 0;

void appendPngBytes(void* p_Context, void* p_Data, const int p_Size)
{
	auto* l_Out = static_cast<std::vector<uint8_t>*>(p_Context);
	l_Out->insert(l_Out->end(), static_cast<const uint8_t*>(p_Data), static_cast<const uint8_t*>(p_Data) + p_Size);
}

// A PNG with a gradient, a transparent corner and a border
std::vector<uint8_t> makeTestPng(const int p_Width, const int p_Height)
{
	std::vector<uint8_t> l_Pixels(static_cast<size_t>(p_Width) * static_cast<size_t>(p_Height) * 4);
	for (int y = 0; y < p_Height; ++y)
	{
		for (int x = 0; x < p_Width; ++x)
		{
			uint8_t* l_Pixel = &l_Pixels[(static_cast<size_t>(y) * static_cast<size_t>(p_Width) + static_cast<size_t>(x)) * 4];
			const bool l_Border = x < 4 || y < 4 || x >= p_Width - 4 || y >= p_Height - 4;
			const bool l_Hole = x < p_Width / 4 && y < p_Height / 4;
			l_Pixel[0] = l_Border ? 20 : static_cast<uint8_t>(255 * x / p_Width);
			l_Pixel[1] = l_Border ? 20 : static_cast<uint8_t>(255 * y / p_Height);
			l_Pixel[2] = l_Border ? 20 : 160;
			l_Pixel[3] = l_Hole ? 0 : 255;
		}
	}
	std::vector<uint8_t> l_Bytes;
	stbi_write_png_to_func(&appendPngBytes, &l_Bytes, p_Width, p_Height, 4, l_Pixels.data(), p_Width * 4);
	return l_Bytes;
}

// An animated GIF (a square moving across a pale background). Valid but uncompressed: the LZW stream restarts every few pixels.
std::vector<uint8_t> makeTestGif(const int p_Size, const int p_Frames)
{
	std::vector<uint8_t> l_Out{ 'G', 'I', 'F', '8', '9', 'a' };
	const auto l_U16 = [&](const int p_Value) { l_Out.push_back(static_cast<uint8_t>(p_Value & 0xFF)); l_Out.push_back(static_cast<uint8_t>((p_Value >> 8) & 0xFF)); };
	l_U16(p_Size);
	l_U16(p_Size);
	l_Out.push_back(0xF3); // global palette of 16 colours
	l_Out.push_back(0);
	l_Out.push_back(0);
	static constexpr uint8_t GIF_COLORS[16][3] = { { 255, 255, 255 }, { 235, 240, 250 }, { 229, 57, 53 }, { 251, 140, 0 }, { 253, 216, 53 }, { 67, 160, 71 }, { 0, 172, 193 }, { 30, 136, 229 }, { 94, 53, 177 }, { 216, 27, 96 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
	for (const auto& l_Color : GIF_COLORS)
		l_Out.insert(l_Out.end(), l_Color, l_Color + 3);

	for (int l_Frame = 0; l_Frame < p_Frames; ++l_Frame)
	{
		l_Out.insert(l_Out.end(), { 0x21, 0xF9, 0x04, 0x04, 20, 0, 0, 0 }); // 200 ms
		l_Out.push_back(0x2C);
		l_U16(0);
		l_U16(0);
		l_U16(p_Size);
		l_U16(p_Size);
		l_Out.push_back(0);
		l_Out.push_back(4); // minimum code size

		std::vector<uint8_t> l_Stream;
		uint32_t l_Bits = 0;
		int l_BitCount = 0;
		const auto l_Code = [&](const uint32_t p_Value)
		{
			l_Bits |= p_Value << l_BitCount;
			l_BitCount += 5;
			while (l_BitCount >= 8)
			{
				l_Stream.push_back(static_cast<uint8_t>(l_Bits & 0xFF));
				l_Bits >>= 8;
				l_BitCount -= 8;
			}
		};
		const int l_Square = p_Size / 4;
		const int l_Left = (p_Size - l_Square) * l_Frame / std::max(p_Frames - 1, 1);
		int l_SincePause = 0;
		l_Code(16); // clear
		for (int y = 0; y < p_Size; ++y)
		{
			for (int x = 0; x < p_Size; ++x)
			{
				const bool l_Inside = x >= l_Left && x < l_Left + l_Square && y >= (p_Size - l_Square) / 2 && y < (p_Size + l_Square) / 2;
				l_Code(l_Inside ? static_cast<uint32_t>(2 + l_Frame % 8) : 1u);
				if (++l_SincePause == 12)
				{
					l_Code(16);
					l_SincePause = 0;
				}
			}
		}
		l_Code(17); // end of information
		if (l_BitCount > 0)
			l_Stream.push_back(static_cast<uint8_t>(l_Bits & 0xFF));
		for (size_t l_Pos = 0; l_Pos < l_Stream.size(); l_Pos += 255)
		{
			const size_t l_Chunk = std::min<size_t>(255, l_Stream.size() - l_Pos);
			l_Out.push_back(static_cast<uint8_t>(l_Chunk));
			l_Out.insert(l_Out.end(), l_Stream.begin() + static_cast<std::ptrdiff_t>(l_Pos), l_Stream.begin() + static_cast<std::ptrdiff_t>(l_Pos + l_Chunk));
		}
		l_Out.push_back(0);
	}
	l_Out.push_back(0x3B);
	return l_Out;
}

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
	{
		// Save, edit, reload: the board must come back exactly, and the GPU mirror must follow the replaced document
		const size_t l_Count = m_Editor.document().size();
		const std::filesystem::path l_File = std::filesystem::temp_directory_path() / "wb_smoke_test.wbrd";
		const auto l_Fail = [&](const char* p_What)
		{
			spdlog::error("Smoke test (save/open): {}", p_What);
			m_Failed = true;
		};
		if (!m_Session.dirty())
			l_Fail("the board should have unsaved changes after drawing");
		if (const IoResult l_Saved = m_Session.saveTo(l_File); !l_Saved.ok)
			l_Fail(l_Saved.error.c_str());
		else if (m_Session.dirty())
			l_Fail("the board should be clean after saving");
		m_Editor.undo(); // dirty again; opening must discard that
		if (!m_Session.dirty())
			l_Fail("undo after saving should make the board dirty");
		if (const IoResult l_Opened = m_Session.open(l_File); !l_Opened.ok)
			l_Fail(l_Opened.error.c_str());
		else if (m_Editor.document().size() != l_Count || m_Session.dirty())
			l_Fail("reopened board differs from the saved one");
		removeFileQuiet(l_File);
		if (!m_Failed)
			spdlog::info("Smoke test: save / reopen round trip of {} objects OK", l_Count);
		return;
	}
	case 8:
	{
		// Every device keeps its own tool; Alt+key borrows a tool while held; the hand tool pans
		const auto l_Check = [&](const bool p_Ok, const char* p_What)
		{
			if (!p_Ok)
			{
				spdlog::error("Smoke test (tools): {}", p_What);
				m_Failed = true;
			}
		};
		const auto l_Move = [&](const platform::PointerDevice p_Device)
		{
			m_Editor.handlePointer(platform::PointerEvent{ .phase = platform::PointerPhase::Move, .device = p_Device, .position = l_C });
		};
		const auto l_Key = [&](const SDL_Keycode p_Key, const SDL_Keymod p_Mod, const bool p_Repeat) -> SDL_KeyboardEvent
		{
			SDL_KeyboardEvent l_Event{};
			l_Event.key = p_Key;
			l_Event.mod = p_Mod;
			l_Event.repeat = p_Repeat;
			return l_Event;
		};

		l_Move(platform::PointerDevice::Mouse);
		m_Editor.setTool(tools::ToolKind::Pen);
		m_Editor.setDeviceTool(platform::PointerDevice::Pen, tools::ToolKind::Select);
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Pen, "setting the pen's tool changed the mouse's");
		l_Move(platform::PointerDevice::Pen);
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Select, "the pen did not keep its own tool");
		l_Move(platform::PointerDevice::Mouse);
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Pen, "the mouse lost its tool when the pen was used");
		m_Editor.setDeviceTool(platform::PointerDevice::Pen, tools::ToolKind::Pen);

		m_Editor.handleKeyDown(l_Key(SDLK_E, SDL_KMOD_LALT, false));
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Eraser && m_Editor.toolIsBorrowed(), "Alt+E did not borrow the eraser");
		m_Editor.handleKeyDown(l_Key(SDLK_E, SDL_KMOD_LALT, true));
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Eraser, "key repeat changed the borrowed tool");
		m_Editor.handleKeyUp(l_Key(SDLK_E, SDL_KMOD_NONE, false));
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Pen && !m_Editor.toolIsBorrowed(), "releasing E did not give the pen back");
		m_Editor.handleKeyDown(l_Key(SDLK_V, SDL_KMOD_LALT, false));
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Select, "Alt+V did not borrow the select tool");
		m_Editor.handleKeyUp(l_Key(SDLK_LALT, SDL_KMOD_NONE, false));
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Pen, "releasing Alt did not give the pen back");
		m_Editor.handleKeyDown(l_Key(SDLK_E, SDL_KMOD_NONE, false));
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Eraser && !m_Editor.toolIsBorrowed(), "plain E should switch for good");
		m_Editor.handleKeyUp(l_Key(SDLK_E, SDL_KMOD_NONE, false));
		l_Check(m_Editor.selectedTool() == tools::ToolKind::Eraser, "releasing a plain tool key must not change the tool");

		// Hand tool: dragging moves the view like the wheel does, and draws nothing
		m_Editor.setTool(tools::ToolKind::Hand);
		const size_t l_Objects = m_Editor.document().size();
		const DVec2 l_Before = m_Editor.camera().center();
		const double l_Ppu = m_Editor.camera().pixelsPerUnit();
		m_Editor.handlePointer(platform::PointerEvent{ .phase = platform::PointerPhase::Down, .device = platform::PointerDevice::Mouse, .button = platform::PointerButton::Primary, .buttons = platform::ButtonMask::Primary, .position = l_C });
		m_Editor.handlePointer(platform::PointerEvent{ .phase = platform::PointerPhase::Move, .device = platform::PointerDevice::Mouse, .buttons = platform::ButtonMask::Primary, .position = l_C + Vec2{ 100.f * l_S, 50.f * l_S } });
		m_Editor.handlePointer(platform::PointerEvent{ .phase = platform::PointerPhase::Up, .device = platform::PointerDevice::Mouse, .button = platform::PointerButton::Primary, .position = l_C + Vec2{ 100.f * l_S, 50.f * l_S } });
		const DVec2 l_Moved = m_Editor.camera().center() - l_Before;
		l_Check(std::abs(l_Moved.x + 100.f * l_S / l_Ppu) < 1e-3 && std::abs(l_Moved.y + 50.f * l_S / l_Ppu) < 1e-3, "the hand tool did not pan the view with the pointer");
		l_Check(m_Editor.document().size() == l_Objects, "the hand tool changed the board");
		m_Editor.lookAt(l_Before, m_Editor.camera().zoom());
		if (!m_Failed)
			spdlog::info("Smoke test: per-device tools, Alt+key borrowing and the hand tool OK");
		return;
	}
	case 9:
		// Box-select the sine wave and the mouse line
		m_Editor.setTool(tools::ToolKind::Select);
		m_Editor.selectState().mode = tools::SelectMode::Box;
		queueStroke(platform::PointerDevice::Mouse, {}, sampleCurve(30, [&](const float p_T) { return l_C + Vec2{ (-640.f + p_T * 1280.f) * l_S, (-330.f + p_T * 270.f) * l_S }; }), {});
		return;
	case 10:
	{
		static const auto s_Frame = [](const tools::SelectionOverlay& p_Overlay) { return (p_Overlay.corners[0] + p_Overlay.corners[2]) * 0.5f; };
		if (m_Editor.selection().empty())
		{
			spdlog::error("Smoke test (selection): the box selected nothing");
			m_Failed = true;
			return;
		}
		spdlog::info("Smoke test: box selected {} objects", m_Editor.selection().size());
		s_SmokeBounds = m_Editor.selection().bounds();
		// Move: grab inside the box and drag
		const Vec2 l_From = s_Frame(m_Editor.selectionOverlay());
		queueStroke(platform::PointerDevice::Mouse, {}, sampleCurve(20, [&](const float p_T) { return l_From + Vec2{ 80.f * l_S, 60.f * l_S } * p_T; }), {});
		return;
	}
	case 11:
	{
		const double l_Ppu = m_Editor.camera().pixelsPerUnit();
		const DVec2 l_Delta = m_Editor.selection().bounds().center() - s_SmokeBounds.center();
		if (std::abs(l_Delta.x - 80.0 * l_S / l_Ppu) > 1e-2 || std::abs(l_Delta.y - 60.0 * l_S / l_Ppu) > 1e-2)
		{
			spdlog::error("Smoke test (selection): moved by ({:.3f}, {:.3f}) instead of ({:.3f}, {:.3f})", l_Delta.x, l_Delta.y, 80.0 * l_S / l_Ppu, 60.0 * l_S / l_Ppu);
			m_Failed = true;
		}
		const size_t l_Undo = m_Editor.history().undoCount();
		m_Editor.undo();
		const DVec2 l_Back = m_Editor.selection().bounds().center() - s_SmokeBounds.center();
		if (std::abs(l_Back.x) > 1e-2 || std::abs(l_Back.y) > 1e-2 || m_Editor.history().undoCount() + 1 != l_Undo)
		{
			spdlog::error("Smoke test (selection): undoing the move did not restore the objects");
			m_Failed = true;
		}
		m_Editor.redo();
		s_SmokeBounds = m_Editor.selection().bounds();
		// Scale: drag the south-east handle outwards
		const tools::SelectionOverlay l_Overlay = m_Editor.selectionOverlay();
		const Vec2 l_Handle = l_Overlay.handles[3];
		queueStroke(platform::PointerDevice::Mouse, {}, sampleCurve(20, [&](const float p_T) { return l_Handle + Vec2{ 120.f * l_S, 60.f * l_S } * p_T; }), {});
		return;
	}
	case 12:
	{
		const DVec2 l_Before = s_SmokeBounds.size();
		const DVec2 l_After = m_Editor.selection().bounds().size();
		if (l_After.x < l_Before.x * 1.05 || l_After.y < l_Before.y * 1.05)
		{
			spdlog::error("Smoke test (selection): scaling did not grow the selection ({:.1f}x{:.1f} -> {:.1f}x{:.1f})", l_Before.x, l_Before.y, l_After.x, l_After.y);
			m_Failed = true;
		}
		s_SmokeBounds = m_Editor.selection().bounds();
		// Rotate: drag the rotate handle sideways
		const tools::SelectionOverlay l_Overlay = m_Editor.selectionOverlay();
		const Vec2 l_Handle = l_Overlay.rotateHandle;
		queueStroke(platform::PointerDevice::Mouse, {}, sampleCurve(20, [&](const float p_T) { return l_Handle + Vec2{ 160.f * l_S, 40.f * l_S } * p_T; }), {});
		return;
	}
	case 13:
	{
		bool l_Rotated = false;
		for (const ObjectId l_Id : m_Editor.selection().ids())
		{
			if (const Object* l_Object = m_Editor.document().find(l_Id))
				l_Rotated = l_Rotated || std::abs(l_Object->transform.linear[0].y) > 0.05;
		}
		if (!l_Rotated)
		{
			spdlog::error("Smoke test (selection): the rotate handle did not rotate the objects");
			m_Failed = true;
		}
		// Edit commands
		const size_t l_Total = m_Editor.document().size();
		const size_t l_Selected = m_Editor.selection().size();
		m_Editor.duplicateSelection();
		if (m_Editor.document().size() != l_Total + l_Selected || m_Editor.selection().size() != l_Selected)
		{
			spdlog::error("Smoke test (selection): duplicate produced the wrong objects");
			m_Failed = true;
		}
		m_Editor.deleteSelection();
		if (m_Editor.document().size() != l_Total || !m_Editor.selection().empty())
		{
			spdlog::error("Smoke test (selection): deleting the duplicates left something behind");
			m_Failed = true;
		}
		m_Editor.undo(); // brings the duplicates back
		m_Editor.undo(); // removes them again
		if (m_Editor.document().size() != l_Total)
		{
			spdlog::error("Smoke test (selection): undo after duplicate and delete is off");
			m_Failed = true;
		}
		if (!m_Failed)
			spdlog::info("Smoke test: select, move, scale, rotate, duplicate, delete and undo OK");
		return;
	}
	case 14:
		m_Editor.clearSelection();
		m_Editor.setTool(tools::ToolKind::Pen);
		// Final view: zoomed around the drawing
		m_Editor.flyTo(m_Editor.camera().center(), m_Options.smokeZoom);
		return;
	case 15:
	{
		// Pictures: a still PNG and an animated GIF, inserted like a paste does
		m_Editor.clearSelection();
		s_SmokeObjectsBefore = m_Editor.document().size();
		std::vector<platform::ClipboardPicture> l_Pictures;
		l_Pictures.push_back(platform::ClipboardPicture{ .bytes = makeTestPng(320, 200), .name = "gradient.png" });
		l_Pictures.push_back(platform::ClipboardPicture{ .bytes = makeTestGif(96, 6), .name = "mover.gif" });
		l_Pictures.push_back(platform::ClipboardPicture{ .bytes = { 1, 2, 3 }, .name = "garbage.bin" });
		importPictures(std::move(l_Pictures), l_C + Vec2{ 330.f * l_S, 90.f * l_S });
		return;
	}
	case 16:
	{
		const Document& l_Document = m_Editor.document();
		s_SmokePictures.clear();
		for (const ObjectId l_Id : m_Editor.selection().ids())
			s_SmokePictures.push_back(l_Id);
		if (l_Document.size() != s_SmokeObjectsBefore + 2 || s_SmokePictures.size() != 2 || l_Document.assets().size() < 2)
		{
			spdlog::error("Smoke test (pictures): expected two pictures, found {} new objects", l_Document.size() - s_SmokeObjectsBefore);
			m_Failed = true;
			return;
		}
		m_Editor.flipSelection(true);
		// Give the decoder threads time to finish: the frame loop keeps running while pictures load
		return;
	}
	case 17:
	{
		if (m_Canvas.images().busy())
		{
			--m_SmokeStep; // wait for the decoders
			return;
		}
		bool l_Ok = true;
		for (const ObjectId l_Id : s_SmokePictures)
		{
			const Object* l_Object = m_Editor.document().find(l_Id);
			const render::ImageInfo l_Info = m_Canvas.images().info(l_Object->image()->asset);
			if (!l_Info.ready)
				l_Ok = false;
		}
		const render::ImageInfo l_Gif = m_Canvas.images().info(m_Editor.document().find(s_SmokePictures.back())->image()->asset);
		if (!l_Ok || l_Gif.frames != 6 || l_Gif.durationMs != 1200)
		{
			spdlog::error("Smoke test (pictures): decoding went wrong (gif frames {}, {} ms)", l_Gif.frames, l_Gif.durationMs);
			m_Failed = true;
		}
		// Undo the flip, redo it, and undo/redo the insertion itself
		const size_t l_Size = m_Editor.document().size();
		m_Editor.undo();
		m_Editor.undo();
		m_Editor.redo();
		m_Editor.redo();
		if (m_Editor.document().size() != l_Size)
		{
			spdlog::error("Smoke test (pictures): undo/redo changed the object count");
			m_Failed = true;
		}
		// Pause the animation on its current frame, then let it play again
		m_Editor.selection().set(std::vector<ObjectId>{ s_SmokePictures.back() });
		toggleSelectedPlayback();
		const bool l_Paused = !m_Editor.document().find(s_SmokePictures.back())->image()->playing;
		toggleSelectedPlayback();
		const bool l_Playing = m_Editor.document().find(s_SmokePictures.back())->image()->playing;
		if (!l_Paused || !l_Playing)
		{
			spdlog::error("Smoke test (pictures): pausing and playing did not work");
			m_Failed = true;
		}
		if (!m_Failed)
			spdlog::info("Smoke test: pictures (png + animated gif), flip, undo/redo and pause OK");
		// Rotate the still one (live, without history) to look at its edges
		const ObjectId l_Still = s_SmokePictures.front();
		m_Editor.document().modify(l_Still, [&](Object& p_Object)
		{
			p_Object.transform = Affine2::around(p_Object.transform.translation, Affine2::rotate(0.3)) * p_Object.transform;
		}, ObjectChange::Transform);
		m_Editor.selection().set(std::vector<ObjectId>{ s_SmokePictures.front(), s_SmokePictures.back() });
		return;
	}
	case 18:
	{
		// Leave a panel open for the screenshot
		const std::string& l_Ui = m_Options.smokeUi;
		if (l_Ui == "pen" || l_Ui == "picker")
		{
			m_Editor.setTool(tools::ToolKind::Pen);
			openPopup(Popup::Pen);
			m_PickerOpen = l_Ui == "picker";
			m_RecentColors[0] = 0x8E24AAFFu;
			m_RecentColors[1] = 0x26A69AFFu;
		}
		else if (l_Ui == "eraser")
		{
			m_Editor.setTool(tools::ToolKind::Eraser);
			openPopup(Popup::Eraser);
		}
		else if (l_Ui == "select")
		{
			m_Editor.setTool(tools::ToolKind::Select);
			openPopup(Popup::Select);
		}
		else if (l_Ui == "selection" || l_Ui == "selcolor")
		{
			m_Editor.setTool(tools::ToolKind::Select);
			m_Editor.selectAll();
			if (l_Ui == "selcolor")
				openPopup(Popup::SelectionColor);
		}
		else if (l_Ui == "menu")
		{
			openPopup(Popup::Menu);
			m_MenuSection = 1;
		}
		else if (l_Ui == "shortcuts")
		{
			m_ShowShortcuts = true;
		}
		else if (l_Ui == "dialog")
		{
			m_PromptOpen = true;
			m_PromptAction = Action::Quit;
		}
		else if (l_Ui == "toast")
		{
			showToast("Saved Untitled");
		}
		return;
	}
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
