// Vector eraser: sweeps a disc over the board and either cuts strokes or removes them whole. A drag is one undo step.
module;
#include <cstdint>
#include <memory>

export module wb.tools.eraser;

import wb.math;
import wb.doc.commands;
import wb.platform.input;
import wb.tools.tool;

export namespace wb::tools
{
class EraserTool final : public Tool
{
public:
	[[nodiscard]] ToolKind kind() const override { return ToolKind::Eraser; }
	void onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context) override;
	void cancel(ToolContext& p_Context) override;
	[[nodiscard]] bool isBusy() const override { return m_Command != nullptr; }
	[[nodiscard]] CursorKind cursor() const override { return CursorKind::Crosshair; }

private:
	void eraseAlong(const DVec2 p_From, const DVec2 p_To, ToolContext& p_Context);
	void finish(ToolContext& p_Context);

	std::unique_ptr<ReplaceObjectsCommand> m_Command;
	platform::PointerDevice m_Device = platform::PointerDevice::Mouse;
	DVec2 m_Last{ 0.0 };
};
} // namespace wb::tools
