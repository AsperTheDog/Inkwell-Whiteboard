// The state of a text box being edited: the string, the caret and selection, an input-method composition and an undo
// stack of its own. It knows nothing about fonts or lines; movement that depends on the layout (up, down, home, end,
// clicking) is done by the caller with wb.text.layout and applied with moveTo().
module;
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

export module wb.text.edit;

export namespace wb::text
{
class TextEditor
{
public:
	TextEditor() = default;
	// The caret starts after the last character
	explicit TextEditor(std::string p_Text);

	[[nodiscard]] const std::string& text() const { return m_Text; }
	[[nodiscard]] size_t caret() const { return m_Caret; }
	[[nodiscard]] size_t anchor() const { return m_Anchor; }
	[[nodiscard]] bool hasSelection() const { return m_Caret != m_Anchor; }
	[[nodiscard]] size_t selectionBegin() const { return m_Caret < m_Anchor ? m_Caret : m_Anchor; }
	[[nodiscard]] size_t selectionEnd() const { return m_Caret < m_Anchor ? m_Anchor : m_Caret; }
	[[nodiscard]] std::string selectedText() const { return m_Text.substr(selectionBegin(), selectionEnd() - selectionBegin()); }
	// Changes whenever the text changes
	[[nodiscard]] uint64_t revision() const { return m_Revision; }

	// ---- editing; each returns true when the text changed. Input is sanitized (see wb.text.utf8).
	bool insert(std::string_view p_Text);
	bool backspace(bool p_Word);
	bool erase(bool p_Word); // the Delete key
	bool deleteSelection();
	// Replaces everything (undoable)
	bool setText(std::string_view p_Text);

	// ---- movement; p_Extend keeps the anchor (Shift held)
	void moveLeft(bool p_Extend, bool p_Word);
	void moveRight(bool p_Extend, bool p_Word);
	void moveTo(size_t p_Byte, bool p_Extend);
	void selectAll();
	void selectWordAt(size_t p_Byte);

	// ---- input method composition: shown at the caret but not part of the text until committed with insert()
	void setComposition(std::string_view p_Text, size_t p_CursorInComposition);
	void clearComposition();
	[[nodiscard]] bool composing() const { return !m_Composition.empty(); }
	[[nodiscard]] const std::string& composition() const { return m_Composition; }
	// The text as it is displayed (composition included) and where the caret is in it
	[[nodiscard]] std::string displayText() const;
	[[nodiscard]] size_t displayCaret() const { return m_Caret + m_CompositionCursor; }
	// Maps a byte of the displayed text to the real text (bytes inside the composition map to the caret)
	[[nodiscard]] size_t realIndex(size_t p_DisplayByte) const;
	[[nodiscard]] size_t displayIndex(size_t p_RealByte) const;

	// ---- undo
	[[nodiscard]] bool canUndo() const { return !m_Undo.empty(); }
	[[nodiscard]] bool canRedo() const { return !m_Redo.empty(); }
	void undo();
	void redo();

private:
	struct Snapshot
	{
		std::string text;
		size_t caret = 0;
		size_t anchor = 0;
	};

	enum class Kind : uint8_t
	{
		None,
		Typing,
		Deleting,
		Other,
	};

	void remember(Kind p_Kind);
	void changed();
	void restore(const Snapshot& p_Snapshot);

	std::string m_Text;
	size_t m_Caret = 0;
	size_t m_Anchor = 0;
	std::string m_Composition;
	size_t m_CompositionCursor = 0;
	std::vector<Snapshot> m_Undo;
	std::vector<Snapshot> m_Redo;
	Kind m_LastKind = Kind::None;
	uint64_t m_Revision = 0;
};
} // namespace wb::text
