#include <filesystem>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.hit;
import wb.doc.edit;
import wb.io.file;
import wb.io.serializer;

namespace
{
wb::ObjectId addVideo(wb::Document& p_Document, const wb::AssetId p_Asset, const wb::DVec2 p_Center)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate(p_Center);
	l_Object->payload = wb::VideoData{ .asset = p_Asset, .size = { 160.f, 90.f } };
	const wb::ObjectId l_Id = l_Object->id;
	p_Document.insert(std::move(l_Object));
	return l_Id;
}

wb::AssetId addFile(wb::Document& p_Document, const uint8_t p_Fill)
{
	wb::ImageAsset l_Asset;
	l_Asset.bytes.assign(64, p_Fill);
	l_Asset.width = 160;
	l_Asset.height = 90;
	l_Asset.name = "clip.mp4";
	return p_Document.addAsset(std::move(l_Asset));
}
} // namespace

TEST(Video, HitTestsLikeABox)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addVideo(l_Doc, addFile(l_Doc, 1), { 0.0, 0.0 });
	EXPECT_TRUE(wb::hitsPoint(*l_Doc.find(l_Id), { 70.0, 30.0 }, 0.0));
	EXPECT_FALSE(wb::hitsPoint(*l_Doc.find(l_Id), { 90.0, 0.0 }, 0.0));
}

TEST(Video, SurvivesSavingAndLoading)
{
	wb::Document l_Doc;
	const wb::AssetId l_Asset = addFile(l_Doc, 9);
	const wb::ObjectId l_Id = addVideo(l_Doc, l_Asset, { 5.0, 6.0 });
	l_Doc.modify(l_Id, [](wb::Object& p_Object) { p_Object.video()->loop = true; p_Object.video()->muted = false; });
	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Doc, {});

	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	ASSERT_TRUE(wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta).ok);
	const wb::Object* l_Back = l_Loaded.find(l_Id);
	ASSERT_NE(l_Back, nullptr);
	ASSERT_NE(l_Back->video(), nullptr);
	EXPECT_TRUE(l_Back->video()->loop);
	EXPECT_FALSE(l_Back->video()->muted);
	const wb::ImageAsset* l_File = l_Loaded.findAsset(l_Back->video()->asset);
	ASSERT_NE(l_File, nullptr);
	EXPECT_EQ(l_File->bytes, l_Doc.findAsset(l_Asset)->bytes);
}

TEST(Video, CopyAndPasteCarryTheFile)
{
	wb::Document l_Source;
	wb::History l_History;
	const wb::ObjectId l_Id = addVideo(l_Source, addFile(l_Source, 3), { 0.0, 0.0 });
	const wb::ObjectId l_Ids[]{ l_Id };
	const wb::ObjectClip l_Clip = wb::copyObjects(l_Source, l_Ids);
	ASSERT_EQ(l_Clip.assets.size(), 1u);

	wb::Document l_Target;
	addFile(l_Target, 1); // a different asset numbering than the source
	const std::vector<wb::ObjectId> l_Pasted = wb::pasteObjects(l_Target, l_History, l_Clip, { 100.0, 100.0 }, "Paste");
	ASSERT_EQ(l_Pasted.size(), 1u);
	const wb::VideoData* l_Data = l_Target.find(l_Pasted[0])->video();
	ASSERT_NE(l_Data, nullptr);
	const wb::ImageAsset* l_File = l_Target.findAsset(l_Data->asset);
	ASSERT_NE(l_File, nullptr);
	EXPECT_EQ(l_File->bytes, l_Source.findAsset(l_Source.find(l_Id)->video()->asset)->bytes);
}

TEST(Video, EditsAreUndoable)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_Id = addVideo(l_Doc, addFile(l_Doc, 1), { 0.0, 0.0 });
	const wb::ObjectId l_Ids[]{ l_Id };
	wb::editVideos(l_Doc, l_History, l_Ids, "Loop", [](wb::VideoData& p_Data) { p_Data.loop = true; return true; });
	EXPECT_TRUE(l_Doc.find(l_Id)->video()->loop);
	l_History.undo(l_Doc);
	EXPECT_FALSE(l_Doc.find(l_Id)->video()->loop);
}

TEST(Video, BigFilesAreWrittenFromTheDocumentWithoutCopying)
{
	wb::Document l_Doc;
	wb::ImageAsset l_Big;
	l_Big.bytes.assign(300000, 7);
	l_Big.bytes[12345] = 99;
	l_Big.width = 16;
	l_Big.height = 9;
	const wb::AssetId l_BigId = l_Doc.addAsset(std::move(l_Big));
	const wb::ObjectId l_Id = addVideo(l_Doc, l_BigId, { 0.0, 0.0 });
	const wb::ObjectId l_Small = addVideo(l_Doc, addFile(l_Doc, 5), { 300.0, 0.0 });

	const wb::SplitBoard l_Split = wb::serializeBoardSplit(l_Doc, {}, 100000);
	ASSERT_EQ(l_Split.big.size(), 1u);
	EXPECT_EQ(l_Split.big[0].data, l_Doc.findAsset(l_BigId)->bytes.data()); // not copied
	EXPECT_LT(l_Split.head.size(), 10000u);

	const std::filesystem::path l_Path = std::filesystem::temp_directory_path() / "wb_split_test.wbrd";
	ASSERT_TRUE(wb::writeSplitBoardAtomic(l_Path, l_Split).ok);
	std::vector<uint8_t> l_Bytes;
	ASSERT_TRUE(wb::readFile(l_Path, l_Bytes).ok);
	wb::removeFileQuiet(l_Path);

	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	ASSERT_TRUE(wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta).ok);
	ASSERT_NE(l_Loaded.find(l_Id), nullptr);
	ASSERT_NE(l_Loaded.find(l_Small), nullptr);
	EXPECT_EQ(l_Loaded.findAsset(l_Loaded.find(l_Id)->video()->asset)->bytes, l_Doc.findAsset(l_BigId)->bytes);

	// A flipped bit inside the big file is caught by its checksum
	l_Bytes[l_Bytes.size() / 2] ^= 1;
	wb::Document l_Damaged;
	EXPECT_FALSE(wb::deserializeBoard(l_Bytes, l_Damaged, l_Meta).ok);
}

TEST(Video, DeferredChecksumsMatchTheImmediateOnes)
{
	wb::Document l_Doc;
	addVideo(l_Doc, addFile(l_Doc, 5), { 0.0, 0.0 });
	addVideo(l_Doc, addFile(l_Doc, 9), { 300.0, 0.0 });

	const wb::SplitBoard l_Now = wb::serializeBoardSplit(l_Doc, {}, 100000);
	wb::SplitBoard l_Later = wb::serializeBoardSplit(l_Doc, {}, 100000, true);
	EXPECT_FALSE(l_Later.pendingChecksums.empty());
	EXPECT_NE(l_Later.head, l_Now.head); // the checksums are still zero
	wb::finishChecksums(l_Later);
	EXPECT_TRUE(l_Later.pendingChecksums.empty());
	EXPECT_EQ(l_Later.head, l_Now.head);
}
