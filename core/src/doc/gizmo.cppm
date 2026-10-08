// The transform box shown around a selection and the maths behind dragging its handles.
//
// The box (SelectionFrame) is an oriented rectangle: it starts axis-aligned around the selection and then follows
// the rotations the user applies, so a rotated group keeps scaling along its own axes.
module;
#include <cstdint>
#include <glm/glm.hpp>

export module wb.doc.gizmo;

import wb.math;

export namespace wb
{
enum class Handle : uint8_t
{
	None,
	// Scale handles, clockwise from the top (screen orientation: y grows downwards)
	N,
	NE,
	E,
	SE,
	S,
	SW,
	W,
	NW,
	Rotate,
};

inline constexpr int SCALE_HANDLE_COUNT = 8;

// Handle i (0 = N ... 7 = NW) as a Handle value
[[nodiscard]] constexpr Handle scaleHandle(const int p_Index)
{
	return static_cast<Handle>(static_cast<int>(Handle::N) + p_Index);
}

[[nodiscard]] constexpr bool isScaleHandle(const Handle p_Handle)
{
	return p_Handle >= Handle::N && p_Handle <= Handle::NW;
}

// Position of a scale handle on the box in box units: components in {-1, 0, 1} (y = -1 is the top edge)
[[nodiscard]] DVec2 handleDirection(Handle p_Handle);

struct SelectionFrame
{
	DVec2 center{ 0.0 };
	double angle = 0.0;      // radians, counter-clockwise in world space (y up) / clockwise on screen
	DVec2 half{ 0.0 };       // half extents along the box's own axes

	[[nodiscard]] static SelectionFrame fromBounds(const Rect& p_Bounds)
	{
		return SelectionFrame{ .center = p_Bounds.center(), .angle = 0.0, .half = p_Bounds.size() * 0.5 };
	}

	// Box-local offset from the centre -> world, and back
	[[nodiscard]] Affine2 localToWorld() const { return Affine2::translate(center) * Affine2::rotate(angle); }
	[[nodiscard]] DVec2 toWorld(const DVec2 p_Local) const { return localToWorld().apply(p_Local); }
	[[nodiscard]] DVec2 toLocal(const DVec2 p_World) const { return Affine2::rotate(-angle).apply(p_World - center); }

	// World position of a scale handle sitting p_Padding (world units) outside the box edge
	[[nodiscard]] DVec2 handlePosition(const Handle p_Handle, const double p_Padding = 0.0) const
	{
		const DVec2 l_Direction = handleDirection(p_Handle);
		return toWorld(l_Direction * (half + DVec2{ p_Padding }));
	}
};

struct ResizeOptions
{
	bool keepAspect = false; // corners and edges scale both axes by the same factor
	bool fromCenter = false; // the centre stays fixed instead of the opposite edge
};

struct FrameTransform
{
	Affine2 transform{};     // world transform to apply to the selected objects
	SelectionFrame frame{};  // the box after applying it
};

// Dragging scale handle p_Handle so that the handle's own position moves to p_Target (world)
[[nodiscard]] FrameTransform resizeFrame(const SelectionFrame& p_Frame, Handle p_Handle, DVec2 p_Target, const ResizeOptions& p_Options);

// Rotates the box (and what it holds) around its centre
[[nodiscard]] FrameTransform rotateFrame(const SelectionFrame& p_Frame, double p_DeltaRadians);

// Moves the box
[[nodiscard]] FrameTransform translateFrame(const SelectionFrame& p_Frame, DVec2 p_Delta);

// Rounds p_Radians to a multiple of p_Step
[[nodiscard]] double snapAngle(double p_Radians, double p_Step);
// Wraps into (-pi, pi]
[[nodiscard]] double wrapAngle(double p_Radians);
} // namespace wb
