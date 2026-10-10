module;
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module wb.text.subtitles;

namespace wb
{
namespace
{
bool validUtf8(const std::string_view p_Text)
{
	size_t i = 0;
	while (i < p_Text.size())
	{
		const uint8_t l_Byte = static_cast<uint8_t>(p_Text[i]);
		size_t l_Extra = 0;
		if (l_Byte < 0x80)
			l_Extra = 0;
		else if ((l_Byte & 0xE0) == 0xC0 && l_Byte >= 0xC2)
			l_Extra = 1;
		else if ((l_Byte & 0xF0) == 0xE0)
			l_Extra = 2;
		else if ((l_Byte & 0xF8) == 0xF0 && l_Byte <= 0xF4)
			l_Extra = 3;
		else
			return false;
		for (size_t k = 1; k <= l_Extra; ++k)
		{
			if (i + k >= p_Text.size() || (static_cast<uint8_t>(p_Text[i + k]) & 0xC0) != 0x80)
				return false;
		}
		i += l_Extra + 1;
	}
	return true;
}

void appendCodePoint(std::string& p_Out, const uint32_t p_Code)
{
	if (p_Code < 0x80)
	{
		p_Out.push_back(static_cast<char>(p_Code));
	}
	else if (p_Code < 0x800)
	{
		p_Out.push_back(static_cast<char>(0xC0 | (p_Code >> 6)));
		p_Out.push_back(static_cast<char>(0x80 | (p_Code & 0x3F)));
	}
	else
	{
		p_Out.push_back(static_cast<char>(0xE0 | (p_Code >> 12)));
		p_Out.push_back(static_cast<char>(0x80 | ((p_Code >> 6) & 0x3F)));
		p_Out.push_back(static_cast<char>(0x80 | (p_Code & 0x3F)));
	}
}

// Windows-1252 differs from Latin-1 only in 0x80..0x9F
uint32_t windows1252(const uint8_t p_Byte)
{
	static constexpr uint16_t HIGH[32] = { 0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
		0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };
	return p_Byte >= 0x80 && p_Byte < 0xA0 ? HIGH[p_Byte - 0x80] : p_Byte;
}

std::string trim(std::string p_Text)
{
	const auto l_Blank = [](const char p_Char) { return p_Char == ' ' || p_Char == '\t' || p_Char == '\r' || p_Char == '\n'; };
	while (!p_Text.empty() && l_Blank(p_Text.back()))
		p_Text.pop_back();
	size_t l_Start = 0;
	while (l_Start < p_Text.size() && l_Blank(p_Text[l_Start]))
		++l_Start;
	return p_Text.substr(l_Start);
}

std::vector<std::string_view> splitLines(const std::string_view p_Text)
{
	std::vector<std::string_view> l_Lines;
	size_t l_Start = 0;
	while (l_Start <= p_Text.size())
	{
		size_t l_End = p_Text.find('\n', l_Start);
		if (l_End == std::string_view::npos)
			l_End = p_Text.size();
		std::string_view l_Line = p_Text.substr(l_Start, l_End - l_Start);
		if (!l_Line.empty() && l_Line.back() == '\r')
			l_Line.remove_suffix(1);
		l_Lines.push_back(l_Line);
		l_Start = l_End + 1;
	}
	return l_Lines;
}

// "<i>", "{\an8}" and friends removed, the few HTML entities that turn up put back
std::string stripMarkup(const std::string_view p_Line)
{
	std::string l_Out;
	for (size_t i = 0; i < p_Line.size();)
	{
		const char l_Char = p_Line[i];
		if (l_Char == '<')
		{
			const size_t l_End = p_Line.find('>', i);
			if (l_End != std::string_view::npos && l_End - i <= 64)
			{
				i = l_End + 1;
				continue;
			}
		}
		else if (l_Char == '{')
		{
			const size_t l_End = p_Line.find('}', i);
			if (l_End != std::string_view::npos && l_End - i <= 128)
			{
				i = l_End + 1;
				continue;
			}
		}
		else if (l_Char == '&')
		{
			static constexpr std::pair<std::string_view, char> ENTITIES[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&nbsp;", ' ' }, { "&apos;", '\'' } };
			bool l_Done = false;
			for (const auto& [l_Name, l_Replacement] : ENTITIES)
			{
				if (p_Line.substr(i, l_Name.size()) == l_Name)
				{
					l_Out.push_back(l_Replacement);
					i += l_Name.size();
					l_Done = true;
					break;
				}
			}
			if (l_Done)
				continue;
		}
		l_Out.push_back(l_Char);
		++i;
	}
	return l_Out;
}

// "01:02:03,456", "01:02:03.456", "02:03.456" -> seconds; negative when it is no time
double parseTimestamp(std::string_view p_Text)
{
	while (!p_Text.empty() && (p_Text.front() == ' ' || p_Text.front() == '\t'))
		p_Text.remove_prefix(1);
	double l_Parts[3] = { 0.0, 0.0, 0.0 };
	int l_Count = 0;
	size_t i = 0;
	while (i < p_Text.size() && l_Count < 3)
	{
		size_t l_Digits = 0;
		double l_Value = 0.0;
		while (i < p_Text.size() && p_Text[i] >= '0' && p_Text[i] <= '9')
		{
			l_Value = l_Value * 10.0 + static_cast<double>(p_Text[i] - '0');
			++i;
			++l_Digits;
		}
		if (l_Digits == 0)
			return -1.0;
		if (i < p_Text.size() && (p_Text[i] == ',' || p_Text[i] == '.') && l_Count >= 1)
		{
			// The fraction of a second ends the time
			++i;
			double l_Scale = 0.1;
			double l_Fraction = 0.0;
			while (i < p_Text.size() && p_Text[i] >= '0' && p_Text[i] <= '9')
			{
				l_Fraction += static_cast<double>(p_Text[i] - '0') * l_Scale;
				l_Scale *= 0.1;
				++i;
			}
			l_Parts[l_Count++] = l_Value + l_Fraction;
			break;
		}
		l_Parts[l_Count++] = l_Value;
		if (i < p_Text.size() && p_Text[i] == ':')
			++i;
		else
			break;
	}
	if (l_Count < 2)
		return -1.0;
	// h:m:s, or m:s when there are two parts
	return l_Count == 3 ? l_Parts[0] * 3600.0 + l_Parts[1] * 60.0 + l_Parts[2] : l_Parts[0] * 60.0 + l_Parts[1];
}

void sortCues(std::vector<SubtitleCue>& p_Cues)
{
	std::ranges::stable_sort(p_Cues, [](const SubtitleCue& p_A, const SubtitleCue& p_B) { return p_A.start < p_B.start; });
}

std::vector<SubtitleCue> parseArrowFormat(const std::vector<std::string_view>& p_Lines)
{
	std::vector<SubtitleCue> l_Cues;
	for (size_t i = 0; i < p_Lines.size(); ++i)
	{
		const size_t l_Arrow = p_Lines[i].find("-->");
		if (l_Arrow == std::string_view::npos)
			continue;
		const double l_Start = parseTimestamp(p_Lines[i].substr(0, l_Arrow));
		std::string_view l_After = p_Lines[i].substr(l_Arrow + 3);
		while (!l_After.empty() && l_After.front() == ' ')
			l_After.remove_prefix(1);
		const size_t l_Space = l_After.find_first_of(" \t"); // WebVTT puts settings after the time
		const double l_End = parseTimestamp(l_After.substr(0, l_Space));
		if (l_Start < 0.0 || l_End < 0.0)
			continue;
		std::string l_Text;
		size_t k = i + 1;
		for (; k < p_Lines.size() && !trim(std::string(p_Lines[k])).empty(); ++k)
		{
			if (p_Lines[k].find("-->") != std::string_view::npos)
				break; // a missing blank line
			const std::string l_Line = trim(stripMarkup(p_Lines[k]));
			if (l_Line.empty())
				continue;
			if (!l_Text.empty())
				l_Text.push_back('\n');
			l_Text += l_Line;
		}
		if (!l_Text.empty())
			l_Cues.push_back(SubtitleCue{ .start = l_Start, .end = std::max(l_End, l_Start + 0.2), .text = std::move(l_Text) });
		i = k - 1;
	}
	return l_Cues;
}

std::vector<SubtitleCue> parseAss(const std::vector<std::string_view>& p_Lines)
{
	std::vector<SubtitleCue> l_Cues;
	for (const std::string_view l_Line : p_Lines)
	{
		if (!l_Line.starts_with("Dialogue:"))
			continue;
		// Dialogue: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text
		std::string_view l_Rest = l_Line.substr(9);
		std::string_view l_Fields[9];
		bool l_Ok = true;
		for (std::string_view& l_Field : l_Fields)
		{
			const size_t l_Comma = l_Rest.find(',');
			if (l_Comma == std::string_view::npos)
			{
				l_Ok = false;
				break;
			}
			l_Field = l_Rest.substr(0, l_Comma);
			l_Rest.remove_prefix(l_Comma + 1);
		}
		if (!l_Ok)
			continue;
		const double l_Start = parseTimestamp(l_Fields[1]);
		const double l_End = parseTimestamp(l_Fields[2]);
		if (l_Start < 0.0 || l_End < 0.0)
			continue;
		std::string l_Text;
		for (size_t i = 0; i < l_Rest.size();)
		{
			if (l_Rest[i] == '{')
			{
				const size_t l_Close = l_Rest.find('}', i);
				if (l_Close == std::string_view::npos)
					break;
				i = l_Close + 1;
			}
			else if (l_Rest[i] == '\\' && i + 1 < l_Rest.size() && (l_Rest[i + 1] == 'N' || l_Rest[i + 1] == 'n'))
			{
				l_Text.push_back('\n');
				i += 2;
			}
			else if (l_Rest[i] == '\\' && i + 1 < l_Rest.size() && l_Rest[i + 1] == 'h')
			{
				l_Text.push_back(' ');
				i += 2;
			}
			else
			{
				l_Text.push_back(l_Rest[i]);
				++i;
			}
		}
		l_Text = trim(std::move(l_Text));
		if (!l_Text.empty())
			l_Cues.push_back(SubtitleCue{ .start = l_Start, .end = std::max(l_End, l_Start + 0.2), .text = std::move(l_Text) });
	}
	return l_Cues;
}
} // namespace

std::string subtitleTextToUtf8(std::string_view p_Data)
{
	if (p_Data.starts_with("\xEF\xBB\xBF"))
		p_Data.remove_prefix(3);
	if (validUtf8(p_Data))
		return std::string(p_Data);
	std::string l_Out;
	l_Out.reserve(p_Data.size() + p_Data.size() / 8);
	for (const char l_Char : p_Data)
		appendCodePoint(l_Out, windows1252(static_cast<uint8_t>(l_Char)));
	return l_Out;
}

std::vector<SubtitleCue> parseSubtitles(const std::string_view p_Data)
{
	const std::string l_Text = subtitleTextToUtf8(p_Data);
	const std::vector<std::string_view> l_Lines = splitLines(l_Text);
	std::vector<SubtitleCue> l_Cues;
	if (l_Text.find("-->") != std::string::npos)
		l_Cues = parseArrowFormat(l_Lines);
	else if (l_Text.find("Dialogue:") != std::string::npos)
		l_Cues = parseAss(l_Lines);
	sortCues(l_Cues);
	return l_Cues;
}
} // namespace wb
