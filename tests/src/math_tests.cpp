#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;

TEST(Color, Rgba8RoundTrip)
{
	const wb::Color l_Color = wb::Color::fromRgba8(0x3366CCFFu);
	EXPECT_EQ(l_Color.toRgba8(), 0x3366CCFFu);
	EXPECT_FLOAT_EQ(l_Color.a, 1.f);
}

TEST(Rect, DefaultIsEmpty)
{
	const wb::Rect l_Rect{};
	EXPECT_TRUE(l_Rect.isEmpty());
	EXPECT_FALSE(l_Rect.intersects(wb::Rect::fromPoints({ -1.0, -1.0 }, { 1.0, 1.0 })));
}

TEST(Rect, ExpandAndIntersect)
{
	wb::Rect l_Rect{};
	l_Rect.expand(wb::DVec2{ 1.0, 2.0 });
	l_Rect.expand(wb::DVec2{ -3.0, 5.0 });
	EXPECT_EQ(l_Rect.min, (wb::DVec2{ -3.0, 2.0 }));
	EXPECT_EQ(l_Rect.max, (wb::DVec2{ 1.0, 5.0 }));
	EXPECT_TRUE(l_Rect.contains({ 0.0, 3.0 }));
	EXPECT_TRUE(l_Rect.intersects(wb::Rect::fromPoints({ 1.0, 5.0 }, { 9.0, 9.0 })));
	EXPECT_FALSE(l_Rect.intersects(wb::Rect::fromPoints({ 1.5, 5.0 }, { 9.0, 9.0 })));
}

TEST(Rect, PrecisionFarFromOrigin)
{
	const wb::DVec2 l_Center{ 1e9, -1e9 };
	const wb::Rect l_Rect = wb::Rect::fromCenter(l_Center, { 0.25, 0.25 });
	EXPECT_TRUE(l_Rect.contains(l_Center + wb::DVec2{ 0.2, -0.2 }));
	EXPECT_FALSE(l_Rect.contains(l_Center + wb::DVec2{ 0.3, 0.0 }));
}
