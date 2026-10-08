module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

module wb.ui.draw;

import wb.math;
import wb.ui.font;

namespace wb::ui
{
namespace
{
constexpr float MODE_BOX = 0.f;
constexpr float MODE_SHADOW = 1.f;
constexpr float MODE_GLYPH = 2.f;
constexpr float MODE_CAPSULE = 3.f;
constexpr float MODE_SV_SQUARE = 4.f;
constexpr float MODE_HUE_BAR = 5.f;
} // namespace

void DrawList::begin(FontAtlas& p_Font, const Vec2 p_Viewport)
{
	m_Font = &p_Font;
	m_Viewport = p_Viewport;
	m_Opacity = 1.f;
	m_ClipStack.clear();
	m_ClipStack.push_back(Rect2{ Vec2{ 0.f }, p_Viewport });
	m_Prims.clear();
}

void DrawList::pushClip(const Rect2 p_Rect)
{
	const Rect2& l_Top = m_ClipStack.back();
	m_ClipStack.push_back(Rect2{ glm::max(l_Top.min, p_Rect.min), glm::min(l_Top.max, p_Rect.max) });
}

void DrawList::popClip()
{
	if (m_ClipStack.size() > 1)
		m_ClipStack.pop_back();
}

std::array<float, 4> DrawList::premultiplied(const Color p_Color) const
{
	const float l_Alpha = p_Color.a * m_Opacity;
	return { p_Color.r * l_Alpha, p_Color.g * l_Alpha, p_Color.b * l_Alpha, l_Alpha };
}

Prim& DrawList::add(const Rect2 p_Bounds, const float p_Mode)
{
	Prim& l_Prim = m_Prims.emplace_back();
	l_Prim.bounds = { p_Bounds.min.x, p_Bounds.min.y, p_Bounds.max.x, p_Bounds.max.y };
	const Rect2& l_Clip = m_ClipStack.back();
	l_Prim.clip = { l_Clip.min.x, l_Clip.min.y, l_Clip.max.x, l_Clip.max.y };
	l_Prim.misc[3] = p_Mode;
	return l_Prim;
}

void DrawList::rect(const Rect2 p_Rect, const float p_Radius, const Color p_Fill)
{
	rectBorder(p_Rect, p_Radius, p_Fill, p_Fill, 0.f);
}

void DrawList::rectBorder(const Rect2 p_Rect, const float p_Radius, const Color p_Fill, const Color p_Border, const float p_BorderWidth)
{
	if (p_Rect.width() <= 0.f || p_Rect.height() <= 0.f)
		return;
	const Vec2 l_Half = p_Rect.size() * 0.5f;
	const Vec2 l_Center = p_Rect.center();
	Prim& l_Prim = add(p_Rect.inflated(1.f), MODE_BOX);
	l_Prim.shape = { l_Center.x, l_Center.y, l_Half.x, l_Half.y };
	l_Prim.color = premultiplied(p_Fill);
	l_Prim.color2 = premultiplied(p_Border);
	l_Prim.misc[0] = std::min(p_Radius, std::min(l_Half.x, l_Half.y));
	l_Prim.misc[1] = p_BorderWidth;
}

void DrawList::outline(const Rect2 p_Rect, const float p_Radius, const Color p_Border, const float p_BorderWidth)
{
	rectBorder(p_Rect, p_Radius, Color{ 0.f, 0.f, 0.f, 0.f }, p_Border, p_BorderWidth);
}

void DrawList::circle(const Vec2 p_Center, const float p_Radius, const Color p_Fill)
{
	rect(Rect2::fromCenter(p_Center, Vec2{ p_Radius * 2.f }), p_Radius, p_Fill);
}

void DrawList::ring(const Vec2 p_Center, const float p_Radius, const Color p_Border, const float p_BorderWidth)
{
	outline(Rect2::fromCenter(p_Center, Vec2{ p_Radius * 2.f }), p_Radius, p_Border, p_BorderWidth);
}

void DrawList::shadow(const Rect2 p_Rect, const float p_Radius, const float p_Blur, const Vec2 p_Offset, const Color p_Color)
{
	const Rect2 l_Shifted = p_Rect.translated(p_Offset);
	const Vec2 l_Half = l_Shifted.size() * 0.5f;
	const Vec2 l_Center = l_Shifted.center();
	Prim& l_Prim = add(l_Shifted.inflated(p_Blur + 1.f), MODE_SHADOW);
	l_Prim.shape = { l_Center.x, l_Center.y, l_Half.x, l_Half.y };
	l_Prim.color = premultiplied(p_Color);
	l_Prim.misc[0] = std::min(p_Radius, std::min(l_Half.x, l_Half.y));
	l_Prim.misc[2] = p_Blur;
}

void DrawList::line(const Vec2 p_A, const Vec2 p_B, const float p_Width, const Color p_Color)
{
	const float l_Half = p_Width * 0.5f;
	Prim& l_Prim = add(Rect2{ glm::min(p_A, p_B) - Vec2{ l_Half + 1.f }, glm::max(p_A, p_B) + Vec2{ l_Half + 1.f } }, MODE_CAPSULE);
	l_Prim.shape = { p_A.x, p_A.y, p_B.x, p_B.y };
	l_Prim.color = premultiplied(p_Color);
	l_Prim.misc[0] = l_Half;
}

void DrawList::polyline(const std::span<const Vec2> p_Points, const float p_Width, const Color p_Color, const bool p_Closed)
{
	if (p_Points.size() < 2)
		return;
	for (size_t i = 0; i + 1 < p_Points.size(); ++i)
		line(p_Points[i], p_Points[i + 1], p_Width, p_Color);
	if (p_Closed && p_Points.size() > 2)
		line(p_Points.back(), p_Points.front(), p_Width, p_Color);
}

float DrawList::text(const Vec2 p_Position, const std::string_view p_Text, const int p_PixelSize, const Color p_Color, const TextAlign p_Align)
{
	const float l_Width = m_Font->measure(p_Text, p_PixelSize);
	float l_Pen = p_Position.x;
	if (p_Align == TextAlign::Center)
		l_Pen -= l_Width * 0.5f;
	else if (p_Align == TextAlign::Right)
		l_Pen -= l_Width;
	const float l_Baseline = std::round(p_Position.y + m_Font->capHeight(p_PixelSize) * 0.5f);
	const std::array<float, 4> l_Color = premultiplied(p_Color);

	size_t l_Index = 0;
	while (l_Index < p_Text.size())
	{
		const uint32_t l_Code = nextCodepoint(p_Text, l_Index);
		const GlyphQuad* l_Glyph = m_Font->glyph(FontFace::Text, l_Code, p_PixelSize);
		if (l_Glyph != nullptr && l_Glyph->size.x > 0.f)
		{
			const Vec2 l_Min{ std::round(l_Pen) + l_Glyph->offset.x, l_Baseline + l_Glyph->offset.y };
			Prim& l_Prim = add(Rect2{ l_Min, l_Min + l_Glyph->size }, MODE_GLYPH);
			l_Prim.uv = { l_Glyph->uv0.x, l_Glyph->uv0.y, l_Glyph->uv1.x, l_Glyph->uv1.y };
			l_Prim.color = l_Color;
		}
		l_Pen += m_Font->advance(FontFace::Text, l_Code, p_PixelSize);
	}
	return l_Width;
}

void DrawList::icon(const Icon p_Icon, const Vec2 p_Center, const int p_PixelSize, const Color p_Color)
{
	const GlyphQuad* l_Glyph = m_Font->glyph(FontFace::Icons, static_cast<uint32_t>(p_Icon), p_PixelSize);
	if (l_Glyph == nullptr || l_Glyph->size.x <= 0.f)
		return;
	// Centre the inked area (not the em box) on p_Center
	const Vec2 l_Min = glm::round(p_Center - l_Glyph->size * 0.5f);
	Prim& l_Prim = add(Rect2{ l_Min, l_Min + l_Glyph->size }, MODE_GLYPH);
	l_Prim.uv = { l_Glyph->uv0.x, l_Glyph->uv0.y, l_Glyph->uv1.x, l_Glyph->uv1.y };
	l_Prim.color = premultiplied(p_Color);
}

void DrawList::saturationValueSquare(const Rect2 p_Rect, const float p_Radius, const float p_Hue)
{
	const Vec2 l_Half = p_Rect.size() * 0.5f;
	const Vec2 l_Center = p_Rect.center();
	Prim& l_Prim = add(p_Rect.inflated(1.f), MODE_SV_SQUARE);
	l_Prim.shape = { l_Center.x, l_Center.y, l_Half.x, l_Half.y };
	l_Prim.color = premultiplied(Color{ 1.f, 1.f, 1.f, 1.f });
	l_Prim.misc[0] = p_Radius;
	l_Prim.misc[2] = p_Hue;
}

void DrawList::hueBar(const Rect2 p_Rect, const float p_Radius)
{
	const Vec2 l_Half = p_Rect.size() * 0.5f;
	const Vec2 l_Center = p_Rect.center();
	Prim& l_Prim = add(p_Rect.inflated(1.f), MODE_HUE_BAR);
	l_Prim.shape = { l_Center.x, l_Center.y, l_Half.x, l_Half.y };
	l_Prim.color = premultiplied(Color{ 1.f, 1.f, 1.f, 1.f });
	l_Prim.misc[0] = p_Radius;
}
} // namespace wb::ui
