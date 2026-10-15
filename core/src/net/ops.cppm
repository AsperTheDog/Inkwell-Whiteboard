// Collaboration operations: the small, self-contained edits two peers exchange to keep their documents equal.
//
// Replication is state based. Every Document mutation (a tool, undo, a remote peer's edit) becomes one of these, so the
// undo history stays purely local. Objects travel in the .wbrd record encoding (wb.io.serializer), which already
// validates everything it reads.
module;
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

export module wb.net.ops;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.net.wire;

export namespace wb::net
{
inline constexpr size_t MAX_OBJECT_BYTES = 64u << 20;
inline constexpr uint32_t MAX_ORDER_IDS = 4u << 20;

// A new object, placed directly above `below` (INVALID_OBJECT_ID: at the very back; an unknown id: on top)
struct AddOp
{
	ObjectId below = INVALID_OBJECT_ID;
	bool locked = false;
	std::vector<uint8_t> object;
};
struct RemoveOp
{
	ObjectId id = INVALID_OBJECT_ID;
};
struct TransformOp
{
	ObjectId id = INVALID_OBJECT_ID;
	Affine2 transform{};
};
// New payload and transform for an existing object (same kind of object, new content)
struct UpdateOp
{
	std::vector<uint8_t> object;
};
struct LockOp
{
	ObjectId id = INVALID_OBJECT_ID;
	bool locked = false;
};
// The object joins a group, or leaves its group (INVALID_OBJECT_ID)
struct GroupOp
{
	ObjectId id = INVALID_OBJECT_ID;
	ObjectId group = INVALID_OBJECT_ID;
};
// The object's note (the text of the notes panel) is replaced
struct NoteOp
{
	ObjectId id = INVALID_OBJECT_ID;
	std::string note;
};
// The whole stacking order, back to front
struct OrderOp
{
	std::vector<ObjectId> ids;
};
// The board was replaced (new, open): everything is gone
struct ClearOp
{
};

using Op = std::variant<AddOp, RemoveOp, TransformOp, UpdateOp, LockOp, OrderOp, ClearOp, GroupOp, NoteOp>;

void encodeOp(ByteWriter& p_Writer, const Op& p_Op);
// False when the data is malformed
[[nodiscard]] bool decodeOp(ByteReader& p_Reader, Op& p_Op);

// The object an op creates or changes (INVALID_OBJECT_ID for ops that name none)
[[nodiscard]] ObjectId opTarget(const Op& p_Op);

struct ApplyResult
{
	bool ok = false;
	std::vector<ObjectId> changed; // objects whose content was replaced or edited
	std::vector<ObjectId> removed;
};

// Applies an op to the document. Rejects (ok == false) anything that does not make sense for the current state
// (unknown ids, duplicate adds, a payload of another kind), leaving the document untouched.
ApplyResult applyOp(Document& p_Document, const Op& p_Op);
} // namespace wb::net
