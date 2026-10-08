// App: the floating action bar next to the selection (duplicate, delete, order, recolour).
module;
#include <algorithm>
#include <array>
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
#include <volk.h>

module wb.app;

import wb.doc.commands;
import wb.doc.object;
import wb.render.canvas_renderer;
import wb.render.image_store;
import wb.editor;
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
void App::buildSelectionBar()
{
	if (!m_Editor.hasSelection() || m_Editor.isBusy() || modalOpen() || m_Editor.textEditing())
	{
		if (m_Popup == Popup::SelectionColor)
			closePopup("selection bar hidden");
		m_SelectionBarRect = ui::Rect2{};
		return;
	}

	const tools::SelectionOverlay l_Overlay = m_Editor.selectionOverlay();
	const Vec2 l_Viewport = m_Ui.viewport();

	// What is selected decides which buttons the bar offers
	bool l_HasStrokes = false;
	bool l_HasText = false;
	size_t l_Pictures = 0;
	size_t l_Animated = 0;
	size_t l_AnimatedPlaying = 0;
	for (const ObjectId l_Id : m_Editor.selection().ids())
	{
		const Object* l_Object = m_Editor.document().find(l_Id);
		if (l_Object == nullptr)
			continue;
		if (l_Object->stroke() != nullptr)
			l_HasStrokes = true;
		if (l_Object->text() != nullptr)
			l_HasText = true;
		if (const ImageData* l_Image = l_Object->image())
		{
			++l_Pictures;
			if (m_Canvas.images().animated(l_Image->asset))
			{
				++l_Animated;
				l_AnimatedPlaying += l_Image->playing ? 1 : 0;
			}
		}
	}

	const float l_Button = m_Ui.px(38.f);
	const float l_Pad = m_Ui.px(5.f);
	const float l_Gap = m_Ui.px(2.f);
	const float l_Separator = m_Ui.px(13.f);
	const int l_CountPx = m_Ui.fontPx(13.f);
	const std::string l_Count = std::to_string(m_Editor.selection().size());
	const float l_CountWidth = m_Ui.font().measure(l_Count, l_CountPx) + m_Ui.px(22.f);

	// duplicate, delete, front, back, flip horizontally, flip vertically, then the extras
	size_t l_Buttons = 6;
	l_Buttons += l_Animated > 0 ? 1 : 0;
	l_Buttons += l_Pictures > 0 ? 1 : 0;
	l_Buttons += l_HasText ? 1 : 0;
	const bool l_Tilted = m_Editor.selectionIsTilted();
	l_Buttons += l_Tilted ? 1 : 0;
	// A single object that is not text can switch between its own axes and the board's
	const bool l_SpaceToggle = m_Editor.selection().size() == 1 && !l_HasText;
	l_Buttons += l_SpaceToggle ? 1 : 0;
	const bool l_ShowColour = l_HasStrokes || l_HasText;
	const float l_Width = l_Pad * 2.f + l_CountWidth + static_cast<float>(l_Buttons) * l_Button + static_cast<float>(l_Buttons - 1) * l_Gap + (l_ShowColour ? l_Separator + l_Button : 0.f);
	const float l_Height = l_Button + l_Pad * 2.f;
	const float l_Margin = m_Ui.px(14.f);
	const float l_SelectionCenter = (l_Overlay.boundsMin.x + l_Overlay.boundsMax.x) * 0.5f;
	const float l_X = std::clamp(l_SelectionCenter - l_Width * 0.5f, l_Margin, std::max(l_Margin, l_Viewport.x - l_Width - l_Margin));

	// Above the selection (clear of the rotate handle), or below it when there is no room; always on screen
	const float l_TopLimit = m_Ui.px(70.f);
	const float l_BottomLimit = l_Viewport.y - m_Ui.px(100.f) - l_Height;
	const float l_Above = l_Overlay.boundsMin.y - m_Ui.px(52.f) - l_Height;
	float l_Y = l_Above >= l_TopLimit ? l_Above : l_Overlay.boundsMax.y + m_Ui.px(16.f);
	l_Y = std::clamp(l_Y, l_TopLimit, std::max(l_TopLimit, l_BottomLimit));

	const ui::Rect2 l_Bar = ui::Rect2::fromPosSize(Vec2{ l_X, l_Y }, Vec2{ l_Width, l_Height });
	m_SelectionBarRect = l_Bar;
	m_Ui.panel(l_Bar, m_Ui.px(18.f));

	float l_Cursor = l_Bar.min.x + l_Pad;
	const float l_ButtonY = l_Bar.min.y + l_Pad;
	m_Ui.draw().text(Vec2{ l_Cursor + m_Ui.px(11.f), l_Bar.center().y }, l_Count, l_CountPx, m_Ui.theme().textMuted);
	l_Cursor += l_CountWidth;

	const auto l_Next = [&]
	{
		const ui::Rect2 l_Rect = ui::Rect2::fromPosSize(Vec2{ l_Cursor, l_ButtonY }, Vec2{ l_Button });
		l_Cursor += l_Button + l_Gap;
		return l_Rect;
	};
	if (m_Ui.iconButton("selbar.duplicate", l_Next(), ui::Icon::Duplicate, false, true, "Duplicate (Ctrl+D)"))
		m_Editor.duplicateSelection();
	if (m_Ui.iconButton("selbar.delete", l_Next(), ui::Icon::Trash, false, true, "Delete (Del)"))
		m_Editor.deleteSelection();
	if (!m_Editor.hasSelection())
		return; // deleted: the rest of the bar is gone
	if (m_Ui.iconButton("selbar.front", l_Next(), ui::Icon::BringToFront, false, true, "Bring to front (Ctrl+Shift+])"))
		m_Editor.reorderSelection(ZOrderMove::ToFront);
	if (m_Ui.iconButton("selbar.back", l_Next(), ui::Icon::SendToBack, false, true, "Send to back (Ctrl+Shift+[)"))
		m_Editor.reorderSelection(ZOrderMove::ToBack);
	if (m_Ui.iconButton("selbar.fliph", l_Next(), ui::Icon::FlipH, false, true, "Flip horizontally"))
		m_Editor.flipSelection(true);
	if (m_Ui.iconButton("selbar.flipv", l_Next(), ui::Icon::FlipV, false, true, "Flip vertically"))
		m_Editor.flipSelection(false);
	if (l_Animated > 0)
	{
		const bool l_Playing = l_AnimatedPlaying == l_Animated;
		if (m_Ui.iconButton("selbar.play", l_Next(), l_Playing ? ui::Icon::Pause : ui::Icon::Play, false, true, l_Playing ? "Pause the animation" : "Play the animation"))
			toggleSelectedPlayback();
	}
	if (l_SpaceToggle)
	{
		tools::SelectState& l_Select = m_Editor.selectState();
		const bool l_Local = l_Select.space == tools::TransformSpace::Local;
		if (m_Ui.iconButton("selbar.space", l_Next(), ui::Icon::TransformSpace, l_Local, true, l_Local ? "Transform box follows the object. Click for the board's axes" : "Transform box uses the board's axes. Click to follow the object"))
			l_Select.space = l_Local ? tools::TransformSpace::Global : tools::TransformSpace::Local;
	}
	if (l_Tilted)
	{
		if (m_Ui.iconButton("selbar.reset", l_Next(), ui::Icon::ResetTransform, false, true, "Make upright: remove rotation and flips"))
			m_Editor.resetSelectionTransform();
	}
	if (l_HasText)
	{
		const ui::Rect2 l_TextRect = l_Next();
		m_TextAnchor = l_TextRect;
		if (m_Ui.iconButton("selbar.text", l_TextRect, ui::Icon::Type, m_Popup == Popup::Text, true, "Font, size and alignment of the selected text"))
			openTextPopup(false);
	}
	if (l_Pictures > 0)
	{
		if (m_Ui.iconButton("selbar.shrink", l_Next(), ui::Icon::ShrinkImage, false, true, "Reduce the file size of the selected pictures"))
			compressSelectedPictures();
	}

	if (l_ShowColour)
	{
		l_Cursor += -l_Gap;
		const float l_Line = l_Cursor + l_Separator * 0.5f;
		m_Ui.draw().line(Vec2{ l_Line, l_Bar.min.y + m_Ui.px(12.f) }, Vec2{ l_Line, l_Bar.max.y - m_Ui.px(12.f) }, 1.f, m_Ui.theme().divider);
		l_Cursor += l_Separator;
		const ui::Rect2 l_ColorRect = l_Next();
		if (m_Ui.iconButton("selbar.colour", l_ColorRect, ui::Icon::Palette, m_Popup == Popup::SelectionColor, true, "Recolour the selection"))
			togglePopup(Popup::SelectionColor);
	}
	else if (m_Popup == Popup::SelectionColor)
	{
		closePopup("no strokes selected");
	}
}
} // namespace wb
