module;
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
} // namespace

void PenTool::onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	switch (p_Event.phase)
	{
	case platform::PointerPhase::Down:
		if (p_Event.button != platform::PointerButton::Primary || m_Builder.isActive())
			return;
		m_Device = p_Event.device;
		m_Builder.begin(toStrokeInput(p_Event), p_Event.device == platform::PointerDevice::Pen, p_Context.brush.effectivePoints(p_Context.camera.zoom()), p_Context.brush.color, p_Context.camera, p_Context.brushSettings);
		return;

	case platform::PointerPhase::Move:
		if (m_Builder.isActive() && p_Event.device == m_Device)
			m_Builder.add(toStrokeInput(p_Event));
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
	const StrokeInput l_Last = p_Last != nullptr ? toStrokeInput(*p_Last) : StrokeInput{};
	const DVec2 l_Origin = m_Builder.origin();
	StrokeData l_Stroke = m_Builder.finish(p_Last != nullptr ? &l_Last : nullptr);
	if (l_Stroke.points.empty())
		return;

	auto l_Object = std::make_unique<Object>();
	l_Object->id = p_Context.document.allocateId();
	l_Object->transform = Affine2::translate(l_Origin);
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
