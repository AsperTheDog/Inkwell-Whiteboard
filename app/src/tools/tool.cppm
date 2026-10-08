// Tool interface. The editor routes pointer events to the active tool after handling navigation.
module;
#include <cstddef>
#include <cstdint>
#include <optional>

export module wb.tools.tool;

import wb.math;
import wb.doc.document;
import wb.doc.history;
import wb.doc.selection;
import wb.view.camera;
import wb.brush.stroke_builder;
import wb.platform.input;

export namespace wb::tools
{
enum class ToolKind : uint8_t
{
	Pen,
	Eraser,
	Select,
	Hand, // drags the view around, like the scroll wheel does with a pointer
};

inline constexpr size_t TOOL_KIND_COUNT = 4;

enum class CursorKind : uint8_t
{
	Default,
	Crosshair,
	Move,
	Pointer,
	ResizeEW,
	ResizeNS,
	ResizeNWSE,
	ResizeNESW,
	Rotate,
};

inline constexpr size_t CURSOR_KIND_COUNT = 9;

// What the user picked in the toolbar for drawing
struct BrushState
{
	Color color = Color::fromRgba8(0x1F1F1FFFu);
	float sizePoints = 3.f; // stroke width in logical points on screen
};

enum class EraserMode : uint8_t
{
	Segment, // cuts strokes exactly where the eraser touches them
	Stroke,  // removes every stroke it touches
};

struct EraserState
{
	EraserMode mode = EraserMode::Segment;
	float sizePoints = 18.f; // eraser diameter in logical points on screen
};

enum class SelectMode : uint8_t
{
	Box,   // drag a rectangle
	Lasso, // draw around the objects
};

struct SelectState
{
	SelectMode mode = SelectMode::Box;
};

struct ToolContext
{
	Document& document;
	History& history;
	Selection& selection;
	SelectState& select;
	const Camera& camera;
	BrushState& brush;
	EraserState& eraser;
	BrushSettings& brushSettings;
};

class Tool
{
public:
	virtual ~Tool() = default;

	[[nodiscard]] virtual ToolKind kind() const = 0;
	virtual void onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context) = 0;
	// Abort any gesture in progress (tool switch, focus loss...)
	virtual void cancel(ToolContext& p_Context) = 0;
	// True while a gesture is in progress (the tool then receives every pointer event, even over UI)
	[[nodiscard]] virtual bool isBusy() const = 0;
	[[nodiscard]] virtual CursorKind cursor() const { return CursorKind::Default; }
};
} // namespace wb::tools
