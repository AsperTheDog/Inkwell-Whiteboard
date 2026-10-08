#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
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
import wb.doc.gizmo;
import wb.doc.edit;
import wb.io.serializer;

namespace
{
using wb::DVec2;

wb::ImageAsset makeAsset(const uint8_t p_Fill, const size_t p_Size = 16)
{
	wb::ImageAsset l_Asset;
	l_Asset.bytes.assign(p_Size, p_Fill);
	l_Asset.width = 4;
	l_Asset.height = 4;
	l_Asset.name = "test.png";
	return l_Asset;
}

wb::ObjectId addImage(wb::Document& p_Document, const wb::AssetId p_Asset, const DVec2 p_Center, const wb::Vec2 p_Size = { 100.f, 50.f })
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate(p_Center);
	l_Object->payload = wb::ImageData{ .asset = p_Asset, .size = p_Size };
	const wb::ObjectId l_Id = l_Object->id;
	p_Document.insert(std::move(l_Object));
	return l_Id;
}

wb::ObjectId addStroke(wb::Document& p_Document, const DVec2 p_Origin)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate(p_Origin);
	l_Object->payload = wb::StrokeData{ .style = {}, .points = { { .position = { 0.f, 0.f }, .radius = 1.f }, { .position = { 10.f, 0.f }, .radius = 1.f } } };
	const wb::ObjectId l_Id = l_Object->id;
	p_Document.insert(std::move(l_Object));
	return l_Id;
}
} // namespace

TEST(Assets, IdenticalBytesShareOneAsset)
{
	wb::Document l_Doc;
	const wb::AssetId l_A = l_Doc.addAsset(makeAsset(1));
	const wb::AssetId l_B = l_Doc.addAsset(makeAsset(1));
	const wb::AssetId l_C = l_Doc.addAsset(makeAsset(2));
	EXPECT_EQ(l_A, l_B);
	EXPECT_NE(l_A, l_C);
	EXPECT_EQ(l_Doc.assets().size(), 2u);
	ASSERT_NE(l_Doc.findAsset(l_C), nullptr);
	EXPECT_EQ(l_Doc.findAsset(l_C)->id, l_C);
	EXPECT_EQ(l_Doc.findAsset(9999), nullptr);
}

TEST(ImageHit, RotatedImageIsHitInsideItsRectangleOnly)
{
	wb::Document l_Doc;
	const wb::AssetId l_Asset = l_Doc.addAsset(makeAsset(1));
	const wb::ObjectId l_Id = addImage(l_Doc, l_Asset, { 0.0, 0.0 }, { 100.f, 20.f });
	l_Doc.modify(l_Id, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::rotate(wb::PI * 0.5); });
	const wb::Object& l_Object = *l_Doc.find(l_Id);
	// Rotated a quarter turn: 20 wide, 100 tall
	EXPECT_TRUE(wb::hitsPoint(l_Object, { 0.0, 45.0 }, 0.0));
	EXPECT_FALSE(wb::hitsPoint(l_Object, { 45.0, 0.0 }, 0.0));
	EXPECT_TRUE(wb::hitsPoint(l_Object, { 12.0, 0.0 }, 3.0)) << "tolerance widens the target";
	EXPECT_EQ(wb::pickTopmost(l_Doc, { 0.0, -40.0 }, 0.0), l_Id);
}

TEST(ImageHit, LassoTouchesImageByEdgeOrContainment)
{
	wb::Document l_Doc;
	const wb::AssetId l_Asset = l_Doc.addAsset(makeAsset(1));
	const wb::ObjectId l_Id = addImage(l_Doc, l_Asset, { 0.0, 0.0 }, { 100.f, 100.f });
	const wb::Object& l_Object = *l_Doc.find(l_Id);

	// Tiny area inside the picture: no vertex of the picture, no crossing edge
	const std::array<DVec2, 4> l_Inside{ DVec2{ -2.0, -2.0 }, DVec2{ 2.0, -2.0 }, DVec2{ 2.0, 2.0 }, DVec2{ -2.0, 2.0 } };
	EXPECT_TRUE(wb::touchesPolygon(l_Object, l_Inside));
	// Fully around it
	const std::array<DVec2, 4> l_Around{ DVec2{ -200.0, -200.0 }, DVec2{ 200.0, -200.0 }, DVec2{ 200.0, 200.0 }, DVec2{ -200.0, 200.0 } };
	EXPECT_TRUE(wb::touchesPolygon(l_Object, l_Around));
	// Crossing one edge
	const std::array<DVec2, 4> l_Crossing{ DVec2{ 40.0, -10.0 }, DVec2{ 80.0, -10.0 }, DVec2{ 80.0, 10.0 }, DVec2{ 40.0, 10.0 } };
	EXPECT_TRUE(wb::touchesPolygon(l_Object, l_Crossing));
	const std::array<DVec2, 4> l_Away{ DVec2{ 60.0, -10.0 }, DVec2{ 80.0, -10.0 }, DVec2{ 80.0, 10.0 }, DVec2{ 60.0, 10.0 } };
	EXPECT_FALSE(wb::touchesPolygon(l_Object, l_Away));
}

TEST(ImageIo, RoundTripKeepsImagesAndOnlyUsedAssets)
{
	wb::Document l_Doc;
	const wb::AssetId l_Used = l_Doc.addAsset(makeAsset(7, 1000));
	l_Doc.addAsset(makeAsset(9, 5000)); // never used by an object: must not be saved
	addStroke(l_Doc, { 5.0, 5.0 });
	const wb::ObjectId l_Image = addImage(l_Doc, l_Used, { 20.0, 30.0 }, { 64.f, 32.f });
	l_Doc.modify(l_Image, [](wb::Object& p_Object)
	{
		p_Object.image()->playing = false;
		p_Object.image()->frame = 3;
		p_Object.transform = wb::Affine2::around({ 20.0, 30.0 }, wb::Affine2::scale({ -2.0, 1.5 })) * p_Object.transform;
	});

	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Doc, wb::BoardMeta{});
	EXPECT_LT(l_Bytes.size(), 3000u) << "the unused asset was saved";

	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	ASSERT_EQ(l_Loaded.size(), 2u);
	const wb::Object* l_Back = l_Loaded.find(l_Image);
	ASSERT_NE(l_Back, nullptr);
	ASSERT_NE(l_Back->image(), nullptr);
	EXPECT_FALSE(l_Back->image()->playing);
	EXPECT_EQ(l_Back->image()->frame, 3u);
	EXPECT_FLOAT_EQ(l_Back->image()->size.x, 64.f);
	EXPECT_DOUBLE_EQ(l_Back->transform.linear[0][0], -2.0);
	const wb::ImageAsset* l_Asset = l_Loaded.findAsset(l_Back->image()->asset);
	ASSERT_NE(l_Asset, nullptr);
	EXPECT_EQ(l_Asset->bytes, l_Doc.findAsset(l_Used)->bytes);
	EXPECT_EQ(l_Asset->name, "test.png");
	EXPECT_EQ(l_Loaded.assets().size(), 1u);
}

TEST(ImageIo, TruncatedFilesWithImagesAreRejected)
{
	wb::Document l_Doc;
	const wb::AssetId l_Asset = l_Doc.addAsset(makeAsset(7, 400));
	addImage(l_Doc, l_Asset, { 0.0, 0.0 });
	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Doc, wb::BoardMeta{});
	for (size_t l_Cut = 0; l_Cut < l_Bytes.size(); l_Cut += 7)
	{
		wb::Document l_Target;
		wb::BoardMeta l_Meta;
		const std::vector<uint8_t> l_Short(l_Bytes.begin(), l_Bytes.begin() + static_cast<std::ptrdiff_t>(l_Cut));
		EXPECT_FALSE(wb::deserializeBoard(l_Short, l_Target, l_Meta).ok) << "cut at " << l_Cut;
	}
}

TEST(ImageEdit, FlipMirrorsAboutTheSelectionCentreAndUndoes)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::AssetId l_Asset = l_Doc.addAsset(makeAsset(1));
	const wb::ObjectId l_Image = addImage(l_Doc, l_Asset, { 100.0, 0.0 }, { 20.f, 20.f });
	const wb::ObjectId l_Stroke = addStroke(l_Doc, { 0.0, 0.0 });
	const std::array<wb::ObjectId, 2> l_Ids{ l_Image, l_Stroke };

	wb::flipObjects(l_Doc, l_History, l_Ids, true);
	// The group spans x = -1 .. 110, so its centre is x = 54.5 and the image moves to 54.5 * 2 - 100
	EXPECT_NEAR(l_Doc.find(l_Image)->transform.translation.x, 9.0, 1e-9);
	EXPECT_LT(l_Doc.find(l_Image)->transform.linear[0][0], 0.0);
	EXPECT_NEAR(l_Doc.find(l_Stroke)->transform.translation.y, 0.0, 1e-9);

	l_History.undo(l_Doc);
	EXPECT_NEAR(l_Doc.find(l_Image)->transform.translation.x, 100.0, 1e-9);
	EXPECT_GT(l_Doc.find(l_Image)->transform.linear[0][0], 0.0);
}

TEST(ImageEdit, PlaybackEditsAreUndoable)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::AssetId l_Asset = l_Doc.addAsset(makeAsset(1));
	const wb::ObjectId l_Image = addImage(l_Doc, l_Asset, { 0.0, 0.0 });
	const std::array<wb::ObjectId, 1> l_Ids{ l_Image };

	wb::editImages(l_Doc, l_History, l_Ids, "Pause", [](wb::ImageData& p_Data) { p_Data.playing = false; p_Data.frame = 5; return true; });
	EXPECT_FALSE(l_Doc.find(l_Image)->image()->playing);
	EXPECT_EQ(l_Doc.find(l_Image)->image()->frame, 5u);
	// An edit that changes nothing adds no undo step
	const size_t l_Steps = l_History.undoCount();
	wb::editImages(l_Doc, l_History, l_Ids, "Pause", [](wb::ImageData& p_Data) { p_Data.playing = false; return true; });
	EXPECT_EQ(l_History.undoCount(), l_Steps);

	l_History.undo(l_Doc);
	EXPECT_TRUE(l_Doc.find(l_Image)->image()->playing);
	EXPECT_EQ(l_Doc.find(l_Image)->image()->frame, 0u);
}

TEST(ImageEdit, PasteIntoAnotherBoardBringsTheAssetAlong)
{
	wb::Document l_Source;
	wb::History l_SourceHistory;
	const wb::AssetId l_Asset = l_Source.addAsset(makeAsset(4, 64));
	const wb::ObjectId l_Image = addImage(l_Source, l_Asset, { 0.0, 0.0 });
	const std::array<wb::ObjectId, 1> l_Ids{ l_Image };
	const wb::ObjectClip l_Clip = wb::copyObjects(l_Source, l_Ids);
	ASSERT_EQ(l_Clip.assets.size(), 1u);

	wb::Document l_Target;
	wb::History l_TargetHistory;
	l_Target.addAsset(makeAsset(1)); // different asset id numbering than the source
	const std::vector<wb::ObjectId> l_Pasted = wb::pasteObjects(l_Target, l_TargetHistory, l_Clip, { 10.0, 10.0 });
	ASSERT_EQ(l_Pasted.size(), 1u);
	const wb::ImageData* l_Data = l_Target.find(l_Pasted[0])->image();
	ASSERT_NE(l_Data, nullptr);
	const wb::ImageAsset* l_Found = l_Target.findAsset(l_Data->asset);
	ASSERT_NE(l_Found, nullptr);
	EXPECT_EQ(l_Found->bytes, l_Source.findAsset(l_Asset)->bytes);
}

TEST(GizmoFlip, DraggingAHandlePastTheOppositeEdgeMirrors)
{
	const wb::SelectionFrame l_Frame{ .center = { 0.0, 0.0 }, .angle = 0.0, .half = { 50.0, 25.0 } };
	// Drag the east handle to the far side of the west edge
	const wb::FrameTransform l_Result = wb::resizeFrame(l_Frame, wb::Handle::E, { -100.0, 0.0 }, {});
	EXPECT_LT(l_Result.transform.linear[0][0], 0.0) << "x is mirrored";
	EXPECT_GT(l_Result.transform.linear[1][1], 0.0);
	EXPECT_GT(l_Result.frame.half.x, 0.0) << "the frame keeps a positive size";
	// The west edge stays where it was
	EXPECT_NEAR(l_Result.transform.apply({ -50.0, 0.0 }).x, -50.0, 1e-9);

	// Corner with the aspect locked flips both axes independently by the pointer's side
	const wb::FrameTransform l_Corner = wb::resizeFrame(l_Frame, wb::Handle::SE, { -80.0, 60.0 }, wb::ResizeOptions{ .keepAspect = true });
	EXPECT_LT(l_Corner.transform.linear[0][0], 0.0);
	EXPECT_GT(l_Corner.transform.linear[1][1], 0.0);
	EXPECT_NEAR(std::abs(l_Corner.transform.linear[0][0]), std::abs(l_Corner.transform.linear[1][1]), 1e-9);
}
