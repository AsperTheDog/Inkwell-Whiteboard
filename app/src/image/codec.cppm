// Image file decoding (PNG, JPEG, BMP, GIF... through stb_image) and recompression. Pure CPU, no Vulkan, safe to call
// from worker threads.
module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module wb.image.codec;

export namespace wb::image
{
struct ImageInfo
{
	uint32_t width = 0;
	uint32_t height = 0;
	bool gif = false; // may be animated
};

// Reads the header only. Nullopt for anything that is not a supported picture.
[[nodiscard]] std::optional<ImageInfo> probe(std::span<const uint8_t> p_Bytes);

// All frames of a picture as premultiplied RGBA8 (what the GPU blends), ready to upload
struct Decoded
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<std::vector<uint8_t>> frames; // each width * height * 4 bytes
	std::vector<uint32_t> delaysMs;           // per frame (animations)
	bool truncated = false;                   // frames were dropped to stay within the memory budget
};

struct DecodeLimits
{
	uint32_t maxDimension = 16384;            // larger pictures are scaled down to fit (the GPU's limit)
	size_t maxBytes = 512ull * 1024 * 1024;   // budget for all frames together
};

[[nodiscard]] std::optional<Decoded> decode(std::span<const uint8_t> p_Bytes, const DecodeLimits& p_Limits, std::string& p_Error);

// Result of recompressing a still picture
struct Recompressed
{
	std::vector<uint8_t> bytes;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string extension; // "jpg" or "png"
};

// Re-encodes a still picture to take less space: opaque pictures become JPEG (quality p_JpegQuality), pictures with
// transparency stay PNG; pictures larger than p_MaxDimension are scaled down. Returns nullopt when that would not
// make the file meaningfully smaller, or for animated pictures.
[[nodiscard]] std::optional<Recompressed> recompress(std::span<const uint8_t> p_Bytes, uint32_t p_MaxDimension, int p_JpegQuality, std::string& p_Error);
} // namespace wb::image
