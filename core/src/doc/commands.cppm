// Standard document commands. Each restores the exact z-order on undo.
module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module wb.doc.commands;

import wb.math;
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
// Gives objects new transforms. Used for drags (already applied live, then pushed) and for nudges, which merge
// into one undo step while the history allows it.
class TransformObjectsCommand final : public Command
{
public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		Affine2 before{};
		Affine2 after{};
	};

	TransformObjectsCommand(std::vector<Entry> p_Entries, std::string p_Name, bool p_Mergeable = false);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	bool mergeWith(Command& p_Next) override;
	[[nodiscard]] bool changesAnything() const;

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
	bool m_Mergeable = false;
};

enum class ZOrderMove : uint8_t
{
	ToFront,
	ToBack,
	Forward,  // one step up past the next object that is not part of the move
	Backward,
};

// Changes the stacking order of some objects, keeping their relative order
class ReorderObjectsCommand final : public Command
{
public:
	ReorderObjectsCommand(std::vector<ObjectId> p_Ids, ZOrderMove p_Move, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	// After apply(): whether the order really changed (moving the top object forward changes nothing)
	[[nodiscard]] bool changedOrder() const { return m_Changed; }

private:
	std::vector<ObjectId> m_Ids;
	ZOrderMove m_Move;
	bool m_Changed = false;
	std::vector<std::pair<ObjectId, size_t>> m_OldPositions; // ascending by position
	std::string m_Name;
};

// Recolours strokes (the alpha channel of each stroke is kept)
class SetStrokeColorCommand final : public Command
{
public:
	SetStrokeColorCommand(std::vector<ObjectId> p_Ids, Color p_Color, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }

private:
	std::vector<ObjectId> m_Ids;
	std::vector<Color> m_Old;
	Color m_New{};
	std::string m_Name;
};
} // namespace wb
