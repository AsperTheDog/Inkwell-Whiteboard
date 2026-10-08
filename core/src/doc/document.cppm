// The board: objects in z-order (back to front) plus change notifications.
//
// All mutations go through Document so listeners (GPU caches, spatial index, autosave...) stay in sync.
// Undoable edits are expressed as Commands (wb.doc.history) that call these primitives.
module;
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

export module wb.doc.document;

import wb.math;
import wb.doc.object;

export namespace wb
{
// What a modify() call touched, so listeners can skip work (e.g. no GPU re-upload for a pure move)
namespace ObjectChange
{
inline constexpr uint32_t Transform = 1u << 0;
inline constexpr uint32_t Geometry = 1u << 1; // payload shape (stroke points, image size, text...)
inline constexpr uint32_t Style = 1u << 2;    // color, size and other appearance
inline constexpr uint32_t All = Transform | Geometry | Style;
} // namespace ObjectChange

class DocumentListener
{
public:
	virtual ~DocumentListener() = default;
	virtual void onObjectAdded(const Object& p_Object) = 0;
	virtual void onObjectRemoved(const Object& p_Object) = 0;
	virtual void onObjectChanged(const Object& p_Object, uint32_t p_Changes) = 0;
	virtual void onObjectsReordered() {}
	virtual void onDocumentCleared() = 0;
};

class Document
{
public:
	struct Removed
	{
		std::unique_ptr<Object> object;
		size_t zIndex = 0;
	};

	Document() = default;
	Document(const Document&) = delete;
	Document& operator=(const Document&) = delete;

	[[nodiscard]] ObjectId allocateId() { return m_NextId++; }
	// Ensures future ids are greater than p_Id (used when loading files)
	void reserveId(ObjectId p_Id);

	// Inserts at p_ZIndex (clamped; SIZE_MAX appends on top). The object id must be valid and unused.
	void insert(std::unique_ptr<Object> p_Object, size_t p_ZIndex = SIZE_MAX);
	// Removes the object and hands it back with the z-index it had
	[[nodiscard]] Removed take(ObjectId p_Id);
	// Applies p_Edit, then bumps the version and notifies listeners. No-op for unknown ids.
	// p_Changes (ObjectChange flags) describes what the edit may touch.
	void modify(ObjectId p_Id, const std::function<void(Object&)>& p_Edit, uint32_t p_Changes = ObjectChange::All);
	// Moves an object to a new z-index (clamped)
	void moveTo(ObjectId p_Id, size_t p_ZIndex);
	void clear();

	[[nodiscard]] const Object* find(ObjectId p_Id) const;
	[[nodiscard]] size_t indexOf(ObjectId p_Id) const; // SIZE_MAX when absent
	[[nodiscard]] std::span<const std::unique_ptr<Object>> objects() const { return m_Objects; }
	[[nodiscard]] size_t size() const { return m_Objects.size(); }
	[[nodiscard]] bool empty() const { return m_Objects.empty(); }
	[[nodiscard]] Rect contentBounds() const;

	// Incremented by every mutation
	[[nodiscard]] uint64_t revision() const { return m_Revision; }

	void addListener(DocumentListener* p_Listener);
	void removeListener(DocumentListener* p_Listener);

private:
	std::vector<std::unique_ptr<Object>> m_Objects; // back to front
	std::unordered_map<ObjectId, Object*> m_ById;
	std::vector<DocumentListener*> m_Listeners;
	ObjectId m_NextId = 1;
	uint64_t m_Revision = 0;
};
} // namespace wb
