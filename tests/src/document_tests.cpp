#include <algorithm>
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
import wb.doc.commands;

namespace
{
std::unique_ptr<wb::Object> makeStroke(wb::Document& p_Document, const float p_X)
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate({ static_cast<double>(p_X), 0.0 });
	l_Object->payload = wb::StrokeData{
		.style = {},
		.points = { { .position = { 0.f, 0.f }, .radius = 1.f }, { .position = { 10.f, 0.f }, .radius = 2.f } },
	};
	return l_Object;
}

std::vector<wb::ObjectId> order(const wb::Document& p_Document)
{
	std::vector<wb::ObjectId> l_Ids;
	for (const auto& l_Object : p_Document.objects())
		l_Ids.push_back(l_Object->id);
	return l_Ids;
}

struct CountingListener final : wb::DocumentListener
{
	int added = 0;
	int removed = 0;
	int changed = 0;
	void onObjectAdded(const wb::Object&) override { ++added; }
	void onObjectRemoved(const wb::Object&) override { ++removed; }
	void onObjectChanged(const wb::Object&, uint32_t) override { ++changed; }
	void onDocumentCleared() override {}
};
} // namespace

TEST(Document, InsertFindTakeNotify)
{
	wb::Document l_Doc;
	CountingListener l_Listener;
	l_Doc.addListener(&l_Listener);

	auto l_A = makeStroke(l_Doc, 0.f);
	const wb::ObjectId l_Id = l_A->id;
	l_Doc.insert(std::move(l_A));
	ASSERT_NE(l_Doc.find(l_Id), nullptr);
	EXPECT_EQ(l_Doc.indexOf(l_Id), 0u);
	EXPECT_EQ(l_Listener.added, 1);

	l_Doc.modify(l_Id, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 5.0, 5.0 }); });
	EXPECT_EQ(l_Doc.find(l_Id)->version, 1u);
	EXPECT_EQ(l_Listener.changed, 1);

	wb::Document::Removed l_Removed = l_Doc.take(l_Id);
	ASSERT_TRUE(l_Removed.object);
	EXPECT_EQ(l_Removed.zIndex, 0u);
	EXPECT_EQ(l_Doc.find(l_Id), nullptr);
	EXPECT_EQ(l_Listener.removed, 1);
	l_Doc.removeListener(&l_Listener);
}

TEST(Document, WorldBoundsIncludeRadius)
{
	wb::Document l_Doc;
	auto l_A = makeStroke(l_Doc, 100.f);
	const wb::ObjectId l_Id = l_A->id;
	l_Doc.insert(std::move(l_A));
	const wb::Rect l_Bounds = l_Doc.find(l_Id)->worldBounds();
	EXPECT_DOUBLE_EQ(l_Bounds.min.x, 99.0);  // first point radius 1
	EXPECT_DOUBLE_EQ(l_Bounds.max.x, 112.0); // second point radius 2
	EXPECT_DOUBLE_EQ(l_Bounds.max.y, 2.0);

	// Bounds follow modifications
	l_Doc.modify(l_Id, [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 0.0, 50.0 }); });
	EXPECT_DOUBLE_EQ(l_Doc.find(l_Id)->worldBounds().min.y, 48.0);
}

TEST(History, AddUndoRedoKeepsOrder)
{
	wb::Document l_Doc;
	wb::History l_History;
	std::vector<wb::ObjectId> l_Expected;
	for (int i = 0; i < 3; ++i)
	{
		std::vector<std::unique_ptr<wb::Object>> l_Objects;
		l_Objects.push_back(makeStroke(l_Doc, static_cast<float>(i)));
		l_Expected.push_back(l_Objects.back()->id);
		l_History.execute(l_Doc, std::make_unique<wb::AddObjectsCommand>(std::move(l_Objects), "Draw"));
	}
	EXPECT_EQ(order(l_Doc), l_Expected);

	EXPECT_TRUE(l_History.undo(l_Doc));
	EXPECT_EQ(l_Doc.size(), 2u);
	EXPECT_TRUE(l_History.redo(l_Doc));
	EXPECT_EQ(order(l_Doc), l_Expected);
	EXPECT_FALSE(l_History.redo(l_Doc));
}

TEST(History, RemoveRestoresZOrder)
{
	wb::Document l_Doc;
	wb::History l_History;
	std::vector<wb::ObjectId> l_Ids;
	for (int i = 0; i < 5; ++i)
	{
		auto l_Object = makeStroke(l_Doc, static_cast<float>(i));
		l_Ids.push_back(l_Object->id);
		l_Doc.insert(std::move(l_Object));
	}
	const std::vector<wb::ObjectId> l_Before = order(l_Doc);

	l_History.execute(l_Doc, std::make_unique<wb::RemoveObjectsCommand>(std::vector<wb::ObjectId>{ l_Ids[3], l_Ids[1], l_Ids[4] }, "Delete"));
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_Ids[0], l_Ids[2] }));

	l_History.undo(l_Doc);
	EXPECT_EQ(order(l_Doc), l_Before);
	l_History.redo(l_Doc);
	EXPECT_EQ(order(l_Doc), (std::vector<wb::ObjectId>{ l_Ids[0], l_Ids[2] }));
	l_History.undo(l_Doc);
	EXPECT_EQ(order(l_Doc), l_Before);
}

TEST(History, NewCommandClearsRedoAndStateTracksSavePoint)
{
	wb::Document l_Doc;
	wb::History l_History;
	const uint64_t l_Clean = l_History.stateId();

	std::vector<std::unique_ptr<wb::Object>> l_First;
	l_First.push_back(makeStroke(l_Doc, 0.f));
	l_History.execute(l_Doc, std::make_unique<wb::AddObjectsCommand>(std::move(l_First), "Draw"));
	const uint64_t l_Saved = l_History.stateId();
	EXPECT_NE(l_Saved, l_Clean);

	l_History.undo(l_Doc);
	EXPECT_EQ(l_History.stateId(), l_Clean);
	l_History.redo(l_Doc);
	EXPECT_EQ(l_History.stateId(), l_Saved);

	l_History.undo(l_Doc);
	std::vector<std::unique_ptr<wb::Object>> l_Second;
	l_Second.push_back(makeStroke(l_Doc, 1.f));
	l_History.execute(l_Doc, std::make_unique<wb::AddObjectsCommand>(std::move(l_Second), "Draw"));
	EXPECT_FALSE(l_History.canRedo());
	EXPECT_NE(l_History.stateId(), l_Saved);
}

TEST(History, CapacityDropsOldest)
{
	wb::Document l_Doc;
	wb::History l_History(2);
	for (int i = 0; i < 4; ++i)
	{
		std::vector<std::unique_ptr<wb::Object>> l_Objects;
		l_Objects.push_back(makeStroke(l_Doc, static_cast<float>(i)));
		l_History.execute(l_Doc, std::make_unique<wb::AddObjectsCommand>(std::move(l_Objects), "Draw"));
	}
	EXPECT_EQ(l_History.undoCount(), 2u);
	EXPECT_TRUE(l_History.undo(l_Doc));
	EXPECT_TRUE(l_History.undo(l_Doc));
	EXPECT_FALSE(l_History.undo(l_Doc));
	EXPECT_EQ(l_Doc.size(), 2u);
}

namespace
{
void expectBoundsMatch(const wb::Document& p_Document)
{
	const std::span<const wb::Rect> l_Bounds = p_Document.boundsList();
	ASSERT_EQ(l_Bounds.size(), p_Document.size());
	for (size_t i = 0; i < l_Bounds.size(); ++i)
	{
		EXPECT_EQ(l_Bounds[i].min, p_Document.objects()[i]->worldBounds().min) << "slot " << i;
		EXPECT_EQ(l_Bounds[i].max, p_Document.objects()[i]->worldBounds().max) << "slot " << i;
	}
}
} // namespace

TEST(Document, BoundsListStaysInStepWithTheObjects)
{
	wb::Document l_Doc;
	expectBoundsMatch(l_Doc);
	std::vector<wb::ObjectId> l_Ids;
	for (int i = 0; i < 10; ++i)
	{
		auto l_Object = makeStroke(l_Doc, static_cast<float>(i) * 100.f);
		l_Ids.push_back(l_Object->id);
		l_Doc.insert(std::move(l_Object), i % 3 == 0 ? 0 : SIZE_MAX);
		expectBoundsMatch(l_Doc);
	}

	// A few moved objects are patched, many are rebuilt
	l_Doc.modify(l_Ids[4], [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 500.0, 500.0 }); });
	expectBoundsMatch(l_Doc);
	for (int round = 0; round < 3; ++round)
	{
		for (const wb::ObjectId l_Id : l_Ids)
			l_Doc.modify(l_Id, [&](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ static_cast<double>(round) * 7.0, static_cast<double>(l_Id) }); });
		for (int k = 0; k < 6; ++k)
			l_Doc.modify(l_Ids[static_cast<size_t>(k)], [&](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 1.0, static_cast<double>(k) }); });
		expectBoundsMatch(l_Doc);
	}

	// Edit, then remove before anything reads the list
	l_Doc.modify(l_Ids[2], [](wb::Object& p_Object) { p_Object.transform = wb::Affine2::translate({ 9.0, 9.0 }); });
	(void)l_Doc.take(l_Ids[2]);
	expectBoundsMatch(l_Doc);

	l_Doc.moveTo(l_Ids[0], 5);
	expectBoundsMatch(l_Doc);
	std::vector<wb::ObjectId> l_Reversed = order(l_Doc);
	std::ranges::reverse(l_Reversed);
	l_Doc.setOrder(l_Reversed);
	expectBoundsMatch(l_Doc);
	l_Doc.clear();
	expectBoundsMatch(l_Doc);
}
