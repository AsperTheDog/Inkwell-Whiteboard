// App: notes. A note is a description kept with an object or a group. It is written and read in a floating panel instead
// of on the board, and a small marker on the board shows which objects have one.
module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <volk.h>

module wb.app;

import wb.doc.document;
import wb.doc.object;
import wb.editor;
import wb.io.locale;
import wb.math;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.render.image_store;
import wb.session;
import wb.tools.tool;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;
import wb.view.camera;

namespace wb
{
namespace
{
constexpr uint64_t NO_SELECTION = UINT64_MAX;
} // namespace

void App::toggleNotesPanel()
{
	// Open: close it (a note that opened by itself is closed too). Closed: open it and keep it open for other selections.
	m_NotesOpen = !m_NotesOpen;
	m_NotesPinned = m_NotesOpen;
	if (!m_NotesOpen)
		m_Ui.blurField();
	requestRedraw();
}

void App::buildNotesPanel()
{
	const bool l_Visible = m_Editor.hasSelection() && !m_Editor.isBusy() && !modalOpen() && !m_Editor.textEditing() && !m_Editor.cropping();
	const uint64_t l_Key = l_Visible ? m_Editor.selection().revision() : NO_SELECTION;
	if (l_Key != m_NoteKey)
	{
		m_NoteKey = l_Key;
		m_NoteTargets.clear();
		m_NoteDraft.clear();
		if (l_Visible)
		{
			m_NoteTargets = m_Editor.selection().orderedIds();
			const Editor::SelectionNote l_Note = m_Editor.selectionNote();
			m_NotesOpen = m_NotesPinned || !l_Note.text.empty() || l_Note.mixed;
		}
		else
		{
			m_NotesOpen = false;
		}
	}
	if (!l_Visible || !m_NotesOpen)
		return;

	// Somebody else's change shows up while this person is not typing
	const bool l_Typing = m_Ui.fieldFocused("note.text");
	const Editor::SelectionNote l_Shared = m_Editor.selectionNote();
	if (!l_Typing)
		m_NoteDraft = l_Shared.text;

	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Width = std::min(m_Ui.px(300.f), l_Viewport.x - m_Ui.px(28.f));
	const float l_Height = std::min(m_Ui.px(270.f), l_Viewport.y - m_Ui.px(180.f));
	if (l_Width < m_Ui.px(120.f) || l_Height < m_Ui.px(120.f))
		return;
	const ui::Rect2 l_Panel = ui::Rect2::fromPosSize(Vec2{ l_Viewport.x - l_Width - m_Ui.px(14.f), m_Ui.px(84.f) }, Vec2{ l_Width, l_Height });
	m_Ui.panel(l_Panel, m_Ui.px(18.f));

	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const float l_Pad = m_Ui.px(12.f);
	const float l_HeaderHeight = m_Ui.px(34.f);
	l_Draw.icon(ui::Icon::Note, Vec2{ l_Panel.min.x + l_Pad + m_Ui.px(8.f), l_Panel.min.y + l_Pad + l_HeaderHeight * 0.5f }, m_Ui.fontPx(16.f), l_Theme.accent);
	l_Draw.text(Vec2{ l_Panel.min.x + l_Pad + m_Ui.px(28.f), l_Panel.min.y + l_Pad + l_HeaderHeight * 0.5f }, tr("notes.title"), m_Ui.fontPx(14.5f), l_Theme.text);
	const ui::Rect2 l_Close = ui::Rect2::fromPosSize(Vec2{ l_Panel.max.x - l_Pad - l_HeaderHeight, l_Panel.min.y + l_Pad }, Vec2{ l_HeaderHeight });
	if (m_Ui.iconButton("note.close", l_Close, ui::Icon::Close, false, true, tr("notes.hide")))
	{
		m_Ui.blurField();
		m_NotesPinned = false;
		m_NotesOpen = false;
		requestRedraw();
		return;
	}

	const bool l_ReadOnly = m_Editor.readOnly();
	const std::string_view l_Hint = l_ReadOnly ? tr("notes.readOnly") : (l_Shared.mixed ? tr("notes.mixed") : std::string_view{});
	const int l_HintPx = m_Ui.fontPx(12.f);
	const float l_HintHeight = l_Hint.empty() ? 0.f : m_Ui.px(34.f);
	const ui::Rect2 l_Area = ui::Rect2{ Vec2{ l_Panel.min.x + l_Pad, l_Panel.min.y + l_Pad + l_HeaderHeight + m_Ui.px(6.f) }, Vec2{ l_Panel.max.x - l_Pad, l_Panel.max.y - l_Pad - l_HintHeight } };
	if (l_ReadOnly)
	{
		std::string l_Copy = m_NoteDraft; // can be selected and copied, not changed
		(void)m_Ui.textArea("note.text", l_Area, l_Copy, tr("notes.placeholder"), MAX_NOTE_BYTES);
	}
	else if (m_Ui.textArea("note.text", l_Area, m_NoteDraft, tr("notes.placeholder"), MAX_NOTE_BYTES))
	{
		// Every character goes to the board (and to the others) as it is typed; typing in a row is one undo step
		(void)m_Editor.setNote(m_NoteTargets, m_NoteDraft);
	}
	if (!l_Hint.empty())
	{
		// Wrapped by hand: the hint is a sentence
		const float l_HintWidth = l_Area.width();
		std::string l_Line;
		float l_Y = l_Area.max.y + m_Ui.px(14.f);
		size_t l_Pos = 0;
		const std::string l_Text(l_Hint);
		while (l_Pos < l_Text.size())
		{
			size_t l_End = l_Text.find(' ', l_Pos);
			const std::string l_Word = l_Text.substr(l_Pos, l_End == std::string::npos ? std::string::npos : l_End - l_Pos + 1);
			l_Pos = l_End == std::string::npos ? l_Text.size() : l_End + 1;
			if (!l_Line.empty() && m_Ui.font().measure(l_Line + l_Word, l_HintPx) > l_HintWidth)
			{
				l_Draw.text(Vec2{ l_Area.min.x, l_Y }, l_Line, l_HintPx, l_Theme.textMuted);
				l_Y += m_Ui.px(15.f);
				l_Line.clear();
			}
			l_Line += l_Word;
		}
		if (!l_Line.empty())
			l_Draw.text(Vec2{ l_Area.min.x, l_Y }, l_Line, l_HintPx, l_Theme.textMuted);
	}
}

// A small bubble on every object (or group) that has a note, so it is clear which ones to click
void App::buildNoteMarkers()
{
	const Document& l_Document = m_Editor.document();
	if (m_NoteMarkersRevision != l_Document.revision() || m_NoteMarkersSize != l_Document.size())
	{
		m_NoteMarkersRevision = l_Document.revision();
		m_NoteMarkersSize = l_Document.size();
		m_NoteMarkers.clear();
		std::unordered_map<ObjectId, size_t> l_ByGroup;
		for (const std::unique_ptr<Object>& l_Object : l_Document.objects())
		{
			if (l_Object->note.empty())
				continue;
			if (l_Object->group != INVALID_OBJECT_ID)
			{
				const auto [l_It, l_New] = l_ByGroup.try_emplace(l_Object->group, m_NoteMarkers.size());
				if (!l_New)
				{
					m_NoteMarkers[l_It->second].push_back(l_Object->id);
					continue;
				}
			}
			m_NoteMarkers.push_back({ l_Object->id });
		}
	}
	if (m_NoteMarkers.empty() || m_Editor.isBusy() || modalOpen() || m_Editor.textEditing() || m_Editor.cropping())
		return;

	const Camera& l_Camera = m_Editor.camera();
	const ui::Theme& l_Theme = m_Ui.theme();
	ui::DrawList& l_Draw = m_Ui.draw();
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Size = m_Ui.px(24.f);
	for (const std::vector<ObjectId>& l_Members : m_NoteMarkers)
	{
		if (m_NotesOpen && m_Editor.selection().contains(l_Members.front()))
			continue; // its note is open
		Rect l_Bounds{};
		for (const ObjectId l_Id : l_Members)
		{
			if (const Object* l_Object = l_Document.find(l_Id))
				l_Bounds.expand(l_Object->worldBounds());
		}
		const Vec2 l_A{ l_Camera.worldToScreen(l_Bounds.min) };
		const Vec2 l_B{ l_Camera.worldToScreen(l_Bounds.max) };
		const Vec2 l_Lo = glm::min(l_A, l_B);
		const Vec2 l_Hi = glm::max(l_A, l_B);
		if (l_Hi.x < 0.f || l_Hi.y < 0.f || l_Lo.x > l_Viewport.x || l_Lo.y > l_Viewport.y)
			continue;
		// At the top right corner, kept on screen
		Vec2 l_Center{ l_Hi.x + l_Size * 0.3f, l_Lo.y - l_Size * 0.3f };
		l_Center = glm::clamp(l_Center, Vec2{ l_Size, m_Ui.px(70.f) }, Vec2{ l_Viewport.x - l_Size, l_Viewport.y - l_Size });
		const ui::Rect2 l_Rect = ui::Rect2::fromCenter(l_Center, Vec2{ l_Size });
		m_Ui.beginPanel(l_Rect);
		const uint32_t l_MarkerId = ui::Context::id("note.marker", static_cast<int>(l_Members.front() & 0x7FFFFFFF));
		const ui::Interaction l_State = m_Ui.interact(l_MarkerId, l_Rect);
		l_Draw.shadow(l_Rect, l_Size * 0.5f, m_Ui.px(8.f), Vec2{ 0.f, m_Ui.px(2.f) }, l_Theme.shadow);
		l_Draw.circle(l_Center, l_Size * 0.5f * (l_State.hovered ? 1.08f : 1.f), l_Theme.accent);
		l_Draw.icon(ui::Icon::Note, l_Center, m_Ui.fontPx(13.f), Color{ 1.f, 1.f, 1.f, 1.f });
		m_Ui.noteTooltip(l_MarkerId, l_Rect, tr("notes.show"), l_State.hovered);
		if (l_State.clicked)
		{
			m_Editor.setTool(tools::ToolKind::Select);
			m_Editor.selection().set(l_Members);
			requestRedraw();
		}
	}
}
} // namespace wb
