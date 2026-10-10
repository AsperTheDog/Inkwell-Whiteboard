module;
#include <array>
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <memory>
#include <unordered_map>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.io.serializer;

import wb.math;
import wb.doc.document;
import wb.doc.object;
import wb.io.blob;
import wb.io.file;

static_assert(std::endian::native == std::endian::little, "the .wbrd reader/writer assumes a little endian host");

namespace wb
{
namespace
{
constexpr std::array<uint8_t, 8> MAGIC{ 'W', 'B', 'R', 'D', 0x0D, 0x0A, 0x1A, 0x0A };
constexpr size_t CHUNK_HEADER_BYTES = 4 + 8 + 4;
constexpr size_t MAX_PATH_BYTES = 4096;

constexpr uint32_t tag(const char (&p_Text)[5])
{
	return static_cast<uint32_t>(static_cast<uint8_t>(p_Text[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(p_Text[1])) << 8) | (static_cast<uint32_t>(static_cast<uint8_t>(p_Text[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(p_Text[3])) << 24);
}
constexpr uint32_t TAG_META = tag("META");
constexpr uint32_t TAG_VIEW = tag("VIEW");
constexpr uint32_t TAG_OBJS = tag("OBJS");
constexpr uint32_t TAG_ASET = tag("ASET");
constexpr uint32_t TAG_FONT = tag("FONT");
constexpr uint32_t TAG_LOCK = tag("LOCK"); // ids of the locked objects
constexpr uint32_t TAG_ABIG = tag("ABIG"); // one big asset, written by a background save
constexpr uint32_t TAG_AREF = tag("AREF"); // big assets that stay in a file of their own (recovery files)
constexpr uint32_t TAG_END = tag("END ");

enum class ObjectType : uint8_t
{
	Stroke = 1,
	Image = 2,
	Text = 3,
	Video = 4,
};

constexpr size_t IMAGE_BODY_BYTES = 6 * sizeof(double) + sizeof(uint64_t) + 2 * sizeof(float) + 1 + sizeof(uint32_t);
constexpr size_t VIDEO_BODY_BASE_BYTES = 6 * sizeof(double) + sizeof(uint64_t) + 2 * sizeof(float) + 2 + sizeof(float) + 2;

size_t videoBodyBytes(const VideoData& p_Video)
{
	size_t l_Bytes = VIDEO_BODY_BASE_BYTES + 1; // and the number of subtitle files
	for (const SubtitleFile& l_File : p_Video.subtitleFiles)
		l_Bytes += 2 * sizeof(uint32_t) + l_File.name.size() + l_File.text.size();
	return l_Bytes;
}
constexpr size_t MAX_NAME_BYTES = 1024;
constexpr size_t MAX_TEXT_BYTES = 64u << 20;
constexpr uint64_t MAX_FONT_BYTES = 1ull << 28;
constexpr uint64_t MAX_ASSET_BYTES = 1ull << 40;
// Chunks up to this size are read into memory to be parsed; the only bigger ones that make sense carry big files
constexpr uint64_t MAX_RESIDENT_CHUNK = 1ull << 31;
// Header fields of a big asset chunk (before the file itself) never need more room than this
constexpr size_t MAX_ASSET_PREFIX_BYTES = 8 + 4 + 4 + 4 + MAX_NAME_BYTES + 8;

constexpr size_t TRANSFORM_BYTES = 6 * sizeof(double);
constexpr size_t STROKE_HEADER_BYTES = 4 * sizeof(float) + sizeof(float) + 1 + sizeof(uint32_t);
constexpr size_t STROKE_POINT_BYTES = 3 * sizeof(float);
constexpr size_t OBJECT_HEADER_BYTES = sizeof(uint64_t) + 1 + sizeof(uint32_t);

constexpr std::array<uint32_t, 256> makeCrcTable()
{
	std::array<uint32_t, 256> l_Table{};
	for (uint32_t i = 0; i < 256; ++i)
	{
		uint32_t l_Value = i;
		for (int k = 0; k < 8; ++k)
			l_Value = (l_Value & 1u) != 0 ? 0xEDB88320u ^ (l_Value >> 1) : l_Value >> 1;
		l_Table[i] = l_Value;
	}
	return l_Table;
}
constexpr std::array<uint32_t, 256> CRC_TABLE = makeCrcTable();

// Slicing-by-8 tables: eight bytes per step (big videos are checksummed, so the plain loop is too slow)
constexpr std::array<std::array<uint32_t, 256>, 8> makeCrcSlices()
{
	std::array<std::array<uint32_t, 256>, 8> l_Slices{};
	l_Slices[0] = CRC_TABLE;
	for (size_t i = 0; i < 256; ++i)
	{
		uint32_t l_Value = l_Slices[0][i];
		for (size_t k = 1; k < 8; ++k)
		{
			l_Value = l_Slices[0][l_Value & 0xFFu] ^ (l_Value >> 8);
			l_Slices[k][i] = l_Value;
		}
	}
	return l_Slices;
}
constexpr std::array<std::array<uint32_t, 256>, 8> CRC_SLICES = makeCrcSlices();

// Continues a checksum (start with 0xFFFFFFFF, finish with ~)
uint32_t crcUpdate(uint32_t p_Crc, const uint8_t* p_Data, size_t p_Size)
{
	while (p_Size >= 8)
	{
		uint32_t l_Low;
		uint32_t l_High;
		std::memcpy(&l_Low, p_Data, 4);
		std::memcpy(&l_High, p_Data + 4, 4);
		l_Low ^= p_Crc;
		p_Crc = CRC_SLICES[7][l_Low & 0xFFu] ^ CRC_SLICES[6][(l_Low >> 8) & 0xFFu] ^ CRC_SLICES[5][(l_Low >> 16) & 0xFFu] ^ CRC_SLICES[4][l_Low >> 24]
			^ CRC_SLICES[3][l_High & 0xFFu] ^ CRC_SLICES[2][(l_High >> 8) & 0xFFu] ^ CRC_SLICES[1][(l_High >> 16) & 0xFFu] ^ CRC_SLICES[0][l_High >> 24];
		p_Data += 8;
		p_Size -= 8;
	}
	while (p_Size-- > 0)
		p_Crc = CRC_TABLE[(p_Crc ^ *p_Data++) & 0xFFu] ^ (p_Crc >> 8);
	return p_Crc;
}

class Writer
{
public:
	void u8(const uint8_t p_Value) { m_Bytes.push_back(p_Value); }
	void u32(const uint32_t p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void u64(const uint64_t p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void f32(const float p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void f64(const double p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void string(const std::string_view p_Text)
	{
		u32(static_cast<uint32_t>(p_Text.size()));
		raw(p_Text.data(), p_Text.size());
	}
	void raw(const void* p_Data, const size_t p_Size)
	{
		const auto* l_Bytes = static_cast<const uint8_t*>(p_Data);
		m_Bytes.insert(m_Bytes.end(), l_Bytes, l_Bytes + p_Size);
	}

	void beginChunk(const uint32_t p_Tag)
	{
		m_ChunkStart = m_Bytes.size();
		u32(p_Tag);
		u64(0); // length, patched by endChunk
		u32(0); // crc, patched by endChunk
	}
	void endChunk()
	{
		const size_t l_PayloadStart = m_ChunkStart + CHUNK_HEADER_BYTES;
		const uint64_t l_Length = m_Bytes.size() - l_PayloadStart;
		std::memcpy(m_Bytes.data() + m_ChunkStart + 4, &l_Length, sizeof(l_Length));
		if (m_DeferredCrcs != nullptr)
		{
			m_DeferredCrcs->push_back(m_ChunkStart);
			return;
		}
		const uint32_t l_Crc = crc32(std::span<const uint8_t>(m_Bytes).subspan(l_PayloadStart));
		std::memcpy(m_Bytes.data() + m_ChunkStart + 12, &l_Crc, sizeof(l_Crc));
	}
	// Chunk checksums are not computed by endChunk: the start of every finished chunk is added to p_Chunks instead
	// (see finishChecksums)
	void deferChecksums(std::vector<size_t>* p_Chunks) { m_DeferredCrcs = p_Chunks; }

	void reserve(const size_t p_Bytes) { m_Bytes.reserve(p_Bytes); }
	std::vector<uint8_t> take() { return std::move(m_Bytes); }

private:
	std::vector<uint8_t> m_Bytes;
	size_t m_ChunkStart = 0;
	std::vector<size_t>* m_DeferredCrcs = nullptr;
};

// Bounds-checked cursor. Once a read runs past the end, ok() stays false and every later read returns zero.
class Reader
{
public:
	explicit Reader(const std::span<const uint8_t> p_Data) : m_Data(p_Data) {}

	[[nodiscard]] bool ok() const { return m_Ok; }
	[[nodiscard]] size_t remaining() const { return m_Data.size() - m_Pos; }

	uint8_t u8() { return read<uint8_t>(); }
	uint32_t u32() { return read<uint32_t>(); }
	uint64_t u64() { return read<uint64_t>(); }
	float f32() { return read<float>(); }
	double f64() { return read<double>(); }

	std::span<const uint8_t> take(const size_t p_Size)
	{
		if (!m_Ok || p_Size > remaining())
		{
			m_Ok = false;
			return {};
		}
		const std::span<const uint8_t> l_Result = m_Data.subspan(m_Pos, p_Size);
		m_Pos += p_Size;
		return l_Result;
	}

	std::string string(const size_t p_MaxBytes)
	{
		const uint32_t l_Size = u32();
		if (l_Size > p_MaxBytes)
		{
			m_Ok = false;
			return {};
		}
		const std::span<const uint8_t> l_Bytes = take(l_Size);
		return std::string(reinterpret_cast<const char*>(l_Bytes.data()), l_Bytes.size());
	}

private:
	template <typename T>
	T read()
	{
		T l_Value{};
		if (!m_Ok || sizeof(T) > remaining())
		{
			m_Ok = false;
			return l_Value;
		}
		std::memcpy(&l_Value, m_Data.data() + m_Pos, sizeof(T));
		m_Pos += sizeof(T);
		return l_Value;
	}

	std::span<const uint8_t> m_Data;
	size_t m_Pos = 0;
	bool m_Ok = true;
};

bool finite(const double p_Value)
{
	return std::isfinite(p_Value);
}

void writeTransform(Writer& p_Writer, const Affine2& p_Transform)
{
	p_Writer.f64(p_Transform.linear[0][0]);
	p_Writer.f64(p_Transform.linear[0][1]);
	p_Writer.f64(p_Transform.linear[1][0]);
	p_Writer.f64(p_Transform.linear[1][1]);
	p_Writer.f64(p_Transform.translation.x);
	p_Writer.f64(p_Transform.translation.y);
}

bool readTransform(Reader& p_Reader, Affine2& p_Transform)
{
	double l_Values[6];
	for (double& l_Value : l_Values)
		l_Value = p_Reader.f64();
	if (!p_Reader.ok())
		return false;
	for (const double l_Value : l_Values)
		if (!finite(l_Value))
			return false;
	p_Transform.linear[0][0] = l_Values[0];
	p_Transform.linear[0][1] = l_Values[1];
	p_Transform.linear[1][0] = l_Values[2];
	p_Transform.linear[1][1] = l_Values[3];
	p_Transform.translation = DVec2{ l_Values[4], l_Values[5] };
	return true;
}

size_t strokeBodyBytes(const StrokeData& p_Stroke)
{
	return TRANSFORM_BYTES + STROKE_HEADER_BYTES + p_Stroke.points.size() * STROKE_POINT_BYTES;
}

void writeStroke(Writer& p_Writer, const Object& p_Object, const StrokeData& p_Stroke)
{
	p_Writer.u64(p_Object.id);
	p_Writer.u8(static_cast<uint8_t>(ObjectType::Stroke));
	p_Writer.u32(static_cast<uint32_t>(strokeBodyBytes(p_Stroke)));
	writeTransform(p_Writer, p_Object.transform);
	p_Writer.f32(p_Stroke.style.color.r);
	p_Writer.f32(p_Stroke.style.color.g);
	p_Writer.f32(p_Stroke.style.color.b);
	p_Writer.f32(p_Stroke.style.color.a);
	p_Writer.f32(p_Stroke.style.size);
	p_Writer.u8(static_cast<uint8_t>(p_Stroke.style.brush));
	p_Writer.u32(static_cast<uint32_t>(p_Stroke.points.size()));
	// x, y, radius as three floats each, which is exactly how a point sits in memory
	static_assert(sizeof(StrokePoint) == STROKE_POINT_BYTES && std::is_trivially_copyable_v<StrokePoint>);
	p_Writer.raw(p_Stroke.points.data(), p_Stroke.points.size() * STROKE_POINT_BYTES);
}

// Returns false for corrupt data; p_Skipped is set for well-formed objects of a kind this version does not know
bool readStroke(Reader& p_Reader, Object& p_Object, bool& p_Skipped)
{
	if (!readTransform(p_Reader, p_Object.transform))
		return false;
	StrokeData l_Stroke;
	l_Stroke.style.color.r = p_Reader.f32();
	l_Stroke.style.color.g = p_Reader.f32();
	l_Stroke.style.color.b = p_Reader.f32();
	l_Stroke.style.color.a = p_Reader.f32();
	l_Stroke.style.size = p_Reader.f32();
	const uint8_t l_Brush = p_Reader.u8();
	const uint32_t l_Count = p_Reader.u32();
	if (!p_Reader.ok() || static_cast<uint64_t>(l_Count) * STROKE_POINT_BYTES > p_Reader.remaining())
		return false;
	if (!finite(l_Stroke.style.color.r) || !finite(l_Stroke.style.color.g) || !finite(l_Stroke.style.color.b) || !finite(l_Stroke.style.color.a) || !finite(l_Stroke.style.size))
		return false;
	if (l_Brush > static_cast<uint8_t>(BrushKind::Highlighter))
	{
		p_Skipped = true; // a brush from a newer version
		return true;
	}
	l_Stroke.style.brush = static_cast<BrushKind>(l_Brush);

	l_Stroke.points.resize(l_Count);
	for (StrokePoint& l_Point : l_Stroke.points)
	{
		l_Point.position.x = p_Reader.f32();
		l_Point.position.y = p_Reader.f32();
		l_Point.radius = p_Reader.f32();
		if (!finite(l_Point.position.x) || !finite(l_Point.position.y) || !finite(l_Point.radius) || l_Point.radius < 0.f)
			return false;
	}
	if (!p_Reader.ok() || l_Stroke.points.empty())
		return false;
	p_Object.payload = std::move(l_Stroke);
	return true;
}

void writeImage(Writer& p_Writer, const Object& p_Object, const ImageData& p_Image)
{
	p_Writer.u64(p_Object.id);
	p_Writer.u8(static_cast<uint8_t>(ObjectType::Image));
	p_Writer.u32(static_cast<uint32_t>(IMAGE_BODY_BYTES));
	writeTransform(p_Writer, p_Object.transform);
	p_Writer.u64(p_Image.asset);
	p_Writer.f32(p_Image.size.x);
	p_Writer.f32(p_Image.size.y);
	p_Writer.u8(p_Image.playing ? 1 : 0);
	p_Writer.u32(p_Image.frame);
}

bool readImage(Reader& p_Reader, Object& p_Object)
{
	if (!readTransform(p_Reader, p_Object.transform))
		return false;
	ImageData l_Image;
	l_Image.asset = p_Reader.u64();
	l_Image.size.x = p_Reader.f32();
	l_Image.size.y = p_Reader.f32();
	l_Image.playing = p_Reader.u8() != 0;
	l_Image.frame = p_Reader.u32();
	if (!p_Reader.ok() || l_Image.asset == INVALID_ASSET_ID || !finite(l_Image.size.x) || !finite(l_Image.size.y) || l_Image.size.x <= 0.f || l_Image.size.y <= 0.f)
		return false;
	p_Object.payload = l_Image;
	return true;
}

void writeVideo(Writer& p_Writer, const Object& p_Object, const VideoData& p_Video)
{
	p_Writer.u64(p_Object.id);
	p_Writer.u8(static_cast<uint8_t>(ObjectType::Video));
	p_Writer.u32(static_cast<uint32_t>(videoBodyBytes(p_Video)));
	writeTransform(p_Writer, p_Object.transform);
	p_Writer.u64(p_Video.asset);
	p_Writer.f32(p_Video.size.x);
	p_Writer.f32(p_Video.size.y);
	p_Writer.u8(p_Video.loop ? 1 : 0);
	p_Writer.u8(p_Video.muted ? 1 : 0);
	p_Writer.f32(p_Video.volume);
	p_Writer.u8(static_cast<uint8_t>(p_Video.audioTrack));
	p_Writer.u8(static_cast<uint8_t>(p_Video.subtitleTrack));
	p_Writer.u8(static_cast<uint8_t>(std::min(p_Video.subtitleFiles.size(), MAX_SUBTITLE_FILES)));
	for (size_t i = 0; i < p_Video.subtitleFiles.size() && i < MAX_SUBTITLE_FILES; ++i)
	{
		p_Writer.string(p_Video.subtitleFiles[i].name);
		p_Writer.string(p_Video.subtitleFiles[i].text);
	}
}

bool readVideo(Reader& p_Reader, Object& p_Object)
{
	if (!readTransform(p_Reader, p_Object.transform))
		return false;
	VideoData l_Video;
	l_Video.asset = p_Reader.u64();
	l_Video.size.x = p_Reader.f32();
	l_Video.size.y = p_Reader.f32();
	l_Video.loop = p_Reader.u8() != 0;
	l_Video.muted = p_Reader.u8() != 0;
	if (p_Reader.remaining() >= sizeof(float)) // boards saved before the volume slider end here
	{
		l_Video.volume = p_Reader.f32();
		if (!finite(l_Video.volume))
			return false;
		l_Video.volume = std::clamp(l_Video.volume, 0.f, 1.f);
	}
	if (p_Reader.remaining() >= 2) // boards saved before sound tracks and subtitles end here
	{
		l_Video.audioTrack = static_cast<int8_t>(std::max<int>(-1, static_cast<int8_t>(p_Reader.u8())));
		l_Video.subtitleTrack = static_cast<int8_t>(std::max<int>(-1, static_cast<int8_t>(p_Reader.u8())));
	}
	if (p_Reader.remaining() >= 1) // boards saved before subtitle files end here
	{
		const uint8_t l_Count = p_Reader.u8();
		if (l_Count > MAX_SUBTITLE_FILES)
			return false;
		for (uint8_t i = 0; i < l_Count; ++i)
		{
			SubtitleFile l_File;
			l_File.name = p_Reader.string(MAX_NAME_BYTES);
			l_File.text = p_Reader.string(MAX_SUBTITLE_FILE_BYTES);
			if (!p_Reader.ok())
				return false;
			l_Video.subtitleFiles.push_back(std::move(l_File));
		}
	}
	if (!p_Reader.ok() || l_Video.asset == INVALID_ASSET_ID || !finite(l_Video.size.x) || !finite(l_Video.size.y) || l_Video.size.x <= 0.f || l_Video.size.y <= 0.f)
		return false;
	p_Object.payload = l_Video;
	return true;
}

size_t textBodyBytes(const TextData& p_Text)
{
	return TRANSFORM_BYTES + 4 + p_Text.family.size() + 1 + sizeof(float) + 4 * sizeof(float) + 1 + sizeof(float) + 2 * sizeof(float) + 4 + p_Text.text.size();
}

void writeText(Writer& p_Writer, const Object& p_Object, const TextData& p_Text)
{
	p_Writer.u64(p_Object.id);
	p_Writer.u8(static_cast<uint8_t>(ObjectType::Text));
	p_Writer.u32(static_cast<uint32_t>(textBodyBytes(p_Text)));
	writeTransform(p_Writer, p_Object.transform);
	p_Writer.string(p_Text.family);
	p_Writer.u8(p_Text.style);
	p_Writer.f32(p_Text.fontSize);
	p_Writer.f32(p_Text.color.r);
	p_Writer.f32(p_Text.color.g);
	p_Writer.f32(p_Text.color.b);
	p_Writer.f32(p_Text.color.a);
	p_Writer.u8(static_cast<uint8_t>(p_Text.align));
	p_Writer.f32(p_Text.wrapWidth);
	p_Writer.f32(p_Text.size.x);
	p_Writer.f32(p_Text.size.y);
	p_Writer.string(p_Text.text);
}

bool readText(Reader& p_Reader, Object& p_Object)
{
	if (!readTransform(p_Reader, p_Object.transform))
		return false;
	TextData l_Text;
	l_Text.family = p_Reader.string(MAX_NAME_BYTES);
	l_Text.style = p_Reader.u8();
	l_Text.fontSize = p_Reader.f32();
	l_Text.color = Color{ p_Reader.f32(), p_Reader.f32(), p_Reader.f32(), p_Reader.f32() };
	const uint8_t l_Align = p_Reader.u8();
	l_Text.wrapWidth = p_Reader.f32();
	l_Text.size.x = p_Reader.f32();
	l_Text.size.y = p_Reader.f32();
	l_Text.text = p_Reader.string(MAX_TEXT_BYTES);
	if (!p_Reader.ok() || l_Align > static_cast<uint8_t>(TextAlign::Right) || !finite(l_Text.fontSize) || l_Text.fontSize <= 0.f || !finite(l_Text.wrapWidth) || l_Text.wrapWidth < 0.f ||
	    !finite(l_Text.size.x) || !finite(l_Text.size.y) || l_Text.size.x <= 0.f || l_Text.size.y <= 0.f || !finite(l_Text.color.r) || !finite(l_Text.color.g) || !finite(l_Text.color.b) || !finite(l_Text.color.a))
		return false;
	l_Text.style &= TextStyle::Bold | TextStyle::Italic;
	l_Text.align = static_cast<TextAlign>(l_Align);
	p_Object.payload = std::move(l_Text);
	return true;
}

void writeFonts(Writer& p_Writer, const Document& p_Document)
{
	// Only fonts some text still uses are saved
	std::vector<const FontAsset*> l_Used;
	for (const FontAsset& l_Font : p_Document.fontAssets())
	{
		for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
		{
			const TextData* l_Text = l_Object->text();
			if (l_Text != nullptr && l_Text->family == l_Font.family)
			{
				l_Used.push_back(&l_Font);
				break;
			}
		}
	}
	if (l_Used.empty())
		return;
	p_Writer.beginChunk(TAG_FONT);
	p_Writer.u32(static_cast<uint32_t>(l_Used.size()));
	for (const FontAsset* l_Font : l_Used)
	{
		p_Writer.string(l_Font->family);
		p_Writer.u8(l_Font->style);
		p_Writer.u64(l_Font->bytes.size());
		p_Writer.raw(l_Font->bytes.data(), l_Font->bytes.size());
	}
	p_Writer.endChunk();
}

bool parseFonts(const std::span<const uint8_t> p_Payload, std::vector<FontAsset>& p_Fonts)
{
	Reader l_Reader(p_Payload);
	const uint32_t l_Count = l_Reader.u32();
	if (!l_Reader.ok())
		return false;
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		FontAsset l_Font;
		l_Font.family = l_Reader.string(MAX_NAME_BYTES);
		l_Font.style = l_Reader.u8() & (TextStyle::Bold | TextStyle::Italic);
		const uint64_t l_Size = l_Reader.u64();
		if (!l_Reader.ok() || l_Size > MAX_FONT_BYTES || l_Size > l_Reader.remaining())
			return false;
		const std::span<const uint8_t> l_Bytes = l_Reader.take(static_cast<size_t>(l_Size));
		l_Font.bytes.assign(l_Bytes.begin(), l_Bytes.end());
		p_Fonts.push_back(std::move(l_Font));
	}
	return l_Reader.ok();
}

struct AssetRefEntry
{
	AssetId id = INVALID_ASSET_ID;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string name;
	uint64_t size = 0;
	std::string path;
	uint64_t offset = 0;
	int64_t modified = 0;
};

void writeAssets(Writer& p_Writer, const Document& p_Document, const SplitOptions& p_Options, std::vector<BigAssetRef>* p_Big)
{
	// Only assets some object still uses are saved (undone or deleted images do not bloat the file)
	std::vector<AssetId> l_Used;
	std::unordered_set<AssetId> l_Seen;
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
	{
		const AssetId l_Asset = l_Object->image() != nullptr ? l_Object->image()->asset : (l_Object->video() != nullptr ? l_Object->video()->asset : INVALID_ASSET_ID);
		if (l_Asset != INVALID_ASSET_ID && l_Seen.insert(l_Asset).second)
			l_Used.push_back(l_Asset);
	}
	// An asset the document does not hold (still being received in a collaboration) has nothing to write
	std::erase_if(l_Used, [&](const AssetId p_Id)
	{
		const ImageAsset* l_Asset = p_Document.findAsset(p_Id);
		return l_Asset == nullptr || !l_Asset->complete();
	});
	// Big assets are not copied here: the caller writes them in pieces from where they are
	std::vector<AssetRefEntry> l_Refs;
	if (p_Big != nullptr)
	{
		std::erase_if(l_Used, [&](const AssetId p_Id)
		{
			const ImageAsset* l_Asset = p_Document.findAsset(p_Id);
			if (l_Asset == nullptr || l_Asset->size() < p_Options.bigAssetBytes)
				return false;
			std::error_code l_PathError;
			const bool l_Excluded = !p_Options.noReferencesTo.empty() && l_Asset->stream && !l_Asset->stream->path().empty()
				&& (std::filesystem::equivalent(l_Asset->stream->path(), p_Options.noReferencesTo, l_PathError) || l_Asset->stream->path() == p_Options.noReferencesTo);
			if (p_Options.referenceFiles && l_Asset->stream && !l_Excluded)
			{
				// A file of the user's own is only pointed at; a download exists nowhere else, so it cannot be
				if (!l_Asset->stream->temporary() && !l_Asset->stream->path().empty())
				{
					l_Refs.push_back(AssetRefEntry{ .id = p_Id, .width = l_Asset->width, .height = l_Asset->height, .name = l_Asset->name, .size = l_Asset->size(),
						.path = pathToUtf8(l_Asset->stream->path()), .offset = l_Asset->stream->fileOffset(), .modified = l_Asset->stream->modified() });
				}
				return true;
			}
			p_Big->push_back(BigAssetRef{ .id = p_Id, .width = l_Asset->width, .height = l_Asset->height, .name = l_Asset->name, .source = l_Asset->stream,
				.data = l_Asset->stream ? nullptr : l_Asset->bytes.data(), .size = l_Asset->size() });
			return true;
		});
	}
	p_Writer.beginChunk(TAG_ASET);
	p_Writer.u32(static_cast<uint32_t>(l_Used.size()));
	for (const AssetId l_Id : l_Used)
	{
		const ImageAsset* l_Asset = p_Document.findAsset(l_Id);
		const std::vector<uint8_t> l_Loaded = l_Asset != nullptr && l_Asset->stream ? l_Asset->load() : std::vector<uint8_t>{};
		const uint8_t* l_Data = l_Asset != nullptr ? (l_Asset->stream ? l_Loaded.data() : l_Asset->bytes.data()) : nullptr;
		const size_t l_Size = l_Asset != nullptr ? (l_Asset->stream ? l_Loaded.size() : l_Asset->bytes.size()) : 0;
		p_Writer.u64(l_Id);
		p_Writer.u32(l_Asset != nullptr ? l_Asset->width : 0);
		p_Writer.u32(l_Asset != nullptr ? l_Asset->height : 0);
		p_Writer.string(l_Asset != nullptr ? std::string_view(l_Asset->name) : std::string_view());
		p_Writer.u64(l_Size);
		if (l_Size > 0)
			p_Writer.raw(l_Data, l_Size);
	}
	p_Writer.endChunk();
	if (!l_Refs.empty())
	{
		p_Writer.beginChunk(TAG_AREF);
		p_Writer.u32(static_cast<uint32_t>(l_Refs.size()));
		for (const AssetRefEntry& l_Ref : l_Refs)
		{
			p_Writer.u64(l_Ref.id);
			p_Writer.u32(l_Ref.width);
			p_Writer.u32(l_Ref.height);
			p_Writer.string(l_Ref.name);
			p_Writer.u64(l_Ref.size);
			p_Writer.string(l_Ref.path);
			p_Writer.u64(l_Ref.offset);
			p_Writer.u64(static_cast<uint64_t>(l_Ref.modified));
		}
		p_Writer.endChunk();
	}
}

bool parseOneAsset(Reader& p_Reader, std::vector<ImageAsset>& p_Assets, std::unordered_set<AssetId>& p_Seen)
{
	ImageAsset l_Asset;
	l_Asset.id = p_Reader.u64();
	l_Asset.width = p_Reader.u32();
	l_Asset.height = p_Reader.u32();
	l_Asset.name = p_Reader.string(MAX_NAME_BYTES);
	const uint64_t l_Size = p_Reader.u64();
	if (!p_Reader.ok() || l_Size > MAX_ASSET_BYTES || l_Size > p_Reader.remaining() || l_Asset.id == INVALID_ASSET_ID || !p_Seen.insert(l_Asset.id).second)
		return false;
	const std::span<const uint8_t> l_Bytes = p_Reader.take(static_cast<size_t>(l_Size));
	l_Asset.bytes.assign(l_Bytes.begin(), l_Bytes.end());
	p_Assets.push_back(std::move(l_Asset));
	return true;
}

bool parseAssets(const std::span<const uint8_t> p_Payload, std::vector<ImageAsset>& p_Assets, std::unordered_set<AssetId>& p_Seen)
{
	Reader l_Reader(p_Payload);
	const uint32_t l_Count = l_Reader.u32();
	if (!l_Reader.ok())
		return false;
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		if (!parseOneAsset(l_Reader, p_Assets, p_Seen))
			return false;
	}
	return l_Reader.ok();
}

// Files that recovery files point at, opened once however many assets live in them
struct OpenFiles
{
	std::unordered_map<std::string, std::shared_ptr<FileSource>> files;
	std::vector<std::string> missing;
};

bool parseRefs(const std::span<const uint8_t> p_Payload, std::vector<ImageAsset>& p_Assets, std::unordered_set<AssetId>& p_Seen, OpenFiles& p_Open)
{
	Reader l_Reader(p_Payload);
	const uint32_t l_Count = l_Reader.u32();
	if (!l_Reader.ok())
		return false;
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		ImageAsset l_Asset;
		l_Asset.id = l_Reader.u64();
		l_Asset.width = l_Reader.u32();
		l_Asset.height = l_Reader.u32();
		l_Asset.name = l_Reader.string(MAX_NAME_BYTES);
		const uint64_t l_Size = l_Reader.u64();
		const std::string l_Path = l_Reader.string(MAX_PATH_BYTES);
		const uint64_t l_Offset = l_Reader.u64();
		const int64_t l_Modified = static_cast<int64_t>(l_Reader.u64());
		if (!l_Reader.ok() || l_Size > MAX_ASSET_BYTES || l_Offset > MAX_ASSET_BYTES * 64 || l_Asset.id == INVALID_ASSET_ID || !p_Seen.insert(l_Asset.id).second)
			return false;

		std::shared_ptr<FileSource>& l_File = p_Open.files[l_Path];
		if (!l_File)
		{
			std::string l_Error;
			l_File = FileSource::open(pathFromUtf8(l_Path), l_Error);
		}
		// Only the very same file will do: if it was edited since, what it holds is not what the board showed
		if (!l_File || l_File->modified() != l_Modified || l_File->size() < l_Offset || l_File->size() - l_Offset < l_Size)
		{
			p_Open.missing.push_back(l_Asset.name.empty() ? l_Path : l_Asset.name);
			continue; // the objects that used it are dropped like those of an unknown kind
		}
		l_Asset.stream = std::make_shared<RangeSource>(l_File, l_Offset, l_Size);
		p_Assets.push_back(std::move(l_Asset));
	}
	return l_Reader.ok();
}

struct ParsedMeta
{
	uint64_t nextId = 0;
	uint64_t objectCount = 0;
	std::string sourcePath;
};

LoadResult failure(std::string p_Message)
{
	return LoadResult{ .ok = false, .error = std::move(p_Message) };
}

bool parseObjects(const std::span<const uint8_t> p_Payload, std::vector<std::unique_ptr<Object>>& p_Objects, uint32_t& p_Skipped)
{
	Reader l_Reader(p_Payload);
	const uint32_t l_Count = l_Reader.u32();
	if (!l_Reader.ok() || static_cast<uint64_t>(l_Count) * OBJECT_HEADER_BYTES > l_Reader.remaining())
		return false;
	p_Objects.reserve(l_Count);

	std::unordered_set<ObjectId> l_Seen;
	l_Seen.reserve(l_Count);
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		const ObjectId l_Id = l_Reader.u64();
		const uint8_t l_Type = l_Reader.u8();
		const uint32_t l_BodySize = l_Reader.u32();
		const std::span<const uint8_t> l_Body = l_Reader.take(l_BodySize);
		if (!l_Reader.ok() || l_Id == INVALID_OBJECT_ID || !l_Seen.insert(l_Id).second)
			return false;

		if (l_Type != static_cast<uint8_t>(ObjectType::Stroke) && l_Type != static_cast<uint8_t>(ObjectType::Image) && l_Type != static_cast<uint8_t>(ObjectType::Text) && l_Type != static_cast<uint8_t>(ObjectType::Video))
		{
			++p_Skipped;
			continue;
		}
		auto l_Object = std::make_unique<Object>();
		l_Object->id = l_Id;
		Reader l_BodyReader(l_Body);
		bool l_Skipped = false;
		if (l_Type == static_cast<uint8_t>(ObjectType::Image))
		{
			if (!readImage(l_BodyReader, *l_Object))
				return false;
		}
		else if (l_Type == static_cast<uint8_t>(ObjectType::Text))
		{
			if (!readText(l_BodyReader, *l_Object))
				return false;
		}
		else if (l_Type == static_cast<uint8_t>(ObjectType::Video))
		{
			if (!readVideo(l_BodyReader, *l_Object))
				return false;
		}
		else if (!readStroke(l_BodyReader, *l_Object, l_Skipped))
		{
			return false;
		}
		if (l_Skipped)
			++p_Skipped;
		else
			p_Objects.push_back(std::move(l_Object));
	}
	return true;
}
} // namespace

uint32_t crc32(const std::span<const uint8_t> p_Bytes)
{
	return ~crcUpdate(0xFFFFFFFFu, p_Bytes.data(), p_Bytes.size());
}

std::vector<uint8_t> encodeObject(const Object& p_Object)
{
	Writer l_Writer;
	if (const StrokeData* l_Stroke = p_Object.stroke())
		writeStroke(l_Writer, p_Object, *l_Stroke);
	else if (const ImageData* l_Image = p_Object.image())
		writeImage(l_Writer, p_Object, *l_Image);
	else if (const VideoData* l_Video = p_Object.video())
		writeVideo(l_Writer, p_Object, *l_Video);
	else if (const TextData* l_Text = p_Object.text())
		writeText(l_Writer, p_Object, *l_Text);
	return l_Writer.take();
}

ObjectDecode decodeObject(const std::span<const uint8_t> p_Bytes, std::unique_ptr<Object>& p_Object, size_t* p_Consumed)
{
	p_Object.reset();
	Reader l_Reader(p_Bytes);
	const ObjectId l_Id = l_Reader.u64();
	const uint8_t l_Type = l_Reader.u8();
	const uint32_t l_BodySize = l_Reader.u32();
	const std::span<const uint8_t> l_Body = l_Reader.take(l_BodySize);
	if (!l_Reader.ok() || l_Id == INVALID_OBJECT_ID)
		return ObjectDecode::Corrupt;
	if (p_Consumed != nullptr)
		*p_Consumed = p_Bytes.size() - l_Reader.remaining();

	auto l_Object = std::make_unique<Object>();
	l_Object->id = l_Id;
	Reader l_BodyReader(l_Body);
	bool l_Skipped = false;
	switch (l_Type)
	{
	case static_cast<uint8_t>(ObjectType::Stroke):
		if (!readStroke(l_BodyReader, *l_Object, l_Skipped))
			return ObjectDecode::Corrupt;
		break;
	case static_cast<uint8_t>(ObjectType::Image):
		if (!readImage(l_BodyReader, *l_Object))
			return ObjectDecode::Corrupt;
		break;
	case static_cast<uint8_t>(ObjectType::Text):
		if (!readText(l_BodyReader, *l_Object))
			return ObjectDecode::Corrupt;
		break;
	case static_cast<uint8_t>(ObjectType::Video):
		if (!readVideo(l_BodyReader, *l_Object))
			return ObjectDecode::Corrupt;
		break;
	default:
		return ObjectDecode::Unknown;
	}
	if (l_Skipped)
		return ObjectDecode::Unknown;
	p_Object = std::move(l_Object);
	return ObjectDecode::Ok;
}

namespace
{
// Everything but the closing chunk; big assets (when p_Big is set) are left out and listed there
// A picture or video whose file the document does not hold (still being received in a collaboration)
bool lacksAsset(const Document& p_Document, const Object& p_Object)
{
	const AssetId l_Id = p_Object.image() != nullptr ? p_Object.image()->asset : (p_Object.video() != nullptr ? p_Object.video()->asset : INVALID_ASSET_ID);
	if (l_Id == INVALID_ASSET_ID)
		return false;
	const ImageAsset* l_Asset = p_Document.findAsset(l_Id);
	return l_Asset == nullptr || !l_Asset->complete();
}

// p_IncludeUnresolved: also write pictures and videos whose file is missing (a collaboration snapshot; the receiver gets
// the file separately). A saved file leaves them out, as it could not show them.
std::vector<uint8_t> serializeBody(const Document& p_Document, const BoardMeta& p_Meta, const SplitOptions& p_Options, std::vector<BigAssetRef>* p_Big, std::vector<size_t>* p_DeferredCrcs = nullptr, const bool p_IncludeUnresolved = false)
{
	std::vector<const Object*> l_Objects;
	l_Objects.reserve(p_Document.size());
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
	{
		if (p_IncludeUnresolved || !lacksAsset(p_Document, *l_Object))
			l_Objects.push_back(l_Object.get());
	}
	size_t l_Estimate = 256 + p_Meta.sourcePath.size();
	for (const Object* l_Object : l_Objects)
	{
		if (const StrokeData* l_Stroke = l_Object->stroke())
			l_Estimate += OBJECT_HEADER_BYTES + strokeBodyBytes(*l_Stroke);
		else if (l_Object->image() != nullptr)
			l_Estimate += OBJECT_HEADER_BYTES + IMAGE_BODY_BYTES;
		else if (l_Object->video() != nullptr)
			l_Estimate += OBJECT_HEADER_BYTES + videoBodyBytes(*l_Object->video());
		else if (const TextData* l_Text = l_Object->text())
			l_Estimate += OBJECT_HEADER_BYTES + textBodyBytes(*l_Text);
	}
	for (const auto& [l_Id, l_Asset] : p_Document.assets())
		l_Estimate += (p_Big != nullptr && l_Asset.size() >= p_Options.bigAssetBytes ? 0 : l_Asset.size()) + 64;

	Writer l_Writer;
	l_Writer.deferChecksums(p_DeferredCrcs);
	l_Writer.reserve(l_Estimate);
	l_Writer.raw(MAGIC.data(), MAGIC.size());
	l_Writer.u32(BOARD_FORMAT_VERSION);

	l_Writer.beginChunk(TAG_META);
	l_Writer.u64(p_Document.peekNextId());
	l_Writer.u64(l_Objects.size());
	l_Writer.string(p_Meta.sourcePath);
	l_Writer.endChunk();

	l_Writer.beginChunk(TAG_VIEW);
	l_Writer.f64(p_Meta.viewCenter.x);
	l_Writer.f64(p_Meta.viewCenter.y);
	l_Writer.f64(p_Meta.viewZoom);
	l_Writer.endChunk();

	writeAssets(l_Writer, p_Document, p_Options, p_Big);
	writeFonts(l_Writer, p_Document);

	l_Writer.beginChunk(TAG_OBJS);
	l_Writer.u32(static_cast<uint32_t>(l_Objects.size()));
	for (const Object* l_Object : l_Objects)
	{
		if (const StrokeData* l_Stroke = l_Object->stroke())
			writeStroke(l_Writer, *l_Object, *l_Stroke);
		else if (const ImageData* l_Image = l_Object->image())
			writeImage(l_Writer, *l_Object, *l_Image);
		else if (const VideoData* l_Video = l_Object->video())
			writeVideo(l_Writer, *l_Object, *l_Video);
		else if (const TextData* l_Text = l_Object->text())
			writeText(l_Writer, *l_Object, *l_Text);
	}
	l_Writer.endChunk();

	uint32_t l_LockedCount = 0;
	for (const Object* l_Object : l_Objects)
		l_LockedCount += l_Object->locked ? 1u : 0u;
	if (l_LockedCount > 0)
	{
		l_Writer.beginChunk(TAG_LOCK);
		l_Writer.u32(l_LockedCount);
		for (const Object* l_Object : l_Objects)
		{
			if (l_Object->locked)
				l_Writer.u64(l_Object->id);
		}
		l_Writer.endChunk();
	}
	return l_Writer.take();
}

std::vector<uint8_t> closingChunk()
{
	Writer l_Writer;
	l_Writer.beginChunk(TAG_END);
	l_Writer.endChunk();
	return l_Writer.take();
}
} // namespace

std::vector<uint8_t> serializeBoard(const Document& p_Document, const BoardMeta& p_Meta)
{
	std::vector<uint8_t> l_Bytes = serializeBody(p_Document, p_Meta, SplitOptions{}, nullptr);
	const std::vector<uint8_t> l_End = closingChunk();
	l_Bytes.insert(l_Bytes.end(), l_End.begin(), l_End.end());
	return l_Bytes;
}

std::vector<uint8_t> serializeBoardShell(const Document& p_Document, const BoardMeta& p_Meta, const size_t p_InlineAssetBytes)
{
	std::vector<BigAssetRef> l_Left;
	std::vector<uint8_t> l_Bytes = serializeBody(p_Document, p_Meta, SplitOptions{ .bigAssetBytes = p_InlineAssetBytes }, &l_Left, nullptr, true);
	const std::vector<uint8_t> l_End = closingChunk();
	l_Bytes.insert(l_Bytes.end(), l_End.begin(), l_End.end());
	return l_Bytes;
}

SplitBoard serializeBoardSplit(const Document& p_Document, const BoardMeta& p_Meta, const SplitOptions& p_Options)
{
	SplitBoard l_Board;
	l_Board.head = serializeBody(p_Document, p_Meta, p_Options, &l_Board.big, p_Options.deferChecksums ? &l_Board.pendingChecksums : nullptr);
	l_Board.tail = closingChunk();
	for (const BigAssetRef& l_Ref : l_Board.big)
		l_Board.bigBytes += l_Ref.size;
	return l_Board;
}

void finishChecksums(SplitBoard& p_Board)
{
	for (const size_t l_Start : p_Board.pendingChecksums)
	{
		const std::span<const uint8_t> l_Payload = std::span<const uint8_t>(p_Board.head).subspan(l_Start + CHUNK_HEADER_BYTES);
		uint64_t l_Length = 0;
		std::memcpy(&l_Length, p_Board.head.data() + l_Start + 4, sizeof(l_Length));
		const uint32_t l_Crc = crc32(l_Payload.first(static_cast<size_t>(l_Length)));
		std::memcpy(p_Board.head.data() + l_Start + 12, &l_Crc, sizeof(l_Crc));
	}
	p_Board.pendingChecksums.clear();
}

IoResult writeSplitBoardAtomic(const std::filesystem::path& p_Path, const SplitBoard& p_Board, std::vector<PlacedAsset>* p_Placed, WriteProgress* p_Progress)
{
	std::error_code l_Error;
	if (p_Path.has_parent_path())
		std::filesystem::create_directories(p_Path.parent_path(), l_Error);
	std::filesystem::path l_Temp = p_Path;
	l_Temp += ".tmp";
	if (p_Placed != nullptr)
		p_Placed->clear();
	std::string l_Problem;
	{
		std::ofstream l_File(l_Temp, std::ios::binary | std::ios::trunc);
		if (!l_File)
			return IoResult::failure("Cannot create " + pathToUtf8(l_Temp));
		const auto l_Write = [&](const void* p_Data, const size_t p_Size)
		{
			l_File.write(static_cast<const char*>(p_Data), static_cast<std::streamsize>(p_Size));
			return static_cast<bool>(l_File);
		};
		bool l_Good = l_Write(p_Board.head.data(), p_Board.head.size());
		for (const BigAssetRef& l_Ref : p_Board.big)
		{
			if (!l_Good)
				break;
			// One chunk per big asset: header fields first, then the file itself (the checksum covers both)
			Writer l_Fields;
			l_Fields.u64(l_Ref.id);
			l_Fields.u32(l_Ref.width);
			l_Fields.u32(l_Ref.height);
			l_Fields.string(l_Ref.name);
			l_Fields.u64(l_Ref.size);
			const std::vector<uint8_t> l_Prefix = l_Fields.take();
			const uint64_t l_Length = l_Prefix.size() + l_Ref.size;
			const uint32_t l_Tag = TAG_ABIG;
			const uint32_t l_NoCrc = 0;
			const std::streamoff l_HeaderAt = static_cast<std::streamoff>(l_File.tellp());
			l_Good = l_Write(&l_Tag, sizeof(l_Tag)) && l_Write(&l_Length, sizeof(l_Length)) && l_Write(&l_NoCrc, sizeof(l_NoCrc)) && l_Write(l_Prefix.data(), l_Prefix.size());
			if (!l_Good)
				break;
			const uint64_t l_PayloadAt = static_cast<uint64_t>(l_File.tellp());
			uint32_t l_Crc = crcUpdate(0xFFFFFFFFu, l_Prefix.data(), l_Prefix.size());
			bool l_Cancelled = false;
			const auto l_Piece = [&](const uint8_t* p_Data, const size_t p_Size)
			{
				if (p_Progress != nullptr && p_Progress->cancel.load())
				{
					l_Cancelled = true;
					return false;
				}
				l_Crc = crcUpdate(l_Crc, p_Data, p_Size);
				if (!l_Write(p_Data, p_Size))
					return false;
				if (p_Progress != nullptr)
					p_Progress->written.fetch_add(p_Size);
				return true;
			};
			if (l_Ref.source)
			{
				l_Good = streamBytes(*l_Ref.source, 0, l_Ref.size, l_Piece);
			}
			else
			{
				constexpr uint64_t BLOCK = uint64_t{ 1 } << 20;
				for (uint64_t l_At = 0; l_Good && l_At < l_Ref.size; l_At += BLOCK)
					l_Good = l_Piece(l_Ref.data + l_At, static_cast<size_t>(std::min<uint64_t>(BLOCK, l_Ref.size - l_At)));
			}
			if (!l_Good)
			{
				l_Problem = l_Cancelled ? "The save was cancelled."
					: (l_File ? "A picture or video (" + l_Ref.name + ") could not be read: was its file moved, or its drive removed?" : std::string());
				break;
			}
			// Now that the checksum is known, go back and fill it in
			l_Crc = ~l_Crc;
			l_File.seekp(l_HeaderAt + static_cast<std::streamoff>(sizeof(l_Tag) + sizeof(l_Length)));
			l_Good = l_Write(&l_Crc, sizeof(l_Crc));
			l_File.seekp(0, std::ios::end);
			if (!l_Good)
				break;
			if (p_Placed != nullptr)
				p_Placed->push_back(PlacedAsset{ .id = l_Ref.id, .offset = l_PayloadAt, .size = l_Ref.size });
		}
		if (l_Good)
			l_Good = l_Write(p_Board.tail.data(), p_Board.tail.size());
		l_File.flush();
		if (!l_Good || !l_File)
		{
			l_File.close();
			std::filesystem::remove(l_Temp, l_Error);
			if (p_Placed != nullptr)
				p_Placed->clear();
			return IoResult::failure(l_Problem.empty() ? "Cannot write " + pathToUtf8(p_Path) + " (disk full or no permission?)" : l_Problem);
		}
	}
	return replaceFile(l_Temp, p_Path);
}

// ------------------------------------------------------------------------------------------------ loading

namespace
{
struct LoadState
{
	std::optional<ParsedMeta> meta;
	std::optional<std::array<double, 3>> view;
	std::vector<std::unique_ptr<Object>> objects;
	std::vector<ImageAsset> assets;
	std::vector<FontAsset> fonts;
	std::unordered_set<ObjectId> locked;
	std::unordered_set<AssetId> seenAssets;
	OpenFiles open;
	bool haveAssets = false;
	bool haveObjects = false;
	bool haveEnd = false;
	uint32_t skipped = 0;
};

// Checks the first bytes of a file: empty when fine, otherwise what is wrong
std::string checkFileHeader(Reader& p_File)
{
	const std::span<const uint8_t> l_Magic = p_File.take(MAGIC.size());
	if (!p_File.ok() || std::memcmp(l_Magic.data(), MAGIC.data(), MAGIC.size()) != 0)
		return "This is not a Inkwell file.";
	const uint32_t l_Version = p_File.u32();
	if (!p_File.ok() || l_Version == 0)
		return "The file header is damaged.";
	if (l_Version > BOARD_FORMAT_VERSION)
		return "This file was made by a newer version of Inkwell.";
	return {};
}

// One chunk that is in memory. Empty when fine, otherwise what is wrong.
std::string consumeChunk(LoadState& p_State, const uint32_t p_Tag, const std::span<const uint8_t> p_Payload)
{
	Reader l_Reader(p_Payload);
	switch (p_Tag)
	{
	case TAG_META:
	{
		ParsedMeta l_Parsed;
		l_Parsed.nextId = l_Reader.u64();
		l_Parsed.objectCount = l_Reader.u64();
		l_Parsed.sourcePath = l_Reader.string(MAX_PATH_BYTES);
		if (!l_Reader.ok())
			return "The file is damaged (bad header chunk).";
		p_State.meta = std::move(l_Parsed);
		break;
	}
	case TAG_VIEW:
	{
		const std::array<double, 3> l_Values{ l_Reader.f64(), l_Reader.f64(), l_Reader.f64() };
		if (!l_Reader.ok() || !finite(l_Values[0]) || !finite(l_Values[1]) || !finite(l_Values[2]) || l_Values[2] <= 0.0)
			return "The file is damaged (bad view chunk).";
		p_State.view = l_Values;
		break;
	}
	case TAG_OBJS:
		if (p_State.haveObjects || !parseObjects(p_Payload, p_State.objects, p_State.skipped))
			return "The file is damaged (bad object data).";
		p_State.haveObjects = true;
		break;
	case TAG_ASET:
		if (p_State.haveAssets || !parseAssets(p_Payload, p_State.assets, p_State.seenAssets))
			return "The file is damaged (bad image data).";
		p_State.haveAssets = true;
		break;
	case TAG_ABIG:
		if (!parseOneAsset(l_Reader, p_State.assets, p_State.seenAssets))
			return "The file is damaged (bad image data).";
		break;
	case TAG_AREF:
		if (!parseRefs(p_Payload, p_State.assets, p_State.seenAssets, p_State.open))
			return "The file is damaged (bad file references).";
		break;
	case TAG_FONT:
		if (!p_State.fonts.empty() || !parseFonts(p_Payload, p_State.fonts))
			return "The file is damaged (bad font data).";
		break;
	case TAG_LOCK:
	{
		const uint32_t l_Count = l_Reader.u32();
		if (!l_Reader.ok() || static_cast<uint64_t>(l_Count) * sizeof(uint64_t) > l_Reader.remaining())
			return "The file is damaged (bad lock data).";
		for (uint32_t i = 0; i < l_Count; ++i)
			p_State.locked.insert(l_Reader.u64());
		break;
	}
	case TAG_END:
		p_State.haveEnd = true;
		break;
	default:
		break; // a chunk from a newer version
	}
	return {};
}

// Everything was read: checks it hangs together and only then touches the document
LoadResult commitLoad(LoadState& p_State, Document& p_Document, BoardMeta& p_Meta, const bool p_AllowMissingAssets)
{
	if (!p_State.haveEnd)
		return failure("The file is incomplete (truncated).");
	if (!p_State.meta || !p_State.haveObjects)
		return failure("The file is damaged (missing data).");
	// Images whose picture is missing cannot be shown: drop them like objects of an unknown kind
	if (!p_AllowMissingAssets)
	{
		std::unordered_set<AssetId> l_Known;
		for (const ImageAsset& l_Asset : p_State.assets)
			l_Known.insert(l_Asset.id);
		p_State.skipped += static_cast<uint32_t>(std::erase_if(p_State.objects, [&](const std::unique_ptr<Object>& p_Object)
		{
			const ImageData* l_Image = p_Object->image();
			const VideoData* l_Video = p_Object->video();
			return (l_Image != nullptr && !l_Known.contains(l_Image->asset)) || (l_Video != nullptr && !l_Known.contains(l_Video->asset));
		}));
	}
	if (p_State.meta->objectCount != p_State.objects.size() + p_State.skipped)
		return failure("The file is damaged (object count mismatch).");

	p_Document.clear();
	if (p_State.meta->nextId > 0)
		p_Document.reserveId(p_State.meta->nextId - 1);
	for (ImageAsset& l_Asset : p_State.assets)
		p_Document.insertAsset(std::move(l_Asset));
	for (FontAsset& l_Font : p_State.fonts)
		p_Document.setFontAsset(std::move(l_Font));
	for (std::unique_ptr<Object>& l_Object : p_State.objects)
	{
		l_Object->locked = p_State.locked.contains(l_Object->id);
		p_Document.insert(std::move(l_Object));
	}

	p_Meta = BoardMeta{};
	p_Meta.sourcePath = std::move(p_State.meta->sourcePath);
	if (p_State.view)
	{
		p_Meta.viewCenter = DVec2{ (*p_State.view)[0], (*p_State.view)[1] };
		p_Meta.viewZoom = (*p_State.view)[2];
	}
	LoadResult l_Result{ .ok = true, .skippedObjects = p_State.skipped };
	l_Result.missingFiles = std::move(p_State.open.missing);
	return l_Result;
}

// Reads exact pieces of a file at a moving position
class FileCursor
{
public:
	FileCursor(const ByteSource& p_File, const uint64_t p_Start, const uint64_t p_End) : m_File(p_File), m_At(p_Start), m_End(p_End) {}

	[[nodiscard]] uint64_t position() const { return m_At; }
	[[nodiscard]] uint64_t remaining() const { return m_End - m_At; }
	bool get(void* p_Target, const size_t p_Size)
	{
		if (p_Size > remaining() || m_File.read(m_At, p_Target, p_Size) != p_Size)
			return false;
		m_At += p_Size;
		return true;
	}
	template <typename T>
	bool get(T& p_Value)
	{
		return get(&p_Value, sizeof(T));
	}
	bool skip(const uint64_t p_Size)
	{
		if (p_Size > remaining())
			return false;
		m_At += p_Size;
		return true;
	}

private:
	const ByteSource& m_File;
	uint64_t m_At;
	uint64_t m_End;
};

// An asset chunk too big to read at once (an old file with videos inside the one ASET chunk): the big files stay where
// they are. Empty when fine.
std::string streamAssetList(const std::shared_ptr<const ByteSource>& p_File, const uint64_t p_Start, const uint64_t p_End, LoadState& p_State)
{
	static const char* const DAMAGED = "The file is damaged (bad image data).";
	FileCursor l_Cursor(*p_File, p_Start, p_End);
	uint32_t l_Count = 0;
	if (!l_Cursor.get(l_Count))
		return DAMAGED;
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		ImageAsset l_Asset;
		uint32_t l_NameSize = 0;
		uint64_t l_Size = 0;
		if (!l_Cursor.get(l_Asset.id) || !l_Cursor.get(l_Asset.width) || !l_Cursor.get(l_Asset.height) || !l_Cursor.get(l_NameSize) || l_NameSize > MAX_NAME_BYTES)
			return DAMAGED;
		l_Asset.name.resize(l_NameSize);
		if (!l_Cursor.get(l_Asset.name.data(), l_NameSize) || !l_Cursor.get(l_Size))
			return DAMAGED;
		if (l_Size > MAX_ASSET_BYTES || l_Size > l_Cursor.remaining() || l_Asset.id == INVALID_ASSET_ID || !p_State.seenAssets.insert(l_Asset.id).second)
			return DAMAGED;
		if (l_Size >= LARGE_ASSET_BYTES)
		{
			l_Asset.stream = std::make_shared<RangeSource>(p_File, l_Cursor.position(), l_Size);
			l_Cursor.skip(l_Size);
		}
		else
		{
			l_Asset.bytes.resize(static_cast<size_t>(l_Size));
			if (!l_Cursor.get(l_Asset.bytes.data(), l_Asset.bytes.size()))
				return DAMAGED;
		}
		p_State.assets.push_back(std::move(l_Asset));
	}
	return {};
}

// One big asset chunk whose file stays where it is. Empty when fine.
std::string streamBigAsset(const std::shared_ptr<const ByteSource>& p_File, const uint64_t p_Start, const uint64_t p_Length, LoadState& p_State)
{
	static const char* const DAMAGED = "The file is damaged (bad image data).";
	std::vector<uint8_t> l_Prefix(static_cast<size_t>(std::min<uint64_t>(p_Length, MAX_ASSET_PREFIX_BYTES)));
	if (p_File->read(p_Start, l_Prefix.data(), l_Prefix.size()) != l_Prefix.size())
		return DAMAGED;
	Reader l_Reader(l_Prefix);
	ImageAsset l_Asset;
	l_Asset.id = l_Reader.u64();
	l_Asset.width = l_Reader.u32();
	l_Asset.height = l_Reader.u32();
	l_Asset.name = l_Reader.string(MAX_NAME_BYTES);
	const uint64_t l_Size = l_Reader.u64();
	if (!l_Reader.ok())
		return DAMAGED;
	const uint64_t l_PrefixBytes = l_Prefix.size() - l_Reader.remaining();
	if (l_Size > MAX_ASSET_BYTES || l_PrefixBytes + l_Size != p_Length || l_Asset.id == INVALID_ASSET_ID || !p_State.seenAssets.insert(l_Asset.id).second)
		return DAMAGED;
	l_Asset.stream = std::make_shared<RangeSource>(p_File, p_Start + l_PrefixBytes, l_Size);
	p_State.assets.push_back(std::move(l_Asset));
	return {};
}
} // namespace

LoadResult deserializeBoard(const std::span<const uint8_t> p_Bytes, Document& p_Document, BoardMeta& p_Meta, const bool p_AllowMissingAssets)
{
	Reader l_File(p_Bytes);
	if (std::string l_Problem = checkFileHeader(l_File); !l_Problem.empty())
		return failure(std::move(l_Problem));

	LoadState l_State;
	while (l_File.remaining() > 0 && !l_State.haveEnd)
	{
		const uint32_t l_Tag = l_File.u32();
		const uint64_t l_Length = l_File.u64();
		const uint32_t l_ExpectedCrc = l_File.u32();
		if (!l_File.ok() || l_Length > l_File.remaining())
			return failure("The file is incomplete (truncated).");
		const std::span<const uint8_t> l_Payload = l_File.take(static_cast<size_t>(l_Length));
		if (crc32(l_Payload) != l_ExpectedCrc)
			return failure("The file is damaged (checksum mismatch).");
		if (std::string l_Problem = consumeChunk(l_State, l_Tag, l_Payload); !l_Problem.empty())
			return failure(std::move(l_Problem));
	}
	return commitLoad(l_State, p_Document, p_Meta, p_AllowMissingAssets);
}

LoadResult loadBoardFile(const std::shared_ptr<const ByteSource>& p_File, Document& p_Document, BoardMeta& p_Meta)
{
	const uint64_t l_Total = p_File->size();
	std::array<uint8_t, 12> l_Head{};
	if (l_Total < l_Head.size() || p_File->read(0, l_Head.data(), l_Head.size()) != l_Head.size())
		return failure("This is not a Inkwell file.");
	Reader l_HeadReader(l_Head);
	if (std::string l_Problem = checkFileHeader(l_HeadReader); !l_Problem.empty())
		return failure(std::move(l_Problem));

	LoadState l_State;
	uint64_t l_At = l_Head.size();
	while (l_At < l_Total && !l_State.haveEnd)
	{
		std::array<uint8_t, CHUNK_HEADER_BYTES> l_ChunkHead{};
		if (p_File->read(l_At, l_ChunkHead.data(), l_ChunkHead.size()) != l_ChunkHead.size())
			return failure("The file is incomplete (truncated).");
		uint32_t l_Tag = 0;
		uint64_t l_Length = 0;
		uint32_t l_ExpectedCrc = 0;
		std::memcpy(&l_Tag, l_ChunkHead.data(), 4);
		std::memcpy(&l_Length, l_ChunkHead.data() + 4, 8);
		std::memcpy(&l_ExpectedCrc, l_ChunkHead.data() + 12, 4);
		l_At += CHUNK_HEADER_BYTES;
		if (l_Length > l_Total - l_At)
			return failure("The file is incomplete (truncated).");

		// Big files are not read: they are used from where they are (and not checksummed, which would read all of them)
		if (l_Tag == TAG_ABIG && l_Length >= LARGE_ASSET_BYTES)
		{
			if (std::string l_Problem = streamBigAsset(p_File, l_At, l_Length, l_State); !l_Problem.empty())
				return failure(std::move(l_Problem));
		}
		else if (l_Tag == TAG_ASET && l_Length > MAX_RESIDENT_CHUNK / 32 && !l_State.haveAssets)
		{
			if (std::string l_Problem = streamAssetList(p_File, l_At, l_At + l_Length, l_State); !l_Problem.empty())
				return failure(std::move(l_Problem));
			l_State.haveAssets = true;
		}
		else
		{
			if (l_Length > MAX_RESIDENT_CHUNK)
				return failure("The file is damaged (a chunk is too large).");
			std::vector<uint8_t> l_Payload;
			try
			{
				l_Payload.resize(static_cast<size_t>(l_Length));
			}
			catch (const std::bad_alloc&)
			{
				return failure("There is not enough memory to open this file.");
			}
			if (p_File->read(l_At, l_Payload.data(), l_Payload.size()) != l_Payload.size())
				return failure("The file is incomplete (truncated).");
			if (crc32(l_Payload) != l_ExpectedCrc)
				return failure("The file is damaged (checksum mismatch).");
			if (std::string l_Problem = consumeChunk(l_State, l_Tag, l_Payload); !l_Problem.empty())
				return failure(std::move(l_Problem));
		}
		l_At += l_Length;
	}
	return commitLoad(l_State, p_Document, p_Meta, false);
}
} // namespace wb
