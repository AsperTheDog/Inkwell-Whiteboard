// Text layout: breaks a string into lines (explicit line breaks and word wrapping), places every glyph, and answers the
// questions an editor asks (where is the caret for this byte offset, which offset is under this point, what rectangles
// does a selection cover).
//
// Fonts are reached through the Metrics interface, so the layout is independent of the font library and can be
// tested with a fake font. All lengths are in the units of TextData::fontSize; the box origin is its top-left corner.
module;
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

export module wb.text.layout;

import wb.math;
import wb.doc.object;

export namespace wb::text
{
// How one code point is drawn. `font` and `glyph` are opaque ids chosen by the Metrics implementation after font
// fallback; the layout hands them back unchanged.
struct GlyphInfo
{
	uint32_t font = 0;
	uint32_t glyph = 0;
	float advance = 0.f; // in ems
};

struct LineMetrics
{
	float ascent = 0.8f;   // above the baseline, in ems
	float descent = -0.2f; // below the baseline (negative), in ems
	float lineGap = 0.f;   // extra space between lines, in ems
};

class Metrics
{
public:
	virtual ~Metrics() = default;
	[[nodiscard]] virtual GlyphInfo glyph(uint32_t p_Codepoint) const = 0;
	// Extra advance (in ems, usually negative) between two glyphs that follow each other
	[[nodiscard]] virtual float kerning(const GlyphInfo& p_Left, uint32_t p_LeftCodepoint, const GlyphInfo& p_Right, uint32_t p_RightCodepoint) const
	{
		(void)p_Left;
		(void)p_LeftCodepoint;
		(void)p_Right;
		(void)p_RightCodepoint;
		return 0.f;
	}
	[[nodiscard]] virtual LineMetrics lineMetrics() const = 0;
};

struct LayoutOptions
{
	float fontSize = 32.f;
	float wrapWidth = 0.f; // 0: no wrapping
	TextAlign align = TextAlign::Left;
};

struct LaidGlyph
{
	uint32_t codepoint = 0;
	uint32_t font = 0;
	uint32_t glyph = 0;
	uint32_t byteBegin = 0; // the glyph's bytes in the text
	uint32_t byteEnd = 0;
	uint32_t line = 0;
	float x = 0.f;       // left edge of the advance box, inside the layout box
	float advance = 0.f; // includes kerning
};

struct LaidLine
{
	uint32_t byteBegin = 0;
	uint32_t byteEnd = 0; // exclusive; the line break itself is not part of the line
	uint32_t glyphBegin = 0;
	uint32_t glyphEnd = 0;
	float x = 0.f;            // left edge of the first glyph (alignment applied)
	float width = 0.f;        // all glyphs
	float visibleWidth = 0.f; // without the spaces left hanging at a soft break
	float top = 0.f;
	float baseline = 0.f;
	float height = 0.f;
	bool softBreak = false; // wrapped here (no line break character follows)
};

struct TextLayout
{
	std::vector<LaidGlyph> glyphs;
	std::vector<LaidLine> lines; // never empty
	Vec2 size{ 0.f };
	float fontSize = 0.f;
};

struct CaretPosition
{
	uint32_t line = 0;
	float x = 0.f;
	float top = 0.f;
	float height = 0.f;
};

[[nodiscard]] TextLayout layoutText(std::string_view p_Text, const Metrics& p_Metrics, const LayoutOptions& p_Options);

// Caret for a byte offset. At a soft line break the caret belongs to the line that starts there, unless
// p_EndOfPreviousLine asks for the end of the line before.
[[nodiscard]] CaretPosition caretAt(const TextLayout& p_Layout, size_t p_Byte, bool p_EndOfPreviousLine = false);
// Byte offset nearest to p_Point (layout coordinates)
[[nodiscard]] size_t indexAtPoint(const TextLayout& p_Layout, Vec2 p_Point);
// Byte offset on the line p_Delta lines away from the line of p_Byte, at horizontal position p_PreferredX. Moving past
// the first or last line goes to the start or end of the text.
[[nodiscard]] size_t moveVertically(const TextLayout& p_Layout, size_t p_Byte, int p_Delta, float p_PreferredX);
// Home / End of the visual line holding p_Byte
[[nodiscard]] size_t lineStart(const TextLayout& p_Layout, size_t p_Byte);
[[nodiscard]] size_t lineEnd(const TextLayout& p_Layout, size_t p_Byte);
// Rectangles covering the bytes [p_Begin, p_End), one per line, in layout coordinates
[[nodiscard]] std::vector<Rect> selectionRects(const TextLayout& p_Layout, size_t p_Begin, size_t p_End);
} // namespace wb::text
