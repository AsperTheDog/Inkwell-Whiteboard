// Freehand pen: feeds pointer samples to a StrokeBuilder and commits the result as an undoable command.
module;
#include <cstdint>
#include <optional>

export module wb.tools.pen;

import wb.math;
import wb.brush.stroke_builder;
import wb.doc.object;
import wb.view.ruler;
import wb.platform.input;
import wb.tools.tool;

export namespace wb::tools
{
class PenTool final : public Tool
{
public:
	// The same tool draws with the pen and with the highlighter
	explicit PenTool(const BrushKind p_Kind = BrushKind::Pen) : m_BrushKind(p_Kind) {}

	[[nodiscard]] ToolKind kind() const override { return m_BrushKind == BrushKind::Highlighter ? ToolKind::Highlighter : ToolKind::Pen; }
	void onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context) override;
	void cancel(ToolContext& p_Context) override;
	[[nodiscard]] bool isBusy() const override { return m_Builder.isActive(); }
	[[nodiscard]] CursorKind cursor() const override { return CursorKind::Crosshair; }

	[[nodiscard]] const StrokeBuilder& builder() const { return m_Builder; }

private:
	void commit(const platform::PointerEvent* p_Last, ToolContext& p_Context);

	// Moves a pointer position onto the ruler's edge when this stroke follows it
	[[nodiscard]] StrokeInput snapped(const platform::PointerEvent& p_Event, const ToolContext& p_Context) const;

	BrushKind m_BrushKind = BrushKind::Pen;
	StrokeBuilder m_Builder;
	platform::PointerDevice m_Device = platform::PointerDevice::Mouse;
	std::optional<RulerLine> m_Edge; // the ruler edge the stroke in progress follows
};
} // namespace wb::tools
