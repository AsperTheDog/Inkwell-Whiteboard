// Everything the app needs to lay out and draw text, independent of Vulkan: the font registry, a cache of laid-out
// text, and the CPU side of the glyph atlas (signed distance fields, packed on demand). TextRenderer uploads the atlas
// and draws the glyph records this builds.
module;
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

export module wb.text.system;

import wb.math;
import wb.doc.object;
import wb.text.layout;
import wb.text.fonts;

export namespace wb::text
{
// One glyph quad as the text shader reads it (object-local coordinates, y down)
struct GpuGlyph
{
	float rect[4];      // x, y, width, height of the quad
	float uv[4];        // u0, v0, u1, v1 in the atlas
	float baseline;     // object-local y of the baseline (italics shear around it)
	uint32_t flags;     // GLYPH_BOLD, GLYPH_ITALIC
	float padding[2];
};
static_assert(sizeof(GpuGlyph) == 48);

inline constexpr uint32_t GLYPH_BOLD = 1u << 0;
inline constexpr uint32_t GLYPH_ITALIC = 1u << 1;

// Signed distance field glyphs in one R8 atlas. When it is full it is emptied and refilled from what is drawn next.
class GlyphAtlas
{
public:
	static constexpr uint32_t SIZE = 2048;

	struct Entry
	{
		float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
		float left = 0.f, top = 0.f; // ems, from the glyph origin to the quad's top-left
		float width = 0.f, height = 0.f; // ems
		bool blank = false;
	};

	GlyphAtlas();

	// Call once per frame. Returns true when the atlas had overflowed and was emptied.
	bool beginFrame();
	// Rasterizes on first use. nullptr when the atlas is full (it is reset by the next beginFrame()).
	[[nodiscard]] const Entry* get(const FontRegistry& p_Fonts, FaceId p_Face, uint32_t p_Glyph);

	[[nodiscard]] const uint8_t* pixels() const { return m_Pixels.data(); }
	bool takeDirtyRows(uint32_t& p_FirstRow, uint32_t& p_RowCount);
	void markAllDirty();
	[[nodiscard]] size_t glyphCount() const { return m_Entries.size(); }
	[[nodiscard]] bool overflowed() const { return m_Overflowed; }

private:
	void reset();

	std::vector<uint8_t> m_Pixels;
	std::unordered_map<uint64_t, Entry> m_Entries;
	uint32_t m_ShelfX = 1;
	uint32_t m_ShelfY = 1;
	uint32_t m_ShelfHeight = 0;
	uint32_t m_DirtyFirst = 0;
	uint32_t m_DirtyLast = 0;
	bool m_Overflowed = false;
};

class TextSystem
{
public:
	TextSystem();
	~TextSystem();
	TextSystem(const TextSystem&) = delete;
	TextSystem& operator=(const TextSystem&) = delete;

	[[nodiscard]] FontRegistry& fonts() { return m_Fonts; }
	[[nodiscard]] const FontRegistry& fonts() const { return m_Fonts; }
	[[nodiscard]] GlyphAtlas& atlas() { return m_Atlas; }

	// Call once per frame: ages the layout cache. Returns true when fonts changed since the last call (text boxes
	// may have to be measured again).
	bool beginFrame();

	// The layout of p_Data (its text, family, style, size, wrap width and alignment; the rest is ignored). The
	// reference stays valid until the next beginFrame().
	[[nodiscard]] const TextLayout& layout(const TextData& p_Data);
	// The extent p_Data needs
	[[nodiscard]] Vec2 measure(const TextData& p_Data) { return layout(p_Data).size; }

	// Appends the glyph quads of p_Data (object-local coordinates, centred on the origin like the object's box).
	// False when the atlas ran out of space, so some glyphs are missing from this frame.
	bool buildGlyphs(const TextData& p_Data, std::vector<GpuGlyph>& p_Out);

private:
	struct CacheEntry
	{
		TextData key;
		TextLayout layout;
		uint64_t lastUsed = 0;
	};

	[[nodiscard]] static uint64_t hashOf(const TextData& p_Data);
	[[nodiscard]] static bool sameLayoutInputs(const TextData& p_A, const TextData& p_B);

	FontRegistry m_Fonts;
	GlyphAtlas m_Atlas;
	std::unordered_multimap<uint64_t, CacheEntry> m_Cache;
	uint64_t m_Frame = 0;
	uint64_t m_SeenGeneration = 0;
};
} // namespace wb::text
