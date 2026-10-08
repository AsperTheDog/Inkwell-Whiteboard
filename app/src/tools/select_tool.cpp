module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.tools.select;

import wb.math;
import wb.doc.commands;
import wb.doc.document;
import wb.doc.edit;
import wb.doc.gizmo;
import wb.doc.hit;
import wb.doc.object;
import wb.doc.selection;
import wb.platform.input;
import wb.text.system;
import wb.view.camera;
import wb.tools.tool;

namespace wb::tools
{
namespace
{
// Sizes are in logical points (multiplied by the display's pixel scale when used)
constexpr double FRAME_PADDING = 5.0;          // gap between the selection and its box
constexpr double ROTATE_HANDLE_OFFSET = 28.0;  // how far the rotate handle sits above the box
constexpr double EDGE_HANDLE_MIN_SIDE = 40.0;  // edge handles are hidden on boxes smaller than this
constexpr double LASSO_SPACING = 3.0;
constexpr size_t MAX_LASSO_VERTICES = 300;
constexpr size_t MAX_OBJECT_BOXES = 300;
constexpr double ROTATE_STEP = PI / 12.0;      // 15 degrees, with Shift
constexpr double ROTATE_MAGNET = PI / 120.0;   // 1.5 degrees: quarter turns attract
constexpr double RAD_TO_DEG = 180.0 / PI;

double perDevice(const platform::PointerDevice p_Device, const double p_Mouse, const double p_Pen, const double p_Touch)
{
	switch (p_Device)
	{
	case platform::PointerDevice::Pen:
		return p_Pen;
	case platform::PointerDevice::Touch:
		return p_Touch;
	default:
		return p_Mouse;
	}
}

double handleRadius(const platform::PointerDevice p_Device) { return perDevice(p_Device, 9.0, 13.0, 22.0); }
double pickTolerance(const platform::PointerDevice p_Device) { return perDevice(p_Device, 4.0, 7.0, 12.0); }
double dragThreshold(const platform::PointerDevice p_Device) { return perDevice(p_Device, 3.0, 4.0, 8.0); }

bool isCorner(const Handle p_Handle)
{
	return p_Handle == Handle::NE || p_Handle == Handle::SE || p_Handle == Handle::SW || p_Handle == Handle::NW;
}

const char* gestureName(const Handle p_Handle)
{
	return p_Handle == Handle::Rotate ? "Rotate" : "Scale";
}
} // namespace

// ------------------------------------------------------------------------------------------------ geometry helpers

double SelectTool::paddingWorld(const ToolContext& p_Context) const
{
	return FRAME_PADDING * p_Context.camera.pixelScale() / p_Context.camera.pixelsPerUnit();
}

SelectTool::TextInfo SelectTool::textInfo(const ToolContext& p_Context) const
{
	TextInfo l_Info;
	for (const ObjectId l_Id : p_Context.selection.ids())
	{
		const Object* l_Object = p_Context.document.find(l_Id);
		if (l_Object == nullptr || l_Object->text() == nullptr)
			continue;
		l_Info.any = true;
		l_Info.id = l_Id;
	}
	l_Info.single = l_Info.any && p_Context.selection.size() == 1 && p_Context.document.find(l_Info.id)->transform.isInvertible();
	return l_Info;
}

bool SelectTool::edgeHandleShown(const Handle p_Handle, const ToolContext& p_Context) const
{
	if (isCorner(p_Handle))
		return true;
	if (const TextInfo l_Text = textInfo(p_Context); l_Text.any && (!l_Text.single || p_Handle == Handle::N || p_Handle == Handle::S))
		return false;
	const double l_Side = (p_Handle == Handle::N || p_Handle == Handle::S ? m_Frame.half.x : m_Frame.half.y) * 2.0 * p_Context.camera.pixelsPerUnit();
	return l_Side >= EDGE_HANDLE_MIN_SIDE * p_Context.camera.pixelScale();
}

bool SelectTool::insideFrame(const DVec2 p_World, const ToolContext& p_Context) const
{
	if (!m_HasFrame)
		return false;
	const DVec2 l_Local = m_Frame.toLocal(p_World);
	const DVec2 l_Limit = m_Frame.half + DVec2{ paddingWorld(p_Context) };
	return std::abs(l_Local.x) <= l_Limit.x && std::abs(l_Local.y) <= l_Limit.y;
}

Handle SelectTool::hitHandle(const DVec2 p_Screen, const platform::PointerDevice p_Device, const ToolContext& p_Context) const
{
	if (!m_HasFrame)
		return Handle::None;
	const Camera& l_Camera = p_Context.camera;
	const double l_Padding = paddingWorld(p_Context);
	const double l_Radius = handleRadius(p_Device) * l_Camera.pixelScale();

	Handle l_Best = Handle::None;
	double l_BestDistance = l_Radius;
	const auto l_Consider = [&](const Handle p_Handle, const DVec2 p_World)
	{
		const double l_Distance = glm::length(l_Camera.worldToScreen(p_World) - p_Screen);
		if (l_Distance <= l_BestDistance)
		{
			l_BestDistance = l_Distance;
			l_Best = p_Handle;
		}
	};

	for (int i = 0; i < SCALE_HANDLE_COUNT; ++i)
	{
		const Handle l_Handle = scaleHandle(i);
		if (edgeHandleShown(l_Handle, p_Context))
			l_Consider(l_Handle, m_Frame.handlePosition(l_Handle, l_Padding));
	}
	const double l_Offset = ROTATE_HANDLE_OFFSET * l_Camera.pixelScale() / l_Camera.pixelsPerUnit();
	l_Consider(Handle::Rotate, m_Frame.toWorld({ 0.0, -(m_Frame.half.y + l_Padding) - l_Offset }));
	return l_Best;
}

CursorKind SelectTool::cursorForHandle(const Handle p_Handle) const
{
	if (p_Handle == Handle::Rotate)
		return CursorKind::Rotate;
	if (!isScaleHandle(p_Handle))
		return CursorKind::Default;
	// Direction of the handle on screen, folded onto the four resize cursors
	const DVec2 l_Direction = Affine2::rotate(m_Frame.angle).applyVector(handleDirection(p_Handle));
	double l_Degrees = std::atan2(l_Direction.y, l_Direction.x) * RAD_TO_DEG;
	if (l_Degrees < 0.0)
		l_Degrees += 180.0;
	switch (static_cast<int>(std::lround(l_Degrees / 45.0)) % 4)
	{
	case 1:
		return CursorKind::ResizeNWSE;
	case 2:
		return CursorKind::ResizeNS;
	case 3:
		return CursorKind::ResizeNESW;
	default:
		return CursorKind::ResizeEW;
	}
}

// ------------------------------------------------------------------------------------------------ frame

// The box of one object in the object's own axes
SelectionFrame SelectTool::orientedFrame(const Object& p_Object)
{
	const Rect l_Local = p_Object.localBounds();
	const DMat2& l_Linear = p_Object.transform.linear;
	return SelectionFrame{
		.center = p_Object.transform.apply(l_Local.center()),
		.angle = std::atan2(l_Linear[0].y, l_Linear[0].x),
		.half = l_Local.size() * 0.5 * DVec2{ glm::length(l_Linear[0]), glm::length(l_Linear[1]) },
	};
}

void SelectTool::ensureFrame(ToolContext& p_Context)
{
	const Selection& l_Selection = p_Context.selection;
	if (l_Selection.empty())
	{
		m_HasFrame = false;
		m_FrameSelectionRevision = l_Selection.revision();
		return;
	}
	const bool l_Transforming = m_Gesture == Gesture::Move || m_Gesture == Gesture::Resize || m_Gesture == Gesture::Rotate;
	const bool l_Stale = !m_HasFrame || m_FrameSelectionRevision != l_Selection.revision() || m_FrameDocumentRevision != p_Context.document.revision() || m_FrameSpace != p_Context.select.space;
	if (l_Transforming || !l_Stale)
		return;

	const Rect l_Bounds = l_Selection.bounds();
	m_HasFrame = !l_Bounds.isEmpty();
	m_FrameSpace = p_Context.select.space;
	if (m_HasFrame)
	{
		// One object: its box follows its own axes (always for text and pictures, which are rectangles; for strokes
		// when the local space is chosen). Otherwise the box surrounds everything upright.
		const Object* l_Single = l_Selection.size() == 1 ? p_Context.document.find(l_Selection.ids().front()) : nullptr;
		const bool l_Oriented = l_Single != nullptr && l_Single->transform.isInvertible() && (l_Single->text() != nullptr || l_Single->image() != nullptr || p_Context.select.space == TransformSpace::Local);
		m_Frame = l_Oriented ? orientedFrame(*l_Single) : SelectionFrame::fromBounds(l_Bounds);
	}
	m_FrameSelectionRevision = l_Selection.revision();
	m_FrameDocumentRevision = p_Context.document.revision();
}

void SelectTool::noteTransformed(const Affine2& p_World, const uint64_t p_RevisionBefore, ToolContext& p_Context)
{
	const bool l_InSync = m_HasFrame && m_Gesture == Gesture::None && m_FrameDocumentRevision == p_RevisionBefore && m_FrameSelectionRevision == p_Context.selection.revision();
	if (!l_InSync)
		return; // the box is rebuilt from the objects on the next use
	m_Frame.center = p_World.apply(m_Frame.center);
	m_Frame.half = m_Frame.half * DVec2{ glm::length(p_World.linear[0]), glm::length(p_World.linear[1]) };
	m_Frame.angle = wrapAngle(m_Frame.angle + std::atan2(p_World.linear[0].y, p_World.linear[0].x));
	m_FrameDocumentRevision = p_Context.document.revision();
}

SelectionOverlay SelectTool::overlay(ToolContext& p_Context, const bool p_Interactive)
{
	SelectionOverlay l_Overlay;
	l_Overlay.interactive = p_Interactive;
	ensureFrame(p_Context);
	const Camera& l_Camera = p_Context.camera;

	if (m_HasFrame)
	{
		l_Overlay.visible = true;
		const double l_Padding = paddingWorld(p_Context);
		const DVec2 l_Half = m_Frame.half + DVec2{ l_Padding };
		const auto l_ToScreen = [&](const DVec2 p_Local) { return Vec2{ l_Camera.worldToScreen(m_Frame.toWorld(p_Local)) }; };

		l_Overlay.corners = { l_ToScreen({ -l_Half.x, -l_Half.y }), l_ToScreen({ l_Half.x, -l_Half.y }), l_ToScreen({ l_Half.x, l_Half.y }), l_ToScreen({ -l_Half.x, l_Half.y }) };
		for (int i = 0; i < SCALE_HANDLE_COUNT; ++i)
		{
			const Handle l_Handle = scaleHandle(i);
			l_Overlay.handles[static_cast<size_t>(i)] = l_ToScreen(handleDirection(l_Handle) * l_Half);
			l_Overlay.handleShown[static_cast<size_t>(i)] = edgeHandleShown(l_Handle, p_Context);
		}
		const double l_Offset = ROTATE_HANDLE_OFFSET * l_Camera.pixelScale() / l_Camera.pixelsPerUnit();
		l_Overlay.rotateStem = l_ToScreen({ 0.0, -l_Half.y });
		l_Overlay.rotateHandle = l_ToScreen({ 0.0, -l_Half.y - l_Offset });
		l_Overlay.hovered = m_Hover;
		l_Overlay.active = (m_Gesture == Gesture::Resize || m_Gesture == Gesture::Rotate) ? m_ActiveHandle : Handle::None;

		l_Overlay.boundsMin = l_Overlay.rotateHandle;
		l_Overlay.boundsMax = l_Overlay.rotateHandle;
		for (const Vec2 l_Corner : l_Overlay.corners)
		{
			l_Overlay.boundsMin = glm::min(l_Overlay.boundsMin, l_Corner);
			l_Overlay.boundsMax = glm::max(l_Overlay.boundsMax, l_Corner);
		}

		if (p_Context.selection.size() > 1 && p_Context.selection.size() <= MAX_OBJECT_BOXES)
		{
			for (const ObjectId l_Id : p_Context.selection.ids())
			{
				if (const Object* l_Object = p_Context.document.find(l_Id))
				{
					const Rect& l_Bounds = l_Object->worldBounds();
					l_Overlay.objectBoxes.emplace_back(Vec2{ l_Camera.worldToScreen(l_Bounds.min) }, Vec2{ l_Camera.worldToScreen(l_Bounds.max) });
				}
			}
		}
	}

	if (m_Gesture == Gesture::Marquee && m_Dragged)
	{
		l_Overlay.marquee = true;
		const Rect l_Area = Rect::fromPoints(l_Camera.worldToScreen(m_DownWorld), l_Camera.worldToScreen(m_CurrentWorld));
		l_Overlay.marqueeMin = Vec2{ l_Area.min };
		l_Overlay.marqueeMax = Vec2{ l_Area.max };
	}
	if (m_Gesture == Gesture::Lasso && m_Dragged)
	{
		l_Overlay.lasso.reserve(m_Lasso.size());
		for (const DVec2 l_Point : m_Lasso)
			l_Overlay.lasso.push_back(Vec2{ l_Camera.worldToScreen(l_Point) });
	}
	return l_Overlay;
}

// ------------------------------------------------------------------------------------------------ input

void SelectTool::onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	switch (p_Event.phase)
	{
	case platform::PointerPhase::Down:
		if (p_Event.button == platform::PointerButton::Primary && m_Gesture == Gesture::None)
			begin(p_Event, p_Context);
		return;

	case platform::PointerPhase::Move:
		if (m_Gesture != Gesture::None)
		{
			if (p_Event.device == m_Device)
				drag(p_Event, p_Context);
		}
		else if (p_Event.buttons == 0)
		{
			updateHover(p_Event, p_Context);
		}
		return;

	case platform::PointerPhase::Up:
		if (m_Gesture != Gesture::None && p_Event.device == m_Device && p_Event.button == platform::PointerButton::Primary)
		{
			drag(p_Event, p_Context);
			finish(p_Context);
			updateHover(p_Event, p_Context);
		}
		return;

	case platform::PointerPhase::Cancel:
		if (m_Gesture != Gesture::None && p_Event.device == m_Device)
			finish(p_Context);
		return;
	}
}

void SelectTool::cancel(ToolContext& p_Context)
{
	if (m_Gesture == Gesture::None)
		return;
	restore(p_Context);
}

void SelectTool::updateHover(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	ensureFrame(p_Context);
	m_Hover = hitHandle(DVec2{ p_Event.position }, p_Event.device, p_Context);
	if (m_Hover != Handle::None)
		m_Cursor = cursorForHandle(m_Hover);
	else if (insideFrame(p_Context.camera.screenToWorld(DVec2{ p_Event.position }), p_Context))
		m_Cursor = CursorKind::Move;
	else
		m_Cursor = CursorKind::Default;
}

void SelectTool::startTransform(const Gesture p_Gesture, ToolContext& p_Context)
{
	ensureFrame(p_Context);
	if (!m_HasFrame)
	{
		m_Gesture = Gesture::None;
		return;
	}
	m_Gesture = p_Gesture;
	m_BaseFrame = m_Frame;
	m_Bases.clear();
	for (const ObjectId l_Id : p_Context.selection.ids())
	{
		if (const Object* l_Object = p_Context.document.find(l_Id))
			m_Bases.push_back(Base{ .id = l_Id, .transform = l_Object->transform });
	}
	if (p_Gesture == Gesture::Rotate)
	{
		const DVec2 l_Arm = m_DownWorld - m_Frame.center;
		m_RotateStart = std::atan2(l_Arm.y, l_Arm.x);
	}
	if (p_Gesture == Gesture::Resize)
		m_GrabOffset = m_DownWorld - m_Frame.handlePosition(m_ActiveHandle);
	m_TextResize = false;
	if (p_Gesture == Gesture::Resize && (m_ActiveHandle == Handle::E || m_ActiveHandle == Handle::W))
	{
		if (const TextInfo l_Text = textInfo(p_Context); l_Text.single)
		{
			const Object* l_Object = p_Context.document.find(l_Text.id);
			m_TextResize = true;
			m_TextResizeId = l_Text.id;
			m_TextBase = *l_Object->text();
			m_TextBaseTransform = l_Object->transform;
		}
	}
	m_Dragged = p_Gesture != Gesture::Move; // resizing and rotating start at once; moving waits for the drag threshold
}

void SelectTool::begin(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	Selection& l_Selection = p_Context.selection;
	m_Device = p_Event.device;
	m_DownScreen = DVec2{ p_Event.position };
	m_DownWorld = p_Context.camera.screenToWorld(m_DownScreen);
	m_CurrentWorld = m_DownWorld;
	m_Dragged = false;
	m_Additive = (p_Event.modifiers & platform::Modifier::Shift) != 0;
	m_ClickReduce = INVALID_OBJECT_ID;
	m_ClickToggle = INVALID_OBJECT_ID;
	m_ActiveHandle = Handle::None;
	ensureFrame(p_Context);

	if (!l_Selection.empty())
	{
		const Handle l_Handle = hitHandle(m_DownScreen, p_Event.device, p_Context);
		if (l_Handle != Handle::None)
		{
			m_ActiveHandle = l_Handle;
			startTransform(l_Handle == Handle::Rotate ? Gesture::Rotate : Gesture::Resize, p_Context);
			m_Cursor = cursorForHandle(l_Handle);
			return;
		}
	}

	const double l_Tolerance = pickTolerance(p_Event.device) * p_Context.camera.pixelScale() / p_Context.camera.pixelsPerUnit();
	const ObjectId l_Picked = pickTopmost(p_Context.document, m_DownWorld, l_Tolerance);
	if (l_Picked != INVALID_OBJECT_ID)
	{
		if (m_Additive)
		{
			if (l_Selection.contains(l_Picked))
				m_ClickToggle = l_Picked;
			else
				l_Selection.add(l_Picked);
		}
		else if (!l_Selection.contains(l_Picked))
		{
			const std::array<ObjectId, 1> l_Only{ l_Picked };
			l_Selection.set(l_Only);
		}
		else if (l_Selection.size() > 1)
		{
			m_ClickReduce = l_Picked;
		}
		startTransform(Gesture::Move, p_Context);
		m_Cursor = CursorKind::Move;
		return;
	}

	// Inside the box of the current selection: grab it all (Shift starts a box selection instead)
	if (!m_Additive && !l_Selection.empty() && insideFrame(m_DownWorld, p_Context))
	{
		startTransform(Gesture::Move, p_Context);
		m_Cursor = CursorKind::Move;
		return;
	}

	// Empty space: box or lasso. Without Shift the old selection goes away immediately.
	m_KeptSelection.clear();
	if (m_Additive)
		m_KeptSelection.assign(l_Selection.ids().begin(), l_Selection.ids().end());
	else
		l_Selection.clear();
	m_Gesture = p_Context.select.mode == SelectMode::Lasso ? Gesture::Lasso : Gesture::Marquee;
	m_Lasso.clear();
	m_Lasso.push_back(m_DownWorld);
	m_Cursor = CursorKind::Crosshair;
}

// Dragging the east or west handle of a text box: the opposite edge stays, the lines wrap at the new width
void SelectTool::applyTextResize(ToolContext& p_Context)
{
	// Measured along the box's own x axis, which is the text's own axis (turned and possibly mirrored)
	const bool l_East = m_ActiveHandle == Handle::E;
	const double l_Scale = glm::length(m_TextBaseTransform.linear[0]);
	const DVec2 l_Grab = m_BaseFrame.toLocal(m_CurrentWorld - m_GrabOffset);
	const double l_WidthWorld = l_East ? l_Grab.x + m_BaseFrame.half.x : m_BaseFrame.half.x - l_Grab.x;
	const float l_Wrap = std::max(static_cast<float>(l_WidthWorld / std::max(l_Scale, 1e-12)), m_TextBase.fontSize * 1.5f);
	// The edge opposite to the dragged handle stays: the text's left edge, unless the text is mirrored
	const bool l_Mirrored = glm::dot(m_TextBaseTransform.linear[0], Affine2::rotate(m_BaseFrame.angle).applyVector(DVec2{ 1.0, 0.0 })) < 0.0;
	const bool l_KeepLeft = l_East != l_Mirrored;

	TextData l_Data = m_TextBase;
	l_Data.wrapWidth = l_Wrap;
	l_Data.size = p_Context.text.measure(l_Data);
	const Affine2 l_Transform = anchoredTextTransform(m_TextBaseTransform, m_TextBase.size, l_Data.size, l_KeepLeft ? TextAlign::Left : TextAlign::Right);
	p_Context.document.modify(m_TextResizeId, [&](Object& p_Object)
	{
		*p_Object.text() = l_Data;
		p_Object.transform = l_Transform;
	});
	if (const Object* l_Object = p_Context.document.find(m_TextResizeId))
		m_Frame = orientedFrame(*l_Object);
}

void SelectTool::applyTransform(const FrameTransform& p_Result, ToolContext& p_Context)
{
	for (const Base& l_Base : m_Bases)
		p_Context.document.modify(l_Base.id, [&](Object& p_Object) { p_Object.transform = p_Result.transform * l_Base.transform; }, ObjectChange::Transform);
	m_Frame = p_Result.frame;
}

void SelectTool::drag(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	const DVec2 l_Screen{ p_Event.position };
	m_CurrentWorld = p_Context.camera.screenToWorld(l_Screen);

	if (!m_Dragged)
	{
		if (glm::length(l_Screen - m_DownScreen) < dragThreshold(m_Device) * p_Context.camera.pixelScale())
			return;
		m_Dragged = true;
	}

	const bool l_Shift = (p_Event.modifiers & platform::Modifier::Shift) != 0;
	const bool l_Alt = (p_Event.modifiers & platform::Modifier::Alt) != 0;
	switch (m_Gesture)
	{
	case Gesture::Move:
		applyTransform(translateFrame(m_BaseFrame, m_CurrentWorld - m_DownWorld), p_Context);
		break;

	case Gesture::Resize:
		if (m_TextResize)
			applyTextResize(p_Context);
		else
			applyTransform(resizeFrame(m_BaseFrame, m_ActiveHandle, m_CurrentWorld - m_GrabOffset, ResizeOptions{ .keepAspect = l_Shift || (isCorner(m_ActiveHandle) && textInfo(p_Context).any), .fromCenter = l_Alt }), p_Context);
		break;

	case Gesture::Rotate:
	{
		const DVec2 l_Arm = m_CurrentWorld - m_BaseFrame.center;
		double l_Total = m_BaseFrame.angle + wrapAngle(std::atan2(l_Arm.y, l_Arm.x) - m_RotateStart);
		if (l_Shift)
		{
			l_Total = snapAngle(l_Total, ROTATE_STEP);
		}
		else
		{
			const double l_Quarter = snapAngle(l_Total, PI * 0.5);
			if (std::abs(l_Total - l_Quarter) < ROTATE_MAGNET)
				l_Total = l_Quarter;
		}
		applyTransform(rotateFrame(m_BaseFrame, l_Total - m_BaseFrame.angle), p_Context);
		break;
	}

	case Gesture::Lasso:
	{
		const double l_Spacing = LASSO_SPACING * p_Context.camera.pixelScale() / p_Context.camera.pixelsPerUnit();
		if (glm::length(m_CurrentWorld - m_Lasso.back()) >= l_Spacing)
			m_Lasso.push_back(m_CurrentWorld);
		break;
	}

	default:
		break;
	}
}

void SelectTool::finish(ToolContext& p_Context)
{
	Selection& l_Selection = p_Context.selection;
	const Gesture l_Gesture = m_Gesture;
	const Handle l_Handle = m_ActiveHandle;
	m_Gesture = Gesture::None;
	m_ActiveHandle = Handle::None;

	const auto l_Combine = [&](std::vector<ObjectId> p_Hits)
	{
		if (m_Additive)
		{
			std::unordered_set<ObjectId> l_Known(m_KeptSelection.begin(), m_KeptSelection.end());
			for (const ObjectId l_Id : p_Hits)
			{
				if (l_Known.insert(l_Id).second)
					m_KeptSelection.push_back(l_Id);
			}
			p_Hits = m_KeptSelection;
		}
		l_Selection.set(p_Hits);
	};

	switch (l_Gesture)
	{
	case Gesture::Move:
	case Gesture::Resize:
	case Gesture::Rotate:
	{
		if (m_Dragged && m_TextResize)
		{
			const Object* l_Object = p_Context.document.find(m_TextResizeId);
			if (l_Object != nullptr && l_Object->text() != nullptr && !(*l_Object->text() == m_TextBase))
			{
				std::vector<SetTextCommand::Entry> l_Entries;
				l_Entries.push_back(SetTextCommand::Entry{ .id = m_TextResizeId, .before = m_TextBase, .after = *l_Object->text(), .transformBefore = m_TextBaseTransform, .transformAfter = l_Object->transform });
				p_Context.history.push(std::make_unique<SetTextCommand>(std::move(l_Entries), "Resize text"));
			}
			m_TextResize = false;
			m_FrameDocumentRevision = p_Context.document.revision();
			m_Bases.clear();
		}
		else if (m_Dragged)
		{
			std::vector<TransformObjectsCommand::Entry> l_Entries;
			l_Entries.reserve(m_Bases.size());
			for (const Base& l_Base : m_Bases)
			{
				if (const Object* l_Object = p_Context.document.find(l_Base.id))
					l_Entries.push_back(TransformObjectsCommand::Entry{ .id = l_Base.id, .before = l_Base.transform, .after = l_Object->transform });
			}
			auto l_Command = std::make_unique<TransformObjectsCommand>(std::move(l_Entries), l_Gesture == Gesture::Move ? "Move" : gestureName(l_Handle));
			if (l_Command->changesAnything())
				p_Context.history.push(std::move(l_Command));
			m_FrameDocumentRevision = p_Context.document.revision();
		}
		else
		{
			// A click: Move began without moving, so nothing changed in the document
			if (m_ClickReduce != INVALID_OBJECT_ID)
			{
				const std::array<ObjectId, 1> l_Only{ m_ClickReduce };
				l_Selection.set(l_Only);
			}
			if (m_ClickToggle != INVALID_OBJECT_ID)
				l_Selection.remove(m_ClickToggle);
		}
		m_Bases.clear();
		break;
	}

	case Gesture::Marquee:
		if (m_Dragged)
			l_Combine(objectsInRect(p_Context.document, Rect::fromPoints(m_DownWorld, m_CurrentWorld)));
		else if (m_Additive)
			l_Selection.set(m_KeptSelection);
		break;

	case Gesture::Lasso:
		if (m_Dragged && m_Lasso.size() >= 3)
		{
			if (m_Lasso.size() > MAX_LASSO_VERTICES)
			{
				// Evenly thinned copy keeps the cost of the overlap tests bounded
				std::vector<DVec2> l_Thinned;
				const double l_Step = static_cast<double>(m_Lasso.size()) / static_cast<double>(MAX_LASSO_VERTICES);
				for (size_t i = 0; i < MAX_LASSO_VERTICES; ++i)
					l_Thinned.push_back(m_Lasso[static_cast<size_t>(static_cast<double>(i) * l_Step)]);
				m_Lasso = std::move(l_Thinned);
			}
			l_Combine(objectsInPolygon(p_Context.document, m_Lasso));
		}
		else if (m_Additive)
		{
			l_Selection.set(m_KeptSelection);
		}
		m_Lasso.clear();
		break;

	case Gesture::None:
		break;
	}
	m_Dragged = false;
	m_Cursor = CursorKind::Default;
}

// Puts the objects back where the gesture found them
void SelectTool::restore(ToolContext& p_Context)
{
	const Gesture l_Gesture = m_Gesture;
	m_Gesture = Gesture::None;
	m_ActiveHandle = Handle::None;
	if (l_Gesture == Gesture::Move || l_Gesture == Gesture::Resize || l_Gesture == Gesture::Rotate)
	{
		if (m_TextResize)
		{
			p_Context.document.modify(m_TextResizeId, [&](Object& p_Object)
			{
				if (p_Object.text() != nullptr)
					*p_Object.text() = m_TextBase;
				p_Object.transform = m_TextBaseTransform;
			});
			m_TextResize = false;
		}
		for (const Base& l_Base : m_Bases)
			p_Context.document.modify(l_Base.id, [&](Object& p_Object) { p_Object.transform = l_Base.transform; }, ObjectChange::Transform);
		m_Frame = m_BaseFrame;
		m_FrameDocumentRevision = p_Context.document.revision();
		m_Bases.clear();
	}
	m_Lasso.clear();
	m_Dragged = false;
	m_Cursor = CursorKind::Default;
}
} // namespace wb::tools
