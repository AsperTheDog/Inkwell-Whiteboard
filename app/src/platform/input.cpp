module;
#include <cstdint>
#include <optional>
#include <variant>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

module wb.platform.input;

import wb.math;

namespace wb::platform
{
namespace
{
uint8_t penStateToMask(const SDL_PenInputFlags p_State)
{
	uint8_t l_Mask = 0;
	if ((p_State & SDL_PEN_INPUT_DOWN) != 0)
		l_Mask |= ButtonMask::Primary;
	if ((p_State & SDL_PEN_INPUT_BUTTON_1) != 0)
		l_Mask |= ButtonMask::Secondary;
	if ((p_State & SDL_PEN_INPUT_BUTTON_2) != 0)
		l_Mask |= ButtonMask::Middle;
	return l_Mask;
}

std::optional<PointerButton> mouseButton(const Uint8 p_Button)
{
	switch (p_Button)
	{
	case SDL_BUTTON_LEFT:
		return PointerButton::Primary;
	case SDL_BUTTON_RIGHT:
		return PointerButton::Secondary;
	case SDL_BUTTON_MIDDLE:
		return PointerButton::Middle;
	default:
		return std::nullopt;
	}
}

uint8_t buttonToMask(const PointerButton p_Button)
{
	switch (p_Button)
	{
	case PointerButton::Primary:
		return ButtonMask::Primary;
	case PointerButton::Secondary:
		return ButtonMask::Secondary;
	case PointerButton::Middle:
		return ButtonMask::Middle;
	case PointerButton::None:
		break;
	}
	return 0;
}

// SDL numbers barrel buttons from 1
PointerButton penBarrelButton(const Uint8 p_Button)
{
	switch (p_Button)
	{
	case 1:
		return PointerButton::Secondary;
	case 2:
		return PointerButton::Middle;
	default:
		return PointerButton::None;
	}
}

bool isSyntheticMouse(const SDL_MouseID p_Which)
{
	return p_Which == SDL_PEN_MOUSEID || p_Which == SDL_TOUCH_MOUSEID;
}
} // namespace

void InputRouter::configureHints()
{
	SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
	SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
}

uint8_t InputRouter::currentModifiers()
{
	const SDL_Keymod l_Mod = SDL_GetModState();
	uint8_t l_Mask = 0;
	if ((l_Mod & SDL_KMOD_SHIFT) != 0)
		l_Mask |= Modifier::Shift;
	if ((l_Mod & SDL_KMOD_CTRL) != 0)
		l_Mask |= Modifier::Ctrl;
	if ((l_Mod & SDL_KMOD_ALT) != 0)
		l_Mask |= Modifier::Alt;
	return l_Mask;
}

void InputRouter::updatePenRate(const uint64_t p_TimestampNs)
{
	++m_Pen.eventCount;
	m_Pen.lastTimestampNs = p_TimestampNs;
	if (m_RateWindowStartNs == 0 || p_TimestampNs < m_RateWindowStartNs)
		m_RateWindowStartNs = p_TimestampNs;
	++m_RateWindowCount;
	const uint64_t l_Elapsed = p_TimestampNs - m_RateWindowStartNs;
	if (l_Elapsed >= 500'000'000ull)
	{
		m_Pen.eventsPerSecond = static_cast<double>(m_RateWindowCount) * 1e9 / static_cast<double>(l_Elapsed);
		m_RateWindowStartNs = p_TimestampNs;
		m_RateWindowCount = 0;
	}
}

PointerEvent InputRouter::makePenEvent(const PointerPhase p_Phase, const Vec2 p_Points, const uint64_t p_TimestampNs, const float p_PixelDensity) const
{
	return PointerEvent{
		.phase = p_Phase,
		.device = PointerDevice::Pen,
		.button = PointerButton::None,
		.buttons = m_Pen.buttons,
		.modifiers = currentModifiers(),
		.eraser = m_Pen.eraser,
		.position = p_Points * p_PixelDensity,
		.pressure = m_Pen.pressure,
		.tilt = m_Pen.tilt,
		.timestampNs = p_TimestampNs,
	};
}

std::optional<InputEvent> InputRouter::translate(const SDL_Event& p_Event, const float p_PixelDensity)
{
	switch (p_Event.type)
	{
	// ---------------------------------------------------------------- pen
	case SDL_EVENT_PEN_PROXIMITY_IN:
		m_Pen.id = p_Event.pproximity.which;
		m_Pen.inProximity = true;
		return std::nullopt;

	case SDL_EVENT_PEN_PROXIMITY_OUT:
	{
		m_Pen.inProximity = false;
		if (!m_Pen.down)
			return std::nullopt;

		// Lifted out of range without a proper up event
		m_Pen.down = false;
		m_Pen.buttons = 0;
		++m_Stats.pointerEvents;
		return makePenEvent(PointerPhase::Cancel, m_Pen.position, p_Event.pproximity.timestamp, p_PixelDensity);
	}

	case SDL_EVENT_PEN_AXIS:
	{
		const SDL_PenAxisEvent& l_Axis = p_Event.paxis;
		m_Pen.id = l_Axis.which;
		switch (l_Axis.axis)
		{
		case SDL_PEN_AXIS_PRESSURE:
			m_Pen.pressure = l_Axis.value;
			break;
		case SDL_PEN_AXIS_XTILT:
			m_Pen.tilt.x = l_Axis.value;
			break;
		case SDL_PEN_AXIS_YTILT:
			m_Pen.tilt.y = l_Axis.value;
			break;
		case SDL_PEN_AXIS_ROTATION:
			m_Pen.rotation = l_Axis.value;
			break;
		default:
			break;
		}
		updatePenRate(l_Axis.timestamp);
		return std::nullopt;
	}

	case SDL_EVENT_PEN_DOWN:
	case SDL_EVENT_PEN_UP:
	{
		const SDL_PenTouchEvent& l_Touch = p_Event.ptouch;
		const bool l_Down = p_Event.type == SDL_EVENT_PEN_DOWN;
		m_Pen.id = l_Touch.which;
		m_Pen.down = l_Down;
		m_Pen.eraser = l_Touch.eraser;
		m_Pen.position = Vec2{ l_Touch.x, l_Touch.y };
		m_Pen.buttons = penStateToMask(l_Touch.pen_state);
		if (l_Down)
			m_Pen.buttons |= ButtonMask::Primary;
		else
			m_Pen.buttons &= static_cast<uint8_t>(~ButtonMask::Primary);
		updatePenRate(l_Touch.timestamp);

		PointerEvent l_Event = makePenEvent(l_Down ? PointerPhase::Down : PointerPhase::Up, m_Pen.position, l_Touch.timestamp, p_PixelDensity);
		l_Event.button = PointerButton::Primary;
		++m_Stats.pointerEvents;
		m_Stats.lastDevice = PointerDevice::Pen;
		return l_Event;
	}

	case SDL_EVENT_PEN_BUTTON_DOWN:
	case SDL_EVENT_PEN_BUTTON_UP:
	{
		const SDL_PenButtonEvent& l_Button = p_Event.pbutton;
		const PointerButton l_Which = penBarrelButton(l_Button.button);
		const bool l_Down = p_Event.type == SDL_EVENT_PEN_BUTTON_DOWN;
		m_Pen.id = l_Button.which;
		m_Pen.position = Vec2{ l_Button.x, l_Button.y };
		m_Pen.eraser = (l_Button.pen_state & SDL_PEN_INPUT_ERASER_TIP) != 0;
		m_Pen.buttons = penStateToMask(l_Button.pen_state);
		if (l_Which != PointerButton::None)
		{
			if (l_Down)
				m_Pen.buttons |= buttonToMask(l_Which);
			else
				m_Pen.buttons &= static_cast<uint8_t>(~buttonToMask(l_Which));
		}
		updatePenRate(l_Button.timestamp);

		// While the tip is down the barrel buttons are only reported through the held-buttons mask
		if (l_Which == PointerButton::None || m_Pen.down)
			return std::nullopt;

		PointerEvent l_Event = makePenEvent(l_Down ? PointerPhase::Down : PointerPhase::Up, m_Pen.position, l_Button.timestamp, p_PixelDensity);
		l_Event.button = l_Which;
		++m_Stats.pointerEvents;
		m_Stats.lastDevice = PointerDevice::Pen;
		return l_Event;
	}

	case SDL_EVENT_PEN_MOTION:
	{
		const SDL_PenMotionEvent& l_Motion = p_Event.pmotion;
		m_Pen.id = l_Motion.which;
		m_Pen.position = Vec2{ l_Motion.x, l_Motion.y };
		m_Pen.eraser = (l_Motion.pen_state & SDL_PEN_INPUT_ERASER_TIP) != 0;
		m_Pen.inProximity = true;
		updatePenRate(l_Motion.timestamp);
		++m_Stats.pointerEvents;
		m_Stats.lastDevice = PointerDevice::Pen;
		return makePenEvent(PointerPhase::Move, m_Pen.position, l_Motion.timestamp, p_PixelDensity);
	}

	// ---------------------------------------------------------------- mouse
	case SDL_EVENT_MOUSE_MOTION:
	{
		const SDL_MouseMotionEvent& l_Motion = p_Event.motion;
		if (isSyntheticMouse(l_Motion.which))
		{
			++m_Stats.droppedSyntheticMouse;
			return std::nullopt;
		}
		++m_Stats.pointerEvents;
		m_Stats.lastDevice = PointerDevice::Mouse;
		return PointerEvent{
			.phase = PointerPhase::Move,
			.device = PointerDevice::Mouse,
			.buttons = m_MouseButtons,
			.modifiers = currentModifiers(),
			.position = Vec2{ l_Motion.x, l_Motion.y } * p_PixelDensity,
			.timestampNs = l_Motion.timestamp,
		};
	}

	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	{
		const SDL_MouseButtonEvent& l_Button = p_Event.button;
		if (isSyntheticMouse(l_Button.which))
		{
			++m_Stats.droppedSyntheticMouse;
			return std::nullopt;
		}
		const std::optional<PointerButton> l_Which = mouseButton(l_Button.button);
		if (!l_Which)
			return std::nullopt;

		const bool l_Down = p_Event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
		if (l_Down)
			m_MouseButtons |= buttonToMask(*l_Which);
		else
			m_MouseButtons &= static_cast<uint8_t>(~buttonToMask(*l_Which));

		++m_Stats.pointerEvents;
		m_Stats.lastDevice = PointerDevice::Mouse;
		return PointerEvent{
			.phase = l_Down ? PointerPhase::Down : PointerPhase::Up,
			.device = PointerDevice::Mouse,
			.button = *l_Which,
			.buttons = m_MouseButtons,
			.modifiers = currentModifiers(),
			.position = Vec2{ l_Button.x, l_Button.y } * p_PixelDensity,
			.timestampNs = l_Button.timestamp,
		};
	}

	case SDL_EVENT_MOUSE_WHEEL:
	{
		const SDL_MouseWheelEvent& l_Wheel = p_Event.wheel;
		if (isSyntheticMouse(l_Wheel.which))
			return std::nullopt;
		const float l_Sign = l_Wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
		const Vec2 l_Delta{ l_Wheel.x * l_Sign, l_Wheel.y * l_Sign };
		const bool l_Precise = l_Wheel.integer_x == 0 && l_Wheel.integer_y == 0 && (l_Delta.x != 0.f || l_Delta.y != 0.f);
		return WheelEvent{
			.delta = l_Delta,
			.position = Vec2{ l_Wheel.mouse_x, l_Wheel.mouse_y } * p_PixelDensity,
			.modifiers = currentModifiers(),
			.precise = l_Precise,
			.timestampNs = l_Wheel.timestamp,
		};
	}

	// ---------------------------------------------------------------- touch (first finger acts as a pointer)
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
	{
		const SDL_TouchFingerEvent& l_Finger = p_Event.tfinger;
		if (SDL_GetTouchDeviceType(l_Finger.touchID) != SDL_TOUCH_DEVICE_DIRECT)
			return std::nullopt; // touchpads arrive as mouse/wheel events

		PointerPhase l_Phase = PointerPhase::Move;
		if (p_Event.type == SDL_EVENT_FINGER_DOWN)
		{
			if (m_FingerActive)
				return std::nullopt; // multi-touch gestures come later; only the first finger is a pointer
			m_FingerActive = true;
			m_ActiveFinger = l_Finger.fingerID;
			l_Phase = PointerPhase::Down;
		}
		else
		{
			if (!m_FingerActive || l_Finger.fingerID != m_ActiveFinger)
				return std::nullopt;
			if (p_Event.type == SDL_EVENT_FINGER_UP)
				l_Phase = PointerPhase::Up;
			else if (p_Event.type == SDL_EVENT_FINGER_CANCELED)
				l_Phase = PointerPhase::Cancel;
			if (l_Phase != PointerPhase::Move)
				m_FingerActive = false;
		}

		// Finger coordinates are normalized to the window
		int l_Width = 0;
		int l_Height = 0;
		if (SDL_Window* l_Window = SDL_GetWindowFromID(l_Finger.windowID); l_Window != nullptr)
			SDL_GetWindowSize(l_Window, &l_Width, &l_Height);
		const Vec2 l_Points{ l_Finger.x * static_cast<float>(l_Width), l_Finger.y * static_cast<float>(l_Height) };

		++m_Stats.pointerEvents;
		m_Stats.lastDevice = PointerDevice::Touch;
		return PointerEvent{
			.phase = l_Phase,
			.device = PointerDevice::Touch,
			.button = l_Phase == PointerPhase::Move ? PointerButton::None : PointerButton::Primary,
			.buttons = m_FingerActive ? ButtonMask::Primary : uint8_t{ 0 },
			.modifiers = currentModifiers(),
			.position = l_Points * p_PixelDensity,
			.pressure = l_Finger.pressure > 0.f ? l_Finger.pressure : 1.f,
			.timestampNs = l_Finger.timestamp,
		};
	}

	default:
		return std::nullopt;
	}
}
} // namespace wb::platform
