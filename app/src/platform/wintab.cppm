// WinTab fallback for graphics tablets whose driver has Windows Ink turned off (Windows only; an inert stub elsewhere).
//
// Without Windows Ink the driver only emulates a mouse, so SDL never sees a pen. WinTab (Wintab32.dll, loaded
// dynamically so nothing is required at build or run time) still reports pressure, tilt and the eraser end. Only those
// axes are taken from it: the position and the buttons keep coming from the driver's mouse events, which are already in
// the right window coordinates whatever the tablet mapping or monitor layout.
module;
#include <cstdint>
#include <memory>
#include <SDL3/SDL.h>

export module wb.platform.wintab;

import wb.math;

export namespace wb::platform
{
struct TabletSample
{
	float pressure = 0.f; // 0..1
	Vec2 tilt{ 0.f };     // degrees, -90..90; zero when the tablet reports no orientation
	bool eraser = false;  // the pen is held with its eraser end
};

class Wintab
{
public:
	Wintab();
	~Wintab();
	Wintab(const Wintab&) = delete;
	Wintab& operator=(const Wintab&) = delete;

	// Opens a context on the window. False when there is no WinTab driver or no pressure-sensitive tablet.
	bool open(SDL_Window* p_Window);
	void close();
	[[nodiscard]] bool isOpen() const;

	// Brings the context to the front while the window has focus
	void setFocused(bool p_Focused);

	// Drains the queued packets into sample(). True when at least one arrived.
	bool poll(uint64_t p_NowNs);

	[[nodiscard]] const TabletSample& sample() const { return m_Sample; }
	[[nodiscard]] uint64_t lastPacketNs() const { return m_LastPacketNs; }

private:
	struct Impl;
	std::unique_ptr<Impl> m_Impl;
	TabletSample m_Sample{};
	uint64_t m_LastPacketNs = 0;
};
} // namespace wb::platform
