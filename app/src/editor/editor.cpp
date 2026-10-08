module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

module wb.editor;

import wb.math;
import wb.doc.commands;
import wb.doc.document;
import wb.doc.edit;
import wb.doc.history;
import wb.doc.object;
import wb.doc.selection;
import wb.io.serializer;
import wb.view.camera;
import wb.brush.stroke_builder;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.tools.tool;
import wb.tools.pen;
import wb.tools.eraser;
import wb.tools.select;

namespace wb
{
namespace
{
constexpr double WHEEL_ZOOM_STEP = 1.2;     // per wheel notch
constexpr double KEY_ZOOM_STEP = 1.25;
constexpr double WHEEL_PAN_POINTS = 60.0;   // per wheel notch
constexpr double PAN_RATE = 14.0;           // 1/s; scroll easing
constexpr double ANIMATION_RATE = 18.0;    // 1/s; higher = snappier camera animations
constexpr double FIT_MARGIN_POINTS = 48.0;
constexpr double DUPLICATE_OFFSET_POINTS = 24.0;
constexpr float NUDGE_POINTS = 1.f;
constexpr float NUDGE_POINTS_BIG = 10.f;
constexpr uint64_t NUDGE_MERGE_NS = 700'000'000ull; // arrow taps closer together than this are one undo step

bool isPanButton(const platform::PointerButton p_Button)
{
	return p_Button == platform::PointerButton::Secondary || p_Button == platform::PointerButton::Middle;
}
} // namespace

Editor::Editor()
	: m_Pen(std::make_unique<tools::PenTool>()), m_Eraser(std::make_unique<tools::EraserTool>()), m_Select(std::make_unique<tools::SelectTool>()), m_Hand(std::make_unique<tools::HandTool>())
{
	m_ActiveTool = m_Pen.get();
}

void Editor::setViewport(const Vec2 p_SizePixels, const float p_PixelScale)
{
	m_Camera.setViewport(p_SizePixels, p_PixelScale);
}

tools::ToolContext Editor::toolContext()
{
	return tools::ToolContext{
		.document = m_Document,
		.history = m_History,
		.selection = m_Selection,
		.select = m_SelectState,
		.camera = m_Camera,
		.brush = m_Brush,
		.eraser = m_EraserState,
		.brushSettings = m_BrushSettings,
	};
}

tools::Tool* Editor::toolFor(const tools::ToolKind p_Kind) const
{
	switch (p_Kind)
	{
	case tools::ToolKind::Eraser:
		return m_Eraser.get();
	case tools::ToolKind::Select:
		return m_Select.get();
	case tools::ToolKind::Hand:
		return m_Hand.get();
	default:
		return m_Pen.get();
	}
}

// The tool that would handle a new gesture right now. A borrowed tool (Alt+key) wins over the device's own choice;
// the pen's eraser end always erases.
tools::Tool* Editor::toolInEffect() const
{
	if (m_ActiveTool != nullptr && m_ActiveTool->isBusy())
		return m_ActiveTool;
	if (m_Momentary)
		return toolFor(m_Momentary->kind);
	if (m_HoverEraser)
		return m_Eraser.get();
	return toolFor(m_DeviceTools[static_cast<size_t>(m_LastDevice)]);
}

tools::ToolKind Editor::selectedTool() const
{
	return m_Momentary ? m_Momentary->kind : m_DeviceTools[static_cast<size_t>(m_LastDevice)];
}

void Editor::setTool(const tools::ToolKind p_Kind)
{
	setDeviceTool(m_LastDevice, p_Kind);
}

void Editor::setDeviceTool(const platform::PointerDevice p_Device, const tools::ToolKind p_Kind)
{
	m_DeviceTools[static_cast<size_t>(p_Device)] = p_Kind;
	if (p_Device != m_LastDevice)
		return;
	if (m_ActiveTool != nullptr && m_ActiveTool->isBusy())
	{
		tools::ToolContext l_Context = toolContext();
		m_ActiveTool->cancel(l_Context);
	}
	m_ActiveTool = toolInEffect();
}

void Editor::borrowTool(const tools::ToolKind p_Kind, const SDL_Keycode p_Key)
{
	if (m_Momentary && m_Momentary->kind == p_Kind && m_Momentary->key == p_Key)
		return;
	m_Momentary = BorrowedTool{ .kind = p_Kind, .key = p_Key };
	if (m_ActiveTool == nullptr || !m_ActiveTool->isBusy())
		m_ActiveTool = toolInEffect();
}

void Editor::returnTool()
{
	if (!m_Momentary)
		return;
	m_Momentary.reset();
	if (m_ActiveTool == nullptr || !m_ActiveTool->isBusy())
		m_ActiveTool = toolInEffect();
}

std::optional<EraserCursor> Editor::eraserCursor() const
{
	if (m_Panning || m_SpaceHeld || toolInEffect() != m_Eraser.get())
		return std::nullopt;
	return EraserCursor{ .center = m_LastPointer, .radiusPixels = m_EraserState.sizePoints * 0.5f * static_cast<float>(m_Camera.pixelScale()) };
}

std::optional<EraserCursor> Editor::brushCursor() const
{
	if (m_Panning || m_SpaceHeld || toolInEffect() != m_Pen.get())
		return std::nullopt;
	return EraserCursor{ .center = m_LastPointer, .radiusPixels = m_Brush.effectivePoints(m_Camera.zoom()) * 0.5f * static_cast<float>(m_Camera.pixelScale()) };
}

bool Editor::isBusy() const
{
	return m_Panning || (m_ActiveTool != nullptr && m_ActiveTool->isBusy());
}

tools::CursorKind Editor::cursor() const
{
	if (m_Panning || m_SpaceHeld)
		return tools::CursorKind::Move;
	return m_ActiveTool != nullptr ? m_ActiveTool->cursor() : tools::CursorKind::Default;
}

std::optional<render::LiveStrokeView> Editor::liveStroke() const
{
	const StrokeBuilder& l_Builder = m_Pen->builder();
	if (!l_Builder.isActive())
		return std::nullopt;
	return render::LiveStrokeView{
		.origin = l_Builder.origin(),
		.color = l_Builder.style().color,
		.committed = l_Builder.committedPoints(),
		.tail = l_Builder.tailPoints(),
	};
}

// ------------------------------------------------------------------------------------------------ input

void Editor::notePointerDevice(const platform::PointerDevice p_Device)
{
	if (p_Device == m_LastDevice)
		return;
	m_LastDevice = p_Device;
	if (m_ActiveTool == nullptr || !m_ActiveTool->isBusy())
		m_ActiveTool = toolInEffect();
}

void Editor::handlePointer(const platform::PointerEvent& p_Event)
{
	m_LastPointer = p_Event.position;
	m_LastDevice = p_Event.device;
	m_HoverEraser = p_Event.device == platform::PointerDevice::Pen && p_Event.eraser;

	// Panning gesture in progress
	if (m_Panning)
	{
		if (p_Event.device != m_PanDevice)
			return;
		if (p_Event.phase == platform::PointerPhase::Move)
		{
			m_Animation = CameraAnimation::None;
			m_Camera.panPixels(DVec2{ p_Event.position - m_PanLast });
			m_PanLast = p_Event.position;
		}
		else if (p_Event.phase == platform::PointerPhase::Cancel || (p_Event.phase == platform::PointerPhase::Up && p_Event.button == m_PanButton))
		{
			m_Panning = false;
		}
		return;
	}

	// Start panning (never in the middle of a tool gesture)
	const bool l_ToolBusy = m_ActiveTool != nullptr && m_ActiveTool->isBusy();
	if (!l_ToolBusy)
		m_ActiveTool = toolInEffect();
	const bool l_HandTool = m_ActiveTool != nullptr && m_ActiveTool->kind() == tools::ToolKind::Hand;
	if (p_Event.phase == platform::PointerPhase::Down && !l_ToolBusy && (isPanButton(p_Event.button) || ((m_SpaceHeld || l_HandTool) && p_Event.button == platform::PointerButton::Primary)))
	{
		m_Panning = true;
		m_PanDevice = p_Event.device;
		m_PanButton = p_Event.button;
		m_PanLast = p_Event.position;
		return;
	}

	if (m_ActiveTool != nullptr)
	{
		// Starting to draw leaves the selection behind
		if (p_Event.phase == platform::PointerPhase::Down && p_Event.button == platform::PointerButton::Primary && m_ActiveTool->kind() == tools::ToolKind::Pen)
			m_Selection.clear();
		tools::ToolContext l_Context = toolContext();
		m_ActiveTool->onPointer(p_Event, l_Context);
	}
}

void Editor::handleWheel(const platform::WheelEvent& p_Event)
{
	m_LastPointer = p_Event.position;
	// Wheel zooms by default; Ctrl + wheel scrolls (Shift: horizontally)
	if ((p_Event.modifiers & platform::Modifier::Ctrl) == 0)
	{
		const double l_Base = m_Animation == CameraAnimation::ZoomAtAnchor ? m_TargetZoom : m_Camera.zoom();
		startZoom(l_Base * std::pow(WHEEL_ZOOM_STEP, static_cast<double>(p_Event.delta.y)), DVec2{ p_Event.position });
		return;
	}

	DVec2 l_Delta{ p_Event.delta.x, p_Event.delta.y };
	if ((p_Event.modifiers & platform::Modifier::Shift) != 0 && l_Delta.x == 0.0)
		l_Delta = DVec2{ l_Delta.y, 0.0 };
	// Wheel up / right moves the view up / right, so content moves down / left
	const double l_Step = WHEEL_PAN_POINTS * m_Camera.pixelScale();
	m_PendingPan += DVec2{ -l_Delta.x * l_Step, l_Delta.y * l_Step };
}

void Editor::handlePinch(const platform::PinchEvent& p_Event)
{
	m_Animation = CameraAnimation::None;
	m_Camera.zoomAt(DVec2{ m_LastPointer }, m_Camera.zoom() * static_cast<double>(p_Event.scale));
}

bool Editor::handleKeyDown(const SDL_KeyboardEvent& p_Event)
{
	const bool l_Ctrl = (p_Event.mod & SDL_KMOD_CTRL) != 0;
	const bool l_Shift = (p_Event.mod & SDL_KMOD_SHIFT) != 0;
	const bool l_Alt = (p_Event.mod & SDL_KMOD_ALT) != 0;
	const SDL_Keycode l_Key = p_Event.key;

	if (l_Key == SDLK_SPACE)
	{
		m_SpaceHeld = true;
		return true;
	}

	// Keys that make sense while held down
	if (l_Ctrl && l_Key == SDLK_Z)
	{
		l_Shift ? redo() : undo();
		return true;
	}
	if (l_Ctrl && l_Key == SDLK_Y)
	{
		redo();
		return true;
	}
	if (l_Ctrl && (l_Key == SDLK_EQUALS || l_Key == SDLK_PLUS || l_Key == SDLK_KP_PLUS))
	{
		zoomAroundCenter(KEY_ZOOM_STEP);
		return true;
	}
	if (l_Ctrl && (l_Key == SDLK_MINUS || l_Key == SDLK_KP_MINUS))
	{
		zoomAroundCenter(1.0 / KEY_ZOOM_STEP);
		return true;
	}
	if (!l_Ctrl && !l_Alt && !m_Selection.empty() && !isBusy() && (l_Key == SDLK_LEFT || l_Key == SDLK_RIGHT || l_Key == SDLK_UP || l_Key == SDLK_DOWN))
	{
		const float l_Step = l_Shift ? NUDGE_POINTS_BIG : NUDGE_POINTS;
		nudgeSelection(Vec2{ l_Key == SDLK_LEFT ? -l_Step : (l_Key == SDLK_RIGHT ? l_Step : 0.f), l_Key == SDLK_UP ? -l_Step : (l_Key == SDLK_DOWN ? l_Step : 0.f) });
		return true;
	}

	// Everything below acts once per key press
	const auto l_Handled = [&]() { return true; };
	if (l_Ctrl && (l_Key == SDLK_0 || l_Key == SDLK_KP_0))
	{
		if (!p_Event.repeat)
			resetZoom();
		return l_Handled();
	}
	if (l_Ctrl)
	{
		switch (l_Key)
		{
		case SDLK_A:
			if (!p_Event.repeat)
				selectAll();
			return l_Handled();
		case SDLK_C:
			if (!p_Event.repeat)
				copySelection();
			return l_Handled();
		case SDLK_X:
			if (!p_Event.repeat)
				cutSelection();
			return l_Handled();
		case SDLK_V:
			if (!p_Event.repeat)
				paste();
			return l_Handled();
		case SDLK_D:
			if (!p_Event.repeat)
				duplicateSelection();
			return l_Handled();
		case SDLK_RIGHTBRACKET:
			if (!p_Event.repeat)
				reorderSelection(l_Shift ? ZOrderMove::ToFront : ZOrderMove::Forward);
			return l_Handled();
		case SDLK_LEFTBRACKET:
			if (!p_Event.repeat)
				reorderSelection(l_Shift ? ZOrderMove::ToBack : ZOrderMove::Backward);
			return l_Handled();
		default:
			return false;
		}
	}

	if (l_Key == SDLK_DELETE || l_Key == SDLK_BACKSPACE)
	{
		if (!p_Event.repeat)
			deleteSelection();
		return l_Handled();
	}
	if (l_Key == SDLK_ESCAPE)
	{
		if (m_ActiveTool != nullptr && m_ActiveTool->isBusy() && m_ActiveTool->kind() == tools::ToolKind::Select)
		{
			tools::ToolContext l_Context = toolContext();
			m_ActiveTool->cancel(l_Context); // a move, scale or rotation in progress is undone
		}
		else
		{
			clearSelection();
		}
		return l_Handled();
	}
	if (l_Key == SDLK_HOME)
	{
		fitContent();
		return true;
	}

	// Tool keys. With Alt the tool is only borrowed while the key stays down.
	std::optional<tools::ToolKind> l_Tool;
	switch (l_Key)
	{
	case SDLK_P:
		l_Tool = tools::ToolKind::Pen;
		break;
	case SDLK_E:
		l_Tool = tools::ToolKind::Eraser;
		break;
	case SDLK_V:
		l_Tool = tools::ToolKind::Select;
		break;
	case SDLK_H:
		l_Tool = tools::ToolKind::Hand;
		break;
	default:
		break;
	}
	if (l_Tool)
	{
		if (!p_Event.repeat)
			l_Alt ? borrowTool(*l_Tool, l_Key) : setTool(*l_Tool);
		return true;
	}
	return false;
}

void Editor::handleKeyUp(const SDL_KeyboardEvent& p_Event)
{
	if (p_Event.key == SDLK_SPACE)
		m_SpaceHeld = false;
	// Letting go of either the tool key or Alt gives the borrowed tool back
	if (m_Momentary && (p_Event.key == m_Momentary->key || p_Event.key == SDLK_LALT || p_Event.key == SDLK_RALT))
		returnTool();
}

void Editor::handleFocusLost()
{
	m_SpaceHeld = false;
	m_Panning = false;
	if (m_ActiveTool != nullptr)
	{
		tools::ToolContext l_Context = toolContext();
		m_ActiveTool->cancel(l_Context);
	}
	returnTool();
}

// Ends whatever the pointer is doing (stroke, erase drag, pan, scroll easing, camera animation)
void Editor::cancelGestures()
{
	m_Panning = false;
	m_PendingPan = DVec2{ 0.0 };
	m_Animation = CameraAnimation::None;
	if (m_ActiveTool != nullptr)
	{
		tools::ToolContext l_Context = toolContext();
		m_ActiveTool->cancel(l_Context);
	}
}

// ------------------------------------------------------------------------------------------------ selection

tools::SelectionOverlay Editor::selectionOverlay()
{
	tools::ToolContext l_Context = toolContext();
	return m_Select->overlay(l_Context, toolInEffect() == m_Select.get());
}

void Editor::selectAll()
{
	if (isBusy())
		return;
	std::vector<ObjectId> l_Ids;
	l_Ids.reserve(m_Document.size());
	for (const std::unique_ptr<Object>& l_Object : m_Document.objects())
		l_Ids.push_back(l_Object->id);
	m_Selection.set(l_Ids);
}

void Editor::clearSelection()
{
	m_Selection.clear();
}

void Editor::deleteSelection()
{
	if (isBusy() || m_Selection.empty())
		return;
	const std::vector<ObjectId> l_Ids = m_Selection.orderedIds();
	deleteObjects(m_Document, m_History, l_Ids);
}

void Editor::duplicateSelection()
{
	if (isBusy() || m_Selection.empty())
		return;
	const double l_Offset = DUPLICATE_OFFSET_POINTS / m_Camera.zoom();
	const std::vector<ObjectId> l_Copies = duplicateObjects(m_Document, m_History, m_Selection.orderedIds(), DVec2{ l_Offset });
	m_Selection.set(l_Copies);
}

void Editor::copySelection()
{
	if (m_Selection.empty())
		return;
	m_Clip = copyObjects(m_Document, m_Selection.orderedIds());
	m_PasteRepeat = 0;
}

void Editor::cutSelection()
{
	if (isBusy() || m_Selection.empty())
		return;
	copySelection();
	deleteSelection();
}

void Editor::paste()
{
	if (isBusy() || m_Clip.empty())
		return;
	// Lands under the pointer when it is over the canvas, else in the middle of the view
	const DVec2 l_Viewport = m_Camera.viewport();
	const bool l_PointerInside = m_LastPointer.x >= 0.f && m_LastPointer.y >= 0.f && m_LastPointer.x <= l_Viewport.x && m_LastPointer.y <= l_Viewport.y;
	DVec2 l_Center = m_Camera.screenToWorld(l_PointerInside ? DVec2{ m_LastPointer } : l_Viewport * 0.5);

	// Pasting again at the same spot staggers the copies instead of piling them up
	const double l_Stagger = DUPLICATE_OFFSET_POINTS / m_Camera.zoom();
	if (m_PasteRepeat > 0 && glm::length(l_Center - m_LastPasteCenter) < l_Stagger)
		l_Center = m_LastPasteCenter + DVec2{ l_Stagger };
	m_LastPasteCenter = l_Center;
	++m_PasteRepeat;

	const std::vector<ObjectId> l_Ids = pasteObjects(m_Document, m_History, m_Clip, l_Center);
	m_Selection.set(l_Ids);
}

void Editor::reorderSelection(const ZOrderMove p_Move)
{
	if (isBusy() || m_Selection.empty())
		return;
	reorderObjects(m_Document, m_History, m_Selection.orderedIds(), p_Move);
}

void Editor::recolorSelection(const Color p_Color)
{
	if (isBusy() || m_Selection.empty())
		return;
	recolorObjects(m_Document, m_History, m_Selection.orderedIds(), p_Color);
}

void Editor::nudgeSelection(const Vec2 p_Points)
{
	if (isBusy() || m_Selection.empty())
		return;
	const uint64_t l_Now = SDL_GetTicksNS();
	if (l_Now - m_LastNudgeNs > NUDGE_MERGE_NS)
		m_History.sealLast();
	m_LastNudgeNs = l_Now;

	const Affine2 l_Move = Affine2::translate(DVec2{ p_Points } / m_Camera.zoom());
	const uint64_t l_RevisionBefore = m_Document.revision();
	transformObjects(m_Document, m_History, m_Selection.orderedIds(), l_Move, "Nudge", true);
	tools::ToolContext l_Context = toolContext();
	m_Select->noteTransformed(l_Move, l_RevisionBefore, l_Context);
}

// ------------------------------------------------------------------------------------------------ commands

std::vector<uint8_t> Editor::saveBoard(const std::string& p_SourcePath) const
{
	BoardMeta l_Meta;
	l_Meta.viewCenter = m_Camera.center();
	l_Meta.viewZoom = m_Camera.zoom();
	l_Meta.sourcePath = p_SourcePath;
	return serializeBoard(m_Document, l_Meta);
}

LoadResult Editor::loadBoard(const std::span<const uint8_t> p_Bytes, BoardMeta& p_Meta)
{
	cancelGestures();
	const LoadResult l_Result = deserializeBoard(p_Bytes, m_Document, p_Meta);
	if (l_Result.ok)
	{
		m_History.clear();
		lookAt(p_Meta.viewCenter, p_Meta.viewZoom);
	}
	return l_Result;
}

void Editor::newBoard()
{
	cancelGestures();
	m_Document.clear();
	m_History.clear();
	lookAt(DVec2{ 0.0 }, 1.0);
}

void Editor::undo()
{
	if (isBusy())
		return;
	m_History.undo(m_Document);
}

void Editor::redo()
{
	if (isBusy())
		return;
	m_History.redo(m_Document);
}

void Editor::zoomAroundCenter(const double p_Factor)
{
	const double l_Base = m_Animation == CameraAnimation::ZoomAtAnchor ? m_TargetZoom : m_Camera.zoom();
	startZoom(l_Base * p_Factor, m_Camera.viewport() * 0.5);
}

void Editor::resetZoom()
{
	startZoom(1.0, m_Camera.viewport() * 0.5);
}

void Editor::fitContent()
{
	const Rect l_Content = m_Document.contentBounds();
	if (l_Content.isEmpty())
	{
		startFlyTo(DVec2{ 0.0 }, 1.0);
		return;
	}
	Camera l_Target = m_Camera;
	l_Target.fit(l_Content, FIT_MARGIN_POINTS * m_Camera.pixelScale(), 2.0);
	startFlyTo(l_Target.center(), l_Target.zoom());
}

void Editor::lookAt(const DVec2 p_Center, const double p_Zoom)
{
	m_Animation = CameraAnimation::None;
	m_Camera.setZoom(p_Zoom);
	m_Camera.setCenter(p_Center);
}

// ------------------------------------------------------------------------------------------------ animation

void Editor::startZoom(const double p_TargetZoom, const DVec2 p_AnchorScreen)
{
	m_Animation = CameraAnimation::ZoomAtAnchor;
	m_TargetZoom = std::clamp(p_TargetZoom, Camera::MIN_ZOOM, Camera::MAX_ZOOM);
	m_ZoomAnchor = p_AnchorScreen;
}

void Editor::startFlyTo(const DVec2 p_Center, const double p_Zoom)
{
	m_Animation = CameraAnimation::FlyTo;
	m_TargetCenter = p_Center;
	m_TargetZoom = std::clamp(p_Zoom, Camera::MIN_ZOOM, Camera::MAX_ZOOM);
}

bool Editor::update(const double p_DeltaSeconds)
{
	// Frame-rate independent exponential approach
	const double l_T = 1.0 - std::exp(-ANIMATION_RATE * std::clamp(p_DeltaSeconds, 0.0, 0.1));

	// Eased scroll: move a fraction of the remaining distance each frame
	bool l_Panning = false;
	if (m_PendingPan.x != 0.0 || m_PendingPan.y != 0.0)
	{
		const bool l_Snap = glm::length(m_PendingPan) < 0.3;
		const DVec2 l_Step = l_Snap ? m_PendingPan : m_PendingPan * (1.0 - std::exp(-PAN_RATE * std::clamp(p_DeltaSeconds, 0.0, 0.1)));
		m_Camera.panPixels(l_Step);
		m_PendingPan = l_Snap ? DVec2{ 0.0 } : m_PendingPan - l_Step;
		l_Panning = !l_Snap;
	}

	if (m_Animation == CameraAnimation::None)
		return l_Panning;
	const double l_LogZoom = std::log(m_Camera.zoom());
	const double l_LogTarget = std::log(m_TargetZoom);

	if (m_Animation == CameraAnimation::ZoomAtAnchor)
	{
		const bool l_Done = std::abs(l_LogTarget - l_LogZoom) < 1e-4;
		m_Camera.zoomAt(m_ZoomAnchor, l_Done ? m_TargetZoom : std::exp(l_LogZoom + (l_LogTarget - l_LogZoom) * l_T));
		if (l_Done)
			m_Animation = CameraAnimation::None;
		return !l_Done;
	}

	// FlyTo: move in screen space so the motion looks uniform regardless of zoom
	const DVec2 l_CenterDeltaPixels = (m_TargetCenter - m_Camera.center()) * m_Camera.pixelsPerUnit();
	const bool l_Done = std::abs(l_LogTarget - l_LogZoom) < 1e-4 && glm::length(l_CenterDeltaPixels) < 0.25;
	if (l_Done)
	{
		m_Camera.setZoom(m_TargetZoom);
		m_Camera.setCenter(m_TargetCenter);
		m_Animation = CameraAnimation::None;
		return false;
	}
	m_Camera.setZoom(std::exp(l_LogZoom + (l_LogTarget - l_LogZoom) * l_T));
	m_Camera.setCenter(m_Camera.center() + (m_TargetCenter - m_Camera.center()) * l_T);
	return true;
}
} // namespace wb
