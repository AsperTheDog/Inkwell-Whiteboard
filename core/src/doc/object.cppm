// Canvas objects. Every object has an id, an affine transform (local -> world) and a typed payload.
//
// Adding an object type means: a payload struct here, an entry in ObjectPayload, a renderer, hit-testing and
// serialization support. Tools and commands only deal with Object.
module;
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>
#include <glm/glm.hpp>

export module wb.doc.object;

import wb.math;

export namespace wb
{
using ObjectId = uint64_t;
inline constexpr ObjectId INVALID_OBJECT_ID = 0;

enum class BrushKind : uint8_t
{
	Pen,
};

struct StrokeStyle
{
	Color color{};
	float size = 4.f; // nominal diameter in world units (scaled per point by pressure)
	BrushKind brush = BrushKind::Pen;

	bool operator==(const StrokeStyle&) const = default;
};

// Stroke sample in object-local space
struct StrokePoint
{
	Vec2 position{ 0.f };
	float radius = 0.f; // half the stroke width at this point

	bool operator==(const StrokePoint&) const = default;
};

struct StrokeData
{
	StrokeStyle style{};
	std::vector<StrokePoint> points; // polyline; a single point is a dot

	// Tight local bounds including the radius at every point
	[[nodiscard]] Rect localBounds() const
	{
		Rect l_Bounds{};
		for (const StrokePoint& l_Point : points)
		{
			const DVec2 l_Center{ l_Point.position };
			l_Bounds.expand(Rect::fromCenter(l_Center, DVec2{ static_cast<double>(l_Point.radius) }));
		}
		return l_Bounds;
	}

	[[nodiscard]] float maxRadius() const
	{
		float l_Max = 0.f;
		for (const StrokePoint& l_Point : points)
			l_Max = std::max(l_Max, l_Point.radius);
		return l_Max;
	}
};

using ObjectPayload = std::variant<StrokeData>;

struct Object
{
	ObjectId id = INVALID_OBJECT_ID;
	Affine2 transform{};
	ObjectPayload payload{};
	uint32_t version = 0; // bumped on every change; lets caches (GPU buffers...) detect stale data

	// Recomputes the cached bounds. Document calls this on insert and after every modify().
	void refreshBounds()
	{
		m_LocalBounds = std::visit([](const auto& p_Data) { return p_Data.localBounds(); }, payload);
		m_WorldBounds = transform.applyBounds(m_LocalBounds);
	}

	// Cached; valid once the object is in a Document (or after refreshBounds())
	[[nodiscard]] const Rect& localBounds() const { return m_LocalBounds; }
	[[nodiscard]] const Rect& worldBounds() const { return m_WorldBounds; }

	[[nodiscard]] StrokeData* stroke() { return std::get_if<StrokeData>(&payload); }
	[[nodiscard]] const StrokeData* stroke() const { return std::get_if<StrokeData>(&payload); }

private:
	Rect m_LocalBounds{};
	Rect m_WorldBounds{};
};

// Deep copy under a new id (duplicate, paste)
[[nodiscard]] inline std::unique_ptr<Object> cloneObject(const Object& p_Source, const ObjectId p_NewId)
{
	auto l_Copy = std::make_unique<Object>(p_Source);
	l_Copy->id = p_NewId;
	l_Copy->version = 0;
	return l_Copy;
}
} // namespace wb
