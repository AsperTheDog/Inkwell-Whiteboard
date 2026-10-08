#include <cmath>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.view.camera;

namespace
{
void expectNear(const wb::DVec2 p_A, const wb::DVec2 p_B, const double p_Eps = 1e-9)
{
	EXPECT_NEAR(p_A.x, p_B.x, p_Eps);
	EXPECT_NEAR(p_A.y, p_B.y, p_Eps);
}
} // namespace

TEST(Affine2, ComposeAndInverse)
{
	const wb::Affine2 l_T = wb::Affine2::translate({ 10.0, -5.0 }) * wb::Affine2::rotate(0.7) * wb::Affine2::scale({ 2.0, 3.0 });
	const wb::DVec2 l_P{ 1.5, -2.25 };
	expectNear(l_T.inverse().apply(l_T.apply(l_P)), l_P);
	expectNear((l_T * l_T.inverse()).apply(l_P), l_P);
	EXPECT_NEAR(l_T.determinant(), 6.0, 1e-12);
	EXPECT_NEAR(l_T.uniformScale(), std::sqrt(6.0), 1e-12);
}

TEST(Affine2, RotateIsCounterClockwiseInYUp)
{
	expectNear(wb::Affine2::rotate(wb::PI / 2.0).apply({ 1.0, 0.0 }), { 0.0, 1.0 });
}

TEST(Affine2, AroundPivotKeepsPivot)
{
	const wb::DVec2 l_Pivot{ 3.0, 4.0 };
	const wb::Affine2 l_T = wb::Affine2::around(l_Pivot, wb::Affine2::rotate(1.0) * wb::Affine2::scale({ 2.0, 0.5 }));
	expectNear(l_T.apply(l_Pivot), l_Pivot);
}

TEST(Affine2, ApplyBoundsContainsRotatedCorners)
{
	const wb::Rect l_Local = wb::Rect::fromPoints({ -1.0, -1.0 }, { 1.0, 1.0 });
	const wb::Rect l_World = wb::Affine2::rotate(wb::PI / 4.0).applyBounds(l_Local);
	EXPECT_NEAR(l_World.max.x, std::sqrt(2.0), 1e-12);
	EXPECT_NEAR(l_World.min.y, -std::sqrt(2.0), 1e-12);
}

TEST(Segments, ProjectAndDistance)
{
	const wb::SegmentProjection l_Proj = wb::projectOnSegment({ 5.0, 3.0 }, { 0.0, 0.0 }, { 10.0, 0.0 });
	EXPECT_NEAR(l_Proj.distance, 3.0, 1e-12);
	EXPECT_NEAR(l_Proj.t, 0.5, 1e-12);

	EXPECT_NEAR(wb::segmentSegmentDistanceSq({ 0.0, 0.0 }, { 10.0, 0.0 }, { 5.0, -1.0 }, { 5.0, 1.0 }), 0.0, 1e-12); // crossing
	EXPECT_NEAR(wb::segmentSegmentDistanceSq({ 0.0, 0.0 }, { 10.0, 0.0 }, { 0.0, 2.0 }, { 10.0, 2.0 }), 4.0, 1e-12); // parallel
	EXPECT_NEAR(wb::segmentSegmentDistanceSq({ 0.0, 0.0 }, { 0.0, 0.0 }, { 3.0, 4.0 }, { 3.0, 4.0 }), 25.0, 1e-12); // points
}

TEST(Camera, ScreenWorldRoundTrip)
{
	wb::Camera l_Camera;
	l_Camera.setViewport({ 1920.f, 1080.f }, 1.5f);
	l_Camera.setCenter({ 1e7, -3e6 });
	l_Camera.setZoom(2.5);
	const wb::DVec2 l_Screen{ 123.25, 987.5 };
	expectNear(l_Camera.worldToScreen(l_Camera.screenToWorld(l_Screen)), l_Screen, 1e-6);
	expectNear(l_Camera.worldToScreenTransform().apply({ 1e7 + 1.0, -3e6 }), l_Camera.worldToScreen({ 1e7 + 1.0, -3e6 }), 1e-6);
}

TEST(Camera, ZoomAtKeepsAnchor)
{
	wb::Camera l_Camera;
	l_Camera.setViewport({ 800.f, 600.f }, 1.f);
	const wb::DVec2 l_Anchor{ 100.0, 450.0 };
	const wb::DVec2 l_WorldBefore = l_Camera.screenToWorld(l_Anchor);
	l_Camera.zoomAt(l_Anchor, 3.0);
	EXPECT_DOUBLE_EQ(l_Camera.zoom(), 3.0);
	expectNear(l_Camera.screenToWorld(l_Anchor), l_WorldBefore, 1e-9);
}

TEST(Camera, ZoomIsClamped)
{
	wb::Camera l_Camera;
	l_Camera.setZoom(1e9);
	EXPECT_DOUBLE_EQ(l_Camera.zoom(), wb::Camera::MAX_ZOOM);
	l_Camera.setZoom(0.0);
	EXPECT_DOUBLE_EQ(l_Camera.zoom(), wb::Camera::MIN_ZOOM);
}

TEST(Camera, FitCentersContent)
{
	wb::Camera l_Camera;
	l_Camera.setViewport({ 1000.f, 500.f }, 2.f);
	const wb::Rect l_Content = wb::Rect::fromPoints({ 0.0, 0.0 }, { 100.0, 100.0 });
	l_Camera.fit(l_Content, 50.0, 100.0);
	expectNear(l_Camera.center(), { 50.0, 50.0 });
	// Height-limited: (500 - 100) px / 100 units / pixelScale 2
	EXPECT_NEAR(l_Camera.zoom(), 2.0, 1e-12);
}

TEST(Camera, PrecisionFarFromOrigin)
{
	wb::Camera l_Camera;
	l_Camera.setViewport({ 1000.f, 1000.f }, 1.f);
	l_Camera.setCenter({ 1e8, 1e8 });
	l_Camera.setZoom(64.0);
	// One screen pixel must still resolve to distinct world positions
	const wb::DVec2 l_A = l_Camera.screenToWorld({ 500.0, 500.0 });
	const wb::DVec2 l_B = l_Camera.screenToWorld({ 501.0, 500.0 });
	EXPECT_NEAR(l_B.x - l_A.x, 1.0 / 64.0, 1e-9);
}
