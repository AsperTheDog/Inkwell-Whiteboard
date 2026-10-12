// Layout of text with right-to-left scripts: reading order stays logical, drawing order follows the bidirectional
// algorithm, and the caret, click and selection queries have to agree with where glyphs are drawn.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <gtest/gtest.h>

import wb.math;
import wb.doc.object;
import wb.text.utf8;
import wb.text.layout;

namespace
{
using wb::text::LayoutOptions;
using wb::text::TextLayout;

// Every character is half an em wide, the line is one em high; at font size 10 a character is 5 wide
class Fixed final : public wb::text::Metrics
{
public:
	[[nodiscard]] wb::text::GlyphInfo glyph(const uint32_t p_Codepoint) const override { return wb::text::GlyphInfo{ .font = 1u, .glyph = p_Codepoint, .advance = 0.5f }; }
	[[nodiscard]] wb::text::LineMetrics lineMetrics() const override { return {}; }
};

// Like Fixed, but shapes a run into one glyph per two characters (a stand-in for ligatures) and records what it was asked
class Pairs final : public wb::text::Metrics
{
public:
	[[nodiscard]] wb::text::GlyphInfo glyph(const uint32_t p_Codepoint) const override { return wb::text::GlyphInfo{ .font = 1u, .glyph = p_Codepoint, .advance = 0.5f }; }
	[[nodiscard]] wb::text::LineMetrics lineMetrics() const override { return {}; }
	[[nodiscard]] bool shapeRun(const std::string_view p_Text, const size_t p_Begin, const size_t p_End, const bool p_Rtl, std::vector<ShapedGlyph>& p_Out) const override
	{
		runs.push_back({ p_Begin, p_End, p_Rtl });
		size_t l_Index = p_Begin;
		while (l_Index < p_End)
		{
			const size_t l_Start = l_Index;
			size_t l_Characters = 0;
			while (l_Index < p_End && l_Characters < 2)
			{
				(void)wb::text::decodeNext(p_Text, l_Index);
				++l_Characters;
			}
			p_Out.push_back(ShapedGlyph{ .font = 1u, .glyph = 7u, .advance = 0.5f * static_cast<float>(l_Characters), .byteBegin = static_cast<uint32_t>(l_Start), .byteEnd = static_cast<uint32_t>(l_Index) });
		}
		return true;
	}
	struct Run
	{
		size_t begin;
		size_t end;
		bool rtl;
	};
	mutable std::vector<Run> runs;
};

TextLayout layout(const std::string_view p_Text, const float p_Wrap = 0.f, const wb::TextAlign p_Align = wb::TextAlign::Left)
{
	return wb::text::layoutText(p_Text, Fixed{}, LayoutOptions{ .fontSize = 10.f, .wrapWidth = p_Wrap, .align = p_Align });
}

// x of the glyph that draws the character starting at byte p_Byte
float glyphX(const TextLayout& p_Layout, const size_t p_Byte)
{
	for (const wb::text::LaidGlyph& l_Glyph : p_Layout.glyphs)
	{
		if (l_Glyph.byteBegin == p_Byte)
			return l_Glyph.x;
	}
	return -1000.f;
}

// Hebrew letters (two bytes each)
const std::string SHALOM = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"; // shin lamed vav final mem
} // namespace

TEST(BidiLayout, PlainTextIsLaidOutInReadingOrder)
{
	const TextLayout l_Layout = layout("abc");
	ASSERT_EQ(l_Layout.lines.size(), 1u);
	EXPECT_FALSE(l_Layout.lines[0].bidi);
	EXPECT_FALSE(l_Layout.lines[0].rtl);
	EXPECT_FLOAT_EQ(l_Layout.glyphs[2].x, 10.f);
}

TEST(BidiLayout, HebrewRunsRightToLeft)
{
	const TextLayout l_Layout = layout(SHALOM);
	ASSERT_EQ(l_Layout.glyphs.size(), 4u);
	EXPECT_TRUE(l_Layout.lines[0].rtl);
	EXPECT_TRUE(l_Layout.lines[0].bidi);
	// Glyphs stay in reading order; the first letter is drawn rightmost
	EXPECT_FLOAT_EQ(l_Layout.glyphs[0].x, 15.f);
	EXPECT_FLOAT_EQ(l_Layout.glyphs[3].x, 0.f);
	EXPECT_EQ(l_Layout.glyphs[0].level, 1);
	EXPECT_FLOAT_EQ(l_Layout.size.x, 20.f);
}

TEST(BidiLayout, CaretClickAndSelectionInHebrew)
{
	const TextLayout l_Layout = layout(SHALOM);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 0).x, 20.f); // before the first letter: its right edge
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 2).x, 15.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 6).x, 5.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 8).x, 0.f); // the end of the text is at the left

	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 19.f, 3.f }), 0u);
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 16.f, 3.f }), 2u);
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 1.f, 3.f }), 8u);
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 500.f, 3.f }), 0u); // past the right edge: the start of the text
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { -500.f, 3.f }), 8u);

	const std::vector<wb::Rect> l_Rects = wb::text::selectionRects(l_Layout, 2, 6); // the middle two letters
	ASSERT_EQ(l_Rects.size(), 1u);
	EXPECT_DOUBLE_EQ(l_Rects[0].min.x, 5.0);
	EXPECT_DOUBLE_EQ(l_Rects[0].max.x, 15.0);
}

TEST(BidiLayout, HebrewInsideEnglishIsReversedInPlace)
{
	// "ab " + Hebrew + " cd": the Hebrew word is a right-to-left island in a left-to-right line
	const std::string l_Text = "ab " + SHALOM + " cd";
	const TextLayout l_Layout = layout(l_Text);
	EXPECT_FALSE(l_Layout.lines[0].rtl);
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 0), 0.f);   // a
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 3), 30.f);  // shin: the island's right end
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 9), 15.f);  // final mem: its left end
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 12), 40.f); // c
	EXPECT_FLOAT_EQ(l_Layout.size.x, 50.f);

	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 3).x, 35.f);  // before shin: its right edge
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 9).x, 20.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 14).x, 50.f); // the end

	// Selecting "b", the space and shin gives two rectangles: they are not next to each other on screen
	const std::vector<wb::Rect> l_Rects = wb::text::selectionRects(l_Layout, 1, 5);
	ASSERT_EQ(l_Rects.size(), 2u);
	EXPECT_DOUBLE_EQ(l_Rects[0].min.x, 5.0);
	EXPECT_DOUBLE_EQ(l_Rects[0].max.x, 15.0);
	EXPECT_DOUBLE_EQ(l_Rects[1].min.x, 30.0);
	EXPECT_DOUBLE_EQ(l_Rects[1].max.x, 35.0);
	// The whole Hebrew word is one rectangle
	const std::vector<wb::Rect> l_Word = wb::text::selectionRects(l_Layout, 3, 11);
	ASSERT_EQ(l_Word.size(), 1u);
	EXPECT_DOUBLE_EQ(l_Word[0].min.x, 15.0);
	EXPECT_DOUBLE_EQ(l_Word[0].max.x, 35.0);
}

TEST(BidiLayout, EnglishInsideHebrewKeepsItsOrder)
{
	// Hebrew first makes the paragraph right to left; "abc" stays left to right inside it
	const std::string l_Text = SHALOM + " abc";
	const TextLayout l_Layout = layout(l_Text);
	EXPECT_TRUE(l_Layout.lines[0].rtl);
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 9), 0.f);   // a: leftmost
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 11), 10.f); // c
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 8), 15.f);  // the space between the words
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 0), 35.f);  // shin: rightmost
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 0).x, 40.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 9).x, 0.f);   // before 'a'
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 12).x, 15.f); // after 'c'
	EXPECT_TRUE(wb::text::rightToLeftAt(l_Layout, 0));
	EXPECT_FALSE(wb::text::rightToLeftAt(l_Layout, 9));
}

TEST(BidiLayout, NumbersInHebrewReadLeftToRight)
{
	const std::string l_Text = SHALOM + " 123";
	const TextLayout l_Layout = layout(l_Text);
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 9), 0.f);   // 1
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 10), 5.f);  // 2
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 11), 10.f); // 3
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 0), 35.f);
}

TEST(BidiLayout, WrappedHebrewHangsItsSpaceOnTheLeft)
{
	const std::string l_Text = SHALOM + " " + SHALOM;
	// Left aligned: the word starts at 0 and the space hangs out to the left
	const TextLayout l_Left = layout(l_Text, 22.f, wb::TextAlign::Left);
	ASSERT_EQ(l_Left.lines.size(), 2u);
	EXPECT_TRUE(l_Left.lines[0].softBreak);
	EXPECT_FLOAT_EQ(glyphX(l_Left, 8), -5.f); // the space
	EXPECT_FLOAT_EQ(glyphX(l_Left, 6), 0.f);  // final mem, the left end of the word
	EXPECT_FLOAT_EQ(glyphX(l_Left, 0), 15.f);
	// Right aligned: the word ends at the right edge
	const TextLayout l_Right = layout(l_Text, 22.f, wb::TextAlign::Right);
	EXPECT_FLOAT_EQ(glyphX(l_Right, 0) + 5.f, 22.f);
	EXPECT_EQ(l_Right.glyphs.size(), l_Left.glyphs.size());
	// Centred: the word's middle is the box's middle
	const TextLayout l_Centre = layout(l_Text, 22.f, wb::TextAlign::Center);
	EXPECT_FLOAT_EQ(glyphX(l_Centre, 0) + 5.f - 10.f, 11.f);
}

TEST(BidiLayout, SoftBreaksStayInReadingOrder)
{
	const std::string l_Text = SHALOM + " " + SHALOM + " " + SHALOM;
	const TextLayout l_Layout = layout(l_Text, 22.f);
	ASSERT_EQ(l_Layout.lines.size(), 3u);
	for (size_t i = 1; i < l_Layout.lines.size(); ++i)
		EXPECT_EQ(l_Layout.lines[i].byteBegin, l_Layout.lines[i - 1].byteEnd); // the space stays on the line it follows
	EXPECT_EQ(l_Layout.lines[2].byteEnd, l_Text.size());
}

TEST(BidiLayout, NewlinesSeparateParagraphsOfDifferentDirections)
{
	const std::string l_Text = SHALOM + "\nabc";
	const TextLayout l_Layout = layout(l_Text);
	ASSERT_EQ(l_Layout.lines.size(), 2u);
	EXPECT_TRUE(l_Layout.lines[0].rtl);
	EXPECT_FALSE(l_Layout.lines[1].rtl);
	EXPECT_FLOAT_EQ(glyphX(l_Layout, 9), 0.f);
	// A selection through the break marks the break at the left end of the right-to-left line
	const std::vector<wb::Rect> l_Rects = wb::text::selectionRects(l_Layout, 0, 10);
	ASSERT_GE(l_Rects.size(), 1u);
	EXPECT_DOUBLE_EQ(l_Rects[0].min.x, -3.0);
	EXPECT_DOUBLE_EQ(l_Rects[0].max.x, 20.0);
}

TEST(BidiLayout, CaretAndClickAgreeEverywhere)
{
	const std::vector<std::string> l_Texts = {
		SHALOM,
		"ab " + SHALOM + " cd",
		SHALOM + " abc",
		SHALOM + " 123 " + SHALOM,
		"x " + SHALOM + " " + SHALOM + " y",
	};
	for (const std::string& l_Text : l_Texts)
	{
		const TextLayout l_Layout = layout(l_Text);
		for (size_t l_Byte = 0; l_Byte <= l_Text.size(); l_Byte = wb::text::nextCluster(l_Text, l_Byte))
		{
			const float l_X = wb::text::caretAt(l_Layout, l_Byte).x;
			// Where two reading positions share a screen position (the seam of two runs) either is right: the caret
			// has to come out where the click was
			const size_t l_Found = wb::text::indexAtPoint(l_Layout, { l_X, 3.f });
			EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, l_Found).x, l_X) << "text " << l_Text << " byte " << l_Byte << " found " << l_Found;
			if (l_Byte == l_Text.size())
				break;
		}
	}
}

TEST(BidiLayout, SelectionCoversExactlyTheSelectedCharacters)
{
	const std::string l_Text = "ab " + SHALOM + " " + SHALOM + " cd";
	const TextLayout l_Layout = layout(l_Text);
	std::vector<size_t> l_Boundaries;
	for (size_t l_Byte = 0; l_Byte <= l_Text.size(); l_Byte = wb::text::nextCluster(l_Text, l_Byte))
	{
		l_Boundaries.push_back(l_Byte);
		if (l_Byte == l_Text.size())
			break;
	}
	for (size_t a = 0; a < l_Boundaries.size(); ++a)
	{
		for (size_t b = a + 1; b < l_Boundaries.size(); ++b)
		{
			double l_Covered = 0.0;
			const std::vector<wb::Rect> l_Rects = wb::text::selectionRects(l_Layout, l_Boundaries[a], l_Boundaries[b]);
			for (const wb::Rect& l_Rect : l_Rects)
				l_Covered += l_Rect.max.x - l_Rect.min.x;
			// Five pixels per character
			EXPECT_DOUBLE_EQ(l_Covered, 5.0 * static_cast<double>(b - a)) << "from " << l_Boundaries[a] << " to " << l_Boundaries[b];
			for (size_t i = 1; i < l_Rects.size(); ++i)
				EXPECT_GT(l_Rects[i].min.x, l_Rects[i - 1].max.x) << "rectangles of one selection must not touch";
		}
	}
}

TEST(BidiLayout, ShapedGlyphsCanCoverSeveralCharacters)
{
	// Four Hebrew letters shaped into two two-letter glyphs (like a ligature): 10 wide each
	const std::string l_Text = SHALOM;
	const Pairs l_Metrics;
	const TextLayout l_Layout = wb::text::layoutText(l_Text, l_Metrics, LayoutOptions{ .fontSize = 10.f });
	ASSERT_EQ(l_Metrics.runs.size(), 1u);
	EXPECT_TRUE(l_Metrics.runs[0].rtl);
	ASSERT_EQ(l_Layout.glyphs.size(), 2u);
	EXPECT_EQ(l_Layout.glyphs[0].byteBegin, 0u);
	EXPECT_EQ(l_Layout.glyphs[0].byteEnd, 4u);
	EXPECT_FLOAT_EQ(l_Layout.glyphs[0].x, 10.f);
	EXPECT_FLOAT_EQ(l_Layout.glyphs[1].x, 0.f);

	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 0).x, 20.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 2).x, 15.f); // between the two letters of the first glyph
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 4).x, 10.f);
	// Selecting only the first letter covers the right half of its glyph
	const std::vector<wb::Rect> l_Rects = wb::text::selectionRects(l_Layout, 0, 2);
	ASSERT_EQ(l_Rects.size(), 1u);
	EXPECT_DOUBLE_EQ(l_Rects[0].min.x, 15.0);
	EXPECT_DOUBLE_EQ(l_Rects[0].max.x, 20.0);
}

TEST(BidiLayout, RunsAreSplitByDirection)
{
	const std::string l_Text = "ab " + SHALOM + " cd";
	const Pairs l_Metrics;
	(void)wb::text::layoutText(l_Text, l_Metrics, LayoutOptions{ .fontSize = 10.f });
	// "ab " (left to right), the Hebrew word (right to left), " cd" (left to right)
	ASSERT_EQ(l_Metrics.runs.size(), 3u);
	EXPECT_FALSE(l_Metrics.runs[0].rtl);
	EXPECT_TRUE(l_Metrics.runs[1].rtl);
	EXPECT_FALSE(l_Metrics.runs[2].rtl);
	EXPECT_EQ(l_Metrics.runs[1].begin, 3u);
	EXPECT_EQ(l_Metrics.runs[1].end, 11u);
}

TEST(BidiLayout, VerticalMovementWorksOnRightToLeftLines)
{
	const std::string l_Text = SHALOM + "\n" + SHALOM + "\n" + SHALOM;
	const TextLayout l_Layout = layout(l_Text);
	const float l_X = wb::text::caretAt(l_Layout, 2).x;
	const size_t l_Down = wb::text::moveVertically(l_Layout, 2, 1, l_X);
	EXPECT_EQ(l_Down, 11u); // the same position one line down
	EXPECT_EQ(wb::text::moveVertically(l_Layout, l_Down, -1, l_X), 2u);
}

TEST(BidiLayout, HomeAndEndAreReadingPositions)
{
	const TextLayout l_Layout = layout(SHALOM);
	EXPECT_EQ(wb::text::lineStart(l_Layout, 4), 0u);
	EXPECT_EQ(wb::text::lineEnd(l_Layout, 4), 8u);
}

TEST(BidiLayout, EmptyRightToLeftTextAndFormattingCharacters)
{
	// A right-to-left mark alone, and Hebrew with marks: nothing may crash, and the line count stays right
	const TextLayout l_Mark = layout("\xE2\x80\x8F");
	EXPECT_EQ(l_Mark.lines.size(), 1u);
	const TextLayout l_Mixed = layout("\xE2\x80\x8F" + SHALOM + "\xE2\x80\x8E" + "abc");
	EXPECT_EQ(l_Mixed.lines.size(), 1u);
	EXPECT_EQ(l_Mixed.glyphs.size(), 9u);
}

// Random mixes of English, Hebrew, Arabic, digits and punctuation, with and without wrapping: whatever the text, the
// layout has to tile its lines with glyphs, keep every byte in exactly one glyph, and agree with itself about carets
TEST(BidiLayout, RandomTextKeepsItsInvariants)
{
	const std::vector<std::string> l_Pieces = { "a", "b", "c", " ", " ", "1", "2", ".", ",", "(", ")", "\n",
		"\xD7\x90", "\xD7\x91", "\xD7\x92", "\xD7\x93", "\xD8\xA7", "\xD8\xA8", "\xD8\xAA", "\xD9\x84", "\xE2\x80\x8F", "\xE2\x80\x8E" };
	uint32_t l_State = 12345u;
	const auto l_Next = [&l_State](const uint32_t p_Bound) { l_State = l_State * 1664525u + 1013904223u; return (l_State >> 8) % p_Bound; };
	for (int l_Round = 0; l_Round < 400; ++l_Round)
	{
		std::string l_Text;
		const uint32_t l_Length = l_Next(24);
		for (uint32_t i = 0; i < l_Length; ++i)
			l_Text += l_Pieces[l_Next(static_cast<uint32_t>(l_Pieces.size()))];
		const float l_Wrap = l_Round % 3 == 0 ? 0.f : 10.f + static_cast<float>(l_Next(40));
		const wb::TextAlign l_Align = static_cast<wb::TextAlign>(l_Next(3));
		const TextLayout l_Layout = layout(l_Text, l_Wrap, l_Align);
		const std::string l_Context = "round " + std::to_string(l_Round) + " wrap " + std::to_string(l_Wrap);

		// Every byte belongs to exactly one glyph, in reading order across the whole text (line breaks to none)
		size_t l_Covered = 0;
		bool l_Ordered = true;
		for (const wb::text::LaidGlyph& l_Glyph : l_Layout.glyphs)
		{
			if (l_Glyph.byteBegin != l_Covered)
			{
				// Only a line break may lie between two glyphs
				l_Ordered = l_Ordered && l_Glyph.byteBegin > l_Covered && l_Text.substr(l_Covered, l_Glyph.byteBegin - l_Covered).find_first_not_of('\n') == std::string::npos;
			}
			l_Covered = l_Glyph.byteEnd;
		}
		EXPECT_TRUE(l_Ordered) << l_Context << " text " << l_Text;
		const size_t l_NewlineCount = static_cast<size_t>(std::ranges::count(l_Text, '\n'));
		size_t l_GlyphBytes = 0;
		for (const wb::text::LaidGlyph& l_Glyph : l_Layout.glyphs)
			l_GlyphBytes += l_Glyph.byteEnd - l_Glyph.byteBegin;
		EXPECT_EQ(l_GlyphBytes + l_NewlineCount, l_Text.size()) << l_Context;
		EXPECT_FALSE(l_Layout.lines.empty()) << l_Context;
		EXPECT_EQ(l_Layout.lines.size() >= l_NewlineCount + 1, true) << l_Context;

		for (const wb::text::LaidLine& l_Line : l_Layout.lines)
		{
			// Glyphs of a line tile an interval of width l_Line.width without overlapping
			std::vector<std::pair<float, float>> l_Spans;
			for (uint32_t i = l_Line.glyphBegin; i < l_Line.glyphEnd; ++i)
				l_Spans.emplace_back(l_Layout.glyphs[i].x, l_Layout.glyphs[i].x + l_Layout.glyphs[i].advance);
			std::ranges::sort(l_Spans);
			for (size_t i = 1; i < l_Spans.size(); ++i)
				EXPECT_NEAR(l_Spans[i].first, l_Spans[i - 1].second, 0.001f) << l_Context;
			if (!l_Spans.empty())
				EXPECT_NEAR(l_Spans.back().second - l_Spans.front().first, l_Line.width, 0.001f) << l_Context;
		}

		// A caret placed anywhere is found again by clicking there
		for (size_t l_Byte = 0; l_Byte <= l_Text.size(); l_Byte = l_Byte >= l_Text.size() ? l_Text.size() + 1 : wb::text::nextCluster(l_Text, l_Byte))
		{
			const wb::text::CaretPosition l_Caret = wb::text::caretAt(l_Layout, l_Byte);
			if (l_Byte > wb::text::lineEnd(l_Layout, l_Byte))
				continue; // among the spaces left hanging at a soft break: not a place to click (as in left-to-right text)
			if (!l_Layout.lines[l_Caret.line].bidi)
				continue; // lines without right-to-left glyphs take the plain path, which this change does not touch
			const size_t l_Found = wb::text::indexAtPoint(l_Layout, { l_Caret.x, l_Caret.top + 1.f });
			EXPECT_NEAR(wb::text::caretAt(l_Layout, l_Found).x, l_Caret.x, 0.001f) << l_Context << " byte " << l_Byte << " text " << l_Text;
			if (l_Byte == l_Text.size())
				break;
		}

		// A selection covers five pixels per selected character on the lines it touches (line breaks add a mark)
		const size_t l_A = l_Next(static_cast<uint32_t>(l_Text.size() + 1));
		const size_t l_B = l_Next(static_cast<uint32_t>(l_Text.size() + 1));
		const size_t l_From = wb::text::previousCluster(l_Text, std::min(l_A, l_B) + (std::min(l_A, l_B) < l_Text.size() ? 1 : 0));
		const size_t l_To = wb::text::nextCluster(l_Text, std::max(l_A, l_B) > 0 ? wb::text::previousCluster(l_Text, std::max(l_A, l_B)) : 0);
		if (l_From < l_To && l_To <= l_Text.size())
		{
			double l_Width = 0.0;
			for (const wb::Rect& l_Rect : wb::text::selectionRects(l_Layout, l_From, l_To))
				l_Width += l_Rect.max.x - l_Rect.min.x;
			size_t l_Characters = 0;
			size_t l_Breaks = 0;
			for (size_t l_At = l_From; l_At < l_To; l_At = wb::text::nextCluster(l_Text, l_At))
			{
				if (l_Text[l_At] == '\n')
					++l_Breaks;
				else
					l_Characters += 1;
			}
			// Formatting marks are zero wide in the real layout but five here; breaks add a 3 wide mark
			EXPECT_GE(l_Width, 0.0) << l_Context;
			EXPECT_LE(l_Width, 5.0 * static_cast<double>(l_Characters) + 3.0 * static_cast<double>(l_Breaks) + 0.001) << l_Context << " selection " << l_From << ".." << l_To << " text " << l_Text;
		}
	}
}
