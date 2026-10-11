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
import wb.io.blob;
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
import wb.tools.shape;
import wb.tools.laser;
import wb.view.ruler;

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

	// ---- read-only (a collaboration guest the host only lets watch): only the hand and laser tools work, nothing on the
	// board can be added, changed, deleted or undone. Looking around is unaffected.
	void setReadOnly(bool p_ReadOnly);
	[[nodiscard]] bool readOnly() const { return m_ReadOnly; }

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
	// Locking: how much of the selection is locked, and the switch (one undo step). Locking an unlocked selection
	// locks all of it; unlocking a partly or fully locked one unlocks all of it.
	enum class LockState : uint8_t
	{
		None,
		Some,
		All,
	};
	// Lines the selected objects up, or spaces them evenly (one undo step)
	void alignSelection(AlignMode p_Mode);
	// Evens out the selected strokes a little more each time (one undo step)
	void smoothSelection();
	[[nodiscard]] LockState selectionLockState() const;
	void setSelectionLocked(bool p_Locked);
	void toggleSelectionLock();
	// Grouping: the selected objects become one that is selected, moved, copied and deleted as a whole (one undo step).
	// canGroupSelection() is true for two or more unlocked objects that are not already one group; the selection is
	// "grouped" when it contains a group.
	[[nodiscard]] bool canGroupSelection() const;
	[[nodiscard]] bool selectionHasGroup() const;
	void groupSelection();
	void ungroupSelection();
	void toggleSelectionGroup();
	// Makes the selected objects upright again (one undo step)
	void resetSelectionTransform();
	[[nodiscard]] bool selectionIsTilted() const;
	// Mirrors the selection about its centre
	void flipSelection(bool p_Horizontal);
	// Puts a picture on the board, centred on p_WorldCenter and sized to look natural at the current zoom, and selects it
	ObjectId insertPicture(ImageAsset p_Asset, DVec2 p_WorldCenter);
	// Same for a video (the asset holds the encoded file; its width and height are the picture size)
	ObjectId insertVideo(ImageAsset p_Asset, DVec2 p_WorldCenter);
	// The pages of a PDF, each drawn to a picture. p_Size is how big the page is on the board.
	struct PageImage
	{
		ImageAsset asset;
		Vec2 size{ 0.f };
	};
	// Lays the pages out one under another at the top of the view, locked so they stay put while drawing over them
	// (one undo step), and flies to the first one. Returns the ids.
	std::vector<ObjectId> insertPages(std::vector<PageImage> p_Pages);
	// Edits one video (one undo step); p_Edit returns false to leave it alone
	void editVideo(ObjectId p_Id, const char* p_Name, const std::function<bool(VideoData&)>& p_Edit);
	// Edits the selected pictures (one undo step); p_Edit returns false to leave a picture alone
	void editSelectedImages(const char* p_Name, const std::function<bool(ImageData&)>& p_Edit);
	// The same for given pictures, whether they are selected or not (one undo step)
	void editImages(std::span<const ObjectId> p_Ids, const char* p_Name, const std::function<bool(ImageData&)>& p_Edit);
	// ---- cropping. Shows the whole picture with the hidden part darkened; dragging the frame's handles (or the frame
	// itself) chooses what is shown. Each drag is one undo step, and the file is never changed, so a crop can always be
	// widened again. Enter / Escape, or a click outside the picture, ends it.
	struct CropView
	{
		ObjectId id = INVALID_OBJECT_ID;
		Affine2 transform{};  // picture-local -> world
		Rect full{};          // the whole picture, in picture-local space
		Rect shown{};         // the frame, same space (follows the pointer while dragging)
		int hovered = -1;     // handle under the pointer, or being dragged (see cropHandle*)
		bool dragging = false;
	};
	// The 8 handles go clockwise from the top left corner; 8 is the inside of the frame (it moves)
	static constexpr int CROP_HANDLE_COUNT = 8;
	static constexpr int CROP_HANDLE_MOVE = 8;
	bool beginCrop(ObjectId p_Id);
	bool beginCropSelected();
	void endCrop();
	[[nodiscard]] bool cropping() const { return m_CropId != INVALID_OBJECT_ID; }
	[[nodiscard]] std::optional<CropView> cropView() const;
	// Where a handle is, in picture-local space
	[[nodiscard]] static DVec2 cropHandlePoint(const Rect& p_Shown, int p_Handle);

	// ---- text
	[[nodiscard]] text::TextSystem& textSystem() { return m_TextSystem; }
	[[nodiscard]] tools::TextState& textState() { return m_TextState; }
	[[nodiscard]] bool textEditing() const { return m_TextSession.active(); }
	[[nodiscard]] std::optional<TextEditView> textEditView() { return m_TextSession.view(); }
	void handleTextInput(std::string_view p_Text);
	void handleTextEditing(std::string_view p_Text, int p_CursorCodepoints);
	// Edits the text being typed, or else the selected text objects (one undo step)
	void applyTextStyle(const std::function<void(TextData&)>& p_Edit);
	// Sets the size the text appears at on screen, in logical points at the current zoom
	void setTextSizePoints(float p_Points);
	// The text being edited, or the first selected text object, and how much its object is scaled
	struct CurrentText
	{
		TextData data;
		double scale = 1.0;
	};
	[[nodiscard]] std::optional<CurrentText> currentText() const;
	// Starts typing in the selected text object (when exactly one is selected)
	bool beginEditingSelectedText();
	void endTextEditing() { m_TextSession.end(); }
	// Somebody else deleted the text being edited (collaboration): the session just stops, there is nothing left to keep
	void abortTextEditingIfGone()
	{
		if (m_TextSession.active() && m_Document.find(m_TextSession.id()) == nullptr)
			m_TextSession.abort();
	}
	// Puts pasted text on the board as a text box with the current text style, centred on p_WorldCenter
	ObjectId insertText(std::string p_Text, DVec2 p_WorldCenter);
	// Measures every text object again (fonts that were not available when the board was made may have arrived)
	void remeasureText();
	void finishLoading(const BoardMeta& p_Meta);

	// Changes whenever something is copied or cut inside the app (the system clipboard may hold something newer)
	[[nodiscard]] uint64_t clipSerial() const { return m_ClipSerial; }
	void recolorSelection(Color p_Color);
	// Moves the selection by p_Points logical points on screen (zoom independent)
	void nudgeSelection(Vec2 p_Points);

	// Whole-board operations (used by the session: save, open, new). Loading and new end any gesture in progress and
	// clear the undo history.
	[[nodiscard]] std::vector<uint8_t> saveBoard(const std::string& p_SourcePath);
	// For a background write: big files are referenced, not copied (see serializeBoardSplit)
	[[nodiscard]] SplitBoard saveBoardSplit(const std::string& p_SourcePath, const SplitOptions& p_Options);
	// A collaboration snapshot: the board without its big pictures and videos (they are streamed afterwards)
	[[nodiscard]] std::vector<uint8_t> saveBoardShell(size_t p_InlineAssetBytes);
	// p_AllowMissingAssets keeps pictures and videos whose file is not in the data (collaboration)
	[[nodiscard]] LoadResult loadBoard(std::span<const uint8_t> p_Bytes, BoardMeta& p_Meta, bool p_AllowMissingAssets = false);
	// A board file opened in place: its big pictures and videos stay in the file and are read from there
	[[nodiscard]] LoadResult loadBoardFile(const std::shared_ptr<const ByteSource>& p_File, BoardMeta& p_Meta);
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
	[[nodiscard]] tools::BrushState& highlighterBrush() { return m_HighlighterBrush; }
	[[nodiscard]] tools::ShapeState& shapeState() { return m_ShapeState; }
	[[nodiscard]] const tools::LaserTool& laser() const { return *m_Laser; }
	[[nodiscard]] tools::LaserTool& laser() { return *m_Laser; }

	// ---- ruler. Not part of the board: pens that start next to its edge follow the edge.
	[[nodiscard]] const Ruler& ruler() const { return m_Ruler; }
	[[nodiscard]] bool rulerVisible() const { return m_Ruler.visible; }
	void toggleRuler();
	// True while the ruler is being turned, for the angle readout
	[[nodiscard]] bool rulerTurning() const { return m_RulerDrag == RulerDrag::Turn; }

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
	bool updateCamera(double p_DeltaSeconds);
	// Moves / turns the ruler; returns true when the event belongs to it
	bool handleRulerPointer(const platform::PointerEvent& p_Event, DVec2 p_World);
	[[nodiscard]] TextData newTextData() const;
	void embedFonts();
	ObjectId insertMedia(ImageAsset p_Asset, DVec2 p_WorldCenter, bool p_Video);
	void applyTextStyleScaled(const std::function<void(TextData&, double)>& p_Edit);
	// Pointer events while cropping; true when the crop took the event
	bool handleCropPointer(const platform::PointerEvent& p_Event, DVec2 p_World);
	[[nodiscard]] int cropHandleAt(const Object& p_Object, const Rect& p_Shown, DVec2 p_Screen) const;

	Document m_Document;
	History m_History;
	Selection m_Selection{ m_Document };
	Camera m_Camera;
	tools::BrushState m_Brush;
	tools::BrushState m_HighlighterBrush{ .color = Color::fromRgba8(0xFDD835FFu), .sizePoints = 18.f };
	tools::ShapeState m_ShapeState;
	Ruler m_Ruler;
	enum class RulerDrag : uint8_t
	{
		None,
		Move,
		Turn,
	};
	RulerDrag m_RulerDrag = RulerDrag::None;
	DVec2 m_RulerGrab{ 0.0 };
	tools::EraserState m_EraserState;
	tools::SelectState m_SelectState;
	BrushSettings m_BrushSettings;
	tools::TextState m_TextState;
	text::TextSystem m_TextSystem;
	TextSession m_TextSession{ m_Document, m_History, m_Selection, m_Camera, m_TextSystem };
	std::unique_ptr<tools::PenTool> m_Pen;
	std::unique_ptr<tools::PenTool> m_Highlighter;
	std::unique_ptr<tools::ShapeTool> m_ShapeTool;
	std::unique_ptr<tools::LaserTool> m_Laser;
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

	// Cropping
	ObjectId m_CropId = INVALID_OBJECT_ID;
	int m_CropHover = -1;
	int m_CropDrag = -1;  // handle being dragged, -1 when none
	Rect m_CropShown{};   // the frame while dragging
	DVec2 m_CropGrab{ 0.0 }; // moving the frame: where in the picture the pointer took hold
	Rect m_CropStart{};

	// Panning
	bool m_SpaceHeld = false;
	bool m_ReadOnly = false;
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
