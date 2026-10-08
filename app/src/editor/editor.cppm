// The editing session: document, undo history, camera, tools and navigation.
//
// Navigation is handled here, before tools see events, so it behaves the same with every tool:
//   - right / middle mouse drag, pen barrel button drag, or Space + drag: pan
//   - wheel or touchpad pinch: zoom at the pointer; Ctrl + wheel: scroll (Shift: horizontal)
//   - Ctrl+0: 100%, Ctrl+= / Ctrl+-: zoom in/out, Home: fit all content
//
// Every input device (mouse, pen, touch) remembers its own tool: picking the Pen for the mouse and the Select tool
// for the tablet leaves each of them with its choice. Holding Alt while pressing a tool key borrows that tool for as
// long as the key is held.
module;
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <SDL3/SDL.h>

export module wb.editor;

import wb.math;
import wb.doc.document;
import wb.doc.object;
import wb.doc.commands;
import wb.doc.edit;
import wb.doc.history;
import wb.doc.selection;
import wb.io.serializer;
import wb.view.camera;
import wb.brush.stroke_builder;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.editor.text_session;
import wb.text.system;
import wb.tools.tool;
import wb.tools.pen;
import wb.tools.eraser;
import wb.tools.select;

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
	// A pointer event that is not delivered to the tools (the UI has it) still tells which device is in use
	void notePointerDevice(platform::PointerDevice p_Device);
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
	// Set while the pen is the tool in effect: the outline of the stroke width at the pointer
	[[nodiscard]] std::optional<EraserCursor> brushCursor() const;

	// ---- tools. The selected tool belongs to the device used last (what the toolbar shows).
	void setTool(tools::ToolKind p_Kind);
	[[nodiscard]] tools::ToolKind selectedTool() const;
	[[nodiscard]] platform::PointerDevice activeDevice() const { return m_LastDevice; }
	void setDeviceTool(platform::PointerDevice p_Device, tools::ToolKind p_Kind);
	[[nodiscard]] tools::ToolKind deviceTool(platform::PointerDevice p_Device) const { return m_DeviceTools[static_cast<size_t>(p_Device)]; }
	// A tool borrowed with Alt+key and given back when the key is released
	[[nodiscard]] bool toolIsBorrowed() const { return m_Momentary.has_value(); }

	// ---- selection and object editing (each edit is one undo step)
	[[nodiscard]] Selection& selection() { return m_Selection; }
	[[nodiscard]] const Selection& selection() const { return m_Selection; }
	[[nodiscard]] tools::SelectState& selectState() { return m_SelectState; }
	[[nodiscard]] tools::SelectionOverlay selectionOverlay();
	[[nodiscard]] bool hasSelection() const { return !m_Selection.empty(); }
	[[nodiscard]] bool canPaste() const { return !m_Clip.empty(); }
	void selectAll();
	void clearSelection();
	void deleteSelection();
	void duplicateSelection();
	void copySelection();
	void cutSelection();
	void paste();
	void reorderSelection(ZOrderMove p_Move);
	// Mirrors the selection about its centre
	void flipSelection(bool p_Horizontal);
	// Puts a picture on the board, centred on p_WorldCenter and sized to look natural at the current zoom, and selects it
	ObjectId insertPicture(ImageAsset p_Asset, DVec2 p_WorldCenter);
	// Edits the selected pictures (one undo step); p_Edit returns false to leave a picture alone
	void editSelectedImages(const char* p_Name, const std::function<bool(ImageData&)>& p_Edit);
	// ---- text
	[[nodiscard]] text::TextSystem& textSystem() { return m_TextSystem; }
	[[nodiscard]] tools::TextState& textState() { return m_TextState; }
	[[nodiscard]] bool textEditing() const { return m_TextSession.active(); }
	[[nodiscard]] std::optional<TextEditView> textEditView() { return m_TextSession.view(); }
	void handleTextInput(std::string_view p_Text);
	void handleTextEditing(std::string_view p_Text, int p_CursorCodepoints);
	// Edits the text being typed, or else the selected text objects (one undo step)
	void applyTextStyle(const std::function<void(TextData&)>& p_Edit);
	// The text being edited, or the first selected text object
	[[nodiscard]] std::optional<TextData> currentText() const;
	// Starts typing in the selected text object (when exactly one is selected)
	bool beginEditingSelectedText();
	void endTextEditing() { m_TextSession.end(); }
	// Measures every text object again (fonts that were not available when the board was made may have arrived)
	void remeasureText();

	// Changes whenever something is copied or cut inside the app (the system clipboard may hold something newer)
	[[nodiscard]] uint64_t clipSerial() const { return m_ClipSerial; }
	void recolorSelection(Color p_Color);
	// Moves the selection by p_Points logical points on screen (zoom independent)
	void nudgeSelection(Vec2 p_Points);

	// Whole-board operations (used by the session: save, open, new). Loading and new end any gesture in progress and
	// clear the undo history.
	[[nodiscard]] std::vector<uint8_t> saveBoard(const std::string& p_SourcePath);
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

	struct BorrowedTool
	{
		tools::ToolKind kind = tools::ToolKind::Pen;
		SDL_Keycode key = SDLK_UNKNOWN;
	};

	[[nodiscard]] tools::ToolContext toolContext();
	[[nodiscard]] tools::Tool* toolFor(tools::ToolKind p_Kind) const;
	[[nodiscard]] tools::Tool* toolInEffect() const;
	void borrowTool(tools::ToolKind p_Kind, SDL_Keycode p_Key);
	void returnTool();
	void cancelGestures();
	void startZoom(double p_TargetZoom, DVec2 p_AnchorScreen);
	void startFlyTo(DVec2 p_Center, double p_Zoom);
	[[nodiscard]] TextData newTextData() const;
	void embedFonts();

	Document m_Document;
	History m_History;
	Selection m_Selection{ m_Document };
	Camera m_Camera;
	tools::BrushState m_Brush;
	tools::EraserState m_EraserState;
	tools::SelectState m_SelectState;
	BrushSettings m_BrushSettings;
	tools::TextState m_TextState;
	text::TextSystem m_TextSystem;
	TextSession m_TextSession{ m_Document, m_History, m_Selection, m_Camera, m_TextSystem };
	std::unique_ptr<tools::PenTool> m_Pen;
	std::unique_ptr<tools::EraserTool> m_Eraser;
	std::unique_ptr<tools::SelectTool> m_Select;
	std::unique_ptr<tools::HandTool> m_Hand;
	std::unique_ptr<tools::TextTool> m_TextTool;
	std::array<tools::ToolKind, 3> m_DeviceTools{ tools::ToolKind::Pen, tools::ToolKind::Pen, tools::ToolKind::Hand }; // mouse, pen, touch
	platform::PointerDevice m_LastDevice = platform::PointerDevice::Mouse;
	std::optional<BorrowedTool> m_Momentary;
	tools::Tool* m_ActiveTool = nullptr; // tool receiving pointer events (the gesture's tool while busy)
	bool m_HoverEraser = false;          // the pen's eraser end is in use / hovering

	ObjectClip m_Clip;
	uint64_t m_ClipSerial = 0;
	DVec2 m_LastPasteCenter{ 0.0 };
	int m_PasteRepeat = 0;
	uint64_t m_LastNudgeNs = 0;

	// Panning
	bool m_SpaceHeld = false;
	DVec2 m_PendingPan{ 0.0 }; // scroll-wheel pan (pixels) still to be applied, eased in by update()
	bool m_Panning = false;
	bool m_SwallowPointer = false; // the press that began text editing: its drag and release are ignored
	platform::PointerDevice m_SwallowDevice = platform::PointerDevice::Mouse;
	uint64_t m_LastClickNs = 0;
	DVec2 m_LastClickScreen{ 0.0 };
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
