module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

module wb.io.gif;

namespace wb
{
namespace
{
constexpr uint64_t MAX_PIXELS = 400'000'000ull;
constexpr size_t MAX_CODES = 4096;

struct Palette
{
	std::vector<uint8_t> rgb; // 3 bytes per entry
	[[nodiscard]] size_t size() const { return rgb.size() / 3; }
};

// Cursor over the byte stream; every read is bounds checked
class Bytes
{
public:
	explicit Bytes(const std::span<const uint8_t> p_Data) : m_Data(p_Data) {}
	[[nodiscard]] bool atEnd() const { return m_Pos >= m_Data.size(); }
	[[nodiscard]] size_t remaining() const { return m_Data.size() - std::min(m_Pos, m_Data.size()); }
	[[nodiscard]] bool has(const size_t p_Count) const { return remaining() >= p_Count; }
	uint8_t u8() { return m_Pos < m_Data.size() ? m_Data[m_Pos++] : (m_Failed = true, uint8_t{ 0 }); }
	uint16_t u16()
	{
		const uint16_t l_Low = u8();
		return static_cast<uint16_t>(l_Low | (static_cast<uint16_t>(u8()) << 8));
	}
	void skip(const size_t p_Count) { m_Pos = std::min(m_Pos + p_Count, m_Data.size()); }
	// Skips data sub-blocks up to and including the terminator
	void skipSubBlocks()
	{
		while (!atEnd())
		{
			const uint8_t l_Size = u8();
			if (l_Size == 0)
				return;
			skip(l_Size);
		}
	}
	// Concatenates data sub-blocks up to and including the terminator
	std::vector<uint8_t> subBlocks()
	{
		std::vector<uint8_t> l_Data;
		while (!atEnd())
		{
			const uint8_t l_Size = u8();
			if (l_Size == 0)
				break;
			const size_t l_Take = std::min<size_t>(l_Size, remaining());
			l_Data.insert(l_Data.end(), m_Data.begin() + static_cast<std::ptrdiff_t>(m_Pos), m_Data.begin() + static_cast<std::ptrdiff_t>(m_Pos + l_Take));
			m_Pos += l_Take;
		}
		return l_Data;
	}
	[[nodiscard]] bool failed() const { return m_Failed; }

private:
	std::span<const uint8_t> m_Data;
	size_t m_Pos = 0;
	bool m_Failed = false;
};

bool readPalette(Bytes& p_Bytes, const size_t p_Entries, Palette& p_Palette)
{
	if (!p_Bytes.has(p_Entries * 3))
		return false;
	p_Palette.rgb.resize(p_Entries * 3);
	for (uint8_t& l_Component : p_Palette.rgb)
		l_Component = p_Bytes.u8();
	return true;
}

// LZW decompression into palette indices. Stops quietly at damaged data; indices not reached stay 0.
void lzwDecode(const std::vector<uint8_t>& p_Data, const uint32_t p_MinCodeSize, std::vector<uint8_t>& p_Indices)
{
	const uint32_t l_Clear = 1u << p_MinCodeSize;
	const uint32_t l_End = l_Clear + 1;
	std::array<uint16_t, MAX_CODES> l_Prefix{};
	std::array<uint8_t, MAX_CODES> l_Suffix{};
	std::array<uint8_t, MAX_CODES + 1> l_Stack{};
	for (uint32_t i = 0; i < l_Clear; ++i)
		l_Suffix[i] = static_cast<uint8_t>(i);

	uint32_t l_CodeSize = p_MinCodeSize + 1;
	uint32_t l_Next = l_End + 1;
	int32_t l_Previous = -1;
	uint8_t l_First = 0;
	uint32_t l_Bits = 0;
	uint32_t l_BitCount = 0;
	size_t l_In = 0;
	size_t l_Out = 0;

	while (l_Out < p_Indices.size())
	{
		while (l_BitCount < l_CodeSize)
		{
			if (l_In >= p_Data.size())
				return;
			l_Bits |= static_cast<uint32_t>(p_Data[l_In++]) << l_BitCount;
			l_BitCount += 8;
		}
		const uint32_t l_Code = l_Bits & ((1u << l_CodeSize) - 1);
		l_Bits >>= l_CodeSize;
		l_BitCount -= l_CodeSize;

		if (l_Code == l_Clear)
		{
			l_CodeSize = p_MinCodeSize + 1;
			l_Next = l_End + 1;
			l_Previous = -1;
			continue;
		}
		if (l_Code == l_End)
			return;

		if (l_Previous < 0)
		{
			if (l_Code >= l_Clear)
				return;
			p_Indices[l_Out++] = l_Suffix[l_Code];
			l_First = l_Suffix[l_Code];
			l_Previous = static_cast<int32_t>(l_Code);
			continue;
		}

		uint32_t l_Current = l_Code;
		size_t l_Depth = 0;
		if (l_Current >= l_Next)
		{
			if (l_Current != l_Next)
				return; // damaged
			l_Stack[l_Depth++] = l_First;
			l_Current = static_cast<uint32_t>(l_Previous);
		}
		while (l_Current >= l_Clear)
		{
			if (l_Current >= MAX_CODES || l_Depth >= MAX_CODES)
				return;
			l_Stack[l_Depth++] = l_Suffix[l_Current];
			l_Current = l_Prefix[l_Current];
		}
		l_First = l_Suffix[l_Current];
		l_Stack[l_Depth++] = l_First;
		while (l_Depth > 0 && l_Out < p_Indices.size())
			p_Indices[l_Out++] = l_Stack[--l_Depth];

		if (l_Next < MAX_CODES)
		{
			l_Prefix[l_Next] = static_cast<uint16_t>(l_Previous);
			l_Suffix[l_Next] = l_First;
			++l_Next;
			if (l_Next == (1u << l_CodeSize) && l_CodeSize < 12)
				++l_CodeSize;
		}
		l_Previous = static_cast<int32_t>(l_Code);
	}
}

// Row of the picture that the n-th stored row of an interlaced image belongs to
uint32_t interlacedRow(const uint32_t p_Stored, const uint32_t p_Height)
{
	static constexpr std::array<uint32_t, 4> START{ 0, 4, 2, 1 };
	static constexpr std::array<uint32_t, 4> STEP{ 8, 8, 4, 2 };
	uint32_t l_Index = p_Stored;
	for (size_t l_Pass = 0; l_Pass < 4; ++l_Pass)
	{
		const uint32_t l_Rows = p_Height > START[l_Pass] ? (p_Height - START[l_Pass] + STEP[l_Pass] - 1) / STEP[l_Pass] : 0;
		if (l_Index < l_Rows)
			return START[l_Pass] + l_Index * STEP[l_Pass];
		l_Index -= l_Rows;
	}
	return p_Height; // out of range
}
} // namespace

bool readGifHeader(const std::span<const uint8_t> p_Bytes, GifHeader& p_Header)
{
	if (p_Bytes.size() < 13 || (std::memcmp(p_Bytes.data(), "GIF87a", 6) != 0 && std::memcmp(p_Bytes.data(), "GIF89a", 6) != 0))
		return false;
	p_Header.width = static_cast<uint32_t>(p_Bytes[6] | (p_Bytes[7] << 8));
	p_Header.height = static_cast<uint32_t>(p_Bytes[8] | (p_Bytes[9] << 8));
	return true;
}

bool decodeGif(const std::span<const uint8_t> p_Bytes, const GifFrameSink& p_Sink)
{
	GifHeader l_Header;
	if (!readGifHeader(p_Bytes, l_Header) || l_Header.width == 0 || l_Header.height == 0 || static_cast<uint64_t>(l_Header.width) * l_Header.height > MAX_PIXELS)
		return false;
	const uint32_t l_Width = l_Header.width;
	const uint32_t l_Height = l_Header.height;

	Bytes l_Stream(p_Bytes);
	l_Stream.skip(10);
	const uint8_t l_ScreenFlags = l_Stream.u8();
	l_Stream.skip(2); // background colour index and pixel aspect ratio: ignored, like browsers do
	Palette l_Global;
	if ((l_ScreenFlags & 0x80) != 0 && !readPalette(l_Stream, static_cast<size_t>(1) << ((l_ScreenFlags & 7) + 1), l_Global))
		return false;

	// The screen starts transparent
	std::vector<uint8_t> l_Canvas(static_cast<size_t>(l_Width) * l_Height * 4, 0);
	std::vector<uint8_t> l_Saved; // canvas before a frame that is to be undone ("restore to previous")

	struct Pending
	{
		uint8_t disposal = 0;
		bool transparent = false;
		uint8_t transparentIndex = 0;
		uint32_t delayMs = 0;
	} l_Control;
	struct Applied
	{
		uint8_t disposal = 0;
		uint32_t left = 0, top = 0, width = 0, height = 0;
	} l_Last;

	size_t l_Frames = 0;
	while (!l_Stream.atEnd() && !l_Stream.failed())
	{
		const uint8_t l_Block = l_Stream.u8();
		if (l_Block == 0x3B)
			break; // trailer
		if (l_Block == 0x21)
		{
			const uint8_t l_Label = l_Stream.u8();
			if (l_Label == 0xF9)
			{
				const uint8_t l_Size = l_Stream.u8();
				if (l_Size >= 4)
				{
					const uint8_t l_Flags = l_Stream.u8();
					l_Control.disposal = static_cast<uint8_t>((l_Flags >> 2) & 7);
					l_Control.transparent = (l_Flags & 1) != 0;
					l_Control.delayMs = static_cast<uint32_t>(l_Stream.u16()) * 10u;
					l_Control.transparentIndex = l_Stream.u8();
					l_Stream.skip(l_Size - 4u);
				}
				else
				{
					l_Stream.skip(l_Size);
				}
			}
			l_Stream.skipSubBlocks();
			continue;
		}
		if (l_Block != 0x2C)
			break; // not a GIF block: stop with what we have

		// ---- image
		const uint32_t l_Left = l_Stream.u16();
		const uint32_t l_Top = l_Stream.u16();
		const uint32_t l_FrameWidth = l_Stream.u16();
		const uint32_t l_FrameHeight = l_Stream.u16();
		const uint8_t l_ImageFlags = l_Stream.u8();
		Palette l_Local;
		if ((l_ImageFlags & 0x80) != 0 && !readPalette(l_Stream, static_cast<size_t>(1) << ((l_ImageFlags & 7) + 1), l_Local))
			break;
		const uint32_t l_MinCodeSize = l_Stream.u8();
		if (l_Stream.failed() || l_MinCodeSize < 1 || l_MinCodeSize > 11)
			break;
		const std::vector<uint8_t> l_Data = l_Stream.subBlocks();
		if (l_FrameWidth == 0 || l_FrameHeight == 0 || static_cast<uint64_t>(l_FrameWidth) * l_FrameHeight > MAX_PIXELS)
		{
			l_Control = Pending{};
			continue;
		}

		std::vector<uint8_t> l_Indices(static_cast<size_t>(l_FrameWidth) * l_FrameHeight, 0);
		lzwDecode(l_Data, std::max(l_MinCodeSize, 2u), l_Indices);

		// What the previous frame asked for happens before this one is drawn
		if (l_Frames > 0)
		{
			if (l_Last.disposal == 2)
			{
				for (uint32_t y = l_Last.top; y < std::min(l_Last.top + l_Last.height, l_Height); ++y)
				{
					const uint32_t l_From = std::min(l_Last.left, l_Width);
					const uint32_t l_To = std::min(l_Last.left + l_Last.width, l_Width);
					if (l_To > l_From)
						std::memset(&l_Canvas[(static_cast<size_t>(y) * l_Width + l_From) * 4], 0, static_cast<size_t>(l_To - l_From) * 4);
				}
			}
			else if (l_Last.disposal == 3 && l_Saved.size() == l_Canvas.size())
			{
				l_Canvas = l_Saved;
			}
		}
		if (l_Control.disposal == 3)
			l_Saved = l_Canvas;

		const Palette& l_Palette = (l_ImageFlags & 0x80) != 0 ? l_Local : l_Global;
		const bool l_Interlaced = (l_ImageFlags & 0x40) != 0;
		if (l_Palette.size() > 0)
		{
			for (uint32_t l_Stored = 0; l_Stored < l_FrameHeight; ++l_Stored)
			{
				const uint32_t l_Row = l_Interlaced ? interlacedRow(l_Stored, l_FrameHeight) : l_Stored;
				const uint32_t y = l_Top + l_Row;
				if (l_Row >= l_FrameHeight || y >= l_Height)
					continue;
				for (uint32_t x = 0; x < l_FrameWidth && l_Left + x < l_Width; ++x)
				{
					const uint8_t l_Index = l_Indices[static_cast<size_t>(l_Stored) * l_FrameWidth + x];
					if (l_Control.transparent && l_Index == l_Control.transparentIndex)
						continue;
					uint8_t* l_Pixel = &l_Canvas[(static_cast<size_t>(y) * l_Width + l_Left + x) * 4];
					if (l_Index < l_Palette.size())
					{
						l_Pixel[0] = l_Palette.rgb[l_Index * 3];
						l_Pixel[1] = l_Palette.rgb[l_Index * 3 + 1];
						l_Pixel[2] = l_Palette.rgb[l_Index * 3 + 2];
						l_Pixel[3] = 255;
					}
				}
			}
		}

		++l_Frames;
		if (!p_Sink(l_Canvas, l_Control.delayMs))
			return true;
		l_Last = Applied{ .disposal = l_Control.disposal, .left = l_Left, .top = l_Top, .width = l_FrameWidth, .height = l_FrameHeight };
		l_Control = Pending{};
	}
	return l_Frames > 0;
}
} // namespace wb
