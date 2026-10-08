// Vector eraser geometry. Strokes are never rasterized: the eraser is a capsule (a swept disc) in world space and
// the stroke is cut exactly where its outline touches that capsule.
//
// Per segment, "touching" is a convex condition in the segment parameter t (distance to the eraser capsule is
// convex, the stroke half-width is linear), so the erased part of a segment is a single interval whose ends are
// found by bisection. Cuts are therefore exact and not snapped to the stroke's points.
module;
#include <optional>
#include <vector>

export module wb.brush.eraser;

import wb.math;
import wb.doc.object;

export namespace wb
{
// Swept eraser disc in world space; a == b is a single press
struct EraserCapsule
{
	DVec2 a{ 0.0 };
	DVec2 b{ 0.0 };
	double radius = 1.0;

	[[nodiscard]] Rect bounds() const { return Rect::fromPoints(a, b).inflated(radius); }
};

// True when the eraser touches any part of the stroke (whole-stroke mode)
[[nodiscard]] bool strokeTouches(const Object& p_Object, const EraserCapsule& p_Eraser);

// Cuts the stroke (segment mode). Returns nullopt when untouched; otherwise the surviving pieces, in the object's
// local space with the object's style (possibly none when everything was erased).
[[nodiscard]] std::optional<std::vector<StrokeData>> cutStroke(const Object& p_Object, const EraserCapsule& p_Eraser);
} // namespace wb
