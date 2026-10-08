module;
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

module wb.doc.document;

import wb.math;
import wb.doc.object;

namespace wb
{
void Document::reserveId(const ObjectId p_Id)
{
	m_NextId = std::max(m_NextId, p_Id + 1);
}

void Document::insert(std::unique_ptr<Object> p_Object, const size_t p_ZIndex)
{
	if (!p_Object || p_Object->id == INVALID_OBJECT_ID)
		throw std::invalid_argument("Document::insert: object without id");
	if (m_ById.contains(p_Object->id))
		throw std::invalid_argument("Document::insert: duplicate object id");

	reserveId(p_Object->id);
	p_Object->refreshBounds();
	Object* l_Raw = p_Object.get();
	const size_t l_Index = std::min(p_ZIndex, m_Objects.size());
	m_Objects.insert(m_Objects.begin() + static_cast<std::ptrdiff_t>(l_Index), std::move(p_Object));
	m_ById.emplace(l_Raw->id, l_Raw);
	++m_Revision;

	for (DocumentListener* l_Listener : m_Listeners)
		l_Listener->onObjectAdded(*l_Raw);
}

Document::Removed Document::take(const ObjectId p_Id)
{
	const size_t l_Index = indexOf(p_Id);
	if (l_Index == SIZE_MAX)
		return {};

	std::unique_ptr<Object> l_Object = std::move(m_Objects[l_Index]);
	m_Objects.erase(m_Objects.begin() + static_cast<std::ptrdiff_t>(l_Index));
	m_ById.erase(p_Id);
	++m_Revision;

	for (DocumentListener* l_Listener : m_Listeners)
		l_Listener->onObjectRemoved(*l_Object);
	return Removed{ .object = std::move(l_Object), .zIndex = l_Index };
}

void Document::modify(const ObjectId p_Id, const std::function<void(Object&)>& p_Edit, const uint32_t p_Changes)
{
	const auto l_It = m_ById.find(p_Id);
	if (l_It == m_ById.end())
		return;

	Object& l_Object = *l_It->second;
	p_Edit(l_Object);
	assert(l_Object.id == p_Id && "modify() must not change the object id");
	l_Object.refreshBounds();
	++l_Object.version;
	++m_Revision;

	for (DocumentListener* l_Listener : m_Listeners)
		l_Listener->onObjectChanged(l_Object, p_Changes);
}

void Document::moveTo(const ObjectId p_Id, const size_t p_ZIndex)
{
	const size_t l_From = indexOf(p_Id);
	if (l_From == SIZE_MAX)
		return;
	const size_t l_To = std::min(p_ZIndex, m_Objects.size() - 1);
	if (l_From == l_To)
		return;

	std::unique_ptr<Object> l_Object = std::move(m_Objects[l_From]);
	m_Objects.erase(m_Objects.begin() + static_cast<std::ptrdiff_t>(l_From));
	m_Objects.insert(m_Objects.begin() + static_cast<std::ptrdiff_t>(l_To), std::move(l_Object));
	++m_Revision;

	for (DocumentListener* l_Listener : m_Listeners)
		l_Listener->onObjectsReordered();
}

void Document::setOrder(const std::span<const ObjectId> p_Order)
{
	if (p_Order.size() != m_Objects.size())
		return;

	// Validate before touching anything: every id must be known and used once
	std::unordered_map<const Object*, size_t> l_Slots;
	l_Slots.reserve(m_Objects.size());
	for (size_t i = 0; i < m_Objects.size(); ++i)
		l_Slots.emplace(m_Objects[i].get(), i);
	std::vector<size_t> l_Sources;
	l_Sources.reserve(p_Order.size());
	for (const ObjectId l_Id : p_Order)
	{
		const auto l_ById = m_ById.find(l_Id);
		if (l_ById == m_ById.end())
			return;
		const auto l_Slot = l_Slots.find(l_ById->second);
		if (l_Slot == l_Slots.end())
			return;
		l_Sources.push_back(l_Slot->second);
		l_Slots.erase(l_Slot); // a repeated id then fails the lookup above
	}

	std::vector<std::unique_ptr<Object>> l_Reordered;
	l_Reordered.reserve(m_Objects.size());
	for (const size_t l_Source : l_Sources)
		l_Reordered.push_back(std::move(m_Objects[l_Source]));
	m_Objects = std::move(l_Reordered);
	++m_Revision;
	for (DocumentListener* l_Listener : m_Listeners)
		l_Listener->onObjectsReordered();
}

void Document::clear()
{
	m_Objects.clear();
	m_ById.clear();
	++m_Revision;
	for (DocumentListener* l_Listener : m_Listeners)
		l_Listener->onDocumentCleared();
}

const Object* Document::find(const ObjectId p_Id) const
{
	const auto l_It = m_ById.find(p_Id);
	return l_It != m_ById.end() ? l_It->second : nullptr;
}

size_t Document::indexOf(const ObjectId p_Id) const
{
	const Object* l_Object = find(p_Id);
	if (l_Object == nullptr)
		return SIZE_MAX;
	const auto l_It = std::ranges::find_if(m_Objects, [&](const std::unique_ptr<Object>& p_Candidate) { return p_Candidate.get() == l_Object; });
	return static_cast<size_t>(l_It - m_Objects.begin());
}

Rect Document::contentBounds() const
{
	Rect l_Bounds{};
	for (const std::unique_ptr<Object>& l_Object : m_Objects)
		l_Bounds.expand(l_Object->worldBounds());
	return l_Bounds;
}

void Document::addListener(DocumentListener* p_Listener)
{
	if (p_Listener != nullptr && std::ranges::find(m_Listeners, p_Listener) == m_Listeners.end())
		m_Listeners.push_back(p_Listener);
}

void Document::removeListener(DocumentListener* p_Listener)
{
	std::erase(m_Listeners, p_Listener);
}
} // namespace wb
