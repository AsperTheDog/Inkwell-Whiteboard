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

void Selection::set(const std::span<const ObjectId> p_Ids)
{
	std::vector<ObjectId> l_Ids;
	std::unordered_set<ObjectId> l_Set;
	for (const ObjectId l_Id : p_Ids)
	{
		if (m_Document.find(l_Id) != nullptr && l_Set.insert(l_Id).second)
			l_Ids.push_back(l_Id);
	}
	if (l_Ids == m_Ids)
		return;
	m_Ids = std::move(l_Ids);
	m_Set = std::move(l_Set);
	++m_Revision;
}

void Selection::add(const ObjectId p_Id)
{
	if (m_Document.find(p_Id) == nullptr || !m_Set.insert(p_Id).second)
		return;
	m_Ids.push_back(p_Id);
	++m_Revision;
}

void Selection::remove(const ObjectId p_Id)
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
	remove(p_Object.id);
}

void Selection::onDocumentCleared()
{
	clear();
}
} // namespace wb
