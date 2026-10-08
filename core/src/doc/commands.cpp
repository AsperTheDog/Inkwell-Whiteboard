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
} // namespace wb
