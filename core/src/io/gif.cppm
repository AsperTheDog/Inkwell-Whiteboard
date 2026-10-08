// GIF decoder. Every frame is composed onto the logical screen the way browsers do (frame rectangles, transparency,
// local palettes, interlacing and the "leave / restore to background / restore to previous" disposal methods), so
// each frame handed out is a complete picture.
module;
#include <cstdint>
#include <functional>
#include <span>

export module wb.io.gif;

export namespace wb
{
// Receives one composed frame (straight alpha RGBA8, width * height * 4 bytes) and its delay in milliseconds as
// written in the file (0 when the file gives none). Return false to stop decoding.
using GifFrameSink = std::function<bool(std::span<const uint8_t> p_Rgba, uint32_t p_DelayMs)>;

struct GifHeader
{
	uint32_t width = 0;
	uint32_t height = 0;
};

// Reads the signature and the logical screen size. False when p_Bytes is not a GIF.
[[nodiscard]] bool readGifHeader(std::span<const uint8_t> p_Bytes, GifHeader& p_Header);

// Decodes frames in order. Frames decoded before damaged data are still delivered; the result is false only when no
// frame could be decoded at all.
[[nodiscard]] bool decodeGif(std::span<const uint8_t> p_Bytes, const GifFrameSink& p_Sink);
} // namespace wb
