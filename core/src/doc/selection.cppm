// The set of selected objects. It listens to the document so ids of objects that disappear (deleted, erased,
// undone...) drop out by themselves.
module;
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

export module wb.doc.selection;

import wb.math;
import wb.doc.object;
import wb.doc.document;

export namespace wb
{
class Selection final : public DocumentListener
{
public:
	explicit Selection(Document& p_Document);
	~Selection() override;
	Selection(const Selection&) = delete;
	Selection& operator=(const Selection&) = delete;

	void clear();
	// Replaces the selection (ids that are not in the document are ignored)
	void set(std::span<const ObjectId> p_Ids);
	void add(ObjectId p_Id);
	void remove(ObjectId p_Id);
	void toggle(ObjectId p_Id);

	[[nodiscard]] bool contains(const ObjectId p_Id) const { return m_Set.contains(p_Id); }
	[[nodiscard]] bool empty() const { return m_Ids.empty(); }
	[[nodiscard]] size_t size() const { return m_Ids.size(); }
	// In selection order
	[[nodiscard]] std::span<const ObjectId> ids() const { return m_Ids; }
	// Back-to-front (document order); what editing commands want
	[[nodiscard]] std::vector<ObjectId> orderedIds() const;
	// Union of the objects' tight world bounds
	[[nodiscard]] Rect bounds() const;

	// Changes whenever the set of selected objects changes
	[[nodiscard]] uint64_t revision() const { return m_Revision; }

	// DocumentListener
	void onObjectAdded(const Object&) override {}
	void onObjectRemoved(const Object& p_Object) override;
	void onObjectChanged(const Object&, uint32_t) override {}
	void onDocumentCleared() override;

private:
	Document& m_Document;
	std::vector<ObjectId> m_Ids;
	std::unordered_set<ObjectId> m_Set;
	uint64_t m_Revision = 0;
};
} // namespace wb
