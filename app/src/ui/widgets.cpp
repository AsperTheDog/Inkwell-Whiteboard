// The widgets of ui::Context.
module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

module wb.ui.context;

import wb.math;
import wb.platform.input;
import wb.text.edit;
import wb.text.utf8;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;

namespace wb::ui
{
namespace
{
Color scaledAlpha(const Color p_Color, const float p_Factor)
{
	return withAlpha(p_Color, p_Color.a * p_Factor);
}

float luminance(const Color p_Color)
{
	return 0.2126f * p_Color.r + 0.7152f * p_Color.g + 0.0722f * p_Color.b;
}
} // namespace

bool Context::iconButton(const std::string_view p_Key, const Rect2 p_Rect, const Icon p_Icon, const bool p_Selected, const bool p_Enabled, const std::string_view p_Tooltip, const Color* p_Badge)
{
	const uint32_t l_Id = id(p_Key);
	Interaction l_State = interact(l_Id, p_Rect);
	if (!p_Enabled)
		l_State.clicked = false;
	const bool l_Hot = l_State.hovered && p_Enabled;
	const float l_Hover = anim(l_Id + 1, l_Hot ? 1.f : 0.f, 22.f);
	const float l_Press = anim(l_Id + 2, (l_State.held && p_Enabled) ? 1.f : 0.f, 34.f);
	const float l_Select = anim(l_Id + 3, p_Selected ? 1.f : 0.f, 20.f);

	const float l_Radius = std::min(p_Rect.width(), p_Rect.height()) * 0.3f;
	if (l_Select > 0.001f)
		m_Draw.rect(p_Rect, l_Radius, scaledAlpha(m_Theme.accentSoft, l_Select));
	if (l_Hover > 0.001f)
		m_Draw.rect(p_Rect, l_Radius, scaledAlpha(m_Theme.hover, l_Hover));
	if (l_Press > 0.001f)
		m_Draw.rect(p_Rect, l_Radius, scaledAlpha(m_Theme.pressed, l_Press));

	Color l_IconColor = mixColors(m_Theme.text, m_Theme.accent, l_Select);
	if (!p_Enabled)
		l_IconColor = m_Theme.textFaint;
	const Vec2 l_Center = p_Rect.center() + Vec2{ 0.f, p_Badge != nullptr ? -px(2.f) : 0.f };
	m_Draw.icon(p_Icon, l_Center, fontPx(20.f), l_IconColor);
	if (p_Badge != nullptr)
	{
		const Vec2 l_Dot{ p_Rect.center().x, p_Rect.max.y - px(7.f) };
		m_Draw.circle(l_Dot, px(4.f), *p_Badge);
		m_Draw.ring(l_Dot, px(4.f), withAlpha(m_Theme.text, 0.25f), 1.f);
	}

	noteTooltip(l_Id, p_Rect, p_Tooltip, l_State.hovered);
	return l_State.clicked;
}

bool Context::button(const std::string_view p_Key, const Rect2 p_Rect, const std::string_view p_Label, const ButtonStyle p_Style, const bool p_Enabled, const Icon p_Icon)
{
	const uint32_t l_Id = id(p_Key);
	Interaction l_State = interact(l_Id, p_Rect);
	if (!p_Enabled)
		l_State.clicked = false;
	const float l_Hover = anim(l_Id + 1, (l_State.hovered && p_Enabled) ? 1.f : 0.f, 22.f);
	const float l_Press = anim(l_Id + 2, (l_State.held && p_Enabled) ? 1.f : 0.f, 34.f);

	const float l_Radius = px(10.f);
	Color l_Text = m_Theme.text;
	switch (p_Style)
	{
	case ButtonStyle::Primary:
		m_Draw.rect(p_Rect, l_Radius, m_Theme.accent);
		l_Text = m_Theme.textOnAccent;
		break;
	case ButtonStyle::Danger:
		m_Draw.rect(p_Rect, l_Radius, m_Theme.danger);
		l_Text = m_Theme.textOnAccent;
		break;
	case ButtonStyle::Secondary:
		m_Draw.outline(p_Rect, l_Radius, m_Theme.divider, 1.f);
		break;
	case ButtonStyle::Ghost:
		break;
	}
	const bool l_Filled = p_Style == ButtonStyle::Primary || p_Style == ButtonStyle::Danger;
	const Color l_Overlay = l_Filled ? Color{ 1.f, 1.f, 1.f, 1.f } : m_Theme.hover;
	if (l_Hover > 0.001f)
		m_Draw.rect(p_Rect, l_Radius, l_Filled ? withAlpha(l_Overlay, 0.14f * l_Hover) : scaledAlpha(m_Theme.hover, l_Hover));
	if (l_Press > 0.001f)
		m_Draw.rect(p_Rect, l_Radius, l_Filled ? withAlpha(Color{ 0.f, 0.f, 0.f, 1.f }, 0.14f * l_Press) : scaledAlpha(m_Theme.pressed, l_Press));
	if (!p_Enabled)
		l_Text = m_Theme.textFaint;

	const int l_Px = fontPx(14.f);
	const float l_TextWidth = m_Font->measure(p_Label, l_Px);
	const float l_IconSpace = p_Icon != Icon::None ? px(18.f) + px(8.f) : 0.f;
	float l_X = p_Rect.center().x - (l_TextWidth + l_IconSpace) * 0.5f;
	if (p_Icon != Icon::None)
	{
		m_Draw.icon(p_Icon, Vec2{ l_X + px(9.f), p_Rect.center().y }, fontPx(18.f), l_Text);
		l_X += l_IconSpace;
	}
	m_Draw.text(Vec2{ l_X, p_Rect.center().y }, p_Label, l_Px, l_Text);
	return l_State.clicked;
}

bool Context::menuRow(const std::string_view p_Key, const Rect2 p_Rect, const Icon p_Icon, const std::string_view p_Label, const std::string_view p_Shortcut, const bool p_Enabled, const bool p_Checked)
{
	const uint32_t l_Id = id(p_Key);
	Interaction l_State = interact(l_Id, p_Rect);
	if (!p_Enabled)
		l_State.clicked = false;
	const float l_Hover = anim(l_Id + 1, (l_State.hovered && p_Enabled) ? 1.f : 0.f, 26.f);
	const float l_Press = anim(l_Id + 2, (l_State.held && p_Enabled) ? 1.f : 0.f, 34.f);
	if (l_Hover > 0.001f)
		m_Draw.rect(p_Rect, px(8.f), scaledAlpha(m_Theme.hover, l_Hover));
	if (l_Press > 0.001f)
		m_Draw.rect(p_Rect, px(8.f), scaledAlpha(m_Theme.pressed, l_Press));

	const Color l_Text = p_Enabled ? m_Theme.text : m_Theme.textFaint;
	const float l_CenterY = p_Rect.center().y;
	if (p_Icon != Icon::None)
		m_Draw.icon(p_Icon, Vec2{ p_Rect.min.x + px(20.f), l_CenterY }, fontPx(18.f), p_Enabled ? m_Theme.textMuted : m_Theme.textFaint);
	m_Draw.text(Vec2{ p_Rect.min.x + px(40.f), l_CenterY }, p_Label, fontPx(14.f), l_Text);
	float l_RightEdge = p_Rect.max.x - px(12.f);
	if (p_Checked)
	{
		m_Draw.icon(Icon::Check, Vec2{ l_RightEdge - px(8.f), l_CenterY }, fontPx(16.f), m_Theme.accent);
		l_RightEdge -= px(26.f);
	}
	if (!p_Shortcut.empty())
		m_Draw.text(Vec2{ l_RightEdge, l_CenterY }, p_Shortcut, fontPx(12.f), m_Theme.textFaint, TextAlign::Right);
	return l_State.clicked;
}

bool Context::swatch(const std::string_view p_Key, const Rect2 p_Rect, const Color p_Color, const bool p_Selected, const std::string_view p_Tooltip)
{
	const uint32_t l_Id = id(p_Key);
	const Interaction l_State = interact(l_Id, p_Rect);
	const float l_Hover = anim(l_Id + 1, l_State.hovered ? 1.f : 0.f, 26.f);
	const float l_Select = anim(l_Id + 3, p_Selected ? 1.f : 0.f, 24.f);
	const float l_Press = anim(l_Id + 2, l_State.held ? 1.f : 0.f, 34.f);

	const Vec2 l_Center = p_Rect.center();
	const float l_Radius = std::min(p_Rect.width(), p_Rect.height()) * 0.5f;
	const float l_Size = (l_Radius - px(3.f)) * (1.f + 0.07f * l_Hover - 0.06f * l_Press);
	if (l_Select > 0.001f)
		m_Draw.ring(l_Center, l_Radius - px(0.5f), scaledAlpha(m_Theme.accent, l_Select), px(2.f));
	m_Draw.circle(l_Center, l_Size, p_Color);
	m_Draw.ring(l_Center, l_Size, withAlpha(m_Theme.text, 0.18f), 1.f);
	if (l_Select > 0.5f)
	{
		const Color l_Mark = luminance(p_Color) > 0.62f ? Color{ 0.1f, 0.1f, 0.1f, 1.f } : Color{ 1.f, 1.f, 1.f, 1.f };
		m_Draw.icon(Icon::Check, l_Center, fontPx(14.f), scaledAlpha(l_Mark, l_Select));
	}
	noteTooltip(l_Id, p_Rect, p_Tooltip, l_State.hovered);
	return l_State.clicked;
}

bool Context::slider(const std::string_view p_Key, const Rect2 p_Rect, float& p_Value, const float p_Min, const float p_Max, const bool p_Logarithmic)
{
	const uint32_t l_Id = id(p_Key);
	const Interaction l_State = interact(l_Id, p_Rect);

	const float l_Inset = px(10.f);
	const float l_Left = p_Rect.min.x + l_Inset;
	const float l_Right = p_Rect.max.x - l_Inset;
	const auto l_ToFraction = [&](const float p_V)
	{
		if (p_Logarithmic)
			return std::log(p_V / p_Min) / std::log(p_Max / p_Min);
		return (p_V - p_Min) / (p_Max - p_Min);
	};
	const auto l_FromFraction = [&](const float p_T)
	{
		if (p_Logarithmic)
			return p_Min * std::pow(p_Max / p_Min, p_T);
		return p_Min + (p_Max - p_Min) * p_T;
	};

	bool l_Changed = false;
	if (l_State.held || l_State.clicked)
	{
		const float l_T = std::clamp((m_Pos.x - l_Left) / std::max(l_Right - l_Left, 1.f), 0.f, 1.f);
		const float l_New = std::clamp(l_FromFraction(l_T), p_Min, p_Max);
		if (l_New != p_Value)
		{
			p_Value = l_New;
			l_Changed = true;
		}
	}

	const float l_Fraction = std::clamp(l_ToFraction(p_Value), 0.f, 1.f);
	const float l_ThumbX = l_Left + (l_Right - l_Left) * l_Fraction;
	const float l_Y = p_Rect.center().y;
	const float l_Hover = anim(l_Id + 1, (l_State.hovered || l_State.held) ? 1.f : 0.f, 24.f);
	const float l_TrackHalf = px(2.5f);

	m_Draw.rect(Rect2{ Vec2{ l_Left, l_Y - l_TrackHalf }, Vec2{ l_Right, l_Y + l_TrackHalf } }, l_TrackHalf, m_Theme.track);
	m_Draw.rect(Rect2{ Vec2{ l_Left, l_Y - l_TrackHalf }, Vec2{ std::max(l_ThumbX, l_Left + l_TrackHalf * 2.f), l_Y + l_TrackHalf } }, l_TrackHalf, m_Theme.accent);

	const float l_ThumbRadius = px(8.f + 1.5f * l_Hover);
	m_Draw.shadow(Rect2::fromCenter(Vec2{ l_ThumbX, l_Y }, Vec2{ l_ThumbRadius * 2.f }), l_ThumbRadius, px(5.f), Vec2{ 0.f, px(1.5f) }, withAlpha(m_Theme.shadow, m_Theme.shadow.a * 1.4f));
	m_Draw.circle(Vec2{ l_ThumbX, l_Y }, l_ThumbRadius, Color{ 1.f, 1.f, 1.f, 1.f });
	m_Draw.ring(Vec2{ l_ThumbX, l_Y }, l_ThumbRadius, m_Theme.accent, px(2.f));
	return l_Changed;
}

bool Context::segmented(const std::string_view p_Key, const Rect2 p_Rect, const std::span<const std::string_view> p_Labels, const std::span<const Icon> p_Icons, int& p_Selected)
{
	const uint32_t l_Id = id(p_Key);
	const int l_Count = static_cast<int>(p_Labels.size());
	if (l_Count == 0)
		return false;
	const float l_Radius = px(10.f);
	const float l_Inset = px(3.f);
	m_Draw.rect(p_Rect, l_Radius, m_Theme.hover);

	const float l_SegmentWidth = (p_Rect.width() - l_Inset * 2.f) / static_cast<float>(l_Count);
	const float l_Index = anim(l_Id + 9, static_cast<float>(p_Selected), 24.f);
	const float l_PillX = p_Rect.min.x + l_Inset + l_Index * l_SegmentWidth;
	const Rect2 l_Pill{ Vec2{ l_PillX, p_Rect.min.y + l_Inset }, Vec2{ l_PillX + l_SegmentWidth, p_Rect.max.y - l_Inset } };
	m_Draw.shadow(l_Pill, l_Radius - l_Inset, px(4.f), Vec2{ 0.f, px(1.f) }, withAlpha(m_Theme.shadow, m_Theme.shadow.a * 0.7f));
	m_Draw.rect(l_Pill, l_Radius - l_Inset, m_Theme.panel);

	bool l_Changed = false;
	for (int i = 0; i < l_Count; ++i)
	{
		const Rect2 l_Segment{ Vec2{ p_Rect.min.x + l_Inset + static_cast<float>(i) * l_SegmentWidth, p_Rect.min.y }, Vec2{ p_Rect.min.x + l_Inset + static_cast<float>(i + 1) * l_SegmentWidth, p_Rect.max.y } };
		const Interaction l_State = interact(id(p_Key, i + 1), l_Segment);
		if (l_State.clicked && p_Selected != i)
		{
			p_Selected = i;
			l_Changed = true;
		}
		const float l_Active = std::clamp(1.f - std::abs(l_Index - static_cast<float>(i)), 0.f, 1.f);
		const Color l_Color = mixColors(m_Theme.textMuted, m_Theme.text, l_Active);
		const int l_Px = fontPx(13.5f);
		const float l_TextWidth = m_Font->measure(p_Labels[static_cast<size_t>(i)], l_Px);
		const bool l_HasIcon = static_cast<size_t>(i) < p_Icons.size() && p_Icons[static_cast<size_t>(i)] != Icon::None;
		const float l_IconSpace = l_HasIcon ? px(16.f) + px(6.f) : 0.f;
		float l_X = l_Segment.center().x - (l_TextWidth + l_IconSpace) * 0.5f;
		if (l_HasIcon)
		{
			m_Draw.icon(p_Icons[static_cast<size_t>(i)], Vec2{ l_X + px(8.f), l_Segment.center().y }, fontPx(16.f), l_Color);
			l_X += l_IconSpace;
		}
		m_Draw.text(Vec2{ l_X, l_Segment.center().y }, p_Labels[static_cast<size_t>(i)], l_Px, l_Color);
	}
	return l_Changed;
}

bool Context::colorPicker(const std::string_view p_Key, const Rect2 p_Placed, Hsv& p_Hsv)
{
	// Its gradients cannot be mirrored: it is placed mirrored and works in window coordinates
	const bool l_Island = m_Mirror;
	const Rect2 p_Rect = enterIsland(p_Placed);
	const float l_BarHeight = px(16.f);
	const float l_Gap = px(12.f);
	const Rect2 l_Square{ p_Rect.min, Vec2{ p_Rect.max.x, p_Rect.max.y - l_BarHeight - l_Gap } };
	const Rect2 l_Bar{ Vec2{ p_Rect.min.x, p_Rect.max.y - l_BarHeight }, p_Rect.max };
	bool l_Changed = false;

	const Interaction l_SquareState = interact(id(p_Key, 1), l_Square);
	if (l_SquareState.held || l_SquareState.clicked)
	{
		p_Hsv.s = std::clamp((m_Pos.x - l_Square.min.x) / l_Square.width(), 0.f, 1.f);
		p_Hsv.v = 1.f - std::clamp((m_Pos.y - l_Square.min.y) / l_Square.height(), 0.f, 1.f);
		l_Changed = true;
	}
	const Interaction l_BarState = interact(id(p_Key, 2), l_Bar.inflated(px(6.f)));
	if (l_BarState.held || l_BarState.clicked)
	{
		p_Hsv.h = std::clamp((m_Pos.x - l_Bar.min.x) / l_Bar.width(), 0.f, 0.9999f);
		l_Changed = true;
	}

	m_Draw.saturationValueSquare(l_Square, px(10.f), p_Hsv.h);
	m_Draw.outline(l_Square, px(10.f), withAlpha(m_Theme.text, 0.12f), 1.f);
	m_Draw.hueBar(l_Bar, l_BarHeight * 0.5f);

	const Color l_Current = hsvToRgb(p_Hsv);
	const Vec2 l_SquareThumb{ l_Square.min.x + p_Hsv.s * l_Square.width(), l_Square.min.y + (1.f - p_Hsv.v) * l_Square.height() };
	m_Draw.circle(l_SquareThumb, px(9.f), Color{ 1.f, 1.f, 1.f, 1.f });
	m_Draw.circle(l_SquareThumb, px(7.f), l_Current);
	m_Draw.ring(l_SquareThumb, px(9.5f), Color{ 0.f, 0.f, 0.f, 0.3f }, 1.f);

	const Vec2 l_BarThumb{ l_Bar.min.x + p_Hsv.h * l_Bar.width(), l_Bar.center().y };
	m_Draw.circle(l_BarThumb, px(10.f), Color{ 1.f, 1.f, 1.f, 1.f });
	m_Draw.circle(l_BarThumb, px(7.5f), hsvToRgb(Hsv{ p_Hsv.h, 1.f, 1.f }));
	m_Draw.ring(l_BarThumb, px(10.5f), Color{ 0.f, 0.f, 0.f, 0.3f }, 1.f);
	leaveIsland(l_Island);
	return l_Changed;
}

// ------------------------------------------------------------------------------------------------ text field

namespace
{
// Byte offset in p_Text whose caret is nearest to p_X (measured from the left of the text as drawn)
size_t indexAtX(const FontAtlas& p_Font, const std::string& p_Text, const int p_PixelSize, const float p_X)
{
	size_t l_Best = 0;
	float l_BestDistance = std::abs(p_Font.caretX(p_Text, 0, p_PixelSize) - p_X);
	size_t l_Index = 0;
	while (l_Index < p_Text.size())
	{
		l_Index = text::nextCluster(p_Text, l_Index);
		const float l_Distance = std::abs(p_Font.caretX(p_Text, l_Index, p_PixelSize) - p_X);
		if (l_Distance < l_BestDistance)
		{
			l_BestDistance = l_Distance;
			l_Best = l_Index;
		}
	}
	return l_Best;
}
} // namespace

bool Context::textField(const std::string_view p_Key, const Rect2 p_Placed, std::string& p_Text, const std::string_view p_Placeholder, const std::string_view p_Allowed, const size_t p_MaxBytes, const Icon p_Icon)
{
	// Text, caret and selection are all in window coordinates: the field is placed mirrored and works unmirrored
	const bool l_Island = m_Mirror;
	const Rect2 p_Rect = enterIsland(p_Placed);
	const uint32_t l_Id = id(p_Key);
	const Interaction l_State = interact(l_Id, p_Rect);

	// A press anywhere else leaves the field
	if (m_FieldId == l_Id && m_AnyPress && !hit(p_Rect, m_AnyPressPos))
		m_FieldId = 0;

	const int l_PixelSize = fontPx(13.5f);
	const float l_Pad = px(11.f);
	const float l_IconSpace = p_Icon != Icon::None ? px(24.f) : 0.f;
	const float l_TextLeft = p_Rect.min.x + l_Pad + l_IconSpace;
	const float l_TextWidth = std::max(p_Rect.max.x - l_Pad - l_TextLeft, 1.f);
	// Text that fits starts at the right edge in a right-to-left layout
	const auto l_Shift = [&](const std::string& p_Shown)
	{
		if (!m_RtlLayout)
			return 0.f;
		return std::max(l_TextWidth - m_Font->measure(p_Shown, l_PixelSize), 0.f);
	};

	const bool l_PressedHere = m_Pressed && hit(p_Rect, m_PressPos);
	if (l_PressedHere)
	{
		if (m_FieldId != l_Id)
		{
			m_FieldId = l_Id;
			m_FieldMulti = false;
			m_Field = text::TextEditor(p_Text);
			m_Field.selectAll();
			m_FieldAllowed = std::string(p_Allowed);
			m_FieldMaxBytes = p_MaxBytes;
			m_FieldScroll = 0.f;
		}
		else
		{
			m_Field.moveTo(indexAtX(*m_Font, m_Field.text(), l_PixelSize, m_PressPos.x - l_TextLeft - l_Shift(m_Field.text()) + m_FieldScroll), false);
		}
		m_FieldBlink = 0.0;
	}
	const bool l_Focused = m_FieldId == l_Id;
	if (l_Focused)
	{
		m_FieldSeen = true;
		m_FieldBlink += m_Dt;
		m_AnimatingNext = true;
		if (l_State.held && !l_PressedHere)
			m_Field.moveTo(indexAtX(*m_Font, m_Field.text(), l_PixelSize, m_Pos.x - l_TextLeft - l_Shift(m_Field.text()) + m_FieldScroll), true);
	}

	bool l_Changed = false;
	if (l_Focused && m_FieldDirty)
	{
		p_Text = m_Field.text();
		l_Changed = true;
		m_FieldDirty = false;
	}

	const float l_Hover = anim(l_Id + 1, l_State.hovered || l_Focused ? 1.f : 0.f, 24.f);
	const float l_Radius = px(10.f);
	m_Draw.rect(p_Rect, l_Radius, scaledAlpha(m_Theme.hover, 0.6f + 0.4f * l_Hover));
	if (l_Focused)
		m_Draw.outline(p_Rect, l_Radius, m_Theme.accent, 1.5f * m_Scale);
	if (p_Icon != Icon::None)
		m_Draw.icon(p_Icon, Vec2{ p_Rect.min.x + l_Pad + px(7.f), p_Rect.center().y }, fontPx(15.f), m_Theme.textMuted);

	const std::string& l_Shown = l_Focused ? m_Field.text() : p_Text;
	const float l_CaretOffset = l_Focused ? m_Font->caretX(l_Shown, m_Field.caret(), l_PixelSize) : 0.f;
	if (l_Focused)
	{
		if (l_CaretOffset - m_FieldScroll > l_TextWidth - px(2.f))
			m_FieldScroll = l_CaretOffset - l_TextWidth + px(2.f);
		if (l_CaretOffset < m_FieldScroll)
			m_FieldScroll = l_CaretOffset;
		m_FieldScroll = std::max(m_FieldScroll, 0.f);
	}
	m_Draw.pushClip(Rect2{ Vec2{ l_TextLeft, p_Rect.min.y }, Vec2{ l_TextLeft + l_TextWidth + px(2.f), p_Rect.max.y } });
	const float l_Origin = l_TextLeft + l_Shift(l_Shown) - (l_Focused ? m_FieldScroll : 0.f);
	if (l_Focused && m_Field.hasSelection())
	{
		const float l_From = m_Font->caretX(l_Shown, m_Field.selectionBegin(), l_PixelSize);
		const float l_To = m_Font->caretX(l_Shown, m_Field.selectionEnd(), l_PixelSize);
		const float l_HalfHeight = px(11.f);
		m_Draw.rect(Rect2{ Vec2{ l_Origin + std::min(l_From, l_To), p_Rect.center().y - l_HalfHeight }, Vec2{ l_Origin + std::max(l_From, l_To), p_Rect.center().y + l_HalfHeight } }, px(3.f), withAlpha(m_Theme.accent, 0.3f));
	}
	if (l_Shown.empty() && !l_Focused)
		m_Draw.text(Vec2{ l_TextLeft + l_Shift(std::string(p_Placeholder)), p_Rect.center().y }, p_Placeholder, l_PixelSize, m_Theme.textFaint);
	else
		m_Draw.text(Vec2{ l_Origin, p_Rect.center().y }, l_Shown, l_PixelSize, m_Theme.text);
	if (l_Focused && std::fmod(m_FieldBlink, 1.06) < 0.53)
	{
		const float l_CaretX = std::round(l_Origin + l_CaretOffset);
		m_Draw.line(Vec2{ l_CaretX, p_Rect.center().y - px(9.f) }, Vec2{ l_CaretX, p_Rect.center().y + px(9.f) }, std::max(1.f, 1.2f * m_Scale), m_Theme.text);
	}
	m_Draw.popClip();
	leaveIsland(l_Island);
	return l_Changed;
}

// ------------------------------------------------------------------------------------------------ text area

namespace
{
// Cuts p_Text into the lines it is drawn in: at every line feed, and before the word that would pass p_Width
// (inside a word that is longer than a line)
std::vector<std::pair<size_t, size_t>> wrapText(const FontAtlas& p_Font, const std::string& p_Text, const int p_PixelSize, const float p_Width)
{
	std::vector<std::pair<size_t, size_t>> l_Lines;
	size_t l_Position = 0;
	for (;;)
	{
		const size_t l_Feed = p_Text.find('\n', l_Position);
		const size_t l_HardEnd = l_Feed == std::string::npos ? p_Text.size() : l_Feed;
		size_t l_Start = l_Position;
		for (;;)
		{
			size_t l_Index = l_Start;
			size_t l_Break = std::string::npos;
			while (l_Index < l_HardEnd)
			{
				const size_t l_Next = text::nextCluster(p_Text, l_Index);
				if (l_Index > l_Start && p_Font.measure(p_Text.substr(l_Start, l_Next - l_Start), p_PixelSize) > p_Width)
					break;
				if (p_Text[l_Index] == ' ')
					l_Break = l_Next;
				l_Index = l_Next;
			}
			if (l_Index >= l_HardEnd)
			{
				l_Lines.emplace_back(l_Start, l_HardEnd);
				break;
			}
			const size_t l_Cut = l_Break != std::string::npos && l_Break > l_Start ? l_Break : l_Index;
			l_Lines.emplace_back(l_Start, l_Cut);
			l_Start = l_Cut;
		}
		if (l_Feed == std::string::npos)
			break;
		l_Position = l_Feed + 1;
	}
	return l_Lines;
}
} // namespace

const std::vector<Context::LineSpan>& Context::areaLines()
{
	const std::string& l_Text = m_Field.text();
	if (m_AreaWidth < 0.f)
		return m_AreaLines;
	if (m_AreaLines.empty() || m_AreaText != l_Text)
	{
		m_AreaText = l_Text;
		m_AreaLines.clear();
		for (const auto& [l_Begin, l_End] : wrapText(*m_Font, l_Text, m_AreaPixelSize, m_AreaWidth))
			m_AreaLines.push_back(LineSpan{ l_Begin, l_End });
	}
	return m_AreaLines;
}

bool Context::textArea(const std::string_view p_Key, const Rect2 p_Placed, std::string& p_Text, const std::string_view p_Placeholder, const size_t p_MaxBytes)
{
	const bool l_Island = m_Mirror;
	const Rect2 p_Rect = enterIsland(p_Placed);
	const uint32_t l_Id = id(p_Key);
	const Interaction l_State = interact(l_Id, p_Rect);
	if (m_FieldId == l_Id && m_AnyPress && !hit(p_Rect, m_AnyPressPos))
		m_FieldId = 0;

	const int l_PixelSize = fontPx(14.f);
	const float l_Pad = px(11.f);
	const float l_LineHeight = std::round(static_cast<float>(l_PixelSize) * 1.45f);
	const Rect2 l_Inner = Rect2{ p_Rect.min + Vec2{ l_Pad, px(9.f) }, p_Rect.max - Vec2{ l_Pad, px(9.f) } };
	const float l_Width = std::max(l_Inner.width(), 1.f);
	const float l_ViewHeight = std::max(l_Inner.height(), 1.f);

	const bool l_PressedHere = m_Pressed && hit(p_Rect, m_PressPos);
	if (l_PressedHere && m_FieldId != l_Id)
	{
		m_FieldId = l_Id;
		m_Field = text::TextEditor(p_Text);
		m_FieldMulti = true;
		m_FieldAllowed.clear();
		m_FieldMaxBytes = p_MaxBytes;
		m_AreaScroll = 0.f;
		m_AreaLines.clear();
	}
	const bool l_Focused = m_FieldId == l_Id;
	const std::string& l_Shown = l_Focused ? m_Field.text() : p_Text;

	// The lines of what is shown (the focused text lives in the editor, the rest in p_Text)
	std::vector<LineSpan> l_Lines;
	if (l_Focused)
	{
		if (m_AreaWidth != l_Width || m_AreaPixelSize != l_PixelSize)
			m_AreaLines.clear();
		m_AreaWidth = l_Width;
		m_AreaPixelSize = l_PixelSize;
		l_Lines = areaLines();
	}
	else
	{
		for (const auto& [l_Begin, l_End] : wrapText(*m_Font, l_Shown, l_PixelSize, l_Width))
			l_Lines.push_back(LineSpan{ l_Begin, l_End });
	}
	const float l_ContentHeight = static_cast<float>(l_Lines.size()) * l_LineHeight;
	const auto l_LineOf = [&](const size_t p_Byte)
	{
		size_t l_Line = 0;
		while (l_Line + 1 < l_Lines.size() && l_Lines[l_Line + 1].begin <= p_Byte)
			++l_Line;
		return l_Line;
	};
	const auto l_IndexAt = [&](const Vec2 p_Point)
	{
		const float l_Row = std::floor((p_Point.y - l_Inner.min.y + m_AreaScroll) / l_LineHeight);
		const size_t l_Line = static_cast<size_t>(std::clamp(l_Row, 0.f, static_cast<float>(l_Lines.size() - 1)));
		const LineSpan l_Span = l_Lines[l_Line];
		return l_Span.begin + indexAtX(*m_Font, l_Shown.substr(l_Span.begin, l_Span.end - l_Span.begin), l_PixelSize, p_Point.x - l_Inner.min.x);
	};

	if (l_Focused)
	{
		m_FieldSeen = true;
		m_FieldBlink += m_Dt;
		m_AnimatingNext = true;
		if (l_PressedHere)
		{
			m_Field.moveTo(l_IndexAt(m_PressPos), false);
			m_FieldBlink = 0.0;
		}
		else if (l_State.held)
		{
			m_Field.moveTo(l_IndexAt(m_Pos), true);
		}
	}

	bool l_Changed = false;
	if (l_Focused && m_FieldDirty)
	{
		p_Text = m_Field.text();
		l_Changed = true;
		m_FieldDirty = false;
	}

	// Keep the caret in view
	if (l_Focused)
	{
		const float l_CaretTop = static_cast<float>(l_LineOf(m_Field.caret())) * l_LineHeight;
		if (l_CaretTop + l_LineHeight - m_AreaScroll > l_ViewHeight)
			m_AreaScroll = l_CaretTop + l_LineHeight - l_ViewHeight;
		if (l_CaretTop < m_AreaScroll)
			m_AreaScroll = l_CaretTop;
	}
	m_AreaScroll = std::clamp(m_AreaScroll, 0.f, std::max(l_ContentHeight - l_ViewHeight, 0.f));

	const float l_Hover = anim(l_Id + 1, l_State.hovered || l_Focused ? 1.f : 0.f, 24.f);
	const float l_Radius = px(10.f);
	m_Draw.rect(p_Rect, l_Radius, scaledAlpha(m_Theme.hover, 0.6f + 0.4f * l_Hover));
	if (l_Focused)
		m_Draw.outline(p_Rect, l_Radius, m_Theme.accent, 1.5f * m_Scale);

	m_Draw.pushClip(Rect2{ Vec2{ l_Inner.min.x, p_Rect.min.y + px(2.f) }, Vec2{ l_Inner.max.x + px(2.f), p_Rect.max.y - px(2.f) } });
	if (l_Shown.empty() && !l_Focused)
		m_Draw.text(Vec2{ l_Inner.min.x, l_Inner.min.y + l_LineHeight * 0.5f }, p_Placeholder, l_PixelSize, m_Theme.textFaint);
	for (size_t i = 0; i < l_Lines.size(); ++i)
	{
		const float l_Top = l_Inner.min.y + static_cast<float>(i) * l_LineHeight - m_AreaScroll;
		if (l_Top + l_LineHeight < p_Rect.min.y || l_Top > p_Rect.max.y)
			continue;
		const LineSpan l_Span = l_Lines[i];
		const std::string l_Line = l_Shown.substr(l_Span.begin, l_Span.end - l_Span.begin);
		if (l_Focused && m_Field.hasSelection())
		{
			const size_t l_From = std::max(m_Field.selectionBegin(), l_Span.begin);
			const size_t l_To = std::min(m_Field.selectionEnd(), l_Span.end);
			const bool l_PastFeed = m_Field.selectionBegin() <= l_Span.end && m_Field.selectionEnd() > l_Span.end && l_Span.end < l_Shown.size() && l_Shown[l_Span.end] == '\n';
			if (l_From <= l_To && (l_From < l_To || l_PastFeed))
			{
				const float l_X0 = m_Font->caretX(l_Line, l_From - l_Span.begin, l_PixelSize);
				const float l_X1 = m_Font->caretX(l_Line, l_To - l_Span.begin, l_PixelSize) + (l_PastFeed ? px(5.f) : 0.f);
				m_Draw.rect(Rect2{ Vec2{ l_Inner.min.x + l_X0, l_Top }, Vec2{ l_Inner.min.x + l_X1, l_Top + l_LineHeight } }, px(2.f), withAlpha(m_Theme.accent, 0.3f));
			}
		}
		m_Draw.text(Vec2{ l_Inner.min.x, l_Top + l_LineHeight * 0.5f }, l_Line, l_PixelSize, m_Theme.text);
	}
	if (l_Focused && std::fmod(m_FieldBlink, 1.06) < 0.53)
	{
		const size_t l_CaretLine = l_LineOf(m_Field.caret());
		const LineSpan l_Span = l_Lines[l_CaretLine];
		const float l_CaretX = std::round(l_Inner.min.x + m_Font->caretX(l_Shown.substr(l_Span.begin, l_Span.end - l_Span.begin), m_Field.caret() - l_Span.begin, l_PixelSize));
		const float l_Top = l_Inner.min.y + static_cast<float>(l_CaretLine) * l_LineHeight - m_AreaScroll;
		m_Draw.line(Vec2{ l_CaretX, l_Top + px(2.f) }, Vec2{ l_CaretX, l_Top + l_LineHeight - px(2.f) }, std::max(1.f, 1.2f * m_Scale), m_Theme.text);
	}
	m_Draw.popClip();
	leaveIsland(l_Island);
	return l_Changed;
}

bool Context::keyEvent(const SDL_KeyboardEvent& p_Event)
{
	if (m_FieldId == 0)
		return false;
	const bool l_Ctrl = (p_Event.mod & SDL_KMOD_CTRL) != 0;
	const bool l_Shift = (p_Event.mod & SDL_KMOD_SHIFT) != 0;
	const SDL_Keycode l_Key = p_Event.key;
	m_FieldBlink = 0.0;
	if (m_FieldMulti && (l_Key == SDLK_RETURN || l_Key == SDLK_KP_ENTER) && !l_Ctrl)
	{
		textInput("\n");
		return true;
	}
	if (m_FieldMulti && (l_Key == SDLK_UP || l_Key == SDLK_DOWN || l_Key == SDLK_HOME || l_Key == SDLK_END) && !l_Ctrl)
	{
		const std::vector<LineSpan>& l_Lines = areaLines();
		if (!l_Lines.empty())
		{
			const std::string& l_Text = m_Field.text();
			size_t l_Line = 0;
			while (l_Line + 1 < l_Lines.size() && l_Lines[l_Line + 1].begin <= m_Field.caret())
				++l_Line;
			const LineSpan l_Span = l_Lines[l_Line];
			if (l_Key == SDLK_HOME)
				m_Field.moveTo(l_Span.begin, l_Shift);
			else if (l_Key == SDLK_END)
				m_Field.moveTo(l_Span.end, l_Shift);
			else if ((l_Key == SDLK_UP && l_Line == 0) || (l_Key == SDLK_DOWN && l_Line + 1 == l_Lines.size()))
				m_Field.moveTo(l_Key == SDLK_UP ? 0 : l_Text.size(), l_Shift);
			else
			{
				const float l_X = m_Font->caretX(l_Text.substr(l_Span.begin, l_Span.end - l_Span.begin), m_Field.caret() - l_Span.begin, m_AreaPixelSize);
				const LineSpan l_Other = l_Lines[l_Key == SDLK_UP ? l_Line - 1 : l_Line + 1];
				m_Field.moveTo(l_Other.begin + indexAtX(*m_Font, l_Text.substr(l_Other.begin, l_Other.end - l_Other.begin), m_AreaPixelSize, l_X), l_Shift);
			}
		}
		return true;
	}
	switch (l_Key)
	{
	case SDLK_ESCAPE:
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
	case SDLK_TAB:
		m_FieldId = 0;
		return true;
	case SDLK_LEFT:
		m_Field.moveLeft(l_Shift, l_Ctrl);
		return true;
	case SDLK_RIGHT:
		m_Field.moveRight(l_Shift, l_Ctrl);
		return true;
	case SDLK_HOME:
		m_Field.moveTo(0, l_Shift);
		return true;
	case SDLK_END:
		m_Field.moveTo(m_Field.text().size(), l_Shift);
		return true;
	case SDLK_BACKSPACE:
		m_FieldDirty = m_Field.backspace(l_Ctrl) || m_FieldDirty;
		return true;
	case SDLK_DELETE:
		m_FieldDirty = m_Field.erase(l_Ctrl) || m_FieldDirty;
		return true;
	default:
		break;
	}
	if (l_Ctrl)
	{
		switch (l_Key)
		{
		case SDLK_A:
			m_Field.selectAll();
			return true;
		case SDLK_C:
			if (m_Field.hasSelection())
				SDL_SetClipboardText(m_Field.selectedText().c_str());
			return true;
		case SDLK_X:
			if (m_Field.hasSelection())
			{
				SDL_SetClipboardText(m_Field.selectedText().c_str());
				m_FieldDirty = m_Field.deleteSelection() || m_FieldDirty;
			}
			return true;
		case SDLK_V:
			if (char* l_Text = SDL_GetClipboardText())
			{
				textInput(l_Text);
				SDL_free(l_Text);
			}
			return true;
		case SDLK_Z:
			l_Shift ? m_Field.redo() : m_Field.undo();
			m_FieldDirty = true;
			return true;
		default:
			return false;
		}
	}
	return l_Key < 0x40000000 && l_Key != SDLK_UNKNOWN;
}

void Context::textInput(const std::string_view p_Text)
{
	if (m_FieldId == 0)
		return;
	std::string l_Clean;
	for (const char l_Char : text::sanitize(p_Text))
	{
		if ((l_Char == '\n' && !m_FieldMulti) || l_Char == '\t')
			continue;
		if (!m_FieldAllowed.empty() && m_FieldAllowed.find(l_Char) == std::string::npos)
			continue;
		l_Clean.push_back(l_Char);
	}
	const size_t l_Selected = m_Field.hasSelection() ? m_Field.selectionEnd() - m_Field.selectionBegin() : 0;
	if (l_Clean.empty() || m_Field.text().size() - l_Selected + l_Clean.size() > m_FieldMaxBytes)
		return;
	m_FieldDirty = m_Field.insert(l_Clean) || m_FieldDirty;
	m_FieldBlink = 0.0;
}
} // namespace wb::ui
