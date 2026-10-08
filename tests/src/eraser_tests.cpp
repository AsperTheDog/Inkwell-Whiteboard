#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.brush.eraser;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.commands;

namespace
{
// Horizontal stroke from x = 0 to x = 100 with a constant radius, a point every unit
std::unique_ptr<wb::Object> makeLine(wb::Document& p_Document, const float p_Radius = 1.f, const wb::Affine2& p_Transform = {})
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = p_Transform;
	wb::StrokeData l_Stroke;
	for (int i = 0; i <= 100; ++i)
		l_Stroke.points.push_back({ .position = { static_cast<float>(i), 0.f }, .radius = p_Radius });
	l_Object->payload = std::move(l_Stroke);
	l_Object->refreshBounds();
	return l_Object;
}

wb::EraserCapsule press(const double p_X, const double p_Y, const double p_Radius)
{
	return { .a = { p_X, p_Y }, .b = { p_X, p_Y }, .radius = p_Radius };
}
} // namespace

TEST(Eraser, MiddlePressSplitsIntoTwoPiecesWithExactCuts)
{
	wb::Document l_Doc;
	const auto l_Line = makeLine(l_Doc);
	const auto l_Pieces = wb::cutStroke(*l_Line, press(50.0, 0.0, 5.0));
	ASSERT_TRUE(l_Pieces.has_value());
	ASSERT_EQ(l_Pieces->size(), 2u);
	// The stroke outline (radius 1) must stop exactly at the eraser disc (radius 5): x = 50 -/+ 6
	EXPECT_NEAR(l_Pieces->front().points.back().position.x, 44.f, 1e-3f);
	EXPECT_NEAR(l_Pieces->back().points.front().position.x, 56.f, 1e-3f);
	EXPECT_FLOAT_EQ(l_Pieces->front().points.front().position.x, 0.f);
	EXPECT_FLOAT_EQ(l_Pieces->back().points.back().position.x, 100.f);
}

TEST(Eraser, MissLeavesStrokeUntouched)
{
	wb::Document l_Doc;
	const auto l_Line = makeLine(l_Doc);
	EXPECT_FALSE(wb::cutStroke(*l_Line, press(50.0, 20.0, 5.0)).has_value());
	EXPECT_FALSE(wb::strokeTouches(*l_Line, press(50.0, 20.0, 5.0)));
	EXPECT_TRUE(wb::strokeTouches(*l_Line, press(50.0, 5.5, 5.0)));
}

TEST(Eraser, CoveringTheWholeStrokeRemovesIt)
{
	wb::Document l_Doc;
	const auto l_Line = makeLine(l_Doc);
	const auto l_Pieces = wb::cutStroke(*l_Line, { .a = { -10.0, 0.0 }, .b = { 110.0, 0.0 }, .radius = 5.0 });
	ASSERT_TRUE(l_Pieces.has_value());
	EXPECT_TRUE(l_Pieces->empty());
}

TEST(Eraser, CuttingTheEndLeavesOnePiece)
{
	wb::Document l_Doc;
	const auto l_Line = makeLine(l_Doc);
	const auto l_Pieces = wb::cutStroke(*l_Line, press(0.0, 0.0, 5.0));
	ASSERT_TRUE(l_Pieces.has_value());
	ASSERT_EQ(l_Pieces->size(), 1u);
	EXPECT_NEAR(l_Pieces->front().points.front().position.x, 6.f, 1e-3f);
	EXPECT_FLOAT_EQ(l_Pieces->front().points.back().position.x, 100.f);
}

TEST(Eraser, SweepCutsEveryCrossedPart)
{
	wb::Document l_Doc;
	const auto l_Line = makeLine(l_Doc);
	// A vertical swipe cuts one gap; the horizontal part of a swipe along the stroke removes a whole run
	const auto l_Swipe = wb::cutStroke(*l_Line, { .a = { 30.0, -20.0 }, .b = { 30.0, 20.0 }, .radius = 2.0 });
	ASSERT_TRUE(l_Swipe.has_value());
	EXPECT_EQ(l_Swipe->size(), 2u);
	const auto l_Along = wb::cutStroke(*l_Line, { .a = { 20.0, 0.0 }, .b = { 70.0, 0.0 }, .radius = 2.0 });
	ASSERT_TRUE(l_Along.has_value());
	ASSERT_EQ(l_Along->size(), 2u);
	EXPECT_NEAR(l_Along->front().points.back().position.x, 17.f, 1e-3f);
	EXPECT_NEAR(l_Along->back().points.front().position.x, 73.f, 1e-3f);
}

TEST(Eraser, CutsInsideLongSegmentsAreNotSnappedToPoints)
{
	wb::Document l_Doc;
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = l_Doc.allocateId();
	l_Object->payload = wb::StrokeData{ .style = {}, .points = { { .position = { 0.f, 0.f }, .radius = 1.f }, { .position = { 100.f, 0.f }, .radius = 1.f } } };
	l_Object->refreshBounds();
	const auto l_Pieces = wb::cutStroke(*l_Object, press(37.0, 0.0, 3.0));
	ASSERT_TRUE(l_Pieces.has_value());
	ASSERT_EQ(l_Pieces->size(), 2u);
	EXPECT_NEAR(l_Pieces->front().points.back().position.x, 33.f, 1e-3f);
	EXPECT_NEAR(l_Pieces->back().points.front().position.x, 41.f, 1e-3f);
}

TEST(Eraser, WidthChangesMoveTheCut)
{
	wb::Document l_Doc;
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = l_Doc.allocateId();
	// Thin at the left, thick at the right: the thick side touches the eraser from further away
	l_Object->payload = wb::StrokeData{ .style = {}, .points = { { .position = { 0.f, 0.f }, .radius = 0.f }, { .position = { 100.f, 0.f }, .radius = 10.f } } };
	l_Object->refreshBounds();
	const auto l_Pieces = wb::cutStroke(*l_Object, press(50.0, 0.0, 2.0));
	ASSERT_TRUE(l_Pieces.has_value());
	ASSERT_EQ(l_Pieces->size(), 2u);
	// |x - 50| <= 2 + 0.1 x  =>  x in [48 / 1.1, 52 / 0.9]
	EXPECT_NEAR(l_Pieces->front().points.back().position.x, 48.f / 1.1f, 1e-3f);
	EXPECT_NEAR(l_Pieces->back().points.front().position.x, 52.f / 0.9f, 1e-3f);
}

TEST(Eraser, DotsAreRemovedWhenTouched)
{
	wb::Document l_Doc;
	auto l_Dot = std::make_unique<wb::Object>();
	l_Dot->id = l_Doc.allocateId();
	l_Dot->payload = wb::StrokeData{ .style = {}, .points = { { .position = { 10.f, 10.f }, .radius = 2.f } } };
	l_Dot->refreshBounds();
	EXPECT_TRUE(wb::strokeTouches(*l_Dot, press(14.0, 10.0, 2.5)));
	EXPECT_FALSE(wb::strokeTouches(*l_Dot, press(20.0, 10.0, 2.5)));
	const auto l_Pieces = wb::cutStroke(*l_Dot, press(14.0, 10.0, 2.5));
	ASSERT_TRUE(l_Pieces.has_value());
	EXPECT_TRUE(l_Pieces->empty());
}

TEST(Eraser, TransformedStrokesAreErasedInWorldSpace)
{
	wb::Document l_Doc;
	// Local line 0..100 scaled x2 and moved: world x = 10 + 2 * local
	const wb::Affine2 l_Transform = wb::Affine2::translate({ 10.0, 0.0 }) * wb::Affine2::scale({ 2.0, 2.0 });
	const auto l_Line = makeLine(l_Doc, 1.f, l_Transform);
	// World radius of the stroke is 2; eraser radius 10 at world x = 110 (local 50): cut at local 50 -/+ (10 + 2) / 2
	const auto l_Pieces = wb::cutStroke(*l_Line, press(110.0, 0.0, 10.0));
	ASSERT_TRUE(l_Pieces.has_value());
	ASSERT_EQ(l_Pieces->size(), 2u);
	EXPECT_NEAR(l_Pieces->front().points.back().position.x, 44.f, 1e-3f);
	EXPECT_NEAR(l_Pieces->back().points.front().position.x, 56.f, 1e-3f);
}

TEST(Eraser, ReplaceCommandRoundTripKeepsZOrder)
{
	wb::Document l_Doc;
	wb::History l_History;
	auto l_A = makeLine(l_Doc);
	auto l_B = makeLine(l_Doc);
	auto l_C = makeLine(l_Doc);
	const wb::ObjectId l_IdA = l_A->id;
	const wb::ObjectId l_IdB = l_B->id;
	const wb::ObjectId l_IdC = l_C->id;
	l_Doc.insert(std::move(l_A));
	l_Doc.insert(std::move(l_B));
	l_Doc.insert(std::move(l_C));

	auto l_Command = std::make_unique<wb::ReplaceObjectsCommand>("Erase");
	for (int i = 0; i < 2; ++i)
	{
		// Two successive cuts: the second one works on a piece of the first
		const wb::Object* l_Target = i == 0 ? l_Doc.find(l_IdB) : l_Doc.objects()[1].get();
		const wb::ObjectId l_TargetId = l_Target->id;
		const auto l_Pieces = wb::cutStroke(*l_Target, press(i == 0 ? 50.0 : 20.0, 0.0, 3.0));
		ASSERT_TRUE(l_Pieces.has_value());
		std::vector<std::unique_ptr<wb::Object>> l_Added;
		for (const wb::StrokeData& l_Piece : *l_Pieces)
		{
			auto l_Object = std::make_unique<wb::Object>();
			l_Object->id = l_Doc.allocateId();
			l_Object->payload = l_Piece;
			l_Added.push_back(std::move(l_Object));
		}
		l_Command->replace(l_Doc, l_TargetId, std::move(l_Added));
	}
	EXPECT_EQ(l_Doc.size(), 5u);
	EXPECT_EQ(l_Doc.objects().front()->id, l_IdA);
	EXPECT_EQ(l_Doc.objects().back()->id, l_IdC);
	l_History.push(std::move(l_Command));

	ASSERT_TRUE(l_History.undo(l_Doc));
	ASSERT_EQ(l_Doc.size(), 3u);
	EXPECT_EQ(l_Doc.objects()[0]->id, l_IdA);
	EXPECT_EQ(l_Doc.objects()[1]->id, l_IdB);
	EXPECT_EQ(l_Doc.objects()[2]->id, l_IdC);

	ASSERT_TRUE(l_History.redo(l_Doc));
	EXPECT_EQ(l_Doc.size(), 5u);
	EXPECT_EQ(l_Doc.find(l_IdB), nullptr);
	ASSERT_TRUE(l_History.undo(l_Doc));
	EXPECT_EQ(l_Doc.size(), 3u);
	EXPECT_NE(l_Doc.find(l_IdB), nullptr);
}
