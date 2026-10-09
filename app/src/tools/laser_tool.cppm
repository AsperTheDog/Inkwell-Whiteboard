// Laser pointer: while the pointer is down it leaves a glowing trail that fades away by itself. Nothing is added to the
// board, so it is for pointing things out while presenting.
module;
#include <cstdint>
#include <deque>
#include <span>

export module wb.tools.laser;

import wb.math;
import wb.platform.input;
import wb.tools.tool;

export namespace wb::tools
{
struct LaserPoint
{
	DVec2 world{ 0.0 };
	uint64_t timeNs = 0;
	bool newStroke = false; // the pointer was lifted before this point: do not join it to the previous one
};

class LaserTool final : public Tool
{
public:
	static constexpr float DEFAULT_TRAIL_SECONDS = 1.1f;
	static constexpr float MIN_TRAIL_SECONDS = 0.2f;
	static constexpr float MAX_TRAIL_SECONDS = 6.f;

	// How long a point of the trail stays visible
	void setTrailSeconds(const float p_Seconds) { m_TrailSeconds = p_Seconds < MIN_TRAIL_SECONDS ? MIN_TRAIL_SECONDS : (p_Seconds > MAX_TRAIL_SECONDS ? MAX_TRAIL_SECONDS : p_Seconds); }
	[[nodiscard]] float trailSeconds() const { return m_TrailSeconds; }
	[[nodiscard]] uint64_t lifetimeNs() const { return static_cast<uint64_t>(static_cast<double>(m_TrailSeconds) * 1e9); }

	[[nodiscard]] ToolKind kind() const override { return ToolKind::Laser; }
	void onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context) override;
	void cancel(ToolContext& p_Context) override;
	[[nodiscard]] bool isBusy() const override { return m_Down; }
	[[nodiscard]] CursorKind cursor() const override { return CursorKind::Crosshair; }

	// Drops the points that have faded out. Returns true while a trail is still visible.
	bool update(uint64_t p_NowNs);
	[[nodiscard]] const std::deque<LaserPoint>& trail() const { return m_Trail; }
	[[nodiscard]] bool pressed() const { return m_Down; }

private:
	float m_TrailSeconds = DEFAULT_TRAIL_SECONDS;
	bool m_Down = false;
	platform::PointerDevice m_Device = platform::PointerDevice::Mouse;
	bool m_StartNew = true;
	std::deque<LaserPoint> m_Trail;
};
} // namespace wb::tools
