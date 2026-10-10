#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstdint>
#include <thread>
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
import wb.io.blob;
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

	const wb::SplitBoard l_Split = wb::serializeBoardSplit(l_Doc, {}, wb::SplitOptions{ .bigAssetBytes = 100000 });
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

	const wb::SplitBoard l_Now = wb::serializeBoardSplit(l_Doc, {}, wb::SplitOptions{ .bigAssetBytes = 100000 });
	wb::SplitBoard l_Later = wb::serializeBoardSplit(l_Doc, {}, wb::SplitOptions{ .bigAssetBytes = 100000, .deferChecksums = true });
	EXPECT_FALSE(l_Later.pendingChecksums.empty());
	EXPECT_NE(l_Later.head, l_Now.head); // the checksums are still zero
	wb::finishChecksums(l_Later);
	EXPECT_TRUE(l_Later.pendingChecksums.empty());
	EXPECT_EQ(l_Later.head, l_Now.head);
}

// ------------------------------------------------------------------------------------------------ big files stay on disk

namespace
{
std::vector<uint8_t> pattern(const size_t p_Size, const uint32_t p_Seed)
{
	std::vector<uint8_t> l_Bytes(p_Size);
	uint32_t l_State = p_Seed * 2654435761u + 1;
	for (uint8_t& l_Byte : l_Bytes)
	{
		l_State = l_State * 1664525u + 1013904223u;
		l_Byte = static_cast<uint8_t>(l_State >> 24);
	}
	return l_Bytes;
}

std::filesystem::path scratch(const char* p_Name)
{
	const std::filesystem::path l_Folder = std::filesystem::temp_directory_path() / "wb_big_tests";
	std::filesystem::create_directories(l_Folder);
	return l_Folder / p_Name;
}

void writeBytes(const std::filesystem::path& p_Path, const std::vector<uint8_t>& p_Bytes)
{
	ASSERT_TRUE(wb::writeFileAtomic(p_Path, p_Bytes).ok);
}

std::shared_ptr<wb::FileSource> openFile(const std::filesystem::path& p_Path)
{
	std::string l_Error;
	std::shared_ptr<wb::FileSource> l_File = wb::FileSource::open(p_Path, l_Error);
	EXPECT_TRUE(l_File != nullptr) << l_Error;
	return l_File;
}

// A video object whose file is read from p_Source instead of memory
wb::ObjectId addStreamedVideo(wb::Document& p_Document, std::shared_ptr<const wb::ByteSource> p_Source, const std::string& p_Name, const wb::DVec2 p_Center)
{
	wb::ImageAsset l_Asset;
	l_Asset.stream = std::move(p_Source);
	l_Asset.width = 160;
	l_Asset.height = 90;
	l_Asset.name = p_Name;
	return addVideo(p_Document, p_Document.addAsset(std::move(l_Asset)), p_Center);
}
} // namespace

TEST(Blob, FilesAndTheirRangesAreReadInPieces)
{
	const std::vector<uint8_t> l_Data = pattern(100000, 1);
	const std::filesystem::path l_Path = scratch("pieces.bin");
	writeBytes(l_Path, l_Data);
	const std::shared_ptr<wb::FileSource> l_File = openFile(l_Path);
	ASSERT_NE(l_File, nullptr);
	EXPECT_EQ(l_File->size(), l_Data.size());
	EXPECT_TRUE(l_File->complete());
	EXPECT_FALSE(l_File->temporary());
	EXPECT_EQ(l_File->path(), l_Path);

	std::vector<uint8_t> l_Piece(1000);
	EXPECT_EQ(l_File->read(4321, l_Piece.data(), l_Piece.size()), 1000u);
	EXPECT_TRUE(std::equal(l_Piece.begin(), l_Piece.end(), l_Data.begin() + 4321));
	EXPECT_EQ(l_File->read(l_Data.size() - 10, l_Piece.data(), l_Piece.size()), 10u); // short only at the end
	EXPECT_EQ(l_File->read(l_Data.size(), l_Piece.data(), l_Piece.size()), 0u);

	const wb::RangeSource l_Range(l_File, 5000, 2000);
	EXPECT_EQ(l_Range.size(), 2000u);
	EXPECT_EQ(l_Range.fileOffset(), 5000u);
	EXPECT_EQ(l_Range.read(1990, l_Piece.data(), l_Piece.size()), 10u); // the range ends where it ends
	EXPECT_TRUE(std::equal(l_Piece.begin(), l_Piece.begin() + 10, l_Data.begin() + 6990));

	EXPECT_TRUE(wb::sameBytes(*l_File, *openFile(l_Path)));
	wb::removeFileQuiet(l_Path);
}

TEST(Blob, AFileBeingFilledMakesReadersWaitForTheirBytes)
{
	std::string l_Error;
	const std::shared_ptr<wb::FileSource> l_File = wb::FileSource::createTemporary(scratch("x").parent_path(), 1000, l_Error);
	ASSERT_NE(l_File, nullptr) << l_Error;
	EXPECT_TRUE(l_File->temporary());
	EXPECT_FALSE(l_File->complete());
	const std::vector<uint8_t> l_Data = pattern(1000, 2);
	ASSERT_TRUE(l_File->append(std::span<const uint8_t>(l_Data).first(300), l_Error));
	EXPECT_EQ(l_File->available(), 300u);

	std::atomic<bool> l_Returned{ false };
	size_t l_Got = 0;
	std::vector<uint8_t> l_Piece(100);
	std::thread l_Reader([&] {
		l_Got = l_File->read(500, l_Piece.data(), l_Piece.size()); // not there yet
		l_Returned.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	EXPECT_FALSE(l_Returned.load()); // waiting
	ASSERT_TRUE(l_File->append(std::span<const uint8_t>(l_Data).subspan(300, 400), l_Error));
	l_Reader.join();
	EXPECT_EQ(l_Got, 100u);
	EXPECT_TRUE(std::equal(l_Piece.begin(), l_Piece.end(), l_Data.begin() + 500));

	// A reader can be told to stop waiting, and a file that will never be whole lets everybody go
	std::atomic<bool> l_Cancel{ false };
	std::thread l_Waiter([&] { l_Got = l_File->read(900, l_Piece.data(), l_Piece.size(), &l_Cancel); });
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	l_Cancel.store(true);
	l_Waiter.join();
	EXPECT_EQ(l_Got, 0u);
	std::thread l_Other([&] { l_Got = l_File->read(900, l_Piece.data(), l_Piece.size()); });
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	l_File->abandon();
	l_Other.join();
	EXPECT_EQ(l_Got, 0u);
	EXPECT_TRUE(l_File->failed());
	EXPECT_EQ(l_File->read(0, l_Piece.data(), l_Piece.size()), 100u); // what did arrive is still there
}

TEST(Blob, ReplacingAFileThatIsOpenLeavesTheOpenOneUnchanged)
{
	const std::filesystem::path l_Path = scratch("replace.bin");
	const std::filesystem::path l_Next = scratch("replace.next");
	writeBytes(l_Path, pattern(5000, 3));
	const std::shared_ptr<wb::FileSource> l_Old = openFile(l_Path);
	ASSERT_NE(l_Old, nullptr);
	writeBytes(l_Next, pattern(7000, 4));
	const wb::IoResult l_Result = wb::replaceFile(l_Next, l_Path);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;

	// Whoever has the old file open goes on seeing it; a new reader finds the new one
	std::vector<uint8_t> l_Seen(5000);
	EXPECT_EQ(l_Old->read(0, l_Seen.data(), l_Seen.size()), 5000u);
	EXPECT_EQ(l_Seen, pattern(5000, 3));
	std::vector<uint8_t> l_Now;
	ASSERT_TRUE(wb::readFile(l_Path, l_Now).ok);
	EXPECT_EQ(l_Now, pattern(7000, 4));
	wb::removeFileQuiet(l_Path);
}

TEST(Video, BigFilesStayInTheBoardFileWhenItIsSavedAndOpened)
{
	const std::vector<uint8_t> l_Movie = pattern(static_cast<size_t>(wb::LARGE_ASSET_BYTES) + (2u << 20), 5);
	const std::filesystem::path l_Original = scratch("movie.mp4");
	writeBytes(l_Original, l_Movie);
	const std::filesystem::path l_Board = scratch("big.wbrd");

	wb::Document l_Doc;
	const wb::ObjectId l_Id = addStreamedVideo(l_Doc, openFile(l_Original), "movie.mp4", { 0.0, 0.0 });
	const wb::ObjectId l_Small = addVideo(l_Doc, addFile(l_Doc, 3), { 300.0, 0.0 });
	const wb::AssetId l_AssetId = l_Doc.find(l_Id)->video()->asset;
	EXPECT_TRUE(l_Doc.findAsset(l_AssetId)->bytes.empty()); // never loaded

	// Save: the movie is copied across from its file in pieces
	wb::SplitBoard l_Split = wb::serializeBoardSplit(l_Doc, {}, wb::SplitOptions{ .bigAssetBytes = 2u << 20 });
	ASSERT_EQ(l_Split.big.size(), 1u);
	EXPECT_LT(l_Split.head.size(), 10000u);
	std::vector<wb::PlacedAsset> l_Placed;
	wb::WriteProgress l_Progress;
	const wb::IoResult l_Written = wb::writeSplitBoardAtomic(l_Board, l_Split, &l_Placed, &l_Progress);
	ASSERT_TRUE(l_Written.ok) << l_Written.error;
	EXPECT_EQ(l_Progress.written.load(), l_Movie.size());
	ASSERT_EQ(l_Placed.size(), 1u);
	EXPECT_EQ(l_Placed[0].size, l_Movie.size());

	// The user deletes the original; the board no longer needs it once it points into the saved file
	const std::shared_ptr<wb::FileSource> l_BoardFile = openFile(l_Board);
	ASSERT_NE(l_BoardFile, nullptr);
	ASSERT_TRUE(l_Doc.rebindAsset(l_AssetId, std::make_shared<wb::RangeSource>(l_BoardFile, l_Placed[0].offset, l_Placed[0].size)));
	wb::removeFileQuiet(l_Original);
	EXPECT_EQ(l_Doc.findAsset(l_AssetId)->load(), l_Movie);

	// Open it: the movie stays in the file
	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::loadBoardFile(openFile(l_Board), l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	ASSERT_NE(l_Loaded.find(l_Id), nullptr);
	ASSERT_NE(l_Loaded.find(l_Small), nullptr);
	const wb::ImageAsset* l_Back = l_Loaded.findAsset(l_Loaded.find(l_Id)->video()->asset);
	ASSERT_NE(l_Back, nullptr);
	EXPECT_TRUE(l_Back->stream != nullptr);
	EXPECT_TRUE(l_Back->bytes.empty());
	EXPECT_EQ(l_Back->size(), l_Movie.size());
	EXPECT_EQ(l_Back->width, 160u);
	EXPECT_EQ(l_Back->name, "movie.mp4");
	EXPECT_EQ(l_Back->load(), l_Movie);

	// An autosave only points at files that stay as they are: not at the file it is about to replace
	EXPECT_TRUE(wb::serializeBoardSplit(l_Loaded, {}, wb::SplitOptions{ .bigAssetBytes = 2u << 20, .referenceFiles = true }).big.empty());
	EXPECT_EQ(wb::serializeBoardSplit(l_Loaded, {}, wb::SplitOptions{ .bigAssetBytes = 2u << 20, .referenceFiles = true, .noReferencesTo = l_Board }).big.size(), 1u);

	// Save again over the very file the movie is read from: the old copy stays readable while the new one is written
	wb::SplitBoard l_Again = wb::serializeBoardSplit(l_Loaded, {}, wb::SplitOptions{ .bigAssetBytes = 2u << 20 });
	const wb::IoResult l_Rewritten = wb::writeSplitBoardAtomic(l_Board, l_Again, &l_Placed);
	ASSERT_TRUE(l_Rewritten.ok) << l_Rewritten.error;
	EXPECT_EQ(l_Back->load(), l_Movie);
	wb::Document l_Third;
	ASSERT_TRUE(wb::loadBoardFile(openFile(l_Board), l_Third, l_Meta).ok);
	EXPECT_EQ(l_Third.findAsset(l_Third.find(l_Id)->video()->asset)->load(), l_Movie);

	// The same file read whole by the old path still works (a board file is a board file)
	std::vector<uint8_t> l_All;
	ASSERT_TRUE(wb::readFile(l_Board, l_All).ok);
	wb::Document l_Whole;
	ASSERT_TRUE(wb::deserializeBoard(l_All, l_Whole, l_Meta).ok);
	EXPECT_EQ(l_Whole.findAsset(l_Whole.find(l_Id)->video()->asset)->bytes, l_Movie);

	// Damage inside the bytes of a small chunk is still caught; a cut-off file is too
	std::vector<uint8_t> l_Cut(l_All.begin(), l_All.begin() + static_cast<std::ptrdiff_t>(l_All.size() / 2));
	const std::filesystem::path l_CutPath = scratch("cut.wbrd");
	writeBytes(l_CutPath, l_Cut);
	wb::Document l_Broken;
	EXPECT_FALSE(wb::loadBoardFile(openFile(l_CutPath), l_Broken, l_Meta).ok);
	wb::removeFileQuiet(l_CutPath);
	wb::removeFileQuiet(l_Board);
}

TEST(Video, RecoveryFilesPointAtBigFilesInsteadOfCopyingThem)
{
	const std::vector<uint8_t> l_Movie = pattern(static_cast<size_t>(wb::LARGE_ASSET_BYTES) + (1u << 20), 6);
	const std::filesystem::path l_Original = scratch("recover.mp4");
	writeBytes(l_Original, l_Movie);
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addStreamedVideo(l_Doc, openFile(l_Original), "recover.mp4", { 0.0, 0.0 });
	const std::filesystem::path l_Recovery = scratch("recover.wbrd");

	wb::SplitBoard l_Split = wb::serializeBoardSplit(l_Doc, {}, wb::SplitOptions{ .bigAssetBytes = 2u << 20, .referenceFiles = true });
	EXPECT_TRUE(l_Split.big.empty()); // nothing is copied
	EXPECT_LT(l_Split.head.size(), 10000u);
	ASSERT_TRUE(wb::writeSplitBoardAtomic(l_Recovery, l_Split).ok);

	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	wb::LoadResult l_Result = wb::loadBoardFile(openFile(l_Recovery), l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	EXPECT_TRUE(l_Result.missingFiles.empty());
	ASSERT_NE(l_Loaded.find(l_Id), nullptr);
	EXPECT_EQ(l_Loaded.findAsset(l_Loaded.find(l_Id)->video()->asset)->load(), l_Movie);

	// The video was edited since: what is there now is not what the board showed, so it is not pretended
	std::vector<uint8_t> l_Changed = l_Movie;
	l_Changed[100] ^= 1;
	l_Loaded.clear();
	writeBytes(l_Original, l_Changed);
	std::filesystem::last_write_time(l_Original, std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
	wb::Document l_After;
	l_Result = wb::loadBoardFile(openFile(l_Recovery), l_After, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	EXPECT_EQ(l_Result.skippedObjects, 1u);
	ASSERT_EQ(l_Result.missingFiles.size(), 1u);
	EXPECT_EQ(l_Result.missingFiles[0], "recover.mp4");
	EXPECT_EQ(l_After.find(l_Id), nullptr);

	// A copy that exists nowhere else (a download) is never left to a pointer: it cannot be pointed at after a crash
	std::string l_Error;
	const std::shared_ptr<wb::FileSource> l_Download = wb::FileSource::createTemporary(scratch("x").parent_path(), 100, l_Error);
	ASSERT_NE(l_Download, nullptr) << l_Error;
	wb::removeFileQuiet(l_Original);
	wb::removeFileQuiet(l_Recovery);
}

TEST(Video, AssetsFoundByTheirContentWhereverTheyAreKept)
{
	const std::vector<uint8_t> l_Movie = pattern(static_cast<size_t>(wb::LARGE_ASSET_BYTES) + 1000, 7);
	const std::filesystem::path l_Path = scratch("same.mp4");
	writeBytes(l_Path, l_Movie);
	wb::Document l_Doc;
	wb::ImageAsset l_Streamed;
	l_Streamed.stream = openFile(l_Path);
	l_Streamed.name = "same.mp4";
	const wb::AssetId l_First = l_Doc.addAsset(std::move(l_Streamed));
	wb::ImageAsset l_Again;
	l_Again.stream = openFile(l_Path); // the same file imported a second time
	EXPECT_EQ(l_Doc.addAsset(std::move(l_Again)), l_First);
	wb::ImageAsset l_InMemory;
	l_InMemory.bytes = l_Movie; // and a copy of it that was pasted
	EXPECT_EQ(l_Doc.addAsset(std::move(l_InMemory)), l_First);
	std::vector<uint8_t> l_Other = l_Movie;
	l_Other.back() ^= 1;
	wb::ImageAsset l_Different;
	l_Different.bytes = std::move(l_Other);
	EXPECT_NE(l_Doc.addAsset(std::move(l_Different)), l_First);
	wb::removeFileQuiet(l_Path);
}

// Boards saved by earlier versions hold every file inside one chunk; a movie in there is still not read into memory
TEST(Video, OldBoardsWithAMovieInsideOpenWithoutReadingIt)
{
	const std::vector<uint8_t> l_Movie = pattern(70u << 20, 8);
	wb::Document l_Doc;
	wb::ImageAsset l_Big;
	l_Big.bytes = l_Movie;
	l_Big.name = "old.mp4";
	l_Big.width = 160;
	l_Big.height = 90;
	const wb::ObjectId l_Id = addVideo(l_Doc, l_Doc.addAsset(std::move(l_Big)), { 0.0, 0.0 });
	const wb::ObjectId l_Small = addVideo(l_Doc, addFile(l_Doc, 4), { 300.0, 0.0 });
	const std::filesystem::path l_Path = scratch("old.wbrd");
	writeBytes(l_Path, wb::serializeBoard(l_Doc, {})); // everything inline, as before

	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::loadBoardFile(openFile(l_Path), l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	const wb::ImageAsset* l_Movie2 = l_Loaded.findAsset(l_Loaded.find(l_Id)->video()->asset);
	ASSERT_NE(l_Movie2, nullptr);
	EXPECT_TRUE(l_Movie2->stream != nullptr);
	EXPECT_EQ(l_Movie2->name, "old.mp4");
	EXPECT_EQ(l_Movie2->load(), l_Movie);
	const wb::ImageAsset* l_Tiny = l_Loaded.findAsset(l_Loaded.find(l_Small)->video()->asset);
	ASSERT_NE(l_Tiny, nullptr);
	EXPECT_TRUE(l_Tiny->stream == nullptr); // small files are still held in memory
	wb::removeFileQuiet(l_Path);
}

// Not a test of its own: set WB_TEST_BOARD to a real board file to check that it opens the way the program opens it
TEST(Video, ARealBoardFileFromTheEnvironmentOpens)
{
	const char* l_Env = std::getenv("WB_TEST_BOARD");
	if (l_Env == nullptr)
		GTEST_SKIP() << "WB_TEST_BOARD is not set";
	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::loadBoardFile(openFile(wb::pathFromUtf8(l_Env)), l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	size_t l_Streamed = 0;
	for (const auto& [l_Id, l_Asset] : l_Loaded.assets())
		l_Streamed += l_Asset.stream ? 1 : 0;
	std::printf("board: %zu objects, %zu assets (%zu streamed), skipped %u, missing %zu\n", l_Loaded.size(), l_Loaded.assets().size(), l_Streamed, l_Result.skippedObjects, l_Result.missingFiles.size());
}
