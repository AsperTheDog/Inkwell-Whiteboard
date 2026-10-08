module;
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.io.serializer;

import wb.math;
import wb.doc.document;
import wb.doc.object;

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
constexpr uint32_t TAG_END = tag("END ");

enum class ObjectType : uint8_t
{
	Stroke = 1,
	Image = 2,
	Text = 3,
	Video = 4,
};

constexpr size_t IMAGE_BODY_BYTES = 6 * sizeof(double) + sizeof(uint64_t) + 2 * sizeof(float) + 1 + sizeof(uint32_t);
constexpr size_t VIDEO_BODY_BYTES = 6 * sizeof(double) + sizeof(uint64_t) + 2 * sizeof(float) + 2;
constexpr size_t MAX_NAME_BYTES = 1024;
constexpr size_t MAX_TEXT_BYTES = 64u << 20;
constexpr uint64_t MAX_FONT_BYTES = 1ull << 28;
constexpr uint64_t MAX_ASSET_BYTES = 1ull << 31;

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
		const uint32_t l_Crc = crc32(std::span<const uint8_t>(m_Bytes).subspan(l_PayloadStart));
		std::memcpy(m_Bytes.data() + m_ChunkStart + 4, &l_Length, sizeof(l_Length));
		std::memcpy(m_Bytes.data() + m_ChunkStart + 12, &l_Crc, sizeof(l_Crc));
	}

	void reserve(const size_t p_Bytes) { m_Bytes.reserve(p_Bytes); }
	std::vector<uint8_t> take() { return std::move(m_Bytes); }

private:
	std::vector<uint8_t> m_Bytes;
	size_t m_ChunkStart = 0;
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
	for (const StrokePoint& l_Point : p_Stroke.points)
	{
		p_Writer.f32(l_Point.position.x);
		p_Writer.f32(l_Point.position.y);
		p_Writer.f32(l_Point.radius);
	}
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
	if (l_Brush > static_cast<uint8_t>(BrushKind::Pen))
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
	p_Writer.u32(static_cast<uint32_t>(VIDEO_BODY_BYTES));
	writeTransform(p_Writer, p_Object.transform);
	p_Writer.u64(p_Video.asset);
	p_Writer.f32(p_Video.size.x);
	p_Writer.f32(p_Video.size.y);
	p_Writer.u8(p_Video.loop ? 1 : 0);
	p_Writer.u8(p_Video.muted ? 1 : 0);
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

void writeAssets(Writer& p_Writer, const Document& p_Document)
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
	p_Writer.beginChunk(TAG_ASET);
	p_Writer.u32(static_cast<uint32_t>(l_Used.size()));
	for (const AssetId l_Id : l_Used)
	{
		const ImageAsset* l_Asset = p_Document.findAsset(l_Id);
		p_Writer.u64(l_Id);
		p_Writer.u32(l_Asset != nullptr ? l_Asset->width : 0);
		p_Writer.u32(l_Asset != nullptr ? l_Asset->height : 0);
		p_Writer.string(l_Asset != nullptr ? std::string_view(l_Asset->name) : std::string_view());
		p_Writer.u64(l_Asset != nullptr ? l_Asset->bytes.size() : 0);
		if (l_Asset != nullptr)
			p_Writer.raw(l_Asset->bytes.data(), l_Asset->bytes.size());
	}
	p_Writer.endChunk();
}

bool parseAssets(const std::span<const uint8_t> p_Payload, std::vector<ImageAsset>& p_Assets)
{
	Reader l_Reader(p_Payload);
	const uint32_t l_Count = l_Reader.u32();
	if (!l_Reader.ok())
		return false;
	std::unordered_set<AssetId> l_Seen;
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		ImageAsset l_Asset;
		l_Asset.id = l_Reader.u64();
		l_Asset.width = l_Reader.u32();
		l_Asset.height = l_Reader.u32();
		l_Asset.name = l_Reader.string(MAX_NAME_BYTES);
		const uint64_t l_Size = l_Reader.u64();
		if (!l_Reader.ok() || l_Size > MAX_ASSET_BYTES || l_Size > l_Reader.remaining() || l_Asset.id == INVALID_ASSET_ID || !l_Seen.insert(l_Asset.id).second)
			return false;
		const std::span<const uint8_t> l_Bytes = l_Reader.take(static_cast<size_t>(l_Size));
		l_Asset.bytes.assign(l_Bytes.begin(), l_Bytes.end());
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
	uint32_t l_Crc = 0xFFFFFFFFu;
	for (const uint8_t l_Byte : p_Bytes)
		l_Crc = CRC_TABLE[(l_Crc ^ l_Byte) & 0xFFu] ^ (l_Crc >> 8);
	return ~l_Crc;
}

std::vector<uint8_t> serializeBoard(const Document& p_Document, const BoardMeta& p_Meta)
{
	size_t l_Estimate = 256 + p_Meta.sourcePath.size();
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
	{
		if (const StrokeData* l_Stroke = l_Object->stroke())
			l_Estimate += OBJECT_HEADER_BYTES + strokeBodyBytes(*l_Stroke);
		else if (l_Object->image() != nullptr)
			l_Estimate += OBJECT_HEADER_BYTES + IMAGE_BODY_BYTES;
		else if (l_Object->video() != nullptr)
			l_Estimate += OBJECT_HEADER_BYTES + VIDEO_BODY_BYTES;
		else if (const TextData* l_Text = l_Object->text())
			l_Estimate += OBJECT_HEADER_BYTES + textBodyBytes(*l_Text);
	}
	for (const auto& [l_Id, l_Asset] : p_Document.assets())
		l_Estimate += l_Asset.bytes.size() + 64;

	Writer l_Writer;
	l_Writer.reserve(l_Estimate);
	l_Writer.raw(MAGIC.data(), MAGIC.size());
	l_Writer.u32(BOARD_FORMAT_VERSION);

	l_Writer.beginChunk(TAG_META);
	l_Writer.u64(p_Document.peekNextId());
	l_Writer.u64(p_Document.size());
	l_Writer.string(p_Meta.sourcePath);
	l_Writer.endChunk();

	l_Writer.beginChunk(TAG_VIEW);
	l_Writer.f64(p_Meta.viewCenter.x);
	l_Writer.f64(p_Meta.viewCenter.y);
	l_Writer.f64(p_Meta.viewZoom);
	l_Writer.endChunk();

	writeAssets(l_Writer, p_Document);
	writeFonts(l_Writer, p_Document);

	l_Writer.beginChunk(TAG_OBJS);
	l_Writer.u32(static_cast<uint32_t>(p_Document.size()));
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
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

	l_Writer.beginChunk(TAG_END);
	l_Writer.endChunk();
	return l_Writer.take();
}

LoadResult deserializeBoard(const std::span<const uint8_t> p_Bytes, Document& p_Document, BoardMeta& p_Meta)
{
	Reader l_File(p_Bytes);
	const std::span<const uint8_t> l_Magic = l_File.take(MAGIC.size());
	if (!l_File.ok() || std::memcmp(l_Magic.data(), MAGIC.data(), MAGIC.size()) != 0)
		return failure("This is not a Whiteboard file.");
	const uint32_t l_Version = l_File.u32();
	if (!l_File.ok() || l_Version == 0)
		return failure("The file header is damaged.");
	if (l_Version > BOARD_FORMAT_VERSION)
		return failure("This file was made by a newer version of Whiteboard.");

	std::optional<ParsedMeta> l_ParsedMeta;
	std::optional<std::array<double, 3>> l_View;
	std::vector<std::unique_ptr<Object>> l_Objects;
	std::vector<ImageAsset> l_Assets;
	std::vector<FontAsset> l_Fonts;
	bool l_HaveObjects = false;
	bool l_HaveEnd = false;
	uint32_t l_Skipped = 0;

	while (l_File.remaining() > 0 && !l_HaveEnd)
	{
		const uint32_t l_Tag = l_File.u32();
		const uint64_t l_Length = l_File.u64();
		const uint32_t l_ExpectedCrc = l_File.u32();
		if (!l_File.ok() || l_Length > l_File.remaining())
			return failure("The file is incomplete (truncated).");
		const std::span<const uint8_t> l_Payload = l_File.take(static_cast<size_t>(l_Length));
		if (crc32(l_Payload) != l_ExpectedCrc)
			return failure("The file is damaged (checksum mismatch).");

		Reader l_Reader(l_Payload);
		switch (l_Tag)
		{
		case TAG_META:
		{
			ParsedMeta l_Parsed;
			l_Parsed.nextId = l_Reader.u64();
			l_Parsed.objectCount = l_Reader.u64();
			l_Parsed.sourcePath = l_Reader.string(MAX_PATH_BYTES);
			if (!l_Reader.ok())
				return failure("The file is damaged (bad header chunk).");
			l_ParsedMeta = std::move(l_Parsed);
			break;
		}
		case TAG_VIEW:
		{
			const std::array<double, 3> l_Values{ l_Reader.f64(), l_Reader.f64(), l_Reader.f64() };
			if (!l_Reader.ok() || !finite(l_Values[0]) || !finite(l_Values[1]) || !finite(l_Values[2]) || l_Values[2] <= 0.0)
				return failure("The file is damaged (bad view chunk).");
			l_View = l_Values;
			break;
		}
		case TAG_OBJS:
			if (l_HaveObjects || !parseObjects(l_Payload, l_Objects, l_Skipped))
				return failure("The file is damaged (bad object data).");
			l_HaveObjects = true;
			break;
		case TAG_ASET:
			if (!l_Assets.empty() || !parseAssets(l_Payload, l_Assets))
				return failure("The file is damaged (bad image data).");
			break;
		case TAG_FONT:
			if (!l_Fonts.empty() || !parseFonts(l_Payload, l_Fonts))
				return failure("The file is damaged (bad font data).");
			break;
		case TAG_END:
			l_HaveEnd = true;
			break;
		default:
			break; // a chunk from a newer version
		}
	}

	if (!l_HaveEnd)
		return failure("The file is incomplete (truncated).");
	if (!l_ParsedMeta || !l_HaveObjects)
		return failure("The file is damaged (missing data).");
	// Images whose picture is missing cannot be shown: drop them like objects of an unknown kind
	{
		std::unordered_set<AssetId> l_Known;
		for (const ImageAsset& l_Asset : l_Assets)
			l_Known.insert(l_Asset.id);
		l_Skipped += static_cast<uint32_t>(std::erase_if(l_Objects, [&](const std::unique_ptr<Object>& p_Object)
		{
			const ImageData* l_Image = p_Object->image();
			const VideoData* l_Video = p_Object->video();
			return (l_Image != nullptr && !l_Known.contains(l_Image->asset)) || (l_Video != nullptr && !l_Known.contains(l_Video->asset));
		}));
	}
	if (l_ParsedMeta->objectCount != l_Objects.size() + l_Skipped)
		return failure("The file is damaged (object count mismatch).");

	// Everything checked: only now touch the document
	p_Document.clear();
	if (l_ParsedMeta->nextId > 0)
		p_Document.reserveId(l_ParsedMeta->nextId - 1);
	for (ImageAsset& l_Asset : l_Assets)
		p_Document.insertAsset(std::move(l_Asset));
	for (FontAsset& l_Font : l_Fonts)
		p_Document.setFontAsset(std::move(l_Font));
	for (std::unique_ptr<Object>& l_Object : l_Objects)
		p_Document.insert(std::move(l_Object));

	p_Meta = BoardMeta{};
	p_Meta.sourcePath = std::move(l_ParsedMeta->sourcePath);
	if (l_View)
	{
		p_Meta.viewCenter = DVec2{ (*l_View)[0], (*l_View)[1] };
		p_Meta.viewZoom = (*l_View)[2];
	}
	return LoadResult{ .ok = true, .skippedObjects = l_Skipped };
}
} // namespace wb
