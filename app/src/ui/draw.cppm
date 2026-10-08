// Collects the primitives of one UI frame (rounded boxes, shadows, lines, glyphs...). The renderer uploads them
// as instances; the layout is the order they were added in. Everything is in window pixels, y down.
module;
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

export module wb.ui.draw;

import wb.math;
import wb.ui.font;

export namespace wb::ui
{
// Screen-space rectangle in pixels
struct Rect2
{
	Vec2 min{ 0.f };
	Vec2 max{ 0.f };

	[[nodiscard]] static Rect2 fromPosSize(const Vec2 p_Pos, const Vec2 p_Size) { return Rect2{ p_Pos, p_Pos + p_Size }; }
	[[nodiscard]] static Rect2 fromCenter(const Vec2 p_Center, const Vec2 p_Size) { return Rect2{ p_Center - p_Size * 0.5f, p_Center + p_Size * 0.5f }; }

	[[nodiscard]] float width() const { return max.x - min.x; }
	[[nodiscard]] float height() const { return max.y - min.y; }
	[[nodiscard]] Vec2 size() const { return max - min; }
	[[nodiscard]] Vec2 center() const { return (min + max) * 0.5f; }
	[[nodiscard]] bool contains(const Vec2 p_Point) const { return p_Point.x >= min.x && p_Point.x < max.x && p_Point.y >= min.y && p_Point.y < max.y; }
	[[nodiscard]] Rect2 inflated(const float p_Amount) const { return Rect2{ min - Vec2{ p_Amount }, max + Vec2{ p_Amount } }; }
	[[nodiscard]] Rect2 shrunk(const float p_Amount) const { return inflated(-p_Amount); }
	[[nodiscard]] Rect2 translated(const Vec2 p_Offset) const { return Rect2{ min + p_Offset, max + p_Offset }; }
};

enum class TextAlign : uint8_t
{
	Left,   // x is the left edge
	Center, // x is the centre
	Right,  // x is the right edge
};

// GPU layout of one primitive (matches ui.slang)
struct Prim
{
	std::array<float, 4> bounds{};
	std::array<float, 4> shape{};
	std::array<float, 4> uv{};
	std::array<float, 4> color{};
	std::array<float, 4> color2{};
	std::array<float, 4> misc{};
	std::array<float, 4> clip{};
};
static_assert(sizeof(Prim) == 112);

class DrawList
{
public:
	// Starts a frame. p_Viewport is the window size in pixels.
	void begin(FontAtlas& p_Font, Vec2 p_Viewport);

	[[nodiscard]] FontAtlas& font() const { return *m_Font; }
	[[nodiscard]] Vec2 viewport() const { return m_Viewport; }

	// Multiplies the alpha of everything drawn afterwards (fade animations)
	void setOpacity(const float p_Opacity) { m_Opacity = p_Opacity; }
	[[nodiscard]] float opacity() const { return m_Opacity; }
	void pushClip(Rect2 p_Rect);
	void popClip();

	void rect(Rect2 p_Rect, float p_Radius, Color p_Fill);
	void rectBorder(Rect2 p_Rect, float p_Radius, Color p_Fill, Color p_Border, float p_BorderWidth);
	void outline(Rect2 p_Rect, float p_Radius, Color p_Border, float p_BorderWidth);
	void circle(Vec2 p_Center, float p_Radius, Color p_Fill);
	void ring(Vec2 p_Center, float p_Radius, Color p_Border, float p_BorderWidth);
	// Blurred drop shadow of a rounded rectangle
	void shadow(Rect2 p_Rect, float p_Radius, float p_Blur, Vec2 p_Offset, Color p_Color);
	void line(Vec2 p_A, Vec2 p_B, float p_Width, Color p_Color);
	void polyline(std::span<const Vec2> p_Points, float p_Width, Color p_Color, bool p_Closed);

	// Text whose vertical centre (of the capital letters) is at p_Position.y. Returns the width drawn.
	float text(Vec2 p_Position, std::string_view p_Text, int p_PixelSize, Color p_Color, TextAlign p_Align = TextAlign::Left);
	void icon(Icon p_Icon, Vec2 p_Center, int p_PixelSize, Color p_Color);

	// Colour picker pieces
	void saturationValueSquare(Rect2 p_Rect, float p_Radius, float p_Hue);
	void hueBar(Rect2 p_Rect, float p_Radius);

	[[nodiscard]] std::span<const Prim> prims() const { return m_Prims; }

private:
	[[nodiscard]] std::array<float, 4> premultiplied(Color p_Color) const;
	[[nodiscard]] Prim& add(Rect2 p_Bounds, float p_Mode);

	FontAtlas* m_Font = nullptr;
	Vec2 m_Viewport{ 0.f };
	float m_Opacity = 1.f;
	std::vector<Rect2> m_ClipStack;
	std::vector<Prim> m_Prims;
};
} // namespace wb::ui
