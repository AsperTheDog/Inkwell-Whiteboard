// Immediate-mode UI core: pointer state, hit testing between overlapping panels, animations, tooltips and the
// widgets the app is built from. Everything is in window pixels; sizes written in "points" are multiplied by the
// UI scale (px()).
//
// Input model: pointer events arrive between frames. pointerEvent() decides immediately whether the UI owns the
// event (using the panels registered in the previous frame) and records press/release edges; the next frame's
// widgets consume the edges. A press that starts on the UI is captured: the whole drag belongs to the UI.
module;
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module wb.ui.context;

import wb.math;
import wb.platform.input;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;

export namespace wb::ui
{
struct FrameParams
{
	Vec2 viewport{ 0.f };
	float scale = 1.f;
	double dt = 0.0;
	Theme theme{};
};

struct Interaction
{
	bool hovered = false;
	bool held = false;
	bool clicked = false;
};

enum class ButtonStyle : uint8_t
{
	Primary,   // filled with the accent colour
	Secondary, // outlined
	Ghost,     // text only
	Danger,    // filled with the danger colour
};

class Context
{
public:
	void init(FontAtlas& p_Font);

	void beginFrame(const FrameParams& p_Params);
	// Draws what floats above everything (tooltips) and closes the frame's input edges
	void endFrame();

	// ---- input
	// Returns true when the UI owns the event (the editor must not see it)
	bool pointerEvent(const platform::PointerEvent& p_Event);
	[[nodiscard]] bool wantsPointer(Vec2 p_Position) const;
	[[nodiscard]] bool captured() const { return m_Captured; }
	// The UI scrolls or zooms nothing itself; the wheel over a panel is simply swallowed
	[[nodiscard]] Vec2 pointerPosition() const { return m_Pos; }
	[[nodiscard]] platform::PointerDevice pointerDevice() const { return m_Device; }
	// True while frames must keep coming for animations or a pending tooltip
	[[nodiscard]] bool animating() const { return m_Animating || m_TooltipPending; }
	// The pointer was over a clickable widget in the last frame (the app shows a pointing cursor)
	[[nodiscard]] bool overInteractive() const { return m_HoverAnyPrev; }

	// ---- state
	[[nodiscard]] DrawList& draw() { return m_Draw; }
	[[nodiscard]] FontAtlas& font() { return *m_Font; }
	[[nodiscard]] const Theme& theme() const { return m_Theme; }
	[[nodiscard]] Vec2 viewport() const { return m_Viewport; }
	[[nodiscard]] float scale() const { return m_Scale; }
	[[nodiscard]] float px(const float p_Points) const { return p_Points * m_Scale; }
	// Whole-pixel font size for a size in points
	[[nodiscard]] int fontPx(float p_Points) const;
	[[nodiscard]] double deltaTime() const { return m_Dt; }

	// ---- identity and animation
	[[nodiscard]] static uint32_t id(std::string_view p_Key);
	[[nodiscard]] static uint32_t id(std::string_view p_Key, int p_Index);
	// Eases toward p_Target. The first call starts at p_Initial (default: the target itself, so nothing animates).
	float anim(uint32_t p_Key, float p_Target, float p_Rate = 18.f);
	float animFrom(uint32_t p_Key, float p_Initial, float p_Target, float p_Rate = 18.f);
	// Jumps an animation to a value (e.g. back to 0 when a popup closes, so it plays again when it reopens)
	void setAnim(const uint32_t p_Key, const float p_Value) { m_Anims[p_Key] = p_Value; }

	// ---- layers: a panel blocks the pointer for everything below it
	void beginPanel(Rect2 p_Rect);
	// Draws a floating panel (shadow, fill, border) and registers it
	void panel(Rect2 p_Rect, float p_Radius);
	[[nodiscard]] bool hit(Rect2 p_Rect, Vec2 p_Position) const;

	// ---- widgets. Keys only need to be unique within a frame.
	Interaction interact(uint32_t p_Id, Rect2 p_Rect);
	bool iconButton(std::string_view p_Key, Rect2 p_Rect, Icon p_Icon, bool p_Selected, bool p_Enabled, std::string_view p_Tooltip, const Color* p_Badge = nullptr);
	bool button(std::string_view p_Key, Rect2 p_Rect, std::string_view p_Label, ButtonStyle p_Style, bool p_Enabled = true, Icon p_Icon = Icon::None);
	bool menuRow(std::string_view p_Key, Rect2 p_Rect, Icon p_Icon, std::string_view p_Label, std::string_view p_Shortcut, bool p_Enabled, bool p_Checked);
	bool swatch(std::string_view p_Key, Rect2 p_Rect, Color p_Color, bool p_Selected, std::string_view p_Tooltip = {});
	// Returns true when the value changed. p_Logarithmic maps the track exponentially (p_Min must be > 0).
	bool slider(std::string_view p_Key, Rect2 p_Rect, float& p_Value, float p_Min, float p_Max, bool p_Logarithmic);
	bool segmented(std::string_view p_Key, Rect2 p_Rect, std::span<const std::string_view> p_Labels, std::span<const Icon> p_Icons, int& p_Selected);
	// Saturation/value square above a hue bar filling p_Rect
	bool colorPicker(std::string_view p_Key, Rect2 p_Rect, Hsv& p_Hsv);
	void divider(Vec2 p_A, Vec2 p_B);
	// Text with the UI's regular colours
	float label(Vec2 p_Position, std::string_view p_Text, float p_Points, Color p_Color, TextAlign p_Align = TextAlign::Left);

	// Tooltip for the widget under the pointer (called by the widgets)
	void noteTooltip(uint32_t p_Id, Rect2 p_Rect, std::string_view p_Text, bool p_Hovered);

private:
	[[nodiscard]] bool coveredAbove(size_t p_Layer, Vec2 p_Position) const;

	FontAtlas* m_Font = nullptr;
	DrawList m_Draw;
	Theme m_Theme{};
	Vec2 m_Viewport{ 0.f };
	float m_Scale = 1.f;
	double m_Dt = 0.0;

	// Pointer
	Vec2 m_Pos{ -1000.f };
	Vec2 m_PressPos{ 0.f };
	Vec2 m_ReleasePos{ 0.f };
	bool m_Down = false;
	bool m_Pressed = false;
	bool m_Released = false;
	bool m_Captured = false;
	bool m_HoverValid = false;
	platform::PointerDevice m_Device = platform::PointerDevice::Mouse;

	// Panels: [i] has layer i + 1
	std::vector<Rect2> m_Panels;
	std::vector<Rect2> m_PrevPanels;
	size_t m_Layer = 0;

	uint32_t m_Active = 0;
	bool m_HoverAny = false;
	bool m_HoverAnyPrev = false;

	// Animation
	std::unordered_map<uint32_t, float> m_Anims;
	bool m_Animating = false;
	bool m_AnimatingNext = false;

	// Tooltip
	uint32_t m_HoverId = 0;
	double m_HoverTime = 0.0;
	bool m_HoverSeen = false;
	bool m_TooltipPending = false;
	Rect2 m_TooltipRect{};
	std::string m_TooltipText;
	bool m_TooltipShown = false;
};
} // namespace wb::ui
