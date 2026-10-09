// --smoke-test: a scripted session that exercises the drawing and rendering paths under validation, and
// optionally (--screenshot) saves the final frame so the output can be inspected.
module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <span>
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
import wb.render.video_store;
import wb.editor;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.io.file;
import wb.io.serializer;
import wb.text.system;
import wb.session;
import wb.math;
import wb.platform.input;
import wb.tools.select;
import wb.tools.tool;
import wb.view.camera;
import wb.view.ruler;
import wb.brush.shapes;

namespace wb
{
namespace
{
constexpr uint64_t EVENT_INTERVAL_NS = 4'000'000; // 250 Hz, like a typical pen
constexpr size_t EVENTS_PER_FRAME = 6;

Rect s_SmokeBounds{}; // selection bounds remembered between scenario steps
std::filesystem::path s_SmokeExportPath;
uint32_t s_SmokeExportWidth = 0;
uint32_t s_SmokeExportHeight = 0;
std::vector<ObjectId> s_SmokePictures;
size_t s_SmokeObjectsBefore = 0;
size_t s_SmokeUndoBefore = 0;
float s_SmokeWidthBefore = 0.f;
bool s_SmokeExtraPicture = false;
ObjectId s_SmokeVideo = INVALID_OBJECT_ID;
uint32_t s_SmokeVideoWaits = 0;
uint64_t s_SmokeVideoMarkNs = 0;

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

// A PDF with two pages (Letter size, a heading and a blue box on each), built by hand
std::string makeTestPdf()
{
	std::vector<std::string> l_Objects;
	l_Objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
	l_Objects.push_back("<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>");
	const auto l_Stream = [](const std::string& p_Content) { return "<< /Length " + std::to_string(p_Content.size()) + " >>\nstream\n" + p_Content + "\nendstream"; };
	l_Objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 7 0 R >> >> >>");
	l_Objects.push_back(l_Stream("BT /F1 40 Tf 72 700 Td (Smoke test page 1) Tj ET 0.1 0.4 0.9 rg 72 500 300 120 re f"));
	l_Objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R /Resources << /Font << /F1 7 0 R >> >> >>");
	l_Objects.push_back(l_Stream("BT /F1 40 Tf 72 700 Td (Smoke test page 2) Tj ET 0.9 0.2 0.2 rg 200 300 250 250 re f"));
	l_Objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

	std::string l_Out = "%PDF-1.4\n";
	std::vector<size_t> l_Offsets;
	for (size_t i = 0; i < l_Objects.size(); ++i)
	{
		l_Offsets.push_back(l_Out.size());
		l_Out += std::to_string(i + 1) + " 0 obj\n" + l_Objects[i] + "\nendobj\n";
	}
	const size_t l_XrefAt = l_Out.size();
	l_Out += "xref\n0 " + std::to_string(l_Objects.size() + 1) + "\n0000000000 65535 f \n";
	for (const size_t l_Offset : l_Offsets)
	{
		char l_Line[32];
		std::snprintf(l_Line, sizeof(l_Line), "%010zu 00000 n \n", l_Offset);
		l_Out += l_Line;
	}
	l_Out += "trailer\n<< /Size " + std::to_string(l_Objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(l_XrefAt) + "\n%%EOF\n";
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
		// WB_SMOKE_PICTURE=<file>: also insert a real picture (to check a particular file by eye)
		if (const char* l_Extra = SDL_getenv("WB_SMOKE_PICTURE"))
		{
			platform::ClipboardPicture l_Real;
			if (readFile(pathFromUtf8(l_Extra), l_Real.bytes).ok)
			{
				l_Real.name = "real";
				l_Pictures.push_back(std::move(l_Real));
				s_SmokeExtraPicture = true;
			}
		}
		importPictures(std::move(l_Pictures), l_C + Vec2{ 330.f * l_S, 90.f * l_S });
		return;
	}
	case 16:
	{
		const Document& l_Document = m_Editor.document();
		s_SmokePictures.clear();
		for (const ObjectId l_Id : m_Editor.selection().ids())
			s_SmokePictures.push_back(l_Id);
		if (l_Document.size() != s_SmokeObjectsBefore + 2 + (s_SmokeExtraPicture ? 1 : 0) || s_SmokePictures.size() != 2 + (s_SmokeExtraPicture ? 1u : 0u) || l_Document.assets().size() < 2)
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
		if (s_SmokeExtraPicture)
		{
			const render::ImageInfo l_Real = m_Canvas.images().info(m_Editor.document().find(s_SmokePictures.back())->image()->asset);
			spdlog::info("Smoke test: the extra picture has {} frames, {} ms, {}x{}", l_Real.frames, l_Real.durationMs, l_Real.width, l_Real.height);
			s_SmokePictures.pop_back();
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
		// Text: typed through the editor like the keyboard does it
		if (m_Options.smokeUi == "text")
		{
			m_Editor.selectAll();
			m_Editor.deleteSelection();
			m_Editor.lookAt(DVec2{ 0.0 }, 1.0);
		}
		s_SmokeObjectsBefore = m_Editor.document().size();
		s_SmokeUndoBefore = m_Editor.history().undoCount();
		const auto l_Click = [&](const Vec2 p_Screen)
		{
			m_SmokeTimeNs += 1'000'000'000ull;
			platform::PointerEvent l_Event{ .phase = platform::PointerPhase::Down, .device = platform::PointerDevice::Mouse, .button = platform::PointerButton::Primary, .buttons = platform::ButtonMask::Primary, .position = p_Screen, .timestampNs = m_SmokeTimeNs };
			m_Editor.handlePointer(l_Event);
			l_Event.phase = platform::PointerPhase::Up;
			l_Event.buttons = 0;
			m_Editor.handlePointer(l_Event);
		};
		const auto l_Key = [&](const SDL_Keycode p_Key)
		{
			SDL_KeyboardEvent l_Event{};
			l_Event.type = SDL_EVENT_KEY_DOWN;
			l_Event.key = p_Key;
			l_Event.down = true;
			m_Editor.handleKeyDown(l_Event);
		};
		m_Editor.setTool(tools::ToolKind::Text);
		tools::TextState& l_State = m_Editor.textState();
		m_Editor.brush().color = Color::fromRgba8(0x1F1F1FFFu);

		l_State = { .family = "Inter", .style = 0, .sizePoints = 28.f, .align = TextAlign::Left };
		l_Click(l_C + Vec2{ -560.f * l_S, -300.f * l_S });
		m_Editor.handleTextInput("Inkwell text: Inter");
		m_Editor.handleTextInput("\nsecond line, kerning: AVA To Wa");
		l_Key(SDLK_ESCAPE);

		l_State = { .family = "Caveat", .style = TextStyle::Bold, .sizePoints = 54.f, .align = TextAlign::Left };
		m_Editor.brush().color = Color::fromRgba8(0xE53935FFu);
		l_Click(l_C + Vec2{ -560.f * l_S, -190.f * l_S });
		m_Editor.handleTextInput("Handwriting looks good");
		l_Key(SDLK_ESCAPE);

		l_State = { .family = "Source Serif 4", .style = TextStyle::Italic, .sizePoints = 30.f, .align = TextAlign::Center };
		m_Editor.brush().color = Color::fromRgba8(0x1E88E5FFu);
		l_Click(l_C + Vec2{ 120.f * l_S, -300.f * l_S });
		m_Editor.handleTextInput("Serif italic, centred\nshort\na much longer line here");
		l_Key(SDLK_ESCAPE);

		l_State = { .family = "JetBrains Mono", .style = TextStyle::Bold, .sizePoints = 22.f, .align = TextAlign::Right };
		m_Editor.brush().color = Color::fromRgba8(0x43A047FFu);
		l_Click(l_C + Vec2{ 480.f * l_S, -170.f * l_S });
		m_Editor.handleTextInput("int main()\n{\n\treturn 0;\n}");
		l_Key(SDLK_ESCAPE);

		// Fallback fonts for characters Inter lacks, and synthetic styles for a face without bold / italic
		l_State = { .family = "Patrick Hand", .style = TextStyle::Bold | TextStyle::Italic, .sizePoints = 34.f, .align = TextAlign::Left };
		m_Editor.brush().color = Color::fromRgba8(0x8E24AAFFu);
		l_Click(l_C + Vec2{ -560.f * l_S, -60.f * l_S });
		m_Editor.handleTextInput("Synthetic bold italic");
		l_Key(SDLK_ESCAPE);

		l_State = { .family = "Inter", .style = 0, .sizePoints = 26.f, .align = TextAlign::Left };
		m_Editor.brush().color = Color::fromRgba8(0x1F1F1FFFu);
		l_Click(l_C + Vec2{ -560.f * l_S, 20.f * l_S });
		m_Editor.handleTextInput(reinterpret_cast<const char*>(u8"Fallback: \u65e5\u672c\u8a9e \u2713 \u041f\u0440\u0438\u0432\u0435\u0442 \u0395\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac \u2192 \u2211"));
		l_Key(SDLK_ESCAPE);

		// A wrapped box, sized through the style like the width handle does
		l_State = { .family = "Inter", .style = 0, .sizePoints = 24.f, .align = TextAlign::Left };
		l_Click(l_C + Vec2{ 120.f * l_S, 90.f * l_S });
		m_Editor.handleTextInput("This box wraps its text at a fixed width, so a long sentence breaks into several lines instead of growing wider and wider.");
		m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.wrapWidth = 380.f; });
		l_Key(SDLK_ESCAPE);
		return;
	}
	case 19:
	{
		const size_t l_Added = m_Editor.document().size() - s_SmokeObjectsBefore;
		const size_t l_Steps = m_Editor.history().undoCount() - s_SmokeUndoBefore;
		if (l_Added != 7 || l_Steps != 7 || m_Editor.textEditing())
		{
			spdlog::error("Smoke test (text): expected 7 new text boxes in 7 undo steps, got {} boxes and {} steps (editing: {})", l_Added, l_Steps, m_Editor.textEditing());
			m_Failed = true;
			return;
		}
		// The boxes are as big as their laid-out text, and the wrapped one is as wide as asked
		bool l_Sized = true;
		const TextData* l_Wrapped = nullptr;
		for (const std::unique_ptr<Object>& l_Object : m_Editor.document().objects())
		{
			const TextData* l_Text = l_Object->text();
			if (l_Text == nullptr)
				continue;
			const Vec2 l_Size = m_Editor.textSystem().measure(*l_Text);
			l_Sized = l_Sized && std::abs(l_Size.x - l_Text->size.x) < 0.01f && std::abs(l_Size.y - l_Text->size.y) < 0.01f;
			if (l_Text->wrapWidth > 0.f)
				l_Wrapped = l_Text;
		}
		if (!l_Sized || l_Wrapped == nullptr || std::abs(l_Wrapped->size.x - 380.f) > 0.01f || l_Wrapped->size.y < l_Wrapped->fontSize * 2.f)
		{
			spdlog::error("Smoke test (text): the boxes do not match their layout (sized: {})", l_Sized);
			m_Failed = true;
			return;
		}
		// Edit the wrapped box again: select it, press Enter, type at the end, finish. One undo step, undoable.
		m_Editor.setTool(tools::ToolKind::Select);
		m_Editor.selection().set(std::vector<ObjectId>{ m_Editor.document().objects().back()->id });
		SDL_KeyboardEvent l_Enter{};
		l_Enter.type = SDL_EVENT_KEY_DOWN;
		l_Enter.key = SDLK_RETURN;
		l_Enter.down = true;
		m_Editor.handleKeyDown(l_Enter);
		if (!m_Editor.textEditing())
		{
			spdlog::error("Smoke test (text): Enter did not start editing the selected text");
			m_Failed = true;
			return;
		}
		m_Editor.handleTextInput(" More words.");
		l_Enter.key = SDLK_ESCAPE;
		m_Editor.handleKeyDown(l_Enter);
		const size_t l_Undo = m_Editor.history().undoCount();
		const std::string l_Edited = m_Editor.document().objects().back()->text()->text;
		m_Editor.undo();
		const bool l_Reverted = m_Editor.document().objects().back()->text()->text.find("More words.") == std::string::npos;
		m_Editor.redo();
		if (l_Undo != s_SmokeUndoBefore + 8 || l_Edited.find("More words.") == std::string::npos || !l_Reverted || m_Editor.document().objects().back()->text()->text != l_Edited)
		{
			spdlog::error("Smoke test (text): editing an existing text box did not make one undo step");
			m_Failed = true;
			return;
		}
		// Save and load keep the text and the fonts it needs
		BoardMeta l_Meta;
		const std::vector<uint8_t> l_Bytes = m_Editor.saveBoard("");
		const size_t l_Objects = m_Editor.document().size();
		if (!m_Editor.loadBoard(l_Bytes, l_Meta).ok || m_Editor.document().size() != l_Objects)
		{
			spdlog::error("Smoke test (text): saving and loading lost objects");
			m_Failed = true;
			return;
		}
		m_Editor.clearSelection();
		m_Editor.setTool(tools::ToolKind::Pen);
		if (!m_Failed)
			spdlog::info("Smoke test: text boxes (fonts, styles, alignment, wrapping, fallback), editing, undo and save/load OK");
		return;
	}
	case 20:
	{
		// Resize the last text box with its side handle: it wraps at the new width, in one undo step
		m_Editor.setTool(tools::ToolKind::Select);
		m_Editor.selection().set(std::vector<ObjectId>{ m_Editor.document().objects().back()->id });
		// Turned by 30 degrees: the box turns with it and the side handle still works along the text's own axis
		m_Editor.document().modify(m_Editor.document().objects().back()->id, [&](Object& p_Object)
		{
			p_Object.transform = Affine2::around(p_Object.transform.translation, Affine2::rotate(0.5)) * p_Object.transform;
		}, ObjectChange::Transform);
		s_SmokeUndoBefore = m_Editor.history().undoCount();
		s_SmokeWidthBefore = m_Editor.document().objects().back()->text()->wrapWidth;
		const tools::SelectionOverlay l_Overlay = m_Editor.selectionOverlay();
		Vec2 l_Handle{ 0.f };
		bool l_Found = false;
		for (size_t i = 0; i < l_Overlay.handles.size(); ++i)
		{
			const bool l_Corner = i % 2 == 1;
			if (l_Overlay.handleShown[i] && !l_Corner && !l_Found)
			{
				l_Handle = l_Overlay.handles[i];
				l_Found = true;
			}
		}
		if (!l_Found)
		{
			spdlog::error("Smoke test (text): a text box shows no side handle");
			m_Failed = true;
			return;
		}
		queueStroke(platform::PointerDevice::Mouse, {}, sampleCurve(20, [&](const float p_T) { return l_Handle + Vec2{ -70.f * l_S * std::cos(0.5f), -70.f * l_S * std::sin(0.5f) } * p_T; }), {});
		return;
	}
	case 21:
	{
		const Object* l_Object = m_Editor.document().objects().back().get();
		const float l_Width = l_Object->text()->wrapWidth;
		if (std::abs(l_Width - s_SmokeWidthBefore) < 20.f || m_Editor.history().undoCount() != s_SmokeUndoBefore + 1)
		{
			spdlog::error("Smoke test (text): the side handle did not change the wrap width ({} -> {}, {} undo steps)", s_SmokeWidthBefore, l_Width, m_Editor.history().undoCount() - s_SmokeUndoBefore);
			m_Failed = true;
			return;
		}
		m_Editor.undo();
		if (std::abs(m_Editor.document().objects().back()->text()->wrapWidth - s_SmokeWidthBefore) > 0.01f)
		{
			spdlog::error("Smoke test (text): undoing the resize did not restore the width");
			m_Failed = true;
			return;
		}
		m_Editor.redo();
		// Reset: upright again, same centre, undoable
		const DVec2 l_CenterBefore = m_Editor.document().objects().back()->worldBounds().center();
		if (!m_Editor.selectionIsTilted())
		{
			spdlog::error("Smoke test (text): a turned text box is not reported as tilted");
			m_Failed = true;
			return;
		}
		m_Editor.resetSelectionTransform();
		const Object* l_After = m_Editor.document().objects().back().get();
		if (m_Editor.selectionIsTilted() || glm::length(l_After->worldBounds().center() - l_CenterBefore) > 1e-6)
		{
			spdlog::error("Smoke test (text): resetting the transform failed");
			m_Failed = true;
			return;
		}
		m_Editor.undo();
		if (!m_Editor.selectionIsTilted())
		{
			spdlog::error("Smoke test (text): undoing the reset did not bring the rotation back");
			m_Failed = true;
			return;
		}
		m_Editor.redo();
		if (!m_Failed)
			spdlog::info("Smoke test: resizing a rotated text box through its side handle, and resetting its transform OK");
		return;
	}
	case 22:
	{
		// Video: the test clip is imported like a dropped file
		m_Editor.clearSelection();
		std::filesystem::path l_Path;
		if (const char* l_Env = SDL_getenv("WB_SMOKE_VIDEO"))
		{
			l_Path = pathFromUtf8(l_Env);
		}
		else
		{
			const char* l_Base = SDL_GetBasePath();
			l_Path = (l_Base != nullptr ? std::filesystem::path(reinterpret_cast<const char8_t*>(l_Base)) : std::filesystem::current_path()) / "assets" / "test" / "clip.mp4";
		}
		platform::ClipboardPicture l_Clip;
		if (!readFile(l_Path, l_Clip.bytes).ok)
		{
			spdlog::error("Smoke test (video): cannot read {}", pathToUtf8(l_Path));
			m_Failed = true;
			return;
		}
		l_Clip.name = "clip.mp4";
		s_SmokeObjectsBefore = m_Editor.document().size();
		std::vector<platform::ClipboardPicture> l_Clips;
		l_Clips.push_back(std::move(l_Clip));
		importPictures(std::move(l_Clips), l_C + Vec2{ -300.f * l_S, 140.f * l_S });
		s_SmokeVideoWaits = 0;
		return;
	}
	case 23:
	{
		const Document& l_Document = m_Editor.document();
		s_SmokeVideo = l_Document.size() == s_SmokeObjectsBefore + 1 ? l_Document.objects().back()->id : INVALID_OBJECT_ID;
		if (s_SmokeVideo == INVALID_OBJECT_ID || l_Document.find(s_SmokeVideo)->video() == nullptr)
		{
			spdlog::error("Smoke test (video): the clip was not imported as a video");
			m_Failed = true;
			return;
		}
		return;
	}
	case 24:
	{
		// The player opens on a worker thread: wait for it and for the first picture
		const render::VideoStatus l_Status = m_Canvas.videos().status(s_SmokeVideo);
		if (!l_Status.ready)
		{
			if (l_Status.failed || ++s_SmokeVideoWaits > 400)
			{
				spdlog::error("Smoke test (video): the video did not open ({})", l_Status.error);
				m_Failed = true;
				return;
			}
			--m_SmokeStep;
			return;
		}
		if (std::abs(l_Status.duration - 3.0) > 0.2 || l_Status.playing || l_Status.position != 0.0 || !l_Status.hasAudio)
		{
			spdlog::error("Smoke test (video): wrong state after opening (duration {}, playing {}, position {}, audio {})", l_Status.duration, l_Status.playing, l_Status.position, l_Status.hasAudio);
			m_Failed = true;
			return;
		}
		m_Canvas.videos().setPlaying(s_SmokeVideo, true);
		s_SmokeVideoMarkNs = SDL_GetTicksNS();
		return;
	}
	case 25:
	{
		if (SDL_GetTicksNS() - s_SmokeVideoMarkNs < 600'000'000ull)
		{
			--m_SmokeStep;
			return;
		}
		const render::VideoStatus l_Status = m_Canvas.videos().status(s_SmokeVideo);
		if (!l_Status.playing || l_Status.position < 0.3 || l_Status.position > 1.5)
		{
			spdlog::error("Smoke test (video): playback did not advance as expected (playing {}, position {})", l_Status.playing, l_Status.position);
			m_Failed = true;
			return;
		}
		if (!m_Canvas.animating())
		{
			spdlog::error("Smoke test (video): a playing video should keep frames coming");
			m_Failed = true;
			return;
		}
		// Seek while playing, pause, and make sure the position holds
		m_Canvas.videos().seek(s_SmokeVideo, 2.0);
		m_Canvas.videos().setPlaying(s_SmokeVideo, false);
		s_SmokeVideoMarkNs = SDL_GetTicksNS();
		return;
	}
	case 26:
	{
		if (SDL_GetTicksNS() - s_SmokeVideoMarkNs < 400'000'000ull)
		{
			--m_SmokeStep;
			return;
		}
		const render::VideoStatus l_Status = m_Canvas.videos().status(s_SmokeVideo);
		if (l_Status.playing || std::abs(l_Status.position - 2.0) > 0.1)
		{
			spdlog::error("Smoke test (video): seek and pause went wrong (playing {}, position {})", l_Status.playing, l_Status.position);
			m_Failed = true;
			return;
		}
		// Loop and sound switches are undoable edits of the object
		const size_t l_Undo = m_Editor.history().undoCount();
		m_Editor.editVideo(s_SmokeVideo, "Loop video", [](VideoData& p_Data) { p_Data.loop = true; return true; });
		const bool l_Looping = m_Editor.document().find(s_SmokeVideo)->video()->loop;
		m_Editor.undo();
		const bool l_Undone = !m_Editor.document().find(s_SmokeVideo)->video()->loop;
		m_Editor.redo();
		if (!l_Looping || !l_Undone || m_Editor.history().undoCount() != l_Undo + 1)
		{
			spdlog::error("Smoke test (video): the loop switch is not an undoable edit");
			m_Failed = true;
			return;
		}
		// Save and load keeps the video, its settings and its file
		BoardMeta l_Meta;
		const std::vector<uint8_t> l_Saved = serializeBoard(m_Editor.document(), l_Meta);
		Document l_Loaded;
		BoardMeta l_LoadedMeta;
		const LoadResult l_Result = deserializeBoard(l_Saved, l_Loaded, l_LoadedMeta);
		const Object* l_Back = l_Result.ok ? l_Loaded.find(s_SmokeVideo) : nullptr;
		const ImageAsset* l_Asset = l_Back != nullptr && l_Back->video() != nullptr ? l_Loaded.findAsset(l_Back->video()->asset) : nullptr;
		const ImageAsset* l_Original = m_Editor.document().findAsset(m_Editor.document().find(s_SmokeVideo)->video()->asset);
		if (l_Asset == nullptr || !l_Back->video()->loop || l_Asset->bytes != l_Original->bytes)
		{
			spdlog::error("Smoke test (video): the video did not survive saving and loading");
			m_Failed = true;
			return;
		}
		// Deleting a video stops its player, undoing brings it back
		m_Editor.selection().set(std::vector<ObjectId>{ s_SmokeVideo });
		m_Canvas.videos().setPlaying(s_SmokeVideo, true);
		m_Editor.deleteSelection();
		m_Editor.undo();
		m_Editor.selection().clear();
		m_Canvas.videos().setPlaying(s_SmokeVideo, false);
		if (!m_Failed)
			spdlog::info("Smoke test: video import, decoding, play, seek, pause, loop switch, save/load and delete OK");
		return;
	}
	case 27:
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
		else if (l_Ui == "text")
		{
			m_Editor.setTool(tools::ToolKind::Text);
			openTextPopup(true);
		}
		else if (l_Ui == "textedit" || l_Ui == "textstyle")
		{
			m_Editor.setTool(tools::ToolKind::Select);
			m_Editor.selection().set(std::vector<ObjectId>{ m_Editor.document().objects().back()->id });
			if (l_Ui == "textedit")
			{
				m_Editor.beginEditingSelectedText();
				for (int i = 0; i < 6; ++i)
				{
					SDL_KeyboardEvent l_Left{};
					l_Left.type = SDL_EVENT_KEY_DOWN;
					l_Left.key = SDLK_LEFT;
					l_Left.mod = SDL_KMOD_SHIFT;
					l_Left.down = true;
					m_Editor.handleKeyDown(l_Left);
				}
			}
			else
			{
				openTextPopup(false);
			}
		}
		else if (l_Ui == "video" || l_Ui == "videovolume")
		{
			// The pointer rests on the video, so its viewer shows; turned and selected, the selection bar lands on it too
			m_Editor.setTool(tools::ToolKind::Select);
			m_Editor.document().modify(s_SmokeVideo, [&](Object& p_Object)
			{
				p_Object.transform = Affine2::around(p_Object.transform.translation, Affine2::rotate(0.6)) * Affine2::around(p_Object.transform.translation, Affine2::scale(DVec2{ 2.2 })) * p_Object.transform;
			}, ObjectChange::Transform);
			m_Editor.selection().set(std::vector<ObjectId>{ s_SmokeVideo });
			m_Canvas.videos().seek(s_SmokeVideo, 1.0);
			if (const Object* l_Object = m_Editor.document().find(s_SmokeVideo))
			{
				const Vec2 l_Center{ m_Editor.camera().worldToScreen(l_Object->worldBounds().center()) };
				m_PointerInWindow = true;
				m_CursorOverUi = false;
				m_LastPointer = platform::PointerEvent{ .phase = platform::PointerPhase::Move, .device = platform::PointerDevice::Mouse, .position = l_Center };
			}
		}
		else if (l_Ui == "oriented")
		{
			m_Editor.setTool(tools::ToolKind::Select);
			m_Editor.selection().set(std::vector<ObjectId>{ s_SmokePictures.front() });
		}
		else if (l_Ui == "eraser")
		{
			m_Editor.setTool(tools::ToolKind::Eraser);
			openPopup(Popup::Eraser);
		}
		else if (l_Ui == "laser")
		{
			m_Editor.setTool(tools::ToolKind::Laser);
			m_Editor.laser().setTrailSeconds(3.f);
			openPopup(Popup::Laser);
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
	case 28:
	{
		// A few frames later the viewer's bar is known: rest the pointer on its speaker
		if (m_Options.smokeUi == "videovolume" && m_VideoBarRect.max.x > m_VideoBarRect.min.x)
		{
			m_LastPointer = platform::PointerEvent{ .phase = platform::PointerPhase::Move, .device = platform::PointerDevice::Mouse, .position = Vec2{ m_VideoBarRect.max.x - 20.f * l_S, m_VideoBarRect.center().y } };
			m_PointerInWindow = true;
		}
		else if (m_Options.smokeUi == "videovolume")
		{
			--m_SmokeStep;
		}
		return;
	}
	// ---- --smoke-ui=tools: the highlighter, shapes, ruler, lock, export and laser pointer on a fresh board
	case 29:
	{
		if (m_Options.smokeUi != "tools")
			return;
		m_Editor.newBoard();
		m_Editor.lookAt(DVec2{ 0.0 }, 1.0);
		m_Editor.setTool(tools::ToolKind::Pen);
		queueStroke(platform::PointerDevice::Mouse, { .color = Color::fromRgba8(0x1F1F1FFFu), .sizePoints = 3.f },
			sampleCurve(60, [&](const float p_T) { return l_C + Vec2{ (-420.f + p_T * 380.f) * l_S, (-250.f + 14.f * std::sin(p_T * 14.f)) * l_S }; }), {});
		return;
	}
	case 30:
	{
		if (m_Options.smokeUi != "tools")
			return;
		// The marker goes over the pen's line and stays see-through
		m_Editor.setTool(tools::ToolKind::Highlighter);
		m_Editor.highlighterBrush() = { .color = Color::fromRgba8(0xFDD835FFu), .sizePoints = 26.f };
		queueStroke(platform::PointerDevice::Mouse, {},
			sampleCurve(40, [&](const float p_T) { return l_C + Vec2{ (-380.f + p_T * 250.f) * l_S, (-252.f + 6.f * std::sin(p_T * 9.f)) * l_S }; }), {});
		return;
	}
	case 31:
	case 32:
	case 33:
	case 34:
	case 35:
	case 36:
	{
		if (m_Options.smokeUi != "tools")
			return;
		static constexpr std::array<ShapeKind, 6> KINDS{ ShapeKind::Rectangle, ShapeKind::Ellipse, ShapeKind::Triangle, ShapeKind::Diamond, ShapeKind::Arrow, ShapeKind::Line };
		const size_t l_Index = m_SmokeStep - 1 - 31;
		m_Editor.setTool(tools::ToolKind::Shape);
		m_Editor.shapeState().kind = KINDS[l_Index];
		m_Editor.brush() = { .color = Color::fromRgba8(l_Index % 2 == 0 ? 0x1E88E5FFu : 0xE53935FFu), .sizePoints = 4.f };
		const float l_X = (-60.f + static_cast<float>(l_Index) * 120.f) * l_S;
		queueStroke(platform::PointerDevice::Mouse, m_Editor.brush(), { l_C + Vec2{ l_X, -180.f * l_S }, l_C + Vec2{ l_X + 100.f * l_S, -80.f * l_S } }, {});
		return;
	}
	case 37:
	{
		if (m_Options.smokeUi != "tools")
			return;
		// The ruler: a wobbly pen line drawn along its lower edge comes out straight
		m_Editor.toggleRuler();
		m_Editor.setTool(tools::ToolKind::Pen);
		m_Editor.brush() = { .color = Color::fromRgba8(0x43A047FFu), .sizePoints = 4.f };
		const Ruler& l_Ruler = m_Editor.ruler();
		const RulerLine l_Edge = l_Ruler.edge(1);
		const Camera& l_Camera = m_Editor.camera();
		std::vector<Vec2> l_Points;
		for (int i = 0; i <= 50; ++i)
		{
			const double l_Along = -l_Ruler.length * 0.4 + l_Ruler.length * 0.8 * static_cast<double>(i) / 50.0;
			const DVec2 l_World = l_Edge.origin + l_Edge.direction * (l_Along + l_Ruler.length * 0.5) + l_Ruler.normal() * (4.0 * std::sin(i * 0.9) + 3.0);
			l_Points.push_back(Vec2{ l_Camera.worldToScreen(l_World) });
		}
		queueStroke(platform::PointerDevice::Mouse, m_Editor.brush(), l_Points, {});
		return;
	}
	case 38:
	{
		if (m_Options.smokeUi != "tools")
			return;
		const Object& l_Line = *m_Editor.document().objects().back();
		const Ruler& l_Ruler = m_Editor.ruler();
		const RulerLine l_Edge = l_Ruler.edge(1);
		double l_Worst = 0.0;
		const StrokeData* l_Stroke = l_Line.stroke();
		if (l_Stroke == nullptr || l_Stroke->points.size() < 2)
		{
			spdlog::error("Smoke test (ruler): the last object is not a stroke");
			m_Failed = true;
			return;
		}
		for (const StrokePoint& l_Point : l_Stroke->points)
		{
			const DVec2 l_World = l_Line.transform.apply(DVec2{ l_Point.position });
			l_Worst = std::max(l_Worst, std::abs(glm::dot(l_World - l_Edge.origin, l_Ruler.normal())));
		}
		if (l_Worst > 0.05)
		{
			spdlog::error("Smoke test (ruler): the stroke strays {:.3f} units from the ruler's edge", l_Worst);
			m_Failed = true;
		}
		else
		{
			spdlog::info("Smoke test: the ruler kept the stroke straight (worst {:.4f} units off)", l_Worst);
		}
		return;
	}
	case 39:
	{
		if (m_Options.smokeUi != "tools")
			return;
		// Lock the shapes, then try to move and delete them
		m_Editor.setTool(tools::ToolKind::Select);
		std::vector<ObjectId> l_Shapes;
		const auto l_Objects = m_Editor.document().objects();
		for (size_t i = 2; i + 1 < l_Objects.size(); ++i)
			l_Shapes.push_back(l_Objects[i]->id);
		m_Editor.selection().set(l_Shapes);
		const Affine2 l_Before = m_Editor.document().find(l_Shapes.front())->transform;
		const size_t l_Count = m_Editor.document().size();
		m_Editor.toggleSelectionLock();
		m_Editor.nudgeSelection(Vec2{ 40.f, 40.f });
		m_Editor.deleteSelection();
		const bool l_Locked = m_Editor.selectionLockState() == Editor::LockState::All;
		const bool l_Stayed = m_Editor.document().find(l_Shapes.front()) != nullptr && m_Editor.document().find(l_Shapes.front())->transform.translation == l_Before.translation && m_Editor.document().size() == l_Count;
		if (!l_Locked || !l_Stayed)
		{
			spdlog::error("Smoke test (lock): locked objects were moved or deleted");
			m_Failed = true;
		}
		else
		{
			spdlog::info("Smoke test: locked objects ignore nudging and deleting");
		}
		return;
	}
	case 40:
	{
		if (m_Options.smokeUi != "tools")
			return;
		m_Editor.clearSelection();
		m_ExportOptions = ExportOptions{ .selectionOnly = false, .scale = 1, .transparent = true };
		const ExportPlan l_ExportPlan = exportPlan();
		s_SmokeExportWidth = l_ExportPlan.extent.width;
		s_SmokeExportHeight = l_ExportPlan.extent.height;
		s_SmokeExportPath = std::filesystem::temp_directory_path() / "wb_smoke_export.png";
		removeFileQuiet(s_SmokeExportPath);
		startExport(s_SmokeExportPath);
		return;
	}
	case 41:
	{
		if (m_Options.smokeUi != "tools")
			return;
		if (m_ExportJob || m_ExportWrite.valid())
		{
			--m_SmokeStep; // wait for the picture to be drawn and written
			return;
		}
		std::vector<uint8_t> l_Png;
		const bool l_Read = readFile(s_SmokeExportPath, l_Png).ok;
		uint32_t l_Width = 0;
		uint32_t l_Height = 0;
		static constexpr std::array<uint8_t, 8> SIGNATURE{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		if (l_Read && l_Png.size() > 33 && std::equal(SIGNATURE.begin(), SIGNATURE.end(), l_Png.begin()))
		{
			const auto l_BigEndian = [&](const size_t p_At) { return (static_cast<uint32_t>(l_Png[p_At]) << 24) | (static_cast<uint32_t>(l_Png[p_At + 1]) << 16) | (static_cast<uint32_t>(l_Png[p_At + 2]) << 8) | l_Png[p_At + 3]; };
			l_Width = l_BigEndian(16);
			l_Height = l_BigEndian(20);
		}
		if (!l_Read || l_Width != s_SmokeExportWidth || l_Height != s_SmokeExportHeight)
		{
			spdlog::error("Smoke test (export): expected a {}x{} PNG, got {}x{} ({} bytes)", s_SmokeExportWidth, s_SmokeExportHeight, l_Width, l_Height, l_Png.size());
			m_Failed = true;
		}
		else
		{
			spdlog::info("Smoke test: exported a {}x{} PNG ({} bytes)", l_Width, l_Height, l_Png.size());
			if (!m_Options.screenshotPath.empty())
				std::filesystem::copy_file(s_SmokeExportPath, std::filesystem::path(m_Options.screenshotPath).replace_extension(".export.png"), std::filesystem::copy_options::overwrite_existing);
		}
		removeFileQuiet(s_SmokeExportPath);
		m_Editor.setTool(tools::ToolKind::Laser);
		return;
	}
	case 42:
	{
		if (m_Options.smokeUi != "tools")
			return;
		// The laser pointer keeps drawing a circle until the last frame, so its trail is there for the screenshot
		const uint64_t l_Frame = m_Stats.frameCount;
		if (l_Frame + 2 < m_Options.smokeTestFrames)
		{
			--m_SmokeStep;
			for (int i = 0; i < 3; ++i)
			{
				const float l_Angle = (static_cast<float>(l_Frame) * 3.f + static_cast<float>(i)) * 0.045f;
				platform::PointerEvent l_Event{
					.phase = l_Frame == 0 && i == 0 ? platform::PointerPhase::Down : platform::PointerPhase::Move,
					.device = platform::PointerDevice::Mouse,
					.button = platform::PointerButton::Primary,
					.buttons = platform::ButtonMask::Primary,
					.position = l_C + Vec2{ std::cos(l_Angle) * 90.f * l_S + 330.f * l_S, std::sin(l_Angle) * 90.f * l_S + 120.f * l_S },
					.timestampNs = SDL_GetTicksNS(),
				};
				if (!m_Editor.laser().pressed())
					l_Event.phase = platform::PointerPhase::Down;
				m_Editor.handlePointer(l_Event);
			}
		}
		return;
	}
	// ---- --smoke-ui=pdf: a two page PDF lands on the board as locked pictures
	case 43:
	{
		if (m_Options.smokeUi != "pdf")
			return;
		m_Editor.newBoard();
		m_Editor.lookAt(DVec2{ 0.0 }, 1.0);
		const std::filesystem::path l_Path = std::filesystem::temp_directory_path() / "wb_smoke.pdf";
		const std::string l_Pdf = makeTestPdf();
		if (const IoResult l_Written = writeFileAtomic(l_Path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(l_Pdf.data()), l_Pdf.size())); !l_Written.ok)
		{
			spdlog::error("Smoke test (pdf): {}", l_Written.error);
			m_Failed = true;
			return;
		}
		startPdfImport(l_Path);
		return;
	}
	case 44:
	{
		if (m_Options.smokeUi != "pdf")
			return;
		if (m_PdfJob.valid() || m_Canvas.images().busy())
		{
			--m_SmokeStep; // wait for the pages
			return;
		}
		size_t l_Locked = 0;
		for (const auto& l_Object : m_Editor.document().objects())
			l_Locked += l_Object->image() != nullptr && l_Object->locked ? 1 : 0;
		if (m_Editor.document().size() != 2 || l_Locked != 2)
		{
			spdlog::error("Smoke test (pdf): expected 2 locked pages, found {} objects ({} locked pictures)", m_Editor.document().size(), l_Locked);
			m_Failed = true;
		}
		else
		{
			const Vec2 l_Size = m_Editor.document().objects().front()->image()->size;
			spdlog::info("Smoke test: a PDF became 2 locked pages ({:.0f}x{:.0f} units each)", l_Size.x, l_Size.y);
		}
		removeFileQuiet(std::filesystem::temp_directory_path() / "wb_smoke.pdf");
		return;
	}
	default:
		return;
	}
}

// --perf-test=N: N strokes spread over a big board, a camera that pans and zooms, then a frame time report
void App::drivePerfTest()
{
	constexpr uint32_t WARMUP = 30;
	constexpr uint32_t FRAMES = 600;
	requestRedraw(1);
	if (m_PerfFrame == 0)
	{
		m_Editor.lookAt(DVec2{ 0.0 }, 1.0);
		const double l_Side = std::sqrt(static_cast<double>(m_Options.perfStrokes)) * 90.0;
		addStressStrokes(m_Options.perfStrokes, Rect::fromPoints(DVec2{ -l_Side * 0.5 }, DVec2{ l_Side * 0.5 }));
	}
	else
	{
		if (m_PerfFrame > WARMUP)
		{
			m_PerfCpuMs.push_back(m_Stats.cpuFrameMs);
			m_PerfGpuMs.push_back(m_Frames.gpuFrameMs());
		}
		const double l_T = static_cast<double>(m_PerfFrame) / 60.0;
		const double l_Radius = std::sqrt(static_cast<double>(m_Options.perfStrokes)) * 25.0;
		// First half: pan at 100%. Second half: zoom out and in over the whole board
		const double l_Zoom = m_PerfFrame < FRAMES / 2 ? 1.0 : std::exp(std::sin(l_T * 1.5) * 2.0 - 1.5);
		m_Editor.lookAt(DVec2{ std::cos(l_T) * l_Radius, std::sin(l_T * 0.7) * l_Radius }, l_Zoom);
	}
	++m_PerfFrame;
	if (m_PerfFrame < FRAMES)
		return;

	const auto l_Report = [](const char* p_Name, std::vector<double> p_Values)
	{
		std::sort(p_Values.begin(), p_Values.end());
		double l_Sum = 0.0;
		for (const double l_Value : p_Values)
			l_Sum += l_Value;
		const auto l_At = [&](const double p_Fraction) { return p_Values[std::min(p_Values.size() - 1, static_cast<size_t>(static_cast<double>(p_Values.size()) * p_Fraction))]; };
		spdlog::info("Perf {}: avg {:.3f} ms, median {:.3f}, p99 {:.3f}, max {:.3f}", p_Name, l_Sum / static_cast<double>(p_Values.size()), l_At(0.5), l_At(0.99), p_Values.back());
	};
	spdlog::info("Perf test: {} strokes, {} objects, {} frames", m_Options.perfStrokes, m_Editor.document().size(), m_PerfCpuMs.size());
	l_Report("CPU", m_PerfCpuMs);
	l_Report("GPU", m_PerfGpuMs);
	{
		const uint64_t l_Start = SDL_GetTicksNS();
		const std::vector<uint8_t> l_Bytes = m_Editor.saveBoard({});
		spdlog::info("Perf save: {:.1f} ms, {:.1f} MiB", static_cast<double>(SDL_GetTicksNS() - l_Start) * 1e-6, static_cast<double>(l_Bytes.size()) / (1024.0 * 1024.0));
	}
	m_Running = false;
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
