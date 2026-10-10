#include <string>
#include <vector>
#include <gtest/gtest.h>

import wb.text.subtitles;

TEST(Subtitles, ReadsSrtWithItsOddities)
{
	const std::string l_File = "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i> {\\an8}there\r\nsecond &amp; last\r\n\r\n"
		"2\r\n00:01:00,250 --> 00:01:03,000\r\nA minute in\r\n\r\n";
	const std::vector<wb::SubtitleCue> l_Cues = wb::parseSubtitles(l_File);
	ASSERT_EQ(l_Cues.size(), 2u);
	EXPECT_DOUBLE_EQ(l_Cues[0].start, 1.0);
	EXPECT_DOUBLE_EQ(l_Cues[0].end, 2.5);
	EXPECT_EQ(l_Cues[0].text, "Hello there\nsecond & last");
	EXPECT_DOUBLE_EQ(l_Cues[1].start, 60.25);
	EXPECT_EQ(l_Cues[1].text, "A minute in");
}

TEST(Subtitles, ReadsWebVttWithSettingsAndShortTimes)
{
	const std::string l_File = "WEBVTT\n\nNOTE a comment\n\ncue1\n00:03.000 --> 00:04.500 align:start position:10%\nFirst\n\n01:00:00.000 --> 01:00:01.000\nAn hour in\n";
	const std::vector<wb::SubtitleCue> l_Cues = wb::parseSubtitles(l_File);
	ASSERT_EQ(l_Cues.size(), 2u);
	EXPECT_DOUBLE_EQ(l_Cues[0].start, 3.0);
	EXPECT_DOUBLE_EQ(l_Cues[0].end, 4.5);
	EXPECT_EQ(l_Cues[0].text, "First");
	EXPECT_DOUBLE_EQ(l_Cues[1].start, 3600.0);
}

TEST(Subtitles, ReadsAssDialogue)
{
	const std::string l_File = "[Script Info]\nTitle: x\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
		"Dialogue: 0,0:00:05.50,0:00:07.00,Default,,0,0,0,,{\\i1}Italic{\\i0}\\Nnext, with a comma\n";
	const std::vector<wb::SubtitleCue> l_Cues = wb::parseSubtitles(l_File);
	ASSERT_EQ(l_Cues.size(), 1u);
	EXPECT_DOUBLE_EQ(l_Cues[0].start, 5.5);
	EXPECT_DOUBLE_EQ(l_Cues[0].end, 7.0);
	EXPECT_EQ(l_Cues[0].text, "Italic\nnext, with a comma");
}

TEST(Subtitles, OldEncodingsBecomeUtf8AndNonsenseIsRefused)
{
	// "caf\xE9" is Windows-1252 for "café"
	const std::vector<wb::SubtitleCue> l_Cues = wb::parseSubtitles("1\n00:00:01,000 --> 00:00:02,000\ncaf\xE9 \x93quoted\x94\n");
	ASSERT_EQ(l_Cues.size(), 1u);
	EXPECT_EQ(l_Cues[0].text, "caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D");
	EXPECT_TRUE(wb::parseSubtitles("just some words\nnothing timed").empty());
	EXPECT_TRUE(wb::parseSubtitles("").empty());
	EXPECT_TRUE(wb::parseSubtitles("00:00:01,000 --> nope\ntext\n").empty());
}

TEST(Subtitles, CuesAreOrderedByStart)
{
	const std::vector<wb::SubtitleCue> l_Cues = wb::parseSubtitles("1\n00:00:09,000 --> 00:00:10,000\nlate\n\n2\n00:00:01,000 --> 00:00:02,000\nearly\n");
	ASSERT_EQ(l_Cues.size(), 2u);
	EXPECT_EQ(l_Cues[0].text, "early");
}
