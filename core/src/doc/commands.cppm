// Standard document commands. Each restores the exact z-order on undo.
module;
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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

// A batch of "replace one object by zero or more others, at the same z-position" steps (the eraser's cuts).
// Steps are performed live through replace() while a gesture is in progress; the finished command is then pushed
// to the history, so the whole gesture is a single undo step.
class ReplaceObjectsCommand final : public Command
{
public:
	explicit ReplaceObjectsCommand(std::string p_Name) : m_Name(std::move(p_Name)) {}

	// Performs the replacement now and records it. Unknown ids are ignored.
	void replace(Document& p_Document, ObjectId p_Id, std::vector<std::unique_ptr<Object>> p_Added);
	[[nodiscard]] bool empty() const { return m_Steps.empty(); }

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }

private:
	struct Step
	{
		ObjectId removedId = INVALID_OBJECT_ID;
		size_t zIndex = 0;
		std::unique_ptr<Object> removed;            // owned while the step is applied (the object is out of the document)
		std::vector<ObjectId> addedIds;
		std::vector<std::unique_ptr<Object>> added; // owned while the step is reverted
	};
	std::vector<Step> m_Steps;
	std::string m_Name;
};
} // namespace wb
