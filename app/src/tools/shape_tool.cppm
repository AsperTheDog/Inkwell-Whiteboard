// Shape tool: drag to draw a line, arrow, rectangle, ellipse, triangle or diamond. The result is an ordinary stroke
// (a polyline), so it can be erased, selected, recoloured and saved like any other ink.
//   Shift: equal sides (square, circle) or lines in steps of 45 degrees. Alt: the press is the centre.
module;
#include <cstdint>
#include <span>
#include <vector>

export module wb.tools.shape;

import wb.math;
import wb.doc.object;
import wb.brush.shapes;
import wb.platform.input;
import wb.tools.tool;

export namespace wb::tools
{
class ShapeTool final : public Tool
{
public:
	[[nodiscard]] ToolKind kind() const override { return ToolKind::Shape; }
	void onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context) override;
	void cancel(ToolContext& p_Context) override;
	[[nodiscard]] bool isBusy() const override { return m_Active; }
	[[nodiscard]] CursorKind cursor() const override { return CursorKind::Crosshair; }

	// The shape as it would be drawn if the pointer were released now (relative to origin())
	[[nodiscard]] DVec2 origin() const { return m_Origin; }
	[[nodiscard]] Color color() const { return m_Color; }
	[[nodiscard]] std::span<const StrokePoint> preview() const { return m_Preview; }

private:
	void rebuild(const platform::PointerEvent& p_Event);
	void commit(ToolContext& p_Context);

	bool m_Active = false;
	platform::PointerDevice m_Device = platform::PointerDevice::Mouse;
	ShapeKind m_Kind = ShapeKind::Rectangle;
	DVec2 m_Start{ 0.0 };
	DVec2 m_Current{ 0.0 };
	float m_Radius = 1.f;
	Color m_Color{};
	DVec2 m_Origin{ 0.0 };
	std::vector<StrokePoint> m_Preview;
	double m_PixelsPerUnit = 1.0;
};
} // namespace wb::tools
