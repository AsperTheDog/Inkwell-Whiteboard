#include <cmath>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.brush.shapes;
import wb.view.ruler;

namespace
{
using wb::DVec2;
}

TEST(Shapes, RectangleIsClosedAndSpansTheDrag)
{
	const std::vector<DVec2> l_Points = wb::shapeOutline(wb::ShapeKind::Rectangle, { 10.0, 20.0 }, { 110.0, 70.0 }, {});
	ASSERT_EQ(l_Points.size(), 5u);
	EXPECT_EQ(l_Points.front(), l_Points.back());
	EXPECT_EQ(l_Points[0], DVec2(10.0, 20.0));
	EXPECT_EQ(l_Points[2], DVec2(110.0, 70.0));
}

TEST(Shapes, DraggingBackwardsGivesTheSameRectangle)
{
	const std::vector<DVec2> l_Forward = wb::shapeOutline(wb::ShapeKind::Rectangle, { 10.0, 20.0 }, { 110.0, 70.0 }, {});
	const std::vector<DVec2> l_Backward = wb::shapeOutline(wb::ShapeKind::Rectangle, { 110.0, 70.0 }, { 10.0, 20.0 }, {});
	EXPECT_EQ(l_Forward, l_Backward);
}

TEST(Shapes, ConstrainMakesSquaresAndCircles)
{
	const std::vector<DVec2> l_Square = wb::shapeOutline(wb::ShapeKind::Rectangle, { 0.0, 0.0 }, { 100.0, 40.0 }, { .constrain = true });
	ASSERT_EQ(l_Square.size(), 5u);
	EXPECT_EQ(l_Square[2], DVec2(100.0, 100.0));

	const std::vector<DVec2> l_Circle = wb::shapeOutline(wb::ShapeKind::Ellipse, { 0.0, 0.0 }, { 60.0, -20.0 }, { .constrain = true });
	ASSERT_GT(l_Circle.size(), 8u);
	DVec2 l_Min{ 1e9 };
	DVec2 l_Max{ -1e9 };
	for (const DVec2 l_Point : l_Circle)
	{
		l_Min = glm::min(l_Min, l_Point);
		l_Max = glm::max(l_Max, l_Point);
	}
	EXPECT_NEAR(l_Max.x - l_Min.x, l_Max.y - l_Min.y, 1e-6);
}

TEST(Shapes, ConstrainedLinesGoInSteps)
{
	const std::vector<DVec2> l_Line = wb::shapeOutline(wb::ShapeKind::Line, { 0.0, 0.0 }, { 100.0, 12.0 }, { .constrain = true });
	ASSERT_EQ(l_Line.size(), 2u);
	EXPECT_NEAR(l_Line[1].y, 0.0, 1e-9); // 6.8 degrees rounds to horizontal
	EXPECT_NEAR(l_Line[1].x, std::hypot(100.0, 12.0), 1e-9);
}

TEST(Shapes, FromCenterGrowsBothWays)
{
	const std::vector<DVec2> l_Points = wb::shapeOutline(wb::ShapeKind::Rectangle, { 50.0, 50.0 }, { 70.0, 60.0 }, { .fromCenter = true });
	ASSERT_EQ(l_Points.size(), 5u);
	EXPECT_EQ(l_Points[0], DVec2(30.0, 40.0));
	EXPECT_EQ(l_Points[2], DVec2(70.0, 60.0));
}

TEST(Shapes, ArrowHeadSitsAtTheEnd)
{
	const std::vector<DVec2> l_Arrow = wb::shapeOutline(wb::ShapeKind::Arrow, { 0.0, 0.0 }, { 200.0, 0.0 }, { .strokeWidth = 4.0 });
	ASSERT_EQ(l_Arrow.size(), 5u);
	EXPECT_EQ(l_Arrow[1], DVec2(200.0, 0.0));
	EXPECT_EQ(l_Arrow[3], DVec2(200.0, 0.0));
	EXPECT_LT(l_Arrow[2].x, 200.0);
	EXPECT_NEAR(l_Arrow[2].y, -l_Arrow[4].y, 1e-9);
}

TEST(Shapes, TinyDragsMakeNothing)
{
	EXPECT_TRUE(wb::shapeOutline(wb::ShapeKind::Rectangle, { 5.0, 5.0 }, { 5.2, 80.0 }, {}).empty());
	EXPECT_TRUE(wb::shapeOutline(wb::ShapeKind::Line, { 5.0, 5.0 }, { 5.3, 5.3 }, {}).empty());
}

TEST(Ruler, SnapsToTheNearEdgeOnly)
{
	wb::Ruler l_Ruler;
	l_Ruler.center = { 100.0, 100.0 };
	l_Ruler.length = 400.0;
	l_Ruler.angle = 0.0;
	const double l_Half = wb::Ruler::THICKNESS * 0.5;

	const auto l_Top = l_Ruler.snapEdge({ 120.0, 100.0 - l_Half - 3.0 }, 8.0);
	ASSERT_TRUE(l_Top.has_value());
	EXPECT_NEAR(l_Top->origin.y, 100.0 - l_Half, 1e-9);

	const auto l_Bottom = l_Ruler.snapEdge({ 120.0, 100.0 + l_Half + 3.0 }, 8.0);
	ASSERT_TRUE(l_Bottom.has_value());
	EXPECT_NEAR(l_Bottom->origin.y, 100.0 + l_Half, 1e-9);

	EXPECT_FALSE(l_Ruler.snapEdge({ 120.0, 100.0 }, 8.0).has_value()) << "the middle of the body is for dragging";
	EXPECT_FALSE(l_Ruler.snapEdge({ 120.0, 100.0 - l_Half - 40.0 }, 8.0).has_value());
	EXPECT_FALSE(l_Ruler.snapEdge({ 100.0 + 220.0, 100.0 - l_Half }, 8.0).has_value()) << "past the end";
}

TEST(Ruler, ProjectionFollowsTheTiltedEdge)
{
	wb::Ruler l_Ruler;
	l_Ruler.angle = 3.14159265358979323846 / 6.0;
	const wb::RulerLine l_Edge = l_Ruler.edge(0);
	const DVec2 l_Projected = wb::projectOntoLine(l_Edge, l_Edge.origin + l_Edge.direction * 50.0 + l_Ruler.normal() * 4.0);
	EXPECT_NEAR(glm::length(l_Projected - (l_Edge.origin + l_Edge.direction * 50.0)), 0.0, 1e-9);
	EXPECT_TRUE(l_Ruler.inBody(l_Ruler.center));
	EXPECT_NEAR(wb::snapRulerAngle(0.01, 3.14159265358979323846 / 4.0, 0.03), 0.0, 1e-12);
	EXPECT_NEAR(wb::snapRulerAngle(0.2, 3.14159265358979323846 / 4.0, 0.03), 0.2, 1e-12);
}

// ---------------------------------------------------------------------------------------------- smoothing

import wb.doc.object;
import wb.brush.smoothing;

TEST(Smoothing, FlattensWobblesAndKeepsTheEnds)
{
	wb::StrokeData l_Stroke;
	for (int i = 0; i <= 80; ++i)
		l_Stroke.points.push_back({ .position = { static_cast<float>(i) * 1.5f, (i % 2 == 0 ? 1.f : -1.f) * 0.8f }, .radius = 1.f });
	const std::vector<wb::StrokePoint> l_After = wb::smoothedPoints(l_Stroke.points, wb::smoothingReach(l_Stroke));
	ASSERT_EQ(l_After.size(), l_Stroke.points.size());
	EXPECT_EQ(l_After.front(), l_Stroke.points.front());
	EXPECT_EQ(l_After.back(), l_Stroke.points.back());
	for (size_t i = 10; i < 70; ++i)
		EXPECT_LT(std::abs(l_After[i].position.y), 0.2f);
}

TEST(Smoothing, LeavesShortStrokesAlone)
{
	std::vector<wb::StrokePoint> l_Points(4, wb::StrokePoint{ .position = { 0.f, 0.f }, .radius = 1.f });
	EXPECT_TRUE(wb::smoothedPoints(l_Points, 5.0).empty());
}
