// Maps the infinite world plane to the window.
//
// Screen space is window pixels with y down. One world unit is one logical point at 100% zoom, so the
// on-screen size of content is the same on every display density: pixels per world unit = zoom * pixelScale.
module;
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

export module wb.view.camera;

import wb.math;

export namespace wb
{
class Camera
{
public:
	static constexpr double MIN_ZOOM = 0.02; // 2%
	static constexpr double MAX_ZOOM = 64.0; // 6400%

	void setViewport(const Vec2 p_SizePixels, const float p_PixelScale)
	{
		m_Viewport = DVec2{ p_SizePixels };
		m_PixelScale = std::max(0.25, static_cast<double>(p_PixelScale));
	}

	[[nodiscard]] DVec2 viewport() const { return m_Viewport; }
	[[nodiscard]] DVec2 center() const { return m_Center; }
	[[nodiscard]] double zoom() const { return m_Zoom; }
	[[nodiscard]] double pixelScale() const { return m_PixelScale; }
	[[nodiscard]] double pixelsPerUnit() const { return m_Zoom * m_PixelScale; }

	void setCenter(const DVec2 p_Center) { m_Center = p_Center; }
	void setZoom(const double p_Zoom) { m_Zoom = std::clamp(p_Zoom, MIN_ZOOM, MAX_ZOOM); }

	[[nodiscard]] DVec2 screenToWorld(const DVec2 p_Screen) const
	{
		return m_Center + (p_Screen - m_Viewport * 0.5) / pixelsPerUnit();
	}

	[[nodiscard]] DVec2 worldToScreen(const DVec2 p_World) const
	{
		return (p_World - m_Center) * pixelsPerUnit() + m_Viewport * 0.5;
	}

	// World-to-screen as an affine transform (compose with an object's transform for local-to-screen)
	[[nodiscard]] Affine2 worldToScreenTransform() const
	{
		const double l_Ppu = pixelsPerUnit();
		return Affine2{ .linear = DMat2{ l_Ppu, 0.0, 0.0, l_Ppu }, .translation = m_Viewport * 0.5 - m_Center * l_Ppu };
	}

	[[nodiscard]] Rect visibleWorldRect() const
	{
		return Rect{ .min = screenToWorld(DVec2{ 0.0 }), .max = screenToWorld(m_Viewport) };
	}

	// Moves the view by a screen-space delta (content follows the pointer)
	void panPixels(const DVec2 p_ScreenDelta)
	{
		m_Center -= p_ScreenDelta / pixelsPerUnit();
	}

	// Changes zoom keeping the world point under p_Screen fixed on screen
	void zoomAt(const DVec2 p_Screen, const double p_NewZoom)
	{
		const DVec2 l_Anchor = screenToWorld(p_Screen);
		setZoom(p_NewZoom);
		m_Center = l_Anchor - (p_Screen - m_Viewport * 0.5) / pixelsPerUnit();
	}

	// Centers and zooms so p_World fills the view with p_MarginPixels around it (zoom capped at p_MaxZoom)
	void fit(const Rect& p_World, const double p_MarginPixels, const double p_MaxZoom = 1.0)
	{
		if (p_World.isEmpty())
			return;
		const DVec2 l_Size = glm::max(p_World.size(), DVec2{ 1e-9 });
		const DVec2 l_Available = glm::max(m_Viewport - DVec2{ 2.0 * p_MarginPixels }, DVec2{ 1.0 });
		const double l_Zoom = std::min(l_Available.x / l_Size.x, l_Available.y / l_Size.y) / m_PixelScale;
		setZoom(std::min(l_Zoom, p_MaxZoom));
		m_Center = p_World.center();
	}

private:
	DVec2 m_Center{ 0.0 };
	double m_Zoom = 1.0;
	double m_PixelScale = 1.0;
	DVec2 m_Viewport{ 1.0 };
};
} // namespace wb
