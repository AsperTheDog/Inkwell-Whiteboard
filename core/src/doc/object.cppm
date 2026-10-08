// Canvas objects. Every object has an id, an affine transform (local -> world) and a typed payload.
//
// Adding an object type means: a payload struct here, an entry in ObjectPayload, a renderer, hit-testing and
// serialization support. Tools and commands only deal with Object.
module;
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <glm/glm.hpp>

export module wb.doc.object;

import wb.math;

export namespace wb
{
using ObjectId = uint64_t;
inline constexpr ObjectId INVALID_OBJECT_ID = 0;

// Encoded image files live in the document's asset table; image objects refer to them
using AssetId = uint64_t;
inline constexpr AssetId INVALID_ASSET_ID = 0;

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

// A picture (or animated GIF). The object-local rectangle is centred on the origin and p_Size units big; the pixels of
// the asset are stretched over it, so recompressing an asset to fewer pixels does not change how the image looks.
struct ImageData
{
	AssetId asset = INVALID_ASSET_ID;
	Vec2 size{ 0.f };
	bool playing = true;  // animated images: false freezes the animation on `frame`
	uint32_t frame = 0;

	[[nodiscard]] Rect localBounds() const { return Rect::fromCenter(DVec2{ 0.0 }, DVec2{ size } * 0.5); }

	bool operator==(const ImageData&) const = default;
};

// A video. Like a picture the object-local rectangle is centred on the origin and p_Size units big; the encoded file is
// an asset (the video's pictures and sound are decoded while it is on screen). Where it is playing from is not part
// of the board: a video always opens paused at the start.
struct VideoData
{
	AssetId asset = INVALID_ASSET_ID;
	Vec2 size{ 0.f };
	bool loop = false;
	bool muted = true;

	[[nodiscard]] Rect localBounds() const { return Rect::fromCenter(DVec2{ 0.0 }, DVec2{ size } * 0.5); }

	bool operator==(const VideoData&) const = default;
};

enum class TextAlign : uint8_t
{
	Left,
	Center,
	Right,
};

namespace TextStyle
{
inline constexpr uint8_t Bold = 1u << 0;
inline constexpr uint8_t Italic = 1u << 1;
} // namespace TextStyle

// Text. The object-local rectangle is centred on the origin like a picture's. `size` is the extent of the laid-out
// text; whoever changes the text, font or wrap width measures it again and stores it here (the document itself knows
// nothing about fonts, so files load and hit-test without them).
struct TextData
{
	std::string text;                  // UTF-8, line feed separates lines
	std::string family = "Inter";      // font family name; the font itself is looked up at draw time
	uint8_t style = 0;                 // TextStyle flags
	float fontSize = 32.f;             // em size in local units
	Color color = Color::fromRgba8(0x1F1F1FFFu);
	TextAlign align = TextAlign::Left;
	float wrapWidth = 0.f;             // local units; 0 = lines end only where the text has a line break
	Vec2 size{ 1.f, 1.f };             // measured extent in local units

	[[nodiscard]] Rect localBounds() const { return Rect::fromCenter(DVec2{ 0.0 }, DVec2{ size } * 0.5); }

	bool operator==(const TextData&) const = default;
};

using ObjectPayload = std::variant<StrokeData, ImageData, TextData, VideoData>;

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
	[[nodiscard]] ImageData* image() { return std::get_if<ImageData>(&payload); }
	[[nodiscard]] const ImageData* image() const { return std::get_if<ImageData>(&payload); }
	[[nodiscard]] VideoData* video() { return std::get_if<VideoData>(&payload); }
	[[nodiscard]] const VideoData* video() const { return std::get_if<VideoData>(&payload); }
	[[nodiscard]] TextData* text() { return std::get_if<TextData>(&payload); }
	[[nodiscard]] const TextData* text() const { return std::get_if<TextData>(&payload); }

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
