#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.commands;
import wb.doc.hit;
import wb.doc.selection;
import wb.doc.gizmo;
import wb.doc.edit;

namespace
{
using wb::DVec2;

// A horizontal stroke from (0,0) to (length,0) with the given radius, placed at p_Origin
wb::ObjectId addStroke(wb::Document& p_Document, const DVec2 p_Origin, const float p_Length = 10.f, const float p_Radius = 1.f)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate(p_Origin);
	l_Object->payload = wb::StrokeData{ .style = {}, .points = { { .position = { 0.f, 0.f }, .radius = p_Radius }, { .position = { p_Length, 0.f }, .radius = p_Radius } } };
	const wb::ObjectId l_Id = l_Object->id;
	p_Document.insert(std::move(l_Object));
	return l_Id;
}

std::vector<wb::ObjectId> order(const wb::Document& p_Document)
{
	std::vector<wb::ObjectId> l_Ids;
	for (const auto& l_Object : p_Document.objects())
		l_Ids.push_back(l_Object->id);
	return l_Ids;
}

void expectNear(const DVec2 p_A, const DVec2 p_B, const double p_Eps = 1e-9)
{
	EXPECT_NEAR(p_A.x, p_B.x, p_Eps);
	EXPECT_NEAR(p_A.y, p_B.y, p_Eps);
}
} // namespace

// ---------------------------------------------------------------------------------------------- hit testing

TEST(Hit, PointOnAndNearStroke)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addStroke(l_Doc, { 100.0, 50.0 });
	const wb::Object& l_Object = *l_Doc.find(l_Id);

	EXPECT_TRUE(wb::hitsPoint(l_Object, { 105.0, 50.0 }, 0.0));
	EXPECT_TRUE(wb::hitsPoint(l_Object, { 105.0, 50.9 }, 0.0)) << "inside the stroke's radius";
	EXPECT_FALSE(wb::hitsPoint(l_Object, { 105.0, 52.0 }, 0.0));
	EXPECT_TRUE(wb::hitsPoint(l_Object, { 105.0, 52.0 }, 1.5)) << "tolerance widens the target";
	EXPECT_FALSE(wb::hitsPoint(l_Object, { 130.0, 50.0 }, 2.0));
	EXPECT_TRUE(wb::hitsPoint(l_Object, { 111.5, 50.0 }, 1.0)) << "past the round end cap";
}

TEST(Hit, TransformedStrokeIsHitWhereItIsDrawn)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addStroke(l_Doc, { 0.0, 0.0 }, 10.f, 1.f);
	// Rotate 90 degrees, then scale x2: the stroke now runs from (0,0) to (0,20) with radius 2
	l_Doc.modify(l_Id, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::scale({ 2.0, 2.0 }) * wb::Affine2::rotate(wb::PI * 0.5); });
	const wb::Object& l_Object = *l_Doc.find(l_Id);

	EXPECT_TRUE(wb::hitsPoint(l_Object, { 0.0, 10.0 }, 0.0));
	EXPECT_TRUE(wb::hitsPoint(l_Object, { 1.9, 10.0 }, 0.0)) << "radius doubled by the scale";
	EXPECT_FALSE(wb::hitsPoint(l_Object, { 10.0, 0.0 }, 0.0)) << "where the untransformed stroke would be";
}

TEST(Hit, TopmostWins)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Bottom = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_Top = addStroke(l_Doc, { 0.0, 0.0 });
	EXPECT_EQ(wb::pickTopmost(l_Doc, { 5.0, 0.0 }, 0.5), l_Top);
	EXPECT_EQ(wb::pickTopmost(l_Doc, { 5.0, 20.0 }, 0.5), wb::INVALID_OBJECT_ID);
	(void)l_Bottom;
}

TEST(Hit, SinglePointDot)
{
	wb::Document l_Doc;
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = l_Doc.allocateId();
	l_Object->payload = wb::StrokeData{ .style = {}, .points = { { .position = { 3.f, 4.f }, .radius = 2.f } } };
	const wb::ObjectId l_Id = l_Object->id;
	l_Doc.insert(std::move(l_Object));
	EXPECT_TRUE(wb::hitsPoint(*l_Doc.find(l_Id), { 3.0, 5.5 }, 0.0));
	EXPECT_FALSE(wb::hitsPoint(*l_Doc.find(l_Id), { 3.0, 7.0 }, 0.0));
}

TEST(Hit, TightBoundsFollowRotation)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addStroke(l_Doc, { 0.0, 0.0 }, 10.f, 0.f);
	l_Doc.modify(l_Id, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::rotate(wb::PI * 0.5); });
	const wb::Rect l_Tight = wb::tightWorldBounds(*l_Doc.find(l_Id));
	expectNear(l_Tight.min, { 0.0, 0.0 }, 1e-6);
	expectNear(l_Tight.max, { 0.0, 10.0 }, 1e-6);
}

TEST(Hit, RectSelectsTouchedStrokesOnly)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Inside = addStroke(l_Doc, { 10.0, 10.0 });
	const wb::ObjectId l_Crossing = addStroke(l_Doc, { 25.0, 15.0 });  // runs from x=25 to 35, the rect ends at x=30
	const wb::ObjectId l_Outside = addStroke(l_Doc, { 100.0, 100.0 });
	const wb::ObjectId l_Spanning = addStroke(l_Doc, { -50.0, 20.0 }, 200.f); // passes through without an end point inside
	const wb::ObjectId l_Above = addStroke(l_Doc, { 10.0, 60.0 });

	const wb::Rect l_Area{ .min = { 0.0, 0.0 }, .max = { 30.0, 40.0 } };
	const std::vector<wb::ObjectId> l_Hits = wb::objectsInRect(l_Doc, l_Area);
	EXPECT_EQ(l_Hits, (std::vector<wb::ObjectId>{ l_Inside, l_Crossing, l_Spanning }));
	(void)l_Outside;
	(void)l_Above;
}

TEST(Hit, TinyAreaInsideThickStroke)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addStroke(l_Doc, { 0.0, 0.0 }, 100.f, 10.f);
	const wb::Rect l_Area{ .min = { 40.0, -1.0 }, .max = { 42.0, 1.0 } };
	EXPECT_EQ(wb::objectsInRect(l_Doc, l_Area), std::vector<wb::ObjectId>{ l_Id });
}

TEST(Hit, LassoPolygonConcave)
{
	wb::Document l_Doc;
	const wb::ObjectId l_InTheBody = addStroke(l_Doc, { 12.0, 5.0 }, 4.f, 0.5f);
	const wb::ObjectId l_Caught = addStroke(l_Doc, { 2.0, 2.0 }, 4.f, 0.5f);
	const wb::ObjectId l_InTheNotch = addStroke(l_Doc, { 12.0, 25.0 }, 4.f, 0.5f);
	// A "U" shape open towards the top: the notch (x in 10..20, y < 20) is outside the polygon
	const std::array<DVec2, 8> l_Polygon{ { { 0.0, 0.0 }, { 30.0, 0.0 }, { 30.0, 30.0 }, { 20.0, 30.0 }, { 20.0, 20.0 }, { 10.0, 20.0 }, { 10.0, 30.0 }, { 0.0, 30.0 } } };
	EXPECT_TRUE(wb::pointInPolygon({ 5.0, 10.0 }, l_Polygon));
	EXPECT_FALSE(wb::pointInPolygon({ 15.0, 25.0 }, l_Polygon));
	const std::vector<wb::ObjectId> l_Hits = wb::objectsInPolygon(l_Doc, l_Polygon);
	EXPECT_EQ(l_Hits, (std::vector<wb::ObjectId>{ l_InTheBody, l_Caught }));
	(void)l_InTheNotch;
}

// ---------------------------------------------------------------------------------------------- selection

TEST(Selection, DropsObjectsThatDisappear)
{
	wb::Document l_Doc;
	wb::Selection l_Selection(l_Doc);
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 20.0 });
	const std::array<wb::ObjectId, 3> l_Ids{ l_A, l_B, 999 };
	l_Selection.set(l_Ids);
	EXPECT_EQ(l_Selection.size(), 2u) << "unknown ids are ignored";

	const uint64_t l_Revision = l_Selection.revision();
	(void)l_Doc.take(l_A);
	EXPECT_FALSE(l_Selection.contains(l_A));
	EXPECT_TRUE(l_Selection.contains(l_B));
	EXPECT_NE(l_Selection.revision(), l_Revision);

	l_Doc.clear();
	EXPECT_TRUE(l_Selection.empty());
}

TEST(Selection, ToggleAddRemoveAndOrder)
{
	wb::Document l_Doc;
	wb::Selection l_Selection(l_Doc);
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 20.0 });
	const wb::ObjectId l_C = addStroke(l_Doc, { 0.0, 40.0 });
	l_Selection.add(l_C);
	l_Selection.add(l_A);
	l_Selection.add(l_A);
	EXPECT_EQ(l_Selection.size(), 2u);
	l_Selection.toggle(l_B);
	l_Selection.toggle(l_C);
	EXPECT_EQ(l_Selection.orderedIds(), (std::vector<wb::ObjectId>{ l_A, l_B })) << "document order, not selection order";
	const wb::Rect l_Bounds = l_Selection.bounds();
	expectNear(l_Bounds.min, { -1.0, -1.0 });
	expectNear(l_Bounds.max, { 11.0, 21.0 });
}

// ---------------------------------------------------------------------------------------------- gizmo

TEST(Gizmo, ResizeFromEdgeKeepsOppositeEdge)
{
	const wb::SelectionFrame l_Frame{ .center = { 0.0, 0.0 }, .angle = 0.0, .half = { 1.0, 1.0 } };
	const wb::FrameTransform l_Result = wb::resizeFrame(l_Frame, wb::Handle::E, { 3.0, 0.0 }, {});
	expectNear(l_Result.transform.apply({ -1.0, 0.5 }), { -1.0, 0.5 });
	expectNear(l_Result.transform.apply({ 1.0, 0.5 }), { 3.0, 0.5 });
	expectNear(l_Result.frame.center, { 1.0, 0.0 });
	expectNear(l_Result.frame.half, { 2.0, 1.0 });
}

TEST(Gizmo, ResizeFromCenter)
{
	const wb::SelectionFrame l_Frame{ .center = { 10.0, 10.0 }, .angle = 0.0, .half = { 2.0, 1.0 } };
	const wb::FrameTransform l_Result = wb::resizeFrame(l_Frame, wb::Handle::SE, { 16.0, 13.0 }, { .keepAspect = false, .fromCenter = true });
	expectNear(l_Result.frame.center, { 10.0, 10.0 });
	expectNear(l_Result.frame.half, { 6.0, 3.0 });
}

TEST(Gizmo, CornerKeepAspectUsesLargerAxis)
{
	const wb::SelectionFrame l_Frame{ .center = { 0.0, 0.0 }, .angle = 0.0, .half = { 2.0, 1.0 } };
	// Pull the bottom-right corner to (6, 1): x doubles (k=2), y stays (k=1); uniform scale follows the larger factor
	const wb::FrameTransform l_Result = wb::resizeFrame(l_Frame, wb::Handle::SE, { 6.0, 1.0 }, { .keepAspect = true, .fromCenter = false });
	expectNear(l_Result.frame.half, { 4.0, 2.0 });
	expectNear(l_Result.transform.apply({ -2.0, -1.0 }), { -2.0, -1.0 });
}

TEST(Gizmo, ResizeOfRotatedFrameScalesAlongItsAxes)
{
	// A frame turned 90 degrees: its local x axis points along world y
	const wb::SelectionFrame l_Frame{ .center = { 0.0, 0.0 }, .angle = wb::PI * 0.5, .half = { 2.0, 1.0 } };
	const DVec2 l_HandleWorld = l_Frame.handlePosition(wb::Handle::E);
	expectNear(l_HandleWorld, { 0.0, 2.0 });
	const wb::FrameTransform l_Result = wb::resizeFrame(l_Frame, wb::Handle::E, { 0.0, 6.0 }, {});
	expectNear(l_Result.frame.half, { 4.0, 1.0 });
	expectNear(l_Result.transform.apply({ 0.0, 2.0 }), { 0.0, 6.0 });
	expectNear(l_Result.transform.apply({ 0.0, -2.0 }), { 0.0, -2.0 });
	expectNear(l_Result.transform.applyVector({ 1.0, 0.0 }), { 1.0, 0.0 }); // the perpendicular direction is untouched
}

TEST(Gizmo, ResizeNeverCollapses)
{
	const wb::SelectionFrame l_Frame{ .center = { 0.0, 0.0 }, .angle = 0.0, .half = { 1.0, 1.0 } };
	const wb::FrameTransform l_Result = wb::resizeFrame(l_Frame, wb::Handle::E, { -50.0, 0.0 }, {});
	EXPECT_GT(l_Result.frame.half.x, 0.0);
	EXPECT_TRUE(std::isfinite(l_Result.transform.linear[0][0]));
}

TEST(Gizmo, RotateAndTranslate)
{
	const wb::SelectionFrame l_Frame{ .center = { 5.0, 5.0 }, .angle = 0.0, .half = { 2.0, 1.0 } };
	const wb::FrameTransform l_Rotated = wb::rotateFrame(l_Frame, wb::PI * 0.5);
	expectNear(l_Rotated.transform.apply({ 6.0, 5.0 }), { 5.0, 6.0 }, 1e-9);
	expectNear(l_Rotated.frame.center, { 5.0, 5.0 });
	EXPECT_NEAR(l_Rotated.frame.angle, wb::PI * 0.5, 1e-12);

	const wb::FrameTransform l_Moved = wb::translateFrame(l_Frame, { 3.0, -1.0 });
	expectNear(l_Moved.frame.center, { 8.0, 4.0 });
	expectNear(l_Moved.transform.apply({ 0.0, 0.0 }), { 3.0, -1.0 });
}

TEST(Gizmo, AngleHelpers)
{
	const double l_Step = wb::PI / 12.0; // 15 degrees
	EXPECT_NEAR(wb::snapAngle(0.30, l_Step), l_Step, 1e-12);
	EXPECT_NEAR(wb::snapAngle(0.10, l_Step), 0.0, 1e-12);
	EXPECT_NEAR(wb::wrapAngle(3.0 * wb::PI), wb::PI, 1e-9);
	EXPECT_NEAR(wb::wrapAngle(-wb::PI * 1.5), wb::PI * 0.5, 1e-9);
}

// ---------------------------------------------------------------------------------------------- commands and edits

TEST(Edit, TransformUndoRedoAndNudgeMerge)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_Id = addStroke(l_Doc, { 0.0, 0.0 });
	const std::array<wb::ObjectId, 1> l_Ids{ l_Id };

	for (int i = 0; i < 5; ++i)
		wb::transformObjects(l_Doc, l_History, l_Ids, wb::Affine2::translate({ 1.0, 0.0 }), "Nudge", true);
	expectNear(l_Doc.find(l_Id)->transform.translation, { 5.0, 0.0 });
	EXPECT_EQ(l_History.undoCount(), 1u) << "nudges merge into one undo step";

	l_History.undo(l_Doc);
	expectNear(l_Doc.find(l_Id)->transform.translation, { 0.0, 0.0 });
	l_History.redo(l_Doc);
	expectNear(l_Doc.find(l_Id)->transform.translation, { 5.0, 0.0 });

	// A different command in between ends the merge; a no-op transform records nothing
	wb::transformObjects(l_Doc, l_History, l_Ids, wb::Affine2{}, "Nudge", true);
	EXPECT_EQ(l_History.undoCount(), 1u);
	wb::transformObjects(l_Doc, l_History, l_Ids, wb::Affine2::translate({ 0.0, 2.0 }), "Move", false);
	EXPECT_EQ(l_History.undoCount(), 2u);
}

TEST(Edit, DeleteAndUndoKeepZOrder)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 20.0 });
	const wb::ObjectId l_C = addStroke(l_Doc, { 0.0, 40.0 });
	const wb::ObjectId l_D = addStroke(l_Doc, { 0.0, 60.0 });
	const std::array<wb::ObjectId, 2> l_Ids{ l_B, l_D };

	wb::deleteObjects(l_Doc, l_History, l_Ids);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_A, l_C }));
	l_History.undo(l_Doc);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_A, l_B, l_C, l_D }));
}

TEST(Edit, DuplicateOffsetsCopiesAndOnlyCopies)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 20.0 });
	const std::array<wb::ObjectId, 1> l_Ids{ l_A };

	const std::vector<wb::ObjectId> l_Copies = wb::duplicateObjects(l_Doc, l_History, l_Ids, { 7.0, 3.0 });
	ASSERT_EQ(l_Copies.size(), 1u);
	EXPECT_NE(l_Copies[0], l_A);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_A, l_B, l_Copies[0] })) << "copies go on top";
	expectNear(l_Doc.find(l_Copies[0])->transform.translation, { 7.0, 3.0 });
	expectNear(l_Doc.find(l_A)->transform.translation, { 0.0, 0.0 });

	// Editing the copy must not touch the original
	l_Doc.modify(l_Copies[0], [](wb::Object& p_Object) { p_Object.stroke()->points[0].radius = 9.f; });
	EXPECT_FLOAT_EQ(l_Doc.find(l_A)->stroke()->points[0].radius, 1.f);

	l_History.undo(l_Doc);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_A, l_B }));
}

TEST(Edit, CopyPasteCentersOnTarget)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 }, 10.f, 0.f);
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 10.0 }, 10.f, 0.f);
	const std::array<wb::ObjectId, 2> l_Ids{ l_A, l_B };

	const wb::ObjectClip l_Clip = wb::copyObjects(l_Doc, l_Ids);
	ASSERT_EQ(l_Clip.objects.size(), 2u);
	expectNear(l_Clip.bounds.center(), { 5.0, 5.0 });

	// The clip survives deleting the originals (cut) and can be pasted any number of times
	wb::deleteObjects(l_Doc, l_History, l_Ids);
	EXPECT_TRUE(l_Doc.empty());
	const std::vector<wb::ObjectId> l_First = wb::pasteObjects(l_Doc, l_History, l_Clip, { 100.0, 100.0 });
	const std::vector<wb::ObjectId> l_Second = wb::pasteObjects(l_Doc, l_History, l_Clip, { 200.0, 100.0 });
	ASSERT_EQ(l_First.size(), 2u);
	ASSERT_EQ(l_Second.size(), 2u);
	EXPECT_EQ(l_Doc.size(), 4u);
	expectNear(l_Doc.find(l_First[0])->transform.translation, { 95.0, 95.0 });
	expectNear(l_Doc.find(l_First[1])->transform.translation, { 95.0, 105.0 });
	EXPECT_LT(l_Doc.indexOf(l_First[0]), l_Doc.indexOf(l_First[1])) << "relative stacking is kept";
	EXPECT_EQ(std::count(l_First.begin(), l_First.end(), l_Second[0]), 0) << "every paste gets fresh ids";
}

TEST(Edit, ReorderMovesAndUndoes)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 20.0 });
	const wb::ObjectId l_C = addStroke(l_Doc, { 0.0, 40.0 });
	const wb::ObjectId l_D = addStroke(l_Doc, { 0.0, 60.0 });
	const std::vector<wb::ObjectId> l_Original = order(l_Doc);

	const auto l_Apply = [&](const std::vector<wb::ObjectId>& p_Ids, const wb::ZOrderMove p_Move) { wb::reorderObjects(l_Doc, l_History, p_Ids, p_Move); };

	l_Apply({ l_A, l_C }, wb::ZOrderMove::ToFront);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_B, l_D, l_A, l_C }));
	l_History.undo(l_Doc);
	EXPECT_EQ(order(l_Doc), l_Original);

	l_Apply({ l_B, l_D }, wb::ZOrderMove::ToBack);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_B, l_D, l_A, l_C }));
	l_History.undo(l_Doc);
	EXPECT_EQ(order(l_Doc), l_Original);

	l_Apply({ l_B }, wb::ZOrderMove::Forward);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_A, l_C, l_B, l_D }));
	l_History.undo(l_Doc);
	l_Apply({ l_C }, wb::ZOrderMove::Backward);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_A, l_C, l_B, l_D }));
	l_History.undo(l_Doc);
	EXPECT_EQ(order(l_Doc), l_Original);

	// Adjacent selected objects move together; the top object cannot go further forward
	l_Apply({ l_B, l_C }, wb::ZOrderMove::Forward);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_A, l_D, l_B, l_C }));
	l_History.undo(l_Doc);
	l_Apply({ l_D }, wb::ZOrderMove::Forward);
	EXPECT_EQ(order(l_Doc), l_Original);
	EXPECT_EQ(l_History.undoCount(), 0u) << "a move that changes nothing is not recorded";
}

TEST(Edit, RecolorKeepsAlphaAndUndoes)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 20.0 });
	l_Doc.modify(l_A, [](wb::Object& p_Object) { p_Object.stroke()->style.color = wb::Color{ 1.f, 0.f, 0.f, 0.5f }; });
	const std::array<wb::ObjectId, 2> l_Ids{ l_A, l_B };

	wb::recolorObjects(l_Doc, l_History, l_Ids, wb::Color{ 0.f, 1.f, 0.f, 1.f });
	EXPECT_EQ(l_Doc.find(l_A)->stroke()->style.color, (wb::Color{ 0.f, 1.f, 0.f, 0.5f }));
	EXPECT_EQ(l_Doc.find(l_B)->stroke()->style.color, (wb::Color{ 0.f, 1.f, 0.f, 1.f }));
	l_History.undo(l_Doc);
	EXPECT_EQ(l_Doc.find(l_A)->stroke()->style.color, (wb::Color{ 1.f, 0.f, 0.f, 0.5f }));
	EXPECT_EQ(l_Doc.find(l_B)->stroke()->style.color, wb::Color{});

	// Recolouring to the colour everything already has is not an edit
	wb::recolorObjects(l_Doc, l_History, std::array<wb::ObjectId, 1>{ l_B }, wb::Color{});
	EXPECT_EQ(l_History.undoCount(), 0u);
}

TEST(Document, SetOrderValidates)
{
	wb::Document l_Doc;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 0.0, 20.0 });
	const wb::ObjectId l_C = addStroke(l_Doc, { 0.0, 40.0 });

	l_Doc.setOrder(std::array<wb::ObjectId, 3>{ l_C, l_A, l_B });
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_C, l_A, l_B }));

	// Invalid permutations leave the document untouched
	l_Doc.setOrder(std::array<wb::ObjectId, 3>{ l_A, l_A, l_B });
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_C, l_A, l_B }));
	l_Doc.setOrder(std::array<wb::ObjectId, 3>{ l_A, l_B, 999 });
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_C, l_A, l_B }));
	l_Doc.setOrder(std::array<wb::ObjectId, 2>{ l_A, l_B });
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_C, l_A, l_B }));
}

// ---------------------------------------------------------------------------------------------- locking

TEST(Lock, LockedObjectsAreSkippedByEdits)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 100.0, 0.0 });
	const std::array<wb::ObjectId, 2> l_Both{ l_A, l_B };
	const std::array<wb::ObjectId, 1> l_OnlyA{ l_A };

	wb::lockObjects(l_Doc, l_History, l_OnlyA, true);
	EXPECT_TRUE(l_Doc.find(l_A)->locked);
	EXPECT_FALSE(l_Doc.find(l_B)->locked);

	wb::transformObjects(l_Doc, l_History, l_Both, wb::Affine2::translate({ 5.0, 0.0 }), "Move", false);
	expectNear(l_Doc.find(l_A)->transform.translation, { 0.0, 0.0 });
	expectNear(l_Doc.find(l_B)->transform.translation, { 105.0, 0.0 });

	wb::recolorObjects(l_Doc, l_History, l_Both, wb::Color{ 1.f, 0.f, 0.f, 1.f });
	EXPECT_NE(l_Doc.find(l_A)->stroke()->style.color.r, 1.f);
	EXPECT_EQ(l_Doc.find(l_B)->stroke()->style.color.r, 1.f);

	wb::deleteObjects(l_Doc, l_History, l_Both);
	EXPECT_NE(l_Doc.find(l_A), nullptr);
	EXPECT_EQ(l_Doc.find(l_B), nullptr);

	// Locking is an undo step of its own
	l_History.undo(l_Doc);
	l_History.undo(l_Doc);
	l_History.undo(l_Doc);
	l_History.undo(l_Doc);
	EXPECT_FALSE(l_Doc.find(l_A)->locked);
	l_History.redo(l_Doc);
	EXPECT_TRUE(l_Doc.find(l_A)->locked);

	wb::lockObjects(l_Doc, l_History, l_OnlyA, false);
	wb::transformObjects(l_Doc, l_History, l_OnlyA, wb::Affine2::translate({ 1.0, 0.0 }), "Move", false);
	expectNear(l_Doc.find(l_A)->transform.translation, { 1.0, 0.0 });
}

TEST(Lock, CopiesAreNotLocked)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const std::array<wb::ObjectId, 1> l_Ids{ l_A };
	wb::lockObjects(l_Doc, l_History, l_Ids, true);
	const std::vector<wb::ObjectId> l_Copies = wb::duplicateObjects(l_Doc, l_History, l_Ids, { 10.0, 10.0 });
	ASSERT_EQ(l_Copies.size(), 1u);
	EXPECT_FALSE(l_Doc.find(l_Copies[0])->locked);
	EXPECT_TRUE(l_Doc.find(l_A)->locked);
}

// ---------------------------------------------------------------------------------------------- alignment

TEST(Align, EdgesCentresAndSpacing)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 }, 10.f, 1.f);   // x from -1 to 11
	const wb::ObjectId l_B = addStroke(l_Doc, { 50.0, 30.0 }, 10.f, 1.f);  // x from 49 to 61
	const wb::ObjectId l_C = addStroke(l_Doc, { 200.0, -20.0 }, 10.f, 1.f); // x from 199 to 211
	const std::array<wb::ObjectId, 3> l_Ids{ l_A, l_B, l_C };

	wb::alignObjects(l_Doc, l_History, l_Ids, wb::AlignMode::Left);
	for (const wb::ObjectId l_Id : l_Ids)
		EXPECT_NEAR(wb::tightWorldBounds(*l_Doc.find(l_Id)).min.x, -1.0, 1e-9);
	EXPECT_NEAR(l_Doc.find(l_B)->transform.translation.y, 30.0, 1e-9); // only x moved

	l_History.undo(l_Doc);
	EXPECT_NEAR(l_Doc.find(l_C)->transform.translation.x, 200.0, 1e-9);

	wb::alignObjects(l_Doc, l_History, l_Ids, wb::AlignMode::Bottom);
	for (const wb::ObjectId l_Id : l_Ids)
		EXPECT_NEAR(wb::tightWorldBounds(*l_Doc.find(l_Id)).max.y, 31.0, 1e-9);
	l_History.undo(l_Doc);

	// The outer two stay, the middle one lands halfway between them: gaps of 94 on both sides
	wb::alignObjects(l_Doc, l_History, l_Ids, wb::AlignMode::DistributeX);
	const wb::Rect l_BoundsA = wb::tightWorldBounds(*l_Doc.find(l_A));
	const wb::Rect l_BoundsB = wb::tightWorldBounds(*l_Doc.find(l_B));
	const wb::Rect l_BoundsC = wb::tightWorldBounds(*l_Doc.find(l_C));
	EXPECT_NEAR(l_BoundsA.min.x, -1.0, 1e-9);
	EXPECT_NEAR(l_BoundsC.max.x, 211.0, 1e-9);
	EXPECT_NEAR(l_BoundsB.min.x - l_BoundsA.max.x, l_BoundsC.min.x - l_BoundsB.max.x, 1e-9);
}

TEST(Align, LockedObjectsStayPut)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_A = addStroke(l_Doc, { 0.0, 0.0 });
	const wb::ObjectId l_B = addStroke(l_Doc, { 100.0, 40.0 });
	const std::array<wb::ObjectId, 2> l_Ids{ l_A, l_B };
	const std::array<wb::ObjectId, 1> l_OnlyA{ l_A };
	wb::lockObjects(l_Doc, l_History, l_OnlyA, true);

	wb::alignObjects(l_Doc, l_History, l_Ids, wb::AlignMode::CenterY);
	EXPECT_NEAR(l_Doc.find(l_A)->transform.translation.y, 0.0, 1e-9);
	EXPECT_NEAR(l_Doc.find(l_B)->transform.translation.y, 20.0, 1e-9);
}
