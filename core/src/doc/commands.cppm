// Standard document commands. Each restores the exact z-order on undo.
module;
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module wb.doc.commands;

import wb.doc.document;
import wb.doc.history;
import wb.doc.object;

export namespace wb
{
// Adds new objects on top of the board
class AddObjectsCommand final : public Command
{
public:
	AddObjectsCommand(std::vector<std::unique_ptr<Object>> p_Objects, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }

private:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		std::unique_ptr<Object> object; // owned while not in the document
		size_t zIndex = SIZE_MAX;       // SIZE_MAX until first applied (append)
	};
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Removes objects (and puts them back at their original z-index on undo)
class RemoveObjectsCommand final : public Command
{
public:
	RemoveObjectsCommand(std::vector<ObjectId> p_Ids, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }

private:
	std::vector<ObjectId> m_Ids;
	std::vector<Document::Removed> m_Removed; // sorted by z-index, ascending
	std::string m_Name;
};
} // namespace wb
