module;
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module wb.doc.commands;

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
} // namespace wb
