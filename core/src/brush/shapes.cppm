// Outlines of the shapes the shape tool draws. A shape is a polyline: the renderer draws it like any other stroke, so
// shapes can be erased, recoloured, selected and saved without any special treatment.
module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

export module wb.brush.shapes;

import wb.math;

export namespace wb
{
enum class ShapeKind : uint8_t
{
	Line,
	Arrow,
	Rectangle,
	Ellipse,
	Triangle,
	Diamond,
};

inline constexpr int SHAPE_KIND_COUNT = 6;

struct ShapeOptions
{
	bool constrain = false;  // Shift: equal sides (square, circle), lines in steps of 45 degrees
	bool fromCenter = false; // Alt: the first point is the centre instead of a corner
	double strokeWidth = 4.0; // world units, sizes the arrow head
};

// The outline of the shape dragged from p_From to p_To, in world coordinates. Closed shapes end where they began.
// Empty when the drag is too short to make a shape.
[[nodiscard]] inline std::vector<DVec2> shapeOutline(const ShapeKind p_Kind, const DVec2 p_From, const DVec2 p_To, const ShapeOptions& p_Options)
{
	DVec2 l_Delta = p_To - p_From;
	if (p_Options.constrain)
	{
		if (p_Kind == ShapeKind::Line || p_Kind == ShapeKind::Arrow)
		{
			const double l_Length = glm::length(l_Delta);
			if (l_Length > 0.0)
			{
				const double l_Step = 3.14159265358979323846 / 4.0;
				const double l_Angle = std::round(std::atan2(l_Delta.y, l_Delta.x) / l_Step) * l_Step;
				l_Delta = DVec2{ std::cos(l_Angle), std::sin(l_Angle) } * l_Length;
			}
		}
		else
		{
			const double l_Side = std::max(std::abs(l_Delta.x), std::abs(l_Delta.y));
			l_Delta = DVec2{ l_Delta.x < 0.0 ? -l_Side : l_Side, l_Delta.y < 0.0 ? -l_Side : l_Side };
		}
	}

	DVec2 l_A = p_From;
	DVec2 l_B = p_From + l_Delta;
	if (p_Options.fromCenter)
	{
		l_A = p_From - l_Delta;
		l_B = p_From + l_Delta;
	}
	const DVec2 l_Min = glm::min(l_A, l_B);
	const DVec2 l_Max = glm::max(l_A, l_B);
	const DVec2 l_Size = l_Max - l_Min;

	constexpr double MIN_EXTENT = 1.0;
	if (p_Kind == ShapeKind::Line || p_Kind == ShapeKind::Arrow)
	{
		const double l_Length = glm::length(l_B - l_A);
		if (l_Length < MIN_EXTENT)
			return {};
		if (p_Kind == ShapeKind::Line)
			return { l_A, l_B };
		const DVec2 l_Direction = (l_B - l_A) / l_Length;
		const DVec2 l_Side{ -l_Direction.y, l_Direction.x };
		const double l_Head = std::min(std::clamp(l_Length * 0.22, p_Options.strokeWidth * 3.0, p_Options.strokeWidth * 9.0), l_Length * 0.6);
		const DVec2 l_Base = l_B - l_Direction * l_Head;
		return { l_A, l_B, l_Base + l_Side * (l_Head * 0.55), l_B, l_Base - l_Side * (l_Head * 0.55) };
	}

	if (l_Size.x < MIN_EXTENT || l_Size.y < MIN_EXTENT)
		return {};
	switch (p_Kind)
	{
	case ShapeKind::Rectangle:
		return { l_Min, DVec2{ l_Max.x, l_Min.y }, l_Max, DVec2{ l_Min.x, l_Max.y }, l_Min };
	case ShapeKind::Triangle:
		return { DVec2{ (l_Min.x + l_Max.x) * 0.5, l_Min.y }, DVec2{ l_Max.x, l_Max.y }, DVec2{ l_Min.x, l_Max.y }, DVec2{ (l_Min.x + l_Max.x) * 0.5, l_Min.y } };
	case ShapeKind::Diamond:
	{
		const DVec2 l_Mid = (l_Min + l_Max) * 0.5;
		return { DVec2{ l_Mid.x, l_Min.y }, DVec2{ l_Max.x, l_Mid.y }, DVec2{ l_Mid.x, l_Max.y }, DVec2{ l_Min.x, l_Mid.y }, DVec2{ l_Mid.x, l_Min.y } };
	}
	case ShapeKind::Ellipse:
	{
		const DVec2 l_Mid = (l_Min + l_Max) * 0.5;
		const DVec2 l_Radius = l_Size * 0.5;
		// About one point every 3 units of the outline, enough for it to look round at any size
		const double l_Perimeter = 2.0 * 3.14159265358979323846 * std::max(l_Radius.x, l_Radius.y);
		// A multiple of four, so the points include the extremes in all four directions
		const int l_Count = std::clamp((static_cast<int>(std::ceil(l_Perimeter / 3.0)) + 3) / 4 * 4, 36, 720);
		std::vector<DVec2> l_Points;
		l_Points.reserve(static_cast<size_t>(l_Count) + 1);
		for (int i = 0; i <= l_Count; ++i)
		{
			const double l_T = 2.0 * 3.14159265358979323846 * static_cast<double>(i % l_Count) / static_cast<double>(l_Count);
			l_Points.push_back(l_Mid + DVec2{ std::cos(l_T) * l_Radius.x, std::sin(l_T) * l_Radius.y });
		}
		return l_Points;
	}
	default:
		return {};
	}
}
} // namespace wb
