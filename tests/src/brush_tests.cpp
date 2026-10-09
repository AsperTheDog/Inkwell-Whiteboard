#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.object;
import wb.view.camera;
import wb.brush.stroke_builder;
import wb.util.range_allocator;

namespace
{
wb::Camera makeCamera(const double p_Zoom = 1.0)
{
	wb::Camera l_Camera;
	l_Camera.setViewport({ 1000.f, 1000.f }, 1.f);
	l_Camera.setZoom(p_Zoom);
	return l_Camera;
}

wb::StrokeInput input(const float p_X, const float p_Y, const uint64_t p_Ms, const float p_Pressure = 1.f)
{
	return wb::StrokeInput{ .screen = { p_X, p_Y }, .pressure = p_Pressure, .timestampNs = p_Ms * 1'000'000ull };
}

float maxDeviationFromLine(const std::vector<wb::StrokePoint>& p_Points, const float p_Y)
{
	float l_Max = 0.f;
	for (const wb::StrokePoint& l_Point : p_Points)
		l_Max = std::max(l_Max, std::abs(l_Point.position.y - p_Y));
	return l_Max;
}
} // namespace

TEST(OneEuroFilter, ConvergesToConstantInput)
{
	wb::OneEuroFilter l_Filter;
	l_Filter.configure(1.f, 0.f, 1.f);
	wb::Vec2 l_Value = l_Filter.filter({ 0.f, 0.f }, 0.01);
	for (int i = 0; i < 2000; ++i)
		l_Value = l_Filter.filter({ 10.f, -4.f }, 0.01);
	EXPECT_NEAR(l_Value.x, 10.f, 1e-3f);
	EXPECT_NEAR(l_Value.y, -4.f, 1e-3f);
}

TEST(StrokeBuilder, TapProducesADot)
{
	wb::StrokeBuilder l_Builder;
	const wb::StrokeInput l_Tap = input(500.f, 500.f, 0);
	l_Builder.begin(l_Tap, false, 8.f, {}, makeCamera(), {});
	const wb::StrokeData l_Stroke = l_Builder.finish(&l_Tap);
	ASSERT_EQ(l_Stroke.points.size(), 1u);
	EXPECT_FLOAT_EQ(l_Stroke.points[0].radius, 4.f);
	EXPECT_FALSE(l_Builder.isActive());
}

TEST(StrokeBuilder, StraightLineStaysStraightAndReachesEnd)
{
	wb::StrokeBuilder l_Builder;
	const wb::Camera l_Camera = makeCamera();
	l_Builder.begin(input(100.f, 300.f, 0), false, 4.f, {}, l_Camera, {});
	for (int i = 1; i <= 100; ++i)
		l_Builder.add(input(100.f + static_cast<float>(i) * 5.f, 300.f, static_cast<uint64_t>(i) * 8));
	const wb::StrokeInput l_Last = input(600.f, 300.f, 808);
	const wb::StrokeData l_Stroke = l_Builder.finish(&l_Last);

	ASSERT_GE(l_Stroke.points.size(), 2u);
	EXPECT_LT(maxDeviationFromLine(l_Stroke.points, 0.f), 1e-3f);
	// Points are relative to the first sample; the end is exactly the pen-up position
	EXPECT_NEAR(l_Stroke.points.back().position.x, 500.f, 1e-3f);
	// A straight constant-width line simplifies to (almost) just its endpoints
	EXPECT_LE(l_Stroke.points.size(), 4u);
}

TEST(StrokeBuilder, WidthIsScreenRelative)
{
	wb::StrokeBuilder l_Builder;
	const wb::StrokeInput l_Tap = input(500.f, 500.f, 0);
	l_Builder.begin(l_Tap, false, 8.f, {}, makeCamera(4.0), {});
	const wb::StrokeData l_Stroke = l_Builder.finish(&l_Tap);
	EXPECT_FLOAT_EQ(l_Stroke.style.size, 2.f); // 8 points on screen at 400% = 2 world units
	EXPECT_FLOAT_EQ(l_Stroke.points[0].radius, 1.f);
}

TEST(StrokeBuilder, PressureControlsWidth)
{
	wb::BrushSettings l_Settings;
	l_Settings.pressureSmoothing = 0.f;
	l_Settings.minWidthFraction = 0.25f;
	l_Settings.pressureGamma = 1.f;

	wb::StrokeBuilder l_Builder;
	l_Builder.begin(input(0.f, 0.f, 0, 0.f), true, 10.f, {}, makeCamera(), l_Settings);
	const wb::StrokeData l_Light = l_Builder.finish(nullptr);
	EXPECT_FLOAT_EQ(l_Light.points[0].radius, 1.25f); // 10 * 0.25 / 2

	l_Builder.begin(input(0.f, 0.f, 0, 1.f), true, 10.f, {}, makeCamera(), l_Settings);
	const wb::StrokeData l_Full = l_Builder.finish(nullptr);
	EXPECT_FLOAT_EQ(l_Full.points[0].radius, 5.f);
}

TEST(StrokeBuilder, CommittedPlusTailCoversLatestSample)
{
	wb::StrokeBuilder l_Builder;
	l_Builder.begin(input(0.f, 0.f, 0), false, 4.f, {}, makeCamera(), {});
	for (int i = 1; i <= 20; ++i)
		l_Builder.add(input(static_cast<float>(i) * 10.f, 0.f, static_cast<uint64_t>(i) * 8));
	ASSERT_FALSE(l_Builder.committedPoints().empty());
	ASSERT_FALSE(l_Builder.tailPoints().empty());
	// The tail extends past the committed part towards the newest (filtered) sample
	EXPECT_GT(l_Builder.tailPoints().back().position.x, l_Builder.committedPoints().back().position.x);
	l_Builder.cancel();
	EXPECT_TRUE(l_Builder.committedPoints().empty());
}

TEST(StrokeBuilder, ResamplingSpacingIsBounded)
{
	wb::BrushSettings l_Settings;
	l_Settings.simplifyTolerancePx = -1.f; // measure the resampling alone
	wb::StrokeBuilder l_Builder;
	l_Builder.begin(input(0.f, 0.f, 0), false, 4.f, {}, makeCamera(), l_Settings);
	// Sparse samples on a circle
	for (int i = 1; i <= 36; ++i)
	{
		const float l_Angle = static_cast<float>(i) * 0.1745f;
		l_Builder.add(input(200.f * std::sin(l_Angle), 200.f - 200.f * std::cos(l_Angle), static_cast<uint64_t>(i) * 16));
	}
	const wb::StrokeData l_Stroke = l_Builder.finish(nullptr);
	for (size_t i = 1; i < l_Stroke.points.size(); ++i)
	{
		EXPECT_LE(glm::length(l_Stroke.points[i].position - l_Stroke.points[i - 1].position), l_Settings.resampleSpacingPx * 1.01f)
			<< "between points " << i - 1 << " (" << l_Stroke.points[i - 1].position.x << ", " << l_Stroke.points[i - 1].position.y << ") and " << i << " ("
			<< l_Stroke.points[i].position.x << ", " << l_Stroke.points[i].position.y << ") of " << l_Stroke.points.size();
	}
}

TEST(SimplifyStroke, KeepsCornersAndWidthChanges)
{
	std::vector<wb::StrokePoint> l_Points;
	for (int i = 0; i <= 10; ++i)
		l_Points.push_back({ .position = { static_cast<float>(i), 0.f }, .radius = 1.f });
	for (int i = 1; i <= 10; ++i)
		l_Points.push_back({ .position = { 10.f, static_cast<float>(i) }, .radius = 1.f });
	EXPECT_EQ(wb::simplifyStroke(l_Points, 0.01f).size(), 3u); // start, corner, end

	// A spike in width must survive, with its two flanks (otherwise the width would ramp from the endpoints)
	l_Points[5].radius = 3.f;
	const std::vector<wb::StrokePoint> l_Simplified = wb::simplifyStroke(l_Points, 0.01f);
	EXPECT_EQ(l_Simplified.size(), 6u);
	EXPECT_TRUE(std::ranges::any_of(l_Simplified, [](const wb::StrokePoint& p_Point) { return p_Point.radius == 3.f; }));
}

TEST(RangeAllocator, AllocateFreeCoalesce)
{
	wb::RangeAllocator l_Alloc(100);
	const auto l_A = l_Alloc.allocate(30);
	const auto l_B = l_Alloc.allocate(30);
	const auto l_C = l_Alloc.allocate(40);
	ASSERT_TRUE(l_A && l_B && l_C);
	EXPECT_FALSE(l_Alloc.allocate(1).has_value());
	EXPECT_EQ(l_Alloc.used(), 100u);

	l_Alloc.free(*l_A, 30);
	l_Alloc.free(*l_C, 40);
	EXPECT_EQ(l_Alloc.freeBlockCount(), 2u);
	l_Alloc.free(*l_B, 30);
	EXPECT_EQ(l_Alloc.freeBlockCount(), 1u);
	EXPECT_EQ(l_Alloc.largestFreeBlock(), 100u);
	EXPECT_EQ(l_Alloc.used(), 0u);
}

TEST(RangeAllocator, GrowMergesWithTail)
{
	wb::RangeAllocator l_Alloc(10);
	const auto l_A = l_Alloc.allocate(6);
	ASSERT_TRUE(l_A);
	EXPECT_FALSE(l_Alloc.allocate(8).has_value());
	l_Alloc.grow(20);
	const auto l_B = l_Alloc.allocate(14);
	ASSERT_TRUE(l_B);
	EXPECT_EQ(*l_B, 6u);
	EXPECT_EQ(l_Alloc.used(), 20u);
}

TEST(StrokeBuilder, StabilizerRopeIronsOutWobblesAndStillReachesTheEnd)
{
	const auto l_Draw = [](const float p_Rope)
	{
		wb::BrushSettings l_Settings;
		l_Settings.ropePx = p_Rope;
		wb::StrokeBuilder l_Builder;
		l_Builder.begin(input(100.f, 500.f, 0), false, 4.f, {}, makeCamera(), l_Settings);
		for (int i = 1; i <= 120; ++i)
			l_Builder.add(input(100.f + static_cast<float>(i) * 5.f, 500.f + (i % 2 == 0 ? 6.f : -6.f), static_cast<uint64_t>(i) * 4));
		const wb::StrokeInput l_Last = input(700.f, 500.f, 500);
		return l_Builder.finish(&l_Last);
	};
	const wb::StrokeData l_Plain = l_Draw(0.f);
	const wb::StrokeData l_Roped = l_Draw(40.f);
	// Away from the ends the wobble nearly vanishes with the rope
	const auto l_Wobble = [](const wb::StrokeData& p_Stroke)
	{
		float l_Max = 0.f;
		for (size_t i = p_Stroke.points.size() / 4; i < p_Stroke.points.size() * 3 / 4; ++i)
			l_Max = std::max(l_Max, std::abs(p_Stroke.points[i].position.y - p_Stroke.points[p_Stroke.points.size() / 2].position.y));
		return l_Max;
	};
	EXPECT_LT(l_Wobble(l_Roped), l_Wobble(l_Plain) * 0.5f);
	// The stroke still ends at the pen-up position (the first point is the origin)
	EXPECT_NEAR(l_Roped.points.back().position.x, 600.f, 1.f);
}
