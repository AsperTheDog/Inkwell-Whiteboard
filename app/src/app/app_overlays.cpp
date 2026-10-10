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
import wb.net.collab;
import wb.net.protocol;
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

// ------------------------------------------------------------------------------------------------ files on their way

// A turning arc and a percentage over every picture or video that is still being received
void App::buildAssetSpinners()
{
	if (!m_Collab || !m_Collab->online() || (m_Collab->assetProgress().files == 0 && !m_Collab->hasAssetNotices()))
		return;
	ui::DrawList& l_Draw = m_Ui.draw();
	const Camera& l_Camera = m_Editor.camera();
	const ui::Theme& l_Theme = m_Ui.theme();
	const float l_Scale = m_Ui.scale();
	const Vec2 l_Viewport = m_Ui.viewport();
	const double l_Time = static_cast<double>(SDL_GetTicksNS()) * 1e-9;
	const std::span<const std::unique_ptr<Object>> l_Objects = m_Editor.document().objects();
	const std::span<const Rect> l_Bounds = m_Editor.document().boundsList();
	bool l_Any = false;
	for (size_t i = 0; i < l_Objects.size() && i < l_Bounds.size(); ++i)
	{
		const Object& l_Object = *l_Objects[i];
		const ImageData* l_Image = l_Object.image();
		const VideoData* l_Video = l_Object.video();
		const AssetId l_Asset = l_Image != nullptr ? l_Image->asset : (l_Video != nullptr ? l_Video->asset : INVALID_ASSET_ID);
		if (l_Asset == INVALID_ASSET_ID)
			continue;
		const double l_Fraction = m_Collab->assetFraction(l_Asset);
		const bool l_Stalled = l_Fraction < 0.0 && m_Collab->assetStalled(l_Asset);
		const bool l_Resend = l_Fraction < 0.0 && m_Collab->assetReuploadable(l_Asset);
		if (l_Fraction < 0.0 && !l_Stalled && !l_Resend)
			continue;
		const Vec2 l_Min{ l_Camera.worldToScreen(l_Bounds[i].min) };
		const Vec2 l_Max{ l_Camera.worldToScreen(l_Bounds[i].max) };
		const Vec2 l_Lo = glm::min(l_Min, l_Max);
		const Vec2 l_Hi = glm::max(l_Min, l_Max);
		if (l_Hi.x < 0.f || l_Hi.y < 0.f || l_Lo.x > l_Viewport.x || l_Lo.y > l_Viewport.y)
			continue;
		const float l_Short = std::min(l_Hi.x - l_Lo.x, l_Hi.y - l_Lo.y);
		if (l_Short < 22.f * l_Scale)
			continue; // too small to hold it
		const Vec2 l_Center = (l_Lo + l_Hi) * 0.5f;
		const float l_Radius = std::clamp(l_Short * 0.2f, 9.f * l_Scale, 20.f * l_Scale);

		if (l_Resend)
		{
			// The others cannot get this file: a button to send it again
			const bool l_Wide = l_Short > 150.f * l_Scale;
			const ui::Rect2 l_Button = ui::Rect2::fromCenter(l_Center, Vec2{ (l_Wide ? 168.f : 84.f) * l_Scale, 32.f * l_Scale });
			m_Ui.beginPanel(l_Button); // the board underneath must not take the click
			if (m_Ui.button("asset.resend." + std::to_string(l_Object.id), l_Button, l_Wide ? "Re-send for the others" : "Re-send", ui::ButtonStyle::Primary))
				m_Collab->reupload(l_Asset);
			continue;
		}
		if (l_Stalled)
		{
			// Nobody is sending it: a paused badge, and who can fix it
			l_Draw.circle(l_Center, l_Radius + 5.f * l_Scale, ui::withAlpha(l_Theme.shadow, 0.55f));
			l_Draw.ring(l_Center, l_Radius, Color{ 1.f, 1.f, 1.f, 0.35f }, 2.5f * l_Scale);
			for (const float l_Side : { -1.f, 1.f })
				l_Draw.line(l_Center + Vec2{ l_Side * 4.f * l_Scale, -6.f * l_Scale }, l_Center + Vec2{ l_Side * 4.f * l_Scale, 6.f * l_Scale }, 3.f * l_Scale, Color{ 1.f, 1.f, 1.f, 0.95f });
			const std::string l_Owner = m_Collab->assetOwnerName(l_Asset);
			if (l_Short > 90.f * l_Scale)
				l_Draw.text(l_Center + Vec2{ 0.f, l_Radius + 16.f * l_Scale }, l_Owner.empty() ? "Not available" : "Waiting for " + l_Owner + " to send it again", m_Ui.fontPx(12.f), Color{ 1.f, 1.f, 1.f, 0.95f }, ui::TextAlign::Center);
			continue;
		}
		l_Any = true;

		l_Draw.circle(l_Center, l_Radius + 5.f * l_Scale, ui::withAlpha(l_Theme.shadow, 0.55f));
		l_Draw.ring(l_Center, l_Radius, Color{ 1.f, 1.f, 1.f, 0.22f }, 2.5f * l_Scale);
		constexpr int SEGMENTS = 14;
		std::array<Vec2, SEGMENTS + 1> l_Arc{};
		const double l_Start = l_Time * 5.0;
		for (int s = 0; s <= SEGMENTS; ++s)
		{
			const double l_Angle = l_Start + (static_cast<double>(s) / SEGMENTS) * 1.9;
			l_Arc[static_cast<size_t>(s)] = l_Center + Vec2{ static_cast<float>(std::cos(l_Angle)), static_cast<float>(std::sin(l_Angle)) } * l_Radius;
		}
		l_Draw.polyline(l_Arc, 2.5f * l_Scale, Color{ 1.f, 1.f, 1.f, 0.95f }, false);
		if (l_Fraction > 0.0 && l_Short > 60.f * l_Scale)
			l_Draw.text(l_Center + Vec2{ 0.f, l_Radius + 16.f * l_Scale }, std::to_string(static_cast<int>(l_Fraction * 100.0)) + "%", m_Ui.fontPx(12.f), Color{ 1.f, 1.f, 1.f, 0.95f }, ui::TextAlign::Center);
	}
	if (l_Any)
		requestRedraw(2);
}

// ------------------------------------------------------------------------------------------------ remote pointers

void App::buildRemoteCursors()
{
	if (!m_Collab || !m_Collab->online())
	{
		m_ShownCursors.clear();
		return;
	}
	const std::vector<net::CollabSession::CursorView> l_Views = m_Collab->cursors();
	const uint64_t l_Now = SDL_GetTicksNS();
	const float l_Dt = m_CursorFrameNs == 0 ? 0.016f : std::clamp(static_cast<float>(l_Now - m_CursorFrameNs) * 1e-9f, 0.f, 0.1f);
	m_CursorFrameNs = l_Now;
	std::erase_if(m_ShownCursors, [&](const auto& p_Entry)
	{
		return std::ranges::none_of(l_Views, [&](const net::CollabSession::CursorView& p_View) { return p_View.slot == p_Entry.first; });
	});

	ui::DrawList& l_Draw = m_Ui.draw();
	const Camera& l_Camera = m_Editor.camera();
	const ui::Theme& l_Theme = m_Ui.theme();
	const float l_Scale = m_Ui.scale();
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Follow = 1.f - std::exp(-l_Dt * 22.f);
	const float l_Fade = 1.f - std::exp(-l_Dt * 12.f);
	constexpr double IDLE_SECONDS = 8.0; // a pointer that has not moved for this long is dimmed
	bool l_Animating = false;

	for (const net::CollabSession::CursorView& l_View : l_Views)
	{
		const net::UserInfo* l_User = m_Collab->user(l_View.slot);
		if (l_User == nullptr)
			continue;
		ShownCursor& l_Shown = m_ShownCursors[l_View.slot];
		const float l_TargetAlpha = !l_View.visible ? 0.f : (l_View.ageSeconds < IDLE_SECONDS ? 1.f : 0.35f);
		if (!l_Shown.placed)
		{
			l_Shown.position = l_View.position;
			l_Shown.placed = true;
		}
		else
		{
			l_Shown.position += (l_View.position - l_Shown.position) * static_cast<double>(l_Follow);
		}
		l_Shown.alpha += (l_TargetAlpha - l_Shown.alpha) * l_Fade;

		const Vec2 l_Target{ l_Camera.worldToScreen(l_View.position) };
		Vec2 l_At{ l_Camera.worldToScreen(l_Shown.position) };
		l_Animating = l_Animating || glm::length(l_Target - l_At) > 0.3f || std::abs(l_TargetAlpha - l_Shown.alpha) > 0.01f || (l_View.visible && l_View.ageSeconds < IDLE_SECONDS + 1.0);
		if (l_Shown.alpha < 0.02f)
			continue;

		const Color l_Color = Color::fromRgba8(l_User->color);
		const Color l_Ink{ 1.f, 1.f, 1.f, 1.f };
		l_Draw.setOpacity(l_Shown.alpha);

		// Off screen: a dot at the edge in the direction of the pointer, so you can tell where they are
		const float l_Edge = 18.f * l_Scale;
		const bool l_Outside = l_At.x < 0.f || l_At.y < 0.f || l_At.x > l_Viewport.x || l_At.y > l_Viewport.y;
		if (l_Outside)
			l_At = Vec2{ std::clamp(l_At.x, l_Edge, l_Viewport.x - l_Edge), std::clamp(l_At.y, l_Edge, l_Viewport.y - l_Edge) };
		if (l_Outside)
		{
			l_Draw.circle(l_At, 8.f * l_Scale, ui::withAlpha(l_Theme.shadow, 0.35f));
			l_Draw.circle(l_At, 6.5f * l_Scale, l_Color);
			l_Draw.ring(l_At, 6.5f * l_Scale, l_Ink, 1.5f * l_Scale);
		}
		else
		{
			// The arrow: a white halo, then the user's colour on top (thick outline strokes fill the small shape)
			const std::array<Vec2, 7> l_Shape{ Vec2{ 0.f, 0.f }, Vec2{ 0.f, 15.f }, Vec2{ 4.f, 11.6f }, Vec2{ 7.f, 18.f }, Vec2{ 9.4f, 16.9f }, Vec2{ 6.5f, 10.6f }, Vec2{ 11.4f, 10.2f } };
			std::array<Vec2, 7> l_Points{};
			for (size_t i = 0; i < l_Shape.size(); ++i)
				l_Points[i] = l_At + (l_Shape[i] + Vec2{ 1.6f, 2.2f }) * l_Scale;
			l_Draw.polyline(l_Points, 6.6f * l_Scale, l_Ink, true);
			l_Draw.polyline(l_Points, 3.8f * l_Scale, l_Color, true);
		}

		// The name, in the user's colour
		const int l_FontPx = m_Ui.fontPx(12.f);
		const std::string l_Name = m_Ui.font().fit(l_User->name, l_FontPx, 160.f * l_Scale);
		const float l_Width = m_Ui.font().measure(l_Name, l_FontPx) + 14.f * l_Scale;
		const float l_Height = 20.f * l_Scale;
		const Vec2 l_LabelMin = l_Outside ? l_At + Vec2{ 12.f * l_Scale, -l_Height * 0.5f } : l_At + Vec2{ 14.f, 20.f } * l_Scale;
		Vec2 l_Clamped = l_LabelMin;
		l_Clamped.x = std::min(l_Clamped.x, l_Viewport.x - l_Width - 4.f * l_Scale);
		l_Clamped.y = std::min(l_Clamped.y, l_Viewport.y - l_Height - 4.f * l_Scale);
		const ui::Rect2 l_Label = ui::Rect2::fromPosSize(l_Clamped, Vec2{ l_Width, l_Height });
		l_Draw.rect(l_Label, l_Height * 0.5f, l_Color);
		l_Draw.text(Vec2{ l_Label.min.x + 7.f * l_Scale, l_Label.center().y }, l_Name, l_FontPx, l_Ink);
	}
	l_Draw.setOpacity(1.f);
	if (l_Animating)
		requestRedraw(2);
}
} // namespace wb
