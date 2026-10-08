// App: the menu, the shortcuts overlay, modal dialogs and toasts.
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <volk.h>

module wb.app;

import wb.doc.commands;
import wb.editor;
import wb.io.file;
import wb.math;
import wb.platform.input;
import wb.session;
import wb.tools.tool;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.renderer;
import wb.ui.theme;

namespace wb
{
namespace
{
using ui::Rect2;

constexpr float EDGE_MARGIN = 16.f;

std::string describeAge(const int64_t p_Seconds)
{
	const auto l_Count = [](const int64_t p_Value, const char* p_Unit) { return std::to_string(p_Value) + " " + p_Unit + (p_Value == 1 ? "" : "s") + " ago"; };
	if (p_Seconds < 60)
		return "less than a minute ago";
	if (p_Seconds < 3600)
		return l_Count(p_Seconds / 60, "minute");
	if (p_Seconds < 86400)
		return l_Count(p_Seconds / 3600, "hour");
	return l_Count(p_Seconds / 86400, "day");
}

struct DialogButton
{
	std::string_view label;
	ui::ButtonStyle style = ui::ButtonStyle::Secondary;
};

// A modal card over a dimmed board. Returns the index of the clicked button, or -1.
int dialogCard(ui::Context& p_Ui, const std::string_view p_Key, const std::string_view p_Title, const std::string_view p_Body, const std::span<const DialogButton> p_Buttons)
{
	ui::DrawList& l_Draw = p_Ui.draw();
	const ui::Theme& l_Theme = p_Ui.theme();
	const Vec2 l_Viewport = p_Ui.viewport();
	const float l_Fade = p_Ui.animFrom(ui::Context::id(p_Key, 1), 0.f, 1.f, 20.f);

	// The scrim blocks everything below the card
	const Rect2 l_Screen{ Vec2{ 0.f }, l_Viewport };
	p_Ui.beginPanel(l_Screen);
	l_Draw.setOpacity(l_Fade);
	l_Draw.rect(l_Screen, 0.f, l_Theme.scrim);

	const float l_Pad = p_Ui.px(26.f);
	const float l_Width = std::min(p_Ui.px(440.f), l_Viewport.x - p_Ui.px(EDGE_MARGIN) * 2.f);
	const float l_BodyWidth = l_Width - l_Pad * 2.f;
	const int l_BodyPx = p_Ui.fontPx(14.f);
	const std::vector<std::string> l_Lines = p_Ui.font().wrap(p_Body, l_BodyPx, l_BodyWidth);
	const float l_LineHeight = p_Ui.px(21.f);
	const float l_ButtonHeight = p_Ui.px(40.f);
	const float l_Height = l_Pad + p_Ui.px(26.f) + p_Ui.px(10.f) + l_LineHeight * static_cast<float>(l_Lines.size()) + p_Ui.px(24.f) + l_ButtonHeight + l_Pad;
	const float l_Lift = (1.f - l_Fade) * p_Ui.px(10.f);
	const Rect2 l_Card = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, (l_Viewport.y - l_Height) * 0.5f + l_Lift }, Vec2{ l_Width, l_Height });
	p_Ui.panel(l_Card, p_Ui.px(22.f));

	float l_Y = l_Card.min.y + l_Pad;
	p_Ui.label(Vec2{ l_Card.min.x + l_Pad, l_Y + p_Ui.px(13.f) }, p_Title, 18.f, l_Theme.text);
	l_Y += p_Ui.px(26.f) + p_Ui.px(10.f);
	for (const std::string& l_Line : l_Lines)
	{
		l_Draw.text(Vec2{ l_Card.min.x + l_Pad, l_Y + l_LineHeight * 0.5f }, l_Line, l_BodyPx, l_Theme.textMuted);
		l_Y += l_LineHeight;
	}
	l_Y += p_Ui.px(24.f);

	// Buttons, right aligned; the last one is the default
	int l_Clicked = -1;
	float l_X = l_Card.max.x - l_Pad;
	for (int i = static_cast<int>(p_Buttons.size()) - 1; i >= 0; --i)
	{
		const DialogButton& l_Button = p_Buttons[static_cast<size_t>(i)];
		const float l_ButtonWidth = std::max(p_Ui.px(96.f), p_Ui.font().measure(l_Button.label, p_Ui.fontPx(14.f)) + p_Ui.px(40.f));
		const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ l_X - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, l_ButtonHeight });
		if (p_Ui.button(std::string(p_Key) + ".button" + std::to_string(i), l_Rect, l_Button.label, l_Button.style))
			l_Clicked = i;
		l_X -= l_ButtonWidth + p_Ui.px(10.f);
	}
	l_Draw.setOpacity(1.f);
	return l_Clicked;
}
} // namespace

// ------------------------------------------------------------------------------------------------ menu

void App::buildMenu()
{
	if (m_Popup != Popup::Menu)
		return;

	const float l_Width = m_Ui.px(312.f);
	const float l_Pad = m_Ui.px(8.f);
	const float l_HeaderHeight = m_Ui.px(40.f);
	const float l_RowHeight = m_Ui.px(36.f);
	static constexpr std::array<int, 4> ROW_COUNTS{ 5, 12, 8, 1 };
	static constexpr std::array<const char*, 4> TITLES{ "Board", "Edit", "View", "Help" };
	const bool l_Idle = !m_Editor.isBusy();
	const ui::Theme& l_Theme = m_Ui.theme();

	const float l_Open = m_Ui.animFrom(ui::Context::id("popover.open"), 0.f, 1.f, 24.f);
	const float l_Height = l_Pad * 2.f + 4.f * l_HeaderHeight + (m_MenuSection >= 0 ? l_RowHeight * static_cast<float>(ROW_COUNTS[static_cast<size_t>(m_MenuSection)]) : 0.f);
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Margin = m_Ui.px(EDGE_MARGIN);
	const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ l_Viewport.x - l_Margin - l_Width, m_MenuAnchor.max.y + m_Ui.px(10.f) - (1.f - l_Open) * m_Ui.px(8.f) }, Vec2{ l_Width, std::min(l_Height, l_Viewport.y - m_MenuAnchor.max.y - m_Ui.px(24.f)) });
	m_Ui.draw().setOpacity(l_Open);
	m_Ui.panel(l_Rect, m_Ui.px(18.f));

	bool l_Close = false;
	float l_Y = l_Rect.min.y + l_Pad;
	const float l_Left = l_Rect.min.x + l_Pad;
	const float l_Inner = l_Width - l_Pad * 2.f;

	const auto l_Row = [&](const ui::Icon p_Icon, const char* p_Label, const char* p_Shortcut, const bool p_Enabled = true, const bool p_Checked = false) -> bool
	{
		const Rect2 l_RowRect = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_RowHeight });
		l_Y += l_RowHeight;
		return m_Ui.menuRow(std::string("menu.") + p_Label, l_RowRect, p_Icon, p_Label, p_Shortcut, p_Enabled, p_Checked);
	};

	for (int l_Section = 0; l_Section < 4; ++l_Section)
	{
		// Section header (accordion)
		const Rect2 l_HeaderRect = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_HeaderHeight });
		const uint32_t l_HeaderId = ui::Context::id("menu.section", l_Section);
		const ui::Interaction l_State = m_Ui.interact(l_HeaderId, l_HeaderRect);
		const float l_Hover = m_Ui.anim(l_HeaderId + 1, l_State.hovered ? 1.f : 0.f, 24.f);
		if (l_Hover > 0.001f)
			m_Ui.draw().rect(l_HeaderRect, m_Ui.px(8.f), ui::withAlpha(l_Theme.hover, l_Theme.hover.a * l_Hover));
		const bool l_IsOpen = m_MenuSection == l_Section;
		m_Ui.label(Vec2{ l_HeaderRect.min.x + m_Ui.px(14.f), l_HeaderRect.center().y }, TITLES[static_cast<size_t>(l_Section)], 13.f, l_IsOpen ? l_Theme.text : l_Theme.textMuted);
		m_Ui.draw().icon(l_IsOpen ? ui::Icon::ChevronDown : ui::Icon::ChevronRight, Vec2{ l_HeaderRect.max.x - m_Ui.px(20.f), l_HeaderRect.center().y }, m_Ui.fontPx(16.f), l_Theme.textMuted);
		if (l_State.clicked)
			m_MenuSection = l_IsOpen ? -1 : l_Section;
		l_Y += l_HeaderHeight;

		if (!l_IsOpen)
			continue;

		const bool l_HasSelection = m_Editor.hasSelection() && l_Idle;
		switch (l_Section)
		{
		case 0:
			if (l_Row(ui::Icon::NewFile, "New board", "Ctrl+N", l_Idle))
			{
				requestAction(Action::NewBoard);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Open, "Open...", "Ctrl+O", l_Idle))
			{
				requestAction(Action::OpenFile);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Save, "Save", "Ctrl+S", l_Idle))
			{
				requestSave(false, Action::None);
				l_Close = true;
			}
			if (l_Row(ui::Icon::SaveAs, "Save as...", "Ctrl+Shift+S", l_Idle))
			{
				requestSave(true, Action::None);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Close, "Exit", "Alt+F4"))
			{
				requestAction(Action::Quit);
				l_Close = true;
			}
			break;
		case 1:
			if (l_Row(ui::Icon::Undo, "Undo", "Ctrl+Z", m_Editor.history().canUndo() && l_Idle))
				m_Editor.undo();
			if (l_Row(ui::Icon::Redo, "Redo", "Ctrl+Y", m_Editor.history().canRedo() && l_Idle))
				m_Editor.redo();
			if (l_Row(ui::Icon::Cut, "Cut", "Ctrl+X", l_HasSelection))
			{
				m_Editor.cutSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Copy, "Copy", "Ctrl+C", l_HasSelection))
			{
				m_Editor.copySelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Paste, "Paste", "Ctrl+V", m_Editor.canPaste() && l_Idle))
			{
				m_Editor.paste();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Duplicate, "Duplicate", "Ctrl+D", l_HasSelection))
			{
				m_Editor.duplicateSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Trash, "Delete", "Del", l_HasSelection))
			{
				m_Editor.deleteSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::SelectAll, "Select all", "Ctrl+A", l_Idle && !m_Editor.document().empty()))
			{
				m_Editor.selectAll();
				l_Close = true;
			}
			if (l_Row(ui::Icon::BringToFront, "Bring to front", "Ctrl+Shift+]", l_HasSelection))
			{
				m_Editor.reorderSelection(ZOrderMove::ToFront);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Forward, "Bring forward", "Ctrl+]", l_HasSelection))
				m_Editor.reorderSelection(ZOrderMove::Forward);
			if (l_Row(ui::Icon::Backward, "Send backward", "Ctrl+[", l_HasSelection))
				m_Editor.reorderSelection(ZOrderMove::Backward);
			if (l_Row(ui::Icon::SendToBack, "Send to back", "Ctrl+Shift+[", l_HasSelection))
			{
				m_Editor.reorderSelection(ZOrderMove::ToBack);
				l_Close = true;
			}
			break;
		case 2:
			if (l_Row(ui::Icon::Plus, "Zoom in", "Ctrl+="))
				m_Editor.zoomAroundCenter(1.25);
			if (l_Row(ui::Icon::Minus, "Zoom out", "Ctrl+-"))
				m_Editor.zoomAroundCenter(1.0 / 1.25);
			if (l_Row(ui::Icon::None, "Actual size", "Ctrl+0"))
				m_Editor.resetZoom();
			if (l_Row(ui::Icon::Fit, "Fit everything", "Home"))
			{
				m_Editor.fitContent();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Grid, "Grid", "", true, m_ShowGrid))
				m_ShowGrid = !m_ShowGrid;
			if (l_Row(ui::Icon::Moon, "Dark theme", "", true, m_DarkTheme))
				m_DarkTheme = !m_DarkTheme;
			if (l_Row(ui::Icon::Fullscreen, "Fullscreen", "F11"))
			{
				const bool l_Fullscreen = (SDL_GetWindowFlags(m_Window.handle()) & SDL_WINDOW_FULLSCREEN) != 0;
				SDL_SetWindowFullscreen(m_Window.handle(), !l_Fullscreen);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Bug, "Debug panel", "F3", true, m_ShowDebug))
				m_ShowDebug = !m_ShowDebug;
			break;
		case 3:
			if (l_Row(ui::Icon::Keyboard, "Keyboard shortcuts", "F1"))
			{
				m_ShowShortcuts = true;
				l_Close = true;
			}
			break;
		default:
			break;
		}
	}
	m_Ui.draw().setOpacity(1.f);
	if (l_Close)
		closePopup("menu item used");
}

// ------------------------------------------------------------------------------------------------ shortcuts

namespace
{
struct ShortcutRow
{
	const char* keys;
	const char* what;
};

struct ShortcutSection
{
	const char* title;
	std::vector<ShortcutRow> rows;
};

const std::vector<ShortcutSection>& leftSections()
{
	static const std::vector<ShortcutSection> s_Sections{
		{ "Tools", {
			{ "P  E  V  H", "Pen, eraser, select, hand" },
			{ "Alt + P/E/V/H", "Borrow a tool while held" },
			{ "Pen eraser end", "Erases, whatever the tool" },
		} },
		{ "Navigation", {
			{ "Mouse wheel", "Zoom at the pointer" },
			{ "Ctrl + wheel", "Scroll (Shift: sideways)" },
			{ "Space + drag", "Move the board" },
			{ "Right / middle drag", "Move the board" },
			{ "Ctrl + =  /  Ctrl + -", "Zoom in / out" },
			{ "Ctrl + 0", "Actual size" },
			{ "Home", "Fit everything" },
		} },
		{ "File", {
			{ "Ctrl + N / O", "New / open a board" },
			{ "Ctrl + S", "Save" },
			{ "Ctrl + Shift + S", "Save as" },
		} },
		{ "Window", {
			{ "F1", "This list" },
			{ "F3", "Debug panel" },
			{ "F11", "Fullscreen" },
		} },
	};
	return s_Sections;
}

const std::vector<ShortcutSection>& rightSections()
{
	static const std::vector<ShortcutSection> s_Sections{
		{ "Editing", {
			{ "Ctrl + Z", "Undo" },
			{ "Ctrl + Y / Ctrl + Shift + Z", "Redo" },
			{ "Ctrl + A", "Select all" },
			{ "Ctrl + C / X / V", "Copy / cut / paste" },
			{ "Ctrl + D", "Duplicate" },
			{ "Delete", "Delete the selection" },
			{ "Arrows (+ Shift)", "Nudge by 1 (10) points" },
			{ "Ctrl + ] / [", "Bring forward / send back" },
			{ "Ctrl + Shift + ] / [", "To front / to back" },
			{ "Esc", "Cancel a drag, or deselect" },
		} },
		{ "Selection", {
			{ "Click, box or lasso", "Select (Shift adds)" },
			{ "Drag inside the box", "Move" },
			{ "Drag a handle", "Scale (Shift: keep shape)" },
			{ "Alt + drag a handle", "Scale from the centre" },
			{ "Drag the top circle", "Rotate (Shift: 15 degrees)" },
		} },
	};
	return s_Sections;
}

float sectionsHeight(ui::Context& p_Ui, const std::vector<ShortcutSection>& p_Sections)
{
	float l_Height = 0.f;
	for (const ShortcutSection& l_Section : p_Sections)
		l_Height += p_Ui.px(34.f) + p_Ui.px(28.f) * static_cast<float>(l_Section.rows.size()) + p_Ui.px(8.f);
	return l_Height;
}
} // namespace

void App::buildShortcuts()
{
	const float l_Open = m_Ui.anim(ui::Context::id("shortcuts.open"), m_ShowShortcuts ? 1.f : 0.f, 20.f);
	if (!m_ShowShortcuts && l_Open < 0.01f)
		return;

	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const Vec2 l_Viewport = m_Ui.viewport();

	const Rect2 l_Screen{ Vec2{ 0.f }, l_Viewport };
	m_Ui.beginPanel(l_Screen);
	l_Draw.setOpacity(l_Open);
	l_Draw.rect(l_Screen, 0.f, l_Theme.scrim);
	if (m_ShowShortcuts && m_Ui.interact(ui::Context::id("shortcuts.scrim"), l_Screen).clicked)
		m_ShowShortcuts = false;

	const float l_Pad = m_Ui.px(28.f);
	const float l_Gap = m_Ui.px(36.f);
	const float l_Width = std::min(m_Ui.px(900.f), l_Viewport.x - m_Ui.px(EDGE_MARGIN) * 2.f);
	const float l_Column = (l_Width - l_Pad * 2.f - l_Gap) * 0.5f;
	const float l_Content = std::max(sectionsHeight(m_Ui, leftSections()), sectionsHeight(m_Ui, rightSections()));
	const float l_Height = std::min(l_Pad * 2.f + m_Ui.px(40.f) + l_Content, l_Viewport.y - m_Ui.px(EDGE_MARGIN) * 2.f);
	const float l_Lift = (1.f - l_Open) * m_Ui.px(10.f);
	const Rect2 l_Card = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, (l_Viewport.y - l_Height) * 0.5f + l_Lift }, Vec2{ l_Width, l_Height });
	m_Ui.panel(l_Card, m_Ui.px(24.f));

	m_Ui.label(Vec2{ l_Card.min.x + l_Pad, l_Card.min.y + l_Pad + m_Ui.px(14.f) }, "Keyboard shortcuts", 19.f, l_Theme.text);
	if (m_Ui.iconButton("shortcuts.close", Rect2::fromPosSize(Vec2{ l_Card.max.x - l_Pad - m_Ui.px(36.f) + m_Ui.px(8.f), l_Card.min.y + l_Pad - m_Ui.px(4.f) }, Vec2{ m_Ui.px(36.f) }), ui::Icon::Close, false, true, "Close (Esc)"))
		m_ShowShortcuts = false;

	l_Draw.pushClip(l_Card);
	const auto l_DrawColumn = [&](const std::vector<ShortcutSection>& p_Sections, const float p_X)
	{
		float l_Y = l_Card.min.y + l_Pad + m_Ui.px(48.f);
		const int l_KeyPx = m_Ui.fontPx(12.5f);
		const int l_TextPx = m_Ui.fontPx(13.5f);
		const float l_KeyColumn = std::min(m_Ui.px(196.f), l_Column * 0.5f);
		for (const ShortcutSection& l_Section : p_Sections)
		{
			m_Ui.label(Vec2{ p_X, l_Y + m_Ui.px(12.f) }, l_Section.title, 12.f, l_Theme.accent);
			l_Y += m_Ui.px(34.f);
			for (const ShortcutRow& l_Row : l_Section.rows)
			{
				const float l_RowCenter = l_Y + m_Ui.px(14.f);
				const std::string l_Keys = m_Ui.font().fit(l_Row.keys, l_KeyPx, l_KeyColumn - m_Ui.px(24.f));
				const float l_KeysWidth = m_Ui.font().measure(l_Keys, l_KeyPx) + m_Ui.px(16.f);
				const Rect2 l_Cap = Rect2::fromPosSize(Vec2{ p_X, l_RowCenter - m_Ui.px(11.f) }, Vec2{ l_KeysWidth, m_Ui.px(22.f) });
				l_Draw.rectBorder(l_Cap, m_Ui.px(6.f), l_Theme.hover, l_Theme.divider, 1.f);
				l_Draw.text(Vec2{ l_Cap.min.x + m_Ui.px(8.f), l_RowCenter }, l_Keys, l_KeyPx, l_Theme.text);
				l_Draw.text(Vec2{ p_X + l_KeyColumn, l_RowCenter }, m_Ui.font().fit(l_Row.what, l_TextPx, l_Column - l_KeyColumn), l_TextPx, l_Theme.textMuted);
				l_Y += m_Ui.px(28.f);
			}
			l_Y += m_Ui.px(8.f);
		}
	};
	l_DrawColumn(leftSections(), l_Card.min.x + l_Pad);
	l_DrawColumn(rightSections(), l_Card.min.x + l_Pad + l_Column + l_Gap);
	l_Draw.popClip();
	l_Draw.setOpacity(1.f);
}

// ------------------------------------------------------------------------------------------------ dialogs

void App::buildDialogs()
{
	if (!modalOpen())
	{
		m_Ui.setAnim(ui::Context::id("dialog.prompt", 1), 0.f);
		m_Ui.setAnim(ui::Context::id("dialog.recovery", 1), 0.f);
		m_Ui.setAnim(ui::Context::id("dialog.message", 1), 0.f);
		return;
	}

	if (m_MessageOpen)
	{
		static constexpr std::array<DialogButton, 1> BUTTONS{ DialogButton{ "OK", ui::ButtonStyle::Primary } };
		if (dialogCard(m_Ui, "dialog.message", "Whiteboard", m_Message, BUTTONS) == 0)
			m_MessageOpen = false;
		return;
	}

	if (m_PromptOpen)
	{
		static constexpr std::array<DialogButton, 3> BUTTONS{ DialogButton{ "Cancel", ui::ButtonStyle::Secondary }, DialogButton{ "Don't save", ui::ButtonStyle::Secondary }, DialogButton{ "Save", ui::ButtonStyle::Primary } };
		const std::string l_Body = "\"" + m_Session.displayName() + "\" has changes that are not saved.";
		const Action l_Action = m_PromptAction;
		switch (dialogCard(m_Ui, "dialog.prompt", "Unsaved changes", l_Body, BUTTONS))
		{
		case 0:
			m_PromptOpen = false;
			m_PromptAction = Action::None;
			break;
		case 1:
			m_PromptOpen = false;
			m_PromptAction = Action::None;
			m_QuitDiscard = l_Action == Action::Quit;
			performAction(l_Action);
			break;
		case 2:
			m_PromptOpen = false;
			m_PromptAction = Action::None;
			requestSave(false, l_Action);
			break;
		default:
			break;
		}
		return;
	}

	if (m_RecoveryOpen && m_Recovery)
	{
		static constexpr std::array<DialogButton, 2> BUTTONS{ DialogButton{ "Discard", ui::ButtonStyle::Secondary }, DialogButton{ "Recover", ui::ButtonStyle::Primary } };
		const std::string l_Name = m_Recovery->sourcePath.empty() ? std::string("an untitled board") : "\"" + pathToUtf8(pathFromUtf8(m_Recovery->sourcePath).filename()) + "\"";
		const std::string l_Body = "Whiteboard closed before " + l_Name + " was saved.\n" + std::to_string(m_Recovery->objectCount) + " objects, last autosaved " + describeAge(m_Recovery->ageSeconds) + ".";
		switch (dialogCard(m_Ui, "dialog.recovery", "Recover unsaved work", l_Body, BUTTONS))
		{
		case 0:
			m_RecoveryOpen = false;
			m_Recovery.reset();
			m_Session.discardRecovery();
			break;
		case 1:
			m_RecoveryOpen = false;
			m_Recovery.reset();
			if (const IoResult l_Result = m_Session.recover(); l_Result.ok)
			{
				updateTitle();
				showToast("Recovered unsaved work");
			}
			else
			{
				showMessage("The unsaved work could not be recovered.\n\n" + l_Result.error);
			}
			break;
		default:
			break;
		}
		return;
	}
	m_RecoveryOpen = false; // nothing left to recover
}

// ------------------------------------------------------------------------------------------------ toast

void App::buildToast()
{
	const bool l_Active = m_ToastUntilNs != 0 && !m_Toast.empty();
	const float l_Show = m_Ui.anim(ui::Context::id("toast"), l_Active ? 1.f : 0.f, 14.f);
	if (l_Show < 0.01f || m_Toast.empty())
		return;

	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const int l_Px = m_Ui.fontPx(14.f);
	const float l_Width = m_Ui.font().measure(m_Toast, l_Px) + m_Ui.px(44.f);
	const float l_Height = m_Ui.px(42.f);
	const Vec2 l_Viewport = m_Ui.viewport();
	const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, m_Ui.px(70.f) - (1.f - l_Show) * m_Ui.px(12.f) }, Vec2{ l_Width, l_Height });
	l_Draw.setOpacity(l_Show);
	l_Draw.shadow(l_Rect, l_Height * 0.5f, m_Ui.px(18.f), Vec2{ 0.f, m_Ui.px(6.f) }, l_Theme.shadow);
	l_Draw.rect(l_Rect, l_Height * 0.5f, l_Theme.tooltip);
	l_Draw.text(Vec2{ l_Rect.center().x, l_Rect.center().y }, m_Toast, l_Px, l_Theme.tooltipText, ui::TextAlign::Center);
	l_Draw.setOpacity(1.f);
}
} // namespace wb
