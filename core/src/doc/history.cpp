module;
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

module wb.doc.history;

import wb.doc.object;
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

size_t History::invalidateAsset(const AssetId p_Asset)
{
	return prune([&](const Command& p_Command) { return p_Command.usesAsset(p_Asset); });
}

size_t History::invalidate(const std::span<const ObjectId> p_Ids, const bool p_Removed)
{
	return prune([&](const Command& p_Command) { return std::ranges::any_of(p_Ids, [&](const ObjectId p_Id) { return p_Command.dependsOn(p_Id, p_Removed); }); });
}

size_t History::prune(const std::function<bool(const Command&)>& p_Stale)
{
	const auto l_Stale = [&](const std::unique_ptr<Command>& p_Command) { return p_Stale(*p_Command); };
	const auto l_Prune = [&](std::vector<std::unique_ptr<Command>>& p_Commands, std::vector<uint64_t>& p_States)
	{
		size_t l_Dropped = 0;
		for (size_t i = p_Commands.size(); i-- > 0;)
		{
			if (!l_Stale(p_Commands[i]))
				continue;
			p_Commands.erase(p_Commands.begin() + static_cast<std::ptrdiff_t>(i));
			p_States.erase(p_States.begin() + static_cast<std::ptrdiff_t>(i));
			++l_Dropped;
		}
		return l_Dropped;
	};

	const size_t l_Dropped = l_Prune(m_Undo, m_UndoStateIds) + l_Prune(m_Redo, m_RedoStateIds);
	if (l_Dropped != 0)
		m_LastSealed = true;
	markExternalChange();
	return l_Dropped;
}

void History::markExternalChange()
{
	m_StateId = m_NextStateId++;
	if (!m_UndoStateIds.empty())
		m_UndoStateIds.back() = m_StateId;
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
