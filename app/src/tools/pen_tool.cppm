// Freehand pen: feeds pointer samples to a StrokeBuilder and commits the result as an undoable command.
module;
#include <cstdint>

export module wb.tools.pen;

import wb.math;
import wb.brush.stroke_builder;
import wb.platform.input;
import wb.tools.tool;

export namespace wb::tools
{
class PenTool final : public Tool
{
public:
	[[nodiscard]] ToolKind kind() const override { return ToolKind::Pen; }
	void onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context) override;
	void cancel(ToolContext& p_Context) override;
	[[nodiscard]] bool isBusy() const override { return m_Builder.isActive(); }
	[[nodiscard]] CursorKind cursor() const override { return CursorKind::Crosshair; }

	[[nodiscard]] const StrokeBuilder& builder() const { return m_Builder; }

private:
	void commit(const platform::PointerEvent* p_Last, ToolContext& p_Context);

	StrokeBuilder m_Builder;
	platform::PointerDevice m_Device = platform::PointerDevice::Mouse;
};
} // namespace wb::tools
