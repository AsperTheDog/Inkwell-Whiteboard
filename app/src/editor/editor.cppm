// The editing session: document, undo history, camera, tools and navigation.
//
// Navigation is handled here, before tools see events, so it behaves the same with every tool:
//   - right / middle mouse drag, pen barrel button drag, or Space + drag: pan
//   - wheel or touchpad pinch: zoom at the pointer; Ctrl + wheel: scroll (Shift: horizontal)
//   - Ctrl+0: 100%, Ctrl+= / Ctrl+-: zoom in/out, Home: fit all content
module;
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <SDL3/SDL.h>

export module wb.editor;

import wb.math;
import wb.doc.document;
import wb.doc.history;
import wb.io.serializer;
import wb.view.camera;
import wb.brush.stroke_builder;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.tools.tool;
import wb.tools.pen;
import wb.tools.eraser;

export namespace wb
{
// Eraser outline to draw over the canvas (window pixels)
struct EraserCursor
{
	Vec2 center{ 0.f };
	float radiusPixels = 0.f;
};

class Editor
{
public:
	Editor();

	void setViewport(Vec2 p_SizePixels, float p_PixelScale);

	void handlePointer(const platform::PointerEvent& p_Event);
	void handleWheel(const platform::WheelEvent& p_Event);
	void handlePinch(const platform::PinchEvent& p_Event);
	// Returns true when the key was used
	bool handleKeyDown(const SDL_KeyboardEvent& p_Event);
	void handleKeyUp(const SDL_KeyboardEvent& p_Event);
	// Window lost focus: end gestures cleanly
	void handleFocusLost();

	// Advances animations; returns true while something is still animating
	bool update(double p_DeltaSeconds);

	// True while a pointer gesture (stroke, pan...) is in progress: it must receive every pointer event
	[[nodiscard]] bool isBusy() const;
	[[nodiscard]] tools::CursorKind cursor() const;
	[[nodiscard]] std::optional<render::LiveStrokeView> liveStroke() const;
	// Set while the eraser is the tool in effect (selected, or the pen's eraser end is hovering / pressed)
	[[nodiscard]] std::optional<EraserCursor> eraserCursor() const;

	void setTool(tools::ToolKind p_Kind);
	[[nodiscard]] tools::ToolKind selectedTool() const { return m_SelectedTool; }

	// Whole-board operations (used by the session: save, open, new). Loading and new end any gesture in progress and
	// clear the undo history.
	[[nodiscard]] std::vector<uint8_t> saveBoard(const std::string& p_SourcePath) const;
	[[nodiscard]] LoadResult loadBoard(std::span<const uint8_t> p_Bytes, BoardMeta& p_Meta);
	void newBoard();

	void undo();
	void redo();
	void zoomAroundCenter(double p_Factor);
	void resetZoom();
	void fitContent();
	// Jumps (no animation) / flies to a view
	void lookAt(DVec2 p_Center, double p_Zoom);
	void flyTo(DVec2 p_Center, double p_Zoom) { startFlyTo(p_Center, p_Zoom); }

	[[nodiscard]] Document& document() { return m_Document; }
	[[nodiscard]] const Document& document() const { return m_Document; }
	[[nodiscard]] History& history() { return m_History; }
	[[nodiscard]] const Camera& camera() const { return m_Camera; }
	[[nodiscard]] tools::BrushState& brush() { return m_Brush; }
	[[nodiscard]] tools::EraserState& eraser() { return m_EraserState; }
	[[nodiscard]] BrushSettings& brushSettings() { return m_BrushSettings; }

private:
	enum class CameraAnimation : uint8_t
	{
		None,
		ZoomAtAnchor, // wheel/keyboard zoom: keeps the anchor point fixed
		FlyTo,        // fit/recenter: moves center and zoom together
	};

	[[nodiscard]] tools::ToolContext toolContext();
	[[nodiscard]] tools::Tool* toolFor(tools::ToolKind p_Kind);
	[[nodiscard]] tools::Tool* toolInEffect() const;
	void cancelGestures();
	void startZoom(double p_TargetZoom, DVec2 p_AnchorScreen);
	void startFlyTo(DVec2 p_Center, double p_Zoom);

	Document m_Document;
	History m_History;
	Camera m_Camera;
	tools::BrushState m_Brush;
	tools::EraserState m_EraserState;
	BrushSettings m_BrushSettings;
	std::unique_ptr<tools::PenTool> m_Pen;
	std::unique_ptr<tools::EraserTool> m_Eraser;
	tools::ToolKind m_SelectedTool = tools::ToolKind::Pen;
	tools::Tool* m_ActiveTool = nullptr; // tool receiving pointer events (the gesture's tool while busy)
	bool m_HoverEraser = false;          // the pen's eraser end is in use / hovering

	// Panning
	bool m_SpaceHeld = false;
	DVec2 m_PendingPan{ 0.0 }; // scroll-wheel pan (pixels) still to be applied, eased in by update()
	bool m_Panning = false;
	platform::PointerDevice m_PanDevice = platform::PointerDevice::Mouse;
	platform::PointerButton m_PanButton = platform::PointerButton::None;
	Vec2 m_PanLast{ 0.f };
	Vec2 m_LastPointer{ 0.f };

	// Camera animation
	CameraAnimation m_Animation = CameraAnimation::None;
	double m_TargetZoom = 1.0;
	DVec2 m_ZoomAnchor{ 0.0 };
	DVec2 m_TargetCenter{ 0.0 };
};
} // namespace wb
