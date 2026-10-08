#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>

import wb.io.gif;

namespace
{
struct TestFrame
{
	uint16_t x = 0;
	uint16_t y = 0;
	uint16_t w = 0;
	uint16_t h = 0;
	std::vector<uint8_t> indices; // picture order, w * h palette indices
	uint8_t disposal = 1;
	int transparent = -1;
	uint16_t delayCs = 10;
	bool interlaced = false;
};

// Palette entry i is (i * 16, 255 - i * 16, i * 8)
void paletteColor(const int p_Index, uint8_t* p_Rgb)
{
	p_Rgb[0] = static_cast<uint8_t>(p_Index * 16);
	p_Rgb[1] = static_cast<uint8_t>(255 - p_Index * 16);
	p_Rgb[2] = static_cast<uint8_t>(p_Index * 8);
}

// Valid GIF with an uncompressed LZW stream: the encoder restarts the dictionary every 12 pixels
std::vector<uint8_t> encodeGif(const int p_Width, const int p_Height, const std::vector<TestFrame>& p_Frames)
{
	std::vector<uint8_t> l_Out{ 'G', 'I', 'F', '8', '9', 'a' };
	const auto l_U16 = [&](const int p_Value)
	{
		l_Out.push_back(static_cast<uint8_t>(p_Value & 0xFF));
		l_Out.push_back(static_cast<uint8_t>((p_Value >> 8) & 0xFF));
	};
	l_U16(p_Width);
	l_U16(p_Height);
	l_Out.insert(l_Out.end(), { 0xF3, 0, 0 });
	for (int i = 0; i < 16; ++i)
	{
		uint8_t l_Rgb[3];
		paletteColor(i, l_Rgb);
		l_Out.insert(l_Out.end(), l_Rgb, l_Rgb + 3);
	}

	for (const TestFrame& l_Frame : p_Frames)
	{
		l_Out.insert(l_Out.end(), { 0x21, 0xF9, 0x04 });
		l_Out.push_back(static_cast<uint8_t>((l_Frame.disposal << 2) | (l_Frame.transparent >= 0 ? 1 : 0)));
		l_U16(l_Frame.delayCs);
		l_Out.push_back(static_cast<uint8_t>(std::max(l_Frame.transparent, 0)));
		l_Out.push_back(0);

		l_Out.push_back(0x2C);
		l_U16(l_Frame.x);
		l_U16(l_Frame.y);
		l_U16(l_Frame.w);
		l_U16(l_Frame.h);
		l_Out.push_back(l_Frame.interlaced ? 0x40 : 0);
		l_Out.push_back(4);

		// Rows in stored order
		std::vector<uint8_t> l_Stored;
		if (l_Frame.interlaced)
		{
			for (const auto& [l_Start, l_Step] : std::array<std::pair<int, int>, 4>{ { { 0, 8 }, { 4, 8 }, { 2, 4 }, { 1, 2 } } })
			{
				for (int l_Row = l_Start; l_Row < l_Frame.h; l_Row += l_Step)
					l_Stored.insert(l_Stored.end(), l_Frame.indices.begin() + l_Row * l_Frame.w, l_Frame.indices.begin() + (l_Row + 1) * l_Frame.w);
			}
		}
		else
		{
			l_Stored = l_Frame.indices;
		}

		std::vector<uint8_t> l_Stream;
		uint32_t l_Bits = 0;
		int l_BitCount = 0;
		const auto l_Code = [&](const uint32_t p_Value)
		{
			l_Bits |= p_Value << l_BitCount;
			l_BitCount += 5;
			while (l_BitCount >= 8)
			{
				l_Stream.push_back(static_cast<uint8_t>(l_Bits & 0xFF));
				l_Bits >>= 8;
				l_BitCount -= 8;
			}
		};
		l_Code(16);
		int l_Since = 0;
		for (const uint8_t l_Index : l_Stored)
		{
			l_Code(l_Index);
			if (++l_Since == 12)
			{
				l_Code(16);
				l_Since = 0;
			}
		}
		l_Code(17);
		if (l_BitCount > 0)
			l_Stream.push_back(static_cast<uint8_t>(l_Bits & 0xFF));
		for (size_t l_Pos = 0; l_Pos < l_Stream.size(); l_Pos += 255)
		{
			const size_t l_Chunk = std::min<size_t>(255, l_Stream.size() - l_Pos);
			l_Out.push_back(static_cast<uint8_t>(l_Chunk));
			l_Out.insert(l_Out.end(), l_Stream.begin() + static_cast<std::ptrdiff_t>(l_Pos), l_Stream.begin() + static_cast<std::ptrdiff_t>(l_Pos + l_Chunk));
		}
		l_Out.push_back(0);
	}
	l_Out.push_back(0x3B);
	return l_Out;
}

struct Decoded
{
	std::vector<std::vector<uint8_t>> frames;
	std::vector<uint32_t> delays;
};

Decoded decodeAll(const std::vector<uint8_t>& p_Gif, bool* p_Result = nullptr)
{
	Decoded l_Decoded;
	const bool l_Ok = wb::decodeGif(p_Gif, [&](const std::span<const uint8_t> p_Rgba, const uint32_t p_Delay)
	{
		l_Decoded.frames.emplace_back(p_Rgba.begin(), p_Rgba.end());
		l_Decoded.delays.push_back(p_Delay);
		return true;
	});
	if (p_Result != nullptr)
		*p_Result = l_Ok;
	return l_Decoded;
}

void expectPixel(const std::vector<uint8_t>& p_Frame, const int p_Width, const int p_X, const int p_Y, const int p_PaletteIndex)
{
	uint8_t l_Rgb[3];
	paletteColor(p_PaletteIndex, l_Rgb);
	const uint8_t* l_Pixel = &p_Frame[(static_cast<size_t>(p_Y) * static_cast<size_t>(p_Width) + static_cast<size_t>(p_X)) * 4];
	EXPECT_EQ(l_Pixel[0], l_Rgb[0]) << "at " << p_X << "," << p_Y;
	EXPECT_EQ(l_Pixel[1], l_Rgb[1]) << "at " << p_X << "," << p_Y;
	EXPECT_EQ(l_Pixel[2], l_Rgb[2]) << "at " << p_X << "," << p_Y;
	EXPECT_EQ(l_Pixel[3], 255) << "at " << p_X << "," << p_Y;
}

void expectTransparent(const std::vector<uint8_t>& p_Frame, const int p_Width, const int p_X, const int p_Y)
{
	EXPECT_EQ(p_Frame[(static_cast<size_t>(p_Y) * static_cast<size_t>(p_Width) + static_cast<size_t>(p_X)) * 4 + 3], 0) << "at " << p_X << "," << p_Y;
}

TestFrame solid(const int p_X, const int p_Y, const int p_W, const int p_H, const uint8_t p_Index)
{
	TestFrame l_Frame;
	l_Frame.x = static_cast<uint16_t>(p_X);
	l_Frame.y = static_cast<uint16_t>(p_Y);
	l_Frame.w = static_cast<uint16_t>(p_W);
	l_Frame.h = static_cast<uint16_t>(p_H);
	l_Frame.indices.assign(static_cast<size_t>(p_W) * static_cast<size_t>(p_H), p_Index);
	return l_Frame;
}
} // namespace

TEST(Gif, FullFramesKeepOrderColoursAndDelays)
{
	std::vector<TestFrame> l_Frames{ solid(0, 0, 4, 4, 1), solid(0, 0, 4, 4, 2), solid(0, 0, 4, 4, 3) };
	l_Frames[1].delayCs = 25;
	const Decoded l_Decoded = decodeAll(encodeGif(4, 4, l_Frames));
	ASSERT_EQ(l_Decoded.frames.size(), 3u);
	EXPECT_EQ(l_Decoded.delays, (std::vector<uint32_t>{ 100, 250, 100 }));
	expectPixel(l_Decoded.frames[0], 4, 2, 2, 1);
	expectPixel(l_Decoded.frames[1], 4, 0, 3, 2);
	expectPixel(l_Decoded.frames[2], 4, 3, 0, 3);
}

TEST(Gif, SubRectangleFramesAreComposedOntoThePreviousOnes)
{
	// The animation keeps drawing small rectangles over the first picture: every frame must contain all of it
	std::vector<TestFrame> l_Frames{ solid(0, 0, 6, 6, 1), solid(1, 1, 2, 2, 2), solid(3, 3, 2, 2, 3), solid(0, 4, 3, 2, 4) };
	l_Frames[1].indices[3] = 0; // the last pixel of the 2x2 rectangle is transparent
	l_Frames[1].transparent = 0;
	const Decoded l_Decoded = decodeAll(encodeGif(6, 6, l_Frames));
	ASSERT_EQ(l_Decoded.frames.size(), 4u);
	expectPixel(l_Decoded.frames[1], 6, 1, 1, 2);
	expectPixel(l_Decoded.frames[1], 6, 2, 2, 1); // transparent index: shows the frame below
	expectPixel(l_Decoded.frames[1], 6, 5, 5, 1);
	expectPixel(l_Decoded.frames[2], 6, 1, 1, 2); // still there
	expectPixel(l_Decoded.frames[2], 6, 4, 4, 3);
	expectPixel(l_Decoded.frames[3], 6, 3, 3, 3);
	expectPixel(l_Decoded.frames[3], 6, 1, 5, 4);
	expectPixel(l_Decoded.frames[3], 6, 0, 0, 1);
}

TEST(Gif, RestoreToBackgroundClearsOnlyThatRectangle)
{
	std::vector<TestFrame> l_Frames{ solid(0, 0, 4, 4, 1), solid(0, 0, 2, 2, 2), solid(3, 3, 1, 1, 3) };
	l_Frames[1].disposal = 2;
	const Decoded l_Decoded = decodeAll(encodeGif(4, 4, l_Frames));
	ASSERT_EQ(l_Decoded.frames.size(), 3u);
	expectPixel(l_Decoded.frames[1], 4, 0, 0, 2);
	expectTransparent(l_Decoded.frames[2], 4, 0, 0); // cleared after frame 2
	expectPixel(l_Decoded.frames[2], 4, 2, 2, 1);    // outside the rectangle: untouched
	expectPixel(l_Decoded.frames[2], 4, 3, 3, 3);
}

TEST(Gif, RestoreToPreviousBringsBackTheCanvas)
{
	std::vector<TestFrame> l_Frames{ solid(0, 0, 4, 4, 1), solid(0, 0, 2, 2, 2), solid(3, 3, 1, 1, 3) };
	l_Frames[1].disposal = 3;
	const Decoded l_Decoded = decodeAll(encodeGif(4, 4, l_Frames));
	ASSERT_EQ(l_Decoded.frames.size(), 3u);
	expectPixel(l_Decoded.frames[1], 4, 0, 0, 2);
	expectPixel(l_Decoded.frames[2], 4, 0, 0, 1); // back to what was below frame 2
	expectPixel(l_Decoded.frames[2], 4, 3, 3, 3);
}

TEST(Gif, InterlacedRowsAreRestoredToPictureOrder)
{
	TestFrame l_Frame = solid(0, 0, 3, 10, 0);
	for (int y = 0; y < 10; ++y)
		std::fill(l_Frame.indices.begin() + y * 3, l_Frame.indices.begin() + (y + 1) * 3, static_cast<uint8_t>(y + 1));
	l_Frame.interlaced = true;
	const Decoded l_Decoded = decodeAll(encodeGif(3, 10, { l_Frame }));
	ASSERT_EQ(l_Decoded.frames.size(), 1u);
	for (int y = 0; y < 10; ++y)
		expectPixel(l_Decoded.frames[0], 3, 1, y, y + 1);
}

TEST(Gif, DamagedFilesKeepTheFramesBeforeTheDamage)
{
	std::vector<TestFrame> l_Frames{ solid(0, 0, 8, 8, 1), solid(0, 0, 8, 8, 2), solid(0, 0, 8, 8, 3) };
	const std::vector<uint8_t> l_Gif = encodeGif(8, 8, l_Frames);
	// Cut in the middle of the third frame
	const std::vector<uint8_t> l_Cut(l_Gif.begin(), l_Gif.begin() + static_cast<std::ptrdiff_t>(l_Gif.size() - 30));
	bool l_Ok = false;
	const Decoded l_Decoded = decodeAll(l_Cut, &l_Ok);
	EXPECT_TRUE(l_Ok);
	EXPECT_GE(l_Decoded.frames.size(), 2u);
	expectPixel(l_Decoded.frames[0], 8, 4, 4, 1);
	expectPixel(l_Decoded.frames[1], 8, 4, 4, 2);
}

TEST(Gif, NotAGifIsRejected)
{
	bool l_Ok = true;
	const std::vector<uint8_t> l_Png{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0 };
	EXPECT_TRUE(decodeAll(l_Png, &l_Ok).frames.empty());
	EXPECT_FALSE(l_Ok);
	wb::GifHeader l_Header;
	EXPECT_FALSE(wb::readGifHeader(l_Png, l_Header));
	const std::vector<uint8_t> l_Gif = encodeGif(5, 7, { solid(0, 0, 5, 7, 1) });
	ASSERT_TRUE(wb::readGifHeader(l_Gif, l_Header));
	EXPECT_EQ(l_Header.width, 5u);
	EXPECT_EQ(l_Header.height, 7u);
}
