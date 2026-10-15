// Glyph atlas for the user interface. Glyphs (text and icons) are rasterized with stb_truetype at the exact pixel
// size they are drawn at, so small text stays crisp, and packed on demand into one R8 atlas. The renderer uploads
// the rows that changed since the last frame.
//
// Text uses Inter; icons are glyphs of the Lucide icon font (ISC licence), addressed by the Icon enum.
//
// Inter has Latin, Greek and Cyrillic. A character it lacks is looked up, in this order, in the language's own font
// (setLanguageFont: a file shipped with the app that holds the glyphs of that language's texts, so the interface is
// readable on any machine) and then in whatever the fallback finder offers (the fonts installed on the system).
//
// Scripts that need more than one glyph per character, or run right to left (Arabic, Hebrew, Indic, Thai...), are
// shaped with HarfBuzz and ordered with SheenBidi (see shape()); everything else keeps the plain, fast path.
module;
#include <cstdint>
#include <filesystem>
#include <functional>
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
	Crop = 0xE0AF,
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
	Captions = 0xE3A7,
	Note = 0xE302,
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

// A font that has some character, as the fallback finder reports it. `id` tells the atlas it has seen the face before.
struct FallbackFace
{
	const uint8_t* data = nullptr; // must stay alive and unchanged for as long as the atlas is used
	int offset = 0;                // where the font starts in the data (a collection holds several)
	uint64_t id = 0;
	size_t size = 0;               // bytes in data
};

// One glyph of shaped text, in the order it is drawn left to right
struct ShapedGlyph
{
	FontFace face = FontFace::Text;
	uint32_t glyph = 0;     // glyph index in the face (not a code point)
	float advance = 0.f;    // pixels
	float xOffset = 0.f;    // pixels, from the pen position
	float yOffset = 0.f;    // pixels, up is positive
	uint32_t cluster = 0;   // byte offset in the source text of the characters this glyph draws
	bool rtl = false;       // belongs to a right-to-left run
};

struct ShapedText
{
	std::vector<ShapedGlyph> glyphs;
	float width = 0.f;
	bool rtl = false; // the paragraph direction
};

class FontAtlas
{
public:
	struct Font;
	using FallbackFinder = std::function<FallbackFace(uint32_t p_Codepoint)>;

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

	// The font for the characters of the current language that Inter lacks (an empty path: none). False when the file
	// cannot be read. Glyphs already drawn from another language font are forgotten.
	bool setLanguageFont(const std::filesystem::path& p_Path);
	// Paragraph direction for text without strong characters (digits, punctuation): the interface language's
	void setRightToLeft(bool p_Rtl);
	[[nodiscard]] bool rightToLeft() const { return m_BaseRtl; }
	// True for text that has to be shaped: it has characters of scripts that join, reorder or run right to left
	[[nodiscard]] static bool needsShaping(std::string_view p_Text);
	// Shapes p_Text into glyphs in drawing order (reordered for mixed directions). Cached for the next calls.
	[[nodiscard]] const ShapedText& shape(std::string_view p_Text, int p_PixelSize) const;
	// Where the caret sits for the character at byte p_Index, measured from the left of the text as drawn
	[[nodiscard]] float caretX(std::string_view p_Text, size_t p_Index, int p_PixelSize) const;
	// A glyph by its index in the face, as shape() reports them
	[[nodiscard]] const GlyphQuad* glyphByIndex(FontFace p_Face, uint32_t p_Glyph, int p_PixelSize);

	// Asked once per character that neither Inter nor the language font has
	void setFallbackFinder(FallbackFinder p_Finder);
	// Call when the fallback finder may know more than before (the installed fonts were scanned): characters that had
	// no font are asked about again
	void forgetFallbackMisses();

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
	// The face that draws p_Codepoint when p_Face is asked to: Inter, or the language font, or a fallback
	[[nodiscard]] FontFace resolve(FontFace p_Face, uint32_t p_Codepoint) const;
	FontFace addBorrowed(const uint8_t* p_Data, int p_Offset, size_t p_Size) const;
	[[nodiscard]] const GlyphQuad* place(uint64_t p_Key, const Font& p_Font, int p_Glyph, int p_PixelSize);
	[[nodiscard]] ShapedText shapeUncached(std::string_view p_Text, int p_PixelSize) const;
	[[nodiscard]] static uint64_t key(FontFace p_Face, uint32_t p_Codepoint, int p_PixelSize);
	[[nodiscard]] bool pack(uint32_t p_Width, uint32_t p_Height, uint32_t& p_X, uint32_t& p_Y);
	void reset();

	std::unique_ptr<Font> m_Text;
	std::unique_ptr<Font> m_Icons;
	mutable std::vector<std::unique_ptr<Font>> m_Extra;
	FallbackFinder m_Finder;
	FontFace m_LanguageFace = FontFace::Text; // Text: no language font
	std::unordered_map<std::string, FontFace> m_LanguageFaces; // by file, so switching back and forth reads each once
	mutable std::unordered_map<uint32_t, FontFace> m_Resolved;  // characters Inter lacks -> their face (Text: none found)
	mutable std::unordered_map<uint64_t, FontFace> m_FallbackFaces;
	bool m_BaseRtl = false;
	mutable std::unordered_map<std::string, ShapedText> m_Shaped;
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
