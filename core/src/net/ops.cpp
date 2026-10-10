module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module wb.net.ops;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.io.serializer;
import wb.net.wire;

namespace wb::net
{
namespace
{
enum class OpKind : uint8_t
{
	Add = 1,
	Remove = 2,
	Transform = 3,
	Update = 4,
	Lock = 5,
	Order = 6,
	Clear = 7,
};

// A decoded object record that must be a single, complete, known-kind object
std::unique_ptr<Object> decodeWhole(const std::span<const uint8_t> p_Bytes)
{
	std::unique_ptr<Object> l_Object;
	size_t l_Consumed = 0;
	if (decodeObject(p_Bytes, l_Object, &l_Consumed) != ObjectDecode::Ok || l_Consumed != p_Bytes.size())
		return nullptr;
	return l_Object;
}

ObjectId idOfRecord(const std::span<const uint8_t> p_Bytes)
{
	ByteReader l_Reader(p_Bytes);
	const ObjectId l_Id = l_Reader.u64();
	return l_Reader.ok() ? l_Id : INVALID_OBJECT_ID;
}
} // namespace

void encodeOp(ByteWriter& p_Writer, const Op& p_Op)
{
	if (const auto* l_Add = std::get_if<AddOp>(&p_Op))
	{
		p_Writer.u8(static_cast<uint8_t>(OpKind::Add));
		p_Writer.u64(l_Add->below);
		p_Writer.boolean(l_Add->locked);
		p_Writer.blob(l_Add->object);
	}
	else if (const auto* l_Remove = std::get_if<RemoveOp>(&p_Op))
	{
		p_Writer.u8(static_cast<uint8_t>(OpKind::Remove));
		p_Writer.u64(l_Remove->id);
	}
	else if (const auto* l_Transform = std::get_if<TransformOp>(&p_Op))
	{
		p_Writer.u8(static_cast<uint8_t>(OpKind::Transform));
		p_Writer.u64(l_Transform->id);
		p_Writer.transform(l_Transform->transform);
	}
	else if (const auto* l_Update = std::get_if<UpdateOp>(&p_Op))
	{
		p_Writer.u8(static_cast<uint8_t>(OpKind::Update));
		p_Writer.blob(l_Update->object);
	}
	else if (const auto* l_Lock = std::get_if<LockOp>(&p_Op))
	{
		p_Writer.u8(static_cast<uint8_t>(OpKind::Lock));
		p_Writer.u64(l_Lock->id);
		p_Writer.boolean(l_Lock->locked);
	}
	else if (const auto* l_Order = std::get_if<OrderOp>(&p_Op))
	{
		p_Writer.u8(static_cast<uint8_t>(OpKind::Order));
		p_Writer.u32(static_cast<uint32_t>(l_Order->ids.size()));
		for (const ObjectId l_Id : l_Order->ids)
			p_Writer.u64(l_Id);
	}
	else
	{
		p_Writer.u8(static_cast<uint8_t>(OpKind::Clear));
	}
}

bool decodeOp(ByteReader& p_Reader, Op& p_Op)
{
	switch (static_cast<OpKind>(p_Reader.u8()))
	{
	case OpKind::Add:
	{
		AddOp l_Add;
		l_Add.below = p_Reader.u64();
		l_Add.locked = p_Reader.boolean();
		const std::span<const uint8_t> l_Object = p_Reader.blob(MAX_OBJECT_BYTES);
		if (!p_Reader.ok())
			return false;
		l_Add.object.assign(l_Object.begin(), l_Object.end());
		p_Op = std::move(l_Add);
		return true;
	}
	case OpKind::Remove:
		p_Op = RemoveOp{ .id = p_Reader.u64() };
		return p_Reader.ok();
	case OpKind::Transform:
	{
		TransformOp l_Transform;
		l_Transform.id = p_Reader.u64();
		if (!p_Reader.transform(l_Transform.transform))
			return false;
		p_Op = l_Transform;
		return true;
	}
	case OpKind::Update:
	{
		UpdateOp l_Update;
		const std::span<const uint8_t> l_Object = p_Reader.blob(MAX_OBJECT_BYTES);
		if (!p_Reader.ok())
			return false;
		l_Update.object.assign(l_Object.begin(), l_Object.end());
		p_Op = std::move(l_Update);
		return true;
	}
	case OpKind::Lock:
	{
		LockOp l_Lock;
		l_Lock.id = p_Reader.u64();
		l_Lock.locked = p_Reader.boolean();
		p_Op = l_Lock;
		return p_Reader.ok();
	}
	case OpKind::Order:
	{
		const uint32_t l_Count = p_Reader.u32();
		if (!p_Reader.ok() || l_Count > MAX_ORDER_IDS || static_cast<uint64_t>(l_Count) * sizeof(uint64_t) > p_Reader.remaining())
			return false;
		OrderOp l_Order;
		l_Order.ids.reserve(l_Count);
		for (uint32_t i = 0; i < l_Count; ++i)
			l_Order.ids.push_back(p_Reader.u64());
		p_Op = std::move(l_Order);
		return p_Reader.ok();
	}
	case OpKind::Clear:
		p_Op = ClearOp{};
		return p_Reader.ok();
	}
	p_Reader.fail();
	return false;
}

ObjectId opTarget(const Op& p_Op)
{
	if (const auto* l_Add = std::get_if<AddOp>(&p_Op))
		return idOfRecord(l_Add->object);
	if (const auto* l_Update = std::get_if<UpdateOp>(&p_Op))
		return idOfRecord(l_Update->object);
	if (const auto* l_Remove = std::get_if<RemoveOp>(&p_Op))
		return l_Remove->id;
	if (const auto* l_Transform = std::get_if<TransformOp>(&p_Op))
		return l_Transform->id;
	if (const auto* l_Lock = std::get_if<LockOp>(&p_Op))
		return l_Lock->id;
	return INVALID_OBJECT_ID;
}

ApplyResult applyOp(Document& p_Document, const Op& p_Op)
{
	ApplyResult l_Result;
	if (const auto* l_Add = std::get_if<AddOp>(&p_Op))
	{
		std::unique_ptr<Object> l_Object = decodeWhole(l_Add->object);
		if (!l_Object || p_Document.find(l_Object->id) != nullptr)
			return l_Result;
		size_t l_Index = SIZE_MAX;
		if (l_Add->below == INVALID_OBJECT_ID)
			l_Index = 0;
		else if (const size_t l_Below = p_Document.indexOf(l_Add->below); l_Below != SIZE_MAX)
			l_Index = l_Below + 1;
		l_Object->locked = l_Add->locked;
		p_Document.insert(std::move(l_Object), l_Index);
		l_Result.ok = true;
	}
	else if (const auto* l_Remove = std::get_if<RemoveOp>(&p_Op))
	{
		if (p_Document.find(l_Remove->id) == nullptr)
			return l_Result;
		(void)p_Document.take(l_Remove->id);
		l_Result.removed.push_back(l_Remove->id);
		l_Result.ok = true;
	}
	else if (const auto* l_Transform = std::get_if<TransformOp>(&p_Op))
	{
		if (p_Document.find(l_Transform->id) == nullptr)
			return l_Result;
		p_Document.modify(l_Transform->id, [&](Object& p_Object) { p_Object.transform = l_Transform->transform; }, ObjectChange::Transform);
		l_Result.changed.push_back(l_Transform->id);
		l_Result.ok = true;
	}
	else if (const auto* l_Update = std::get_if<UpdateOp>(&p_Op))
	{
		std::unique_ptr<Object> l_Object = decodeWhole(l_Update->object);
		if (!l_Object)
			return l_Result;
		const Object* l_Existing = p_Document.find(l_Object->id);
		if (l_Existing == nullptr || l_Existing->payload.index() != l_Object->payload.index())
			return l_Result;
		const ObjectId l_Id = l_Object->id;
		// How each person likes a video (loop, sound) is their own business
		if (const VideoData* l_Old = l_Existing->video(); l_Old != nullptr && l_Object->video() != nullptr)
		{
			l_Object->video()->loop = l_Old->loop;
			l_Object->video()->muted = l_Old->muted;
			l_Object->video()->volume = l_Old->volume;
		}
		p_Document.modify(l_Id, [&](Object& p_Target)
		{
			p_Target.payload = std::move(l_Object->payload);
			p_Target.transform = l_Object->transform;
		}, ObjectChange::Transform | ObjectChange::Geometry | ObjectChange::Style);
		l_Result.changed.push_back(l_Id);
		l_Result.ok = true;
	}
	else if (const auto* l_Lock = std::get_if<LockOp>(&p_Op))
	{
		if (p_Document.find(l_Lock->id) == nullptr)
			return l_Result;
		p_Document.modify(l_Lock->id, [&](Object& p_Object) { p_Object.locked = l_Lock->locked; }, ObjectChange::Lock);
		l_Result.changed.push_back(l_Lock->id);
		l_Result.ok = true;
	}
	else if (const auto* l_Order = std::get_if<OrderOp>(&p_Op))
	{
		// Objects added or removed meanwhile may be missing from either side: keep the sender's order for the ones we
		// know and leave the others (our own newer objects) on top, in their current order
		std::vector<ObjectId> l_Ordered;
		l_Ordered.reserve(p_Document.size());
		std::unordered_set<ObjectId> l_Placed;
		l_Placed.reserve(p_Document.size());
		for (const ObjectId l_Id : l_Order->ids)
		{
			if (p_Document.find(l_Id) != nullptr && l_Placed.insert(l_Id).second)
				l_Ordered.push_back(l_Id);
		}
		for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
		{
			if (!l_Placed.contains(l_Object->id))
				l_Ordered.push_back(l_Object->id);
		}
		p_Document.setOrder(l_Ordered);
		l_Result.ok = true;
	}
	else
	{
		p_Document.clear();
		l_Result.ok = true;
	}
	return l_Result;
}
} // namespace wb::net
