module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

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
};

bool isBreakAfter(const uint32_t p_Codepoint)
{
	return isSpace(p_Codepoint) || breaksAnywhere(p_Codepoint) || p_Codepoint == '-' || p_Codepoint == 0x2010 || p_Codepoint == 0x2013;
}

void shape(const std::string_view p_Text, const size_t p_Begin, const size_t p_End, const Metrics& p_Metrics, const float p_FontSize, std::vector<Shaped>& p_Out)
{
	p_Out.clear();
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

// Appends glyphs [p_First, p_Last) of the paragraph as one line
void emitLine(TextLayout& p_Layout, const std::vector<Shaped>& p_Glyphs, const size_t p_First, const size_t p_Last, const size_t p_ParagraphBegin, const bool p_Soft)
{
	LaidLine l_Line;
	l_Line.glyphBegin = static_cast<uint32_t>(p_Layout.glyphs.size());
	l_Line.softBreak = p_Soft;
	const uint32_t l_Index = static_cast<uint32_t>(p_Layout.lines.size());
	float l_X = 0.f;
	for (size_t i = p_First; i < p_Last; ++i)
	{
		const Shaped& l_Glyph = p_Glyphs[i];
		p_Layout.glyphs.push_back(LaidGlyph{ .codepoint = l_Glyph.codepoint, .font = l_Glyph.font, .glyph = l_Glyph.glyph, .byteBegin = l_Glyph.begin, .byteEnd = l_Glyph.end, .line = l_Index, .x = l_X, .advance = l_Glyph.advance });
		l_X += l_Glyph.advance;
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

void wrapParagraph(TextLayout& p_Layout, const std::vector<Shaped>& p_Glyphs, const size_t p_ParagraphBegin, const float p_WrapWidth)
{
	const size_t l_Count = p_Glyphs.size();
	if (p_WrapWidth <= 0.f)
	{
		emitLine(p_Layout, p_Glyphs, 0, l_Count, p_ParagraphBegin, false);
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
			emitLine(p_Layout, p_Glyphs, l_LineStart, l_Next, p_ParagraphBegin, true);
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
	emitLine(p_Layout, p_Glyphs, l_LineStart, l_Count, p_ParagraphBegin, false);
}

// Horizontal position of byte offset p_Byte on a line
float caretX(const TextLayout& p_Layout, const LaidLine& p_Line, const size_t p_Byte)
{
	for (uint32_t i = p_Line.glyphBegin; i < p_Line.glyphEnd; ++i)
	{
		if (p_Layout.glyphs[i].byteBegin >= p_Byte)
			return p_Layout.glyphs[i].x;
	}
	return p_Line.x + p_Line.width;
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
		shape(p_Text, l_Position, l_End, p_Metrics, l_FontSize, l_Glyphs);
		wrapParagraph(l_Layout, l_Glyphs, l_Position, p_Options.wrapWidth);
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
		if (p_Options.align == TextAlign::Center)
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
		const double l_X0 = caretX(p_Layout, l_Line, l_Lo);
		double l_X1 = caretX(p_Layout, l_Line, l_Hi);
		if (l_NewlineSelected)
			l_X1 += static_cast<double>(p_Layout.fontSize * NEWLINE_MARK_EM);
		l_Rects.push_back(Rect{ .min = DVec2{ l_X0, static_cast<double>(l_Line.top) }, .max = DVec2{ l_X1, static_cast<double>(l_Line.top + l_Line.height) } });
	}
	return l_Rects;
}
} // namespace wb::text
