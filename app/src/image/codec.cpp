module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "image/stb_image_all.hpp"

module wb.image.codec;

import wb.io.gif;

namespace wb::image
{
namespace
{
constexpr uint64_t MAX_PIXELS = 400'000'000ull; // refuses decompression bombs
constexpr uint32_t DEFAULT_DELAY_MS = 100;

struct StbiDeleter
{
	void operator()(unsigned char* p_Pixels) const { stbi_image_free(p_Pixels); }
};
using StbiPixels = std::unique_ptr<unsigned char, StbiDeleter>;

bool isGif(const std::span<const uint8_t> p_Bytes)
{
	return p_Bytes.size() > 6 && std::memcmp(p_Bytes.data(), "GIF8", 4) == 0;
}

void premultiply(std::vector<uint8_t>& p_Pixels)
{
	for (size_t i = 0; i + 3 < p_Pixels.size(); i += 4)
	{
		const uint32_t l_Alpha = p_Pixels[i + 3];
		if (l_Alpha == 255)
			continue;
		p_Pixels[i] = static_cast<uint8_t>((p_Pixels[i] * l_Alpha + 127) / 255);
		p_Pixels[i + 1] = static_cast<uint8_t>((p_Pixels[i + 1] * l_Alpha + 127) / 255);
		p_Pixels[i + 2] = static_cast<uint8_t>((p_Pixels[i + 2] * l_Alpha + 127) / 255);
	}
}

// Straight-alpha RGBA8 resize, never larger than the original
std::vector<uint8_t> resizeRgba(const uint8_t* p_Pixels, const uint32_t p_Width, const uint32_t p_Height, const uint32_t p_NewWidth, const uint32_t p_NewHeight)
{
	std::vector<uint8_t> l_Out(static_cast<size_t>(p_NewWidth) * p_NewHeight * 4);
	stbir_resize_uint8_srgb(p_Pixels, static_cast<int>(p_Width), static_cast<int>(p_Height), static_cast<int>(p_Width) * 4, l_Out.data(), static_cast<int>(p_NewWidth), static_cast<int>(p_NewHeight), static_cast<int>(p_NewWidth) * 4, STBIR_RGBA);
	return l_Out;
}

void fitWithin(const uint32_t p_Width, const uint32_t p_Height, const uint32_t p_Max, uint32_t& p_OutWidth, uint32_t& p_OutHeight)
{
	p_OutWidth = p_Width;
	p_OutHeight = p_Height;
	const uint32_t l_Longest = std::max(p_Width, p_Height);
	if (l_Longest <= p_Max)
		return;
	const double l_Scale = static_cast<double>(p_Max) / static_cast<double>(l_Longest);
	p_OutWidth = std::max(1u, static_cast<uint32_t>(std::lround(p_Width * l_Scale)));
	p_OutHeight = std::max(1u, static_cast<uint32_t>(std::lround(p_Height * l_Scale)));
}

void appendBytes(void* p_Context, void* p_Data, const int p_Size)
{
	auto* l_Out = static_cast<std::vector<uint8_t>*>(p_Context);
	const auto* l_Bytes = static_cast<const uint8_t*>(p_Data);
	l_Out->insert(l_Out->end(), l_Bytes, l_Bytes + p_Size);
}
} // namespace

std::optional<ImageInfo> probe(const std::span<const uint8_t> p_Bytes)
{
	int l_Width = 0, l_Height = 0, l_Components = 0;
	if (p_Bytes.empty() || p_Bytes.size() > static_cast<size_t>(INT32_MAX) || stbi_info_from_memory(p_Bytes.data(), static_cast<int>(p_Bytes.size()), &l_Width, &l_Height, &l_Components) == 0)
		return std::nullopt;
	if (l_Width <= 0 || l_Height <= 0 || static_cast<uint64_t>(l_Width) * static_cast<uint64_t>(l_Height) > MAX_PIXELS)
		return std::nullopt;
	return ImageInfo{ .width = static_cast<uint32_t>(l_Width), .height = static_cast<uint32_t>(l_Height), .gif = isGif(p_Bytes) };
}

std::optional<Decoded> decode(const std::span<const uint8_t> p_Bytes, const DecodeLimits& p_Limits, std::string& p_Error)
{
	const std::optional<ImageInfo> l_Info = probe(p_Bytes);
	if (!l_Info)
	{
		p_Error = "This is not a picture Whiteboard can read.";
		return std::nullopt;
	}
	const int l_Size = static_cast<int>(p_Bytes.size());

	if (l_Info->gif)
	{
		// stb_image only handles the simplest GIFs (it drops frames of many animations), so GIFs have their own decoder
		GifHeader l_GifHeader;
		if (readGifHeader(p_Bytes, l_GifHeader) && l_GifHeader.width > 0 && l_GifHeader.height > 0)
		{
			Decoded l_Gif;
			fitWithin(l_GifHeader.width, l_GifHeader.height, p_Limits.maxDimension, l_Gif.width, l_Gif.height);
			const size_t l_GifFrameBytes = static_cast<size_t>(l_Gif.width) * l_Gif.height * 4;
			const size_t l_GifMaxFrames = std::max<size_t>(1, p_Limits.maxBytes / std::max<size_t>(l_GifFrameBytes, 1));
			const bool l_Resize = l_Gif.width != l_GifHeader.width || l_Gif.height != l_GifHeader.height;
			const bool l_Decoded = decodeGif(p_Bytes, [&](const std::span<const uint8_t> p_Rgba, const uint32_t p_DelayMs)
			{
				if (l_Gif.frames.size() >= l_GifMaxFrames)
				{
					l_Gif.truncated = true;
					return false;
				}
				std::vector<uint8_t> l_Frame = l_Resize ? resizeRgba(p_Rgba.data(), l_GifHeader.width, l_GifHeader.height, l_Gif.width, l_Gif.height) : std::vector<uint8_t>(p_Rgba.begin(), p_Rgba.end());
				premultiply(l_Frame);
				l_Gif.frames.push_back(std::move(l_Frame));
				// Browsers play delays of 0 or 10 ms as 100 ms
				l_Gif.delaysMs.push_back(p_DelayMs <= 10 ? DEFAULT_DELAY_MS : p_DelayMs);
				return true;
			});
			if (l_Decoded && !l_Gif.frames.empty())
				return l_Gif;
		}
	}

	std::vector<const uint8_t*> l_Sources; // one pointer per frame into the decoded block
	StbiPixels l_Block;
	int* l_RawDelays = nullptr;
	int l_Width = 0, l_Height = 0, l_FrameCount = 1, l_Components = 0;

	if (l_Info->gif)
	{
		// The size of a GIF is known from its header, so refuse bombs before decoding every frame
		l_Block.reset(stbi_load_gif_from_memory(p_Bytes.data(), l_Size, &l_RawDelays, &l_Width, &l_Height, &l_FrameCount, &l_Components, 4));
	}
	else
	{
		l_Block.reset(stbi_load_from_memory(p_Bytes.data(), l_Size, &l_Width, &l_Height, &l_Components, 4));
	}
	if (!l_Block || l_Width <= 0 || l_Height <= 0 || l_FrameCount <= 0)
	{
		const char* l_Reason = stbi_failure_reason();
		p_Error = std::string("The picture could not be decoded") + (l_Reason != nullptr ? std::string(" (") + l_Reason + ")." : ".");
		if (l_RawDelays != nullptr)
			stbi_image_free(l_RawDelays);
		return std::nullopt;
	}

	Decoded l_Result;
	uint32_t l_OutWidth = 0, l_OutHeight = 0;
	fitWithin(static_cast<uint32_t>(l_Width), static_cast<uint32_t>(l_Height), p_Limits.maxDimension, l_OutWidth, l_OutHeight);
	l_Result.width = l_OutWidth;
	l_Result.height = l_OutHeight;

	const size_t l_SourceFrameBytes = static_cast<size_t>(l_Width) * static_cast<size_t>(l_Height) * 4;
	const size_t l_FrameBytes = static_cast<size_t>(l_OutWidth) * static_cast<size_t>(l_OutHeight) * 4;
	const size_t l_MaxFrames = std::max<size_t>(1, p_Limits.maxBytes / std::max<size_t>(l_FrameBytes, 1));
	const size_t l_Keep = std::min<size_t>(static_cast<size_t>(l_FrameCount), l_MaxFrames);
	l_Result.truncated = l_Keep < static_cast<size_t>(l_FrameCount);

	l_Result.frames.reserve(l_Keep);
	for (size_t i = 0; i < l_Keep; ++i)
	{
		const uint8_t* l_Source = l_Block.get() + i * l_SourceFrameBytes;
		std::vector<uint8_t> l_Frame = (l_OutWidth != static_cast<uint32_t>(l_Width) || l_OutHeight != static_cast<uint32_t>(l_Height))
			? resizeRgba(l_Source, static_cast<uint32_t>(l_Width), static_cast<uint32_t>(l_Height), l_OutWidth, l_OutHeight)
			: std::vector<uint8_t>(l_Source, l_Source + l_FrameBytes);
		premultiply(l_Frame);
		l_Result.frames.push_back(std::move(l_Frame));
		const int l_Delay = l_RawDelays != nullptr ? l_RawDelays[i] : 0;
		// Browsers play delays of 0 or 10 ms (1 hundredth) as 100 ms
		l_Result.delaysMs.push_back(l_Delay <= 10 ? DEFAULT_DELAY_MS : static_cast<uint32_t>(l_Delay));
	}
	if (l_RawDelays != nullptr)
		stbi_image_free(l_RawDelays);
	return l_Result;
}

std::optional<Recompressed> recompress(const std::span<const uint8_t> p_Bytes, const uint32_t p_MaxDimension, const int p_JpegQuality, std::string& p_Error)
{
	const std::optional<ImageInfo> l_Info = probe(p_Bytes);
	if (!l_Info || l_Info->gif)
	{
		p_Error = l_Info ? "Animated pictures cannot be recompressed." : "This is not a picture Whiteboard can read.";
		return std::nullopt;
	}
	int l_Width = 0, l_Height = 0, l_Components = 0;
	const StbiPixels l_Pixels(stbi_load_from_memory(p_Bytes.data(), static_cast<int>(p_Bytes.size()), &l_Width, &l_Height, &l_Components, 4));
	if (!l_Pixels)
	{
		p_Error = "The picture could not be decoded.";
		return std::nullopt;
	}

	bool l_HasAlpha = false;
	const size_t l_PixelCount = static_cast<size_t>(l_Width) * static_cast<size_t>(l_Height);
	for (size_t i = 0; i < l_PixelCount && !l_HasAlpha; ++i)
		l_HasAlpha = l_Pixels.get()[i * 4 + 3] != 255;

	uint32_t l_OutWidth = 0, l_OutHeight = 0;
	fitWithin(static_cast<uint32_t>(l_Width), static_cast<uint32_t>(l_Height), p_MaxDimension, l_OutWidth, l_OutHeight);
	std::vector<uint8_t> l_Resized;
	const uint8_t* l_Source = l_Pixels.get();
	if (l_OutWidth != static_cast<uint32_t>(l_Width) || l_OutHeight != static_cast<uint32_t>(l_Height))
	{
		l_Resized = resizeRgba(l_Source, static_cast<uint32_t>(l_Width), static_cast<uint32_t>(l_Height), l_OutWidth, l_OutHeight);
		l_Source = l_Resized.data();
	}

	Recompressed l_Result;
	l_Result.width = l_OutWidth;
	l_Result.height = l_OutHeight;
	if (l_HasAlpha)
	{
		l_Result.extension = "png";
		stbi_write_png_compression_level = 9;
		stbi_write_png_to_func(&appendBytes, &l_Result.bytes, static_cast<int>(l_OutWidth), static_cast<int>(l_OutHeight), 4, l_Source, static_cast<int>(l_OutWidth) * 4);
	}
	else
	{
		l_Result.extension = "jpg";
		std::vector<uint8_t> l_Rgb(static_cast<size_t>(l_OutWidth) * l_OutHeight * 3);
		for (size_t i = 0; i < static_cast<size_t>(l_OutWidth) * l_OutHeight; ++i)
		{
			l_Rgb[i * 3] = l_Source[i * 4];
			l_Rgb[i * 3 + 1] = l_Source[i * 4 + 1];
			l_Rgb[i * 3 + 2] = l_Source[i * 4 + 2];
		}
		stbi_write_jpg_to_func(&appendBytes, &l_Result.bytes, static_cast<int>(l_OutWidth), static_cast<int>(l_OutHeight), 3, l_Rgb.data(), p_JpegQuality);
	}

	// Worth it only when it saves at least a tenth
	if (l_Result.bytes.empty() || l_Result.bytes.size() * 10 > p_Bytes.size() * 9)
	{
		p_Error = "This picture is already small.";
		return std::nullopt;
	}
	return l_Result;
}
} // namespace wb::image
