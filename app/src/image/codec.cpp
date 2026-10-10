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
#include "video/ffmpeg.hpp"

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

bool isWebp(const std::span<const uint8_t> p_Bytes)
{
	return p_Bytes.size() > 30 && std::memcmp(p_Bytes.data(), "RIFF", 4) == 0 && std::memcmp(p_Bytes.data() + 8, "WEBP", 4) == 0;
}

// Size (and whether it is animated) from the first chunk, without decoding anything
bool readWebpHeader(const std::span<const uint8_t> p_Bytes, uint32_t& p_Width, uint32_t& p_Height, bool& p_Animated)
{
	const uint8_t* l_Data = p_Bytes.data();
	p_Animated = false;
	if (std::memcmp(l_Data + 12, "VP8X", 4) == 0)
	{
		p_Animated = (l_Data[20] & 0x02) != 0;
		p_Width = 1u + (l_Data[24] | (l_Data[25] << 8) | (l_Data[26] << 16));
		p_Height = 1u + (l_Data[27] | (l_Data[28] << 8) | (l_Data[29] << 16));
		return true;
	}
	if (std::memcmp(l_Data + 12, "VP8 ", 4) == 0)
	{
		if (l_Data[23] != 0x9d || l_Data[24] != 0x01 || l_Data[25] != 0x2a)
			return false;
		p_Width = (l_Data[26] | (l_Data[27] << 8)) & 0x3fff;
		p_Height = (l_Data[28] | (l_Data[29] << 8)) & 0x3fff;
		return p_Width > 0 && p_Height > 0;
	}
	if (std::memcmp(l_Data + 12, "VP8L", 4) == 0)
	{
		if (l_Data[20] != 0x2f)
			return false;
		const uint32_t l_Bits = l_Data[21] | (l_Data[22] << 8) | (l_Data[23] << 16) | (static_cast<uint32_t>(l_Data[24]) << 24);
		p_Width = (l_Bits & 0x3fff) + 1;
		p_Height = ((l_Bits >> 14) & 0x3fff) + 1;
		return true;
	}
	return false;
}

struct CodecContextDeleter
{
	void operator()(AVCodecContext* p_Context) const { avcodec_free_context(&p_Context); }
};
struct FrameDeleter
{
	void operator()(AVFrame* p_Frame) const { av_frame_free(&p_Frame); }
};
struct PacketDeleter
{
	void operator()(AVPacket* p_Packet) const { av_packet_free(&p_Packet); }
};
struct ScalerDeleter
{
	void operator()(SwsContext* p_Scaler) const { sws_freeContext(p_Scaler); }
};

// Decodes a still WebP (or what FFmpeg's decoder can make of a file) with FFmpeg's decoder as straight-alpha RGBA8. p_OnFrame gets the pixels, their size
// and how long the frame shows in milliseconds (0 when unknown); it returns false to stop.
template <class F>
bool decodeWebpStill(const std::span<const uint8_t> p_Bytes, F&& p_OnFrame)
{
	const AVCodec* l_Codec = avcodec_find_decoder(AV_CODEC_ID_WEBP);
	if (l_Codec == nullptr)
		return false;
	const std::unique_ptr<AVCodecContext, CodecContextDeleter> l_Context(avcodec_alloc_context3(l_Codec));
	if (!l_Context)
		return false;
	l_Context->pkt_timebase = AVRational{ 1, 1000 };
	if (avcodec_open2(l_Context.get(), l_Codec, nullptr) < 0)
		return false;
	const std::unique_ptr<AVPacket, PacketDeleter> l_Packet(av_packet_alloc());
	const std::unique_ptr<AVFrame, FrameDeleter> l_Frame(av_frame_alloc());
	if (!l_Packet || !l_Frame || av_new_packet(l_Packet.get(), static_cast<int>(p_Bytes.size())) < 0)
		return false;
	std::memcpy(l_Packet->data, p_Bytes.data(), p_Bytes.size());
	if (avcodec_send_packet(l_Context.get(), l_Packet.get()) < 0)
		return false;
	avcodec_send_packet(l_Context.get(), nullptr); // nothing follows: lets the decoder give out every frame

	std::unique_ptr<SwsContext, ScalerDeleter> l_Scaler;
	std::vector<uint8_t> l_Rgba;
	bool l_Any = false;
	while (avcodec_receive_frame(l_Context.get(), l_Frame.get()) == 0)
	{
		const int l_Width = l_Frame->width, l_Height = l_Frame->height;
		if (l_Width <= 0 || l_Height <= 0 || static_cast<uint64_t>(l_Width) * static_cast<uint64_t>(l_Height) > MAX_PIXELS)
			return false;
		l_Scaler.reset(sws_getCachedContext(l_Scaler.release(), l_Width, l_Height, static_cast<AVPixelFormat>(l_Frame->format), l_Width, l_Height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr));
		if (!l_Scaler)
			return false;
		const size_t l_Bytes = static_cast<size_t>(l_Width) * l_Height * 4;
		l_Rgba.resize(l_Bytes + 64); // swscale may write a little past the last row
		uint8_t* l_Planes[4] = { l_Rgba.data(), nullptr, nullptr, nullptr };
		const int l_Strides[4] = { l_Width * 4, 0, 0, 0 };
		sws_scale(l_Scaler.get(), l_Frame->data, l_Frame->linesize, 0, l_Height, l_Planes, l_Strides);
		l_Rgba.resize(l_Bytes);
		const int64_t l_Delay = l_Frame->duration > 0 ? l_Frame->duration : 0; // the time base is a millisecond
		l_Any = true;
		const bool l_Continue = p_OnFrame(std::span<const uint8_t>(l_Rgba), static_cast<uint32_t>(l_Width), static_cast<uint32_t>(l_Height), static_cast<uint32_t>(l_Delay));
		av_frame_unref(l_Frame.get());
		if (!l_Continue)
			break;
	}
	return l_Any;
}

uint32_t le24(const uint8_t* p_Data)
{
	return p_Data[0] | (p_Data[1] << 8) | (p_Data[2] << 16);
}

void putLe32(std::vector<uint8_t>& p_Out, const uint32_t p_Value)
{
	for (int i = 0; i < 4; ++i)
		p_Out.push_back(static_cast<uint8_t>(p_Value >> (8 * i)));
}

void putLe24(std::vector<uint8_t>& p_Out, const uint32_t p_Value)
{
	for (int i = 0; i < 3; ++i)
		p_Out.push_back(static_cast<uint8_t>(p_Value >> (8 * i)));
}

// FFmpeg's decoder knows nothing of animations, so each frame of one is cut out as a still WebP of its own, decoded,
// and laid on a canvas following the blend and dispose rules of the file. Same contract as decodeWebpStill.
template <class F>
bool decodeWebpAnimated(const std::span<const uint8_t> p_Bytes, const uint32_t p_CanvasWidth, const uint32_t p_CanvasHeight, F&& p_OnFrame)
{
	std::vector<uint8_t> l_Canvas(static_cast<size_t>(p_CanvasWidth) * p_CanvasHeight * 4, 0);
	bool l_Any = false;
	bool l_PreviousDispose = false;
	uint32_t l_PreviousX = 0, l_PreviousY = 0, l_PreviousWidth = 0, l_PreviousHeight = 0;

	size_t l_Offset = 12;
	while (l_Offset + 8 <= p_Bytes.size())
	{
		const uint8_t* l_Chunk = p_Bytes.data() + l_Offset;
		const size_t l_Size = static_cast<size_t>(l_Chunk[4]) | (static_cast<size_t>(l_Chunk[5]) << 8) | (static_cast<size_t>(l_Chunk[6]) << 16) | (static_cast<size_t>(l_Chunk[7]) << 24);
		const size_t l_Next = l_Offset + 8 + l_Size + (l_Size & 1);
		if (l_Offset + 8 + l_Size > p_Bytes.size())
			break;
		if (std::memcmp(l_Chunk, "ANMF", 4) != 0 || l_Size < 16)
		{
			l_Offset = l_Next;
			continue;
		}

		const uint8_t* l_Header = l_Chunk + 8;
		const uint32_t l_X = le24(l_Header) * 2, l_Y = le24(l_Header + 3) * 2;
		const uint32_t l_Width = le24(l_Header + 6) + 1, l_Height = le24(l_Header + 9) + 1;
		const uint32_t l_Duration = le24(l_Header + 12);
		const bool l_NoBlend = (l_Header[15] & 0x02) != 0;
		const bool l_Dispose = (l_Header[15] & 0x01) != 0;
		if (static_cast<uint64_t>(l_X) + l_Width > p_CanvasWidth || static_cast<uint64_t>(l_Y) + l_Height > p_CanvasHeight)
			return l_Any;

		// The frame's own chunks (ALPH, VP8, VP8L) become a file of their own; ALPH only counts inside a VP8X
		const std::span<const uint8_t> l_Data(l_Header + 16, l_Size - 16);
		const bool l_HasAlpha = l_Data.size() >= 4 && std::memcmp(l_Data.data(), "ALPH", 4) == 0;
		std::vector<uint8_t> l_File = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X' };
		putLe32(l_File, 10);
		l_File.push_back(l_HasAlpha ? 0x10 : 0);
		l_File.insert(l_File.end(), 3, 0);
		putLe24(l_File, l_Width - 1);
		putLe24(l_File, l_Height - 1);
		l_File.insert(l_File.end(), l_Data.begin(), l_Data.end());
		const uint32_t l_RiffSize = static_cast<uint32_t>(l_File.size() - 8);
		for (int i = 0; i < 4; ++i)
			l_File[4 + i] = static_cast<uint8_t>(l_RiffSize >> (8 * i));

		bool l_Drew = false;
		decodeWebpStill(l_File, [&](const std::span<const uint8_t> p_Rgba, const uint32_t p_Width, const uint32_t p_Height, uint32_t)
		{
			if (p_Width != l_Width || p_Height != l_Height)
				return false;
			if (l_PreviousDispose) // the previous frame asked for its area to be cleared before this one
			{
				for (uint32_t y = 0; y < l_PreviousHeight; ++y)
					std::memset(l_Canvas.data() + (static_cast<size_t>(l_PreviousY + y) * p_CanvasWidth + l_PreviousX) * 4, 0, static_cast<size_t>(l_PreviousWidth) * 4);
			}
			for (uint32_t y = 0; y < l_Height; ++y)
			{
				const uint8_t* l_Source = p_Rgba.data() + static_cast<size_t>(y) * l_Width * 4;
				uint8_t* l_Target = l_Canvas.data() + (static_cast<size_t>(l_Y + y) * p_CanvasWidth + l_X) * 4;
				for (uint32_t x = 0; x < l_Width; ++x, l_Source += 4, l_Target += 4)
				{
					const uint32_t l_SourceAlpha = l_Source[3];
					if (l_NoBlend || l_SourceAlpha == 255 || l_Target[3] == 0)
					{
						std::memcpy(l_Target, l_Source, 4);
						continue;
					}
					if (l_SourceAlpha == 0)
						continue;
					const uint32_t l_TargetAlpha = l_Target[3] * (255 - l_SourceAlpha) / 255;
					const uint32_t l_OutAlpha = l_SourceAlpha + l_TargetAlpha;
					for (int c = 0; c < 3; ++c)
						l_Target[c] = static_cast<uint8_t>((l_Source[c] * l_SourceAlpha + l_Target[c] * l_TargetAlpha) / l_OutAlpha);
					l_Target[3] = static_cast<uint8_t>(l_OutAlpha);
				}
			}
			l_Drew = true;
			return false;
		});
		if (!l_Drew)
			return l_Any;
		l_PreviousDispose = l_Dispose;
		l_PreviousX = l_X;
		l_PreviousY = l_Y;
		l_PreviousWidth = l_Width;
		l_PreviousHeight = l_Height;
		l_Any = true;
		if (!p_OnFrame(std::span<const uint8_t>(l_Canvas), p_CanvasWidth, p_CanvasHeight, l_Duration))
			break;
		l_Offset = l_Next;
	}
	return l_Any;
}

template <class F>
bool decodeWebp(const std::span<const uint8_t> p_Bytes, F&& p_OnFrame)
{
	uint32_t l_Width = 0, l_Height = 0;
	bool l_Animated = false;
	if (readWebpHeader(p_Bytes, l_Width, l_Height, l_Animated) && l_Animated)
		return decodeWebpAnimated(p_Bytes, l_Width, l_Height, p_OnFrame);
	return decodeWebpStill(p_Bytes, p_OnFrame);
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
	if (isWebp(p_Bytes))
	{
		uint32_t l_WebpWidth = 0, l_WebpHeight = 0;
		bool l_Animated = false;
		if (p_Bytes.size() > static_cast<size_t>(INT32_MAX) || !readWebpHeader(p_Bytes, l_WebpWidth, l_WebpHeight, l_Animated) || static_cast<uint64_t>(l_WebpWidth) * l_WebpHeight > MAX_PIXELS)
			return std::nullopt;
		return ImageInfo{ .width = l_WebpWidth, .height = l_WebpHeight, .gif = l_Animated, .webp = true };
	}
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
		p_Error = "This is not a picture Inkwell can read.";
		return std::nullopt;
	}
	const int l_Size = static_cast<int>(p_Bytes.size());

	if (l_Info->webp)
	{
		Decoded l_Webp;
		uint32_t l_SourceWidth = 0, l_SourceHeight = 0;
		size_t l_MaxFrames = 1;
		const bool l_Decoded = decodeWebp(p_Bytes, [&](const std::span<const uint8_t> p_Rgba, const uint32_t p_Width, const uint32_t p_Height, const uint32_t p_DelayMs)
		{
			if (l_Webp.frames.empty())
			{
				l_SourceWidth = p_Width;
				l_SourceHeight = p_Height;
				fitWithin(p_Width, p_Height, p_Limits.maxDimension, l_Webp.width, l_Webp.height);
				l_MaxFrames = std::max<size_t>(1, p_Limits.maxBytes / (static_cast<size_t>(l_Webp.width) * l_Webp.height * 4));
			}
			else if (p_Width != l_SourceWidth || p_Height != l_SourceHeight)
			{
				return false;
			}
			if (l_Webp.frames.size() >= l_MaxFrames)
			{
				l_Webp.truncated = true;
				return false;
			}
			std::vector<uint8_t> l_Frame = (l_Webp.width != p_Width || l_Webp.height != p_Height) ? resizeRgba(p_Rgba.data(), p_Width, p_Height, l_Webp.width, l_Webp.height) : std::vector<uint8_t>(p_Rgba.begin(), p_Rgba.end());
			premultiply(l_Frame);
			l_Webp.frames.push_back(std::move(l_Frame));
			l_Webp.delaysMs.push_back(p_DelayMs <= 10 ? DEFAULT_DELAY_MS : p_DelayMs);
			return true;
		});
		if (!l_Decoded || l_Webp.frames.empty())
		{
			p_Error = "The picture could not be decoded (WebP).";
			return std::nullopt;
		}
		return l_Webp;
	}

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
		p_Error = l_Info ? "Animated pictures cannot be recompressed." : "This is not a picture Inkwell can read.";
		return std::nullopt;
	}
	int l_Width = 0, l_Height = 0, l_Components = 0;
	StbiPixels l_Pixels;
	std::vector<uint8_t> l_WebpPixels; // a WebP is decoded by FFmpeg into this instead
	if (l_Info->webp)
	{
		decodeWebp(p_Bytes, [&](const std::span<const uint8_t> p_Rgba, const uint32_t p_Width, const uint32_t p_Height, uint32_t)
		{
			l_WebpPixels.assign(p_Rgba.begin(), p_Rgba.end());
			l_Width = static_cast<int>(p_Width);
			l_Height = static_cast<int>(p_Height);
			return false;
		});
	}
	else
	{
		l_Pixels.reset(stbi_load_from_memory(p_Bytes.data(), static_cast<int>(p_Bytes.size()), &l_Width, &l_Height, &l_Components, 4));
	}
	const uint8_t* l_Decoded = l_Info->webp ? l_WebpPixels.data() : l_Pixels.get();
	if (l_Decoded == nullptr || l_Width <= 0 || l_Height <= 0)
	{
		p_Error = "The picture could not be decoded.";
		return std::nullopt;
	}

	bool l_HasAlpha = false;
	const size_t l_PixelCount = static_cast<size_t>(l_Width) * static_cast<size_t>(l_Height);
	for (size_t i = 0; i < l_PixelCount && !l_HasAlpha; ++i)
		l_HasAlpha = l_Decoded[i * 4 + 3] != 255;

	uint32_t l_OutWidth = 0, l_OutHeight = 0;
	fitWithin(static_cast<uint32_t>(l_Width), static_cast<uint32_t>(l_Height), p_MaxDimension, l_OutWidth, l_OutHeight);
	std::vector<uint8_t> l_Resized;
	const uint8_t* l_Source = l_Decoded;
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
