module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

module wb.doc.history;

import wb.doc.document;

namespace wb
{
void History::execute(Document& p_Document, std::unique_ptr<Command> p_Command)
{
	p_Command->apply(p_Document);
	push(std::move(p_Command));
}

void History::push(std::unique_ptr<Command> p_Command)
{
	m_Redo.clear();
	m_RedoStateIds.clear();

	if (!m_LastSealed && !m_Undo.empty() && m_Undo.back()->mergeWith(*p_Command))
	{
		m_StateId = m_NextStateId++;
		m_UndoStateIds.back() = m_StateId;
		return;
	}

	m_Undo.push_back(std::move(p_Command));
	m_StateId = m_NextStateId++;
	m_UndoStateIds.push_back(m_StateId);
	m_LastSealed = false;
	trim();
}

bool History::undo(Document& p_Document)
{
	if (m_Undo.empty())
		return false;

	std::unique_ptr<Command> l_Command = std::move(m_Undo.back());
	m_Undo.pop_back();
	const uint64_t l_State = m_UndoStateIds.back();
	m_UndoStateIds.pop_back();

	l_Command->revert(p_Document);
	m_Redo.push_back(std::move(l_Command));
	m_RedoStateIds.push_back(l_State);
	m_StateId = m_UndoStateIds.empty() ? 0 : m_UndoStateIds.back();
	m_LastSealed = true;
	return true;
}

bool History::redo(Document& p_Document)
{
	if (m_Redo.empty())
		return false;

	std::unique_ptr<Command> l_Command = std::move(m_Redo.back());
	m_Redo.pop_back();
	const uint64_t l_State = m_RedoStateIds.back();
	m_RedoStateIds.pop_back();

	l_Command->apply(p_Document);
	m_Undo.push_back(std::move(l_Command));
	m_UndoStateIds.push_back(l_State);
	m_StateId = l_State;
	m_LastSealed = true;
	return true;
}

void History::clear()
{
	m_Undo.clear();
	m_Redo.clear();
	m_UndoStateIds.clear();
	m_RedoStateIds.clear();
	m_StateId = 0;
	m_LastSealed = true;
}

void History::trim()
{
	if (m_Undo.size() <= m_MaxEntries)
		return;
	const size_t l_Excess = m_Undo.size() - m_MaxEntries;
	m_Undo.erase(m_Undo.begin(), m_Undo.begin() + static_cast<std::ptrdiff_t>(l_Excess));
	m_UndoStateIds.erase(m_UndoStateIds.begin(), m_UndoStateIds.begin() + static_cast<std::ptrdiff_t>(l_Excess));
}
} // namespace wb
