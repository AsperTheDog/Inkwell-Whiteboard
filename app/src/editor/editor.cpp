module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

module wb.editor;

import wb.math;
import wb.doc.document;
import wb.doc.history;
import wb.view.camera;
import wb.brush.stroke_builder;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.tools.tool;
import wb.tools.pen;
import wb.tools.eraser;

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

bool isPanButton(const platform::PointerButton p_Button)
{
	return p_Button == platform::PointerButton::Secondary || p_Button == platform::PointerButton::Middle;
}
} // namespace

Editor::Editor()
	: m_Pen(std::make_unique<tools::PenTool>()), m_Eraser(std::make_unique<tools::EraserTool>())
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
		.camera = m_Camera,
		.brush = m_Brush,
		.eraser = m_EraserState,
		.brushSettings = m_BrushSettings,
	};
}

tools::Tool* Editor::toolFor(const tools::ToolKind p_Kind)
{
	return p_Kind == tools::ToolKind::Eraser ? static_cast<tools::Tool*>(m_Eraser.get()) : static_cast<tools::Tool*>(m_Pen.get());
}

// The tool that would handle a new gesture right now: the pen's eraser end always erases
tools::Tool* Editor::toolInEffect() const
{
	if (m_ActiveTool != nullptr && m_ActiveTool->isBusy())
		return m_ActiveTool;
	return m_HoverEraser ? static_cast<tools::Tool*>(m_Eraser.get()) : (m_SelectedTool == tools::ToolKind::Eraser ? static_cast<tools::Tool*>(m_Eraser.get()) : static_cast<tools::Tool*>(m_Pen.get()));
}

void Editor::setTool(const tools::ToolKind p_Kind)
{
	if (m_ActiveTool != nullptr && m_ActiveTool->isBusy())
	{
		tools::ToolContext l_Context = toolContext();
		m_ActiveTool->cancel(l_Context);
	}
	m_SelectedTool = p_Kind;
	m_ActiveTool = toolFor(p_Kind);
}

std::optional<EraserCursor> Editor::eraserCursor() const
{
	if (m_Panning || m_SpaceHeld || toolInEffect() != m_Eraser.get())
		return std::nullopt;
	return EraserCursor{ .center = m_LastPointer, .radiusPixels = m_EraserState.sizePoints * 0.5f * static_cast<float>(m_Camera.pixelScale()) };
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

void Editor::handlePointer(const platform::PointerEvent& p_Event)
{
	m_LastPointer = p_Event.position;
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
	if (p_Event.phase == platform::PointerPhase::Down && !l_ToolBusy && (isPanButton(p_Event.button) || (m_SpaceHeld && p_Event.button == platform::PointerButton::Primary)))
	{
		m_Panning = true;
		m_PanDevice = p_Event.device;
		m_PanButton = p_Event.button;
		m_PanLast = p_Event.position;
		return;
	}

	if (m_ActiveTool != nullptr)
	{
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

	if (p_Event.key == SDLK_SPACE)
	{
		m_SpaceHeld = true;
		return true;
	}
	if (l_Ctrl && p_Event.key == SDLK_Z)
	{
		l_Shift ? redo() : undo();
		return true;
	}
	if (l_Ctrl && p_Event.key == SDLK_Y)
	{
		redo();
		return true;
	}
	if (l_Ctrl && (p_Event.key == SDLK_0 || p_Event.key == SDLK_KP_0))
	{
		resetZoom();
		return true;
	}
	if (l_Ctrl && (p_Event.key == SDLK_EQUALS || p_Event.key == SDLK_PLUS || p_Event.key == SDLK_KP_PLUS))
	{
		zoomAroundCenter(KEY_ZOOM_STEP);
		return true;
	}
	if (l_Ctrl && (p_Event.key == SDLK_MINUS || p_Event.key == SDLK_KP_MINUS))
	{
		zoomAroundCenter(1.0 / KEY_ZOOM_STEP);
		return true;
	}
	if (!l_Ctrl && p_Event.key == SDLK_E)
	{
		setTool(tools::ToolKind::Eraser);
		return true;
	}
	if (!l_Ctrl && p_Event.key == SDLK_P)
	{
		setTool(tools::ToolKind::Pen);
		return true;
	}
	if (p_Event.key == SDLK_HOME)
	{
		fitContent();
		return true;
	}
	return false;
}

void Editor::handleKeyUp(const SDL_KeyboardEvent& p_Event)
{
	if (p_Event.key == SDLK_SPACE)
		m_SpaceHeld = false;
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
}

// ------------------------------------------------------------------------------------------------ commands

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
