module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <SDL3/SDL.h>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

module wb.platform.wintab;

namespace wb::platform
{
#if defined(_WIN32)
namespace
{
// Minimal subset of wintab.h / wintab32.dll (the official SDK headers carry a licence, so the few pieces needed are declared here)
using HCTX = HANDLE;
using WTPKT = DWORD;
using FIX32 = DWORD;

struct WtAxis
{
	LONG min;
	LONG max;
	UINT units;
	FIX32 resolution;
};

struct WtLogContext
{
	WCHAR name[40];
	UINT options;
	UINT status;
	UINT locks;
	UINT msgBase;
	UINT device;
	UINT pktRate;
	WTPKT pktData;
	WTPKT pktMode;
	WTPKT moveMask;
	DWORD btnDnMask;
	DWORD btnUpMask;
	LONG inOrgX, inOrgY, inOrgZ;
	LONG inExtX, inExtY, inExtZ;
	LONG outOrgX, outOrgY, outOrgZ;
	LONG outExtX, outExtY, outExtZ;
	FIX32 sensX, sensY, sensZ;
	BOOL sysMode;
	int sysOrgX, sysOrgY;
	int sysExtX, sysExtY;
	FIX32 sysSensX, sysSensY;
};

// Packet layout follows the PK_* bit order of the requested data (all fields are 4 bytes)
struct WtPacket
{
	UINT status;   // PK_STATUS
	LONG x;        // PK_X
	LONG y;        // PK_Y
	UINT pressure; // PK_NORMAL_PRESSURE
	int azimuth;   // PK_ORIENTATION: tenths of a degree, clockwise from the top of the tablet
	int altitude;  // tenths of a degree above the tablet surface (900 = upright), negative when inverted
	int twist;
};

constexpr UINT WTI_DEVICES = 100;
constexpr UINT WTI_DEFSYSCTX = 4;
constexpr UINT DVC_NPRESSURE = 15;
constexpr UINT DVC_ORIENTATION = 17;
constexpr WTPKT PK_STATUS = 0x0002;
constexpr WTPKT PK_X = 0x0080;
constexpr WTPKT PK_Y = 0x0100;
constexpr WTPKT PK_NORMAL_PRESSURE = 0x0400;
constexpr WTPKT PK_ORIENTATION = 0x1000;
constexpr WTPKT PACKET_DATA = PK_STATUS | PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION;
constexpr UINT TPS_INVERT = 0x0010;
constexpr int QUEUE_SIZE = 256;
constexpr int PACKETS_PER_POLL = 64;

constexpr float RAD_TO_DEG = 57.29577951308232f;
constexpr float DEG_TO_RAD = 0.017453292519943295f;

using WTInfoW_t = UINT(WINAPI*)(UINT, UINT, LPVOID);
using WTOpenW_t = HCTX(WINAPI*)(HWND, WtLogContext*, BOOL);
using WTClose_t = BOOL(WINAPI*)(HCTX);
using WTPacketsGet_t = int(WINAPI*)(HCTX, int, LPVOID);
using WTQueueSizeSet_t = BOOL(WINAPI*)(HCTX, int);
using WTEnable_t = BOOL(WINAPI*)(HCTX, BOOL);
using WTOverlap_t = BOOL(WINAPI*)(HCTX, BOOL);

template <typename T>
T procedure(const HMODULE p_Library, const char* p_Name)
{
	return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(p_Library, p_Name)));
}
} // namespace

struct Wintab::Impl
{
	HMODULE library = nullptr;
	HCTX context = nullptr;
	WTInfoW_t info = nullptr;
	WTOpenW_t open = nullptr;
	WTClose_t close = nullptr;
	WTPacketsGet_t packetsGet = nullptr;
	WTQueueSizeSet_t queueSizeSet = nullptr;
	WTEnable_t enable = nullptr;
	WTOverlap_t overlap = nullptr;
	float maxPressure = 1.f;
	bool hasOrientation = false;
	bool loadFailed = false;

	bool load()
	{
		if (library != nullptr)
			return true;
		if (loadFailed)
			return false;
		library = LoadLibraryW(L"Wintab32.dll");
		if (library != nullptr)
		{
			info = procedure<WTInfoW_t>(library, "WTInfoW");
			open = procedure<WTOpenW_t>(library, "WTOpenW");
			close = procedure<WTClose_t>(library, "WTClose");
			packetsGet = procedure<WTPacketsGet_t>(library, "WTPacketsGet");
			queueSizeSet = procedure<WTQueueSizeSet_t>(library, "WTQueueSizeSet");
			enable = procedure<WTEnable_t>(library, "WTEnable");
			overlap = procedure<WTOverlap_t>(library, "WTOverlap");
		}
		if (library == nullptr || info == nullptr || open == nullptr || close == nullptr || packetsGet == nullptr || queueSizeSet == nullptr || enable == nullptr || overlap == nullptr)
		{
			if (library != nullptr)
				FreeLibrary(library);
			library = nullptr;
			loadFailed = true;
			return false;
		}
		return true;
	}

	void release()
	{
		if (context != nullptr)
			close(context);
		context = nullptr;
	}
};

Wintab::Wintab() : m_Impl(std::make_unique<Impl>()) {}

Wintab::~Wintab()
{
	close();
	if (m_Impl->library != nullptr)
		FreeLibrary(m_Impl->library);
}

bool Wintab::open(SDL_Window* p_Window)
{
	if (isOpen())
		return true;
	if (p_Window == nullptr || !m_Impl->load())
		return false;

	const HWND l_Hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(p_Window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (l_Hwnd == nullptr || m_Impl->info(0, 0, nullptr) == 0) // no driver installed
		return false;

	WtLogContext l_Context{};
	if (m_Impl->info(WTI_DEFSYSCTX, 0, &l_Context) == 0)
		return false;

	// Only tablets that report pressure are worth taking over from the mouse
	const UINT l_Device = static_cast<int>(l_Context.device) < 0 ? 0u : l_Context.device;
	WtAxis l_Pressure{};
	if (m_Impl->info(WTI_DEVICES + l_Device, DVC_NPRESSURE, &l_Pressure) == 0 || l_Pressure.max <= 0)
		return false;
	WtAxis l_Orientation[3]{};
	m_Impl->hasOrientation = m_Impl->info(WTI_DEVICES + l_Device, DVC_ORIENTATION, l_Orientation) != 0 && l_Orientation[1].resolution != 0;
	m_Impl->maxPressure = static_cast<float>(l_Pressure.max);

	// A system context keeps the cursor moving like the driver's mouse emulation; only the packet contents change
	l_Context.pktData = PACKET_DATA;
	l_Context.pktMode = 0; // absolute
	l_Context.moveMask = PACKET_DATA;
	l_Context.btnDnMask = 0;
	l_Context.btnUpMask = 0;
	lstrcpynW(l_Context.name, L"Inkwell", 40);

	m_Impl->context = m_Impl->open(l_Hwnd, &l_Context, TRUE);
	if (m_Impl->context == nullptr)
		return false;
	m_Impl->queueSizeSet(m_Impl->context, QUEUE_SIZE);
	m_Sample = {};
	m_LastPacketNs = 0;
	SDL_Log("WinTab tablet enabled (pressure 0..%ld%s)", l_Pressure.max, m_Impl->hasOrientation ? ", tilt" : "");
	return true;
}

void Wintab::close()
{
	m_Impl->release();
	m_Sample = {};
}

bool Wintab::isOpen() const
{
	return m_Impl->context != nullptr;
}

void Wintab::setFocused(const bool p_Focused)
{
	if (!isOpen())
		return;
	m_Impl->enable(m_Impl->context, p_Focused ? TRUE : FALSE);
	if (p_Focused)
		m_Impl->overlap(m_Impl->context, TRUE);
}

bool Wintab::poll(const uint64_t p_NowNs)
{
	if (!isOpen())
		return false;

	WtPacket l_Packets[PACKETS_PER_POLL];
	bool l_Any = false;
	for (;;)
	{
		const int l_Count = m_Impl->packetsGet(m_Impl->context, PACKETS_PER_POLL, l_Packets);
		if (l_Count <= 0)
			break;
		const WtPacket& l_Last = l_Packets[l_Count - 1];
		l_Any = true;

		m_Sample.pressure = std::clamp(static_cast<float>(l_Last.pressure) / m_Impl->maxPressure, 0.f, 1.f);
		m_Sample.eraser = (l_Last.status & TPS_INVERT) != 0;
		m_Sample.tilt = Vec2{ 0.f };
		if (m_Impl->hasOrientation && l_Last.altitude != 0)
		{
			// Altitude is measured from the surface, azimuth clockwise from the top; SDL's tilt is per axis from the vertical
			const float l_FromVertical = std::clamp(90.f - std::abs(static_cast<float>(l_Last.altitude)) * 0.1f, 0.f, 89.f);
			const float l_Azimuth = static_cast<float>(l_Last.azimuth) * 0.1f * DEG_TO_RAD;
			const float l_Reach = std::tan(l_FromVertical * DEG_TO_RAD);
			m_Sample.tilt = Vec2{ std::atan(l_Reach * std::sin(l_Azimuth)) * RAD_TO_DEG, std::atan(-l_Reach * std::cos(l_Azimuth)) * RAD_TO_DEG };
		}
		if (l_Count < PACKETS_PER_POLL)
			break;
	}
	if (l_Any)
		m_LastPacketNs = p_NowNs;
	return l_Any;
}

#else // not Windows: SDL's own pen support is all there is

struct Wintab::Impl
{
};

Wintab::Wintab() : m_Impl(std::make_unique<Impl>()) {}
Wintab::~Wintab() = default;
bool Wintab::open(SDL_Window*) { return false; }
void Wintab::close() {}
bool Wintab::isOpen() const { return false; }
void Wintab::setFocused(bool) {}
bool Wintab::poll(uint64_t) { return false; }
#endif
} // namespace wb::platform
