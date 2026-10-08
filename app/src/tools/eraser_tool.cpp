module;
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.tools.eraser;

import wb.math;
import wb.brush.eraser;
import wb.doc.commands;
import wb.doc.object;
import wb.platform.input;
import wb.tools.tool;

namespace wb::tools
{
void EraserTool::onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	const DVec2 l_World = p_Context.camera.screenToWorld(DVec2{ p_Event.position });
	switch (p_Event.phase)
	{
	case platform::PointerPhase::Down:
		if (p_Event.button != platform::PointerButton::Primary || m_Command != nullptr)
			return;
		m_Device = p_Event.device;
		m_Command = std::make_unique<ReplaceObjectsCommand>("Erase");
		m_Last = l_World;
		eraseAlong(l_World, l_World, p_Context);
		return;

	case platform::PointerPhase::Move:
		if (m_Command != nullptr && p_Event.device == m_Device)
		{
			eraseAlong(m_Last, l_World, p_Context);
			m_Last = l_World;
		}
		return;

	case platform::PointerPhase::Up:
		if (m_Command != nullptr && p_Event.device == m_Device && p_Event.button == platform::PointerButton::Primary)
		{
			eraseAlong(m_Last, l_World, p_Context);
			finish(p_Context);
		}
		return;

	case platform::PointerPhase::Cancel:
		if (m_Command != nullptr && p_Event.device == m_Device)
			finish(p_Context);
		return;
	}
}

void EraserTool::cancel(ToolContext& p_Context)
{
	if (m_Command != nullptr)
		finish(p_Context);
}

void EraserTool::finish(ToolContext& p_Context)
{
	std::unique_ptr<ReplaceObjectsCommand> l_Command = std::move(m_Command);
	if (l_Command != nullptr && !l_Command->empty())
		p_Context.history.push(std::move(l_Command));
}

void EraserTool::eraseAlong(const DVec2 p_From, const DVec2 p_To, ToolContext& p_Context)
{
	// Eraser size is in screen points, so it covers the same screen area at every zoom
	const double l_Radius = static_cast<double>(p_Context.eraser.sizePoints) * 0.5 / p_Context.camera.zoom();
	const EraserCapsule l_Capsule{ .a = p_From, .b = p_To, .radius = l_Radius };
	const Rect l_Area = l_Capsule.bounds();

	struct Hit
	{
		ObjectId id = INVALID_OBJECT_ID;
		std::vector<StrokeData> pieces;
		Affine2 transform{};
	};
	std::vector<Hit> l_Hits;

	// Collect first: replacing objects invalidates the iteration
	for (const std::unique_ptr<Object>& l_Object : p_Context.document.objects())
	{
		if (l_Object->stroke() == nullptr || !l_Object->worldBounds().intersects(l_Area))
			continue;
		if (p_Context.eraser.mode == EraserMode::Stroke)
		{
			if (strokeTouches(*l_Object, l_Capsule))
				l_Hits.push_back(Hit{ .id = l_Object->id });
		}
		else if (std::optional<std::vector<StrokeData>> l_Pieces = cutStroke(*l_Object, l_Capsule))
		{
			l_Hits.push_back(Hit{ .id = l_Object->id, .pieces = std::move(*l_Pieces), .transform = l_Object->transform });
		}
	}

	for (Hit& l_Hit : l_Hits)
	{
		std::vector<std::unique_ptr<Object>> l_Added;
		for (StrokeData& l_Piece : l_Hit.pieces)
		{
			auto l_Object = std::make_unique<Object>();
			l_Object->id = p_Context.document.allocateId();
			l_Object->transform = l_Hit.transform;
			l_Object->payload = std::move(l_Piece);
			l_Added.push_back(std::move(l_Object));
		}
		m_Command->replace(p_Context.document, l_Hit.id, std::move(l_Added));
	}
}
} // namespace wb::tools
