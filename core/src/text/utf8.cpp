module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

module wb.text.utf8;

namespace wb::text
{
uint32_t decodeNext(const std::string_view p_Text, size_t& p_Index)
{
	const auto l_At = [&](const size_t p_Offset) -> uint32_t { return p_Index + p_Offset < p_Text.size() ? static_cast<uint8_t>(p_Text[p_Index + p_Offset]) : 0u; };
	const uint32_t l_First = l_At(0);
	if (l_First < 0x80)
	{
		++p_Index;
		return l_First;
	}
	size_t l_Length = 0;
	uint32_t l_Code = 0;
	uint32_t l_Min = 0;
	if (l_First >= 0xF0 && l_First < 0xF5)
	{
		l_Length = 4;
		l_Code = l_First & 0x07;
		l_Min = 0x10000;
	}
	else if (l_First >= 0xE0 && l_First < 0xF0)
	{
		l_Length = 3;
		l_Code = l_First & 0x0F;
		l_Min = 0x800;
	}
	else if (l_First >= 0xC2 && l_First < 0xE0)
	{
		l_Length = 2;
		l_Code = l_First & 0x1F;
		l_Min = 0x80;
	}
	else
	{
		++p_Index;
		return REPLACEMENT_CHAR;
	}
	if (p_Index + l_Length > p_Text.size())
	{
		++p_Index;
		return REPLACEMENT_CHAR;
	}
	for (size_t i = 1; i < l_Length; ++i)
	{
		const uint32_t l_Byte = l_At(i);
		if ((l_Byte & 0xC0) != 0x80)
		{
			++p_Index;
			return REPLACEMENT_CHAR;
		}
		l_Code = (l_Code << 6) | (l_Byte & 0x3F);
	}
	if (l_Code < l_Min || l_Code > 0x10FFFF || (l_Code >= 0xD800 && l_Code <= 0xDFFF))
	{
		++p_Index;
		return REPLACEMENT_CHAR;
	}
	p_Index += l_Length;
	return l_Code;
}

void appendCodepoint(std::string& p_Out, uint32_t p_Codepoint)
{
	if (p_Codepoint > 0x10FFFF || (p_Codepoint >= 0xD800 && p_Codepoint <= 0xDFFF))
		p_Codepoint = REPLACEMENT_CHAR;
	if (p_Codepoint < 0x80)
	{
		p_Out.push_back(static_cast<char>(p_Codepoint));
	}
	else if (p_Codepoint < 0x800)
	{
		p_Out.push_back(static_cast<char>(0xC0 | (p_Codepoint >> 6)));
		p_Out.push_back(static_cast<char>(0x80 | (p_Codepoint & 0x3F)));
	}
	else if (p_Codepoint < 0x10000)
	{
		p_Out.push_back(static_cast<char>(0xE0 | (p_Codepoint >> 12)));
		p_Out.push_back(static_cast<char>(0x80 | ((p_Codepoint >> 6) & 0x3F)));
		p_Out.push_back(static_cast<char>(0x80 | (p_Codepoint & 0x3F)));
	}
	else
	{
		p_Out.push_back(static_cast<char>(0xF0 | (p_Codepoint >> 18)));
		p_Out.push_back(static_cast<char>(0x80 | ((p_Codepoint >> 12) & 0x3F)));
		p_Out.push_back(static_cast<char>(0x80 | ((p_Codepoint >> 6) & 0x3F)));
		p_Out.push_back(static_cast<char>(0x80 | (p_Codepoint & 0x3F)));
	}
}

std::string sanitize(const std::string_view p_Text)
{
	std::string l_Out;
	l_Out.reserve(p_Text.size());
	size_t l_Index = 0;
	while (l_Index < p_Text.size())
	{
		const uint32_t l_Code = decodeNext(p_Text, l_Index);
		if (l_Code == '\r')
		{
			// CRLF -> LF; a lone CR also ends a line
			if (l_Index < p_Text.size() && p_Text[l_Index] == '\n')
				continue;
			l_Out.push_back('\n');
			continue;
		}
		if (l_Code < 0x20 && l_Code != '\n' && l_Code != '\t')
			continue;
		if (l_Code == 0x7F || (l_Code >= 0x80 && l_Code < 0xA0))
			continue;
		appendCodepoint(l_Out, l_Code);
	}
	return l_Out;
}

bool isCombining(const uint32_t p_Codepoint)
{
	return (p_Codepoint >= 0x0300 && p_Codepoint <= 0x036F) || (p_Codepoint >= 0x1AB0 && p_Codepoint <= 0x1AFF) || (p_Codepoint >= 0x1DC0 && p_Codepoint <= 0x1DFF) ||
	       (p_Codepoint >= 0x20D0 && p_Codepoint <= 0x20FF) || (p_Codepoint >= 0xFE00 && p_Codepoint <= 0xFE0F) || (p_Codepoint >= 0xFE20 && p_Codepoint <= 0xFE2F) ||
	       p_Codepoint == 0x200D || (p_Codepoint >= 0x1F3FB && p_Codepoint <= 0x1F3FF) || (p_Codepoint >= 0xE0100 && p_Codepoint <= 0xE01EF);
}

bool isSpace(const uint32_t p_Codepoint)
{
	return p_Codepoint == ' ' || p_Codepoint == '\t' || p_Codepoint == 0xA0 || p_Codepoint == 0x3000 || (p_Codepoint >= 0x2000 && p_Codepoint <= 0x200A);
}

bool isFormatting(const uint32_t p_Codepoint)
{
	return (p_Codepoint >= 0x200B && p_Codepoint <= 0x200F) || (p_Codepoint >= 0x2028 && p_Codepoint <= 0x202E) || (p_Codepoint >= 0x2060 && p_Codepoint <= 0x2064) || p_Codepoint == 0xFEFF || p_Codepoint == 0x00AD;
}

bool breaksAnywhere(const uint32_t p_Codepoint)
{
	return (p_Codepoint >= 0x2E80 && p_Codepoint <= 0x9FFF) || (p_Codepoint >= 0xAC00 && p_Codepoint <= 0xD7AF) || (p_Codepoint >= 0xF900 && p_Codepoint <= 0xFAFF) ||
	       (p_Codepoint >= 0xFF00 && p_Codepoint <= 0xFFEF) || (p_Codepoint >= 0x20000 && p_Codepoint <= 0x2FFFF);
}

namespace
{
// Start of the code point that ends at p_Index
size_t previousCodepoint(const std::string_view p_Text, size_t p_Index)
{
	if (p_Index == 0)
		return 0;
	--p_Index;
	while (p_Index > 0 && (static_cast<uint8_t>(p_Text[p_Index]) & 0xC0) == 0x80)
		--p_Index;
	return p_Index;
}

uint32_t codepointAt(const std::string_view p_Text, size_t p_Index)
{
	return decodeNext(p_Text, p_Index);
}

enum class CharClass : uint8_t
{
	Space,
	Word,
	Other,
};

CharClass classify(const uint32_t p_Codepoint)
{
	if (isSpace(p_Codepoint) || p_Codepoint == '\n')
		return CharClass::Space;
	if (p_Codepoint >= 0x80 || (p_Codepoint >= '0' && p_Codepoint <= '9') || (p_Codepoint >= 'a' && p_Codepoint <= 'z') || (p_Codepoint >= 'A' && p_Codepoint <= 'Z') || p_Codepoint == '_')
		return CharClass::Word;
	return CharClass::Other;
}
} // namespace

size_t previousCluster(const std::string_view p_Text, size_t p_Index)
{
	p_Index = std::min(p_Index, p_Text.size());
	if (p_Index == 0)
		return 0;
	p_Index = previousCodepoint(p_Text, p_Index);
	while (p_Index > 0 && isCombining(codepointAt(p_Text, p_Index)))
		p_Index = previousCodepoint(p_Text, p_Index);
	return p_Index;
}

size_t nextCluster(const std::string_view p_Text, size_t p_Index)
{
	if (p_Index >= p_Text.size())
		return p_Text.size();
	decodeNext(p_Text, p_Index);
	while (p_Index < p_Text.size())
	{
		size_t l_Probe = p_Index;
		if (!isCombining(decodeNext(p_Text, l_Probe)))
			break;
		p_Index = l_Probe;
	}
	return p_Index;
}

size_t previousWord(const std::string_view p_Text, size_t p_Index)
{
	p_Index = std::min(p_Index, p_Text.size());
	// Skip spaces (a line break stops the move), then the run of one class
	const size_t l_Start = p_Index;
	while (p_Index > 0 && p_Text[p_Index - 1] != '\n' && classify(codepointAt(p_Text, previousCluster(p_Text, p_Index))) == CharClass::Space)
		p_Index = previousCluster(p_Text, p_Index);
	if (p_Index > 0 && p_Text[p_Index - 1] == '\n')
		return p_Index == l_Start ? p_Index - 1 : p_Index;
	if (p_Index == 0)
		return 0;
	const CharClass l_Class = classify(codepointAt(p_Text, previousCluster(p_Text, p_Index)));
	while (p_Index > 0)
	{
		const size_t l_Prev = previousCluster(p_Text, p_Index);
		if (classify(codepointAt(p_Text, l_Prev)) != l_Class)
			break;
		p_Index = l_Prev;
	}
	return p_Index;
}

size_t nextWord(const std::string_view p_Text, size_t p_Index)
{
	const size_t l_Size = p_Text.size();
	p_Index = std::min(p_Index, l_Size);
	if (p_Index >= l_Size)
		return l_Size;
	if (p_Text[p_Index] == '\n')
		return p_Index + 1;
	const CharClass l_Class = classify(codepointAt(p_Text, p_Index));
	if (l_Class != CharClass::Space)
	{
		while (p_Index < l_Size && classify(codepointAt(p_Text, p_Index)) == l_Class)
			p_Index = nextCluster(p_Text, p_Index);
	}
	while (p_Index < l_Size && p_Text[p_Index] != '\n' && classify(codepointAt(p_Text, p_Index)) == CharClass::Space)
		p_Index = nextCluster(p_Text, p_Index);
	return p_Index;
}

void wordRange(const std::string_view p_Text, const size_t p_Index, size_t& p_Begin, size_t& p_End)
{
	const size_t l_Size = p_Text.size();
	if (l_Size == 0)
	{
		p_Begin = p_End = 0;
		return;
	}
	// A click after the last character selects what is before it
	size_t l_At = std::min(p_Index, l_Size);
	if (l_At == l_Size || p_Text[l_At] == '\n')
		l_At = previousCluster(p_Text, l_At);
	if (l_At >= l_Size || p_Text[l_At] == '\n')
	{
		p_Begin = p_End = std::min(p_Index, l_Size);
		return;
	}
	const CharClass l_Class = classify(codepointAt(p_Text, l_At));
	p_Begin = l_At;
	while (p_Begin > 0)
	{
		const size_t l_Prev = previousCluster(p_Text, p_Begin);
		if (p_Text[l_Prev] == '\n' || classify(codepointAt(p_Text, l_Prev)) != l_Class)
			break;
		p_Begin = l_Prev;
	}
	p_End = l_At;
	while (p_End < l_Size && p_Text[p_End] != '\n' && classify(codepointAt(p_Text, p_End)) == l_Class)
		p_End = nextCluster(p_Text, p_End);
}

size_t countCodepoints(const std::string_view p_Text)
{
	size_t l_Count = 0;
	size_t l_Index = 0;
	while (l_Index < p_Text.size())
	{
		decodeNext(p_Text, l_Index);
		++l_Count;
	}
	return l_Count;
}
} // namespace wb::text
