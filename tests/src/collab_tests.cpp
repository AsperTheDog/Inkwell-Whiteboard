#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.object;
import wb.doc.commands;
import wb.doc.document;
import wb.doc.history;
import wb.io.blob;
import wb.io.serializer;
import wb.net.collab;
import wb.net.connection;
import wb.net.crypto;
import wb.net.ops;
import wb.net.protocol;
import wb.net.socket;
import wb.net.wire;
import wb.util.sha256;

namespace
{
std::string hex(const wb::Sha256Digest& p_Digest)
{
	static constexpr char DIGITS[] = "0123456789abcdef";
	std::string l_Text;
	for (const uint8_t l_Byte : p_Digest)
	{
		l_Text.push_back(DIGITS[l_Byte >> 4]);
		l_Text.push_back(DIGITS[l_Byte & 15]);
	}
	return l_Text;
}

std::string sha(const std::string& p_Text)
{
	return hex(wb::sha256(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p_Text.data()), p_Text.size())));
}

std::unique_ptr<wb::Object> makeStroke(wb::Document& p_Document, const float p_X)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate({ static_cast<double>(p_X), 1.0 });
	l_Object->payload = wb::StrokeData{
		.style = {},
		.points = { { .position = { 0.f, 0.f }, .radius = 1.f }, { .position = { p_X, 3.f }, .radius = 2.f } },
	};
	return l_Object;
}

std::vector<wb::ObjectId> idsOf(const wb::Document& p_Document)
{
	std::vector<wb::ObjectId> l_Ids;
	for (const auto& l_Object : p_Document.objects())
		l_Ids.push_back(l_Object->id);
	return l_Ids;
}

// One participant: a document plus its session, wired to the real .wbrd serializer
struct Node
{
	wb::Document doc;
	std::vector<std::string> messages;
	std::vector<std::string> errors;
	std::unique_ptr<wb::net::CollabSession> session;
	// Assets at least this big are left out of the join snapshot and streamed afterwards (the app uses 256 KiB)
	size_t inlineAssetBytes = SIZE_MAX;

	explicit Node(const bool p_Thumbnails = false)
	{
		wb::net::CollabHooks l_Hooks;
		if (p_Thumbnails)
		{
			// A fixed 4x2 thumbnail whose first pixel carries the file's first byte
			l_Hooks.makePreview = [](const std::shared_ptr<const wb::ByteSource>& p_Source) -> std::optional<wb::AssetPreview>
			{
				wb::AssetPreview l_Preview{ .width = 4, .height = 2, .rgba = std::vector<uint8_t>(32, 7) };
				uint8_t l_First = 0;
				l_Preview.rgba[0] = p_Source->read(0, &l_First, 1) == 1 ? l_First : 0;
				return l_Preview;
			};
		}
		l_Hooks.makeSnapshot = [this] { return wb::serializeBoardShell(doc, wb::BoardMeta{}, inlineAssetBytes); };
		l_Hooks.loadSnapshot = [this](const std::span<const uint8_t> p_Bytes)
		{
			wb::BoardMeta l_Meta;
			return wb::deserializeBoard(p_Bytes, doc, l_Meta, true).ok;
		};
		l_Hooks.message = [this](const std::string& p_Text, const bool p_Error) { (p_Error ? errors : messages).push_back(p_Text); };
		session = std::make_unique<wb::net::CollabSession>(doc, std::move(l_Hooks));
	}
};

bool pumpUntil(const std::vector<Node*>& p_Nodes, const std::function<bool()>& p_Done, const int p_TimeoutMs = 8000)
{
	const auto l_End = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_TimeoutMs);
	while (std::chrono::steady_clock::now() < l_End)
	{
		for (Node* l_Node : p_Nodes)
			l_Node->session->tick();
		if (p_Done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return false;
}

bool joinedAndOnline(const Node& p_Node)
{
	return p_Node.session->online();
}

std::vector<uint8_t> noise(const size_t p_Size, const uint32_t p_Seed)
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

// A picture object on p_Document using a new asset of p_Bytes
wb::ObjectId addPicture(wb::Document& p_Document, std::vector<uint8_t> p_Bytes, const std::string& p_Name)
{
	wb::ImageAsset l_Asset;
	l_Asset.bytes = std::move(p_Bytes);
	l_Asset.width = 40;
	l_Asset.height = 30;
	l_Asset.name = p_Name;
	const wb::AssetId l_AssetId = p_Document.addAsset(std::move(l_Asset));
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->payload = wb::ImageData{ .asset = l_AssetId, .size = { 40.f, 30.f } };
	const wb::ObjectId l_Id = l_Object->id;
	p_Document.insert(std::move(l_Object));
	return l_Id;
}

// The file is on the board and all of it has arrived (a big one is on the board from its first piece)
bool hasWhole(const wb::Document& p_Document, const wb::AssetId p_Id)
{
	const wb::ImageAsset* l_Asset = p_Document.findAsset(p_Id);
	return l_Asset != nullptr && l_Asset->complete();
}

const wb::ImageAsset* assetOf(const wb::Document& p_Document, const wb::ObjectId p_Id)
{
	const wb::Object* l_Object = p_Document.find(p_Id);
	return l_Object != nullptr && l_Object->image() != nullptr ? p_Document.findAsset(l_Object->image()->asset) : nullptr;
}
} // namespace

TEST(Sha256, MatchesKnownVectors)
{
	EXPECT_EQ(sha(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	EXPECT_EQ(sha("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	EXPECT_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Endpoint, ParsesTheFormsPeopleType)
{
	std::string l_Host;
	uint16_t l_Port = 0;
	EXPECT_TRUE(wb::net::parseEndpoint("192.168.1.5:7000", 5000, l_Host, l_Port));
	EXPECT_EQ(l_Host, "192.168.1.5");
	EXPECT_EQ(l_Port, 7000);
	EXPECT_TRUE(wb::net::parseEndpoint("  example.org ", 5000, l_Host, l_Port));
	EXPECT_EQ(l_Host, "example.org");
	EXPECT_EQ(l_Port, 5000);
	EXPECT_TRUE(wb::net::parseEndpoint("[::1]:8123", 5000, l_Host, l_Port));
	EXPECT_EQ(l_Host, "::1");
	EXPECT_EQ(l_Port, 8123);
	EXPECT_TRUE(wb::net::parseEndpoint("fe80::1", 5000, l_Host, l_Port));
	EXPECT_EQ(l_Host, "fe80::1");
	EXPECT_EQ(l_Port, 5000);
	EXPECT_FALSE(wb::net::parseEndpoint("", 5000, l_Host, l_Port));
	EXPECT_FALSE(wb::net::parseEndpoint(":80", 5000, l_Host, l_Port));
	EXPECT_FALSE(wb::net::parseEndpoint("host:0", 5000, l_Host, l_Port));
	EXPECT_FALSE(wb::net::parseEndpoint("host:70000", 5000, l_Host, l_Port));
	EXPECT_FALSE(wb::net::parseEndpoint("host:12ab", 5000, l_Host, l_Port));
	EXPECT_FALSE(wb::net::parseEndpoint("[::1", 5000, l_Host, l_Port));
}

TEST(Protocol, MessagesRoundTripAndRejectDamage)
{
	using namespace wb::net;
	Hello l_Hello;
	l_Hello.name = "Ana";
	l_Hello.proof = wb::net::crypto::Digest{ 1, 2, 3 };
	Hello l_HelloBack;
	ASSERT_TRUE(decode(encode(l_Hello), l_HelloBack));
	EXPECT_EQ(l_HelloBack.name, "Ana");
	EXPECT_EQ(l_HelloBack.proof, l_Hello.proof);

	Welcome l_Welcome;
	l_Welcome.slot = 3;
	l_Welcome.users = { UserInfo{ .slot = 0, .name = "Host", .color = 0x112233FFu, .host = true }, UserInfo{ .slot = 3, .name = "Ana", .color = 0x445566FFu, .host = false } };
	Welcome l_WelcomeBack;
	ASSERT_TRUE(decode(encode(l_Welcome), l_WelcomeBack));
	ASSERT_EQ(l_WelcomeBack.users.size(), 2u);
	EXPECT_TRUE(l_WelcomeBack.users[0].host);
	EXPECT_EQ(l_WelcomeBack.users[1].color, 0x445566FFu);

	// Truncated, padded and oversized payloads are all refused
	std::vector<uint8_t> l_Bytes = encode(l_Welcome);
	Welcome l_Scratch;
	l_Bytes.pop_back();
	EXPECT_FALSE(decode(l_Bytes, l_Scratch));
	l_Bytes = encode(l_Welcome);
	l_Bytes.push_back(0);
	EXPECT_FALSE(decode(l_Bytes, l_Scratch));
	Hello l_LongName = l_Hello;
	l_LongName.name = std::string(MAX_NAME_BYTES + 1, 'x');
	EXPECT_FALSE(decode(encode(l_LongName), l_HelloBack));

	// An op count the payload cannot hold
	ByteWriter l_Writer;
	l_Writer.u32(1u << 19);
	OpsMessage l_Ops;
	EXPECT_FALSE(decode(l_Writer.take(), l_Ops));

	// The host proof rides along in Welcome
	l_Welcome.proof[5] = 0x42;
	ASSERT_TRUE(decode(encode(l_Welcome), l_WelcomeBack));
	EXPECT_EQ(l_WelcomeBack.proof, l_Welcome.proof);

	Challenge l_Challenge;
	l_Challenge.salt[3] = 9;
	Challenge l_ChallengeBack;
	ASSERT_TRUE(decode(encode(l_Challenge), l_ChallengeBack));
	EXPECT_EQ(l_ChallengeBack.salt, l_Challenge.salt);
}

TEST(Protocol, NamesAreCleaned)
{
	EXPECT_EQ(wb::net::sanitizeName("  Ana  "), "Ana");
	EXPECT_EQ(wb::net::sanitizeName(""), "Guest");
	EXPECT_EQ(wb::net::sanitizeName("\t\n"), "Guest");
	EXPECT_EQ(wb::net::sanitizeName("A\nB"), "AB");
	const std::string l_Cut = wb::net::sanitizeName(std::string(100, 'x'));
	EXPECT_EQ(l_Cut.size(), wb::net::MAX_NAME_BYTES);
	// Never cut through a multi-byte character
	const std::string l_Accents = wb::net::sanitizeName(std::string(31, 'a') + "\xC3\xA9\xC3\xA9");
	EXPECT_LE(l_Accents.size(), wb::net::MAX_NAME_BYTES);
	EXPECT_NE(static_cast<uint8_t>(l_Accents.back()) & 0xC0, 0xC0);
}

TEST(Protocol, AssetMessagesRoundTripAndRejectLies)
{
	using namespace wb::net;
	AssetChunk l_Chunk;
	l_Chunk.id = 5;
	l_Chunk.total = 1000;
	l_Chunk.offset = 900;
	l_Chunk.width = 3;
	l_Chunk.height = 4;
	l_Chunk.name = "clip.mp4";
	l_Chunk.bytes.assign(100, 0xAB);
	AssetChunk l_Back;
	ASSERT_TRUE(decode(encode(l_Chunk), l_Back));
	EXPECT_EQ(l_Back.id, 5u);
	EXPECT_EQ(l_Back.total, 1000u);
	EXPECT_EQ(l_Back.offset, 900u);
	EXPECT_EQ(l_Back.name, "clip.mp4");
	EXPECT_EQ(l_Back.bytes.size(), 100u);

	AssetChunk l_Bad = l_Chunk;
	l_Bad.bytes.assign(101, 0); // runs past the end of the file
	EXPECT_FALSE(decode(encode(l_Bad), l_Back));
	l_Bad = l_Chunk;
	l_Bad.total = MAX_ASSET_BYTES + 1;
	EXPECT_FALSE(decode(encode(l_Bad), l_Back));
	l_Bad = l_Chunk;
	l_Bad.offset = 1000; // starts at the end
	EXPECT_FALSE(decode(encode(l_Bad), l_Back));
	l_Bad = l_Chunk;
	l_Bad.bytes.clear();
	EXPECT_FALSE(decode(encode(l_Bad), l_Back));
	l_Bad = l_Chunk;
	l_Bad.total = 1u << 30;
	l_Bad.offset = 0;
	l_Bad.bytes.assign(ASSET_CHUNK + 1, 0); // bigger than a piece may be
	EXPECT_FALSE(decode(encode(l_Bad), l_Back));

	AssetRequest l_Request;
	l_Request.id = 9;
	AssetRequest l_RequestBack;
	ASSERT_TRUE(decode(encode(l_Request), l_RequestBack));
	EXPECT_EQ(l_RequestBack.id, 9u);
	l_Request.id = wb::INVALID_ASSET_ID;
	EXPECT_FALSE(decode(encode(l_Request), l_RequestBack));
}

TEST(Protocol, CursorMessagesRoundTripAndRejectNonsense)
{
	using namespace wb::net;
	CursorMessage l_Cursor{ .slot = 3, .x = -12.5, .y = 4096.25, .visible = true };
	CursorMessage l_Back;
	ASSERT_TRUE(decode(encode(l_Cursor), l_Back));
	EXPECT_EQ(l_Back.slot, 3);
	EXPECT_EQ(l_Back.x, -12.5);
	EXPECT_EQ(l_Back.y, 4096.25);
	EXPECT_TRUE(l_Back.visible);
	l_Cursor.x = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(decode(encode(l_Cursor), l_Back));
	l_Cursor.x = 0.0;
	l_Cursor.y = 1e13;
	EXPECT_FALSE(decode(encode(l_Cursor), l_Back));
	std::vector<uint8_t> l_Short = encode(CursorMessage{});
	l_Short.pop_back();
	EXPECT_FALSE(decode(l_Short, l_Back));
}

// Everybody sees where everybody else points, labelled with the right person
TEST(Protocol, StrokePreviewMessagesRoundTripAndRejectNonsense)
{
	wb::net::StrokePreviewMessage l_Message;
	l_Message.slot = 3;
	l_Message.stroke = 17;
	l_Message.color[0] = 0.25f;
	l_Message.color[3] = 0.5f;
	l_Message.originX = -1234.5;
	l_Message.originY = 99.0;
	l_Message.base = 40;
	for (int i = 0; i < 100; ++i)
		l_Message.points.push_back(wb::StrokePoint{ .position = wb::Vec2{ static_cast<float>(i), -static_cast<float>(i) }, .radius = 1.5f });
	const std::vector<uint8_t> l_Bytes = wb::net::encode(l_Message);
	wb::net::StrokePreviewMessage l_Back;
	ASSERT_TRUE(wb::net::decode(l_Bytes, l_Back));
	EXPECT_EQ(l_Back.slot, 3);
	EXPECT_EQ(l_Back.stroke, 17u);
	EXPECT_FALSE(l_Back.end);
	EXPECT_EQ(l_Back.base, 40u);
	EXPECT_EQ(l_Back.originX, -1234.5);
	EXPECT_EQ(l_Back.color[3], 0.5f);
	EXPECT_EQ(l_Back.points, l_Message.points);

	// Cut short, or with trailing bytes
	EXPECT_FALSE(wb::net::decode(std::span<const uint8_t>(l_Bytes).first(l_Bytes.size() - 1), l_Back));
	std::vector<uint8_t> l_Long = l_Bytes;
	l_Long.push_back(0);
	EXPECT_FALSE(wb::net::decode(l_Long, l_Back));

	// Numbers that are not numbers, colours out of range, and more points than a message may hold
	auto l_Bad = l_Message;
	l_Bad.points[5].position.x = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(wb::net::decode(wb::net::encode(l_Bad), l_Back));
	l_Bad = l_Message;
	l_Bad.points[5].radius = -1.f;
	EXPECT_FALSE(wb::net::decode(wb::net::encode(l_Bad), l_Back));
	l_Bad = l_Message;
	l_Bad.color[1] = 2.f;
	EXPECT_FALSE(wb::net::decode(wb::net::encode(l_Bad), l_Back));
	l_Bad = l_Message;
	l_Bad.originX = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(wb::net::decode(wb::net::encode(l_Bad), l_Back));
	l_Bad = l_Message;
	l_Bad.points.assign(wb::net::MAX_STROKE_POINTS_PER_MESSAGE + 1, wb::StrokePoint{});
	EXPECT_FALSE(wb::net::decode(wb::net::encode(l_Bad), l_Back));
}

// Somebody draws: the others see the line grow (its provisional end replaced as it changes) until it is finished
TEST(Collab, LinesBeingDrawnAreSeenByEveryoneAndGoAwayWhenFinished)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo) && l_Ana.session->users().size() == 3 && l_Bo.session->users().size() == 3; }));
	const uint16_t l_AnaSlot = l_Ana.session->localSlot();
	const auto find = [](const Node& p_Node, const uint16_t p_Slot) -> std::optional<wb::net::CollabSession::StrokeView>
	{
		for (const auto& l_View : p_Node.session->liveStrokes())
		{
			if (l_View.slot == p_Slot)
				return l_View;
		}
		return std::nullopt;
	};
	const auto points = [](const int p_From, const int p_To)
	{
		std::vector<wb::StrokePoint> l_Points;
		for (int i = p_From; i < p_To; ++i)
			l_Points.push_back(wb::StrokePoint{ .position = wb::Vec2{ static_cast<float>(i), 0.f }, .radius = 2.f });
		return l_Points;
	};

	// The first points, with a provisional tail
	std::vector<wb::StrokePoint> l_Points = points(0, 12);
	wb::net::CollabSession::LocalStroke l_Stroke{ .origin = { 500.0, -20.0 }, .color = wb::Color{ .r = 1.f, .g = 0.f, .b = 0.f, .a = 1.f } };
	l_Stroke.stable = std::span<const wb::StrokePoint>(l_Points).first(10);
	l_Stroke.tail = std::span<const wb::StrokePoint>(l_Points).last(2);
	l_Ana.session->setLocalStroke(&l_Stroke);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Host, l_AnaSlot) && find(l_Bo, l_AnaSlot); }));
	EXPECT_FALSE(find(l_Ana, l_AnaSlot)); // nobody is shown their own line
	EXPECT_EQ(find(l_Bo, l_AnaSlot)->points.size(), 12u);
	EXPECT_EQ(find(l_Bo, l_AnaSlot)->origin.x, 500.0);
	EXPECT_EQ(find(l_Bo, l_AnaSlot)->color.r, 1.f);

	// It grows: the old tail is replaced, the earlier points are not sent again
	l_Points = points(0, 30);
	l_Points[11].radius = 9.f; // the provisional end moved
	l_Stroke.stable = std::span<const wb::StrokePoint>(l_Points).first(25);
	l_Stroke.tail = std::span<const wb::StrokePoint>(l_Points).last(5);
	l_Ana.session->setLocalStroke(&l_Stroke);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Bo, l_AnaSlot) && find(l_Bo, l_AnaSlot)->points.size() == 30 && find(l_Host, l_AnaSlot)->points.size() == 30; }));
	EXPECT_TRUE(std::ranges::equal(find(l_Bo, l_AnaSlot)->points, l_Points));

	// Finished: gone everywhere
	l_Ana.session->setLocalStroke(nullptr);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return !find(l_Bo, l_AnaSlot) && !find(l_Host, l_AnaSlot); }));

	// The host's own line reaches the clients, and a person who leaves in the middle takes theirs along
	l_Host.session->setLocalStroke(&l_Stroke);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Ana, 0) && find(l_Bo, 0); }));
	l_Ana.session->setLocalStroke(&l_Stroke);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Bo, l_AnaSlot).has_value(); }));
	l_Ana.session->leave();
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Bo }, [&] { return !find(l_Bo, l_AnaSlot) && !find(l_Host, l_AnaSlot); }));
	EXPECT_TRUE(find(l_Bo, 0)); // the host's is still being drawn
}

// Play/pause and the playhead are shared; loop, mute and volume stay with each person
TEST(Collab, VideoPlaybackAndLoopAreSharedButVolumeIsNot)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	const wb::ObjectId l_Video = addPicture(l_Host.doc, noise(4u << 10, 5), "clip.mp4");
	const wb::AssetId l_Asset = l_Host.doc.find(l_Video)->image()->asset;
	l_Host.doc.modify(l_Video, [&](wb::Object& p_Object) { p_Object.payload = wb::VideoData{ .asset = l_Asset, .size = { 40.f, 30.f }, .loop = true, .muted = false, .volume = 0.3f }; }, wb::ObjectChange::Geometry | wb::ObjectChange::Style);
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo) && l_Ana.session->users().size() == 3 && l_Bo.session->users().size() == 3; }));
	ASSERT_NE(l_Bo.doc.find(l_Video)->video(), nullptr);

	// Ana presses play at 12.5 s: the host and Bo are told
	l_Ana.session->sendVideoState(l_Video, true, 12.5);
	std::vector<wb::net::CollabSession::VideoEvent> l_HostEvents;
	std::vector<wb::net::CollabSession::VideoEvent> l_BoEvents;
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] {
		for (const auto& l_Event : l_Host.session->takeVideoEvents())
			l_HostEvents.push_back(l_Event);
		for (const auto& l_Event : l_Bo.session->takeVideoEvents())
			l_BoEvents.push_back(l_Event);
		return !l_HostEvents.empty() && !l_BoEvents.empty();
	}));
	EXPECT_EQ(l_BoEvents.back().id, l_Video);
	EXPECT_TRUE(l_BoEvents.back().playing);
	EXPECT_EQ(l_BoEvents.back().position, 12.5);
	EXPECT_EQ(l_HostEvents.back().position, 12.5);
	EXPECT_TRUE(l_Ana.session->takeVideoEvents().empty()); // nobody is told about their own

	// Bo likes it quiet and not looping: the loop switch reaches everybody, the sound settings do not
	l_Bo.doc.modify(l_Video, [&](wb::Object& p_Object) { *p_Object.video() = wb::VideoData{ .asset = l_Asset, .size = { 40.f, 30.f }, .loop = false, .muted = true, .volume = 0.9f, .audioTrack = 1, .subtitleTrack = 0 }; }, wb::ObjectChange::Geometry | wb::ObjectChange::Style);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return !l_Host.doc.find(l_Video)->video()->loop && !l_Ana.doc.find(l_Video)->video()->loop; }));
	EXPECT_EQ(l_Host.doc.find(l_Video)->video()->volume, 0.3f);
	EXPECT_EQ(l_Host.doc.find(l_Video)->video()->audioTrack, -1); // which tracks somebody watches is their own
	EXPECT_EQ(l_Host.doc.find(l_Video)->video()->subtitleTrack, -1);

	// A real change from the host (the size) reaches Bo, who keeps his own sound settings
	l_Host.doc.modify(l_Video, [&](wb::Object& p_Object) { p_Object.video()->size = { 80.f, 60.f }; }, wb::ObjectChange::Geometry);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Bo.doc.find(l_Video)->video()->size.x == 80.f; }));
	EXPECT_FALSE(l_Bo.doc.find(l_Video)->video()->loop);
	EXPECT_TRUE(l_Bo.doc.find(l_Video)->video()->muted);
	EXPECT_EQ(l_Bo.doc.find(l_Video)->video()->volume, 0.9f);
	EXPECT_EQ(l_Bo.doc.find(l_Video)->video()->audioTrack, 1);
	EXPECT_EQ(l_Bo.doc.find(l_Video)->video()->subtitleTrack, 0);

	// Somebody joining finds it playing, a little further on
	Node l_Cy;
	l_Cy.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Cy");
	std::vector<wb::net::CollabSession::VideoEvent> l_CyEvents;
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo, &l_Cy }, [&] {
		for (const auto& l_Event : l_Cy.session->takeVideoEvents())
			l_CyEvents.push_back(l_Event);
		return !l_CyEvents.empty();
	}));
	EXPECT_EQ(l_CyEvents.back().id, l_Video);
	EXPECT_TRUE(l_CyEvents.back().playing);
	EXPECT_GE(l_CyEvents.back().position, 12.5);
	EXPECT_LT(l_CyEvents.back().position, 60.0);
}

// Anybody can add a subtitle file to a video and it is listed for everybody; which one each person watches is their own
TEST(Collab, SubtitleFilesAreSharedButTheChoiceIsNot)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	const wb::ObjectId l_Video = addPicture(l_Host.doc, noise(4u << 10, 6), "clip.mp4");
	const wb::AssetId l_Asset = l_Host.doc.find(l_Video)->image()->asset;
	l_Host.doc.modify(l_Video, [&](wb::Object& p_Object) { p_Object.payload = wb::VideoData{ .asset = l_Asset, .size = { 40.f, 30.f } }; }, wb::ObjectChange::Geometry | wb::ObjectChange::Style);
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo) && l_Ana.session->users().size() == 3 && l_Bo.session->users().size() == 3; }));

	// Ana adds a file and watches with it; the others see it in their list but are not switched to it
	l_Ana.doc.modify(l_Video, [&](wb::Object& p_Object)
	{
		p_Object.video()->subtitleFiles.push_back(wb::SubtitleFile{ .name = "english.srt", .text = "1\n00:00:01,000 --> 00:00:02,000\nHello\n" });
		p_Object.video()->subtitleTrack = 0;
	}, wb::ObjectChange::Geometry | wb::ObjectChange::Style);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.doc.find(l_Video)->video()->subtitleFiles.size() == 1 && l_Bo.doc.find(l_Video)->video()->subtitleFiles.size() == 1; }));
	EXPECT_EQ(l_Bo.doc.find(l_Video)->video()->subtitleFiles[0].name, "english.srt");
	EXPECT_EQ(l_Bo.doc.find(l_Video)->video()->subtitleFiles[0].text, "1\n00:00:01,000 --> 00:00:02,000\nHello\n");
	EXPECT_EQ(l_Ana.doc.find(l_Video)->video()->subtitleTrack, 0);
	EXPECT_EQ(l_Host.doc.find(l_Video)->video()->subtitleTrack, -1);
	EXPECT_EQ(l_Bo.doc.find(l_Video)->video()->subtitleTrack, -1);

	// Bo picks it for themselves, then the host adds another: Bo keeps what they chose
	l_Bo.doc.modify(l_Video, [](wb::Object& p_Object) { p_Object.video()->subtitleTrack = 0; }, wb::ObjectChange::Style);
	l_Host.doc.modify(l_Video, [](wb::Object& p_Object) { p_Object.video()->subtitleFiles.push_back(wb::SubtitleFile{ .name = "spanish.srt", .text = "1\n00:00:01,000 --> 00:00:02,000\nHola\n" }); }, wb::ObjectChange::Style);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Ana.doc.find(l_Video)->video()->subtitleFiles.size() == 2 && l_Bo.doc.find(l_Video)->video()->subtitleFiles.size() == 2; }));
	EXPECT_EQ(l_Bo.doc.find(l_Video)->video()->subtitleTrack, 0);
	EXPECT_EQ(l_Ana.doc.find(l_Video)->video()->subtitleTrack, 0);
	EXPECT_EQ(l_Host.doc.find(l_Video)->video()->subtitleTrack, -1);
}

TEST(Protocol, VideoStateMessagesRoundTripAndRejectNonsense)
{
	wb::net::VideoStateMessage l_Back;
	ASSERT_TRUE(wb::net::decode(wb::net::encode(wb::net::VideoStateMessage{ .id = 77, .playing = true, .position = 3.25 }), l_Back));
	EXPECT_EQ(l_Back.id, 77u);
	EXPECT_TRUE(l_Back.playing);
	EXPECT_EQ(l_Back.position, 3.25);
	EXPECT_FALSE(wb::net::decode(wb::net::encode(wb::net::VideoStateMessage{ .id = wb::INVALID_OBJECT_ID, .playing = true, .position = 1.0 }), l_Back));
	EXPECT_FALSE(wb::net::decode(wb::net::encode(wb::net::VideoStateMessage{ .id = 1, .playing = true, .position = -1.0 }), l_Back));
	EXPECT_FALSE(wb::net::decode(wb::net::encode(wb::net::VideoStateMessage{ .id = 1, .playing = true, .position = std::numeric_limits<double>::quiet_NaN() }), l_Back));
	EXPECT_FALSE(wb::net::decode(wb::net::encode(wb::net::VideoStateMessage{ .id = 1, .playing = true, .position = 1e12 }), l_Back));
}

// The host does not trust a client's editor to respect locks
TEST(Collab, TheHostRefusesEditsToLockedObjectsAndCorrectsTheSender)
{
	Node l_Host;
	Node l_Ana;
	const wb::ObjectId l_Id = addPicture(l_Host.doc, noise(2u << 10, 6), "pic.png");
	l_Host.doc.modify(l_Id, [&](wb::Object& p_Object) { p_Object.locked = true; }, wb::ObjectChange::Lock);
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); }));
	ASSERT_TRUE(l_Ana.doc.find(l_Id)->locked);

	// Ana's copy is tampered with: moved, then deleted
	const wb::Affine2 l_Before = l_Host.doc.find(l_Id)->transform;
	l_Ana.doc.modify(l_Id, [&](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate(wb::DVec2{ 500.0, 0.0 }); }, wb::ObjectChange::Transform);
	pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Ana.doc.find(l_Id)->transform.translation.x == l_Before.translation.x; }, 3000);
	EXPECT_EQ(l_Host.doc.find(l_Id)->transform.translation.x, l_Before.translation.x);
	EXPECT_EQ(l_Ana.doc.find(l_Id)->transform.translation.x, l_Before.translation.x); // put back

	l_Ana.doc.take(l_Id);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Ana.doc.find(l_Id) != nullptr; }, 3000));
	EXPECT_NE(l_Host.doc.find(l_Id), nullptr);
	EXPECT_TRUE(l_Ana.doc.find(l_Id)->locked);

	// Unlocking is allowed, and then the object can be moved
	l_Ana.doc.modify(l_Id, [&](wb::Object& p_Object) { p_Object.locked = false; }, wb::ObjectChange::Lock);
	l_Ana.doc.modify(l_Id, [&](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate(wb::DVec2{ 70.0, 0.0 }); }, wb::ObjectChange::Transform);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Host.doc.find(l_Id)->transform.translation.x == 70.0; }));
	EXPECT_FALSE(l_Host.doc.find(l_Id)->locked);
}

TEST(Collab, AReadOnlyGuestWatchesButTheirEditsAreIgnored)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo) && l_Ana.session->users().size() == 3 && l_Bo.session->users().size() == 3; }));
	const uint16_t l_AnaSlot = l_Ana.session->localSlot();
	EXPECT_FALSE(l_Ana.session->localReadOnly());
	EXPECT_FALSE(l_Ana.session->setReadOnly(l_AnaSlot, true)); // only the host decides
	EXPECT_FALSE(l_Host.session->setReadOnly(999, true));      // nobody by that slot

	ASSERT_TRUE(l_Host.session->setReadOnly(l_AnaSlot, true));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Ana.session->localReadOnly() && l_Bo.session->user(l_AnaSlot) != nullptr && l_Bo.session->user(l_AnaSlot)->readOnly; }));
	EXPECT_FALSE(l_Bo.session->localReadOnly());
	EXPECT_FALSE(l_Host.session->localReadOnly());

	// Ana's edits (here forced past her editor) never reach the host or Bo; Bo's still reach everybody, Ana included
	const wb::ObjectId l_Mine = addPicture(l_Ana.doc, noise(2u << 10, 3), "mine.png");
	const wb::ObjectId l_Bos = addPicture(l_Bo.doc, noise(2u << 10, 4), "bos.png");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.doc.find(l_Bos) != nullptr && l_Ana.doc.find(l_Bos) != nullptr; }));
	pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.doc.find(l_Mine) != nullptr; }, 1000);
	EXPECT_EQ(l_Host.doc.find(l_Mine), nullptr);
	EXPECT_EQ(l_Bo.doc.find(l_Mine), nullptr);

	// Lifting it lets her edits through again
	ASSERT_TRUE(l_Host.session->setReadOnly(l_AnaSlot, false));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return !l_Ana.session->localReadOnly(); }));
	const wb::ObjectId l_Later = addPicture(l_Ana.doc, noise(2u << 10, 5), "later.png");
	EXPECT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.doc.find(l_Later) != nullptr && l_Bo.doc.find(l_Later) != nullptr; }));
}

TEST(Protocol, UserReadOnlyFlagRoundTrips)
{
	using namespace wb::net;
	UserInfo l_User{ .slot = 4, .name = "Ana", .color = 0x445566FFu, .host = false, .readOnly = true };
	UserInfo l_Back;
	ASSERT_TRUE(decode(encode(l_User), l_Back));
	EXPECT_TRUE(l_Back.readOnly);
	EXPECT_EQ(l_Back.slot, 4);
}

TEST(Collab, PointersAreSharedAndAttributedToTheirOwner)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo) && l_Ana.session->users().size() == 3 && l_Bo.session->users().size() == 3; }));
	const uint16_t l_AnaSlot = l_Ana.session->localSlot();
	const auto find = [](const Node& p_Node, const uint16_t p_Slot) -> std::optional<wb::net::CollabSession::CursorView>
	{
		for (const auto& l_View : p_Node.session->cursors())
		{
			if (l_View.slot == p_Slot)
				return l_View;
		}
		return std::nullopt;
	};

	// A client's pointer reaches the host and the other client, under the client's slot
	l_Ana.session->setLocalCursor({ 120.5, -40.0 }, true);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Host, l_AnaSlot) && find(l_Bo, l_AnaSlot); }));
	EXPECT_EQ(find(l_Bo, l_AnaSlot)->position.x, 120.5);
	EXPECT_EQ(find(l_Host, l_AnaSlot)->position.y, -40.0);
	EXPECT_TRUE(find(l_Bo, l_AnaSlot)->visible);
	EXPECT_FALSE(find(l_Ana, l_AnaSlot)); // nobody shows you your own pointer

	// The host's pointer reaches both clients (slot 0)
	l_Host.session->setLocalCursor({ 7.0, 8.0 }, true);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Ana, 0) && find(l_Bo, 0); }));
	EXPECT_EQ(find(l_Ana, 0)->position.x, 7.0);

	// Only the latest position is kept when it moves quickly
	for (int i = 1; i <= 50; ++i)
		l_Ana.session->setLocalCursor({ static_cast<double>(i), 0.0 }, true);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Bo, l_AnaSlot)->position.x == 50.0; }));

	// A held laser pointer leaves a trail on the other side, which ends when it is lifted
	l_Ana.session->setLocalCursor({ 60.0, 0.0 }, true, true, 2.f);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Bo, l_AnaSlot)->laser; }));
	l_Ana.session->setLocalCursor({ 70.0, 5.0 }, true, true, 2.f);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return find(l_Bo, l_AnaSlot)->trail.size() >= 2; }));
	EXPECT_TRUE(find(l_Bo, l_AnaSlot)->trail.front().newStroke);
	EXPECT_EQ(find(l_Bo, l_AnaSlot)->trail.back().position.x, 70.0);
	EXPECT_EQ(find(l_Bo, l_AnaSlot)->trailSeconds, 2.0);
	l_Ana.session->setLocalCursor({ 70.0, 5.0 }, true, false, 2.f);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return !find(l_Bo, l_AnaSlot)->laser; }));
	EXPECT_FALSE(find(l_Host, l_AnaSlot)->trail.empty());

	// Leaving the board hides it; leaving the session removes it
	l_Ana.session->setLocalCursor({ 50.0, 0.0 }, false);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return !find(l_Bo, l_AnaSlot)->visible; }));
	l_Ana.session->leave();
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Bo }, [&] { return !find(l_Bo, l_AnaSlot) && !find(l_Host, l_AnaSlot); }));
}

TEST(Collab, JoinSnapshotThenEditsFlowBothWays)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	l_Host.doc.insert(makeStroke(l_Host.doc, 1.f));
	l_Host.doc.insert(makeStroke(l_Host.doc, 2.f));

	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	const uint16_t l_Port = l_Host.session->listenPort();
	ASSERT_NE(l_Port, 0);

	l_Ana.session->join("127.0.0.1", l_Port, "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); })) << (l_Ana.errors.empty() ? "timeout" : l_Ana.errors.back());
	EXPECT_EQ(idsOf(l_Ana.doc), idsOf(l_Host.doc));
	EXPECT_NE(l_Ana.session->localSlot(), 0);

	l_Bo.session->join("127.0.0.1", l_Port, "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Bo) && l_Ana.session->users().size() == 3; }));
	EXPECT_NE(l_Ana.session->localSlot(), l_Bo.session->localSlot());
	ASSERT_EQ(l_Host.session->users().size(), 3u);
	EXPECT_TRUE(l_Host.session->users()[0].host);
	// Everybody sees the same people with the same colours
	for (Node* l_Node : { &l_Ana, &l_Bo })
	{
		ASSERT_EQ(l_Node->session->users().size(), 3u);
		for (size_t i = 0; i < 3; ++i)
		{
			EXPECT_EQ(l_Node->session->users()[i].slot, l_Host.session->users()[i].slot);
			EXPECT_EQ(l_Node->session->users()[i].color, l_Host.session->users()[i].color);
			EXPECT_EQ(l_Node->session->users()[i].name, l_Host.session->users()[i].name);
		}
	}

	// A client edit reaches the host and the other client
	const wb::ObjectId l_FromAna = l_Ana.doc.allocateId();
	EXPECT_EQ(wb::idSlotOf(l_FromAna), l_Ana.session->localSlot());
	auto l_Stroke = makeStroke(l_Ana.doc, 7.f);
	const wb::ObjectId l_AnaId = l_Stroke->id;
	l_Ana.doc.insert(std::move(l_Stroke));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.doc.find(l_AnaId) != nullptr && l_Bo.doc.find(l_AnaId) != nullptr; }));

	// A host edit reaches both clients
	const wb::ObjectId l_First = l_Host.doc.objects().front()->id;
	l_Host.doc.modify(l_First, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 50.0, 60.0 }); }, wb::ObjectChange::Transform);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Ana.doc.find(l_First)->transform.translation.x == 50.0 && l_Bo.doc.find(l_First)->transform.translation.x == 50.0; }));

	// Removal too
	(void)l_Bo.doc.take(l_AnaId);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.doc.find(l_AnaId) == nullptr && l_Ana.doc.find(l_AnaId) == nullptr; }));
	EXPECT_EQ(idsOf(l_Host.doc), idsOf(l_Ana.doc));
	EXPECT_EQ(idsOf(l_Host.doc), idsOf(l_Bo.doc));

	// A client leaving shows up for everybody else
	l_Bo.session->leave();
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Host.session->users().size() == 2 && l_Ana.session->users().size() == 2; }));
	EXPECT_EQ(l_Bo.session->state(), wb::net::LinkState::Offline);

	// The host ending the session tells the clients
	l_Host.session->leave();
	ASSERT_TRUE(pumpUntil({ &l_Ana }, [&] { return !l_Ana.session->active(); }));
	ASSERT_FALSE(l_Ana.errors.empty());
	EXPECT_NE(l_Ana.errors.back().find("host ended"), std::string::npos);
}

// The board arrives right behind Welcome in frames far bigger than the pre-login limit
TEST(Collab, JoinsABoardLargerThanTheLoginFrameLimit)
{
	Node l_Host;
	Node l_Ana;
	for (int i = 0; i < 8; ++i)
	{
		auto l_Stroke = makeStroke(l_Host.doc, static_cast<float>(i));
		auto& l_Data = std::get<wb::StrokeData>(l_Stroke->payload);
		for (int p = 0; p < 20000; ++p)
			l_Data.points.push_back({ .position = { static_cast<float>(p), static_cast<float>(i) }, .radius = 1.f });
		l_Host.doc.insert(std::move(l_Stroke));
	}
	ASSERT_GT(wb::serializeBoard(l_Host.doc, wb::BoardMeta{}).size(), size_t{ 1 } << 20);

	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); })) << (l_Ana.errors.empty() ? "timeout" : l_Ana.errors.back());
	EXPECT_EQ(idsOf(l_Ana.doc), idsOf(l_Host.doc));
}

// Pictures and videos follow the board in small pieces instead of riding in the snapshot
TEST(Collab, BigFilesStreamAfterTheBoardArrives)
{
	Node l_Host;
	Node l_Ana;
	l_Host.inlineAssetBytes = 256u << 10;
	const std::vector<uint8_t> l_Big = noise(3u << 20, 1);   // streamed
	const std::vector<uint8_t> l_Small = noise(10u << 10, 2); // inside the snapshot
	const wb::ObjectId l_BigId = addPicture(l_Host.doc, l_Big, "big.mp4");
	const wb::ObjectId l_SmallId = addPicture(l_Host.doc, l_Small, "small.png");

	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); })) << (l_Ana.errors.empty() ? "timeout" : l_Ana.errors.back());

	// The objects are there at once; the small file came with them, the big one is on its way
	ASSERT_NE(l_Ana.doc.find(l_BigId), nullptr);
	ASSERT_NE(l_Ana.doc.find(l_SmallId), nullptr);
	ASSERT_NE(assetOf(l_Ana.doc, l_SmallId), nullptr);
	EXPECT_EQ(assetOf(l_Ana.doc, l_SmallId)->bytes, l_Small);
	EXPECT_EQ(assetOf(l_Ana.doc, l_BigId), nullptr);
	EXPECT_EQ(l_Ana.session->assetProgress().files, 1u);

	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return assetOf(l_Ana.doc, l_BigId) != nullptr; }, 20000));
	EXPECT_EQ(assetOf(l_Ana.doc, l_BigId)->bytes, l_Big);
	EXPECT_EQ(assetOf(l_Ana.doc, l_BigId)->name, "big.mp4");
	EXPECT_EQ(assetOf(l_Ana.doc, l_BigId)->width, 40u);
	EXPECT_EQ(l_Ana.session->assetProgress().files, 0u);
}

// A picture added by a client reaches the host, which passes it on to the other clients
TEST(Collab, ClientPicturesReachEveryone)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo); }));

	const std::vector<uint8_t> l_Bytes = noise(1u << 20, 3);
	const wb::ObjectId l_Id = addPicture(l_Ana.doc, l_Bytes, "photo.jpg");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return assetOf(l_Host.doc, l_Id) != nullptr && assetOf(l_Bo.doc, l_Id) != nullptr; }, 20000));
	EXPECT_EQ(assetOf(l_Host.doc, l_Id)->bytes, l_Bytes);
	EXPECT_EQ(assetOf(l_Bo.doc, l_Id)->bytes, l_Bytes);
	EXPECT_EQ(l_Host.session->assetProgress().files, 0u);
	EXPECT_EQ(l_Bo.session->assetProgress().files, 0u);
}

// Reducing a picture swaps its file for a smaller one: everybody gets the new file, and putting the old one back (undo) works too
TEST(Collab, ASwappedPictureFileReachesEveryoneAndSwapsBack)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo); }));

	const std::vector<uint8_t> l_Big = noise(1u << 20, 5);
	const wb::ObjectId l_Id = addPicture(l_Ana.doc, l_Big, "photo.png");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return assetOf(l_Host.doc, l_Id) != nullptr && assetOf(l_Bo.doc, l_Id) != nullptr; }, 20000));
	const wb::AssetId l_OldAsset = l_Ana.doc.find(l_Id)->image()->asset;

	const std::vector<uint8_t> l_Small = noise(40u << 10, 6);
	wb::ImageAsset l_New;
	l_New.bytes = l_Small;
	l_New.width = 40;
	l_New.height = 30;
	l_New.name = "photo.jpg";
	const wb::AssetId l_NewAsset = l_Ana.doc.addAsset(std::move(l_New));
	l_Ana.doc.modify(l_Id, [&](wb::Object& p_Object) { p_Object.image()->asset = l_NewAsset; }, wb::ObjectChange::Geometry | wb::ObjectChange::Style);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return assetOf(l_Host.doc, l_Id) != nullptr && assetOf(l_Host.doc, l_Id)->bytes == l_Small && assetOf(l_Bo.doc, l_Id) != nullptr && assetOf(l_Bo.doc, l_Id)->bytes == l_Small; }, 20000));
	EXPECT_EQ(assetOf(l_Bo.doc, l_Id)->name, "photo.jpg");

	l_Ana.doc.modify(l_Id, [&](wb::Object& p_Object) { p_Object.image()->asset = l_OldAsset; }, wb::ObjectChange::Geometry | wb::ObjectChange::Style);
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return assetOf(l_Host.doc, l_Id) != nullptr && assetOf(l_Host.doc, l_Id)->bytes == l_Big && assetOf(l_Bo.doc, l_Id) != nullptr && assetOf(l_Bo.doc, l_Id)->bytes == l_Big; }, 20000));
}

// A client cannot make the host store a file under an id that belongs to somebody else
TEST(Collab, ClientsCannotPlantFilesUnderOthersIds)
{
	Node l_Host;
	Node l_Ana;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); }));

	// A picture that names a file owned by the host's id range, which only the host (and nobody else) can supply
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = l_Ana.doc.allocateId();
	l_Object->payload = wb::ImageData{ .asset = 5, .size = { 10.f, 10.f } };
	const wb::ObjectId l_Id = l_Object->id;
	l_Ana.doc.insert(std::move(l_Object));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Host.doc.find(l_Id) != nullptr; }));
	pumpUntil({ &l_Host, &l_Ana }, [] { return false; }, 300);
	EXPECT_EQ(l_Host.session->assetProgress().files, 0u); // the host did not ask Ana for it
	EXPECT_EQ(l_Host.doc.findAsset(5), nullptr);
}

// A thumbnail reaches the receiver ahead of the file
TEST(Collab, ThumbnailsTravelWithTheFile)
{
	Node l_Host(true);
	Node l_Ana;
	l_Host.inlineAssetBytes = 1;
	std::vector<uint8_t> l_Bytes = noise(2u << 20, 5);
	const wb::ObjectId l_Id = addPicture(l_Host.doc, l_Bytes, "big.png");
	const wb::AssetId l_Asset = l_Host.doc.find(l_Id)->image()->asset;

	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); }));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return assetOf(l_Ana.doc, l_Id) != nullptr; }, 20000));
	const wb::AssetPreview* l_Preview = l_Ana.doc.findPreview(l_Asset);
	ASSERT_NE(l_Preview, nullptr);
	EXPECT_EQ(l_Preview->width, 4u);
	EXPECT_EQ(l_Preview->height, 2u);
	EXPECT_EQ(l_Preview->rgba[0], l_Bytes[0]);
}

// The person sending a file leaves before it is complete: the transfer is cancelled and its pictures go for everybody
// A file too big for memory goes into a temporary file as it arrives, and can be read (played) from the start meanwhile
TEST(Collab, VeryBigFilesArriveInATemporaryFile)
{
	Node l_Host;
	Node l_Ana;
	l_Host.inlineAssetBytes = 256u << 10;
	const std::vector<uint8_t> l_Big = noise(static_cast<size_t>(wb::LARGE_ASSET_BYTES) + (3u << 20), 9);
	const wb::ObjectId l_Id = addPicture(l_Host.doc, l_Big, "movie.mp4");
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); }));

	// It is on the board from its first piece, readable up to what has arrived
	bool l_SawPartial = false;
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] {
		const wb::ImageAsset* l_Asset = assetOf(l_Ana.doc, l_Id);
		if (l_Asset == nullptr)
			return false;
		if (!l_Asset->complete())
		{
			l_SawPartial = true;
			EXPECT_LT(l_Asset->stream->available(), l_Asset->size());
			uint8_t l_First = 0;
			EXPECT_EQ(l_Asset->read(0, &l_First, 1), 1u); // the start is there as soon as there is a piece
			EXPECT_EQ(l_First, l_Big[0]);
		}
		return l_Asset->complete();
	}, 30000));
	(void)l_SawPartial;
	const wb::ImageAsset* l_Asset = assetOf(l_Ana.doc, l_Id);
	ASSERT_NE(l_Asset, nullptr);
	EXPECT_TRUE(l_Asset->stream != nullptr);
	EXPECT_TRUE(l_Asset->bytes.empty());
	EXPECT_TRUE(l_Asset->stream->temporary());
	EXPECT_EQ(l_Asset->size(), l_Big.size());
	EXPECT_EQ(l_Asset->load(), l_Big);
	EXPECT_EQ(l_Ana.session->assetProgress().files, 0u);

	// What is held in a file is served on to others like any other file
	Node l_Bo;
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return assetOf(l_Bo.doc, l_Id) != nullptr && assetOf(l_Bo.doc, l_Id)->complete(); }, 30000));
	EXPECT_EQ(assetOf(l_Bo.doc, l_Id)->load(), l_Big);
}

TEST(Collab, ASenderLeavingMidTransferCancelsItsFile)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo); }));

	const wb::ObjectId l_Keep = l_Host.doc.allocateId();
	l_Host.doc.insert(makeStroke(l_Host.doc, 1.f));
	const wb::ObjectId l_Id = addPicture(l_Ana.doc, noise(24u << 20, 6), "huge.jpg");
	// As soon as the host knows about the picture and has asked for the file, the sender disappears
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] {
		if (l_Host.doc.find(l_Id) == nullptr || l_Host.session->assetProgress().files == 0)
			return false;
		l_Ana.session->leave();
		return true;
	}));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Bo }, [&] { return l_Host.doc.find(l_Id) == nullptr && l_Bo.doc.find(l_Id) == nullptr; }));
	EXPECT_EQ(l_Host.session->assetProgress().files, 0u);
	EXPECT_EQ(assetOf(l_Host.doc, l_Id), nullptr); // no half a file is left on the board
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Bo }, [&] { return l_Bo.session->assetProgress().files == 0; })); // Bo stops waiting for it too
	EXPECT_EQ(l_Host.doc.size(), 1u); // the other things on the board stay
	EXPECT_EQ(l_Bo.doc.size(), 1u);
	(void)l_Keep;
	bool l_Told = false;
	for (const std::string& l_Message : l_Host.messages)
		l_Told = l_Told || l_Message.find("finished arriving") != std::string::npos;
	EXPECT_TRUE(l_Told);
}

// A saved board leaves out pictures whose file has not arrived, and still loads; a join snapshot keeps them
TEST(Collab, SavingSkipsUnfinishedPicturesButSnapshotsKeepThem)
{
	wb::Document l_Doc;
	l_Doc.insert(makeStroke(l_Doc, 1.f));
	auto l_Picture = std::make_unique<wb::Object>();
	l_Picture->id = l_Doc.allocateId();
	l_Picture->payload = wb::ImageData{ .asset = 99, .size = { 10.f, 10.f } };
	l_Doc.insert(std::move(l_Picture));
	addPicture(l_Doc, noise(1000, 7), "ok.png");
	ASSERT_EQ(l_Doc.size(), 3u);

	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::deserializeBoard(wb::serializeBoard(l_Doc, wb::BoardMeta{}), l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	EXPECT_EQ(l_Loaded.size(), 2u); // the stroke and the picture that is complete

	wb::Document l_Joiner;
	const wb::LoadResult l_Snapshot = wb::deserializeBoard(wb::serializeBoardShell(l_Doc, wb::BoardMeta{}, 1u << 20), l_Joiner, l_Meta, true);
	ASSERT_TRUE(l_Snapshot.ok) << l_Snapshot.error;
	EXPECT_EQ(l_Joiner.size(), 3u);
}

// Undo must not bring back a picture whose file was cancelled; a finished one keeps its file, so undo is instant
TEST(Collab, UndoAfterDeletingPicturesDependsOnTheirFile)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_Done = addPicture(l_Doc, noise(500, 8), "done.png");
	auto l_Pending = std::make_unique<wb::Object>();
	l_Pending->id = l_Doc.allocateId();
	l_Pending->payload = wb::ImageData{ .asset = 4242, .size = { 10.f, 10.f } }; // its file never arrived
	const wb::ObjectId l_PendingId = l_Pending->id;
	l_Doc.insert(std::move(l_Pending));
	const wb::AssetId l_DoneAsset = l_Doc.find(l_Done)->image()->asset;

	l_History.execute(l_Doc, std::make_unique<wb::RemoveObjectsCommand>(std::vector<wb::ObjectId>{ l_Done }, "Delete"));
	l_History.execute(l_Doc, std::make_unique<wb::RemoveObjectsCommand>(std::vector<wb::ObjectId>{ l_PendingId }, "Delete"));
	ASSERT_EQ(l_Doc.size(), 0u);

	// The unfinished one's transfer is cancelled: its delete can no longer be undone
	EXPECT_EQ(l_History.invalidateAsset(4242), 1u);
	EXPECT_FALSE(l_History.undo(l_Doc) && l_Doc.find(l_PendingId) != nullptr);
	EXPECT_EQ(l_Doc.find(l_PendingId), nullptr);

	// The finished one still has its file in memory
	EXPECT_NE(l_Doc.findAsset(l_DoneAsset), nullptr);
}

// A file nobody is sending is stalled; deleting its object and dragging the same file in again is a fresh start: it is
// sent from the beginning
TEST(Collab, DraggingTheSameFileInAgainStartsFresh)
{
	Node l_Host;
	Node l_Ana;
	Node l_Bo;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	l_Bo.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Bo");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return joinedAndOnline(l_Ana) && joinedAndOnline(l_Bo); }));

	const std::vector<uint8_t> l_Bytes = noise(24u << 20, 11);
	const wb::ObjectId l_Id = addPicture(l_Ana.doc, l_Bytes, "photo.jpg");
	const wb::AssetId l_Asset = l_Ana.doc.find(l_Id)->image()->asset;
	std::unique_ptr<wb::Object> l_Taken;
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] {
		if (l_Host.doc.find(l_Id) == nullptr || l_Host.session->assetProgress().files == 0)
			return false;
		l_Taken = std::move(l_Host.doc.take(l_Id).object);
		return true;
	}));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.session->assetProgress().files == 0 && l_Bo.doc.find(l_Id) == nullptr; }));
	l_Host.doc.insert(std::move(l_Taken));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return l_Host.session->assetStalled(l_Asset) && l_Bo.session->assetStalled(l_Asset); }));

	// Ana deletes the object and drags the same file in once more
	(void)l_Ana.doc.take(l_Id);
	const wb::ObjectId l_Again = addPicture(l_Ana.doc, l_Bytes, "photo.jpg");
	ASSERT_EQ(l_Ana.doc.find(l_Again)->image()->asset, l_Asset); // the same file, found by its content
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana, &l_Bo }, [&] { return hasWhole(l_Host.doc, l_Asset) && hasWhole(l_Bo.doc, l_Asset); }, 40000));
	EXPECT_FALSE(l_Host.session->assetStalled(l_Asset));
	EXPECT_FALSE(l_Bo.session->assetStalled(l_Asset));
	EXPECT_EQ(l_Host.doc.findAsset(l_Asset)->load(), l_Bytes);
}

// The owner deletes their own unfinished picture and undoes: the file is sent again from the beginning
TEST(Collab, TheOwnersOwnUndoRestartsTheTransfer)
{
	Node l_Host;
	Node l_Ana;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Hosty", l_Error)) << l_Error;
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "secret", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); }));

	const std::vector<uint8_t> l_Bytes = noise(24u << 20, 10);
	const wb::ObjectId l_Id = addPicture(l_Ana.doc, l_Bytes, "photo.jpg");
	const wb::AssetId l_Asset = l_Ana.doc.find(l_Id)->image()->asset;
	std::unique_ptr<wb::Object> l_Taken;
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] {
		if (l_Host.doc.find(l_Id) == nullptr || l_Host.session->assetProgress().files == 0)
			return false;
		l_Taken = std::move(l_Ana.doc.take(l_Id).object);
		return true;
	}));
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return l_Host.doc.find(l_Id) == nullptr && l_Host.session->assetProgress().files == 0; }));
	l_Ana.doc.insert(std::move(l_Taken)); // undo
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return hasWhole(l_Host.doc, l_Asset); }, 30000));
	EXPECT_EQ(l_Host.doc.findAsset(l_Asset)->load(), l_Bytes);
	EXPECT_FALSE(l_Host.session->assetStalled(l_Asset));
}

TEST(Collab, WrongPasswordIsRefusedAndNothingChanges)
{
	Node l_Host;
	Node l_Mallory;
	l_Host.doc.insert(makeStroke(l_Host.doc, 1.f));
	l_Mallory.doc.insert(makeStroke(l_Mallory.doc, 9.f));
	const std::vector<wb::ObjectId> l_Mine = idsOf(l_Mallory.doc);

	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Host", l_Error));
	l_Mallory.session->join("127.0.0.1", l_Host.session->listenPort(), "guess", "Mallory");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Mallory }, [&] { return !l_Mallory.session->active(); }));
	ASSERT_FALSE(l_Mallory.errors.empty());
	EXPECT_NE(l_Mallory.errors.back().find("Wrong password"), std::string::npos);
	EXPECT_EQ(idsOf(l_Mallory.doc), l_Mine); // the board stays as it was
	EXPECT_EQ(l_Host.session->users().size(), 1u);
}

TEST(Collab, CannotConnectToNothing)
{
	Node l_Lonely;
	// A port nothing listens on: grab a free one and release it
	wb::net::Socket l_Probe;
	std::string l_Error;
	l_Probe = wb::net::Socket::listenTcp(0, l_Error);
	ASSERT_TRUE(l_Probe.valid());
	const uint16_t l_Port = l_Probe.localPort();
	l_Probe.close();

	l_Lonely.session->join("127.0.0.1", l_Port, "x", "Me");
	ASSERT_TRUE(pumpUntil({ &l_Lonely }, [&] { return !l_Lonely.session->active(); }, 12000));
	ASSERT_FALSE(l_Lonely.errors.empty());
	// The message says why, and the log records the address that was tried
	EXPECT_NE(l_Lonely.errors.back().find("refused"), std::string::npos) << l_Lonely.errors.back();
	const auto& l_Log = l_Lonely.session->diagnostics();
	const bool l_Tried = std::ranges::any_of(l_Log, [](const std::string& p_Line) { return p_Line.find("127.0.0.1") != std::string::npos; });
	EXPECT_TRUE(l_Tried);
}

TEST(Collab, HostLogsWhoConnectsAndWhy)
{
	Node l_Host;
	Node l_Mallory;
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Host", l_Error));
	l_Mallory.session->join("127.0.0.1", l_Host.session->listenPort(), "guess", "Mallory");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Mallory }, [&] { return !l_Mallory.session->active(); }));
	const auto has = [&](const char* p_Text)
	{
		return std::ranges::any_of(l_Host.session->diagnostics(), [&](const std::string& p_Line) { return p_Line.find(p_Text) != std::string::npos; });
	};
	EXPECT_TRUE(has("Incoming connection"));
	EXPECT_TRUE(has("Rejected"));
	EXPECT_TRUE(has("Wrong password"));
}

TEST(Collab, ClientsCannotForgeIdsOrWipeTheBoard)
{
	Node l_Host;
	Node l_Ana;
	l_Host.doc.insert(makeStroke(l_Host.doc, 1.f));
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "", "Host", l_Error));
	l_Ana.session->join("127.0.0.1", l_Host.session->listenPort(), "", "Ana");
	ASSERT_TRUE(pumpUntil({ &l_Host, &l_Ana }, [&] { return joinedAndOnline(l_Ana); }));

	// Ana's board is cleared locally (a "new board" while connected): the host must not follow
	l_Ana.doc.clear();
	// ...and an object with an id from the host's slot is not accepted either
	auto l_Forged = makeStroke(l_Ana.doc, 3.f);
	l_Forged->id = 0x1234; // slot 0
	l_Ana.doc.insert(std::move(l_Forged));
	pumpUntil({ &l_Host, &l_Ana }, [] { return false; }, 400);
	EXPECT_EQ(l_Host.doc.size(), 1u);
	EXPECT_EQ(l_Host.doc.find(0x1234), nullptr);
}

namespace
{
// Somebody between a client and a host who completes a key exchange with each (they can read and change everything on
// the way) but does not know the password
struct ManInTheMiddle
{
	wb::net::Socket listener;
	std::unique_ptr<wb::net::Connection> towardClient;
	std::unique_ptr<wb::net::Connection> towardHost;
	std::atomic<int> forwarded{ 0 };
	std::string hostSaid;

	bool start(const uint16_t p_HostPort)
	{
		using wb::net::Connection;
		std::string l_Error;
		listener = wb::net::Socket::listenTcp(0, l_Error);
		if (!listener.valid())
			return false;
		std::atomic<bool> l_Cancel{ false };
		wb::net::Socket l_ToHost = wb::net::Socket::connectTcp("127.0.0.1", p_HostPort, 3000, l_Cancel, l_Error);
		if (!l_ToHost.valid())
			return false;
		towardHost = std::make_unique<Connection>(std::move(l_ToHost), Connection::Role::Client, [this](wb::net::Frame p_Frame)
		{
			++forwarded;
			if (p_Frame.type == static_cast<uint8_t>(wb::net::MsgType::Reject))
			{
				wb::net::Reject l_Reject;
				if (wb::net::decode(p_Frame.payload, l_Reject))
					hostSaid = l_Reject.reason;
			}
			if (towardClient)
				towardClient->send(p_Frame.type, std::move(p_Frame.payload));
		}, [](const std::string&) {});
		towardHost->setMaxFrame(1u << 24);
		return true;
	}
	void accept()
	{
		using wb::net::Connection;
		wb::net::Socket l_FromClient = listener.accept(3000);
		ASSERT_TRUE(l_FromClient.valid());
		towardClient = std::make_unique<Connection>(std::move(l_FromClient), Connection::Role::Server, [this](wb::net::Frame p_Frame)
		{
			++forwarded;
			towardHost->send(p_Frame.type, std::move(p_Frame.payload));
		}, [](const std::string&) {});
		towardClient->setMaxFrame(1u << 24);
		towardClient->start();
		towardHost->start();
	}
};
} // namespace

TEST(Collab, SomebodyInTheMiddleWhoDoesNotKnowThePasswordCannotJoinThemIn)
{
	Node l_Host;
	Node l_Ana;
	l_Host.doc.insert(makeStroke(l_Host.doc, 1.f));
	std::string l_Error;
	ASSERT_TRUE(l_Host.session->host(0, "secret", "Host", l_Error));

	ManInTheMiddle l_Mallory;
	ASSERT_TRUE(l_Mallory.start(l_Host.session->listenPort()));
	l_Ana.session->join("127.0.0.1", l_Mallory.listener.localPort(), "secret", "Ana"); // the right password, the wrong road
	std::thread l_Accept([&] { l_Mallory.accept(); });
	const bool l_Failed = pumpUntil({ &l_Host, &l_Ana }, [&] { return !l_Ana.session->active(); });
	l_Accept.join();
	ASSERT_TRUE(l_Failed);
	// The host saw a proof for a different key exchange and refused it; nothing of the board reached Ana
	EXPECT_EQ(l_Mallory.hostSaid, "Wrong password");
	EXPECT_EQ(l_Host.session->users().size(), 1u);
	EXPECT_TRUE(l_Ana.doc.empty());
}

TEST(Collab, AHostThatDoesNotKnowThePasswordIsNotTrusted)
{
	using wb::net::Connection;
	std::string l_Error;
	wb::net::Socket l_Listener = wb::net::Socket::listenTcp(0, l_Error);
	ASSERT_TRUE(l_Listener.valid());

	// A fake host: completes the key exchange, accepts any proof and welcomes the client with a made-up proof of its own
	std::unique_ptr<Connection> l_Fake;
	std::atomic<bool> l_SawHello{ false };
	Node l_Ana;
	l_Ana.session->join("127.0.0.1", l_Listener.localPort(), "secret", "Ana");
	wb::net::Socket l_Accepted;
	for (int i = 0; i < 100 && !l_Accepted.valid(); ++i)
		l_Accepted = l_Listener.accept(100);
	ASSERT_TRUE(l_Accepted.valid());
	l_Fake = std::make_unique<Connection>(std::move(l_Accepted), Connection::Role::Server, [&](wb::net::Frame p_Frame)
	{
		if (p_Frame.type != static_cast<uint8_t>(wb::net::MsgType::Hello))
			return;
		l_SawHello = true;
		wb::net::Welcome l_Welcome;
		l_Welcome.slot = 1;
		l_Welcome.users = { wb::net::UserInfo{ .slot = 0, .name = "Fake", .color = 0xFFFFFFFFu, .host = true }, wb::net::UserInfo{ .slot = 1, .name = "Ana", .color = 0xFFFFFFFFu, .host = false } };
		l_Fake->send(static_cast<uint8_t>(wb::net::MsgType::Welcome), wb::net::encode(l_Welcome)); // proof stays zero
	}, [](const std::string&) {});
	l_Fake->setMaxFrame(1u << 20);
	l_Fake->start();
	wb::net::Challenge l_Challenge;
	l_Fake->send(static_cast<uint8_t>(wb::net::MsgType::Challenge), wb::net::encode(l_Challenge));

	ASSERT_TRUE(pumpUntil({ &l_Ana }, [&] { return !l_Ana.session->active(); }));
	EXPECT_TRUE(l_SawHello.load());
	ASSERT_FALSE(l_Ana.errors.empty());
	EXPECT_NE(l_Ana.errors.back().find("prove it knows the password"), std::string::npos) << l_Ana.errors.back();
}
