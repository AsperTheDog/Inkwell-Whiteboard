// Basic math vocabulary shared by every layer.
//
// World-space positions use double precision (DVec2) so the canvas stays precise far away from the origin;
// object-local geometry uses float (Vec2). Modules that do arithmetic on these types must also include
// <glm/glm.hpp> in their own global module fragment: glm's operators live in the GMF of this module and are
// not re-exported.
module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <glm/glm.hpp>

export module wb.math;

export namespace wb
{
using Vec2 = glm::vec2;
using DVec2 = glm::dvec2;
using Vec4 = glm::vec4;
using Mat2 = glm::mat2;
using DMat2 = glm::dmat2;

inline constexpr double PI = 3.14159265358979323846;

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

	[[nodiscard]] bool contains(const Rect& p_Other) const
	{
		return !p_Other.isEmpty() && contains(p_Other.min) && contains(p_Other.max);
	}

	[[nodiscard]] bool intersects(const Rect& p_Other) const
	{
		return !isEmpty() && !p_Other.isEmpty() && min.x <= p_Other.max.x && max.x >= p_Other.min.x && min.y <= p_Other.max.y && max.y >= p_Other.min.y;
	}
};

// General 2D affine transform in double precision: p' = linear * p + translation.
// A full 2x2 matrix (instead of translation/rotation/scale) keeps transforms closed under composition, e.g.
// scaling a rotated object along the selection's axes.
struct Affine2
{
	DMat2 linear{ 1.0 };
	DVec2 translation{ 0.0 };

	[[nodiscard]] static Affine2 translate(const DVec2 p_Offset) { return Affine2{ .linear = DMat2{ 1.0 }, .translation = p_Offset }; }
	[[nodiscard]] static Affine2 scale(const DVec2 p_Scale) { return Affine2{ .linear = DMat2{ p_Scale.x, 0.0, 0.0, p_Scale.y } }; }

	[[nodiscard]] static Affine2 rotate(const double p_Radians)
	{
		const double l_C = std::cos(p_Radians);
		const double l_S = std::sin(p_Radians);
		// glm matrices are column-major: columns are the images of the x and y axes
		return Affine2{ .linear = DMat2{ l_C, l_S, -l_S, l_C } };
	}

	// Transform that applies p_Inner around the pivot (translate(-pivot), inner, translate(pivot))
	[[nodiscard]] static Affine2 around(const DVec2 p_Pivot, const Affine2& p_Inner)
	{
		return translate(p_Pivot) * p_Inner * translate(-p_Pivot);
	}

	[[nodiscard]] DVec2 apply(const DVec2 p_Point) const { return linear * p_Point + translation; }
	[[nodiscard]] DVec2 applyVector(const DVec2 p_Vector) const { return linear * p_Vector; }

	[[nodiscard]] double determinant() const { return glm::determinant(linear); }
	// Average scale factor, used to scale widths/radii (exact for similarity transforms)
	[[nodiscard]] double uniformScale() const { return std::sqrt(std::abs(determinant())); }
	[[nodiscard]] bool isInvertible() const { return std::abs(determinant()) > 1e-300; }

	[[nodiscard]] Affine2 inverse() const
	{
		const DMat2 l_Inv = glm::inverse(linear);
		return Affine2{ .linear = l_Inv, .translation = -(l_Inv * translation) };
	}

	// (this * other).apply(p) == this->apply(other.apply(p))
	[[nodiscard]] Affine2 operator*(const Affine2& p_Other) const
	{
		return Affine2{ .linear = linear * p_Other.linear, .translation = linear * p_Other.translation + translation };
	}

	// Bounds of a transformed rectangle (all four corners)
	[[nodiscard]] Rect applyBounds(const Rect& p_Local) const
	{
		if (p_Local.isEmpty())
			return p_Local;
		Rect l_Result{};
		l_Result.expand(apply(p_Local.min));
		l_Result.expand(apply(p_Local.max));
		l_Result.expand(apply(DVec2{ p_Local.min.x, p_Local.max.y }));
		l_Result.expand(apply(DVec2{ p_Local.max.x, p_Local.min.y }));
		return l_Result;
	}

	bool operator==(const Affine2&) const = default;
};

// Distance from p to segment [a, b], and the clamped parameter of the closest point
struct SegmentProjection
{
	double distance = 0.0;
	double t = 0.0;
};

[[nodiscard]] inline SegmentProjection projectOnSegment(const DVec2 p_Point, const DVec2 p_A, const DVec2 p_B)
{
	const DVec2 l_Ab = p_B - p_A;
	const double l_LengthSq = glm::dot(l_Ab, l_Ab);
	const double l_T = l_LengthSq > 0.0 ? std::clamp(glm::dot(p_Point - p_A, l_Ab) / l_LengthSq, 0.0, 1.0) : 0.0;
	return SegmentProjection{ .distance = glm::length(p_Point - (p_A + l_Ab * l_T)), .t = l_T };
}

// Squared distance between two segments [p1, q1] and [p2, q2] (Ericson, Real-Time Collision Detection 5.1.9)
[[nodiscard]] inline double segmentSegmentDistanceSq(const DVec2 p_P1, const DVec2 p_Q1, const DVec2 p_P2, const DVec2 p_Q2)
{
	const DVec2 l_D1 = p_Q1 - p_P1;
	const DVec2 l_D2 = p_Q2 - p_P2;
	const DVec2 l_R = p_P1 - p_P2;
	const double l_A = glm::dot(l_D1, l_D1);
	const double l_E = glm::dot(l_D2, l_D2);
	const double l_F = glm::dot(l_D2, l_R);
	constexpr double l_Eps = 1e-24;

	double l_S = 0.0;
	double l_T = 0.0;
	if (l_A <= l_Eps && l_E <= l_Eps)
	{
		return glm::dot(l_R, l_R);
	}
	if (l_A <= l_Eps)
	{
		l_T = std::clamp(l_F / l_E, 0.0, 1.0);
	}
	else
	{
		const double l_C = glm::dot(l_D1, l_R);
		if (l_E <= l_Eps)
		{
			l_S = std::clamp(-l_C / l_A, 0.0, 1.0);
		}
		else
		{
			const double l_B = glm::dot(l_D1, l_D2);
			const double l_Denom = l_A * l_E - l_B * l_B;
			l_S = l_Denom > l_Eps ? std::clamp((l_B * l_F - l_C * l_E) / l_Denom, 0.0, 1.0) : 0.0;
			l_T = (l_B * l_S + l_F) / l_E;
			if (l_T < 0.0)
			{
				l_T = 0.0;
				l_S = std::clamp(-l_C / l_A, 0.0, 1.0);
			}
			else if (l_T > 1.0)
			{
				l_T = 1.0;
				l_S = std::clamp((l_B - l_C) / l_A, 0.0, 1.0);
			}
		}
	}
	const DVec2 l_Diff = (p_P1 + l_D1 * l_S) - (p_P2 + l_D2 * l_T);
	return glm::dot(l_Diff, l_Diff);
}
} // namespace wb
