module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

module wb.doc.selection;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.hit;

namespace wb
{
Selection::Selection(Document& p_Document) : m_Document(p_Document)
{
	m_Document.addListener(this);
}

Selection::~Selection()
{
	m_Document.removeListener(this);
}

void Selection::clear()
{
	if (m_Ids.empty())
		return;
	m_Ids.clear();
	m_Set.clear();
	++m_Revision;
}

namespace
{
// Adds the rest of every group that has a member in p_Ids (one pass over the board, and only when a group is involved)
void addGroupMates(const Document& p_Document, std::vector<ObjectId>& p_Ids, std::unordered_set<ObjectId>& p_Set)
{
	std::unordered_set<ObjectId> l_Groups;
	for (const ObjectId l_Id : p_Ids)
	{
		if (const Object* l_Object = p_Document.find(l_Id); l_Object != nullptr && l_Object->group != INVALID_OBJECT_ID)
			l_Groups.insert(l_Object->group);
	}
	if (l_Groups.empty())
		return;
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
	{
		if (l_Object->group != INVALID_OBJECT_ID && l_Groups.contains(l_Object->group) && p_Set.insert(l_Object->id).second)
			p_Ids.push_back(l_Object->id);
	}
}
} // namespace

void Selection::set(const std::span<const ObjectId> p_Ids)
{
	std::vector<ObjectId> l_Ids;
	std::unordered_set<ObjectId> l_Set;
	for (const ObjectId l_Id : p_Ids)
	{
		if (m_Document.find(l_Id) != nullptr && l_Set.insert(l_Id).second)
			l_Ids.push_back(l_Id);
	}
	addGroupMates(m_Document, l_Ids, l_Set);
	if (l_Ids == m_Ids)
		return;
	m_Ids = std::move(l_Ids);
	m_Set = std::move(l_Set);
	++m_Revision;
}

void Selection::add(const ObjectId p_Id)
{
	if (m_Document.find(p_Id) == nullptr)
		return;
	std::vector<ObjectId> l_Added;
	if (m_Set.insert(p_Id).second)
		l_Added.push_back(p_Id);
	const size_t l_Own = l_Added.size();
	addGroupMates(m_Document, l_Added, m_Set);
	if (l_Own == 0 && l_Added.empty())
		return;
	m_Ids.insert(m_Ids.end(), l_Added.begin(), l_Added.end());
	++m_Revision;
}

void Selection::remove(const ObjectId p_Id)
{
	// A group leaves the selection together
	if (const Object* l_Object = m_Document.find(p_Id); l_Object != nullptr && l_Object->group != INVALID_OBJECT_ID)
	{
		const ObjectId l_Group = l_Object->group;
		bool l_Changed = false;
		for (const std::unique_ptr<Object>& l_Mate : m_Document.objects())
		{
			if (l_Mate->group == l_Group)
				l_Changed |= m_Set.erase(l_Mate->id) != 0;
		}
		if (l_Changed)
		{
			std::erase_if(m_Ids, [&](const ObjectId p_Selected) { return !m_Set.contains(p_Selected); });
			++m_Revision;
		}
		return;
	}
	drop(p_Id);
}

void Selection::drop(const ObjectId p_Id)
{
	if (m_Set.erase(p_Id) == 0)
		return;
	std::erase(m_Ids, p_Id);
	++m_Revision;
}

void Selection::toggle(const ObjectId p_Id)
{
	if (contains(p_Id))
		remove(p_Id);
	else
		add(p_Id);
}

std::vector<ObjectId> Selection::orderedIds() const
{
	std::vector<ObjectId> l_Result;
	l_Result.reserve(m_Ids.size());
	if (m_Ids.empty())
		return l_Result;
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
	{
		if (m_Set.contains(l_Object->id))
			l_Result.push_back(l_Object->id);
	}
	return l_Result;
}

Rect Selection::bounds() const
{
	Rect l_Bounds{};
	for (const ObjectId l_Id : m_Ids)
	{
		if (const Object* l_Object = m_Document.find(l_Id))
			l_Bounds.expand(tightWorldBounds(*l_Object));
	}
	return l_Bounds;
}

void Selection::onObjectRemoved(const Object& p_Object)
{
	drop(p_Object.id); // only this one: its group mates stay selected
}

void Selection::onDocumentCleared()
{
	clear();
}
} // namespace wb
