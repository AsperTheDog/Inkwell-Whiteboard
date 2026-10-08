module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

module wb.ui.context;

import wb.math;
import wb.platform.input;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;

namespace wb::ui
{
namespace
{
constexpr double TOOLTIP_DELAY_SECONDS = 0.55;
} // namespace

void Context::init(FontAtlas& p_Font)
{
	m_Font = &p_Font;
}

uint32_t Context::id(const std::string_view p_Key)
{
	uint32_t l_Hash = 2166136261u;
	for (const char l_Char : p_Key)
		l_Hash = (l_Hash ^ static_cast<uint8_t>(l_Char)) * 16777619u;
	return l_Hash == 0 ? 1u : l_Hash;
}

uint32_t Context::id(const std::string_view p_Key, const int p_Index)
{
	const uint32_t l_Base = id(p_Key);
	return (l_Base ^ (static_cast<uint32_t>(p_Index) + 0x9E3779B9u)) * 16777619u | 1u;
}

int Context::fontPx(const float p_Points) const
{
	return std::max(1, static_cast<int>(std::lround(p_Points * m_Scale)));
}

void Context::beginFrame(const FrameParams& p_Params)
{
	m_Theme = p_Params.theme;
	m_Viewport = p_Params.viewport;
	m_Scale = p_Params.scale;
	m_Dt = p_Params.dt;
	m_Draw.begin(*m_Font, m_Viewport);
	m_HoverAnyPrev = m_HoverAny;
	m_HoverAny = false;
	m_Panels.clear();
	m_Layer = 0;
	m_AnimatingNext = false;
	m_HoverSeen = false;
	m_TooltipPending = false;
	m_TooltipShown = false;
}

void Context::endFrame()
{
	// Tooltip floats above everything
	if (m_TooltipShown && !m_TooltipText.empty())
	{
		const int l_Px = fontPx(12.5f);
		const float l_Pad = px(9.f);
		const float l_Width = m_Font->measure(m_TooltipText, l_Px) + l_Pad * 2.f;
		const float l_Height = px(28.f);
		float l_X = m_TooltipRect.center().x - l_Width * 0.5f;
		l_X = std::clamp(l_X, px(8.f), std::max(px(8.f), m_Viewport.x - l_Width - px(8.f)));
		const bool l_Above = m_TooltipRect.center().y > m_Viewport.y * 0.5f;
		const float l_Y = l_Above ? m_TooltipRect.min.y - l_Height - px(8.f) : m_TooltipRect.max.y + px(8.f);
		const Rect2 l_Box = Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Width, l_Height });
		const float l_Fade = anim(id("tooltip.fade"), 1.f, 30.f);
		const float l_Previous = m_Draw.opacity();
		m_Draw.setOpacity(l_Previous * l_Fade);
		m_Draw.rect(l_Box, px(8.f), m_Theme.tooltip);
		m_Draw.text(Vec2{ l_X + l_Pad, l_Box.center().y }, m_TooltipText, l_Px, m_Theme.tooltipText);
		m_Draw.setOpacity(l_Previous);
	}
	else
	{
		m_Anims[id("tooltip.fade")] = 0.f;
	}

	if (!m_HoverSeen)
	{
		m_HoverId = 0;
		m_HoverTime = 0.0;
	}
	if (m_Active != 0 && !m_Down)
		m_Active = 0;
	m_Pressed = false;
	m_Released = false;
	m_PrevPanels = m_Panels;
	m_Animating = m_AnimatingNext;
}

// ------------------------------------------------------------------------------------------------ input

bool Context::wantsPointer(const Vec2 p_Position) const
{
	if (m_Captured)
		return true;
	return std::ranges::any_of(m_PrevPanels, [&](const Rect2& p_Rect) { return p_Rect.contains(p_Position); });
}

bool Context::pointerEvent(const platform::PointerEvent& p_Event)
{
	m_Device = p_Event.device;
	m_Pos = p_Event.position;
	const bool l_Touch = p_Event.device == platform::PointerDevice::Touch;

	switch (p_Event.phase)
	{
	case platform::PointerPhase::Down:
	{
		m_HoverValid = true;
		if (!wantsPointer(p_Event.position))
			return false;
		m_Captured = true;
		if (p_Event.button == platform::PointerButton::Primary || p_Event.button == platform::PointerButton::None)
		{
			m_Pressed = true;
			m_PressPos = p_Event.position;
			m_Down = true;
		}
		return true;
	}
	case platform::PointerPhase::Move:
		m_HoverValid = !l_Touch || m_Captured;
		return m_Captured || wantsPointer(p_Event.position);
	case platform::PointerPhase::Up:
	case platform::PointerPhase::Cancel:
	{
		if (l_Touch)
			m_HoverValid = false;
		if (!m_Captured)
			return false;
		if (m_Down)
		{
			m_Released = true;
			m_ReleasePos = p_Event.position;
			m_Down = false;
		}
		if (p_Event.buttons == 0)
			m_Captured = false;
		return true;
	}
	}
	return false;
}

// ------------------------------------------------------------------------------------------------ animation

float Context::animFrom(const uint32_t p_Key, const float p_Initial, const float p_Target, const float p_Rate)
{
	float& l_Value = m_Anims.try_emplace(p_Key, p_Initial).first->second;
	const float l_Step = 1.f - std::exp(-p_Rate * static_cast<float>(m_Dt));
	l_Value += (p_Target - l_Value) * l_Step;
	if (std::abs(p_Target - l_Value) > 0.003f)
		m_AnimatingNext = true;
	else
		l_Value = p_Target;
	return l_Value;
}

float Context::anim(const uint32_t p_Key, const float p_Target, const float p_Rate)
{
	return animFrom(p_Key, p_Target, p_Target, p_Rate);
}

// ------------------------------------------------------------------------------------------------ layers

void Context::beginPanel(const Rect2 p_Rect)
{
	m_Panels.push_back(p_Rect);
	m_Layer = m_Panels.size();
}

void Context::panel(const Rect2 p_Rect, const float p_Radius)
{
	beginPanel(p_Rect);
	m_Draw.shadow(p_Rect, p_Radius, px(28.f), Vec2{ 0.f, px(8.f) }, m_Theme.shadow);
	m_Draw.shadow(p_Rect, p_Radius, px(5.f), Vec2{ 0.f, px(1.5f) }, withAlpha(m_Theme.shadow, m_Theme.shadow.a * 0.8f));
	m_Draw.rectBorder(p_Rect, p_Radius, m_Theme.panel, m_Theme.panelBorder, 1.f);
}

bool Context::coveredAbove(const size_t p_Layer, const Vec2 p_Position) const
{
	// Panels of the previous frame with a higher layer than p_Layer (layer n is index n - 1)
	for (size_t i = p_Layer; i < m_PrevPanels.size(); ++i)
	{
		if (m_PrevPanels[i].contains(p_Position))
			return true;
	}
	return false;
}

bool Context::hit(const Rect2 p_Rect, const Vec2 p_Position) const
{
	return p_Rect.contains(p_Position) && !coveredAbove(m_Layer, p_Position);
}

// ------------------------------------------------------------------------------------------------ interaction

Interaction Context::interact(const uint32_t p_Id, const Rect2 p_Rect)
{
	Interaction l_Result;
	const bool l_HoverNow = m_HoverValid && hit(p_Rect, m_Pos);

	if (m_Active == 0 && m_Pressed && hit(p_Rect, m_PressPos))
		m_Active = p_Id;

	l_Result.hovered = l_HoverNow && (m_Active == 0 || m_Active == p_Id);
	if (l_Result.hovered)
		m_HoverAny = true;
	if (m_Active == p_Id)
	{
		l_Result.held = true;
		if (m_Released)
		{
			l_Result.clicked = hit(p_Rect, m_ReleasePos);
			l_Result.held = false;
			m_Active = 0;
		}
		else if (!m_Down)
		{
			l_Result.held = false;
			m_Active = 0;
		}
	}
	return l_Result;
}

void Context::noteTooltip(const uint32_t p_Id, const Rect2 p_Rect, const std::string_view p_Text, const bool p_Hovered)
{
	if (!p_Hovered || p_Text.empty() || m_Device == platform::PointerDevice::Touch || m_Active != 0)
		return;
	m_HoverSeen = true;
	if (m_HoverId != p_Id)
	{
		m_HoverId = p_Id;
		m_HoverTime = 0.0;
	}
	else
	{
		m_HoverTime += m_Dt;
	}
	if (m_HoverTime >= TOOLTIP_DELAY_SECONDS)
	{
		m_TooltipShown = true;
		m_TooltipRect = p_Rect;
		m_TooltipText = std::string(p_Text);
	}
	else
	{
		m_TooltipPending = true;
	}
}

float Context::label(const Vec2 p_Position, const std::string_view p_Text, const float p_Points, const Color p_Color, const TextAlign p_Align)
{
	return m_Draw.text(p_Position, p_Text, fontPx(p_Points), p_Color, p_Align);
}

void Context::divider(const Vec2 p_A, const Vec2 p_B)
{
	m_Draw.line(p_A, p_B, 1.f, m_Theme.divider);
}
} // namespace wb::ui
