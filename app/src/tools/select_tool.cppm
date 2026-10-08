// Selection: click, box and lasso selection, plus a transform box with move, 8 scale handles and a rotate handle.
//
// Dragging edits the objects live (so the renderer shows the result at once) and records one undoable
// TransformObjectsCommand when the gesture ends. Escape / focus loss puts everything back.
module;
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

export module wb.tools.select;

import wb.math;
import wb.doc.object;
import wb.doc.gizmo;
import wb.platform.input;
import wb.tools.tool;

export namespace wb::tools
{
// Everything the UI needs to draw the selection; all positions are window pixels
struct SelectionOverlay
{
	bool visible = false;     // a selection box exists
	bool interactive = false; // handles can be grabbed (the Select tool is in effect)

	std::array<Vec2, 4> corners{};                          // NW, NE, SE, SW
	std::array<Vec2, SCALE_HANDLE_COUNT> handles{};         // N, NE, E, SE, S, SW, W, NW
	std::array<bool, SCALE_HANDLE_COUNT> handleShown{};     // edge handles hide on very small boxes
	Vec2 rotateHandle{ 0.f };
	Vec2 rotateStem{ 0.f };                                 // where the rotate handle's line meets the box
	Handle hovered = Handle::None;
	Handle active = Handle::None;
	Vec2 boundsMin{ 0.f };                                  // screen AABB of the box, for placing bars next to it
	Vec2 boundsMax{ 0.f };
	std::vector<std::pair<Vec2, Vec2>> objectBoxes;         // per-object outlines when several objects are selected

	bool marquee = false;
	Vec2 marqueeMin{ 0.f };
	Vec2 marqueeMax{ 0.f };
	std::vector<Vec2> lasso;
};

class SelectTool final : public Tool
{
public:
	[[nodiscard]] ToolKind kind() const override { return ToolKind::Select; }
	void onPointer(const platform::PointerEvent& p_Event, ToolContext& p_Context) override;
	void cancel(ToolContext& p_Context) override;
	[[nodiscard]] bool isBusy() const override { return m_Gesture != Gesture::None; }
	[[nodiscard]] CursorKind cursor() const override { return m_Cursor; }

	// Overlay for the current selection (valid whichever tool is in effect; only interactive for this one)
	[[nodiscard]] SelectionOverlay overlay(ToolContext& p_Context, bool p_Interactive);

	// The selected objects were transformed by something other than a gesture of this tool (arrow-key nudge):
	// the box follows. p_RevisionBefore is the document revision before that edit.
	void noteTransformed(const Affine2& p_World, uint64_t p_RevisionBefore, ToolContext& p_Context);

private:
	enum class Gesture : uint8_t
	{
		None,
		Move,
		Resize,
		Rotate,
		Marquee,
		Lasso,
	};

	struct Base
	{
		ObjectId id = INVALID_OBJECT_ID;
		Affine2 transform{};
	};

	// What the selection holds of text: its boxes scale as a whole (corners keep the proportions); the side handles of a
	// single upright text box change where its lines wrap instead
	struct TextInfo
	{
		bool any = false;
		bool singleUpright = false;
		ObjectId id = INVALID_OBJECT_ID;
	};
	[[nodiscard]] TextInfo textInfo(const ToolContext& p_Context) const;
	void applyTextResize(ToolContext& p_Context);
	void ensureFrame(ToolContext& p_Context);
	void begin(const platform::PointerEvent& p_Event, ToolContext& p_Context);
	void drag(const platform::PointerEvent& p_Event, ToolContext& p_Context);
	void finish(ToolContext& p_Context);
	void restore(ToolContext& p_Context);
	void updateHover(const platform::PointerEvent& p_Event, ToolContext& p_Context);
	void startTransform(Gesture p_Gesture, ToolContext& p_Context);
	void applyTransform(const FrameTransform& p_Result, ToolContext& p_Context);
	[[nodiscard]] Handle hitHandle(DVec2 p_Screen, platform::PointerDevice p_Device, const ToolContext& p_Context) const;
	[[nodiscard]] bool insideFrame(DVec2 p_World, const ToolContext& p_Context) const;
	[[nodiscard]] double paddingWorld(const ToolContext& p_Context) const;
	[[nodiscard]] bool edgeHandleShown(Handle p_Handle, const ToolContext& p_Context) const;
	[[nodiscard]] CursorKind cursorForHandle(Handle p_Handle) const;

	Gesture m_Gesture = Gesture::None;
	platform::PointerDevice m_Device = platform::PointerDevice::Mouse;
	CursorKind m_Cursor = CursorKind::Default;

	// Transform box
	SelectionFrame m_Frame{};
	bool m_HasFrame = false;
	uint64_t m_FrameSelectionRevision = 0;
	uint64_t m_FrameDocumentRevision = 0;
	Handle m_Hover = Handle::None;

	// Gesture state
	DVec2 m_DownScreen{ 0.0 };
	DVec2 m_DownWorld{ 0.0 };
	DVec2 m_CurrentWorld{ 0.0 };
	bool m_Dragged = false;
	bool m_Additive = false;
	ObjectId m_ClickReduce = INVALID_OBJECT_ID; // click without dragging on one of several selected objects keeps only that one
	ObjectId m_ClickToggle = INVALID_OBJECT_ID; // shift+click without dragging on a selected object deselects it
	Handle m_ActiveHandle = Handle::None;
	DVec2 m_GrabOffset{ 0.0 };
	double m_RotateStart = 0.0;
	SelectionFrame m_BaseFrame{};
	std::vector<Base> m_Bases;
	bool m_TextResize = false; // dragging a side handle of a text box: changes its wrap width
	ObjectId m_TextResizeId = INVALID_OBJECT_ID;
	TextData m_TextBase;
	Affine2 m_TextBaseTransform{};
	std::vector<ObjectId> m_KeptSelection; // additive box / lasso: what was selected before
	std::vector<DVec2> m_Lasso;
};

// Clicking with this tool starts typing; the editor runs the text session
class TextTool final : public Tool
{
public:
	[[nodiscard]] ToolKind kind() const override { return ToolKind::Text; }
	void onPointer(const platform::PointerEvent&, ToolContext&) override {}
	void cancel(ToolContext&) override {}
	[[nodiscard]] bool isBusy() const override { return false; }
	[[nodiscard]] CursorKind cursor() const override { return CursorKind::IBeam; }
};

// Dragging with this tool moves the view; the editor does the panning
class HandTool final : public Tool
{
public:
	[[nodiscard]] ToolKind kind() const override { return ToolKind::Hand; }
	void onPointer(const platform::PointerEvent&, ToolContext&) override {}
	void cancel(ToolContext&) override {}
	[[nodiscard]] bool isBusy() const override { return false; }
	[[nodiscard]] CursorKind cursor() const override { return CursorKind::Move; }
};
} // namespace wb::tools
