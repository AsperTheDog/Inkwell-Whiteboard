// Undo/redo. Every user-visible edit is a Command; History owns the applied ones.
module;
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

export module wb.doc.history;

import wb.doc.object;
import wb.doc.document;

export namespace wb
{
class Command
{
public:
	virtual ~Command() = default;
	virtual void apply(Document& p_Document) = 0;
	virtual void revert(Document& p_Document) = 0;
	[[nodiscard]] virtual std::string_view name() const = 0;
	// Absorbs p_Next (applied right after this one) into this command; return false to keep them separate
	virtual bool mergeWith(Command& p_Next)
	{
		(void)p_Next;
		return false;
	}
	// Collaboration: whether undoing or redoing this would be wrong after a remote peer changed object p_Id (or, when
	// p_Removed, deleted it). Such commands are dropped from the history instead of overwriting the peer's work.
	[[nodiscard]] virtual bool dependsOn(ObjectId p_Id, bool p_Removed) const
	{
		(void)p_Id;
		(void)p_Removed;
		return false;
	}
	// Collaboration: whether undoing or redoing this would bring back a picture or video that uses file p_Asset. Used when
	// that file was cancelled before it arrived: nothing may resurrect the object it belonged to.
	[[nodiscard]] virtual bool usesAsset(AssetId p_Asset) const
	{
		(void)p_Asset;
		return false;
	}
};

class History
{
public:
	explicit History(const size_t p_MaxEntries = 500) : m_MaxEntries(p_MaxEntries) {}

	// Applies the command and records it
	void execute(Document& p_Document, std::unique_ptr<Command> p_Command);
	// Records a command whose effect is already in the document
	void push(std::unique_ptr<Command> p_Command);

	bool undo(Document& p_Document);
	bool redo(Document& p_Document);
	void clear();

	// Drops every undo/redo entry that depends on one of p_Ids (see Command::dependsOn) and marks the history as
	// changed. Returns how many entries were dropped.
	size_t invalidate(std::span<const ObjectId> p_Ids, bool p_Removed);
	// Drops every undo/redo entry that would bring back an object using file p_Asset (see Command::usesAsset)
	size_t invalidateAsset(AssetId p_Asset);
	// The document changed through something other than this history (a remote edit): the board counts as modified
	void markExternalChange();

	[[nodiscard]] bool canUndo() const { return !m_Undo.empty(); }
	[[nodiscard]] bool canRedo() const { return !m_Redo.empty(); }
	[[nodiscard]] size_t undoCount() const { return m_Undo.size(); }
	[[nodiscard]] size_t redoCount() const { return m_Redo.size(); }
	[[nodiscard]] std::string_view undoName() const { return m_Undo.empty() ? std::string_view{} : m_Undo.back()->name(); }
	[[nodiscard]] std::string_view redoName() const { return m_Redo.empty() ? std::string_view{} : m_Redo.back()->name(); }

	// Changes whenever the undo state changes; compare against a saved value to know whether there are unsaved edits
	[[nodiscard]] uint64_t stateId() const { return m_StateId; }

	// While read-only (a collaboration guest the host only lets watch) nothing is executed, recorded, undone or redone
	void setReadOnly(const bool p_ReadOnly) { m_ReadOnly = p_ReadOnly; }
	[[nodiscard]] bool readOnly() const { return m_ReadOnly; }

	// Prevents the next push from merging into the previous command (e.g. at the end of a drag)
	void sealLast() { m_LastSealed = true; }

private:
	void trim();
	size_t prune(const std::function<bool(const Command&)>& p_Stale);

	std::vector<std::unique_ptr<Command>> m_Undo;
	std::vector<std::unique_ptr<Command>> m_Redo;
	size_t m_MaxEntries;
	uint64_t m_StateId = 0;
	uint64_t m_NextStateId = 1;
	std::vector<uint64_t> m_UndoStateIds; // state id after each undo entry
	std::vector<uint64_t> m_RedoStateIds;
	bool m_LastSealed = true;
	bool m_ReadOnly = false;
};
} // namespace wb
