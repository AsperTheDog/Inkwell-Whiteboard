// Basic math vocabulary shared by every layer.
//
// World-space positions use double precision (DVec2) so the canvas stays precise far away from the origin;
// object-local geometry uses float (Vec2). Modules that do arithmetic on these types must also include
// <glm/glm.hpp> in their own global module fragment: glm's operators live in the GMF of this module and are
// not re-exported.
module;
#include <cstdint>
#include <algorithm>
#include <limits>
#include <glm/glm.hpp>

export module wb.math;

export namespace wb
{
using Vec2 = glm::vec2;
using DVec2 = glm::dvec2;
using Vec4 = glm::vec4;
using Mat3 = glm::mat3;
using DMat3 = glm::dmat3;

// Straight (non-premultiplied) sRGB color, 0..1 per channel
struct Color
{
	float r = 0.f;
	float g = 0.f;
	float b = 0.f;
	float a = 1.f;

	[[nodiscard]] static constexpr Color fromRgba8(const uint32_t p_Rgba)
	{
		return Color{
			.r = static_cast<float>((p_Rgba >> 24) & 0xFF) / 255.f,
			.g = static_cast<float>((p_Rgba >> 16) & 0xFF) / 255.f,
			.b = static_cast<float>((p_Rgba >> 8) & 0xFF) / 255.f,
			.a = static_cast<float>(p_Rgba & 0xFF) / 255.f,
		};
	}

	[[nodiscard]] constexpr uint32_t toRgba8() const
	{
		const auto l_Channel = [](const float p_Value) -> uint32_t
		{
			return static_cast<uint32_t>(std::clamp(p_Value, 0.f, 1.f) * 255.f + 0.5f);
		};
		return (l_Channel(r) << 24) | (l_Channel(g) << 16) | (l_Channel(b) << 8) | l_Channel(a);
	}

	[[nodiscard]] constexpr Vec4 toVec4() const { return Vec4{ r, g, b, a }; }

	constexpr bool operator==(const Color&) const = default;
};

// Axis-aligned bounding box in world space. A default-constructed Rect is empty (min > max).
struct Rect
{
	DVec2 min{ std::numeric_limits<double>::max() };
	DVec2 max{ std::numeric_limits<double>::lowest() };

	[[nodiscard]] static Rect fromPoints(const DVec2 p_A, const DVec2 p_B)
	{
		return Rect{ .min = glm::min(p_A, p_B), .max = glm::max(p_A, p_B) };
	}

	[[nodiscard]] static Rect fromCenter(const DVec2 p_Center, const DVec2 p_HalfExtent)
	{
		return Rect{ .min = p_Center - p_HalfExtent, .max = p_Center + p_HalfExtent };
	}

	[[nodiscard]] bool isEmpty() const { return min.x > max.x || min.y > max.y; }
	[[nodiscard]] DVec2 size() const { return isEmpty() ? DVec2{ 0.0 } : max - min; }
	[[nodiscard]] DVec2 center() const { return (min + max) * 0.5; }

	void expand(const DVec2 p_Point)
	{
		min = glm::min(min, p_Point);
		max = glm::max(max, p_Point);
	}

	void expand(const Rect& p_Other)
	{
		if (p_Other.isEmpty())
			return;
		min = glm::min(min, p_Other.min);
		max = glm::max(max, p_Other.max);
	}

	[[nodiscard]] Rect inflated(const double p_Amount) const
	{
		if (isEmpty())
			return *this;
		return Rect{ .min = min - DVec2{ p_Amount }, .max = max + DVec2{ p_Amount } };
	}

	[[nodiscard]] bool contains(const DVec2 p_Point) const
	{
		return p_Point.x >= min.x && p_Point.x <= max.x && p_Point.y >= min.y && p_Point.y <= max.y;
	}

	[[nodiscard]] bool intersects(const Rect& p_Other) const
	{
		return !isEmpty() && !p_Other.isEmpty() && min.x <= p_Other.max.x && max.x >= p_Other.min.x && min.y <= p_Other.max.y && max.y >= p_Other.min.y;
	}
};
} // namespace wb
