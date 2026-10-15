#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.commands;
import wb.io.serializer;
import wb.net.wire;
import wb.net.ops;
import wb.net.replicator;

namespace
{
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

std::unique_ptr<wb::Object> makeText(wb::Document& p_Document, const char* p_Text)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	wb::TextData l_Text;
	l_Text.text = p_Text;
	l_Text.family = "Inter";
	l_Text.fontSize = 24.f;
	l_Text.size = { 100.f, 30.f };
	l_Object->payload = std::move(l_Text);
	return l_Object;
}

std::vector<wb::ObjectId> order(const wb::Document& p_Document)
{
	std::vector<wb::ObjectId> l_Ids;
	for (const auto& l_Object : p_Document.objects())
		l_Ids.push_back(l_Object->id);
	return l_Ids;
}

// Two documents joined by in-memory queues, the way a host and a client are
struct Pair
{
	wb::Document a;
	wb::Document b;
	std::deque<wb::net::Op> toB;
	std::deque<wb::net::Op> toA;
	wb::net::Replicator repA{ a, [this](wb::net::Op p_Op) { toB.push_back(std::move(p_Op)); } };
	wb::net::Replicator repB{ b, [this](wb::net::Op p_Op) { toA.push_back(std::move(p_Op)); } };

	Pair()
	{
		a.setIdSlot(0);
		b.setIdSlot(1);
	}

	// Sends every queued op over the wire format and applies it on the other side
	void flush()
	{
		const auto l_Deliver = [](std::deque<wb::net::Op>& p_Queue, wb::net::Replicator& p_Target)
		{
			while (!p_Queue.empty())
			{
				wb::net::ByteWriter l_Writer;
				wb::net::encodeOp(l_Writer, p_Queue.front());
				p_Queue.pop_front();
				wb::net::ByteReader l_Reader(l_Writer.bytes());
				wb::net::Op l_Decoded;
				ASSERT_TRUE(wb::net::decodeOp(l_Reader, l_Decoded));
				EXPECT_EQ(l_Reader.remaining(), 0u);
				EXPECT_TRUE(p_Target.applyRemote(l_Decoded).ok);
			}
		};
		l_Deliver(toB, repB);
		l_Deliver(toA, repA);
	}
};

void expectSameBoard(const wb::Document& p_A, const wb::Document& p_B)
{
	ASSERT_EQ(order(p_A), order(p_B));
	for (const auto& l_Object : p_A.objects())
	{
		const wb::Object* l_Other = p_B.find(l_Object->id);
		ASSERT_NE(l_Other, nullptr);
		EXPECT_EQ(wb::encodeObject(*l_Object), wb::encodeObject(*l_Other));
		EXPECT_EQ(l_Object->locked, l_Other->locked);
		EXPECT_EQ(l_Object->group, l_Other->group);
	}
}
} // namespace

TEST(Net, ObjectRecordRoundTrips)
{
	wb::Document l_Doc;
	auto l_Stroke = makeStroke(l_Doc, 4.f);
	const std::vector<uint8_t> l_Bytes = wb::encodeObject(*l_Stroke);

	std::unique_ptr<wb::Object> l_Decoded;
	size_t l_Consumed = 0;
	ASSERT_EQ(wb::decodeObject(l_Bytes, l_Decoded, &l_Consumed), wb::ObjectDecode::Ok);
	EXPECT_EQ(l_Consumed, l_Bytes.size());
	EXPECT_EQ(l_Decoded->id, l_Stroke->id);
	EXPECT_EQ(wb::encodeObject(*l_Decoded), l_Bytes);

	// Truncated records are corrupt, never a crash
	for (size_t l_Cut = 0; l_Cut < l_Bytes.size(); ++l_Cut)
		EXPECT_EQ(wb::decodeObject(std::span<const uint8_t>(l_Bytes).first(l_Cut), l_Decoded), wb::ObjectDecode::Corrupt);
}

TEST(Net, OpsSurviveTheWireFormat)
{
	wb::Document l_Doc;
	std::vector<wb::net::Op> l_Ops;
	l_Ops.emplace_back(wb::net::AddOp{ .below = 7, .locked = true, .object = wb::encodeObject(*makeStroke(l_Doc, 1.f)) });
	l_Ops.emplace_back(wb::net::RemoveOp{ .id = 9 });
	l_Ops.emplace_back(wb::net::TransformOp{ .id = 3, .transform = wb::Affine2::translate({ 5.0, -2.0 }) });
	l_Ops.emplace_back(wb::net::UpdateOp{ .object = wb::encodeObject(*makeText(l_Doc, "hi")) });
	l_Ops.emplace_back(wb::net::LockOp{ .id = 4, .locked = true });
	l_Ops.emplace_back(wb::net::GroupOp{ .id = 4, .group = 11 });
	l_Ops.emplace_back(wb::net::OrderOp{ .ids = { 3, 1, 2 } });
	l_Ops.emplace_back(wb::net::ClearOp{});

	for (const wb::net::Op& l_Op : l_Ops)
	{
		wb::net::ByteWriter l_Writer;
		wb::net::encodeOp(l_Writer, l_Op);
		wb::net::ByteReader l_Reader(l_Writer.bytes());
		wb::net::Op l_Decoded;
		ASSERT_TRUE(wb::net::decodeOp(l_Reader, l_Decoded));
		EXPECT_EQ(l_Decoded.index(), l_Op.index());

		wb::net::ByteWriter l_Again;
		wb::net::encodeOp(l_Again, l_Decoded);
		EXPECT_EQ(std::vector<uint8_t>(l_Again.bytes().begin(), l_Again.bytes().end()), std::vector<uint8_t>(l_Writer.bytes().begin(), l_Writer.bytes().end()));

		// Every strict prefix of an op is malformed
		for (size_t l_Cut = 0; l_Cut < l_Writer.size(); ++l_Cut)
		{
			wb::net::ByteReader l_Short(l_Writer.bytes().first(l_Cut));
			wb::net::Op l_Ignored;
			EXPECT_FALSE(wb::net::decodeOp(l_Short, l_Ignored));
		}
	}
}

TEST(Net, RejectsHostileOps)
{
	// Unknown kind
	const uint8_t l_Unknown[] = { 200 };
	wb::net::ByteReader l_UnknownReader(l_Unknown);
	wb::net::Op l_Op;
	EXPECT_FALSE(wb::net::decodeOp(l_UnknownReader, l_Op));

	// An order list claiming far more ids than the message holds
	wb::net::ByteWriter l_Writer;
	l_Writer.u8(6);
	l_Writer.u32(0x7fffffff);
	wb::net::ByteReader l_Huge(l_Writer.bytes());
	EXPECT_FALSE(wb::net::decodeOp(l_Huge, l_Op));

	// A non-finite transform
	wb::net::ByteWriter l_Nan;
	l_Nan.u8(3);
	l_Nan.u64(1);
	for (int i = 0; i < 6; ++i)
		l_Nan.f64(i == 4 ? std::numeric_limits<double>::quiet_NaN() : 1.0);
	wb::net::ByteReader l_NanReader(l_Nan.bytes());
	EXPECT_FALSE(wb::net::decodeOp(l_NanReader, l_Op));

	// Applying garbage object bytes, a duplicate add and unknown targets changes nothing
	wb::Document l_Doc;
	EXPECT_FALSE(wb::net::applyOp(l_Doc, wb::net::AddOp{ .object = { 1, 2, 3 } }).ok);
	EXPECT_FALSE(wb::net::applyOp(l_Doc, wb::net::RemoveOp{ .id = 5 }).ok);
	EXPECT_FALSE(wb::net::applyOp(l_Doc, wb::net::TransformOp{ .id = 5 }).ok);
	EXPECT_FALSE(wb::net::applyOp(l_Doc, wb::net::LockOp{ .id = 5, .locked = true }).ok);
	auto l_Stroke = makeStroke(l_Doc, 1.f);
	const std::vector<uint8_t> l_Record = wb::encodeObject(*l_Stroke);
	EXPECT_TRUE(wb::net::applyOp(l_Doc, wb::net::AddOp{ .object = l_Record }).ok);
	EXPECT_FALSE(wb::net::applyOp(l_Doc, wb::net::AddOp{ .object = l_Record }).ok);
	EXPECT_EQ(l_Doc.size(), 1u);
}

TEST(Net, ReplicatorKeepsTwoDocumentsEqual)
{
	Pair l_Pair;
	auto l_S1 = makeStroke(l_Pair.a, 1.f);
	auto l_S2 = makeStroke(l_Pair.a, 2.f);
	const wb::ObjectId l_Id1 = l_S1->id;
	const wb::ObjectId l_Id2 = l_S2->id;
	l_Pair.a.insert(std::move(l_S1));
	l_Pair.a.insert(std::move(l_S2));
	l_Pair.flush();
	auto l_Text = makeText(l_Pair.b, "hello");
	const wb::ObjectId l_TextId = l_Text->id;
	l_Pair.b.insert(std::move(l_Text), 0);
	l_Pair.flush();
	expectSameBoard(l_Pair.a, l_Pair.b);
	EXPECT_EQ(l_Pair.a.size(), 3u);

	// Ids from the two peers never collide
	EXPECT_NE(wb::idSlotOf(l_Id1), wb::idSlotOf(l_TextId));

	l_Pair.b.modify(l_Id1, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 40.0, 2.0 }); }, wb::ObjectChange::Transform);
	l_Pair.a.modify(l_TextId, [](wb::Object& p_Object) { std::get<wb::TextData>(p_Object.payload).text = "changed"; }, wb::ObjectChange::Geometry);
	l_Pair.a.modify(l_Id2, [](wb::Object& p_Object) { p_Object.locked = true; }, wb::ObjectChange::Lock);
	l_Pair.a.modify(l_Id1, [](wb::Object& p_Object) { p_Object.group = 77; }, wb::ObjectChange::Group);
	l_Pair.flush();
	expectSameBoard(l_Pair.a, l_Pair.b);
	EXPECT_TRUE(l_Pair.b.find(l_Id2)->locked);
	EXPECT_EQ(l_Pair.b.find(l_Id1)->group, 77u);

	l_Pair.b.modify(l_Id1, [](wb::Object& p_Object) { p_Object.note = "explained"; }, wb::ObjectChange::Note);
	l_Pair.flush();
	EXPECT_EQ(l_Pair.a.find(l_Id1)->note, "explained");
	EXPECT_EQ(std::get<wb::TextData>(l_Pair.b.find(l_TextId)->payload).text, "changed");

	l_Pair.b.moveTo(l_Id1, 2);
	l_Pair.flush();
	expectSameBoard(l_Pair.a, l_Pair.b);
	EXPECT_EQ(order(l_Pair.a).back(), l_Id1);

	(void)l_Pair.a.take(l_Id2);
	l_Pair.flush();
	expectSameBoard(l_Pair.a, l_Pair.b);
	EXPECT_EQ(l_Pair.b.size(), 2u);

	l_Pair.b.clear();
	l_Pair.flush();
	EXPECT_TRUE(l_Pair.a.empty());
}

TEST(Net, UndoReplicatesAndRemoteEditsStayOutOfTheHistory)
{
	Pair l_Pair;
	wb::History l_HistoryA;
	l_Pair.repA.setRemoteChangeHandler([&](std::span<const wb::ObjectId> p_Ids, bool p_Removed) { l_HistoryA.invalidate(p_Ids, p_Removed); });

	std::vector<std::unique_ptr<wb::Object>> l_Added;
	l_Added.push_back(makeStroke(l_Pair.a, 1.f));
	const wb::ObjectId l_Id = l_Added.front()->id;
	l_HistoryA.execute(l_Pair.a, std::make_unique<wb::AddObjectsCommand>(std::move(l_Added), "Draw"));
	l_Pair.flush();
	ASSERT_EQ(l_Pair.b.size(), 1u);

	// A's own undo is sent to B like any other edit
	EXPECT_TRUE(l_HistoryA.undo(l_Pair.a));
	l_Pair.flush();
	EXPECT_TRUE(l_Pair.b.empty());
	EXPECT_TRUE(l_HistoryA.redo(l_Pair.a));
	l_Pair.flush();
	EXPECT_EQ(l_Pair.b.size(), 1u);

	// A moves the stroke, B moves it too: A's undo must not overwrite B's move
	l_HistoryA.execute(l_Pair.a, std::make_unique<wb::TransformObjectsCommand>(std::vector{ wb::TransformObjectsCommand::Entry{ .id = l_Id, .before = l_Pair.a.find(l_Id)->transform, .after = wb::Affine2::translate({ 50.0, 0.0 }) } }, "Move"));
	l_Pair.flush();
	l_Pair.b.modify(l_Id, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 99.0, 0.0 }); }, wb::ObjectChange::Transform);
	l_Pair.flush();
	EXPECT_EQ(l_HistoryA.undoName(), "Draw");
	EXPECT_TRUE(l_HistoryA.undo(l_Pair.a)); // undoes the draw, not the stale move
	l_Pair.flush();
	EXPECT_TRUE(l_Pair.a.empty());
	EXPECT_TRUE(l_Pair.b.empty());
}

TEST(Net, OrderOpKeepsObjectsTheSenderDoesNotKnow)
{
	wb::Document l_Doc;
	l_Doc.insert(makeStroke(l_Doc, 1.f));
	l_Doc.insert(makeStroke(l_Doc, 2.f));
	l_Doc.insert(makeStroke(l_Doc, 3.f));
	const std::vector<wb::ObjectId> l_Before = order(l_Doc);

	// The sender knows only two of them, in reverse, and one that is already gone here
	EXPECT_TRUE(wb::net::applyOp(l_Doc, wb::net::OrderOp{ .ids = { l_Before[1], 9999, l_Before[0] } }).ok);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_Before[1], l_Before[0], l_Before[2] }));
}

TEST(Net, AddOpPlacesTheObjectAboveItsNeighbour)
{
	wb::Document l_Doc;
	l_Doc.insert(makeStroke(l_Doc, 1.f));
	l_Doc.insert(makeStroke(l_Doc, 2.f));
	const std::vector<wb::ObjectId> l_Before = order(l_Doc);

	wb::Document l_Other;
	l_Other.setIdSlot(2);
	const auto l_New = makeStroke(l_Other, 5.f);
	EXPECT_TRUE(wb::net::applyOp(l_Doc, wb::net::AddOp{ .below = l_Before[0], .object = wb::encodeObject(*l_New) }).ok);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_Before[0], l_New->id, l_Before[1] }));

	const auto l_Bottom = makeStroke(l_Other, 6.f);
	EXPECT_TRUE(wb::net::applyOp(l_Doc, wb::net::AddOp{ .below = wb::INVALID_OBJECT_ID, .object = wb::encodeObject(*l_Bottom) }).ok);
	EXPECT_EQ(order(l_Doc).front(), l_Bottom->id);
}

TEST(Net, ConcurrentAddsConvergeOnceTheHostOrderArrives)
{
	Pair l_Pair;
	l_Pair.a.insert(makeStroke(l_Pair.a, 1.f));
	l_Pair.b.insert(makeText(l_Pair.b, "mine"), 0); // both edit before hearing from the other side
	l_Pair.flush();
	EXPECT_EQ(l_Pair.a.size(), 2u);
	EXPECT_EQ(l_Pair.b.size(), 2u);

	// The host (A) is authoritative for stacking order: its order op settles the difference
	wb::net::OrderOp l_Host;
	l_Host.ids = order(l_Pair.a);
	EXPECT_TRUE(l_Pair.repB.applyRemote(l_Host).ok);
	EXPECT_EQ(order(l_Pair.a), order(l_Pair.b));
}

// The file follows the object (it is streamed separately), so a picture may exist before its asset does
TEST(Net, ObjectsMayArriveBeforeTheirAsset)
{
	wb::Document l_Sender;
	wb::Document l_Receiver;
	l_Sender.setIdSlot(1);
	auto l_Picture = std::make_unique<wb::Object>();
	l_Picture->id = l_Sender.allocateId();
	l_Picture->payload = wb::ImageData{ .asset = 77, .size = { 10.f, 10.f } };
	wb::net::AddOp l_Add{ .below = wb::INVALID_OBJECT_ID, .locked = false, .object = wb::encodeObject(*l_Picture) };

	EXPECT_TRUE(wb::net::applyOp(l_Receiver, l_Add).ok);
	EXPECT_EQ(l_Receiver.size(), 1u);
	EXPECT_EQ(l_Receiver.findAsset(77), nullptr);
}

