// The editing session: document, undo history, camera, tools and navigation.
//
// Navigation is handled here, before tools see events, so it behaves the same with every tool:
//   - right / middle mouse drag, pen barrel button drag, or Space + drag: pan
//   - wheel: pan (Shift: horizontal); Ctrl + wheel or touchpad pinch: zoom at the pointer
//   - Ctrl+0: 100%, Ctrl+= / Ctrl+-: zoom in/out, Home: fit all content
module;
#include <cstdint>
#include <memory>
#include <optional>
#include <SDL3/SDL.h>

export module wb.editor;

import wb.math;
import wb.doc.document;
import wb.doc.history;
import wb.view.camera;
import wb.brush.stroke_builder;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.tools.tool;
import wb.tools.pen;

export namespace wb
{
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
	[[nodiscard]] BrushSettings& brushSettings() { return m_BrushSettings; }

private:
	enum class CameraAnimation : uint8_t
	{
		None,
		ZoomAtAnchor, // wheel/keyboard zoom: keeps the anchor point fixed
		FlyTo,        // fit/recenter: moves center and zoom together
	};

	[[nodiscard]] tools::ToolContext toolContext();
	void startZoom(double p_TargetZoom, DVec2 p_AnchorScreen);
	void startFlyTo(DVec2 p_Center, double p_Zoom);

	Document m_Document;
	History m_History;
	Camera m_Camera;
	tools::BrushState m_Brush;
	BrushSettings m_BrushSettings;
	std::unique_ptr<tools::PenTool> m_Pen;
	tools::Tool* m_ActiveTool = nullptr;

	// Panning
	bool m_SpaceHeld = false;
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
