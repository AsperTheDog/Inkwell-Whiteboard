// Turns raw pointer samples into a smooth stroke.
//
// Pipeline (all in screen pixels, converted to world with the camera captured at stroke start):
//   1. One Euro filter on position: steady when moving slowly, low lag when moving fast
//   2. Pressure smoothing and the pressure->width curve (or simulated pressure for mice)
//   3. Centripetal Catmull-Rom interpolation between filtered samples, resampled at a fixed spacing
//   4. On finish: reach the exact pen-up position, taper the ends, simplify (width-aware Douglas-Peucker)
//
// While drawing, the output is split into committed points (final) and a provisional tail towards the latest
// sample, which is recomputed on every new sample.
module;
#include <cstdint>
#include <span>
#include <vector>
#include <glm/glm.hpp>

export module wb.brush.stroke_builder;

import wb.math;
import wb.doc.object;
import wb.view.camera;

export namespace wb
{
// One Euro filter (Casiez et al. 2012) for a 2D signal
class OneEuroFilter
{
public:
	void configure(float p_MinCutoffHz, float p_Beta, float p_DerivativeCutoffHz);
	void reset();
	[[nodiscard]] Vec2 filter(Vec2 p_Value, double p_DeltaSeconds);

private:
	float m_MinCutoff = 1.f;
	float m_Beta = 0.f;
	float m_DerivativeCutoff = 1.f;
	bool m_Initialized = false;
	Vec2 m_Value{ 0.f };
	Vec2 m_Derivative{ 0.f };
};

struct BrushSettings
{
	// Smoothing (One Euro filter)
	float smoothingMinCutoff = 2.5f; // Hz; lower = smoother but laggier at low speed
	float smoothingBeta = 0.02f;     // speed coefficient; higher = less lag when fast
	float pressureSmoothing = 0.35f; // 0 = raw pressure, towards 1 = heavier smoothing

	// Pressure -> width: width = size * (minWidth + (1 - minWidth) * pressure^gamma)
	float pressureGamma = 0.8f;
	float minWidthFraction = 0.2f;
	float pressureSensitivity = 1.f; // 0 = constant width, 1 = full pressure response
	bool simulatePressureForMouse = false; // velocity-based thinning for devices without pressure

	// Geometry
	float resampleSpacingPx = 1.5f;  // spacing of interpolated points
	float minSampleDistancePx = 0.6f; // closer samples are merged
	float simplifyTolerancePx = 0.15f; // negative disables simplification
	float taperStartPx = 0.f;
	float taperEndPx = 0.f;
};

struct StrokeInput
{
	Vec2 screen{ 0.f };    // window pixels
	float pressure = 1.f;  // 0..1
	uint64_t timestampNs = 0;
};

class StrokeBuilder
{
public:
	// p_SizePoints is the stroke width in logical points on screen (constant on-screen size at any zoom)
	void begin(const StrokeInput& p_First, bool p_HasPressure, float p_SizePoints, Color p_Color, const Camera& p_Camera, const BrushSettings& p_Settings);
	void add(const StrokeInput& p_Input);
	// Finishes the stroke. Points are relative to origin(); place the object with Affine2::translate(origin()).
	[[nodiscard]] StrokeData finish(const StrokeInput* p_Last);
	void cancel();

	[[nodiscard]] bool isActive() const { return m_Active; }
	[[nodiscard]] DVec2 origin() const { return m_Origin; }
	[[nodiscard]] const StrokeStyle& style() const { return m_Style; }
	[[nodiscard]] std::span<const StrokePoint> committedPoints() const { return m_Committed; }
	[[nodiscard]] std::span<const StrokePoint> tailPoints() const { return m_Tail; }
	// Bumped whenever the visible output changes
	[[nodiscard]] uint64_t revision() const { return m_Revision; }

private:
	struct Sample
	{
		Vec2 screen{ 0.f };
		float pressure = 1.f;
	};

	void pushSample(const StrokeInput& p_Input, bool p_Filter);
	[[nodiscard]] float widthFactor(float p_Pressure) const;
	[[nodiscard]] StrokePoint toStrokePoint(Vec2 p_Screen, float p_Pressure) const;
	// Interpolates segment samples[i] -> samples[i+1] and appends to p_Out (excluding the start point)
	void emitSegment(size_t p_Index, std::vector<StrokePoint>& p_Out) const;
	void commitReadySegments();
	void rebuildTail();
	// Thins points from p_From on while the arc length (running total in p_Arc) is inside the start taper
	void applyStartTaper(std::vector<StrokePoint>& p_Points, size_t p_From, float& p_Arc) const;
	void applyEndTaper(std::vector<StrokePoint>& p_Points) const;

	BrushSettings m_Settings{};
	StrokeStyle m_Style{};
	Camera m_Camera{};
	DVec2 m_Origin{ 0.0 };
	double m_RadiusWorldPerUnitWidth = 0.0; // world radius for widthFactor() == 1
	bool m_HasPressure = true;
	bool m_Active = false;

	OneEuroFilter m_PositionFilter;
	float m_SmoothedPressure = 1.f;
	uint64_t m_LastTimestampNs = 0;
	Vec2 m_LastRawScreen{ 0.f };

	std::vector<Sample> m_Samples;  // filtered samples
	size_t m_CommittedSegments = 0; // segments samples[i]->samples[i+1] already in m_Committed
	float m_CommittedArc = 0.f;     // arc length of m_Committed (world units), for the start taper
	std::vector<StrokePoint> m_Committed;
	std::vector<StrokePoint> m_Tail;
	uint64_t m_Revision = 0;
};

// Width-aware Douglas-Peucker: drops points whose removal changes position and radius by less than the
// tolerance (in the same units as the points). Always keeps the first and last point; a negative tolerance
// keeps every point.
[[nodiscard]] std::vector<StrokePoint> simplifyStroke(std::span<const StrokePoint> p_Points, float p_Tolerance);
} // namespace wb
