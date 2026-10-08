module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "ui/stb_truetype.hpp"

module wb.ui.font;

import wb.math;

namespace wb::ui
{
struct FontAtlas::Font
{
	std::vector<uint8_t> data;
	stbtt_fontinfo info{};
	int ascent = 0;
	int descent = 0;
	int lineGap = 0;
};

namespace
{
bool loadFont(const std::filesystem::path& p_Path, FontAtlas::Font& p_Font, std::string& p_Error)
{
	std::ifstream l_File(p_Path, std::ios::binary | std::ios::ate);
	if (!l_File)
	{
		p_Error = "Missing font file: " + p_Path.string();
		return false;
	}
	const std::streamsize l_Size = l_File.tellg();
	p_Font.data.resize(static_cast<size_t>(l_Size));
	l_File.seekg(0);
	l_File.read(reinterpret_cast<char*>(p_Font.data.data()), l_Size);
	if (stbtt_InitFont(&p_Font.info, p_Font.data.data(), stbtt_GetFontOffsetForIndex(p_Font.data.data(), 0)) == 0)
	{
		p_Error = "Unsupported font file: " + p_Path.string();
		return false;
	}
	stbtt_GetFontVMetrics(&p_Font.info, &p_Font.ascent, &p_Font.descent, &p_Font.lineGap);
	return true;
}

// Scale that maps one em to p_PixelSize pixels
float emScale(const stbtt_fontinfo& p_Info, const int p_PixelSize)
{
	return stbtt_ScaleForMappingEmToPixels(&p_Info, static_cast<float>(p_PixelSize));
}
} // namespace

uint32_t nextCodepoint(const std::string_view p_Text, size_t& p_Index)
{
	const auto l_At = [&](const size_t p_Offset) -> uint32_t { return p_Index + p_Offset < p_Text.size() ? static_cast<uint8_t>(p_Text[p_Index + p_Offset]) : 0u; };
	const uint32_t l_First = l_At(0);
	size_t l_Length = 1;
	uint32_t l_Code = l_First;
	if (l_First >= 0xF0 && l_First < 0xF8)
	{
		l_Length = 4;
		l_Code = l_First & 0x07;
	}
	else if (l_First >= 0xE0 && l_First < 0xF0)
	{
		l_Length = 3;
		l_Code = l_First & 0x0F;
	}
	else if (l_First >= 0xC0 && l_First < 0xE0)
	{
		l_Length = 2;
		l_Code = l_First & 0x1F;
	}
	else if (l_First >= 0x80)
	{
		++p_Index;
		return 0xFFFD;
	}
	if (p_Index + l_Length > p_Text.size())
	{
		p_Index = p_Text.size();
		return 0xFFFD;
	}
	for (size_t i = 1; i < l_Length; ++i)
	{
		const uint32_t l_Byte = l_At(i);
		if ((l_Byte & 0xC0) != 0x80)
		{
			p_Index += i;
			return 0xFFFD;
		}
		l_Code = (l_Code << 6) | (l_Byte & 0x3F);
	}
	p_Index += l_Length;
	return l_Code;
}

FontAtlas::FontAtlas() : m_Text(std::make_unique<Font>()), m_Icons(std::make_unique<Font>()), m_Pixels(static_cast<size_t>(ATLAS_SIZE) * ATLAS_SIZE, 0)
{
}

FontAtlas::~FontAtlas() = default;

bool FontAtlas::load(const std::filesystem::path& p_Directory, std::string& p_Error)
{
	if (!loadFont(p_Directory / "Inter.ttf", *m_Text, p_Error))
		return false;
	if (!loadFont(p_Directory / "lucide.ttf", *m_Icons, p_Error))
		return false;
	m_Glyphs.clear();
	return true;
}

uint64_t FontAtlas::key(const FontFace p_Face, const uint32_t p_Codepoint, const int p_PixelSize)
{
	return static_cast<uint64_t>(p_Face) | (static_cast<uint64_t>(p_PixelSize & 0xFFFF) << 8) | (static_cast<uint64_t>(p_Codepoint) << 24);
}

void FontAtlas::reset()
{
	std::ranges::fill(m_Pixels, uint8_t{ 0 });
	m_Glyphs.clear();
	m_ShelfX = 1;
	m_ShelfY = 1;
	m_ShelfHeight = 0;
	markAllDirty();
}

void FontAtlas::markAllDirty()
{
	m_DirtyFirst = 0;
	m_DirtyLast = ATLAS_SIZE;
}

bool FontAtlas::beginFrame()
{
	if (!m_Overflowed)
		return false;
	m_Overflowed = false;
	reset();
	return true;
}

bool FontAtlas::pack(const uint32_t p_Width, const uint32_t p_Height, uint32_t& p_X, uint32_t& p_Y)
{
	if (p_Width + 2 > ATLAS_SIZE)
		return false;
	if (m_ShelfX + p_Width + 1 > ATLAS_SIZE)
	{
		m_ShelfY += m_ShelfHeight + 1;
		m_ShelfX = 1;
		m_ShelfHeight = 0;
	}
	if (m_ShelfY + p_Height + 1 > ATLAS_SIZE)
		return false;
	p_X = m_ShelfX;
	p_Y = m_ShelfY;
	m_ShelfX += p_Width + 1;
	m_ShelfHeight = std::max(m_ShelfHeight, p_Height);
	return true;
}

const GlyphQuad* FontAtlas::glyph(const FontFace p_Face, const uint32_t p_Codepoint, const int p_PixelSize)
{
	const uint64_t l_Key = key(p_Face, p_Codepoint, p_PixelSize);
	if (const auto l_It = m_Glyphs.find(l_Key); l_It != m_Glyphs.end())
		return &l_It->second;
	if (m_Overflowed)
		return nullptr;

	const Font& l_Font = p_Face == FontFace::Text ? *m_Text : *m_Icons;
	const float l_Scale = emScale(l_Font.info, p_PixelSize);
	GlyphQuad l_Quad{};

	int l_X0 = 0, l_Y0 = 0, l_X1 = 0, l_Y1 = 0;
	stbtt_GetCodepointBitmapBox(&l_Font.info, static_cast<int>(p_Codepoint), l_Scale, l_Scale, &l_X0, &l_Y0, &l_X1, &l_Y1);
	const uint32_t l_Width = static_cast<uint32_t>(std::max(l_X1 - l_X0, 0));
	const uint32_t l_Height = static_cast<uint32_t>(std::max(l_Y1 - l_Y0, 0));
	if (l_Width > 0 && l_Height > 0)
	{
		uint32_t l_PackX = 0, l_PackY = 0;
		if (!pack(l_Width, l_Height, l_PackX, l_PackY))
		{
			m_Overflowed = true;
			return nullptr;
		}
		stbtt_MakeCodepointBitmap(&l_Font.info, m_Pixels.data() + static_cast<size_t>(l_PackY) * ATLAS_SIZE + l_PackX, static_cast<int>(l_Width), static_cast<int>(l_Height), static_cast<int>(ATLAS_SIZE), l_Scale, l_Scale, static_cast<int>(p_Codepoint));
		l_Quad.offset = Vec2{ static_cast<float>(l_X0), static_cast<float>(l_Y0) };
		l_Quad.size = Vec2{ static_cast<float>(l_Width), static_cast<float>(l_Height) };
		const float l_Inv = 1.f / static_cast<float>(ATLAS_SIZE);
		l_Quad.uv0 = Vec2{ static_cast<float>(l_PackX), static_cast<float>(l_PackY) } * l_Inv;
		l_Quad.uv1 = Vec2{ static_cast<float>(l_PackX + l_Width), static_cast<float>(l_PackY + l_Height) } * l_Inv;

		if (m_DirtyFirst >= m_DirtyLast)
		{
			m_DirtyFirst = l_PackY;
			m_DirtyLast = l_PackY + l_Height;
		}
		else
		{
			m_DirtyFirst = std::min(m_DirtyFirst, l_PackY);
			m_DirtyLast = std::max(m_DirtyLast, l_PackY + l_Height);
		}
	}
	return &m_Glyphs.emplace(l_Key, l_Quad).first->second;
}

float FontAtlas::advance(const FontFace p_Face, const uint32_t p_Codepoint, const int p_PixelSize) const
{
	const Font& l_Font = p_Face == FontFace::Text ? *m_Text : *m_Icons;
	int l_Advance = 0, l_Bearing = 0;
	stbtt_GetCodepointHMetrics(&l_Font.info, static_cast<int>(p_Codepoint), &l_Advance, &l_Bearing);
	return static_cast<float>(l_Advance) * emScale(l_Font.info, p_PixelSize);
}

float FontAtlas::measure(const std::string_view p_Text, const int p_PixelSize) const
{
	float l_Width = 0.f;
	size_t l_Index = 0;
	while (l_Index < p_Text.size())
		l_Width += advance(FontFace::Text, nextCodepoint(p_Text, l_Index), p_PixelSize);
	return l_Width;
}

float FontAtlas::ascent(const int p_PixelSize) const
{
	return static_cast<float>(m_Text->ascent) * emScale(m_Text->info, p_PixelSize);
}

float FontAtlas::lineHeight(const int p_PixelSize) const
{
	return static_cast<float>(m_Text->ascent - m_Text->descent + m_Text->lineGap) * emScale(m_Text->info, p_PixelSize);
}

float FontAtlas::capHeight(const int p_PixelSize) const
{
	// Inter's cap height is 0.727 em
	return 0.727f * static_cast<float>(p_PixelSize);
}

std::string FontAtlas::fit(const std::string_view p_Text, const int p_PixelSize, const float p_MaxWidth) const
{
	if (measure(p_Text, p_PixelSize) <= p_MaxWidth)
		return std::string(p_Text);
	static constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";
	const float l_Budget = p_MaxWidth - measure(ELLIPSIS, p_PixelSize);
	std::string l_Result;
	float l_Width = 0.f;
	size_t l_Index = 0;
	while (l_Index < p_Text.size())
	{
		const size_t l_Start = l_Index;
		const uint32_t l_Code = nextCodepoint(p_Text, l_Index);
		l_Width += advance(FontFace::Text, l_Code, p_PixelSize);
		if (l_Width > l_Budget)
			break;
		l_Result.append(p_Text.substr(l_Start, l_Index - l_Start));
	}
	return l_Result + std::string(ELLIPSIS);
}

std::vector<std::string> FontAtlas::wrap(const std::string_view p_Text, const int p_PixelSize, const float p_MaxWidth) const
{
	std::vector<std::string> l_Lines;
	size_t l_Pos = 0;
	while (l_Pos <= p_Text.size())
	{
		size_t l_End = p_Text.find('\n', l_Pos);
		if (l_End == std::string_view::npos)
			l_End = p_Text.size();
		const std::string_view l_Paragraph = p_Text.substr(l_Pos, l_End - l_Pos);

		std::string l_Line;
		size_t l_WordPos = 0;
		while (l_WordPos < l_Paragraph.size())
		{
			size_t l_WordEnd = l_Paragraph.find(' ', l_WordPos);
			if (l_WordEnd == std::string_view::npos)
				l_WordEnd = l_Paragraph.size();
			const std::string_view l_Word = l_Paragraph.substr(l_WordPos, l_WordEnd - l_WordPos);
			const std::string l_Candidate = l_Line.empty() ? std::string(l_Word) : l_Line + " " + std::string(l_Word);
			if (l_Line.empty() || measure(l_Candidate, p_PixelSize) <= p_MaxWidth)
			{
				l_Line = l_Candidate;
			}
			else
			{
				l_Lines.push_back(std::move(l_Line));
				l_Line = std::string(l_Word);
			}
			l_WordPos = l_WordEnd + 1;
		}
		l_Lines.push_back(std::move(l_Line));
		l_Pos = l_End + 1;
	}
	return l_Lines;
}

bool FontAtlas::takeDirtyRows(uint32_t& p_FirstRow, uint32_t& p_RowCount)
{
	if (m_DirtyFirst >= m_DirtyLast)
		return false;
	p_FirstRow = m_DirtyFirst;
	p_RowCount = std::min(m_DirtyLast, ATLAS_SIZE) - m_DirtyFirst;
	m_DirtyFirst = m_DirtyLast = 0;
	return true;
}
} // namespace wb::ui
