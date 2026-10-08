module;
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.brush.eraser;

import wb.math;
import wb.doc.object;

namespace wb
{
namespace
{
constexpr int SEARCH_ITERATIONS = 40;
constexpr double END_EPSILON = 1e-9;

// The eraser expressed in the stroke's local space
struct LocalEraser
{
	DVec2 a{ 0.0 };
	DVec2 b{ 0.0 };
	double radius = 0.0;
};

struct Segment
{
	DVec2 p0{ 0.0 };
	DVec2 p1{ 0.0 };
	double r0 = 0.0;
	double r1 = 0.0;

	[[nodiscard]] DVec2 point(const double p_T) const { return p_T <= 0.0 ? p0 : (p_T >= 1.0 ? p1 : p0 + (p1 - p0) * p_T); }
	[[nodiscard]] double radius(const double p_T) const { return p_T <= 0.0 ? r0 : (p_T >= 1.0 ? r1 : r0 + (r1 - r0) * p_T); }
};

struct Interval
{
	double t0 = 0.0;
	double t1 = 1.0;
};

// <= 0 when the stroke's outline at parameter p_T touches or overlaps the eraser
double clearance(const Segment& p_Segment, const double p_T, const LocalEraser& p_Eraser)
{
	return projectOnSegment(p_Segment.point(p_T), p_Eraser.a, p_Eraser.b).distance - (p_Eraser.radius + p_Segment.radius(p_T));
}

// Finds the boundary between a cleared (> 0) and an erased (<= 0) parameter; p_Clear and p_Erased bracket it
double bisect(const Segment& p_Segment, const LocalEraser& p_Eraser, double p_Clear, double p_Erased)
{
	for (int i = 0; i < SEARCH_ITERATIONS; ++i)
	{
		const double l_Mid = (p_Clear + p_Erased) * 0.5;
		if (clearance(p_Segment, l_Mid, p_Eraser) <= 0.0)
			p_Erased = l_Mid;
		else
			p_Clear = l_Mid;
	}
	return p_Erased;
}

std::optional<Interval> erasedInterval(const Segment& p_Segment, const LocalEraser& p_Eraser)
{
	// Cheap rejection before the iterative search
	const double l_Reach = p_Eraser.radius + std::max(p_Segment.r0, p_Segment.r1);
	if (segmentSegmentDistanceSq(p_Segment.p0, p_Segment.p1, p_Eraser.a, p_Eraser.b) > l_Reach * l_Reach)
		return std::nullopt;

	const double l_F0 = clearance(p_Segment, 0.0, p_Eraser);
	const double l_F1 = clearance(p_Segment, 1.0, p_Eraser);

	// clearance(t) is convex: locate its minimum
	double l_Lo = 0.0;
	double l_Hi = 1.0;
	for (int i = 0; i < SEARCH_ITERATIONS; ++i)
	{
		const double l_M1 = l_Lo + (l_Hi - l_Lo) / 3.0;
		const double l_M2 = l_Hi - (l_Hi - l_Lo) / 3.0;
		if (clearance(p_Segment, l_M1, p_Eraser) < clearance(p_Segment, l_M2, p_Eraser))
			l_Hi = l_M2;
		else
			l_Lo = l_M1;
	}
	double l_MinT = (l_Lo + l_Hi) * 0.5;
	double l_MinF = clearance(p_Segment, l_MinT, p_Eraser);
	if (l_F0 < l_MinF)
	{
		l_MinT = 0.0;
		l_MinF = l_F0;
	}
	if (l_F1 < l_MinF)
	{
		l_MinT = 1.0;
		l_MinF = l_F1;
	}
	if (l_MinF > 0.0)
		return std::nullopt;

	return Interval{
		.t0 = l_F0 <= 0.0 ? 0.0 : bisect(p_Segment, p_Eraser, 0.0, l_MinT),
		.t1 = l_F1 <= 0.0 ? 1.0 : bisect(p_Segment, p_Eraser, 1.0, l_MinT),
	};
}

std::optional<LocalEraser> toLocal(const Object& p_Object, const EraserCapsule& p_Eraser)
{
	if (!p_Object.transform.isInvertible())
		return std::nullopt;
	const Affine2 l_Inverse = p_Object.transform.inverse();
	return LocalEraser{
		.a = l_Inverse.apply(p_Eraser.a),
		.b = l_Inverse.apply(p_Eraser.b),
		.radius = p_Eraser.radius * l_Inverse.uniformScale(),
	};
}

Segment segmentAt(const StrokeData& p_Stroke, const size_t p_Index)
{
	const StrokePoint& l_P0 = p_Stroke.points[p_Index];
	const StrokePoint& l_P1 = p_Stroke.points[p_Index + 1];
	return Segment{ .p0 = DVec2{ l_P0.position }, .p1 = DVec2{ l_P1.position }, .r0 = l_P0.radius, .r1 = l_P1.radius };
}

StrokePoint pointAt(const Segment& p_Segment, const double p_T)
{
	return StrokePoint{ .position = Vec2{ p_Segment.point(p_T) }, .radius = static_cast<float>(p_Segment.radius(p_T)) };
}
} // namespace

bool strokeTouches(const Object& p_Object, const EraserCapsule& p_Eraser)
{
	const StrokeData* l_Stroke = p_Object.stroke();
	if (l_Stroke == nullptr || l_Stroke->points.empty() || !p_Object.worldBounds().intersects(p_Eraser.bounds()))
		return false;
	const std::optional<LocalEraser> l_Local = toLocal(p_Object, p_Eraser);
	if (!l_Local)
		return false;

	if (l_Stroke->points.size() == 1)
	{
		const StrokePoint& l_Dot = l_Stroke->points.front();
		return projectOnSegment(DVec2{ l_Dot.position }, l_Local->a, l_Local->b).distance <= l_Local->radius + l_Dot.radius;
	}
	for (size_t i = 0; i + 1 < l_Stroke->points.size(); ++i)
	{
		if (erasedInterval(segmentAt(*l_Stroke, i), *l_Local))
			return true;
	}
	return false;
}

std::optional<std::vector<StrokeData>> cutStroke(const Object& p_Object, const EraserCapsule& p_Eraser)
{
	const StrokeData* l_Stroke = p_Object.stroke();
	if (l_Stroke == nullptr || l_Stroke->points.empty() || !p_Object.worldBounds().intersects(p_Eraser.bounds()))
		return std::nullopt;
	const std::optional<LocalEraser> l_Local = toLocal(p_Object, p_Eraser);
	if (!l_Local)
		return std::nullopt;

	if (l_Stroke->points.size() == 1)
	{
		if (!strokeTouches(p_Object, p_Eraser))
			return std::nullopt;
		return std::vector<StrokeData>{};
	}

	std::vector<StrokeData> l_Pieces;
	StrokeData l_Current{ .style = l_Stroke->style, .points = {} };
	bool l_Touched = false;

	const auto l_Flush = [&]
	{
		// A lone point is only the sliver left at a cut boundary
		if (l_Current.points.size() >= 2)
			l_Pieces.push_back(std::move(l_Current));
		l_Current = StrokeData{ .style = l_Stroke->style, .points = {} };
	};

	for (size_t i = 0; i + 1 < l_Stroke->points.size(); ++i)
	{
		const Segment l_Segment = segmentAt(*l_Stroke, i);
		const std::optional<Interval> l_Erased = erasedInterval(l_Segment, *l_Local);
		if (!l_Erased)
		{
			if (l_Current.points.empty())
				l_Current.points.push_back(l_Stroke->points[i]);
			l_Current.points.push_back(l_Stroke->points[i + 1]);
			continue;
		}

		l_Touched = true;
		if (l_Erased->t0 > END_EPSILON)
		{
			if (l_Current.points.empty())
				l_Current.points.push_back(l_Stroke->points[i]);
			l_Current.points.push_back(pointAt(l_Segment, l_Erased->t0));
		}
		l_Flush();
		if (l_Erased->t1 < 1.0 - END_EPSILON)
		{
			l_Current.points.push_back(pointAt(l_Segment, l_Erased->t1));
			l_Current.points.push_back(l_Stroke->points[i + 1]);
		}
	}
	l_Flush();

	if (!l_Touched)
		return std::nullopt;
	return l_Pieces;
}
} // namespace wb
