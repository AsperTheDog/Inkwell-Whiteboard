// App: the menu, the shortcuts overlay, modal dialogs and toasts.
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
import wb.io.locale;
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
	if (p_Seconds < 60)
		return tr("age.lessThanMinute");
	if (p_Seconds < 3600)
		return tr(p_Seconds / 60 == 1 ? "age.minute" : "age.minutes", { std::to_string(p_Seconds / 60) });
	if (p_Seconds < 86400)
		return tr(p_Seconds / 3600 == 1 ? "age.hour" : "age.hours", { std::to_string(p_Seconds / 3600) });
	return tr(p_Seconds / 86400 == 1 ? "age.day" : "age.days", { std::to_string(p_Seconds / 86400) });
}

struct DialogButton
{
	std::string_view label;
	ui::ButtonStyle style = ui::ButtonStyle::Secondary;
};

// A modal card over a dimmed board. Title and button labels are language ids; the body is already translated text.
// Returns the index of the clicked button, or -1.
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
	p_Ui.label(Vec2{ l_Card.min.x + l_Pad, l_Y + p_Ui.px(13.f) }, tr(p_Title), 18.f, l_Theme.text);
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
		const float l_ButtonWidth = std::max(p_Ui.px(96.f), p_Ui.font().measure(tr(l_Button.label), p_Ui.fontPx(14.f)) + p_Ui.px(40.f));
		const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ l_X - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, l_ButtonHeight });
		if (p_Ui.button(std::string(p_Key) + ".button" + std::to_string(i), l_Rect, tr(l_Button.label), l_Button.style))
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
	static constexpr std::array<int, 5> ROW_COUNTS{ 6, 16, 12, 2, 1 };
	static constexpr std::array<const char*, 5> TITLES{ "menu.section.board", "menu.section.edit", "menu.section.view", "menu.section.collaborate", "menu.section.help" };
	const bool l_Idle = !m_Editor.isBusy();
	const ui::Theme& l_Theme = m_Ui.theme();

	const float l_Open = m_Ui.animFrom(ui::Context::id("popover.open"), 0.f, 1.f, 24.f);
	const float l_Height = l_Pad * 2.f + 5.f * l_HeaderHeight + (m_MenuSection >= 0 ? l_RowHeight * static_cast<float>(ROW_COUNTS[static_cast<size_t>(m_MenuSection)]) : 0.f);
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Margin = m_Ui.px(EDGE_MARGIN);
	const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ l_Viewport.x - l_Margin - l_Width, m_MenuAnchor.max.y + m_Ui.px(10.f) - (1.f - l_Open) * m_Ui.px(8.f) }, Vec2{ l_Width, std::min(l_Height, l_Viewport.y - m_MenuAnchor.max.y - m_Ui.px(24.f)) });
	m_Ui.draw().setOpacity(l_Open);
	m_Ui.panel(l_Rect, m_Ui.px(18.f));

	bool l_Close = false;
	float l_Y = l_Rect.min.y + l_Pad;
	const float l_Left = l_Rect.min.x + l_Pad;
	const float l_Inner = l_Width - l_Pad * 2.f;
	m_Ui.draw().pushClip(l_Rect); // in a short window nothing is drawn past the panel

	const auto l_Row = [&](const ui::Icon p_Icon, const char* p_Id, const char* p_Shortcut, const bool p_Enabled = true, const bool p_Checked = false) -> bool
	{
		const Rect2 l_RowRect = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_RowHeight });
		l_Y += l_RowHeight;
		return m_Ui.menuRow(p_Id, l_RowRect, p_Icon, tr(p_Id), p_Shortcut, p_Enabled, p_Checked);
	};

	for (int l_Section = 0; l_Section < 5; ++l_Section)
	{
		// Section header (accordion)
		const Rect2 l_HeaderRect = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_HeaderHeight });
		const uint32_t l_HeaderId = ui::Context::id("menu.section", l_Section);
		const ui::Interaction l_State = m_Ui.interact(l_HeaderId, l_HeaderRect);
		const float l_Hover = m_Ui.anim(l_HeaderId + 1, l_State.hovered ? 1.f : 0.f, 24.f);
		if (l_Hover > 0.001f)
			m_Ui.draw().rect(l_HeaderRect, m_Ui.px(8.f), ui::withAlpha(l_Theme.hover, l_Theme.hover.a * l_Hover));
		const bool l_IsOpen = m_MenuSection == l_Section;
		m_Ui.label(Vec2{ l_HeaderRect.min.x + m_Ui.px(14.f), l_HeaderRect.center().y }, tr(TITLES[static_cast<size_t>(l_Section)]), 13.f, l_IsOpen ? l_Theme.text : l_Theme.textMuted);
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
			if (l_Row(ui::Icon::NewFile, "menu.new", "Ctrl+N", l_Idle))
			{
				requestAction(Action::NewBoard);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Open, "menu.open", "Ctrl+O", l_Idle))
			{
				requestAction(Action::OpenFile);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Save, "menu.save", "Ctrl+S", l_Idle))
			{
				requestSave(false, Action::None);
				l_Close = true;
			}
			if (l_Row(ui::Icon::SaveAs, "menu.saveAs", "Ctrl+Shift+S", l_Idle))
			{
				requestSave(true, Action::None);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Export, "menu.exportPng", "Ctrl+Shift+E", l_Idle))
			{
				openExportDialog();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Close, "menu.exit", "Alt+F4"))
			{
				requestAction(Action::Quit);
				l_Close = true;
			}
			break;
		case 1:
			if (l_Row(ui::Icon::Undo, "menu.undo", "Ctrl+Z", m_Editor.history().canUndo() && l_Idle))
				m_Editor.undo();
			if (l_Row(ui::Icon::Redo, "menu.redo", "Ctrl+Y", m_Editor.history().canRedo() && l_Idle))
				m_Editor.redo();
			if (l_Row(ui::Icon::Image, "menu.insert", "Ctrl+I", l_Idle))
			{
				showInsertPictureDialog();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Cut, "menu.cut", "Ctrl+X", l_HasSelection))
			{
				m_Editor.cutSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Copy, "menu.copy", "Ctrl+C", l_HasSelection))
			{
				m_Editor.copySelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Paste, "menu.paste", "Ctrl+V", m_Editor.canPaste() && l_Idle))
			{
				m_Editor.paste();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Duplicate, "menu.duplicate", "Ctrl+D", l_HasSelection))
			{
				m_Editor.duplicateSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Trash, "menu.delete", "Del", l_HasSelection))
			{
				m_Editor.deleteSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::SelectAll, "menu.selectAll", "Ctrl+A", l_Idle && !m_Editor.document().empty()))
			{
				m_Editor.selectAll();
				l_Close = true;
			}
			// In a session only the host may wipe the board
			if (l_Row(ui::Icon::Trash, "menu.clearBoard", "", l_Idle && !m_Editor.document().empty() && !m_Editor.readOnly() && !(m_Collab && m_Collab->role() == net::Role::Client)))
			{
				m_ClearOpen = true;
				l_Close = true;
			}
			if (l_Row(ui::Icon::Layers, "menu.group", "Ctrl+G", l_Idle && m_Editor.canGroupSelection()))
			{
				m_Editor.groupSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Layers, "menu.ungroup", "Ctrl+Shift+G", l_Idle && m_Editor.selectionHasGroup()))
			{
				m_Editor.ungroupSelection();
				l_Close = true;
			}
			if (l_Row(ui::Icon::BringToFront, "menu.toFront", "Ctrl+Shift+]", l_HasSelection))
			{
				m_Editor.reorderSelection(ZOrderMove::ToFront);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Forward, "menu.forward", "Ctrl+]", l_HasSelection))
				m_Editor.reorderSelection(ZOrderMove::Forward);
			if (l_Row(ui::Icon::Backward, "menu.backward", "Ctrl+[", l_HasSelection))
				m_Editor.reorderSelection(ZOrderMove::Backward);
			if (l_Row(ui::Icon::SendToBack, "menu.toBack", "Ctrl+Shift+[", l_HasSelection))
			{
				m_Editor.reorderSelection(ZOrderMove::ToBack);
				l_Close = true;
			}
			break;
		case 2:
			if (l_Row(ui::Icon::Plus, "menu.zoomIn", "Ctrl+="))
				m_Editor.zoomAroundCenter(1.25);
			if (l_Row(ui::Icon::Minus, "menu.zoomOut", "Ctrl+-"))
				m_Editor.zoomAroundCenter(1.0 / 1.25);
			if (l_Row(ui::Icon::None, "menu.actualSize", "Ctrl+0"))
				m_Editor.resetZoom();
			if (l_Row(ui::Icon::Fit, "menu.fit", "Home"))
			{
				m_Editor.fitContent();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Grid, "menu.grid", "", true, m_ShowGrid))
				m_ShowGrid = !m_ShowGrid;
			if (l_Row(ui::Icon::Ruler, "menu.ruler", "R", true, m_Editor.rulerVisible()))
				m_Editor.toggleRuler();
			if (l_Row(ui::Icon::Moon, "menu.darkTheme", "", true, m_DarkTheme))
				m_DarkTheme = !m_DarkTheme;
			if (l_Row(ui::Icon::Play, "menu.playAnimations", "", true, m_PlayAnimations))
				m_PlayAnimations = !m_PlayAnimations;
			if (l_Row(ui::Icon::Layers, "menu.hideInterface", "F2"))
			{
				toggleInterface();
				l_Close = true;
			}
			if (l_Row(ui::Icon::Fullscreen, "menu.fullscreen", "F11"))
			{
				const bool l_Fullscreen = (SDL_GetWindowFlags(m_Window.handle()) & SDL_WINDOW_FULLSCREEN) != 0;
				SDL_SetWindowFullscreen(m_Window.handle(), !l_Fullscreen);
				l_Close = true;
			}
			if (l_Row(ui::Icon::Bug, "menu.debugPanel", "F3", true, m_ShowDebug))
				m_ShowDebug = !m_ShowDebug;
			if (l_Row(ui::Icon::Sliders, "menu.settings", ""))
			{
				m_ShowSettings = true;
				l_Close = true;
			}
			break;
		case 3:
			if (m_Collab && m_Collab->active())
			{
				if (l_Row(ui::Icon::Layers, "menu.session", "", m_Collab->online()))
				{
					openCollabDialog(CollabDialog::Session);
					l_Close = true;
				}
				if (l_Row(ui::Icon::Close, m_Collab->role() == net::Role::Host ? "menu.stopHosting" : "menu.leave", ""))
				{
					m_Collab->leave();
					showToast(tr("toast.leftSession"));
					l_Close = true;
				}
			}
			else
			{
				if (l_Row(ui::Icon::Upload, "menu.host", "", l_Idle))
				{
					openCollabDialog(CollabDialog::Host);
					l_Close = true;
				}
				if (l_Row(ui::Icon::Open, "menu.join", "", l_Idle))
				{
					openCollabDialog(CollabDialog::Join);
					l_Close = true;
				}
			}
			break;
		case 4:
			if (l_Row(ui::Icon::Keyboard, "menu.shortcuts", "F1"))
			{
				m_ShowShortcuts = true;
				l_Close = true;
			}
			break;
		default:
			break;
		}
	}
	m_Ui.draw().popClip();
	m_Ui.draw().setOpacity(1.f);
	if (l_Close)
		closePopup("menu item used");
}

// ------------------------------------------------------------------------------------------------ shortcuts

namespace
{
// The sheet is data in the language file: "shortcuts.<section>.title" and, per row, "shortcuts.<section>.<n>.keys" and
// "shortcuts.<section>.<n>.what" with n from 1
struct ShortcutSection
{
	const char* id;
	int rows;
};

constexpr std::array<ShortcutSection, 4> LEFT_SECTIONS{ ShortcutSection{ "tools", 6 }, ShortcutSection{ "navigation", 7 }, ShortcutSection{ "file", 3 }, ShortcutSection{ "window", 4 } };
constexpr std::array<ShortcutSection, 3> RIGHT_SECTIONS{ ShortcutSection{ "editing", 15 }, ShortcutSection{ "text", 6 }, ShortcutSection{ "selection", 5 } };

std::string shortcutId(const ShortcutSection& p_Section, const int p_Row, const char* p_Part)
{
	return std::string("shortcuts.") + p_Section.id + "." + std::to_string(p_Row) + "." + p_Part;
}

float sectionsHeight(ui::Context& p_Ui, const std::span<const ShortcutSection> p_Sections)
{
	float l_Height = 0.f;
	for (const ShortcutSection& l_Section : p_Sections)
		l_Height += p_Ui.px(34.f) + p_Ui.px(28.f) * static_cast<float>(l_Section.rows) + p_Ui.px(8.f);
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
	const float l_Content = std::max(sectionsHeight(m_Ui, LEFT_SECTIONS), sectionsHeight(m_Ui, RIGHT_SECTIONS));
	const float l_Height = std::min(l_Pad * 2.f + m_Ui.px(40.f) + l_Content, l_Viewport.y - m_Ui.px(EDGE_MARGIN) * 2.f);
	const float l_Lift = (1.f - l_Open) * m_Ui.px(10.f);
	const Rect2 l_Card = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, (l_Viewport.y - l_Height) * 0.5f + l_Lift }, Vec2{ l_Width, l_Height });
	m_Ui.panel(l_Card, m_Ui.px(24.f));

	m_Ui.label(Vec2{ l_Card.min.x + l_Pad, l_Card.min.y + l_Pad + m_Ui.px(14.f) }, tr("shortcuts.title"), 19.f, l_Theme.text);
	if (m_Ui.iconButton("shortcuts.close", Rect2::fromPosSize(Vec2{ l_Card.max.x - l_Pad - m_Ui.px(36.f) + m_Ui.px(8.f), l_Card.min.y + l_Pad - m_Ui.px(4.f) }, Vec2{ m_Ui.px(36.f) }), ui::Icon::Close, false, true, tr("common.closeEsc")))
		m_ShowShortcuts = false;

	l_Draw.pushClip(l_Card);
	const auto l_DrawColumn = [&](const std::span<const ShortcutSection> p_Sections, const float p_X)
	{
		float l_Y = l_Card.min.y + l_Pad + m_Ui.px(48.f);
		const int l_KeyPx = m_Ui.fontPx(12.5f);
		const int l_TextPx = m_Ui.fontPx(13.5f);
		const float l_KeyColumn = std::min(m_Ui.px(196.f), l_Column * 0.5f);
		for (const ShortcutSection& l_Section : p_Sections)
		{
			m_Ui.label(Vec2{ p_X, l_Y + m_Ui.px(12.f) }, tr(std::string("shortcuts.") + l_Section.id + ".title"), 12.f, l_Theme.accent);
			l_Y += m_Ui.px(34.f);
			for (int l_RowIndex = 1; l_RowIndex <= l_Section.rows; ++l_RowIndex)
			{
				const float l_RowCenter = l_Y + m_Ui.px(14.f);
				const std::string l_Keys = m_Ui.font().fit(tr(shortcutId(l_Section, l_RowIndex, "keys")), l_KeyPx, l_KeyColumn - m_Ui.px(24.f));
				const float l_KeysWidth = m_Ui.font().measure(l_Keys, l_KeyPx) + m_Ui.px(16.f);
				const Rect2 l_Cap = Rect2::fromPosSize(Vec2{ p_X, l_RowCenter - m_Ui.px(11.f) }, Vec2{ l_KeysWidth, m_Ui.px(22.f) });
				l_Draw.rectBorder(l_Cap, m_Ui.px(6.f), l_Theme.hover, l_Theme.divider, 1.f);
				l_Draw.text(Vec2{ l_Cap.min.x + m_Ui.px(8.f), l_RowCenter }, l_Keys, l_KeyPx, l_Theme.text);
				l_Draw.text(Vec2{ p_X + l_KeyColumn, l_RowCenter }, m_Ui.font().fit(tr(shortcutId(l_Section, l_RowIndex, "what")), l_TextPx, l_Column - l_KeyColumn), l_TextPx, l_Theme.textMuted);
				l_Y += m_Ui.px(28.f);
			}
			l_Y += m_Ui.px(8.f);
		}
	};
	l_DrawColumn(LEFT_SECTIONS, l_Card.min.x + l_Pad);
	l_DrawColumn(RIGHT_SECTIONS, l_Card.min.x + l_Pad + l_Column + l_Gap);
	l_Draw.popClip();
	l_Draw.setOpacity(1.f);
}

// ------------------------------------------------------------------------------------------------ settings

void App::buildSettings()
{
	const float l_Open = m_Ui.anim(ui::Context::id("settings.open"), m_ShowSettings ? 1.f : 0.f, 20.f);
	if (!m_ShowSettings && l_Open < 0.01f)
		return;

	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const Vec2 l_Viewport = m_Ui.viewport();

	const Rect2 l_Screen{ Vec2{ 0.f }, l_Viewport };
	m_Ui.beginPanel(l_Screen);
	l_Draw.setOpacity(l_Open);
	l_Draw.rect(l_Screen, 0.f, l_Theme.scrim);
	if (m_ShowSettings && m_Ui.interact(ui::Context::id("settings.scrim"), l_Screen).clicked)
		m_ShowSettings = false;

	const float l_Pad = m_Ui.px(26.f);
	const float l_Width = std::min(m_Ui.px(460.f), l_Viewport.x - m_Ui.px(EDGE_MARGIN) * 2.f);
	const float l_Inner = l_Width - l_Pad * 2.f;
	const float l_LabelHeight = m_Ui.px(22.f);
	const float l_SliderHeight = m_Ui.px(34.f);
	const int l_HintPx = m_Ui.fontPx(12.5f);
	tools::BrushState& l_Brush = m_Editor.brush();
	tools::SelectState& l_Select = m_Editor.selectState();
	const std::vector<std::string> l_SizeHint = m_Ui.font().wrap(tr("settings.sizeScale.hint"), l_HintPx, l_Inner);
	const bool l_ScreenSized = l_Brush.sizeMode == tools::BrushSizeMode::Screen;
	const std::vector<std::string> l_ModeHint = m_Ui.font().wrap(tr(l_ScreenSized ? "settings.mode.screen.hint" : "settings.mode.board.hint"), l_HintPx, l_Inner);
	const bool l_Local = l_Select.space == tools::TransformSpace::Local;
	const std::vector<std::string> l_SpaceHint = m_Ui.font().wrap(tr(l_Local ? "settings.space.local.hint" : "settings.space.global.hint"), l_HintPx, l_Inner);
	const float l_Line = m_Ui.px(17.f);
	const float l_SectionHeight = m_Ui.px(34.f);
	const float l_ButtonsHeight = m_Ui.px(40.f);
	const float l_ListRow = m_Ui.px(34.f);
	const float l_LanguageBlock = l_SectionHeight + l_ButtonsHeight + m_Ui.px(16.f);
	const float l_Content = l_LanguageBlock + l_SectionHeight + l_LabelHeight + l_SliderHeight + m_Ui.px(8.f) + l_Line * static_cast<float>(l_SizeHint.size()) + m_Ui.px(16.f)
		+ l_ButtonsHeight + m_Ui.px(8.f) + l_Line * static_cast<float>(l_ModeHint.size()) + m_Ui.px(16.f)
		+ l_SectionHeight + l_ButtonsHeight + m_Ui.px(8.f) + l_Line * static_cast<float>(l_SpaceHint.size());
	const float l_Height = std::min(l_Pad * 2.f + m_Ui.px(48.f) + l_Content, l_Viewport.y - m_Ui.px(EDGE_MARGIN) * 2.f);
	const float l_Lift = (1.f - l_Open) * m_Ui.px(10.f);
	const Rect2 l_Card = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, (l_Viewport.y - l_Height) * 0.5f + l_Lift }, Vec2{ l_Width, l_Height });
	m_Ui.panel(l_Card, m_Ui.px(22.f));

	m_Ui.label(Vec2{ l_Card.min.x + l_Pad, l_Card.min.y + l_Pad + m_Ui.px(14.f) }, tr("settings.title"), 19.f, l_Theme.text);
	if (m_Ui.iconButton("settings.close", Rect2::fromPosSize(Vec2{ l_Card.max.x - l_Pad - m_Ui.px(36.f) + m_Ui.px(8.f), l_Card.min.y + l_Pad - m_Ui.px(4.f) }, Vec2{ m_Ui.px(36.f) }), ui::Icon::Close, false, true, tr("common.closeEsc")))
		m_ShowSettings = false;

	const float l_Left = l_Card.min.x + l_Pad;
	float l_Y = l_Card.min.y + l_Pad + m_Ui.px(48.f);
	const auto l_Hints = [&](const std::vector<std::string>& p_Lines)
	{
		for (const std::string& l_Text : p_Lines)
		{
			l_Draw.text(Vec2{ l_Left, l_Y + m_Ui.px(8.f) }, l_Text, l_HintPx, l_Theme.textMuted);
			l_Y += l_Line;
		}
	};
	static constexpr std::array<ui::Icon, 2> NO_ICONS{ ui::Icon::None, ui::Icon::None };

	// Language: the current one on a button, the list of the others under it
	m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(12.f) }, tr("settings.language"), 12.f, l_Theme.accent);
	l_Y += l_SectionHeight;
	const std::string l_Current = currentLanguage();
	const auto l_CurrentInfo = std::ranges::find(m_Languages, l_Current, &LanguageInfo::code);
	const Rect2 l_LanguageButton = Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_ButtonsHeight });
	if (m_Ui.button("settings.language", l_LanguageButton, l_CurrentInfo != m_Languages.end() ? l_CurrentInfo->name : l_Current, ui::ButtonStyle::Secondary, true, m_LanguageListOpen ? ui::Icon::ChevronUp : ui::Icon::ChevronDown))
	{
		m_LanguageListOpen = !m_LanguageListOpen;
		m_LanguageScrollToSelected = m_LanguageListOpen;
	}
	l_Y += l_ButtonsHeight + m_Ui.px(16.f);
	m_LanguageListRect = {};

	m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(12.f) }, tr("settings.pen"), 12.f, l_Theme.accent);
	l_Y += l_SectionHeight;
	char l_Value[32];
	std::snprintf(l_Value, sizeof(l_Value), "%.2fx", static_cast<double>(l_Brush.sizeScale));
	m_Ui.label(Vec2{ l_Left, l_Y + l_LabelHeight * 0.5f }, tr("settings.sizeScale"), 13.f, l_Theme.textMuted);
	m_Ui.label(Vec2{ l_Left + l_Inner, l_Y + l_LabelHeight * 0.5f }, l_Value, 13.f, l_Theme.text, ui::TextAlign::Right);
	l_Y += l_LabelHeight;
	const float l_Inset = m_Ui.px(10.f);
	m_Ui.slider("settings.sizeScale", Rect2{ Vec2{ l_Left - l_Inset, l_Y }, Vec2{ l_Left + l_Inner + l_Inset, l_Y + l_SliderHeight } }, l_Brush.sizeScale, 0.25f, 4.f, true);
	l_Y += l_SliderHeight + m_Ui.px(8.f);
	l_Hints(l_SizeHint);
	l_Y += m_Ui.px(16.f);

	int l_Mode = l_ScreenSized ? 0 : 1;
	const std::array<std::string_view, 2> MODE_LABELS{ tr("settings.mode.screen"), tr("settings.mode.board") };
	if (m_Ui.segmented("settings.sizeMode", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_ButtonsHeight }), MODE_LABELS, NO_ICONS, l_Mode))
		l_Brush.sizeMode = l_Mode == 0 ? tools::BrushSizeMode::Screen : tools::BrushSizeMode::Board;
	l_Y += l_ButtonsHeight + m_Ui.px(8.f);
	l_Hints(l_ModeHint);
	l_Y += m_Ui.px(16.f);

	m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(12.f) }, tr("settings.selection"), 12.f, l_Theme.accent);
	l_Y += l_SectionHeight;
	int l_Space = l_Local ? 1 : 0;
	const std::array<std::string_view, 2> SPACE_LABELS{ tr("settings.space.global"), tr("settings.space.local") };
	if (m_Ui.segmented("settings.space", Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_Inner, l_ButtonsHeight }), SPACE_LABELS, NO_ICONS, l_Space))
		l_Select.space = l_Space == 0 ? tools::TransformSpace::Global : tools::TransformSpace::Local;
	l_Y += l_ButtonsHeight + m_Ui.px(8.f);
	l_Hints(l_SpaceHint);

	// The list of languages floats over the window, under the button (or above it when there is no room below), with
	// a fixed height: it scrolls when it holds more rows than fit
	if (m_LanguageListOpen)
	{
		const size_t l_Rows = m_Languages.size() + 1; // the first one follows the system's language
		const float l_ListPad = m_Ui.px(6.f);
		const float l_Margin = m_Ui.px(EDGE_MARGIN);
		const float l_Wanted = std::min(l_ListRow * static_cast<float>(l_Rows), l_ListRow * 6.5f) + l_ListPad * 2.f;
		const float l_Below = l_Viewport.y - l_Margin - l_LanguageButton.max.y - m_Ui.px(6.f);
		const float l_Above = l_LanguageButton.min.y - m_Ui.px(6.f) - l_Margin;
		const bool l_GoUp = l_Below < l_Wanted && l_Above > l_Below;
		const float l_ListHeight = std::min(l_Wanted, l_GoUp ? l_Above : l_Below);
		const float l_Top = l_GoUp ? l_LanguageButton.min.y - m_Ui.px(6.f) - l_ListHeight : l_LanguageButton.max.y + m_Ui.px(6.f);
		const Rect2 l_Box = Rect2::fromPosSize(Vec2{ l_LanguageButton.min.x, l_Top }, Vec2{ l_LanguageButton.width(), l_ListHeight });
		const Rect2 l_View{ Vec2{ l_Box.min.x, l_Box.min.y + l_ListPad }, Vec2{ l_Box.max.x, l_Box.max.y - l_ListPad } };
		m_Ui.panel(l_Box, m_Ui.px(12.f));

		const float l_ContentHeight = l_ListRow * static_cast<float>(l_Rows);
		const float l_ViewHeight = l_View.max.y - l_View.min.y;
		const float l_MaxScroll = std::max(0.f, l_ContentHeight - l_ViewHeight);
		if (m_LanguageScrollToSelected)
		{
			m_LanguageScrollToSelected = false;
			size_t l_Selected = 0;
			if (!m_LanguageSetting.empty())
			{
				const auto l_It = std::ranges::find(m_Languages, l_Current, &LanguageInfo::code);
				l_Selected = l_It == m_Languages.end() ? 0 : static_cast<size_t>(l_It - m_Languages.begin()) + 1;
			}
			m_LanguageScroll = static_cast<float>(l_Selected) * l_ListRow - l_ViewHeight * 0.5f + l_ListRow * 0.5f;
		}
		m_LanguageScroll = std::clamp(m_LanguageScroll, 0.f, l_MaxScroll);
		const float l_Scroll = m_Ui.anim(ui::Context::id("settings.language.scroll"), m_LanguageScroll, 30.f);
		// The wheel handler works in window coordinates
		m_LanguageListRect = m_Ui.rightToLeftLayout() ? Rect2{ Vec2{ l_Viewport.x - l_Box.max.x, l_Box.min.y }, Vec2{ l_Viewport.x - l_Box.min.x, l_Box.max.y } } : l_Box;

		l_Draw.pushClip(l_View);
		const float l_RowRight = l_View.max.x - l_ListPad - (l_MaxScroll > 0.f ? m_Ui.px(8.f) : 0.f);
		for (size_t i = 0; i < l_Rows; ++i)
		{
			const float l_RowTop = l_View.min.y + static_cast<float>(i) * l_ListRow - l_Scroll;
			if (l_RowTop + l_ListRow < l_View.min.y)
				continue;
			if (l_RowTop > l_View.max.y)
				break;
			const Rect2 l_Full{ Vec2{ l_View.min.x + l_ListPad, l_RowTop }, Vec2{ l_RowRight, l_RowTop + l_ListRow } };
			const Rect2 l_Visible{ Vec2{ l_Full.min.x, std::max(l_Full.min.y, l_View.min.y) }, Vec2{ l_Full.max.x, std::min(l_Full.max.y, l_View.max.y) } };
			const bool l_Auto = i == 0;
			const LanguageInfo* l_Info = l_Auto ? nullptr : &m_Languages[i - 1];
			const bool l_Checked = l_Auto ? m_LanguageSetting.empty() : (l_Info->code == l_Current && !m_LanguageSetting.empty());
			// A row cut by the edge only answers inside the visible part
			const std::string l_Key = l_Auto ? std::string("settings.language.auto") : "settings.language." + l_Info->code;
			if (m_Ui.menuRow(l_Key, l_Full, ui::Icon::None, l_Auto ? tr("settings.language.auto") : std::string_view(l_Info->name), "", true, l_Checked) && l_Visible.contains(m_Ui.pointerPosition()))
			{
				setUiLanguage(l_Auto ? std::string() : l_Info->code);
				m_LanguageListOpen = false;
			}
		}
		l_Draw.popClip();

		if (l_MaxScroll > 0.f)
		{
			const float l_Track = l_ViewHeight - m_Ui.px(8.f);
			const float l_Thumb = std::max(m_Ui.px(28.f), l_Track * l_ViewHeight / l_ContentHeight);
			const float l_ThumbTop = l_View.min.y + m_Ui.px(4.f) + (l_Track - l_Thumb) * (l_Scroll / l_MaxScroll);
			const Rect2 l_Grab = Rect2::fromPosSize(Vec2{ l_View.max.x - m_Ui.px(14.f), l_ThumbTop }, Vec2{ m_Ui.px(14.f), l_Thumb });
			const uint32_t l_Id = ui::Context::id("settings.language.scrollbar");
			const ui::Interaction l_Action = m_Ui.interact(l_Id, l_Grab);
			if (l_Action.held)
				m_LanguageScroll = std::clamp((m_Ui.pointerPosition().y - l_View.min.y - l_Thumb * 0.5f) / std::max(l_Track - l_Thumb, 1.f), 0.f, 1.f) * l_MaxScroll;
			const float l_Emphasis = m_Ui.anim(l_Id + 1, l_Action.hovered || l_Action.held ? 1.f : 0.f, 24.f);
			const float l_BarWidth = m_Ui.px(3.f + 2.f * l_Emphasis);
			l_Draw.rect(Rect2::fromPosSize(Vec2{ l_View.max.x - m_Ui.px(7.f) - l_BarWidth * 0.5f, l_ThumbTop }, Vec2{ l_BarWidth, l_Thumb }), l_BarWidth * 0.5f, ui::withAlpha(l_Theme.textMuted, 0.45f + 0.3f * l_Emphasis));
		}
	}
	l_Draw.setOpacity(1.f);
}

// ------------------------------------------------------------------------------------------------ dialogs

void App::buildDialogs()
{
	if (!modalOpen())
	{
		m_Ui.setAnim(ui::Context::id("dialog.clear", 1), 0.f);
		m_Ui.setAnim(ui::Context::id("dialog.prompt", 1), 0.f);
		m_Ui.setAnim(ui::Context::id("dialog.recovery", 1), 0.f);
		m_Ui.setAnim(ui::Context::id("dialog.message", 1), 0.f);
		m_Ui.setAnim(ui::Context::id("dialog.collab", 1), 0.f);
		return;
	}

	if (m_MessageOpen)
	{
		static constexpr std::array<DialogButton, 1> BUTTONS{ DialogButton{ "common.ok", ui::ButtonStyle::Primary } };
		if (dialogCard(m_Ui, "dialog.message", "app.name", m_Message, BUTTONS) == 0)
			m_MessageOpen = false;
		return;
	}

	if (m_CollabDialog != CollabDialog::None)
	{
		buildCollabDialog();
		return;
	}

	if (collabBusy())
	{
		buildCollabBusy();
		return;
	}

	if (m_ShowExport)
	{
		buildExportDialog();
		return;
	}

	if (m_ClearOpen)
	{
		static constexpr std::array<DialogButton, 2> BUTTONS{ DialogButton{ "common.cancel", ui::ButtonStyle::Secondary }, DialogButton{ "dialog.clear.confirm", ui::ButtonStyle::Primary } };
		const bool l_Shared = m_Collab && m_Collab->active();
		const std::string l_Body = tr(l_Shared ? "dialog.clear.bodyShared" : "dialog.clear.body");
		const int l_Clicked = dialogCard(m_Ui, "dialog.clear", "dialog.clear.title", l_Body, BUTTONS);
		if (l_Clicked >= 0)
		{
			m_ClearOpen = false;
			if (l_Clicked == 1 && !(m_Collab && m_Collab->role() == net::Role::Client))
			{
				m_Editor.clearBoard();
				showToast(tr("toast.boardCleared"));
			}
		}
		return;
	}

	if (m_PromptOpen)
	{
		static constexpr std::array<DialogButton, 3> BUTTONS{ DialogButton{ "common.cancel", ui::ButtonStyle::Secondary }, DialogButton{ "dialog.prompt.discard", ui::ButtonStyle::Secondary }, DialogButton{ "dialog.prompt.save", ui::ButtonStyle::Primary } };
		const std::string l_Body = tr("dialog.prompt.body", { m_Session.displayName() });
		const Action l_Action = m_PromptAction;
		switch (dialogCard(m_Ui, "dialog.prompt", "dialog.prompt.title", l_Body, BUTTONS))
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
		static constexpr std::array<DialogButton, 2> BUTTONS{ DialogButton{ "dialog.recovery.discard", ui::ButtonStyle::Secondary }, DialogButton{ "dialog.recovery.recover", ui::ButtonStyle::Primary } };
		const std::string l_Name = m_Recovery->sourcePath.empty() ? tr("dialog.recovery.untitled") : "\"" + pathToUtf8(pathFromUtf8(m_Recovery->sourcePath).filename()) + "\"";
		const std::string l_Body = tr("dialog.recovery.body", { l_Name, std::to_string(m_Recovery->objectCount), describeAge(m_Recovery->ageSeconds) });
		switch (dialogCard(m_Ui, "dialog.recovery", "dialog.recovery.title", l_Body, BUTTONS))
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
				showToast(tr("toast.recovered"));
			}
			else
			{
				showMessage(tr("dialog.recovery.failed", { l_Result.error }));
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
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const int l_Px = m_Ui.fontPx(14.f);
	const float l_Height = m_Ui.px(42.f);
	const Vec2 l_Viewport = m_Ui.viewport();

	// Work in the background (a big file being read, a PDF being drawn): a pill with a spinner that stays until it is done
	const std::string l_Busy = busyText();
	if (!l_Busy.empty())
		m_BusyShown = l_Busy;
	const float l_BusyShow = m_Ui.anim(ui::Context::id("busy"), l_Busy.empty() ? 0.f : 1.f, 14.f);
	if (l_BusyShow >= 0.01f && !m_BusyShown.empty())
	{
		const float l_SpinnerSpace = m_Ui.px(30.f);
		const float l_Width = m_Ui.font().measure(m_BusyShown, l_Px) + m_Ui.px(44.f) + l_SpinnerSpace;
		const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, m_Ui.px(70.f) - (1.f - l_BusyShow) * m_Ui.px(12.f) }, Vec2{ l_Width, l_Height });
		l_Draw.setOpacity(l_BusyShow);
		l_Draw.shadow(l_Rect, l_Height * 0.5f, m_Ui.px(18.f), Vec2{ 0.f, m_Ui.px(6.f) }, l_Theme.shadow);
		l_Draw.rect(l_Rect, l_Height * 0.5f, l_Theme.tooltip);
		const Vec2 l_Spinner{ l_Rect.min.x + m_Ui.px(22.f) + m_Ui.px(8.f), l_Rect.center().y };
		const float l_Radius = m_Ui.px(8.f);
		const double l_Time = static_cast<double>(SDL_GetTicksNS()) * 1e-9;
		l_Draw.ring(l_Spinner, l_Radius, ui::withAlpha(l_Theme.tooltipText, 0.25f), 2.f * m_Ui.scale());
		constexpr int SEGMENTS = 14;
		std::array<Vec2, SEGMENTS + 1> l_Arc{};
		for (int s = 0; s <= SEGMENTS; ++s)
		{
			const double l_Angle = l_Time * 5.0 + (static_cast<double>(s) / SEGMENTS) * 1.9;
			l_Arc[static_cast<size_t>(s)] = l_Spinner + Vec2{ static_cast<float>(std::cos(l_Angle)), static_cast<float>(std::sin(l_Angle)) } * l_Radius;
		}
		l_Draw.polyline(l_Arc, 2.f * m_Ui.scale(), l_Theme.tooltipText, false);
		l_Draw.text(Vec2{ l_Spinner.x + l_Radius + m_Ui.px(14.f), l_Rect.center().y }, m_BusyShown, l_Px, l_Theme.tooltipText);
		l_Draw.setOpacity(1.f);
		requestRedraw(2); // keeps the spinner turning
	}

	const bool l_Active = m_ToastUntilNs != 0 && !m_Toast.empty();
	const float l_Show = m_Ui.anim(ui::Context::id("toast"), l_Active ? 1.f : 0.f, 14.f);
	if (l_Show < 0.01f || m_Toast.empty())
		return;

	const float l_Width = m_Ui.font().measure(m_Toast, l_Px) + m_Ui.px(44.f);
	// Under the busy pill when both are showing
	const float l_Top = m_Ui.px(70.f) + l_BusyShow * (l_Height + m_Ui.px(10.f));
	const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, l_Top - (1.f - l_Show) * m_Ui.px(12.f) }, Vec2{ l_Width, l_Height });
	l_Draw.setOpacity(l_Show);
	l_Draw.shadow(l_Rect, l_Height * 0.5f, m_Ui.px(18.f), Vec2{ 0.f, m_Ui.px(6.f) }, l_Theme.shadow);
	l_Draw.rect(l_Rect, l_Height * 0.5f, l_Theme.tooltip);
	l_Draw.text(Vec2{ l_Rect.center().x, l_Rect.center().y }, m_Toast, l_Px, l_Theme.tooltipText, ui::TextAlign::Center);
	l_Draw.setOpacity(1.f);
}
} // namespace wb
