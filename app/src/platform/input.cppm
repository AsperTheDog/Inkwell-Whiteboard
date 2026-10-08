// Unifies mouse, pen and touch input into one PointerEvent stream (positions in window pixels).
//
// Pen pressure/tilt arrive as separate SDL axis events; the router keeps the latest values per pen and stamps
// them onto every pointer event. Synthetic mouse events generated from pen/touch are dropped so tools never
// see the same contact twice.
module;
#include <cstdint>
#include <optional>
#include <variant>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

export module wb.platform.input;

import wb.math;

export namespace wb::platform
{
enum class PointerDevice : uint8_t
{
	Mouse,
	Pen,
	Touch,
};

enum class PointerPhase : uint8_t
{
	Down,
	Move,
	Up,
	Cancel, // contact lost without a proper release (pen left proximity, window lost focus...)
};

enum class PointerButton : uint8_t
{
	None,
	Primary,   // left mouse button / pen tip / finger
	Secondary, // right mouse button / pen barrel button 1
	Middle,    // middle mouse button / pen barrel button 2
};

namespace ButtonMask
{
inline constexpr uint8_t Primary = 1u << 0;
inline constexpr uint8_t Secondary = 1u << 1;
inline constexpr uint8_t Middle = 1u << 2;
} // namespace ButtonMask

namespace Modifier
{
inline constexpr uint8_t Shift = 1u << 0;
inline constexpr uint8_t Ctrl = 1u << 1;
inline constexpr uint8_t Alt = 1u << 2;
} // namespace Modifier

struct PointerEvent
{
	PointerPhase phase = PointerPhase::Move;
	PointerDevice device = PointerDevice::Mouse;
	PointerButton button = PointerButton::None; // button that changed (Down/Up only)
	uint8_t buttons = 0;                        // ButtonMask of buttons held after this event
	uint8_t modifiers = 0;                      // Modifier mask
	bool eraser = false;                        // pen is using its eraser end
	Vec2 position{ 0.f };                       // window pixels
	float pressure = 1.f;                       // 0..1; 1 for devices without pressure
	Vec2 tilt{ 0.f };                           // degrees, -90..90 (pen only)
	uint64_t timestampNs = 0;                   // SDL event timestamp (SDL_GetTicksNS clock)
};

struct WheelEvent
{
	Vec2 delta{ 0.f };    // scroll amount (positive y = away from user, positive x = right)
	Vec2 position{ 0.f }; // window pixels
	uint8_t modifiers = 0;
	bool precise = false; // fractional deltas (touchpad / high-resolution wheel)
	uint64_t timestampNs = 0;
};

using InputEvent = std::variant<PointerEvent, WheelEvent>;

// Live pen state, kept for diagnostics (F3) and for stamping axis values onto events.
struct PenState
{
	SDL_PenID id = 0;
	bool inProximity = false;
	bool down = false;
	bool eraser = false;
	float pressure = 0.f;
	Vec2 tilt{ 0.f };
	float rotation = 0.f;
	Vec2 position{ 0.f }; // window points
	uint8_t buttons = 0;
	uint64_t eventCount = 0;
	uint64_t lastTimestampNs = 0;
	double eventsPerSecond = 0.0;
};

struct InputStats
{
	uint64_t pointerEvents = 0;
	uint64_t droppedSyntheticMouse = 0;
	PointerDevice lastDevice = PointerDevice::Mouse;
};

class InputRouter
{
public:
	// Call once before SDL_Init: disables SDL's pen/touch -> mouse emulation.
	static void configureHints();

	// Translates one SDL event. Returns nothing for events that are not pointer/wheel input (or are filtered).
	[[nodiscard]] std::optional<InputEvent> translate(const SDL_Event& p_Event, float p_PixelDensity);

	[[nodiscard]] const PenState& penState() const { return m_Pen; }
	[[nodiscard]] const InputStats& stats() const { return m_Stats; }

private:
	[[nodiscard]] static uint8_t currentModifiers();
	void updatePenRate(uint64_t p_TimestampNs);
	[[nodiscard]] PointerEvent makePenEvent(PointerPhase p_Phase, Vec2 p_Points, uint64_t p_TimestampNs, float p_PixelDensity) const;

	PenState m_Pen{};
	InputStats m_Stats{};
	uint8_t m_MouseButtons = 0;
	SDL_FingerID m_ActiveFinger = 0;
	bool m_FingerActive = false;
	uint64_t m_RateWindowStartNs = 0;
	uint64_t m_RateWindowCount = 0;
};
} // namespace wb::platform
