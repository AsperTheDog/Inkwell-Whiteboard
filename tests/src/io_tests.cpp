#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.document;
import wb.doc.object;
import wb.io.file;
import wb.io.serializer;
import wb.io.settings;

namespace
{
std::unique_ptr<wb::Object> makeStroke(wb::Document& p_Document, const int p_Seed, const size_t p_Points = 20)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate({ 1000.0 * p_Seed, -3.5 * p_Seed }) * wb::Affine2::rotate(0.1 * p_Seed) * wb::Affine2::scale({ 1.5, 0.75 });
	wb::StrokeData l_Stroke;
	l_Stroke.style.color = { 0.1f * static_cast<float>(p_Seed % 10), 0.5f, 0.25f, 0.8f };
	l_Stroke.style.size = 3.5f + static_cast<float>(p_Seed);
	for (size_t i = 0; i < p_Points; ++i)
		l_Stroke.points.push_back({ .position = { static_cast<float>(i) * 1.25f, static_cast<float>(p_Seed) + std::sin(static_cast<float>(i)) }, .radius = 0.5f + 0.01f * static_cast<float>(i) });
	l_Object->payload = std::move(l_Stroke);
	return l_Object;
}

void fill(wb::Document& p_Document, const int p_Count)
{
	for (int i = 1; i <= p_Count; ++i)
		p_Document.insert(makeStroke(p_Document, i));
}

// Appends a chunk (tag, length, crc, payload) in the file's layout
void appendChunk(std::vector<uint8_t>& p_Bytes, const char (&p_Tag)[5], const std::vector<uint8_t>& p_Payload)
{
	for (int i = 0; i < 4; ++i)
		p_Bytes.push_back(static_cast<uint8_t>(p_Tag[i]));
	const uint64_t l_Length = p_Payload.size();
	const uint32_t l_Crc = wb::crc32(p_Payload);
	const auto* l_LengthBytes = reinterpret_cast<const uint8_t*>(&l_Length);
	p_Bytes.insert(p_Bytes.end(), l_LengthBytes, l_LengthBytes + 8);
	const auto* l_CrcBytes = reinterpret_cast<const uint8_t*>(&l_Crc);
	p_Bytes.insert(p_Bytes.end(), l_CrcBytes, l_CrcBytes + 4);
	p_Bytes.insert(p_Bytes.end(), p_Payload.begin(), p_Payload.end());
}
} // namespace

TEST(Serializer, RoundTripPreservesEverything)
{
	wb::Document l_Source;
	fill(l_Source, 5);
	wb::BoardMeta l_Meta{ .viewCenter = { 123456.789, -42.5 }, .viewZoom = 3.25, .sourcePath = "C:/boards/demo.wbrd" };
	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Source, l_Meta);

	wb::Document l_Loaded;
	wb::BoardMeta l_LoadedMeta;
	const wb::LoadResult l_Result = wb::deserializeBoard(l_Bytes, l_Loaded, l_LoadedMeta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	EXPECT_EQ(l_Result.skippedObjects, 0u);
	EXPECT_EQ(l_LoadedMeta.viewCenter, l_Meta.viewCenter);
	EXPECT_EQ(l_LoadedMeta.viewZoom, l_Meta.viewZoom);
	EXPECT_EQ(l_LoadedMeta.sourcePath, l_Meta.sourcePath);

	ASSERT_EQ(l_Loaded.size(), l_Source.size());
	for (size_t i = 0; i < l_Source.size(); ++i)
	{
		const wb::Object& l_A = *l_Source.objects()[i];
		const wb::Object& l_B = *l_Loaded.objects()[i];
		EXPECT_EQ(l_A.id, l_B.id);
		EXPECT_EQ(l_A.transform.linear, l_B.transform.linear);
		EXPECT_EQ(l_A.transform.translation, l_B.transform.translation);
		ASSERT_NE(l_B.stroke(), nullptr);
		EXPECT_EQ(l_A.stroke()->style, l_B.stroke()->style);
		EXPECT_EQ(l_A.stroke()->points, l_B.stroke()->points);
	}
	// Ids handed out after loading must not collide with loaded ones
	EXPECT_GE(l_Loaded.allocateId(), l_Source.peekNextId());
}

TEST(Serializer, EmptyBoardRoundTrips)
{
	wb::Document l_Source;
	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Source, {});
	wb::Document l_Loaded;
	fill(l_Loaded, 3); // loading replaces the content
	wb::BoardMeta l_Meta;
	ASSERT_TRUE(wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta).ok);
	EXPECT_TRUE(l_Loaded.empty());
}

TEST(Serializer, UnknownChunksAreSkipped)
{
	wb::Document l_Source;
	fill(l_Source, 2);
	std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Source, {});
	// Insert a chunk from "a newer version" right before the END chunk (16 bytes: empty payload)
	std::vector<uint8_t> l_Extra;
	appendChunk(l_Extra, "ZZZZ", { 1, 2, 3, 4, 5 });
	l_Bytes.insert(l_Bytes.end() - 16, l_Extra.begin(), l_Extra.end());

	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	EXPECT_EQ(l_Loaded.size(), 2u);
}

TEST(Serializer, NewerVersionIsRejectedWithAClearMessage)
{
	wb::Document l_Source;
	std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Source, {});
	l_Bytes[8] = static_cast<uint8_t>(wb::BOARD_FORMAT_VERSION + 1);
	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta);
	EXPECT_FALSE(l_Result.ok);
	EXPECT_NE(l_Result.error.find("newer"), std::string::npos);
}

TEST(Serializer, EveryTruncationIsRejectedAndLeavesTheDocumentAlone)
{
	wb::Document l_Source;
	fill(l_Source, 3);
	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Source, {});

	wb::Document l_Target;
	fill(l_Target, 2);
	const size_t l_Before = l_Target.size();
	wb::BoardMeta l_Meta;
	for (size_t l_Length = 0; l_Length < l_Bytes.size(); ++l_Length)
	{
		const wb::LoadResult l_Result = wb::deserializeBoard(std::span<const uint8_t>(l_Bytes.data(), l_Length), l_Target, l_Meta);
		ASSERT_FALSE(l_Result.ok) << "truncated to " << l_Length << " bytes was accepted";
		ASSERT_EQ(l_Target.size(), l_Before);
	}
	EXPECT_TRUE(wb::deserializeBoard(l_Bytes, l_Target, l_Meta).ok);
}

TEST(Serializer, EverySingleByteCorruptionIsRejected)
{
	wb::Document l_Source;
	fill(l_Source, 2);
	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Source, { .sourcePath = "x" });

	wb::Document l_Target;
	wb::BoardMeta l_Meta;
	for (size_t i = 0; i < l_Bytes.size(); ++i)
	{
		std::vector<uint8_t> l_Damaged = l_Bytes;
		l_Damaged[i] ^= 0x5A;
		const wb::LoadResult l_Result = wb::deserializeBoard(l_Damaged, l_Target, l_Meta);
		if (!l_Result.ok)
			continue;
		// The only tolerated survivor: the tag of the optional VIEW chunk became an "unknown chunk" and was skipped.
		// The objects must still be exact.
		ASSERT_EQ(l_Target.size(), l_Source.size()) << "byte " << i;
		for (size_t k = 0; k < l_Source.size(); ++k)
			ASSERT_EQ(l_Source.objects()[k]->stroke()->points, l_Target.objects()[k]->stroke()->points) << "byte " << i;
		ASSERT_EQ(l_Meta.viewZoom, 1.0) << "byte " << i;
	}
}

TEST(Serializer, RandomGarbageNeverCrashes)
{
	std::mt19937 l_Rng(1234);
	wb::Document l_Source;
	fill(l_Source, 3);
	const std::vector<uint8_t> l_Valid = wb::serializeBoard(l_Source, {});

	wb::Document l_Target;
	wb::BoardMeta l_Meta;
	for (int l_Round = 0; l_Round < 300; ++l_Round)
	{
		std::vector<uint8_t> l_Bytes = l_Valid;
		// Pure noise, or a valid file with a handful of bytes replaced
		if (l_Round % 3 == 0)
		{
			l_Bytes.resize(l_Rng() % 400);
			for (uint8_t& l_Byte : l_Bytes)
				l_Byte = static_cast<uint8_t>(l_Rng());
		}
		else
		{
			for (int k = 0; k < 8; ++k)
				l_Bytes[l_Rng() % l_Bytes.size()] = static_cast<uint8_t>(l_Rng());
		}
		(void)wb::deserializeBoard(l_Bytes, l_Target, l_Meta); // must return, whatever the verdict
	}
	SUCCEED();
}

TEST(Serializer, ChunksWithValidChecksumsButBadContentAreRejected)
{
	// A crafted file: valid framing, object count far larger than the data
	std::vector<uint8_t> l_Bytes{ 'W', 'B', 'R', 'D', 0x0D, 0x0A, 0x1A, 0x0A, 1, 0, 0, 0 };
	std::vector<uint8_t> l_Meta(8 + 8 + 4, 0);
	l_Meta[8] = 1;
	appendChunk(l_Bytes, "META", l_Meta);
	std::vector<uint8_t> l_Objects{ 0xFF, 0xFF, 0xFF, 0xFF };
	appendChunk(l_Bytes, "OBJS", l_Objects);
	appendChunk(l_Bytes, "END ", {});

	wb::Document l_Target;
	wb::BoardMeta l_MetaOut;
	EXPECT_FALSE(wb::deserializeBoard(l_Bytes, l_Target, l_MetaOut).ok);
}

TEST(FileIo, AtomicWriteReplacesAndLeavesNoTempFile)
{
	const std::filesystem::path l_Dir = std::filesystem::temp_directory_path() / "wb_io_test";
	std::filesystem::remove_all(l_Dir);
	const std::filesystem::path l_File = l_Dir / "nested" / "board.wbrd";

	const std::vector<uint8_t> l_First{ 1, 2, 3 };
	const std::vector<uint8_t> l_Second{ 9, 8, 7, 6, 5 };
	ASSERT_TRUE(wb::writeFileAtomic(l_File, l_First).ok);
	ASSERT_TRUE(wb::writeFileAtomic(l_File, l_Second).ok);

	std::vector<uint8_t> l_Read;
	ASSERT_TRUE(wb::readFile(l_File, l_Read).ok);
	EXPECT_EQ(l_Read, l_Second);
	EXPECT_FALSE(std::filesystem::exists(l_File.string() + ".tmp"));

	EXPECT_FALSE(wb::readFile(l_Dir / "missing.wbrd", l_Read).ok);
	wb::removeFileQuiet(l_File);
	EXPECT_FALSE(std::filesystem::exists(l_File));
	std::filesystem::remove_all(l_Dir);
}

TEST(FileIo, Utf8PathsRoundTrip)
{
	const std::string l_Utf8 = "dir/pi\xC3\xB1" "ata \xE2\x9C\x93.wbrd";
	EXPECT_EQ(wb::pathToUtf8(wb::pathFromUtf8(l_Utf8)), l_Utf8);
}

TEST(FileIo, FullBoardSurvivesDisk)
{
	const std::filesystem::path l_File = std::filesystem::temp_directory_path() / "wb_io_test_board.wbrd";
	wb::Document l_Source;
	fill(l_Source, 4);
	ASSERT_TRUE(wb::writeFileAtomic(l_File, wb::serializeBoard(l_Source, {})).ok);

	std::vector<uint8_t> l_Bytes;
	ASSERT_TRUE(wb::readFile(l_File, l_Bytes).ok);
	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	ASSERT_TRUE(wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta).ok);
	EXPECT_EQ(l_Loaded.size(), 4u);
	wb::removeFileQuiet(l_File);
}

TEST(Settings, RoundTripKeepsUnknownKeysAndIgnoresGarbage)
{
	const std::filesystem::path l_File = std::filesystem::temp_directory_path() / "wb_settings_test.ini";
	wb::Settings l_Settings;
	l_Settings.setFloat("brush.size", 6.25f);
	l_Settings.setBool("ui.dark", true);
	l_Settings.setInt("n", -7);
	l_Settings.set("future.key", "some value");
	ASSERT_TRUE(l_Settings.save(l_File).ok);

	wb::Settings l_Loaded;
	l_Loaded.load(l_File);
	EXPECT_FLOAT_EQ(l_Loaded.getFloat("brush.size", 0.f), 6.25f);
	EXPECT_TRUE(l_Loaded.getBool("ui.dark", false));
	EXPECT_EQ(l_Loaded.getInt("n", 0), -7);
	EXPECT_EQ(l_Loaded.getString("future.key"), "some value");
	EXPECT_EQ(l_Loaded.getInt("absent", 42), 42);
	EXPECT_FLOAT_EQ(l_Loaded.getFloat("future.key", 1.5f), 1.5f); // not a number: default
	wb::removeFileQuiet(l_File);
}

TEST(Serializer, LockedObjectsStayLocked)
{
	wb::Document l_Doc;
	fill(l_Doc, 4);
	const wb::ObjectId l_Second = l_Doc.objects()[1]->id;
	const wb::ObjectId l_Fourth = l_Doc.objects()[3]->id;
	l_Doc.modify(l_Second, [](wb::Object& p_Object) { p_Object.locked = true; }, wb::ObjectChange::Lock);
	l_Doc.modify(l_Fourth, [](wb::Object& p_Object) { p_Object.locked = true; }, wb::ObjectChange::Lock);

	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Doc, {});
	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	ASSERT_TRUE(wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta).ok);
	ASSERT_EQ(l_Loaded.size(), 4u);
	for (const auto& l_Object : l_Loaded.objects())
		EXPECT_EQ(l_Object->locked, l_Object->id == l_Second || l_Object->id == l_Fourth);
}
