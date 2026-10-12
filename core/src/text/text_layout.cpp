module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>
#include <SheenBidi/SheenBidi.h>

module wb.text.layout;

import wb.math;
import wb.doc.object;
import wb.text.utf8;

namespace wb::text
{
namespace
{
constexpr float TAB_SPACES = 4.f;
constexpr float MIN_WIDTH_EM = 0.1f;
constexpr float NEWLINE_MARK_EM = 0.3f; // width highlighted for a selected line break

struct Shaped
{
	uint32_t codepoint = 0;
	uint32_t font = 0;
	uint32_t glyph = 0;
	uint32_t begin = 0;
	uint32_t end = 0;
	float advance = 0.f;
	float xOffset = 0.f;
	float yOffset = 0.f;
	uint8_t level = 0;
};

// Scripts that run right to left or need shaping: the paragraph goes through the bidirectional algorithm
bool isComplexScript(const uint32_t p_Code)
{
	return (p_Code >= 0x0590 && p_Code <= 0x109F) || (p_Code >= 0x1780 && p_Code <= 0x17FF) || p_Code == 0x200E || p_Code == 0x200F || (p_Code >= 0x202A && p_Code <= 0x202E) || (p_Code >= 0x2066 && p_Code <= 0x2069)
		|| (p_Code >= 0xFB1D && p_Code <= 0xFDFF) || (p_Code >= 0xFE70 && p_Code <= 0xFEFF) || (p_Code >= 0x10800 && p_Code <= 0x10FFF) || (p_Code >= 0x1E800 && p_Code <= 0x1EFFF);
}

bool hasComplexScript(const std::string_view p_Text, const size_t p_Begin, const size_t p_End)
{
	const std::string_view l_Slice = p_Text.substr(0, p_End);
	size_t l_Index = p_Begin;
	while (l_Index < p_End)
	{
		if (static_cast<uint8_t>(l_Slice[l_Index]) < 0xC0) // ASCII and stray continuation bytes
		{
			++l_Index;
			continue;
		}
		if (isComplexScript(decodeNext(l_Slice, l_Index)))
			return true;
	}
	return false;
}

bool isBreakAfter(const uint32_t p_Codepoint)
{
	return isSpace(p_Codepoint) || breaksAnywhere(p_Codepoint) || p_Codepoint == '-' || p_Codepoint == 0x2010 || p_Codepoint == 0x2013;
}

// One glyph per character, appended to p_Out
void shapeAppend(const std::string_view p_Text, const size_t p_Begin, const size_t p_End, const Metrics& p_Metrics, const float p_FontSize, std::vector<Shaped>& p_Out)
{
	const float l_Space = p_Metrics.glyph(' ').advance * p_FontSize;
	GlyphInfo l_PreviousInfo{};
	bool l_HasPrevious = false;
	size_t l_Index = p_Begin;
	const std::string_view l_Slice = p_Text.substr(0, p_End);
	while (l_Index < p_End)
	{
		const size_t l_Start = l_Index;
		const uint32_t l_Code = decodeNext(l_Slice, l_Index);
		const GlyphInfo l_Info = p_Metrics.glyph(l_Code == '\t' ? ' ' : l_Code);
		Shaped l_Glyph{ .codepoint = l_Code, .font = l_Info.font, .glyph = l_Info.glyph, .begin = static_cast<uint32_t>(l_Start), .end = static_cast<uint32_t>(l_Index), .advance = l_Info.advance * p_FontSize };
		if (l_Code == '\t')
			l_Glyph.advance = l_Space * TAB_SPACES;
		if (isCombining(l_Code) || isFormatting(l_Code))
			l_Glyph.advance = 0.f;
		if (l_HasPrevious && !p_Out.empty() && !isCombining(l_Code) && l_Code != '\t' && p_Out.back().codepoint != '\t')
			p_Out.back().advance += p_Metrics.kerning(l_PreviousInfo, p_Out.back().codepoint, l_Info, l_Code) * p_FontSize;
		p_Out.push_back(l_Glyph);
		if (!isCombining(l_Code))
		{
			l_PreviousInfo = l_Info;
			l_HasPrevious = true;
		}
	}
}

void shape(const std::string_view p_Text, const size_t p_Begin, const size_t p_End, const Metrics& p_Metrics, const float p_FontSize, std::vector<Shaped>& p_Out)
{
	p_Out.clear();
	shapeAppend(p_Text, p_Begin, p_End, p_Metrics, p_FontSize, p_Out);
}

// One run of one embedding level, shaped by the metrics when they can, one glyph per character otherwise
void appendRun(const std::string_view p_Text, const size_t p_Begin, const size_t p_End, const uint8_t p_Level, const Metrics& p_Metrics, const float p_FontSize, std::vector<Shaped>& p_Out)
{
	std::vector<Metrics::ShapedGlyph> l_Shaped;
	if (p_Metrics.shapeRun(p_Text, p_Begin, p_End, (p_Level & 1) != 0, l_Shaped) && !l_Shaped.empty())
	{
		for (const Metrics::ShapedGlyph& l_Glyph : l_Shaped)
		{
			size_t l_At = l_Glyph.byteBegin;
			Shaped l_Out{ .codepoint = decodeNext(p_Text.substr(0, p_End), l_At), .font = l_Glyph.font, .glyph = l_Glyph.glyph, .begin = l_Glyph.byteBegin, .end = l_Glyph.byteEnd,
				.advance = l_Glyph.advance * p_FontSize, .xOffset = l_Glyph.xOffset * p_FontSize, .yOffset = l_Glyph.yOffset * p_FontSize, .level = p_Level };
			p_Out.push_back(l_Out);
		}
		return;
	}
	const size_t l_First = p_Out.size();
	shapeAppend(p_Text, p_Begin, p_End, p_Metrics, p_FontSize, p_Out);
	for (size_t i = l_First; i < p_Out.size(); ++i)
		p_Out[i].level = p_Level;
}

// A paragraph with right-to-left or complex scripts: glyphs in reading order, each with its embedding level.
// p_Rtl reports the paragraph direction (from its first strong character).
void shapeComplex(const std::string_view p_Text, const size_t p_Begin, const size_t p_End, const Metrics& p_Metrics, const float p_FontSize, std::vector<Shaped>& p_Out, bool& p_Rtl)
{
	p_Out.clear();
	p_Rtl = false;
	if (p_Begin >= p_End)
		return;
	const SBCodepointSequence l_Sequence{ SBStringEncodingUTF8, p_Text.data() + p_Begin, static_cast<SBUInteger>(p_End - p_Begin) };
	SBAlgorithmRef l_Algorithm = SBAlgorithmCreate(&l_Sequence);
	if (l_Algorithm == nullptr)
	{
		shape(p_Text, p_Begin, p_End, p_Metrics, p_FontSize, p_Out);
		return;
	}
	bool l_First = true;
	for (SBUInteger l_Offset = 0; l_Offset < p_End - p_Begin;)
	{
		SBParagraphRef l_Paragraph = SBAlgorithmCreateParagraph(l_Algorithm, l_Offset, static_cast<SBUInteger>(p_End - p_Begin) - l_Offset, SBLevelDefaultLTR);
		if (l_Paragraph == nullptr)
			break;
		const SBUInteger l_Length = SBParagraphGetLength(l_Paragraph);
		if (l_Length == 0)
		{
			SBParagraphRelease(l_Paragraph);
			break;
		}
		if (l_First)
			p_Rtl = (SBParagraphGetBaseLevel(l_Paragraph) & 1) != 0;
		l_First = false;
		const SBLevel* l_Levels = SBParagraphGetLevelsPtr(l_Paragraph);
		// Runs of one level; each is shaped on its own
		for (SBUInteger l_Start = 0; l_Start < l_Length;)
		{
			const SBLevel l_Level = l_Levels[l_Start];
			SBUInteger l_End = l_Start + 1;
			while (l_End < l_Length && l_Levels[l_End] == l_Level)
				++l_End;
			// A run never ends inside a character: levels are per byte but constant over a character
			appendRun(p_Text, p_Begin + l_Offset + l_Start, p_Begin + l_Offset + l_End, static_cast<uint8_t>(l_Level), p_Metrics, p_FontSize, p_Out);
			l_Start = l_End;
		}
		l_Offset += l_Length;
		SBParagraphRelease(l_Paragraph);
	}
	SBAlgorithmRelease(l_Algorithm);
}

// Appends glyphs [p_First, p_Last) of the paragraph as one line
void emitLine(TextLayout& p_Layout, const std::vector<Shaped>& p_Glyphs, const size_t p_First, const size_t p_Last, const size_t p_ParagraphBegin, const bool p_Soft, const bool p_Rtl)
{
	LaidLine l_Line;
	l_Line.glyphBegin = static_cast<uint32_t>(p_Layout.glyphs.size());
	l_Line.softBreak = p_Soft;
	l_Line.rtl = p_Rtl;
	const uint32_t l_Index = static_cast<uint32_t>(p_Layout.lines.size());
	float l_X = 0.f;
	bool l_Bidi = p_Rtl;
	for (size_t i = p_First; i < p_Last; ++i)
		l_Bidi = l_Bidi || (p_Glyphs[i].level & 1) != 0;
	l_Line.bidi = l_Bidi;

	if (!l_Bidi)
	{
		for (size_t i = p_First; i < p_Last; ++i)
		{
			const Shaped& l_Glyph = p_Glyphs[i];
			p_Layout.glyphs.push_back(LaidGlyph{ .codepoint = l_Glyph.codepoint, .font = l_Glyph.font, .glyph = l_Glyph.glyph, .byteBegin = l_Glyph.begin, .byteEnd = l_Glyph.end, .line = l_Index, .x = l_X, .advance = l_Glyph.advance,
				.xOffset = l_Glyph.xOffset, .yOffset = l_Glyph.yOffset, .level = l_Glyph.level });
			l_X += l_Glyph.advance;
		}
	}
	else
	{
		// Display order (rules L1 and L2): spaces at the end of the line take the paragraph's level, then runs of
		// the highest level are reversed, down to the lowest odd level
		const size_t l_Count = p_Last - p_First;
		const uint8_t l_Base = p_Rtl ? 1 : 0;
		std::vector<std::pair<size_t, uint8_t>> l_Order(l_Count);
		for (size_t i = 0; i < l_Count; ++i)
			l_Order[i] = { i, p_Glyphs[p_First + i].level };
		for (size_t i = l_Count; i > 0 && isSpace(p_Glyphs[p_First + i - 1].codepoint); --i)
			l_Order[i - 1].second = l_Base;
		std::vector<uint8_t> l_Levels(l_Count);
		for (size_t i = 0; i < l_Count; ++i)
			l_Levels[i] = l_Order[i].second;
		uint8_t l_Highest = 0;
		uint8_t l_LowestOdd = 255;
		for (const auto& l_Item : l_Order)
		{
			l_Highest = std::max(l_Highest, l_Item.second);
			if ((l_Item.second & 1) != 0)
				l_LowestOdd = std::min(l_LowestOdd, l_Item.second);
		}
		for (int l_Level = l_Highest; l_Level >= l_LowestOdd && l_Level > 0; --l_Level)
		{
			for (size_t i = 0; i < l_Count;)
			{
				if (l_Order[i].second < l_Level)
				{
					++i;
					continue;
				}
				size_t l_End = i;
				while (l_End < l_Count && l_Order[l_End].second >= l_Level)
					++l_End;
				std::reverse(l_Order.begin() + static_cast<std::ptrdiff_t>(i), l_Order.begin() + static_cast<std::ptrdiff_t>(l_End));
				i = l_End;
			}
		}
		std::vector<float> l_Xs(l_Count);
		for (const auto& l_Item : l_Order)
		{
			l_Xs[l_Item.first] = l_X;
			l_X += p_Glyphs[p_First + l_Item.first].advance;
		}
		for (size_t i = 0; i < l_Count; ++i)
		{
			const Shaped& l_Glyph = p_Glyphs[p_First + i];
			p_Layout.glyphs.push_back(LaidGlyph{ .codepoint = l_Glyph.codepoint, .font = l_Glyph.font, .glyph = l_Glyph.glyph, .byteBegin = l_Glyph.begin, .byteEnd = l_Glyph.end, .line = l_Index, .x = l_Xs[i], .advance = l_Glyph.advance,
				.xOffset = l_Glyph.xOffset, .yOffset = l_Glyph.yOffset, .level = l_Levels[i] });
		}
	}
	l_Line.glyphEnd = static_cast<uint32_t>(p_Layout.glyphs.size());
	l_Line.width = l_X;
	l_Line.visibleWidth = l_X;
	if (p_Soft)
	{
		for (size_t i = p_Last; i > p_First && isSpace(p_Glyphs[i - 1].codepoint); --i)
			l_Line.visibleWidth -= p_Glyphs[i - 1].advance;
	}
	l_Line.byteBegin = p_First < p_Last ? p_Glyphs[p_First].begin : static_cast<uint32_t>(p_ParagraphBegin);
	l_Line.byteEnd = p_First < p_Last ? p_Glyphs[p_Last - 1].end : static_cast<uint32_t>(p_ParagraphBegin);
	p_Layout.lines.push_back(l_Line);
}

void wrapParagraph(TextLayout& p_Layout, const std::vector<Shaped>& p_Glyphs, const size_t p_ParagraphBegin, const float p_WrapWidth, const bool p_Rtl)
{
	const size_t l_Count = p_Glyphs.size();
	if (p_WrapWidth <= 0.f)
	{
		emitLine(p_Layout, p_Glyphs, 0, l_Count, p_ParagraphBegin, false, p_Rtl);
		return;
	}

	size_t l_LineStart = 0;
	float l_X = 0.f;
	std::ptrdiff_t l_LastBreak = -1; // glyph after which the line may break
	size_t i = 0;
	while (i < l_Count)
	{
		const Shaped& l_Glyph = p_Glyphs[i];
		const bool l_Space = isSpace(l_Glyph.codepoint);
		// Spaces hang past the edge instead of wrapping; combining marks stay with their base
		if (!l_Space && l_Glyph.advance > 0.f && l_X + l_Glyph.advance > p_WrapWidth && i > l_LineStart)
		{
			size_t l_Next = i;
			if (l_LastBreak >= static_cast<std::ptrdiff_t>(l_LineStart))
				l_Next = static_cast<size_t>(l_LastBreak) + 1;
			emitLine(p_Layout, p_Glyphs, l_LineStart, l_Next, p_ParagraphBegin, true, p_Rtl);
			l_LineStart = l_Next;
			l_LastBreak = -1;
			l_X = 0.f;
			for (size_t k = l_LineStart; k < i; ++k)
				l_X += p_Glyphs[k].advance;
			continue; // the glyph is tested again on the new line
		}
		l_X += l_Glyph.advance;
		if (isBreakAfter(l_Glyph.codepoint))
			l_LastBreak = static_cast<std::ptrdiff_t>(i);
		++i;
	}
	emitLine(p_Layout, p_Glyphs, l_LineStart, l_Count, p_ParagraphBegin, false, p_Rtl);
}

float caretXBidi(const TextLayout& p_Layout, const LaidLine& p_Line, size_t p_Byte);
size_t indexInBidiLine(const TextLayout& p_Layout, const LaidLine& p_Line, float p_X);

// Horizontal position of byte offset p_Byte on a line
float caretX(const TextLayout& p_Layout, const LaidLine& p_Line, const size_t p_Byte)
{
	if (p_Line.bidi)
		return caretXBidi(p_Layout, p_Line, p_Byte);
	for (uint32_t i = p_Line.glyphBegin; i < p_Line.glyphEnd; ++i)
	{
		if (p_Layout.glyphs[i].byteBegin >= p_Byte)
			return p_Layout.glyphs[i].x;
	}
	return p_Line.x + p_Line.width;
}

// The glyph of a line that draws the character at p_Byte
const LaidGlyph* glyphAt(const TextLayout& p_Layout, const LaidLine& p_Line, const size_t p_Byte)
{
	for (uint32_t i = p_Line.glyphBegin; i < p_Line.glyphEnd; ++i)
	{
		const LaidGlyph& l_Glyph = p_Layout.glyphs[i];
		if (l_Glyph.byteBegin <= p_Byte && p_Byte < l_Glyph.byteEnd)
			return &l_Glyph;
	}
	return nullptr;
}

// Horizontal position of byte offset p_Byte on a line with right-to-left glyphs: the edge of the character's glyph
// it starts at, which is the right one in a right-to-left run. Inside a ligature the position is interpolated.
float caretXBidi(const TextLayout& p_Layout, const LaidLine& p_Line, const size_t p_Byte)
{
	if (p_Line.glyphBegin == p_Line.glyphEnd)
		return p_Line.x + p_Line.width;
	if (p_Byte >= p_Line.byteEnd)
	{
		const LaidGlyph& l_Last = p_Layout.glyphs[p_Line.glyphEnd - 1];
		return (l_Last.level & 1) != 0 ? l_Last.x : l_Last.x + l_Last.advance;
	}
	const LaidGlyph* l_Glyph = glyphAt(p_Layout, p_Line, p_Byte);
	if (l_Glyph == nullptr)
	{
		for (uint32_t i = p_Line.glyphBegin; i < p_Line.glyphEnd; ++i)
		{
			if (p_Layout.glyphs[i].byteBegin >= p_Byte)
			{
				l_Glyph = &p_Layout.glyphs[i];
				break;
			}
		}
		if (l_Glyph == nullptr)
			return p_Line.x + p_Line.width;
	}
	const bool l_Rtl = (l_Glyph->level & 1) != 0;
	float l_Fraction = 0.f; // how far into the glyph's characters the offset lies
	if (p_Byte > l_Glyph->byteBegin && l_Glyph->byteEnd > l_Glyph->byteBegin)
		l_Fraction = static_cast<float>(p_Byte - l_Glyph->byteBegin) / static_cast<float>(l_Glyph->byteEnd - l_Glyph->byteBegin);
	return l_Rtl ? l_Glyph->x + l_Glyph->advance * (1.f - l_Fraction) : l_Glyph->x + l_Glyph->advance * l_Fraction;
}

// Byte offset of the End key on a line: before the spaces left hanging at a soft break
size_t lineEndByte(const TextLayout& p_Layout, const LaidLine& p_Line)
{
	if (!p_Line.softBreak)
		return p_Line.byteEnd;
	uint32_t l_End = p_Line.glyphEnd;
	while (l_End > p_Line.glyphBegin && isSpace(p_Layout.glyphs[l_End - 1].codepoint))
		--l_End;
	if (l_End == p_Line.glyphEnd)
		return p_Line.byteEnd;
	return l_End > p_Line.glyphBegin ? p_Layout.glyphs[l_End - 1].byteEnd : p_Line.byteBegin;
}

size_t indexInBidiLine(const TextLayout& p_Layout, const LaidLine& p_Line, const float p_X)
{
	if (p_Line.glyphBegin == p_Line.glyphEnd)
		return p_Line.byteBegin;
	// The reading position whose caret is drawn nearest to the point. Choosing among the positions the caret can
	// actually take (not by glyph edges) keeps the caret where the click was, even at the seam of two runs.
	std::vector<uint32_t> l_Positions;
	l_Positions.push_back(p_Line.byteBegin);
	for (uint32_t i = p_Line.glyphBegin; i < p_Line.glyphEnd; ++i)
	{
		l_Positions.push_back(p_Layout.glyphs[i].byteBegin);
		l_Positions.push_back(p_Layout.glyphs[i].byteEnd);
	}
	std::ranges::sort(l_Positions);
	l_Positions.erase(std::unique(l_Positions.begin(), l_Positions.end()), l_Positions.end());
	const size_t l_Last = lineEndByte(p_Layout, p_Line); // spaces hanging at a soft break are not places to click
	size_t l_Best = p_Line.byteBegin;
	float l_BestDistance = std::numeric_limits<float>::max();
	for (const uint32_t l_Position : l_Positions)
	{
		if (l_Position < p_Line.byteBegin || l_Position > l_Last)
			continue;
		const float l_Distance = std::abs(caretXBidi(p_Layout, p_Line, l_Position) - p_X);
		if (l_Distance < l_BestDistance)
		{
			l_BestDistance = l_Distance;
			l_Best = l_Position;
		}
	}
	return l_Best;
}

uint32_t lineIndexFor(const TextLayout& p_Layout, const size_t p_Byte, const bool p_EndOfPreviousLine)
{
	const size_t l_Count = p_Layout.lines.size();
	for (size_t i = 0; i < l_Count; ++i)
	{
		const LaidLine& l_Line = p_Layout.lines[i];
		if (p_Byte < l_Line.byteEnd)
			return static_cast<uint32_t>(i);
		if (p_Byte == l_Line.byteEnd)
		{
			if (l_Line.softBreak && !p_EndOfPreviousLine && i + 1 < l_Count)
				continue;
			return static_cast<uint32_t>(i);
		}
	}
	return static_cast<uint32_t>(l_Count - 1);
}

size_t indexInLine(const TextLayout& p_Layout, const LaidLine& p_Line, const float p_X)
{
	if (p_Line.bidi)
		return indexInBidiLine(p_Layout, p_Line, p_X);
	for (uint32_t i = p_Line.glyphBegin; i < p_Line.glyphEnd; ++i)
	{
		const LaidGlyph& l_Glyph = p_Layout.glyphs[i];
		if (isCombining(l_Glyph.codepoint))
			continue;
		// The glyph's cluster extends over the combining marks that follow it
		float l_Advance = l_Glyph.advance;
		for (uint32_t k = i + 1; k < p_Line.glyphEnd && isCombining(p_Layout.glyphs[k].codepoint); ++k)
			l_Advance += p_Layout.glyphs[k].advance;
		if (p_X < l_Glyph.x + l_Advance * 0.5f)
			return l_Glyph.byteBegin;
	}
	return lineEndByte(p_Layout, p_Line);
}
} // namespace

TextLayout layoutText(const std::string_view p_Text, const Metrics& p_Metrics, const LayoutOptions& p_Options)
{
	TextLayout l_Layout;
	l_Layout.fontSize = p_Options.fontSize;
	const LineMetrics l_Metrics = p_Metrics.lineMetrics();
	const float l_FontSize = p_Options.fontSize;
	const float l_LineHeight = (l_Metrics.ascent - l_Metrics.descent + l_Metrics.lineGap) * l_FontSize;
	const float l_BaselineOffset = (l_Metrics.lineGap * 0.5f + l_Metrics.ascent) * l_FontSize;

	std::vector<Shaped> l_Glyphs;
	size_t l_Position = 0;
	while (true)
	{
		const size_t l_Newline = p_Text.find('\n', l_Position);
		const size_t l_End = l_Newline == std::string_view::npos ? p_Text.size() : l_Newline;
		bool l_Rtl = false;
		if (hasComplexScript(p_Text, l_Position, l_End))
			shapeComplex(p_Text, l_Position, l_End, p_Metrics, l_FontSize, l_Glyphs, l_Rtl);
		else
			shape(p_Text, l_Position, l_End, p_Metrics, l_FontSize, l_Glyphs);
		wrapParagraph(l_Layout, l_Glyphs, l_Position, p_Options.wrapWidth, l_Rtl);
		if (l_Newline == std::string_view::npos)
			break;
		l_Position = l_Newline + 1;
	}

	float l_Width = 0.f;
	for (const LaidLine& l_Line : l_Layout.lines)
		l_Width = std::max(l_Width, l_Line.visibleWidth);
	if (p_Options.wrapWidth > 0.f)
		l_Width = p_Options.wrapWidth;
	l_Width = std::max(l_Width, MIN_WIDTH_EM * l_FontSize);

	for (size_t i = 0; i < l_Layout.lines.size(); ++i)
	{
		LaidLine& l_Line = l_Layout.lines[i];
		float l_Offset = 0.f;
		if (l_Line.rtl)
		{
			// The spaces left hanging at a soft break sit at the left end of a right-to-left line
			const float l_Hang = l_Line.width - l_Line.visibleWidth;
			if (p_Options.align == TextAlign::Center)
				l_Offset = (l_Width - l_Line.visibleWidth) * 0.5f - l_Hang;
			else if (p_Options.align == TextAlign::Right)
				l_Offset = l_Width - l_Line.width;
			else
				l_Offset = -l_Hang;
		}
		else if (p_Options.align == TextAlign::Center)
			l_Offset = (l_Width - l_Line.visibleWidth) * 0.5f;
		else if (p_Options.align == TextAlign::Right)
			l_Offset = l_Width - l_Line.visibleWidth;
		l_Line.x = l_Offset;
		l_Line.top = static_cast<float>(i) * l_LineHeight;
		l_Line.baseline = l_Line.top + l_BaselineOffset;
		l_Line.height = l_LineHeight;
		// Glyph positions are stored absolute inside the box
		for (uint32_t k = l_Line.glyphBegin; k < l_Line.glyphEnd; ++k)
			l_Layout.glyphs[k].x += l_Offset;
	}
	l_Layout.size = Vec2{ l_Width, static_cast<float>(l_Layout.lines.size()) * l_LineHeight };
	return l_Layout;
}

CaretPosition caretAt(const TextLayout& p_Layout, const size_t p_Byte, const bool p_EndOfPreviousLine)
{
	const uint32_t l_Index = lineIndexFor(p_Layout, p_Byte, p_EndOfPreviousLine);
	const LaidLine& l_Line = p_Layout.lines[l_Index];
	return CaretPosition{ .line = l_Index, .x = caretX(p_Layout, l_Line, p_Byte), .top = l_Line.top, .height = l_Line.height };
}

size_t indexAtPoint(const TextLayout& p_Layout, const Vec2 p_Point)
{
	size_t l_Line = p_Layout.lines.size() - 1;
	for (size_t i = 0; i < p_Layout.lines.size(); ++i)
	{
		if (p_Point.y < p_Layout.lines[i].top + p_Layout.lines[i].height)
		{
			l_Line = i;
			break;
		}
	}
	return indexInLine(p_Layout, p_Layout.lines[l_Line], p_Point.x);
}

size_t moveVertically(const TextLayout& p_Layout, const size_t p_Byte, const int p_Delta, const float p_PreferredX)
{
	const int l_Current = static_cast<int>(caretAt(p_Layout, p_Byte).line);
	const int l_Target = l_Current + p_Delta;
	if (l_Target < 0)
		return 0;
	if (l_Target >= static_cast<int>(p_Layout.lines.size()))
		return p_Layout.lines.back().byteEnd;
	return indexInLine(p_Layout, p_Layout.lines[static_cast<size_t>(l_Target)], p_PreferredX);
}

size_t lineStart(const TextLayout& p_Layout, const size_t p_Byte)
{
	return p_Layout.lines[caretAt(p_Layout, p_Byte).line].byteBegin;
}

size_t lineEnd(const TextLayout& p_Layout, const size_t p_Byte)
{
	return lineEndByte(p_Layout, p_Layout.lines[caretAt(p_Layout, p_Byte).line]);
}

// Selected parts of a line with right-to-left glyphs: one rectangle per stretch of selected glyphs that touch
static void appendBidiSelection(const TextLayout& p_Layout, const LaidLine& p_Line, const size_t p_Begin, const size_t p_End, const bool p_NewlineSelected, std::vector<Rect>& p_Rects)
{
	std::vector<std::pair<float, float>> l_Spans;
	float l_MinX = p_Line.x;
	float l_MaxX = p_Line.x;
	bool l_Any = false;
	for (uint32_t i = p_Line.glyphBegin; i < p_Line.glyphEnd; ++i)
	{
		const LaidGlyph& l_Glyph = p_Layout.glyphs[i];
		l_MinX = l_Any ? std::min(l_MinX, l_Glyph.x) : l_Glyph.x;
		l_MaxX = l_Any ? std::max(l_MaxX, l_Glyph.x + l_Glyph.advance) : l_Glyph.x + l_Glyph.advance;
		l_Any = true;
		const size_t l_Lo = std::max<size_t>(p_Begin, l_Glyph.byteBegin);
		const size_t l_Hi = std::min<size_t>(p_End, l_Glyph.byteEnd);
		if (l_Lo >= l_Hi)
			continue;
		const float l_Length = static_cast<float>(l_Glyph.byteEnd - l_Glyph.byteBegin);
		const float l_From = static_cast<float>(l_Lo - l_Glyph.byteBegin) / l_Length; // part of the glyph's characters
		const float l_To = static_cast<float>(l_Hi - l_Glyph.byteBegin) / l_Length;
		if ((l_Glyph.level & 1) != 0)
			l_Spans.emplace_back(l_Glyph.x + l_Glyph.advance * (1.f - l_To), l_Glyph.x + l_Glyph.advance * (1.f - l_From));
		else
			l_Spans.emplace_back(l_Glyph.x + l_Glyph.advance * l_From, l_Glyph.x + l_Glyph.advance * l_To);
	}
	if (!l_Any)
	{
		l_MinX = l_MaxX = p_Line.x + p_Line.width;
	}
	if (p_NewlineSelected)
	{
		const float l_Mark = p_Layout.fontSize * NEWLINE_MARK_EM;
		if (p_Line.rtl)
			l_Spans.emplace_back(l_MinX - l_Mark, l_MinX);
		else
			l_Spans.emplace_back(l_MaxX, l_MaxX + l_Mark);
	}
	std::ranges::sort(l_Spans);
	for (size_t i = 0; i < l_Spans.size();)
	{
		float l_Begin = l_Spans[i].first;
		float l_Finish = l_Spans[i].second;
		size_t k = i + 1;
		while (k < l_Spans.size() && l_Spans[k].first <= l_Finish + 0.001f) // glyphs that touch make one rectangle
			l_Finish = std::max(l_Finish, l_Spans[k++].second);
		p_Rects.push_back(Rect{ .min = DVec2{ static_cast<double>(l_Begin), static_cast<double>(p_Line.top) }, .max = DVec2{ static_cast<double>(l_Finish), static_cast<double>(p_Line.top + p_Line.height) } });
		i = k;
	}
}

bool rightToLeftAt(const TextLayout& p_Layout, const size_t p_Byte)
{
	const LaidLine& l_Line = p_Layout.lines[lineIndexFor(p_Layout, p_Byte, false)];
	if (const LaidGlyph* l_Glyph = glyphAt(p_Layout, l_Line, p_Byte))
		return (l_Glyph->level & 1) != 0;
	if (l_Line.glyphBegin != l_Line.glyphEnd)
		return (p_Layout.glyphs[l_Line.glyphEnd - 1].level & 1) != 0; // after the last character
	return l_Line.rtl;
}

size_t moveVisually(const TextLayout& p_Layout, const size_t p_Byte, const int p_Direction)
{
	const uint32_t l_Index = lineIndexFor(p_Layout, p_Byte, false);
	const LaidLine& l_Line = p_Layout.lines[l_Index];
	if (!l_Line.bidi || l_Line.glyphBegin == l_Line.glyphEnd)
		return NO_VISUAL_MOVE;

	// Every place the caret can stand on this line: glyph edges, except those between a letter and its marks
	std::vector<uint32_t> l_Positions;
	l_Positions.push_back(l_Line.byteBegin);
	for (uint32_t i = l_Line.glyphBegin; i < l_Line.glyphEnd; ++i)
	{
		const LaidGlyph& l_Glyph = p_Layout.glyphs[i];
		if (!isCombining(l_Glyph.codepoint))
			l_Positions.push_back(l_Glyph.byteBegin);
		l_Positions.push_back(l_Glyph.byteEnd);
	}
	std::ranges::sort(l_Positions);
	l_Positions.erase(std::unique(l_Positions.begin(), l_Positions.end()), l_Positions.end());
	const size_t l_Last = lineEndByte(p_Layout, l_Line);

	const float l_From = caretXBidi(p_Layout, l_Line, p_Byte);
	constexpr float EPSILON = 0.001f;
	size_t l_Best = NO_VISUAL_MOVE;
	float l_BestGap = std::numeric_limits<float>::max();
	for (const uint32_t l_Position : l_Positions)
	{
		if (l_Position < l_Line.byteBegin || l_Position > l_Last || l_Position == p_Byte)
			continue;
		const float l_Shift = (caretXBidi(p_Layout, l_Line, l_Position) - l_From) * static_cast<float>(p_Direction);
		if (l_Shift <= EPSILON) // not further along in that direction (positions sharing a spot are one place)
			continue;
		const size_t l_Distance = l_Position > p_Byte ? l_Position - p_Byte : p_Byte - l_Position;
		if (l_Shift < l_BestGap - EPSILON || (std::abs(l_Shift - l_BestGap) <= EPSILON && l_Distance < (l_Best > p_Byte ? l_Best - p_Byte : p_Byte - l_Best)))
		{
			l_BestGap = l_Shift;
			l_Best = l_Position;
		}
	}
	if (l_Best != NO_VISUAL_MOVE)
		return l_Best;

	// The edge of the line: on to the line that continues it. The left edge of a right-to-left line is its end.
	const bool l_ToEnd = (p_Direction > 0) != l_Line.rtl; // arriving at the logical end of this line
	if (l_ToEnd)
		return l_Index + 1 < p_Layout.lines.size() ? p_Layout.lines[l_Index + 1].byteBegin : l_Last;
	return l_Index > 0 ? lineEndByte(p_Layout, p_Layout.lines[l_Index - 1]) : l_Line.byteBegin;
}

std::vector<Rect> selectionRects(const TextLayout& p_Layout, const size_t p_Begin, const size_t p_End)
{
	std::vector<Rect> l_Rects;
	if (p_Begin >= p_End)
		return l_Rects;
	for (const LaidLine& l_Line : p_Layout.lines)
	{
		const size_t l_Lo = std::max<size_t>(p_Begin, l_Line.byteBegin);
		const size_t l_Hi = std::min<size_t>(p_End, l_Line.byteEnd);
		const bool l_NewlineSelected = !l_Line.softBreak && p_Begin <= l_Line.byteEnd && p_End > l_Line.byteEnd;
		if (l_Lo >= l_Hi && !l_NewlineSelected)
			continue;
		if (l_Lo > l_Line.byteEnd || p_End < l_Line.byteBegin)
			continue;
		if (l_Line.bidi)
		{
			appendBidiSelection(p_Layout, l_Line, p_Begin, p_End, l_NewlineSelected, l_Rects);
			continue;
		}
		const double l_X0 = caretX(p_Layout, l_Line, l_Lo);
		double l_X1 = caretX(p_Layout, l_Line, l_Hi);
		if (l_NewlineSelected)
			l_X1 += static_cast<double>(p_Layout.fontSize * NEWLINE_MARK_EM);
		l_Rects.push_back(Rect{ .min = DVec2{ l_X0, static_cast<double>(l_Line.top) }, .max = DVec2{ l_X1, static_cast<double>(l_Line.top + l_Line.height) } });
	}
	return l_Rects;
}
} // namespace wb::text
