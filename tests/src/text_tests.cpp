#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <glm/glm.hpp>

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.commands;
import wb.doc.hit;
import wb.doc.edit;
import wb.io.serializer;
import wb.text.utf8;
import wb.text.layout;
import wb.text.edit;

namespace
{
using wb::DVec2;

// Every character is half an em wide; the line is one em high (0.8 above the baseline, 0.2 below). Combining marks
// advance like any other glyph here; the layout zeroes them.
class FixedMetrics final : public wb::text::Metrics
{
public:
	[[nodiscard]] wb::text::GlyphInfo glyph(const uint32_t p_Codepoint) const override
	{
		return wb::text::GlyphInfo{ .font = p_Codepoint == 'W' ? 2u : 1u, .advance = p_Codepoint == 'i' ? 0.25f : 0.5f };
	}
	[[nodiscard]] wb::text::LineMetrics lineMetrics() const override { return {}; }
};

wb::text::TextLayout layout(const std::string_view p_Text, const float p_Wrap = 0.f, const wb::TextAlign p_Align = wb::TextAlign::Left)
{
	return wb::text::layoutText(p_Text, FixedMetrics{}, wb::text::LayoutOptions{ .fontSize = 10.f, .wrapWidth = p_Wrap, .align = p_Align });
}

wb::ObjectId addText(wb::Document& p_Document, const DVec2 p_Center, std::string p_Text = "hello")
{
	auto l_Object = std::make_unique<wb::Object>();
	l_Object->id = p_Document.allocateId();
	l_Object->transform = wb::Affine2::translate(p_Center);
	wb::TextData l_Data;
	l_Data.text = std::move(p_Text);
	l_Data.size = { 60.f, 20.f };
	l_Object->payload = std::move(l_Data);
	const wb::ObjectId l_Id = l_Object->id;
	p_Document.insert(std::move(l_Object));
	return l_Id;
}
} // namespace

// ------------------------------------------------------------------------------------------------ utf8

TEST(Utf8, DecodesAndEncodesRoundTrip)
{
	std::string l_Encoded;
	for (const uint32_t l_Code : { 0x41u, 0xE9u, 0x20ACu, 0x1F600u })
		wb::text::appendCodepoint(l_Encoded, l_Code);
	EXPECT_EQ(l_Encoded.size(), 1u + 2u + 3u + 4u);
	size_t l_Index = 0;
	EXPECT_EQ(wb::text::decodeNext(l_Encoded, l_Index), 0x41u);
	EXPECT_EQ(wb::text::decodeNext(l_Encoded, l_Index), 0xE9u);
	EXPECT_EQ(wb::text::decodeNext(l_Encoded, l_Index), 0x20ACu);
	EXPECT_EQ(wb::text::decodeNext(l_Encoded, l_Index), 0x1F600u);
	EXPECT_EQ(l_Index, l_Encoded.size());
}

TEST(Utf8, MalformedBytesBecomeReplacementCharacters)
{
	const std::string l_Bad = "a\xC3(\xFF" "b";
	size_t l_Index = 0;
	EXPECT_EQ(wb::text::decodeNext(l_Bad, l_Index), 'a');
	EXPECT_EQ(wb::text::decodeNext(l_Bad, l_Index), wb::text::REPLACEMENT_CHAR);
	EXPECT_EQ(wb::text::decodeNext(l_Bad, l_Index), '(');
	EXPECT_EQ(wb::text::decodeNext(l_Bad, l_Index), wb::text::REPLACEMENT_CHAR);
	EXPECT_EQ(wb::text::decodeNext(l_Bad, l_Index), 'b');
	// Overlong and surrogate encodings are not accepted either
	size_t l_Overlong = 0;
	EXPECT_EQ(wb::text::decodeNext(std::string("\xC0\x80"), l_Overlong), wb::text::REPLACEMENT_CHAR);
}

TEST(Utf8, SanitizeNormalizesLineBreaksAndDropsControls)
{
	EXPECT_EQ(wb::text::sanitize("a\r\nb\rc\x01\x7F" "d\te"), "a\nb\ncd\te");
}

TEST(Utf8, ClustersKeepCombiningMarksTogether)
{
	const std::string l_Text = "e\xCC\x81" "x"; // e + combining acute, x
	EXPECT_EQ(wb::text::nextCluster(l_Text, 0), 3u);
	EXPECT_EQ(wb::text::previousCluster(l_Text, 3), 0u);
	EXPECT_EQ(wb::text::previousCluster(l_Text, 4), 3u);
	EXPECT_EQ(wb::text::nextCluster(l_Text, 4), 4u);
}

TEST(Utf8, WordMovement)
{
	const std::string l_Text = "foo  bar.baz\nqux";
	EXPECT_EQ(wb::text::nextWord(l_Text, 0), 5u);   // to the start of "bar"
	EXPECT_EQ(wb::text::nextWord(l_Text, 5), 8u);   // "bar" ends at the dot
	EXPECT_EQ(wb::text::previousWord(l_Text, 8), 5u);
	EXPECT_EQ(wb::text::previousWord(l_Text, 5), 0u);
	EXPECT_EQ(wb::text::previousWord(l_Text, 13), 12u); // from the start of a line to the line break before it
	size_t l_Begin = 0, l_End = 0;
	wb::text::wordRange(l_Text, 6, l_Begin, l_End);
	EXPECT_EQ(l_Begin, 5u);
	EXPECT_EQ(l_End, 8u);
}

// ------------------------------------------------------------------------------------------------ layout

TEST(TextLayout, SingleLine)
{
	const wb::text::TextLayout l_Layout = layout("hello");
	ASSERT_EQ(l_Layout.lines.size(), 1u);
	EXPECT_FLOAT_EQ(l_Layout.size.x, 25.f);
	EXPECT_FLOAT_EQ(l_Layout.size.y, 10.f);
	EXPECT_FLOAT_EQ(l_Layout.lines[0].baseline, 8.f);
	ASSERT_EQ(l_Layout.glyphs.size(), 5u);
	EXPECT_FLOAT_EQ(l_Layout.glyphs[2].x, 10.f);
	EXPECT_EQ(l_Layout.glyphs[2].byteBegin, 2u);
}

TEST(TextLayout, EmptyTextHasOneLine)
{
	const wb::text::TextLayout l_Layout = layout("");
	ASSERT_EQ(l_Layout.lines.size(), 1u);
	EXPECT_GT(l_Layout.size.x, 0.f);
	EXPECT_FLOAT_EQ(l_Layout.size.y, 10.f);
	EXPECT_EQ(wb::text::layoutText("a\n", FixedMetrics{}, {}).lines.size(), 2u);
}

TEST(TextLayout, LineBreaks)
{
	const wb::text::TextLayout l_Layout = layout("ab\n\ncde");
	ASSERT_EQ(l_Layout.lines.size(), 3u);
	EXPECT_EQ(l_Layout.lines[0].byteBegin, 0u);
	EXPECT_EQ(l_Layout.lines[0].byteEnd, 2u);
	EXPECT_EQ(l_Layout.lines[1].byteBegin, 3u);
	EXPECT_EQ(l_Layout.lines[1].byteEnd, 3u);
	EXPECT_EQ(l_Layout.lines[2].byteBegin, 4u);
	EXPECT_FLOAT_EQ(l_Layout.size.x, 15.f);
	EXPECT_FLOAT_EQ(l_Layout.size.y, 30.f);
}

TEST(TextLayout, WrapsAtSpaces)
{
	// 'aaaa bbbb cccc' is 14 characters = 70 wide; a 40 wide box fits 8 characters
	const wb::text::TextLayout l_Layout = layout("aaaa bbbb cccc", 40.f);
	ASSERT_EQ(l_Layout.lines.size(), 3u);
	EXPECT_TRUE(l_Layout.lines[0].softBreak);
	EXPECT_EQ(l_Layout.lines[0].byteEnd, 5u); // "aaaa " (the space hangs)
	EXPECT_EQ(l_Layout.lines[1].byteBegin, 5u);
	EXPECT_FLOAT_EQ(l_Layout.lines[0].visibleWidth, 20.f);
	EXPECT_FLOAT_EQ(l_Layout.size.x, 40.f);
	EXPECT_FALSE(l_Layout.lines[2].softBreak);
}

TEST(TextLayout, LongWordBreaksInsideTheWord)
{
	const wb::text::TextLayout l_Layout = layout("abcdefghij", 20.f); // 4 characters per line
	ASSERT_EQ(l_Layout.lines.size(), 3u);
	EXPECT_EQ(l_Layout.lines[0].byteEnd, 4u);
	EXPECT_EQ(l_Layout.lines[1].byteEnd, 8u);
	EXPECT_EQ(l_Layout.lines[2].byteEnd, 10u);
}

TEST(TextLayout, Alignment)
{
	const wb::text::TextLayout l_Center = layout("ab\ncdef", 0.f, wb::TextAlign::Center);
	EXPECT_FLOAT_EQ(l_Center.lines[0].x, 5.f);
	EXPECT_FLOAT_EQ(l_Center.glyphs[0].x, 5.f);
	const wb::text::TextLayout l_Right = layout("ab\ncdef", 0.f, wb::TextAlign::Right);
	EXPECT_FLOAT_EQ(l_Right.lines[0].x, 10.f);
	EXPECT_FLOAT_EQ(l_Right.lines[1].x, 0.f);
}

TEST(TextLayout, CombiningMarksTakeNoSpace)
{
	const std::string l_Text = "e\xCC\x81" "x";
	const wb::text::TextLayout l_Layout = layout(l_Text);
	ASSERT_EQ(l_Layout.glyphs.size(), 3u);
	EXPECT_FLOAT_EQ(l_Layout.glyphs[1].advance, 0.f);
	EXPECT_FLOAT_EQ(l_Layout.glyphs[2].x, 5.f);
}

TEST(TextLayout, CaretPositions)
{
	const wb::text::TextLayout l_Layout = layout("ab\ncd");
	const wb::text::CaretPosition l_Start = wb::text::caretAt(l_Layout, 0);
	EXPECT_EQ(l_Start.line, 0u);
	EXPECT_FLOAT_EQ(l_Start.x, 0.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 2).x, 10.f); // end of the first line, before the break
	EXPECT_EQ(wb::text::caretAt(l_Layout, 2).line, 0u);
	EXPECT_EQ(wb::text::caretAt(l_Layout, 3).line, 1u);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 4).x, 5.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 5).x, 10.f);
	EXPECT_FLOAT_EQ(wb::text::caretAt(l_Layout, 5).top, 10.f);
	EXPECT_EQ(wb::text::caretAt(l_Layout, 99).line, 1u); // beyond the end clamps
}

TEST(TextLayout, CaretAtSoftBreakBelongsToTheNextLine)
{
	const wb::text::TextLayout l_Layout = layout("aaaa bbbb", 25.f); // "aaaa " / "bbbb"
	ASSERT_EQ(l_Layout.lines.size(), 2u);
	EXPECT_EQ(wb::text::caretAt(l_Layout, 5).line, 1u);
	EXPECT_EQ(wb::text::caretAt(l_Layout, 5, true).line, 0u);
	// End on the first line goes before the hanging space
	EXPECT_EQ(wb::text::lineEnd(l_Layout, 2), 4u);
	EXPECT_EQ(wb::text::lineStart(l_Layout, 7), 5u);
}

TEST(TextLayout, IndexAtPoint)
{
	const wb::text::TextLayout l_Layout = layout("abcd\nef");
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 0.f, 3.f }), 0u);
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 4.f, 3.f }), 1u);  // past half of 'a'
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 2.f, 3.f }), 0u);
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 500.f, 3.f }), 4u); // beyond the line end: before the break
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 6.f, 14.f }), 6u);  // second line, between 'e' and 'f'
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 6.f, -50.f }), 1u); // above the box: first line
	EXPECT_EQ(wb::text::indexAtPoint(l_Layout, { 6.f, 500.f }), 6u); // below: last line
}

TEST(TextLayout, VerticalMovement)
{
	const wb::text::TextLayout l_Layout = layout("abcdef\nab\nabcdef");
	EXPECT_EQ(wb::text::moveVertically(l_Layout, 5, 1, 25.f), 9u);   // the short line ends before 25
	EXPECT_EQ(wb::text::moveVertically(l_Layout, 9, 1, 25.f), 15u);  // back out to x = 25 on the third line
	EXPECT_EQ(wb::text::moveVertically(l_Layout, 4, -1, 20.f), 0u);  // above the first line: start of the text
	EXPECT_EQ(wb::text::moveVertically(l_Layout, 14, 1, 20.f), 16u); // below the last line: end of the text
}

TEST(TextLayout, SelectionRectangles)
{
	const wb::text::TextLayout l_Layout = layout("abc\ndef");
	const std::vector<wb::Rect> l_Rects = wb::text::selectionRects(l_Layout, 1, 6);
	ASSERT_EQ(l_Rects.size(), 2u);
	EXPECT_DOUBLE_EQ(l_Rects[0].min.x, 5.0);
	EXPECT_GT(l_Rects[0].max.x, 15.0); // the selected line break is marked
	EXPECT_DOUBLE_EQ(l_Rects[1].min.x, 0.0);
	EXPECT_DOUBLE_EQ(l_Rects[1].max.x, 10.0);
	EXPECT_TRUE(wb::text::selectionRects(l_Layout, 2, 2).empty());
}

// ------------------------------------------------------------------------------------------------ editor

TEST(TextEditor, TypingAndDeleting)
{
	wb::text::TextEditor l_Editor;
	EXPECT_TRUE(l_Editor.insert("ab"));
	EXPECT_TRUE(l_Editor.insert("c"));
	EXPECT_EQ(l_Editor.text(), "abc");
	EXPECT_EQ(l_Editor.caret(), 3u);
	EXPECT_TRUE(l_Editor.backspace(false));
	EXPECT_EQ(l_Editor.text(), "ab");
	l_Editor.moveLeft(false, false);
	EXPECT_TRUE(l_Editor.erase(false));
	EXPECT_EQ(l_Editor.text(), "a");
	EXPECT_FALSE(l_Editor.erase(false)); // nothing after the caret
	EXPECT_FALSE(wb::text::TextEditor().backspace(false));
}

TEST(TextEditor, SelectionIsReplacedByTyping)
{
	wb::text::TextEditor l_Editor("hello world");
	l_Editor.selectAll();
	EXPECT_TRUE(l_Editor.hasSelection());
	EXPECT_EQ(l_Editor.selectedText(), "hello world");
	l_Editor.moveTo(5, false);
	l_Editor.moveTo(0, true);
	EXPECT_EQ(l_Editor.selectedText(), "hello");
	EXPECT_TRUE(l_Editor.insert("bye"));
	EXPECT_EQ(l_Editor.text(), "bye world");
	EXPECT_EQ(l_Editor.caret(), 3u);
	EXPECT_FALSE(l_Editor.hasSelection());
}

TEST(TextEditor, InputIsSanitized)
{
	wb::text::TextEditor l_Editor;
	EXPECT_TRUE(l_Editor.insert("a\r\nb"));
	EXPECT_EQ(l_Editor.text(), "a\nb");
	EXPECT_FALSE(l_Editor.insert("\x01\x02"));
}

TEST(TextEditor, UndoGroupsTypingAndRestoresTheCaret)
{
	wb::text::TextEditor l_Editor;
	for (const char* l_Char : { "h", "e", "l", "l", "o" })
		l_Editor.insert(l_Char);
	l_Editor.insert(" ");
	l_Editor.insert("w");
	EXPECT_EQ(l_Editor.text(), "hello w");
	l_Editor.undo(); // "w" (the space broke the group)
	EXPECT_EQ(l_Editor.text(), "hello ");
	l_Editor.undo(); // the space
	EXPECT_EQ(l_Editor.text(), "hello");
	l_Editor.undo(); // all of "hello"
	EXPECT_EQ(l_Editor.text(), "");
	EXPECT_FALSE(l_Editor.canUndo());
	l_Editor.redo();
	EXPECT_EQ(l_Editor.text(), "hello");
	EXPECT_EQ(l_Editor.caret(), 5u);
	l_Editor.insert("!"); // a new edit drops the redo history
	EXPECT_FALSE(l_Editor.canRedo());
}

TEST(TextEditor, WordDeletion)
{
	wb::text::TextEditor l_Editor("one two three");
	EXPECT_TRUE(l_Editor.backspace(true));
	EXPECT_EQ(l_Editor.text(), "one two ");
	l_Editor.moveTo(0, false);
	EXPECT_TRUE(l_Editor.erase(true));
	EXPECT_EQ(l_Editor.text(), "two ");
}

TEST(TextEditor, CompositionIsDisplayedButNotStored)
{
	wb::text::TextEditor l_Editor("ab");
	l_Editor.moveTo(1, false);
	l_Editor.setComposition("XYZ", 2);
	EXPECT_TRUE(l_Editor.composing());
	EXPECT_EQ(l_Editor.text(), "ab");
	EXPECT_EQ(l_Editor.displayText(), "aXYZb");
	EXPECT_EQ(l_Editor.displayCaret(), 3u);
	EXPECT_EQ(l_Editor.realIndex(5), 2u);
	EXPECT_EQ(l_Editor.displayIndex(2), 5u);
	EXPECT_EQ(l_Editor.realIndex(2), 1u); // inside the composition: the caret
	l_Editor.insert("é"); // committing replaces the composition
	EXPECT_FALSE(l_Editor.composing());
	EXPECT_EQ(l_Editor.text(), "aéb");
}

// ------------------------------------------------------------------------------------------------ objects

TEST(TextObject, HitTestAndSelectionUseTheBox)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addText(l_Doc, { 100.0, 100.0 }); // 60 x 20 around (100, 100)
	EXPECT_TRUE(wb::hitsPoint(*l_Doc.find(l_Id), { 120.0, 105.0 }, 0.0));
	EXPECT_FALSE(wb::hitsPoint(*l_Doc.find(l_Id), { 140.0, 100.0 }, 0.0));
	EXPECT_EQ(wb::pickTopmost(l_Doc, { 75.0, 95.0 }, 0.0), l_Id);
	EXPECT_EQ(wb::objectsInRect(l_Doc, wb::Rect::fromPoints({ 60.0, 80.0 }, { 90.0, 120.0 })), std::vector<wb::ObjectId>{ l_Id });
	EXPECT_TRUE(wb::objectsInRect(l_Doc, wb::Rect::fromPoints({ 0.0, 0.0 }, { 20.0, 20.0 })).empty());
}

TEST(TextObject, AnchoredTransformKeepsTheAlignedEdgeInPlace)
{
	const wb::Affine2 l_Old = wb::Affine2::translate({ 100.0, 100.0 }); // box 60 x 20: left edge at x = 70, top at y = 90
	const wb::Affine2 l_Left = wb::anchoredTextTransform(l_Old, { 60.f, 20.f }, { 100.f, 40.f }, wb::TextAlign::Left);
	EXPECT_DOUBLE_EQ(l_Left.apply({ -50.0, -20.0 }).x, 70.0);
	EXPECT_DOUBLE_EQ(l_Left.apply({ -50.0, -20.0 }).y, 90.0);
	const wb::Affine2 l_Center = wb::anchoredTextTransform(l_Old, { 60.f, 20.f }, { 100.f, 40.f }, wb::TextAlign::Center);
	EXPECT_DOUBLE_EQ(l_Center.translation.x, 100.0); // grows around the centre line
	EXPECT_DOUBLE_EQ(l_Center.apply({ 0.0, -20.0 }).y, 90.0);
	const wb::Affine2 l_Right = wb::anchoredTextTransform(l_Old, { 60.f, 20.f }, { 100.f, 40.f }, wb::TextAlign::Right);
	EXPECT_DOUBLE_EQ(l_Right.apply({ 50.0, -20.0 }).x, 130.0);
}

TEST(TextObject, EditIsUndoable)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_Id = addText(l_Doc, { 100.0, 100.0 });
	wb::TextData l_After = *l_Doc.find(l_Id)->text();
	l_After.text = "hello world";
	l_After.size = { 120.f, 20.f };
	wb::editText(l_Doc, l_History, l_Id, l_After, "Edit text");
	EXPECT_EQ(l_Doc.find(l_Id)->text()->text, "hello world");
	EXPECT_DOUBLE_EQ(l_Doc.find(l_Id)->worldBounds().min.x, 70.0); // the left edge did not move
	EXPECT_DOUBLE_EQ(l_Doc.find(l_Id)->worldBounds().max.x, 190.0);
	l_History.undo(l_Doc);
	EXPECT_EQ(l_Doc.find(l_Id)->text()->text, "hello");
	EXPECT_DOUBLE_EQ(l_Doc.find(l_Id)->worldBounds().max.x, 130.0);
	l_History.redo(l_Doc);
	EXPECT_EQ(l_Doc.find(l_Id)->text()->text, "hello world");
	// Nothing changes: no history entry
	const size_t l_Count = l_History.undoCount();
	wb::editText(l_Doc, l_History, l_Id, *l_Doc.find(l_Id)->text(), "Edit text");
	EXPECT_EQ(l_History.undoCount(), l_Count);
}

TEST(TextObject, RecolorAppliesToText)
{
	wb::Document l_Doc;
	wb::History l_History;
	const wb::ObjectId l_Id = addText(l_Doc, { 0.0, 0.0 });
	const wb::Color l_Before = l_Doc.find(l_Id)->text()->color;
	const std::array<wb::ObjectId, 1> l_Ids{ l_Id };
	wb::recolorObjects(l_Doc, l_History, l_Ids, wb::Color{ 1.f, 0.f, 0.f, 1.f });
	EXPECT_FLOAT_EQ(l_Doc.find(l_Id)->text()->color.r, 1.f);
	l_History.undo(l_Doc);
	EXPECT_EQ(l_Doc.find(l_Id)->text()->color, l_Before);
}

TEST(TextObject, SurvivesSaveAndLoad)
{
	wb::Document l_Doc;
	const wb::ObjectId l_Id = addText(l_Doc, { 12.5, -7.0 }, "line one\nlíne two ✓");
	{
		wb::TextData* l_Text = l_Doc.find(l_Id) ? const_cast<wb::Object*>(l_Doc.find(l_Id))->text() : nullptr;
		ASSERT_NE(l_Text, nullptr);
		l_Text->family = "Caveat";
		l_Text->style = wb::TextStyle::Bold | wb::TextStyle::Italic;
		l_Text->fontSize = 48.f;
		l_Text->align = wb::TextAlign::Right;
		l_Text->wrapWidth = 300.f;
		l_Text->color = wb::Color{ 0.1f, 0.2f, 0.3f, 0.5f };
	}
	l_Doc.setFontAsset(wb::FontAsset{ .family = "Caveat", .style = wb::TextStyle::Bold, .bytes = { 1, 2, 3, 4, 5 } });
	l_Doc.setFontAsset(wb::FontAsset{ .family = "Unused", .style = 0, .bytes = { 9 } });

	const std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Doc, {});
	wb::Document l_Loaded;
	wb::BoardMeta l_Meta;
	const wb::LoadResult l_Result = wb::deserializeBoard(l_Bytes, l_Loaded, l_Meta);
	ASSERT_TRUE(l_Result.ok) << l_Result.error;
	const wb::Object* l_Object = l_Loaded.find(l_Id);
	ASSERT_NE(l_Object, nullptr);
	ASSERT_NE(l_Object->text(), nullptr);
	EXPECT_EQ(*l_Object->text(), *l_Doc.find(l_Id)->text());
	EXPECT_DOUBLE_EQ(l_Object->transform.translation.x, 12.5);
	// Only the font a text uses is stored
	ASSERT_EQ(l_Loaded.fontAssets().size(), 1u);
	ASSERT_NE(l_Loaded.findFontAsset("Caveat", wb::TextStyle::Bold), nullptr);
	EXPECT_EQ(l_Loaded.findFontAsset("Caveat", wb::TextStyle::Bold)->bytes, (std::vector<uint8_t>{ 1, 2, 3, 4, 5 }));
	EXPECT_EQ(l_Loaded.findFontAsset("Unused", 0), nullptr);
}

TEST(TextObject, DamagedTextDataIsRejectedNotTrusted)
{
	wb::Document l_Doc;
	addText(l_Doc, { 0.0, 0.0 });
	std::vector<uint8_t> l_Bytes = wb::serializeBoard(l_Doc, {});
	// Every truncation must be reported as an error, never crash
	for (size_t l_Size = 0; l_Size < l_Bytes.size(); l_Size += 7)
	{
		wb::Document l_Target;
		wb::BoardMeta l_Meta;
		const wb::LoadResult l_Result = wb::deserializeBoard(std::span<const uint8_t>(l_Bytes.data(), l_Size), l_Target, l_Meta);
		EXPECT_FALSE(l_Result.ok);
	}
}
