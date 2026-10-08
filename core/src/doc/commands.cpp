module;
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.doc.commands;

import wb.math;
import wb.doc.document;
import wb.doc.history;
import wb.doc.object;

namespace wb
{
AddObjectsCommand::AddObjectsCommand(std::vector<std::unique_ptr<Object>> p_Objects, std::string p_Name)
	: m_Name(std::move(p_Name))
{
	m_Entries.reserve(p_Objects.size());
	for (std::unique_ptr<Object>& l_Object : p_Objects)
	{
		const ObjectId l_Id = l_Object->id;
		m_Entries.push_back(Entry{ .id = l_Id, .object = std::move(l_Object) });
	}
}

void AddObjectsCommand::apply(Document& p_Document)
{
	// Ascending z-index restores every object exactly where it was
	for (Entry& l_Entry : m_Entries)
	{
		p_Document.insert(std::move(l_Entry.object), l_Entry.zIndex);
		l_Entry.zIndex = p_Document.indexOf(l_Entry.id);
	}
}

void AddObjectsCommand::revert(Document& p_Document)
{
	for (auto l_It = m_Entries.rbegin(); l_It != m_Entries.rend(); ++l_It)
	{
		Document::Removed l_Removed = p_Document.take(l_It->id);
		l_It->object = std::move(l_Removed.object);
		l_It->zIndex = l_Removed.zIndex;
	}
}

RemoveObjectsCommand::RemoveObjectsCommand(std::vector<ObjectId> p_Ids, std::string p_Name)
	: m_Ids(std::move(p_Ids)), m_Name(std::move(p_Name))
{
}

void RemoveObjectsCommand::apply(Document& p_Document)
{
	// Take from the top down so the recorded indices stay valid for re-insertion bottom-up
	std::vector<std::pair<size_t, ObjectId>> l_Order;
	l_Order.reserve(m_Ids.size());
	for (const ObjectId l_Id : m_Ids)
	{
		if (const size_t l_Index = p_Document.indexOf(l_Id); l_Index != SIZE_MAX)
			l_Order.emplace_back(l_Index, l_Id);
	}
	std::ranges::sort(l_Order, std::greater{});

	m_Removed.clear();
	for (const auto& [l_Index, l_Id] : l_Order)
	{
		Document::Removed l_Removed = p_Document.take(l_Id);
		if (l_Removed.object)
			m_Removed.push_back(std::move(l_Removed));
	}
	std::ranges::reverse(m_Removed);
}

void RemoveObjectsCommand::revert(Document& p_Document)
{
	for (Document::Removed& l_Removed : m_Removed)
		p_Document.insert(std::move(l_Removed.object), l_Removed.zIndex);
	m_Removed.clear();
}

void ReplaceObjectsCommand::replace(Document& p_Document, const ObjectId p_Id, std::vector<std::unique_ptr<Object>> p_Added)
{
	Document::Removed l_Removed = p_Document.take(p_Id);
	if (!l_Removed.object)
		return;

	Step l_Step{ .removedId = p_Id, .zIndex = l_Removed.zIndex, .removed = std::move(l_Removed.object) };
	size_t l_Index = l_Step.zIndex;
	for (std::unique_ptr<Object>& l_Object : p_Added)
	{
		l_Step.addedIds.push_back(l_Object->id);
		p_Document.insert(std::move(l_Object), l_Index++);
	}
	m_Steps.push_back(std::move(l_Step));
}

void ReplaceObjectsCommand::apply(Document& p_Document)
{
	for (Step& l_Step : m_Steps)
	{
		l_Step.removed = p_Document.take(l_Step.removedId).object;
		size_t l_Index = l_Step.zIndex;
		for (std::unique_ptr<Object>& l_Object : l_Step.added)
			p_Document.insert(std::move(l_Object), l_Index++);
		l_Step.added.clear();
	}
}

void ReplaceObjectsCommand::revert(Document& p_Document)
{
	for (auto l_It = m_Steps.rbegin(); l_It != m_Steps.rend(); ++l_It)
	{
		Step& l_Step = *l_It;
		l_Step.added.clear();
		for (const ObjectId l_Id : l_Step.addedIds)
			l_Step.added.push_back(p_Document.take(l_Id).object);
		p_Document.insert(std::move(l_Step.removed), l_Step.zIndex);
	}
}
TransformObjectsCommand::TransformObjectsCommand(std::vector<Entry> p_Entries, std::string p_Name, const bool p_Mergeable)
	: m_Entries(std::move(p_Entries)), m_Name(std::move(p_Name)), m_Mergeable(p_Mergeable)
{
}

void TransformObjectsCommand::apply(Document& p_Document)
{
	for (const Entry& l_Entry : m_Entries)
		p_Document.modify(l_Entry.id, [&](Object& p_Object) { p_Object.transform = l_Entry.after; }, ObjectChange::Transform);
}

void TransformObjectsCommand::revert(Document& p_Document)
{
	for (const Entry& l_Entry : m_Entries)
		p_Document.modify(l_Entry.id, [&](Object& p_Object) { p_Object.transform = l_Entry.before; }, ObjectChange::Transform);
}

bool TransformObjectsCommand::mergeWith(Command& p_Next)
{
	auto* l_Next = dynamic_cast<TransformObjectsCommand*>(&p_Next);
	if (!m_Mergeable || l_Next == nullptr || !l_Next->m_Mergeable || l_Next->m_Name != m_Name || l_Next->m_Entries.size() != m_Entries.size())
		return false;
	for (size_t i = 0; i < m_Entries.size(); ++i)
	{
		if (m_Entries[i].id != l_Next->m_Entries[i].id)
			return false;
	}
	for (size_t i = 0; i < m_Entries.size(); ++i)
		m_Entries[i].after = l_Next->m_Entries[i].after;
	return true;
}

bool TransformObjectsCommand::changesAnything() const
{
	return std::ranges::any_of(m_Entries, [](const Entry& p_Entry)
	{
		const Affine2& l_A = p_Entry.before;
		const Affine2& l_B = p_Entry.after;
		return l_A.linear[0] != l_B.linear[0] || l_A.linear[1] != l_B.linear[1] || l_A.translation != l_B.translation;
	});
}

namespace
{
std::vector<ObjectId> documentOrder(const Document& p_Document)
{
	std::vector<ObjectId> l_Order;
	l_Order.reserve(p_Document.size());
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
		l_Order.push_back(l_Object->id);
	return l_Order;
}
} // namespace

ReorderObjectsCommand::ReorderObjectsCommand(std::vector<ObjectId> p_Ids, const ZOrderMove p_Move, std::string p_Name)
	: m_Ids(std::move(p_Ids)), m_Move(p_Move), m_Name(std::move(p_Name))
{
}

void ReorderObjectsCommand::apply(Document& p_Document)
{
	std::vector<ObjectId> l_Order = documentOrder(p_Document);
	const std::unordered_set<ObjectId> l_Moving(m_Ids.begin(), m_Ids.end());
	const auto l_IsMoving = [&](const ObjectId p_Id) { return l_Moving.contains(p_Id); };

	m_OldPositions.clear();
	for (size_t i = 0; i < l_Order.size(); ++i)
	{
		if (l_IsMoving(l_Order[i]))
			m_OldPositions.emplace_back(l_Order[i], i);
	}

	switch (m_Move)
	{
	case ZOrderMove::ToFront:
		std::ranges::stable_partition(l_Order, [&](const ObjectId p_Id) { return !l_IsMoving(p_Id); });
		break;
	case ZOrderMove::ToBack:
		std::ranges::stable_partition(l_Order, l_IsMoving);
		break;
	case ZOrderMove::Forward:
		for (size_t i = l_Order.size(); i-- > 1;)
		{
			if (l_IsMoving(l_Order[i - 1]) && !l_IsMoving(l_Order[i]))
				std::swap(l_Order[i - 1], l_Order[i]);
		}
		break;
	case ZOrderMove::Backward:
		for (size_t i = 1; i < l_Order.size(); ++i)
		{
			if (l_IsMoving(l_Order[i]) && !l_IsMoving(l_Order[i - 1]))
				std::swap(l_Order[i - 1], l_Order[i]);
		}
		break;
	}
	m_Changed = l_Order != documentOrder(p_Document);
	p_Document.setOrder(l_Order);
}

void ReorderObjectsCommand::revert(Document& p_Document)
{
	std::vector<ObjectId> l_Order = documentOrder(p_Document);
	std::unordered_set<ObjectId> l_Moved;
	for (const auto& [l_Id, l_Position] : m_OldPositions)
		l_Moved.insert(l_Id);
	std::erase_if(l_Order, [&](const ObjectId p_Id) { return l_Moved.contains(p_Id); });
	for (const auto& [l_Id, l_Position] : m_OldPositions) // ascending, so each insert lands on its final index
		l_Order.insert(l_Order.begin() + static_cast<std::ptrdiff_t>(std::min(l_Position, l_Order.size())), l_Id);
	p_Document.setOrder(l_Order);
}

SetStrokeColorCommand::SetStrokeColorCommand(std::vector<ObjectId> p_Ids, const Color p_Color, std::string p_Name)
	: m_Ids(std::move(p_Ids)), m_New(p_Color), m_Name(std::move(p_Name))
{
}

void SetStrokeColorCommand::apply(Document& p_Document)
{
	m_Old.clear();
	for (const ObjectId l_Id : m_Ids)
	{
		const Object* l_Object = p_Document.find(l_Id);
		const StrokeData* l_Stroke = l_Object != nullptr ? l_Object->stroke() : nullptr;
		m_Old.push_back(l_Stroke != nullptr ? l_Stroke->style.color : m_New);
		if (l_Stroke == nullptr)
			continue;
		const Color l_Color{ m_New.r, m_New.g, m_New.b, l_Stroke->style.color.a };
		p_Document.modify(l_Id, [&](Object& p_Object) { p_Object.stroke()->style.color = l_Color; }, ObjectChange::Style);
	}
}

void SetStrokeColorCommand::revert(Document& p_Document)
{
	for (size_t i = 0; i < m_Ids.size() && i < m_Old.size(); ++i)
	{
		const Object* l_Object = p_Document.find(m_Ids[i]);
		if (l_Object == nullptr || l_Object->stroke() == nullptr)
			continue;
		p_Document.modify(m_Ids[i], [&](Object& p_Object) { p_Object.stroke()->style.color = m_Old[i]; }, ObjectChange::Style);
	}
}
} // namespace wb
