// App: the always-visible interface (canvas overlays, top bar, tool bar, zoom pill) and popup handling.
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
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <volk.h>

module wb.app;

import wb.doc.commands;
import wb.doc.gizmo;
import wb.editor;
import wb.io.file;
import wb.math;
import wb.platform.input;
import wb.session;
import wb.tools.select;
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

constexpr float EDGE_MARGIN = 16.f;     // points between floating panels and the window edge
constexpr float POPOVER_GAP = 12.f;     // points between the tool bar and its popover

ui::Icon toolIcon(const tools::ToolKind p_Kind)
{
	switch (p_Kind)
	{
	case tools::ToolKind::Pen:
		return ui::Icon::Pen;
	case tools::ToolKind::Eraser:
		return ui::Icon::Eraser;
	case tools::ToolKind::Select:
		return ui::Icon::Select;
	case tools::ToolKind::Hand:
		return ui::Icon::Hand;
	}
	return ui::Icon::None;
}

ui::Icon deviceIcon(const platform::PointerDevice p_Device)
{
	switch (p_Device)
	{
	case platform::PointerDevice::Mouse:
		return ui::Icon::Mouse;
	case platform::PointerDevice::Pen:
		return ui::Icon::Tablet;
	case platform::PointerDevice::Touch:
		return ui::Icon::Touch;
	}
	return ui::Icon::Mouse;
}

const char* deviceLabel(const platform::PointerDevice p_Device)
{
	switch (p_Device)
	{
	case platform::PointerDevice::Mouse:
		return "mouse";
	case platform::PointerDevice::Pen:
		return "pen";
	case platform::PointerDevice::Touch:
		return "touch";
	}
	return "mouse";
}
} // namespace

// ------------------------------------------------------------------------------------------------ popups

void App::toggleInterface()
{
	m_HideUi = !m_HideUi;
	closePopup("interface hidden");
	m_ShowShortcuts = false;
	if (m_HideUi)
		showToast("Interface hidden. Press F2 to bring it back");
	requestRedraw();
}

void App::openPopup(const Popup p_Popup)
{
	if (m_Popup == p_Popup)
		return;
	closePopup("another popup opened");
	m_Popup = p_Popup;
	m_PopupDevice = m_Editor.activeDevice();
	m_PickerOpen = false;
	m_Ui.setAnim(ui::Context::id("popover.open"), 0.f);
	requestRedraw();
}

void App::closePopup(const char* p_Why)
{
	if (m_Popup == Popup::None)
		return;
	spdlog::debug("Popup {} closed: {}", static_cast<int>(m_Popup), p_Why);
	if (m_Popup == Popup::Pen && m_PickerOpen)
		pushRecentColor(m_Editor.brush().color);
	m_Popup = Popup::None;
	m_PickerOpen = false;
	requestRedraw();
}

void App::togglePopup(const Popup p_Popup)
{
	if (m_Popup == p_Popup)
		closePopup("toggled");
	else
		openPopup(p_Popup);
}

void App::pushRecentColor(const Color p_Color)
{
	const uint32_t l_Packed = Color{ p_Color.r, p_Color.g, p_Color.b, 1.f }.toRgba8();
	for (const uint32_t l_Swatch : PALETTE)
	{
		if (l_Swatch == l_Packed)
			return; // palette colours are always at hand
	}
	// Move it to the front (or insert it, dropping the oldest)
	size_t l_Last = m_RecentColors.size() - 1;
	for (size_t i = 0; i < m_RecentColors.size(); ++i)
	{
		if (m_RecentColors[i] == l_Packed)
			l_Last = i;
	}
	for (size_t i = l_Last; i > 0; --i)
		m_RecentColors[i] = m_RecentColors[i - 1];
	m_RecentColors[0] = l_Packed;
}

bool App::handleUiKey(const SDL_KeyboardEvent& p_Event)
{
	requestRedraw();
	const bool l_Escape = p_Event.key == SDLK_ESCAPE;
	const bool l_Enter = p_Event.key == SDLK_RETURN || p_Event.key == SDLK_KP_ENTER;

	if (m_MessageOpen)
	{
		if (!p_Event.repeat && (l_Escape || l_Enter))
			m_MessageOpen = false;
		return true;
	}
	if (m_PromptOpen)
	{
		if (!p_Event.repeat && l_Escape)
		{
			m_PromptOpen = false;
			m_PromptAction = Action::None;
		}
		else if (!p_Event.repeat && l_Enter)
		{
			const Action l_Action = m_PromptAction;
			m_PromptOpen = false;
			m_PromptAction = Action::None;
			requestSave(false, l_Action);
		}
		return true;
	}
	if (m_RecoveryOpen)
	{
		if (!p_Event.repeat && l_Enter && m_Recovery)
		{
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
		}
		return true; // the choice has to be made with the buttons
	}
	if (l_Escape && !p_Event.repeat)
	{
		if (m_ShowShortcuts)
		{
			m_ShowShortcuts = false;
			return true;
		}
		if (m_Popup != Popup::None && !m_Editor.isBusy())
		{
			closePopup("Escape");
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------------------------------------------ canvas overlays

void App::buildCanvasOverlays()
{
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const float l_Scale = m_Ui.scale();
	const Color l_Accent = l_Theme.accent;

	const tools::SelectionOverlay l_Overlay = m_Editor.selectionOverlay();
	if (l_Overlay.marquee)
		l_Draw.rectBorder(Rect2{ l_Overlay.marqueeMin, l_Overlay.marqueeMax }, 2.f * l_Scale, ui::withAlpha(l_Accent, 0.1f), l_Accent, 1.5f * l_Scale);
	if (l_Overlay.lasso.size() >= 2)
		l_Draw.polyline(l_Overlay.lasso, 1.8f * l_Scale, l_Accent, true);

	if (l_Overlay.visible)
	{
		for (const auto& [l_Min, l_Max] : l_Overlay.objectBoxes)
			l_Draw.outline(Rect2{ l_Min, l_Max }, 1.5f * l_Scale, ui::withAlpha(l_Accent, 0.5f), 1.f * l_Scale);
		l_Draw.polyline(l_Overlay.corners, 1.5f * l_Scale, l_Accent, true);

		if (l_Overlay.interactive)
		{
			const auto l_Emphasis = [&](const Handle p_Handle) { return p_Handle == l_Overlay.active ? 2.f : (p_Handle == l_Overlay.hovered ? 1.f : 0.f); };

			l_Draw.line(l_Overlay.rotateStem, l_Overlay.rotateHandle, 1.5f * l_Scale, l_Accent);
			const float l_RotateEmphasis = l_Emphasis(Handle::Rotate);
			const float l_RotateRadius = (6.5f + l_RotateEmphasis) * l_Scale;
			l_Draw.shadow(Rect2::fromCenter(l_Overlay.rotateHandle, Vec2{ l_RotateRadius * 2.f }), l_RotateRadius, 4.f * l_Scale, Vec2{ 0.f, 1.f * l_Scale }, ui::withAlpha(l_Theme.shadow, 0.6f));
			l_Draw.circle(l_Overlay.rotateHandle, l_RotateRadius, l_RotateEmphasis > 0.f ? l_Accent : l_Theme.handleFill);
			l_Draw.ring(l_Overlay.rotateHandle, l_RotateRadius, l_Accent, 1.5f * l_Scale);

			for (int i = 0; i < SCALE_HANDLE_COUNT; ++i)
			{
				if (!l_Overlay.handleShown[static_cast<size_t>(i)])
					continue;
				const float l_HandleEmphasis = l_Emphasis(scaleHandle(i));
				const float l_Half = (4.75f + l_HandleEmphasis) * l_Scale;
				const Rect2 l_Box = Rect2::fromCenter(l_Overlay.handles[static_cast<size_t>(i)], Vec2{ l_Half * 2.f });
				l_Draw.shadow(l_Box, 2.5f * l_Scale, 4.f * l_Scale, Vec2{ 0.f, 1.f * l_Scale }, ui::withAlpha(l_Theme.shadow, 0.6f));
				l_Draw.rectBorder(l_Box, 2.5f * l_Scale, l_HandleEmphasis > 0.f ? l_Accent : l_Theme.handleFill, l_Accent, 1.5f * l_Scale);
			}
		}
	}

	// Brush outline under the pointer: how wide the stroke is on the board right now
	if (!m_CursorOverUi && m_PointerInWindow)
	{
		if (const std::optional<EraserCursor> l_Cursor = m_Editor.brushCursor())
		{
			const float l_Radius = std::max(l_Cursor->radiusPixels, 1.5f * l_Scale);
			l_Draw.ring(l_Cursor->center, l_Radius + l_Scale, Color{ 0.f, 0.f, 0.f, 0.3f }, 1.25f * l_Scale);
			l_Draw.ring(l_Cursor->center, l_Radius, Color{ 1.f, 1.f, 1.f, 0.85f }, 1.25f * l_Scale);
		}
	}

	// Eraser outline under the pointer
	if (!m_CursorOverUi && m_PointerInWindow)
	{
		if (const std::optional<EraserCursor> l_Cursor = m_Editor.eraserCursor())
		{
			l_Draw.ring(l_Cursor->center, l_Cursor->radiusPixels + l_Scale, Color{ 0.f, 0.f, 0.f, 0.35f }, 1.5f * l_Scale);
			l_Draw.ring(l_Cursor->center, l_Cursor->radiusPixels, Color{ 1.f, 1.f, 1.f, 0.92f }, 1.5f * l_Scale);
		}
	}
}

// ------------------------------------------------------------------------------------------------ top bar

void App::buildTopBar()
{
	const float l_Scale = m_Ui.scale();
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Margin = m_Ui.px(EDGE_MARGIN);
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();

	// Board name: opens the menu
	{
		const int l_FontPx = m_Ui.fontPx(14.f);
		const std::string l_Name = m_Ui.font().fit(m_Session.displayName(), l_FontPx, m_Ui.px(220.f));
		const float l_TextWidth = m_Ui.font().measure(l_Name, l_FontPx);
		const bool l_Dirty = m_Session.dirty();
		const float l_DotSpace = l_Dirty ? m_Ui.px(16.f) : 0.f;
		const float l_Height = m_Ui.px(40.f);
		const Rect2 l_Chip = Rect2::fromPosSize(Vec2{ l_Margin, l_Margin }, Vec2{ m_Ui.px(32.f) + l_TextWidth + l_DotSpace, l_Height });
		m_Ui.panel(l_Chip, l_Height * 0.5f);

		const uint32_t l_Id = ui::Context::id("topbar.name");
		const ui::Interaction l_State = m_Ui.interact(l_Id, l_Chip);
		const float l_Hover = m_Ui.anim(l_Id + 1, l_State.hovered ? 1.f : 0.f, 22.f);
		if (l_Hover > 0.001f)
			l_Draw.rect(l_Chip, l_Height * 0.5f, ui::withAlpha(l_Theme.hover, l_Theme.hover.a * l_Hover));
		float l_X = l_Chip.min.x + m_Ui.px(16.f);
		if (l_Dirty)
		{
			l_Draw.circle(Vec2{ l_X + m_Ui.px(3.f), l_Chip.center().y }, m_Ui.px(3.5f), l_Theme.accent);
			l_X += l_DotSpace;
		}
		l_Draw.text(Vec2{ l_X, l_Chip.center().y }, l_Name, l_FontPx, l_Theme.text);
		m_Ui.noteTooltip(l_Id, l_Chip, l_Dirty ? "Unsaved changes. Open the menu (Ctrl+S saves)" : "Open the menu", l_State.hovered);
		if (l_State.clicked)
			togglePopup(Popup::Menu);
	}

	// Theme toggle and menu
	{
		const float l_Button = m_Ui.px(40.f);
		const float l_Pad = m_Ui.px(4.f);
		const Vec2 l_Size{ l_Pad * 2.f + l_Button * 2.f + m_Ui.px(2.f), l_Button + l_Pad * 2.f };
		const Rect2 l_Pill = Rect2::fromPosSize(Vec2{ l_Viewport.x - l_Margin - l_Size.x, l_Margin - m_Ui.px(2.f) }, l_Size);
		m_Ui.panel(l_Pill, l_Size.y * 0.5f);
		const Rect2 l_ThemeButton = Rect2::fromPosSize(l_Pill.min + Vec2{ l_Pad }, Vec2{ l_Button });
		const Rect2 l_MenuButton = Rect2::fromPosSize(Vec2{ l_ThemeButton.max.x + m_Ui.px(2.f), l_ThemeButton.min.y }, Vec2{ l_Button });
		m_MenuAnchor = l_Pill;

		if (m_Ui.iconButton("topbar.theme", l_ThemeButton, m_DarkTheme ? ui::Icon::Sun : ui::Icon::Moon, false, true, m_DarkTheme ? "Switch to the light theme" : "Switch to the dark theme"))
			m_DarkTheme = !m_DarkTheme;
		if (m_Ui.iconButton("topbar.menu", l_MenuButton, ui::Icon::Menu, m_Popup == Popup::Menu, true, "Menu"))
			togglePopup(Popup::Menu);
	}
	(void)l_Scale;
}

// ------------------------------------------------------------------------------------------------ tool bar

void App::buildToolbar()
{
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Margin = m_Ui.px(EDGE_MARGIN + 2.f);
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();

	const float l_Button = m_Ui.px(44.f);
	const float l_Gap = m_Ui.px(2.f);
	const float l_Pad = m_Ui.px(6.f);
	const float l_Chip = m_Ui.px(30.f);
	const float l_Separator = m_Ui.px(17.f);
	constexpr std::array<tools::ToolKind, 4> ORDER{ tools::ToolKind::Select, tools::ToolKind::Pen, tools::ToolKind::Eraser, tools::ToolKind::Hand };

	const float l_Width = l_Pad * 2.f + l_Chip + l_Separator + 4.f * l_Button + 3.f * l_Gap + l_Separator + 2.f * l_Button + l_Gap;
	const float l_Height = l_Button + l_Pad * 2.f;
	const Rect2 l_Bar = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, l_Viewport.y - l_Margin - l_Height }, Vec2{ l_Width, l_Height });
	m_ToolbarRect = l_Bar;
	m_Ui.panel(l_Bar, m_Ui.px(20.f));

	float l_X = l_Bar.min.x + l_Pad;
	const float l_Y = l_Bar.min.y + l_Pad;
	const auto l_Divider = [&]
	{
		const float l_Line = l_X + l_Separator * 0.5f;
		l_Draw.line(Vec2{ l_Line, l_Bar.min.y + m_Ui.px(15.f) }, Vec2{ l_Line, l_Bar.max.y - m_Ui.px(15.f) }, 1.f, l_Theme.divider);
		l_X += l_Separator;
	};

	// Which device's tools these are
	{
		const platform::PointerDevice l_Device = m_Editor.activeDevice();
		const Rect2 l_ChipRect = Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Chip, l_Button });
		const uint32_t l_Id = ui::Context::id("toolbar.device");
		const ui::Interaction l_State = m_Ui.interact(l_Id, l_ChipRect);
		l_Draw.icon(deviceIcon(l_Device), l_ChipRect.center(), m_Ui.fontPx(16.f), m_Editor.toolIsBorrowed() ? l_Theme.accent : l_Theme.textMuted);
		if (m_Editor.toolIsBorrowed())
			l_Draw.circle(Vec2{ l_ChipRect.center().x + m_Ui.px(8.f), l_ChipRect.center().y - m_Ui.px(9.f) }, m_Ui.px(3.f), l_Theme.accent);
		const std::string l_Tip = std::string("Tools for the ") + deviceLabel(l_Device) + (m_Editor.toolIsBorrowed() ? " (borrowed while the key is held)" : ". Mouse, pen and touch each keep their own tool");
		m_Ui.noteTooltip(l_Id, l_ChipRect, l_Tip, l_State.hovered);
		l_X += l_Chip;
	}
	l_Divider();

	const tools::ToolKind l_Selected = m_Editor.selectedTool();
	static const char* const TIPS[] = { "Pen (P). Click again for colours and size", "Eraser (E). Click again for options", "Select (V). Click again for box or lasso", "Hand (H). Drag to move the board" };
	static const char* const SIMPLE_TIPS[] = { "Pen (P)", "Eraser (E)", "Select (V)", "Hand (H). Drag to move the board" };
	for (const tools::ToolKind l_Kind : ORDER)
	{
		const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Button });
		m_ToolRects[static_cast<size_t>(l_Kind)] = l_Rect;
		const bool l_IsSelected = l_Selected == l_Kind;
		const Color l_PenColor = m_Editor.brush().color;
		const char* l_Tip = l_IsSelected ? TIPS[static_cast<size_t>(l_Kind)] : SIMPLE_TIPS[static_cast<size_t>(l_Kind)];
		if (m_Ui.iconButton(std::string("toolbar.tool") + std::to_string(static_cast<int>(l_Kind)), l_Rect, toolIcon(l_Kind), l_IsSelected, true, l_Tip, l_Kind == tools::ToolKind::Pen ? &l_PenColor : nullptr))
		{
			const Popup l_Wanted = l_Kind == tools::ToolKind::Pen ? Popup::Pen : (l_Kind == tools::ToolKind::Eraser ? Popup::Eraser : (l_Kind == tools::ToolKind::Select ? Popup::Select : Popup::None));
			if (l_IsSelected && l_Wanted != Popup::None)
			{
				togglePopup(l_Wanted);
			}
			else
			{
				m_Editor.setTool(l_Kind);
				closePopup("another tool picked");
			}
		}
		l_X += l_Button + l_Gap;
	}
	l_X += -l_Gap;
	l_Divider();

	if (m_Ui.iconButton("toolbar.undo", Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Button }), ui::Icon::Undo, false, m_Editor.history().canUndo() && !m_Editor.isBusy(), "Undo (Ctrl+Z)"))
		m_Editor.undo();
	l_X += l_Button + l_Gap;
	if (m_Ui.iconButton("toolbar.redo", Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Button }), ui::Icon::Redo, false, m_Editor.history().canRedo() && !m_Editor.isBusy(), "Redo (Ctrl+Y)"))
		m_Editor.redo();
}

// ------------------------------------------------------------------------------------------------ zoom pill

void App::buildZoomPill()
{
	const Vec2 l_Viewport = m_Ui.viewport();
	const float l_Margin = m_Ui.px(EDGE_MARGIN + 2.f);
	const float l_Button = m_Ui.px(36.f);
	const float l_Percent = m_Ui.px(58.f);
	const float l_Pad = m_Ui.px(4.f);
	const float l_Separator = m_Ui.px(9.f);
	const float l_Width = l_Pad * 2.f + l_Button * 3.f + l_Percent + l_Separator;
	const float l_Height = l_Button + l_Pad * 2.f;
	const Rect2 l_Pill = Rect2::fromPosSize(Vec2{ l_Viewport.x - m_Ui.px(EDGE_MARGIN) - l_Width, l_Viewport.y - l_Margin - l_Height - m_Ui.px(2.f) }, Vec2{ l_Width, l_Height });
	m_Ui.panel(l_Pill, l_Height * 0.5f);

	float l_X = l_Pill.min.x + l_Pad;
	const float l_Y = l_Pill.min.y + l_Pad;
	if (m_Ui.iconButton("zoom.out", Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Button }), ui::Icon::Minus, false, true, "Zoom out (Ctrl+-)"))
		m_Editor.zoomAroundCenter(1.0 / 1.25);
	l_X += l_Button;

	const int l_Percentage = static_cast<int>(std::lround(m_Editor.camera().zoom() * 100.0));
	if (m_Ui.button("zoom.percent", Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Percent, l_Button }), std::to_string(l_Percentage) + "%", ui::ButtonStyle::Ghost))
		m_Editor.resetZoom();
	l_X += l_Percent;

	if (m_Ui.iconButton("zoom.in", Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Button }), ui::Icon::Plus, false, true, "Zoom in (Ctrl+=)"))
		m_Editor.zoomAroundCenter(1.25);
	l_X += l_Button;

	const float l_Line = l_X + l_Separator * 0.5f;
	m_Ui.draw().line(Vec2{ l_Line, l_Pill.min.y + m_Ui.px(12.f) }, Vec2{ l_Line, l_Pill.max.y - m_Ui.px(12.f) }, 1.f, m_Ui.theme().divider);
	l_X += l_Separator;

	if (m_Ui.iconButton("zoom.fit", Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Button }), ui::Icon::Fit, false, true, "Fit everything (Home)"))
		m_Editor.fitContent();
}
} // namespace wb
