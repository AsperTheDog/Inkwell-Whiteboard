module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

module wb.net.replicator;

import wb.doc.object;
import wb.doc.document;
import wb.io.serializer;
import wb.net.ops;

namespace wb::net
{
Replicator::Replicator(Document& p_Document, Sink p_Sink) : m_Document(p_Document), m_Sink(std::move(p_Sink))
{
	m_Document.addListener(this);
}

Replicator::~Replicator()
{
	m_Document.removeListener(this);
}

ApplyResult Replicator::applyRemote(const Op& p_Op)
{
	ApplyResult l_Result;
	{
		Pause l_Pause(*this);
		l_Result = applyOp(m_Document, p_Op);
	}
	for (const ObjectId l_Id : l_Result.changed)
		m_VideoSent.erase(l_Id);
	if (l_Result.ok && m_OnRemoteChange)
	{
		if (!l_Result.removed.empty())
			m_OnRemoteChange(l_Result.removed, true);
		if (!l_Result.changed.empty())
			m_OnRemoteChange(l_Result.changed, false);
	}
	return l_Result;
}

namespace
{
// A video as everybody shares it: loop, sound, volume and the tracks watched are each person's own, so they do not travel
std::vector<uint8_t> encodeShared(const Object& p_Object)
{
	if (p_Object.video() == nullptr)
		return encodeObject(p_Object);
	Object l_Copy = p_Object;
	VideoData& l_Video = *l_Copy.video();
	l_Video.loop = false;
	l_Video.muted = true;
	l_Video.volume = 1.f;
	l_Video.audioTrack = -1;
	l_Video.subtitleTrack = -1;
	return encodeObject(l_Copy);
}
} // namespace

void Replicator::onObjectAdded(const Object& p_Object)
{
	if (m_Paused != 0)
		return;
	const size_t l_Index = m_Document.indexOf(p_Object.id);
	AddOp l_Add;
	l_Add.below = (l_Index != SIZE_MAX && l_Index > 0) ? m_Document.objects()[l_Index - 1]->id : INVALID_OBJECT_ID;
	l_Add.locked = p_Object.locked;
	l_Add.object = encodeShared(p_Object);
	if (p_Object.video() != nullptr)
		m_VideoSent[p_Object.id] = l_Add.object;
	emit(std::move(l_Add));
}

void Replicator::onObjectRemoved(const Object& p_Object)
{
	m_VideoSent.erase(p_Object.id);
	emit(RemoveOp{ .id = p_Object.id });
}

void Replicator::onObjectChanged(const Object& p_Object, const uint32_t p_Changes)
{
	if (m_Paused != 0)
		return;
	if ((p_Changes & (ObjectChange::Geometry | ObjectChange::Style)) != 0)
	{
		std::vector<uint8_t> l_Bytes = encodeShared(p_Object);
		bool l_Send = true;
		if (p_Object.video() != nullptr)
		{
			// Changing the volume or loop is no news to the others
			std::vector<uint8_t>& l_Last = m_VideoSent[p_Object.id];
			l_Send = l_Last != l_Bytes;
			l_Last = l_Bytes;
		}
		if (l_Send)
			emit(UpdateOp{ .object = std::move(l_Bytes) });
	}
	else if ((p_Changes & ObjectChange::Transform) != 0)
		emit(TransformOp{ .id = p_Object.id, .transform = p_Object.transform });
	if ((p_Changes & ObjectChange::Lock) != 0)
		emit(LockOp{ .id = p_Object.id, .locked = p_Object.locked });
}

void Replicator::onObjectsReordered()
{
	if (m_Paused != 0)
		return;
	OrderOp l_Order;
	l_Order.ids.reserve(m_Document.size());
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
		l_Order.ids.push_back(l_Object->id);
	emit(std::move(l_Order));
}

void Replicator::onDocumentCleared()
{
	m_VideoSent.clear();
	emit(ClearOp{});
}
} // namespace wb::net
