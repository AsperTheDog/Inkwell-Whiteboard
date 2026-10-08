module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

module wb.text.system;

import wb.math;
import wb.doc.object;
import wb.text.layout;
import wb.text.fonts;
import wb.text.utf8;

namespace wb::text
{
namespace
{
constexpr size_t MAX_CACHED_LAYOUTS = 3000;
constexpr uint64_t CACHE_KEEP_FRAMES = 900;

uint64_t mix(uint64_t p_Hash, const uint64_t p_Value)
{
	p_Hash ^= p_Value + 0x9E3779B97F4A7C15ull + (p_Hash << 6) + (p_Hash >> 2);
	return p_Hash;
}

uint64_t glyphKey(const FaceId p_Face, const uint32_t p_Glyph)
{
	return (static_cast<uint64_t>(p_Face) << 32) | p_Glyph;
}
} // namespace

// ------------------------------------------------------------------------------------------------ atlas

GlyphAtlas::GlyphAtlas() : m_Pixels(static_cast<size_t>(SIZE) * SIZE, 0)
{
}

void GlyphAtlas::reset()
{
	m_Entries.clear();
	std::fill(m_Pixels.begin(), m_Pixels.end(), static_cast<uint8_t>(0));
	m_ShelfX = 1;
	m_ShelfY = 1;
	m_ShelfHeight = 0;
	m_Overflowed = false;
	markAllDirty();
}

bool GlyphAtlas::beginFrame()
{
	if (!m_Overflowed)
		return false;
	reset();
	return true;
}

void GlyphAtlas::markAllDirty()
{
	m_DirtyFirst = 0;
	m_DirtyLast = SIZE;
}

bool GlyphAtlas::takeDirtyRows(uint32_t& p_FirstRow, uint32_t& p_RowCount)
{
	if (m_DirtyFirst >= m_DirtyLast)
		return false;
	p_FirstRow = m_DirtyFirst;
	p_RowCount = m_DirtyLast - m_DirtyFirst;
	m_DirtyFirst = SIZE;
	m_DirtyLast = 0;
	return true;
}

const GlyphAtlas::Entry* GlyphAtlas::get(const FontRegistry& p_Fonts, const FaceId p_Face, const uint32_t p_Glyph)
{
	const uint64_t l_Key = glyphKey(p_Face, p_Glyph);
	if (const auto l_It = m_Entries.find(l_Key); l_It != m_Entries.end())
		return &l_It->second;
	if (m_Overflowed)
		return nullptr;

	SdfBitmap l_Bitmap;
	Entry l_Entry;
	if (!p_Fonts.rasterize(p_Face, p_Glyph, l_Bitmap) || l_Bitmap.width <= 0 || l_Bitmap.height <= 0)
	{
		l_Entry.blank = true;
		return &(m_Entries[l_Key] = l_Entry);
	}

	const uint32_t l_Width = static_cast<uint32_t>(l_Bitmap.width);
	const uint32_t l_Height = static_cast<uint32_t>(l_Bitmap.height);
	if (l_Width + 2 > SIZE || l_Height + 2 > SIZE)
	{
		l_Entry.blank = true;
		return &(m_Entries[l_Key] = l_Entry);
	}
	if (m_ShelfX + l_Width + 1 > SIZE)
	{
		m_ShelfX = 1;
		m_ShelfY += m_ShelfHeight + 1;
		m_ShelfHeight = 0;
	}
	if (m_ShelfY + l_Height + 1 > SIZE)
	{
		m_Overflowed = true;
		return nullptr;
	}
	const uint32_t l_X = m_ShelfX;
	const uint32_t l_Y = m_ShelfY;
	m_ShelfX += l_Width + 1;
	m_ShelfHeight = std::max(m_ShelfHeight, l_Height);

	for (uint32_t l_Row = 0; l_Row < l_Height; ++l_Row)
		std::memcpy(&m_Pixels[static_cast<size_t>(l_Y + l_Row) * SIZE + l_X], &l_Bitmap.pixels[static_cast<size_t>(l_Row) * l_Width], l_Width);
	m_DirtyFirst = std::min(m_DirtyFirst, l_Y);
	m_DirtyLast = std::max(m_DirtyLast, l_Y + l_Height);

	const float l_Inverse = 1.f / static_cast<float>(SIZE);
	l_Entry.u0 = static_cast<float>(l_X) * l_Inverse;
	l_Entry.v0 = static_cast<float>(l_Y) * l_Inverse;
	l_Entry.u1 = static_cast<float>(l_X + l_Width) * l_Inverse;
	l_Entry.v1 = static_cast<float>(l_Y + l_Height) * l_Inverse;
	l_Entry.left = l_Bitmap.left;
	l_Entry.top = l_Bitmap.top;
	l_Entry.width = l_Bitmap.emWidth;
	l_Entry.height = l_Bitmap.emHeight;
	return &(m_Entries[l_Key] = l_Entry);
}

// ------------------------------------------------------------------------------------------------ system

TextSystem::TextSystem() = default;
TextSystem::~TextSystem() = default;

bool TextSystem::beginFrame()
{
	++m_Frame;
	m_Atlas.beginFrame();
	if (m_Cache.size() > MAX_CACHED_LAYOUTS)
	{
		for (auto l_It = m_Cache.begin(); l_It != m_Cache.end();)
			l_It = m_Frame - l_It->second.lastUsed > CACHE_KEEP_FRAMES ? m_Cache.erase(l_It) : std::next(l_It);
	}
	if (m_SeenGeneration != m_Fonts.generation())
	{
		m_SeenGeneration = m_Fonts.generation();
		m_Cache.clear();
		return true;
	}
	return false;
}

uint64_t TextSystem::hashOf(const TextData& p_Data)
{
	uint64_t l_Hash = 1469598103934665603ull;
	for (const char l_Char : p_Data.text)
		l_Hash = (l_Hash ^ static_cast<uint8_t>(l_Char)) * 1099511628211ull;
	for (const char l_Char : p_Data.family)
		l_Hash = (l_Hash ^ static_cast<uint8_t>(l_Char)) * 1099511628211ull;
	uint32_t l_Bits = 0;
	std::memcpy(&l_Bits, &p_Data.fontSize, sizeof(l_Bits));
	l_Hash = mix(l_Hash, l_Bits);
	std::memcpy(&l_Bits, &p_Data.wrapWidth, sizeof(l_Bits));
	l_Hash = mix(l_Hash, l_Bits);
	return mix(l_Hash, (static_cast<uint64_t>(p_Data.style) << 8) | static_cast<uint8_t>(p_Data.align));
}

bool TextSystem::sameLayoutInputs(const TextData& p_A, const TextData& p_B)
{
	return p_A.fontSize == p_B.fontSize && p_A.wrapWidth == p_B.wrapWidth && p_A.style == p_B.style && p_A.align == p_B.align && p_A.family == p_B.family && p_A.text == p_B.text;
}

const TextLayout& TextSystem::layout(const TextData& p_Data)
{
	const uint64_t l_Hash = hashOf(p_Data);
	const auto [l_Begin, l_End] = m_Cache.equal_range(l_Hash);
	for (auto l_It = l_Begin; l_It != l_End; ++l_It)
	{
		if (sameLayoutInputs(l_It->second.key, p_Data))
		{
			l_It->second.lastUsed = m_Frame;
			return l_It->second.layout;
		}
	}
	const FamilyMetrics l_Metrics(m_Fonts, p_Data.family, p_Data.style);
	CacheEntry l_Entry;
	l_Entry.key = p_Data;
	l_Entry.key.color = {};
	l_Entry.key.size = {};
	l_Entry.layout = layoutText(p_Data.text, l_Metrics, LayoutOptions{ .fontSize = p_Data.fontSize, .wrapWidth = p_Data.wrapWidth, .align = p_Data.align });
	l_Entry.lastUsed = m_Frame;
	return m_Cache.emplace(l_Hash, std::move(l_Entry))->second.layout;
}

bool TextSystem::buildGlyphs(const TextData& p_Data, std::vector<GpuGlyph>& p_Out)
{
	const TextLayout& l_Layout = layout(p_Data);
	const float l_FontSize = p_Data.fontSize;
	const float l_HalfWidth = l_Layout.size.x * 0.5f;
	const float l_HalfHeight = l_Layout.size.y * 0.5f;
	bool l_Complete = true;
	for (const LaidGlyph& l_Glyph : l_Layout.glyphs)
	{
		if (isSpace(l_Glyph.codepoint) || isFormatting(l_Glyph.codepoint) || l_Glyph.font == NO_FACE)
			continue;
		if (l_Glyph.glyph == 0 && isCombining(l_Glyph.codepoint))
			continue;
		const GlyphAtlas::Entry* l_Entry = m_Atlas.get(m_Fonts, l_Glyph.font, l_Glyph.glyph);
		if (l_Entry == nullptr)
		{
			l_Complete = false;
			continue;
		}
		if (l_Entry->blank)
			continue;
		const LaidLine& l_Line = l_Layout.lines[l_Glyph.line];
		const uint8_t l_Synthetic = static_cast<uint8_t>(p_Data.style & ~m_Fonts.faceStyle(l_Glyph.font));
		GpuGlyph l_Out{};
		l_Out.rect[0] = l_Glyph.x + l_Entry->left * l_FontSize - l_HalfWidth;
		l_Out.rect[1] = l_Line.baseline + l_Entry->top * l_FontSize - l_HalfHeight;
		l_Out.rect[2] = l_Entry->width * l_FontSize;
		l_Out.rect[3] = l_Entry->height * l_FontSize;
		l_Out.uv[0] = l_Entry->u0;
		l_Out.uv[1] = l_Entry->v0;
		l_Out.uv[2] = l_Entry->u1;
		l_Out.uv[3] = l_Entry->v1;
		l_Out.baseline = l_Line.baseline - l_HalfHeight;
		l_Out.flags = ((l_Synthetic & TextStyle::Bold) != 0 ? GLYPH_BOLD : 0u) | ((l_Synthetic & TextStyle::Italic) != 0 ? GLYPH_ITALIC : 0u);
		p_Out.push_back(l_Out);
	}
	return l_Complete;
}
} // namespace wb::text
