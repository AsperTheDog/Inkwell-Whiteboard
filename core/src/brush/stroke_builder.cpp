module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.brush.stroke_builder;

import wb.math;
import wb.doc.object;
import wb.view.camera;

namespace wb
{
namespace
{
constexpr double MIN_DELTA_SECONDS = 1.0 / 2000.0;
constexpr double DEFAULT_DELTA_SECONDS = 1.0 / 120.0;

float smoothingAlpha(const double p_DeltaSeconds, const float p_CutoffHz)
{
	const double l_Tau = 1.0 / (2.0 * PI * static_cast<double>(p_CutoffHz));
	return static_cast<float>(1.0 / (1.0 + l_Tau / p_DeltaSeconds));
}

// Centripetal Catmull-Rom (alpha = 0.5) through p1 -> p2, evaluated at u in [0, 1]. Centripetal
// parameterization never forms cusps or loops on unevenly spaced input.
Vec2 catmullRom(const Vec2 p_P0, const Vec2 p_P1, const Vec2 p_P2, const Vec2 p_P3, const float p_U)
{
	const auto l_Knot = [](const Vec2 p_A, const Vec2 p_B, const float p_T)
	{
		return p_T + std::max(std::sqrt(glm::length(p_B - p_A)), 1e-4f);
	};
	const float l_T0 = 0.f;
	const float l_T1 = l_Knot(p_P0, p_P1, l_T0);
	const float l_T2 = l_Knot(p_P1, p_P2, l_T1);
	const float l_T3 = l_Knot(p_P2, p_P3, l_T2);
	const float l_T = l_T1 + (l_T2 - l_T1) * p_U;

	// Barry-Goldman pyramidal formulation
	const auto l_Lerp = [](const Vec2 p_A, const Vec2 p_B, const float p_Ta, const float p_Tb, const float p_X)
	{
		const float l_W = (p_X - p_Ta) / (p_Tb - p_Ta);
		return p_A * (1.f - l_W) + p_B * l_W;
	};
	const Vec2 l_A1 = l_Lerp(p_P0, p_P1, l_T0, l_T1, l_T);
	const Vec2 l_A2 = l_Lerp(p_P1, p_P2, l_T1, l_T2, l_T);
	const Vec2 l_A3 = l_Lerp(p_P2, p_P3, l_T2, l_T3, l_T);
	const Vec2 l_B1 = l_Lerp(l_A1, l_A2, l_T0, l_T2, l_T);
	const Vec2 l_B2 = l_Lerp(l_A2, l_A3, l_T1, l_T3, l_T);
	return l_Lerp(l_B1, l_B2, l_T1, l_T2, l_T);
}

// Smoothstep from 15% to 100% of the width over a taper
float taperFactor(const float p_X)
{
	const float l_X = std::clamp(p_X, 0.f, 1.f);
	return 0.15f + 0.85f * l_X * l_X * (3.f - 2.f * l_X);
}

// Perpendicular "distance" of p from segment a-b in (x, y, radius) space
float distanceToSegment3(const StrokePoint& p_P, const StrokePoint& p_A, const StrokePoint& p_B)
{
	const glm::vec3 l_P{ p_P.position, p_P.radius };
	const glm::vec3 l_A{ p_A.position, p_A.radius };
	const glm::vec3 l_B{ p_B.position, p_B.radius };
	const glm::vec3 l_Ab = l_B - l_A;
	const float l_LengthSq = glm::dot(l_Ab, l_Ab);
	const float l_T = l_LengthSq > 0.f ? std::clamp(glm::dot(l_P - l_A, l_Ab) / l_LengthSq, 0.f, 1.f) : 0.f;
	return glm::length(l_P - (l_A + l_Ab * l_T));
}
} // namespace

// ------------------------------------------------------------------------------------------------ OneEuroFilter

void OneEuroFilter::configure(const float p_MinCutoffHz, const float p_Beta, const float p_DerivativeCutoffHz)
{
	m_MinCutoff = std::max(p_MinCutoffHz, 1e-3f);
	m_Beta = std::max(p_Beta, 0.f);
	m_DerivativeCutoff = std::max(p_DerivativeCutoffHz, 1e-3f);
}

void OneEuroFilter::reset()
{
	m_Initialized = false;
	m_Derivative = Vec2{ 0.f };
}

Vec2 OneEuroFilter::filter(const Vec2 p_Value, const double p_DeltaSeconds)
{
	if (!m_Initialized)
	{
		m_Initialized = true;
		m_Value = p_Value;
		m_Derivative = Vec2{ 0.f };
		return p_Value;
	}

	const double l_Dt = std::max(p_DeltaSeconds, MIN_DELTA_SECONDS);
	const Vec2 l_RawDerivative = (p_Value - m_Value) / static_cast<float>(l_Dt);
	const float l_DerivativeAlpha = smoothingAlpha(l_Dt, m_DerivativeCutoff);
	m_Derivative = m_Derivative + (l_RawDerivative - m_Derivative) * l_DerivativeAlpha;

	const float l_Cutoff = m_MinCutoff + m_Beta * glm::length(m_Derivative);
	const float l_Alpha = smoothingAlpha(l_Dt, l_Cutoff);
	m_Value = m_Value + (p_Value - m_Value) * l_Alpha;
	return m_Value;
}

// ------------------------------------------------------------------------------------------------ StrokeBuilder

void StrokeBuilder::begin(const StrokeInput& p_First, const bool p_HasPressure, const float p_SizePoints, const Color p_Color, const Camera& p_Camera, const BrushSettings& p_Settings)
{
	m_Settings = p_Settings;
	m_Camera = p_Camera;
	m_HasPressure = p_HasPressure;
	m_Active = true;

	// Constant on-screen width at the current zoom
	const double l_SizeWorld = static_cast<double>(p_SizePoints) / p_Camera.zoom();
	m_Style = StrokeStyle{ .color = p_Color, .size = static_cast<float>(l_SizeWorld), .brush = BrushKind::Pen };
	m_RadiusWorldPerUnitWidth = l_SizeWorld * 0.5;
	m_Origin = p_Camera.screenToWorld(DVec2{ p_First.screen });

	m_PositionFilter.configure(m_Settings.smoothingMinCutoff, m_Settings.smoothingBeta, 1.f);
	m_PositionFilter.reset();
	m_SmoothedPressure = m_HasPressure ? p_First.pressure : 1.f;
	m_LastTimestampNs = p_First.timestampNs;
	m_LastRawScreen = p_First.screen;

	m_Samples.clear();
	m_Committed.clear();
	m_Tail.clear();
	m_CommittedSegments = 0;
	m_CommittedArc = 0.f;

	pushSample(p_First, true);
	m_Committed.push_back(toStrokePoint(m_Samples.front().screen, m_Samples.front().pressure));
	applyStartTaper(m_Committed, 0, m_CommittedArc);
	++m_Revision;
}

void StrokeBuilder::add(const StrokeInput& p_Input)
{
	if (!m_Active)
		return;
	pushSample(p_Input, true);
	commitReadySegments();
	rebuildTail();
	++m_Revision;
}

void StrokeBuilder::pushSample(const StrokeInput& p_Input, const bool p_Filter)
{
	double l_Dt = DEFAULT_DELTA_SECONDS;
	if (p_Input.timestampNs > m_LastTimestampNs)
		l_Dt = static_cast<double>(p_Input.timestampNs - m_LastTimestampNs) * 1e-9;
	m_LastTimestampNs = std::max(m_LastTimestampNs, p_Input.timestampNs);

	Vec2 l_Position = p_Filter ? m_PositionFilter.filter(p_Input.screen, l_Dt) : p_Input.screen;
	if (m_Settings.ropePx > 0.f)
	{
		if (m_Samples.empty())
		{
			m_Rope = l_Position;
		}
		else
		{
			const Vec2 l_Pull = l_Position - m_Rope;
			const float l_Length = glm::length(l_Pull);
			if (l_Length > m_Settings.ropePx)
				m_Rope += l_Pull / l_Length * (l_Length - m_Settings.ropePx);
		}
		l_Position = m_Rope;
	}

	float l_Pressure = 1.f;
	if (m_HasPressure)
	{
		const float l_Keep = std::clamp(m_Settings.pressureSmoothing, 0.f, 0.95f);
		m_SmoothedPressure = m_SmoothedPressure * l_Keep + std::clamp(p_Input.pressure, 0.f, 1.f) * (1.f - l_Keep);
		l_Pressure = m_SmoothedPressure;
	}
	else if (m_Settings.simulatePressureForMouse)
	{
		// Faster strokes get thinner, like ink spreading less
		const float l_Speed = glm::length(p_Input.screen - m_LastRawScreen) / static_cast<float>(std::max(l_Dt, MIN_DELTA_SECONDS));
		const float l_Target = std::clamp(1.f - l_Speed / 4000.f, 0.25f, 1.f);
		m_SmoothedPressure = m_SmoothedPressure * 0.7f + l_Target * 0.3f;
		l_Pressure = m_SmoothedPressure;
	}
	m_LastRawScreen = p_Input.screen;

	if (!m_Samples.empty() && glm::length(l_Position - m_Samples.back().screen) < m_Settings.minSampleDistancePx)
	{
		// Too close to be a new sample, but keep the freshest pressure
		m_Samples.back().pressure = l_Pressure;
		if (m_Samples.size() == 1 && m_Committed.size() == 1)
		{
			m_Committed.front() = toStrokePoint(m_Samples.front().screen, l_Pressure);
			float l_Arc = 0.f;
			applyStartTaper(m_Committed, 0, l_Arc);
		}
		return;
	}
	m_Samples.push_back(Sample{ .screen = l_Position, .pressure = l_Pressure });
}

float StrokeBuilder::widthFactor(const float p_Pressure) const
{
	const float l_MinWidth = std::clamp(m_Settings.minWidthFraction, 0.f, 1.f);
	const float l_Curve = std::pow(std::clamp(p_Pressure, 0.f, 1.f), std::max(m_Settings.pressureGamma, 0.05f));
	const float l_Sensitivity = std::clamp(m_Settings.pressureSensitivity, 0.f, 1.f);
	return 1.f + (l_MinWidth + (1.f - l_MinWidth) * l_Curve - 1.f) * l_Sensitivity;
}

StrokePoint StrokeBuilder::toStrokePoint(const Vec2 p_Screen, const float p_Pressure) const
{
	const DVec2 l_World = m_Camera.screenToWorld(DVec2{ p_Screen });
	return StrokePoint{
		.position = Vec2{ l_World - m_Origin },
		.radius = static_cast<float>(m_RadiusWorldPerUnitWidth * static_cast<double>(widthFactor(p_Pressure))),
	};
}

void StrokeBuilder::emitSegment(const size_t p_Index, std::vector<StrokePoint>& p_Out) const
{
	const size_t l_Count = m_Samples.size();
	const Sample& l_S1 = m_Samples[p_Index];
	const Sample& l_S2 = m_Samples[p_Index + 1];
	// Missing neighbours are mirrored so the curve starts/ends with a natural tangent
	const Vec2 l_P0 = p_Index > 0 ? m_Samples[p_Index - 1].screen : l_S1.screen * 2.f - l_S2.screen;
	const Vec2 l_P3 = p_Index + 2 < l_Count ? m_Samples[p_Index + 2].screen : l_S2.screen * 2.f - l_S1.screen;

	// Arc-length resampling: the curve is evaluated densely and a point is emitted just before the distance from
	// the previous emitted point would exceed the spacing (centripetal Catmull-Rom has uneven parametric speed,
	// so equal steps in u would not give equal spacing)
	const float l_Spacing = std::max(m_Settings.resampleSpacingPx, 0.25f);
	const float l_Chord = glm::length(l_S2.screen - l_S1.screen);
	const int l_FineSteps = std::max(2, static_cast<int>(std::ceil(l_Chord / l_Spacing)) * 8);

	Vec2 l_LastEmitted = l_S1.screen;
	Vec2 l_Previous = l_S1.screen;
	float l_PreviousU = 0.f;
	for (int i = 1; i <= l_FineSteps; ++i)
	{
		const float l_U = static_cast<float>(i) / static_cast<float>(l_FineSteps);
		const Vec2 l_Position = i == l_FineSteps ? l_S2.screen : catmullRom(l_P0, l_S1.screen, l_S2.screen, l_P3, l_U);
		if (glm::length(l_Position - l_LastEmitted) > l_Spacing && l_PreviousU > 0.f && l_Previous != l_LastEmitted)
		{
			// Pressure is interpolated linearly: a cubic would overshoot
			p_Out.push_back(toStrokePoint(l_Previous, l_S1.pressure + (l_S2.pressure - l_S1.pressure) * l_PreviousU));
			l_LastEmitted = l_Previous;
		}
		l_Previous = l_Position;
		l_PreviousU = l_U;
	}
	p_Out.push_back(toStrokePoint(l_S2.screen, l_S2.pressure));
}

void StrokeBuilder::commitReadySegments()
{
	// Segment i -> i+1 is final once sample i+2 exists
	while (m_CommittedSegments + 2 < m_Samples.size())
	{
		const size_t l_First = m_Committed.size();
		emitSegment(m_CommittedSegments, m_Committed);
		applyStartTaper(m_Committed, l_First, m_CommittedArc);
		++m_CommittedSegments;
	}
}

void StrokeBuilder::rebuildTail()
{
	m_Tail.clear();
	if (m_Committed.empty())
		return;

	// The tail continues the committed polyline: seed it with the last committed point for the taper
	m_Tail.push_back(m_Committed.back());
	for (size_t i = m_CommittedSegments; i + 1 < m_Samples.size(); ++i)
		emitSegment(i, m_Tail);
	float l_Arc = m_CommittedArc;
	applyStartTaper(m_Tail, 1, l_Arc);
	m_Tail.erase(m_Tail.begin());
}

void StrokeBuilder::applyStartTaper(std::vector<StrokePoint>& p_Points, const size_t p_From, float& p_Arc) const
{
	if (m_Settings.taperStartPx <= 0.f)
		return;
	const float l_Length = static_cast<float>(static_cast<double>(m_Settings.taperStartPx) / m_Camera.pixelsPerUnit());
	for (size_t i = p_From; i < p_Points.size(); ++i)
	{
		if (i > 0)
			p_Arc += glm::length(p_Points[i].position - p_Points[i - 1].position);
		if (p_Arc < l_Length)
			p_Points[i].radius *= taperFactor(p_Arc / l_Length);
	}
}

void StrokeBuilder::applyEndTaper(std::vector<StrokePoint>& p_Points) const
{
	if (m_Settings.taperEndPx <= 0.f || p_Points.size() < 2)
		return;
	const float l_Length = static_cast<float>(static_cast<double>(m_Settings.taperEndPx) / m_Camera.pixelsPerUnit());
	float l_Arc = 0.f;
	for (size_t i = p_Points.size(); i-- > 0;)
	{
		if (i + 1 < p_Points.size())
			l_Arc += glm::length(p_Points[i + 1].position - p_Points[i].position);
		if (l_Arc >= l_Length)
			break;
		p_Points[i].radius *= taperFactor(l_Arc / l_Length);
	}
}

StrokeData StrokeBuilder::finish(const StrokeInput* p_Last)
{
	StrokeData l_Result{ .style = m_Style };
	if (!m_Active)
		return l_Result;

	if (p_Last != nullptr)
	{
		// Reach the exact pen-up position: smoothing must not shorten the stroke
		const Vec2 l_Previous = m_Samples.back().screen;
		const float l_PreviousPressure = m_Samples.back().pressure;
		if (glm::length(p_Last->screen - l_Previous) >= m_Settings.minSampleDistancePx)
		{
			m_Samples.push_back(Sample{ .screen = p_Last->screen, .pressure = l_PreviousPressure });
		}
	}

	std::vector<StrokePoint> l_Points = std::move(m_Committed);
	const size_t l_First = l_Points.size();
	for (size_t i = m_CommittedSegments; i + 1 < m_Samples.size(); ++i)
		emitSegment(i, l_Points);
	applyStartTaper(l_Points, l_First, m_CommittedArc);
	applyEndTaper(l_Points);
	const float l_Tolerance = static_cast<float>(static_cast<double>(m_Settings.simplifyTolerancePx) / m_Camera.pixelsPerUnit());
	l_Result.points = simplifyStroke(l_Points, l_Tolerance);

	cancel();
	return l_Result;
}

void StrokeBuilder::cancel()
{
	m_Active = false;
	m_Samples.clear();
	m_Committed.clear();
	m_Tail.clear();
	m_CommittedSegments = 0;
	++m_Revision;
}

std::vector<StrokePoint> simplifyStroke(const std::span<const StrokePoint> p_Points, const float p_Tolerance)
{
	if (p_Points.size() <= 2 || p_Tolerance < 0.f)
		return { p_Points.begin(), p_Points.end() };

	std::vector<bool> l_Keep(p_Points.size(), false);
	l_Keep.front() = true;
	l_Keep.back() = true;

	std::vector<std::pair<size_t, size_t>> l_Stack;
	l_Stack.emplace_back(0, p_Points.size() - 1);
	while (!l_Stack.empty())
	{
		const auto [l_First, l_Last] = l_Stack.back();
		l_Stack.pop_back();
		if (l_Last <= l_First + 1)
			continue;

		float l_MaxDistance = 0.f;
		size_t l_MaxIndex = l_First;
		for (size_t i = l_First + 1; i < l_Last; ++i)
		{
			const float l_Distance = distanceToSegment3(p_Points[i], p_Points[l_First], p_Points[l_Last]);
			if (l_Distance > l_MaxDistance)
			{
				l_MaxDistance = l_Distance;
				l_MaxIndex = i;
			}
		}
		if (l_MaxDistance > p_Tolerance)
		{
			l_Keep[l_MaxIndex] = true;
			l_Stack.emplace_back(l_First, l_MaxIndex);
			l_Stack.emplace_back(l_MaxIndex, l_Last);
		}
	}

	std::vector<StrokePoint> l_Result;
	for (size_t i = 0; i < p_Points.size(); ++i)
	{
		if (l_Keep[i])
			l_Result.push_back(p_Points[i]);
	}
	return l_Result;
}
} // namespace wb
