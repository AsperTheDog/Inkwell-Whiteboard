// App: things drawn over the board that are not part of it: the ruler and the laser pointer's trail.
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <volk.h>

module wb.app;

import wb.editor;
import wb.math;
import wb.tools.laser;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;
import wb.view.camera;
import wb.view.ruler;

namespace wb
{
namespace
{
constexpr double PI_VALUE = 3.14159265358979323846;

struct TickLevel
{
	double step;      // world units between ticks
	float length;     // in points
	bool labelled;
};
} // namespace

// ------------------------------------------------------------------------------------------------ ruler

void App::buildRulerOverlay()
{
	if (!m_Editor.rulerVisible())
		return;
	const Ruler& l_Ruler = m_Editor.ruler();
	const Camera& l_Camera = m_Editor.camera();
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const float l_Scale = m_Ui.scale();
	const double l_Ppu = l_Camera.pixelsPerUnit();

	const auto l_Screen = [&](const DVec2 p_Local) { return Vec2{ l_Camera.worldToScreen(l_Ruler.toWorld(p_Local)) }; };
	const double l_Half = Ruler::THICKNESS * 0.5;
	const double l_HalfLength = l_Ruler.length * 0.5;
	const float l_Thickness = static_cast<float>(Ruler::THICKNESS * l_Ppu);

	// The body: a pill whose round ends sit inside the rectangle
	const Vec2 l_A = l_Screen({ -l_HalfLength + l_Half, 0.0 });
	const Vec2 l_B = l_Screen({ l_HalfLength - l_Half, 0.0 });
	const Color l_Body = ui::withAlpha(l_Theme.panel, 0.86f);
	l_Draw.line(l_A, l_B, l_Thickness + 3.f * l_Scale, ui::withAlpha(l_Theme.accent, 0.55f));
	l_Draw.line(l_A, l_B, l_Thickness, l_Body);

	// Ticks along the upper edge, in centimetres and millimetres, as dense as the zoom level allows
	const double l_Cm = Ruler::CENTIMETRE;
	const double l_CmPixels = l_Cm * l_Ppu / l_Camera.pixelScale();
	std::vector<TickLevel> l_Levels;
	if (l_CmPixels >= 34.0)
		l_Levels.push_back({ l_Cm * 0.1, 5.f, false });
	if (l_CmPixels >= 14.0)
		l_Levels.push_back({ l_Cm * 0.5, 9.f, false });
	if (l_CmPixels >= 5.0)
		l_Levels.push_back({ l_Cm, 14.f, l_CmPixels >= 26.0 });
	else if (l_CmPixels * 5.0 >= 10.0)
		l_Levels.push_back({ l_Cm * 5.0, 14.f, l_CmPixels * 5.0 >= 28.0 });
	else
		l_Levels.push_back({ l_Cm * 10.0, 14.f, l_CmPixels * 10.0 >= 28.0 });

	const double l_Start = -l_HalfLength + l_Half; // the first tick sits where the flat part of the body begins
	const int l_TextPx = m_Ui.fontPx(11.f);
	const Color l_Ink = ui::withAlpha(l_Theme.text, 0.7f);
	const Vec2 l_Viewport = m_Ui.viewport();
	for (const TickLevel& l_Level : l_Levels)
	{
		const double l_Span = (l_HalfLength - l_Half) - l_Start;
		const int l_Count = static_cast<int>(std::floor(l_Span / l_Level.step + 1e-6));
		if (l_Count > 6000)
			continue;
		for (int i = 0; i <= l_Count; ++i)
		{
			// Coarser levels draw over the finer ones at the same place, so skip those
			const double l_X = l_Start + l_Level.step * i;
			const Vec2 l_Top = l_Screen({ l_X, -l_Half });
			if (l_Top.x < -50.f || l_Top.y < -50.f || l_Top.x > l_Viewport.x + 50.f || l_Top.y > l_Viewport.y + 50.f)
				continue;
			const Vec2 l_Tick = l_Screen({ l_X, -l_Half + static_cast<double>(l_Level.length) * l_Scale / l_Ppu });
			l_Draw.line(l_Top, l_Tick, 1.2f * l_Scale, l_Ink);
			if (l_Level.labelled)
			{
				const double l_Label = l_Level.step * i / l_Cm;
				const Vec2 l_At = l_Screen({ l_X, -l_Half + 24.0 * l_Scale / l_Ppu });
				l_Draw.text(l_At, std::to_string(static_cast<int>(std::lround(l_Label))), l_TextPx, ui::withAlpha(l_Theme.textMuted, 0.9f), ui::TextAlign::Center);
			}
		}
	}

	// The knob that turns it
	const Vec2 l_Knob = Vec2{ l_Camera.worldToScreen(l_Ruler.knobCenter()) };
	const float l_KnobRadius = static_cast<float>(Ruler::knobRadius() * l_Ppu);
	const bool l_Turning = m_Editor.rulerTurning();
	l_Draw.circle(l_Knob, l_KnobRadius, l_Turning ? l_Theme.accent : l_Theme.handleFill);
	l_Draw.ring(l_Knob, l_KnobRadius, l_Theme.accent, 1.5f * l_Scale);
	l_Draw.icon(ui::Icon::Repeat, l_Knob, std::max(8, static_cast<int>(l_KnobRadius * 1.2f)), l_Turning ? l_Theme.textOnAccent : l_Theme.accent);

	if (l_Turning)
	{
		double l_Degrees = std::fmod(l_Ruler.angle * 180.0 / PI_VALUE + 360.0, 360.0);
		if (l_Degrees > 180.0)
			l_Degrees -= 360.0;
		const std::string l_Text = std::to_string(static_cast<int>(std::lround(l_Degrees))) + " deg";
		const Vec2 l_At = Vec2{ l_Camera.worldToScreen(l_Ruler.center) } + Vec2{ 0.f, l_Thickness * 0.5f + 22.f * l_Scale };
		const float l_Width = m_Ui.font().measure(l_Text, m_Ui.fontPx(13.f)) + 20.f * l_Scale;
		l_Draw.rect(ui::Rect2::fromCenter(l_At, Vec2{ l_Width, 26.f * l_Scale }), 13.f * l_Scale, l_Theme.tooltip);
		l_Draw.text(l_At, l_Text, m_Ui.fontPx(13.f), l_Theme.tooltipText, ui::TextAlign::Center);
	}
}

// ------------------------------------------------------------------------------------------------ laser pointer

void App::buildLaserOverlay()
{
	const tools::LaserTool& l_Laser = m_Editor.laser();
	const std::deque<tools::LaserPoint>& l_Trail = l_Laser.trail();
	if (l_Trail.empty())
		return;
	ui::DrawList& l_Draw = m_Ui.draw();
	const Camera& l_Camera = m_Editor.camera();
	const float l_Scale = m_Ui.scale();
	const uint64_t l_Now = SDL_GetTicksNS();
	constexpr Color LASER{ 1.f, 0.16f, 0.14f, 1.f };

	const auto l_Fade = [&](const tools::LaserPoint& p_Point)
	{
		const double l_Age = static_cast<double>(l_Now > p_Point.timeNs ? l_Now - p_Point.timeNs : 0) / static_cast<double>(l_Laser.lifetimeNs());
		const float l_T = static_cast<float>(std::clamp(1.0 - l_Age, 0.0, 1.0));
		return l_T * l_T * (3.f - 2.f * l_T);
	};

	for (size_t i = 1; i < l_Trail.size(); ++i)
	{
		if (l_Trail[i].newStroke)
			continue;
		const float l_Alpha = std::min(l_Fade(l_Trail[i]), l_Fade(l_Trail[i - 1]));
		if (l_Alpha <= 0.01f)
			continue;
		const Vec2 l_P0{ l_Camera.worldToScreen(l_Trail[i - 1].world) };
		const Vec2 l_P1{ l_Camera.worldToScreen(l_Trail[i].world) };
		l_Draw.line(l_P0, l_P1, (6.f + 12.f * l_Alpha) * l_Scale, ui::withAlpha(LASER, 0.16f * l_Alpha));
		l_Draw.line(l_P0, l_P1, (2.f + 3.f * l_Alpha) * l_Scale, ui::withAlpha(LASER, 0.95f * l_Alpha));
	}

	// The dot where the pointer is
	if (l_Laser.pressed())
	{
		const Vec2 l_Head{ l_Camera.worldToScreen(l_Trail.back().world) };
		l_Draw.circle(l_Head, 15.f * l_Scale, ui::withAlpha(LASER, 0.22f));
		l_Draw.circle(l_Head, 8.f * l_Scale, ui::withAlpha(LASER, 0.9f));
		l_Draw.circle(l_Head, 3.f * l_Scale, Color{ 1.f, 1.f, 1.f, 0.95f });
	}
}
} // namespace wb
