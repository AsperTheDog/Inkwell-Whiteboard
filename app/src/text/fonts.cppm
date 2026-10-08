// Fonts for text objects.
//
// Families come from four places, in this order of precedence: the fonts shipped with the app, the fonts installed
// on the system (found by a background scan of the font folders; only their name tables are read), the fonts the user
// imported (copied to the app's data folder) and the fonts embedded in the open board. A family has up to four real
// faces (regular, bold, italic, bold italic); a missing one is imitated at draw time (see FontRegistry::resolve).
//
// A face is loaded (read completely and parsed) the first time something is drawn with it.
module;
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

export module wb.text.fonts;

import wb.math;
import wb.doc.object;
import wb.text.layout;

export namespace wb::text
{
using FaceId = uint32_t;
inline constexpr FaceId NO_FACE = UINT32_MAX;

// A family as the font picker lists it
struct FamilyInfo
{
	std::string name;
	uint8_t styles = 0;      // bit s set: a real face with TextStyle flags s exists
	bool bundled = false;    // ships with the app
	bool embeddable = true;  // the licence allows storing the font in a board (OS/2 fsType)
};

// The outcome of resolve(): which face to draw with and which parts of the requested style it does not have
struct ResolvedFace
{
	FaceId face = NO_FACE;
	uint8_t actualStyle = 0;    // TextStyle flags of the face
	uint8_t syntheticStyle = 0; // requested flags the face lacks: bold is drawn thicker, italic slanted
};

// A rasterized glyph as a signed distance field. Offsets and size are in ems relative to the glyph origin on the
// baseline (y points down); `pixels` has width * height bytes (128 is the outline, higher is inside).
struct SdfBitmap
{
	int width = 0;
	int height = 0;
	float left = 0.f;
	float top = 0.f;
	float emWidth = 0.f;
	float emHeight = 0.f;
	std::vector<uint8_t> pixels;
};

namespace sdf
{
inline constexpr int EM_PIXELS = 64;     // the em square is rasterized at this size
inline constexpr int PADDING = 8;        // pixels of distance field around the outline
inline constexpr int ON_EDGE = 128;      // value at the outline
inline constexpr float DISTANCE_SCALE = static_cast<float>(ON_EDGE) / static_cast<float>(PADDING); // value units per pixel
} // namespace sdf

class FontRegistry
{
public:
	FontRegistry();
	~FontRegistry();
	FontRegistry(const FontRegistry&) = delete;
	FontRegistry& operator=(const FontRegistry&) = delete;

	// ---- sources
	// Registers every font file in p_Directory as shipped with the app. Returns how many were found.
	size_t addBundledDirectory(const std::filesystem::path& p_Directory);
	// Folder where imported fonts are kept; it is scanned like a system folder
	void setUserDirectory(const std::filesystem::path& p_Directory);
	// Reads the name tables of the installed fonts on a worker thread. pump() brings the result in.
	void scanSystemFonts();
	// Call once per frame: merges finished scan results. True when the family list changed.
	bool pump();
	[[nodiscard]] bool scanning() const { return m_ScanRunning.load(); }
	// Copies a font file into the user folder and registers it. Returns the family name, or empty with p_Error set.
	std::string importFont(const std::filesystem::path& p_File, std::string& p_Error);
	// Registers a font embedded in a board. Ignored when the family and style already exist from another source.
	void addDocumentFont(std::string_view p_Family, uint8_t p_Style, std::vector<uint8_t> p_Bytes);

	// ---- queries
	// All families sorted by name; changes only when pump() reports it. Bundled families come first.
	[[nodiscard]] const std::vector<FamilyInfo>& families() const { return m_FamilyList; }
	[[nodiscard]] bool hasFamily(std::string_view p_Name) const;
	// Increments whenever the set of fonts changes (cached layouts are then out of date)
	[[nodiscard]] uint64_t generation() const { return m_Generation; }
	static constexpr std::string_view DEFAULT_FAMILY = "Inter";

	// Face for a family and style. An unknown family falls back to the default one.
	ResolvedFace resolve(std::string_view p_Family, uint8_t p_Style);
	// A face that has p_Codepoint (the default family first, then installed fonts for scripts and symbols), or NO_FACE
	FaceId fallbackFor(uint32_t p_Codepoint, uint8_t p_Style);

	// ---- faces (valid for ids returned by resolve() / fallbackFor())
	[[nodiscard]] uint32_t glyphIndex(FaceId p_Face, uint32_t p_Codepoint) const;
	[[nodiscard]] float advanceEm(FaceId p_Face, uint32_t p_Glyph) const;
	[[nodiscard]] float kerningEm(FaceId p_Face, uint32_t p_LeftGlyph, uint32_t p_RightGlyph) const;
	[[nodiscard]] LineMetrics lineMetrics(FaceId p_Face) const;
	[[nodiscard]] uint8_t faceStyle(FaceId p_Face) const;
	// Draws a glyph into a distance field bitmap. False for blank glyphs (space).
	bool rasterize(FaceId p_Face, uint32_t p_Glyph, SdfBitmap& p_Bitmap) const;
	// The file of a face, for embedding in a board; empty when its licence forbids embedding
	[[nodiscard]] std::span<const uint8_t> embeddableBytes(FaceId p_Face) const;
	[[nodiscard]] std::string_view faceFamily(FaceId p_Face) const;
	// True when a family is small enough to be read just to show its name in itself (the font picker)
	[[nodiscard]] bool isLightweight(std::string_view p_Family) const;
	// Fonts that ship with the app are never embedded in boards
	[[nodiscard]] bool isBundled(FaceId p_Face) const;

	// The parsed font of a face, for code that draws glyphs itself (the font picker preview). The pointer stays valid
	// for the registry's lifetime. Returns nullptr and 0 for an unknown face.
	[[nodiscard]] const uint8_t* faceData(FaceId p_Face, size_t& p_Size) const;

private:
	struct Source
	{
		std::filesystem::path path;
		uint32_t faceIndex = 0; // inside a font collection
		std::vector<uint8_t> bytes; // embedded fonts keep their data here
		bool bundled = false;
		bool document = false;
		bool embeddable = true;
		FaceId face = NO_FACE; // once loaded
	};

	struct Family
	{
		std::string name;
		std::array<std::optional<Source>, 4> styles;
	};

	struct Face;

	struct ScanResult
	{
		std::string family;
		uint8_t style = 0;
		std::filesystem::path path;
		uint32_t faceIndex = 0;
		bool embeddable = true;
	};

	void registerSource(const std::string& p_Family, uint8_t p_Style, Source p_Source);
	void rebuildFamilyList();
	FaceId load(Source& p_Source, uint8_t p_Style, const std::string& p_Family);
	Family* findFamily(std::string_view p_Name);
	const Family* findFamily(std::string_view p_Name) const;

	std::map<std::string, Family> m_Families; // key: lower-case family name
	std::vector<FamilyInfo> m_FamilyList;
	std::vector<std::unique_ptr<Face>> m_Faces;
	std::unordered_map<uint64_t, FaceId> m_FallbackCache; // codepoint + style -> face
	std::filesystem::path m_UserDirectory;
	uint64_t m_Generation = 1;

	std::thread m_ScanThread;
	std::atomic<bool> m_ScanRunning{ false };
	std::mutex m_ScanMutex;
	std::vector<ScanResult> m_ScanResults;
	bool m_ScanReady = false;
};

// Metrics for the layout of one family and style: every code point is drawn by the family's face, or by a fallback
// face when it lacks the glyph. The object must not outlive its registry.
class FamilyMetrics final : public Metrics
{
public:
	FamilyMetrics(FontRegistry& p_Registry, std::string_view p_Family, uint8_t p_Style);

	[[nodiscard]] GlyphInfo glyph(uint32_t p_Codepoint) const override;
	[[nodiscard]] float kerning(const GlyphInfo& p_Left, uint32_t p_LeftCodepoint, const GlyphInfo& p_Right, uint32_t p_RightCodepoint) const override;
	[[nodiscard]] LineMetrics lineMetrics() const override;

	[[nodiscard]] const ResolvedFace& primary() const { return m_Primary; }

	// Synthetic bold makes glyphs wider by this many ems
	static constexpr float SYNTHETIC_BOLD_EM = 0.04f;

private:
	FontRegistry& m_Registry;
	ResolvedFace m_Primary;
	uint8_t m_Style = 0;
};
} // namespace wb::text
