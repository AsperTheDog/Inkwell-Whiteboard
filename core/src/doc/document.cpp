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
import wb.io.blob;

namespace wb
{
void Document::reserveId(const ObjectId p_Id)
{
	if (idSlotOf(p_Id) == m_Slot)
		m_NextId = std::max(m_NextId, p_Id + 1);
}

void Document::setIdSlot(const uint16_t p_Slot)
{
	m_Slot = p_Slot;
	const uint64_t l_Base = static_cast<uint64_t>(p_Slot) << ID_SLOT_SHIFT;
	m_NextId = l_Base | 1;
	m_NextAsset = l_Base | 1;
	for (const std::unique_ptr<Object>& l_Object : m_Objects)
		reserveId(l_Object->id);
	for (const auto& [l_Id, l_Asset] : m_Assets)
	{
		if (idSlotOf(l_Id) == m_Slot)
			m_NextAsset = std::max(m_NextAsset, l_Id + 1);
	}
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
	if (!m_BoundsDirty)
		m_Bounds.insert(m_Bounds.begin() + static_cast<std::ptrdiff_t>(l_Index), l_Raw->worldBounds());
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
	if (!m_BoundsDirty)
		m_Bounds.erase(m_Bounds.begin() + static_cast<std::ptrdiff_t>(l_Index));
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
	if (!m_BoundsDirty)
	{
		m_StaleBounds.push_back(p_Id);
		if (m_StaleBounds.size() > 48)
			m_BoundsDirty = true;
	}

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
	m_BoundsDirty = true;
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
	m_BoundsDirty = true;
	++m_Revision;
	for (DocumentListener* l_Listener : m_Listeners)
		l_Listener->onObjectsReordered();
}

namespace
{
// Same for memory and streamed assets with the same content, so they find each other
uint64_t assetFingerprint(const ImageAsset& p_Asset)
{
	uint64_t l_Hash = 14695981039346656037ull;
	const auto l_Mix = [&](const uint8_t* p_Data, const size_t p_Size)
	{
		for (size_t i = 0; i < p_Size; ++i)
			l_Hash = (l_Hash ^ p_Data[i]) * 1099511628211ull;
	};
	const uint64_t l_Size = p_Asset.size();
	l_Mix(reinterpret_cast<const uint8_t*>(&l_Size), sizeof(l_Size));
	if (!p_Asset.complete())
		return l_Hash; // reading the middle of a download would wait for it
	constexpr size_t SAMPLE = 64u << 10;
	std::vector<uint8_t> l_Buffer(static_cast<size_t>(std::min<uint64_t>(SAMPLE, std::max<uint64_t>(l_Size, 1))));
	const uint64_t l_Middle = l_Size / 2 > SAMPLE / 2 ? l_Size / 2 - SAMPLE / 2 : 0;
	const uint64_t l_Starts[3] = { 0, l_Middle, l_Size > SAMPLE ? l_Size - SAMPLE : 0 };
	for (const uint64_t l_Start : l_Starts)
		l_Mix(l_Buffer.data(), p_Asset.read(l_Start, l_Buffer.data(), l_Buffer.size()));
	return l_Hash;
}

bool sameContent(const ImageAsset& p_A, const ImageAsset& p_B)
{
	if (p_A.size() != p_B.size() || !p_A.complete() || !p_B.complete())
		return false;
	if (!p_A.stream && !p_B.stream)
		return p_A.bytes == p_B.bytes;
	return sameBytes(*p_A.source(), *p_B.source());
}
} // namespace

AssetId Document::addAsset(ImageAsset p_Asset)
{
	const uint64_t l_Hash = assetFingerprint(p_Asset);
	std::vector<AssetId>& l_Candidates = m_AssetsByHash[l_Hash];
	for (const AssetId l_Id : l_Candidates)
	{
		if (sameContent(m_Assets.at(l_Id), p_Asset))
			return l_Id;
	}
	p_Asset.id = m_NextAsset++;
	const AssetId l_Id = p_Asset.id;
	l_Candidates.push_back(l_Id);
	m_Assets.emplace(l_Id, std::move(p_Asset));
	return l_Id;
}

bool Document::insertAsset(ImageAsset p_Asset)
{
	if (p_Asset.id == INVALID_ASSET_ID || m_Assets.contains(p_Asset.id))
		return false;
	const uint64_t l_Hash = assetFingerprint(p_Asset);
	if (idSlotOf(p_Asset.id) == m_Slot)
		m_NextAsset = std::max(m_NextAsset, p_Asset.id + 1);
	m_AssetsByHash[l_Hash].push_back(p_Asset.id);
	m_Assets.emplace(p_Asset.id, std::move(p_Asset));
	return true;
}

bool Document::rebindAsset(const AssetId p_Id, std::shared_ptr<const ByteSource> p_Source)
{
	const auto l_It = m_Assets.find(p_Id);
	if (l_It == m_Assets.end() || !p_Source || p_Source->size() != l_It->second.size())
		return false;
	l_It->second.stream = std::move(p_Source);
	std::vector<uint8_t>().swap(l_It->second.bytes);
	return true;
}

void Document::eraseAsset(const AssetId p_Id)
{
	const auto l_It = m_Assets.find(p_Id);
	if (l_It == m_Assets.end())
		return;
	for (auto& [l_Hash, l_Ids] : m_AssetsByHash)
		std::erase(l_Ids, p_Id);
	m_Assets.erase(l_It);
}

const ImageAsset* Document::findAsset(const AssetId p_Id) const
{
	const auto l_It = m_Assets.find(p_Id);
	return l_It != m_Assets.end() ? &l_It->second : nullptr;
}

void Document::setFontAsset(FontAsset p_Font)
{
	for (FontAsset& l_Existing : m_Fonts)
	{
		if (l_Existing.style == p_Font.style && l_Existing.family == p_Font.family)
		{
			l_Existing = std::move(p_Font);
			return;
		}
	}
	m_Fonts.push_back(std::move(p_Font));
}

const FontAsset* Document::findFontAsset(const std::string_view p_Family, const uint8_t p_Style) const
{
	for (const FontAsset& l_Font : m_Fonts)
	{
		if (l_Font.style == p_Style && l_Font.family == p_Family)
			return &l_Font;
	}
	return nullptr;
}

void Document::clear()
{
	m_Fonts.clear();
	m_Assets.clear();
	m_Previews.clear();
	m_AssetsByHash.clear();
	m_NextAsset = (static_cast<uint64_t>(m_Slot) << ID_SLOT_SHIFT) | 1;
	m_Objects.clear();
	m_ById.clear();
	m_Bounds.clear();
	m_StaleBounds.clear();
	m_BoundsDirty = false;
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

std::span<const Rect> Document::boundsList() const
{
	if (m_BoundsDirty)
	{
		m_Bounds.resize(m_Objects.size());
		for (size_t i = 0; i < m_Objects.size(); ++i)
			m_Bounds[i] = m_Objects[i]->worldBounds();
		m_BoundsDirty = false;
	}
	else
	{
		for (const ObjectId l_Id : m_StaleBounds)
		{
			if (const size_t l_Index = indexOf(l_Id); l_Index != SIZE_MAX)
				m_Bounds[l_Index] = m_Objects[l_Index]->worldBounds();
		}
	}
	m_StaleBounds.clear();
	return m_Bounds;
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
