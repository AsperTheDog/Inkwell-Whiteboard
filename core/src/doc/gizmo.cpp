module;
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

module wb.doc.gizmo;

import wb.math;

namespace wb
{
namespace
{
constexpr double MIN_SCALE = 1e-3;
constexpr double MAX_SCALE = 1e6;
constexpr double MIN_HALF_EXTENT = 1e-9;
} // namespace

DVec2 handleDirection(const Handle p_Handle)
{
	switch (p_Handle)
	{
	case Handle::N:
		return { 0.0, -1.0 };
	case Handle::NE:
		return { 1.0, -1.0 };
	case Handle::E:
		return { 1.0, 0.0 };
	case Handle::SE:
		return { 1.0, 1.0 };
	case Handle::S:
		return { 0.0, 1.0 };
	case Handle::SW:
		return { -1.0, 1.0 };
	case Handle::W:
		return { -1.0, 0.0 };
	case Handle::NW:
		return { -1.0, -1.0 };
	default:
		return { 0.0, 0.0 };
	}
}

FrameTransform resizeFrame(const SelectionFrame& p_Frame, const Handle p_Handle, const DVec2 p_Target, const ResizeOptions& p_Options)
{
	const DVec2 l_Direction = handleDirection(p_Handle);
	if (!isScaleHandle(p_Handle))
		return FrameTransform{ .transform = Affine2{}, .frame = p_Frame };

	// Work in box units: the box centre is the origin and its axes are x and y
	const DVec2 l_Target = p_Frame.toLocal(p_Target);
	const DVec2 l_Handle = l_Direction * p_Frame.half;
	const DVec2 l_Pivot = p_Options.fromCenter ? DVec2{ 0.0 } : -l_Handle;

	const auto l_Factor = [&](const double p_Direction, const double p_Handle, const double p_Pivot, const double p_Target) -> double
	{
		if (p_Direction == 0.0)
			return 1.0;
		const double l_Span = p_Handle - p_Pivot;
		if (std::abs(l_Span) < MIN_HALF_EXTENT)
			return 1.0;
		return std::clamp((p_Target - p_Pivot) / l_Span, MIN_SCALE, MAX_SCALE);
	};
	double l_Kx = l_Factor(l_Direction.x, l_Handle.x, l_Pivot.x, l_Target.x);
	double l_Ky = l_Factor(l_Direction.y, l_Handle.y, l_Pivot.y, l_Target.y);

	if (p_Options.keepAspect)
	{
		// Follow whichever axis the pointer moved further along; an edge handle drags the other axis with it
		const double l_Uniform = l_Direction.x == 0.0 ? l_Ky : (l_Direction.y == 0.0 ? l_Kx : std::max(l_Kx, l_Ky));
		l_Kx = l_Uniform;
		l_Ky = l_Uniform;
	}

	// Scale about the pivot (in box space); an edge handle with locked aspect scales the free axis about the centre line
	const DVec2 l_PivotLocal{ l_Direction.x == 0.0 ? 0.0 : l_Pivot.x, l_Direction.y == 0.0 ? 0.0 : l_Pivot.y };
	const Affine2 l_ToWorld = p_Frame.localToWorld();
	const Affine2 l_Scale = Affine2::around(l_PivotLocal, Affine2::scale(DVec2{ l_Kx, l_Ky }));
	const Affine2 l_World = l_ToWorld * l_Scale * l_ToWorld.inverse();

	SelectionFrame l_Frame = p_Frame;
	l_Frame.center = l_World.apply(p_Frame.center);
	l_Frame.half = p_Frame.half * DVec2{ l_Kx, l_Ky };
	return FrameTransform{ .transform = l_World, .frame = l_Frame };
}

FrameTransform rotateFrame(const SelectionFrame& p_Frame, const double p_DeltaRadians)
{
	SelectionFrame l_Frame = p_Frame;
	l_Frame.angle = wrapAngle(p_Frame.angle + p_DeltaRadians);
	return FrameTransform{ .transform = Affine2::around(p_Frame.center, Affine2::rotate(p_DeltaRadians)), .frame = l_Frame };
}

FrameTransform translateFrame(const SelectionFrame& p_Frame, const DVec2 p_Delta)
{
	SelectionFrame l_Frame = p_Frame;
	l_Frame.center += p_Delta;
	return FrameTransform{ .transform = Affine2::translate(p_Delta), .frame = l_Frame };
}

double snapAngle(const double p_Radians, const double p_Step)
{
	return p_Step > 0.0 ? std::round(p_Radians / p_Step) * p_Step : p_Radians;
}

double wrapAngle(const double p_Radians)
{
	double l_Angle = std::fmod(p_Radians, 2.0 * PI);
	if (l_Angle > PI)
		l_Angle -= 2.0 * PI;
	else if (l_Angle <= -PI)
		l_Angle += 2.0 * PI;
	return l_Angle;
}
} // namespace wb
