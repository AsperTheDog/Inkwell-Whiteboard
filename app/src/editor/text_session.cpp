module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

module wb.editor.text_session;

import wb.math;
import wb.doc.commands;
import wb.doc.document;
import wb.doc.edit;
import wb.doc.history;
import wb.doc.object;
import wb.doc.selection;
import wb.view.camera;
import wb.platform.input;
import wb.text.edit;
import wb.text.layout;
import wb.text.system;
import wb.text.utf8;

namespace wb
{
namespace
{
constexpr uint64_t MULTI_CLICK_NS = 450'000'000ull;
constexpr double MULTI_CLICK_PIXELS = 8.0;
constexpr double BOX_GRAB_POINTS = 6.0; // a press this close to the box still counts as inside
constexpr int PAGE_LINES = 8;

bool isBlank(const std::string_view p_Text)
{
	for (const char l_Char : p_Text)
	{
		if (l_Char != ' ' && l_Char != '\n' && l_Char != '\t' && l_Char != '\r')
			return false;
	}
	return true;
}
} // namespace

TextSession::TextSession(Document& p_Document, History& p_History, Selection& p_Selection, const Camera& p_Camera, text::TextSystem& p_Text)
	: m_Document(p_Document), m_History(p_History), m_Selection(p_Selection), m_Camera(p_Camera), m_TextSystem(p_Text)
{
}

// ------------------------------------------------------------------------------------------------ lifetime

void TextSession::beginNew(const DVec2 p_World, const TextData& p_Style)
{
	end();
	TextData l_Data = p_Style;
	l_Data.text.clear();
	l_Data.size = m_TextSystem.measure(l_Data);

	// The click is the left edge (or centre line, or right edge) and the middle of the first line
	const double l_Fraction = l_Data.align == TextAlign::Left ? 0.0 : (l_Data.align == TextAlign::Center ? 0.5 : 1.0);
	const DVec2 l_Anchor{ (l_Fraction - 0.5) * l_Data.size.x, -0.5 * l_Data.size.y };
	const DVec2 l_Center = p_World + DVec2{ 0.0, 0.5 * l_Data.size.y } - l_Anchor;

	auto l_Object = std::make_unique<Object>();
	l_Object->id = m_Document.allocateId();
	l_Object->transform = Affine2::translate(l_Center);
	l_Object->payload = l_Data;
	m_Id = l_Object->id;
	m_Document.insert(std::move(l_Object));

	m_Active = true;
	m_IsNew = true;
	m_Data = l_Data;
	m_BeforeData = l_Data;
	m_BeforeTransform = Affine2::translate(l_Center);
	m_Editor = text::TextEditor();
	m_HasPreferredX = false;
	m_Dragging = false;
	m_Selection.clear();
	touch();
}

void TextSession::beginExisting(const ObjectId p_Id, const std::optional<DVec2> p_Click)
{
	end();
	const Object* l_Object = m_Document.find(p_Id);
	const TextData* l_Text = l_Object != nullptr ? l_Object->text() : nullptr;
	if (l_Text == nullptr)
		return;
	m_Id = p_Id;
	m_Data = *l_Text;
	m_BeforeData = *l_Text;
	m_BeforeTransform = l_Object->transform;
	m_Editor = text::TextEditor(l_Text->text);
	m_Active = true;
	m_IsNew = false;
	m_HasPreferredX = false;
	m_Dragging = false;
	m_Selection.set(std::vector<ObjectId>{ p_Id });
	if (p_Click)
		m_Editor.moveTo(indexAt(*p_Click), false);
	touch();
}

void TextSession::abort()
{
	m_Active = false;
	m_Dragging = false;
	m_Id = INVALID_OBJECT_ID;
}

void TextSession::end()
{
	if (!m_Active)
		return;
	m_Active = false;
	m_Dragging = false;
	const ObjectId l_Id = m_Id;
	m_Id = INVALID_OBJECT_ID;

	m_Editor.clearComposition();
	const Object* l_Object = m_Document.find(l_Id);
	if (l_Object == nullptr || l_Object->text() == nullptr)
		return;

	TextData l_Final = *l_Object->text();
	l_Final.text = m_Editor.text();
	l_Final.size = m_TextSystem.measure(l_Final);
	const Affine2 l_FinalTransform = anchoredTextTransform(l_Object->transform, l_Object->text()->size, l_Final.size, l_Final.align);
	const bool l_Empty = isBlank(l_Final.text);

	if (m_IsNew)
	{
		Document::Removed l_Removed = m_Document.take(l_Id);
		if (l_Empty || l_Removed.object == nullptr)
			return;
		Object& l_Live = *l_Removed.object;
		l_Live.payload = l_Final;
		l_Live.transform = l_FinalTransform;
		l_Live.refreshBounds();
		std::vector<std::unique_ptr<Object>> l_Objects;
		l_Objects.push_back(std::move(l_Removed.object));
		m_History.execute(m_Document, std::make_unique<AddObjectsCommand>(std::move(l_Objects), "Add text"));
		m_Selection.set(std::vector<ObjectId>{ l_Id });
		return;
	}

	// Put the box back as it was so the history entry starts from there
	const auto l_Restore = [&](const TextData& p_Data, const Affine2& p_Transform)
	{
		m_Document.modify(l_Id, [&](Object& p_Object)
		{
			*p_Object.text() = p_Data;
			p_Object.transform = p_Transform;
		});
	};
	if (l_Empty)
	{
		l_Restore(m_BeforeData, m_BeforeTransform);
		const std::vector<ObjectId> l_Ids{ l_Id };
		deleteObjects(m_Document, m_History, l_Ids);
		return;
	}
	if (l_Final == m_BeforeData && l_FinalTransform.translation == m_BeforeTransform.translation)
	{
		l_Restore(m_BeforeData, m_BeforeTransform);
		return;
	}
	l_Restore(l_Final, l_FinalTransform);
	std::vector<SetTextCommand::Entry> l_Entries;
	l_Entries.push_back(SetTextCommand::Entry{ .id = l_Id, .before = m_BeforeData, .after = l_Final, .transformBefore = m_BeforeTransform, .transformAfter = l_FinalTransform });
	m_History.push(std::make_unique<SetTextCommand>(std::move(l_Entries), "Edit text"));
}

// ------------------------------------------------------------------------------------------------ document sync

void TextSession::refresh()
{
	if (!m_Active)
		return;
	const Object* l_Object = m_Document.find(m_Id);
	if (l_Object == nullptr || l_Object->text() == nullptr)
	{
		abort();
		return;
	}
	TextData l_Data = m_Data;
	l_Data.text = m_Editor.displayText();
	l_Data.size = m_TextSystem.measure(l_Data);
	const Affine2 l_Transform = anchoredTextTransform(l_Object->transform, l_Object->text()->size, l_Data.size, l_Data.align);
	m_Data.size = l_Data.size;
	m_Document.modify(m_Id, [&](Object& p_Object)
	{
		*p_Object.text() = l_Data;
		p_Object.transform = l_Transform;
	});
	touch();
}

void TextSession::applyStyle(const std::function<void(TextData&)>& p_Edit)
{
	if (!m_Active)
		return;
	p_Edit(m_Data);
	m_Data.text.clear();
	refresh();
}

// ------------------------------------------------------------------------------------------------ geometry

bool TextSession::insideBox(const DVec2 p_World) const
{
	const Object* l_Object = m_Document.find(m_Id);
	if (l_Object == nullptr || !l_Object->transform.isInvertible())
		return false;
	const DVec2 l_Local = l_Object->transform.inverse().apply(p_World);
	const double l_Grab = BOX_GRAB_POINTS / std::max(m_Camera.pixelsPerUnit() * l_Object->transform.uniformScale(), 1e-12);
	return l_Object->text()->localBounds().inflated(l_Grab).contains(l_Local);
}

size_t TextSession::indexAt(const DVec2 p_World)
{
	const Object* l_Object = m_Document.find(m_Id);
	if (l_Object == nullptr || !l_Object->transform.isInvertible())
		return m_Editor.caret();
	TextData l_Data = m_Data;
	l_Data.text = m_Editor.displayText();
	const text::TextLayout& l_Layout = m_TextSystem.layout(l_Data);
	const DVec2 l_Local = l_Object->transform.inverse().apply(p_World) + DVec2{ l_Layout.size } * 0.5;
	return m_Editor.realIndex(text::indexAtPoint(l_Layout, Vec2{ l_Local }));
}

std::optional<TextEditView> TextSession::view()
{
	if (!m_Active)
		return std::nullopt;
	const Object* l_Object = m_Document.find(m_Id);
	if (l_Object == nullptr || l_Object->text() == nullptr)
		return std::nullopt;
	TextData l_Data = m_Data;
	l_Data.text = m_Editor.displayText();
	const text::TextLayout& l_Layout = m_TextSystem.layout(l_Data);

	TextEditView l_View;
	l_View.id = m_Id;
	l_View.transform = l_Object->transform;
	l_View.size = l_Layout.size;
	l_View.fontSize = l_Data.fontSize;
	l_View.activity = m_Activity;
	const text::CaretPosition l_Caret = text::caretAt(l_Layout, m_Editor.displayCaret());
	l_View.hasCaret = true;
	l_View.caretX = l_Caret.x;
	l_View.caretTop = l_Caret.top;
	l_View.caretHeight = l_Caret.height;
	if (m_Editor.composing())
	{
		const size_t l_Begin = m_Editor.caret();
		l_View.composition = text::selectionRects(l_Layout, l_Begin, l_Begin + m_Editor.composition().size());
	}
	else if (m_Editor.hasSelection())
	{
		l_View.selection = text::selectionRects(l_Layout, m_Editor.selectionBegin(), m_Editor.selectionEnd());
	}
	return l_View;
}

// ------------------------------------------------------------------------------------------------ input

bool TextSession::pointer(const platform::PointerEvent& p_Event, const DVec2 p_World)
{
	if (!m_Active)
		return false;
	if (m_Dragging)
	{
		if (p_Event.phase == platform::PointerPhase::Move)
		{
			m_Editor.moveTo(indexAt(p_World), true);
			m_HasPreferredX = false;
			touch();
		}
		else if (p_Event.phase == platform::PointerPhase::Up || p_Event.phase == platform::PointerPhase::Cancel)
		{
			m_Dragging = false;
		}
		return true;
	}
	if (p_Event.phase != platform::PointerPhase::Down || p_Event.button != platform::PointerButton::Primary)
		return false;
	if (!insideBox(p_World))
	{
		end();
		return false;
	}

	// Count quick presses at the same spot: double selects a word, triple everything
	const DVec2 l_Screen{ p_Event.position };
	const bool l_Repeat = m_ClickCount > 0 && p_Event.timestampNs - m_LastClickNs < MULTI_CLICK_NS && glm::length(l_Screen - m_LastClickScreen) < MULTI_CLICK_PIXELS;
	m_ClickCount = l_Repeat ? m_ClickCount + 1 : 1;
	m_LastClickNs = p_Event.timestampNs;
	m_LastClickScreen = l_Screen;

	const size_t l_Index = indexAt(p_World);
	const bool l_Extend = (p_Event.modifiers & platform::Modifier::Shift) != 0;
	if (m_ClickCount == 2)
	{
		m_Editor.selectWordAt(l_Index);
	}
	else if (m_ClickCount >= 3)
	{
		m_Editor.selectAll();
		m_ClickCount = 0;
	}
	else
	{
		m_Editor.moveTo(l_Index, l_Extend);
	}
	m_Dragging = m_ClickCount == 1;
	m_HasPreferredX = false;
	touch();
	return true;
}

void TextSession::moveVertical(const int p_Lines, const bool p_Extend)
{
	TextData l_Data = m_Data;
	l_Data.text = m_Editor.displayText();
	const text::TextLayout& l_Layout = m_TextSystem.layout(l_Data);
	const size_t l_From = m_Editor.displayCaret();
	if (!m_HasPreferredX)
	{
		m_PreferredX = text::caretAt(l_Layout, l_From).x;
		m_HasPreferredX = true;
	}
	const size_t l_To = text::moveVertically(l_Layout, l_From, p_Lines, m_PreferredX);
	m_Editor.moveTo(m_Editor.realIndex(l_To), p_Extend);
}

bool TextSession::key(const SDL_KeyboardEvent& p_Event)
{
	if (!m_Active)
		return false;
	const bool l_Ctrl = (p_Event.mod & SDL_KMOD_CTRL) != 0;
	const bool l_Shift = (p_Event.mod & SDL_KMOD_SHIFT) != 0;
	const bool l_Alt = (p_Event.mod & SDL_KMOD_ALT) != 0;
	const SDL_Keycode l_Key = p_Event.key;

	// While an input method is composing, it owns the keys
	if (m_Editor.composing() && l_Key != SDLK_ESCAPE)
		return true;

	bool l_KeepPreferredX = false;
	bool l_Changed = false;
	switch (l_Key)
	{
	case SDLK_ESCAPE:
	{
		const ObjectId l_Id = m_Id;
		end();
		if (m_Document.find(l_Id) != nullptr)
			m_Selection.set(std::vector<ObjectId>{ l_Id });
		return true;
	}
	case SDLK_LEFT:
		m_Editor.moveLeft(l_Shift, l_Ctrl);
		break;
	case SDLK_RIGHT:
		m_Editor.moveRight(l_Shift, l_Ctrl);
		break;
	case SDLK_UP:
		moveVertical(-1, l_Shift);
		l_KeepPreferredX = true;
		break;
	case SDLK_DOWN:
		moveVertical(1, l_Shift);
		l_KeepPreferredX = true;
		break;
	case SDLK_PAGEUP:
		moveVertical(-PAGE_LINES, l_Shift);
		l_KeepPreferredX = true;
		break;
	case SDLK_PAGEDOWN:
		moveVertical(PAGE_LINES, l_Shift);
		l_KeepPreferredX = true;
		break;
	case SDLK_HOME:
	case SDLK_END:
	{
		const bool l_Home = l_Key == SDLK_HOME;
		if (l_Ctrl)
		{
			m_Editor.moveTo(l_Home ? 0 : m_Editor.text().size(), l_Shift);
		}
		else
		{
			const text::TextLayout& l_Layout = m_TextSystem.layout([&] { TextData l_Data = m_Data; l_Data.text = m_Editor.text(); return l_Data; }());
			m_Editor.moveTo(l_Home ? text::lineStart(l_Layout, m_Editor.caret()) : text::lineEnd(l_Layout, m_Editor.caret()), l_Shift);
		}
		break;
	}
	case SDLK_BACKSPACE:
		l_Changed = m_Editor.backspace(l_Ctrl);
		break;
	case SDLK_DELETE:
		l_Changed = m_Editor.erase(l_Ctrl);
		break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		if (l_Ctrl)
		{
			end();
			return true;
		}
		l_Changed = m_Editor.insert("\n");
		break;
	case SDLK_TAB:
		l_Changed = m_Editor.insert("\t");
		break;
	default:
		if (l_Ctrl && !l_Alt)
		{
			switch (l_Key)
			{
			case SDLK_A:
				selectAll();
				return true;
			case SDLK_C:
				copy();
				return true;
			case SDLK_X:
				cut();
				return true;
			case SDLK_V:
				paste();
				return true;
			case SDLK_Z:
				l_Shift ? redo() : undo();
				return true;
			case SDLK_Y:
				redo();
				return true;
			case SDLK_B:
				applyStyle([](TextData& p_Data) { p_Data.style ^= TextStyle::Bold; });
				return true;
			case SDLK_I:
				applyStyle([](TextData& p_Data) { p_Data.style ^= TextStyle::Italic; });
				return true;
			default:
				return false; // Ctrl+S and friends belong to the application
			}
		}
		// Typing keys are consumed so the tool shortcuts (P, E, V...) do not fire; the characters arrive as text input
		return l_Key < 0x40000000 && l_Key != SDLK_UNKNOWN && !l_Ctrl && !l_Alt;
	}

	if (!l_KeepPreferredX)
		m_HasPreferredX = false;
	if (l_Changed)
		refresh();
	else
		touch();
	return true;
}

void TextSession::textInput(const std::string_view p_Text)
{
	if (!m_Active)
		return;
	if (m_Editor.insert(p_Text))
	{
		m_HasPreferredX = false;
		refresh();
	}
}

void TextSession::composition(const std::string_view p_Text, const int p_CursorCodepoints)
{
	if (!m_Active)
		return;
	size_t l_Cursor = p_Text.size();
	if (p_CursorCodepoints >= 0)
	{
		size_t l_Index = 0;
		for (int i = 0; i < p_CursorCodepoints && l_Index < p_Text.size(); ++i)
			text::decodeNext(p_Text, l_Index);
		l_Cursor = l_Index;
	}
	m_Editor.setComposition(p_Text, l_Cursor);
	refresh();
}

void TextSession::undo()
{
	if (!m_Active)
		return;
	m_Editor.undo();
	m_HasPreferredX = false;
	refresh();
}

void TextSession::redo()
{
	if (!m_Active)
		return;
	m_Editor.redo();
	m_HasPreferredX = false;
	refresh();
}

void TextSession::selectAll()
{
	if (!m_Active)
		return;
	m_Editor.selectAll();
	touch();
}

void TextSession::copy()
{
	if (m_Active && m_Editor.hasSelection())
		SDL_SetClipboardText(m_Editor.selectedText().c_str());
}

void TextSession::cut()
{
	if (!m_Active || !m_Editor.hasSelection())
		return;
	copy();
	if (m_Editor.deleteSelection())
		refresh();
}

void TextSession::paste()
{
	if (!m_Active || !SDL_HasClipboardText())
		return;
	char* l_Text = SDL_GetClipboardText();
	if (l_Text == nullptr)
		return;
	const std::string l_Pasted(l_Text);
	SDL_free(l_Text);
	if (m_Editor.insert(l_Pasted))
	{
		m_HasPreferredX = false;
		refresh();
	}
}
} // namespace wb
