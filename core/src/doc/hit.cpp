module;
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <vector>
#include <glm/glm.hpp>

module wb.doc.hit;

import wb.math;
import wb.doc.object;
import wb.doc.document;

namespace wb
{
namespace
{
struct WorldSegment
{
	DVec2 a{ 0.0 };
	DVec2 b{ 0.0 };
	double ra = 0.0;
	double rb = 0.0;
};

// Calls p_Visit(WorldSegment) for every segment of the stroke (a lone point is a zero-length segment), stopping
// early when it returns true
template <typename Visitor>
bool anySegment(const Object& p_Object, const StrokeData& p_Stroke, Visitor&& p_Visit)
{
	const double l_Scale = p_Object.transform.uniformScale();
	const std::vector<StrokePoint>& l_Points = p_Stroke.points;
	if (l_Points.empty())
		return false;
	if (l_Points.size() == 1)
	{
		const DVec2 l_P = p_Object.transform.apply(DVec2{ l_Points[0].position });
		const double l_R = static_cast<double>(l_Points[0].radius) * l_Scale;
		return p_Visit(WorldSegment{ l_P, l_P, l_R, l_R });
	}

	DVec2 l_Prev = p_Object.transform.apply(DVec2{ l_Points[0].position });
	double l_PrevRadius = static_cast<double>(l_Points[0].radius) * l_Scale;
	for (size_t i = 1; i < l_Points.size(); ++i)
	{
		const DVec2 l_Next = p_Object.transform.apply(DVec2{ l_Points[i].position });
		const double l_NextRadius = static_cast<double>(l_Points[i].radius) * l_Scale;
		if (p_Visit(WorldSegment{ l_Prev, l_Next, l_PrevRadius, l_NextRadius }))
			return true;
		l_Prev = l_Next;
		l_PrevRadius = l_NextRadius;
	}
	return false;
}

// Pictures and text are solid rectangles
bool boxBounds(const Object& p_Object, Rect& p_Local)
{
	if (const ImageData* l_Image = p_Object.image())
		p_Local = l_Image->localBounds();
	else if (const TextData* l_Text = p_Object.text())
		p_Local = l_Text->localBounds();
	else
		return false;
	return true;
}

Rect polygonBounds(const std::span<const DVec2> p_Polygon)
{
	Rect l_Bounds{};
	for (const DVec2 l_Point : p_Polygon)
		l_Bounds.expand(l_Point);
	return l_Bounds;
}
} // namespace

Rect tightWorldBounds(const Object& p_Object)
{
	const StrokeData* l_Stroke = p_Object.stroke();
	if (l_Stroke == nullptr)
		return p_Object.worldBounds();
	const double l_Scale = p_Object.transform.uniformScale();
	Rect l_Bounds{};
	for (const StrokePoint& l_Point : l_Stroke->points)
		l_Bounds.expand(Rect::fromCenter(p_Object.transform.apply(DVec2{ l_Point.position }), DVec2{ static_cast<double>(l_Point.radius) * l_Scale }));
	return l_Bounds;
}

bool hitsPoint(const Object& p_Object, const DVec2 p_World, const double p_Tolerance)
{
	if (Rect l_Box; boxBounds(p_Object, l_Box))
	{
		if (!p_Object.worldBounds().inflated(p_Tolerance).contains(p_World) || !p_Object.transform.isInvertible())
			return false;
		const DVec2 l_Local = p_Object.transform.inverse().apply(p_World);
		return l_Box.inflated(p_Tolerance / std::max(p_Object.transform.uniformScale(), 1e-12)).contains(l_Local);
	}
	const StrokeData* l_Stroke = p_Object.stroke();
	if (l_Stroke == nullptr || !p_Object.worldBounds().inflated(p_Tolerance).contains(p_World))
		return false;
	return anySegment(p_Object, *l_Stroke, [&](const WorldSegment& p_Segment)
	{
		const SegmentProjection l_Projection = projectOnSegment(p_World, p_Segment.a, p_Segment.b);
		const double l_Radius = p_Segment.ra + (p_Segment.rb - p_Segment.ra) * l_Projection.t;
		return l_Projection.distance <= l_Radius + p_Tolerance;
	});
}

ObjectId pickTopmost(const Document& p_Document, const DVec2 p_World, const double p_Tolerance)
{
	const std::span<const std::unique_ptr<Object>> l_Objects = p_Document.objects();
	for (auto l_It = l_Objects.rbegin(); l_It != l_Objects.rend(); ++l_It)
	{
		if (hitsPoint(**l_It, p_World, p_Tolerance))
			return (*l_It)->id;
	}
	return INVALID_OBJECT_ID;
}

bool pointInPolygon(const DVec2 p_Point, const std::span<const DVec2> p_Polygon)
{
	bool l_Inside = false;
	const size_t l_Count = p_Polygon.size();
	for (size_t i = 0, j = l_Count - 1; i < l_Count; j = i++)
	{
		const DVec2 l_A = p_Polygon[i];
		const DVec2 l_B = p_Polygon[j];
		if ((l_A.y > p_Point.y) != (l_B.y > p_Point.y) && p_Point.x < (l_B.x - l_A.x) * (p_Point.y - l_A.y) / (l_B.y - l_A.y) + l_A.x)
			l_Inside = !l_Inside;
	}
	return l_Inside;
}

namespace
{
// A (possibly rotated or mirrored) picture or text rectangle against a polygon
bool boxTouchesPolygon(const Object& p_Object, const Rect& p_Local, const std::span<const DVec2> p_Polygon)
{
	const Rect& l_Local = p_Local;
	const std::array<DVec2, 4> l_Corners{
		p_Object.transform.apply(l_Local.min),
		p_Object.transform.apply(DVec2{ l_Local.max.x, l_Local.min.y }),
		p_Object.transform.apply(l_Local.max),
		p_Object.transform.apply(DVec2{ l_Local.min.x, l_Local.max.y }),
	};
	for (const DVec2 l_Corner : l_Corners)
	{
		if (pointInPolygon(l_Corner, p_Polygon))
			return true;
	}
	if (p_Object.transform.isInvertible())
	{
		const Affine2 l_Inverse = p_Object.transform.inverse();
		for (const DVec2 l_Vertex : p_Polygon)
		{
			if (l_Local.contains(l_Inverse.apply(l_Vertex)))
				return true;
		}
	}
	const size_t l_Count = p_Polygon.size();
	for (size_t i = 0; i < 4; ++i)
	{
		for (size_t k = 0; k < l_Count; ++k)
		{
			if (segmentSegmentDistanceSq(l_Corners[i], l_Corners[(i + 1) % 4], p_Polygon[k], p_Polygon[(k + 1) % l_Count]) <= 1e-18)
				return true;
		}
	}
	return false;
}
} // namespace

bool touchesPolygon(const Object& p_Object, const std::span<const DVec2> p_Polygon)
{
	if (p_Polygon.size() < 3)
		return false;
	const Rect l_Area = polygonBounds(p_Polygon);
	if (!p_Object.worldBounds().intersects(l_Area))
		return false;
	if (Rect l_Box; boxBounds(p_Object, l_Box))
		return boxTouchesPolygon(p_Object, l_Box, p_Polygon);
	const StrokeData* l_Stroke = p_Object.stroke();
	if (l_Stroke == nullptr)
		return false;

	// A small area lying inside a thick stroke contains no vertex and crosses no edge of the stroke's centre line
	if (hitsPoint(p_Object, p_Polygon[0], 0.0))
		return true;

	return anySegment(p_Object, *l_Stroke, [&](const WorldSegment& p_Segment)
	{
		const double l_Reach = std::max(p_Segment.ra, p_Segment.rb);
		if (!Rect::fromPoints(p_Segment.a, p_Segment.b).inflated(l_Reach).intersects(l_Area))
			return false;
		if (pointInPolygon(p_Segment.a, p_Polygon) || pointInPolygon(p_Segment.b, p_Polygon))
			return true;
		const size_t l_Count = p_Polygon.size();
		for (size_t i = 0; i < l_Count; ++i)
		{
			if (segmentSegmentDistanceSq(p_Segment.a, p_Segment.b, p_Polygon[i], p_Polygon[(i + 1) % l_Count]) <= l_Reach * l_Reach)
				return true;
		}
		return false;
	});
}

std::vector<ObjectId> objectsInPolygon(const Document& p_Document, const std::span<const DVec2> p_Polygon)
{
	std::vector<ObjectId> l_Result;
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
	{
		if (touchesPolygon(*l_Object, p_Polygon))
			l_Result.push_back(l_Object->id);
	}
	return l_Result;
}

std::vector<ObjectId> objectsInRect(const Document& p_Document, const Rect& p_Area)
{
	if (p_Area.isEmpty())
		return {};
	const std::array<DVec2, 4> l_Corners{ p_Area.min, DVec2{ p_Area.max.x, p_Area.min.y }, p_Area.max, DVec2{ p_Area.min.x, p_Area.max.y } };
	return objectsInPolygon(p_Document, l_Corners);
}
} // namespace wb
