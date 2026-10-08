// Tool interface. The editor routes pointer events to the active tool after handling navigation.
module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

export module wb.tools.tool;

import wb.math;
import wb.doc.object;
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
	Text, // click to type; the editor runs the text session
};

inline constexpr size_t TOOL_KIND_COUNT = 5;

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
	IBeam,
};

inline constexpr size_t CURSOR_KIND_COUNT = 10;

// How the brush size relates to the zoom level
enum class BrushSizeMode : uint8_t
{
	Screen, // the stroke is as wide on screen whatever the zoom: zooming in gives finer lines on the board
	Board,  // the stroke has a fixed width on the board: zooming in gives wider lines on screen
};

// What the user picked in the toolbar for drawing
struct BrushState
{
	Color color = Color::fromRgba8(0x1F1F1FFFu);
	float sizePoints = 3.f; // stroke width in logical points (on screen, or on the board at 100% zoom in Board mode)
	BrushSizeMode sizeMode = BrushSizeMode::Screen;
	float sizeScale = 1.f;  // global multiplier on the size

	// Width to draw with right now, in logical points on screen, for a camera at p_Zoom
	[[nodiscard]] float effectivePoints(const double p_Zoom) const
	{
		return sizePoints * sizeScale * (sizeMode == BrushSizeMode::Board ? static_cast<float>(p_Zoom) : 1.f);
	}
};

// What the user picked in the toolbar for typing. The size is in logical points on screen at the zoom the text is
// created at.
struct TextState
{
	std::string family = "Inter";
	uint8_t style = 0; // TextStyle flags
	float sizePoints = 28.f;
	TextAlign align = TextAlign::Left;
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
