// App: text on the board. The caret and selection drawn over the text being typed, the format bar above it, the text
// popover (font list, style, size, colour) and importing fonts.
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <volk.h>

module wb.app;

import wb.doc.object;
import wb.editor;
import wb.editor.text_session;
import wb.io.file;
import wb.math;
import wb.platform.input;
import wb.text.fonts;
import wb.text.system;
import wb.tools.tool;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.renderer;
import wb.ui.theme;
import wb.view.camera;

namespace wb
{
namespace
{
using ui::Rect2;

constexpr float EDGE_MARGIN = 16.f;
constexpr float POPOVER_GAP = 12.f;
constexpr float LIST_ROW_POINTS = 34.f;
constexpr double CARET_PERIOD_SECONDS = 1.06;

Rect2 beginPopoverAt(ui::Context& p_Ui, const Rect2& p_Anchor, const float p_Width, const float p_Height, const bool p_Below)
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

void endPopoverAt(ui::Context& p_Ui)
{
	p_Ui.draw().setOpacity(1.f);
}

std::string lowered(std::string_view p_Text)
{
	std::string l_Out(p_Text);
	for (char& l_Char : l_Out)
		l_Char = static_cast<char>(std::tolower(static_cast<unsigned char>(l_Char)));
	return l_Out;
}

bool sameRgb(const Color p_A, const Color p_B)
{
	return p_A.r == p_B.r && p_A.g == p_B.g && p_A.b == p_B.b;
}
} // namespace

// ------------------------------------------------------------------------------------------------ canvas overlay

void App::buildTextOverlay()
{
	const std::optional<TextEditView> l_View = m_Editor.textEditView();
	if (!l_View)
	{
		m_CaretActivity = ~0ull;
		return;
	}
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const float l_Scale = m_Ui.scale();
	const Camera& l_Camera = m_Editor.camera();
	const auto l_ToScreen = [&](const DVec2 p_Layout) { return Vec2{ l_Camera.worldToScreen(l_View->toWorld(p_Layout)) }; };

	// The box
	const std::array<Vec2, 4> l_Corners{ l_ToScreen({ 0.0, 0.0 }), l_ToScreen({ l_View->size.x, 0.0 }), l_ToScreen({ l_View->size.x, l_View->size.y }), l_ToScreen({ 0.0, l_View->size.y }) };
	l_Draw.polyline(l_Corners, 1.5f * l_Scale, ui::withAlpha(l_Theme.accent, 0.55f), true);

	// Rotated or mirrored text selects with rounded bars; upright text with plain rectangles
	const bool l_Upright = std::abs(l_View->transform.linear[0].y) < 1e-6 && std::abs(l_View->transform.linear[1].x) < 1e-6 && l_View->transform.linear[0].x > 0.0 && l_View->transform.linear[1].y > 0.0;
	const auto l_Highlight = [&](const std::vector<Rect>& p_Rects, const Color p_Color)
	{
		for (const Rect& l_Rect : p_Rects)
		{
			if (l_Upright)
			{
				l_Draw.rect(Rect2{ l_ToScreen(l_Rect.min), l_ToScreen(l_Rect.max) }, 2.f * l_Scale, p_Color);
				continue;
			}
			const Vec2 l_A = l_ToScreen(DVec2{ l_Rect.min.x, (l_Rect.min.y + l_Rect.max.y) * 0.5 });
			const Vec2 l_B = l_ToScreen(DVec2{ l_Rect.max.x, (l_Rect.min.y + l_Rect.max.y) * 0.5 });
			const float l_Height = glm::length(l_ToScreen(DVec2{ l_Rect.min.x, l_Rect.max.y }) - l_ToScreen(l_Rect.min));
			const Vec2 l_Direction = glm::normalize(l_B - l_A + Vec2{ 1e-6f, 0.f });
			l_Draw.line(l_A + l_Direction * std::min(l_Height * 0.5f, glm::length(l_B - l_A) * 0.5f), l_B - l_Direction * std::min(l_Height * 0.5f, glm::length(l_B - l_A) * 0.5f), l_Height, p_Color);
		}
	};
	l_Highlight(l_View->selection, ui::withAlpha(l_Theme.accent, 0.3f));

	// Part still being composed by an input method: underlined
	for (const Rect& l_Rect : l_View->composition)
	{
		const Vec2 l_A = l_ToScreen(DVec2{ l_Rect.min.x, l_Rect.max.y });
		const Vec2 l_B = l_ToScreen(l_Rect.max);
		l_Draw.line(l_A, l_B, 1.5f * l_Scale, l_Theme.accent);
	}

	// Caret: blinks, but stays solid while the user is typing
	const uint64_t l_Now = SDL_GetTicksNS();
	if (l_View->activity != m_CaretActivity)
	{
		m_CaretActivity = l_View->activity;
		m_CaretResetNs = l_Now;
	}
	const double l_Phase = std::fmod(static_cast<double>(l_Now - m_CaretResetNs) * 1e-9, CARET_PERIOD_SECONDS);
	const Vec2 l_Top = l_ToScreen(DVec2{ l_View->caretX, l_View->caretTop });
	const Vec2 l_Bottom = l_ToScreen(DVec2{ l_View->caretX, l_View->caretTop + l_View->caretHeight });
	if (l_Phase < CARET_PERIOD_SECONDS * 0.5)
		l_Draw.line(l_Top, l_Bottom, std::max(1.5f * l_Scale, 0.04f * glm::length(l_Bottom - l_Top)), l_Theme.text);

	// Tell the input method where the text is (its candidate list opens next to it)
	const float l_Density = m_Window.pixelDensity();
	const SDL_Rect l_Area{ static_cast<int>(std::min(l_Top.x, l_Bottom.x) / l_Density), static_cast<int>(std::min(l_Top.y, l_Bottom.y) / l_Density), 2, std::max(1, static_cast<int>(glm::length(l_Bottom - l_Top) / l_Density)) };
	SDL_SetTextInputArea(m_Window.handle(), &l_Area, 0);
}

// ------------------------------------------------------------------------------------------------ format bar

void App::openTextPopup(const bool p_FromToolbar)
{
	if (m_Popup == Popup::Text)
	{
		closePopup("toggled");
		return;
	}
	openPopup(Popup::Text);
	m_TextPopupFromToolbar = p_FromToolbar;
	m_FontScrollToSelected = true;
	m_FontFilter.clear();
}

// A small bar above the text being typed: font, bold, italic, alignment, colour
void App::buildTextBar()
{
	m_TextBarRect = Rect2{};
	const std::optional<TextEditView> l_View = m_Editor.textEditView();
	const std::optional<Editor::CurrentText> l_Current = m_Editor.currentText();
	if (!l_View || !l_Current || modalOpen())
		return;
	const Camera& l_Camera = m_Editor.camera();
	const Vec2 l_Viewport = m_Ui.viewport();
	const TextData& l_Data = l_Current->data;

	// Screen box of the text
	Vec2 l_Min{ 1e30f }, l_Max{ -1e30f };
	for (const DVec2 l_Corner : { DVec2{ 0.0 }, DVec2{ l_View->size.x, 0.0 }, DVec2{ l_View->size.x, l_View->size.y }, DVec2{ 0.0, l_View->size.y } })
	{
		const Vec2 l_Point{ l_Camera.worldToScreen(l_View->toWorld(l_Corner)) };
		l_Min = glm::min(l_Min, l_Point);
		l_Max = glm::max(l_Max, l_Point);
	}

	const float l_Button = m_Ui.px(36.f);
	const float l_Pad = m_Ui.px(5.f);
	const float l_Gap = m_Ui.px(2.f);
	const int l_FamilyPx = m_Ui.fontPx(13.5f);
	const std::string l_Family = m_Ui.font().fit(l_Data.family, l_FamilyPx, m_Ui.px(120.f));
	const float l_FamilyWidth = m_Ui.font().measure(l_Family, l_FamilyPx) + m_Ui.px(40.f);
	const float l_Separator = m_Ui.px(11.f);
	const float l_Width = l_Pad * 2.f + l_FamilyWidth + l_Separator + 2.f * l_Button + l_Gap + l_Separator + 3.f * l_Button + 2.f * l_Gap + l_Separator + l_Button;
	const float l_Height = l_Button + l_Pad * 2.f;
	const float l_Margin = m_Ui.px(14.f);
	const float l_X = std::clamp((l_Min.x + l_Max.x) * 0.5f - l_Width * 0.5f, l_Margin, std::max(l_Margin, l_Viewport.x - l_Width - l_Margin));
	const float l_TopLimit = m_Ui.px(70.f);
	const float l_Above = l_Min.y - m_Ui.px(16.f) - l_Height;
	float l_Y = l_Above >= l_TopLimit ? l_Above : l_Max.y + m_Ui.px(16.f);
	l_Y = std::clamp(l_Y, l_TopLimit, std::max(l_TopLimit, l_Viewport.y - m_Ui.px(100.f) - l_Height));
	const Rect2 l_Bar = Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Width, l_Height });
	m_TextBarRect = l_Bar;
	m_Ui.panel(l_Bar, m_Ui.px(18.f));

	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	float l_Cursor = l_Bar.min.x + l_Pad;
	const float l_ButtonY = l_Bar.min.y + l_Pad;

	// Font chip: opens the full text popover
	{
		const Rect2 l_Chip = Rect2::fromPosSize(Vec2{ l_Cursor, l_ButtonY }, Vec2{ l_FamilyWidth, l_Button });
		m_TextAnchor = l_Chip;
		const uint32_t l_Id = ui::Context::id("textbar.font");
		const ui::Interaction l_State = m_Ui.interact(l_Id, l_Chip);
		const float l_Hover = m_Ui.anim(l_Id + 1, l_State.hovered || m_Popup == Popup::Text ? 1.f : 0.f, 22.f);
		if (l_Hover > 0.001f)
			l_Draw.rect(l_Chip, m_Ui.px(12.f), ui::withAlpha(l_Theme.hover, l_Theme.hover.a * l_Hover));
		l_Draw.text(Vec2{ l_Chip.min.x + m_Ui.px(12.f), l_Chip.center().y }, l_Family, l_FamilyPx, l_Theme.text);
		l_Draw.icon(ui::Icon::ChevronDown, Vec2{ l_Chip.max.x - m_Ui.px(14.f), l_Chip.center().y }, m_Ui.fontPx(13.f), l_Theme.textMuted);
		m_Ui.noteTooltip(l_Id, l_Chip, "Font, size and colour", l_State.hovered);
		if (l_State.clicked)
			openTextPopup(false);
		l_Cursor += l_FamilyWidth;
	}
	const auto l_Divider = [&]
	{
		const float l_Line = l_Cursor + l_Separator * 0.5f;
		l_Draw.line(Vec2{ l_Line, l_Bar.min.y + m_Ui.px(12.f) }, Vec2{ l_Line, l_Bar.max.y - m_Ui.px(12.f) }, 1.f, l_Theme.divider);
		l_Cursor += l_Separator;
	};
	const auto l_Next = [&]
	{
		const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ l_Cursor, l_ButtonY }, Vec2{ l_Button });
		l_Cursor += l_Button + l_Gap;
		return l_Rect;
	};
	l_Divider();
	if (m_Ui.iconButton("textbar.bold", l_Next(), ui::Icon::Bold, (l_Data.style & TextStyle::Bold) != 0, true, "Bold (Ctrl+B)"))
	{
		const bool l_On = (l_Data.style & TextStyle::Bold) == 0;
		m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.style = static_cast<uint8_t>(l_On ? (p_Data.style | TextStyle::Bold) : (p_Data.style & ~TextStyle::Bold)); });
		m_Editor.textState().style = static_cast<uint8_t>(l_On ? (m_Editor.textState().style | TextStyle::Bold) : (m_Editor.textState().style & ~TextStyle::Bold));
	}
	if (m_Ui.iconButton("textbar.italic", l_Next(), ui::Icon::Italic, (l_Data.style & TextStyle::Italic) != 0, true, "Italic (Ctrl+I)"))
	{
		const bool l_On = (l_Data.style & TextStyle::Italic) == 0;
		m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.style = static_cast<uint8_t>(l_On ? (p_Data.style | TextStyle::Italic) : (p_Data.style & ~TextStyle::Italic)); });
		m_Editor.textState().style = static_cast<uint8_t>(l_On ? (m_Editor.textState().style | TextStyle::Italic) : (m_Editor.textState().style & ~TextStyle::Italic));
	}
	l_Cursor -= l_Gap;
	l_Divider();
	constexpr std::array<std::pair<TextAlign, ui::Icon>, 3> ALIGNS{ { { TextAlign::Left, ui::Icon::AlignLeft }, { TextAlign::Center, ui::Icon::AlignCenter }, { TextAlign::Right, ui::Icon::AlignRight } } };
	constexpr std::array<const char*, 3> ALIGN_TIPS{ "Align left", "Centre", "Align right" };
	for (size_t i = 0; i < ALIGNS.size(); ++i)
	{
		if (m_Ui.iconButton(std::string("textbar.align") + std::to_string(i), l_Next(), ALIGNS[i].second, l_Data.align == ALIGNS[i].first, true, ALIGN_TIPS[i]))
		{
			const TextAlign l_Align = ALIGNS[i].first;
			m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.align = l_Align; });
			m_Editor.textState().align = l_Align;
		}
	}
	l_Cursor -= l_Gap;
	l_Divider();
	{
		const Rect2 l_Dot = Rect2::fromPosSize(Vec2{ l_Cursor, l_ButtonY }, Vec2{ l_Button });
		const uint32_t l_Id = ui::Context::id("textbar.color");
		const ui::Interaction l_State = m_Ui.interact(l_Id, l_Dot);
		const float l_Hover = m_Ui.anim(l_Id + 1, l_State.hovered ? 1.f : 0.f, 22.f);
		const float l_Radius = m_Ui.px(11.f + 1.5f * l_Hover);
		l_Draw.circle(l_Dot.center(), l_Radius, l_Data.color);
		l_Draw.ring(l_Dot.center(), l_Radius, ui::withAlpha(l_Theme.text, 0.2f), 1.f);
		m_Ui.noteTooltip(l_Id, l_Dot, "Colour", l_State.hovered);
		if (l_State.clicked)
		{
			m_TextAnchor = l_Dot;
			openTextPopup(false);
		}
	}
}

// ------------------------------------------------------------------------------------------------ popover

void App::buildTextPopover(const Rect2 p_Anchor)
{
	text::FontRegistry& l_Fonts = m_Editor.textSystem().fonts();
	tools::TextState& l_State = m_Editor.textState();
	tools::BrushState& l_Brush = m_Editor.brush();
	const std::optional<Editor::CurrentText> l_Current = m_Editor.currentText();
	const ui::Theme& l_Theme = m_Ui.theme();
	ui::DrawList& l_Draw = m_Ui.draw();

	const std::string l_Family = l_Current ? l_Current->data.family : l_State.family;
	const uint8_t l_Style = l_Current ? l_Current->data.style : l_State.style;
	const TextAlign l_Align = l_Current ? l_Current->data.align : l_State.align;
	const Color l_Color = l_Current ? l_Current->data.color : l_Brush.color;
	float l_Points = l_Current ? static_cast<float>(l_Current->data.fontSize * m_Editor.camera().zoom() * l_Current->scale) : l_State.sizePoints;

	const float l_Width = m_Ui.px(320.f);
	const float l_Pad = m_Ui.px(18.f);
	const float l_Inner = l_Width - l_Pad * 2.f;
	const float l_Row = m_Ui.px(LIST_ROW_POINTS);

	// As many list rows as the window leaves room for
	const float l_FixedPoints = 36.f + 46.f + 44.f + 29.f + 52.f + 66.f + 82.f + 38.f;
	const float l_AvailablePoints = m_Ui.viewport().y / m_Ui.scale() - 150.f - l_FixedPoints;
	const int l_Rows = std::clamp(static_cast<int>(l_AvailablePoints / LIST_ROW_POINTS), 3, 8);
	const float l_ListHeight = l_Row * static_cast<float>(l_Rows);
	const float l_Height = m_Ui.px(l_FixedPoints) + l_ListHeight;

	const Rect2 l_Rect = beginPopoverAt(m_Ui, p_Anchor, l_Width, l_Height, p_Anchor.min.y < m_Ui.viewport().y * 0.4f);
	m_Ui.panel(l_Rect, m_Ui.px(20.f));
	const float l_Left = l_Rect.min.x + l_Pad;
	float l_Y = l_Rect.min.y + l_Pad;

	// Search
	m_Ui.textField("text.search", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(38.f) }), m_FontFilter, "Search fonts", {}, 64, ui::Icon::Search);
	l_Y += m_Ui.px(38.f) + m_Ui.px(8.f);

	// Font list
	std::vector<const text::FamilyInfo*> l_Shown;
	{
		const std::string l_Needle = lowered(m_FontFilter);
		for (const text::FamilyInfo& l_Info : l_Fonts.families())
		{
			if (l_Needle.empty() || lowered(l_Info.name).find(l_Needle) != std::string::npos)
				l_Shown.push_back(&l_Info);
		}
	}
	const Rect2 l_List = Rect2::fromPosSize(Vec2{ l_Left - m_Ui.px(6.f), l_Y }, Vec2{ l_Inner + m_Ui.px(12.f), l_ListHeight });
	m_FontListRect = l_List;
	const float l_ContentHeight = l_Row * static_cast<float>(l_Shown.size());
	const float l_MaxScroll = std::max(0.f, l_ContentHeight - l_ListHeight);
	if (m_FontScrollToSelected)
	{
		m_FontScrollToSelected = false;
		for (size_t i = 0; i < l_Shown.size(); ++i)
		{
			if (lowered(l_Shown[i]->name) == lowered(l_Family))
				m_FontScroll = std::max(0.f, static_cast<float>(i) * l_Row - l_ListHeight * 0.5f + l_Row * 0.5f);
		}
	}
	m_FontScroll = std::clamp(m_FontScroll, 0.f, l_MaxScroll);
	const float l_Scroll = m_Ui.anim(ui::Context::id("font.scroll"), m_FontScroll, 30.f);

	if (l_Shown.empty())
		m_Ui.label(l_List.center(), "No font with that name", 13.f, l_Theme.textMuted, ui::TextAlign::Center);
	l_Draw.pushClip(l_List);
	const int l_NamePx = m_Ui.fontPx(15.f);
	const size_t l_FirstRow = static_cast<size_t>(std::max(0.f, std::floor(l_Scroll / l_Row)));
	for (size_t i = l_FirstRow; i < l_Shown.size(); ++i)
	{
		const float l_RowTop = l_List.min.y + static_cast<float>(i) * l_Row - l_Scroll;
		if (l_RowTop > l_List.max.y)
			break;
		const Rect2 l_RowRect{ Vec2{ l_List.min.x, std::max(l_RowTop, l_List.min.y) }, Vec2{ l_List.max.x - m_Ui.px(8.f), std::min(l_RowTop + l_Row, l_List.max.y) } };
		const bool l_Selected = lowered(l_Shown[i]->name) == lowered(l_Family);
		const uint32_t l_Id = ui::Context::id("font.row", static_cast<int>(i));
		const ui::Interaction l_Action = m_Ui.interact(l_Id, l_RowRect);
		const Rect2 l_Full = Rect2::fromPosSize(Vec2{ l_List.min.x, l_RowTop }, Vec2{ l_RowRect.width(), l_Row });
		const float l_Hover = m_Ui.anim(l_Id + 1, l_Action.hovered ? 1.f : 0.f, 24.f);
		if (l_Selected)
			l_Draw.rect(l_Full.shrunk(m_Ui.px(1.f)), m_Ui.px(10.f), l_Theme.accentSoft);
		else if (l_Hover > 0.001f)
			l_Draw.rect(l_Full.shrunk(m_Ui.px(1.f)), m_Ui.px(10.f), ui::withAlpha(l_Theme.hover, l_Theme.hover.a * l_Hover));

		// The name in the font itself (small fonts only: reading a huge file just for a preview is not worth it)
		ui::FontFace l_Face = ui::FontFace::Text;
		if (l_Fonts.isLightweight(l_Shown[i]->name))
		{
			const text::ResolvedFace l_Resolved = l_Fonts.resolve(l_Shown[i]->name, 0);
			if (l_Resolved.face != text::NO_FACE)
			{
				const auto l_Cached = m_FontPreviewFaces.find(l_Resolved.face);
				if (l_Cached != m_FontPreviewFaces.end())
				{
					l_Face = l_Cached->second;
				}
				else
				{
					size_t l_Size = 0;
					const uint8_t* l_Data = l_Fonts.faceData(l_Resolved.face, l_Size);
					l_Face = l_Data != nullptr ? m_FontAtlas.addFace(l_Data, l_Size) : ui::FontFace::Text;
					m_FontPreviewFaces[l_Resolved.face] = l_Face;
				}
			}
		}
		const std::string l_Name = m_Ui.font().fit(l_Shown[i]->name, l_NamePx, l_Full.width() - m_Ui.px(52.f));
		l_Draw.text(Vec2{ l_Full.min.x + m_Ui.px(12.f), l_Full.center().y }, l_Name, l_NamePx, l_Selected ? l_Theme.accent : l_Theme.text, ui::TextAlign::Left, l_Face);
		if (l_Selected)
			l_Draw.icon(ui::Icon::Check, Vec2{ l_Full.max.x - m_Ui.px(16.f), l_Full.center().y }, m_Ui.fontPx(15.f), l_Theme.accent);
		if (l_Action.clicked)
		{
			const std::string l_Picked = l_Shown[i]->name;
			l_State.family = l_Picked;
			m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.family = l_Picked; });
		}
	}
	l_Draw.popClip();

	// Scroll bar
	if (l_MaxScroll > 0.f)
	{
		const float l_Track = l_ListHeight - m_Ui.px(8.f);
		const float l_Thumb = std::max(m_Ui.px(28.f), l_Track * l_ListHeight / l_ContentHeight);
		const float l_ThumbTop = l_List.min.y + m_Ui.px(4.f) + (l_Track - l_Thumb) * (l_Scroll / l_MaxScroll);
		const Rect2 l_Grab = Rect2::fromPosSize(Vec2{ l_List.max.x - m_Ui.px(14.f), l_ThumbTop }, Vec2{ m_Ui.px(14.f), l_Thumb });
		const uint32_t l_Id = ui::Context::id("font.scrollbar");
		const ui::Interaction l_Action = m_Ui.interact(l_Id, l_Grab);
		if (l_Action.held)
		{
			const float l_T = std::clamp((m_Ui.pointerPosition().y - l_List.min.y - l_Thumb * 0.5f) / std::max(l_Track - l_Thumb, 1.f), 0.f, 1.f);
			m_FontScroll = l_T * l_MaxScroll;
		}
		const float l_Emphasis = m_Ui.anim(l_Id + 1, l_Action.hovered || l_Action.held ? 1.f : 0.f, 24.f);
		const float l_BarWidth = m_Ui.px(3.f + 2.f * l_Emphasis);
		l_Draw.rect(Rect2::fromPosSize(Vec2{ l_List.max.x - m_Ui.px(7.f) - l_BarWidth * 0.5f, l_ThumbTop }, Vec2{ l_BarWidth, l_Thumb }), l_BarWidth * 0.5f, ui::withAlpha(l_Theme.textMuted, 0.45f + 0.3f * l_Emphasis));
	}
	l_Y += l_ListHeight + m_Ui.px(8.f);

	if (m_Ui.button("text.addfont", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, m_Ui.px(36.f) }), "Add a font...", ui::ButtonStyle::Secondary, true, ui::Icon::Upload))
		showInsertFontDialog();
	l_Y += m_Ui.px(36.f) + m_Ui.px(14.f);
	m_Ui.divider(Vec2{ l_Left, l_Y }, Vec2{ l_Left + l_Inner, l_Y });
	l_Y += 1.f + m_Ui.px(14.f);

	// Bold, italic, alignment
	{
		const float l_Button = m_Ui.px(40.f);
		const Rect2 l_BoldRect = Rect2::fromPosSize(Vec2{ l_Left - m_Ui.px(2.f), l_Y }, Vec2{ l_Button });
		if (m_Ui.iconButton("text.bold", l_BoldRect, ui::Icon::Bold, (l_Style & TextStyle::Bold) != 0, true, "Bold (Ctrl+B)"))
		{
			const bool l_On = (l_Style & TextStyle::Bold) == 0;
			l_State.style = static_cast<uint8_t>(l_On ? (l_State.style | TextStyle::Bold) : (l_State.style & ~TextStyle::Bold));
			m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.style = static_cast<uint8_t>(l_On ? (p_Data.style | TextStyle::Bold) : (p_Data.style & ~TextStyle::Bold)); });
		}
		if (m_Ui.iconButton("text.italic", Rect2::fromPosSize(Vec2{ l_BoldRect.max.x + m_Ui.px(2.f), l_Y }, Vec2{ l_Button }), ui::Icon::Italic, (l_Style & TextStyle::Italic) != 0, true, "Italic (Ctrl+I)"))
		{
			const bool l_On = (l_Style & TextStyle::Italic) == 0;
			l_State.style = static_cast<uint8_t>(l_On ? (l_State.style | TextStyle::Italic) : (l_State.style & ~TextStyle::Italic));
			m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.style = static_cast<uint8_t>(l_On ? (p_Data.style | TextStyle::Italic) : (p_Data.style & ~TextStyle::Italic)); });
		}
		int l_AlignIndex = l_Align == TextAlign::Left ? 0 : (l_Align == TextAlign::Center ? 1 : 2);
		static constexpr std::array<std::string_view, 3> LABELS{ "", "", "" };
		static constexpr std::array<ui::Icon, 3> ICONS{ ui::Icon::AlignLeft, ui::Icon::AlignCenter, ui::Icon::AlignRight };
		const float l_AlignWidth = m_Ui.px(132.f);
		if (m_Ui.segmented("text.align", Rect2::fromPosSize(Vec2{ l_Left + l_Inner - l_AlignWidth, l_Y }, Vec2{ l_AlignWidth, l_Button }), LABELS, ICONS, l_AlignIndex))
		{
			const TextAlign l_NewAlign = l_AlignIndex == 0 ? TextAlign::Left : (l_AlignIndex == 1 ? TextAlign::Center : TextAlign::Right);
			l_State.align = l_NewAlign;
			m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.align = l_NewAlign; });
		}
		l_Y += l_Button + m_Ui.px(12.f);
	}

	// Size: as it looks on screen right now
	{
		const float l_LabelHeight = m_Ui.px(22.f);
		m_Ui.label(Vec2{ l_Left, l_Y + l_LabelHeight * 0.5f }, "Size", 13.f, l_Theme.textMuted);
		char l_Text[32];
		std::snprintf(l_Text, sizeof(l_Text), "%.0f pt", static_cast<double>(l_Points));
		m_Ui.label(Vec2{ l_Left + l_Inner, l_Y + l_LabelHeight * 0.5f }, l_Text, 13.f, l_Theme.text, ui::TextAlign::Right);
		l_Y += l_LabelHeight;
		const float l_Inset = m_Ui.px(10.f);
		if (m_Ui.slider("text.size", Rect2{ Vec2{ l_Left - l_Inset, l_Y }, Vec2{ l_Left + l_Inner + l_Inset, l_Y + m_Ui.px(34.f) } }, l_Points, 8.f, 300.f, true))
		{
			l_State.sizePoints = l_Points;
			m_Editor.setTextSizePoints(l_Points);
		}
		l_Y += m_Ui.px(34.f) + m_Ui.px(10.f);
	}

	// Colour
	{
		const auto l_SetColor = [&](const Color p_New)
		{
			l_Brush.color = Color{ p_New.r, p_New.g, p_New.b, 1.f };
			m_Editor.recolorSelection(l_Brush.color);
		};
		Color l_Picked;
		if (paletteGrid(Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, 2.f * m_Ui.px(30.f) + m_Ui.px(10.f) }), m_Ui.px(30.f), 6, l_Color, l_Picked))
			l_SetColor(l_Picked);
		l_Y += 2.f * m_Ui.px(30.f) + m_Ui.px(10.f) + m_Ui.px(12.f);

		const Rect2 l_Swatch = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ m_Ui.px(38.f) });
		l_Draw.circle(l_Swatch.center(), m_Ui.px(13.f), l_Color);
		l_Draw.ring(l_Swatch.center(), m_Ui.px(13.f), ui::withAlpha(l_Theme.text, 0.18f), 1.f);
		if (!m_Ui.wantsKeyboard())
			m_HexText = ui::colorToHex(l_Color);
		if (m_Ui.textField("text.hex", Rect2::fromPosSize(Vec2{ l_Left + m_Ui.px(48.f), l_Y }, Vec2{ l_Inner - m_Ui.px(48.f), m_Ui.px(38.f) }), m_HexText, "#RRGGBB", "0123456789abcdefABCDEF#", 7))
		{
			if (const std::optional<Color> l_Parsed = ui::colorFromHex(m_HexText); l_Parsed && !sameRgb(*l_Parsed, l_Color))
				l_SetColor(*l_Parsed);
		}
	}
	endPopoverAt(m_Ui);
}

// ------------------------------------------------------------------------------------------------ fonts

void App::showInsertFontDialog()
{
	if (m_DialogKind != DialogKind::None)
		return;
	static const SDL_DialogFileFilter s_Filters[] = { { "Fonts", "ttf;otf;ttc;otc" } };
	m_DialogKind = DialogKind::Fonts;
	SDL_ShowOpenFileDialog(&App::dialogCallback, this, m_Window.handle(), s_Filters, 1, m_LastDirectory.empty() ? nullptr : m_LastDirectory.c_str(), true);
}

void App::importFontFiles(const std::vector<std::string>& p_Paths)
{
	std::string l_Last;
	size_t l_Imported = 0;
	for (const std::string& l_Path : p_Paths)
	{
		std::string l_Error;
		const std::string l_Family = m_Editor.textSystem().fonts().importFont(pathFromUtf8(l_Path), l_Error);
		if (l_Family.empty())
		{
			showMessage(l_Error);
			continue;
		}
		l_Last = l_Family;
		++l_Imported;
	}
	if (l_Imported == 0)
		return;
	// Use the new font right away
	m_Editor.textState().family = l_Last;
	m_Editor.applyTextStyle([&](TextData& p_Data) { p_Data.family = l_Last; });
	m_FontFilter.clear();
	m_FontScrollToSelected = true;
	showToast(l_Imported == 1 ? "Added the font " + l_Last : "Added " + std::to_string(l_Imported) + " fonts");
	requestRedraw();
}
} // namespace wb
