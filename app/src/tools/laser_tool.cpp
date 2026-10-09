module;
#include <cstdint>
#include <deque>

module wb.tools.laser;

import wb.math;
import wb.platform.input;
import wb.tools.tool;

namespace wb::tools
{
void LaserTool::onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context)
{
	const DVec2 l_World = p_Context.camera.screenToWorld(DVec2{ p_Event.position });
	switch (p_Event.phase)
	{
	case platform::PointerPhase::Down:
		if (p_Event.button != platform::PointerButton::Primary || m_Down)
			return;
		m_Down = true;
		m_Device = p_Event.device;
		m_Trail.push_back(LaserPoint{ .world = l_World, .timeNs = p_Event.timestampNs, .newStroke = true });
		return;

	case platform::PointerPhase::Move:
		if (m_Down && p_Event.device == m_Device)
			m_Trail.push_back(LaserPoint{ .world = l_World, .timeNs = p_Event.timestampNs });
		return;

	case platform::PointerPhase::Up:
		if (m_Down && p_Event.device == m_Device && p_Event.button == platform::PointerButton::Primary)
		{
			m_Trail.push_back(LaserPoint{ .world = l_World, .timeNs = p_Event.timestampNs });
			m_Down = false;
		}
		return;

	case platform::PointerPhase::Cancel:
		if (m_Down && p_Event.device == m_Device)
			m_Down = false;
		return;
	}
}

void LaserTool::cancel(ToolContext&)
{
	m_Down = false;
}

bool LaserTool::update(const uint64_t p_NowNs)
{
	if (m_Down && !m_Trail.empty())
		m_Trail.back().timeNs = p_NowNs; // held still: the dot stays
	while (!m_Trail.empty() && p_NowNs > m_Trail.front().timeNs + lifetimeNs())
		m_Trail.pop_front();
	return !m_Trail.empty() || m_Down;
}
} // namespace wb::tools
