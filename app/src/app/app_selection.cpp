// App: the floating action bar next to the selection (duplicate, delete, lock, transform, recolour).
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

import wb.doc.document;
import wb.doc.commands;
import wb.doc.object;
import wb.io.locale;
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
	if (!m_Editor.hasSelection() || m_Editor.isBusy() || modalOpen() || m_Editor.textEditing() || m_Editor.cropping())
	{
		if (m_Popup == Popup::SelectionColor || m_Popup == Popup::Arrange || m_Popup == Popup::Media)
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
	size_t l_Videos = 0;
	size_t l_Animated = 0;
	size_t l_AnimatedPlaying = 0;
	AssetId l_MediaAsset = INVALID_ASSET_ID;
	for (const ObjectId l_Id : m_Editor.selection().ids())
	{
		const Object* l_Object = m_Editor.document().find(l_Id);
		if (l_Object == nullptr)
			continue;
		if (l_Object->stroke() != nullptr)
			l_HasStrokes = true;
		if (l_Object->text() != nullptr)
			l_HasText = true;
		if (const VideoData* l_Video = l_Object->video())
		{
			l_MediaAsset = l_Video->asset;
			++l_Videos;
		}
		if (const ImageData* l_Image = l_Object->image())
		{
			l_MediaAsset = l_Image->asset;
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

	// duplicate, delete, lock, arrange, then the extras. Order, flips and alignment live in the Transform popover
	size_t l_Buttons = 4;
	const bool l_CanAlign = m_Editor.selection().size() >= 2;
	l_Buttons += l_HasStrokes ? 1 : 0; // smoothing
	l_Buttons += l_Animated > 0 ? 1 : 0;
	const bool l_CanCrop = m_Editor.selection().size() == 1 && l_Pictures + l_Videos == 1;
	// One picture or video selected: its file can be saved on its own
	const bool l_CanSaveFile = m_Editor.selection().size() == 1 && l_MediaAsset != INVALID_ASSET_ID;
	// Shrinking and saving share a popover; either one alone stays on the bar
	const bool l_CanShrink = l_Pictures > l_Animated; // animated pictures cannot be recompressed
	const bool l_FilePopover = l_CanShrink && l_CanSaveFile;
	l_Buttons += l_FilePopover ? 1 : (l_CanShrink ? 1 : 0) + (l_CanSaveFile ? 1 : 0);
	l_Buttons += l_HasText ? 1 : 0;
	const bool l_Tilted = m_Editor.selectionIsTilted();
	// A single stroke can switch between its own axes and the board's (text and pictures always use their own)
	const bool l_SpaceToggle = m_Editor.selection().size() == 1 && l_HasStrokes;
	m_SelectionActions = SelectionActions{ l_CanAlign, l_Tilted, l_SpaceToggle, l_CanCrop, l_CanShrink, l_CanSaveFile, l_MediaAsset };
	const bool l_CanGroup = m_Editor.canGroupSelection();
	const bool l_HasGroup = m_Editor.selectionHasGroup();
	l_Buttons += l_CanGroup || l_HasGroup ? 1 : 0;
	l_Buttons += 1; // notes
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
	if (m_Ui.iconButton("selbar.duplicate", l_Next(), ui::Icon::Duplicate, false, true, tr("tip.duplicate")))
		m_Editor.duplicateSelection();
	const Editor::LockState l_Lock = m_Editor.selectionLockState();
	const bool l_Editable = l_Lock != Editor::LockState::All; // a locked selection can only be unlocked (or copied)
	if (m_Ui.iconButton("selbar.delete", l_Next(), ui::Icon::Trash, false, l_Editable, tr("tip.delete")))
		m_Editor.deleteSelection();
	if (!m_Editor.hasSelection())
		return; // deleted: the rest of the bar is gone
	if (m_Ui.iconButton("selbar.transform", l_Next(), ui::Icon::Move, m_Popup == Popup::Arrange, true, tr(l_CanAlign ? "tip.transform.align" : "tip.transform")))
		togglePopup(Popup::Arrange);
	if (m_Ui.iconButton("selbar.lock", l_Next(), l_Lock == Editor::LockState::None ? ui::Icon::Lock : ui::Icon::Unlock, l_Lock != Editor::LockState::None, true, tr(l_Lock == Editor::LockState::None ? "tip.lock" : "tip.unlock")))
		m_Editor.toggleSelectionLock();
	if (l_CanGroup || l_HasGroup)
	{
		if (m_Ui.iconButton("selbar.group", l_Next(), ui::Icon::Layers, !l_CanGroup, l_Editable, tr(l_CanGroup ? "tip.group" : "tip.ungroup")))
			m_Editor.toggleSelectionGroup();
	}
	const Editor::SelectionNote l_SelectionNote = m_Editor.selectionNote();
	const Color l_NoteBadge = m_Ui.theme().accent;
	if (m_Ui.iconButton("selbar.note", l_Next(), ui::Icon::Note, m_NotesOpen, true, tr("tip.note"), !l_SelectionNote.text.empty() || l_SelectionNote.mixed ? &l_NoteBadge : nullptr))
		toggleNotesPanel();
	if (l_Animated > 0)
	{
		const bool l_Playing = l_AnimatedPlaying == l_Animated;
		if (m_Ui.iconButton("selbar.play", l_Next(), l_Playing ? ui::Icon::Pause : ui::Icon::Play, false, true, tr(l_Playing ? "tip.animPause" : "tip.animPlay")))
			toggleSelectedPlayback();
	}
	if (l_HasStrokes)
	{
		if (m_Ui.iconButton("selbar.smooth", l_Next(), ui::Icon::Spline, false, l_Editable, tr("tip.smooth")))
			m_Editor.smoothSelection();
	}
	if (l_HasText)
	{
		const ui::Rect2 l_TextRect = l_Next();
		m_TextAnchor = l_TextRect;
		if (m_Ui.iconButton("selbar.text", l_TextRect, ui::Icon::Type, m_Popup == Popup::Text, true, tr("tip.selText")))
			openTextPopup(false);
	}
	if (l_FilePopover)
	{
		if (m_Ui.iconButton("selbar.file", l_Next(), ui::Icon::ShrinkImage, m_Popup == Popup::Media, true, tr("tip.selFile")))
			togglePopup(Popup::Media);
	}
	else if (m_Popup == Popup::Media)
	{
		closePopup("nothing to group in the file popover");
	}
	if (l_CanShrink && !l_FilePopover)
	{
		if (m_Ui.iconButton("selbar.shrink", l_Next(), ui::Icon::ShrinkImage, false, true, tr("tip.shrink")))
			compressSelectedPictures();
	}
	if (l_CanSaveFile && !l_FilePopover)
	{
		const ImageAsset* l_HereAsset = m_Editor.document().findAsset(l_MediaAsset);
		const bool l_Here = l_HereAsset != nullptr && l_HereAsset->complete();
		if (m_Ui.iconButton("selbar.savefile", l_Next(), ui::Icon::SaveAs, false, l_Here, tr(l_Here ? "tip.saveFile" : "tip.saveFileLater")))
			showSaveMediaDialog(l_MediaAsset);
	}

	if (l_ShowColour)
	{
		l_Cursor += -l_Gap;
		const float l_Line = l_Cursor + l_Separator * 0.5f;
		m_Ui.draw().line(Vec2{ l_Line, l_Bar.min.y + m_Ui.px(12.f) }, Vec2{ l_Line, l_Bar.max.y - m_Ui.px(12.f) }, 1.f, m_Ui.theme().divider);
		l_Cursor += l_Separator;
		const ui::Rect2 l_ColorRect = l_Next();
		if (m_Ui.iconButton("selbar.colour", l_ColorRect, ui::Icon::Palette, m_Popup == Popup::SelectionColor, true, tr("pop.recolor")))
			togglePopup(Popup::SelectionColor);
	}
	else if (m_Popup == Popup::SelectionColor)
	{
		closePopup("no strokes selected");
	}
}
} // namespace wb
