module;
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.tools.pen;

import wb.math;
import wb.doc.object;
import wb.doc.commands;
import wb.brush.stroke_builder;
import wb.view.ruler;
import wb.platform.input;
import wb.tools.tool;

namespace wb::tools
{
namespace
{
StrokeInput toStrokeInput(const platform::PointerEvent& p_Event)
{
	return StrokeInput{ .screen = p_Event.position, .pressure = p_Event.pressure, .timestampNs = p_Event.timestampNs };
}

constexpr double RULER_SNAP_PIXELS = 10.0;
} // namespace

StrokeInput PenTool::snapped(const platform::PointerEvent& p_Event, const ToolContext& p_Context) const
{
	StrokeInput l_Input = toStrokeInput(p_Event);
	if (m_Edge)
	{
		const DVec2 l_World = projectOntoLine(*m_Edge, p_Context.camera.screenToWorld(DVec2{ p_Event.position }));
		l_Input.screen = Vec2{ p_Context.camera.worldToScreen(l_World) };
	}
	return l_Input;
}
void PenTool::onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	switch (p_Event.phase)
	{
	case platform::PointerPhase::Down:
		if (p_Event.button != platform::PointerButton::Primary || m_Builder.isActive())
			return;
		m_Device = p_Event.device;
		{
			const bool l_Highlighter = m_BrushKind == BrushKind::Highlighter;
			const BrushState& l_Brush = l_Highlighter ? p_Context.highlighter : p_Context.brush;
			BrushSettings l_Settings = p_Context.brushSettings;
			Color l_Color = l_Brush.color;
			if (l_Highlighter)
			{
				// A marker has one width and flat ends, and is see-through
				l_Settings.pressureSensitivity = 0.f;
				l_Settings.simulatePressureForMouse = false;
				l_Settings.taperStartPx = 0.f;
				l_Settings.taperEndPx = 0.f;
				l_Color.a = HIGHLIGHTER_ALPHA;
			}
			m_Edge.reset();
			if (p_Context.ruler.visible)
			{
				const double l_Tolerance = std::min(RULER_SNAP_PIXELS * p_Context.camera.pixelScale() / p_Context.camera.pixelsPerUnit(), Ruler::THICKNESS * 0.3);
				m_Edge = p_Context.ruler.snapEdge(p_Context.camera.screenToWorld(DVec2{ p_Event.position }), l_Tolerance);
			}
			m_Builder.begin(snapped(p_Event, p_Context), p_Event.device == platform::PointerDevice::Pen, l_Brush.effectivePoints(p_Context.camera.zoom()), l_Color, p_Context.camera, l_Settings);
		}
		return;

	case platform::PointerPhase::Move:
		if (m_Builder.isActive() && p_Event.device == m_Device)
			m_Builder.add(snapped(p_Event, p_Context));
		return;

	case platform::PointerPhase::Up:
		if (m_Builder.isActive() && p_Event.device == m_Device && p_Event.button == platform::PointerButton::Primary)
			commit(&p_Event, p_Context);
		return;

	case platform::PointerPhase::Cancel:
		// The pen left the tablet's range while drawing: keep what was drawn
		if (m_Builder.isActive() && p_Event.device == m_Device)
			commit(nullptr, p_Context);
		return;
	}
}

void PenTool::commit(const platform::PointerEvent* p_Last, ToolContext& p_Context)
{
	const StrokeInput l_Last = p_Last != nullptr ? snapped(*p_Last, p_Context) : StrokeInput{};
	const DVec2 l_Origin = m_Builder.origin();
	StrokeData l_Stroke = m_Builder.finish(p_Last != nullptr ? &l_Last : nullptr);
	if (l_Stroke.points.empty())
		return;

	auto l_Object = std::make_unique<Object>();
	l_Object->id = p_Context.document.allocateId();
	l_Object->transform = Affine2::translate(l_Origin);
	l_Stroke.style.brush = m_BrushKind;
	l_Object->payload = std::move(l_Stroke);

	std::vector<std::unique_ptr<Object>> l_Objects;
	l_Objects.push_back(std::move(l_Object));
	p_Context.history.execute(p_Context.document, std::make_unique<AddObjectsCommand>(std::move(l_Objects), "Draw"));
}

void PenTool::cancel(ToolContext& p_Context)
{
	// Switching tools mid-stroke keeps the stroke, like lifting the pen
	if (m_Builder.isActive())
		commit(nullptr, p_Context);
}
} // namespace wb::tools
