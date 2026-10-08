// Editing the text of one text object: keyboard, pointer, input-method composition, selection and the object's undo
// entry. While a session is active the text object lives in the document in its current state (so it is drawn like
// any other), but nothing reaches the undo history until the session ends: then the whole session becomes a single
// "Add text" / "Edit text" step. Inside the session Ctrl+Z undoes typing.
module;
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

export module wb.editor.text_session;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.selection;
import wb.view.camera;
import wb.platform.input;
import wb.text.edit;
import wb.text.layout;
import wb.text.system;

export namespace wb
{
// What the overlay needs to draw the caret and selection, in the text box's layout coordinates (origin at the box's
// top-left corner, in object-local units)
struct TextEditView
{
	ObjectId id = INVALID_OBJECT_ID;
	Affine2 transform{}; // layout box -> world needs this plus the half-size shift (see localToWorld)
	Vec2 size{ 0.f };
	float fontSize = 0.f;
	float caretX = 0.f;
	float caretTop = 0.f;
	float caretHeight = 0.f;
	bool hasCaret = false;
	std::vector<Rect> selection;
	std::vector<Rect> composition; // the part still being composed by the input method
	uint64_t activity = 0;         // changes with every edit or caret move (the caret stops blinking then)

	// World position of a point of the layout box
	[[nodiscard]] DVec2 toWorld(const DVec2 p_Layout) const { return transform.apply(p_Layout - DVec2{ size } * 0.5); }
};

class TextSession
{
public:
	TextSession(Document& p_Document, History& p_History, Selection& p_Selection, const Camera& p_Camera, text::TextSystem& p_Text);

	[[nodiscard]] bool active() const { return m_Active; }
	[[nodiscard]] ObjectId object() const { return m_Id; }
	// True while a selection drag with the pointer is in progress
	[[nodiscard]] bool dragging() const { return m_Dragging; }

	// A new empty text box whose top-left corner (the middle of its first line) is at p_World
	void beginNew(DVec2 p_World, const TextData& p_Style);
	// p_Click places the caret there; without it the caret goes to the end
	void beginExisting(ObjectId p_Id, std::optional<DVec2> p_Click);
	// Finishes: keeps the text (an empty one is removed) and makes it one undo step
	void end();
	// Forgets the session without touching the document (it is being replaced)
	void abort();

	// Pointer events with the position in world coordinates. Returns true when the session used the event; a press
	// outside the text box ends the session and returns false so the tool can handle it.
	bool pointer(const platform::PointerEvent& p_Event, DVec2 p_World);
	// Returns true when the key belongs to the text (all typing keys, caret movement, clipboard...)
	bool key(const SDL_KeyboardEvent& p_Event);
	void textInput(std::string_view p_Text);
	// Composition by an input method. p_CursorCodepoints is SDL's cursor position inside the composition.
	void composition(std::string_view p_Text, int p_CursorCodepoints);

	// Text-level undo / redo (the history of this session)
	void undo();
	void redo();
	void selectAll();
	void copy();
	void cut();
	void paste();
	[[nodiscard]] bool hasSelection() const { return m_Active && m_Editor.hasSelection(); }

	// Changes the style of the text being edited (font, size, colour, alignment, wrap width...)
	void applyStyle(const std::function<void(TextData&)>& p_Edit);
	[[nodiscard]] const TextData& data() const { return m_Data; }

	[[nodiscard]] std::optional<TextEditView> view();

private:
	[[nodiscard]] bool insideBox(DVec2 p_World) const;
	[[nodiscard]] size_t indexAt(DVec2 p_World);
	void refresh();
	void touch() { ++m_Activity; }
	void moveVertical(int p_Lines, bool p_Extend);

	Document& m_Document;
	History& m_History;
	Selection& m_Selection;
	const Camera& m_Camera;
	text::TextSystem& m_TextSystem;

	bool m_Active = false;
	bool m_IsNew = false;
	ObjectId m_Id = INVALID_OBJECT_ID;
	TextData m_Data;           // the style being edited; its text field is not used while the session runs
	TextData m_BeforeData;
	Affine2 m_BeforeTransform{};
	text::TextEditor m_Editor;

	float m_PreferredX = 0.f;
	bool m_HasPreferredX = false;
	bool m_Dragging = false;
	uint64_t m_LastClickNs = 0;
	DVec2 m_LastClickScreen{ 0.0 };
	int m_ClickCount = 0;
	uint64_t m_Activity = 0;
};
} // namespace wb
