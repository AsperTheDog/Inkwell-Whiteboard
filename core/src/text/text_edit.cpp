module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module wb.text.edit;

import wb.text.utf8;

namespace wb::text
{
namespace
{
constexpr size_t MAX_UNDO = 256;
} // namespace

TextEditor::TextEditor(std::string p_Text) : m_Text(sanitize(p_Text))
{
	m_Caret = m_Anchor = m_Text.size();
}

void TextEditor::remember(const Kind p_Kind)
{
	// Consecutive typing (or deleting) is one undo step
	if (p_Kind != Kind::Other && p_Kind == m_LastKind)
		return;
	m_Undo.push_back(Snapshot{ m_Text, m_Caret, m_Anchor });
	if (m_Undo.size() > MAX_UNDO)
		m_Undo.erase(m_Undo.begin());
	m_Redo.clear();
	m_LastKind = p_Kind;
}

void TextEditor::changed()
{
	++m_Revision;
}

void TextEditor::restore(const Snapshot& p_Snapshot)
{
	m_Text = p_Snapshot.text;
	m_Caret = std::min(p_Snapshot.caret, m_Text.size());
	m_Anchor = std::min(p_Snapshot.anchor, m_Text.size());
	m_Composition.clear();
	m_LastKind = Kind::None;
	changed();
}

bool TextEditor::insert(const std::string_view p_Text)
{
	m_Composition.clear();
	m_CompositionCursor = 0;
	const std::string l_Clean = sanitize(p_Text);
	if (l_Clean.empty() && !hasSelection())
		return false;
	const bool l_Breaks = l_Clean.find_first_of(" \n\t") != std::string::npos || l_Clean.size() > 8;
	remember(hasSelection() || l_Breaks ? Kind::Other : Kind::Typing);
	const size_t l_Begin = selectionBegin();
	m_Text.replace(l_Begin, selectionEnd() - l_Begin, l_Clean);
	m_Caret = m_Anchor = l_Begin + l_Clean.size();
	if (l_Breaks)
		m_LastKind = Kind::None;
	changed();
	return true;
}

bool TextEditor::deleteSelection()
{
	if (!hasSelection())
		return false;
	remember(Kind::Other);
	const size_t l_Begin = selectionBegin();
	m_Text.erase(l_Begin, selectionEnd() - l_Begin);
	m_Caret = m_Anchor = l_Begin;
	changed();
	return true;
}

bool TextEditor::backspace(const bool p_Word)
{
	if (hasSelection())
		return deleteSelection();
	if (m_Caret == 0)
		return false;
	remember(Kind::Deleting);
	const size_t l_From = p_Word ? previousWord(m_Text, m_Caret) : previousCluster(m_Text, m_Caret);
	m_Text.erase(l_From, m_Caret - l_From);
	m_Caret = m_Anchor = l_From;
	changed();
	return true;
}

bool TextEditor::erase(const bool p_Word)
{
	if (hasSelection())
		return deleteSelection();
	if (m_Caret >= m_Text.size())
		return false;
	remember(Kind::Deleting);
	const size_t l_To = p_Word ? nextWord(m_Text, m_Caret) : nextCluster(m_Text, m_Caret);
	m_Text.erase(m_Caret, l_To - m_Caret);
	changed();
	return true;
}

bool TextEditor::setText(const std::string_view p_Text)
{
	const std::string l_Clean = sanitize(p_Text);
	if (l_Clean == m_Text)
		return false;
	remember(Kind::Other);
	m_Text = l_Clean;
	m_Caret = m_Anchor = m_Text.size();
	m_LastKind = Kind::None;
	changed();
	return true;
}

void TextEditor::moveLeft(const bool p_Extend, const bool p_Word)
{
	m_LastKind = Kind::None;
	if (hasSelection() && !p_Extend)
	{
		m_Caret = m_Anchor = selectionBegin();
		return;
	}
	m_Caret = p_Word ? previousWord(m_Text, m_Caret) : previousCluster(m_Text, m_Caret);
	if (!p_Extend)
		m_Anchor = m_Caret;
}

void TextEditor::moveRight(const bool p_Extend, const bool p_Word)
{
	m_LastKind = Kind::None;
	if (hasSelection() && !p_Extend)
	{
		m_Caret = m_Anchor = selectionEnd();
		return;
	}
	m_Caret = p_Word ? nextWord(m_Text, m_Caret) : nextCluster(m_Text, m_Caret);
	if (!p_Extend)
		m_Anchor = m_Caret;
}

void TextEditor::moveTo(const size_t p_Byte, const bool p_Extend)
{
	m_LastKind = Kind::None;
	m_Caret = std::min(p_Byte, m_Text.size());
	if (!p_Extend)
		m_Anchor = m_Caret;
}

void TextEditor::selectAll()
{
	m_LastKind = Kind::None;
	m_Anchor = 0;
	m_Caret = m_Text.size();
}

void TextEditor::selectWordAt(const size_t p_Byte)
{
	m_LastKind = Kind::None;
	wordRange(m_Text, p_Byte, m_Anchor, m_Caret);
}

void TextEditor::setComposition(const std::string_view p_Text, const size_t p_CursorInComposition)
{
	if (p_Text.empty())
	{
		clearComposition();
		return;
	}
	if (hasSelection())
		deleteSelection();
	m_Composition = sanitize(p_Text);
	m_CompositionCursor = std::min(p_CursorInComposition, m_Composition.size());
}

void TextEditor::clearComposition()
{
	m_Composition.clear();
	m_CompositionCursor = 0;
}

std::string TextEditor::displayText() const
{
	if (m_Composition.empty())
		return m_Text;
	std::string l_Text = m_Text;
	l_Text.insert(m_Caret, m_Composition);
	return l_Text;
}

size_t TextEditor::realIndex(const size_t p_DisplayByte) const
{
	if (m_Composition.empty() || p_DisplayByte <= m_Caret)
		return p_DisplayByte;
	if (p_DisplayByte < m_Caret + m_Composition.size())
		return m_Caret;
	return p_DisplayByte - m_Composition.size();
}

size_t TextEditor::displayIndex(const size_t p_RealByte) const
{
	if (m_Composition.empty() || p_RealByte <= m_Caret)
		return p_RealByte;
	return p_RealByte + m_Composition.size();
}

void TextEditor::undo()
{
	if (m_Undo.empty())
		return;
	m_Redo.push_back(Snapshot{ m_Text, m_Caret, m_Anchor });
	const Snapshot l_Snapshot = std::move(m_Undo.back());
	m_Undo.pop_back();
	restore(l_Snapshot);
}

void TextEditor::redo()
{
	if (m_Redo.empty())
		return;
	m_Undo.push_back(Snapshot{ m_Text, m_Caret, m_Anchor });
	const Snapshot l_Snapshot = std::move(m_Redo.back());
	m_Redo.pop_back();
	restore(l_Snapshot);
}
} // namespace wb::text
