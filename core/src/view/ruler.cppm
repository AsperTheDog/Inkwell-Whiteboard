// The ruler: a straight edge lying on the board. It is not part of the document (it never gets saved); pens that
// start near one of its long edges follow that edge, which gives perfectly straight lines.
module;
#include <algorithm>
#include <cmath>
#include <optional>
#include <glm/glm.hpp>

export module wb.view.ruler;

import wb.math;

export namespace wb
{
struct RulerLine
{
	DVec2 origin{ 0.0 };
	DVec2 direction{ 1.0, 0.0 }; // unit length
};

struct Ruler
{
	static constexpr double THICKNESS = 84.0;          // world units
	static constexpr double MIN_LENGTH = 240.0;
	static constexpr double MAX_LENGTH = 6000.0;
	static constexpr double CENTIMETRE = 96.0 / 2.54;  // world units are 1/96 inch at 100% zoom

	bool visible = false;
	DVec2 center{ 0.0 };
	double angle = 0.0;   // radians, direction of the long edges
	double length = 720.0;

	[[nodiscard]] DVec2 axis() const { return DVec2{ std::cos(angle), std::sin(angle) }; }
	[[nodiscard]] DVec2 normal() const { return DVec2{ -std::sin(angle), std::cos(angle) }; }

	// World -> ruler space: x along the long edges, y across, measured from the centre
	[[nodiscard]] DVec2 toLocal(const DVec2 p_World) const
	{
		const DVec2 l_Offset = p_World - center;
		return DVec2{ glm::dot(l_Offset, axis()), glm::dot(l_Offset, normal()) };
	}

	[[nodiscard]] DVec2 toWorld(const DVec2 p_Local) const { return center + axis() * p_Local.x + normal() * p_Local.y; }

	[[nodiscard]] bool inBody(const DVec2 p_World) const
	{
		const DVec2 l_Local = toLocal(p_World);
		return std::abs(l_Local.x) <= length * 0.5 && std::abs(l_Local.y) <= THICKNESS * 0.5;
	}

	// The long edge on one side (0: towards -normal, 1: towards +normal)
	[[nodiscard]] RulerLine edge(const int p_Side) const
	{
		return RulerLine{ .origin = toWorld(DVec2{ 0.0, p_Side == 0 ? -THICKNESS * 0.5 : THICKNESS * 0.5 }), .direction = axis() };
	}

	// The long edge p_World is within p_Tolerance of (and not beyond its ends by more than that), if any
	[[nodiscard]] std::optional<RulerLine> snapEdge(const DVec2 p_World, const double p_Tolerance) const
	{
		const DVec2 l_Local = toLocal(p_World);
		if (std::abs(l_Local.x) > length * 0.5 + p_Tolerance)
			return std::nullopt;
		const double l_FromTop = std::abs(l_Local.y + THICKNESS * 0.5);
		const double l_FromBottom = std::abs(l_Local.y - THICKNESS * 0.5);
		if (std::min(l_FromTop, l_FromBottom) > p_Tolerance)
			return std::nullopt;
		return edge(l_FromTop <= l_FromBottom ? 0 : 1);
	}

	// Centre of the knob that turns the ruler (near its right end, inside the body)
	[[nodiscard]] DVec2 knobCenter() const { return toWorld(DVec2{ length * 0.5 - THICKNESS * 0.5, 0.0 }); }
	[[nodiscard]] static double knobRadius() { return THICKNESS * 0.26; }
};

// The point of p_Line closest to p_Point
[[nodiscard]] inline DVec2 projectOntoLine(const RulerLine& p_Line, const DVec2 p_Point)
{
	return p_Line.origin + p_Line.direction * glm::dot(p_Point - p_Line.origin, p_Line.direction);
}

// Rounds an angle to the nearest multiple of p_Step when it is within p_Reach of one (both in radians)
[[nodiscard]] inline double snapRulerAngle(const double p_Angle, const double p_Step, const double p_Reach)
{
	const double l_Nearest = std::round(p_Angle / p_Step) * p_Step;
	return std::abs(p_Angle - l_Nearest) <= p_Reach ? l_Nearest : p_Angle;
}
} // namespace wb
