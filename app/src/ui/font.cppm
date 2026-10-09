// Glyph atlas for the user interface. Glyphs (text and icons) are rasterized with stb_truetype at the exact pixel
// size they are drawn at, so small text stays crisp, and packed on demand into one R8 atlas. The renderer uploads
// the rows that changed since the last frame.
//
// Text uses Inter; icons are glyphs of the Lucide icon font (ISC licence), addressed by the Icon enum.
module;
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module wb.ui.font;

import wb.math;

export namespace wb::ui
{
// Lucide codepoints (private use area)
enum class Icon : uint32_t
{
	None = 0,
	Pen = 0xE1F8,
	Eraser = 0xE28E,
	Select = 0xE1C2,
	Hand = 0xE1D6,
	Undo = 0xE2A0,
	Redo = 0xE29F,
	Minus = 0xE11F,
	Plus = 0xE140,
	Fit = 0xE115,
	Menu = 0xE118,
	Sun = 0xE17B,
	Moon = 0xE121,
	Copy = 0xE0A2,
	Trash = 0xE18D,
	BringToFront = 0xE4F3,
	SendToBack = 0xE4F7,
	Cut = 0xE151,
	Paste = 0xE3EB,
	Lasso = 0xE1CD,
	BoxSelect = 0xE1CA,
	Mouse = 0xE28D,
	Tablet = 0xE134,
	Touch = 0xE1E7,
	Keyboard = 0xE283,
	Bug = 0xE20B,
	Help = 0xE082,
	NewFile = 0xE0CD,
	Open = 0xE246,
	Save = 0xE150,
	SaveAs = 0xE413,
	Grid = 0xE0EC,
	Check = 0xE070,
	Close = 0xE1B1,
	Layers = 0xE52D,
	ChevronUp = 0xE074,
	ChevronDown = 0xE071,
	ChevronRight = 0xE073,
	ChevronLeft = 0xE072,
	Fullscreen = 0xE538,
	Palette = 0xE1DC,
	Move = 0xE124,
	Sliders = 0xE299,
	SelectAll = 0xE1CE,
	Duplicate = 0xE401,
	Forward = 0xE45F,
	Backward = 0xE459,
	Image = 0xE0F9,
	FlipH = 0xE361,
	FlipV = 0xE363,
	Play = 0xE13F,
	Pause = 0xE131,
	ShrinkImage = 0xE540,
	Type = 0xE197,
	Bold = 0xE061,
	Italic = 0xE100,
	AlignLeft = 0xE041,
	AlignCenter = 0xE03C,
	AlignRight = 0xE042,
	Search = 0xE154,
	Upload = 0xE19D,
	ResetTransform = 0xE14B,
	TransformSpace = 0xE2FD,
	Repeat = 0xE149,
	Volume = 0xE1AA,
	VolumeOff = 0xE1AB,
	Lock = 0xE10E,
	Unlock = 0xE10F,
	AlignLeftEdges = 0xE270,
	AlignCenterX = 0xE26C,
	AlignRightEdges = 0xE26E,
	AlignTopEdges = 0xE26F,
	AlignCenterY = 0xE26B,
	AlignBottomEdges = 0xE26D,
	DistributeX = 0xE03D,
	DistributeY = 0xE27D,
	AlignObjects = 0xE274,
	Ruler = 0xE14E,
	Shapes = 0xE4B7,
	Highlighter = 0xE0F7,
	Export = 0xE31A,
	Document = 0xE0D0,
	Spline = 0xE38E,
	Laser = 0xE29D,
	Square = 0xE16A,
	Circle = 0xE07A,
	Triangle = 0xE191,
	Line = 0xE521,
	Arrow = 0xE497,
	Diamond = 0xE2D1,
};

// Text and Icons are the interface's own faces; addFace() hands out more (the font picker shows each family in itself)
enum class FontFace : uint16_t
{
	Text,
	Icons,
};

// A rasterized glyph placed in the atlas. offset is from the pen position (baseline origin) to the quad's top-left.
struct GlyphQuad
{
	Vec2 offset{ 0.f };
	Vec2 size{ 0.f }; // zero for blank glyphs (space)
	Vec2 uv0{ 0.f };
	Vec2 uv1{ 0.f };
};

class FontAtlas
{
public:
	struct Font;

	static constexpr uint32_t ATLAS_SIZE = 1024;

	FontAtlas();
	~FontAtlas();
	FontAtlas(const FontAtlas&) = delete;
	FontAtlas& operator=(const FontAtlas&) = delete;

	// Loads Inter.ttf and lucide.ttf from p_Directory. Returns false (with p_Error set) when a file is missing.
	bool load(const std::filesystem::path& p_Directory, std::string& p_Error);

	// Adds a face drawn from p_Data, which must stay alive and unchanged for as long as the atlas is used. Returns
	// FontFace::Text when the data cannot be read as a font.
	FontFace addFace(const uint8_t* p_Data, size_t p_Size);

	// Call once per frame before building the UI. Returns true when the atlas had overflowed and was reset (the UI
	// of the previous frame was missing glyphs, so another frame is worth drawing).
	bool beginFrame();

	// Rasterizes on first use. Returns nullptr only when the atlas is out of space (reset on the next frame).
	[[nodiscard]] const GlyphQuad* glyph(FontFace p_Face, uint32_t p_Codepoint, int p_PixelSize);
	[[nodiscard]] float advance(FontFace p_Face, uint32_t p_Codepoint, int p_PixelSize) const;
	// Width of p_Text at p_PixelSize (no wrapping)
	[[nodiscard]] float measure(std::string_view p_Text, int p_PixelSize, FontFace p_Face = FontFace::Text) const;
	// Distance from the top of the line box to the baseline, and the line height
	[[nodiscard]] float ascent(int p_PixelSize) const;
	[[nodiscard]] float lineHeight(int p_PixelSize) const;
	// Height of capital letters: centring text on this looks optically centred
	[[nodiscard]] float capHeight(int p_PixelSize) const;
	// Shortens p_Text with an ellipsis so it fits p_MaxWidth
	[[nodiscard]] std::string fit(std::string_view p_Text, int p_PixelSize, float p_MaxWidth) const;
	// Breaks p_Text into lines no wider than p_MaxWidth (words; explicit newlines are kept)
	[[nodiscard]] std::vector<std::string> wrap(std::string_view p_Text, int p_PixelSize, float p_MaxWidth) const;

	// Atlas contents and what changed. takeDirtyRows returns false when nothing changed.
	[[nodiscard]] const uint8_t* pixels() const { return m_Pixels.data(); }
	bool takeDirtyRows(uint32_t& p_FirstRow, uint32_t& p_RowCount);
	// The whole atlas must be uploaded again (after a reset)
	void markAllDirty();

private:
	[[nodiscard]] static uint64_t key(FontFace p_Face, uint32_t p_Codepoint, int p_PixelSize);
	[[nodiscard]] bool pack(uint32_t p_Width, uint32_t p_Height, uint32_t& p_X, uint32_t& p_Y);
	void reset();

	std::unique_ptr<Font> m_Text;
	std::unique_ptr<Font> m_Icons;
	std::vector<std::unique_ptr<Font>> m_Extra;
	[[nodiscard]] const Font& fontFor(FontFace p_Face) const;
	std::vector<uint8_t> m_Pixels;
	std::unordered_map<uint64_t, GlyphQuad> m_Glyphs;

	// Shelf packer
	uint32_t m_ShelfX = 1;
	uint32_t m_ShelfY = 1;
	uint32_t m_ShelfHeight = 0;

	uint32_t m_DirtyFirst = 0;
	uint32_t m_DirtyLast = 0; // exclusive; first >= last means clean
	bool m_Overflowed = false;
};

// Decodes the next UTF-8 code point of p_Text starting at p_Index (advances it). Malformed bytes become U+FFFD.
[[nodiscard]] uint32_t nextCodepoint(std::string_view p_Text, size_t& p_Index);
} // namespace wb::ui
