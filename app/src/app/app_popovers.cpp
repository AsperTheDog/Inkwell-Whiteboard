// App: the popovers that open from the tool bar (pen, eraser, select) and from the selection bar (recolour).
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <volk.h>

module wb.app;

import wb.brush.stroke_builder;
import wb.editor;
import wb.math;
import wb.session;
import wb.tools.tool;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.renderer;
import wb.ui.theme;

namespace wb
{
namespace
{
using ui::Rect2;

constexpr float EDGE_MARGIN = 16.f;
constexpr float POPOVER_GAP = 12.f;

// Positions a popover next to p_Anchor (above by default) and starts its fade-in. Pair with endPopover().
Rect2 beginPopover(ui::Context& p_Ui, const Rect2& p_Anchor, const float p_Width, const float p_Height, const bool p_Below)
{
	const float l_Open = p_Ui.animFrom(ui::Context::id("popover.open"), 0.f, 1.f, 24.f);
	const Vec2 l_Viewport = p_Ui.viewport();
	const float l_Margin = p_Ui.px(EDGE_MARGIN);
	const float l_X = std::clamp(p_Anchor.center().x - p_Width * 0.5f, l_Margin, std::max(l_Margin, l_Viewport.x - p_Width - l_Margin));
	float l_Y = p_Below ? p_Anchor.max.y + p_Ui.px(POPOVER_GAP) : p_Anchor.min.y - p_Ui.px(POPOVER_GAP) - p_Height;
	l_Y = std::clamp(l_Y, l_Margin, std::max(l_Margin, l_Viewport.y - p_Height - l_Margin));
	l_Y += (1.f - l_Open) * p_Ui.px(8.f) * (p_Below ? -1.f : 1.f);
	p_Ui.draw().setOpacity(l_Open);
	return Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ p_Width, p_Height });
}

void endPopover(ui::Context& p_Ui)
{
	p_Ui.draw().setOpacity(1.f);
}

std::string formatNumber(const char* p_Format, const double p_Value)
{
	char l_Buffer[48];
	std::snprintf(l_Buffer, sizeof(l_Buffer), p_Format, p_Value);
	return l_Buffer;
}

// A little sample of what the brush draws: width follows a pressure curve when the pen is pressure sensitive
void drawStrokePreview(ui::Context& p_Ui, const Rect2& p_Box, const Color p_Color, const float p_SizePoints, const float p_Sensitivity)
{
	ui::DrawList& l_Draw = p_Ui.draw();
	constexpr int SEGMENTS = 56;
	const float l_Left = p_Box.min.x + p_Ui.px(22.f);
	const float l_Right = p_Box.max.x - p_Ui.px(22.f);
	const float l_Amplitude = std::min(p_Box.height() * 0.24f, p_Ui.px(10.f));
	Vec2 l_Previous{ 0.f };
	float l_PreviousRadius = 0.f;
	for (int i = 0; i <= SEGMENTS; ++i)
	{
		const float l_T = static_cast<float>(i) / static_cast<float>(SEGMENTS);
		const Vec2 l_Point{ l_Left + (l_Right - l_Left) * l_T, p_Box.center().y - std::sin(l_T * 2.f * static_cast<float>(PI)) * l_Amplitude };
		const float l_Pressure = std::max(std::pow(std::sin(l_T * static_cast<float>(PI)), 0.7f), 0.12f);
		const float l_Radius = std::max(std::min(p_SizePoints * p_Ui.scale(), p_Box.height() * 0.55f) * 0.5f * (1.f - p_Sensitivity + p_Sensitivity * l_Pressure), 0.5f);
		if (i > 0)
			l_Draw.line(l_Previous, l_Point, (l_Radius + l_PreviousRadius), p_Color);
		l_Previous = l_Point;
		l_PreviousRadius = l_Radius;
	}
}

std::vector<std::string> hintLines(ui::Context& p_Ui, const std::string_view p_Text, const float p_Width)
{
	return p_Ui.font().wrap(p_Text, p_Ui.fontPx(12.5f), p_Width);
}

float drawHint(ui::Context& p_Ui, const Vec2 p_TopLeft, const std::vector<std::string>& p_Lines)
{
	const float l_LineHeight = p_Ui.px(17.f);
	float l_Y = p_TopLeft.y + l_LineHeight * 0.5f;
	for (const std::string& l_Line : p_Lines)
	{
		p_Ui.draw().text(Vec2{ p_TopLeft.x, l_Y }, l_Line, p_Ui.fontPx(12.5f), p_Ui.theme().textMuted);
		l_Y += l_LineHeight;
	}
	return l_LineHeight * static_cast<float>(p_Lines.size());
}

bool sameRgb(const Color p_A, const Color p_B)
{
	return p_A.r == p_B.r && p_A.g == p_B.g && p_A.b == p_B.b;
}
} // namespace

bool App::paletteGrid(const Rect2 p_Area, const float p_Diameter, const int p_Columns, const std::optional<Color>& p_Selected, Color& p_Picked)
{
	const float l_GapX = p_Columns > 1 ? (p_Area.width() - static_cast<float>(p_Columns) * p_Diameter) / static_cast<float>(p_Columns - 1) : 0.f;
	const float l_GapY = m_Ui.px(12.f);
	bool l_Clicked = false;
	for (size_t i = 0; i < PALETTE.size(); ++i)
	{
		const int l_Column = static_cast<int>(i) % p_Columns;
		const int l_Row = static_cast<int>(i) / p_Columns;
		const Rect2 l_Rect = Rect2::fromPosSize(p_Area.min + Vec2{ static_cast<float>(l_Column) * (p_Diameter + l_GapX), static_cast<float>(l_Row) * (p_Diameter + l_GapY) }, Vec2{ p_Diameter });
		const Color l_Color = Color::fromRgba8(PALETTE[i]);
		if (m_Ui.swatch("palette." + std::to_string(i), l_Rect, l_Color, p_Selected && sameRgb(*p_Selected, l_Color)))
		{
			p_Picked = l_Color;
			l_Clicked = true;
		}
	}
	return l_Clicked;
}

void App::buildPopover()
{
	// A tool popup closes when the device that opened it switches to another tool (keyboard shortcuts included).
	// Other devices speaking up in the meantime (a stray mouse or touch event next to a tablet) must not close it.
	const tools::ToolKind l_Tool = m_Editor.deviceTool(m_PopupDevice);
	if ((m_Popup == Popup::Text && m_TextPopupFromToolbar && l_Tool != tools::ToolKind::Text) || (m_Popup == Popup::Text && !m_TextPopupFromToolbar && !m_Editor.currentText()) || (m_Popup == Popup::Pen && l_Tool != tools::ToolKind::Pen) || (m_Popup == Popup::Eraser && l_Tool != tools::ToolKind::Eraser) || (m_Popup == Popup::Select && l_Tool != tools::ToolKind::Select))
		closePopup("its device switched tools");
	if (m_Popup == Popup::SelectionColor && !m_Editor.hasSelection())
		closePopup("selection gone");

	switch (m_Popup)
	{
	case Popup::Pen:
		buildPenPopover(m_ToolRects[static_cast<size_t>(tools::ToolKind::Pen)]);
		break;
	case Popup::Eraser:
		buildEraserPopover(m_ToolRects[static_cast<size_t>(tools::ToolKind::Eraser)]);
		break;
	case Popup::Select:
		buildSelectPopover(m_ToolRects[static_cast<size_t>(tools::ToolKind::Select)]);
		break;
	case Popup::Text:
		buildTextPopover(m_TextPopupFromToolbar ? m_ToolRects[static_cast<size_t>(tools::ToolKind::Text)] : m_TextAnchor);
		break;
	case Popup::SelectionColor:
	{
		const float l_Pad = m_Ui.px(18.f);
		const float l_Diameter = m_Ui.px(32.f);
		const float l_Width = m_Ui.px(272.f);
		const float l_Height = l_Pad * 2.f + m_Ui.px(20.f) + (2.f * l_Diameter + m_Ui.px(12.f)) + m_Ui.px(16.f) + l_Diameter;
		const bool l_Below = m_SelectionBarRect.min.y < m_Ui.viewport().y * 0.45f;
		const Rect2 l_Rect = beginPopover(m_Ui, m_SelectionBarRect, l_Width, l_Height, l_Below);
		m_Ui.panel(l_Rect, m_Ui.px(20.f));
		float l_Y = l_Rect.min.y + l_Pad;
		const float l_Left = l_Rect.min.x + l_Pad;
		const float l_Inner = l_Width - l_Pad * 2.f;
		m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(10.f) }, "Recolour the selection", 12.5f, m_Ui.theme().textMuted);
		l_Y += m_Ui.px(20.f);
		Color l_Picked;
		if (paletteGrid(Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, 2.f * l_Diameter + m_Ui.px(12.f) }), l_Diameter, 6, std::nullopt, l_Picked))
			m_Editor.recolorSelection(l_Picked);
		l_Y += 2.f * l_Diameter + m_Ui.px(12.f) + m_Ui.px(16.f);
		const float l_GapX = (l_Inner - 6.f * l_Diameter) / 5.f;
		for (size_t i = 0; i < m_RecentColors.size(); ++i)
		{
			const Rect2 l_Slot = Rect2::fromPosSize(Vec2{ l_Left + static_cast<float>(i) * (l_Diameter + l_GapX), l_Y }, Vec2{ l_Diameter });
			if (m_RecentColors[i] == 0)
			{
				m_Ui.draw().ring(l_Slot.center(), l_Diameter * 0.5f - m_Ui.px(3.f), m_Ui.theme().divider, 1.5f * m_Ui.scale());
				continue;
			}
			const Color l_Color = Color::fromRgba8(m_RecentColors[i]);
			if (m_Ui.swatch("recolor.recent." + std::to_string(i), l_Slot, l_Color, false))
				m_Editor.recolorSelection(l_Color);
		}
		endPopover(m_Ui);
		break;
	}
	case Popup::None:
	case Popup::Menu:
		break;
	}
}

// ------------------------------------------------------------------------------------------------ pen

void App::buildPenPopover(const Rect2 p_Anchor)
{
	const float l_Width = m_Ui.px(308.f);
	const float l_Pad = m_Ui.px(18.f);
	const float l_Diameter = m_Ui.px(34.f);
	const float l_RowGap = m_Ui.px(12.f);
	const float l_LabelHeight = m_Ui.px(22.f);
	const float l_SliderHeight = m_Ui.px(34.f);
	const float l_PreviewHeight = m_Ui.px(62.f);
	tools::BrushState& l_Brush = m_Editor.brush();
	BrushSettings& l_Settings = m_Editor.brushSettings();
	const ui::Theme& l_Theme = m_Ui.theme();
	ui::DrawList& l_Draw = m_Ui.draw();

	float l_Height;
	if (!m_PickerOpen)
		l_Height = l_Pad * 2.f + (2.f * l_Diameter + l_RowGap) + m_Ui.px(14.f) + l_Diameter + m_Ui.px(16.f) + 1.f + m_Ui.px(14.f) + l_LabelHeight + l_SliderHeight + l_PreviewHeight + m_Ui.px(10.f) + l_LabelHeight + l_SliderHeight + l_LabelHeight + l_SliderHeight + m_Ui.px(8.f) + m_Ui.px(38.f) + m_Ui.px(10.f);
	else
		l_Height = l_Pad * 2.f + m_Ui.px(34.f) + m_Ui.px(12.f) + m_Ui.px(190.f) + m_Ui.px(14.f) + m_Ui.px(38.f) + m_Ui.px(16.f) + l_Diameter;

	const Rect2 l_Rect = beginPopover(m_Ui, p_Anchor, l_Width, l_Height, false);
	m_Ui.panel(l_Rect, m_Ui.px(20.f));
	const float l_Left = l_Rect.min.x + l_Pad;
	const float l_Inner = l_Width - l_Pad * 2.f;
	const float l_GapX = (l_Inner - 6.f * l_Diameter) / 5.f;
	float l_Y = l_Rect.min.y + l_Pad;

	const auto l_RecentRow = [&](const size_t p_FirstColumn)
	{
		for (size_t i = 0; i < m_RecentColors.size(); ++i)
		{
			const Rect2 l_Slot = Rect2::fromPosSize(Vec2{ l_Left + static_cast<float>(i + p_FirstColumn) * (l_Diameter + l_GapX), l_Y }, Vec2{ l_Diameter });
			if (m_RecentColors[i] == 0)
			{
				l_Draw.ring(l_Slot.center(), l_Diameter * 0.5f - m_Ui.px(3.f), l_Theme.divider, 1.5f * m_Ui.scale());
				continue;
			}
			const Color l_Color = Color::fromRgba8(m_RecentColors[i]);
			if (m_Ui.swatch("pen.recent." + std::to_string(i), l_Slot, l_Color, sameRgb(l_Color, l_Brush.color)))
				l_Brush.color = Color{ l_Color.r, l_Color.g, l_Color.b, 1.f };
		}
	};

	if (m_PickerOpen)
	{
		// Custom colour page
		if (m_Ui.iconButton("pen.back", Rect2::fromPosSize(Vec2{ l_Left - m_Ui.px(8.f), l_Y }, Vec2{ m_Ui.px(34.f) }), ui::Icon::ChevronLeft, false, true, "Back"))
		{
			pushRecentColor(l_Brush.color);
			m_PickerOpen = false;
		}
		m_Ui.label(Vec2{ l_Left + m_Ui.px(32.f), l_Y + m_Ui.px(17.f) }, "Custom colour", 15.f, l_Theme.text);
		const Vec2 l_Current{ l_Left + l_Inner - m_Ui.px(13.f), l_Y + m_Ui.px(17.f) };
		l_Draw.circle(l_Current, m_Ui.px(12.f), l_Brush.color);
		l_Draw.ring(l_Current, m_Ui.px(12.f), ui::withAlpha(l_Theme.text, 0.18f), 1.f);
		l_Y += m_Ui.px(34.f) + m_Ui.px(12.f);

		if (m_Ui.colorPicker("pen.picker", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(190.f) }), m_PickerHsv))
		{
			const Color l_New = ui::hsvToRgb(m_PickerHsv);
			l_Brush.color = Color{ l_New.r, l_New.g, l_New.b, 1.f };
		}
		l_Y += m_Ui.px(190.f) + m_Ui.px(14.f);

		// The value can be typed or pasted
		if (!m_Ui.wantsKeyboard())
			m_HexText = ui::colorToHex(l_Brush.color);
		if (m_Ui.textField("pen.hex", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(38.f) }), m_HexText, "#RRGGBB", "0123456789abcdefABCDEF#", 7))
		{
			if (const std::optional<Color> l_Parsed = ui::colorFromHex(m_HexText))
			{
				l_Brush.color = *l_Parsed;
				m_PickerHsv = ui::rgbToHsv(*l_Parsed);
			}
		}
		l_Y += m_Ui.px(38.f) + m_Ui.px(16.f);
		l_RecentRow(0);
		endPopover(m_Ui);
		return;
	}

	// Palette
	Color l_Picked;
	if (paletteGrid(Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, 2.f * l_Diameter + l_RowGap }), l_Diameter, 6, l_Brush.color, l_Picked))
		l_Brush.color = l_Picked;
	l_Y += 2.f * l_Diameter + l_RowGap + m_Ui.px(14.f);

	// Custom colour button, then the recently used custom colours
	{
		const Rect2 l_Custom = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Diameter });
		const uint32_t l_Id = ui::Context::id("pen.custom");
		const ui::Interaction l_State = m_Ui.interact(l_Id, l_Custom);
		const float l_Hover = m_Ui.anim(l_Id + 1, l_State.hovered ? 1.f : 0.f, 24.f);
		const float l_Inset = m_Ui.px(3.f) - m_Ui.px(1.5f) * l_Hover;
		l_Draw.hueBar(l_Custom.shrunk(l_Inset), l_Diameter * 0.5f);
		l_Draw.ring(l_Custom.center(), l_Diameter * 0.5f - l_Inset, ui::withAlpha(l_Theme.text, 0.18f), 1.f);
		l_Draw.icon(ui::Icon::Plus, l_Custom.center(), m_Ui.fontPx(18.f), Color{ 1.f, 1.f, 1.f, 1.f });
		m_Ui.noteTooltip(l_Id, l_Custom, "Custom colour", l_State.hovered);
		if (l_State.clicked)
		{
			const ui::Hsv l_FromBrush = ui::rgbToHsv(l_Brush.color);
			m_PickerHsv = ui::Hsv{ l_FromBrush.s < 0.03f ? m_PickerHsv.h : l_FromBrush.h, l_FromBrush.s, l_FromBrush.v };
			m_PickerOpen = true;
		}
	}
	l_RecentRow(1);
	l_Y += l_Diameter + m_Ui.px(16.f);
	m_Ui.divider(Vec2{ l_Left, l_Y }, Vec2{ l_Left + l_Inner, l_Y });
	l_Y += 1.f + m_Ui.px(14.f);

	// Size
	const auto l_Slider = [&](const char* p_Key, Rect2 p_Row, float& p_Value, const float p_Min, const float p_Max, const bool p_Log)
	{
		const float l_Inset = m_Ui.px(10.f);
		return m_Ui.slider(p_Key, Rect2{ Vec2{ p_Row.min.x - l_Inset, p_Row.min.y }, Vec2{ p_Row.max.x + l_Inset, p_Row.max.y } }, p_Value, p_Min, p_Max, p_Log);
	};
	m_Ui.label(Vec2{ l_Left, l_Y + l_LabelHeight * 0.5f }, "Size", 13.f, l_Theme.textMuted);
	m_Ui.label(Vec2{ l_Left + l_Inner, l_Y + l_LabelHeight * 0.5f }, formatNumber("%.1f pt", l_Brush.sizePoints), 13.f, l_Theme.text, ui::TextAlign::Right);
	l_Y += l_LabelHeight;
	l_Slider("pen.size", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_SliderHeight }), l_Brush.sizePoints, 0.5f, 40.f, true);
	l_Y += l_SliderHeight;
	const Rect2 l_Preview = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_PreviewHeight });
	l_Draw.rect(l_Preview, m_Ui.px(12.f), l_Theme.hover);
	drawStrokePreview(m_Ui, l_Preview, l_Brush.color, l_Brush.sizePoints * l_Brush.sizeScale, l_Settings.pressureSensitivity);
	l_Y += l_PreviewHeight + m_Ui.px(10.f);

	// Size multiplier and how the size follows the zoom
	m_Ui.label(Vec2{ l_Left, l_Y + l_LabelHeight * 0.5f }, "Size multiplier", 13.f, l_Theme.textMuted);
	m_Ui.label(Vec2{ l_Left + l_Inner, l_Y + l_LabelHeight * 0.5f }, formatNumber("%.2fx", l_Brush.sizeScale), 13.f, l_Theme.text, ui::TextAlign::Right);
	l_Y += l_LabelHeight;
	l_Slider("pen.sizeScale", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_SliderHeight }), l_Brush.sizeScale, 0.25f, 4.f, true);
	l_Y += l_SliderHeight + m_Ui.px(8.f);
	{
		int l_Mode = l_Brush.sizeMode == tools::BrushSizeMode::Screen ? 0 : 1;
		static constexpr std::array<std::string_view, 2> LABELS{ "Fixed on screen", "Fixed on board" };
		static constexpr std::array<ui::Icon, 2> NO_ICONS{ ui::Icon::None, ui::Icon::None };
		if (m_Ui.segmented("pen.sizeMode", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(38.f) }), LABELS, NO_ICONS, l_Mode))
			l_Brush.sizeMode = l_Mode == 0 ? tools::BrushSizeMode::Screen : tools::BrushSizeMode::Board;
		l_Y += m_Ui.px(38.f) + m_Ui.px(10.f);
	}

	// Pressure
	m_Ui.label(Vec2{ l_Left, l_Y + l_LabelHeight * 0.5f }, "Pen pressure", 13.f, l_Theme.textMuted);
	m_Ui.label(Vec2{ l_Left + l_Inner, l_Y + l_LabelHeight * 0.5f }, formatNumber("%.0f%%", l_Settings.pressureSensitivity * 100.0), 13.f, l_Theme.text, ui::TextAlign::Right);
	l_Y += l_LabelHeight;
	l_Slider("pen.pressure", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_SliderHeight }), l_Settings.pressureSensitivity, 0.f, 1.f, false);
	endPopover(m_Ui);
}

// ------------------------------------------------------------------------------------------------ eraser

void App::buildEraserPopover(const Rect2 p_Anchor)
{
	const float l_Width = m_Ui.px(284.f);
	const float l_Pad = m_Ui.px(18.f);
	const float l_Inner = l_Width - l_Pad * 2.f;
	const float l_LabelHeight = m_Ui.px(22.f);
	const float l_SliderHeight = m_Ui.px(34.f);
	const float l_PreviewHeight = m_Ui.px(100.f);
	tools::EraserState& l_Eraser = m_Editor.eraser();
	const ui::Theme& l_Theme = m_Ui.theme();

	const bool l_Segment = l_Eraser.mode == tools::EraserMode::Segment;
	const std::vector<std::string> l_Hint = hintLines(m_Ui, l_Segment ? "Cuts strokes exactly where the eraser touches them." : "Removes every whole stroke the eraser touches.", l_Inner);
	const float l_HintHeight = m_Ui.px(17.f) * static_cast<float>(l_Hint.size());
	const float l_Height = l_Pad * 2.f + m_Ui.px(40.f) + m_Ui.px(10.f) + m_Ui.px(34.f) + m_Ui.px(8.f) + l_LabelHeight + l_SliderHeight + m_Ui.px(8.f) + l_PreviewHeight;
	(void)l_HintHeight;

	const Rect2 l_Rect = beginPopover(m_Ui, p_Anchor, l_Width, l_Height, false);
	m_Ui.panel(l_Rect, m_Ui.px(20.f));
	const float l_Left = l_Rect.min.x + l_Pad;
	float l_Y = l_Rect.min.y + l_Pad;

	int l_Mode = l_Segment ? 0 : 1;
	static constexpr std::array<std::string_view, 2> LABELS{ "Segment", "Stroke" };
	static constexpr std::array<ui::Icon, 2> NO_ICONS{ ui::Icon::None, ui::Icon::None };
	if (m_Ui.segmented("eraser.mode", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(40.f) }), LABELS, NO_ICONS, l_Mode))
		l_Eraser.mode = l_Mode == 0 ? tools::EraserMode::Segment : tools::EraserMode::Stroke;
	l_Y += m_Ui.px(40.f) + m_Ui.px(10.f);
	drawHint(m_Ui, Vec2{ l_Left, l_Y }, l_Hint);
	l_Y += m_Ui.px(34.f) + m_Ui.px(8.f);

	m_Ui.label(Vec2{ l_Left, l_Y + l_LabelHeight * 0.5f }, "Size", 13.f, l_Theme.textMuted);
	m_Ui.label(Vec2{ l_Left + l_Inner, l_Y + l_LabelHeight * 0.5f }, formatNumber("%.0f pt", l_Eraser.sizePoints), 13.f, l_Theme.text, ui::TextAlign::Right);
	l_Y += l_LabelHeight;
	const float l_Inset = m_Ui.px(10.f);
	m_Ui.slider("eraser.size", Rect2{ Vec2{ l_Left - l_Inset, l_Y }, Vec2{ l_Left + l_Inner + l_Inset, l_Y + l_SliderHeight } }, l_Eraser.sizePoints, 4.f, 120.f, true);
	l_Y += l_SliderHeight + m_Ui.px(8.f);

	const Rect2 l_Preview = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_PreviewHeight });
	m_Ui.draw().rect(l_Preview, m_Ui.px(12.f), l_Theme.hover);
	const float l_Diameter = std::min(l_Eraser.sizePoints * m_Ui.scale(), l_PreviewHeight - m_Ui.px(12.f));
	m_Ui.draw().ring(l_Preview.center(), l_Diameter * 0.5f, l_Theme.text, 1.5f * m_Ui.scale());
	endPopover(m_Ui);
}

// ------------------------------------------------------------------------------------------------ select

void App::buildSelectPopover(const Rect2 p_Anchor)
{
	const float l_Width = m_Ui.px(270.f);
	const float l_Pad = m_Ui.px(18.f);
	const float l_Inner = l_Width - l_Pad * 2.f;
	tools::SelectState& l_Select = m_Editor.selectState();

	const bool l_Box = l_Select.mode == tools::SelectMode::Box;
	const std::vector<std::string> l_Hint = hintLines(m_Ui, l_Box ? "Drag a rectangle around what to select. Hold Shift to add to the selection." : "Draw around what to select. Anything the line touches is selected.", l_Inner);
	const bool l_Local = l_Select.space == tools::TransformSpace::Local;
	const std::vector<std::string> l_SpaceHint = hintLines(m_Ui, l_Local ? "The box of a single object turns with it, so you can stretch it along its own sides." : "The box always stays upright around the object, whatever its rotation. Text always follows its own axes.", l_Inner);
	const float l_Height = l_Pad * 2.f + m_Ui.px(40.f) + m_Ui.px(12.f) + m_Ui.px(17.f) * 3.f + m_Ui.px(14.f) + 1.f + m_Ui.px(14.f) + m_Ui.px(22.f) + m_Ui.px(40.f) + m_Ui.px(10.f) + m_Ui.px(17.f) * static_cast<float>(l_SpaceHint.size());

	const Rect2 l_Rect = beginPopover(m_Ui, p_Anchor, l_Width, l_Height, false);
	m_Ui.panel(l_Rect, m_Ui.px(20.f));
	const float l_Left = l_Rect.min.x + l_Pad;
	float l_Y = l_Rect.min.y + l_Pad;

	int l_Mode = l_Box ? 0 : 1;
	static constexpr std::array<std::string_view, 2> LABELS{ "Box", "Lasso" };
	static constexpr std::array<ui::Icon, 2> ICONS{ ui::Icon::BoxSelect, ui::Icon::Lasso };
	if (m_Ui.segmented("select.mode", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(40.f) }), LABELS, ICONS, l_Mode))
		l_Select.mode = l_Mode == 0 ? tools::SelectMode::Box : tools::SelectMode::Lasso;
	l_Y += m_Ui.px(40.f) + m_Ui.px(12.f);
	drawHint(m_Ui, Vec2{ l_Left, l_Y }, l_Hint);
	l_Y += m_Ui.px(17.f) * 3.f + m_Ui.px(14.f);
	m_Ui.divider(Vec2{ l_Left, l_Y }, Vec2{ l_Left + l_Inner, l_Y });
	l_Y += 1.f + m_Ui.px(14.f);

	m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(11.f) }, "Transform box", 13.f, m_Ui.theme().textMuted);
	l_Y += m_Ui.px(22.f);
	int l_Space = l_Local ? 1 : 0;
	static constexpr std::array<std::string_view, 2> SPACE_LABELS{ "Board axes", "Object axes" };
	static constexpr std::array<ui::Icon, 2> NO_ICONS{ ui::Icon::None, ui::Icon::None };
	if (m_Ui.segmented("select.space", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(40.f) }), SPACE_LABELS, NO_ICONS, l_Space))
		l_Select.space = l_Space == 0 ? tools::TransformSpace::Global : tools::TransformSpace::Local;
	l_Y += m_Ui.px(40.f) + m_Ui.px(10.f);
	drawHint(m_Ui, Vec2{ l_Left, l_Y }, l_SpaceHint);
	endPopover(m_Ui);
}
} // namespace wb
