module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <hb.h>
#include <SheenBidi/SheenBidi.h>
#include "ui/stb_truetype.hpp"

module wb.ui.font;

import wb.math;

namespace wb::ui
{
struct FontAtlas::Font
{
	std::vector<uint8_t> data;
	stbtt_fontinfo info{};
	size_t size = 0; // bytes at info.data
	int ascent = 0;
	int descent = 0;
	int lineGap = 0;
	hb_font_t* shaper = nullptr; // made when first needed

	Font() = default;
	Font(const Font&) = delete;
	Font& operator=(const Font&) = delete;
	~Font()
	{
		if (shaper != nullptr)
			hb_font_destroy(shaper);
	}
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
	p_Font.size = p_Font.data.size();
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

// Format and direction marks, joiners and variation selectors: they draw nothing and need no font of their own
bool isInvisible(const uint32_t p_Code)
{
	return (p_Code >= 0x200B && p_Code <= 0x200F) || (p_Code >= 0x202A && p_Code <= 0x202E) || (p_Code >= 0x2060 && p_Code <= 0x206F) || (p_Code >= 0xFE00 && p_Code <= 0xFE0F) || p_Code == 0xFEFF || p_Code == 0x061C;
}

// Characters of scripts the plain one-glyph-per-character path cannot draw
bool isComplex(const uint32_t p_Code)
{
	return (p_Code >= 0x0590 && p_Code <= 0x109F) || (p_Code >= 0x1780 && p_Code <= 0x17FF) || (p_Code >= 0x200E && p_Code <= 0x200F) || (p_Code >= 0x202A && p_Code <= 0x202E) || (p_Code >= 0x2066 && p_Code <= 0x2069)
		|| (p_Code >= 0xFB1D && p_Code <= 0xFDFF) || (p_Code >= 0xFE70 && p_Code <= 0xFEFF);
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
	return static_cast<uint64_t>(p_Face) | (static_cast<uint64_t>(p_PixelSize & 0xFFFF) << 16) | (static_cast<uint64_t>(p_Codepoint) << 32);
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

const GlyphQuad* FontAtlas::glyph(const FontFace p_Requested, const uint32_t p_Codepoint, const int p_PixelSize)
{
	const FontFace l_Face = resolve(p_Requested, p_Codepoint);
	const uint64_t l_Key = key(l_Face, p_Codepoint, p_PixelSize);
	if (const auto l_It = m_Glyphs.find(l_Key); l_It != m_Glyphs.end())
		return &l_It->second;
	const Font& l_Font = fontFor(l_Face);
	return place(l_Key, l_Font, stbtt_FindGlyphIndex(&l_Font.info, static_cast<int>(p_Codepoint)), p_PixelSize);
}

const GlyphQuad* FontAtlas::glyphByIndex(const FontFace p_Face, const uint32_t p_Glyph, const int p_PixelSize)
{
	// Glyph indices share the table with code points: bit 63 tells them apart (code points need 21 bits)
	const uint64_t l_Key = key(p_Face, p_Glyph, p_PixelSize) | (1ull << 63);
	if (const auto l_It = m_Glyphs.find(l_Key); l_It != m_Glyphs.end())
		return &l_It->second;
	return place(l_Key, fontFor(p_Face), static_cast<int>(p_Glyph), p_PixelSize);
}

// Rasterizes a glyph into the atlas (or finds the atlas full: nullptr, and the atlas is reset on the next frame)
const GlyphQuad* FontAtlas::place(const uint64_t p_Key, const Font& p_Font, const int p_Glyph, const int p_PixelSize)
{
	if (m_Overflowed)
		return nullptr;
	const float l_Scale = emScale(p_Font.info, p_PixelSize);
	GlyphQuad l_Quad{};

	int l_X0 = 0, l_Y0 = 0, l_X1 = 0, l_Y1 = 0;
	stbtt_GetGlyphBitmapBox(&p_Font.info, p_Glyph, l_Scale, l_Scale, &l_X0, &l_Y0, &l_X1, &l_Y1);
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
		stbtt_MakeGlyphBitmap(&p_Font.info, m_Pixels.data() + static_cast<size_t>(l_PackY) * ATLAS_SIZE + l_PackX, static_cast<int>(l_Width), static_cast<int>(l_Height), static_cast<int>(ATLAS_SIZE), l_Scale, l_Scale, p_Glyph);
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
	return &m_Glyphs.emplace(p_Key, l_Quad).first->second;
}

float FontAtlas::advance(const FontFace p_Requested, const uint32_t p_Codepoint, const int p_PixelSize) const
{
	const Font& l_Font = fontFor(resolve(p_Requested, p_Codepoint));
	int l_Advance = 0, l_Bearing = 0;
	stbtt_GetCodepointHMetrics(&l_Font.info, static_cast<int>(p_Codepoint), &l_Advance, &l_Bearing);
	return static_cast<float>(l_Advance) * emScale(l_Font.info, p_PixelSize);
}

float FontAtlas::measure(const std::string_view p_Text, const int p_PixelSize, const FontFace p_Face) const
{
	if (p_Face == FontFace::Text && needsShaping(p_Text))
		return shape(p_Text, p_PixelSize).width;
	float l_Width = 0.f;
	size_t l_Index = 0;
	while (l_Index < p_Text.size())
		l_Width += advance(p_Face, nextCodepoint(p_Text, l_Index), p_PixelSize);
	return l_Width;
}

const FontAtlas::Font& FontAtlas::fontFor(const FontFace p_Face) const
{
	const size_t l_Index = static_cast<size_t>(p_Face);
	if (l_Index >= 2 && l_Index - 2 < m_Extra.size())
		return *m_Extra[l_Index - 2];
	return p_Face == FontFace::Icons ? *m_Icons : *m_Text;
}

FontFace FontAtlas::addBorrowed(const uint8_t* p_Data, const int p_Offset, const size_t p_Size) const
{
	auto l_Font = std::make_unique<Font>();
	if (p_Offset < 0 || stbtt_InitFont(&l_Font->info, p_Data, p_Offset) == 0)
		return FontFace::Text;
	l_Font->size = p_Size;
	stbtt_GetFontVMetrics(&l_Font->info, &l_Font->ascent, &l_Font->descent, &l_Font->lineGap);
	m_Extra.push_back(std::move(l_Font));
	return static_cast<FontFace>(m_Extra.size() + 1);
}

FontFace FontAtlas::addFace(const uint8_t* p_Data, const size_t p_Size)
{
	return addBorrowed(p_Data, stbtt_GetFontOffsetForIndex(p_Data, 0), p_Size);
}

bool FontAtlas::setLanguageFont(const std::filesystem::path& p_Path)
{
	FontFace l_Face = FontFace::Text;
	if (!p_Path.empty())
	{
		const std::string l_Key = p_Path.string();
		if (const auto l_It = m_LanguageFaces.find(l_Key); l_It != m_LanguageFaces.end())
		{
			l_Face = l_It->second;
		}
		else
		{
			auto l_Font = std::make_unique<Font>();
			std::string l_Error;
			if (!loadFont(p_Path, *l_Font, l_Error))
				return false;
			m_Extra.push_back(std::move(l_Font));
			l_Face = static_cast<FontFace>(m_Extra.size() + 1);
			m_LanguageFaces.emplace(l_Key, l_Face);
		}
	}
	m_LanguageFace = l_Face;
	m_Resolved.clear();
	return true;
}

void FontAtlas::setFallbackFinder(FallbackFinder p_Finder)
{
	m_Finder = std::move(p_Finder);
	m_Resolved.clear();
}

void FontAtlas::forgetFallbackMisses()
{
	for (auto l_It = m_Resolved.begin(); l_It != m_Resolved.end();)
		l_It = l_It->second == FontFace::Text ? m_Resolved.erase(l_It) : std::next(l_It);
}

FontFace FontAtlas::resolve(const FontFace p_Face, const uint32_t p_Codepoint) const
{
	if (p_Face != FontFace::Text || p_Codepoint <= 0x20 || isInvisible(p_Codepoint) || stbtt_FindGlyphIndex(&m_Text->info, static_cast<int>(p_Codepoint)) != 0)
		return p_Face;
	if (const auto l_It = m_Resolved.find(p_Codepoint); l_It != m_Resolved.end())
		return l_It->second;

	FontFace l_Found = FontFace::Text;
	if (m_LanguageFace != FontFace::Text && stbtt_FindGlyphIndex(&fontFor(m_LanguageFace).info, static_cast<int>(p_Codepoint)) != 0)
	{
		l_Found = m_LanguageFace;
	}
	else if (m_Finder)
	{
		if (const FallbackFace l_Fallback = m_Finder(p_Codepoint); l_Fallback.data != nullptr)
		{
			if (const auto l_Known = m_FallbackFaces.find(l_Fallback.id); l_Known != m_FallbackFaces.end())
			{
				l_Found = l_Known->second;
			}
			else
			{
				l_Found = addBorrowed(l_Fallback.data, l_Fallback.offset, l_Fallback.size);
				m_FallbackFaces.emplace(l_Fallback.id, l_Found);
			}
		}
	}
	m_Resolved.emplace(p_Codepoint, l_Found);
	return l_Found;
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
	if (needsShaping(p_Text))
	{
		// Joined letters change width with what follows them: try each prefix as a whole
		size_t l_Index = 0;
		size_t l_Keep = 0;
		while (l_Index < p_Text.size())
		{
			(void)nextCodepoint(p_Text, l_Index);
			if (measure(std::string(p_Text.substr(0, l_Index)) + std::string(ELLIPSIS), p_PixelSize) > p_MaxWidth)
				break;
			l_Keep = l_Index;
		}
		return std::string(p_Text.substr(0, l_Keep)) + std::string(ELLIPSIS);
	}
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

namespace
{
// Characters of scripts written without spaces: a line may break between any two of them
bool breaksAnywhere(const uint32_t p_Code)
{
	return (p_Code >= 0x2E80 && p_Code <= 0x30FF && p_Code != 0x3000) || (p_Code >= 0x3100 && p_Code <= 0x9FFF) || (p_Code >= 0xAC00 && p_Code <= 0xD7AF) || (p_Code >= 0xF900 && p_Code <= 0xFAFF) || (p_Code >= 0xFF21 && p_Code <= 0xFF5A) || (p_Code >= 0x20000 && p_Code <= 0x3FFFF);
}

// Punctuation that must not start a line: it stays with the character before it
bool staysWithPrevious(const uint32_t p_Code)
{
	switch (p_Code)
	{
	case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F: // 、。，．：；！？
	case 0x3009: case 0x300B: case 0x300D: case 0x300F: case 0x3011: case 0xFF09: case 0xFF3D: case 0xFF5D: // closing brackets
	case 0x30FC: case 0x2026: case 0x30FB: // long vowel mark, ellipsis, middle dot
		return true;
	default:
		return false;
	}
}

struct WrapToken
{
	std::string text;
	size_t spacesBefore = 0;
};

// Words, split at spaces; each character of a script without spaces is a word of its own
std::vector<WrapToken> wrapTokens(const std::string_view p_Paragraph)
{
	std::vector<WrapToken> l_Tokens;
	size_t l_Spaces = 0;
	bool l_InWord = false;
	size_t l_Index = 0;
	while (l_Index < p_Paragraph.size())
	{
		const size_t l_Start = l_Index;
		const uint32_t l_Code = nextCodepoint(p_Paragraph, l_Index);
		const std::string_view l_Char = p_Paragraph.substr(l_Start, l_Index - l_Start);
		if (l_Code == ' ')
		{
			++l_Spaces;
			l_InWord = false;
			continue;
		}
		if (breaksAnywhere(l_Code))
		{
			l_Tokens.push_back(WrapToken{ std::string(l_Char), l_Spaces });
			l_InWord = false;
		}
		else if (l_InWord || (staysWithPrevious(l_Code) && l_Spaces == 0 && !l_Tokens.empty()))
		{
			l_Tokens.back().text.append(l_Char);
			l_InWord = true;
		}
		else
		{
			l_Tokens.push_back(WrapToken{ std::string(l_Char), l_Spaces });
			l_InWord = true;
		}
		l_Spaces = 0;
	}
	return l_Tokens;
}
} // namespace

std::vector<std::string> FontAtlas::wrap(const std::string_view p_Text, const int p_PixelSize, const float p_MaxWidth) const
{
	std::vector<std::string> l_Lines;
	size_t l_Pos = 0;
	while (l_Pos <= p_Text.size())
	{
		size_t l_End = p_Text.find('\n', l_Pos);
		if (l_End == std::string_view::npos)
			l_End = p_Text.size();

		std::string l_Line;
		for (const WrapToken& l_Token : wrapTokens(p_Text.substr(l_Pos, l_End - l_Pos)))
		{
			const std::string l_Candidate = l_Line.empty() ? l_Token.text : l_Line + std::string(l_Token.spacesBefore, ' ') + l_Token.text;
			if (l_Line.empty() || measure(l_Candidate, p_PixelSize) <= p_MaxWidth)
			{
				l_Line = l_Candidate;
				continue;
			}
			l_Lines.push_back(std::move(l_Line));
			l_Line = l_Token.text;
		}
		// A word wider than the line (an address, a path) is cut where it no longer fits
		for (size_t l_Fit = 0; l_Fit < 4096 && measure(l_Line, p_PixelSize) > p_MaxWidth; ++l_Fit)
		{
			size_t l_Index = 0;
			size_t l_Cut = 0;
			while (l_Index < l_Line.size())
			{
				(void)nextCodepoint(l_Line, l_Index);
				if (measure(std::string_view(l_Line).substr(0, l_Index), p_PixelSize) > p_MaxWidth)
					break;
				l_Cut = l_Index;
			}
			if (l_Cut == 0)
			{
				// Not even one character fits: take one anyway, so the loop ends
				size_t l_First = 0;
				(void)nextCodepoint(l_Line, l_First);
				l_Cut = l_First;
			}
			if (l_Cut >= l_Line.size())
				break;
			l_Lines.push_back(l_Line.substr(0, l_Cut));
			l_Line.erase(0, l_Cut);
		}
		l_Lines.push_back(std::move(l_Line));
		l_Pos = l_End + 1;
	}
	return l_Lines;
}

// ------------------------------------------------------------------------------------------------ shaping

bool FontAtlas::needsShaping(const std::string_view p_Text)
{
	size_t l_Index = 0;
	while (l_Index < p_Text.size())
	{
		if (static_cast<uint8_t>(p_Text[l_Index]) < 0xC0) // ASCII and continuation bytes: nothing to shape
		{
			++l_Index;
			continue;
		}
		if (isComplex(nextCodepoint(p_Text, l_Index)))
			return true;
	}
	return false;
}

void FontAtlas::setRightToLeft(const bool p_Rtl)
{
	m_BaseRtl = p_Rtl;
	m_Shaped.clear();
}

namespace
{
// HarfBuzz's font for one of the atlas's faces. The face data stays with the atlas, so the blob only points at it.
hb_font_t* shaperFor(FontAtlas::Font& p_Font)
{
	if (p_Font.shaper != nullptr)
		return p_Font.shaper;
	// A font collection holds several fonts: which one starts where stb_truetype was told to read?
	unsigned int l_Index = 0;
	for (int l_Offset = stbtt_GetFontOffsetForIndex(p_Font.info.data, 0); l_Offset >= 0 && l_Offset != p_Font.info.fontstart; l_Offset = stbtt_GetFontOffsetForIndex(p_Font.info.data, static_cast<int>(++l_Index)))
	{
	}
	hb_blob_t* l_Blob = hb_blob_create(reinterpret_cast<const char*>(p_Font.info.data), static_cast<unsigned int>(p_Font.size), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
	hb_face_t* l_Face = hb_face_create(l_Blob, l_Index);
	hb_blob_destroy(l_Blob);
	p_Font.shaper = hb_font_create(l_Face);
	hb_face_destroy(l_Face);
	return p_Font.shaper;
}

// The script a character belongs to for shaping; punctuation and combining marks take that of their neighbours
hb_script_t scriptOf(const uint32_t p_Code)
{
	return hb_unicode_script(hb_unicode_funcs_get_default(), p_Code);
}

bool neutralScript(const hb_script_t p_Script)
{
	return p_Script == HB_SCRIPT_COMMON || p_Script == HB_SCRIPT_INHERITED || p_Script == HB_SCRIPT_UNKNOWN;
}
} // namespace

ShapedText FontAtlas::shapeUncached(const std::string_view p_Text, const int p_PixelSize) const
{
	ShapedText l_Result;
	if (p_Text.empty())
		return l_Result;

	const SBCodepointSequence l_Sequence{ SBStringEncodingUTF8, p_Text.data(), static_cast<SBUInteger>(p_Text.size()) };
	SBAlgorithmRef l_Algorithm = SBAlgorithmCreate(&l_Sequence);
	if (l_Algorithm == nullptr)
		return l_Result;

	hb_buffer_t* l_Buffer = hb_buffer_create();
	const SBLevel l_Base = m_BaseRtl ? SBLevelDefaultRTL : SBLevelDefaultLTR;
	bool l_FirstParagraph = true;
	for (SBUInteger l_Offset = 0; l_Offset < p_Text.size();)
	{
		SBUInteger l_Length = 0;
		SBUInteger l_Separator = 0;
		SBAlgorithmGetParagraphBoundary(l_Algorithm, l_Offset, static_cast<SBUInteger>(p_Text.size()) - l_Offset, &l_Length, &l_Separator);
		if (l_Length == 0)
			break;
		SBParagraphRef l_Paragraph = SBAlgorithmCreateParagraph(l_Algorithm, l_Offset, l_Length, l_Base);
		if (l_Paragraph == nullptr)
			break;
		if (l_FirstParagraph)
			l_Result.rtl = (SBParagraphGetBaseLevel(l_Paragraph) & 1) != 0;
		l_FirstParagraph = false;
		SBLineRef l_Line = SBParagraphCreateLine(l_Paragraph, l_Offset, l_Length - l_Separator);
		if (l_Line != nullptr)
		{
			const SBRun* l_Runs = SBLineGetRunsPtr(l_Line);
			for (SBUInteger r = 0; r < SBLineGetRunCount(l_Line); ++r)
			{
				const bool l_Rtl = (l_Runs[r].level & 1) != 0;
				const size_t l_RunEnd = l_Runs[r].offset + l_Runs[r].length;

				// Split the run where the font or the script changes; each piece is shaped on its own
				struct Piece
				{
					size_t begin;
					size_t end;
					FontFace face;
					hb_script_t script;
				};
				std::vector<Piece> l_Pieces;
				hb_script_t l_Script = HB_SCRIPT_COMMON;
				for (size_t l_Index = l_Runs[r].offset; l_Index < l_RunEnd;)
				{
					const size_t l_Start = l_Index;
					const uint32_t l_Code = nextCodepoint(p_Text, l_Index);
					const FontFace l_Face = resolve(FontFace::Text, l_Code);
					hb_script_t l_Own = scriptOf(l_Code);
					if (neutralScript(l_Own))
						l_Own = l_Pieces.empty() ? l_Script : l_Pieces.back().script; // stays with what came before
					if (!l_Pieces.empty() && l_Pieces.back().face == l_Face && (l_Pieces.back().script == l_Own || neutralScript(l_Own)))
					{
						l_Pieces.back().end = l_Index;
						continue;
					}
					if (neutralScript(l_Own))
						l_Own = HB_SCRIPT_COMMON;
					l_Pieces.push_back(Piece{ l_Start, l_Index, l_Face, l_Own });
					l_Script = l_Own;
				}
				// Pieces were made in reading order; a right-to-left run is drawn from its right end
				if (l_Rtl)
					std::ranges::reverse(l_Pieces);

				for (const Piece& l_Piece : l_Pieces)
				{
					Font& l_Font = const_cast<Font&>(fontFor(l_Piece.face));
					hb_font_t* l_Shaper = shaperFor(l_Font);
					hb_buffer_clear_contents(l_Buffer);
					hb_buffer_add_utf8(l_Buffer, p_Text.data(), static_cast<int>(p_Text.size()), static_cast<unsigned int>(l_Piece.begin), static_cast<int>(l_Piece.end - l_Piece.begin));
					hb_buffer_set_direction(l_Buffer, l_Rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
					hb_buffer_set_script(l_Buffer, neutralScript(l_Piece.script) ? HB_SCRIPT_LATIN : l_Piece.script);
					hb_buffer_set_language(l_Buffer, hb_language_get_default());
					hb_shape(l_Shaper, l_Buffer, nullptr, 0);

					unsigned int l_Count = 0;
					const hb_glyph_info_t* l_Infos = hb_buffer_get_glyph_infos(l_Buffer, &l_Count);
					const hb_glyph_position_t* l_Positions = hb_buffer_get_glyph_positions(l_Buffer, &l_Count);
					const float l_Scale = emScale(l_Font.info, p_PixelSize);
					const float l_Units = static_cast<float>(hb_face_get_upem(hb_font_get_face(l_Shaper)));
					const float l_ToPixels = l_Units > 0.f ? static_cast<float>(p_PixelSize) / l_Units : l_Scale;
					for (unsigned int g = 0; g < l_Count; ++g)
					{
						ShapedGlyph l_Glyph;
						l_Glyph.face = l_Piece.face;
						l_Glyph.glyph = l_Infos[g].codepoint;
						l_Glyph.advance = static_cast<float>(l_Positions[g].x_advance) * l_ToPixels;
						l_Glyph.xOffset = static_cast<float>(l_Positions[g].x_offset) * l_ToPixels;
						l_Glyph.yOffset = static_cast<float>(l_Positions[g].y_offset) * l_ToPixels;
						l_Glyph.cluster = l_Infos[g].cluster;
						l_Glyph.rtl = l_Rtl;
						l_Result.width += l_Glyph.advance;
						l_Result.glyphs.push_back(l_Glyph);
					}
				}
			}
			SBLineRelease(l_Line);
		}
		SBParagraphRelease(l_Paragraph);
		l_Offset += l_Length;
	}
	hb_buffer_destroy(l_Buffer);
	SBAlgorithmRelease(l_Algorithm);
	return l_Result;
}

const ShapedText& FontAtlas::shape(const std::string_view p_Text, const int p_PixelSize) const
{
	std::string l_Key = std::to_string(p_PixelSize);
	l_Key += '|';
	l_Key += p_Text;
	if (const auto l_It = m_Shaped.find(l_Key); l_It != m_Shaped.end())
		return l_It->second;
	if (m_Shaped.size() > 2048) // text that changes every frame must not pile up
		m_Shaped.clear();
	return m_Shaped.emplace(std::move(l_Key), shapeUncached(p_Text, p_PixelSize)).first->second;
}

float FontAtlas::caretX(const std::string_view p_Text, const size_t p_Index, const int p_PixelSize) const
{
	if (!needsShaping(p_Text))
		return measure(p_Text.substr(0, p_Index), p_PixelSize);
	const ShapedText& l_Shaped = shape(p_Text, p_PixelSize);
	if (l_Shaped.glyphs.empty())
		return 0.f;

	// Left and right edge of every glyph in drawing order
	std::vector<float> l_Left(l_Shaped.glyphs.size());
	float l_Pen = 0.f;
	for (size_t g = 0; g < l_Shaped.glyphs.size(); ++g)
	{
		l_Left[g] = l_Pen;
		l_Pen += l_Shaped.glyphs[g].advance;
	}

	// The glyph for the character at p_Index: the one with the greatest cluster not past it. A glyph's characters
	// start at its right edge when it runs right to left.
	const bool l_End = p_Index >= p_Text.size();
	int l_Best = -1;
	for (size_t g = 0; g < l_Shaped.glyphs.size(); ++g)
	{
		const uint32_t l_Cluster = l_Shaped.glyphs[g].cluster;
		if (l_Cluster > p_Index && !l_End)
			continue;
		if (l_Best < 0 || l_Cluster > l_Shaped.glyphs[static_cast<size_t>(l_Best)].cluster)
			l_Best = static_cast<int>(g);
	}
	if (l_Best < 0)
		return l_Shaped.rtl ? l_Shaped.width : 0.f;
	const size_t l_Glyph = static_cast<size_t>(l_Best);
	const ShapedGlyph& l_Found = l_Shaped.glyphs[l_Glyph];
	// Of the glyphs sharing that cluster, the caret goes to the outermost edge in reading direction
	float l_X = l_Found.rtl ? l_Left[l_Glyph] + l_Found.advance : l_Left[l_Glyph];
	for (size_t g = 0; g < l_Shaped.glyphs.size(); ++g)
	{
		if (l_Shaped.glyphs[g].cluster != l_Found.cluster)
			continue;
		l_X = l_Found.rtl ? std::max(l_X, l_Left[g] + l_Shaped.glyphs[g].advance) : std::min(l_X, l_Left[g]);
	}
	if (l_End)
	{
		// After the last character: the far edge of the last glyph in reading order
		l_X = l_Found.rtl ? l_Left[l_Glyph] : l_Left[l_Glyph] + l_Found.advance;
		for (size_t g = 0; g < l_Shaped.glyphs.size(); ++g)
		{
			if (l_Shaped.glyphs[g].cluster != l_Found.cluster)
				continue;
			l_X = l_Found.rtl ? std::min(l_X, l_Left[g]) : std::max(l_X, l_Left[g] + l_Shaped.glyphs[g].advance);
		}
	}
	return l_X;
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
