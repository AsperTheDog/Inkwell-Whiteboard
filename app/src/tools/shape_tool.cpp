module;
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.tools.shape;

import wb.math;
import wb.doc.object;
import wb.doc.commands;
import wb.brush.shapes;
import wb.platform.input;
import wb.tools.tool;

namespace wb::tools
{
void ShapeTool::onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	switch (p_Event.phase)
	{
	case platform::PointerPhase::Down:
		if (p_Event.button != platform::PointerButton::Primary || m_Active)
			return;
		m_Active = true;
		m_Device = p_Event.device;
		m_Kind = p_Context.shape.kind;
		m_Start = p_Context.camera.screenToWorld(DVec2{ p_Event.position });
		m_Current = m_Start;
		m_Color = p_Context.brush.color;
		m_PixelsPerUnit = p_Context.camera.pixelsPerUnit();
		m_Radius = p_Context.brush.effectivePoints(p_Context.camera.zoom()) / static_cast<float>(p_Context.camera.zoom()) * 0.5f;
		m_Preview.clear();
		return;

	case platform::PointerPhase::Move:
		if (m_Active && p_Event.device == m_Device)
		{
			m_Current = p_Context.camera.screenToWorld(DVec2{ p_Event.position });
			rebuild(p_Event);
		}
		return;

	case platform::PointerPhase::Up:
		if (m_Active && p_Event.device == m_Device && p_Event.button == platform::PointerButton::Primary)
		{
			m_Current = p_Context.camera.screenToWorld(DVec2{ p_Event.position });
			rebuild(p_Event);
			commit(p_Context);
		}
		return;

	case platform::PointerPhase::Cancel:
		if (m_Active && p_Event.device == m_Device)
		{
			m_Active = false;
			m_Preview.clear();
		}
		return;
	}
}

void ShapeTool::rebuild(const platform::PointerEvent& p_Event)
{
	const ShapeOptions l_Options{
		.constrain = (p_Event.modifiers & platform::Modifier::Shift) != 0,
		.fromCenter = (p_Event.modifiers & platform::Modifier::Alt) != 0,
		.strokeWidth = static_cast<double>(m_Radius) * 2.0,
	};
	const std::vector<DVec2> l_Outline = shapeOutline(m_Kind, m_Start, m_Current, l_Options);
	m_Preview.clear();
	if (l_Outline.empty())
		return;
	m_Origin = l_Outline.front();
	m_Preview.reserve(l_Outline.size());
	for (const DVec2 l_Point : l_Outline)
		m_Preview.push_back(StrokePoint{ .position = Vec2{ l_Point - m_Origin }, .radius = m_Radius });
}

void ShapeTool::commit(ToolContext& p_Context)
{
	m_Active = false;
	if (m_Preview.empty())
		return;
	StrokeData l_Stroke;
	l_Stroke.style = StrokeStyle{ .color = m_Color, .size = m_Radius * 2.f, .brush = BrushKind::Pen };
	l_Stroke.points = std::move(m_Preview);
	m_Preview.clear();

	auto l_Object = std::make_unique<Object>();
	l_Object->id = p_Context.document.allocateId();
	l_Object->transform = Affine2::translate(m_Origin);
	l_Object->payload = std::move(l_Stroke);
	const ObjectId l_Id = l_Object->id;

	std::vector<std::unique_ptr<Object>> l_Objects;
	l_Objects.push_back(std::move(l_Object));
	p_Context.history.execute(p_Context.document, std::make_unique<AddObjectsCommand>(std::move(l_Objects), "Draw shape"));
	// Leave the new shape selected: drawn shapes are often adjusted right away
	const std::vector<ObjectId> l_Selected{ l_Id };
	p_Context.selection.set(l_Selected);
}

void ShapeTool::cancel(ToolContext&)
{
	m_Active = false;
	m_Preview.clear();
}
} // namespace wb::tools
