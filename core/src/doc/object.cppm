// Canvas objects. Every object has an id, an affine transform (local -> world) and a typed payload.
//
// Adding an object type means: a payload struct here, an entry in ObjectPayload, a renderer, hit-testing and
// serialization support. Tools and commands only deal with Object.
module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
	Highlighter, // see-through ink, a fixed colour alpha of HIGHLIGHTER_ALPHA
};

inline constexpr float HIGHLIGHTER_ALPHA = 0.4f;

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
// Cropping only chooses which part of the asset is shown: `crop` is that part as a rectangle of the whole picture
// (0..1 on both axes, y down), and `size` is how big the shown part is. The file itself is never changed.
struct ImageData
{
	AssetId asset = INVALID_ASSET_ID;
	Vec2 size{ 0.f };
	bool playing = true;  // animated images: false freezes the animation on `frame`
	uint32_t frame = 0;
	Vec2 cropMin{ 0.f };  // top left of the shown part, as a fraction of the whole picture
	Vec2 cropMax{ 1.f };  // bottom right

	[[nodiscard]] Rect localBounds() const { return Rect::fromCenter(DVec2{ 0.0 }, DVec2{ size } * 0.5); }
	[[nodiscard]] bool cropped() const { return cropMin != Vec2{ 0.f } || cropMax != Vec2{ 1.f }; }
	// The whole picture in object-local space (the shown part is localBounds())
	[[nodiscard]] Rect fullLocalBounds() const
	{
		const DVec2 l_Fraction = DVec2{ cropMax } - DVec2{ cropMin };
		const DVec2 l_Full = DVec2{ size } / l_Fraction;
		const DVec2 l_Min = -DVec2{ size } * 0.5 - DVec2{ cropMin } * l_Full;
		return Rect::fromPoints(l_Min, l_Min + l_Full);
	}

	bool operator==(const ImageData&) const = default;
};

// A video. Like a picture the object-local rectangle is centred on the origin and p_Size units big; the encoded file is
// an asset (the video's pictures and sound are decoded while it is on screen). Where it is playing from is not part
// of the board: a video always opens paused at the start.
// A subtitle file somebody added to a video (SRT, WebVTT or ASS). It belongs to the video, so everyone sees it in the list.
struct SubtitleFile
{
	std::string name; // the file's name
	std::string text; // its content as UTF-8

	bool operator==(const SubtitleFile&) const = default;
};
inline constexpr size_t MAX_SUBTITLE_FILES = 16;
inline constexpr size_t MAX_SUBTITLE_FILE_BYTES = size_t{ 2 } << 20;

struct VideoData
{
	AssetId asset = INVALID_ASSET_ID;
	Vec2 size{ 0.f };
	bool loop = false;
	bool muted = true;
	float volume = 1.f; // 0..1 on the volume slider; the sound plays at volume squared
	// Which sound track and which subtitles each person watches with (like loop and volume, their own business)
	int8_t audioTrack = -1;    // position among the file's sound tracks; -1: the file's own choice
	int8_t subtitleTrack = -1; // among the file's own text subtitle tracks, then subtitleFiles; -1: none
	std::vector<SubtitleFile> subtitleFiles; // shared: added by anyone, listed for everyone

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

// A stretch of a text drawn in its own colour instead of TextData::color. Only the RGB counts: the alpha always comes
// from the text's own colour.
struct ColorSpan
{
	uint32_t begin = 0; // byte offsets into the text, [begin, end)
	uint32_t end = 0;
	Color color{};

	bool operator==(const ColorSpan&) const = default;
};

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
	// Letters that differ from `color`: sorted, never empty or overlapping, and never equal to `color` in RGB
	// (normalizeSpans() establishes that). Changing the text means moving the spans with it (see editSpans()).
	std::vector<ColorSpan> spans;
	TextAlign align = TextAlign::Left;
	float wrapWidth = 0.f;             // local units; 0 = lines end only where the text has a line break
	Vec2 size{ 1.f, 1.f };             // measured extent in local units

	[[nodiscard]] Rect localBounds() const { return Rect::fromCenter(DVec2{ 0.0 }, DVec2{ size } * 0.5); }

	bool operator==(const TextData&) const = default;
};

[[nodiscard]] inline bool sameSpanRgb(const Color& p_A, const Color& p_B)
{
	return p_A.r == p_B.r && p_A.g == p_B.g && p_A.b == p_B.b;
}

// The span holding byte p_Byte, or nullptr
[[nodiscard]] inline const ColorSpan* spanAt(const std::vector<ColorSpan>& p_Spans, const size_t p_Byte)
{
	const auto l_It = std::ranges::upper_bound(p_Spans, p_Byte, {}, [](const ColorSpan& p_Span) { return static_cast<size_t>(p_Span.begin); });
	if (l_It == p_Spans.begin())
		return nullptr;
	const ColorSpan& l_Span = *(l_It - 1);
	return p_Byte < l_Span.end ? &l_Span : nullptr;
}

// Puts spans in canonical form: clipped to [0, p_TextSize), sorted, empty ones and those equal to p_Base dropped, and
// neighbours with the same colour merged. Overlaps are resolved in favour of the span that comes later in the list.
inline void normalizeSpans(std::vector<ColorSpan>& p_Spans, const size_t p_TextSize, const Color& p_Base)
{
	// Paint the spans in order onto per-boundary pieces, so a later span wins where two overlap
	std::vector<ColorSpan> l_Painted;
	for (ColorSpan l_Span : p_Spans)
	{
		l_Span.end = static_cast<uint32_t>(std::min<size_t>(l_Span.end, p_TextSize));
		if (l_Span.begin >= l_Span.end)
			continue;
		std::vector<ColorSpan> l_Next;
		for (const ColorSpan& l_Old : l_Painted)
		{
			if (l_Old.end <= l_Span.begin || l_Old.begin >= l_Span.end)
			{
				l_Next.push_back(l_Old);
				continue;
			}
			if (l_Old.begin < l_Span.begin)
				l_Next.push_back(ColorSpan{ l_Old.begin, l_Span.begin, l_Old.color });
			if (l_Old.end > l_Span.end)
				l_Next.push_back(ColorSpan{ l_Span.end, l_Old.end, l_Old.color });
		}
		l_Next.push_back(l_Span);
		l_Painted = std::move(l_Next);
	}
	std::ranges::sort(l_Painted, {}, &ColorSpan::begin);
	p_Spans.clear();
	for (ColorSpan& l_Span : l_Painted)
	{
		l_Span.color.a = 1.f;
		if (sameSpanRgb(l_Span.color, p_Base))
			continue;
		if (!p_Spans.empty() && p_Spans.back().end == l_Span.begin && sameSpanRgb(p_Spans.back().color, l_Span.color))
			p_Spans.back().end = l_Span.end;
		else
			p_Spans.push_back(l_Span);
	}
}

// Colours the bytes [p_Begin, p_End) (the base colour itself removes the override)
inline void paintSpans(std::vector<ColorSpan>& p_Spans, const size_t p_Begin, const size_t p_End, const Color& p_Color, const size_t p_TextSize, const Color& p_Base)
{
	if (p_Begin >= p_End)
		return;
	p_Spans.push_back(ColorSpan{ static_cast<uint32_t>(p_Begin), static_cast<uint32_t>(p_End), p_Color });
	normalizeSpans(p_Spans, p_TextSize, p_Base);
}

// Keeps spans in step with an edit that replaced the p_Removed bytes at p_Position with p_Inserted new bytes. The new
// bytes take the colour of the span in p_Inherit (nullptr: the base colour).
inline void editSpans(std::vector<ColorSpan>& p_Spans, const size_t p_Position, const size_t p_Removed, const size_t p_Inserted, const ColorSpan* p_Inherit)
{
	const std::optional<Color> l_Color = p_Inherit != nullptr ? std::optional<Color>(p_Inherit->color) : std::nullopt;
	const size_t l_RemovedEnd = p_Position + p_Removed;
	std::vector<ColorSpan> l_Result;
	const auto l_Shift = [&](const size_t p_At) { return p_At >= l_RemovedEnd ? p_At - p_Removed + p_Inserted : p_Position; };
	for (const ColorSpan& l_Span : p_Spans)
	{
		// The part before the edit and the part after it, each kept in place or moved
		if (l_Span.begin < p_Position)
			l_Result.push_back(ColorSpan{ l_Span.begin, static_cast<uint32_t>(std::min<size_t>(l_Span.end, p_Position)), l_Span.color });
		if (l_Span.end > l_RemovedEnd)
		{
			const size_t l_From = std::max<size_t>(l_Span.begin, l_RemovedEnd);
			l_Result.push_back(ColorSpan{ static_cast<uint32_t>(l_Shift(l_From)), static_cast<uint32_t>(l_Shift(l_Span.end)), l_Span.color });
		}
	}
	if (l_Color && p_Inserted > 0)
		l_Result.push_back(ColorSpan{ static_cast<uint32_t>(p_Position), static_cast<uint32_t>(p_Position + p_Inserted), *l_Color });
	std::ranges::sort(l_Result, {}, &ColorSpan::begin);
	// Spans split by the edit that touch again are merged back
	p_Spans.clear();
	for (const ColorSpan& l_Span : l_Result)
	{
		if (l_Span.begin >= l_Span.end)
			continue;
		if (!p_Spans.empty() && p_Spans.back().end == l_Span.begin && sameSpanRgb(p_Spans.back().color, l_Span.color))
			p_Spans.back().end = l_Span.end;
		else
			p_Spans.push_back(l_Span);
	}
}

using ObjectPayload = std::variant<StrokeData, ImageData, TextData, VideoData>;

struct Object
{
	ObjectId id = INVALID_OBJECT_ID;
	Affine2 transform{};
	ObjectPayload payload{};
	bool locked = false;  // cannot be moved, edited, erased or deleted until unlocked
	// Objects that share a group id (other than INVALID_OBJECT_ID) are selected and edited together, as one. The id is
	// allocated like an object id, so it is unique across peers; nothing else about it matters.
	ObjectId group = INVALID_OBJECT_ID;
	uint32_t version = 0; // bumped on every change; lets caches (GPU buffers...) detect stale data
	// The renderer's private index for this object's GPU data. Copies carry it along, so the renderer checks that the
	// slot it points to really belongs to this id before trusting it.
	mutable uint32_t renderSlot = 0;

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
	l_Copy->locked = false;
	l_Copy->renderSlot = 0;
	return l_Copy;
}
} // namespace wb
