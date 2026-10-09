// Smoothing of a stroke that is already drawn: every point moves towards the average of its neighbours, weighted by
// how far along the stroke they are (a Gaussian over arc length). The ends stay where they are.
module;
#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

export module wb.brush.smoothing;

import wb.math;
import wb.doc.object;

export namespace wb
{
inline constexpr size_t MIN_SMOOTHABLE_POINTS = 6;

// How far (world units, along the stroke) the smoothing reaches for a stroke of this thickness
[[nodiscard]] inline double smoothingReach(const StrokeData& p_Stroke)
{
	double l_Sum = 0.0;
	for (const StrokePoint& l_Point : p_Stroke.points)
		l_Sum += static_cast<double>(l_Point.radius);
	const double l_Mean = p_Stroke.points.empty() ? 0.0 : l_Sum / static_cast<double>(p_Stroke.points.size());
	return 5.0 * l_Mean + 2.0;
}

// The points after one smoothing pass; empty when the stroke is too short to smooth
[[nodiscard]] inline std::vector<StrokePoint> smoothedPoints(const std::vector<StrokePoint>& p_Points, const double p_Sigma)
{
	const size_t l_Count = p_Points.size();
	if (l_Count < MIN_SMOOTHABLE_POINTS || p_Sigma <= 0.0)
		return {};

	std::vector<double> l_Arc(l_Count, 0.0);
	for (size_t i = 1; i < l_Count; ++i)
		l_Arc[i] = l_Arc[i - 1] + static_cast<double>(glm::length(p_Points[i].position - p_Points[i - 1].position));

	std::vector<StrokePoint> l_Result = p_Points;
	const double l_Reach = 3.0 * p_Sigma;
	const double l_TwoSigmaSquared = 2.0 * p_Sigma * p_Sigma;
	for (size_t i = 1; i + 1 < l_Count; ++i)
	{
		DVec2 l_Position{ 0.0 };
		double l_Radius = 0.0;
		double l_Weights = 0.0;
		for (size_t j = i; j-- > 0 && l_Arc[i] - l_Arc[j] <= l_Reach;)
		{
			const double l_Distance = l_Arc[i] - l_Arc[j];
			const double l_Weight = std::exp(-l_Distance * l_Distance / l_TwoSigmaSquared);
			l_Position += DVec2{ p_Points[j].position } * l_Weight;
			l_Radius += static_cast<double>(p_Points[j].radius) * l_Weight;
			l_Weights += l_Weight;
		}
		for (size_t j = i; j < l_Count && l_Arc[j] - l_Arc[i] <= l_Reach; ++j)
		{
			const double l_Distance = l_Arc[j] - l_Arc[i];
			const double l_Weight = std::exp(-l_Distance * l_Distance / l_TwoSigmaSquared);
			l_Position += DVec2{ p_Points[j].position } * l_Weight;
			l_Radius += static_cast<double>(p_Points[j].radius) * l_Weight;
			l_Weights += l_Weight;
		}
		l_Result[i].position = Vec2{ l_Position / l_Weights };
		l_Result[i].radius = static_cast<float>(l_Radius / l_Weights);
	}
	return l_Result;
}
} // namespace wb
